#!/usr/bin/env bash
# Manual mid-refactor testing with persistent, separate user profiles and a log.
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${APP_DIR}/build/mid-refactor-user-tests"
APP_BINARY="${APP_DIR}/build/src/prusa-slicer"
if [[ ! -x "${APP_BINARY}" ]]; then
    printf 'Brak aplikacji: %s\nZbuduj ją poleceniem: bash ./build_linux.sh\n' "${APP_BINARY}" >&2
    exit 2
fi
# Reject older builds that hide current editing or slicing corrections.
for app_source in \
    src/clipper/clipper.cpp src/clipper/ClipperIntegerRounding.hpp \
    src/libslic3r/ClipperUtils.cpp \
    src/libslic3r/Preset.cpp src/libslic3r/Preset.hpp src/slic3r/GUI/Tab.cpp src/slic3r/GUI/Tab.hpp \
    src/libslic3r/ClipperOffsetProfile.hpp \
    src/libslic3r/SLAPrint.cpp \
    src/libslic3r/SLAPrint.hpp \
    src/libslic3r/Format/SL1.cpp \
    src/libslic3r/Format/SL1.hpp \
    src/libslic3r/Format/AnycubicSLA.cpp \
    src/libslic3r/Format/CrealityCXDLPv4.cpp \
    src/libslic3r/SLA/GeneratedLayerOwnership.hpp \
    src/libslic3r/SLA/FreshLayerJob.cpp \
    src/libslic3r/SLA/FreshLayerJob.hpp \
    src/libslic3r/SLA/ProjectArchive.cpp \
    src/libslic3r/SLA/ProjectArchive.hpp \
    src/libslic3r/SLA/SL1FreshExportVerify.hpp \
    src/libslic3r/SLA/SL1FreshExportPlan.hpp \
    src/libslic3r/SLA/EncodedMaskReuse.hpp \
    src/libslic3r/SLA/LayerDocument.cpp \
    src/libslic3r/SLA/LayerDocument.hpp \
    src/libslic3r/SLA/LayerMaskValidation.hpp \
    src/libslic3r/SLA/LayerPreviewCache.hpp \
    src/libslic3r/SLA/CrealityCXDLPv4FreshExportPlan.cpp \
    src/libslic3r/SLA/SupportPointGenerator.cpp \
    src/libslic3r/SLA/SupportPointGenerator.hpp \
    src/libslic3r/SLA/SupportOverhangSafetyOffset.hpp \
    src/libslic3r/SLA/SupportIslands/UniformSupportIsland.cpp \
    src/libslic3r/SLA/SupportIslands/SupportIslandPoint.hpp \
    src/libslic3r/SLA/SupportIslands/SupportAlignmentTransaction.hpp \
    src/libslic3r/SLA/PeninsulaMinimumOffsetReuse.hpp \
    src/slic3r/GUI/LayerEditorDialog.cpp \
    src/slic3r/GUI/LayerEditorDialog.hpp \
    src/slic3r/GUI/LayerCanvasGeometry.hpp \
    src/slic3r/GUI/LayerRangeSelection.hpp \
    src/slic3r/GUI/LayerRangeSlider.hpp \
    src/slic3r/GUI/Plater.cpp \
    src/slic3r/GUI/Sidebar.cpp \
    src/slic3r/GUI/BackgroundSlicingProcess.cpp \
    src/slic3r/GUI/MainFrame.cpp \
    src/slic3r/GUI/GUI_Factories.cpp \
    src/slic3r/GUI/TopBar.cpp \
    src/slic3r/GUI/TopBarMenus.cpp \
    src/slic3r/GUI/Gizmos/GLGizmosManager.cpp \
    src/slic3r/GUI/Gizmos/GLGizmosManager.hpp \
    src/slic3r/GUI/SlaRasterToolsDialog.cpp \
    src/slic3r/GUI/ExposureFinderDialog.cpp \
    src/slic3r/GUI/ExposureFinderDialog.hpp \
    src/slic3r/GUI/RasterGeneratorTask.hpp; do
    if [[ "${APP_DIR}/${app_source}" -nt "${APP_BINARY}" ]]; then
        printf 'Aplikacja jest starsza niż %s. Przebuduj ją: bash ./build_linux.sh\n' "${app_source}" >&2
        exit 3
    fi
done
mkdir -p "${TEST_DIR}/presets"
printf 'Program: %s\nSHA256: ' "${APP_BINARY}"
sha256sum "${APP_BINARY}"
printf 'Pędzel: po slicingu wybierz Edit layers → Sliced → Paint masks. Zakres ustawisz polami nad podglądem lub suwakiem po prawej; odznacz Single layer.\n'
printf 'Profile testowe: %s\nLog aplikacji: %s\n' "${TEST_DIR}/presets" "${TEST_DIR}/application.log"
exec bash "${APP_DIR}/run_prusa_slicer.sh" --datadir "${TEST_DIR}/presets" "$@" >> "${TEST_DIR}/application.log" 2>&1
