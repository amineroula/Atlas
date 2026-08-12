add_library(AtlasCore
  src/Asset.cpp
  src/FileOperations.cpp
  src/StorageAnalyzer.cpp
)
add_library(Atlas::Core ALIAS AtlasCore)
target_include_directories(AtlasCore PUBLIC include)
target_compile_features(AtlasCore PUBLIC cxx_std_20)
