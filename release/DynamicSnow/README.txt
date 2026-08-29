DynamicSnow - Dynamic Snow Deformation
======================================
Version: v609 (2026-08-28)
Requires: Skyrim SE/AE / SKSE64 / Address Library / DynamicShaderCore / Smooth Terrain

【FEATURES】
- Player leaves continuous trenches in snow / sand / mud (Black-Myth-style
  real geometric deformation, no shader tricks)
- NPCs, enemies, wolves / horses / bears / foxes, Dwemer machines leave
  trails as they move
- Corpses leave a shallow imprint pit + surrounding snow mound (burial
  effect) on death or when grabbed and dropped
- Weapon strikes raise snow mounds; arrows / spell impacts blast craters
- Dropped items carve rolling trails
- Fully compatible with ENB (pure geometry, no shader replacement)

【REQUIREMENTS】(install all, load BEFORE this plugin)
- SKSE64            -> https://skse.silverlock.org/
- Address Library   -> https://www.nexusmods.com/skyrimspecialedition/mods/32444
- Smooth Terrain    -> https://www.nexusmods.com/skyrimspecialedition/mods/186875
  (high-density terrain meshes this plugin deforms; patch methods are
  shared with it)
- DynamicShaderCore -> core runtime "DynamicShader.dll" this plugin
  connects to (released together with this plugin / bundled in the
  author's mod pack)

【INSTALL】
1. Install all requirements first.
2. Install with Mod Organizer 2 (drag & drop the zip), or extract the
   whole "DynamicSnow" folder into your mods folder and enable it.
3. Load order: SKSE64 (automatic) -> Address Library -> Smooth Terrain
   -> DynamicShaderCore -> DynamicSnow.

【CONFIGURE】
Edit Data\SKSE\Plugins\DynamicSnow.ini (in MO2: right-click the mod ->
"Open in Explorer"):

  [General]
  MaxFootprints=400    ; snow pile / footprint count kept alive
                       ; 100 - 2000, default 400 (~10s of trail)
                       ; lower  = shorter trail, steadier FPS
                       ; higher = longer trail, heavier cost

Restart the game to apply. Log: Documents\My Games\Skyrim Special
Edition\SKSE\DynamicSnow.log

【COMPATIBILITY】
- Works alongside ENB Series (tested)
- Do NOT enable together with Community Shaders' dynamic snow feature
  (pick one)

【OPEN SOURCE】
- License: GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception
- Derived from (original copyright notices retained):
  - Skyrim Community Shaders (doodlum, MIT) - deformation map / stamping /
    snow class logic: https://github.com/doodlum/skyrim-community-shaders
  - Snow Deformation feature PR (Josef/PppPlyr1) - ConeCS slope / refill
  - Smooth Terrain (hakasapl) - REL offsets & call-site patching methods
- Source: https://github.com/jatelop8/SnowDeformationPlugin
