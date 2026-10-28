#include "guest_flat_memory.h"

// iOS flat guest space. Same two-view layout as the macOS backend, with two differences the iOS
// sandbox forces: the 4 GiB space goes wherever the kernel finds room (see guest_flat_memory.h),
// and the shared physical memory behind each view is an anonymous Mach memory entry instead of an
// unlinked /tmp file, which an app container cannot create there and whose dirty pages would
// otherwise be written back to flash. <mach/mach_vm.h> is unavailable on iOS; on arm64 the vm_*
// routines of <mach/vm_map.h> take the same 64-bit addresses.
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace GuestFlat {
extern const bool g_requiresCheckedAccess = static_cast<size_t>(getpagesize()) > kGuestPageSize;
uint8_t* g_flatGuestBase = nullptr;
namespace {
struct Mapping { uint32_t base; uint64_t size; uint8_t* host; };
std::mutex g_mutex;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
bool g_active = false;

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}
bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}
uint64_t RoundToPage(uint64_t size) {
    const uint64_t page = static_cast<uint64_t>(getpagesize());
    return (size + page - 1) & ~(page - 1);
}
[[noreturn]] void Fail(const char* what, kern_return_t kr) {
    throw std::runtime_error(std::string(what) + " (" + mach_error_string(kr) + ")");
}
// One view of `entry` at `address` (VM_FLAGS_ANYWHERE when `anywhere`), read/write.
uint8_t* MapView(mach_port_t entry, uint64_t offset, uint64_t size, vm_address_t address, bool anywhere) {
    const int flags = anywhere ? VM_FLAGS_ANYWHERE : (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    const kern_return_t kr = vm_map(mach_task_self(), &address, static_cast<vm_size_t>(size), 0, flags, entry,
        static_cast<memory_object_offset_t>(offset), FALSE, VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) Fail("unable to map iOS guest alias", kr);
    return reinterpret_cast<uint8_t*>(address);
}
} // namespace

bool IsActive() { return g_active; }
void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    if (g_active) { if (!Same(g_layout, regions)) throw std::runtime_error("flat guest layout cannot be remapped"); return; }
    vm_address_t address = 0;
    if (const kern_return_t kr = vm_allocate(mach_task_self(), &address, static_cast<vm_size_t>(kGuestSpaceSize), VM_FLAGS_ANYWHERE);
        kr != KERN_SUCCESS)
        Fail("unable to reserve the 4 GiB iOS guest address space; the app needs the "
             "com.apple.developer.kernel.extended-virtual-addressing entitlement", kr);
    auto* base = reinterpret_cast<uint8_t*>(address);
    struct Store { Backing kind; uint32_t owned; uint64_t size; mach_port_t entry; };
    std::vector<Store> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) { return s.kind == r.backing && s.owned == owned; });
        const uint64_t need = Offset(r) + r.size;
        if (it == stores.end()) stores.push_back({r.backing, owned, need, MACH_PORT_NULL}); else it->size = std::max(it->size, need);
    }
    for (auto& s : stores) {
        memory_object_size_t size = RoundToPage(s.size);
        const kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &size, 0,
            MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &s.entry, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) Fail("unable to create iOS guest backing store", kr);
    }
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) { return x.kind == r.backing && x.owned == owned; });
        uint8_t* host = MapView(s.entry, Offset(r), r.size, 0, true);
        uint8_t* guest = MapView(s.entry, Offset(r), r.size, reinterpret_cast<vm_address_t>(base + r.base), false);
        if (guest != base + r.base) throw std::runtime_error("unable to map iOS guest alias at its address");
        g_mappings.push_back({r.base, r.size, host});
    }
    // The mappings hold the memory objects; the entries' send rights are no longer needed.
    for (auto& s : stores) mach_port_deallocate(mach_task_self(), s.entry);
    g_flatGuestBase = base;
    g_layout = regions; g_active = true;
}
uint8_t* HostPointer(uint32_t a) { for (const auto& m : g_mappings) if (a >= m.base && uint64_t(a - m.base) < m.size) return m.host + (a - m.base); return nullptr; }
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }
} // namespace GuestFlat
