#!/usr/bin/env python3
"""
Find the Night Tide timer using the AOB from Control The Tide mod.
The mod hooks: mulss xmm1,[nightreign.exe+OFFSET]
AOB pattern: F3 0F 59 0D ?? ?? ?? ?? F3 0F 2C F1

This instruction multiplies a time delta by a speed value.
The register rbx+0x118 holds a time-related value before this.
"""

import struct
import time
from memory_reader import MemoryReader

reader = MemoryReader()
print("Attaching...")
if not reader.attach():
    print("Can't find nightreign.exe")
    exit(1)

# Find the AOB pattern
print("Scanning for Night Tide AOB pattern...")
pattern = bytes([0xF3, 0x0F, 0x59, 0x0D])  # mulss xmm1, [rip+??]
suffix = bytes([0xF3, 0x0F, 0x2C, 0xF1])   # cvttss2si esi, xmm1

base, size = reader._get_module_info()
print(f"Module base: 0x{base:X}, size: 0x{size:X}")

# Scan for the pattern
matches = []
chunk_size = 0x100000
for offset in range(0, size, chunk_size):
    read_size = min(chunk_size + 16, size - offset)
    data = reader._read_bytes(base + offset, read_size)
    if data is None:
        continue
    pos = 0
    while True:
        idx = data.find(pattern, pos)
        if idx == -1:
            break
        # Check suffix at idx+8
        if idx + 12 <= len(data) and data[idx+8:idx+12] == suffix:
            abs_addr = base + offset + idx
            # Read the RIP-relative offset
            rip_disp = struct.unpack("<i", data[idx+4:idx+8])[0]
            target = abs_addr + 8 + rip_disp  # RIP = instruction addr + 8 (instruction length)
            matches.append((abs_addr, target))
            print(f"  MATCH at 0x{abs_addr:X} -> target 0x{target:X} (nightreign.exe+0x{abs_addr-base:X})")
        pos = idx + 1

if not matches:
    print("  No matches found! AOB may have changed in this version.")
    print("  Trying broader pattern...")
    # Try just the first part
    for offset in range(0, size, chunk_size):
        read_size = min(chunk_size + 16, size - offset)
        data = reader._read_bytes(base + offset, read_size)
        if data is None:
            continue
        pos = 0
        count = 0
        while count < 5:
            idx = data.find(pattern, pos)
            if idx == -1:
                break
            abs_addr = base + offset + idx
            rip_disp = struct.unpack("<i", data[idx+4:idx+8])[0]
            target = abs_addr + 8 + rip_disp
            # Check if target is a reasonable float
            target_val = reader.read_f32(target)
            if target_val and 0.5 < target_val < 2000.0:
                print(f"  Candidate at 0x{abs_addr:X} -> 0x{target:X} = {target_val:.4f}")
                matches.append((abs_addr, target))
            pos = idx + 1
            count += 1

if matches:
    print(f"\nFound {len(matches)} matches. Reading target values:")
    for instr_addr, target_addr in matches:
        val = reader.read_f32(target_addr)
        print(f"  0x{target_addr:X} = {val}")

    # Now let's look at what rbx+0x118 might be
    # The instruction before the hook is: mulss xmm1,[rbx+0x118]
    # We need to find rbx at runtime. Let's scan the target addresses for
    # interesting nearby floats that could be the timer
    print("\nScanning near the target address for timer-like values...")
    for instr_addr, target_addr in matches:
        print(f"\n  Around target 0x{target_addr:X}:")
        for off in range(-0x20, 0x40, 4):
            f = reader.read_f32(target_addr + off)
            i = reader.read_i32(target_addr + off)
            if f and 0.001 < abs(f) < 10000.0:
                marker = ""
                if off == 0:
                    marker = " <-- TIDE SPEED MULTIPLIER"
                print(f"    +{off:+3d}: float={f:.4f} int={i}{marker}")

    # Monitor the tide speed multiplier over time
    if matches:
        target = matches[0][1]
        print(f"\nMonitoring tide speed at 0x{target:X} for 10 seconds...")
        for i in range(20):
            val = reader.read_f32(target)
            print(f"  [{i*0.5:.1f}s] tide_speed = {val}")
            time.sleep(0.5)

print("\n=== Done ===")
