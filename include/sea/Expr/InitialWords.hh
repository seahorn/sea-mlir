// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
/// Initial byte memories read as words (docs/design/SeaM-initial-words.md):
/// a solver-facing encoding step on the final query, after the
/// boundary translation (store-maps, width terms not over an array constant,
/// memory-operation terms) and before the unique-name check and the
/// Ackermann lowering.
///
/// An array constant A : bv(n) -> bv(8) (the initial memory `nd.<site>` of a
/// region, or the fresh array of a memhavoc) that is only read (every
/// occurrence the array of a select or a wselect; no binder in the query) is
/// replaced by a fresh word array A_w : bv(n) -> bv(8w), A_w = `<A>.w<w>`,
/// read through the decoding
///
///     A[a] = byte (a mod w) of A_w[a - (a mod w)]          (little endian)
///
/// Every read of A becomes reads of A_w at w-aligned indices: an exact
/// re-encoding (equisatisfiable, no premise: A_w's entries at unaligned
/// indices are never read). The one exception is the annotated fast path:
/// a w-byte wselect whose trusted `align` is >= w becomes A_w[j] directly
/// (counted: words.aligned-by-annotation). A_w is the packing of A at every
/// index, so this is an identity whenever the annotation holds; a wrong
/// annotation can only give a spurious sat, which replay rejects.
///
/// Indices stay linear for the Ackermann lowering: an index
/// j = S + c (LinearForms) whose symbolic part S is provably w-aligned (every
/// coefficient, with the layout values substituted, a multiple of w) is read
/// at `j - (c mod w) + q w`; otherwise one canonical atom per S, B_S =
/// S & ~(w-1) and o_S = S mod w (S rebuilt as j - c from the first index
/// seen with that S), and the bytes come from the words B_S + q w selected by
/// o_S (a w-way ite of constant extracts). All word indices of one S are then
/// B_S + multiples of w: offset pairs, free for the lowering.
///
/// The map A -> A_w is frozen by analyze() (Contract 2); translate() only
/// applies it. WordsModel (Contract 3) gives every model consumer the bytes
/// of A back from a model of the word query.
///
/// Shadow memories start as constant arrays (sea.mem.const shadow), never as
/// array constants, and fat-pointer companions have wider cells: neither is
/// a candidate (the registry's roots are not array constants).

#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprOpArray.hh"
#include "sea/Expr/Smt/Model.hh"

#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {

class InitialWords {
public:
  enum class Mode {
    All,  ///< every eligible array
    Auto, ///< eligible arrays whose aligned word reads cover the bytes read
  };

  /// Per candidate array: what the analysis saw, and the decision.
  struct Census {
    Expr array;
    Expr word; ///< A_w (null if not chosen)
    bool eligible = false;
    bool chosen = false;
    uint64_t selects = 0;  ///< byte reads (select)
    uint64_t wselects = 0; ///< wide reads (wselect)
    uint64_t alignedWords =
        0;                   ///< w-byte wselects aligned (proved or annotated)
    uint64_t otherBytes = 0; ///< bytes of the other reads
    std::string reason;      ///< why not chosen
  };

  struct Stats {
    uint64_t candidates = 0; ///< bv(n) -> bv(8) array constants
    uint64_t eligible = 0;   ///< of them only read
    uint64_t chosen = 0;
    uint64_t translated = 0; ///< reads of chosen arrays translated
    uint64_t wordReads = 0;  ///< whole aligned word reads (A_w[j])
    uint64_t alignedProved = 0;
    uint64_t alignedByAnnotation = 0;
    uint64_t alignedPartial =
        0; ///< reads with a proved-aligned S, not a whole word
    uint64_t maskedReads = 0; ///< reads through a canonical B_S
    uint64_t maskedParts = 0; ///< distinct symbolic parts with a B_S
  };

  /// \p w a power of two >= 2; \p layout: constants with a known value (the
  /// layout: obj.k = numeral), used to prove alignment.
  InitialWords(unsigned w, Mode mode,
               const std::unordered_map<ENode *, mpz_class> *layout)
      : m_w(w), m_mode(mode), m_layout(layout) {
    while ((1u << m_t) < w)
      ++m_t;
  }

