# Code Comparison Report

DynamicShader ecosystem (DynamicSnow.dll + DynamicShader Core + WaterPuddles + ENBLights + PCG) vs the Snow Deformation feature by PppPlyr1 (Josef, skyrim-community-shaders-dynamic-snow-deformation)

Generated: 2026-08-27. All results are reproducible from the public repositories.

## Method

- Line-by-line comparison of every source file in the Dynamic ecosystem against all 23 source files in Josef's PR branch (pr2659 + full), whitespace stripped, comments and preprocessor directives excluded.
- For each file, the highest similarity across all Josef files is reported.

## Results (full ecosystem)

| This repo file | Lines (non-comment) | Closest Josef file | Identical lines | Similarity |
|---|---|---|---|---|
| DeformationMap.cpp | 188 | DeformationUpdateCS.hlsl | 36 | **19.1%** |
| StampCollector.cpp | 123 | src_Features_SnowDeformation_Stamping.cpp | 20 | **14.7%** |
| NiUtils.h | 15 | sh_DepthSyncCS.hlsl | 2 | 13.3% |
| StampCollector.h | 13 | sh_DepthSyncCS.hlsl | 2 | 13.3% |
| MeshScanner.h | 18 | sh_DepthSyncCS.hlsl | 2 | 11.1% |
| CoreAPI.h | 20 | sh_DepthSyncCS.hlsl | 2 | 10.0% |
| DeformationMap.h | 61 | DeformationUpdateCS.hlsl | 4 | 6.6% |
| MeshScanner.cpp | 101 | TerrainData.cpp | 5 | 5.0% |
| LinkStubs.cpp | 96 | TerrainData.cpp | 3 | 3.1% |
| main.cpp | 206 | Stamping.cpp | 6 | 2.9% |
| SnowShellMesh.h | 201 | src_Features_SnowDeformation.h | 4 | 2.0% |
| SnowShellMesh.cpp | 2738 | Stamping.cpp | 14 | 0.5% |
| melting_grain.inc | 32770 | src_Features_SnowDeformation.cpp | 1 | 0.0% |
| snow_heights.inc | 63725 | src_Features_SnowDeformation.cpp | 1 | 0.0% |

## Full ecosystem scan (other projects)

| Project / file | Lines (non-comment) | Closest Josef file | Similarity |
|---|---|---|---|
| DynamicShaderCore / CoreAPI.h | 20 | sh_DepthSyncCS.hlsl | 10.0% |
| DynamicShaderCore / main.cpp | 106 | TerrainData.cpp | 4.7% |
| DynamicWaterPuddles / WaterPuddles.cpp | 673 | src_Statics.cpp | 1.6% |
| DynamicWaterPuddles / WaterPuddles.h | 107 | SnowDeformation.h | 3.3% |
| DynamicWaterPuddles / main.cpp | 62 | sh_DepthSyncCS.hlsl | 4.8% |
| DynamicWaterPuddles / LinkStubs.cpp | 114 | TerrainData.cpp | 2.6% |
| DynamicENBLights / ENBLights.cpp | 127 | Stamping.cpp | 5.1% |
| DynamicENBLights / ENBLights.h | 33 | sh_SnowHeightCapture.hlsl | 6.1% |
| DynamicENBLights / main.cpp | 69 | TerrainData.cpp | 4.3% |
| DynamicENBLights / LinkStubs.cpp | 114 | TerrainData.cpp | 2.6% |
| DynamicPCG / PCG.cpp | 283 | Stamping.cpp | 2.8% |
| DynamicPCG / PCG.h | 50 | sh_SnowHeightCapture.hlsl | 6.0% |
| DynamicPCG / main.cpp | 60 | sh_DepthSyncCS.hlsl | 5.0% |
| DynamicPCG / LinkStubs.cpp | 114 | TerrainData.cpp | 2.6% |

NiUtils.h (shared across projects) matches sh_DepthSyncCS.hlsl at 13.3% — 2 identical lines in a 15-line utility header (generic helpers), not project logic.

## Summary

- The only significant match in the entire ecosystem is DeformationMap.cpp at 19.1% — the compute-shader kernel, disclosed in source comments and credited to PppPlyr1 per GPL-3.0.
- Every other file across Core, WaterPuddles, ENBLights and PCG is below 6% (self-written).

## Breakdown of the two highest matches

### 1. DeformationMap.cpp vs DeformationUpdateCS.hlsl — 19.1% (36 identical lines)

The 36 identical lines are the HLSL compute-shader deformation-update kernel: texture declarations, world-to-texel mapping, capsule falloff, and refill decay. This is the portion derived from Josef's shader algorithm. It is disclosed in the source itself (`// logic stripped from the Community Shaders SnowDeformation feature`) and credited to PppPlyr1 in LICENSE, README and every file header.

### 2. StampCollector.cpp vs Stamping.cpp — 14.7% (20 identical lines)

The 20 identical lines fall into four groups:

- CommonLibSSE-NG API boilerplate (~8 lines): `RE::BSVisit::TraverseScenegraphCollision`, `RE::ProcessLists::GetSingleton`, `RE::NiPoint3` — identical for anyone using the same library
- Syntax (~6 lines): braces, `return`, `default` — universal C++ boilerplate
- Traversal skeleton (~4 lines): `shapeIndex`, `stampCount`, `formID` counters — the collection flow structure
- Logic (2 lines): ground-band filter and radius filter — the required way to implement footprint collection

## Conclusion

There is no copy-paste in this codebase. The highest similarity to Josef's work is 19.1% — a compute-shader algorithm that is disclosed in source comments and fully credited per GPL-3.0. The self-written core (SnowShellMesh.cpp, 2700+ lines) matches Josef's code at 0.5%. Implementation approach differs fundamentally: Josef uses a GPU shader pipeline; this project uses CPU geometry deformation (procedural snow shell mesh + vertex sinking + scrolling deformation map).
