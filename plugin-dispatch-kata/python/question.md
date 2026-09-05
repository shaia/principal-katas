# The Python track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

This is the track that tests whether you re-derive or translate. Two things invert relative to the
other tracks. The *performance* half of the brief collapses — the dispatch-mechanism question the
other tracks spend their effort on is meaningless when every attribute access is a dictionary lookup,
and the target rate is off by orders of magnitude, which you must state before designing anything.
The *ABI* half, meanwhile, is more real here than it is in Go: CPython met exactly this problem and
shipped exactly the versioned C interface the Principal rubric asks for, so you can critique the
reference answer rather than invent one.

The step that carries the track is P1, and the one that pays back the most is P3.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step P1 — State the ceiling before you design anything

The brief asks for several million packets per second. Establish whether you can have them, in
numbers, before you choose a structure.

Then answer the question that result forces.

**Be prepared to say:**

- what one `h.matches(p)` costs, decomposed into the operations the interpreter actually performs.
  Use `dis` and count; do not estimate
- the measured ceiling of the *original loop* on your machine, in packets/sec, with the handlers
  doing nothing at all
- the measured ceiling of the best possible dispatch — a dict lookup and one call, no matching — and
  the ratio between the two. That ratio is the main kata's step 2, and it is larger here than
  anywhere else
- both numbers next to the brief's requirement, and the gap expressed as an order of magnitude
- what you tell someone whose requirement is genuinely above that ceiling. There is a correct
  engineering answer and it is not "optimize harder"

<details>
<summary>Hint</summary>

Measure both halves, because the interesting result is the *ratio* rather than either number. Fifty
interpreted predicate calls replaced by one C-level dict lookup is a much bigger multiple than the
same substitution in C++, where the scan and the lookup are both a handful of nanoseconds apart. The
cross-track finding is in that comparison: the higher the per-operation overhead of the runtime, the
more the O(n)→O(1) fix dominates and the less the polymorphism mechanism matters. All three of the
brief's mechanism options are noise here.

Then take the absolute number seriously. If the ceiling is two orders of magnitude below the
requirement, no arrangement of Python code closes it, and a design document that says so — with the
measurement, the requirement, and where the line crosses — is a better deliverable than one that
quietly aims below the target.

Which turns the brief into a different question, and it is the one this track is really about: given
that Python cannot be the data path, what is it for in this system, and where does the boundary
actually fall? Answer that before step P2.

</details>

**Done when** you can quote both ceilings against the requirement, and state in one sentence what
role Python plays in a service that must move packets at this rate.

---

## Step P2 — The dispatch fix, and what it does and does not buy

Do step 2 of the main kata here, and measure it. Then find its limit, because the mechanism that
makes it fast also constrains what a handler is allowed to be.

**Be prepared to say:**

- your table: the container, the key, and where the lookup lands relative to interpreted code
- how much of the win survives if the residual predicates from step 2 still run interpreted for every
  packet in the fallback set — put a number on the fallback set's affordable size
- `functools.singledispatch`, `match`, and an `if/elif` chain over 50 protocols: what each one does at
  runtime, and which of the three is still a linear scan
- the micro-optimizations that matter at this budget and are usually cargo-culted: binding the lookup
  to a local, `__slots__`, avoiding attribute chains in the hot loop. Measure each, and discard the
  ones that do nothing
- the version question: what the specializing interpreter changed about all of the above, and whether
  your measurements are specific to the version you ran them on

<details>
<summary>Hint</summary>

The reason this works is that a dict lookup happens entirely below the interpreter and the loop does
not — so the goal is not "fewer operations", it is *moving the per-packet work out of bytecode
entirely*. Judge every candidate structure by that criterion and the ranking becomes obvious before
you benchmark.

Which is also the constraint. The instant a handler's selection needs interpreted code, that packet
pays interpreted cost, so the fallback list is not merely a design wart as it was in C++ — it is a
second performance tier with a different cost model, and you should know its size and its rate.

For the third probe, look at what each construct is implemented as. Two of them are dispatch on a
value through a hash; one of them is a sequence of comparisons wearing better syntax. Being wrong
about which is which is the common failure.

</details>

**Done when** you have a measured ns/packet for table hits and for fallback misses, and can state the
fallback rate at which the two-tier design stops being worth it.

---

## Step P3 — The reference answer already exists; critique it

