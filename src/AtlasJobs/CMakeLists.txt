add_library(AtlasJobs src/JobQueue.cpp)
add_library(Atlas::Jobs ALIAS AtlasJobs)
target_include_directories(AtlasJobs PUBLIC include)
target_compile_features(AtlasJobs PUBLIC cxx_std_20)
