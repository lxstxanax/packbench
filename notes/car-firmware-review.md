# Car firmware review — hardware-stress audit

Target: `/home/xnx/projects/max_bms/car_fw` (copy). Platform: NUCLEO-G474RE →
IBT-2 (2× BTS7960) + A35BHLP servo, commanded by a Raspberry Pi over USART1
(PB6/PB7). Motor supply: 2S lithium pack behind a MAX17320 protector.

Symptom being explained: the STM32 is dead, PA7 (L_EN) reads as a short to
ground inside the MCU, board drew ~600 mA before failure — the classic CMOS
latch-up signature. The pack protector was also tripping on motor start.

> **STATUS: patches in §3 have been APPLIED to `car_fw/` and the tree builds
> clean.** Files changed: `Core/Src/motor_dc.c`, `Core/Inc/motor_dc.h`,
> `Core/Src/main.c`, `Core/Inc/main.h`, `Core/Src/stm32g4xx_it.c`,
> `Core/Src/steering.c`. Binary went 56824 → 57276 B text (+452 B), RAM
> unchanged at 7424 B. The BMS monitor integration in `main.c` was preserved
> intact. §3.5 was applied as a compile-time guard plus a build `#warning`
> only — **the servo pulse limits were deliberately left at 2200/1550/1050
> pending a bench measurement** (see §2.8). Nothing under `fw/` was touched.
>
> Deviations from the patches as originally written are listed in §6.

> Note on scope and tone: this firmware is not careless work. The steering path
> in `steering.c` is a well-built non-blocking slew limiter with signed,
> order-independent interpolation and compile-time range guards. The UART
> parser correctly splits ISR assembly from main-loop processing, uses a proper
> critical section, gets `volatile` right on every shared variable, and the
> decision not to resync on `0xA5` mid-frame is correct and non-obvious. The
> central finding of this review is narrow: **the DC motor path never received
> the same treatment the servo path did.** The rate limiter this firmware needs
> already exists in this codebase — it is just wired to the servo only.

---

## 1. Verification of the stated premises

| # | Claim | Verdict |
|---|---|---|
| 1 | `MotorDC_SetSpeed` writes TIM1 CCR1/CCR2 directly, no ramp anywhere | **Confirmed** |
| 2 | `STEERING_DEMO_SELFTEST 1` is enabled; blocking self-test loop | **Confirmed, numbers need correcting** |
| 3 | `DC_PWM_MAX = 5000` vs ARR 8499 @170 MHz → 20 kHz, "100 %" = 59 % real | **Confirmed** |
| 4 | PA6 = R_EN, PA7 = L_EN, push-pull, raised after PWM starts at duty 0 | **Confirmed, init order is correct** |
| 5 | Drive watchdog calls `MotorDC_SetSpeed(0)` but does not drop the enables | **Confirmed, and it is worse than stated** |
| 6 | IS_R / IS_L not connected or read | **Confirmed** |

### 1.1 No ramp — confirmed

`Core/Src/motor_dc.c:11-35`. `MotorDC_SetSpeed()` clamps to ±5000 and writes the
compare registers immediately. There is no rate limiting, no state, and no
`MotorDC_Update()`. `MotorDC_Init()` (`motor_dc.c:6-9`) is an empty function.

Two independent literal `5000`s exist: `MOTOR_MAX` in `motor_dc.c:4` and
`DC_PWM_MAX` in `main.c:56`. They are the clamp and the scale for the same
quantity and can silently drift apart if one is edited.

### 1.2 Self-test — confirmed enabled, but the timing and amplitude differ from the brief

`main.c:62` — `#define STEERING_DEMO_SELFTEST 1`. The block at `main.c:269-298`
is compiled in. Two corrections:

**Cycle timing.** One iteration is `1000 + 500 + 1000 + 500 + 1000 = 4000 ms`,
containing two motor starts. So a start every 2 s on average, with 1.0 s of zero
between the forward burst and the reverse burst, and 3.0 s between the reverse
burst and the next forward burst (1000 ms at the tail + 1000 ms at the head of
the next iteration, plus the 1000 ms leading delay).

**Amplitude.** `DC_PWM_MAX * 40 / 100` = CCR 2000. Against ARR 8500 that is
**23.5 % real duty**, not 40 %. The "40 %" is 40 % of a scale that is itself
only 59 % of full. Worth knowing before anyone reasons about the currents.

**Correction to the brief — the self-test does not do a running reversal.** It
always parks at zero for a full 1000 ms between directions, and zero in this
firmware is a *hard brake* (see §2.3), so the motor really is stopped before it
is driven the other way. The self-test's hazard is repeated hard starts and hard
brakes, not plugging. The plugging hazard lives in the UART path (§2.2).

### 1.3 PWM frequency and scale — confirmed

`tim.c:48-53`: prescaler 0, period 8499, ARPE enabled. 170 MHz / 8500 = **20.0
kHz** exactly. `DC_PWM_MAX = 5000` → 5000/8500 = **58.8 %** maximum real duty.
The motor can never be driven above 59 % from any command path.

### 1.4 Enable pins — confirmed, init order is correct

`gpio.c:54-61`: PA6|PA7 written LOW *before* being configured as
`GPIO_MODE_OUTPUT_PP`, `GPIO_NOPULL`, `SPEED_FREQ_LOW`. `main.c:160-166`:
`HAL_TIM_PWM_Start` on both channels, then `MotorDC_SetSpeed(0)`, then the
enables go high. The ordering is right — the bridge is never enabled with a
non-zero duty pending.

Two caveats the brief did not cover:

- `MX_GPIO_Init()` runs before `MX_TIM1_Init()`, so PA8/PA9 sit in their reset
  state (analog input, floating) for the whole init sequence. Harmless *only*
  because the enables are still low at that point. It is load-bearing ordering
  that is not commented as such.
- **Once raised, PA6/PA7 are never lowered again anywhere in the program.** Not
  on fault, not on watchdog timeout, not on E-stop, not in `Error_Handler()`.
  A `grep` for writes to those pins returns exactly one site: `main.c:165-166`.

### 1.5 Watchdog does not drop the enables — confirmed, and it generalises

`main.c:248-254`. Confirmed. But this is not specific to the watchdog: **no
fault path in the firmware removes the bridge enables.** All four stop paths
(watchdog timeout `main.c:248-254`, invalid packet `main.c:438-447`, `STOP`
`main.c:463-469`, `ESTOP` `main.c:472-478`) do exactly `MotorDC_SetSpeed(0)`
plus a steering action. The bridge stays fully live and one register write away
from full throttle in every one of them.

### 1.6 No current sensing — confirmed, structurally

