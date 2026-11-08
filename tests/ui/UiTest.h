// UI test driver: plays a script against the running app with synthetic ImGui input (never the real mouse, keyboard or
// screen), finds widgets by their label through Dear ImGui's test-engine hooks, checks the database, saves its own screenshots.
// Built only into the test copy (BM_UI_TEST + IMGUI_ENABLE_TEST_ENGINE, imgui compiled from source); see tests/ui/README.md.
#pragma once
#include <string>
struct GLFWwindow;
struct ImVec2;
namespace uitest {
bool Active();                                          // BM_UI_SCRIPT is set
void BeforeWindow();                                    // window hints: fixed size, never takes the focus
void Init(GLFWwindow* w, const std::string& dbPath);    // after the ImGui backends: real input disconnected
void NewFrame(GLFWwindow* w);                           // instead of ImGui_ImplGlfw_NewFrame: size, fixed time step, the script's input
void AfterRender(GLFWwindow* w);                        // screenshots of the frame just drawn
void Note(const char* label, const ImVec2& min, const ImVec2& max);   // a clickable thing that is not an ImGui item (links)
}
