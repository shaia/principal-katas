#pragma once
//
// The packet, the result, and the packet stream.
//
// Packet is 32 bytes on purpose: two per cache line, so a walk over the pool is
// a sequential prefetchable stream and the dispatch cost being measured is not
// buried under the cost of fetching the input.

#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace switchyard {

struct Packet {
    std::uint16_t protocol;   // the cheap exact key
    std::uint16_t length;
    std::uint32_t flags;
    std::uint8_t  payload[24];
};
static_assert(sizeof(Packet) == 32, "Packet is a wire-ish struct; keep it two-per-cache-line");

struct Result {
    // What the handler computed. Arms are compared by checksumming these, so
    // this field is simultaneously the useful output and the reason the
    // optimizer cannot delete the dispatch loop.
    std::uint64_t value;
    // Which handler produced it. kNone is the brief's Result::unsupported().
    std::uint16_t handler;

    static constexpr std::uint16_t kNone = 0xFFFFu;
    static Result unsupported() { return Result{0, kNone}; }
};

// FNV-1a over a result stream. Every arm accumulates one, and phase 1 asserts
// they are all equal to the reference scan's. That single equality does two
// jobs: it is the correctness invariant, and it is what keeps a dead-code
// eliminator from deleting the arm entirely. The mpsc kata never needed an
// optimizer barrier because every measured path crossed a thread boundary;
// this one is a single-threaded loop and does.
struct Checksum {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    void feed(const Result& r) {
        h = (h ^ r.value) * 0x100000001b3ULL;
        h = (h ^ r.handler) * 0x100000001b3ULL;
    }
    std::uint64_t value() const { return h; }
};

// How protocol keys are distributed across the packet stream. The brief gives a
// handler *count* and never gives this, which step 9 argues is the larger of the
// two numbers. Naming the distributions as first-class objects is the point.
enum class Distribution {
    Uniform,   // every handler equally likely — the flattering-to-tables case
    Zipf,      // s = 1.1, which is roughly what real protocol mixes look like
    Hot95,     // 95 % of packets to two protocols
    Worst,     // always the last handler in registration order
};

inline const char* name_of(Distribution d) {
    switch (d) {
        case Distribution::Uniform: return "uniform";
        case Distribution::Zipf:    return "zipf";
        case Distribution::Hot95:   return "95/5";
        case Distribution::Worst:   return "worst";
    }
    return "?";
}

// Generated once, replayed by every arm. Generation is deliberately outside the
// measured loop; an arm that had to synthesize its own input would be measuring
// the generator.
class PacketStream {
  public:
    // `keys` maps handler index to the key that handler claims. The stream is
    // generated against the *registered* key set rather than against handler
    // indices, because handlers with a residual predicate share keys — so
    // "which handler was chosen" and "what key went on the wire" are not the
    // same question, which is the whole reason the residual list exists.
    PacketStream(Distribution d, const std::vector<std::uint16_t>& keys, std::uint64_t seed)
        : dist_(d), handlers_(static_cast<int>(keys.size())) {
        std::mt19937_64 rng(seed);
        build_cdf(d, handlers_, rng);

        packets_.resize(kPacketPool);
        freq_.assign(keys.size(), 0);

        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (auto& p : packets_) {
            if (unit(rng) < kUnmatchedFraction) {
                // Deliberately outside every handler's claimed key. A scan pays
                // its full length for these; a table pays one miss.
                p.protocol = static_cast<std::uint16_t>(kUnmatchedKeyBase + (rng() % 256));
            } else {
                const int h = pick(unit(rng));
                p.protocol = keys[static_cast<std::size_t>(h)];
                ++freq_[static_cast<std::size_t>(h)];
            }
            p.length = 64;
            p.flags  = static_cast<std::uint32_t>(rng());
            for (auto& b : p.payload) b = static_cast<std::uint8_t>(rng());
        }
    }

    const Packet* data() const { return packets_.data(); }
    std::size_t   size() const { return packets_.size(); }
    Distribution  dist() const { return dist_; }
    int           handlers() const { return handlers_; }

    // True frequency of each handler in *this* stream. The frequency-ordered
    // scan arm sorts by this, which is the most favourable possible version of
    // that arm — it is being given the answer. If it still loses, the loss is
    // real.
    const std::vector<std::uint64_t>& frequencies() const { return freq_; }

  private:
    void build_cdf(Distribution d, int n, std::mt19937_64& rng) {
        cdf_.assign(static_cast<std::size_t>(n), 0.0);
        perm_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) perm_[static_cast<std::size_t>(i)] = i;

        // Which handler gets which weight is SHUFFLED, so registration order is
        // uncorrelated with traffic frequency.
        //
        // Without this the weights land on handlers 0, 1, 2 ... in order, which
        // means the busiest handler is already first in the list and sorting by
        // frequency is a no-op. That silently rigs the central comparison of
        // this phase — the cheap competitor gets no chance to help, and the
        // table wins by default. Nobody registers handlers in traffic order by
        // accident, and a benchmark that assumes they did is measuring its own
        // generator.
        if (d != Distribution::Worst)
            std::shuffle(perm_.begin(), perm_.end(), rng);

        std::vector<double> w(static_cast<std::size_t>(n), 0.0);

        switch (d) {
            case Distribution::Uniform:
                for (auto& x : w) x = 1.0;
                break;
            case Distribution::Zipf:
                for (int i = 0; i < n; ++i)
                    w[static_cast<std::size_t>(i)] = 1.0 / std::pow(i + 1.0, 1.1);
                break;
            case Distribution::Hot95:
                for (int i = 0; i < n; ++i)
                    w[static_cast<std::size_t>(i)] = (i < 2) ? 0.475 : 0.05 / (n - 2);
                break;
            case Distribution::Worst:
                for (int i = 0; i < n; ++i)
                    w[static_cast<std::size_t>(i)] = (i == n - 1) ? 1.0 : 0.0;
                break;
        }

        double sum = 0.0;
        for (double x : w) sum += x;
        double acc = 0.0;
        for (int i = 0; i < n; ++i) {
            acc += w[static_cast<std::size_t>(i)] / sum;
            cdf_[static_cast<std::size_t>(i)] = acc;
        }
    }

    int pick(double u) const {
        // Linear scan of the CDF. This runs during generation only, never on a
        // measured path, so its own O(n) is not a problem — but it would be a
        // funny one to leave in the timed loop, given the subject.
        for (std::size_t i = 0; i < cdf_.size(); ++i)
            if (u <= cdf_[i]) return perm_[i];
        return perm_.back();
    }

    Distribution               dist_;
    int                        handlers_;
    std::vector<Packet>        packets_;
    std::vector<double>        cdf_;
    std::vector<int>           perm_;   // weight rank -> handler index
    std::vector<std::uint64_t> freq_;
};

}  // namespace switchyard
