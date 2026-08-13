#pragma once

class OverlayWindow
{
public:
	enum class Kind
	{
		// Drawn every frame, whether or not the overlay is open, and responsible
		// for opening its own window. Notifications and chat sit against game
		// content, and the binding dialog can be opened by in-game code.
		Hud,

		// Drawn as a tab of the single shell window. The shell has already
		// opened the tab by the time render runs, so these draw their contents
		// directly without a Begin of their own.
		Tab,

		// Free-floating window with its own visibility flag, switched on from
		// the list in the Debug tab. Only drawn while the overlay is open.
		Tool,
	};

	OverlayWindow();
	virtual ~OverlayWindow() = default;

	virtual Kind kind() const { return Kind::Tool; }

	// Tab label, or the entry in the Debug tab's tool list.
	virtual const char* name() const = 0;

	// Tabs and tools are shown in this order rather than the order their
	// translation units happened to initialise in.
	virtual int order() const { return 100; }

	// Left out of the UI entirely in release builds.
	virtual bool debug_only() const { return false; }

	virtual void init() = 0;
	virtual void render(bool overlayEnabled) = 0;

	// Tools only, switched from the list in the Debug tab. Resets each launch;
	// ImGui's own ini keeps the window's position and size, not this.
	bool visible = false;
};

class Overlay
{
public:
	inline static float GlobalFontScale = 2.0f;
	inline static float GlobalOpacity = 0.8f;

	enum Themes
	{
		Theme_DarkCoast = 0,
		Theme_Coast2Coast,
		Theme_Luna,
		Theme_Dark,
		Theme_Light,
		Theme_Classic,
		Theme_Count
	};
	inline static int CurrentTheme = Theme_DarkCoast;

	// Theme_Count entries, in enum order.
	static const char* const* theme_names();

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

	inline static int ChatMode = 0;
	inline static bool ChatHideBackground = true;
	inline static float ChatFontSize = 1.0f;

	enum ChatModes
	{
		ChatMode_Disabled = 0,
		ChatMode_Enabled = 1,
		ChatMode_EnabledOnMenus = 2,
	};

	inline static bool IsActive = false;

	inline static bool RequestBindingDialog = false;
	inline static bool IsBindingDialogActive = false;
	inline static bool RequestMouseHide = false;

	// The 4:3 area the game draws its menus inside, in screen pixels. When
	// letterboxing is on, everything outside this is covered by black bars, so
	// overlay elements that sit against game content stay within it.
	struct ContentRect
	{
		float x, y;
		float width, height;
	};
	static ContentRect content_rect();

private:
	// Keep windows vector inside function-local static, to ensure vector actually exists
	// before OverlayWindows try to register themselves.
	static std::vector<OverlayWindow*>& windows_storage()
	{
		static std::vector<OverlayWindow*> s_windows;
		return s_windows;
	}

	inline static bool s_hasInited = false;

	static void render_shell();

public:
	static void init();
	static void init_imgui();

	// Colours and metrics for every overlay window, from CurrentTheme. Split out
	// from init_imgui so the style can be put back after the ImGui style editor
	// has been played with, and so a theme change can re-run it.
	static void apply_style();

	// Rasterises the UI font at the current GlobalFontScale. The D3D font texture
	// has to be dropped afterwards, so set FontsDirty rather than calling this
	// directly; D3DEndScene picks it up between frames.
	static void rebuild_fonts();
	inline static bool FontsDirty = false;

	static bool settings_read();
	static bool settings_write();

	static void add_window(OverlayWindow* window)
	{
		windows_storage().emplace_back(window);
	}

	// Registered windows, sorted by kind then order. Rebuilt on first use, since
	// nothing registers after the overlay has started rendering.
	static const std::vector<OverlayWindow*>& windows();

	static bool render();
};
