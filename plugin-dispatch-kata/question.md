# Kata 2 — dispatch selection and the plugin ABI boundary

## The brief

You are designing a C++20 packet-processing service that loads protocol handlers dynamically.

The hot path currently looks like this:

```cpp
struct Handler {
    virtual ~Handler() = default;

    virtual bool matches(const Packet&) const = 0;
    virtual Result process(const Packet&) = 0;
};

std::vector<std::unique_ptr<Handler>> handlers;

Result dispatch(const Packet& p)
{
    for (auto& h : handlers) {
        if (h->matches(p))
            return h->process(p);
    }

    return Result::unsupported();
}
```

There may be 50–100 handlers. Some are built into the executable; others come from separately
compiled shared libraries.

Profiling shows that `dispatch` is becoming expensive at several million packets/sec.

The team proposes four alternatives:

1. Keep virtual interfaces
2. `std::variant` + `std::visit`
3. Templates / static polymorphism
4. Custom type erasure

Design the architecture. Explain what you would choose for the internal hot path versus the plugin
ABI boundary, and discuss: virtual-call cost, branch prediction, instruction-cache pressure, data
locality, binary size, ABI stability, ownership across shared-library boundaries, exceptions and
RTTI, allocator/runtime compatibility.

## About the framing

The brief is quoted, not translated. It is stated in C++ because that is how it was asked, and the
four options are a **C++ menu** — which is itself part of the exercise. Working out which of them
your language has, which have no equivalent at all, and which have one that does not do what its
name implies is a step in the answer rather than an obstacle to reaching it.

What steps 0–10 are about is **how a handler gets selected** and **where the boundary between two
binaries falls**. Every language has both, and neither argument is about polymorphism.

If C++ is not your language, read the hot path above as:

| the brief | Go | Python |
|---|---|---|
| `std::vector<std::unique_ptr<Handler>>` | `[]Handler` — a slice of two-word interface values | `list[Handler]` |
| a `virtual` call through a base pointer | an interface method call through an itab | an attribute lookup, then a call |
| a `.so` opened at runtime | see the Go track; this is where the premise breaks | a C extension module, or an imported one |
| `Result::unsupported()` | a zero value or a sentinel | `None` or a sentinel |

The table is orientation only. What each language actually charges for those operations is the
subject of its track.

## How to use this kata

That brief is the whole question, and you could answer it in one sitting. The eleven steps below
exist because the answer is not reached by knowing more about polymorphism — it is reached by
refusing the question's framing in a particular order, and each step is one refusal.

- **Answer a step before you open its hint.** Out loud, or on paper, in the number of sentences the
  *Done when* line asks for. A hint read first replaces the work it was meant to provoke.
- **Hints reframe, they never conclude.** None of them tells you what to build.
- **The answer sketch is at the bottom, folded.** It is the only place in this file where the shape
  is given away. Steps 0–10 do not name it.
- **Do not skip step 0.** It is the shortest step and the one the rest is built on.
- **Language tracks come after step 10.** They assume you have an answer to defend.

---

## Step 0 — Read the profile for what it does not say

One sentence in the brief is a measurement. One is a list of four options. They are not about the
same thing, and the list quietly asserts a diagnosis the measurement never made.

Quote the measurement exactly. Then say what it attributes the cost to.

**Be prepared to say:**

- what the profile actually claims is expensive, in the brief's own words
- what all four proposed alternatives change — the property they share, stated as one clause
- the number in the brief that suggests the cost is somewhere the four alternatives do not touch
- what it would take to distinguish the two diagnoses experimentally, in one afternoon

<details>
<summary>Hint</summary>

Every one of the four options is a different answer to the question *how should a call to `process`
be dispatched*. That is a real question. Check whether it is the question the profiler asked.

Read the body of `dispatch` again and count the things that happen per packet. Then read the brief
for the number that multiplies one of them. A profiler attributing time to a function does not tell
you which line, and the four options were chosen before anyone looked.

</details>

**Done when** you can state, in one sentence, the alternative diagnosis the option list has ruled out
without testing it.

