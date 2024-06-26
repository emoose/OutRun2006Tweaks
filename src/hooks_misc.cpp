#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

class RestoreJPClarissa : public Hook
{
	// Patch get_load_heroine_chrset to use the non-USA models
	// (oddly there are 3 USA models defined, but code only seems to use 2 of them?)
	const static int get_load_heroine_chrset_Addr1 = 0x88044 + 1;
	const static int get_load_heroine_chrset_Addr2 = 0x8804E + 1;

public:
	std::string_view description() override
	{
		return "RestoreJPClarissa";
	}

	bool validate() override
	{
		return Settings::RestoreJPClarissa;
	}

	bool apply() override
	{
		// chrset 6 -> 5
		Memory::VP::Patch(Module::exe_ptr<int>(get_load_heroine_chrset_Addr1), 5);
		// chrset 8 -> 7
		Memory::VP::Patch(Module::exe_ptr<int>(get_load_heroine_chrset_Addr2), 7);

		// there is another USA chrset 15 at 0x654950 which could be patched to 14
		// but doesn't seem any code uses it which could be patched?
		// forcing it to load shows clarissa with bugged anims, wonder if it's driver chrset...
		return true;
	}

	static RestoreJPClarissa instance;
};
RestoreJPClarissa RestoreJPClarissa::instance;

class CommandLineArguments : public Hook
{
	const static int Win32_CfgRead_Addr = 0xE4D0;

	inline static SafetyHookInline hook_orig = {};
	static void destination(char* cfgFileName)
	{
		hook_orig.call(cfgFileName);

		// game settings that we want to override after game has loaded in config file

		int argc;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return;

		for (int i = 1; i < argc; ++i)
		{
			try
			{
				if (!wcsicmp(argv[i], L"-width") && i + 1 < argc)
					*Game::screen_width = std::stol(argv[++i], 0, 0);
				else if (!wcsicmp(argv[i], L"-height") && i + 1 < argc)
					*Game::screen_height = std::stol(argv[++i], 0, 0);
				else if ((!wcsicmp(argv[i], L"-antialiasing") || !wcsicmp(argv[i], L"-aa")) && i + 1 < argc)
					*Game::D3DAntialiasing = std::stol(argv[++i], 0, 0); // TODO: clamp to valid AA options
				else if (!wcsicmp(argv[i], L"-window") || !wcsicmp(argv[i], L"-windowed") || !wcsicmp(argv[i], L"-w"))
					*Game::D3DWindowed = 1;
				else if (!wcsicmp(argv[i], L"-fullscreen") || !wcsicmp(argv[i], L"-fs"))
					*Game::D3DWindowed = 0;
				else if (!wcsicmp(argv[i], L"-fog"))
					*Game::D3DFogEnabled = 1;
				else if (!wcsicmp(argv[i], L"-nofog"))
					*Game::D3DFogEnabled = 0;
			}
			catch (...) {}
		}

		LocalFree(argv);
	}

public:
	std::string_view description() override
	{
		return "CommandLineArguments";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// cmd-line args we want to apply before game runs
		// TODO: should probably merge this into Settings::read somehow
		int argc;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return true; // no cmd-line so hook is pointless..

		for (int i = 1; i < argc; ++i)
		{
			try
			{
				if (!wcsicmp(argv[i], L"-SkipIntros"))
				{
					spdlog::info("CommandLineArguments: SkipIntroLogos = true");
					Settings::SkipIntroLogos = true;
				}
				else if (!wcsicmp(argv[i], L"-OuttaTime")) // same arg as FXT wrapper
				{
					spdlog::info("CommandLineArguments: DisableCountdownTimer = true");
					Settings::DisableCountdownTimer = true;
				}
			}
			catch (...) {}
		}
		LocalFree(argv);

		hook_orig = safetyhook::create_inline(Module::exe_ptr(Win32_CfgRead_Addr), destination);
		return !!hook_orig;
	}

	static CommandLineArguments instance;
};
CommandLineArguments CommandLineArguments::instance;

class GameDefaultConfigOverride : public Hook
{
public:
	std::string_view description() override
	{
		return "GameDefaultConfigOverride";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		// Override the default settings used when game INI doesn't exist/is empty
		// (these can still be overridden via INI if needed)

		if (Settings::SkipIntroLogos)
			*Game::Sumo_IntroLogosEnable = false;
		if (Settings::DisableCountdownTimer)
			*Game::Sumo_CountdownTimerEnable = false;

		if (Settings::AutoDetectResolution)
		{
			// Use the width/height of the primary screen
			int width = GetSystemMetrics(SM_CXSCREEN);
			int height = GetSystemMetrics(SM_CYSCREEN);
			if (width < 640 || height < 480)
				return false; // bail out if resolution is less than the default

			*Game::screen_width = width;
			*Game::screen_height = height;

			*Game::D3DFogEnabled = true;
			*Game::D3DWindowed = true;
			*Game::D3DAntialiasing = 2;
			*Game::CfgLanguage = 0;

			spdlog::info("GameDefaultConfigOverride: default resolution set to {}x{}, windowed enabled, fog enabled, antialiasing 2", width, height);
		}

		return true;
	}

	static GameDefaultConfigOverride instance;
};
GameDefaultConfigOverride GameDefaultConfigOverride::instance;