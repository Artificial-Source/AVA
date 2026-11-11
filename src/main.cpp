#include "sys.h"
#include "ava/app/Application.h"
#include "utils/AIAlert.h"

#include <iostream>
#include <string>
#include <utility>

namespace {

// Print error to std::cerr, prefixed with the program name and ": ".
void print_error(char const* program, AIAlert::Error const& error)
{
  std::string error_prefix = program;
  error_prefix += ": ";
  std::cerr << AIAlert::ErrorPrefix(error_prefix.c_str()) << error << std::endl;
}

} // namespace

int main(int argc, char** argv)
{
  int exit_code;
  try
  {
    ava::app::Application application;
    application.run(argc, argv);
    exit_code = std::to_underlying(application.exit_code());
  }
  // We get here when it is impossible to create or run the application.
  catch (ava::app::AIAlert::ExitCode const& error)
  {
    // Thrown on purpose: the code is the exit code that main must return.
    exit_code = error.getCode().value();
    print_error(argv[0], error);
  }
  catch (AIAlert::Error const& error)
  {
    exit_code = std::to_underlying(ava::app::ExitCode::internal_error);
    print_error(argv[0], error);
  }
#ifndef CWDEBUG // Commented out in Debug mode so we can see in gdb where an exception is thrown from.
  catch (std::exception& exception)
  {
    DoutFatal(dc::core, "\033[31mstd::exception: " << exception.what() << " caught in src/main.cpp\033[0m");
    exit_code = std::to_underlying(ava::app::ExitCode::internal_error);
  }
#endif
  Dout(dc::notice, "Returning exit code " << exit_code);
  return exit_code;
}
