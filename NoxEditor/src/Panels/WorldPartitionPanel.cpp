#include "WorldPartitionPanel.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

#include <SDL3/SDL_keyboard.h>

#include <NoxUI/Widgets/UIPanZoom.h>
#include <NoxUI/Widgets/UIPropertyRows.h>
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Scene/HLODBuilder.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Renderer/Font.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Core/UITheme.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIScrollView.h>

namespace Nox
{
    namespace
    {
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

        // The world's indexed cells (not the HLOD grid's) and the ones loaded without a file yet; big cells first (small ones drawn over).
        std::vector<WorldCellCoord> AllCells(const Scene& scene)
        {
            std::set<WorldCellCoord> cells;
            for (const auto& [cell, info] : scene.GetWorldPartition().Cells)
                if (cell.Grid == WorldGrid::WorldGridIndex)
                    cells.insert(cell);
            for (const WorldCellCoord& cell : scene.GetLoadedWorldCells(true))
                cells.insert(cell);
            std::vector<WorldCellCoord> ordered(cells.begin(), cells.end());
            std::stable_sort(ordered.begin(), ordered.end(), [](const WorldCellCoord& a, const WorldCellCoord& b) { return a.Level > b.Level; });
            return ordered;
        }

        glm::vec4 Token(const NoxUI::UITheme& theme, const char* name, const NoxUI::UIColor& fallback)
        {
            const NoxUI::UIColor c = theme.GetToken(name, fallback);
            return { c.r, c.g, c.b, c.a };
        }
    }

    // The top-down map of a world's cells: pan with the right or middle button, zoom with the wheel, click a cell (Ctrl adds), drag
    // a box, double-click loads the cell under the pointer. Colours are theme tokens (WorldCell*), with these defaults.
    class WorldPartitionMap final : public NoxUI::UIElement
    {
    public:
        std::function<Ref<Scene>()> GetScene;
        std::function<glm::dvec3()> GetCamera;
        std::function<bool()> Editing;
        bool* FollowCamera = nullptr;
        std::set<WorldCellCoord>* Selection = nullptr;

        WorldPartitionMap()
        {
            WidthMode = NoxUI::UISizeMode::Fill;
            HeightMode = NoxUI::UISizeMode::Fill;
            HitTestMode = NoxUI::UIHitTestMode::Self;
            ClipChildren = true;
        }
        void FitNextPaint() { m_NeedsFit = true; }

        bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
        {
            m_Pointer = e.Position;
            if (e.Button == 1)
            {
                const auto now = std::chrono::steady_clock::now();
                const bool doubleClick = now - m_LastClick < std::chrono::milliseconds(350);
                m_LastClick = now;
                if (doubleClick && Editing && Editing())
                {
                    if (const WorldCellCoord* cell = CellAt(e.Position))
                        if (Ref<Scene> scene = GetScene ? GetScene() : nullptr)
                            scene->LoadWorldCells({ *cell });
                    return true;
                }
                if (Editing && Editing())
                {
                    m_Boxing = true;
                    m_BoxStart = ToWorld(e.Position);
                }
                return true;
            }
            if (e.Button == 2 || e.Button == 3)
            {
                m_Panning = true;
                return true;
            }
            return UIElement::OnPointerDown(e);
        }

        bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
        {
            const NoxUI::UIVec2 delta = e.Position - m_Pointer;
            m_Pointer = e.Position;
            if (m_Panning)
            {
                m_View.Pan += delta;
                if (FollowCamera)
                    *FollowCamera = false;
            }
            UpdateTooltip();
            return true;
        }