  /// Decides the arrays of \p root and freezes the map (once).
  void analyze(const Expr &root) {
    using namespace ackermann_detail;
    if (m_frozen)
      return;
    m_frozen = true;
    std::unordered_set<ENode *> seen, bad;
    std::vector<ENode *> cands;
    std::vector<ENode *> stack{root.get()};
    if (isArrayConstant(root.get()))
      bad.insert(root.get());
    bool binders = false;
    std::unordered_map<ENode *, Census> census;
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (isArrayConstant(n)) {
        if (isCandidateSort(Expr(n)))
          cands.push_back(n);
        continue;
      }
      if (isLeaf(n))
        continue;
      if (isOpX<FORALL>(n) || isOpX<EXISTS>(n) || isOpX<LAMBDA>(n))
        binders = true;
      bool read = isOpX<SELECT>(n) || isOpX<W_SELECT>(n);
      for (size_t i = 0; i < n->arity(); ++i) {
        ENode *k = n->arg(i);
        if (isArrayConstant(k) && !(read && i == 0))
          bad.insert(k);
        stack.push_back(k);
      }
      if (read && isArrayConstant(n->arg(0))) {
        Census &c = census[n->arg(0)];
        if (isOpX<SELECT>(n)) {
          ++c.selects;
          ++c.otherBytes;
        } else {
          ++c.wselects;
          unsigned r = op::bv::width(n->arg(2)) / 8;
          if (r == m_w && (op::array::promisedAlign(Expr(n)) >= m_w ||
                           provedAligned(Expr(n->arg(1)))))
            ++c.alignedWords;
          else
            c.otherBytes += r;
        }
      }
    }
    for (ENode *a : cands) {
      Census c = census[a];
      c.array = Expr(a);
      ++m_stats.candidates;
      if (binders)
        c.reason = "binder";
      else if (bad.count(a))
        c.reason = "not only read";
      else if (op::bv::width(
                   sort::arrayIndexTy(op::bind::declaredSort(c.array))) < m_t)
        c.reason = "index narrower than a word";
      else
        c.eligible = true;
      if (c.eligible) {
        ++m_stats.eligible;
        // [HYP] Words pay where aligned word traffic covers the bytes
        // read some other way; the auto rule uses this heuristic.
        if (m_mode == Mode::All ||
            (c.alignedWords > 0 && c.alignedWords * m_w >= c.otherBytes))
          c.chosen = true;
        else
          c.reason = c.alignedWords ? "byte-dominated" : "no aligned word read";
      }
      if (c.chosen) {
        ++m_stats.chosen;
        c.word = wordArray(c.array);
        m_chosen.emplace(a, c.word);
        m_keep.push_back(c.array);
        m_keep.push_back(c.word);
      }
      m_census.push_back(std::move(c));
    }
#ifndef NDEBUG
    // The word arrays must be distinct and fresh: two chosen arrays sharing
    // a word array, or a word array that is already a query constant, would
    // identify reads that are independent (a wrong unsat, as the index-only
    // cache key did).
    // - Distinct holds by construction: A_w is named name(A) + ".w<k>" with
    //   sort array(index(A), bv(8k)), and two different chosen arrays are
    //   both byte arrays, so they differ in name or index sort.
    // - Fresh holds by the VC's naming convention only (no name ends in
    //   ".w<k>"), hence this check, in debug builds.
    {
      std::unordered_set<ENode *> words;
      for (const auto &[a, w] : m_chosen) {
        (void)a;
        assert(words.insert(w.get()).second &&
               "InitialWords: two chosen arrays share a word array");
        assert(!seen.count(w.get()) &&
               "InitialWords: a word array is already a query constant");
      }
    }
#endif
  }

  /// \p root with every read of a chosen array replaced by its decoding.
  /// Null if a chosen array is left (a bug: \p why says so).
  Expr translate(const Expr &root, std::string &why) {
    using namespace ackermann_detail;
    if (!m_frozen)
      analyze(root);
    if (m_chosen.empty())
      return root;
    Expr res = rewriteDag(
        root, m_memo, m_keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          bool sel = isOpX<SELECT>(n), wsel = isOpX<W_SELECT>(n);
          if ((sel || wsel) && m_chosen.count(n->arg(0))) {
            ++m_stats.translated;
            ENode *a = n->arg(0);
            unsigned r = sel ? 1 : op::bv::width(n->arg(2)) / 8;
            uint64_t al = wsel ? op::array::promisedAlign(Expr(n)) : 1;
            Expr d = decode(a, kids[1], r, al);
            if (m_record)
              m_reads.push_back({Expr(n), d});
            return d;
          }
          return withKids(n, kids);
        });
    // postcondition: no chosen array left
    std::unordered_set<ENode *> seen;
    std::vector<ENode *> stack{res.get()};
    while (!stack.empty()) {
      ENode *n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second)
        continue;
      if (m_chosen.count(n)) {
        why = "initial words: a chosen array is left in the query";
        return Expr();
      }
      if (!isLeaf(n))
        for (size_t i = 0; i < n->arity(); ++i)
          stack.push_back(n->arg(i));
    }
    return res;
  }

  bool isChosen(ENode *a) const { return m_chosen.count(a); }
  /// A_w of a chosen array (null otherwise).
  Expr wordOf(ENode *a) const {
    auto it = m_chosen.find(a);
    return it == m_chosen.end() ? Expr() : it->second;
  }
  /// The word index terms read of A_w (for the model's array view).
  const std::vector<Expr> &wordIndices(ENode *a) const {
    static const std::vector<Expr> none;
    auto it = m_indices.find(a);
    return it == m_indices.end() ? none : it->second;
  }
  unsigned wordBytes() const { return m_w; }
  /// Record every translated read with its decoding (--words-check
  /// localizes a failure). Set before translate().
  void recordReads(bool b) { m_record = b; }
  /// (original read, its decoding) pairs, in translation order.
  const std::vector<std::pair<Expr, Expr>> &reads() const { return m_reads; }
  const Stats &stats() const { return m_stats; }
  const std::vector<Census> &census() const { return m_census; }

