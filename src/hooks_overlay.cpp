#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx9.h>

bool show_demo_window = true;

bool f11_prev_state = false; // previously seen F11 state

bool overlay_visible = false; // user wants overlay to show?
bool overlay_showing = false;

void Overlay_GlobalsWindow()
{
	extern bool EnablePauseMenu;

	ImGui::Begin("Globals");

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.51f, 0.00f, 0.14f, 0.00f));
	if (ImGui::Button("-"))
	{
		if (ImGui::GetIO().FontGlobalScale > 1.0f)
		{
			ImGui::GetIO().FontGlobalScale -= 0.05f;
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("+"))
	{
		if (ImGui::GetIO().FontGlobalScale < 4.0f)
		{
			ImGui::GetIO().FontGlobalScale += 0.05f;
		}
	}

	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::Text("Overlay Font Size");

	ImGui::Separator();
	ImGui::Text("Info");
	EVWORK_CAR* car = Game::event(8)->data<EVWORK_CAR>();
	ImGui::Text("Car position: %.3f %.3f %.3f", car->position_14.x, car->position_14.y, car->position_14.z);
	ImGui::Text("OnRoadPlace coli %d, stg %d, section %d", 
		car->OnRoadPlace_5C.loadColiType_0, 
		car->OnRoadPlace_5C.curStageIdx_C, 
		car->OnRoadPlace_5C.roadSectionNum_8);

	int cur_stage_num = *Game::stg_stage_num;
	const char* cur_stage_name = Game::GetStageUniqueName(cur_stage_num);
	ImGui::Text("Loaded Stage: %d (%s)", cur_stage_num, cur_stage_name);

	ImGui::Separator();
	ImGui::Text("Gameplay");

	ImGui::Checkbox("Countdown timer enabled", Game::Sumo_CountdownTimerEnable);
	ImGui::Checkbox("Pause menu enabled", &EnablePauseMenu);
	ImGui::Checkbox("HUD enabled", (bool*)Game::navipub_disp_flg);

	ImGui::Separator();
	ImGui::Text("Controls");

	ImGui::SliderFloat("SteeringDeadZone", &Settings::SteeringDeadZone, 0.01, 1.0f);
	ImGui::SliderInt("VibrationStrength", &Settings::VibrationStrength, 0, 10);
	ImGui::SliderFloat("ImpulseVibrationLeftMultiplier", &Settings::ImpulseVibrationLeftMultiplier, 0.1, 1);
	ImGui::SliderFloat("ImpulseVibrationRightMultiplier", &Settings::ImpulseVibrationRightMultiplier, 0.1, 1);

	ImGui::Separator();
	ImGui::Text("Graphics");

	ImGui::SliderInt("FramerateLimit", &Settings::FramerateLimit, 30, 300);
	ImGui::SliderInt("DrawDistanceIncrease", &Settings::DrawDistanceIncrease, 0, 4096);
	ImGui::SliderInt("DrawDistanceBehind", &Settings::DrawDistanceBehind, 0, 4096);

	ImGui::End();
}

bool Overlay_Update()
{
	bool f11_pressed = (GetAsyncKeyState(VK_F11) & 1);
	if (f11_prev_state && !f11_pressed) // f11 was pressed and now released?
	{
		overlay_visible = !overlay_visible;
		if (overlay_visible)
			ShowCursor(true);
		else
			ShowCursor(false);
	}

	f11_prev_state = f11_pressed;

	if (!overlay_visible)
		return false;

	// Start the Dear ImGui frame
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

#ifdef _DEBUG
	static bool show_demo_window = true;
	// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
	if (show_demo_window)
	{
		ImGui::ShowDemoWindow(&show_demo_window);
	}
#endif

	Overlay_GlobalsWindow();

	void DrawDist_DrawOverlay();
	DrawDist_DrawOverlay();

	ImGui::EndFrame();

	return true;
}

void Overlay_Render()
{
	overlay_showing = Overlay_Update();
	if (overlay_showing)
	{
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
	}
}

class ImguiOverlay : public Hook
{
	inline static bool overlayInited = false;

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
			Overlay_Render();
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
		constexpr int Direct3D_EndScene_CallerAddr = 0x17D4E;

		midhook_d3dinit = safetyhook::create_mid(Module::exe_ptr(InitDirectX_CallerResult_Addr), dest_d3dinit);
		midhook_d3dendscene = safetyhook::create_mid(Module::exe_ptr(Direct3D_EndScene_CallerAddr), dest_d3dendscene);

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
		if (overlay_showing)
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
				else if (msg == WM_ERASEBKGND) // erase window to white during device reset
				{
					RECT rect;
					GetClientRect(hwnd, &rect);
					HBRUSH brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
					FillRect((HDC)wParam, &rect, brush);
					DeleteObject(brush);
					return 1;
				}
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
