# MAX17320G2 2S4P BMS board — hardware notes

Source: `ref-f767-max17320/altium-bms/` (`0001.SchDoc`, `max17320g_2.PcbDoc`,
`Batterie_Überwachung.pdf`). Netlist was reconstructed from the Altium
schematic records (wires + pin coordinates + net labels/power ports) and then
**independently cross-checked against the PCB netlist** (`Nets6` + `Pads6` +
`Components6`). Where both documents are quoted below they agree, unless a
mismatch is explicitly called out.

---

## 1. Connector pinout — P1 (TSW-105-05-L-S, 1x5, 2.54 mm, through-hole, vertical)

| Pin | Net    | Goes to (on this board)                                                    | Silkscreen | PCB pad X (mm) |
|-----|--------|----------------------------------------------------------------------------|------------|----------------|
| 1   | SYSP   | System/pack **positive** (after fuse B1 + both protection FETs Q2, Q1)      | `SYSP`     | 31.694 |
| 2   | SDA    | U1 pin 9 (SDA/DQ) — **nothing else, no pull-up**                           | `SDA`      | 34.234 |
| 3   | SCL    | U1 pin 8 (SCL/OD) — **nothing else, no pull-up**                           | `SCL`      | 36.774 |
| 4   | AOLDO  | U1 pin 12 (AOLDO) + C13 470 nF + D3 5.6 V zener + SG3 + Q5 source/R14      | `AOLDO`    | 39.314 |
| 5   | AGND   | System/pack **negative** = CSN side of the 5 mΩ shunt R18                   | `AGND`     | 41.854 |

**The first-pass guess (1=SYSP, 2=SDA, 3=SCL, 4=AOLDO, 5=AGND) is CONFIRMED**,
by three independent sources: schematic connectivity, PCB pad→net assignment,
and the top-overlay silkscreen labels.

### Physical orientation / pin-1 marking

- All five pads sit on one row at Y = 49.838 mm, X = 31.694 → 41.854 mm, exactly
  2.540 mm (100 mil) pitch, hole Ø 1.02 mm, multi-layer (through-hole).
- **Pad 1 (SYSP) is rectangular/square; pads 2–5 are round** — that is the pin-1
  marking. (Pad shape byte = 2 for pad 1, 1 for pads 2–5.)
- Every pin is additionally **labelled on the top silkscreen**: `SYSP`, `SDA`,
  `SCL`, `AOLDO` (printed below the row, the others above), `AGND`.
- Pin 1 is the end **farthest from** the three cell-tap headers J1/J2/J3, which
  are at the opposite (X ≈ 80 mm) edge of the board. Pad field spans
  X 28.2–81.7 mm, Y 28.2–54.0 mm, so P1 is near the top-left, cell taps at the right.

### The other headers (battery side — not for the MCU)

| Ref | Net   | Meaning                          | Pos (mm)        |
|-----|-------|----------------------------------|-----------------|
| J1  | BATTP | Cell-stack **positive**          | (79.99, 52.69)  |
| J2  | BATT2 | Cell-stack **midpoint** (2S tap) | (79.99, 46.63)  |
| J3  | BATTN | Cell-stack **negative**          | (80.01, 40.77)  |

All three are single-pin TSW-101-07-L-S with square pads.

---

## 2. What to connect from the Nucleo

**Connect (3 wires):**

| Nucleo            | P1 pin | Note |
|-------------------|--------|------|
| I2C SDA (open-drain, e.g. PB9) | 2 | needs an external pull-up, see below |
| I2C SCL (open-drain, e.g. PB8) | 3 | needs an external pull-up, see below |
| GND               | 5 (AGND) | the only ground the MCU should touch |

**You MUST add pull-up resistors — the board has none.** 2.2 k–10 k (4.7 k is a
good default) from SDA→rail and SCL→rail. Two options for the rail:

