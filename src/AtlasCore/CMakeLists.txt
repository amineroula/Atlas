add_library(AtlasCore
  src/Asset.cpp
  src/AssetCatalogModel.cpp
  src/FabIngestService.cpp
  src/FileOperations.cpp
  src/MorphisPreview.cpp
  src/PbrMaterial.cpp
  src/QuixelAsset.cpp
  src/StorageAnalyzer.cpp
)
add_library(Atlas::Core ALIAS AtlasCore)
target_include_directories(AtlasCore PUBLIC include)
target_compile_features(AtlasCore PUBLIC cxx_std_20)
