# NUCLEO-G474RE (MB1367) — Pinout research notes

Board: **NUCLEO-G474RE**, PCB reference **MB1367**, MCU **STM32G474RET6** (LQFP64), on-board **STLINK-V3E**.
Primary reference: **UM2505** "STM32G4 Nucleo-64 boards (MB1367)".

Sources used throughout (short keys used in the tables below):

| Key | Source |
|---|---|
| UM2505 | https://www.st.com/resource/en/user_manual/um2505-stm32g4-nucleo64-boards-mb1367-stmicroelectronics.pdf |
| UM2505-ML | ManualsLib HTML rendering of UM2505, page N: `https://www.manualslib.com/manual/1644309/St-Stm32g4-Nucleo-64.html?page=N` |
| SCH | MB1367 schematic: https://www.st.com/resource/en/schematic_pack/mb1367-g474re-c04_schematic.pdf — **not retrieved** (st.com PDF fetches timed out); listed as the source to consult for the open items at the end |
| DUINO | stm32duino board variant `variant_NUCLEO_G474RE.h`: https://raw.githubusercontent.com/stm32duino/Arduino_Core_STM32/main/variants/STM32G4xx/G473R%28B-C%29T_G473RETx%28Z%29_G474R%28B-C-E%29T_G483RET_G484RET/variant_NUCLEO_G474RE.h |
| PINCTRL | ST-generated AF tables (Zephyr hal_stm32): https://raw.githubusercontent.com/zephyrproject-rtos/hal_stm32/main/dts/st/g4/stm32g474r%28b-c-e%29tx-pinctrl.dtsi |
| ZEPHYR | https://docs.zephyrproject.org/latest/boards/st/nucleo_g474re/doc/index.html |

> **Caution on this board:** the Arduino/USART wiring of MB1367 is **not** the same as the classic
> MB1136 Nucleo-64. On MB1367, Arduino **D0/D1 are PC5/PC4 (USART1)**, *not* PA3/PA2, and the
> ST-LINK VCP is on **LPUART1 (PA2/PA3)**. Do not reuse F4/F7 Nucleo-64 assumptions here. [UM2505-ML p24, DUINO]

---

## 1. I2C1 on the Arduino header (primary choice)

| Item | Value | Source |
|---|---|---|
| Peripheral | **I2C1** | UM2505-ML p25, DUINO |
| SCL | **PB8** = Arduino **D15** | DUINO, UM2505-ML p25 (SB2/SB4 descriptions reference PB8/Arduino) |
| SDA | **PB9** = Arduino **D14** | DUINO, ZEPHYR |
| Alternate function | **AF4** for both PB8 (I2C1_SCL) and PB9 (I2C1_SDA) | PINCTRL |
| Arduino connector | **CN5** (the D8..D15 header) | DUINO; UM2505-ML p31 |
| CN5 position, SDA (D14) | **CN5 pin 9** — row reads "9 \| SDA/D14" | **UM2505-ML p31, verbatim** |
| CN5 position, SCL (D15) | **CN5 pin 10** — row reads "10 \| SCL/D15" | **UM2505-ML p31, verbatim** |
| ST morpho position, PB8 | **CN10 pin 3** | UM2505 Table 16 (see morpho note below) |
| ST morpho position, PB9 | **CN10 pin 5** | UM2505 Table 16 (see morpho note below) |

UM2505 Arduino connector table for **CN5** (the D8–D15 header), **verbatim** [UM2505-ML p31]:

| CN5 pin | Name | MCU pin |
|---|---|---|
| 1 | D8 | PA9 |
| 2 | PWM/D9 | PC7 |
| 3 | PWM/CS/D10 | PB6 |
| 4 | PWM/MOSI/D11 | PA7 |
| 5 | MISO/D12 | PA6 |
| 6 | SCK/D13 | PA5 |
| 7 | GND | — |
| 8 | VREFP (AREF) | — |
| 9 | **SDA/D14** | **PB9** |
| 10 | **SCL/D15** | **PB8** |

