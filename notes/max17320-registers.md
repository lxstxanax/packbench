# MAX17320 / MAX17320G2x — READ-ONLY runtime register map

Target: monitoring dashboard on STM32 (F767) over I2C. **Rsense = 5 mΩ** assumed throughout.

## 0. Sources & confidence

| Source | Type | Used for |
|---|---|---|
| **MAX17320 datasheet, 19-100749; Rev 12; 7/25, 180 pp.** (analog.com, retrieved via web.archive.org snapshot `20260211225213` of `https://www.analog.com/media/en/technical-documentation/data-sheets/max17320.pdf`) | **PRIMARY** | Everything below unless marked otherwise |
| github.com/msalinoh/max17320_read (`src/max17320.h/.c`) | cross-check | addresses, bit indices — agrees with datasheet |
| github.com/ajaykumar666/MAX17320-arduino-library (`register.h`, `MAX17320.h`, `config.h`) | cross-check | addresses, LSBs — agrees |
| github.com/shaoyuancc/max17320 (`src/register.rs`) | cross-check | addresses, bit indices — agrees |
| github.com/gswdh/max17320_drv | **empty repo** (README only, 0 bytes) — no useful content | — |

Bitfield bit-index assignments below were verified against the datasheet's own table
column geometry (extracted `pdftotext -bbox` x-coordinates of each cell vs. the D15..D0
header row), not just by eye. Anything not confirmable from the datasheet is marked
**UNCONFIRMED**.

---

## 1. Bus, addressing, endianness

Datasheet Table 90 / Table 116.

| 7-bit addr | 8-bit addr | Protocol | Byte-offset range you send | Internal 9-bit address |
|---|---|---|---|---|
| **0x36** | 6Ch | I2C | 0x00–0xFF | 0x000–0x0FF |
| **0x0B** | 16h | SMBus | 0x00–0x7F | 0x100–0x17F |
| **0x0B** | 16h | I2C | 0x80–0xFF | 0x180–0x1FF |

Notes:
* Your framing "0x100–0x1FF at 0x0B" is correct, but it splits: `0x100–0x17F` is the **SBS/SMBus**
  half (Temp1-4, AvgTemp1-4, LeakCurrRep live here) and `0x180–0x1FF` is the **NVM shadow-RAM** half.
  Rule used by every driver: `if (addr > 0xFF) { slave = 0x0B; reg = addr & 0xFF; }`.
* SMBus Read-Word == STM32 `HAL_I2C_Mem_Read(..., MEMADDR_SIZE_8BIT, buf, 2)`, so reading
  `0x100–0x17F` works with a plain I2C mem-read. `Config.DisBlockRead` defaults to **1**
  (= normal reads in the 16h space), so no configuration is needed for this.
* **All registers are 16-bit, transmitted LSB byte first** (datasheet, Slave Address section:
  "…byte (LSB) first"). `val = buf[0] | (buf[1] << 8)`.
* Max SCL = **400 kHz** (see §9).
* Part decode: `MAX17320G2x` = 24-TQFN, **I2C** interface (G1x = 1-Wire, X.. = 30-WLP).
  `G22` adds SHA-256. Standard addresses 6Ch/16h. *Only* the `MAX17320X32` variant uses the
  alternate addresses 0xEC/0x96 — a G2x part is 6Ch/16h.
* `DevName` (0x021) reads **4209h, 420Ah or 420Bh**. Good "is it alive" probe.

### Standard register-type resolutions (datasheet Table 15)

| Type | LSB | Range | Human units @ Rsense = 5 mΩ |
|---|---|---|---|
| Capacity | 5.0 µVh / Rsense | 0 … 327.675 mVh/Rsense | **1.0 mAh / LSB** |
| Percentage | 1/256 % | 0 … 255.9961 % | `raw / 256.0` % |
| Voltage | 0.078125 mV | 0 … 5.11992 V | `raw * 0.078125` mV |
| Current | 1.5625 µV / Rsense, **signed 2's complement** | ±51.2 mV/Rsense | **0.3125 mA / LSB** |
| Temperature | 1/256 °C, **signed** | -128 … +127.996 °C | `int16 / 256.0` °C |
| Resistance | 1/4096 Ω | 0 … 15.99976 Ω | `raw / 4096.0` Ω |
| Time | 5.625 s | 0 … 102.3984 h | `raw * 5.625` s |
| Special | per-register | — | — |

**Batt / PCKP are Special, NOT the Voltage type** — they use **0.3125 mV (312.5 µV) LSB** on a
20.48 V scale (datasheet §Batt Register + EC table `VBLSB`/`VPLSB` = 312.5 µV). This is the
"pack/Vbatt LSB differs" you asked about: **confirmed, 4× coarser than the per-cell LSB.**

---

## 2. Voltages (all at slave 0x36)

| Addr | Name | LSB/Scaling | Formula for human units | Notes |
|---|---|---|---|---|
| `0x01A` | **VCell** | 0.078125 mV | `V = raw * 0.078125 / 1000` | **Lowest** of all enabled cell channels. This is the fuel-gauge voltage input, NOT the pack voltage. |
| `0x019` | **AvgVCell** | 0.078125 mV | `V = raw * 0.078125 / 1000` | Filtered VCell, 12 s…24 min window (nFilterCfg.VOLT) |
| `0x0D8` | **Cell1** | 0.078125 mV | `V = raw * 0.078125 / 1000` | Cell between CSP/GND and CELL1 (bottom cell) |
| `0x0D7` | **Cell2** | 0.078125 mV | `V = raw * 0.078125 / 1000` | Cell between CELL1 and CELL2 |
| `0x0D6` | Cell3 | 0.078125 mV | same | 3S/4S only |
| `0x0D5` | Cell4 | 0.078125 mV | same | 4S only (measured at BATTS) |
| `0x0D4` | **AvgCell1** | 0.078125 mV | `V = raw * 0.078125 / 1000` | 8-sample average of Cell1 |
| `0x0D3` | **AvgCell2** | 0.078125 mV | same | 8-sample average of Cell2 |
| `0x0D2` | AvgCell3 | 0.078125 mV | same | |
| `0x0D1` | AvgCell4 | 0.078125 mV | same | |
| `0x0DA` | **Batt** | **0.3125 mV** | `V = raw * 0.3125 / 1000` | Total stack voltage measured **inside** the protector (BATTS pin). 20.48 V scale. Update rate 22.4 s unless `nPackCfg.BtPkEn = 1`. |
| `0x0DB` | **PCKP** | **0.3125 mV** | `V = raw * 0.3125 / 1000` | Voltage between **PACK+ and GND** — i.e. the system/pack-connector side, *outside* the FETs. Same update-rate caveat. |
| `0x008` | MaxMinVolt | 20 mV per byte, unsigned | `Vmax = (raw>>8) * 0.02`, `Vmin = (raw & 0xFF) * 0.02` | Max/min cell V since reset. Reset by writing `0x00FF`. |
| `0x0B2` | VRipple | 1.25 mV / 128 | `mV = raw * 1.25 / 128` | RMS ripple of VCell vs AvgVCell |
| `0x0FB` | VFOCV | 0.078125 mV | `V = raw * 0.078125 / 1000` | Voltage-fuel-gauge open-circuit voltage |

**"VBatt" = `Batt` (0x0DA). "VSys" — there is NO VSys register on the MAX17320.**
The closest readable equivalent is **PCKP (0x0DB)** = PACK+ to GND, which is what the
system actually sees. (`MinSysVoltage` at `0x0A8` is a *dynamic-power configuration input*,
not a measurement — do not display it as a system voltage.)
`Batt - PCKP` ≈ the drop across the two series FETs; if the FETs are open, PCKP collapses
(or sits at the charger voltage) while Batt stays at the stack voltage — a useful
cross-check on the FET state.

---

## 3. Current (slave 0x36)

| Addr | Name | LSB/Scaling | Formula @ Rsense = 5 mΩ | Notes |
|---|---|---|---|---|
| `0x01C` | **Current** | 1.5625 µV / Rsense, **int16** | `mA = (int16_t)raw * 1.5625 / 5.0` = `raw * 0.3125` | ADC across CSP–CSN, ±51.2 mV range → **±10.24 A** with 5 mΩ. Updated every **351 ms** in active mode. |
| `0x01D` | **AvgCurrent** | same | `mA = (int16_t)raw * 0.3125` | Filter 0.7 s … 6.4 h (nFilterCfg.nCurr) |
| `0x00A` | MaxMinCurr | 0.4 mV / Rsense per **int8** byte | `Imax_mA = (int8_t)(raw>>8) * 0.4/5.0*1000 = *80`; same for low byte | Max/min since reset. Reset by writing `0x807F`. |
| `0x0AE` | MinCurr | — | — | Dynamic-power related; not needed for basic monitoring |
| `0x0B1` | Power | 1.6 mW @ 5 mΩ | `mW = (int16_t)raw * 1.6` | Instantaneous |
| `0x0B3` | AvgPower | 1.6 mW @ 5 mΩ | `mW = (int16_t)raw * 1.6` | Filter in `Config2.POWR` |
| `0x04D` / `0x04E` | QH / QL | Capacity | 32-bit coulomb counter | |

### Sign convention — CONFIRMED

> nProtMiscTh.CurrDet description, datasheet p.86:
> *"It is a threshold to detect discharging and charging events. **If current > CurrDet then
> charging; if current < -CurrDet then discharging.**"*

* **Positive Current = CHARGING** (current flowing *into* the battery).
* **Negative Current = DISCHARGING**.
* Read as `int16_t` and sign-extend. Do **not** treat as unsigned.
* Full scale at 5 mΩ: `0x7FFF` → +10.239 A, `0x8000` → −10.24 A (values outside range saturate).

---

## 4. Gauge / fuel-gauge outputs (slave 0x36)

