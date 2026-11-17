#include "brain/spec.h"

#include "brain/preprocess.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace vyrn::brain {

namespace {

[[noreturn]] void bad_spec(const std::string& what) {
  throw SpecError("The Python side sent a description Vyrn doesn't understand: " + what);
}

int positive_int(const json& v, const char* what) {
  if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 1'000'000) {
    bad_spec(std::string(what) + " must be a whole number of at least 1");
  }
  return static_cast<int>(v.get<int64_t>());
}

std::vector<std::string> string_list(const json& msg, const char* key, bool required) {
  std::vector<std::string> out;
  auto it = msg.find(key);
  if (it == msg.end() || it->is_null()) {
    if (required) bad_spec(std::string("\"") + key + "\" is missing");
    return out;
  }
  if (!it->is_array()) bad_spec(std::string("\"") + key + "\" must be a list of names");
  for (const auto& v : *it) {
    if (!v.is_string()) bad_spec(std::string("\"") + key + "\" must contain only text");
    out.push_back(v.get<std::string>());
  }
  return out;
}

std::string join(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ", ";
    out += names[i];
  }
  return out.empty() ? "(none)" : out;
}

std::string size_text(int h, int w) { return std::to_string(w) + "x" + std::to_string(h); }

const char* kind_phrase(ObsSpec::Kind k) {
  return k == ObsSpec::Kind::Picture ? "a picture" : "numbers";
}

}  // namespace

const char* to_string(ObsSpec::Kind kind) {
  return kind == ObsSpec::Kind::Picture ? "picture" : "numbers";
}

Spec parse_spec(const json& msg) {
  if (!msg.is_object()) bad_spec("it is not a JSON object");
  Spec spec;
  spec.raw = msg;
  spec.mode = msg.value("mode", "");
  if (spec.mode != "sim" && spec.mode != "realtime") bad_spec("\"mode\" must be \"sim\" or \"realtime\"");

  auto obs = msg.find("obs");
  if (obs == msg.end() || !obs->is_object()) bad_spec("\"obs\" is missing");
  const std::string kind = obs->value("kind", "");
  auto shape = obs->find("shape");
  if (shape == obs->end() || !shape->is_array()) bad_spec("\"obs.shape\" is missing");
  if (kind == "picture") {
    if (shape->size() != 3 || (*shape)[2] != 3) bad_spec("a picture's shape must be [height, width, 3]");
    spec.obs.kind = ObsSpec::Kind::Picture;
    spec.obs.h = positive_int((*shape)[0], "the picture height");
    spec.obs.w = positive_int((*shape)[1], "the picture width");
  } else if (kind == "numbers") {
    if (shape->size() != 1) bad_spec("the numbers' shape must be [count]");
    spec.obs.kind = ObsSpec::Kind::Numbers;
    spec.obs.d = positive_int((*shape)[0], "the number count");
    spec.obs.names = string_list(*obs, "names", false);
    if (!spec.obs.names.empty() && static_cast<int>(spec.obs.names.size()) != spec.obs.d) {
      bad_spec("there must be one name per number");
    }
  } else {
    bad_spec("\"obs.kind\" must be \"picture\" or \"numbers\"");
  }

  spec.choices = string_list(msg, "choices", true);
  if (spec.choices.size() < 2) bad_spec("the AI needs at least 2 choices");
  std::set<std::string> unique(spec.choices.begin(), spec.choices.end());
  if (unique.size() != spec.choices.size()) bad_spec("two choices have the same name");

  spec.labels = string_list(msg, "labels", false);
  if (spec.labels.size() != spec.choices.size()) {
    spec.labels = spec.choices;
    for (auto& label : spec.labels) std::replace(label.begin(), label.end(), '_', ' ');
  }
  spec.effects = string_list(msg, "effects", false);
  spec.effects.resize(spec.choices.size());

  auto uses = msg.find("uses");
  if (uses != msg.end() && uses->is_object()) {
    spec.uses_keys = uses->value("keys", false);
    spec.uses_screen = uses->value("screen", false);
  }
  return spec;
}

BrainShape make_brain_shape(const Spec& spec, int frames) {
  BrainShape s;
  s.kind = spec.obs.kind;
  s.choices = spec.choices;
  if (spec.obs.kind == ObsSpec::Kind::Picture) {
    const Geometry g = choose_geometry(spec.obs.h, spec.obs.w);
    s.frames = std::max(1, frames);
    s.in_c = 3 * s.frames;
    s.in_h = g.h;
    s.in_w = g.w;
    s.small_picture = g.small;
  } else {
    s.frames = 1;   // numbers are never stacked
    s.d = spec.obs.d;
    s.inputs = spec.obs.names;
  }
  return s;
}

