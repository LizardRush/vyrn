// The ZIP subset (docs/dev/package.md section 2): Vyrn's writer, its reader, compatibility with Python's zipfile
// and unzip, and every refusal, using hand-built archives.
#include "helpers.h"
#include "package/zip.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>

using namespace pkg_test;
namespace pd = vyrn::package_detail;

namespace {

std::string refusal(const std::string& bytes, pd::ZipLimits limits = {}) {
  try {
    const pd::ZipReader zip(bytes, limits);
    for (const pd::ZipEntry& e : zip.entries()) zip.read(e);
  } catch (const pd::ZipError& e) {
    return e.what();
  }
  return "";
}

RawZip one(RawEntry e) {
  RawZip z;
  RawEntry mime;
  mime.name = "mimetype";
  mime.data = "application/x-vyrn-project";
  z.entries.push_back(mime);
  z.entries.push_back(std::move(e));
  return z;
}

RawEntry entry(const std::string& name, const std::string& data = "print('hi')\n", bool deflate = false) {
  RawEntry e;
  e.name = name;
  e.data = data;
  e.deflate = deflate;
  return e;
}

bool have(const char* tool) {
  const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

}  // namespace

TEST(PackageZip, WriterOutputReadsBack) {
  std::string bytes;
  pd::ZipWriter zip([&](const char* d, size_t n) { bytes.append(d, n); }, 1763061600);
  zip.add("mimetype", "application/x-vyrn-brain", false);
  zip.add("vyrn-brain.json", "{\"a\": 1}", true);
  zip.add("model.safetensors", std::string(1000, '\x07'), false);
  zip.add("project/sub/naïve.py", std::string(5000, 'x'), true);
  zip.add("empty.txt", "", true);
  zip.finish();
  // Bytes 30.. are "mimetype" + the type (the desktop's magic rule).
  EXPECT_EQ(bytes.substr(30, 8 + 24), "mimetypeapplication/x-vyrn-brain");
  const pd::ZipReader reader(bytes, {});
  ASSERT_EQ(reader.entries().size(), 5u);
  EXPECT_EQ(reader.first_name(), "mimetype");
  EXPECT_EQ(reader.read(reader.entries()[1]), "{\"a\": 1}");
  EXPECT_EQ(reader.read(reader.entries()[2]), std::string(1000, '\x07'));
  EXPECT_EQ(reader.entries()[3].method, 8);
  EXPECT_EQ(reader.read(reader.entries()[3]), std::string(5000, 'x'));
  EXPECT_EQ(reader.read(reader.entries()[4]), "");
  for (const auto& e : reader.entries()) EXPECT_TRUE(e.flags & 0x0800) << e.name;
}

TEST(PackageZip, PythonZipfileAndUnzipReadVyrnArchives) {
  TempDir dir;
  std::string bytes;
  pd::ZipWriter zip([&](const char* d, size_t n) { bytes.append(d, n); }, 1763061600);
  zip.add("mimetype", "application/x-vyrn-project", false);
  zip.add("vyrn-project.json", "{}", true);
  zip.add("project/pictures/日本.png", tiny_png(), false);
  zip.add("project/inputs.py", "def see(world):\n    return [1]\n", true);
  zip.finish();
  write_file(dir / "a.vrlp", bytes);
  if (have("python3")) {
    const std::string script = dir / "check.py";
    write_file(script,
               "import sys, zipfile\n"
               "z = zipfile.ZipFile(sys.argv[1])\n"
               "assert z.testzip() is None\n"
               "names = z.namelist()\n"
               "assert names[0] == 'mimetype' and z.getinfo('mimetype').compress_type == 0, names\n"
               "assert 'project/pictures/日本.png' in names, names\n"
               "assert z.read('project/inputs.py').startswith(b'def see')\n"
               "assert all(i.external_attr >> 16 == 0o100644 for i in z.infolist())\n");
    EXPECT_EQ(std::system(("python3 -I " + script + " " + (dir / "a.vrlp")).c_str()), 0);
  }
  if (have("unzip")) {
    EXPECT_EQ(std::system(("unzip -tq " + (dir / "a.vrlp") + " >/dev/null").c_str()), 0);
  }
}

TEST(PackageZip, ReadsWhatZipToolsMakeFromIt) {
  if (!have("python3")) GTEST_SKIP() << "python3 is missing";
  TempDir dir;
  const std::string script = dir / "make.py";
  // A person unzipped a package, edited it and zipped it again: folder entries, deflated entries, a comment and
  // data descriptors (zipfile writes them when the output isn't seekable).
  write_file(script,
             "import sys, zipfile, io\n"
             "class NoSeek(io.RawIOBase):\n"
             "    def __init__(self, f): self.f = f\n"
             "    def writable(self): return True\n"
             "    def write(self, b): return self.f.write(b)\n"
             "with open(sys.argv[1], 'wb') as raw:\n"
             "    with zipfile.ZipFile(NoSeek(raw), 'w') as z:\n"
             "        z.writestr(zipfile.ZipInfo('mimetype'), 'application/x-vyrn-project')\n"
             "        z.writestr('project/', '')\n"
             "        z.writestr('project/inputs.py', 'x = 1\\n' * 100, compress_type=zipfile.ZIP_DEFLATED)\n"
             "        z.comment = b'edited by hand'\n");
  ASSERT_EQ(std::system(("python3 -I " + script + " " + (dir / "b.zip")).c_str()), 0);
  const std::string bytes = read_file(dir / "b.zip");
  EXPECT_EQ(refusal(bytes), "");
  const pd::ZipReader zip(bytes, {});
  ASSERT_EQ(zip.entries().size(), 2u);   // the folder entry is ignored
  EXPECT_EQ(zip.read(*zip.find("project/inputs.py")), [] {
    std::string s;
    for (int i = 0; i < 100; ++i) s += "x = 1\n";
    return s;
  }());
  if (have("zip")) {
    // Info-ZIP's zip adds its own extra fields (time stamps, uid/gid): they are skipped.
    write_file(dir / "w/mimetype", "application/x-vyrn-project");
    write_file(dir / "w/project/points.py", "def points(world):\n    return 1\n");
    ASSERT_EQ(std::system(("cd " + (dir / "w") + " && zip -q -0 ../c.zip mimetype && zip -q -r ../c.zip project").c_str()),
              0);
    EXPECT_EQ(refusal(read_file(dir / "c.zip")), "");
  }
}

TEST(PackageZip, DataDescriptorsAreAcceptedWhenTheyAgree) {
  RawEntry e = entry("project/a.py", std::string(300, 'a'), true);
  e.descriptor = true;
  EXPECT_EQ(refusal(one(e).build()), "");
}

TEST(PackageZip, RefusesDangerousNames) {
  struct Case {
    std::string name, expect;
  };
  const std::vector<Case> cases = {
      {"../evil.py", "leads outside"},
      {"project/../../evil.py", "leads outside"},
      {"/etc/passwd", "absolute path"},
      {"project\\evil.py", "backslash"},
      {"C:evil.py", "colon"},
      {"project/a\x01.py", "control character"},
      {"project//a.py", "empty folder name"},
      {"project/./a.py", "\".\" folder"},
      {".hidden", "hidden"},
      {"project/.git/config", "hidden"},
      {"project/" + std::string(256, 'a'), "longer than 255"},
      {std::string(1100, 'a'), "longer than 1024"},
      {"project/bad\xFF.py", "UTF-8"},
  };
  for (const Case& c : cases) {
    const std::string why = refusal(one(entry(c.name)).build());
    EXPECT_NE(why.find(c.expect), std::string::npos) << c.name << " -> " << why;
  }
  // A folder entry for a hidden folder is refused too; a plain one is ignored.
  EXPECT_NE(refusal(one(entry("project/.git/", "")).build()).find("hidden"), std::string::npos);
  EXPECT_EQ(refusal(one(entry("project/pictures/", "")).build()), "");
  EXPECT_NE(refusal(one(entry("project/pictures/", "data")).build()).find("holds data"), std::string::npos);
}

TEST(PackageZip, RefusesNamesThatOnlyDifferInCase) {
  RawZip z = one(entry("project/Points.py"));
  z.entries.push_back(entry("project/points.py"));
  EXPECT_NE(refusal(z.build()).find("upper and lower case"), std::string::npos);
  RawZip same = one(entry("project/a.py"));
  same.entries.push_back(entry("project/a.py"));
  EXPECT_NE(refusal(same.build()).find("upper and lower case"), std::string::npos);
  RawZip unicode = one(entry("project/\xC3\x89T\xC3\x89.txt"));   // ÉTÉ
  unicode.entries.push_back(entry("project/\xC3\xA9t\xC3\xA9.txt"));   // été
  EXPECT_NE(refusal(unicode.build()).find("upper and lower case"), std::string::npos);
  RawZip file_and_folder = one(entry("project/data"));
  file_and_folder.entries.push_back(entry("project/Data/x.txt"));
  EXPECT_NE(refusal(file_and_folder.build()).find("file and for a folder"), std::string::npos);
}

TEST(PackageZip, RefusesOverlappingEntries) {
  // b.txt's local header and data sit inside a.txt's stored data: two entries share bytes.
  RawEntry inner = entry("b.txt", "hello");
  RawZip just_b;
  just_b.entries.push_back(inner);
  const std::string built = just_b.build();
  const std::string b_local = built.substr(0, 30 + 5 + 5);   // local header + name + data
  RawEntry outer = entry("a.txt", b_local);
  RawEntry b = inner;
  b.cd_only = true;
  b.cd_offset = 30 + 8 + 30 + 26 + 5;   // after mimetype (30 + 8 + 26) and a.txt's header (30 + 5)
  RawZip z;
  RawEntry mime = entry("mimetype", "application/x-vyrn-project");
  z.entries = {mime, outer, b};
  EXPECT_NE(refusal(z.build()).find("share the same bytes"), std::string::npos) << refusal(z.build());
  // Two records for one local header.
  RawEntry again = entry("a.txt", b_local);
  again.cd_only = true;
  again.cd_offset = 30 + 8 + 26;
  RawZip twice;
  twice.entries = {mime, outer, again};
  EXPECT_NE(refusal(twice.build()), "");
}

TEST(PackageZip, RefusesBadChecksumsAndLyingSizes) {
  RawEntry bad_crc = entry("project/a.py", "abc");
  bad_crc.crc = 12345;
  EXPECT_NE(refusal(one(bad_crc).build()).find("checksum"), std::string::npos);

  RawEntry more = entry("project/a.py", std::string(5000, 'z'), true);
  more.cd_usize = 100;   // says 100 bytes, inflates to 5000: never followed
  more.local_usize = 100;
  more.crc = crc32_of(std::string(100, 'z'));
  EXPECT_NE(refusal(one(more).build()).find("damaged"), std::string::npos);

  RawEntry less = entry("project/a.py", std::string(50, 'z'), true);
  less.cd_usize = 5000;
  less.local_usize = 5000;
  EXPECT_NE(refusal(one(less).build()).find("damaged"), std::string::npos);

  RawEntry stored = entry("project/a.py", "abcdef");
  stored.cd_usize = 3;
  stored.local_usize = 3;
  EXPECT_NE(refusal(one(stored).build()).find("don't add up"), std::string::npos);

  RawEntry mismatch = entry("project/a.py", "abcdef");
  mismatch.local_csize = 4;   // local header and central directory disagree
  EXPECT_NE(refusal(one(mismatch).build()), "");

  RawEntry name_mismatch = entry("project/a.py");
  name_mismatch.local_name = "project/b.py";
  EXPECT_NE(refusal(one(name_mismatch).build()).find("doesn't match"), std::string::npos);

  RawEntry beyond = entry("project/a.py", "abc");
  beyond.cd_csize = 1u << 30;
  beyond.cd_usize = 1u << 30;
  beyond.local_csize = 1u << 30;
  beyond.local_usize = 1u << 30;
  EXPECT_NE(refusal(one(beyond).build()), "");

  RawEntry bad_descriptor = entry("project/a.py", "abc");
  bad_descriptor.descriptor = true;
  bad_descriptor.raw_body = "abc";
  bad_descriptor.cd_csize = 2;   // the descriptor after the data says 3
  EXPECT_NE(refusal(one(bad_descriptor).build()), "");

  RawEntry garbage = entry("project/a.py", "abc", true);
  garbage.raw_body = "\xFF\xFF\xFF\xFF";
  EXPECT_NE(refusal(one(garbage).build()).find("damaged"), std::string::npos);
}

TEST(PackageZip, RefusesBombsBeforeInflating) {
  // 100 MB of zeros deflates to about 100 KB: refused from the declared size alone.
  pd::ZipLimits limits;
  RawEntry big = entry("project/zeros.txt", "", true);
  big.raw_body = deflate_raw(std::string(1 << 20, '\0'));
  big.cd_usize = 100u << 20;
  big.local_usize = 100u << 20;
  EXPECT_NE(refusal(one(big).build(), limits).find("bigger than"), std::string::npos);
  // Many entries that add up to too much.
  limits.max_total = 1u << 20;
  RawZip many = one(entry("project/0.txt", std::string(600000, 'a'), true));
  many.entries.push_back(entry("project/1.txt", std::string(600000, 'a'), true));
  EXPECT_NE(refusal(many.build(), limits).find("add up to more than"), std::string::npos);
}

TEST(PackageZip, RefusesZip64EncryptionAndOtherFeatures) {
  RawEntry zip64 = entry("project/a.py");
  zip64.cd_extra = std::string("\x01\x00\x08\x00", 4) + std::string(8, '\0');
  EXPECT_NE(refusal(one(zip64).build()).find("ZIP64"), std::string::npos);
  RawEntry zip64_local = entry("project/a.py");
  zip64_local.local_extra = std::string("\x01\x00\x08\x00", 4) + std::string(8, '\0');
  EXPECT_NE(refusal(one(zip64_local).build()).find("ZIP64"), std::string::npos);
  RawEntry ff = entry("project/a.py");
  ff.cd_usize = 0xFFFFFFFFu;
  EXPECT_NE(refusal(one(ff).build()).find("ZIP64"), std::string::npos);
  RawZip locator = one(entry("project/a.py"));
  locator.zip64_locator = true;
  EXPECT_NE(refusal(locator.build()).find("ZIP64"), std::string::npos);

  RawEntry encrypted = entry("project/a.py");
  encrypted.flags = 0x0801;
  EXPECT_NE(refusal(one(encrypted).build()).find("encrypted"), std::string::npos);
  RawEntry aes = entry("project/a.py");
  aes.method = 99;
  EXPECT_NE(refusal(one(aes).build()).find("encrypted"), std::string::npos);
  RawEntry strong = entry("project/a.py");
  strong.flags = 0x0840;
  EXPECT_NE(refusal(one(strong).build()).find("encrypted"), std::string::npos);
  RawEntry patched = entry("project/a.py");
  patched.flags = 0x0820;
  EXPECT_NE(refusal(one(patched).build()).find("patched"), std::string::npos);
  RawEntry newer = entry("project/a.py");
  newer.version_needed = 63;
  EXPECT_NE(refusal(one(newer).build()).find("newer ZIP reader"), std::string::npos);
  RawEntry bzip2 = entry("project/a.py");
  bzip2.method = 12;
  EXPECT_NE(refusal(one(bzip2).build()).find("compressed in a way"), std::string::npos);
  RawEntry bad_extra = entry("project/a.py");
  bad_extra.cd_extra = std::string("\x55\x54\x20\x00\x01", 5);   // says 32 bytes, has 1
  EXPECT_NE(refusal(one(bad_extra).build()).find("extra information"), std::string::npos);
}

TEST(PackageZip, RefusesBrokenContainers) {
  EXPECT_NE(refusal("").find("isn't a ZIP"), std::string::npos);
  EXPECT_NE(refusal("PK\x03\x04 not really a zip at all, just text").find("isn't a ZIP"), std::string::npos);
  const std::string good = one(entry("project/a.py")).build();
  EXPECT_EQ(refusal(good), "");
  EXPECT_NE(refusal(good.substr(0, good.size() - 5)), "");   // cut off
  EXPECT_NE(refusal(good.substr(10)), "");                   // something in front is missing
  RawZip split = one(entry("project/a.py"));
  split.disk = 1;
  EXPECT_NE(refusal(split.build()).find("several disks"), std::string::npos);
  RawZip count = one(entry("project/a.py"));
  count.count = 3;
  EXPECT_NE(refusal(count.build()), "");
  RawZip commented = one(entry("project/a.py"));
  commented.comment = "made by hand";
  EXPECT_EQ(refusal(commented.build()), "");
  pd::ZipLimits limits;
  limits.max_entries = 2;
  RawZip three = one(entry("project/a.py"));
  three.entries.push_back(entry("project/b.py"));
  EXPECT_NE(refusal(three.build(), limits).find("more than 2 files"), std::string::npos);
  limits.max_file = 100;
  EXPECT_NE(refusal(good + std::string(200, ' '), limits), "");
}

TEST(PackageZip, SymlinkAttributesAreIgnored) {
  // An entry marked as a symlink by its Unix attributes is just a file with that text in it.
  RawZip z = one(entry("project/link.txt", "/etc/passwd"));
  std::string bytes = z.build();
  const size_t cd = bytes.rfind(std::string("\x50\x4b\x01\x02", 4));
  ASSERT_NE(cd, std::string::npos);
  const uint32_t symlink = 0120777u << 16;
  for (int i = 0; i < 4; ++i) bytes[cd + 38 + static_cast<size_t>(i)] = static_cast<char>((symlink >> (8 * i)) & 0xFF);
  const pd::ZipReader zip(bytes, {});
  EXPECT_EQ(zip.read(*zip.find("project/link.txt")), "/etc/passwd");
}
