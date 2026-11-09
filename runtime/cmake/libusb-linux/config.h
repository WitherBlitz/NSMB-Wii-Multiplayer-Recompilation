/* libusb build configuration for Linux (glibc 2.35+, the release's baseline), for Bluetooth
   passthrough (runtime/cmake/BtPassthrough.cmake). No udev: netlink hotplug, which is unused. */
#pragma once

#define DEFAULT_VISIBILITY
#define ENABLE_LOGGING 1
#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DECL_EFD_CLOEXEC 1
#define HAVE_DECL_EFD_NONBLOCK 1
#define HAVE_DECL_TFD_CLOEXEC 1
#define HAVE_DECL_TFD_NONBLOCK 1
#define HAVE_EVENTFD 1
#define HAVE_TIMERFD 1
#define HAVE_NFDS_T 1
#define HAVE_PIPE2 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_SYS_TIME_H 1
#define PLATFORM_POSIX 1
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