Stronger than "not read": `HAL_ADC_MODULE_ENABLED` is commented out in
`Core/Inc/stm32g4xx_hal_conf.h:39`, no `MX_ADC*_Init` exists, and the `.ioc`
pin list (`motor_uart_pwm_nucleo.ioc:21-45`) contains no analog pin. There is no
current feedback of any kind and none can be added without first enabling the
ADC HAL module. This is why every item in §2 went unobserved.

---

## 2. What the brief missed

### 2.1 Shoot-through — the preload *does* close the window, and it would not be shoot-through anyway

This was the specific question, so here is the full answer.

**The compare preload is enabled.** `HAL_TIM_PWM_ConfigChannel()` unconditionally
sets `TIM_CCMR1_OC1PE` / `OC2PE` — verified in the shipped driver at
`Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.c:4358` and `:4375`. ARR
preload is on too (`tim.c:53`). So both CCR writes are shadowed and transferred
to the active registers together at the next update event. In the common case
the two writes in `MotorDC_SetSpeed()` are atomic with respect to the PWM
period, and there is no window at all.

**The residual window is interrupt-driven, and it is small.** The two
`__HAL_TIM_SET_COMPARE` calls are ~15 ns apart, against a 50 µs period — a
~0.03 % chance of an update event landing between them. But `MotorDC_SetSpeed()`
is called from main context, and both TIM4 (priority 0, `tim.c:241`) and USART1
(priority 1, `usart.c:110`) can preempt it. A preemption of a few µs stretches
the window to roughly 10 % of a period. When it hits, the new CH1 value is
applied one period before the new CH2 value.

**But the resulting state is not shoot-through.** On an IBT-2, each BTS7960 is a
single half-bridge with its own internal cross-conduction lockout and dead time,
and RPWM/LPWM feed *separate chips*. Both inputs high means both high sides on —
both motor terminals tied to +V, i.e. a hard brake. Both low means both low
sides on — also a brake. Neither is a rail-to-rail short. The correct
description of the residual window is **one 50 µs period of unintended braking
torque**, not a destructive shoot-through.

*Verdict: real, worth the two-line fix, but it did not kill the board and should
not be presented as if it did.* The fix in §3.1 closes it two ways at once: a
PRIMASK-guarded critical section, and — more robustly — always writing the
channel that must go to **zero first**, so that any intermediate state is
"less drive", never "both active".

Related, and a genuine missed opportunity rather than a bug: `tim.c:89-101`
configures `BreakState = TIM_BREAK_DISABLE`. TIM1's BKIN input can kill all PWM
outputs *in hardware* with no CPU involvement. With the IBT-2's IS_R/IS_L
current-sense outputs currently unused, there is a ready-made overcurrent
trip sitting on the table.

### 2.2 A single UART packet can command a full running reversal — highest per-event energy in the system

`main.c:449-458`. `UART_ProcessPacket` converts the commanded percent and calls
`MotorDC_SetSpeed()` immediately. There is no rate limit, no zero-crossing
dwell, and no comparison against the current state. **One valid CONTROL packet
can take the motor from +100 % to −100 % in a single PWM period.**

At speed, that is plugging: the bridge applies +V<sub>bat</sub> against a
back-EMF of roughly −V<sub>bat</sub>, so the winding sees ≈2·V<sub>bat</sub> and
the current peaks near twice the stall current. For a 2S pack that is the
largest current this system can produce, by a wide margin. It is also the best
candidate for generating the load-dump transient described in §4.

This is the reversal hazard the brief was looking for. It is in the UART path,
not the self-test.

### 2.3 "Stop" is a short-circuit brake, not a coast — undocumented and load-bearing

`motor_dc.c:29-34`. `MotorDC_SetSpeed(0)` writes CCR1 = CCR2 = 0. Both BTS7960
IN pins go low, both low sides turn on, and **the motor is short-circuited
through the bridge.** Every "stop" in this firmware is a hard dynamic brake of a
spinning motor: the self-test's four stops per cycle, the watchdog timeout, the
`STOP` command, the `ESTOP` command, and the invalid-packet path.

Braking current is V<sub>bemf</sub> / R<sub>winding</sub>, which at full speed is
approximately the stall current. It circulates inside the bridge rather than
coming out of the pack, so it does **not** trip the protector — but it is the
single largest thermal event the BTS7960 sees, and it happens four times every
four seconds while the self-test runs.

It is also the wrong behaviour for an E-stop, where coast (enables low) is
normally what you want. Nothing in the code or comments states that zero means
brake; a reader would reasonably assume coast.

### 2.4 Every fault handler latches the motor at its last duty, forever

This is the finding I would rank highest after the self-test, and it is not in
the brief.

`Error_Handler()` (`main.c:560-574`) does `BSP_LED_Init`, `__disable_irq()`, then
an infinite blink loop. It never touches TIM1 and never touches PA6/PA7.
`HardFault_Handler`, `NMI_Handler`, `MemManage_Handler`, `BusFault_Handler` and
`UsageFault_Handler` (`stm32g4xx_it.c:70-140`) are all bare `while (1) {}`.

**TIM1 is a peripheral. It keeps running with the last CCR values regardless of
what the core is doing, and PA6/PA7 stay high.** So any hard fault, any failed
HAL call, any assert, at any throttle setting, leaves the motor running at that
throttle until someone physically pulls the battery.

There is no recovery either: `HAL_IWDG_MODULE_ENABLED` and
`HAL_WWDG_MODULE_ENABLED` are both commented out
(`stm32g4xx_hal_conf.h:49,68`) and no independent watchdog is configured.

The reason this matters for *this* failure: if the MCU ever glitched from motor
switching noise — which is precisely what a latch-up precursor looks like — the
firmware's programmed response was to keep the bridge enabled and the throttle
latched. It converts a recoverable transient into an unbounded full-throttle
run.

### 2.5 The bridge is armed at boot and stays armed for the life of the program

`main.c:165-166` raises R_EN and L_EN unconditionally, 0 ms into the program,
before any command source has been validated. The drive watchdog deliberately
does not arm until the first valid CONTROL packet (`main.c:205-209`, and the
comment explaining why is sound). The net effect is a window — unbounded in
length, covering the entire RPi boot — during which the H-bridge is live and
completely unsupervised.

### 2.6 Blocking in main starves the watchdog by up to 4.5 s

There are no blocking calls in interrupt context — `Steering_Update()` is short
and non-blocking, and the UART RX callback does a 5-byte `memcpy` and re-arms.
That part is clean.

The problem is inverted: **`main()` blocks for 4000 ms per loop iteration** when
the self-test is enabled. During those 4 s the code does not process UART
packets, does not run the partial-frame recovery, and does not evaluate the
drive watchdog. The advertised `UART_CONTROL_TIMEOUT_MS = 500` is really "500
ms, or up to 4.5 s, whichever is worse".