| Addr | Name | LSB/Scaling | Formula for human units | Notes |
|---|---|---|---|---|
| `0x006` | **RepSOC** | 1/256 % | `pct = raw / 256.0` | The register to show the user. High byte alone = integer %. |
| `0x005` | **RepCap** | 5.0 µVh/Rsense | `mAh = raw * 5.0 / 5.0` = `raw * 1.0` | Remaining capacity matching RepSOC |
| `0x010` | **FullCapRep** | 5.0 µVh/Rsense | `mAh = raw * 1.0` | Learned full capacity; RepSOC = RepCap/FullCapRep |
| `0x0FF` | **VFSOC** | 1/256 % | `pct = raw / 256.0` | Voltage-fuel-gauge SOC. Note: on nominally "RESERVED" page 0Fh but individually valid (datasheet: "some individual user registers are located on RESERVED memory pages"). Used internally for end-of-charge gating. |
| `0x007` | **Age** | 1/256 % | `pct = raw / 256.0` | `= 100% × FullCapNom / DesignCap` — battery health |
| `0x017` | **Cycles** | **25 % of a cycle** | `cycles = raw * 0.25` | Range 0…16383 cycles. (Datasheet Table 61; LSb is 25 % for the volatile `Cycles`; `nCycles` at 0x1A4 may use 25/50/100/200 % per `nNVCfg2.FibScl`.) |
| `0x011` | **TTE** | 5.625 s | `hours = raw * 5.625 / 3600` | Saturates at `0xFFFF` = 102.3 h. Only meaningful while discharging. |
| `0x020` | **TTF** | 5.625 s | `hours = raw * 5.625 / 3600` | Only meaningful while charging. |
| `0x01B` | **Temp** | 1/256 °C, **int16** | `°C = (int16_t)raw / 256.0` | Highest enabled thermistor, or die temp if thermistors disabled |
| `0x016` | **AvgTA** | 1/256 °C, int16 | `°C = (int16_t)raw / 256.0` | Average of Temp; 6 min…12 h window |
| `0x034` | **DieTemp** | 1/256 °C, int16 | `°C = (int16_t)raw / 256.0` | Internal die temp — used as the FET-temperature proxy for the DieHot fault |
| `0x040` | AvgDieTemp | 1/256 °C, int16 | same | 4-sample average |
| `0x009` | MaxMinTemp | 1 °C per **int8** byte | `Tmax = (int8_t)(raw>>8)`, `Tmin = (int8_t)(raw & 0xFF)` | Reset by writing `0x807F` |
| `0x01F` | AvCap | Capacity | `mAh = raw * 1.0` | |
| `0x00E` | AvSOC | 1/256 % | `raw / 256.0` | |
| `0x00D` | MixSOC | 1/256 % | `raw / 256.0` | |
| `0x02B` | MixCap | Capacity | `mAh = raw * 1.0` | |
| `0x035` | FullCap | Capacity | `mAh = raw * 1.0` | |
| `0x023` | FullCapNom | Capacity | `mAh = raw * 1.0` | |
| `0x018` | DesignCap | Capacity | `mAh = raw * 1.0` | |
| `0x014` | RCell | 1/4096 Ω | `Ω = raw / 4096.0` | Avg internal resistance per cell |
| `0x01E` | IChgTerm | Current | `mA = (int16_t)raw * 0.3125` | Charge-termination threshold; needed for the "full" test in §6 |
| `0x013` | FullSocThr | 1/256 % | `raw / 256.0` | End-of-charge SOC gate |
| `0x03A` | VEmpty | Special: VE 10 mV (D15:D7), VR 40 mV (D6:D0) | `VE_V = (raw>>7)*0.01`, `VR_V = (raw & 0x7F)*0.04` | |
| `0x03E` | Timer | 175.8 ms | `s = raw * 0.1758` | 0…3.2 h |
| `0x0BE` | TimerH | 3.2 h | `h = raw * 3.2` | 0…23.94 years — pack age |
| `0x021` | DevName | — | `Device = raw & 0x0F`, `Revision = raw >> 4` | 4209h / 420Ah / 420Bh |

Thermistor channels (slave **0x0B**, SMBus half):

| Addr | Name | LSB | Notes |
|---|---|---|---|
| `0x13A`/`0x139`/`0x138`/`0x137` | Temp1 / Temp2 / Temp3 / Temp4 | 1/256 °C int16 | Per-thermistor, only if enabled in nPackCfg.NThrms |
| `0x136`/`0x135`/`0x134`/`0x133` | AvgTemp1..4 | 1/256 °C int16 | 4-sample average |
| `0x16F` | LeakCurrRep | 1.5625 µV/16 per LSB → **0.3125 mA/16 = 19.53 µA** @ 5 mΩ, 15-bit unsigned left-justified | Internal self-discharge leakage estimate |

---

## 5. Status / fault registers — the important part

### 5.1 `Status` — 0x000, slave 0x36 (datasheet Table 45)

Initial value after POR: `0x0002`.

| Bit | Name | Meaning | Clearing |
|---|---|---|---|
| D15 | **PA** | **Protection Alert.** Set when any protection event occurs; details in `ProtAlrt` (0x0AF). | **Must be cleared by host.** Write `ProtAlrt = 0x0000` FIRST, then clear PA. |
| D14 | Smx | SOC above max `SAlrtTh` | Sticky if `Config.SS = 1`, else auto |
| D13 | Tmx | Temperature above max `TAlrtTh` | Sticky if `Config.TS = 1`, else auto |
| D12 | Vmx | VCell above max `VAlrtTh` | Sticky if `Config.VS = 1`, else auto |
| D11 | X | Don't care (undefined, may read 0 or 1) | — |
| D10 | Smn | SOC below min `SAlrtTh` | as Smx |
| D9 | Tmn | Temperature below min `TAlrtTh` | as Tmx |
| D8 | Vmn | VCell below min `VAlrtTh` | as Vmx |
| D7 | **dSOCi** | RepSOC crossed a 1 % integer boundary | **Host must clear** |
| D6 | Imx | Current above max `IAlrtTh` | **Auto-clears** when Current falls back below |
| D5 | X | Don't care | — |
| D4 | X | Don't care | — |
| D3 | X | Don't care | — |
| D2 | Imn | Current below min `IAlrtTh` | **Auto-clears** when Current rises back above |
| D1 | **POR** | Power-On Reset detected (HW or SW). Set to 1 at power-up. | **Host must clear** to detect the next POR |
| D0 | X | Don't care | — |

Mask the "X" bits off before comparing/logging: `status & 0xF7C6` are the defined bits
(D15,D14,D13,D12,D10,D9,D8,D7,D6,D2,D1).

### 5.2 `ProtStatus` — 0x0D9, slave 0x36 (datasheet Table 49)

**Live state of the protection state machine.** Bits reflect the *present* latched fault state
of the protector — they clear themselves when the fault condition is released by the
protector's state machine. **Read-only for monitoring; do not write.**

| Bit | Name | Class | Meaning |
|---|---|---|---|
| D15 | **ChgWDT** | Charge fault | Charge communication watchdog timer expired |
| D14 | **TooHotC** | Charge fault | Overtemperature for charging |
| D13 | **Full** | Charge "fault" | Full detection — charging stopped because pack is full (**not an error**) |
| D12 | **TooColdC** | Charge fault | Undertemperature for charging |
| D11 | **OVP** | Charge fault | Cell overvoltage |
| D10 | **OCCP** | Charge fault | Overcharge current |
| D9 | **Qovflw** | Charge fault | Capacity overflow (charger pushed more than DesignCap × coeff) |
| D8 | **PreqF** | Charge fault | Prequal (precharge) timeout |
| D7 | **Imbalance** | Charge fault | Multi-cell imbalance |
| D6 | **PermFail** | Both | **Permanent failure detected** — see §5.5 |
| D5 | **DieHot** | Both | Die (FET-proxy) overtemperature |
| D4 | **TooHotD** | Discharge fault | Overtemperature for discharging |
| D3 | **UVP** | Discharge fault | Cell undervoltage |
| D2 | **ODCP** | Discharge fault | Overdischarge current |
| D1 | **ResDFault** | ? | Present in the bit table but **NOT described anywhere in the datasheet**. Treat as reserved/unknown. **UNCONFIRMED meaning.** (All three driver repos also flag it as undocumented.) |
| D0 | **Ship** | State flag | Device is in Ship state (FETs open, waiting for wake-up). Not a fault. |

`ProtStatus == 0x0000` → protector healthy and not in ship state.
Practical "protection tripped" test: `(ProtStatus & 0xFEFE) != 0` (everything except Ship and ResDFault),
or exclude `Full` too if you don't want normal end-of-charge flagged as a fault:
`(ProtStatus & 0xDEFE) != 0`.

### 5.3 `ProtAlrt` (a.k.a. ProtAlrts) — 0x0AF, slave 0x36 (datasheet Table 50)

**Sticky history** of protection events. Mirrors `ProtStatus` bit-for-bit **except D0**.
Once a bit sets, it stays set until the host writes the register to `0x0000`.
If any bit is 1 and `Config.PAen = 1`, then `Status.PA` is also 1 (and the ALRT pin is
driven low if `Config.Aen = 1` too).

| Bit | Name | Meaning |
|---|---|---|
| D15 | ChgWDT | as ProtStatus |
| D14 | TooHotC | as ProtStatus |
| D13 | Full | as ProtStatus |
| D12 | TooColdC | as ProtStatus |
| D11 | OVP | as ProtStatus |
| D10 | OCCP | as ProtStatus |
| D9 | Qovflw | as ProtStatus |
| D8 | PreqF | as ProtStatus |
| D7 | Imbalance | as ProtStatus |
| D6 | PermFail | as ProtStatus |
| D5 | DieHot | as ProtStatus |
| D4 | TooHotD | as ProtStatus |
| D3 | UVP | as ProtStatus |
| D2 | ODCP | as ProtStatus |
| D1 | ResDFault | **UNCONFIRMED** (undocumented, same as ProtStatus D1) |
| D0 | **LDet** | **Leakage Detection fault** — internal self-discharge detected. *This is the only bit that differs from ProtStatus (which has `Ship` at D0).* |

### 5.4 `HProtCfg2` — 0x0F1, slave 0x36 (datasheet Table 51) — **FET STATE LIVES HERE**

POR value `0x0000`. Bit positions verified against the datasheet table geometry.

| Bit(s) | Name | Meaning |
|---|---|---|
| D15:D14 | **AOLDO** | Always-on LDO config: `00b` disabled, `01b` enabled 3.4 V, `10b` enabled 1.8 V, `11b` enabled 3.4 V |
| D13:D9 | (unlabelled / reserved) | — |
| D8 | **CommOvrd** | Command Override Enable. 1 = `CommStat.CHGOff`/`DISOff` are allowed to force FETs off. |
| D7 | (unlabelled) | — |
| D6:D5 | **CPCfg** | Charge-pump gate drive: `00b` = 6 V, `01b` = 8 V, `10b` = 10 V |
| D4 | (unlabelled) | — |
| D3 | **PBEN** | Pushbutton wake-up on ALRT enabled |
| D2 | (unlabelled) | — |
| D1 | **DISs** | **DIS FET Status: 1 = ON, 0 = OFF** |
| D0 | **CHGs** | **CHG FET Status: 1 = ON, 0 = OFF** |

