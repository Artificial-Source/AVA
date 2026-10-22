#include "sys.h"
#include "SessionIdentity.h"

namespace ava::frontend {

// Copy all canonical options under one read lock before deriving either display value.
void SessionIdentity::refresh()
{
  Data const* data = frontend_->get_data(index_);
  // index_ must identify the stable SessionStoreOptions_ts registered during frontend initialization.
  ASSERT(dynamic_cast<session::SessionStoreOptions_ts const*>(data) != nullptr);
  auto const* unlocked_options = static_cast<session::SessionStoreOptions_ts const*>(data);
  session::SessionStoreOptions_ts::crat options_r(*unlocked_options);

  auto const next_path = session::session_path_for_options(*options_r).string();
  session_id_.changed_ = session_id_.changed_ || options_snapshot_.session_id != options_r->session_id;
  session_path_.changed_ = session_path_.changed_ || session_path_snapshot_ != next_path;
  options_snapshot_ = *options_r;
  session_path_snapshot_ = next_path;
}

// Store the registered options index and require an initial whole-value snapshot.
void SessionIdentity::initialize(session::SessionStoreOptions_ts const* unlocked_options)
{
  // Call initialize exactly once for each frontend projection.
  ASSERT(index_.undefined());
  index_ = unlocked_options->index();
  dirty_ = true;
}

} // namespace ava::frontend
