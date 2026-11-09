#pragma once

#include "ava/diagnostics/records.h"
#include "ava/app/ExitCode.h"
#include "ava/config/xdg_paths.h"

#include <filesystem>
#include <iosfwd>

namespace ava::app {

[[nodiscard]] ava::diagnostics::DoctorReport collect_passive_doctor_report(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir);
[[nodiscard]] ExitCode run_doctor(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir, bool json, std::ostream& out,
                                  std::ostream& err);
[[nodiscard]] ExitCode run_support_export(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir, std::ostream& out, std::ostream& err);

}  // namespace ava::app
