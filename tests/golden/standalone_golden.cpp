// Golden fixtures for the standalone runtime (docs/dev/standalone.md section 5): what the C++ brain computes, for
// tests/python/test_runtime_golden.py to compare with vyrn.runtime (numpy).
//
//   standalone_golden <out_dir>
//
// writes <out_dir>/golden.json (the index) and raw little-endian files next to it:
// - for an MLP, a small CNN and a DER brain (tests/cpp/package/helpers: random weights, one update): the exported
//   .vrl (vyrn::export_brain), random network inputs and their Q-values, and a played sequence (raw observations
//   -> preprocessing, frame stack with segment starts / number statistics -> network input -> Q-values);
// - to_frame() of random pictures of 1x1, 10x10, 32x32, 33x33, 640x480 (and 9x5) for their own geometry, the
//   small 10x10 one and the big 84x84 one.
#include "helpers.h"

#include "brain/checkpoint.h"
#include "brain/dqn.h"
#include "brain/nets.h"
#include "brain/preprocess.h"
#include "vyrn/package.h"

#include <nlohmann/json.hpp>
#include <torch/torch.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace vyrn::brain;

fs::path g_out;

template <typename T>
std::string write_raw(const std::string& name, const std::vector<T>& values) {
  std::ofstream f(g_out / name, std::ios::binary);
  f.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
  if (!f) throw std::runtime_error("can't write " + name);
  return name;
}

std::vector<float> q_of(QNet& net, const torch::Tensor& x) {
  torch::NoGradGuard no_grad;
  const torch::Tensor q = net->forward(x).contiguous().to(torch::kFloat);
  return std::vector<float>(q.data_ptr<float>(), q.data_ptr<float>() + q.numel());
}

std::vector<uint8_t> random_picture(std::mt19937& rng, int h, int w) {
  std::uniform_int_distribution<int> byte(0, 255);
  std::vector<uint8_t> p(static_cast<size_t>(h) * w * 3);
  for (auto& v : p) v = static_cast<uint8_t>(byte(rng));
  return p;
}

json net_fixture(const std::string& root, pkg_test::Net kind, const std::string& name, std::mt19937& rng) {
  const std::string project = pkg_test::make_project(root, name, name);
  pkg_test::add_brain(project, "20261115-120000", kind, 11);
  const std::string vrl = name + ".vrl";
  vyrn::export_brain(project, (g_out / vrl).string());

  BrainMeta meta;
  const std::string dir = (fs::path(project) / "brains" / "20261115-120000").string();
  if (!read_checkpoint_meta(dir, "best", meta)) throw std::runtime_error("no best brain in " + dir);
  const BrainShape shape = meta.j.at("shape").get<BrainShape>();
  const NetConfig cfg = net_config_for(shape);
  Dqn dqn(DqnConfig{}, cfg);
  if (!load_checkpoint(dir, "best", dqn, meta)) throw std::runtime_error("can't load " + dir);
  QNet& net = dqn.online();

  json out = {{"name", name}, {"vrl", vrl}, {"n", cfg.n_actions}};
  const int64_t batch = 8;
  if (kind == pkg_test::Net::MLP) {
    std::normal_distribution<float> normal(0.0f, 3.0f);
    std::vector<float> x(static_cast<size_t>(batch) * cfg.d);
    for (auto& v : x) v = normal(rng);
    x[0] = 1e6f;   // clipped to +10 by the statistics
    out["inputs"] = write_raw(name + "_inputs.f32", x);
    out["input_shape"] = {batch, cfg.d};
    out["q"] = write_raw(name + "_q.f32", q_of(net, torch::from_blob(x.data(), {batch, cfg.d}).clone()));

    // A played sequence: raw see() numbers -> the saved statistics -> the network.
    RunningMeanStd rms(shape.d);
    rms.from_json(meta.j.at("normalizer"));
    rms.frozen = true;
    const int steps = 6;
    std::vector<float> raw(static_cast<size_t>(steps) * cfg.d), norm(raw.size()), q;
    for (auto& v : raw) v = normal(rng);
    raw[1] = -1e7f;
    for (int s = 0; s < steps; ++s) {
      rms.normalize(raw.data() + static_cast<size_t>(s) * cfg.d, norm.data() + static_cast<size_t>(s) * cfg.d);
      std::vector<float> row(norm.begin() + s * cfg.d, norm.begin() + (s + 1) * cfg.d);
      const auto qs = q_of(net, torch::from_blob(row.data(), {1, cfg.d}).clone());
      q.insert(q.end(), qs.begin(), qs.end());
    }
    out["sequence"] = {{"raw", write_raw(name + "_seq_raw.f32", raw)}, {"steps", steps},
                       {"net_input", write_raw(name + "_seq_input.f32", norm)},
                       {"q", write_raw(name + "_seq_q.f32", q)}};
    return out;
  }

  const int64_t flat = int64_t{cfg.in_c} * cfg.in_h * cfg.in_w;
  std::uniform_int_distribution<int> byte(0, 255);
  std::vector<uint8_t> x(static_cast<size_t>(batch * flat));
  for (auto& v : x) v = static_cast<uint8_t>(byte(rng));
  out["inputs"] = write_raw(name + "_inputs.u8", x);
  out["input_shape"] = {batch, cfg.in_c, cfg.in_h, cfg.in_w};
  out["q"] = write_raw(name + "_q.f32", q_of(net, torch::from_blob(x.data(), {batch, flat}, torch::kUInt8).clone()));

  // A played sequence with two segments (a reset at step 0 and at step 4): pictures -> to_frame -> FrameStack.
  const Geometry geo{shape.small_picture, shape.in_h, shape.in_w};
  const int ph = shape.small_picture ? 7 : 120, pw = shape.small_picture ? 13 : 160;   // resized either way
  const size_t frame_bytes = static_cast<size_t>(3) * geo.h * geo.w;
  FrameStack stack(shape.frames, frame_bytes);
  const int steps = 7;
  std::vector<uint8_t> pictures, inputs;
  std::vector<float> q;
  std::vector<uint8_t> frame(frame_bytes);
  for (int s = 0; s < steps; ++s) {
    const std::vector<uint8_t> p = random_picture(rng, ph, pw);
    pictures.insert(pictures.end(), p.begin(), p.end());
    to_frame(p.data(), ph, pw, geo, frame.data());
    if (s == 0 || s == 4) {
      stack.reset(frame.data());
    } else {
      stack.push(frame.data());
    }
    const std::vector<uint8_t>& stacked = stack.stacked();
    inputs.insert(inputs.end(), stacked.begin(), stacked.end());
    std::vector<uint8_t> copy(stacked);
    const auto qs = q_of(net, torch::from_blob(copy.data(), {1, flat}, torch::kUInt8).clone());
    q.insert(q.end(), qs.begin(), qs.end());
  }
  out["sequence"] = {{"pictures", write_raw(name + "_seq_pictures.u8", pictures)}, {"steps", steps},
                     {"picture_shape", {ph, pw, 3}}, {"starts", {0, 4}},
                     {"net_input", write_raw(name + "_seq_input.u8", inputs)},
                     {"q", write_raw(name + "_seq_q.f32", q)}};
  return out;
}