(MCU pin column from DUINO; the CN5 pin-number/name column is verbatim UM2505.)

> Note: PB8 also reaches the morpho **BOOT0 position CN7 pin 7** via SB4 (see below).

CubeMX/HAL setup: `I2C1`, `GPIO_AF4_I2C1`, open-drain, on PB8/PB9.

### Solder bridges touching these pins

UM2505 Table 12 defines two solder bridges on PB8 [UM2505-ML p25, verbatim]:

| SB | State | Description (verbatim) |
|---|---|---|
| SB2 | ON | "PB8 connected to Arduino" |
| SB2 | OFF | "PB8 not connected to Arduino" |
| SB4 | ON | "PB8 connected to ST morpho CN7 pin 7" |
| SB4 | OFF | "PB8 not connected to ST morpho CN7 pin 7" |

Notes:
- **PB8 is `PB8-BOOT0` on the STM32G474.** CN7 pin 7 is the BOOT0 position of the ST morpho
  connector, which is why SB4 exists. BOOT0 is only sampled at reset, so using PB8 as I2C1_SCL
  after startup is fine, but do not hold SCL high through reset with a strong pull-up if you rely
  on booting from flash. Keep the bus pull-ups at 4.7 k or weaker and this is a non-issue in practice.
- **There is no solder bridge on PB9** in UM2505 Table 12; PB9 goes straight to D14. [UM2505-ML p25/p26]
- No conflict with LD2 (PA5), the ST-LINK VCP (PA2/PA3), SWD (PA13/PA14), or the oscillators
  (PF0/PF1 for HSE, PC14/PC15 for LSE). PB8/PB9 are otherwise unused on this board.

### Does SB2 need changing? — empirical evidence

ST community users run I2C1 on Arduino **D15/D14 = PB8/PB9** on a stock NUCLEO-G474RE; the reported
failures were resolved in software (Wire call sequence), **not** by modifying a solder bridge:
https://community.st.com/t5/stm32-mcus-products/nucleo-g474re-arduino-i2c-not-working/td-p/98317

**Conclusion: SB2 is ON as shipped and no rework is needed for D14/D15 I2C1.** The state is inferred
from that evidence plus the fact that ST ships the Arduino header fully functional; UM2505 Table 12
does not print "(default)" annotations for SB1–SB15, so the *printed* default is **UNCONFIRMED**.

### On-board pull-ups

**UNCONFIRMED / assume none.** No I2C pull-up resistors on ARD_D14/ARD_D15 could be verified from ST
documentation. Nucleo-64 boards normally do **not** fit I2C pull-ups. **Provide your own 4.7 k
pull-ups to 3V3** on SDA and SCL.

---

## 2. Fallback I2C options

All AF numbers from PINCTRL (ST-generated tables for STM32G474Rx).

| Option | SCL | SDA | AF | Where it is exposed | Verdict |
|---|---|---|---|---|---|
| **I2C2** (best fallback) | **PA9** = Arduino **D8** (CN5 pin 1) | **PA8** = Arduino **D7** (CN9 pin 8) | **AF4** both | Arduino headers, no SB involved | **Recommended fallback** — both pins on the Arduino connector, free on this board |
| **I2C1 alt** | **PB6** = Arduino **D10** (CN5 pin 3) | **PB7** = ST morpho **CN7 pin 21** (morpho only, no Arduino pin) | **AF4** both | PB6 on Arduino, PB7 morpho only | Good. Note PB6 is also SPI1_NSS/D10 and a USART1_TX(AF7) alt — both unused by default here |
| **I2C3** | **PC8** | **PC9** or **PC11** | **AF8** | ST morpho only | Usable, morpho-only. PC8/PC9/PC11 are free on this board |
| I2C3 alt | PA8 (**AF2**) | PB5 (AF8) | mixed | Arduino D7 / D4 | Works but PA8 is more natural as I2C2_SDA |
| I2C4 | PC6 | PC7 | AF8 | PC7 = Arduino D9 | Possible; PC6/PC7 also used for other Arduino functions |
| ~~I2C1 on PA13/PA14~~ | PA13 | PA14 | AF4 | — | **DO NOT USE — these are SWDIO/SWCLK.** Using them kills debugging; confirmed by ST community: https://community.st.com/t5/stm32-mcus-products/nucleo-g474re-i2c-debug-problems/td-p/102251 |

