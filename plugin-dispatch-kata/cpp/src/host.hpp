#pragma once
//
// The host side of the boundary: load, negotiate, and adapt.
//
// The adapter at the bottom of this file is the two-plane split made concrete.
// What crossed the boundary was a C struct of function pointers with POD
// arguments, chosen so it survives a different compiler three years from now.
// What goes into the dispatch table is an ordinary C++ object the optimizer
// understands. Nothing requires those to be the same mechanism, and the whole
// answer is that they should not be.

#include "handler.hpp"
#include "plugin_abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace switchyard {

// Offset arithmetic is how a host reads a struct an older plugin may have
// truncated. offsetof + sizeof of the field is the whole test.
inline constexpr std::size_t kBatchFieldEnd =
    offsetof(SwyPluginApi, process_batch) + sizeof(void*);

class LoadedPlugin {
  public:
    LoadedPlugin() = default;
    LoadedPlugin(const LoadedPlugin&)            = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;
    ~LoadedPlugin() { unload(); }

    bool load(const std::string& path) {
        unload();
#if defined(_WIN32)
        handle_ = ::LoadLibraryA(path.c_str());
        if (!handle_) { error_ = "LoadLibraryA failed"; return false; }
        auto entry = reinterpret_cast<SwyPluginEntry>(
            reinterpret_cast<void*>(::GetProcAddress(handle_, SWY_PLUGIN_ENTRY_NAME)));
#else
        // RTLD_LOCAL deliberately: the plugin's symbols must not be visible to
        // the rest of the process, nor the process's to it. It is also what
        // makes cross-binary RTTI unreliable — see solution.md section 8.
        handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) { error_ = "dlopen failed"; return false; }
        auto entry = reinterpret_cast<SwyPluginEntry>(
            ::dlsym(handle_, SWY_PLUGIN_ENTRY_NAME));
#endif
        if (!entry) { error_ = "entry symbol not found"; unload(); return false; }

        api_ = entry();
        if (!api_) { error_ = "entry returned null"; unload(); return false; }

        // Version gate first: struct_size cannot express a change in what an
        // existing function *means*, so a major mismatch is refused outright
        // rather than probed field by field.
        if (api_->abi_version != SWY_ABI_VERSION) {
            error_ = "abi_version mismatch";
            api_   = nullptr;
            unload();
            return false;
        }
        // Then the size gate. A plugin older than this host reports a shorter
        // struct; every field beyond that point is absent, not merely unset,
        // and reading one would be reading whatever follows the object.
        if (api_->struct_size < offsetof(SwyPluginApi, process)) {
            error_ = "struct_size below the version-1 minimum";
            api_   = nullptr;
            unload();
            return false;
        }
        return true;
    }

    void unload() {
        // Deliberately does NOT unmap. See solution.md section 7: correct
        // unloading requires proving that no in-flight call, no retained
        // pointer, no registered callback and no thread the plugin created is
        // still inside its code, and this design cannot prove any of that. The
        // handle is dropped and the mapping stays for the life of the process,
        // which is a cost stated rather than a bug hidden.
        api_ = nullptr;
        handle_ = nullptr;
    }

    bool ok() const { return api_ != nullptr; }
    const std::string& error() const { return error_; }
    const SwyPluginApi* api() const { return api_; }

    // Whether this plugin is new enough to have the batch entry point. The
    // question a host asks of an old plugin, answered by arithmetic on a field
    // that had to exist in version 1 or could never be added.
    bool has_batch() const { return api_ && api_->struct_size >= kBatchFieldEnd; }

    std::vector<std::uint16_t> claimed_keys() const {
        if (!api_) return {};
        const std::uint16_t need = api_->claimed_keys(nullptr, 0);
        std::vector<std::uint16_t> out(need);
        if (need) api_->claimed_keys(out.data(), need);
        return out;
    }

  private:
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    const SwyPluginApi* api_ = nullptr;
    std::string         error_;
};

// The adapter: a plugin, wearing the internal interface.
//
// Note what it does NOT do. It does not expose the C struct to the dispatch
// table, it does not let the plugin's types into the host's type system, and it
// does not let the plugin's memory become the host's problem — create() and
// destroy() are paired across the same boundary that produced them.
class PluginBackedHandler final : public Declaring {
  public:
    PluginBackedHandler(const SwyPluginApi* api, std::uint16_t key)
        : api_(api), key_(key), self_(api->create()) {}

    ~PluginBackedHandler() override {
        if (self_) api_->destroy(self_);
    }
    PluginBackedHandler(const PluginBackedHandler&)            = delete;
    PluginBackedHandler& operator=(const PluginBackedHandler&) = delete;

    bool valid() const { return self_ != nullptr; }

    bool matches(const Packet& p) const override { return p.protocol == key_; }

    Result process(const Packet& p) override {
        // The packet is handed across as a borrowed view with an explicit
        // length. The host owns the bytes for exactly the duration of this
        // call, and the interface gives the plugin nothing it could usefully
        // retain them for.
        const SwyPacketView view{p.protocol, 0, static_cast<std::uint32_t>(sizeof(p.payload)),
                                 p.payload};
        SwyResult out{};
        const int32_t rc = api_->process(self_, &view, &out);
        if (rc != SWY_OK) return Result::unsupported();
        return Result{out.value, out.handler};
    }

    Registration declare() const override { return Registration{key_, false}; }

  private:
    const SwyPluginApi* api_;
    std::uint16_t       key_;
    void*               self_;
};

}  // namespace switchyard
