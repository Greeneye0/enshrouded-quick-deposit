// powrprof.dll proxy: loads the real system DLL and forwards every export to it
// through the jump stubs in proxy_stubs.asm.
#include <windows.h>
#include "proxy_exports.h"
#include "log.h"

extern "C" void* g_proxyTargets[kProxyExportCount] = {};

bool LoadProxyTargets()
{
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 20)
        return false;
    lstrcatW(path, L"\\powrprof.dll");

    HMODULE real = LoadLibraryW(path);
    if (!real)
        return false;

    for (int i = 0; i < kProxyExportCount; ++i)
        g_proxyTargets[i] = reinterpret_cast<void*>(GetProcAddress(real, kProxyExportNames[i]));
    return true;
}
