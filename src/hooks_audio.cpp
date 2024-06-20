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

std::vector<std::string> SongTitles =
{
	"#01. Splash Wave",
	"#02. Magical Sound Shower",
	"#03. Passing Breeze",
	"#04. Risky Ride",
	"#05. Shiny World",
	"#06. Night Flight",
	"#07. Life was a Bore",
	"#08. Radiation",
	"#09. Night Bird",
	"#10. Splash Wave (1986)",
	"#11. Magical Sound Shower (1986)",
	"#12. Passing Breeze (1986)",
	"#13. Shake the Street (1989)",
	"#14. Rush a Difficulty (1989)",
	"#15. Who are You (1989)",
	"#16. Keep Your Heart (1989)",
	"#17. Splash Wave (EuroMix)",
	"#18. Magical Sound Shower (EuroMix)",
	"#19. Passing Breeze (EuroMix)",
	"#20. Risky Ride (Guitar Mix)",
	"#21. Shake the Street (ARRANGED)",
	"#22. Who are You (ARRANGED)",
	"#23. Rush a difficulty (ARRANGED)",
	"#24. Keep your Heart  (ARRANGED)",
	"#25. Shiny World (Prototype)",
	"#26. Night Flight (Prototype)",
	"#27. Life was a Bore (Instrumental)",
	"#28. Night Flight (Instrumental)",
	"#29. Last Wave",
	"#30. SEGA Intro",
	"#31. Beach Waves",
	"#32. C2C TitlePattern",
	"#33. C2C TITLE 01",
	"SEL 01 Splash Wave",
	"SEL 02 Magical Sound Shower",
	"SEL 03 Passing Breeze",
	"SEL 04 Risky Ride",
	"SEL 05 Shiny World",
	"SEL 06 Night Flight",
	"SEL 07 Life was a Bore",
	"SEL 08 Radiation",
	"SEL 09 Night Bird",
	"SEL 10 Splash Wave 1986",
	"SEL 11 Magical Sound Shower 1986",
	"SEL 12 Passing Breeze 1986",
	"SEL 13 Shake the Street 1989",
	"SEL 14 Rush a Difficulty",
	"SEL 15 Who are You 1989",
	"SEL 16 Keep Your Heart 1989",
	"SEL 17 Splash Wave EuroMix",
	"SEL 18 Magical Sound Shower EuroMix",
	"SEL 19 Passing Breeze EuroMix",
	"SEL 20 Risky Ride Guitar",
	"SEL 21 Shake the Street ARRANGED",
	"SEL 22 Who are you ARRANGED",
	"SEL 23 Rush a Difficulty ARRANGED",
	"SEL 24 Keep your Heart ARRANGED",
	"SEL 25 Shiny World Prototype",
	"SEL 26 Night Flight Prototype",
	"SEL 27 Life was a Bore Instrumental",
	"SEL 28 Night Flight Instrumental",
	"OR2ED1",
	"OR2ED2",
	"OR2ED3A",
	"OR2ED3B",
	"OR2ED4",
	"OR2ED5",
};

