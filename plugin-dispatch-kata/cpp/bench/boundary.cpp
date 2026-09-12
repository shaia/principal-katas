// Phase 6 — the plugin boundary.
//
// Everything before this measured code the compiler could see. This measures
// the other plane: a real shared library, loaded at runtime, reached through a
// C struct of function pointers.
//
// Three things get established.
//
//   1. What a crossing costs, against an internal handler doing the identical
//      arithmetic. The difference is the boundary, not the work.
//
//   2. What batching recovers. A crossing that costs a meaningful share of a
//      sub-microsecond budget cannot be paid per packet, and the fix changes
//      the *interface shape* rather than its implementation — which is a design
//      consequence of a measurement, the sort of thing step 5 is really asking
//      for.
//
//   3. That an old plugin works under a new host. plugin_v1 is compiled against
//      a genuinely shorter SwyPluginApi — it does not have the process_batch
//      field at all — so the size-field extension mechanism is exercised rather
//      than asserted.

#include "harness.hpp"
#include "host.hpp"
#include "packet.hpp"
#include "phases.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace switchyard::bench {
namespace {

#if defined(_WIN32)
constexpr const char* kExt = ".dll";
#else
constexpr const char* kExt = ".so";
#endif

std::string find_plugin(const char* stem) {
    static const char* kDirs[] = {"", "bin/", "../bin/", "cpp/bin/"};
    for (const char* d : kDirs) {
        std::string path = std::string(d) + stem + kExt;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return path;
        }
    }
    return {};
}

// The same arithmetic the plugin performs, compiled into this binary. The
// difference between this and the plugin path is the boundary and nothing else.
struct InternalEquivalent final : Declaring {
    explicit InternalEquivalent(std::uint16_t key) : key_(key) {}
    bool matches(const Packet& p) const override { return p.protocol == key_; }
    Result process(const Packet& p) override {
        std::uint64_t h = 0xcbf29ce484222325ULL ^ p.protocol;
        for (std::uint32_t i = 0; i < 24u; ++i) h = (h ^ p.payload[i]) * 0x100000001b3ULL;
        return Result{h, p.protocol};
    }
    Registration declare() const override { return Registration{key_, false}; }
    std::uint16_t key_;
};

// One handler, called directly. Selection is not the subject here.
template <typename H>
struct OneHandler {
    H* h;
    Result dispatch(const Packet& p) { return h->process(p); }
};

}  // namespace

