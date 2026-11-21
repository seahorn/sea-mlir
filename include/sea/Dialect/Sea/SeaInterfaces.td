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

#endif // SEA_INTERFACES
