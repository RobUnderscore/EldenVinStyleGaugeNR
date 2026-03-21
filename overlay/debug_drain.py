#!/usr/bin/env python3
"""Measure the actual drain rate of style effect timers."""

import time
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
print("Ready. Build up style rank, then stop attacking and watch the drain.\n")

# Sample every 0.5 seconds for 30 seconds
prev_time = None
prev_total = None

for i in range(60):
    head = reader.get_sp_effect_list_head()
    if head == 0:
        time.sleep(0.5)
        continue

    node = head
    count = 0
    effects = []

    while node != 0 and count < 500:
        eid = reader.read_i32(node + 0x08)
        if STYLE_EFFECT_MIN <= eid <= STYLE_EFFECT_MAX:
            dur = reader.read_f32(node + 0x48)
            effects.append((eid, dur))
        count += 1
        nxt = reader.read_ptr(node + 0x38)
        if nxt == 0 or nxt == head or nxt == node:
            break
        node = nxt

    now = time.time()
    total_dur = sum(d for _, d in effects if d >= 0)
    n_effects = len(effects)

    rate_str = ""
    if prev_time is not None and prev_total is not None and n_effects > 0:
        dt = now - prev_time
        d_total = total_dur - prev_total
        if dt > 0:
            rate = d_total / dt  # seconds of game time per second of real time
            rate_str = f"  drain rate: {rate:+.3f} game-sec/real-sec"

    durs = " ".join(f"{d:.2f}" for _, d in sorted(effects))
    print(f"[{i*0.5:5.1f}s] rank={n_effects} total={total_dur:.2f}  [{durs}]{rate_str}")

    prev_time = now
    prev_total = total_dur
    time.sleep(0.5)
