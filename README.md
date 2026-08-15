# packbench

Bench tooling for MAX17320-based lithium packs: an STM32G474 monitor and
provisioner, a live desktop dashboard, and the reverse-engineering it took
to make a TPS26750 + BQ25756 charging chain actually deliver current — plus
the vehicle firmware that carries the same pack monitor on board.

Built against a 2S4P pack of Molicel INR18650-P30B (11400 mAh nominal) on a
custom MAX17320G2 board with a 5 mΩ shunt.

The pack-monitor firmware is **read-only by default**. The only writes it can
perform are clearing the sticky alert history and, behind an explicit
confirmation, one NVM provisioning commit — and that commit exists **only in
the bench monitor**. The car build contains no provisioning code at all
(see [Provisioning](#provisioning) and [Current limits](#current-limits-at-5-mω)).

---

## Two firmware projects, one kind of board

Both projects run on a NUCLEO-G474RE and are flashed over the same ST-Link, so
say which one you mean before wiring anything or pressing any button:

| | Car | Bench monitor |
|---|---|---|
| Directory | `car_fw/` | `max17320_gui/` |
| Image | `build/Debug/motor_uart_pwm_nucleo.elf` | `build/Debug/max17320_gui.elf` |
| Gauge bus | **I2C3 — PC8 / PC9 on CN10** | **I2C1 — PA15 / PB7 on CN7** |
| Also on the board | IBT-2 motor driver, steering servo, Raspberry Pi link on USART1 (PB6/PB7) | nothing else |
| Provisioning (`!` menu) | **compiled out** (`BMS_REALTIME_HOST=1`, and `max17320_provision.c` is not built) | present, behind `BURN` |
| VS Code buttons | `Build car`, `Flash car` | `Build monitor`, `Flash monitor` |

---

## What is in here

| Path | What |
|---|---|
| `car_fw/` | The vehicle firmware (CubeMX, CMake): IBT-2 motor driver, steering servo, Raspberry Pi joystick link, and the MAX17320 monitor merged into the same superloop on I2C3. |
| `max17320_gui/` | CubeMX project (STM32G474RE, CMake) for the standalone bench monitor. Generated code only. |
| `fw/` | The firmware: I2C driver, register decoding, dashboard, provisioning, host tests. Shared by both projects, and outside the CubeMX trees so regeneration cannot touch it. |
| `tools/check_board.py` | One-shot health check of a board. Read-only. |
| `tools/bms_dashboard/` | PySide6 desktop dashboard with CSV recording. |
| `tools/serial_monitor.sh` | picocom on the VCP at 115200. |
| `notes/` | The research: register maps, board netlists, pinouts, the charger chain — every claim cited to a datasheet, schematic or user guide. |
| `data/` | A real capture: a 7 A charge session, one row per sample. |

The MAX17320 NVM **provisioning** driver this borrows its I2C layer from
lives in its own repository:
[stm32f767zi-max17320-bms](https://github.com/lxstxanax/stm32f767zi-max17320-bms).

---

## Wiring

**The two projects put the gauge on different pins.** Wire the one you are
actually running. Getting this wrong is not cosmetic: the pin the bench
monitor uses for SDA is the pin the car listens to the Raspberry Pi on.

### Car — `car_fw/`, I2C3, morpho **CN10**

| Signal | BMS board P1 | Nucleo G474RE |
|---|---|---|
| SCL | P1.3 | **PC8 = CN10 pin 2** |
| SDA | P1.2 | **PC9 = CN10 pin 1** |
| GND | P1.5 (AGND) | **CN10 pin 9** |
| pull-ups | — | **4.7–5 kΩ** from SDA and SCL to 3V3 (**CN7 pin 16** or **CN6 pin 4**) |

Checked against `car_fw/motor_uart_pwm_nucleo.ioc` (`PC8.Signal=I2C3_SCL`,
`PC9.Signal=I2C3_SDA`) and `car_fw/CMakeLists.txt`, which compiles the pin
names into the firmware's own bus check: `BMS_SCL_NAME="PC8 (CN10-2)"`,
`BMS_SDA_NAME="PC9 (CN10-1)"`. SDA and SCL are the two pins at the very end of
CN10, facing each other across the connector — pin 1 is the odd column, pin 2
the even one.

> ### ⚠ Do not wire the car by the bench monitor's table
>
> In `car_fw`, **PB7 is `USART1_RX` — the Raspberry Pi joystick link**
> (`PB7.Signal=USART1_RX`, `PB6.Signal=USART1_TX`; PB7 is exactly the CN7 pin
> 21 in the bench monitor's table below, and PB6 is Arduino D10 = CN5 pin 3).
> Hanging an I2C pull-up and a gauge on it kills the command link, and
> **PA15 is not configured at all** in the car build, so SCL simply would not
> work either. The old "CN7 pin 17 / 19 / 21" wiring below belongs to
> `max17320_gui/` only.

Two hazards specific to this end of CN10:

* **CN10 pin 3 is PB8 = BOOT0.** It is the next position along the odd column
  from SDA (pin 1). An I2C line idles high through its pull-up, including
  through reset, so a wire that slips one position boots the MCU into the
  system bootloader instead of your firmware — and JP7, the BOOT0 shunt, is
  not fitted, so nothing holds the pin low. The board then looks dead while
  being perfectly healthy.
* **CN10 pin 8 is raw USB 5 V** (`5V_USB_CHGR`), diagonally adjacent to the
  GND at pin 9. Count pins before clipping a ground lead on.

`notes/nucleo-g474re-pinout.md` flags the morpho GND list as the weakest item
in it (ST's own table survives HTML extraction badly). CN10 pin 9 = GND is the
standard Nucleo-64 layout and the rest of that layout is corroborated at five
verbatim points, but if you want certainty, buzz it out or use one of the
verbatim-confirmed grounds instead: **CN6 pin 6/7** or **CN5 pin 7**.

### Bench monitor — `max17320_gui/`, I2C1, morpho **CN7**

| Signal | BMS board P1 | Nucleo G474RE |
|---|---|---|
| SCL | P1.3 | PA15 = **CN7 pin 17** |
| GND | P1.5 (AGND) | **CN7 pin 19** |
| SDA | P1.2 | PB7 = **CN7 pin 21** |
| pull-ups | — | **4.7–5 kΩ** from SDA and SCL to 3V3 (CN7 pin 16) |

Checked against `max17320_gui/max17320_gui.ioc` (`PA15.Signal=I2C1_SCL`,
`PB7.Signal=I2C1_SDA`). Three consecutive positions in one column of CN7, with
3V3 for the pull-ups on the same connector. Looking at the top of the board
with the USB pointing away, CN7 is the outer-left header and pin 1 is at the
USB end; the odd column is the one nearest the board edge. The bottom
silkscreen prints the odd **pin numbers** — not signal names.

**CN7 pin 18 is +5V and sits directly opposite pin 17.** A solder bridge
across that row puts 5 V on a 3.3 V-only GPIO. Pin 15, one row up in the same
column, is SWCLK — bridge it and the debugger dies.

### Both projects — the BMS board end

**Do not connect P1.1 (SYSP).** It is pack positive, 6.0–8.4 V, and it sits
directly next to SDA. There is no ESD protection on SDA/SCL on that board.

**Ground to P1.5 only.** AGND is pack negative — the CSN side of the 5 mΩ
shunt R18. The cell-negative header J3 (BATTN) is the CSP side. Grounding to
both shorts the shunt and zeroes every current reading.

**The BMS board has no pull-ups of its own.** Net `SDA` is exactly
{P1.2, U1.9} and `SCL` is {P1.3, U1.8} in both the schematic and the PCB
netlist. Fit your own, to 3V3, never to AOLDO.

---

## Before driving

Six hardware items that no firmware change can substitute for. The previous
STM32 was destroyed by latch-up on a pin driving the motor driver's enable, and
the pack protector was tripping on motor start; everything below is the
hardware half of that fix. Work through it before the car moves under power.

- [ ] **2.2 kΩ in series in each of the four IBT-2 logic lines.**
      R_EN/L_EN are **PA6** (Arduino D12 = CN5 pin 5) and **PA7** (D11 = CN5
      pin 4); RPWM is **PA8** (D7 = CN9 pin 8) and LPWM is **PA9** (D8 = CN5
      pin 1) — pin functions from `car_fw/motor_uart_pwm_nucleo.ioc`, header
      positions from `notes/nucleo-g474re-pinout.md`. The resistor is what
      limits current injected back into the MCU's ESD clamps to a few mA,
      below the latch-up trigger, when the IBT-2's rail moves.
      `notes/car-firmware-review.md` §5 calls this the single highest-value
      change and puts the useful band at 330 Ω – 1 kΩ; 2.2 kΩ trades a little
      edge speed for more margin. After fitting them, check R_EN/L_EN still
      read a solid logic high **at the IBT-2 header** — any pull-down on the
      driver's input forms a divider with the series resistor.
- [ ] **JP5 moved to [5-6] and 5 V fed into E5V (CN7 pin 6).** JP5 is the 4×2
      "5V_SEL" header; the factory position is [1-2], which powers the board
      from the ST-Link USB and leaves it dead without a laptop attached.
      [5-6] is the one configuration that runs the target from an external
      5 V (4.75–5.25 V) **and** keeps the ST-Link usable: JP5 pin 1 is left
      open, so no contention path with USB VBUS exists. Do **not** instead
      push 5 V into the `5V` pin with JP5 at [1-2] — that is the fault case
      (`notes/nucleo-g474re-pinout.md`, Appendix A.4/A.5).
- [ ] **External power on before USB.** With JP5 at [5-6] the target is dead
      until E5V is live, so plugging USB in first gives you an ST-Link that
      enumerates over a board that is not running — which looks exactly like a
      dead debugger. Power up E5V, then USB; unplug USB first on the way down.
- [ ] **Motor power wires twisted together** the whole run from the pack to the
      IBT-2, with the return paired to its own feed. The load-dump transient
      when the MAX17320 opens mid-current is the event most likely to have
      reached PA7; a twisted pair keeps its loop area, and the field it throws
      at the logic harness, small.
- [ ] **Logic ground tapped at the pack negative — AGND only.** That is P1.5 on
      the BMS board, the **CSN side of the 5 mΩ shunt R18**. Never the
      cell-negative side (J3 / BATTN, the CSP side): bridging both shorts the
      shunt and zeroes every current reading, and motor return current must not
      share a conductor with the MCU's ground reference in any case — that
      shared return is what produced the ground bounce. Star-ground at the pack
      negative. Note that whatever current the MCU does return through AGND
      flows through the shunt and is counted by the gauge, so keep it small.
- [ ] **Steering limits measured before commanding position 0 or 100.** Every
      build deliberately carries a `#warning` about this: `steering.c` compiles
      2200 / 1550 / 1050 µs, while the comment in `main.c` says the limits are
      2130 / 1080 µs. Both pairs are legal pulse widths and both pass the
      compile-time guards, so nothing but a measurement settles it — and if
      `main.c` is the right one, position 0 commands 70 µs past the left stop
      and position 100 commands 30 µs past the right one. A servo held against
      its stop stalls and draws stall current continuously from the same 2S
      pack: a raised baseline, not a spike, so it does not look like a fault
      on a scope. With the servo (PA1, TIM2_CH2, A1 = CN8 pin 2) installed,
      walk the commanded position outward in small steps, watch where the
      linkage actually stops, and only then trust the ends of the range. Leave
      the `#warning` in place until that is done — it is the only thing that
      says so on every build.

If the build ever prints a **second** warning about `STEERING_DEMO_SELFTEST`,
do not flash it: that build drives the motor on its own, unattended.

---

## Bring-up

1. **Know which project you are flashing.** `Flash car` →
   `car_fw/build/Debug/motor_uart_pwm_nucleo.elf`; `Flash monitor` →
   `max17320_gui/build/Debug/max17320_gui.elf`. They use different I2C pins
   (see [Wiring](#wiring)), so the wrong image on the wrong hardware fails in
   confusing ways — on the car it also puts an I2C peripheral where the
   Raspberry Pi link should be.
2. **Pack first.** The gauge is powered from the pack (BATTP → 10 Ω → IN).
   With no cells it does not answer, and that reads exactly like a wiring
   fault.
3. **Pull-ups.** The BMS board has none of its own. Fit 4.7 kΩ to 3V3 on both
   lines.
4. **Flash**: VS Code task `Flash car` or `Flash monitor`.
5. **Watch**: task `Serial Monitor (ST-Link VCP)` — both projects put the
   console on LPUART1, the ST-Link VCP, at 115200. Expect `probe: OK` and
   `DevName 0x420A`. A garbage DevName means the bus is wrong, not the pack.
6. The green LED blinks slowly when the gauge answers and quickly when it
   does not, so the board tells you from across the bench.

---

## Console

| Key | Action |
|---|---|
| `d` | ANSI dashboard (default) |
| `j` | JSON stream, one object per sample — feeds the desktop dashboard |
| `r` | raw status register dump |
| `n` | NV config check against the pack profile |
| `x` | read any register — `x0D8` = Cell1, `x1B5` = nPackCfg |
| `b` | bus check: SCL/SDA idle levels, floating and against the MCU's 40 kΩ |
| `w` | life check / weak-bus mode |
| `c` | clear sticky protection alerts (asks first) |
| `p` | re-probe |
| `!` | provisioning menu |
| `ESC` | back to the top level from any prompt |

**Three of these do not exist in the car build.** `car_fw` compiles with
`BMS_REALTIME_HOST=1`, which removes everything that could block the motor
loop for seconds: `!` (the provisioning menu), `w` (the weak-bus probe) and
`t` (trip capture). The rest behaves identically, and the register poll is
spread a few registers at a time across loop iterations.

**`n`, `x`, `c`, `b` and `p` are refused while the bridge is armed.** They do
blocking I2C — `n` is ten register reads, up to a second on a sick bus — which
is longer than the 300 ms drive watchdog, so a keystroke would stall the loop
that stops the motor. The console says what it refused and why. Stop the car
(`e`, or let the CONTROL stream lapse) and press again.

### Car-only console keys — `car_fw` on the ST-Link VCP

These ride on the same console and are now listed under the `h` help screen
too — `bms_app` offers `h` to the host handler so a vehicle key cannot go
missing at the moment someone needs it:

| Key | Action |
|---|---|
| `t` `v` | **bench drive** forward / reverse, with no RPi attached — see below |
| `l` | **RPi link statistics** and a verdict on what is wrong with the link |
| `e` | clear a latched emergency stop (button B1 also clears it) |
| `i` | waive the pack-telemetry interlock for this session (asks `y` within 5 s) |
| `m` | motor limit, and the bridge / e-stop / interlock state |
| `+` `-` | motor limit ±5 % |
| `s` | steering: commanded pulse vs. current pulse |
| `=` | steering centre |
| `>` `<` `.` `,` | steering pulse ±10 µs / ±50 µs (calibration) |

**Bench drive (`t` / `v`)** makes the motor testable with no Raspberry Pi
connected. It runs the *same* path a CONTROL packet takes — `Drive_Arm()`, the
ramp, the speed limiter, the same watchdog — so what is tested is what will
run. Two deliberate guards:

- **The first press only arms.** Driving needs a second press within
  `BENCH_ARM_WINDOW_MS`. A console key has no start byte, length or checksum
  behind it, and one stray byte must not be able to command throttle.
- **It stops on its own.** Keys must keep arriving; `BENCH_DRIVE_TIMEOUT_MS`
  (900 ms) opens the bridge after the last one. That is deliberately longer
  than the RPi's 500 ms, because terminal auto-repeat does not start at the
  repeat *rate* — it starts after a 500–660 ms *delay*, and one shared deadline
  made every hold run arm → fail-safe → re-arm.

**The two board buttons are not two user buttons.** `B1` (blue, PC13) is the
only input: press to latch an emergency stop, press again after
`ESTOP_BUTTON_REARM_MS` to clear it. Clearing removes the block only — it never
re-enables the bridge by itself. `B2` (black) is wired to NRST and is a reset;
it cannot be bound to anything.

**Steering limits are calibrated**, measured on the car 2026-08-16:
`750 / 1550 / 2400 µs` (right / centre / left). Travel is 850 µs left and
800 µs right; the interpolation is two-sided, so the asymmetry is handled. If
the linkage changes, re-measure with `=`, `.`, `,` and `s`.

> **If the gauge link dies mid-demonstration, this is the escape hatch.**
> Without fresh pack data the monitor reports `UNKNOWN`; the car keeps driving
> for a 2 s grace period (≈3.5 s from the bus actually dying, including the
> 1.5 s staleness limit) and then drops the bridge and refuses to re-arm.
> Press **`i` then `y`** on the VCP console to waive it for the rest of the
> session — it prints a loud banner and a power cycle puts it back. Building
> with `-DBMS_INTERLOCK_REQUIRE_TELEMETRY=0` does the same permanently, and is
> also what a bench with no gauge attached needs, since `Drive_Arm()` now
> requires a positive `BMS_PACK_OK` and not merely the absence of bad news.
>
> **Neither escape touches `BMS_PACK_BLOCKED`.** That is the protector saying
> it has opened the discharge path, and the bridge drops on it unconditionally
> — hammering a recovering protector is what destroyed the previous board.

`b` and `w` exist because "the gauge does not answer" has three causes that
look identical: no pull-ups, an unpowered gauge, or a wire on the wrong pin.
`b` reads the idle levels twice — floating, then against the MCU's internal
pull-up — which separates "nothing pulls the bus up" from "hard-tied low".
`w` then runs the bus at ~20 kHz off those internal pull-ups: too weak for
real use, but enough to get an ACK out of a working part, which proves the
part is alive before you own a single resistor.

### Readings the firmware refuses to dress up

A gauge with no NV profile, one below its minimum supply, or one with an open
cell tap still returns perfectly well-formed numbers. They are meaningless,
so SOC, capacity, Age, cycles and TTE/TTF are replaced with `--`, the reason
is printed at the top of the frame, and the JSON carries `null` for those
fields plus `trustworthy` / `provisioned` / `supply_ok` / `cells_plausible`.

---

## Provisioning

**Bench monitor only.** `max17320_gui/` is the only project that compiles
`fw/max17320_provision.c`, and it is also the only one that defines
`MAX17320_I_KNOW_THIS_BURNS_NVM=1`. `car_fw/CMakeLists.txt` does not build that
file at all and compiles `BMS_REALTIME_HOST=1`, which removes the `!` menu, so
**nothing in the car can write a byte of the gauge's NVM**. Provision the pack
on the bench, then drive it.

The NVM block takes **7 writes for the life of the part**, one already spent
at Maxim's factory test. The `!` menu runs the free steps first:

| Key | Action |
|---|---|
| `1` | diff the part against the target profile (read-only) |
| `2` | write the profile to **shadow RAM** and verify — volatile, costs nothing, and the gauge computes correctly immediately |
| `3` | how many NVM writes remain |
| `5` | restart the gauge model so it reloads the config it holds now (no NVM write) |
| `4` | commit shadow RAM to NVM — irreversible, spends one |

`4` requires typing `BURN` and refuses below 6 V: the datasheet minimum
supply is 4.2 V, and a flash write under it can land corrupted while still
spending the cycle.

**A virgin die is easy to mistake for a configured one.** On a 2S board with
a 5 mΩ shunt, `nPackCfg = 0x0004` and `nRSense = 0x01F4` are simultaneously
the correct values *and* the factory defaults. `nDesignCap = 0x0000` is the
giveaway, and it is what the firmware tests.

Sequence that works: `2` → `5` → check the capacity reads 11400 mAh → `4`.

### Current limits at 5 mΩ

> **These thresholds live in the pack, not in the car's firmware.** The only
> code that *writes* them — `fw/max17320_provision.c` — is not compiled into
> `car_fw` at all. Flashing the car therefore **cannot change the pack's
> protection by any value**: the MAX17320 enforces whatever is already burned
> into its own NVM, and it keeps enforcing it with the MCU unpowered. If a
> limit needs to change, it changes on the bench with the monitor firmware
> (or the F7 provisioning tool), by burning one of the pack's remaining NVM
> writes — never by rebuilding `car_fw`.
>
> `MAX17320_MAX_CURRENT_LIMITS` is spelled out in
> `max17320_gui/CMakeLists.txt`, but it **defaults to `1` in
> `fw/max17320_config.h`**, so both builds target the max-current profile.
> What it selects is the target table, and the car does read that table — for
> the `n` comparison below, not to write anything.
>
> The `n` NV-config check takes its expected values straight from that target
> table (`max17320_target_config[]`), so a die burned with the max-current
> values **reads back "ok"**, in both builds. It used to carry its own copy of
> the expected values and reported a correctly provisioned die as "differs";
> that copy is gone. A "differs" verdict now means what it says.

`MAX17320_MAX_CURRENT_LIMITS=1` raises four registers, not one — the lowest
ceiling wins:

| Register | Stock profile | Max | Meaning |
|---|---|---|---|
| `nIPrtTh1` | `0x4B80` | `0x7E80` | +10.08 A / −10.24 A slow OCCP/ODCP |
| `nODSCTh` | `0x0C00` | `0x0166` | OCTH **+7.75 A**, SC −20 A, OD −12.5 A |
| `nJEITAC` | `0x644B` | `0xAFFF` | 7.00 A in every temperature zone |
| `nStepChg` | `0xC884` | `0xFF00` | no derating above 4.12 V/cell |

**Discharge reaches −10.24 A; charge is capped at +7.75 A.** OCTH is an
inverted 5-bit code whose maximum is +38.75 mV across the shunt with no
documented way to disable it, so at 5 mΩ that is a hard ceiling — a smaller
shunt is the only way past it (2 mΩ → 19.4 A).

`OCCP` is `0x7E`, not `0x7F`, deliberately: the Current register saturates at
`0x7FFF`, so its upper byte can equal `0x7F` but never exceed it, and the
fault fires only on "exceeds" — `0x7F` would leave slow overcharge protection
permanently unarmed.

Flattening `nJEITAC` buys maximum current at a real cost: **the graceful
temperature derating is gone**. What remains is the hard block at 55 °C.
Watch the pack temperature during a fast charge.

---

## The charging chain

A TPS26750 USB-PD controller negotiates the contract and configures a
BQ25756 charger. Both live on their own EVMs joined by a ribbon.

```
 USB-PD source ──▶ TPS26750 (PD controller, I2C master on I2Cc)
                        │ writes ICHG/IAC_DPM/VAC_DPM/VFB_REG/ITERM
                        ▼
                   BQ25756 (charger) ──▶ pack ──▶ MAX17320 (protector)
```

Four traps, each of which cost real bench time:

1. **The BQ25756 has no non-volatile settings.** Every register returns to
   POR on power-up. `ICHG_REG` POR is **20 A**.
2. **A 40 s watchdog resets `ICHG_REG` back to 20 A**, and
   `EN_CHG_BIT_RESET_BEHAVIOR` defaults to *keep charging* — so expiry does
   not stop the charge, it raises it.
3. **The PD controller rewrites the charger's registers on every contract
   change**, so a value poked in over I2C is transient by design.
4. **The charger's bus does not support multi-controller.** An external
   master on it fights the PD controller.

So the durable answer is not to write the charger. It is either the
**ICHG-pin resistor** — `ICHG_MAX = 50 / R_ICHG` in A and kΩ, so 10 kΩ gives
5 A, 8.25 kΩ gives 6 A, and the charger takes the *lower* of pin and register
— or the PD controller's **own configuration**, set in TI's Application
Customization Tool (Question 15 = Charge Current Limit) and flashed to
EEPROM. TI ships a native Linux x86-64 build of that tool.

With a protector that trips at 7.75 A, the charger must be held below it, or
it starts, trips within microseconds, collapses and retries — an audible
hiccup at the inductor, zero current delivered, and a sticky `OCCP` that
re-arms the instant you clear it.

### Talking to the hardware from Linux

Both adapters are usable without TI's Windows GUI, and both are documented in
`notes/`:

* **TPS26750EVM "Redline TivaLine" adapter** — a CDC serial port speaking an
  ASCII command set. `i2cr <host> <speed_kHz> <addr> <reg> <len>` and
  `i2cw <host> <speed_kHz> <addr> <reg> <bytes…>`. Reads return `[len][data…]`.
  Verified: `i2cr 0 100 0x21 0x03 8` returns `04 41 50 50 20` = `"APP "`,
  the PD controller's MODE register.
* **USB2ANY** — 64-byte HID reports, report ID `0x3F`, packet
  `['T', CRC8, payload_len, type, flags, seq, status, opcode, payload…]`,
  CRC-8 poly 0x07 init 0 over the length byte through the payload. Verified
  against the hardware: a firmware-version read returns `2.8.2.0`.
  Needs a udev rule for non-root access.

---

## VS Code tasks

Every task that builds or programs an image names its target in its own label,
carries its own working directory and names its own `.elf`. There is no
project-neutral "Build" or "Flash" button any more, and no workspace-wide
`cwd` — a single unlabelled Flash button inheriting one was how the bench
monitor could end up on the car.

| Car (`car_fw/`) | Bench monitor (`max17320_gui/`) | Board / host, no image |
|---|---|---|
| `Build car` | `Build monitor` | `Reset` |
| `Clean car` | `Clean monitor` | `Full CHIP Erase` |
| `Clean & Rebuild car` | `Clean & Rebuild monitor` | `Test Decode (host)` |
| `Size car` | `Size monitor` | `Serial Monitor (ST-Link VCP)` |
| `Flash car` | `Flash monitor` | `BMS Dashboard` |
| `Erase & Flash car` | `Erase & Flash monitor` | |
| `CubeMX car` | `CubeMX monitor` | |

The status bar shows the common ones; the rest run from **Run Task…**. Debug
configurations follow the same rule: `Debug car (car_fw, OpenOCD)` and
`Debug monitor (max17320_gui, OpenOCD)`.

`car_fw/.vscode/` carries the same car-only set, so opening `car_fw/` as its
own folder gives buttons that cannot build or flash anything else.

`Test Decode (host)` builds and runs the register-decoding tests on the PC —
no board needed, useful after touching anything in `fw/`.

---

## Notes

Everything below was established from primary sources — datasheets, the
board's own Altium netlist, ST and TI user guides — and each file marks what
is confirmed versus inferred.

| File | Contents |
|---|---|
| `notes/max17320-registers.md` | The register map: addresses, LSBs, bitfields, what must never be written, and the decode traps. |
| `notes/board-hardware.md` | The BMS board rebuilt from its schematic and PCB netlists: connector pinout, absence of pull-ups, high-side FETs, shunt placement. |
| `notes/nucleo-g474re-pinout.md` | NUCLEO-G474RE pinout, solder bridges, the PB8-BOOT0 trap, and (Appendix A) external power, JP5 and E5V. |
| `notes/car-firmware-review.md` | Review of the car firmware: the latch-up path, the drive-safety findings, and §5 "before powering the last spare board". |
| `notes/tps26750-evm.md` | The charging chain: bus topology, addresses, BQ25756 registers, the four traps above, and every path into the hardware. |
| `notes/usb2any-bq.md` | The USB2ANY HID protocol, recovered and verified. |

---

## Shunt

Every current, capacity and power scale derives from the sense resistor. Both
projects set it, and they must agree — `max17320_gui/CMakeLists.txt` and
`car_fw/CMakeLists.txt`:

```cmake
MAX17320_RSENSE_MOHM=5      # R18, MFC0603-R005FT5 on this board
```

This only scales what the firmware *reports*. The gauge's own protection
thresholds are the ones burned into its NVM against `nRSense`; changing this
number does not change them.
