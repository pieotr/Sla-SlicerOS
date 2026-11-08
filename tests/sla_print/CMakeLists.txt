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
    sla_sl1s_tests.cpp
    sla_klipper_tests.cpp
    sla_archive_registry_tests.cpp
    sla_printer_profile_tests.cpp
    sla_pcb_calibration_tests.cpp
    sla_anycubic_model_precision_tests.cpp
    sla_anycubic_pcb_precision_tests.cpp
    sla_pm4n_producer_tests.cpp
    sla_pm4n_legacy_tests.cpp
    sla_exposure_finder_raster_tests.cpp
    sla_exposure_finder_table_tests.cpp
    sla_exposure_finder_draft_tests.cpp
    sla_exposure_position_tests.cpp
    sla_exposure_sequence_tests.cpp
    sla_ctb_encrypted_editor_tests.cpp
    sla_ctb_modern_editor_tests.cpp
    sla_cbddlp_versions_tests.cpp
    sla_anycubic_legacy_tests.cpp
    sla_layer_parameters_tests.cpp
    sla_sl1_fresh_export_tests.cpp
    sla_fresh_statistics_tests.cpp
    sla_statistics_presentation_tests.cpp
    sla_cxdlpv4_fresh_export_tests.cpp
    sla_cxdlpv4_rle_tests.cpp
    sla_anycubic_subimages_tests.cpp
    sla_anycubic_editor_tests.cpp
    sla_anycubic_tsmc_tests.cpp
    sla_anycubic_version_contract_tests.cpp
    sla_layer_archive_cancel_tests.cpp
    sla_ctb_conversion_readback_tests.cpp
    sla_ctb_parameter_transfer_tests.cpp
    sla_ctb_bootstrap_readback_tests.cpp
    sla_brush_span_tests.cpp
    sla_paint_batch_tests.cpp
    sla_overhang_safety_tests.cpp
    sla_encoded_mask_reuse_tests.cpp
    sla_layer_image_tests.cpp
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
# The main suite exercises the production default even if a developer shell
# carries the full-clipping fallback or old-kernel detailed profiling flags.
add_test(NAME ${_TEST_NAME}_tests COMMAND ${CMAKE_COMMAND} -E env
    --unset=SLA_EXPERIMENT_ISLAND_BBOX_CLIP --unset=SLA_PROFILE_ISLAND_CLIP_SETUP
    --unset=SLA_EXPERIMENT_SUPPORT_LINK_BBOX --unset=SLA_PROFILE_SUPPORT_LINK_REDUCTION
    $<TARGET_FILE:${_TEST_NAME}_tests> ${CATCH_EXTRA_ARGS})

# Detailed counters describe the complete original kernel. Keep that contract
# explicit and separate; never silently run it under the reduced-kernel label.
add_test(NAME sla_island_full_clip_profile COMMAND ${CMAKE_COMMAND} -E env
    --unset=SLA_PROFILE_ISLAND_CLIP_SETUP SLA_EXPERIMENT_ISLAND_BBOX_CLIP=0
    $<TARGET_FILE:${_TEST_NAME}_tests> "[sla_island_clip_profile]")
set_tests_properties(sla_island_full_clip_profile PROPERTIES
    LABELS "sla;geometry;diagnostic" TIMEOUT 120)

# Hidden link fixtures must run in their declared kernel/profile environment.
# Keep normal exit-code handling: an output-regex match must not mask a native
# failure. Dedicated native assertions remain authoritative.
add_test(NAME sla_support_link_full_profile COMMAND ${CMAKE_COMMAND} -E env
    --unset=SLA_EXPERIMENT_ISLAND_BBOX_CLIP --unset=SLA_PROFILE_ISLAND_CLIP_SETUP
    SLA_EXPERIMENT_SUPPORT_LINK_BBOX=0 SLA_PROFILE_SUPPORT_LINK_REDUCTION=1
    $<TARGET_FILE:${_TEST_NAME}_tests> "[sla_support_link_profile_fixture]")
set_tests_properties(sla_support_link_full_profile PROPERTIES
    LABELS "sla;geometry;diagnostic" TIMEOUT 120)

add_test(NAME sla_support_link_profile_policy COMMAND ${CMAKE_COMMAND} -E env
    --unset=SLA_EXPERIMENT_ISLAND_BBOX_CLIP --unset=SLA_PROFILE_ISLAND_CLIP_SETUP
    SLA_EXPERIMENT_SUPPORT_LINK_BBOX=1 SLA_PROFILE_SUPPORT_LINK_REDUCTION=1
    $<TARGET_FILE:${_TEST_NAME}_tests> "[sla_support_link_profile_policy]")
set_tests_properties(sla_support_link_profile_policy PROPERTIES
    LABELS "sla;geometry;diagnostic" TIMEOUT 120)

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

# Hidden native cases preserve actual historical project payloads. Register both
# explicitly so the default CTest suite cannot silently omit migration coverage.
# This dependency is reached only when building the SLA test suite.
if(SLIC3R_SLA_ONLY)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    # Test the actual intersection event order and a deterministic work bound;
    # wall-clock assertions would be unstable across machines.
    if(MSVC)
        set(_clipper_contract_compiler_style msvc)
    else()
        set(_clipper_contract_compiler_style gnu)
    endif()
    add_test(NAME sla_clipper_intersection_order COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/verify_clipper_intersection_order.py
        --source ${CMAKE_SOURCE_DIR}/src/clipper/clipper.cpp
        --work-dir ${CMAKE_CURRENT_BINARY_DIR}/clipper-order-runtime
        --compiler ${CMAKE_CXX_COMPILER}
        --compiler-style ${_clipper_contract_compiler_style})
    set_tests_properties(sla_clipper_intersection_order PROPERTIES
        LABELS "sla;geometry;performance-contract" TIMEOUT 90 RUN_SERIAL TRUE)
    foreach(_legacy_kind IN ITEMS model pcb)
        add_test(NAME sla_legacy_${_legacy_kind}_migration COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_anycubic_515_legacy_migration.py
            --kind ${_legacy_kind}
            --executable $<TARGET_FILE:${_TEST_NAME}_tests>
            --build-root ${CMAKE_BINARY_DIR}
            --work-root ${CMAKE_CURRENT_BINARY_DIR}/migration-runtime
            --fixtures ${CMAKE_SOURCE_DIR}/tests/data/anycubic_515_legacy_${_legacy_kind})
        set_tests_properties(sla_legacy_${_legacy_kind}_migration PROPERTIES
            LABELS "sla;migration;legacy;selfcontained;smoke" TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    # Genuine PM4n calibration/model/PCB projects include literal auto and 515.
    # Their unknown Gray8 data must survive independently of new-job precision.
    add_test(NAME sla_legacy_pm4n_migration COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/run_pm4n_legacy_migration.py
        --executable $<TARGET_FILE:${_TEST_NAME}_tests>
        --build-root ${CMAKE_BINARY_DIR}
        --work-root ${CMAKE_CURRENT_BINARY_DIR}/migration-runtime
        --fixtures ${CMAKE_SOURCE_DIR}/tests/data/pm4n_515_legacy)
    set_tests_properties(sla_legacy_pm4n_migration PROPERTIES
        LABELS "sla;migration;legacy;selfcontained;smoke" TIMEOUT 120 RUN_SERIAL TRUE)
endif()
