#ifndef DARTPLANT_FIXTURE_DART_API_ADAPTER_H_
#define DARTPLANT_FIXTURE_DART_API_ADAPTER_H_

#include "dart_raw_bridge.h"
#include "dartplant/vm_adapter.h"

DartPlantStatus dartplant_fixture_create_dart_api_adapter(void* api_dl_data,
                                                          DartPlantVmAdapter** out_adapter);
DartPlantStatus dartplant_fixture_destroy_dart_api_adapter();
DartPlantVmAdapter* dartplant_fixture_dart_api_adapter();
bool dartplant_fixture_dart_api_adapter_ready_for_hooks();
void dartplant_fixture_set_raw_handle_bridge(DartPlantRawToHandle raw_to_handle,
                                             DartPlantHandleToRaw handle_to_raw,
                                             DartPlantCurrentIdentity current_identity);

#endif  // DARTPLANT_FIXTURE_DART_API_ADAPTER_H_
