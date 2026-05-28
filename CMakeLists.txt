cmake_minimum_required(VERSION 3.18)
project(pmu_metrics VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Perfetto SDK — provided by the host project.
#
# The host MUST export a CMake target named "perfetto" before add_subdirectory()
# or find_package(pmu_metrics).  Typically the host does:
#
#   add_library(perfetto STATIC third_party/perfetto/sdk/perfetto.cc)
#   target_include_directories(perfetto PUBLIC third_party/perfetto/sdk)
#
# We do NOT vendor Perfetto here; we depend on the host-exported target.
# ---------------------------------------------------------------------------
if(NOT TARGET perfetto)
    message(FATAL_ERROR
        "[pmu_metrics] CMake target 'perfetto' not found.\n"
        "The host project must define a 'perfetto' target that exposes "
        "the Perfetto SDK headers and objects before including pmu_metrics.")
endif()

# ---------------------------------------------------------------------------
# Static library  (CONSTRAINT A: NEVER SHARED or MODULE)
# ---------------------------------------------------------------------------
add_library(pmu_metrics STATIC
    src/pmu_data_source.cc
    src/pmu_session.cc
    src/perf_group.cc
    src/static_registration.cc
)

# Alias so consumers can use the namespaced form: pmu_metrics::pmu_metrics
add_library(pmu_metrics::pmu_metrics ALIAS pmu_metrics)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
target_include_directories(pmu_metrics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
target_link_libraries(pmu_metrics
    PUBLIC
        # Perfetto symbols must be visible to the host linker so the in-process
        # DataSource shares the same SDK instance.  Using PUBLIC here propagates
        # the Perfetto headers/link to the host automatically.
        perfetto
    PRIVATE
        # Linux perf_event_open is in the kernel ABI; no extra library needed.
        # If the host is cross-compiling for aarch64, the sysroot must contain
        # linux/perf_event.h.
)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
target_compile_options(pmu_metrics PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
    # Perfetto headers generate some warnings; suppress selectively.
    -Wno-deprecated-declarations
)

# ---------------------------------------------------------------------------
# Whole-archive hint for host projects using LTO.
#
# The static_registration.cc TU must not be dead-code-eliminated before
# main() runs.  With LTO the linker may strip it.  Expose an interface
# property so the host can use it:
#
#   target_link_options(my_host_binary PRIVATE
#       -Wl,--whole-archive $<TARGET_FILE:pmu_metrics::pmu_metrics>
#       -Wl,--no-whole-archive)
#
# Or use CMake ≥ 3.24 WHOLE_ARCHIVE property (see README).
# ---------------------------------------------------------------------------
set_target_properties(pmu_metrics PROPERTIES
    CXX_VISIBILITY_PRESET   hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ---------------------------------------------------------------------------
# Install rules (optional — skip if used only as add_subdirectory)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS pmu_metrics
    EXPORT  pmu_metricsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT pmu_metricsTargets
    NAMESPACE   pmu_metrics::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pmu_metrics
)