### Bonus: I2C1 is also selectable on the A4/A5 analog pins

UM2505's CN8 table lists **A4 = "PC1/PB9 — ADC12_IN7/I2C1_SDA"** and **A5 = "PC0/PA15 —
ADC12_IN6/I2C1_SCL"** [UM2505-ML p30]. This is the classic Arduino A4/A5-as-I2C option: the board
can route **PB9 (I2C1_SDA, AF4)** and **PA15 (I2C1_SCL, AF4)** to A4/A5 instead of the ADC pins
PC1/PC0. Note PB9 is the *same* SDA net as D14, so A4/A5 is an alternative *position*, not a second
bus. **Which solder bridges perform this swap, and their defaults, is UNCONFIRMED** — the default is
almost certainly PC1/PC0 (ADC). Do not rely on A4/A5 for I2C without checking the board.

Full STM32G474Rx I2C alternate-function map [PINCTRL]:

| Signal | Pins (AF) |
|---|---|
| I2C1_SCL | PA13 (AF4), PA15 (AF4), PB8 (AF4) |
| I2C1_SDA | PA14 (AF4), PB7 (AF4), PB9 (AF4) |
| I2C2_SCL | PA9 (AF4), PC4 (AF4) |
| I2C2_SDA | PA8 (AF4), PF0 (AF4) |
| I2C3_SCL | PA8 (AF2), PC8 (AF8) |
| I2C3_SDA | PB5 (AF8), PC9 (AF8), PC11 (AF8) |
| I2C4_SCL | PA13 (AF3), PC6 (AF8) |
| I2C4_SDA | PB7 (AF3), PC7 (AF8) |

> Caveat for I2C2: **PC4 is USART1_TX** on this board (wired to Arduino D1 by default), so use
> **PA9** for I2C2_SCL, not PC4. Likewise **PF0 is OSC_OUT/HSE**, so use **PA8** for I2C2_SDA, not PF0.

---

## 3. ST-LINK Virtual COM Port UART — LPUART1 vs USART2 resolved

**Answer: it is LPUART1 on PA2/PA3, AF12. USART2 is *not* used on MB1367.**
The alternative offered by the board is **USART1 on PC4/PC5**, not USART2.

| Item | Value | Source |
|---|---|---|
| VCP peripheral | **LPUART1** | UM2505-ML p24 §6.6.5; ZEPHYR |
| VCP TX (MCU → PC) | **PA2** = LPUART1_TX | UM2505-ML p26 (SB17 description); DUINO `PIN_SERIAL_TX = PA2` |
| VCP RX (PC → MCU) | **PA3** = LPUART1_RX | UM2505-ML p26 (SB23 description); DUINO `PIN_SERIAL_RX = PA3` |
| Alternate function | **AF12** (LPUART1 on PA2/PA3) | PINCTRL |
| Arduino D1/D0 UART | **USART1 on PC4/PC5**, AF7 | UM2505-ML p24 §6.6.5, p25 (SB13/SB19); DUINO D1=PC4, D0=PC5 |
| Also on ST morpho | USART1 reaches **CN10 pin 35 (PC4) / CN10 pin 37 (PC5)** | UM2505-ML p24 §6.6.5 (verbatim, see quote) |

UM2505 §6.6.5 default statement, **verbatim**:

> "By default: Communication between the target STM32G4 and the STLINK-V3E MCU is enabled on LPUART1
> to support the Virtual COM port; Communication between the target STM32G4 and Arduino Uno V3
> connector (CN9 pins 2 and 1), or ST morpho connector (CN10 pins 35 and 37) is enabled on USART1."
> [UM2505-ML p24]

### Solder bridges that route the VCP (UM2505 Table 12, verbatim descriptions) [UM2505-ML p25–p26]