---

## Step 1 — Cost the loop before costing the call

Take a packet that is matched by the 40th handler. Walk the loop and write down what the machine
does.

Not "it's O(n)". Actual quantities: how many calls, how many branches, how many cache lines, and how
many of those branches a predictor can learn.

**Be prepared to say:**

- your per-packet budget in nanoseconds at 3 M packets/sec on one core, computed, not guessed
- the number of indirect calls, and the number of data-dependent branches, executed before the
  matching handler is found — in the average case and the worst
- how many distinct targets the `matches` call site sees across a run, and what that does to the
  indirect-branch predictor. Then price a mispredict against the budget above
- how many cache lines the walk touches — count the indirections per element in *your* language's
  version of that container, and say where each handler object actually lives
- the ceiling: if every remaining call in this loop were made free, what is the most the four
  alternatives can win?

<details>
<summary>Hint</summary>

Price the loop, not the call. A correctly predicted indirect call is a few nanoseconds; a
mispredicted one is roughly an order of magnitude worse. Multiply by the number of probes rather
than by one, and compare against the budget you computed.

Then do the arithmetic that decides the whole kata: put the total cost of the *scan* next to the
total cost of the *calls*, and ask which one an optimization of the call mechanism can reach. If
the scan is most of it, the best conceivable version of options 2, 3 and 4 wins you a fraction of
a fraction.

The last probe is the important one, and it is not a rhetorical question — compute it.

</details>

**Done when** you can say what fraction of the per-packet budget the search consumes before any
handler has done a single byte of useful work.

---

## Step 2 — Stop searching

The loop is a linear search. The way to make a linear search fast is to stop performing one.

Design the replacement. Then find out what it costs you, because it is not free and the price is not
paid in nanoseconds.

**Be prepared to say:**

- the key: what you extract from the packet, how many bytes, and how you know it is cheap
- the property every `matches()` must have for a table to be *legal* — state it precisely, because
  some of the 50–100 handlers will not have it
- your table's shape, and the density argument behind it: a 16-bit key means 65 536 slots, so say
  whether that is an array, a hash, or two levels, and why
- what happens when two handlers claim the same key, and what happens when none does
- what changed semantically. The original returns the *first* match, so list order is a behaviour
  that somebody depends on. Say who, and what you owe them

<details>
<summary>Hint</summary>

The obstacle is not building a table. It is that `matches()` is arbitrary code, and arbitrary
predicates do not have keys.

So the design question is whether each predicate is *factorable*: can it be split into a cheap exact
key plus an optional residual test? If it can, the table selects a small candidate set and the
residual runs only over that set — which means your structure is not "table instead of loop" but
"table, then a very short loop", and the short loop is where the awkward handlers go.

Now go looking for the handlers that will not factor: the ones matching on ranges, on a payload
byte, on a combination of fields, on state from a previous packet. Do not design them out of
existence — design the fallback that carries them, and put a bound on how big it may grow.

The semantic change is the part that gets missed in review. First-match-wins over a list an operator
can reorder is a feature, and a table has no order at all.

</details>

**Done when** you can state the key, the table shape, the fallback for unfactorable predicates, and
the one behaviour that a table cannot preserve.

---

## Step 3 — Now choose the call mechanism

The scan is gone. One call per packet remains. Re-price the brief's four options — or your
language's equivalents — against *that* number rather than against the one from step 1.

**Be prepared to say:**

- the fraction of your remaining per-packet budget that one indirect call now represents
- what the call site's target distribution looks like after step 2, and whether the predictor finds
  it easy or hard — this depends on a property of your traffic, so name the property
- what inlining actually buys when `process` is a few hundred instructions long, and why the answer
  differs from the microbenchmark you have seen
- for each mechanism your language actually offers, its cost in **binary size and instruction
  cache**, with 50–100 handlers, not two
- which of the four options the brief's own requirements disqualify outright — there is at least one,
  and the disqualification is not about performance

<details>
<summary>Hint</summary>

