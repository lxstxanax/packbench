# MAX17320 monitor firmware — STM32G474RE

Read-only I2C monitoring of a MAX17320G2 2S4P BMS board, with a live ANSI
dashboard and a JSON stream on the ST-Link Virtual COM Port.

These sources are **shared by two projects** and they are not on the same
pins or built with the same options:

| | Bench monitor | Car |
|---|---|---|
| Project | `../max17320_gui/` | `../car_fw/` |
| Gauge bus | **I2C1 — PA15 (SCL) / PB7 (SDA)**, CN7 | **I2C3 — PC8 (SCL) / PC9 (SDA)**, CN10 |
| Provisioning (`max17320_provision.c`) | compiled in | **not compiled at all** |
| Blocking modes (`!`, `w`, `t`) | present | removed by `BMS_REALTIME_HOST=1` |
| Shares the loop with | nothing | motor, servo, Raspberry Pi link |

The I2C transport comes from the NVM provisioning driver in
`github.com/lxstxanax/stm32f767zi-max17320-bms`. The provisioning half was
later ported here as `max17320_provision.c`, but it is **doubly gated**: it
is only compiled by the bench monitor, and only acts when that project also
defines `MAX17320_I_KNOW_THIS_BURNS_NVM=1`. The part allows 7 lifetime NVM
writes, so the car — which is the build most likely to be reflashed in a
hurry — simply does not contain the code.

## Files

| File | What it does |
|---|---|
| `max17320.h/.c` | I2C transport: probe, word read/write, block read. Datasheet Table 116 addressing (0x36 for 0x000–0x0FF, 0x0B for 0x100–0x1FF). |
| `max17320_monitor.h/.c` | Register map, fixed-point decoding, derived pack state, bit-name tables, and the one permitted write (alert clearing). |
| `max17320_provision.h/.c` | Shadow-RAM config write, verify, and the one NVM commit. **Bench monitor only** — `car_fw` does not build it. |
| `max17320_config.h` | The target pack profile the provisioner writes, including the current-limit registers. Read by `max17320_provision.c` only. |
| `bms_pins.h` | The I2C pins the gauge is on and the names printed for them — the single source of truth, defaulting to the bench monitor's PA15/PB7 and overridden by `car_fw` to PC8/PC9. |
| `bms_io.h/.c` | Blocking console on LPUART1 + non-blocking key input + fixed-point formatting (no `%f`, so no float printf in the image). |
| `bms_dashboard.h/.c` | ANSI dashboard, JSON emitter, raw dump, help. |
| `bms_app.h/.c` | Superloop scheduler: poll 250 ms, render 500 ms, keys every iteration. |
| `test/test_decode.c` | Host test of the decoding math against a fake I2C device. |

Register addresses, LSB sizes and bit meanings come from MAX17320
datasheet Rev 12; the working notes with per-table citations are in
[`../notes/max17320-registers.md`](../notes/max17320-registers.md).

## Hardware

### Bench monitor — `../max17320_gui/`, I2C1 on CN7

| Signal | BMS board P1 | Nucleo G474RE |
|---|---|---|
| SCL | P1.3 | PA15 = **CN7 pin 17** |
| GND | P1.5 (AGND) | **CN7 pin 19** |
| SDA | P1.2 | PB7 = **CN7 pin 21** |
| pull-ups | — | 4.7 k from each line to 3V3 (CN7 pin 16) |

Three consecutive positions in one column of the CN7 morpho header, with
3V3 for the pull-ups on the same connector. The morpho pin names are
printed on the bottom silkscreen — look for `PA15`, `GND`, `PB7`.

### Car — `../car_fw/`, I2C3 on CN10

| Signal | BMS board P1 | Nucleo G474RE |
|---|---|---|
| SCL | P1.3 | PC8 = **CN10 pin 2** |
| SDA | P1.2 | PC9 = **CN10 pin 1** |
| GND | P1.5 (AGND) | **CN10 pin 9** |
| pull-ups | — | 4.7 k from each line to 3V3 (CN7 pin 16 or CN6 pin 4) |

`PC8.Signal=I2C3_SCL` / `PC9.Signal=I2C3_SDA` in
`../car_fw/motor_uart_pwm_nucleo.ioc`, and the same pair is compiled into the
firmware's console messages from `../car_fw/CMakeLists.txt`.

> **Never wire the car's gauge by the bench monitor's table above.** On
> `car_fw`, **PB7 is `USART1_RX` — the Raspberry Pi joystick link** (PB6 is
> `USART1_TX`), and PA15 is not configured at all. Following the bench
> monitor's wiring on the car costs you the command link and gets you no I2C.

Things that will cost you an evening if missed:

