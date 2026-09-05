# The C++ track

Extends [../question.md](../question.md). **Do steps 0–10 there first** — this track asks about *your*
design, and every step below assumes you have one you can defend.

C++ is the only track where all four of the brief's options are real, which makes it the only track
that has to disqualify one of them properly. It is also the language where the boundary between two
binaries is entirely your problem: nothing checks that two shared objects agree about layout, about
which allocator owns a pointer, or about what a type's identity is — they will link, load, and run,
and be wrong. Four of the five steps below are questions the other two tracks cannot ask. The fifth
is about proving the optimizer did the thing you assumed it did.

Same rules as the main kata: answer before opening the hint, and the fold at the bottom is the only
place the shape is given.

---

## Step C1 — Price all four, and eliminate one on the premise

Step 3 of the main kata asked which mechanism, post-lookup. Now do it properly, with numbers, for
50–100 handlers rather than the two in every blog post.

**Be prepared to say:**

- `std::variant<TcpHandler, UdpHandler, …>` over 80 alternatives: what `std::visit` generates, how
  large the object is, what compile time does — and the property of the type that makes it
  unusable here regardless of any of that
- templates: what monomorphizing your pipeline per handler does to `.text`, and what happens to that
  argument the moment a handler lives in a different translation unit, let alone a different `.so`
- custom type erasure: write down what you are actually building, and say honestly how it differs
  from what the compiler already emitted for you
- small-buffer optimization in a type-erased handler: the size you would pick, how you would pick it,
  and what you measure to know it was right
- the case for *keeping virtual*: state it as a positive argument, not as a failure to find better

<details>
<summary>Hint</summary>

Do the first probe last-clause-first. Ask what set of types a `std::variant` may hold, and when that
set is fixed. Then re-read the sentence in the brief about where some handlers come from. The
elimination is one sentence long and no benchmark is involved — which makes it a better answer than
any measurement, because it cannot be argued with.

For the middle two: a hand-rolled type-erased handler is a struct holding a pointer to data and a
pointer to a table of function pointers. Write it out and put it next to what the compiler emits for
an abstract base class. If your version is faster, say precisely which cost you removed — there are
real answers here (no RTTI, no unwind tables, control over layout, one indirection instead of two,
a table you can copy into hot memory), and there are imaginary ones.

For code size, get the actual number rather than reasoning about it: build with each mechanism and
compare section sizes.

</details>

**Done when** you can eliminate one option in a sentence that mentions no performance number at all,
and give measured `.text` sizes for the ones you kept.

---

## Step C2 — Write the ABI in bytes

Step 5 of the main kata asked for the boundary. Now write it as code, and defend every line of it as
a promise you can keep for five years. Its probe about types that cross cleanly and are wrong is a
general question there; here it has a specific answer, and it is the third probe below.

**Be prepared to say:**

- the exported symbol's name and type, how you keep the compiler from mangling it, and how you keep
  every *other* symbol in the plugin from being exported
- why each field is the type it is: fixed-width integers, explicit lengths, no `bool` in a struct
  that crosses, no `size_t`, no enum without an underlying type. Give the reason for each rule, not
  the rule
- your extension mechanism. Two patterns are in wide use; name both, say how they compose, and note
  which one must be present in version 1 or never
- calling convention and struct layout: what you are relying on, what would change it, and whether
  a packing pragma is a fix or a symptom
- the constructor problem: what runs in the plugin before your `create` does, in what order, and
  what happens if it throws

<details>
<summary>Hint</summary>

Apply one test to every field: could this be produced by a different compiler, a different standard
library, or a different language, three years from now? `std::string`, `std::vector`,
`std::shared_ptr` and `std::function` all fail it, and none of them fails at link time.
`_GLIBCXX_USE_CXX11_ABI` is the famous instance; it is not the only one, and the ones without names
are worse.

For extension, look at what shipped systems do — a size field the caller fills in, and a capability
or version gate. One of them lets a new host detect an old plugin's shorter struct; the other lets an
old host ignore a new plugin's extra entries. You want both, and you cannot add the first one later.

The last probe is easy to skip. Loading a shared object runs static initializers before you call
anything, in an order you do not control, in a binary you did not build.

</details>

**Done when** you can hand the struct to someone writing a plugin in C and have them need nothing
else, and you can add a function to it without recompiling a deployed plugin.

---

## Step C3 — Two standard libraries in one address space

The host and the plugin were built by different people. Enumerate what can be different, and what
each difference does. This is step 6's probe about two runtimes in one process, in its C++ form —
which is the worst of the three languages', because most of it is silent.

**Be prepared to say:**

- what happens when the plugin links the standard library statically and the host does so
  dynamically, or they link different versions of it — for the allocator specifically
- the same question for the C runtime on Windows, where the failure is more immediate and easier to
  diagnose. Say why
- ODR: two definitions of an inline function or a template with different flags, merged by the
  loader. Describe the resulting program, and say which tool would have told you
- `RTLD_LOCAL` versus `RTLD_GLOBAL`, and what each one does to symbol interposition between host and
  plugin
- your build-side defence: what you compile plugins with, what you check at load, and what you
  cannot check at all

<details>
<summary>Hint</summary>

Start with the allocator, because it makes the rest concrete. If the plugin's `new` and the host's
`delete` are different functions operating on different heaps, then any object crossing the boundary
that is destroyed on the wrong side is a corruption whose crash arrives later, elsewhere, in
unrelated code. This is the mechanical reason behind the `create`/`destroy` pairing of step 6 — not
symmetry, arithmetic.

