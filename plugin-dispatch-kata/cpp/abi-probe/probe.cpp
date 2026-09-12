// The ABI failure modes that only exist on ELF, asserted rather than narrated.
//
// This does not build or run on the Windows target the rest of this kata is
// measured on: RTLD_LOCAL, RTLD_GLOBAL and the type_info identity rules are
// POSIX/ELF concepts with no PE equivalent. It is run under WSL, exactly as the
// mpsc kata runs ThreadSanitizer there for the same reason — the tool that
// demonstrates the point does not exist on the primary target.
//
// Each probe exits non-zero if the phenomenon it claims does NOT occur. A probe
// that merely printed what it saw would pass whether or not the thing being
// demonstrated is real.

#include "shared.hpp"

#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <typeinfo>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) ++g_failures;
}

using MakeFn = Base* (*)();
using KindFn = const char* (*)();
using AddrFn = const void* (*)();
using NewFn  = void* (*)();

struct Observation {
    bool        cast_ok;
    bool        typeid_equal;
    const void* their_typeinfo;
};

Observation probe(int mode, const char* mode_name) {
    void* h = ::dlopen("./libhandler.so", RTLD_NOW | mode);
    if (!h) {
        std::printf("  FAIL  dlopen(%s): %s\n", mode_name, ::dlerror());
        ++g_failures;
        return Observation{false, false, nullptr};
    }

    auto make = reinterpret_cast<MakeFn>(::dlsym(h, "make_derived"));
    auto kind = reinterpret_cast<KindFn>(::dlsym(h, "derived_type_name"));
    auto addr = reinterpret_cast<AddrFn>(::dlsym(h, "derived_typeinfo_address"));
    if (!make || !kind || !addr) {
        std::printf("  FAIL  dlsym(%s)\n", mode_name);
        ++g_failures;
        ::dlclose(h);
        return Observation{false, false, nullptr};
    }

    Base* obj = make();

    // The object was constructed in the shared object from a class definition
    // identical to the one this binary compiled. Whether the host can recognize
    // it depends on whether the two binaries agree that the two type_infos are
    // the same type — which is a linkage question, not a language question.
    Derived* d = dynamic_cast<Derived*>(obj);

    const bool  same_typeid = (typeid(*obj) == typeid(Derived));
    const void* theirs      = addr();
    const void* mine        = &typeid(Derived);

    std::printf("  %-12s cast %-4s  typeid== %-5s  type_info @ host %p / plugin %p %s\n",
                mode_name, d ? "ok" : "NULL", same_typeid ? "true" : "false", mine, theirs,
                mine == theirs ? "(shared)" : "(DISTINCT)");
    (void)kind;

    delete obj;
    ::dlclose(h);
    return Observation{d != nullptr, same_typeid, theirs};
}

}  // namespace

int main() {
    std::printf("switchyard abi-probe — ELF-only failure modes (run under WSL)\n\n");

    std::printf("== type identity across a DSO ==\n");
    const Observation local  = probe(RTLD_LOCAL, "RTLD_LOCAL");
    const Observation global = probe(RTLD_GLOBAL, "RTLD_GLOBAL");

    // What this probe originally asserted, and what actually happens.
    //
    // The folklore — repeated in a great many places, and in the first draft of
    // this file — is that a class built into a shared object with hidden
    // visibility and loaded RTLD_LOCAL gets a second, distinct type identity,
    // so dynamic_cast across the boundary silently returns null.
    //
    // Half of that is true here and the conclusion is not. The two binaries DO
    // hold two distinct type_info objects. The cast succeeds anyway, because
    // this libstdc++ is built with __GXX_MERGED_TYPEINFO_NAMES = 0, which makes
    // type_info::operator== fall back to strcmp on the mangled name rather than
    // comparing the addresses. The implementation defends against exactly this.
    //
    // So the mechanism is real and the failure is a configuration away, not a
    // certainty: on a toolchain that compares type_info by address only, this
    // identical code returns null. Asserting the folklore would have been
    // asserting something this platform does not do.
    std::printf("\n");
    check(local.cast_ok && global.cast_ok,
          "dynamic_cast across the DSO succeeds under BOTH RTLD_LOCAL and RTLD_GLOBAL");
    check(local.typeid_equal,
          "typeid comparison agrees across the boundary despite hidden visibility");
#ifdef __GXX_MERGED_TYPEINFO_NAMES
    std::printf("  note  __GXX_MERGED_TYPEINFO_NAMES = %d\n", __GXX_MERGED_TYPEINFO_NAMES);
    check(__GXX_MERGED_TYPEINFO_NAMES == 0,
          "type_info comparison falls back to strcmp, which is WHY the cast works");
#else
    std::printf("  note  __GXX_MERGED_TYPEINFO_NAMES undefined\n");
#endif

    std::printf("\n== allocator identity ==\n");
    void* h = ::dlopen("./libhandler.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        auto opnew = reinterpret_cast<NewFn>(::dlsym(h, "plugin_operator_new_address"));
        if (opnew) {
            void* mine =
                reinterpret_cast<void*>(static_cast<void* (*)(std::size_t)>(::operator new));
            void* theirs = opnew();
            std::printf("  host operator new   %p\n  plugin operator new %p\n", mine, theirs);
            std::printf("  %s\n",
                        mine == theirs
                            ? "both resolve to the shared libstdc++, so one heap here. A plugin\n"
                              "  linking the C++ runtime statically would not, and then a pointer\n"
                              "  freed on the wrong side crosses heaps — which is why the ABI\n"
                              "  pairs create() with destroy() instead of letting the host delete."
                            : "different functions: a pointer freed on the wrong side crosses heaps");
        }
        ::dlclose(h);
    }

    std::printf("\n%s\n", g_failures == 0 ? "all probes passed" : "PROBES FAILED");
    return g_failures == 0 ? 0 : 1;
}
