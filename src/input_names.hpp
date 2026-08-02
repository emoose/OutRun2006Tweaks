#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <string_view>
#include <optional>
#include <string.h> // stricmp

// fixups for SDL3 sillyness (mirrors input_manager.hpp)
#ifndef SDL_GAMEPAD_BUTTON_A
#define SDL_GAMEPAD_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_GAMEPAD_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_GAMEPAD_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_GAMEPAD_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH
#endif

//
// All input naming lives here as data.
//
// Two name spaces, deliberately kept separate:
//   ini      - what we read/write in OutRun2006Tweaks.input.ini. Stable, must
//              not change or existing user binding files break.
//   display  - what the binding UI shows, varies by controller style.
//
namespace InputNames
{
	enum class PadStyle { Xbox, PlayStation, Nintendo };

	inline PadStyle styleFor(SDL_GamepadType type)
	{
		switch (type)
		{
		case SDL_GAMEPAD_TYPE_PS3:
		case SDL_GAMEPAD_TYPE_PS4:
		case SDL_GAMEPAD_TYPE_PS5:
			return PadStyle::PlayStation;
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
			return PadStyle::Nintendo;
		default:
			return PadStyle::Xbox;
		}
	}

	struct ButtonNames
	{
		SDL_GamepadButton button;
		const char* ini;       // nullptr = cannot be written to the ini
		const char* universal; // non-null = identical on every pad style
		const char* xbox;
		const char* ps;
		const char* nintendo;
	};

