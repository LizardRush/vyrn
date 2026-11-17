// Helpers for test_package: temp folders, test projects with real (small) brains, and a raw ZIP builder.
#include "helpers.h"

#include "brain/checkpoint.h"
#include "brain/dqn.h"
#include "brain/nets.h"
#include "package/text.h"
#include "package/zip.h"
#include "vyrn/project.h"

#include <nlohmann/json.hpp>
#include <torch/torch.h>
#include <zlib.h>

#include <stdlib.h>
#include "platform_test.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pkg_test {

using json = nlohmann::json;
namespace pd = vyrn::package_detail;

TempDir::TempDir() {
  std::string pattern = (fs::temp_directory_path() / "vyrn-package-test-XXXXXX").string();
  if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
  path_ = pattern;
}

TempDir::~TempDir() {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(path_, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code perm_ec;
    if (!it->is_symlink(perm_ec)) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, perm_ec);
  }
  fs::remove_all(path_, ec);
}

ScopedEnv::ScopedEnv(const char* name, const std::string& value) : name_(name) {
  if (const char* old = std::getenv(name)) old_ = old;
  ::setenv(name, value.c_str(), 1);
}

ScopedEnv::~ScopedEnv() {
  if (old_) {
    ::setenv(name_.c_str(), old_->c_str(), 1);
  } else {
    ::unsetenv(name_.c_str());
  }
}