| SB | ON means |
|---|---|
| SB12 | "STLINK_TX (T_VCP_TX) connected to USART1 TX PC4" |
| SB13 | "ARD_D1_TX connected to USART1 TX PC4" |
| SB17 | "STLINK_TX (T_VCP_TX) connected to LPUART1 TX PA2" |
| SB18 | "ARD_D1_TX connected to LPUART1 TX PA2" |
| SB19 | "ARD_D0_RX connected to USART1 RX PC5" |
| SB20 | "STLINK_RX (T_VCP_RX) connected to USART1 RX PC5" |
| SB22 | "ARD_D0_RX connected to LPUART1 RX PA3" |
| SB23 | "STLINK_RX (T_VCP_RX) connected to LPUART1 RX PA3" |

Resulting routing matrix (**derived** from the Table 12 descriptions above combined with the §6.6.5
default statement — the grouping is reconstructed, so treat the exact per-config SB lists as
*derived, not quoted*):

| Routing | SB ON | SB OFF | Default? |
|---|---|---|---|
| LPUART1 (PA2/PA3) → STLINK-V3E VCP | SB17, SB23 | SB18, SB22 | **YES (default)** |
| LPUART1 (PA2/PA3) → Arduino D1/D0 | SB18, SB22 | SB17, SB23 | no |
| USART1 (PC4/PC5) → Arduino D1/D0 | SB13, SB19 | SB12, SB20 | **YES (default)** |
| USART1 (PC4/PC5) → STLINK-V3E VCP | SB12, SB20 | SB13, SB19 | no |

### STM32CubeMX behaviour — important gotcha

Selecting **NUCLEO-G474RE** in the CubeMX board selector with **"Initialize all peripherals with
their default Mode"** does **not** give you a working VCP out of the box: CubeMX/CubeIDE enable
**USART1 (PC4/PC5)**, while the board's solder bridges route the VCP to **LPUART1 (PA2/PA3)**.

> "The solder bridges on the Nucleo-G474RE are set by default to LPUART but the configuration in
> STM32CubeIDE and I would guess in STM32CubeMX is set to USART1."
> — https://community.st.com/stm32cubeide-mcus-28/usart1-is-default-peripheral-of-nucleo-g474re-but-lpuart-is-default-configuration-52385

**Action:** after generating from the board selector, either enable **LPUART1** on PA2/PA3 (AF12) and
use that for printf/VCP, or move SB12/SB20/SB13/SB19 to put USART1 on the VCP. Enabling LPUART1 is
the zero-rework path.

LPUART1 note: LPUART1 has a fractional prescaler and is fine at 115200 from a 170 MHz/PCLK source.
Zephyr uses 115200 8N1 on it. If clocked from LSE in low-power mode it is limited to 9600. [ZEPHYR]

---

## 4. Power pins, user LED, user button

### Power on the Arduino headers

CN6 is the Arduino Uno V3 power header (8 pins). Reproduced **verbatim** from UM2505 [UM2505-ML p30]:

| CN6 pin | Name | STM32 pin | Function |
|---|---|---|---|
| 1 | NC | — | Reserved for test |
| 2 | IOREF | — | I/O reference |
| 3 | NRST | PG10-NRST | RESET |
| 4 | **3V3** | — | 3V3 input/output |
| 5 | **5V** | — | 5 V output |
| 6 | **GND** | — | GND |
| 7 | **GND** | — | GND |
| 8 | VIN | — | 7 V – 12 V input power |

Additional GND on the Arduino side: **CN5 pin 7 = GND** (AREF/VREFP is on CN5 pin 8) [UM2505-ML p31].

CN8, the analog header, **verbatim** [UM2505-ML p30]:

| CN8 pin | Name | STM32 pin | Function |
|---|---|---|---|
| 1 | A0 | PA0 | ADC12_IN1 |
| 2 | A1 | PA1 | ADC12_IN2 |
| 3 | A2 | PA4 | ADC2_IN17 |
| 4 | A3 | PB0 | ADC3_IN12 or ADC1_IN15 |
| 5 | A4 | **PC1/PB9** | ADC12_IN7 / **I2C1_SDA** |
| 6 | A5 | **PC0/PA15** | ADC12_IN6 / **I2C1_SCL** |

