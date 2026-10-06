#include "sys.h"
#include "Data.h"
#include "FrontEnd.h"

namespace ava::app::frontend {

Data::Data(FrontEnd* frontend) : frontend_(frontend), index_(frontend->register_data(this))
{
}

void Data::notify()
{
  frontend_->data_changed(index_);
}

} // namespace ava::app::frontend
