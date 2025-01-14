#pragma once

#include <filesystem>

#include "game.hpp"

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

	inline bool DrawDistanceDebugEnabled = false;

	inline GamepadType CurrentPadType = GamepadType::PC;
	inline GamepadType ForcedPadType = GamepadType::None;
};

namespace Settings
{
	inline int FramerateLimit = 60;
	inline bool FramerateLimitMode = 0;
	inline int FramerateFastLoad = 3;
	inline bool FramerateUnlockExperimental = true;
	inline int VSync = 1;
	inline bool SingleCoreAffinity = true;

	inline bool WindowedBorderless = true;
	inline int WindowPositionX = 0;
	inline int WindowPositionY = 0;
	inline bool WindowedHideMouseCursor = true;
	inline bool DisableDPIScaling = true;
	inline bool AutoDetectResolution = true;

	inline bool AllowHorn = true;
	inline bool AllowWAV = true;
	inline bool AllowFLAC = true;

	inline bool CDSwitcherEnable = false;
	inline bool CDSwitcherDisplayTitle = true;
	inline int CDSwitcherTitleFont = 2;
	inline float CDSwitcherTitleFontSizeX = 0.3f;
	inline float CDSwitcherTitleFontSizeY = 0.5f;
	inline int CDSwitcherTitlePositionX = 375;
	inline int CDSwitcherTitlePositionY = 450;
	inline bool CDSwitcherShuffleTracks = false;
	inline std::string CDSwitcherTrackNext = "Back";
	inline std::string CDSwitcherTrackPrevious = "RS+Back";

	inline std::vector<std::pair<std::string, std::string>> CDTracks;

	inline int UIScalingMode = 1;
	inline int UILetterboxing = 1;
	inline int AnisotropicFiltering = 16;
	inline int ReflectionResolution = 2048;
	inline bool UseHiDefCharacters = true;
	inline bool TransparencySupersampling = true;
	inline bool ScreenEdgeCullFix = true;
	inline bool DisableVehicleLODs = true;
	inline bool DisableStageCulling = true;
	inline bool FixZBufferPrecision = true;
	inline float CarBaseShadowOpacity = 1.0f;
	inline int DrawDistanceIncrease = 0;
	inline int DrawDistanceBehind = 0;

	inline std::string TextureBaseFolder = "textures";
	inline bool SceneTextureReplacement = true;
	inline bool SceneTextureExtract = false;
	inline bool UITextureReplacement = true;
	inline bool UITextureExtract = false;
	inline bool EnableTextureCache = true;
	inline bool UseNewTextureAllocator = true;

	inline bool UseNewInput = false;
	inline float SteeringDeadZone = 0.2f;
	inline bool ControllerHotPlug = false;
	inline bool DefaultManualTransmission = false;
	inline std::string HudToggleKey = "";
	inline int VibrationMode = 0;
	inline int VibrationStrength = 10;
	inline int VibrationControllerId = 0;
	inline int ImpulseVibrationMode = 0;
	inline float ImpulseVibrationLeftMultiplier = 0.25f;
	inline float ImpulseVibrationRightMultiplier = 0.25f;

	inline int EnableHollyCourse2 = 1;
	inline bool SkipIntroLogos = false;
	inline bool DisableCountdownTimer = false;
	inline bool EnableLevelSelect = false;
	inline bool RestoreJPClarissa = false;
	inline bool ShowOutRunMilesOnMenu = true;
	inline bool AllowCharacterSelection = false;
	inline bool RandomHighwayAnimSets = false;
	inline std::string DemonwareServerOverride = "clarissa.port0.org";
	inline bool ProtectLoginData = true;

	inline bool OverlayEnabled = true;

	inline bool FixPegasusClopping = true;
	inline bool FixRightSideBunkiAnimations = true;
	inline bool FixC2CRankings = true;
	inline bool PreventDESTSaveCorruption = true;
	inline bool FixLensFlarePath = true;
	inline bool FixIncorrectShading = true;
	inline bool FixParticleRendering = true;
	inline bool FixFullPedalChecks = true;
	inline bool HideOnlineSigninText = true;
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
