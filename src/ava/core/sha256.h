#pragma once

#include "ava/core/hex.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <string.h>

namespace ava::core {
namespace detail {

inline void sha256_compress(std::array<std::uint32_t, 8>& hash, std::span<std::byte const, 64> block) noexcept
{
  static constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU,
      0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU,
      0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U,
      0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16U; ++index)
  {
    auto const offset = index * 4U;
    words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) | (std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
                   (std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) | std::to_integer<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16U; index < words.size(); ++index)
  {
    auto const s0 = std::rotr(words[index - 15U], 7) ^ std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
    auto const s1 = std::rotr(words[index - 2U], 17) ^ std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }
  auto a = hash[0];
  auto b = hash[1];
  auto c = hash[2];
  auto d = hash[3];
  auto e = hash[4];
  auto f = hash[5];
  auto g = hash[6];
  auto h = hash[7];
  for (std::size_t index = 0; index < words.size(); ++index)
  {
    auto const s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    auto const choose = (e & f) ^ ((~e) & g);
    auto const t1 = h + s1 + choose + constants[index] + words[index];
    auto const s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    auto const majority = (a & b) ^ (a & c) ^ (b & c);
    auto const t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  hash[0] += a;
  hash[1] += b;
  hash[2] += c;
  hash[3] += d;
  hash[4] += e;
  hash[5] += f;
  hash[6] += g;
  hash[7] += h;
  // Linux's non-elidable erase preserves the session snapshot's temporary-byte clearing contract.
  ::explicit_bzero(words.data(), sizeof(words));
}

}  // namespace detail

// Hash borrowed bytes without allocating a padded input copy. Only the final partial
// block is copied, and both that scratch and the compression schedule are erased.
[[nodiscard]] inline std::array<std::uint8_t, 32> sha256(std::span<std::byte const> bytes) noexcept
{
  auto const bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  while (bytes.size() >= 64U)
  {
    detail::sha256_compress(hash, bytes.first<64>());
    bytes = bytes.subspan(64U);
  }
  std::array<std::byte, 128> tail{};
  std::ranges::copy(bytes, tail.begin());
  tail[bytes.size()] = std::byte{0x80};
  auto const padded_size = bytes.size() < 56U ? 64U : 128U;
  for (std::size_t index = 0; index < 8U; ++index)
    tail[padded_size - 1U - index] = static_cast<std::byte>((bit_length >> (index * 8U)) & 0xffU);
  detail::sha256_compress(hash, std::span(tail).first<64>());
  if (padded_size == 128U)
    detail::sha256_compress(hash, std::span(tail).subspan<64, 64>());
  ::explicit_bzero(tail.data(), sizeof(tail));

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < hash.size(); ++index)
    for (std::size_t byte = 0; byte < 4U; ++byte)
      digest[index * 4U + byte] = static_cast<std::uint8_t>((hash[index] >> ((3U - byte) * 8U)) & 0xffU);
  return digest;
}

[[nodiscard]] inline std::array<std::uint8_t, 32> sha256(std::span<std::uint8_t const> bytes) noexcept
{
  return sha256(std::as_bytes(bytes));
}

[[nodiscard]] inline std::array<std::uint8_t, 32> sha256(std::string_view bytes) noexcept
{
  return sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

[[nodiscard]] inline std::string sha256_hex(std::string_view bytes)
{
  return hex_encode(sha256(bytes));
}

}  // namespace ava::core
