#include "sys.h"
#include "ava/app/acp/codec_json_internal.h"
#include "ava/app/acp/content.h"
#include "ava/provider/provider.h"
#include "ava/core/ascii.h"
#include "ava/core/base64.h"

#include <string_view>
#include <utility>

namespace ava::app::acp {
namespace {

JsonRpcError content_error(std::string message)
{
  return JsonRpcError{.code = -32602,
                      .message = std::move(message),
                      .data_json = std::nullopt,
                      .id = std::nullopt,
                      .intent = EnvelopeIntent::Unknown,
                      .suppress_response = false};
}

std::optional<std::string> decode_base64_canonical(std::string_view encoded)
{
  if (encoded.empty())
    return std::nullopt;
  return ava::core::base64_decode(encoded, ava::core::Base64Alphabet::Standard, ava::core::Base64Padding::Required, ava::core::Base64TailBits::Canonical,
                                  kMaxAcpPromptImageBytes);
}

void append_prompt_line(std::string& text, std::string_view line)
{
  if (!text.empty())
    text.push_back('\n');
  text.append(line);
}

}  // namespace

PromptContentDecodeResult decode_prompt_content(std::string_view prompt_json)
{
  auto decoded = decode_json_value(prompt_json);
  if (!decoded)
    return std::unexpected(std::move(decoded.error()));
  auto const& prompt = *decoded;
  if (!prompt.is_array() || prompt.empty() || prompt.size() > kMaxAcpPromptBlocks)
    return std::unexpected(content_error("session/prompt requires a non-empty bounded prompt array"));

  AcpPromptContent content;
  std::size_t image_bytes = 0;
  for (auto const& block : prompt)
  {
    if (!block.is_object())
      return std::unexpected(content_error("prompt content blocks must be objects"));
    auto type = block.find("type");
    if (type == block.end() || !type->is_string() || type->get_ref<std::string const&>().empty())
      return std::unexpected(content_error("prompt content block type is required"));
    auto const& discriminator = type->get_ref<std::string const&>();
    if (discriminator == "text")
    {
      auto text = block.find("text");
      if (text == block.end() || !text->is_string())
        return std::unexpected(content_error("text prompt blocks require text"));
      if (content.text.size() + text->get_ref<std::string const&>().size() + (content.text.empty() ? 0U : 1U) > kMaxAcpPromptTextBytes)
        return std::unexpected(content_error("prompt text exceeds the ACP prompt byte limit"));
      append_prompt_line(content.text, text->get_ref<std::string const&>());
      continue;
    }

    if (discriminator == "resource_link")
    {
      auto name = block.find("name");
      auto uri = block.find("uri");
      if (name == block.end() || uri == block.end() || !name->is_string() || !uri->is_string() || name->get_ref<std::string const&>().empty() ||
          uri->get_ref<std::string const&>().empty() || name->get_ref<std::string const&>().size() > 4096 ||
          uri->get_ref<std::string const&>().size() > 16U * 1024U || ava::core::has_c0_or_del(name->get_ref<std::string const&>()) ||
          ava::core::has_c0_or_del(uri->get_ref<std::string const&>()))
      {
        return std::unexpected(content_error("resource_link prompt blocks require bounded safe name and uri strings"));
      }
      std::string const reference = "Resource link reference (not fetched): " + name->get<std::string>() + " <" + uri->get<std::string>() + ">";
      if (content.text.size() + reference.size() + (content.text.empty() ? 0U : 1U) > kMaxAcpPromptTextBytes)
        return std::unexpected(content_error("prompt text exceeds the ACP prompt byte limit"));
      append_prompt_line(content.text, reference);
      continue;
    }

    if (discriminator == "image")
    {
      if (content.images.size() >= kMaxAcpPromptImages)
        return std::unexpected(content_error("prompt image count exceeds the ACP limit"));
      auto data = block.find("data");
      auto mime = block.find("mimeType");
      if (data == block.end() || mime == block.end() || !data->is_string() || !mime->is_string() ||
          !ava::provider::is_supported_image_mime_type(mime->get_ref<std::string const&>()) || ava::core::has_c0_or_del(mime->get_ref<std::string const&>()))
        return std::unexpected(content_error("image prompt blocks require an exactly supported MIME type and base64 data"));
      auto decoded = decode_base64_canonical(data->get_ref<std::string const&>());
      if (!decoded)
        return std::unexpected(content_error("image data must be canonical base64 within the ACP image byte limit"));
      image_bytes += decoded->size();
      if (image_bytes > kMaxAcpPromptImageTotalBytes)
        return std::unexpected(content_error("prompt images exceed the ACP aggregate image byte limit"));
      content.images.push_back(AcpPromptImage{.mime_type = mime->get<std::string>(), .bytes = std::move(*decoded)});
      continue;
    }

    if (discriminator == "audio" || discriminator == "resource")
      return std::unexpected(content_error("audio and embedded resource prompt content are not supported or advertised by AVA ACP"));
    return std::unexpected(content_error("prompt content discriminator is not supported or advertised by AVA ACP"));
  }

  if (content.text.empty() && content.images.empty())
    return std::unexpected(content_error("session/prompt requires text, image, or resource_link content"));
  if (content.text.empty())
    content.text = "[Image attachment]";
  return content;
}

}  // namespace ava::app::acp
