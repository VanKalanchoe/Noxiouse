#include "NetRelevancy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Nox::Net
{
    uint64_t RelevancyGrid::CellKey(int64_t x, int64_t z)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
    }

    void RelevancyGrid::Build(std::span<const RelevancyEntity> entities, const std::unordered_set<uint64_t>& streamedLevels, double cellSize)
    {
        m_Entities = entities;
        m_StreamedLevels = &streamedLevels;
        m_CellSize = cellSize > 0.0 ? cellSize : 1.0;
        m_MaxGriddedCull = 0.0;
        for (auto& [_, cell] : m_Cells)
            cell.clear(); // (the buckets are kept: the same cells are mostly used again next snapshot)
        m_Wide.clear();
        m_Global.clear();
        m_Index.clear();
        m_Index.reserve(entities.size());
        m_Owned.clear();
        m_Stamp.assign(entities.size(), 0);
        m_Query = 0;
        const double wide = m_CellSize * 4.0;
        for (uint32_t index = 0; index < entities.size(); ++index)
        {
            const RelevancyEntity& entity = entities[index];
            m_Index.emplace(entity.Id, index);
            if (entity.Owner != InvalidClientId)
                m_Owned[entity.Owner].push_back(index);
            if (entity.AlwaysRelevant || entity.OnlyRelevantToOwner)
                m_Global.push_back(index);
            else if (entity.CullDistance > wide)
                m_Wide.push_back(index);
            else
            {
                m_MaxGriddedCull = std::max(m_MaxGriddedCull, entity.CullDistance);
                const int64_t x = static_cast<int64_t>(std::floor(entity.Position.x / m_CellSize));
                const int64_t z = static_cast<int64_t>(std::floor(entity.Position.z / m_CellSize));
                m_Cells[CellKey(x, z)].push_back(index);
            }
        }
    }

    int64_t RelevancyGrid::Find(NetId id) const
    {
        auto found = m_Index.find(id);
        return found != m_Index.end() ? static_cast<int64_t>(found->second) : -1;
    }

    float RelevancyGrid::PriorityAt(const RelevancyEntity& entity, double distance, bool owned) const
    {
        // Unreal's AActor::GetNetPriority: the viewer's own entities four times as important; things far away less (here: full
        // priority within a quarter of the cull distance, a quarter of it at the cull distance).
        float scale = 1.0f;
        if (owned)
            scale = 4.0f;
        else if (entity.CullDistance > 0.0 && std::isfinite(distance))
        {
            const double t = std::clamp((distance / entity.CullDistance - 0.25) / 0.75, 0.0, 1.0);
            scale = static_cast<float>(1.0 - 0.75 * t);
        }
        return std::max(entity.Priority, 0.0f) * scale;
    }

    bool RelevancyGrid::IsAllowed(const RelevancyEntity& entity, const RelevancyViewer& viewer) const
    {
        if (entity.OnlyRelevantToOwner && entity.Owner != viewer.Client)
            return false;
        if (entity.Level != 0 && m_StreamedLevels && m_StreamedLevels->contains(entity.Level))
            return viewer.Levels && viewer.Levels->contains(entity.Level); // (the client has not loaded its level: it has no copy)
        return true;
    }

    bool RelevancyGrid::IsRelevant(const RelevancyEntity& entity, const RelevancyViewer& viewer, float* priority) const
    {
        if (!IsAllowed(entity, viewer))
            return false;
        const bool owned = entity.Owner != InvalidClientId && entity.Owner == viewer.Client;
        double nearest = std::numeric_limits<double>::infinity();
        for (const glm::dvec3& position : viewer.Positions)
            nearest = std::min(nearest, glm::distance(position, entity.Position));
        const bool relevant = entity.AlwaysRelevant || owned || (!entity.OnlyRelevantToOwner && nearest <= entity.CullDistance);
        if (relevant && priority)
            *priority = PriorityAt(entity, nearest, owned);
        return relevant;
    }

    void RelevancyGrid::Query(const RelevancyViewer& viewer, std::vector<RelevantEntity>& out) const
    {
        out.clear();
        if (++m_Query == 0) // (wrapped: the stamps start over)
        {
            std::fill(m_Stamp.begin(), m_Stamp.end(), 0u);
            m_Query = 1;
        }
        auto consider = [&](uint32_t index)
        {
            if (m_Stamp[index] == m_Query)
                return;
            float priority = 0.0f;
            if (!IsRelevant(m_Entities[index], viewer, &priority))
                return;
            m_Stamp[index] = m_Query;
            out.push_back({ index, priority });
        };
        for (uint32_t index : m_Global)
            consider(index);
        for (uint32_t index : m_Wide)
            consider(index);
        if (m_MaxGriddedCull > 0.0 || !m_Cells.empty())
        {
            const int64_t reach = static_cast<int64_t>(std::ceil(m_MaxGriddedCull / m_CellSize));
            for (const glm::dvec3& position : viewer.Positions)
            {
                const int64_t cx = static_cast<int64_t>(std::floor(position.x / m_CellSize));
                const int64_t cz = static_cast<int64_t>(std::floor(position.z / m_CellSize));
                for (int64_t x = cx - reach; x <= cx + reach; ++x)
                {
                    for (int64_t z = cz - reach; z <= cz + reach; ++z)
                    {
                        auto cell = m_Cells.find(CellKey(x, z));
                        if (cell == m_Cells.end())
                            continue;
                        for (uint32_t index : cell->second)
                            consider(index);
                    }
                }
            }
        }
        // The viewer's own entities wherever they are (a character beyond every other viewer's reach is still its own).
        if (auto owned = m_Owned.find(viewer.Client); viewer.Client != InvalidClientId && owned != m_Owned.end())
            for (uint32_t index : owned->second)
                consider(index);
    }

    void ClientRelevancy::Update(const RelevancyGrid& grid, const RelevancyViewer& viewer, double now, double timeout,
                                 std::vector<RelevantEntity>& out)
    {
        grid.Query(viewer, m_Query);
        out = m_Query;
        std::unordered_map<NetId, double> next;
        next.reserve(m_Query.size() + m_LastRelevant.size());
        for (const RelevantEntity& relevant : m_Query)
            next.emplace(grid.GetEntities()[relevant.Index].Id, now);
        for (const auto& [id, at] : m_LastRelevant)
        {
            if (next.contains(id) || now - at >= timeout)
                continue;
            const int64_t index = grid.Find(id);
            if (index < 0 || !grid.IsAllowed(grid.GetEntities()[static_cast<size_t>(index)], viewer))
                continue; // gone, or no longer allowed (its level was unloaded on the client, handed to someone else)
            const RelevancyEntity& entity = grid.GetEntities()[static_cast<size_t>(index)];
            out.push_back({ static_cast<uint32_t>(index), std::max(entity.Priority, 0.0f) * 0.25f });
            next.emplace(id, at);
        }
        m_LastRelevant = std::move(next);
    }

    void FilterWorld(const WorldState& world, const RelevancyGrid& grid, std::span<const RelevantEntity> relevant, WorldState& out,
                     std::unordered_map<NetId, float>& priorities)
    {
        out.Tick = world.Tick;
        out.Entities.clear();
        out.Entities.reserve(relevant.size());
        priorities.clear();
        priorities.reserve(relevant.size());
        for (const RelevantEntity& entry : relevant)
        {
            const NetId id = grid.GetEntities()[entry.Index].Id;
            auto state = world.Entities.find(id);
            if (state == world.Entities.end())
                continue;
            out.Entities.emplace(id, state->second);
            priorities.emplace(id, entry.Priority);
        }
    }
}
