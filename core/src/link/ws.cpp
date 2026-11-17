#include "link/ws.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace vyrn::link::ws {

namespace {

constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// True when a comma-separated header value contains token (case-insensitive), e.g. "keep-alive, Upgrade".
bool has_token(std::string_view value, std::string_view token) {
  const std::string wanted = lower(token);
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    if (end == std::string_view::npos) end = value.size();
    if (lower(trim(value.substr(start, end - start))) == wanted) return true;
    start = end + 1;
  }
  return false;
}

bool is_base64_key(std::string_view key) {
  // 16 random bytes in base64: 22 characters and "==".
  if (key.size() != 24 || key.substr(22) != "==") return false;
  return std::all_of(key.begin(), key.begin() + 22, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/';
  });
}

std::string http_response(const std::string& status, const std::string& extra_headers, const std::string& body) {
  return "HTTP/1.1 " + status + "\r\n" + extra_headers + "Content-Type: text/plain; charset=utf-8\r\n" +
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

std::string sha1(std::string_view data) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string msg(data);
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back('\0');
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + 4 * i);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  std::string out;
  for (uint32_t v : h) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((v >> s) & 0xFF));
  }
  return out;
}

std::string base64(std::string_view data) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) |
                       uint32_t(uint8_t(data[i + 2]));
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i < data.size()) {
    uint32_t v = uint32_t(uint8_t(data[i])) << 16;
    if (i + 1 < data.size()) v |= uint32_t(uint8_t(data[i + 1])) << 8;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::string accept_key(std::string_view client_key) { return base64(sha1(std::string(client_key) + kGuid)); }

// ---- HTTP -------------------------------------------------------------------------------------------------------

std::string HttpRequest::header(std::string_view lowercase_name) const {
  std::string value;
  for (const auto& [name, v] : headers) {
    if (name != lowercase_name) continue;
    if (!value.empty()) value += ", ";
    value += v;
  }
  return value;
}

size_t http_head_size(std::string_view data) {
  const size_t end = data.find("\r\n\r\n");
  return end == std::string_view::npos ? 0 : end + 4;
}

std::optional<HttpRequest> parse_http_request(std::string_view head) {
  HttpRequest req;
  size_t pos = 0;
  bool first = true;
  while (pos < head.size()) {
    size_t end = head.find("\r\n", pos);
    if (end == std::string_view::npos) end = head.size();
    const std::string_view line = head.substr(pos, end - pos);
    pos = end + 2;
    if (first) {
      first = false;
      const size_t a = line.find(' ');
      const size_t b = a == std::string_view::npos ? a : line.find(' ', a + 1);
      if (a == std::string_view::npos || b == std::string_view::npos) return std::nullopt;
      req.method = std::string(line.substr(0, a));
      req.target = std::string(line.substr(a + 1, b - a - 1));
      req.version = std::string(line.substr(b + 1));
      if (req.version.rfind("HTTP/", 0) != 0) return std::nullopt;
      continue;
    }
    if (line.empty()) break;
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return std::nullopt;
    req.headers.emplace_back(lower(trim(line.substr(0, colon))), std::string(trim(line.substr(colon + 1))));
  }
  if (first) return std::nullopt;
  return req;
}

HandshakeAnswer answer_upgrade(const HttpRequest& req) {
  HandshakeAnswer a;
  const std::string help =
      "This is Vyrn Link. Games and mods connect here with a WebSocket, for example ws://<this computer>:<port>/ "
      "(docs/user/vyrn-link.md).\n";
  if (req.method != "GET" || req.version != "HTTP/1.1") {
    a.problem = "not a WebSocket request (" + req.method + " " + req.version + ")";
    a.response = http_response("400 Bad Request", "", help);
    return a;
  }
  if (!has_token(req.header("upgrade"), "websocket") || !has_token(req.header("connection"), "upgrade")) {
    a.problem = "an HTTP request without a WebSocket upgrade (a web browser?)";
    a.response = http_response("426 Upgrade Required", "Upgrade: websocket\r\nConnection: Upgrade\r\n", help);
    return a;
  }
  if (trim(req.header("sec-websocket-version")) != "13") {
    a.problem = "an unsupported WebSocket version (Vyrn speaks version 13)";
    a.response = http_response("426 Upgrade Required", "Sec-WebSocket-Version: 13\r\n", help);
    return a;
  }
  const std::string key(trim(req.header("sec-websocket-key")));
  if (!is_base64_key(key)) {
    a.problem = "a WebSocket request without a valid Sec-WebSocket-Key";
    a.response = http_response("400 Bad Request", "", help);
    return a;
  }
  a.upgrade = true;
  a.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
               accept_key(key) + "\r\n\r\n";
  return a;
}

// ---- frames -----------------------------------------------------------------------------------------------------

std::string encode_frame(uint8_t opcode, std::string_view payload, bool fin, const uint8_t* mask) {
  std::string out;
  out.reserve(payload.size() + 14);
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));
  const uint8_t mask_bit = mask ? 0x80 : 0x00;
  const size_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<char>(mask_bit | n));
  } else if (n < 65536) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int s = 56; s >= 0; s -= 8) out.push_back(static_cast<char>((static_cast<uint64_t>(n) >> s) & 0xFF));
  }
  if (!mask) {
    out.append(payload.data(), payload.size());
    return out;
  }
  out.append(reinterpret_cast<const char*>(mask), 4);
  for (size_t i = 0; i < n; ++i) out.push_back(static_cast<char>(payload[i] ^ static_cast<char>(mask[i % 4])));
  return out;
}

