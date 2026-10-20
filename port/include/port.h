#pragma once
#include <cstddef>
#include <new>
#include <strings.h>
#include <sys/types.h>
#include "port_window.h"

#define PORT_INIT(...) = __VA_ARGS__
#define PORT_RETURN(...) return __VA_ARGS__
#define PORT_FIX(...) __VA_ARGS__
[[noreturn]] void Ps2Unimplemented(const char *function, const char *file, int line);
#define PS2_UNIMPLEMENTED() Ps2Unimplemented(__func__, __FILE__, __LINE__)
#define PORT_OVERRIDE

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#pragma push_macro("NULL")
#define size_t ps2_size_t
#define u_long ps2_native_u_long
#include "types.h"
#undef u_long
#define u_long u64
#undef size_t
#pragma pop_macro("NULL")
#pragma clang diagnostic pop

#include "common.h"
#undef STATIC_ASSERT
#define STATIC_ASSERT(expr)
#undef PORT_RECORD_ASSERT
#define PORT_RECORD_ASSERT(expr) static_assert(expr)

#include "pal_timing.hpp"
#undef PAL_TIMING
#define PAL_TIMING(pal, ntsc) pal

template <typename T> struct ps2_pointer {
    unsigned int address;
    operator T *() const { return address ? reinterpret_cast<T *>(PORT_WINDOW + address) : nullptr; }
    T *operator->() const { return *this; }
    ps2_pointer &operator=(T *pointer) {
        address = pointer ? static_cast<unsigned int>(reinterpret_cast<__UINTPTR_TYPE__>(pointer) - PORT_WINDOW) : 0u;
        return *this;
    }
    ps2_pointer &operator+=(__PTRDIFF_TYPE__ count) {
        address += static_cast<unsigned int>(count * sizeof(T));
        return *this;
    }
};
