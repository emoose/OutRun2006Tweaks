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
		// Expiry and snapshotting happen under the same lock. The updater runs on
		// a detached background thread, so even a size() read outside this lock is
		// a data race. Rendering from a copy also avoids holding the mutex while
		// ImGui and click callbacks execute.
		std::deque<Notification> visibleNotifications;
		const auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lock(notificationsMutex);

			while (!notifications.empty())
			{
				auto& front = notifications.front();

				auto duration = displayDuration;
				if (front.minDisplaySeconds > 0)
					duration = std::chrono::seconds(front.minDisplaySeconds);

				if (now - front.timestamp <= duration)
					break;

				notifications.pop_front();
			}

			visibleNotifications = notifications;
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

		// Latest notification goes against the right edge of the game's content,
		// which letterboxing pulls inward.
		const Overlay::ContentRect content = Overlay::content_rect();

		float startX = content.x + content.width - notificationSize.x - 10.f;
		float curY = (content.height / 4.0f) -
			(visibleNotifications.size() * (notificationSize.y + notificationSpacing) / 2.0f);

		for (size_t i = 0; i < visibleNotifications.size(); ++i)
		{
			auto windowSize = notificationSize;
			const auto& notification = visibleNotifications[i];

			ImGui::SetNextWindowPos(ImVec2(startX, curY));

			std::string windowName = "Notification " + std::to_string(i);
			ImGui::Begin(windowName.c_str(), nullptr, ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

			if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				if (notification.onMouseClick)
					notification.onMouseClick();

			ImGui::SetWindowFontScale(notificationTextScale);

			std::vector<std::string> lines;
			if (notification.message.find("---") == std::string::npos)
				lines.push_back(notification.message);
			else
			{
				std::istringstream messageStream(notification.message);
				std::string line;
				while (std::getline(messageStream, line))
					lines.push_back(line);
			}

			float totalTextHeight = 0.0f;
			for (const auto& singleLine : lines)
			{
				ImVec2 lineSize = ImGui::CalcTextSize(singleLine.c_str(), nullptr, true, windowSize.x - 20.0f);
				totalTextHeight += lineSize.y;
			}
			totalTextHeight += (lines.size() - 1) * ImGui::GetStyle().ItemSpacing.y;

			if (totalTextHeight + 40.0f > windowSize.y)
				windowSize.y = totalTextHeight + 40.0f;

			ImGui::SetWindowSize(windowSize);
			curY += windowSize.y + notificationSpacing;

			float paddingY = 5.0f;
			float currentYOffset = (windowSize.y - totalTextHeight) / 2.0f;
			currentYOffset = max(currentYOffset, paddingY);

			for (const auto& singleLine : lines)
			{
				ImVec2 lineSize = ImGui::CalcTextSize(singleLine.c_str(), nullptr, true, windowSize.x - 20.0f);

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

				currentYOffset += lineSize.y + ImGui::GetStyle().ItemSpacing.y;
			}

			ImGui::End();
		}
	}

	static Notifications instance;
};
