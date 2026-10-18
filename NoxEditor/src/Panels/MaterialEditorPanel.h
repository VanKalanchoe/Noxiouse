#pragma once
#include <string>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    // The Material Editor (docs/Future.md item 2): one window per open material (.nmat), opened by double-clicking a material in
    // the Content Browser or by a material slot's Edit button in the Properties panel -- the material lives in its own window like
    // UE's Material Instance editor instead of only inside the entity's Properties. Edits apply live to every instance using the
    // material and are saved to its .nmat (assets are not part of the scene's undo history).
    class MaterialEditorPanel
    {
    public:
        explicit MaterialEditorPanel(AssetHandle material);

        void OnImGuiRender();
        bool IsOpen() const { return m_Open; }
        AssetHandle GetMaterial() const { return m_Material; }
        const std::string& GetTitle() const { return m_Title; }

        // The material's factors, texture slots and alpha mode. Changes go into the loaded material (every instance shades with
        // them this frame) and are written to its .nmat. Shared with the Properties panel's Quick Edit.
        static void DrawProperties(AssetHandle handle, MaterialData& data);

    private:
        AssetHandle m_Material = 0;
        std::string m_Title;
        bool m_Open = true;
    };
}
