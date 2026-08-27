add_executable(
  gitnortek-indexer
  main.cpp
  indexer.cpp
  diffgrabber.cpp
  map.cpp
  reduce.cpp
  resultwriter.cpp
  common.cpp
  lexerpool.cpp
  idstorage.cpp)
target_link_libraries(gitnortek-indexer index)
target_compile_definitions(gitnortek-indexer
                           PRIVATE GITNORTEK_VERSION="${GITNORTEK_VERSION}")

set_target_properties(gitnortek-indexer PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                   $<TARGET_FILE_DIR:gitnortek>)

if(APPLE)
  set_target_properties(
    gitnortek-indexer PROPERTIES INSTALL_RPATH
                               "/usr/local/qt;@loader_path/../Frameworks")
else()
  if(WIN32)
    target_link_libraries(gitnortek-indexer Dbghelp.lib)
  elseif(NOT FLATAPK)
    set_target_properties(gitnortek-indexer PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()

  install(
    TARGETS gitnortek-indexer
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ${GITNORTEK_NAME})
endif()
