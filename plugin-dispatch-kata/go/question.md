# The Go track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

Go is where this kata stops being a translation exercise, and it breaks in a different place than
you expect. The dispatch half ports almost unchanged — a slice indexed by a key is a slice indexed by
a key. It is the *premise* that fails: Go's mechanism for loading code at runtime cannot do what the
brief requires, and every alternative moves the boundary somewhere that charges you on the hot path.
Two of the brief's four options do not exist in the language, and the third does not do what a C++
engineer's intuition says it does.

The most valuable step is G2, and the most surprising one is G3.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step G1 — Write the ordinary Go answer, then price its call

Before porting anything: write what a competent Go engineer produces in an afternoon. Standard
library, no `unsafe`, interfaces and a table.

Then price the one indirect call that remains, because the C++ intuitions about it are only half
right here.

**Be prepared to say:**

- your ordinary design in one sentence — the interface, the table's type, and how a handler is
  selected
- what an interface value *is* in the runtime: how many words, what each one points at, and what
  therefore happens to a `[]Handler` walk in terms of cache lines
- what the compiler does with a method call through an interface, and under what conditions it can
  avoid the indirect call. Name the mechanism and the version it arrived in, then check whether it
  fires on *your* call site
- the allocation trap: which concrete types cost you a heap allocation the moment they are stored in
  an interface, and how you detect that in a test rather than in a profile
- whether escape analysis can see through your dispatch, and what that does to the packet argument

<details>
<summary>Hint</summary>

The C++ answer to "the compiler can devirtualize if it can see the type" has a Go counterpart, but it
does not come from inlining — it comes from profile data, and it is opt-in. Find it, turn it on, and
then verify rather than assume: build with the compiler's optimization diagnostics and look for the
call site by name. A devirtualization that did not fire is indistinguishable, from the outside, from
one that did.

For the second probe: an interface value is a pair, and the second word is a pointer to the concrete
object. So a table of interfaces is a table of pairs pointing at objects allocated separately — which
is the same pointer-chase the C++ `vector<unique_ptr<>>` had, and worth costing the same way.

The allocation probe has a cheap, mechanical answer that belongs in CI rather than in a comment.

</details>

**Done when** you can state the per-packet cost of your dispatch in nanoseconds, and say whether the
indirect call is still there — having looked, not assumed.

---

## Step G2 — The premise does not hold

The brief says handlers come from separately compiled shared libraries. Go and read what Go's
`plugin` package actually requires, and what it supports.

Then say where the boundary goes instead, and price each option against your per-packet budget.

**Be prepared to say:**

- the three constraints `plugin` imposes that make it unusable for the brief's deployment story, and
  the platform question you should have asked first
- step 7 of the main kata asks what happens to type identity across the boundary. Load a symbol whose
  type both binaries compiled, assert it against the host's interface, and say what happens — then
  say whether the failure is *reported* or *silent*, and compare that to the C++ track's answer
- the four places the boundary can go instead — in-process C ABI, out-of-process, a sandboxed
  bytecode runtime, and not-at-runtime-at-all. For each: what crosses, and what it costs
- the arithmetic that eliminates most of them: your per-packet budget at 3 M packets/sec against the
  measured cost of one boundary crossing. Measure the crossing; do not quote a number you read
- the consequence for the *interface shape*. Given that arithmetic, `process(one packet)` is not the
  signature you want. Say what is, and what that does to latency
- the answer Go culture would give — rebuild and redeploy the binary — and the conditions under
  which it is correct rather than a cop-out

<details>
<summary>Hint</summary>

Read `plugin`'s documentation as a list of promises it does *not* make. One of them is about
operating systems, one is about toolchain and dependency versions, and one is about what you may
never do to a loaded plugin. Any one of the three would be disqualifying for a service that loads
third-party handlers; you have all three.

The version constraint and the type-identity question are the same fact seen twice. Find out what
the loader does when two binaries disagree about a shared package — whether it refuses or proceeds —
and note that whichever it is, someone chose it. The C++ track's answer to the identical question
went the other way, and the difference is worth a sentence in your writeup.

So the boundary moves, and now the main kata's step 4 does real work: you are choosing not between
abstraction mechanisms but between *address spaces*. Cost each crossing empirically — write the
benchmark, because the numbers here span three orders of magnitude and your intuition will not rank
them correctly.

