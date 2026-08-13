#pragma once

#include <filesystem>

#include "game.hpp"
#include "settings.hpp"

extern void DInput_RegisterNewDevices(); // hooks_input.cpp
extern void SetVibration(int userId, float leftMotor, float rightMotor); // hooks_forcefeedback.cpp
extern void AudioHooks_Update(int numUpdates); // hooks_audio.cpp
extern void CDSwitcher_ReadIni(const std::filesystem::path& iniPath);

namespace Module
{
	// Info about our module
	inline HMODULE DllHandle{ 0 };
	inline std::filesystem::path DllPath{};

	// Info about the module we've been loaded into
	inline HMODULE ExeHandle{ 0 };
	inline std::filesystem::path ExePath{};

	inline std::filesystem::path LogPath{};
	inline std::filesystem::path IniPath{};
	inline std::filesystem::path UserIniPath{};
	inline std::filesystem::path LodIniPath{};
	inline std::filesystem::path OverlayIniPath{};
	inline std::filesystem::path BindingsIniPath{};

	template <typename T>
	inline T* exe_ptr(uintptr_t offset) { if (ExeHandle) return (T*)(((uintptr_t)ExeHandle) + offset); else return nullptr; }
	inline uint8_t* exe_ptr(uintptr_t offset) { if (ExeHandle) return (uint8_t*)(((uintptr_t)ExeHandle) + offset); else return nullptr; }

	template <typename T>
	inline T fn_ptr(uintptr_t offset) { if (ExeHandle) return (T)(((uintptr_t)ExeHandle) + offset); else return nullptr; }

	// Deduce the type by providing it as an argument, no need for ugly decltype stuff
	template <typename T>
	inline T fn_ptr(uintptr_t offset, T& var)
	{
		if (ExeHandle)
			return reinterpret_cast<T>(((uintptr_t)ExeHandle) + offset);
		else
			return nullptr;
	}

	void init();
}

namespace Game
{
	enum class GamepadType
	{
		None,
		PC,
		Xbox,
		PS,
		Switch
	};

	inline static const char* PadTypes[] =
	{
		"None",
		"PC",
		"Xbox",
		"PlayStation",
		"Switch"
	};

	inline std::chrono::system_clock::time_point StartupTime;
	inline float DeltaTime = (1.f / 60.f);

	inline GamepadType CurrentPadType = GamepadType::PC;
	inline GamepadType ForcedPadType = GamepadType::None;
};

// Each setting is defined in the file that implements the feature it belongs
// to, next to the hook that reads it. Only the ones read from more than one
// file are declared here.
namespace Settings
{
	extern Setting<int> FramerateLimit;                    // hooks_framerate.cpp
	extern Setting<int> FramerateFastLoad;                 // hooks_framerate.cpp
	extern Setting<bool> FramerateInterpolation;           // hooks_framerate.cpp

	extern Setting<float> FramerateInterpolationDebugAlpha; // interpolation.cpp
	extern Setting<bool> FramerateInterpolationDebugLog;    // interpolation.cpp
	extern Setting<std::string> HudToggleKey;               // hooks_input.cpp

	extern Setting<int> UIScalingMode;                     // hooks_uiscaling.cpp
	extern Setting<int> UILetterboxing;                    // hooks_uiscaling.cpp
	extern Setting<int> SkyGlowFactor;                     // hooks_graphics.cpp
	extern Setting<bool> SkyGlowTwoStep;                   // hooks_graphics.cpp
	extern Setting<bool> UseHiDefCharacters;               // hooks_graphics.cpp
	extern Setting<int> DrawDistanceIncrease;              // hooks_drawdistance.cpp
	extern Setting<int> DrawDistanceBehind;                // hooks_drawdistance.cpp

	extern Setting<bool> AllowFLAC;                        // hooks_flac.cpp

