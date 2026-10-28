#pragma once

#include "ava/app/commands.h"
#include "ava/config/model_config.h"

#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

[[nodiscard]] std::vector<std::string> model_configuration_diagnostics(ava::config::ModelInfo const& model, bool provider_registered);
[[nodiscard]] ava::core::Result<CommandResult> run_models_command(runtime::session_ts& unlocked_session, std::string_view query = {});
[[nodiscard]] ava::core::Result<CommandResult> run_providers_command(runtime::session_ts& unlocked_session, std::string_view query = {});

}  // namespace ava::app
