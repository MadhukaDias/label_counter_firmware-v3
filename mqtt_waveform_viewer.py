#!/usr/bin/env python3
"""
MQTT waveform + calibration viewer for the Label Counter firmware.

Subscribes to the devices' telemetry topic, decodes the JSON packets that
src/mqtt_mgr.cpp publishes, and shows:

  * Waveform tab    - message list + a single magnitude-vs-time chart
  * Per-device tab  - one stacked chart per device, latest waveform each
  * Calibration tab - a table with every device's current calibration values

Packet shapes on  labelcounter/<deviceId>/data :

  waveform (button_inc / button_dec / count_reset / interval_update):
    { device, timestamp, event, count, data:[ <magnitude> ... ] }   # 50 Hz

  calibration (calibration_state on boot/reconnect, calibration_done on finish):
    { device, timestamp, event, status:"none"|"success"|"canceled",
      calibration: null | {
        startThreshold, stopThreshold, toleratingThreshold,
        minDurationMs, silenceMs, noiseCleanMax, noiseCleanMin,
        spikeThreshold, lockPeak } }

Install deps:  pip install -r tools/requirements.txt
Run:           python tools/mqtt_waveform_viewer.py [--connect]
"""

import argparse
import csv
import json
import queue
import random
import sys
import time
from collections import deque
from datetime import datetime

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
except Exception:  # pragma: no cover
    sys.exit("This tool needs Tkinter (bundled with the standard python.org CPython).")

try:
    import numpy as np
except Exception:
    sys.exit("Missing dependency: numpy (installed automatically with matplotlib).")

try:
    import matplotlib
    matplotlib.use("TkAgg")
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
    from matplotlib.figure import Figure
except Exception:
    sys.exit("Missing dependency: matplotlib  ->  pip install matplotlib")

try:
    import paho.mqtt.client as mqtt
except Exception:
    sys.exit("Missing dependency: paho-mqtt  ->  pip install paho-mqtt")


DEFAULT_HOST = "broker.emqx.io"
DEFAULT_PORT = 1883
DEFAULT_TOPIC = "labelcounter/+/data"
DEFAULT_RATE_HZ = 50.0            # IMU_SAMPLE_HZ in include/config.h
MAX_MESSAGES = 500               # ring buffer depth
MAX_DEVICE_PANELS = 6            # per-device tab

CALIB_EVENTS = ("calibration_state", "calibration_done")

# (json key, short label, column width) - drives the calibration table
CALIB_FIELDS = [
    ("startThreshold",      "start",    64),
    ("stopThreshold",       "stop",     64),
    ("toleratingThreshold", "tol",      64),
    ("minDurationMs",       "minDur",   64),
    ("silenceMs",           "silence",  66),
    ("noiseCleanMax",       "noiseMax", 74),
    ("noiseCleanMin",       "noiseMin", 74),
    ("spikeThreshold",      "spikeThr", 74),
    ("lockPeak",            "lockPeak", 78),
]

# palette roughly matching the device / web UI
C_BG = "#0d0f12"
C_SURF = "#161a1f"
C_GRID = "#2a2f38"
C_ACC = "#00e5a0"
C_ACC2 = "#ff6b35"
C_TXT = "#e0e6ef"
C_DIM = "#7f8a9a"


