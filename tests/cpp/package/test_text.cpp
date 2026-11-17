// Text helpers and SHA-256 (package/text.h).
#include "package/text.h"

#include <gtest/gtest.h>

using namespace vyrn::package_detail;

TEST(PackageText, Sha256KnownVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  EXPECT_EQ(sha256_hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(PackageText, Utf8Validation) {
  EXPECT_TRUE(valid_utf8("plain"));
  EXPECT_TRUE(valid_utf8("Caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x9A\x80"));
  EXPECT_FALSE(valid_utf8("\xC0\xAF"));          // overlong '/'
  EXPECT_FALSE(valid_utf8("\xED\xA0\x80"));      // surrogate
  EXPECT_FALSE(valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  EXPECT_FALSE(valid_utf8("abc\xE6\x97"));       // cut off
  EXPECT_FALSE(valid_utf8("\xFF"));
}

TEST(PackageText, CleanTextReplacesAndCuts) {
  EXPECT_EQ(clean_text("  hi\x01 there\n ", 100), "hi there");
  EXPECT_EQ(clean_text("line1\nline2", 100, true), "line1\nline2");
  EXPECT_EQ(clean_text("bad\xFFtext", 100), "bad\xEF\xBF\xBDtext");
  EXPECT_EQ(clean_text("abc\xE2\x80\xAE" "def", 100), "abcdef");   // right-to-left override removed
  EXPECT_EQ(clean_text("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 3), "\xC3\xA9\xC3\xA9\xC3\xA9");
}

TEST(PackageText, FoldCase) {
  EXPECT_EQ(fold_case("README.MD"), fold_case("readme.md"));
  EXPECT_EQ(fold_case("\xC3\x89t\xC3\xA9"), fold_case("\xC3\xA9T\xC3\x89"));   // Été / éTÉ
  EXPECT_EQ(fold_case("\xD0\x9F\xD1\x80\xD0\xB8"), fold_case("\xD0\xBF\xD0\xA0\xD0\x98"));   // При / пРИ
  EXPECT_NE(fold_case("a.py"), fold_case("b.py"));
}

TEST(PackageText, HumanSize) {
  EXPECT_EQ(human_size(1), "1 byte");
  EXPECT_EQ(human_size(412 * 1024), "412 KB");
  EXPECT_EQ(human_size(3250586), "3.1 MB");
  EXPECT_EQ(human_size(32u << 20), "32 MB");
  EXPECT_EQ(human_size(1u << 30), "1 GB");
}
