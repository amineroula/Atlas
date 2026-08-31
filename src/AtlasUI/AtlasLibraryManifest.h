#pragma once

#include <AtlasCore/QuixelAsset.h>
#include <AtlasDatabase/Catalog.h>

#include <QString>

#include <vector>

namespace atlas {

[[nodiscard]] QString atlasLibraryManifestPath();
[[nodiscard]] bool writeAtlasLibraryManifest(
    const std::vector<CatalogMaterial>& materials,
    const std::vector<QuixelAsset>& quixelAssets,
    QString* error = nullptr);

}  // namespace atlas
