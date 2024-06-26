#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <mmiscapi.h>
#include <fstream>

std::string BGMOverridePath;

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
		std::filesystem::path strWaveFileName = *(const char**)(ctx.esp + 0x54);
		if (!BGMOverridePath.empty())
		{
			if (!std::filesystem::exists(BGMOverridePath))
				BGMOverridePath = ".\\Sound\\" + BGMOverridePath;

			if (std::filesystem::exists(BGMOverridePath))
			{
				strcpy_s(CurWavFilePath, BGMOverridePath.c_str());
				*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
				strWaveFileName = BGMOverridePath;
			}
			BGMOverridePath.clear();
		}

		// If a .wav file exists with the same filename, let's try checking that file instead
		std::filesystem::path fileNameAsWav = strWaveFileName;
		fileNameAsWav = fileNameAsWav.replace_extension(".wav");

		bool useWavFile = std::filesystem::exists(fileNameAsWav);
		if (useWavFile)
			strWaveFileName = fileNameAsWav;

		// Game hardcodes this to 3, but 1 allows using CWaveFile to load the audio
		int waveFileType = ctx.eax;

		FILE* file;
		if (fopen_s(&file, strWaveFileName.string().c_str(), "rb") != 0)
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
			{
				strcpy_s(CurWavFilePath, strWaveFileName.string().c_str());
				*(const char**)(ctx.esp + 0x54) = CurWavFilePath;
			}
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

uint32_t ParseButtonCombination(std::string_view combo)
{
	int retval = 0;
	std::string cur_token;

	// Parse combo tokens into buttons bitfield (tokens seperated by any non-alphabetical char, eg. +)
	for (char c : combo)
	{
		if (!isalpha(c) && c != '-')
		{
			if (cur_token.length() && XInputButtonMap.count(cur_token))
				retval |= XInputButtonMap.at(cur_token);

			cur_token.clear();
			continue;
		}
		cur_token += ::tolower(c);
	}

	if (cur_token.length() && XInputButtonMap.count(cur_token))
		retval |= XInputButtonMap.at(cur_token);

	return retval;
}

class CDSwitcher : public Hook
{
	constexpr static int SongTitleDisplaySeconds = 2;
	constexpr static int SongTitleDisplayFrames = SongTitleDisplaySeconds * 60;
	constexpr static float SongTitleFadeBeginSeconds = 0.75;
	constexpr static int SongTitleFadeBeginFrame = int(SongTitleFadeBeginSeconds * 60.f);

	const static int Game_Ctrl_Addr = 0x9C840;

	inline static int SongTitleDisplayTimer = 0;
	inline static bool PrevKeyStatePrev = false;
	inline static bool PrevKeyStateNext = false;

	inline static uint32_t PadButtonCombo_Next = XINPUT_GAMEPAD_BACK;
	inline static uint32_t PadButtonCombo_Prev = XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK;
	inline static uint32_t PadButtonCombo_Next_BitCount = 0;
	inline static uint32_t PadButtonCombo_Prev_BitCount = 0;

