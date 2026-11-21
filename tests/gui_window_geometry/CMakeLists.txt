# Pure geometry contract: no GUI backend, renderer, core or TBB dependency.
add_executable(gui_window_geometry_tests test_window_geometry.cpp)
target_compile_features(gui_window_geometry_tests PRIVATE cxx_std_17)
target_include_directories(gui_window_geometry_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gui_window_geometry_tests PRIVATE test_common)
set_property(TARGET gui_window_geometry_tests PROPERTY FOLDER "tests")
add_test(NAME sla_window_geometry COMMAND gui_window_geometry_tests)
set_tests_properties(sla_window_geometry PROPERTIES LABELS "sla;gui;geometry;unit" TIMEOUT 15)