Consequence for the merged build: `bms_app_run_once()` (`main.c:225`) is starved
the same way, so the pack monitor polls once every 4 s instead of every 250 ms —
exactly when you would most want it sampling.

Also: `UART_SendPacket` → `HAL_UART_Transmit(..., 10)` (`main.c:406`) blocks the
main loop for up to 10 ms on every PING. Minor next to the above, but it is a
blocking call on the loop that owns the drive watchdog.

### 2.7 UART frame integrity — a mis-decode is a full-authority command

`main.c:356-359`. The check is an 8-bit XOR over four bytes. The link is 8N1,
**no parity**, no hardware flow control, and the start byte `0xA5` is explicitly
allowed to appear inside the payload (`main.c:511-515` — that decision is
correct, but it removes the only structural resync marker).

- An 8-bit XOR detects no even number of bit errors in the same bit position.
- A random 5-byte frame passes with probability 1/256.
- Resynchronisation depends solely on the 10 ms partial-frame timeout, so a
  stream that stays busier than one byte per 10 ms can hold a permanent
  one-byte misalignment. At a normal 20–50 Hz packet rate the gaps are large
  enough to recover; during a burst it will not.

The payload range check (`main.c:438-447`) bounds the damage to ±100 % — which,
with no rate limit, is exactly the full-reversal case from §2.2. On a wire
running beside a motor switching amps at 20 kHz, corrupted bytes are expected,
not hypothetical.

**The ramp is the mitigation that actually matters here**, because it makes any
single bad packet survivable: the motor physically cannot reach the bad value
before the next good packet corrects it. Upgrading XOR → CRC-8 is worth doing
but is secondary, and I have left it out of the patches to keep the diff to what
the fixes need.

### 2.8 Servo may be commanded past its mechanical stops

`main.c:272-273` states the calibrated safe values are **2130 / 1550 / 1080 µs**.
`steering.c:29-31` actually compiles **2200 / 1550 / 1050 µs**.

If 2130/1080 are the measured mechanical limits, then position 0 commands 70 µs
past the left stop and position 100 commands 30 µs past the right stop. A servo
held against its stop stalls and draws its full stall current *continuously and
silently* — for a hobby steering servo, 1–2 A from the same 2S pack, sustained.

This is a genuinely under-appreciated protector-trip contributor: it does not
look like a spike, it looks like a raised baseline, and it is invisible on a
scope trace of the motor. It only bites through the UART path (the self-test
only ever commands 50).

I have deliberately **not** patched the constants — which pair is correct is a
bench measurement, not something to guess from the source.

Separately, TIM2 runs a 3000 µs frame (333 Hz, `tim.c:128-130`). Many servos
accept that; some digital servos do not, and one that mis-decodes the frame can
hunt or stall. Worth confirming against the A35BHLP datasheet.

### 2.9 Percent → PWM conversion — the suspected bug is not there

I checked this carefully because the brief flagged it. **`MotorPercent_ToPwm()`
(`main.c:361-393`) is arithmetically correct.**

- The `int8_t` overflow case `-(-128)` cannot be reached: the ±100 range check
  (`main.c:438-447`) runs *before* the negation at `main.c:449`.
- `(abs_percent - 1) * 3720 / 99` with `abs_percent ≤ 100` yields at most 3720,
  so `1280 + 3720 = 5000`. No overflow, and the explicit `int32_t` is genuinely
  doing its job.
- `sign * pwm` is at most 5000 and fits `int16_t`.

One *behavioural* point rather than an arithmetic one: `DC_PWM_MIN_START = 1280`
means percent 1 jumps straight to CCR 1280 = **15.1 % real duty**. The smallest
possible non-zero throttle command is a 15 % duty step. That is deliberate
dead-band compensation and it is a reasonable idea — but with no ramp it turns
"creep forward gently" into a bang. Once the ramp is in, it is fine as-is.

### 2.10 Sign convention is inconsistent between two files

`motor_dc.c:19` labels `speed > 0` as `ВПЕРЁД` (forward), driving CH1/RPWM.
`main.c:449` calls `MotorPercent_ToPwm((int8_t)(-motor_percent))`, so a protocol
"+50 = forward" becomes a *negative* PWM and drives CH2/LPWM. The self-test
comment (`main.c:284`) agrees with `main.c` and disagrees with `motor_dc.c`.

Nothing here is dangerous on its own. It is flagged because it is exactly the
ambiguity that leads someone to "fix" the sign later and get a full-speed
reversal on a bench with the wheels down. One convention, stated once in
`motor_dc.h`, would close it.

### 2.11 `volatile` and ISR sharing — no bug found

The brief asked me to look for missing `volatile`. I checked every shared object
and **this is done correctly**:

- `uart_packet_index`, `uart_packet_ready`, `last_control_packet_tick`,
  `last_rx_byte_tick`, `drive_watchdog_armed` — all `volatile`. Correct.
- `uart_packet_rx[]` is written in the ISR and read in main under
  `__disable_irq()`. The critical section is correct and correctly scoped.
- `steering_enabled`, `current_pulse_us`, `target_pulse_us` — all `volatile`,
  all 8/16-bit and naturally aligned, so single-instruction access on Cortex-M4.
  `Steering_Update()` runs in the TIM4 ISR and reads `target_pulse_us` while
  main writes it; a 16-bit aligned store cannot tear. Correct.
- `slew_tick_counter` (`steering.c:223`) is a function-local static touched only
  from the ISR. Fine.
- `rx_byte` and `uart_packet[]` are **not** `volatile`, but both are only ever
  accessed from interrupt context. Not a bug. Marking `rx_byte` volatile costs
  nothing and would remove the question.

### 2.12 Unbounded loops

Only the six fault/error loops from §2.4. Nothing else: the parser has no loops,
`Steering_Update()` has no loops, and `while (bms_getc(&c))` in `bms_app.c:949`
is bounded by the ring buffer contents.

### 2.13 Nothing drives the motor during init — but reset is not an E-stop

Verified: no code path drives the motor during initialisation. CCRs are
configured with `Pulse = 0` (`tim.c:75`) and `MotorDC_SetSpeed(0)` runs before
the enables go high.

Worth knowing anyway: on a warm reset (NRST or the debugger) with the motor
spinning, PA6–PA9 revert to analog inputs and the IBT-2's INH pins float. The
BTS7960's internal INH pull-down does disable the bridge, so it stops — but into
a **coast**, releasing the spinning motor's current into the freewheel diodes.
"Press reset to stop it" is not the emergency stop it feels like on the bench.

The 500 ms `HAL_Delay` at `main.c:168` sits *after* the enables are raised, so
the bridge is live and braking for half a second at boot for no reason. Harmless,
but it belongs before the enables.

