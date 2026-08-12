# Morphis Atlas

Morphis Atlas is a non-destructive digital asset operating system and desktop file
manager for Windows creative workflows. Atlas observes originals for indexing and
preview generation; automatic services write only to the Atlas catalog and cache.
Explicit file operations run only after a user action.

## Atlas 0.1 foundation

- Explorer tabs with back/forward/up history, address navigation, folder tree,
  bookmarks, grid/details views, instant folder filters, dual panes, drag-out, and
  persisted sessions.
- Background job queue with progress, cancellation primitives, failure reporting,
  and bounded workers.
- Recursive cancellable indexing with batched SQLite WAL writes, stale-entry
  reconciliation, stable IDs, schema migration, and filesystem change refresh.
- Indexed filename/path, extension, size, and date search APIs plus a global search UI.
- Disk-backed, modification-aware image thumbnail cache and large preview panel.
- Conflict-aware copy/move, rename, duplicate, folder creation, and Recycle Bin
  deletion. Originals are never changed by scanning, search, preview, or analysis.
- Drive usage, largest files/folders, exact content duplicates, empty-folder analysis,
  and catalog statistics.
- ZIP/7Z creation and ZIP/7Z/RAR extraction through an installed `7z` executable.
- Structured timestamped logs, restoreable window layouts, automated tests, CI, and
  CPack installer configuration.

## Windows build

The project requires CMake 3.24+, C++20, SQLite, and Qt 6.5+ for the desktop app.
For the Qt Online Installer's MinGW kit used in this workspace:

```powershell
$env:PATH = 'D:\QT\Tools\mingw1310_64\bin;D:\QT\Tools\Ninja;' + $env:PATH
cmake -S . -B build -G Ninja `
  -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_PREFIX_PATH=D:/QT/6.11.1/mingw_64 `
  -DSQLite3_INCLUDE_DIR=D:/QT/Tools/mingw1310_64/opt/include `
  -DSQLite3_LIBRARY=D:/QT/Tools/mingw1310_64/opt/lib/libsqlite3.a `
  -DBUILD_TESTING=ON -DATLAS_BUILD_DESKTOP=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run `build/src/AtlasUI/atlas.exe`. Atlas data is stored under the platform-local
application data and cache directories, never alongside indexed assets.

To produce an installable package, install NSIS and run `cpack --config build/CPackConfig.cmake`.

## Headless build

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DATLAS_BUILD_DESKTOP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

See [docs/architecture.md](docs/architecture.md) for module boundaries and invariants.
