#pragma once

#include "Data.h"
#include "DisplayString.h"
#include "RefreshResult.h"
#include "ava/app/runtime/Directories.h"

#include <atomic>
#include <string>

namespace ava::frontend {

class FrontEnd;

class Directories
{
 private:
  FrontEnd const* frontend_;
  // Copy of the entire canonical ava::app::runtime::Directories value protected by Directories_ts:
  // workspace_dir, current_dir, and additional_writable_dirs.
  app::runtime::Directories directories_snapshot_;
  DisplayString workspace_root_;
  DisplayString current_directory_;
  std::atomic_bool dirty_;
  DataListIndex index_;

  void refresh();

 public:
  explicit Directories(FrontEnd const* frontend) : frontend_(frontend) { }

  void initialize(app::runtime::Directories_ts const* unlocked_directories);
  void dirty() { dirty_ = true; }

  RefreshResult<std::string const&> workspace_root_refresh();
  RefreshResult<std::string const&> current_directory_refresh();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
