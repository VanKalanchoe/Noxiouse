#include "WorldPartitionPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <string>

#include <imgui.h>

#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Scene/HLODBuilder.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    namespace
    {
        constexpr ImU32 kUnloadedFill = IM_COL32(70, 70, 76, 110);
        constexpr ImU32 kLoadingFill = IM_COL32(230, 170, 40, 150);
        constexpr ImU32 kLoadedFill = IM_COL32(60, 170, 90, 140);       // activated: in the world
        constexpr ImU32 kStagedFill = IM_COL32(70, 130, 210, 140);      // loaded, not activated (§5.13.3)

        const char* StageName(Scene::WorldCellStage stage)
        {
            switch (stage)
            {
            case Scene::WorldCellStage::Loading: return "Loading";
            case Scene::WorldCellStage::Loaded: return "Loaded (not activated: not in the world)";
            case Scene::WorldCellStage::Activating: return "Activating";
            case Scene::WorldCellStage::Activated: return "Activated (in the world)";
            default: return "Unloaded";
            }
        }
        constexpr ImU32 kCellBorder = IM_COL32(150, 150, 160, 200);
        constexpr ImU32 kSelectedBorder = IM_COL32(255, 255, 255, 255);
        constexpr ImU32 kBoundsColor = IM_COL32(120, 200, 255, 160);
        constexpr ImU32 kCameraColor = IM_COL32(255, 110, 60, 255);

        std::vector<WorldCellCoord> AllCells(const Scene& scene)
        {
            // The world's indexed cells (not the HLOD grid's) and the ones loaded without a file yet (new, or emptied and not saved).
            std::set<WorldCellCoord> cells;
            for (const auto& [cell, info] : scene.GetWorldPartition().Cells)
                if (cell.Grid == WorldGrid::WorldGridIndex)
                    cells.insert(cell);
            for (const WorldCellCoord& cell : scene.GetLoadedWorldCells(true))
                cells.insert(cell);
            // Big cells first: the small ones are drawn over them.
            std::vector<WorldCellCoord> ordered(cells.begin(), cells.end());
            std::stable_sort(ordered.begin(), ordered.end(), [](const WorldCellCoord& a, const WorldCellCoord& b) { return a.Level > b.Level; });
            return ordered;
        }
    }

    void WorldPartitionPanel::OnImGuiRender(const Ref<Scene>& scene, const glm::dvec3& cameraPosition, bool editing)
    {
        if (!m_Open)
            return;
        ImGui::SetNextWindowSize(ImVec2(520.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("World Partition", &m_Open))
        {
            ImGui::End();
            return;
        }
        if (!scene)
        {
            ImGui::TextDisabled("No scene");
            ImGui::End();
            return;
        }

        DrawSettings(*scene, editing);
        if (scene->IsWorldPartitioned())
        {
            DrawHLODs(*scene, editing);
            DrawToolbar(*scene, cameraPosition, editing);
            DrawMap(*scene, cameraPosition, editing);
        }
        DrawUnloadPrompt(*scene);
        ImGui::End();
    }

    void WorldPartitionPanel::DrawSettings(Scene& scene, bool editing)
    {
        WorldPartition& world = scene.GetWorldPartition();
        // Another scene: its settings in the fields, the map fitted to it again.
        if (m_SettingsScene != &scene)
        {
            m_SettingsScene = &scene;
            m_NewSettings = world.Settings.Enabled ? world.Settings : WorldPartitionSettings();
            m_UnitsPerPixel = 0.0f;
            m_Selection.clear();
        }

        if (!world.Settings.Enabled)
        {
            ImGui::TextWrapped("This scene is not a world. Converted, its entities with something in the world (meshes, models, prefabs, "
                               "local lights, bodies ...) are kept in grid cells, each in its own file next to the scene, and streamed in "
                               "and out around the camera in Play. Sky, sun, cameras, the player and logic stay in the scene file. The cell "
                               "files are written on the next save.");
            ImGui::Spacing();
            ImGui::TextDisabled("Each root entity goes into one cell. A model placed as one instance (Bistro) would stream as one piece:\n"
                                "split it first (Properties > Model Instance > Split Into Separate Entities).");
            ImGui::Spacing();
            ImGui::BeginDisabled(!editing);
            ImGui::DragFloat("Cell Size (m)", &m_NewSettings.CellSize, 1.0f, 8.0f, 4096.0f, "%.0f");
            ImGui::DragFloat("Loading Range (m)", &m_NewSettings.LoadingRange, 1.0f, 8.0f, 16384.0f, "%.0f");
            if (ImGui::Button("Convert to World Partition"))
            {
                scene.EnableWorldPartition(m_NewSettings);
                m_UnitsPerPixel = 0.0f;
            }
            ImGui::EndDisabled();
            return;
        }

        if (!ImGui::CollapsingHeader("Settings"))
            return;
        ImGui::BeginDisabled(!editing);
        // The loading range only changes what streams: any time. A new cell size moves every entity to another cell, which the save
        // can only do with all of them in memory.
        if (ImGui::DragFloat("Loading Range (m)", &world.Settings.LoadingRange, 1.0f, 8.0f, 16384.0f, "%.0f"))
            m_NewSettings.LoadingRange = world.Settings.LoadingRange;
        bool allLoaded = true;
        for (const auto& [cell, info] : world.Cells)
            allLoaded &= cell.Grid != WorldGrid::WorldGridIndex || scene.GetWorldCellState(cell) == Scene::LevelState::Loaded;
        ImGui::BeginDisabled(!allLoaded);
        ImGui::DragFloat("Cell Size (m)", &m_NewSettings.CellSize, 1.0f, 8.0f, 4096.0f, "%.0f");
        ImGui::SameLine();
        if (ImGui::Button("Apply") && m_NewSettings.CellSize != world.Settings.CellSize)
        {
            WorldPartitionSettings settings = world.Settings;
            settings.CellSize = m_NewSettings.CellSize;
            scene.EnableWorldPartition(settings);
            m_Selection.clear();
        }
        ImGui::EndDisabled();
        if (!allLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Load every cell first (Load All): the next save then moves every entity into the cells of the new size.");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Cells: %s", world.CellDirectory.empty() ? "(written on the next save)" : world.CellDirectory.generic_string().c_str());
    }

    void WorldPartitionPanel::DrawToolbar(Scene& scene, const glm::dvec3& cameraPosition, bool editing)
    {
        const WorldPartition& world = scene.GetWorldPartition();
        size_t cells = 0, activated = 0, loaded = 0, loading = 0, entities = 0;
        for (const WorldCellCoord& cell : scene.GetLoadedWorldCells(true))
        {
            const Scene::WorldCellStage stage = scene.GetWorldCellStage(cell);
            if (stage == Scene::WorldCellStage::Activated)
                ++activated;
            else if (stage == Scene::WorldCellStage::Loaded)
                ++loaded;
            else
                ++loading;
        }
        for (const auto& [cell, info] : world.Cells)
        {
            if (cell.Grid != WorldGrid::WorldGridIndex)
                continue;
            ++cells;
            entities += info.Entities;
        }
        if (loaded > 0)
            ImGui::Text("%zu cells: %zu activated, %zu loaded (not in the world), %zu loading; %zu entities in cells", cells, activated, loaded,
                        loading, entities);
        else
            ImGui::Text("%zu cells, %zu loaded, %zu loading, %zu entities in cells", cells, activated, loading, entities);
        if (!editing)
            ImGui::TextDisabled("Play / Simulate: cells stream around the camera and streaming sources");

        ImGui::BeginDisabled(!editing);
        const float range = WorldUnits::FromMeters(world.Settings.LoadingRange);
        if (ImGui::Button("Load Around Camera"))
            scene.LoadWorldCells(scene.WorldCellsAround(cameraPosition, range));
        ImGui::SameLine();
        if (ImGui::Button("Load All"))
        {
            std::vector<WorldCellCoord> all;
            for (const auto& [cell, info] : world.Cells)
                if (cell.Grid == WorldGrid::WorldGridIndex)
                    all.push_back(cell);
            scene.LoadWorldCells(all);
        }
        ImGui::SameLine();
        if (ImGui::Button("Unload All"))
            RequestUnload(scene, scene.GetLoadedWorldCells(true));
        ImGui::BeginDisabled(m_Selection.empty());
        if (ImGui::Button("Load Selected"))
            scene.LoadWorldCells(std::vector<WorldCellCoord>(m_Selection.begin(), m_Selection.end()));
        ImGui::SameLine();
        if (ImGui::Button("Unload Selected"))
            RequestUnload(scene, std::vector<WorldCellCoord>(m_Selection.begin(), m_Selection.end()));
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Follow Camera", &m_FollowCamera);
        ImGui::SameLine();
        ImGui::Checkbox("Show in Viewport", &m_ShowInViewport);
    }

    void WorldPartitionPanel::DrawHLODs(Scene& scene, bool editing)
    {
        WorldPartition& world = scene.GetWorldPartition();
        size_t hlodCells = 0, hlods = 0;
        for (const auto& [cell, info] : world.Cells)
        {
            if (cell.Grid != WorldGrid::HLODGridIndex)
                continue;
            ++hlodCells;
            hlods += info.Entities;
        }
        const HLODBuilder* running = m_RunningBuild ? m_RunningBuild() : nullptr;
        if (running)
        {
            ImGui::ProgressBar(running->GetProgress(), ImVec2(-1.0f, 0.0f), running->GetStage().c_str());
            return;
        }
        if (!ImGui::CollapsingHeader("HLOD"))
        {
            ImGui::SameLine();
            ImGui::TextDisabled(hlods ? "%zu HLODs" : "none built", hlods);
            return;
        }
        // Unreal's World Partition HLODs: every cell's static meshes merged, simplified and baked into one mesh, shown while the cell is out.
        bool show = scene.GetShowHLODs();
        if (ImGui::Checkbox("Show HLODs", &show))
            scene.SetShowHLODs(show);
        ImGui::SetItemTooltip("While editing, the HLODs stream around the editor camera: the cells not loaded show as their HLODs.");
        ImGui::Text("%zu HLODs in %zu HLOD cells", hlods, hlodCells);
        ImGui::BeginDisabled(!editing);
        ImGui::DragFloat("HLOD Cell Size (m)", &world.Settings.HLODCellSize, 1.0f, 8.0f, 16384.0f, "%.0f");
        ImGui::DragFloat("HLOD Loading Range (m)", &world.Settings.HLODLoadingRange, 1.0f, 8.0f, 65536.0f, "%.0f");
        ImGui::SetItemTooltip("How far the HLODs stream: beyond the world's loading range, where the cells themselves are out.");
        ImGui::DragFloat("Simplification Error (m)", &world.Settings.HLODError, 0.01f, 0.01f, 100.0f, "%.2f");
        ImGui::SetItemTooltip("How far the HLOD's surface may stray from the cell's: half is the voxel shell's distance off the parts, half\n"
                              "the simplification (parts under twice this are left out). About a pixel\n"
                              "where the HLOD is first seen, the loading range: a pixel there is about loading range / 1000 at 1080p\n"
                              "(100 m: 0.1 m; 500 m: 0.5 m). Seen closer than that, any HLOD looks coarse.");
        int maxTriangles = static_cast<int>(world.Settings.HLODMaxTriangles);
        if (ImGui::DragInt("Max Triangles", &maxTriangles, 100.0f, 100, 1000000))
            world.Settings.HLODMaxTriangles = static_cast<uint32_t>(std::max(maxTriangles, 100));
        ImGui::DragFloat("Texels per Meter", &world.Settings.HLODTexelsPerMeter, 0.1f, 0.25f, 64.0f, "%.2f");
        ImGui::SetItemTooltip("The baked textures' density (power of two sizes 128-2048, the atlas filled).");
        ImGui::BeginDisabled(!m_BuildHLODs);
        if (ImGui::Button("Build HLODs"))
            m_BuildHLODs();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Saves the scene, builds an HLOD for every cell from its file (on workers; the editor stays usable), imports\n"
                              "them, writes the HLOD grid and saves again.");
        ImGui::EndDisabled();
    }

    void WorldPartitionPanel::DrawMap(Scene& scene, const glm::dvec3& cameraPosition, bool editing)
    {
        const WorldPartition& world = scene.GetWorldPartition();
        const float cellSize = WorldUnits::FromMeters(world.Settings.CellSize);
        const float range = WorldUnits::FromMeters(world.Settings.LoadingRange);
        const std::vector<WorldCellCoord> cells = AllCells(scene);

        ImVec2 size = ImGui::GetContentRegionAvail();
        size.x = std::max(size.x, 64.0f);
        size.y = std::max(size.y, 64.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##WorldMap", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(24, 24, 28, 255));

        // Fitted to the cells and the camera the first time.
        if (m_UnitsPerPixel <= 0.0f)
        {
            glm::vec2 low(cameraPosition.x, cameraPosition.z), high = low;
            for (const WorldCellCoord& cell : cells)
            {
                glm::vec2 rectMin, rectMax;
                WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                low = glm::min(low, rectMin);
                high = glm::max(high, rectMax);
            }
            low -= glm::vec2(range);
            high += glm::vec2(range);
            m_ViewCenter = (low + high) * 0.5f;
            m_UnitsPerPixel = std::max((high.x - low.x) / size.x, (high.y - low.y) / size.y);
            m_UnitsPerPixel = std::max(m_UnitsPerPixel, 1e-3f);
        }
        if (m_FollowCamera)
            m_ViewCenter = glm::vec2(cameraPosition.x, cameraPosition.z);

        const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
        auto toScreen = [&](const glm::vec2& world2) { return ImVec2(center.x + (world2.x - m_ViewCenter.x) / m_UnitsPerPixel, center.y + (world2.y - m_ViewCenter.y) / m_UnitsPerPixel); };
        auto toWorld = [&](const ImVec2& screen) { return glm::vec2(m_ViewCenter.x + (screen.x - center.x) * m_UnitsPerPixel, m_ViewCenter.y + (screen.y - center.y) * m_UnitsPerPixel); };

        // Zoom around the mouse, pan with the right / middle button.
        const ImGuiIO& io = ImGui::GetIO();
        if (hovered && io.MouseWheel != 0.0f)
        {
            const glm::vec2 before = toWorld(io.MousePos);
            m_UnitsPerPixel *= std::pow(0.85f, io.MouseWheel);
            m_UnitsPerPixel = std::clamp(m_UnitsPerPixel, 1e-3f, 1e7f);
            const glm::vec2 after = toWorld(io.MousePos);
            m_ViewCenter += before - after;
            m_FollowCamera = false;
        }
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
        {
            m_ViewCenter -= glm::vec2(io.MouseDelta.x, io.MouseDelta.y) * m_UnitsPerPixel;
            m_FollowCamera = false;
        }

        // The level 0 grid where it is fine enough to read.
        if (cellSize / m_UnitsPerPixel >= 12.0f)
        {
            const glm::vec2 low = toWorld(origin), high = toWorld(ImVec2(origin.x + size.x, origin.y + size.y));
            for (float x = std::floor(low.x / cellSize) * cellSize; x <= high.x; x += cellSize)
                draw->AddLine(toScreen({ x, low.y }), toScreen({ x, high.y }), IM_COL32(48, 48, 56, 255));
            for (float z = std::floor(low.y / cellSize) * cellSize; z <= high.y; z += cellSize)
                draw->AddLine(toScreen({ low.x, z }), toScreen({ high.x, z }), IM_COL32(48, 48, 56, 255));
        }

        // The cells: state as the fill, content bounds inside, selection outlined; the one under the mouse is the smallest there.
        const glm::vec2 mouseWorld = toWorld(io.MousePos);
        const WorldCellCoord* hoveredCell = nullptr;
        for (const WorldCellCoord& cell : cells)
        {
            glm::vec2 rectMin, rectMax;
            WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
            const Scene::WorldCellStage stage = scene.GetWorldCellStage(cell);
            const ImU32 fill = stage == Scene::WorldCellStage::Activated ? kLoadedFill
                             : stage == Scene::WorldCellStage::Loaded    ? kStagedFill
                             : stage == Scene::WorldCellStage::Unloaded  ? kUnloadedFill
                                                                         : kLoadingFill;
            const ImVec2 a = toScreen(rectMin), b = toScreen(rectMax);
            draw->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 1.0f), b, fill);
            const bool selected = m_Selection.contains(cell);
            draw->AddRect(a, b, selected ? kSelectedBorder : kCellBorder, 0.0f, 0, selected ? 2.0f : 1.0f);
            if (auto info = world.Cells.find(cell); info != world.Cells.end())
            {
                const ImVec2 boundsA = toScreen({ info->second.BoundsMin.x, info->second.BoundsMin.z });
                const ImVec2 boundsB = toScreen({ info->second.BoundsMax.x, info->second.BoundsMax.z });
                draw->AddRect(boundsA, ImVec2(std::max(boundsB.x, boundsA.x + 1.0f), std::max(boundsB.y, boundsA.y + 1.0f)), kBoundsColor);
                if (b.x - a.x > 70.0f && b.y - a.y > 30.0f)
                {
                    const std::string label = WorldGrid::CellName(cell) + "\n" + std::to_string(info->second.Entities);
                    draw->AddText(ImVec2(a.x + 4.0f, a.y + 3.0f), IM_COL32(230, 230, 235, 220), label.c_str());
                }
            }
            if (hovered && mouseWorld.x >= rectMin.x && mouseWorld.x < rectMax.x && mouseWorld.y >= rectMin.y && mouseWorld.y < rectMax.y &&
                (!hoveredCell || cell.Level <= hoveredCell->Level))
                hoveredCell = &cell;
        }

        // The camera and its loading range.
        const ImVec2 camera = toScreen({ cameraPosition.x, cameraPosition.z });
        draw->AddCircle(camera, range / m_UnitsPerPixel, kCameraColor, 64, 1.5f);
        draw->AddCircleFilled(camera, 4.0f, kCameraColor);

        // Selection: click a cell (Ctrl adds), drag a box, double-click loads it.
        if (editing && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_BoxSelecting = true;
            m_BoxStart = mouseWorld;
        }
        if (m_BoxSelecting)
        {
            const ImVec2 a = toScreen(m_BoxStart);
            draw->AddRect(a, io.MousePos, IM_COL32(255, 255, 255, 180));
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_BoxSelecting = false;
                if (!io.KeyCtrl)
                    m_Selection.clear();
                const glm::vec2 low = glm::min(m_BoxStart, mouseWorld), high = glm::max(m_BoxStart, mouseWorld);
                const bool click = glm::length(high - low) / m_UnitsPerPixel < 4.0f;
                if (click)
                {
                    if (hoveredCell)
                        m_Selection.insert(*hoveredCell);
                }
                else
                {
                    for (const WorldCellCoord& cell : cells)
                    {
                        glm::vec2 rectMin, rectMax;
                        WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                        if (rectMin.x < high.x && rectMax.x > low.x && rectMin.y < high.y && rectMax.y > low.y)
                            m_Selection.insert(cell);
                    }
                }
            }
        }
        if (editing && hovered && hoveredCell && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            scene.LoadWorldCells({ *hoveredCell });

        if (hoveredCell && !m_BoxSelecting)
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(WorldGrid::CellName(*hoveredCell).c_str());
            ImGui::TextDisabled("%s", StageName(scene.GetWorldCellStage(*hoveredCell)));
            if (auto info = world.Cells.find(*hoveredCell); info != world.Cells.end())
            {
                const glm::vec3 extent = glm::vec3(info->second.BoundsMax - info->second.BoundsMin);
                ImGui::Text("%u entities", info->second.Entities);
                ImGui::Text("Content %s x %s", WorldUnits::Format(extent.x).c_str(), WorldUnits::Format(extent.z).c_str());
            }
            else
                ImGui::TextDisabled("No file yet");
            if (editing)
                ImGui::TextDisabled("Double-click: load");
            ImGui::EndTooltip();
        }
        draw->PopClipRect();
    }

    void WorldPartitionPanel::RequestUnload(Scene& scene, std::vector<WorldCellCoord> cells)
    {
        std::erase_if(cells, [&](const WorldCellCoord& cell) { return scene.GetWorldCellState(cell) == Scene::LevelState::Unloaded; });
        if (cells.empty())
            return;
        // A cell whose entities changed since its file was written would lose those changes: saved first, if the user wants.
        for (const WorldCellCoord& cell : cells)
        {
            if (scene.GetWorldCellState(cell) == Scene::LevelState::Loaded && scene.IsWorldCellModified(cell))
            {
                m_PendingUnload = std::move(cells);
                m_OpenUnloadPrompt = true;
                return;
            }
        }
        scene.UnloadWorldCells(cells);
        for (const WorldCellCoord& cell : cells)
            m_Selection.erase(cell);
    }

    void WorldPartitionPanel::DrawUnloadPrompt(Scene& scene)
    {
        if (m_OpenUnloadPrompt)
        {
            ImGui::OpenPopup("Unload Cells");
            m_OpenUnloadPrompt = false;
        }
        if (!ImGui::BeginPopupModal("Unload Cells", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextUnformatted("Cells to unload have unsaved changes.");
        ImGui::TextDisabled("Unloading them would lose the changes: the scene is saved first.");
        if (ImGui::Button("Save and Unload") && m_Save)
        {
            m_Save();
            // Not saved after all (Save As cancelled, a write failed): the cells stay.
            bool saved = true;
            for (const WorldCellCoord& cell : m_PendingUnload)
                saved &= scene.GetWorldCellState(cell) != Scene::LevelState::Loaded || !scene.IsWorldCellModified(cell);
            if (saved)
            {
                scene.UnloadWorldCells(m_PendingUnload);
                for (const WorldCellCoord& cell : m_PendingUnload)
                    m_Selection.erase(cell);
            }
            m_PendingUnload.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingUnload.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
