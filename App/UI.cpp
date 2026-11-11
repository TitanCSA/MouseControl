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
} // namespace
Window::Window(bool offline) : offline_(offline) {
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()));
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown **>(text_.GetAddressOf())));
    const float sizes[] = {12, 14, 16, 21, 28, 16};
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
    if (mono_)
        DeleteObject(mono_);
    if (fieldBrush_)
        DeleteObject(fieldBrush_);
}
D2D1_RECT_F Window::Box(int id) const {
    const float right = width_ - 408, edge = width_ - 432;
    if (id == 1)
        return D2D1::RectF(width_ - 164, 28, width_ - 32, 70);
    if (id == 2)
        return D2D1::RectF(width_ - 332, 28, width_ - 180, 70);
    if (id == 3)
        return D2D1::RectF(edge - 120, 185, edge - 24, 223);
    if (id == 10)
        return D2D1::RectF(right + 24, 310, width_ - 56, 350);
    if (id >= 20 && id < 25) {
        const float x = right + 24 + (id - 20) * 66.0f;
        return D2D1::RectF(x, 252, x + 58, 291);
    }
    return {};
}
int Window::Hit(float x, float y) const {
    if (state_.error && Inside(D2D1::RectF(32, 108, width_ - 32, 150), x, y))
        return 12;
    for (int id : {1, 2, 3, 10, 20, 21, 22, 23, 24})
        if (Inside(Box(id), x, y))
            return id;
    return 0;
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
    code_ =
        CreateWindowExW(0, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
                            ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL,
                        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(204), type.hInstance, nullptr);
    if (!code_)
        throw std::runtime_error("Could not create the code editor.");
    if (!SetWindowSubclass(code_, EditorProcedure, 1, reinterpret_cast<DWORD_PTR>(this)))
        throw std::runtime_error("Could not initialize code indentation.");
    SendMessageW(code_, EM_SETLIMITTEXT, 16384, 0);
    changing_ = true;
    SetWindowTextW(code_, WindowsLines(Wide(settings_.source.empty() ? Script(settings_.program)
                                                                     : settings_.source))
                              .c_str());
    changing_ = false;
    Layout();
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
    scale_ = std::max(.5f, std::min({GetDpiForWindow(window_) / 96.0f, client.right / 1120.0f,
                                     client.bottom / 760.0f}));
    width_ = client.right / scale_;
    height_ = client.bottom / scale_;
    if (target_)
        target_->Resize(D2D1::SizeU(client.right, client.bottom));
    const auto old = mono_;
    mono_ = CreateFontW(-static_cast<int>(16 * scale_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        FIXED_PITCH, L"Consolas");
    if (code_) {
        MoveWindow(code_, static_cast<int>(74 * scale_), static_cast<int>(276 * scale_),
                   static_cast<int>((width_ - 536) * scale_),
                   static_cast<int>((height_ - 414) * scale_), TRUE);
        SendMessageW(code_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), TRUE);
        const DWORD tabs = 16;
        SendMessageW(code_, EM_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tabs));
    }
    if (old)
        DeleteObject(old);
    Animate();
    InvalidateRect(window_, nullptr, FALSE);
}
void Window::Notify(const std::wstring &message) {
    notice_ = message;
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::Changed() {
    Validate(settings_);
    notice_.clear();
    if (!offline_) {
        engine_.Configure(settings_);
        SetTimer(window_, 3, 500, nullptr);
    }
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::Compile() {
    try {
        auto next = settings_;
        next.source = Utf8(Value(code_));
        next.program = Parse(next.source);
        Validate(next);
        settings_ = std::move(next);
        codeError_.clear();
        Changed();
    } catch (const std::exception &error) {
        codeError_ = Wide(error.what());
        InvalidateRect(window_, nullptr, FALSE);
    }
}
void Window::ApplyEdit(const TextEdit &edit) {
    if (edit.text.size() > 16384 || edit.begin > edit.end || edit.end > edit.text.size())
        throw std::invalid_argument("The edited sequence exceeds the editor limit.");
    const auto previous = Value(code_);
    std::size_t first = 0;
    while (first < previous.size() && first < edit.text.size() &&
           previous[first] == edit.text[first])
        ++first;
    auto a = previous.size(), b = edit.text.size();
    while (a > first && b > first && previous[a - 1] == edit.text[b - 1]) {
        --a;
        --b;
    }
    if (a != first || b != first) {
        SendMessageW(code_, EM_SETSEL, first, a);
        const auto replacement = edit.text.substr(first, b - first);
        SendMessageW(code_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(replacement.c_str()));
    }
    SendMessageW(code_, EM_SETSEL, edit.begin, edit.end);
    SendMessageW(code_, EM_SCROLLCARET, 0, 0);
}
LRESULT CALLBACK Window::EditorProcedure(HWND editor, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                         DWORD_PTR reference) {
    auto *self = reinterpret_cast<Window *>(reference);
    try {
        if (message == WM_GETDLGCODE)
            return DefSubclassProc(editor, message, w, l) | DLGC_WANTTAB;
        if (message == WM_CHAR && (w == L'\r' || w == L'\t' || w == L'}')) {
            DWORD begin = 0, end = 0;
            SendMessageW(editor, EM_GETSEL, reinterpret_cast<WPARAM>(&begin),
                         reinterpret_cast<LPARAM>(&end));
            const auto value = Value(editor);
            self->ApplyEdit(w == L'\r' ? Newline(value, begin, end)
                            : w == L'}'
                                ? ClosingBrace(value, begin, end)
                                : Tab(value, begin, end, (GetKeyState(VK_SHIFT) & 0x8000) != 0));
            return 0;
        }
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(editor, EditorProcedure, 1);
    } catch (const std::exception &error) {
        self->Notify(Wide(error.what()));
        return 0;
    }
    return DefSubclassProc(editor, message, w, l);
}
void Window::Action(int id) {
    const auto before = settings_;
    try {
        if (id == 1) {
            on_ = !on_;
            state_.paused = !on_;
            state_.error = 0;
            if (!offline_)
                engine_.Running(on_);
            else
                state_.title = on_ ? L"Ready - hold your trigger" : L"Remap OFF";
        } else if (id == 2) {
            if (!offline_)
                OpenSetup(window_);
            else
                Notify(L"Offline preview: setup was not launched.");
        } else if (id == 12) {
            MessageBoxW(window_, state_.detail.c_str(), L"MouseControl - driver details",
                        MB_OK | MB_ICONINFORMATION);
        } else if (id == 3) {
            auto next = settings_;
            next.source = Utf8(IndentCode(Value(code_)));
            next.program = Parse(next.source);
            Validate(next);
            const auto formatted = Wide(next.source);
            ApplyEdit({formatted, 0, 0});
            Compile();
        } else if (id == 10) {
            if (!offline_ && !state_.connected) {
                Notify(L"Switch ON with the driver installed before recording a key.");
                return;
            }
            state_.recording = !state_.recording;
            if (!offline_)
                engine_.RecordKey(state_.recording);
            Notify(state_.recording ? L"Press one keyboard key. Escape cancels."
                                    : L"Key recording cancelled.");
        } else if (id >= 20 && id < 25) {
            settings_.trigger = Trigger::Mouse(static_cast<std::uint16_t>(id - 19));
            Changed();
        }
    } catch (const std::exception &error) {
        settings_ = before;
        Notify(Wide(error.what()));
    }
    Animate();
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
void Window::SnapshotChanged() {
    state_ = engine_.Read();
    on_ = !state_.paused;
    if (state_.recordVersion != recorded_) {
        recorded_ = state_.recordVersion;
        if (state_.recorded) {
            const auto before = settings_;
            try {
                settings_.trigger = Trigger::Keyboard(*state_.recorded);
                Changed();
            } catch (const std::exception &e) {
                settings_ = before;
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
    if (animate_ &&
        (state_.active || transition || std::abs(toggleMix_ - (on_ ? 1.0f : 0.0f)) > .005f))
        SetTimer(window_, 1, 16, nullptr);
    else {
        KillTimer(window_, 1);
        hoverMix_ = hover_ ? 1.0f : 0.0f;
        toggleMix_ = on_ ? 1.0f : 0.0f;
    }
}
void Window::Draw(ID2D1RenderTarget *rt) {
    ComPtr<ID2D1SolidColorBrush> brush;
    Check(rt->CreateSolidColorBrush(D2D1::ColorF(White), &brush));
    const auto color = [&](UINT c, float alpha = 1) { brush->SetColor(D2D1::ColorF(c, alpha)); };
    const auto text = [&](const std::wstring &value, float x, float y, float w, UINT c = White,
                          unsigned font = 1, float h = 30) {
        color(c);
        rt->DrawText(value.data(), static_cast<UINT32>(value.size()), fonts_[font].Get(),
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
    const auto button = [&](int id, const std::wstring &label, bool selected = false) {
        auto box = Box(id);
        rect(box, selected ? 0x203E67 : Field, 10);
        if (id == hover_)
            rect(box, White, 10, .06f + hoverMix_ * .04f);
        if (selected)
            stroke(box, Blue, 10);
        text(label, box.left + 14, box.top + 10, box.right - box.left - 20,
             selected ? 0x9CC5FF : White);
    };
    rt->Clear(D2D1::ColorF(Background));
    rect(D2D1::RectF(0, 0, width_, 96), 0x0E1725, 0);
    rect(D2D1::RectF(32, 28, 76, 72), Blue, 14);
    color(White);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(46, 37, 62, 63), 8, 8), brush.Get(),
                             1.7f);
    rt->DrawLine(D2D1::Point2F(54, 38), D2D1::Point2F(54, 46), brush.Get(), 1.5f);
    text(L"MouseControl", 92, 25, 300, White, 4, 38);
    text(L"One trigger. Your code. Release to stop.", 93, 65, 460, Muted, 0);
    button(2, L"Driver setup");
    const auto toggle = Box(1);
    rect(toggle, Field, 12);
    text(on_ ? L"ON" : L"OFF", toggle.left + 15, toggle.top + 11, 45, on_ ? White : Muted);
    const auto track =
        D2D1::RectF(toggle.right - 66, toggle.top + 8, toggle.right - 12, toggle.bottom - 8);
    rect(track, on_ ? Blue : Line, 13);
    color(White);
    rt->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(track.left + 13 + toggleMix_ * 28, track.top + 13), 10, 10),
        brush.Get());
    rect(D2D1::RectF(32, 108, width_ - 32, 150), Panel, 12);
    color(state_.error ? Amber : Blue);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(51, 129), 4, 4), brush.Get());
    text(state_.recording ? L"Press one key - Escape cancels" : state_.title, 65, 119, width_ - 330,
         state_.error ? Amber : White);
    text(L"Ctrl + Alt + F12  |  ON / OFF", width_ - 280, 120, 240, Muted, 0);
    const float edge = width_ - 432, right = width_ - 408;
    rect(D2D1::RectF(32, 166, edge, height_ - 84), Panel, 18);
    stroke(D2D1::RectF(32, 166, edge, height_ - 84), Line, 18);
    text(L"Movement sequence", 56, 188, edge - 220, White, 3);
    button(3, L"Indent");
    text(L"Enter indents automatically. Tab / Shift+Tab adds or removes four spaces.", 56, 231,
         edge - 90, Muted, 0);
    rect(D2D1::RectF(56, 260, edge - 24, height_ - 118), Field, 12);
    const auto source = settings_.source.empty() ? Script(settings_.program) : settings_.source;
    text(WindowsLines(Wide(source)), 74, 278, edge - 130, White, 5, height_ - 416);
    text(L"move(-100) = down 100 counts     |     wait(30ms) = wait 30 ms", 56, height_ - 108,
         edge - 90, Muted, 0);
    rect(D2D1::RectF(right, 166, width_ - 32, 420), Panel, 18);
    stroke(D2D1::RectF(right, 166, width_ - 32, 420), Line, 18);
    text(L"Hold to trigger", right + 24, 190, 320, White, 3);
    text(L"Choose one mouse button or one keyboard key.", right + 24, 223, 330, Muted, 0);
    const wchar_t *labels[] = {L"Left", L"Right", L"Mid", L"X1", L"X2"};
    for (int i = 0; i < 5; ++i)
        button(20 + i, labels[i],
               settings_.trigger.kind == TriggerKind::Mouse && settings_.trigger.code == i + 1);
    button(10,
           state_.recording                                  ? L"Press a key..."
           : settings_.trigger.kind == TriggerKind::Keyboard ? L"Change keyboard key"
                                                             : L"Use a keyboard key",
           state_.recording || settings_.trigger.kind == TriggerKind::Keyboard);
    const auto trigger =
        settings_.trigger.kind == TriggerKind::Keyboard
            ? L"Selected: " + KeyName({settings_.trigger.code, settings_.trigger.extended})
            : L"Selected: " + std::wstring(labels[settings_.trigger.code - 1]);
    text(trigger, right + 24, 372, 325, White, 1);
    const float live = 436, bottom = height_ - 84;
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
        for (const auto &step : settings_.program.loop)
            if (step.operation == Operation::Move) {
                x = step.x;
                y = -step.y;
                break;
            }
        const auto length =
            std::max(1.0, std::hypot(static_cast<double>(x), static_cast<double>(y)));
        const float dx = static_cast<float>(x / length * 38),
                    dy = static_cast<float>(y / length * 38);
        color(Blue);
        rt->DrawLine(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx + dx, cy + dy), brush.Get(),
                     2.5f);
        const float progress = state_.active ? std::fmod(phase_, 1.0f) : .6f;
        rt->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx + dx * (progress * 2 - 1), cy + dy * (progress * 2 - 1)),
                          5, 5),
            brush.Get());
        text(std::to_wstring(state_.movements), right + 187, live + 78, 158, White, 4, 38);
        text(L"movement packets added", right + 187, live + 118, 165, Muted, 0);
    }
    text(L"No added loop delay", right + 24, bottom - 79, 325, White, 1);
    text(L"Only your wait() commands pace the sequence.", right + 24, bottom - 55, 325, Muted, 0);
    text(state_.target, right + 24, bottom - 29, 325, Muted, 0);
    const auto footer =
        offline_ ? L"Offline preview - no driver or input access."
                 : L"Keyboard & Mouse demo - 1 hour per boot - actions run outside this window.";
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
    case WM_LBUTTONDOWN:
        pressed_ = Hit(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
        if (pressed_)
            SetCapture(window_);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = Hit(GET_X_LPARAM(l) / scale_, GET_Y_LPARAM(l) / scale_);
        const int clicked = pressed_ && hit == pressed_ ? hit : 0;
        pressed_ = 0;
        if (GetCapture() == window_)
            ReleaseCapture();
        if (clicked)
            Action(clicked);
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressed_ = 0;
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE && LOWORD(w) == 204 && !changing_)
            SetTimer(window_, 2, 220, nullptr);
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(w), RGB(26, 36, 50));
        SetTextColor(reinterpret_cast<HDC>(w), RGB(237, 243, 252));
        return reinterpret_cast<LRESULT>(fieldBrush_);
    case WM_TIMER:
        if (w == 1) {
            phase_ = std::fmod(phase_ + .025f, 1.0f);
            toggleMix_ += ((on_ ? 1.0f : 0.0f) - toggleMix_) * .24f;
            hoverMix_ += (static_cast<float>(hover_ != 0) - hoverMix_) * .2f;
            Animate();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (w == 2) {
            KillTimer(window_, 2);
            Compile();
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
    const auto expect = [&](bool value) {
        ++checks;
        if (!value)
            throw std::runtime_error("UI check failed: " + std::to_string(checks));
    };
    expect(offline_);
    expect(settings_.trigger.kind == TriggerKind::Mouse && settings_.trigger.code == 1);
    expect(code_ != nullptr);
    for (int id = 20; id < 25; ++id) {
        Action(id);
        expect(settings_.trigger.code == id - 19);
        expect(settings_.trigger.kind == TriggerKind::Mouse);
    }
    Action(24);
    expect(settings_.trigger.code == 5);
    Action(20);
    expect(settings_.trigger.code == 1);
    Action(1);
    expect(!on_);
    Action(1);
    expect(on_);
    changing_ = true;
    SetWindowTextW(code_, L"loop {}");
    changing_ = false;
    SendMessageW(code_, EM_SETSEL, 6, 6);
    SendMessageW(code_, WM_CHAR, L'\r', 0);
    expect(Value(code_) == L"loop {\r\n    \r\n}");
    SendMessageW(code_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"move(-100)"));
    Compile();
    expect(codeError_.empty());
    expect(settings_.program.loop.size() == 1);
    const auto valid = settings_.source;
    changing_ = true;
    SetWindowTextW(code_, L"loop {invalid(1)}");
    changing_ = false;
    Compile();
    expect(!codeError_.empty());
    expect(settings_.source == valid);
    changing_ = true;
    SetWindowTextW(code_, L"// comment\r\nloop {\r\nmove(-2) // keep\r\n}");
    changing_ = false;
    Action(3);
    expect(codeError_.empty());
    expect(Value(code_).find(L"    move(-2) // keep") != std::wstring::npos);
    expect(settings_.source.find("// keep") != std::string::npos);
    settings_.trigger = Trigger::Keyboard({0x1D, 0});
    Changed();
    Action(21);
    expect(settings_.trigger.kind == TriggerKind::Mouse && settings_.trigger.code == 2);
    for (int id : {1, 2, 3, 10, 20, 21, 22, 23, 24}) {
        const auto b = Box(id);
        expect(Hit(b.left + 3, b.top + 3) == id);
    }
    const auto point =
        MAKELPARAM(static_cast<short>((Box(1).left + 5) * scale_), static_cast<short>(40 * scale_));
    Message(WM_LBUTTONUP, 0, point);
    expect(on_);
    pressed_ = 1;
    Message(WM_LBUTTONUP, 0, point);
    expect(!on_);
    Action(1);
    expect(Encode(Decode(Encode(settings_))) == Encode(settings_));
    Paint();
    return checks;
}
} // namespace MouseControl
