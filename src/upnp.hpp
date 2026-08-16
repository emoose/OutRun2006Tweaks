#pragma once

#include <array>
#include <chrono>
#include <string>
#include <vector>

// Port forwarding for the game's netcode ports, negotiated with the router over UPnP.
// Every call into miniupnpc does network I/O, so an attempt runs on a worker thread and the
// result is read back through status().
namespace UPnP
{
	// The ports Network_SocketInit binds, mapped for both TCP and UDP.
	inline constexpr std::array<int, 3> Ports = { 41455, 41456, 41457 };

	enum class Stage
	{
		// The hook that starts this hangs off DemonwareServerOverride, so an empty override
		// means no attempt was made rather than one that failed.
		NotAttempted,

		Discovering,
		Mapping,
		Verifying,
		Done,
		Failed,
	};

	enum class Failure
	{
		None,

		DiscoveryFailed,
		NoDevices,
		NoIgd,
		MappingRejected,
	};

	struct PortMapping
	{
		int port = 0;

		bool tcpAdded = false;
		bool udpAdded = false;

		int tcpError = 0;
		int udpError = 0;

		// Whether reading the mapping back found it. Some routers answer "success" and store
		// nothing.
		bool tcpVerified = false;
		bool udpVerified = false;

		// The LAN address the router has the mapping pointing at. When it isn't ours, another
		// machine on the network claimed the port first.
		std::string tcpClient;
		std::string udpClient;
	};

	struct Status
	{
		Stage stage = Stage::NotAttempted;
		Failure failure = Failure::None;

		// Result of whichever call failed. Meaning depends on failure.
		int errorCode = 0;

		// Raw UPNP_GetValidIGD result. Anything other than UPNP_CONNECTED_IGD is recorded
		// rather than acted on: some routers report themselves disconnected while forwarding ports
		// perfectly well.
		int igdResult = 0;

		std::string lanAddress;

		// Empty when the router didn't report one, which is not the same as reporting a bad one.
		std::string externalAddress;

		// The router is itself behind another NAT, so no mapping it makes can be reached.
		bool externalIsUnroutable = false;

		std::vector<PortMapping> ports;

		std::chrono::steady_clock::time_point finishedAt{};

		// Every port mapped, read back, and pointing at this machine.
		bool all_ports_open() const;
	};

	// Snapshot of the last attempt, safe to call from any thread.
	Status status();

	bool busy();

	// Starts an attempt, or does nothing if one is already running. Returns immediately.
	void refresh();

	// Removes the mappings this added and waits for any attempt still running.
	void shutdown();

	const char* stage_name(Stage stage);
	const char* failure_text(Failure failure);

	// Null when the router reported nothing worth passing on.
	const char* igd_note(int igdResult);

	const char* error_text(int errorCode);
}
