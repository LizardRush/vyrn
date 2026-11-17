#include "brain/nets.h"

#include <sstream>
#include <stdexcept>

namespace vyrn::brain {

namespace F = torch::nn::functional;

bool operator==(const NetConfig& a, const NetConfig& b) {
  return a.kind == b.kind && a.in_c == b.in_c && a.in_h == b.in_h && a.in_w == b.in_w && a.d == b.d &&
         a.n_actions == b.n_actions;
}

const char* to_string(NetKind kind) {
  switch (kind) {
    case NetKind::MLP: return "mlp";
    case NetKind::SmallCNN: return "small_cnn";
    case NetKind::DER: return "der";
  }
  return "?";
}

std::string describe(const NetConfig& cfg) {
  std::ostringstream os;
  switch (cfg.kind) {
    case NetKind::MLP: os << "MLP, " << cfg.d << " numbers"; break;
    case NetKind::SmallCNN: os << "small CNN, " << cfg.in_c << "x" << cfg.in_h << "x" << cfg.in_w; break;
    case NetKind::DER: os << "DER CNN, " << cfg.in_c << "x" << cfg.in_h << "x" << cfg.in_w; break;
  }
  os << " -> " << cfg.n_actions << " choices";
  return os.str();
}

NetConfig net_config_for(const BrainShape& shape) {
  NetConfig cfg;
  cfg.n_actions = static_cast<int>(shape.choices.size());
  if (shape.kind == ObsSpec::Kind::Numbers) {
    cfg.kind = NetKind::MLP;
    cfg.d = shape.d;
  } else {
    cfg.kind = shape.small_picture ? NetKind::SmallCNN : NetKind::DER;
    cfg.in_c = shape.in_c;
    cfg.in_h = shape.in_h;
    cfg.in_w = shape.in_w;
  }
  return cfg;
}

namespace {

int conv_out(int in, int kernel, int stride, int pad) {
  const int span = in + 2 * pad - kernel;
  return span < 0 ? 0 : span / stride + 1;   // 0: the kernel doesn't fit
}

}  // namespace

QNetImpl::QNetImpl(const NetConfig& cfg) : cfg_(cfg) {
  if (cfg.n_actions < 1) throw std::invalid_argument("A network needs at least one choice.");
  using torch::nn::Conv2dOptions;
  using torch::nn::Linear;
  switch (cfg.kind) {
    case NetKind::MLP: {
      if (cfg.d < 1) throw std::invalid_argument("The number network needs at least one input.");
      fc1 = register_module("fc1", Linear(cfg.d, 128));
      fc2 = register_module("fc2", Linear(128, 128));
      head = register_module("head", Linear(128, cfg.n_actions));
      break;
    }
    case NetKind::SmallCNN: {
      // 16@3x3 s1 p1 -> 32@3x3 s2 p1 -> FC 256. The padding lets even 1xN pictures through.
      if (cfg.in_c < 1 || cfg.in_h < 1 || cfg.in_w < 1) throw std::invalid_argument("Empty picture input.");
      conv1 = register_module("conv1", torch::nn::Conv2d(Conv2dOptions(cfg.in_c, 16, 3).stride(1).padding(1)));
      conv2 = register_module("conv2", torch::nn::Conv2d(Conv2dOptions(16, 32, 3).stride(2).padding(1)));
      const int h = conv_out(cfg.in_h, 3, 2, 1), w = conv_out(cfg.in_w, 3, 2, 1);
      fc1 = register_module("fc1", Linear(32 * h * w, 256));
      head = register_module("head", Linear(256, cfg.n_actions));
      break;
    }
    case NetKind::DER: {
      // 32@5x5 s5 -> 64@5x5 s5 -> FC 256 (84x84 -> 16x16 -> 3x3).
      const int h1 = conv_out(cfg.in_h, 5, 5, 0), w1 = conv_out(cfg.in_w, 5, 5, 0);
      const int h2 = conv_out(h1, 5, 5, 0), w2 = conv_out(w1, 5, 5, 0);
      if (cfg.in_c < 1 || h2 < 1 || w2 < 1) throw std::invalid_argument("The DER network needs pictures of at least 25x25.");
      conv1 = register_module("conv1", torch::nn::Conv2d(Conv2dOptions(cfg.in_c, 32, 5).stride(5)));
      conv2 = register_module("conv2", torch::nn::Conv2d(Conv2dOptions(32, 64, 5).stride(5)));
      fc1 = register_module("fc1", Linear(64 * h2 * w2, 256));
      head = register_module("head", Linear(256, cfg.n_actions));
      break;
    }
  }
}

torch::Tensor QNetImpl::forward(torch::Tensor x) {
  // One observation without a batch dimension ([d], [C,H,W] or flat) counts as a batch of one.
  const bool single = cfg_.kind == NetKind::MLP ? x.dim() <= 1 : (x.dim() <= 1 || x.dim() == 3);
  const int64_t batch = single ? 1 : x.size(0);
  if (cfg_.kind == NetKind::MLP) {
    x = x.reshape({batch, cfg_.d}).to(torch::kFloat);
    x = torch::relu(fc1->forward(x));
    x = torch::relu(fc2->forward(x));
    return head->forward(x);
  }
  // Out of place on purpose: a float input must not be changed for the caller.
  x = x.reshape({batch, cfg_.in_c, cfg_.in_h, cfg_.in_w}).to(torch::kFloat) * (1.0f / 255.0f);
  x = torch::relu(conv1->forward(x));
  x = torch::relu(conv2->forward(x));
  x = torch::relu(fc1->forward(x.flatten(1)));
  return head->forward(x);
}

void QNetImpl::grow_actions(int new_n) {
  const int old_n = cfg_.n_actions;
  if (new_n < old_n) throw std::invalid_argument("A brain can only gain choices, not lose them.");
  if (new_n == old_n) return;
  torch::NoGradGuard no_grad;
  torch::nn::Linear fresh(head->options.in_features(), new_n);
  fresh->to(head->weight.device());
  fresh->weight.narrow(0, 0, old_n).copy_(head->weight);
  fresh->bias.narrow(0, 0, old_n).copy_(head->bias);
  fresh->weight.narrow(0, old_n, new_n - old_n).uniform_(-1e-3, 1e-3);
  fresh->bias.narrow(0, old_n, new_n - old_n).zero_();
  head = replace_module("head", fresh);
  cfg_.n_actions = new_n;
}

QNet make_qnet(const NetConfig& cfg) { return QNet(cfg); }

void grow_actions(QNet& net, int new_n) { net->grow_actions(new_n); }

void copy_weights(const QNet& src, QNet& dst) {
  if (src->config() != dst->config()) throw std::invalid_argument("copy_weights: the networks differ");
  torch::NoGradGuard no_grad;
  auto from = src->named_parameters(true);
  for (auto& p : dst->named_parameters(true)) p.value().copy_(from[p.key()]);
  auto from_buffers = src->named_buffers(true);
  for (auto& b : dst->named_buffers(true)) b.value().copy_(from_buffers[b.key()]);
}

QNet clone_qnet(const QNet& src) {
  QNet copy(src->config());
  auto params = src->parameters();
  if (!params.empty()) copy->to(params.front().device());
  copy_weights(src, copy);
  return copy;
}

}  // namespace vyrn::brain
