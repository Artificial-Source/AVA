#pragma once

// Define LIBCWD_USING_OSTREAM_PRELUDE and AVA_USING_OSTREAM_PRELUDE.
#include "src/ava/debug/AVA_USING_OSTREAM_PRELUDE.h"

#include "cwds/debug.h"

// define AVA_DEBUG_PRINT_MEMBERS_ON.
#include "src/ava/debug/print_members_on.h"

#ifdef CWDEBUG
// Declare all debug channels.
#include "src/ava/debug/debug_channels.h"
// Make print_on members visible.
#include "src/ava/debug/ava_print_on.h"
// Make the maxlen() debug IO manipulator available inside Dout() expressions.
#include "src/ava/debug/maxlen.h"
#endif
