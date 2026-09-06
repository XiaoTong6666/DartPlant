// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADAPTERS_FLUTTER_VM_H_
#define DARTPLANT_ADAPTERS_FLUTTER_VM_H_

#include <stdint.h>

#include "dartplant/dartplant.h"
#include "dartplant/vm_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARTPLANT_FLUTTER_VM_ADAPTER_API_VERSION 1u

typedef struct DartPlantFlutterVmAdapter DartPlantFlutterVmAdapter;

typedef struct DartPlantFlutterVmAdapterOptions {
    uint32_t struct_size;
    uint32_t api_version;
    void* api_dl_data;
    uint64_t thread;
    uint64_t isolate_generation;
    const char* snapshot_hash;
    const char* snapshot_features;
} DartPlantFlutterVmAdapterOptions;

typedef struct DartPlantFlutterVmDescriptor {
    uint32_t struct_size;
    uint32_t descriptor_version;
    uint32_t vm_adapter_version;
    const char* descriptor_id;
    const char* dart_version;
    const char* flutter_version;
    const char* snapshot_hash;
    const char* flutter_module_name;
    const char* flutter_build_id;
    uint32_t pointer_size;
    uint8_t compressed_pointers;
    uint8_t product_mode;
    uint8_t reserved[2];
} DartPlantFlutterVmDescriptor;

DARTPLANT_EXPORT DartPlantStatus dartplant_flutter_vm_adapter_create(
    const DartPlantFlutterVmAdapterOptions* options, DartPlantFlutterVmAdapter** out_instance);
DARTPLANT_EXPORT DartPlantVmAdapter* dartplant_flutter_vm_adapter_get(
    DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_adapter_descriptor(
    const DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_destroy(DartPlantFlutterVmAdapter* instance);
// Exact descriptors compiled into this adapter. The implementation is
// process-global because dart_api_dl itself is process-global; create returns
// VM_ADAPTER_BUSY while another instance is attached.
DARTPLANT_EXPORT uint32_t dartplant_flutter_vm_descriptor_count(void);
DARTPLANT_EXPORT const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_descriptor_at(
    uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
