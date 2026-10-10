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
using DataList = utils::Vector<Data const*, DataListIndex, core::Application::Vec8Alloc::rebind<Data const*>::other>;

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
  FrontEnd* frontend_;          // The frontend that this object was registered with, or nullptr if it wasn't registered (yet).
  DataListIndex index_;         // The handle returned upon registration with FrontEnd::register_data (or `undefined` if not registered).

 protected:
  // Construct a Data object that is not registered with any FrontEnd.
  Data() : frontend_(nullptr) { }

  // Construct a Data object in its final place and register it with `frontend`.
  // Use the default constructor for unregistered Data objects (including for application modes that don't have a frontend).
  Data(FrontEnd* frontend);

  // A Data object is not moveable, see comment above class UnlockedData.
  Data(Data&& orig) = delete;
  Data& operator=(Data&& orig) = delete;

  virtual ~Data() = default;

  // This must be called by the backend whenever the data associated with this object has changed.
  void notify();

 public:
  // Called from FrontEnd::register_frontend_data.
  void register_with_front_end(utils::Badge<FrontEnd>, FrontEnd* frontend);

#if CW_DEBUG
  // Returns true if this object is registered with a FrontEnd.
  bool is_registered() const { return frontend_; }
#endif

  // This class uses a custom print_on.
  AVA_PRINT_ON_MEMBERS
};

} // namespace ava::app::frontend
