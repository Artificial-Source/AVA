#include "sys.h"
#include "ava/http/transport.h"
#include "ava/config/openai_oauth.h"
#include "ava/core/json.h"
#include "ava/core/version.h"

#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace ava::config {
namespace {

constexpr std::string_view kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr std::string_view kAuthorizeUrl = "https://auth.openai.com/oauth/authorize";
constexpr std::string_view kTokenUrl = "https://auth.openai.com/oauth/token";
constexpr std::string_view kDeviceUserCodeUrl = "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr std::string_view kDeviceTokenUrl = "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr std::string_view kDeviceVerificationUrl = "https://auth.openai.com/codex/device";
constexpr std::string_view kDeviceRedirectUri = "https://auth.openai.com/deviceauth/callback";
constexpr std::string_view kRedirectUri = "http://localhost:1455/auth/callback";
constexpr std::string_view kScope = "openid profile email offline_access";
constexpr std::string_view kBase64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

class ScopedFd
{
 public:
  explicit ScopedFd(int fd) : fd_(fd) { }
  ScopedFd(ScopedFd const&) = delete;
  ScopedFd& operator=(ScopedFd const&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  ScopedFd& operator=(ScopedFd&& other) noexcept
  {
    if (this != &other)
    {
      close_if_open();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~ScopedFd() { close_if_open(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  void close_if_open() noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
  }

  int fd_ = -1;
};

std::string errno_message()
{
  return std::strerror(errno);
}

std::string base64_url_encode(std::span<std::uint8_t const> bytes)
{
  std::string output;
  output.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 3 <= bytes.size())
  {
    auto const block =
        (static_cast<std::uint32_t>(bytes[index]) << 16U) | (static_cast<std::uint32_t>(bytes[index + 1]) << 8U) | static_cast<std::uint32_t>(bytes[index + 2]);
    output.push_back(kBase64UrlAlphabet[(block >> 18U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[(block >> 12U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[(block >> 6U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[block & 0x3FU]);
    index += 3;
  }
  auto const remaining = bytes.size() - index;
  if (remaining == 1)
  {
    auto const block = static_cast<std::uint32_t>(bytes[index]) << 16U;
    output.push_back(kBase64UrlAlphabet[(block >> 18U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[(block >> 12U) & 0x3FU]);
  }
  else if (remaining == 2)
  {
    auto const block = (static_cast<std::uint32_t>(bytes[index]) << 16U) | (static_cast<std::uint32_t>(bytes[index + 1]) << 8U);
    output.push_back(kBase64UrlAlphabet[(block >> 18U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[(block >> 12U) & 0x3FU]);
    output.push_back(kBase64UrlAlphabet[(block >> 6U) & 0x3FU]);
  }
  return output;
}

std::optional<std::vector<std::uint8_t>> base64_url_decode(std::string_view value)
{
  std::vector<std::uint8_t> output;
  output.reserve((value.size() * 3) / 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (char const ch : value)
  {
    if (ch == '=')
      break;
    auto const position = kBase64UrlAlphabet.find(ch);
    if (position == std::string_view::npos)
      return std::nullopt;
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(position);
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      output.push_back(static_cast<std::uint8_t>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
    }
  }
  return output;
}

std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
  return (x & y) ^ (~x & z);
}
std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
  return (x & y) ^ (x & z) ^ (y & z);
}
std::uint32_t big_sigma0(std::uint32_t x)
{
  return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}
std::uint32_t big_sigma1(std::uint32_t x)
{
  return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}
std::uint32_t small_sigma0(std::uint32_t x)
{
  return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
}
std::uint32_t small_sigma1(std::uint32_t x)
{
  return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10U);
}

std::array<std::uint8_t, 32> sha256(std::string_view text)
{
  constexpr std::array<std::uint32_t, 64> kConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU,
      0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU,
      0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U,
      0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::vector<std::uint8_t> data(text.begin(), text.end());
  auto const bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
  data.push_back(0x80U);
  while ((data.size() % 64) != 56) data.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
  {
    data.push_back(static_cast<std::uint8_t>((bit_length >> static_cast<unsigned>(shift)) & 0xFFU));
  }

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64)
  {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index)
    {
      auto const offset = chunk + index * 4;
      words[index] = (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(data[offset + 2]) << 8U) | static_cast<std::uint32_t>(data[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index)
    {
      words[index] = small_sigma1(words[index - 2]) + words[index - 7] + small_sigma0(words[index - 15]) + words[index - 16];
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
      auto const temp1 = h + big_sigma1(e) + choose(e, f, g) + kConstants[index] + words[index];
      auto const temp2 = big_sigma0(a) + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < hash.size(); ++index)
  {
    digest[index * 4] = static_cast<std::uint8_t>((hash[index] >> 24U) & 0xFFU);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((hash[index] >> 16U) & 0xFFU);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((hash[index] >> 8U) & 0xFFU);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(hash[index] & 0xFFU);
  }
  return digest;
}

std::string url_encode(std::string_view value)
{
  constexpr std::string_view hex = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.size());
  for (unsigned char const byte : value)
  {
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
        byte == '~')
    {
      output.push_back(static_cast<char>(byte));
    }
    else
    {
      output.push_back('%');
      output.push_back(hex[(byte >> 4U) & 0x0FU]);
      output.push_back(hex[byte & 0x0FU]);
    }
  }
  return output;
}

std::string_view trim_ascii(std::string_view value)
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) value.remove_suffix(1);
  return value;
}

bool is_complete_json_object(std::string_view value)
{
  if (!ava::core::json::is_valid_object(value))
    return false;
  value = trim_ascii(value);
  if (value.empty() || value.front() != '{')
    return false;

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = 0; index < value.size(); ++index)
  {
    char const ch = value[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string)
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == '{')
    {
      ++depth;
      continue;
    }
    if (ch == '}')
    {
      --depth;
      if (depth < 0)
        return false;
      if (depth == 0)
        return trim_ascii(value.substr(index + 1)).empty();
    }
  }
  return false;
}

std::optional<int> positive_integer_from_json_field(std::string_view body, std::string_view field)
{
  if (auto const integer = ava::core::json::integer_field(body, field))
  {
    if (*integer > 0 && *integer <= 3600)
      return static_cast<int>(*integer);
    return std::nullopt;
  }

  auto const text = ava::core::json::string_field(body, field);
  if (!text || text->empty())
    return std::nullopt;
  int value = 0;
  auto const* begin = text->data();
  auto const* end = begin + text->size();
  auto const [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || ptr != end || value <= 0 || value > 3600)
    return std::nullopt;
  return value;
}

long long token_response_expiry(std::string_view body, long long now_seconds)
{
  if (auto const expires_at = ava::core::json::integer_field(body, "expires_at"))
    return *expires_at;
  return now_seconds + ava::core::json::integer_field(body, "expires_in").value_or(3600);
}

std::optional<std::string> token_response_account_id(std::string_view body, std::string_view access_token)
{
  auto account_id = ava::core::json::string_field(body, "account_id");
  if (account_id && !account_id->empty())
    return account_id;
  auto id_token = ava::core::json::string_field(body, "id_token");
  account_id = id_token ? openai_oauth_account_id_from_token(*id_token) : std::optional<std::string>{};
  if (!account_id)
    account_id = openai_oauth_account_id_from_token(access_token);
  return account_id;
}

ava::core::Result<OpenAICredential> parse_openai_oauth_token_response(std::string_view body, long long now_seconds, std::string_view refresh_fallback,
                                                                      std::string_view account_id_fallback)
{
  if (!is_complete_json_object(body))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth response was malformed JSON"));
  }

  auto access = ava::core::json::string_field(body, "access_token");
  if (!access || access->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth response did not include an access credential"));
  }
  auto refresh = ava::core::json::string_field(body, "refresh_token");
  auto account_id = token_response_account_id(body, *access);
  if (!account_id && !account_id_fallback.empty())
    account_id = std::string(account_id_fallback);
  // Validate the timestamp before token_response_expiry performs signed arithmetic.
  if (ava::core::json::field_value_start(body, "expires_at"))
  {
    auto const expires_at = ava::core::json::integer_field(body, "expires_at");
    if (!expires_at || *expires_at <= 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth response included an invalid expires_at"));
  }
  else
  {
    auto const expires_in =
        ava::core::json::field_value_start(body, "expires_in") ? ava::core::json::integer_field(body, "expires_in") : std::optional<long long>(3600);
    if (!expires_in || *expires_in <= 0 || now_seconds > std::numeric_limits<long long>::max() - *expires_in)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth response included an invalid expires_in"));
  }

  return OpenAICredential{.type = OpenAICredentialType::OAuth,
                          .access_token = *access,
                          .refresh_token = refresh && !refresh->empty() ? *refresh : std::string(refresh_fallback),
                          .expires_at = token_response_expiry(body, now_seconds),
                          .account_id = account_id.value_or(""),
                          .source_path = {}};
}

ava::core::Result<ava::http::HttpResponse> post_openai_oauth_token_form(std::string body, ava::http::Transport& transport, std::string_view failure_message)
{
  auto response = transport.send(ava::http::HttpRequest{.method = "POST",
                                                        .url = std::string(kTokenUrl),
                                                        .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
                                                        .body = std::move(body),
                                                        .timeout_ms = 60000,
                                                        .follow_redirects = false,
                                                        .include_response_headers = false,
                                                        .resolve_hosts = {}});
  if (!response)
    return std::unexpected(response.error());
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, std::string(failure_message));
    error.with_context("status", std::to_string(response->status_code));
    return std::unexpected(std::move(error));
  }
  return response;
}

std::string openai_oauth_user_agent()
{
  return "ava/" + std::string(ava::core::version::kDisplayVersion);
}

std::optional<std::string> jwt_payload(std::string_view token)
{
  auto const first = token.find('.');
  if (first == std::string_view::npos)
    return std::nullopt;
  auto const second = token.find('.', first + 1);
  if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos)
    return std::nullopt;
  auto decoded = base64_url_decode(token.substr(first + 1, second - first - 1));
  if (!decoded)
    return std::nullopt;
  return std::string(decoded->begin(), decoded->end());
}

std::optional<std::string> account_id_from_payload(std::string_view payload)
{
  auto account = ava::core::json::string_field(payload, "chatgpt_account_id");
  if (account && !account->empty())
    return account;
  auto auth = ava::core::json::object_field(payload, "https://api.openai.com/auth");
  if (auth)
  {
    account = ava::core::json::string_field(*auth, "chatgpt_account_id");
    if (account && !account->empty())
      return account;
  }
  auto const organizations = ava::core::json::objects_in_array_field(payload, "organizations");
  if (!organizations.empty())
  {
    account = ava::core::json::string_field(organizations.front(), "id");
    if (account && !account->empty())
      return account;
  }
  return std::nullopt;
}

ava::core::Result<std::string> random_token(std::size_t bytes)
{
  std::vector<std::uint8_t> data(bytes);
  ScopedFd const fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to open randomness source");
    error.with_context("path", "/dev/urandom");
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  std::size_t offset = 0;
  while (offset < data.size())
  {
    auto const count = ::read(fd.get(), data.data() + offset, data.size() - offset);
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to read randomness source");
      error.with_context("path", "/dev/urandom");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (count == 0)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "randomness source returned no data"));
    }
    offset += static_cast<std::size_t>(count);
  }
  return base64_url_encode(data);
}

}  // namespace

