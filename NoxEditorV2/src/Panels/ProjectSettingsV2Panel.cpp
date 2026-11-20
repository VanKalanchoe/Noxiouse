#include "ProjectSettingsV2Panel.h"

#include <algorithm>

#include "Panels/SettingsRowsV2.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Project/ProjectRendererSettings.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox
{
    namespace
    {
        ProjectConfig* Config()
        {
            return Project::GetActive() ? &Project::GetActive()->GetConfig() : nullptr;
        }

        // The scenes of the project, sorted by path, for the Start Scene choice ("(none)" first).
        std::vector<std::pair<std::string, AssetHandle>> Scenes()
        {
            std::vector<std::pair<std::string, AssetHandle>> scenes;
            if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
                for (const auto& [handle, metadata] : assets->GetAssetRegistry())
                    if (metadata.Type == AssetType::Scene)
                        scenes.emplace_back(metadata.FilePath.generic_string(), handle);
            std::sort(scenes.begin(), scenes.end());
            return scenes;
        }

        constexpr Renderer::RenderQuality kQualities[] = { Renderer::RenderQuality::Low, Renderer::RenderQuality::Medium, Renderer::RenderQuality::High,
                                                           Renderer::RenderQuality::RayTracedAll, Renderer::RenderQuality::RasterAll };
    }

    const Ref<UI::UIElement>& ProjectSettingsV2Panel::BuildUI(UI::UIContext* context)
    {
        m_Context = context;
        m_SurfaceFields.clear();
        m_Rebuild = false;
        // The root stays (it is in a window); built again, only what is in it changes.
        Ref<UI::UIStackBox> root = m_Root ? Ref<UI::UIStackBox>(static_cast<UI::UIStackBox*>(m_Root.get())) : UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 0.0f);
        root->ClearChildren();
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        auto scroll = UI::MakeUI<UI::UIScrollView>();
        scroll->WidthMode = UI::UISizeMode::Fill;
        scroll->HeightMode = UI::UISizeMode::Fill;
        root->AddChild(scroll);
        m_Body = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        m_Body->WidthMode = UI::UISizeMode::Fill;
        m_Body->Padding = UI::UIThickness::All(8.0f);
        scroll->AddChild(m_Body);
        m_Root = root;

        ProjectConfig* config = Config();
        Renderer* r = m_Hooks.Render;
        if (!config || !r)
            return m_Root;
        SettingsRowsV2 rows(m_Body, context);
        rows.Note("Saved in the project file at each change.");

        // ---- Maps ----
        {
            SettingsRowsV2 maps = rows.Section("Maps", true, &m_Expanded);
            // The list is read when the row is made; a scene saved later shows after the panel is opened again.
            const auto scenes = Scenes();
            std::vector<std::string> names{ "(none: an empty scene)" };
            for (const auto& scene : scenes)
                names.push_back(scene.first);
            maps.Choice("Start Scene", std::move(names),
                [scenes]()
                {
                    const ProjectConfig* c = Config();
                    if (!c || c->StartScene == 0)
                        return 0;
                    for (size_t i = 0; i < scenes.size(); ++i)
                        if (scenes[i].second == c->StartScene)
                            return static_cast<int>(i) + 1;
                    return 0;
                },
                [scenes](int index)
                {
                    if (ProjectConfig* c = Config())
                    {
                        c->StartScene = index == 0 ? AssetHandle(0) : scenes[static_cast<size_t>(index - 1)].second;
                        Project::SaveActive();
                    }
                },
                "The scene the editor opens this project with, and the packaged game starts in (Unreal's Editor Startup Map / Game Default Map).");
            if (scenes.empty())
                maps.Note("No scenes yet: save one (File > Save Scene As...).");
        }

        // ---- Rendering ----
        {
            SettingsRowsV2 rendering = rows.Section("Rendering", true, &m_Expanded);
            std::vector<std::string> names{ std::string("Auto (") + Renderer::GetQualityName(r->GetRecommendedQuality()) + " for this GPU)" };
            for (Renderer::RenderQuality quality : kQualities)
                names.push_back(Renderer::GetQualityName(quality));
            names.push_back("Custom (keep the settings, no preset at start)");
            Hooks hooks = m_Hooks;
            rendering.Choice("Render Quality", std::move(names),
                []()
                {
                    const ProjectConfig* c = Config();
                    if (!c || c->RenderQuality == "Auto")
                        return 0;
                    for (size_t i = 0; i < std::size(kQualities); ++i)
                        if (c->RenderQuality == Renderer::GetQualityName(kQualities[i]))
                            return static_cast<int>(i) + 1;
                    return static_cast<int>(std::size(kQualities)) + 1;
                },
                [r, hooks](int index)
                {
                    ProjectConfig* c = Config();
                    if (!c)
                        return;
                    if (index == 0)
                    {
                        c->RenderQuality = "Auto";
                        if (hooks.ApplyQuality)
                            hooks.ApplyQuality(static_cast<int>(r->GetRecommendedQuality()));
                    }
                    else if (index <= static_cast<int>(std::size(kQualities)))
                    {
                        const Renderer::RenderQuality quality = kQualities[static_cast<size_t>(index - 1)];
                        c->RenderQuality = Renderer::GetQualityName(quality);
                        if (hooks.ApplyQuality)
                            hooks.ApplyQuality(static_cast<int>(quality));
                    }
                    else
                        c->RenderQuality = "Custom";
                    Project::SaveActive();
                },
                "Each preset sets every feature on or off. Low: RT sun shadows, screen-probe GI (24 px), SSAO, DLSS Performance. Medium: + RT "
                "reflections, probes 16 px, DLSS Balanced. High: + RTAO, RT glass, DLSS Quality. Ray Traced (All): High with every light "
                "shadowed, Ray Reconstruction, Auto Exposure, OIT. Raster (All): raster except reflections. The project opens with it; "
                "changing a setting in Renderer Settings makes it Custom for this session.");
            rendering.Text([r]() { return std::string("The renderer's settings now equal: ") + Renderer::GetQualityName(r->GetCurrentQuality()); });

            rendering.Header("Saved on top of the preset", "The .nproj's Renderer settings: the editor and the packaged game start with them.");
            ProjectRendererSettings* saved = &config->Renderer;
            const auto choice = [&rendering, saved, r](const std::string& label, int32_t ProjectRendererSettings::*field,
                                                      std::vector<std::pair<int32_t, const char*>> options)
            {
                std::vector<std::string> names{ "Preset's" };
                for (const auto& option : options)
                    names.push_back(option.second);
                rendering.Choice(label, std::move(names),
                    [saved, field, options]()
                    {
                        for (size_t i = 0; i < options.size(); ++i)
                            if (options[i].first == saved->*field)
                                return static_cast<int>(i) + 1;
                        return 0;
                    },
                    [saved, field, options, r](int index)
                    {
                        saved->*field = index == 0 ? -1 : options[static_cast<size_t>(index - 1)].first;
                        r->ApplyProjectRendererSettings(*saved);
                        Project::SaveActive();
                    });
            };
            choice("Ray Tracing Resolution", &ProjectRendererSettings::RayTracingResolution, { { 1, "Full" }, { 2, "Half" } });
            choice("Diffuse GI", &ProjectRendererSettings::DiffuseGI, { { 0, "Environment (IBL)" }, { 1, "DDGI" }, { 2, "ReSTIR GI" }, { 3, "Screen Probes" }, { 4, "Raster Probe Volume" } });
            choice("Shadowed Lights", &ProjectRendererSettings::ShadowedLights, { { 0, "All" }, { 1, "1" }, { 2, "2" }, { 4, "4" }, { 8, "8" } });
            choice("Shadows", &ProjectRendererSettings::Shadows, { { 0, "Off" }, { 1, "On" } });
            choice("Sun Shadows", &ProjectRendererSettings::SunShadows, { { 0, "Ray Traced" }, { 1, "Shadow Maps" } });
            choice("Local Light Shadows", &ProjectRendererSettings::LocalShadows, { { 0, "Ray Traced" }, { 1, "Shadow Maps" } });
            choice("Contact Shadows", &ProjectRendererSettings::ContactShadows, { { 0, "Off" }, { 1, "On" } });
            choice("Screen-Space AO", &ProjectRendererSettings::SSAOMethod, { { 0, "Classic SSAO" }, { 1, "GTAO" } });
            choice("Screen-Space Reflections", &ProjectRendererSettings::SSR, { { 0, "Off" }, { 1, "On" } });
            choice("Screen-Space GI", &ProjectRendererSettings::SSGI, { { 0, "Off" }, { 1, "On" } });
            choice("Volumetric Fog", &ProjectRendererSettings::VolumetricFog, { { 0, "Off" }, { 1, "On" } });
            choice("Bloom", &ProjectRendererSettings::Bloom, { { 0, "Off" }, { 1, "On" } });
            choice("Depth of Field", &ProjectRendererSettings::DepthOfField, { { 0, "Off" }, { 1, "On" } });
            choice("Motion Blur", &ProjectRendererSettings::MotionBlur, { { 0, "Off" }, { 1, "On" } });
            choice("Order-Independent Transparency", &ProjectRendererSettings::OIT, { { 0, "Off" }, { 1, "Weighted Blended" }, { 2, "AVBOIT" } });
            choice("Temporal Anti-Aliasing", &ProjectRendererSettings::TAA, { { 0, "Off" }, { 1, "On" } });
        }

        // ---- Packaged game ----
        {
            SettingsRowsV2 game = rows.Section("Packaged Game", false, &m_Expanded);
            static const char* modes[] = { "Windowed", "Borderless", "Fullscreen" };
            game.Choice("Game Window", { modes[0], modes[1], modes[2] },
                []() { const ProjectConfig* c = Config(); return c ? static_cast<int>(ParseWindowMode(c->WindowMode)) : 1; },
                [](int index) { if (ProjectConfig* c = Config()) { c->WindowMode = modes[index]; Project::SaveActive(); } },
                "How the packaged game opens (NoxRuntime): Borderless covers the screen at the desktop's resolution, Fullscreen switches "
                "the display (exclusive). Alt+Enter toggles a window in the game.");
        }

        // ---- Physics ----
        {
            SettingsRowsV2 physics = rows.Section("Physical Surfaces", false, &m_Expanded,
                "Unreal's Physical Surface types: materials (Material editor) and bodies (Rigid Body 3D) pick one; footstep notifies play a sound per surface.");
            physics.Note("Rename a surface in its field (Enter or click away applies it); materials and the open scene's bodies that use it follow.");
            m_Surfaces = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 3.0f);
            m_Surfaces->WidthMode = UI::UISizeMode::Fill;
            physics.GetBody()->AddChild(m_Surfaces);
            RebuildSurfaces();
        }
        return m_Root;
    }

    void ProjectSettingsV2Panel::RebuildSurfaces()
    {
        m_SurfacesDirty = false;
        m_SurfaceFields.clear();
        if (!m_Surfaces)
            return;
        m_Surfaces->ClearChildren();
        ProjectConfig* config = Config();
        if (!config)
            return;
        std::vector<std::string>& surfaces = config->PhysicalSurfaces;
        for (size_t i = 0; i < surfaces.size(); ++i)
        {
            auto row = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
            row->WidthMode = UI::UISizeMode::Fill;
            if (i == 0)
            {
                auto label = UI::Label("Default  (a surface without one; always there)", 13.5f);
                row->AddChild(label);
                m_SurfaceFields.push_back(nullptr);
                m_Surfaces->AddChild(row);
                continue;
            }
            auto field = UI::MakeUI<UI::UITextInput>();
            field->Text = surfaces[i];
            field->WidthMode = UI::UISizeMode::Fill;
            field->OnSubmit = [this, i](const std::string& text) { CommitSurface(i, text); };
            row->AddChild(field);
            Ref<UI::UIButton> remove(new UI::UIButton("Remove", [this, i]()
            {
                ProjectConfig* c = Config();
                if (!c || i >= c->PhysicalSurfaces.size())
                    return;
                c->PhysicalSurfaces.erase(c->PhysicalSurfaces.begin() + static_cast<std::ptrdiff_t>(i));
                Project::SaveActive();
                m_SurfacesDirty = true; // (built again next frame, not inside this button's click)
            }));
            remove->Tooltip = "Materials and bodies that use it keep its name (they count as Default until it is added again).";
            row->AddChild(remove);
            m_SurfaceFields.push_back(field);
            m_Surfaces->AddChild(row);
        }
        Ref<UI::UIButton> add(new UI::UIButton("Add Surface", [this]()
        {
            ProjectConfig* c = Config();
            if (!c)
                return;
            std::string name = "New Surface";
            for (int suffix = 2; std::find(c->PhysicalSurfaces.begin(), c->PhysicalSurfaces.end(), name) != c->PhysicalSurfaces.end(); ++suffix)
                name = "New Surface " + std::to_string(suffix);
            c->PhysicalSurfaces.push_back(name);
            Project::SaveActive();
            m_SurfacesDirty = true;
        }));
        add->HAlign = UI::UIAlignment::Start;
        m_Surfaces->AddChild(add);
    }

    void ProjectSettingsV2Panel::CommitSurface(size_t index, const std::string& typed)
    {
        ProjectConfig* config = Config();
        if (!config || index == 0 || index >= config->PhysicalSurfaces.size())
            return;
        std::vector<std::string>& surfaces = config->PhysicalSurfaces;
        const std::string from = surfaces[index];
        const bool taken = std::find(surfaces.begin(), surfaces.end(), typed) != surfaces.end();
        if (typed.empty() || typed == from || taken)
        {
            m_SurfacesDirty = true; // (the old name back in the field)
            return;
        }
        surfaces[index] = typed;
        Project::SaveActive();
        // The old editor renamed only the list: every material using the name lost its surface. Materials keep theirs in the asset
        // registry (one write); bodies in the open scene follow through the hook.
        if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
        {
            std::vector<AssetHandle> users;
            for (const auto& [handle, metadata] : assets->GetAssetRegistry())
                if (metadata.Type == AssetType::Material && metadata.PhysicalSurface == from)
                    users.push_back(handle);
            for (AssetHandle handle : users)
                assets->SetPhysicalSurface(handle, typed);
        }
        if (m_Hooks.SurfaceRenamed)
            m_Hooks.SurfaceRenamed(from, typed);
    }

    void ProjectSettingsV2Panel::Update()
    {
        if (m_Rebuild)
            BuildUI(m_Context);
        if (m_SurfacesDirty)
            RebuildSurfaces();
        // A surface field left with an edit not applied by Enter: applied now (or put back).
        if (ProjectConfig* config = Config())
        {
            for (size_t i = 1; i < m_SurfaceFields.size() && i < config->PhysicalSurfaces.size(); ++i)
            {
                const Ref<UI::UITextInput>& field = m_SurfaceFields[i];
                if (field && !UI::HasState(field->GetState(), UI::UIState::Focused) && field->Text != config->PhysicalSurfaces[i])
                    CommitSurface(i, field->Text);
            }
        }
    }
}
