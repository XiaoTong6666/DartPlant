#ifndef DARTPLANT_FIXTURE_DART_RAW_BRIDGE_H_
#define DARTPLANT_FIXTURE_DART_RAW_BRIDGE_H_

#include <stdint.h>

struct _Dart_Handle;
using DartPlantRawToHandle = _Dart_Handle* (*) (uint64_t raw);
using DartPlantHandleToRaw = uint64_t (*)(_Dart_Handle* handle);
using DartPlantCurrentIdentity = bool (*)(uint64_t* group, uint64_t* generation);

bool dartplant_fixture_initialize_raw_bridge();
void dartplant_fixture_shutdown_raw_bridge();
DartPlantRawToHandle dartplant_fixture_raw_to_handle();
DartPlantHandleToRaw dartplant_fixture_handle_to_raw();
const char* dartplant_fixture_raw_bridge_backend();
const char* dartplant_fixture_raw_bridge_build_id();
const char* dartplant_fixture_raw_bridge_engine_revision();

// Returns group identity only when an engine-owned shim registered it.
bool dartplant_fixture_current_group_identity(uint64_t* out_group, uint64_t* out_generation);
bool dartplant_fixture_current_group_matches(uint64_t group, uint64_t generation);
const char* dartplant_fixture_current_identity_backend();

// Called by an engine-owned C ABI shim before adapter initialization.
void dartplant_fixture_set_engine_raw_handle_shim(DartPlantRawToHandle raw_to_handle,
                                                  DartPlantHandleToRaw handle_to_raw,
                                                  DartPlantCurrentIdentity current_identity);

#endif  // DARTPLANT_FIXTURE_DART_RAW_BRIDGE_H_
