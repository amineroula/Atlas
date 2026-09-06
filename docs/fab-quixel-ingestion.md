# Fab / Quixel ingestion in Atlas

## Ownership

Atlas is the source of truth for external asset providers. Morphis consumes the Atlas library and does not maintain a second Quixel database.

Pipeline:

`Fab / Quixel -> Atlas ingest -> Atlas Asset Library -> Morphis`

## Supported acquisition boundary

Atlas must use supported Epic/Fab workflows. It must not depend on undocumented Epic authentication or private download APIs.

The first provider transport is Fab in Epic Games Launcher using its Custom Socket export target. The initial local receiver port is `13428` on loopback only.

## Ingestion stages

1. Receive the Fab export JSON payload.
2. Validate JSON and discover exported asset files/directories.
3. Resolve the Quixel/Fab vendor id, display name, type, tags, preview, meshes, LODs, textures, map roles and resolutions when present.
4. Normalize the asset into Atlas' existing `QuixelAsset`/catalog representation.
5. Register the asset in Atlas storage/indexing.
6. Generate or select a thumbnail/preview.
7. Publish the normalized asset through `Morphis/Atlas/library-v1.json` using stable `atlas://quixel/<vendor-id>` identifiers.
8. Morphis refreshes its Atlas Browser and imports the selected Atlas asset into the scene.

## Storage rule

Downloaded source files remain in the configured Atlas asset-library storage. The manifest stores references and metadata; it does not duplicate large meshes or textures.

Atlas should preserve provider metadata alongside normalized Atlas metadata so assets can be re-indexed without losing their original identity.

Suggested provider metadata directory:

`<Atlas Library>/<asset>/metadata/fab-export.json`

The current Morphis/Atlas bridge manifest remains:

`QStandardPaths::GenericDataLocation/Morphis/Atlas/library-v1.json`

## Stable identity

Quixel assets use:

- asset: `atlas://quixel/<vendor-id>`
- default material: `atlas://quixel/<vendor-id>/material/default`

The vendor id is the primary deduplication key. Re-exporting the same asset should update/reconcile the existing library entry instead of creating another logical asset.

## Provider architecture

Fab/Quixel is the first provider, not a special-case library. Future providers should pass through the same ingestion boundary:

- Fab / Quixel
- Poly Haven
- local scans
- Formative assets
- user models/materials/HDRIs

Provider-specific code acquires and parses data. Atlas owns normalization, indexing, caching, previews and persistence.

## Morphis responsibilities

Morphis owns only the consumption UX:

- search/filter Atlas assets
- show thumbnails and metadata
- request preferred LOD/resolution
- drag/drop or Add to Scene
- create geometry/material nodes
- place the asset in the viewport

Morphis must not own Fab credentials, duplicate Atlas indexing, or copy the entire asset catalog into a second database.

## Next implementation milestone

Implement `FabIngestService` in Atlas. It should receive Custom Socket payloads, persist the original payload, normalize exported files into Atlas assets, trigger an incremental index update, and republish the Atlas library manifest. Keep the service independent of the UI so Atlas can later run ingestion in a background worker.