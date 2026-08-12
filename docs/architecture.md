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

## Catalog invariants

- Normalized paths are unique. Windows paths are compared in normalized lower case.
- Rescanning refreshes metadata without changing the asset ID.
- Bulk discoveries commit transactionally in bounded batches.
- A successful full scan removes catalog entries no longer observed below its root.
- `PRAGMA user_version` records schema version 2; version 1 catalogs migrate in place.
- Foreign keys, WAL, normal synchronous writes, and a busy timeout are configured.

## Safety boundaries

- Scanner, search, preview, thumbnail generation, and statistics never write originals.
- Deletion uses the operating system Recycle Bin through Qt.
- Name conflicts are resolved before transfers; the service supports skip, overwrite,
  and collision-free rename policies.
- Archive operations are delegated to the installed 7-Zip executable and report errors
  through the background job surface.

## Scale strategy

The current catalog uses indexed SQLite queries and batched writes. UI directory views
are lazy through `QFileSystemModel`; previews are generated on demand and keyed by path,
size, modification time, and requested dimensions. Later releases can replace the
preview decoder and watcher backend without changing asset identity or catalog APIs.