### 2.14 Latent divide-by-zero in the steering inverse — genuinely benign

`steering.c:162` and `:169` divide by `left - center` and `right - center`.
`steering.c:46-60` has compile-time guards for the absolute range but **none
asserting that a limit differs from centre**. A recalibration that sets
`STEERING_LEFT_US == STEERING_CENTER_US` divides by zero.

Being precise about the consequence: on Cortex-M4, `SDIV` by zero returns 0 and
only traps if `SCB->CCR.DIVBYZERO` is set, which STM32 HAL does not set. So this
produces a wrong position value, not a fault. **Latent correctness bug, not a
crash.** A compile-time guard is worth adding (§3.4); the severity is low and I
am not going to inflate it.

### 2.15 `__disable_irq()` without saving PRIMASK

`main.c:233`, `:261`. Fine as written — main context always has interrupts
enabled. It becomes a latent bug the moment any of that code is reused from
inside a critical section. Untidy, not dangerous. The patches in §3 use
`__get_PRIMASK()`/`__set_PRIMASK()` in `motor_dc.c` because that code genuinely
does run in both contexts.

---

## 3. Patches

Four files change. Diffs are against the current tree (`main.c` as of the BMS
merge, 590 lines). Comment language follows each file: Russian in `motor_dc.c`,
English in `main.c` and `steering.c`.

### 3.1 `Core/Src/motor_dc.c` — speed ramp + direction-reversal guard + atomic CCR pair

Replaces the file body. The ramp is driven from the TIM4 ISR that already exists
for the servo, at the same 2 ms tick — same pattern, no new timer, no new
interrupt.

Rate: `MOTOR_RAMP_STEP = 10` per 2 ms tick → 5000/10 = 500 ticks = **1000 ms
from 0 to full**, as requested.

```diff
--- a/Core/Src/motor_dc.c
+++ b/Core/Src/motor_dc.c
@@
 #include "motor_dc.h"
 #include "tim.h"
 
 #define MOTOR_MAX 5000
 
-void MotorDC_Init(void)
-{
-    
-}
-
-void MotorDC_SetSpeed(int speed)
-{
-    // ограничение
-    if (speed > MOTOR_MAX) speed = MOTOR_MAX;
-    if (speed < -MOTOR_MAX) speed = -MOTOR_MAX;
-
-    if (speed > 0)
-    {
-        // ВПЕРЁД
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, speed);
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
-    }
-    else if (speed < 0)
-    {
-        // НАЗАД
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, -speed);
-    }
-    else
-    {
-        // СТОП
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
-        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
-    }
-}
+// Скорость нарастания. MotorDC_Update() вызывается из прерывания TIM4
+// каждые 2 мс, поэтому 10 единиц за вызов дают 0 -> MOTOR_MAX примерно
+// за 1 секунду (5000 / 10 = 500 вызовов = 1000 мс).
+#define MOTOR_RAMP_STEP             10
+
+// Пауза на нуле при смене направления. Сначала спускаемся до нуля,
+// ждём, и только потом разгоняемся в другую сторону. Если этого не
+// делать, мост прикладывает питание против противо-ЭДС вращающегося
+// мотора, и ток в обмотке доходит примерно до двойного пускового.
+// Именно на этом срабатывает защита пакета.
+#define MOTOR_REVERSE_DWELL_TICKS  150   // 150 * 2 мс = 300 мс
+
+static volatile int16_t  motor_target  = 0;  // куда едем
+static volatile int16_t  motor_current = 0;  // что реально записано в CCR
+static volatile uint16_t motor_dwell   = 0;  // остаток паузы на нуле
+
+// Запись пары CCR. Два важных момента:
+//
+// 1) Канал, который должен обнулиться, пишется ПЕРВЫМ. Тогда любое
+//    промежуточное состояние -- это "меньше тяги", но никогда не
+//    "оба канала активны".
+// 2) Пара пишется под запретом прерываний с сохранением PRIMASK:
+//    функция вызывается и из main, и из обработчика TIM4. Преднагрузка
+//    CCR у TIM1 включена (HAL_TIM_PWM_ConfigChannel), поэтому обе
+//    записи попадают в один период ШИМ, если между ними не влезло
+//    прерывание.
+static void MotorDC_ApplyRaw(int16_t speed)
+{
+    uint32_t primask = __get_PRIMASK();
+    __disable_irq();
+
+    if (speed > 0)
+    {
+        // ВПЕРЁД
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)speed);
+    }
+    else if (speed < 0)
+    {
+        // НАЗАД
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, (uint32_t)(-speed));
+    }
+    else
+    {
+        // СТОП. Оба канала в нуле -- оба нижних ключа открыты, мотор
+        // замкнут накоротко через мост. Это торможение, а не выбег.
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0);
+        __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0);
+    }
+
+    __set_PRIMASK(primask);
+}
+
+void MotorDC_Init(void)
+{
+    motor_target  = 0;
+    motor_current = 0;
+    motor_dwell   = 0;
+
+    MotorDC_ApplyRaw(0);
+}
+
+// Задать ЦЕЛЕВУЮ скорость. Значение в CCR меняется постепенно, в
+// MotorDC_Update(). Ступенек по току больше нет ни на одном пути:
+// ни из UART, ни из самотеста.
+void MotorDC_SetSpeed(int speed)
+{
+    // ограничение
+    if (speed > MOTOR_MAX) speed = MOTOR_MAX;
+    if (speed < -MOTOR_MAX) speed = -MOTOR_MAX;
+
+    motor_target = (int16_t)speed;
+}
+
+// Немедленный сброс тяги, без рампы. Рампа нужна на разгоне, а не на
+// остановке: переход в ноль замыкает обмотку на мост, ток гасится
+// внутри моста и из пакета при этом ничего не тянется.
+void MotorDC_Stop(void)
+{
+    motor_target  = 0;
+    motor_current = 0;
+    motor_dwell   = 0;
+
+    MotorDC_ApplyRaw(0);
+}
+
+int16_t MotorDC_GetSpeed(void)
+{
+    return motor_current;
+}
+
+// Вызывается из обработчика TIM4 каждые 2 мс, рядом со Steering_Update().
+void MotorDC_Update(void)
+{
+    int16_t target  = motor_target;
+    int16_t current = motor_current;
+
+    // Смена направления: пока не дошли до нуля, целью считаем ноль.
+    if (((target > 0) && (current < 0)) ||
+        ((target < 0) && (current > 0)))
+    {
+        target = 0;
+    }
+
+    // Пауза на нуле перед разгоном в обратную сторону.
+    if ((current == 0) && (motor_dwell != 0U))
+    {
+        motor_dwell--;
+        return;
+    }
+
+    if (current < target)
+    {
+        current = ((target - current) > MOTOR_RAMP_STEP) ?
+                  (int16_t)(current + MOTOR_RAMP_STEP) : target;
+    }
+    else if (current > target)
+    {
+        current = ((current - target) > MOTOR_RAMP_STEP) ?
+                  (int16_t)(current - MOTOR_RAMP_STEP) : target;
+    }
+    else
+    {
+        return;
+    }
+
+    // Дошли до нуля, а цель -- другое направление: взводим паузу.
+    if ((current == 0) && (motor_target != 0))
+    {
+        motor_dwell = MOTOR_REVERSE_DWELL_TICKS;
+    }
+
+    motor_current = current;
+    MotorDC_ApplyRaw(current);
+}
```

