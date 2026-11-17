// What the Python worker says the AI sees and can do (the `spec` reply), and what a saved brain was built for.
// No torch in this header.
#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace vyrn::brain {

using json = nlohmann::json;

// A spec message that doesn't follow protocol.md §3.2. The message is plain English.
struct SpecError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct ObsSpec {
  enum class Kind { Picture, Numbers } kind = Kind::Numbers;
  int h = 0, w = 0;                  // pictures: size in pixels (they always arrive as uint8 [h, w, 3])
  int d = 0;                         // numbers: how many
  std::vector<std::string> names;    // numbers: names from a dict, in order; empty when see() returned a list
};

struct Spec {
  std::string mode;                  // "sim" | "realtime"
  ObsSpec obs;
  std::vector<std::string> choices, labels, effects;
  bool uses_keys = false, uses_screen = false;
  json raw;                          // the whole message, for the UI
};

// Parses and validates the worker's `spec` reply. Throws SpecError.
Spec parse_spec(const json& spec_msg);

// What a saved brain was built for (stored in brain.json under "shape").
struct BrainShape {
  ObsSpec::Kind kind = ObsSpec::Kind::Numbers;
  int in_c = 0, in_h = 0, in_w = 0;  // pictures: network input = in_c (3 x frames) x in_h x in_w
  int d = 0;                         // numbers: input size
  bool small_picture = false;        // pictures with max side <= 32 keep their native size (small CNN)
  int frames = 1;                    // stacked frames (pictures only; numbers are never stacked)
  std::vector<std::string> choices;  // ordered choice names
  std::vector<std::string> inputs;   // ordered input names (numbers from a dict), else empty
};

// The brain a new run would build for `spec` (frames is ignored for numbers).
BrainShape make_brain_shape(const Spec& spec, int frames);

void to_json(json& j, const BrainShape& s);
void from_json(const json& j, BrainShape& s);   // throws SpecError on a malformed shape

enum class Compat { Same, AppendedChoices, Incompatible };

// Can a brain saved for `saved` keep learning with what the scripts produce now?
// `why_plain_english` (optional) is cleared, then set whenever there is something to tell the user:
//  - Incompatible: the reason, ending in "This change starts a new brain."
//  - AppendedChoices: which choices were added at the end.
//  - Same: a note when small pictures changed size (they are nearest-resized to the saved size).
// Picture brains: a small brain (max side <= 32) and a big one (84x84) are different networks, so
// moving between the two is Incompatible; any other size change is handled by resizing.
Compat compare(const BrainShape& saved, const Spec& now, std::string* why_plain_english);

const char* to_string(ObsSpec::Kind kind);   // "picture" | "numbers"

}  // namespace vyrn::brain
