#pragma once

#include "FrontEnd.h"
#include "ava/app/runtime/Directories.h"

#include <atomic>
#include <string>

namespace ava::frontend {

class Directories
{
 private:
  FrontEnd const* frontend_;
  app::runtime::Directories directories_snapshot_;
  DisplayString workspace_root_;
  DisplayString current_directory_;
  std::atomic_bool dirty_;
  DataListIndex index_;

  void refresh_snapshot();

 public:
  explicit Directories(FrontEnd const* frontend) : frontend_(frontend) { }

  void initialize(app::runtime::Directories_ts const* unlocked_directories);
  void dirty() { dirty_ = true; }

  RefreshResult<std::string const&> workspace_root_refresh();
  RefreshResult<std::string const&> current_directory_refresh();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::frontend
