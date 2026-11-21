get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_organic_supports_tests.cpp
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
    sla_goo_tests.cpp
    sla_goo_reader_tests.cpp
    sla_printer_profile_tests.cpp
    sla_pcb_calibration_tests.cpp
    sla_calibration_generators_tests.cpp
    sla_layer_issues_tests.cpp
    sla_anycubic_model_precision_tests.cpp
    sla_anycubic_pcb_precision_tests.cpp
    sla_pm4n_producer_tests.cpp
    sla_pm4n_legacy_tests.cpp
    sla_exposure_finder_raster_tests.cpp
    sla_exposure_finder_table_tests.cpp
    sla_exposure_finder_draft_tests.cpp
    sla_resin_stock_tests.cpp
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
    sla_layer_large_document_tests.cpp
    sla_project_save_perf_tests.cpp
    sla_layer_stream_tests.cpp
    sla_layer_brush_tests.cpp
    sla_layer_operations_tests.cpp
    sla_layer_f41_tests.cpp
    sla_stir_export_tests.cpp
    sla_layer_ops_document_tests.cpp
    sla_layer_suggestions_tests.cpp
    sla_layer_recipe_tests.cpp
    sla_reader_parameters_tests.cpp
    sla_layer_operations2_tests.cpp
    sla_more_readers_tests.cpp
    sla_more_readers2_tests.cpp
    sla_layer_ops_document2_tests.cpp
    sla_zcorrection_tests.cpp
    sla_offline_tests.cpp
    sla_header_mirror_tests.cpp
    sla_cxdlp_edit_tests.cpp
    sla_fresh_direct_export_tests.cpp
    sla_representation_adjustment_tests.cpp
    sla_fff_project_3mf_tests.cpp
    sla_legacy_datadir_tests.cpp
    sla_config_schema_tests.cpp
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
add_test(NAME sla_fresh_direct_export COMMAND ${_TEST_NAME}_tests "[sla_fresh_direct]")
set_tests_properties(sla_fresh_direct_export PROPERTIES LABELS "sla;formats;smoke" TIMEOUT 600)
add_test(NAME sla_representation_adjustment COMMAND ${_TEST_NAME}_tests "[sla_representation]")
set_tests_properties(sla_representation_adjustment PROPERTIES LABELS "sla;formats;smoke" TIMEOUT 600)
add_test(NAME sla_header_mirror COMMAND ${_TEST_NAME}_tests "[sla_header_mirror]")
set_tests_properties(sla_header_mirror PROPERTIES LABELS "sla;formats;smoke" TIMEOUT 300)

# W9b-2: the schema dump written by the first test is checked against the shipped SLA profiles by the second.
add_test(NAME sla_config_schema_dump COMMAND ${CMAKE_COMMAND} -E env SLA_SCHEMA_DUMP=${CMAKE_CURRENT_BINARY_DIR}/config-schema
    $<TARGET_FILE:${_TEST_NAME}_tests> "[sla_config_schema]")
set_tests_properties(sla_config_schema_dump PROPERTIES LABELS "sla;profiles;smoke" TIMEOUT 60 FIXTURES_SETUP sla_config_schema)
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

add_test(NAME sla_layer_large_documents COMMAND ${_TEST_NAME}_tests "[sla_layers_large]")
set_tests_properties(sla_layer_large_documents PROPERTIES LABELS "sla;layers;memory" TIMEOUT 300)
# Real >1 GiB container (about a minute, several GiB of RAM and disk): opt-in only.
option(SLA_SLOW_LAYER_TESTS "Register the multi-gigabyte layer document test" OFF)
if(SLA_SLOW_LAYER_TESTS)
    add_test(NAME sla_layer_huge_container COMMAND ${_TEST_NAME}_tests "[sla_layers_slow]")
    set_tests_properties(sla_layer_huge_container PROPERTIES LABELS "sla;layers;slow" TIMEOUT 900)
endif()

add_test(NAME sla_pcb_calibration COMMAND ${_TEST_NAME}_tests "[sla_pcb],[sla_calibration]")
set_tests_properties(sla_pcb_calibration PROPERTIES LABELS "sla;pcb;calibration;smoke")

add_test(NAME sla_resin_stock COMMAND ${_TEST_NAME}_tests "[sla_resin_stock]")
set_tests_properties(sla_resin_stock PROPERTIES LABELS "sla;resin;smoke" TIMEOUT 60)

add_test(NAME sla_offline_api COMMAND ${_TEST_NAME}_tests "[sla_offline]")
set_tests_properties(sla_offline_api PROPERTIES LABELS "sla;offline;smoke" TIMEOUT 15)

