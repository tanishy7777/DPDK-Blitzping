#!/usr/bin/env python3
"""
BlitzPing GUI (PyQt6)
---------------------
A reference GUI to drive your existing C implementation of a low-latency pinger with
optional DPDK I/O and a built-in traceroute. This app assumes you expose a shared
library (e.g., libblitzping.so / blitzping.dll / libblitzping.dylib) with the
function signatures sketched below. If your real signatures differ, adapt the
`BlitzPingAPI` class accordingly.

Key features
* Start/Stop active ping sessions (host, packet size, rate/pps)
* Toggle DPDK, pick NIC port, and pass a core mask
* Live statistics panel: sent/recv, loss, min/avg/p95/max, jitter, TX/RX PPS, NIC drops, CPU util
* Traceroute tab that lists hops as they are discovered
* Non-blocking UI via QThreads; safe signal-based updates
* Works with the real C library via ctypes; includes a Mock fallback for development

Build/Link notes for your C code
* Build your C core as a shared library exposing the functions used below.
  Example (Linux):
    gcc -O3 -fPIC -shared -o libblitzping.so your_blitzping.c \
        -ldpdk -lpthread -lm
* Place the resulting shared library next to this script, or adjust the search
  paths in `BlitzPingAPI._load_library()`.

Python deps
* PyQt6  (pip install PyQt6)

(Optionally) You can swap the mock with the real library any time; the GUI detects
and uses the real one automatically if found.
"""
from __future__ import annotations

import ctypes as C
import os
import sys
import threading
import time
from dataclasses import dataclass
from typing import Optional, List

from PyQt6.QtCore import Qt, QThread, pyqtSignal, QObject
from PyQt6.QtGui import QIcon
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QComboBox, QFormLayout, QGridLayout, QGroupBox,
    QHBoxLayout, QLabel, QLineEdit, QMainWindow, QPushButton, QSpinBox,
    QTabWidget, QTextEdit, QVBoxLayout, QWidget, QTableWidget, QTableWidgetItem
)


from PyQt6.QtCore import QObject, pyqtSignal

class LogBridge(QObject):
    log = pyqtSignal(str)


# ---------------------------
# ctypes structures & helpers
# ---------------------------

class CBlitzStats(C.Structure):
    _fields_ = [
        ("sent", C.c_uint64),
        ("received", C.c_uint64),
        ("loss", C.c_double),           # percent [0..100]
        ("min_rtt_ms", C.c_double),
        ("avg_rtt_ms", C.c_double),
        ("p95_rtt_ms", C.c_double),
        ("max_rtt_ms", C.c_double),
        ("jitter_ms", C.c_double),
        ("tx_pps", C.c_double),
        ("rx_pps", C.c_double),
        ("nic_drops", C.c_uint64),
        ("cpu_util", C.c_double),       # percent [0..100]
    ]

class CTracerouteHop(C.Structure):
    _fields_ = [
        ("ttl", C.c_int),
        ("ip", C.c_char * 64),
        ("rtt_ms", C.c_double),
        ("reached", C.c_int),  # 0/1
    ]

@dataclass
class Hop:
    ttl: int
    ip: str
    rtt_ms: float
    reached: bool

# -----------------
# API abstraction
# -----------------

