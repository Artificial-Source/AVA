// Prints a borrowed raw pointer's address without inspecting its target.
#pragma once

#include "print_members_on.h"

#include <ostream>
#include <type_traits>

namespace ava_utils {

// A stream-expression value used by generated member printers. It copies only
// the pointer value; it never owns or reads the target. Any logging thread may
// use it after synchronizing the original member read. The stream remains the
// caller's responsibility. This adapter provides no ownership of the target.
template <typename T>
class MemberPointer final
{
 public:
  // Copy the already-read pointer. Arrays must remain arrays at the call site;
  // only declared pointer members should use this adapter.
  explicit MemberPointer(T value) : value_(value) { }

  // Force the standard address insertion, bypassing C-string and content
  // printers. AVA's POSIX platforms support the function-pointer conversion.
  void print_on(std::ostream& os) const
  {
    if (value_ == nullptr)
    {
      os << "nullptr";
      return;
    }
    if constexpr (std::is_function_v<std::remove_pointer_t<T>>)
      os.operator<<(reinterpret_cast<void const*>(value_));
    else
      os.operator<<(const_cast<void const*>(static_cast<void const volatile*>(value_)));
  }

  // The hand-written printer must not recursively print this pointer member.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Address snapshot made by the caller; no ownership or target access follows.
  T value_;
};

// Keep address-only insertion discoverable without AVA's general pointer
// overloads; the adapter and stream must survive this synchronous expression.
template <typename T>
std::ostream& operator<<(std::ostream& os, MemberPointer<T> const& pointer)
{
  pointer.print_on(os);
  return os;
}

} // namespace ava_utils