# The executable must also follow the SLA-only configuration contract. Native
# writer tests do not exercise command parsing, exit codes or automatic suffixes.
if(TARGET PrusaSlicer)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME sla_config_schema_keys COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/verify_sla_schema_keys.py
            ${CMAKE_CURRENT_BINARY_DIR}/config-schema ${CMAKE_SOURCE_DIR}/resources/profiles)
        set_tests_properties(sla_config_schema_keys PROPERTIES LABELS "sla;profiles;smoke" TIMEOUT 60 FIXTURES_REQUIRED sla_config_schema)
        add_test(NAME sla_cli_smoke COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cli_sla_smoke.py $<TARGET_FILE:PrusaSlicer>
            --output ${CMAKE_CURRENT_BINARY_DIR}/cli-sla-smoke)
        set_tests_properties(sla_cli_smoke PROPERTIES LABELS "sla;cli;smoke" TIMEOUT 120)
    endif()
endif()

# Hidden native cases preserve actual historical project payloads. Register both
# explicitly so the default CTest suite cannot silently omit migration coverage.
# This dependency is reached only when building the SLA test suite.
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

# Offline build must not link web view / network libraries (goal G12).
if (UNIX AND NOT APPLE)
    add_test(NAME sla_offline_no_network_libs
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/architecture/verify_offline_no_network_libs.py
                     --binary $<TARGET_FILE:PrusaSlicer>)
    set_tests_properties(sla_offline_no_network_libs PROPERTIES LABELS "sla;offline;architecture;smoke" TIMEOUT 15)
endif()

# Independent readback against the UVtools reference (development oracle only, never a
# runtime dependency). Set UVTOOLS_CMD to UVtoolsCmd; without it the scripts exit 77 = skipped.
if (Python3_EXECUTABLE)
    add_test(NAME sla_oracle_issues_vs_uvtools
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/oracle/issues_vs_uvtools.py
                     --slicer $<TARGET_FILE:PrusaSlicer> --output ${CMAKE_BINARY_DIR}/oracle-issues)
    add_test(NAME sla_oracle_format_sweep
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/oracle/uvtools_format_sweep.py
                     --slicer $<TARGET_FILE:PrusaSlicer> --output ${CMAKE_BINARY_DIR}/oracle-sweep/ctest --jobs 12)
    set_tests_properties(sla_oracle_issues_vs_uvtools PROPERTIES LABELS "sla;oracle" SKIP_RETURN_CODE 77 TIMEOUT 900)
    set_tests_properties(sla_oracle_format_sweep PROPERTIES LABELS "sla;oracle;formats" SKIP_RETURN_CODE 77 TIMEOUT 1800)
    # Layer-editor saves (recipe edit -> native save) read back by UVtools. Uses python3 from PATH
    # (needs numpy + Pillow; exit 77 = SKIPPED without them or without UVTOOLS_CMD).
    add_test(NAME sla_oracle_editor_save
             COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/oracle/editor_save_vs_uvtools.py
                     --slicer $<TARGET_FILE:PrusaSlicer> --output ${CMAKE_BINARY_DIR}/oracle-editor-save)
    set_tests_properties(sla_oracle_editor_save PROPERTIES LABELS "sla;oracle;formats" SKIP_RETURN_CODE 77 TIMEOUT 1800)
endif()