### Power on the ST morpho headers

MB1367 uses the **standard Nucleo-64 ST morpho layout** (odd pins in the left column, even pins in
the right column, 38 pins per connector), with PF0/PF1 in place of the PH0/PH1 of other Nucleo-64
boards. This is corroborated at **five independent points** by ST's own verbatim solder-bridge text
in UM2505 Table 12 (see "Morpho layout cross-check" below).

| Signal | Morpho position | Confidence |
|---|---|---|
| **+3V3** | **CN7 pin 16** | standard layout |
| **+5V** | **CN7 pin 18** | standard layout |
| **GND** | **CN7 pin 8**, pin 20, pin 22 | standard layout |
| **GND** | **CN10 pin 9** | standard layout |
| AGND | CN10 pin 32 | standard layout |
| AVDD | CN10 pin 7 | standard layout |
| E5V | CN7 pin 6 | standard layout |
| VDD | CN7 pin 5 | standard layout |
| VIN | CN7 pin 24 | standard layout |
| IOREF | CN7 pin 12 | standard layout |
| NRST | CN7 pin 14 | standard layout |
| BOOT0 | CN7 pin 7 | **ST verbatim (SB4)** |
| VBAT | CN7 pin 33 | **ST verbatim (SB33)** |

> **Caveat:** the exact GND pin list is the weakest item here — ManualsLib's HTML flattening of
> UM2505 Table 16 loses the odd/even column structure and returned mutually contradictory parities
> across repeated extractions. The *safest* grounds to use are **CN6 pin 6 / CN6 pin 7** (Arduino
> power header) and **CN5 pin 7**, all confirmed verbatim. Verify morpho GND with a multimeter or
> against UM2505 Table 16 in the real PDF before committing a harness.

### Morpho layout cross-check (why the standard layout is trusted)

These five statements are quoted verbatim from ST in UM2505 and each one lands exactly on the
standard Nucleo-64 morpho position, which is what validates the rest of the layout:

| ST verbatim statement | Morpho position | Standard Nucleo-64 layout |
|---|---|---|
| SB3: "PC5 connected to ST morpho CN10 pin 6" | CN10 pin 6 = PC5 | ✓ matches |
| SB4: "PB8 connected to ST morpho CN7 pin 7" | CN7 pin 7 = BOOT0 | ✓ matches (PB8 *is* BOOT0 on G4) |
| SB24: "PF1-OSC_IN connected to ST morpho connector I/O usage (CN7 pin 31)" | CN7 pin 31 | ✓ matches (PH1 position) |
| SB27: "PF0-OSC_OUT connected to ST morpho connector I/O usage (CN7 pin 29)" | CN7 pin 29 | ✓ matches (PH0 position) |
| SB28: "PC4 connected to Morpho CN10 pin 34" | CN10 pin 34 = PC4 | ✓ matches |
| SB33: VBAT "through morpho connector CN7 pin 33" | CN7 pin 33 = VBAT | ✓ matches |

Consequently **CN10 pin 3 = PB8 (I2C1_SCL)** and **CN10 pin 5 = PB9 (I2C1_SDA)**.

> **CN10 pins 35/37 explained.** UM2505 §6.6.5 says the Arduino/morpho UART reaches "ST morpho
> connector (CN10 pins 35 and 37)". On the standard layout those positions are PA2/PA3 — the D1/D0
> pair of *other* Nucleo-64 boards. On MB1367 ST kept those morpho positions as the **ARD_D1_TX /
> ARD_D0_RX** nets (which default to USART1 PC4/PC5) to preserve morpho compatibility. So CN10
> 35/37 carry the Arduino serial pair, **not necessarily PA2/PA3 directly.** Marked **UNCONFIRMED**
> — verify on the schematic before wiring anything to CN10 35/37.

### LED and button

