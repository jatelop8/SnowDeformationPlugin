// DynamicShader-DynamicSnow (https://github.com/jatelop8/SnowDeformationPlugin)
// Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception.
// Derived from open-source projects (all GPL-3.0):
//   - Skyrim Community Shaders (https://github.com/doodlum/skyrim-community-shaders),
//     DeformationMap / StampCollector / SnowShellMesh logic (incl. kSnowClasses, GetShapeBound)
//     based on the "Snow Deformation feature" PR by PppPlyr1 (Josef)
//   - Smooth Terrain by hakasapl (https://www.nexusmods.com/skyrimspecialedition/mods/186875),
//     REL offsets and call-site patching methods used in SnowShellMesh.cpp / main.cpp

#pragma once

#include <RE/Skyrim.h>

#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <vector>
#include <unordered_map>

struct ID3D11DeviceContext;

namespace SnowDeform
{
	// 程序化"雪壳网格"（Phase 2 核心：地形雪变形视觉载体）
	// 用 NiStream 加载模板 NIF（assets/snowshell.nif，NiNode + BSTriShape，
	// 2304 顶点平面）——引擎 NIF 加载器自动创建 rendererData（GPU 渲染数据）。
	//
	// v63（动态网格正路，2026-08-20）：
	//   引擎解析 BSDynamicTriShape（动态网格块类型）在"静态+动态混合"数据上会崩
	//   （v59b 实测）；因此走**静态 BSTriShape + D3D11 每帧更新 GPU 顶点缓冲**：
	//   - rendererData = BSGraphics::TriShape* @0x138（BSGeometry 成员）
	//   - rawVertexData @0x20 = CPU 顶点数组（28B/顶点：POSITION 12B + UV 4B +
	//     NORMAL 4B + TANGENT 4B ...）
	//   - vertexBuffer @0x00 = ID3D11Buffer*（GPU 缓冲）
	//   每帧（渲染线程，Present hook）：改 rawVertexData 的 POSITION 为世界坐标
	//   → UpdateSubresource 上传 GPU。顶点写世界坐标 + 挂世界节点（单位变换）。
	class SnowShellMesh
	{
	public:

		// 每帧记录玩家位置（Present hook 内调用；与 LANDSCAPE 更新同线程）
		void UpdatePlayerPos(const RE::NiPoint3& a_pos);
		// 从场景移除并释放

		[[nodiscard]] bool IsValid() const { return root != nullptr; }

