#pragma once
#include "Engine.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <string>

namespace MouseControl {
class Window {
    using Factory = Microsoft::WRL::ComPtr<ID2D1Factory>;
    HWND window_ = nullptr, editA_ = nullptr, editB_ = nullptr, code_ = nullptr, gap_ = nullptr;
    HFONT font_ = nullptr, mono_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    Factory factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> text_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    std::array<Microsoft::WRL::ComPtr<IDWriteTextFormat>, 6> fonts_;
    Settings settings_;
    Engine engine_;
    Snapshot state_;
    bool offline_ = false, codeView_ = false, changing_ = false, paused_ = false, tracking_ = false,
         animate_ = true;
    float width_ = 1320, height_ = 860, scale_ = 1, hoverMix_ = 0, phase_ = 0;
    int selected_ = 1, scroll_ = 0, hover_ = 0, pressed_ = 0;
    std::uint64_t recorded_ = 0;
    std::wstring notice_, codeError_;
    void Changed(bool refreshCode = true);
    void RefreshEditors();
    void Layout();
    void SnapshotChanged();
    void Animate();
    int Hit(float x, float y) const;
    D2D1_RECT_F Box(int id) const;
    Step *Selected();
    void Edit(int id);
    void Action(int id);
    void Paint();
    void Draw(ID2D1RenderTarget *target);
    void Notify(const std::wstring &message);
    LRESULT Message(UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l);

  public:
    explicit Window(bool offline);
    ~Window();
    void Create(bool visible = true);
    void Render(const std::wstring &path);
    unsigned SelfTest();
};
} // namespace MouseControl
