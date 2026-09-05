# plugin-dispatch-kata — dispatch selection and the plugin ABI boundary

One systems-design question, asked three times in three languages — each from that language's own
cost model rather than translated from the others.

[**question.md**](question.md) — a packet-processing service with 50–100 protocol handlers, some
built in and some arriving from separately compiled shared libraries, dispatching by linear scan over
`matches()` at several million packets/sec. Profiling says `dispatch` is expensive. The team proposes
four polymorphism mechanisms. **The profile never said the virtual call was the problem** — it said
the function was — and the four options all change how `process` is called rather than how many times
`matches` is. That is the clause the whole kata turns on, and the second half is the one it leads to:
the mechanism that is right at the ABI boundary and the mechanism that is right on the hot path are
not the same mechanism, and nothing requires them to be.

It is staged as a kata — eleven steps that build the argument in the order it has to be built, each
with probes, a hint, and a self-assessable gate, and the answer sketch folded at the bottom. Three
language tracks then ask what the shared question cannot:
[cpp/question.md](cpp/question.md) · [go/question.md](go/question.md) ·
[python/question.md](python/question.md).

## Why three languages, when the menu is C++'s

The brief's four options — virtual, `std::variant`, templates, custom type erasure — are a C++
artifact. That is the point of running it in three languages rather than an obstacle to it: the
*question* underneath the menu is universal, and watching which options survive translation is what
exposes which parts of the answer were about polymorphism and which were about architecture.

