# DynamicShader-DynamicSnow (DynamicSnow.dll)

SKSE plugin that deforms the game's **real landscape geometry** in The Elder Scrolls V: Skyrim Special Edition — walk through snow, sand or mud and leave **genuine 3D footprints and tracks that persist**. Pure geometry deformation: no shader replacement, no geometry injection, fully **ENB compatible**.

## Features

- **Real landscape deformation** (LANDSCAPE vertices) — true 3D trenches and footprints, not decals
- **Player footprints** in snow / sand / mud with material-dependent depth (sand & mud leave shallow imprints, no mounds)
- **Horses & animals** leave independent hoof prints — exact V1 stamping algorithm (v630)
- NPCs, enemies, wolves / horses / bears / foxes, Dwemer machines leave trails as they move
- **Corpses** leave a shallow imprint pit + surrounding snow mound on death or when grabbed and dropped
- **Weapon strikes** raise snow mounds; arrows / spell impacts blast craters
- **Dropped items** carve rolling trails
- Footprints **persist across cell borders** — continuous world-space deformation field, seamless
- **Recalculated normals** (dual-pass Gaussian smoothing, kNormScale 3.0) so footprints catch light correctly
- **Jump detection** — no stamps while airborne; corpse imprints are kept
- **Fast startup** — first attempt ~5s after load, 1s retry until ready
- Works alongside **ENB Series** (tested)

## Requirements

- Skyrim Special Edition (SE / AE)
- SKSE64
- Address Library for SKSE Plugins
- Smooth Terrain (high-density terrain meshes — provides the deformation grid)
- DynamicShader Core (`DynamicShader.dll` — core runtime this plugin connects to)

## Installation

1. Install all requirements first.
2. Install with Mod Organizer 2 / Vortex, or extract the mod folder into your Data folder.
3. Load order: SKSE64 (automatic) → Address Library → Smooth Terrain → DynamicShader Core → DynamicSnow.

## Configuration

Edit `Data\SKSE\Plugins\DynamicSnow.ini` (in MO2: right-click the mod → "Open in Explorer"):

```ini
[General]
MaxFootprints=100    ; footprints / snow piles kept alive (trail length vs cost)
                     ; 100 - 2000, default 400 (~10s trail)
                     ; lower = shorter trail, steadier FPS
                     ; higher = longer trail, heavier per-stamp rebuild cost
```

Restart the game to apply. Log: `My Games\Skyrim Special Edition\SKSE\DynamicSnow.log`

## Compatibility

- Works alongside ENB Series (tested)
- Do **not** enable together with Community Shaders' dynamic snow feature (pick one)

## Changelog

### v630 (2026-08-30)
- Fully restore the original V1 (00afed2) horse/animal stamping algorithm: `ForEachHighActor` (high-priority active actors only), 300-unit distance, global 300 ms throttle shared by all animals, 20-unit gate per foot, single-point ellipse hoof print (rL=6 / rS=4, depth 0.3 → pit −5.4, shape 14)
- `ForAllActors` keeps only corpse imprints (v590+)
- Riding leaves clean ellipse hoof prints under each hoof

### Earlier v6xx line
- v620 sand/mud shallow imprints · v621 horse 4-hoof independent stamps · v622 fast startup (~5s) · v623 jump detection + unified hoof depth · v628–v629 V1 algorithm recovery · smoothed geometry & normals (kNormScale 3.0)

## Building from source

1. Run `build_ninja.bat` (loads VsDevCmd → cmake --preset NINJA → build → auto-deploy).
2. Output: `build\NINJA\DynamicSnow.dll` (requires DynamicShader Core's `DynamicShader.dll` at runtime).

Requirements: Visual Studio Build Tools (MSVC x64), CMake ≥ 3.24, Ninja, vcpkg (spdlog preinstalled), a local CommonLibSSE-NG clone.

## Credits & License

Licensed under **GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception**.
See [LICENSE](LICENSE) and [EXCEPTIONS.md](EXCEPTIONS.md).

This project is derived from the following open-source projects (all GPL-3.0):

- [Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders) (Copyright (C) Community Shaders contributors) — core codebase; based on the "Snow Deformation feature" pull request by PppPlyr1 (Josef), including kSnowClasses and GetShapeBound helpers
- [Smooth Terrain](https://www.nexusmods.com/skyrimspecialedition/mods/186875) by hakasapl (GPL-3.0) — RE offsets, call-site patching and terrain mesh methods

Build dependencies (MIT, not derived from):

- [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) (MIT) — RE/REL framework
- [spdlog](https://github.com/gabime/spdlog) (MIT) — logging

If you use or modify this code, please keep these attributions and the GPL-3.0 license notice.
