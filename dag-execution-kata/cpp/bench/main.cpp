#include "dag.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <string>
using Clock=std::chrono::steady_clock;
using namespace dag;
struct Work { unsigned iterations{}; };
std::uint64_t task(std::span<const std::uint64_t> input,std::stop_token,void* p) {
    std::uint64_t x=1;for(auto v:input)x+=v;
    for(unsigned i=0;i<static_cast<Work*>(p)->iterations;++i)x=(x^(x>>13))*0x9e3779b97f4a7c15ULL+i;
    return x;
}
std::shared_ptr<const Graph> graph(const std::string& shape,Work* work) {
    Builder b;std::mt19937 rng(33);
    for(Id i=0;i<128;++i) {
        b.add(task,work);
        if(shape=="chain" && i)b.edge(i-1,i);
        else if(shape=="wide" && i)b.edge(0,i);
        else if(shape=="fanin" && i>=8)for(Id j=0;j<8;++j)b.edge(i-8+j,i);
        else if(shape=="irregular" && i){std::vector<Id> parents;for(int j=0;j<3;++j){Id p=rng()%i;if(std::find(parents.begin(),parents.end(),p)==parents.end()){parents.push_back(p);b.edge(p,i);}}}
        else if(shape=="mixed" && i>0 && i<32)b.edge(i-1,i);
    }return b.freeze();
}
// Intentionally incorrect litmus, isolated from default execution. No second
// producer handshake publishes A's result to B. Join is after C reads.
int relaxed_bug() {
    for(int round=0;round<1000;++round) {
        int a=0,b=0;std::atomic<unsigned> pending{2};std::atomic<bool> queued=false;
        std::thread x([&]{a=42;if(pending.fetch_sub(1,std::memory_order_relaxed)==1)queued.store(true,std::memory_order_release);});
        std::thread y([&]{b=84;if(pending.fetch_sub(1,std::memory_order_relaxed)==1)queued.store(true,std::memory_order_release);});
        while(!queued.load(std::memory_order_acquire))std::this_thread::yield();
        const auto result=a+b; x.join();y.join();if(result!=126)return 1;
    }std::cout<<"not detected; this is not evidence of correctness\n";return 0;
}
int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);
    bool full=false;unsigned samples=1000,reps=3;double rate=2000;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--bug" && i+1<argc && std::string(argv[++i])=="relaxed-deps")return relaxed_bug();
        else if(arg=="--full"){full=true;samples=10000;reps=5;}
        else if(arg=="--samples" && i+1<argc)samples=static_cast<unsigned>(std::stoul(argv[++i]));
        else if(arg=="--rate" && i+1<argc)rate=std::stod(argv[++i]);
        else {std::cerr<<"usage: solution [--full] [--samples N] [--rate events/s] | --bug relaxed-deps\n";return 2;}}
    if(!samples || !(rate>0) || rate>1e9)return 2;
    std::cerr<<"logical_cpus="<<std::thread::hardware_concurrency()<<" seed=33 clock=steady_clock nodes/event=128 affinity=uncontrolled\n";
    std::cout<<"shape,work_ns,arm,workers,policy,rep,offered,accepted,rejected,p50_us,p99_us,events_s,tasks,steal_attempts,steals,inline,work_units,span_units,busy_ns,queue_ns,numa_pinned,metadata_bytes,foreground_p99_us,background_p99_us\n";
    for(unsigned target:full?std::vector<unsigned>{450,1000}:std::vector<unsigned>{450}) {
      Work work{1}; // Calibrate a deterministic integer kernel, never a timed spin per node.
      for(int pass=0;pass<3;++pass){auto begin=Clock::now();std::uint64_t checksum=0;for(std::uint64_t k=0;k<10000;++k)checksum+=task({&k,1}, {}, &work);
        const auto ns=std::chrono::duration<double,std::nano>(Clock::now()-begin).count()/10000;
        if(!checksum)std::cerr<<"checksum zero\n";
        work.iterations=std::max(1u,static_cast<unsigned>(work.iterations*target/std::max(1.0,ns)));}
      for(const auto& shape:full?std::vector<std::string>{"chain","wide","fanin","irregular","mixed"}:std::vector<std::string>{"irregular"}) {
       auto g=graph(shape,&work);std::vector<std::uint64_t> oracle(g->nodes.size());
       for(auto id:g->order){std::array<std::uint64_t,8> in{};auto& n=g->nodes[id];for(unsigned j=0;j<n.count;++j)in[j]=oracle[n.predecessors[j]];oracle[id]=task({in.data(),n.count},{},&work);}
       for(unsigned workers:full?std::vector<unsigned>{1,4,16,32,64}:std::vector<unsigned>{4})
       for(unsigned policy:full?std::vector<unsigned>{0,1}:std::vector<unsigned>{1})
       for(unsigned rep=0;rep<reps;++rep)
       for(unsigned index=0;index<3;++index) {
        unsigned arm=rep%2 ? 2-index:index;
        Options o;o.workers=workers;o.arm=static_cast<Arm>(arm);o.work_first=policy!=0;o.priority=policy!=0;Executor ex(o);
        auto validate=[&](const std::shared_ptr<Run>& r){r->wait();for(Id id=0;id<oracle.size();++id)if(r->result(id)!=oracle[id])throw std::runtime_error("benchmark oracle failed");};
        auto warm=Clock::now()+std::chrono::seconds(1);do{validate(ex.submit(g));}while(Clock::now()<warm);
        struct Sample{Clock::time_point arrival;std::shared_ptr<Run> run;};std::vector<Sample> runs;runs.reserve(samples);
        // Completion timestamps must be collected independently of the arrival
        // loop: Run publishes its own terminal timestamp before done.
        auto begin=Clock::now();
        for(unsigned i=0;i<samples;++i){auto arrival=begin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(i/rate));
          std::this_thread::sleep_until(arrival);if(auto r=ex.submit(g,shape!="mixed" || i%4==0))runs.push_back({arrival,std::move(r)});}
        std::vector<double> latency,foreground,background;for(auto& sample:runs){validate(sample.run);auto us=std::chrono::duration<double,std::micro>(sample.run->finished-sample.arrival).count();latency.push_back(us);(sample.run->foreground?foreground:background).push_back(us);}
        const double seconds=std::chrono::duration<double>(Clock::now()-begin).count();ex.shutdown();auto c=ex.counters();std::sort(latency.begin(),latency.end());
        auto percentile=[&](double p){return latency.empty()?0.0:latency[std::min(latency.size()-1,static_cast<std::size_t>(p*latency.size()))];};
        auto tail=[](std::vector<double>& v){if(v.empty())return 0.0;std::sort(v.begin(),v.end());return v[std::min(v.size()-1,static_cast<std::size_t>(.99*v.size()))];};
        const auto metadata=workers*sizeof(Worker)+g->nodes.size()*sizeof(Node)+g->successors.size()*sizeof(Id)+g->offsets.size()*sizeof(std::size_t)+runs.size()*(sizeof(Run)+g->nodes.size()*sizeof(Task));
        std::cout<<shape<<','<<target<<','<<arm<<','<<workers<<','<<policy<<','<<rep<<','<<samples<<','<<runs.size()<<','<<samples-runs.size()<<','<<percentile(.5)<<','<<percentile(.99)<<','<<runs.size()/seconds<<','<<c[0]<<','<<c[1]<<','<<c[2]<<','<<c[3]<<','<<g->work<<','<<g->span<<','<<c[4]<<','<<c[5]<<','<<ex.placement_verified()<<','<<metadata<<','<<tail(foreground)<<','<<tail(background)<<'\n';
       }
      }
    }
}
