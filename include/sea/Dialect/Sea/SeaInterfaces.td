// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaInterfaces.td - sea op interfaces ---------------*- tablegen -*-===//

#ifndef SEA_INTERFACES
#define SEA_INTERFACES

include "mlir/IR/OpBase.td"

def Sea_NondetSiteOpInterface : OpInterface<"NondetSiteOpInterface"> {
  let description = [{
    An op that names a program input by a nondet site (`[<site>]`): the
    nondet ops, memhavoc, allocations, and ops of dialects lowered to sea
    that become one of them (`seaimp.var`). `sea.program`'s verifier keeps
    the sites unique over all of them.
  }];
  let cppNamespace = "::sea";
  let methods = [
    InterfaceMethod<"The nondet site of the op.", "uint64_t", "getSite">,
  ];
}

def Sea_BoxBodyReadOpInterface : OpInterface<"BoxBodyReadOpInterface"> {
  let description = [{
    An op of a dialect lowered to sea that may stand in a `sea.box.all`
    body because it only reads memory defined outside the box: the region
    reads of seaimp (`seaimp.load`, `seaimp.is_modified`). Their lowering
    reads the memory before the box. The box's verifier admits them; their
    own verifier says what they read.
  }];
  let cppNamespace = "::sea";
}

#endif // SEA_INTERFACES
