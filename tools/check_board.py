#!/usr/bin/env python3
"""
One-shot health check for a MAX17320 BMS board.

Drives the monitor firmware's console over the ST-Link VCP and answers, in
one pass: is the gauge alive, is the bus wired and pulled up, is the die
provisioned, how many NVM writes are left, is the pack sane, and has any
protection tripped. Ends with a verdict and the next step.

    python3 tools/check_board.py                 # auto-detects /dev/ttyACM*
    python3 tools/check_board.py --port /dev/ttyACM0

Read-only: it never writes a register and never touches NVM. Close the
serial monitor and the dashboard first — the port is exclusive.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")

# Registers the firmware's NV check compares, with the amps/mAh each
# target value means, so a diff line explains itself.
MEANING = {
    "nIChgTerm":   "charge-termination current",
    "nFullCapNom": "13224 mAh nominal",
    "nFullCapRep": "11400 mAh full",
    "nDesignCap":  "11400 mAh design  <-- 0x0000 means never provisioned",
    "nIPrtTh1":    "+10.08 A charge / -10.24 A discharge",
    "nJEITAC":     "7.00 A in every temperature zone",
    "nStepChg":    "no derating above 4.12 V/cell",
    "nODSCTh":     "OCTH +7.75 A, SC -20 A, OD -12.5 A",
    "nUVPrtTh":    "undervoltage protection",
    "nOVPrtTh":    "overvoltage protection",
    "nRComp0":     "cell model resistance",
    "nTPrtTh3":    "permanent-fail temperature",
    "nNVCFG0":     "which NV values the gauge actually uses",
    "nProtCfg2":   "protection config + checksum",
}


class Console:
    """Thin wrapper that sends a key and collects output until a marker."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        self.ser = serial.Serial(port, baud, timeout=1)
        time.sleep(0.6)
        self.reset_state()

    def reset_state(self) -> None:
        """ESC leaves any submenu or half-typed command, then 'd' puts the
        firmware back on the ANSI dashboard — a known starting point."""
        self.ser.reset_input_buffer()
        self.ser.write(b"\x1bd")
        time.sleep(1.2)
        self.ser.reset_input_buffer()

    def send(self, keys: bytes, until: str | None = None, timeout: float = 20.0) -> str:
        self.ser.reset_input_buffer()
        for k in keys:
            self.ser.write(bytes([k]))
            time.sleep(0.12)
        buf, t0 = "", time.time()
        while time.time() - t0 < timeout:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf += chunk.decode("utf-8", "replace")
            if until and until in buf:
                break
            time.sleep(0.2)
        return ANSI.sub("", buf)

    def close(self) -> None:
        try:
            self.reset_state()
        finally:
            self.ser.close()


def section(title: str) -> None:
    print(f"\n{title}\n{'-' * len(title)}")


