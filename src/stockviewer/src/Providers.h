#pragma once
#include "IMarketDataProvider.h"
#include <memory>
#include <vector>

std::unique_ptr<IMarketDataProvider> MakeBiquoteForexProvider();
std::unique_ptr<IMarketDataProvider> MakeBinanceCryptoProvider();
std::unique_ptr<IMarketDataProvider> MakeNaftemporikiProvider();

std::vector<std::unique_ptr<IMarketDataProvider>> MakeAllProviders();