So the brief is quoted rather than translated, and steps 0–10 are answerable in any of the three.
[The framing note](question.md#about-the-framing) at the top of the question says how to read the
C++ hot path if C++ is not your language, and each track holds the specific form of the probes the
shared steps had to ask generally.

| | what the language does to the question |
|---|---|
| **C++** | The only track where all four options are real, so it is the only one that has to disqualify one on the *premise* rather than the benchmark — a `variant` is a closed set, and the brief says handlers arrive at runtime. Then the boundary in full: layout, versioning, two standard libraries in one address space, and two binaries that agree on every byte of a class and still hold two different identities for it. |
| **Go** | The premise breaks. Go's `plugin` cannot do what the brief requires — platform coverage, identical-toolchain requirement, no unload — so the boundary has to move, and every place it can move to charges per crossing. That arithmetic forces a *batch* ABI, which is a different design than C++ reaches, for reasons unrelated to polymorphism. Also: one of the four options has no equivalent at all, one is what interfaces already are, and generics do not monomorphize the way the C++ intuition says. |
| **Python** | Both halves invert. The mechanism question is noise — but the O(n)→O(1) fix wins a much larger multiple than in C++, which is the cross-track finding: the higher the runtime's per-operation cost, the more dispatch *selection* dominates and the less the mechanism matters. Meanwhile the ABI half is more real than in Go, because CPython shipped exactly the versioned C interface the Principal rubric asks for, and the track's job is to critique the reference answer rather than invent one. |

## The two halves

Every track reaches the same two-part structure and disagrees about the second part.

**Stop searching.** At 50–100 handlers the dominant cost is the scan — dozens of data-dependent,
poorly-predicted branches and a pointer chase per element, before any handler does useful work.
Extract a cheap key, index a table, and O(handlers) becomes O(1) before any question about
polymorphism is asked. What it costs is not nanoseconds: predicates that are not a function of the
key need a bounded fallback, and first-match-over-an-ordered-list was a behaviour somebody depended
on.

**Two planes, two mechanisms.** The plugin boundary optimizes for stability across compilers it has
never seen; the hot path optimizes for everything being visible to the optimizer at once. Those are
opposed, and one mechanism satisfying both satisfies neither. The Principal sentence is that nothing
requires the same abstraction mechanism at both — a stable, versioned, POD C ABI at the boundary,
adapted internally into whatever dispatches fastest.

**And the number the brief withholds.** It gives handler *count* and never gives traffic
*distribution*. If real traffic is dominated by a handful of protocols and the list is ordered by
frequency, the scan costs two probes and the table buys the remaining few percent — which makes a
one-line `partition` the competitor the rewrite has to beat. Step 9 exists to make that comparison
before anything is built.

## The answer, so far

| | thesis |
|---|---|
| [**cpp/solution.md**](cpp/solution.md) | Two mechanisms, because there are two problems. Internally a key-indexed table and a plain `virtual` call; across the boundary a versioned `extern "C"` struct of function pointers. **`std::variant` is disqualified by the premise, not the benchmark** — a closed set cannot admit a type that arrives from a `.dll`. And the finding that nearly went the other way: **at 95/5 traffic, sorting the handler list captures 94 % of the entire available win and the table adds 6 %.** |
| go/solution.md | not written — [go/question.md](go/question.md) |
| python/solution.md | not written — [python/question.md](python/question.md) |

## Notable results

- **The profile pointed at a function; the option list assumed a line.** At 100 handlers the scan
  runs 52.3 `matches()` calls and spends 142.8 ns — 43 % of the 333 ns budget — before any handler
  does useful work. The dispatch mechanism the four options compete over costs 8.6–11.0 ns.
  ([cpp §2](cpp/solution.md#2-costing-the-scan), [§4](cpp/solution.md#4-the-mechanism-re-priced))
- **The instruction cache costs three times more than the mechanism** — 33–37 ns against ~10 ns —
  and a one-handler microbenchmark cannot see it, because with one handler resident all four
  mechanisms land within 1.5 ns of each other. The control that makes this readable is a `direct-call`
  arm whose i-cache column reads **0.10 ns**. ([cpp §4](cpp/solution.md#4-the-mechanism-re-priced))
- **The benchmark nearly lied about its own headline.** The first packet generator assigned traffic
  weights to handlers in registration order, so the busiest handler was already first and sorting had
  nothing to fix. Sorting then captured 5 % of the available win instead of 76 %, which supports the
  exact opposite conclusion. ([cpp §10](cpp/solution.md#10-does-the-table-actually-pay))
- **`dynamic_cast` across a DSO did not return null**, contrary to the folklore — and contrary to
  what this kata's C++ track asserted until the probe refuted it. The two binaries genuinely hold
  distinct `type_info` objects, and the probe prints both addresses; but libstdc++ is built with
  `__GXX_MERGED_TYPEINFO_NAMES = 0`, so comparison falls back to `strcmp` on the mangled name and the
  cast succeeds. The hazard is a toolchain configuration away, not a certainty.
  ([cpp §9](cpp/solution.md#9-errors-and-a-claim-that-did-not-survive-contact))
- **Batching the plugin crossing recovers nothing.** The crossing costs 3.0 ns, 1 % of the budget;
  batching ×64 gives back −0.1 ns. An in-process C ABI crossing is an indirect call and a struct
  copy — there is no transition to amortize. Batching is the right answer for a cgo call or an IPC
  round trip, and designing it in here would have been complexity measured against nothing.
  ([cpp §7](cpp/solution.md#7-the-abi))
- **This machine's own measurement bugs cost more than most of the effects being measured**: a
  32 MiB packet pool sitting on the L3 boundary made the *table* slower at 50 handlers than at 100;
  an unpinned thread migrated between P-cores and E-cores mid-run; and arm-major repetition ordering
  made two arms doing identical work differ by 27 %, stably and reproducibly.
  ([cpp §12](cpp/solution.md#12-verification-and-what-this-platform-cannot-measure))

## Verify

```sh
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build cpp/build
cd cpp/bin && ./solution.exe        # exit code is the verdict
```

Run it **twice and read the second**: the first run of a freshly linked binary is about three times
slower than every subsequent one, and phase 0 prints the instrument before any result depends on it.

<details>
<summary>The ELF-only ABI probes (needs WSL)</summary>

`RTLD_LOCAL`, `RTLD_GLOBAL` and `type_info` identity are POSIX/ELF concepts with no PE equivalent, so
they run under WSL — the same split [mpsc-event-pipeline-kata](../mpsc-event-pipeline-kata/) uses for
ThreadSanitizer, and for the same reason.

```sh
wsl -- bash -lc 'cd /mnt/c/.../plugin-dispatch-kata/cpp/abi-probe && make run && make abi-break'
```

Each probe exits non-zero if the phenomenon it claims does *not* occur, which is how the
`dynamic_cast` folklore above got caught.

</details>

``` text
plugin-dispatch-kata/
  question.md          the kata — 11 steps, the rubric, the folded answer sketch
  cpp/                 question.md solution.md  src/ plugin/ bench/ abi-probe/  -> bin/solution.exe
  go/question.md       G1–G6   the boring answer · the premise breaking · generics that don't monomorphize · cgo ownership · panics · benchstat
  python/question.md   P1–P6   the ceiling first · the fix that pays more · critique abi3 · refcounts · the GIL · Python as control plane
```

## Why switchyard

A **switchyard** is where a railway sorts arriving cars onto the tracks that will carry them onward.
The name earns its place because it describes both halves of this kata rather than one:

| the yard | the design |
|---|---|
| reads a tag off each car and throws a **switch**, rather than asking every track in turn | a key extracted from the packet indexes a table — the O(n) → O(1) move |
| a **hump** yard replaced linear shunting with one gravity-driven sort | the whole argument of §2: stop searching |
| cars from **other railroads** run on your track only because the coupler and the gauge are standardized | the plugin ABI — a small, versioned, boring contract is what makes foreign code admissible at all |
| the yardmaster knows the traffic mix and **orders the tracks by it** | §10, where sorting by observed frequency captures 94 % of the win and nearly makes the rewrite unnecessary |

The last row is why it is not just a pleasant image. A switchyard's throughput is set by how well its
layout matches the traffic actually arriving, not by how clever the switching mechanism is — which is
this kata's finding, arrived at by measurement after the prose had already assumed otherwise.
