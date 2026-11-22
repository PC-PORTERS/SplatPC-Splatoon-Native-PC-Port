#include "gx2_online_menu.h"

#include "../../../runtime/netplay.h"
#include "../../../runtime/netplay_connect.h"
#include "../../../runtime/port_settings.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gx2_online_menu {
namespace {
namespace rs = rbrew_settings;
namespace net = rbrew_net;
using RouterState = net::RouterStatus::State;

const ImVec4 kGood(0.35f, 0.95f, 0.45f, 1.0f), kWait(0.95f, 0.85f, 0.35f, 1.0f), kBad(0.95f, 0.4f, 0.4f, 1.0f);

char s_join[256] = "";
bool s_join_loaded = false;
bool s_confirm_join = false;
std::string s_message;
std::string s_local_ip;
double s_local_ip_checked = -1.0e9;

void help(const char* text) {
    ImGui::SetItemTooltip("%s", text);
}

void address_row(const char* label, const std::string& address, const char* id) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextColored(kGood, "%s", address.c_str());
    ImGui::SameLine();
    ImGui::PushID(id);
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(address.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy --join")) ImGui::SetClipboardText(("Splatoon.exe --join " + address).c_str());
    ImGui::PopID();
}

void router_line(const net::RouterStatus& r, uint16_t port) {
    switch (r.state) {
    case RouterState::Off:
        ImGui::TextDisabled("Automatic port forwarding is off.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open the port now")) net::router_open(port);
        break;
    case RouterState::Working:
        ImGui::TextColored(kWait, "Asking your router to open port %u ...", unsigned(port));
        break;
    case RouterState::Open: ImGui::TextColored(kGood, "Port %u is open on your router.", unsigned(port)); break;
    case RouterState::Failed:
        ImGui::TextColored(kBad, "Automatic port forwarding failed:");
        ImGui::TextWrapped("%s Forward the port by hand (see below).", r.error.c_str());
        if (ImGui::SmallButton("Try again")) net::router_open(port);
        break;
    }
}

void build_addresses(uint16_t port, const net::RouterStatus& r) {
    if (ImGui::GetTime() - s_local_ip_checked > 10.0) {
        s_local_ip = net::local_ip();
        s_local_ip_checked = ImGui::GetTime();
    }
    const std::string suffix = ":" + std::to_string(port);
    const std::string looked_up = net::public_ip();
    // Prefer a public address: behind a second router the router's own one is private.
    const bool router_public = !r.external_ip.empty() && !net::is_private_ipv4(r.external_ip);
    const bool looked_public = !looked_up.empty() && !net::is_private_ipv4(looked_up);
    const std::string internet = router_public            ? r.external_ip
                                 : looked_public          ? looked_up
                                 : !r.external_ip.empty() ? r.external_ip
                                                          : looked_up;
    if (!internet.empty() && !net::is_private_ipv4(internet)) {
        address_row("Friends on the internet join:", internet + suffix, "internet");
    } else if (!internet.empty()) {
        ImGui::TextColored(kBad, "Your router's internet address %s is not reachable from the internet.",
                           internet.c_str());
        ImGui::TextWrapped("Your router sits behind another router, or your provider shares one address between "
                           "customers (carrier-grade NAT). Port forwarding cannot work then. Use a virtual LAN such "
                           "as Tailscale, ZeroTier or Radmin VPN on both PCs and give your friend that address.");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Friends on the internet join:");
        ImGui::SameLine();
        if (net::public_ip_busy())
            ImGui::TextDisabled("looking it up ...");
        else if (ImGui::SmallButton("Find my internet address"))
            net::public_ip_lookup();
        help("Asks api.ipify.org which address your internet connection has.");
    }
    if (!r.external_ip.empty() && !looked_up.empty() && r.external_ip != looked_up)
        ImGui::TextWrapped("Note: your router reports %s, but the internet sees %s. A second router is in between; "
                           "forward the port on that one too.",
                           r.external_ip.c_str(), looked_up.c_str());
    if (!s_local_ip.empty()) address_row("Same home network:", s_local_ip + suffix, "lan");
}

void build_manual_help(uint16_t port, const net::RouterStatus& r) {
    if (!ImGui::CollapsingHeader("How to forward the port by hand")) return;
    const std::string router = r.router.empty() ? "192.168.0.1 or 192.168.1.1" : r.router;
    const std::string pc = s_local_ip.empty() ? "this PC" : s_local_ip;
    ImGui::TextWrapped("1. Open your router's page in a browser: %s (the address is often printed on the router).",
                       router.c_str());
    if (!r.router.empty()) {
        if (ImGui::SmallButton("Open router page")) net::open_url("http://" + r.router);
    }
    ImGui::TextWrapped("2. Find \"Port forwarding\" (also called \"Virtual server\", \"NAT\" or \"Port sharing\").");
    ImGui::TextWrapped("3. Forward TCP port %u to %s (internal and external port %u).", unsigned(port), pc.c_str(),
                       unsigned(port));
    ImGui::TextWrapped("4. When Windows asks about the firewall, allow Splatoon on private and public networks.");
    ImGui::TextWrapped("5. Send your friend your internet address with the port, for example 203.0.113.7:%u.",
                       unsigned(port));
}

