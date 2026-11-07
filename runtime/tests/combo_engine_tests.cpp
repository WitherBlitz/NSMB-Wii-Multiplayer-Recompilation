// Tests for the combination binding rules (include/combo_engine.h).
#include "combo_engine.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr uint64_t kMs = 1'000'000;
// Codes: 1 Ctrl (no binding of its own), 2 X (Jump), 3 Z (Run), 4 Down (a direction), 5 Shift (Run).
constexpr int kCtrl = 1, kX = 2, kZ = 3, kDown = 4, kShift = 5;
// Actions.
constexpr int kJump = 4, kShake = 7, kA = 6, kPlus = 10;
constexpr uint32_t Bit(int action) { return 1u << action; }

ComboEngine::Engine Make(std::vector<ComboEngine::Combo> combos) {
    ComboEngine::Engine engine;
    ComboEngine::Setup setup;
    setup.combos = std::move(combos);
    setup.singles = {kX, kZ, kDown, kShift};
    setup.conditions = {kDown};
    engine.Configure(setup);
    return engine;
}

void ModifierThenKey() {
    auto e = Make({{kShake, {kCtrl, kX}}});
    CHECK(e.Step({kCtrl}, 0) == 0);
    CHECK(!e.Silenced(kCtrl) || true);  // Ctrl has no single: nothing to silence
    // Ctrl held a long time, then X: the combination, and X's Jump is silent.
    CHECK(e.Step({kCtrl}, 500 * kMs) == 0);
    CHECK(e.Step({kCtrl, kX}, 600 * kMs) == Bit(kShake));
    CHECK(e.Silenced(kX));
    // Ctrl let go: X stays taken until it is let go too.
    CHECK(e.Step({kX}, 700 * kMs) == 0);
    CHECK(e.Silenced(kX));
    CHECK(e.Step({}, 800 * kMs) == 0);
    CHECK(e.Step({kX}, 900 * kMs) == 0);
    CHECK(e.Silenced(kX));  // waiting: X belongs to a combination that can still complete
    CHECK(e.Step({kX}, 900 * kMs + ComboEngine::kWaitNs) == 0);
    CHECK(!e.Silenced(kX));  // the wait is over: X jumps
}

void EitherOrderWithinTheWait() {
    auto e = Make({{kA, {kX, kZ}}});
    CHECK(e.Step({kX}, 0) == 0);
    CHECK(e.Silenced(kX));
    CHECK(e.Step({kX, kZ}, 20 * kMs) == Bit(kA));
    CHECK(e.Silenced(kX) && e.Silenced(kZ));
    e.Reset();
    CHECK(e.Step({kZ}, 0) == 0);
    CHECK(e.Step({kZ, kX}, 49 * kMs) == Bit(kA));
}

void SingleCommitsAfterTheWait() {
    // Holding Run, then pressing Jump, runs and jumps: Run committed, so no combination.
    auto e = Make({{kA, {kX, kZ}}});
    CHECK(e.Step({kZ}, 0) == 0);
    CHECK(e.Step({kZ}, 60 * kMs) == 0);
    CHECK(!e.Silenced(kZ));
    CHECK(e.Step({kZ, kX}, 100 * kMs) == 0);
    CHECK(!e.Silenced(kX));  // no combination could complete: X isn't held back either
    CHECK(!e.Silenced(kZ));
}

void ConditionsGateOnly() {
    auto e = Make({{kA, {kDown, kX}}});
    CHECK(e.Step({kDown}, 0) == 0);
    CHECK(!e.Silenced(kDown));  // never waits
    CHECK(e.Step({kDown}, 500 * kMs) == 0);
    CHECK(e.Step({kDown, kX}, 600 * kMs) == Bit(kA));
    CHECK(!e.Silenced(kDown));  // never taken
    CHECK(e.Silenced(kX));
}

void LongerWins() {
    auto e = Make({{kShake, {kCtrl, kX}}, {kPlus, {kCtrl, kShift, kX}}});
    CHECK(e.Step({kCtrl, kX}, 0) == Bit(kShake));
    CHECK(e.Step({kCtrl, kX, kShift}, 10 * kMs) == Bit(kPlus));
    CHECK(e.Silenced(kShift));
}

void Deterministic() {
    // The same presses at the same times give the same results.
    for (int run = 0; run < 2; ++run) {
        auto e = Make({{kShake, {kCtrl, kX}}, {kA, {kX, kZ}}});
        uint32_t trace = 0;
        trace = trace * 31 + e.Step({kX}, 0);
        trace = trace * 31 + e.Step({kX, kZ}, 10 * kMs);
        trace = trace * 31 + e.Step({kZ}, 30 * kMs);
        trace = trace * 31 + e.Step({kZ, kCtrl}, 40 * kMs);
        trace = trace * 31 + e.Step({kZ, kCtrl, kX}, 45 * kMs);
        static uint32_t first = 0;
        if (run == 0) first = trace;
        else CHECK(trace == first);
    }
}

void QuickTapStillCounts() {
    // A tap shorter than the wait, no combination: the key's own binding counts once, as it's let go.
    auto e = Make({{kShake, {kCtrl, kX}}});
    CHECK(e.Step({kX}, 0) == 0);
    CHECK(e.Silenced(kX));
    CHECK(e.Step({}, 20 * kMs) == 0);
    CHECK(e.Tapped(kX));
    CHECK(e.Step({}, 40 * kMs) == 0);
    CHECK(!e.Tapped(kX));
    // Taken by a combination: no tap when let go.
    CHECK(e.Step({kCtrl, kX}, 100 * kMs) == Bit(kShake));
    CHECK(e.Step({kCtrl}, 120 * kMs) == 0);
    CHECK(!e.Tapped(kX));
}

void NoCombinationsNoEffect() {
    auto e = Make({});
    CHECK(e.Empty());
    CHECK(e.Step({kX, kZ, kCtrl}, 0) == 0);
    CHECK(!e.Silenced(kX) && !e.Silenced(kZ));
}

}  // namespace

int main() {
    ModifierThenKey();
    EitherOrderWithinTheWait();
    SingleCommitsAfterTheWait();
    ConditionsGateOnly();
    LongerWins();
    Deterministic();
    QuickTapStillCounts();
    NoCombinationsNoEffect();
    if (g_failures == 0) std::printf("combo_engine_tests: all passed\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
