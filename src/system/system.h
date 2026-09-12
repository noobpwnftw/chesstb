#pragma once

#if defined(_WIN32) || defined(WIN32)

#define OS_WINDOWS

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

namespace sys_common
{
    using Native_Handle = void*;
    const Native_Handle INVALID_HANDLE_VALUE = (Native_Handle)(-1);
}

#elif defined (__linux__)

#define OS_LINUX

namespace sys_common
{
    using Native_Handle = int;
    const Native_Handle INVALID_HANDLE_VALUE = -1;
}

#else

#error "Unsupported OS"

#endif