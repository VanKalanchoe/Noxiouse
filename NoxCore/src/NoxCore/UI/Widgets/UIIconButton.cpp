#include "UIIconButton.h"

#include <SDL3/SDL_scancode.h>

#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIIconButton::UIIconButton(IconPainter icon, std::string tooltip, std::function<void()> onClick)
        : DrawIcon(std::move(icon)), OnClick(std::move(onClick))
    {
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        Tooltip = std::move(tooltip);
        WidthMode = UISizeMode::Fixed;
        HeightMode = UISizeMode::Fixed;
        FixedSize = { 26.0f, 24.0f };
        VAlign = UIAlignment::Center;
    }

    Ref<UIIconButton> UIIconButton::FromTexture(uint32_t textureSlot, std::string tooltip, std::function<void()> onClick)
    {
        Ref<UIIconButton> button(new UIIconButton({}, std::move(tooltip), std::move(onClick)));
        button->TextureSlot = textureSlot;
        button->FixedSize = { 30.0f, 30.0f };
        button->Padding = UIThickness::All(1.0f);
        return button;
    }

    void UIIconButton::Activate()
    {
        // Copied first: a callback may rebuild what holds this button.
        auto onToggled = OnToggled;
        auto onClick = OnClick;
        auto onClickAt = OnClickAt;
        const UIRect bounds = GetBounds();
        if (Toggle)
        {
            Active = !Active;
            if (onToggled)
                onToggled(Active);
        }
        if (onClick)
            onClick();
        if (onClickAt)
            onClickAt(bounds);
    }

    bool UIIconButton::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == UIMouseButton::Left && IsEnabled() && GetBounds().Contains(e.Position))
        {
            Activate();
            return true;
        }
        return UIElement::OnPointerUp(e);
    }

    bool UIIconButton::OnKeyDown(const UIKeyEvent& e)
    {
        if (!e.Repeat && IsEnabled() && (e.KeyCode == SDL_SCANCODE_RETURN || e.KeyCode == SDL_SCANCODE_KP_ENTER || e.KeyCode == SDL_SCANCODE_SPACE))
        {
            Activate();
            return true;
        }
        return UIElement::OnKeyDown(e);
    }

    UIColor UIIconButton::IconColor(const UITheme& theme) const
    {
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        if (!IsEnabled())
        {
            UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
            muted.a *= 0.5f;
            return muted;
        }
        if (Active)
            return ActiveStyle == ActiveLook::Fill ? glm::mix(accent, UIColor{ 1.0f }, 0.45f) : accent;
        if (HasState(GetState(), UIState::Hovered) || HasState(GetState(), UIState::Pressed))
            return ActiveStyle == ActiveLook::Tint ? accent : theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f });
        return theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
    }

    void UIIconButton::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        const UIRect& b = GetBounds();
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const bool hovered = HasState(GetState(), UIState::Hovered) && IsEnabled();
        const bool pressed = HasState(GetState(), UIState::Pressed) && IsEnabled();
        const glm::vec4 radius(CornerRadius);

        if (Active && ActiveStyle == ActiveLook::Fill)
            drawList.AddRoundedRect(b.Min, b.Max, { accent.r, accent.g, accent.b, hovered ? 0.36f : 0.26f }, radius, 1.0f,
                                    { accent.r, accent.g, accent.b, 0.85f });
        else if (Active || hovered || pressed)
        {
            UIColor fill = theme.GetToken("Text", { 1.0f, 1.0f, 1.0f, 1.0f });
            fill.a = pressed ? 0.16f : 0.10f;
            drawList.AddRoundedRect(b.Min, b.Max, fill, radius);
        }

        if (TextureSlot != ~0u)
        {
            const UIRect inner = b.Inset(Padding);
            UIColor tint{ 1.0f };
            if (!IsEnabled())
                tint.a = 0.45f;
            drawList.AddTexturedQuad(inner.Min, inner.Max, TextureSlot, { 0.0f, 0.0f }, { 1.0f, 1.0f }, tint, glm::vec4(0.0f));
        }
        if (DrawIcon)
            DrawIcon(drawList, b, IconColor(theme));
    }
}
