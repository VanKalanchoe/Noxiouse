#include "WireRouter.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace Nox::WireRouting
{
    namespace
    {
        ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
        ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
        ImVec2 Scale(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }
        bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }
        bool Near(ImVec2 a, ImVec2 b) { return Near(a.x, b.x) && Near(a.y, b.y); }
        float Sign(float v) { return v > 0.01f ? 1.0f : (v < -0.01f ? -1.0f : 0.0f); }

        Rect Inflate(const Rect& r, float by) { return { ImVec2(r.Min.x - by, r.Min.y - by), ImVec2(r.Max.x + by, r.Max.y + by) }; }
        bool Overlaps(const Rect& a, const Rect& b) { return a.Max.x > b.Min.x && a.Min.x < b.Max.x && a.Max.y > b.Min.y && a.Min.y < b.Max.y; }

        // Does the axis-aligned segment a-b run through the inside of `r` (its edges do not count)?
        bool CrossesInterior(ImVec2 a, ImVec2 b, const Rect& r)
        {
            const float minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x);
            const float minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);
            return maxX > r.Min.x + 0.01f && minX < r.Max.x - 0.01f && maxY > r.Min.y + 0.01f && minY < r.Max.y - 0.01f;
        }

        void SortUnique(std::vector<float>& values)
        {
            std::sort(values.begin(), values.end());
            std::vector<float> unique;
            unique.reserve(values.size());
            for (float v : values)
                if (unique.empty() || v - unique.back() > 0.5f)
                    unique.push_back(v);
            values = std::move(unique);
        }

        // Lines through the middle of the wide gaps between the lines already there: a wire between two columns of nodes runs down
        // the middle of the channel rather than along one side of it.
        void AddChannelMiddles(std::vector<float>& lines, float minimumGap)
        {
            const size_t count = lines.size();
            for (size_t i = 0; i + 1 < count; ++i)
                if (lines[i + 1] - lines[i] > minimumGap)
                    lines.push_back(0.5f * (lines[i] + lines[i + 1]));
            SortUnique(lines);
        }

        size_t IndexOf(const std::vector<float>& lines, float value)
        {
            size_t best = 0;
            for (size_t i = 1; i < lines.size(); ++i)
                if (std::abs(lines[i] - value) < std::abs(lines[best] - value))
                    best = i;
            return best;
        }

        enum Direction { Right = 0, Left = 1, Down = 2, Up = 3 };
        constexpr int kStepX[4] = { 1, -1, 0, 0 };
        constexpr int kStepY[4] = { 0, 0, 1, -1 };
        constexpr int kOpposite[4] = { Left, Right, Up, Down };

        // The cheapest path from `a` (reached going right, out of the source pin's stub) to `b` (left going right, into the target
        // pin's stub) over the grid of lines along the inflated edges of the `used` boxes. Cost: length; running inside a box's
        // margin costs a few times its length, through a box far more than any detour; each corner `BendCost`; and a tiny pull of
        // vertical legs toward the middle between the two pins, so equal routes settle on the tidy one.
        std::vector<ImVec2> Search(ImVec2 a, ImVec2 b, const std::vector<Rect>& boxes, const std::vector<int>& used, const RouteSettings& settings)
        {
            constexpr float kNearFactor = 3.0f;    // extra cost per unit of length inside a node's margin
            constexpr float kInsideFactor = 50.0f; // and inside the node
            constexpr float kInsideFixed = 2000.0f;
            constexpr float kColumnPull = 0.0002f;

            std::vector<Rect> inner, outer;
            inner.reserve(used.size());
            outer.reserve(used.size());
            std::vector<float> xs{ a.x, b.x, 0.5f * (a.x + b.x) };
            std::vector<float> ys{ a.y, b.y };
            for (int k : used)
            {
                inner.push_back(boxes[k]);
                outer.push_back(Inflate(boxes[k], settings.Margin));
                xs.push_back(outer.back().Min.x);
                xs.push_back(outer.back().Max.x);
                ys.push_back(outer.back().Min.y);
                ys.push_back(outer.back().Max.y);
            }
            SortUnique(xs);
            SortUnique(ys);
            AddChannelMiddles(xs, 2.0f * settings.Margin);
            AddChannelMiddles(ys, 2.0f * settings.Margin);

            const int nx = static_cast<int>(xs.size());
            const int ny = static_cast<int>(ys.size());
            const int goal = static_cast<int>(IndexOf(ys, b.y)) * nx + static_cast<int>(IndexOf(xs, b.x));
            const int origin = static_cast<int>(IndexOf(ys, a.y)) * nx + static_cast<int>(IndexOf(xs, a.x));
            const float middleX = 0.5f * (a.x + b.x);

            // The extra cost of each grid edge: rightCost[j * nx + i] for (i, j)-(i + 1, j), downCost for (i, j)-(i, j + 1).
            auto edgeCost = [&](ImVec2 p, ImVec2 q)
            {
                const float length = std::abs(q.x - p.x) + std::abs(q.y - p.y);
                float cost = length;
                for (size_t k = 0; k < inner.size(); ++k)
                {
                    if (CrossesInterior(p, q, inner[k]))
                        cost += kInsideFixed + kInsideFactor * length;
                    else if (CrossesInterior(p, q, outer[k]))
                        cost += kNearFactor * length;
                }
                if (Near(p.x, q.x))
                    cost += kColumnPull * length * std::abs(p.x - middleX);
                return cost;
            };
            std::vector<float> rightCost(static_cast<size_t>(nx) * ny, 0.0f), downCost(static_cast<size_t>(nx) * ny, 0.0f);
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i)
                {
                    if (i + 1 < nx)
                        rightCost[j * nx + i] = edgeCost(ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j]));
                    if (j + 1 < ny)
                        downCost[j * nx + i] = edgeCost(ImVec2(xs[i], ys[j]), ImVec2(xs[i], ys[j + 1]));
                }

            // A* over (grid point, direction of arrival).
            const size_t stateCount = static_cast<size_t>(nx) * ny * 4;
            std::vector<float> best(stateCount, FLT_MAX);
            std::vector<int> parent(stateCount, -1);
            struct Open
            {
                float F, G;
                int State;
                bool operator>(const Open& other) const { return F > other.F; }
            };
            std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
            auto heuristic = [&](int node) { return std::abs(xs[node % nx] - b.x) + std::abs(ys[node / nx] - b.y); };
            const int startState = origin * 4 + Right;
            best[startState] = 0.0f;
            open.push({ heuristic(origin), 0.0f, startState });
            float goalCost = FLT_MAX;
            int goalState = -1;
            while (!open.empty())
            {
                const Open top = open.top();
                open.pop();
                if (top.F >= goalCost)
                    break;
                if (top.G > best[top.State])
                    continue;
                const int node = top.State / 4;
                const int direction = top.State % 4;
                if (node == goal && direction != Left)
                {
                    // The last leg goes right into the pin: arriving vertically is one more corner.
                    const float total = top.G + (direction == Right ? 0.0f : settings.BendCost);
                    if (total < goalCost)
                    {
                        goalCost = total;
                        goalState = top.State;
                    }
                    continue;
                }
                const int i = node % nx, j = node / nx;
                for (int next = 0; next < 4; ++next)
                {
                    if (next == kOpposite[direction])
                        continue;
                    const int ni = i + kStepX[next], nj = j + kStepY[next];
                    if (ni < 0 || nj < 0 || ni >= nx || nj >= ny)
                        continue;
                    float cost = 0.0f;
                    switch (next)
                    {
                    case Right: cost = rightCost[j * nx + i]; break;
                    case Left:  cost = rightCost[j * nx + ni]; break;
                    case Down:  cost = downCost[j * nx + i]; break;
                    case Up:    cost = downCost[nj * nx + i]; break;
                    }
                    if (next != direction)
                        cost += settings.BendCost;
                    const float g = top.G + cost;
                    const int nextNode = nj * nx + ni;
                    const int state = nextNode * 4 + next;
                    if (g < best[state])
                    {
                        best[state] = g;
                        parent[state] = top.State;
                        open.push({ g + heuristic(nextNode), g, state });
                    }
                }
            }

            std::vector<ImVec2> path;
            if (goalState < 0)
                return { a, b };
            for (int state = goalState; state >= 0; state = parent[state])
            {
                const int node = state / 4;
                path.push_back(ImVec2(xs[node % nx], ys[node / nx]));
            }
            std::reverse(path.begin(), path.end());
            // The grid points nearest the stubs' ends are the stubs' ends (both are grid lines), but say so exactly.
            path.front() = a;
            path.back() = b;
            return path;
        }
    }

    void Simplify(std::vector<ImVec2>& points)
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            std::vector<ImVec2> out;
            out.reserve(points.size());
            for (const ImVec2& p : points)
            {
                if (!out.empty() && Near(out.back(), p))
                {
                    changed = true;
                    continue;
                }
                if (out.size() >= 2)
                {
                    const ImVec2 a = out[out.size() - 2], b = out.back();
                    if ((Near(a.x, b.x) && Near(b.x, p.x)) || (Near(a.y, b.y) && Near(b.y, p.y)))
                    {
                        out.back() = p;
                        changed = true;
                        continue;
                    }
                }
                out.push_back(p);
            }
            points = std::move(out);
        }
    }

    std::vector<ImVec2> RouteWire(ImVec2 start, ImVec2 end, const std::vector<Rect>& boxes, const RouteSettings& settings)
    {
        const ImVec2 a(start.x + settings.Stub, start.y);
        const ImVec2 b(end.x - settings.Stub, end.y);
        const Rect region{ ImVec2(std::min(a.x, b.x) - settings.Reach, std::min(a.y, b.y) - settings.Reach),
                           ImVec2(std::max(a.x, b.x) + settings.Reach, std::max(a.y, b.y) + settings.Reach) };
        std::vector<int> used;
        std::vector<bool> isUsed(boxes.size(), false);
        for (size_t k = 0; k < boxes.size(); ++k)
            if (Overlaps(Inflate(boxes[k], settings.Margin), region))
            {
                used.push_back(static_cast<int>(k));
                isUsed[k] = true;
            }

        // Nodes near the two ends first; a node farther out that the route then runs into joins them and it is searched again.
        std::vector<ImVec2> points;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            points.clear();
            points.push_back(start);
            const std::vector<ImVec2> path = Search(a, b, boxes, used, settings);
            points.insert(points.end(), path.begin(), path.end());
            points.push_back(end);
            Simplify(points);

            bool added = false;
            for (size_t k = 0; k < boxes.size(); ++k)
            {
                if (isUsed[k])
                    continue;
                const Rect near = Inflate(boxes[k], 0.5f * settings.Margin);
                for (size_t i = 0; i + 1 < points.size(); ++i)
                    if (CrossesInterior(points[i], points[i + 1], near))
                    {
                        used.push_back(static_cast<int>(k));
                        isUsed[k] = true;
                        added = true;
                        break;
                    }
            }
            if (!added)
                break;
        }
        return points;
    }

    std::vector<float> RouteCoordinates(const std::vector<ImVec2>& points)
    {
        // Legs alternate horizontal (even index) / vertical (odd); the first and last are horizontal, on the pins' rows.
        std::vector<float> coordinates;
        if (points.size() < 4 || points.size() % 2 != 0)
            return coordinates;
        for (size_t k = 0; k + 1 < points.size(); ++k)
        {
            const bool vertical = Near(points[k].x, points[k + 1].x);
            const bool horizontal = Near(points[k].y, points[k + 1].y);
            if ((k % 2 == 1 && !vertical) || (k % 2 == 0 && !horizontal))
                return {};
            if (k >= 1 && k + 2 < points.size())
                coordinates.push_back(vertical ? points[k].x : points[k].y);
        }
        return coordinates;
    }

    std::vector<ImVec2> RouteFromCoordinates(ImVec2 start, ImVec2 end, const std::vector<float>& coordinates)
    {
        if (coordinates.empty())
        {
            if (Near(start.y, end.y))
                return { start, end };
            const float middle = 0.5f * (start.x + end.x);
            return { start, ImVec2(middle, start.y), ImVec2(middle, end.y), end };
        }
        // An odd count: vertical, horizontal, ..., vertical.
        const size_t count = coordinates.size() % 2 == 1 ? coordinates.size() : coordinates.size() - 1;
        std::vector<ImVec2> points{ start };
        float y = start.y;
        for (size_t i = 0; i < count; ++i)
        {
            if (i % 2 == 0)
                points.push_back(ImVec2(coordinates[i], y));
            else
            {
                y = coordinates[i];
                points.push_back(ImVec2(points.back().x, y));
            }
        }
        points.push_back(ImVec2(points.back().x, end.y));
        points.push_back(end);
        return points;
    }

    void SeparateOverlaps(std::vector<std::vector<ImVec2>>& wires, const std::vector<uint64_t>& nets, float spacing)
    {
        struct Leg
        {
            int Wire = 0, Index = 0;
            bool Vertical = false;
            float Coord = 0.0f, Lo = 0.0f, Hi = 0.0f, Key = 0.0f;
            uint64_t Net = 0;
        };
        std::vector<Leg> legs;
        for (size_t w = 0; w < wires.size(); ++w)
        {
            const std::vector<ImVec2>& points = wires[w];
            for (size_t i = 1; i + 2 < points.size(); ++i)
            {
                const ImVec2 a = points[i], b = points[i + 1];
                const bool vertical = Near(a.x, b.x) && !Near(a.y, b.y);
                const bool horizontal = Near(a.y, b.y) && !Near(a.x, b.x);
                if (!vertical && !horizontal)
                    continue;
                Leg leg;
                leg.Wire = static_cast<int>(w);
                leg.Index = static_cast<int>(i);
                leg.Vertical = vertical;
                leg.Net = nets[w];
                // Which way the wire turns off at each end of the leg decides its side: of two legs sharing a column that both
                // turn right at the bottom, the one turning lower must be the left one or the other's turn would cross it.
                const bool aFirst = vertical ? a.y < b.y : a.x < b.x;
                const ImVec2 first = aFirst ? a : b, last = aFirst ? b : a;
                const ImVec2 firstNeighbour = aFirst ? points[i - 1] : points[i + 2];
                const ImVec2 lastNeighbour = aFirst ? points[i + 2] : points[i - 1];
                if (vertical)
                {
                    leg.Coord = a.x;
                    leg.Lo = first.y;
                    leg.Hi = last.y;
                    leg.Key = Sign(firstNeighbour.x - first.x) * first.y - Sign(lastNeighbour.x - last.x) * last.y;
                }
                else
                {
                    leg.Coord = a.y;
                    leg.Lo = first.x;
                    leg.Hi = last.x;
                    leg.Key = Sign(firstNeighbour.y - first.y) * first.x - Sign(lastNeighbour.y - last.y) * last.x;
                }
                legs.push_back(leg);
            }
        }

        std::vector<int> parent(legs.size());
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&](int i)
        {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        std::vector<int> order(legs.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int l, int r)
        {
            if (legs[l].Vertical != legs[r].Vertical)
                return legs[l].Vertical;
            return legs[l].Coord < legs[r].Coord;
        });
        // Legs of the same direction closer than `spacing` whose extents overlap run along the same line: one group.
        for (size_t p = 0; p < order.size(); ++p)
            for (size_t q = p + 1; q < order.size(); ++q)
            {
                const Leg& l = legs[order[p]];
                const Leg& r = legs[order[q]];
                if (r.Vertical != l.Vertical || r.Coord - l.Coord >= spacing)
                    break;
                if (std::min(l.Hi, r.Hi) - std::max(l.Lo, r.Lo) > 1.0f)
                    parent[find(order[p])] = find(order[q]);
            }

        std::unordered_map<int, std::vector<int>> groups;
        for (size_t i = 0; i < legs.size(); ++i)
            groups[find(static_cast<int>(i))].push_back(static_cast<int>(i));

        std::vector<float> shift(legs.size(), 0.0f);
        for (const auto& [root, members] : groups)
        {
            struct NetLane
            {
                uint64_t Net;
                float KeySum = 0.0f;
                int Count = 0;
            };
            std::vector<NetLane> lanes;
            float center = 0.0f;
            for (int member : members)
            {
                center += legs[member].Coord;
                auto lane = std::find_if(lanes.begin(), lanes.end(), [&](const NetLane& n) { return n.Net == legs[member].Net; });
                if (lane == lanes.end())
                {
                    lanes.push_back({ legs[member].Net });
                    lane = lanes.end() - 1;
                }
                lane->KeySum += legs[member].Key;
                ++lane->Count;
            }
            center /= static_cast<float>(members.size());
            if (lanes.size() < 2)
            {
                // One net on (nearly) one line: put its legs exactly on one line.
                for (int member : members)
                    shift[member] = center - legs[member].Coord;
                continue;
            }
            std::sort(lanes.begin(), lanes.end(), [](const NetLane& l, const NetLane& r)
            {
                const float lk = l.KeySum / static_cast<float>(l.Count), rk = r.KeySum / static_cast<float>(r.Count);
                return lk != rk ? lk < rk : l.Net < r.Net;
            });
            const float half = 0.5f * static_cast<float>(lanes.size() - 1);
            for (int member : members)
            {
                const auto lane = std::find_if(lanes.begin(), lanes.end(), [&](const NetLane& n) { return n.Net == legs[member].Net; });
                const float offset = (static_cast<float>(lane - lanes.begin()) - half) * spacing;
                shift[member] = center + offset - legs[member].Coord;
            }
        }

        for (size_t i = 0; i < legs.size(); ++i)
        {
            if (shift[i] == 0.0f)
                continue;
            std::vector<ImVec2>& points = wires[legs[i].Wire];
            const int k = legs[i].Index;
            if (legs[i].Vertical)
            {
                points[k].x += shift[i];
                points[k + 1].x += shift[i];
            }
            else
            {
                points[k].y += shift[i];
                points[k + 1].y += shift[i];
            }
        }
    }

    float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
    {
        const ImVec2 ab = Sub(b, a), ap = Sub(p, a);
        const float lengthSq = ab.x * ab.x + ab.y * ab.y;
        const float t = lengthSq > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / lengthSq, 0.0f, 1.0f) : 0.0f;
        const ImVec2 closest = Add(a, Scale(ab, t));
        const ImVec2 d = Sub(p, closest);
        return std::sqrt(d.x * d.x + d.y * d.y);
    }

    int NearestSegment(const std::vector<ImVec2>& points, ImVec2 p, float* outDistance)
    {
        int nearest = -1;
        float distance = FLT_MAX;
        for (size_t i = 0; i + 1 < points.size(); ++i)
        {
            const float d = DistanceToSegment(p, points[i], points[i + 1]);
            if (d < distance)
            {
                distance = d;
                nearest = static_cast<int>(i);
            }
        }
        if (outDistance)
            *outDistance = distance;
        return nearest;
    }

    void StrokeRoundedPolyline(ImDrawList* drawList, const std::vector<ImVec2>& points, ImU32 color, float thickness, float radius)
    {
        if (points.size() < 2)
            return;
        drawList->PathLineTo(points.front());
        for (size_t i = 1; i + 1 < points.size(); ++i)
        {
            const ImVec2 a = points[i - 1], c = points[i], b = points[i + 1];
            const float inLength = std::abs(c.x - a.x) + std::abs(c.y - a.y);
            const float outLength = std::abs(b.x - c.x) + std::abs(b.y - c.y);
            const float r = std::min(radius, 0.5f * std::min(inLength, outLength));
            if (r < 0.5f || inLength < 0.01f || outLength < 0.01f)
            {
                drawList->PathLineTo(c);
                continue;
            }
            drawList->PathLineTo(Sub(c, Scale(Sub(c, a), r / inLength)));
            drawList->PathBezierQuadraticCurveTo(c, Add(c, Scale(Sub(b, c), r / outLength)), 6);
        }
        drawList->PathLineTo(points.back());
        drawList->PathStroke(color, thickness); // (ImGui 1.92.8 order: thickness, then flags)
    }
}
