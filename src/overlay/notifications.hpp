#include <imgui.h>
#include <deque>
#include <mutex>

inline size_t maxNotifications = 3;
inline std::chrono::seconds displayDuration = std::chrono::seconds(7);

inline ImVec2 notificationSize = { 600, 100 };
inline float notificationSpacing = 10.0f;

class Notifications
{
private:
	struct Notification
	{
		std::string message;
		std::chrono::time_point<std::chrono::steady_clock> timestamp;
	};

	std::deque<Notification> notifications;
	std::mutex notificationsMutex;

public:
	void add(const std::string& message)
	{
		std::lock_guard<std::mutex> lock(notificationsMutex);
		notifications.push_back({ message, std::chrono::steady_clock::now() });

		if (notifications.size() > maxNotifications)
			notifications.pop_front();
	}

	void render()
	{
		// Remove expired notifications
		auto now = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lock(notificationsMutex);
			while (!notifications.empty() && now - notifications.front().timestamp > displayDuration)
				notifications.pop_front();
		}

		ImVec2 screenSize = ImGui::GetIO().DisplaySize;

		// Calculate starting position for the latest notification
		float startX = screenSize.x - notificationSize.x - 10.0f;  // 10px padding from the right
		float startY = (screenSize.y / 4.0f) - (notifications.size() * (notificationSize.y + notificationSpacing) / 2.0f);

		std::lock_guard<std::mutex> lock(notificationsMutex);

		for (size_t i = 0; i < notifications.size(); ++i)
		{
			const auto& notification = notifications[i];

			float posY = startY + i * (notificationSize.y + notificationSpacing);
			ImGui::SetNextWindowPos(ImVec2(startX, posY));
			ImGui::SetNextWindowSize(notificationSize);

			std::string windowName = "Notification " + std::to_string(i);
			ImGui::Begin(windowName.c_str(), nullptr, ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

			ImGui::SetWindowFontScale(1.5f);

			// Calculate offsets to center text
			ImVec2 textSize = ImGui::CalcTextSize(notification.message.c_str());
			float offsetX = (notificationSize.x - textSize.x) / 2.0f;
			float offsetY = (notificationSize.y - textSize.y) / 2.0f;

			// Padding
			float paddingX = 10.0f;
			float paddingY = 5.0f;
			offsetX = max(offsetX, paddingX);
			offsetY = max(offsetY, paddingY);

			// Position cursor for centered text
			ImGui::SetCursorPos(ImVec2(offsetX, offsetY));
			ImGui::TextWrapped("%s", notification.message.c_str());
			ImGui::End();
		}
	}

	static Notifications instance;
};
