#pragma once

#include "Data.h"
#include "DisplayString.h"
#include "RefreshResult.h"
#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"
#include "utils/macros.h"

#include <atomic>
#include <string>

namespace ava::frontend {

class FrontEnd;

// Cache the registered session-store options and project their display identity on the frontend thread.
class SessionIdentity
{
 private:
  FrontEnd const* frontend_;
  // Copy of the entire canonical ava::session::SessionStoreOptions value protected by SessionStoreOptions_ts:
  // root_dir, workspace_dir, and session_id.
  session::SessionStoreOptions options_snapshot_;
  // Derived cache, not a canonical member copy: session_path_for_options uses ava::session::SessionStoreOptions::{root_dir, workspace_dir, session_id}.
  // The source is read under SessionStoreOptions_ts; changes are detected by comparing the derived path strings.
  ava::core::NoLogString session_path_snapshot_;
  DataListIndex index_;
  std::atomic_bool dirty_;

  // Cached strings for immediate display.
  DisplayString session_id_;
  DisplayString session_path_;

  // Copy one coherent options value and retain independent pending ID and path changes.
  void refresh();

 public:
  // Construct a projection for frontend, which must outlive this object.
  explicit SessionIdentity(FrontEnd const* frontend) : frontend_(frontend) { }

  // Connect once to the nested store options at their stable registered address.
  void initialize(session::SessionStoreOptions_ts const* unlocked_options);

  // Mark the options snapshot stale from a backend notification thread.
  void dirty() { dirty_ = true; }

  // Return the canonical session ID display and consume only its pending change flag.
  RefreshResult<std::string const&> session_id_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = session_id_.clear_changed();
    if (AI_UNLIKELY(changed))
      session_id_.display_ = options_snapshot_.session_id;
    return {session_id_.display_, changed};
  }

  // Return the path derived from the same whole options snapshot and consume only its change flag.
  RefreshResult<std::string const&> session_path_refresh()
  {
    if (dirty_.exchange(false))
      refresh();
    bool const changed = session_path_.clear_changed();
    if (AI_UNLIKELY(changed))
      session_path_.display_ = session_path_snapshot_;
    return {session_path_.display_, changed};
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::frontend
