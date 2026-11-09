# Bluetooth passthrough (runtime/src/bt/): a USB Bluetooth adapter driven through libusb, on Windows
# and Linux. Sets NSMBW_BT_LIBUSB to the libusb target to link, or leaves it empty, in which case
# the passthrough code compiles to its "not part of this build" stub (Android, macOS, or a tree
# without libusb's source). Included after aurora, whose vendored SDL3 builds libusb on Windows.
set(NSMBW_BT_LIBUSB "")
if(MKW_PLATFORM_WINDOWS)
    # aurora-main/cmake/AuroraLibUSB.cmake: libusb 1.0.30, static, already linked into SDL3.
    if(TARGET usb-1.0)
        set(NSMBW_BT_LIBUSB usb-1.0)
    endif()
elseif(MKW_PLATFORM_LINUX)
    # The same pinned libusb source (Dependencies/libusb, handed in like every other dependency),
    # built here for Linux: static, without udev (hotplug isn't used), its symbols hidden so a
    # system libusb that SDL may load at run time never binds to them.
    set(_nsmbw_libusb_source "${FETCHCONTENT_SOURCE_DIR_LIBUSB}")
    if(NOT _nsmbw_libusb_source OR NOT EXISTS "${_nsmbw_libusb_source}/libusb/core.c")
        set(_nsmbw_libusb_source "${CMAKE_CURRENT_LIST_DIR}/../../Dependencies/libusb")
    endif()
    if(EXISTS "${_nsmbw_libusb_source}/libusb/core.c")
        set(_root "${_nsmbw_libusb_source}/libusb")
        add_library(nsmbw_libusb STATIC
            "${_root}/core.c"
            "${_root}/descriptor.c"
            "${_root}/hotplug.c"
            "${_root}/io.c"
            "${_root}/strerror.c"
            "${_root}/sync.c"
            "${_root}/os/events_posix.c"
            "${_root}/os/threads_posix.c"
            "${_root}/os/linux_usbfs.c"
            "${_root}/os/linux_netlink.c")
        target_include_directories(nsmbw_libusb
            PUBLIC "${_root}"
            PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libusb-linux" "${_root}/os")
        target_compile_options(nsmbw_libusb PRIVATE -w -fvisibility=hidden)
        set_target_properties(nsmbw_libusb PROPERTIES UNITY_BUILD OFF POSITION_INDEPENDENT_CODE ON)
        find_package(Threads REQUIRED)
        target_link_libraries(nsmbw_libusb PUBLIC Threads::Threads)
        set(NSMBW_BT_LIBUSB nsmbw_libusb)
    else()
        message(STATUS "Bluetooth passthrough left out: no libusb source (Dependencies/libusb)")
    endif()
endif()
if(NSMBW_BT_LIBUSB)
    message(STATUS "Bluetooth passthrough: on, with ${NSMBW_BT_LIBUSB}")
endif()

# The runtime objects that use it (src/bt/*.cpp are part of mkw_runtime_common).
function(nsmbw_bt_passthrough_configure_runtime target)
    if(NSMBW_BT_LIBUSB)
        target_link_libraries(${target} PRIVATE ${NSMBW_BT_LIBUSB})
        set_source_files_properties(
            "${MKW_RUNTIME_SOURCE_DIR}/src/bt/bt_usb_libusb.cpp"
            "${MKW_RUNTIME_SOURCE_DIR}/src/bt/bt_passthrough.cpp"
            PROPERTIES COMPILE_DEFINITIONS "NSMBW_BT_PASSTHROUGH=1" SKIP_UNITY_BUILD_INCLUSION ON
                       SKIP_PRECOMPILE_HEADERS ON)
    endif()
endfunction()

# The products, which link the runtime's objects (an OBJECT library passes on no link dependencies).
function(nsmbw_bt_passthrough_link_product target)
    if(NSMBW_BT_LIBUSB)
        target_link_libraries(${target} PRIVATE ${NSMBW_BT_LIBUSB})
    endif()
endfunction()
