#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <json/json.h>
#include <string>
#include <iostream>
#include "plugin.hpp"
#include <spdlog/spdlog.h>
#include "notifications.hpp"
#include "resource.h"

uint64_t VersionToInteger(const std::string& version)
{
	std::vector<int> parts;
	size_t start = 0;
	size_t end = 0;
	if (!version.empty() && version[0] == 'v')
		start = 1;
	while ((end = version.find('.', start)) != std::string::npos)
	{
		parts.push_back(std::stoi(version.substr(start, end - start)));
		start = end + 1;
	}

	parts.push_back(std::stoi(version.substr(start)));
	uint64_t value = 0;
	for (size_t i = 0; i < min(4, parts.size()); ++i)
		value |= (static_cast<uint64_t>(parts[i]) << ((3 - i) * 16));
	return value;
}

bool IsVersionNewer(const std::string& latest, const std::string& current)
{
	return VersionToInteger(latest) > VersionToInteger(current);
}

// Function to check the latest release version
std::string UpdateCheck_IsNewerAvailable(const std::string& currentVersion, const std::string& repoOwner, const std::string& repoName)
{
	std::wstring path = L"/repos/" + std::wstring(repoOwner.begin(), repoOwner.end()) +
		L"/" + std::wstring(repoName.begin(), repoName.end()) + L"/releases/latest";

	std::string jsonResponse = Util::HttpGetRequest("api.github.com", path, 443);
	if (jsonResponse.empty())
	{
		spdlog::error("UpdateCheck_IsNewerAvailable: Failed to fetch the latest release information");
		return "";
	}

	// Parse JSON response
	Json::CharReaderBuilder builder;
	Json::Value root;
	std::string errs;

	std::istringstream stream(jsonResponse);
	if (!Json::parseFromStream(builder, stream, &root, &errs))
	{
		spdlog::error("UpdateCheck_IsNewerAvailable: JSON parsing error: " + errs);
		return "";
	}

	std::string latestVersion = root["tag_name"].asString();
	if (latestVersion.empty())
	{
		spdlog::error("UpdateCheck_IsNewerAvailable: Invalid JSON: Missing 'tag_name'.");
		return "";
	}

	// Compare versions
	if (IsVersionNewer(latestVersion, currentVersion))
	{
		spdlog::info("UpdateCheck_IsNewerAvailable: newer version {} available, current version {}", latestVersion, currentVersion);
		return latestVersion;
	}
	else
	{
		spdlog::info("UpdateCheck_IsNewerAvailable: latest version {} matches current version {}", latestVersion, currentVersion);
		return "";
	}
}

void UpdateCheck_Thread(const std::string& currentVersion, const std::string& repoOwner, const std::string& repoName)
{
	std::string newerVersion = UpdateCheck_IsNewerAvailable(currentVersion, repoOwner, repoName);
	if (!newerVersion.empty())
		Notifications::instance.add(std::format("A newer version of OutRun2006Tweaks is available ({})\n---\nPress F11 and click here to visit release page.", newerVersion), 20,
            [newerVersion]() {
                std::string url = "https://github.com/emoose/OutRun2006Tweaks/releases";
                ShellExecuteA(nullptr, "open", url.c_str(), 0, 0, SW_SHOWNORMAL);
            });
}

std::thread updateCheckThread;

void UpdateCheck_Init()
{
	if (Overlay::NotifyUpdateCheck)
	{
		updateCheckThread = std::thread(&UpdateCheck_Thread, MODULE_VERSION_STR, "emoose", "OutRun2006Tweaks");
		updateCheckThread.detach();
	}
}
