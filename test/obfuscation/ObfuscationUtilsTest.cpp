// Password obfuscation for the credential files (wifi.json, opds.json, koreader.json): the key id
// that says which reader wrote a file, and the plausibility check that keeps a file older firmware
// wrote on another reader from being stamped with this reader's key id. The reader under test has
// the T5S3's MAC (shims/esp_mac.h); "the other reader" is the X3 of the same device check.
#include <ObfuscationUtils.h>
#include <base64.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

namespace {

constexpr uint8_t kOtherReaderKey[6] = {0x70, 0xaf, 0x09, 0x5f, 0x8a, 0x84};
const std::string kOtherReaderKeyId = "70-af-09-5f-8a-84";

// What a reader with `key` writes for `password`: the base64 of the password XORed with its key.
std::string obfuscatedWith(const uint8_t key[6], std::string password) {
  obfuscation::xorTransform(password, key, 6);
  return base64::encode(reinterpret_cast<const uint8_t*>(password.data()), password.size()).c_str();
}

std::string decodeHere(const std::string& encoded, const uint8_t* writerKey = nullptr) {
  bool ok = false;
  return obfuscation::deobfuscateFromBase64(encoded.c_str(), 64, &ok, nullptr, writerKey);
}

}  // namespace

TEST(ObfuscationUtils, RoundTripsUnderThisReadersKey) {
  for (const std::string password : {"hunter2", "WiFi P@ssw0rd!", "Kennwort-Ä-ß-€", "パスワード", "a"}) {
    EXPECT_EQ(decodeHere(obfuscation::obfuscateToBase64(password).c_str()), password);
  }
}

TEST(ObfuscationUtils, TheKeyIdSaysWhichReaderWroteAFile) {
  uint8_t key[6] = {};
  EXPECT_EQ(obfuscation::deviceKeyId(), "dc-b4-d9-2b-ee-d4");
  EXPECT_EQ(obfuscation::passwordWriter(obfuscation::deviceKeyId(), key), obfuscation::PasswordWriter::ThisReader);
  ASSERT_EQ(obfuscation::passwordWriter(kOtherReaderKeyId, key), obfuscation::PasswordWriter::OtherReader);
  EXPECT_EQ(0, memcmp(key, kOtherReaderKey, sizeof(key)));
  EXPECT_EQ(obfuscation::passwordWriter("", key), obfuscation::PasswordWriter::Unknown) << "older firmware";
  EXPECT_EQ(obfuscation::passwordWriter("70-af-09-5f-8a", key), obfuscation::PasswordWriter::Unknown);
  EXPECT_EQ(obfuscation::passwordWriter("70:af:09:5f:8a:84", key), obfuscation::PasswordWriter::Unknown);
}

TEST(ObfuscationUtils, AFileFromAnotherReaderDecodesWithItsKey) {
  const std::string stored = obfuscatedWith(kOtherReaderKey, "opds-secret");
  EXPECT_EQ(decodeHere(stored, kOtherReaderKey), "opds-secret");
  EXPECT_NE(decodeHere(stored), "opds-secret") << "with this reader's key: the right length, the wrong bytes";
}

// --- plausiblePassword ---------------------------------------------------------------------------

TEST(ObfuscationUtils, APasswordSomebodyTypedIsPlausible) {
  for (const std::string password :
       {"hunter2", "correct horse battery staple", "P@ss w0rd!~", "Kennwort-Ä-ß-€", "пароль", "パスワード", "🔑key"}) {
    EXPECT_TRUE(obfuscation::plausiblePassword(password)) << password;
  }
  EXPECT_TRUE(obfuscation::plausiblePassword("")) << "an empty password is nothing to doubt";
}

TEST(ObfuscationUtils, ControlCharactersAndBrokenUtf8AreNot) {
  for (const std::string& bad : {std::string("tab\there"), std::string("new\nline"), std::string("del\x7f"),
                                 std::string("nul\0x", 5), std::string("c1 \xc2\x85"), std::string("lone \x80"),
                                 std::string("cut \xe3\x81"), std::string("overlong \xc0\xaf"),
                                 std::string("surrogate \xed\xa0\x80"), std::string("replacement \xef\xbf\xbd")}) {
    EXPECT_FALSE(obfuscation::plausiblePassword(bad)) << bad;
  }
}

TEST(ObfuscationUtils, ACorrectDecodeIsPlausibleAndAWrongOneIsNot) {
  // The device check's koreader.json case: written by the X3 with no key id, loaded on the T5S3.
  const std::string fromTheX3 = obfuscatedWith(kOtherReaderKey, "kosync-password-1");
  EXPECT_FALSE(obfuscation::plausiblePassword(decodeHere(fromTheX3)));
  EXPECT_TRUE(obfuscation::plausiblePassword(decodeHere(fromTheX3, kOtherReaderKey)));
  // Written here by older firmware.
  EXPECT_TRUE(obfuscation::plausiblePassword(decodeHere(obfuscation::obfuscateToBase64("kosync-password-1").c_str())));
}

