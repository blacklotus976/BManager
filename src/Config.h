#pragma once
#include <string>

// Persists the configured DB path to config.ini next to the exe. Declared here
// so both main.cpp (reads it at startup) and the Settings UI (writes it when
// the user picks a new location) share one implementation.
void WriteConfiguredDbPath(const std::string& path);
// Settings: switch between the full database and the empty one (empty.db, same schema) -- writes config.ini and starts the app again;
// the caller then closes this instance. Returns false when the new instance could not be started.
// which: "main" (the full one), "empty" (empty.db) or "fruits" (fruits.db, the greengrocer's).
bool SwitchDatabaseAndRelaunch(const std::string& currentDbPath, const std::string& which);
// is this database file (empty.db, fruits.db) next to the program?
bool DatabaseFileExists(const std::string& name);
