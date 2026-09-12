// A real plugin, built twice from this one file: once at SWY_PLUGIN_ABI_LEVEL=1
// and once at 2. The level-1 build genuinely does not have the process_batch
// field — it is compiled against a shorter struct — so the host's handling of
// an old plugin is exercised rather than simulated.
//
// Nothing C++ crosses the boundary. This translation unit uses classes,
// exceptions and new/delete freely, and the host cannot observe any of it,
// which is the property the C ABI was chosen to buy.

#include "plugin_abi.h"

#include <cstdint>
#include <new>

namespace {

// The plugin's own handler type. The host has a `Handler` abstract class; this
// is not it, does not inherit from it, and was compiled without ever seeing it.
struct PluginHandler {
    std::uint64_t calls;
};

// What this plugin claims. Keys well outside the built-in range, so a collision
// with a built-in handler is a deliberate test rather than an accident.
const std::uint16_t kKeys[] = {2000, 2001, 2002, 2003};
const std::uint16_t kKeyCount = 4;

std::uint64_t mix(const SwyPacketView* pkt) {
    std::uint64_t h = 0xcbf29ce484222325ULL ^ pkt->protocol;
    const std::uint32_t n = pkt->bytes_len < 24u ? pkt->bytes_len : 24u;
    for (std::uint32_t i = 0; i < n; ++i)
        h = (h ^ pkt->bytes[i]) * 0x100000001b3ULL;
    return h;
}

// Declarative registration: the host builds its table before traffic arrives,
// so it cannot learn what this plugin handles by running a predicate on
// packets. Returning the required count when cap is too small lets the host
// size its buffer without a second entry point.
std::uint16_t claimed_keys(std::uint16_t* out, std::uint16_t cap) {
    if (cap < kKeyCount || out == nullptr) return kKeyCount;
    for (std::uint16_t i = 0; i < kKeyCount; ++i) out[i] = kKeys[i];
    return kKeyCount;
}

// Allocated here, freed by destroy() here. Not symmetry for its own sake: this
// binary's operator new and the host's may be entirely different functions
// operating on entirely different heaps, and a pointer freed on the wrong side
// is a corruption whose crash arrives later, elsewhere, in unrelated code.
void* create(void) {
    return new (std::nothrow) PluginHandler{0};
}

void destroy(void* self) {
    delete static_cast<PluginHandler*>(self);
}

// Every entry point is noexcept AND catches. noexcept alone would call
// std::terminate, which is a correct-but-fatal answer; catching converts the
// fault into the return value the ABI already has. An exception is a
// stack-unwinding protocol involving unwind tables, a personality routine and a
// shared runtime, and the frame above this one belongs to a binary that may
// share none of them.
int32_t process(void* self, const SwyPacketView* pkt, SwyResult* out) noexcept {
    try {
        if (self == nullptr || pkt == nullptr || out == nullptr) return SWY_ERROR;
        auto* h = static_cast<PluginHandler*>(self);
        ++h->calls;
        out->value     = mix(pkt);
        out->handler   = pkt->protocol;
        out->reserved0 = 0;
        out->reserved1 = 0;
        return SWY_OK;
    } catch (...) {
        return SWY_ERROR;
    }
}

#if SWY_PLUGIN_ABI_LEVEL >= 2
// Appended in level 2. Exists because a crossing that costs a meaningful share
// of a sub-microsecond per-packet budget cannot be paid once per packet.
int32_t process_batch(void* self, const SwyPacketView* pkts, size_t n,
                      SwyResult* out) noexcept {
    try {
        if (self == nullptr || pkts == nullptr || out == nullptr) return SWY_ERROR;
        auto* h = static_cast<PluginHandler*>(self);
        h->calls += n;
        for (size_t i = 0; i < n; ++i) {
            out[i].value     = mix(&pkts[i]);
            out[i].handler   = pkts[i].protocol;
            out[i].reserved0 = 0;
            out[i].reserved1 = 0;
        }
        return SWY_OK;
    } catch (...) {
        return SWY_ERROR;
    }
}
#endif

const SwyPluginApi kApi = {
    // struct_size is sizeof of whatever this build actually has, which at
    // level 1 is smaller. That is the extension mechanism working.
    static_cast<std::uint32_t>(sizeof(SwyPluginApi)),
    SWY_ABI_VERSION,
    claimed_keys,
    create,
    destroy,
    process,
#if SWY_PLUGIN_ABI_LEVEL >= 2
    process_batch,
#endif
};

}  // namespace

extern "C" SWY_EXPORT const SwyPluginApi* switchyard_plugin_entry(void) {
    return &kApi;
}
