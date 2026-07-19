#pragma once

#include <glm/glm.hpp>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

namespace Nox
{
    class Input
    {
    public:  //replace with custom keycode
        static bool IsKeyPressed(SDL_Keycode key);

        static bool IsMouseButtonPressed(SDL_MouseButtonFlags button);
        static glm::vec2 GetMousePosition();
        static float GetMouseX();
        static float GetMouseY();
        static float x;
        static float y;
    };
}