Then ODR, which is the one you cannot defend against with discipline alone. The loader is permitted
to pick one definition of a symbol that appears in both binaries. If the two definitions were
compiled with different standard-library versions, different `-D` flags, or different inlining
decisions, one binary is now calling a function that does not match what it was compiled against —
silently, with no diagnostic anywhere in the toolchain.

Then notice which of these the C boundary from C2 makes impossible rather than merely unlikely, and
write that down as the *reason* for the C boundary. It is a stronger argument than "C is portable".

</details>

**Done when** you can name, for each difference, whether it fails at link, at load, at first call, or
much later — and say which of them your step C2 boundary rules out by construction.

---

## Step C4 — Exceptions and type identity between binaries

Two failure modes that look like language features working normally and are not. The second is step
7's type-identity probe, and the received answer — that the mismatch is silent, a cast that simply
returns null — is worth treating as a claim to be checked rather than a fact. Whether it holds turns
out to depend on a build-time constant of the standard library you did not choose.

**Be prepared to say:**

- what a `throw` needs from the runtime to cross a frame, and which of those things the host and
  plugin must share
- your shim: where the `catch (...)` lives, what it converts to, and what it must be marked to be
  correct rather than merely conventional
- `dynamic_cast<Derived*>` on an object created in a plugin, in a host that has the identical class
  definition. Explain the mechanism that could make it return null, say which loader flag is
  implicated — and then actually run it, because the mechanism being real does not settle whether
  the failure occurs
- what `typeid(x) == typeid(y)` actually compares in your implementation, and why it can disagree
  between two binaries that agree byte-for-byte about the class
- whether you would build plugins with `-fno-exceptions` / `-fno-rtti`, and what that decides for
  everyone downstream

<details>
<summary>Hint</summary>

Unwinding is a protocol with participants: unwind tables, a personality routine, and the runtime
that implements it. Two binaries that do not share one do not share exceptions, and the failure is
not a compile error.

RTTI is the subtler half. Implementations commonly compare `type_info` by *address* of the name or
the object, with a fallback to string comparison depending on the platform and the flags. Two copies
of the same class in two DSOs can therefore have two distinct type identities — and whether they
collapse into one depends on how the object was loaded and how symbols were made visible. Go and
find the flag; the interaction with `-fvisibility=hidden`, which you almost certainly wanted for
step C2, is the part that surprises people.

Both failures share a shape worth naming: the language feature is not broken, it is *scoped*, and
its scope is one binary rather than one process.

</details>

**Done when** you can explain a null `dynamic_cast` between agreeing binaries, and say what your
boundary does with an exception that reaches it.

---

## Step C5 — Prove the optimizer did what you assumed

You have made claims about inlining, devirtualization, branch prediction and instruction cache. Each
of those is checkable. Check them.

**Be prepared to say:**

- how you verify a devirtualization actually happened — the specific thing you look at, not "check
  the assembly"
- the counters that distinguish your step 1 diagnosis from the option list's: which hardware events
  you collect, and what pattern in them supports which story
- how you observe instruction-cache pressure specifically, given that it does not show up as a
  simple IPC drop
- the microbenchmark trap: what a dispatch benchmark holds resident that your service does not, and
  how you build a benchmark that does not lie in that direction
- what you would measure on the *plugin* path that you do not measure on the internal path, and why
  the numbers are not comparable

<details>
<summary>Hint</summary>

Two of these have direct tooling answers and you should find them rather than reason about them:
branch mispredictions and front-end stalls are countable events, and the difference between "we are
mispredicting the scan's branches" and "the indirect call is slow" shows up as different counters
moving. That is what makes step 0 of the main kata an empirical question rather than an argument.

For devirtualization, the honest check is at the machine-code level — look for the load of the vtable
pointer and the indirect branch, and note whether they are still there. LTO changes the answer, and
whether your handlers are in the same translation unit changes it more.

For the benchmark trap: a loop calling one handler keeps its code hot, its data hot, and its branch
target perfectly predicted. Your service interleaves 80 handlers. Build the benchmark that
interleaves, with a distribution you took from step 9 of the main kata, and watch the ranking of the
mechanisms change.

</details>

**Done when** you have counter evidence for the step 0 diagnosis and can point at the instruction
that either is or is not an indirect branch.

---

<details>
<summary><b>The shape of the C++ answer</b> — read after you have your own.</summary>

The elimination that costs no benchmark: `std::variant` is a closed set fixed at compile time, and
the brief says handlers arrive from separately compiled shared libraries. Option 2 is out on the
premise. Templates go the same way for the plugin half and survive only for built-ins — which is
already the two-plane split arriving whether you invited it or not.

What is left internally is a table of `Handler*` and one indirect call, which after step 2 of the
main kata is a small fraction of the per-packet budget. Keeping `virtual` is a defensible positive
choice at that point, and custom type erasure earns its place only if you can name the specific cost
it removes — usually a second indirection, or control over where the function pointers live so that
the dispatch table is contiguous and hot.

The boundary is `extern "C"`, POD, fixed-width, size-and-version extensible, with allocation paired
across it, `noexcept` shims at every entry point, no RTTI dependency, and `-fvisibility=hidden` so
that only the one entry symbol is exported. The reason is not portability in the abstract: it is that
a C boundary makes the allocator mismatch, the ODR merge, the unwinder mismatch and the `type_info`
identity problem impossible rather than merely unlikely — four silent failure modes removed by one
decision.

The measurement that decides whether any of it was necessary is a hardware counter, and it is
available before you write a line of it.

</details>

Then read what the same questions look like in the other two:
[go/question.md](../go/question.md) · [python/question.md](../python/question.md).
