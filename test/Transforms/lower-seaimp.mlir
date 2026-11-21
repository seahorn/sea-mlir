// RUN: sea-opt %s --sea-lower-seaimp | FileCheck %s
// RUN: sea-opt %s --sea-lower-seaimp | FileCheck %s --check-prefix=NOIMP
// RUN: sea-opt %s --sea-vc-lower | FileCheck %s --check-prefix=NOIMP

// sea-lower-seaimp (Mem2Reg): no seaimp op is left.
// NOIMP-NOT: seaimp.

// x = a; if (a < b) x = b; assert(x >= a): the get at the join becomes a
// block argument fed by the set on each edge.
// CHECK-LABEL: sea.program @max
// CHECK: %[[A:.*]] = sea.nondet "nd_int" [2] : i32
// CHECK: %[[B:.*]] = sea.nondet "nd_int" [3] : i32
// CHECK: sea.cond_br %{{.*}}, ^[[T:.*]], ^[[J:.*]](%[[A]] : i32)
// CHECK: ^[[T]]:
// CHECK-NEXT: sea.br ^[[J]](%[[B]] : i32)
// CHECK: ^[[J]](%[[M:.*]]: i32):
// CHECK-NEXT: sea.icmp sge, %[[M]], %[[A]] : i32
sea.program @max {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.nondet "nd_int" [2] : i32
  %b = sea.nondet "nd_int" [3] : i32
  seaimp.set %x, %a : !seaimp.var<i32>
  %lt = sea.icmp slt, %a, %b : i32
  sea.cond_br %lt, ^bb1, ^bb2
^bb1:
  seaimp.set %x, %b : !seaimp.var<i32>
  sea.br ^bb2
^bb2:
  %m = seaimp.get %x : !seaimp.var<i32>
  %ok = sea.icmp sge, %m, %a : i32
  sea.assert %ok
  sea.halt
}

// A loop: the header gets a block argument, fed by the entry and by the
// back edge.
// CHECK-LABEL: sea.program @loop
// CHECK: %[[Z:.*]] = sea.const 0 : i32
// CHECK: sea.br ^[[H:.*]](%[[Z]] : i32)
// CHECK: ^[[H]](%[[I:.*]]: i32):
// CHECK: sea.cond_br %{{.*}}, ^[[BODY:.*]], ^[[EXIT:[a-z0-9]*]]
// CHECK: ^[[BODY]]:
// CHECK: %[[W:.*]] = sea.add %[[I]], %{{.*}} : i32
// CHECK-NEXT: sea.br ^[[H]](%[[W]] : i32)
// CHECK: ^[[EXIT]]:
// CHECK-NEXT: sea.icmp eq, %[[I]], %{{.*}} : i32
sea.program @loop {
  %i = seaimp.var "i" [1] : !seaimp.var<i32>
  %z = sea.const 0 : i32
  seaimp.set %i, %z : !seaimp.var<i32>
  sea.br ^head
^head:
  %v = seaimp.get %i : !seaimp.var<i32>
  %n = sea.const 10 : i32
  %c = sea.icmp ult, %v, %n : i32
  sea.cond_br %c, ^body, ^exit
^body:
  %one = sea.const 1 : i32
  %w = sea.add %v, %one : i32
  seaimp.set %i, %w : !seaimp.var<i32>
  sea.br ^head
^exit:
  %e = seaimp.get %i : !seaimp.var<i32>
  %ok = sea.icmp eq, %e, %n : i32
  sea.assert %ok
  sea.halt
}

// A read before any set is the variable's initial value: one nondet with
// the variable's name and site, at the start of the entry block, the same
// value at every such read (here: the entry value of the loop's join and
// the read on the path around the set).
// CHECK-LABEL: sea.program @read_before_set
// CHECK-NEXT: %[[X0:.*]] = sea.nondet "x" [5] : i8
// CHECK: sea.cond_br %{{.*}}, ^[[S:.*]], ^[[J:.*]](%[[X0]] : i8)
// CHECK: ^[[S]]:
// CHECK: sea.br ^[[J]](%{{.*}} : i8)
// CHECK: ^[[J]](%[[XJ:.*]]: i8):
// CHECK-NEXT: sea.icmp ule, %[[X0]], %[[XJ]] : i8
sea.program @read_before_set {
  %x = seaimp.var "x" [5] : !seaimp.var<i8>
  %c = sea.nondet "nd_bool" [6] : i1
  %first = seaimp.get %x : !seaimp.var<i8>
  sea.cond_br %c, ^set, ^join
^set:
  %k = sea.const -1 : i8
  seaimp.set %x, %k : !seaimp.var<i8>
  sea.br ^join
^join:
  %v = seaimp.get %x : !seaimp.var<i8>
  %ok = sea.icmp ule, %first, %v : i8
  sea.assert %ok
  sea.halt
}

