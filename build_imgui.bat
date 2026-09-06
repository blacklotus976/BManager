@echo off
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VCPKG_TOOLCHAIN=C:/Users/james/PycharmProjects/LEPP/AUDIO_ENGINE_CPP/vcpkg/scripts/buildsystems/vcpkg.cmake"
set "BUILD_DIR=build"
set "CXX="
set "CC="

echo.
echo --- 1. Configuring (ImGui/GLFW via shared LEPP vcpkg) ---
"%CMAKE%" -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -T "version=14.44" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DVCPKG_TARGET_TRIPLET=x64-windows
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. & exit /b 1 )

echo.
echo --- 2. Building Release (PropertyManager) ---
"%CMAKE%" --build %BUILD_DIR% --config Release --target PropertyManager -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- 2b. Building Release (StockViewer) ---
"%CMAKE%" --build %BUILD_DIR% --config Release --target StockViewer -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- 3. Deploying runtime DLLs next to the exes ---
set "VCPKG_BIN=C:\Users\james\PycharmProjects\LEPP\AUDIO_ENGINE_CPP\vcpkg\installed\x64-windows\bin"
if exist "%VCPKG_BIN%\glfw3.dll" copy /Y "%VCPKG_BIN%\glfw3.dll" "build\Release\"
set "MSVC_REDIST_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\v143\x64\Microsoft.VC143.CRT"
if exist "%MSVC_REDIST_DIR%" (
    copy /Y "%MSVC_REDIST_DIR%\msvcp140*.dll" "build\Release\"
    copy /Y "%MSVC_REDIST_DIR%\vcruntime140*.dll" "build\Release\"
    copy /Y "%MSVC_REDIST_DIR%\concrt140.dll" "build\Release\"
)

echo.
echo --- SUCCESS ---
