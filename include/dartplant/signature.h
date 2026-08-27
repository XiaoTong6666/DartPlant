// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_SIGNATURE_H_
#define DARTPLANT_SIGNATURE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DARTPLANT_DART_PARAMETER_NAME_MAX 160

typedef enum DartPlantDartTypeKind {
    DARTPLANT_DART_TYPE_UNKNOWN = 0,
    DARTPLANT_DART_TYPE_NULL,
    DARTPLANT_DART_TYPE_DYNAMIC,
    DARTPLANT_DART_TYPE_VOID,
    DARTPLANT_DART_TYPE_NEVER,
    DARTPLANT_DART_TYPE_INTERFACE,
    DARTPLANT_DART_TYPE_FUNCTION,
    DARTPLANT_DART_TYPE_RECORD,
    DARTPLANT_DART_TYPE_PARAMETER,
} DartPlantDartTypeKind;

typedef enum DartPlantDartNullability {
    DARTPLANT_DART_NULLABILITY_UNKNOWN = 0,
    DARTPLANT_DART_NULLABILITY_NULLABLE,
    DARTPLANT_DART_NULLABILITY_NON_NULLABLE,
    DARTPLANT_DART_NULLABILITY_LEGACY,
} DartPlantDartNullability;

typedef enum DartPlantDartParameterKind {
    DARTPLANT_DART_PARAMETER_IMPLICIT = 0,
    DARTPLANT_DART_PARAMETER_REQUIRED_POSITIONAL,
    DARTPLANT_DART_PARAMETER_OPTIONAL_POSITIONAL,
    DARTPLANT_DART_PARAMETER_NAMED,
} DartPlantDartParameterKind;

typedef struct DartPlantDartTypeInfo {
    uint32_t struct_size;
    DartPlantDartTypeKind kind;
    DartPlantDartNullability nullability;
    // CID of the AbstractType object itself. This is diagnostic VM identity,
    // not the class represented by an interface Type.
    uint32_t object_cid;
    // For DARTPLANT_DART_TYPE_INTERFACE, the ClassId encoded by Type.flags.
    // Other kinds report zero and must not be inferred from this field.
    uint32_t type_class_id;
    // Only meaningful for DARTPLANT_DART_TYPE_PARAMETER. base is the number of
    // enclosing function type parameters and index is the VM's finalized type
    // argument position. Together with is_function_type_parameter these retain
    // the semantic identity required to distinguish T/U-like parameters.
    uint32_t type_parameter_base;
    uint32_t type_parameter_index;
    uint8_t is_function_type_parameter;
    uint8_t reserved_flags[3];
} DartPlantDartTypeInfo;

typedef struct DartPlantDartFunctionSignatureInfo {
    uint32_t struct_size;
    // FunctionType formal parameters only, including VM-only implicit formals.
    // parameter_count is exactly fixed_parameter_count + optional_parameter_count.
    // It does not include hidden machine-call transport such as a generic
    // function's type-arguments vector or an ArgumentsDescriptor.
    uint32_t parameter_count;
    uint32_t implicit_parameter_count;
    uint32_t fixed_parameter_count;
    uint32_t optional_parameter_count;
    uint32_t type_parameter_count;
    uint32_t parent_type_argument_count;
    uint8_t has_named_optional_parameters;
    uint8_t reserved_flags[3];
    DartPlantDartTypeInfo result_type;
} DartPlantDartFunctionSignatureInfo;

typedef struct DartPlantDartParameterInfo {
    uint32_t struct_size;
    uint32_t index;
    DartPlantDartParameterKind kind;
    // True for required positional parameters and `required` named parameters.
    // VM-only implicit parameters report false.
    uint8_t is_required;
    uint8_t reserved_flags[3];
    DartPlantDartTypeInfo type;
    // AOT retains names for named parameters in FunctionType. Positional and
    // implicit parameter names are intentionally reported as an empty string.
    char name[DARTPLANT_DART_PARAMETER_NAME_MAX];
} DartPlantDartParameterInfo;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_SIGNATURE_H_
