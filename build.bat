@echo off
REM Reuse the existing bootstrapped vcpkg from the LEPP audio project instead
REM of cloning a second ~1.7GB vcpkg tree. This installs only wxwidgets and
REM sqlite3 (static triplet) into it -- everything else it needs is already there.
set "VCPKG_ROOT=C:/Users/james/PycharmProjects/LEPP/AUDIO_ENGINE_CPP/vcpkg"
set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
set "BUILD_DIR=build"
set "TRIPLET=x64-windows-static"

echo.
echo --- 0. Ensuring wxwidgets + sqlite3 are installed (static triplet) ---
"%VCPKG_ROOT%/vcpkg.exe" install wxwidgets:%TRIPLET% sqlite3:%TRIPLET%
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] vcpkg install failed. & goto :eof )

REM This machine has multiple MSVC toolsets installed and some of them
REM have a broken/empty lib\x64 (missing MSVCRTD.lib), which the VS-generator's
REM default toolset selection can pick and fail to link. -T version=14.44 pins
REM the one known-complete toolset (same fix as the LEPP audio engine build).
REM A stale CXX/CC env var pointing at a dead compiler path causes CMake
REM config to fail outright, so clear both before configuring.
set "CXX="
set "CC="

echo.
echo --- 1. Cleaning build directory ---
if exist %BUILD_DIR% ( rd /s /q %BUILD_DIR% )
mkdir %BUILD_DIR%
cd %BUILD_DIR%

echo.
echo --- 2. Running CMake (Static triplet) ---
cmake .. -G "Visual Studio 17 2022" -A x64 -T "version=14.44" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DVCPKG_TARGET_TRIPLET=%TRIPLET%
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. & goto :eof )

echo.
echo --- 3. Compiling PropertyManager.exe ---
cmake --build . --config Release --target PropertyManager -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & goto :eof )
cd ..

echo.
echo --- 4. Deploying standalone EXE ---
if not exist "dist" mkdir dist
copy /Y "build\Release\PropertyManager.exe" "dist\"
if exist "data.db" copy /Y "data.db" "dist\"
if exist "create_tables.sql" copy /Y "create_tables.sql" "dist\"

echo.
echo ========================================================
echo SUCCESS: dist\PropertyManager.exe is fully static!
echo It runs on any Windows 10/11 machine with NO dependencies.
echo ========================================================
