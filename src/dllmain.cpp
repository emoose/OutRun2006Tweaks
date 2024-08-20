#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <ini.h>

#include "hook_mgr.hpp"
#include "resource.h"
#include "plugin.hpp"
#include "game_addrs.hpp"

void InitExceptionHandler(); // hooks_exceptions.cpp

namespace Module
{
	constexpr std::string_view TargetFilename = "OR2006C2C.exe";

	constexpr std::string_view IniFileName = "OutRun2006Tweaks.ini";
	constexpr std::string_view UserIniFileName = "OutRun2006Tweaks.user.ini";
	constexpr std::string_view LogFileName = "OutRun2006Tweaks.log";

	void init()
	{
		if (!DllHandle)
			return;

		ExeHandle = GetModuleHandle(nullptr);

		// Fetch paths of the DLL & EXE
		DllPath = Util::GetModuleFilePath(DllHandle);
		ExePath = Util::GetModuleFilePath(ExeHandle);

		// Setup Log & INI paths, always located next to the DLL instead of the EXE
		auto dllParent = DllPath.parent_path();
		LogPath = dllParent / LogFileName;
		IniPath = dllParent / IniFileName;
		UserIniPath = dllParent / UserIniFileName;

		Game::init();
	}

	void to_log()
	{
		// Print some info about the users setup to log, can come in useful for debugging issues
		spdlog::info("EXE module (base address: {:p}):", fmt::ptr(ExeHandle));
		spdlog::info("  File path: {}", ExePath.string());
		spdlog::info("  Header timestamp: {}", Util::GetModuleTimestamp(ExeHandle));
		spdlog::info("DLL module (base address: {:p}):", fmt::ptr(DllHandle));
		spdlog::info("  File path: {}", DllPath.string());
	}
};

