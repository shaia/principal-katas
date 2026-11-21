#pragma once
#include "deque.hpp"
#include "placement.hpp"
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dag {
using Id = std::uint32_t;
using Function = std::uint64_t(*)(std::span<const std::uint64_t>, std::stop_token, void*);
enum class State { waiting, running, succeeded, failed, canceled, skipped };
enum class Arm { global, sharded, chase, sequential };
struct Node {
    Function function{}; void* context{};
    std::array<Id, 8> predecessors{};
    std::uint32_t count{};
    std::uint64_t cost{1}, rank{}, earliest{};
    bool critical{};
};
struct Graph {
    std::vector<Node> nodes;
    std::vector<std::size_t> offsets;
    std::vector<Id> successors;
    std::vector<Id> order;
    std::uint64_t span{}, work{};
};
class Builder {
    std::vector<Node> nodes_;
public:
    Id add(Function fn, void* context = nullptr, std::uint64_t cost = 1) {
        if (!fn || !cost || nodes_.size() >= std::numeric_limits<Id>::max()) throw std::invalid_argument("invalid task");
        Node n; n.function=fn; n.context=context; n.cost=cost;
        nodes_.push_back(n); return static_cast<Id>(nodes_.size()-1);
    }
    void edge(Id from, Id to) {
        if (from >= nodes_.size() || to >= nodes_.size()) throw std::invalid_argument("invalid node");
        auto& n=nodes_[to];
        if (n.count == 8 || std::find(n.predecessors.begin(), n.predecessors.begin()+n.count, from) != n.predecessors.begin()+n.count)
            throw std::invalid_argument("duplicate edge or indegree exceeds eight");
        n.predecessors[n.count++]=from;
    }
    std::shared_ptr<const Graph> freeze() const {
        auto g=std::make_shared<Graph>(); g->nodes=nodes_; g->offsets.resize(nodes_.size()+1);
        std::vector<std::uint32_t> pending;
        for (const auto& n:nodes_) {
            pending.push_back(n.count);
            for (std::uint32_t j=0;j<n.count;++j) ++g->offsets[n.predecessors[j]+1];
        }
        for (std::size_t i=1;i<g->offsets.size();++i) g->offsets[i]+=g->offsets[i-1];
        g->successors.resize(g->offsets.back()); auto cursor=g->offsets;
        for (Id i=0;i<nodes_.size();++i) for (std::uint32_t j=0;j<nodes_[i].count;++j) g->successors[cursor[nodes_[i].predecessors[j]]++]=i;
        for (Id i=0;i<nodes_.size();++i) if (!pending[i]) g->order.push_back(i);
        for (std::size_t k=0;k<g->order.size();++k) {
            const auto i=g->order[k]; auto& n=g->nodes[i];
            if (n.cost > std::numeric_limits<std::uint64_t>::max()-g->work) throw std::overflow_error("work overflow");
            g->work+=n.cost;
            for (auto p=g->offsets[i];p<g->offsets[i+1];++p) {
                const auto s=g->successors[p]; g->nodes[s].earliest=std::max(g->nodes[s].earliest,n.earliest+n.cost);
                if (!--pending[s]) g->order.push_back(s);
            }
        }
        if (g->order.size()!=nodes_.size()) throw std::invalid_argument("cycle");
        for (auto it=g->order.rbegin();it!=g->order.rend();++it) {
            auto& n=g->nodes[*it]; n.rank=n.cost;
            for (auto p=g->offsets[*it];p<g->offsets[*it+1];++p) n.rank=std::max(n.rank,n.cost+g->nodes[g->successors[p]].rank);
            g->span=std::max(g->span,n.rank);
        }
        for (auto& n:g->nodes) n.critical=n.earliest+n.rank==g->span;
        return g;
    }
};
struct Run;
struct Task {
    Run* run{}; Id id{}; Task* next{}; Task* previous{};
    std::atomic<std::uint32_t> pending{};
    State state{State::waiting}; std::uint64_t output{};
    std::chrono::steady_clock::time_point ready{};
};
inline thread_local const void* executing_pool = nullptr;
struct Run : std::enable_shared_from_this<Run> {
    std::shared_ptr<const Graph> graph;
    std::unique_ptr<Task[]> tasks;
    std::atomic<std::size_t> remaining{};
    std::atomic<bool> done{false};
    std::chrono::steady_clock::time_point finished{};
    std::stop_source cancellation;
    const void* pool{};
    bool foreground{true};
    void cancel() noexcept { cancellation.request_stop(); }
    void wait() const {
        if (executing_pool==pool) throw std::logic_error("worker cannot wait on its own executor");
        while (!done.load(std::memory_order_acquire)) done.wait(false, std::memory_order_acquire);
    }
    State state(Id i) const { wait(); return tasks_at(i).state; }
    std::uint64_t result(Id i) const {
        wait(); const auto& t=tasks_at(i);
        if (t.state!=State::succeeded) throw std::logic_error("result unavailable");
        return t.output;
    }
private:
    const Task& tasks_at(Id i) const {
        if (i>=graph->nodes.size()) throw std::out_of_range("node"); return tasks[i];
    }
};
struct List {
    Task* first{}; Task* last{};
    void push(Task* t) noexcept { t->next=nullptr; t->previous=last; if(last) last->next=t; else first=t; last=t; }
    Task* pop() noexcept { auto* t=first; if(t) { first=t->next; if(!first) last=nullptr; else first->previous=nullptr; } return t; }
    Task* pop_back() noexcept { auto* t=last; if(t) { last=t->previous; if(!last) first=nullptr; else last->next=nullptr; } return t; }
};
struct Worker {
    std::mutex mutex, park_mutex;
    std::condition_variable cv;
    std::uint64_t epoch{};
    List inbox[2];
    Deque<Task> deque[2]; Deque<Task,4096,true> reference[2];
    unsigned urgent_streak{};
    std::uint64_t random{1}, attempts{}, steals{}, executed{}, continuations{};
    std::uint64_t busy_ns{}, queue_ns{};
    bool pinned{};
};
struct Options {
    unsigned workers{std::clamp(std::thread::hardware_concurrency(),1u,64u)};
    std::size_t capacity{1'000'000}; Arm arm{Arm::chase};
    bool work_first{true}, priority{false};
    bool numa{true};
};
class Executor {
    Options options_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    std::mutex control_, global_mutex_;
    std::condition_variable drained_;
    std::unordered_map<Run*,std::shared_ptr<Run>> active_;
    List global_[2]; std::size_t admitted_{}; bool closed_{};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> idle_{0};
    std::vector<unsigned> nodes_;
    bool multi_node_{};
    int lane(Task* t) const { return options_.priority && t->run->foreground && t->run->graph->nodes[t->id].critical ? 1 : 0; }
    void wake(unsigned preferred) {
        // Sequence registration/recheck against publisher observation. Timed
        // parking is also a 1 ms liveness backstop, not a latency guarantee.
        auto mask=idle_.load(std::memory_order_seq_cst);
        unsigned target=preferred;
        if(mask) { target=0; while (!(mask & (std::uint64_t{1}<<target))) ++target; }
        auto& w=*workers_[target];
        { std::lock_guard lock(w.park_mutex); ++w.epoch; }
        w.cv.notify_one();
    }
    void enqueue(Task* t,unsigned owner,bool local) {
        t->ready=std::chrono::steady_clock::now();
        const auto l=lane(t); auto& w=*workers_[owner];
        if(options_.arm==Arm::global) { std::lock_guard lock(global_mutex_); global_[l].push(t); }
        else if(local && options_.arm==Arm::chase && w.deque[l].push(t)) {}
        else if(local && options_.arm==Arm::sequential && w.reference[l].push(t)) {}
        else { std::lock_guard lock(w.mutex); w.inbox[l].push(t); }
        wake(owner);
    }
    Task* take_lane(unsigned me,unsigned victim,int l) {
        auto& w=*workers_[victim];
        if(options_.arm==Arm::global) { std::lock_guard lock(global_mutex_); return global_[l].pop(); }
        Task* t=nullptr;
        if(options_.arm==Arm::chase) t=me==victim ? w.deque[l].pop():w.deque[l].steal();
        if(options_.arm==Arm::sequential) t=me==victim ? w.reference[l].pop():w.reference[l].steal();
        if(t) return t;
        std::lock_guard lock(w.mutex); return me==victim ? w.inbox[l].pop_back():w.inbox[l].pop();
    }
    Task* find(unsigned me) {
        auto& w=*workers_[me];
        const int first=w.urgent_streak<8 ? 1:0;
        for(int k=0;k<2;++k) if(auto* t=take_lane(me,me,k ? 1-first:first)) {
            w.urgent_streak=lane(t) ? w.urgent_streak+1:0; return t;
        }
        // A full randomized-start scan bounds attempts and avoids a single
        // shared PRNG. Placement/NUMA is deliberately reported as unmeasured.
        w.random^=w.random<<13; w.random^=w.random>>7; w.random^=w.random<<17;
        const auto start=static_cast<unsigned>(w.random%workers_.size());
        for(unsigned pass=0;pass<(multi_node_?2u:1u);++pass)
        for(unsigned j=0;j<workers_.size();++j) {
            const auto v=(start+j)%static_cast<unsigned>(workers_.size()); if(v==me) continue;
            if(multi_node_ && ((nodes_[v]==nodes_[me]) != (pass==0)))continue;
            ++w.attempts;
            for(int k=0;k<2;++k) if(auto* t=take_lane(me,v,k ? 1-first:first)) { ++w.steals; w.urgent_streak=lane(t)?w.urgent_streak+1:0; return t; }
        }
        return nullptr;
    }
    Task* execute(Task* task,unsigned me,unsigned chain) {
        const auto began=std::chrono::steady_clock::now();
        workers_[me]->queue_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(began-task->ready).count());
        auto keep=task->run->shared_from_this(); auto& r=*keep;
        const auto& g=*r.graph; const auto& n=g.nodes[task->id];
        bool valid=true; std::array<std::uint64_t,8> input{};
        for(unsigned j=0;j<n.count;++j) {
            const auto& p=r.tasks[n.predecessors[j]];
            if(p.state!=State::succeeded) valid=false; else input[j]=p.output;
        }
        if(!valid) task->state=State::skipped;
        else if(r.cancellation.stop_requested()) task->state=State::canceled;
        else {
            task->state=State::running;
            try { task->output=n.function({input.data(),n.count},r.cancellation.get_token(),n.context); task->state=State::succeeded; }
            catch(...) { task->state=State::failed; }
        }
        ++workers_[me]->executed;
        Task* continuation=nullptr;
        for(auto p=g.offsets[task->id];p<g.offsets[task->id+1];++p) {
            auto* next=&r.tasks[g.successors[p]];
            // Each writer publishes its output/state. RMW modification order
            // chains all predecessor releases to the final acquire.
            if(next->pending.fetch_sub(1,std::memory_order_acq_rel)==1) {
                next->ready=std::chrono::steady_clock::now();
                if(options_.work_first && chain<31 && (!options_.priority || workers_[me]->urgent_streak<8)) {
                    if(!continuation) continuation=next;
                    else if(g.nodes[next->id].rank>g.nodes[continuation->id].rank) { enqueue(continuation,me,true); continuation=next; }
                    else enqueue(next,me,true);
                } else enqueue(next,me,true);
            }
        }
        workers_[me]->busy_ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count());
        if(r.remaining.fetch_sub(1,std::memory_order_acq_rel)==1) {
            { std::lock_guard lock(control_); admitted_-=g.nodes.size(); active_.erase(&r); }
            r.finished=std::chrono::steady_clock::now();
            r.done.store(true,std::memory_order_release); r.done.notify_all(); drained_.notify_all();
        }
        if(continuation) { ++workers_[me]->continuations; if(lane(continuation)) ++workers_[me]->urgent_streak; else workers_[me]->urgent_streak=0; }
        return continuation;
    }
    void loop(unsigned me) {
        executing_pool=this; auto& w=*workers_[me]; w.random=me+1;
        if(multi_node_) w.pinned=pin_worker(me);
        while(!stop_.load(std::memory_order_acquire)) {
            Task* t=find(me);
            if(t) { unsigned chain=0; do { t=execute(t,me,chain++); } while(t); continue; }
            std::unique_lock lock(w.park_mutex); const auto epoch=w.epoch;
            idle_.fetch_or(std::uint64_t{1}<<me,std::memory_order_seq_cst);
            lock.unlock(); t=find(me); lock.lock();
            if(!t) w.cv.wait_for(lock,std::chrono::milliseconds(1),[&]{return w.epoch!=epoch || stop_.load();});
            idle_.fetch_and(~(std::uint64_t{1}<<me),std::memory_order_seq_cst); lock.unlock();
            if(t) { unsigned chain=0; do { t=execute(t,me,chain++); } while(t); }
        }
        executing_pool=nullptr;
    }
