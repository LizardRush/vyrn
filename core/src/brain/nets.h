// The three Q-networks: an MLP for numbers, a small CNN for small pictures and the DER CNN for 84x84 ones.
#pragma once

#include "brain/spec.h"

#include <torch/torch.h>

#include <string>

namespace vyrn::brain {

enum class NetKind { MLP, SmallCNN, DER };

struct NetConfig {
  NetKind kind = NetKind::MLP;
  int in_c = 0, in_h = 0, in_w = 0;   // pictures: stacked channels (3 x frames) and frame size
  int d = 0;                          // numbers: input size
  int n_actions = 0;
};

bool operator==(const NetConfig& a, const NetConfig& b);
inline bool operator!=(const NetConfig& a, const NetConfig& b) { return !(a == b); }
const char* to_string(NetKind kind);   // "mlp" | "small_cnn" | "der"
std::string describe(const NetConfig& cfg);   // e.g. "DER CNN, 12x84x84 -> 3 choices"

// The network a brain shape needs: numbers -> MLP; small pictures -> small CNN; others -> DER.
NetConfig net_config_for(const BrainShape& shape);

// forward(x) -> Q-values [B, n_actions].
// MLP: x is float, [B, d] (normalized numbers). Picture nets: x holds raw 0-255 values (uint8 expected),
// [B, in_c, in_h, in_w] or flat [B, in_c*in_h*in_w]; it is converted to float and divided by 255 inside.
struct QNetImpl : torch::nn::Module {
  explicit QNetImpl(const NetConfig& cfg);
  torch::Tensor forward(torch::Tensor x);
  const NetConfig& config() const { return cfg_; }
  void grow_actions(int new_n);   // see grow_actions() below

 private:
  NetConfig cfg_;
  torch::nn::Conv2d conv1{nullptr}, conv2{nullptr};
  torch::nn::Linear fc1{nullptr}, fc2{nullptr}, head{nullptr};
};
TORCH_MODULE(QNet);

QNet make_qnet(const NetConfig& cfg);

// Appends output rows for new choices (small random weights, zero bias) and keeps everything else (M5).
void grow_actions(QNet& net, int new_n);

// A new network with the same config and a copy of every parameter and buffer of `src`.
QNet clone_qnet(const QNet& src);

// Copies all parameters and buffers of `src` into `dst` (same config). No autograd.
void copy_weights(const QNet& src, QNet& dst);

}  // namespace vyrn::brain
