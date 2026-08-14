# TPS26750EVM from Linux — what talks to what, and how to lower charge current

Working notes, researched 2026-08-14. Every factual claim is tagged **[C]** (confirmed
against a TI primary source, cited) or **[U]** (unconfirmed — inference, or claim I could
not back with a primary source). **Do not act on a [U] item with the battery connected
without checking it first.**

---

## 0. TL;DR

* The "BQ" is a **BQ25756 or BQ25756E** on a **separate BQ25756EVM** board. There is no
  charger IC on the TPS26750EVM itself. [C] **The two variants have different I²C
  addresses — 0x6B vs 0x6A. Scan before you write.** [C] §2.
* The **TPS26750 is the I²C controller (master)** of the bus the charger sits on. A host
  must not drive that bus in parallel — the part explicitly does not support
  multi-controller. [C]
* Charge current is set by **BQ25756 register 0x02 `ICHG_REG`**, 50 mA/bit. For **5.0 A
  write 16-bit `0x0190`**; for **6.0 A write `0x01E0`**. [C]
* The serial protocol of the on-board "Redline TivaLine Adapter" (a TM4C123 USB↔I²C
  bridge) is **undocumented and not reverse-engineerable from public sources**. TI has
  refused to release the firmware or its source. [C] See §3 for what to do instead.
* **You do not need Windows.** TI ships a **native Linux x86-64 build** of the
  Application Customization Tool, and it is the tool that owns this EVM. [C] §7 Path B.
* The *durable* fix is **not** to poke the charger's register at runtime — the PD
  controller rewrites it on every PD contract. Change the PD controller's configuration
  (Question 15) and reflash. §7 Path B.

### Do this first, in this order

1. **Read the charger's die marking** — BQ25756 (0x6B) or BQ25756E (0x6A). §2.
2. **Measure R2 and R24 on the BQ25756EVM.** Both should be 5 mΩ. If the input one is
   2 mΩ you have an older board and every input-current number is off by 2.5×. §8.
3. **Fit the hardware ceiling.** Remove JP4, fit 10 kΩ via JP3 → a hard 5 A limit that no
   software or watchdog can defeat. §7 Path D. Do this before anything else if the pack
   matters.
4. **Then** set the durable software value: ACT on Linux, Question 15 = 5.0 A, reflash.
   §7 Path B.
5. Only if you need live control/telemetry: wire your own master to J2 and use the
   documented `I2Cw`/`I2Cr` passthrough. §7 Path A.

---

## 1. What is actually plugged into this machine

Observed locally (`lsusb`, `lsusb -t`, `/sys`):

| Device | ID | Driver | Notes |
|---|---|---|---|
| TUSB2036 Hub | `0451:2036` | `hub` | U14 on the TPS26750EVM [C] |
| CDC serial "Redline TivaLine Adapter" | `1cbe:0002` | `cdc_acm` → `/dev/ttyACM1` | U22 = TM4C123GH6PM [C] |
| **USB2ANY/OneDemo** | `2047:0301` | `usbhid` → `/dev/hidraw0` | **separate adapter**, see §6 |
| STLINK-V3 | `0483:374e` | — → `/dev/ttyACM0` | the Nucleo, unrelated |

USB descriptor of the Tiva adapter: `iManufacturer = "Texas Instruments"`,
`iProduct = "Redline TivaLine Adapter"`, `iSerial = ffffffff…` (unprogrammed). Plain CDC-ACM,
2 interfaces, bulk IN/OUT 64 B + interrupt IN 16 B. **No vendor-specific interface** — so
whatever protocol exists rides the serial byte stream, not control transfers.

The USB2ANY sits on a *different* root-hub port than the TUSB2036, so it is a second,
physically separate adapter. **The BQ25756EVM's J4 is "Communication port for the
USB2ANY"** [C, SLUUCT7D Table 2-3], so this is very probably the USB2ANY cabled to the
charger EVM — but confirm by unplugging it and re-running `lsusb`. [U]

> `/dev/hidraw0` is `root:root 0600`. A udev rule is needed for non-root access:
> `SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2047", ATTRS{idProduct}=="0301", MODE="0666"`

---

## 2. Bus topology and addresses

The TPS26750 has **two** separate I²C ports [C, SLVSH67 Table 7-4]:

* **I2Ct** — *target*. For an external MCU / the on-board Tiva. Also the patch-load bus.
* **I2Cc** — *controller* (master). "Connect to a I2C EEPROM, Battery Charger. …
  **Multi-controller configuration is not supported.**" [C, SLVSH67 Table 7-4]

```
                 TPS26750EVM                                   BQ25756EVM
  ┌───────────────────────────────────────────┐            ┌────────────────┐
  │                                           │            │                │
  │  USB ─ TUSB2036 ─ TM4C123 (U22) ── I2Ct ──┼── TPS26750 │  BQ25756  0x6B │
  │        hub U14   "Redline Tiva"   0x20-23 │      │     │       │        │
  │                                           │    I2Cc ───┼───────┘  (J9   │
  │                             CAT24C512 ────┼──────┘     │  ribbon → J8)  │
  │                             EEPROM 0x50   │            │                │
  └───────────────────────────────────────────┘            └────────────────┘
```