class BlitzPingAPI:
    """ctypes bridge to your C library. Adapts to a mock if not found."""

    def __init__(self):
        self._lib = None
        self._mock = None
        self._load_library()

    # ---- Public API expected by the GUI ----
    def init(self, dpdk_enabled: bool, core_mask: int, port_id: int) -> int:
        if self._lib is not None:
            return self._lib.blitz_init(int(dpdk_enabled), C.c_uint64(core_mask), C.c_int(port_id))
        return self._mock.init(dpdk_enabled, core_mask, port_id)

    def start_ping(self, target: str, pkt_size: int, rate_pps: int) -> int:
        if self._lib is not None:
            return self._lib.blitz_start_ping(target.encode('utf-8'), pkt_size, rate_pps)
        return self._mock.start_ping(target, pkt_size, rate_pps)

    def stop_ping(self) -> None:
        if self._lib is not None:
            self._lib.blitz_stop_ping()
            return
        self._mock.stop_ping()

    def get_stats(self) -> CBlitzStats:
        if self._lib is not None:
            stats = CBlitzStats()
            rc = self._lib.blitz_get_stats(C.byref(stats))
            if rc != 0:
                # If your C returns errors, you can raise or handle here
                pass
            return stats
        return self._mock.get_stats()

    def traceroute_begin(self, target: str, max_hops: int, timeout_ms: int) -> int:
        if self._lib is not None:
            return self._lib.blitz_traceroute_begin(target.encode('utf-8'), max_hops, timeout_ms)
        return self._mock.traceroute_begin(target, max_hops, timeout_ms)

    def traceroute_next(self) -> Optional[Hop]:
        if self._lib is not None:
            hop = CTracerouteHop()
            rc = self._lib.blitz_traceroute_next(C.byref(hop))
            if rc == 1:
                return Hop(hop.ttl, hop.ip.decode('utf-8'), hop.rtt_ms, bool(hop.reached))
            return None
        return self._mock.traceroute_next()

    def traceroute_end(self) -> None:
        if self._lib is not None:
            self._lib.blitz_traceroute_end()
            return
        self._mock.traceroute_end()

    # ---- Loader & mock ----
    def _load_library(self):
        names = [
            os.path.join(os.path.dirname(__file__), 'libblitzping.so'),
            'libblitzping.so',
            'blitzping.dll',
            'libblitzping.dylib',
        ]
        for name in names:
            try:
                lib = C.CDLL(name)
                # declare signatures
                lib.blitz_init.argtypes = [C.c_int, C.c_uint64, C.c_int]
                lib.blitz_init.restype = C.c_int

                lib.blitz_start_ping.argtypes = [C.c_char_p, C.c_int, C.c_int]
                lib.blitz_start_ping.restype = C.c_int

                lib.blitz_stop_ping.argtypes = []
                lib.blitz_stop_ping.restype = None

                lib.blitz_get_stats.argtypes = [C.POINTER(CBlitzStats)]
                lib.blitz_get_stats.restype = C.c_int

                # Traceroute API: begin/next/end
                lib.blitz_traceroute_begin.argtypes = [C.c_char_p, C.c_int, C.c_int]
                lib.blitz_traceroute_begin.restype = C.c_int

                lib.blitz_traceroute_next.argtypes = [C.POINTER(CTracerouteHop)]
                lib.blitz_traceroute_next.restype = C.c_int  # 1=have hop, 0=done

                lib.blitz_traceroute_end.argtypes = []
                lib.blitz_traceroute_end.restype = None

                self._lib = lib
                print(f"Loaded BlitzPing native library: {name}")
                return
            except OSError:
                continue
        # Fallback to mock
        # self._mock = _MockBlitzPing()
        self._mock = _SystemPingTraceroute()
        print("Using MOCK BlitzPing backend.")


# -----------------
# Mock backend (dev)
# -----------------

class _MockBlitzPing:
    def __init__(self):
        self._lock = threading.Lock()
        self._running = False
        self._start_ts = 0.0
        self._sent = 0
        self._recv = 0
        self._rate = 0
        self._pkt = 64
        self._traceroute_iter = None

    def init(self, dpdk_enabled: bool, core_mask: int, port_id: int) -> int:
        # pretend success
        return 0

    def start_ping(self, target: str, pkt_size: int, rate_pps: int) -> int:
        with self._lock:
            self._running = True
            self._start_ts = time.time()
            self._rate = max(1, rate_pps)
            self._pkt = pkt_size
            self._sent = 0
            self._recv = 0
        return 0

    def stop_ping(self) -> None:
        with self._lock:
            self._running = False

    def get_stats(self) -> CBlitzStats:
        with self._lock:
            now = time.time()
            if self._running:
                elapsed = max(1e-9, now - self._start_ts)
                # simple toy dynamics
                self._sent = int(elapsed * self._rate)
                self._recv = int(self._sent * 0.98)
                loss = 100.0 * (1 - (self._recv / max(1, self._sent))) if self._sent > 0 else 0.0
                base = 0.5
                jitter = 0.05 + 0.02 * (self._pkt / 1500)
                avg = base + 0.4 * (self._pkt / 1500)
                stats = CBlitzStats(
                    sent=self._sent,
                    received=self._recv,
                    loss=loss,
                    min_rtt_ms=avg - 0.2,
                    avg_rtt_ms=avg,
                    p95_rtt_ms=avg + 0.3,
                    max_rtt_ms=avg + 0.8,
                    jitter_ms=jitter,
                    tx_pps=float(self._rate),
                    rx_pps=float(self._rate) * 0.98,
                    nic_drops=C.c_uint64(int(self._sent * 0.01)),
                    cpu_util=7.5,
                )
                return stats
            else:
                return CBlitzStats()

    def traceroute_begin(self, target: str, max_hops: int, timeout_ms: int) -> int:
        hops = []
        for ttl in range(1, max_hops + 1):
            ip = f"10.0.{ttl}.1"
            rtt = 0.3 + 0.15 * ttl
            reached = (ttl == min(max_hops, 7))
            hops.append(Hop(ttl, ip, rtt, reached))
            if reached:
                break
        self._traceroute_iter = iter(hops)
        return 0

    def traceroute_next(self) -> Optional[Hop]:
        if self._traceroute_iter is None:
            return None
        try:
            time.sleep(0.2)
            return next(self._traceroute_iter)
        except StopIteration:
            return None

    def traceroute_end(self) -> None:
        self._traceroute_iter = None


