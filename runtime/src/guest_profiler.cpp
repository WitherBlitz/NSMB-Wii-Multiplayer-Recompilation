// NSMBW_PROFILE=<start s>:<length s>: a sampling profiler for the game thread. From <start> seconds
// after the first presented frame, for <length> seconds, the thread that presents frames is sampled
// about a thousand times a second: its instruction pointer and call stack are recorded (Windows:
// suspended and unwound; Android and Linux: interrupted by SIGPROF, which walks the frame-pointer
// chain). At the end, samples are mapped back to the translated guest function whose code they fall
// in (by each translated function's host entry point) and the top entries are logged with their
// guest address, so a slow scene can be traced to the game code (or the runtime and aurora code) it
// spends time in. On Android set it with `adb shell setprop debug.nsmbw.env NSMBW_PROFILE=20:20`.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <atomic>
#include <dlfcn.h>
#include <link.h>
#include <csignal>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

std::filesystem::path RuntimeRunLogDirectory();  // main.cpp

namespace GuestProfiler {
namespace {

#if defined(_WIN32) || defined(__linux__)
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}
#endif

// The last present, for NSMBW_PROFILE_HITCH.
std::atomic<int64_t> g_lastPresentNanos{0};
std::atomic<uint32_t> g_presentedFrames{0};

int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// NSMBW_PROFILE_HITCH=<ms>: the threshold (in ns) past which no present means a hitch, else 0.
int64_t HitchNanos() {
    static const int64_t nanos = [] {
        const char* value = std::getenv("NSMBW_PROFILE_HITCH");
        return value != nullptr ? static_cast<int64_t>(std::atof(value) * 1e6) : int64_t{0};
    }();
    return nanos;
}

bool InHitch() {
    const int64_t last = g_lastPresentNanos.load(std::memory_order_relaxed);
    return last != 0 && NowNanos() - last >= HitchNanos();
}

#ifdef _WIN32
void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    // Every translated function's host entry point, sorted, to find which one a sample falls in.
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80340000u}, std::pair{0x807684C0u, 0x80B8E340u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew);
    const uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
    const auto describe = [&](uintptr_t rip) -> std::string {
        if (rip >= exeBase && rip < exeEnd) {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            // Translated code sits in its own range: past the last entry point it is runtime code.
            if (it != entries.begin() && it != entries.end()) {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - exeBase));
            return text;
        }
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module)) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        const char* slash = std::strrchr(path, '\\');
        return std::string("[") + (slash ? slash + 1 : path) + "]";
    };
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module)) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            key = std::string("[") + (slash ? slash + 1 : path) + "]";
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
    // Every sample inside the exe by its RVA, for naming runtime and aurora code offline (llvm-nm):
    // the table above files them under the nearest translated function.
    std::vector<std::pair<uintptr_t, uint32_t>> rvas;
    for (const auto& [rip, count] : samples) {
        if (rip >= exeBase && rip < exeEnd) {
            rvas.emplace_back(rip - exeBase, count);
        }
    }
    std::sort(rvas.begin(), rvas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < rvas.size() && i < 1500; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile rva: %llX %u\n", static_cast<unsigned long long>(rvas[i].first),
                rvas[i].second);
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    // NSMBW_PROFILE_HITCH=<ms>: keep only the samples taken while no frame has been presented for that
    // long, so the report shows what the game thread does during a hitch rather than on average.
    const int64_t hitchNanos = HitchNanos();
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    // The sampler's own late wake-ups: when this thread misses its 1 ms tick by a lot too, the whole
    // process was off the CPU (another process, paging), not just the game thread.
    uint32_t lateTicks = 0;
    double lateMs = 0, worstLateMs = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        const auto woke = std::chrono::steady_clock::now();
        const double late = std::chrono::duration<double, std::milli>(woke - next).count();
        if (late > 5.0) {
            ++lateTicks;
            lateMs += late;
            worstLateMs = std::max(worstLateMs, late);
            next = woke;
        }
        if (hitchNanos > 0 && !InHitch()) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                ++samples[static_cast<uintptr_t>(context.Rip)];
                ++total;
                // The call stack, by the unwind data every x64 function carries.
                std::vector<uintptr_t> stack;
                for (int depth = 0; depth < 24 && context.Rip != 0; ++depth) {
                    stack.push_back(static_cast<uintptr_t>(context.Rip));
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                        context.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
                stacks.push_back(std::move(stack));
            }
            ResumeThread(thread);
        }
    }
    RT_LOGF(RT_TAG_RUNTIME, "profile: sampler woke over 5 ms late %u times (%.0f ms in all, worst %.1f ms)%s\n",
            lateTicks, lateMs, worstLateMs, hitchNanos > 0 ? "; only samples during hitches kept" : "");
    if (total > 0) {
        Report(samples, stacks, total);
    }
}

#endif

#if defined(__linux__)
// Every translated function's host entry point, sorted.
std::vector<Entry> TranslatedEntries() {
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80340000u}, std::pair{0x807684C0u, 0x80B8E340u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    return entries;
}