def check(port: str) -> int:
    con = Console(port)
    problems: list[str] = []
    todo: list[str] = []

    # ---- 1. is anything there ----
    section("1. gauge")
    out = con.send(b"p", until="press h for keys", timeout=25)
    alive = "probe: OK" in out
    dev = re.search(r"DevName 0x([0-9A-F]{4})", out)
    if alive:
        print(f"  alive — both I2C addresses answer, DevName 0x{dev.group(1) if dev else '????'}")
    else:
        print("  NOT RESPONDING")
        problems.append("the gauge does not answer on either address")

    # ---- 2. the bus itself ----
    section("2. bus")
    out = con.send(b"b", until="->", timeout=20)
    levels = re.findall(r"(SCL|SDA)\s+\w+\s+(HIGH|LOW)\s+(HIGH|LOW)", out)
    for name, floating, pulled in levels:
        print(f"  {name}: floating {floating}, with internal pull-up {pulled}")
    idle_high = all(f == "HIGH" for _, f, _ in levels) and len(levels) == 2
    if idle_high:
        print("  pull-ups present and alive")
    elif levels:
        print("  NO WORKING PULL-UPS on the bus")
        problems.append("no pull-ups: fit 5k from SCL and SDA to 3V3 (CN7-16)")

    # ---- 3. provisioning ----
    section("3. NV configuration")
    out = con.send(b"!1", until="registers differ", timeout=90)
    diffs = re.findall(r"0x([0-9A-F]{3}) (\w+)\s+is 0x([0-9A-F]{4}), want 0x([0-9A-F]{4})", out)
    total = re.search(r"(\d+) of (\d+) registers differ", out)
    provisioned = True
    for addr, name, is_v, want in diffs:
        note = MEANING.get(name, "")
        print(f"  0x{addr} {name:<13} is 0x{is_v}, want 0x{want}   {note}")
        if name == "nDesignCap" and is_v == "0000":
            provisioned = False
    if total:
        n = int(total.group(1))
        print(f"  {n} of {total.group(2)} registers differ")
        if n == 0:
            print("  this die already carries the profile")
        else:
            todo.append(f"provision it: {n} registers differ from the pack profile")
    if not provisioned:
        print("  nDesignCap = 0x0000 -> this die has NEVER been provisioned")

    out = con.send(b"3", until="remaining", timeout=20)
    left = re.search(r"NVM writes used: (\d+) of 7\s+remaining: (\d+)", out)
    if left:
        print(f"  NVM writes used {left.group(1)} of 7, remaining {left.group(2)}")
        if left.group(2) == "0":
            problems.append("no NVM writes left — this die can never be reconfigured")
    con.send(b"q", timeout=5)

    # ---- 4. the pack ----
    section("4. pack")
    raw = con.send(b"j", timeout=12)
    sample = None
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                sample = json.loads(line)
            except json.JSONDecodeError:
                pass
    con.send(b"d", timeout=5)

    if not sample or not sample.get("ok", False):
        print("  no valid sample — the gauge is not reporting")
        problems.append("no measurements available")
    else:
        cells = sample.get("cells") or []
        print(f"  pack     {sample.get('v_pack')} V (BATT)    PACK+ {sample.get('v_pckp')} V")
        if len(cells) >= 2:
            imbalance = abs(cells[0] - cells[1]) * 1000.0
            print(f"  cells    {cells[0]:.4f} / {cells[1]:.4f} V    imbalance {imbalance:.1f} mV")
            if not sample.get("cells_plausible", True):
                print("  CELL CHANNEL CLAMPED AT 0 V — open tap or miswired stack")
                problems.append("a cell channel reads exactly 0 V: check the pack taps")
            elif imbalance > 50:
                print("  imbalance is high for a healthy pack")
                todo.append(f"investigate the {imbalance:.0f} mV cell imbalance")
            for i, v in enumerate(cells[:2], 1):
                if v < 2.5:
                    problems.append(f"cell {i} is at {v:.3f} V — deeply discharged")
        print(f"  current  {sample.get('i')} A ({sample.get('flow')})")
        print(f"  temp     {sample.get('temp')} C    die {sample.get('die_temp')} C")
        if not sample.get("supply_ok", True):
            print("  SUPPLY BELOW THE 4.2 V DATASHEET MINIMUM")
            problems.append("pack voltage is under the gauge's minimum supply")
        if sample.get("soc") is not None:
            print(f"  soc      {sample.get('soc')} %    {sample.get('cap_mah')} / "
                  f"{sample.get('full_mah')} mAh")
        else:
            print("  soc      suppressed — no valid capacity profile")

        # ---- 5. protection ----
        section("5. protection")
        print(f"  CHG FET {'ON' if sample.get('chg_fet') else 'OFF'}    "
              f"DIS FET {'ON' if sample.get('dis_fet') else 'OFF'}")
        if sample.get("permfail") or sample.get("nbatt_status"):
            print(f"  PERMANENT FAILURE  nBattStatus 0x{sample.get('nbatt_status', 0):04X}")
            problems.append("permanent failure latched — the pack may be unrecoverable")
        elif sample.get("faulted"):
            print(f"  protection tripped, ProtStatus 0x{sample.get('prot_status', 0):04X}")
            problems.append("a protection is currently tripped")
        else:
            print("  no faults, no permanent failure")
        if sample.get("prot_alrt"):
            print(f"  sticky alert history: 0x{sample.get('prot_alrt'):04X} "
                  "(something tripped at some point)")
            todo.append("read the sticky alert history before clearing it with 'c'")

    # ---- verdict ----
    section("verdict")
    if problems:
        print("  BLOCKED:")
        for p in problems:
            print(f"    - {p}")
    else:
        print("  board is healthy")
    if todo:
        print("  next:")
        for t in todo:
            print(f"    - {t}")
    if not provisioned and not problems:
        print("\n  to provision: '!' then 2 (shadow, free) -> 5 (restart model)")
        print("  check the capacity, then 4 and type BURN to commit")

    con.close()
    return 1 if problems else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (default: first ACM/USB port)")
    args = ap.parse_args()

    port = args.port
    if not port:
        for p in list_ports.comports():
            if "ACM" in p.device or "usbmodem" in p.device or "USB" in p.device:
                port = p.device
                break
    if not port:
        sys.exit("no serial port found — is the board plugged in? (or pass --port)")

    print(f"checking the board on {port} — this takes about a minute")
    return check(port)


if __name__ == "__main__":
    sys.exit(main())
