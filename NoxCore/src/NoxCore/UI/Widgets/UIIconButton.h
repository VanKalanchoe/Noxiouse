#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // A small button that shows only an icon: a toolbar's Play or Move tool, a card's "...", a chevron that opens a section.
    // The icon is a texture (TextureSlot, inset by Padding) or drawn by DrawIcon in the colour the button's state gives.
    //
    // Look: nothing at rest; hovered or pressed, a soft fill; Active (a mode that is on: the Move tool, Play while playing) an
    // accent fill with an accent border (ActiveLook::Fill) or only the soft fill with the icon in the accent colour
    // (ActiveLook::Tint, a chevron that is open). Icon colours: Active "Accent" (Fill: a light accent), hovered "Text", at rest
    // "TextMuted" (theme tokens).
    //
    // Toggle: a click flips Active first and calls OnToggled. OnClickAt gets the button's bounds (a menu that opens under it).
    class UIIconButton : public UIElement
    {
    public:
        using IconPainter = std::function<void(UIDrawList& drawList, const UIRect& bounds, const UIColor& color)>;
        enum class ActiveLook : uint8_t { Fill, Tint };

        explicit UIIconButton(IconPainter icon = {}, std::string tooltip = {}, std::function<void()> onClick = {});
        // A texture as the icon (a bindless descriptor slot).
        static Ref<UIIconButton> FromTexture(uint32_t textureSlot, std::string tooltip = {}, std::function<void()> onClick = {});

        IconPainter DrawIcon;
        uint32_t TextureSlot = ~0u;
        bool Active = false;
        bool Toggle = false;
        ActiveLook ActiveStyle = ActiveLook::Fill;
        float CornerRadius = 4.0f;

        std::function<void()> OnClick;
        std::function<void(bool active)> OnToggled;
        std::function<void(const UIRect& bounds)> OnClickAt;

        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;

        // The colour the icon is drawn in now (DrawIcon gets it).
        UIColor IconColor(const UITheme& theme) const;

    protected:
        UIVec2 ComputeDesiredSize() override { return FixedSize; }
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void Activate();
    };
}
