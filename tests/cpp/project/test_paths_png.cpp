// Path safety (safe_join, is_inside) and PNG encoding (checked by decoding with Python's Pillow).
#include "helpers.h"
#include "vyrn/project.h"

#include <gtest/gtest.h>

#include "platform_test.h"

#include <cstdio>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace project_test;

// ---- safe_join ------------------------------------------------------------------------------------------------

TEST(SafeJoin, AcceptsPlainFileNames) {
  EXPECT_EQ(vyrn::safe_join("/home/me/Vyrn/Game", "points.py"), "/home/me/Vyrn/Game/points.py");
  EXPECT_EQ(vyrn::safe_join("/home/me/Vyrn/Game/", "inputs.py"), "/home/me/Vyrn/Game/inputs.py");
  EXPECT_EQ(vyrn::safe_join("/p", "my file.py"), "/p/my file.py");
  EXPECT_EQ(vyrn::safe_join("/p", "a..b.py"), "/p/a..b.py");
  EXPECT_EQ(vyrn::safe_join("/p", "caf\xc3\xa9.py"), "/p/caf\xc3\xa9.py");
}

TEST(SafeJoin, RefusesEverythingElse) {
  const std::vector<std::string> bad = {
      "", "../x", "..", ".", "a/b", "/etc/passwd", "sub/", ".hidden", ".vyrn-lock", "a\\b", "..\\x",
      std::string("a\0b", 3), std::string("\0", 1), "line\nbreak.py", "tab\t.py", std::string(256, 'x')};
  for (const std::string& name : bad) {
    try {
      vyrn::safe_join("/p", name);
      ADD_FAILURE() << "accepted \"" << name << "\"";
    } catch (const std::invalid_argument& e) {
      const std::string message = e.what();
      EXPECT_FALSE(message.empty());
      EXPECT_EQ(message.find('\0'), std::string::npos);
      EXPECT_EQ(message.find('\n'), std::string::npos) << message;
    }
  }
  EXPECT_THROW(vyrn::safe_join("", "points.py"), std::invalid_argument);
  EXPECT_NO_THROW(vyrn::safe_join("/p", std::string(255, 'x')));
}

// ---- is_inside ----------------------------------------------------------------------------------------------------

TEST(IsInside, PlainPaths) {
  TempDir tmp;
  const fs::path dir = fs::path(tmp.path()) / "project";
  write_file(dir / "inputs.py", "");
  write_file(fs::path(tmp.path()) / "project2" / "x.py", "");
  EXPECT_TRUE(vyrn::is_inside(dir.string(), dir.string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string() + "/", dir.string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), dir.string() + "/"));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (dir / "inputs.py").string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (dir / "new" / "deeper" / "file.py").string()));   // not there yet
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (dir / "new" / ".." / "file.py").string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / ".." / "project2" / "x.py").string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "new" / ".." / ".." / "x").string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (fs::path(tmp.path()) / "project2").string()));   // common prefix
  EXPECT_FALSE(vyrn::is_inside(dir.string(), tmp.path()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), "/etc/passwd"));
  EXPECT_FALSE(vyrn::is_inside("", dir.string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), ""));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), dir.string() + std::string("/a\0b", 4)));
  EXPECT_TRUE(vyrn::is_inside("/", "/anything/at/all"));
}

TEST(IsInside, SymlinksAreResolved) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  TempDir tmp;
  const fs::path t = tmp.path();
  const fs::path dir = t / "project";
  const fs::path outside = t / "outside";
  write_file(dir / "sub" / "inputs.py", "");
  write_file(outside / "deep" / "secret.txt", "");
  fs::create_symlink(outside, dir / "out");                 // inside, pointing out
  fs::create_symlink(outside / "deep", dir / "out-deep");
  fs::create_symlink(dir / "sub", t / "into");              // outside, pointing in
  fs::create_symlink(dir, t / "project-link");              // another name for the project
  fs::create_symlink(dir / "sub" / "inputs.py", dir / "ok-link.py");

  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "out").string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "out" / "deep" / "secret.txt").string()));
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "out" / "new-file.py").string()));   // missing, behind a link
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (t / "into" / "inputs.py").string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (t / "into" / "missing.py").string()));
  EXPECT_TRUE(vyrn::is_inside((t / "project-link").string(), (dir / "sub" / "inputs.py").string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (t / "project-link" / "sub").string()));
  EXPECT_TRUE(vyrn::is_inside(dir.string(), (dir / "ok-link.py").string()));
  // "..": after a link the kernel goes up from the link's target, not from the folder holding the link.
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "out-deep" / ".." / "x.py").string()));
  // A missing folder, "..", then a link out: once the folder exists that path leads outside.
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "missing" / ".." / "out" / "x.py").string()));
  // A link that points at itself can't be resolved: refused rather than guessed.
  fs::create_symlink(dir / "loop", dir / "loop");
  EXPECT_FALSE(vyrn::is_inside(dir.string(), (dir / "loop" / "x.py").string()));
}

