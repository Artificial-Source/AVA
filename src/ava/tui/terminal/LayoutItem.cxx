#include "sys.h"
#include "LayoutItem.h"

namespace ava::tui::terminal {

Width& Width::operator+=(Width w)
{
  // Don't try to add Width's with unknown size.
  ASSERT(columns_ != unknown && w.columns_ != unknown);
  if (columns_ == unlimited || w.columns_ == unlimited)
    columns_ = unlimited;
  else
    columns_ += w.columns_;   // This should be always way smaller than `unlimited`.
  return *this;
}

Width operator+(Width w1, Width w2)
{
  Width sum{w1};
  sum += w2;
  return sum;
}

Width operator-(Width w1, Width w2)
{
  // Don't try to subtract Width's with unknown size.
  ASSERT(!w1.is_unknown() && !w2.is_unknown());
  // You are not allowed to substract a width that is larger: the result must be non-negative.
  ASSERT(w1.columns_ >= w2.columns_);
  // Something is wrong with the caller if w2 is unlimted.
  ASSERT(!w2.is_unlimited());
  if (w1.is_unlimited())
    return w1;
  return w1.columns_ - w2.columns_;
}

bool operator<(Width w1, Width w2)
{
  // Don't try to compare Width's with unknown size.
  ASSERT(!w1.is_unknown() && !w2.is_unknown());
  return w1.columns_ < w2.columns_;
}

} // namespace ava::tui::terminal
