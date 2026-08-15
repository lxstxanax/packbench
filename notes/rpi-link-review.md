# Raspberry Pi ↔ STM32 command link — review and hardening patch

Scope: the USART1 (PB6/PB7, 115200 8N1) command path in `car_fw/`, from the
GPIO pin to `UART_ProcessPacket()`. Everything else in the firmware is out of
scope except where it can stop this link working.

Snapshot reviewed: `car_fw/Core/Src/main.c` at 1238 lines, `Core/Src/usart.c`,
`Core/Src/stm32g4xx_it.c`, `Core/Src/motor_dc.c`, `Core/Src/tim.c`,
`Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c`, `motor_uart_pwm_nucleo.ioc`.
Git HEAD `e297dce`, working tree modified.

**`main.c` was being edited concurrently while this was written.** Nothing here
was applied. Every patch below is given as a **complete replacement function**
or as an insertion anchored on quoted text, not as a line-numbered diff, so it
survives whatever else landed in the file. Re-read each anchor before applying.

---

## 0. The three things that most deserve fixing

**1 — XOR-8 accepts 100 % of the misaligned frames this protocol actually
produces.** Not "1 in 256". One hundred per cent. Because XOR is
order-independent, a parser that has locked onto a payload byte of value `0xA5`
(motor percent −91) sees the two `0xA5` bytes cancel, and the check collapses to
"is this frame's command byte equal to the next frame's command byte" — which is
always true in a steady CONTROL stream. Verified by enumeration: 10201 of 10201
such captures accepted by XOR-8, 1 of 101 by CRC-8. When the steering value in
that frame happens to be 16, 17 or 19, the mis-decoded command byte becomes
CONTROL-with-garbage, ESTOP or STOP, and the car brakes hard and snaps the
steering to centre. §3.2 and §5 carry the derivation. **Fix: CRC-8/AUTOSAR,
§3.3 — a coordinated change, the RPi must be updated in the same breath.**

**2 — After a lost byte, resynchronisation is not bounded; it depends on an
unverified claim about the RPi's send rate.** The only two recovery paths today
are (a) the 10 ms partial-frame timeout, which can only fire if there is an idle
gap between frames, and (b) luck — the parser happening not to find an `0xA5`
in the payload. A comment now in `main.c` states the RPi *streams* CONTROL
back-to-back at "hundreds of packets per second". If that is true there is no
idle gap, path (a) never fires, an IDLE-line detector would never fire either,
and resynchronisation rests entirely on luck. **Fix: sliding-window resync on
checksum failure, §4.4. It is deterministic in both regimes and needs no
assumption about the sender's timing at all** — about 15 lines in the ISR.

**3 — A single corrupted byte currently disarms the drive.** The partial-frame
timeout calls `Drive_FailSafe()`, which zeroes the PWM with `MotorDC_Stop()`
*bypassing the ramp*, shorts the motor through the bridge (that is a brake, not
a coast), drops R_EN/L_EN, and then needs 512 ms of ramp just to break away
again and 1.1 s to return to speed. One noise glitch on a jumper wire on a
vibrating vehicle produces a visible lurch-and-stall. The CONTROL watchdog at
300 ms already covers a genuinely dead link. **Fix: §4.5 — drop one frame, count
it, do not touch the bridge.**

Nothing else on the list is close to these three in impact.

---

## 1. Premise check

Every stated premise was re-verified against the source. All hold, with three
corrections and one that turns out to be much weaker than it sounds.

| # | Premise | Verdict |
|---|---|---|
| 1 | 5-byte fixed frame, start `0xA5`, assembled byte-by-byte in `HAL_UART_RxCpltCallback` | **Confirmed.** `main.c` `HAL_UART_RxCpltCallback()`. Index bounds are correct: max write index is 4, no overflow. |
| 2 | Deliberately does not resync on `0xA5` mid-frame, because byte 2 can legitimately be `0xA5` | **Confirmed and the reasoning is right.** `0xA5` as `int8_t` is −91, an ordinary joystick value. Mid-frame resync would be wrong. §4.4 adds recovery *after* a frame fails, which does not violate this. |
| 3 | `HAL_UART_ErrorCallback` resets the index, aborts and restarts reception, so a latched ORE does not kill the link permanently | **Confirmed, and it is load-bearing in a way that is not obvious.** See §2.1 — the HAL disarms the receiver *after* the RxCplt callback has already re-armed it. Without the re-arm inside `ErrorCallback`, one overrun would kill the link for good. Do not "simplify" that function. |
| 4 | Integrity is XOR-8, named `UART_CalcCRC`, not a CRC | **Confirmed.** `return (uint8_t)(start ^ cmd ^ motor ^ steer);` |
| 5 | Command values range-checked before use | **Confirmed** for CONTROL (`steer > 100`, `motor > 100`, `motor < −100`). Note the out-of-range branch does not merely reject the packet — it calls `Drive_FailSafe()` + `Steering_Disable()`. That makes a corrupt value a *hard stop*, which is why §3 matters. |
| 6 | A drive watchdog stops the motor if CONTROL packets stop | **Confirmed**, now 300 ms (was 500 ms). |
| 7 | A speed ramp limits how fast a command takes physical effect | **Confirmed** — but it is bypassed on every fail-safe path. `MotorDC_Stop()` sets `motor_current = 0` and writes the compares immediately. The ramp protects acceleration only. |
| 8 | USART1 on PB6/PB7 at 115200 | **Confirmed.** PCLK2 = 170 MHz, `BRR` = 1476, actual 115176 baud, **−0.021 %** — the divisor is not the problem (§6.7). |

Two premises that were *not* stated but that the code assumes:

- **The RPi's packet rate is unknown.** `main.c` now asserts in a comment that
  the RPi "streams" at hundreds of packets per second and that the older
  "20..50 Hz" figure "was never measured and is wrong". There is no RPi source
  in this repository (`grep` over `*.py` finds only the BMS dashboard and
  `check_board.py`) and no measurement here either. **UNCONFIRMED — and it is the
  single most load-bearing unknown in this review.** It decides whether the
  partial-frame timeout ever fires, whether an IDLE detector is useful, and how
  long a mis-decoded packet is in force. §4 and §5 give the arithmetic for both.
- **Nothing else uses USART1.** Confirmed. The BMS console is LPUART1 via BSP
  COM1 on PA2/PA3 and is fully non-blocking (`fw/bms_io.c` uses TX and RX rings
  drained from `LPUART1_IRQHandler`, NVIC priority 6). The console cannot stall
  the main loop. That was worth checking and it is clean.

---

## 2. Receive path — line-by-line findings

Read: `usart.c` (`MX_USART1_UART_Init`, `HAL_UART_MspInit`), `USART1_IRQHandler`
in `stm32g4xx_it.c`, `HAL_UART_IRQHandler` / `UART_RxISR_8BIT` /
`UART_EndRxTransfer` / `UART_Start_Receive_IT` / `HAL_UART_AbortReceive` in the
HAL, and `HAL_UART_RxCpltCallback`, `HAL_UART_ErrorCallback`,
`UART_ProcessPacket`, `UART_SendPacket`, `UART_CalcCRC` in `main.c`.

Findings are ranked by what they can actually do to the demonstration.

### L1 — CRITICAL — XOR-8 is order-independent and the frame has two `0xA5` bytes in it

Full treatment in §3. The headline: this is not a probabilistic weakness, it is
a deterministic one for the specific misalignment this protocol produces.

### L2 — CRITICAL — resynchronisation after a lost byte is unbounded

Full treatment in §4.

### L3 — HIGH — one bad byte disarms the drive with a hard brake

```c
if (partial_frame_dropped && drive_enabled)
{
    Drive_FailSafe();
}
```

`Drive_FailSafe()` → `MotorDC_Stop()` → `motor_current = 0` and
`MotorDC_ApplyRaw(0)`, which sets **both** compares to zero. Read
`motor_dc.c`'s own comment: *"СТОП. Оба канала в нуле — оба нижних ключа
открыты, мотор замкнут накоротко через мост. Это торможение, а не выбег."*
Both low sides on, motor shorted through the bridge — a brake. Applied within
one 50 µs PWM period from whatever speed the car was doing. Then `HAL_Delay(3)`,
then the enables drop.

Recovery costs 512 ms of ramp to reach breakaway and 1.107 s to return to the
40 %-limit top speed (§5.1). A single glitch on a Dupont jumper on a moving
vehicle therefore produces a lurch, a stall, and a slow crawl back up.

This is a safety mechanism aimed at the wrong event. A truncated frame means one
command out of hundreds was lost. A *dead link* is what should drop the bridge,
and the 300 ms CONTROL watchdog already does exactly that. Patch in §4.5.

### L4 — HIGH — PB7 (RX) has no pull-up, so the line floats whenever the RPi is not driving it

