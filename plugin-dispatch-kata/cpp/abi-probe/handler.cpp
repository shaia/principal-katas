// The shared object side. Compiled from the same shared.hpp as the host, with
// hidden visibility — which is what every sane plugin build does, and what
// makes the host unable to recognize the type it gets back.

#include "shared.hpp"

#include <cstddef>
#include <new>
#include <typeinfo>

#define EXPORT __attribute__((visibility("default")))

extern "C" EXPORT Base* make_derived() { return new Derived(); }

extern "C" EXPORT const char* derived_type_name() { return typeid(Derived).name(); }

// The address of THIS binary's type_info for Derived. If it differs from the
// host's, the two binaries genuinely hold two distinct type_info objects — and
// whether that matters depends entirely on how type_info::operator== is
// implemented, which is the point the probe makes.
extern "C" EXPORT const void* derived_typeinfo_address() { return &typeid(Derived); }

// The plugin's own operator new, so the host can compare function addresses.
extern "C" EXPORT void* plugin_operator_new_address() {
    return reinterpret_cast<void*>(static_cast<void* (*)(std::size_t)>(::operator new));
}
