# USB2ANY HID protocol + BQ charger registers

Research notes. **Nothing here has been written to the hardware.** No bytes were sent to
`/dev/hidraw0` while producing this document.

Status legend used throughout:

- **CONFIRMED** — derived from a primary source (TI binary, TI source code, TI datasheet)
  and, where possible, cross-checked against a second independent source.
- **UNCONFIRMED** — plausible/inferred, not verified. Treat as a hypothesis.

---

## 0. TL;DR

1. The USB2ANY protocol **is fully recoverable from public sources** — and better than that,
   it has been reconstructed here from TI's own shipping code and verified byte-for-byte
   against a real packet capture. See §1–§3. It is safe to write a pure-Python
   `/dev/hidraw0` driver; no `hidapi`, no `pyusb`.
2. The charger paired with the TPS26750 is the **BQ25756** (or **BQ25756E**), not any of the
   BQ25792/98/25703A/25730 candidates. 7-bit address **0x6B** (BQ25756) / **0x6A**
   (BQ25756E). See §5.
3. **The single most important safety fact in this document:** BQ25756 `ICHG_REG` powers up
   at its **maximum, 20 A**, and is **reset back to 20 A whenever the I²C watchdog expires**
   (default watchdog = 40 s). A 40-second watchdog silently reverting the charge limit to
   20 A is an excellent candidate explanation for a charge cycle that "starts, trips,
   retries" against a 7.75 A protector — **time the retry cadence**, ~40 s points at the
   watchdog. See §6.3 and §6.8.
   Corollary: **writing only the low byte of the current register also lands you at 20 A**
   (POR high byte 0x06 → 21 A → clamped to 20 A). Write both bytes, then read back. See §6.1.
4. **Architectural caveat that may block the whole plan:** on the TPS26750EVM the charger
   does *not* sit on the I²C bus your adapter is connected to. The adapter and the PD
   controller (0x21) are on **I2Ct**; the charger is on **I2Cc**, where the TPS26750 itself
   is the bus master, and TI states multi-controller operation is *not supported* on that
   bus. See §5.3 — read this before planning any write.

---

## 1. The adapter on this machine

```
/dev/hidraw0            mode 0666
HID_NAME                Texas Instruments USB2ANY/OneDemo device
HID_ID                  0003:00002047:00000301      (VID 0x2047, PID 0x0301)
HID_UNIQ                49EF086F13001600
driver                  hid-generic
endpoints               EP1 IN interrupt, 64 bytes, bInterval 1
                        EP1 OUT interrupt, 64 bytes, bInterval 1
```

### 1.1 HID report descriptor (read from this machine) — CONFIRMED

Raw, from `/sys/class/hidraw/hidraw0/device/report_descriptor`:

```
06 00 FF  09 01  A1 01
   85 3F  95 3F  75 08  25 01  15 01  09 01  81 02     <- Input  report, ID 0x3F, 63 bytes
   85 3F  95 3F  75 08  25 01  15 01  09 01  91 02     <- Output report, ID 0x3F, 63 bytes
C0
```

Decoded:

| Item | Value |
|---|---|
| Usage Page | Vendor-defined 0xFF00 |
| **Report ID** | **0x3F** — present, therefore **mandatory as the first byte of every write** |
| Report Count | 63 |
| Report Size | 8 bits |
| Total transfer | 1 (report ID) + 63 = **64 bytes** |

Consequences for raw `/dev/hidraw0` I/O:

- Every `os.write()` must be **exactly 64 bytes**, first byte `0x3F`.
- Every `os.read(fd, 64)` returns **64 bytes with the report ID `0x3F` included** as byte 0.
  (Linux hidraw includes the report ID on read when the device declares report IDs.)
- Short writes are rejected; always zero-pad to 64.

This is the standard **MSP430 USB HID-datapipe** convention (report ID 0x3F, byte 1 = count
of valid bytes). The USB2ANY firmware runs on an MSP430F5529.

> Note: the TPS26750EVM's USB bridge is a TM4C123GH6PM, not an MSP430, but it presents the
> same VID/PID and the same USB2ANY protocol. Whether your `/dev/hidraw0` is the EVM's
> on-board bridge or a separate USB2ANY dongle, the framing below applies either way.

---

## 2. The USB2ANY packet format — CONFIRMED

### 2.1 Sources

Three independent sources, all agreeing:

1. **TI's own JavaScript implementation**, shipped inside TI GUI Composer's
   `ti-core-databind` component:
   `components/ti-core-databind/src/internal/reg/USB2ANY.js`
   e.g. <https://github.com/sgs-weather-and-environmental-systems/demo-visualizer/blob/3669f245a10b3e3b9ed753daefc3cb6a1692fe05/components/ti-core-databind/src/internal/reg/USB2ANY.js>
   (same file appears in every mmWave Demo Visualizer distribution). This file contains the
   complete command enum, the field offsets, and the CRC routine, in plain readable source.
2. **TI's `USB2ANY.dll` v2.7.0.0** from the official USB2ANY SDK
   (<https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/14/software-development-kit.zip>),
   disassembled. The packet builder and the CRC table were read directly out of the binary.
3. **A real packet capture** of TI's software driving an LMX2594EVM, published as hardcoded
   hex strings in <https://github.com/Meteopresscz/LMX2594EVM/blob/master/lmx.py>.

The CRC algorithm recovered from (1) and (2) reproduces **all seven** captured packets in (3)
byte-exactly. That is the verification that matters.

> The official *API Reference for the USB2ANY SDK v2.7.0* PDF is **not** a useful source for
> this — its Chapter 3 "Programming and Communications Protocol" contains the single word
> "TBD". The protocol is documented only in code.

### 2.2 Frame layout

One 64-byte HID report carries exactly one USB2ANY packet.