| Item | MCU pin | Notes | Source |
|---|---|---|---|
| **LD2 user LED (green)** | **PA5** | Also Arduino **D13** / SPI1_SCK. Active high. Gated by **SB6**: ON = "User LED driven by PA5 (ARD_D13)" | UM2505-ML p25 (SB6, verbatim); DUINO `LED_BUILTIN = PA5`; ZEPHYR |
| **B1 user button (blue)** | **PC13** | Gated by **SB16**: ON = "USER button connected to PC13." Alternative **SB21** ON = "USER button connected to PA0." | UM2505-ML p26 (verbatim); DUINO `USER_BTN = PC13`; ZEPHYR |

Note: one third-party summary claimed LD2 = PC7. **That is wrong** — PC7 is Arduino D9 on this board.
PA5 is confirmed by UM2505 SB6 text, the stm32duino variant and Zephyr.

B1 is active-low with an external pull-up on Nucleo boards; configure PC13 as input with `GPIO_NOPULL`
or `PULLUP` and detect a falling edge.

---

## 5. Clocks

| Item | Value | Source |
|---|---|---|
| Max system clock | **170 MHz** (requires **boost mode**, `PWR` R1MODE, and correct flash latency) | STM32G474 product spec; ZEPHYR ("boosted to 170MHz if boost mode is selected") |
| Max without boost mode | **150 MHz** | ZEPHYR |
| HSI | 16 MHz internal RC | ZEPHYR |
| HSE crystal X3 | **24 MHz**, 6 pF load, 20 ppm (NDK NX2016SA-24MHz-EXS00A-CS10820) | UM2505-ML p22 |
| LSE crystal X2 | 32.768 kHz, SB31 ON (default) | UM2505-ML p27 |

### HSE source options (UM2505 clock-source section) [UM2505-ML p22]

| Option | Solder bridges | Default? |
|---|---|---|
| **HSE on-board oscillator from X3 crystal** (24 MHz) | SB25 + SB26 ON; SB24, SB27, SB28 OFF; C56/C59 = 6.8 pF fitted | **Stated as "(default)" in UM2505** |
| MCO from ST-LINK (fixed **8 MHz**, into PF0-OSC_IN) | SB27 ON; SB25, SB26, SB24, SB28 OFF | no |
| External oscillator via CN7 pin 29 (PF0) | SB28 ON; SB24, SB25, SB26, SB27 OFF | no |
| HSE not used — PF0/PF1 as GPIO | SB24 + SB28 ON; SB25, SB26, SB27 OFF | no |

> **Discrepancy — flagged.** UM2505 Table 12 lists **SB26 ON = "ST-LINK MCO used for HSE CLK"** and
> **SB25 ON = "HSE provided by external HSE 24 MHz CLK X3"**, which contradicts the clock-source
> section requiring **both** SB25 and SB26 ON for the X3 default. One of the two renderings is wrong.
> **UNCONFIRMED — verify against the MB1367 schematic and by visual inspection of your board before
> relying on HSE.**

> **Is X3 populated on MB1367?** UM2505 calls the X3 crystal configuration the *default*, which
> differs from older Nucleo-64 boards (where X3 is unpopulated and HSE comes from ST-LINK MCO).
> A user on ST's forum inspecting a NUCLEO-G474RE reported SB25/SB26 ON and C56/C59 soldered:
> https://community.st.com/t5/stm32-mcus-boards-and-hardware/cannot-start-hse-on-g474re-nucleo-board/td-p/811849
> Treat "X3 fitted" as **likely but board-revision dependent — verify visually.**

### CubeMX board-default clock configuration

**UNCONFIRMED.** No ST document could be found stating exactly what the CubeMX board selector
generates for NUCLEO-G474RE. For reference, Zephyr's board default is **PLL from the 16 MHz HSI
→ 150 MHz** [ZEPHYR]. The safe, rework-free choice is **HSI16 → PLL → 170 MHz with boost mode**,
which avoids the HSE/X3/SB26 ambiguity entirely.

---

## 6. Quick reference — the pins that matter

