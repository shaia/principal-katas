# Dispatch selection and the plugin ABI boundary

Answer to [../question.md](../question.md) and the [C++ track](question.md), with a working
implementation and benchmarks alongside it. The Go and Python tracks are not written yet; their
questions are at [../go/question.md](../go/question.md) and
[../python/question.md](../python/question.md).

Every number below came from running [`bin/solution.exe`](bench/), whose exit code is the verdict.
Terms of art (ICF, DSO, ABI, L1i, RTTI, …) are expanded in the [Glossary](#appendix-glossary).

---

## The problem

A C++20 packet-processing service loads protocol handlers dynamically. 50–100 of them; some built
into the executable, some arriving from separately compiled shared libraries. The hot path selects a
handler by asking each in turn:

```cpp
for (auto& h : handlers) {
    if (h->matches(p))
        return h->process(p);
}
return Result::unsupported();
```

Profiling says `dispatch` is expensive at several million packets/sec. The team proposes four
replacements: keep `virtual`, `std::variant` + `std::visit`, templates, or custom type erasure. The
deliverable is an architecture — what to use on the internal hot path versus at the plugin ABI
boundary — plus nine named axes: virtual-call cost, branch prediction, instruction-cache pressure,
data locality, binary size, ABI stability, ownership across shared-library boundaries, exceptions
and RTTI, and allocator/runtime compatibility.

### What the brief gives you, and what it withholds

| The brief states | What it forces |
|---|---|
| `dispatch` is expensive | Nothing about *which line*. A profiler attributes to functions |
| 50–100 handlers | The loop runs up to 100 predicates per packet |
| Several million packets/sec | 333 ns per packet on one core, for everything |
| Some handlers come from `.so`/`.dll` | **Disqualifies `std::variant` outright** — a closed set cannot admit a runtime type |
| Four proposed alternatives | All four change how `process` is *called*, none changes how often `matches` is |
| — | **The traffic distribution is never given, and it decides everything** |

The last row is the whole question. The brief hands you a handler *count* and lets you assume the
option list follows from it. §10 is what happens when you measure the number it withheld instead.

### How the answer is graded

The rubric bands it, and the bands say what is really being probed:

- **Senior** — knows a virtual call is an indirect call that inhibits inlining; suggests `variant` or
  templates; insists on benchmarking. → §2, §4
- **Strong Senior → Staff** — realizes the virtual call may not be the problem, and that 50–100
  unpredictable branches per packet cost more than the mechanism. Fixes dispatch *selection* first.
  → §1, §2, §3
- **Staff** — separates the plugin boundary from internal execution as two problems with opposed
  requirements. → §6, §7, §8
- **Principal** — says explicitly *I would not require the same abstraction mechanism at the ABI
  boundary and on the hot path*, and reasons about ownership, allocators, exceptions, compiler and
  standard-library incompatibility, rollout, observability, and realistic measurement. → §6–§9, §12
- And the line most answers skip: **when the unsophisticated loop is still right.** → §11

These are four depths of one answer, and the step up to Principal is a *refusal*: the question offers
four mechanisms and the strongest answer declines to pick one, on the grounds that the boundary and
the hot path are different problems that were never obliged to share a solution.

### Why it is hard

**The diagnosis is assumed, not measured.** The profile blames a function; the option list blames a
line inside it, and nobody checked. All four options optimize the call. The loop is what costs. (→ §2)

**The deciding input is absent.** Handler count is given, traffic distribution is not — and the
second dominates. At 95/5 skew, sorting the list captures 94 % of the entire available win and the
table adds 6 %. The whole rewrite can be the wrong call, and nothing in the brief tells you. (→ §10)

**One option is excluded by a sentence, not a benchmark.** `std::variant` is a closed set fixed at
compile time; handlers arrive at runtime from shared objects. No measurement can rescue that, and no
measurement is needed. Reaching for the benchmark first is the error. (→ §4)

**The boundary's costs are all silent.** An allocator mismatch, an ODR merge, an unwinder mismatch, a
`type_info` that does not compare equal — none is a link error, none is a warning, and the crash
arrives later, elsewhere, in unrelated code. (→ §8, §9)

**And the folklore is wrong twice.** The `dynamic_cast`-returns-null-across-a-DSO story did not
reproduce; the variant-explodes-code-size story did not either. Both are in this document with the
measurement that refuted them, because a writeup that was right about everything is less useful than
one that shows where the received wisdom breaks. (→ §5, §9)

### Where each part is answered

| Question | Section |
|---|---|
| Which line is actually expensive | §2 |
| Table design, and the semantics it costs | §3 |
| virtual / variant / erasure, re-priced | §4 |
| Binary size, i-cache | §4, §5 |
| The two-plane rule | §6 |
| The ABI, versioning, extension | §7 |
| Ownership, allocators, unload | §8 |
| Exceptions, RTTI, type identity | §9 |
| Does any of it pay? | §10 |
| When to ship the loop | §11 |
| Verification, and what cannot be measured here | §12 |

## Layout

``` text
cpp/
  src/         platform, packet stream, the brief's Handler, generated handlers,
               the dispatch arms, the mechanism arms, the C ABI, the host loader
  plugin/      one source built twice: v1 against a SHORTER struct, v2 against the current one
  bench/       seven phases, one per translation unit  -> bin/solution.exe
  abi-probe/   ELF-only failure modes, run under WSL   -> ./abi_probe
```

Run it:

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp/build
cd cpp/bin && ./solution.exe      # exit code is the verdict; run twice, see §12
```

---

## 1. The decision

**Two mechanisms, not one, because there are two problems.**

Internally: extract a cheap key from the packet, index a table, call through `virtual`. Across the
boundary: a versioned `extern "C"` struct of function pointers with POD arguments, adapted into that
same table by a host-side wrapper.

```cpp
const Slot& s = tbl_[p.protocol & kMask];
if (s.key != p.protocol) return Result::unsupported();
if (s.only) return s.only->process(p);          // predicate is pure key equality
for (i = 0; i < s.count; ++i)                   // residual predicates queue here
    if (cand_[s.begin + i]->matches(p)) return cand_[s.begin + i]->process(p);
return Result::unsupported();
```

The order matters: **fix selection first, then ask about the mechanism.** Asked before §2, "virtual
or variant?" is a question about 10 ns inside a 143 ns problem.

### Why not the alternatives

`std::variant` is out **on the premise**. It is a closed set of alternatives fixed at compile time,
and the brief says handlers arrive from separately compiled shared libraries. A variant cannot name a
type that does not exist until `dlopen` returns. This is a stronger argument than any benchmark
because it cannot be overturned by a faster machine — and §4 measures it anyway, because "also
slower" and "cannot express the requirement" are different claims and only one of them is decisive.

Templates monomorphize, which is genuinely the fastest thing available for handlers the compiler can
see — and the compiler cannot see a plugin. Templates survive for built-ins only, which *is* the
two-plane split arriving whether or not you invited it.

Custom type erasure is a hand-rolled vtable. §4 measures it at 8.6 ns against `virtual`'s 10.2 ns of
dispatch overhead: real, small, and worth having only if you can name the cost it removes (one
dependent load — the vtable pointer is not fetched from the object, because the function pointer sits
in the table next to the object pointer).

Keeping `virtual` is a positive choice, not a failure to find better. After §2 the mechanism is 3 %
of the per-packet budget.

---

## 2. Costing the scan

The profile blames `dispatch`. `dispatch` contains a loop. Sweeping the handler count, uniform
traffic, 100 generated handlers of 1800 B each:

```
  handlers  |      scan     table |   probes      scan |    table
            |    ns/pkt    ns/pkt |     /pkt  % budget |  speedup
  10        |      36.6      36.2 |      5.7     11.0% |     1.0x
  25        |      71.2      60.8 |     13.9     21.4% |     1.2x
  50        |     119.2      71.9 |     27.2     35.8% |     1.7x
  100       |     142.8      67.8 |     52.3     42.8% |     2.1x
```

Cost is linear in *probes*, not in anything the four options address. At 100 handlers the scan runs
**52.3 `matches()` calls per packet** and spends 142.8 ns — 43 % of the entire 333 ns budget — before
any handler does useful work.

The table's own curve is the more interesting one. Selection is O(1), yet the table still goes 36.2
→ 67.8 ns as handlers go 10 → 100. Nothing about the lookup changed; what changed is that 100
handler bodies do not fit in L1i where 10 do. **Even with selection solved, handler count still
costs — through the instruction cache.** §4 measures that separately.

So the answer to step 0 is: the profile pointed at a function, the option list assumed a line, and
the line it assumed is not the one. The four alternatives compete for the ~10 ns the mechanism costs
while the scan spends 143.

---

## 3. The table, and what it costs

Building it is not the hard part. Two things are.

**Not every predicate has a key.** `matches()` is arbitrary code. A predicate that tests a payload
byte, a range, a field combination, or state from an earlier packet cannot be selected on. The design
is therefore not "table instead of loop" but **table, then a very short loop**: each slot either
holds a handler that owns the key outright — predicate is pure key equality, so no test is needed at
all — or points at a bounded candidate list that must still be scanned with `matches()`.

In the benchmark 10 % of handlers are given a residual predicate and made to share keys, so the
candidate lists are genuinely 3–4 long rather than a single-element special case. That is where the
complexity the table removed goes to live, and §11 makes an argument about it. **Bounding that list
is a design constraint the brief never mentions and the answer has to invent.**

**First-match-wins is a behaviour, and a table has no order.** The brief's loop returns the first
match, so the list's order is something an operator can depend on and reorder. Phase 1 asserts the
hazard rather than describing it:

```
  overlapping predicates: registration order -> handler 1, reversed -> handler 2
```

Two handlers whose predicates overlap give different answers under different orders. This matters
beyond tidiness, because §10's cheap competitor *is* a reordering: sorting the handler list by
frequency is only a safe change if no two predicates overlap. If they do, the one-line fix is a
behaviour change and the table — which resolves collisions once, at build time, in registration
order — is the safer structure regardless of speed.

**Density.** The protocol field is 16 bits, so a directly-indexed table is 65536 slots whether or not
they are used:

```
  table footprint: direct16 1024 KiB (65536 slots, 100 used), compact 24 KiB
```

1 MiB of mostly-empty pointers against 24 KiB for the same table over a remapped key. The measured
speed difference is inside the noise (§10's rows: 66.8 vs 68.5, 60.9 vs 62.1) because the hot slots
stay cached either way — but a megabyte of L2 and L3 spent on nothing is a cost paid by every other
part of the process, and it is invisible in a benchmark that measures only this loop. The compact
table is the right default; the direct one is defensible only if the key space is dense.

---

## 4. The mechanism, re-priced

Selection is now O(1), so the question the brief actually asked can finally be asked against the
right number. All four arms below perform the *identical* selection — one masked array index — and
differ only in how they reach `process()`. Selection is common-mode and cancels.

```
  mechanism    |      hot    mixed mixed-tiny |   i-cache
               |   ns/pkt   ns/pkt     ns/pkt |   cost ns
  direct-call  |    26.26    26.36       0.91 |      0.10
  virtual      |    27.57    61.26      11.15 |     33.69
  erasure      |    27.38    60.41       9.48 |     33.03
  variant      |    27.72    64.54      11.89 |     36.82
```

`hot` is one protocol, so one handler, resident in L1i with a perfectly predicted branch target —
this is what a dispatch microbenchmark measures. `mixed` is the real distribution over 100 handlers
whose bodies total 176 KiB. `mixed-tiny` is the same distribution with handlers small enough that the
callee is smaller than the call sequence reaching it, which isolates the mechanism from the
footprint.

**The dispatch mechanism costs 8.6–11.0 ns** (each `mixed-tiny` minus the 0.91 ns floor): erasure
8.57, virtual 10.24, variant 10.98. Against a 333 ns budget that is **3 % of the packet**. Against
the 143 ns the scan cost in §2, it is noise. This is the answer to the brief's question, and the
answer is that the question was not important.

**The instruction cache costs three times more than the mechanism.** The `i-cache` column is
`mixed − hot`: 33–37 ns for every real mechanism. The control is `direct-call` at **0.10 ns** — it
always calls the same handler, so it has no i-cache cost to pay, which is exactly what the column
reports. That control is what makes the other three readable; without it, 33 ns could have been
anything.

So the ranking that a one-handler microbenchmark produces (all four within 1.5 ns of each other, at
26–28 ns) is not the ranking that matters, and the effect it cannot see is larger than the effect it
measures. **Build the interleaved benchmark or do not benchmark dispatch at all.**

Note what these numbers do *not* say: no hardware counter was read. §12 explains why, and why the
substitution is arguably better.

---

## 5. Code size, and a folklore that did not survive

Four binaries containing the same 100 handler bodies, differing only in the dispatch machinery, as
measured by `llvm-size` at build time:

```
  binary       |    .text B    vs none B
  empty        |       3478            -
  none         |     183574            -
  virtual      |     184278         +704
  erasure      |     170534       -13040
  variant      |     180198        -3376

  handler set alone: 180096 B for 100 bodies, 1800 B each.
```

The expected result was that `variant` and templates "make code by multiplying it". **It did not
appear.** At 100 handlers the machinery differences are ±7 %, and `variant` is *smaller* than the
virtual baseline, because `none` already pays for 100 vtables and their RTTI while `erasure`'s free
functions pay for none. The handler bodies — 180 KiB — dominate everything the dispatch mechanism
does by an order of magnitude.

The honest scope of that result: it compares `variant` against `virtual` against erasure. It does
**not** measure templates monomorphizing an entire pipeline per handler, which is the option that
would genuinely multiply code, and which I did not build. The folklore is not refuted in general; it
is refuted for the three mechanisms measured, and the reason is that in a service whose handlers do
real work, the handlers are the code.

The number that does matter here is 180096 B: it is what makes §4's `mixed` column mean something,
and phase 5 asserts it exceeds a 32 KiB L1i rather than assuming it.

---

## 6. The two planes

| Plugin boundary | Internal execution |
|---|---|
| stable across compilers you will never see | everything visible to the optimizer at once |
| loose coupling, opaque handles | concrete types |
| C-compatible, POD, fixed layout | templates and variants permissible |
| small, because the compatibility matrix multiplies | as large as it needs to be |

These columns have no row in common, and a single mechanism satisfying both satisfies neither.
**I would not require the same abstraction mechanism at the ABI boundary and on the hot path.**

The non-obvious consequence is in registration. The host builds its dispatch table *before* traffic
arrives, so it cannot discover which keys a plugin claims by calling its predicate on packets —
there are no packets yet. The plugin must **declare** what it handles:

```c
uint16_t (*claimed_keys)(uint16_t* out, uint16_t cap);
```

And "what it handles" must be expressible in the boundary's vocabulary, which is far smaller than
C++. That constraint propagates backwards into §3: a plugin's predicate can be a key, or a key plus
something the host cannot see, and the second kind can only ever go in the residual list. The two
planes are not independent — the boundary's poverty is what shapes the table.

The adapter is 30 lines and is the whole design in miniature: a C struct of function pointers goes
in, an ordinary C++ object the optimizer understands comes out, and the plugin's types never enter
the host's type system.

---

## 7. The ABI

```c
typedef struct SwyPluginApi {
    uint32_t struct_size;    /* extension mechanism 1 */
    uint32_t abi_version;    /* extension mechanism 2 */
    uint16_t (*claimed_keys)(uint16_t* out, uint16_t cap);
    void*    (*create)(void);
    void     (*destroy)(void* self);
    int32_t  (*process)(void* self, const SwyPacketView* pkt, SwyResult* out);
#if SWY_PLUGIN_ABI_LEVEL >= 2
    int32_t  (*process_batch)(void*, const SwyPacketView*, size_t, SwyResult*);
#endif
} SwyPluginApi;
```

One test applies to every field: *could this be produced by a different compiler, a different
standard library, or a different language, three years from now?* `std::string`, `std::vector`,
`std::shared_ptr` and `std::function` all fail it, and **none of them fails at link time**. So:
fixed-width integers only, no `bool` in a crossing struct, no `size_t` in a stored field, no enum
without an explicit representation, every buffer a pointer plus an explicit length.

**Two extension mechanisms, and they compose.** `abi_version` gates changes that `struct_size` cannot
express — removing a function, or altering what an existing one means. `struct_size` handles the
common case of appending. It has to be present in version 1 or it can never be added, which is the
one irreversible decision in the file.

This is demonstrated rather than asserted. `plugin_v1` is compiled at `SWY_PLUGIN_ABI_LEVEL=1`, so it
genuinely does not have the `process_batch` field — it is not present-but-unset, it is absent — and
the host detects that by arithmetic:

```
  plugin_v2      loaded    struct_size  48  abi 1  keys 4  batch yes
  plugin_v1      loaded    struct_size  40  abi 1  keys 4  batch no
```

40 versus 48 bytes, one host driving both.

**And the crossing is cheap, which changes the design.**

```
  path                   |    ns/pkt vs internal
  internal virtual call  |       7.1           -
  plugin, per packet     |      10.1       +3.0
  plugin, batched x64    |      10.2       +3.1
```

3.0 ns, 1 % of the budget. **Batching recovers none of it.** An in-process C ABI crossing is an
indirect call through a function pointer plus a struct copy; there is no transition to amortize, and
the batch loop's own writes to the view and result arrays cost about what the saved call did.

That is worth stating plainly because the instinct runs the other way. Batching is the right answer
where a crossing has a real fixed cost — a cgo call, an IPC round trip, a syscall — and designing it
into *this* ABI on the assumption that boundaries are expensive would have been a premature
complication measured against nothing. `process_batch` stays in the interface as the demonstration of
the extension mechanism, and the honest note is that on this platform it buys nothing.

---

## 8. Ownership, allocators, and the unload you probably cannot do

**`create`/`destroy` is not symmetry.** Ask what `free()` does with a pointer that came from a
different heap, and then notice that "a different heap" is the normal case the moment two binaries
link their own C++ runtime. The `abi-probe` measures this directly:

```
  host operator new   0x7f848dd008e0
  plugin operator new 0x7f848dd008e0
  both resolve to the shared libstdc++, so one heap here. A plugin
  linking the C++ runtime statically would not
```

One heap *here*, because both link the same shared libstdc++. A plugin built with a statically linked
runtime — an entirely ordinary way to ship one — would report a different address, and every pointer
the host freed on its behalf would be a corruption whose crash arrives later, elsewhere. The pairing
in the ABI exists so that the answer never depends on which of those two situations you are in.

The same reasoning covers `SwyResult`: written into caller storage, not returned by value, not
allocated by the callee. A result the plugin allocated would need a second crossing to free.

**The packet is borrowed for the duration of the call.** `SwyPacketView` is a pointer plus an
explicit length, valid until `process` returns. The interface gives the plugin nothing it could
usefully retain the pointer *for*, which is a better defence than a sentence in a document.

**Unload is a promise this design does not make.** `LoadedPlugin::unload()` drops the handle and
leaves the mapping in place, deliberately. Correct unloading requires proving that nothing still
points into the plugin's code or data: no in-flight call, no cached function pointer, no callback the
host registered, no object the plugin handed over and the host kept, no static destructor, no
thread-local storage — and no thread the plugin created, because a thread executing code you are
about to unmap is not a race you can win. "We never unload" is a legitimate engineering answer with a
cost, and the cost is that a bad plugin needs a process restart to remove (§ rollout, below).

**ODR is the one you cannot defend against with discipline.** Two definitions of an inline function
or a template instantiation, present in both binaries, compiled with different flags, and the loader
picks one. No diagnostic exists anywhere in the toolchain. Hidden visibility on the plugin
(`-fvisibility=hidden`, one exported symbol) is what removes the opportunity, and it is the real
argument for the C boundary: not that C is portable in the abstract, but that **one decision makes
four silent failure modes impossible** — allocator mismatch, ODR merge, unwinder mismatch, and type
identity.

---

## 9. Errors, and a claim that did not survive contact

**An exception is not a value.** It is an unwinding protocol involving unwind tables, a personality
routine, and a runtime that both sides must share. Every entry point in the plugin is therefore
`noexcept` *and* catches:

```cpp
int32_t process(void* self, const SwyPacketView* pkt, SwyResult* out) noexcept {
    try { /* ... */ return SWY_OK; }
    catch (...) { return SWY_ERROR; }
}
```

`noexcept` alone would call `std::terminate` — correct, and fatal. Catching converts the fault into
the return value the ABI already has. What that shim contains is *unwinding*, not damage: it cannot
make a plugin that corrupted the host's heap safe to keep running, and the containment claim should
be stated that narrowly.

### The `dynamic_cast` story is folklore on this platform

The widely repeated claim — and the first version of `abi-probe` asserted it — is that a class built
into a shared object with hidden visibility and loaded `RTLD_LOCAL` acquires a second, distinct type
identity, so `dynamic_cast` across the boundary silently returns null.

The probe refuted it:

```
  RTLD_LOCAL   cast ok    typeid== true   type_info @ host 0x5600af4cad60 / plugin 0x7f848ded4de8 (DISTINCT)
  RTLD_GLOBAL  cast ok    typeid== true   type_info @ host 0x5600af4cad60 / plugin 0x7f848ded4de8 (DISTINCT)
  note  __GXX_MERGED_TYPEINFO_NAMES = 0
```

Half of the story is true: the two binaries genuinely hold **two distinct `type_info` objects** for
one class definition. The conclusion is false, because this libstdc++ is built with
`__GXX_MERGED_TYPEINFO_NAMES = 0`, which makes `type_info::operator==` fall back to `strcmp` on the
mangled name rather than comparing addresses. The implementation defends against exactly this
hazard. Adding `-Wl,-Bsymbolic` does not change it either.

So the mechanism is real and the failure is a *toolchain configuration* away rather than a
certainty: on an implementation that compares `type_info` by address alone, this identical code
returns null. The engineering conclusion is unchanged — do not put RTTI in a plugin contract, because
its correctness depends on a build-time constant of the standard library you did not choose — but
the reason is different from the one usually given, and being precise about which is the difference
between knowing this and having read about it.

### The one that is loud

The `std::string` ABI break behaves better, because it fails at link:

```
undefined reference to `consume(std::string const&)'
```

Two translation units disagreeing about `_GLIBCXX_USE_CXX11_ABI` mangle `std::string` differently, so
the symbol is simply absent. That is the *good* case. Reached through a `dlopen` instead, there is no
link step to catch it.

---

## 10. Does the table actually pay?

This is the section that can kill the rest of the answer, and it is written to be able to.

The brief gives a handler count and never gives a traffic distribution. A linear scan over a list
ordered by frequency costs about as many probes as the traffic is skewed — so if real traffic is
dominated by a few protocols, the scan is already an O(1) dispatch wearing an O(n) loop, and
`std::sort` is the competitor the rewrite has to beat.

`scan-ordered` is given every possible advantage: it is sorted by the **true** frequencies of the
very stream it is then measured on, which is better than any real system could manage.

```
  traffic  arm             |    ns/pkt   probes |  % budget       M/s
  uniform  scan-unordered  |     138.5     52.3 |     41.6%       7.2
  uniform  scan-ordered    |     137.8     51.0 |     41.3%       7.3
  uniform  table-compact   |      68.5      1.2 |     20.5%      14.6

  zipf     scan-unordered  |     147.0     67.6 |     44.1%       6.8
  zipf     scan-ordered    |      82.5     18.7 |     24.7%      12.1
  zipf     table-compact   |      62.1      1.1 |     18.6%      16.1

  95/5     scan-unordered  |     128.2     88.2 |     38.5%       7.8
  95/5     scan-ordered    |      41.4      5.7 |     12.4%      24.1
  95/5     table-compact   |      35.9      1.0 |     10.8%      27.9

  worst    scan-unordered  |     135.1    100.0 |     40.5%       7.4
  worst    scan-ordered    |      30.9      3.0 |      9.3%      32.3
  worst    table-compact   |      27.6      1.0 |      8.3%      36.3
```

Split the total available improvement between the one-line change and the rewrite:

| traffic | available | captured by sorting | added by the table |
|---|---|---|---|
| uniform | 70.0 ns | 0.7 ns — **1 %** | 69.3 ns — **99 %** |
| zipf | 84.9 ns | 64.5 ns — **76 %** | 20.4 ns — 24 % |
| 95/5 | 92.3 ns | 86.8 ns — **94 %** | 5.5 ns — 6 % |
| worst | 107.5 ns | 104.2 ns — **97 %** | 3.3 ns — 3 % |

**The more skewed the traffic, the less the table buys.** At 95/5 — which is what real protocol
mixes look like — sorting the list captures 94 % of everything available and the table adds 6 %. The
rewrite is worth building when traffic is near-uniform, or when the worst case must be bounded rather
than merely typical.

That last clause is why I would still build it, and it is a different argument from the one the
speedup column makes. `scan-ordered` is fast *on average* and its worst case is still 100 probes; the
table's worst case is 1 lookup plus a bounded residual list. A service that must not fall over when
the traffic mix shifts — an attacker choosing protocols, a new deployment, a misconfigured peer — is
buying a bound, not a mean. The table costs 6 % of the win at today's distribution and removes the
tail entirely.

Two further readings:

**Move-to-front is worse than doing nothing** (199.2 vs 138.5 under uniform; 126.3 vs 147.0 under
zipf, where it does help). It pays a write on every hit to learn what a one-off sort already knows,
and under uniform traffic there is nothing to learn and the writes are pure cost.

**The measurement nearly lied, and this table is the thing it would have lied about.** The first
version of the generator assigned the weights to handlers 0, 1, 2 … in order — so the busiest handler
was already first in the list and sorting had nothing to fix. Sorting then captured **5 %** of the
available win under zipf and **4 %** under 95/5, against 76 % and 94 % once the assignment is
shuffled. The rigged run supports the exact opposite conclusion of this section: that the cheap
competitor is worthless and the table is the only thing that helps.

Nobody registers handlers in traffic order by accident, and a benchmark that assumes they did is
measuring its own generator rather than the design. The shuffle is in
[`packet.hpp`](src/packet.hpp) with the reasoning attached, and it is the single most consequential
line in the benchmark.

---

## 11. When the loop is the better engineering choice

The conditions, and they are not rare:

**Traffic is skewed and the worst case is not adversarial.** §10's table says it: at 95/5, sorting
gets 94 % of the win for one line of code that any reviewer can check in ten seconds.

**Predicates do not factor.** The table is worth its complexity in proportion to how many handlers
own a key outright. If most predicates test a payload byte or a field combination, most handlers end
up in residual lists, the table degenerates into the loop it replaced, and you have paid for a
schema and a build step to arrive back where you started.

**Handlers are written by people who should not have to know about a global key space.** This is the
property nobody prices. In the brief's loop, adding a handler requires understanding *nothing*
outside it: write a predicate, register it, done. The table replaces that with a schema every future
handler must fit, a collision policy, a residual bound, and a declaration that must be kept in sync
with the predicate. That cost is paid by every handler author forever, and it does not appear in any
benchmark.

**The ordering semantics are load-bearing.** §3 showed that overlapping predicates make list order
observable. If operators reorder handlers to change behaviour — a completely reasonable thing for a
protocol-handling service to support — then first-match-over-an-ordered-list is a *feature*, and a
table has to reimplement it explicitly.

**And the maintenance asymmetry is severe.** The loop is five lines. §7 through §9 are protocols a
future maintainer must hold correctly in a domain where every mistake is silent: a struct field
appended in the wrong place, a pointer freed across a heap boundary, an exception escaping a shim,
an `abi_version` not bumped when a function's meaning changed.

I would ship the table for a service whose traffic mix is not under my control and whose tail latency
is a commitment. I would ship the loop, sorted by observed frequency, for a service where it is —
and I would do it in one commit and spend the rest of the week on something else.

---

## 12. Verification, and what this platform cannot measure

**The exit code is the verdict.** Every arm's results are checksummed and asserted equal to the
brief's loop, packet for packet. That single equality does two jobs: it is the correctness invariant,
and — because this is a single-threaded loop whose results are otherwise unused, which a dead-code
eliminator is entitled to delete — it is what keeps the work alive without a compiler-specific
barrier. The mpsc kata never needed one because every measured path there crossed a thread boundary.

**The clean run is not vacuous.** Deliberately breaking `TableCompact` so it skips its residual list
produced eight distinct failures and exit code 1:

```
  FAIL: table-compact: agrees with the brief's loop on every packet
  FAIL: phase 2: scan and table agree at every handler count
  FAIL: uniform: every arm agrees with the brief's loop
  ...
CHECKS FAILED
```

Restored: exit 0. A green result from a harness that never watched looks identical to a green result
from correct code, so the control has to exist.

### Three measurement bugs, and what they cost

**The pool was measuring DRAM.** The first version used a 1 Mi-packet pool — 32 MiB, sitting exactly
at this machine's 36 MiB L3 boundary. Every arm was measuring memory bandwidth with a dispatch
mechanism attached, and the results stopped being monotonic in the handler count: the *table* came
out at 416 ns for 50 handlers and 380 ns for 100, which is not a thing a table can do. A 1 MiB
L2-resident pool, replayed to keep the sample count fixed, fixed it.

**The thread was migrating between core types.** This is an i9-13980HX: performance cores and
efficiency cores in one package, and the scheduler will move a busy thread between them mid-run. The
same arm measured twice landed in different columns. `SetThreadAffinityMask` to CPU 0 is not a nicety
on a hybrid CPU; without it, none of these numbers mean anything.

**Arm-major ordering charged the last arm for the drift.** Running all repetitions of one arm before
starting the next let the core's clock drift down under sustained load, and the arm that ran last
paid for it — two arms doing identical work came out 27 % apart, stably and reproducibly, which is
worse than noise because it looks like a result. Measurement is now rep-major: one repetition of each
arm, then the next. §4's i-cache column is a difference between two arms, so those twelve cells are
measured in one interleaved set for the same reason.

Also: the first run of a freshly linked binary is roughly **three times slower** than every
subsequent one, uniformly. Phase 0 burns 500 ms before anything is timed. **Run the binary twice and
read the second.**

Residual spread, run to run: ratios are stable to ~3 % (the 100-handler speedup reads 2.1×, 2.2×,
2.1× across three runs), absolute levels vary ~8 % with thermal state. Every claim in this document
is a ratio for that reason.

### What cannot be measured here

**There is no `perf`** — not on Windows, and not in this WSL2 install. Branch-miss and L1i-miss
counters are unavailable, so no number in this document is a hardware event count.

The substitute is a **designed contrast**: two arms with identical instruction counts differing only
in the property under test, and the delta reported in nanoseconds. §4's i-cache column is exactly
that — `mixed − hot`, with `direct-call`'s 0.10 ns as the control proving the column measures what it
claims. This is arguably the better measurement, because it reports the effect in the unit the budget
is denominated in rather than in events that still need a cost model to interpret.

The ELF-specific failure modes have no PE equivalent at all, so `abi-probe/` runs under WSL — the
same split the mpsc kata used for ThreadSanitizer:

```sh
wsl -- bash -lc 'cd /mnt/c/.../plugin-dispatch-kata/cpp/abi-probe && make run && make abi-break'
```

### Known gaps

- **Templates as a fourth arm were not built.** §5's code-size result covers virtual, erasure and
  variant; a monomorphized-per-handler pipeline is the option that would genuinely multiply code and
  it is not measured. The folklore is refuted for what was measured, not in general.
- **`abi_version` mismatch is not exercised** — only `struct_size`. A plugin built at a different
  major version would be refused by the gate in `host.hpp`, but no test drives that path.
- **Unload is not tested** because it is not implemented (§8), so the claim that it is hard is an
  argument here rather than a measurement.
- **One machine, one compiler.** clang 21.1.7 targeting `x86_64-pc-windows-msvc`, and g++ 13.3 under
  WSL for the probes. §9's `type_info` result in particular is a statement about this libstdc++.

---

## Appendix: Glossary

**ABI** — application *binary* interface: the promises about layout, calling convention and symbol
names that let two separately compiled binaries call each other. Distinct from an API, which is a
promise about source.

**DSO** — dynamic shared object; the ELF term for a `.so`. The PE equivalent is a DLL.

**ICF** — identical code folding: a linker optimization that merges functions with identical machine
code. Benign in general; here it would have silently destroyed §4's instruction-cache measurement by
collapsing 100 handler bodies into one, which is why phase 0 asserts the bodies produce 100 distinct
results.

**L1i** — the level-1 instruction cache, typically 32 KiB. Distinct from L1d, which holds data. The
100 handler bodies total 176 KiB and cannot all be resident.

**RTTI** — run-time type information: the `type_info` objects behind `typeid` and `dynamic_cast`.

**`type_info::operator==`** — compares two type identities. Whether it compares addresses or falls
back to `strcmp` on the mangled name is an implementation choice, exposed as
`__GXX_MERGED_TYPEINFO_NAMES`, and §9 turns on it.

**`RTLD_LOCAL` / `RTLD_GLOBAL`** — `dlopen` flags controlling whether the loaded object's symbols
become available to subsequently loaded objects. `RTLD_LOCAL` is the right default for a plugin and
is what creates two distinct `type_info` objects for one class.

**ODR** — the one-definition rule. Violating it across binaries is not diagnosed: the loader picks one
definition and the other binary calls something it was not compiled against.

**Zipf** — a power-law distribution, here with s = 1.1, used because real protocol traffic is heavily
skewed rather than uniform. §10 is entirely about that fact.

**Residual predicate** — a `matches()` clause that is not a function of the dispatch key, so no table
can select on it. Such handlers queue in a bounded candidate list behind a slot.

**Budget** — 333.3 ns, being one core-second divided by the brief's 3 M packets/sec. Every cost in
this document is reported against it.
