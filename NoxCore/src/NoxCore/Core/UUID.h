#pragma once

#include <xhash>

namespace Nox
{
    class UUID
    {
    public:
        UUID();
        UUID(uint64_t uuid);
        UUID(const UUID&) = default;

        operator uint64_t() const { return m_UUID; }
    private:
        uint64_t m_UUID; //128bit uuid in the future
    };
}

namespace std
{
    template <typename T> struct hash;
    
    template<>
    struct hash<Nox::UUID>
    {
        std::size_t operator()(const Nox::UUID& uuid) const
        {
            /*return hash<uint64_t>()((uint64_t)uuid);*/
            return (uint64_t)uuid;
        }
    };
}