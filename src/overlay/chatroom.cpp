#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "notifications.hpp"
#include <array>
#include <random>
#include "overlay.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <deque>
#include <string>
#include <chrono>

struct ChatMessage
{
	std::string content;
	std::chrono::system_clock::time_point timestamp;
};

class ChatRoom : public OverlayWindow
{
private:
	ix::WebSocket webSocket;
	bool isActive = false;
	char inputBuffer[256] = "";
	std::deque<ChatMessage> messages;
	static constexpr size_t MAX_MESSAGES = 100;
	static constexpr float MESSAGE_DISPLAY_DURATION = 6.0f; // seconds
	static constexpr float MESSAGE_VERYRECENT_DURATION = 3.0f;

	void connectWebSocket()
	{
		std::string url = "ws://" + Settings::DemonwareServerOverride + "/ws";
		if (Settings::DemonwareServerOverride == "localhost")
			url = "ws://localhost:4444/ws";
		webSocket.setUrl(url);

		webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg)
		{
			if (msg->type == ix::WebSocketMessageType::Message)
				addMessage(msg->str);
		});

		webSocket.start();
	}

	void addMessage(const std::string& content)
	{
		messages.push_front({ content, std::chrono::system_clock::now() });

		if (messages.size() > MAX_MESSAGES)
			messages.pop_back();
	}

public:
	void init() override
	{
		connectWebSocket();
	}

	static void RenderTextWithOutline(ImVec2 pos, const char* text, ImU32 textColor, ImU32 outlineColor, float outlineThickness = 1.0f)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		// Render outline by drawing text slightly offset in all directions
		drawList->AddText(ImVec2(pos.x - outlineThickness, pos.y), outlineColor, text);
		drawList->AddText(ImVec2(pos.x + outlineThickness, pos.y), outlineColor, text);
		drawList->AddText(ImVec2(pos.x, pos.y - outlineThickness), outlineColor, text);
		drawList->AddText(ImVec2(pos.x, pos.y + outlineThickness), outlineColor, text);

		// Render the main text
		drawList->AddText(pos, textColor, text);
	}

	static void RenderTextWithOutlineWrapped(const char* text, ImU32 textColor, ImU32 outlineColor, float wrapWidth = -1.0f, float outlineThickness = 1.0f)
	{
		// Get starting position
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();

		// Calculate wrapped text
		ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);

		// Render the text with outline
		RenderTextWithOutline(cursorPos, text, textColor, outlineColor, outlineThickness);

		// Manually advance the cursor position
		ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + textSize.y));
	}

	void render(bool overlayEnabled) override
	{
		// Toggle active mode with 'Y' key
		bool justOpened = false;
		if (!isActive && ImGui::IsKeyReleased(ImGuiKey_Y))
		{
			justOpened = true;
			isActive = true;
		}

		if (isActive && ImGui::IsKeyReleased(ImGuiKey_Escape))
			isActive = false;

		if (isActive && ImGui::IsKeyReleased(ImGuiKey_Enter))
			isActive = false;

		if (isActive)
			Overlay::IsActive = true;

		auto currentTime = std::chrono::system_clock::now();
		bool hasRecentMessages = false;
		bool hasVeryRecentMessages = false;

		for (const auto& msg : messages)
		{
			auto duration = std::chrono::duration_cast<std::chrono::seconds>(
				currentTime - msg.timestamp).count();

			if (duration < MESSAGE_VERYRECENT_DURATION)
			{
				hasVeryRecentMessages = true;
				hasRecentMessages = true;
			}
			else if (duration < MESSAGE_DISPLAY_DURATION)
				hasRecentMessages = true;
		}

		// If not active then only show window if there are recent messages
		if (!isActive && !hasRecentMessages)
			return;

		if (Overlay::ChatHideBackground)
			ImGui::SetNextWindowBgAlpha(0.f);
		else
		{
			if (!isActive && !hasVeryRecentMessages)
				ImGui::SetNextWindowBgAlpha(0.3f);
		}

		float screenWidth = float(Game::screen_resolution->x);
		float screenHeight = float(Game::screen_resolution->y);
		float contentWidth = screenHeight / (3.f / 4.f);
		float borderWidth = ((screenWidth - contentWidth) / 2) + 0.5f;

		float screenQuarterHeight = screenHeight / 4;

		ImGui::SetNextWindowPos(ImVec2(borderWidth + 20, (screenQuarterHeight * 3) - 20));
		ImGui::SetNextWindowSize(ImVec2(contentWidth - 40, screenQuarterHeight), ImGuiCond_FirstUseEver);

		if (ImGui::Begin("Chat Messages", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav))
		{
			ImGui::BeginChild("ScrollingRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);

			// Get total available height of scroll region
			float availableHeight = ImGui::GetContentRegionAvail().y;

			// Calculate total height of messages
			float totalMessageHeight = 0;
			for (auto it = messages.rbegin(); it != messages.rend(); ++it)
			{
				const auto& msg = *it;
				float textHeight = ImGui::CalcTextSize(msg.content.c_str(), nullptr, true, ImGui::GetContentRegionAvail().x).y;
				totalMessageHeight += textHeight + ImGui::GetStyle().ItemSpacing.y;
			}

			// Add dummy spacing if content doesn't fill the height
			if (totalMessageHeight < availableHeight)
				ImGui::Dummy(ImVec2(0, availableHeight - totalMessageHeight));

			// Draw messages
			for (auto it = messages.rbegin(); it != messages.rend(); ++it)
			{
				const auto& msg = *it;

				if (Overlay::ChatHideBackground)
					RenderTextWithOutlineWrapped(msg.content.c_str(), 0xFF000000, 0xFFFFFFFF);
				else
					ImGui::TextWrapped("%s", msg.content.c_str());
			}

			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
				ImGui::SetScrollHereY(1.0f);

			ImGui::EndChild();

			if (isActive)
			{
				// Input box
				ImGui::Separator();

				float availableWidth = ImGui::GetContentRegionAvail().x;
				ImGui::PushItemWidth(availableWidth);
				if (ImGui::InputText("##Message", inputBuffer, sizeof(inputBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
				{
					if (inputBuffer[0] != '\0')
					{
						webSocket.send(inputBuffer);
						inputBuffer[0] = '\0';
					}
				}
				ImGui::PopItemWidth();

				if (justOpened)
					ImGui::SetKeyboardFocusHere(-1);
			}
		}
		ImGui::End();

	}

	~ChatRoom()
	{
		webSocket.stop();
	}

	static ChatRoom instance;
};

ChatRoom ChatRoom::instance;