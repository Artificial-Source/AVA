// The exit codes that main can return, grouped by the kind of failure that caused them.
#pragma once

#include "ava/debug/print_members_on.h"
#include "utils/AIAlert.h"
#include <cstdlib>
#include <ostream>
#include <system_error>

namespace ava::app {

// The value returned by main, named after its cause.
//
// Several names currently share the value 1; they exist so that every place that ends AVA states why,
// and so that a category can later get a value of its own without revisiting those places.
// Only the low eight bits reach the parent process, so every value must be in the range [0, 255].
//
// This header is part of AVA::app_common so that the TUI may use it too; it is header-only.
enum class ExitCode : int
{
  success = EXIT_SUCCESS,
  usage = 2,                    // The command line, or an answer to an interactive question, is invalid.
  interrupted = 130,            // SIGINT, SIGTERM or SIGHUP ended the run.
  environment_unavailable = 1,  // A resource AVA needs to start is missing: memory, storage, process supervision, a terminal, ...
  configuration_invalid = 1,    // The provider or model configuration can not be used.
  session_unavailable = 1,      // The requested session could not be opened.
  credentials_unavailable = 1,  // A provider credential could not be obtained, read or stored.
  run_failed = 1,               // AVA started correctly, but the requested work failed.
  terminal_io_failed = 1,       // Reading from or writing to AVA's own terminal or standard streams failed.
  cleanup_incomplete = 1,       // The work finished, but shutdown could not release everything.
  check_failed = 1,             // A diagnostic command ran correctly and reports a failure.
  internal_error = 1,           // An unexpected exception or other internal failure.
};

// Return an std::error_code that carries exit_code; defined in Application.cpp (AVA::app).
//
// This allows an AIAlert::ErrorCode thrown with an ExitCode to tell main which value to return.
std::error_code make_error_code(ExitCode exit_code);

// Write exit_code to os as its numeric value.
inline std::ostream& operator<<(std::ostream& os, ExitCode exit_code)
{
  os << static_cast<int>(exit_code);
  return os;
}

} // namespace ava::app

// Register ava::app::ExitCode as an error code enum, so it converts implicitly to std::error_code.
//
// This must be visible in every translation unit that uses ExitCode, hence it is here.
template <>
struct std::is_error_code_enum<ava::app::ExitCode> : std::true_type
{
};

/// Throw alert message with exit code (ava::app::AIAlert::ExitCode) - not modal, no function prefix.
#define THROW_EXIT_CODE(enum_name, ...) throw ava::app::AIAlert::ExitCode(ava::app::ExitCode::enum_name, __VA_ARGS__)
#define THROW_EXIT_CODE_V(code, ...) throw ava::app::AIAlert::ExitCode(code, __VA_ARGS__)

namespace ava::app::AIAlert {

//
// Exception class used to throw an exit code with an informative user message.
//
// Same as Error but allows to pass an additional exit code.
//
class ExitCode : public ::AIAlert::Error
{
 private:
  std::error_code mCode;        //< The underlaying error code.

 public:
  // Accessor for the error code.
  std::error_code getCode() const { return mCode; }

  // Construct just an ExitCode with an exit code (no args).
  ExitCode(ava::app::ExitCode code, Error const& alert) :
    Error({}, ::AIAlert::not_modal, alert), mCode(::AIAlert::convert_to_error_code(code)) { }

  // Construct a string with zero or more replacements (with args).
  ExitCode(ava::app::ExitCode code, std::string const& xml_desc, AIArgs const& args = AIArgs()) :
    Error({}, ::AIAlert::not_modal, xml_desc, args), mCode(::AIAlert::convert_to_error_code(code)) { }

  // Construct a string (with args), prepending the message with the text of another alert.
  ExitCode(ava::app::ExitCode code, Error const& alert, std::string const& xml_desc, AIArgs const& args = AIArgs()) :
    Error({}, ::AIAlert::not_modal, alert, xml_desc, args), mCode(::AIAlert::convert_to_error_code(code)) { }

  // Construct a string (no args), appending the message with the text of another alert (no args).
  ExitCode(ava::app::ExitCode code, std::string const& xml_desc, Error const& alert) :
    Error({}, ::AIAlert::not_modal, xml_desc, alert), mCode(::AIAlert::convert_to_error_code(code)) { }

  // Construct an Error with a code, appending the message with the text of another alert (with args).
  ExitCode(ava::app::ExitCode code, std::string const& xml_desc, AIArgs const& args, Error const& alert) :
    Error({}, ::AIAlert::not_modal, xml_desc, args, alert), mCode(::AIAlert::convert_to_error_code(code)) { }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace ava::app::AIAlert