`usart.c:104`: `GPIO_InitStruct.Pull = GPIO_NOPULL;` and the `.ioc` has no
`PB7.GPIOParameters` entry, so a CubeMX regeneration reproduces it.

A UART RX line at rest must be **high**. While the Pi is booting, held in reset,
powered down, or the connector is out, PB7 is a floating high-impedance input on
a wire running next to a 20 kHz BTS7960 bridge switching motor current. It will
pick up noise, and every noise excursion below the input threshold is a start
bit. What follows: framing errors and overruns at whatever rate the noise
provides, `HAL_UART_ErrorCallback` on each, and — with the current code — a
`Drive_FailSafe()` every time the noise leaves a partial frame behind (L3).

This is the cheapest fix in the whole document and the demo scenario ("plug the
Pi in after the car is powered", "the Pi reboots between runs") is exactly when
it bites. Patch in §7.3.

### L5 — MEDIUM — the return value of `HAL_UART_Receive_IT()` is discarded in both callbacks

```c
HAL_UART_Receive_IT(&huart1, &rx_byte, 1);   // RxCpltCallback
HAL_UART_Receive_IT(&huart1, &rx_byte, 1);   // ErrorCallback
```

`HAL_UART_Receive_IT` returns `HAL_BUSY` if `RxState != HAL_UART_STATE_READY`,
and if it ever does, **nothing in this firmware ever re-arms the receiver**. The
link goes silent permanently, the CONTROL watchdog stops the car after 300 ms,
and the only recovery is a board reset. On a stage, that is the demo over.

I traced every path and could not construct a case where it actually returns
`HAL_BUSY` in this build:

- `UART_RxISR_8BIT` sets `RxState = READY` and `RxISR = NULL` *before* calling
  `HAL_UART_RxCpltCallback`, so the re-arm in the RxCplt path always succeeds.
- On ORE/RTO ("blocking" errors) the HAL calls `UART_EndRxTransfer`, which also
  sets `RxState = READY`, before `HAL_UART_ErrorCallback`.
- On FE/NE/PE ("non-blocking") `RxState` is still `BUSY_RX`, but the
  `HAL_UART_AbortReceive()` in the error callback forces it to `READY` first.
- This HAL version does **not** use `__HAL_LOCK` in `HAL_UART_Transmit`,
  `HAL_UART_Receive_IT` or `UART_Start_Receive_IT` (checked in
  `stm32g4xx_hal_uart.c`), and `gState`/`RxState` are independent — so the
  blocking PING transmit cannot lock out the RX re-arm. That was the failure I
  most expected to find and it is not there.

So this is currently latent, not live. But it is a single point of total failure
guarded only by an argument about HAL internals, and the insurance is six lines
in the main loop that check the receiver is actually armed. §7.2. For a link
this critical, take the insurance.

**Related and worth stating explicitly:** the ordering inside
`HAL_UART_IRQHandler` on an overrun is
`RxISR()` → `UART_EndRxTransfer()` → `HAL_UART_ErrorCallback()`. The RxCplt
callback re-arms reception, and then `UART_EndRxTransfer` immediately tears that
re-arm down again. **The link only survives an overrun because
`HAL_UART_ErrorCallback` re-arms a second time.** Anyone who later "cleans up"
that error callback to just reset the parser index will kill the link on the
first overrun. Add a comment saying so (included in the §7.2 patch).

### L6 — MEDIUM — the one-packet mailbox drops non-CONTROL commands; ESTOP is handled, STOP is not

The concurrent edit already fixed the important half of this: ESTOP is now
detected and latched inside the ISR with `Drive_EmergencyOff()` called at
register level, precisely because "latest packet wins" would otherwise let a
CONTROL frame arriving 4 ms later overwrite the stop. That reasoning is correct
and the implementation is right.

`UART_CMD_STOP` (0x13) still travels through the one-packet mailbox and can
still be overwritten before the main loop reads it. Lower severity — STOP is
graceful and the RPi can achieve the same thing with `CONTROL motor=0` — but if
the RPi's UI has a "stop" button wired to 0x13, it can be silently dropped.
Either latch it the same way as ESTOP, or tell the RPi to use `CONTROL motor=0`
and treat 0x13 as decorative. Recommendation: the latter, it needs no firmware
change.

### L7 — LOW — `volatile` and linkage on the shared parser state

```c
uint8_t rx_byte;                              // not volatile, not static
uint8_t uart_packet[UART_PACKET_SIZE];        // not volatile, not static
volatile uint8_t uart_packet_index = 0;       // not static
static volatile uint8_t uart_packet_ready = 0;
static uint8_t uart_packet_rx[UART_PACKET_SIZE];   // not volatile
```

Checked each one for a real miscompilation risk:

- `rx_byte` — written by the HAL through `*huart->pRxBuffPtr` inside a function
  call, read in the callback after that call returns. The compiler cannot cache
  it across the call boundary. Safe in practice; `volatile` costs nothing and
  removes the argument.
- `uart_packet[]` — touched only from the ISR. No cross-context access. Fine.
- `uart_packet_rx[]` — written by the ISR, read by the main loop, but the main
  loop reads it only inside `__disable_irq()`/`__enable_irq()`, which are compiler
  barriers as well as interrupt barriers. Correct as written.
- `uart_packet_index`, `uart_packet_ready`, `last_rx_byte_tick`,
  `last_control_packet_tick` — all `volatile`, all naturally aligned, all
  single-word. Atomic on Cortex-M4. Correct.

**No missing-`volatile` bug found.** The earlier review's assessment ("gets
`volatile` right on every shared variable") holds. Only cosmetic: `rx_byte`,
`uart_packet` and `uart_packet_index` have external linkage for no reason and
should be `static`.

### L8 — LOW — the tick-sampling race in the partial-frame check is already fixed

Worth recording because it was a real bug and someone might undo it. The old
form sampled `now = HAL_GetTick()` *outside* the critical section; the USART ISR
could then fire and push `last_rx_byte_tick` past `now`, and the unsigned
subtraction wrapped to ~4.29e9, discarding a frame that was mid-flight. The
current code reads the tick **inside** `__disable_irq()` and the comment explains
why. Correct. Keep it.

The CONTROL watchdog does not have this race: `last_control_packet_tick` is
written from `UART_ProcessPacket()` in the main loop, not from the ISR. Fine as
it is.

### L9 — LOW — `UART_SendPacket()` blocks the main loop, and PING is unauthenticated work

`HAL_UART_Transmit(&huart1, packet, 5, 10)` is polled: 434 µs of main loop held
per PING reply, up to 10 ms if TXE never sets. It is called from
`UART_ProcessPacket()` on every PING that survives the checksum. If the RPi ever
sends PING at the CONTROL rate, the main loop spends a measurable fraction of
its time in it, delaying packet processing and the pack interlock.

Not urgent. If PING is unused, ask the RPi team and delete the case (§8). If it
is used, rate-limit it to, say, 10 Hz.

### L10 — INFORMATIONAL — NVIC priority of the command link is below the 2 ms timer

`TIM4_IRQn` priority 0, `USART1_IRQn` priority 1 (`tim.c:241`, `usart.c:110`,
and the `.ioc`). TIM4 preempts the command link. The RX FIFO is explicitly
disabled (`HAL_UARTEx_DisableFifoMode(&huart1)`), so USART1 tolerates only **one
character time, 87 µs**, of ISR latency before an overrun.

`Steering_Update()` + `MotorDC_Update()` are short — branches, a compare-register
write, and a short `PRIMASK`-guarded pair write — a few microseconds even at
`-O0`. So this is not currently causing overruns and I am not recommending a
change two days out. Recorded so that if anyone later adds work to the TIM4
handler, they know the budget is 87 µs.

If you want the margin anyway, the safe version is to swap the priorities
(USART1 → 0, TIM4 → 1). I checked the shared state: the USART1 ISR touches only
the parser, `estop_latched`, and `Drive_EmergencyOff()` (raw register writes),
and `MotorDC_ApplyRaw()` already guards its compare-register pair with
`PRIMASK`, so USART1 cannot land inside it. Enabling the RX FIFO would buy 8×
more margin but changes which HAL ISR path runs — more risk than it is worth
right now.

---

## 3. The integrity check

### 3.1 What XOR-8 misses on this exact frame

The check is `p0^p1^p2^p3 == p4`, i.e. the XOR of all five bytes must be zero.
It is a per-bit-column parity across the five bytes. I enumerated every error
pattern exhaustively over the 40-bit frame `A5 10 2B 32 | checksum`:

| Error class | XOR-8 undetected | CRC-8/0x2F undetected |
|---|---|---|
| all 1-bit errors (40) | 0 | 0 |
| all 2-bit errors (780) | **80 — 10.26 %** | **0** |
| all 3-bit errors (9880) | 0 | 0 |
| all 4-bit errors (91390) | 2840 — 3.11 % | 693 — 0.76 % |
| all 5-bit errors (658008) | 0 | 0 |
| bursts ≤ 8 bits, polynomial order | 0 | 0 |
| bursts ≤ 8 bits, UART wire order (LSB-first) | 0 | 8 |
| uniformly random 5-byte window | 1/256 = 0.39 % | 1/256 = 0.39 % |

Read that table honestly: for *random* noise XOR-8 is not much worse than a CRC,
and for short physical bursts it is very slightly better. If random bit noise
were the threat model, this would be a marginal upgrade and not worth doing two
days before a demonstration.

It is not the threat model. Two structural weaknesses are the reason to change:

**(a) The 10.26 % of double-bit errors are exactly the correlated ones.** The
undetected pairs are precisely "the same bit position flipped in two different
bytes" — C(5,2) byte-pairs × 8 columns = 80. That is what a periodic disturbance
synchronised to the bit clock produces, and what a receiver sampling slightly
off-centre produces: the same bit index goes wrong in successive bytes. Bridge
switching at 20 kHz against a 115200 bit clock is a periodic disturbance.

**(b) XOR is commutative, so it does not check byte *order* or byte *position*
at all.** This is the fatal one and it is not hypothetical here. §3.2.

### 3.2 The deterministic failure: two `0xA5` bytes cancel

Take a normal frame N during driving:

```
p = [ A5 | C | M | S | X ]        X = A5 ^ C ^ M ^ S
```

Suppose `M == 0xA5` — motor percent −91, a perfectly ordinary joystick position.
Now suppose the parser is misaligned by +2 bytes (§4.2 shows how it gets there
after a single lost byte) and locks onto that `M` as a start byte. It captures:

```
capture = [ M=A5 | S | X | A5(start of frame N+1) | C'(cmd of frame N+1) ]
```

Its computed checksum is

```
A5 ^ S ^ X ^ A5  =  S ^ X  =  S ^ (A5 ^ C ^ A5 ^ S)  =  C
```

and the byte it compares against is `C'`, the *next* frame's command byte. So the
check reduces to **`cmd(N) == cmd(N+1)`** — which is true on every frame of a
steady CONTROL stream. The two `0xA5` bytes cancelled and the frame's identity
disappeared.

Enumerated over all 101×101 (steer, next-steer) combinations:

```
XOR8 accepts 10201 of 10201 (100.0%) offset+2 captures
CRC8 accepts 1 of 101            (the expected ~1/256 residual)
```

What the accepted capture decodes to: `cmd = S`, the steering value of frame N;
`steer = 0xA5 = 165`, out of range. So:

| steer in frame N | mis-decoded `cmd` | what the firmware does |
|---|---|---|
| 16 | `0x10` CONTROL | `steer = 165 > 100` → **`Drive_FailSafe()` + `Steering_Disable()`** |
| 17 | `0x11` ESTOP | **`Drive_EmergencyOff()` in the ISR, then latched stop; needs console `e` or a reset to re-arm** |
| 18 | `0x12` PING | harmless reply |
| 19 | `0x13` STOP | motor ramps to 0, **steering snaps to centre** |
| anything else | falls to `default:` | ignored |

Steer 16–19 on a 0..100 scale is a hard left turn. Motor −91 is most of the
stick. Hard left with plenty of throttle is not an exotic input.

The compound probability is low — you need a byte error *and* motor ≈ −91 *and*
steer ∈ {16,17,19} in the frame right after it. But note what the 17 case costs:
the latched ESTOP is sticky by design and is **only cleared by a console
keypress or a board reset**. On a stage, a spurious latched ESTOP means walking
to the laptop.

CRC-8 removes the whole class: the cancellation depends on XOR's commutativity,
and a CRC is position-dependent by construction.

### 3.3 The replacement: CRC-8/AUTOSAR

**Polynomial 0x2F, init 0xFF, no reflection, final XOR 0xFF.** Check value for
the ASCII string `"123456789"` is **0xDF**.

Why this one:

- **Hamming distance 4 up to 119 data bits** (Koopman's tables; polynomial 0x97
  in Koopman notation). Our payload is 32 bits, well inside that. Confirmed by
  brute force above: **zero** undetected 1-, 2- and 3-bit errors. XOR-8 misses
  10 % of double-bit errors; this misses none of them, at all, ever.
- `0x12F` has an even number of set bits, so it is divisible by (x+1) — every
  odd-weight error pattern is detected.
- **It is a named standard.** `crccheck.crc.Crc8Autosar`, or
  `crcmod.mkCrcFun(0x12F, initCrc=0xFF, rev=False, xorOut=0xFF)`, or any online
  CRC calculator, will reproduce it. Two days before a demonstration, the
  ability for the RPi programmer to check their implementation against a third
  party matters more than the last fraction of a percent of coverage.
- `init=0xFF` / `xorout=0xFF` mean a stuck line does not produce a valid frame:
  `00 00 00 00 00` and `FF FF FF FF FF` both fail. (XOR-8 accepts `00 00 00 00 00`
  on the checksum — it is only rejected by the start-byte test.)

**Considered and rejected: the reflected form** (poly 0xF4 = 0x2F reflected,
same init/xorout). It has identical HD-4 behaviour *and* detects all wire-order
bursts ≤ 8 bits, where the forward form misses 8 of ~4200 (see the table in
§3.1) — because a reflected CRC processes bits LSB-first, the same order a UART
transmits them. Genuinely the better-matched choice on the mathematics. Rejected
anyway: it is not a catalogue entry, so nobody can cross-check it against a
library, and "the two sides disagree" is a far larger risk than eight burst
patterns. If you ever revisit this with time to test properly, the reflected
form is the technically superior option.

**Compute the CRC over all four bytes including the `0xA5` start byte** — same
coverage as the XOR it replaces, so the frame layout does not change at all and
only the algorithm differs.

**Cost.** Table-free, 32 bit-iterations. I compiled the exact function below for
Cortex-M4:

| build | inner loop | total for 4 bytes | at 170 MHz |
|---|---|---|---|
| `-Os` (Release) | 9 instructions, ~10 cycles/bit | ~320 cycles | **~1.9 µs** |
| `-O0` (Debug — **the default in this project**) | 13 instructions, ~17 cycles/bit | ~600 cycles | **~3.5 µs** |

Called at most twice per frame. Even at the claimed several-hundred frames per
second and at `-O0`, that is under 0.2 % of the CPU, and about 4 % of one
character time inside the ISR. It is free. Do **not** add a 256-byte table for
this; if you ever want it faster, a 16-entry nibble table halves it for 16 bytes
of flash.

Note in passing: `CMakeLists.txt` defaults `CMAKE_BUILD_TYPE` to `Debug`, i.e.
`-O0` (`cmake/gcc-arm-none-eabi.cmake:35`). Worth knowing what you are flashing
on the day. I would *not* switch to Release now — different code timing two days
before a demonstration is its own risk — but check which one you have been
testing with and take that one to the ministry.

### 3.4 C implementation (STM32)

```c
// CRC-8/AUTOSAR: polynomial 0x2F, init 0xFF, no input/output reflection,
// final XOR 0xFF. Check value for the ASCII string "123456789" is 0xDF.
//
// Chosen over the XOR-8 this replaces for one specific reason: XOR is
// commutative, so it does not check byte position. A parser that has locked
// onto a payload byte of value 0xA5 (motor percent -91) sees the frame's two
// 0xA5 bytes cancel, and the check collapses to "cmd(N) == cmd(N+1)", which is
// true on every frame of a steady CONTROL stream. Enumerated: XOR-8 accepts
// 100 % of those captures, this accepts 1 in 256.
//
// Table-free on purpose: 32 bit-iterations is about 1.9 us at -Os and 3.5 us
// at -O0 on this part, against a character time of 87 us. A 256-byte table
// would buy nothing that matters and cost flash.
//
// THE RASPBERRY PI SENDER MUST BE CHANGED IN THE SAME BREATH. There is no
// compatibility window: an old sender's XOR byte will fail this check on
// essentially every frame and the vehicle will not move at all.
static uint8_t UART_Crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFFU;

    for (uint8_t i = 0U; i < len; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            crc = (uint8_t)((crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x2FU)
                                          : (uint8_t)(crc << 1));
        }
    }

    return (uint8_t)(crc ^ 0xFFU);
}

// Kept at four scalar arguments so every existing call site is unchanged.
// The name is now accurate: this really is a CRC.
static uint8_t UART_CalcCRC(uint8_t start, uint8_t cmd, uint8_t motor, uint8_t steer)
{
    const uint8_t buf[4] = { start, cmd, motor, steer };

    return UART_Crc8(buf, 4U);
}
```

Add `static uint8_t UART_Crc8(const uint8_t *data, uint8_t len);` to the
prototypes in `USER CODE BEGIN PFP` next to the existing `UART_CalcCRC`
declaration. **No call site changes.** The three call sites — the ESTOP latch in
the ISR, `UART_ProcessPacket()`, `UART_SendPacket()` — all keep working as
written.

### 3.5 Python implementation (Raspberry Pi) — must match exactly

```python
"""Frame codec for the STM32 car link. Must stay byte-identical to
   UART_Crc8() / UART_CalcCRC() in car_fw/Core/Src/main.c."""

import struct

START = 0xA5
CMD_CONTROL = 0x10
CMD_ESTOP   = 0x11
CMD_PING    = 0x12
CMD_STOP    = 0x13

FRAME_LEN = 5


def crc8(data: bytes) -> int:
    """CRC-8/AUTOSAR: poly 0x2F, init 0xFF, no reflection, xorout 0xFF.

    Cross-check: crc8(b"123456789") == 0xDF.
    Equivalent to crcmod.mkCrcFun(0x12F, initCrc=0xFF, rev=False, xorOut=0xFF)
    and to crccheck.crc.Crc8Autosar.
    """
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x2F) & 0xFF if (crc & 0x80) else ((crc << 1) & 0xFF)
    return crc ^ 0xFF


def build_frame(cmd: int, motor: int = 0, steer: int = 50) -> bytes:
    """Build one 5-byte frame.

    motor: -100..+100 (signed percent). steer: 0..100 (0=left, 50=centre).
    Clamped here rather than raising: the receiver treats an out-of-range
    CONTROL value as a fault and performs a full emergency stop, so a stray
    101 from the joystick would brake the car. Never let one reach the wire.
    """
    motor = max(-100, min(100, int(motor)))
    steer = max(0, min(100, int(steer)))

    body = struct.pack("<BBbB", START, cmd, motor, steer)
    return body + bytes([crc8(body)])


# --- self-test: these vectors are also in notes/rpi-link-review.md ----------
if __name__ == "__main__":
    assert crc8(b"123456789") == 0xDF, "CRC-8/AUTOSAR check value wrong"

    vectors = {
        (0xA5, 0x10, 0x00, 0x32): 0x63,   # CONTROL, stop, centre
        (0xA5, 0x10, 0x01, 0x32): 0x8A,   # CONTROL, +1 %
        (0xA5, 0x10, 0x64, 0x00): 0xEB,   # CONTROL, +100 %, full left
        (0xA5, 0x10, 0x9C, 0x64): 0xBB,   # CONTROL, -100 %, full right
        (0xA5, 0x10, 0xA5, 0x32): 0x12,   # CONTROL, -91 % (motor byte == 0xA5)
        (0xA5, 0x10, 0xA5, 0x10): 0x3F,   # the frame that breaks XOR-8
        (0xA5, 0x11, 0x00, 0x32): 0x6D,   # ESTOP
        (0xA5, 0x12, 0x00, 0x00): 0xFC,   # PING
        (0xA5, 0x13, 0x00, 0x32): 0x71,   # STOP
        (0xA5, 0x00, 0x00, 0x00): 0x00,
        (0xA5, 0xFF, 0xFF, 0xFF): 0x93,
    }
    for body, expect in vectors.items():
        got = crc8(bytes(body))
        assert got == expect, f"{body}: got 0x{got:02X}, expected 0x{expect:02X}"

    assert build_frame(CMD_CONTROL, 0, 50) == bytes([0xA5, 0x10, 0x00, 0x32, 0x63])
    print("codec OK")
```

A minimal sender that does the things §8 says it must:

```python
import serial, time

class CarLink:
    def __init__(self, port="/dev/ttyAMA0", baud=115200, period=0.02):
        # exclusive=True so a forgotten minicom/screen cannot share the port.
        # write_timeout so a wedged port raises instead of blocking the loop.
        self.ser = serial.Serial(
            port, baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0, write_timeout=0.05,
            rtscts=False, dsrdtr=False, xonxoff=False,
            exclusive=True,
        )
        self.period = period

    def send(self, cmd, motor=0, steer=50):
        # ONE write() call for the whole frame. Never write byte by byte and
        # never split a frame across two writes: the receiver's resynchroniser
        # and (if enabled) its idle-gap detector both assume a frame arrives as
        # a contiguous run of five bytes.
        self.ser.write(build_frame(cmd, motor, steer))

    def run(self, read_stick):
        next_t = time.monotonic()
        try:
            while True:
                motor, steer = read_stick()
                self.send(CMD_CONTROL, motor, steer)
                next_t += self.period
                time.sleep(max(0.0, next_t - time.monotonic()))
        finally:
            # Do not merely stop sending and rely on the 300 ms watchdog.
            # Say it explicitly, several times, in case one frame is lost.
            for _ in range(5):
                self.send(CMD_CONTROL, 0, 50)
                time.sleep(0.005)
```

### 3.6 This is a coordinated change — state it in the commit message

**The moment either side is flashed with CRC-8 and the other is still on XOR-8,
the link stops working completely.** Not degraded: dead. The receiver rejects
essentially every frame (a 1-in-256 accident aside), the CONTROL watchdog trips
after 300 ms, the drive never arms, and the car does not move at all.

That failure is loud and unmistakable, which is the one good thing about it. But
it means:

- Flash the STM32 and update the RPi in the same session, before any test.
- Run the Python self-test above (`python3 codec.py` → `codec OK`) *before*
  connecting anything.
- Keep the previous STM32 binary on the laptop so you can go back to a
  known-working pair in one minute if the day goes wrong.
- If the RPi source is under version control, tag both sides with the same
  marker so nobody pairs mismatched versions later.

---

## 4. Frame synchronisation

### 4.1 The parser, stated precisely

State is one index. At index 0 the parser discards everything that is not
`0xA5`. Once it accepts an `0xA5` it takes the next four bytes unconditionally,
publishes, and returns to index 0. There is no length field, no end delimiter,
and — correctly — no mid-frame resync on `0xA5`.

So the parser's only alignment evidence is "a byte with value `0xA5` arrived
while the parser was idle". Everything below follows from that.

### 4.2 Trace: one lost byte

Frame N is `[A5][C][M][S][X]`. Suppose `S` is lost (noise ate a stop bit; the
byte is dropped with a framing error).

1. Parser has `A5, C, M` and is at index 3.
2. Next byte is `X` → stored at index 3.
3. Next byte is `A5`, **the start byte of frame N+1** → stored at index 4, frame
   complete. The parser has assembled `[A5][C][M][X][A5]`.
4. Checksum check on that: fails, 255 times out of 256 with XOR-8.
5. Parser returns to index 0, sitting *inside* frame N+1, which still has
   `[C'][M'][S'][X']` to deliver.
6. It now scans those four bytes for `0xA5`:
   - `C'` is a command, 0x10–0x13 — never `0xA5`.
   - `S'` is 0..100 — never `0xA5`.
   - `M'` **can** be `0xA5` (motor −91).
   - `X'` can be any value including `0xA5`.
7. If neither `M'` nor `X'` is `0xA5`, the parser waits, frame N+2 starts with
   `0xA5`, and it **realigns. Two frames lost.**
8. If `M'` is `0xA5`, the parser locks onto it at offset +2 — and that is exactly
   the capture from §3.2, which XOR-8 accepts with probability 1.

So the answer to "how many frames are lost before it realigns" is: **two, in the
common case, and it realigns by accident rather than by construction.** The
mechanism is "payload bytes are rarely `0xA5`", not "the parser knows where the
frame boundary is".

### 4.3 Does the 10 ms partial-frame timeout save it? Only if there is an idle gap

The timeout fires when `uart_packet_index != 0` and no byte has arrived for
10 ms. Whether that ever happens depends entirely on the RPi's send rate, which
is **UNCONFIRMED**:

- **If frames are paced at 20–50 Hz** (20–50 ms apart, frame time 434 µs, so a
  19.5–49.5 ms idle gap): the timeout fires reliably and clears any stalled
  partial frame well before the next frame. It works. It is also, today, the
  thing that calls `Drive_FailSafe()` and hard-brakes the car (L3).
- **If the RPi streams frames back-to-back**, as the comment currently in
  `main.c` asserts: bytes arrive every 87 µs forever, `last_rx_byte_tick` is
  refreshed constantly, and **the 10 ms timeout never fires at all**. It is dead
  code. Resynchronisation then rests entirely on step 7 above — luck.

And this is the important consequence: **an IDLE-line interrupt has exactly the
same dependency.** IDLE fires when the line is quiet for one character time
after data. With a back-to-back sender the line is never quiet, IDLE never
fires, and the "obvious candidate" fix does nothing whatsoever.

So: the current code *does* exploit the inter-frame gap, via the timeout — but
only if the gap exists, and nobody here has measured whether it does.

### 4.4 The fix that does not care: sliding-window resync on checksum failure

When a five-byte window fails its checksum, do not throw away all five bytes.
Throw away **one** byte, then look for the next `0xA5` among the four still in
hand and slide it to the front.

```c
static void UART_Resync(void)
{
    for (uint8_t i = 1U; i < UART_PACKET_SIZE; i++)
    {
        if (uart_packet[i] == UART_PACKET_START)
        {
            memmove(uart_packet, &uart_packet[i], (size_t)(UART_PACKET_SIZE - i));
            uart_packet_index = (uint8_t)(UART_PACKET_SIZE - i);
            return;
        }
    }

    uart_packet_index = 0U;
}
```

Re-run the §4.2 trace with it:

1–4. As before: the parser assembles `[A5][C][M][X][A5]` and the checksum fails.
5. `UART_Resync()` scans positions 1..4, finds `0xA5` at position 4, slides it
   down. Buffer is `[A5]`, index 1.
6. The next four bytes are frame N+1's `[C'][M'][S'][X']` — a **correctly
   aligned frame**. It passes and is delivered.

**One frame lost. Deterministically. With no assumption about the sender's
timing, no idle gap required, and no new peripheral configuration.**

Both figures were checked by simulating the parser over 300 trials of 40 frames
with random joystick values and one byte deleted from frame 10:

```
XOR8  resync=off   frames lost after one byte error: 2.00   bogus frames accepted: 2/300
XOR8  resync=on    frames lost after one byte error: 1.00   bogus frames accepted: 0/300
CRC8  resync=off   frames lost after one byte error: 2.00   bogus frames accepted: 2/300
CRC8  resync=on    frames lost after one byte error: 1.01   bogus frames accepted: 2/300
```

Exactly 2.00 without it, exactly 1.00 with it — the trace above is not an
optimistic case, it is the only case.

Why it is bounded rather than statistical: the resync only ever discards bytes
that precede the earliest `0xA5` still in the window, so the true frame start is
*never* skipped past. Every candidate alignment is tested, and with CRC-8
behind it, wrong ones are rejected at 255/256. Each resync consumes at least one
byte, so it always makes progress and cannot loop.

Note what it does *not* do: it never resyncs inside a frame that is still being
assembled. The deliberate "do not resync on `0xA5` mid-frame" rule is preserved
exactly. Alignment is only ever reconsidered *after* a complete window has
failed its integrity check — which is new information the old code threw away.

Cost: one CRC (~3.5 µs at `-O0`) plus a ≤4-byte `memmove` per failed window.
Worst case — a line full of `0xA5` garbage — one CRC per received byte, 3.5 µs
out of an 87 µs character time. Fine.

**The IDLE-line interrupt is then optional, and I recommend leaving it out for
now.** With the sliding resync in place its only remaining value is clearing a
stalled partial frame in 87 µs instead of 10 ms, which nothing depends on. It
also carries a real risk: if the RPi ever splits a frame across two `write()`
calls with more than one character time between them, an IDLE reset would
destroy *every* frame. The code, if you want it after the bench proves the
sender is paced and atomic, is in §7.6 — with the trap that the HAL will not
clear `IDLEF` for you in standard reception mode, so it must be cleared in
`USER CODE BEGIN USART1_IRQn 0` or the interrupt storms forever.

### 4.5 And stop hard-braking on a truncated frame

Keep the 10 ms partial-frame timeout — it still has a job, clearing stale bytes
if the stream stops mid-frame and later resumes. Remove the `Drive_FailSafe()`
hanging off it, and count the event instead. Rationale: a truncated frame means
one command was lost out of however many hundred per second. A *dead link* is
what should drop the bridge, and the 300 ms CONTROL watchdog does that already
and more reliably. Trading a hard brake per glitch for at most 300 ms of stale
command is the right trade, and the ramp bounds what a stale command can do
(§5).

If you want a middle ground, trip the fail-safe only after N consecutive bad
frames in a window. I am not recommending it: that is new logic with new failure
modes, two days out, to cover a case the 300 ms watchdog already covers.

---

## 5. What a mis-decoded but checksum-passing packet can actually do

### 5.1 The numbers

From `tim.c`, `motor_dc.c` and `main.c`:

| quantity | value |
|---|---|
| TIM1 `ARR` | 8499 → 8500 counts/period, **20.0 kHz** PWM |
| `DC_PWM_MAX` | 5000 counts = 58.8 % duty |
| `DC_PWM_MIN_START` (breakaway) | 1280 counts = **15.06 % duty** |
| speed limit default | 40 % |
| top of the commanded span | 1280 + (5000−1280)×40/100 = **2768 counts** = 32.6 % duty |
| usable stick span above breakaway | 2768 − 1280 = **1488 counts** |
| ramp | `MOTOR_RAMP_STEP` 5 counts per 2 ms tick = **2500 counts/s** = 29.4 %-duty/s |
| 0 → top at the 40 % limit | 2768/5 = 553.6 ticks = **1.107 s** |
| 0 → breakaway | 1280/5 = 256 ticks = **512 ms** |

A bad packet is in force for exactly one inter-packet period T, until the next
good packet resets the target:

| sender rate | T | ramp travel in T | as % of PWM scale | as % of stick span |
|---|---|---|---|---|
| back-to-back (~230 fps) | 4.34 ms | 2 ticks = 10 counts | 0.12 % | 0.7 % |
| 50 Hz | 20 ms | 10 ticks = 50 counts | 0.59 % | 3.4 % |
| 20 Hz | 50 ms | 25 ticks = 125 counts | 1.47 % | 8.4 % |

### 5.2 The runaway case: the ramp makes it a non-event

Worst imaginable acceleration mis-decode: the car is stationary and a bogus
packet commands full throttle. The target jumps to 2768 counts; the ramp
advances at most **125 counts** before the next real packet pulls it back to
zero. Breakaway is 1280 counts.

**125 < 1280. The motor does not turn at all.** A single spurious full-throttle
packet from standstill produces no motion whatsoever, in any of the three rate
regimes. To reach breakaway you need 512 ms of continuously wrong commands —
11 consecutive bad packets at 20 Hz, 118 at 230 fps. At XOR-8's 1/256 residual
for random garbage, 11 in a row is (1/256)¹¹ ≈ 10⁻²⁶. It will not happen by
chance.

At speed, a bogus reverse command: from the 2768-count top, the ramp descends
125 counts in 50 ms → 2643. A 4.5 % torque dip for a twentieth of a second.
Imperceptible. (The 300 ms reverse dwell only arms if the ramp actually reaches
zero, which needs 1.1 s of wrong commands.)

**So the ramp has already solved the runaway problem completely.** That is a
real result and it should change how the checksum upgrade is justified.

### 5.3 The case that actually hurts: a mis-decode that triggers a stop

The ramp is bypassed on every fault path. A checksum-passing frame that decodes
to ESTOP, to STOP, or to CONTROL-with-an-out-of-range-value causes:

- `MotorDC_Stop()` — `motor_current = 0`, both compares written to zero **within
  one 50 µs PWM period**. Both low-side switches on, motor shorted through the
  bridge. That is a **brake**, from whatever speed the car was doing, with no
  ramp on the way down.
- `HAL_Delay(3)`, then R_EN/L_EN low.
- `Steering_Disable()` — and note `steering.c` sets `current_pulse_us =
  STEERING_CENTER_US` and calls `Steering_WritePulse()` **immediately**, with no
  slew. From full lock that is a full-speed servo slam to centre.
- Recovery: 512 ms of ramp before the wheels turn again, 1.107 s to regain
  speed. If the mis-decode was ESTOP, the latch is sticky and needs a console
  keypress or a reset — it does not recover at all on its own.

**That is the worst physical consequence, and it is a stop, not a runaway.** For
a vehicle carrying nothing and demonstrating on a smooth floor, an unexplained
hard brake plus a steering snap in front of an audience is the failure that
matters.

### 5.4 So how much does the checksum upgrade really matter?

Reframed by the above:

- **Against runaway: barely at all.** The ramp already reduces any single bad
  packet to under 1.5 % of PWM scale, below the motor's breakaway threshold.
  The ramp was the right fix and it did its job.
- **Against spurious stops: a great deal, and specifically because of §3.2.**
  The offset+2 misalignment is accepted by XOR-8 with probability **1**, and
  three of its four dangerous decodes are stops. CRC-8 takes that to 1/256 and
  the sliding resync removes the misalignment that feeds it.

The honest summary: **upgrade the checksum not because the car might run away —
it cannot — but because the car might stop for no reason, hard, in front of the
audience, and with XOR-8 there is a deterministic mechanism by which it does.**

---

## 6. What can stop the link working on the day, ranked by likelihood

**6.1 — The Raspberry Pi's serial console owns the port.** The classic, and it
is first for a reason. If `/boot/firmware/cmdline.txt` (or `/boot/cmdline.txt`)
still contains `console=serial0,115200`, and/or `serial-getty@ttyS0.service` is
enabled, then the kernel prints boot messages into the STM32 and a login prompt
sits on the port. The joystick app then either cannot open it, or shares it with
a getty that echoes everything back.
Good news specific to this protocol: console output is ASCII, all bytes < 0x80,
so it can never contain `0xA5` and can never start a frame. It will not make the
car move. It will stop it from being driven.
Check: `raspi-config` → Interface Options → Serial Port → **login shell: NO**,
**hardware enabled: YES**; then `systemctl status serial-getty@ttyS0` and
`grep console /boot/firmware/cmdline.txt`.

**6.2 — The connector, on a moving vehicle.** Dupont jumpers plus vibration is
the highest-probability *physical* failure and it is under-rated because it works
perfectly on the bench. Every intermittent contact is a burst of framing errors;
with the current firmware every burst that leaves a partial frame behind is a
hard brake (L3). Latch the connector, or at minimum strain-relieve and tape the
three wires as a bundle. Re-seat and wiggle-test before the run.

**6.3 — No common ground, or the ground shared with the motor return.** Two
distinct faults. TX/RX with no ground reference at all is intermittent and
baffling. Worse and more likely here: the signal ground returning through the
same conductor as BTS7960 motor current, so bridge switching moves the STM32's
ground relative to the Pi's and corrupts the sampling threshold. Use a dedicated
signal-ground wire from the Pi to the Nucleo, star-grounded at the battery
negative, routed with the data pair and away from the motor leads.

**6.4 — Which UART the Pi is using.** On a Pi 3/4/Zero W, `/dev/ttyS0` is the
mini-UART, whose baud rate is derived from the VPU core clock and **changes when
the core clock scales** unless `enable_uart=1` is in `config.txt` (which pins
it). `/dev/ttyAMA0` is the PL011 with a fixed clock and is the one to use — free
it with `dtoverlay=disable-bt`. Symptom of getting it wrong: works when the Pi
is idle, corrupts under load. Ask which device node the sender opens (§8).

**6.5 — RX floating while the Pi is not driving it.** L4 above. PB7 has no
pull-up, so any interval where the Pi is unpowered, resetting, or unplugged
leaves the STM32's RX input floating next to a switching bridge. Fix in §7.3;
it is two lines.

**6.6 — Motor noise coupled into the data pair.** Keep the UART cable short,
away from and ideally perpendicular to the motor leads, and twist RX with its
own ground return. If you have one, a clip-on ferrite at the Nucleo end costs
nothing to try. Do not add series resistors or capacitors on the day without
measuring — you can turn a working link into a marginal one.

**6.7 — Baud rate error from the clock configuration.** I checked this
carefully and it is **not** a likely cause, contrary to where it sat on the
brief's list. PLL: HSI16 → /4 → ×85 → /2 = 170 MHz SYSCLK; APB2 = /1 = 170 MHz;
USART1 clocked from PCLK2 (`RCC_USART1CLKSOURCE_PCLK2`), OVER16. `BRR` =
(170 000 000 + 57 600)/115 200 = **1476**, giving 115 176.1 baud, **−0.021 %**.
The divisor is essentially exact.
The residual risk is the HSI16 itself: ±1 % at 25 °C, roughly ±2 % over the
temperature range, against a receiver tolerance of about ±3 % for 8N1/OVER16 —
so the STM32 alone eats up to two-thirds of the budget, and if the Pi is on the
mini-UART with a drifting core clock (6.4), the two errors can add.
**Do not switch to HSE two days before the demonstration.** The `.ioc` lists
`PF0/PF1` as HSE-External-Oscillator with `HSE_VALUE=24000000`, but the
generated `SystemClock_Config()` uses HSI and I have no evidence a crystal is
actually fitted on this Nucleo (**UNCONFIRMED**). Changing the clock tree
changes every timer period in the system. If you want margin, the cheap
measure is to run the demo in a room at a normal temperature and let the board
warm up before the run.

**6.8 — Pi-side software: power, SD card, process supervision.** A brownout or a
corrupted SD card reboots the Pi mid-demo. Give the Pi its own supply, not a tap
off the motor rail, and run the sender under systemd with `Restart=always`. This
is more likely than 6.7 and cheaper to fix.

**6.9 — Logic levels.** Both ends are 3.3 V, so this is fine as built — recorded
only so nobody "helpfully" inserts a 5 V level shifter or a 5 V USB-TTL adapter.

**6.10 — Latent firmware faults.** L5 (receiver never re-armed) and the
tick-race class. Currently unreachable as far as I can trace, but they are the
category where the failure is silent and total. §7.2 is the insurance.

---

## 7. The patch

Against the snapshot described at the top. **All of `main.c` is expressed as
complete replacement functions or as insertions anchored on quoted text**, so it
applies over whatever the concurrent edit left behind. Re-read each anchor first.

Apply in the order given. Each tier is independently useful and independently
revertible.

### Tier 1 — do these

#### 7.1 `main.c` — replace the checksum (coordinated with the RPi)

Replace the whole body of `UART_CalcCRC()` with the two functions in §3.4, and
add the `UART_Crc8` prototype next to the existing `UART_CalcCRC` prototype in
`USER CODE BEGIN PFP`. **No call site changes.**

Verify after flashing: the three call sites are the ESTOP latch in
`HAL_UART_RxCpltCallback()`, the check in `UART_ProcessPacket()`, and the frame
built in `UART_SendPacket()`. All three must use the new function — if
`UART_SendPacket()` were left on XOR the PING replies would be undecodable.

#### 7.2 `main.c` — bounded resynchronisation, and stop braking on a lost byte

**(a)** Add near the other UART statics in `USER CODE BEGIN PV`:

```c
// Link health counters. Not used for any control decision -- they exist so a
// marginal cable can be told apart from a software fault on the bench, from
// the console, without a logic analyser.
static volatile uint32_t uart_bad_frames   = 0U;  // failed the CRC
static volatile uint32_t uart_partial_drop = 0U;  // frame started, never finished
static volatile uint32_t uart_rx_restarts  = 0U;  // receiver found disarmed
```

**(b)** Add the prototype `static void UART_Resync(void);` in `USER CODE BEGIN
PFP`, and the function itself next to `UART_ProcessPacket()`:

```c
// Recover frame alignment after a five-byte window fails its CRC.
//
// Drop ONE byte, then look for the next start byte among the four still in
// hand and slide it to the front. This never discards a byte that could still
// be the true frame start, so the correct alignment is always among those
// tested -- which is what makes recovery bounded rather than statistical.
// With CRC-8 behind it, a wrong alignment is rejected 255 times out of 256,
// so a single lost byte costs exactly one frame.
//
// Note what this does NOT do: it never resyncs inside a frame still being
// assembled. Byte 2 is a signed motor percent and can legitimately be 0xA5
// (-91 %), so mid-frame resync would be wrong. Alignment is reconsidered only
// AFTER a complete window has failed -- information the old parser discarded.
//
// Each call consumes at least one byte, so it always makes progress.
static void UART_Resync(void)
{
    for (uint8_t i = 1U; i < UART_PACKET_SIZE; i++)
    {
        if (uart_packet[i] == UART_PACKET_START)
        {
            memmove(uart_packet, &uart_packet[i], (size_t)(UART_PACKET_SIZE - i));
            uart_packet_index = (uint8_t)(UART_PACKET_SIZE - i);
            return;
        }
    }

    uart_packet_index = 0U;
}
```

`<string.h>` is already included in `main.c`; `memmove` needs nothing new.

**(c)** Replace the frame-complete block inside `HAL_UART_RxCpltCallback()`.
Anchor: the `if (uart_packet_index >= UART_PACKET_SIZE)` block, which currently
does the ESTOP latch, then `memcpy` to `uart_packet_rx`, `uart_packet_ready = 1U`,
`uart_packet_index = 0U`. Replace that block with:

```c
            if (uart_packet_index >= UART_PACKET_SIZE)
            {
                // The CRC is computed once, here, and decides two things:
                // whether this window is a real frame at all, and -- if not --
                // that the parser is misaligned and must slide (UART_Resync).
                // The main loop checks the CRC again on its own copy; that is
                // deliberate defence in depth and costs a few microseconds.
                const uint8_t crc_calc = UART_CalcCRC(uart_packet[0],
                                                      uart_packet[1],
                                                      uart_packet[2],
                                                      uart_packet[3]);

                if (crc_calc != uart_packet[4])
                {
                    // Not a valid frame. Do not publish it, and do not throw
                    // all five bytes away: slide to the next candidate start.
                    uart_bad_frames++;
                    UART_Resync();
                }
                else
                {
                    // ESTOP is taken out of the mailbox and latched right here.
                    //
                    // The mailbox below holds exactly one packet and the newest
                    // one wins, which is correct for CONTROL -- an old throttle
                    // value is worthless. It is wrong for ESTOP: the RPi streams
                    // CONTROL continuously, so a CONTROL packet lands behind the
                    // ESTOP within a millisecond and overwrites it, and the car
                    // never stops. Latching cannot be lost, and acting on the
                    // same stop twice is harmless.
                    if (uart_packet[1] == UART_CMD_ESTOP)
                    {
                        estop_latched = 1U;

                        // Kill the bridge now, at register level, instead of
                        // waiting for the main loop: a monitor console command
                        // can hold that loop for a few hundred milliseconds,
                        // and an emergency stop must not queue behind it. The
                        // main loop still runs the full, ordered fail-safe.
                        Drive_EmergencyOff();
                    }

                    // Latest complete packet wins: correct for a high-rate
                    // stream of repeated control commands.
                    memcpy(uart_packet_rx, uart_packet, UART_PACKET_SIZE);
                    uart_packet_ready = 1U;

                    uart_packet_index = 0U;
                }
            }
```

The ESTOP latch is now inside the CRC-valid branch, so the redundant second CRC
evaluation it used to do is gone — the new code is not slower than the old one
despite the stronger algorithm.

**(d)** In the main loop, replace the partial-frame block. Anchor: the comment
`// --- Stalled partial-frame recovery ---` through the
`if (partial_frame_dropped && drive_enabled) { Drive_FailSafe(); }` that follows
it. Replace all of it with:

```c
    // --- Stalled partial-frame recovery --------------------------------
    // A frame that started and never finished leaves stale bytes in front of
    // the next real one. Drop it after a short timeout.
    //
    // The tick is read INSIDE the critical section. Read outside it, the USART
    // ISR could fire between the read and __disable_irq() and push
    // last_rx_byte_tick past the sampled "now" -- the subtraction then wraps to
    // a huge unsigned value and a frame that arrived microseconds ago is
    // discarded as stale, in the middle of the byte stream.
    //
    // This no longer touches the bridge. It used to call Drive_FailSafe(),
    // which zeroes the PWM with MotorDC_Stop() -- bypassing the ramp, shorting
    // the motor through the bridge, which is a brake -- and then needed 512 ms
    // of ramp to break away again and 1.1 s to regain speed. One glitch on a
    // jumper wire produced a visible lurch and stall. A truncated frame means
    // ONE command was lost out of hundreds a second; a DEAD LINK is what should
    // drop the drive, and the CONTROL watchdog above does exactly that within
    // 300 ms. The ramp bounds what a stale command can do in the meantime:
    // at most 125 PWM counts of travel out of 8500, which is below the
    // 1280-count breakaway threshold, so the car cannot even start moving on
    // one bad command.
    __disable_irq();
    {
        uint32_t frame_now = HAL_GetTick();

        if ((uart_packet_index != 0U) &&
            ((frame_now - last_rx_byte_tick) > UART_PARTIAL_FRAME_TIMEOUT_MS))
        {
            uart_packet_index = 0U;
            uart_partial_drop++;
        }
    }
    __enable_irq();

    // --- Receiver liveness ---------------------------------------------
    // Every restart of the byte receiver is a HAL call whose return value the
    // callbacks discard. If one ever returns HAL_BUSY, nothing in this
    // firmware re-arms it: the link dies silently and permanently, and only a
    // reset brings it back. I could not construct a path that reaches it in
    // this HAL version, but the failure is total and the insurance is cheap,
    // so check the receiver is actually armed.
    //
    // No false positives: RxState is only transiently READY inside the USART1
    // ISR, which the main loop cannot observe -- the ISR is never preempted by
    // main. Seeing READY here means reception really has stopped.
    if (huart1.RxState != HAL_UART_STATE_BUSY_RX)
    {
        uart_rx_restarts++;
        (void)HAL_UART_AbortReceive(&huart1);
        (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
```

Delete the now-unused `uint8_t partial_frame_dropped = 0U;` declaration.

**(e)** Add a warning comment to `HAL_UART_ErrorCallback()` so nobody
simplifies it. Replace the function with:

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // DO NOT "SIMPLIFY" THIS TO JUST RESETTING THE INDEX.
        //
        // On an overrun, HAL_UART_IRQHandler runs RxISR (which reaches
        // HAL_UART_RxCpltCallback, which re-arms reception), and only THEN
        // calls UART_EndRxTransfer -- which disarms the re-arm that just
        // happened -- and only then calls this function. The restart below is
        // the one that survives. Remove it and the first overrun kills the
        // link until the board is reset.
        uart_packet_index = 0U;

        (void)HAL_UART_AbortReceive(huart);
        (void)HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}
