#pragma once
//
// The brief's interface, and the one thing it cannot express.

#include "packet.hpp"

#include <cstdint>

namespace switchyard {

// Verbatim from the question. Nothing is added to it, because half the answer is
// about what this interface makes impossible.
struct Handler {
    virtual ~Handler() = default;

    virtual bool   matches(const Packet&) const = 0;
    virtual Result process(const Packet&)       = 0;
};

// What a table needs and `matches()` cannot provide.
//
// The host must build the dispatch table *before* traffic arrives, so it cannot
// discover which keys a handler claims by calling its predicate on packets — it
// would need packets to do that, and the table has to exist first. A handler
// therefore has to *declare* what it handles rather than demonstrate it.
//
// That constraint is the reason step 4 has to be answered before step 2 is
// finished, and it is the same constraint that shapes the plugin ABI: whatever
// a handler declares must be expressible in a vocabulary far smaller than C++,
// because across the boundary it will be two integers in a C struct.
struct Registration {
    std::uint16_t key;
    // True when matches() is key-equality AND something else. Such a handler
    // cannot own a table slot outright; it goes in the residual list the slot
    // points at. Bounding how many of these exist is a design constraint, not
    // an implementation detail.
    bool residual;
};

struct Declaring : Handler {
    virtual Registration declare() const = 0;
};

}  // namespace switchyard
