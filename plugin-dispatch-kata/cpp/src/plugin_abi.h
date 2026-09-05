/* switchyard plugin ABI.
 *
 * This file is C, not C++, and that is the entire point of it. Everything that
 * crosses the boundary has to survive a different compiler, a different
 * standard library, and a different language three years from now, which rules
 * out every type you reach for in an ordinary signature: std::string and
 * std::vector have no guaranteed layout, std::shared_ptr's control block is an
 * implementation detail, and none of them fails at link time.
 *
 * The rules this header follows, each for a reason rather than a convention:
 *
 *   - fixed-width integers only; no int, no size_t in a stored field, no enum
 *     without an explicit underlying representation
 *   - no bool in a struct that crosses (its size is implementation-defined)
 *   - every buffer is a pointer plus an explicit length, and is borrowed for
 *     the duration of the call and no longer
 *   - allocation is paired: whoever produced a pointer destroys it, because the
 *     two binaries have different heaps
 *   - nothing throws; errors are return values
 *
 * SWY_PLUGIN_ABI_LEVEL exists so that a plugin can genuinely be built against
 * an older, shorter version of this struct, rather than merely pretending to be
 * by setting a smaller struct_size. plugin_v1 compiles at level 1 and does not
 * have the process_batch field at all.
 */

#ifndef SWITCHYARD_PLUGIN_ABI_H
#define SWITCHYARD_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifndef SWY_PLUGIN_ABI_LEVEL
#define SWY_PLUGIN_ABI_LEVEL 2
#endif

#if defined(_WIN32)
#define SWY_EXPORT __declspec(dllexport)
#else
#define SWY_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped only for a change that is not expressible through struct_size. Adding
 * a function to the end is NOT such a change; removing one, or altering the
 * meaning of an existing one, is. */
#define SWY_ABI_VERSION 1u

/* A borrowed view of one packet. The host owns the bytes; they are valid for
 * the duration of the call and must not be retained. Nothing in the interface
 * makes retaining them useful, which is a better defence than a comment. */
typedef struct SwyPacketView {
    uint16_t       protocol;
    uint16_t       reserved;
    uint32_t       bytes_len;
    const uint8_t* bytes;
} SwyPacketView;

/* Written into caller-provided storage. Not returned by value, not allocated by
 * the callee: a result the plugin allocated would have to be freed by the
 * plugin, which is one more crossing per packet for no gain. */
typedef struct SwyResult {
    uint64_t value;
    uint16_t handler;
    uint16_t reserved0;
    uint32_t reserved1;
} SwyResult;

/* Errors are return values because an exception cannot cross this boundary.
 * Explicitly int32_t rather than an enum: an enum's underlying type is a
 * compiler's choice, and this struct's layout is a promise. */
#define SWY_OK          ((int32_t)0)
#define SWY_UNSUPPORTED ((int32_t)1)
#define SWY_ERROR       ((int32_t)2)

typedef struct SwyPluginApi {
    /* Extension mechanism 1: filled in by the plugin, read by the host, so a
     * new host can tell how much of this struct an old plugin actually has.
     * It has to be present from version 1 or it can never be added. */
    uint32_t struct_size;
    /* Extension mechanism 2: the gate for changes struct_size cannot express. */
    uint32_t abi_version;

    /* Declarative registration. The host builds its dispatch table before any
     * traffic arrives, so it cannot discover what a plugin handles by calling a
     * predicate on packets — there are no packets yet. The plugin must say.
     * Returns the number of keys written, or the number required if cap is too
     * small. */
    uint16_t (*claimed_keys)(uint16_t* out, uint16_t cap);

    void* (*create)(void);
    void  (*destroy)(void* self);

    int32_t (*process)(void* self, const SwyPacketView* pkt, SwyResult* out);

#if SWY_PLUGIN_ABI_LEVEL >= 2
    /* Appended, not inserted. An old host ignores it; a new host must check
     * struct_size before reading it. Batching exists because a boundary
     * crossing that costs a meaningful share of a sub-microsecond budget cannot
     * be paid per packet. */
    int32_t (*process_batch)(void* self, const SwyPacketView* pkts, size_t n,
                             SwyResult* out);
#endif
} SwyPluginApi;

/* The one exported symbol. Everything else in the plugin stays hidden, so the
 * loader has no opportunity to interpose one binary's definition on the
 * other's. */
#define SWY_PLUGIN_ENTRY_NAME "switchyard_plugin_entry"
typedef const SwyPluginApi* (*SwyPluginEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* SWITCHYARD_PLUGIN_ABI_H */