// Two variables (a swap): each is promoted on its own; the initial value
// of y is read (no set before the swap), x is set first.
// CHECK-LABEL: sea.program @two_vars
// CHECK-NEXT: %[[Y0:.*]] = sea.nondet "y" [2] : i32
// CHECK-NOT: sea.nondet "x"
// CHECK: %[[A:.*]] = sea.nondet "nd" [3] : i32
// CHECK-NEXT: sea.icmp eq, %[[Y0]], %[[Y0]] : i32
// CHECK-NEXT: sea.icmp eq, %[[A]], %[[A]] : i32
sea.program @two_vars {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %y = seaimp.var "y" [2] : !seaimp.var<i32>
  %a = sea.nondet "nd" [3] : i32
  seaimp.set %x, %a : !seaimp.var<i32>
  %xv = seaimp.get %x : !seaimp.var<i32>
  %yv = seaimp.get %y : !seaimp.var<i32>
  seaimp.set %x, %yv : !seaimp.var<i32>
  seaimp.set %y, %xv : !seaimp.var<i32>
  %x2 = seaimp.get %x : !seaimp.var<i32>
  %y2 = seaimp.get %y : !seaimp.var<i32>
  %ok1 = sea.icmp eq, %x2, %yv : i32
  %ok2 = sea.icmp eq, %y2, %a : i32
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

// Nested branches: the inner join and the outer join each get one block
// argument; the branch that does not set passes the value from before.
// CHECK-LABEL: sea.program @nested
// CHECK: %[[Z:.*]] = sea.const 0 : i32
// CHECK: sea.cond_br %{{.*}}, ^[[OUTER:.*]], ^[[OJ:.*]](%[[Z]] : i32)
// CHECK: ^[[OUTER]]:
// CHECK: sea.cond_br %{{.*}}, ^[[INNER:.*]], ^[[IJ:.*]](%[[Z]] : i32)
// CHECK: ^[[INNER]]:
// CHECK-NEXT: sea.br ^[[IJ]](%[[ONE:.*]] : i32)
// CHECK: ^[[IJ]](%[[V1:.*]]: i32):
// CHECK-NEXT: sea.br ^[[OJ]](%[[V1]] : i32)
// CHECK: ^[[OJ]](%[[V2:.*]]: i32):
// CHECK-NEXT: sea.icmp ule, %[[V2]], %{{.*}} : i32
sea.program @nested {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %z = sea.const 0 : i32
  %one = sea.const 1 : i32
  seaimp.set %x, %z : !seaimp.var<i32>
  %c1 = sea.nondet "nd_bool" [2] : i1
  %c2 = sea.nondet "nd_bool" [3] : i1
  sea.cond_br %c1, ^outer, ^outer_join
^outer:
  sea.cond_br %c2, ^inner, ^inner_join
^inner:
  seaimp.set %x, %one : !seaimp.var<i32>
  sea.br ^inner_join
^inner_join:
  sea.br ^outer_join
^outer_join:
  %v = seaimp.get %x : !seaimp.var<i32>
  %ok = sea.icmp ule, %v, %one : i32
  sea.assert %ok
  sea.halt
}

// The initial values of struct and pointer variables.
// CHECK-LABEL: sea.program @initial_values
// CHECK-DAG: sea.struct.nondet "s" [1] : !sea.struct<i8, !sea.ptr<64>>
// CHECK-DAG: sea.ptr.nondet "p" [2] : !sea.ptr<64>
sea.program @initial_values {
  %s = seaimp.var "s" [1] : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  %p = seaimp.var "p" [2] : !seaimp.var<!sea.ptr<64>>
  %sv = seaimp.get %s : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  %pv = seaimp.get %p : !seaimp.var<!sea.ptr<64>>
  %f = sea.struct.extract %sv[1] : !sea.struct<i8, !sea.ptr<64>> -> !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %f, %pv : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// A variable that is never read leaves no nondet.
// CHECK-LABEL: sea.program @unread
// CHECK-NOT: sea.nondet
// CHECK: sea.halt
sea.program @unread {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.const 3 : i32
  seaimp.set %x, %a : !seaimp.var<i32>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
