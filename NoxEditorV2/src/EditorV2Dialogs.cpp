// EditorV2Layer: File > Package Game / Package Server and Create > Streaming Test World (D1).
#include "EditorV2Layer.h"

#include <format>

#include "NoxCore/UI/Widgets/UIPropertyRows.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIModal.h"

namespace Nox
{
    void EditorV2Layer::ShowPackageDialog(bool server)
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || !Project::GetActive())
            return;
        m_AssetEditors.EndPreviews(); // (the package cooks the scenes from disk; a preview's poses are not part of them)
        UI::UIContext* context = &uiLayer->GetContext();
        // Each target its own folder by default (Build/<Project>, Build/<Project>Server); a folder typed for this target is kept.
        if (m_PackageOutputText.empty() || server != m_PackageSettings.Server)
        {
            const std::string name = Project::GetActive()->GetConfig().Name + (server ? "Server" : "");
            m_PackageOutputText = (Project::GetProjectDirectory() / "Build" / name).generic_string();
        }
        m_PackageSettings.Server = server;

        auto content = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        content->WidthMode = UI::UISizeMode::Fill;
        UI::UIPropertyRows rows(content, context);
        rows.Note(server ? "A dedicated server (Unreal's server target): the runtime named <Project>Server -- it starts as a dedicated server, no "
                           "window, GPU or sound, its console takes the server's commands -- with the scenes' cooked assets but no textures, fonts, "
                           "sound clips, baked acoustics or shaders. Clients join it with --connect=<its address>."
                         : "A folder that runs the game on its own: the runtime (NoxRuntime, renamed after the project), the engine's libraries, "
                           "scripts and shaders, and only the assets the scenes need, cooked, without their sources.");
        rows.String("Output Folder", &m_PackageOutputText, "a folder (made if missing)");
        PackageSettings* settings = &m_PackageSettings;
        rows.Check("Every Scene", [settings]() { return settings->AllScenes; }, [settings](bool v) { settings->AllScenes = v; },
                   "The start scene and what it uses is the game; tick this when scripts load other scenes.");
        rows.Check("Pack Files (Content.npak)", [settings]() { return settings->PackFiles; }, [settings](bool v) { settings->PackFiles = v; },
                   "The assets in one pack file the game reads from (only its table of contents stays in memory). Off: a folder of loose files, easier to inspect.");
        Ref<UI::UIElement> shaders = rows.Check("Precompiled Shaders Only (no Slang)", [settings]() { return settings->PrecompiledShadersOnly; },
                   [settings](bool v) { settings->PrecompiledShadersOnly = v; },
                   "The game uses the shader cache the editor wrote (every pipeline it created) and ships without the shader compiler. A shader the "
                   "editor never compiled would fail: off ships Slang to compile it.");
        rows.EnableWhen(shaders, [server]() { return !server; }, []() { return std::string("a server has no shaders"); });
        rows.Check("Self-Contained .NET", [settings]() { return settings->SelfContainedDotNet; }, [settings](bool v) { settings->SelfContainedDotNet = v; },
                   "Copies the .NET 9 runtime into the game (about 70 MB): it runs where .NET is not installed.");

        auto result = UI::Label("", 13.5f);
        Ref<UI::UILabel> resultRef = result;
        resultRef->WordWrap = true;
        resultRef->WidthMode = UI::UISizeMode::Fill;
        Ref<UI::UIButton> package(new UI::UIButton(server ? "Package Server" : "Package Game", [this, resultRef]()
        {
            SaveScene(); // the scene as edited is what ships
            PackageSettings run = m_PackageSettings;
            run.OutputDirectory = std::filesystem::path(m_PackageOutputText);
            if (run.OutputDirectory.empty())
            {
                resultRef->Text = "Give it an output folder.";
                return;
            }
            // (It runs on this thread: the editor waits until it is done -- the cook reads the asset manager.)
            const PackageReport report = PackageGame(run);
            if (!report.Success)
            {
                resultRef->Text = "Failed: " + report.Error;
                Notify(UI::UINotificationSeverity::Error, "Packaging failed", report.Error);
                return;
            }
            std::string text = std::format("Done: {} assets, {} files, {:.1f} MB.\nRun {}", report.Assets, report.Files,
                                           static_cast<double>(report.Bytes) / (1024.0 * 1024.0), report.Executable.generic_string());
            for (const std::string& warning : report.Warnings)
                text += "\n- " + warning;
            resultRef->Text = text;
            Notify(UI::UINotificationSeverity::Success, "Packaged", report.Executable.generic_string());
        }));
        package->StyleVariant = "primary";
        package->HAlign = UI::UIAlignment::Start;
        package->Tooltip = "Saves the scene, then cooks and copies (the editor waits until it is done).";
        content->AddChild(package);
        content->AddChild(resultRef);
        UI::UIModal::ShowCustom(*context, server ? "Package Server" : "Package Game", content, { { "Close", {} } }, 640.0f);
    }

    void EditorV2Layer::ShowStreamingTestWorldDialog()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer || m_TestWorldGeneration || m_SceneState != SceneState::Edit)
            return;
        UI::UIContext* context = &uiLayer->GetContext();
        auto content = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        content->WidthMode = UI::UISizeMode::Fill;
        UI::UIPropertyRows rows(content, context);
        StreamingTestWorldGenerator::Settings* s = &m_TestWorldSettings;
        rows.Note("A world of N x N cells, each with its own terrain mesh and texture (nothing shared), so its data is larger than RAM and VRAM. "
                  "It is written into the project as cooked assets and opened.");
        rows.Drag("Size on Disk (GB)", [s]() { return s->TargetGigabytes; }, [s](float v) { s->TargetGigabytes = v; }, 0.25f, 0.25f, 200.0f, "{:.2f}",
                  "N is chosen for it from the first tile's size. Larger than RAM plus VRAM shows that only what is near the camera is in memory. "
                  "Start small (2 GB) to try it.");
        rows.Drag("Cell Size (m)", [s]() { return s->CellSizeMeters; }, [s](float v) { s->CellSizeMeters = v; }, 1.0f, 8.0f, 1024.0f, "{:.0f}");
        rows.Drag("Loading Range (m)", [s]() { return s->LoadingRangeMeters; }, [s](float v) { s->LoadingRangeMeters = v; }, 1.0f, 16.0f, 8192.0f, "{:.0f}");
        rows.Int("Tile Quads", [s]() { return static_cast<int>(s->TileQuads); }, [s](int v) { s->TileQuads = static_cast<uint32_t>(v); }, 8, 512,
                 "Per side: 128 is 33k triangles a tile.");
        rows.Int("Props per Cell", [s]() { return static_cast<int>(s->PropsPerCell); }, [s](int v) { s->PropsPerCell = static_cast<uint32_t>(v); }, 0, 5000,
                 "Rocks, pines and stumps: an entity each (sharing three meshes), so that cells are as dense as an open world's. 0: only the ground.");
        rows.Drag("World Offset (km)", [s]() { return s->WorldOffsetKm; }, [s](float v) { s->WorldOffsetKm = v; }, 10.0f, 0.0f, 100000.0f, "{:.0f}",
                  "Large world coordinates test: the whole world this far out along X and Z. 1000 km is far past where float positions jitter: "
                  "anything that drifts, shakes or snaps out there and not at 0 is a precision bug.");
        rows.Choice("Texture Size", { "256", "512", "1024", "2048", "4096" },
                    [s]() { int index = 0; while (index < 4 && (256u << index) < s->TextureSize) ++index; return index; },
                    [s](int index) { s->TextureSize = 256u << index; });
        rows.String("Folder", &s->Folder, "a folder under Assets");
        rows.String("Camera Script", &s->CameraScript, "e.g. Facerun.StreamingWorldTest",
                    {}, "A script class put on the camera, e.g. Facerun.StreamingWorldTest (the fly-through test) or Facerun.FlyCamera; empty: none.");
        auto error = UI::Label("", 13.0f);
        Ref<UI::UILabel> errorRef = error;
        Ref<UI::UIButton> generate(new UI::UIButton("Generate", [this, errorRef]()
        {
            if (m_TestWorldSettings.Folder.empty())
            {
                errorRef->Text = "Give it a folder.";
                return;
            }
            m_TestWorldGeneration = CreateScope<StreamingTestWorldGenerator>(m_TestWorldSettings);
            Notify(UI::UINotificationSeverity::Info, "Generating the streaming test world", "Its progress is in the status bar; Create > Cancel Streaming Test World stops it.");
            if (UILayer* ui = Application::Get().GetUILayer())
                ui->GetContext().ClearOverlay();
        }));
        generate->StyleVariant = "primary";
        generate->HAlign = UI::UIAlignment::Start;
        content->AddChild(generate);
        content->AddChild(errorRef);
        UI::UIModal::ShowCustom(*context, "Streaming Test World", content, { { "Cancel", {} } }, 560.0f);
    }

    void EditorV2Layer::CancelStreamingTestWorld()
    {
        if (!m_TestWorldGeneration)
            return;
        m_TestWorldGeneration.reset(); // (what was written stays on disk, unregistered)
        NOX_CORE_WARN("Streaming test world: cancelled");
        Notify(UI::UINotificationSeverity::Warning, "Streaming test world cancelled", "What was written stays on disk, not registered as assets.");
    }
}
