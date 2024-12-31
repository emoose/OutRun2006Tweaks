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
	static constexpr float MESSAGE_DISPLAY_SECONDS = 5.0f;
	static constexpr float MESSAGE_VERYRECENT_SECONDS = 2.0f;

	std::mutex mtx;

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
		std::lock_guard<std::mutex> lock(mtx);

		messages.push_front({ content, std::chrono::system_clock::now() });

		if (messages.size() > MAX_MESSAGES)
			messages.pop_back();
	}

public:
	void init() override
	{
		connectWebSocket();
	}

	static void DrawTextWithOutline(const char* text, ImVec4 textColor, ImVec4 outlineColor, float outlineThickness = 1.0f)
	{
		ImVec2 pos = ImGui::GetCursorPos();

		// Draw the outline by drawing the text multiple times with offsets
		ImGui::PushStyleColor(ImGuiCol_Text, outlineColor);
		for (float dx = -outlineThickness; dx <= outlineThickness; dx += outlineThickness)
		{
			for (float dy = -outlineThickness; dy <= outlineThickness; dy += outlineThickness)
			{
				ImGui::SetCursorPos(ImVec2(pos.x + dx, pos.y + dy));
				ImGui::TextWrapped("%s", text);
			}
		}
		ImGui::PopStyleColor();

		// Draw the main text
		ImGui::SetCursorPos(pos);
		ImGui::PushStyleColor(ImGuiCol_Text, textColor);
		ImGui::TextWrapped("%s", text);
		ImGui::PopStyleColor();
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

		{
			std::lock_guard<std::mutex> lock(mtx);
			for (const auto& msg : messages)
			{
				auto duration = std::chrono::duration_cast<std::chrono::seconds>(
					currentTime - msg.timestamp).count();

				if (duration < MESSAGE_VERYRECENT_SECONDS)
				{
					hasVeryRecentMessages = true;
					hasRecentMessages = true;
					break;
				}
				if (duration < MESSAGE_DISPLAY_SECONDS)
				{
					hasRecentMessages = true;
					break;
				}
			}
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

			{
				std::lock_guard<std::mutex> lock(mtx);

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
						DrawTextWithOutline(msg.content.c_str(), ImVec4(0.0f, 0.0f, 0.0f, 1.0f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f));// 0xFF000000, 0xFFFFFFFF);
					else
						ImGui::TextWrapped("%s", msg.content.c_str());
				}
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