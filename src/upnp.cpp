#include "hook_mgr.hpp"
#include "upnp.hpp"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace UPnP
{
	namespace
	{
		struct State
		{
			std::mutex statusMutex;
			Status current;

			std::atomic<bool> running{ false };

			// By pointer so nothing destroys it: ~thread terminates on a joinable thread, and a
			// finished attempt stays joinable until the next refresh, which may never come.
			std::thread* worker = nullptr;

			// Kept from a successful attempt so shutdown can delete the mappings without
			// discovering all over again.
			std::mutex igdMutex;
			std::string controlUrl;
			std::string serviceType;
			std::string lanAddress;
			bool mapped = false;
		};

		// Never destroyed. Windows kills the worker thread before running the DLL's static
		// teardown, so a worker stopped midway through publishing would leave this half written
		// for a destructor to walk.
		State& state()
		{
			static State* instance = new State();
			return *instance;
		}

		struct DeviceList
		{
			UPNPDev* devices = nullptr;

			~DeviceList()
			{
				if (devices)
					freeUPNPDevlist(devices);
			}
		};

		// UPNP_GetValidIGD leaves this untouched when it finds nothing, so the zero
		// initialisation is what makes the free safe.
		struct IgdUrls
		{
			UPNPUrls urls{};
			bool valid = false;

			~IgdUrls()
			{
				if (valid)
					FreeUPNPUrls(&urls);
			}
		};

		// Routers that think their internet side is down report 0.0.0.0, which is no answer
		// rather than a bad one.
		bool is_unspecified(const std::string& address)
		{
			return address.empty() || address == "0.0.0.0";
		}

		// A router whose own external address is one of these sits behind another NAT, so its
		// mappings are dead ends.
		bool is_unroutable(const std::string& address)
		{
			unsigned int a = 0, b = 0, c = 0, d = 0;
			if (sscanf_s(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
				return false;

			if (a == 0 || a == 127) return true;                 // unspecified, loopback
			if (a == 10) return true;                            // 10.0.0.0/8
			if (a == 172 && b >= 16 && b <= 31) return true;     // 172.16.0.0/12
			if (a == 192 && b == 168) return true;               // 192.168.0.0/16
			if (a == 169 && b == 254) return true;               // link local
			if (a == 100 && b >= 64 && b <= 127) return true;    // 100.64.0.0/10, carrier grade NAT

			return false;
		}

		void publish(const Status& status)
		{
			std::lock_guard _(state().statusMutex);
			state().current = status;
		}

		void publish_failure(Status& status, Failure failure, int errorCode)
		{
			status.stage = Stage::Failed;
			status.failure = failure;
			status.errorCode = errorCode;
			status.finishedAt = std::chrono::steady_clock::now();
			publish(status);
		}

		// Some routers answer "success" to AddPortMapping and store nothing, and some hand the
		// port to a different machine. Neither shows up in the add call's result.
		void verify_mapping(const UPNPUrls& urls, const IGDdatas& data, const char* portStr,
			const char* protocol, bool& verified, std::string& client)
		{
			char intClient[16] = {};
			char intPort[6] = {};
			char desc[80] = {};
			char enabled[4] = {};
			char leaseDuration[16] = {};

			int ret = UPNP_GetSpecificPortMappingEntry(urls.controlURL, data.first.servicetype,
				portStr, protocol, nullptr, intClient, intPort, desc, enabled, leaseDuration);

			verified = (ret == UPNPCOMMAND_SUCCESS);
			if (verified)
				client = intClient;
		}

		void attempt()
		{
			Status status;
			status.stage = Stage::Discovering;
			publish(status);

			int discoverError = UPNPDISCOVER_SUCCESS;
			DeviceList devices;
			devices.devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &discoverError);

			if (discoverError != UPNPDISCOVER_SUCCESS)
			{
				spdlog::error("UPnP: upnpDiscover failed with error {}", discoverError);
				publish_failure(status, Failure::DiscoveryFailed, discoverError);
				return;
			}
			if (!devices.devices)
			{
				spdlog::warn("UPnP: no devices responded to discovery");
				publish_failure(status, Failure::NoDevices, 0);
				return;
			}

			IgdUrls igd;
			IGDdatas data{};

			char lanaddr[64] = {};
			char wanaddr[64] = {};

			int igdResult = UPNP_GetValidIGD(devices.devices, &igd.urls, &data,
				lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));

			// Every non-zero result fills in urls, so all of them need freeing.
			igd.valid = (igdResult != UPNP_NO_IGD);
			status.igdResult = igdResult;

			// Routers report themselves disconnected while forwarding ports perfectly well, so
			// anything that is an IGD at all gets the mappings tried against it. Whether they
			// took is settled by reading them back further down.
			if (igdResult == UPNP_NO_IGD || igdResult == UPNP_UNKNOWN_DEVICE)
			{
				spdlog::error("UPnP: UPNP_GetValidIGD returned {}, nothing here forwards ports", igdResult);
				publish_failure(status, Failure::NoIgd, igdResult);
				return;
			}

			if (igdResult != UPNP_CONNECTED_IGD)
				spdlog::warn("UPnP: router returned {} from UPNP_GetValidIGD, mapping anyway", igdResult);

			status.lanAddress = lanaddr;

			char externalAddress[40] = {};
			if (UPNP_GetExternalIPAddress(igd.urls.controlURL, data.first.servicetype,
				externalAddress) == UPNPCOMMAND_SUCCESS && !is_unspecified(externalAddress))
			{
				status.externalAddress = externalAddress;
				status.externalIsUnroutable = is_unroutable(status.externalAddress);

				if (status.externalIsUnroutable)
					spdlog::warn("UPnP: router's external address {} is itself private, this connection is behind another NAT", status.externalAddress);
			}
			else
			{
				// Left blank. Recording 0.0.0.0 would fail the reserved range check below and
				// report a second layer of NAT, which is a different problem.
				spdlog::warn("UPnP: router did not report an external address");
			}

			status.stage = Stage::Mapping;
			publish(status);

			bool anyRejected = false;
			for (int port : Ports)
			{
				const std::string portStr = std::to_string(port);

				PortMapping mapping;
				mapping.port = port;

				mapping.tcpError = UPNP_AddPortMapping(igd.urls.controlURL, data.first.servicetype,
					portStr.c_str(), portStr.c_str(), lanaddr, "OutRun2006", "TCP", nullptr, nullptr);
				mapping.tcpAdded = (mapping.tcpError == UPNPCOMMAND_SUCCESS);

				mapping.udpError = UPNP_AddPortMapping(igd.urls.controlURL, data.first.servicetype,
					portStr.c_str(), portStr.c_str(), lanaddr, "OutRun2006", "UDP", nullptr, nullptr);
				mapping.udpAdded = (mapping.udpError == UPNPCOMMAND_SUCCESS);

				if (!mapping.tcpAdded)
					spdlog::error("UPnP: AddPortMapping failed for TCP {}: {} ({})", port, error_text(mapping.tcpError), mapping.tcpError);
				if (!mapping.udpAdded)
					spdlog::error("UPnP: AddPortMapping failed for UDP {}: {} ({})", port, error_text(mapping.udpError), mapping.udpError);

				anyRejected |= !mapping.tcpAdded || !mapping.udpAdded;
				status.ports.push_back(mapping);
			}

			status.stage = Stage::Verifying;
			publish(status);

			for (PortMapping& mapping : status.ports)
			{
				const std::string portStr = std::to_string(mapping.port);
				verify_mapping(igd.urls, data, portStr.c_str(), "TCP", mapping.tcpVerified, mapping.tcpClient);
				verify_mapping(igd.urls, data, portStr.c_str(), "UDP", mapping.udpVerified, mapping.udpClient);

				// The one failure that reads as success everywhere else: the port is forwarded,
				// just not to this machine.
				if (mapping.tcpVerified && !mapping.tcpClient.empty() && mapping.tcpClient != status.lanAddress)
					spdlog::warn("UPnP: TCP {} is forwarded to {}, not to us ({})", mapping.port, mapping.tcpClient, status.lanAddress);
				if (mapping.udpVerified && !mapping.udpClient.empty() && mapping.udpClient != status.lanAddress)
					spdlog::warn("UPnP: UDP {} is forwarded to {}, not to us ({})", mapping.port, mapping.udpClient, status.lanAddress);
			}

			{
				std::lock_guard _(state().igdMutex);
				state().controlUrl = igd.urls.controlURL;
				state().serviceType = data.first.servicetype;
				state().lanAddress = lanaddr;
				state().mapped = true;
			}

			if (anyRejected)
			{
				publish_failure(status, Failure::MappingRejected, 0);
				return;
			}

			status.stage = Stage::Done;
			status.failure = Failure::None;
			status.finishedAt = std::chrono::steady_clock::now();
			publish(status);

			spdlog::info("UPnP: port mappings succeeded (lan {}, external {})",
				status.lanAddress, status.externalAddress.empty() ? "unknown" : status.externalAddress);
		}

		void worker_main()
		{
			try
			{
				attempt();
			}
			catch (const std::exception& e)
			{
				spdlog::error("UPnP: attempt threw: {}", e.what());

				// No specific failure: guessing at one would show the user a wrong reason.
				Status status;
				status.stage = Stage::Failed;
				status.finishedAt = std::chrono::steady_clock::now();
				publish(status);
			}

			state().running = false;
		}
	}

	bool Status::all_ports_open() const
	{
		if (stage != Stage::Done || ports.size() != Ports.size())
			return false;

		for (const PortMapping& mapping : ports)
		{
			if (!mapping.tcpAdded || !mapping.udpAdded || !mapping.tcpVerified || !mapping.udpVerified)
				return false;

			// Forwarded, but to somebody else on the network.
			if (!lanAddress.empty() && !mapping.tcpClient.empty() && mapping.tcpClient != lanAddress)
				return false;
			if (!lanAddress.empty() && !mapping.udpClient.empty() && mapping.udpClient != lanAddress)
				return false;
		}

		return true;
	}

	Status status()
	{
		std::lock_guard _(state().statusMutex);
		return state().current;
	}

	bool busy()
	{
		return state().running.load();
	}

	void refresh()
	{
		State& s = state();

		bool expected = false;
		if (!s.running.compare_exchange_strong(expected, true))
			return;

		try
		{
			// Whoever won the exchange above owns the pointer until their attempt ends, and a
			// previous attempt has already finished for running to have been false.
			if (s.worker)
			{
				if (s.worker->joinable())
					s.worker->join();

				delete s.worker;
				s.worker = nullptr;
			}

			s.worker = new std::thread(worker_main);
		}
		catch (const std::exception& e)
		{
			spdlog::error("UPnP: couldn't start worker thread: {}", e.what());
			s.running = false;
		}
	}

	void shutdown()
	{
		State& s = state();

		if (s.worker)
		{
			if (s.worker->joinable())
				s.worker->join();

			delete s.worker;
			s.worker = nullptr;
		}

		std::lock_guard _(s.igdMutex);
		if (!s.mapped)
			return;

		for (int port : Ports)
		{
			const std::string portStr = std::to_string(port);
			UPNP_DeletePortMapping(s.controlUrl.c_str(), s.serviceType.c_str(), portStr.c_str(), "TCP", nullptr);
			UPNP_DeletePortMapping(s.controlUrl.c_str(), s.serviceType.c_str(), portStr.c_str(), "UDP", nullptr);
		}

		s.mapped = false;
	}

	const char* stage_name(Stage stage)
	{
		switch (stage)
		{
		case Stage::NotAttempted: return "Not attempted";
		case Stage::Discovering: return "Searching for router";
		case Stage::Mapping: return "Requesting ports";
		case Stage::Verifying: return "Checking ports";
		case Stage::Done: return "Ports forwarded";
		case Stage::Failed: return "Failed";
		}
		return "Unknown";
	}

	const char* failure_text(Failure failure)
	{
		switch (failure)
		{
		case Failure::None: return "";
		case Failure::DiscoveryFailed: return "Couldn't search the network for a router.";
		case Failure::NoDevices: return "No router answered. UPnP may be turned off in its settings.";
		case Failure::NoIgd: return "Something answered, but nothing that forwards ports.";
		case Failure::MappingRejected: return "The router refused one or more ports.";
		}
		return "";
	}

	const char* error_text(int errorCode)
	{
		return strupnperror(errorCode);
	}

	const char* igd_note(int igdResult)
	{
		if (igdResult == UPNP_PRIVATEIP_IGD)
			return "The router says its own internet address is private.";
		if (igdResult == UPNP_DISCONNECTED_IGD)
			return "The router says it has no internet connection. Many say this while working fine.";

		return nullptr;
	}
}
