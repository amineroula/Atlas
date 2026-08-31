# Morphis Atlas

Morphis Atlas is a non-destructive digital asset operating system and desktop file
manager for Windows creative workflows. Atlas observes originals for indexing and
preview generation; automatic services write only to the Atlas catalog and cache.
Explicit file operations run only after a user action.

## Atlas 0.1 foundation

- Explorer tabs with back/forward/up history, address navigation, one-click folder
  tree navigation, visible bookmarks, generated image thumbnails, grid/details views,
  a persistent 64–256 px thumbnail-size control, instant folder filters, dual panes,
  drag-out, and persisted sessions.
- Every explorer tab owns and restores its independent primary folder, optional second
  split folder, and split-enabled state.
- Background job queue with progress, cancellation primitives, failure reporting,
  bounded workers, visible current paths, live file/folder counts, and a bounded scanner
  activity feed.
- Persistent folder and drive scans with SQLite-backed directory checkpoints. Every
  completed directory and discovered asset is committed atomically, so a scan can be
  stopped, Atlas can be closed, and the same scan can resume from its saved queue.
- A Saved scans panel records each root, state, exact file/folder/byte totals, pending
  directories, update time, and errors. Saved discoveries remain available while a
  scan is stopped or incomplete.
- **Save scan as** creates a named, immutable snapshot of the assets discovered at that
  moment without copying originals. Live scans may continue afterward without changing
  the snapshot. Scan-data and organizer versions are tracked separately, so newer Atlas
  releases can re-run improved material and asset organization against old snapshots
  without traversing the drive again.
- Indexed filename/path, extension, size, and date search APIs plus a global search UI.
- Disk-backed, modification-aware image thumbnail cache and large preview panel. The
  cache root is selectable from **Settings > Choose cache folder** and remains the
  default after Atlas restarts; generated grid thumbnails and previews use separate
  subfolders.
- Conventional Copy/Cut/Paste-here and explicit Copy-here/Move-here drag-and-drop,
  plus rename, duplicate, folder creation, and Recycle Bin deletion. Originals are
  never changed by scanning, search, preview, or analysis.
- Preview-to-bookmark transfer: drag the preview (including a multi-selection) onto a
  bookmarked folder, then explicitly choose safe move-after-success or copy-and-keep.
- Cancellable current-folder and current-drive scans with exact extension-based totals
  for images, 3D models, videos, audio, archives, documents, and other files, plus drive
  usage, largest files/folders, exact content duplicates, and empty folders.
- A Drive Intelligence results workspace with count/size percentages, a largest-file
  heatmap, exact per-extension diagnostics, direct FBX and archive inventories, and
  double-click navigation to every reported location.
- Read-only PBR material discovery groups texture maps by folder and material name,
  recognizes Base Color, Normal, Roughness, Metallic, AO, Height, Opacity, Emissive,
  Specular, and packed ORM/RMA maps, and distinguishes core-complete sets from partial
  candidates without moving or renaming source textures.
- Saved-scan organization deliberately presents PBR materials first, followed by 3D
  files, images, video, archives, audio, documents, and other files. PBR groups report
  core-complete versus incomplete candidate status, and result navigation never moves
  source assets.
- PBR identity is inferred from normalized filename prefixes inside collection folders,
  while dedicated folders are used as a fallback only when multiple distinct map roles
  provide evidence. Exporter noise, resolution markers, map suffixes, numbered variants,
  common misspellings, and legacy Specular/Glossiness naming are normalized without
  merging similarly named material parts such as `leaf1` and `leaf5`.
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

Run `build/src/AtlasUI/atlas.exe`. The SQLite catalog remains in the platform-local
application data directory. Generated thumbnails and previews use the platform cache
directory by default, or the persistent custom folder selected in Settings. Atlas
never puts cache files alongside indexed assets unless that location is explicitly
selected by the user.

To produce an installable package, install NSIS and run `cpack --config build/CPackConfig.cmake`.

## Headless build

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DATLAS_BUILD_DESKTOP=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

See [docs/architecture.md](docs/architecture.md) for module boundaries and invariants.
