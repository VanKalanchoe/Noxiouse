#pragma once

#include "Event.h"

#include <cstdint>
#include <format>
#include <utility>

namespace Nox {

	class ExternalFileDropEvent : public Event
	{
	public:
		 explicit ExternalFileDropEvent(std::string path)
			: m_Path(std::move(path)) {}

		const std::string& GetPath() const { return m_Path; }

		EVENT_CLASS_TYPE(ExternalFileDrop)
		EVENT_CLASS_CATEGORY(EventCategoryApplication | EventCategoryInput)
	private:
		std::string m_Path;
	};

	//
	// Key Events
	//

	class KeyEvent : public Event
	{
	public:
		inline int GetKeyCode() const { return m_KeyCode; }
		
		EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)
	protected:
		KeyEvent(int keycode)
			: m_KeyCode(keycode) {}

		int m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		KeyPressedEvent(int keycode, bool isRepeat, uint16_t modifiers = 0)
			: KeyEvent(keycode), m_IsRepeat(isRepeat), m_Modifiers(modifiers) {}

		inline bool IsRepeat() const { return m_IsRepeat; }
		// The modifier keys held when this key went down (SDL_Keymod bits: SDL_KMOD_CTRL, SDL_KMOD_SHIFT, ...). Shortcuts read
		// these, not the live keyboard state: events are handled once per frame, and at a low frame rate a modifier can already
		// be released by then (Ctrl+Shift+Z read as Ctrl+Z).
		inline uint16_t GetModifiers() const { return m_Modifiers; }

		std::string ToString() const override
		{
			return std::format("KeyPressedEvent: {} (repeat={})", m_KeyCode, m_IsRepeat);
		}

		EVENT_CLASS_TYPE(KeyPressed)
	private:
		bool m_IsRepeat;
		uint16_t m_Modifiers;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		KeyReleasedEvent(int keycode)
			: KeyEvent(keycode) {}

		std::string ToString() const override
		{
			return std::format("KeyReleasedEvent: {}", m_KeyCode);
		}

		EVENT_CLASS_TYPE(KeyReleased)
	};

	//
	// Mouse Events
	//

	class MouseMovedEvent : public Event
	{
	public:
		MouseMovedEvent(double x, double y)
			: m_MouseX(x), m_MouseY(y) {}

		inline double GetX() const { return m_MouseX; }
		inline double GetY() const { return m_MouseY; }

		std::string ToString() const override
		{
			return std::format("MouseMovedEvent: {}, {}", m_MouseX, m_MouseY);
		}

		EVENT_CLASS_TYPE(MouseMoved)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		double m_MouseX, m_MouseY;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(double xOffset, double yOffset)
			: m_XOffset(xOffset), m_YOffset(yOffset) {}

		inline double GetXOffset() const { return m_XOffset; }
		inline double GetYOffset() const { return m_YOffset; }

		std::string ToString() const override
		{
			return std::format("MouseScrolledEvent: {}, {}", m_XOffset, m_YOffset);
		}

		EVENT_CLASS_TYPE(MouseScrolled)
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)
	private:
		double m_XOffset, m_YOffset;
	};

	class MouseButtonEvent : public Event
	{
	public:
		inline int GetMouseButton() const { return m_Button; }
		
		EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton)
	protected:
		MouseButtonEvent(int button)
			: m_Button(button) {}

		int m_Button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonPressedEvent(int button)
			: MouseButtonEvent(button) {}

		std::string ToString() const override
		{
			return std::format("MouseButtonPressedEvent: {}", m_Button);
		}

		EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		MouseButtonReleasedEvent(int button)
			: MouseButtonEvent(button) {}

		std::string ToString() const override
		{
			return std::format("MouseButtonReleasedEvent: {}", m_Button);
		}

		EVENT_CLASS_TYPE(MouseButtonReleased)
	};
	
}