		// v126：真地形变形（LANDSCAPE）——诊断玩家 cell 的 landscape 渲染网格结构
		void DebugLandscape();
		// v129：真地形踩雪变形——FindLandscape（游戏线程缓存）/ UpdateLandscape（渲染线程变形）
		// v209：a_skipBuild=true 时不触发高密重建（Tick 完成后内部重缓存用，防 Build↔Find 死循环）
		void FindLandscape(bool a_skipBuild = false);
		void UpdateLandscape(ID3D11DeviceContext* a_ctx);
		// v544：auto-create 重试用——landscape 缓存是否成功就绪（landReady）
		[[nodiscard]] bool IsLandReady() const { return landReady.load(); }
		// v342：接触模型形状扫描（游戏线程 AddTask 调度）——鞋/武器网格投影地面生成
		// ShapeStamp（mask），盖章/RebuildField 只读缓存
		void ScanContactShapes();
		// v346：Havok 碰撞体扫描（游戏线程 AddTask，50ms）——玩家 3D 树碰撞体
		// → colliders 缓存（CS Dynamic Snow Stamping 同款，安全不读网格顶点）
		void ScanColliders();
		// v447：**移动物品盖章（用户"头盔掉地滚动也出效果"）**——游戏线程 200ms，
		// ForEachReferenceInRange 玩家周围 1500 → 可掉落物（MISC/WEAP/ARMO/ALCH/
		// INGR/AMMO/KEYM/BOOK/SLGM）→ 位置跟踪（lastObjPos）→ 移动 >20 + 贴地
		//（z-GetLandHeight<100）→ 盖章（圆形 r=10，与脚印同场）
		void ScanMovingObjects();
		// v448：**砍击采集联动（用户：OIF 联动，斧砍地面出凹陷+资源）**——游戏线程
		// 50ms 轮询玩家攻击状态（kSwing/kHit/kFollowThrough 边沿触发）+ 斧武器
		//（kOneHandAxe/kTwoHandAxe/木材斧）→ 玩家前方 120 地面盖章（圆形 r=14），
		// **每次 -5 深、同格 40 单位最多 5 次**（mineCounts 格计数，满 5 必须换地）。
		// OIF（Nexus 149484）负责资源掉落，我们并行负责几何凹陷。
		void ScanPlayerMining();
		// v439：地形碰撞体诊断——遍历玩家 cell 3D 碰撞体，确认地形碰撞形状
		// （hkpSampledHeightFieldShape / hkpMoppBvTreeShape）在哪、什么布局，
		// 为"碰撞跟随下降"（玩家踩坑不悬空）铺路。游戏线程低频调度。
		void ScanCellCollision();
		// v170：高密度网格替换（骨架保留，方向转材质视差——见 v171）
		// v190：程序建 255² 高密度网格（引擎 CreateTriShapeData，无需 NIF）+
		// heights 双线性插值 + 替换 mesh.child + 重缓存
		// v195：a_force=true 强制重建（v160 检测到引擎 LOD 重建后自动重新替换）
		// v196：hook K_BUILD_LAND_GEOMETRY 内调用 BuildQuadTriShape 的 call 指令——
		// 引擎每次构建/重建 quad 网格时我们立即 setMesh 换成缓存的 255²（SmoothTerrain
		// 方式）。渲染管线拿到的永远是最新对象 → 根治"只认 2 块"。
		void InstallQuadBuildHook();
		// v559：**投射物命中 hook（箭矢 + 法术爆炸雪地效果）**——hook MissileProjectile/
		// ArrowProjectile vtable 槽 0xBD（AddImpact 命中回调），命中位置写变形场：
		// shape=12 箭矢小坑+雪堆、shape=13 法术爆炸大坑+环形雪堆。AddImpact 在游戏线程
		// 调用（引擎主循环命中检测），footMtx 保护写 footprints。
		using AddImpactFn = void (*)(RE::Projectile*, RE::TESObjectREFR*, const RE::NiPoint3&,
			const RE::NiPoint3&, RE::hkpCollidable*, std::int32_t, std::uint32_t);
		static AddImpactFn g_addImpactArrow;    // 原 ArrowProjectile::AddImpact
		static AddImpactFn g_addImpactMissile;  // 原 MissileProjectile::AddImpact
		static void AddImpactHookArrow(RE::Projectile* self, RE::TESObjectREFR* a_ref,
			const RE::NiPoint3& a_loc, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
			std::int32_t a6, std::uint32_t a7);
		static void AddImpactHookMissile(RE::Projectile* self, RE::TESObjectREFR* a_ref,
			const RE::NiPoint3& a_loc, const RE::NiPoint3& a_vel, RE::hkpCollidable* a_col,
			std::int32_t a6, std::uint32_t a7);
		static void HandleProjectileImpact(RE::Projectile* self, const RE::NiPoint3& a_loc, int a_shape);
		void InstallProjectileHook();
		// v560：**动物脚印（马匹/动物）**——ProcessLists::ForEachHighActor 遍历玩家
		// 附近动物，找 Hoof/Paw/Foot 脚节点（子串匹配收集全部），脚节点世界位置盖章
		//（shape=14 蹄印：小椭圆坑+小雪堆，300s 回填）。动物静止时脚不动 → 距离节流
		// 天然防重复；人形（FaceGenHead race）排除。
		void ScanAnimalFeet();
		// v562：脚印贴花（v561 系列 SimpleDecal 路线）已全部移除（用户拍板——引擎
		// Initialize 不建几何 geom3d=0x0 实锤，手动构造不可控）
		// v207：分帧构建——main.cpp Present 驱动（渲染线程检测队列 → 游戏线程 Tick）
		// v449b：**读档/新游戏复位（F9 闪退修复）**——引擎卸载全部地形/REFR，
		// cells/geom/footprints 等缓存指向悬空对象 → 写崩（vmovdqa [rcx] AV 实锤）。
		// kPreLoadGame/kPostLoadGame/kNewGame 时清空全部状态，下轮 FindLandscape 重建。
		void ResetForLoadGame();