// How often a password decoded with the wrong reader's key still looks typed, which is when a file
// another reader wrote without a key id would wrongly be stamped as this reader's. Random printable
// ASCII passwords (letters weighted up) and random reader keys. Two readers' MACs differ in all six
// bytes when their OUIs differ (the X3's 70:af:09, the T5S3's dc:b4:d9): the check almost never
// errs. They differ in only the last three when they share an OUI, and in one or two when they
// come from one batch; then a wrong key leaves most characters alone, and a flipped one is often
// still a letter (XOR 0x20 changes its case), so the check errs far more often. The bounds below
// record that; it is a limit of the check, not of this test.
TEST(ObfuscationUtils, AWrongKeyDecodeIsAlmostNeverPlausible) {
  std::mt19937 rng(20261020);
  const std::string alphabet =
      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ";
  const auto randomPassword = [&](const int minLen, const int maxLen) {
    std::string password(std::uniform_int_distribution<int>(minLen, maxLen)(rng), ' ');
    for (char& c : password) c = alphabet[std::uniform_int_distribution<size_t>(0, alphabet.size() - 1)(rng)];
    return password;
  };
  const auto randomKey = [&](uint8_t key[6]) {
    for (int i = 0; i < 6; i++) key[i] = static_cast<uint8_t>(std::uniform_int_distribution<int>(0, 255)(rng));
  };
  // Fraction of wrong-key decodes judged plausible.
  const auto falseAcceptRate = [&](const int minLen, const int maxLen, const int sharedKeyBytes) {
    constexpr int kTrials = 200000;
    int accepted = 0;
    int trials = 0;
    for (int t = 0; t < kTrials; t++) {
      uint8_t writer[6];
      uint8_t reader[6];
      randomKey(writer);
      randomKey(reader);
      memcpy(reader, writer, sharedKeyBytes);
      if (memcmp(writer, reader, 6) == 0) continue;
      std::string bytes = randomPassword(minLen, maxLen);
      obfuscation::xorTransform(bytes, writer, 6);  // written by the other reader
      obfuscation::xorTransform(bytes, reader, 6);  // decoded here
      trials++;
      if (obfuscation::plausiblePassword(bytes)) accepted++;
    }
    return static_cast<double>(accepted) / trials;
  };

  struct Case {
    int minLen, maxLen;
    int sharedKeyBytes;  // leading MAC bytes the two readers share: 0, 3 (one OUI), 4 or 5 (one batch)
    double bound;        // what the test holds the rate to
  };
  const Case cases[] = {
      // measured with this seed: 0.080 %, 3.26 %, 9.93 %, 29.9 %
      {8, 20, 0, 0.0015},
      {8, 20, 3, 0.04},
      {8, 20, 4, 0.12},
      {8, 20, 5, 0.35},
      // 0.029 %, 2.05 %; 1.55 %, 17.2 %
      {12, 20, 0, 0.0005},
      {12, 20, 3, 0.025},
      {4, 7, 0, 0.02},
      {4, 7, 3, 0.2},
  };
  for (const Case& c : cases) {
    const double rate = falseAcceptRate(c.minLen, c.maxLen, c.sharedKeyBytes);
    std::printf("false accept, %2d-%2d chars, %d of 6 key bytes differ: %7.4f %%\n", c.minLen, c.maxLen,
                6 - c.sharedKeyBytes, rate * 100);
    EXPECT_LT(rate, c.bound) << c.minLen << "-" << c.maxLen << " chars, " << 6 - c.sharedKeyBytes << " bytes differ";
  }
}

// --- resaveUnderThisKey: what the opds.json and koreader.json loaders do with a file -------------

TEST(ObfuscationUtils, AFileFromAnotherReaderThatNamesItIsReKeyed) {
  using obfuscation::PasswordWriter;
  EXPECT_TRUE(obfuscation::resaveUnderThisKey(PasswordWriter::OtherReader, true, true));
  EXPECT_FALSE(obfuscation::resaveUnderThisKey(PasswordWriter::OtherReader, false, true)) << "no password to re-key";
}

TEST(ObfuscationUtils, AFileThatNamesThisReaderStaysAsItIs) {
  using obfuscation::PasswordWriter;
  EXPECT_FALSE(obfuscation::resaveUnderThisKey(PasswordWriter::ThisReader, true, true));
  EXPECT_FALSE(obfuscation::resaveUnderThisKey(PasswordWriter::ThisReader, true, false));
}

// The device check's regression: a file older firmware wrote on the X3, first loaded on the T5S3,
// was stamped with the T5S3's key id over the X3's bytes, and back on the X3 decoded wrongly.
TEST(ObfuscationUtils, AFileWithoutAKeyIdIsStampedOnlyWhenItDecodesPlausiblyHere) {
  using obfuscation::PasswordWriter;
  const std::string writtenHere = obfuscation::obfuscateToBase64("kosync-password-1").c_str();
  const std::string writtenOnTheX3 = obfuscatedWith(kOtherReaderKey, "kosync-password-1");
  EXPECT_TRUE(obfuscation::resaveUnderThisKey(PasswordWriter::Unknown, true,
                                              obfuscation::plausiblePassword(decodeHere(writtenHere))));
  EXPECT_FALSE(obfuscation::resaveUnderThisKey(PasswordWriter::Unknown, true,
                                               obfuscation::plausiblePassword(decodeHere(writtenOnTheX3))));
  EXPECT_FALSE(obfuscation::resaveUnderThisKey(PasswordWriter::Unknown, false, true)) << "nothing to stamp";
}

// Left as it is means byte for byte: a password decoded with the wrong key goes back to the card as
// the bytes it came from, which is what lets its writer still read it.
TEST(ObfuscationUtils, AWrongDecodeSavedAgainGivesBackTheWritersBytes) {
  const std::string writtenOnTheX3 = obfuscatedWith(kOtherReaderKey, "kosync-password-1");
  EXPECT_EQ(obfuscation::obfuscateToBase64(decodeHere(writtenOnTheX3)).c_str(), writtenOnTheX3);
}
