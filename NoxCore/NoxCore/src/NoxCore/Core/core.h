#pragma once

#include <memory>

#include "Ref.h"

namespace Nox
{
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
