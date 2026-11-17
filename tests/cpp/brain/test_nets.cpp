// Every picture size goes through preprocessing and the right network, forward and backward.
#include "brain/nets.h"
#include "brain/preprocess.h"
#include "brain/threads.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>

using namespace vyrn::brain;

namespace {

Spec picture_spec(int h, int w, int n_choices = 3) {
  Spec s;
  s.mode = "sim";
  s.obs.kind = ObsSpec::Kind::Picture;
  s.obs.h = h;
  s.obs.w = w;
  for (int i = 0; i < n_choices; ++i) s.choices.push_back("choice" + std::to_string(i));
  return s;
}

void expect_finite_grads(QNet& net) {
  for (const auto& p : net->named_parameters()) {
    ASSERT_TRUE(p.value().grad().defined()) << p.key();
    EXPECT_TRUE(torch::isfinite(p.value().grad()).all().item<bool>()) << p.key();
  }
}

}  // namespace

TEST(Nets, EveryPictureSizeRunsThroughTheRightNet) {
  init_torch_process();
  set_torch_threads(2);
  torch::manual_seed(0);
  std::mt19937 rng(1);
  // (height, width): 1x1, 1x10, 4x4, 10x10, 32x32, 33x33, 640x480, 1920x1080 (width x height).
  const std::vector<std::pair<int, int>> sizes = {{1, 1},   {1, 10},  {4, 4},    {10, 10},
                                                  {32, 32}, {33, 33}, {480, 640}, {1080, 1920}};
  for (auto [h, w] : sizes) {
    SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h));
    const BrainShape shape = make_brain_shape(picture_spec(h, w), 4);
    const NetConfig cfg = net_config_for(shape);
    const bool small = std::max(h, w) <= 32;
    EXPECT_EQ(cfg.kind, small ? NetKind::SmallCNN : NetKind::DER);
    EXPECT_EQ(cfg.in_c, 12);
    EXPECT_EQ(cfg.in_h, small ? h : 84);
    EXPECT_EQ(cfg.in_w, small ? w : 84);

    // Two observations, each a stack of 4 preprocessed random pictures.
    const Geometry g = choose_geometry(h, w);
    const size_t frame_bytes = static_cast<size_t>(3) * g.h * g.w;
    std::vector<uint8_t> rgb(static_cast<size_t>(h) * w * 3), frame(frame_bytes);
    std::uniform_int_distribution<int> byte(0, 255);
    auto obs = torch::empty({2, 12 * static_cast<int64_t>(g.h) * g.w}, torch::kUInt8);
    for (int b = 0; b < 2; ++b) {
      FrameStack stack(4, frame_bytes);
      for (int f = 0; f < 4; ++f) {
        for (auto& v : rgb) v = static_cast<uint8_t>(byte(rng));
        to_frame(rgb.data(), h, w, g, frame.data());
        f == 0 ? stack.reset(frame.data()) : stack.push(frame.data());
      }
      std::memcpy(obs[b].data_ptr<uint8_t>(), stack.stacked().data(), stack.stacked().size());
    }

    QNet net = make_qnet(cfg);
    torch::Tensor q = net->forward(obs);
    EXPECT_EQ(q.sizes(), (std::vector<int64_t>{2, 3}));
    EXPECT_TRUE(torch::isfinite(q).all().item<bool>());
    q.sum().backward();
    expect_finite_grads(net);
    // Shaped input [B, C, H, W] and a single unbatched observation give the same values.
    torch::Tensor q4 = net->forward(obs.reshape({2, 12, g.h, g.w}));
    EXPECT_TRUE(torch::allclose(q, q4));
    torch::Tensor q1 = net->forward(obs[1].reshape({12, g.h, g.w}));
    EXPECT_EQ(q1.sizes(), (std::vector<int64_t>{1, 3}));
    EXPECT_TRUE(torch::allclose(q1[0], q[1], 1e-5, 1e-6));
  }
}

TEST(Nets, MlpForNumbers) {
  torch::manual_seed(0);
  QNet net = make_qnet(NetConfig{NetKind::MLP, 0, 0, 0, 7, 4});
  torch::Tensor x = torch::randn({5, 7});
  torch::Tensor q = net->forward(x);
  EXPECT_EQ(q.sizes(), (std::vector<int64_t>{5, 4}));
  q.pow(2).mean().backward();
  expect_finite_grads(net);
  EXPECT_EQ(net->forward(x[0]).sizes(), (std::vector<int64_t>{1, 4}));
  // Layer sizes: 7 -> 128 -> 128 -> 4.
  int64_t count = 0;
  for (const auto& p : net->parameters()) count += p.numel();
  EXPECT_EQ(count, (7 * 128 + 128) + (128 * 128 + 128) + (128 * 4 + 4));
}

TEST(Nets, PictureInputIsDividedBy255Inside) {
  torch::manual_seed(0);
  QNet net = make_qnet(NetConfig{NetKind::SmallCNN, 3, 4, 4, 0, 2});
  torch::Tensor bytes = torch::randint(0, 256, {1, 3 * 4 * 4}, torch::kUInt8);
  torch::Tensor raw_float = bytes.to(torch::kFloat);
  torch::Tensor copy = raw_float.clone();
  EXPECT_TRUE(torch::allclose(net->forward(bytes), net->forward(raw_float)));
  EXPECT_TRUE(torch::equal(raw_float, copy));   // the caller's tensor is not changed in place
}

TEST(Nets, GrowActionsKeepsOldOutputs) {
  torch::manual_seed(0);
  QNet net = make_qnet(NetConfig{NetKind::SmallCNN, 12, 10, 10, 0, 3});
  torch::Tensor x = torch::randint(0, 256, {4, 12 * 10 * 10}, torch::kUInt8);
  torch::Tensor before = net->forward(x).detach();
  grow_actions(net, 5);
  EXPECT_EQ(net->config().n_actions, 5);
  torch::Tensor after = net->forward(x).detach();
  EXPECT_EQ(after.sizes(), (std::vector<int64_t>{4, 5}));
  EXPECT_TRUE(torch::allclose(after.narrow(1, 0, 3), before));
  EXPECT_LT(after.narrow(1, 3, 2).abs().max().item<float>(), 0.5f);   // small new outputs
  EXPECT_THROW(grow_actions(net, 4), std::invalid_argument);
}

TEST(Nets, CloneIsADeepCopy) {
  torch::manual_seed(0);
  QNet net = make_qnet(NetConfig{NetKind::MLP, 0, 0, 0, 3, 2});
  QNet copy = clone_qnet(net);
  torch::Tensor x = torch::randn({2, 3});
  EXPECT_TRUE(torch::equal(net->forward(x), copy->forward(x)));
  {
    torch::NoGradGuard no_grad;
    for (auto& p : net->parameters()) p.add_(1.0);
  }
  EXPECT_FALSE(torch::equal(net->forward(x), copy->forward(x)));
}

TEST(Nets, ConfigForBrainShapes) {
  BrainShape numbers;
  numbers.kind = ObsSpec::Kind::Numbers;
  numbers.d = 3;
  numbers.choices = {"a", "b"};
  const NetConfig n = net_config_for(numbers);
  EXPECT_EQ(n.kind, NetKind::MLP);
  EXPECT_EQ(n.d, 3);
  EXPECT_EQ(n.n_actions, 2);
  EXPECT_NE(describe(n).find("MLP"), std::string::npos);
  EXPECT_THROW(make_qnet(NetConfig{NetKind::DER, 12, 20, 20, 0, 2}), std::invalid_argument);   // too small for DER
}
