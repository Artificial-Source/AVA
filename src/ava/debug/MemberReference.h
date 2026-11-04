// Prints declared reference members as type/address without exposing their content.
// Generated printers reach this adapter through print_member in print_reference.h.
#pragma once

#include "print_members_on.h"
#include "utils/iomanip.h"

#include <memory>
#include <ostream>
#include <libcwd/type_info.h>

namespace ava_utils {

// Call-scoped debug adapter for a declared reference member. It prints only
// type/address, even for const references whose referent has a content printer.
// The referent must outlive the expression; no ownership or synchronization is added.
template <typename T>
class MemberReference final
{
 public:
  // Any logging thread may form this adapter while the borrowed object is alive.
  explicit MemberReference(T const& value) : value_(value) { }
  // Never dereference the borrowed object. Naming its pointer type also supports
  // forward-declared classes without requiring their definitions in a header.
  void print_on(std::ostream& os) const
  {
    os << NAMESPACE_DEBUG::type_name_of<T*>();
    os << '@' << static_cast<void const*>(std::addressof(value_));
  }
  // This adapter has its own address-only printer; recursive generation is unwanted.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
 private:
  // Borrowed only until the enclosing stream expression finishes; never inspected.
  T const& value_;
};

// Keep insertion discoverable through this adapter's namespace, including when
// the referent is a standard-library type with no AVA stream overloads.
template <typename T>
std::ostream& operator<<(std::ostream& os, MemberReference<T> const& reference)
{
  reference.print_on(os);
  return os;
}

} // namespace ava_utils
