#include "Editor.h"
#include <algorithm>
#include <stdexcept>

namespace MouseControl {
namespace {
void Range(const std::wstring &text, std::size_t begin, std::size_t end) {
    if (begin > end || end > text.size())
        throw std::invalid_argument("Invalid editor selection.");
}
std::size_t Start(const std::wstring &text, std::size_t at) {
    if (!at)
        return 0;
    const auto pos = text.rfind(L'\n', at - 1);
    return pos == std::wstring::npos ? 0 : pos + 1;
}
bool Space(wchar_t c) {
    return c == L' ' || c == L'\t';
}
TextEdit Replace(const std::wstring &text, std::size_t begin, std::size_t end,
                 const std::wstring &add, std::size_t caret = std::wstring::npos) {
    Range(text, begin, end);
    auto out = text;
    out.replace(begin, end - begin, add);
    const auto at = begin + (caret == std::wstring::npos ? add.size() : caret);
    return {std::move(out), at, at};
}
} // namespace
std::wstring WindowsLines(const std::wstring &text) {
    std::wstring out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                ++i;
            out += L"\r\n";
        } else if (text[i] == L'\n')
            out += L"\r\n";
        else
            out += text[i];
    }
    return out;
}
std::wstring IndentCode(const std::wstring &source) {
    const auto text = WindowsLines(source);
    std::wstring out;
    unsigned depth = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto next = text.find(L"\r\n", at);
        const auto end = next == std::wstring::npos ? text.size() : next;
        auto line = text.substr(at, end - at);
        const auto first = line.find_first_not_of(L" \t");
        if (first != std::wstring::npos) {
            line.erase(0, first);
            unsigned closing = 0;
            while (closing < line.size() && line[closing] == L'}')
                ++closing;
            const auto indent = depth > closing ? depth - closing : 0;
            out.append(indent * 4, L' ');
            out += line;
            const auto comment = line.find(L"//");
            for (std::size_t i = 0; i < line.size() && i < comment; ++i) {
                if (line[i] == L'{')
                    depth = std::min(16u, depth + 1);
                else if (line[i] == L'}' && depth)
                    --depth;
            }
        }
        if (next == std::wstring::npos)
            break;
        out += L"\r\n";
        at = next + 2;
    }
    return out;
}
TextEdit Newline(const std::wstring &text, std::size_t begin, std::size_t end) {
    Range(text, begin, end);
    const auto start = Start(text, begin);
    std::size_t first = start;
    while (first < begin && Space(text[first]))
        ++first;
    const auto indent = text.substr(start, first - start);
    auto left = begin;
    while (left > start && Space(text[left - 1]))
        --left;
    const auto comment = text.find(L"//", start);
    const bool brace = left > start && text[left - 1] == L'{' && !(comment < begin);
    const auto next = text.find_first_not_of(L" \t", end);
    const auto extra = brace ? L"    " : L"";
    std::wstring add = L"\r\n" + indent + extra;
    const auto caret = add.size();
    if (brace && next != std::wstring::npos && text[next] == L'}') {
        add += L"\r\n" + indent;
        return Replace(text, begin, next, add, caret);
    }
    return Replace(text, begin, end, add);
}
TextEdit Tab(const std::wstring &text, std::size_t begin, std::size_t end, bool outdent) {
    Range(text, begin, end);
    if (begin == end && !outdent)
        return Replace(text, begin, end, L"    ");
    const auto start = Start(text, begin);
    const auto last = Start(text, end > begin ? end - 1 : end);
    auto out = text;
    std::size_t at = start;
    std::ptrdiff_t offset = 0, startDelta = 0, endDelta = 0;
    while (at <= last) {
        const auto actual = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + offset);
        std::size_t removed = 0;
        if (outdent) {
            while (removed < 4 && actual + removed < out.size() && out[actual + removed] == L' ')
                ++removed;
            if (!removed && actual < out.size() && out[actual] == L'\t')
                removed = 1;
            out.erase(actual, removed);
            offset -= static_cast<std::ptrdiff_t>(removed);
            endDelta -= static_cast<std::ptrdiff_t>(std::min(removed, end - at));
            if (at == start)
                startDelta = -static_cast<std::ptrdiff_t>(std::min(removed, begin - start));
        } else {
            out.insert(actual, L"    ");
            offset += 4;
            endDelta += 4;
            if (at == start)
                startDelta = 4;
        }
        const auto next = text.find(L'\n', at);
        if (next == std::wstring::npos)
            break;
        at = next + 1;
    }
    const auto adjusted = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(end) + endDelta);
    const auto first = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(begin) + startDelta);
    return {std::move(out), first, begin == end ? first : adjusted};
}
TextEdit ClosingBrace(const std::wstring &text, std::size_t begin, std::size_t end) {
    Range(text, begin, end);
    const auto start = Start(text, begin);
    bool whitespace = true;
    for (auto i = start; i < begin; ++i)
        whitespace &= Space(text[i]);
    if (whitespace && begin > start) {
        const auto removed = text[begin - 1] == L'\t' ? 1 : std::min<std::size_t>(4, begin - start);
        return Replace(text, begin - removed, end, L"}");
    }
    return Replace(text, begin, end, L"}");
}
} // namespace MouseControl