	private:
		RE::NiAVObject*     root = nullptr;      // NIF 根节点（NiNode）
		RE::BSDynamicTriShape* dynMesh = nullptr;   // BSTriShape（v122 地形采样兜底节点）
		RE::NiPoint3        playerPos{};        // v63：玩家位置（每帧更新）
		void*               rendererData = nullptr;  // BSGraphics::TriShape*（LANDSCAPE BuildCell 替换用）

		// v100：原始顶点局部坐标（x/z）——按顶点坐标变形（适配任意网格，不依赖 i/j 索引）
		std::vector<float>   origXZ;                    // 2 floats/顶点（原始局部 x, z 或 x, y）
		bool                 origIsXZ = true;           // v312：模板平面布局——true=(x,0,z) XZ 平面
		                                                 // （snowshell.nif 旧版 y=0）；false=(x,y,0) XY 平面
		                                                 // （Blender Grid 默认未旋转，z=0）
		std::uint32_t        meshVertexCount = 0;      // 实际顶点数（GetTrishapeRuntimeData）
		bool                 initialized = false;

		// v74：踩雪变形（CPU 脚印）；v121：水平坐标 = x/y（Skyrim Z 是高度）
		// v483b：+tMs（盖章时间戳，物品回填衰减用——雪被压后随时间恢复）
		struct Footprint { float x, y, depth, radius; float dirX, dirY, rL, rS; float prevX, prevY; int shape = 0; unsigned long tMs = 0; }; // v187：+prevX/Y 胶囊线段（上一脚→本脚=战壕）；v342：+shape（0=椭圆 1/2=鞋 mask 3=武器 mask）
		std::vector<Footprint> footprints;                        // 脚印列表（上限 512）

		// v342：**接触模型形状检测**——黑神话级效果：变形按真实模型（鞋/武器）形状，
		// 不是固定椭圆。游戏线程每 500ms 扫描玩家 3D 树（安全：不碰引擎动画/换装），
		// 把鞋底/武器网格顶点投影到地面生成 2D mask；渲染线程盖章/RebuildField 只读
		// 缓存（mutex 保护，写 500ms 一次 / 读每帧一次，无竞争）。
		struct ShapeStamp {
			float cx = 0.0f, cy = 0.0f;        // 形状中心（世界，投影地面）
			float len = 0.0f, wid = 0.0f;      // 沿朝向长 / 垂直朝向宽
			float dirX = 1.0f, dirY = 0.0f;    // 朝向（世界水平单位向量）
			std::vector<float> mask;           // maskDim×maskDim 0/1（形状占据）
			int maskDim = 0;                   // 0 = 无 mask（椭圆兜底）
			float maskRes = 2.0f;              // 单位/cell
			bool valid = false;
		};
		ShapeStamp          bootShape[2];      // [0]=L [1]=R 鞋（游戏线程写）
		ShapeStamp          weaponShape;       // 右手武器（游戏线程写）
		std::mutex          shapeMtx;
		// v382：**footprints 互斥锁**（22:23 堆损坏真凶）——v359 起盖章在游戏线程
		// ScanColliders push_back（vector 扩容），渲染线程 RebuildField/UpdateLandscape
		// 遍历 + erase → 无锁并发 = 堆损坏（v380 布局变化触发 po3/OIF free 崩）。
		std::mutex          footMtx;

