#include "brain/preprocess.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vyrn::brain {

namespace {

// One output position of a 1-D resampling: weights[i] applies to input index first + i.
struct Taps {
  int first = 0;
  std::vector<float> weights;
};

// Area averaging (shrinking): output i covers input [i*s, (i+1)*s), each input pixel weighted by its overlap.
std::vector<Taps> area_taps(int in, int out) {
  std::vector<Taps> taps(out);
  const double scale = static_cast<double>(in) / out;
  for (int i = 0; i < out; ++i) {
    const double start = i * scale, end = (i + 1) * scale;
    const int first = static_cast<int>(std::floor(start));
    const int last = std::min(in - 1, static_cast<int>(std::ceil(end)) - 1);
    taps[i].first = first;
    for (int j = first; j <= last; ++j) {
      const double overlap = std::min(end, j + 1.0) - std::max(start, static_cast<double>(j));
      taps[i].weights.push_back(static_cast<float>(std::max(0.0, overlap) / scale));
    }
  }
  return taps;
}

// Bilinear (growing), pixel centres aligned: input x = (i + 0.5) * in / out - 0.5, clamped to the edges.
std::vector<Taps> linear_taps(int in, int out) {
  std::vector<Taps> taps(out);
  const double scale = static_cast<double>(in) / out;
  for (int i = 0; i < out; ++i) {
    const double x = std::clamp((i + 0.5) * scale - 0.5, 0.0, static_cast<double>(in - 1));
    const int j0 = static_cast<int>(std::floor(x));
    const double f = x - j0;
    taps[i].first = j0;
    if (j0 + 1 < in && f > 0) {
      taps[i].weights = {static_cast<float>(1.0 - f), static_cast<float>(f)};
    } else {
      taps[i].weights = {1.0f};
    }
  }
  return taps;
}

std::vector<Taps> axis_taps(int in, int out) {
  if (in > out) return area_taps(in, out);
  return linear_taps(in, out);   // also the identity when in == out
}

inline uint8_t to_byte(float v) {
  return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

// Nearest pixel, centre-aligned: input i = floor((o + 0.5) * in / out).
inline int nearest(int o, int in, int out) {
  return std::min(in - 1, static_cast<int>(((2LL * o + 1) * in) / (2LL * out)));
}

}  // namespace

Geometry choose_geometry(int h, int w) {
  if (h < 1 || w < 1) {
    throw std::invalid_argument("A picture must be at least 1x1 pixels (got " + std::to_string(w) + "x" +
                                std::to_string(h) + ").");
  }
  if (std::max(h, w) <= kSmallPictureMaxSide) return Geometry{true, h, w};
  return Geometry{false, kBigPictureSide, kBigPictureSide};
}

void to_frame(const uint8_t* rgb, int h, int w, const Geometry& g, uint8_t* out_chw) {
  if (h < 1 || w < 1 || g.h < 1 || g.w < 1) throw std::invalid_argument("to_frame: empty picture or frame");
  const size_t plane = static_cast<size_t>(g.h) * g.w;

  if (g.small || (h == g.h && w == g.w)) {
    // Native size (or nearest-resized to the size the small brain learned on): just reorder to CHW.
    for (int oy = 0; oy < g.h; ++oy) {
      const int sy = (h == g.h) ? oy : nearest(oy, h, g.h);
      for (int ox = 0; ox < g.w; ++ox) {
        const int sx = (w == g.w) ? ox : nearest(ox, w, g.w);
        const uint8_t* px = rgb + (static_cast<size_t>(sy) * w + sx) * 3;
        const size_t o = static_cast<size_t>(oy) * g.w + ox;
        out_chw[o] = px[0];
        out_chw[plane + o] = px[1];
        out_chw[2 * plane + o] = px[2];
      }
    }
    return;
  }

  // Separable resampling: rows first (width), then columns (height).
  const std::vector<Taps> tx = axis_taps(w, g.w);
  const std::vector<Taps> ty = axis_taps(h, g.h);
  std::vector<float> tmp(static_cast<size_t>(h) * g.w * 3);   // [h, g.w, 3]
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = rgb + static_cast<size_t>(y) * w * 3;
    float* dst = tmp.data() + static_cast<size_t>(y) * g.w * 3;
    for (int ox = 0; ox < g.w; ++ox) {
      const Taps& t = tx[ox];
      float r = 0, gg = 0, b = 0;
      const uint8_t* px = row + static_cast<size_t>(t.first) * 3;
      for (size_t i = 0; i < t.weights.size(); ++i, px += 3) {
        const float wt = t.weights[i];
        r += wt * px[0];
        gg += wt * px[1];
        b += wt * px[2];
      }
      dst[ox * 3] = r;
      dst[ox * 3 + 1] = gg;
      dst[ox * 3 + 2] = b;
    }
  }
  std::vector<float> acc(static_cast<size_t>(g.w) * 3);
  for (int oy = 0; oy < g.h; ++oy) {
    const Taps& t = ty[oy];
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (size_t i = 0; i < t.weights.size(); ++i) {
      const float wt = t.weights[i];
      const float* src = tmp.data() + static_cast<size_t>(t.first + static_cast<int>(i)) * g.w * 3;
      for (size_t k = 0; k < acc.size(); ++k) acc[k] += wt * src[k];
    }
    for (int ox = 0; ox < g.w; ++ox) {
      const size_t o = static_cast<size_t>(oy) * g.w + ox;
      out_chw[o] = to_byte(acc[ox * 3]);
      out_chw[plane + o] = to_byte(acc[ox * 3 + 1]);
      out_chw[2 * plane + o] = to_byte(acc[ox * 3 + 2]);
    }
  }
}