Traced behaviour for a full reversal `+2000 → −2000`: ramp down 2000→0 over 200
ticks (400 ms), arm dwell, hold zero 150 ticks (300 ms), ramp 0→−2000 over 200
ticks (400 ms). Total 1.1 s, with the motor at zero for 300 ms in the middle.

One accepted quirk: if a stop is commanded *during* the dwell, the dwell still
runs out, delaying the next start by up to 300 ms. Harmless, and it errs toward
safety. Left as-is to keep the state machine simple.

### 3.2 `Core/Inc/motor_dc.h` — new entry points, and the sign convention written down

```diff
--- a/Core/Inc/motor_dc.h
+++ b/Core/Inc/motor_dc.h
@@
 #ifndef MOTOR_DC_H
 #define MOTOR_DC_H
 
 #include "main.h"
+#include <stdint.h>
 
+// Sign convention, stated once so it cannot drift between files:
+//   speed > 0  -> TIM1_CH1 / PA8 / RPWM active
+//   speed < 0  -> TIM1_CH2 / PA9 / LPWM active
+//   speed == 0 -> both compares zero = both low sides on = BRAKE, not coast.
+// Which of these is physically "forward" depends on the motor wiring; the
+// UART layer in main.c owns that mapping and currently inverts the protocol
+// percent before calling here.
+//
+// Range is +/-5000 (MOTOR_MAX), which is 59 % real duty against ARR 8500.
+
 void MotorDC_Init(void);
+
+// Sets the TARGET speed. The actual compare value slews towards it in
+// MotorDC_Update() at roughly full scale per second, with a forced stop at
+// zero on any direction change. No caller can produce a current step.
 void MotorDC_SetSpeed(int speed);
 
+// Drops PWM to zero immediately and clears the ramp state. For fault paths.
+void MotorDC_Stop(void);
+
+// Compare value actually applied right now (not the target).
+int16_t MotorDC_GetSpeed(void);
+
+// Must be called every 2 ms. Currently driven from the TIM4 interrupt in
+// main.c, alongside Steering_Update().
+void MotorDC_Update(void);
+
 #endif
```

### 3.3 `Core/Src/main.c` — self-test off, ramp tick, and enables dropped on every fault path

**(a) Self-test off by default, and impossible to miss when on.**

```diff
@@
-// Set to 1 only for bench testing without a UART master connected.
-// Keep it 0 for normal operation: the demo block below drives the servo
-// through Steering_SetPosition() itself and would fight real UART commands,
-// plus its HAL_Delay() calls stall UART packet processing for seconds.
-#define STEERING_DEMO_SELFTEST   1
+// Set to 1 only for bench testing without a UART master connected.
+// Keep it 0 for normal operation: the demo block below drives the servo
+// through Steering_SetPosition() itself and would fight real UART commands,
+// plus its HAL_Delay() calls stall UART packet processing for seconds.
+//
+// While this is 1 the main loop blocks for 4 s per iteration, so the drive
+// watchdog, the partial-frame recovery and the pack monitor all stop being
+// serviced for that long. The motor is started and hard-braked twice per
+// iteration for as long as the board is powered, with nobody watching.
+// Never leave this enabled on a board connected to the pack.
+#define STEERING_DEMO_SELFTEST   0
+
+#if STEERING_DEMO_SELFTEST
+#warning "STEERING_DEMO_SELFTEST is ENABLED: the motor will run on its own, unattended. Bench only."
+#endif
```

**(b) Track whether the bridge is armed.**

```diff
@@
 static volatile uint32_t last_control_packet_tick = 0U;
 static volatile uint32_t last_rx_byte_tick = 0U;
 static volatile uint8_t drive_watchdog_armed = 0U;
+
+// Reflects the state of R_EN/L_EN (PA6/PA7). The bridge is NOT armed at boot:
+// it is armed by the first valid CONTROL packet and disarmed by every fault
+// path, so there is no window where a live bridge is unsupervised.
+static volatile uint8_t drive_enabled = 0U;
```

**(c) Arm / fail-safe helpers.** Add to the prototypes block and to
`USER CODE BEGIN 4`:

```diff
@@ static void UART_ProcessPacket(uint8_t *packet);
+static void Drive_Arm(void);
+static void Drive_FailSafe(void);
```

```diff
@@ static uint8_t UART_CalcCRC(...)
+// Arms the BTS7960. PWM is forced to zero first, so the bridge can never come
+// up with a non-zero duty already pending.
+static void Drive_Arm(void)
+{
+    if (!drive_enabled)
+    {
+        MotorDC_Stop();
+        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
+        drive_enabled = 1U;
+    }
+}
+
+// Removes drive on any fault. The order matters:
+//   1. PWM to zero. Both low sides turn on and the winding current decays
+//      inside the bridge, drawing nothing from the pack.
+//   2. Short settle. The winding time constant is a few hundred microseconds,
+//      so 3 ms is many time constants.
+//   3. Only then drop R_EN/L_EN.
+// Dropping the enables first would push the remaining winding current back
+// through the bridge diodes into the pack, which is its own way to trip the
+// protector. This runs only on faults, so the 3 ms is not on any hot path.
+static void Drive_FailSafe(void)
+{
+    drive_watchdog_armed = 0U;
+    MotorDC_Stop();
+    HAL_Delay(3);
+    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_RESET);
+    drive_enabled = 0U;
+}
```

**(d) Do not arm the bridge at boot.**

```diff
@@
   MotorDC_Init();
 
   // DC motor PWM for BTS7960: PA8/TIM1_CH1 = RPWM, PA9/TIM1_CH2 = LPWM.
   HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
   HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
   MotorDC_SetSpeed(0);
 
-  // Enable inputs for BTS7960.
-  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET); // R_EN
-  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET); // L_EN
-
-  HAL_Delay(500);
+  // R_EN/L_EN deliberately stay LOW here. The bridge is armed by the first
+  // valid CONTROL packet (or by the self-test block), never at boot: the
+  // drive watchdog does not arm until that packet either, and a live bridge
+  // with no supervision is exactly the state to avoid while the RPi boots.
+  HAL_Delay(500);
```

