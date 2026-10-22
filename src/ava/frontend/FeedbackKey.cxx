#include "sys.h"
#include "FeedbackKey.h"
#ifdef CWDEBUG
#include <iostream>
#include <iomanip>
#endif

namespace ava::frontend {

#ifdef CWDEBUG
void FeedbackKey::print_on(std::ostream& os) const
{
  os << '{';
  if (type_ == KeyType::normal)
    os << "normal:" << std::hex << (int)value_.wch << std::dec;
  os << '}';
}
#endif

} // namespace ava::frontend
