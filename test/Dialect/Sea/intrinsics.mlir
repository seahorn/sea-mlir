// RUN: sea-opt %s | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @intrinsics
sea.program @intrinsics {
  %a = sea.nondet "nondet_u16" [0] : i16
  %b = sea.nondet "nondet_u16" [1] : i16
  // CHECK: sea.umin %{{.*}}, %{{.*}} : i16
  %0 = sea.umin %a, %b : i16
  // CHECK: sea.umax
  %1 = sea.umax %a, %b : i16
  // CHECK: sea.smin
  %2 = sea.smin %a, %b : i16
  // CHECK: sea.smax
  %3 = sea.smax %a, %b : i16
  // CHECK: sea.bswap %{{.*}} : i16
  %4 = sea.bswap %a : i16
  // CHECK: sea.ctpop %{{.*}} : i16
  %ctp = sea.ctpop %a : i16
  // CHECK: sea.abs %{{.*}} : i16
  %abs = sea.abs %a : i16
  // CHECK: sea.abs %{{.*}} nsw : i16
  %absn = sea.abs %a nsw : i16
  // CHECK: sea.fshl %{{.*}}, %{{.*}}, %{{.*}} : i16
  %5 = sea.fshl %a, %b, %0 : i16
  // CHECK: sea.fshr
  %6 = sea.fshr %a, %b, %0 : i16
  // CHECK: sea.uaddo %{{.*}}, %{{.*}} : i16
  %7 = sea.uaddo %a, %b : i16
  // CHECK: sea.saddo
  %8 = sea.saddo %a, %b : i16
  // CHECK: sea.usubo
  %9 = sea.usubo %a, %b : i16
  // CHECK: sea.ssubo
  %10 = sea.ssubo %a, %b : i16
  // CHECK: sea.umulo
  %11 = sea.umulo %a, %b : i16
  // CHECK: sea.smulo
  %12 = sea.smulo %a, %b : i16
  // CHECK: sea.sdivo
  %13 = sea.sdivo %a, %b : i16
  // CHECK: sea.nego %{{.*}} : i16
  %14 = sea.nego %a : i16
  // CHECK: sea.add %{{.*}}, %{{.*}} nsw nuw : i16
  %15 = sea.add %a, %b nsw nuw : i16
  // CHECK: sea.shl %{{.*}}, %{{.*}} nuw : i16
  %16 = sea.shl %a, %b nuw : i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
