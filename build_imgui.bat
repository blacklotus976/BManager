@echo off
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VCPKG_TOOLCHAIN=C:/Users/james/PycharmProjects/LEPP/AUDIO_ENGINE_CPP/vcpkg/scripts/buildsystems/vcpkg.cmake"
set "BUILD_DIR=build"
set "CXX="
set "CC="

echo.
echo --- 1. Configuring (static triplet -- fully static exe, zero runtime deps) ---
"%CMAKE%" -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -T "version=14.44" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DVCPKG_TARGET_TRIPLET=x64-windows-static
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. & exit /b 1 )

echo.
echo --- 2. Building Release (PropertyManager) ---
"%CMAKE%" --build %BUILD_DIR% --config Release --target PropertyManager -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- 3. Dependency report (what's built-in vs. what the host PC must have) ---
powershell -NoProfile -ExecutionPolicy Bypass -File "dependency_report.ps1"

echo.
echo --- SUCCESS: PropertyManager.exe is fully static, no DLLs to deploy ---

