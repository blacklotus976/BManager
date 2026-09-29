#pragma once
#include <string>
#include <vector>
#include <cstdint>

// One saved stock config. Everything needed to reconstruct a chart on
// startup without asking the user for anything again.
struct SavedStock {
    std::string providerId;
    std::string ticker;
    double      entryPrice   = 0.0;
    double      feePercent   = 0.25;
    double      targetRoiPct = 10.0;
    std::string timeframeId;      // "" = provider default
    bool        visible      = true;
    int64_t     addedAtMs    = 0;
};

struct SavedState {
    int  version   = 1;
    int  activeTab = 0;
    std::vector<SavedStock> stocks;
    // Per-provider API keys, keyed by provider id.
    std::vector<std::pair<std::string, std::string>> apiKeys;
};

// "stockviewer_state.json" next to the exe.
std::string DefaultStatePath();

bool LoadState(const std::string& path, SavedState& out);
bool SaveState(const std::string& path, const SavedState& s);