void phase_boundary() {
    const std::string v2_path = find_plugin("plugin_v2");
    const std::string v1_path = find_plugin("plugin_v1");

    if (v2_path.empty() || v1_path.empty()) {
        check(false, "phase 6: both plugin shared libraries were found next to the binary");
        std::printf("  plugin_v1%s / plugin_v2%s not found; build them first.\n", kExt, kExt);
        return;
    }

    LoadedPlugin v2;
    if (!v2.load(v2_path)) {
        check(false, "phase 6: plugin_v2 loads and negotiates its ABI version");
        std::printf("  %s: %s\n", v2_path.c_str(), v2.error().c_str());
        return;
    }

    LoadedPlugin v1;
    const bool v1_ok = v1.load(v1_path);

    check(v2.ok(), "phase 6: plugin_v2 loads and negotiates its ABI version");
    check(v1_ok, "phase 6: plugin_v1, built against a shorter struct, still loads");
    check(v2.has_batch(), "phase 6: the new plugin offers the appended batch entry point");
    check(v1_ok && !v1.has_batch(),
          "phase 6: the old plugin is correctly detected as not having it");

    const auto keys = v2.claimed_keys();
    check(!keys.empty(), "phase 6: the plugin declares the keys it claims before any traffic");

    std::printf("  %-14s %-9s struct_size %3u  abi %u  keys %zu  batch %s\n", "plugin_v2",
                "loaded", v2.api()->struct_size, v2.api()->abi_version, keys.size(),
                v2.has_batch() ? "yes" : "no");
    if (v1_ok)
        std::printf("  %-14s %-9s struct_size %3u  abi %u  keys %zu  batch %s\n", "plugin_v1",
                    "loaded", v1.api()->struct_size, v1.api()->abi_version,
                    v1.claimed_keys().size(), v1.has_batch() ? "yes" : "no");

    // Traffic entirely to the plugin's keys, so every packet crosses.
    PacketStream stream(Distribution::Uniform, keys, 0xB0117);
    const Packet* p = stream.data();
    const std::size_t n = stream.size();

    PluginBackedHandler plugin_h(v2.api(), keys[0]);
    check(plugin_h.valid(), "phase 6: the plugin allocated its instance through its own create()");
    InternalEquivalent internal_h(keys[0]);

    // The plugin and the internal version must agree, or the crossing cost is
    // being measured against different work.
    {
        Checksum a, b;
        for (std::size_t i = 0; i < 4096; ++i) {
            Packet q = p[i];
            q.protocol = keys[0];
            a.feed(plugin_h.process(q));
            b.feed(internal_h.process(q));
        }
        check(a.value() == b.value(),
              "phase 6: the plugin and the internal equivalent compute the same result");
    }

    OneHandler<PluginBackedHandler> across{&plugin_h};
    OneHandler<InternalEquivalent>  within{&internal_h};

    const auto     mm       = measure_interleaved(p, n, [&] { return across; },
                                                  [&] { return within; });
    const Measured m_across = mm[0];
    const Measured m_within = mm[1];

    // Batched. The conversion to the ABI's view type is part of the cost and is
    // inside the timed region, because a real host pays it too.
    constexpr std::size_t kBatch = 64;
    std::vector<SwyPacketView> views(kBatch);
    std::vector<SwyResult>     outs(kBatch);
    void* self = v2.api()->create();

    double batched_ns = 0.0;
    {
        Checksum   sum;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i + kBatch <= n; i += kBatch) {
            for (std::size_t j = 0; j < kBatch; ++j)
                views[j] = SwyPacketView{p[i + j].protocol, 0, 24u, p[i + j].payload};
            v2.api()->process_batch(self, views.data(), kBatch, outs.data());
            for (std::size_t j = 0; j < kBatch; ++j)
                sum.feed(Result{outs[j].value, outs[j].handler});
        }
        const auto t1 = Clock::now();
        const double dispatched = static_cast<double>(n / kBatch) * static_cast<double>(kBatch);
        batched_ns = std::chrono::duration<double>(t1 - t0).count() * 1e9 / dispatched;
        if (sum.value() == 1) std::printf(" ");
    }
    v2.api()->destroy(self);

    std::printf("\n  %-22s | %9s %11s\n", "path", "ns/pkt", "vs internal");
    std::printf("  %-22s | %9.1f %11s\n", "internal virtual call", m_within.ns_per_packet, "-");
    std::printf("  %-22s | %9.1f %+10.1f\n", "plugin, per packet", m_across.ns_per_packet,
                m_across.ns_per_packet - m_within.ns_per_packet);
    std::printf("  %-22s | %9.1f %+10.1f\n", "plugin, batched x64", batched_ns,
                batched_ns - m_within.ns_per_packet);

    std::printf("\n  the crossing costs %.1f ns, %.0f%% of the %.0f ns budget. Batching it x%zu\n"
                "  recovers %.1f ns of that.\n",
                m_across.ns_per_packet - m_within.ns_per_packet,
                100.0 * (m_across.ns_per_packet - m_within.ns_per_packet) / kBudgetNs, kBudgetNs,
                kBatch, m_across.ns_per_packet - batched_ns);
    std::printf("  Which is to say: it does not. An in-process C ABI crossing is an indirect\n"
                "  call through a function pointer and a struct copy — there is no transition\n"
                "  to amortize, and the batch loop's own writes to the view and result arrays\n"
                "  cost about what the saved call did. Batching is the right answer where a\n"
                "  crossing has a real fixed cost (a cgo call, an IPC round trip, a syscall);\n"
                "  designing it into THIS ABI on the assumption that boundaries are expensive\n"
                "  would have been a premature complication measured against nothing.\n");
}

}  // namespace switchyard::bench
