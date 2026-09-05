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

## Status

Scaffold. The question and its three tracks are written; the worked answers and the measurement code
are not.

``` text
plugin-dispatch-kata/
  question.md          the kata — 11 steps, the rubric, the folded answer sketch
  cpp/question.md      C1–C5   all four options priced · the ABI in bytes · two libstdc++ · RTTI across DSOs · counters
  go/question.md       G1–G6   the boring answer · the premise breaking · generics that don't monomorphize · cgo ownership · panics · benchstat
  python/question.md   P1–P6   the ceiling first · the fix that pays more · critique abi3 · refcounts · the GIL · Python as control plane
```

Each track's `solution.md` and implementation are the next piece of work, following
[mpsc-event-pipeline-kata](../mpsc-event-pipeline-kata/): a benchmark binary that asserts the
invariants under shared labels and exits non-zero if one breaks, with every number quoted in a
solution coming from running it.
