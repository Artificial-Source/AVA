#pragma once

#include "AgentSelection.h"
#include "Data.h"
#include "Directories.h"
#include "GitBranch.h"
#include "ModelSelection.h"
#include "PromptModeAndContextSources.h"
#include "SessionIdentity.h"
#include "SessionStats.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/session_ts.h"

#include <string>

namespace ava::app::runtime {
// Avoid having to define Session.
using SessionUnlockedBase = threadsafe::Unlocked<Session, threadsafe::policy::Primitive<SessionMutex>>;
using SessionConstReadAccess = threadsafe::AccessConst<SessionUnlockedBase>;
} // namespace ava::app::runtime

namespace ava::frontend {

// class FrontEnd
//
// Base class of a frontend.
class FrontEnd
{
 protected:
  DataList data_list_;          // All registered frontend Data objects.

  // Frontend projection objects.
  AgentSelection agent_selection_;
  ModelSelection model_selection_;
  PromptModeAndContextSources prompt_metadata_;
  SessionStats session_stats_;
  Directories directories_;
  GitBranch git_branch_;
  SessionIdentity session_identity_;

 protected:
  // Construct the registry and projections, connecting statistics to this frontend's model projection.
  FrontEnd();

  // The user entered text in the composer area and hit Enter. Called by the derived frontend.
  void on_submit(std::u8string const& input);

 public:
  virtual ~FrontEnd() = default;

  // Register a Data (derived) object.
  DataListIndex register_data(Data const* data);

  // Register all Data (derived) objects.
  void register_frontend_data(app::runtime::session_ts& unlocked_session);

  // Initialize the common projections from session_r after registering their canonical objects.
  // Overrides that use the common projections must call this base implementation.
  void initialize_frontend_state(app::runtime::SessionConstReadAccess const& session_r);
  // Override this for UI *specific* frontend projections; default: the are none.
  virtual void initialize_frontend_state_impl(app::runtime::SessionConstReadAccess const& UNUSED_ARG(session_r)) { }

  // Mark the common projection for index dirty on the notifying backend thread, without accessing UI state.
  // Overrides that use the common projections must forward notifications to this base implementation.
  void data_changed(DataListIndex index);
  // Override this for UI *specific* frontend projections; default: the are none.
  virtual void data_changed_impl(DataListIndex UNUSED_ARG(index)) { }

  // Obtain Data pointer for index previously returned by register_data.
  Data const* get_data(DataListIndex index) const
  {
    // Initialize the object that calls this function, before calling its refresh_*() function.
    ASSERT(!index.undefined());
    return data_list_[index];
  }

  // Accessors.

  AgentSelection& agent_selection() { return agent_selection_; }
  ModelSelection& model_selection() { return model_selection_; }
  ModelSelection const& model_selection() const { return model_selection_; }
  PromptModeAndContextSources& prompt_metadata() { return prompt_metadata_; }
  SessionStats& session_stats() { return session_stats_; }
  SessionStats const& session_stats() const { return session_stats_; }
  Directories& directories() { return directories_; }
  GitBranch& git_branch() { return git_branch_; }
  SessionIdentity& session_identity() { return session_identity_; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::frontend
