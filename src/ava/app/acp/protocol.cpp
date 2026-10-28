#include "sys.h"
#include "ava/app/acp/protocol.h"

#include <utility>

namespace ava::app::acp {

bool ids_equal(JsonRpcId const& lhs, JsonRpcId const& rhs) noexcept
{
  return lhs == rhs;
}

std::string id_debug_string(JsonRpcId const& id)
{
  if (std::holds_alternative<NullJsonRpcId>(id))
    return "null";
  if (auto const* integer = std::get_if<std::int64_t>(&id))
    return std::to_string(*integer);
  return std::get<std::string>(id);
}

std::string id_key(JsonRpcId const& id)
{
  if (std::holds_alternative<NullJsonRpcId>(id))
    return "n:";
  if (auto const* number = std::get_if<std::int64_t>(&id))
    return "i:" + std::to_string(*number);
  return "s:" + std::get<std::string>(id);
}

ava::core::Error protocol_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

}  // namespace ava::app::acp