1. **The BMS board has no pull-ups.** Net `SDA` is exactly {P1.2, U1.9} and
   `SCL` is exactly {P1.3, U1.8} in both the schematic and the PCB netlist.
   Fit your own.
2. **Keep the bus off PB8/PB9 (`D15`/`D14`), and off CN10 pin 3.** PB8 is
   `PB8-BOOT0`, and with the factory `nSWBOOT0` option byte BOOT0 is sampled
   from the pin. An I2C bus idles high, including through reset, so a pull-up
   there boots the MCU into the system bootloader instead of your firmware —
   and the board's BOOT0 shunt JP7 is not fitted, so nothing holds the pin
   low. Neither project maps I2C there; the trap is a slipped wire, and on
   the car's CN10 corner PB8 (pin 3) is the position next to SDA (pin 1).
3. **The gauge is powered from the pack** (BATTP → 10 Ω → IN). With no
   cells attached it does not answer, which looks exactly like a wiring
   fault.

3.3 V pull-ups are correct and safe: SDA/SCL are rated −0.3 V to **+20 V**
absolute maximum with `VIH(min) = 1.5 V`. Do *not* pull up to AOLDO — it can
be configured to 3.4 V (over VDD+0.3 V for a non-5V-tolerant STM32 pin) and
is only rated for ~2 mA anyway.

The protection FETs are **high side**, so tying Nucleo ground to AGND
bypasses nothing. Ground to **P1.5 only** — never also to the cell-negative
header J3, which would short the 5 mΩ shunt and zero every current reading.

## CubeMX configuration — already done, nothing left to click

### Bench monitor (`../max17320_gui/`)

The project was generated from the board selector (NUCLEO-G474RE,
"initialize all peripherals") and needs **no further CubeMX work**:

- **I2C1** on PA15 (`I2C1_SCL`) and PB7 (`I2C1_SDA`), AF4 — what CubeMX
  picked by default, and a good pick: both land on CN7 next to a GND. GPIO
  pull-up is left at none; the 4.7 k externals do the job, and the ~40 k
  internals in parallel only muddy the levels.
- **Console**: no LPUART1 block is needed. Selecting the board pulls in the
  Nucleo BSP, whose `BSP_COM_Init(COM1, …)` in the generated `main.c`
  already configures `COM1_UART = LPUART1` on PA2/PA3 (AF12) at 115200 8N1
  — exactly the pair wired to the ST-Link VCP. The firmware just borrows
  the BSP's handle, `hcom_uart[COM1]`.
  (Do not "fix" this by enabling `USART1`: the board routes USART1 to the
  Arduino D0/D1 pins PC4/PC5, not to the VCP, and it will look dead.)
- **Clock**: HSI16 → PLL → 170 MHz with boost mode, as generated.
- **FreeRTOS**: off. The scheduler here is `HAL_GetTick()` based.

### Car (`../car_fw/`)

Same board selector, but the pin budget is different because the motor,
servo and Raspberry Pi link came first:

- **I2C3** on PC8 (`I2C3_SCL`) and PC9 (`I2C3_SDA`) — the gauge. PA15/PB7
  were not available: **PB7 is `USART1_RX`** from the Raspberry Pi (PB6 is
  `USART1_TX`).
- **TIM1** CH1/CH2 on PA8/PA9 (RPWM/LPWM), **PA6/PA7** as plain outputs
  (R_EN/L_EN), **TIM2** CH2 on PA1 (steering servo), **TIM4** as the control
  tick.
- **Console**: same as the monitor — the Nucleo BSP's `hcom_uart[COM1]` =
  LPUART1 on the ST-Link VCP, which the motor code does not use.

## How it is wired into the CMake projects

The sources stay outside both CubeMX trees so regeneration cannot clobber
them. Bench monitor, `../max17320_gui/CMakeLists.txt`:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/../fw/max17320.c
    ${CMAKE_SOURCE_DIR}/../fw/max17320_monitor.c
    ${CMAKE_SOURCE_DIR}/../fw/max17320_provision.c   # bench only
    ${CMAKE_SOURCE_DIR}/../fw/bms_io.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_dashboard.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_app.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/../fw
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    MAX17320_RSENSE_MOHM=5
    MAX17320_I_KNOW_THIS_BURNS_NVM=1   # arms the NVM commit
    MAX17320_MAX_CURRENT_LIMITS=1      # what the provisioner would write
)
```

Car, `../car_fw/CMakeLists.txt` — the same list **without**
`max17320_provision.c`, plus:

```cmake
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    BMS_REALTIME_HOST=1                # no blocking modes, budgeted poll
    MAX17320_RSENSE_MOHM=5
    BMS_SCL_PORT=GPIOC   BMS_SCL_PIN=GPIO_PIN_8    # PC8
    BMS_SDA_PORT=GPIOC   BMS_SDA_PIN=GPIO_PIN_9    # PC9
    "BMS_SCL_NAME=\"PC8 (CN10-2)\""
    "BMS_SDA_NAME=\"PC9 (CN10-1)\""
)
```

The pin macros are not cosmetic: they are what the `b` bus check and the
probe-failure banner print. A console that names PB7 on the car would send
somebody to unplug the Raspberry Pi link.

And in `Core/Src/main.c`, inside USER CODE markers so they survive
regeneration (`&hi2c1` on the bench monitor, `&hi2c3` in the car):

```c
/* USER CODE BEGIN Includes */
#include "bms_app.h"
/* USER CODE END Includes */

