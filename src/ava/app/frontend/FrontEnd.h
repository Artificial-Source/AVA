#pragma once

#include "Data.h"
#include "ava/debug/print_members_on.h"

#include <string>

namespace ava::app::frontend {

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
  DataListIndex register_data(Data* data);

  // One of the registered Data objects was changed. Called by the backend.
  virtual void data_changed(DataListIndex index) = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::frontend
