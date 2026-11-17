// Reading what the worker sent in a result: observations and vyrn.show() values. No torch in this header.
#pragma once

#include "ipc/proto.h"
#include "vyrn/session.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vyrn::run {

// One observation: a picture (uint8 [h, w, 3]) or numbers (float32 [d]). `rgb` points into the frame it came
// from, so an ObsView is valid only while that frame lives.
struct ObsView {
  bool picture = false;
  int h = 0, w = 0;
  const uint8_t* rgb = nullptr;
  std::vector<float> numbers;
};

// Reads frame.j[key]. Returns false when it is null or missing; throws ipc::ProtocolError when it isn't a
// picture or numbers array.
bool read_obs(const ipc::Frame& frame, const char* key, ObsView& out);

// A frame (uint8 [3, h, w], channels first: what the picture networks get) as an RGB picture.
Picture picture_from_chw(const uint8_t* chw, int h, int w);
// An RGB picture (uint8 [h, w, 3]) copied into a Picture.
Picture picture_from_rgb(const uint8_t* rgb, int h, int w);

// The vyrn.show() values seen so far: results only carry the ones that changed.
class ShowValues {
 public:
  void update(const ipc::Frame& frame);   // reads frame.j["show"]; ignores what it can't use
  void fill(LiveView& view) const;
  bool empty() const { return text_.empty() && pictures_.empty(); }

 private:
  std::vector<std::pair<std::string, std::string>> text_;
  std::vector<std::pair<std::string, Picture>> pictures_;
};

// A number for people: up to 4 significant digits after the point ("3.5", "0.1235", "1240", "1.5e+08").
std::string short_number(double v);

}  // namespace vyrn::run
