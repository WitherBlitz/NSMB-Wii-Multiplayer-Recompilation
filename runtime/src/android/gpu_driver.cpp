// gpu_driver.cpp - the GPU driver picked in the app's start menu (GpuDrivers.java, Snapdragon only).
//
// Dawn loads Vulkan with dlopen("libvulkan.so"). libmain.so links with --wrap=dlopen, so that call
// lands here; with a driver picked it opens libadrenotools.so (which nobody else ever loads) and gets
// its private copy of the system's Vulkan loader, which loads that driver in place of the phone's own.
// Every other dlopen passes straight through, so a game on the phone's own driver only pays for one
// string compare per library opened.
#if defined(__ANDROID__)

#include "runtime_log.h"

#include <adrenotools/driver.h>
#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
void* __real_dlopen(const char* filename, int flags);
int __real_dlclose(void* handle);
void* __wrap_dlopen(const char* filename, int flags);
int __wrap_dlclose(void* handle);
}

namespace {

std::mutex g_lock;
void* g_driverLoader = nullptr;
bool g_tried = false;

bool IsVulkanLoader(const char* filename) {
    if (filename == nullptr) {
        return false;
    }
    const char* slash = std::strrchr(filename, '/');
    return std::strcmp(slash != nullptr ? slash + 1 : filename, "libvulkan.so") == 0;
}

// The loader with the picked driver, or null for the phone's own: no driver picked, or one that would
// not load. All four are set by GpuDrivers.applyToGame before the library starts.
void* OpenPickedDriver(int flags) {
    const std::lock_guard<std::mutex> hold(g_lock);
    if (g_tried) {
        return g_driverLoader;
    }
    g_tried = true;
    const char* driverDir = std::getenv("WIICOMPILED_GPU_DRIVER_DIR");
    const char* library = std::getenv("WIICOMPILED_GPU_DRIVER_LIB");
    const char* hookDir = std::getenv("WIICOMPILED_GPU_HOOK_DIR");
    const char* tmpDir = std::getenv("WIICOMPILED_GPU_TMP_DIR");
    if (driverDir == nullptr || library == nullptr || hookDir == nullptr || tmpDir == nullptr) {
        return nullptr;
    }
    void* tools = __real_dlopen((std::string(hookDir) + "libadrenotools.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    auto* open = tools != nullptr
                     ? reinterpret_cast<decltype(&adrenotools_open_libvulkan)>(dlsym(tools, "adrenotools_open_libvulkan"))
                     : nullptr;
    if (open != nullptr) {
        g_driverLoader = open(flags, ADRENOTOOLS_DRIVER_CUSTOM, tmpDir, hookDir, driverDir, library, nullptr, nullptr);
    }
    if (g_driverLoader != nullptr) {
        RT_LOG(RT_TAG_GX) << "GPU driver: " << driverDir << library << std::endl;
    } else {
        const char* why = dlerror();
        RT_LOG(RT_TAG_GX) << "GPU driver " << driverDir << library << " would not load ("
                          << (why != nullptr ? why : "no reason given") << "); using the phone's own" << std::endl;
    }
    return g_driverLoader;
}

}  // namespace

void* __wrap_dlopen(const char* filename, int flags) {
    if (IsVulkanLoader(filename) && (flags & RTLD_NOLOAD) == 0) {
        if (void* loader = OpenPickedDriver(flags)) {
            return loader;
        }
    }
    return __real_dlopen(filename, flags);
}

// The picked driver stays loaded for the life of the process: it can only be opened once, and each
// start of the game is a new process anyway.
int __wrap_dlclose(void* handle) {
    if (handle != nullptr && handle == g_driverLoader) {
        return 0;
    }
    return __real_dlclose(handle);
}

#endif  // defined(__ANDROID__)
