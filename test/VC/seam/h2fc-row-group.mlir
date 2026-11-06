// Regression for the h2fc VC-generation blowup. Reduced from the Firedancer
// h2 / gRPC flow control export tx-proof-overlay (Apache-2.0) from 13,980 to
// about 330 lines: zero bytes written into one global at symbolic offsets, read at a symbolic
// offset. Without grouping every undecided write adds one ite level that
// bv.ite-merge folds into a growing disjunction (quadratic); with it the
// run of same-value writes is one disjunction built at once
// (row.smap-group), and no ite-merge fires. Counters, not time.
// RUN: sea-vc %s --lower --lower-opt=false --seam --widen-memory=4 --vcgen-only --seam-stats --seam-row-group=0 | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --lower --lower-opt=false --seam --widen-memory=4 --vcgen-only --seam-stats | FileCheck %s --check-prefix=ON
// OFF: seam.rule.bv.ite-merge: {{[1-9][0-9]+}}
// ON-NOT: bv.ite-merge
// ON: seam.rule.row.smap-group: {{[1-9][0-9]+}}
// ON-NOT: bv.ite-merge
module {
  sea.program @main {
    %0 = sea.mem.nondet "region.8" [4] : !sea.mem<!sea.ptr<64>, i8>
    %1 = sea.mem.nondet "region.0" [5] : !sea.mem<!sea.ptr<64>, i8>
    %2 = sea.global @st size 2256 align 16 : !sea.ptr<64>
    %3 = sea.const 2140 : i64
    %4 = sea.ptr.add %2, %3 : !sea.ptr<64>, i64
    %5 = sea.const 0 : i8
    %6 = sea.store %0[%4], %5 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %7 = sea.const 2141 : i64
    %8 = sea.ptr.add %2, %7 : !sea.ptr<64>, i64
    %9 = sea.const 0 : i8
    %10 = sea.store %6[%8], %9 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %11 = sea.const 2142 : i64
    %12 = sea.ptr.add %2, %11 : !sea.ptr<64>, i64
    %13 = sea.const 0 : i8
    %14 = sea.store %10[%12], %13 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %15 = sea.const 2143 : i64
    %16 = sea.ptr.add %2, %15 : !sea.ptr<64>, i64
    %17 = sea.const 0 : i8
    %18 = sea.store %14[%16], %17 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %19 = sea.const 0 : i8
    %20 = sea.store %18[%2], %19 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %21 = sea.const 2148 : i64
    %22 = sea.ptr.add %2, %21 : !sea.ptr<64>, i64
    %23 = sea.const 0 : i8
    %24 = sea.store %20[%22], %23 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %25 = sea.const 2149 : i64
    %26 = sea.ptr.add %2, %25 : !sea.ptr<64>, i64
    %27 = sea.const 0 : i8
    %28 = sea.store %24[%26], %27 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %29 = sea.const 2150 : i64
    %30 = sea.ptr.add %2, %29 : !sea.ptr<64>, i64
    %31 = sea.const 0 : i8
    %32 = sea.store %28[%30], %31 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %33 = sea.const 2151 : i64
    %34 = sea.ptr.add %2, %33 : !sea.ptr<64>, i64
    %35 = sea.const 0 : i8
    %36 = sea.store %32[%34], %35 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %37 = sea.const 0 : i8
    %38 = sea.store %36[%2], %37 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %39 = sea.const 2156 : i64
    %40 = sea.ptr.add %2, %39 : !sea.ptr<64>, i64
    %41 = sea.const 0 : i8
    %42 = sea.store %38[%40], %41 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %43 = sea.const 2157 : i64
    %44 = sea.ptr.add %2, %43 : !sea.ptr<64>, i64
    %45 = sea.const 0 : i8
    %46 = sea.store %42[%44], %45 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %47 = sea.const 2158 : i64
    %48 = sea.ptr.add %2, %47 : !sea.ptr<64>, i64
    %49 = sea.const 0 : i8
    %50 = sea.store %46[%48], %49 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %51 = sea.const 2159 : i64
    %52 = sea.ptr.add %2, %51 : !sea.ptr<64>, i64
    %53 = sea.const 0 : i8
    %54 = sea.store %50[%52], %53 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %55 = sea.const 2164 : i64
    %56 = sea.ptr.add %2, %55 : !sea.ptr<64>, i64
    %57 = sea.const 0 : i8
    %58 = sea.store %54[%56], %57 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %59 = sea.const 2165 : i64
    %60 = sea.ptr.add %2, %59 : !sea.ptr<64>, i64
    %61 = sea.const 0 : i8
    %62 = sea.store %58[%60], %61 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %63 = sea.const 2166 : i64
    %64 = sea.ptr.add %2, %63 : !sea.ptr<64>, i64
    %65 = sea.const 0 : i8
    %66 = sea.store %62[%64], %65 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %67 = sea.const 2167 : i64
    %68 = sea.ptr.add %2, %67 : !sea.ptr<64>, i64
    %69 = sea.const 0 : i8
    %70 = sea.store %66[%68], %69 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %71 = sea.const 0 : i8
    %72 = sea.store %70[%2], %71 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %73 = sea.const 2172 : i64
    %74 = sea.ptr.add %2, %73 : !sea.ptr<64>, i64
    %75 = sea.const 0 : i8
    %76 = sea.store %72[%74], %75 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %77 = sea.const 2173 : i64
    %78 = sea.ptr.add %2, %77 : !sea.ptr<64>, i64
    %79 = sea.const 0 : i8
    %80 = sea.store %76[%78], %79 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %81 = sea.const 2174 : i64
    %82 = sea.ptr.add %2, %81 : !sea.ptr<64>, i64
    %83 = sea.const 0 : i8
    %84 = sea.store %80[%82], %83 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %85 = sea.const 2175 : i64
    %86 = sea.ptr.add %2, %85 : !sea.ptr<64>, i64
    %87 = sea.const 0 : i8
    %88 = sea.store %84[%86], %87 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %89 = sea.const 2180 : i64
    %90 = sea.ptr.add %2, %89 : !sea.ptr<64>, i64
    %91 = sea.const 0 : i8
    %92 = sea.store %88[%90], %91 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %93 = sea.const 2181 : i64
    %94 = sea.ptr.add %2, %93 : !sea.ptr<64>, i64
    %95 = sea.const 0 : i8
    %96 = sea.store %92[%94], %95 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %97 = sea.const 2182 : i64
    %98 = sea.ptr.add %2, %97 : !sea.ptr<64>, i64
    %99 = sea.const 0 : i8
    %100 = sea.store %96[%98], %99 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %101 = sea.const 2183 : i64
    %102 = sea.ptr.add %2, %101 : !sea.ptr<64>, i64
    %103 = sea.const 0 : i8
    %104 = sea.store %100[%102], %103 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %105 = sea.const 2188 : i64
    %106 = sea.ptr.add %2, %105 : !sea.ptr<64>, i64
    %107 = sea.const 0 : i8
    %108 = sea.store %104[%106], %107 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %109 = sea.const 2189 : i64
    %110 = sea.ptr.add %2, %109 : !sea.ptr<64>, i64
    %111 = sea.const 0 : i8
    %112 = sea.store %108[%110], %111 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %113 = sea.const 2190 : i64
    %114 = sea.ptr.add %2, %113 : !sea.ptr<64>, i64
    %115 = sea.const 0 : i8
    %116 = sea.store %112[%114], %115 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %117 = sea.const 2191 : i64
    %118 = sea.ptr.add %2, %117 : !sea.ptr<64>, i64
    %119 = sea.const 0 : i8
    %120 = sea.store %116[%118], %119 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %121 = sea.const 2196 : i64
    %122 = sea.ptr.add %2, %121 : !sea.ptr<64>, i64
    %123 = sea.const 0 : i8
    %124 = sea.store %120[%122], %123 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %125 = sea.const 2197 : i64
    %126 = sea.ptr.add %2, %125 : !sea.ptr<64>, i64
    %127 = sea.const 0 : i8
    %128 = sea.store %124[%126], %127 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %129 = sea.const 2198 : i64
    %130 = sea.ptr.add %2, %129 : !sea.ptr<64>, i64
    %131 = sea.const 0 : i8
    %132 = sea.store %128[%130], %131 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %133 = sea.const 2199 : i64
    %134 = sea.ptr.add %2, %133 : !sea.ptr<64>, i64
    %135 = sea.const 0 : i8
    %136 = sea.store %132[%134], %135 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %137 = sea.const 2204 : i64
    %138 = sea.ptr.add %2, %137 : !sea.ptr<64>, i64
    %139 = sea.const 0 : i8
    %140 = sea.store %136[%138], %139 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %141 = sea.const 2205 : i64
    %142 = sea.ptr.add %2, %141 : !sea.ptr<64>, i64
    %143 = sea.const 0 : i8
    %144 = sea.store %140[%142], %143 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %145 = sea.const 2206 : i64
    %146 = sea.ptr.add %2, %145 : !sea.ptr<64>, i64
    %147 = sea.const 0 : i8
    %148 = sea.store %144[%146], %147 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %149 = sea.const 2207 : i64
    %150 = sea.ptr.add %2, %149 : !sea.ptr<64>, i64
    %151 = sea.const 0 : i8
    %152 = sea.store %148[%150], %151 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %153 = sea.const 2212 : i64
    %154 = sea.ptr.add %2, %153 : !sea.ptr<64>, i64
    %155 = sea.const 0 : i8
    %156 = sea.store %152[%154], %155 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %157 = sea.const 2213 : i64
    %158 = sea.ptr.add %2, %157 : !sea.ptr<64>, i64
    %159 = sea.const 0 : i8
    %160 = sea.store %156[%158], %159 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %161 = sea.const 2214 : i64
    %162 = sea.ptr.add %2, %161 : !sea.ptr<64>, i64
    %163 = sea.const 0 : i8
    %164 = sea.store %160[%162], %163 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %165 = sea.const 2215 : i64
    %166 = sea.ptr.add %2, %165 : !sea.ptr<64>, i64
    %167 = sea.const 0 : i8
    %168 = sea.store %164[%166], %167 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %169 = sea.const 2220 : i64
    %170 = sea.ptr.add %2, %169 : !sea.ptr<64>, i64
    %171 = sea.const 0 : i8
    %172 = sea.store %168[%170], %171 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %173 = sea.const 2221 : i64
    %174 = sea.ptr.add %2, %173 : !sea.ptr<64>, i64
    %175 = sea.const 0 : i8
    %176 = sea.store %172[%174], %175 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %177 = sea.const 2222 : i64
    %178 = sea.ptr.add %2, %177 : !sea.ptr<64>, i64
    %179 = sea.const 0 : i8
    %180 = sea.store %176[%178], %179 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %181 = sea.const 2223 : i64
    %182 = sea.ptr.add %2, %181 : !sea.ptr<64>, i64
    %183 = sea.const 0 : i8
    %184 = sea.store %180[%182], %183 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %185 = sea.const 2228 : i64
    %186 = sea.ptr.add %2, %185 : !sea.ptr<64>, i64
    %187 = sea.const 0 : i8
    %188 = sea.store %184[%186], %187 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %189 = sea.const 2229 : i64
    %190 = sea.ptr.add %2, %189 : !sea.ptr<64>, i64
    %191 = sea.const 0 : i8
    %192 = sea.store %188[%190], %191 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %193 = sea.const 2230 : i64
    %194 = sea.ptr.add %2, %193 : !sea.ptr<64>, i64
    %195 = sea.const 0 : i8
    %196 = sea.store %192[%194], %195 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %197 = sea.const 2231 : i64
    %198 = sea.ptr.add %2, %197 : !sea.ptr<64>, i64
    %199 = sea.const 0 : i8
    %200 = sea.store %196[%198], %199 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %201 = sea.const 2236 : i64
    %202 = sea.ptr.add %2, %201 : !sea.ptr<64>, i64
    %203 = sea.const 0 : i8
    %204 = sea.store %200[%202], %203 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %205 = sea.const 2237 : i64
    %206 = sea.ptr.add %2, %205 : !sea.ptr<64>, i64
    %207 = sea.const 0 : i8
    %208 = sea.store %204[%206], %207 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %209 = sea.const 2238 : i64
    %210 = sea.ptr.add %2, %209 : !sea.ptr<64>, i64
    %211 = sea.const 0 : i8
    %212 = sea.store %208[%210], %211 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %213 = sea.const 2239 : i64
    %214 = sea.ptr.add %2, %213 : !sea.ptr<64>, i64
    %215 = sea.const 0 : i8
    %216 = sea.store %212[%214], %215 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %217 = sea.const 0 : i8
    %218 = sea.store %216[%2], %217 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %219 = sea.const 2244 : i64
    %220 = sea.ptr.add %2, %219 : !sea.ptr<64>, i64
    %221 = sea.const 0 : i8
    %222 = sea.store %218[%220], %221 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %223 = sea.const 2245 : i64
    %224 = sea.ptr.add %2, %223 : !sea.ptr<64>, i64
    %225 = sea.const 0 : i8
    %226 = sea.store %222[%224], %225 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %227 = sea.const 2246 : i64
    %228 = sea.ptr.add %2, %227 : !sea.ptr<64>, i64
    %229 = sea.const 0 : i8
    %230 = sea.store %226[%228], %229 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
    %231 = sea.const true
    %232 = sea.const true
    sea.br ^bb1(%232 : i1)
  ^bb1(%233: i1):  // pred: ^bb0
    sea.br ^bb2
  ^bb2:  // pred: ^bb1
    %234 = sea.nondet "nd_ulong" [27] : i64
    sea.br ^bb3(%231, %234 : i1, i64)
  ^bb3(%235: i1, %236: i64):  // pred: ^bb2
    sea.br ^bb4
  ^bb4:  // pred: ^bb3
    %237 = sea.nondet "nd_ulong" [31] : i64
    sea.br ^bb5(%235, %237 : i1, i64)
  ^bb5(%238: i1, %239: i64):  // pred: ^bb4
    %240 = sea.const 0 : i64
    sea.br ^bb6(%240 : i64)
  ^bb6(%241: i64):  // pred: ^bb5
    sea.br ^bb7
  ^bb7:  // pred: ^bb6
    %242 = sea.const 0 : i64
    sea.br ^bb8(%242 : i64)
  ^bb8(%243: i64):  // pred: ^bb7
    sea.br ^bb9
  ^bb9:  // pred: ^bb8
    %244 = sea.const 0 : i64
    sea.br ^bb10(%244 : i64)
  ^bb10(%245: i64):  // pred: ^bb9
    sea.br ^bb11
  ^bb11:  // pred: ^bb10
    %246 = sea.nondet "nd_long" [38] : i64
    %247 = sea.nondet "sea.reduce" [51] : i64
    sea.br ^bb12(%247, %246 : i64, i64)
  ^bb12(%248: i64, %249: i64):  // pred: ^bb11
    sea.br ^bb13
  ^bb13:  // pred: ^bb12
    sea.br ^bb14
  ^bb14:  // pred: ^bb13
    %250 = sea.const 0 : i64
    sea.br ^bb15
  ^bb15:  // pred: ^bb14
    %251 = sea.const 0 : i64
    sea.br ^bb16(%251 : i64)
  ^bb16(%252: i64):  // pred: ^bb15
    sea.br ^bb17(%250 : i64)
  ^bb17(%253: i64):  // pred: ^bb16
    sea.br ^bb18
  ^bb18:  // pred: ^bb17
    sea.br ^bb19
  ^bb19:  // pred: ^bb18
    sea.br ^bb20
  ^bb20:  // pred: ^bb19
    %254 = sea.const 0 : i64
    sea.br ^bb21(%254 : i64)
  ^bb21(%255: i64):  // pred: ^bb20
    sea.br ^bb22
  ^bb22:  // pred: ^bb21
    %256 = sea.const false
    sea.br ^bb23(%256 : i1)
  ^bb23(%257: i1):  // pred: ^bb22
    %258 = sea.nondet "nd_ulong" [43] : i64
    %259 = sea.global @st size 2256 align 16 : !sea.ptr<64>
    %260 = sea.ptr.index %259[%258] strides [1128] offset 0 inbounds : !sea.ptr<64>, i64
    %261 = sea.ptr.index %260[] strides [] offset 4 inbounds : !sea.ptr<64>
    %262 = sea.load %230[%261] align 4 : !sea.mem<!sea.ptr<64>, i8> -> i32
    sea.br ^bb24
  ^bb24:  // pred: ^bb23
    sea.br ^bb25
  ^bb25:  // pred: ^bb24
    sea.br ^bb26
  ^bb26:  // pred: ^bb25
    %263 = sea.nondet "sea.reduce" [57] : i64
    %264 = sea.const 0 : i64
    sea.br ^bb27(%238, %230, %263, %264 : i1, !sea.mem<!sea.ptr<64>, i8>, i64, i64)
  ^bb27(%265: i1, %266: !sea.mem<!sea.ptr<64>, i8>, %267: i64, %268: i64):  // pred: ^bb26
    sea.br ^bb28
  ^bb28:  // pred: ^bb27
    sea.br ^bb29
  ^bb29:  // pred: ^bb28
    sea.br ^bb30
  ^bb30:  // pred: ^bb29
    %269 = sea.const 0 : i64
    sea.br ^bb31(%269 : i64)
  ^bb31(%270: i64):  // pred: ^bb30
    sea.br ^bb32
  ^bb32:  // pred: ^bb31
    %271 = sea.const true
    sea.br ^bb33(%271 : i1)
  ^bb33(%272: i1):  // pred: ^bb32
    sea.br ^bb34(%265, %1, %266 : i1, !sea.mem<!sea.ptr<64>, i8>, !sea.mem<!sea.ptr<64>, i8>)
  ^bb34(%273: i1, %274: !sea.mem<!sea.ptr<64>, i8>, %275: !sea.mem<!sea.ptr<64>, i8>):  // pred: ^bb33
    sea.br ^bb35
  ^bb35:  // pred: ^bb34
    %276 = sea.const false
    sea.assert %276
    sea.halt
  }
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
