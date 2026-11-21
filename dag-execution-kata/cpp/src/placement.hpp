#pragma once
#include <algorithm>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
namespace dag {
// This teaching implementation supports Windows processor group zero (<=64
// workers). Unknown topology, Linux, and single-node hosts use random victims.
inline std::vector<unsigned> numa_nodes(unsigned workers) {
    std::vector<unsigned> nodes(workers,0);
#ifdef _WIN32
    const auto cpus=std::max(1u,std::thread::hardware_concurrency());
    for(unsigned i=0;i<workers;++i) {
        PROCESSOR_NUMBER cpu{}; cpu.Number=static_cast<BYTE>(i%cpus);
        USHORT node=0; if(GetNumaProcessorNodeEx(&cpu,&node) && node!=0xffff) nodes[i]=node;
    }
#endif
    return nodes;
}
inline bool pin_worker(unsigned worker) {
#ifdef _WIN32
    const auto cpu=worker%std::max(1u,std::thread::hardware_concurrency());
    if(cpu>=sizeof(DWORD_PTR)*8)return false;
    return SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR{1}<<cpu)!=0;
#else
    (void)worker; return false;
#endif
}
}
