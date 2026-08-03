#include "Input.h"

#include <SDL3/SDL_keyboard.h>

namespace Nox
{
    // Definition of static member variables
    float Input::x = 0.0f;  // Initializing static variable 'x'
    float Input::y = 0.0f;  // Initializing static variable 'y'
    
    bool Input::IsKeyPressed(SDL_Keycode key)
    {
        auto state = SDL_GetKeyboardState(NULL);
        return state[key];
    }
    
    bool Input::IsMouseButtonPressed(SDL_MouseButtonFlags button)
    {
        auto state = SDL_GetMouseState(nullptr, nullptr);

        return SDL_BUTTON_MASK(button) == state;
    }

    glm::vec2 Input::GetMousePosition()
    {
        SDL_GetMouseState(&x,&y);
        return {x, y};
    }

    float Input::GetMouseX()
    {
        return GetMousePosition().x;
    }

    float Input::GetMouseY()
    {
        return GetMousePosition().y;
    }
}
