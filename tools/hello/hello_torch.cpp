// Torch side of the M0 toolchain proof. Never include Qt headers here.
#include <torch/torch.h>

#include <chrono>
#include <sstream>
#include <string>

std::string hello_torch() {
  at::globalContext().setUserEnabledMkldnn(false);
  torch::manual_seed(0);
  torch::nn::Sequential net(
      torch::nn::Conv2d(torch::nn::Conv2dOptions(12, 32, 5).stride(5)), torch::nn::ReLU(),
      torch::nn::Conv2d(torch::nn::Conv2dOptions(32, 64, 5).stride(5)), torch::nn::ReLU(),
      torch::nn::Flatten(), torch::nn::Linear(64 * 3 * 3, 256), torch::nn::ReLU(),
      torch::nn::Linear(256, 3));
  torch::optim::Adam opt(net->parameters(), torch::optim::AdamOptions(1e-4));
  auto x = torch::rand({32, 12, 84, 84});
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i) {
    opt.zero_grad();
    auto loss = net->forward(x).pow(2).mean();
    loss.backward();
    opt.step();
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5;
  std::ostringstream os;
  os << "torch " << TORCH_VERSION << " ok (threads " << at::get_num_threads()
     << ", DER-net batch-32 train step " << ms << " ms)";
  return os.str();
}
