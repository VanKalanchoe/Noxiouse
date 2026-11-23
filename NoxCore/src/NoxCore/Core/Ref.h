#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace NoxUI
{
    template <typename T>
    class Ref;
    template <typename T>
    class UIWidget;
}

namespace Nox
{
    class RefCounted
    {
    public:
        void IncRefCount() const { m_RefCount.fetch_add(1, std::memory_order_relaxed); }
        void DecRefCount() const
        {
            if (m_RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
                delete this;
        }
        
        uint32_t GetRefCount() const { return m_RefCount.load(std::memory_order_acquire); }

    protected:
        RefCounted() = default;
        virtual ~RefCounted() = default;

    private:
        mutable std::atomic<uint32_t> m_RefCount = 0;
    };
    
   template<typename T>
   class Ref
    {
    public:
        // Constructors
        Ref() : m_ptr(nullptr) {}
        Ref(std::nullptr_t) : m_ptr(nullptr) {}
        explicit Ref(T* ptr) : m_ptr(ptr) { if (m_ptr) m_ptr->IncRefCount(); }
        Ref(const Ref& other) : m_ptr(other.m_ptr) { if (m_ptr) m_ptr->IncRefCount(); }
        Ref(Ref&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

        // Destructor
        ~Ref() { if (m_ptr) m_ptr->DecRefCount(); }

        // Assignment operators
        Ref& operator=(const Ref& other)
        {
            if (m_ptr != other.m_ptr)
            {
                if (m_ptr) m_ptr->DecRefCount();
                m_ptr = other.m_ptr;
                if (m_ptr) m_ptr->IncRefCount();
            }
            return *this;
        }

        Ref& operator=(Ref&& other) noexcept
        {
            if (m_ptr) m_ptr->DecRefCount();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
            return *this;
        }
       
       Ref<T>& operator=(std::nullptr_t) {
            if (m_ptr) m_ptr->DecRefCount();
            m_ptr = nullptr;
            return *this;
        }
       
       template<typename U>
        Ref(const Ref<U>& other)
        {
            static_assert(std::is_base_of_v<U, T> || std::is_base_of_v<T, U>, "Invalid Ref conversion");
            m_ptr = static_cast<T*>(other.get());
            if (m_ptr)
                m_ptr->IncRefCount();
        }

        template<typename U> requires (std::is_base_of_v<U, T> || std::is_base_of_v<T, U>)
        Ref(const ::NoxUI::Ref<U>& other)
        {
            m_ptr = static_cast<T*>(other.get());
            if (m_ptr)
                m_ptr->IncRefCount();
        }

        template<typename U> requires (std::is_base_of_v<U, T> || std::is_base_of_v<T, U>)
        Ref(const ::NoxUI::UIWidget<U>& widget)
        {
            m_ptr = static_cast<T*>(widget.Get().get());
            if (m_ptr)
                m_ptr->IncRefCount();
        }

        template<typename U> requires (std::is_base_of_v<U, T> || std::is_base_of_v<T, U>)
        operator ::NoxUI::Ref<U>() const
        {
            return ::NoxUI::Ref<U>(static_cast<U*>(m_ptr));
        }

        // Accessors
        T* get() const { return m_ptr; }
        T& operator*() const { return *m_ptr; }
        T* operator->() const { return m_ptr; }
        explicit operator bool() const { return m_ptr != nullptr; }

        // Comparison
        bool operator==(const Ref& other) const { return m_ptr == other.m_ptr; }
        bool operator!=(const Ref& other) const { return m_ptr != other.m_ptr; }
        bool operator==(std::nullptr_t) const { return m_ptr == nullptr; }
        bool operator!=(std::nullptr_t) const { return m_ptr != nullptr; }

    private:
        T* m_ptr;
    };
}