A dispatch-mechanism microbenchmark runs one handler in a loop with all of its code resident in L1i,
and reports that the cheapest mechanism wins. Your service runs 50–100 handlers whose combined code
does not fit in L1i, on packets that arrive in an interleaved order. Ask what the microbenchmark's
winner does to the instruction footprint, and whether the thing it saves per call is larger than the
thing it costs per miss.

Two of the options make code by multiplying it. Work out the multiplier before you decide they are
fast.

The last probe is the one to spend real time on. Re-read the requirements — not the performance
requirements, the deployment ones — and check each option against them. One of the four cannot
express something the brief states as a fact about the system, and no amount of benchmarking will
fix that.

</details>

**Done when** you can say what fraction of the budget the mechanism is now worth, name the option the
premise disqualifies and why, and defend keeping what you have or replacing it — on the post-step-2
number.

---

## Step 4 — Draw the line between the two planes

The brief asks you to choose *a* mechanism. Ask why one.

Write down, in two columns, what the plugin boundary needs and what the hot path needs. Then look
for a row where they agree.

**Be prepared to say:**

- four properties per column, each one a requirement rather than an adjective
- the rule that falls out, in one sentence, with no hedging clause in it
- what the adapter between the two costs — per packet, per handler, and per line of code somebody
  maintains
- how a dynamically loaded handler gets *into* the table from step 2, given that the host builds the
  table and the plugin's predicate lives in another binary
- what the host does when a plugin's declared key collides with a built-in handler's

<details>
<summary>Hint</summary>

The two columns are close to opposites. One side wants nothing to change for years across compilers
it has never seen; the other wants everything to be visible to the optimizer in one translation
unit. A single mechanism satisfying both is a mechanism satisfying neither well.

Once you accept two representations, the interesting problem is the registration protocol, and it
has one non-obvious constraint. The host must build the dispatch table *before* traffic arrives,
which means it cannot discover a plugin's key by calling its predicate on packets. So the plugin has
to say what it handles rather than demonstrate it — and "what it handles" must be expressible in the
boundary's vocabulary, which is much smaller than the language the handler is written in.

That constraint propagates backwards into step 2 and it is the reason to do these two steps in this
order.

</details>

**Done when** you have the rule as one sentence, and can describe the registration call that lets a
handler compiled two years ago enter a table built today.

---

## Step 5 — Write the boundary down

Specify the ABI. Not the header — the bytes.

**Be prepared to say:**

- the exported symbol a plugin must provide, and how the host finds it
- what a version field does and does not buy you. It detects a mismatch; say what makes two
  *compatible* versions compatible
- how you add a seventh function to a six-function interface without recompiling existing plugins —
  and how an old host behaves when handed a new plugin, and a new host an old plugin
- the packet argument's type. It is non-owning: say who owns the bytes, for exactly how long, and
  what happens if the plugin keeps the pointer
- three types from your language's ordinary vocabulary that will cross this boundary, link or load
  cleanly, and be wrong. For each, name the mechanism by which it is wrong

<details>
<summary>Hint</summary>

An ABI is not a header file. It is a promise about layout, calling convention, and symbol names, and
the header is only one party's opinion about it.

The test to apply to every field: *could this be produced by a different compiler, a different
standard library, or a different language, three years from now?* Anything that fails is not part of
your ABI — it is part of your API, and it has no business crossing the boundary.

For extension, look at how this problem has actually been solved in shipped systems. There are two
patterns, they compose, and one of them is a field you have to put in before you need it or never.

The third probe is where the real damage lives. The types that break here are not exotic; they are
the ones you use in every other function signature you write, and the failure is not a link error.

</details>

**Done when** you can add an entry point without breaking a deployed plugin, and state the packet
buffer's ownership in one sentence with a duration in it.

---

## Step 6 — Ownership, allocators, and the unload you probably cannot do

Walk one plugin handler from load to unload and name the owner of every object at every moment.

**Be prepared to say:**

- who allocates the handler instance, who frees it, and why that pairing is a rule rather than a
  preference — the reason should mention something other than symmetry
