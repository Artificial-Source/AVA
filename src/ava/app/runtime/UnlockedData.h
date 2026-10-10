#pragma once

#include "ava/app/frontend/Data.h"
#include "ava/app/frontend/FrontEnd.h"
#include "threadsafe/threadsafe.h"
#include "utils/macros.h"
#include <utility>
#include "debug.h"

namespace ava::app::runtime {

// Moving an object derived from an `UnlockedData` must behave as follows:
//
// ┌─────────────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────┐
// │Operation                                    │Behavior                                                                              │
// ├─────────────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤
// │Construct a live registered object           │Register its final address once.                                                      │
// ├─────────────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤
// │Move-construct from an unregistered object   │Construct a fresh mutex, transfer the protected value, remain unregistered.           │
// ├─────────────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤
// │Move-construct from a registered object      │Reject as a programming error. Its address must remain stable.                        │
// ├─────────────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤
// │Move-assign into an existing object          │Transfer the protected value only; preserve the destination’s mutex and registration. │
// └─────────────────────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────┘
//
// Under these rules, Data itself need not be movable, and its registration fields can remain immutable after construction.
// The derived UnlockedData can still define its own move operations by constructing—or retaining—the Data base rather than moving it.
//
// The practical consequence is that each canonical data object must be registered at its final address, after any construction-time
// movement of the containing Session. Detached construction intermediates remain unregistered.
// That fits session replacement by value assignment, provided replacement preserves the destination object rather than destroying and
// reconstructing it.
//
//
// Usage:
//
// The make an object of type Foobar observable by the FrontEnd, define a new type:
//
//   using Foobar_ts = UnlockedData<Foobar>;
//
// and declare variable using this new type:
//
//   Foobar_ts foobar_;
//
// If `foobar_` is a member of class (and typically it should be) then it should not be public,
// simply because we don't want to make any member public if we can help it.
// Use an accessor pair to make it accessible:
//
//   Foobar_ts& foobar() { return foobar_; }
//   Foobar_ts const& foobar() const { return foobar_; }
//
// Read/write access now requires constructing an Access Type, as documentated by the `threadsafe` git submodule.
// For example ONE of the following lines:
//
//   Foobar_ts::wat foobar_w(obj.foobar());             // For write access.
//   Foobar_ts::rat foobar_r(obj.foobar());             // For read access. Can be converted to a wat.
//   Foobar_ts::crat foobar_r(const_obj.foobar());      // For read access, required if obj is const. Can not be converted to a wat.
//
// The frontend will be notified of a possible change (regardless of whether it changed) upon the destruction of the wat.
//
// For example,
//
//   {
//     Foobar_ts::wat foobar_w(obj.foobar());
//     foobar_w->make_change(payload);
//   } // <- FrontEnd is notified that `obj.foobar_` changed.
//
// Alternatively one can use
//
//   obj.foobar().change([](auto& foobar) {
//     foobar.make_change(payload);
//   });

template<typename T, typename POLICY_MUTEX = threadsafe::policy::Primitive<std::mutex>>
class UnlockedData : protected threadsafe::Unlocked<T, POLICY_MUTEX>, public frontend::Data
{
 public:
  using unlocked_type = threadsafe::Unlocked<T, POLICY_MUTEX>;

  struct DataNotifier {
    UnlockedData& data_;
    DataNotifier(UnlockedData& data) : data_(data) { }
    ~DataNotifier() { data_.notify(); }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  struct wat : DataNotifier {
    unlocked_type::wat write_access;
    wat(UnlockedData& unlocked_ts) : DataNotifier(unlocked_ts), write_access(unlocked_ts) { }
    typename unlocked_type::data_type* operator->() const { return &*write_access; }
    typename unlocked_type::data_type& operator*() const { return *write_access; }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  struct rat {
    unlocked_type::rat read_access;
    rat(UnlockedData& unlocked_ts) : read_access(unlocked_ts) { }
    typename unlocked_type::data_type const* operator->() const { return &*read_access; }
    typename unlocked_type::data_type const& operator*() const { return *read_access; }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  struct crat {
    unlocked_type::crat read_access;
    crat(UnlockedData const& unlocked_ts) : read_access(unlocked_ts) { }
    typename unlocked_type::data_type const* operator->() const { return &*read_access; }
    typename unlocked_type::data_type const& operator*() const { return *read_access; }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // Construct an UnlockedData object in its final place; this registers the object with `frontend`.
  // Passing nullptr for `frontend` is reserved for application modes that don't have a frontend at all.
  template<typename... Args>
  UnlockedData(frontend::FrontEnd* frontend, Args&&... args) :
    threadsafe::Unlocked<T, POLICY_MUTEX>(std::forward<Args>(args)...), frontend::Data(frontend)
  {
  }

  // Construct an unregistered UnlockedData object with a default-constructed value.
  UnlockedData() = default;

  // Construct an unregistered object from value-constructor arguments.
  template<typename FirstArg, typename... Rest>
  requires (!std::is_convertible_v<FirstArg&&, frontend::FrontEnd*>)
  UnlockedData(FirstArg&& first, Rest&&... rest) :
    threadsafe::Unlocked<T, POLICY_MUTEX>(std::forward<FirstArg>(first), std::forward<Rest>(rest)...)
  {
  }

  UnlockedData(UnlockedData&& orig);
  UnlockedData& operator=(UnlockedData&& orig);

  template<typename F>
  void change(F&& update)
  {
    {
      typename threadsafe::Unlocked<T, POLICY_MUTEX>::wat write_access(*this);
      std::invoke(std::forward<F>(update), *write_access);
    }
    notify();
  }

  // This object requires a custom print_on.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

#ifdef CWDEBUG
  void print_on(std::ostream& os) const
  {
    os << static_cast<frontend::Data const&>(*this) << "@" << (void*)static_cast<unlocked_type const*>(this);
  }
#endif
};

template<typename T, typename POLICY_MUTEX>
UnlockedData<T, POLICY_MUTEX>::UnlockedData(UnlockedData&& orig) :
  threadsafe::Unlocked<T, POLICY_MUTEX>(std::move(static_cast<threadsafe::Unlocked<T, POLICY_MUTEX>&&>(orig)))
{
  // Move-construct from a registered object is not allowed.
  ASSERT(!static_cast<frontend::Data const&>(orig).is_registered());
}

template<typename T, typename POLICY_MUTEX>
UnlockedData<T, POLICY_MUTEX>& UnlockedData<T, POLICY_MUTEX>::operator=(UnlockedData&& orig)
{
  if (AI_LIKELY(&orig != this))
  {
    // GPT-6.1 Astra commented:
    //
    // Concurrent a = std::move(b) and b = std::move(a) could each acquire their destination lock and then wait forever for the other.
    //
    // But that will never happen and would be a bug either way (even with proper locking, avoiding a deadlock, that would constitute
    // a race condition between which move happens first, when the result is not the same). The reason this never can happen is that
    // all such moves must go in a single direction. The graph of moves must be (and will be) a directed acyclic graph.
    typename unlocked_type::wat source_w(orig);
    typename unlocked_type::wat target_w(*this);
    *target_w = std::move(*source_w);
  }
  return *this;
}

} // namespace ava::app::runtime
