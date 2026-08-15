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

---
---

# APPENDIX A (appended 2026-08-15) — External power, JP5, and the back-feed question

> **Status of sources: upgraded.** Everything in this appendix comes from the **full UM2505 Rev 4
> PDF (February 2021, 44 pages)** and the **MB1367-G474RE-C04 schematic (21-January-19, 8 sheets)**,
> both retrieved this time via `web.archive.org` and read directly (`pdftotext -layout` for prose and
> tables, `pdftoppm` page renders for the schematic sheets). The "SCH — **not retrieved**" line in the
> source table at the top of this file is now **obsolete**; sheets 5, 6 and 7 were read. Section A.7
> lists the items in the *Unverified items summary* that this closes.

## A.1 Every way MB1367 can be powered

UM2505 §6.4, **verbatim**:

> "The power supply can be provided by five different sources:
> • A host PC connected to CN1 through a USB cable (default setting)
> • An external 7 V - 12 V (VIN) power supply connected to CN7 pin 24
> • An external 5 V (E5V) power supply connected to CN7 pin 6
> • An external 5 V USB charger (5V_USB_CHGR) connected to CN1
> • An external 3.3 V power supply (3V3) connected to CN7 pin 16"

| Source | Connector pin(s) | Voltage range | Max current | On-board 3V3 reg (U12) in path? | ST-LINK functional? | JP5 |
|---|---|---|---|---|---|---|
| **5V_USB_STLK** (host PC) | **CN1** | 5 V | **500 mA** (see caution below) | **Yes** | **Yes** | **[1-2] default** |
| **VIN** | **CN6 pin 8** / **CN7 pin 24** | **7 V – 12 V** | **800 mA** @ 7 V; 450 mA @ 7–9 V; 250 mA @ 9–12 V | **Yes** (+ U11 LD1117S50 5 V LDO first) | **Yes** (via D3) | **[3-4]** |
| **E5V** | **CN7 pin 6** only (*not* on any Arduino header) | **4.75 V – 5.25 V** | **500 mA** | **Yes** | **Yes** (via D2) | **[5-6]** |
| **5V_USB_CHGR** (USB charger) | **CN1** | 5 V | not specified ("-") | **Yes** (bypasses power switch U4) | **No** — Fig. 13 is annotated "No debug" | **[7-8]** |
| **3V3** | **CN6 pin 4** / **CN7 pin 16** | **3 V – 3.6 V** | **1.3 A** | **No** — back-drives U12's output directly | **NO** | **removed** (Fig. 14: "No jumper") |

Ranges/currents are UM2505 Tables 6–9. Two further ST statements, **verbatim**:

> "If the power supply is 3V3, the ST-LINK is not powered and cannot be used." (§6.4)

> **Caution:** "If the maximum current consumption of the STM32G4 Nucleo-64 board and its shield
> boards exceeds 300 mA, it is mandatory to power the STM32G4 Nucleo-64 board with an external power
> supply connected to E5V, VIN or 3.3 V." (§6.4)

Note the 500 mA / 300 mA discrepancy is ST's own: the USB enumeration requests 500 mA and the LD4 OC
LED trips above 500 mA, but the Caution sets the practical ceiling at 300 mA.

**The `5V` pin is documented as an OUTPUT, not an input.** UM2505 Table 15, CN6 pin 5, verbatim
Function column: **"5 V output"**. It is not listed as a power *source* anywhere in §6.4, and in the
Figure 9 power tree the `5V` and `3V3` arrows point *out* to the header block while `E5V`, `VIN` and
`VBAT` point *in*. Also §6.6.1: *"LD3 PWR — The green LED indicates that the STM32G4 part is powered
and +5 V power is available on CN6 pin 5 and CN7 pin 18."*

## A.2 The power-selection jumper is **JP5**, labelled "5V_SEL"

