#include "UI.h"
#include "Storage.h"
#include <windowsx.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace MouseControl {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT Background = 0x0B0F15, Panel = 0x121923, Field = 0x1A2432, Line = 0x273344,
               Blue = 0x438DFF, White = 0xEDF3FC, Muted = 0x98A8BE, Amber = 0xFFC875;
bool Inside(D2D1_RECT_F b, float x, float y) {
    return x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
}
void Check(HRESULT hr) {
    if (FAILED(hr))
        throw std::system_error(static_cast<int>(hr), std::system_category(),
                                "Render user interface");
}
std::wstring Value(HWND edit) {
    const int n = GetWindowTextLengthW(edit);
    if (n > 18000)
        throw std::invalid_argument("Text is too long.");
    std::wstring value(static_cast<std::size_t>(n) + 1, 0);
    GetWindowTextW(edit, value.data(), n + 1);
    value.resize(n);
    return value;
}
int Number(HWND edit) {
    const auto text = Utf8(Value(edit));
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Enter a whole number.");
    return value;
}
std::wstring StepText(const Step &step) {
    if (step.operation == Operation::Wait)
        return L"wait(" + std::to_wstring(step.milliseconds) + L"ms)";
    return L"move(" + (step.x ? std::to_wstring(step.x) + L", " : std::wstring()) +
           std::to_wstring(step.y) + L")";
}
} // namespace
Window::Window(bool offline) : offline_(offline) {
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()));
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown **>(text_.GetAddressOf())));
    const float sizes[] = {12, 14, 16, 21, 28, 15};
    for (unsigned i = 0; i < fonts_.size(); ++i) {
        Check(text_->CreateTextFormat(
            i == 5 ? L"Consolas" : L"Segoe UI", nullptr,
            i == 3 || i == 4 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizes[i], L"en-us", &fonts_[i]));
        fonts_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    fieldBrush_ = CreateSolidBrush(RGB(26, 36, 50));
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    animate_ = animations != FALSE;
    if (offline_) {
        state_.connected = true;
        state_.title = L"Ready · hold your trigger";
        state_.target = L"Physical mouse · automatic source";
    } else
        settings_ = Load(notice_);
}
Window::~Window() {
    engine_.Stop();
    if (window_ && IsWindow(window_))
        DestroyWindow(window_);
    if (font_)
        DeleteObject(font_);
    if (mono_)
        DeleteObject(mono_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
}
D2D1_RECT_F Window::Box(int id) const {
    const float right = width_ - 408, edge = width_ - 432, editor = height_ - 272;
    if (id == 1)
        return D2D1::RectF(width_ - 164, 28, width_ - 32, 70);
    if (id == 2)
        return D2D1::RectF(width_ - 332, 28, width_ - 180, 70);
    if (id == 3)
        return D2D1::RectF(56, height_ - 150, 170, height_ - 110);
    if (id == 4)
        return D2D1::RectF(182, height_ - 150, 296, height_ - 110);
    if (id == 5)
        return D2D1::RectF(edge - 168, height_ - 150, edge - 128, height_ - 110);
    if (id == 6)
        return D2D1::RectF(edge - 120, height_ - 150, edge - 80, height_ - 110);
    if (id == 7)
        return D2D1::RectF(edge - 72, height_ - 150, edge - 24, height_ - 110);
    if (id == 8)
        return D2D1::RectF(edge - 200, 185, edge - 116, 223);
    if (id == 9)
        return D2D1::RectF(edge - 110, 185, edge - 26, 223);
    if (id == 10)
        return D2D1::RectF(right + 24, 308, right + 218, 345);
    if (id >= 20 && id < 25) {
        const float x = right + 24 + (id - 20) * 66.0f;
        return D2D1::RectF(x, 252, x + 58, 291);
    }
    if (id >= 40 && id < 48) {
        const unsigned n = static_cast<unsigned>(id - 40);
        const float x = right + 24 + (n % 3) * 110.0f, y = 358 + (n / 3) * 31.0f;
        return D2D1::RectF(x, y, x + 102, y + 25);
    }
    if (id >= 100) {
        const int row = id - 100 - scroll_;
        return D2D1::RectF(56, 290 + row * 65.0f, edge - 24, 348 + row * 65.0f);
    }
    if (id == 11)
        return D2D1::RectF(56, editor + 42, edge - 24, editor + 80);
    return {};
}
int Window::Hit(float x, float y) const {
    if (state_.error && Inside(D2D1::RectF(32, 108, width_ - 32, 150), x, y))
        return 12;
    for (int id : {1, 2, 8, 9, 10})
        if (Inside(Box(id), x, y))
            return id;
    for (int id = 20; id < 25; ++id)
        if (Inside(Box(id), x, y))
            return id;
    for (std::size_t i = 0; i < settings_.keys.size(); ++i)
        if (Inside(Box(40 + static_cast<int>(i)), x, y))
            return 40 + static_cast<int>(i);
    if (!codeView_) {
        for (int id : {3, 4, 5, 6, 7})
            if (Inside(Box(id), x, y))
                return id;
        if (y >= 290 && y < height_ - 294) {
            const int row = scroll_ + static_cast<int>((y - 290) / 65);
            if (row >= 0 &&
                row < static_cast<int>(settings_.program.start.size() +
                                       settings_.program.loop.size()) &&
                Inside(Box(100 + row), x, y))
                return 100 + row;
        }
    }
    return 0;
}
Step *Window::Selected() {
    if (selected_ < 0)
        return nullptr;
    const auto index = static_cast<std::size_t>(selected_);
    if (index < settings_.program.start.size())
        return &settings_.program.start[index];
    if (index - settings_.program.start.size() < settings_.program.loop.size())
        return &settings_.program.loop[index - settings_.program.start.size()];
    return nullptr;
}
void Window::Create(bool visible) {
    WNDCLASSEXW type{sizeof(type)};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = Procedure;
    type.lpszClassName = L"TitanCSA.MouseControl.Window";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(101));
    type.hIconSm = type.hIcon;
    RegisterClassExW(&type);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = std::min<int>(1380, work.right - work.left - 40),
              h = std::min<int>(940, work.bottom - work.top - 40);
    window_ = CreateWindowExW(0, type.lpszClassName, L"MouseControl",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, work.left + 20, work.top + 20,
                              w, h, nullptr, nullptr, type.hInstance, this);
    if (!window_)
        throw std::system_error(GetLastError(), std::system_category(),
                                "Create MouseControl window");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    const auto edit = [&](int id, DWORD style) {
        return CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | style, 0, 0, 0, 0, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), type.hInstance,
                               nullptr);
    };
    editA_ = edit(201, ES_AUTOHSCROLL);
    editB_ = edit(202, ES_AUTOHSCROLL);
    gap_ = edit(203, ES_AUTOHSCROLL);
    code_ = edit(204, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL);
    if (!editA_ || !editB_ || !gap_ || !code_)
        throw std::runtime_error("Could not create sequence editors.");
    SendMessageW(editA_, EM_SETLIMITTEXT, 12, 0);
    SendMessageW(editB_, EM_SETLIMITTEXT, 12, 0);
    SendMessageW(gap_, EM_SETLIMITTEXT, 4, 0);
    SendMessageW(code_, EM_SETLIMITTEXT, 16384, 0);
    Layout();
    RefreshEditors();
    if (!offline_) {
        engine_.Start(window_, settings_);
        if (!RegisterHotKey(window_, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12))
            notice_ = L"Ctrl+Alt+F12 is already registered by another app. The Pause button "
                      L"remains available.";
    }
    if (visible) {
        ShowWindow(window_, SW_SHOWNORMAL);
        UpdateWindow(window_);
    }
}
void Window::Layout() {
    if (!window_)
        return;
    RECT client{};
    GetClientRect(window_, &client);
    scale_ = std::min(
        {GetDpiForWindow(window_) / 96.0f, client.right / 1120.0f, client.bottom / 760.0f});
    scale_ = std::max(.5f, scale_);
    width_ = client.right / scale_;
    height_ = client.bottom / scale_;
    if (target_)
        target_->Resize(D2D1::SizeU(client.right, client.bottom));
    HFONT old = font_, oldMono = mono_;
    font_ = CreateFontW(-static_cast<int>(14 * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Segoe UI");
    mono_ = CreateFontW(-static_cast<int>(15 * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        FIXED_PITCH, L"Consolas");
    const auto place = [&](HWND control, float x, float y, float w, float h, bool show,
                           HFONT font) {
        if (!control)
            return;
        MoveWindow(control, static_cast<int>(x * scale_), static_cast<int>(y * scale_),
                   static_cast<int>(w * scale_), static_cast<int>(h * scale_), TRUE);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        ShowWindow(control, show ? SW_SHOW : SW_HIDE);
    };
    auto *step = Selected();
    const float bottom = height_ - 272, edge = width_ - 432, right = width_ - 408;
    place(editA_, 240, bottom + 43, 110, 30, !codeView_ && step, font_);
    place(editB_, edge - 150, bottom + 43, 110, 30,
          !codeView_ && step && step->operation == Operation::Move, font_);
    place(gap_, right + 250, 466, 72, 27, true, font_);
    place(code_, 64, 270, edge - 104, height_ - 392, codeView_, mono_);
    if (old)
        DeleteObject(old);
    if (oldMono)
        DeleteObject(oldMono);
    InvalidateRect(window_, nullptr, FALSE);
}
void Window::RefreshEditors() {
    if (!window_)
        return;
    changing_ = true;
    const auto *step = Selected();
    SetWindowTextW(editA_,
                   step ? (step->operation == Operation::Wait ? std::to_wstring(step->milliseconds)
                                                              : std::to_wstring(step->x))
                              .c_str()
                        : L"");
    SetWindowTextW(editB_, step ? std::to_wstring(step->y).c_str() : L"");
    SetWindowTextW(gap_, std::to_wstring(settings_.loopGapMs).c_str());
    SetWindowTextW(code_, Wide(Script(settings_.program)).c_str());
    changing_ = false;
    Layout();
}
void Window::Notify(const std::wstring &message) {
    notice_ = message;
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::Changed(bool refreshCode) {
    Validate(settings_);
    codeError_.clear();
    notice_.clear();
    if (!offline_) {
        engine_.Configure(settings_);
        SetTimer(window_, 3, 500, nullptr);
    }
    if (refreshCode && window_ && !codeView_) {
        changing_ = true;
        SetWindowTextW(code_, Wide(Script(settings_.program)).c_str());
        changing_ = false;
    }
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::Edit(int id) {
    if (changing_)
        return;
    if (id == 204) {
        SetTimer(window_, 2, 220, nullptr);
        return;
    }
    const auto before = settings_;
    try {
        if (id == 203) {
            const int number = Number(gap_);
            if (number < 1 || number > 1000)
                throw std::invalid_argument("Loop gap: 1–1000 ms.");
            settings_.loopGapMs = static_cast<unsigned>(number);
        } else if (auto *step = Selected()) {
            if (step->operation == Operation::Wait) {
                const int value = Number(editA_);
                if (value < 0 || value > 600000)
                    throw std::invalid_argument("Wait: 0–600000 ms.");
                step->milliseconds = static_cast<unsigned>(value);
            } else {
                step->x = Number(editA_);
                step->y = Number(editB_);
            }
        }
        Changed();
    } catch (const std::exception &error) {
        settings_ = before;
        Notify(Wide(error.what()));
    }
}
void Window::Action(int id) {
    const auto before = settings_;
    try {
        if (id == 1) {
            paused_ = state_.error ? false : !paused_;
            state_.error = 0;
            state_.paused = paused_;
            if (!offline_)
                engine_.Running(!paused_);
            else
                state_.title = paused_ ? L"Paused" : L"Ready · hold your trigger";
        } else if (id == 2) {
            if (!offline_)
                OpenSetup(window_);
            else
                Notify(L"Offline preview: setup was not launched.");
        } else if (id == 12) {
            MessageBoxW(window_, state_.detail.c_str(), L"MouseControl · driver details",
                        MB_OK | MB_ICONINFORMATION);
        } else if (id == 8 || id == 9) {
            codeView_ = id == 9;
            codeError_.clear();
            RefreshEditors();
        } else if (id == 10) {
            if (!offline_ && !state_.connected) {
                Notify(L"Install the driver and press Retry before recording a keyboard key.");
                return;
            }
            state_.recording = !state_.recording;
            if (!offline_)
                engine_.RecordKey(state_.recording);
            Notify(state_.recording ? L"Press a keyboard key to add it. Escape cancels."
                                    : L"Key recording cancelled.");
        } else if (id >= 20 && id < 25) {
            settings_.mouseButtons ^= static_cast<std::uint8_t>(1u << (id - 20));
            Changed();
        } else if (id >= 40 && id < 48) {
            const auto index = static_cast<std::size_t>(id - 40);
            if (index < settings_.keys.size()) {
                settings_.keys.erase(settings_.keys.begin() + index);
                Changed();
            }
        } else if (id >= 100) {
            selected_ = id - 100;
            RefreshEditors();
        } else if (id == 3 || id == 4) {
            if (settings_.program.loop.size() >= 64)
                throw std::invalid_argument("The loop already has 64 steps.");
            settings_.program.loop.push_back(id == 3 ? Step::Move(0, -10) : Step::Wait(50));
            selected_ = static_cast<int>(settings_.program.start.size() +
                                         settings_.program.loop.size() - 1);
            scroll_ = std::max(0, selected_ - 2);
            Changed();
            RefreshEditors();
        } else if (id == 5 || id == 6 || id == 7) {
            const auto start = settings_.program.start.size();
            if (selected_ < 0)
                return;
            const bool initial = static_cast<std::size_t>(selected_) < start;
            auto &list = initial ? settings_.program.start : settings_.program.loop;
            const auto index = initial ? static_cast<std::size_t>(selected_)
                                       : static_cast<std::size_t>(selected_) - start;
            if (index >= list.size())
                return;
            if (id == 7) {
                if (list.size() == 1 &&
                    settings_.program.start.size() + settings_.program.loop.size() == 1)
                    throw std::invalid_argument("Keep at least one step.");
                list.erase(list.begin() + index);
                selected_ = std::max(0, selected_ - 1);
            } else if (id == 5 && index) {
                std::swap(list[index], list[index - 1]);
                --selected_;
            } else if (id == 6 && index + 1 < list.size()) {
                std::swap(list[index], list[index + 1]);
                ++selected_;
            }
            Changed();
            RefreshEditors();
        }
    } catch (const std::exception &error) {
        settings_ = before;
        Notify(Wide(error.what()));
        RefreshEditors();
    }
    Animate();
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::SnapshotChanged() {
    state_ = engine_.Read();
    paused_ = state_.paused;
    if (state_.recordVersion != recorded_) {
        recorded_ = state_.recordVersion;
        if (state_.recorded) {
            auto next = settings_;
            if (std::find(next.keys.begin(), next.keys.end(), *state_.recorded) == next.keys.end())
                next.keys.push_back(*state_.recorded);
            try {
                Validate(next);
                settings_ = next;
                Changed();
            } catch (const std::exception &e) {
                Notify(Wide(e.what()));
            }
        } else
            Notify(L"Key recording cancelled.");
    }
    Animate();
    InvalidateRect(window_, nullptr, FALSE);
}
void Window::Animate() {
    if (!window_)
        return;
    if (IsIconic(window_)) {
        KillTimer(window_, 1);
        return;
    }
    const bool transition = hover_ ? hoverMix_ < .995f : hoverMix_ > .005f;
    if (animate_ && (state_.active || transition))
        SetTimer(window_, 1, 16, nullptr);
    else {
        KillTimer(window_, 1);
        hoverMix_ = hover_ ? 1.0f : 0.0f;
    }
}
void Window::Draw(ID2D1RenderTarget *rt) {
    ComPtr<ID2D1SolidColorBrush> brush;
    Check(rt->CreateSolidColorBrush(D2D1::ColorF(White), &brush));
    const auto color = [&](UINT c, float alpha = 1) { brush->SetColor(D2D1::ColorF(c, alpha)); };
    const auto text = [&](const std::wstring &s, float x, float y, float w, UINT c = White,
                          unsigned font = 1, float h = 30) {
        color(c);
        rt->DrawText(s.data(), static_cast<UINT32>(s.size()), fonts_[font].Get(),
                     D2D1::RectF(x, y, x + w, y + h), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto rect = [&](D2D1_RECT_F r, UINT c, float radius = 12, float alpha = 1) {
        color(c, alpha);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    };
    const auto stroke = [&](D2D1_RECT_F r, UINT c, float radius = 12) {
        color(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    };
    const auto button = [&](int id, const std::wstring &label, bool selected = false,
                            bool primary = false) {
        auto b = Box(id);
        rect(b, primary ? Blue : selected ? 0x203E67 : Field, 10);
        if (id == hover_)
            rect(b, White, 10, .06f + hoverMix_ * .04f);
        if (selected)
            stroke(b, Blue, 10);
        text(label, b.left + 14, b.top + 10, b.right - b.left - 20,
             primary    ? 0xFFFFFF
             : selected ? 0x9CC5FF
                        : White,
             1);
    };
    rt->Clear(D2D1::ColorF(Background));
    // A quiet blue header glow; Direct2D text and geometry remain sharp at any DPI.
    rect(D2D1::RectF(0, 0, width_, 96), 0x0E1725, 0);
    rect(D2D1::RectF(32, 28, 76, 72), Blue, 14);
    color(White);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(46, 37, 62, 63), 8, 8), brush.Get(),
                             1.7f);
    rt->DrawLine(D2D1::Point2F(54, 38), D2D1::Point2F(54, 46), brush.Get(), 1.5f);
    text(L"MouseControl", 92, 25, 300, White, 4, 38);
    text(L"Hold a trigger. Add movement. Release to stop.", 93, 65, 460, Muted, 0);
    button(2, L"Driver setup");
    button(1, state_.error ? L"↻  Retry" : paused_ ? L"▶  Resume" : L"Ⅱ  Pause", false, true);
    rect(D2D1::RectF(32, 108, width_ - 32, 150), Panel, 12);
    color(state_.error ? Amber : Blue);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(51, 129), 4, 4), brush.Get());
    text(state_.recording ? L"Press a key to add it · Escape cancels" : state_.title, 65, 119,
         width_ - 330, state_.error ? Amber : White, 1);
    text(L"Ctrl + Alt + F12  ·  pause", width_ - 262, 120, 218, Muted, 0);
    const float edge = width_ - 432, right = width_ - 408;
    rect(D2D1::RectF(32, 166, edge, height_ - 84), Panel, 18);
    stroke(D2D1::RectF(32, 166, edge, height_ - 84), Line, 18);
    text(L"Movement sequence", 56, 188, 370, White, 3);
    button(8, L"Visual", !codeView_);
    button(9, L"Code", codeView_);
    text(L"Real relative input, added to your physical mouse movement.", 56, 229, edge - 90, Muted,
         0);
    if (codeView_) {
        rect(D2D1::RectF(56, 260, edge - 24, height_ - 108), Background, 12);
        text(Wide(Script(settings_.program)), 68, 273, edge - 106, 0xA8CAFF, 5, height_ - 402);
    } else {
        text(L"ORDER", 70, 267, 80, Muted, 0);
        text(L"ACTION", 182, 267, 240, Muted, 0);
        text(L"Click a step to edit", edge - 170, 267, 150, Muted, 0);
        const auto total = settings_.program.start.size() + settings_.program.loop.size();
        const float limit = height_ - 294;
        rt->PushAxisAlignedClip(D2D1::RectF(50, 289, edge - 20, limit),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (std::size_t i = static_cast<std::size_t>(scroll_); i < total; ++i) {
            const auto row = Box(100 + static_cast<int>(i));
            if (row.top >= limit)
                break;
            const bool initial = i < settings_.program.start.size();
            const auto &step = initial ? settings_.program.start[i]
                                       : settings_.program.loop[i - settings_.program.start.size()];
            const bool selected = static_cast<int>(i) == selected_;
            rect(row, selected ? 0x1C304B : Field, 11);
            if (selected)
                stroke(row, Blue, 11);
            text(initial ? L"START" : L"LOOP", row.left + 14, row.top + 11, 72,
                 initial ? Muted : 0x9CC5FF, 0);
            text(initial ? L"once" : L"while held", row.left + 14, row.top + 30, 94, Muted, 0);
            text(StepText(step), row.left + 126, row.top + 16, row.right - row.left - 170, White,
                 5);
            if (selected)
                text(L"•", row.right - 30, row.top + 13, 20, Blue, 2);
        }
        rt->PopAxisAlignedClip();
        const auto editor = height_ - 272;
        rect(D2D1::RectF(56, editor, edge - 24, editor + 88), Background, 12);
        if (auto *step = Selected()) {
            text(step->operation == Operation::Wait ? L"Wait duration" : L"Relative movement", 72,
                 editor + 13, 220, White, 1);
            text(step->operation == Operation::Wait ? L"Milliseconds" : L"Horizontal X", 240,
                 editor + 14, 155, Muted, 0);
            rect(D2D1::RectF(232, editor + 37, 358, editor + 78), Field, 8);
            text(step->operation == Operation::Wait ? std::to_wstring(step->milliseconds)
                                                    : std::to_wstring(step->x),
                 240, editor + 44, 110, White, 1);
            if (step->operation == Operation::Move) {
                text(L"Vertical Y", edge - 150, editor + 14, 130, Muted, 0);
                rect(D2D1::RectF(edge - 158, editor + 37, edge - 32, editor + 78), Field, 8);
                text(std::to_wstring(step->y), edge - 150, editor + 44, 110, White, 1);
                text(L"− down  ·  + up", 72, editor + 45, 150, Muted, 0);
            } else
                text(L"Release cancels the wait", 72, editor + 45, 158, Muted, 0);
        }
        button(3, L"＋ Move");
        button(4, L"＋ Wait");
        button(5, L"↑");
        button(6, L"↓");
        button(7, L"×");
    }
    rect(D2D1::RectF(right, 166, width_ - 32, 510), Panel, 18);
    stroke(D2D1::RectF(right, 166, width_ - 32, 510), Line, 18);
    text(L"Hold to trigger", right + 24, 190, 310, White, 3);
    text(L"All selected inputs must stay pressed.", right + 24, 223, 330, Muted, 0);
    const wchar_t *labels[] = {L"Left", L"Right", L"Mid", L"X1", L"X2"};
    for (int i = 0; i < 5; ++i)
        button(20 + i, labels[i], (settings_.mouseButtons & (1u << i)) != 0);
    button(10, state_.recording ? L"Press a key…" : L"＋ Keyboard key", state_.recording);
    for (std::size_t i = 0; i < settings_.keys.size(); ++i)
        button(40 + static_cast<int>(i), KeyName(settings_.keys[i]) + L" ×", true);
    if (settings_.keys.empty())
        text(L"Optional: add a key or a keyboard chord.", right + 24, 369, 328, Muted, 0);
    text(L"Loop gap", right + 24, 467, 140, White, 1);
    text(L"ms", right + 330, 468, 30, Muted, 0);
    rect(D2D1::RectF(right + 242, 459, right + 326, 497), Field, 8);
    text(std::to_wstring(settings_.loopGapMs), right + 250, 468, 72);
    const float live = 526, bottom = height_ - 84;
    if (bottom > live + 80) {
        rect(D2D1::RectF(right, live, width_ - 32, bottom), Panel, 18);
        stroke(D2D1::RectF(right, live, width_ - 32, bottom), Line, 18);
        text(L"Added motion", right + 24, live + 23, 290, White, 3);
        if (bottom - live > 212) {
            const float cx = right + 100, cy = live + 114;
            for (int i = -2; i <= 2; ++i) {
                color(Line);
                rt->DrawLine(D2D1::Point2F(cx - 52, cy + i * 20.0f),
                             D2D1::Point2F(cx + 52, cy + i * 20.0f), brush.Get());
                rt->DrawLine(D2D1::Point2F(cx + i * 20.0f, cy - 44),
                             D2D1::Point2F(cx + i * 20.0f, cy + 44), brush.Get());
            }
            int x = 0, y = 0;
            for (const auto &s : settings_.program.loop)
                if (s.operation == Operation::Move) {
                    x = s.x;
                    y = -s.y;
                    break;
                }
            const auto length =
                std::max(1.0, std::hypot(static_cast<double>(x), static_cast<double>(y)));
            const float dx = static_cast<float>(x / length * 38),
                        dy = static_cast<float>(y / length * 38);
            color(Blue);
            rt->DrawLine(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx + dx, cy + dy),
                         brush.Get(), 2.5f);
            const float progress = state_.active ? std::fmod(phase_, 1.0f) : .6f;
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx * (progress * 2 - 1),
                                                        cy + dy * (progress * 2 - 1)),
                                          5, 5),
                            brush.Get());
            text(std::to_wstring(state_.movements), right + 187, live + 78, 158, White, 4, 38);
            text(L"movement packets added", right + 187, live + 118, 165, Muted, 0);
        }
        text(state_.target, right + 24, bottom - 48, 325, White, 0);
        text(L"Same source mouse · physical input preserved", right + 24, bottom - 28, 325, Muted,
             0);
    }
    std::wstring footer =
        offline_ ? L"Offline preview · no driver or input access."
                 : L"Keyboard & Mouse demo · 1 hour per boot · actions run outside this window.";
    const auto warning = !codeError_.empty()      ? codeError_
                         : !notice_.empty()       ? notice_
                         : !state_.detail.empty() ? state_.detail
                                                  : footer;
    text(warning, 34, height_ - 60, width_ - 72,
         (!codeError_.empty() || !notice_.empty() || state_.error) ? Amber : Muted, 0, 45);
}
void Window::Paint() {
    PAINTSTRUCT paint{};
    BeginPaint(window_, &paint);
    try {
        if (!target_) {
            RECT r{};
            GetClientRect(window_, &r);
            Check(factory_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(r.right, r.bottom)),
                &target_));
            target_->SetDpi(96, 96);
        }
        target_->BeginDraw();
        target_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
        Draw(target_.Get());
        const auto result = target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
            target_.Reset();
        else
            Check(result);
    } catch (...) {
        target_.Reset();
    }
    EndPaint(window_, &paint);
}
void Window::Render(const std::wstring &path) {
    width_ = 1320;
    height_ = 860;
    ComPtr<IWICImagingFactory> wic;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&wic)));
    ComPtr<IWICBitmap> bitmap;
    Check(
        wic->CreateBitmap(1320, 860, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap));
    ComPtr<ID2D1RenderTarget> rt;
    Check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &rt));
    rt->BeginDraw();
    Draw(rt.Get());
    Check(rt->EndDraw());
    ComPtr<IWICStream> stream;
    Check(wic->CreateStream(&stream));
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr));
    Check(frame->Initialize(nullptr));
    Check(frame->SetSize(1320, 860));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    Check(frame->SetPixelFormat(&format));
    Check(frame->WriteSource(bitmap.Get(), nullptr));
    Check(frame->Commit());
    Check(encoder->Commit());
}
LRESULT CALLBACK Window::Procedure(HWND window, UINT msg, WPARAM w, LPARAM l) {
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(window, msg, w, l);
    try {
        return self->Message(msg, w, l);
    } catch (const std::exception &error) {
        self->Notify(Wide(error.what()));
        return 0;
    }
}
LRESULT Window::Message(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED: {
        const auto r = reinterpret_cast<RECT *>(l);
        SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {900, 650};
        return 0;
    case WM_ACTIVATE:
        if (!offline_)
            engine_.Suspended(LOWORD(w) != WA_INACTIVE);
        return 0;
    case WM_HOTKEY:
        if (w == 1)
            Action(1);
        return 0;
    case SnapshotMessage:
        SnapshotChanged();
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = Hit(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
        if (hit != hover_) {
            hover_ = hit;
            Animate();
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            tracking_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_ = 0;
        Animate();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(window_, &p);
            SetCursor(LoadCursorW(nullptr, Hit(p.x / scale_, p.y / scale_) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
        Action(Hit(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_));
        return 0;
    case WM_MOUSEWHEEL:
        if (!codeView_) {
            const int count =
                static_cast<int>(settings_.program.start.size() + settings_.program.loop.size());
            scroll_ = std::clamp(scroll_ - (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1), 0,
                                 std::max(0, count - 2));
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE)
            Edit(LOWORD(w));
        else if (HIWORD(w) == EN_KILLFOCUS &&
                 (LOWORD(w) == 201 || LOWORD(w) == 202 || LOWORD(w) == 203))
            RefreshEditors();
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(w), RGB(26, 36, 50));
        SetTextColor(reinterpret_cast<HDC>(w), RGB(237, 243, 252));
        return reinterpret_cast<LRESULT>(fieldBrush_);
    case WM_TIMER:
        if (w == 1) {
            phase_ = std::fmod(phase_ + .025f, 1.0f);
            hoverMix_ += (static_cast<float>(hover_ != 0) - hoverMix_) * .2f;
            Animate();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (w == 2) {
            KillTimer(window_, 2);
            try {
                auto next = settings_;
                next.program = Parse(Utf8(Value(code_)));
                Validate(next);
                settings_ = next;
                selected_ = static_cast<int>(settings_.program.start.size());
                Changed(false);
            } catch (const std::exception &error) {
                codeError_ = Wide(error.what());
                InvalidateRect(window_, nullptr, FALSE);
            }
        } else if (w == 3) {
            KillTimer(window_, 3);
            if (!offline_)
                try {
                    Save(settings_);
                } catch (const std::exception &error) {
                    Notify(L"Settings were not saved: " + Wide(error.what()));
                }
        }
        return 0;
    case WM_CLOSE:
        engine_.Stop();
        if (!offline_)
            try {
                Save(settings_);
            } catch (...) {
            }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(window_, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, w, l);
}
unsigned Window::SelfTest() {
    unsigned checks = 0;
    const auto expect = [&](bool ok) {
        ++checks;
        if (!ok)
            throw std::runtime_error("UI check failed: " + std::to_string(checks));
    };
    expect(offline_);
    expect(settings_.mouseButtons == 1);
    expect(Hit(Box(1).left + 10, 40) == 1);
    Action(21);
    expect(settings_.mouseButtons == 3);
    Action(21);
    expect(settings_.mouseButtons == 1);
    Action(20);
    expect(settings_.mouseButtons == 1);
    Action(3);
    expect(settings_.program.loop.size() == 2);
    Action(4);
    expect(settings_.program.loop.size() == 3);
    Action(5);
    expect(Selected()->operation == Operation::Wait);
    Action(6);
    expect(Selected()->operation == Operation::Wait);
    Action(7);
    expect(settings_.program.loop.size() == 2);
    Action(9);
    expect(codeView_ && IsWindowVisible(code_) == FALSE); // Parent is intentionally hidden.
    changing_ = true;
    SetWindowTextW(code_, L"wait(30ms)\nloop { move(-100) wait(5ms) }\n");
    changing_ = false;
    Message(WM_TIMER, 2, 0);
    expect(settings_.program.loop.size() == 2);
    expect(settings_.program.loop[1].operation == Operation::Wait);
    const auto valid = Script(settings_.program);
    changing_ = true;
    SetWindowTextW(code_, L"loop { invalid(1) }");
    changing_ = false;
    Message(WM_TIMER, 2, 0);
    expect(!codeError_.empty());
    expect(Script(settings_.program) == valid);
    Action(8);
    expect(!codeView_);
    selected_ = 0;
    RefreshEditors();
    SetWindowTextW(editA_, L"42");
    expect(settings_.program.start[0].milliseconds == 42);
    SetWindowTextW(gap_, L"1");
    expect(settings_.loopGapMs == 1);
    SetWindowTextW(gap_, L"0");
    expect(settings_.loopGapMs == 1);
    selected_ = static_cast<int>(settings_.program.start.size());
    RefreshEditors();
    SetWindowTextW(editB_, L"-250");
    expect(Selected()->y == -250);
    Action(1);
    expect(paused_);
    Action(1);
    expect(!paused_);
    for (int id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21, 22, 23, 24}) {
        auto b = Box(id);
        expect(Hit(b.left + 3, b.top + 3) == id);
    }
    expect(Encode(Decode(Encode(settings_))) == Encode(settings_));
    Paint();
    return checks;
}
} // namespace MouseControl