namespace Settings
{
	void to_log()
	{
		spdlog::info("Settings values:");
		spdlog::info(" - FramerateLimit: {}", FramerateLimit);
		spdlog::info(" - FramerateLimitMode: {}", FramerateLimitMode);
		spdlog::info(" - FramerateFastLoad: {}", FramerateFastLoad);
		spdlog::info(" - FramerateUnlockExperimental: {}", FramerateUnlockExperimental);
		spdlog::info(" - VSync: {}", VSync);
		spdlog::info(" - SingleCoreAffinity: {}", SingleCoreAffinity);

		spdlog::info(" - WindowedBorderless: {}", WindowedBorderless);
		spdlog::info(" - WindowPosition: {}x{}", WindowPositionX, WindowPositionY);
		spdlog::info(" - WindowedHideMouseCursor: {}", WindowedHideMouseCursor);
		spdlog::info(" - DisableDPIScaling: {}", DisableDPIScaling);
		spdlog::info(" - AutoDetectResolution: {}", AutoDetectResolution);

		spdlog::info(" - AllowHorn: {}", AllowHorn);
		spdlog::info(" - AllowWAV: {}", AllowWAV);
		spdlog::info(" - AllowFLAC: {}", AllowFLAC);

		spdlog::info(" - CDSwitcherEnable: {}", CDSwitcherEnable);
		spdlog::info(" - CDSwitcherDisplayTitle: {}", CDSwitcherDisplayTitle);
		spdlog::info(" - CDSwitcherTitleFont: {}", CDSwitcherTitleFont);
		spdlog::info(" - CDSwitcherTitleFontSizeX: {}", CDSwitcherTitleFontSizeX);
		spdlog::info(" - CDSwitcherTitleFontSizeY: {}", CDSwitcherTitleFontSizeY);
		spdlog::info(" - CDSwitcherTitlePositionX: {}", CDSwitcherTitlePositionX);
		spdlog::info(" - CDSwitcherTitlePositionY: {}", CDSwitcherTitlePositionY);
		spdlog::info(" - CDSwitcherShuffleTracks: {}", CDSwitcherShuffleTracks);
		spdlog::info(" - CDSwitcherTrackNext: {}", CDSwitcherTrackNext);
		spdlog::info(" - CDSwitcherTrackPrevious: {}", CDSwitcherTrackPrevious);

		spdlog::info(" - UIScalingMode: {}", UIScalingMode);
		spdlog::info(" - AnisotropicFiltering: {}", AnisotropicFiltering);
		spdlog::info(" - ReflectionResolution: {}", ReflectionResolution);
		spdlog::info(" - TransparencySupersampling: {}", TransparencySupersampling);
		spdlog::info(" - ScreenEdgeCullFix: {}", ScreenEdgeCullFix);
		spdlog::info(" - DisableVehicleLODs: {}", DisableVehicleLODs);
		spdlog::info(" - DisableStageCulling: {}", DisableStageCulling);
		spdlog::info(" - FixZBufferPrecision: {}", FixZBufferPrecision);
		spdlog::info(" - CarBaseShadowOpacity: {}", CarBaseShadowOpacity);
		spdlog::info(" - DrawDistanceIncrease: {}", DrawDistanceIncrease);
		spdlog::info(" - DrawDistanceBehind: {}", DrawDistanceBehind);

		spdlog::info(" - TextureBaseFolder: {}", TextureBaseFolder);
		spdlog::info(" - SceneTextureReplacement: {}", SceneTextureReplacement);
		spdlog::info(" - SceneTextureExtract: {}", SceneTextureExtract);
		spdlog::info(" - UITextureReplacement: {}", UITextureReplacement);
		spdlog::info(" - UITextureExtract: {}", UITextureExtract);
		spdlog::info(" - EnableTextureCache: {}", EnableTextureCache);
		spdlog::info(" - UseNewTextureAllocator: {}", UseNewTextureAllocator);

		spdlog::info(" - VibrationMode: {}", VibrationMode);
		spdlog::info(" - VibrationStrength: {}", VibrationStrength);
		spdlog::info(" - VibrationControllerId: {}", VibrationControllerId);
		spdlog::info(" - ImpulseVibrationMode: {}", ImpulseVibrationMode);
		spdlog::info(" - ImpulseVibrationLeftMultiplier: {}", ImpulseVibrationLeftMultiplier);
		spdlog::info(" - ImpulseVibrationRightMultiplier: {}", ImpulseVibrationRightMultiplier);

		spdlog::info(" - SkipIntroLogos: {}", SkipIntroLogos);
		spdlog::info(" - DisableCountdownTimer: {}", DisableCountdownTimer);
		spdlog::info(" - RestoreJPClarissa: {}", RestoreJPClarissa);
		spdlog::info(" - ShowOutRunMilesOnMenu: {}", ShowOutRunMilesOnMenu);
		spdlog::info(" - RandomHighwayAnimSets: {}", RandomHighwayAnimSets);
		spdlog::info(" - DemonwareServerOverride: {}", DemonwareServerOverride);

		spdlog::info(" - FixPegasusClopping: {}", FixPegasusClopping);
		spdlog::info(" - FixC2CRankings: {}", FixC2CRankings);
		spdlog::info(" - PreventDESTSaveCorruption: {}", PreventDESTSaveCorruption);
		spdlog::info(" - FixLensFlarePath: {}", FixLensFlarePath);
		spdlog::info(" - FixFullPedalChecks: {}", FixFullPedalChecks);
		spdlog::info(" - HideOnlineSigninText: {}", HideOnlineSigninText);
		spdlog::info(" - ControllerHotPlug: {}", ControllerHotPlug);
	}

