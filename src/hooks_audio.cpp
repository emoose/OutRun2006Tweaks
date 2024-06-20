#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <mmiscapi.h>

class AllowUncompressedBGM : public Hook
{
	// Hook games BGM loader to check the type of the audio file being loaded
	// Game already has CWaveFile code to load in uncompressed WAV audio, but it goes left unused
	// We'll just check if the file looks like a WAV, and if so switch to that code
	// (fortunately games BGM code seems based on DirectX DXUTsound.cpp sample, which included the CWaveFile class)
	// This will also check if there's any .wav file with the same filename too, and try switching to that instead
	const static int CSoundManager__CreateStreaming_HookAddr = 0x11FA3;

	inline static char CurWavFilePath[4096];

	inline static SafetyHookMid hook = {};
	static void destination(safetyhook::Context& ctx)
	{
		const char* strWaveFileName = *(const char**)(ctx.esp + 0x54);

		// If a .wav file exists with the same filename, let's try checking that file instead
		std::filesystem::path fileNameAsWav = strWaveFileName;
		fileNameAsWav = fileNameAsWav.replace_extension(".wav");
		strcpy_s(CurWavFilePath, fileNameAsWav.string().c_str());

		bool useWavFile = std::filesystem::exists(fileNameAsWav);
		if (useWavFile)
			strWaveFileName = CurWavFilePath;

		// Game hardcodes this to 3, but 1 allows using CWaveFile to load the audio
		int waveFileType = ctx.eax;

		FILE* file;
		if (fopen_s(&file, strWaveFileName, "rb") != 0)
			return;

		uint32_t magic;
		size_t numRead = fread(&magic, sizeof(uint32_t), 1, file);
		fclose(file);

		if (numRead != 1)
			return;

		// If file begins with RIFF magic change to filetype 1 so CWaveFile will be used to read it
		if (magic == FOURCC_RIFF || _byteswap_ulong(magic) == FOURCC_RIFF)
		{
			waveFileType = 1;
			// Switch the file game is trying to load to the wav instead
			if (useWavFile)
				*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
		}

		ctx.eax = waveFileType;
	}

public:
	std::string_view description() override
	{
		return "AllowUncompressedBGM";
	}

	bool validate() override
	{
		return Settings::AllowUncompressedBGM;
	}

	bool apply() override
	{
		hook = safetyhook::create_mid(Module::exe_ptr(CSoundManager__CreateStreaming_HookAddr), destination);
		return !!hook;
	}

	static AllowUncompressedBGM instance;
};
AllowUncompressedBGM AllowUncompressedBGM::instance;
