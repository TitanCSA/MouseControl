#include "Engine.h"
#include "Interceptor.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <system_error>

namespace MouseControl {
namespace {
void Require(const Interceptor::Send_Result &sent, std::size_t expected) {
    if (!sent || sent.accepted != expected)
        throw std::system_error(sent.error ? sent.error : ERROR_WRITE_FAULT, std::system_category(),
                                "Forward mouse/keyboard input");
}
std::wstring Wide(const char *text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (!n)
        return L"Unknown driver failure";
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
    out.pop_back();
    return out;
}
class NativeTransport final : public Transport {
  public:
    std::vector<Source> Open() override {
        using namespace Interceptor;
        if (Get_Status().version != IC_VERSION)
            throw std::system_error(ERROR_REVISION_MISMATCH, std::system_category(),
                                    "Incompatible Keyboard & Mouse driver");
        const auto init = Init_AllDevices();
        if (!init.ready)
            throw std::system_error(init.error             ? init.error
                                    : init.reboot_required ? ERROR_SUCCESS_REBOOT_REQUIRED
                                                           : ERROR_NOT_READY,
                                    std::system_category(), "Initialize Keyboard & Mouse devices");
        std::vector<Source> mice;
        for (const auto &d : Get_Devices())
            if (d.type == static_cast<unsigned>(Device_Type::Mouse)) {
                const auto length = static_cast<std::size_t>(
                    std::find(std::begin(d.name), std::end(d.name), 0) - std::begin(d.name));
                std::wstring name(reinterpret_cast<const wchar_t *>(d.name), length);
                if (name.empty())
                    name = L"Mouse " + std::to_wstring(d.id);
                mice.push_back({d.id, std::move(name)});
            }
        if (mice.empty())
            throw std::system_error(ERROR_DEVICE_NOT_CONNECTED, std::system_category(),
                                    "No associated physical mouse is available");
        return mice;
    }
    std::uint64_t Read(DWORD timeout, HANDLE wake,
                       const std::function<void(const Physical &)> &receive) override {
        using namespace Interceptor;
        auto batch = Get_Input_With_Timeout(timeout, wake);
        std::exception_ptr failure;
        for (const auto &input : batch) {
            Physical p;
            p.device = input.device;
            if (const auto *mouse = input.Get<Mouse_Input>()) {
                p.mouse = true;
                for (unsigned i = 0; i < 5; ++i) {
                    if (mouse->stroke.button_flags & (1u << (i * 2)))
                        p.buttonsDown |= static_cast<std::uint8_t>(1u << i);
                    if (mouse->stroke.button_flags & (2u << (i * 2)))
                        p.buttonsUp |= static_cast<std::uint8_t>(1u << i);
                }
            } else if (const auto *keyboard = input.Get<Keyboard_Input>()) {
                p.key = {keyboard->stroke.make_code,
                         static_cast<std::uint16_t>(keyboard->stroke.flags & (KeyE0 | KeyE1))};
                p.down = (keyboard->stroke.flags & KeyUp) == 0;
            } else
                continue;
            try {
                if (!failure)
                    receive(p);
            } catch (...) {
                failure = std::current_exception();
            }
        }
        // Always forward the physical batch, even if a user-mode state update failed.
        const auto size = batch.size();
        if (size)
            Require(Send_Input(batch), size);
        if (failure)
            std::rethrow_exception(failure);
        return size;
    }
    void Send(const Movement &movement) override {
        Interceptor::Mouse_Input packet{};
        packet.device = movement.device;
        packet.stroke.x = movement.x;
        packet.stroke.y = movement.y;
        Require(Interceptor::Send_Input(packet), 1);
    }
    void Close() noexcept override {
        try {
            Interceptor::Destroy_Context();
        } catch (...) {
        }
    }
};
std::wstring DriverHint(DWORD code) {
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_SERVICE_DOES_NOT_EXIST)
        return L"Install Interceptor Keyboard & Mouse using Driver setup, restart if requested, "
               L"then press Retry.";
    if (code == ERROR_EVALUATION_EXPIRATION)
        return L"The shared one-hour driver demo has expired. Use Windows Restart for a new "
               L"allowance, then reopen MouseControl.";
    if (code == ERROR_SUCCESS_REBOOT_REQUIRED || code == ERROR_SUCCESS_RESTART_REQUIRED)
        return L"Windows needs a restart to finish driver associations. Restart, then reopen "
               L"MouseControl.";
    if (code == ERROR_ACCESS_DENIED)
        return L"Administrator rights are required. Reopen MouseControl and accept the Windows "
               L"administrator prompt.";
    return L"Close other capture applications. Check Driver setup > Diagnose, reconnect the device "
           L"if necessary, then press Retry.";
}
} // namespace
std::shared_ptr<Transport> DriverTransport() {
    return std::make_shared<NativeTransport>();
}
std::int64_t Clock() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
Engine::Engine(std::shared_ptr<Transport> t) : transport_(std::move(t)) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_)
        throw std::system_error(GetLastError(), std::system_category(),
                                "Create capture wake event");
}
Engine::~Engine() {
    Stop();
    CloseHandle(wake_);
}
void Engine::Start(HWND owner, const Settings &settings) {
    if (worker_.joinable())
        throw std::logic_error("MouseControl is already running.");
    Configure(settings);
    owner_ = owner;
    worker_ = std::thread([this] { Run(); });
}
void Engine::Stop() noexcept {
    stopping_ = true;
    desired_ = false;
    SetEvent(wake_);
    if (worker_.joinable())
        worker_.join();
}
void Engine::Configure(const Settings &s) {
    Validate(s);
    std::atomic_store(&settings_, std::make_shared<const Settings>(s));
    SetEvent(wake_);
}
void Engine::Running(bool enabled) {
    desired_ = enabled;
    SetEvent(wake_);
}
void Engine::Suspended(bool suspended) {
    suspended_ = suspended;
    SetEvent(wake_);
}
void Engine::RecordKey(bool enabled) {
    recording_ = enabled;
    SetEvent(wake_);
}
void Engine::Publish(const Snapshot &value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = value;
    }
    if (owner_ && !notified_.exchange(true) && !PostMessageW(owner_, SnapshotMessage, 0, 0))
        notified_ = false;
}
Snapshot Engine::Read() {
    notified_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}
