#include "dag.hpp"
#include <iostream>
#include <random>
#include <future>

using namespace dag;
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
std::uint64_t sum(std::span<const std::uint64_t> in,std::stop_token,void*) { std::uint64_t v=1; for(auto x:in) v+=x; return v; }
std::uint64_t fail(std::span<const std::uint64_t>,std::stop_token,void*) { throw std::runtime_error("failure"); }
std::uint64_t count(std::span<const std::uint64_t>,std::stop_token,void* p) { ++*static_cast<std::atomic<unsigned>*>(p);return 1; }
std::uint64_t forbidden_wait(std::span<const std::uint64_t>,std::stop_token,void* p) { static_cast<Run*>(p)->wait();return 1; }
std::uint64_t block(std::span<const std::uint64_t>,std::stop_token stop,void* p) {
    auto& started=*static_cast<std::atomic<bool>*>(p); started=true; started.notify_all();
    while(!stop.stop_requested()) std::this_thread::yield(); return 1;
}
template<class F> void invalid(F fn) { bool caught=false; try { fn(); } catch(const std::invalid_argument&) {caught=true;} require(caught,"expected invalid_argument"); }
template<bool SC> void deque_checks() {
    int values[30000]{}; Deque<int,8,SC> q;
    for(int i=0;i<100;++i) require(!q.pop(),"empty pop");
    for(int i=0;i<8;++i) require(q.push(values+i),"push capacity");
    require(!q.push(values),"full must refuse");
    for(int i=7;i>=0;--i) require(q.pop()==values+i,"owner LIFO");
    std::atomic<bool> done=false; std::atomic<unsigned> seen[30000]{};
    std::thread thief([&]{while(!done.load() ) if(auto* p=q.steal()) ++seen[p-values]; while(auto* p=q.steal()) ++seen[p-values];});
    for(int i=0;i<30000;++i) { while(!q.push(values+i)) if(auto* p=q.pop()) ++seen[p-values]; if(i%2==0) if(auto* p=q.pop()) ++seen[p-values]; }
    while(auto* p=q.pop()) ++seen[p-values]; done=true; thief.join();
    for(auto& n:seen) require(n==1,"deque exactly once");
    Deque<int,8,SC> limit(std::numeric_limits<std::int64_t>::max()-2);
    require(limit.push(values),"near limit push"); require(!limit.push(values+1),"overflow routes to inbox");
    require(limit.pop()==values,"near limit pop"); require(!limit.pop(),"near limit empty");
}
void oracle(const std::shared_ptr<const Graph>& g,Executor& ex) {
    std::vector<std::uint64_t> expected(g->nodes.size());
    for(auto i:g->order) { expected[i]=1; const auto& n=g->nodes[i]; for(unsigned j=0;j<n.count;++j) expected[i]+=expected[n.predecessors[j]]; }
    auto r=ex.submit(g); require(bool(r),"admission"); r->wait();
    for(Id i=0;i<expected.size();++i) require(r->result(i)==expected[i],"serial oracle");
}
int main(int argc,char**) {
    try {
        deque_checks<false>(); deque_checks<true>();
        invalid([]{Builder b; b.add(sum); b.edge(0,1);});
        invalid([]{Builder b; b.add(sum); b.edge(0,0); b.freeze();});
        invalid([]{Builder b; b.add(sum); b.add(sum); b.edge(0,1); b.edge(0,1);});
        invalid([]{Builder b; for(int i=0;i<10;++i)b.add(sum); for(Id i=0;i<9;++i)b.edge(i,9);});
        for(auto arm:{Arm::global,Arm::sharded,Arm::chase,Arm::sequential}) {
            Options o; o.workers=4; o.arm=arm; o.priority=true; Executor ex(o);
            oracle(Builder{}.freeze(),ex);
            for(unsigned seed=1;seed<=20;++seed) {
                Builder b; std::mt19937 rng(seed);
                for(Id i=0;i<500;++i) {
                    b.add(sum); std::vector<Id> parents;
                    for(unsigned j=0;i && j<rng()%9;++j) { auto p=rng()%i; if(std::find(parents.begin(),parents.end(),p)==parents.end()) {parents.push_back(p);b.edge(p,i);} }
                }
                oracle(b.freeze(),ex);
            }
            Builder wide; wide.add(sum); for(Id i=1;i<15000;++i){wide.add(sum);wide.edge(0,i);} oracle(wide.freeze(),ex);
            std::atomic<unsigned> calls{0};Builder counted;
            for(Id i=0;i<20000;++i){counted.add(count,&calls);if(i)counted.edge((i-1)/2,i);}
            auto counted_run=ex.submit(counted.freeze());counted_run->wait();require(calls==20000,"callbacks exactly once");
            Builder chain; for(Id i=0;i<10000;++i){chain.add(sum);if(i)chain.edge(i-1,i);} oracle(chain.freeze(),ex);
            Builder diamond; for(int i=0;i<10;++i)diamond.add(sum); for(Id i=1;i<=8;++i){diamond.edge(0,i);diamond.edge(i,9);} oracle(diamond.freeze(),ex);
            Builder broken; broken.add(fail);broken.add(sum);broken.add(sum);broken.edge(0,1);
            auto r=ex.submit(broken.freeze());r->wait();require(r->state(0)==State::failed && r->state(1)==State::skipped && r->result(2)==1,"failure propagation");
            Builder concurrent; concurrent.add(sum); auto g=concurrent.freeze(); std::vector<std::thread> submitters;
            for(int p=0;p<4;++p)submitters.emplace_back([&]{for(int i=0;i<100;++i)oracle(g,ex);});
            for(auto& t:submitters)t.join();
            ex.shutdown(); require(!ex.submit(g),"closed rejects");
            require(ex.counters()[0]>0,"accounting");
        }
        std::weak_ptr<Run> weak;
        { Options o;o.workers=1;o.capacity=2;Executor ex(o);std::atomic<bool> started=false;
          Builder b;b.add(block,&started);b.add(sum);b.edge(0,1);auto g=b.freeze();auto r=ex.submit(g);weak=r;
          while(!started.load())started.wait(false);
          require(!ex.submit(g),"capacity rejects entire graph");r->cancel();r->wait();
          require(r->state(1)==State::canceled,"canceled successor");r.reset();ex.shutdown(); }
        require(weak.expired(),"run resources released");
        { Options o;o.workers=2;Executor ex(o);Builder b;b.add(sum);auto g=b.freeze();
          std::vector<std::shared_ptr<Run>> accepted;std::atomic<bool> entered=false;
          std::thread producer([&]{entered=true;entered.notify_all();for(int i=0;i<500;++i)if(auto r=ex.submit(g))accepted.push_back(std::move(r));});
          while(!entered.load())entered.wait(false);ex.shutdown();producer.join();
          for(auto& r:accepted)require(r->result(0)==1,"submission versus shutdown"); }
        { Options o;o.workers=1;Executor ex(o);std::atomic<bool> started=false;
          Builder b;b.add(block,&started);auto running=ex.submit(b.freeze());while(!started.load())started.wait(false);
          Builder waiter;waiter.add(forbidden_wait,running.get());auto waiting=ex.submit(waiter.freeze());
          std::thread closer([&]{ex.shutdown();});running->cancel();closer.join();
          require(waiting->state(0)==State::failed,"worker wait is rejected and shutdown drains"); }
        if(argc>1) {Options o;o.workers=64;Executor ex(o);Builder b;for(int i=0;i<1'000'000;++i)b.add(sum);oracle(b.freeze(),ex);}
        std::cout<<"all checks passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
