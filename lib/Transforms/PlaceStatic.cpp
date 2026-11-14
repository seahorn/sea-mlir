// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PlaceStatic.cpp - static placement of objects ------------*- C++ -*-===//
//
// Lowers the abstract memory level (!sea.ptr) to the concrete one
// (!sea.addr) by placing every object at a constant address, as SeaHorn's
// static allocator does (lib/seahorn/BvOpSem2Allocators.cc, dev18 0cb925cd):
// functions from the text segment start (4 units each), then globals, then
// the heap; stack objects downwards from the stack top. An allocation whose
// size is not a constant gets `max-symbolic-size` units and an explicit
// sea.assume (or sea.assert) that its size fits. The placement is recorded
// as sea.segment ops at the start of the entry block; a segment's result is
// the object's base address, and every use of the object becomes that result
// (plus sea.addr.add for offsets), not a numeral: the object stays visible
// to the VC (SeaM) while the interpreter and the default VC see its base.
//
// Contract.
//   Accepts: abstract programs without struct values (scalarize first) and
//     with each allocation op executed at most once (loop-free).
//   Produces: no !sea.ptr anywhere (checked: sea::isAbstract is false);
//     a sea.segment per placed object (its result replaces the object's
//     uses); segments do not overlap. Every object has its own base: an
//     object of size 0 keeps size 0 in its segment but takes one unit of
//     address space, in every layout (no two segments share a base; C's
//     malloc(0) returns null or a unique pointer; a Rust zero-sized type or
//     dangling pointer is not a placed object here). Tool metadata keyed
//     by object identity (sea-lower-tracking's shadow cells at sea.ptr.base)
//     needs it (docs/design/SeaM-null.md, "Shadow memories"). The null pointer
//     is an object too (docs/design/SeaM-null.md): if the program has
//     sea.ptr.null, a segment "null" (kind null, base 0, size 0) whose result
//     replaces every sea.ptr.null; its value is 0, as the numeral was. Only
//     sea.ptr.null is null: an integer 0 cast to a pointer stays a number.
//   Errors: place.struct, place.reuse-unimplemented, and the size and
//     layout errors below.
//
// External placement (place-in / place-out, sea-vc --place-in /
// --place-out): place-out writes the placement computed, one object per
// line, `name=<n> kind=<k> base=<hex> size=<n> align=<n>` (`#` comments).
// place-in takes the bases from such a file instead of computing them:
// the pass is still the allocator and checks them as it places (kind, size
// and align equal to the object's own, base aligned, inside the address
// width, null at 0 and nothing else there, no overlap); a violation is an
// error (place.in-*). An object of the program the file does not name is
// an error (place.in-unplaced): the file's writer must have every object
// of the reader (a pass that creates objects is not supported); objects of
// the file the program does not have are holes. Objects are matched by
// their segment names (<kind>.<site>, global.<name>, func.<name>,
// nondet.<site>, null), unique in the program (place.duplicate-name). Two
// lowerings of one program that share a file place their common objects
// at the same bases (utils/cover-diff.py --level=1). With either file the
// module must hold one program (place.file-programs).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Support/WriteFile.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

// SeaHorn's layout constants (BvOpSem2Allocators.hh).
constexpr uint64_t kTextSegmentStart = 0x08048000;
constexpr uint64_t kMaxStackAddr = 0xC0000000;
constexpr uint64_t kMinStackAddr = kMaxStackAddr - 9437184;
// Every function gets this many units (SeaHorn's falloc).
constexpr uint64_t kFuncSize = 4;
// The null object's alignment: its base 0 is a multiple of anything.
constexpr uint64_t kNullAlign = uint64_t(1) << 62;

struct Object {
  Operation *op;     // the first op denoting the object (erased by step 2)
  Location loc;      // its location, kept for the segment op
  std::string name;  // segment name
  StringRef kind;    // stack, heap, global, func, null
  uint64_t room = 0; // units reserved
  uint64_t align = 1;
  bool symbolicSize = false;
  uint64_t base = 0;
  StringAttr label; // the producer's label (alloca / malloc / global)
};

