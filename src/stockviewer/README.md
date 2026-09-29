# StockViewer

A single-file, fully-static Windows desktop app for tracking stocks, crypto,
and forex charts with fee-aware ROI, break-even, and target-price overlays.

**One exe. No DLLs. No installer. No registry footprint.** Copy
`StockViewer.exe` to any Windows 10/11 machine and it runs. First save creates
`stockviewer_state.json` next to the exe; that file travels with the exe. No
`%AppData%`, no `C:\ProgramData`, nothing outside the exe's own folder.

## Building

Requires:

- Visual Studio 2022 (Community or higher) with the C++ desktop workload
- vcpkg checkout — either set `VCPKG_ROOT` or place it at
  `%USERPROFILE%\vcpkg` (the script also checks
  `%USERPROFILE%\PycharmProjects\LEPP\AUDIO_ENGINE_CPP\vcpkg`)

Then run:
build_stockviewer.bat


What it does:

1. `vcpkg install` on the static-triplet dependencies (imgui, glfw3,
   nlohmann-json). Idempotent — already-installed packages are skipped.
2. CMake configure with the static CRT and static triplet.
3. Build `StockViewer.exe`.
4. `dependency_report.ps1` reads the exe's PE import table and confirms
   every DLL it asks for is a core Windows component.
5. Stages a shareable copy at `dist\StockViewer.exe`.

## Configuration

`stockviewer_state.json` lives **next to the exe**. On first save it's
created from scratch. If it's missing, the app starts fresh (silent — no
crash, no prompt). If it's malformed, the app ignores it and starts fresh.
Delete it to reset everything. Copy it alongside the exe to carry your
settings to another machine.

There is no other persistent state.

## Providers

| Provider | Data | Timeframes | Symbol discovery |
|---|---|---|---|
| **Naftemporiki** | Greek stocks (ASEX), daily EOD | Daily only | Curated ~30-name table + best-effort scrape of guessed endpoints |
| **biquote** | Forex / metals / commodities, live | 1m … 1d | Fetched from upstream on first use |
| **Binance** | Crypto, live | 1m … 1w | Fetched from upstream on first use |

## Future updates (planned)

### 1. Full Naftemporiki symbol list

The endpoint `GetHistoryMetastock.aspx` has no public symbol-discovery API.
Today the app ships a curated ~30-name table (`kNaSym` in `Providers.cpp`)
plus a best-effort scrape of four guessed endpoints. To get complete
coverage:

- Inspect the Network tab on the Naftemporiki MetaStock download page,
  capture the real list URL, and wire it into `TryScrapeNaftemporikiSymbols()`.
- Or scrape the ATHEX index-composition page once per session and cache it.
- Or ship a hand-maintained table of all ~150 tradable ASEX tickers.

Unlisted symbols still work when their Latin code matches the Greek name
the endpoint expects (fallback path in `NaftemporikiProvider::BuildUrl`).

### 2. "Show more" paginated history

Scrolling back currently triggers a silent 500-bar fetch every time the
slider nears the oldest edge, until the provider is exhausted. Nicer UX:
a visible **"Load more"** button under the chart that appears only when
the viewport is at the oldest edge, so the user decides when to fetch.
Also useful for providers with deep history (Binance serves years of 1m
bars; the current 1500-bar buffer holds ~1 day of those).

### 3. Lighter, faster, more responsive

- **Bar cache keyed by `(symbol, timeframe)`** — switching timeframes
  currently refetches everything. A small LRU makes alt-tabbing instant.
- **Batched quote fetches via a small worker pool** — the poller spawns
  one OS thread per quote refresh per symbol. Fine at 10 symbols; noisy
  at 100+. A 4–8 thread pool with a task queue caps thread churn.
- **Bake dense charts to a texture** — currently every candle is drawn
  via `ImDrawList` on the UI thread each frame. For 200+ visible candles,
  rendering to a texture and only re-uploading when data or viewport
  changes cuts CPU significantly.
- **Cache fetched symbol lists on disk** (`exchangeInfo`, `/api/symbols`)
  with a ~24h TTL, so startup works offline and the first Add-modal open
  is instant.

### 4. Generic improvements

- Text filter over the loaded symbol list in the Add modal (rather than
  re-querying the provider on every keystroke).
- Per-stock free-text notes persisted alongside entry / fee / target.
- CSV export of currently-visible bars.
- Dark / light theme toggle (dark-only today).
- Safer plugin loader (separate process + IPC) for third-party DLLs.
- i18n: wrap hardcoded Greek UI strings in a lookup table.

## File layout


src/stockviewer/
CMakeLists.txt build config (static CRT, static vcpkg triplet)
build_stockviewer.bat one-click build (finds vcpkg, installs deps, builds)
dependency_report.ps1 reads the built exe's PE header, reports any
non-system DLL it needs (should be zero)
README.md this file
src/
main.cpp UI, chart drawing, poller, add-stock modal
HttpFetch.cpp/.h WinHTTP wrapper
Providers.cpp/.h Naftemporiki / biquote / Binance providers
Persistence.cpp/.h stockviewer_state.json read/write next to the exe
PluginLoader.cpp/.h optional plugins/ folder scanned for DLLs
RequestStats.h per-provider rolling request / latency meters
IMarketDataProvider.h stable plugin ABI (treat as frozen)
build/ generated by build_stockviewer.bat (gitignore)
dist/ staged, ready-to-share exe (gitignore)
plugins/ optional third-party provider DLLs



## Why no DLLs?

The build compiles with the **static CRT** (`/MT`) and links every
dependency — imgui, glfw3, nlohmann-json, OpenGL — statically. Every C++
runtime symbol, every imgui/glfw function, every string utility is copied
into the single exe. The only DLLs the exe references at launch are core
Windows components (`KERNEL32`, `USER32`, `WINHTTP`, `SHELL32`, `OPENGL32`,
etc.) present on every Windows install since XP.

`dependency_report.ps1` proves this by reading the import table directly
from the PE header (not by observing one run). If it ever flags a
`[CHECK]` entry, that's a real problem — fix the build before shipping.


How it works, briefly
CMakeLists.txt — the CMake config. add_compile_options(/utf-8) and
CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded are the two things that turn a
default dynamic build into a static one. The VCPKG_TARGET_TRIPLET default
makes hand-runs of cmake also produce a static exe.

build_stockviewer.bat — a wrapper that locates vcpkg, installs the
static-triplet packages with the correct imgui backend features, configures,
builds, runs the report, then stages dist\StockViewer.exe. Uses
%~dp0 so it always operates from its own directory, and %USERPROFILE% /
%VCPKG_ROOT% so it has no hardcoded paths.

dependency_report.ps1 — reads the built exe's import table and checks
each entry against a known list of core Windows DLLs. If it prints zero
[CHECK] lines (which it should), the exe is autonomous. If it ever doesn't,
that's the exact signal that something in the build regressed from static to
dynamic.

README.md — documents the build, the config file behaviour, the
providers, and the four future-work buckets you asked for: full
Naftemporiki symbol list, "Load more" paginated history, lighter/faster
work, and generic improvements.