#pragma once
#include <string>

// Persists the configured DB path to config.ini next to the exe. Declared here
// so both main.cpp (reads it at startup) and the Settings UI (writes it when
// the user picks a new location) share one implementation.
void WriteConfiguredDbPath(const std::string& path);