private:
  bool isCandidateSort(const Expr &a) const {
    Expr s = op::bind::declaredSort(a);
    Expr is = sort::arrayIndexTy(s), vs = sort::arrayValTy(s);
    return isOpX<BVSORT>(is) && isOpX<BVSORT>(vs) && op::bv::width(vs) == 8;
  }

  Expr wordArray(const Expr &a) {
    std::string name;
    llvm::raw_string_ostream os(name);
    os << *op::bind::fname(a->first()) << ".w" << m_w;
    os.flush();
    Expr s = op::bind::declaredSort(a);
    Expr vs = op::bv::bvsort(8 * m_w, a->efac());
    return op::bind::mkConst(mkTerm<std::string>(name, a->efac()),
                             sort::arrayTy(sort::arrayIndexTy(s), vs));
  }

  ackermann_detail::LinearForms &forms(unsigned n, bool values) {
    auto &m = values ? m_formsV : m_forms;
    auto it = m.find(n);
    if (it == m.end()) {
      it = m.emplace(n, ackermann_detail::LinearForms(n)).first;
      if (values)
        it->second.setValues(m_layout);
    }
    return it->second;
  }

  unsigned widthOf(const Expr &j) { return op::bv::width(m_tc.typeOf(j)); }

  Expr num(const mpz_class &v, unsigned n, const Expr &like) {
    mpz_class mod, r;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, n);
    mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    return op::bv::bvnum(r, n, like->efac());
  }

  /// j + k (mod 2^n), j itself for k = 0 mod 2^n.
  Expr plus(const Expr &j, const mpz_class &k, unsigned n) {
    Expr kn = num(k, n, j);
    if (op::bv::toMpz(kn) == 0)
      return j;
    return mk<BADD>(j, kn);
  }

  /// Is \p j = 0 mod w in every model of the query (the layout values
  /// substituted)?
  bool provedAligned(const Expr &j) {
    unsigned n = widthOf(j);
    if (n < m_t)
      return false;
    const ackermann_detail::Linear &l = forms(n, true).of(j.get());
    if (LeafDecider::tz(l.c, n) < m_t)
      return false;
    for (const auto &[atom, k] : l.coef)
      if (LeafDecider::tz(k, n) < m_t)
        return false;
    return true;
  }

  /// The word read of chosen array \p a at \p idx, shared by every read of
  /// a at that index. Keyed by (array, index): keyed by the index alone, a
  /// second array read at the same index got the first array's word, which
  /// could produce a wrong unsat result.
  Expr word(ENode *a, const Expr &idx) {
    auto [it, fresh] =
        m_wordReads.emplace(std::make_pair(a, idx.get()), Expr());
    if (fresh) {
      it->second = op::array::select(m_chosen.at(a), idx);
      m_indices[a].push_back(idx);
      m_keep.push_back(idx);
    }
    return it->second;
  }

  /// concat of the words base + q w, q = lo..hi (hi most significant)
  Expr words(ENode *a, const Expr &base, uint64_t lo, uint64_t hi, unsigned n) {
    Expr x;
    for (uint64_t q = lo; q <= hi; ++q) {
      Expr wq = word(a, plus(base, mpz_class((unsigned long)(q * m_w)), n));
      x = x ? op::bv::concat(wq, x) : wq;
    }
    return x;
  }

  /// bytes [from, from + r) of x
  Expr bytes(const Expr &x, uint64_t from, unsigned r) {
    unsigned total = op::bv::width(m_tc.typeOf(x)) / 8;
    if (from == 0 && r == total)
      return x;
    return op::bv::extract(8 * (from + r) - 1, 8 * from, x);
  }

  /// The r-byte read of A at j (bytes j .. j + r - 1, little endian), with
  /// the promised alignment \p align.
  Expr decode(ENode *a, const Expr &j, unsigned r, uint64_t align) {
    using namespace ackermann_detail;
    unsigned n = widthOf(j);
    const uint64_t w = m_w;
    if (r == w && provedAligned(j)) {
      ++m_stats.wordReads;
      ++m_stats.alignedProved;
      return word(a, j);
    }
    if (r == w && align >= w) {
      ++m_stats.wordReads;
      ++m_stats.alignedByAnnotation;
      return word(a, j);
    }
    // S provably aligned: the offset in its word is a numeral
    const Linear &lv = forms(n, true).of(j.get());
    bool symAligned = true;
    for (const auto &[atom, k] : lv.coef)
      symAligned = symAligned && LeafDecider::tz(k, n) >= m_t;
    if (symAligned) {
      ++m_stats.alignedPartial;
      uint64_t o = mpz_fdiv_ui(lv.c.get_mpz_t(), w);
      Expr base = plus(j, mpz_class(0L) - mpz_class((unsigned long)o), n);
      return bytes(words(a, base, 0, (o + r - 1) / w, n), o, r);
    }
    // a canonical atom per symbolic part (the layout not substituted: the
    // base must be a term of the query as it is)
    ++m_stats.maskedReads;
    const Linear &l = forms(n, false).of(j.get());
    LinearForms::Key key = forms(n, false).key(l);
    auto &parts = m_parts[n];
    auto it = parts.find(key);
    if (it == parts.end()) {
      ++m_stats.maskedParts;
      Expr s0 = plus(j, mpz_class(0L) - l.c, n);
      mpz_class mask;
      mpz_ui_pow_ui(mask.get_mpz_t(), 2, n);
      mask = mask - mpz_class((unsigned long)w);
      Expr bs = mk<BAND>(s0, num(mask, n, j));
      Expr os = op::bv::extract(m_t - 1, 0, s0);
      m_keep.push_back(bs);
      m_keep.push_back(os);
      it = parts.emplace(key, Part{bs, os}).first;
    }
    const Part &p = it->second;
    // s0 = S (the symbolic part, its numeral removed), j = s0 + c,
    // s0 = B + o (o in [0, w)). (Was c - c0: off by the first read's
    // numeral; caught by --words-check on fuzz-seam and vcc.)
    mpz_class d = l.c, mod;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, n);
    mpz_fdiv_r(d.get_mpz_t(), d.get_mpz_t(), mod.get_mpz_t());
    mpz_class qlo;
    mpz_fdiv_q_ui(qlo.get_mpz_t(), d.get_mpz_t(), w);
    uint64_t cp = mpz_fdiv_ui(d.get_mpz_t(), w);
    uint64_t span = (w - 1 + cp + r - 1) / w; // words above qlo
    // the words qlo .. qlo + span, as offsets from B
    Expr x;
    for (uint64_t q = 0; q <= span; ++q) {
      mpz_class qq = qlo + mpz_class((unsigned long)q);
      Expr wq = word(a, plus(p.base, qq * mpz_class((unsigned long)w), n));
      x = x ? op::bv::concat(wq, x) : wq;
    }
    Expr res = bytes(x, (w - 1) + cp, r);
    for (uint64_t k = w - 1; k-- > 0;) {
      Expr c = mk<EQ>(p.off, op::bv::bvnum(mpz_class(k), m_t, j->efac()));
      res = mk<ITE>(c, bytes(x, k + cp, r), res);
    }
    return res;
  }

  struct Part {
    Expr base; ///< B_S
    Expr off;  ///< o_S (t bits)
  };

  unsigned m_w, m_t = 0;
  Mode m_mode;
  const std::unordered_map<ENode *, mpz_class> *m_layout;
  bool m_frozen = false;
  TypeChecker m_tc;
  std::unordered_map<ENode *, Expr> m_chosen;
  std::vector<Census> m_census;
  std::map<unsigned, ackermann_detail::LinearForms> m_forms, m_formsV;
  std::map<unsigned, std::map<ackermann_detail::LinearForms::Key, Part>>
      m_parts;
  /// (array, index) -> its word read (the keys are pinned: m_chosen and
  /// m_keep)
  std::map<std::pair<ENode *, ENode *>, Expr> m_wordReads;
  std::unordered_map<ENode *, std::vector<Expr>> m_indices;
  std::unordered_map<ENode *, Expr> m_memo;
  std::vector<Expr> m_keep;
  Stats m_stats;
  bool m_record = false;
  std::vector<std::pair<Expr, Expr>> m_reads;
};

