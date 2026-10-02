#include "console.h"

namespace ep {
namespace {

HANDLE g_out = nullptr;
bool g_isConsole = false;

void Init() {
    static bool done = false;
    if (done) return;
    done = true;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN && GetFileType(h) != FILE_TYPE_CHAR) {
        g_out = h;  // pipe or file
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        g_isConsole = g_out != INVALID_HANDLE_VALUE;
        if (!g_isConsole) g_out = nullptr;
    }
}

}  // namespace

bool HasConsoleOutput() {
    Init();
    return g_out != nullptr;
}

void ConsoleWrite(const std::wstring& text) {
    Init();
    if (!g_out) return;
    DWORD written = 0;
    if (g_isConsole) {
        WriteConsoleW(g_out, text.c_str(), (DWORD)text.size(), &written, nullptr);
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), n, nullptr, nullptr);
    WriteFile(g_out, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

}  // namespace ep
