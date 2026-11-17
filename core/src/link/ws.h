// The RFC 6455 pieces Vyrn Link needs (docs/user/vyrn-link.md): the HTTP upgrade, frames, masking, UTF-8.
//
// Plain functions without sockets, so the server (link_server.cpp) and the tests share them. Text messages only:
// Vyrn negotiates no extensions and no subprotocol.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vyrn::link::ws {

enum Opcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// Close status codes (RFC 6455 section 7.4.1).
inline constexpr uint16_t kCloseNormal = 1000;
inline constexpr uint16_t kCloseGoingAway = 1001;
inline constexpr uint16_t kCloseProtocolError = 1002;
inline constexpr uint16_t kCloseUnsupportedData = 1003;
inline constexpr uint16_t kCloseBadData = 1007;
inline constexpr uint16_t kClosePolicy = 1008;
inline constexpr uint16_t kCloseTooBig = 1009;

inline constexpr size_t kMaxControlPayload = 125;
inline constexpr size_t kMaxHttpHead = 16 * 1024;

// A frame that breaks RFC 6455 (or Vyrn's size limit). close_code is what to close the connection with.
struct WsError : std::runtime_error {
  WsError(uint16_t code, const std::string& message) : std::runtime_error(message), close_code(code) {}
  uint16_t close_code;
};

std::string sha1(std::string_view data);            // 20 raw bytes
std::string base64(std::string_view data);           // standard alphabet, with padding
std::string accept_key(std::string_view client_key); // the Sec-WebSocket-Accept value for a Sec-WebSocket-Key

// ---- the HTTP upgrade -------------------------------------------------------------------------------------------

struct HttpRequest {
  std::string method, target, version;
  std::vector<std::pair<std::string, std::string>> headers;   // names lowercased, values trimmed
  // The header's value ("" when missing); repeated headers are joined with ", ".
  std::string header(std::string_view lowercase_name) const;
};

// Size of the HTTP head at the start of data, including the blank line; 0 while it isn't complete.
size_t http_head_size(std::string_view data);
// nullopt when the head isn't an HTTP request at all.
std::optional<HttpRequest> parse_http_request(std::string_view head);

struct HandshakeAnswer {
  bool upgrade = false;   // true: the response is "101 Switching Protocols" and the connection is a WebSocket
  std::string response;   // the whole HTTP response to send
  std::string problem;    // why it was refused (plain English, for the log)
};
// The answer to an upgrade request: 101 with Sec-WebSocket-Accept, or an HTTP error with a short text body
// (426 for a browser that opened the address, 400 for a broken request).
HandshakeAnswer answer_upgrade(const HttpRequest& request);

// ---- frames -----------------------------------------------------------------------------------------------------

struct Frame {
  bool fin = true;
  uint8_t opcode = kText;
  std::string payload;   // unmasked
};

// One frame. A server sends without a mask; a client passes its 4-byte mask (RFC 6455 section 5.3).
std::string encode_frame(uint8_t opcode, std::string_view payload, bool fin = true, const uint8_t* mask = nullptr);
// The payload of a close frame: the status code (big-endian), then a UTF-8 reason cut to fit in 125 bytes.
std::string close_payload(uint16_t code, std::string_view reason = {});

// Reads the frame that starts at buf[pos] if it is complete: returns it and moves pos past it, or returns nullopt
// (pos unchanged) while bytes are missing. from_client: frames must be masked (a server reading a client);
// otherwise they must not be. A data frame whose payload is longer than max_data_payload is refused as soon as
// its header arrives. Throws WsError for anything RFC 6455 forbids.
std::optional<Frame> take_frame(const std::string& buf, size_t& pos, size_t max_data_payload, bool from_client);

// True when text is valid UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool valid_utf8(std::string_view text);

}  // namespace vyrn::link::ws