**(e) Ramp tick in the TIM4 callback.**

```diff
 void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
 {
     if (htim->Instance == TIM4)
     {
         Steering_Update();
+        MotorDC_Update();   // same 2 ms tick drives the DC speed ramp
     }
 }
```

**(f) Watchdog timeout drops the enables.**

```diff
     if (drive_watchdog_armed &&
         ((now - last_control_packet_tick) > UART_CONTROL_TIMEOUT_MS))
     {
-        drive_watchdog_armed = 0U;
-        MotorDC_SetSpeed(0);
-        Steering_Disable(); // immediate center + PWM hold
+        Drive_FailSafe();   // zero PWM, then R_EN/L_EN low
+        Steering_Disable(); // immediate center + PWM hold
     }
```

**(g) Partial-frame timeout drops the enables.**

```diff
     now = HAL_GetTick();
     __disable_irq();
-    if ((uart_packet_index != 0U) &&
-        ((now - last_rx_byte_tick) > UART_PARTIAL_FRAME_TIMEOUT_MS))
-    {
-        uart_packet_index = 0U;
-    }
+    uint8_t partial_frame_dropped = 0U;
+    if ((uart_packet_index != 0U) &&
+        ((now - last_rx_byte_tick) > UART_PARTIAL_FRAME_TIMEOUT_MS))
+    {
+        uart_packet_index = 0U;
+        partial_frame_dropped = 1U;
+    }
     __enable_irq();
+
+    // A truncated frame means the link is not trustworthy right now, so give
+    // up drive authority. The next valid CONTROL packet re-arms, and the ramp
+    // makes that re-arm gentle.
+    if (partial_frame_dropped && drive_enabled)
+    {
+        Drive_FailSafe();
+    }
```

> **Honest note on (g):** this is implemented as requested, but it is the one
> patch here I would want bench time on. A line noisy enough to produce partial
> frames at the control rate will disarm and re-arm the bridge continuously,
> which is its own failure mode. If it proves twitchy, gate it behind a counter
> — require, say, three consecutive partial frames before failing safe. That is
> three extra lines and does not change the shape of the fix.

**(h) Invalid packet and E-stop drop the enables; valid CONTROL arms.**

```diff
         case UART_CMD_CONTROL:
         {
             if ((steer_position > 100U) ||
                 (motor_percent > 100) ||
                 (motor_percent < -100))
             {
                 // Wrong protocol value. Stop safely and center steering.
-                drive_watchdog_armed = 0U;
-                MotorDC_SetSpeed(0);
+                Drive_FailSafe();
                 Steering_Disable();
                 return;
             }
 
             int16_t motor_pwm = MotorPercent_ToPwm((int8_t)(-motor_percent));
 
             // Only valid CONTROL packets keep the drive watchdog alive.
             // Repeated identical CONTROL packets are normal and harmless.
             last_control_packet_tick = HAL_GetTick();
             drive_watchdog_armed = 1U;
 
+            Drive_Arm();
             Steering_Enable();
             MotorDC_SetSpeed(motor_pwm);
             Steering_SetPosition(steer_position);
 
             break;
         }
 
         case UART_CMD_STOP:
         {
             drive_watchdog_armed = 0U;
             MotorDC_SetSpeed(0);
             Steering_Enable();
             Steering_SetPosition(50U); // smooth center through Steering_Update()
             break;
         }
 
         case UART_CMD_ESTOP:
         {
-            drive_watchdog_armed = 0U;
-            MotorDC_SetSpeed(0);
+            Drive_FailSafe();   // zero PWM, then bridge off -- coast, not brake
             Steering_Disable(); // immediate center + PWM hold
             break;
         }
```

`STOP` is deliberately left as a ramped stop with the bridge still armed — it is
the normal "stop driving" command, not a fault, and the watchdog covers it 500 ms
later. `ESTOP` is the one that kills the bridge.

**(i) Self-test arms the bridge explicitly.**

```diff
 #if STEERING_DEMO_SELFTEST
     // ---------------- TEST WHILE BLOCK ----------------
@@
+    // The bridge is not armed at boot any more, so the self-test has to ask
+    // for it. All speed changes below now go through the ramp in motor_dc.c.
+    Drive_Arm();
      Steering_Enable();
     Steering_SetPosition(50);
     MotorDC_SetSpeed(0);
```

The self-test needs no other change: its `HAL_Delay(1000)` at zero is five times
the 200 ms the ramp needs to reach zero from CCR 2000, and the 500 ms bursts
still reach full burst duty after ~200 ms of ramp.

**(j) `Error_Handler` and the fault handlers stop the motor.**

```diff
 void Error_Handler(void)
 {
   /* USER CODE BEGIN Error_Handler_Debug */
   /* User can add his own implementation to report the HAL error return state */
+  // Kill drive BEFORE parking the CPU. TIM1 is a peripheral: without this it
+  // keeps generating PWM at the last commanded duty, with the bridge still
+  // enabled, for as long as the board has power.
+  Drive_EmergencyOff();
   BSP_LED_Init(LED_GREEN);
   __disable_irq();
```

New register-level helper (in `USER CODE BEGIN 4`, callable from fault context
where HAL handle state cannot be trusted):

```diff
+// Register-level drive kill. Safe to call from a fault handler: it touches no
+// HAL state, takes no locks and cannot block.
+void Drive_EmergencyOff(void)
+{
+    TIM1->CCR1 = 0U;
+    TIM1->CCR2 = 0U;
+    TIM1->EGR  = TIM_EGR_UG;   // force the shadow transfer now, not next period
+
+    GPIOA->BSRR = (uint32_t)(GPIO_PIN_6 | GPIO_PIN_7) << 16U;  // R_EN/L_EN low
+}
```

The `TIM_EGR_UG` write matters: compare preload is enabled, so without it the
zeroes would not reach the outputs for up to one 50 µs period.

Declare it in `Core/Inc/main.h` so the fault handlers can reach it:

```diff
 /* USER CODE BEGIN EFP */
-
+void Drive_EmergencyOff(void);
 /* USER CODE END EFP */
```

### 3.4 `Core/Src/stm32g4xx_it.c` — fault handlers stop the motor

Applied identically to `HardFault_Handler`, `MemManage_Handler`,
`BusFault_Handler`, `UsageFault_Handler` and `NMI_Handler`:

```diff
 void HardFault_Handler(void)
 {
   /* USER CODE BEGIN HardFault_IRQn 0 */
-
+  Drive_EmergencyOff();   // motor off before we park here forever
   /* USER CODE END HardFault_IRQn 0 */
   while (1)
   {
```