```

#### 7.3 `usart.c` and the `.ioc` — pull up the RX line

In `HAL_UART_MspInit()`, the USART1 branch currently configures PB6 and PB7
together with `GPIO_NOPULL`. Split them:

```c
    /**USART1 GPIO Configuration
    PB6     ------> USART1_TX
    PB7     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USER CODE BEGIN USART1_MspInit 1 */
    // RX gets an internal pull-up. A UART line at rest must be HIGH, and PB7
    // is driven only while the Raspberry Pi is powered and has the port open.
    // While the Pi boots, resets, or its connector is out, an unpulled PB7 is
    // a floating high-impedance input on a wire running beside a 20 kHz
    // BTS7960 bridge. Every noise excursion below threshold is a start bit,
    // and the framing errors and overruns that follow used to trip a full
    // drive fail-safe. ~40 kOhm internal, far too weak to load a real driver.
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    /* USER CODE END USART1_MspInit 1 */
```

Putting the PB7 block inside `USER CODE BEGIN/END USART1_MspInit 1` means it
survives a CubeMX regeneration. Also update the `.ioc` so the generator agrees —
add next to the existing `PB7.Locked` / `PB7.Mode` / `PB7.Signal` lines:

```
PB7.GPIOParameters=GPIO_PuPd
PB7.GPIO_PuPd=GPIO_PULLUP
```

A 4.7 kΩ external pull-up to 3.3 V at the Nucleo end is stronger and better if
the noise environment turns out to be bad, but do not add parts on demo day
without measuring first.

### Tier 2 — cheap, do them if there is time

#### 7.4 `main.c` — linkage and `volatile` hygiene

```c
static volatile uint8_t rx_byte;
static uint8_t          uart_packet[UART_PACKET_SIZE];
static volatile uint8_t uart_packet_index = 0U;
```

`rx_byte` is passed to `HAL_UART_Receive_IT()` as `uint8_t *`, so making it
`volatile` needs a cast at the two call sites — `(uint8_t *)&rx_byte`. If that
feels like more churn than it is worth right now, `static` alone is the useful
half. No behavioural change either way; this removes an argument, not a bug.

#### 7.5 `main.c` — report the link counters on the console

Add to `Console_AuxKey()`, next to the existing `'m'` case:

```c
        case 'u':
        case 'U':
            bms_printf("\r\nlink: bad_crc %lu  partial %lu  rx_restarts %lu  "
                       "last_ctrl %lu ms ago\r\n",
                       (unsigned long)uart_bad_frames,
                       (unsigned long)uart_partial_drop,
                       (unsigned long)uart_rx_restarts,
                       (unsigned long)(HAL_GetTick() - last_control_packet_tick));
            return true;
