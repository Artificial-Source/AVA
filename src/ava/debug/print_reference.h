// Debug stream adapters; generated references and raw pointers expose identity without borrowed content.
#pragma once

#include "MemberPointer.h"
#include "MemberReference.h"
#include "print_members_on.h"
#include "utils/iomanip.h"

#include <type_traits>
#include <libcwd/type_info.h>

namespace ava_utils {

// Generated printers pass the member's declared type via unparenthesized
// decltype(member). Values keep existing printers; references and raw pointers
// expose identity only, without traversing borrowed content. NoLogString fields
// are omitted by the generator before reaching this C++ adapter.
// This does not make concurrent reads of ordinary value members safe.
template <typename Declared, typename T>
decltype(auto) print_member(T const& value)
{
  if constexpr (std::is_reference_v<Declared>)
    return MemberReference<T>(value);
  else if constexpr (std::is_pointer_v<Declared>)
    return MemberPointer<std::remove_cv_t<T>>(value);
  else
    return (value);
}

template <typename T>
class NoDereference : public utils::iomanip::Sticky
{
 private:
  static utils::iomanip::Index s_index;

 public:
  NoDereference(long iword_value) : Sticky(s_index, iword_value) { }

  static long get_iword_value(std::ostream& os) { return get_iword_from(os, s_index); }
};

//static
template <typename T>
utils::iomanip::Index NoDereference<T>::s_index;

template <typename T>
struct PrintConstReference
{
  T const& m_ref;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

template <typename T>
struct PrintReference : public PrintConstReference<T>
{
};

template <typename T>
PrintConstReference<T> print_reference(T const& ref)
{
  return {ref};
}

template <typename T>
std::ostream& operator<<(std::ostream& os, PrintConstReference<T> ref)
{
#if CWDEBUG_LOCATION
  os << NAMESPACE_DEBUG::type_name_of<T>();
#endif
  os << '@' << (void*)&ref.m_ref;
  if constexpr (requires { os << ref.m_ref; })
    if (!NoDereference<T>::get_iword_value(os))
      os << ':' << NoDereference<T>(1L) << ref.m_ref << NoDereference<T>(0L);
  return os;
}

template <typename T>
PrintReference<T> print_reference(T& ref)
{
  return {ref};
}

template <typename T>
inline std::ostream& operator<<(std::ostream& os, PrintReference<T> ref)
{
  return os << NoDereference<T>(1L) << static_cast<PrintConstReference<T>>(ref) << NoDereference<T>(0L);
}

} // namespace ava_utils