		// v346：**Havok 碰撞体盖章（CS Dynamic Snow 同款）**——玩家 3D 树碰撞体
		// （bhkNiCollisionObject）逐形状收集 {中心, 半径}，游戏线程扫描（安全：
		// 不读网格顶点，碰撞体不随换装重建）、渲染线程盖章只读（colliderMtx）。
		// 位移 > kColliderStampStep 盖一个胶囊（prev→curr 连续轨迹，CS 同款）。
		struct ColliderStamp {
			float x = 0.0f, y = 0.0f;        // 当前中心（世界水平）
			float px = 0.0f, py = 0.0f;      // 上次盖章位置（胶囊起点）
			float radius = 0.0f;             // 碰撞体半径（世界单位）
			float z = 0.0f;                  // 中心世界 z（贴近地面过滤）
			const RE::hkpShape* shape = nullptr;  // v437b：碰撞形状指针（Havok 管理长存，
			                                      // 盖章时按轨迹方向投影半轴——椭圆战壕）
		};
		std::unordered_map<std::uint32_t, ColliderStamp> colliders;  // key=formID低16位|shapeIdx<<16
		std::mutex          colliderMtx;
		unsigned long       colliderScanLast = 0;  // 扫描节流（50ms）
		// v447：移动物品盖章状态（掉落/滚动物品 → 雪痕）
		// v452：ObjTrack 扩展——记录上次盖章位置/时间（轨迹疏密 + 节流）
		struct ObjTrack {
			RE::NiPoint3 pos{ 0.0f, 0.0f, 0.0f };
			RE::NiPoint3 stampPos{ 0.0f, 0.0f, 0.0f };  // 上次盖章位置
			unsigned long stampTime = 0;                 // 上次盖章时间（ms）
		};
		std::unordered_map<RE::FormID, ObjTrack> lastObjPos;  // 物品跟踪（移动/盖章状态）
		unsigned long       movingScanLast = 0;    // 扫描节流（200ms）
		// v448：砍击采集状态（斧砍地面 → 凹陷 5 次上限）
		std::unordered_map<std::int64_t, int> mineCounts;  // 砍击格计数（key=40 单位格）
		unsigned long       miningScanLast = 0;    // 扫描节流（50ms）
		unsigned long       animalScanLast = 0;    // v560：动物脚印扫描节流（100ms）
		RE::NiPoint3        lastFootPos{};                        // 上次盖章位置
		bool                footInited = false;
		// v287：落地检测盖章——脚 z/水平位置上一采样 + 落地去抖 + 扫描节流
		// （v163 按"玩家移动 60"盖章在脚摆动中触发 → 坑乱 → 改落地瞬间盖）
		float               prevFootZ[2] = { 0.0f, 0.0f };        // [0]=L [1]=R 脚 z 上一采样
		float               prevFootX[2] = { 0.0f, 0.0f };
		float               prevFootY[2] = { 0.0f, 0.0f };
		bool                prevFootInit[2] = { false, false };
		unsigned long       footCooldownUntil[2] = { 0, 0 };      // 落地盖章去抖（每脚 300ms）
		unsigned long       footScanLast = 0;                     // 落地扫描节流（30ms）
		// v293：胶囊链——上一脚盖章位置（每脚独立 → 走出 CS 风格连续战壕/弧形沟）
		float               lastStampX[2] = { 0.0f, 0.0f };        // [0]=L [1]=R 上一脚盖章 xy
		float               lastStampY[2] = { 0.0f, 0.0f };
		bool                stampInited[2] = { false, false };    // 首盖初始化（防 0→fx 跨图）
		// v295：鞋 mesh 形状缓存——**FindBootMesh 整树遍历必须游戏线程执行**
		// （崩溃实锤：渲染线程遍历玩家 3D 树与游戏线程装备挂载竞争 → 悬空指针
		// "Scb" RIP=0 → 盖章即闪退）。游戏线程每 1 秒扫描缓存形状，渲染线程
		// 盖章只读缓存（位置/朝向仍用脚节点 world transform，渲染线程安全）。
		std::atomic<bool>   bootShapeReady{ false };
		std::atomic<bool>   bootShapePending{ false };
		unsigned long       bootScanLast = 0;      // v296：上次鞋 mesh 扫描时间（每 10 秒重扫）

		// v122：地形贴合——雪壳顶点贴合真实 LANDSCAPE 高度（不再是"玩家高度"平面）。
		// terrainH 由游戏线程采样（AddTask 转发，GetLandHeight 是引擎函数只能在游戏
		// 线程调），渲染线程只读缓存。terrainVersion 防撕裂：
		// 渲染线程取版本快照，不一致则本帧跳过贴合（下帧用新数据）。
		std::vector<float>  terrainH;                             // 每顶点地形高度缓存
		std::atomic<std::uint32_t> terrainVersion{ 0 };           // 缓存版本（写端游戏线程）
		RE::NiPoint3        lastTerrainPos{};                     // 上次采样位置
		std::atomic<bool>   terrainSampling{false};               // v569：跨线程（渲染请求/游戏完成）改 atomic
		void                RequestTerrainSample(const RE::NiPoint3& a_playerPos);  // 渲染线程
		void                DoTerrainSample();                    // 游戏线程（AddTask）

