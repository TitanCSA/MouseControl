#include "Core.h"
#include <iostream>
#include <random>
#include <stdexcept>

using namespace MouseControl;
namespace {
unsigned checks = 0;
void Check(bool condition) {
    if (!condition)
        throw std::runtime_error("Core assertion " + std::to_string(checks + 1) + " failed.");
    ++checks;
}
template <class F> void Reject(F f) {
    bool refused = false;
    try {
        f();
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    Check(refused);
}
Scheduler Held(Settings config = {}) {
    Scheduler s(config);
    s.Enable(true);
    s.Mouse(7, 1, 0);
    s.Reconcile(1000);
    return s;
}
void Default() {
    auto s = Held();
    Check(s.Active() && s.Target() == 7);
    Check(!s.Tick(1000));
    Check(s.Deadline() == 31000);
    Check(!s.Tick(30999));
    auto move = s.Tick(31000);
    Check(move && move->device == 7 && move->x == 0 && move->y == 100);
    Check(s.Deadline() == 39000);
    Check(!s.Tick(38999));
    Check(s.Tick(39000).has_value());
    s.Mouse(7, 0, 1);
    s.Reconcile(40000);
    Check(!s.Active() && !s.Deadline());
    Check(!s.Tick(100000000));
    s.Mouse(7, 1, 0);
    s.Reconcile(40000);
    Check(!s.Tick(40000));
    Check(!s.Tick(69999));
    Check(s.Tick(70000).has_value());
}
void Chords() {
    Settings config;
    config.mouseButtons = 3;
    config.keys = {{0x1D, 0}, {0x2E, 0}};
    auto s = Held(config);
    Check(!s.Active());
    s.Mouse(9, 2, 0);
    s.Keyboard(1, {0x1D, 0}, true);
    s.Keyboard(1, {0x2E, 0}, true);
    s.Reconcile(0);
    Check(!s.Active()); // Different mice do not form a chord.
    s.Mouse(7, 2, 0);
    s.Reconcile(0);
    Check(s.Active() && s.Target() == 7);
    s.Tick(0);
    Check(s.Tick(30000)->device == 7);
    s.Mouse(9, 1, 0);
    s.Reconcile(31000);
    Check(s.Target() == 7);
    s.Keyboard(1, {0x2E, 0}, false);
    s.Reconcile(32000);
    Check(!s.Active());
    s.Keyboard(1, {0x2E, 0}, true);
    s.Reconcile(40000);
    s.Tick(40000);
    s.Keyboard(1, {0x1D, 0}, false);
    s.Reconcile(41000);
    Check(!s.Tick(70000));
    config.mouseButtons = 0;
    config.keys = {{0x1D, 0}};
    s.Configure(config);
    s.Mouse(9, 0, 0);
    s.Keyboard(1, {0x1D, 0}, true);
    s.Reconcile(80000);
    Check(s.Target() == 9);
    s.Mouse(7, 0, 0);
    s.Reconcile(80001);
    Check(s.Target() == 9);
    s.Keyboard(2, {0x1D, 0}, true);
    s.Keyboard(1, {0x1D, 0}, false);
    s.Reconcile(80002);
    Check(s.Active());
    s.Keyboard(2, {0x1D, 0}, false);
    s.Reconcile(80003);
    Check(!s.Active());
}
void Cancellation() {
    for (int variant = 0; variant < 4; ++variant) {
        auto s = Held();
        s.Tick(1000);
        if (variant == 0)
            s.Enable(false);
        if (variant == 1)
            s.RemoveMouse(7);
        if (variant == 2)
            s.Clear();
        if (variant == 3)
            s.Configure({});
        Check(!s.Active());
        Check(!s.Tick(1000000));
    }
    Settings config;
    config.program = Parse("move(1, 2) wait(0ms) loop { wait(10ms) move(-50) move(12, 34) }");
    config.loopGapMs = 1;
    auto s = Held(config);
    const auto initial = s.Tick(1000);
    Check(initial && initial->x == 1 && initial->y == -2);
    Check(!s.Tick(1000));
    Check(!s.Tick(10999));
    const auto next = s.Tick(11000);
    Check(next && next->y == 50);
    const auto third = s.Tick(11000);
    Check(third && third->x == 12 && third->y == -34);
    Check(s.Deadline() == 12000);
    Check(!s.Tick(1000000));
    Check(s.Deadline() == 1010000); // Late wake starts the next wait now; no catch-up burst.
    config.program = Parse("wait(0) move(-1)");
    s.Configure(config);
    s.Reconcile(2000000);
    Check(s.Tick(2000000)->y == 1);
    Check(!s.Tick(2000000));
    s.Reconcile(3000000);
    Check(!s.Tick(3000000));
    config.program = Parse("loop { wait(0ms) }");
    s.Configure(config);
    s.Reconcile(0);
    Check(!s.Tick(0));
    Check(s.Deadline() == 1000);
}
void Parsing() {
    Settings config;
    const auto text = Script(config.program);
    Check(text == "wait(30ms)\n\nloop {\n    move(-100)\n}\n");
    Check(Script(Parse(text)) == text);
    config.program = Parse("// comment\nwait(30ms); loop { move(+2, -4); wait(8) move(-1) };");
    Check(config.program.loop.size() == 3);
    Check(config.program.loop[0].x == 2);
    config.keys = {{0x38, 2}, {0x42, 0}};
    config.mouseButtons = 31;
    Check(Encode(Decode(Encode(config))) == Encode(config));
    for (const char *invalid :
         {"", "loop {}", "loop { move(1) ", "wait(-1)", "move(1000001)", "move(-2147483648)",
          "wait(600001ms)", "move(1.2)", "move(1,,2)", "loop { move(1) } move(2)",
          "loop { loop { move(1) } }", "print(1)", "wait(1msx)", "move(1) garbage"})
        Reject([&] { Parse(invalid); });
    Reject([] { Parse(std::string(16385, ' ')); });
    Reject([] { Decode(std::string(20001, 'x')); });
    Reject([] { Decode("MouseControl 1\nbuttons 1\ngap 0\nsequence\nmove(1)"); });
    Reject([] { Decode("MouseControl 1\nbuttons 1\nbuttons 2\ngap 8\nsequence\nmove(1)"); });
    Reject([] {
        Settings s;
        s.mouseButtons = 0;
        Validate(s);
    });
    Reject([] {
        Settings s;
        s.loopGapMs = 0;
        Validate(s);
    });
    Reject([] {
        Settings s;
        s.keys = {{0x1D, 0}, {0x1D, 0}};
        Validate(s);
    });
    std::string steps;
    for (unsigned i = 0; i < 65; ++i)
        steps += "move(1)\n";
    Reject([&] { Parse(steps); });
}
void Stress() {
    Settings fast;
    fast.program.start.clear();
    fast.loopGapMs = 1;
    std::mt19937 random(1234);
    auto s = Held(fast);
    s.Tick(1000);
    std::int64_t time = 31000;
    for (unsigned i = 0; i < 100000; ++i) {
        const bool held = (random() % 4) != 0;
        s.Mouse(7, held ? 1 : 0, held ? 0 : 1);
        s.Reconcile(time);
        const auto value = s.Tick(time);
        if (value)
            Check(held && value->device == 7 && value->y == 100);
        else
            Check(!value);
        time += 1000;
    }
    for (unsigned i = 0; i < 10000; ++i) {
        std::string bad;
        for (unsigned j = 0, n = random() % 90; j < n; ++j)
            bad += static_cast<char>(random() % 128);
        try {
            Parse(bad);
        } catch (const std::invalid_argument &) {
        }
        ++checks;
    }
}
} // namespace
int main() {
    try {
        Default();
        Chords();
        Cancellation();
        Parsing();
        Stress();
        std::cout << "PASS " << checks
                  << " parser, scheduler, multi-mouse chord, keyboard chord, cancellation and "
                     "stress assertions. No driver or desktop input accessed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