public:
    explicit Executor(Options o={}) : options_(o) {
        if(!o.workers || o.workers>64 || !o.capacity) throw std::invalid_argument("workers must be 1..64; capacity positive");
        nodes_=numa_nodes(o.workers);
        multi_node_=o.numa && std::any_of(nodes_.begin(),nodes_.end(),[&](unsigned n){return n!=nodes_[0];});
        for(unsigned i=0;i<o.workers;++i) workers_.push_back(std::make_unique<Worker>());
        try { for(unsigned i=0;i<o.workers;++i) threads_.emplace_back([this,i]{loop(i);}); }
        catch(...) { stop_=true; for(auto& w:workers_) w->cv.notify_all(); for(auto& t:threads_) t.join(); throw; }
    }
    ~Executor() { shutdown(); }
    Executor(const Executor&)=delete; Executor& operator=(const Executor&)=delete;
    std::shared_ptr<Run> submit(std::shared_ptr<const Graph> g,bool foreground=true) {
        if(!g) throw std::invalid_argument("null graph");
        auto r=std::make_shared<Run>(); r->pool=this; r->graph=std::move(g); r->foreground=foreground;
        const auto size=r->graph->nodes.size();
        r->tasks=std::make_unique<Task[]>(size); r->remaining=size;
        for(Id i=0;i<size;++i) { r->tasks[i].run=r.get(); r->tasks[i].id=i; r->tasks[i].pending=r->graph->nodes[i].count; }
        // Control-path serialization prevents shutdown overtaking publication;
        // workers never hold this mutex while manipulating a ready queue.
        std::lock_guard lock(control_);
        if(closed_ || size>options_.capacity-admitted_) return {};
        if(!size) { r->finished=std::chrono::steady_clock::now(); r->done=true; return r; }
        active_.emplace(r.get(),r); admitted_+=size;
        for(Id i=0;i<size;++i) if(!r->graph->nodes[i].count) enqueue(&r->tasks[i],i%options_.workers,false);
        return r;
    }
    void shutdown() {
        if(executing_pool==this) std::terminate(); // destroying a pool inside its callback is invalid
        { std::unique_lock lock(control_); closed_=true; drained_.wait(lock,[&]{return active_.empty();}); }
        stop_.store(true,std::memory_order_release);
        for(auto& w:workers_) { {std::lock_guard lock(w->park_mutex); ++w->epoch;} w->cv.notify_all(); }
        for(auto& t:threads_) if(t.joinable()) t.join();
    }
    // Read only after shutdown: worker-local counters avoid hot global RMWs.
    std::array<std::uint64_t,6> counters() const {
        std::array<std::uint64_t,6> c{}; for(const auto& w:workers_) {c[0]+=w->executed;c[1]+=w->attempts;c[2]+=w->steals;c[3]+=w->continuations;c[4]+=w->busy_ns;c[5]+=w->queue_ns;} return c;
    }
    bool placement_verified() const { return multi_node_ && std::all_of(workers_.begin(),workers_.end(),[](const auto& w){return w->pinned;}); }
};
}
