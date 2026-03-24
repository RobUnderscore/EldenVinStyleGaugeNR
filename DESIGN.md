# Elden Vins Style Rank Gauge - Technical Design Document

## Overview

The Elden Vins mod for Elden Ring Nightreign adds a "Style Rank" system inspired by
Devil May Cry. Hitting enemies, deflecting, and perfect dodging builds a gauge that
increases through ranks D → C → B → A → S → SS → SSS → V. Each rank provides an
escalating attack power bonus. The gauge constantly decays when not actively attacking.

This document describes exactly how the system works at the memory level, for the
purpose of building a real-time on-screen gauge overlay.

## Memory Pointer Chain

```
WorldChrMan (resolved via AOB scan, registered as CE symbol)
  │
  └─ +0x174e8 ──► ChrIns (player character instance)
                    │
                    └─ +0x190 ──► CSSpecialEffect (SpEffect controller)
                                   │
                                   └─ +0x08 ──► Head of active SpEffect linked list
```

### AOB Pattern for WorldChrMan (from Hexinton CE table)
```
48 8B 05 ?? ?? ?? ?? 0F 28 F1 48 85 C0
```
Resolved via RIP-relative addressing at offset +3.

### Key Offsets (Nightreign-specific, differ from base Elden Ring)
| Path | ER Offset | NR Offset | Purpose |
|------|-----------|-----------|---------|
| WorldChrMan → PlayerIns | +0x10EF8 | +0x174e8 | Player character instance |
| ChrIns → SpEffectCtrl | +0x178 | +0x190 | SpEffect controller |
| ChrIns → ChrModuleBase | +0x190 | +0x1B8 | Character modules (HP, FP, etc.) |

## SpEffect Linked List Node Structure

Each active SpEffect on the player is stored in a doubly-linked list node:

```
Offset  Type     Field
──────  ───────  ─────────────────────────
+0x00   ptr      vtable / type pointer
+0x08   int32    SpEffect Param ID
+0x18   int32    val18 (accumulator counter? varies)
+0x38   ptr      Next node in linked list
+0x40   ptr      Previous node in linked list
+0x48   float    Remaining duration (seconds, -1.0 = infinite)
+0x4C   float    Remaining duration (duplicate/backup?)
+0x50   float    Max duration (5.0 for style effects, -1.0 = infinite)
+0x54   float    Timer/elapsed value
+0x58   float    1.0 (probably a rate multiplier)
```

### Walking the List
```
head = read_ptr(SpEffectCtrl + 0x08)
node = head
while node != NULL and node != head (on subsequent iterations):
    id = read_i32(node + 0x08)
    duration = read_f32(node + 0x48)
    max_duration = read_f32(node + 0x50)
    // process effect...
    node = read_ptr(node + 0x38)
    if node == head: break  // circular list
```

## Style Rank SpEffect IDs

The style rank system uses SpEffect IDs **999320410 through 999320429**.

Each hit adds one of these effects with a **5.0 second duration**. The number of
simultaneously active effects in this range determines the current rank:

| Active Count | Rank | Letter Display |
|-------------|------|----------------|
| 0 | None | (nothing) |
| 1 | D | D |
| 2 | C | C |
| 3 | B | B |
| 4 | A | A |
| 5 | S | S |
| 6 | SS | SS |
| 7 | SSS | SSS |
| 8+ | V | V (max) |

### Gauge Behavior
- **Building**: Each qualifying hit (melee attack, deflect, dodge) adds a new
  999320410+ effect with 5.0 second duration. If one with that ID already exists,
  it may be refreshed.
- **Decay**: Each effect has an independent 5.0 second countdown timer. When a
  timer reaches 0, that effect is removed. If no new hits occur, effects expire
  one by one, causing the rank to drop: V → SSS → SS → S → A → B → C → D → None.
- **Gauge fullness within a rank**: The `min_duration` across active style effects
  indicates how close you are to losing one tier. When `min_duration` approaches 0,
  you're about to drop. The `max_duration` indicates the freshest hit.

## Other Related SpEffect IDs

### Rank Display VFX (flash letter on screen)
| SpEffect | VFX ID | Letter |
|----------|--------|--------|
| 1951 | 7000 | D |
| 1953 | 7001 | C |
| 1955 | 7002 | B |
| 1957 | 7003 | A |
| 1959 | 7004 | S+ |

### Rank Persistent Buffs (mutual exclusion groups)
| SpEffect | Excl Group | Rank |
|----------|------------|------|
| 1950 | 459 | D |
| 1952 | 460 | C |
| 1954 | 461 | B |
| 1956 | 462 | A |
| 1958 | 463 | S+ |

### Damage Tier Buffs (attack power multipliers)
SpEffects 7000-7190 provide escalating damage:
| SpEffect | Multiplier (approx) |
|----------|---------------------|
| 7000 | 1.0x (baseline) |
| 7010 | 1.14x |
| 7020 | 1.28x |
| 7030 | 1.66x |
| 7040 | 1.81x |
| 7050 | 1.95x |
| 7060 | 2.27x |
| 7070 | 2.41x |
| 7080 | 2.69x |
| 7090 | 3.25x |
| 7100 | 3.70x |
| 7110 | 4.12x |
| 7120 | 4.84x |
| 7130 | 5.48x |
| 7140 | 6.56x |
| 7150 | 6.69x |
| 7160 | 6.88x |
| 7170 | 7.05x |
| 7180 | 7.20x |
| 7190 | 7.33x |

### Base System SpEffects (on weapons via EquipParamWeapon +0x54)
| SpEffect | Purpose |
|----------|---------|
| 1940 | Base passive enabler (permanent, always active) |
| 1942 | Accumulation trigger (stateInfo=10) |
| 1944 | Accumulation trigger (stateInfo=13) |
| 1946 | Accumulation trigger (stateInfo=30) |
| 1948 | Accumulation trigger (stateInfo=31) |

### EMEVD Event Script
The tier buff application logic is in `common_func.emevd.dcx` at byte offsets
0xD700-0xDA00. This custom EMEVD event maps the accumulation state to the
specific damage tier buff (7000-7190).

## Implementation

The overlay is implemented as a C++ DLL that hooks D3D12 via VTable discovery + MinHook.
It renders using Dear ImGui and is loaded via Mod Engine 2's `[[natives]]` mechanism.

The DLL reads WorldChrMan via AOB scan, walks the SpEffect linked list every frame,
counts active style effects, and renders the gauge using ImGui's draw API.