class PlaceStaticPass
    : public PassWrapper<PlaceStaticPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PlaceStaticPass)

  PlaceStaticPass() = default;
  PlaceStaticPass(const PlaceStaticPass &other) : PassWrapper(other) {}
  explicit PlaceStaticPass(const PlaceFiles &files) {
    placeIn = files.in;
    placeOut = files.out;
  }

  StringRef getArgument() const final { return "sea-place-static"; }
  StringRef getDescription() const final {
    return "Place every object at a constant address (abstract pointers to "
           "addresses) and record the segment map";
  }

  Option<uint64_t> maxSymbolicSize{
      *this, "max-symbolic-size",
      llvm::cl::desc("Units reserved for an allocation of non-constant size"),
      llvm::cl::init(4096)};
  Option<std::string> symbolicSize{
      *this, "symbolic-size",
      llvm::cl::desc("How a non-constant size is bounded: assume, assert, "
                     "refuse"),
      llvm::cl::init("assume")};
  Option<std::string> layout{
      *this, "layout",
      llvm::cl::desc("seahorn (SeaHorn's segments; needs 32-bit or wider "
                     "pointers), compact (all objects upwards from 1), auto "
                     "(seahorn if it fits)"),
      llvm::cl::init("auto")};
  Option<bool> reuse{*this, "reuse",
                     llvm::cl::desc("Share space between objects with "
                                    "disjoint lifetimes (not implemented)"),
                     llvm::cl::init(false)};
  Option<std::string> placeIn{
      *this, "place-in",
      llvm::cl::desc("Take every object's base from this placement file "
                     "(written by place-out), checked; an object the file "
                     "does not name is an error"),
      llvm::cl::init("")};
  Option<std::string> placeOut{
      *this, "place-out",
      llvm::cl::desc("Write the placement to this file (one object per "
                     "line)"),
      llvm::cl::init("")};

  void runOnOperation() override;

private:
  LogicalResult collect(ProgramOp program, SmallVector<Object> &objects,
                        llvm::DenseMap<Operation *, unsigned> &objectOf);
  LogicalResult place(ProgramOp program, unsigned width,
                      SmallVector<Object> &objects);
  LogicalResult placeFromFile(ProgramOp program, unsigned width,
                              SmallVector<Object> &objects);
  LogicalResult writeFile(ProgramOp program, ArrayRef<Object> objects);
};

/// One object of a placement file.
struct Placed {
  std::string kind;
  uint64_t base = 0, size = 0, align = 0;
  unsigned line = 0;
};

/// The objects of a placement file by name; every malformed line is an
/// error (place.in-format).
static FailureOr<llvm::StringMap<Placed>> readPlacement(StringRef path,
                                                        Location loc) {
  auto buf = llvm::MemoryBuffer::getFile(path, /*IsText=*/true);
  if (!buf) {
    diag::error("place.in-file",
                "sea-place-static: cannot read the placement file '" + path +
                    "': " + buf.getError().message(),
                loc);
    return failure();
  }
  llvm::StringMap<Placed> out;
  bool ok = true;
  SmallVector<StringRef> lines;
  (*buf)->getBuffer().split(lines, '\n');
  for (unsigned i = 0; i < lines.size(); ++i) {
    StringRef line = lines[i].trim();
    if (line.empty() || line.starts_with("#"))
      continue;
    auto bad = [&](const Twine &why) {
      diag::error("place.in-format",
                  "sea-place-static: " + path + ":" + Twine(i + 1) + ": " + why,
                  loc);
      ok = false;
    };
    llvm::StringMap<StringRef> kv;
    SmallVector<StringRef> fields;
    line.split(fields, ' ', -1, /*KeepEmpty=*/false);
    bool lineOk = true;
    for (StringRef f : fields) {
      auto [k, v] = f.split('=');
      if (!f.contains('=') || v.empty() ||
          !llvm::is_contained({"name", "kind", "base", "size", "align"}, k)) {
        bad("expected name=, kind=, base=, size=, align=, got '" + f + "'");
        lineOk = false;
        break;
      }
      if (!kv.try_emplace(k, v).second) {
        bad("'" + k + "' twice");
        lineOk = false;
        break;
      }
    }
    if (!lineOk)
      continue;
    if (kv.size() != 5) {
      bad("an object needs name=, kind=, base=, size= and align=");
      continue;
    }
    Placed p;
    p.kind = kv["kind"].str();
    p.line = i + 1;
    if (!llvm::is_contained({"stack", "heap", "global", "func", "null"},
                            p.kind)) {
      bad("kind '" + p.kind + "' is not stack, heap, global, func or null");
      continue;
    }
    if (kv["base"].getAsInteger(0, p.base) ||
        kv["size"].getAsInteger(0, p.size) ||
        kv["align"].getAsInteger(0, p.align) || p.align == 0) {
      bad("base, size and align are unsigned 64-bit numbers, align > 0");
      continue;
    }
    StringRef name = kv["name"];
    if (!out.try_emplace(name, p).second)
      bad("object '" + name + "' twice");
  }
  if (!ok)
    return failure();
  return out;
}

