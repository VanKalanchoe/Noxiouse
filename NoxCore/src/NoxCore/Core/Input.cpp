#include "Input.h"

#include <SDL3/SDL_keyboard.h>
#include "NoxCore/Core/Application.h"

namespace Nox
{
    // Definition of static member variables
    float Input::x = 0.0f;  // Initializing static variable 'x'
    float Input::y = 0.0f;  // Initializing static variable 'y'
    
    bool Input::IsKeyPressed(SDL_Scancode key)
    {
        auto state = SDL_GetKeyboardState(NULL);
        return state[key];
    }
    
    bool Input::IsMouseButtonPressed(SDL_MouseButtonFlags button)
    {
        auto state = SDL_GetMouseState(nullptr, nullptr);

        // Was: `== state`, an exact match against the WHOLE button bitmask -- true only if this was the only button down, so
        // holding right-click while so much as brushing another button (or a stray extra bit some frame) read as "not held"
        // that frame, flickering the fly camera's cursor hide/rotation on and off. A button can be down alongside others.
        return (SDL_BUTTON_MASK(button) & state) != 0;
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

    void Input::WarpMouseInWindow(float x, float y)
    {
        if (SDL_Window* window = SDL_GetMouseFocus())
            SDL_WarpMouseInWindow(window, x, y);
    }

    void Input::SetRelativeMouseMode(bool enabled)
    {
        // Not SDL_GetMouseFocus(): that returns null unless the OS considers this window "mouse-focused" right
        // now, which is not guaranteed true on the very frame a fly session starts (the frame of the RMB press
        // itself) -- silently no-opping the whole call. The app's own window handle is always valid.
        if (SDL_Window* window = Application::Get().getWindow()->getHandle())
            SDL_SetWindowRelativeMouseMode(window, enabled);
    }

    glm::vec2 Input::GetRelativeMouseDelta()
    {
        float dx = 0.0f, dy = 0.0f;
        SDL_GetRelativeMouseState(&dx, &dy);
        return { dx, dy };
    }
}
