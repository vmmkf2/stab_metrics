// pmu_metrics/src/static_registration.cc
//
// Forces the Perfetto DataSource to register itself during static
// initialisation — before main() — so the host's
// perfetto::Tracing::Initialize() call finds "dev.pmu_metrics".
//
// This TU exists solely to guarantee the registration object is not
// stripped by the linker.  Link the host with --whole-archive (or
// CMake's WHOLE_ARCHIVE link option) if using LTO, to prevent DCE.

#include "pmu_data_source.h"

namespace pmu_metrics::internal {

namespace {
// A simple static object whose constructor calls DataSource::Register().
// Constructed before main() by the C++ runtime.
struct DataSourceRegistrar {
    DataSourceRegistrar() {
        perfetto::DataSourceDescriptor dsd;
        dsd.set_name(kDataSourceName);
        PmuDataSource::Register(dsd);
    }
};

// The object itself.  Its address is taken via volatile to prevent the
// compiler from optimising it away even without --whole-archive.
[[maybe_unused]] volatile DataSourceRegistrar g_registrar;
}  // namespace

}  // namespace pmu_metrics::internal
