#pragma once

// Flat 4 GiB guest address space: a guest access is just `*(T*)(kFlatGuestBase + addr)` plus a
// byte swap, no page-table load/branch. Two views alias the same physical memory: the GUEST
// view at kFlatGuestBase has page protections as the interception mechanism (unmapped/MMIO/
// executable/deferred-read pages are uncommitted or protected), while the HOST view is a plain
// alias native runtime code (image loading, DVD reads, HLE, GX) writes through unchecked.
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace GuestFlat {

// Fixed base so the emitted access is `[reg + imm64-in-register]` with no load
// of a global.
inline constexpr uint64_t kGuestSpaceSize = 0x1'0000'0000ull;
inline constexpr size_t kGuestPageSize = 0x1000;
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(MKW_TEST_DYNAMIC_GUEST_BASE)
// iOS caps a process's virtual address space far below macOS's (tens of GiB, and only with the
// extended-virtual-addressing entitlement), and the shared cache and the app's own mappings sit at
// addresses that differ from launch to launch. No fixed address is reliably free, so the kernel
// chooses where the 4 GiB space goes and translated code reads the base from this pointer. It is
// set once, before any guest code runs, and never changes. MKW_TEST_DYNAMIC_GUEST_BASE builds the
// same layout on macOS so tests/ios_guest_flat_memory_tests.cpp can run on a Mac.
#define MKW_GUEST_FLAT_DYNAMIC_BASE 1
extern uint8_t* g_flatGuestBase;
#elif defined(__x86_64__)
// 16 TiB: clear of the Windows ASan shadow (32 TiB) and of the usual image/heap
// placement.
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'1000'0000'0000ull;
#elif defined(__aarch64__) && defined(__APPLE__)
// Keep this well above the low address ranges that Darwin's ASLR may use for
// a PIE executable and its shared cache. Apple Silicon's user VA is wider
// than Linux's 39-bit minimum, so this 512 GiB region is available while the
// Linux AArch64 target retains its 64 GiB placement below.
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'0080'0000'0000ull;
#elif defined(__aarch64__)
// 16 TiB (this arch's x86_64 sibling value) is unreachable on any AArch64
// kernel configured for 39-bit virtual addresses (512 GiB ceiling) - common on
// older/embedded targets (e.g. this project's own tested Jetson/L4T board, kernel
// 4.9). There mmap() silently ignores a hint above the ceiling and hands back an
// address near the top of the real range instead, which this module's caller
// then rejects as "already occupied". NSMBW: 256 GiB, reachable on every AArch64
// VA width in real use (39-bit minimum and up). The upstream 64 GiB collided on
// Android 8 (LG V30+, kernel 4.4, 39-bit): that kernel places PIE executables
// from 4 GiB up with up to 64 GiB of randomization, and app_process64 sat at
// 0x10'2BBE'F000. Newer kernels put the PIE image, heap, shared libraries and
// stack above ~340 GiB of a 39-bit/512 GiB space, so 256-260 GiB is clear of both.
inline constexpr uintptr_t kFixedFlatGuestBase = 0x0000'0040'0000'0000ull;
#else
#error "guest_flat_memory.h has no fixed flat guest base chosen for this architecture"
#endif

#if defined(MKW_GUEST_FLAT_DYNAMIC_BASE)
#define MKW_FLAT_GUEST_BASE (GuestFlat::g_flatGuestBase)
#else
#define MKW_FLAT_GUEST_BASE (reinterpret_cast<uint8_t*>(GuestFlat::kFixedFlatGuestBase))
#endif

enum class Backing {
    Owned,
    Mem1,
    Mem2,
};

struct RegionRequest {
    uint32_t base = 0;
    uint64_t size = 0;
    Backing backing = Backing::Owned;
};

struct FaultCounters {
    uint32_t mmio = 0;      // MMIO access that reached the handler
    uint32_t efb = 0;       // deferred (EFB) read materialized from a trap
    uint32_t xguard = 0;    // executable-page write trap
    uint32_t unmapped = 0;  // guest touches that landed outside every mapped region
    uint32_t unmappedRegions = 0;  // distinct 64 KiB blocks committed on demand
};

// True once the reservation exists and translated code may use the flat path.
bool IsActive();

// True when a host VM page covers more than one 4 KiB Wii page. In that
// configuration, guest-view page protection cannot safely represent per-Wii-
// page MMIO, deferred-read, or executable-write state, so general translated
// accesses must use the checked Memory::* path.
// Windows user mode and x86-64 always use a 4 KiB base page, so those builds
// fold this to a compile-time false: it appears in every flat access and must
// not become a hot-path load. Only AArch64, where the page size is a kernel
// configuration (4/16/64 KiB), has to probe it at runtime.
#if defined(_WIN32) || defined(__x86_64__)
#define MKW_GUEST_FLAT_FIXED_PAGE_SIZE 1
#endif

#if defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)
inline constexpr bool RequiresCheckedAccess() noexcept { return false; }
#else
// Const, set from the page size while the library initializes: a store through a flat guest pointer
// (a memcpy, which may alias any object) cannot change it, so the compiler loads it once per
// function instead of again after every guest store.
extern const bool g_requiresCheckedAccess;
inline bool RequiresCheckedAccess() noexcept { return g_requiresCheckedAccess; }
#endif

// Reserves the 4 GiB space (once per process) and maps every requested region
// into both views. Throws std::runtime_error with a precise diagnosis when the
// reservation, the section objects or a view cannot be created - a silent
// fallback would let translated code read from an unmapped constant base.
void Initialize(const std::vector<RegionRequest>& regions);

// Host-view pointer for a mapped guest address, or nullptr when the address is
// outside every mapped region. This is what the page table and
// Memory::GetPointer hand out.
uint8_t* HostPointer(uint32_t guestAddress);

// Deferred (EFB) reads: the covered guest pages are made PAGE_NOACCESS in the
// guest view so a flat read traps and materializes the copy.
void ProtectDeferredRange(uint32_t address, size_t length);
void UnprotectDeferredRange(uint32_t address, size_t length);

// Executable-write guard: pages fully covered by a registered executable range
// become PAGE_READONLY in the guest view. Registration order does not matter -
// ranges registered before the mapping exists are re-applied by Initialize.
void RegisterExecutableRange(uint32_t start, uint32_t end);

FaultCounters Counters();

// End-of-run report for the counters above. An unmapped touch is a wild guest
// pointer whose block this module silently committed so execution could go on,
// which makes it invisible unless it is repeated at shutdown; a nonzero count
// is therefore reported as a warning. Idempotent, so every exit path (normal
// return, caught exception, abort handler) may call it.
void LogFaultSummary() noexcept;

// Returns true when the access violation was a guest-space fault this module
// resolved; the caller must then resume execution. `faultAddress` is the raw
// host pointer the access violation trapped on (Windows: ExceptionInformation[1];
// POSIX: siginfo_t::si_addr) and `isWrite` is whether it was a write access
// (Windows: ExceptionInformation[0] != 0; POSIX: derived from the ucontext).
// The platform-specific handler that calls this is expected to have already
// done that extraction - this function only ever works with the parsed pair.
bool HandleAccessViolation(void* faultAddress, bool isWrite) noexcept;

} // namespace GuestFlat
