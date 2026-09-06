cmake_minimum_required(VERSION 3.15)
project(PropertyManager VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---- Dear ImGui + GLFW + OpenGL3 via vcpkg (reusing the LEPP vcpkg tree -
#      disk is constrained on this machine, don't stand up a second vcpkg) ----
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

add_custom_command(TARGET PropertyManager POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/create_tables.sql"
        "$<TARGET_FILE_DIR:PropertyManager>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:glfw>
        "$<TARGET_FILE_DIR:PropertyManager>"
)

# ---- StockViewer: second, independent executable (own window/process) ----
# Quick Greek-stocks ROI viewer. Built from the same CMakeLists/vcpkg tree
# as PropertyManager -- one `cmake --build` produces both exes.
add_executable(StockViewer WIN32
    src/stockviewer/main.cpp
    src/stockviewer/HttpFetch.cpp
)

target_link_libraries(StockViewer PRIVATE
    imgui::imgui
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json
    winhttp
)

if(WIN32)
    target_compile_definitions(StockViewer PRIVATE _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_target_properties(StockViewer PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

add_custom_command(TARGET StockViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:glfw>
        "$<TARGET_FILE_DIR:StockViewer>"
)
