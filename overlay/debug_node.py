#!/usr/bin/env python3
"""Dump the raw node data for style effects to find the real duration field."""

import time
from memory_reader import MemoryReader
from style_rank import STYLE_EFFECT_MIN, STYLE_EFFECT_MAX
import struct

reader = MemoryReader()
print("Attaching...")
if not reader.attach():
    print("Can't find nightreign.exe")
    exit(1)

print(f"PID: {reader.pid}")
if not reader.find_world_chr_man():
    print("WorldChrMan not found")
    exit(1)

print(f"WorldChrMan ptr: 0x{reader.world_chr_man_ptr:X}")

# Walk list and dump full node data for style effects
head = reader.get_sp_effect_list_head()
if head == 0:
    print("No SpEffect list head")
    exit(1)

print(f"\nHead: 0x{head:X}")
print("Dumping style effect nodes...\n")

# Do 3 dumps, 2 seconds apart, to see what changes
for dump in range(3):
    if dump > 0:
        print(f"\n--- Waiting 2 seconds (dump {dump+1}/3) ---\n")
        time.sleep(2)

    node = head
    count = 0

    while node != 0 and count < 500:
        effect_id = reader.read_i32(node + 0x08)

        if STYLE_EFFECT_MIN <= effect_id <= STYLE_EFFECT_MAX:
            print(f"Node @ 0x{node:X} - ID={effect_id}")
            # Dump 0x80 bytes as both int32 and float
            for off in range(0, 0x80, 4):
                raw = reader._read_bytes(node + off, 4)
                if raw and len(raw) == 4:
                    vi = struct.unpack("<i", raw)[0]
                    vf = struct.unpack("<f", raw)[0]
                    marker = ""
                    if off == 0x08:
                        marker = " <-- SpEffect ID"
                    elif off == 0x38:
                        marker = " <-- next ptr (lo32)"
                    elif -1.0 < vf < 10.0 and vf != 0.0 and abs(vf) > 0.001:
                        marker = f" <-- POSSIBLE TIMER (float={vf:.4f})"
                    print(f"  +0x{off:02X}: int={vi:12d}  float={vf:12.4f}{marker}")
            print()

        count += 1
        next_node = reader.read_ptr(node + 0x38)
        if next_node == 0 or next_node == head or next_node == node:
            break
        node = next_node

print("Done. Compare the 3 dumps - fields that decrease between dumps are timers.")
