#!/usr/bin/env python3
"""
Scan GameMan and SessionManager for timer-like float values.
Run during gameplay and watch for values that count up or down.
"""

import time
import struct
from memory_reader import MemoryReader

reader = MemoryReader()
print("Attaching...")
if not reader.attach():
    print("Can't find nightreign.exe")
    exit(1)
if not reader.find_world_chr_man():
    print("WorldChrMan not found")
    exit(1)

# We need to find GameMan and SessionManager via AOB scan
# GameMan AOB from Hexinton: 48 8B 05 ?? ?? ?? ?? 83 B8 ?? ?? ?? ?? 00 ?? ?? ?? ?? ?? ?? 48 8D 4C 24
# SessionManager AOB: ?? 8B EC 48 83 EC ?? 48 8B F9 C7 45 F0 00 00 00 00 48 8B 0D

# For now, let's scan the GameMan structure by finding it via the same AOB approach
gameman_aob = "48 8B 05 ?? ?? ?? ?? 83 B8"
gameman_ptr = reader.find_aob(gameman_aob) if hasattr(reader, 'find_aob') else 0

# Simpler approach: just read from known CE symbols if the game is loaded
# Read WorldChrMan and then scan nearby global pointers
wcm = reader.get_world_chr_man()
print(f"WorldChrMan: 0x{wcm:X}")

# Scan the game module for interesting timer values
# Look at GameDataMan offset (from Hexinton: GameDataMan AOB = "48 8B 0D ???????? F3 48 0F 2C C0")
# For now, just dump floats from various game manager structures
# by following pointers from WorldChrMan's neighborhood

# Read ChrIns for reference
chrins = reader.get_chr_ins()
print(f"ChrIns: 0x{chrins:X}")

# Scan nearby large structures for countdown-like floats
# The game's in-game time is usually in GameDataMan or GameMan
# Let's just scan the ChrIns neighborhood for interesting floats
# that might be the day/night timer

print("\nScanning for countdown floats near game structures...")
print("Looking for floats in range 0-600 that change over time\n")

# Take 3 snapshots 2 seconds apart
snapshots = []
for snap in range(3):
    if snap > 0:
        time.sleep(2)

    vals = {}
    # Scan around ChrIns + large offsets where game state might be
    modules = reader.read_ptr(chrins + 0x1b8)

    # Read from various module pointers
    for mod_off in range(0x00, 0x108, 0x08):
        mod_ptr = reader.read_ptr(modules + mod_off)
        if mod_ptr and mod_ptr > 0x10000:
            # Scan this module for timer-like floats
            for off in range(0, 0x200, 4):
                f = reader.read_f32(mod_ptr + off)
                if f and 1.0 < f < 600.0:  # likely timer range (1 second to 10 minutes)
                    key = f"modules+0x{mod_off:02X}->+0x{off:03X}"
                    vals[key] = f

    # Also scan WorldChrMan area
    for off in range(0, 0x200, 4):
        f = reader.read_f32(wcm + off)
        if f and 1.0 < f < 600.0:
            key = f"WCM+0x{off:03X}"
            vals[key] = f

    snapshots.append(vals)
    print(f"Snapshot {snap+1}: {len(vals)} candidate floats")

# Find values that CHANGED between snapshots (timers count up or down)
print("\n=== Values that changed (likely timers) ===\n")
common_keys = set(snapshots[0].keys()) & set(snapshots[1].keys()) & set(snapshots[2].keys())

changing = []
for key in sorted(common_keys):
    v0 = snapshots[0][key]
    v1 = snapshots[1][key]
    v2 = snapshots[2][key]
    # Check if consistently changing in one direction
    d1 = v1 - v0
    d2 = v2 - v1
    if abs(d1) > 0.1 and abs(d2) > 0.1:
        direction = "UP" if d1 > 0 and d2 > 0 else "DOWN" if d1 < 0 and d2 < 0 else "FLUCTUATING"
        rate = (d1 + d2) / 4.0  # per second (2 seconds between snapshots)
        changing.append((key, v0, v1, v2, direction, rate))
        print(f"  {key}: {v0:.2f} -> {v1:.2f} -> {v2:.2f}  ({direction}, {rate:+.2f}/s)")

if not changing:
    print("  No consistently changing values found in scanned range.")
    print("  The timer might be in a different structure (GameMan, SessionManager).")
    print("  Try running this during daytime when the circle is actively closing.")

print("\n=== Done ===")
