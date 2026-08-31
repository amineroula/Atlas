# Architecture

Atlas treats files as immutable external resources unless the user invokes an explicit
file operation. An asset record is a metadata snapshot keyed by a stable catalog ID.
Thumbnails, logs, settings, and future analysis live in Atlas-owned storage.

## Modules

- `AtlasCore`: framework-free asset types, path normalization, file operations, and
  storage analysis.
- `AtlasDatabase`: SQLite catalog, migrations, indexed queries, bookmarks, statistics,
  and scan reconciliation.
- `AtlasScanner`: recursive discovery and progress with cooperative cancellation.
- `AtlasJobs`: bounded `std::jthread` workers, snapshots, progress, pause/resume, and
  cancellation.
- `AtlasUI`: Qt widgets, filesystem presentation, cache-backed image previews,
  orchestration, settings, logs, and explicit user actions.

The dependency graph is acyclic:

`AtlasUI -> {AtlasCore, AtlasDatabase, AtlasScanner, AtlasJobs}`

`AtlasDatabase -> AtlasCore`

`AtlasScanner -> AtlasCore`

## Concurrency

- Filesystem traversal, storage analysis, archives, and bulk file transfers run as jobs.
- Widgets and their models remain on the Qt main thread.
- Each worker opens its own SQLite connection. Connections are never shared across
  threads; WAL and a busy timeout coordinate readers and writers.
- Cancellation uses `std::stop_token`. File operations cancel between individual items;
  an in-progress platform filesystem call is allowed to finish.
- Job observers marshal snapshots back to the UI through queued Qt invocations.
- Recursive scans publish periodic file/folder counts and their current path. The UI
  keeps only the most recent 250 activity messages to avoid unbounded memory use.
- Storage analysis derives category byte totals, extension inventories, largest-item
  rankings, FBX/archive findings, and PBR texture groups during the same read-only
  traversal. PBR grouping is filename- and folder-based diagnostics; it does not edit
  assets or claim to validate shader contents.
- PBR recognition is centralized in `AtlasCore`. It separates map-role detection from
  material identity, groups collection folders by a canonical filename prefix, falls
  back to a dedicated folder only with multi-role evidence, and recognizes modern
  Metal/Roughness, legacy Specular/Glossiness, and foliage/opacity workflows. Single
  orphan maps remain ordinary textures and do not inflate material-set totals.
- Resumable scans traverse one directory at a time. A directory's asset upserts,
  session links, newly discovered subdirectory queue entries, checkpoint, and counters
  share one SQLite transaction. Cancellation before that transaction leaves the
  directory pending, making resume deterministic across process restarts.

## Catalog invariants

- Normalized paths are unique. Windows paths are compared in normalized lower case.
- Rescanning refreshes metadata without changing the asset ID.
- Bulk discoveries commit transactionally in bounded batches.
- A successful full scan removes catalog entries no longer observed below its root.
- `PRAGMA user_version` records schema version 4; older catalogs migrate in place.
- Scan sessions retain their root, state, counters, errors, pending-directory queue,
  and links to catalog assets. The global asset ID remains stable across scan sessions.
- A named scan snapshot clones only catalog asset links in one SQLite transaction; it
  does not duplicate file metadata or source files. Snapshots have no traversal queue
  and remain immutable when the originating live scan resumes. `data_version` describes
  the saved scan representation, while `organization_version` records which inference
  rules most recently interpreted it. Reorganization updates the latter only.
- Foreign keys, WAL, normal synchronous writes, and a busy timeout are configured.

## Safety boundaries

- Scanner, search, preview, thumbnail generation, and statistics never write originals.
- The generated cache root is user-selectable and persisted in application settings.
  Grid thumbnails and large previews live in separate subdirectories. Changing the
  root invalidates only in-memory preview state; Atlas neither moves nor deletes the
  previous cache, while the durable SQLite catalog stays in application data.
- Deletion uses the operating system Recycle Bin through Qt.
- Name conflicts are resolved before transfers; the service supports skip, overwrite,
  and collision-free rename policies.
- Archive operations are delegated to the installed 7-Zip executable and report errors
  through the background job surface.

## Scale strategy

The current catalog uses indexed SQLite queries and batched writes. UI directory views
are lazy through `QFileSystemModel`; grid thumbnails and previews are generated on
demand off the UI thread and keyed by path, size, modification time, and requested
dimensions. In-memory grid icons are bounded. Later releases can replace the preview
decoder and watcher backend without changing asset identity or catalog APIs.