| Device | 7-bit addr | Bus | Source |
|---|---|---|---|
| TPS26750 host interface | **0x20 / 0x21 / 0x22 / 0x23** | I2Ct | [C] SLVSH67 Table 7-5; corroborated SLUUDH4 Table 6-1 ("checks for the PD controller I2Ct address (0x20 through 0x23)") |
| Config EEPROM (U13, CAT24C512) | **0x50** | I2Cc | [C] SLVSH67 §7.3.11 / §7.4.1; SLUUDH4 Table 6-2 ("lock to address 0x50") |
| BQ25756 | **0x6B** | I2Cc | [C] SLUSEN5A §6.3.10.5 — *"The device 7-bit address is defined as 1101 011' (0x6B) by default."* |
| BQ25756**E** | **0x6A** | I2Cc | [C] SLUSFF4 §8.3.10.5 — *"…defined as 1101 011' (0x6A) by default"*; also §1 comparison table |

> **Which variant is on your board?** The EVM guide is titled *"BQ25756**E** Evaluation
> Module"* yet its own description says *"a complete evaluation system for the BQ25756
> IC"*, and its assembly notes give separate jumper lists "For BQ25750 variant", "For
> BQ25756 variant", "For BQ25758 variant" — i.e. one PCB serves several parts. [C,
> SLUUCT7D] The Application Customization Tool likewise offers both BQ25756 and BQ25756E
> [C, SLUUDH4 Table 3-14]. So the address is **0x6B or 0x6A depending on the die actually
> fitted** — read the IC marking, and scan both. [U as to which yours is]

**Which of 0x20–0x23 this EVM uses** is selected by resistor dividers on the ADCIN1/ADCIN2
pins [C, SLVSH67 Tables 7-2 / 7-5 / 7-6]. Reading the EVM schematic (R2 = 100 k, R63 =
115 k, R4/R5 = 0 Ω tying both ADCINs to the same node, R6–R9 not populated, J15 across
R63) gives decoded (5,5) with J15 removed and (0,0) with J15 fitted — **both map to
address index #2 = 0x21**. [U — this is my arithmetic on the schematic, not a printed
statement.] **Scan 0x20–0x23 rather than trusting it.**

Headers that expose the buses [C, SLVUCP8 Tables 2-1, 2-2, 2-5]:

| Header | Pins | Signal |
|---|---|---|
| **J2** (TPS26750 Digital) | 12 / 14 / 16 / 7,8 | **I2Ct_SDA / I2Ct_SCL / I2Ct_IRQ / GND** |
| J5 (Digitizer) | 1 / 2 | I2Cc_SDA / I2Cc_SCL |
| J9 (4×2 IDC → BQ25756EVM J8) | 4 / 6 / 2,3 | I2Cc_SCL / I2Cc_SDA / GND |

J2 is the safe place to attach your own master. J5/J9 are the charger bus, which the
TPS26750 already masters — see §5.

---

## 3. Q1 — the "Redline TivaLine Adapter" serial protocol

### Verdict: undocumented, and I could not reverse-engineer it from public sources.

What is confirmed:

* U22 is a **TM4C123GH6PMTR** (Tiva C, Cortex-M4F) [C, SLVUCP8 BOM Table 5-1].
* Its job is USB↔I²C bridging: *"The TPS25750EVM is not compatible with the USB2ANY, it
  has an on board TIVA MCU that implements to USB to I2C bridge to program the EEPROM."*
  [C, TI on E2E thread 994950]
* **Baud rate 9600.** The user guide's screenshot caption says: *"Check to make sure the
  Port is connected to Texas Instruments, Inc. and the Baud Rate is set to 9600"*
  [C, SLVUCP8 Figure 3-7]. Line settings beyond that are not stated; 8N1 is the obvious
  assumption [U].
* **TI will not release the firmware.** On E2E thread 1264210 TI states they would not
  provide the TIVA firmware as binary or source, and point to the generic SW-TM4C
  examples instead. [C]
* The adapter's exposed capabilities, inferred from what the GUI does with it
  [C, SLUUDH4 §6.1–6.3]: sweep/select COM port; probe I²C addresses 0x20–0x23; read PD
  registers 0x0F (Version) and 0x03 (Mode); erase / write / verify the EEPROM at 0x50;
  and in **Debug Mode** perform arbitrary I²C read/write of PD registers.

What I checked and ruled out:

* No TI document describes the framing. Searched TI literature, E2E, and the web.
* Not an ASCII command shell — the port emits **nothing** on open at 9600 (verified
  locally: 0 bytes in a 4 s passive read), so there is no banner or prompt.
* Not in the GUI's JavaScript. I downloaded the GUI Composer app
  (`dev.ti.com/gallery/view/USBPD/USBCPD_Application_Customization_Tool/ver/2.0.0/`) and
  all 104 of its JS chunks; none contains serial/I²C transport code. The transport lives
  in the GUI Composer runtime / TI Cloud Agent native helper, not the app bundle.
* No open-source reimplementation exists. `python-usb2any`, `pd-tools`, Chromium OS
  `hdctools` — none covers this adapter. The kernel's `tps6598x` driver is an *I²C client*
  driver and knows nothing about any USB bridge.

### If you want the protocol anyway — sniff it

This is the only reliable route, and it is easy because the tool runs natively on Linux
(§7 Path B). Interpose a pty between the GUI and the real port:

```bash
sudo apt install interceptty          # or use socat
interceptty -s 'ispeed 9600 ospeed 9600' /dev/ttyACM1 /tmp/ttyFAKE | tee /tmp/pd.log
# point the GUI at /tmp/ttyFAKE, then click "Identify" (reads 0x0F and 0x03)
```

`Identify` is the ideal first capture: it is a tiny, known transaction (read PD register
0x0F, then 0x03, at address 0x20–0x23), so the correspondence between bytes on the wire
and the I²C transaction is almost forced. Capture `Read` in Debug Mode next for a
register read of known length, and one `Write` for the write framing.

I did **not** attempt to poke the port with speculative bytes. With a lithium pack on the
BQ25756EVM and a config EEPROM at 0x50 on the same system, a malformed write could
corrupt the PD controller's configuration. Sniffing is strictly better.

---

## 4. Q2 — what is on the EVM, and the BQ

* **The TPS26750EVM carries no charger IC.** Its BOM lists U2 TPS26750RSMR, U7 TPS25750DRJK
  (a second PD controller for the power port), U13 CAT24C512 EEPROM, U14 TUSB2036,
  U22 TM4C123GH6PM, plus LDOs/switches/ESD — no BQ part. [C, SLVUCP8 BOM Table 5-1]
* The EVM's headline feature is *"Integrated I2C control for **BQ25756** battery charger"*
  and *"Interfacing connector for BQ25756EVM"*. [C, SLVUCP8 p.1]
* The kit ships a **BQ25756 interposer board and a ribbon cable**; the charger is the
  separate **BQ25756EVM**, joined via TPS26750EVM J1/J7 (power) + J9→J8 (ribbon, I2Cc).
  [C, SLVUCP8 §1.2, §4.2.1]
* The GUI only offers **BQ25756 / BQ25756E** for the TPS26750. [C, SLUUDH4 Table 3-14]
  (The TPS26750 datasheet §8.2.2.1 also names BQ25792; the tool does not offer it for this
  part. Trust the tool.)
* TI reference design pairing the two: **PMP41115**, 240 W USB-PD 3.1 charger, test report
  TIDT407. [C, ti.com/tool/PMP41115]

**BQ25756 in one line:** a *buck-boost battery charge **controller*** (external FETs), VAC
4.2–70 V, up to 20 A charge current, I²C at 0x6B, with a 5 mΩ battery-side sense resistor.
[C, SLUSEN5A]

---

## 5. Q4 — who is the I²C master? (read this before writing anything)

**The TPS26750 is the master of the charger's bus, and it programs the BQ25756 itself.**

* Datasheet §7.1: *"The TPS26750 has one I2C controller to write to and read from external
  target devices such as a battery charger or an optional external EEPROM memory."* [C]
* SLUUDH4 §3.3: the configuration *"enables the PD internal firmware to program the
  BQ/DCDC during operation such as setting output voltage/current based on the PD
  negotiation."* [C]
* Datasheet Table 7-4: I2Cc — *"Multi-controller configuration is not supported."* [C]

So: **do not hang your own I²C master on I2Cc (J5 pins 1/2, or J9 pins 4/6) while the
TPS26750 is powered.** That is real bus contention, not a theoretical one.

### The supported way through: `'I2Cw'` / `'I2Cr'` 4CC passthrough

The TPS26750 exposes two 4CC tasks that make it perform a transaction on I2Cc on your
behalf, so the PD controller stays the only master:

* **`'I2Cr'`** — *"cause the PD controller to read from a specified target address and
  register offset using a I2C read transaction through the I2Cc_SDA and I2Cc_SCL pins."*
  [C, SLVUCR7 §5.5.2 Table 5-24]
* **`'I2Cw'`** — *"cause the PD controller to write a particular I2C transaction using
  I2Cc_SDA and I2Cc_SCL."* [C, SLVUCR7 §5.5.3 Table 5-25]

Limits, from TI's own worked example [C, github.com/TexasInstruments/usb-pd,
`examples/tps25751/mspm0g3507/tps25751_i2c_passthrough/README.md`]:

* payload limited to **10 bytes** each way;
* **~150 ms minimum between passthrough commands** — *"the duration between passthrough
  commands is limited to approximately 150ms due to concurrency measures with the event
  driver on the USB-PD controller"*;
* `I2Cw` only *queues* the transaction; success of the 4CC ≠ the write happened. TI says
  to verify with `I2Cr`.

### The catch that decides the whole approach

The PD firmware **rewrites** `ICHG_REG` (and IAC_DPM / VAC_DPM / VFB_REG / ITERM /
IPRECHG) from its own configuration whenever the PD contract changes [C, SLUUDH4 Table
3-15 + §3.3]. So a value you poke in over passthrough is transient — the next
renegotiation, plug event or reset restores the configured value. **If you want 5–6 A to
stick, change the configuration, not the register.** See §7 Path B.

---

## 6. Q5 — the Linux-side paths that already exist

* **`/dev/i2c-0..8` is a dead end, as suspected.** Verified locally:
  `i2c-0 = SMBus I801 adapter at 0000:00:1f.4` (chipset SMBus), `i2c-1..4 = i915 gmbus
  dp{b,c,d}/misc`, `i2c-5..7 = AUX A/B/C DDI PHY`, `i2c-8 = NVIDIA i2c adapter 1`. All
  motherboard/GPU DDC. None reaches the EVM. `i2c-tools` is not installed anyway.