static const std::unordered_map<std::string, uint32_t> kXInputButtons = {
	{"up", XINPUT_GAMEPAD_DPAD_UP},
	{"down", XINPUT_GAMEPAD_DPAD_DOWN},
	{"left", XINPUT_GAMEPAD_DPAD_LEFT},
	{"right", XINPUT_GAMEPAD_DPAD_RIGHT},

	{"start", XINPUT_GAMEPAD_START},
	{"back", XINPUT_GAMEPAD_BACK},

	{"ls", XINPUT_GAMEPAD_LEFT_THUMB},
	{"rs", XINPUT_GAMEPAD_RIGHT_THUMB},

	{"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
	{"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},

	{"a", XINPUT_GAMEPAD_A},
	{"b", XINPUT_GAMEPAD_B},
	{"x", XINPUT_GAMEPAD_X},
	{"y", XINPUT_GAMEPAD_Y},

	{"lt", XINPUT_DIGITAL_LEFT_TRIGGER},
	{"rt", XINPUT_DIGITAL_RIGHT_TRIGGER},

	{"ls-up", XINPUT_DIGITAL_LS_UP},
	{"ls-down", XINPUT_DIGITAL_LS_DOWN},
	{"ls-left", XINPUT_DIGITAL_LS_LEFT},
	{"ls-right", XINPUT_DIGITAL_LS_RIGHT},

	{"rs-up", XINPUT_DIGITAL_RS_UP},
	{"rs-down", XINPUT_DIGITAL_RS_DOWN},
	{"rs-left", XINPUT_DIGITAL_RS_LEFT},
	{"rs-right", XINPUT_DIGITAL_RS_RIGHT},
};

uint32_t ParseButtonCombination(std::string_view combo)
{
	int retval = 0;
	std::string cur_token;

	// Parse combo tokens into buttons bitfield (tokens seperated by any non-alphabetical char, eg. +)
	for (char c : combo)
	{
		if (!isalpha(c) && c != '-')
		{
			if (cur_token.length() && kXInputButtons.count(cur_token))
				retval |= kXInputButtons.at(cur_token);

			cur_token.clear();
			continue;
		}
		cur_token += ::tolower(c);
	}

	if (cur_token.length() && kXInputButtons.count(cur_token))
		retval |= kXInputButtons.at(cur_token);

	return retval;
}

class CDSwitcher : public Hook
{
	constexpr static int SongTitleDisplaySeconds = 2;
	constexpr static int SongTitleDisplayFrames = SongTitleDisplaySeconds * 60;
	constexpr static float SongTitleFadeBeginTime = 0.75; // seconds
	constexpr static int SongTitleFadeBeginFrame = int(SongTitleFadeBeginTime * 60.f);

	const static int Game_Ctrl_Addr = 0x9C840;

	inline static int SongTitleDisplayTimer = 0;
	inline static bool PrevKeyStatePrev = false;
	inline static bool PrevKeyStateNext = false;

	inline static uint32_t PadButtonCombo_Next = XINPUT_GAMEPAD_BACK;
	inline static uint32_t PadButtonCombo_Prev = XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK;
	inline static uint32_t PadButtonCombo_Next_BitCount = 0;
	inline static uint32_t PadButtonCombo_Prev_BitCount = 0;

	uint32_t bitcount(uint32_t n)
	{
		n = n - ((n >> 1) & 0x55555555);          // put count of each 2 bits into those 2 bits
		n = (n & 0x33333333) + ((n >> 2) & 0x33333333); // put count of each 4 bits into those 4 bits
		n = (n + (n >> 4)) & 0x0F0F0F0F;          // put count of each 8 bits into those 8 bits
		n = n + (n >> 8);                         // put count of each 16 bits into their lowest 8 bits
		n = n + (n >> 16);                        // put count of each 32 bits into their lowest 8 bits
		return n & 0x0000003F;                    // return the count
	}

	inline static SafetyHookInline Game_Ctrl = {};
	static void destination()
	{
		Game_Ctrl.call();

		bool PadStateNext = Input::PadReleased(PadButtonCombo_Next);
		bool PadStatePrev = Input::PadReleased(PadButtonCombo_Prev);
		if (PadStateNext && PadStatePrev)
		{
			// Whichever combination has the most number of buttons takes precedence
			// otherwise there may be issues if one binding is a subset of another one
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
					*Game::sel_bgm_kind_buf = 66;
				BGMChanged = true;
			}
		}
		if (KeyStateNext != PrevKeyStateNext)
		{
			PrevKeyStateNext = KeyStateNext;
			if (KeyStateNext)
			{
				*Game::sel_bgm_kind_buf = *Game::sel_bgm_kind_buf + 1;
				if (*Game::sel_bgm_kind_buf >= 67)
					*Game::sel_bgm_kind_buf = 0;
				BGMChanged = true;
			}
		}

		if (BGMChanged)
		{
			Game::adxPlay(0, *Game::sel_bgm_kind_buf);

			SongTitleDisplayTimer = SongTitleDisplayFrames;
		}
	}

public:
	static void draw(int numUpdates)
	{
		if (SongTitleDisplayTimer > 0 && *Game::sel_bgm_kind_buf < 67)
		{
			Game::sprSetFontPriority(4);
			Game::sprSetPrintFont(2);
			Game::sprSetFontScale(0.3f, 0.5f);
			Game::sprLocateP(350, 450);

			uint32_t color = 0xFFFFFF;
			if (SongTitleDisplayTimer > SongTitleFadeBeginFrame)
			{
				color |= 0xFF000000;
			}
			else
			{
				uint8_t alpha = uint8_t((float(SongTitleDisplayTimer) / SongTitleFadeBeginFrame) * 255.f);
				color |= (alpha << 24);
			}
			Game::sprSetFontColor(color);

			const auto& song = SongTitles[*Game::sel_bgm_kind_buf];
			Game::sprPrintf(song.c_str());

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
		PadButtonCombo_Next_BitCount = bitcount(PadButtonCombo_Next);
		PadButtonCombo_Prev_BitCount = bitcount(PadButtonCombo_Prev);

		Game_Ctrl = safetyhook::create_inline(Module::exe_ptr(Game_Ctrl_Addr), destination);
		return !!Game_Ctrl;
	}

	static CDSwitcher instance;
};
CDSwitcher CDSwitcher::instance;

void CDSwitcher_Draw(int numUpdates)
{
	CDSwitcher::draw(numUpdates);
}