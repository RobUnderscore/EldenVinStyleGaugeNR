#!/usr/bin/env python3
"""Measure how long each read cycle takes."""

import time
from memory_reader import MemoryReader
from style_rank import read_style_rank

reader = MemoryReader()
print("Attaching...")
reader.attach()
reader.find_world_chr_man()

for i in range(20):
    t0 = time.time()
    state = read_style_rank(reader)
    t1 = time.time()
    print(f"Read took {(t1-t0)*1000:.1f}ms - rank={state.rank} effects={state.effect_count}")
    time.sleep(0.1)