import subprocess
import platform
import re
import threading
import time
from dataclasses import dataclass
from typing import Optional
import subprocess, platform, re, threading, time
from dataclasses import dataclass

class _SystemPingTraceroute:
    """Stable backend that uses system ping/traceroute safely with live GUI updates."""

    def __init__(self, log_callback=None):
        self._running = False
        self._thread = None
        self._target = None
        self._stats = {
            "sent": 0, "received": 0, "loss": 0.0,
            "min": 0.0, "avg": 0.0, "max": 0.0, "jitter": 0.0
        }
        self._lock = threading.Lock()
        self._system = platform.system().lower()
        self._log_callback = log_callback or (lambda msg: None)

    def init(self, dpdk_enabled, core_mask, port_id):
        return 0

    def start_ping(self, target, pkt_size, rate_pps):
        if self._running:
            return -1
        self._running = True
        self._target = target
        self._thread = threading.Thread(target=self._ping_loop, daemon=True)
        self._thread.start()
        return 0

    def stop_ping(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)
        self._thread = None

    def get_stats(self):
        class CBlitzStats: pass
        s = CBlitzStats()
        with self._lock:
            s.sent = int(self._stats["sent"])
            s.received = int(self._stats["received"])
            s.loss = float(self._stats["loss"])
            s.min_rtt_ms = float(self._stats["min"])
            s.avg_rtt_ms = float(self._stats["avg"])
            s.p95_rtt_ms = s.avg_rtt_ms
            s.max_rtt_ms = float(self._stats["max"])
            s.jitter_ms = float(self._stats["jitter"])
            s.tx_pps = 0.0
            s.rx_pps = 0.0
            s.nic_drops = 0
            s.cpu_util = 0.0
        return s

    # ---- new safe ping loop ----
    def _ping_loop(self):
        self._log_callback(f"[ping] Started to {self._target}")
        while self._running:
            try:
                cmd = (["ping", "-n", "1", self._target]
                       if "windows" in self._system else
                       ["ping", "-c", "1", self._target])
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
                output = proc.stdout.strip()
                if output:
                    self._log_callback(output)
                    self._parse_ping_output(output)
            except Exception as e:
                self._log_callback(f"[ping] error: {e}")
            time.sleep(1.0)
        self._log_callback("[ping] stopped.")

    def _parse_ping_output(self, text):
        # Match RTT
        match = re.search(r"time[=<]?\s*(\d+(?:\.\d+)?)", text)
        with self._lock:
            self._stats["sent"] += 1
            if match:
                rtt = float(match.group(1))
                self._stats["received"] += 1
                count = self._stats["received"]
                prev_avg = self._stats["avg"]
                self._stats["avg"] = (prev_avg * (count - 1) + rtt) / count
                self._stats["max"] = max(self._stats["max"], rtt)
                self._stats["min"] = rtt if self._stats["min"] == 0 else min(self._stats["min"], rtt)
            else:
                loss = (self._stats["sent"] - self._stats["received"]) / max(1, self._stats["sent"]) * 100
                self._stats["loss"] = loss

    # ---- traceroute ----
       # ---- traceroute ----
    def traceroute_begin(self, target, max_hops, timeout_ms):
        self._target = target
        self._max_hops = max_hops
        self._timeout = timeout_ms
        self._system = platform.system().lower()
        self._log_callback(f"[traceroute] Running to {target} ...")
        self._tr_iter = self._run_traceroute(target)
        return 0

    def traceroute_next(self):
        """Return the next Hop object if available, else None."""
        try:
            return next(self._tr_iter)
        except StopIteration:
            return None

    def traceroute_end(self):
        self._tr_iter = None

    def _run_traceroute(self, target):
        """Runs system traceroute line-by-line and yields Hop objects live."""
        if "windows" in self._system:
            cmd = ["tracert", "-d", "-h", str(self._max_hops), target]
        else:
            cmd = ["traceroute", "-n", "-m", str(self._max_hops), target]

        self._log_callback(f"[traceroute] Executing: {' '.join(cmd)}")

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="ignore"
        )


        

    
        for raw in iter(proc.stdout.readline, ""):
            if not raw:
                break
            line = raw.strip()
            if not line:
                continue
            self._log_callback(line)

            # --- WINDOWS FORMAT EXAMPLES ---
            # 1    <1 ms   <1 ms   <1 ms  192.168.1.1
            # 2    5 ms     6 ms     5 ms  10.117.81.253
            # 3     *        *        *     Request timed out.
            m_win = re.match(r"^\s*(\d+)\s+(.*?)(\d+\.\d+\.\d+\.\d+)", line)
            if m_win:
                ttl = int(m_win.group(1))
                ip = m_win.group(3)
                rtts = re.findall(r"(\d+)\s*ms", line)
                rtt = float(rtts[0]) if rtts else 0.0
                reached = (ip == self._target)
                yield Hop(ttl, ip, rtt, reached)
                if reached:
                    break
                continue

            # --- LINUX FORMAT EXAMPLE ---
            # 1  192.168.1.1  1.045 ms  0.981 ms  0.900 ms
            m_lin = re.match(r"^\s*(\d+)\s+([\d\.]+)\s+(\d+(?:\.\d+)?)\s*ms", line)
            if m_lin:
                ttl = int(m_lin.group(1))
                ip = m_lin.group(2)
                rtt = float(m_lin.group(3))
                reached = (ip == self._target)
                yield Hop(ttl, ip, rtt, reached)
                if reached:
                    break

        proc.wait()
        self._log_callback("[traceroute] finished.")


