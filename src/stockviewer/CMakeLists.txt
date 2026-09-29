cmake_minimum_required(VERSION 3.15)
project(StockViewer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
    # Static CRT: the exe carries its own C++ runtime. No MSVCP140.dll,
    # no VCRUNTIME140.dll, no VC++ redistributable needed on the target.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX _UNICODE UNICODE)
endif()

# Git clones go here, OUTSIDE build\, so wiping build\ doesn't force a
# re-clone every time. First build clones 3 repos (~10MB total); after
# that, rebuilds reuse them.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/_deps" CACHE PATH "" FORCE)
include(FetchContent)

# --- GLFW (has native CMake support) ---
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glfw)

# --- nlohmann/json (header-only, has native CMake target) ---
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(json)

# --- Dear ImGui (no CMake of its own -- we build a small static lib) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

find_package(OpenGL REQUIRED)

# --- The app itself ---
set(SOURCES
    src/main.cpp
    src/HttpFetch.cpp
    src/Providers.cpp
    src/PluginLoader.cpp
    src/Persistence.cpp)

add_executable(StockViewer WIN32 ${SOURCES})
target_include_directories(StockViewer PRIVATE src)
target_link_libraries(StockViewer PRIVATE
    imgui
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json)

if(WIN32)
    # /SUBSYSTEM:WINDOWS   -> GUI app, no console window
    # /ENTRY:mainCRTStartup -> use main() as entry, not WinMain()
    # Both are needed because main.cpp defines main(), not WinMain().
    set_target_properties(StockViewer PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()