// ---- png_encode -------------------------------------------------------------------------------------------------

namespace {

struct Picture {
  int w = 0, h = 0, scale = 1;
  std::vector<uint8_t> rgb;
};

Picture random_picture(int w, int h, int scale, uint32_t seed) {
  Picture p{w, h, scale, std::vector<uint8_t>(static_cast<size_t>(w) * h * 3)};
  std::mt19937 rng(seed);
  for (auto& v : p.rgb) v = static_cast<uint8_t>(rng());
  return p;
}

// Nearest-neighbour enlargement, written independently of the encoder.
std::vector<uint8_t> enlarge(const Picture& p) {
  const int w = p.w * p.scale, h = p.h * p.scale;
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < 3; ++c) {
        out[(static_cast<size_t>(y) * w + x) * 3 + c] =
            p.rgb[(static_cast<size_t>(y / p.scale) * p.w + x / p.scale) * 3 + c];
      }
    }
  }
  return out;
}

std::string hex(const std::vector<uint8_t>& bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}

// Checks every chunk CRC with Python's zlib, then decodes with Pillow. One output line per file:
// "<mode> <width> <height> <pixels as hex>", or "ERROR <message>".
constexpr const char* kDecoder = R"PY(import struct, sys, zlib
from PIL import Image
for path in sys.argv[1:]:
    try:
        data = open(path, "rb").read()
        if data[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError("bad signature")
        pos, kinds = 8, []
        while pos < len(data):
            (n,) = struct.unpack(">I", data[pos:pos + 4])
            kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
            (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
            if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
                raise ValueError("bad CRC in " + kind.decode())
            kinds.append(kind.decode())
            pos += 12 + n
        if kinds[0] != "IHDR" or kinds[-1] != "IEND" or "IDAT" not in kinds:
            raise ValueError("bad chunks " + " ".join(kinds))
        with Image.open(path) as im:
            im.load()
            print(im.mode, im.size[0], im.size[1], im.tobytes().hex(), len(kinds))
    except Exception as e:
        print("ERROR", path, e)
)PY";

std::vector<std::string> decode_with_pillow(const TempDir& tmp, const std::vector<std::string>& files) {
  const std::string script = tmp / "decode.py";
  write_file(script, kDecoder);
  std::string command = "/usr/bin/python3 -I '" + script + "'";
  for (const auto& f : files) command += " '" + f + "'";
  std::vector<std::string> lines;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (!pipe) return lines;
  std::string text;
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, pipe)) > 0) text.append(buf, n);
  ::pclose(pipe);
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

}  // namespace