/// A model of the byte query from a model of the word query (Contract 3):
/// every read of a chosen array A is answered from A_w in \p inner (the
/// AckermannModel when the lowering is on). A read select(M, j) of a memory
/// M over A: j evaluated, M walked down (a store at that index gives its
/// value, an ite its chosen branch) to A, whose byte comes from the word
/// containing it (completed by \p inner: the word's consistent value). Any
/// other occurrence of A (an equality of memories, in replay-only values)
/// is A as the store chain of the words the query read, bytes of words it
/// did not read 0. \p words must outlive it.
class WordsModel : public sea::solver::Model {
public:
  WordsModel(sea::solver::Model &inner, const InitialWords &words)
      : m_inner(inner), m_words(words) {}

  /// Byte reads answered from a word with no value (counted, 0).
  uint64_t defaulted() const { return m_defaulted; }
  /// Byte reads answered from a word.
  uint64_t decoded() const { return m_decoded; }

  Expr eval(Expr e, bool complete) override {
    using namespace ackermann_detail;
    if (m_broken)
      return Expr();
    Expr t = rewriteDag(
        e, m_memo, m_keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          if (m_broken || !isOpX<SELECT>(n) || !hasChosen(n->arg(0)))
            return withKids(n, kids);
          Expr j = m_inner.eval(kids[1], /*complete=*/true);
          if (!j || !op::bv::is_bvnum(j)) {
            m_broken = true;
            return withKids(n, kids);
          }
          Expr v =
              readMem(Expr(n->arg(0)), reducedValue(j), op::bv::widthBvNum(j));
          return v ? v : withKids(n, kids);
        });
    if (m_broken)
      return Expr();
    if (hasChosen(t.get()))
      t = rewriteDag(
          t, m_asArray, m_keep,
          [&](ENode *n) -> Expr {
            return m_words.isChosen(n) ? asArray(n) : Expr(n);
          },
          [](ENode *n, llvm::SmallVectorImpl<Expr> &kids) {
            return withKids(n, kids);
          });
    return m_inner.eval(t, complete);
  }

  void print(llvm::raw_ostream &o) const override { m_inner.print(o); }

  /// A byte of a chosen array is a default iff its word is one of A_w in
  /// the inner model (cell() decodes the byte from that word); other
  /// arrays: the inner model's answer.
  bool defaultCell(Expr arr, Expr idx) override {
    if (!arr || !m_words.isChosen(arr.get()))
      return m_inner.defaultCell(arr, idx);
    if (!idx || !op::bv::is_bvnum(idx))
      return false;
    mpz_class i = ackermann_detail::reducedValue(idx);
    unsigned long off = mpz_fdiv_ui(i.get_mpz_t(), m_words.wordBytes());
    return m_inner.defaultCell(m_words.wordOf(arr.get()),
                               op::bv::bvnum(i - mpz_class(off),
                                             op::bv::widthBvNum(idx),
                                             arr->efac()));
  }

