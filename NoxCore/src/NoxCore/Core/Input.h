#pragma once

#include <glm/glm.hpp>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_mouse.h>

namespace Nox
{
    class Input
    {
    public:  //replace with custom keycode
        static bool IsKeyPressed(SDL_Scancode key);

        static bool IsMouseButtonPressed(SDL_MouseButtonFlags button);
        static glm::vec2 GetMousePosition();
        static float GetMouseX();
        static float GetMouseY();
        static float x;
        static float y;

        // Restores the cursor to a remembered position (the editor camera's fly mode: back to wherever it was hidden from, once
        // it's shown again). The hiding itself goes through ImGui's own cursor state (ImGui::SetMouseCursor(..._None)), not a raw
        // SDL call: ImGui's SDL backend re-asserts the cursor every frame in ImGui_ImplSDL3_NewFrame regardless, so telling it
        // what to do is the only way that doesn't race it.
        static void WarpMouseInWindow(float x, float y);

        // True relative mouse capture (SDL_SetWindowRelativeMouseMode): grabs the OS cursor to this window and stops it
        // moving at all, unlike the ImGui-cursor-hide trick above, which only hides the cursor's icon while the real OS
        // cursor keeps moving underneath -- letting it wander onto, and click, other panels while the camera flies. The
        // editor camera enables this only while actually flying (RMB + WASD), and reads motion through
        // GetRelativeMouseDelta while it is on, never through GetMousePosition (which SDL does not keep moving in this mode).
        // Leaving it puts the cursor back where it was when it was entered (Unreal's viewport too): SDL would leave it where its
        // virtual position had wandered to meanwhile.
        static void SetRelativeMouseMode(bool enabled);
        static bool IsRelativeMouseMode();
        static glm::vec2 GetRelativeMouseDelta();

        // Game scripts' mouse (C# Input.MouseDelta / ScrollDelta): what the window's events moved during the last frame, in pixels
        // (the wheel in notches), summed by Application and taken at the start of each frame. A sum of its own, not SDL's relative
        // state: the editor camera drains that one (GetRelativeMouseDelta), and neither may take the other's motion.
        static void AddMouseMotion(float dx, float dy) { s_PendingMouseDelta += glm::vec2(dx, dy); }
        static void AddMouseWheel(float dy) { s_PendingScroll += dy; }
        static void BeginFrame()
        {
            s_FrameMouseDelta = s_PendingMouseDelta;
            s_FrameScroll = s_PendingScroll;
            s_PendingMouseDelta = glm::vec2(0.0f);
            s_PendingScroll = 0.0f;
        }
        static glm::vec2 GetFrameMouseDelta() { return s_FrameMouseDelta; }
        static float GetFrameScroll() { return s_FrameScroll; }

        // What game scripts may read. The editor turns them off while the viewport is not the target (a click in another panel
        // must not shoot); a standalone game leaves them on. The C# Input API asks; the engine's own code does not.
        static void SetGameInputEnabled(bool keys, bool mouse) { s_GameKeysEnabled = keys; s_GameMouseEnabled = mouse; }
        static bool GameKeysEnabled() { return s_GameKeysEnabled; }
        static bool GameMouseEnabled() { return s_GameMouseEnabled; }
    private:
        static inline bool s_GameKeysEnabled = true;
        static inline bool s_GameMouseEnabled = true;
        static inline glm::vec2 s_RelativeModeStart{ 0.0f };
        static inline glm::vec2 s_PendingMouseDelta{ 0.0f };
        static inline glm::vec2 s_FrameMouseDelta{ 0.0f };
        static inline float s_PendingScroll = 0.0f;
        static inline float s_FrameScroll = 0.0f;
    };
}