# Offline SLA hint catalog: content rules and Polish translation coverage (no display needed).
if (Python3_EXECUTABLE)
    add_test(NAME sla_hint_catalog
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/gui/test_offline_hint_catalog.py)
    add_test(NAME sla_hint_translations
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/gui/test_offline_hint_caption_correspondence.py)
    set_tests_properties(sla_hint_catalog sla_hint_translations PROPERTIES LABELS "sla;gui;smoke" TIMEOUT 60)
    # Polish catalog: .po compiles (msgfmt -c --check-format), committed .mo is current, key strings of the
    # Configuration Wizard and the Layer editor are translated, no localizable string of those sources is missing.
    add_test(NAME sla_pl_catalog
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/gui/test_pl_catalog.py)
    set_tests_properties(sla_pl_catalog PROPERTIES LABELS "sla;gui;smoke;localization" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# Guard for the FFF dead-file purge (W3): removed libslic3r paths stay absent and no libslic3r
# file includes a removed header (text scan, no build tree needed).
if (Python3_EXECUTABLE)
    add_test(NAME sla_fff_purge_guard
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/architecture/verify_fff_purge_guard.py)
    set_tests_properties(sla_fff_purge_guard PROPERTIES LABELS "sla;architecture;smoke" TIMEOUT 120)
endif()

# Runtime GUI acceptance (waves 1-9 were never run before). Each test starts a private, invisible
# KWin/Xwayland/AT-SPI desktop (tests/gui/run_private_gui.sh; exit 77 = SKIPPED when kwin_wayland,
# Xwayland, dbus-run-session or python3-gi+Atspi are missing), so it never touches the user's
# display or ~/.config. They use the PATH `python3` (the one that has PyGObject + Atspi, which the runner
# itself checks), not Python3_EXECUTABLE, which may be a venv without `gi`. Labels: "gui" + "gui-runtime". The fast subset is always registered; the
# slower scripts need -DSLA_GUI_SLOW_TESTS=ON. See doc/TESTING.md section 4 for how to run or
# exclude them (ctest -LE "oracle|gui-runtime").
if (Python3_EXECUTABLE AND UNIX AND NOT APPLE)
    set(SLA_GUI_RUNNER ${CMAKE_SOURCE_DIR}/tests/gui/run_private_gui.sh)
    function(sla_add_gui_test name script timeout)
        add_test(NAME ${name}
                 COMMAND bash ${SLA_GUI_RUNNER} -- python3 ${CMAKE_SOURCE_DIR}/tests/gui/ctest_gui.py
                         --binary $<TARGET_FILE:PrusaSlicer> --output ${CMAKE_BINARY_DIR}/gui-ctest/${name}
                         --script ${script}
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(${name} PROPERTIES LABELS "sla;gui;gui-runtime" SKIP_RETURN_CODE 77 TIMEOUT ${timeout}
                             RUN_SERIAL TRUE)
    endfunction()
    sla_add_gui_test(sla_gui_wizard        printer_wizard_smoke  120)
    sla_add_gui_test(sla_gui_layer_editor  layer_editor_smoke    180)
    sla_add_gui_test(sla_gui_menus         sla_menus_smoke       180)
    sla_add_gui_test(sla_gui_issues        sliced_issues_smoke   300)
    # Every script of tests/gui/run_suite.py (its fixture table is the single source), one CTest test each. The wrapper
    # (tests/gui/ctest_suite.py) runs run_suite.py --only <script>, which starts that script in its own private
    # session; RUN_SERIAL keeps them from overlapping. Label gui-suite lets one select exactly these:
    #   ctest -L gui-suite -R <name>     (names: sla_gui_suite_<script>, ':' becomes '__')
    # The default fast selection (-LE "oracle|gui-runtime") never includes them.
    execute_process(COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/gui/run_suite.py --list
                    OUTPUT_VARIABLE SLA_GUI_SUITE_SCRIPTS RESULT_VARIABLE _suite_list_rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (_suite_list_rc EQUAL 0 AND SLA_GUI_SUITE_SCRIPTS)
        string(REPLACE "\n" ";" SLA_GUI_SUITE_SCRIPTS "${SLA_GUI_SUITE_SCRIPTS}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/gui/run_suite.py)
        foreach (_script IN LISTS SLA_GUI_SUITE_SCRIPTS)
            string(REPLACE ":" "__" _suite_id "${_script}")
            set(_suite_timeout 1500)
            if (_script STREQUAL "release_acceptance_smoke")
                set(_suite_timeout 3600)
            endif()
            add_test(NAME sla_gui_suite_${_suite_id}
                     COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/gui/ctest_suite.py
                             --binary $<TARGET_FILE:PrusaSlicer> --out ${CMAKE_BINARY_DIR}/gui-suite/${_suite_id}
                             --script ${_script} --timeout ${_suite_timeout}
                     WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
            set_tests_properties(sla_gui_suite_${_suite_id} PROPERTIES LABELS "sla;gui;gui-runtime;gui-suite"
                                 SKIP_RETURN_CODE 77 TIMEOUT ${_suite_timeout} RUN_SERIAL TRUE)
        endforeach()
    endif()
    option(SLA_GUI_SLOW_TESTS "Also register the slow runtime GUI tests (operations, canvas tools, PCB, calibration, profile I/O)" OFF)
    if (SLA_GUI_SLOW_TESTS)
        sla_add_gui_test(sla_gui_operations   sliced_operations_smoke        900)
        sla_add_gui_test(sla_gui_canvas       sliced_canvas_tools_smoke      900)
        sla_add_gui_test(sla_gui_pcb          pcb_expose_smoke               600)
        sla_add_gui_test(sla_gui_calibration  calibration_entries_smoke      1200)
        sla_add_gui_test(sla_gui_profile_io   profile_io_smoke               600)
    endif()
endif()
