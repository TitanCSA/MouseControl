#include "Engine.h"
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <system_error>

using namespace MouseControl;
namespace {
unsigned checks = 0;
void Check(bool ok) {
    ++checks;
    if (!ok)
        throw std::runtime_error("Runtime assertion " + std::to_string(checks) + " failed.");
}
template <class F> void Until(F predicate) {
    const auto stop = GetTickCount64() + 3000;
    while (!predicate()) {
        if (GetTickCount64() >= stop)
            throw std::runtime_error("Timed out waiting for the isolated engine.");
        Sleep(1);
    }
    ++checks;
}
class Fake final : public Transport {
    HANDLE data_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex lock_;
    std::vector<Physical> incoming_;

  public:
    std::atomic<unsigned> opens{0}, closes{0}, forwarded{0}, sent{0}, afterRelease{0};
    std::atomic<bool> released{false}, fail{false}, busy{false};
    std::atomic<unsigned> busyAttempts{0};
    std::atomic<std::uint32_t> lastTarget{0};
    ~Fake() { CloseHandle(data_); }
    std::vector<Source> Open() override {
        ++opens;
        return {{7, L"Mouse A"}, {9, L"Mouse B"}};
    }
    void Push(Physical event) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            incoming_.push_back(event);
        }
        SetEvent(data_);
    }
    std::uint64_t Read(DWORD timeout, HANDLE wake,
                       const std::function<void(const Physical &)> &receive) override {
        HANDLE waits[] = {data_, wake};
        const auto result = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (result != WAIT_OBJECT_0)
            return 0;
        std::vector<Physical> events;
        {
            std::lock_guard<std::mutex> guard(lock_);
            events.swap(incoming_);
        }
        for (auto event : events) {
            receive(event);
            if (event.mouse && event.buttonsUp)
                released = true;
            ++forwarded;
        }
        return events.size();
    }
    bool Send(const Movement &move) override {
        if (busy) {
            ++busyAttempts;
            return false;
        }
        if (fail)
            throw std::system_error(ERROR_WRITE_FAULT, std::system_category(),
                                    "Isolated send failure");
        if (released)
            ++afterRelease;
        lastTarget = move.device;
        ++sent;
        return true;
    }
    void Close() noexcept override { ++closes; }
};
Physical Mouse(unsigned id, unsigned down, unsigned up = 0) {
    Physical p;
    p.mouse = true;
    p.device = id;
    p.buttonsDown = static_cast<std::uint8_t>(down);
    p.buttonsUp = static_cast<std::uint8_t>(up);
    return p;
}
void Run() {
    auto transport = std::make_shared<Fake>();
    Engine e(transport);
    e.Start(nullptr, {});
    e.Suspended(false);
    Until([&] { return e.Read().connected; });
    transport->Push(Mouse(7, 1));
    Until([&] { return transport->sent >= 3; });
    Check(transport->forwarded == 1);
    Check(transport->lastTarget == 7);
    transport->Push(Mouse(7, 0, 1));
    Until([&] { return transport->forwarded == 2; });
    Sleep(40);
    Check(transport->afterRelease == 0);
    Check(!e.Read().active);
    transport->released = false;
    transport->Push(Mouse(9, 1));
    Until([&] { return transport->lastTarget == 9; });
    e.Running(false);
    Until([&] { return e.Read().paused; });
    const auto total = transport->sent.load();
    Sleep(40);
    Check(transport->sent == total);
    Check(transport->closes >= 1);
    e.Running(true);
    Until([&] { return transport->opens >= 2; });
    e.Suspended(true);
    transport->Push(Mouse(7, 1));
    Sleep(45);
    Check(transport->sent == total);
    e.Suspended(false);
    Until([&] { return transport->sent > total; });
    transport->fail = true;
    Until([&] { return e.Read().error == ERROR_WRITE_FAULT; });
    Check(!e.Read().connected);
    Check(e.Read().detail.find(L"Windows error") != std::wstring::npos);
    const auto before = GetTickCount64();
    e.Stop();
    Check(GetTickCount64() - before < 1000);
    Check(transport->closes >= 2);
    auto second = std::make_shared<Fake>();
    Engine recording(second);
    recording.Start(nullptr, {});
    Until([&] { return recording.Read().connected; });
    recording.RecordKey(true);
    Physical key;
    key.device = 1;
    key.down = true;
    key.key = {0x1D, 0};
    second->Push(key);
    Until([&] { return recording.Read().recordVersion == 1; });
    Check(recording.Read().recorded == key.key);
    Check(second->sent == 0);
    recording.Stop();
    auto congested = std::make_shared<Fake>();
    congested->busy = true;
    Engine pressure(congested);
    pressure.Start(nullptr, {});
    pressure.Suspended(false);
    Until([&] { return pressure.Read().connected; });
    congested->Push(Mouse(7, 1));
    Until([&] { return congested->busyAttempts > 10; });
    Check(congested->sent == 0);
    congested->Push(Mouse(7, 0, 1));
    Until([&] { return congested->forwarded == 2; });
    congested->busy = false;
    Sleep(30);
    Check(congested->sent == 0 && congested->afterRelease == 0);
    Check(!pressure.Read().error);
    pressure.Stop();
}
} // namespace
int main() {
    try {
        Run();
        std::cout << "PASS " << checks
                  << " worker tests: physical forwarding, same-device movement, release "
                     "cancellation, pause, focus exclusion, resume, failure cleanup and key "
                     "recording. Isolated transport; no host driver, keyboard or mouse capture.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