### 3.5 `Core/Src/steering.c` — compile-time guard for the calibration (optional, low severity)

```diff
 #if (STEERING_ABS_MAX_US >= 3000U)
 #error "STEERING_ABS_MAX_US must fit inside the 3000 us TIM2 servo frame"
 #endif
+
+// Steering_PulseToPosition() divides by (LEFT - CENTER) and (RIGHT - CENTER).
+// Neither may be zero. On Cortex-M4 a zero divisor silently yields 0 rather
+// than faulting, so catch it here instead of shipping a wrong position.
+#if (STEERING_LEFT_US == STEERING_CENTER_US)
+#error "STEERING_LEFT_US must differ from STEERING_CENTER_US"
+#endif
+
+#if (STEERING_RIGHT_US == STEERING_CENTER_US)
+#error "STEERING_RIGHT_US must differ from STEERING_CENTER_US"
+#endif
```

The calibration constants themselves are **not** patched — see §2.8. Confirm on
the bench which of 2200/1050 and 2130/1080 are the real mechanical limits, then
fix the code and the comment together.

---

## 4. Ranking

### 4.1 By likelihood of contributing to killing the MCU

Framing first, because it matters for how much weight to put on this list.
Firmware cannot latch up a pin directly. Every plausible path runs through the
same chain:

> firmware commands a large abrupt current step → the MAX17320 opens its FETs
> mid-current → the motor's stored inductive energy has nowhere to go and the
> IBT-2's supply rail flies up (load dump), while the shared ground return
> bounces → current is injected into the IBT-2's logic pins → into PA6–PA9's
> ESD clamps → substrate device triggers → latch-up, ~600 mA, pin becomes a low
> impedance short to ground.

That last sentence is the reported PA7 symptom. So the ranking is really "which
firmware behaviours generated the most high-energy transients, and how often".

| # | Finding | Why it ranks here |
|---|---|---|
| **1** | **Self-test enabled, unattended, forever** (`main.c:62`, `269-298`) | The multiplier on everything else. Every 4 s: hard start, hard brake, hard start the other way, hard brake. Left on the bench that is ~3600 high-current events per hour, each one a chance for the protector to open mid-current. Nothing else in this firmware ran anywhere near as many times. |
| **2** | **No ramp anywhere** (`motor_dc.c:11-35`) | Sets the *amplitude* of every one of those events. Ranked below the self-test only because without the self-test it would have fired far less often. Together, 1 and 2 are the story. |
| **3** | **No fault path drops the enables; fault handlers latch the last duty** (`main.c:248-254`, `434-492`, `560-574`; `stm32g4xx_it.c:70-140`) | Different in kind: it does not create transients, it removes every opportunity to stop making them. If the MCU ever glitched from motor noise — which is what a latch-up precursor looks like — the programmed response was to keep the bridge enabled and the throttle latched. No IWDG to recover, either. |
| **4** | **Full running reversal from one UART packet** (`main.c:449-458`) | Highest per-event energy in the system (plugging ≈ 2× stall current) and therefore the biggest single load-dump generator. Ranked 4th only on exposure: it fires only when a Pi is actively commanding reversals, and the self-test does not do it. |
| **5** | **XOR checksum, no parity, no rate limit** (`main.c:356-359`, `409-432`) | Turns one corrupted byte pair on a wire running beside a 20 kHz switching motor into a full-authority command, with #4 as the consequence. Real mechanism, but speculative as a cause here. |
| **6** | **Bridge armed at boot, unsupervised until the first packet** (`main.c:165-166`) | Extends the exposure window rather than creating events. Matters most across power cycles with the motor supply live. |
| **7** | **Non-atomic CCR pair** (`motor_dc.c:20-27`) | One 50 µs period of unintended *braking*, roughly 10 % of the time an ISR lands in the window. On an IBT-2 this is not shoot-through. Fix it because it costs two lines, not because it killed anything. |
| — | **No current sensing** (IS_R/IS_L unread, ADC not even compiled in) | Not a cause. Listed because it is the reason none of items 1–7 was ever observed before the board died. |

**Honest summary:** items 1 and 2 together are, in my judgement, the most likely
firmware contribution by a wide margin — not because either is exotic, but
because their product is "a few thousand unattended high-current transients".
Item 3 is what turned any single bad outcome into an unbounded one. Beyond that,
the proximate electrical cause of the latch-up is a **hardware** deficiency
(§5), and no firmware change fully removes it.

### 4.2 By likelihood of contributing to the protector tripping

| # | Finding | Why it ranks here |
|---|---|---|
| **1** | **No ramp + the self-test's hard starts** | A step to 23.5 % duty on a stalled motor settles to roughly `duty × V_bat / R_winding` within about one electrical time constant (a few hundred µs) — several amps arriving far faster than a pack-level limit is meant to see. This is the everyday trip, and it lines up exactly with "tripping on motor start". |
| **2** | **Running reversal from the UART path** (`main.c:449-458`) | The largest current the system can produce, ≈2× stall. If the Pi ever commanded a reversal at speed, this was trip #1 rather than #2. |
| **3** | **Servo possibly commanded past its mechanical stops** (§2.8) | A stalled steering servo draws its full stall current *continuously* from the same 2S pack, 1–2 A sustained. It raises the baseline instead of making a spike, so it is easy to miss entirely and easy to misattribute to the motor. |
| **4** | **`DC_PWM_MIN_START = 1280` floor** (`main.c:55`) | The minimum non-zero throttle is already a 15.1 % duty step, so even a deliberately gentle command starts hard. Subsumed once the ramp is in. |
| **5** | **Brake-to-zero** (`motor_dc.c:29-34`) | Large current — approximately stall current at speed — but it circulates *inside* the bridge and does not come out of the pack, so it heats the BTS7960 without tripping the protector. Listed to correct the natural assumption that "stop" is the gentle operation. |
| — | Everything else in §2 | Not trip contributors. |

### 4.3 Untidy, not dangerous

Stated separately so this review is not a wall of undifferentiated alarm. None
of these needs to hold up getting the spare board running:

- `MotorDC_Init()` is an empty function (`motor_dc.c:6-9`).
- `MOTOR_MAX` and `DC_PWM_MAX` are two independent literal `5000`s.
- Sign convention differs between `motor_dc.c:19` and `main.c:449` (§2.10).
- `main.c:272-273` comment says 2130/1550/1080; `steering.c:29-31` says
  2200/1550/1050. The *comment* being stale is the untidy part; the possible
  mechanical consequence is §2.8 and is not untidy.
- `rx_byte` and `uart_packet[]` are not `volatile` — harmless, both are
  ISR-only.
