#pragma once

#include "ava/core/Application.h"
#include <fstream>
#include <mutex>
#include <string>
#include "debug.h"

class Application : public ava::core::Application
{
 private:
  static std::ofstream log_;
  static std::mutex log_mutex_;
  std::string application_name_;

 private:
  static bool prepare_debug()
  {
    log_.open("debug.out");
    Debug(libcw_do.set_ostream(&log_, &log_mutex_));
    return true;
  }

 public:
  Application(std::string application_name) : ava::core::Application(prepare_debug()), application_name_(std::move(application_name)) { }

  [[nodiscard]] std::string_view application_name() const noexcept override { return application_name_; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
