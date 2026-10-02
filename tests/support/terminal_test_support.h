#pragma once

#include <cstdio>

enum class SupportedMode
{
  KittyProtocol,
  ModifyOtherKeys,
  None
};

void reset_output_file(FILE* file);
// Replace the process terminal's simulated input with a fresh byte sequence after initialization has consumed its startup replies.
void reset_terminal_input_file(FILE* file);
void write_OSC4_reply(FILE* file, int number_of_colors);
// Write realistic, phase-ordered Kitty and fallback keyboard negotiation replies without flushing `file`.
void write_KeyboardInputMode_reply(FILE* file, SupportedMode mode);

// Return the process-lifetime input stream used by the Application-owned terminal Context.
FILE* process_terminal_test_input();

// Return the process-lifetime output stream used by the Application-owned terminal Context.
FILE* process_terminal_test_output();
