#pragma once

class OverlayWindow
{
public:
	OverlayWindow();
	virtual ~OverlayWindow() = default;
	virtual void init() = 0;
	virtual void render(bool overlayEnabled) = 0;
};

class Overlay
{
public:
	inline static float GlobalFontScale = 1.5f;
	inline static float GlobalOpacity = 0.8f;

	inline static bool NotifyEnable = true;
	inline static int NotifyDisplayTime = 7;
	inline static bool NotifyOnlineEnable = true;
	inline static int NotifyOnlineUpdateTime = 20;
	inline static int NotifyHideMode = 1;
	inline static bool NotifyUpdateCheck = true;

	enum NotifyHideModes
	{
		NotifyHideMode_Never = 0,
		NotifyHideMode_OnlineRaces = 1,
		NotifyHideMode_AllRaces = 2
	};

	inline static bool CourseReplacementEnabled = false;
	inline static char CourseReplacementCode[256] = { 0 };

	inline static bool ChatHideBackground = false;
	inline static float ChatFontSize = 1.0f;

	inline static bool IsActive = false;

private:
	inline static std::vector<OverlayWindow*> s_windows;
	inline static bool s_hasInited = false;

public:
	static void init();
	static void init_imgui();

	static bool settings_read();
	static bool settings_write();

	static void add_window(OverlayWindow* window)
	{
		s_windows.emplace_back(window);
	}

	static bool render();
};