Then notice what the arithmetic forces. If a crossing costs a meaningful fraction of your per-packet
budget, the only way to keep the boundary is to stop crossing it per packet. That changes the ABI
from step 5 of the main kata — and it changes what a handler is allowed to assume about the packets
it is handed.

The last probe is not a joke. A service that redeploys in ninety seconds may not need runtime loading
at all, and "the plugin mechanism is a build pipeline" is a legitimate principal-level answer with
consequences you should state.

</details>

**Done when** you can rule out `plugin` in three clauses, and defend your replacement with a measured
crossing cost next to your per-packet budget.

---

## Step G3 — Static polymorphism that is not

The brief's option 3 is templates. Go has generics. They are not the same tool, and the difference is
exactly the thing the option was proposed for.

**Be prepared to say:**

- what the compiler generates for a generic function instantiated at many types — the rule is not
  "one copy per type", so state what it actually is
- what all pointer-typed instantiations share, what gets passed to them to make that work, and
  therefore whether a method call inside a generic function over a pointer constraint is direct or
  indirect
- the case where you *do* get the C++ behaviour, and what your handler types would have to be for it
  to apply
- option 2 has no equivalent at all. What is the nearest thing, and what does a switch over 50
  concrete types compile to? Go and look, then say whether it is a jump table
- given all of that, whether generics belong anywhere in this design — and what they buy if not speed

<details>
<summary>Hint</summary>

The intuition to break is "generic means monomorphized means devirtualized". Go and find what the
compiler shares between instantiations and what it passes to them so that shared code can still find
the right methods. The name of the technique tells you the rule, and the rule has a boundary: types
of the same *shape* share code.

Then work out which shape your handlers have. If the answer is "pointer", every one of them is
sharing one instantiation with a runtime lookup in it — which is a virtual call with extra steps and
a larger binary. The version that behaves like a C++ template requires something specific of your
handler types, and it interacts badly with the plugin half of the brief.

For the sum type: build the 50-arm switch and read the generated code rather than reasoning about
it. Whatever it is, compare it to the array index from step 2 of the main kata, and notice that the
array index was already better and required no type machinery at all.

</details>

**Done when** you can say what a method call inside a generic function over your handler constraint
compiles to, having read the output, and defend using or not using generics on that basis.

---

## Step G4 — Ownership when one side has a collector

Step 6 of the main kata asked who owns what. In Go, one side of the boundary has a garbage collector
and the other does not, and the rules are not advisory.

**Be prepared to say:**

- the rules governing a Go pointer passed to C: what the memory it points to may not contain, and
  what C may not do with it after the call returns
- how the packet gets to the handler given those rules. Copy, pin, or allocate outside the heap —
  price all three per packet
- the mechanism for the pinning option, the version it arrived in, and what it costs
- what happens if a handler stores the packet pointer for use in a later call, and what your API
  must do to make that impossible rather than merely forbidden
- two Go runtimes in one process: when this arises, and what the failure looks like

<details>
<summary>Hint</summary>

The rules exist because the collector may move or free memory the other side is still holding, and
the checker that enforces them is not always on. Find out how to turn it on in tests, and then note
that it does not catch everything.

Work the packet argument as an engineering problem with three priced options rather than a rule to
comply with. A copy per packet is honest and costs a copy per packet; pinning has a mechanism and a
cost; allocating the buffers outside the collector's view changes who owns them and reintroduces
every question from step 6 of the main kata, now with two allocators again.

The retention probe is the interesting one. "The plugin must not keep the pointer" is a sentence in a
document. Ask what your interface would have to look like for keeping it to be useless — the answer
is a shape, and it is the same shape that solved the batching problem in G2.

</details>

**Done when** you can state the pointer rules in two clauses, name your packet-passing choice with a
measured per-packet cost, and describe the API property that makes retention harmless.

---

## Step G5 — Faults, and what they take down with them

Step 7 of the main kata asked what crosses the boundary when a handler fails. Go's failure modes are
different enough that the answer does not port.

**Be prepared to say:**

- what a panic does when it reaches a frame that is not Go, and therefore what your boundary shim
  must do
- `recover`'s two constraints — where it must be called from and which goroutine it covers — and what
  that means for a handler that spawns one
- what recovering per packet costs, and whether a deferred function on the hot path is affordable at
  your budget. Measure it
