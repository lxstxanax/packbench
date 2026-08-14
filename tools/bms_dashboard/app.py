#!/usr/bin/env python3
"""
Live dashboard for the MAX17320 BMS monitor firmware.

Reads the JSON stream the STM32G474 emits on the ST-Link Virtual COM Port
and shows pack state, protection status and history. Press 'j' in the
firmware's terminal UI to switch it from the ANSI dashboard to JSON, or
just start this app -- it sends the 'j' itself on connect.

    ./install.sh     # once: venv + PySide6 + pyqtgraph + pyserial
    ./run.sh         # auto-detects /dev/ttyACM*

Recording: type a pack label in the header and press Record. Samples go to
CSV next to this file, one file per pack, so two packs can be compared
afterwards without re-reading a scrollback buffer.

Design notes:
  * Voltage and current get separate plots with independent scales -- one
    frame with two y-axes invites false correlations.
  * Status colours (good/warning/serious/critical) are reserved for state
    and never reused as a series colour; every status pill carries a glyph
    and a word so it never depends on colour alone.
  * Series colours are categorical slots 1 (blue) and 2 (orange), stepped
    per theme; both clear the CVD and contrast gates in light and dark.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Optional

import serial
from serial.tools import list_ports
from PySide6 import QtCore, QtGui, QtWidgets
import pyqtgraph as pg

HISTORY = 900          # samples kept on the plots (~7.5 min at 2 Hz)
STALE_AFTER = 3.0      # seconds without a sample before the header says so

THEMES = {
    "dark": {
        "plane": "#0d0d0d", "surface": "#1a1a19",
        "ink": "#ffffff", "ink2": "#c3c2b7", "muted": "#898781",
        "grid": "#2c2c2a", "border": "#2c2c2a",
        "series_v": "#3987e5", "series_i": "#d95926",
        "good": "#0ca30c", "warning": "#fab219",
        "serious": "#ec835a", "critical": "#d03b3b",
    },
    "light": {
        "plane": "#f9f9f7", "surface": "#fcfcfb",
        "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
        "grid": "#e1e0d9", "border": "#e1e0d9",
        "series_v": "#2a78d6", "series_i": "#eb6834",
        "good": "#0ca30c", "warning": "#fab219",
        "serious": "#ec835a", "critical": "#d03b3b",
    },
}

GLYPH = {"good": "✓", "warning": "▲", "serious": "■",
         "critical": "✕", "idle": "●"}

FAULT_SEVERITY = {
    "PermFail": "critical", "OVP": "critical", "UVP": "critical",
    "OCCP": "critical", "ODCP": "critical", "ChgWDT": "serious",
    "TooHotC": "serious", "TooColdC": "serious", "TooHotD": "serious",
    "DieHot": "serious", "Imbalance": "warning", "Qovflw": "warning",
    "PreqF": "warning", "LDet": "warning", "Full": "good",
    "Ship": "warning", "ResDFault": "warning",
}

# ProtStatus / ProtAlrt bits, D15 down to D1. D0 differs per register:
# ProtStatus.Ship vs ProtAlrt.LDet.
PROT_BITS = ["ChgWDT", "TooHotC", "Full", "TooColdC", "OVP", "OCCP",
             "Qovflw", "PreqF", "Imbalance", "PermFail", "DieHot",
             "TooHotD", "UVP", "ODCP", "ResDFault"]

CSV_FIELDS = ["iso_time", "mcu_ms", "seq", "v_pack", "v_pckp", "cell1", "cell2",
              "i", "i_avg", "p", "soc", "cap_mah", "full_mah", "age", "cycles",
              "temp", "die_temp", "flow", "chg_fet", "dis_fet", "full", "ship",
              "faulted", "permfail", "tte_s", "ttf_s",
              "status", "prot_status", "prot_alrt", "nbatt_status"]


def decode_prot(word: int, is_alrt: bool) -> list[str]:
    """Set bit names of a ProtStatus/ProtAlrt word."""
    names = [name for i, name in enumerate(PROT_BITS) if word & (1 << (15 - i))]
    if word & 1:
        names.append("LDet" if is_alrt else "Ship")
    return names


def autodetect_port() -> Optional[str]:
    for p in list_ports.comports():
        if "ACM" in p.device or "usbmodem" in p.device or "USB" in p.device:
            return p.device
    return None


class SerialWorker(QtCore.QThread):
    """Reads JSON lines off the port and hands them to the GUI thread."""

    sample = QtCore.Signal(dict)
    connected = QtCore.Signal(str)
    disconnected = QtCore.Signal(str)

    def __init__(self, port: str, baud: int) -> None:
        super().__init__()
        self.port, self.baud = port, baud
        self._stop = False

    def stop(self) -> None:
        self._stop = True

    def run(self) -> None:
        while not self._stop:
            try:
                with serial.Serial(self.port, self.baud, timeout=1) as ser:
                    self.connected.emit(self.port)
                    # ESC first: the firmware may be sitting in a submenu or
                    # half-way through a typed command, where a bare 'j'
                    # would be swallowed as the cancel key and the stream
                    # would never start. ESC returns it to the top level.
                    ser.write(b"\x1bj")
                    while not self._stop:
                        raw = ser.readline()
                        if not raw:
                            continue
                        line = raw.decode("utf-8", "replace").strip()
                        if not line.startswith("{"):
                            continue          # ANSI frame or log noise
                        try:
                            self.sample.emit(json.loads(line))
                        except json.JSONDecodeError:
                            pass
            except serial.SerialException as exc:
                self.disconnected.emit(str(exc))
                for _ in range(20):           # 2 s, but stay interruptible
                    if self._stop:
                        return
                    self.msleep(100)


class MetricCard(QtWidgets.QFrame):
    """Hero number with a label above and context below."""

    def __init__(self, title: str, theme: dict) -> None:
        super().__init__()
        self.theme = theme
        self._tone: Optional[str] = None
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(2)

        self.title = QtWidgets.QLabel(title)
        self.value = QtWidgets.QLabel("--")
        self.sub = QtWidgets.QLabel("")
        for w in (self.title, self.value, self.sub):
            layout.addWidget(w)

        # Tabular figures stop the hero number from twitching as digits
        # change. Qt style sheets have no font-variant-numeric, so ask the
        # font for the OpenType feature instead of writing CSS Qt ignores.
        font = self.value.font()
        try:
            font.setFeature(QtGui.QFont.Tag("tnum"), 1)
            self.value.setFont(font)
        except (AttributeError, ValueError):
            pass                    # Qt < 6.7: digits just shift a little
        self.apply_theme(theme)

    def apply_theme(self, theme: dict) -> None:
        self.theme = theme
        self.setStyleSheet(
            f"QFrame {{ background: {theme['surface']};"
            f" border: 1px solid {theme['border']}; border-radius: 6px; }}")
        self.title.setStyleSheet(
            f"border: none; font-size: 10px; color: {theme['ink2']};")
        # ink2 rather than muted: at 10 px, muted (#898781) lands around
        # 3.5:1 on the light surface, under the 4.5:1 line for body text.
        # These sub-lines carry real values (PACK+, avg current, die temp,
        # imbalance), so they have to stay readable in both themes.
        self.sub.setStyleSheet(
            f"border: none; font-size: 10px; color: {theme['ink2']};")
        self._style_value()

    def _style_value(self) -> None:
        color = self.theme[self._tone] if self._tone else self.theme["ink"]
        self.value.setStyleSheet(
            f"border: none; font-size: 22px; font-weight: 650; color: {color};")

    def set(self, value: str, sub: str = "", tone: Optional[str] = None) -> None:
        if tone != self._tone:      # re-parsing the sheet 10x a second for
            self._tone = tone       # an unchanged colour is pure overhead
            self._style_value()
        self.value.setText(value)
        self.sub.setText(sub)


class StatusPill(QtWidgets.QLabel):
    """Glyph + word + colour, so state never rides on colour alone."""

    def __init__(self, text: str, tone: str, theme: dict) -> None:
        super().__init__(f"{GLYPH.get(tone, GLYPH['idle'])} {text}")
        self.tone = tone
        self.restyle(theme)

    def restyle(self, theme: dict) -> None:
        color = theme.get(self.tone, theme["ink2"])
        self.setStyleSheet(
            f"padding: 3px 9px; border-radius: 9px; font-size: 11px;"
            f" font-weight: 600; color: {color};"
            f" background: {theme['surface']};"
            f" border: 1px solid {theme['border']};")


class Dashboard(QtWidgets.QMainWindow):
    def __init__(self, port: str, baud: int, theme_name: str) -> None:
        super().__init__()
        self.theme_name = theme_name
        self.theme = THEMES[theme_name]
        self.port = port
        self.last_sample_at = 0.0
        self.link_text = "connecting…"

        self.t_hist: deque[float] = deque(maxlen=HISTORY)
        self.v_hist: deque[float] = deque(maxlen=HISTORY)
        self.i_hist: deque[float] = deque(maxlen=HISTORY)
        self.t0 = time.time()

        self.csv_file = None
        self.csv_writer = None

        self.setWindowTitle("MAX17320 BMS dashboard")
        # Tall enough for both plots plus the whole table; at 800 the layout
        # could only meet its minimums and Qt grew the window anyway.
        self.resize(1080, 900)
        self._build()
        self._apply_theme()

        self.worker = SerialWorker(port, baud)
        self.worker.sample.connect(self.on_sample)
        self.worker.connected.connect(
            lambda p: setattr(self, "link_text", f"connected {p}"))
        self.worker.disconnected.connect(
            lambda e: setattr(self, "link_text", f"disconnected: {e}"))
        self.worker.start()

        self.ui_timer = QtCore.QTimer(self)
        self.ui_timer.timeout.connect(self._refresh_header)
        self.ui_timer.start(500)

    # ---------------- layout ----------------
    def _build(self) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(10)

        # --- header ---
        header = QtWidgets.QHBoxLayout()
        self.title_lbl = QtWidgets.QLabel("MAX17320  2S4P")
        self.title_lbl.setStyleSheet("font-size: 15px; font-weight: 700;")
        header.addWidget(self.title_lbl)
        header.addStretch(1)

        header.addWidget(QtWidgets.QLabel("pack:"))
        self.pack_name = QtWidgets.QLineEdit()
        self.pack_name.setPlaceholderText("pack-A")
        self.pack_name.setFixedWidth(120)
        header.addWidget(self.pack_name)

        self.record_btn = QtWidgets.QPushButton("Record")
        self.record_btn.setCheckable(True)
        self.record_btn.toggled.connect(self.on_record_toggled)
        header.addWidget(self.record_btn)

        self.theme_btn = QtWidgets.QPushButton("Theme")
        self.theme_btn.clicked.connect(self.toggle_theme)
        header.addWidget(self.theme_btn)

        self.link_lbl = QtWidgets.QLabel(self.link_text)
        header.addWidget(self.link_lbl)
        root.addLayout(header)

        # --- metric cards ---
        cards = QtWidgets.QHBoxLayout()
        cards.setSpacing(8)
        self.card_pack = MetricCard("PACK VOLTAGE", self.theme)
        self.card_curr = MetricCard("CURRENT", self.theme)
        self.card_soc = MetricCard("STATE OF CHARGE", self.theme)
        self.card_temp = MetricCard("TEMPERATURE", self.theme)
        self.card_cells = MetricCard("CELLS", self.theme)
        self.cards = [self.card_pack, self.card_curr, self.card_soc,
                      self.card_temp, self.card_cells]
        for c in self.cards:
            cards.addWidget(c)
        root.addLayout(cards)

        # --- warning banner: why a reading cannot be trusted ---
        self.warn_lbl = QtWidgets.QLabel("")
        self.warn_lbl.setWordWrap(True)
        self.warn_lbl.hide()
        root.addWidget(self.warn_lbl)

        # --- SOC meter ---
        self.soc_bar = QtWidgets.QProgressBar()
        self.soc_bar.setRange(0, 1000)
        self.soc_bar.setTextVisible(False)
        self.soc_bar.setFixedHeight(14)
        root.addWidget(self.soc_bar)

        # --- status pills ---
        self.pill_row = QtWidgets.QHBoxLayout()
        self.pill_row.setSpacing(6)
        self.pill_row.addStretch(1)
        root.addLayout(self.pill_row)

        # --- plots, one series each, independent scales ---
        self.plot_v = pg.PlotWidget()
        self.plot_i = pg.PlotWidget()
        # The current label is stacked rather than run out in one 40-character
        # line: rotated upright it is taller than the axis whenever the plot
        # sits near its minimum height, and pyqtgraph then clips it at both
        # ends ("rrent [A] + charging / − disc").
        for plot, label in ((self.plot_v, "pack voltage  [V]"),
                            (self.plot_i, "current  [A]<br>"
                                          "+ charging<br>− discharging")):
            plot.setMinimumHeight(150)
            plot.showGrid(x=False, y=True, alpha=0.15)
            plot.setMenuEnabled(False)
            plot.setLabel("left", label)
            plot.getAxis("bottom").setLabel("seconds")
            root.addWidget(plot, 1)     # spare height goes to the plots,
                                        # not by starving the table below

        self.curve_v = self.plot_v.plot([], [], pen=pg.mkPen(
            self.theme["series_v"], width=2))
        self.curve_i = self.plot_i.plot([], [], pen=pg.mkPen(
            self.theme["series_i"], width=2))
        # Zero line on the current plot: the sign is the whole point.
        self.zero_line = pg.InfiniteLine(pos=0, angle=0,
                                         pen=pg.mkPen(self.theme["muted"], width=1))
        self.plot_i.addItem(self.zero_line)

        # --- table view: the same numbers, readable without colour ---
        # _update_table fills a fixed set of rows; claim their height up
        # front so the table is whole from the first frame.
        self.table = QtWidgets.QTableWidget(10, 2)
        self.table.setHorizontalHeaderLabels(["field", "value"])
        self.table.verticalHeader().setVisible(False)
        # Qt's default 30 px row spends the window's height on air; tie the
        # row to the text it holds so the plots keep the rest.
        self.table.verticalHeader().setDefaultSectionSize(
            self.fontMetrics().height() + 8)
        self.table.horizontalHeader().setSectionResizeMode(
            0, QtWidgets.QHeaderView.ResizeToContents)   # field names elided
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.table.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        self._fit_table()
        root.addWidget(self.table)

    def _apply_theme(self) -> None:
        t = self.theme
        self.setStyleSheet(
            f"QMainWindow, QWidget {{ background: {t['plane']}; color: {t['ink']}; }}"
            f"QLabel {{ color: {t['ink2']}; }}"
            f"QLineEdit, QPushButton {{ background: {t['surface']};"
            f" color: {t['ink']}; border: 1px solid {t['border']};"
            f" border-radius: 4px; padding: 4px 10px; }}"
            f"QPushButton:checked {{ color: {t['critical']}; }}"
            f"QTableWidget {{ background: {t['surface']}; color: {t['ink2']};"
            f" gridline-color: {t['grid']}; border: 1px solid {t['border']}; }}"
            f"QHeaderView::section {{ background: {t['surface']};"
            f" color: {t['muted']}; border: none; padding: 4px; }}"
            f"QProgressBar {{ background: {t['grid']}; border: none;"
            f" border-radius: 7px; }}"
            f"QProgressBar::chunk {{ background: {t['series_v']};"
            f" border-radius: 7px; }}")

        for c in self.cards:
            c.apply_theme(t)

        for plot in (self.plot_v, self.plot_i):
            plot.setBackground(t["surface"])
            for axis in ("left", "bottom"):
                plot.getAxis(axis).setPen(pg.mkPen(t["muted"]))
                plot.getAxis(axis).setTextPen(pg.mkPen(t["muted"]))
        self.curve_v.setPen(pg.mkPen(t["series_v"], width=2))
        self.curve_i.setPen(pg.mkPen(t["series_i"], width=2))
        self.zero_line.setPen(pg.mkPen(t["muted"], width=1))

        # Pills and the warning banner style themselves as they are built,
        # so without this they keep the old theme's surface -- white blobs
        # on the dark plane -- until the next sample, or for ever if the
        # link has gone quiet, which is exactly when they are being read.
        for i in range(self.pill_row.count()):
            pill = self.pill_row.itemAt(i).widget()
            if isinstance(pill, StatusPill):
                pill.restyle(t)
        self._style_warn()

    def toggle_theme(self) -> None:
        self.theme_name = "light" if self.theme_name == "dark" else "dark"
        self.theme = THEMES[self.theme_name]
        self._apply_theme()

    # ---------------- recording ----------------
    def on_record_toggled(self, on: bool) -> None:
        if on:
            name = self.pack_name.text().strip() or "pack"
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            path = Path(__file__).resolve().parent / f"{name}-{stamp}.csv"
            self.csv_file = path.open("w", newline="")
            self.csv_writer = csv.DictWriter(
                self.csv_file, fieldnames=CSV_FIELDS, extrasaction="ignore")
            self.csv_writer.writeheader()
            self.record_btn.setText(f"Recording → {path.name}")
        else:
            if self.csv_file:
                self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
            self.record_btn.setText("Record")

    # ---------------- data ----------------
    def _refresh_header(self) -> None:
        stale = self.last_sample_at and (time.time() - self.last_sample_at) > STALE_AFTER
        text = self.link_text + ("   STALE" if stale else "")
        color = self.theme["critical"] if stale else self.theme["muted"]
        self.link_lbl.setText(text)
        self.link_lbl.setStyleSheet(f"color: {color}; font-size: 11px;")

    def on_sample(self, d: dict) -> None:
        self.last_sample_at = time.time()

        if not d.get("ok", False):
            self._set_pills([(f"LINK: {d.get('error', 'no data')}", "critical")])
            return

        v_pack = float(d.get("v_pack", 0.0))
        i_amp = float(d.get("i", 0.0))
        cells = [float(c) for c in d.get("cells", [])]
        flow = d.get("flow", "idle")

        # The firmware sends null for every model output it cannot stand
        # behind (no NV profile, supply under spec, a cell channel clamped
        # at zero). Show a dash rather than inventing a number.
        soc = d.get("soc")
        self._update_warnings(d)

        self.card_pack.set(f"{v_pack:.3f} V",
                           f"PACK+ {float(d.get('v_pckp', 0)):.3f} V")
        self.card_curr.set(
            f"{i_amp:+.3f} A",
            f"avg {float(d.get('i_avg', 0)):+.3f} A    {float(d.get('p', 0)):.2f} W",
            "good" if flow == "charging" else None)
        if soc is None:
            self.card_soc.set("—", "no valid capacity profile", "warning")
            self.soc_bar.setValue(0)
        else:
            soc = float(soc)
            self.card_soc.set(f"{soc:.1f} %",
                              f"{d.get('cap_mah')} / {d.get('full_mah')} mAh")
            self.soc_bar.setValue(int(max(0.0, min(100.0, soc)) * 10))

        self.card_temp.set(f"{float(d.get('temp', 0)):.1f} °C",
                           f"die {float(d.get('die_temp', 0)):.1f} °C")

        if len(cells) >= 2:
            if d.get("cells_plausible", True):
                imbalance_mv = abs(cells[0] - cells[1]) * 1000.0
                self.card_cells.set(f"{cells[0]:.3f} / {cells[1]:.3f}",
                                    f"imbalance {imbalance_mv:.0f} mV",
                                    "warning" if imbalance_mv > 50 else None)
            else:
                # A clamped zero would show up as a 4 V imbalance between
                # two live cells, which is not what is happening.
                self.card_cells.set(f"{cells[0]:.3f} / {cells[1]:.3f}",
                                    "channel clamped — tap open", "critical")

        # --- history ---
        self.t_hist.append(time.time() - self.t0)
        self.v_hist.append(v_pack)
        self.i_hist.append(i_amp)
        self.curve_v.setData(list(self.t_hist), list(self.v_hist))
        self.curve_i.setData(list(self.t_hist), list(self.i_hist))

        # --- pills ---
        pills = []
        if d.get("permfail"):
            pills.append(("PERMANENT FAILURE", "critical"))
        elif d.get("faulted"):
            pills.append(("PROTECTION TRIPPED", "critical"))
        elif d.get("ship"):
            pills.append(("SHIP MODE", "warning"))
        elif d.get("full"):
            pills.append(("FULL", "good"))
        else:
            pills.append(("OK", "good"))

        pills.append((flow.upper(), "good" if flow == "charging" else "idle"))
        pills.append(("CHG FET " + ("ON" if d.get("chg_fet") else "OFF"),
                      "good" if d.get("chg_fet") else "critical"))
        pills.append(("DIS FET " + ("ON" if d.get("dis_fet") else "OFF"),
                      "good" if d.get("dis_fet") else "critical"))
        for name in decode_prot(int(d.get("prot_status", 0)), False):
            pills.append((name, FAULT_SEVERITY.get(name, "warning")))
        self._set_pills(pills)

        self._update_table(d)
        self._write_csv(d, cells)

    def _update_warnings(self, d: dict) -> None:
        """States where the gauge returns well-formed but meaningless
        numbers. Each one gets said out loud rather than being papered
        over with a plausible-looking value."""
        lines = []
        if not d.get("provisioned", True):
            lines.append("GAUGE NOT PROVISIONED (nDesignCap = 0) — capacity, "
                         "SOC, TTE/TTF and Age are meaningless. Load the pack "
                         "profile from the firmware console: '!' then 2.")
        if not d.get("cells_plausible", True):
            lines.append("CELL WIRING — a cell channel reads exactly 0 V. In 2S, "
                         "Cell2 = BATTS − CELL1, so an open top tap goes negative "
                         "and clamps to zero. Check J1 / the top cell group.")
        if not d.get("supply_ok", True):
            lines.append(f"SUPPLY {float(d.get('v_pack', 0)):.3f} V is under the "
                         "4.2 V datasheet minimum — measurements are out of spec.")

        if not lines:
            self.warn_lbl.hide()
            return

        self.warn_lbl.setText("  ⚠  " + "\n  ⚠  ".join(lines))
        self._style_warn()
        self.warn_lbl.show()

    def _style_warn(self) -> None:
        self.warn_lbl.setStyleSheet(
            f"color: {self.theme['critical']}; background: {self.theme['surface']};"
            f" border: 1px solid {self.theme['critical']}; border-radius: 6px;"
            f" padding: 8px; font-size: 11px; font-weight: 600;")

    def _set_pills(self, pills: list[tuple[str, str]]) -> None:
        while self.pill_row.count():
            item = self.pill_row.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        for text, tone in pills:
            self.pill_row.addWidget(StatusPill(text, tone, self.theme))
        self.pill_row.addStretch(1)

    def _fit_table(self) -> None:
        """Hold the table at exactly its content height.

        It has no business scrolling: ten fixed rows are the whole point of
        it. Left to the layout it loses every fight with the expanding plots
        and ends up one row tall behind a scrollbar.
        """
        header = self.table.horizontalHeader()
        height = (max(header.height(), header.sizeHint().height())
                  + sum(self.table.rowHeight(r)
                        for r in range(self.table.rowCount()))
                  + 2 * self.table.frameWidth())
        # Called again on every sample: the first measurement happens before
        # the widget is polished, and comes out a few pixels short -- which,
        # with the scrollbar off, would hide the last row for good.
        if height != self.table.maximumHeight():
            self.table.setFixedHeight(height)

    def _update_table(self, d: dict) -> None:
        alerts = decode_prot(int(d.get("prot_alrt", 0)), True)
        rows = [
            ("pack voltage (BATT)", f"{float(d.get('v_pack', 0)):.4f} V"),
            ("pack+ (PCKP)", f"{float(d.get('v_pckp', 0)):.4f} V"),
            ("cells", "   ".join(f"{c:.4f} V" for c in d.get("cells", []))),
            ("current / average",
             f"{float(d.get('i', 0)):+.4f} A    {float(d.get('i_avg', 0)):+.4f} A"),
            ("soc / capacity",
             "— (no valid profile)" if d.get("soc") is None else
             f"{float(d['soc']):.1f} %    "
             f"{d.get('cap_mah')} / {d.get('full_mah')} mAh"),
            ("age / cycles",
             "—" if d.get("age") is None else f"{d.get('age')} %    {d.get('cycles')}"),
            ("time to full / empty",
             f"{d.get('ttf_s')} s / {d.get('tte_s')} s"),
            ("Status / ProtStatus",
             f"0x{int(d.get('status', 0)):04X}    0x{int(d.get('prot_status', 0)):04X}"),
            ("ProtAlrt (sticky)",
             f"0x{int(d.get('prot_alrt', 0)):04X}    "
             f"{', '.join(alerts) if alerts else 'none'}"),
            ("nBattStatus", f"0x{int(d.get('nbatt_status', 0)):04X}"),
        ]
        if self.table.rowCount() != len(rows):
            self.table.setRowCount(len(rows))
        for row, (name, value) in enumerate(rows):
            self.table.setItem(row, 0, QtWidgets.QTableWidgetItem(name))
            self.table.setItem(row, 1, QtWidgets.QTableWidgetItem(value))
        self._fit_table()

    def _write_csv(self, d: dict, cells: list[float]) -> None:
        if not self.csv_writer:
            return
        row = dict(d)
        row["iso_time"] = datetime.now().isoformat(timespec="milliseconds")
        row["mcu_ms"] = d.get("t")
        row["cell1"] = cells[0] if len(cells) > 0 else ""
        row["cell2"] = cells[1] if len(cells) > 1 else ""
        self.csv_writer.writerow(row)
        self.csv_file.flush()

    def closeEvent(self, event) -> None:
        self.worker.stop()
        self.worker.wait(1500)
        if self.csv_file:
            self.csv_file.close()
        super().closeEvent(event)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (default: first ACM/USB port)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--theme", choices=("dark", "light"), default="dark")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if not port:
        print("no serial port found — is the board plugged in? "
              "(or pass --port)", file=sys.stderr)
        return 1

    pg.setConfigOptions(antialias=True)
    app = QtWidgets.QApplication(sys.argv)
    win = Dashboard(port, args.baud, args.theme)
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
