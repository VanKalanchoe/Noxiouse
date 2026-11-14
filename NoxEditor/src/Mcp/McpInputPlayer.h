#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// inject_input's timing, kept apart from the editor so it can be tested (NoxToolsTest): holds of keys and mouse buttons for a
// time, with mouse motion and wheel notches spread over that time, turned into what changes each frame.
namespace Nox::Mcp
{
    class InputPlayer
    {
    public:
        struct Hold
        {
            std::vector<int> Keys;    // SDL scancodes
            std::vector<int> Buttons; // 1 left, 2 middle, 3 right
            float MoveX = 0.0f, MoveY = 0.0f; // pixels in all, spread over Duration
            float Scroll = 0.0f;              // wheel notches in all, spread over Duration
            double Duration = 0.0;            // seconds; 0: one frame
        };

        // What to tell the input system this frame.
        struct Frame
        {
            std::vector<std::pair<int, bool>> Keys;    // (scancode, down): only where it changes
            std::vector<std::pair<int, bool>> Buttons; // (button, down)
            float MoveX = 0.0f, MoveY = 0.0f;
            float Scroll = 0.0f;
            std::vector<uint64_t> Finished; // holds released this frame
        };

        // Starts on the next Advance. The id names it in Frame::Finished.
        uint64_t Add(Hold hold);
        // Once a frame, `now` in seconds (any steady clock). A hold is down from the frame after it was added for at least one frame
        // and until its Duration has passed; it is released in the first frame after that. Keys or buttons two holds share stay down
        // until both let go.
        Frame Advance(double now);
        // Lets go of everything (what is down comes back as releases); the holds that were going are listed as finished.
        Frame Clear();

        bool Empty() const { return m_Holds.empty(); }
        bool IsDown(int key) const { auto it = m_KeyCount.find(key); return it != m_KeyCount.end() && it->second > 0; }

    private:
        struct Active
        {
            uint64_t Id = 0;
            Hold Input;
            bool Started = false;
            double Start = 0.0;
            int Frames = 0;
            float Reached = 0.0f; // the part of its motion sent so far (0..1)
        };
        void Press(Frame& frame, const Hold& hold, bool down);

        std::vector<Active> m_Holds;
        std::map<int, int> m_KeyCount, m_ButtonCount;
        uint64_t m_NextId = 1;
    };
}