std::string openai_oauth_code_challenge(std::string_view verifier)
{
  return base64_url_encode(sha256(verifier));
}

std::optional<std::string> openai_oauth_account_id_from_token(std::string_view token)
{
  auto const payload = jwt_payload(token);
  if (!payload)
    return std::nullopt;
  return account_id_from_payload(*payload);
}

ava::core::Result<OpenAIOAuthSession> make_openai_oauth_session()
{
  auto verifier = random_token(32);
  if (!verifier)
    return std::unexpected(verifier.error());
  auto state = random_token(24);
  if (!state)
    return std::unexpected(state.error());
  return make_openai_oauth_session(*verifier, *state);
}

ava::core::Result<OpenAIOAuthSession> make_openai_oauth_session(std::string verifier, std::string state)
{
  if (verifier.size() < 43 || verifier.size() > 128)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OAuth PKCE verifier length is invalid");
    error.with_context("length", std::to_string(verifier.size()));
    return std::unexpected(std::move(error));
  }
  if (state.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OAuth state is missing"));
  }
  auto const challenge = openai_oauth_code_challenge(verifier);
  std::string url(kAuthorizeUrl);
  url += "?response_type=code";
  url += "&client_id=" + url_encode(kClientId);
  url += "&redirect_uri=" + url_encode(kRedirectUri);
  url += "&scope=" + url_encode(kScope);
  url += "&code_challenge=" + url_encode(challenge);
  url += "&code_challenge_method=S256";
  url += "&state=" + url_encode(state);
  url += "&id_token_add_organizations=true";
  url += "&codex_cli_simplified_flow=true";
  url += "&originator=ava";
  return OpenAIOAuthSession{.code_verifier = std::move(verifier), .state = std::move(state), .authorization_url = url};
}

