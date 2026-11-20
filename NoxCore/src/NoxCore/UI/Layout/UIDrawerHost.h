#pragma once

#include <functional>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIStackBox;

    // A main element with a drawer that slides up over it from the bottom: it covers what is under it instead of resizing it, as
    // Unreal's Content Drawer does. The drawer is a frame on the theme's Surface0 colour with a shadow over what it covers, a grip
    // on its top edge (drag to resize: HeightFraction of the host's height, kept within MinFraction..MaxFraction; OnResized) and the
    // page (SetPage: one element at a time, e.g. a content browser or a log). Open slides it in and out (Openness 0..1, eased);
    // CloseNow hides it at once. The frame takes every click on it, so nothing behind the open drawer is reached.
    class UIDrawerHost : public UIElement
    {
    public:
        explicit UIDrawerHost(Ref<UIElement> content = {});

        void SetContent(const Ref<UIElement>& content);
        const Ref<UIElement>& GetContent() const { return m_Content; }
        // The drawer's page; null empties it.
        void SetPage(const Ref<UIElement>& page);
        Ref<UIElement> GetPage() const;

        bool Open = false;
        float HeightFraction = 0.42f;
        float MinFraction = 0.15f, MaxFraction = 0.9f;
        float Openness = 0.0f;           // 0 closed .. 1 open (animated towards Open)
        float SlideTimeConstant = 0.045f; // seconds
        std::function<void(float fraction)> OnResized;

        void CloseNow()
        {
            Open = false;
            Openness = 0.0f;
        }
        // Where the drawer is now (empty while it is closed).
        UIRect DrawerRect() const;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnUpdate(float dt) override;
        void ArrangeChildren(const UIRect& rect) override;

    private:
        Ref<UIElement> m_Content;
        Ref<UIElement> m_Frame;
        Ref<UIStackBox> m_PageHost;
    };
}
