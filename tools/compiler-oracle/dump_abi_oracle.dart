// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

// Run this file with the Dart executable from the *same SDK checkout* that
// produced the transformed AOT input. It deliberately reads only compiler-side
// vm.unboxing-info.metadata; source-language types are never used to guess a
// representation.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_from_binary.dart'
    show BinaryBuilderWithMetadata;
import 'package:vm/metadata/unboxing_info.dart';

String representationName(UnboxingType type, {required bool isReturn}) {
  switch (type.kind) {
    case UnboxingKind.boxed:
      return 'tagged';
    case UnboxingKind.int:
      return 'unboxed-int64';
    case UnboxingKind.double:
      return 'unboxed-double';
    case UnboxingKind.record:
      return isReturn ? 'pair-of-tagged' : 'unknown';
    case UnboxingKind.unknown:
      return 'unknown';
  }
}

Map<String, Object?>? encodeMember(
  Library library,
  String className,
  Member member,
  UnboxingInfoMetadataRepository repository,
) {
  final String memberName;
  final FunctionNode function;
  if (member is Procedure) {
    memberName = member.name.text;
    function = member.function;
  } else if (member is Constructor) {
    memberName = member.name.text.isEmpty ? '<init>' : member.name.text;
    function = member.function;
  } else {
    return null;
  }

  final metadata = repository.mapping[member];
  // UnboxingInfoMetadata.isTrivial metadata is intentionally omitted from the
  // transformed Kernel binary. In the matching compiler pipeline absence is
  // therefore exact evidence for boxed fixed parameters/result with default
  // register-CC flags, not "unknown".
  final requiredUserParameters = function.requiredParameterCount;
  final hasReceiver = member.isInstanceMember;
  final parameters = <String>[
    if (hasReceiver) 'tagged',
    for (var index = 0; index < requiredUserParameters; index++)
      metadata == null || index >= metadata.argsInfo.length
          ? 'tagged'
          : representationName(metadata.argsInfo[index], isReturn: false),
  ];
  final resultRepresentation = metadata == null
      ? 'tagged'
      : representationName(metadata.returnInfo, isReturn: true);
  final mustUseStack = metadata?.mustUseStackCallingConvention ?? false;
  final hasOverrides = metadata?.hasOverridesWithLessDirectParameters ?? false;
  final fixedParameterCount = requiredUserParameters + (hasReceiver ? 1 : 0);
  final totalUserParameters =
      function.positionalParameters.length + function.namedParameters.length;
  final hasOptionalParameters = totalUserParameters != requiredUserParameters;

  // Mirrors Function::MaxNumberOfParametersInRegisters() for the ordinary
  // Kernel members enumerated by this producer. Closures/dispatchers are not
  // emitted here and therefore cannot accidentally inherit this rule.
  final int maxParametersInRegisters;
  if (function.typeParameters.isNotEmpty || mustUseStack) {
    maxParametersInRegisters = 0;
  } else if (member is Procedure && (member.isGetter || member.isSetter)) {
    maxParametersInRegisters = fixedParameterCount;
  } else if (hasOverrides) {
    maxParametersInRegisters = (metadata?.argsInfo.length ?? 0) + 1;
  } else {
    maxParametersInRegisters = fixedParameterCount;
  }

  return <String, Object?>{
    'library_uri': library.importUri.toString(),
    'class_name': className,
    'function_name': memberName,
    'metadata_present': metadata != null,
    'parameters': parameters,
    'result': resultRepresentation,
    'implicit_parameter_count': hasReceiver ? 1 : 0,
    'fixed_parameter_count': fixedParameterCount,
    'has_optional_parameters': hasOptionalParameters,
    'must_use_stack_calling_convention': mustUseStack,
    'has_overrides_with_less_direct_parameters': hasOverrides,
    'max_parameters_in_registers': maxParametersInRegisters,
  };
}

void main(List<String> arguments) {
  if (arguments.length != 2) {
    stderr.writeln(
      'usage: dart dump_abi_oracle.dart <transformed.dill> <oracle.json>',
    );
    exitCode = 64;
    return;
  }

  final input = File(arguments[0]);
  if (!input.isFileSync()) {
    stderr.writeln('compiler oracle input not found: ${input.path}');
    exitCode = 66;
    return;
  }

  final component = Component();
  final repository = UnboxingInfoMetadataRepository();
  component.addMetadataRepository(repository);
  BinaryBuilderWithMetadata(Uint8List.fromList(input.readAsBytesSync()))
      .readComponent(component);

  final functions = <Map<String, Object?>>[];
  for (final library in component.libraries) {
    for (final procedure in library.procedures) {
      final encoded = encodeMember(library, '', procedure, repository);
      if (encoded != null) functions.add(encoded);
    }
    for (final clazz in library.classes) {
      for (final constructor in clazz.constructors) {
        final encoded =
            encodeMember(library, clazz.name, constructor, repository);
        if (encoded != null) functions.add(encoded);
      }
      for (final procedure in clazz.procedures) {
        final encoded =
            encodeMember(library, clazz.name, procedure, repository);
        if (encoded != null) functions.add(encoded);
      }
    }
  }

  final output = <String, Object?>{
    'format': 1,
    'source': 'vm.unboxing-info.metadata',
    'input_dill': input.path,
    'functions': functions,
  };
  File(arguments[1]).writeAsStringSync(
    const JsonEncoder.withIndent('  ').convert(output),
  );
}
