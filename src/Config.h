#pragma once
#include <string>

// Persists the configured DB path to config.ini next to the exe. Declared here
// so both main.cpp (reads it at startup) and the Settings UI (writes it when
// the user picks a new location) share one implementation.
void WriteConfiguredDbPath(const std::string& path);
// Settings: switch between the full database and the empty one (empty.db, same schema) -- writes config.ini and starts the app again;
// the caller then closes this instance. Returns false when the new instance could not be started.
bool SwitchDatabaseAndRelaunch(const std::string& currentDbPath);
