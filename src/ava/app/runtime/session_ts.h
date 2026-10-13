#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/thread.h"

#include <threadsafe/threadsafe.h>

namespace ava::app::runtime {

class Session;

#ifdef CWDEBUG
using SessionMutex = ava::core::SessionDebugMutex;

template <typename SESSION>
class UnlockedSession : public threadsafe::Unlocked<SESSION, threadsafe::policy::Primitive<SessionMutex>>
{
 public:
  using threadsafe::Unlocked<SESSION, threadsafe::policy::Primitive<SessionMutex>>::Unlocked;
  using threadsafe::Unlocked<SESSION, threadsafe::policy::Primitive<SessionMutex>>::mutex;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using session_ts = UnlockedSession<Session>;
#else
using SessionMutex = std::mutex;
using session_ts = threadsafe::Unlocked<Session, threadsafe::policy::Primitive<SessionMutex>>;
#endif

}  // namespace ava::app::runtime

#ifndef CWDEBUG
#define AVA_DEBUG_PRINT_MESSAGE(msg)
#else
#define AVA_STRINGIFY_(x) #x
#define AVA_STRINGIFY(x) AVA_STRINGIFY_(x)
#define AVA_DEBUG_PRINT_MESSAGE(msg) \
  ;                                  \
  Dout(dc::session, msg " from " __FILE__ ":" AVA_STRINGIFY(__LINE__))
#endif

#define AVA_DECLARE_ACCESS_TYPE_CR(session_r, unlocked_session) ava::app::runtime::session_ts::crat session_r(unlocked_session)

#define AVA_DECLARE_ACCESS_TYPE_R(session_r, unlocked_session) ava::app::runtime::session_ts::rat session_r(unlocked_session)

#define AVA_DECLARE_ACCESS_TYPE_W(session_w, unlocked_session) ava::app::runtime::session_ts::wat session_w(unlocked_session)

// Use this instead of CRITICAL_AREA_BEGIN_R if `unlocked_session` is a const&.
#define CRITICAL_AREA_BEGIN_CR(session)                       \
  AVA_DECLARE_ACCESS_TYPE_CR(session##_r, unlocked_##session) \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session "_r from unlocked_" #session)

// Locks `unlocked_session` for reading; use `session_r` to access the Session.
// This can only be used once per scope. See CRITICAL_AREA_CONTINUE_R.
#define CRITICAL_AREA_BEGIN_R(session)                       \
  AVA_DECLARE_ACCESS_TYPE_R(session##_r, unlocked_##session) \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session "_r from unlocked_" #session)

// Unlock `session_r`. Using it after this will lead to a SEGFAULT.
#define CRITICAL_AREA_END_R(session)                                      \
  do                                                                      \
  {                                                                       \
    session##_r.unlock() AVA_DEBUG_PRINT_MESSAGE("Unlocked " #session "_r"); \
  } while (0)

// Locks `unlocked_session` again for reading after the use of CRITICAL_AREA_END_R; use `session_r` to access the Session.
#define CRITICAL_AREA_CONTINUE_R(session)                                                   \
  do                                                                                        \
  {                                                                                         \
    session##_r.relock(unlocked_##session) AVA_DEBUG_PRINT_MESSAGE("Relocked " #session "_r"); \
  } while (0)

// Locks `unlocked_session` for reading and writing; use `session_w` to access the Session.
// This can only be used once per scope. See CRITICAL_AREA_CONTINUE_W.
#define CRITICAL_AREA_BEGIN_W(session)                       \
  AVA_DECLARE_ACCESS_TYPE_W(session##_w, unlocked_##session) \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session "_r from unlocked_" #session)

// Unlock `session_w`. Using it after this will lead to a SEGFAULT.
#define CRITICAL_AREA_END_W(session)                                      \
  do                                                                      \
  {                                                                       \
    session##_w.unlock() AVA_DEBUG_PRINT_MESSAGE("Unlocked " #session "_r"); \
  } while (0)

// Locks `unlocked_session` again for reading and writing after the use of CRITICAL_AREA_END_W; use `session_w` to access the Session.
#define CRITICAL_AREA_CONTINUE_W(session)                                                   \
  do                                                                                        \
  {                                                                                         \
    session##_w.relock(unlocked_##session) AVA_DEBUG_PRINT_MESSAGE("Relocked " #session "_w"); \
  } while (0)

// Aliases used when the lock just runs till the end of the scope (no CRITICAL_AREA_END_*).
#define SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session) \
  AVA_DECLARE_ACCESS_TYPE_CR(session_r, unlocked_session)    \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session_r " from " #unlocked_session " (scoped)")

#define SCOPED_CRITICAL_AREA_R(session_r, unlocked_session) \
  AVA_DECLARE_ACCESS_TYPE_R(session_r, unlocked_session)    \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session_r " from " #unlocked_session " (scoped)")

#define SCOPED_CRITICAL_AREA_W(session_w, unlocked_session) \
  AVA_DECLARE_ACCESS_TYPE_W(session_w, unlocked_session)    \
  AVA_DEBUG_PRINT_MESSAGE("Created " #session_w " from " #unlocked_session " (scoped)")