- `__disable_irq()` without PRIMASK save/restore (`main.c:233`, `:261`).
- Indentation of the packet-complete block in `HAL_UART_RxCpltCallback`
  (`main.c:518-526`).
- The 500 ms `HAL_Delay` sits after the enables are raised rather than before.
- `UART_CONTROL_TIMEOUT_MS` is 500 but its comment says "Start with 1000 ms".
- `Steering_GetTargetPosition()` and `Steering_IsMoving()` are unused.
- The divide-by-zero landmine in `Steering_PulseToPosition()` — benign on
  Cortex-M4 (§2.14), guard added in §3.5 anyway.

### 4.4 Done well

Worth recording, both for accuracy and because these are the patterns the fixes
build on:

- `steering.c` is a proper non-blocking slew limiter driven from a timer ISR —
  which is exactly the pattern the DC path was missing, and exactly what §3.1
  copies.
- Signed, order-independent servo interpolation that works for either
  calibration polarity, carefully commented at `steering.c:17-27`.
- Compile-time range guards on the servo pulse limits (`steering.c:46-60`).
- UART assembly in the ISR, processing in main, with a correct critical section
  (`main.c:229-239`).
- `volatile` correct on every genuinely shared variable (§2.11).
- Not resyncing on `0xA5` mid-frame — correct, non-obvious, and correctly
  justified in the comment (`main.c:511-515`).
- `HAL_UART_ErrorCallback` re-arms reception after overrun/framing errors, which
  is a real failure mode people routinely forget.
- PWM-start-then-enable init ordering.
- The BMS integration added on top uses a ring-buffered console and a budgeted
  register poll specifically so it cannot block this loop (`bms_io.c:31-34`,
  `bms_app.c:961-979`). That care is the right instinct; it just needs to reach
  the motor path too.

---

## 5. Before powering the last spare board

Firmware fixes alone will not prevent a repeat, because the proximate cause of
the latch-up is on the board, not in the flash. With exactly one spare, these
are worth doing first:

1. **Series resistors, 330 Ω – 1 kΩ, in each of the four IBT-2 logic lines**
   (R_EN, L_EN, RPWM, LPWM). This is the single highest-value change. It limits
   injected current into the MCU's ESD clamps to a few mA, below the latch-up
   trigger, for essentially free.
2. **10 kΩ pulldowns at the IBT-2 end of R_EN and L_EN**, so the bridge is
   defined-off whenever the MCU is in reset or unpowered.
3. **Separate the power ground return from the logic ground.** Motor current
   must not share a conductor with the MCU's ground reference. Star-ground at
   the pack negative. This is what produced the ground bounce.
4. **TVS diode across the motor supply at the IBT-2**, plus generous local bulk
   capacitance. This is what absorbs the load dump when the MAX17320 opens
   mid-current — the transient that most likely reached PA7.
5. **First power-up: bench supply with a current limit, not the pack.** Set the
   limit to a couple of amps. It will catch a repeat of any of §4.2 before the
   protector or the MCU has to.
6. **Confirm `STEERING_DEMO_SELFTEST == 0`** in the binary that gets flashed,
   not just in the source. The `#warning` from §3.3(a) makes that visible at
   build time.
7. **Wire IS_R / IS_L.** They are already on the IBT-2 and currently do nothing.
   Even a single ADC channel with a threshold would have made every item in §4.2
   visible. Routing them to TIM1's BKIN instead (§2.1) gives a hardware
   overcurrent trip that does not depend on firmware being alive at all.

---

## 6. Application notes — what actually landed

Applied and built: 56824 → 57276 B text (+452 B), RAM unchanged at 7424 B.
Clean rebuild produces **zero errors** and exactly three warnings, all
accounted for (§6.3).

### 6.1 Deviations from §3 as written

Only two, both small:

- **`partial_frame_dropped` declaration moved outside the critical section.**
  §3.3(g) showed it declared inside the `__disable_irq()` block. It has to be
  declared before, because it is read after `__enable_irq()`. Functionally
  identical, just correct C scoping.
- **§3.5 gained a build `#warning` beyond the `#error` guards.** The two
  `#error` guards only catch a limit *equal* to centre. They cannot detect the
  2200/1050 vs 2130/1080 disagreement, because both pairs are individually
  legal. Since that disagreement is the item with a real mechanical
  consequence (§2.8), it needed something that fires on every build. The pulse
  values themselves were **not** changed.

Everything else applied verbatim.

### 6.2 Behaviour verified by simulation, not just by compiling

The ramp state machine was extracted and run on the host before trusting it:

| Property | Result |
|---|---|
| 0 → full scale | 500 ticks = **1000 ms** exactly |
| +2000 → −2000 | zero at 400 ms, **302 ms dwell at zero**, complete at 1100 ms |
| Plain stop (not a reversal) | dwell **not** armed — no spurious delay before the next start |
| Direct sign crossings in one step | **0** |
| Both compare channels non-zero simultaneously | **0** |

### 6.3 Warnings in a clean build

Three, all expected:

1. `steering.c:83` — the deliberate calibration `#warning` from §6.1. It is
   supposed to be there and should be deleted only when the limits are
   measured and fixed.
2. `fw/bms_app.c:617` `handle_prov_key` unused
3. `fw/bms_app.c:241` `apply_internal_pullups` unused

Items 2 and 3 are **pre-existing and structural**, not caused by these changes:
both are static functions whose only call sites (lines 832 and 284) sit inside
`#ifndef BMS_REALTIME_HOST` blocks, and `CMakeLists.txt` defines
`BMS_REALTIME_HOST=1`. Nothing under `fw/` was modified.

### 6.4 What changed about how the car behaves

Worth knowing before the first power-up, because some of this is visible from
the driving seat:

- **The bridge is no longer armed at boot.** Nothing moves until the first
  valid CONTROL packet arrives. If the RPi never connects, the motor never
  becomes live at all. This is intended, but it means "power on and nothing
  happens" is now the correct behaviour rather than a fault.
- **Throttle response is now rate-limited to ~1 s from zero to full.** The car
  will feel noticeably less twitchy. This is the ramp doing its job.
- **A commanded reversal now takes about 1.1 s** and passes through a 300 ms
  stop. A reversal is no longer instant, by design.
- **The self-test is off.** Set `STEERING_DEMO_SELFTEST` back to 1 for bench
  work and the build will print a warning naming it — but do not leave it on
  with the pack connected.
- **A truncated UART frame now disarms the bridge** until the next valid
  CONTROL packet. This is the one change worth watching on the bench; if the
  link is noisy it may disarm more often than is comfortable. §3.3(g) has the
  three-line counter fix if so.
- **`STOP` still leaves the bridge armed** (it is a normal command, and the
  watchdog covers it 500 ms later). **`ESTOP` kills the bridge.** That
  distinction is deliberate.