void write_file(const fs::path& path, const std::string& bytes) {
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool present(const fs::path& path) {
  std::error_code ec;
  return fs::symlink_status(path, ec).type() != fs::file_type::not_found && !ec;
}

std::vector<std::string> tree(const fs::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    out.push_back(fs::relative(it->path(), root).string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string make_project(const std::string& root, const std::string& folder, const std::string& name,
                         const std::string& template_id) {
  const fs::path dir = fs::path(root) / folder;
  write_file(dir / "inputs.py", "def see(world):\n    return [world['x'], world['y'], world['c']]\n");
  write_file(dir / "responses.py", "def left(world):\n    pass\n\ndef right(world):\n    pass\n");
  write_file(dir / "points.py", "def points(world):\n    return 0.0\n");
  write_file(dir / "world.py", "def reset():\n    return {'x': 0, 'y': 0, 'c': 0}\n");
  write_file(dir / "vyrn.json", json{{"name", name}, {"template", template_id}}.dump(2) + "\n");
  return dir.string();
}

std::string shape_json(Net net, const std::vector<std::string>& choices) {
  vyrn::brain::BrainShape s;
  s.choices = choices;
  switch (net) {
    case Net::MLP:
      s.kind = vyrn::brain::ObsSpec::Kind::Numbers;
      s.d = 3;
      s.inputs = {"x", "y", "c"};
      break;
    case Net::SmallCNN:
      s.kind = vyrn::brain::ObsSpec::Kind::Picture;
      s.frames = 4;
      s.in_c = 12;
      s.in_h = 10;
      s.in_w = 10;
      s.small_picture = true;
      break;
    case Net::DER:
      s.kind = vyrn::brain::ObsSpec::Kind::Picture;
      s.frames = 2;
      s.in_c = 6;
      s.in_h = 84;
      s.in_w = 84;
      break;
  }
  json j;
  vyrn::brain::to_json(j, s);
  return j.dump();
}

std::string spec_json(Net net, const std::vector<std::string>& choices) {
  json obs;
  switch (net) {
    case Net::MLP: obs = {{"kind", "numbers"}, {"shape", {3}}, {"names", {"x", "y", "c"}}}; break;
    case Net::SmallCNN: obs = {{"kind", "picture"}, {"shape", {10, 10, 3}}}; break;
    case Net::DER: obs = {{"kind", "picture"}, {"shape", {120, 160, 3}}}; break;
  }
  return json{{"type", "spec"}, {"mode", "sim"}, {"obs", obs}, {"choices", choices}, {"brain", "new"}, {"why", ""}}
      .dump();
}

void add_brain(const std::string& project, const std::string& id, Net net, uint64_t seed, bool with_best,
               const std::vector<std::string>& choices) {
  using namespace vyrn::brain;
  const BrainShape shape = json::parse(shape_json(net, choices)).get<BrainShape>();
  const NetConfig cfg = net_config_for(shape);
  torch::manual_seed(seed);
  DqnConfig dc;
  dc.batch = 4;
  Dqn dqn(dc, cfg);
  dqn.observe_warmup_reward(3.0f);
  Batch b;
  const int64_t batch = 4;
  if (shape.kind == ObsSpec::Kind::Picture) {
    b.obs = torch::randint(0, 256, {batch, int64_t{cfg.in_c} * cfg.in_h * cfg.in_w}, torch::kUInt8);
    b.next_obs = torch::randint(0, 256, {batch, int64_t{cfg.in_c} * cfg.in_h * cfg.in_w}, torch::kUInt8);
  } else {
    b.obs = torch::randn({batch, cfg.d});
    b.next_obs = torch::randn({batch, cfg.d});
  }
  b.actions = torch::randint(0, cfg.n_actions, {batch}, torch::kLong);
  b.returns = torch::randn({batch});
  b.discounts = torch::full({batch}, 0.9f);
  dqn.update(b);

  json meta = json::object();
  meta["format"] = 1;
  meta["vyrn"] = "0.1.0";
  meta["brain"] = id;
  meta["shape"] = shape;
  meta["net"] = to_string(cfg.kind);
  meta["net_description"] = describe(cfg);
  meta["obs"] = json::parse(spec_json(net, choices))["obs"];
  meta["choices"] = choices;
  meta["labels"] = choices;
  json effects = json::array();
  for (const auto& c : choices) effects.push_back("call " + c + "(world)");
  meta["effects"] = effects;
  meta["counters"] = {{"ticks", 6000}, {"rounds", 66}, {"updates", 1}, {"seconds", 12.5}};
  meta["normalizer"] = shape.kind == ObsSpec::Kind::Numbers
                           ? json{{"count", 100.0}, {"mean", {0.5, -1.25, 2.0}}, {"var", {1.0, 0.25, 4.0}}}
                           : json(nullptr);
  meta["best_mean20"] = 0.75;
  meta["saved"] = "2026-11-13T10:00:00";
  const std::string dir = (fs::path(project) / "brains" / id).string();
  if (with_best) save_checkpoint(dir, "best", *dqn.snapshot(), BrainMeta{meta});
  // Latest learned a little more after the best was kept.
  dqn.update(b);
  meta["counters"] = {{"ticks", 7000}, {"rounds", 77}, {"updates", 2}, {"seconds", 14.0}};
  if (!with_best) meta["best_mean20"] = nullptr;
  save_checkpoint(dir, "latest", dqn, BrainMeta{meta});
  std::string why;
  if (!vyrn::merge_settings(project, json{{"brain", id}}.dump(), &why)) throw std::runtime_error(why);
}

namespace {

std::unique_ptr<vyrn::brain::Dqn> load_saved(const std::string& brain_dir, const std::string& which) {
  using namespace vyrn::brain;
  BrainMeta meta;
  if (!read_checkpoint_meta(brain_dir, which, meta)) throw std::runtime_error("no save in " + brain_dir);
  const BrainShape shape = meta.j.at("shape").get<BrainShape>();
  auto dqn = std::make_unique<Dqn>(DqnConfig{}, net_config_for(shape));
  if (!load_checkpoint(brain_dir, which, *dqn, meta)) throw std::runtime_error("can't load " + brain_dir);
  return dqn;
}

}  // namespace

std::map<std::string, std::string> saved_weights(const std::string& brain_dir, const std::string& which) {
  auto dqn = load_saved(brain_dir, which);
  std::map<std::string, std::string> out;
  torch::NoGradGuard no_grad;
  for (const auto& p : dqn->online()->named_parameters(true)) {
    const torch::Tensor t = p.value().detach().to(torch::kFloat).contiguous();
    out[p.key()] = std::string(reinterpret_cast<const char*>(t.data_ptr<float>()), static_cast<size_t>(t.numel()) * 4);
  }
  for (const auto& b : dqn->online()->named_buffers(true)) {
    const torch::Tensor t = b.value().detach().to(torch::kFloat).contiguous();
    out["buffer:" + b.key()] =
        std::string(reinterpret_cast<const char*>(t.data_ptr<float>()), static_cast<size_t>(t.numel()) * 4);
  }
  return out;
}

std::vector<float> saved_q_values(const std::string& brain_dir, const std::string& which) {
  auto dqn = load_saved(brain_dir, which);
  const auto cfg = dqn->net_config();
  torch::manual_seed(42);
  const torch::Tensor x = cfg.kind == vyrn::brain::NetKind::MLP
                              ? torch::randn({5, cfg.d})
                              : torch::randint(0, 256, {5, int64_t{cfg.in_c} * cfg.in_h * cfg.in_w}, torch::kUInt8);
  torch::NoGradGuard no_grad;
  const torch::Tensor q = dqn->online()->forward(x).contiguous();
  return std::vector<float>(q.data_ptr<float>(), q.data_ptr<float>() + q.numel());
}

std::string saved_meta(const std::string& brain_dir, const std::string& which) {
  vyrn::brain::BrainMeta meta;
  if (!vyrn::brain::read_checkpoint_meta(brain_dir, which, meta)) return "";
  return meta.j.dump();
}

int64_t saved_updates(const std::string& brain_dir, const std::string& which) {
  return load_saved(brain_dir, which)->updates();
}

// ---- raw ZIP ----

namespace {

void put16(std::string& out, uint32_t v) {
  out += static_cast<char>(v & 0xFF);
  out += static_cast<char>((v >> 8) & 0xFF);
}

void put32(std::string& out, uint32_t v) {
  put16(out, v & 0xFFFF);
  put16(out, v >> 16);
}

}  // namespace

uint32_t crc32_of(const std::string& data) { return pd::crc32_of(data); }

std::string deflate_raw(const std::string& data) {
  z_stream zs{};
  deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&zs, data.size()) + 16, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

std::string RawZip::build() const {
  std::string out;
  std::vector<std::string> records;
  for (const RawEntry& e : entries) {
    const uint16_t method = e.method ? *e.method : (e.deflate ? 8 : 0);
    const std::string body = e.raw_body ? *e.raw_body : (e.deflate ? deflate_raw(e.data) : e.data);
    const uint32_t crc = e.crc ? *e.crc : crc32_of(e.data);
    const uint32_t csize = static_cast<uint32_t>(body.size()), usize = static_cast<uint32_t>(e.data.size());
    const uint16_t flags = static_cast<uint16_t>(e.flags | (e.descriptor ? 8 : 0));
    const uint32_t offset = static_cast<uint32_t>(out.size());
    if (!e.cd_only) {
      const std::string name = e.local_name ? *e.local_name : e.name;
      put32(out, 0x04034b50);
      put16(out, e.version_needed);
      put16(out, flags);
      put16(out, method);
      put16(out, 0);
      put16(out, 0x5B6D);
      put32(out, e.descriptor ? 0 : crc);
      put32(out, e.local_csize ? *e.local_csize : (e.descriptor ? 0 : csize));
      put32(out, e.local_usize ? *e.local_usize : (e.descriptor ? 0 : usize));
      put16(out, static_cast<uint32_t>(name.size()));
      put16(out, static_cast<uint32_t>(e.local_extra.size()));
      out += name;
      out += e.local_extra;
      out += body;
      if (e.descriptor) {
        put32(out, 0x08074b50);
        put32(out, crc);
        put32(out, csize);
        put32(out, usize);
      }
    }
    std::string cd;
    put32(cd, 0x02014b50);
    put16(cd, (3u << 8) | 20u);
    put16(cd, e.version_needed);
    put16(cd, flags);
    put16(cd, method);
    put16(cd, 0);
    put16(cd, 0x5B6D);
    put32(cd, crc);
    put32(cd, e.cd_csize ? *e.cd_csize : csize);
    put32(cd, e.cd_usize ? *e.cd_usize : usize);
    put16(cd, static_cast<uint32_t>(e.name.size()));
    put16(cd, static_cast<uint32_t>(e.cd_extra.size()));
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put32(cd, 0100644u << 16);
    put32(cd, e.cd_offset ? *e.cd_offset : offset);
    cd += e.name;
    cd += e.cd_extra;
    records.push_back(cd);
  }
  const uint32_t cd_offset = static_cast<uint32_t>(out.size());
  for (const std::string& r : records) out += r;
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;
  if (zip64_locator) {
    put32(out, 0x07064b50);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, 1);
  }
  const uint16_t n = count ? *count : static_cast<uint16_t>(entries.size());
  put32(out, 0x06054b50);
  put16(out, disk);
  put16(out, disk);
  put16(out, n);
  put16(out, n);
  put32(out, cd_size);
  put32(out, cd_offset);
  put16(out, static_cast<uint32_t>(comment.size()));
  out += comment;
  return out;
}

std::vector<std::pair<std::string, std::string>> unzip_all(const std::string& bytes) {
  const pd::ZipReader zip(bytes, pd::ZipLimits{});
  std::vector<std::pair<std::string, std::string>> out;
  for (const pd::ZipEntry& e : zip.entries()) out.emplace_back(e.name, zip.read(e));
  return out;
}

std::string rezip(const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string out;
  pd::ZipWriter zip([&](const char* data, size_t size) { out.append(data, size); }, 1763061600);
  for (const auto& [name, data] : entries) {
    const bool json_file = name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0;
    zip.add(name, data, json_file);
  }
  zip.finish();
  return out;
}

std::string with_entry(const std::string& zip_bytes, const std::string& name, const std::string& data) {
  auto entries = unzip_all(zip_bytes);
  bool found = false;
  for (auto& [n, d] : entries) {
    if (n == name) {
      d = data;
      found = true;
    }
  }
  if (!found) entries.emplace_back(name, data);
  return rezip(entries);
}

std::string entry_of(const std::string& zip_bytes, const std::string& name) {
  for (const auto& [n, d] : unzip_all(zip_bytes)) {
    if (n == name) return d;
  }
  return "";
}

std::string sha256_of(const std::string& data) { return pd::sha256_hex(data); }

std::string tiny_png(int w, int h) {
  const std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 128);
  const std::vector<uint8_t> png = vyrn::png_encode(w, h, rgb);
  return std::string(png.begin(), png.end());
}

}  // namespace pkg_test