// A sample: the interrupted pc, then return addresses outward.
constexpr int kDepth = 32;
struct Sample {
    uintptr_t frames[kDepth];
    int depth;
    unsigned scene;
    uint32_t frame;  // frames presented so far, to group samples by frame (profile-frames.txt)
};
Sample* g_samples = nullptr;
size_t g_capacity = 0;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sampling{false};
std::atomic<unsigned> g_scene{0xFFFFu};
std::string g_stacksName = "profile-stacks.txt";

// Reads a word of this process's memory without faulting on a bad frame pointer.
bool ReadWord(uintptr_t address, uintptr_t* out) {
    iovec local{out, sizeof(*out)};
    iovec remote{reinterpret_cast<void*>(address), sizeof(*out)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == static_cast<long>(sizeof(*out));
}

void OnSample(int, siginfo_t*, void* context) {
    if (!g_sampling.load(std::memory_order_relaxed)) {
        return;
    }
    const size_t index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= g_capacity) {
        return;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
    Sample& sample = g_samples[index];
    sample.scene = g_scene.load(std::memory_order_relaxed);
    sample.frame = g_presentedFrames.load(std::memory_order_relaxed);
#if defined(__aarch64__)
    uintptr_t fp = uc->uc_mcontext.regs[29];
    sample.frames[0] = uc->uc_mcontext.pc;
    // The interrupted function may not have stored its return address yet (a leaf, or its
    // prologue): x30 holds it. Where it has, the frame walk repeats it, and the report drops the
    // repeat.
    sample.frames[1] = uc->uc_mcontext.regs[30];
    int depth = 2;
#elif defined(__x86_64__)
    uintptr_t fp = uc->uc_mcontext.gregs[REG_RBP];
    sample.frames[0] = uc->uc_mcontext.gregs[REG_RIP];
    int depth = 1;
#else
    uintptr_t fp = 0;
    sample.frames[0] = 0;
    int depth = 1;
#endif
    // Frame records: [fp] = the caller's fp, [fp + 8] = the return address. Each record sits above
    // the last on the same stack.
    while (depth < kDepth && fp != 0 && (fp & 7) == 0) {
        uintptr_t next = 0, ret = 0;
        if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
            break;
        }
        sample.frames[depth++] = ret;
        if (next <= fp || next - fp > (1u << 20)) {
            break;
        }
        fp = next;
    }
    sample.depth = depth;
}

void ReportLinux(size_t count) {
    // This library's extent (dladdr is a linear search in bionic: far too slow per frame).
    Dl_info self{};
    dladdr(reinterpret_cast<const void*>(&ReportLinux), &self);
    const auto selfBase = reinterpret_cast<uintptr_t>(self.dli_fbase);
    const char* selfSlash = self.dli_fname ? std::strrchr(self.dli_fname, '/') : nullptr;
    const std::string selfName = selfSlash ? selfSlash + 1 : (self.dli_fname ? self.dli_fname : "?");
    struct Extent {
        uintptr_t base, end;
    } extent{selfBase, selfBase};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* e = static_cast<Extent*>(data);
            if (info->dlpi_addr != e->base) {
                return 0;
            }
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                    e->end = std::max<uintptr_t>(e->end, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr +
                                                             info->dlpi_phdr[i].p_memsz);
                }
            }
            return 1;
        },
        &extent);
    const auto inSelf = [&](uintptr_t address) { return address >= extent.base && address < extent.end; };
    // Other libraries' frames, as module+offset, looked up once each.
    std::unordered_map<uintptr_t, std::string> foreign;
    const auto foreignFrame = [&](uintptr_t address) -> const std::string& {
        auto [it, inserted] = foreign.try_emplace(address);
        if (inserted) {
            Dl_info info{};
            const char* module = "?";
            uintptr_t base = 0;
            if (dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_fname != nullptr) {
                const char* slash = std::strrchr(info.dli_fname, '/');
                module = slash ? slash + 1 : info.dli_fname;
                base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            }
            char text[160];
            std::snprintf(text, sizeof(text), "%s+%llX", module, static_cast<unsigned long long>(address - base));
            it->second = text;
        }
        return it->second;
    };

    // The stacks, for exact symbolizing offline (tools/profile-report.py, against the unstripped
    // library).
    const std::filesystem::path stacksPath = ::RuntimeRunLogDirectory() / g_stacksName;
    if (FILE* stacks = std::fopen(stacksPath.string().c_str(), "w")) {
        std::fprintf(stacks,
                     "# %s base %llX; one sample a line: S<scene>, then frames innermost first, as offsets in it "
                     "or module+offset\n",
                     selfName.c_str(), static_cast<unsigned long long>(selfBase));
        for (size_t i = 0; i < count; ++i) {
            const Sample& sample = g_samples[i];
            std::fprintf(stacks, "S%03X", sample.scene);
            for (int d = 0; d < sample.depth; ++d) {
                const uintptr_t address = sample.frames[d];
                if (inSelf(address)) {
                    std::fprintf(stacks, " %llX", static_cast<unsigned long long>(address - selfBase));
                } else {
                    std::fprintf(stacks, " %s", foreignFrame(address).c_str());
                }
            }
            std::fputc('\n', stacks);
        }
        std::fclose(stacks);
        RT_LOGF(RT_TAG_RUNTIME, "profile: %zu samples; stacks written to %s\n", count, stacksPath.string().c_str());
        // The frame each sample fell in, one a line in the same order (a sample is about 1 ms).
        std::filesystem::path framesPath = stacksPath;
        framesPath.replace_filename("profile-frames.txt");
        if (FILE* frames = std::fopen(framesPath.string().c_str(), "w")) {
            for (size_t i = 0; i < count; ++i) std::fprintf(frames, "%u\n", g_samples[i].frame);
            std::fclose(frames);
        }
    }

    // A rough self-time table on the device: by the translated function a sample stopped in (the
    // nearest entry point at or below it, so runtime code between translated functions lands on its
    // neighbour; the offline report has it exact).
    const std::vector<Entry> entries = TranslatedEntries();
    std::unordered_map<std::string, uint32_t> byFunction;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t pc = g_samples[i].frames[0];
        std::string key;
        auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                                   [](uintptr_t value, const Entry& e) { return value < e.host; });
        if (inSelf(pc) && it != entries.begin() && it != entries.end()) {
            --it;
            char text[200];
            std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
            key = text;
        } else {
            key = inSelf(pc) ? selfName : foreignFrame(pc).substr(0, foreignFrame(pc).find('+'));
        }
        byFunction[key]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byFunction.begin(), byFunction.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / count, sorted[i].first.c_str());
    }
}