```

This is what turns "it felt laggy" into a number, on the floor, in five seconds.
It is the single most useful thing on the list for the bench session.

### Tier 3 — only after the bench tells you the sender is paced

#### 7.6 Optional idle-gap resync

Only worth adding if T2 in §9 shows a real inter-frame gap **and** shows that
the Pi emits each frame as five contiguous bytes. If the Pi streams back-to-back
this never fires; if the Pi ever splits a frame across two writes this destroys
every frame. The sliding resync in 7.2 already makes recovery deterministic
without either assumption — this only shortens 10 ms to 87 µs.

In `stm32g4xx_it.c`, inside `USER CODE BEGIN USART1_IRQn 0` (it **must** be
before `HAL_UART_IRQHandler`):

```c
  // Idle-line resync. The HAL only handles IDLE for HAL_UART_RECEPTION_TOIDLE
  // and this is a standard reception, so it will never clear IDLEF for us --
  // it must be cleared here or the interrupt re-fires forever.
  if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE) &&
      __HAL_UART_GET_IT_SOURCE(&huart1, UART_IT_IDLE))
  {
      __HAL_UART_CLEAR_IDLEFLAG(&huart1);
      UART_IdleResync();          // extern "C" hook in main.c: index = 0
  }
```

with, in `main.c`:

```c
// Called from USART1_IRQHandler on an idle line. One character time of silence
// after data means any partially assembled frame is finished -- and truncated.
void UART_IdleResync(void)
{
    if (uart_packet_index != 0U)
    {
        uart_packet_index = 0U;
        uart_partial_drop++;
    }
}
```

and, once after the initial `HAL_UART_Receive_IT()` in `main()`:

```c
  __HAL_UART_CLEAR_IDLEFLAG(&huart1);
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
```

Verified safe against the HAL: neither `HAL_UART_AbortReceive()` nor
`UART_EndRxTransfer()` clears `IDLEIE` unless `ReceptionType ==
HAL_UART_RECEPTION_TOIDLE`, which it never is here. So the enable survives every
error-recovery restart.

#### 7.7 Optional — decide what PING is for

If the RPi never sends PING, delete the case: it is the only path that blocks
the main loop on a polled `HAL_UART_Transmit()`. If it does use PING, rate-limit
the reply to ~10 Hz. Ask first (§8).

---

## 8. What to ask the Raspberry Pi side

There is no RPi source in this repository, so everything below is invisible from
here. The first four questions are the ones that decide whether the demo works.

**Ask, in this order:**

1. **What is the actual CONTROL packet rate, measured?** Not intended —
   measured, with a timestamp on each `write()`. This decides whether the
   partial-frame timeout and any idle detector ever fire, and how long a bad
   packet is in force. `main.c` currently asserts "hundreds per second"; nothing
   here verifies it.
2. **Is each frame written with a single `write()` of exactly 5 bytes?** Or
   byte-by-byte, or via a buffered `io` wrapper that may split it? A split frame
   defeats any idle-gap detector and stresses the resynchroniser.
3. **Which device node, and is the console disabled?** `/dev/ttyAMA0` (PL011,
   fixed clock — the right answer) or `/dev/ttyS0` (mini-UART, baud tied to the
   core clock) or a USB adapter? Is `console=serial0` gone from `cmdline.txt`
   and `serial-getty@ttyS0` disabled?
4. **What happens when the joystick is unplugged, the app is killed, or it
   crashes?** Does it send zero-throttle CONTROL frames, or does it just stop and
   rely on the 300 ms watchdog? Both work; you need to know which, because one of
   them stops the car in 5 ms and the other in 300 ms.

Then:

5. Does it clamp motor to −100..+100 and steer to 0..100 **before** packing?
   An out-of-range CONTROL value is not ignored by the STM32 — it triggers a
   full emergency stop. A joystick that occasionally emits 101 will brake the
   car for no visible reason.
6. How is the signed motor byte packed? It must be a two's-complement `int8_t`
   (`struct.pack("b", ...)`) — not `abs()` plus a sign flag, not an unsigned
   0..200 offset.
7. Is there a deadband around joystick centre? A drifting analog stick that
   never returns to exactly zero makes the car creep continuously.
8. Does it use PING (0x12), and does it read the STM32's replies? If not, delete
   the PING path from both ends.
9. Does it use STOP (0x13)? It can be dropped by the STM32's one-packet mailbox
   (L6). `CONTROL motor=0` cannot. Prefer that.
10. Is the port opened with `exclusive=True`, and is there a `write_timeout`? A
    forgotten `minicom`/`screen` sharing the port, or a blocking write on a
    wedged port, both look like "the car stopped responding".
11. Is the sender a systemd service with `Restart=always`? What supervises it?
12. Does the send loop share a thread with a GUI or the video pipeline? Anything
    that can block it for over 300 ms will trip the watchdog and stop the car
    mid-run.

**What the sender must get right, restated as requirements:**

- **CRC-8/AUTOSAR over the 4 bytes `[0xA5, cmd, motor, steer]`**, poly 0x2F,
  init 0xFF, no reflection, final XOR 0xFF, appended as byte 5. Verify
  `crc8(b"123456789") == 0xDF` and every vector in §3.5 before connecting
  anything.
- Exactly 5 bytes per frame, one `write()`, no framing characters, no newline,
  no text.
- Nothing else may ever be written to that port. No debug prints, no banner, no
  `print()` that accidentally targets the serial device.
- Clamp both values before packing, always.
- 115200 8N1, no flow control (`rtscts=False`, `xonxoff=False`, `dsrdtr=False`).
- On exit, on exception, and on joystick loss: send several zero-throttle
  CONTROL frames rather than simply going quiet.
- Keep the send period stable. Whatever the rate is, the STM32's watchdog is
  300 ms — a stall longer than that stops the car and the ramp then needs 1.1 s
  to recover, so a hitch is very visible.

---

## 9. What to test on the bench

Before the vehicle is on the floor. **T1–T4 are the ones that must pass.**

**T1 — codec agreement, before anything is connected.** Run the Python
self-test (`codec OK`). Then flash the STM32 and send one known frame by hand:
`A5 10 00 32 63` (CONTROL, stop, centre) must arm the drive; `A5 10 00 32 87`
(the old XOR byte) must be rejected. If the second one is *accepted*, the
firmware did not take the patch.

**T2 — measure what the Pi actually emits.** Scope or logic analyser on PB7,
one capture of several seconds. Record: (a) the inter-frame gap, (b) whether the
five bytes of each frame are contiguous, (c) the bit time — 8.68 µs nominal;
more than about 1.5 % off and go and look at 6.4. **This single measurement
resolves the biggest open question in this review** and decides whether §7.6 is
worth adding.

**T3 — single-byte-loss recovery.** With the car on blocks and driving at a
steady throttle, inject one lost byte (briefly short PB7 to ground, or have the
Pi send one deliberately truncated 4-byte frame). Expected with the patch: the
console counter `bad_crc` increments by 1 or 2, the motor does **not** stop, and
speed does not visibly change. Before the patch, expect a hard stop and a slow
re-acceleration. If the motor still stops, the §7.2(d) change did not apply.

**T4 — pull the cable while driving.** On blocks, at speed. The motor must stop
within 300 ms and stay stopped. Reconnect: the car must ramp back up smoothly,
not jump. Do this at least five times — it is the demo-day failure mode and the
one most worth having muscle memory for.

**T5 — Pi reboot with the STM32 already powered.** Reboot the Pi with the link
connected and the car on blocks. The motor must not twitch at any point during
boot, and the link must come back on its own. This exercises the floating-RX
case (6.5) and any console output on the port (6.1). Check the `bad_crc` and
`partial` counters afterwards — a large `partial` count means the console is
still on the port.

**T6 — noise under load.** Run the motor at the highest speed you intend to
demonstrate, wiggle the connector, flex the cable, and watch the console
counters. `bad_crc` should be zero or single digits over a minute. Tens or
hundreds per minute means the wiring needs fixing before the demonstration, not
after.

**T7 — soak.** Thirty minutes of continuous driving on blocks with the joystick
being worked. Check `rx_restarts` is still 0 (any non-zero value means L5 is real
and the insurance earned its place — tell someone), and `bad_crc` is stable
rather than climbing.

**T8 — emergency stop, both routes.** Console `e` and a real ESTOP frame. Both
must stop the car; both must require a deliberate re-arm. Confirm the operator
knows how to clear the latch **without a laptop**, or accept that a spurious
latch ends the demonstration.

**T9 — the range-check trap.** Have the Pi send one CONTROL with steer = 101.
The car will emergency-stop. This is by design, and the point of the test is
that everyone sees what an out-of-range value costs, so the Pi's clamping is
taken seriously.

---

## 10. UNCONFIRMED

Everything on this list is stated somewhere as fact but is not verified by
anything in this repository.

- **The RPi's CONTROL packet rate.** Asserted in `main.c` as "hundreds of
  packets per second"; the older "20..50 Hz" is asserted to be wrong. Neither is
  measured here. Resolve with T2. Affects §4.3, §4.5, §5.1 and whether §7.6 is
  worth anything.
- **Whether the RPi writes each frame atomically.** Assumed by §7.6. Resolve
  with T2.
- **Everything about the RPi's serial configuration** — device node, console
  state, port settings, supervision. §8.
- **Whether an HSE crystal is fitted on this Nucleo.** The `.ioc` declares
  PF0/PF1 as an external oscillator with `HSE_VALUE=24000000`, but
  `SystemClock_Config()` runs from HSI and NUCLEO-G474RE boards ship without X3
  populated. Nothing here depends on it; recorded because 6.7 mentions HSE as a
  theoretical option and it should not be attempted on the assumption that a
  crystal exists.
- **HSI16 accuracy over the actual demonstration temperature.** Quoted as ±1 %
  at 25 °C and roughly ±2 % over range from the datasheet; not measured on this
  board.
- **Whether the mechanical/connector risks in 6.2 and 6.3 exist as described.**
  I have not seen the vehicle. Ranked by what these failures usually are, not by
  inspection.
- **The exact cycle counts in §3.3** are from disassembling the given function
  with `arm-none-eabi-gcc -mcpu=cortex-m4` at `-Os` and `-O0` and counting
  instructions with typical M4 timings. Not measured on hardware. The conclusion
  (single-digit microseconds, irrelevant against an 87 µs character time) is
  robust to a factor of two either way.
- **`main.c` was being edited concurrently.** Line-level anchors may have moved.
  Re-read each anchor before applying.
