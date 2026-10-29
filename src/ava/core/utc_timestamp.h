#pragma once

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace ava::core {

// Preserve the whole-second UTC representation used by session records and
// persistent permission rules. Clock acquisition remains with the caller.
[[nodiscard]] inline std::string format_utc_timestamp(std::time_t time)
{
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

}  // namespace ava::core
