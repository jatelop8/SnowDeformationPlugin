# 跳过 vcpkg VS 检测 bug，直接指向已手动安装的 vcpkg 预编译库
set(CMAKE_PREFIX_PATH "E:/vcpkg-full/installed/x64-windows-static-md" CACHE PATH "本地 vcpkg 库")
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "" FORCE)
