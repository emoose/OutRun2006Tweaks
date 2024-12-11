#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include <mutex>
#include <winhttp.h>
#include <json/json.h>
#include <set>
#include "notifications.hpp"

#pragma comment(lib, "winhttp.lib")

class ServerUpdater
{
public:
	ServerUpdater() {}

	void init()
	{
		running = true;
		updaterThread = std::thread(&ServerUpdater::monitorServers, this);
	}

	~ServerUpdater()
	{
		running = false;
		if (updaterThread.joinable())
			updaterThread.join();
	}

private:
	std::thread updaterThread;
	bool running = false;
	Json::Value previousServers;
	std::mutex dataMutex;

	int numServerUpdates = 0;

	void monitorServers()
	{
		while (running)
		{
			if (!Settings::OverlayNotifyOnlineEnable || !Settings::OverlayNotifyOnlineUpdateTime)
				return;

			try
			{
				std::string jsonContent = downloadJson();
				Json::Value currentServerList = parseJson(jsonContent);

				if (currentServerList.isMember("Servers"))
				{
					auto& currentServers = currentServerList["Servers"];
					handleNewServers(currentServers);

					// Save the current state as the previous state for the next check
					std::lock_guard<std::mutex> lock(dataMutex);
					previousServers = currentServers;

					numServerUpdates++;
				}
			}
			catch (const std::exception& e)
			{
			}

			if (Settings::OverlayNotifyOnlineUpdateTime < 10)
				Settings::OverlayNotifyOnlineUpdateTime = 10; // pls don't hammer us

			std::this_thread::sleep_for(std::chrono::seconds(Settings::OverlayNotifyOnlineUpdateTime));
		}
	}

	std::string downloadJson()
	{
		HINTERNET hSession = WinHttpOpen(L"ServerUpdater/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0);
		if (!hSession) throw std::runtime_error("Failed to open WinHTTP session.");

		HINTERNET hConnect = WinHttpConnect(hSession, L"clarissa.port0.org", INTERNET_DEFAULT_HTTP_PORT, 0);
		if (!hConnect) {
			WinHttpCloseHandle(hSession);
			throw std::runtime_error("Failed to connect to server.");
		}

		HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/servers.json",
			NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
		if (!hRequest) {
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			throw std::runtime_error("Failed to open request.");
		}

		bool result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
		if (!result || !WinHttpReceiveResponse(hRequest, NULL)) {
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			throw std::runtime_error("Failed to send or receive HTTP request.");
		}

		std::string response;
		DWORD bytesRead;
		char buffer[4096];
		do {
			WinHttpQueryDataAvailable(hRequest, &bytesRead);
			if (bytesRead == 0) break;

			DWORD bytesFetched;
			WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesFetched);
			response.append(buffer, bytesFetched);
		} while (bytesRead > 0);

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);

		return response;
	}

	Json::Value parseJson(const std::string& jsonContent)
	{
		Json::Value root;
		Json::CharReaderBuilder reader;
		std::string errs;
		std::istringstream stream(jsonContent);
		if (!Json::parseFromStream(reader, stream, &root, &errs))
			throw std::runtime_error("Failed to parse JSON: " + errs);

		return root;
	}

	void handleNewServers(const Json::Value& currentServers)
	{
		// Create a set of unique identifiers from the previous servers
		std::set<std::string> previousIdentifiers;
		if (previousServers.isArray())
		{
			for (const auto& previousServer : previousServers)
			{
				if (previousServer.isMember("HostName") && previousServer.isMember("Platform") && previousServer.isMember("Reachable"))
				{
					if (previousServer["Reachable"].asBool())
					{
						std::string identifier = previousServer["HostName"].asString() + "_" + previousServer["Platform"].asString();
						previousIdentifiers.insert(identifier);
					}
				}
			}
		}

		int numValid = 0;

		// Compare current servers to previous identifiers
		for (const auto& server : currentServers)
		{
			if (server.isMember("HostName") && server.isMember("Platform") && server.isMember("Reachable"))
			{
				bool reachable = server["Reachable"].asBool();
				if (reachable)
				{
					numValid++;
				}

				if (numServerUpdates > 0) // have we fetched server info before?
				{
					auto hostName = server["HostName"].asString();

					if (!hostName.empty())
					{
						std::string identifier = hostName + "_" + server["Platform"].asString();
						bool ourLobby = !strncmp(hostName.c_str(), Game::SumoNet_OnlineUserName, 16);
						if (!ourLobby)
						{
							if (reachable && previousIdentifiers.find(identifier) == previousIdentifiers.end())
								Notifications::instance.add(hostName + " started hosting a lobby!");
						}
						else
						{
							Notifications::instance.add(reachable ?
								"Your lobby is active & accessible!" :
								"Your lobby cannot be reached by the master server!\nYou may need to setup port-forwarding for UDP ports 41455/41456/41457.", reachable ? 0 : 20);
						}
					}
				}
			}
		}

		if (numServerUpdates == 0 && numValid > 0)
		{
			// First update since game launch and we have some servers, write a notify about it
			Notifications::instance.add("There are " + std::to_string(numValid) + " online lobbies active!");
		}
	}

public:
	static ServerUpdater instance;
};
ServerUpdater ServerUpdater::instance;

void ServerNotifications_Init()
{
	ServerUpdater::instance.init();
}
