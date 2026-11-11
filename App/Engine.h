#pragma once
#include "Core.h"
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace MouseControl {
inline constexpr UINT SnapshotMessage = WM_APP + 31;
struct Source {
    std::uint32_t id = 0;
    std::wstring name;
};
struct Physical {
    std::uint32_t device = 0;
    bool mouse = false, down = false;
    std::uint8_t buttonsDown = 0, buttonsUp = 0;
    Key key;
};
class Transport {
  public:
    virtual ~Transport() = default;
    virtual std::vector<Source> Open() = 0;
    virtual std::uint64_t Read(DWORD timeout, HANDLE wake,
                               const std::function<void(const Physical &)> &receive) = 0;
    virtual bool Send(const Movement &movement) = 0; // False: capacity busy, nothing accepted.
    virtual void Close() noexcept = 0;
};
std::shared_ptr<Transport> DriverTransport();
std::int64_t Clock() noexcept;
struct Snapshot {
    bool connected = false, paused = false, active = false, suspended = true, recording = false;
    DWORD error = 0;
    std::wstring title = L"Connecting to the driver", detail,
                 target = L"Waiting for a physical mouse";
    std::uint64_t physical = 0, movements = 0, recordVersion = 0;
    std::optional<Key> recorded;
};
class Engine {
    std::shared_ptr<Transport> transport_;
    std::shared_ptr<const Settings> settings_;
    HWND owner_ = nullptr;
    HANDLE wake_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stopping_{false}, desired_{true}, suspended_{true}, recording_{false},
        notified_{false};
    mutable std::mutex mutex_;
    Snapshot snapshot_;
    void Run();
    void Publish(const Snapshot &snapshot);

  public:
    explicit Engine(std::shared_ptr<Transport> transport = DriverTransport());
    ~Engine();
    Engine(const Engine &) = delete;
    void Start(HWND owner, const Settings &settings);
    void Stop() noexcept;
    void Configure(const Settings &settings);
    void Running(bool enabled);
    void Suspended(bool suspended);
    void RecordKey(bool enabled);
    Snapshot Read();
};
} // namespace MouseControl
