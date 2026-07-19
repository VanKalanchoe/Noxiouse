#pragma once

#include <memory>

#include "Ref.h"

//---------Event def------
#define BIT(x) (1 << (x))
#define Nox_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }
//------------------------

namespace Nox
{
    inline bool IsEngineShuttingDown = false;
    
    template <typename T>
    using Scope = std::unique_ptr<T>;
    template <typename T, typename... Args>
    constexpr Scope<T> CreateScope(Args&&... args)
    {
        return std::make_unique<T>(std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    Ref<T> CreateRef(Args&&... args)
    {
        // Simply call constructor instead of T::Create()
        T* obj = new T(std::forward<Args>(args)...);
        return Ref<T>(obj);
    }
}
