# The UI test copy of BManager (PM_uitest): the app's own sources, Dear ImGui compiled here from source with its test-engine
# hooks on, and the script driver (UiTest.cpp). A separate project: the app's own build is not touched.
#   cmake -S tests/ui -B <dir> -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#         -DVCPKG_TARGET_TRIPLET=x64-windows-static -DIMGUI_SRC=<the Dear ImGui source tree of the version vcpkg installed>
#   cmake --build <dir> --config Release
# IMGUI_SRC must be the same version as the installed headers (vcpkg keeps it in buildtrees/imgui/src/<version>.clean).
cmake_minimum_required(VERSION 3.15)
project(bm_uitest C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /bigobj)
endif()
get_filename_component(BM "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT IMGUI_SRC)
    message(FATAL_ERROR "Set -DIMGUI_SRC=<Dear ImGui source tree> (the same version as the installed imgui)")
endif()
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
add_executable(PM_uitest WIN32
    ${BM}/src/main.cpp ${BM}/src/SqliteConn.cpp ${BM}/src/Seed.cpp ${BM}/src/ui/App.cpp ${BM}/src/ui/DatePicker.cpp
    ${BM}/src/ui/MediaViewer.cpp ${BM}/src/ui/StbImage.cpp ${BM}/third_party/sqlite3/sqlite3.c ${BM}/tests/ui/UiTest.cpp
    ${IMGUI_SRC}/imgui.cpp ${IMGUI_SRC}/imgui_draw.cpp ${IMGUI_SRC}/imgui_tables.cpp ${IMGUI_SRC}/imgui_widgets.cpp ${IMGUI_SRC}/imgui_demo.cpp
    ${IMGUI_SRC}/backends/imgui_impl_glfw.cpp ${IMGUI_SRC}/backends/imgui_impl_opengl3.cpp)
target_include_directories(PM_uitest PRIVATE ${IMGUI_SRC} ${IMGUI_SRC}/backends ${BM}/src ${BM}/src/models ${BM}/src/ui
    ${BM}/third_party/sqlite3 ${BM}/third_party/stb ${BM}/tests/ui)
target_compile_definitions(PM_uitest PRIVATE IMGUI_ENABLE_TEST_ENGINE BM_UI_TEST)
if(WIN32)
    target_compile_definitions(PM_uitest PRIVATE _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_target_properties(PM_uitest PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()
target_link_libraries(PM_uitest PRIVATE glfw OpenGL::GL)
