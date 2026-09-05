#pragma once
//
// One class definition, compiled into both binaries. Byte for byte identical in
// each — which is precisely what makes the result interesting.

struct Base {
    virtual ~Base() = default;
    virtual int tag() const = 0;
};

struct Derived : Base {
    int tag() const override { return 42; }
};