std::string close_payload(uint16_t code, std::string_view reason) {
  std::string out;
  out.push_back(static_cast<char>(code >> 8));
  out.push_back(static_cast<char>(code & 0xFF));
  size_t n = std::min(reason.size(), kMaxControlPayload - 2);
  while (n > 0 && n < reason.size() && (static_cast<uint8_t>(reason[n]) & 0xC0) == 0x80) --n;   // whole characters
  out.append(reason.data(), n);
  return out;
}

std::optional<Frame> take_frame(const std::string& buf, size_t& pos, size_t max_data_payload, bool from_client) {
  const size_t avail = buf.size() - pos;
  if (avail < 2) return std::nullopt;
  const auto* p = reinterpret_cast<const uint8_t*>(buf.data() + pos);
  Frame f;
  f.fin = (p[0] & 0x80) != 0;
  f.opcode = p[0] & 0x0F;
  if (p[0] & 0x70) throw WsError(kCloseProtocolError, "a frame used reserved bits (Vyrn uses no WebSocket extensions)");
  const bool control = f.opcode >= 0x8;
  if (f.opcode != kContinuation && f.opcode != kText && f.opcode != kBinary && f.opcode != kClose &&
      f.opcode != kPing && f.opcode != kPong) {
    throw WsError(kCloseProtocolError, "a frame has the unknown type " + std::to_string(f.opcode));
  }
  const bool masked = (p[1] & 0x80) != 0;
  if (from_client && !masked) {
    throw WsError(kCloseProtocolError, "a frame from the game wasn't masked (RFC 6455 section 5.1: clients mask every frame)");
  }
  if (!from_client && masked) throw WsError(kCloseProtocolError, "a frame from the server was masked");
  uint64_t len = p[1] & 0x7F;
  size_t header = 2;
  if (len == 126) {
    if (avail < 4) return std::nullopt;
    len = (uint64_t(p[2]) << 8) | p[3];
    header = 4;
  } else if (len == 127) {
    if (avail < 10) return std::nullopt;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    if (len >> 63) throw WsError(kCloseProtocolError, "a frame has an impossible length");
    header = 10;
  }
  if (control && (!f.fin || len > kMaxControlPayload)) {
    throw WsError(kCloseProtocolError, "a control frame (close, ping or pong) was fragmented or longer than 125 bytes");
  }
  if (!control && len > max_data_payload) {
    throw WsError(kCloseTooBig, "a message is larger than the limit of 1 MiB");
  }
  const size_t mask_bytes = masked ? 4 : 0;
  if (avail < header + mask_bytes + len) return std::nullopt;
  const uint8_t* mask = p + header;
  const char* data = buf.data() + pos + header + mask_bytes;
  f.payload.assign(data, static_cast<size_t>(len));
  if (masked) {
    for (size_t i = 0; i < f.payload.size(); ++i) f.payload[i] = static_cast<char>(f.payload[i] ^ static_cast<char>(mask[i % 4]));
  }
  pos += header + mask_bytes + static_cast<size_t>(len);
  return f;
}

bool valid_utf8(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const auto c = static_cast<uint8_t>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
    i += len;
  }
  return true;
}

}  // namespace vyrn::link::ws