	// Face-button rows are laid out by physical position, so the Nintendo
	// column is intentionally "swapped" relative to Xbox (SDL reports by
	// position, Nintendo labels them the other way round).
	inline constexpr ButtonNames kButtons[] = {
		{ SDL_GAMEPAD_BUTTON_A,              "A",          nullptr,        "A",             "Cross",    "B"     },
		{ SDL_GAMEPAD_BUTTON_B,              "B",          nullptr,        "B",             "Circle",   "A"     },
		{ SDL_GAMEPAD_BUTTON_X,              "X",          nullptr,        "X",             "Square",   "Y"     },
		{ SDL_GAMEPAD_BUTTON_Y,              "Y",          nullptr,        "Y",             "Triangle", "X"     },
		{ SDL_GAMEPAD_BUTTON_BACK,           "Back",       nullptr,        "Back",          "Select",   "Minus" },
		{ SDL_GAMEPAD_BUTTON_START,          "Start",      nullptr,        "Start",         "Start",    "Plus"  },
		{ SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  "LB",         nullptr,        "Left Bumper",   "L1",       "L"     },
		{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB",         nullptr,        "Right Bumper",  "R1",       "R"     },
		{ SDL_GAMEPAD_BUTTON_LEFT_STICK,     "L3",         nullptr,        "LS",            "L3",       "LS"    },
		{ SDL_GAMEPAD_BUTTON_RIGHT_STICK,    "R3",         nullptr,        "RS",            "R3",       "RS"    },
		{ SDL_GAMEPAD_BUTTON_DPAD_UP,        "DPad-Up",    "D-Pad Up",     nullptr,         nullptr,    nullptr },
		{ SDL_GAMEPAD_BUTTON_DPAD_DOWN,      "DPad-Down",  "D-Pad Down",   nullptr,         nullptr,    nullptr },
		{ SDL_GAMEPAD_BUTTON_DPAD_LEFT,      "DPad-Left",  "D-Pad Left",   nullptr,         nullptr,    nullptr },
		{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     "DPad-Right", "D-Pad Right",  nullptr,         nullptr,    nullptr },
	};

	struct AxisNames
	{
		SDL_GamepadAxis axis;
		const char* ini;
		// Sticks use a universal name and get a +/- direction suffix appended.
		// Triggers are named per style and never take a suffix.
		const char* universal;
		const char* xbox;
		const char* ps;
		const char* nintendo;
	};

	inline constexpr AxisNames kAxes[] = {
		{ SDL_GAMEPAD_AXIS_LEFTX,         "LS-X", "Left Stick X",  nullptr,          nullptr, nullptr },
		{ SDL_GAMEPAD_AXIS_LEFTY,         "LS-Y", "Left Stick Y",  nullptr,          nullptr, nullptr },
		{ SDL_GAMEPAD_AXIS_RIGHTX,        "RS-X", "Right Stick X", nullptr,          nullptr, nullptr },
		{ SDL_GAMEPAD_AXIS_RIGHTY,        "RS-Y", "Right Stick Y", nullptr,          nullptr, nullptr },
		{ SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  "LT",   nullptr,         "Left Trigger",   "L2",    "ZL"    },
		{ SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "RT",   nullptr,         "Right Trigger",  "R2",    "ZR"    },
	};

	// PS3 kept Start/Select; PS4 and PS5 renamed them, and differ from each
	// other on Back. Only these two buttons vary within a style, so they're an
	// override on top of kButtons rather than another column.
	inline const char* psSubtypeOverride(SDL_GamepadButton button, SDL_GamepadType type)
	{
		if (type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5)
		{
			if (button == SDL_GAMEPAD_BUTTON_START)
				return "Options";
			if (button == SDL_GAMEPAD_BUTTON_BACK)
				return (type == SDL_GAMEPAD_TYPE_PS4) ? "Share" : "Create";
		}
		return nullptr;
	}

	inline const ButtonNames* findButton(SDL_GamepadButton button)
	{
		for (const auto& row : kButtons)
			if (row.button == button)
				return &row;
		return nullptr;
	}

	inline const AxisNames* findAxis(SDL_GamepadAxis axis)
	{
		for (const auto& row : kAxes)
			if (row.axis == axis)
				return &row;
		return nullptr;
	}

	inline std::optional<SDL_GamepadButton> buttonFromIni(std::string_view name)
	{
		for (const auto& row : kButtons)
			if (row.ini && !stricmp(row.ini, std::string(name).c_str()))
				return row.button;
		return std::nullopt;
	}

	inline std::optional<SDL_GamepadAxis> axisFromIni(std::string_view name)
	{
		for (const auto& row : kAxes)
			if (row.ini && !stricmp(row.ini, std::string(name).c_str()))
				return row.axis;
		return std::nullopt;
	}

	inline std::string iniNameForButton(SDL_GamepadButton button)
	{
		const auto* row = findButton(button);
		return (row && row->ini) ? row->ini : "";
	}

	inline std::string iniNameForAxis(SDL_GamepadAxis axis)
	{
		const auto* row = findAxis(axis);
		return (row && row->ini) ? row->ini : "";
	}

	inline std::string displayNameForButton(SDL_GamepadButton button, SDL_GamepadType padType)
	{
		const auto* row = findButton(button);
		if (!row)
			return "Unknown Button";

		if (row->universal)
			return row->universal;

		const PadStyle style = styleFor(padType);
		if (style == PadStyle::PlayStation)
		{
			if (const char* override = psSubtypeOverride(button, padType))
				return override;
			return row->ps ? row->ps : "Unknown Button";
		}
		if (style == PadStyle::Nintendo)
			return row->nintendo ? row->nintendo : "Unknown Button";

		return row->xbox ? row->xbox : "Unknown Button";
	}

	// direction is the +/- suffix shown for stick axes; steering actions omit
	// it because both directions are meaningful for a single binding.
	inline std::string displayNameForAxis(SDL_GamepadAxis axis, SDL_GamepadType padType,
		bool negated, bool isSteerAction)
	{
		const std::string direction = isSteerAction ? "" : (negated ? "+" : "-");

		const auto* row = findAxis(axis);
		if (!row)
			return "Unknown Axis" + direction;

		if (row->universal)
			return row->universal + direction;

		switch (styleFor(padType))
		{
		case PadStyle::PlayStation: return row->ps ? row->ps : ("Unknown Axis" + direction);
		case PadStyle::Nintendo:    return row->nintendo ? row->nintendo : ("Unknown Axis" + direction);
		default:                    return row->xbox ? row->xbox : ("Unknown Axis" + direction);
		}
	}
}
