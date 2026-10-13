#pragma once

#include "Data.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/session_ts.h"

#include <string>

namespace ava::app {

namespace runtime {
// Avoid having to define Session.
using SessionUnlockedBase = threadsafe::Unlocked<Session, threadsafe::policy::Primitive<SessionMutex>>;
using SessionConstReadAccess = threadsafe::AccessConst<SessionUnlockedBase>;
} // namespace runtime

namespace frontend {

// A string as displayed on the UI.
// This type is used to store (private) state in snapshot objects, in the form of a display string,
// that are members of the final frontend class.
struct DisplayString
{
  std::string display_;
  bool changed_{true};

  bool clear_changed()
  {
    bool changed = changed_;
    changed_ = false;
    return changed;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Type returned by refresh_* functions.
struct RefreshResult
{
  std::string const& display;           // Reference to corresponding `DisplayString::display_`.
  bool const changed;                   // True if `display` was potentially changed since the last time.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// class FrontEnd
//
// Base class of a frontend.
class FrontEnd
{
 protected:
  DataList data_list_;          // All registered frontend Data objects.

 protected:
  // Constructor.
  FrontEnd() : data_list_(core::Application::instance().vec8alloc()) { }

  // The user entered text in the composer area and hit Enter. Called by the derived frontend.
  void on_submit(std::u8string const& input);

 public:
  virtual ~FrontEnd() = default;

  // Register a Data (derived) object.
  DataListIndex register_data(Data const* data);

  // Register all Data (derived) objects.
  void register_frontend_data(runtime::session_ts& unlocked_session);

  // Let the derived class initialize its displayable state. Called by FrontEnd::register_frontend_data.
  virtual void initialize_frontend_state(runtime::SessionConstReadAccess const& session_r) = 0;

  // One of the registered Data objects was changed. Called by the backend.
  virtual void data_changed(DataListIndex index) = 0;

  // Obtain Data pointer for index previously returned by register_data.
  Data const* get_data(DataListIndex index) const { return data_list_[index]; }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace frontend
} // namespace ava::app
