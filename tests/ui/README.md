# UI tests (PM_uitest)

A copy of the app that plays a script: it clicks, types and drags through the real screens with **synthetic ImGui input**
(the real mouse, keyboard and screen are never touched, the window never takes the focus), checks the database after each
step and saves its own screenshots.

- `UiTest.h / UiTest.cpp`: the driver. It finds widgets by their label through Dear ImGui's test-engine hooks, so the copy
  compiles ImGui from source with `IMGUI_ENABLE_TEST_ENGINE`; the app's code is the same, its hooks are behind `BM_UI_TEST`.
- `CMakeLists.txt`: a project of its own for the copy (the app's build is not touched). `IMGUI_SRC` = the ImGui source tree
  of the same version vcpkg installed (`<vcpkg>/buildtrees/imgui/src/<version>.clean`).
- `run.sh <PM_uitest.exe> <script> <work folder> [fresh|demo]`: one script in its own folder; `fresh` starts from a copy of
  `release_db/empty.db`, `demo` from a copy of `release_db/data.db`. Without either the database goes on from the previous
  script, so the scripts run in order. Excel exports skip the Save As dialog (`BM_UI_EXPORT_DIR`) and are saved in the
  script's `out/` folder, next to the screenshots, to be opened and checked.
- `scripts/`: an empty database → a building client → managed → apartments, residents → its partners → the fund and its
  limit (01); monthly costs owed to partners, shortcuts, a resident's payment, paying the cost, the money sections (02);
  services, a package, clients, charges, a contract, a list column dragged, the cube's three tabs, the reversed view and a
  flatten (03); every way a charge gets settled, with the date pickers (a day + OK, the calendar's «Σήμερα», the receipt's
  «Σήμερα»), a part payment, a contract installment, a charge paid at once with its cost, paying the partner with his
  invoice (04); Excel exports of lists, a building's partners, the cube's tables and a client's usage panel (05).
  `90_demo_cube.txt` runs on the demo data (`demo`; give `BM_TEST_CUBE=364,-650,400,0` so the cube opens on a whole year):
  packages and contracts by month, the layer slider updating the tables, the reversed view, the cube's export, and a long
  list's export with the «all pages / only this page» question.

## Script steps
One per line, `#` = comment:

| step | |
|---|---|
| `click / dclick / rclick / hover <target>` | the widget's centre |
| `field <target> = <text>` | click, select all, type, Enter |
| `type <text>`, `key <Enter|Tab|Escape|...>`, `clear` | into the active field |
| `drag <target> => dx,dy`, `dragto <target> => <target>` | press, move in steps, release |
| `press <target>`, `moveby dx,dy[,frames]`, `release` | a drag in pieces (screenshots in the middle) |
| `scrollto <target>` | mouse wheel over the nearest window that scrolls (give `win=` when the target is not drawn yet) |
| `waitfor / expect / expectno <target>`, `wait <frames>` | |
| `check <SQL> => <expected first value>` | read-only connection to the test database |
| `shot <name>`, `log <text>`, `list <target>` (every match with its place), `quit` | |
| `vtx <n>` | fails if a window's draw list has `n` vertices or more (the cube must stay under 65,536) |

`<target>`: the label (the part before `##`, or the whole label such as `##bpn`), `~text` = contains; options after ` | `:
`win=<part of the window name>`, `n=<which match on the page, in drawing order>`. `{d}`, `{ym}`, `{ymd}` = today.
A target that does not show up (or stays covered by another window / behind a modal) fails after a few seconds with a
screenshot and the reason; the script goes on. Results: `uitest.log` and the screenshots in `out/<script>/`.

Combos do not tell ImGui their label: the app registers the ones the scripts need with `UI_NOTE` (test builds only), and links
(`LinkCell`) register themselves.