- what happens when host and plugin were built against different runtimes or standard libraries —
  name the failure, and say which of them your toolchain warns about and which it does not
- where the `Result` comes from: returned by value, written into caller storage, or allocated by the
  callee and freed by whom
- the unload sequence, in order, and the conditions that must hold before it — there are more than
  you first listed
- whether you support unload at all, and what it costs to answer "no"

<details>
<summary>Hint</summary>

The `create`/`destroy` pair in an interface of this shape is not there for tidiness. Ask what
`free()` does with a pointer that came from a different heap, then notice that "a different heap" is
the normal case the moment two binaries link their own allocators.

For unload, list the things that can still hold a pointer into the plugin's code or data after you
have stopped sending it packets. In-flight work is the one everybody names. Keep going: registered
callbacks, cached function pointers, anything the plugin handed you that you kept, static
destructors, thread-local storage — and threads. A thread the plugin created is *executing the
plugin's code*, and unmapping code that a thread is executing is not a race you can win.

"We never unload" is a legitimate engineering answer with a cost. Price it before you rule it out.

</details>

**Done when** you can walk load → register → process → drain → unregister → unload naming every
owner, and state your unload policy with its cost attached.

---

## Step 7 — Errors that cannot unwind

A plugin hits something it cannot handle. Say what crosses the boundary.

**Be prepared to say:**

- what your language's error-unwinding mechanism *is*, mechanically — and therefore what two
  binaries must agree on for one to cross between them
- your boundary's error contract in one sentence, and where the shim that enforces it lives
- the error you cannot enumerate: how a plugin returns a *message*, who allocates it, and who frees
  it. Re-read your step 6 answer before replying
- type identity across the boundary: can code on one side recognize a type defined on the other,
  what is that comparison actually comparing, and what makes two identical definitions fail to
  compare equal
- what a plugin fault should do to the service: contain it or die. Argue for one, and say what your
  choice costs at 3 M packets/sec

<details>
<summary>Hint</summary>

An exception is not a value. It is a stack-unwinding protocol with participants that both sides must
share — in C++ that means unwind tables, a personality routine, and a common runtime; go and find
the equivalent list for your language, because every language has one and none of them survives a
binary boundary by default. "It works on my machine" here means "both halves happened to be built by
the same toolchain this week".

Once the boundary is a C-shaped one, errors are return values and that part is easy. The two hard
parts are the error you could not put in an enum — whose ownership question is step 6's question
again, wearing a different hat — and type identity, where two identical definitions in two binaries
can produce identities that do not compare equal, because the comparison is not doing what you think
it is doing.

For the last probe, notice that containment is not free and not partial: a catch-all shim keeps a
throwing plugin from unwinding through you, but it cannot make a plugin that corrupted your heap
safe to keep running. Decide which failures you are actually containing.

</details>

**Done when** you can state the error contract in one sentence and say, for a plugin that violates
it, whether your service survives and why.

---

## Step 8 — A plugin is a supply chain

Loading a plugin is the moment you grant unreviewed code your address space, your privileges, and
your uptime. Design what happens around it.

**Be prepared to say:**

- what must be true before you load something — and note that this is a security boundary, so say
  what you verify and against what
- how a bad plugin is detected in production, and how it is removed. Put a time on both
- whether two versions of a handler can run at once, and what that buys you at rollout
- the compatibility matrix you are actually committing to test: how many host versions × plugin
  versions × toolchains, and what you do to make that number smaller
- what you tell a plugin author about which of your behaviour they may depend on

<details>
<summary>Hint</summary>

Everything in steps 5 and 6 was about whether a plugin *can* work. This step is about what happens
the first time one does not — which is a certainty, not a risk.

Two questions separate a design from a plan. How long between a plugin misbehaving in production and
it no longer running? And how many combinations have you promised to support, given that every
supported host version multiplies every supported plugin version? The second number is the reason
mature plugin ABIs are much smaller than their authors originally wanted.

The rollout question has a nice property worth finding: the two-plane split from step 4 gives you a
place to run a new handler that receives real traffic and affects nothing.

</details>

