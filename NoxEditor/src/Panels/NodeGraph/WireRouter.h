#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

// Orthogonal wire routing for the node graph canvases: wires run in horizontal and vertical legs with rounded corners, around the
// nodes instead of through them, and wires that would run along the same line are spread into parallel lanes. The approach of
// orthogonal connector routers (libavoid, used by Inkscape / Dunnart; yEd's channel router): a shortest path with a cost per corner
// over a sparse grid made of the lines along the node edges, then a separation pass ("nudging") for shared lines. Canvas space.
namespace Nox::WireRouting
{
    struct Rect
    {
        ImVec2 Min, Max;
    };

    struct RouteSettings
    {
        float Stub = 16.0f;     // straight out of an output pin / into an input pin before the first / after the last corner
        float Margin = 12.0f;   // the lanes beside a node run this far from its edge
        float BendCost = 24.0f; // what a corner costs, in canvas units of length
        float Reach = 160.0f;   // how far around its two ends a wire first looks for nodes in its way (more are added if it meets them)
    };

    // A route from an output pin's wire end `start` (on its node's right edge: the wire leaves to the right) to an input pin's `end`
    // (on its node's left edge: the wire enters from the left), around `boxes` (every node on the canvas, the two ends' own included).
    // The points where the direction changes, `start` first and `end` last.
    std::vector<ImVec2> RouteWire(ImVec2 start, ImVec2 end, const std::vector<Rect>& boxes, const RouteSettings& settings);

    // A route as its free coordinates, which is how a route the user shaped by hand is kept: the x of the first vertical leg, the y
    // of the horizontal leg after it, the x of the next vertical leg, ... (the first and last horizontal legs are on the pins' rows,
    // so they are not stored). A straight wire has none.
    std::vector<float> RouteCoordinates(const std::vector<ImVec2>& points);
    // The route those coordinates describe between `start` and `end` (a vertical leg at the middle when `coordinates` is empty and
    // the two pins are not on one row).
    std::vector<ImVec2> RouteFromCoordinates(ImVec2 start, ImVec2 end, const std::vector<float>& coordinates);

    // Drops repeated points and the middle one of three in a line.
    void Simplify(std::vector<ImVec2>& points);

    // Spreads the legs of different wires that run along the same line (within `spacing`) into parallel lanes `spacing` apart, in an
    // order that keeps them from crossing where they turn off. Wires of one net (the same output pin) may share a line: they carry the
    // same value, like a bus. The first and last leg of every wire stay on their pin's row.
    void SeparateOverlaps(std::vector<std::vector<ImVec2>>& wires, const std::vector<uint64_t>& nets, float spacing);

    float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b);
    // The index of the leg of `points` nearest to `p` and its distance.
    int NearestSegment(const std::vector<ImVec2>& points, ImVec2 p, float* outDistance = nullptr);

    // A polyline with its corners rounded (each by up to `radius`, at most half of the shorter leg).
    void StrokeRoundedPolyline(ImDrawList* drawList, const std::vector<ImVec2>& points, ImU32 color, float thickness, float radius);
}