# -----------------
# Worker threads
# -----------------

class StatsWorker(QThread):
    stats_signal = pyqtSignal(object)  # CBlitzStats
    log_signal = pyqtSignal(str)

    def __init__(self, api: BlitzPingAPI, interval_ms: int = 500):
        super().__init__()
        self.api = api
        self.interval = max(50, interval_ms) / 1000.0
        self._running = False

    def run(self):
        self._running = True
        self.log_signal.emit("Stats worker started.")
        while self._running:
            stats = self.api.get_stats()
            self.stats_signal.emit(stats)
            time.sleep(self.interval)
        self.log_signal.emit("Stats worker stopped.")

    def stop(self):
        self._running = False


class TracerouteWorker(QThread):
    hop_signal = pyqtSignal(object)   # Hop
    done_signal = pyqtSignal()
    log_signal = pyqtSignal(str)

    def __init__(self, api: BlitzPingAPI, target: str, max_hops: int, timeout_ms: int):
        super().__init__()
        self.api = api
        self.target = target
        self.max_hops = max_hops
        self.timeout_ms = timeout_ms

    def run(self):
        self.log_signal.emit(f"Traceroute started to {self.target} ...")
        rc = self.api.traceroute_begin(self.target, self.max_hops, self.timeout_ms)
        if rc != 0:
            self.log_signal.emit(f"Traceroute init failed: rc={rc}")
            self.done_signal.emit()
            return
        while True:
            hop = self.api.traceroute_next()
            if hop is None:
                break
            self.hop_signal.emit(hop)
            if hop.reached:
                break
        self.api.traceroute_end()
        self.log_signal.emit("Traceroute finished.")
        self.done_signal.emit()


# --------------
# Main window UI
# --------------

class BlitzPingWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("BlitzPing – Low Latency Ping (DPDK/Traceroute)")
        self.resize(980, 700)

        self.api = BlitzPingAPI()
        self.stats_worker: Optional[StatsWorker] = None
        self.tr_worker: Optional[TracerouteWorker] = None

        self.log_bridge = LogBridge()
        self.log_bridge.log.connect(self._log)  # deliver logs to GUI thread

        # If we’re using the system mock, route its logs through the bridge
        if isinstance(self.api._mock, _SystemPingTraceroute):
            self.api._mock._log_callback = self.log_bridge.log.emit


        # # Hook the system mock logger into GUI
        # if isinstance(self.api._mock, _SystemPingTraceroute):
        #     self.api._mock._log_callback = self._log


        self._init_ui()

    # ---- UI builders ----
    def _init_ui(self):
        central = QWidget(self)
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)

        self.tabs = QTabWidget()
        layout.addWidget(self.tabs, 1)

        # Tabs
        self._build_ping_tab()
        self._build_traceroute_tab()
        self._build_dpdk_tab()

        # Log output
        self.log = QTextEdit()
        self.log.setReadOnly(True)
        self.log.setPlaceholderText("Logs...")
        layout.addWidget(self.log, 1)

    def _build_ping_tab(self):
        tab = QWidget()
        self.tabs.addTab(tab, "Live Ping")
        v = QVBoxLayout(tab)

        form = QFormLayout()
        self.target_edit = QLineEdit("8.8.8.8")
        self.pkt_size = QSpinBox(); self.pkt_size.setRange(28, 9000); self.pkt_size.setValue(64)
        self.rate_pps = QSpinBox(); self.rate_pps.setRange(1, 1_000_000); self.rate_pps.setValue(1000)
        form.addRow("Target host/IP:", self.target_edit)
        form.addRow("Packet size (bytes):", self.pkt_size)
        form.addRow("Rate (pps):", self.rate_pps)
        v.addLayout(form)

        # Controls
        h = QHBoxLayout()
        self.btn_start = QPushButton("Start Ping")
        self.btn_stop = QPushButton("Stop")
        self.btn_stop.setEnabled(False)
        self.btn_start.clicked.connect(self._start_ping)
        self.btn_stop.clicked.connect(self._stop_ping)
        h.addWidget(self.btn_start)
        h.addWidget(self.btn_stop)
        h.addStretch(1)
        v.addLayout(h)

        # Stats grid
        self.stats_labels = {}
        grid = QGridLayout()
        labels = [
            ("sent", "Sent"), ("received", "Received"), ("loss", "Loss %"),
            ("min_rtt_ms", "Min RTT ms"), ("avg_rtt_ms", "Avg RTT ms"), ("p95_rtt_ms", "p95 RTT ms"), ("max_rtt_ms", "Max RTT ms"), ("jitter_ms", "Jitter ms"),
            ("tx_pps", "TX pps"), ("rx_pps", "RX pps"), ("nic_drops", "NIC drops"), ("cpu_util", "CPU %"),
        ]
        for i, (key, title) in enumerate(labels):
            r, c = divmod(i, 4)
            name = QLabel(f"{title}:")
            val = QLabel("–")
            val.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            grid.addWidget(name, r, c*2)
            grid.addWidget(val, r, c*2+1)
            self.stats_labels[key] = val
        v.addLayout(grid)

    def _build_traceroute_tab(self):
        tab = QWidget()
        self.tabs.addTab(tab, "Traceroute")
        v = QVBoxLayout(tab)

        form = QFormLayout()
        self.tr_target = QLineEdit("8.8.8.8")
        self.tr_max_hops = QSpinBox(); self.tr_max_hops.setRange(1, 64); self.tr_max_hops.setValue(30)
        self.tr_timeout = QSpinBox(); self.tr_timeout.setRange(100, 10000); self.tr_timeout.setSuffix(" ms"); self.tr_timeout.setValue(1000)
        form.addRow("Target host/IP:", self.tr_target)
        form.addRow("Max hops:", self.tr_max_hops)
        form.addRow("Per-hop timeout:", self.tr_timeout)
        v.addLayout(form)

        h = QHBoxLayout()
        self.btn_tr_start = QPushButton("Run Traceroute")
        self.btn_tr_start.clicked.connect(self._start_traceroute)
        h.addWidget(self.btn_tr_start)
        h.addStretch(1)
        v.addLayout(h)

        self.tr_table = QTableWidget(0, 4)
        self.tr_table.setHorizontalHeaderLabels(["TTL", "IP", "RTT (ms)", "Reached?"])
        v.addWidget(self.tr_table, 1)

    def _build_dpdk_tab(self):
        tab = QWidget()
        self.tabs.addTab(tab, "DPDK / Settings")
        v = QVBoxLayout(tab)

        group = QGroupBox("Runtime Settings")
        g = QFormLayout(group)
        self.chk_dpdk = QCheckBox("Enable DPDK fast I/O")
        self.core_mask = QLineEdit("0x3")
        self.port_id = QSpinBox(); self.port_id.setRange(0, 255); self.port_id.setValue(0)
        g.addRow(self.chk_dpdk)
        g.addRow("Core mask (hex or int):", self.core_mask)
        g.addRow("DPDK port id:", self.port_id)
        v.addWidget(group)

        self.btn_init = QPushButton("Initialize Engine")
        self.btn_init.clicked.connect(self._init_engine)
        v.addWidget(self.btn_init)
        v.addStretch(1)

    # ---- Actions ----
    def _parse_core_mask(self, text: str) -> int:
        t = text.strip().lower()
        try:
            if t.startswith('0x'):
                return int(t, 16)
            return int(t)
        except ValueError:
            return 0x3

    def _init_engine(self):
        dpdk = self.chk_dpdk.isChecked()
        core = self._parse_core_mask(self.core_mask.text())
        port = int(self.port_id.value())
        rc = self.api.init(dpdk, core, port)
        if rc == 0:
            self._log(f"Engine initialized. DPDK={'on' if dpdk else 'off'}, core_mask={hex(core)}, port={port}")
        else:
            self._log(f"Engine init failed: rc={rc}")

    def _start_ping(self):
        target = self.target_edit.text().strip()
        pkt = int(self.pkt_size.value())
        rate = int(self.rate_pps.value())
        rc = self.api.start_ping(target, pkt, rate)
        if rc != 0:
            self._log(f"Start ping failed: rc={rc}")
            return
        self._log(f"Ping started -> {target} | {pkt} bytes @ {rate} pps")
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        # start stats worker
        self.stats_worker = StatsWorker(self.api)
        self.stats_worker.stats_signal.connect(self._update_stats)
        self.stats_worker.log_signal.connect(self._log)
        self.stats_worker.start()

    def _stop_ping(self):
        self.api.stop_ping()
        if self.stats_worker is not None:
            self.stats_worker.stop()
            self.stats_worker.wait(1000)
            self.stats_worker = None
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self._log("Ping stopped.")

    def _start_traceroute(self):
        target = self.tr_target.text().strip()
        max_hops = int(self.tr_max_hops.value())
        timeout_ms = int(self.tr_timeout.value())
        # reset table
        self.tr_table.setRowCount(0)
        self.tr_worker = TracerouteWorker(self.api, target, max_hops, timeout_ms)
        self.tr_worker.hop_signal.connect(self._add_hop)
        self.tr_worker.done_signal.connect(lambda: self._log("Traceroute done."))
        self.tr_worker.log_signal.connect(self._log)
        self.tr_worker.start()

    # ---- UI updates ----
    def _update_stats(self, s: CBlitzStats):
        def fmt(x):
            if isinstance(x, float):
                return f"{x:.3f}"
            return str(x)
        self.stats_labels["sent"].setText(fmt(s.sent))
        self.stats_labels["received"].setText(fmt(s.received))
        self.stats_labels["loss"].setText(fmt(s.loss))
        self.stats_labels["min_rtt_ms"].setText(fmt(s.min_rtt_ms))
        self.stats_labels["avg_rtt_ms"].setText(fmt(s.avg_rtt_ms))
        self.stats_labels["p95_rtt_ms"].setText(fmt(s.p95_rtt_ms))
        self.stats_labels["max_rtt_ms"].setText(fmt(s.max_rtt_ms))
        self.stats_labels["jitter_ms"].setText(fmt(s.jitter_ms))
        self.stats_labels["tx_pps"].setText(fmt(s.tx_pps))
        self.stats_labels["rx_pps"].setText(fmt(s.rx_pps))
        self.stats_labels["nic_drops"].setText(fmt(s.nic_drops))
        self.stats_labels["cpu_util"].setText(fmt(s.cpu_util))

    def _add_hop(self, hop: Hop):
        r = self.tr_table.rowCount()
        self.tr_table.insertRow(r)
        self.tr_table.setItem(r, 0, QTableWidgetItem(str(hop.ttl)))
        self.tr_table.setItem(r, 1, QTableWidgetItem(hop.ip))
        self.tr_table.setItem(r, 2, QTableWidgetItem(f"{hop.rtt_ms:.3f}"))
        self.tr_table.setItem(r, 3, QTableWidgetItem("yes" if hop.reached else ""))

    def _log(self, msg: str):
        self.log.append(msg)

    # ---- lifecycle ----
    def closeEvent(self, event):
        try:
            if self.stats_worker is not None:
                self.stats_worker.stop()
                self.stats_worker.wait(500)
            if self.tr_worker is not None:
                self.tr_worker.wait(500)
            self.api.stop_ping()
        finally:
            super().closeEvent(event)


# ---------
# Entrypoint
# ---------

def main():
    app = QApplication(sys.argv)
    win = BlitzPingWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