**Done when** you can state your load-time verification, your time-to-remove, and the size of the
matrix you have committed to.

---

## Step 9 — Measure the thing you did not ask about

You have designed an optimization. Before you have any data, decide whether it works — and check
the one number the brief never gave you.

**Be prepared to say:**

- what you are A/B-ing, and what is held fixed between arms
- the distribution of protocols in your *actual* traffic, how you would obtain it, and why the brief
  not stating it is the biggest hole in the question
- what happens to the original loop's cost if you simply order `handlers` by observed frequency —
  compute it under a realistic skew before dismissing it
- the per-handler observability you need in production, and what you would alert on
- the result that would make you abandon the table and ship a one-line change instead, stated before
  you run anything

<details>
<summary>Hint</summary>

Step 1 costed the loop assuming the matching handler is somewhere in the middle. Real protocol
traffic is not uniform — it is usually dominated by a handful of protocols by an enormous margin.

So re-run step 1's arithmetic with the hot handlers first in the list. If 95 % of packets match
within the first two probes, the scan costs about two probes, not fifty, and everything you designed
in step 2 is buying you the remaining 5 %. That would make sorting the list by frequency — or a
move-to-front list — a change of one or two lines that captures most of the available win.

This does not mean the table is wrong. It means the deciding input is *traffic distribution*, not
handler count, and the brief gave you handler count. Notice which of those two numbers the four
proposed options were chosen on the basis of.

Decide the falsifying result in advance and write it down, because the table will be built by then
and nobody deletes a thing they have already built.

</details>

**Done when** you have the distribution measured (or the plan to measure it), the cheap fix costed
against the expensive one, and the number that would make you not do the rewrite.

---

## Step 10 — Argue against yourself

Make the strongest case for the code in the brief. Not as a straw man — as the thing you would
actually ship, under conditions you name.

**Be prepared to say:**

- the traffic and handler-count regime where the linear scan is genuinely fine, with a reason
- what the loop gives you that the table takes away: name at least three, and make one of them about
  the people who write handlers rather than about the code
- how many distinct protocols a maintainer must hold in their head for each design, and who is on
  call for them
- what would have to be true about your system for the sophisticated answer to be the wrong
  engineering call

<details>
<summary>Hint</summary>

The loop has one property nobody has priced yet: a handler's matching logic is arbitrary code, and
adding a handler requires understanding nothing about a global key space. The table replaces that
with a schema every future handler must fit — and the ones that do not fit end up in the fallback
you bounded in step 2, which is where the complexity you removed goes to live.

Then price the other column honestly. Steps 5 through 8 are protocols a future maintainer must hold
correctly, in a domain where a mistake is a segfault in someone else's binary or a silent ABI
mismatch that links cleanly.

"Table dispatch" is not the goal. It is one means to a goal, and the goal was set in step 1.

</details>

**Done when** you can name the conditions under which you would ship the loop, and mean it.

---

## Rubric

This is the rubric the question was asked against, so it is phrased in C++. The depths it describes
are not.

- **Senior** — knows a virtual call is an indirect call that inhibits inlining; suggests `variant` or
  templates for hot code; insists on benchmarking rather than declaring virtual functions slow.
  → steps 1, 3
- **Strong Senior → Staff** — realizes the virtual call may not be the problem at all, and that
  50–100 unpredictable branches per packet costs more than the dispatch mechanism. Fixes dispatch
  *selection* first, turning O(handlers) into O(1). → steps 0, 1, 2
- **Staff** — separates the plugin boundary from internal execution as two problems with opposed
  requirements, and understands that arbitrary C++ classes across a shared-library boundary create
  compatibility problems. → steps 4, 5, 6
- **Principal** — says explicitly *I would not require the same abstraction mechanism at the ABI
  boundary and on the hot path*, specifies a stable C ABI with versioning, and discusses ownership,
  allocators, exceptions, compiler and standard-library incompatibility, rollout, observability, and
  what realistic measurement means here. → steps 4, 6, 7, 8, 9
- Strong candidates also say **when the unsophisticated loop is still the better engineering call.**
  → step 10

