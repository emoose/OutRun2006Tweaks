#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx9.h>
#include "overlay.hpp"

bool overlayInited = false;
bool overlayActive = false;

struct CUSTOMVERTEX
{
	FLOAT x, y, z, rhw;
	D3DCOLOR color;
};
#define CUSTOMFVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

IDirect3DVertexBuffer9* LetterboxVertex = nullptr;

void CreateLetterboxVertex()
{
	IDirect3DDevice9* d3ddev = Game::D3DDevice();

	float screenWidth = float(Game::screen_resolution->x);
	float screenHeight = float(Game::screen_resolution->y);
	float contentWidth = screenHeight / (3.f / 4.f);
	float borderWidth = ((screenWidth - contentWidth) / 2) + 0.5f;

	// Add half-pixel offset so there's no white border...
	screenHeight = screenHeight + 0.5f;
	screenWidth = screenWidth + 0.5f;

	CUSTOMVERTEX vertices[] =
	{
		// Left border
		{ -0.5f, -0.5f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ borderWidth, -0.5f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ -0.5f, screenHeight, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ borderWidth, screenHeight, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },

		// Right border
		{ (screenWidth - borderWidth), -0.5f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ screenWidth, -0.5f, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ (screenWidth - borderWidth), screenHeight, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) },
		{ screenWidth, screenHeight, 0.0f, 1.0f, D3DCOLOR_XRGB(0, 0, 0) }
	};

	d3ddev->CreateVertexBuffer(8 * sizeof(CUSTOMVERTEX),
		0,
		CUSTOMFVF,
		D3DPOOL_MANAGED,
		&LetterboxVertex,
		NULL);

	void* pVoid;
	LetterboxVertex->Lock(0, 0, &pVoid, 0);
	memcpy(pVoid, vertices, sizeof(vertices));
	LetterboxVertex->Unlock();
}

class D3DHooks : public Hook
{
	inline static SafetyHookMid midhook_d3dinit{};
	static void D3DInit(SafetyHookContext& ctx)
	{
		if (ctx.eax == 0)
			return; // InitDirectX returned false

		CreateLetterboxVertex();

		if (!Settings::OverlayEnabled)
			return;

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
		io.FontGlobalScale = Overlay::GlobalFontScale;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsLight();

		// Setup Platform/Renderer backends
		ImGui_ImplWin32_Init(Game::GameHwnd());
		ImGui_ImplDX9_Init(Game::D3DDevice());

		overlayInited = true;
	}

	inline static SafetyHookMid midhook_d3dendscene{};
	static void D3DEndScene(SafetyHookContext& ctx)
	{
		if (Settings::UILetterboxing > 0 && Settings::UIScalingMode > 0)
		{
			IDirect3DDevice9* d3ddev = Game::D3DDevice();

			// Only show letterboxing if not in game, or UILetterboxing is 2
			if (!Game::is_in_game() || Settings::UILetterboxing == 2)
			{
				// Backup existing cullmode and set to none, otherwise we won't get drawn
				DWORD prevCullMode = D3DCULL_NONE;
				d3ddev->GetRenderState(D3DRS_CULLMODE, &prevCullMode);
				d3ddev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

				// Seems these SetRenderState/SetTexture calls are needed for DGVoodoo to render letterbox while imgui is active
				// DXVK/D3D9 seem to work fine without them
				// TODO: the game keeps its own copies of the render states so it can update them if needed, should we update the games copy here?
				{
					// Set render states
					d3ddev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
					d3ddev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
					d3ddev->SetRenderState(D3DRS_LIGHTING, FALSE);

					// Set texture stage states to avoid any texture influence
					d3ddev->SetTexture(0, NULL);
					d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
				}

				// Set FVF and stream source
				d3ddev->SetFVF(CUSTOMFVF);
				d3ddev->SetStreamSource(0, LetterboxVertex, 0, sizeof(CUSTOMVERTEX));

				// Draw left border
				d3ddev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

				// Draw right border
				d3ddev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2);

				// Restore original cullmode
				d3ddev->SetRenderState(D3DRS_CULLMODE, prevCullMode);
			}
		}

		if (overlayInited)
		{
			ImGui_ImplDX9_NewFrame();
			ImGui_ImplWin32_NewFrame();
			overlayActive = Overlay::render();
			ImGui::Render();
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		}
	}

	inline static SafetyHookMid midhook_d3dTemporariesRelease{};
	static void D3DTemporariesRelease(SafetyHookContext& ctx)
	{
		if (overlayInited)
			ImGui_ImplDX9_InvalidateDeviceObjects();

		if (LetterboxVertex)
		{
			LetterboxVertex->Release();
			LetterboxVertex = nullptr;
		}
	}

	inline static SafetyHookMid midhook_d3dTemporariesCreate{};
	static void D3DTemporariesCreate(SafetyHookContext& ctx)
	{
		if (overlayInited)
			ImGui_ImplDX9_CreateDeviceObjects();

		CreateLetterboxVertex();
	}

public:
	std::string_view description() override
	{
		return "D3DHooks";
	}

	bool validate() override
	{
		return true;
	}

	bool apply() override
	{
		constexpr int InitDirectX_CallerResult_Addr = 0x1775E;
		constexpr int Direct3D_EndScene_CallerAddr = 0x17D4E;
		constexpr int D3D_ReleaseTemporaries_Addr = 0x17970;
		constexpr int D3D_CreateTemporaries_Addr = 0x17A20;

		midhook_d3dinit = safetyhook::create_mid(Module::exe_ptr(InitDirectX_CallerResult_Addr), D3DInit);
		midhook_d3dendscene = safetyhook::create_mid(Module::exe_ptr(Direct3D_EndScene_CallerAddr), D3DEndScene);

		midhook_d3dTemporariesRelease = safetyhook::create_mid(Module::exe_ptr(D3D_ReleaseTemporaries_Addr), D3DTemporariesRelease);
		midhook_d3dTemporariesCreate = safetyhook::create_mid(Module::exe_ptr(D3D_CreateTemporaries_Addr), D3DTemporariesCreate);

		Overlay::init();

		return !!midhook_d3dinit && !!midhook_d3dendscene;
	}

	static D3DHooks instance;
};
D3DHooks D3DHooks::instance;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

class WndprocHook : public Hook
{
	const static int WndProc_Addr = 0x17F90;

	inline static SafetyHookInline dest_orig = {};
	static LRESULT __stdcall destination(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return 1;

		if (Settings::WindowedHideMouseCursor && !overlayActive)
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
