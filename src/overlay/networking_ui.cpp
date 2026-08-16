#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <string>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include <imgui.h>
#include "overlay.hpp"
#include "upnp.hpp"

// Overlay tab: whether the router is forwarding the netcode's ports, and where it thinks it is
// sending them. Reads a snapshot from UPnP, which does the talking on a worker thread.
class NetworkingWindow : public OverlayWindow
{
	static inline const ImVec4 ColourOk{ 0.4f, 0.8f, 0.4f, 1.f };
	static inline const ImVec4 ColourWarn{ 0.9f, 0.75f, 0.35f, 1.f };
	static inline const ImVec4 ColourBad{ 0.9f, 0.4f, 0.4f, 1.f };

	static std::string elapsed_text(std::chrono::steady_clock::time_point when)
	{
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::steady_clock::now() - when).count();

		if (seconds < 60)
			return std::to_string(seconds) + "s ago";
		if (seconds < 3600)
			return std::to_string(seconds / 60) + "m ago";

		return std::to_string(seconds / 3600) + "h ago";
	}

	static void hover_note(const char* text)
	{
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", text);
	}

	void draw_summary(const UPnP::Status& status, bool busy)
	{
		ImGui::SeparatorText("Port forwarding");

		ImVec4 colour = ImGui::GetStyle().Colors[ImGuiCol_Text];
		if (!busy)
		{
			if (status.stage == UPnP::Stage::Done)
				colour = status.all_ports_open() ? ColourOk : ColourWarn;
			else if (status.stage == UPnP::Stage::Failed)
				colour = ColourBad;
		}

		ImGui::TextColored(colour, "%s", UPnP::stage_name(status.stage));

		if (!busy && status.finishedAt.time_since_epoch().count() != 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("checked %s", elapsed_text(status.finishedAt).c_str());
		}

		ImGui::BeginDisabled(busy);
		if (ImGui::Button(status.stage == UPnP::Stage::NotAttempted ? "Check now" : "Re-check"))
			UPnP::refresh();
		ImGui::EndDisabled();
		hover_note("Ask the router again, then read the mappings back.");

		if (status.stage == UPnP::Stage::Failed)
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", UPnP::failure_text(status.failure));
		}
	}

	void draw_not_attempted()
	{
		ImGui::Spacing();

		if (Settings::DemonwareServerOverride.to_string().empty())
			ImGui::TextWrapped("DemonwareServerOverride is empty, so this won't run on its own.");
	}

	void draw_router(const UPnP::Status& status)
	{
		ImGui::SeparatorText("Router");

		ImGui::Text("This PC");
		ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
		ImGui::TextDisabled("%s", status.lanAddress.empty() ? "unknown" : status.lanAddress.c_str());

		ImGui::Text("Router's internet address");
		ImGui::SameLine(ImGui::GetFontSize() * 10.0f);
		ImGui::TextDisabled("%s", status.externalAddress.empty() ? "unknown" : status.externalAddress.c_str());

		// What the router claims about itself. The port rows are measured, so they win.
		if (const char* note = UPnP::igd_note(status.igdResult))
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", note);
		}

		if (!status.externalIsUnroutable)
			return;

		ImGui::Spacing();
		ImGui::TextColored(ColourWarn, "Behind a second router");
		ImGui::TextWrapped(
			"%s is a private address, so this connection goes through another router or the "
			"provider shares one address between customers. Joining others works, hosting won't.",
			status.externalAddress.c_str());
	}

	void draw_port_state(bool added, int error, bool verified, const std::string& client, const std::string& lanAddress)
	{
		if (!added)
		{
			// eg. 718 is ConflictInMappingEntry, more use than the number on its own.
			ImGui::TextColored(ColourBad, "Refused (%d)", error);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", UPnP::error_text(error));
			return;
		}

		if (!verified)
		{
			ImGui::TextColored(ColourWarn, "Not stored");
			hover_note("The router accepted it, then kept no record of it.");
			return;
		}

		if (!client.empty() && !lanAddress.empty() && client != lanAddress)
		{
			ImGui::TextColored(ColourWarn, "Sent to %s", client.c_str());
			hover_note("Forwarded to another machine, so traffic won't reach this PC.");
			return;
		}

		ImGui::TextColored(ColourOk, "Open");
	}

	void draw_ports(const UPnP::Status& status)
	{
		if (status.ports.empty())
			return;

		ImGui::SeparatorText("Ports");

		if (ImGui::BeginTable("##ports", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Port");
			ImGui::TableSetupColumn("TCP");
			ImGui::TableSetupColumn("UDP");
			ImGui::TableHeadersRow();

			for (const UPnP::PortMapping& mapping : status.ports)
			{
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::Text("%d", mapping.port);

				ImGui::TableNextColumn();
				draw_port_state(mapping.tcpAdded, mapping.tcpError, mapping.tcpVerified, mapping.tcpClient, status.lanAddress);

				ImGui::TableNextColumn();
				draw_port_state(mapping.udpAdded, mapping.udpError, mapping.udpVerified, mapping.udpClient, status.lanAddress);
			}

			ImGui::EndTable();
		}

		bool wrongMachine = false;
		for (const UPnP::PortMapping& mapping : status.ports)
		{
			wrongMachine |= mapping.tcpVerified && !mapping.tcpClient.empty() && mapping.tcpClient != status.lanAddress;
			wrongMachine |= mapping.udpVerified && !mapping.udpClient.empty() && mapping.udpClient != status.lanAddress;
		}

		if (wrongMachine)
		{
			ImGui::Spacing();
			ImGui::TextColored(ColourWarn, "Some ports point at another device");
			ImGui::TextWrapped(
				"Another machine claimed them first. Clear those rules in the router's settings, "
				"then Re-check.");
		}
	}

public:
	Kind kind() const override { return Kind::Tab; }
	const char* name() const override { return "Networking"; }
	int order() const override { return 20; }

	void init() override {}

	void render(bool overlayEnabled) override
	{
		const UPnP::Status status = UPnP::status();
		const bool busy = UPnP::busy();

		draw_summary(status, busy);

		if (status.stage == UPnP::Stage::NotAttempted)
		{
			if (!busy)
				draw_not_attempted();
			return;
		}

		// Nothing to show until discovery has found something.
		if (!status.lanAddress.empty() || !status.externalAddress.empty())
		{
			ImGui::Spacing();
			draw_router(status);
		}

		ImGui::Spacing();
		draw_ports(status);
	}

	static NetworkingWindow instance;
};
NetworkingWindow NetworkingWindow::instance;
