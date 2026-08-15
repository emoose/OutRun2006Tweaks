#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>

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
	constexpr std::string_view OverlayIniFileName = "OutRun2006Tweaks.overlay.ini";
	constexpr std::string_view BindingsIniFileName = "OutRun2006Tweaks.input.ini";
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
		OverlayIniPath = dllParent / OverlayIniFileName;
		BindingsIniPath = dllParent / BindingsIniFileName;

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

	// Anything past this point counts as an override, and is what gets written
	// back out to the user INI.
	Settings::mark_base_values();

	if (std::filesystem::exists(Module::UserIniPath))
		Settings::read(Module::UserIniPath);

	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv)
	{
		bool changed = Settings::read_cmd_line(argc, argv);
		LocalFree(argv);
		if (changed)
		{
			// Prevent command-line overrides from being written back to ini
			Settings::DisableSettingsWrite = true;
		} 
	}

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

	// Hooks declare which settings they read as they apply, so the snapshot and
	// the no-consumer check both have to wait until they've all run.
	Settings::mark_startup_values();
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
