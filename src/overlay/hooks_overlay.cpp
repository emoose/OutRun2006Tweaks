#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx9.h>

void Overlay_Init();
bool Overlay_Update();

bool overlayInited = false;
bool overlayActive = false;

class ImguiOverlay : public Hook
{
	inline static SafetyHookMid midhook_d3dinit{};
	static void dest_d3dinit(SafetyHookContext& ctx)
	{
		if (ctx.eax == 0)
			return; // InitDirectX returned false

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
		io.FontGlobalScale = Settings::OverlayFontScale;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsLight();

		// Setup Platform/Renderer backends
		ImGui_ImplWin32_Init(Game::GameHwnd());
		ImGui_ImplDX9_Init(Game::D3DDevice());

		overlayInited = true;
	}

	inline static SafetyHookMid midhook_d3dendscene{};
	static void dest_d3dendscene(SafetyHookContext& ctx)
	{
		if (overlayInited)
		{
			ImGui_ImplDX9_NewFrame();
			ImGui_ImplWin32_NewFrame();
			overlayActive = Overlay_Update();
			if (overlayActive)
			{
				ImGui::Render();
				ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			}
		}
	}

	inline static SafetyHookMid midhook_d3dTemporariesRelease{};
	static void dest_d3dTemporariesRelease(SafetyHookContext& ctx)
	{
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}

	inline static SafetyHookMid midhook_d3dTemporariesCreate{};
	static void dest_d3dTemporariesCreate(SafetyHookContext& ctx)
	{
		ImGui_ImplDX9_CreateDeviceObjects();
	}

public:
	std::string_view description() override
	{
		return "ImguiOverlay";
	}

	bool validate() override
	{
		return Settings::OverlayEnabled;
	}

	bool apply() override
	{
		constexpr int InitDirectX_CallerResult_Addr = 0x1775E;
		constexpr int D3D_ReleaseTemporaries_Addr = 0x17970;
		constexpr int D3D_CreateTemporaries_Addr = 0x17A20;

		// TODO: UILetterboxing also hooks the same spot as Direct3D_EndScene_CallerAddr
		// which could end up having its hook called after this, causing letterbox to draw over overlay
		// so overlay hooks it at +5 to make sure overlay code is always ran afterward, hacky!
		constexpr int Direct3D_EndScene_CallerAddr = 0x17D4E + 5;

		midhook_d3dinit = safetyhook::create_mid(Module::exe_ptr(InitDirectX_CallerResult_Addr), dest_d3dinit);
		midhook_d3dendscene = safetyhook::create_mid(Module::exe_ptr(Direct3D_EndScene_CallerAddr), dest_d3dendscene);

		midhook_d3dTemporariesRelease = safetyhook::create_mid(Module::exe_ptr(D3D_ReleaseTemporaries_Addr), dest_d3dTemporariesRelease);
		midhook_d3dTemporariesCreate = safetyhook::create_mid(Module::exe_ptr(D3D_CreateTemporaries_Addr), dest_d3dTemporariesCreate);

		Overlay_Init();

		return !!midhook_d3dinit && !!midhook_d3dendscene;
	}

	static ImguiOverlay instance;
};
ImguiOverlay ImguiOverlay::instance;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

class WndprocHook : public Hook
{
	const static int WndProc_Addr = 0x17F90;

	inline static SafetyHookInline dest_orig = {};
	static LRESULT __stdcall destination(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (overlayActive)
		{
			if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
				return 1;
		}
		else
		{
			if (Settings::WindowedHideMouseCursor)
			{
				if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && lParam != WA_INACTIVE))
				{
					ShowCursor(false);
				}
			}

			if (msg == WM_ERASEBKGND) // erase window to white during device reset
			{
				RECT rect;
				GetClientRect(hwnd, &rect);
				HBRUSH brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
				FillRect((HDC)wParam, &rect, brush);
				DeleteObject(brush);
				return 1;
			}
		}
		// Other message handling
		return dest_orig.stdcall<LRESULT>(hwnd, msg, wParam, lParam);
	}

public:
	std::string_view description() override
	{
		return "WndprocHook";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		dest_orig = safetyhook::create_inline(Module::exe_ptr(WndProc_Addr), destination);
		return !!dest_orig;
	}

	static WndprocHook instance;
};
WndprocHook WndprocHook::instance;