// NSMBW_PROFILE_THREAD=<name>: sample the thread of that name (as /proc lists it, e.g. FrameWorker)
// instead of the game thread.
pid_t ThreadNamed(const char* name) {
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        if (FILE* comm = std::fopen((task.path() / "comm").string().c_str(), "r")) {
            char text[64] = {};
            const bool read = std::fgets(text, sizeof(text), comm) != nullptr;
            std::fclose(comm);
            if (read) {
                text[std::strcspn(text, "\n")] = 0;
                if (std::strcmp(text, name) == 0) {
                    return static_cast<pid_t>(std::atoi(task.path().filename().string().c_str()));
                }
            }
        }
    }
    return 0;
}

void RunLinux(pid_t thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    if (const char* name = std::getenv("NSMBW_PROFILE_THREAD"); name != nullptr && *name != 0) {
        thread = ThreadNamed(name);
        RT_LOGF(RT_TAG_RUNTIME, "profile: sampling thread %s (%d)\n", name, static_cast<int>(thread));
        if (thread == 0) {
            return;
        }
        g_stacksName = std::string("profile-stacks-") + name + ".txt";
    }
    g_capacity = static_cast<size_t>(lengthSeconds * 1100) + 1000;
    g_samples = new Sample[g_capacity];
    struct sigaction action {};
    action.sa_sigaction = OnSample;
    action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    g_sampling.store(true);
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (next < end) {
        next += std::chrono::microseconds(1000);
        std::this_thread::sleep_until(next);
        if (HitchNanos() > 0 && !InHitch()) {
            continue;  // NSMBW_PROFILE_HITCH: only while no frame has been presented for that long
        }
        syscall(SYS_tgkill, getpid(), thread, SIGPROF);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_sampling.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ReportLinux(std::min(g_count.load(), g_capacity));
}
#endif

} // namespace

void NoteScene(unsigned scene) {
#if defined(__linux__)
    g_scene.store(scene, std::memory_order_relaxed);
#else
    (void)scene;
#endif
}

unsigned ProcessPageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PageFaultCount : 0;
#else
    return 0;
#endif
}

void ThreadCpuMs(double* user, double* kernel) {
    *user = *kernel = 0;
#ifdef _WIN32
    FILETIME created, exited, kernelTime, userTime;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernelTime, &userTime)) {
        const auto ms = [](const FILETIME& t) {
            return ((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e4;
        };
        *user = ms(userTime);
        *kernel = ms(kernelTime);
    }
#endif
}

void OnFramePresented() {
#if defined(_WIN32) || defined(__linux__)
    g_lastPresentNanos.store(NowNanos(), std::memory_order_relaxed);
    g_presentedFrames.fetch_add(1, std::memory_order_relaxed);
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* value = std::getenv("NSMBW_PROFILE");
    double start = 0, length = 0;
    if (value == nullptr || std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
#ifdef _WIN32
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread(Run, thread, start, length).detach();
#else
    // The handler runs on whatever stack the game thread is on, a guest thread's fiber stack among
    // them: give it one of its own.
    static std::vector<char> signalStack(64 * 1024);
    stack_t alternate{};
    alternate.ss_sp = signalStack.data();
    alternate.ss_size = signalStack.size();
    sigaltstack(&alternate, nullptr);
    RT_LOGF(RT_TAG_RUNTIME, "profile: sampling the game thread from %.0f s for %.0f s\n", start, length);
    std::thread(RunLinux, static_cast<pid_t>(syscall(SYS_gettid)), start, length).detach();
#endif
#endif
}

} // namespace GuestProfiler