- a handler that blocks: what it holds while blocked, what the scheduler does about it, and how many
  such handlers it takes to matter
- your containment claim, stated honestly: after recovering from a handler panic, what do you
  actually know about the state of your process?

<details>
<summary>Hint</summary>

The panic question has a hard answer and a soft one. Across a foreign frame it is undefined and the
practical result is that the process dies — so the shim is not a nicety. Within Go it is recoverable,
but `recover` is narrower than people remember: it is scoped to a deferred call and to one
goroutine, and a handler that starts a goroutine which panics takes the process down no matter what
your shim does.

Cost the defer honestly rather than assuming either way — the compiler has made deferred calls much
cheaper than they once were in the common case, and "in the common case" is doing work in that
sentence. Benchmark it inside your actual dispatch, not standalone.

The blocking probe is the one with no C++ counterpart worth having: a handler doing something slow
occupies a scheduler resource, and the consequence is not "that packet is late", it is a service-wide
effect. Work out which resource and how many.

The last probe should make your containment claim smaller than you wanted it to be. That is the
correct outcome.

</details>

**Done when** you can say what a handler panic costs you, what it cannot protect against, and what
one slow handler does to unrelated traffic.

---

## Step G6 — Benchmarks that are not measuring your program

You have made claims about devirtualization, crossing cost, allocation and per-packet budget. Go's
benchmarking is good enough that the failure mode is subtle.

**Be prepared to say:**

- the classic way a Go microbenchmark measures nothing, and the two mechanisms for preventing it
- how you compare two arms with confidence rather than by eyeballing two numbers — name the tool and
  what it needs from you
- how you demonstrate that profile-guided optimization changed your dispatch, as opposed to changing
  the total by an amount within noise
- the traffic-distribution measurement from step 9 of the main kata: how you collect it in a Go
  service, and where you put it so the benchmark and the production build both use the same one
- what your allocation assertion looks like, and why it belongs in a test rather than a benchmark

<details>
<summary>Hint</summary>

A benchmark whose result is unused is a benchmark the compiler may delete, and a dispatch loop over
one handler is one whose branch is perfectly predicted and whose code is entirely resident. Your
service is neither. Build the interleaved benchmark with the real distribution and watch the ranking
change — that is the same trap as the C++ track's, and Go's optimizer is different enough that the
answer might be too.

For the statistics: two numbers printed by two runs are not a comparison. The standard tool wants
several runs per arm and will tell you when the difference is not significant, which is frequently
the honest answer and the one you should be willing to report.

The allocation assertion is the cheapest permanent guarantee in this track. It is one field of the
benchmark result, it can be asserted, and it fails loudly the day someone adds a field that boxes.

</details>

**Done when** you can report your two arms with a significance figure, and show a test that fails if
a future edit puts an allocation on the dispatch path.

---

<details>
<summary><b>The shape of the Go answer</b> — read after you have your own.</summary>

The dispatch half ports directly and is unglamorous: a key extracted from the packet, a slice or
array indexed by it, one interface call. Interfaces are the right internal mechanism because the
alternatives are worse here — generics over pointer-shaped types share one instantiation and carry a
dictionary, so they add indirection and binary size rather than removing them, and Go has no closed
sum type to make a `variant` argument out of.

The plugin half is where the kata bites. `plugin` is not available on every platform the service must
run on, demands that the plugin and host be built by the identical toolchain with identical versions
of every shared dependency, and can never unload — so the brief's deployment story is not
implementable with it. The boundary therefore moves, and every destination charges per crossing:
cgo into a C ABI, a subprocess over a local transport, a sandboxed bytecode runtime, or a build
pipeline fast enough that nothing loads at runtime at all.

The arithmetic that follows is the whole finding. Once a crossing costs a meaningful share of a
sub-microsecond budget, the interface cannot be per-packet, and the ABI becomes a batch interface —
which is a *different* design than the C++ answer reached, for reasons that have nothing to do with
polymorphism. The two-plane split from step 4 of the main kata survives; the shape of the plane
boundary does not.

Underneath sit two problems C++ does not have: the collector's rules about pointers handed to code it
cannot see, which turn packet passing into a costed choice between copying and pinning; and a failure
model where `recover` is narrower than it looks and a single blocking handler is a service-wide
event rather than a local one.

</details>

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [python/question.md](../python/question.md).