ava::core::Result<OpenAICredential> exchange_openai_oauth_code(std::string_view code, std::string_view verifier, ava::http::Transport& transport,
                                                               long long now_seconds)
{
  return exchange_openai_oauth_code(code, verifier, kRedirectUri, transport, now_seconds);
}

ava::core::Result<OpenAICredential> exchange_openai_oauth_code(std::string_view code, std::string_view verifier, std::string_view redirect_uri,
                                                               ava::http::Transport& transport, long long now_seconds)
{
  std::string body = "grant_type=authorization_code";
  body += "&client_id=" + url_encode(kClientId);
  body += "&code=" + url_encode(code);
  body += "&code_verifier=" + url_encode(verifier);
  body += "&redirect_uri=" + url_encode(redirect_uri);

  auto response = post_openai_oauth_token_form(std::move(body), transport, "OpenAI OAuth exchange failed");
  if (!response)
    return std::unexpected(response.error());
  return parse_openai_oauth_token_response(response->body, now_seconds, "", "");
}

ava::core::Result<OpenAIOAuthDeviceAuthorization> start_openai_oauth_device_authorization(ava::http::Transport& transport)
{
  auto response = transport.send(ava::http::HttpRequest{.method = "POST",
                                                        .url = std::string(kDeviceUserCodeUrl),
                                                        .headers = {{"Content-Type", "application/json"}, {"User-Agent", openai_oauth_user_agent()}},
                                                        .body = "{\"client_id\":\"" + ava::core::json::escape(kClientId) + "\"}",
                                                        .timeout_ms = 60000,
                                                        .follow_redirects = false,
                                                        .include_response_headers = false,
                                                        .resolve_hosts = {}});
  if (!response)
    return std::unexpected(response.error());
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device authorization failed");
    error.with_context("status", std::to_string(response->status_code));
    return std::unexpected(std::move(error));
  }
  if (!is_complete_json_object(response->body))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device response was malformed JSON"));
  }

  auto device_auth_id = ava::core::json::string_field(response->body, "device_auth_id");
  auto user_code = ava::core::json::string_field(response->body, "user_code");
  if (!device_auth_id || device_auth_id->empty() || !user_code || user_code->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device response was missing authorization fields"));
  }
  return OpenAIOAuthDeviceAuthorization{.device_auth_id = *device_auth_id,
                                        .user_code = *user_code,
                                        .verification_url = std::string(kDeviceVerificationUrl),
                                        .interval_seconds = positive_integer_from_json_field(response->body, "interval").value_or(5)};
}

