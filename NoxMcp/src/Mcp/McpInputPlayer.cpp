#include "McpInputPlayer.h"

#include <algorithm>

namespace Nox::Mcp
{
    uint64_t InputPlayer::Add(Hold hold)
    {
        Active active;
        active.Id = m_NextId++;
        active.Input = std::move(hold);
        active.Input.Duration = std::max(0.0, active.Input.Duration);
        m_Holds.push_back(std::move(active));
        return m_Holds.back().Id;
    }

    void InputPlayer::Press(Frame& frame, const Hold& hold, bool down)
    {
        auto change = [down](std::map<int, int>& counts, std::vector<std::pair<int, bool>>& changes, int code)
        {
            int& count = counts[code];
            const bool wasDown = count > 0;
            count = std::max(0, count + (down ? 1 : -1));
            if (wasDown != (count > 0))
                changes.emplace_back(code, count > 0);
        };
        // (a key named twice in one hold counts once)
        std::vector<int> keys = hold.Keys, buttons = hold.Buttons;
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::sort(buttons.begin(), buttons.end());
        buttons.erase(std::unique(buttons.begin(), buttons.end()), buttons.end());
        for (int key : keys)
            change(m_KeyCount, frame.Keys, key);
        for (int button : buttons)
            change(m_ButtonCount, frame.Buttons, button);
    }

    InputPlayer::Frame InputPlayer::Advance(double now)
    {
        Frame frame;
        // Released first: holds that were down for at least a frame and whose time is up.
        for (auto it = m_Holds.begin(); it != m_Holds.end();)
        {
            if (it->Started && it->Frames >= 1 && now - it->Start >= it->Input.Duration)
            {
                // (motion a late frame did not reach yet still arrives in full)
                frame.MoveX += it->Input.MoveX * (1.0f - it->Reached);
                frame.MoveY += it->Input.MoveY * (1.0f - it->Reached);
                frame.Scroll += it->Input.Scroll * (1.0f - it->Reached);
                Press(frame, it->Input, false);
                frame.Finished.push_back(it->Id);
                it = m_Holds.erase(it);
            }
            else
                ++it;
        }
        for (Active& active : m_Holds)
        {
            if (!active.Started)
            {
                active.Started = true;
                active.Start = now;
                Press(frame, active.Input, true);
            }
            // How far through its time the hold is: the motion that belongs to the time gone by so far (a hold of no time: all of it in
            // its one frame).
            float fraction = active.Input.Duration <= 0.0 ? 1.0f : static_cast<float>(std::min(1.0, (now - active.Start) / active.Input.Duration));
            fraction = std::max(fraction, active.Reached); // (a clock that steps back sends nothing twice)
            frame.MoveX += active.Input.MoveX * (fraction - active.Reached);
            frame.MoveY += active.Input.MoveY * (fraction - active.Reached);
            frame.Scroll += active.Input.Scroll * (fraction - active.Reached);
            active.Reached = fraction;
            ++active.Frames;
        }
        return frame;
    }

    InputPlayer::Frame InputPlayer::Clear()
    {
        Frame frame;
        for (Active& active : m_Holds)
        {
            if (active.Started)
                Press(frame, active.Input, false);
            frame.Finished.push_back(active.Id);
        }
        m_Holds.clear();
        return frame;
    }
}
