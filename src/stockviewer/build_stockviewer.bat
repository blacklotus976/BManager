@echo off
setlocal
cd /d "%~dp0"
set "CXX="
set "CC="
set "BUILD_DIR=build"

echo.
echo --- 1. Cleaning build directory (git clones are kept in _deps\) ---
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo.
echo --- 2. Configuring (first run clones glfw/imgui/json -- takes a minute) ---
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -T "version=14.44"
if %ERRORLEVEL% NEQ 0 ( echo [ERROR] CMake config failed. & exit /b 1 )

echo.
echo --- 3. Compiling StockViewer.exe ---
cmake --build "%BUILD_DIR%" --config Release --target StockViewer -- /m
if %ERRORLEVEL% NEQ 0 ( echo [ERROR] Build failed. & exit /b 1 )

echo.
echo --- 4. Dependency report (should be zero non-system DLLs) ---
powershell -NoProfile -ExecutionPolicy Bypass -File "dependency_report.ps1"

echo.
echo --- 5. Staging a shareable copy ---
if not exist "dist" mkdir "dist"
copy /Y "%BUILD_DIR%\Release\StockViewer.exe" "dist\" >nul

echo.
echo ========================================================
echo SUCCESS: dist\StockViewer.exe
echo Copy that one file anywhere. No DLLs, no installer.
echo ========================================================
endlocal