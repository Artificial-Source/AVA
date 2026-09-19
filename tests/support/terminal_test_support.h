#pragma once

#include <cstdio>

enum class SupportedMode
{
  KittyProtocol,
  ModifyOtherKeys,
  None
};

void reset_output_file(FILE* file);
void write_OSC4_reply(FILE* file, int number_of_colors);
// Write realistic, phase-ordered Kitty and fallback keyboard negotiation replies without flushing `file`.
void write_KeyboardInputMode_reply(FILE* file, SupportedMode mode);
