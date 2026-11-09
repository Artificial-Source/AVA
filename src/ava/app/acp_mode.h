#pragma once

#include "ava/app/ExitCode.h"

#include <iosfwd>
#include <memory>

namespace ava::diagnostics {
class RuntimeDiagnostics;
}  // namespace ava::diagnostics

namespace ava::process {
class ProcessScopeV1;
}  // namespace ava::process

namespace ava::app {

[[nodiscard]] ExitCode run_acp_mode(std::ostream& error_output, ava::process::ProcessScopeV1 const& application_process_scope,
                                    std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics = nullptr);

} // namespace ava::app
