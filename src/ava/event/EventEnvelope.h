#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <optional>
#include <string>

namespace ava::event {

struct EventEnvelope
{
  int schema_version = 1;
  std::string event_id;
  std::string timestamp;
  std::string session_id;
  std::optional<std::string> run_id;
  std::optional<std::string> turn_id;
  std::optional<std::string> message_id;
  std::optional<std::string> request_id;
  std::optional<std::string> correlation_id;
  std::string name;
  ava::core::NoLogString payload_json = "{}";
  std::string payload_type = "";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::event
