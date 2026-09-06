@echo off
set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=build"
set "CXX="
set "CC="

echo.
echo --- 1. Configuring (FetchContent will clone FLTK) ---
"%CMAKE%" -S . -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -T "version=14.44"
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. & exit /b 1 )

echo.
echo --- 2. Building Release ---
"%CMAKE%" --build %BUILD_DIR% --config Release --target PropertyManager -- /m
IF %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- SUCCESS ---
