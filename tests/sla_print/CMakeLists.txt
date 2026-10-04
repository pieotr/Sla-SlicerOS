get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_raycast_tests.cpp
    sla_parabola_tests.cpp
    sla_voronoi_graph_tests.cpp
    sla_vectorUtils_tests.cpp
    sla_lineUtils_tests.cpp
    sla_supptreeutils_tests.cpp
    sla_archive_readwrite_tests.cpp
    sla_archive_registry_tests.cpp
    sla_printer_profile_tests.cpp
    sla_pcb_calibration_tests.cpp
    sla_layer_document_tests.cpp
    sla_zcorrection_tests.cpp
    sla_offline_tests.cpp
    ../../src/slic3r/Utils/HttpOffline.cpp)

# mold linker for successful linking needs also to link TBB library and link it before libslic3r.
target_link_libraries(${_TEST_NAME}_tests test_common TBB::tbb TBB::tbbmalloc libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    prusaslicer_copy_dlls(${_TEST_NAME}_tests)
endif()

# catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})

# Fast regression checks, separate from the exhaustive slicing/export suite.
add_test(NAME sla_archive_registry COMMAND ${_TEST_NAME}_tests "[sla_registry]")
set_tests_properties(sla_archive_registry PROPERTIES LABELS "sla;formats;smoke")
add_test(NAME sla_reference_roundtrip COMMAND ${_TEST_NAME}_tests "[sla_reference]")
set_tests_properties(sla_reference_roundtrip PROPERTIES LABELS "sla;formats;smoke" TIMEOUT 120)
add_test(NAME sla_printer_profiles COMMAND ${_TEST_NAME}_tests "[sla_profiles]")
set_tests_properties(sla_printer_profiles PROPERTIES LABELS "sla;profiles;smoke")

if (WIN32)
    # Adds a post-build copy of libgmp-10.dll
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_PREFIX_PATH}/bin/libgmp-10.dll"
          $<TARGET_FILE_DIR:${_TEST_NAME}_tests>)
endif()

add_test(NAME sla_layer_document COMMAND ${_TEST_NAME}_tests "[sla_layers]")
set_tests_properties(sla_layer_document PROPERTIES LABELS "sla;layers;smoke")

add_test(NAME sla_pcb_calibration COMMAND ${_TEST_NAME}_tests "[sla_pcb],[sla_calibration]")
set_tests_properties(sla_pcb_calibration PROPERTIES LABELS "sla;pcb;calibration;smoke")

add_test(NAME sla_offline_api COMMAND ${_TEST_NAME}_tests "[sla_offline]")
set_tests_properties(sla_offline_api PROPERTIES LABELS "sla;offline;smoke" TIMEOUT 15)

# The executable must also follow the SLA-only configuration contract. Native
# writer tests do not exercise command parsing, exit codes or automatic suffixes.
if(SLIC3R_SLA_ONLY AND TARGET PrusaSlicer)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME sla_cli_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cli_sla_smoke.py $<TARGET_FILE:PrusaSlicer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/cli-sla-smoke)
        set_tests_properties(sla_cli_smoke PROPERTIES LABELS "sla;cli;smoke" TIMEOUT 120)
    endif()
endif()
