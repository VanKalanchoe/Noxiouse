#pragma once

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Font;

    class EditorV2Layer : public Layer
    {
    public:
        EditorV2Layer();
        ~EditorV2Layer() override = default;

        void OnUpdate(Timestep ts) override;
        void OnRender() override;

    private:
        Ref<Font> m_DefaultFont;
        float m_Elapsed = 0.0f;
    };
}