TEST(Png, PillowDecodesTheExactPixels) {
  if (!VYRN_TEST_LINUX) GTEST_SKIP() << "Linux only: the reference decoder is /usr/bin/python3 with apt's Pillow";
  if (::access("/usr/bin/python3", X_OK) != 0) GTEST_SKIP() << "/usr/bin/python3 is missing";
  TempDir tmp;
  std::vector<Picture> pictures = {
      Picture{1, 1, 1, {255, 0, 0}},
      Picture{3, 2, 1, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}},
      random_picture(10, 10, 1, 1),
      random_picture(400, 300, 1, 2),   // incompressible: several IDAT chunks
      random_picture(10, 10, 4, 3),     // a 10x10 canvas shown large
      random_picture(7, 1, 3, 4),
      random_picture(1, 7, 5, 5),
  };
  Picture gradient{64, 48, 2, std::vector<uint8_t>(64 * 48 * 3)};
  for (int y = 0; y < 48; ++y) {
    for (int x = 0; x < 64; ++x) {
      uint8_t* px = &gradient.rgb[(static_cast<size_t>(y) * 64 + x) * 3];
      px[0] = static_cast<uint8_t>(x * 4);
      px[1] = static_cast<uint8_t>(y * 5);
      px[2] = static_cast<uint8_t>((x * y) & 255);
    }
  }
  pictures.push_back(gradient);

  std::vector<std::string> files;
  for (size_t i = 0; i < pictures.size(); ++i) {
    const Picture& p = pictures[i];
    const std::vector<uint8_t> png = vyrn::png_encode(p.w, p.h, p.rgb, p.scale);
    const std::string file = tmp / ("picture-" + std::to_string(i) + ".png");
    write_file(file, std::string(png.begin(), png.end()));
    files.push_back(file);
  }
  const std::vector<std::string> lines = decode_with_pillow(tmp, files);
  ASSERT_EQ(lines.size(), pictures.size()) << "Python with Pillow could not decode the pictures";
  for (size_t i = 0; i < pictures.size(); ++i) {
    const Picture& p = pictures[i];
    std::istringstream fields(lines[i]);
    std::string mode, pixels;
    int w = 0, h = 0, chunks = 0;
    fields >> mode >> w >> h >> pixels >> chunks;
    ASSERT_EQ(mode, "RGB") << lines[i].substr(0, 300);
    EXPECT_EQ(w, p.w * p.scale) << i;
    EXPECT_EQ(h, p.h * p.scale) << i;
    EXPECT_TRUE(pixels == hex(enlarge(p))) << "picture " << i << " decodes to other pixels";
    if (i == 3) {
      EXPECT_GE(chunks, 4) << "IHDR, 2+ IDAT, IEND";
    }
  }
}

TEST(Png, HeaderAndSize) {
  const std::vector<uint8_t> png = vyrn::png_encode(10, 10, std::vector<uint8_t>(300, 128), 8);
  ASSERT_GT(png.size(), 33u);
  EXPECT_EQ(std::vector<uint8_t>(png.begin(), png.begin() + 8),
            (std::vector<uint8_t>{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}));
  EXPECT_EQ(std::string(png.begin() + 12, png.begin() + 16), "IHDR");
  const auto u32 = [&](size_t at) {
    return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
  };
  EXPECT_EQ(u32(16), 80u);   // width
  EXPECT_EQ(u32(20), 80u);   // height
  EXPECT_EQ(png[24], 8);     // bit depth
  EXPECT_EQ(png[25], 2);     // RGB
  EXPECT_EQ(std::string(png.end() - 8, png.end() - 4), "IEND");
  // Enlarged pictures stay small: repeated rows filter to zeros. (Deflate can't go below ~1/1000 of the raw size.)
  EXPECT_LT(vyrn::png_encode(100, 100, std::vector<uint8_t>(30000, 77), 8).size(), 8000u);   // 1.9 MB raw
  const Picture canvas = random_picture(10, 10, 32, 7);                                     // 307 KB raw
  EXPECT_LT(vyrn::png_encode(canvas.w, canvas.h, canvas.rgb, canvas.scale).size(), 4000u);
}

TEST(Png, RefusesBadInput) {
  const std::vector<uint8_t> one(3, 0);
  EXPECT_THROW(vyrn::png_encode(0, 1, {}), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(1, 0, {}), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(-1, 1, one), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(1, 1, one, 0), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(1, 1, one, -2), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(1, 1, std::vector<uint8_t>(2, 0)), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(2, 1, one), std::invalid_argument);
  EXPECT_THROW(vyrn::png_encode(1, 1, std::vector<uint8_t>(4, 0)), std::invalid_argument);
  const std::vector<uint8_t> big(1000 * 1000 * 3, 0);
  EXPECT_THROW(vyrn::png_encode(1000, 1000, big, 70), std::invalid_argument);   // 70000 pixels wide
  EXPECT_THROW(vyrn::png_encode(1000, 1000, big, 9), std::invalid_argument);    // 81 M pixels
  EXPECT_NO_THROW(vyrn::png_encode(1, 1, one, 1000));
}
