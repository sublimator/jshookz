#include "account/account_json.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace jshookz::provider::types {
namespace {

constexpr char xrplAlphabet[] =
    "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz";
static_assert(sizeof(xrplAlphabet) == 59);

constexpr std::uint32_t sha256Constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::uint32_t rotateRight(std::uint32_t value,
                                    std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

std::uint32_t readBigEndian32(std::uint8_t const *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
         (static_cast<std::uint32_t>(bytes[1]) << 16u) |
         (static_cast<std::uint32_t>(bytes[2]) << 8u) |
         static_cast<std::uint32_t>(bytes[3]);
}

void writeBigEndian32(std::uint8_t *bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 24u);
  bytes[1] = static_cast<std::uint8_t>(value >> 16u);
  bytes[2] = static_cast<std::uint8_t>(value >> 8u);
  bytes[3] = static_cast<std::uint8_t>(value);
}

// Both Base58Check hashes in this seam are one-block SHA-256 messages: the
// first is 21 bytes (version + AccountID), and the second is 32 bytes.
void sha256OneBlock(std::uint8_t const *input, std::uint32_t length,
                    std::uint8_t output[32]) noexcept {
  std::uint8_t block[64]{};
  std::memcpy(block, input, length);
  block[length] = 0x80;
  std::uint64_t const bitLength = static_cast<std::uint64_t>(length) * 8u;
  for (std::uint32_t index = 0; index < 8; ++index)
    block[63u - index] = static_cast<std::uint8_t>(bitLength >> (index * 8u));

  std::uint32_t words[64];
  for (std::uint32_t index = 0; index < 16; ++index)
    words[index] = readBigEndian32(block + index * 4u);
  for (std::uint32_t index = 16; index < 64; ++index) {
    std::uint32_t const s0 = rotateRight(words[index - 15], 7) ^
                             rotateRight(words[index - 15], 18) ^
                             (words[index - 15] >> 3u);
    std::uint32_t const s1 = rotateRight(words[index - 2], 17) ^
                             rotateRight(words[index - 2], 19) ^
                             (words[index - 2] >> 10u);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  std::uint32_t state[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];
  for (std::uint32_t index = 0; index < 64; ++index) {
    std::uint32_t const sum1 =
        rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    std::uint32_t const choose = (e & f) ^ (~e & g);
    std::uint32_t const temporary1 =
        h + sum1 + choose + sha256Constants[index] + words[index];
    std::uint32_t const sum0 =
        rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    std::uint32_t const majority = (a & b) ^ (a & c) ^ (b & c);
    std::uint32_t const temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
  for (std::uint32_t index = 0; index < 8; ++index)
    writeBigEndian32(output + index * 4u, state[index]);
}

bool encodeBase58Checked(std::uint8_t const bytes[25],
                         AccountIDClassicString &output) noexcept {
  std::uint8_t digits[accountIDClassicMaxChars]{};
  std::uint32_t leadingZeros = 0;
  while (leadingZeros < 25 && bytes[leadingZeros] == 0)
    ++leadingZeros;

  for (std::uint32_t inputIndex = leadingZeros; inputIndex < 25; ++inputIndex) {
    std::uint32_t carry = bytes[inputIndex];
    for (std::uint32_t digit = accountIDClassicMaxChars; digit > 0; --digit) {
      std::uint32_t const value =
          static_cast<std::uint32_t>(digits[digit - 1]) * 256u + carry;
      digits[digit - 1] = static_cast<std::uint8_t>(value % 58u);
      carry = value / 58u;
    }
    if (carry != 0)
      return false;
  }

  std::uint32_t firstDigit = 0;
  while (firstDigit < accountIDClassicMaxChars && digits[firstDigit] == 0)
    ++firstDigit;
  std::uint32_t const significantDigits = accountIDClassicMaxChars - firstDigit;
  if (leadingZeros > accountIDClassicMaxChars - significantDigits)
    return false;

  std::uint32_t position = 0;
  for (; position < leadingZeros; ++position)
    output.chars[position] = xrplAlphabet[0];
  for (; firstDigit < accountIDClassicMaxChars; ++firstDigit)
    output.chars[position++] = xrplAlphabet[digits[firstDigit]];
  output.chars[position] = '\0';
  output.length = position;
  return position != 0;
}

} // namespace

bool encodeAccountIDClassic(std::uint8_t const *bytes, std::uint32_t length,
                            AccountIDClassicString *output) noexcept {
  if (output == nullptr)
    return false;
  std::memset(output, 0, sizeof(*output));
  if (bytes == nullptr || length != accountIDPayloadBytes)
    return false;

  std::uint8_t checked[25]{};
  std::memcpy(checked + 1, bytes, accountIDPayloadBytes);
  std::uint8_t firstHash[32];
  std::uint8_t secondHash[32];
  sha256OneBlock(checked, 21, firstHash);
  sha256OneBlock(firstHash, 32, secondHash);
  std::memcpy(checked + 21, secondHash, 4);
  return encodeBase58Checked(checked, *output);
}

JSValue makeAccountIDCanonicalJSONString(JSContext *ctx,
                                         std::uint8_t const *bytes,
                                         std::uint32_t length) noexcept {
  AccountIDClassicString encoded{};
  if (!encodeAccountIDClassic(bytes, length, &encoded))
    return JS_ThrowInternalError(ctx,
                                 "AccountID JSON requires exactly 20 bytes");
  return JS_NewStringLen(ctx, encoded.chars, encoded.length);
}

} // namespace jshookz::provider::types
