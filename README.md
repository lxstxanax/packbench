# packbench

Bench tooling for MAX17320-based lithium packs: an STM32G474 monitor and
provisioner, a live desktop dashboard, and the reverse-engineering it took
to make a TPS26750 + BQ25756 charging chain actually deliver current.

Built against a 2S4P pack of Molicel INR18650-P30B (11400 mAh nominal) on a
custom MAX17320G2 board with a 5 mΩ shunt.

The firmware is **read-only by default**. The only writes it can perform are
clearing the sticky alert history and, behind an explicit confirmation, one
NVM provisioning commit.

---

## What is in here

| Path | What |
|---|---|
| `max17320_gui/` | CubeMX project (STM32G474RE, CMake). Generated code only. |
| `fw/` | The firmware: I2C driver, register decoding, dashboard, provisioning, host tests. Lives outside the CubeMX tree so regeneration cannot touch it. |
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

| Signal | BMS board P1 | Nucleo G474RE |
|---|---|---|
| SCL | P1.3 | PA15 = **CN7 pin 17** |
| GND | P1.5 (AGND) | **CN7 pin 19** |
| SDA | P1.2 | PB7 = **CN7 pin 21** |
| pull-ups | — | **4.7–5 kΩ** from SDA and SCL to 3V3 (CN7 pin 16) |

Three consecutive positions in one column of CN7, with 3V3 for the pull-ups
on the same connector. Looking at the top of the board with the USB pointing
away, CN7 is the outer-left header and pin 1 is at the USB end; the odd
column is the one nearest the board edge. The bottom silkscreen prints the
odd **pin numbers** — not signal names.

**Do not connect P1.1 (SYSP).** It is pack positive, 6.0–8.4 V, and it sits
directly next to SDA. There is no ESD protection on SDA/SCL on that board.

**Ground to P1.5 only.** AGND is pack negative; the 5 mΩ shunt sits between
it and the cell-negative header. Grounding to both shorts the shunt and
zeroes every current reading.

**CN7 pin 18 is +5V and sits directly opposite pin 17.** A solder bridge
across that row puts 5 V on a 3.3 V-only GPIO. Pin 15, one row up in the same
column, is SWCLK — bridge it and the debugger dies.

---

## Bring-up

1. **Pack first.** The gauge is powered from the pack (BATTP → 10 Ω → IN).
   With no cells it does not answer, and that reads exactly like a wiring
   fault.
2. **Pull-ups.** The BMS board has none of its own — the `SDA` net is exactly
   {P1.2, U1.9} and `SCL` is {P1.3, U1.8} in both the schematic and the PCB
   netlist. Fit your own.
3. **Flash**: VS Code task `Flash STM32`.
4. **Watch**: task `Serial Monitor (ST-Link VCP)`. Expect `probe: OK` and
   `DevName 0x420A`. A garbage DevName means the bus is wrong, not the pack.
5. The green LED blinks slowly when the gauge answers and quickly when it
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

`Build` · `Clean` · `Clean & Rebuild` · `BuildSize` · `Test Decode (host)` ·
`OpenConfiguration` · `Flash STM32` · `Reset` · `Erase & Flash STM32` ·
`Full CHIP Erase` · `Start Debug` · `Serial Monitor (ST-Link VCP)` ·
`BMS Dashboard`

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
| `notes/nucleo-g474re-pinout.md` | NUCLEO-G474RE pinout, solder bridges, and the PB8-BOOT0 trap. |
| `notes/tps26750-evm.md` | The charging chain: bus topology, addresses, BQ25756 registers, the four traps above, and every path into the hardware. |
| `notes/usb2any-bq.md` | The USB2ANY HID protocol, recovered and verified. |

---

## Shunt

Every current, capacity and power scale derives from the sense resistor, set
once in `max17320_gui/CMakeLists.txt`:

```cmake
MAX17320_RSENSE_MOHM=5      # R18, MFC0603-R005FT5 on this board
```
