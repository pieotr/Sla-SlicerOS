# Computes SLIC3R_BUILD_ID = "SLA-Slicer-<SLA_SLICER_VERSION>+<suffix>".
#
# suffix, in order of precedence:
#   1. the CMake cache variable / environment variable SLA_SLICER_BUILD_ID (verbatim; used by the
#      docker build, whose container copy of the tree has no .git),
#   2. the first line of the file BUILD_ID in the top-level source directory (written by
#      tools/release/make_source_tarball.sh, so that a build from the source tarball, which has no .git,
#      shows the same id as a build from the git checkout it was made from),
#   3. `git describe --always --dirty` when the source tree is itself a git checkout,
#   4. the literal "source" (tarball / no git).
# The result never ends in "+UNKNOWN".
set(SLA_SLICER_BUILD_ID "" CACHE STRING "Override of the build id suffix (the part after '+'); empty = derive from git")

set(_sla_build_suffix "${SLA_SLICER_BUILD_ID}")
if("${_sla_build_suffix}" STREQUAL "" AND NOT "$ENV{SLA_SLICER_BUILD_ID}" STREQUAL "")
    set(_sla_build_suffix "$ENV{SLA_SLICER_BUILD_ID}")
endif()
if("${_sla_build_suffix}" STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_ID")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_ID" _sla_build_id_file LIMIT_COUNT 1)
    string(STRIP "${_sla_build_id_file}" _sla_build_suffix)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/BUILD_ID")
endif()
if("${_sla_build_suffix}" STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _sla_git_desc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _sla_git_rc)
        if(_sla_git_rc EQUAL 0 AND NOT "${_sla_git_desc}" STREQUAL "")
            set(_sla_build_suffix "${_sla_git_desc}")
        endif()
    endif()
endif()
if("${_sla_build_suffix}" STREQUAL "")
    set(_sla_build_suffix "source")
endif()
string(REGEX REPLACE "[^A-Za-z0-9._-]" "-" _sla_build_suffix "${_sla_build_suffix}")
set(SLIC3R_BUILD_ID "SLA-Slicer-${SLA_SLICER_VERSION}+${_sla_build_suffix}")
message(STATUS "SLA-Slicer ${SLA_SLICER_VERSION} (based on PrusaSlicer ${SLIC3R_VERSION}), build id ${SLIC3R_BUILD_ID}")
