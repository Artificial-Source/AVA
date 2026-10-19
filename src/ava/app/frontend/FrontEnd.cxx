#include "sys.h"
#include "FrontEnd.h"
#include "ava/app/runtime/Session.h"

namespace ava::app::frontend {

// Now that Session is complete, verify that `SessionConstReadAccess` is what we needed for initialize_frontend_state.
static_assert(std::is_same_v<runtime::SessionConstReadAccess, runtime::session_ts::crat>);

DataListIndex FrontEnd::register_data(Data const* data)
{
  DataListIndex index = data_list_.iend();
  data_list_.push_back(data);
  return index;
}

void FrontEnd::register_frontend_data(runtime::session_ts& unlocked_session)
{
  DoutEntering(dc::notice, "FrontEnd::register_frontend_data()");

  runtime::session_ts::wat session_w(unlocked_session);
  session_w->directories().register_with_front_end({}, this);
  session_w->prompt_mode_and_context_sources().register_with_front_end({}, this);
  session_w->model_selection().register_with_front_end({}, this);
  session_w->session_stats().register_with_front_end({}, this);
  session_w->git_branch().register_with_front_end({}, this);
  initialize_frontend_state(session_w);
}

void FrontEnd::on_submit(std::u8string const& UNUSED_ARG(input))
{
}

} // namespace ava::app::frontend