void build_host(rs::Values& v, bool& dirty) {
    ImGui::SeparatorText("Host: a friend joins your game");
    const uint16_t port = net::host_port();
    if (!port) {
        ImGui::SetNextItemWidth(120);
        int chosen = v.net_port;
        if (ImGui::InputInt("Port", &chosen, 0, 0)) {
            v.net_port = std::clamp(chosen, 1024, 65535);
            dirty = true;
        }
        help("TCP port your friend connects to (default 52815).");
        dirty |= ImGui::Checkbox("Open the port on my router automatically (UPnP)", &v.net_upnp);
        help("Most home routers allow this. If yours does not, the page explains how to do it by hand.");
        if (ImGui::Button("Start hosting")) {
            if (net::host_start(uint16_t(v.net_port))) {
                s_message.clear();
                if (v.net_upnp) net::router_open(uint16_t(v.net_port));
            } else {
                s_message = "Port " + std::to_string(v.net_port) + " is already in use. Choose another port.";
            }
        }
        ImGui::TextDisabled("Your friend plays as Player 2 with a controller or keyboard and mouse.");
        return;
    }
    if (net::host_guest_connected())
        ImGui::TextColored(kGood, "Your friend is connected and plays as Player 2.");
    else
        ImGui::TextColored(kWait, "Waiting for your friend on port %u ...", unsigned(port));
    ImGui::SameLine();
    if (ImGui::Button("Stop hosting")) {
        net::host_stop();
        net::router_close();
        return;
    }
    const net::RouterStatus r = net::router_status();
    router_line(r, port);
    build_addresses(port, r);
    build_manual_help(port, r);
    ImGui::TextDisabled("In Battle Dojo your friend sees the TV picture; Tab shows you the GamePad screen.");
}

Result build_join(rs::Values& v, bool& dirty) {
    ImGui::SeparatorText("Join: play in a friend's game");
    if (!s_join_loaded) {
        std::snprintf(s_join, sizeof(s_join), "%s", v.net_join.c_str());
        s_join_loaded = true;
    }
    ImGui::TextWrapped("Enter the address your friend sent you. Your game closes and your friend's game opens "
                       "in a window.");
    ImGui::SetNextItemWidth(260);
    const bool entered = ImGui::InputTextWithHint("##join", "address or address:port", s_join, sizeof(s_join),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Join") || entered) {
        std::string clean;
        if (net::host_port())
            s_message = "Stop hosting first.";
        else if (!net::clean_address(s_join, clean))
            s_message = "That is not an address. It looks like 203.0.113.7 or 203.0.113.7:52815.";
        else {
            s_message.clear();
            std::snprintf(s_join, sizeof(s_join), "%s", clean.c_str());
            if (v.net_join != clean) {
                v.net_join = clean;
                dirty = true;
            }
            s_confirm_join = true;
        }
    }
    if (!s_confirm_join) return Result::Stay;
    ImGui::TextColored(kWait, "Your game closes now. Progress since the last save is lost.");
    if (ImGui::Button("Close my game and join")) {
        s_confirm_join = false;
        if (net::launch_guest(s_join)) return Result::Quit;
        s_message = "The game window for joining could not be started.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) s_confirm_join = false;
    return Result::Stay;
}

void build_story(rs::Values& v, bool& dirty) {
    ImGui::SeparatorText("Story Mode co-op");
    dirty |= ImGui::Checkbox("Allow Player 2 in Octo Valley levels", &v.coop_story);
    ImGui::TextDisabled("Activates automatically on level entry when a second player is connected.");
}
} // namespace

Result build_page() {
    auto v = rs::get();
    bool dirty = false;
    build_host(v, dirty);
    const Result joined = build_join(v, dirty);
    build_story(v, dirty);
    if (ImGui::CollapsingHeader("Stream")) {
        dirty |= ImGui::SliderInt("Video quality", &v.net_kbps, 2000, 30000, "%d kbit/s");
        help("Picture quality sent to your friend. Lower it when their picture stutters; applies when they "
             "(re)connect.");
    }
    if (!s_message.empty()) ImGui::TextColored(kBad, "%s", s_message.c_str());
    ImGui::Separator();
    const bool back = ImGui::Button("Back");
    if (dirty) rs::set(v);
    if (joined == Result::Quit) return Result::Quit;
    return back ? Result::Back : Result::Stay;
}

const char* status_line() {
    if (!net::host_port()) return "";
    return net::host_guest_connected() ? "friend connected" : "hosting";
}

} // namespace gx2_online_menu
