#include "UIViewportManager.h"

#include <algorithm>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>

#include "NRI/NRI.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        class UIViewportChrome final : public UIElement
        {
        public:
            UIViewportChrome(
                UIViewportManager& manager,
                UIViewport& viewport,
                const std::string& title,
                const Ref<UIElement>& content)
                : m_Manager(manager), m_Viewport(viewport)
            {
                StyleClass = "Panel";
                DrawBackground = true;
                ClipChildren = true;
                HitTestMode = UIHitTestMode::Self;

                m_TitleLabel = Ref<UILabel>(new UILabel(title));
                m_TitleLabel->HitTestMode = UIHitTestMode::None;
                m_TitleLabel->VAlign = UIAlignment::Center;
                AddChild(m_TitleLabel);

                m_CloseBtn = Ref<UIButton>(new UIButton("X", [&viewport]()
                {
                    // The dock space may keep it open (unsaved changes to ask about).
                    UIViewportManager* manager = UIViewportManager::GetActive();
                    UIDockSpace* dock = manager ? manager->GetDockSpace() : nullptr;
                    if (dock && dock->CanClosePanel && !dock->CanClosePanel(viewport.Title))
                        return;
                    viewport.PendingClose = true;
                }));
                m_CloseBtn->StyleClass = "TitleClose";
                m_CloseBtn->WidthMode = UISizeMode::Fixed;
                m_CloseBtn->HeightMode = UISizeMode::Fixed;
                AddChild(m_CloseBtn);

                m_ContentHost = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 0.0f));
                m_ContentHost->Padding = UIThickness::All(6.0f);
                if (content)
                {
                    content->Visible = true;
                    content->WidthMode = UISizeMode::Fill;
                    content->HeightMode = UISizeMode::Fill;
                    m_ContentHost->AddChild(content);
                }
                AddChild(m_ContentHost);
                viewport.ContentHost = m_ContentHost;
            }

            UIElement* HitTest(const UIVec2& pos) override
            {
                if (!Visible || HitTestMode == UIHitTestMode::None)
                    return nullptr;
                const UIRect& b = GetBounds();
                if (!b.Contains(pos))
                    return nullptr;
                const UIRect resizeRect{
                    { b.Max.x - kResizeGripSize, b.Max.y - kResizeGripSize },
                    b.Max
                };
                if (resizeRect.Contains(pos))
                    return this;
                return UIElement::HitTest(pos);
            }

            UICursorShape GetCursorShape(const UIVec2& pos) const override
            {
                const UIRect& b = GetBounds();
                const UIRect resizeRect{
                    { b.Max.x - kResizeGripSize, b.Max.y - kResizeGripSize },
                    b.Max
                };
                return resizeRect.Contains(pos) ? UICursorShape::ResizeNWSE : UICursorShape::Default;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                const UIRect& b = GetBounds();
                const UIRect resizeRect{
                    { b.Max.x - kResizeGripSize, b.Max.y - kResizeGripSize },
                    b.Max
                };
                if (resizeRect.Contains(e.Position))
                {
                    m_Manager.BeginResizeViewport(&m_Viewport);
                    return true;
                }
                if (e.Position.y <= b.Min.y + m_TitleBarHeight)
                {
                    const float scale = m_Viewport.Window ? m_Viewport.Window->GetContentScale() : 1.0f;
                    m_Manager.BeginDragViewport(&m_Viewport, { e.Position.x / scale, e.Position.y / scale });
                    return true;
                }
                return false;
            }

        protected:
            UIVec2 ComputeDesiredSize() override
            {
                const UIVec2 titleSz = m_TitleLabel ? m_TitleLabel->Measure() : UIVec2{};
                if (m_CloseBtn)
                    m_CloseBtn->Measure();
                const UIVec2 contentSz = m_ContentHost ? m_ContentHost->Measure() : UIVec2{};
                m_TitleBarHeight = std::max(kTitleBarHeight, titleSz.y + 10.0f);
                return {
                    std::max(titleSz.x + m_TitleBarHeight + 20.0f, contentSz.x),
                    m_TitleBarHeight + contentSz.y
                };
            }

            void ArrangeChildren(const UIRect& /*contentRect*/) override
            {
                const UIRect& b = GetBounds();
                const UIVec2 titleSz = m_TitleLabel ? m_TitleLabel->GetDesiredSize() : UIVec2{};
                m_TitleBarHeight = std::max(kTitleBarHeight, titleSz.y + 10.0f);
                const float closeW = m_TitleBarHeight;
                if (m_TitleLabel)
                {
                    m_TitleLabel->Arrange({
                        { b.Min.x + 10.0f, b.Min.y },
                        { b.Max.x - closeW - 6.0f, b.Min.y + m_TitleBarHeight }
                    });
                }
                if (m_CloseBtn)
                {
                    m_CloseBtn->FixedSize = { closeW, m_TitleBarHeight };
                    m_CloseBtn->Arrange({
                        { b.Max.x - closeW, b.Min.y },
                        { b.Max.x, b.Min.y + m_TitleBarHeight }
                    });
                }
                if (m_ContentHost)
                {
                    m_ContentHost->Arrange({
                        { b.Min.x, b.Min.y + m_TitleBarHeight },
                        { b.Max.x, b.Max.y }
                    });
                }
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIElement::OnPaint(drawList, theme);
                const UIRect& b = GetBounds();
                const UIColor border = theme.GetToken("Border", { 0.204f, 0.216f, 0.255f, 1.0f });
                const UIColor muted = theme.GetToken("TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });
                drawList.AddRoundedRect(
                    b.Min,
                    { b.Max.x, b.Min.y + m_TitleBarHeight },
                    theme.GetToken("Surface1", { 0.141f, 0.149f, 0.173f, 1.0f })
                );
                drawList.AddRoundedRect(
                    { b.Min.x, b.Min.y + m_TitleBarHeight - 1.0f },
                    { b.Max.x, b.Min.y + m_TitleBarHeight },
                    border
                );
                drawList.AddBezier(
                    { b.Max.x - 10.0f, b.Max.y - 3.0f },
                    { b.Max.x - 6.5f, b.Max.y - 6.5f },
                    { b.Max.x - 6.5f, b.Max.y - 6.5f },
                    { b.Max.x - 3.0f, b.Max.y - 10.0f },
                    1.5f,
                    { muted.r, muted.g, muted.b, 0.7f }
                );
            }

        private:
            static constexpr float kTitleBarHeight = 30.0f;
            static constexpr float kResizeGripSize = 14.0f;
            float m_TitleBarHeight = kTitleBarHeight;

            UIViewportManager& m_Manager;
            UIViewport& m_Viewport;
            Ref<UILabel> m_TitleLabel;
            Ref<UIButton> m_CloseBtn;
            Ref<UIStackBox> m_ContentHost;
        };

        uint32_t ExtractWindowID(const SDL_Event& e)
        {
            switch (e.type)
            {
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                return e.window.windowID;
            case SDL_EVENT_MOUSE_MOTION:
                return e.motion.windowID;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                return e.button.windowID;
            case SDL_EVENT_MOUSE_WHEEL:
                return e.wheel.windowID;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                return e.key.windowID;
            case SDL_EVENT_TEXT_INPUT:
                return e.text.windowID;
            default:
                return 0;
            }
        }
    }

    UIViewportManager::UIViewportManager(
        Nox::Window& mainWindow,
        Renderer& renderer,
        UIContext& mainContext,
        UIDockSpace* dockSpace)
        : m_MainWindow(mainWindow)
        , m_Renderer(renderer)
        , m_MainContext(mainContext)
        , m_DockSpace(dockSpace)
    {
        s_Active = this;
    }

    UIViewportManager::~UIViewportManager()
    {
        Shutdown();
        if (s_Active == this)
            s_Active = nullptr;
    }

    void UIViewportManager::Shutdown()
    {
        m_ActiveDragViewport = nullptr;
        m_ActiveResizeViewport = nullptr;
        if (!m_Viewports.empty())
        {
            m_Renderer.WaitDeviceIdle();
            m_Viewports.clear();
        }
    }

    bool UIViewportManager::ClosePanel(const std::string& title)
    {
        for (const auto& vp : m_Viewports)
        {
            if (vp->Title == title)
            {
                vp->PendingClose = true;
                return true;
            }
        }
        return false;
    }

    bool UIViewportManager::HasPanel(const std::string& title) const
    {
        for (const auto& vp : m_Viewports)
            if (vp->Title == title && !vp->PendingClose)
                return true;
        return false;
    }

    UIViewport* UIViewportManager::FindByWindowID(uint32_t windowID) const
    {
        for (const auto& vp : m_Viewports)
        {
            if (vp->WindowID == windowID)
                return vp.get();
        }
        return nullptr;
    }

    UIViewport* UIViewportManager::PromoteToViewport(
        const std::string& title,
        const Ref<UIElement>& content,
        UIVec2 size,
        UIVec2 grabOffset,
        bool startDragging)
    {
        float gx = 0.0f;
        float gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);

        const uint32_t winW = static_cast<uint32_t>(std::max(220.0f, size.x));
        const uint32_t winH = static_cast<uint32_t>(std::max(140.0f, size.y));

        WindowSpecification spec{};
        spec.Ttile = title.empty() ? "Floating Panel" : title;
        spec.Width = winW;
        spec.Height = winH;
        spec.IsResizeable = true;
        spec.IsVulkan = true;
        spec.Borderless = true;
        spec.DestroyOnDestruct = true;
        spec.ScaleByDisplay = false;
        spec.MinWidth = 220;
        spec.MinHeight = 140;
        spec.HasPosition = true;
        spec.PosX = static_cast<int>(gx - grabOffset.x);
        spec.PosY = static_cast<int>(gy - grabOffset.y);

        auto vp = std::make_unique<UIViewport>();
        vp->Title = title;
        vp->Content = content;
        vp->Window = std::make_unique<Nox::Window>(spec);
        vp->WindowID = vp->Window->GetWindowID();

        int pw = 0;
        int ph = 0;
        vp->Window->getSizeInPixels(pw, ph);
        vp->PixelWidth = static_cast<uint32_t>(std::max(pw, 1));
        vp->PixelHeight = static_cast<uint32_t>(std::max(ph, 1));

        vp->Swapchain = m_Renderer.CreateWindowSwapchain(*vp->Window, vp->PixelWidth, vp->PixelHeight, false);
        for (uint32_t i = 0; i < 2; ++i)
        {
            vp->Allocators[i] = m_Renderer.CreateUICommandAllocator();
            if (vp->Allocators[i])
                vp->CmdBuffers[i] = vp->Allocators[i]->allocateCommandBuffer(1);
        }
        vp->Uploads = m_Renderer.CreateUIUploadBuffers();

        if (m_MainContext.GetTheme())
            vp->Context.SetTheme(m_MainContext.GetTheme());

        Ref<UIViewportChrome> chrome(new UIViewportChrome(*this, *vp, title, content));
        vp->Context.SetRoot(chrome);

        if (vp->Window->getHandle())
            SDL_StartTextInput(vp->Window->getHandle());

        UIViewport* raw = vp.get();
        m_Viewports.push_back(std::move(vp));

        if (startDragging)
            BeginDragViewport(raw, grabOffset);

        return raw;
    }

    void UIViewportManager::BeginDragViewport(UIViewport* viewport, UIVec2 grabOffset)
    {
        m_ActiveDragViewport = viewport;
        m_DragGrabOffset = grabOffset;
    }

    void UIViewportManager::BeginResizeViewport(UIViewport* viewport)
    {
        m_ActiveResizeViewport = viewport;
    }

    bool UIViewportManager::ProcessSDLEvent(const SDL_Event& e)
    {
        const uint32_t winID = ExtractWindowID(e);
        if (winID == 0 || winID == m_MainWindow.GetWindowID())
            return false;

        UIViewport* vp = FindByWindowID(winID);
        if (!vp)
            return false;

        switch (e.type)
        {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (!m_DockSpace || !m_DockSpace->CanClosePanel || m_DockSpace->CanClosePanel(vp->Title))
                vp->PendingClose = true;
            return true;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            vp->NeedsSwapchainResize = true;
            vp->LastResizeRequestTime = std::chrono::steady_clock::now();
            return true;
        case SDL_EVENT_MOUSE_MOTION:
        {
            MouseMovedEvent ev(e.motion.x, e.motion.y);
            vp->Context.OnEvent(ev);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            MouseButtonPressedEvent ev(e.button.button);
            vp->Context.OnEvent(ev);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            MouseButtonReleasedEvent ev(e.button.button);
            vp->Context.OnEvent(ev);
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL:
        {
            MouseScrolledEvent ev(e.wheel.x, e.wheel.y);
            vp->Context.OnEvent(ev);
            return true;
        }
        case SDL_EVENT_KEY_DOWN:
        {
            KeyPressedEvent ev(e.key.scancode, e.key.repeat != 0, e.key.mod);
            vp->Context.OnEvent(ev);
            return true;
        }
        case SDL_EVENT_TEXT_INPUT:
        {
            if (e.text.text)
            {
                TextInputEvent ev(e.text.text);
                vp->Context.OnEvent(ev);
            }
            return true;
        }
        default:
            return false;
        }
    }

    void UIViewportManager::UpdateActiveDrag()
    {
        if (!m_ActiveDragViewport || !m_ActiveDragViewport->Window)
            return;

        float gx = 0.0f;
        float gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
        m_ActiveDragViewport->Window->SetPosition(
            static_cast<int>(gx - m_DragGrabOffset.x),
            static_cast<int>(gy - m_DragGrabOffset.y)
        );

        if (m_DockSpace)
        {
            int mainX = 0, mainY = 0, mainW = 0, mainH = 0;
            m_MainWindow.GetPosition(mainX, mainY);
            m_MainWindow.GetSize(mainW, mainH);
            const bool overMain = (gx >= mainX && gx <= mainX + mainW && gy >= mainY && gy <= mainY + mainH);
            if (overMain)
            {
                const float scale = m_MainWindow.GetContentScale();
                const UIVec2 localPos{
                    (gx - static_cast<float>(mainX)) * scale,
                    (gy - static_cast<float>(mainY)) * scale
                };
                m_DockSpace->UpdateDockPreview(localPos);
            }
            else
            {
                m_DockSpace->ClearDockPreview();
            }
        }
    }

    void UIViewportManager::UpdateAndRender(float dt)
    {
        if (m_ActiveDragViewport && m_ActiveDragViewport->Window)
        {
            UpdateActiveDrag();
            float gx = 0.0f;
            float gy = 0.0f;
            const SDL_MouseButtonFlags btns = SDL_GetGlobalMouseState(&gx, &gy);
            if ((btns & SDL_BUTTON_LMASK) == 0)
            {
                if (m_DockSpace && m_DockSpace->HasActiveDropZone())
                {
                    Ref<UIElement> content = m_ActiveDragViewport->Content;
                    if (m_ActiveDragViewport->ContentHost && content)
                        m_ActiveDragViewport->ContentHost->RemoveChild(content);
                    m_DockSpace->CommitDockDrop(m_ActiveDragViewport->Title, content);
                    m_ActiveDragViewport->PendingClose = true;
                }
                else if (m_DockSpace)
                {
                    m_DockSpace->ClearDockPreview();
                }
                m_ActiveDragViewport = nullptr;
            }
        }

        if (m_ActiveResizeViewport && m_ActiveResizeViewport->Window)
        {
            float gx = 0.0f;
            float gy = 0.0f;
            const SDL_MouseButtonFlags btns = SDL_GetGlobalMouseState(&gx, &gy);
            int wx = 0, wy = 0;
            m_ActiveResizeViewport->Window->GetPosition(wx, wy);
            const int newW = std::max(220, static_cast<int>(gx) - wx);
            const int newH = std::max(140, static_cast<int>(gy) - wy);
            m_ActiveResizeViewport->Window->SetSize(newW, newH);
            m_ActiveResizeViewport->NeedsSwapchainResize = true;
            m_ActiveResizeViewport->LastResizeRequestTime = std::chrono::steady_clock::now();

            if ((btns & SDL_BUTTON_LMASK) == 0)
                m_ActiveResizeViewport = nullptr;
        }

        bool anyClosing = false;
        for (const auto& vp : m_Viewports)
        {
            if (vp->PendingClose)
            {
                anyClosing = true;
                break;
            }
        }
        if (anyClosing)
        {
            m_Renderer.WaitDeviceIdle();
            std::erase_if(m_Viewports, [this](const std::unique_ptr<UIViewport>& vp)
            {
                if (!vp->PendingClose)
                    return false;
                if (m_ActiveDragViewport == vp.get())
                    m_ActiveDragViewport = nullptr;
                if (m_ActiveResizeViewport == vp.get())
                    m_ActiveResizeViewport = nullptr;
                return true;
            });
        }

        const auto now = std::chrono::steady_clock::now();
        const uint32_t slot = m_Renderer.GetCurrentFrameIndex() % 2;
        for (const auto& vp : m_Viewports)
        {
            if (!vp->Window)
                continue;

            int pw = 0;
            int ph = 0;
            vp->Window->getSizeInPixels(pw, ph);
            if (pw <= 0 || ph <= 0)
                continue;

            const uint32_t uW = static_cast<uint32_t>(pw);
            const uint32_t uH = static_cast<uint32_t>(ph);
            const bool sizeChanged = (uW != vp->PixelWidth || uH != vp->PixelHeight);
            const bool resizeSettled = (m_ActiveResizeViewport != vp.get()) ||
                                       (now - vp->LastResizeRequestTime >= std::chrono::milliseconds(60));
            if (!vp->Swapchain || ((vp->NeedsSwapchainResize || sizeChanged) && resizeSettled))
            {
                m_Renderer.WaitDeviceIdle();
                vp->Swapchain.reset();
                vp->Swapchain = m_Renderer.CreateWindowSwapchain(*vp->Window, uW, uH, false);
                vp->PixelWidth = uW;
                vp->PixelHeight = uH;
                vp->NeedsSwapchainResize = false;
            }

            if (!vp->Swapchain || !vp->Allocators[slot] || !vp->CmdBuffers[slot] || !vp->Uploads)
                continue;

            vp->Context.UpdateAndRender(vp->DrawList, { static_cast<float>(uW), static_cast<float>(uH) }, dt);
            if (!m_Renderer.RenderSecondaryUIViewport(
                *vp->Swapchain,
                *vp->Allocators[slot],
                *vp->CmdBuffers[slot],
                *vp->Uploads,
                vp->DrawList,
                { static_cast<float>(uW), static_cast<float>(uH) }))
            {
                vp->NeedsSwapchainResize = true;
            }
        }
    }
}
