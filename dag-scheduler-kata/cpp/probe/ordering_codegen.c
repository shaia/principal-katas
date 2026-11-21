/*
 * What the brief's relaxed decrement compiles to, beside the correct ones.
 *
 * Not part of the build. Compile it to assembly for each target and compare:
 *
 *   clang --target=x86_64-linux-gnu  -O2 -S -o - ordering_codegen.c
 *   clang --target=aarch64-linux-gnu -O2 -S -o - ordering_codegen.c                  (ARMv8.0: LL/SC)
 *   clang --target=aarch64-linux-gnu -O2 -S -o - -march=armv8.1-a ordering_codegen.c (LSE atomics)
 *
 * No headers, so any clang can target any of the three. The builtins take the
 * same orderings std::atomic::fetch_sub does, and lower the same way.
 *
 * Two shapes, because the contract has two ends:
 *
 *   finish_*  a predecessor: write the payload, then decrement. The store must
 *             not become visible after the decrement — that is the release.
 *   join_*    the last predecessor: decrement, and if it was the last, read
 *             another predecessor's payload. The read must not be satisfied
 *             before the decrement — that is the acquire.
 *
 * solution.md §2 quotes the output.
 */

typedef unsigned long long u64;
typedef unsigned           u32;

struct node {
    u64 out;
    u32 pending;
};

/* --- a predecessor finishing ------------------------------------------------ */

int finish_relaxed(struct node* me, struct node* succ, u64 v) {
    me->out = v;
    return __atomic_fetch_sub(&succ->pending, 1, __ATOMIC_RELAXED) == 1;
}

int finish_release(struct node* me, struct node* succ, u64 v) {
    me->out = v;
    return __atomic_fetch_sub(&succ->pending, 1, __ATOMIC_RELEASE) == 1;
}

int finish_acq_rel(struct node* me, struct node* succ, u64 v) {
    me->out = v;
    return __atomic_fetch_sub(&succ->pending, 1, __ATOMIC_ACQ_REL) == 1;
}

/* --- the last predecessor reading another's payload ------------------------ */

u64 join_relaxed(struct node* other, struct node* succ) {
    if (__atomic_fetch_sub(&succ->pending, 1, __ATOMIC_RELAXED) != 1) return 0;
    return other->out;
}

u64 join_acq_rel(struct node* other, struct node* succ) {
    if (__atomic_fetch_sub(&succ->pending, 1, __ATOMIC_ACQ_REL) != 1) return 0;
    return other->out;
}

/* release on every decrement; only the last pays for the acquire */
u64 join_release_fence(struct node* other, struct node* succ) {
    if (__atomic_fetch_sub(&succ->pending, 1, __ATOMIC_RELEASE) != 1) return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return other->out;
}

u64 join_release_load(struct node* other, struct node* succ) {
    if (__atomic_fetch_sub(&succ->pending, 1, __ATOMIC_RELEASE) != 1) return 0;
    (void)__atomic_load_n(&succ->pending, __ATOMIC_ACQUIRE);
    return other->out;
}
