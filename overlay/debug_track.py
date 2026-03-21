#!/usr/bin/env python3
"""
Long-running style rank tracker.
Logs to CSV file + prints summary to console.
Run while playing, stop with Ctrl+C.

Output: debug_track_TIMESTAMP.csv
"""

import time
import sys
import os
from datetime import datetime
from memory_reader import MemoryReader
from style_rank import STYLE_EFFECT_MIN, STYLE_EFFECT_MAX

reader = MemoryReader()
print("Attaching...")
if not reader.attach():
    print("Can't find nightreign.exe")
    exit(1)
if not reader.find_world_chr_man():
    print("WorldChrMan not found")
    exit(1)

timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
csv_path = f"debug_track_{timestamp}.csv"
csv = open(csv_path, "w")
csv.write("real_time,elapsed,rank,effect_count,total_dur,min_dur,max_dur,avg_dur,durations,all_changing_floats\n")

print(f"Tracking to {csv_path}")
print("Play the game! Ctrl+C to stop.\n")
print(f"{'Time':>7} {'Rank':>4} {'#':>2} {'Total':>6} {'Min':>5} {'Max':>5} {'Avg':>5}  Durations")
print("-" * 90)

start_time = time.time()
prev_rank = -1

try:
    while True:
        head = reader.get_sp_effect_list_head()
        if head == 0:
            time.sleep(0.1)
            continue

        node = head
        count = 0
        style_effects = []  # (id, dur_0x48, dur_0x4C, dur_0x50, dur_0x54, val_0x20)

        while node != 0 and count < 500:
            eid = reader.read_i32(node + 0x08)

            if STYLE_EFFECT_MIN <= eid <= STYLE_EFFECT_MAX:
                d48 = reader.read_f32(node + 0x48)
                d4c = reader.read_f32(node + 0x4C)
                d50 = reader.read_f32(node + 0x50)
                d54 = reader.read_f32(node + 0x54)
                v20 = reader.read_f32(node + 0x20)
                style_effects.append((eid, d48, d4c, d50, d54, v20))

            count += 1
            nxt = reader.read_ptr(node + 0x38)
            if nxt == 0 or nxt == head or nxt == node:
                break
            node = nxt

        now = time.time()
        elapsed = now - start_time
        n = len(style_effects)
        rank = min(n, 8)

        durs_48 = [e[1] for e in style_effects if e[1] >= 0]
        total = sum(durs_48)
        min_d = min(durs_48) if durs_48 else 0
        max_d = max(durs_48) if durs_48 else 0
        avg_d = (total / len(durs_48)) if durs_48 else 0

        dur_str = " ".join(f"{d:.2f}" for d in sorted(durs_48))

        # Also collect all the float fields for CSV analysis
        all_floats = "|".join(
            f"{e[0]}:0x48={e[1]:.3f},0x4C={e[2]:.3f},0x50={e[3]:.3f},0x54={e[4]:.3f},0x20={e[5]:.3f}"
            for e in style_effects
        )

        # CSV
        csv.write(f"{now:.3f},{elapsed:.3f},{rank},{n},{total:.3f},{min_d:.3f},{max_d:.3f},{avg_d:.3f},\"{dur_str}\",\"{all_floats}\"\n")
        csv.flush()

        # Console - print on rank change or every 1 second
        rank_names = ["", "D", "C", "B", "A", "S", "SS", "SSS", "V"]
        rname = rank_names[rank] if rank < len(rank_names) else "?"

        if rank != prev_rank:
            print(f"{elapsed:7.1f} {rname:>4} {n:>2} {total:6.2f} {min_d:5.2f} {max_d:5.2f} {avg_d:5.2f}  [{dur_str}]  << RANK CHANGE")
            prev_rank = rank
        elif int(elapsed * 2) % 2 == 0 and int((elapsed - 0.1) * 2) % 2 != 0:
            # Print every ~0.5 seconds
            print(f"{elapsed:7.1f} {rname:>4} {n:>2} {total:6.2f} {min_d:5.2f} {max_d:5.2f} {avg_d:5.2f}  [{dur_str}]")

        time.sleep(0.1)

except KeyboardInterrupt:
    pass

csv.close()
print(f"\n\nSaved to {csv_path}")
print("Open in a spreadsheet or analyze with pandas for full details.")