		// v124：法线重算（真实沟壑立体感）——坑壁法线朝侧面 → 光照真实明暗

		// v130：**真地形踩雪变形**（LANDSCAPE）——直接改真实地形网格顶点。
		// 跨 cell 连续：缓存玩家周围 3×3 cell × 4 quadrant geom（9×4）。
		//   q=3：高分辨率 65×65（当前 quadrant，局部坐标 + worldT）
		//   q=0-2：低分辨率 17×17（世界坐标 tr=0）
		// 世界坐标统一 = 局部 + worldT → 脚印用世界坐标连续场匹配 → cell/quadrant
		// 边界顶点共享世界坐标 → 同步下陷 → 无缝无裂缝（v129 单 cell 边界破壳修复）。
		// 双缓冲 landBuf[2]：FindLandscape（游戏线程）写备用缓冲 → 原子切换 idx，
		// 渲染线程每帧用 idx 指向的缓冲——修掉改指针/vector 与遍历的跨线程竞争。
		struct LandCellGeom {
			RE::BSTriShape* geom = nullptr;      // BSTriShape 渲染网格
			std::uint8_t*   raw = nullptr;       // 引擎 rawVertexData（只读！v134 起不写它）
			std::vector<std::uint8_t> orig;      // 原始顶点副本（防累积变形，永远干净）
			std::vector<std::uint8_t> work;      // v134：工作副本——变形写这里再上传，
			                                     // 引擎 raw 保持干净（重缓存 landOrig 从
			                                     // 干净的 raw 拷贝 → 无累积/无地形漂移）
			std::uint32_t   stride = 40;
			std::uint32_t   verts = 0;
			float           worldT[3] = {};      // 世界变换（局部→世界）
			float           centerX = 0.0f;      // 顶点包围盒中心（脚印粗筛）
			float           centerY = 0.0f;
			float           halfDiag = 0.0f;     // 包围盒半对角 + margin
			bool            raised = false;      // v134：已执行首次雪层抬高上传
			int             surfaceClass = 0;   // v445：quad 材质分类（0=其他/1=雪/2=沙/3=泥——FindLandscape 分类）——非雪沙泥不变形不挖坑（v206 起 isSnow，v445 扩展三类）
		};
		LandCellGeom        landBuf[2][49][4];   // v258：7×7 cell 双缓冲 [帧代][cell 0..48][quadrant]
		                                     // 7×7=49 cell（28672² 单位=3.5 cell 半径）——覆盖玩家
		                                     // 视野（3-4 cell）→ 129² 与引擎 65² 的交界（裂缝源，
		                                     // 用户 v257 实锤"走一下又有裂缝"= 5×5 边缘）移到视野外
		                                     // 3×3=1.5 cell 半径时裂缝近在眼前；5×5 视野边缘可见
		std::atomic<int>    landBufIdx{ 0 };     // 当前有效缓冲索引
		std::atomic<bool>   landReady{ false };
		std::atomic<bool>   landFootDirty{ false };   // v197：盖章/重建后置 true——UpdateLandscape 只在 dirty 时全量重算+上传（否则每帧遍历 13 万顶点 → 帧率减半）
		std::atomic<bool>   landRebuildPending{ false }; // v197：v160 检测到引擎重建 → FindLandscape 后置 true，下一帧重算上传  // 缓存就绪（FindLandscape 写）
		std::atomic<float>  landAnchorX{ 0.0f }; // 缓存中心玩家位置（cell 切换检测）
		std::atomic<float>  landAnchorY{ 0.0f };
		// v358：**雪壳固定锚点**（Initialize 时玩家位置快照）——雪壳是世界的一部分
		// （固定），不跟随玩家（v357 跟随被用户否决：4700² 地毯跟着平移太出戏）。
		// 不用 landAnchor（v284b null=96：SmoothTerrain 破坏缓存 → 锚点不更新）。
		std::atomic<float>  fixedAnchorX{ 0.0f };
		std::atomic<float>  fixedAnchorY{ 0.0f };
		// v360：**reentrancy guard**（v359 失败根因：AddTask 多 worker 线程并发跑
		// ScanColliders → 同 map 多线程写 + 同一 px/py 被并发读错乱 → 6 个 stamp
		// 在 3 个线程同时盖 r=8.1 碰撞体 → 多个独立深色斑块+并发崩溃风险）
		std::atomic<bool>    scanning{ false };
		std::uint32_t       landLastRequest = 0; // 最近一次 FindLandscape 请求时间
		bool                landUploaded = false; // 首帧变形日志
		std::uint32_t       landLastDiag = 0;    // v133：周期变形诊断（每 3 秒）
		std::uint32_t       landLastDiag2 = 0;   // v140：玩家 cell 象限状态诊断（每 2 秒）
		std::uint32_t       landLastDiag3 = 0;   // v142：网格重建检测（每 2 秒）
		std::atomic<bool>   landExpDirty{ false }; // v149：实验标志（4 象限抬高 300 验证渲染）
		std::atomic<bool>   landPaused{ false };   // v149：实验期间暂停正常变形
		bool                landLayoutLogged = false; // v159：mesh.child 顶点布局已 dump
		bool                playerSrcLogged = false;  // v260：玩家 cell 源密度已 dump
		bool                landHLogged = false;      // v280：GetLandHeight 静息覆盖只首次（防每次 rebuild 边界跳变闪）