| Offset | Name (TI's own identifier) | Value |
|---:|---|---|
| 0 | *(HID report ID)* | **0x3F** always |
| 1 | *(HID datapipe byte count)* | `payload_len + 8` |
| 2 | `PACKET_ID` | **0x54** = ASCII `'T'` (`PACKET_IDENTIFIER`) |
| 3 | `PACKET_PEC` | **CRC-8**, see §2.3 |
| 4 | `PACKET_PAYLOAD_LEN` | number of payload bytes, 0..54 |
| 5 | `PACKET_TYPE` | **1** = COMMAND on packets you send |
| 6 | `PACKET_FLAGS` | 0 (bit0 = `FLAG_MOREDATA`) |
| 7 | `PACKET_SEQ_NUM` | sequence, 1..254, incrementing |
| 8 | `PACKET_STATUS` | 0 on send; **error code on receive** |
| 9 | `PACKET_COMMAND` | opcode, see §3 |
| 10.. | `PACKET_PAYLOAD` | payload |
| .. 63 | padding | 0x00 |

Constants: `HID_RESERVED = 2`, `PACKET_HEADER_SIZE = 8`, `MAX_PACKET_SIZE = 64`,
**`MAX_PAYLOAD = 54`**.

`PACKET_TYPE` values:

| Value | Name | Meaning |
|---:|---|---|
| 1 | `COMMAND_PACKET` | host → device |
| 2 | `REPLY_PACKET` | device → host, normal reply |
| 3 | `ERROR_PACKET` | device → host, **error; code is at offset 8, as `status - 256`** |
| 4 | `PAYLOAD_PACKET` | device → host, unsolicited data |
| 5 | `INTERRUPT_PACKET` | device → host, interrupt notification |

Verbatim from TI's `sendCommandPacket`:

```js
var packet =
[
    0x3F, buffer.length + PACKET_HEADER_SIZE, PACKET_IDENTIFIER, 0, buffer.length,
    COMMAND_PACKET, 0, this.m_bPacketSeqNum++, 0, cmd
];
for (var i = 0; i < buffer.length; i++) { packet.push(buffer[i]); }
packet[PACKET_PEC] = calculateCRC(packet, PACKET_PAYLOAD_LEN);
```

Sequence numbers: start at 1, increment per packet, wrap `255 -> 1`.
**0 is reserved for asynchronous/unsolicited packets** and must not be used by the host.

### 2.3 Checksum — CONFIRMED

**CRC-8, polynomial 0x07, initial value 0x00, no reflection, no final XOR.**
(The classic CRC-8/ATM a.k.a. CRC-8/SMBUS table.)

Covered range: **from offset 4 (`PACKET_PAYLOAD_LEN`) through the last payload byte,
inclusive** — that is `payload_len + 6` bytes. Offsets 0–3 (report ID, count, `'T'`, and the
CRC byte itself) are **not** covered. Zero padding beyond the payload is not covered.

TI's JS, verbatim:

```js
var calculateCRC = function(buf, offset, len)
{
    var crc = 0;
    len = len || buf.length;
    for (var i = offset; i < len; i++)
    {
        crc = CRC8TABLE[buf[i] ^ crc];
    }
    ...
```

The DLL does the same thing; its 256-byte table at VA `0x1013FDE0` was extracted and matches
a generated poly-0x07 MSB-first table exactly.

Python:

```python
def crc8(data, poly=0x07, crc=0x00):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

# usage: pkt[3] = crc8(pkt[4 : 4 + payload_len + 6])
```

Verification against the real capture (all 7 packets from `lmx.py`, PEC bytes produced by
TI's own Windows software):

| Captured packet | payload_len | PEC in capture | CRC-8 computed |
|---|---:|---|---|
| `3f0c547004010001006a` | 4 | 0x70 | 0x70 ✅ |
| `3f0c54ba04010002000a` | 4 | 0xBA | 0xBA ✅ |
| `3f0c54c904010003006a` | 4 | 0xC9 | 0xC9 ✅ |
| `3f0a54c802010004006802` | 2 | 0xC8 | 0xC8 ✅ |
| `3f1054ce08010005000800010100010100c0` | 8 | 0xCE | 0xCE ✅ |
| `3f1554f30d01000600050101000100000001` | 13 | 0xF3 | 0xF3 ✅ |
| `3f1554d90d01000700060101000100000001` | 13 | 0xD9 | 0xD9 ✅ |

> Aside: the author of `lmx.py` sends `PEC = 0x00` with the comment `# don't care` and reports
> it working, which suggests the firmware does **not** validate the PEC on inbound packets.
> Compute it correctly anyway — it costs nothing and it is what TI's software does.

### 2.4 Response framing — CONFIRMED

Read 64 bytes from `/dev/hidraw0`. Same layout. TI's validation order in `decode()`:

1. `raw[0] == 0x3F`, else ignore the report.
2. length `>= 10` and `<= 64`.
3. `raw[2] == 0x54` and `raw[4] <= 54`, else `ERR_INVALID_PACKET_HEADER` (-28).
4. CRC over `raw[4 .. raw[4]+9]` must equal `raw[3]`, else `ERR_COM_CRC_FAILED` (-13).
5. `raw[9]` (command) must be `<= 0x90`.
6. Dispatch on `raw[5]` (`PACKET_TYPE`).

Matching a reply to a request:

- `raw[7]` (`PACKET_SEQ_NUM`) **echoes the sequence number of the request**.
- `raw[9]` (`PACKET_COMMAND`) **echoes the opcode of the request**. TI's DLL logs
  `"Incorrect response received - command should be 0x%02X, not 0x%02X"` on mismatch.
- Returned data is at offset 10, length `raw[4]`.
- If `raw[5] == 3` (`ERROR_PACKET`), the error code is `raw[8] - 256`.

### 2.5 Error codes

From the SDK API reference, Appendix A. The I²C ones are what you will actually hit:

| Code | Mnemonic |
|---:|---|
| 0 | `ERR_OK` |
| -9 | `ERR_COM_READ_TIMEOUT` |
| -13 | `ERR_COM_CRC_FAILED` |
| -22 | `ERR_INVALID_FUNCTION_CODE` |
| -23 | `ERR_BAD_PACKET_SIZE` |
| -26 | `ERR_PARAM_OUT_OF_RANGE` |
| -27 | `ERR_PACKET_OUT_OF_SEQUENCE` |
| -28 | `ERR_INVALID_PACKET_HEADER` |
| -32 | `ERR_UNSUPPORTED_FIRMWARE` |
| -39 | `ERR_NOT_ENABLED` |
| **-40** | **`ERR_I2C_INIT_ERROR`** |
| **-41** | **`ERR_I2C_READ_ERROR`** |
| **-42** | **`ERR_I2C_WRITE_ERROR`** |
| -43 | `ERR_I2C_BUSY` |
| **-44** | **`ERR_I2C_ADDR_NAK`** — address not acknowledged (wrong address / device absent) |
| **-45** | **`ERR_I2C_DATA_NAK`** |
| -46 | `ERR_I2C_READ_TIMEOUT` |
| -52 | I2C not in Master mode |
| -53 | I2C arbitration lost |
| -54 | I2C pullups require 3.3 V power |

`-53 arbitration lost` is the code to expect if you contend with the TPS26750 on I2Cc (§5.3).

---

## 3. Opcodes — CONFIRMED

Complete enum, transcribed from TI's `USB2ANY.js`. Valid range 0x00..0x8F
(`Cmd_END = 0x90`; the DLL rejects `cmd >= 0x90`).

### 3.1 The ones you need

| Opcode | Name | Payload |
|---:|---|---|
| **0x00** | `Cmd_LoopPacket` | **echo/ping** — payload is returned unchanged |
| **0x0A** | `Cmd_FirmwareVersion_Read` | `[0,0,0,0]` → returns 4 version bytes |
| **0x6A** | `Cmd_Status_GetControllerType` | `[0,0,0,0]` |
| **0x01** | `Cmd_I2C_Control` | `[speed, addrBits, pullups]` |
| **0x02** | `Cmd_I2C_Write` | `[addrHi, addrLo, nBusBytes, b0, b1, ...]` |
| **0x03** | `Cmd_I2C_Read` | `[addrHi, addrLo, nBytes]` — raw read, no register phase |
| **0x64** | `Cmd_I2C_ReadWithAddress` | `[addrHi, addrLo, regAddr, nBytes]` — write-then-read |
| 0x65 | `Cmd_I2C_ReadInternal` | see below |
| 0x66 | `Cmd_I2C_WriteInternal` | 0/1/2-byte internal address form |
| 0x60 | `Cmd_I2C_BlkWriteBlkRead` | block write + block read — **payload layout UNCONFIRMED**, see §3.4 |
| 0x18 | `Cmd_Power_ReadStatus` | `[0, 0, 0x5A, 0x5A]` |
| 0x67 | `Cmd_GetErrorList` | |
| 0x63 | `Cmd_Restart` | |

`Cmd_Status_GetControllerType` (0x6A) reply values:
`0 = CTRLR_UNKNOWN`, `1 = CTRLR_USB2ANY`, `2 = CTRLR_ONEDEMO`, `4 = CTRLR_UNSUPPORTED`.

`Cmd_FirmwareVersion_Read` (0x0A) returns 4 bytes = major, minor, revision, build.
TI's software requires **>= 2.6.2.20**.

`Cmd_I2C_ReadInternal` (0x65) payload, from TI's own comments:

```js
// readData[0-1] - device address
// readData[2]   - size of internal address, in bytes (must be 0, 1, or 2)
// readData[3-4] - number of bytes of data
// readData[5-6] - Internal address of the data to read
```

**There is no I²C bus-scan opcode.** A scan must be synthesised host-side by issuing a
1-byte read to each address 0x08..0x77 and treating `ERR_I2C_ADDR_NAK` (-44) as "absent".
Do not do this on a live battery bus without thinking about §5.3 first.

`Cmd_I2C_Control` parameter values (verbatim from TI's JS):

```js
var I2C_100kHz = 0;   var I2C_400kHz = 1;   var I2C_10kHz  = 2;   var I2C_800kHz = 3;
var I2C_7Bits  = 0;   var I2C_10Bits = 1;
var I2C_PullUps_OFF = 0;   var I2C_PullUps_ON = 1;
```

Note the ordering trap: **speed 0 is 100 kHz, and 10 kHz is code 2, not 0.**

### 3.2 Full opcode table

| Hex | Name | Hex | Name |
|---|---|---|---|
| 0x00 | Cmd_LoopPacket | 0x3C | Cmd_SMBUS_Control |
| 0x01 | Cmd_I2C_Control | 0x3D | Cmd_SMBUS_GetEchoBuffer |
| 0x02 | Cmd_I2C_Write | 0x3E | Cmd_RFFE_RegZeroWrite |
| 0x03 | Cmd_I2C_Read | 0x3F | Cmd_RFFE_RegWrite |
| 0x04 | Cmd_I2CRead_WithAddress | 0x40 | Cmd_RFFE_ExtRegWrite |
| 0x05 | Cmd_GPIO_Write_Control | 0x41 | Cmd_RFFE_ExtRegWriteLong |
| 0x06 | Cmd_GPIO_Write_States | 0x42 | Cmd_RFFE_RegRead |
| 0x07 | Cmd_GPIO_Read_States | 0x43 | Cmd_RFFE_ExtRegRead |
| 0x08 | Cmd_SPI_Control | 0x44 | Cmd_RFFE_ExtRegReadLong |
| 0x09 | Cmd_SPI_WriteAndRead | 0x45 | Cmd_OneWire_SetMode |
| **0x0A** | **Cmd_FirmwareVersion_Read** | 0x46 | Cmd_OneWire_PulseSetup |
| 0x0B | Cmd_MSP430_WordWrite | 0x47 | Cmd_OneWire_PulseWrite |
| 0x0C | Cmd_MSP430_WordRead | 0x48 | Cmd_OneWire_SetState |
| 0x0D | Cmd_MSP430_ByteWrite | 0x54 | Cmd_Packet |
| 0x0E | Cmd_MSP430_ByteRead | 0x55 | Cmd_GPIO_SetCustomPort |
| 0x0F | Cmd_UART_Control | 0x56 | Cmd_GPIO_WriteCustomPort |
| 0x10 | Cmd_MSP430_MemoryWrite | 0x57 | Cmd_GPIO_ReadCustomPort |
| 0x11 | Cmd_MSP430_MemoryRead | 0x58 | Cmd_GPIO_WritePulse |
| 0x12 | Cmd_UART_Write | **0x60** | **Cmd_I2C_BlkWriteBlkRead** |
| 0x13 | Cmd_UART_SetMode | 0x61 | Cmd_InvokeBSL |
| 0x14 | Cmd_UART_Read | 0x62 | Cmd_FirmwareDebugMode |
| 0x15 | Cmd_Local_I2C_Write | 0x63 | Cmd_Restart |
| 0x16 | Cmd_PWM_Write_Control | **0x64** | **Cmd_I2C_ReadWithAddress** |
| 0x17 | Cmd_Power_WriteControl | 0x65 | Cmd_I2C_ReadInternal |
| 0x18 | Cmd_Power_ReadStatus | 0x66 | Cmd_I2C_WriteInternal |
| 0x19 | Cmd_ADC_Control | 0x67 | Cmd_GetErrorList |
| 0x1A | Cmd_ADC_ConvertAndRead | 0x68 | Cmd_LED_SetState |
| 0x1B | Cmd_LED_Control | 0x69 | Cmd_Power_SetVoltageRef |
| 0x1C | Cmd_Clock_Control | **0x6A** | **Cmd_Status_GetControllerType** |
| 0x1D | Cmd_FEC_Control | 0x6B | Cmd_Power_Enable |
| 0x1E | Cmd_FEC_CountAndRead | 0x6C | Cmd_ADC_Enable |
| 0x1F | Cmd_Interrupt_Control | 0x6D | Cmd_ADC_Acquire |
| 0x20 | Cmd_Interrupt_Received | 0x6E | Cmd_ADC_GetData |
| 0x21 | Cmd_EasyScale_Control | 0x6F | Cmd_ADC_GetStatus |
| 0x22 | Cmd_EasyScale_Write | 0x70 | Cmd_ADC_SetReference |
| 0x23 | Cmd_EasyScale_Read | 0x71 | Cmd_Status_GetBoardRevision |
| 0x24 | Cmd_EasyScale_ACK_Received | 0x72 | Cmd_Status_EVMDetect |
| 0x25 | Cmd_GPIO_SetPort | 0x73 | Cmd_ADC_AcquireTriggered |
| 0x26 | Cmd_GPIO_WritePort | 0x74 | Cmd_Power_Notify |
| 0x27 | Cmd_GPIO_ReadPort | 0x75 | Cmd_Digital_Capture |
| 0x32 | Cmd_SMBUS_SendByte | 0x76 | Cmd_Digital_GetData |
| 0x33 | Cmd_SMBUS_WriteByte | 0x77 | Cmd_Digital_GetStatus |
| 0x34 | Cmd_SMBUS_WriteWord | 0x78 | Cmd_EasyScale_WriteAndRead |
| 0x35 | Cmd_SMBUS_WriteBlock | 0x79 | Cmd_DisplayScale_Set |
| 0x36 | Cmd_SMBUS_ReceiveByte | 0x7A | Cmd_DisplayScale_WriteReg |
| 0x37 | Cmd_SMBUS_ReadByte | 0x7B | Cmd_DisplayScale_ReadReg |
| 0x38 | Cmd_SMBUS_ReadWord | 0x7C | Cmd_DisplayScale_WriteAndRead |
| 0x39 | Cmd_SMBUS_ReadBlock | 0x7F | Cmd_Invalid |
| 0x3A | Cmd_SMBUS_ProcessCall | 0x80–0x87 | Stream / SPI stream commands |
| 0x3B | Cmd_SMBUS_BWBRProcessCall | 0x8C–0x8F | Cmd_Port_Setup/Read/Write/WritePulse |

0x28–0x31, 0x49–0x53, 0x59–0x5F, 0x7D, 0x7E, 0x88, 0x89, 0x8B are marked
"Reserved / UNUSED COMMAND" by TI.

### 3.3 I²C payload layouts — CONFIRMED

From TI's `I2C_Interface` in `USB2ANY.js`:

```js
this.readData        = [ i2cAddressHi, i2cAddressLo ];
this.writeData       = [ i2cAddressHi, i2cAddressLo ];
this.writeAddressData= [ i2cAddressHi, i2cAddressLo, 1, 0 ];

// register write:
this.writeData[2] = info.nBytes + 1;      // total bytes placed on the bus
this.writeData[3] = info.addr;            // register address
setBytes(this.writeData, info.nBytes, value, 4);
u2a.sendCommandPacket(Command.Cmd_I2C_Write, this.writeData);

// register read:
this.readData[2] = info.addr;             // register address
this.readData[3] = info.nBytes;
u2a.sendCommandPacket(Command.Cmd_I2C_ReadWithAddress, this.readData);
```

So:

| Command | Payload |
|---|---|
| `0x02 Cmd_I2C_Write` | `[addrHi, addrLo, N, byte0, byte1, ... byteN-1]` where **N = total bytes clocked out after the address**, i.e. 1 register byte + data bytes |
| `0x64 Cmd_I2C_ReadWithAddress` | `[addrHi, addrLo, regAddr, nBytesToRead]` |
| `0x03 Cmd_I2C_Read` | `[addrHi, addrLo, nBytesToRead]` (raw read, no register write phase) |
| `0x01 Cmd_I2C_Control` | `[speed, addrBits, pullups]` |

**Address encoding:** `addrHi:addrLo` is a 16-bit big-endian field carrying the slave address.
The SDK documents the parameter as *"The address of the I2C device. May be 7 or 10 bits"*,
and `Cmd_I2C_Control` separately selects 7- vs 10-bit addressing mode — so the value passed is
the **7-bit address, unshifted** (`0x006B` for the BQ25756, not `0x00D6`).
**UNCONFIRMED in the strict sense** — this is a strong reading of the API docs plus the
10-bit-mode selector, but it is the one thing here not proven by capture. Resolve it
empirically with a harmless read (§4, example 4): if `0x006B` returns `ERR_I2C_ADDR_NAK`
(-44), retry with `0x00D6`.

**Multi-byte ordering on the bus is exactly the order of the bytes in the payload.** The
BQ25756's 16-bit registers are little-endian in register-address order (low byte at the lower
register address), so a 2-byte write starting at 0x02 sends the low byte first.

### 3.4 What is *not* publicly sourced — do not guess these

- **`Cmd_I2C_BlkWriteBlkRead` (0x60) payload layout.** The opcode name is in TI's enum but no
  public code ever builds the packet. For a repeated-START register read, use
  **`Cmd_I2C_ReadWithAddress` (0x64)**, which is fully sourced and does the same job.
- **All SMBus payload layouts (0x32–0x3D).** Enum names only, never constructed. If a device
  needs SMBus framing, prefer the I²C opcodes. (The BQ25756 is plain I²C, so this does not
  affect us here.)

### 3.5 Opcodes to stay away from

- **`Cmd_InvokeBSL` (0x61)** — puts the adapter's MCU into its bootloader. Recovering from
  this needs a reflash.
- **`Cmd_Power_Enable` (0x6B)** and **`Cmd_Power_WriteControl` (0x17)** — drive the adapter's
  3.3 V / 5 V rails onto the target board. Do not enable rails onto a board that is already
  powered from the pack.
- **`I2C_PullUps_ON`** in `Cmd_I2C_Control` — switches in the adapter's internal 1.5 kΩ
  pullups, which will fight the pullups already present on the EVM. Keep it 0.
- **`Cmd_Restart` (0x63)** — resets the adapter mid-session.

---

## 4. Worked byte-by-byte examples

All frames below were generated with the verified CRC-8 routine. Each is shown up to its
last significant byte; **pad with 0x00 to a full 64 bytes before writing.**

### 4.1 Handshake — safe, causes no bus traffic

**`Cmd_FirmwareVersion_Read` (0x0A), seq 1, payload `[0,0,0,0]`:**

```
3F 0C 54 DC 04 01 00 01 00 0A 00 00 00 00   (+ 50 × 0x00 = 64 bytes)
 |  |  |  |  |  |  |  |  |  |  \___________ payload (4 bytes)
 |  |  |  |  |  |  |  |  |  \______________ command 0x0A
 |  |  |  |  |  |  |  |  \_________________ status 0
 |  |  |  |  |  |  |  \____________________ seq 1
 |  |  |  |  |  |  \_______________________ flags 0
 |  |  |  |  |  \__________________________ type 1 = COMMAND
 |  |  |  |  \_____________________________ payload len 4
 |  |  |  \________________________________ CRC-8 over bytes[4..13]
 |  |  \___________________________________ 'T'
 |  \______________________________________ datapipe count = 4+8 = 12
 \_________________________________________ HID report ID
```

Expected reply: 64 bytes, `[0]=0x3F`, `[2]=0x54`, `[5]=2` (REPLY), `[7]=1`, `[9]=0x0A`,
`[4]=4`, and 4 version bytes at `[10..13]`. TI's minimum supported firmware is 2.6.2.20, and
version nibbles are packed one per byte.

**This is the correct first test.** It touches no I²C pins at all.

**`Cmd_Status_GetControllerType` (0x6A), seq 2** — also safe:

```
3F 0C 54 16 04 01 00 02 00 6A 00 00 00 00
```

Reply payload byte 0: `1 = USB2ANY`, `2 = OneDemo`, `0 = unknown`, `4 = unsupported`.

**`Cmd_LoopPacket` (0x00), seq 3, payload `DE AD BE EF`** — pure echo, the most conservative
possible test; the payload comes back unchanged:

```
3F 0C 54 72 04 01 00 03 00 00 DE AD BE EF
```

> Sanity note: these first three frames are exactly what TI's own `u2aOpen()` emits, and the
> captured trace in `lmx.py` opens with `Cmd_Status_GetControllerType` then
> `Cmd_FirmwareVersion_Read`. Two independently written packet builders produced identical
> bytes for the echo frame above (CRC 0x72), which is a good cross-check on the CRC routine.

### 4.2 Configure the I²C master

**`Cmd_I2C_Control` (0x01), seq 3, 100 kHz / 7-bit / pullups OFF:**

```
3F 0B 54 F5 03 01 00 03 00 01 00 00 00
                              ^^ ^^ ^^
                              |  |  pullups 0 = OFF
                              |  addrBits 0 = 7-bit
                              speed 0 = 100 kHz
```

Leave the USB2ANY's internal 1.5 kΩ pullups **OFF** — the EVM already has pullups, and TI
returns `-54 (pullups require 3.3 V power)` if the rail is not enabled. 100 kHz is the safe
choice for a first contact.

### 4.3 I²C READ — worked example

**Read 1 byte from register 0x21 (`Charger_Status_1`) of the BQ25756 at 0x6B.**
`Cmd_I2C_ReadWithAddress` (0x64), seq 4:

```
3F 0C 54 1D 04 01 00 04 00 64 00 6B 21 01
                              ^^ ^^ ^^ ^^
                              |  |  |  nBytes = 1
                              |  |  register 0x21
                              |  addrLo = 0x6B
                              addrHi = 0x00
```

On the wire this produces: `S 0xD6 0x21 Sr 0xD7 <data> P`.

Expected reply: `[5]=2`, `[7]=4`, `[9]=0x64`, `[4]=1`, data byte at `[10]`.

**Read the 2-byte ICHG_REG (registers 0x02 + 0x03)**, seq 5:

```
3F 0C 54 5A 04 01 00 05 00 64 00 6B 02 02
```

Reply payload `[10]` = REG0x02 (low byte), `[11]` = REG0x03 (high byte).
Reassemble as `raw = data[1] << 8 | data[0]`, then `I_mA = (raw >> 2) * 50`.

### 4.4 I²C WRITE — worked example

**Set BQ25756 charge current to 5.0 A** (write 0x0190 to the 16-bit register at 0x02).
`Cmd_I2C_Write` (0x02), seq 6:

```
3F 0E 54 67 06 01 00 06 00 02 00 6B 03 02 90 01
                              ^^ ^^ ^^ ^^ ^^ ^^
                              |  |  |  |  |  REG0x03 = 0x01 (high byte)
                              |  |  |  |  REG0x02 = 0x90 (low byte)
                              |  |  |  starting register 0x02
                              |  |  N = 3 bytes on the bus (reg + 2 data)
                              |  addrLo 0x6B
                              addrHi 0x00
```

On the wire: `S 0xD6 0x02 0x90 0x01 P`.

**Set 6.0 A** (0x01E0), seq 7:

```
3F 0E 54 BC 06 01 00 07 00 02 00 6B 03 02 E0 01
```

**Disable the I²C watchdog** — write REG0x15 = 0x0D (POR 0x1D with `WATCHDOG[5:4]` cleared),
seq 8:

```
3F 0D 54 FD 05 01 00 08 00 02 00 6B 02 15 0D
                              ^^ ^^ ^^ ^^
                              |  |  |  value 0x0D
                              |  |  register 0x15
                              |  N = 2 (reg + 1 data)
                              addr
```

### 4.5 Minimal Python skeleton (no third-party modules)

```python
import os, struct

def crc8(data, poly=0x07, crc=0x00):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

class USB2ANY:
    def __init__(self, path="/dev/hidraw0"):
        self.fd = os.open(path, os.O_RDWR)
        self.seq = 1

    def _frame(self, cmd, payload):
        n = len(payload)
        assert n <= 54
        p = bytearray([0x3F, n + 8, 0x54, 0x00, n, 0x01, 0x00, self.seq, 0x00, cmd])
        p += bytes(payload)
        p[3] = crc8(p[4:4 + n + 6])
        seq = self.seq
        self.seq = 1 if self.seq >= 254 else self.seq + 1
        return bytes(p).ljust(64, b"\x00"), seq

    def cmd(self, cmd, payload=()):
        pkt, seq = self._frame(cmd, payload)
        os.write(self.fd, pkt)
        for _ in range(20):                       # tolerate async packets
            r = os.read(self.fd, 64)
            if len(r) < 10 or r[0] != 0x3F or r[2] != 0x54:
                continue
            if r[3] != crc8(r[4:4 + r[4] + 6]):
                raise IOError("USB2ANY CRC failed")
            if r[5] == 3:                          # ERROR_PACKET
                raise IOError("USB2ANY error %d" % (r[8] - 256))
            if r[5] == 2 and r[7] == seq:          # REPLY_PACKET, our sequence
                if r[9] != cmd:
                    raise IOError("command echo mismatch")
                return bytes(r[10:10 + r[4]])
        raise IOError("no reply")

    # --- convenience ---
    def firmware_version(self):
        return self.cmd(0x0A, [0, 0, 0, 0])

    def i2c_control(self, speed=0, addr_bits=0, pullups=0):
        return self.cmd(0x01, [speed, addr_bits, pullups])

    def i2c_read(self, addr7, reg, n):
        return self.cmd(0x64, [(addr7 >> 8) & 0xFF, addr7 & 0xFF, reg, n])

    def i2c_write(self, addr7, reg, data):
        data = bytes(data)
        return self.cmd(0x02, [(addr7 >> 8) & 0xFF, addr7 & 0xFF,
                               len(data) + 1, reg] + list(data))
```

**Recommended first session, in order, stopping at the first surprise:**

1. `firmware_version()` — proves framing and CRC. No I²C.
2. `i2c_control()`.
3. `i2c_read(0x21, 0x03, 5)` — read the TPS26750's **MODE** register on I2Ct. You have already
   confirmed this device answers and that MODE reads `"APP "`, so it is the ideal oracle: it
   validates the address encoding question from §3.3 against a *known-good* target with a
   *known-good expected value*, and it touches nothing on the charger.
   - Expect ASCII `APP ` in the returned bytes. TPS6598x-family reads are length-prefixed, so
     byte 0 is typically the length (0x04) and bytes 1–4 are `41 50 50 20`. Read 5 bytes and
     look for the pattern rather than assuming the offset.
   - If this NAKs (`-44`), retry with address `0x42` (the 8-bit form of 0x21). Whichever form
     works here is the form to use for the charger too.
4. Only then consider the charger — after reading §5.3.

---

## 5. Which BQ charger, and where it actually sits

### 5.1 The answer: BQ25756 — CONFIRMED

The TPS26750EVM (**SLVUCP8A**) carries **no charger at all**. Its BOM is the TPS26750 (U2),
a second TPS25750 (U7), TPD4S480 (U12), a CAT24C512 EEPROM (U13), a TUSB2036 hub (U14), and a
**TM4C123GH6PM (U22)** acting as the USB-to-I²C bridge — that last part is what enumerates as
your `/dev/hidraw0`.

The charger arrives on a **companion board**. TI ships a TPS26750EVM→BQ25756EVM interposer and
ribbon cable; the procedure is documented in **SLUAAY7, "How to Connect the BQ25756 EVM to the
TPS26750 EVM"** (<https://www.ti.com/lit/pdf/sluaay7>).

Decisively, the **USBCPD Application Customization Tool User Guide (SLUUDH4), Table 3-14**
lists which chargers each PD controller's firmware can drive:

| PD controller | Supported charger |
|---|---|
| **TPS26750 / TPS26750A** | **BQ25756** (1S–14S, full 240 W EPR) and **BQ25756E** (1S–7S) |
| TPS25751 / TPS25751A | BQ25790/2/8, BQ25713, BQ25731, BQ25756, BQ25756E, BQ25690 (≤100 W) |
| TPS25752A | TPS55288, TPS55289, LM251772 |

The reference design is **PMP41115** — 240 W USB-C PD3.1 charger, TPS26750 + BQ25756 +
TPD4S480, 5–48 V in, 48 V/5 A out (<https://www.ti.com/tool/PMP41115>).

**So: BQ25792, BQ25798, BQ25703A, BQ25730 are all off the table for a TPS26750 system.**
They are not supported companions. §7 keeps their data anyway for completeness.

### 5.2 Addresses

| Part | 7-bit | 8-bit W / R | Notes |
|---|---|---|---|
| **BQ25756** | **0x6B** | 0xD6 / 0xD7 | "device 7-bit address is defined as `1101011`" (SLUSEN5A §6.3.10.5) |
| **BQ25756E** | **0x6A** | 0xD4 / 0xD5 | ⚠️ different from the plain BQ25756 |
| TPS26750 (I2Ct) | 0x20–0x23 | — | ADCIN1/ADCIN2 strapped; **your 0x21 = index #2** |
| CAT24C512 EEPROM (I2Cc) | 0x50 | 0xA0 / 0xA1 | TPS26750 patch/config store |

**Scan for both 0x6B and 0x6A** — if the companion board is a BQ25756E you will find nothing
at 0x6B. Neither part has an address-select pin; TI provides address diversity by part number.

### 5.3 ⚠️ The charger is not on your bus — READ BEFORE WRITING

The TPS26750 has **two** I²C ports with opposite roles (TPS26750 datasheet SLVSH67, Table 7-4):

| Bus | TPS26750's role | What is on it |
|---|---|---|
| **I2Ct** | **Target** | the TPS26750 itself at 0x21; this is the host/GUI bus, broken out on **J2** |
| **I2Cc** | **Controller (master)** | EEPROM at 0x50, **and the BQ25756**; broken out on **J5**, and on **J9** pins 4/6 to the companion board |

TI states plainly for I2Cc: *"Connect to a I2C EEPROM, Battery Charger.
**Multi-controller configuration is not supported.**"*

Implications:

- Your adapter reached the PD controller at 0x21, so it is wired to **I2Ct**. The charger is
  **not reachable from there** — an address scan on I2Ct will not find 0x6B/0x6A no matter
  what you do. That is expected, not a fault.
- To reach the charger you must physically move onto **I2Cc** (header J5, or J9 pins 4/6).
- Doing so makes you a **second master on a bus the TPS26750 is actively driving**, which TI
  explicitly does not support. Expect `-53 arbitration lost`, and — worse — expect the
  TPS26750 to overwrite whatever you set, since it programs the charger in response to PD
  negotiation (SLUUDH4 §3.3 maps its config fields directly onto BQ25756 registers 0x00, 0x02,
  0x06, 0x08, 0x10, 0x12).
- The supported way to reach the charger from a host is the TPS26750's **`'I2Cr'` / `'I2Cw'`
  4CC tasks** (TRM SLVUCR7), issued to the PD controller at 0x21 on I2Ct. The PD controller
  then performs the transaction on I2Cc as the legitimate master. There is an interrupt bit
  `INT_EVENTx.I2CControllerNACKed` for failures. **This is the architecturally correct route
  and is worth pursuing before bit-banging I2Cc directly.** The exact 4CC task payload layout
  is in SLVUCR7 and is **not yet transcribed here** — UNCONFIRMED, needs a follow-up pass.

---

## 6. BQ25756 registers — CONFIRMED (datasheet SLUSEN5A, Rev A)

<https://www.ti.com/lit/ds/symlink/bq25756.pdf>

7-bit address **0x6B**. Plain 8-bit register pointer. 16-bit registers are **little-endian in
register-address order**; the datasheet spells this out per register as e.g.
*"I2C REG0x03=[15:8], I2C REG0x02=[7:0]"*.

### 6.1 Charge current — REG0x02 (16-bit, POR 0x0640)

| Field | Bits | Type | POR | Meaning |
|---|---|---|---|---|
| RESERVED | 15:11 | R | 0 | |
| **ICHG_REG** | **10:2** | R/W | **0x190** | Fast-charge current regulation limit, **with 5 mΩ RBAT_SNS** |
| RESERVED | 1:0 | R | 0 | |

- **Bit step: 50 mA.** Range 400 mA – 20 000 mA (field 0x008 – 0x190).
- **POR = 20 000 mA = the maximum.** The datasheet says so explicitly: *"The default ICHG_REG
  is set to maximum code, allowing ICHG pin to limit the current in hardware."*
- **Reset by: REG_RESET and WATCHDOG.** ← see §6.3.
- Field sits at bit 2, so `raw_register = field << 2` and `field = raw >> 2`.
  Sanity check: `400 << 2 = 0x640` = the stated POR. ✅

`ICHG_mA = (raw >> 2) × 50`  and  `raw = (ICHG_mA / 50) << 2`

| Target | field | raw 16-bit | **REG0x02 (low)** | **REG0x03 (high)** |
|---:|---:|---|---|---|
| 4.0 A | 80 | 0x0140 | **0x40** | **0x01** |
| **5.0 A** | **100** | **0x0190** | **0x90** | **0x01** |
| **6.0 A** | **120** | **0x01E0** | **0xE0** | **0x01** |
| 7.0 A | 140 | 0x0230 | 0x30 | 0x02 |
| 20.0 A (POR) | 400 | 0x0640 | 0x40 | 0x06 |

Given a protector that trips hard at **+7.75 A**, 5.0 A or 6.0 A both leave sane margin;
6.0 A leaves 1.75 A of headroom for ripple and measurement error, 5.0 A leaves 2.75 A.

> **Sense-resistor dependency.** All of the above assumes **RBAT_SNS = 5 mΩ**. The datasheet
> §7.2.1.2.7 says RBAT_SNS is *"fixed at 5 mΩ; using a different value is not recommended"*,
> and gives **no rescaling formula**. `I_actual = field × 50 mA × (5 mΩ / R_actual)` is the
> obvious inference but is **UNCONFIRMED**. Verify the fitted resistor before trusting any
> current figure.
>
> ⚠️ **The datasheet contradicts itself on the *input* sense resistor RAC_SNS**: the parameter
> table (§5.3) gives TYP **5 mΩ**, while §7.2.1.2.7 says *"typically **2 mΩ**"*. **Measure the
> board.** If it is 2 mΩ and you assume 5 mΩ, the input current is 2.5× what you intended.

#### 🔴 Read back after writing — this is not optional

Two failure modes make a blind write genuinely dangerous on this part:

1. The field is marked **`Clamped High`**. An out-of-range value does **not** produce an error
   — it silently clamps, and the clamp is at **20 A**.
2. **Writing only the low byte is a live hazard.** If you write `REG0x02 = 0x90` and leave
   `REG0x03` at its POR value `0x06`, the register reads `0x0690` → field 420 → 21 000 mA →
   clamps to **20 A**. You would have intended 5 A and armed 20 A.

**Always write both bytes in one transaction** (the `Cmd_I2C_Write` example in §4.4 does
exactly this — it sends register 0x02 followed by two data bytes, and the charger's pointer
auto-increments into 0x03), then **read the pair back and confirm it equals 0x0190 / 0x01E0**
before enabling charge.

### 6.2 Input current limit — REG0x06 (16-bit, POR 0x0640)

Identical structure to REG0x02: **IAC_DPM** at bits **10:2**, **50 mA/step**, with 5 mΩ
RAC_SNS, POR 0x190 = 20 A. `I2C REG0x07=[15:8], REG0x06=[7:0]`.
Actual input limit is the **lower** of IAC_DPM and the ILIM_HIZ pin resistor.

| Target | field | raw | REG0x06 | REG0x07 |
|---:|---:|---|---|---|
| 3.0 A | 60 | 0x00F0 | 0xF0 | 0x00 |
| 5.0 A | 100 | 0x0190 | 0x90 | 0x01 |

### 6.3 ⚠️ Watchdog — the thing most likely to be biting you

**REG0x15_Timer_Control (8-bit, POR 0x1D)**

| Field | Bits | POR | Values |
|---|---|---|---|
| TOPOFF_TMR | 7:6 | 0b00 | 00 disable / 01 15 min / 10 30 min / 11 45 min |
| **WATCHDOG** | **5:4** | **0b01 = 40 s** | **00 = Disable** / 01 = 40 s / 10 = 80 s / 11 = 160 s |
| EN_CHG_TMR | 3 | 1 | 0 disable / 1 enable charge safety timer |
| CHG_TMR | 2:1 | 0b10 | 00 = 5 h / 01 = 8 h / ... |

**REG0x17_Charger_Control (8-bit, POR 0xC9)**

| Field | Bits | POR | Values |
|---|---|---|---|
| VRECHG | 7:6 | 0b11 | recharge threshold, 11 = 97.6 % × VFB_REG |
| **WD_RST** | **5** | 0 | write **1** to kick the watchdog; self-clears |
| DIS_CE_PIN | 4 | 0 | 1 = ignore the /CE pin |
| **EN_CHG_BIT_RESET_BEHAVIOR** | 3 | **1** | on watchdog expiry: 0 = EN_CHG resets to 0, **1 = EN_CHG resets to 1** |
| EN_HIZ | 2 | 0 | 1 = HIZ |
| EN_IBAT_LOAD | 1 | 0 | battery load enable |
| **EN_CHG** | **0** | **1** | **1 = charging enabled** (POR is *enabled*), plus /CE pin must be LOW |

**Why this matters so much here.** These fields are marked *"Reset by: … WATCHDOG"*:

- `ICHG_REG` (REG0x02) → **reverts to 20 000 mA**
- `IAC_DPM` (REG0x06) → reverts to 20 A
- `EN_ICHG_PIN`, `EN_ILIM_HIZ_PIN` (REG0x18) → revert to enabled
- `EN_CHG` → reverts to **1** (because `EN_CHG_BIT_RESET_BEHAVIOR` defaults to 1)

So with default settings: you write 5 A, and **40 seconds later the watchdog expires, the
charge limit snaps back to 20 A, and charging stays enabled.** The only remaining limit is
the ICHG-pin resistor. That is a very good fit for "it starts, trips the protector, and
retries."

**Therefore, before setting any current: disable the watchdog.**

```
write REG0x15 = 0x0D          # POR 0x1D with WATCHDOG[5:4] = 00
```

or kick `WD_RST` (REG0x17 bit 5 = 1) more often than every 40 s. Disabling is safer for
manual bench work.

TI's own EVM bring-up procedure (BQ25756E EVM guide, SLUUCT7D, step 10d) does exactly this,
in this order:

> *"Click Read All Register, then set **WATCHDOG and EN_CHG to disabled**. Set **ICHG_REG to
> 4000 mA** and IPRECHG to 1000 mA"* … and only then, step 11: *"Set EN_CHG to enabled."*

**Also set `EN_CHG_BIT_RESET_BEHAVIOR` (REG0x17 bit 3) to 0.** Its POR value of 1 is the worst
possible choice here: it means a watchdog expiry *re-enables* charging at the reverted 20 A
limit. With it cleared, a watchdog expiry stops charging instead. This is a cheap belt-and-
braces measure even if you have disabled the watchdog.

**Recommended write order:**

1. Read REG0x21 / 0x22 / 0x24, and REG0x3D, purely to observe (see §6.8).
2. `REG0x17` bit0 `EN_CHG = 0` — stop charging.
3. `REG0x15` = 0x0D — disable the watchdog. **Read it back and confirm bits [5:4] are 00.**
4. `REG0x17` bit3 `EN_CHG_BIT_RESET_BEHAVIOR = 0`.
5. `REG0x02` + `REG0x03` = 5–6 A, **written as one two-byte transaction**.
6. **Read REG0x02/0x03 back and confirm 0x0190 (5 A) or 0x01E0 (6 A).** Do not skip this.
7. `REG0x06` = input limit, if desired (also two bytes, also read back).
8. `REG0x2B` bit7 `ADC_EN = 1` so you can monitor actual current (POR is 0, and it is
   watchdog-reset).
9. `REG0x17` bit0 `EN_CHG = 1` — start charging.
10. Watch REG0x21 `CHARGE_STAT`, REG0x24, and REG0x2F (actual amps).

### 6.4 The ICHG pin — the hardware limit, and the no-software option

**REG0x18_Pin_Control (POR 0xC0)**: bit 7 `EN_ICHG_PIN` = 1 (enabled), bit 6
`EN_ILIM_HIZ_PIN` = 1. Both **reset by WATCHDOG**.

The actual charge limit is **the lower of the ICHG-pin setting and ICHG_REG**. The pin sets:

```
ICHG_MAX = K_ICHG / R_ICHG        K_ICHG = 50 A·kΩ  (min 48, typ 50, max 52)
```

| Target | R_ICHG |
|---:|---|
| 5 A | 10 kΩ |
| **6 A** | **8.33 kΩ** (8.25 kΩ E96 → 6.06 A typ) |
| 7 A | 7.15 kΩ |
| 10 A | 5 kΩ ← EVM default |

Tolerance: with K_ICHG spanning 48–52, an 8.25 kΩ resistor gives **5.82–6.30 A**. Still
comfortably under 7.75 A. A 10 kΩ resistor gives **4.8–5.2 A**, very safe.

On the BQ25756E EVM: **JP4** installed selects the default ICHG resistor = **10 A**; **JP3**
connects an external ICHG resistor (and can be shorted to PGND to disable the hardware limit
entirely). **JP10** sets the default ILIM_HIZ input limit = 8 A.
*(Jumper designators are from SLUUCT7D, the BQ25756**E** EVM guide — confirm against your own
board's silkscreen and its own user guide before cutting anything. UNCONFIRMED for the plain
BQ25756EVM.)*

**This is the strongest fallback in the whole document:** fitting a resistor on JP3 sets a
hard current ceiling that survives watchdog resets, PD renegotiation, power cycles, and any
software mistake. See §8.

### 6.5 Charge voltage — REG0x00 (POR 0x0010)

`VFB_REG` at bits 4:0, **2 mV/step**, offset 1504 mV, range 1504–1566 mV, POR 1536 mV.

Note this is a **feedback-node** voltage, not the pack voltage: the actual battery voltage is
set by the external divider (the EVM's JP1 selects a 7-cell default). Do not attempt to derive
pack voltage from this register without the divider ratio.

### 6.6 Status and fault registers — what to read when it hiccups

All at 8-bit addresses. **0x21–0x24 are live status (not latched); 0x25–0x27 are the
corresponding latched flags** (`Charger_Flag_1/2`, `Fault_Flag`), which are the ones to read
to catch a transient that has already cleared. `0x28–0x2A` are the interrupt masks.

**REG0x21_Charger_Status_1**

| Bit | Field | Meaning |
|---:|---|---|
| 7 | ADC_DONE_STAT | one-shot ADC complete |
| **6** | **IAC_DPM_STAT** | 1 = **in input-current regulation** (ILIM pin or IAC_DPM) |
| **5** | **VAC_DPM_STAT** | 1 = in input-voltage regulation (VAC_DPM or VSYS_REV) |
| **3** | **WD_STAT** | **1 = watchdog timer expired** ← check this first |
| **2:0** | **CHARGE_STAT** | 000 not charging / 001 trickle / 010 pre-charge / **011 fast charge (CC)** / 100 taper (CV) / 110 top-off / 111 termination done |

**REG0x22_Charger_Status_2**

| Bit | Field | Meaning |
|---:|---|---|
| 7 | PG_STAT | 1 = input power good |
| 6:4 | TS_STAT | 000 normal / 001 warm / 010 cool / 011 cold / 100 hot |
| 1:0 | MPPT_STAT | MPPT state |

**REG0x23_Charger_Status_3**: 5:4 `FSW_SYNC_STAT`, 2 `REVERSE_STAT`.

**REG0x24_Fault_Status** — the important one for a trip/retry loop

| Bit | Field | Meaning |
|---:|---|---|
| 7 | VAC_UV_STAT | input undervoltage protection |
| 6 | VAC_OV_STAT | input overvoltage protection |
| **5** | **IBAT_OCP_STAT** | **battery overcurrent detected** ← direct evidence of an overcurrent event |
| 4 | VBAT_OV_STAT | battery overvoltage protection |
| 3 | TSHUT_STAT | thermal shutdown |
| 2 | CHG_TMR_STAT | charge safety timer expired |
| 1 | DRV_OKZ_STAT | DRV_SUP out of range (reads 1 in battery-only mode with ADC off) |

**Diagnostic reading for a "starts, trips, retries" cycle:**

- `WD_STAT = 1` → **watchdog expired; your ICHG_REG has been reverted to 20 A.** Prime suspect.
- `IBAT_OCP_STAT = 1` → charger itself saw overcurrent.
- `IAC_DPM_STAT = 1` → you are input-limited, not battery-limited.
- `CHARGE_STAT` cycling `011 → 000 → 011` → restart loop.
- `PG_STAT` dropping → the *source* is collapsing (PD renegotiation), a different failure.

### 6.7 Live current measurement — REG0x2F

**REG0x2F_IBAT_ADC**, 16-bit, `I2C REG0x30=[15:8], REG0x2F=[7:0]`.
**2 mA/step, two's complement**, ±20 000 mA. Requires the ADC to be enabled via REG0x2B.

`I_mA = signed16(raw) × 2`

This is the cheapest way to confirm what the charger is actually delivering, and to catch it
running at 20 A after a watchdog reset.

Requires `ADC_EN` = **REG0x2B bit 7**, which is **POR 0 and watchdog-reset** — so if IBAT_ADC
reads a constant 0, check that bit before believing it.

### 6.8 Identifying the part, and reading the retry cadence

**REG0x3D** returns the part number: **0x12** for the BQ25756 (`PART_NUM = 0b010`). Read this
first — it settles the BQ25756-vs-BQ25756E question and confirms you are talking to the right
device at all.

**The retry period is diagnostic.** Time the interval between charge attempts:

| Observed cadence | Most likely cause |
|---|---|
| **~1 second** | The charger's own **battery OCP** — `VICHG_OC` is 120–170 mV across 5 mΩ = **24–34 A**, and the part *"stops charging and attempts to restart after one second"*. Note that threshold is far above your 7.75 A protector, so **your FET board will trip long before the charger's own OCP does** — a 1 s cadence more likely means something else tripped and released. |
| **~40 / 80 / 160 seconds** | **The I²C watchdog** (§6.3). This is the cadence to look for. |
| irregular, with `PG_STAT` dropping | The *source* is collapsing — PD renegotiation, not a charger fault. |

Because the charger's internal OCP sits at 24–34 A and the pack protector trips at 7.75 A, the
protector will essentially always act first. That means `IBAT_OCP_STAT` in REG0x24 may well
stay **0** during a trip, and the absence of that flag is *not* evidence that current was low.
Trust REG0x2F (actual amps) and the retry cadence over the fault bits.

Other retry sources worth ruling out: the safety timer `CHG_TMR_STAT` (REG0x24 bit 2, POR
12 h), and thermal shutdown `TSHUT_STAT`. Note this part has **no thermal-regulation (TREG)
status and no VSYS-regulation status** — those simply do not exist in its register map, so do
not go looking for them.

---

## 7. The other candidates — for completeness only

**None of these are TPS26750 companions** (§5.1). Recorded so the candidate list is closed out.

| Part | Topology | Max charge current | Cells | 7-bit addr |
|---|---|---:|---|---|
| BQ25792 | buck-boost, integrated FETs | **5 A** (10 mA step) | 1–4S | 0x6B |
| BQ25798 | buck-boost, integrated, MPPT | **5 A** (10 mA step) | 1–4S | 0x6B |
| BQ25703A | buck-boost NVDC controller | 8.128 A (64 mA step, 10 mΩ) | 1–4S | 0x6B |
| BQ25730 | buck-boost NVDC controller | 16.256 A | 1–5S | 0x6B |
| BQ25720 | buck-boost NVDC controller | 16.256 A | 1–4S | **0x09** (SMBus) |
| BQ25756 | buck-boost controller, ext. FETs | **0.4–20 A** (50 mA step, 5 mΩ) | 1–14S | **0x6B** |
| BQ25756E | buck-boost controller, ext. FETs | same | 1–7S | **0x6A** |

Notes on the 7-bit/8-bit confusion, which is a real trap in this family:

- `0x6B << 1 = 0xD6`. **0x6B and 0xD6 are the same address**, 7-bit vs 8-bit notation.
  Linux `i2cdetect`, `ioctl(I2C_SLAVE)`, and the USB2ANY payload all want the **7-bit** form.
- **BQ25703A's datasheet contains a typo**: §8.5 states *"The I2C address is D6h
  (`1101101_X`)"*, but `1101101` is 0x6D, and `1101101_0` = 0xDA ≠ 0xD6. The binary digits are
  transposed; the value consistent with D6h is `1101011` = 0x6B, matching the rest of the
  family. Use **0x6B**. *(The typo diagnosis is an inference from arithmetic plus the
  self-consistent BQ25730 text — TI has published no erratum. UNCONFIRMED.)*
- **BQ25720 and BQ25710 genuinely are 0x09**, not 0x6B — they are the SMBus variants. Their
  8-bit write address 0x12 is often mistaken for a register number.

### 7.1 Charge-current registers, all candidates side by side

**Do not extrapolate BQ25756 register numbers onto these — the maps are entirely different.**

| | **BQ25792 / BQ25798** | **BQ25703A** | **BQ25730** | **BQ25756** |
|---|---|---|---|---|
| Register | `REG03` | `ChargeCurrent()` | `ChargeCurrent()` | `REG0x02` |
| Address pair | 0x03 / 0x04 | 0x02 / 0x03 | 0x02 / 0x03 | 0x02 / 0x03 |
| **Byte order** | **MSB at 0x03** ⚠️ | **LSB first** | **LSB first** | **LSB first** |
| Field bits | [8:0] | [12:6] | [12:6] | [10:2] |
| Mask | 0x01FF | 0x1FC0 | 0x1FC0 | 0x07FC |
| **LSB** | **10 mA** | **64 mA** @10 mΩ | **128 mA** @5 mΩ | **50 mA** @5 mΩ |
| Range | 50–**5000 mA** | 64–8128 mA | 0–16256 mA | 400–20000 mA |
| POR | 1–2 A (per cell strap) | **0 A** | **0 A** | **20 A** 🔴 |
| Watchdog resets it to | 1–2 A (safe) | **0 A** (safe) | **0 A** (safe) | **20 A** 🔴 |
| Sense resistor | integrated, none | RSR 10 mΩ | RSR **5 mΩ** | RBAT_SNS 5 mΩ |

**Values to write:**

| Part | ~5.0 A | ~6.0 A |
|---|---|---|
| BQ25792 / BQ25798 | 500 = `0x01F4`, wire from 0x03: `01 F4` | ❌ **impossible**, 5000 mA is the ceiling. A naive 6 A write overflows the 9-bit field and silently lands at **880 mA** |
| BQ25703A (10 mΩ) | 78 → 4992 mA, word `0x1380`, wire from 0x02: `80 13` | 93 → 5952 mA, word `0x1740`, wire: `40 17` |
| BQ25730 (5 mΩ) | 39 → 4992 mA, word `0x09C0`, wire from 0x02: `C0 09` | 46 → 5888 mA, word `0x0B80`, wire: `80 0B` |
| BQ25756 (5 mΩ) | word `0x0190`, wire from 0x02: `90 01` | word `0x01E0`, wire: `E0 01` |

Handy property of the BQ25703A/BQ25730 encoding: because the LSB sits at bit 6 and the step is
64 mA (BQ25703A), **the 16-bit word value in decimal equals the current in mA**.

### 7.2 Supporting registers, all candidates

| | BQ25792 / BQ25798 | BQ25703A | BQ25730 | BQ25756 |
|---|---|---|---|---|
| **Input current limit** | `REG06` [8:0], 10 mA, **max 3300 mA** | `0F/0Eh`: all bits in **0Fh[6:0]**, 50 mA, offset 50 mA | `0Fh[6:0]`, 100 mA @5 mΩ | `REG0x06` [10:2], 50 mA |
| 3 A / 5 A | `0x012C` / **out of range** | `0Fh=0x3B` / `0Fh=0x63` | `0Fh=0x1E` / `0Fh=0x32` | `0x00F0` / `0x0190` |
| **Charge enable** | `REG0F` bit 5 `EN_CHG`, **1 = enable** | `00h` bit 0 `CHRG_INHIBIT`, **0 = enable** | `00h` bit 0 `CHRG_INHIBIT`, **0 = enable** | `REG0x17` bit 0 `EN_CHG`, **1 = enable** |
| **Watchdog reg** | `REG10` [2:0], POR 40 s, **000 = off** | `01h` [6:5], POR 175 s, **00 = off** (POR 0xE2 → 0x82) | `01h` [6:5], POR 175 s, **00 = off** (POR 0xE7 → 0x87) | `REG0x15` [5:4], POR 40 s, **00 = off** (0x1D → 0x0D) |
| **Watchdog kick** | `REG10` bit 3 `WD_RST` | — | — | `REG0x17` bit 5 `WD_RST` |
| **Charge voltage** | `REG01` [10:0], 10 mV | `05/04h`, 16 mV | `05/04h`, 8 mV | `REG0x00` [4:0], 2 mV — **FB trim, not pack volts** |
| **Status** | `0x1B`–`0x21` live; `0x22`–`0x27` latched **clear-on-read** | `21/20h` | `21/20h` | `0x21`–`0x24` live; `0x25`–`0x27` latched clear-on-read |
| **Charge state field** | `0x1C` [7:5] CHG_STAT | `21h` bits 2/1 FCHRG/PCHRG | `21h` bits 2/1 | `0x21` [2:0] CHARGE_STAT |
| **Watchdog expired flag** | `0x1B` bit 5 WD_STAT | — | — | `0x21` bit 3 WD_STAT |
| **Overcurrent flag** | `0x20` bit 3 IBAT_OCP | `20h` bit 6 FAULT_BATOC | `20h` bit 6 BATOC | `0x24` bit 5 IBAT_OCP |
| **Thermal** | `0x1D` bit 2 TREG; `0x21` bit 2 TSHUT | `20h` (TSHUT) | — | `0x24` bit 3 TSHUT (no TREG) |

Traps specific to these parts, worth recording so they are not rediscovered the hard way:

- **BQ25703A vs BQ25730 have the same register addresses but different LSBs** (64 vs 128 mA)
  and the **RSNS bit polarity is inverted**: `31h[2] RSNS_RSR` is `0 = 10 mΩ` (POR) on the
  BQ25703A but `1 = 5 mΩ` (POR) on the BQ25730. Same bit, opposite meaning, factor-of-two
  current error.
- **BQ25703A/BQ25730 require the low byte to be written first.** *"If host writes MSB byte
  first, the command will be ignored."*
- **BQ25703A Tables 23/24 are titled "Charge Current Register (14h)" — that is a TI typo.**
  The address is `03/02h`, confirmed by the section heading, Figure 32, and the mainline Linux
  driver (`BQ25703_CHARGE_CURRENT 0x02`).
- **BQ25703A/BQ25730 `EN_LWPWR` (`01h` bit 7, POR 1)** disables the ADC. Clear it before
  trusting any current readback. It does not gate charging — `CHRG_INHIBIT` does.
- **BQ25792/98: changing the `CELL` field (`REG0A[7:6]`) resets ICHG, VSYSMIN and VREG to POR.**
- **BQ25730 battery OVP at 104 % of `ChargeVoltage()` shuts the converter with no status bit
  at all.** If a BQ25730 ever is the part, check `ChargeVoltage()` against the pack first.

---

## 8. Alternatives, ranked

The protocol question is settled, so these are ranked by *risk to the pack*, not by
difficulty.

### 8.1 Best: set the hardware ICHG resistor (§6.4)

Fit a resistor on the ICHG pin (JP3 on the EVM) — **8.25 kΩ for ~6 A, 10 kΩ for ~5 A**. The
limit is then enforced by analogue hardware:

- survives the I²C watchdog expiring,
- survives the TPS26750 reprogramming the charger after PD renegotiation,
- survives power cycles and any software bug,
- and the charger takes **the lower** of pin and register, so it composes safely with anything
  software does later.

For a pack behind a 7.75 A comparator, this is the correct primary defence. Everything else
should be layered on top of it, not instead of it.

### 8.2 Ask the PD controller to do it — the architecturally correct software route

Use the TPS26750's **`'I2Cw'` / `'I2Cr'` 4CC tasks** (TRM SLVUCR7) via the PD controller at
0x21 on I2Ct — the bus you are already on. The PD controller performs the transfer on I2Cc as
the legitimate master, so there is no bus contention. Needs the 4CC task payload format
transcribed from SLVUCR7 (**not yet done — follow-up item**).

Better still, the durable fix is to **change the charger configuration the TPS26750 applies**,
using the USBCPD Application Customization Tool (SLUUDH4) — its config fields map directly
onto BQ25756 registers 0x00/0x02/0x06/0x08/0x10/0x12, and the result is flashed to the EEPROM
at 0x50. Then the PD controller sets 6 A itself, every time, and never fights you.

### 8.3 Direct I²C from the USB2ANY on I2Cc — works, but contended

Move to header J5 / J9 pins 4/6 and drive 0x6B directly with §4. Expect arbitration loss and
expect the TPS26750 to overwrite your settings on PD events (§5.3). Acceptable for a
*read-only* diagnostic pass (reading 0x21/0x24/0x2F to find out why it trips) — that is
low-risk and high-information. Less good as the permanent control path.

### 8.4 STM32G474 Nucleo as the I²C master

The same objection as 8.3 applies — it is still a second master on I2Cc. It is *not* an
improvement over the USB2ANY for this particular bus, since the problem is topological, not
protocol-level. It becomes the right answer only if you take the TPS26750 out of the loop
entirely (hold it in reset / disconnect I2Cc) and let the STM32 own the charger. Since the
G474 already runs the BMS monitor, having the thing that watches the pack also own the charge
limit is arguably the cleanest end state — but it means giving up PD-negotiated power
management.

### 8.5 TI GUI on Windows/Wine

Not needed for protocol discovery. Still useful as a cross-check oracle: the BQ25756 GUI does
the correct init order and can confirm which registers the TPS26750 is writing. The USB2ANY
DLL is 32-bit and uses `WriteFile`/`ReadFile` on the HID handle, so Wine + `winusb`/hidraw
*may* work, but this is **UNCONFIRMED and untested**, and is strictly a convenience, not a
requirement.

---

## 9. Open items

1. **7-bit vs 8-bit address in the USB2ANY payload** (§3.3) — resolve with a harmless read
   against the TPS26750 at 0x21, which is known to respond.
2. **TPS26750 `'I2Cr'`/`'I2Cw'` 4CC task payload layout** — needs transcribing from SLVUCR7.
   This is the blocker for the recommended software route (§8.2).
3. **RBAT_SNS actual value** on the specific companion board — all current figures in §6.1
   scale directly with it, and the rescaling formula itself is an inference, not documented.
4. **RAC_SNS actual value** — the BQ25756 datasheet says 5 mΩ in §5.3 and 2 mΩ in §7.2.1.2.7.
   These cannot both be right. Measure it.
5. **Which companion board** is actually attached: BQ25756 (0x6B) or BQ25756E (0x6A). Read
   REG0x3D (§6.8) to settle it.
6. **EVM jumper designators** in §6.4 are from the BQ25756**E** EVM guide; confirm against the
   board in hand.
7. **BQ25792 16-bit byte order** is not stated anywhere in its datasheet — inferred. Only
   matters if that part ever turns out to be in play. (It fails safe if reversed: a byte-
   swapped write decodes to 10 mA, not a high current.)

## 10. Sources

Protocol:
- TI `USB2ANY.js` (GUI Composer `ti-core-databind`) — <https://github.com/sgs-weather-and-environmental-systems/demo-visualizer/blob/3669f245a10b3e3b9ed753daefc3cb6a1692fe05/components/ti-core-databind/src/internal/reg/USB2ANY.js>
  - newer/richer variants of the same TI file (identical opcode enum, more I²C helpers):
    <https://raw.githubusercontent.com/tejash-9/mmWave-Demo-Visualizer_3.6.0/master/components/ti-core-databind/src/internal/reg/USB2ANY.js>
    and <https://raw.githubusercontent.com/arghasen10/mmWave-Demo-Visualizer/master/components/ti-core-databind/src/internal/reg/USB2ANY.js>
- USB2ANY SDK 2.7.0 (`USB2ANY.dll`, API reference PDF) — <https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/14/software-development-kit.zip>
- Packet capture — <https://github.com/Meteopresscz/LMX2594EVM/blob/master/lmx.py>
- USB2ANY hardware user's guide, SNAU228 — <https://www.ti.com/lit/ug/snau228/snau228.pdf>
- udev rule precedent — <https://github.com/matthuszagh/nixos/blob/master/modules/hardware/ti-usb2any.nix>
  (`ENV{ID_VENDOR_ID}=="2047", ENV{ID_MODEL_ID}=="0301", MODE:="666"` — your `/dev/hidraw0` is
  already 0666, so no action needed)

Dead ends checked, so they need not be rechecked:
- The SDK API reference PDF Chapter 3 "Programming and Communications Protocol" is **"TBD"**.
- `zuyazii/LMX2594-Tools/scripts/usb2anyapi.py` and `bsnelling9/PGA305OWICalibration/USB2Any.cs`
  are ctypes / P-Invoke wrappers around the closed Windows DLL — no wire format.
- **No PyPI package** named `usb2any`, `pyusb2any`, or `ti-usb2any` exists. Nothing to install;
  the driver in §4.5 is stdlib-only by necessity and by preference.

Hardware / silicon:
- TPS26750 datasheet SLVSH67 — <https://www.ti.com/lit/ds/symlink/tps26750.pdf>
- TPS26750 TRM SLVUCR7 — <https://www.ti.com/lit/pdf/slvucr7>
- TPS26750EVM user's guide SLVUCP8A — <https://www.ti.com/lit/ug/slvucp8a/slvucp8a.pdf>
- SLUAAY7, connecting BQ25756EVM to TPS26750EVM — <https://www.ti.com/lit/pdf/sluaay7>
- SLUUDH4, USBCPD Application Customization Tool — <https://www.ti.com/lit/ug/sluudh4/sluudh4.pdf>
- PMP41115 reference design — <https://www.ti.com/tool/PMP41115>
- **BQ25756 datasheet SLUSEN5A** (Rev A, Aug 2026) — <https://www.ti.com/lit/ds/symlink/bq25756.pdf> ← the one that matters
- BQ25756E datasheet SLUSFF4 — <https://www.ti.com/lit/ds/symlink/bq25756e.pdf>
- BQ25756E EVM user's guide SLUUCT7D
- BQ25798 datasheet SLUSDV2C — <https://www.ti.com/lit/ds/symlink/bq25798.pdf>
- BQ25703A datasheet SLUSCU1A — <https://www.ti.com/lit/ds/symlink/bq25703a.pdf>
- BQ25730 datasheet SLUSE65A — <https://www.ti.com/lit/ds/symlink/bq25730.pdf>
- BQ25792 datasheet SLUSDG1C — ⚠️ `ti.com/lit/ds/symlink/bq25792.pdf` and `ti.com/product/BQ25792`
  both currently return **HTTP 404**; the figures in §7 came from a mirror of SLUSDG1C
  cross-checked against the TI-hosted BQ25798 (identical register map) and against the
  mainline Linux driver `include/linux/mfd/bq257xx.h` / `drivers/mfd/bq257xx.c`.
  Beware: `ti.com/lit/ds/sluse07/sluse07.pdf` is **BQ28Z610**, not BQ25792.