void to_json(json& j, const BrainShape& s) {
  j = json{{"kind", to_string(s.kind)},
           {"in_c", s.in_c},
           {"in_h", s.in_h},
           {"in_w", s.in_w},
           {"d", s.d},
           {"small_picture", s.small_picture},
           {"frames", s.frames},
           {"choices", s.choices},
           {"inputs", s.inputs}};
}

void from_json(const json& j, BrainShape& s) {
  try {
    const std::string kind = j.at("kind").get<std::string>();
    if (kind != "picture" && kind != "numbers") bad_spec("the saved brain has an unknown kind");
    s.kind = kind == "picture" ? ObsSpec::Kind::Picture : ObsSpec::Kind::Numbers;
    s.in_c = j.value("in_c", 0);
    s.in_h = j.value("in_h", 0);
    s.in_w = j.value("in_w", 0);
    s.d = j.value("d", 0);
    s.small_picture = j.value("small_picture", false);
    s.frames = j.value("frames", 1);
    s.choices = j.at("choices").get<std::vector<std::string>>();
    s.inputs = j.value("inputs", std::vector<std::string>{});
  } catch (const json::exception& e) {
    throw SpecError(std::string("The saved brain's description is damaged: ") + e.what());
  }
  const bool picture_ok = s.in_c >= 1 && s.in_h >= 1 && s.in_w >= 1 && s.frames >= 1;
  if (s.kind == ObsSpec::Kind::Picture ? !picture_ok : s.d < 1) {
    throw SpecError("The saved brain's description is damaged: its input size is missing.");
  }
}

Compat compare(const BrainShape& saved, const Spec& now, std::string* why_plain_english) {
  std::string scratch;
  std::string& why = why_plain_english ? *why_plain_english : scratch;
  why.clear();
  const char* const kNewBrain = " This change starts a new brain.";

  if (saved.kind != now.obs.kind) {
    why = std::string("The AI used to see ") + kind_phrase(saved.kind) + " and now sees " +
          kind_phrase(now.obs.kind) + "." + kNewBrain;
    return Compat::Incompatible;
  }
  std::string note;
  if (saved.kind == ObsSpec::Kind::Numbers) {
    if (saved.d != now.obs.d) {
      why = "see() used to return " + std::to_string(saved.d) + " numbers and now returns " +
            std::to_string(now.obs.d) + "." + kNewBrain;
      return Compat::Incompatible;
    }
    if (saved.inputs != now.obs.names) {
      why = "The names of the numbers changed (they were: " + join(saved.inputs) +
            "; now: " + join(now.obs.names) + ")." + kNewBrain;
      return Compat::Incompatible;
    }
  } else {
    const Geometry g = choose_geometry(now.obs.h, now.obs.w);
    if (g.small != saved.small_picture) {
      const std::string was = saved.small_picture ? size_text(saved.in_h, saved.in_w) : "bigger than 32 pixels";
      why = "The picture changed size (it was " + was + ", now it is " + size_text(now.obs.h, now.obs.w) +
            "). Small pictures (up to 32 pixels) and bigger ones need different brains." + kNewBrain;
      return Compat::Incompatible;
    }
    if (g.small && (now.obs.h != saved.in_h || now.obs.w != saved.in_w)) {
      note = "The picture is now " + size_text(now.obs.h, now.obs.w) + " but this brain learned on " +
             size_text(saved.in_h, saved.in_w) + " pictures, so Vyrn resizes it to " +
             size_text(saved.in_h, saved.in_w) + ".";
    }
  }

  const auto& was = saved.choices;
  const auto& is = now.choices;
  if (is == was) {
    why = note;
    return Compat::Same;
  }
  if (is.size() > was.size() && std::equal(was.begin(), was.end(), is.begin())) {
    std::vector<std::string> added(is.begin() + static_cast<std::ptrdiff_t>(was.size()), is.end());
    why = "New choices were added at the end (" + join(added) + ").";
    if (!note.empty()) why += " " + note;
    return Compat::AppendedChoices;
  }
  std::vector<std::string> removed;
  for (const auto& name : was) {
    if (std::find(is.begin(), is.end(), name) == is.end()) removed.push_back(name);
  }
  std::ostringstream os;
  if (removed.empty() && std::is_permutation(was.begin(), was.end(), is.begin(), is.end())) {
    os << "The choices were reordered (they were: " << join(was) << "; now: " << join(is) << ").";
  } else if (!removed.empty()) {
    os << (removed.size() == 1 ? "The choice " : "The choices ") << join(removed)
       << (removed.size() == 1 ? " was" : " were") << " removed or renamed (choices now: " << join(is) << ").";
  } else {
    os << "The choices changed (they were: " << join(was) << "; now: " << join(is)
       << "). New choices can only be added at the end.";
  }
  why = os.str() + kNewBrain;
  return Compat::Incompatible;
}

}  // namespace vyrn::brain
