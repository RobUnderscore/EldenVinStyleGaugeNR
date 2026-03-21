#!/usr/bin/env python3
"""
Elden Vins Style Rank Gauge - Cross-platform overlay.
Works on Windows and Linux (Proton).

Usage:
    python main.py
"""

import sys
import time
import threading
import tkinter as tk
from memory_reader import MemoryReader
from style_rank import read_style_rank, StyleRankState, RANK_NAMES, RANK_COLORS


class StyleRankOverlay:
    """Transparent overlay window showing the style rank gauge."""

    GAUGE_WIDTH = 300
    GAUGE_HEIGHT = 16
    UPDATE_INTERVAL_MS = 50  # 20 FPS update rate
    WINDOW_WIDTH = 400
    WINDOW_HEIGHT = 100
    SMOOTH_WINDOW = 2.0  # seconds to smooth rank over (eliminates recycle flicker)

    def __init__(self):
        self.reader = MemoryReader()
        self.state = StyleRankState()
        self.connected = False
        self.running = True

        # Create window
        self.root = tk.Tk()
        self.root.title("Style Rank")
        self.root.overrideredirect(True)  # No window border
        self.root.attributes("-topmost", True)  # Always on top

        # Transparent background
        if sys.platform == "win32":
            self.root.attributes("-transparentcolor", "black")
            self.root.config(bg="black")
        else:
            # Linux: use RGBA visual for transparency
            self.root.wait_visibility(self.root)
            self.root.attributes("-alpha", 0.9)
            self.root.config(bg="#111111")

        # Position: bottom center of screen
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        x = (screen_w - self.WINDOW_WIDTH) // 2
        y = screen_h - 200
        self.root.geometry(f"{self.WINDOW_WIDTH}x{self.WINDOW_HEIGHT}+{x}+{y}")

        # Make window click-through on Windows
        if sys.platform == "win32":
            import ctypes
            hwnd = ctypes.windll.user32.GetForegroundWindow()
            # Will be set after window is created

        # Canvas for drawing
        bg = "black" if sys.platform == "win32" else "#111111"
        self.canvas = tk.Canvas(
            self.root,
            width=self.WINDOW_WIDTH,
            height=self.WINDOW_HEIGHT,
            bg=bg,
            highlightthickness=0,
            bd=0,
        )
        self.canvas.pack(fill=tk.BOTH, expand=True)

        # Allow dragging the window
        self.canvas.bind("<Button-1>", self._start_drag)
        self.canvas.bind("<B1-Motion>", self._drag)

        # Right-click to quit
        self.canvas.bind("<Button-3>", lambda e: self._quit())

        # Rank smoothing - use max rank seen in last SMOOTH_WINDOW seconds
        self._rank_history = []  # list of (timestamp, rank)
        self._smoothed_rank = 0

        # Start memory reading thread
        self.mem_thread = threading.Thread(target=self._memory_loop, daemon=True)
        self.mem_thread.start()

        # Start UI update loop
        self.root.after(self.UPDATE_INTERVAL_MS, self._update_ui)

    def _start_drag(self, event):
        self._drag_x = event.x
        self._drag_y = event.y

    def _drag(self, event):
        x = self.root.winfo_x() + event.x - self._drag_x
        y = self.root.winfo_y() + event.y - self._drag_y
        self.root.geometry(f"+{x}+{y}")

    def _quit(self):
        self.running = False
        self.reader.detach()
        self.root.destroy()

    def _memory_loop(self):
        """Background thread that reads game memory."""
        while self.running:
            if not self.connected:
                # Try to attach
                if self.reader.attach():
                    print(f"Attached to nightreign.exe (PID {self.reader.pid})")
                    if self.reader.find_world_chr_man():
                        print(f"Found WorldChrMan at 0x{self.reader.world_chr_man_ptr:X}")
                        self.connected = True
                    else:
                        print("WorldChrMan not found yet, retrying...")
                        self.reader.detach()
                        time.sleep(5)
                        continue
                else:
                    time.sleep(3)
                    continue

            # Read style rank
            try:
                self.state = read_style_rank(self.reader)
            except Exception as e:
                print(f"Read error: {e}")
                self.connected = False
                self.reader.detach()
                time.sleep(1)
                continue

            time.sleep(0.05)  # ~20 reads per second

    def _update_ui(self):
        """Update the overlay canvas."""
        if not self.running:
            return

        self.canvas.delete("all")
        state = self.state

        # No smoothing needed - style_rank.py handles recycle grace period
        state_rank = state.rank

        if not self.connected:
            self.canvas.create_text(
                self.WINDOW_WIDTH // 2, self.WINDOW_HEIGHT // 2,
                text="Waiting for Nightreign...",
                fill="#666666", font=("Arial", 12)
            )
        elif state_rank > 0:
            r, g, b = RANK_COLORS[state_rank] if state_rank < len(RANK_COLORS) else (128,128,128)
            color = f"#{r:02x}{g:02x}{b:02x}"

            # Rank letter - large centered
            self.canvas.create_text(
                self.WINDOW_WIDTH // 2 + 2, 27,
                text=RANK_NAMES[state_rank] if state_rank < len(RANK_NAMES) else "?",
                fill="#000000",
                font=("Arial", 32, "bold")
            )
            self.canvas.create_text(
                self.WINDOW_WIDTH // 2, 25,
                text=RANK_NAMES[state_rank] if state_rank < len(RANK_NAMES) else "?",
                fill=color,
                font=("Arial", 32, "bold")
            )

            # Gauge bar - shows rank as discrete segments (8 max)
            bar_x = (self.WINDOW_WIDTH - self.GAUGE_WIDTH) // 2
            bar_y = 55

            # Background
            self.canvas.create_rectangle(
                bar_x, bar_y,
                bar_x + self.GAUGE_WIDTH, bar_y + self.GAUGE_HEIGHT,
                fill="#1a1a1a", outline="#333333", width=1
            )

            # Fill based on rank (each rank = 1/8 of the bar)
            fill_ratio = state_rank / 8.0
            fill_width = int(self.GAUGE_WIDTH * fill_ratio)
            if fill_width > 0:
                self.canvas.create_rectangle(
                    bar_x + 1, bar_y + 1,
                    bar_x + 1 + fill_width, bar_y + self.GAUGE_HEIGHT - 1,
                    fill=color, outline=""
                )

            # Segment dividers
            for i in range(1, 8):
                tick_x = bar_x + (self.GAUGE_WIDTH * i // 8)
                self.canvas.create_line(
                    tick_x, bar_y, tick_x, bar_y + self.GAUGE_HEIGHT,
                    fill="#555555"
                )

            # Small rank labels under segments
            labels = ["D", "C", "B", "A", "S", "SS", "SSS", "V"]
            for i, label in enumerate(labels):
                lx = bar_x + (self.GAUGE_WIDTH * i // 8) + (self.GAUGE_WIDTH // 16)
                label_color = color if i < state_rank else "#333333"
                self.canvas.create_text(
                    lx, bar_y + self.GAUGE_HEIGHT + 10,
                    text=label,
                    fill=label_color,
                    font=("Arial", 7)
                )
        else:
            # No rank - hide or show minimal
            pass

        self.root.after(self.UPDATE_INTERVAL_MS, self._update_ui)

    def run(self):
        """Start the overlay."""
        print("Elden Vins Style Rank Gauge")
        print("Left-click and drag to reposition. Right-click to quit.")
        print("Searching for nightreign.exe...")
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            self._quit()


if __name__ == "__main__":
    overlay = StyleRankOverlay()
    overlay.run()
