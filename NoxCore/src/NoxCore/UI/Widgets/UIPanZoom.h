#pragma once

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Core/UITypes.h"

namespace Nox::UI
{
    class UIDrawList;
    class UITheme;

    // The pan and zoom of a canvas (a node graph, a state machine, a map): graph units to screen pixels, screen = bounds.Min + Pan + p * Zoom.
    // A canvas keeps one and draws through it; the wheel zooms around the pointer (ZoomAt: the point under it stays under it),
    // Fit frames a box, PaintGrid draws the background grid and PaintZoomLabel the "100 %" in the corner.
    struct UIPanZoom
    {
        UIVec2 Pan{ 0.0f };
        float Zoom = 1.0f;
        float MinZoom = 0.2f, MaxZoom = 2.5f;

        UIVec2 ToScreen(const UIRect& bounds, const UIVec2& p) const { return bounds.Min + Pan + p * Zoom; }
        UIVec2 ToGraph(const UIRect& bounds, const UIVec2& p) const { return (p - bounds.Min - Pan) / Zoom; }

        // `steps` wheel notches (up: in), each a factor of `stepFactor`, within MinZoom..MaxZoom.
        void ZoomAt(const UIRect& bounds, const UIVec2& pointer, float steps, float stepFactor = 1.15f)
        {
            const UIVec2 before = ToGraph(bounds, pointer);
            Zoom = std::clamp(Zoom * std::pow(stepFactor, steps), MinZoom, MaxZoom);
            Pan = pointer - bounds.Min - before * Zoom;
        }
        // The box min..max in the middle of the view: `marginUnits` graph units added around it, `marginPixels` kept free at the
        // view's edges, the zoom within minZoom..maxZoom (a tighter range than the wheel's: framing never zooms in too far).
        void Fit(const UIRect& bounds, const UIVec2& min, const UIVec2& max, float minZoom, float maxZoom, float marginPixels = 0.0f,
                 float marginUnits = 0.0f)
        {
            const UIVec2 size = glm::max(max - min + UIVec2(marginUnits * 2.0f), UIVec2(1.0f));
            Zoom = std::clamp(std::min((bounds.Width() - marginPixels * 2.0f) / size.x, (bounds.Height() - marginPixels * 2.0f) / size.y), minZoom, maxZoom);
            Pan = bounds.Size() * 0.5f - (min + max) * 0.5f * Zoom;
        }
        // The graph point `p` in the middle of the view, the zoom kept (a map following the camera).
        void CenterOn(const UIRect& bounds, const UIVec2& p) { Pan = bounds.Size() * 0.5f - p * Zoom; }
        // Zoom 1 around the graph point now in the middle of the view.
        void ResetZoom(const UIRect& bounds)
        {
            const UIVec2 center = ToGraph(bounds, bounds.Center());
            Zoom = 1.0f;
            Pan = bounds.Size() * 0.5f - center;
        }

        // Lines every `fineStep` and `coarseStep` graph units (each only while at least 8 px apart), white at the given opacities.
        void PaintGrid(UIDrawList& drawList, const UIRect& bounds, float fineStep = 16.0f, float coarseStep = 128.0f, float fineAlpha = 0.025f,
                       float coarseAlpha = 0.05f) const;
        // The zoom as a percentage at the bottom right, in the theme's TextMuted.
        void PaintZoomLabel(UIDrawList& drawList, const UIRect& bounds, const UITheme& theme) const;
    };
}
