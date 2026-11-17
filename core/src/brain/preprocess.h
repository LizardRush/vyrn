// Turning what see() returns into network input: picture resizing, frame stacks and the number normalizer.
// No torch in this header.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vyrn::brain {

using json = nlohmann::json;

constexpr int kSmallPictureMaxSide = 32;   // pictures up to this size keep their native size
constexpr int kBigPictureSide = 84;        // bigger pictures are resized to 84 x 84

struct Geometry {
  bool small = false;   // small (max side <= 32): native size, small CNN; else 84 x 84, DER CNN
  int h = 0, w = 0;     // the frame size the network sees
};

// The frame size for a picture of h x w pixels. Throws std::invalid_argument for sizes below 1.
Geometry choose_geometry(int h, int w);

// Converts one RGB picture (uint8 [h, w, 3], row-major) into a network frame (uint8 [3, g.h, g.w], CHW).
// Small geometry: copied as is, or nearest-resized when the picture's size differs from g.
// Big geometry: each axis is area-averaged when shrinking and bilinear-interpolated when growing.
void to_frame(const uint8_t* rgb, int h, int w, const Geometry& g, uint8_t* out_chw);

// The last k frames, oldest first, as one contiguous block (k * frame_bytes). After reset(f) every slot
// holds f, so a segment's first frame is repeated until real history exists (AAAA -> AAAB -> AABC).
class FrameStack {
 public:
  FrameStack(int k, size_t frame_bytes);
  void reset(const uint8_t* frame);   // start of a segment
  void push(const uint8_t* frame);    // next frame of the same segment
  const std::vector<uint8_t>& stacked() const { return data_; }
  const uint8_t* newest() const { return data_.data() + (static_cast<size_t>(k_) - 1) * frame_bytes_; }
  int k() const { return k_; }
  size_t frame_bytes() const { return frame_bytes_; }

 private:
  int k_;
  size_t frame_bytes_;
  std::vector<uint8_t> data_;
};

// Running mean and variance of the numbers (float64), used to scale them for the network:
// out = clip((x - mean) / sqrt(var + 1e-8), -10, 10). Starts at mean 0, var 1 (count 0).
class RunningMeanStd {
 public:
  explicit RunningMeanStd(int d);
  void update(const float* x, int n_rows);         // rows of d numbers; does nothing while frozen
  void normalize(const float* x, float* out) const;
  void normalize_rows(const float* x, float* out, int n_rows) const;   // e.g. a sampled batch
  json to_json() const;
  void from_json(const json& j);                   // throws std::invalid_argument if it doesn't fit
  bool frozen = false;                             // Watch: use the saved statistics as they are

  int dim() const { return d_; }
  double count() const { return count_; }
  const std::vector<double>& mean() const { return mean_; }
  const std::vector<double>& var() const { return var_; }

 private:
  int d_;
  double count_ = 0;
  std::vector<double> mean_, var_;
};

}  // namespace vyrn::brain
