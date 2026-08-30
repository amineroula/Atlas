# Atlas → Morphis Preview Bridge

This document is the integration contract between the Atlas development branch
(currently being developed with Claude) and Morphis. It deliberately avoids a
compile-time dependency between the two applications.

## Ownership boundary

Atlas owns catalog IDs, PBR map recognition, material grouping, user selection,
and cache policy. Morphis owns preview geometry, UV evaluation, PBR shading,
lights, camera, viewport interaction, and image rendering.

Atlas source assets are read-only. Morphis may write only the request's explicit
`output.image` and `output.result` locations.

## Process API

Interactive preview:

```powershell
morphis_studio.exe --atlas-preview "C:\AtlasCache\requests\42.json"
```

Headless cached render:

```powershell
morphis_studio.exe --atlas-preview "C:\AtlasCache\requests\42.json" `
  --headless --output "C:\AtlasCache\renders\42.png" `
  --result "C:\AtlasCache\renders\42.result.json"
```

Atlas should invoke this asynchronously with `QProcess`, capture standard error,
enforce a configurable timeout for headless work, and never block its UI thread.

## Preview request v1

```json
{
  "schema": "morphis.atlas-preview/1",
  "requestId": "atlas-material-42",
  "material": {
    "name": "Alien Metal",
    "workflow": "metal-roughness",
    "maps": {
      "baseColor": "D:/Assets/Alien/alien_basecolor.png",
      "normal": "D:/Assets/Alien/alien_normalgl.png",
      "roughness": "D:/Assets/Alien/alien_roughness.png",
      "metallic": "D:/Assets/Alien/alien_metallic.png",
      "ambientOcclusion": "D:/Assets/Alien/alien_ao.png",
      "height": "D:/Assets/Alien/alien_height.exr",
      "opacity": "D:/Assets/Alien/alien_opacity.png",
      "emissive": "D:/Assets/Alien/alien_emissive.png"
    },
    "parameters": {
      "baseColor": [1.0, 1.0, 1.0],
      "roughness": 0.5,
      "metallic": 0.0,
      "normalStrength": 1.0,
      "heightScale": 0.02,
      "opacity": 1.0,
      "emissiveStrength": 1.0,
      "uvScale": [1.0, 1.0],
      "uvOffset": [0.0, 0.0],
      "uvRotationDegrees": 0.0,
      "normalConvention": "opengl"
    }
  },
  "preview": {
    "geometry": "sphere",
    "model": null,
    "environment": null,
    "background": [0.035, 0.04, 0.05],
    "lights": {
      "preset": "studio-three-point",
      "keyIntensity": 5.0,
      "fillIntensity": 1.5,
      "rimIntensity": 3.0,
      "exposure": 0.0
    },
    "camera": {
      "orbitDegrees": 25.0,
      "elevationDegrees": 18.0,
      "distance": 3.2,
      "focalLengthMm": 55.0
    }
  },
  "output": {
    "width": 1024,
    "height": 1024,
    "transparent": false,
    "image": "C:/AtlasCache/renders/42.png",
    "result": "C:/AtlasCache/renders/42.result.json"
  }
}
```

All paths are absolute UTF-8 paths. Missing optional maps are valid. Unknown
fields must be ignored for forward compatibility. A request with an unknown
major schema version must fail clearly.

Supported preview geometry values for v1 are `sphere`, `plane`, `cube`,
`cylinder`, and `model`. When `geometry` is `model`, `preview.model` is required.

## Result v1

```json
{
  "schema": "morphis.atlas-preview-result/1",
  "requestId": "atlas-material-42",
  "status": "success",
  "image": "C:/AtlasCache/renders/42.png",
  "renderer": "morphis-preview-gl",
  "durationMs": 184,
  "warnings": [],
  "unsupportedMaps": []
}
```

Exit code `0` means success, `2` means an invalid request, `3` means missing or
unreadable assets, `4` means renderer initialization failure, and `5` means the
render failed. Morphis must attempt to write the result JSON for every failure
after it has successfully parsed the `--result` argument.

## Atlas implementation checklist (Claude handoff)

1. Add `MorphisPreviewRequest` and JSON serialization in `AtlasCore`; do not add
   Qt or Morphis dependencies to that module.
2. Map `PbrMapKind` to the exact v1 keys above. Convert Glossiness to a declared
   `glossiness` map rather than silently treating it as Roughness. Pass packed
   map channel meaning explicitly in a later compatible field.
3. Add a `MorphisPreviewClient` in `AtlasUI` using asynchronous `QProcess`.
4. Resolve the Morphis executable from Settings, with an optional adjacent-app
   auto-detection fallback.
