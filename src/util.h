#pragma once

#include "common.h"

namespace ep {

// Full, long-form path without a trailing separator (except for drive roots).
std::wstring NormalizePath(const std::wstring& path);
std::wstring ParentPath(const std::wstring& path);
std::wstring FileNamePart(const std::wstring& path);
bool PathEqualsI(const std::wstring& a, const std::wstring& b);
bool PathExists(const std::wstring& path);

std::wstring ExePath();
std::wstring ExeDirectory();

// UI strings come from the string tables in ExplorerPinned.rc (English and Japanese).
// The language follows the user's display language unless HKCU\Software\ExplorerPinned
// has Language = "ja" or "en".
LANGID UiLanguage();
void SetUiLanguage(const std::wstring& code);  // "ja"/"japanese" or anything else for English
std::wstring LoadStrLang(HMODULE module, UINT id, LANGID language);
std::wstring LoadStr(UINT id);
std::wstring FormatStr(UINT id, const std::wstring& arg);

bool RegWriteString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value);
bool RegReadString(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* value);
bool RegDeleteValueIn(HKEY root, const std::wstring& key, const wchar_t* name);

bool IsElevated();

void LogLine(const wchar_t* fmt, ...);

}  // namespace ep