		// v266：**CPU 变形场（世界坐标采样）**——用户方案"边界顶点链接一起变形"：
		// 所有网格（129² + 引擎 289/65²）的顶点从**同一张世界坐标变形场**采样 →
		// 相邻 cell 边界顶点（同一世界位置）变形量必然一致 → 无交界高低差 → 无裂缝，
		// 且坑/雪堆可跨 cell 边界连续（不再需要 v265 的边缘 5 圈截断）。
		// v401：**场分辨率 32→16（用户选 A：强化几何，CS 数据对照）**——CS 变形图
		// 4 单位/texel；我们 32 时战壕直径 35.6 < 1 texel → 双线性摊薄 → 沟壑离散
		// （用户"不如之前 LANDSCAPE 效果好"）。16 单位/texel：战壕 35.6 = 2.2 texel
		// → 双线性采样平滑；几何顶点 16 单位格 = 场 texel 1:1 → 沟壑细腻连续。
		// 覆盖 896×16=14336 单位（玩家周围 7168，脚印 2600 清理 + 盒子 2048 足够）。
		// RebuildField 循环量 ×4（3000 脚印 × 15×15）——若卡再降 stamps 或 R 半径。
		// v438：**场分辨率 16→8（对齐 CS 4/texel 的一半，用户"三角尖刺/不圆润"）**——
		// 坑半径 10~18 < 16 场 texel → 坑只覆盖 1-2 采样点 → 双线性插出金字塔尖塔
		// （三角尖刺感）。v476：**场分辨率 8→4（用户"场分辨率变成2"）**——
		// step=2 全量会卡死（dim 7168、assign 5138万×2 场、内存 400MB+、重建
		// 80-130ms）；step=4 + dim 3584 = 覆盖保持 14336（不变）+ assign ×4
		//（~20-30ms 可接受）+ 方块边长 16→8（更圆）。注意：方块根源是地形
		// 顶点 16 间距（129² 上限）——场变细只改善边缘，盖章 ≥24 半径才真圆。
		static constexpr int    kFieldDim = 3584;       // 采样点/边（14336/4）
		static constexpr float  kFieldStep = 4.0f;      // 采样间隔（世界单位）
		std::vector<float>      deformField;            // 压缩度场（0~1，坑下陷）[dim*dim]——玩家脚印
		std::vector<float>      ridgeField;             // 雪堆场（正，凸起）[dim*dim]——玩家脚印
		std::vector<float>      deformFieldObj;         // v529：物品/拖痕/深坑场（与玩家脚印分离——用户"武器盖章覆盖脚下导致脚印变化，互不影响"）
		std::vector<float>      ridgeFieldObj;          // v529：物品/拖痕/深坑雪堆
		float                   fieldOriginX = 0.0f;    // 场原点（对齐 kFieldStep 网格）
		float                   fieldOriginY = 0.0f;
		std::atomic<bool>   fieldReady{false};  // v567：跨线程（FindLandscape 游戏线程 false / RebuildField 渲染线程 true）改 atomic
		void                    RebuildField();         // 清空 + 所有脚印写入（盖章/回填/跨 cell）
		void                    SampleField(float wx, float wy, float& deformOut, float& ridgeOut) const;
		void                    SampleFieldObj(float wx, float wy, float& deformOut, float& ridgeOut) const;  // v529：物体场采样
		void                    SampleFieldNearest(float wx, float wy, float& deformOut, float& ridgeOut) const;     // v564：最近邻（kFieldStep=4 顶点对齐，热路径 4 读→1 读）
		void                    SampleFieldObjNearest(float wx, float wy, float& deformOut, float& ridgeOut) const;  // v564：物体场最近邻

