#pragma once

#include <array>
#include <cstdint>

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
            s_InjectedFrameMouseDelta = glm::vec2(0.0f);
            s_InjectedFrameScroll = 0.0f;
        }
        static glm::vec2 GetFrameMouseDelta() { return s_FrameMouseDelta; }
        static float GetFrameScroll() { return s_FrameScroll; }

        // What game scripts may read. The editor turns them off while the viewport is not the target (a click in another panel
        // must not shoot); a standalone game leaves them on. The C# Input API asks; the engine's own code does not.
        static void SetGameInputEnabled(bool keys, bool mouse) { s_GameKeysEnabled = keys; s_GameMouseEnabled = mouse; }
        static bool GameKeysEnabled() { return s_GameKeysEnabled; }
        static bool GameMouseEnabled() { return s_GameMouseEnabled; }

        // Automation input (the editor's MCP inject_input; Unreal's automation input): keys and mouse buttons held as if pressed, and
        // mouse motion / wheel notches, seen by game scripts only -- the editor's own camera, gizmos and panels read the real devices.
        // Not gated by SetGameInputEnabled: an agent drives the game while the editor window is not the one in front. Whoever injects
        // releases what it holds (ClearInjected lets go of everything: leaving Play, the MCP server stopping).
        static void SetInjectedKey(SDL_Scancode key, bool down)
        {
            if (key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT)
                s_InjectedKeys[key] = down;
        }
        static void SetInjectedMouseButton(int button, bool down) // 1 left, 2 middle, 3 right (SDL's numbering, C# MouseButton)
        {
            if (button >= 1 && button <= 5)
                s_InjectedButtons = down ? (s_InjectedButtons | SDL_BUTTON_MASK(button)) : (s_InjectedButtons & ~SDL_BUTTON_MASK(button));
        }
        // Motion and wheel count in this frame (inject before the game updates, as the editor does at the start of its frame).
        static void AddInjectedMouseMotion(float dx, float dy) { s_InjectedFrameMouseDelta += glm::vec2(dx, dy); }
        static void AddInjectedScroll(float dy) { s_InjectedFrameScroll += dy; }
        static void ClearInjected()
        {
            s_InjectedKeys.fill(false);
            s_InjectedButtons = 0;
            s_InjectedFrameMouseDelta = glm::vec2(0.0f);
            s_InjectedFrameScroll = 0.0f;
        }

        // What game scripts read (the C# Input API): the real devices while the game has them (SetGameInputEnabled), plus the
        // injected input.
        static bool IsGameKeyDown(SDL_Scancode key)
        {
            const bool injected = key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && s_InjectedKeys[key];
            return injected || (s_GameKeysEnabled && IsKeyPressed(key));
        }
        static bool IsGameMouseButtonDown(int button)
        {
            const bool injected = button >= 1 && button <= 5 && (s_InjectedButtons & SDL_BUTTON_MASK(button)) != 0;
            return injected || (s_GameMouseEnabled && IsMouseButtonPressed(static_cast<SDL_MouseButtonFlags>(button)));
        }
        static glm::vec2 GetGameMouseDelta() { return (s_GameMouseEnabled ? s_FrameMouseDelta : glm::vec2(0.0f)) + s_InjectedFrameMouseDelta; }
        static float GetGameScroll() { return (s_GameMouseEnabled ? s_FrameScroll : 0.0f) + s_InjectedFrameScroll; }
    private:
        static inline bool s_GameKeysEnabled = true;
        static inline bool s_GameMouseEnabled = true;
        static inline glm::vec2 s_RelativeModeStart{ 0.0f };
        static inline glm::vec2 s_PendingMouseDelta{ 0.0f };
        static inline glm::vec2 s_FrameMouseDelta{ 0.0f };
        static inline float s_PendingScroll = 0.0f;
        static inline float s_FrameScroll = 0.0f;
        static inline std::array<bool, SDL_SCANCODE_COUNT> s_InjectedKeys{};
        static inline uint32_t s_InjectedButtons = 0;
        static inline glm::vec2 s_InjectedFrameMouseDelta{ 0.0f };
        static inline float s_InjectedFrameScroll = 0.0f;
    };
}
