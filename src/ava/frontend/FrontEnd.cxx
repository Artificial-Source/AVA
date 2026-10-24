#include "sys.h"
#include "FrontEnd.h"
#include "ava/app/runtime/Session.h"

namespace ava::frontend {

// Now that Session is complete, verify that `SessionConstReadAccess` is what we needed for initialize_frontend_state.
static_assert(std::is_same_v<app::runtime::SessionConstReadAccess, app::runtime::session_ts::crat>);

// Projection constructors retain pointers only; canonical data is connected after session registration.
FrontEnd::FrontEnd()
    : data_list_(core::Application::instance().vec8alloc()),
      agent_selection_(this),
      model_selection_(this),
      prompt_metadata_(this),
      session_stats_(this, &model_selection_),
      directories_(this),
      git_branch_(this),
      session_identity_(this)
{
}

DataListIndex FrontEnd::register_data(Data const* data)
{
  DataListIndex index = data_list_.iend();
  data_list_.push_back(data);
  return index;
}

void FrontEnd::register_frontend_data(app::runtime::session_ts& unlocked_session)
{
  DoutEntering(dc::notice, "FrontEnd::register_frontend_data()");

  app::runtime::session_ts::wat session_w(unlocked_session);
  session_w->agent_selection().register_with_front_end({}, this);
  session_w->directories().register_with_front_end({}, this);
  session_w->prompt_mode_and_context_sources().register_with_front_end({}, this);
  session_w->model_selection().register_with_front_end({}, this);
  session_w->session_stats().register_with_front_end({}, this);
  session_w->git_branch().register_with_front_end({}, this);
  session_w->store.options().register_with_front_end({}, this);
  initialize_frontend_state(session_w);
}

// Connect every common projection to its already registered canonical source before the first refresh.
void FrontEnd::initialize_frontend_state(app::runtime::SessionConstReadAccess const& session_r)
{
  agent_selection_.initialize(&session_r->agent_selection());
  model_selection_.initialize(&session_r->model_selection());
  prompt_metadata_.initialize(&session_r->prompt_mode_and_context_sources());
  session_stats_.initialize(&session_r->session_stats());
  directories_.initialize(&session_r->directories());
  git_branch_.initialize(&session_r->git_branch());
  session_identity_.initialize(&session_r->store.options());
  initialize_frontend_state_impl(session_r);
}

// Record canonical changes for the frontend thread without refreshing caches or waking a particular UI.
// Other registered Data types have no common projection and are left to derived notification handlers.
void FrontEnd::data_changed(DataListIndex index)
{
  DoutEntering(dc::frontend, "FrontEnd::data_changed(" << index << ")");
  Data const* data = get_data(index);

  if (dynamic_cast<app::runtime::AgentSelection_ts const*>(data) != nullptr)
    agent_selection_.dirty();
  else if (dynamic_cast<app::runtime::Directories_ts const*>(data) != nullptr)
    directories_.dirty();
  else if (dynamic_cast<app::runtime::ModelSelection_ts const*>(data) != nullptr)
    model_selection_.dirty();
  else if (dynamic_cast<app::runtime::PromptModeAndContextSources_ts const*>(data) != nullptr)
    prompt_metadata_.dirty();
  else if (dynamic_cast<session::Stats_ts const*>(data) != nullptr)
    session_stats_.dirty();
  else if (dynamic_cast<app::runtime::GitBranch_ts const*>(data) != nullptr)
    git_branch_.dirty();
  else if (dynamic_cast<session::SessionStoreOptions_ts const*>(data) != nullptr)
    session_identity_.dirty();
  else
    data_changed_impl(index);
}

void FrontEnd::on_submit(std::u8string const& UNUSED_ARG(input))
{
}

} // namespace ava::frontend