5. Write requests and results beneath the user-selected Atlas cache root.
6. Add **Open in Morphis** and **Render preview** actions to a selected PBR
   material. Disable them with an explanation when Morphis is not configured.
7. Use `requestId` to discard stale results when the user changes selection.
8. Cache key = schema version + normalized map paths + mtimes + parameters +
   preview settings + output dimensions.
9. Never modify, rename, move, or bake into source texture directories.
10. Unit-test request serialization and map-role conversion independently of
    process launching.

## Current Morphis implementation

Morphis 0.21 and later implement this contract in `src/core/AtlasPreview.*` and
the `morphis_studio` command-line runtime. A request is compiled into ordinary
Morphis texture, Production PBR, geometry, camera, light, scene, and output
nodes. Interactive requests remain editable and can be saved as `.morphis`
projects.

Morphis 0.23 added `--render-backend auto|filament|opengl`. Atlas uses `auto`:
the optional Filament 1.75 helper provides production offscreen previews with
IBL, physical lights, ACES output, PBR reflection layers, parallax height,
anti-aliasing, and anisotropic filtering; Morphis falls back to its OpenGL
preview renderer when Filament is unavailable or cannot load the requested
geometry. Result JSON identifies the renderer that produced the image.

## Atlas implementation status

The Atlas-side checklist above is implemented on `agent/complete-phase-one`:

- `MorphisPreviewRequest` + `serializeMorphisPreviewRequest` live in `AtlasCore`
  (`src/AtlasCore/include/AtlasCore/MorphisPreview.h`, `.../src/MorphisPreview.cpp`),
  no Qt or process dependency. `morphisPreviewMapKey(PbrMapKind)` implements the
  role→v1-key mapping (Glossiness stays its own `glossiness` key, never folded into
  Roughness; Specular/IOR/Thickness/Translucency/Packed have no v1 slot yet and are
  silently omitted, per the checklist). Unit-tested independently of process
  launching in `tests/AtlasTests.cpp`
  (`morphisPreviewMapsWorkflowToV1Keys`, `morphisPreviewRequestSerializesToV1Schema`).
- `MorphisPreviewClient` (`src/AtlasUI/MorphisPreviewClient.{h,cpp}`) wraps
  `QProcess`, asynchronously, off the UI thread, with a two-minute cold-renderer
  timeout and structured result/error capture.
  - Executable path: `Settings > Choose Morphis executable...`, persisted via
    `QSettings` (`morphis/executablePath`), with adjacent-folder auto-detection
    (`morphis_studio.exe` next to `atlas.exe`, or in a sibling `Morphis/` folder)
    as a fallback when unset.
  - Requests/results/images are written under
    `<cacheRoot>/morphis-previews/<cacheKey>.{request.json,result.json,png}`.
    `cacheKey` = SHA-256 over schema version, every map's normalized path + mtime,
    all material/light/camera parameters, geometry, and output dimensions — this
    doubles as `requestId`, so a repeat request for an unchanged material serves the
    cached PNG without relaunching Morphis, and a late-arriving result for a
    material the user has since navigated away from is discarded (checked against
    both the returned `requestId` and the still-selected item).
- **Open Library** (Saved Scans → a scan → *Open Library*) adds **Render preview**
  (headless) and **Open in Morphis** (interactive) actions to a selected PBR
  material's detail panel; both are disabled with an explanatory tooltip until
  Morphis is configured. Artists can preview on a sphere, plane, cube, cylinder,
  or a linked Wavefront OBJ model discovered with the material. A successful headless render
  replaces the texture thumbnail with the rendered PNG and reports whether
  Filament or the OpenGL compatibility renderer produced it.
- Atlas serializes the complete Morphis Production PBR scalar parameter set:
  IOR/specular, anisotropy, coat, sheen, UV transform, normal convention,
  height, opacity, and emission controls. These values are included in the
  cache key so changing a material parameter cannot reuse a stale render.
- Atlas never writes into source texture directories; only into its own cache root.

No Morphis source change is required for this Atlas integration. Configure a
current `morphis_studio.exe` in Atlas Settings and click **Render preview** on a
persisted material in **Open Library**. Atlas uses the public Morphis runtime
contract rather than linking against Morphis internals, so both applications
can continue evolving independently.

## Why this proves Morphis app-making

Atlas is not receiving a one-off hard-coded renderer. The request is compiled
into ordinary Morphis nodes and evaluated by the same scene, material, light,
camera, and render systems used by Morphis Studio. Later, the same public process
API can power product turntables, batch thumbnails, material comparison sheets,
and other small applications made from Morphis graphs.