		// v435：**场景雪堆（墙边/岩石边自动堆雪）**——游戏线程（FindLandscape 末尾）
		// 重建：收集玩家周围静态 REFR 的碰撞包围球（GetColliderBound，只读碰撞体安全）
		// → 224²@64 稀疏雪堆场（贴墙隆起 kSceneLiftMax，向外 kSceneSpan 衰减到 0）。
		// 渲染线程 SampleSceneLift 叠加到顶点 z（与坑/坑沿雪堆同场合成）。
		// 双缓冲 + 原子 idx 切换：重建期间渲染线程读旧缓冲，零竞争零闪烁。
		struct SceneObstacle {
			float x, y, radius;   // 世界水平中心 + 包围半径
		};
		struct SceneLiftBuf {
			float ox = 0.0f, oy = 0.0f;  // 场原点（@kSceneStep 对齐）
			std::vector<float> field;    // [kSceneDim*kSceneDim] 雪堆高度（正）
		};
		static constexpr int    kSceneDim = 224;        // 采样点/边（14336/64，与主场同覆盖）
		static constexpr float  kSceneStep = 64.0f;     // 采样间隔（世界单位）
		std::array<SceneLiftBuf, 2> sceneLiftBuf;       // 双缓冲（重建写 1-idx，写完原子切）
		std::atomic<int>        sceneLiftIdx{ 0 };
		void                    BuildSceneLift();       // 游戏线程（FindLandscape 末尾）
		void                    SampleSceneLift(float wx, float wy, float& out) const;

