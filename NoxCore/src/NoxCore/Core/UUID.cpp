#include "UUID.h"

#include <random>

namespace Nox
{
    // An engine per thread, each seeded from the device: entities are made on workers too (a world cell is built on its loading job).
    static uint64_t RandomUUID()
    {
        thread_local std::mt19937_64 engine{ std::random_device{}() };
        thread_local std::uniform_int_distribution<uint64_t> distribution;
        return distribution(engine);
    }

    UUID::UUID()
        : m_UUID(RandomUUID())
    {
    }

    UUID::UUID(uint64_t uuid)
        : m_UUID(uuid)
    {
    }
}