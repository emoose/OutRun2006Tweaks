#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <ini.h>

#include "hook_mgr.hpp"
#include "resource.h"
#include "plugin.hpp"

namespace Module
{
	constexpr std::string_view TargetFilename = "OR2006C2C.exe";

	constexpr std::string_view IniFileName = "OutRun2006Tweaks.ini";
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
		spdlog::info(" - FramerateFastLoad: {}", FramerateFastLoad);
		spdlog::info(" - FramerateUnlockExperimental: {}", FramerateUnlockExperimental);

		spdlog::info(" - WindowedBorderless: {}", WindowedBorderless);
		spdlog::info(" - WindowedHideMouseCursor: {}", WindowedHideMouseCursor);
		spdlog::info(" - DisableDPIScaling: {}", DisableDPIScaling);

		spdlog::info(" - AnisotropicFiltering: {}", AnisotropicFiltering);
		spdlog::info(" - TransparencySupersampling: {}", TransparencySupersampling);
		spdlog::info(" - ScreenEdgeCullFix: {}", ScreenEdgeCullFix);
		spdlog::info(" - DisableVehicleLODs: {}", DisableVehicleLODs);
		spdlog::info(" - DisableStageCulling: {}", DisableStageCulling);
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
			spdlog::error("Settings::read - INI read failed, error code {}", result);
			return false;
		}
		inih::INIReader ini(iniFile);
		fclose(iniFile);

		FramerateLimit = ini.Get("Performance", "FramerateLimit", std::move(FramerateLimit));
		FramerateFastLoad = ini.Get("Performance", "FramerateFastLoad", std::move(FramerateFastLoad));
		FramerateUnlockExperimental = ini.Get("Performance", "FramerateUnlockExperimental", std::move(FramerateUnlockExperimental));

		WindowedBorderless = ini.Get("Window", "WindowedBorderless", std::move(WindowedBorderless));
		WindowedHideMouseCursor = ini.Get("Window", "WindowedHideMouseCursor", std::move(WindowedHideMouseCursor));
		DisableDPIScaling = ini.Get("Window", "DisableDPIScaling", std::move(DisableDPIScaling));

		AnisotropicFiltering = ini.Get("Graphics", "AnisotropicFiltering", std::move(AnisotropicFiltering));
		AnisotropicFiltering = std::clamp(AnisotropicFiltering, 0, 16);
		TransparencySupersampling = ini.Get("Graphics", "TransparencySupersampling", std::move(TransparencySupersampling));
		ScreenEdgeCullFix = ini.Get("Graphics", "ScreenEdgeCullFix", std::move(ScreenEdgeCullFix));
		DisableVehicleLODs = ini.Get("Graphics", "DisableVehicleLODs", std::move(DisableVehicleLODs));
		DisableStageCulling = ini.Get("Graphics", "DisableStageCulling", std::move(DisableStageCulling));

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

	Settings::read(Module::IniPath);
	Settings::to_log();

	Game::StartupTime = std::chrono::system_clock::now();

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
