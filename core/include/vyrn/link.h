// Vyrn Link for the app and vyrn-cli: a project's pairing code and the address a game types in
// (docs/dev/link.md; the protocol for mod authors is docs/user/vyrn-link.md).
//
// Standard library only: the Qt app and vyrn-cli include this header. Implemented in core/src/link/ (library
// vyrn_link). The server itself is internal (core/src/link/link_server.h): a Session starts it for projects whose
// vyrn.json has "link".
#pragma once

#include <string>
#include <vector>

namespace vyrn {

// The pairing code lives in <project>/.vyrn-link-code (6 digits and a newline, readable only by its owner). Hidden
// files are never shared in .vrlp files, so the code stays on this computer; vyrn.json, which is shared, never
// holds it.
inline constexpr const char* kLinkCodeFile = ".vyrn-link-code";

// The project's pairing code (6 digits, never a leading 0). A missing or broken file gets a new code. Never
// throws: when the file can't be written, *saved is false and the code is only good for this run.
std::string link_code(const std::string& project_dir, bool* saved = nullptr);

// Makes a new pairing code for the project and saves it. Returns "" with a plain-English *why when it can't be
// saved. A run that is going on keeps the code it started with.
std::string new_link_code(const std::string& project_dir, std::string* why = nullptr);

// This computer's IPv4 addresses that a game on another computer can use, best first: home and office networks
// (192.168.x.x, 10.x.x.x, 172.16-31.x.x on Wi-Fi or Ethernet) before VPNs and containers. Never 127.0.0.1; empty
// when this computer isn't on a network.
std::vector<std::string> lan_addresses();

// "ws://192.168.1.20:7457/": what to type into the game's mod (the best LAN address; 127.0.0.1 without one).
std::string link_url(int port);

}  // namespace vyrn
