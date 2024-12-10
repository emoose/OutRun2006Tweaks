#include <imgui.h>
#include <deque>
#include <mutex>

inline size_t maxNotifications = 3;
inline std::chrono::seconds displayDuration = std::chrono::seconds(7);

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
	};

	std::deque<Notification> notifications;
	std::mutex notificationsMutex;

public:
	void add(const std::string& message, int minDisplaySeconds = 0)
	{
		std::lock_guard<std::mutex> lock(notificationsMutex);
		notifications.push_back({ message, std::chrono::steady_clock::now(), minDisplaySeconds });

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
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

			ImGui::SetWindowFontScale(notificationTextScale);
			ImVec2 textSize = ImGui::CalcTextSize(notification.message.c_str(), nullptr, true, windowSize.x - 20.0f); // Account for padding

			// Adjust window height if text is larger than current window height
			if (textSize.y + 40.0f > windowSize.y)
				windowSize.y = textSize.y + 40.0f;

			ImGui::SetWindowSize(windowSize);

			curY += windowSize.y + notificationSpacing;

			// Center text with padding
			float paddingX = 10.0f, paddingY = 5.0f;
			float offsetX = (windowSize.x - textSize.x) / 2.0f;
			float offsetY = (windowSize.y - textSize.y) / 2.0f;
			offsetX = max(offsetX, paddingX);
			offsetY = max(offsetY, paddingY);

			ImGui::SetCursorPos(ImVec2(offsetX, offsetY));
			ImGui::TextWrapped("%s", notification.message.c_str());

			ImGui::End();
		}
	}

	static Notifications instance;
};
