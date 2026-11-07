// Link shims for building against the real msvcrt.dll with a UCRT-configured MinGW toolchain.
// libmingwex's snprintf alias calls __ms_vsnprintf, which only exists in the msvcrt-flavoured
// libmingwex. Route it to mingw's own C99 printf (statically linked, no DLL import, supports %lld).
#include <stdio.h>
#include <stdarg.h>
extern "C" int __mingw_vsnprintf(char*, size_t, const char*, va_list);
extern "C" int __ms_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) { return __mingw_vsnprintf(buf, n, fmt, ap); }
