#pragma once
#include <string>
class DataConn;

// Entry point for the ImGui shell. Owns the GLFW/OpenGL window and the main loop.
int RunApp(DataConn* conn, const std::string& dbPath);
