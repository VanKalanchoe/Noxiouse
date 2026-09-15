#include "PlatformUtils.h"

// Platform code that SDL3 does not cover: SDL3 (3.4) only reports installed RAM (SDL_GetSystemRAM), not the
// memory used by this process. Add other operating systems here next to the Windows implementation.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace Nox::Platform
{
    ProcessMemory QueryProcessMemory()
    {
        ProcessMemory result;
#ifdef _WIN32
        // PROCESS_MEMORY_COUNTERS_EX adds PrivateUsage; GetProcessMemoryInfo (K32GetProcessMemoryInfo in
        // kernel32 with PSAPI_VERSION 2) accepts it when cb is set to its size.
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            result.WorkingSetBytes = counters.WorkingSetSize;
            result.PrivateBytes = counters.PrivateUsage;
        }
#endif
        return result;
    }
}
