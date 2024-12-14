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
	constexpr std::string_view LodIniFileName = "OutRun2006Tweaks.lods.ini";
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
		LodIniPath = dllParent / LodIniFileName;

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
		spdlog::info(" - UILetterboxing: {}", UILetterboxing);
		spdlog::info(" - AnisotropicFiltering: {}", AnisotropicFiltering);
		spdlog::info(" - ReflectionResolution: {}", ReflectionResolution);
		spdlog::info(" - UseHiDefCharacters: {}", UseHiDefCharacters);
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

		spdlog::info(" - SteeringDeadZone: {}", SteeringDeadZone);
		spdlog::info(" - ControllerHotPlug: {}", ControllerHotPlug);
		spdlog::info(" - DefaultManualTransmission: {}", DefaultManualTransmission);
		spdlog::info(" - VibrationMode: {}", VibrationMode);
		spdlog::info(" - VibrationStrength: {}", VibrationStrength);
		spdlog::info(" - VibrationControllerId: {}", VibrationControllerId);
		spdlog::info(" - ImpulseVibrationMode: {}", ImpulseVibrationMode);
		spdlog::info(" - ImpulseVibrationLeftMultiplier: {}", ImpulseVibrationLeftMultiplier);
		spdlog::info(" - ImpulseVibrationRightMultiplier: {}", ImpulseVibrationRightMultiplier);

		spdlog::info(" - EnableHollyCourse2: {}", EnableHollyCourse2);
		spdlog::info(" - SkipIntroLogos: {}", SkipIntroLogos);
		spdlog::info(" - DisableCountdownTimer: {}", DisableCountdownTimer);
		spdlog::info(" - EnableLevelSelect: {}", EnableLevelSelect);
		spdlog::info(" - HudToggleKey: {}", HudToggleKey);
		spdlog::info(" - RestoreJPClarissa: {}", RestoreJPClarissa);
		spdlog::info(" - ShowOutRunMilesOnMenu: {}", ShowOutRunMilesOnMenu);
		spdlog::info(" - AllowCharacterSelection: {}", AllowCharacterSelection);
		spdlog::info(" - RandomHighwayAnimSets: {}", RandomHighwayAnimSets);
		spdlog::info(" - DemonwareServerOverride: {}", DemonwareServerOverride);

		spdlog::info(" - OverlayEnabled: {}", OverlayEnabled);
		spdlog::info(" - OverlayFontScale: {}", OverlayFontScale);
		spdlog::info(" - OverlayNotifyDisplayTime: {}", OverlayNotifyDisplayTime);
		spdlog::info(" - OverlayNotifyOnlineEnable: {}", OverlayNotifyOnlineEnable);
		spdlog::info(" - OverlayNotifyOnlineUpdateTime: {}", OverlayNotifyOnlineUpdateTime);
		spdlog::info(" - OverlayNotifyHideMode: {}", OverlayNotifyHideMode);

		spdlog::info(" - FixPegasusClopping: {}", FixPegasusClopping);
		spdlog::info(" - FixRightSideBunkiAnimations: {}", FixRightSideBunkiAnimations);
		spdlog::info(" - FixC2CRankings: {}", FixC2CRankings);
		spdlog::info(" - PreventDESTSaveCorruption: {}", PreventDESTSaveCorruption);
		spdlog::info(" - FixLensFlarePath: {}", FixLensFlarePath);
		spdlog::info(" - FixIncorrectShading: {}", FixIncorrectShading);
		spdlog::info(" - FixParticleRendering: {}", FixParticleRendering);
		spdlog::info(" - FixFullPedalChecks: {}", FixFullPedalChecks);
		spdlog::info(" - HideOnlineSigninText: {}", HideOnlineSigninText);
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

		FramerateLimit = ini.Get("Performance", "FramerateLimit", FramerateLimit);
		FramerateLimitMode = ini.Get("Performance", "FramerateLimitMode", FramerateLimitMode);
		FramerateFastLoad = ini.Get("Performance", "FramerateFastLoad", FramerateFastLoad);
		FramerateUnlockExperimental = ini.Get("Performance", "FramerateUnlockExperimental", FramerateUnlockExperimental);
		VSync = ini.Get("Performance", "VSync", VSync);
		SingleCoreAffinity = ini.Get("Performance", "SingleCoreAffinity", SingleCoreAffinity);

		WindowedBorderless = ini.Get("Window", "WindowedBorderless", WindowedBorderless);
		WindowPositionX = ini.Get("Window", "WindowPositionX", WindowPositionX);
		WindowPositionY = ini.Get("Window", "WindowPositionY", WindowPositionY);
		WindowedHideMouseCursor = ini.Get("Window", "WindowedHideMouseCursor", WindowedHideMouseCursor);
		DisableDPIScaling = ini.Get("Window", "DisableDPIScaling", DisableDPIScaling);
		AutoDetectResolution = ini.Get("Window", "AutoDetectResolution", AutoDetectResolution);

		AllowHorn = ini.Get("Audio", "AllowHorn", AllowHorn);
		AllowWAV = ini.Get("Audio", "AllowWAV", AllowWAV);
		AllowFLAC = ini.Get("Audio", "AllowFLAC", AllowFLAC);

		CDSwitcherEnable = ini.Get("CDSwitcher", "SwitcherEnable", CDSwitcherEnable);
		CDSwitcherDisplayTitle = ini.Get("CDSwitcher", "SwitcherDisplayTitle", CDSwitcherDisplayTitle);
		CDSwitcherTitleFont = ini.Get("CDSwitcher", "SwitcherTitleFont", CDSwitcherTitleFont);
		CDSwitcherTitleFont = std::clamp(CDSwitcherTitleFont, 0, 9);
		CDSwitcherTitleFontSizeX = ini.Get("CDSwitcher", "SwitcherTitleFontSizeX", CDSwitcherTitleFontSizeX);
		CDSwitcherTitleFontSizeY = ini.Get("CDSwitcher", "SwitcherTitleFontSizeY", CDSwitcherTitleFontSizeY);
		CDSwitcherTitlePositionX = ini.Get("CDSwitcher", "SwitcherTitlePositionX", CDSwitcherTitlePositionX);
		CDSwitcherTitlePositionY = ini.Get("CDSwitcher", "SwitcherTitlePositionY", CDSwitcherTitlePositionY);
		CDSwitcherShuffleTracks = ini.Get("CDSwitcher", "SwitcherShuffleTracks", CDSwitcherShuffleTracks);
		CDSwitcherTrackNext = ini.Get("CDSwitcher", "TrackNext", CDSwitcherTrackNext);
		CDSwitcherTrackPrevious = ini.Get("CDSwitcher", "TrackPrevious", CDSwitcherTrackPrevious);

		UIScalingMode = ini.Get("Graphics", "UIScalingMode", UIScalingMode);
		UIScalingMode = std::clamp(UIScalingMode, 0, 2);
		UILetterboxing = ini.Get("Graphics", "UILetterboxing", UILetterboxing);
		UILetterboxing = std::clamp(UILetterboxing, 0, 2);

		AnisotropicFiltering = ini.Get("Graphics", "AnisotropicFiltering", AnisotropicFiltering);
		AnisotropicFiltering = std::clamp(AnisotropicFiltering, 0, 16);
		ReflectionResolution = ini.Get("Graphics", "ReflectionResolution", ReflectionResolution);
		ReflectionResolution = std::clamp(ReflectionResolution, 0, 8192);
		UseHiDefCharacters = ini.Get("Graphics", "UseHiDefCharacters", UseHiDefCharacters);
		TransparencySupersampling = ini.Get("Graphics", "TransparencySupersampling", TransparencySupersampling);
		ScreenEdgeCullFix = ini.Get("Graphics", "ScreenEdgeCullFix", ScreenEdgeCullFix);
		DisableVehicleLODs = ini.Get("Graphics", "DisableVehicleLODs", DisableVehicleLODs);
		DisableStageCulling = ini.Get("Graphics", "DisableStageCulling", DisableStageCulling);
		FixZBufferPrecision = ini.Get("Graphics", "FixZBufferPrecision", FixZBufferPrecision);
		CarBaseShadowOpacity = ini.Get("Graphics", "CarBaseShadowOpacity", CarBaseShadowOpacity);
		DrawDistanceIncrease = ini.Get("Graphics", "DrawDistanceIncrease", DrawDistanceIncrease);
		DrawDistanceBehind = ini.Get("Graphics", "DrawDistanceBehind", DrawDistanceBehind);

		TextureBaseFolder = ini.Get("Graphics", "TextureBaseFolder", TextureBaseFolder);
		SceneTextureReplacement = ini.Get("Graphics", "SceneTextureReplacement", SceneTextureReplacement);
		SceneTextureExtract = ini.Get("Graphics", "SceneTextureExtract", SceneTextureExtract);
		UITextureReplacement = ini.Get("Graphics", "UITextureReplacement", UITextureReplacement);
		UITextureExtract = ini.Get("Graphics", "UITextureExtract", UITextureExtract);
		EnableTextureCache = ini.Get("Graphics", "EnableTextureCache", EnableTextureCache);
		UseNewTextureAllocator = ini.Get("Graphics", "UseNewTextureAllocator", UseNewTextureAllocator);

		SteeringDeadZone = ini.Get("Controls", "SteeringDeadZone", SteeringDeadZone);
		SteeringDeadZone = std::clamp(SteeringDeadZone, 0.f, 1.f);
		ControllerHotPlug = ini.Get("Controls", "ControllerHotPlug", ControllerHotPlug);
		DefaultManualTransmission = ini.Get("Controls", "DefaultManualTransmission", DefaultManualTransmission);
		HudToggleKey = ini.Get("Controls", "HudToggleKey", HudToggleKey);
		VibrationMode = ini.Get("Controls", "VibrationMode", VibrationMode);
		VibrationMode = std::clamp(VibrationMode, 0, 3);
		VibrationStrength = ini.Get("Controls", "VibrationStrength", VibrationStrength);
		VibrationStrength = std::clamp(VibrationStrength, 0, 10);
		VibrationControllerId = ini.Get("Controls", "VibrationControllerId", VibrationControllerId);
		VibrationControllerId = std::clamp(VibrationControllerId, 0, 4);

		ImpulseVibrationMode = ini.Get("Controls", "ImpulseVibrationMode", ImpulseVibrationMode);
		ImpulseVibrationMode = std::clamp(ImpulseVibrationMode, 0, 3);
		ImpulseVibrationLeftMultiplier = ini.Get("Controls", "ImpulseVibrationLeftMultiplier", ImpulseVibrationLeftMultiplier);
		ImpulseVibrationLeftMultiplier = std::clamp(ImpulseVibrationLeftMultiplier, 0.0f, 1.0f);
		ImpulseVibrationRightMultiplier = ini.Get("Controls", "ImpulseVibrationRightMultiplier", ImpulseVibrationRightMultiplier);
		ImpulseVibrationRightMultiplier = std::clamp(ImpulseVibrationRightMultiplier, 0.0f, 1.0f);

		EnableHollyCourse2 = ini.Get("Misc", "EnableHollyCourse2", EnableHollyCourse2);
		SkipIntroLogos = ini.Get("Misc", "SkipIntroLogos", SkipIntroLogos);
		DisableCountdownTimer = ini.Get("Misc", "DisableCountdownTimer", DisableCountdownTimer);
		EnableLevelSelect = ini.Get("Misc", "EnableLevelSelect", EnableLevelSelect);
		RestoreJPClarissa = ini.Get("Misc", "RestoreJPClarissa", RestoreJPClarissa);
		ShowOutRunMilesOnMenu = ini.Get("Misc", "ShowOutRunMilesOnMenu", ShowOutRunMilesOnMenu);
		AllowCharacterSelection = ini.Get("Misc", "AllowCharacterSelection", AllowCharacterSelection);
		RandomHighwayAnimSets = ini.Get("Misc", "RandomHighwayAnimSets", RandomHighwayAnimSets);
		DemonwareServerOverride = ini.Get("Misc", "DemonwareServerOverride", DemonwareServerOverride);

		OverlayEnabled = ini.Get("Overlay", "Enabled", OverlayEnabled);
		OverlayFontScale = ini.Get("Overlay", "FontScale", OverlayFontScale);
		OverlayNotifyDisplayTime = ini.Get("Overlay", "NotifyDisplayTime", OverlayNotifyDisplayTime);
		OverlayNotifyOnlineEnable = ini.Get("Overlay", "NotifyOnlineEnable", OverlayNotifyOnlineEnable);
		OverlayNotifyOnlineUpdateTime = ini.Get("Overlay", "NotifyOnlineUpdateTime", OverlayNotifyOnlineUpdateTime);
		OverlayNotifyHideMode = ini.Get("Overlay", "NotifyHideMode", OverlayNotifyHideMode);

		FixPegasusClopping = ini.Get("Bugfixes", "FixPegasusClopping", FixPegasusClopping);
		FixRightSideBunkiAnimations = ini.Get("Bugfixes", "FixRightSideBunkiAnimations", FixRightSideBunkiAnimations);
		FixC2CRankings = ini.Get("Bugfixes", "FixC2CRankings", FixC2CRankings);
		PreventDESTSaveCorruption = ini.Get("Bugfixes", "PreventDESTSaveCorruption", PreventDESTSaveCorruption);
		FixLensFlarePath = ini.Get("Bugfixes", "FixLensFlarePath", FixLensFlarePath);
		FixIncorrectShading = ini.Get("Bugfixes", "FixIncorrectShading", FixIncorrectShading);
		FixParticleRendering = ini.Get("Bugfixes", "FixParticleRendering", FixParticleRendering);
		FixFullPedalChecks = ini.Get("Bugfixes", "FixFullPedalChecks", FixFullPedalChecks);
		HideOnlineSigninText = ini.Get("Bugfixes", "HideOnlineSigninText", HideOnlineSigninText);

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

	spdlog::info("OutRun2006Tweaks v" MODULE_VERSION_STR " - github.com/emoose/OutRun2006Tweaks");
	Module::to_log();

	if (!Settings::read(Module::IniPath))
		spdlog::error("Settings::read - Launching game with default OR2006Tweaks INI settings!");

	if (std::filesystem::exists(Module::UserIniPath))
		Settings::read(Module::UserIniPath);

	Settings::to_log();

	Game::StartupTime = std::chrono::system_clock::now();

	// Create save folder if it doesn't exist, otherwise game will have issues writing savegame...
	auto saveFolder = Module::ExePath.parent_path() / "SaveGame";
	if (!std::filesystem::exists(saveFolder))
	{
		spdlog::warn("Plugin_Init: SaveGame folder doesn't exist, trying to create it...");
		try
		{
			std::filesystem::create_directory(saveFolder);
			spdlog::info("Plugin_Init: SaveGame folder created");
		}
		catch (const std::exception&)
		{
			spdlog::error("Plugin_Init: Failed to create SaveGame folder (may require permissions?) - game might have issues writing savegame!");
		}
	}

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
