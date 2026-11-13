#pragma once
#include "NetProtocol.h"
#include "NetSnapshot.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

// Interest management (docs/Networking_Plan_2026-11.md, NET4; Unreal's relevancy and Replication Graph): which replicated entities
// each client gets. Unreal's rules (AActor::IsNetRelevantFor):
//   - always relevant (bAlwaysRelevant): a game mode's state, a scoreboard;
//   - the client's own entities (owner) always;
//   - only relevant to the owner (bOnlyRelevantToOwner): to nobody else;
//   - else within its cull distance (NetCullDistanceSquared) of one of the client's viewers -- the entities it owns (its character,
//     its vehicle);
//   - and, in a streamed world, only when that client has the entity's level (a world cell, a streamed sub-level) loaded too
//     (Unreal's level visibility: ServerUpdateLevelVisibility).
// A relevant entity stays relevant for a while after it leaves the range (RelevantTimeout), so one walking along the edge does not
// appear and disappear. The entities are bucketed in a grid on the ground plane (the Replication Graph's spatial grid), so a query
// looks at the cells around each viewer, not at every entity. Nothing here knows the scene (tested alone, Tests/NetTest.cpp).
namespace Nox::Net
{
    struct RelevancyEntity
    {
        NetId Id = 0;
        glm::dvec3 Position{ 0.0 };   // world units
        double CullDistance = 0.0;    // world units
        ClientId Owner = InvalidClientId;
        uint64_t Level = 0;           // the streamed level (or world cell) it belongs to; 0: always loaded
        float Priority = 1.0f;        // Unreal's NetPriority
        bool AlwaysRelevant = false;
        bool OnlyRelevantToOwner = false;
    };

    // One client as relevancy sees it.
    struct RelevancyViewer
    {
        ClientId Client = InvalidClientId;
        std::vector<glm::dvec3> Positions;                    // its viewers (world units)
        const std::unordered_set<uint64_t>* Levels = nullptr; // the streamed levels it has loaded (null: none)
    };

    struct RelevantEntity
    {
        uint32_t Index = 0;    // into the grid's entities
        float Priority = 1.0f; // NetPriority, times 4 for its own entities, less far away (Unreal's AActor::GetNetPriority)
    };

    class RelevancyGrid
    {
    public:
        // Every replicated entity this snapshot. `streamedLevels`: the levels the server has streamed in -- an entity of one is relevant
        // only to clients that have it loaded too (entities of levels that are not streamed -- the scene itself, always loaded
        // sub-levels -- are in every client's world). Kept by reference until the next Build.
        void Build(std::span<const RelevancyEntity> entities, const std::unordered_set<uint64_t>& streamedLevels, double cellSize);
        // The entities relevant to `viewer` (each once).
        void Query(const RelevancyViewer& viewer, std::vector<RelevantEntity>& out) const;
        // The same rules for one entity, without the grid (the reference the grid is tested against).
        bool IsRelevant(const RelevancyEntity& entity, const RelevancyViewer& viewer, float* priority = nullptr) const;
        // The rules that do not depend on where it is (owner only, level loaded): an entity failing them leaves at once, not after
        // the relevant time-out.
        bool IsAllowed(const RelevancyEntity& entity, const RelevancyViewer& viewer) const;

        std::span<const RelevancyEntity> GetEntities() const { return m_Entities; }
        // The entity with this id (-1: none).
        int64_t Find(NetId id) const;

    private:
        static uint64_t CellKey(int64_t x, int64_t z);
        float PriorityAt(const RelevancyEntity& entity, double distance, bool owned) const;

        std::span<const RelevancyEntity> m_Entities;
        const std::unordered_set<uint64_t>* m_StreamedLevels = nullptr;
        double m_CellSize = 1.0;
        double m_MaxGriddedCull = 0.0;                        // the largest cull distance in the grid (how far a query looks)
        std::unordered_map<uint64_t, std::vector<uint32_t>> m_Cells;
        std::vector<uint32_t> m_Wide;                          // cull distances beyond 4 cells: checked one by one
        std::vector<uint32_t> m_Global;                        // always relevant, or relevant to the owner only
        std::unordered_map<NetId, uint32_t> m_Index;
        std::unordered_map<ClientId, std::vector<uint32_t>> m_Owned;  // each client's own entities
        mutable std::vector<uint32_t> m_Stamp;                 // (a query's de-duplication)
        mutable uint32_t m_Query = 0;
    };

    // One client's relevant entities from snapshot to snapshot, with Unreal's relevant time-out.
    class ClientRelevancy
    {
    public:
        // This snapshot's relevant entities: the grid's query, plus those relevant within the last `timeout` seconds that still exist
        // and are still allowed (at a low priority).
        void Update(const RelevancyGrid& grid, const RelevancyViewer& viewer, double now, double timeout, std::vector<RelevantEntity>& out);
        void Clear() { m_LastRelevant.clear(); }

    private:
        std::unordered_map<NetId, double> m_LastRelevant;
        std::vector<RelevantEntity> m_Query;
    };

    // The world a client gets: the server's world filtered to the relevant entities (`priorities` filled for the snapshot's budget).
    void FilterWorld(const WorldState& world, const RelevancyGrid& grid, std::span<const RelevantEntity> relevant, WorldState& out,
                     std::unordered_map<NetId, float>& priorities);
}
