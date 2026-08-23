#include "dart_raw_bridge.h"

namespace {

struct BridgeState {
    DartPlantRawToHandle raw_to_handle = nullptr;
    DartPlantHandleToRaw handle_to_raw = nullptr;
    const char* backend = "unavailable";
};

BridgeState g_state;
DartPlantRawToHandle g_registered_raw_to_handle = nullptr;
DartPlantHandleToRaw g_registered_handle_to_raw = nullptr;
DartPlantCurrentIdentity g_registered_current_identity = nullptr;

}  // namespace

bool dartplant_fixture_initialize_raw_bridge() {
    g_state.raw_to_handle = g_registered_raw_to_handle;
    g_state.handle_to_raw = g_registered_handle_to_raw;
    if (g_state.raw_to_handle == nullptr || g_state.handle_to_raw == nullptr) {
        g_state = {};
        g_state.backend = "unavailable-engine-shim-required";
        return false;
    }
    g_state.backend = "explicit-engine-shim";
    return true;
}

void dartplant_fixture_shutdown_raw_bridge() { g_state = {}; }

DartPlantRawToHandle dartplant_fixture_raw_to_handle() { return g_state.raw_to_handle; }

DartPlantHandleToRaw dartplant_fixture_handle_to_raw() { return g_state.handle_to_raw; }

const char* dartplant_fixture_raw_bridge_backend() { return g_state.backend; }

const char* dartplant_fixture_raw_bridge_build_id() { return ""; }

const char* dartplant_fixture_raw_bridge_engine_revision() { return "engine-shim"; }

bool dartplant_fixture_current_group_identity(uint64_t* group, uint64_t* generation) {
    return g_registered_current_identity != nullptr &&
           g_registered_current_identity(group, generation);
}

bool dartplant_fixture_current_group_matches(uint64_t group, uint64_t generation) {
    if (g_registered_current_identity == nullptr) return false;
    uint64_t current_group = 0;
    uint64_t current_generation = 0;
    return g_registered_current_identity(&current_group, &current_generation) &&
           current_group == group && current_generation == generation;
}

const char* dartplant_fixture_current_identity_backend() {
    return g_registered_current_identity == nullptr ? "unavailable" : "explicit-engine-shim";
}

void dartplant_fixture_set_engine_raw_handle_shim(DartPlantRawToHandle raw_to_handle,
                                                  DartPlantHandleToRaw handle_to_raw,
                                                  DartPlantCurrentIdentity current_identity) {
    g_registered_raw_to_handle = raw_to_handle;
    g_registered_handle_to_raw = handle_to_raw;
    g_registered_current_identity = current_identity;
}
