// Crash-safe brain checkpoints: <brain>/<which>/save-<n>/{model.pt, optim.pt, brain.json} plus a small
// <brain>/<which>/current file naming the live save. A save writes a fresh save-<n>/, fsyncs it, then
// atomically replaces `current` and deletes older save-* folders, so a crash at any moment leaves the
// previous checkpoint loadable.
#pragma once

#include "brain/dqn.h"

#include <functional>
#include <string>

namespace vyrn::brain {

struct BrainMeta {
  json j;   // spec/shape, config, counters, normalizer stats, reward scale, best score, version
};

// Saves the whole learner (weights, target, Adam state, counters). `which` is a folder name such as
// "latest" or "best". brain.json = meta.j plus a "dqn" entry with the Dqn's counters and reward scale.
// Throws std::runtime_error / std::filesystem::filesystem_error on I/O errors.
void save_checkpoint(const std::string& brain_dir, const std::string& which, const Dqn& dqn, const BrainMeta& meta);

// Weights only (e.g. the Best weights kept in RAM): model.pt + brain.json, no optim.pt. Loading it into a
// Dqn gives a fresh optimizer.
void save_checkpoint(const std::string& brain_dir, const std::string& which, const Policy& weights,
                     const BrainMeta& meta);

// Loads the live save into `dqn` and `meta`. Falls back to the newest other save that loads when the live
// one is missing or damaged. Returns false if there is no loadable checkpoint. Throws BrainMismatch when a
// save exists but was built for a different network (check the shape in brain.json first).
bool load_checkpoint(const std::string& brain_dir, const std::string& which, Dqn& dqn, BrainMeta& meta);

// Reads only brain.json of the save load_checkpoint would use first (to check the shape before building a
// Dqn). Returns false if there is none.
bool read_checkpoint_meta(const std::string& brain_dir, const std::string& which, BrainMeta& meta);

namespace test_hooks {
// Test-only: called with the name of each save stage ("folder", "model", "optim", "meta", "synced",
// "pointer", "swapped"). Throwing from it simulates a crash at that point (nothing is cleaned up).
extern std::function<void(const char* stage)> save_stage;
}  // namespace test_hooks

}  // namespace vyrn::brain