/// The pointer width used by the program (all pointers must agree).
static FailureOr<unsigned> pointerWidth(ProgramOp program) {
  std::optional<unsigned> width;
  bool mixed = false;
  auto visit = [&](Type t) {
    if (auto m = dyn_cast<MemType>(t))
      t = m.getIndex();
    auto p = dyn_cast<PtrType>(t);
    if (!p)
      return;
    if (width && *width != p.getWidth())
      mixed = true;
    width = p.getWidth();
  };
  program.walk([&](Operation *op) {
    for (Type t : op->getResultTypes())
      visit(t);
    for (Region &r : op->getRegions())
      for (Block &b : r)
        for (Type t : b.getArgumentTypes())
          visit(t);
  });
  if (mixed) {
    program.emitError("sea-place-static: pointers of different widths");
    return failure();
  }
  return width.value_or(0);
}

/// True if `block` is on a cycle of the CFG.
static bool onCycle(Block *block) {
  llvm::SmallPtrSet<Block *, 16> seen;
  SmallVector<Block *> work(block->getSuccessors().begin(),
                            block->getSuccessors().end());
  while (!work.empty()) {
    Block *b = work.pop_back_val();
    if (b == block)
      return true;
    if (!seen.insert(b).second)
      continue;
    work.append(b->getSuccessors().begin(), b->getSuccessors().end());
  }
  return false;
}

LogicalResult
PlaceStaticPass::collect(ProgramOp program, SmallVector<Object> &objects,
                         llvm::DenseMap<Operation *, unsigned> &objectOf) {
  llvm::StringMap<unsigned> byName; // globals and functions
  bool ok = true;
  // The null object (SeaM-null.md): first, so its order is 0. Size 0 (no
  // access of positive width is in it), at address 0 (outside every other
  // object: placement starts at 1 or at the text segment).
  bool hasNull = false;
  program.walk([&](PtrNullOp) { hasNull = true; });
  if (hasNull) {
    Object o{nullptr, program.getLoc(), "null", "null"};
    o.room = 0;
    o.align = kNullAlign;
    objects.push_back(o);
  }
  program.walk([&](Operation *op) {
    auto addAlloc = [&](Value size, uint64_t align, uint64_t site,
                        StringRef kind) {
      if (onCycle(op->getBlock())) {
        diag::error("place.alloc-in-loop",
                    "sea-place-static: an allocation inside a loop cannot be "
                    "placed statically (unroll first)",
                    op->getLoc());
        ok = false;
        return;
      }
      Object o{op, op->getLoc(), (kind + "." + llvm::Twine(site)).str(), kind};
      o.align = align;
      // A size computed from constants (calloc's n * size) is a constant.
      if (std::optional<APInt> c = foldConstant(size)) {
        o.room = c->getZExtValue();
      } else {
        o.symbolicSize = true;
        o.room = maxSymbolicSize;
      }
      o.label = op->getAttrOfType<StringAttr>("label");
      objectOf[op] = objects.size();
      objects.push_back(o);
    };
    if (auto pn = dyn_cast<PtrNondetOp>(op)) {
      // size 0, but one unit of address space: distinct from every object
      Object o{op, op->getLoc(), ("nondet." + llvm::Twine(pn.getSite())).str(),
               "heap"};
      o.room = 1;
      objectOf[op] = objects.size();
      objects.push_back(o);
    } else if (auto a = dyn_cast<AllocaOp>(op))
      addAlloc(a.getSize(), a.getAlign(), a.getSite(), "stack");
    else if (auto m = dyn_cast<MallocOp>(op))
      addAlloc(m.getSize(), m.getAlign(), m.getSite(), "heap");
    else if (auto g = dyn_cast<GlobalOp>(op)) {
      auto [it, inserted] =
          byName.try_emplace(("global:" + g.getName()).str(), objects.size());
      if (inserted) {
        Object o{op, op->getLoc(), ("global." + g.getName()).str(), "global"};
        o.room = g.getSize();
        o.align = g.getAlign();
        objects.push_back(o);
      }
      if (!objects[it->second].label)
        objects[it->second].label = g.getLabelAttr();
      objectOf[op] = it->second;
    } else if (auto f = dyn_cast<FuncAddrOp>(op)) {
      auto [it, inserted] =
          byName.try_emplace(("func:" + f.getName()).str(), objects.size());
      if (inserted) {
        Object o{op, op->getLoc(), ("func." + f.getName()).str(), "func"};
        o.room = kFuncSize;
        o.align = kFuncSize;
        objects.push_back(o);
      }
      objectOf[op] = it->second;
    }
  });
  return success(ok);
}

