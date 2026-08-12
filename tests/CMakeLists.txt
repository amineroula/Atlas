add_executable(AtlasTests AtlasTests.cpp)
target_link_libraries(AtlasTests PRIVATE Atlas::Core Atlas::Database Atlas::Scanner Atlas::Jobs)
add_test(NAME AtlasTests COMMAND AtlasTests)
