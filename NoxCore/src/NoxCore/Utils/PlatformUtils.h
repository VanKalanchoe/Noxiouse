#pragma once
#include <cstdint>

namespace Nox::Platform
{
    struct ProcessMemory
    {
        uint64_t WorkingSetBytes = 0; // physical RAM currently mapped into the process
        uint64_t PrivateBytes = 0;    // committed memory that belongs only to this process
    };

    ProcessMemory QueryProcessMemory();
}