/// The address space an object takes: its room, at least one unit. An
/// object of size 0 (malloc(0), alloca of 0, an empty global) keeps size 0
/// (no access of positive width is in it) but gets its own base, so no two
/// objects share a base (C: malloc(0) returns null or a unique pointer).
/// Tool metadata keyed by object base (sea-lower-tracking's shadow) relies
/// on it. The null object is not placed by this (base 0, below every other).
static uint64_t addressRoom(const Object &o) {
  return std::max<uint64_t>(o.room, 1);
}

LogicalResult PlaceStaticPass::place(ProgramOp program, unsigned width,
                                     SmallVector<Object> &objects) {
  APInt limit = APInt::getMaxValue(width);
  bool seahorn =
      layout == "seahorn" || (layout == "auto" && limit.uge(kMaxStackAddr));
  if (layout == "seahorn" && limit.ult(kMaxStackAddr)) {
    diag::error("place.layout-too-wide",
                "sea-place-static: the seahorn layout needs 32-bit or wider "
                "pointers, the program uses " +
                    llvm::Twine(width) + " bits",
                program.getLoc());
    return failure();
  }
  if (!seahorn && layout == "auto")
    diag::warn("place.compact-layout",
               "pointers of " + llvm::Twine(width) +
                   " bits are too narrow for SeaHorn's layout; placing all "
                   "objects upwards from address 1",
               program.getLoc());

  auto fail = [&](const Object &o) {
    diag::error("place.out-of-space",
                "sea-place-static: no room for " + o.name + " (" +
                    llvm::Twine(o.room) + " units) in a " + llvm::Twine(width) +
                    "-bit address space",
                o.loc);
    return failure();
  };

  if (!seahorn) {
    // Address 0 stays null; objects follow in program order.
    uint64_t next = 1;
    for (Object &o : objects) {
      if (o.kind == "null")
        continue; // base 0, no room
      uint64_t base = llvm::alignTo(next, o.align);
      uint64_t end;
      // unchecked uint64 arithmetic wrapped a huge object (malloc(-1)) onto
      // low addresses: overflow is out of space
      if (base < next || __builtin_add_overflow(base, addressRoom(o), &end) ||
          APInt(64, end - 1).ugt(limit.zext(64)))
        return fail(o);
      o.base = base;
      next = end;
    }
    return success();
  }

  // SeaHorn: functions, then globals, then heap upwards from the text
  // segment start; stack downwards from the stack top.
  // An object of size 0 gets one unit of address space (addressRoom): its
  // own base, as in the compact layout.
  uint64_t next = kTextSegmentStart;
  for (StringRef kind : {"func", "global", "heap"})
    for (Object &o : objects)
      if (o.kind == kind) {
        o.base = llvm::alignTo(next, o.align);
        uint64_t end;
        if (o.base < next ||
            __builtin_add_overflow(o.base, addressRoom(o), &end))
          return fail(o);
        next = llvm::alignTo(end, o.align);
        if (next < end)
          return fail(o);
      }
  if (next > kMinStackAddr)
    return fail(objects.back());
  uint64_t top = kMaxStackAddr;
  for (Object &o : objects)
    if (o.kind == "stack") {
      if (top < addressRoom(o))
        return fail(o);
      top = llvm::alignDown(top - addressRoom(o), o.align);
      if (top < kMinStackAddr)
        return fail(o);
      o.base = top;
    }
  return success();
}

