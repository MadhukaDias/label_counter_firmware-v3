#!/usr/bin/env python3
"""
MQTT waveform viewer for the Label Counter firmware.

Subscribes to the device's telemetry topic, decodes the JSON waveform packets
that mqtt_mgr.cpp publishes, and plots the vibration magnitude as a
magnitude-vs-time graph.

Payload shape (see src/mqtt_mgr.cpp / src/main.cpp):
    {
      "device":    "LC-CD8510",
      "timestamp": 123456,            # millis() uptime, not wall clock
      "event":     "interval_update", # or button_inc / button_dec / count_reset
      "count":     42,
      "data":      [ <magnitude>, ... ]   # 50 Hz samples (IMU_SAMPLE_HZ)
    }
Event-only packets (calibration_start / calibration_done) carry no "data".

Topic: labelcounter/<deviceId>/data   (default subscription uses a + wildcard)
Broker: broker.emqx.io:1883 by default (matches config.h MQTT_HOST/PORT)

Install deps:
    pip install paho-mqtt matplotlib

Run:
    python tools/mqtt_waveform_viewer.py
    python tools/mqtt_waveform_viewer.py --host broker.emqx.io --topic "labelcounter/+/data"
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
        if self._want:
            self.q.put(("status", f"disconnected (rc={rc}) - retrying..."))
        else:
            self.q.put(("status", "disconnected"))

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception as e:
            self.q.put(("status", f"bad payload on {msg.topic}: {e}"))
            return
        rec = parse_packet(msg.topic, payload)
        self.q.put(("packet", rec))


def parse_packet(topic, payload):
    """Normalise a raw payload dict into the record the UI stores."""
    device = payload.get("device")
    if not device:
        parts = topic.split("/")
        device = parts[1] if len(parts) >= 3 else topic
    data = payload.get("data")
    data = [float(x) for x in data] if isinstance(data, list) else None
    return {
        "recv": time.time(),
        "topic": topic,
        "device": str(device),
        "event": str(payload.get("event", "?")),
        "count": payload.get("count"),
        "uptime_ms": payload.get("timestamp"),
        "data": data,
        "raw": payload,
    }


# --------------------------------------------------------------------------- #
#  Waveform statistics
# --------------------------------------------------------------------------- #
def waveform_stats(y, rate_hz):
    a = np.asarray(y, dtype=float)
    n = len(a)
    dur = (n - 1) / rate_hz if n > 1 else 0.0
    return {
        "samples": n,
        "duration_s": dur,
        "peak": float(a.max()) if n else 0.0,
        "min": float(a.min()) if n else 0.0,
        "mean": float(a.mean()) if n else 0.0,
        "median": float(np.median(a)) if n else 0.0,
        "rms": float(np.sqrt(np.mean(a * a))) if n else 0.0,
        "peak_idx": int(a.argmax()) if n else 0,
    }


# --------------------------------------------------------------------------- #
#  Main application window
# --------------------------------------------------------------------------- #
class App(tk.Tk):
    def __init__(self, host, port, topic):
        super().__init__()
        self.title("Label Counter - MQTT waveform viewer")
        self.geometry("1240x760")
        self.configure(bg=C_BG)

        self.q: "queue.Queue" = queue.Queue()
        self.worker = MqttWorker(self.q)
        self.messages: "deque" = deque(maxlen=MAX_MESSAGES)
        self.selected = None

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
        st.configure("TFrame", background=C_BG)
        st.configure("TLabel", background=C_BG, foreground=C_TXT)
        st.configure("TButton", background=C_SURF, foreground=C_TXT)
        st.map("TButton", background=[("active", C_GRID)])
        st.configure("TCheckbutton", background=C_BG, foreground=C_TXT)
        st.configure("TEntry", fieldbackground=C_SURF, foreground=C_TXT)
        st.configure("Treeview", background=C_SURF, fieldbackground=C_SURF,
                     foreground=C_TXT, rowheight=22, borderwidth=0)
        st.configure("Treeview.Heading", background=C_GRID, foreground=C_TXT)
        st.map("Treeview", background=[("selected", C_ACC)],
               foreground=[("selected", C_BG)])

    # ---- connection bar ----------------------------------------------- #
    def _build_conn_bar(self, host, port, topic):
        bar = ttk.Frame(self, padding=(8, 6))
        bar.pack(fill="x")

        ttk.Label(bar, text="Broker").pack(side="left")
        self.e_host = ttk.Entry(bar, width=22)
        self.e_host.insert(0, host)
        self.e_host.pack(side="left", padx=(4, 8))

        ttk.Label(bar, text="Port").pack(side="left")
        self.e_port = ttk.Entry(bar, width=7)
        self.e_port.insert(0, str(port))
        self.e_port.pack(side="left", padx=(4, 8))

        ttk.Label(bar, text="Topic").pack(side="left")
        self.e_topic = ttk.Entry(bar, width=26)
        self.e_topic.insert(0, topic)
        self.e_topic.pack(side="left", padx=(4, 8))

        self.b_conn = ttk.Button(bar, text="Connect", command=self._toggle_conn)
        self.b_conn.pack(side="left", padx=4)
        self.connected = False

        ttk.Label(bar, text="Rate (Hz)").pack(side="left", padx=(16, 0))
        self.e_rate = ttk.Entry(bar, width=6)
        self.e_rate.insert(0, str(DEFAULT_RATE_HZ))
        self.e_rate.pack(side="left", padx=4)
        self.e_rate.bind("<Return>", lambda _e: self._replot())

        ttk.Button(bar, text="Load JSON...", command=self._load_file).pack(side="right", padx=4)

    # ---- body: message list + plot ---------------------------------- #
    def _build_body(self):
        pane = ttk.PanedWindow(self, orient="horizontal")
        pane.pack(fill="both", expand=True, padx=8, pady=4)

        # left: list
        left = ttk.Frame(pane)
        pane.add(left, weight=1)

        cols = ("time", "device", "event", "count", "samples", "peak")
        self.tree = ttk.Treeview(left, columns=cols, show="headings", height=20)
        widths = dict(time=90, device=100, event=118, count=60, samples=68, peak=76)
        for c in cols:
            self.tree.heading(c, text=c.capitalize())
            self.tree.column(c, width=widths[c], anchor="center")
        self.tree.column("device", anchor="w")
        self.tree.column("event", anchor="w")
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

        # right: plot + stats
        right = ttk.Frame(pane)
        pane.add(right, weight=3)

        self.fig = Figure(figsize=(7, 4.2), dpi=100, facecolor=C_SURF)
        self.ax = self.fig.add_subplot(111)
        self._style_axes()
        self.canvas = FigureCanvasTkAgg(self.fig, master=right)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)
        NavigationToolbar2Tk(self.canvas, right)

        ov = ttk.Frame(right, padding=(0, 4))
        ov.pack(fill="x")
        ttk.Label(ov, text="Start thr").pack(side="left")
        self.e_start = ttk.Entry(ov, width=8)
        self.e_start.pack(side="left", padx=(4, 10))
        ttk.Label(ov, text="Stop thr").pack(side="left")
        self.e_stop = ttk.Entry(ov, width=8)
        self.e_stop.pack(side="left", padx=4)
        ttk.Button(ov, text="Apply overlays", command=self._replot).pack(side="left", padx=10)
        for e in (self.e_start, self.e_stop):
            e.bind("<Return>", lambda _e: self._replot())

        self.lbl_stats = tk.Label(right, text="No waveform selected.", justify="left",
                                  bg=C_SURF, fg=C_DIM, font=("Consolas", 9), anchor="w")
        self.lbl_stats.pack(fill="x", pady=(2, 0))

    def _build_statusbar(self):
        self.status = tk.StringVar(value="idle")
        tk.Label(self, textvariable=self.status, bg=C_GRID, fg=C_TXT,
                 anchor="w", padx=8).pack(fill="x", side="bottom")

    def _style_axes(self):
        ax = self.ax
        ax.clear()
        ax.set_facecolor(C_SURF)
        for s in ax.spines.values():
            s.set_color(C_GRID)
        ax.tick_params(colors=C_DIM, labelsize=8)
        ax.grid(True, color=C_GRID, linewidth=0.6, alpha=0.6)
        ax.set_xlabel("Time (s)", color=C_DIM, fontsize=9)
        ax.set_ylabel("Vibration magnitude", color=C_DIM, fontsize=9)

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
        self.after(100, self._drain_queue)

    # ---- message handling ------------------------------------------ #
    def _add_message(self, rec):
        self.messages.append(rec)
        if self._passes_filter(rec):
            self._insert_row(rec, at_top=True)
            self._trim_tree()
        if self.v_follow.get() and rec["data"]:
            kids = self.tree.get_children()
            if kids:
                self.tree.selection_set(kids[0])
                self.tree.see(kids[0])

    def _passes_filter(self, rec):
        return not (self.v_hide_empty.get() and not rec["data"])

    def _row_values(self, rec):
        t = datetime.fromtimestamp(rec["recv"]).strftime("%H:%M:%S")
        peak = f"{max(rec['data']):.0f}" if rec["data"] else "-"
        n = len(rec["data"]) if rec["data"] else 0
        return (t, rec["device"], rec["event"],
                "-" if rec["count"] is None else rec["count"], n, peak)

    def _insert_row(self, rec, at_top=False):
        idx = self.messages.index(rec)
        self.tree.insert("", 0 if at_top else "end", iid=str(id(rec)),
                         values=self._row_values(rec))

    def _trim_tree(self):
        kids = self.tree.get_children()
        for k in kids[MAX_MESSAGES:]:
            self.tree.delete(k)

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
        if not sel:
            return
        rec = self._find_rec(sel[0])
        if rec is None:
            return
        self.selected = rec
        self._replot()

    # ---- plotting ------------------------------------------------- #
    def _rate(self):
        try:
            r = float(self.e_rate.get())
            return r if r > 0 else DEFAULT_RATE_HZ
        except ValueError:
            return DEFAULT_RATE_HZ

    def _replot(self):
        rec = self.selected
        self._style_axes()
        if rec is None:
            self.lbl_stats.config(text="No waveform selected.")
            self.canvas.draw_idle()
            return

        meta = (f"device={rec['device']}  event={rec['event']}  "
                f"count={rec['count']}  uptime={rec['uptime_ms']} ms  "
                f"recv={datetime.fromtimestamp(rec['recv']).strftime('%H:%M:%S')}")

        if not rec["data"]:
            self.ax.text(0.5, 0.5, f"'{rec['event']}' carries no waveform data",
                         ha="center", va="center", color=C_DIM, transform=self.ax.transAxes)
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

        for entry, lbl, col in ((self.e_start, "start thr", C_ACC2),
                                (self.e_stop, "stop thr", "#ffd166")):
            txt = entry.get().strip()
            if txt:
                try:
                    self.ax.axhline(float(txt), color=col, linewidth=1.0,
                                    linestyle=":", label=lbl)
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
            meta + "\n" +
            f"samples={s['samples']}  duration={s['duration_s']:.2f} s  "
            f"peak={s['peak']:.0f} @ {s['peak_idx']/rate:.2f} s  min={s['min']:.0f}  "
            f"mean={s['mean']:.1f}  median={s['median']:.1f}  rms={s['rms']:.1f}"
        ))

    # ---- actions ------------------------------------------------- #
    def _toggle_conn(self):
        if self.connected:
            self.worker.disconnect()
            self.connected = False
            self.b_conn.config(text="Connect")
            self.status.set("disconnected")
        else:
            self.worker.connect(self.e_host.get().strip(),
                                self.e_port.get().strip(),
                                self.e_topic.get().strip())
            self.connected = True
            self.b_conn.config(text="Disconnect")

    def _clear(self):
        self.messages.clear()
        self.selected = None
        self.tree.delete(*self.tree.get_children())
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
            blobs = []
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