- **Nucleo 3.3 V** (simplest, guarantees the STM32 sees a valid V_IH).
  Caveat: verify in the MAX17320 datasheet that SDA/SCL tolerate 3.3 V pull-up
  while the part is running from the pack — *UNCONFIRMED from these files*.
- **P1.4 (AOLDO)** (guaranteed compatible with the gauge's own logic).
  Caveat: the AOLDO output voltage and its current capability are set by the
  part/NV config and are **UNCONFIRMED from these files** — measure P1.4 with
  respect to P1.5 before relying on it. If AOLDO turns out to be ~1.8 V, it is
  probably too low for the STM32's V_IH (≈0.7·VDD) and you should use 3.3 V instead.

**Do NOT connect:**

- **P1.1 (SYSP)** — leave it open. It is the pack positive terminal, ~6.0–8.4 V
  for 2S (≈7.2–7.4 V nominal). It is physically adjacent to SDA (pin 2), so a
  slipped jumper puts full pack voltage on an MCU pin. Leaving it unconnected is
  safe and changes nothing for I2C (see §5 below).
- **Do not also tie MCU ground to J3/BATTN.** AGND and BATTN are *not* the same
  node — they are separated by the 5 mΩ sense resistor. Grounding to both
  shorts out the shunt and destroys current/coulomb measurement.

**Also worth knowing before you connect:**

- The gauge is powered **only from the battery** (BATTP → 10 Ω → IN). The pack
  must be attached at J1/J2/J3 or there is nothing on the bus to talk to.
- I2C slave addresses (matches `../max17320.h`): **0x36** (7-bit) for memory
  0x000–0x0FF and **0x0B** (7-bit) for 0x100–0x1FF. Max SCL frequency:
  *UNCONFIRMED here* (datasheet; 400 kHz is the usual figure).
- `HAL_I2C_IsDeviceReady()` in the existing driver will fail with no pull-ups —
  a missing pull-up looks exactly like "board not responding".
- **ALRT is not on the connector.** U1 pin 7 goes through R17 150 Ω only to the
  gate of Q5, which drives the red LED DS2. Alerts must be polled over I2C.
- **PFAIL is destructive.** U1 pin 11 → gate of Q4 (2N7002K, 10 k pulldown R8) →
  heater of B1 (ITV4030L0812NR self-control protector = one-shot fuse). If PFAIL
  ever asserts, the fuse is blown permanently and the board is scrap. Be careful
  with anything that writes protection config / triggers a permanent failure.
- There is no ESD protection on SDA/SCL (the three spark gaps SG1/SG2/SG3 sit on
  SYSP, the ALRT/Q5-gate node, and AOLDO respectively).

---

## 3. Answers to the specific questions

### Q1 — P1 pin-to-net mapping
Confirmed as above. Evidence:
- Schematic net groups: `SDA = {P1.2, U1.9}`, `SCL = {P1.3, U1.8}`,
  `SYSP ∋ P1.1`, `AOLDO ∋ P1.4`, `AGND ∋ P1.5`.
- PCB pad→net: `P1.1=SYSP, P1.2=SDA, P1.3=SCL, P1.4=AOLDO, P1.5=AGND`.
- Silkscreen text positions line up with those pads.

### Q2 — Pull-ups on SDA/SCL: **NO. None on this board.**
This is definitive, from two independent files:

- Schematic netlist: `SDA` contains exactly two pins — `P1.2` and `U1.9 (SDA/DQ)`.
  `SCL` contains exactly two pins — `P1.3` and `U1.8 (SCL/OD)`.
- PCB netlist: net `SDA` has exactly **2 pads** (P1.2, U1.9); net `SCL` has
  exactly **2 pads** (P1.3, U1.8).

No resistor, no capacitor, no diode, nothing else is on either net. **An external
master must supply the pull-ups.** (The MAX17320's SCL/OD and SDA/DQ are
open-drain, so the bus is dead without them.)

### Q3 — What AOLDO connects to
Net `AOLDO` = 7 nodes (identical in schematic and PCB):

| Node | Part | Role |
|------|------|------|
| U1.12 | MAX17320 AOLDO pin | the always-on LDO output itself |
| C13.1 | 470 nF 25 V 0603 (other end AGND) | bypass/decoupling |
| D3.2  | BZD27C5V6P-E3-08, 5.6 V zener, cathode on AOLDO, anode AGND | clamp |
| SG3.2 | spark gap to AGND | ESD |
| Q5.2  | BSS223PWH P-channel MOSFET **source** | LED driver supply |
| R14.2 | 100 kΩ to Q5 gate | gate pull-up (holds the LED off) |
| P1.4  | connector | brought out to the header |

So the only *load* is the ALRT indicator branch: Q5 (P-ch, source = AOLDO)
→ R12 365 Ω → DS2 red LED (LTST-C190KRKT) → AGND. Q5's gate is held at AOLDO by
R14 (LED off) and is pulled down either by ALRT through R17 150 Ω, or by the
tactile switch SW2 to AGND. So AOLDO only sources LED current when ALRT is
asserted or SW2 is pressed. Its voltage/current rating is **UNCONFIRMED** from
these files — measure it before using it as a pull-up rail.

### Q4 — What SYSP is
SYSP is the **system / pack positive terminal**, i.e. the switched output of the
protection path:

```
J1 (BATTP, cell-stack +) → B1 ITV4030L0812NR fuse (FUSE_1→FUSE_2)
    → Q2 SI4420BDY source(1,2,3) … drain(5..8)     [gate = CHG, via R6 100 Ω]
    → Q1 SI4420BDY drain(5..8) … source(1,2,3)     [gate = DIS, via R5 100 Ω]
    → SYSP → P1.1
```

- Voltage: essentially the pack voltage when the FETs are on — for 2S Li-ion
  roughly **6.0–8.4 V** (~7.2–7.4 V nominal), minus the FET/fuse IR drop. When
  protection opens the FETs, SYSP collapses to whatever the load/charger imposes.
- Other things on SYSP: R3 1 kΩ → PCKP (pack-detect input, with C6 10 nF and D1
  RB520G-30 to AGND); a 330 Ω resistor → ZVC (zero-volt-charge input); R7 680 kΩ
  → DS1 green LED → AGND (a ~10 µA "SYSP present" indicator); C1+C2 in series
  across the FET pair; SG1 spark gap to AGND; Q3 (2N7002K, source on SYSP, gate
  pulled to AGND by R4 10 k, drain on Q1's gate node); SW1 + R1 1 kΩ, a manual
  push-button path from SYSP back to the Q2 source node.
- **Safe to leave unconnected?** Yes. SYSP is just the load/charger terminal; the
  gauge and its I2C interface do not need it. With it open the gauge simply sees
  no charger present at PCKP. Leaving it open is the *safer* choice — see the
  warning in §2 about it sitting next to SDA.

### Q5 — AGND vs pack negative, and where the FETs are

**The protection FETs are HIGH side.** Both Q1 and Q2 (SI4420BDY, N-channel,
SO-8: pins 1–3 = Source, 4 = Gate, 5–8 = Drain) are in the *positive* rail,
back-to-back common-drain:

- `Q1.S (1,2,3) = SYSP`, `Q1.D (5,6,7,8) = ` common node with `Q2.D (5,6,7,8)`
- `Q2.S (1,2,3) = ` fuse output node `B1.3 (FUSE_2)`, and `B1.1 (FUSE_1) = BATTP`
- `Q1.G = DIS` (U1 pin 4) through R5 100 Ω → discharge FET (pack side)
- `Q2.G = CHG` (U1 pin 3) through R6 100 Ω → charge FET (battery side)
- Confirmed by the charge pump: U1 pin 2 = CP with C11 470 nF to IN — needed only
  to drive high-side N-channel FETs.

**There is NO low-side FET at all**, so tying an external MCU ground to AGND
**cannot bypass a protection FET**. That concern does not apply to this board.

**But AGND is not literally the cell-stack negative.** There are two distinct
ground nodes, separated by the sense resistor:

| Node | Also known as | What is on it |
|------|---------------|---------------|
| `AGND` | pack/system negative (P1.5) | `R18.1`, `U1.14 (CSN)`, P1.5, C3/C6/C13 grounds, D1.A, D2.A, D3.A, DS1.K, DS2.K, Q4.Source, R4.1, R8.1, SG1/2/3 pin 1, SW2.B |
| `BATTN` | cell-stack negative (J3); also carries the schematic's `GND` power-port symbol | `R18.2`, `U1.15 (CSP)`, `U1.20 (GND)`, `U1.25 (EPAD)`, `J3.1`, RT1–RT4 pin 2, C9/C12/C14/C15 grounds |

`R18 = MFC0603-R005FT5, 5.0 mΩ 1% 0.5 W` sits between them: **pin 1 → AGND
(CSN side), pin 2 → BATTN (CSP side)**. So the shunt is in the **low side** of
the pack (in the negative return), while the FETs are in the high side.

Practical consequences:

- The MCU's ground must be **AGND (P1.5)** — that is the system-negative terminal
  the connector is designed around.
- The MAX17320's own logic ground is BATTN (CSP), one shunt away. At full current
  (12 A) that is a 60 mV offset between MCU ground and gauge ground — harmless
  for I2C levels, but it exists.
- Any current the MCU returns into AGND flows through the shunt and is counted by
  the coulomb counter (µA-level for I2C — negligible, but conceptually real).
- Because the FETs are high side, AGND stays solidly connected to the cells even
  when protection trips, so **I2C remains usable with the FETs off**.
- Never bridge AGND to BATTN/J3 externally (that shorts the shunt).

### Q6 — Thermistors, shunt, and the rest

**Thermistors — four, all populated, all on-board 0402 NTCs**
(NCU15XH103E6SRC, 10 kΩ ±3%), each from a THx pin to BATTN:

| PCB ref (silkscreen) | Schematic ref | Pin | PCB position (mm) |
|----------------------|---------------|-----|-------------------|
| RT1 | RT4 | U1.10 TH1 | (63.25, 32.77) |
| RT2 | RT1 | U1.16 TH2 | (67.60, 37.26) |
| RT3 | RT2 | U1.18 TH3 | (67.60, 40.03) |
| RT4 | RT3 | U1.19 TH4 | (61.95, 40.03) |

No bias/divider resistors and no external thermistor connector — the TH pins are
brought out nowhere, so **all four channels measure PCB temperature**, not cell
temperature (unless the cells are physically pressed to the board). The gauge
biases them internally.

**Current sense resistor** — `R18`, `MFC0603-R005FT5`, **0.005 Ω**, 1%, 500 mW,
0603, low side, between `BATTN` (CSP, U1.15) and `AGND` (CSN, U1.14). This
matches `MAX17320_RSENSE_MOHM = 5` in `../max17320_config.h`.

**Cell tap wiring (2S)** — the stack is measured through:
`BATTP —49.9 Ω(R9)→ BATTS (U1.24)`, `BATT2/J2 —49.9 Ω→ CELL2 (U1.22)`,
`CELL2 —0 Ω→ CELL3 (U1.23)`, `CELL2 —0 Ω→ CELL1 (U1.21)`, `BATTN = CSP/GND`.
So the unused cell inputs are strapped to the 2S midpoint, i.e. CELL1 = midpoint,
BATTS = pack top. Filter caps C7/C8/C10/C12 (0.01 µF 1206) sit between adjacent
taps. **The midpoint wire to J2 must be connected** or the gauge cannot see 2 cells.

**Supply/decoupling of the gauge** — BATTP → 10 Ω → IN (U1.1) with C9 100 nF;
C11 470 nF between IN and CP (charge-pump); REG2 (U1.17) and REG3 (U1.13) each
bypassed with 470 nF (C15, C14) to BATTN; AOLDO with 470 nF (C13) to AGND.

**Other user controls / indicators on the board**

- `SW1` (EVQ-Q2K03W tactile) + `R1` 1 kΩ: manual path from SYSP to the Q2-source
  node — a wake/recovery button across the FET pair.
- `SW2` (tactile) pulls the Q5 gate to AGND → lights the red LED DS2 manually.
- `DS1` green LED via R7 680 kΩ from SYSP → "system positive present" (very dim,
  ~10 µA).
- `DS2` red LED via R12 365 Ω, driven by Q5 from AOLDO → ALRT indicator.
- `R2` (0 Ω, 0805) is wired **in parallel with the B1 fuse element**
  (BATTP ↔ B1.3) — i.e. a fuse-bypass strap. Whether it is actually meant to be
  populated in production is **UNCONFIRMED** (it is not marked DNP in the data),
  but if fitted it would partially defeat the permanent-fuse function.

---

## 4. Key net summary (schematic; identical in the PCB netlist)

```
SDA    : P1.2, U1.9 (SDA/DQ)                      <- no pull-up
SCL    : P1.3, U1.8 (SCL/OD)                      <- no pull-up
AOLDO  : U1.12, C13.1, D3.2, SG3.2, Q5.2, R14.2, P1.4
AGND   : U1.14 (CSN), R18.1, P1.5, C3.1, C6.1, C13.2, D1.A, D2.2, D3.1,
         DS1.2, DS2.2, Q4.2, R4.1, R8.1, SG1.1, SG2.1, SG3.1, SW2.B
BATTN  : U1.15 (CSP), U1.20 (GND), U1.25 (EPAD), R18.2, J3.1,
         RT1.2, RT2.2, RT3.2, RT4.2, C9.2, C12.2, C14.2, C15.2
SYSP   : P1.1, Q1.1/2/3 (S), Q3.2, C1.1, C4.2, R1.2, R3.1, R7.2, SG1.2, 330Ω→ZVC
BATTP  : J1.1, B1.1 (FUSE_1), R2.2, R9.2, 10Ω→IN
ALRT   : U1.7, R17.2         (not on the connector)
PFAIL  : U1.11, Q4.1 (gate), R8.2   -> Q4 drain -> B1.2 HEATER  (one-shot fuse)
TH1..4 : U1.10/16/18/19, each with a 10 k NTC to BATTN
```

---

## 5. Uncertainties / things marked UNCONFIRMED

- **AOLDO output voltage and current capability** — not derivable from the
  schematic/PCB/BOM. Measure P1.4 relative to P1.5, or check the datasheet/NV
  config, before using it as the I2C pull-up rail.
- **Whether a 3.3 V pull-up is within the MAX17320's SDA/SCL absolute maximum** —
  not answerable from these files; check the datasheet.
- **Maximum I2C clock rate** — not in these files (datasheet).
- **R2 (0 Ω across the fuse) fitted or not** — schematic shows it connected with
  no DNP marking; intent unclear.
- **Schematic ↔ PCB designator mismatch (real, verified by component UniqueID):**
  three groups are annotated differently in the two documents. Function and
  values are identical in both; only the printed reference designators differ.
  The **PCB** names are what is silkscreened on the physical board.

  | Function / value | Schematic calls it | PCB (silkscreen) calls it |
  |------------------|--------------------|---------------------------|
  | 10 Ω, BATTP→IN supply filter | R10 | **R11** |
  | 0 Ω, CELL2→CELL3 strap | R11 | **R10** |
  | 330 Ω, SYSP→ZVC | R15 | **R16** |
  | 0 Ω, CELL2→CELL1 strap | R16 | **R15** |
  | NTC on TH1 | RT4 | **RT1** |
  | NTC on TH2 | RT1 | **RT2** |
  | NTC on TH3 | RT2 | **RT3** |
  | NTC on TH4 | RT3 | **RT4** |

  Everything else (58 components) matches between the two documents.