# --------------------------------------------------------------------------- #
#  MQTT client wrapper - pushes decoded packets onto a thread-safe queue
# --------------------------------------------------------------------------- #
class MqttWorker:
    def __init__(self, out_queue: "queue.Queue"):
        self.q = out_queue
        self.client = None
        self._want = False
        self.topic = DEFAULT_TOPIC

    def _make_client(self):
        cid = "lc-wave-viewer-%06x" % random.randint(0, 0xFFFFFF)
        try:  # paho-mqtt >= 2.0
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1,
                            client_id=cid, clean_session=True)
        except (AttributeError, TypeError):  # paho-mqtt 1.x
            c = mqtt.Client(client_id=cid, clean_session=True)
        c.on_connect = self._on_connect
        c.on_disconnect = self._on_disconnect
        c.on_message = self._on_message
        c.reconnect_delay_set(min_delay=1, max_delay=15)
        return c

    def connect(self, host, port, topic):
        self.disconnect()
        self._want = True
        self.topic = topic
        self.client = self._make_client()
        self.q.put(("status", f"connecting to {host}:{port} ..."))
        try:
            self.client.connect_async(host, int(port), keepalive=30)
            self.client.loop_start()
        except Exception as e:
            self.q.put(("status", f"connect failed: {e}"))

    def disconnect(self):
        self._want = False
        if self.client is not None:
            try:
                self.client.loop_stop()
                self.client.disconnect()
            except Exception:
                pass
            self.client = None

    # paho callbacks (background thread) -----------------------------------
    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(self.topic, qos=0)
            self.q.put(("status", f"connected - subscribed to {self.topic}"))
        else:
            self.q.put(("status", f"connect refused (rc={rc})"))

    def _on_disconnect(self, client, userdata, rc):
        self.q.put(("status", f"disconnected (rc={rc})" +
                    (" - retrying..." if self._want else "")))

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception as e:
            self.q.put(("status", f"bad payload on {msg.topic}: {e}"))
            return
        self.q.put(("packet", parse_packet(msg.topic, payload)))


def parse_packet(topic, payload):
    """Normalise a raw payload dict into the record the UI stores."""
    device = payload.get("device")
    if not device:
        parts = topic.split("/")
        device = parts[1] if len(parts) >= 3 else topic
    data = payload.get("data")
    data = [float(x) for x in data] if isinstance(data, list) else None
    cal = payload.get("calibration")
    if not isinstance(cal, dict):
        cal = None
    return {
        "recv": time.time(),
        "topic": topic,
        "device": str(device),
        "event": str(payload.get("event", "?")),
        "count": payload.get("count"),
        "uptime_ms": payload.get("timestamp"),
        "status": payload.get("status"),
        "data": data,
        "calibration": cal,
        "raw": payload,
    }


# --------------------------------------------------------------------------- #
#  Waveform statistics
# --------------------------------------------------------------------------- #
def waveform_stats(y, rate_hz):
    a = np.asarray(y, dtype=float)
    n = len(a)
    return {
        "samples": n,
        "duration_s": (n - 1) / rate_hz if n > 1 else 0.0,
        "peak": float(a.max()) if n else 0.0,
        "min": float(a.min()) if n else 0.0,
        "mean": float(a.mean()) if n else 0.0,
        "median": float(np.median(a)) if n else 0.0,
        "rms": float(np.sqrt(np.mean(a * a))) if n else 0.0,
        "peak_idx": int(a.argmax()) if n else 0,
    }


def style_axes(ax, title=None):
    ax.set_facecolor(C_SURF)
    for s in ax.spines.values():
        s.set_color(C_GRID)
    ax.tick_params(colors=C_DIM, labelsize=8)
    ax.grid(True, color=C_GRID, linewidth=0.6, alpha=0.6)
    ax.set_xlabel("Time (s)", color=C_DIM, fontsize=9)
    ax.set_ylabel("Magnitude", color=C_DIM, fontsize=9)
    if title:
        ax.set_title(title, color=C_TXT, fontsize=10)