	bool read(std::filesystem::path& iniPath)
	{
		spdlog::info("Settings::read - reading INI from {}", iniPath.string());

		const std::wstring iniPathStr = iniPath.wstring();

		// Read INI via FILE* since INIReader doesn't support wstring
		FILE* iniFile;
		errno_t result = _wfopen_s(&iniFile, iniPathStr.c_str(), L"r");
		if (result != 0 || !iniFile)
		{
			spdlog::error("Settings::read - INI read failed! Error code {}", result);
			return false;
		}

		inih::INIReader ini;
		try
		{
			ini = inih::INIReader(iniFile);
		}
		catch (...)
		{
			spdlog::error("Settings::read - INI read failed! The file may be invalid or have duplicate settings inside");
			fclose(iniFile);
			return false;
		}
		fclose(iniFile);

		FramerateLimit = ini.Get("Performance", "FramerateLimit", std::move(FramerateLimit));
		FramerateLimitMode = ini.Get("Performance", "FramerateLimitMode", std::move(FramerateLimitMode));
		FramerateFastLoad = ini.Get("Performance", "FramerateFastLoad", std::move(FramerateFastLoad));
		FramerateUnlockExperimental = ini.Get("Performance", "FramerateUnlockExperimental", std::move(FramerateUnlockExperimental));
		VSync = ini.Get("Performance", "VSync", std::move(VSync));
		SingleCoreAffinity = ini.Get("Performance", "SingleCoreAffinity", std::move(SingleCoreAffinity));

		WindowedBorderless = ini.Get("Window", "WindowedBorderless", std::move(WindowedBorderless));
		WindowPositionX = ini.Get("Window", "WindowPositionX", std::move(WindowPositionX));
		WindowPositionY = ini.Get("Window", "WindowPositionY", std::move(WindowPositionY));
		WindowedHideMouseCursor = ini.Get("Window", "WindowedHideMouseCursor", std::move(WindowedHideMouseCursor));
		DisableDPIScaling = ini.Get("Window", "DisableDPIScaling", std::move(DisableDPIScaling));
		AutoDetectResolution = ini.Get("Window", "AutoDetectResolution", std::move(AutoDetectResolution));

		AllowHorn = ini.Get("Audio", "AllowHorn", std::move(AllowHorn));
		AllowWAV = ini.Get("Audio", "AllowWAV", std::move(AllowWAV));
		AllowFLAC = ini.Get("Audio", "AllowFLAC", std::move(AllowFLAC));

		CDSwitcherEnable = ini.Get("CDSwitcher", "SwitcherEnable", std::move(CDSwitcherEnable));
		CDSwitcherDisplayTitle = ini.Get("CDSwitcher", "SwitcherDisplayTitle", std::move(CDSwitcherDisplayTitle));
		CDSwitcherTitleFont = ini.Get("CDSwitcher", "SwitcherTitleFont", std::move(CDSwitcherTitleFont));
		CDSwitcherTitleFont = std::clamp(CDSwitcherTitleFont, 0, 9);
		CDSwitcherTitleFontSizeX = ini.Get("CDSwitcher", "SwitcherTitleFontSizeX", std::move(CDSwitcherTitleFontSizeX));
		CDSwitcherTitleFontSizeY = ini.Get("CDSwitcher", "SwitcherTitleFontSizeY", std::move(CDSwitcherTitleFontSizeY));
		CDSwitcherTitlePositionX = ini.Get("CDSwitcher", "SwitcherTitlePositionX", std::move(CDSwitcherTitlePositionX));
		CDSwitcherTitlePositionY = ini.Get("CDSwitcher", "SwitcherTitlePositionY", std::move(CDSwitcherTitlePositionY));
		CDSwitcherShuffleTracks = ini.Get("CDSwitcher", "SwitcherShuffleTracks", std::move(CDSwitcherShuffleTracks));
		CDSwitcherTrackNext = ini.Get("CDSwitcher", "TrackNext", std::move(CDSwitcherTrackNext));
		CDSwitcherTrackPrevious = ini.Get("CDSwitcher", "TrackPrevious", std::move(CDSwitcherTrackPrevious));

		UIScalingMode = ini.Get("Graphics", "UIScalingMode", std::move(UIScalingMode));
		UIScalingMode = std::clamp(UIScalingMode, 0, 2);
		UILetterboxing = ini.Get("Graphics", "UILetterboxing", std::move(UILetterboxing));
		UILetterboxing = std::clamp(UILetterboxing, 0, 2);

		AnisotropicFiltering = ini.Get("Graphics", "AnisotropicFiltering", std::move(AnisotropicFiltering));
		AnisotropicFiltering = std::clamp(AnisotropicFiltering, 0, 16);
		ReflectionResolution = ini.Get("Graphics", "ReflectionResolution", std::move(ReflectionResolution));
		ReflectionResolution = std::clamp(ReflectionResolution, 0, 8192);
		TransparencySupersampling = ini.Get("Graphics", "TransparencySupersampling", std::move(TransparencySupersampling));
		ScreenEdgeCullFix = ini.Get("Graphics", "ScreenEdgeCullFix", std::move(ScreenEdgeCullFix));
		DisableVehicleLODs = ini.Get("Graphics", "DisableVehicleLODs", std::move(DisableVehicleLODs));
		DisableStageCulling = ini.Get("Graphics", "DisableStageCulling", std::move(DisableStageCulling));
		FixZBufferPrecision = ini.Get("Graphics", "FixZBufferPrecision", std::move(FixZBufferPrecision));
		CarBaseShadowOpacity = ini.Get("Graphics", "CarBaseShadowOpacity", std::move(CarBaseShadowOpacity));
		DrawDistanceIncrease = ini.Get("Graphics", "DrawDistanceIncrease", std::move(DrawDistanceIncrease));
		DrawDistanceBehind = ini.Get("Graphics", "DrawDistanceBehind", std::move(DrawDistanceBehind));

		TextureBaseFolder = ini.Get("Graphics", "TextureBaseFolder", std::move(TextureBaseFolder));
		SceneTextureReplacement = ini.Get("Graphics", "SceneTextureReplacement", std::move(SceneTextureReplacement));
		SceneTextureExtract = ini.Get("Graphics", "SceneTextureExtract", std::move(SceneTextureExtract));
		UITextureReplacement = ini.Get("Graphics", "UITextureReplacement", std::move(UITextureReplacement));
		UITextureExtract = ini.Get("Graphics", "UITextureExtract", std::move(UITextureExtract));
		EnableTextureCache = ini.Get("Graphics", "EnableTextureCache", std::move(EnableTextureCache));
		UseNewTextureAllocator = ini.Get("Graphics", "UseNewTextureAllocator", std::move(UseNewTextureAllocator));

		VibrationMode = ini.Get("Vibration", "VibrationMode", std::move(VibrationMode));
		VibrationMode = std::clamp(VibrationMode, 0, 3);
		VibrationStrength = ini.Get("Vibration", "VibrationStrength", std::move(VibrationStrength));
		VibrationStrength = std::clamp(VibrationStrength, 0, 10);
		VibrationControllerId = ini.Get("Vibration", "VibrationControllerId", std::move(VibrationControllerId));
		VibrationControllerId = std::clamp(VibrationControllerId, 0, 4);

		ImpulseVibrationMode = ini.Get("Vibration", "ImpulseVibrationMode", std::move(ImpulseVibrationMode));
		ImpulseVibrationMode = std::clamp(ImpulseVibrationMode, 0, 3);
		ImpulseVibrationLeftMultiplier = ini.Get("Vibration", "ImpulseVibrationLeftMultiplier", std::move(ImpulseVibrationLeftMultiplier));
		ImpulseVibrationLeftMultiplier = std::clamp(ImpulseVibrationLeftMultiplier, 0.0f, 1.0f);
		ImpulseVibrationRightMultiplier = ini.Get("Vibration", "ImpulseVibrationRightMultiplier", std::move(ImpulseVibrationRightMultiplier));
		ImpulseVibrationRightMultiplier = std::clamp(ImpulseVibrationRightMultiplier, 0.0f, 1.0f);

		SkipIntroLogos = ini.Get("Misc", "SkipIntroLogos", std::move(SkipIntroLogos));
		DisableCountdownTimer = ini.Get("Misc", "DisableCountdownTimer", std::move(DisableCountdownTimer));
		RestoreJPClarissa = ini.Get("Misc", "RestoreJPClarissa", std::move(RestoreJPClarissa));
		ShowOutRunMilesOnMenu = ini.Get("Misc", "ShowOutRunMilesOnMenu", std::move(ShowOutRunMilesOnMenu));
		RandomHighwayAnimSets = ini.Get("Misc", "RandomHighwayAnimSets", std::move(RandomHighwayAnimSets));
		DemonwareServerOverride = ini.Get("Misc", "DemonwareServerOverride", std::move(DemonwareServerOverride));

		FixPegasusClopping = ini.Get("Bugfixes", "FixPegasusClopping", std::move(FixPegasusClopping));
		FixC2CRankings = ini.Get("Bugfixes", "FixC2CRankings", std::move(FixC2CRankings));
		PreventDESTSaveCorruption = ini.Get("Bugfixes", "PreventDESTSaveCorruption", std::move(PreventDESTSaveCorruption));
		FixLensFlarePath = ini.Get("Bugfixes", "FixLensFlarePath", std::move(FixLensFlarePath));
		FixFullPedalChecks = ini.Get("Bugfixes", "FixFullPedalChecks", std::move(FixFullPedalChecks));
		HideOnlineSigninText = ini.Get("Bugfixes", "HideOnlineSigninText", std::move(HideOnlineSigninText));
		ControllerHotPlug = ini.Get("Bugfixes", "ControllerHotPlug", std::move(ControllerHotPlug));

		// INIReader doesn't preserve the order of the keys/values in a section
		// Will need to try opening INI ourselves to grab cd tracks...
		CDSwitcher_ReadIni(iniPath);

		return true;
	}
};