        bool OnPointerUp(const NoxUI::UIPointerEvent& e) override
        {
            if (e.Button == 2 || e.Button == 3)
            {
                m_Panning = false;
                return true;
            }
            if (e.Button != 1 || !m_Boxing || !Selection)
                return UIElement::OnPointerUp(e);
            m_Boxing = false;
            Ref<Scene> scene = GetScene ? GetScene() : nullptr;
            if (!scene)
                return true;
            const bool ctrl = (e.Modifiers & NoxUI::UIMod::Ctrl) != 0;
            if (!ctrl)
                Selection->clear();
            const glm::vec2 end = ToWorld(e.Position);
            const glm::vec2 low = glm::min(m_BoxStart, end), high = glm::max(m_BoxStart, end);
            if (glm::length(high - low) * m_View.Zoom < 4.0f)
            {
                if (const WorldCellCoord* cell = CellAt(e.Position))
                    Selection->insert(*cell);
                return true;
            }
            const float cellSize = WorldUnits::FromMeters(scene->GetWorldPartition().Settings.CellSize);
            for (const WorldCellCoord& cell : AllCells(*scene))
            {
                glm::vec2 rectMin, rectMax;
                WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                if (rectMin.x < high.x && rectMax.x > low.x && rectMin.y < high.y && rectMax.y > low.y)
                    Selection->insert(cell);
            }
            return true;
        }

        bool OnMouseWheel(const NoxUI::UIPointerEvent& e) override
        {
            if (m_NeedsFit)
                return true;
            m_View.ZoomAt(GetBounds(), e.Position, e.ScrollDelta.y, 1.0f / 0.85f);
            if (FollowCamera)
                *FollowCamera = false;
            return true;
        }

    protected:
        NoxUI::UIVec2 ComputeDesiredSize() override { return { 300.0f, 320.0f }; }

        void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            const NoxUI::UIRect b = GetBounds();
            d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, Token(theme, "WorldMapBackground", { 0.094f, 0.094f, 0.11f, 1.0f }));
            Ref<Scene> scene = GetScene ? GetScene() : nullptr;
            if (!scene || b.Width() < 8.0f || b.Height() < 8.0f)
                return;
            const WorldPartition& world = scene->GetWorldPartition();
            const float cellSize = WorldUnits::FromMeters(world.Settings.CellSize);
            const float range = WorldUnits::FromMeters(world.Settings.LoadingRange);
            const glm::dvec3 camera = GetCamera ? GetCamera() : glm::dvec3(0.0);
            const std::vector<WorldCellCoord> cells = AllCells(*scene);

            if (m_NeedsFit)
            {
                glm::vec2 low(camera.x, camera.z), high = low;
                for (const WorldCellCoord& cell : cells)
                {
                    glm::vec2 rectMin, rectMax;
                    WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                    low = glm::min(low, rectMin);
                    high = glm::max(high, rectMax);
                }
                low -= glm::vec2(range);
                high += glm::vec2(range);
                m_View.Fit(b, low, high, m_View.MinZoom, m_View.MaxZoom);
                m_NeedsFit = false;
            }
            if (FollowCamera && *FollowCamera)
                m_View.CenterOn(b, { static_cast<float>(camera.x), static_cast<float>(camera.z) });