json frame_fixtures(std::mt19937& rng) {
  json cases = json::array();
  const std::vector<std::pair<int, int>> sizes = {{1, 1}, {10, 10}, {32, 32}, {33, 33}, {480, 640}, {5, 9}};
  for (const auto& [h, w] : sizes) {
    const std::vector<uint8_t> p = random_picture(rng, h, w);
    const std::string picture = write_raw("picture_" + std::to_string(h) + "x" + std::to_string(w) + ".u8", p);
    const std::vector<Geometry> geos = {choose_geometry(h, w), Geometry{true, 10, 10}, Geometry{false, 84, 84}};
    for (size_t i = 0; i < geos.size(); ++i) {
      const Geometry& g = geos[i];
      std::vector<uint8_t> out(static_cast<size_t>(3) * g.h * g.w);
      to_frame(p.data(), h, w, g, out.data());
      const std::string file = "frame_" + std::to_string(h) + "x" + std::to_string(w) + "_" + std::to_string(i) + ".u8";
      cases.push_back({{"h", h}, {"w", w}, {"picture", picture}, {"small", g.small}, {"gh", g.h}, {"gw", g.w},
                       {"frame", write_raw(file, out)}});
    }
  }
  return cases;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: standalone_golden <out_dir>\n";
    return 2;
  }
  try {
    g_out = fs::absolute(argv[1]);
    fs::remove_all(g_out);   // fixtures from an earlier run would make export refuse to overwrite them
    fs::create_directories(g_out);
    torch::set_num_threads(1);
    std::mt19937 rng(20261115);
    pkg_test::TempDir tmp;
    json index = {{"nets", json::array()}, {"frames", frame_fixtures(rng)}};
    index["nets"].push_back(net_fixture(tmp.path(), pkg_test::Net::MLP, "mlp", rng));
    index["nets"].push_back(net_fixture(tmp.path(), pkg_test::Net::SmallCNN, "small_cnn", rng));
    index["nets"].push_back(net_fixture(tmp.path(), pkg_test::Net::DER, "der", rng));
    std::ofstream(g_out / "golden.json") << index.dump(1) << "\n";
    std::cout << "standalone golden fixtures written to " << g_out.string() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "standalone_golden: " << e.what() << "\n";
    return 1;
  }
}