void Plugin_Init()
{
	// setup our log & INI paths
	Module::init();

	// spdlog setup
	{
		std::vector<spdlog::sink_ptr> sinks;
		sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>(true)); // Print logs to debug output
		try
		{
			sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(Module::LogPath.string(), true));
		}
		catch (const std::exception&)
		{
			// spdlog failed to open log file for writing (happens in some WinStore apps)
			// let's just try to continue instead of crashing
		}

		auto combined_logger = std::make_shared<spdlog::logger>("", begin(sinks), end(sinks));
#ifdef _DEBUG
		combined_logger->set_level(spdlog::level::debug);
#else
		combined_logger->set_level(spdlog::level::debug);
#endif
		spdlog::set_default_logger(combined_logger);
		spdlog::flush_on(spdlog::level::debug);

	}

	spdlog::info("OutRun2006Tweaks v{} - github.com/emoose/OutRun2006Tweaks", MODULE_VERSION_STR);
	Module::to_log();

	if (!Settings::read(Module::IniPath))
		spdlog::error("Settings::read - Launching game with default OR2006Tweaks INI settings!");

	if (std::filesystem::exists(Module::UserIniPath))
		Settings::read(Module::UserIniPath);

	Settings::to_log();

	Game::StartupTime = std::chrono::system_clock::now();

	InitExceptionHandler();

	HookManager::ApplyHooks();
}

#include "Proxy.hpp"

BOOL APIENTRY DllMain(HMODULE hModule, int ul_reason_for_call, LPVOID lpReserved)
{
	DisableThreadLibraryCalls(hModule);

	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		Module::DllHandle = hModule;
		proxy::on_attach(Module::DllHandle);

		static std::once_flag flag;
		std::call_once(flag, Plugin_Init);
	}
	else if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		proxy::on_detach();
	}

	return TRUE;
}
