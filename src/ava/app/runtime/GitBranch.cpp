#include "sys.h"
#include "GitBranch.h"
#include "Session.h"

#include <fstream>
#include <string_view>
#include <utility>

namespace ava::app::runtime {

std::string observe_git_branch(std::filesystem::path const& workspace)
{
  std::ifstream input(workspace / ".git" / "HEAD");
  if (!input)
    return {};

  std::string head;
  std::getline(input, head);
  constexpr std::string_view ref_prefix = "ref: refs/heads/";
  if (head.starts_with(ref_prefix))
    return head.substr(ref_prefix.size());
  return head.size() > 12 ? head.substr(0, 12) : head;
}

bool refresh_git_branch(session_ts& unlocked_session)
{
  auto const identity = [&] {
    session_ts::crat session_r(unlocked_session);
    return std::pair{session_r->store.session_id(), session_r->workspace_dir()};
  }();
  auto observed = observe_git_branch(identity.second);

  session_ts::wat session_w(unlocked_session);
  if (session_w->store.session_id() != identity.first || session_w->workspace_dir() != identity.second)
    return false;
  if (*GitBranch_ts::crat(session_w->git_branch()) == observed)
    return false;
  session_w->git_branch().change([&observed](std::string& branch) { branch = std::move(observed); });
  return true;
}

} // namespace ava::app::runtime