LogicalResult PlaceStaticPass::placeFromFile(ProgramOp program, unsigned width,
                                             SmallVector<Object> &objects) {
  FailureOr<llvm::StringMap<Placed>> file =
      readPlacement(placeIn, program.getLoc());
  if (failed(file))
    return failure();
  APInt limit = APInt::getMaxValue(width);
  bool ok = true;
  auto error = [&](StringRef kind, const Object &o, const Twine &why) {
    diag::error(kind, "sea-place-static: " + o.name + " " + why, o.loc);
    ok = false;
  };
  for (Object &o : objects) {
    auto it = file->find(o.name);
    if (it == file->end()) {
      error("place.in-unplaced", o,
            "is not in the placement file " + placeIn +
                " (its writer did not have this object)");
      continue;
    }
    const Placed &p = it->second;
    if (p.kind != o.kind || p.size != o.room || p.align != o.align) {
      error("place.in-mismatch", o,
            "is kind=" + o.kind + " size=" + Twine(o.room) +
                " align=" + Twine(o.align) + " here, kind=" + p.kind +
                " size=" + Twine(p.size) + " align=" + Twine(p.align) + " in " +
                placeIn + ":" + Twine(p.line));
      continue;
    }
    if ((o.kind == "null") != (p.base == 0)) {
      error("place.in-null", o,
            o.kind == "null" ? "is null: its base must be 0, " + placeIn +
                                   " gives " + Twine(p.base)
                             : "is at address 0 in " + placeIn +
                                   ", the null pointer's address");
      continue;
    }
    if (p.base % o.align) {
      error("place.in-align", o,
            "is at " + Twine(p.base) + " in " + placeIn +
                ", not a multiple of its align " + Twine(o.align));
      continue;
    }
    uint64_t end;
    if (o.kind != "null" &&
        (__builtin_add_overflow(p.base, addressRoom(o), &end) ||
         APInt(64, end - 1).ugt(limit.zext(64)))) {
      error("place.in-width", o,
            "at " + Twine(p.base) + " (" + Twine(addressRoom(o)) +
                " units) does not fit a " + Twine(width) +
                "-bit address space");
      continue;
    }
    o.base = p.base;
  }
  if (!ok)
    return failure();
  // no overlap: the address ranges of the objects, by base
  SmallVector<const Object *> byBase;
  for (const Object &o : objects)
    if (o.kind != "null")
      byBase.push_back(&o);
  llvm::sort(byBase, [](const Object *a, const Object *b) {
    return a->base < b->base;
  });
  for (unsigned i = 1; i < byBase.size(); ++i)
    if (byBase[i - 1]->base + addressRoom(*byBase[i - 1]) > byBase[i]->base)
      error("place.in-overlap", *byBase[i],
            "overlaps " + byBase[i - 1]->name + " in " + placeIn);
  return success(ok);
}

LogicalResult PlaceStaticPass::writeFile(ProgramOp program,
                                         ArrayRef<Object> objects) {
  return sea::writeFileAtomic(
      placeOut,
      [&](llvm::raw_ostream &os) {
        os << "# sea-place-static placement of @" << program.getSymName()
           << ": one object per line\n";
        for (const Object &o : objects)
          os << "name=" << o.name << " kind=" << o.kind << " base=0x"
             << llvm::utohexstr(o.base, /*LowerCase=*/true)
             << " size=" << o.room << " align=" << o.align << "\n";
        return success();
      },
      "place.out-file", "sea-place-static: the placement file",
      program.getLoc());
}

void PlaceStaticPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  WalkResult structs = program.walk([](Operation *op) {
    auto isStruct = [](Type t) { return isa<StructType>(t); };
    if (llvm::any_of(op->getResultTypes(), isStruct))
      return WalkResult::interrupt();
    for (Region &r : op->getRegions())
      for (Block &b : r)
        if (llvm::any_of(b.getArgumentTypes(), isStruct))
          return WalkResult::interrupt();
    return WalkResult::advance();
  });
  if (structs.wasInterrupted()) {
    diag::error("place.struct",
                "sea-place-static: the program has struct values (run "
                "sea-scalarize-structs first)",
                program.getLoc());
    return signalPassFailure();
  }
  if (reuse) {
    diag::error("place.reuse-unimplemented",
                "sea-place-static: reuse=true is not implemented",
                program.getLoc());
    return signalPassFailure();
  }
  if (symbolicSize != "assume" && symbolicSize != "assert" &&
      symbolicSize != "refuse") {
    program.emitError("sea-place-static: symbolic-size must be assume, "
                      "assert or refuse");
    return signalPassFailure();
  }

  const bool files = !placeIn.empty() || !placeOut.empty();
  if (!placeIn.empty() && !placeOut.empty()) {
    diag::error("place.file-both",
                "sea-place-static: place-in and place-out exclude each other",
                program.getLoc());
    return signalPassFailure();
  }
  if (files) {
    // one file, one program (the file has no program names)
    auto programs = program->getParentOp()->getRegion(0).getOps<ProgramOp>();
    if (std::distance(programs.begin(), programs.end()) != 1) {
      diag::error("place.file-programs",
                  "sea-place-static: place-in / place-out need a module "
                  "with one sea.program",
                  program.getLoc());
      return signalPassFailure();
    }
  }

  FailureOr<unsigned> width = pointerWidth(program);
  if (failed(width))
    return signalPassFailure();
  if (*width == 0) {
    // nothing abstract: already concrete, no object to place; the file is
    // read (checked) or written (empty) all the same
    if ((!placeIn.empty() &&
         failed(readPlacement(placeIn, program.getLoc()))) ||
        (!placeOut.empty() && failed(writeFile(program, {}))))
      return signalPassFailure();
    return;
  }

  SmallVector<Object> objects;
  llvm::DenseMap<Operation *, unsigned> objectOf;
  if (failed(collect(program, objects, objectOf)))
    return signalPassFailure();
  if (files) {
    // the file matches objects by name
    llvm::StringSet<> names;
    for (const Object &o : objects)
      if (!names.insert(o.name).second) {
        diag::error("place.duplicate-name",
                    "sea-place-static: two objects named " + o.name +
                        " (place-in / place-out match objects by name)",
                    o.loc);
        return signalPassFailure();
      }
  }
  if (failed(placeIn.empty() ? place(program, *width, objects)
                             : placeFromFile(program, *width, objects)))
    return signalPassFailure();
  // postcondition: every object has its own base
  llvm::DenseMap<uint64_t, const Object *> byBase;
  for (const Object &o : objects)
    if (!byBase.try_emplace(o.base, &o).second) {
      (void)postconditionFailed("sea-place-static",
                                "two objects at one base (" +
                                    byBase[o.base]->name + ", " + o.name + ")",
                                program.getOperation());
      return signalPassFailure();
    }

  // 1. Retype: pointers become addresses, memories are indexed by them.
  auto addrTy = AddrType::get(ctx, *width, 8);
  auto convert = [&](Type t) -> Type {
    if (isa<PtrType>(t))
      return addrTy;
    if (auto m = dyn_cast<MemType>(t))
      if (isa<PtrType>(m.getIndex()))
        return MemType::get(ctx, addrTy, m.getCell(), m.getLayout());
    return t;
  };
  program.walk([&](Operation *op) {
    for (Value r : op->getResults())
      r.setType(convert(r.getType()));
    for (Region &region : op->getRegions())
      for (Block &b : region)
        for (BlockArgument a : b.getArguments())
          a.setType(convert(a.getType()));
  });

  // 2. Rewrite the ops of the abstract level. Values are already retyped,
  // so use untyped operands (typed accessors would cast to !sea.ptr).
  OpBuilder b(ctx);
  auto iW = IntegerType::get(ctx, *width);
  auto constant = [&](Location loc, const APInt &v) -> Value {
    return ConstOp::create(b, loc, iW, IntegerAttr::get(iW, v));
  };
  // The segment map first, at the start of the entry block: its results are
  // the objects' base addresses, which replace the objects below (an object
  // keeps its identity through placement; docs/design/SeaM.md).
  b.setInsertionPointToStart(&program.getBody().front());
  auto i64 = IntegerType::get(ctx, 64);
  SmallVector<Value> baseOf;
  for (const Object &o : objects)
    baseOf.push_back(SegmentOp::create(
        b, o.loc, addrTy, b.getStringAttr(o.name),
        IntegerAttr::get(iW, APInt(*width, o.base)),
        IntegerAttr::get(iW, APInt(*width, o.room)),
        IntegerAttr::get(i64, o.align), b.getStringAttr(o.kind), o.label));
  unsigned nullIndex = ~0u;
  for (unsigned i = 0; i < objects.size(); ++i)
    if (objects[i].kind == "null")
      nullIndex = i;
  bool ok = true;
  SmallVector<Operation *> toErase;
  program.walk([&](Operation *op) {
    b.setInsertionPoint(op);
    Location loc = op->getLoc();
    auto replace = [&](Value v) {
      op->getResult(0).replaceAllUsesWith(v);
      toErase.push_back(op);
    };
    if (isa<AllocaOp, MallocOp>(op)) {
      Object &o = objects[objectOf.lookup(op)];
      if (o.symbolicSize) {
        Value size = op->getOperand(0);
        if (symbolicSize == "refuse") {
          diag::error("place.symbolic-size-refused",
                      "an allocation of non-constant size (symbolic-size="
                      "refuse)",
                      loc);
          ok = false;
          return;
        }
        diag::warn("place.symbolic-size-" + symbolicSize.getValue(),
                   "an allocation of non-constant size gets " +
                       llvm::Twine(o.room) + " units; its size is " +
                       (symbolicSize == "assume" ? "assumed" : "asserted") +
                       " to fit",
                   loc);
        Value fits = ICmpOp::create(b, loc, ICmpPredicate::ule, size,
                                    constant(loc, APInt(*width, o.room)));
        if (symbolicSize == "assume")
          AssumeOp::create(b, loc, fits);
        else
          AssertOp::create(b, loc, fits);
      }
      replace(baseOf[objectOf.lookup(op)]);
    } else if (isa<GlobalOp, FuncAddrOp, PtrNondetOp>(op)) {
      replace(baseOf[objectOf.lookup(op)]);
    } else if (isa<FreeOp>(op)) {
      // Lifetimes do not matter without reuse.
      toErase.push_back(op);
    } else if (auto p = dyn_cast<PtrAddOp>(op)) {
      replace(AddrAddOp::create(b, loc, addrTy, op->getOperand(0),
                                op->getOperand(1)));
    } else if (auto p = dyn_cast<PtrIndexOp>(op)) {
      // the same arithmetic on the base's address (flags kept: placement
      // turns the abstract pointer into its address, the promise is the
      // same, P3 now about that address)
      replace(PtrIndexOp::create(b, loc, addrTy, p.getBase(), p.getIndices(),
                                 p.getStrides(), p.getOffset(), p.getInbounds(),
                                 p.getNusw(), p.getNuw()));
    } else if (auto p = dyn_cast<PtrDiffOp>(op)) {
      replace(AddrDiffOp::create(b, loc, p.getType(), op->getOperand(0),
                                 op->getOperand(1)));
    } else if (auto p = dyn_cast<PtrCmpOp>(op)) {
      replace(AddrCmpOp::create(b, loc, p.getPredicate(), op->getOperand(0),
                                op->getOperand(1)));
    } else if (isa<PtrNullOp>(op)) {
      replace(baseOf[nullIndex]);
    } else if (isa<PtrBaseOp, PtrDereferenceableOp, IsModifiedOp,
                   ResetModifiedOp, TrackingOnOp, TrackingOffOp>(op) ||
               op->hasAttr("tracked")) {
      diag::error("place.fat-pointer-op",
                  op->getName().getStringRef() +
                      " needs the object of a pointer, which addresses do "
                      "not keep: it needs the fat-pointer / shadow-memory "
                      "lowering (not implemented yet)",
                  loc);
      ok = false;
    } else if (auto p = dyn_cast<PtrToIntOp>(op)) {
      replace(AddrToIntOp::create(b, loc, p.getType(), op->getOperand(0)));
    } else if (auto p = dyn_cast<PtrFromIntOp>(op)) {
      replace(AddrFromIntOp::create(b, loc, addrTy, op->getOperand(0)));
    }
  });
  for (Operation *op : toErase)
    op->erase();
  if (!ok)
    return signalPassFailure();

  if (isAbstract(program)) {
    Operation *at = findValueOfType(program, [](Type t) {
      if (auto m = dyn_cast<MemType>(t))
        t = m.getIndex();
      return isa<PtrType>(t);
    });
    (void)postconditionFailed("sea-place-static", "a pointer",
                              at ? at : program.getOperation());
    return signalPassFailure();
  }
  if (!placeOut.empty() && failed(writeFile(program, objects)))
    return signalPassFailure();
}

} // namespace

std::unique_ptr<Pass> createPlaceStaticPass() {
  return std::make_unique<PlaceStaticPass>();
}

std::unique_ptr<Pass> createPlaceStaticPass(const PlaceFiles &files) {
  return std::make_unique<PlaceStaticPass>(files);
}

} // namespace sea
