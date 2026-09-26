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

        // What game scripts may read. The editor turns them off while the viewport is not the target (a click in another panel
        // must not shoot); a standalone game leaves them on. The C# Input API asks; the engine's own code does not.
        static void SetGameInputEnabled(bool keys, bool mouse) { s_GameKeysEnabled = keys; s_GameMouseEnabled = mouse; }
        static bool GameKeysEnabled() { return s_GameKeysEnabled; }
        static bool GameMouseEnabled() { return s_GameMouseEnabled; }
    private:
        static inline bool s_GameKeysEnabled = true;
        static inline bool s_GameMouseEnabled = true;
    };
}
