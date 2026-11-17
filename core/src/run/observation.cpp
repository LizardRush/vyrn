#include "run/observation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vyrn::run {

bool read_obs(const ipc::Frame& frame, const char* key, ObsView& out) {
  auto it = frame.j.find(key);
  if (it == frame.j.end() || it->is_null()) return false;
  const ipc::ArrayView a = frame.array(*it);
  if (a.dtype == "u8" && a.shape.size() == 3 && a.shape[2] == 3 && a.shape[0] >= 1 && a.shape[1] >= 1) {
    out.picture = true;
    out.h = static_cast<int>(a.shape[0]);
    out.w = static_cast<int>(a.shape[1]);
    out.rgb = a.data;
    out.numbers.clear();
    return true;
  }
  if (a.dtype == "f32" && a.shape.size() == 1 && a.shape[0] >= 1) {
    out.picture = false;
    out.h = out.w = 0;
    out.rgb = nullptr;
    out.numbers.resize(static_cast<size_t>(a.shape[0]));
    std::memcpy(out.numbers.data(), a.data, out.numbers.size() * sizeof(float));   // little-endian host
    return true;
  }
  throw ipc::ProtocolError(std::string("\"") + key + "\" is neither a picture [H,W,3] u8 nor numbers [D] f32");
}

Picture picture_from_chw(const uint8_t* chw, int h, int w) {
  Picture p;
  p.w = w;
  p.h = h;
  const size_t plane = static_cast<size_t>(h) * static_cast<size_t>(w);
  p.rgb.resize(plane * 3);
  for (size_t i = 0; i < plane; ++i) {
    p.rgb[i * 3] = chw[i];
    p.rgb[i * 3 + 1] = chw[plane + i];
    p.rgb[i * 3 + 2] = chw[2 * plane + i];
  }
  return p;
}

Picture picture_from_rgb(const uint8_t* rgb, int h, int w) {
  Picture p;
  p.w = w;
  p.h = h;
  p.rgb.assign(rgb, rgb + static_cast<size_t>(h) * static_cast<size_t>(w) * 3);
  return p;
}

std::string short_number(double v) {
  if (!std::isfinite(v)) return std::isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf");
  if (v == std::floor(v) && std::fabs(v) < 1e9) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f", v == 0 ? 0.0 : v);
    return text;
  }
  char text[32];
  std::snprintf(text, sizeof text, "%.4g", v);
  return text;
}

void ShowValues::update(const ipc::Frame& frame) {
  auto show = frame.j.find("show");
  if (show == frame.j.end() || !show->is_object()) return;
  for (auto it = show->begin(); it != show->end(); ++it) {
    const std::string& name = it.key();
    const auto& v = it.value();
    std::string text;
    bool is_text = true;
    if (v.is_number()) {
      text = short_number(v.get<double>());
    } else if (v.is_string()) {
      text = v.get<std::string>();
    } else if (v.is_boolean()) {
      text = v.get<bool>() ? "True" : "False";
    } else if (v.is_object() && v.contains("$a")) {
      is_text = false;
      try {
        const ipc::ArrayView a = frame.array(v);
        if (a.dtype != "u8" || a.shape.size() != 3 || a.shape[2] != 3) continue;
        Picture p = picture_from_rgb(a.data, static_cast<int>(a.shape[0]), static_cast<int>(a.shape[1]));
        auto found = std::find_if(pictures_.begin(), pictures_.end(), [&](const auto& e) { return e.first == name; });
        if (found != pictures_.end()) {
          found->second = std::move(p);
        } else {
          pictures_.emplace_back(name, std::move(p));
        }
      } catch (const ipc::ProtocolError&) {
      }
    } else {
      continue;
    }
    if (!is_text) continue;
    auto found = std::find_if(text_.begin(), text_.end(), [&](const auto& e) { return e.first == name; });
    if (found != text_.end()) {
      found->second = std::move(text);
    } else {
      text_.emplace_back(name, std::move(text));
    }
  }
}

void ShowValues::fill(LiveView& view) const {
  view.show_text = text_;
  view.show_pictures = pictures_;
}

}  // namespace vyrn::run