```
chg_fet_on = (HProtCfg2 >> 0) & 1;
dis_fet_on = (HProtCfg2 >> 1) & 1;
```

This is the **only** register that reports the *live* FET state. `CommStat.CHGOff/DISOff`
report only the *host override request*, not the actual FET state.

### 5.5 Permanent-fail state: `nBattStatus` — 0x1A8, slave **0x0B** (offset 0xA8) (Table 47)

Permanent failure is **latched into NVM** and both FETs stay off across power cycles.
`ProtStatus.PermFail` (D6) tells you *that* it happened; `nBattStatus` tells you *why*.

| Bit | Name | Meaning |
|---|---|---|
| D15 | **PermFail** | Any permanent failure detected |
| D14 | OVPF | Severe overvoltage permanent failure (cell > `nOVPrtTh.OVPPermFail`) |
| D13 | OTPF | Severe overtemperature permanent failure (Temp > `nTPrtTh3.TpermFailHot`) |
| D12 | CFETFs | **Charge FET failure — SHORTED** (cannot be opened) |
| D11 | DFETFs | **Discharge FET failure — SHORTED** |
| D10 | FETFo | FET failure — OPEN (either FET) |
| D9 | LDet | Leakage detection fault |
| D8 | ChksumF / UVPF | Protection-NVM checksum failure, **or** undervoltage permanent failure (VCell < UV perm-fail threshold). Datasheet labels this single bit with both names. |
| D7:D0 | LeakCurr | 8-bit unsigned leakage current, LSB **3.125 µV = 0.625 mA @ 5 mΩ**, range 0–159.375 mA |

Reading `nBattStatus` is a plain word read at slave 0x0B, offset 0xA8 — **read-only, safe**.
When permanent failure trips, the **PFAIL pin is driven high** (blows the fuse / latches the
secondary protector), so recovery may be physically impossible.