void Engine::Run() {
    Snapshot state;
    std::int64_t published = 0;
    while (!stopping_) {
        if (!desired_) {
            state.paused = true;
            state.active = state.connected = false;
            state.recording = recording_;
            if (!state.error) {
                state.title = L"Paused";
                state.detail = L"Capture released. Physical input passes through normally.";
            }
            Publish(state);
            WaitForSingleObject(wake_, INFINITE);
            continue;
        }
        try {
            auto configured = std::atomic_load(&settings_);
            Scheduler scheduler(*configured);
            const auto sources = transport_->Open();
            for (const auto &source : sources)
                scheduler.Mouse(source.id, 0, 0);
            state.error = 0;
            state.paused = false;
            state.connected = true;
            state.title = L"Ready";
            state.detail.clear();
            Publish(state);
            while (desired_ && !stopping_) {
                const auto next = std::atomic_load(&settings_);
                if (next != configured) {
                    scheduler.Configure(*next);
                    configured = next;
                }
                scheduler.Enable(!suspended_ && !recording_);
                const auto due = scheduler.Deadline();
                const auto now = Clock();
                const auto timeout =
                    due ? static_cast<DWORD>(*due <= now ? 0
                                                         : std::min<std::int64_t>(
                                                               600000, (*due - now + 999) / 1000))
                        : INFINITE;
                const bool before = state.active;
                const auto beforeRecord = state.recordVersion;
                state.physical += transport_->Read(timeout, wake_, [&](const Physical &input) {
                    if (input.mouse)
                        scheduler.Mouse(input.device, input.buttonsDown, input.buttonsUp);
                    else {
                        scheduler.Keyboard(input.device, input.key, input.down);
                        if (recording_ && input.down) {
                            state.recorded = input.key.code == 1 && !input.key.extended
                                                 ? std::optional<Key>{}
                                                 : input.key;
                            ++state.recordVersion;
                            recording_ = false;
                        }
                    }
                });
                // Re-read cancellation/focus flags after the wait and before every synthetic
                // packet. No look-ahead queue of future movements is ever submitted to the driver.
                const auto newest = std::atomic_load(&settings_);
                if (newest != configured) {
                    scheduler.Configure(*newest);
                    configured = newest;
                }
                scheduler.Enable(desired_ && !stopping_ && !suspended_ && !recording_);
                scheduler.Reconcile(Clock());
                if (const auto movement = scheduler.Tick(Clock())) {
                    transport_->Send(*movement);
                    ++state.movements;
                }
                state.active = scheduler.Active();
                state.suspended = suspended_;
                state.recording = recording_;
                if (scheduler.Target()) {
                    const auto source =
                        std::find_if(sources.begin(), sources.end(),
                                     [&](const auto &s) { return s.id == scheduler.Target(); });
                    state.target = source == sources.end()
                                       ? L"Mouse " + std::to_wstring(scheduler.Target())
                                       : source->name;
                }
                state.title = state.active      ? L"Sequence running"
                              : state.suspended ? L"Ready · use outside this window"
                                                : L"Ready · hold your trigger";
                const auto stamp = Clock();
                if (before != state.active || beforeRecord != state.recordVersion ||
                    stamp - published >= 50000) {
                    Publish(state);
                    published = stamp;
                }
            }
        } catch (const std::system_error &error) {
            state.error = static_cast<DWORD>(error.code().value());
            state.title = L"Driver needs attention";
            state.detail = DriverHint(state.error) + L"\n\n" + Wide(error.what()) +
                           L" (Windows error " + std::to_wstring(state.error) + L").";
            desired_ = false;
        } catch (const std::exception &error) {
            state.error = ERROR_GEN_FAILURE;
            state.title = L"Capture stopped safely";
            state.detail = Wide(error.what());
            desired_ = false;
        }
        transport_->Close();
        state.connected = state.active = false;
    }
    transport_->Close();
    state.active = state.connected = false;
    state.title = L"Stopped";
    Publish(state);
}
} // namespace MouseControl
