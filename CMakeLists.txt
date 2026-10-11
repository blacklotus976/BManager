cmake_minimum_required(VERSION 3.15)
project(PropertyManager VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
    # ---- Fully static MSVC runtime (/MT, not /MD) ----
    # This is the actual fix for "runs on any Windows PC with zero prerequisites":
    # dynamic linking (/MD, the default) means every exe needs MSVCP140.dll /
    # VCRUNTIME140.dll present on the target machine -- absent on a clean PC
    # that never had Visual Studio or the VC++ Redistributable installed, which
    # is exactly what broke for the client. Bundling those DLLs app-local (the
    # previous fix) works, but static linking removes the dependency entirely:
    # the runtime is compiled directly into the exe, nothing to ship or find.
    # Every vcpkg package used below (imgui/glfw3/nlohmann_json) is pulled from
    # the x64-windows-static triplet specifically so their .lib files match this
    # linkage -- mixing /MT app code against /MD-built static libs fails to link
    # (CRT mismatch), which is why this couldn't just be flipped without also
    # installing the static-triplet packages first.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- Dear ImGui + GLFW + OpenGL3 + nlohmann_json, all from the x64-windows-static
#      triplet of the shared LEPP vcpkg tree (installed alongside the pre-existing
#      dynamic-triplet copies -- see the note above on why the triplet matters). ----
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static")
endif()
if(WIN32)
    find_package(imgui CONFIG REQUIRED)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
else()
    # Linux/macOS: system GLFW (libglfw3-dev) + Dear ImGui built from a source
    # checkout pointed to by IMGUI_DIR (see build_linux.sh).
    find_package(glfw3 REQUIRED)
    if(NOT IMGUI_DIR)
        message(FATAL_ERROR "Set -DIMGUI_DIR=<path to a Dear ImGui checkout>")
    endif()
    add_library(imgui_src STATIC
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui_src PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(imgui_src PUBLIC glfw)
    add_library(imgui::imgui ALIAS imgui_src)
endif()
find_package(OpenGL REQUIRED)

# ---- Vendored sqlite3 amalgamation (no vcpkg) ----
add_library(sqlite3_vendored STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3_vendored PUBLIC third_party/sqlite3)
if(WIN32)
    target_compile_definitions(sqlite3_vendored PRIVATE SQLITE_API=)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/models)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/ui)

set(SOURCES
    src/main.cpp
    src/SqliteConn.cpp
    src/Seed.cpp
    src/ui/App.cpp
    src/ui/DatePicker.cpp
    src/ui/MediaViewer.cpp
    src/ui/StbImage.cpp
)

add_executable(PropertyManager WIN32 ${SOURCES})

target_link_libraries(PropertyManager PRIVATE
    sqlite3_vendored
    imgui::imgui
    glfw
    OpenGL::GL
)
target_include_directories(PropertyManager PRIVATE third_party/stb)

# ---- Optional in-app video playback (FFmpeg). Without it the app still builds and opens videos with the
#      system's player. Linux: apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev.
#      Windows: vcpkg install ffmpeg:x64-windows-static (then find_package(FFMPEG) below finds it). ----
option(BMANAGER_VIDEO "In-app video playback with FFmpeg" ON)
if(BMANAGER_VIDEO)
    if(WIN32)
        find_package(FFMPEG QUIET)
        if(FFMPEG_FOUND)
            target_include_directories(PropertyManager PRIVATE ${FFMPEG_INCLUDE_DIRS})
            target_link_directories(PropertyManager PRIVATE ${FFMPEG_LIBRARY_DIRS})
            target_link_libraries(PropertyManager PRIVATE ${FFMPEG_LIBRARIES})
            target_compile_definitions(PropertyManager PRIVATE BMANAGER_VIDEO)
        endif()
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
            if(FFMPEG_FOUND)
                target_link_libraries(PropertyManager PRIVATE PkgConfig::FFMPEG)
                target_compile_definitions(PropertyManager PRIVATE BMANAGER_VIDEO)
            endif()
        endif()
    endif()
    if(FFMPEG_FOUND)
        message(STATUS "Video playback: FFmpeg found, in-app player enabled")
    else()
        message(STATUS "Video playback: FFmpeg not found, videos open in the system player")
    endif()
endif()

if(WIN32)
    target_compile_definitions(PropertyManager PRIVATE _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_target_properties(PropertyManager PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

# GLFW and the MSVC runtime are both statically linked in -- nothing external
# left to bundle at runtime. create_tables.sql is NOT read by the app (the real
# schema lives as inline CREATE TABLE statements in SqliteConn.cpp's constructor,
# has for a long time) -- it was a leftover from the project's very first draft,
# copied into every release for no functional reason. Correcting that here:
# nothing needs copying post-build anymore.

# ---- Unit tests (no GUI deps): cmake --build <dir> --target money_test && ctest --test-dir <dir> ----
enable_testing()
add_executable(money_test EXCLUDE_FROM_ALL tests/money_test.cpp)
target_include_directories(money_test PRIVATE src)
add_test(NAME money COMMAND money_test)

add_executable(receipts_test EXCLUDE_FROM_ALL tests/receipts_test.cpp src/SqliteConn.cpp)
target_link_libraries(receipts_test PRIVATE sqlite3_vendored)
add_test(NAME receipts COMMAND receipts_test)

add_executable(history_test EXCLUDE_FROM_ALL tests/history_test.cpp src/SqliteConn.cpp)
target_link_libraries(history_test PRIVATE sqlite3_vendored)
add_test(NAME history COMMAND history_test)

add_executable(audit_test EXCLUDE_FROM_ALL tests/audit_test.cpp src/SqliteConn.cpp)
target_link_libraries(audit_test PRIVATE sqlite3_vendored)
add_test(NAME audit COMMAND audit_test)

add_executable(attachments_test EXCLUDE_FROM_ALL tests/attachments_test.cpp src/SqliteConn.cpp)
target_link_libraries(attachments_test PRIVATE sqlite3_vendored)
add_test(NAME attachments COMMAND attachments_test)
