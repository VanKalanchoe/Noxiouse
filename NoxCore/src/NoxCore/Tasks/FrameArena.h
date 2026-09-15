#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Nox
{
    // Linear per-frame allocator (§5.2.4). One per thread slot (JobSystem::GetFrameArena), reset at the frame sync
    // point, so frame graph tasks allocate scratch data without heap churn or allocator contention. Memory stays
    // reserved across frames. Only frame graph tasks and the main thread use it; async tasks outlive a frame and
    // must not.
    class FrameArena
    {
    public:
        static constexpr size_t DefaultBlockSize = 1024 * 1024;

        FrameArena() = default;
        FrameArena(const FrameArena&) = delete;
        FrameArena& operator=(const FrameArena&) = delete;
        FrameArena(FrameArena&&) noexcept = default;
        FrameArena& operator=(FrameArena&&) noexcept = default;

        void* Allocate(size_t size, size_t alignment);

        // Default-initialized array; the arena never runs destructors, so T must be trivially destructible.
        template <typename T>
        std::span<T> NewArray(size_t count)
        {
            static_assert(std::is_trivially_destructible_v<T>, "FrameArena never runs destructors");
            if (count == 0)
                return {};

            T* data = static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
            for (size_t i = 0; i < count; ++i)
                new (data + i) T;
            return { data, count };
        }

        // Constructs one object; the caller runs its destructor before the arena is reset if it has one.
        template <typename T, typename... Args>
        T* New(Args&&... args)
        {
            return new (Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
        }

        // Keeps every block for the next frame.
        void Reset();

    private:
        struct Block
        {
            std::unique_ptr<std::byte[]> Memory;
            size_t Size = 0;
        };

    private:
        std::vector<Block> m_Blocks;
        size_t m_BlockIndex = 0;
        size_t m_Offset = 0;
    };
}
