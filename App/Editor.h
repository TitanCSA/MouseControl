#pragma once
#include <cstddef>
#include <string>
namespace MouseControl {
struct TextEdit {
    std::wstring text;
    std::size_t begin = 0, end = 0;
};
std::wstring WindowsLines(const std::wstring &text);
std::wstring IndentCode(const std::wstring &text);
TextEdit Newline(const std::wstring &text, std::size_t begin, std::size_t end);
TextEdit Tab(const std::wstring &text, std::size_t begin, std::size_t end, bool outdent);
TextEdit ClosingBrace(const std::wstring &text, std::size_t begin, std::size_t end);
} // namespace MouseControl
