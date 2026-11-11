#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MouseControl {
enum class Operation { Move, Wait };
struct Step {
    Operation operation = Operation::Move;
    int x = 0, y = -100;
    unsigned milliseconds = 0;
    static Step Move(int x, int y) { return {Operation::Move, x, y, 0}; }
    static Step Wait(unsigned ms) { return {Operation::Wait, 0, 0, ms}; }
};
struct Program {
    std::vector<Step> start{Step::Wait(30)};
    std::vector<Step> loop{Step::Move(0, -100)};
};
struct Key {
    std::uint16_t code = 0, extended = 0;
    bool operator<(const Key &other) const noexcept {
        return code < other.code || (code == other.code && extended < other.extended);
    }
    bool operator==(const Key &other) const noexcept {
        return code == other.code && extended == other.extended;
    }
};
struct Settings {
    Program program;
    std::uint8_t mouseButtons = 1; // Left, right, middle, X1, X2. All must be held on one mouse.
    std::vector<Key> keys;         // AND, not OR; state comes from physical driver packets.
    unsigned loopGapMs = 8;        // Bounded scheduler cadence; never busy-spin an empty loop.
};
Program Parse(const std::string &text);
std::string Script(const Program &program);
void Validate(const Settings &settings);
std::string Encode(const Settings &settings);
Settings Decode(const std::string &text);
struct Movement {
    std::uint32_t device = 0;
    int x = 0, y = 0;
}; // Raw Windows relative counts; +Y is down.

// Deterministic state machine. It never sleeps or sends input itself.
// The transport processes/forwards physical packets before calling Tick().
class Scheduler {
    Settings settings_;
    std::map<std::uint32_t, std::uint8_t> mice_;
    std::map<std::uint32_t, std::set<Key>> keyboards_;
    std::uint32_t lastMouse_ = 0, target_ = 0;
    bool enabled_ = false, active_ = false, inLoop_ = false, finished_ = false;
    std::size_t index_ = 0;
    std::int64_t due_ = 0;
    bool KeysHeld() const;
    bool Held(std::uint32_t mouse) const;
    void Cancel() noexcept;

  public:
    explicit Scheduler(const Settings &settings = {});
    void Configure(const Settings &settings);
    void Enable(bool enabled) noexcept;
    void Mouse(std::uint32_t device, std::uint8_t down, std::uint8_t up);
    void Keyboard(std::uint32_t device, Key key, bool down);
    void RemoveMouse(std::uint32_t device);
    void Clear() noexcept;
    void Reconcile(std::int64_t now);
    std::optional<Movement> Tick(std::int64_t now);
    std::optional<std::int64_t> Deadline() const noexcept;
    bool Active() const noexcept { return active_ && !finished_; }
    std::uint32_t Target() const noexcept { return target_; }
};
} // namespace MouseControl
