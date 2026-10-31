#pragma once
//
// The counter-argument, built properly: a ring under one mutex with two
// condition variables — the main kata's step 10.
//
// Here reservation, construction and publication happen inside one critical
// section, so the reserved-but-unpublished state still exists but no other
// thread can observe it. The wakeup cannot be lost (the check and the sleep are
// under the lock the notifier takes), and a close cannot strand a slot (the
// flag and the push are checked under the same lock). What it costs is that
// every operation serializes on one line, and a preempted holder blocks
// everyone. The batch calls exist so that cost is measured at its best, not at
// its worst: one acquisition per batch rather than per event.

#include "platform.hpp"
#include "queue_api.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace turnstile {

template <typename T, std::size_t N>
class MutexQueue {
public:
    MutexQueue() : ring_(new T[N]) {}

    Push try_push(T&& v, OpStats&) {
        {
            std::lock_guard lk(mu_);
            if (closed_) return Push::closed;
            if (size_ == N) return Push::full;
            put(std::move(v));
        }
        not_empty_.notify_one();
        return Push::ok;
    }

    Push push(T&& v, Clock::time_point until, OpStats& st) {
        {
            std::unique_lock lk(mu_);
            while (!closed_ && size_ == N) {
                ++st.parks;
                if (not_full_.wait_until(lk, until) == std::cv_status::timeout && size_ == N && !closed_)
                    return Push::timeout;
            }
            if (closed_) return Push::closed;
            put(std::move(v));
        }
        not_empty_.notify_one();
        return Push::ok;
    }

    // Accepts items in order until all n are in, the queue closes, or `until`
    // passes; returns how many were accepted. One acquisition while there is room.
    std::size_t push_batch(T* items, std::size_t n, Clock::time_point until, OpStats& st) {
        std::size_t done = 0;
        std::unique_lock lk(mu_);
        while (done < n) {
            while (!closed_ && size_ == N) {
                ++st.parks;
                if (not_full_.wait_until(lk, until) == std::cv_status::timeout && size_ == N && !closed_) {
                    lk.unlock();
                    not_empty_.notify_all();
                    return done;
                }
            }
            if (closed_) break;
            while (done < n && size_ < N) put(std::move(items[done++]));
            lk.unlock();
            not_empty_.notify_all();
            lk.lock();
        }
        return done;
    }

    Pop try_pop(T& out, OpStats&) {
        {
            std::lock_guard lk(mu_);
            if (size_ == 0) return Pop::empty;
            take(out);
        }
        not_full_.notify_one();
        return Pop::ok;
    }

    Pop pop(T& out, Clock::time_point until, OpStats& st) {
        {
            std::unique_lock lk(mu_);
            while (size_ == 0) {
                if (closed_) return Pop::drained;
                ++st.parks;
                if (not_empty_.wait_until(lk, until) == std::cv_status::timeout && size_ == 0)
                    return closed_ ? Pop::drained : Pop::timeout;
            }
            take(out);
        }
        not_full_.notify_one();
        return Pop::ok;
    }

    // Up to `max` events in one acquisition. Returns 0 only when drained or timed out.
    std::size_t pop_batch(T* out, std::size_t max, Clock::time_point until, OpStats& st) {
        std::size_t n = 0;
        {
            std::unique_lock lk(mu_);
            while (size_ == 0) {
                if (closed_) return 0;
                ++st.parks;
                if (not_empty_.wait_until(lk, until) == std::cv_status::timeout && size_ == 0) return 0;
            }
            while (n < max && size_ > 0) take(out[n++]);
        }
        not_full_.notify_all();
        return n;
    }

    void close() {
        {
            std::lock_guard lk(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() {
        std::lock_guard lk(mu_);
        return closed_;
    }

private:
    void put(T&& v) {
        ring_[(head_ + size_) % N] = std::move(v);
        ++size_;
    }
    void take(T& out) {
        out   = std::move(ring_[head_]);
        head_ = (head_ + 1) % N;
        --size_;
    }

    std::mutex              mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::unique_ptr<T[]>    ring_;
    std::size_t             head_   = 0;
    std::size_t             size_   = 0;
    bool                    closed_ = false;
};

}  // namespace turnstile
