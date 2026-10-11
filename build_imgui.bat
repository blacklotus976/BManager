@echo off
setlocal EnableExtensions
REM ===========================================================================
REM  PropertyManager - Windows build (MSVC + vcpkg, fully static exe)
REM
REM  Usage:   build_imgui.bat            normal build (+ tests)
REM           build_imgui.bat video      also install FFmpeg for the in-app video player
REM                                      (first time takes a long while; without it videos
REM                                       open in the Windows default player)
REM
REM  Settings (optional, set before running or edit the defaults below):
REM    VCPKG_ROOT      folder of your vcpkg checkout (must contain vcpkg.exe)
REM    MSVC_TOOLSET    MSVC toolset to pin, e.g. 14.44 ("none" = let Visual Studio choose)
REM    BUILD_DIR       build folder (default: build)
REM ===========================================================================

if not defined VCPKG_ROOT set "VCPKG_ROOT=C:/Users/james/PycharmProjects/LEPP/AUDIO_ENGINE_CPP/vcpkg"
if not defined MSVC_TOOLSET set "MSVC_TOOLSET=14.44"
if not defined BUILD_DIR set "BUILD_DIR=build"
set "TRIPLET=x64-windows-static"
REM A stale CXX/CC pointing at a removed compiler makes CMake fail; clear them.
set "CXX="
set "CC="

REM ---- find cmake: PATH first, then the copy bundled with Visual Studio --------
REM (no parenthesised blocks here: %ProgramFiles(x86)% contains ")" and would end them early)
set "CMAKE="
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%i"
if defined CMAKE goto :have_cmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :have_cmake
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :have_cmake
if exist "%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
:have_cmake
if not defined CMAKE echo [ERROR] cmake not found. Install Visual Studio 2022 with "C++ CMake tools" or CMake.
if not defined CMAKE exit /b 1
REM ctest lives next to cmake
for %%d in ("%CMAKE%") do set "CTEST=%%~dpdctest.exe"
if not exist "%VCPKG_ROOT%\vcpkg.exe" echo [ERROR] vcpkg.exe not found in VCPKG_ROOT=%VCPKG_ROOT% - set VCPKG_ROOT to your vcpkg folder.
if not exist "%VCPKG_ROOT%\vcpkg.exe" exit /b 1
echo Using cmake: %CMAKE%
echo Using vcpkg: %VCPKG_ROOT%

echo.
echo --- 1. Dependencies (static triplet, already-installed ones are skipped quickly) ---
"%VCPKG_ROOT%\vcpkg.exe" install "imgui[glfw-binding,opengl3-binding]" glfw3 nlohmann-json --triplet %TRIPLET%
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] vcpkg install failed. & exit /b 1 )
if /i "%~1"=="video" (
    echo Installing FFmpeg for the in-app video player...
    "%VCPKG_ROOT%\vcpkg.exe" install ffmpeg --triplet %TRIPLET%
    IF ERRORLEVEL 1 ( echo [ERROR] vcpkg install ffmpeg failed. & exit /b 1 )
)

echo.
echo --- 2. Configuring ---
set "TOOLSET_ARG="
if /i not "%MSVC_TOOLSET%"=="none" set "TOOLSET_ARG=-T version=%MSVC_TOOLSET%"
"%CMAKE%" -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 %TOOLSET_ARG% -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=%TRIPLET%
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. If the toolset is the problem, run: set MSVC_TOOLSET=none & exit /b 1 )

echo.
echo --- 3. Building Release (app + tests) ---
"%CMAKE%" --build %BUILD_DIR% --config Release --target PropertyManager money_test receipts_test history_test audit_test attachments_test -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- 4. Tests ---
pushd %BUILD_DIR%
"%CTEST%" -C Release --output-on-failure
set "TESTS_RC=%ERRORLEVEL%"
popd
IF NOT "%TESTS_RC%"=="0" ( echo [ERROR] Tests failed - do not ship this build. & exit /b 1 )

echo.
echo --- 5. Dependency report (what's built-in vs. what the host PC must have) ---
powershell -NoProfile -ExecutionPolicy Bypass -File "dependency_report.ps1"

echo.
echo --- 6. dist\ ---
if not exist "dist" mkdir dist
copy /Y "%BUILD_DIR%\Release\PropertyManager.exe" "dist\" >nul
echo dist\PropertyManager.exe  (copy the database folder together with its attachments\ folder and -wal/-shm files)

echo.
echo --- SUCCESS ---
endlocal
