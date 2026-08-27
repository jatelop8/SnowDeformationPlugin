# DynamicShader-DynamicSnow (DynamicSnow.dll)

独立 SKSE 功能插件：几何类雪地变形的**数据层**（变形图）。作为 [DynamicShader Core](https://github.com/jatelop8/DynamicShaderCore) 生态的功能插件运行，与 ENB 共存，不替换引擎着色器、不注入几何。

## 构建

1. 双击 `build_ninja.bat`（加载 VsDevCmd → cmake --preset NINJA → 编译 → 自动部署）
2. 输出：`build\NINJA\DynamicSnow.dll`（需同时安装 DynamicShader Core 的 `DynamicShader.dll`）

## License

Licensed under **GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception**.
See [LICENSE](LICENSE) and [EXCEPTIONS.md](EXCEPTIONS.md).

This project is derived from the following open-source projects (all GPL-3.0):

- [Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders) (Copyright (C) Community Shaders contributors) — core codebase; based on the "Snow Deformation feature" pull request by PppPlyr1 (Josef), including kSnowClasses and GetShapeBound helpers
- [Smooth Terrain](https://www.nexusmods.com/skyrimspecialedition/mods/186875) by hakasapl (GPL-3.0) — RE offsets, call-site patching and terrain mesh methods

If you use or modify this code, please keep these attributions and the GPL-3.0 license notice.
