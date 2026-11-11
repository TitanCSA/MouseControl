#include "Core.h"
#include "Editor.h"
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
    Check(s.Deadline() == 31000);
    Check(s.Tick(31000).has_value());
    Check(s.Tick(31000).has_value());
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
void SingleTrigger() {
    Settings config;
    config.trigger = Trigger::Mouse(2);
    auto s = Held(config);
    Check(!s.Active());
    s.Mouse(9, 2, 0);
    s.Reconcile(0);
    Check(s.Active() && s.Target() == 9);
    s.Tick(0);
    Check(s.Tick(30000)->device == 9);
    s.Mouse(7, 2, 0);
    s.Reconcile(31000);
    Check(s.Target() == 9);
    s.Mouse(9, 0, 2);
    s.Reconcile(32000);
    Check(s.Target() == 7);
    config.trigger = Trigger::Keyboard({0x1D, 0});
    s.Configure(config);
    s.Mouse(9, 0, 0);
    s.Keyboard(1, {0x2E, 0}, true);
    s.Reconcile(0);
    Check(!s.Active());
    s.Keyboard(1, {0x1D, 0}, true);
    s.Reconcile(0);
    Check(s.Active() && s.Target() == 9);
    s.Mouse(7, 0, 0);
    s.Reconcile(1);
    Check(s.Target() == 9);
    s.Keyboard(2, {0x1D, 0}, true);
    s.Keyboard(1, {0x1D, 0}, false);
    s.Reconcile(2);
    Check(s.Active());
    s.Keyboard(2, {0x1D, 0}, false);
    s.Reconcile(3);
    Check(!s.Active());
    config.trigger = Trigger::Mouse(1);
    s.Configure(config);
    s.Mouse(7, 1, 0);
    s.Reconcile(4);
    Check(s.Active() && s.Target() == 7);
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
    auto s = Held(config);
    const auto initial = s.Tick(1000);
    Check(initial && initial->x == 1 && initial->y == -2);
    Check(!s.Tick(1000));
    Check(!s.Tick(10999));
    const auto next = s.Tick(11000);
    Check(next && next->y == 50);
    const auto third = s.Tick(11000);
    Check(third && third->x == 12 && third->y == -34);
    Check(s.Deadline() == 11000);
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
    Reject([&] { s.Configure(config); });
}
void Parsing() {
    Settings config;
    const auto text = Script(config.program);
    Check(text == "wait(30ms)\n\nloop {\n    move(-100)\n}\n");
    Check(Script(Parse(text)) == text);
    config.program = Parse("// comment\nwait(30ms); loop { move(+2, -4); wait(8) move(-1) };");
    Check(config.program.loop.size() == 3);
    Check(config.program.loop[0].x == 2);
    config.trigger = Trigger::Keyboard({0x38, 2});
    Check(Encode(Decode(Encode(config))) == Encode(config));
    for (const char *invalid :
         {"", "loop {}", "loop { move(1) ", "wait(-1)", "move(1000001)", "move(-2147483648)",
          "wait(600001ms)", "move(1.2)", "move(1,,2)", "loop { move(1) } move(2)",
          "loop { loop { move(1) } }", "print(1)", "wait(1msx)", "move(1) garbage"})
        Reject([&] { Parse(invalid); });
    Reject([] { Parse(std::string(16385, ' ')); });
    Reject([] { Decode(std::string(20001, 'x')); });
    Check(Decode("MouseControl 1\nbuttons 1\ngap 0\nsequence\nmove(1)").trigger.code == 1);
    Reject([] { Decode("MouseControl 1\nbuttons 1\ngap 1001\nsequence\nmove(1)"); });
    Reject([] { Decode("MouseControl 1\nbuttons 1\nbuttons 2\ngap 8\nsequence\nmove(1)"); });
    Reject([] {
        Settings s;
        s.trigger = Trigger::Mouse(0);
        Validate(s);
    });
    Reject([] {
        Settings s;
        s.trigger = Trigger::Mouse(6);
        Validate(s);
    });
    Reject([] {
        Settings s;
        s.trigger = Trigger::Keyboard({0, 0});
        Validate(s);
    });
    Reject([] {
        Settings s;
        s.trigger = Trigger::Keyboard({0x1D, 1});
        Validate(s);
    });
    const auto old = Decode(
        "MouseControl 1\nbuttons 3\ngap 8\nkey 29 0\nsequence\nwait(30ms)\nloop { move(-100) }\n");
    Check(old.trigger.kind == TriggerKind::Mouse && old.trigger.code == 1);
    auto migrated = Held(old);
    migrated.Tick(1000);
    Check(migrated.Tick(31000).has_value());
    Check(migrated.Tick(31000).has_value());
    const auto keyboard =
        Decode("MouseControl 1\nbuttons 0\ngap 8\nkey 29 0\nkey 56 0\nsequence\nmove(-100)\n");
    Check(keyboard.trigger.kind == TriggerKind::Keyboard && keyboard.trigger.code == 29);
    const auto comments = Decode("MouseControl 2\ntrigger mouse 1 0\nsequence\n// retain me\nloop "
                                 "{\n    move(-2) // note\n}\n");
    Check(Encode(comments).find("// retain me") != std::string::npos);
    Check(Encode(comments).find("gap") == std::string::npos);
    Check(Encode(Decode(Encode(comments))) == Encode(comments));
    Reject(
        [] { Decode("MouseControl 2\ntrigger mouse 1 0\ntrigger mouse 2 0\nsequence\nmove(1)"); });
    std::string steps;
    for (unsigned i = 0; i < 65; ++i)
        steps += "move(1)\n";
    Reject([&] { Parse(steps); });
}
void Editing() {
    Check(WindowsLines(L"a\nb\r\nc\r") == L"a\r\nb\r\nc\r\n");
    Check(IndentCode(L"loop {\nmove(-1) // }\nwait(4)\n}\n") ==
          L"loop {\r\n    move(-1) // }\r\n    wait(4)\r\n}\r\n");
    auto edit = Newline(L"loop {}", 6, 6);
    Check(edit.text == L"loop {\r\n    \r\n}" && edit.begin == 12 && edit.end == 12);
    edit = Newline(L"    move(1)", 11, 11);
    Check(edit.text == L"    move(1)\r\n    ");
    edit = Tab(L"move(1)", 0, 0, false);
    Check(edit.text == L"    move(1)" && edit.begin == 4);
    edit = Tab(L"    move(1)", 2, 2, true);
    Check(edit.text == L"move(1)" && edit.begin == 0);
    edit = Tab(L"    move(1)", 0, 2, true);
    Check(edit.begin == 0 && edit.end == 0);
    edit = Tab(L"a\r\nb", 0, 4, false);
    Check(edit.text == L"    a\r\n    b");
    edit = Tab(edit.text, 0, edit.text.size(), true);
    Check(edit.text == L"a\r\nb");
    edit = ClosingBrace(L"    ", 4, 4);
    Check(edit.text == L"}" && edit.begin == 1);
    std::mt19937 random(521);
    for (unsigned i = 0; i < 4000; ++i) {
        std::wstring value;
        for (unsigned n = random() % 100; n--;) {
            const wchar_t chars[] = L" abc{}\r\n\t";
            value += chars[random() % 10];
        }
        const auto a = random() % (value.size() + 1), b = a + random() % (value.size() - a + 1);
        for (bool outdent : {false, true}) {
            const auto result = Tab(value, a, b, outdent);
            Check(result.begin <= result.end && result.end <= result.text.size());
        }
        const auto line = Newline(value, a, b);
        Check(line.begin == line.end && line.end <= line.text.size());
    }
}
void Stress() {
    Settings fast;
    fast.program.start.clear();
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
        SingleTrigger();
        Cancellation();
        Parsing();
        Editing();
        Stress();
        std::cout << "PASS " << checks
                  << " parser, scheduler, single-trigger selection, zero-gap loops, indentation, "
                     "cancellation and "
                     "stress assertions. No driver or desktop input accessed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
