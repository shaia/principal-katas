#pragma once
//
// The dispatch arms.
//
// Every arm is a concrete type with a NON-virtual dispatch(), and the driving
// loop is a template. That is not tidiness: making the arm itself virtual would
// add one indirect call to every measurement, which is the same order as the
// thing phase 4 is trying to measure. The arm interface has to be free.
//
// Probe counting is compiled out of the timed path and run in a separate
// untimed pass. A counter incremented per probe would cost more than some of
// the differences being reported.

#include "generated.hpp"
#include "handler.hpp"
#include "packet.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace switchyard {

// ---------------------------------------------------------------- scan arms

// The brief's dispatch, verbatim. Everything else in this file is a candidate
// replacement for these five lines.
class ScanUnordered {
  public:
    explicit ScanUnordered(std::vector<Declaring*> hs) : handlers_(std::move(hs)) {}

    Result dispatch(const Packet& p) { return run<false>(p); }
    Result dispatch_counted(const Packet& p) { return run<true>(p); }

    std::uint64_t probes() const { return probes_; }
    void          reset_probes() { probes_ = 0; }
    const char*   name() const { return "scan-unordered"; }

  protected:
    template <bool Count>
    Result run(const Packet& p) {
        for (auto* h : handlers_) {
            if constexpr (Count) ++probes_;
            if (h->matches(p)) return h->process(p);
        }
        return Result::unsupported();
    }

    std::vector<Declaring*> handlers_;
    std::uint64_t           probes_ = 0;
};

// The one-line competitor. Identical code to the arm above; the only difference
// is the order of the vector, sorted by observed frequency. Step 9 argues this
// is the change the table has to beat, and it is given every advantage here:
// the ordering is built from the true frequencies of the very stream it will be
// measured on, which is the best any real system could ever do.
class ScanOrdered : public ScanUnordered {
  public:
    ScanOrdered(std::vector<Declaring*> hs, const std::vector<std::uint64_t>& freq)
        : ScanUnordered(std::move(hs)) {
        std::vector<std::size_t> idx(handlers_.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(),
                         [&](std::size_t a, std::size_t b) { return freq[a] > freq[b]; });
        std::vector<Declaring*> sorted;
        sorted.reserve(handlers_.size());
        for (std::size_t i : idx) sorted.push_back(handlers_[i]);
        handlers_.swap(sorted);
    }
    const char* name() const { return "scan-ordered"; }
};

// Self-organizing: no prior knowledge of the distribution, learns it. Promotes
// on hit rather than swapping to front outright, which is the cheaper and more
// stable of the two classic policies.
class ScanMoveToFront : public ScanUnordered {
  public:
    using ScanUnordered::ScanUnordered;

    Result dispatch(const Packet& p) { return run<false>(p); }
    Result dispatch_counted(const Packet& p) { return run<true>(p); }
    const char* name() const { return "scan-mtf"; }

  private:
    template <bool Count>
    Result run(const Packet& p) {
        for (std::size_t i = 0; i < handlers_.size(); ++i) {
            if constexpr (Count) ++probes_;
            if (handlers_[i]->matches(p)) {
                Result r = handlers_[i]->process(p);
                if (i > 0) std::swap(handlers_[i], handlers_[i - 1]);
                return r;
            }
        }
        return Result::unsupported();
    }
};

// --------------------------------------------------------------- table arms

// A key's candidates, in registration order.
//
// `only` is the fast path and the reason a table is worth anything: a handler
// whose predicate is pure key equality needs no test at all once the key has
// selected it. The range is the slow path, and it exists because some
// predicates are not a function of the key — those handlers cannot own a slot,
// so they queue behind one. Bounding `count` is a design constraint the brief
// does not mention and step 2 has to invent.
struct Slot {
    Handler*      only  = nullptr;
    std::uint32_t begin = 0;
    std::uint16_t count = 0;
};

// Shared construction: gather each key's candidates in registration order, so
// first-match-wins is preserved exactly.
class TableBuilder {
  public:
    explicit TableBuilder(const std::vector<Declaring*>& hs) {
        for (auto* h : hs) {
            const Registration r = h->declare();
            keyed_.push_back({r.key, r.residual, h});
        }
    }

    struct Entry { std::uint16_t key; bool residual; Declaring* h; };
    const std::vector<Entry>& entries() const { return keyed_; }

    std::vector<std::uint16_t> distinct_keys() const {
        std::vector<std::uint16_t> ks;
        for (const auto& e : keyed_)
            if (std::find(ks.begin(), ks.end(), e.key) == ks.end()) ks.push_back(e.key);
        return ks;
    }

  private:
    std::vector<Entry> keyed_;
};

