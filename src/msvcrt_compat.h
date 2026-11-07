// Force-included when building against the real msvcrt.dll (-mcrtdll=msvcrt-os). This toolchain's
// libstdc++ was configured for UCRT and its <cstdlib> re-exports quick_exit/at_quick_exit, which
// msvcrt's headers don't declare. Declaring them is enough: nothing calls them, so nothing imports them.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int __cdecl at_quick_exit(void (__cdecl *)(void));
void __cdecl quick_exit(int) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