These are not four different answers. They are four depths of one, and the depth is set at step 0.

---

## Language tracks

Steps 0–10 are answerable in any of the three, for the reason the framing note gives. What is *not*
portable is the menu: the brief's four options are a C++ artifact — in Go one has no equivalent at
all, one does not do what its name implies, and one is what interfaces already are; in Python three
of the four are meaningless and the boundary moves out of the language entirely.

Each track also holds the specific form of the probes the shared steps had to ask generally — the
types that cross a boundary and are wrong (step 5), two runtimes in one process (step 6), and type
identity between binaries (step 7).

Do steps 0–10 first; each track assumes you have an answer to defend.

| Track | What it adds |
|---|---|
| [**cpp/question.md**](cpp/question.md) | The only track where all four options are real — so it is the track that has to disqualify one on the premise rather than the benchmark. Then the boundary in full: layout, versioning, two standard libraries in one process, and two binaries that agree on every byte of a class and still hold two different identities for it. |
| [**go/question.md**](go/question.md) | The track where the *premise* breaks. Go's plugin mechanism cannot do what the brief requires, so the boundary has to move — and every place it can move to charges you on the hot path. Also: generics do not monomorphize the way the C++ intuition says, and there is no sum type at all. |
| [**python/question.md**](python/question.md) | The track that tests whether you re-derive or translate. The performance premise is off by orders of magnitude and must be stated before anything is designed — but the ABI half is *more* real here than in Go, because CPython shipped exactly the versioned C interface the Principal rubric asks for, and you can critique the reference answer directly. |

---

<details>
<summary><b>Answer sketch</b> — the shape of a strong answer. Read after you have committed at step 4.</summary>

The answer has two independent halves, and the central claim is that they must not share a
mechanism. This is the shape in the brief's own language; each track's fold gives its own.

**Half one: stop searching.** The profile blames `dispatch`, and every proposed option changes how
`process` is *called* — but at 50–100 handlers the dominant cost is the scan, which executes dozens
of data-dependent, poorly-predicted branches and chases a pointer per element before any useful work
happens. Extract a cheap key from the packet and select the handler by lookup:

```cpp
using HandlerId = uint16_t;
std::array<Handler*, MaxProtocolId> table;

auto* h = table[p.protocol()];
return h ? h->process(p) : Result::unsupported();
```

O(handlers) becomes O(1) before any question about polymorphism is asked. Predicates that are not a
function of the key do not disappear — they go into a bounded residual list the table selects into,
and first-match ordering, which the list had for free, becomes something you must specify.

**Half two: two planes, two mechanisms.**

``` text
Plugin boundary          Internal execution
----------------         ------------------
stable ABI               maximize performance
loose coupling           aggressive optimization
opaque objects           concrete types
C-compatible API         templates / variants possible
```

The boundary is a versioned `extern "C"` struct of function pointers with POD arguments:

```cpp
extern "C" {
struct PluginApi {
    uint32_t abi_version;

    void* (*create)();
    void  (*destroy)(void*);
    bool  (*process)(void*, const PacketView*, Result*);
};
}
```

— plus a declarative registration that tells the host which keys the plugin claims, since the host
builds the table before traffic arrives. Internally the host adapts that into whatever the optimizer
likes best.

Note what this disqualifies: `std::variant` is a *closed* set of alternatives, so it cannot admit a
handler that arrives at runtime from a shared object. That is not a performance verdict; it is the
premise excluding an option.

Around it: allocation paired across the boundary because the two binaries have different heaps; no
exception may unwind across it; RTTI identity is not reliable between DSOs; unload is a much harder
promise than it looks and "we never unload" is a defensible answer; loading is a security boundary;
and the compatibility matrix is the reason the interface must stay small.

**And the measurement that decides all of it.** The brief gives handler count but not traffic
distribution. If real traffic is dominated by a few protocols and the list is ordered by frequency,
the scan costs two probes and the table buys the remaining few percent. Measure the distribution
before building anything, and state in advance the result that would make you ship a one-line sort
instead.

</details>
