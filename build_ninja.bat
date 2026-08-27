@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul 2>&1
set VCPKG_ROOT=E:\vcpkg-full
set VCPKG_DOWNLOADS=E:\vcpkg-full\downloads
cd /d D:\Modding\DynamicShader-DynamicSnow
cmake --preset NINJA
if errorlevel 1 exit /b 1
cmake --build --preset NINJA
exit /b %errorlevel%