# --------------------------------------------------------------------------- #
#  Main application window
# --------------------------------------------------------------------------- #
class App(tk.Tk):
    def __init__(self, host, port, topic):
        super().__init__()
        self.title("Label Counter - MQTT waveform & calibration viewer")
        self.geometry("1300x820")
        self.configure(bg=C_BG)

        self.q: "queue.Queue" = queue.Queue()
        self.worker = MqttWorker(self.q)
        self.messages: "deque" = deque(maxlen=MAX_MESSAGES)
        self.selected = None
        self.latest_wave_by_device = {}     # device -> rec (has data)
        self.calib_by_device = {}           # device -> {status, cal, recv, event}
        self._pd_job = None

        self._build_style()
        self._build_conn_bar(host, port, topic)
        self._build_body()
        self._build_statusbar()

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._drain_queue)

    # ---- styling -------------------------------------------------------- #
    def _build_style(self):
        st = ttk.Style(self)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure(".", background=C_BG, foreground=C_TXT, fieldbackground=C_SURF)
        for w in ("TFrame", "TLabel", "TCheckbutton", "TNotebook"):
            st.configure(w, background=C_BG, foreground=C_TXT)
        st.configure("TButton", background=C_SURF, foreground=C_TXT)
        st.map("TButton", background=[("active", C_GRID)])
        st.configure("TNotebook.Tab", background=C_SURF, foreground=C_DIM, padding=(12, 5))
        st.map("TNotebook.Tab", background=[("selected", C_GRID)],
               foreground=[("selected", C_TXT)])
        st.configure("Treeview", background=C_SURF, fieldbackground=C_SURF,
                     foreground=C_TXT, rowheight=22, borderwidth=0)
        st.configure("Treeview.Heading", background=C_GRID, foreground=C_TXT)
        st.map("Treeview", background=[("selected", C_ACC)], foreground=[("selected", C_BG)])

    # ---- connection bar ----------------------------------------------- #
    def _build_conn_bar(self, host, port, topic):
        bar = ttk.Frame(self, padding=(8, 6))
        bar.pack(fill="x")

        ttk.Label(bar, text="Broker").pack(side="left")
        self.e_host = self._entry(bar, host, 20)
        ttk.Label(bar, text="Port").pack(side="left")
        self.e_port = self._entry(bar, str(port), 6)
        ttk.Label(bar, text="Topic").pack(side="left")
        self.e_topic = self._entry(bar, topic, 24)

        self.b_conn = ttk.Button(bar, text="Connect", command=self._toggle_conn)
        self.b_conn.pack(side="left", padx=6)
        self.connected = False

        ttk.Label(bar, text="Rate (Hz)").pack(side="left", padx=(14, 0))
        self.e_rate = self._entry(bar, str(DEFAULT_RATE_HZ), 6)
        self.e_rate.bind("<Return>", lambda _e: self._replot())

        ttk.Button(bar, text="Load JSON...", command=self._load_file).pack(side="right", padx=4)

    def _entry(self, parent, value, width):
        e = ttk.Entry(parent, width=width)
        e.insert(0, value)
        e.pack(side="left", padx=(4, 8))
        return e

    # ---- body: notebook --------------------------------------------- #
    def _build_body(self):
        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=8, pady=4)
        self._build_tab_waveform()
        self._build_tab_perdevice()
        self._build_tab_calibration()
        self.nb.bind("<<NotebookTabChanged>>", self._on_tab_change)

    def _build_tab_waveform(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="Waveform")
        pane = ttk.PanedWindow(tab, orient="horizontal")
        pane.pack(fill="both", expand=True)

        left = ttk.Frame(pane)
        pane.add(left, weight=1)
        cols = ("time", "device", "event", "count", "samples", "peak")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", height=20)
        widths = dict(time=86, device=98, event=120, count=54, samples=64, peak=70)
        for c in cols:
            self.tree.heading(c, text=c.capitalize())
            self.tree.column(c, width=widths[c],
                             anchor="w" if c in ("device", "event") else "center")
        self.tree.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        sb.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        btns = ttk.Frame(left)
        btns.pack(fill="x", pady=4)
        self.v_follow = tk.BooleanVar(value=True)
        ttk.Checkbutton(btns, text="Follow latest", variable=self.v_follow).pack(side="left")
        self.v_hide_empty = tk.BooleanVar(value=False)
        ttk.Checkbutton(btns, text="Hide no-waveform", variable=self.v_hide_empty,
                        command=self._rebuild_tree).pack(side="left", padx=6)
        ttk.Button(btns, text="Clear", command=self._clear).pack(side="right")
        ttk.Button(btns, text="Export CSV", command=self._export_csv).pack(side="right", padx=4)

        right = ttk.Frame(pane)
        pane.add(right, weight=3)
        self.fig = Figure(figsize=(7, 4.2), dpi=100, facecolor=C_SURF)
        self.ax = self.fig.add_subplot(111)
        style_axes(self.ax)
        self.canvas = FigureCanvasTkAgg(self.fig, master=right)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)
        NavigationToolbar2Tk(self.canvas, right)

        ov = ttk.Frame(right, padding=(0, 4))
        ov.pack(fill="x")
        ttk.Label(ov, text="Manual start thr").pack(side="left")
        self.e_start = self._entry(ov, "", 8)
        ttk.Label(ov, text="stop thr").pack(side="left")
        self.e_stop = self._entry(ov, "", 8)
        ttk.Button(ov, text="Apply", command=self._replot).pack(side="left", padx=6)
        self.v_dev_thr = tk.BooleanVar(value=True)
        ttk.Checkbutton(ov, text="Overlay device calibration thresholds",
                        variable=self.v_dev_thr, command=self._replot).pack(side="left", padx=12)
        for e in (self.e_start, self.e_stop):
            e.bind("<Return>", lambda _e: self._replot())

        self.lbl_stats = tk.Label(right, text="No waveform selected.", justify="left",
                                  bg=C_SURF, fg=C_DIM, font=("Consolas", 9), anchor="w")
        self.lbl_stats.pack(fill="x", pady=(2, 0))

    def _build_tab_perdevice(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="Per-device")
        top = ttk.Frame(tab, padding=(2, 4))
        top.pack(fill="x")
        ttk.Label(top, text=f"Latest waveform per device (up to {MAX_DEVICE_PANELS}). "
                            "Time axis uses the Rate (Hz) box above.").pack(side="left")
        ttk.Button(top, text="Refresh", command=self._refresh_perdevice).pack(side="right")
        self.fig_pd = Figure(figsize=(7, 6), dpi=100, facecolor=C_SURF)
        self.canvas_pd = FigureCanvasTkAgg(self.fig_pd, master=tab)
        self.canvas_pd.get_tk_widget().pack(fill="both", expand=True)
        NavigationToolbar2Tk(self.canvas_pd, tab)
        self._refresh_perdevice()

    def _build_tab_calibration(self):
        tab = ttk.Frame(self.nb)
        self.nb.add(tab, text="Calibration")
        ttk.Label(tab, padding=(4, 6),
                  text="Devices publish their calibration on boot, on every MQTT "
                       "reconnect, and when a calibration finishes or is aborted. "
                       "'—' means the device has never been calibrated.").pack(anchor="w")

        cols = ("device", "status") + tuple(f[0] for f in CALIB_FIELDS) + ("updated",)
        self.ctree = ttk.Treeview(tab, columns=cols, show="headings", height=16)
        headings = {"device": ("device", 110), "status": ("status", 78),
                    "updated": ("updated", 90)}
        headings.update({k: (lbl, w) for k, lbl, w in CALIB_FIELDS})
        for c in cols:
            lbl, w = headings[c]
            self.ctree.heading(c, text=lbl)
            self.ctree.column(c, width=w, anchor="w" if c == "device" else "center")
        self.ctree.tag_configure("success", foreground=C_ACC)
        self.ctree.tag_configure("canceled", foreground=C_ACC2)
        self.ctree.tag_configure("none", foreground=C_DIM)
        self.ctree.pack(fill="both", expand=True, padx=4, pady=4)

    def _build_statusbar(self):
        self.status = tk.StringVar(value="idle")
        tk.Label(self, textvariable=self.status, bg=C_GRID, fg=C_TXT,
                 anchor="w", padx=8).pack(fill="x", side="bottom")

    # ---- queue pump -------------------------------------------------- #
    def _drain_queue(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "status":
                    self.status.set(payload)
                elif kind == "packet":
                    self._add_message(payload)
        except queue.Empty:
            pass
        self.after(120, self._drain_queue)

    # ---- message handling ------------------------------------------ #
    def _add_message(self, rec):
        self.messages.append(rec)
        dev = rec["device"]

        if self._passes_filter(rec):
            self.tree.insert("", 0, iid=str(id(rec)), values=self._row_values(rec))
            for k in self.tree.get_children()[MAX_MESSAGES:]:
                self.tree.delete(k)
        if self.v_follow.get() and rec["data"]:
            kids = self.tree.get_children()
            if kids:
                self.tree.selection_set(kids[0])
                self.tree.see(kids[0])

        if rec["data"]:
            self.latest_wave_by_device[dev] = rec
            self._schedule_perdevice()

        if rec["event"] in CALIB_EVENTS:
            self.calib_by_device[dev] = {
                "status": rec["status"], "cal": rec["calibration"],
                "recv": rec["recv"], "event": rec["event"],
            }
            self._refresh_calib_table()
            if self.selected and self.selected["device"] == dev:
                self._replot()

    def _passes_filter(self, rec):
        return not (self.v_hide_empty.get() and not rec["data"])

    def _row_values(self, rec):
        t = datetime.fromtimestamp(rec["recv"]).strftime("%H:%M:%S")
        peak = f"{max(rec['data']):.0f}" if rec["data"] else "-"
        n = len(rec["data"]) if rec["data"] else 0
        label = rec["event"]
        if rec["status"]:
            label += f" ({rec['status']})"
        return (t, rec["device"], label,
                "-" if rec["count"] is None else rec["count"], n, peak)

    def _rebuild_tree(self):
        self.tree.delete(*self.tree.get_children())
        for rec in reversed(self.messages):
            if self._passes_filter(rec):
                self.tree.insert("", "end", iid=str(id(rec)), values=self._row_values(rec))

    def _find_rec(self, iid):
        for rec in self.messages:
            if str(id(rec)) == iid:
                return rec
        return None

    def _on_select(self, _evt):
        sel = self.tree.selection()
        if sel:
            rec = self._find_rec(sel[0])
            if rec is not None:
                self.selected = rec
                self._replot()

    def _on_tab_change(self, _evt):
        if self.nb.tab(self.nb.select(), "text") == "Per-device":
            self._refresh_perdevice()

    # ---- rate ------------------------------------------------------ #
    def _rate(self):
        try:
            r = float(self.e_rate.get())
            return r if r > 0 else DEFAULT_RATE_HZ
        except ValueError:
            return DEFAULT_RATE_HZ

    # ---- single waveform plot ----------------------------------- #
    def _replot(self):
        rec = self.selected
        self.ax.clear()
        style_axes(self.ax)
        if rec is None:
            self.lbl_stats.config(text="No waveform selected.")
            self.canvas.draw_idle()
            return

        meta = (f"device={rec['device']}  event={rec['event']}"
                + (f"  status={rec['status']}" if rec['status'] else "")
                + f"  count={rec['count']}  uptime={rec['uptime_ms']} ms"
                + f"  recv={datetime.fromtimestamp(rec['recv']).strftime('%H:%M:%S')}")

        self._draw_device_thresholds(rec["device"])

        if not rec["data"]:
            txt = f"'{rec['event']}' carries no waveform data"
            if rec["calibration"]:
                txt += "\n\ncalibration:\n" + "\n".join(
                    f"  {k} = {rec['calibration'].get(k)}" for k, _l, _w in CALIB_FIELDS)
            elif rec["event"] in CALIB_EVENTS:
                txt += f"\n\ncalibration: null  (status: {rec['status']})"
            self.ax.text(0.5, 0.5, txt, ha="center", va="center", color=C_DIM,
                         transform=self.ax.transAxes, family="monospace", fontsize=9)
            self.lbl_stats.config(text=meta)
            self.canvas.draw_idle()
            return

        y = np.asarray(rec["data"], dtype=float)
        rate = self._rate()
        t = np.arange(len(y)) / rate
        s = waveform_stats(y, rate)

        self.ax.plot(t, y, color=C_ACC, linewidth=1.1)
        self.ax.fill_between(t, y, color=C_ACC, alpha=0.08)
        self.ax.axhline(s["mean"], color=C_DIM, linewidth=0.8, linestyle="--",
                        label=f"mean {s['mean']:.0f}")
        self.ax.plot(s["peak_idx"] / rate, s["peak"], "o", color=C_ACC2, ms=5,
                     label=f"peak {s['peak']:.0f}")

        for entry, lbl, col in ((self.e_start, "manual start", C_ACC2),
                                (self.e_stop, "manual stop", "#ffd166")):
            txt = entry.get().strip()
            if txt:
                try:
                    self.ax.axhline(float(txt), color=col, linewidth=1.0,
                                    linestyle="-.", label=lbl)
                except ValueError:
                    pass

        self.ax.set_title(f"{rec['device']}  -  {rec['event']}", color=C_TXT, fontsize=10)
        self.ax.set_xlim(0, max(t[-1], 1e-6))
        leg = self.ax.legend(loc="upper right", fontsize=8, framealpha=0.2)
        if leg:
            for txt in leg.get_texts():
                txt.set_color(C_TXT)
        self.fig.tight_layout()
        self.canvas.draw_idle()

        self.lbl_stats.config(text=(
            meta + "\n"
            + f"samples={s['samples']}  duration={s['duration_s']:.2f} s  "
            + f"peak={s['peak']:.0f} @ {s['peak_idx']/rate:.2f} s  min={s['min']:.0f}  "
            + f"mean={s['mean']:.1f}  median={s['median']:.1f}  rms={s['rms']:.1f}"))

    def _draw_device_thresholds(self, device):
        if not self.v_dev_thr.get():
            return
        cal = self.calib_by_device.get(device, {}).get("cal")
        if not cal:
            return
        for key, lbl, col in (("startThreshold", "dev start", C_ACC2),
                              ("stopThreshold", "dev stop", "#66d9ff")):
            v = cal.get(key)
            if isinstance(v, (int, float)):
                self.ax.axhline(float(v), color=col, linewidth=1.1,
                                linestyle=":", label=lbl)

    # ---- per-device plot -------------------------------------- #
    def _schedule_perdevice(self):
        if self._pd_job is not None:
            self.after_cancel(self._pd_job)
        self._pd_job = self.after(500, self._refresh_perdevice)

    def _refresh_perdevice(self):
        self._pd_job = None
        self.fig_pd.clear()
        devs = sorted(self.latest_wave_by_device)
        if not devs:
            self.fig_pd.text(0.5, 0.5, "no waveforms received yet",
                             ha="center", va="center", color=C_DIM)
            self.canvas_pd.draw_idle()
            return
        rate = self._rate()
        shown = devs[:MAX_DEVICE_PANELS]
        for i, dev in enumerate(shown):
            rec = self.latest_wave_by_device[dev]
            ax = self.fig_pd.add_subplot(len(shown), 1, i + 1)
            style_axes(ax)
            y = np.asarray(rec["data"], dtype=float)
            t = np.arange(len(y)) / rate
            ax.plot(t, y, color=C_ACC, linewidth=1.0)
            ax.fill_between(t, y, color=C_ACC, alpha=0.08)
            cal = self.calib_by_device.get(dev, {}).get("cal")
            if cal and isinstance(cal.get("startThreshold"), (int, float)):
                ax.axhline(float(cal["startThreshold"]), color=C_ACC2,
                           linewidth=0.9, linestyle=":")
            rt = datetime.fromtimestamp(rec["recv"]).strftime("%H:%M:%S")
            ax.set_title(f"{dev}   {rec['event']}   count={rec['count']}   "
                         f"peak={y.max():.0f}   ({rt})", color=C_TXT, fontsize=9)
            if i < len(shown) - 1:
                ax.set_xlabel("")
        if len(devs) > MAX_DEVICE_PANELS:
            self.fig_pd.suptitle(f"showing {MAX_DEVICE_PANELS} of {len(devs)} devices",
                                 color=C_DIM, fontsize=8)
        self.fig_pd.tight_layout()
        self.canvas_pd.draw_idle()

    # ---- calibration table ---------------------------------- #
    def _refresh_calib_table(self):
        self.ctree.delete(*self.ctree.get_children())
        for dev in sorted(self.calib_by_device):
            info = self.calib_by_device[dev]
            cal = info["cal"] or {}
            status = info["status"] or ("success" if cal else "none")
            vals = [dev, status]
            for key, _lbl, _w in CALIB_FIELDS:
                v = cal.get(key)
                vals.append("—" if v is None else v)
            vals.append(datetime.fromtimestamp(info["recv"]).strftime("%H:%M:%S"))
            tag = status if status in ("success", "canceled", "none") else "none"
            self.ctree.insert("", "end", iid=dev, values=vals, tags=(tag,))

    # ---- actions ------------------------------------------------- #
    def _toggle_conn(self):
        if self.connected:
            self.worker.disconnect()
            self.connected = False
            self.b_conn.config(text="Connect")
            self.status.set("disconnected")
        else:
            self.worker.connect(self.e_host.get().strip(), self.e_port.get().strip(),
                                self.e_topic.get().strip())
            self.connected = True
            self.b_conn.config(text="Disconnect")

    def _clear(self):
        self.messages.clear()
        self.selected = None
        self.latest_wave_by_device.clear()
        self.calib_by_device.clear()
        self.tree.delete(*self.tree.get_children())
        self._refresh_calib_table()
        self._refresh_perdevice()
        self._replot()

    def _export_csv(self):
        rec = self.selected
        if not rec or not rec["data"]:
            messagebox.showinfo("Export CSV", "Select a message that has waveform data first.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".csv", initialfile=f"{rec['device']}_{rec['event']}.csv",
            filetypes=[("CSV", "*.csv")])
        if not path:
            return
        rate = self._rate()
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time_s", "magnitude"])
            for i, v in enumerate(rec["data"]):
                w.writerow([f"{i / rate:.4f}", v])
        self.status.set(f"exported {len(rec['data'])} samples -> {path}")

    def _load_file(self):
        path = filedialog.askopenfilename(
            filetypes=[("JSON / JSON lines", "*.json *.jsonl *.txt"), ("All", "*.*")])
        if not path:
            return
        added = 0
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read().strip()
            try:
                obj = json.loads(text)
                blobs = obj if isinstance(obj, list) else [obj]
            except json.JSONDecodeError:
                blobs = [json.loads(ln) for ln in text.splitlines() if ln.strip()]
            for b in blobs:
                topic = b.get("_topic", f"labelcounter/{b.get('device', 'file')}/data")
                self._add_message(parse_packet(topic, b))
                added += 1
        except Exception as e:
            messagebox.showerror("Load JSON", str(e))
            return
        self.status.set(f"loaded {added} packet(s) from {path}")

    def _on_close(self):
        self.worker.disconnect()
        self.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--topic", default=DEFAULT_TOPIC)
    ap.add_argument("--connect", action="store_true", help="connect on startup")
    args = ap.parse_args()

    app = App(args.host, args.port, args.topic)
    if args.connect:
        app.after(300, app._toggle_conn)
    app.mainloop()


if __name__ == "__main__":
    main()