	extern Setting<bool> UseNewInput;                      // input_manager.cpp
	extern Setting<bool> BypassGameSensitivity;            // input_manager.cpp
	extern Setting<float> SteeringDeadZone;                // hooks_input.cpp
	extern Setting<bool> ControllerHotPlug;                // hooks_input.cpp
	extern Setting<int> ImpulseVibrationMode;              // hooks_input.cpp
	extern Setting<float> ImpulseVibrationLeftMultiplier;  // hooks_input.cpp
	extern Setting<float> ImpulseVibrationRightMultiplier; // hooks_input.cpp
	extern Setting<int> VibrationMode;                     // hooks_forcefeedback.cpp
	extern Setting<int> VibrationStrength;                 // hooks_forcefeedback.cpp
	extern Setting<int> VibrationControllerId;             // hooks_forcefeedback.cpp

	extern Setting<bool> RestoreJPClarissa;                // hooks_misc.cpp
	extern Setting<std::string> DemonwareServerOverride;   // hooks_misc.cpp
	extern Setting<bool> FixFullPedalChecks;               // hooks_bugfixes.cpp
	extern Setting<bool> OverlayEnabled;                   // overlay/hooks_overlay.cpp

	// Track list for the CD switcher. Read by its own parser rather than as a
	// setting, since INIReader doesn't preserve the order of a section's keys.
	inline std::vector<std::pair<std::string, std::string>> CDTracks;
}

namespace Util
{
	std::string HttpGetRequest(const std::string& host, const std::wstring& path, int portNum = 80); // network.cpp

	inline uint32_t GetModuleTimestamp(HMODULE moduleHandle)
	{
		if (!moduleHandle)
			return 0;

		uint8_t* moduleData = (uint8_t*)moduleHandle;
		const IMAGE_DOS_HEADER* dosHeader = (IMAGE_DOS_HEADER*)moduleData;
		const IMAGE_NT_HEADERS* ntHeaders = (IMAGE_NT_HEADERS*)(moduleData + dosHeader->e_lfanew);
		return ntHeaders->FileHeader.TimeDateStamp;
	}

	// Fetches path of module as std::filesystem::path, resizing buffer automatically if path length above MAX_PATH
	inline std::filesystem::path GetModuleFilePath(HMODULE moduleHandle)
	{
		std::vector<wchar_t> buffer(MAX_PATH, L'\0');

		DWORD result = GetModuleFileNameW(moduleHandle, buffer.data(), buffer.size());
		while (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			// Buffer was too small, resize and try again
			buffer.resize(buffer.size() * 2, L'\0');
			result = GetModuleFileNameW(moduleHandle, buffer.data(), buffer.size());
		}

		return std::wstring(buffer.data(), result);
	}

	inline uint32_t BitCount(uint32_t n)
	{
		n = n - ((n >> 1) & 0x55555555);          // put count of each 2 bits into those 2 bits
		n = (n & 0x33333333) + ((n >> 2) & 0x33333333); // put count of each 4 bits into those 4 bits
		n = (n + (n >> 4)) & 0x0F0F0F0F;          // put count of each 8 bits into those 8 bits
		n = n + (n >> 8);                         // put count of each 16 bits into their lowest 8 bits
		n = n + (n >> 16);                        // put count of each 32 bits into their lowest 8 bits
		return n & 0x0000003F;                    // return the count
	}

	// Function to trim spaces from the start of a string
	inline std::string ltrim(const std::string& s)
	{
		auto start = std::find_if_not(s.begin(), s.end(), [](unsigned char ch)
		{
			return std::isspace(ch);
		});
		return std::string(start, s.end());
	}

	// Function to trim spaces from the end of a string
	inline std::string rtrim(const std::string& s)
	{
		auto end = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char ch)
		{
			return std::isspace(ch);
		});
		return std::string(s.begin(), end.base());
	}

	// Function to trim spaces from both ends of a string
	inline std::string trim(const std::string& s)
	{
		return ltrim(rtrim(s));
	}
}

inline void WaitForDebugger()
{
#ifdef _DEBUG
	while (!IsDebuggerPresent())
	{
	}
#endif
}
