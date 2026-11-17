// The torch side of sharing: the online network's weights <-> model.safetensors (docs/dev/package.md section 3).
// Package data is never given to torch::load: tensors are copied by exact name and shape into a freshly built
// network, which is then saved with the core's own checkpoint code.
#include "brain/checkpoint.h"
#include "brain/dqn.h"
#include "brain/nets.h"
#include "package/internal.h"

#include <torch/torch.h>

#include <cstring>
#include <map>

namespace vyrn::package_detail {

namespace {

struct Named {
  std::string name;
  torch::Tensor tensor;
  bool parameter;
};

std::vector<Named> named_state(const brain::QNet& net) {
  std::vector<Named> out;
  for (const auto& p : net->named_parameters(true)) out.push_back({p.key(), p.value(), true});
  for (const auto& b : net->named_buffers(true)) out.push_back({b.key(), b.value(), false});
  return out;
}

std::vector<int64_t> sizes_of(const torch::Tensor& t) { return std::vector<int64_t>(t.sizes().begin(), t.sizes().end()); }

// The shape in a saved brain.json, with the bounds a shared brain must keep.
brain::BrainShape checked_saved_shape(const json& meta) {
  brain::BrainShape shape;
  try {
    shape = meta.at("shape").get<brain::BrainShape>();
  } catch (const std::exception&) {
    fail("This brain's saved description is damaged, so it can't be shared.");
  }
  try {
    check_shape_bounds(shape);
  } catch (const std::invalid_argument& e) {
    fail(std::string("This brain can't be shared: ") + e.what());
  }
  return shape;
}

}  // namespace

std::string net_name_for(const brain::BrainShape& shape) { return brain::to_string(brain::net_config_for(shape).kind); }

std::string net_description_for(const brain::BrainShape& shape) {
  return brain::describe(brain::net_config_for(shape));
}

LocalBrain read_local_brain(const std::string& brain_dir, const std::string& which) {
  const std::string other = which == "best" ? "latest" : "best";
  for (const std::string& w : {which, other}) {
    brain::BrainMeta meta;
    bool have = false;
    try {
      have = brain::read_checkpoint_meta(brain_dir, w, meta);
    } catch (const std::exception&) {
      have = false;
    }
    if (!have) continue;

    LocalBrain out;
    out.shape = checked_saved_shape(meta.j);
    brain::Dqn dqn(brain::DqnConfig{}, brain::net_config_for(out.shape));
    bool loaded = false;
    try {
      loaded = brain::load_checkpoint(brain_dir, w, dqn, meta);
    } catch (const std::exception&) {
      fail("This brain's saved weights don't match its description, so it can't be shared.");
    }
    if (!loaded) continue;
    const brain::BrainShape loaded_shape = checked_saved_shape(meta.j);   // the save that was really loaded
    if (brain::net_config_for(loaded_shape) != brain::net_config_for(out.shape)) {
      fail("This brain's saved weights don't match its description, so it can't be shared.");
    }
    out.shape = loaded_shape;

    torch::NoGradGuard no_grad;
    std::vector<std::string> buffers;
    std::vector<Named> state = named_state(dqn.online());
    buffers.reserve(state.size());
    std::vector<TensorView> views;
    for (const Named& n : state) {
      const torch::Tensor t = n.tensor.detach().to(torch::kCPU, torch::kFloat).contiguous();
      if (!torch::isfinite(t).all().item<bool>()) {
        fail("This brain's weights hold invalid numbers (training went wrong), so it can't be shared.");
      }
      buffers.emplace_back(reinterpret_cast<const char*>(t.data_ptr<float>()), static_cast<size_t>(t.numel()) * 4);
      if (n.parameter) out.parameters += t.numel();
    }
    for (size_t i = 0; i < state.size(); ++i) views.push_back({state[i].name, sizes_of(state[i].tensor), buffers[i]});
    out.safetensors = encode_safetensors(views, net_name_for(out.shape));
    out.tensors = static_cast<int>(state.size());
    out.updates = dqn.state().updates;
    out.max_abs_reward = dqn.state().max_abs_reward;
    out.reward_frozen = dqn.state().reward_frozen;
    out.meta = std::move(meta.j);
    out.which = w;
    return out;
  }
  fail("This brain has no saved weights yet. Train it for a little while first.");
}

void check_weights_fit(const brain::BrainShape& shape, const std::vector<TensorView>& tensors, int64_t* parameters) {
  const brain::QNet net = brain::make_qnet(brain::net_config_for(shape));
  std::map<std::string, const TensorView*> by_name;
  for (const TensorView& t : tensors) by_name[t.name] = &t;
  const std::vector<Named> state = named_state(net);
  if (state.size() != tensors.size()) {
    throw WeightsError("the network has " + std::to_string(state.size()) + " tensors, the file " +
                       std::to_string(tensors.size()));
  }
  int64_t count = 0;
  for (const Named& n : state) {
    const auto it = by_name.find(n.name);
    if (it == by_name.end()) throw WeightsError("tensor " + n.name + " is missing");
    if (it->second->shape != sizes_of(n.tensor)) throw WeightsError("tensor " + n.name + " has the wrong shape");
    if (n.parameter) count += n.tensor.numel();
  }
  if (parameters) *parameters = count;
}

void write_brain_checkpoints(const std::string& brain_dir, const BrainPackage& brain, const json& meta) {
  const std::vector<TensorView> tensors = decode_safetensors(brain.weights, brain.net);
  check_weights_fit(brain.shape, tensors, nullptr);
  std::map<std::string, const TensorView*> by_name;
  for (const TensorView& t : tensors) by_name[t.name] = &t;

  brain::QNet net = brain::make_qnet(brain::net_config_for(brain.shape));
  {
    torch::NoGradGuard no_grad;
    for (const Named& n : named_state(net)) {
      const TensorView& view = *by_name.at(n.name);
      torch::Tensor t = n.tensor;
      if (!t.is_contiguous() || t.scalar_type() != torch::kFloat || !t.device().is_cpu() ||
          static_cast<size_t>(t.numel()) * 4 != view.data.size()) {
        throw WeightsError("tensor " + n.name + " can't be filled");
      }
      std::memcpy(t.data_ptr<float>(), view.data.data(), view.data.size());
    }
  }
  brain::ModelState state;
  state.updates = brain.model_updates;
  state.max_abs_reward = brain.max_abs_reward;
  state.reward_frozen = brain.reward_frozen;
  const brain::Policy policy(net, state);
  const brain::BrainMeta checkpoint_meta{meta};
  brain::save_checkpoint(brain_dir, "best", policy, checkpoint_meta);
  brain::save_checkpoint(brain_dir, "latest", policy, checkpoint_meta);
}

}  // namespace vyrn::package_detail