`nFaultLog` — 0x1AE, slave 0x0B offset 0xAE — is a *history* of protection events **only if
`nNVCfg2.enFL = 1`** (otherwise it's Age-Forecast data or user memory). Format:
D7 TooHotC, D6 TooColdC, D5 OVP, D4 OCCP, D3 DieHot, D2 Imbalance, D1 UVP, D0 ODCP;
D15:D8 reserved. Verify `nNVCfg2.enFL` before displaying it.

### 5.6 `CommStat` — 0x061, slave 0x36 (datasheet Table 102)

| Bit | Name | Meaning |
|---|---|---|
| D15:D10 | X | Don't care |
| D9 | **DISOff** | Host request: force DIS FET off (only effective if `nProtCfg.CmOvrdEn = 1`) |
| D8 | **CHGOff** | Host request: force CHG FET off (only effective if `nProtCfg.CmOvrdEn = 1`) |
| D7 | WP5 | Write-protects page 1Dh |
| D6 | WP4 | Write-protects page 1Ch |
| D5 | WP3 | Write-protects pages 18h, 19h |
| D4 | WP2 | Write-protects pages 01h,02h,03h,04h,0Bh,0Dh |
| D3 | WP1 | Write-protects pages 1Ah, 1Bh, 1Eh |
| D2 | **NVError** | Last NVM/SHA command failed (or Full Reset was executed). Host must clear. |
| D1 | **NVBusy** | Read-only: NVM operation in progress |
| D0 | **WPGlobal** | Global write protect. 1 = all pages protected. |

Normal locked-down value is `0x00F9` (WPGlobal + WP1..WP5). `0x0000` = fully unlocked.
For a read-only dashboard: **read it, never write it.**

### 5.7 `Config` — 0x00B, slave 0x36 (datasheet Table 68)

| Bit | Name | Meaning |
|---|---|---|
| D15 | 0 | Must be written 0 |
| D14 | SS | SOC alert sticky |
| D13 | TS | Temperature alert sticky |
| D12 | VS | Voltage alert sticky |
| D11 | **DisLDO** | 1 = disable the AOLDO even if enabled in `nPackCfg.AOCfg` |
| D10 | PBen | Pushbutton wake-up enable |
| D9 | DisBlockRead | 1 = normal (non-block) reads in the 16h space. Default 1. |
| D8 | 0 | Must be 0 |
| D7 | **SHIP** | Write 1 to force ship/deepship (**both FETs open within 1.4 s**). Reads 0 normally. |
| D6 | **COMMSH** | 1 = enter shutdown if SDA **and** SCL are both held low longer than the shutdown timer. **Relevant to your STM32 wiring — see §9.** |
| D5 | 0 | Must be 0 |
| D4 | ETHRM | Enable automatic thermistor bias + measurement |
| D3 | FTHRM | Force thermistor bias always on (+~200 µA) |
| D2 | **Aen** | Enable ALRT pin assertion on fuel-gauge alerts |
| D1 | 0 | Must be 0 |
| D0 | **PAen** | Protection Alert Enable — gates `Status.PA` / protector-fault logging into `nBattStatus` |

### 5.8 `Config2` — 0x0AB, slave 0x36 (datasheet Table 69)

| Bit | Name | Meaning |
|---|---|---|
| D15 | **POR_CMD** | Write 1 = firmware restart (no NVM recall). Self-clearing. **Do not write during monitoring.** |
| D14 | 0 | Must be 0 |
| D13 | AtRtEn | AtRate engine enable |
| D12 | ADCFIFOen | Single-cycle ADC FIFO acquisition |
| D11:D8 | POWR | AvgPower time constant: `τ = 45 s × 2^(POWR-6)`; POR 0000b → 0.7 s |
| D7 | dSOCen | Enable `Status.dSOCi` |
| D6 | TAlrten | Enable temperature alerts |
| D5 | 0 | Must be 0 |
| D4 | 1 | Must be 1 |
| D3:D2 | DRCfg | Deep-relax time: 00b 0.8–1.6 h, 01b 1.6–3.2 h, 10b 3.2–6.4 h, 11b 6.4–12.8 h |
| D1:D0 | 0 | Must be 0 |

### 5.9 Other status registers

| Addr | Name | Bits |
|---|---|---|
| `0x0B0` | **Status2** | D1 = **Hib** (1 = hibernate mode, 0 = active). All other bits don't-care. |
| `0x03D` | **FStat** | D9 RelDt (cell relaxed), D8 EDet (empty detected), D6 RelDt2 (long relaxation 48–96 min), D0 **DNR** (Data Not Ready — gauge outputs not yet valid after cell insertion; clears after 445 ms–1.845 s). Datasheet: **"Do not write to this register location."** |
| `0x07F` | Lock | D4..D0 = LOCK5..LOCK1 state (1 = permanently locked) |
| `0x0BB` | FOTPStat | D15:D12 = PatchID; rest don't-care |

**`ChgStat` does not exist on the MAX17320.** (It exists on other Maxim gauges; there is no
such register in this memory map.) **`FProtStat` does not exist either.** The equivalents are:
live protector state = `ProtStatus` (0x0D9), sticky history = `ProtAlrt` (0x0AF),
permanent-fail latch = `nBattStatus` (0x1A8), live FET state = `HProtCfg2` (0x0F1).

---

## 6. Charging control readback + charging / discharging / idle / full

| Addr | Name | LSB/Scaling | Formula @ 5 mΩ | Notes |
|---|---|---|---|---|
| `0x028` | **ChargingCurrent** | Current type: 1.5625 µV/Rsense | `mA = (int16_t)raw * 0.3125` | **Prescribed** charge current the external charger should use. Reflects the active JEITA zone × step-charging stage × prequal state. |
| `0x02A` | **ChargingVoltage** | Voltage type: 0.078125 mV | `V = raw * 0.078125 / 1000` | **Prescribed** per-cell charge voltage for the active JEITA zone. |

These are **outputs** of the IC, not measurements — they tell the charger what to do. They
change automatically with temperature (6-zone JEITA from `nJEITAV`/`nJEITAC`) and with the
step-charging state machine (`nStepChg`, 0x1DB).

### JEITA / step-charging state readback

* **There is no documented register that reports the current JEITA zone number.**
  Infer it: read `Temp` (0x01B) and compare against `nTPrtTh1` (0x1D1), `nTPrtTh2` (0x1D5),
  `nTPrtTh3` (0x1D2); or simpler, watch `ChargingVoltage`/`ChargingCurrent` change and map
  them back to the `nJEITAV`/`nJEITAC` fields.
* Figure 10 of the datasheet labels the step-charging state machine output as
  **`ProtTmrStat.ChargeStep`**, but **no `ProtTmrStat` register is defined anywhere in Rev 12
  of the datasheet** — no address, no bit table, and it is not in Table 92. **UNCONFIRMED /
  undocumented; do not rely on it.** Derive the step instead: compare `ChargingCurrent`
  against `nJEITAC.RoomChargingCurrent` scaled by `(nStepChg.StepCurr1+1)/16` and
  `(nStepChg.StepCurr2+1)/16`.
* `nStepChg` (0x1DB, slave 0x0B offset 0xDB), factory default `C884h`, format:
  D15:D12 StepCurr1, D11:D8 StepCurr2, D7:D4 StepdV0, D3:D0 StepdV1.
  `StepV0 = ChargingVoltage_JEITA − StepdV0 × 10 mV`, `StepV1 = ChargingVoltage_JEITA − StepdV1 × 10 mV`.
  Set `nStepChg = FF00h` disables step charging.

### Deciding charging / discharging / idle / full — recommended logic

Threshold source: `nProtMiscTh` (0x1D6, slave 0x0B offset 0xD6), factory default `7A28h`.
Field layout (verified against the datasheet table geometry and cross-checked against the
stated "normally configured to a setting of 2 → 15 mA" text):

| Bits | Field | Decode |
|---|---|---|
| D15:D12 | QovflwTh | `coeff = 1.0625 + QovflwTh × 0.0625` |
| D11:D8 | TooHotDischarge | 2 °C LSB above `nTPrtTh1.T4` |
| D7:D4 | **CurrDet** | `threshold_mA = (CurrDet + 1) × 5 mA` @ 5 mΩ |
| D3:D0 | DieTempTh | `50 °C + DieTempTh × 5 °C` |

Default `7A28h` → CurrDet = 2 → **±15 mA** deadband.

```c
int32_t i_ma      = current_ma;          // signed, +ve = charging
int32_t i_avg_ma  = avg_current_ma;      // signed
int32_t det_ma    = 15;                  // (nProtMiscTh.CurrDet + 1) * 5

if      (i_ma >  det_ma)  state = CHARGING;
else if (i_ma < -det_ma)  state = DISCHARGING;
else                      state = IDLE;

// "Full" — the IC's own end-of-charge criterion (datasheet, nIChgTerm section):
bool full = (vfsoc > fullsocthr)
         && (i_ma     > ichgterm_ma * 0.125) && (i_ma     < ichgterm_ma * 1.25)
         && (i_avg_ma > ichgterm_ma * 0.125) && (i_avg_ma < ichgterm_ma * 1.25);

// Or, simpler and authoritative: the protector's own "Full" latch
bool full_latched = (protstatus >> 13) & 1;   // ProtStatus.Full
```

Cross-checks worth showing on the dashboard:
* `ProtStatus.Full` (D13) set → protector stopped charging because the pack is full.
* `HProtCfg2.CHGs = 0` while a charger is present (`PCKP > Batt`) → charging is blocked, look at `ProtStatus`.
* `HProtCfg2.DISs = 0` → discharge blocked (UVP / ODCP / TooHotD / ship / permfail).
* RepSOC ≈ 100 % and `Current` inside the IChgTerm band → idle/full.
* Note the IC's "ideal diode" behaviour: during a *charge* fault the CHG FET is deliberately
  turned **on** when a discharge is detected (to avoid body-diode heating), then off again
  when a charger reappears. So `CHGs = 1` does **not** by itself mean "no charge fault" —
  always read `ProtStatus` too.

---

## 7. Writes required to clear alerts, and the safe way to do it

Everything below is the *only* set of writes a monitoring dashboard should ever perform.
All of them are on slave 0x36 and none touch NVM.

### 7.1 Clearing a protection alert (`Status.PA`)

Datasheet, `Status.PA` description and Alert Function section:
> *"This bit must be cleared by system software… **However, prior to clearing this bit, the
> ProtAlrts register must first be written to 0x0000.**"*

Order matters:

```c
1. read  ProtAlrt (0x0AF)          // capture what happened, for the log
2. write ProtAlrt (0x0AF) = 0x0000 // MUST be first
3. read  Status   (0x000)
4. write Status   (0x000) = status_read & ~(1 << 15)   // clear PA only
```

### 7.2 Clearing fuel-gauge alert bits in `Status` (0x000)

Read-modify-write, clearing **only** the bits you observed as set. The standard, safe idiom
used across all Maxim ModelGauge parts:

```c
uint16_t s = read(0x000);
write(0x000, s & ~bits_to_clear);   // never write 0x0000 blindly
```

* `POR` (D1) and `dSOCi` (D7) **must** be host-cleared.
* `Vmn/Vmx`, `Tmn/Tmx`, `Smn/Smx` are host-cleared only if `Config.VS/TS/SS = 1`; otherwise
  they self-clear and writing them is unnecessary.
* `Imn` (D2) and `Imx` (D6) **always** self-clear — do not try to clear them.
* Do not write 1s to bits that are currently 0 (you can re-arm/spuriously set flags).
* Do not write the don't-care bits D11, D5, D4, D3, D0 to anything other than what you read.

**Write protection:** `Status` (page 00h) and `ProtAlrt` (page 0Ah) are **not** covered by
WP1–WP5 (which cover pages 01h–04h, 0Bh, 0Dh, 18h–1Eh). Page 00h is explicitly exempt from
LOCK2 as well. In practice `Status`/`ProtAlrt` clearing works with write protection left in
its normal `0x00F9` state — no unlock sequence, no NVM writes. If a write is silently
rejected, verify by reading back rather than by unlocking.

### 7.3 Registers a monitoring dashboard must NOT touch

| Register | Addr | Why |
|---|---|---|
| **Command** | `0x060` | Triggers resets, NVM block copies, NVM/history recalls, SHA ops, permanent LOCK. `0xE904` = block copy (burns one of only **7** NVM writes). `0x000F` = full reset. `0x6Axx` = **permanent, irreversible** lock. |
| **CommStat** | `0x061` | Writing it twice in a row changes write protection; `CHGOff`/`DISOff` (D9/D8) can physically open the FETs and kill system power. |
| **All NVM / shadow-RAM** | `0x180–0x1FF` (slave 0x0B, offsets 0x80–0xFF) | User-accessible NVM is limited to **7 writes total, ever**. Page 1Ah is limited to 100. A block copy is irreversible once spent. Read freely; never write. |
| **Config** | `0x00B` | `SHIP` (D7) opens both FETs within 1.4 s; `COMMSH` (D6) can cause bus-collapse shutdown; `DisLDO` (D11) kills the AOLDO rail. |
| **Config2** | `0x0AB` | `POR_CMD` (D15) restarts firmware — you will lose gauge state and see a POR. |
| **FStat** | `0x03D` | Datasheet: *"Do not write to this register location."* |
| **ProtStatus** | `0x0D9` | Live protector state; it is not a host-clearable latch (that's `ProtAlrt`). Read-only. |
| **HProtCfg2** | `0x0F1` | Contains AOLDO/CPCfg config alongside the FET status bits. Read-only. |
| **Lock** | `0x07F` | Read-only status of permanent locks. |
| **Any RESERVED location** | pages 05h–09h, 0Eh–0Fh (except the individually-valid 0F1h/0FBh/0FFh), grey cells of Table 92 | Datasheet: *"Data read from RESERVED locations is not defined"* and *"should never be written to."* |
| **`AtRate` (0x004)** | | Writable, but writing it perturbs the AtQResidual/AtTTE/AtAvSOC/AtAvCap outputs. Leave alone unless you're deliberately using AtRate. |
| **`MaxMinVolt`/`MaxMinCurr`/`MaxMinTemp`** | 0x008/0x00A/0x009 | Writing the magic reset values (`0x00FF`/`0x807F`/`0x807F`) is legal and harmless, but destroys the min/max log. Only do it deliberately. |

---

## 8. Suggested read-only polling set (one pass, all slave 0x36 unless noted)

Everything updates at the 351.5 ms task period, so 1 Hz polling is plenty.
Batt/PCKP update every 22.4 s unless `nPackCfg.BtPkEn = 1`.

```
0x000 Status          0x006 RepSOC        0x0D8 Cell1        0x01C Current
0x0AF ProtAlrt        0x005 RepCap        0x0D7 Cell2        0x01D AvgCurrent
0x0D9 ProtStatus      0x010 FullCapRep    0x0D4 AvgCell1     0x0DA Batt
0x0F1 HProtCfg2       0x0FF VFSOC         0x0D3 AvgCell2     0x0DB PCKP
0x0B0 Status2         0x007 Age           0x01A VCell        0x01B Temp
0x03D FStat           0x017 Cycles        0x019 AvgVCell     0x016 AvgTA
0x061 CommStat        0x011 TTE           0x028 ChargingCurrent   0x034 DieTemp
0x00B Config          0x020 TTF           0x02A ChargingVoltage   0x0B1 Power
0x0AB Config2         0x01E IChgTerm      0x013 FullSocThr        0x021 DevName

slave 0x0B: 0xA8 -> nBattStatus (0x1A8)   [permanent-fail detail, poll slowly]
slave 0x0B: 0xD6 -> nProtMiscTh (0x1D6)   [read ONCE at boot for CurrDet]
```

Known datasheet inconsistency (harmless): the `AtAvSOC` section header says **0CEh** while
Table 92 places AtAvSOC at page 0Dh word Eh = **0DEh**. Page 0Ch is SHA memory, so **0x0DE
is almost certainly correct**. Not needed for monitoring. **UNCONFIRMED which is right.**

---

## 9. Hardware questions

### 9.1 AOLDO pin

**Pin 12 (24-TQFN) / D5 (30-WLP), name `AOLDO` = "Always-On LDO".**

* Datasheet Pin Description: *"Always-On LDO. Configurable as 3.4V or 1.8V. Bypass to GND
  with a 0.47 µF/10 V ceramic capacitor. Leave disconnected or connect to GND with a 10 kΩ
  resistor if not used."*
* **Regulated output: 3.4 V or 1.8 V, selected by `nPackCfg.AOCfg` (0x1B5, D15:D14):**
  `00b` = disabled, `01b` = **3.4 V**, `10b` = **1.8 V**, `11b` = 3.4 V.
  Live/effective setting is readable at `HProtCfg2` (0x0F1) D15:D14.
* Electrical Characteristics, `VAOLDO`:
  * **1.8 V output, I_LOAD = 2 mA: min 1.62 V, typ 1.80 V, max 1.98 V**
  * **3.4 V output, I_LOAD = 2 mA: min 3.00 V, typ 3.40 V, max 3.80 V**
* **Load capability: ~2 mA.** Datasheet: *"a configurable always-on LDO (1.8 V or 3.4 V) that
  can power small critical loads (**less than 2 mA**) inside the battery or on the system side
  without overloading the cells even under fault conditions."* Intended for an RTC or a
  keep-alive supervisor — **not** for powering an MCU, and **not** an adequate I2C pull-up
  supply if you want fast edges at 400 kHz with 400 pF of bus capacitance.
* It is "always on" in the sense that it survives protection events / FET-open states.
  It can be force-disabled at runtime with `Config.DisLDO` (0x00B D11).
* Absolute max: **REG3, AOLDO to GND = -0.3 V to +6 V.**
* Distinct from `REG3` (pin 13, internal 3.4 V regulator) and `REG2` (pin 17, internal 1.8 V).

### 9.2 SDA/SCL absolute maximum ratings and logic thresholds — **3.3 V is safe**

Direct quotes:

* **Absolute Maximum Ratings:** `SCL, SDA, ALRT to GND ....... **-0.3V to +20V**`
* **Electrical Characteristics, INPUT / OUTPUT:**
  * `Input Logic High, SCL/OD, SDA/DQ, ALRT — **VIH — MIN 1.5 V**` (no max listed; the 20 V abs-max governs)
  * `Input Logic Low, SCL/OD, SDA/DQ, ALRT — **VIL — MAX 0.44 V**`
  * `Output Drive Low, ALRT, SDA/DQ, PFAIL — **VOL — MAX 0.4 V** at IOL = 4 mA, REG3 = 3.4 V`
  * `SCL, SDA Input Capacitance — CBIN — typ 6 pF`
  * `Capacitive Load for Each Bus Line — CB — max 400 pF`
  * `Communication Removal Test Current — IPD — SDA, SCL pin = 0.4 V — 0.05 / 0.2 / 0.4 µA`
    (SDA and SCL have a weak **internal pulldown** used to sense pack disconnection)

**Answer: yes, it is electrically safe to pull SDA/SCL up to 3.3 V from an STM32 3.3 V rail.**
No level shifter and no AOLDO-referenced pull-up are required.

* 3.3 V is 16.7 V below the 20 V absolute-maximum on those pins — enormous margin. The pins
  are explicitly rated to +20 V precisely so they can survive being shorted to PACK+ on a
  battery connector.
* VIH min = 1.5 V, so a 3.3 V high is recognised with >1.8 V of margin.
* VIL max = 0.44 V, and the STM32's push-pull/open-drain low is well under that (and the
  MAX17320's own VOL is ≤ 0.4 V at 4 mA), so both directions meet the low threshold.
* The MAX17320's SDA is open-drain and it never drives high — it cannot back-drive 3.4 V
  into the STM32. The STM32's 3.3 V-tolerant inputs see at most 3.3 V from your own pull-ups.
* **Do not** pull up to AOLDO if AOLDO is configured for 3.4 V and your STM32 pin is
  *not* 5 V-tolerant: 3.4 V typ / **3.8 V max** exceeds VDD+0.3 V on a 3.3 V STM32 I/O and
  would forward-bias the ESD diode. Pulling up to your own 3.3 V rail is strictly better.
* Also do not pull up to AOLDO for load reasons — AOLDO is a ~2 mA regulator; two 2.2 kΩ
  pull-ups alone would draw ~3 mA.
* Recommended: 2.2 kΩ–4.7 kΩ to the STM32's 3.3 V, keeping total bus capacitance ≤ 400 pF
  and rise time within the 300 ns spec.

**One real caveat, not a voltage one:** `Config.COMMSH` (0x00B, D6). If COMMSH = 1 and
**both** SDA and SCL are held low longer than the shutdown timer (`nDelayCfg.UVPTimer`), the
IC enters Ship/DeepShip — **both FETs open** and the pack disconnects. An unpowered STM32
with pull-ups on the MAX17320 side, or a bus held low by a hung transaction, can trigger this.
There is also a **30 ms SCL-low timeout** in the EC table. Read `Config` at startup and check
D6; if COMMSH = 1 be careful never to leave the bus parked low. (Recovery is by a
high-to-low edge on a comm line, charger detect, or pushbutton — but it is a nasty surprise
on a bench setup.)

### 9.3 Max I2C clock frequency

**400 kHz.** Electrical Characteristics, 2-WIRE INTERFACE (I2C and SMBus):
`SCL Clock Frequency — fSCL — MAX 400 kHz`, and §2-Wire Bus System:
*"SDA and SCL provide bidirectional communication between the IC and a master device at
speeds up to 400kHz"* / *"The IC is compatible with any bus timing up to 400kHz… No special
configuration is required to operate at any speed."*

Supporting timing (all mins): tLOW 1.3 µs, tHIGH 0.6 µs, tBUF 1.3 µs, tSU:DAT 100 ns,
tR/tF 5–300 ns, tSP 50 ns spike suppression, SCL low timeout 30 ms.
**Note 5:** *"Timing must be fast enough to prevent the IC from entering shutdown mode due to
bus being low for a period greater than the shutdown timer setting."*

### 9.4 Where is MAX17320 GND referenced? (STM32 Nucleo grounding)

**The MAX17320 is a HIGH-SIDE protector.** Both protection FETs are N-channel and sit in the
**positive** path:

```
 cell stack + ── IN ──[CHG FET]──[DIS FET]── PCKP ── PACK+
 cell stack − ── CSP ──[ RSENSE 5 mΩ ]── CSN ───────  PACK−
                 │
                GND (device ground, its own trace)
```

* Pin 15 **CSP** — *"Device Ground and Current Measurement Positive Sense Point. Kelvin connect
  to **cell side** of the sense resistor."*
* Pin 14 **CSN** — *"**System Ground** and Current Measurement Negative Sense Point. Kelvin
  connect to **load side** of the sense resistor."*  → CSN is the PACK− node.
* Pin 20 **GND** — *"Ground Pin. Connect to ground. **Do not share the ground trace with the
  CSP Kelvin-sense trace.**"*
* EC table **Note 1: "All voltages are referenced to CSP."**

**Consequences for connecting an externally-powered STM32 Nucleo:**

1. **The ground path is never broken.** Because the FETs are high-side, PACK− is permanently
   tied to cell-stack negative through the 5 mΩ sense resistor. Even in permanent-fail, ship
   mode, or a tripped protection state, **battery negative and pack negative stay connected**
   (they differ only by I × 5 mΩ — 50 mV at 10 A, sub-millivolt at idle). This is the opposite
   of a low-side protector, where opening the FETs disconnects the ground and makes a shared
   host ground dangerous. Here, tying Nucleo GND to the board's PACK− is inherently safe.
2. **Land the Nucleo ground on CSN / PACK−, not on CSP or the GND pin's Kelvin trace.** Return
   current from an externally-powered host, plus the I2C pull-up current, would otherwise flow
   through the current-sense Kelvin trace and corrupt every `Current`, `AvgCurrent`, `RepCap`
   and coulomb-count reading. At 5 mΩ, a 1 LSB current error is 1.5625 µV — a few milliamps
   of stray return current through the wrong copper is a real, visible offset. The datasheet
   makes this explicit for the GND pin, and it applies with double force to an external host.
3. **What is dangerous is PACK+ (not ground).** With high-side FETs, PACK+ is the node that
   disappears when the protector trips or ship mode is entered. Anything you power *from*
   PACK+ will drop out; ground stays. So don't try to power the Nucleo from PACK+ and expect
   it to survive a protection event.
4. **Watch for a second ground path through the Nucleo's USB/PC.** If the Nucleo is USB-powered
   from a mains-referenced PC and you also bond its GND to the pack, you create a loop through
   PC earth. Use an isolated supply or a USB isolator if the pack is simultaneously connected
   to a mains-powered charger or an electronic load — otherwise charger return current can find
   its way through your sense resistor or your ground wire.
5. **Sequencing / hot-plug.** Connect the ground wire **first** and remove it **last**. SDA and
   SCL tolerate 20 V so they will survive a floating-ground hot-plug, but the ADC references
   everything to CSP; a floating ground during connection produces garbage readings and can
   momentarily pull both comm lines low (see the COMMSH warning in §9.2).
6. If you also connect **ALRT**, remember it is open-drain and rated to 20 V — a 3.3 V pull-up
   into an STM32 EXTI pin is fine — but note that driving ALRT low is *also* the "ALRT pin
   FET override" (forces both FETs off) when `nProtCfg.OvrdEn = 1`, and the pushbutton wake-up
   input when `nConfig.PBen = 1`. Configure it as an input only.

---

## 10. Quick reference — address cheat sheet

```
slave 0x36 (6Ch), 8-bit offsets == internal 0x000-0x0FF
  000 Status        005 RepCap       006 RepSOC     007 Age       008 MaxMinVolt
  009 MaxMinTemp    00A MaxMinCurr   00B Config     00D MixSOC    00E AvSOC
  010 FullCapRep    011 TTE          013 FullSocThr 014 RCell     016 AvgTA
  017 Cycles        018 DesignCap    019 AvgVCell   01A VCell     01B Temp
  01C Current       01D AvgCurrent   01E IChgTerm   01F AvCap
  020 TTF           021 DevName      023 FullCapNom 028 ChargingCurrent
  02A ChargingVoltage                02B MixCap
  034 DieTemp       035 FullCap      03A VEmpty     03D FStat     03E Timer
  040 AvgDieTemp    04A VFRemCap     04D QH         04E QL
  060 Command(!)    061 CommStat(!)  07F Lock
  0AB Config2       0AC IAlrtTh      0AF ProtAlrt   0B0 Status2   0B1 Power
  0B2 VRipple       0B3 AvgPower     0BB FOTPStat   0BE TimerH
  0D1 AvgCell4  0D2 AvgCell3  0D3 AvgCell2  0D4 AvgCell1
  0D5 Cell4     0D6 Cell3     0D7 Cell2     0D8 Cell1
  0D9 ProtStatus    0DA Batt         0DB PCKP       0DD AtTTE     0DE AtAvSOC
  0F1 HProtCfg2 (CHG/DIS FET status) 0FB VFOCV      0FF VFSOC

slave 0x0B (16h), offsets 0x00-0x7F == internal 0x100-0x17F (SMBus half)
  133-136 AvgTemp4..AvgTemp1        137-13A Temp4..Temp1        16F LeakCurrRep

slave 0x0B (16h), offsets 0x80-0xFF == internal 0x180-0x1FF (NVM shadow RAM - READ ONLY!)
  1A4 nCycles     1A8 nBattStatus(permfail)  1AE nFaultLog   1AF nTimerH
  1B0 nConfig     1B5 nPackCfg(AOLDO cfg)    1B8-1BA nNVCfg0/1/2
  1C6 nFullSOCThr 1C8 nCGain    1CA nThermCfg
  1D0 nUVPrtTh    1D1 nTPrtTh1  1D2 nTPrtTh3  1D3 nIPrtTh1  1D4 nBALTh
  1D5 nTPrtTh2    1D6 nProtMiscTh(CurrDet)   1D7 nProtCfg
  1D8 nJEITAC     1D9 nJEITAV   1DA nOVPrtTh  1DB nStepChg   1DC nDelayCfg
  1DE nODSCCfg    1DF nProtCfg2 1CF nRSense
```

`(!)` = never write from a monitoring dashboard.

---

## 11. Items explicitly NOT confirmed

| Item | Status |
|---|---|
| `ProtStatus.ResDFault` / `ProtAlrt.ResDFault` (D1) meaning | **UNCONFIRMED** — present in the datasheet bit table, described nowhere in Rev 12. All three driver repos also note it as undocumented. |
| `ProtTmrStat` register (JEITA/step-charge state readback) | **UNCONFIRMED** — referenced only as a label in Figure 10; no address, no bit table, absent from Table 92. Do not attempt to read it. |
| A readable "current JEITA zone" register | **Does not appear to exist.** Infer from Temp + ChargingVoltage/ChargingCurrent. |
| `ChgStat` register | **Does not exist on MAX17320.** |
| `FProtStat` register | **Does not exist on MAX17320.** Use ProtStatus / ProtAlrt / nBattStatus / HProtCfg2. |
| `VSys` register | **Does not exist.** Use `PCKP` (0x0DB). `MinSysVoltage` (0x0A8) is a config input, not a measurement. |
| `AtAvSOC` address 0x0CE vs 0x0DE | **UNCONFIRMED** — datasheet self-contradicts (section header says 0CEh, Table 92 says page 0Dh word Eh = 0DEh; page 0Ch is SHA memory so 0DEh is the likely truth). Irrelevant for monitoring. |
| HProtCfg2 bits D13:D9, D7, D4, D2 | Unlabelled in Table 51 — treat as reserved, mask them off. |
| Exact `Power`/`AvgPower` LSB for Rsense ≠ 5 mΩ | Datasheet only states "1.6 mW with a 5 mΩ sense resistor". Scaling for other Rsense values is **UNCONFIRMED**. |

---

## 12. `nPackCfg` (0x1B5) full bit layout + factory-default trap

Source: datasheet Rev 12, **Table 16 "nPackCfg (1B5h) Register Format"** (p. 79) and the
NVM default table (p. 133, row `1B5h`). Bit spans below were verified by `pdftotext -bbox`
on p. 79: each field's x-centre was matched against the x-centres of the D15..D0 header
cells (e.g. AOCfg centre 87.16 == midpoint of D15 centre 70.71 and D14 centre 103.64).

**Register Type: Special. Factory Default Value: `0004h`.**

| Bits | Field | Meaning |
|---|---|---|
| D15:D14 | **AOCfg** | Always-on LDO: `00b` disabled, `01b` 3.4 V, `10b` 1.8 V, `11b` 3.4 V |
| D13 | **BtPkEn** | 0 = Batt/PCKP update every 22.4 s; 1 = update after every cell-measurement cycle |
| D12 | 0 | Always write 0 |
| D11 | **THType** | 0 = 10 kΩ NTC, 1 = 100 kΩ NTC |
| D10 | 0 | Always write 0 |
| D9:D8 | **CPCfg** | Charge-pump gate drive (DevName 420Ah+): `00b` 6 V, `01b` 8 V, `10b` 10 V |
| D7 | 0 | Always write 0 |
| D6 | 0 | Always write 0 |
| D5 | 0 | Always write 0 |
| D4:D2 | **NThrms** | `000b` die only, `001b` +TH1, `010b` +TH1,2, `011b` +TH1,2,3, `100b` +TH1,2,3,4 |
| D1:D0 | **NCELLS** | **Cell count − 2.** `00b` = 2S, `01b` = 3S, `10b` = 4S |

**There are no per-channel `CxEn` / `BALCFG` / `TdEn` / `A1En` / `A2En` / `BtEn` bits.**
Those names do not exist in the MAX17320 map. Cell channels are selected *only* by NCELLS
(Table 12: "Direct cell measurements of **selected number of cells**"); thermistor channels
*only* by NThrms; `BtPkEn` (D13) is a rate control, not a channel enable — Batt and PCKP are
always measured. Current is always measured "regardless of nPackCfg settings".

### Decode of `0x0004`
`AOCfg=00b` AOLDO **disabled** · `BtPkEn=0` Batt/PCKP **22.4 s** rate · `THType=0` 10 kΩ NTC ·
`CPCfg=00b` **6 V** gate drive · `NThrms=001b` die temp **+ TH1 only** · `NCELLS=00b` → **2S**.

### The trap
`0x0004` **is the factory default** — it does not prove anyone configured the part. Cross-check
against the other NVM defaults (datasheet p. 132–135 default table):

| Reg | Factory default | Meaning if you read exactly this |
|---|---|---|
| `1B3h` nDesignCap | **0x0000** | never programmed |
| `1A5h` nFullCapNom | **0x0D48** | never programmed (3400 is the *default*, not your cell) |
| `1A9h` nFullCapRep | **0x0D48** | never programmed |
| `1B5h` nPackCfg | **0x0004** | never programmed (2S / TH1 / no AOLDO / 6 V CP) |
| `1CFh` nRSense | **0x01F4** | never programmed — 0x01F4 = 5 mΩ is the *default*, so a 5 mΩ board reads "correct" on a virgin part |
| `1B9h` nNVCfg1 | 0x0182 | D8 `enProt` = 1 → **protector enabled by default** |

A virgin MAX17320 on a 2S / 5 mΩ board therefore looks "already configured" on every register
you would normally check. Confirm with nDesignCap = 0x0000.

### Cell-channel mapping and the negative-difference artefact

Datasheet Note 10 (EC table) defines the taps: **`BATTS−CELL3`, `CELL3−CELL2`, `CELL2−CELL1`,
`CELL1−CSP`.** The top cell is always referenced to **BATTS**, so in a 2S configuration
`Cell1 = CELL1 − CSP` and `Cell2 = BATTS − CELL1`.

`Cell1..Cell4` are **Voltage** type: Table 15 gives minimum value **0.0 V** (unsigned, no sign
bit). If a tap inverts (BATTS below CELL1, e.g. top cell missing) the difference is negative and
**the register reads 0.0000 V** — this is a *legitimate reading of an inverted stack*, not a
disabled channel and not a comms fault. What `Cell3`/`Cell4` report when above NCELLS is
**UNCONFIRMED** (Rev 12 never states it).

### Batt (0x0DA) / PCKP (0x0DB) — LSB re-verified, notes in §2 are correct

Three independent datasheet statements agree, and **neither LSB scales with NCELLS**:

* Batt Register (0DAh) section: *"total pack voltage measured inside the protector on a
  **20.48V scale with an LSB of 0.3125mV**"* — 65536 × 0.3125 mV = 20.48 V exactly (plain
  16-bit unsigned).
* PCKP Register (0DBh) section: *"voltage between **PACK+ and GND** on a 20.48V scale with an
  LSB of 0.3125mV"*.
* EC table: `VBLSB` "BATT Voltage Measurement Resolution, **BATTS**" = **312.5 µV**;
  `VPLSB` "PCKP Voltage Measurement Resolution, **PCKP pin**" = **312.5 µV**.

Measurement points: **Batt = BATTS pin** (top of the cell stack, *inside* the protector,
before the FETs). **PCKP = PCKP pin** (PACK+ side, *outside* the FETs). Only NCELLS-scaled
quantities in the whole map are the SBS mirrors: *"sDesignVolt … × (nPackCfg.NCells + 2).
sChargingVoltage and sMinSysVoltage are also scaled with nPackCfg.NCells."* — `Batt`/`PCKP`
are **not** in that list.

**Guaranteed measurement range is `VBFS`/`VPFS` = 4.2 V to 19.6 V.** Below 4.2 V the Batt/PCKP
readings are outside the specified range and their accuracy is unspecified. Likewise
`VIN` (Supply Voltage) min is **4.2 V** — a part reporting Batt < 4.2 V is running
below its minimum operating supply.

---

# APPENDIX A — Charge/discharge current configuration (added 2026-08-14)

**Scope:** everything needed to answer "what is the maximum charge and discharge current
this part can be configured for at Rsense = 5 mΩ, and why did a charge only run at 1 A".
Same primary source as §0: **MAX17320 datasheet 19-100749; Rev 12; 7/25, 180 pp.**
(verified: page 1 of the retrieved PDF carries `19-100749; Rev 12; 7/25`).
Bit spans below were re-verified by `pdftotext -bbox` column geometry, same method as §12:
each field label's x-centre was matched against the midpoint of the D-bit header cells it
spans. Agreement was within ~2 pt in every case (typically < 0.5 pt).
Cross-referenced against `ref-f767-max17320/max17320_config.h` (the `fertig_11400.INI`
working profile). **Rsense = 5 mΩ throughout.** This section is ADDITIVE — nothing above is
changed or retracted.

## A.1 Register layouts (all confirmed unless marked)

### `nIPrtTh1` — 0x1D3, slave 0x0B offset 0xD3 (Table 25, p. 83)
Factory default `4BB5h`. Working profile `4B80h`.

| Bits | Field | Encoding |
|---|---|---|
| D15:D8 | **OCCP** | Overcharge Current Protection @ room temp. **signed 2's complement, 400 µV LSb** |
| D7:D0 | **ODCP** | Overdischarge Current Protection. **signed 2's complement, 400 µV LSb** |

Datasheet wording: *"Protection threshold limits are configurable with 400μV resolution over
the full operating range of the current register… This field is signed 2's complement with
400μV LSb resolution **to match the upper byte of the current register**."*
→ OCCP/ODCP are compared against `Current[15:8]` (Current LSB 1.5625 µV × 256 = 400 µV exactly).

At 5 mΩ: **80 mA per LSB**, full range −10.24 A … +10.16 A.

* OCCP is re-scaled by `nJEITAC` HotCOEF/WarmCOEF/ColdCOEF **only if `nNVCfg1.enJP = 1`**
  (Table 71: *"Clear this bit to disable JEITA protection and make OVP and OCCP thresholds
  become flat"*). enJP = 0 in **both** the factory default and the working profile → OCCP flat.
* **ODCP is never zone-scaled** (Table 3 lists JEITA zones only for slow overcharge).
* Fault delay for both: `nDelayCfg.OverCurrTimer` (D8:D6).

### `nODSCTh` — 0x1DD, slave 0x0B offset 0xDD (Table 22, p. 82) — **fast comparators**
Factory default `0EAFh`. Working profile `0C00h`.

| Bits | Field | mV formula (datasheet) | Amps @ 5 mΩ |
|---|---|---|---|
| D15 | X | don't care | — |
| D14:D10 | **OCTH** | `+38.75 mV − OCTH × 1.25 mV` | `+7.75 A − OCTH × 0.25 A` |
| D9:D5 | **SCTH** | `−155 mV + SCTH × 5 mV` | `−31.0 A + SCTH × 1.0 A` |
| D4:D0 | **ODTH** | `−77.5 mV + ODTH × 2.5 mV` | `−15.5 A + ODTH × 0.5 A` |

All three are **inverted codes**: `00h` = maximum magnitude, `1Fh` = 0 mV (disabled/trip-always).
Confirmed against Table 23 "OCTH, SCTh, and ODTH Sample Values", which tabulates the amps
column *for a 5 mΩ sense resistor* — e.g. OCTH `00h` = 38.75 mV = 7.75 A, SCTH `00h` =
−155 mV = −31.00 A, ODTH `00h` = −77.5 mV = −15.50 A. Exact match.

> **Note on `max17320_config.h`:** the `MAX17320_ODSCTH()` macro's constants
> (`155 − 5·code`, `−620 + 20·code`, `−310 + 10·code`) are **correct** — they are expressed in
> **quarter-millivolts** (0.25 mV), as the macro's own comment says. 155/4 = 38.75, 5/4 = 1.25,
> 620/4 = 155, 20/4 = 5, 310/4 = 77.5, 10/4 = 2.5. They are *not* mV.

> **HARD CEILING:** `OCTH` maxes out at **38.75 mV = 7.75 A at 5 mΩ**. There is **no documented
> way to disable the fast overcharge comparator** — Table 24 (`nODSCCfg`) contains only
> `SCDLY` (D11:D8) and `OCDLY` (D3:D0) plus fixed bits, no enable bits, even though the
> "Fast Overcurrent Comparators" prose (p. 46) claims *"The nODSCCfg register **enables each
> comparator**"* and refers to a `nNVCfg1.enODSC` bit that **does not exist in Table 71**.
> **UNCONFIRMED** whether any disable exists. Treat 7.75 A as the maximum achievable charge
> current at 5 mΩ.

### `nJEITAC` — 0x1D8 (Table 26, p. 83)
Factory default = working profile = `644Bh`.

| Bits | Field | Encoding |
|---|---|---|
| D15:D8 | **RoomChargingCurrent** | **unsigned, 200 µV LSb** → **40 mA/LSB @ 5 mΩ** |
| D7:D6 | **WarmCOEF** | `WarmI = RoomI × (WarmCOEF + 5)/8` → 62.5 %…100 % |
| D5:D3 | **ColdCOEF** | `ColdI = RoomI × (ColdCOEF + 1)/8` → 12.5 %…100 % |
| D2:D0 | **HotCOEF** | `HotI  = RoomI × (HotCOEF  + 1)/8` → 12.5 %…100 % |

The 2/3/3 split is independently proved by the datasheet's own statement: *"To disable the
temperature dependence and create a flat charging current across the temperature range, set
the **lower byte of nJEITAC to a value of FFh**."* — and `(3+5)/8 = (7+1)/8 = (7+1)/8 = 100 %`
for `WarmCOEF=3, ColdCOEF=7, HotCOEF=7`, i.e. low byte `FFh`. Exact.

**Decode of `644Bh` @ 5 mΩ:** Room = 100 → 20.00 mV → **4.000 A**;
Warm = 1 → 75 % → **3.000 A**; Cold = 1 → 25 % → **1.000 A**; Hot = 3 → 50 % → **2.000 A**.

Minor datasheet off-by-one: the text says the range is *"00h (0mV) to FFh (51.2mV)"*, but
255 × 200 µV = **51.0 mV** (10.20 A @ 5 mΩ). 51.2 mV would need code 256. Use 10.20 A.

### `nJEITAV` — 0x1D9 (Table 19, p. 80)
Factory default = working profile = `0059h`. **All values are per-cell.**

| Bits | Field | Encoding |
|---|---|---|
| D15:D8 | **RoomChargeV** | **signed int8, 5 mV LSb, offset from 4.2 V** → `4.2 V + int8 × 5 mV`, range 3.56–4.835 V |
| D7:D6 | **dWarmChargeV** | unsigned, `WarmV = RoomV − dWarm × 20 mV` (0…−60 mV) |
| D5:D3 | **dColdChargeV** | unsigned, `ColdV = RoomV − dCold × 20 mV` (0…−140 mV) |
| D2:D0 | **dHotChargeV** | unsigned, `HotV = **WarmV** − dHot × 20 mV` (0…−140 mV) — **relative to WarmV, not RoomV** |

**Decode of `0059h`:** Room = 0 → **4.200 V**; dWarm = 1 → **4.180 V**;
dCold = 3 → **4.140 V**; dHot = 1 → 4.180 − 0.020 = **4.160 V**.
(2S pack terminal equivalents: 8.40 / 8.36 / 8.28 / 8.32 V.)
`nJEITAV` also sets the OVP reference (`nOVPrtTh.dOVP` is relative to it).

### `nChgCfg` — 0x1C2 (Table 60, p. 102) — the datasheet calls it **nChgCfg**, not nChgCfg0
Factory default = working profile = `2061h`.

| Bits | Field | Encoding |
|---|---|---|
| D15:D13 | fixed `001b` | must be written as-is |
| D12:D8 | **PreQualVolt** | signed 2's compl., `PrequalV = UVP + PreQualVolt × 20 mV`, range UVP−320 mV…UVP+300 mV |
| D7:D5 | **HeatLim** | FET dissipation limit during prequal regulation = `(HeatLim+1) × 102 mW` (102…819 mW) |
| D4:D0 | **PreChgCurr** | `PreChargeCurrent = nJEITAC.RoomChargingCurrent × (PreChgCurr+1)/128` (RoomI/128 … RoomI/4) |

**Decode of `2061h`:** PreQualVolt = 0 → prequal threshold = UVP = **2.80 V/cell**;
HeatLim = 3 → **408 mW**; PreChgCurr = 1 → RoomI × 2/128 = RoomI/64 = **62.5 mA** at RoomI = 4 A.
Gated by `nProtCfg.PreqEn` (D8) = **1** in the default/profile.
Datasheet warning: *"It may take approximately **1 minute** for the charge current to begin to
flow when in prequal mode."*

### `nChgCtrl` — 0x1C3 (called `nChgCtrl0` in the config header)
**No bit layout is published anywhere in Rev 12.** The only statements are
*"Set nChgCtl (1C3h) = **00E1h** for proper operation"* and Table 98's
*"Always Required for Charge Control"*. **UNCONFIRMED layout — leave at `00E1h`.**

### `0x1C0` — datasheet name is **`nPReserved0`**, not `nChgCtrl1` (Table 18, p. 80)
Factory default = working profile = `0000h`. Table 98: *"Do Not Modify without Special
Guidance from Maxim."*

| Bits | Field | Encoding |
|---|---|---|
| D12:D10 | **UV_ChargeBlockThr** | `1 V + code × 0.25 V` (1.25–2.75 V). **`000b` = disabled.** Blocks charging if ANY cell is below this. DevName 420Ah+ |
| others | X / reserved | — |

Currently `000b` = disabled. **If this were ever set non-zero it would block charging outright**
on a deeply-discharged pack.

### `nProtCfg` — 0x1D7 (Table 44, p. 90)
Factory default = working profile = `0900h`.
`ChgWDTEn` D15 · `0` D14 · `0` D13 · **`SCTest` D12:D11** · `CmOvrdEn` D10 · `0` D9 · **`PreqEn` D8** ·
`Reserved` D7 · `PFEn` D6 · `DeepShpEn` D5 · `OvrdEn` D4 · `UVRdy` D3 · `FetPFEn` D2 ·
`BlockDisCEn` D1 · `Reserved` D0.
(SCTest span D12:D11 confirmed by geometry: label x-centre 295.2 vs D12/D11 midpoint 295.9.)

**Decode of `0900h`:** SCTest = `01b`, **PreqEn = 1**, everything else 0 — so `PFEn = 0`
(no permanent-fail), `UVRdy = 0` (CHG FET + pumps stay powered during UVP), `CmOvrdEn = 0`
(CommStat.CHGOff/DISOff are inert). **Contains no current limit.**
Oddity: the default `0900h` sets `SCTest = 01b` although the text says *"Set SCTest = 00b to
disable"*. Datasheet inconsistency, unchanged from default — noted, not resolved.

### `nProtCfg2` — 0x1DF (Table 40, p. 89)
Factory default `A065h`; working profile `8016h`.
`1` D15 · `0` D14 · **`CEEn` D13** · `0` D12 · **`LeakCurrTh` D11:D8** · **`CheckSum` D7:D0**.
**Decode of `8016h`:** CEEn = 0 (self-discharge detection off), LeakCurrTh = 0,
CheckSum = `16h`. **Contains no current limit.**
The CheckSum byte covers 1B0h–1BBh, 1C0h–1C3h, 1D0h–1DEh and is only checked when
`nNVCfg1.enProtChksm = 1` — which is **0** in the profile, so it need not be recomputed when
protection thresholds change. If enProtChksm is ever enabled, it must be.

### `nProtMiscTh` — 0x1D6 (Table 30, p. 85)
**Two conflicting factory defaults in Rev 12:** the register section says `7A28h`,
Table 98 (p. 134) says **`7A58h`**. The working profile uses `7A58h`. §6 of this document
quotes the `7A28h` figure; both appear in the datasheet. **UNCONFIRMED which is authoritative.**

**Decode of `7A58h`:** QovflwTh = 7 → coefficient **1.5** · TooHotDischarge = 0xA → T4 + 20 °C =
**75 °C** · **CurrDet = 5 → (5+1) × 5 mA = 30 mA** (not 15 mA — §6's worked example assumes the
`7A28h` default) · DieTempTh = 8 → **90 °C**.

**`QovflwTh` is the only capacity-dependent charge block in the part:**
*"Capacity overflow protection threshold = **designCap × coefficient** … If the delta Q exceeds
the capacity overflow-protection threshold … then a `ProtStatus.Qovrflw` fault is generated"*
(p. 85), and p. 36: *"If any charge session delivers more charge (coulombs) to the battery than
the expected full design capacity, **charging is blocked**."*

### `nDPLimit` — 0x1E0
**No register section, no bit table, no format anywhere in Rev 12** — it appears only in the
memory map, in Table 97 ("Free if feature is not used") and Table 98 ("Configures Dynamic
Power"). Gated by `nNVCfg0.enDP`, which is **0** in both the factory default (`0A00h`) and the
working profile (`0A80h`) → Dynamic Power disabled, `nDPLimit` is free user memory.
Dynamic Power is an **estimation/reporting** feature (outputs `MaxPeakPower`/`SusPeakPower`/
`MPPCurrent`/`SPPCurrent`); it does **not** limit or gate actual current.
**Layout UNCONFIRMED — do not attempt to configure it from Rev 12 alone.**

### `nDelayCfg` — 0x1DC (Table 32, p. 87)
Factory default = working profile = `AB3Dh`.
`CHGWDT` D15:D14 · `PrequalTimer` D13:D11 · `OVPTimer` D10:D9 · **`OverCurrTimer` D8:D6** ·
`PermFailTimer` D5:D4 · `TempTimer` D3:D2 · `UVPTimer` D1:D0.
**Decode of `AB3Dh`:** CHGWDT = 2 · PrequalTimer = 5 · OVPTimer = 1 ·
**OverCurrTimer = 4 → 2.8 s…5.6 s** (OCCP/ODCP debounce) · PermFailTimer = 3 (the required
value) · TempTimer = 3 → 5.625–11.25 s · UVPTimer = 1 → UVP 2.8–5.625 s / shutdown 45–90 s.

### `nODSCCfg` — 0x1DE (Table 24, p. 83)
Factory default = working profile = `4355h`.
`X` D15 · `1` D14 · `X` D13 · `X` D12 · **`SCDLY` D11:D8** · `X` D7 · `1` D6 · `X` D5 · `1` D4 ·
**`OCDLY` D3:D0**.
`SCDLY`: `70 µs + 61 µs × SCDLY` (0h–Fh). `OCDLY`: `70 µs + 977 µs × OCDLY`, documented range
**1h–Fh** (0h not documented — **UNCONFIRMED** whether 0h disables).
**Decode of `4355h`:** SCDLY = 3 → **253 µs**; OCDLY = 5 → **4.955 ms**.

### `nUVPrtTh` — 0x1D0 (Table 17, p. 80) — needed to interpret prequal
Factory default `508Ch`; working profile `785Bh`.
`UVP` D15:D10 (6b, `2.2 V + code × 20 mV`) · `0` D9 · `UOCVP` D8:D4 (5b, `UVP + code × 40 mV`) ·
`UVShdn` D3:D0 (**signed** 4b, `UVP + int4 × 40 mV`, −320…+280 mV).
**Decode of `785Bh`:** UVP = 30 → **2.80 V** · UOCVP = 5 → **3.00 V** · UVShdn = −5 → **2.60 V**.

### `nTPrtTh1/2/3` decode of the working profile (1 °C signed LSB)
`nTPrtTh1 = 3700h` → T4 (TooHot) = **55 °C**, T1 (TooCold) = **0 °C**
`nTPrtTh2 = 2D0Ah` → T3 (Hot) = **45 °C**, T2 (Cold) = **10 °C**
`nTPrtTh3 = 5F28h` → TpermFailHot = **95 °C**, Twarm = **40 °C**

→ **JEITA zones and prescribed current with `nJEITAC = 644Bh`:**

| Temp | Zone | ChargingCurrent | ChargingVoltage (per cell) |
|---|---|---|---|
| < 0 °C | TooCold | **0 A** (charging blocked) | — |
| 0…10 °C | Cold | **1.000 A** | 4.140 V |
| 10…40 °C | Room | **4.000 A** | 4.200 V |
| 40…45 °C | Warm | **3.000 A** | 4.180 V |
| 45…55 °C | Hot | **2.000 A** | 4.160 V |
| > 55 °C | TooHot | **0 A** (charging blocked) | — |

## A.2 How `ChargingCurrent` (0x028) is actually computed

Authoritative list — **Table 1, "Summary of Protector Registers by Function", p. 39**, section
*"Charging Prescription (ChargingCurrent, ChargingVoltage registers)"*:

| Function | Register |
|---|---|
| Charging Voltage | `nJEITAV` |
| **Charging Current** | **`nJEITAC`** |
| Prequal Current | `nChgCfg` |
| Step Charging | `nStepChg` |

**`nDesignCap` / `nFullCapNom` / `nFullCapRep` are NOT in that list, and appear nowhere in the
ChargingCurrent path.** `RoomChargingCurrent` is an **absolute voltage across Rsense**
(200 µV/LSB), **not a C-rate**. Evaluation order:

```
if (prequal active: VCell < nChgCfg.PrequalVolt && nProtCfg.PreqEn)
      ChargingCurrent = nJEITAC.RoomChargingCurrent × (nChgCfg.PreChgCurr + 1)/128
else  ChargingCurrent = JEITA-zone current (Room / Cold / Warm / Hot, per A.1)
                        × step-charging stage ratio (Stage 0 = 1, Stage 1/2 per nStepChg)
```
and the whole feature is gated by **`nNVCfg1.enJ` (D7)** = *"Enable ChargingCurrent and
ChargingVoltage"*. `nNVCfg1 = 0182h` → **enJ = 1**, enProt = 1, **enJP = 0**, enCTE = 1.

Step-charging stage (Figure 10, p. 52) is selected by VCell vs
`StepV0 = ChargingVoltage_zone − StepdV0 × 10 mV` and `StepV1 = … − StepdV1 × 10 mV`.
With `nStepChg = C884h` and a 4.2 V zone voltage: StepV0 = 4.12 V, StepV1 = 4.16 V.

> **Rev 12 contradicts itself on the step ratio.** The Step Charging section (p. 52) says
> `nJEITAC × (StepCurr1 + 1)/16`; the `nStepChg` register section (Table 59, p. 101) says
> `ChargingCurrent_JEITAZONE × StepCurr1/16` with the worked example `2000 mA × 12/16 = 1500 mA`.
> With `C884h` (StepCurr1 = 12, StepCurr2 = 8) at a 4 A room current that is 3.25/2.25 A
> (p. 52 formula) or 3.00/2.00 A (Table 59 formula). §6 of this document quotes the p. 52 form.
> **UNCONFIRMED which is correct.** Neither produces 1 A. Set `nStepChg = FF00h` to disable.

### Does a too-small capacity reduce the prescribed charging current? — **REFUTED**
No register in the ChargingCurrent path reads any capacity. A wrong capacity has three *other*
consequences, none of which is a lower prescribed current:

1. **`Qovflw` charge block.** With `nNVCfg0.enDC = 0` (the default, and also the value in the
   working profile — `0A80h` has D4 = 0), `nDesignCap` is *not* used; the **Alternate Initial
   Value of DesignCap is the `FullCapRep` register value** (nDesignCap register section, p. 107,
   and Table 98 row 1B3h: *"FullCapRep → DesignCap"*). So on a part with
   `nDesignCap = 0000h` and `nFullCapRep = 0D48h`, **DesignCap = 3400 mAh**, and charging is
   blocked once a single session delivers `3400 × 1.5 = 5100 mAh`.
2. **Premature `Full`.** `FullCapRep = 3400 mAh` on an 11400 mAh pack → RepSOC reaches 100 %
   after ~3.4 Ah → `ProtStatus.Full` → charging stops at ~30 % real SOC.
3. **Wrong `IChgTerm`.** With `nNVCfg0.enICT = 0` (factory default `0A00h`), IChgTerm's
   Alternate Initial Value is *"1/3rd the value of the nFullCapNom register (corresponds to
   C/9.6)"* → 3400/9.6 = **354 mA** instead of the profile's `nIChgTerm = 0720h` = 570 mA.

## A.3 Maximum-current configuration @ 5 mΩ

**Hardware ceiling first:** the current ADC is ±51.2 mV/Rsense (Table 15) = **−10.24 A …
+10.2397 A** at 5 mΩ. Nothing can be measured or protected beyond that.

| Register | Profile | Max-current value | Meaning |
|---|---|---|---|
| `nIPrtTh1` 0x1D3 | `4B80h` (+6.00 / −10.24 A) | **`7F80h`** | OCCP `7Fh` = +10.16 A, ODCP `80h` = −10.24 A |
| `nODSCTh` 0x1DD | `0C00h` (+7.00 / −31.0 / −15.5 A) | **`0166h`** (recommended) | OCTH `00h` = **+7.75 A (the true cap)**, SCTH `0Bh` = −20 A, ODTH `06h` = −12.5 A |
| `nJEITAC` 0x1D8 | `644Bh` (4/3/1/2 A) | **`FFFFh`** absolute / **`AFFFh`** practical | `FFFFh` = 10.20 A flat; `AFFFh` = 7.00 A flat, safely under OCTH |
| `nChgCfg` 0x1C2 | `2061h` (62.5 mA prequal) | `207Fh` (`PreChgCurr=31`) | prequal = RoomI/4 |
| `nStepChg` 0x1DB | `C884h` | `FF00h` | disables step-charging derating |
| `nProtCfg` 0x1D7 | `0900h` | unchanged | no current limit in it |
| `nProtCfg2` 0x1DF | `8016h` | unchanged | no current limit in it |
| `nProtMiscTh` 0x1D6 | `7A58h` | unchanged | no current limit; QovflwTh needs a correct DesignCap |
| `nDPLimit` 0x1E0 | `0000h` | unchanged | Dynamic Power disabled (`enDP = 0`); reporting only |
| `nChgCtrl` 0x1C3 | `00E1h` | unchanged | mandated fixed value |
| `nPReserved0` 0x1C0 | `0000h` | unchanged (**keep 0**) | non-zero `UV_ChargeBlockThr` blocks charging |

**Net result at 5 mΩ:**
* **Discharge: −10.24 A** — reachable; limited by the shunt/ADC, not by any threshold.
* **Charge: +7.75 A** — limited by `nODSCTh.OCTH`, *not* by OCCP (+10.16 A) or nJEITAC (10.20 A).
  A 10 A charge is **not achievable at 5 mΩ**. To go higher the shunt must shrink
  (e.g. 2 mΩ → OCTH ceiling 19.4 A, ADC ±25.6 A).

Caveat on `OCCP = 7Fh` (**inference, not datasheet text**): the Current register saturates at
`7FFFh`, so `Current[15:8]` can *equal* `7Fh` but never *exceed* it. Since the datasheet says
the fault occurs when the reading *"exceeds this value"*, `OCCP = 7Fh` most likely renders slow
overcharge protection permanently inactive. Use `7Eh` (+10.08 A) if an armed threshold is wanted.