private:
  bool hasChosen(ENode *root) {
    using namespace ackermann_detail;
    std::vector<std::pair<ENode *, bool>> stack{{root, false}};
    while (!stack.empty()) {
      auto [n, expanded] = stack.back();
      if (m_has.count(n)) {
        stack.pop_back();
        continue;
      }
      if (isLeaf(n)) {
        stack.pop_back();
        m_has.emplace(n, m_words.isChosen(n));
        m_keep.push_back(Expr(n));
        continue;
      }
      if (!expanded) {
        stack.back().second = true;
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_has.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      bool h = false;
      for (size_t i = 0; i < n->arity() && !h; ++i)
        h = m_has.find(n->arg(i))->second;
      m_has.emplace(n, h);
      m_keep.push_back(Expr(n));
    }
    return m_has.find(root)->second;
  }

  Expr readMem(Expr mem, const mpz_class &idx, unsigned n) {
    while (true) {
      if (!hasChosen(mem.get()))
        return m_inner.eval(
            op::array::select(mem, op::bv::bvnum(idx, n, mem->efac())),
            /*complete=*/true);
      if (m_words.isChosen(mem.get()))
        return cell(mem.get(), idx, n);
      if (isOpX<STORE>(mem)) {
        Expr i = eval(mem->arg(1), /*complete=*/true);
        if (!i || !op::bv::is_bvnum(i))
          return Expr();
        if (ackermann_detail::reducedValue(i) == idx)
          return eval(mem->arg(2), /*complete=*/true);
        mem = mem->arg(0);
        continue;
      }
      if (isOpX<ITE>(mem)) {
        Expr c = eval(mem->arg(0), /*complete=*/true);
        if (!c || !(isOpX<TRUE>(c) || isOpX<FALSE>(c)))
          return Expr();
        mem = isOpX<TRUE>(c) ? mem->arg(1) : mem->arg(2);
        continue;
      }
      return Expr();
    }
  }

  /// The value of word index \p widx of A_w (a numeral), null if none.
  Expr wordValue(ENode *a, const mpz_class &widx, unsigned n) {
    Expr v = m_inner.eval(
        op::array::select(m_words.wordOf(a), op::bv::bvnum(widx, n, a->efac())),
        /*complete=*/true);
    return v && op::bv::is_bvnum(v) ? v : Expr();
  }

  Expr cell(ENode *a, const mpz_class &idx, unsigned n) {
    const unsigned w = m_words.wordBytes();
    unsigned long off = mpz_fdiv_ui(idx.get_mpz_t(), w);
    Expr v = wordValue(a, idx - mpz_class(off), n);
    if (!v) {
      ++m_defaulted;
      return op::bv::bvnum(mpz_class(0UL), 8, a->efac());
    }
    ++m_decoded;
    mpz_class b((unsigned long)mpz_fdiv_ui(
        mpz_class(ackermann_detail::reducedValue(v) >> (8 * off)).get_mpz_t(),
        256));
    return op::bv::bvnum(b, 8, a->efac());
  }

  /// A as an array term: the bytes of the words the query read.
  Expr asArray(ENode *a) {
    const unsigned w = m_words.wordBytes();
    Expr s = op::bind::declaredSort(Expr(a));
    Expr is = sort::arrayIndexTy(s);
    unsigned n = op::bv::width(is);
    Expr res =
        op::array::constArray(is, op::bv::bvnum(mpz_class(0UL), 8, a->efac()));
    std::map<mpz_class, Expr> seen;
    for (const Expr &idx : m_words.wordIndices(a)) {
      Expr i = m_inner.eval(idx, /*complete=*/true);
      if (!i || !op::bv::is_bvnum(i))
        continue;
      mpz_class wi = ackermann_detail::reducedValue(i);
      if (!seen.emplace(wi, Expr()).second)
        continue;
      Expr v = wordValue(a, wi, n);
      if (!v)
        continue;
      mpz_class val = ackermann_detail::reducedValue(v);
      mpz_class mod;
      mpz_ui_pow_ui(mod.get_mpz_t(), 2, n);
      for (unsigned k = 0; k < w; ++k) {
        mpz_class b((unsigned long)mpz_fdiv_ui(
            mpz_class(val >> (8 * k)).get_mpz_t(), 256));
        mpz_class at = wi + mpz_class((unsigned long)k);
        mpz_fdiv_r(at.get_mpz_t(), at.get_mpz_t(), mod.get_mpz_t());
        res = op::array::store(res, op::bv::bvnum(at, n, a->efac()),
                               op::bv::bvnum(b, 8, a->efac()));
      }
    }
    return res;
  }

  sea::solver::Model &m_inner;
  const InitialWords &m_words;
  std::unordered_map<ENode *, Expr> m_memo, m_asArray;
  std::unordered_map<ENode *, bool> m_has;
  std::vector<Expr> m_keep;
  bool m_broken = false;
  uint64_t m_defaulted = 0;
  uint64_t m_decoded = 0;
};

} // namespace expr
