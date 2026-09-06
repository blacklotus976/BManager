#pragma once
#include <string>

// A named package/bundle grouping several real, independent, already-reusable
// `services` rows together for organizational/viewing purposes only. See
// master_service_components for the N-M link to its component services.
// Payments never attach to a MasterService directly -- they still attach to
// one real component service + one user, exactly as before.
struct MasterService {
    int id = -1;
    std::string label;
};