	inline static SafetyHookInline Game_Ctrl = {};
	static void destination()
	{
		Game_Ctrl.call();

		bool PadStateNext = Input::PadReleased(PadButtonCombo_Next);
		bool PadStatePrev = Input::PadReleased(PadButtonCombo_Prev);
		if (PadStateNext && PadStatePrev)
		{
			// Whichever combination has the most number of buttons takes precedence
			// (else there could be issues if one binding is a subset of another one)
			if (PadButtonCombo_Prev_BitCount > PadButtonCombo_Next_BitCount)
				PadStateNext = false;
			else
				PadStatePrev = false;
		}

		bool KeyStateNext = (GetAsyncKeyState('E') || PadStateNext);
		bool KeyStatePrev = (GetAsyncKeyState('Q') || PadStatePrev);

		bool BGMChanged = false;

		if (KeyStatePrev != PrevKeyStatePrev)
		{
			PrevKeyStatePrev = KeyStatePrev;
			if (KeyStatePrev)
			{
				*Game::sel_bgm_kind_buf = *Game::sel_bgm_kind_buf - 1;
				if (*Game::sel_bgm_kind_buf < 0)
					*Game::sel_bgm_kind_buf = Settings::CDTracks.size() - 1;
				BGMChanged = true;
			}
		}
		if (KeyStateNext != PrevKeyStateNext)
		{
			PrevKeyStateNext = KeyStateNext;
			if (KeyStateNext)
			{
				*Game::sel_bgm_kind_buf = *Game::sel_bgm_kind_buf + 1;
				if (*Game::sel_bgm_kind_buf >= Settings::CDTracks.size())
					*Game::sel_bgm_kind_buf = 0;
				BGMChanged = true;
			}
		}

		if (BGMChanged)
		{
			BGMOverridePath = Settings::CDTracks[*Game::sel_bgm_kind_buf].first;
			Game::adxPlay(0, 0);

			SongTitleDisplayTimer = SongTitleDisplayFrames;
		}
	}

public:
	static void draw(int numUpdates)
	{
		if (SongTitleDisplayTimer > 0 && *Game::sel_bgm_kind_buf < Settings::CDTracks.size())
		{
			Game::sprSetFontPriority(4);
			Game::sprSetPrintFont(Settings::CDSwitcherTitleFont);
			Game::sprSetFontScale(Settings::CDSwitcherTitleFontSizeX, Settings::CDSwitcherTitleFontSizeY);
			Game::sprLocateP(Settings::CDSwitcherTitlePositionX, Settings::CDSwitcherTitlePositionY);

			uint32_t color = 0xFFFFFF;
			if (SongTitleDisplayTimer > SongTitleFadeBeginFrame)
				color |= 0xFF000000;
			else
			{
				uint8_t alpha = uint8_t((float(SongTitleDisplayTimer) / SongTitleFadeBeginFrame) * 255.f);
				color |= (alpha << 24);
			}
			Game::sprSetFontColor(color);

			const auto& song = Settings::CDTracks[*Game::sel_bgm_kind_buf].second;
			Game::sprPrintf("#%02d. %s", (*Game::sel_bgm_kind_buf) + 1, song.c_str());

			SongTitleDisplayTimer -= numUpdates;
			if (SongTitleDisplayTimer <= 0)
				SongTitleDisplayTimer = 0;
		}
	}

	std::string_view description() override
	{
		return "CDSwitcher";
	}

	bool validate() override
	{
		return Settings::CDSwitcherEnable;
	}

	bool apply() override
	{
		PadButtonCombo_Next = ParseButtonCombination(Settings::CDSwitcherTrackNext);
		PadButtonCombo_Prev = ParseButtonCombination(Settings::CDSwitcherTrackPrevious);
		PadButtonCombo_Next_BitCount = Util::BitCount(PadButtonCombo_Next);
		PadButtonCombo_Prev_BitCount = Util::BitCount(PadButtonCombo_Prev);

		Game_Ctrl = safetyhook::create_inline(Module::exe_ptr(Game_Ctrl_Addr), destination);
		return !!Game_Ctrl;
	}

	static CDSwitcher instance;
};
CDSwitcher CDSwitcher::instance;

void CDSwitcher_Draw(int numUpdates)
{
	if (!Settings::CDSwitcherDisplayTitle)
		return;
	CDSwitcher::draw(numUpdates);
}

void CDSwitcher_ReadIni(const std::filesystem::path& iniPath)
{
	Settings::CDTracks.clear();

	if (!std::filesystem::exists(iniPath))
	{
		// TODO: fill in defaults if no INI found? for now we'll just disable switcher
		Settings::CDSwitcherEnable = false;
	}

	std::ifstream file(iniPath);
	if (file && file.is_open())
	{
		std::string line;
		bool inCDTracksSection = false;

		while (std::getline(file, line))
		{
			if (inCDTracksSection)
			{
				if (line.empty())
					continue;
				line = Util::trim(line);
				if (line.front() == '[' && line.back() == ']') // Reached a new section
					break;
				if (line.front() == '#' || line.front() == ';')
					continue;

				auto delimiterPos = line.find('=');
				if (delimiterPos != std::string::npos)
				{
					std::string key = Util::trim(line.substr(0, delimiterPos));
					std::string value = Util::trim(line.substr(delimiterPos + 1));
					Settings::CDTracks.emplace_back(key, value);
				}
			}
			else if (line == "[CDTracks]")
			{
				inCDTracksSection = true;
			}
		}

		file.close();
	}
}
