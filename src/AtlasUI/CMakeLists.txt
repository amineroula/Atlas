qt_standard_project_setup()
qt_add_executable(atlas main.cpp MainWindow.cpp MainWindow.h ThumbnailCache.cpp ThumbnailCache.h
  MorphisPreviewClient.cpp MorphisPreviewClient.h)
target_link_libraries(atlas PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets Atlas::Core Atlas::Scanner Atlas::Database Atlas::Jobs)
set_target_properties(atlas PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE)

include(GNUInstallDirs)
install(TARGETS atlas
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
qt_generate_deploy_app_script(
  TARGET atlas
  OUTPUT_SCRIPT atlas_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${atlas_deploy_script})