// ---- FrameStack ------------------------------------------------------------------------------------

FrameStack::FrameStack(int k, size_t frame_bytes)
    : k_(k), frame_bytes_(frame_bytes), data_(static_cast<size_t>(std::max(k, 1)) * frame_bytes) {
  if (k < 1 || frame_bytes == 0) throw std::invalid_argument("FrameStack needs k >= 1 and a non-empty frame");
}

void FrameStack::reset(const uint8_t* frame) {
  for (int i = 0; i < k_; ++i) std::memcpy(data_.data() + static_cast<size_t>(i) * frame_bytes_, frame, frame_bytes_);
}

void FrameStack::push(const uint8_t* frame) {
  if (k_ > 1) std::memmove(data_.data(), data_.data() + frame_bytes_, (static_cast<size_t>(k_) - 1) * frame_bytes_);
  std::memcpy(data_.data() + (static_cast<size_t>(k_) - 1) * frame_bytes_, frame, frame_bytes_);
}

// ---- RunningMeanStd ----------------------------------------------------------------------------------

RunningMeanStd::RunningMeanStd(int d) : d_(d), mean_(static_cast<size_t>(std::max(d, 0)), 0.0),
                                        var_(static_cast<size_t>(std::max(d, 0)), 1.0) {
  if (d < 1) throw std::invalid_argument("RunningMeanStd needs at least one number");
}

void RunningMeanStd::update(const float* x, int n_rows) {
  if (frozen || n_rows <= 0) return;
  const double n = n_rows;
  const double total = count_ + n;
  for (int i = 0; i < d_; ++i) {
    // Batch mean and variance (two passes), then Chan et al.'s parallel combination.
    double sum = 0;
    for (int r = 0; r < n_rows; ++r) sum += x[static_cast<size_t>(r) * d_ + i];
    const double batch_mean = sum / n;
    double sq = 0;
    for (int r = 0; r < n_rows; ++r) {
      const double dv = x[static_cast<size_t>(r) * d_ + i] - batch_mean;
      sq += dv * dv;
    }
    const double delta = batch_mean - mean_[i];
    const double m2 = var_[i] * count_ + sq + delta * delta * count_ * n / total;
    mean_[i] += delta * n / total;
    var_[i] = m2 / total;
  }
  count_ = total;
}

void RunningMeanStd::normalize(const float* x, float* out) const {
  for (int i = 0; i < d_; ++i) {
    const double z = (x[i] - mean_[i]) / std::sqrt(var_[i] + 1e-8);
    out[i] = static_cast<float>(std::clamp(z, -10.0, 10.0));
  }
}

void RunningMeanStd::normalize_rows(const float* x, float* out, int n_rows) const {
  for (int r = 0; r < n_rows; ++r) normalize(x + static_cast<size_t>(r) * d_, out + static_cast<size_t>(r) * d_);
}

json RunningMeanStd::to_json() const {
  return json{{"count", count_}, {"mean", mean_}, {"var", var_}};
}

void RunningMeanStd::from_json(const json& j) {
  try {
    auto mean = j.at("mean").get<std::vector<double>>();
    auto var = j.at("var").get<std::vector<double>>();
    const double count = j.at("count").get<double>();
    if (static_cast<int>(mean.size()) != d_ || static_cast<int>(var.size()) != d_) {
      throw std::invalid_argument("The saved number statistics are for " + std::to_string(mean.size()) +
                                  " numbers, but see() returns " + std::to_string(d_) + ".");
    }
    mean_ = std::move(mean);
    var_ = std::move(var);
    count_ = count;
  } catch (const json::exception& e) {
    throw std::invalid_argument(std::string("The saved number statistics are damaged: ") + e.what());
  }
}

}  // namespace vyrn::brain
