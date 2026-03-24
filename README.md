# VinStyleRankGauge

A real-time on-screen gauge overlay for Elden Ring Nightreign's **Elden Vins** mod style rank system. Displays your current style rank (D through V) with a DMC-inspired gauge, complete with glow effects, pulsing, and electricity crackling at higher ranks.

![DLL Overlay](https://img.shields.io/badge/type-DLL%20overlay-blue)
![D3D12](https://img.shields.io/badge/graphics-D3D12%20%2B%20ImGui-green)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Proton-orange)

## What It Does

The Elden Vins mod adds a hidden style rank system to Nightreign. Attacking, deflecting, and perfect dodging builds a gauge that progresses through ranks D, C, B, A, S, SS, SSS, and V. Each rank provides an escalating damage bonus. The gauge decays when you stop attacking.

**The game shows a brief flash when you rank up, but there's no persistent gauge.** This overlay fixes that by reading the game's memory in real-time and rendering a visible gauge on screen.

| Rank | Active Effects | Approx. Damage |
|------|---------------|----------------|
| D    | 1             | 1.0x           |
| C    | 2             | ~1.14x         |
| B    | 3             | ~1.28x         |
| A    | 4             | ~1.66x         |
| S    | 5             | ~1.95x         |
| SS   | 6             | ~2.27x         |
| SSS  | 7             | ~2.69x         |
| V    | 8+            | ~3.25x+        |

## Building

### Requirements

- **Windows**: Visual Studio 2022 with C++ desktop development workload
- **CMake** 3.20+

### Build Steps

1. Open a **VS2022 Developer PowerShell** (or regular PowerShell — the build script sets up the environment automatically).

2. Run the build script:
   ```powershell
   .\build.ps1
   ```

3. The output DLL will be at `build\Release\VinStyleRankGauge.dll`.

### Manual Build (without the script)

```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## Installation

### With Mod Engine 2 (recommended)

1. Copy `VinStyleRankGauge.dll` to your Mod Engine 2 mod folder (the same one with Elden Vins).

2. Edit your `.me3` profile file and add the DLL under `[[natives]]`:
   ```toml
   [[natives]]
   path = "VinStyleRankGauge.dll"
   ```

3. Launch the game through Mod Engine 2 as usual.

### With a DLL Injector

Any DLL injector that can load into the `nightreign.exe` process will work. Inject after the game has loaded to the title screen.

## Usage

Once loaded, the overlay appears automatically when you have an active style rank. It hides when your rank drops to zero.

### Repositioning the Gauge (F7)

Press **F7** to enter position mode:
- A border appears around the gauge and your mouse cursor is freed
- **Drag** the gauge to your preferred position
- Press **F7** again to lock the position and save it

The position is saved to the config file and persists across sessions.

## Configuration

The overlay reads its settings from an INI file. On first launch, a default config is created automatically.

### Config File Locations

The overlay checks these locations in order:

1. **User config** (preferred): `%APPDATA%\VinStyleRankGauge\VinStyleRankGauge.ini`
2. **DLL-relative**: `VinStyleRankGauge.ini` next to the DLL file

If neither exists, defaults are written to the DLL-relative path.

### Config Options

```ini
# VinStyleRankGauge Configuration

# Position on screen (0.0 to 1.0)
#   posX: 0.0 = left edge, 0.5 = center, 1.0 = right edge
#   posY: 0.0 = top edge, 0.5 = middle, 1.0 = bottom edge
posX = 0.5
posY = 0.88

# Size
gaugeWidth = 300
gaugeHeight = 14
fontSize = 2.5

# Transparency (0.0 = invisible, 1.0 = fully opaque)
opacity = 0.85

# Show/hide elements (1 = show, 0 = hide)
showBar = 1
showLabels = 1
showBorder = 0
```

| Setting      | Default | Description                                           |
|-------------|---------|-------------------------------------------------------|
| `posX`      | `0.5`   | Horizontal position (0.0 = left, 1.0 = right)        |
| `posY`      | `0.88`  | Vertical position (0.0 = top, 1.0 = bottom)          |
| `gaugeWidth`| `300`   | Width of the gauge bar in pixels                      |
| `gaugeHeight`| `14`   | Height of the gauge bar in pixels                     |
| `fontSize`  | `2.5`   | Scale of the rank letter display                      |
| `opacity`   | `0.85`  | Overall transparency (0.0 = invisible, 1.0 = opaque) |
| `showBar`   | `1`     | Show the horizontal gauge bar                         |
| `showLabels`| `1`     | Show rank labels (D, C, B, ...) below the bar         |
| `showBorder`| `0`     | Show a border around the gauge                        |

Lines starting with `#` or `;` are treated as comments. You can edit the file while the game is running — changes take effect on next launch (or when position mode saves).

## Platform Support

### Windows (Native)

Works out of the box. The DLL hooks D3D12 directly via VTable discovery using a temporary swap chain, then uses MinHook for inline function hooking.

### Linux (Proton / Steam Play)

The overlay works under Proton because:

- Nightreign uses D3D12, which Proton translates to Vulkan via VKD3D-Proton
- The DLL is loaded by Mod Engine 2 inside the Wine/Proton process, so it runs as native Windows code from the game's perspective
- D3D12 hooks intercept API calls *before* VKD3D translation, so ImGui rendering goes through the same pipeline as the game

**Setup is the same as Windows** — just configure Mod Engine 2 as you normally would for Proton and add the DLL to your `[[natives]]`.

The config file under Proton will be written relative to the DLL location (since `%APPDATA%` maps to a Wine prefix path). This still works fine — the file ends up in your Mod Engine 2 mod folder.

### Logging

The DLL writes a log to `VinStyleRankGauge.log` in the game's working directory. Check this if the overlay doesn't appear — it logs each initialization step (AOB scan, D3D12 hook, ImGui setup) with success/failure status.

## How It Works

The overlay reads the game's memory to determine the current style rank. Here's the chain:

1. **AOB scan** finds the `WorldChrMan` global pointer using the pattern `48 8B 05 ?? ?? ?? ?? 0F 28 F1 48 85 C0`
2. **Pointer chain traversal**: `WorldChrMan` +0x174e8 -> `ChrIns` +0x190 -> `SpEffectCtrl` +0x08 -> linked list head
3. **Linked list walk**: Each node has a SpEffect ID at +0x08 and remaining duration at +0x48. The overlay counts active effects with IDs in the range 999320410-999320429
4. **Rank mapping**: The count of active effects maps directly to the rank (1=D, 2=C, ... 8+=V)
5. **Debouncing**: A 0.25-second grace period prevents flickering when effects expire at rank boundaries

The D3D12 overlay:
- Creates a temporary D3D12 device/swap chain to discover VTable addresses
- Hooks `IDXGISwapChain::Present` (VTable[8]) and `ID3D12CommandQueue::ExecuteCommandLists` (VTable[10])
- Initializes Dear ImGui with the D3D12 backend on the first hooked Present call
- Renders the gauge each frame using ImGui's draw API

For full technical details including memory offsets and SpEffect structures, see [DESIGN.md](DESIGN.md).

## Visual Effects

The gauge rendering includes several rank-dependent effects:

- **Rank letter** with multi-layer glow and drop shadow
- **Gauge bar** with gradient fill matching the rank color
- **Pulsing** — brightness oscillation that increases in speed and intensity with rank
- **Shine highlights** along the top edge and leading edge of the fill
- **Segment dividers** splitting the bar into 8 rank sections with labels
- **Electricity crackling** at S rank and above — random lightning bolts with branching, scaling from 2 bolts at S to 7 at V
- **Ambient glow** at V rank — full-bar pulsing halo

## Dependencies

All dependencies are vendored in `deps/` and built from source:

| Library | Purpose | License |
|---------|---------|---------|
| [MinHook](https://github.com/TsudaKageworking/minhook) | Inline function hooking | BSD 2-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) | Immediate-mode GUI rendering | MIT |
| [Pattern16](https://github.com/Jenocn/Pattern16) | SIMD-accelerated AOB pattern scanning | MIT |

## License

MIT
