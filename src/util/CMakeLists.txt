add_library(gitnortekUtil Path.cpp Debug.h Debug.cpp PerformanceTrace.cpp
                              WaitCursor.cpp WaitCursor.h)

target_link_libraries(gitnortekUtil Qt6::Core Qt6::Gui)
target_include_directories(gitnortekUtil INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(gitnortekUtil PROPERTIES AUTOMOC ON)
