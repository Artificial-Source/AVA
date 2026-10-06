#include "sys.h"
#include "FrontEnd.h"

namespace ava::app::frontend {

DataListIndex FrontEnd::register_data(Data* data)
{
  DataListIndex index = data_list_.iend();
  data_list_.push_back(data);
  return index;
}

void FrontEnd::on_submit(std::u8string const& input)
{
}

} // namespace ava::app::frontend
