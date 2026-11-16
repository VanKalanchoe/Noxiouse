#pragma once

#include <algorithm>

#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Nox::UI
{
    class UIDrawList;

    // One vertical scroll bar: geometry, pointer handling and look. Not an element -- the scrolling widget owns one, gives it
    // its view rect every layout and forwards the pointer (UIScrollView, UITreeView, UILogView, UITableView all use this one, so
    // they grab, highlight and page the same way).
    //
    // The whole lane (kLaneWidth at the right edge of the view) takes the pointer, not only the painted thumb: pressing the thumb
    // grabs it where it was pressed (it does not jump), pressing the lane beside it brings the thumb's middle under the pointer
    // and keeps dragging. The thumb widens and brightens under the pointer, takes the accent colour while dragged and lights up
    // for a moment when the content was scrolled by the wheel or the keyboard (NotifyScrolled).
    class UIScrollBar
    {
    public:
        static constexpr float kLaneWidth = 12.0f;

        // Every layout: the lane is the right edge of `viewRect`, the thumb's length follows viewportSize / contentSize.
        void SetGeometry(const UIRect& viewRect, float contentSize, float viewportSize);

        bool IsNeeded() const { return m_Viewport > 1.0f && m_Content > m_Viewport + 0.5f; }
        float GetMaxScroll() const { return IsNeeded() ? m_Content - m_Viewport : 0.0f; }
        // What the widget keeps free of content at its right edge (0 when everything fits).
        float GetReservedWidth() const { return IsNeeded() ? kLaneWidth : 0.0f; }

        UIRect GetLaneRect() const;
        UIRect GetThumbRect(float scrollOffset) const; // the full lane's width: what the pointer hits
        bool Contains(const UIVec2& pos) const { return IsNeeded() && GetLaneRect().Contains(pos); }
        bool IsDragging() const { return m_Dragging; }

        // Return true when the bar took the event; scrollOffset is updated in place.
        bool OnPointerDown(const UIVec2& pos, float& scrollOffset);
        bool OnPointerMove(const UIVec2& pos, float& scrollOffset);
        bool OnPointerUp();

        // The content moved without the bar (wheel, keyboard, scroll-into-view): lights the thumb up for a moment.
        void NotifyScrolled() { m_Activity = 1.0f; }

        // Once a frame; `hovered`: the pointer is over the lane and nothing else has the pointer.
        void Update(float dt, bool hovered);
        void Paint(UIDrawList& drawList, const UITheme& theme, float scrollOffset) const;

    private:
        float TrackTop() const { return m_View.Min.y + kTrackInset; }
        float TrackLength() const { return std::max(1.0f, m_View.Height() - kTrackInset * 2.0f); }
        float ThumbLength() const;

        static constexpr float kTrackInset = 3.0f;
        static constexpr float kMinThumb = 28.0f;

        UIRect m_View{};
        float m_Content = 0.0f;
        float m_Viewport = 0.0f;

        bool m_Dragging = false;
        float m_GrabOffset = 0.0f; // pointer y - thumb top when the drag started

        float m_Hover = 0.0f;    // 0 idle .. 1 under the pointer / dragged (animated)
        float m_Activity = 0.0f; // 1 right after a scroll, fades to 0
    };
}