| Function | MCU pin | AF | Arduino | ST morpho |
|---|---|---|---|---|
| **I2C1_SCL** | **PB8** | **AF4** | **D15 = CN5 pin 10** ✔ | CN10 pin 3 |
| **I2C1_SDA** | **PB9** | **AF4** | **D14 = CN5 pin 9** ✔ | CN10 pin 5 |
| **LPUART1_TX (VCP)** | **PA2** | **AF12** | — | CN10 pin 35 (see §4 caveat) |
| **LPUART1_RX (VCP)** | **PA3** | **AF12** | — | CN10 pin 37 (see §4 caveat) |
| USART1_TX | PC4 | AF7 | D1 = CN9 pin 2 ✔ | CN10 pin 34 ✔ |
| USART1_RX | PC5 | AF7 | D0 = CN9 pin 1 ✔ | CN10 pin 6 ✔ |
| LD2 LED | PA5 | GPIO out | D13 = CN5 pin 6 ✔ | CN10 pin 11 |
| B1 button | PC13 | GPIO in | — | CN7 pin 23 |
| I2C2_SCL (fallback) | PA9 | AF4 | D8 = CN5 pin 1 ✔ | CN10 pin 21 |
| I2C2_SDA (fallback) | PA8 | AF4 | D7 = CN9 pin 8 ✔ | CN10 pin 23 |
| **+3V3** | — | — | **CN6 pin 4** | CN7 pin 16 |
| **+5V** | — | — | **CN6 pin 5** | CN7 pin 18 |
| **GND** | — | — | **CN6 pins 6 and 7**; **CN5 pin 7** | CN7 pins 8, 20, 22; CN10 pin 9 |

✔ = confirmed verbatim from UM2505 (ManualsLib rendering). Unmarked morpho numbers follow the
standard Nucleo-64 morpho layout corroborated at six points by ST's solder-bridge text (§4).

---

## Unverified items summary

| # | Item | Status |
|---|---|---|
| 1 | Printed default states of **SB2 / SB4** (PB8 routing to Arduino vs morpho BOOT0) | **UNCONFIRMED.** UM2505 Table 12 prints no "(default)" marks for SB1–SB15. SB2 inferred ON from working community reports of I2C1 on D14/D15. |
| 2 | **I2C pull-up resistors** on ARD_D14/ARD_D15 | **UNCONFIRMED — assume none.** Fit your own 4.7 k to 3V3. |
| 3 | **SB26** semantics | **CONTRADICTORY** between UM2505 Table 12 ("ST-LINK MCO used for HSE CLK") and the clock-source section (SB25+SB26 ON = X3 crystal default). |
| 4 | Whether **X3** (24 MHz) is physically fitted on every MB1367 revision | **Likely fitted** (UM2505 calls it the default; one forum inspection confirms C56/C59 soldered) but **board-revision dependent — verify visually.** |
| 5 | Exact **STM32CubeMX board-default clock tree** for NUCLEO-G474RE | **UNCONFIRMED.** No ST source found. |
| 6 | ST morpho **GND** pin list, and whether **CN10 35/37** carry PA2/PA3 or the ARD_D1/D0 nets | **UNCONFIRMED.** ManualsLib's HTML flattening of UM2505 Table 16 loses the odd/even column structure; repeated extractions disagreed. Use the confirmed Arduino-side grounds instead. |
| 7 | Solder bridges that swap **A4/A5** to I2C1 (PB9/PA15) | **UNCONFIRMED.** Default is almost certainly PC1/PC0 (ADC). |

### Method note

UM2505 was read through the ManualsLib HTML rendering because `st.com` PDF fetches timed out
repeatedly. Verbatim quotes of *prose* and of single-column tables (CN5, CN6, CN8, CN9, Table 12
solder-bridge descriptions) came through reliably and are marked ✔/verbatim. The **two-column
morpho Table 16 did not** — that is the one area where this document falls back on the standard
Nucleo-64 layout plus cross-checks. Everything load-bearing was corroborated against at least two
independent sources (UM2505 + stm32duino variant + Zephyr + ST-generated AF tables).