Step 5 of the main kata asked you to design a versioned C ABI for loading code into a running
process. CPython did this. Go and read what it shipped, then evaluate it as a design.

**Be prepared to say:**

- the two boundaries Python actually offers — importing a module, and a compiled extension — and
  which of the brief's problems each one has and does not have
- the stable-ABI mechanism by name: what it guarantees, what it forbids you from touching, and what
  it costs at call time compared to the unrestricted interface
- what a version field buys, in a system where the *host* is the thing being versioned — and how a
  plugin says which hosts it works with
- the free-threaded build: what it does to the ABI story, and what a plugin author now has to ship
- the pure-Python boundary has no ABI problem at all. Say what problem it has instead, and why it is
  not obviously the easier one

<details>
<summary>Hint</summary>

Map the brief's `PluginApi` struct onto what CPython does and the correspondence is almost
line-for-line: a versioned contract, opaque handles, functions reached through a table rather than by
layout, and an explicit rule about who owns what. Reading a shipped answer to your own design
question is worth more than another round of inventing one — including its compromises, because the
restrictions the stable interface imposes are precisely the ones step C3 of the [C++
track](../cpp/question.md) explains the need for.

For the version probe, notice the inversion: in the brief the host defines the ABI and plugins
target it, and here that is also true — so a plugin is built against a version of the host and must
declare the range it supports. That is the same problem as the main kata's step 8 compatibility
matrix, and the packaging ecosystem's answer to it is worth studying as an operational design rather
than as a tooling detail.

The last probe is the one people get wrong by assuming duck typing removed the problem. It moved it:
there is no layout to get wrong, but nothing checks that a handler still means what it meant last
release, and the failure arrives at runtime in production rather than at load.

</details>

**Done when** you can name the stable-ABI mechanism, state one thing it forbids and why that
restriction exists, and say what the pure-Python boundary trades the ABI problem *for*.

---

## Step P4 — Ownership is a count, and it crosses the boundary

Step 6 of the main kata asked who owns what. Here ownership is a number stored in the object, and
the boundary's job is to agree about who changes it.

**Be prepared to say:**

- borrowed versus owned, stated as a rule about who is responsible for what — and the single most
  common way a C extension gets it wrong
- what happens when a plugin keeps a borrowed reference past the call that lent it. Describe the
  observed failure, and say how far from the cause it appears
- how the packet bytes reach the handler without a copy, which protocol makes that possible, and what
  the handler must do when it is finished
- the allocator question from step 6, in Python's vocabulary: the several allocation interfaces
  available to an extension, and what happens if memory acquired from one is released to another
- what any of this looks like from a *pure-Python* handler, and why that boundary is safe by
  construction

<details>
<summary>Hint</summary>

The refcount is the direct analogue of the C++ track's "who calls `destroy`", and it fails the same
way: too few and you free memory somebody is using, too many and you leak — with the same asymmetry,
that the leak is the one that survives testing.

The zero-copy probe is the interesting engineering question rather than the trivia one. There is a
protocol for handing out a view of a buffer without copying it, it has an explicit acquire/release
discipline, and that discipline exists precisely because the owner may not resize or free the buffer
while a view is outstanding. That is the same lifetime contract as step 5 of the main kata's packet
argument, enforced by a runtime rather than by a comment — so say what you would still have to
enforce yourself.

For the allocators, note that there is more than one interface and they are not interchangeable, and
that mismatching them is exactly the two-heaps failure from the C++ track with different function
names.

</details>

**Done when** you can state the borrowed/owned rule in one sentence, describe a retained-reference
failure and where it surfaces, and name the protocol that gets bytes across without a copy.

---

## Step P5 — Errors, and the lock you are holding

An exception cannot cross a C frame, and a handler running in your process holds something every
other handler needs.

**Be prepared to say:**

- the error-return protocol at the C boundary: what the function returns, what else it must have
  done, and what the caller must check. Say what happens if a caller forgets
- what a plugin must do around a long or blocking operation, what it must not touch while doing it,
  and what it must re-check afterwards
- whether releasing that lock is safe given your step P4 answer about the packet buffer — this is the
  interaction, and it is not obvious
- what a handler that never yields does to the rest of the service, and what your containment story
  is for a handler that crashes rather than raises
- the free-threaded build: what it changes about all of the above, and what a plugin must declare

<details>
<summary>Hint</summary>

