#pragma once
#include "Core.h"
#include <windows.h>
#include <string>
namespace MouseControl {
Settings Load(std::wstring &notice);
void Save(const Settings &settings);
void OpenSetup(HWND owner);
std::wstring KeyName(Key key);
std::wstring Wide(const std::string &text);
std::string Utf8(const std::wstring &text);
} // namespace MouseControl
