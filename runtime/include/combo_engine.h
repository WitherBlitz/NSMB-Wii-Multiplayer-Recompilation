#pragma once

// NSMBW: combination bindings, the rules alone (no SDL, no config), so they can be tested on their own
// (runtime/tests/combo_engine_tests.cpp). A combination is 2 or 3 members (keys, mouse buttons, or a
// controller's buttons and axis directions, as plain codes here) bound to one action; it is held while
// all its members are, pressed in any order. combo_bindings.cpp feeds it what is held on each input
// read; the result is decided before the game's input is sampled, so in LAN play every peer gets the
// same remote bits whatever its combinations (FrameInput latches and sends the sample).
//
// The rules:
// 1. A member with no binding of its own is a modifier: it never does anything alone, and can be held
//    any time before the rest.
// 2. A member that also has a binding of its own (a "single") waits kWaitNs from its press for the rest
//    of a combination. Completed in time, the single never happens. Otherwise the single starts then,
//    and that member can't take part in a combination until it is let go (holding Run, then pressing
//    Jump, still runs and jumps when Run + Jump is a combination). A tap let go within the wait, with
//    no combination, still counts once (Tapped).
// 3. While a combination is held its members are taken: their singles stay silent until each member
//    is let go, even when the combination breaks first.
// 4. When two combinations are held and one's members are part of the other's, the longer one wins.
// 5. Conditions (the directions and tilt) never wait and are never taken: they keep doing their own
//    thing and only gate a combination (Down + Jump doesn't delay or swallow Down).
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ComboEngine {

constexpr uint64_t kWaitNs = 50'000'000;  // 50 ms, fixed (the user's choice): 3 frames
constexpr size_t kMaxMembers = 3;

struct Combo {
    int action = 0;
    std::vector<int> members;  // 2 or 3 codes
};

struct Setup {
    std::vector<Combo> combos;
    std::vector<int> singles;     // codes with a binding of their own
    std::vector<int> conditions;  // codes that only gate (directions, tilt)
};

class Engine {
public:
    void Configure(Setup setup) {
        m_setup = std::move(setup);
        for (Combo& combo : m_setup.combos) {
            std::sort(combo.members.begin(), combo.members.end());
            combo.members.erase(std::unique(combo.members.begin(), combo.members.end()), combo.members.end());
        }
        m_setup.combos.erase(std::remove_if(m_setup.combos.begin(), m_setup.combos.end(),
                                            [](const Combo& c) { return c.members.size() < 2; }),
                             m_setup.combos.end());
        Reset();
    }

    bool Empty() const { return m_setup.combos.empty(); }

    // Everything let go (focus lost, a controller unplugged, a capture began).
    void Reset() {
        m_keys.clear();
        m_actions = 0;
        m_silenced.clear();
        m_tapped.clear();
    }

    // `held`: every code held now (others are ignored). Returns the actions held through combinations,
    // as 1 << action bits.
    uint32_t Step(const std::vector<int>& held, uint64_t nowNs) {
        // Keys let go are forgotten; new ones start now. One let go while it was still waiting (a tap
        // shorter than the wait, with no combination) counts as pressed for this read: Tapped.
        m_tapped.clear();
        for (const Key& key : m_keys) {
            if (!Contains(held, key.code) && key.waiting) m_tapped.push_back(key.code);
        }
        m_keys.erase(std::remove_if(m_keys.begin(), m_keys.end(),
                                    [&](const Key& k) { return !Contains(held, k.code); }),
                     m_keys.end());
        for (const int code : held) {
            if (IsMember(code) && FindKey(code) == nullptr) {
                m_keys.push_back(Key{code, nowNs, false, false, false});
            }
        }
        // The combinations held, members not committed to their singles.
        std::vector<const Combo*> held2;
        for (const Combo& combo : m_setup.combos) {
            bool all = true;
            for (const int member : combo.members) {
                const Key* key = FindKey(member);
                if (key == nullptr || (key->committed && !IsCondition(member))) {
                    all = false;
                    break;
                }
            }
            if (all) held2.push_back(&combo);
        }
        // The longer wins where one's members are all in the other's.
        m_actions = 0;
        for (const Combo* combo : held2) {
            const bool beaten = std::any_of(held2.begin(), held2.end(), [&](const Combo* other) {
                return other != combo && other->members.size() > combo->members.size() &&
                       std::includes(other->members.begin(), other->members.end(), combo->members.begin(),
                                     combo->members.end());
            });
            if (beaten) continue;
            m_actions |= 1u << combo->action;
            for (const int member : combo->members) {
                if (!IsCondition(member)) FindKey(member)->consumed = true;
            }
        }
        // Singles waiting for a combination that can still complete; the wait over, they commit.
        m_silenced.clear();
        for (Key& key : m_keys) {
            key.waiting = false;
            if (IsCondition(key.code)) continue;
            if (key.consumed) {
                m_silenced.push_back(key.code);
                continue;
            }
            if (key.committed || !Contains(m_setup.singles, key.code)) continue;
            if (nowNs - key.downNs < kWaitNs && CanStillComplete(key.code)) {
                m_silenced.push_back(key.code);
                key.waiting = true;
            } else {
                key.committed = true;
            }
        }
        return m_actions;
    }

    uint32_t Actions() const { return m_actions; }
    // Whether `code`'s own binding is silenced right now (taken by a combination, or waiting).
    bool Silenced(int code) const { return Contains(m_silenced, code); }
    const std::vector<int>& SilencedCodes() const { return m_silenced; }
    // Whether `code` was let go this read while still waiting: its own binding counts as pressed once.
    bool Tapped(int code) const { return Contains(m_tapped, code); }
    const std::vector<int>& TappedCodes() const { return m_tapped; }

private:
    struct Key {
        int code;
        uint64_t downNs;
        bool consumed;   // taken by a combination until let go
        bool committed;  // its single started: out of combinations until let go
        bool waiting;    // silenced, waiting for a combination
    };

    static bool Contains(const std::vector<int>& list, int code) {
        return std::find(list.begin(), list.end(), code) != list.end();
    }
    bool IsCondition(int code) const { return Contains(m_setup.conditions, code); }
    bool IsMember(int code) const {
        return std::any_of(m_setup.combos.begin(), m_setup.combos.end(),
                           [&](const Combo& c) { return Contains(c.members, code); });
    }
    Key* FindKey(int code) {
        for (Key& key : m_keys) {
            if (key.code == code) return &key;
        }
        return nullptr;
    }
    const Key* FindKey(int code) const { return const_cast<Engine*>(this)->FindKey(code); }
    // A combination with `code` none of whose other (non-condition) members has committed.
    bool CanStillComplete(int code) const {
        for (const Combo& combo : m_setup.combos) {
            if (!Contains(combo.members, code)) continue;
            const bool blocked = std::any_of(combo.members.begin(), combo.members.end(), [&](int member) {
                const Key* key = FindKey(member);
                return member != code && !IsCondition(member) && key != nullptr && key->committed;
            });
            if (!blocked) return true;
        }
        return false;
    }

    Setup m_setup;
    std::vector<Key> m_keys;
    uint32_t m_actions = 0;
    std::vector<int> m_silenced;
    std::vector<int> m_tapped;
};

} // namespace ComboEngine
