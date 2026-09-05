// Phase 5 — code size per mechanism.
//
// Step 3 asks each option's cost in binary size and instruction cache "with
// 50-100 handlers, not two", and two of the four options make code by
// multiplying it. This reads what llvm-size measured at build time on four
// binaries that differ only in which dispatch mechanism they contain.
//
// `none` holds the same 100 handler bodies and no dispatch machinery, so every
// other row minus `none` is the machinery, not the handlers.
//
// If llvm-size was not found at configure time the file is absent and this
// phase says so. A missing measurement is reported as missing; it is not
// replaced with an estimate.

#include "harness.hpp"
#include "phases.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace switchyard::bench {
namespace {

struct SizeRow {
    std::string name;
    long long   text = 0;
};

// llvm-size's Berkeley format: a header line, then "text data bss dec hex file".
std::vector<SizeRow> read_sizes(const char* path) {
    std::vector<SizeRow> rows;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return rows;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        long long text = 0, data = 0, bss = 0, dec = 0;
        char hex[64] = {0}, file[512] = {0};
        if (std::sscanf(line, "%lld %lld %lld %lld %63s %511s", &text, &data, &bss, &dec, hex,
                        file) != 6)
            continue;
        // Reduce the path to the probe's name: .../size_virtual.exe -> virtual
        std::string nm(file);
        const auto slash = nm.find_last_of("/\\");
        if (slash != std::string::npos) nm = nm.substr(slash + 1);
        const auto dot = nm.find_last_of('.');
        if (dot != std::string::npos) nm = nm.substr(0, dot);
        if (nm.rfind("size_", 0) == 0) nm = nm.substr(5);
        rows.push_back(SizeRow{nm, text});
    }
    std::fclose(f);
    return rows;
}

}  // namespace

void phase_codesize() {
    // The binary runs from bin/ but may be invoked from anywhere.
    static const char* kCandidates[] = {"sizes.txt", "bin/sizes.txt", "../bin/sizes.txt",
                                        "cpp/bin/sizes.txt"};
    std::vector<SizeRow> rows;
    for (const char* p : kCandidates) {
        rows = read_sizes(p);
        if (!rows.empty()) break;
    }

    if (rows.empty()) {
        std::printf("  sizes.txt not found — llvm-size was unavailable at configure time,\n"
                    "  so this phase has no measurement and does not offer an estimate.\n");
        return;
    }

    long long base = 0, empty = 0;
    for (const auto& r : rows) {
        if (r.name == "none") base = r.text;
        if (r.name == "empty") empty = r.text;
    }

    std::printf("  %-12s | %10s %12s\n", "binary", ".text B", "vs none B");
    for (const auto& r : rows) {
        if (r.name == "none" || r.name == "empty")
            std::printf("  %-12s | %10lld %12s\n", r.name.c_str(), r.text, "-");
        else
            std::printf("  %-12s | %10lld %+12lld\n", r.name.c_str(), r.text, r.text - base);
    }

    check(base > 0 && empty > 0, "phase 5: the baseline binaries were measured");
    if (base > 0 && empty > 0) {
        const long long handlers = base - empty;
        std::printf("\n  handler set alone: %lld B for %d bodies, %lld B each.\n", handlers,
                    kHandlers, handlers / kHandlers);
        // Phase 4's interleaved column only means something if the handler set
        // does not fit in L1i. 32 KiB is the smallest L1i this is likely to run
        // on. Asserted here rather than in phase 0, because this is the number
        // the linker actually produced.
        check(handlers > 32 * 1024,
              "phase 5: the handler set exceeds a 32 KiB L1i, so phase 4's mixed column means something");
    }
    std::printf("  empty is a C++ program with no handlers; none adds the 100 bodies and no\n"
                "  dispatch. Every row below none holds the same bodies, so the difference\n"
                "  from none is the dispatch machinery alone.\n");
}

}  // namespace switchyard::bench
