#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"

namespace Nox
{
    // The window the editor opens with when no project is given (Unreal's Project Browser, Godot's Project Manager): the recent
    // projects to open, a form to make a new one, Browse for a .nproj elsewhere. Picking one hands the window over to the editor
    // (EditorLayer) with that project; the editor's File > Project Manager... comes back here (a new editor process, as both engines
    // do: one project per editor process).
    class ProjectManagerLayer : public Layer
    {
    public:
        ProjectManagerLayer();
        ~ProjectManagerLayer() override;

        void OnImGuiRender() override;

        // The manager keeps ImGui from reading or writing imgui.ini while it runs (the file holds the editor's layout, and part of it
        // is read by handlers only the editor registers); the editor takes the name back and loads the file itself.
        static const char* TakeIniFilename();

    private:
        struct Entry
        {
            std::filesystem::path File;
            std::string Name;
            bool Exists = false;
            bool Sample = false;
            std::string Modified; // "3 hours ago"
            Ref<Texture2D> Thumbnail; // Saved/Thumbnail.png, when the project has one
        };
        void Refresh();
        // `createBasicScene`: a project just made from the Basic template (the editor makes and saves its first scene).
        void Open(const std::filesystem::path& projectFile, bool createBasicScene = false);
        void DrawProjectList(float height);
        void DrawNewProject();

        std::vector<Entry> m_Projects;
        int m_Selected = -1;
        char m_Search[128] = "";
        char m_Name[65] = "MyGame";
        char m_Location[1024] = "";
        int m_Template = 0; // 0: Basic, 1: Blank
        int m_Units = 0;   // 0: centimeters (100 units per meter), 1: meters
        int m_Quality = 0; // index into the quality names
        bool m_CreateScripts = true;
        bool m_OpenLast = false;
        bool m_Opening = false;
        std::string m_Error;
    };
}
