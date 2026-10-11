# BManager — PropertyManager

Desktop management app for a building-management / home-services business: clients, buildings & apartments,
services & packages, bills (charges), payments, receipts, attachments, reports. Dear ImGui + GLFW + SQLite,
single static executable.

## Build — Windows (Visual Studio 2022 + vcpkg)

```bat
set VCPKG_ROOT=C:\path\to\vcpkg          :: once, or edit the default inside the script
build_imgui.bat                          :: app + tests -> dist\PropertyManager.exe
build_imgui.bat video                    :: also installs FFmpeg for the in-app video player
```
If CMake complains about the toolset: `set MSVC_TOOLSET=none` and run again.

## Build — Linux

```bash
sudo apt install cmake g++ libglfw3-dev libgl-dev
sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev   # optional: in-app video
./build_linux.sh                                                               # app + tests
./build-linux/PropertyManager
```

## Data

- `data.db` (SQLite, WAL mode) next to the executable, plus `data.db-wal` / `data.db-shm` while it runs.
- `attachments/` next to the database: attached files, named by their SHA-256.
- `backups/`: a daily copy of the database and the audit-log checkpoints.
- Move or back up the database **with** its `attachments/` folder (close the app first, or copy the -wal/-shm too).

## Docs

- `docs/ARCHITECTURE_REVIEW.md` — data model, integrity rules, roadmap
- `docs/UI_PROPOSALS.md` — edit history / receipts UI, search at 70k clients
- `docs/PROMOTIONS_DESIGN.md` — contracts, promotions and offers (design)
- `docs/JOINT_CHARGES.md` — Κοινόχρηστα: costs split across a building's apartments; error log