ava::core::Result<std::optional<OpenAICredential>> poll_openai_oauth_device_authorization(OpenAIOAuthDeviceAuthorization const& authorization,
                                                                                          ava::http::Transport& transport, long long now_seconds)
{
  if (authorization.device_auth_id.empty() || authorization.user_code.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth device authorization is missing required fields"));
  }

  std::string body = "{\"device_auth_id\":\"" + ava::core::json::escape(authorization.device_auth_id) + "\",\"user_code\":\"" +
                     ava::core::json::escape(authorization.user_code) + "\"}";
  auto response = transport.send(ava::http::HttpRequest{.method = "POST",
                                                        .url = std::string(kDeviceTokenUrl),
                                                        .headers = {{"Content-Type", "application/json"}, {"User-Agent", openai_oauth_user_agent()}},
                                                        .body = std::move(body),
                                                        .timeout_ms = 60000,
                                                        .follow_redirects = false,
                                                        .include_response_headers = false,
                                                        .resolve_hosts = {}});
  if (!response)
    return std::unexpected(response.error());
  if (response->status_code == 403 || response->status_code == 404)
    return std::optional<OpenAICredential>{};
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device authorization polling failed");
    error.with_context("status", std::to_string(response->status_code));
    return std::unexpected(std::move(error));
  }
  if (!is_complete_json_object(response->body))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device token response was malformed JSON"));
  }

  auto authorization_code = ava::core::json::string_field(response->body, "authorization_code");
  auto code_verifier = ava::core::json::string_field(response->body, "code_verifier");
  if (!authorization_code || authorization_code->empty() || !code_verifier || code_verifier->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI OAuth device token response was missing exchange fields"));
  }

  auto credential = exchange_openai_oauth_code(*authorization_code, *code_verifier, kDeviceRedirectUri, transport, now_seconds);
  if (!credential)
    return std::unexpected(std::move(credential.error()));
  return std::optional<OpenAICredential>{std::move(*credential)};
}

ava::core::Result<OpenAICredential> refresh_openai_oauth_credential(OpenAICredential const& credential, ava::http::Transport& transport, long long now_seconds)
{
  if (credential.type != OpenAICredentialType::OAuth)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth refresh requires an OAuth credential"));
  }
  if (credential.refresh_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth refresh token is missing"));
  }

  std::string body = "grant_type=refresh_token";
  body += "&refresh_token=" + url_encode(credential.refresh_token);
  body += "&client_id=" + url_encode(kClientId);

  auto response = post_openai_oauth_token_form(std::move(body), transport, "OpenAI OAuth refresh failed");
  if (!response)
    return std::unexpected(response.error());
  auto refreshed = parse_openai_oauth_token_response(response->body, now_seconds, credential.refresh_token, credential.account_id);
  if (!refreshed)
    return std::unexpected(refreshed.error());
  refreshed->source_path = credential.source_path;
  return refreshed;
}

}  // namespace ava::config