/* USER CODE BEGIN WHILE */
bms_app_init(&hi2c1, &hcom_uart[COM1]);

while (1)
{
    bms_app_run_once();
/* USER CODE END WHILE */
```

Init goes in `USER CODE BEGIN WHILE`, not `BEGIN 2`, because CubeMX places
`BSP_COM_Init()` *after* the `BEGIN 2` block — initialising the console
from there would hand `bms_io` a UART that does not exist yet.

Build and flash — or use the VS Code tasks, which name their target:
`Build car` / `Flash car`, `Build monitor` / `Flash monitor`.

```sh
cd ../max17320_gui                     # or ../car_fw
cmake --preset Debug
cmake --build --preset Debug
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg \
        -c "program build/Debug/max17320_gui.elf verify reset exit"
        # car: build/Debug/motor_uart_pwm_nucleo.elf
```

If the shunt on the board is ever changed from 5 mΩ, rebuild with
`-DMAX17320_RSENSE_MOHM=<value>` — every current, capacity and power scale
derives from it.

## Using it

Open the VCP at 115200 8N1 (`picocom -b 115200 /dev/ttyACM0`). Keys:

| Key | Action |
|---|---|
| `d` | ANSI dashboard (default) |
| `j` | JSON stream, one object per sample — feeds `../tools/bms_dashboard/` |
| `r` | one-shot raw register dump |
| `n` | NV config check: diff the part against the provisioned profile |
| `x` | read any register — `x0D8` = Cell1, `x1B5` = nPackCfg |
| `b` | bus check: SCL/SDA idle levels, floating and against the MCU's 40 k |
| `w` | life check / weak-bus mode (below) |
| `c` | clear sticky protection alerts (asks first) |
| `p` | re-probe the gauge |
| `!` | provisioning menu (below) |
| `h` | help |

In the car build (`BMS_REALTIME_HOST=1`) the keys that can block the loop
for seconds are compiled out: `!`, `w`, and `t` (trip capture). Everything
else is identical, and the poll is spread a few registers per iteration.

The keys that remain but still do blocking I2C — `n`, `x`, `c`, `b`, `p` —
are gated by `bms_app_set_block_guard()`. A host registers a predicate; when
it returns false the command is refused with the worst-case stall printed.
`car_fw` registers "the bridge is not armed", so those five keys work
normally with the car stopped and are refused while it is driving. No
predicate registered (the bench monitor) = always allowed, unchanged.
The automatic reconnect in `bms_app_run_once()` is deliberately *not* gated:
it still probes every 2 s while the gauge is unreachable, because gating it
would leave the interlock blind for the rest of a trip after one bus glitch.

### `b` and `w` — telling bus faults apart

"The gauge does not answer" has three causes that look identical: no
pull-ups, an unpowered gauge, or a wire on the wrong pin. `b` reads the
idle levels twice, floating and with the MCU's internal ~40 k pull-up:

| floating | +40 k | meaning |
|---|---|---|
| HIGH | HIGH | pull-ups alive, bus idle — nothing is answering, so check the pack |
| LOW | HIGH | nothing pulls the bus up: no resistors, or their rail is dead |
| LOW | LOW | hard-tied low — wrong pin (a GND pin usually) or a short |

`w` then drops the bus to ~20 kHz and runs it off those internal pull-ups.
It is too weak for real use, but enough to get an ACK: if the part answers,
it is alive and the wiring is right. On success the firmware **stays** in
that mode so a bench with no resistors fitted can still measure, and says
so in the dashboard header until you press `w` again.

### Readings the firmware refuses to dress up

A gauge with no NV profile, one below its minimum supply, or one with an
open cell tap still returns well-formed numbers. They are meaningless, so
SOC, capacity, Age, cycles and TTE/TTF are replaced with `--`, the reason
is printed at the top of the frame, and the JSON carries `null` for those
fields plus `trustworthy` / `provisioned` / `supply_ok` / `cells_plausible`.

In 2S, `Cell2 = BATTS − CELL1`. An open top tap makes that negative, and
the register is unsigned, so it lands on exactly `0x0000` — a number that
looks like a measurement and is not one. That case is flagged rather than
rendered as a 4 V cell imbalance.

## Provisioning — the `!` menu

**Bench monitor only.** `max17320_provision.c` is compiled by
`../max17320_gui/` and by nothing else; `../car_fw/` neither builds it nor
keeps the `!` menu. Nothing in the car can write the gauge's NVM.

The NVM block takes **7 writes for the life of the part**, one already
spent at Maxim's factory test. So the menu is ordered so the free,
reversible steps come first:

| Key | Action |
|---|---|
| `1` | diff the part against the target profile (read-only) |
| `2` | write the profile to **shadow RAM** and verify — volatile, costs nothing, and the gauge computes correctly immediately |
| `3` | how many NVM writes remain |
| `4` | commit shadow RAM to NVM — irreversible, spends one |

`4` requires typing `BURN`, and refuses outright below
`MAX17320_MIN_COMMIT_MV` (6 V): the datasheet's minimum supply is 4.2 V,
and a flash write under it can land corrupted while still spending the
cycle.

A virgin die is easy to mistake for a configured one. On a 2S board with a
5 mΩ shunt, `nPackCfg = 0x0004` and `nRSense = 0x01F4` are simultaneously
the correct values *and* the factory defaults. **`nDesignCap = 0x0000` is
the giveaway**, and it is what the firmware tests.

### Current limits at 5 mΩ

> **This is a provisioning-time setting, and the car cannot act on it.** The
> only code that writes these registers is `max17320_provision.c`, which
> `../car_fw/` does not compile; the car also defines `BMS_REALTIME_HOST=1`,
> removing the menu that would have used it. So the car runs on **whatever
> thresholds are already burned into the MAX17320's NVM**, the gauge keeps
> enforcing them with the MCU off, and rebuilding or reflashing `car_fw`
> cannot move them by a single milliamp. Changing them means burning one of
> the pack's remaining NVM writes from the bench monitor (or the F7 tool).
>
> `MAX17320_MAX_CURRENT_LIMITS` defaults to `1` in `max17320_config.h`, so
> **both** builds carry the max-current target table — `max17320_gui`'s
> `CMakeLists.txt` only states it explicitly. The car reads that table for
> one purpose: the `n` comparison.
>
> `n` looks its expected values up in `max17320_target_config[]` rather than
> keeping a second copy, so a die burned with the values below **reads back
> "ok"**. The old duplicate table reported a correctly provisioned die as
> "differs"; it is gone, and a "differs" verdict now means what it says.

`MAX17320_MAX_CURRENT_LIMITS=1` raises four registers, not one — the
lowest ceiling wins:

| Register | Profile | Max | Meaning |
|---|---|---|---|
| `nIPrtTh1` | `0x4B80` | `0x7E80` | +10.08 A / −10.24 A slow OCCP/ODCP |
| `nODSCTh` | `0x0C00` | `0x0166` | OCTH **+7.75 A**, SC −20 A, OD −12.5 A |
| `nJEITAC` | `0x644B` | `0xAFFF` | 7.00 A in every temperature zone |
| `nStepChg` | `0xC884` | `0xFF00` | no derating above 4.12 V/cell |

**Discharge reaches −10.24 A; charge is capped at +7.75 A.** OCTH is an
inverted 5-bit code whose maximum is +38.75 mV across the shunt, with no
documented way to disable it, so at 5 mΩ that is a hard ceiling — a
smaller shunt is the only way past it (2 mΩ → 19.4 A).

`OCCP` is `0x7E`, not `0x7F`, deliberately: the Current register saturates
at `0x7FFF`, so its upper byte can equal `0x7F` but never exceed it, and
the fault fires only on "exceeds" — `0x7F` would leave slow overcharge
protection permanently unarmed.

`c` is the only write the **car** build can perform, and the only non-NVM
write either build can. It follows the datasheet
order (`ProtAlrt` → 0x0000 first, then clear `Status.PA` with a
read-modify-write) and touches no NVM — but it erases the fault history,
which is often the only evidence of what tripped, so it asks for
confirmation and is never automatic.

## Host test

Runs on a PC, no hardware:

```sh
cd fw
cc -std=c11 -Wall -Wextra -I. -Itest -o /tmp/test_decode \
   test/test_decode.c max17320.c max17320_monitor.c
/tmp/test_decode
```

It drives the real `max17320_monitor_poll()` against a fake I2C device
whose raw values decode to exact round numbers (4.2000 V per cell,
+1000.0 mA, 87.5 %, 12.25 cycles, …), so an address typo'd into the wrong
struct field fails the run. `test/stm32g4xx_hal.h` is a stub for the host
build only — the firmware build uses the real HAL.
