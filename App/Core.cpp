#include "Core.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace MouseControl {
namespace {
class Parser {
    const std::string &text_;
    std::size_t at_ = 0;
    [[noreturn]] void Error(const char *message) const {
        const auto line =
            1 + std::count(text_.begin(), text_.begin() + static_cast<std::ptrdiff_t>(at_), '\n');
        throw std::invalid_argument("Line " + std::to_string(line) + ": " + message);
    }
    void Space() {
        for (;;) {
            while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])))
                ++at_;
            if (at_ + 1 < text_.size() && text_[at_] == '/' && text_[at_ + 1] == '/')
                while (at_ < text_.size() && text_[at_] != '\n')
                    ++at_;
            else
                break;
        }
    }
    bool Take(char c) {
        Space();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }
    void Expect(char c) {
        if (!Take(c))
            Error("Unexpected character; check parentheses and braces.");
    }
    std::string Word() {
        Space();
        const auto begin = at_;
        while (at_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[at_])))
            ++at_;
        if (begin == at_)
            Error("Expected move(), wait(), or loop { }.");
        return text_.substr(begin, at_ - begin);
    }
    int Number() {
        Space();
        const auto begin = at_;
        if (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+'))
            ++at_;
        const auto digits = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
            ++at_;
        if (digits == at_)
            Error("Expected a whole number.");
        int value = 0;
        const char *first = text_.data() + begin;
        if (*first == '+')
            ++first;
        const auto result = std::from_chars(first, text_.data() + at_, value);
        if (result.ec != std::errc{} || value < -1000000 || value > 1000000)
            Error("Movement must be between -1000000 and 1000000 counts.");
        return value;
    }
    Step Command(const std::string &word) {
        Expect('(');
        const int a = Number();
        if (word == "move") {
            int x = 0, y = a;
            if (Take(',')) {
                x = a;
                y = Number();
            }
            Expect(')');
            Take(';');
            return Step::Move(x, y);
        }
        if (word == "wait") {
            Space();
            if (text_.compare(at_, 2, "ms") == 0)
                at_ += 2;
            Expect(')');
            Take(';');
            if (a < 0 || a > 600000)
                Error("Wait must be between 0 and 600000 ms.");
            return Step::Wait(static_cast<unsigned>(a));
        }
        Error("Unknown command. Use move() or wait().");
    }

  public:
    explicit Parser(const std::string &text) : text_(text) {
        if (text.size() > 16384)
            throw std::invalid_argument("The sequence is too long (16 KB maximum).");
    }
    Program Run() {
        Program p;
        p.start.clear();
        p.loop.clear();
        bool loop = false;
        for (;;) {
            Space();
            if (at_ == text_.size())
                break;
            if (loop)
                Error("Put the loop at the end of the sequence.");
            const auto word = Word();
            if (word == "loop") {
                loop = true;
                Expect('{');
                while (!Take('}')) {
                    if (p.loop.size() >= 64)
                        Error("A loop can contain at most 64 steps.");
                    p.loop.push_back(Command(Word()));
                }
                Take(';');
            } else {
                if (p.start.size() >= 64)
                    Error("The start section can contain at most 64 steps.");
                p.start.push_back(Command(word));
            }
        }
        if (p.start.empty() && p.loop.empty())
            Error("Add at least one movement or wait.");
        if (loop && p.loop.empty())
            Error("The loop needs at least one step.");
        return p;
    }
};
std::string Command(const Step &s) {
    if (s.operation == Operation::Wait)
        return "wait(" + std::to_string(s.milliseconds) + "ms)";
    return "move(" + (s.x ? std::to_string(s.x) + ", " : std::string()) + std::to_string(s.y) + ")";
}
} // namespace
Program Parse(const std::string &text) {
    return Parser(text).Run();
}
std::string Script(const Program &p) {
    std::string out;
    for (const auto &s : p.start)
        out += Command(s) + "\n";
    if (!p.loop.empty()) {
        if (!out.empty())
            out += '\n';
        out += "loop {\n";
        for (const auto &s : p.loop)
            out += "    " + Command(s) + "\n";
        out += "}\n";
    }
    return out;
}
void Validate(const Settings &s) {
    if (s.mouseButtons > 31 || s.keys.size() > 8 || (!s.mouseButtons && s.keys.empty()))
        throw std::invalid_argument("Choose at least one trigger; at most eight keyboard keys.");
    if (s.loopGapMs < 1 || s.loopGapMs > 1000)
        throw std::invalid_argument("Loop gap must be 1 to 1000 ms.");
    if (s.program.start.size() > 64 || s.program.loop.size() > 64 ||
        (s.program.start.empty() && s.program.loop.empty()))
        throw std::invalid_argument("Use 1 to 128 sequence steps, at most 64 per section.");
    std::set<Key> seen;
    for (const auto &k : s.keys)
        if (!k.code || k.code > 255 || k.extended > 6 || (k.extended & 1) || !seen.insert(k).second)
            throw std::invalid_argument("Invalid or duplicate keyboard trigger.");
    for (const auto *section : {&s.program.start, &s.program.loop})
        for (const auto &step : *section) {
            if (step.operation == Operation::Wait) {
                if (step.milliseconds > 600000)
                    throw std::invalid_argument("Wait exceeds 600000 ms.");
            } else if (step.operation != Operation::Move || step.x < -1000000 || step.x > 1000000 ||
                       step.y < -1000000 || step.y > 1000000)
                throw std::invalid_argument(
                    "Invalid movement; maximum magnitude is 1000000 counts.");
        }
}
std::string Encode(const Settings &s) {
    Validate(s);
    std::string out = "MouseControl 1\nbuttons " + std::to_string(s.mouseButtons) + "\ngap " +
                      std::to_string(s.loopGapMs) + "\n";
    for (auto k : s.keys)
        out += "key " + std::to_string(k.code) + " " + std::to_string(k.extended) + "\n";
    return out + "sequence\n" + Script(s.program);
}
Settings Decode(const std::string &text) {
    if (text.size() > 20000)
        throw std::invalid_argument("Settings file exceeds 20 KB.");
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    if (line != "MouseControl 1")
        throw std::invalid_argument("Unsupported MouseControl settings.");
    Settings s;
    s.keys.clear();
    bool buttons = false, gap = false, body = false;
    while (std::getline(in, line)) {
        if (line == "sequence") {
            body = true;
            break;
        }
        std::istringstream row(line);
        std::string name, extra;
        unsigned a = 0, b = 0;
        row >> name >> a;
        if (!row)
            throw std::invalid_argument("Invalid settings header.");
        if (name == "buttons" && !buttons) {
            if (a > 31)
                throw std::invalid_argument("Invalid button mask.");
            s.mouseButtons = static_cast<std::uint8_t>(a);
            buttons = true;
        } else if (name == "gap" && !gap) {
            s.loopGapMs = a;
            gap = true;
        } else if (name == "key") {
            if (!(row >> b) || a > 255 || b > 6 || s.keys.size() >= 8)
                throw std::invalid_argument("Invalid key setting.");
            s.keys.push_back({static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(b)});
        } else
            throw std::invalid_argument("Unknown or duplicate settings header.");
        if (row >> extra)
            throw std::invalid_argument("Unexpected settings header data.");
    }
    if (!buttons || !gap || !body)
        throw std::invalid_argument("Incomplete settings file.");
    std::string script((std::istreambuf_iterator<char>(in)), {});
    s.program = Parse(script);
    Validate(s);
    return s;
}
Scheduler::Scheduler(const Settings &s) {
    Configure(s);
}
void Scheduler::Cancel() noexcept {
    active_ = inLoop_ = finished_ = false;
    target_ = 0;
    index_ = 0;
    due_ = 0;
}
void Scheduler::Configure(const Settings &s) {
    Validate(s);
    settings_ = s;
    Cancel();
}
void Scheduler::Enable(bool on) noexcept {
    enabled_ = on;
    if (!on)
        Cancel();
}
void Scheduler::Mouse(std::uint32_t device, std::uint8_t down, std::uint8_t up) {
    if (!device)
        return;
    if (mice_.size() >= 256 && !mice_.count(device))
        return;
    auto &held = mice_[device];
    held = static_cast<std::uint8_t>((held | down) & ~up & 31);
    lastMouse_ = device;
}
void Scheduler::Keyboard(std::uint32_t device, Key key, bool down) {
    if (!device || !key.code)
        return;
    if (keyboards_.size() >= 256 && !keyboards_.count(device))
        return;
    auto &keys = keyboards_[device];
    if (down) {
        if (keys.size() < 256)
            keys.insert(key);
    } else
        keys.erase(key);
}
void Scheduler::RemoveMouse(std::uint32_t device) {
    mice_.erase(device);
    if (target_ == device)
        Cancel();
    if (lastMouse_ == device)
        lastMouse_ = mice_.empty() ? 0 : mice_.begin()->first;
}
void Scheduler::Clear() noexcept {
    Cancel();
    mice_.clear();
    keyboards_.clear();
    lastMouse_ = 0;
}
bool Scheduler::KeysHeld() const {
    for (auto key : settings_.keys) {
        bool held = false;
        for (const auto &keyboard : keyboards_)
            if (keyboard.second.count(key)) {
                held = true;
                break;
            }
        if (!held)
            return false;
    }
    return true;
}
bool Scheduler::Held(std::uint32_t id) const {
    const auto it = mice_.find(id);
    return it != mice_.end() && (it->second & settings_.mouseButtons) == settings_.mouseButtons &&
           KeysHeld();
}
void Scheduler::Reconcile(std::int64_t now) {
    if (!enabled_) {
        Cancel();
        return;
    }
    if (active_) {
        if (!Held(target_))
            Cancel();
        else
            return;
    }
    std::uint32_t target = 0;
    if (settings_.mouseButtons) {
        for (auto item : mice_)
            if (Held(item.first)) {
                target = item.first;
                break;
            }
    } else if (Held(lastMouse_))
        target = lastMouse_;
    if (!target)
        return;
    active_ = true;
    target_ = target;
    index_ = 0;
    inLoop_ = false;
    finished_ = false;
    due_ = now;
}
std::optional<Movement> Scheduler::Tick(std::int64_t now) {
    if (!active_ || finished_ || !enabled_ || !Held(target_) || now < due_) {
        if (active_ && !Held(target_))
            Cancel();
        return {};
    }
    for (unsigned transitions = 0; transitions < 130; ++transitions) {
        const auto &steps = inLoop_ ? settings_.program.loop : settings_.program.start;
        if (index_ >= steps.size()) {
            if (!inLoop_) {
                if (settings_.program.loop.empty()) {
                    finished_ = true;
                    return {};
                }
                inLoop_ = true;
                index_ = 0;
                continue;
            }
            index_ = 0;
            due_ = now + static_cast<std::int64_t>(settings_.loopGapMs) * 1000;
            return {};
        }
        const auto step = steps[index_++];
        if (step.operation == Operation::Wait) {
            due_ = now + static_cast<std::int64_t>(step.milliseconds) * 1000;
            if (step.milliseconds)
                return {};
            continue;
        }
        // Script coordinates: negative Y means down, matching the requested move(-100).
        if (inLoop_ && index_ == steps.size()) {
            index_ = 0;
            due_ = now + static_cast<std::int64_t>(settings_.loopGapMs) * 1000;
        }
        return Movement{target_, step.x, -step.y};
    }
    throw std::logic_error("Sequence transition bound exceeded.");
}
std::optional<std::int64_t> Scheduler::Deadline() const noexcept {
    if (!active_ || finished_)
        return {};
    return due_;
}
} // namespace MouseControl