* **`tps6598x` does not and cannot bind here.** The module exists
  (`/lib/modules/6.17.0-40-generic/kernel/drivers/usb/typec/tipd/tps6598x.ko.zst`, aliases
  `i2c:tps6598x`, `of:...ti,tps25750`) but it is an **I²C client** driver: it needs the PD
  controller on a kernel I²C adapter with a DT/ACPI node. The EVM is behind a USB CDC
  bridge with no kernel I²C adapter, so there is nothing to bind to. It is not loaded.
* **The `ucsi` interface you see is the laptop's own, not the EVM.** Loaded:
  `ucsi_acpi`, `typec_ucsi`, `typec`; sysfs shows `port0/port1` and
  `ucsi-source-psy-USBC000:001/:002`. Those are the host's ACPI UCSI ports.
  The TPS26750EVM does not present UCSI.
* **Useful Linux-side entry points that do exist:** `/dev/ttyACM1` (the Tiva bridge,
  9600 baud, undocumented protocol) and `/dev/hidraw0` (the USB2ANY, HID). Python has
  `pyserial 3.5` installed; `hid`/`hidapi` is **not** installed.

---

## 7. Practical recipes, best first

### Path A — external I²C master on J2 (fully documented, no reverse engineering)

You already have a NUCLEO-G474RE in this project doing I²C. Wire it to **J2**:

| J2 pin | Signal | To Nucleo |
|---|---|---|
| 12 | I2Ct_SDA | your SDA |
| 14 | I2Ct_SCL | your SCL |
| 7 or 8 | GND | GND |
| 16 | I2Ct_IRQ | optional, an input |

The EVM already has 2.20 kΩ pull-ups on I2Ct to LDO_3V3 [U — read from the schematic],
so **do not add your own**. Do not connect to J5/J9 (§5).

Then speak the documented host interface. Every byte below is from a TI document.

**Register access protocol** [C, SLVUCR7 §1.3.1 Figures 1-2/1-3; SLVSH67 §7.3.11.1.3]:

```
write:  S | addr+W | RegNum | ByteCount=N | D1 | D2 | … | DN | P
read:   S | addr+W | RegNum | Sr | addr+R | ByteCount=N | D1 | … | DN | P
```

Note the **byte-count byte** — it is part of the payload in both directions. Clock
stretching is supported and the controller must tolerate it [C, SLVSH67 §7.3.11.1.1].

**Key registers** [C, SLVUCR7 §3–§4]:

| Offset | Name | Notes |
|---|---|---|
| 0x03 | `MODE` | 4 ASCII bytes: `'APP '`, `'PTCH'`, `'BOOT'` |
| 0x08 | `CMD1` | 4CC command; reads back 0 on success, `'!CMD'` if rejected |
| 0x09 | `DATA1` | command payload / response |
| 0x0F | Version | what the GUI's Identify reads |
| 0x14 / 0x16 / 0x18 | INT_EVENT1 / INT_MASK1 / INT_CLEAR1 | |
| 0x1A | STATUS | |
| 0x3F | POWER_STATUS | |

> SLVUCR7 §4.1 prose says "Command (0x09), Data (0x08)" — that is an erratum. The section
> headings §4.3/§4.4 and the register summary all say **CMD1 = 0x08, DATA1 = 0x09**, and
> TI's own code uses 0x08/0x09. Use 0x08/0x09. [C]