            d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });
            const glm::vec4 gridColor = Token(theme, "WorldMapGrid", { 0.19f, 0.19f, 0.22f, 1.0f });
            if (cellSize * m_View.Zoom >= 12.0f)
            {
                const glm::vec2 low = ToWorld(b.Min), high = ToWorld(b.Max);
                for (float x = std::floor(low.x / cellSize) * cellSize; x <= high.x; x += cellSize)
                {
                    const NoxUI::UIVec2 p = ToScreen({ x, 0.0f });
                    d.AddRoundedRect({ p.x, b.Min.y }, { p.x + 1.0f, b.Max.y }, gridColor);
                }
                for (float z = std::floor(low.y / cellSize) * cellSize; z <= high.y; z += cellSize)
                {
                    const NoxUI::UIVec2 p = ToScreen({ 0.0f, z });
                    d.AddRoundedRect({ b.Min.x, p.y }, { b.Max.x, p.y + 1.0f }, gridColor);
                }
            }

            const glm::vec4 unloaded = Token(theme, "WorldCellUnloaded", { 0.27f, 0.27f, 0.30f, 0.43f });
            const glm::vec4 loading = Token(theme, "WorldCellLoading", { 0.90f, 0.67f, 0.16f, 0.59f });
            const glm::vec4 activated = Token(theme, "WorldCellActivated", { 0.24f, 0.67f, 0.35f, 0.55f });
            const glm::vec4 staged = Token(theme, "WorldCellLoaded", { 0.27f, 0.51f, 0.82f, 0.55f });
            const glm::vec4 border = Token(theme, "WorldCellBorder", { 0.59f, 0.59f, 0.63f, 0.78f });
            const glm::vec4 selected = Token(theme, "WorldCellSelected", { 1.0f, 1.0f, 1.0f, 1.0f });
            const glm::vec4 bounds = Token(theme, "WorldCellBounds", { 0.47f, 0.78f, 1.0f, 0.63f });
            for (const WorldCellCoord& cell : cells)
            {
                glm::vec2 rectMin, rectMax;
                WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                const Scene::WorldCellStage stage = scene->GetWorldCellStage(cell);
                const glm::vec4 fill = stage == Scene::WorldCellStage::Activated ? activated
                                     : stage == Scene::WorldCellStage::Loaded    ? staged
                                     : stage == Scene::WorldCellStage::Unloaded  ? unloaded
                                                                                 : loading;
                const NoxUI::UIVec2 a = ToScreen(rectMin), c = ToScreen(rectMax);
                const bool isSelected = Selection && Selection->contains(cell);
                d.AddRoundedRect({ a.x, a.y }, { c.x, c.y }, fill, glm::vec4(0.0f), isSelected ? 2.0f : 1.0f, isSelected ? selected : border);
                if (auto info = world.Cells.find(cell); info != world.Cells.end())
                {
                    const NoxUI::UIVec2 ba = ToScreen({ static_cast<float>(info->second.BoundsMin.x), static_cast<float>(info->second.BoundsMin.z) });
                    const NoxUI::UIVec2 bb = ToScreen({ static_cast<float>(info->second.BoundsMax.x), static_cast<float>(info->second.BoundsMax.z) });
                    d.AddRoundedRect({ ba.x, ba.y }, { std::max(bb.x, ba.x + 1.0f), std::max(bb.y, ba.y + 1.0f) }, glm::vec4(0.0f), glm::vec4(0.0f), 1.0f, bounds);
                    if (c.x - a.x > 70.0f && c.y - a.y > 34.0f)
                    {
                        d.AddText(WorldGrid::CellName(cell), nullptr, { a.x + 4.0f, a.y + 3.0f }, 12.0f, NoxUI::ThemeColor("WorldCellName", { 0.9f, 0.9f, 0.92f, 0.86f }));
                        d.AddText(std::to_string(info->second.Entities) + " entities", nullptr, { a.x + 4.0f, a.y + 17.0f }, 12.0f, NoxUI::ThemeColor("WorldCellCount", { 0.9f, 0.9f, 0.92f, 0.7f }));
                    }
                }
            }

            // The camera and its loading range (a circle: a rounded rect as round as it is wide).
            const glm::vec4 cameraColor = Token(theme, "WorldMapCamera", { 1.0f, 0.43f, 0.24f, 1.0f });
            const NoxUI::UIVec2 cam = ToScreen({ static_cast<float>(camera.x), static_cast<float>(camera.z) });
            const float radius = range * m_View.Zoom;
            d.AddRoundedRect({ cam.x - radius, cam.y - radius }, { cam.x + radius, cam.y + radius }, glm::vec4(0.0f), glm::vec4(radius), 1.5f, cameraColor);
            d.AddRoundedRect({ cam.x - 4.0f, cam.y - 4.0f }, { cam.x + 4.0f, cam.y + 4.0f }, cameraColor, glm::vec4(4.0f));

            if (m_Boxing)
            {
                const NoxUI::UIVec2 a = ToScreen(m_BoxStart);
                d.AddRoundedRect({ std::min(a.x, m_Pointer.x), std::min(a.y, m_Pointer.y) }, { std::max(a.x, m_Pointer.x), std::max(a.y, m_Pointer.y) },
                                 NoxUI::ThemeColor("WorldMapBoxFill", { 1.0f, 1.0f, 1.0f, 0.06f }), glm::vec4(0.0f), 1.0f, NoxUI::ThemeColor("WorldMapBoxBorder", { 1.0f, 1.0f, 1.0f, 0.7f }));
            }
            d.PopClipRect();
        }

    private:
        // World x / z to the map and back (UI::UIPanZoom: zoom is pixels per world unit).
        NoxUI::UIVec2 ToScreen(const glm::vec2& world) const { return m_View.ToScreen(GetBounds(), world); }
        glm::vec2 ToWorld(const NoxUI::UIVec2& screen) const { return m_View.ToGraph(GetBounds(), screen); }

        // The smallest cell under the point.
        const WorldCellCoord* CellAt(const NoxUI::UIVec2& screen)
        {
            Ref<Scene> scene = GetScene ? GetScene() : nullptr;
            if (!scene || m_NeedsFit)
                return nullptr;
            const float cellSize = WorldUnits::FromMeters(scene->GetWorldPartition().Settings.CellSize);
            const glm::vec2 point = ToWorld(screen);
            m_HitCells = AllCells(*scene);
            const WorldCellCoord* best = nullptr;
            for (const WorldCellCoord& cell : m_HitCells)
            {
                glm::vec2 rectMin, rectMax;
                WorldGrid::CellRect(cell, cellSize, rectMin, rectMax);
                if (point.x >= rectMin.x && point.x < rectMax.x && point.y >= rectMin.y && point.y < rectMax.y && (!best || cell.Level <= best->Level))
                    best = &cell;
            }
            return best;
        }

        void UpdateTooltip()
        {
            Ref<Scene> scene = GetScene ? GetScene() : nullptr;
            const WorldCellCoord* cell = m_Boxing ? nullptr : CellAt(m_Pointer);
            if (!scene || !cell)
            {
                Tooltip = "Right / middle drag: pan. Wheel: zoom. Click a cell (Ctrl adds), drag a box; double-click: load.";
                return;
            }
            std::string text = WorldGrid::CellName(*cell) + "\n" + StageName(scene->GetWorldCellStage(*cell));
            const WorldPartition& world = scene->GetWorldPartition();
            if (auto info = world.Cells.find(*cell); info != world.Cells.end())
            {
                const glm::vec3 extent = glm::vec3(info->second.BoundsMax - info->second.BoundsMin);
                text += "\n" + std::to_string(info->second.Entities) + " entities\nContent " + WorldUnits::Format(extent.x) + " x " + WorldUnits::Format(extent.z);
            }
            else
                text += "\nNo file yet";
            if (Editing && Editing())
                text += "\nDouble-click: load";
            Tooltip = std::move(text);
        }

        NoxUI::UIPanZoom m_View{ {}, 1.0f, 1e-7f, 1e3f }; // pixels per world unit within 1e-7 .. 1000 (the wheel: 1/0.85 a notch)
        bool m_NeedsFit = true;                         // fitted to the cells and the camera on the next paint
        bool m_Panning = false;
        bool m_Boxing = false;
        glm::vec2 m_BoxStart{ 0.0f };
        NoxUI::UIVec2 m_Pointer{ 0.0f };
        std::chrono::steady_clock::time_point m_LastClick{};
        std::vector<WorldCellCoord> m_HitCells;
    };

    void WorldPartitionPanel::Changed() const
    {
        if (m_Hooks.SceneChanged)
            m_Hooks.SceneChanged();
    }

    const Ref<NoxUI::UIElement>& WorldPartitionPanel::BuildUI(NoxUI::UIContext* context)
    {
        m_Context = context;
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 0.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = NoxUI::UIThickness::All(8.0f);
        m_Body = root;
        m_Root = root;
        m_Rebuild = true;
        Update();
        return m_Root;
    }

    uint64_t WorldPartitionPanel::Signature() const
    {
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        uint64_t h = reinterpret_cast<uintptr_t>(scene.get()) * 0x9E3779B97F4A7C15ull;
        if (scene)
            h ^= (scene->IsWorldPartitioned() ? 5ull : 7ull) + (h << 6) + (h >> 2);
        h ^= (m_Hooks.Editing && m_Hooks.Editing() ? 11ull : 13ull) + (h << 6) + (h >> 2);
        h ^= (m_Hooks.RunningBuild && m_Hooks.RunningBuild() ? 17ull : 19ull) + (h << 6) + (h >> 2);
        return h;
    }

    void WorldPartitionPanel::Update()
    {
        if (!m_Body)
            return;
        const uint64_t signature = Signature();
        if (m_Rebuild || signature != m_Signature)
        {
            m_Signature = signature;
            m_Rebuild = false;
            Rebuild();
        }
    }

    void WorldPartitionPanel::Rebuild()
    {
        m_Body->ClearChildren();
        m_Map = nullptr;
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        NoxUI::UIPropertyRows rows(m_Body, m_Context);
        if (!scene)
        {
            rows.Note("No scene");
            return;
        }
        // Another scene: its settings in the fields, the selection dropped.
        if (m_SettingsScene != scene.get())
        {
            m_SettingsScene = scene.get();
            m_NewSettings = scene->GetWorldPartition().Settings.Enabled ? scene->GetWorldPartition().Settings : WorldPartitionSettings();
            m_Selection.clear();
        }
        const bool editing = m_Hooks.Editing && m_Hooks.Editing();
        std::function<Ref<Scene>()> getScene = m_Hooks.ActiveScene;
        WorldPartitionSettings* fields = &m_NewSettings;

        if (!scene->IsWorldPartitioned())
        {
            rows.Note("This scene is not a world. Converted, its entities with something in the world (meshes, models, prefabs, local "
                      "lights, bodies ...) are kept in grid cells, each in its own file next to the scene, and streamed in and out around "
                      "the camera in Play. Sky, sun, cameras, the player and logic stay in the scene file. The cell files are written on the "
                      "next save.");
            rows.Note("Each root entity goes into one cell. A model placed as one instance would stream as one piece: split it first "
                      "(Inspector > Model Instance > Split Into Separate Entities).");
            NoxUI::UIPropertyRows convert = rows.Group();
            convert.Drag("Cell Size (m)", [fields]() { return fields->CellSize; }, [fields](float v) { fields->CellSize = v; }, 1.0f, 8.0f, 4096.0f, "{:.0f}");
            convert.Drag("Loading Range (m)", [fields]() { return fields->LoadingRange; }, [fields](float v) { fields->LoadingRange = v; }, 1.0f, 8.0f, 16384.0f, "{:.0f}");
            convert.Button("Convert to World Partition", [this, getScene, fields]()
            {
                if (Ref<Scene> s = getScene())
                {
                    s->EnableWorldPartition(*fields);
                    Changed();
                    m_Rebuild = true;
                }
            });
            rows.EnableWhen(convert.GetBody(), [editing]() { return editing; }, []() { return std::string("Play / Simulate"); });
            return;
        }

        // ---- Settings ----
        {
            NoxUI::UIPropertyRows settings = rows.Section("Settings", false, &m_Expanded);
            settings.Drag("Loading Range (m)", [getScene]() { Ref<Scene> s = getScene(); return s ? s->GetWorldPartition().Settings.LoadingRange : 0.0f; },
                [this, getScene, fields](float v)
                {
                    if (Ref<Scene> s = getScene())
                    {
                        s->GetWorldPartition().Settings.LoadingRange = v;
                        fields->LoadingRange = v;
                        Changed();
                    }
                }, 1.0f, 8.0f, 16384.0f, "{:.0f}", "How far from a streaming source cells load. Changes only what streams: any time.");
            // A new cell size moves every entity to another cell, which the save can only do with all of them in memory.
            const auto allLoaded = [getScene]()
            {
                Ref<Scene> s = getScene();
                if (!s)
                    return false;
                for (const auto& [cell, info] : s->GetWorldPartition().Cells)
                    if (cell.Grid == WorldGrid::WorldGridIndex && s->GetWorldCellState(cell) != Scene::LevelState::Loaded)
                        return false;
                return true;
            };
            NoxUI::UIPropertyRows size = settings.Group();
            size.Drag("Cell Size (m)", [fields]() { return fields->CellSize; }, [fields](float v) { fields->CellSize = v; }, 1.0f, 8.0f, 4096.0f, "{:.0f}");
            size.Button("Apply Cell Size", [this, getScene, fields]()
            {
                Ref<Scene> s = getScene();
                if (!s || fields->CellSize == s->GetWorldPartition().Settings.CellSize)
                    return;
                WorldPartitionSettings next = s->GetWorldPartition().Settings;
                next.CellSize = fields->CellSize;
                s->EnableWorldPartition(next);
                m_Selection.clear();
                Changed();
            }, "The next save moves every entity into the cells of the new size.");
            settings.EnableWhen(size.GetBody(), [=]() { return editing && allLoaded(); },
                                [=]() { return std::string(editing ? "load every cell first (Load All)" : "Play / Simulate"); });
            settings.Text([getScene]()
            {
                Ref<Scene> s = getScene();
                if (!s)
                    return std::string();
                const auto& directory = s->GetWorldPartition().CellDirectory;
                return "Cells: " + (directory.empty() ? std::string("(written on the next save)") : directory.generic_string());
            });
            rows.EnableWhen(settings.GetBody(), [editing]() { return editing; }, []() { return std::string("Play / Simulate"); });
        }

        // ---- HLOD ----
        if (const HLODBuilder* running = m_Hooks.RunningBuild ? m_Hooks.RunningBuild() : nullptr)
        {
            (void)running;
            std::function<const HLODBuilder*()> getBuild = m_Hooks.RunningBuild;
            rows.Text([getBuild]()
            {
                const HLODBuilder* build = getBuild ? getBuild() : nullptr;
                if (!build)
                    return std::string();
                char text[256];
                std::snprintf(text, sizeof(text), "Building HLODs: %.0f %%  %s", build->GetProgress() * 100.0f, build->GetStage().c_str());
                return std::string(text);
            }, NoxUI::ThemeColor("StatusInfo", { 0.45f, 0.78f, 1.0f, 1.0f }));
        }
        else
        {
            NoxUI::UIPropertyRows hlod = rows.Section("HLOD", false, &m_Expanded,
                "Unreal's World Partition HLODs: every cell's static meshes merged, simplified and baked into one mesh, shown while the cell is out.");
            hlod.Check("Show HLODs", [getScene]() { Ref<Scene> s = getScene(); return s && s->GetShowHLODs(); },
                       [getScene](bool v) { if (Ref<Scene> s = getScene()) s->SetShowHLODs(v); },
                       "While editing, the HLODs stream around the editor camera: the cells not loaded show as their HLODs.");
            hlod.Text([getScene]()
            {
                Ref<Scene> s = getScene();
                size_t cells = 0, hlods = 0;
                if (s)
                    for (const auto& [cell, info] : s->GetWorldPartition().Cells)
                        if (cell.Grid == WorldGrid::HLODGridIndex)
                        {
                            ++cells;
                            hlods += info.Entities;
                        }
                return hlods ? std::to_string(hlods) + " HLODs in " + std::to_string(cells) + " HLOD cells" : std::string("No HLODs built yet");
            });
            NoxUI::UIPropertyRows build = hlod.Group();
            const auto settingRow = [&build, getScene, this](const std::string& label, float WorldPartitionSettings::*field, float speed, float min, float max,
                                                            const char* format, const std::string& tooltip)
            {
                build.Drag(label, [getScene, field]() { Ref<Scene> s = getScene(); return s ? s->GetWorldPartition().Settings.*field : 0.0f; },
                           [this, getScene, field](float v) { if (Ref<Scene> s = getScene()) { s->GetWorldPartition().Settings.*field = v; Changed(); } },
                           speed, min, max, format, tooltip);
            };
            settingRow("HLOD Cell Size (m)", &WorldPartitionSettings::HLODCellSize, 1.0f, 8.0f, 16384.0f, "{:.0f}", {});
            settingRow("HLOD Loading Range (m)", &WorldPartitionSettings::HLODLoadingRange, 1.0f, 8.0f, 65536.0f, "{:.0f}",
                       "How far the HLODs stream: beyond the world's loading range, where the cells themselves are out.");
            settingRow("Simplification Error (m)", &WorldPartitionSettings::HLODError, 0.01f, 0.01f, 100.0f, "{:.2f}",
                       "How far the HLOD's surface may stray from the cell's. About a pixel where the HLOD is first seen: a pixel there is about "
                       "loading range / 1000 at 1080p (100 m: 0.1 m; 500 m: 0.5 m).");
            build.Drag("Max Triangles", [getScene]() { Ref<Scene> s = getScene(); return s ? static_cast<float>(s->GetWorldPartition().Settings.HLODMaxTriangles) : 0.0f; },
                       [this, getScene](float v) { if (Ref<Scene> s = getScene()) { s->GetWorldPartition().Settings.HLODMaxTriangles = static_cast<uint32_t>(std::max(v, 100.0f)); Changed(); } },
                       100.0f, 100.0f, 1000000.0f, "{:.0f}");
            settingRow("Texels per Meter", &WorldPartitionSettings::HLODTexelsPerMeter, 0.1f, 0.25f, 64.0f, "{:.2f}",
                       "The baked textures' density (power of two sizes 128-2048, the atlas filled).");
            std::function<void()> buildHLODs = m_Hooks.BuildHLODs;
            build.Button("Build HLODs", [buildHLODs]() { if (buildHLODs) buildHLODs(); },
                         "Saves the scene, builds an HLOD for every cell from its file (on workers; the editor stays usable), imports them, "
                         "writes the HLOD grid and saves again.");
            hlod.EnableWhen(build.GetBody(), [editing]() { return editing; }, []() { return std::string("Play / Simulate"); });
        }

        // ---- Cells ----
        rows.Text([getScene]()
        {
            Ref<Scene> s = getScene();
            if (!s)
                return std::string();
            size_t cells = 0, activated = 0, loaded = 0, loading = 0, entities = 0;
            for (const WorldCellCoord& cell : s->GetLoadedWorldCells(true))
            {
                const Scene::WorldCellStage stage = s->GetWorldCellStage(cell);
                if (stage == Scene::WorldCellStage::Activated)
                    ++activated;
                else if (stage == Scene::WorldCellStage::Loaded)
                    ++loaded;
                else
                    ++loading;
            }
            for (const auto& [cell, info] : s->GetWorldPartition().Cells)
                if (cell.Grid == WorldGrid::WorldGridIndex)
                {
                    ++cells;
                    entities += info.Entities;
                }
            char text[200];
            if (loaded > 0)
                std::snprintf(text, sizeof(text), "%zu cells: %zu activated, %zu loaded (not in the world), %zu loading; %zu entities in cells", cells, activated, loaded, loading, entities);
            else
                std::snprintf(text, sizeof(text), "%zu cells, %zu loaded, %zu loading, %zu entities in cells", cells, activated, loading, entities);
            return std::string(text);
        }, NoxUI::ThemeColor("WorldPartitionSummary", { 0.85f, 0.88f, 0.94f, 1.0f }));
        if (!editing)
            rows.Note("Play / Simulate: cells stream around the camera and streaming sources.");

        auto toolbar = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 6.0f);
        toolbar->WidthMode = NoxUI::UISizeMode::Fill;
        std::function<glm::dvec3()> camera = m_Hooks.StreamingSource;
        const auto addButton = [&toolbar](const char* label, std::function<void()> click, const std::string& tooltip = {})
        {
            Ref<NoxUI::UIButton> button(new NoxUI::UIButton(label, std::move(click)));
            button->Tooltip = tooltip;
            toolbar->AddChild(button);
            return button;
        };
        addButton("Load Around Camera", [getScene, camera]()
        {
            if (Ref<Scene> s = getScene())
                s->LoadWorldCells(s->WorldCellsAround(camera ? camera() : glm::dvec3(0.0), WorldUnits::FromMeters(s->GetWorldPartition().Settings.LoadingRange)));
        });
        addButton("Load All", [getScene]()
        {
            Ref<Scene> s = getScene();
            if (!s)
                return;
            std::vector<WorldCellCoord> all;
            for (const auto& [cell, info] : s->GetWorldPartition().Cells)
                if (cell.Grid == WorldGrid::WorldGridIndex)
                    all.push_back(cell);
            s->LoadWorldCells(all);
        });
        addButton("Unload All", [this, getScene]() { if (Ref<Scene> s = getScene()) RequestUnload(s->GetLoadedWorldCells(true)); });
        Ref<NoxUI::UIButton> loadSelected = addButton("Load Selected", [this, getScene]()
        {
            if (Ref<Scene> s = getScene())
                s->LoadWorldCells(std::vector<WorldCellCoord>(m_Selection.begin(), m_Selection.end()));
        });
        Ref<NoxUI::UIButton> unloadSelected = addButton("Unload Selected", [this]() { RequestUnload(std::vector<WorldCellCoord>(m_Selection.begin(), m_Selection.end())); });
        NoxUI::AppendSync(*toolbar, [this, loadSelected, unloadSelected]() // (on their row: it holds them anyway)
        {
            loadSelected->SetEnabled(!m_Selection.empty());
            unloadSelected->SetEnabled(!m_Selection.empty());
        });
        toolbar->VeilWhenDisabled = true;
        toolbar->SetEnabled(editing);
        m_Body->AddChild(toolbar);

        auto options = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Horizontal, 12.0f);
        Ref<NoxUI::UICheckBox> follow(new NoxUI::UICheckBox("Follow Camera", m_FollowCamera, [this](bool v) { m_FollowCamera = v; }));
        Ref<NoxUI::UICheckBox> show(new NoxUI::UICheckBox("Show in Viewport", m_ShowInViewport, [this](bool v) { m_ShowInViewport = v; }));
        show->Tooltip = "The loaded cells' content bounds, in green, in the viewports.";
        options->AddChild(follow);
        options->AddChild(show);
        m_Body->AddChild(options);
        NoxUI::AppendSync(*options, [this, follow]() { follow->Checked = m_FollowCamera; }); // (panning the map turns it off)

        Ref<WorldPartitionMap> map(new WorldPartitionMap());
        map->GetScene = getScene;
        map->GetCamera = camera;
        map->Editing = m_Hooks.Editing;
        map->FollowCamera = &m_FollowCamera;
        map->Selection = &m_Selection;
        m_Map = map;
        m_Body->AddChild(m_Map);
    }

    void WorldPartitionPanel::RequestUnload(std::vector<WorldCellCoord> cells)
    {
        const Ref<Scene> scene = m_Hooks.ActiveScene ? m_Hooks.ActiveScene() : nullptr;
        if (!scene)
            return;
        std::erase_if(cells, [&](const WorldCellCoord& cell) { return scene->GetWorldCellState(cell) == Scene::LevelState::Unloaded; });
        if (cells.empty())
            return;
        std::function<Ref<Scene>()> getScene = m_Hooks.ActiveScene;
        const auto unload = [this, getScene, cells]()
        {
            Ref<Scene> s = getScene();
            if (!s)
                return;
            s->UnloadWorldCells(cells);
            for (const WorldCellCoord& cell : cells)
                m_Selection.erase(cell);
        };
        const bool modified = std::any_of(cells.begin(), cells.end(), [&](const WorldCellCoord& cell)
        {
            return scene->GetWorldCellState(cell) == Scene::LevelState::Loaded && scene->IsWorldCellModified(cell);
        });
        if (!modified || !m_Hooks.AskSave)
        {
            unload();
            return;
        }
        // The editor's question: Save (then unload; not saved after all -- Save As cancelled -- the cells stay), Don't Save (unload,
        // the changes to those cells are dropped), Cancel. The old panel had no Don't Save.
        m_Hooks.AskSave("Some of the cells to unload have unsaved changes. Save the scene before unloading them?",
            [this, getScene, cells, unload]()
            {
                if (m_Hooks.Save)
                    m_Hooks.Save();
                Ref<Scene> s = getScene();
                const bool saved = s && std::none_of(cells.begin(), cells.end(), [&](const WorldCellCoord& cell)
                {
                    return s->GetWorldCellState(cell) == Scene::LevelState::Loaded && s->IsWorldCellModified(cell);
                });
                if (saved)
                    unload();
            },
            unload);
    }
}
