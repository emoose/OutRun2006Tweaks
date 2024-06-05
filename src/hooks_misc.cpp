#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"

class CommandLineArguments : public Hook
{
	const static int Win32_CfgRead_Addr = 0xE4D0;

	inline static SafetyHookInline hook_orig = {};
	static void destination(char* cfgFileName)
	{
		hook_orig.call(cfgFileName);

		// Split the command line into individual arguments
		int argc;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argv == nullptr)
			return;

		for (int i = 1; i < argc; ++i)
		{
			try
			{
				if (!wcscmp(argv[i], L"-width") && i + 1 < argc)
					*Game::screen_width = std::stol(argv[++i], 0, 0);
				else if (!wcscmp(argv[i], L"-height") && i + 1 < argc)
					*Game::screen_height = std::stol(argv[++i], 0, 0);
				else if ((!wcscmp(argv[i], L"-antialiasing") || !wcscmp(argv[i], L"-aa")) && i + 1 < argc)
					*Game::D3DAntialiasing = std::stol(argv[++i], 0, 0); // TODO: clamp to valid AA options
				else if (!wcscmp(argv[i], L"-window") || !wcscmp(argv[i], L"-windowed") || !wcscmp(argv[i], L"-w"))
					*Game::D3DWindowed = 1;
				else if (!wcscmp(argv[i], L"-fullscreen") || !wcscmp(argv[i], L"-fs"))
					*Game::D3DWindowed = 0;
				else if (!wcscmp(argv[i], L"-fog"))
					*Game::D3DFogEnabled = 1;
				else if (!wcscmp(argv[i], L"-nofog"))
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
		hook_orig = safetyhook::create_inline(Module::exe_ptr(Win32_CfgRead_Addr), destination);
		return !!hook_orig;
	}

	static CommandLineArguments instance;
};
CommandLineArguments CommandLineArguments::instance;