		// v170：高密度网格替换（打破 65×65 天花板）——Blender 导出 highres_quadrant.nif
		// （129/257 网格平面）→ NiStream 加载（引擎建 rendererData/vb/ib）→ 替换
		// mesh[q].child[0]（引擎真正渲染对象，v156 实锤）→ 每帧高度插值+脚印变形。
		// 注：v171 用户转向"材质视差"路线（ENB 复杂视差+高度图），本骨架暂存。
		std::atomic<bool>  highResReady{ false };     // 加载成功
		std::atomic<bool>  highResReplaceQueued{ false }; // 渲染线程检测→游戏线程替换
		// v430：草式雪壳（v299/v301 B 方案）已清理——纯地形版不需要雪壳层
		// 高密度网格原始高度插值源（替换时从被替换的 65×65 网格采样，防 z=0 塌陷）
		// v190：程序建 255² 高密度网格（引擎 CreateTriShapeData 函数，顶点做到最好）
		// 255²=65025 顶点 < 65536（uint16 索引上限）→ 间距 8.06，比 SmoothTerrain
		// 129²（间距 16）细 2 倍——战壕/鞋印/雪堆细腻。高度源 = heights[4][289]
		// 双线性插值（无 Catmull-Rom → 无尖塔过冲）。替换 mesh.child 的 rendererData
		//（SmoothTerrain setMesh 模式：rendererData/vertexCount/triangleCount/bound）。
		// v193：per-quadrant 数组——**4 个象限全部替换**（v190 只换 1 个象限 =
		// "只渲染一个地块"翻版！v156 教训：引擎渲染 mesh[0..3].child 全部 4 个）。
		bool               highResBuilt = false;      // 已建网格
		// v196：255² → 181²！triangleCount 是 uint16（max 65535），255² 的三角形数
		// 254×254×2=129032 溢出被截断 → 引擎只渲染部分三角形 → "两块应用两块空"
		// 真凶。181²：32761 顶点 < 65536 ✓、64800 三角形 < 65535 ✓、间距 11.38——
		// uint16 极限内的最大密度（SmoothTerrain 129² 更保守但间距 16 更粗）。
		// v201：181² 保留（用户否决降密度——之前白做）。接缝正路 = **扩展 181² 到 3×3 cell**：
		// 玩家 cell + 8 邻居全部替换 181² 同算法 → 边界顶点世界坐标一致 → 无缝。
		// v213：129²（间距 16）——v212 低配版 65²（间距 32）坑半径放大到 38 太大"不像人踩的"；
		// 129² 间距 16 < 鞋印宽 36，坑半径 18 真实大小。129² 与 65² 顶点对齐（偶数索引
		// 重合：32=2×16）→ 边缘无缝无拉伸。uint16 内 181² 是极限但 129² 帧率更稳（v174-188
		// 用户验证"很不错"）。3×3 覆盖（9 cell × 4 quad × 16641 = 60 万顶点，中等帧率代价）。
		// v226：129²（间距 16）——坑半径 1.5 倍 spacing 覆盖 3×3 顶点可见（v226 用户实测有效，
		// 只是"太宽+排开"观感；v227/v228 改密度/半径后"啥也没有"——回退 v226 保可见性）。
		std::uint32_t      highResDim = 129;          // 网格密度（顶点数 = dim²）
		// v224：**5×5 cell（25 cell × 4 quad = 100 块）129²——关键裂缝修复**：
		// v213-223 时代构建范围 3×3 < landBuf 缓存 5×5，玩家移动后新 3×3（新快照）与
		// 旧 3×3 残留 129²（旧快照）交界 → "走了以后裂缝"。5×5 与 landBuf 完全同尺寸
		// 同快照 → 视野内全同批 129²，组内零裂缝。129² 与 65² 顶点对齐（32=2×16）无缝。
		void*              highResRd[49][4] = {};     // 每 cell×quad 新 rendererData
		std::vector<std::uint8_t> highResVerts[49][4]; // 每 cell×quad CPU 顶点（stride=40）
		std::vector<std::uint16_t> highResIdx;        // 共用索引（局部网格相同），重建 rd 用
		// v258：分帧构建状态——49 cell 每帧建 4 个（约 13 帧 ~0.2s 完成，中心优先）
		// v226：自动补建——主队列建完时引擎低分辨率 LOD（17²/33²）的 cell 被 v225 密度
		// 门槛 skip → 记录到 coarseSkips，每 1 秒重缓存（FindLandscape(true)）+ 重扫补建。
		// 引擎升级 65² 后自动补建成功 → "走一次有裂缝，来回走才平"变成"自动 1-3 秒弄平"。
		// v196：hook 状态（引擎构建 quad 网格的瞬间 setMesh 替换 255²）
		RE::TESObjectLAND::LoadedLandData* highResHookLD = nullptr;  // 玩家 cell 的 ld（hook 判断）
		static RE::BSTriShape* (*s_origBuildQuad)(RE::TESObjectLAND::LoadedLandData*, std::uint32_t);  // 原函数
		static RE::BSTriShape* BuildQuadHookThunk(RE::TESObjectLAND::LoadedLandData*, std::uint32_t);  // thunk（类成员→可访问私有）

		// v430：盒子 EnvMask/法线动态纹理（v364-v396）已清理——纯地形版不需要


		// v430：LoadMesh/AttachSnowMaterial/AttachSnowShell 等雪壳死代码已清理——
		// 纯地形版无 NIF 加载链，stream/snowStream 成员随删（无悬空风险）
	};

	// 全局单例
	SnowShellMesh& GetSnowShellMesh();
}
