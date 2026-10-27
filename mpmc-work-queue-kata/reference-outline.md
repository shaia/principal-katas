# The reference outline

The concise answer outline that came with the question, reproduced as it was given. Only the
formatting is restored: code and diagrams are fenced, and the tradeoffs are set as a table. Its
section numbering is the original's. [cpp/solution.md §13](cpp/solution.md#13-the-reference-outline-checked)
checks it claim by claim against measurements, including the experiment it proposes in §6.

Read [question.md](question.md) and commit to an answer first. This is an answer.

---

## A. Why the original implementation fails

Consider:

``` text
Producer A                     Consumer B

tail.fetch_add(1)
     │
     ▼
reserves slot 42
                               head < tail
                                     │
                                     ▼
                               reads slot 42
     │
     ▼
writes slot 42
```

The consumer may access an element before the producer has finished writing it.

The queue also has no capacity check. A sufficiently fast producer can overwrite unread elements.

Worse, `head.fetch_add()` can assign a consumer a position that is no longer valid by the time it
accesses the buffer.

Atomic index increments guarantee unique tickets, not safe access to the objects associated with
those tickets.

## B. Use per-slot sequence numbers

A common bounded MPMC design associates a sequence number with each slot.

```cpp
template<typename T>
struct Slot {
    std::atomic<uint64_t> sequence;
    alignas(T) std::byte storage[sizeof(T)];
};
```

Each slot cycles through generations.

For a queue of capacity N, conceptually:

``` text
Producer reserves ticket P
         │
         ▼
Wait/check until slot.sequence == P
         │
         ▼
Construct element
         │
         ▼
slot.sequence.store(P + 1, release)
         │
         ▼
Consumer observes P + 1 with acquire
         │
         ▼
Read and destroy element
         │
         ▼
slot.sequence.store(P + N, release)
```

The producer and consumer indices are advanced using CAS operations only after verifying the
relevant slot state.

An enqueue must return full rather than blindly reserving a ticket when capacity is exhausted.

The sequence number distinguishes different generations occupying the same physical slot.

## C. Memory ordering

The producer publishes an initialized element:

```cpp
// Element construction happens before publication.
slot.sequence.store(
    ticket + 1,
    std::memory_order_release
);
```

The consumer observes publication:

```cpp
auto sequence = slot.sequence.load(
    std::memory_order_acquire
);
```

When the consumer observes the matching published generation, the acquire operation synchronizes
with the producer's release.

The element's initialization is now visible to the consumer.

After processing, the consumer publishes that the slot is reusable.

Atomic ticket allocation can often use relaxed operations because the per-slot publication protocol
establishes the required visibility.

However, the algorithm must separately prove that no two threads simultaneously own the same slot.

For nontrivial T, object construction and destruction must also be handled correctly. Throwing
constructors or moves require an explicit failure protocol; otherwise, an abandoned reservation can
permanently block queue progress.

## 4. The difficult problem: lost wakeups

Suppose consumers sleep when the queue is empty.

A naive approach is:

```cpp
if (queue.empty()) {
    std::unique_lock lock(mutex);
    cv.wait(lock);
}
```

Consider:

``` text
Consumer                       Producer

checks queue: empty
                               pushes item
                               notify_one()
consumer starts waiting
```

The notification has already occurred.

The consumer may now sleep indefinitely even though work exists.

A correct solution needs a protocol that atomically relates the condition being observed to the act
of waiting.

In C++20, one option is a notification-generation counter:

```cpp
std::atomic<uint64_t> wake_epoch{0};
```

A producer increments the counter after publishing an event and notifies:

```cpp
wake_epoch.fetch_add(
    1, std::memory_order_release
);

wake_epoch.notify_one();
```

The consumer captures the generation before checking whether work exists:

```cpp
while (true) {
    auto epoch = wake_epoch.load(
        std::memory_order_acquire
    );

    if (queue.try_pop(item))
        break;

    wake_epoch.wait(
        epoch,
        std::memory_order_acquire
    );
}
```

If a producer increments the generation between the load and `wait()`, the wait returns rather than
sleeping on the old value.

This eliminates that specific lost-wakeup race.

Production code must also handle shutdown, multiple waiting consumers, and the queue's precise
publication semantics. Blocking and notification do not replace the underlying queue correctness
proof.

## 5. Principal-level problem: preempted producers

Consider a producer that successfully claims the next slot but is preempted before publishing its
element.

``` text
Producer A
    │
    ▼
reserves ticket 100
    │
    ▼
PREEMPTED

Producer B
    │
    ▼
publishes ticket 101
```

The consumer expects ticket 100.

It cannot safely skip the missing element merely because ticket 101 is ready.

This creates head-of-line blocking.

Under certain schedules, a stalled producer can prevent consumers from progressing and eventually
cause the bounded queue to fill.

This exposes an important distinction:

A queue that uses atomic operations and avoids mutexes is not necessarily lock-free in the formal
progress-guarantee sense.

A Principal candidate should evaluate whether the selected algorithm actually provides the required
progress guarantees when an individual thread is indefinitely delayed.

The system might accept this limitation, introduce a more sophisticated helping protocol, or use a
different queue architecture.

## 6. Key tradeoffs

| Design | Advantages | Disadvantages |
|---|---|---|
| Mutex + condition variable | Simple correctness and shutdown | Contention and blocking |
| Bounded atomic MPMC ring | Fixed memory, efficient common path | Complex synchronization and progress analysis |
| Per-producer SPSC queues | Excellent locality, simpler synchronization | Consumer aggregation and balancing |
| Spin waiting | Low wakeup latency | CPU consumption and contention |
| Atomic wait/notify | Efficient idle behavior | Wakeup latency and coordination complexity |

At 5 million events/sec, I would also benchmark 32 producer-local SPSC queues against one shared
MPMC queue.

Eliminating shared producer contention may outperform optimizing a highly contended global queue.

## 7. Principal-level answer

My design would establish these invariants:

1. Every accepted event has exactly one owning slot.
2. Consumers never access unpublished elements.
3. Slots are never reused until their previous elements are consumed and destroyed.
4. Queue capacity is always bounded.
5. Shutdown prevents new reservations, allows accepted in-flight publications to finish, and drains
   all committed elements.
6. Sleeping consumers can always observe new work or shutdown.

Shutdown deserves special attention: setting a closed flag is insufficient if a producer has
already passed its admission check but not completed publication. Closure must coordinate with
in-flight reservations.

I would measure throughput, p50/p99 enqueue and dequeue latency, CAS retries, queue occupancy,
full-queue frequency, wakeup latency, and CPU consumption while idle.

Principal-level insight:

A high-performance concurrent queue is not primarily about atomic indices. It is about ownership
transfer, publication ordering, bounded resources, and well-defined progress guarantees.
