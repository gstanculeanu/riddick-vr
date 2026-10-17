// GH #80 (requirement 1) test support: an independent, *unpatched* resolution
// path for tests/sim_input_bridge_win32_tests.cpp.
//
// The interceptor test installs the bridge on the test executable's own
// ``KERNEL32.dll!GetProcAddress`` IAT entry and then checks which procs
// ``GetProcAddress`` returns. To decide whether a proc was *replaced* it needs a
// baseline from the same process that did NOT go through the patched entry.
//
// Getting that baseline from the test executable itself is unreliable: the
// optimizer is free to common-subexpression-eliminate two
// ``GetProcAddress(hModule, "SameName")`` calls (it did), which makes the
// "after" resolution silently reuse the "before" result and hides a broken
// intercept. This DLL is loaded by the test and only patches the *test's* IAT,
// never ours, so ``ProbeGetProcAddress`` always asks the real loader.
#include <windows.h>

extern "C" __declspec(dllexport) FARPROC WINAPI ProbeGetProcAddressA(HMODULE hModule,
                                                                    LPCSTR procName) {
    return GetProcAddress(hModule, procName);
}
