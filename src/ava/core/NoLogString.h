#pragma once

#include <string>

namespace ava::core {

// Mark owned text that must not be included in diagnostic logging, including credentials and private content.
// This alias records the logging policy without changing string operations or ownership.
// It is not a distinct C++ type and does not itself redact output; logging and code-generation support must honor the annotation.
using NoLogString = std::string;

} // namespace ava::core