The error protocol is two things, not one, and every C-boundary bug of this class is a function that
did one of them. Then ask what a caller who ignores the return value produces — the answer is not a
clean failure, it is an exception that surfaces at an unrelated later point, which makes ignoring the
return value a bug that shows up in someone else's code.

The lock probe is where the design content is. Releasing it around slow work is correct and necessary
— and it is also the moment another thread may run, which means anything the handler is holding a
raw view of must still be valid when it resumes. Put that next to your zero-copy decision from P4 and
decide whether the two are compatible, because on the face of it they are not.

For the last probe: the ABI question from P3 and the concurrency question here are the same question
in the free-threaded build, and the mechanism by which an extension states its position is explicit
and checkable.

</details>

**Done when** you can state both halves of the error protocol, and say whether a handler may release
the lock while holding a view of the packet.

---

## Step P6 — When the handlers are Python and the path is not

Take the answer from step P1 seriously and build the system it implies: policy expressed in Python,
packets handled by something else.

**Be prepared to say:**

- what crosses that boundary, and how often. If the answer is "per packet" you have not taken P1
  seriously
- how a Python-authored handler becomes something the fast path can execute: what is generated, when,
  and by what. Name the artifact
- what you gain by keeping the authoring layer in Python, stated as capabilities rather than as
  comfort — there are at least three and they are the reason this architecture is common
- how you validate a handler before it reaches the fast path, and what you can prove offline that you
  could not prove in C++
- reloading: what `importlib.reload` actually does to objects that already exist, why two copies of a
  module yield two classes that `isinstance` will not equate — step 7 of the main kata's type-identity
  probe, in the form Python has it — and why the hot-reload story people expect is not the one they
  get

<details>
<summary>Hint</summary>

The move is to stop treating the Python layer as a slow implementation of the data path and start
treating it as the thing that *produces* the data path's configuration. Then the per-packet cost of
Python is zero, because Python is not on the packet path — it runs when the handler set changes, not
when a packet arrives.

That reframes every question in this kata. The step 5 ABI becomes a description format rather than a
calling convention; the step 6 ownership problem largely evaporates because nothing is shared at
runtime; step 8's rollout question gets easier because the artifact is data you can diff, sign, and
roll back; and validation gets *better* than the C++ track could manage, because you are checking a
declarative object rather than arbitrary code.

The cost is that handlers can only express what the description format can express — which is the
main kata's step 2 constraint returning as an architecture-wide one. Say what you do with the handler
that does not fit.

The reload probe is the trap. Existing instances keep their old classes; other modules keep their old
references; anything captured at import time is unchanged. Work out what that means for a table you
built at startup.

</details>

**Done when** you can name the artifact the Python layer emits, say what the fast path does with it,
and state what a handler is no longer allowed to do.

---

<details>
<summary><b>The shape of the Python answer</b> — read after you have your own.</summary>

Two numbers decide this track, and both are measured in step P1. The first is that replacing the
50-probe interpreted scan with one dict lookup wins a far larger multiple than the same change wins
in C++ — so the main kata's central insight is not merely portable here, it is amplified, and the
brief's four mechanism options are noise by comparison. The second is that even the best number is
one to two orders of magnitude below the requirement, which means Python is not the data path and
saying so with the measurement attached is the deliverable.

That produces the honest architecture: Python is the control plane. Handlers are authored in Python,
validated offline, and compiled into a table or rule set that a C, Rust, or kernel-level fast path
executes. Per-packet Python cost is zero because Python is not there when the packet is. What is
gained is expressiveness, offline validation, safe authorship by people who are not systems
engineers, and a deployable artifact that is data rather than code — signable, diffable, reversible,
which is a better answer to the main kata's step 8 than any of the other tracks manage.

If a compiled extension *is* required, the ABI half of the brief is real and CPython already answered
it — a versioned, opaque, table-based C interface with an explicit ownership discipline, whose
restrictions map one-for-one onto the failure modes the C++ track enumerates. Its compromises are
worth studying: what the stable interface forbids, what it costs at call time, and how the
free-threaded build splits the compatibility matrix again.

And the ownership question does not go away, it changes units. Refcounts are the `create`/`destroy`
pairing; the buffer protocol is the packet argument's lifetime contract; the several allocation
interfaces are the two-heaps problem with different names; and releasing the interpreter lock around
slow work interacts with zero-copy views in a way you have to decide deliberately.

</details>

Then read what the same questions look like in the other two:
[cpp/question.md](../cpp/question.md) · [go/question.md](../go/question.md).