// Directly indexed by the raw 16-bit protocol field. No hashing, no remapping,
// one load. It is also 65536 slots regardless of how many are used — the
// density question step 2 asks, made measurable rather than argued.
class TableDirect16 {
  public:
    explicit TableDirect16(const std::vector<Declaring*>& hs) {
        tbl_.resize(kKeySpace16);
        TableBuilder b(hs);
        for (std::uint16_t k : b.distinct_keys()) {
            std::vector<Declaring*> cands;
            bool all_pure = true;
            for (const auto& e : b.entries())
                if (e.key == k) {
                    cands.push_back(e.h);
                    if (e.residual) all_pure = false;
                }
            Slot& s = tbl_[k];
            if (cands.size() == 1 && all_pure) {
                s.only = cands[0];
            } else {
                s.begin = static_cast<std::uint32_t>(cand_.size());
                s.count = static_cast<std::uint16_t>(cands.size());
                for (auto* c : cands) cand_.push_back(c);
            }
        }
    }

    Result dispatch(const Packet& p) { return run<false>(p); }
    Result dispatch_counted(const Packet& p) { return run<true>(p); }

    std::uint64_t probes() const { return probes_; }
    void          reset_probes() { probes_ = 0; }
    const char*   name() const { return "table-direct16"; }
    std::size_t   bytes() const { return tbl_.size() * sizeof(Slot); }

  private:
    template <bool Count>
    Result run(const Packet& p) {
        const Slot& s = tbl_[p.protocol];
        if constexpr (Count) ++probes_;
        if (s.only) return s.only->process(p);
        for (std::uint16_t i = 0; i < s.count; ++i) {
            if constexpr (Count) ++probes_;
            Handler* h = cand_[s.begin + i];
            if (h->matches(p)) return h->process(p);
        }
        return Result::unsupported();
    }

    std::vector<Slot>     tbl_;
    std::vector<Handler*> cand_;
    std::uint64_t         probes_ = 0;
};

// The same table over a remapped key that actually fits in cache. The mapping
// is the low bits of the protocol, verified injective over the registered key
// set at construction — a real system needs collision handling here, and the
// point of measuring both is that the 512 KiB version's extra cost is a cache
// miss per lookup, not an instruction.
class TableCompact {
  public:
    static constexpr std::size_t kBits = 10;
    static constexpr std::size_t kSize = std::size_t{1} << kBits;
    static constexpr std::uint16_t kMask = static_cast<std::uint16_t>(kSize - 1);

    explicit TableCompact(const std::vector<Declaring*>& hs) {
        tbl_.assign(kSize, CSlot{});
        TableBuilder b(hs);
        injective_ = true;
        for (std::uint16_t k : b.distinct_keys()) {
            CSlot& s = tbl_[k & kMask];
            if (s.key != kEmpty && s.key != k) { injective_ = false; continue; }
            s.key = k;
            std::vector<Declaring*> cands;
            bool all_pure = true;
            for (const auto& e : b.entries())
                if (e.key == k) {
                    cands.push_back(e.h);
                    if (e.residual) all_pure = false;
                }
            if (cands.size() == 1 && all_pure) {
                s.only = cands[0];
            } else {
                s.begin = static_cast<std::uint32_t>(cand_.size());
                s.count = static_cast<std::uint16_t>(cands.size());
                for (auto* c : cands) cand_.push_back(c);
            }
        }
    }

    // Whether the low-bit mapping happened to be collision-free over the
    // registered keys. Asserted by phase 1 rather than assumed: if it is false,
    // this arm's results would silently diverge from the scan's, which is
    // exactly the sort of thing a benchmark reports as a speedup.
    bool injective() const { return injective_; }

    Result dispatch(const Packet& p) { return run<false>(p); }
    Result dispatch_counted(const Packet& p) { return run<true>(p); }

    std::uint64_t probes() const { return probes_; }
    void          reset_probes() { probes_ = 0; }
    const char*   name() const { return "table-compact"; }
    std::size_t   bytes() const { return kSize * sizeof(CSlot); }

  private:
    static constexpr std::uint16_t kEmpty = 0xFFFFu;
    struct CSlot {
        std::uint16_t key   = kEmpty;
        Handler*      only  = nullptr;
        std::uint32_t begin = 0;
        std::uint16_t count = 0;
    };

    template <bool Count>
    Result run(const Packet& p) {
        const CSlot& s = tbl_[p.protocol & kMask];
        if constexpr (Count) ++probes_;
        if (s.key != p.protocol) return Result::unsupported();
        if (s.only) return s.only->process(p);
        for (std::uint16_t i = 0; i < s.count; ++i) {
            if constexpr (Count) ++probes_;
            Handler* h = cand_[s.begin + i];
            if (h->matches(p)) return h->process(p);
        }
        return Result::unsupported();
    }

    std::vector<CSlot>    tbl_;
    std::vector<Handler*> cand_;
    bool                  injective_ = true;
    std::uint64_t         probes_    = 0;
};

}  // namespace switchyard