**4CC codes** (ASCII, LSB-first as written) [C, `common/tps25751.h` in TI's usb-pd repo]:
`'I2Cr'` = `49 32 43 72`, `'I2Cw'` = `49 32 43 77`, `'GAID'` = `47 41 49 44`.

**`I2Cw` DATA1 layout** [C, SLVUCR7 Table 5-25 + TI's `tI2CwDataReg`]:

```
byte 0 : ByteCount = 13
byte 1 : target address (7-bit, bit7 reserved)     e.g. 0x6B
byte 2 : length = (payload bytes + 1)   ← +1 accounts for the register-offset byte
byte 3 : register offset                            e.g. 0x02
byte 4..13 : payload (up to 10 bytes)
```

**`I2Cr` DATA1 layout** [C, SLVUCR7 Table 5-24 + TI's `tI2CrDataReg`]:

```
byte 0 : ByteCount = 14
byte 1 : target address                             e.g. 0x6B
byte 2 : register offset
byte 3 : number of bytes to read
```

Response, read back from DATA1: `byte0 = count`, `byte1 = task return code`,
`byte2.. = data` [C, TI's `tI2CrRespReg`].

Reference Python (transport-agnostic — plug in any master; `i2c_rdwr` shown for
`smbus2`, but the same three primitives work over an MCU bridge):

```python
import time

PD_ADDR   = 0x21          # SCAN 0x20..0x23 FIRST — see §2
BQ_ADDR   = 0x6B          # 0x6B = BQ25756, 0x6A = BQ25756E. CHECK THE DIE MARKING.
CMD1, DATA1, MODE = 0x08, 0x09, 0x03

def pd_write_reg(bus, reg, payload: bytes):
    """S | addr+W | reg | count | data... | P"""
    bus.write_i2c_block_data(PD_ADDR, reg, [len(payload)] + list(payload))

def pd_read_reg(bus, reg, n):
    """S | addr+W | reg | Sr | addr+R | count | data... | P"""
    raw = bus.read_i2c_block_data(PD_ADDR, reg, n + 1)
    count = raw[0]
    return bytes(raw[1:1 + count])

def wait_cmd_done(bus, timeout=1.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        c = pd_read_reg(bus, CMD1, 4)
        if c == b'\x00\x00\x00\x00':
            return True
        if c == b'!CMD':
            raise RuntimeError('4CC rejected (!CMD)')
        time.sleep(0.01)
    raise TimeoutError('CMD1 never cleared')

def bq_write(bus, reg, data: bytes):
    """I2Cw passthrough: write `data` to BQ25756 register `reg`."""
    d = bytes([BQ_ADDR, len(data) + 1, reg]) + data
    d = d.ljust(13, b'\x00')
    pd_write_reg(bus, DATA1, d)
    pd_write_reg(bus, CMD1, b'I2Cw')
    wait_cmd_done(bus)
    time.sleep(0.15)                    # TI: ~150 ms between passthrough commands

def bq_read(bus, reg, n):
    """I2Cr passthrough: read n bytes from BQ25756 register `reg`."""
    d = bytes([BQ_ADDR, reg, n]).ljust(14, b'\x00')
    pd_write_reg(bus, DATA1, d)
    pd_write_reg(bus, CMD1, b'I2Cr')
    wait_cmd_done(bus)
    resp = pd_read_reg(bus, DATA1, n + 2)
    status = resp[0]
    if status != 0:
        raise RuntimeError(f'I2Cr task returned status 0x{status:02X}')
    time.sleep(0.15)
    return resp[1:1 + n]

# --- sanity check before anything else ---
# assert pd_read_reg(bus, MODE, 4) == b'APP '     # PTCH = no config loaded

# --- set 5.0 A ---
# ichg = 5000 // 50            # = 100 = 0x064
# word = ichg << 2             # = 0x0190
# bq_write(bus, 0x02, bytes([word & 0xFF, word >> 8]))   # little-endian
# print(bq_read(bus, 0x02, 2).hex())
```

Remember §5: this is a *transient* change.

### Path B — Application Customization Tool, natively on Linux (the durable fix)

**TI ships a Linux x86-64 build.** [C — the gallery listing offers
`platform=linux64`, and I downloaded it: a 14 MB ELF self-extracting installer,
`USBCPD_Application_Customization_Tool-2.0.0.setup-linux-x64.run`]

```
https://dev.ti.com/gallery/dl/USBPD/USBCPD_Application_Customization_Tool/ver/2.0.0?platform=linux64
```

There is also a browser version (Chrome/Firefox/Safari + TI Cloud Agent)
[C, SLVUCP8 §3.1–3.2].

To lower the charge current [C, SLVUCP8 §4.2.2]:

1. Select **Configuration Index 1 or 3** in Question 1 (the ones with a BQ block).
2. Question 11 → **BQ25756**.
3. **Question 15 → Charge Current Limit → enter 5.0 (or 6.0) A.**
   *"enter the Charge Current Limit in units of Ampere (0.4A through 20A, 50mA/bit) …
   This questionnaire configures register 0x02 - ICHG\_REG of the BQ25756."*
4. Other BQ questions worth setting while you are there: Q12 → IAC_DPM (0x06), Q13 →
   VAC_DPM (0x08), Q14 → VFB_REG (0x00), Q16 → ITERM (0x12), Q17 → IPRECHG (0x10).
5. Flash: connect a USB-C cable to the **Data Type-C port J6**, make sure **J11 is
   populated**, Options → Serial Port Configuration (9600), then
   **Flash to Device / Flash to EEPROM**.

**Sequencing rule, and it is not optional** [C, SLVUCP8 §3.4.5 Note]:

> *"connect the BQ25756EVM to the TPS26750EVM **after** configuration has been
> successfully loaded AND both Power Type-C and Data Type-C connections have been removed
> from ports J8 and J6 … If a new configuration needs to be loaded … all connections to
> the BQ25756EVM must be removed until the new configuration is loaded and ports J4, J8,
> and J6 are disconnected."*

The config lands in the CAT24C512 EEPROM at 0x50, which the TPS26750 reads at every boot.
**J14 must be populated** for the EEPROM to be connected. [C, SLVUCP8 Table 2-3]

### Path C — talk to the BQ25756 directly with the USB2ANY

If the USB2ANY on `/dev/hidraw0` is indeed on the BQ25756EVM's J4, the **TI Charger GUI**
drives it and is a browser app ("Evaluate in the cloud"), which the BQ25756EVM user's
guide documents as the supported flow [C, SLUUCT7D §2.4.2]. Whether it works under Linux
Chrome depends on WebHID/TI Cloud Agent support — I could not confirm that from a primary
source. [U]

The USB2ANY's **raw HID packet framing is not publicly documented**. TI publishes only a
Windows DLL API reference (which does leak command IDs, e.g. `Cmd_I2C_ReadInternal = 101
(0x65)`, `Cmd_I2C_WriteInternal = 102 (0x66)`), not the on-wire report layout. [C] TI has
stated on E2E that no portable open-source implementation exists. [C]

**Important:** driving the BQ25756 from the USB2ANY *while the TPS26750 is powered and
mastering I2Cc* is the contention case from §5 — the USB2ANY hangs off the charger's own
I²C, which is the same bus. Only do this with the ribbon to the TPS26750EVM disconnected.

### Path D — the hardware limit (belt and braces, no software at all)

The BQ25756's **ICHG pin** sets a hardware charge-current ceiling, and *"the actual charge
current limit is the **lower** value between ICHG pin setting and I2C register setting"*
[C, SLUSEN5A §6.3.4.1.1]:

```
ICHG_MAX = K_ICHG / R_ICHG          K_ICHG = 48…50…52 A·kΩ   [C, SLUSEN5A K_ICHG spec]
```

| Target | R_ICHG |
|---|---|
| 5 A | **10 kΩ** |
| 6 A | 8.33 kΩ (8.25 kΩ E96 → 6.06 A) |
| 10 A | 4.99 kΩ ← **the EVM's factory default** |

On the BQ25756EVM [C, SLUUCT7D Table 2-4]:

* **JP4 — installed by default — selects the on-board default ICHG resistor, R67 = 4.99 kΩ,
  giving 10 A.** (Guide states "the default ICHG current is set to 10A"; 50/4.99 = 10.02 A.)
* **JP3** connects an *external* ICHG resistor; shorting JP3 to PGND disables the hardware
  limit entirely.

So: **remove JP4, fit a 10 kΩ resistor via JP3 → hard 5 A ceiling**, independent of
anything the PD controller writes. This is the most robust way to guarantee you never
exceed 5 A. Note it also scales the derived limits: `IPRECHG_MAX = 20% × ICHG_MAX` and
`ITERM = 10% × ICHG_MAX`. [C, SLUSEN5A §6.3.4.1.1]

Also relevant: **JP10 (installed) sets the default ILIM_HIZ input current limit to 8 A**;
JP11 is the external-resistor equivalent. [C, SLUUCT7D Table 2-4]

---

## 8. Q3 — BQ25756 registers

All from **SLUSEN5A** (BQ25756 datasheet, Aug 2023 rev. Aug 2026), §6.5. [C]
16-bit registers are **little-endian**: the datasheet writes it as
`I2C REG0x03=[15:8], I2C REG0x02=[7:0]` — i.e. low byte at the lower address.

### 0x02 — Charge Current Limit (the one you want)

```
REG0x02_Charge_Current_Limit  (16-bit, reset 0x0640)
  bits 15:11  reserved
  bits 10:2   ICHG_REG   ← 9-bit field, so the field is SHIFTED LEFT BY 2 in the word
  bits  1:0   reserved
  step 50 mA, offset 0, range 0x008…0x190 (400 mA … 20000 mA), POR 0x190 = 20 A
  "Fast Charge Current Regulation Limit with 5mΩ RBAT_SNS"
  Reset by: REG_RESET, **WATCHDOG**
```

`word = (target_mA / 50) << 2`

| Target | field | 16-bit word | REG0x02 | REG0x03 |
|---|---|---|---|---|
| 4.0 A | 0x050 | `0x0140` | 0x40 | 0x01 |
| **5.0 A** | 0x064 | **`0x0190`** | **0x90** | **0x01** |
| 5.5 A | 0x06E | `0x01B8` | 0xB8 | 0x01 |
| **6.0 A** | 0x078 | **`0x01E0`** | **0xE0** | **0x01** |
| 10 A | 0x0C8 | `0x0320` | 0x20 | 0x03 |
| 20 A (POR) | 0x190 | `0x0640` | 0x40 | 0x06 |

> Watch out for the shift. The datasheet's own example — *"if the register setting is 10 A
> (0xC8)"* — quotes the **field**, while `[Reset = 0x0640]` quotes the **word**
> (0x190 << 2 = 0x640, which cross-checks the shift). 0x190 is the *field* for 20 A and
> the *word* for 5 A. Do not mix them up.
>
> The mA scaling assumes **RBAT_SNS = 5 mΩ**, which the datasheet lists with a single
> typical value and §6.3.4.1.1 states is *required* for the ICHG-pin path. [C] The EVM
> BOM fits **R2 = 5 mΩ (2512) and R24 = 5 mΩ (4320)**. [C, SLUUCT7D BOM]
>
> **Board-revision trap:** SLUUCT7D's Recommended Operating Conditions carries the
> footnote *"Previous EVMs were built with a 2mΩ sense resistor and current EVMs are built
> with a 5mΩ…"*, attached to the **IAC (input) sense resistor**. [C] On an older board
> that makes every **IAC_DPM / IAC_ADC** number off by 2.5×. Measure the resistors before
> trusting any current figure.

### The rest

| Reg | Field | Bits | Layout | Reset | Notes |
|---|---|---|---|---|---|
| 0x00 | `VFB_REG` | 4:0 | 8-bit | 0x10 | 1504 mV + 2 mV/bit, 1504–1566 mV |
| **0x06** | `IAC_DPM` | 10:2 | 16-bit LE | 0x0640 | **input current limit**, 50 mA/bit, 400 mA–20 A, "with 5 mΩ RAC_SNS". Actual = min(IAC_DPM, ILIM_HIZ pin) |
| 0x08 | `VAC_DPM` | 13:2 | 16-bit LE | 0x0348 | input voltage limit, 4200 mV + 20 mV/bit |
| 0x10 | `IPRECHG` | — | 16-bit LE | 0x0140 | 50 mA/bit |
| 0x12 | `ITERM` | — | 16-bit LE | 0x00A0 | 50 mA/bit |
| **0x15** | `WATCHDOG` | **5:4** | 8-bit, reset **0x1D** | `01` = **40 s** | `00`=disable, `01`=40 s, `10`=80 s, `11`=160 s |
| 0x15 | `EN_CHG_TMR` / `CHG_TMR` | 3 / 2:1 | | 1 / `10` | safety timer enabled, 12 h |
| **0x17** | `EN_CHG` | **bit 0** | 8-bit, reset **0xC9** | 1 = enabled | `0`=disable, `1`=enable. Reset by REG_RESET, **WATCHDOG** |
| 0x17 | `WD_RST` | 5 | | 0 | write 1 to kick the watchdog; self-clears |
| 0x17 | `EN_CHG_BIT_RESET_BEHAVIOR` | 3 | | 1 | on WD expiry: `0`→EN_CHG resets to 0, `1`→resets to 1 |
| 0x17 | `EN_HIZ` | 2 | | 0 | |
| 0x17 | `VRECHG` | 7:6 | | `11` = 97.6% × VFB_REG | |
| **0x18** | `EN_ICHG_PIN` | 7 | 8-bit, reset 0xC0 | 1 = pin enabled | set 0 to ignore the ICHG resistor |
| 0x18 | `EN_ILIM_HIZ_PIN` | 6 | | 1 | set 0 to ignore the ILIM_HIZ resistor |
| **0x1C** | `JEITA_ISETC` | 3:2 | 8-bit, reset 0x57 | `01` = **20% × ICHG_REG** | cool-region current: `00`=suspend, `01`=20%, `10`=40%, `11`=100% |
| 0x1C | `EN_JEITA` | 1 | | 1 = enabled | `0` = COLD/HOT only |

### The watchdog — very likely relevant to your "hiccup"

**`ICHG_REG` is listed as "Reset by: REG_RESET, **WATCHDOG**".** [C, SLUSEN5A Table 6-10]
The watchdog default is **40 s** (REG0x15[5:4] = `01`). So if nothing services it, **the
charge current limit snaps back to 20 A after 40 seconds.** `EN_CHG`, `EN_HIZ`,
`EN_ICHG_PIN`, `EN_ILIM_HIZ_PIN`, `ADC_EN` and `EN_CHG_TMR` are likewise watchdog-reset.
(`IAC_DPM`, by contrast, is listed as REG_RESET only.)

Kick it by writing `WD_RST` (0x17 bit 5) = 1, or disable it:

```
REG0x15 = 0x1D & ~0x30 = 0x0D      # WATCHDOG = 00b, everything else default
```

TI's own EVM bring-up procedure does exactly this: *"set WATCHDOG and EN_CHG to disabled.
Set ICHG_REG to 4000mA and IPRECHG to 1000mA"*, then re-enable EN_CHG last.
[C, SLUUCT7D §2.5 step 10d/11]

Whether the TPS26750's event-driven writes also service the BQ watchdog is **not stated in
any document I found** — [U], and worth scoping on the bus.

### Status and fault registers — for diagnosing the hiccup

| Reg | Field | Bits | Meaning |
|---|---|---|---|
| 0x21 | `CHARGE_STAT` | 2:0 | 0=not charging, 1=trickle, 2=precharge, 3=**fast charge (CC)**, 4=taper (CV), 6=top-off, 7=terminated |
| 0x21 | `WD_STAT` | 3 | 1 = **watchdog expired** |
| 0x21 | `VAC_DPM_STAT` | 5 | 1 = in input **voltage** regulation |
| 0x21 | `IAC_DPM_STAT` | 6 | 1 = in input **current** regulation |
| 0x21 | `ADC_DONE_STAT` | 7 | one-shot ADC complete |
| 0x22 | `PG_STAT` | 7 | input power good |
| 0x22 | `TS_STAT` | 6:4 | 0=normal, 1=warm, 2=cool, 3=cold, 4=hot |
| 0x22 | `MPPT_STAT` | 1:0 | |
| 0x23 | `REVERSE_STAT` | 2 | reverse mode on |
| **0x24** | `VAC_UV_STAT` | 7 | input undervoltage |
| 0x24 | `VAC_OV_STAT` | 6 | input overvoltage |
| 0x24 | `IBAT_OCP_STAT` | 5 | battery overcurrent |
| 0x24 | `VBAT_OV_STAT` | 4 | battery overvoltage |
| 0x24 | `TSHUT_STAT` | 3 | thermal shutdown |
| 0x24 | `CHG_TMR_STAT` | 2 | safety timer expired |
| 0x24 | `DRV_OKZ_STAT` | 1 | DRV_SUP out of range |
| 0x25/0x26/0x27 | flags | | latched versions of the above (`_FLAG`) |
| 0x28/0x29/0x2A | masks | | interrupt masks |

**Prime suspects for hiccuping**, in the order I would check them:

1. **Watchdog expiry** (0x21 bit 3) resetting `ICHG_REG` back to 20 A and possibly
   `EN_CHG`. Highest prior — 40 s default, and it explicitly resets both.
2. **Input current/voltage DPM** (0x21 bits 6/5): the source can't hold up, so the charger
   folds back. With a PD contract this is normal-ish but shows as sagging current.
3. **VAC_UV / VAC_OV** (0x24 bits 7/6) — PD renegotiation transients.
4. **JEITA derating — and the default is brutal.** `REG0x1C[3:2] JEITA_ISETC` POR = `01b`
   = **20% × ICHG_REG** in the cool region (T1<TS<T2), and `EN_JEITA` (0x1C[1]) POR = 1.
   [C, SLUSEN5A §6.5.17] So a merely cool pack silently drops you to a fifth of the set
   current. Warm-region current is set by the neighbouring field (40% or 100%). Check
   `TS_STAT` (0x22[6:4]) alongside it. The BQ25756EVM ships JP5+JP6 installed to force TS
   "normal" via a divider [C, SLUUCT7D Table 2-4] — if you moved those to a real
   thermistor, this is a prime suspect.
5. **CHG_TMR_STAT** (0x24 bit 2) — 12 h default safety timer.
6. **TSHUT_STAT** (0x24 bit 3).

ADC readback for live current/voltage [C, SLUSEN5A §6.5.31–6.5.38]:
`0x2B` ADC_Control (`ADC_EN` bit 7; `ADC_RATE` bit 6: 0=continuous, 1=one-shot),
`0x2C` channel enables, then 16-bit LE results:
`0x2D` IAC (0.8 mA/bit, 2's complement, 5 mΩ RAC_SNS),
`0x2F` **IBAT (2 mA/bit, 2's complement, 5 mΩ RBAT_SNS)**,
`0x31` VAC, `0x33` VBAT, `0x37` TS, `0x39` VFB.
`ADC_EN` is watchdog-reset too.

---

## 9. Cautions

* **Never drive I2Cc yourself while the TPS26750 is powered** — multi-controller is not
  supported (§5). Use `I2Cw`/`I2Cr` passthrough, or power down the PD controller first.
* **Don't guess at the ttyACM1 protocol by writing bytes.** The EEPROM holding the PD
  configuration is at 0x50 on the same system; a malformed write can corrupt it. Sniff
  instead (§3).
* **Follow the connect/disconnect order in §7 Path B** when reflashing. TI is emphatic
  about it.
* **S2 on the TPS26750EVM: do not press.** *"Use of S2 can result in the MCU not
  functioning, thus preventing the loading of configurations."* [C, SLVUCP8 §2.6]
* Lowering `ICHG_REG` is always safe in the sense that the effective limit is
  `min(ICHG pin, ICHG_REG)` — you cannot accidentally *raise* current above the 10 A
  hardware ceiling by writing the register. Raising it above the pin setting has no
  effect.
* The BQ25756EVM has no isolation and small creepage; at 48 V treat every terminal as
  hazardous live. [C, SLUUCT7D §1]

---

## 10. Sources

| Doc | Number | URL |
|---|---|---|
| TPS26750EVM User's Guide | SLVUCP8 / SLVUCP8A | https://www.ti.com/lit/ug/slvucp8a/slvucp8a.pdf |
| TPS26750 datasheet | SLVSH67 | https://www.ti.com/lit/ds/symlink/tps26750.pdf |
| TPS26750 Technical Reference Manual | SLVUCR7 | https://www.ti.com/lit/pdf/slvucr7 |
| USBCPD Application Customization Tool UG | SLUUDH4 | https://www.ti.com/lit/ug/sluudh4/sluudh4.pdf |
| BQ25756 datasheet | SLUSEN5A | https://www.ti.com/lit/ds/symlink/bq25756.pdf |
| BQ25756**E** datasheet | SLUSFF4 | https://www.ti.com/lit/ds/symlink/bq25756e.pdf |
| BQ25756EVM User's Guide | SLUUCT7D | https://www.ti.com/lit/ug/sluuct7d/sluuct7d.pdf |
| TPS25751/TPS26750 EEPROM update over I²C | SLVAFL1 | https://www.ti.com/lit/pdf/slvafl1 |
| USB2ANY Interface Adapter UG (hardware only) | SNAU228 | https://www.ti.com/lit/pdf/snau228 |
| USB2ANY SDK API Reference v2.7.0 (Windows DLL API) | — | https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/48/api_5F00_reference_5F00_for_5F00_usb2any_5F00_sdk_5F00_2.7.0.pdf |
| TI USB-PD code examples (I²C passthrough) | — | https://github.com/TexasInstruments/usb-pd |
| ACT download (Linux x86-64) | — | https://dev.ti.com/gallery/dl/USBPD/USBCPD_Application_Customization_Tool/ver/2.0.0?platform=linux64 |
| PMP41115 — 240 W TPS26750 + BQ25756 reference design | TIDT407 | https://www.ti.com/tool/PMP41115 |
| E2E: TIVA firmware not released | thread 1264210 | https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1264210/ |
| E2E: EVM uses on-board TIVA, not USB2ANY | thread 994950 | https://e2e.ti.com/support/interface-group/interface/f/interface-forum/994950/ |
