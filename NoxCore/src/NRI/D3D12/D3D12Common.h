#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

#include "NoxCore/Core/Log.h"

namespace NRI
{
    using Microsoft::WRL::ComPtr;

    inline void ThrowIfFailed(HRESULT hr, const char* message = nullptr)
    {
        if (FAILED(hr))
        {
            char errorMsg[512];
            snprintf(errorMsg, sizeof(errorMsg), "D3D12 Error (HRESULT 0x%08X)%s%s", 
                     static_cast<unsigned int>(hr), message ? ": " : "", message ? message : "");
            NOX_CORE_ERROR("{}", errorMsg);
            throw std::runtime_error(errorMsg);
        }
    }
}

#endif // _WIN32
