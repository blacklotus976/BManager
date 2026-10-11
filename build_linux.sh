#!/usr/bin/env bash
# Linux build of PropertyManager (counterpart of build_imgui.bat).
# Needs: cmake, g++, libglfw3-dev, libgl-dev  (Debian/Ubuntu: apt install cmake g++ libglfw3-dev libgl-dev)
set -euo pipefail
cd "$(dirname "$0")"
IMGUI_DIR="${IMGUI_DIR:-third_party/imgui}"
IMGUI_TAG="${IMGUI_TAG:-v1.91.9}"
if [ ! -f "$IMGUI_DIR/imgui.cpp" ]; then
    echo "--- Fetching Dear ImGui $IMGUI_TAG into $IMGUI_DIR ---"
    git clone --depth 1 -b "$IMGUI_TAG" https://github.com/ocornut/imgui "$IMGUI_DIR"
fi
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DIMGUI_DIR="$(realpath "$IMGUI_DIR")"
cmake --build build-linux --target PropertyManager money_test receipts_test history_test audit_test -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
echo "--- Built: build-linux/PropertyManager ---"
