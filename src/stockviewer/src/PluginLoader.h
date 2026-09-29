#pragma once
#include "IMarketDataProvider.h"
#include <string>
#include <vector>
#include <memory>

// Scans `dir` for *.dll, loads each with LoadLibrary, verifies the plugin
// exports StockViewer_PluginApiVersion() == "1", and calls
// StockViewer_CreateProvider(). Returns the loaded providers plus a human
// log of what was accepted/rejected. The DLLs are kept loaded for the
// lifetime of the process -- call UnloadAll() at shutdown if you care about
// tidy cleanup (Windows will do it for you anyway).
std::vector<std::unique_ptr<IMarketDataProvider>>
LoadPluginsFrom(const std::string& dir, std::vector<std::string>& outLog);

void UnloadAllPlugins();