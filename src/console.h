#pragma once

#include "common.h"

namespace ep {

// Writes UTF-8 text to stdout when it is redirected, otherwise to the parent
// console (this is a GUI-subsystem program, so it has no console of its own).
void ConsoleWrite(const std::wstring& text);
bool HasConsoleOutput();

}  // namespace ep
