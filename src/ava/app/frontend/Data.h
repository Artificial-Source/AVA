#pragma once

#include "ava/core/Application.h"
#include "utils/Vector.h"

namespace ava::app::frontend {

// Forward declaration.
class Data;
class FrontEnd;

struct DataListCategory
{
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
using DataListIndex = utils::VectorIndex<DataListCategory>;
using DataList = utils::Vector<Data*, DataListIndex, core::Application::Vec8Alloc::rebind<Data*>::other>;

// class Data
//
// Base class for objects containing data that the UI (might) need(s).
//
// The life-time contract is as follows: the frontend (derived from FrontEnd) is created first.
// Next any objects derived from Data are constructed (which register themselves with the FrontEnd).
// Finally the FrontEnd is started.
// Data objects may not be deleted while the FrontEnd is being used. Only after termination of the
// frontend both can be destructed (in either order: the FrontEnd destructor should not access the
// Data* objects in FrontEnd::data_list_, nor should the destructor of a class derived from Data
// access the frontend).
//
class Data
{
 private:
  FrontEnd* const frontend_;            // The frontend that this object was registered with.
  DataListIndex const index_;           // The handle returned upon registration with FrontEnd::register_data.

 protected:
  Data(FrontEnd* frontend);

  virtual ~Data() = default;

  // This must be called by the backend whenever the data associated with this object has changed.
  void notify();

 public:
  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::frontend
