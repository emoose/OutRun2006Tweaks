#pragma once

#include <imgui.h>
#include <deque>
#include <mutex>
#include "game_addrs.hpp"
#include "overlay.hpp"

inline size_t maxNotifications = 5;
inline std::chrono::seconds displayDuration = std::chrono::seconds(10);

inline ImVec2 notificationSize = { 600, 100 };
inline float notificationSpacing = 10.0f;
inline float notificationTextScale = 1.5f;

class Notifications
{
private:
	struct Notification
	{
		std::string message;
		std::chrono::time_point<std::chrono::steady_clock> timestamp;
		int minDisplaySeconds;
        std::function<void()> onMouseClick;
	};

	std::deque<Notification> notifications;
	std::mutex notificationsMutex;

public:
	void add(const std::string& message, int minDisplaySeconds = 0, std::function<void()> onMouseClick = nullptr)
	{
		std::lock_guard<std::mutex> lock(notificationsMutex);
        notifications.push_back({ message, std::chrono::steady_clock::now(), minDisplaySeconds, onMouseClick });

		if (notifications.size() > maxNotifications)
			notifications.pop_front();
	}

	void render()
	{
		// Remove expired notifications
		auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lock(notificationsMutex);

			while (!notifications.empty())
			{
				auto& front = notifications.front();

				auto duration = displayDuration;
				if (front.minDisplaySeconds > 0)
					duration = std::chrono::seconds(front.minDisplaySeconds);

				if (now - front.timestamp <= duration) // notif time hasn't elapsed yet?
					break;

				notifications.pop_front();
			}
		}

		if (Game::is_in_game())
		{
			if (Overlay::NotifyHideMode == Overlay::NotifyHideMode_AllRaces)
				return;

			if (Overlay::NotifyHideMode == Overlay::NotifyHideMode_OnlineRaces &&
				*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_in_lobby() &&
				(*Game::game_mode == 3 || *Game::game_mode == 4))
				return;
		}

		if (!Overlay::NotifyEnable)
			return;

		ImVec2 screenSize = ImGui::GetIO().DisplaySize;

		// Calculate starting position for the latest notification
		// (move it outside of letterbox if letterboxing enabled)
		float contentWidth = screenSize.y / (3.f / 4.f);
		float borderWidth = ((screenSize.x - contentWidth) / 2) + 0.5f;
		if (Settings::UILetterboxing != 1 || Game::is_in_game())
			borderWidth = 0;

		float startX = screenSize.x - notificationSize.x - borderWidth - 10.f;  // 10px padding from the right
		float curY = (screenSize.y / 4.0f) - (notifications.size() * (notificationSize.y + notificationSpacing) / 2.0f);

		std::lock_guard<std::mutex> lock(notificationsMutex);

		for (size_t i = 0; i < notifications.size(); ++i)
		{
			auto windowSize = notificationSize;
			const auto& notification = notifications[i];

			ImGui::SetNextWindowPos(ImVec2(startX, curY));

			std::string windowName = "Notification " + std::to_string(i);
			ImGui::Begin(windowName.c_str(), nullptr, ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

            // Check for click on the notification
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                if (notification.onMouseClick)
                    notification.onMouseClick();

			ImGui::SetWindowFontScale(notificationTextScale);

			// Split the message into lines
			std::vector<std::string> lines;
			if (notification.message.find("---") == std::string::npos)
				lines.push_back(notification.message); // notificiation doesn't have splitter, treat it as all single line for centering
			else
			{
				std::istringstream messageStream(notification.message);
				std::string line;
				while (std::getline(messageStream, line)) {
					lines.push_back(line);
				}
			}

			// Calculate total height for all lines
			float totalTextHeight = 0.0f;
			for (const auto& singleLine : lines) {
				ImVec2 lineSize = ImGui::CalcTextSize(singleLine.c_str(), nullptr, true, windowSize.x - 20.0f);
				totalTextHeight += lineSize.y;
			}
			totalTextHeight += (lines.size() - 1) * ImGui::GetStyle().ItemSpacing.y;

			// Adjust window height if necessary
			if (totalTextHeight + 40.0f > windowSize.y)
				windowSize.y = totalTextHeight + 40.0f;

			ImGui::SetWindowSize(windowSize);

			curY += windowSize.y + notificationSpacing;

			// Center text block vertically
			float paddingY = 5.0f;
			float currentYOffset = (windowSize.y - totalTextHeight) / 2.0f;
			currentYOffset = max(currentYOffset, paddingY);

			for (const auto& singleLine : lines) {
				// Calculate individual line size
				ImVec2 lineSize = ImGui::CalcTextSize(singleLine.c_str(), nullptr, true, windowSize.x - 20.0f);

				// Center line horizontally
				if (singleLine != "---")
				{
					float paddingX = 10.0f;
					float offsetX = (windowSize.x - lineSize.x) / 2.0f;
					offsetX = max(offsetX, paddingX);

					ImGui::SetCursorPos(ImVec2(offsetX, currentYOffset));
					ImGui::TextWrapped("%s", singleLine.c_str());
				}
				else
				{
					ImGui::SetCursorPos(ImVec2(0, currentYOffset + (paddingY * 3)));
					ImGui::Separator();
				}

				// Move down for the next line, including spacing
				currentYOffset += lineSize.y + ImGui::GetStyle().ItemSpacing.y;
			}

			ImGui::End();
		}
	}

	static Notifications instance;
};
