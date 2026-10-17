// linux_sampler.h - opt-in in-process sampling profiler for Linux builds.
//
// FABLE2_PROFILE=1 starts it. Every thread of the process gets a CPU-time
// timer (CLOCK_THREAD_CPUTIME_ID-style per-tid clock, SIGEV_THREAD_ID), so a
// thread is only sampled while it is actually running (blocked threads are
// never interrupted, which keeps syscalls free of EINTR surprises). The
// SIGPROF handler records the interrupted PC into a lock-free ring. Every
// 10 s the samples of the last 60 s are aggregated per (thread name, module, offset) and
// written to profile.txt in the working directory (the game folder):
//   <count> <thread> <module> 0x<offset>
// Symbolize offline (tools/symbolize_profile.py).
//
// Linux-only diagnostics; not compiled elsewhere.

#pragma once

#if defined(__linux__)

#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace fable2::sampler {

struct Sample {
  uint64_t pc;
  int32_t tid;
};
constexpr size_t kRing = size_t(1) << 20;
inline Sample* g_ring = nullptr;
inline std::atomic<uint64_t> g_head{0};

inline void OnSignal(int, siginfo_t*, void* uctx) {
  auto* uc = static_cast<ucontext_t*>(uctx);
  const uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
  Sample& s = g_ring[i & (kRing - 1)];
  s.pc = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
  s.tid = int32_t(syscall(SYS_gettid));
}

// Linux per-thread CPU clock id (same encoding glibc uses for
// pthread_getcpuclockid): CPUCLOCK_SCHED | CPUCLOCK_PERTHREAD_MASK.
inline clockid_t ThreadClock(pid_t tid) {
  return clockid_t((~unsigned(tid) << 3) | 6);
}

inline std::string ThreadName(pid_t tid) {
  std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string n;
  std::getline(f, n);
  for (char& c : n)
    if (c == ' ') c = '_';
  return n.empty() ? "?" : n;
}

inline void Dump(const std::unordered_map<int32_t, std::string>& names, uint64_t from,
                 uint64_t to) {
  std::map<std::tuple<std::string, std::string, uint64_t>, uint64_t> counts;
  std::unordered_map<uint64_t, std::pair<std::string, uint64_t>> module_cache;
  for (uint64_t i = from; i < to; ++i) {
    const Sample s = g_ring[i & (kRing - 1)];
    auto& mod = module_cache[s.pc];
    if (mod.first.empty()) {
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(s.pc), &info) && info.dli_fname) {
        mod.first = std::filesystem::path(info.dli_fname).filename().string();
        mod.second = s.pc - uint64_t(info.dli_fbase);
      } else {
        mod.first = "?";
        mod.second = s.pc;
      }
    }
    auto it = names.find(s.tid);
    counts[{it != names.end() ? it->second : std::to_string(s.tid), mod.first, mod.second}]++;
  }
  std::FILE* f = std::fopen("profile.txt", "w");
  if (!f) return;
  std::fprintf(f, "# samples %llu\n", (unsigned long long)(to - from));
  for (const auto& [key, n] : counts)
    std::fprintf(f, "%llu %s %s 0x%llx\n", (unsigned long long)n, std::get<0>(key).c_str(),
                 std::get<1>(key).c_str(), (unsigned long long)std::get<2>(key));
  std::fclose(f);
}

inline void Start() {
  const char* v = std::getenv("FABLE2_PROFILE");
  if (!v || v[0] != '1') return;
  g_ring = new Sample[kRing]();
  struct sigaction sa {};
  sa.sa_sigaction = OnSignal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, nullptr);
  std::thread([] {
    std::set<pid_t> armed;
    std::unordered_map<int32_t, std::string> names;
    std::vector<uint64_t> marks{g_head.load()};  // head every 10 s
    auto last_dump = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        const pid_t tid = pid_t(std::atoi(e.path().filename().c_str()));
        names[tid] = ThreadName(tid);  // names change after creation
        if (!armed.insert(tid).second) continue;
        sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = tid;
        timer_t timer;
        if (timer_create(ThreadClock(tid), &sev, &timer) == 0) {
          itimerspec its{};
          its.it_interval.tv_nsec = 2'000'000;  // every 2 ms of thread CPU time
          its.it_value.tv_nsec = 2'000'000;
          timer_settime(timer, 0, &its, nullptr);
        }
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last_dump >= std::chrono::seconds(10)) {
        last_dump = now;
        // Window = the last 60 s, so menus/loading do not skew a gameplay profile.
        const uint64_t head = g_head.load();
        marks.push_back(head);
        uint64_t from = marks.size() > 6 ? marks[marks.size() - 7] : marks.front();
        if (head - from > kRing) from = head - kRing;
        Dump(names, from, head);
      }
    }
  }).detach();
}

}  // namespace fable2::sampler

#endif  // __linux__
