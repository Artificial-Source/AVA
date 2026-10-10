#include "sys.h"
#include "Data.h"
#include "FrontEnd.h"

namespace ava::app::frontend {

Data::Data(FrontEnd* frontend) : frontend_(frontend), index_(frontend->register_data(this))
{
}

void Data::notify()
{
  if (frontend_)
    frontend_->data_changed(index_);
}

#ifdef CWDEBUG
void Data::print_members(std::ostream& os, char const* prefix) const
{
  os << prefix;
  if (!frontend_)
    os << "<unregistered>";
  else
    os << "frontend:" << frontend_ << ", index:" << index_;
}
#endif

} // namespace ava::app::frontend
