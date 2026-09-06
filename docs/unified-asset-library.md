# Unified Atlas Asset Library

Atlas is the persistent organizer; Morphis is the universal reader and preview renderer.

## Asset flow
1. Atlas scans source folders without moving or modifying originals.
2. Every discovered file is typed: Model3D, Material, Animation, VDB, Plant, HDRI, Decal, Image or Audio.
3. Morphis AssetIO inspects supported 3D/scene formats and returns normalized geometry/material/dependency metadata. The format adapter layer is extensible rather than hard-coded to FBX.
4. Atlas groups textures and dependencies with the owning asset.
5. Atlas asks the Morphis preview bridge for a standard preview set: hero/perspective, front, back, left, right and top. Preview renders are cached by asset fingerprint.
6. Atlas stores the thumbnails on the discovered asset record.
7. The user creates virtual folders and drags thumbnail selections into them. Originals stay where they are; folders store stable Atlas asset IDs.
8. Assets can be rated 0-5 without modifying source files.
9. Saving publishes the folder tree, ratings, metadata, dependencies and preview paths in the Atlas library manifest.
10. Morphis reads the same manifest and presents the exact Atlas folders as its asset library.

## Important separation
- Physical source folders are not the Atlas organization system.
- Atlas folders are virtual collections and can contain assets from any disk or provider.
- Provider is metadata only (Quixel, KitBash, local, scan, future providers).
- One asset can be referenced by multiple folders without duplication.
- Imported originals remain immutable.

## Morphis AssetIO contract
Morphis should expose one inspection/import contract with adapters for formats such as FBX, OBJ, glTF/GLB, USD, Alembic and later other geometry/scene formats. Separate adapters cover VDB, animation interchange and material/image sources. Atlas calls inspection/preview functionality; it does not implement a second 3D parser.

## Preview contract
Default model previews: perspective/hero, front, back, left, right, top. Materials use Morphis material preview geometry. Animations use a representative pose/frame plus optional motion preview later. VDB uses a volume preview. Preview jobs run in the background and are regenerated only when the source fingerprint changes.
