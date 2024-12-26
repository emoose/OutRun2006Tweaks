#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include <mutex>
#include <json/json.h>
#include <set>
#include "notifications.hpp"

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
			if (!Overlay::NotifyOnlineEnable || !Overlay::NotifyOnlineUpdateTime)
				return;

			try
			{
				std::string jsonContent = Util::HttpGetRequest(L"clarissa.port0.org", L"/servers.json", false);
				if (!jsonContent.empty())
				{
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
			}
			catch (const std::exception& e)
			{
			}

			if (Overlay::NotifyOnlineUpdateTime < 10)
				Overlay::NotifyOnlineUpdateTime = 10; // pls don't hammer us

			std::this_thread::sleep_for(std::chrono::seconds(Overlay::NotifyOnlineUpdateTime));
		}
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
					numValid++;

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
							if (*Game::SumoNet_CurNetDriver && (*Game::SumoNet_CurNetDriver)->is_hosting_online() && !Game::is_in_game())
								Notifications::instance.add(reachable ?
									"Your lobby is active & accessible!" :
									"Your lobby cannot be reached by the master server!\n\nYou may need to setup port-forwarding for UDP ports 41455/41456/41457.", reachable ? 0 : 20);
						}
					}
				}
			}
		}

		// First update since game launch and we have some servers, write a notify about it
		if (numServerUpdates == 0 && numValid > 0)
		{
			if (numValid == 1)
				Notifications::instance.add("There is 1 online lobby active!");
			else
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
