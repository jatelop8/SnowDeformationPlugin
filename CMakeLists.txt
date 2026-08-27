cmake_minimum_required(VERSION 3.24)
project(DynamicSnow)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 独立 CommonLibSSE-NG（官方仓库，不再引用 Community Shaders 的 extern——用户要求绕开 CS 生态）
# -DCOMMONLIB_DIR 可覆盖路径，默认 D:/Modding/CommonLibSSE-NG
if(NOT DEFINED COMMONLIB_DIR)
    set(COMMONLIB_DIR "D:/Modding/CommonLibSSE-NG")
endif()

# vcpkg 预装库（手动安装，不走 vcpkg 自动安装——规避 VS 检测 bug）
if(NOT DEFINED VCPKG_PREFIX)
    set(VCPKG_PREFIX "E:/vcpkg-full/installed/x64-windows-static-md")
endif()

set(CommonLibPath "${COMMONLIB_DIR}")
set(CommonLibName "CommonLibSSE")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/DeformationMap.cpp
    src/StampCollector.cpp
    src/MeshScanner.cpp
    src/SnowShellMesh.cpp
    src/LinkStubs.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN SKYRIM)

target_compile_options(${PROJECT_NAME} PRIVATE
    /MP
    /W4
    /wd4200
    /wd4819
    /wd4996
)

target_compile_options(${PROJECT_NAME} PUBLIC
    "$<$<CONFIG:DEBUG>:/fp:strict;/ZI;/Od;/Gy>"
    "$<$<CONFIG:RELEASE>:/Zi;/fp:fast;/GL;/Gy-;/O2;/Ob2;/Oi;/Ot;/Oy>"
)

# CommonLibSSE-NG 源码（用户已验证可编译）
# 跳过 check_ipo_supported 的 MSBuild 试编译（某些环境下 MSBuild 崩溃导致配置失败；
# IPO/LTO 对本插件非必需，禁用后照常构建）
set(_CMAKE_CXX_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
set(_CMAKE_C_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
add_subdirectory(${CommonLibPath} ${CommonLibName} EXCLUDE_FROM_ALL)

# 依赖：spdlog / directxtk（CommonLibSSE-NG 需要），从 vcpkg 预装库查找
find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE "${VCPKG_PREFIX}/include")

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog)

# 生成后复制到游戏 Data 目录（-DPLUGIN_DEPLOY_DIR 指定）
if(DEFINED PLUGIN_DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            "${PLUGIN_DEPLOY_DIR}/SKSE/Plugins/DynamicSnow.dll"
    )
endif()
