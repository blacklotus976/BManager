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
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

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
)

add_executable(PropertyManager WIN32 ${SOURCES})

target_link_libraries(PropertyManager PRIVATE
    sqlite3_vendored
    imgui::imgui
    glfw
    OpenGL::GL
)

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