UM2505 Table 4 "Jumper configuration", **verbatim** (defaults bold per ST's footnote 1):

| Jumper | Definition | Position | Comment |
|---|---|---|---|
| JP1 | NRST | OFF | STLINK-V3E reset |
| JP3 | T_RST | ON | - |
| **JP5** | **5 V power-source selection** | **ON [1-2] (Default)** | **5V_USB_STLK (from ST-LINK)** |
| | | ON [3-4] (optional) | 5V_VIN |
| | | ON [5-6] (optional) | E5V |
| | | ON [7-8] (optional) | 5V_USB_CHGR |
| JP6 | IDD | ON | - |
| JP7 | BOOT0 | OFF | - |
| JP8 | VREF+ selection | ON [1-2] (Default) / ON [2-3] (optional) | VREF+ supplied with VREF / with VDD |

**Factory default = JP5 shunt on [1-2] = powered from the ST-LINK USB.** Schematic sheet 6 prints
"Shunt Fitted 1-2" next to JP5. JP5 is a **4×2 header** (8 pins), not the 2-pin U5V/E5V header of the
older MB1136 — do not carry MB1136 habits over. The spare-shunt parking post next to it is **HW5**.

> **⚠ UM2505 Rev 4 typo — flagged.** The VIN paragraph says *"jumper **JP2** on pins 3-4 '5V_VIN'"*
> and the charger paragraph says *"jumper **JP2** on pins 7-8 '5V_CHGR'"*. **This is wrong; it means
> JP5.** Table 4, §6.4.1 (*"Connect jumper JP5 between pins 5 & 6 for E5V or between pins 3 & 4 for
> VIN"*) and the schematic all say JP5 — and **there is no JP2 (or JP4) on MB1367 at all**; the only
> jumpers on the C-04 schematic are JP1, JP3, JP5, JP6, JP7, JP8.

## A.3 The real topology (MB1367-C-04 sheets 6 and 7) — this is what answers everything

```
                                   ┌──── D1 BAT60J ────┐
CN1 VBUS ── net 5V_USB_CHGR ───────┤                   │
   │            (also on CN10 p8)  │                   │
   │                               │                   ├──> U7 LD3985M33R ──> 3V3_STLK
   └─> U4 STMPS2151STR  IN         │                   │     (ST-LINK's OWN 150 mA
       EN <- T_PWR_EN (R8 10k pd)  │                   │      supply -> STM32F723)
       OUT = net 5V_USB_STLK ──> JP5 pin 1             │
                                   │                   │
CN7 p6  ── net E5V ────────┬───> JP5 pin 5             │
                           └──── D2 BAT60J ────────────┤
                                                       │
CN6 p8 /  ── VIN ─> U11 LD1117S50TR ─> net 5V_VIN ─┬─> JP5 pin 3
CN7 p24     (linear!)                              └── D3 BAT60J ──┘

JP5 pins 2,4,6,8 are ALL commoned  ==  net "5V"
                                        ├──> CN6 pin 5  and  CN7 pin 18   (the "5V" pin)
                                        ├──> R7 510R + LD3 green (5V_PWR)
                                        ├──> R12 2K7 / R11 4K7 divider -> T_PWR_EXT (PB1 of ST-LINK)
                                        └──> U12 LD39050PU33R ──> 3V3 ──SB5──> JP6(IDD) ──> VDD ──> MCU
```

Four consequences, all schematic-verified, none of them obvious from the text:

1. **The ST-LINK has its own supply, diode-OR'd from three sources** (D1/D2/D3, BAT60JFILM Schottkys,
   anodes at `5V_USB_CHGR` / `E5V` / `5V_VIN`, cathodes commoned into U7 LD3985M33R). The diodes make
   these three mutually non-back-feeding. **The `5V` net is NOT one of the OR'd inputs.**
2. Therefore **feeding the `5V` pin does not power the ST-LINK**, and feeding **E5V does** — even with
   JP5 pointed elsewhere.
3. **Between the four sources and the `5V` net there is nothing but the JP5 shunt** — no diode, no
   ideal-diode MOSFET, no fuse, no series resistor. The `5V` pin is hard-wired to whichever source
   JP5 selects.
4. **VIN goes through a *linear* regulator (U11 LD1117S50TR)**, which is why the VIN current budget
   collapses from 800 mA to 250 mA as VIN rises — it is thermal, not a current limit. Sheet 5 carries
   ST's own note: **"WARNING voltage applied to VIN <12V"**.

Doc nit: Figure 9 calls the ST-LINK LDO **U17**; the C-04 schematic calls it **U7**. Same part
(LD3985M33R).

## A.4 Is there a config that works with AND without USB, with no jumper change?

**Yes — exactly one: E5V on CN7 pin 6 with JP5 on [5-6].** (One permanent move off the factory
default; after that the shunt never moves again.)

| Option | Works without USB? | Plug USB in while externally powered? | Jumper must move? |
|---|---|---|---|
| **E5V, CN7 pin 6, JP5 [5-6]** | ✅ **Yes** — 5 V rail *and* ST-LINK both fed from E5V | ✅ **Safe.** JP5 pin 1 is left open, so U4's output goes nowhere even if the ST-LINK turns it on. USB VBUS reaches only D1, which the OR-diodes isolate. **No contention path exists.** | Once, [1-2]→[5-6], then never again |
| VIN 7–12 V, JP5 [3-4] | ✅ Yes | ✅ Safe — same reason (JP5 pin 1 open) | Once, [1-2]→[3-4] |
| **VIN fed with 5 V** | ❌ **No — do not.** U11 LD1117S50 needs ~1.1 V dropout, so 5 V in gives ≈3.9 V on the `5V` rail and ≈3.5 V into the ST-LINK's LDO. Out of spec (ST says 7–12 V) and marginal. | — | — |
| **5 V into the `5V` pin, JP5 [1-2]** | ✅ runs, but **no debug** (ST-LINK unpowered — see A.3.2) | 🚫 **NOT safe — this is the fault case.** See A.5. | — |
| 3V3, CN6 p4 / CN7 p16, JP5 removed | ✅ Yes | ❌ **No debug ever** — "the ST-LINK is not powered and cannot be used" | Shunt must be **removed** |
| Factory default JP5 [1-2] | ❌ No — board is dead without USB | — | — |

**There is no configuration that runs without USB while JP5 stays at the factory [1-2] position.**
With JP5 on [5-6], the converse is also true and is a *feature*: USB alone will not power the target
(only the ST-LINK), so the board's power state is unambiguous.

## A.5 Back-feed: what happens if 5 V is applied to the `5V` pin with USB connected

**UM2505 says nothing about this case** — the `5V` pin is documented "5 V output" only. The schematic
and the U4 datasheet are the authority. Answer to "diode, ideal-diode MOSFET, or nothing?":

- Between the four sources and the `5V` net: **nothing but the JP5 shunt.**
- Between USB VBUS and `5V_USB_STLK`: **U4, an STMPS2151STR** — an **N-channel** high-side switch
  (not a P-channel, so no naive OUT→IN body diode) with **specified reverse-current blocking**.

STMPS2151 datasheet (ST DS5410 Rev 7), §3.4 "Reversed current blocking", **verbatim**:

> "When the switch is OFF (disabled through the EN pin), or when the STMPS device is unpowered
> (VIN = 0 V) the switch behaves as an Hi-Z at the output pin, ensuring that no reverse current will
> flow into the device when VIN < VOUT."

> "**Note:** In the case where the switch is ON, and a voltage higher than VIN is applied to the OUT
> pin, a reverse current occurs. **This operating condition is not allowed.**"

Reverse leakage when OFF is **≤ 2 µA** (Table 12). Absolute maximum (Table 7): **VOUT ≤ VIN + 0.3 V**.
R_ON at 5 V: **typ 90 mΩ, max 110 mΩ**. Forward current limit I_OS: **0.60 / 0.80 / 1.00 A**
min/typ/max, rated 500 mA. FAULT is blanked ~4–15 ms and drives LD4.

So the behaviour splits cleanly in two:

- **U4 OFF** (before enumeration, or if firmware holds it off): Hi-Z, ≤2 µA. **No back-feed to the PC.**
- **U4 ON** (after a successful enumeration asserts T_PWR_EN): the buck's 5 V and the PC's VBUS are
  **hard-paralleled through ~90 mΩ**. Reverse current is limited only by that 90 mΩ plus the USB cable
  and connector resistance — **the 0.6–1.0 A current limit protects the forward direction only**, and
  the reverse current is not an overcurrent event, so **LD4 stays dark**. ST's own words: *"not
  allowed."*

**Can it produce hundreds of mA? Yes, easily — the arithmetic lands right on ~600 mA.** A 5.00 V buck
against a PC VBUS that sits at 4.75–4.95 V at the far end of a micro-B cable gives 50–250 mV of
mismatch; across 90 mΩ plus ~0.2–0.4 Ω of cable/connector that is **roughly 150 mA to >1 A of
circulating current that does no work**. To get exactly 600 mA you need only ~54 mV of mismatch across
the FET alone, or ~230 mV across FET+cable. A healthy NUCLEO-G474RE (MCU at 170 MHz + STLINK-V3E +
LEDs) should be roughly **100–150 mA**, so ~450–500 mA of unexplained draw is exactly this signature.

**Damage risk:** the condition violates U4's absolute maximum (VOUT ≤ VIN + 0.3 V) whenever the buck
exceeds VBUS by >0.3 V, and it back-powers the host PC's USB port. Whether that has already damaged
anything here is **UNCONFIRMED**, but it is a credible mechanism.

> **⚠ UNCONFIRMED — the one gap.** Sheet 7 shows the ST-LINK sensing the `5V` net through an
> **R12 2K7 / R11 4K7 divider into T_PWR_EXT (PB1 of the STM32F723)** — evidently so the firmware can
> detect that external 5 V is already present. **Whether STLINK-V3E firmware then withholds T_PWR_EN
> and keeps U4 OFF is undocumented in UM2505 and unverified.** If it does, the back-feed never starts
> and the excess current has some other cause. This is precisely why the measurement in A.6 is worth
> doing before concluding.

### A.5.1 Decisive, non-destructive test

The raw USB VBUS net is brought out to a documented morpho pin: **CN10 pin 8 = `5V_USB_CHGR`**
(UM2505 Table 16, with footnote 4: *"5V_USB_CHGR is the 5 V power from the STLINK-V3E USB connector
that rises first. It rises before the 5 V rising on the board."*). So, with the buck live and USB
plugged in, measure DC between **CN10 pin 8** and **CN6 pin 5**:

- **Difference of hundreds of mV, stable** → U4 is OFF, the rails are isolated, **back-feed refuted**;
  look elsewhere for the 600 mA.
- **Difference of only a few tens of mV** → U4 is ON and the rails are tied. **Back-feed confirmed**,
  and you can read the current straight off the meter: **I_reverse ≈ ΔV / 0.09 Ω** (54 mV ⇒ 600 mA).

Cross-check: unplug USB and re-measure the buck's output current. A drop of ~450–500 mA to a sane
~100–150 mA confirms it outright.

## A.6 Recommended wiring

**Do this:**

| What | Where |
|---|---|
| Buck **+5 V** (trim to **4.9–5.1 V**; spec is 4.75–5.25 V) | **CN7 pin 6 = E5V** |
| Buck **GND** | **CN7 pin 8** (or CN6 pin 6 / CN6 pin 7 / CN5 pin 7) |
| **JP5 shunt** | move **[1-2] → [5-6]**, once, permanently. Park the spare on HW5. |
| Budget | **≤ 500 mA** on E5V |

Then plug and unplug the ST-LINK USB freely — the board's behaviour is identical either way, and
there is no path by which your buck can push current into the PC. Green **LD3** ON is the confirmation
that the `5V` rail is live. ST's own procedure (§6.4.1), **verbatim**:

> "When powered by VIN or E5V, it is still possible to use the ST-LINK for programming or debugging
> only, but it is mandatory to power the board first using VIN or EXT, then to connect the USB cable
> to the PC. In this way the enumeration succeeds, thanks to the external power source.
> The following power-sequence procedure must be respected:
> 1. Connect jumper JP5 between pins 5 & 6 for E5V or between pins 3 & 4 for VIN
> 2. Connect the external power source to VIN or E5V
> 3. Power on the external power supply 7V < VIN < 12 V for VIN, or 5V for E5V
> 4. Check that the green LED LD3 is turned ON
> 5. Connect the PC to the USB connector CN1
> If this order is not respected, the board may be powered by USB first, then by VIN or E5V as the
> following risks may be encountered:
> 1. If more than 300 mA current is needed by the board, the PC may be damaged or the current supplied
>    can be limited by the PC. As a consequence, the board is not powered correctly.
> 2. 300 mA is requested at enumeration so there is risk that the request is rejected and the
>    enumeration does not succeed if the PC cannot provide such current. Consequently, the board is
>    not power supplied (LED LD3 remains OFF)."

With JP5 on [5-6] the ordering in that procedure stops mattering much, because the ST-LINK draws its
own 3V3 from E5V through D2 regardless of USB — but following it costs nothing.

### A.6.1 Optional: make USB-only operation work too, without touching the jumper

If you also want the board to run from USB alone when the pack is disconnected, **fit a Schottky
(BAT60J, SS14, 1N5819 — anything ≥200 mA, low Vf) with its ANODE on JP5 pin 1 and its CATHODE on
JP5 pin 2, keeping the [5-6] shunt in place.** JP5 pin 1 is `5V_USB_STLK` (U4's output) and pin 2 is
already the `5V` net, so this ORs the USB in behind a blocking diode:

- Pack present → E5V (5.0 V) holds the rail; the diode is reverse-biased; **no reverse current into
  U4 or the PC**, which is exactly the condition its datasheet forbids.
- Pack absent, USB present → rail sits at VBUS − ~0.35 V ≈ 4.65 V, comfortably above U12's dropout.

This needs **no board rework** — build it as a two-pin plug that seats on JP5 pins 1-2. It is my
recommendation derived from the schematic; **ST does not document this configuration**, so treat it
as an engineering addition rather than an ST-sanctioned one, and keep the total under 500 mA.

### A.6.2 Do NOT do these

- ❌ **5 V to the `5V` pin (CN6 p5 / CN7 p18) with USB connected** — the fault case of A.5.
- ❌ **5 V to VIN** — LD1117S50 dropout leaves ~3.9 V; ST specifies 7–12 V.
- ❌ **Pack 8.3 V to the `5V` pin** — sheet 6 shows it landing directly on U12's input with nothing in
  series, and (via a switched-on U4) on the PC's VBUS. *(U12 LD39050's exact absolute maximum was not
  checked in this pass — **UNCONFIRMED** — but 8.3 V on a 3.3 V LDO's input is out of spec regardless.)*
- ❌ **Anything to the `3V3` pin** unless you also pull the JP5 shunt and accept losing the debugger.

## A.7 Items this appendix closes from the *Unverified items summary*

| # | Was | Now |
|---|---|---|
| 6 | ST morpho **GND** pin list — UNCONFIRMED | **RESOLVED.** UM2505 Table 16 read directly: **CN7 GND = pins 8, 19, 20, 22**; **CN10 GND = pins 9, 20**; **CN10 pin 32 = AGND**. Note **CN7 pin 19 is also GND** (an odd pin — the earlier standard-layout guess had only 8/20/22). Power: **CN7 p5 VDD, p6 E5V, p12 IOREF, p14 NRST, p16 3V3, p18 5V, p24 VIN, p33 VBAT** — all as previously assumed. **CN10 pin 8 = 5V_USB_CHGR** (raw USB VBUS, was not in the earlier table at all). |
| 6b | Whether **CN10 35/37** carry PA2/PA3 or the ARD_D1/D0 nets — UNCONFIRMED | **RESOLVED: they carry the ARD_D1_TX / ARD_D0_RX nets**, i.e. **USART1 PC4/PC5 by default**, not PA2/PA3. Sheet 5 wires CN10 p35/p37 to the same nets as CN9 p2/p1 and annotates *"Default: USART1 from PC4/PC5 — Optional: LPUART1 from PA2/PA3"*; Table 16 lists them as "PA2 / PC4" and "PA3 / PC5" with the default in bold. **CN10 pins 36 and 38 are not connected.** The §4 caveat in this file was right to warn. |
| — | SCH "not retrieved" | **RESOLVED** — MB1367-G474RE-C04 read (sheets 5, 6, 7). |

Sheet 5 also settles, incidentally: **SB33 gates PC4 to CN10 pin 34**, and **SB34/SB37 are DNF**
(marked "Close only for I2C on A4/A5"), which supports item 7's guess that A4/A5 default to PC1/PC0.

### Appendix method note

`curl` from `web.archive.org` → `pdftotext -layout` (prose + tables) and `pdftoppm -r 150/400 -png`
(schematic sheets read as images). STMPS2151STR data from ST DS5410 Rev 7 (15 Feb 2022) via the
`r.jina.ai` proxy. Everything marked verbatim is quoted from those PDFs.
