// A project's Vyrn Link pairing code and this computer's network addresses (vyrn/link.h).
#include "vyrn/link.h"

#include "link/link_server.h"
#include "platform/files.h"
#include "platform/net.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <tuple>

namespace vyrn {

namespace {

namespace fs = std::filesystem;

bool valid_code(const std::string& code) {
  return code.size() == 6 && code[0] >= '1' && code[0] <= '9' &&
         std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The code in the file, if it holds one (a plain file, not a symlink: "483920" with optional white space).
std::optional<std::string> read_code(const fs::path& file) {
  int err = 0;
  platform::File in = platform::File::open_read(file.string(), /*no_follow=*/true, err);
  if (!in) return std::nullopt;
  platform::FileInfo st;
  char buf[64];
  size_t n = 0;
  if (in.info(st) == 0 && st.is_regular() && st.size < sizeof buf && in.read(buf, sizeof buf, n) != 0) n = 0;
  in.close();
  if (n == 0) return std::nullopt;
  std::string text(buf, n);
  text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }),
             text.end());
  if (!valid_code(text)) return std::nullopt;
  return text;
}

// Writes the code next to `file` and moves it into place: replace = an atomic rename over what is there; otherwise
// only when there is no file yet (EEXIST then). Returns 0 or the errno.
int write_code(const fs::path& file, const std::string& code, bool replace) {
  std::string tmp;
  int err = 0;
  // Hidden too, like the code file; mode 0600: only the owner can read the code.
  platform::File out = platform::File::create_unique(file.string() + ".tmp-", tmp, err);
  if (!out) return err;
  const std::string text = code + "\n";
  err = out.write_all(text.data(), text.size());
  if (const int closed = out.close(); err == 0) err = closed;
  if (err != 0) {
    platform::remove_file(tmp);
    return err;
  }
  err = replace ? platform::rename_replace(tmp, file.string())
                : platform::rename_no_replace(tmp, file.string(), /*directory=*/false);
  if (err != 0) platform::remove_file(tmp);
  return err;
}

bool starts_with(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// Interfaces of containers, VPNs and virtual machines: a game on the home network can't use their addresses.
bool virtual_interface(const std::string& name) {
  static const char* const kVirtual[] = {"docker", "br-",  "veth", "virbr", "lxc", "lxd", "podman", "cni",
                                         "flannel", "kube", "tailscale", "tun", "tap", "wg", "zt", "ham"};
  return std::any_of(std::begin(kVirtual), std::end(kVirtual), [&](const char* p) { return starts_with(name, p); });
}

// 0: 192.168.x.x, 1: 10.x.x.x, 2: 172.16-31.x.x, 3: anything else, 4: 100.64.0.0/10 (carrier NAT, VPNs),
// 5: 169.254.x.x (no network yet).
int address_rank(uint32_t a) {
  if ((a >> 16) == 0xC0A8) return 0;
  if ((a >> 24) == 10) return 1;
  if ((a >> 20) == 0xAC1) return 2;
  if ((a >> 22) == (0x6440u >> 6)) return 4;
  if ((a >> 16) == 0xA9FE) return 5;
  return 3;
}

}  // namespace

std::string link_code(const std::string& project_dir, bool* saved) {
  if (saved) *saved = true;
  const fs::path file = fs::path(project_dir) / kLinkCodeFile;
  if (auto code = read_code(file)) return *code;
  std::error_code ec;
  const bool something_there = fs::symlink_status(file, ec).type() != fs::file_type::not_found && !ec;
  const std::string code = link::LinkServer::make_code();
  const int err = write_code(file, code, something_there);   // a broken file is replaced
  if (err == EEXIST) {                                       // another program made one just now: use it
    if (auto theirs = read_code(file)) return *theirs;
  }
  if (err != 0 && saved) *saved = false;
  return code;
}

std::string new_link_code(const std::string& project_dir, std::string* why) {
  const fs::path file = fs::path(project_dir) / kLinkCodeFile;
  std::string code = link::LinkServer::make_code();
  if (auto old = read_code(file)) {
    while (code == *old) code = link::LinkServer::make_code();
  }
  if (const int err = write_code(file, code, true); err != 0) {
    if (why) *why = "Vyrn couldn't save the new pairing code in " + file.string() + ": " + platform::error_text(err) + ".";
    return "";
  }
  return code;
}

std::vector<std::string> lan_addresses() {
  std::vector<std::tuple<bool, int, std::string, std::string>> found;   // (virtual, rank, interface, address)
  for (const platform::net::Ipv4Interface& i : platform::net::ipv4_interfaces()) {
    const uint32_t host = i.address;
    if ((host >> 24) == 127 || host == 0) continue;
    found.emplace_back(virtual_interface(i.name), address_rank(host), i.name, platform::net::ipv4_text(host));
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  for (const auto& f : found) {
    const std::string& address = std::get<3>(f);
    if (std::find(out.begin(), out.end(), address) == out.end()) out.push_back(address);
  }
  return out;
}

std::string link_url(int port) {
  const std::vector<std::string> addresses = lan_addresses();
  return "ws://" + (addresses.empty() ? std::string("127.0.0.1") : addresses.front()) + ":" + std::to_string(port) +
         "/";
}

}  // namespace vyrn
