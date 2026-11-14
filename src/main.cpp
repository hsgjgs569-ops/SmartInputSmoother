#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <cocos2d.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <cmath>

using namespace geode::prelude;

namespace {
    // Composite key: (player1 ? 0 : 1) in high bits + button id
    inline int makeKey(int button, bool isPlayer1) {
        return (button & 0x7fffffff) | (isPlayer1 ? 0 : (1 << 30));
    }

    struct PulseState {
        bool physicalHeld = false;
        bool virtualDown = false;
        int button = 0;
        bool isPlayer1 = true;
    };

    // Per-layer runtime state
    struct LayerState {
        bool suppressHook = false;
        std::unordered_map<int, PulseState> states;
        bool scheduled = false;
    };

    // Use a global map keyed by layer pointer; cleared on layer destruction risk is low in practice,
    // but we also lazily clean by checking layer validity is non-trivial. For simplicity keep per-instance via field.
}

class $modify(SmoothInputLayer, GJBaseGameLayer) {
    struct Fields {
        bool suppressHook = false;
        std::unordered_map<int, PulseState> states;
        bool pulseScheduled = false;
    };

    static void onModify(auto& self) {
        // Intercept early so other mods see smoothed stream; adjust if needed.
        (void) self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarlyPre);
    }

    // Safe dispatch of (possibly emulated) input back into the game.
    void dispatchButton(bool down, int button, bool isPlayer1) {
        m_fields->suppressHook = true;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        m_fields->suppressHook = false;
    }

    int getTargetCps() {
        int cps = 30;
        cps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-cps"));
        if (cps < 10) cps = 10;
        if (cps > 60) cps = 60;
        return cps;
    }

    std::string getMode() {
        auto mode = Mod::get()->getSettingValue<std::string>("smoothing-mode");
        if (mode != "Linear" && mode != "Burst") mode = "Linear";
        return mode;
    }

    bool isEnabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    // Pulse engine: called every frame while any physical button is held.
    void pulseUpdate(float dt) {
        auto scheduler = cocos2d::CCDirector::sharedDirector()->getScheduler();

        if (!isEnabled()) {
            // Safety: release any virtual holds if disabled mid-hold
            for (auto& [key, st] : m_fields->states) {
                if (st.virtualDown) {
                    st.virtualDown = false;
                    dispatchButton(false, st.button, st.isPlayer1);
                }
                st.physicalHeld = false;
            }
            scheduler->unscheduleSelector(schedule_selector(SmoothInputLayer::pulseUpdate), this);
            m_fields->pulseScheduled = false;
            return;
        }

        int cps = getTargetCps();
        float interval = 1.0f / static_cast<float>(cps);
        auto mode = getMode();

        // We keep a small per-key timer via accumulators stored in tag-like way? easier: use static-like accum per key in map
        // but PulseState has no accum; we'll compute using global time slices via scheduler dt accumulation in fields not present.
        // Simpler: drive pulses by elapsed time per call using member accumulators stored in a side map.
        // To keep code self-contained, store accumulators in another map.
        struct Accum { float t = 0.f; int burstPhase = 0; };
        static thread_local std::unordered_map<void*, std::unordered_map<int, Accum>> accumMap;
        auto& accs = accumMap[this];

        bool anyHeld = false;
        for (auto& [key, st] : m_fields->states) {
            if (!st.physicalHeld) continue;
            anyHeld = true;
            auto& ac = accs[key];
            ac.t += dt;

            if (mode == "Linear") {
                // Emit a micro-click (down+up) once per interval while held.
                // Press duration is a small fraction of interval, but we must ensure release between pulses.
                float pressDur = std::min(0.02f, interval * 0.35f);
                // Cycle: [0, pressDur) -> down, [pressDur, interval) -> up
                if (ac.t >= interval) ac.t = std::fmod(ac.t, interval);
                if (ac.t < pressDur) {
                    if (!st.virtualDown) {
                        st.virtualDown = true;
                        dispatchButton(true, st.button, st.isPlayer1);
                    }
                } else {
                    if (st.virtualDown) {
                        st.virtualDown = false;
                        dispatchButton(false, st.button, st.isPlayer1);
                    }
                }
            } else { // Burst
                // Burst: 3 quick clicks, then pause. Effective rate approximates target CPS.
                float clickGap = std::max(0.015f, interval * 0.5f); // fast within burst
                float pressDur = std::min(0.012f, clickGap * 0.4f);
                float burstLen = 3.f * clickGap;
                float pauseLen = std::max(0.f, 3.f * interval - burstLen);
                float cycle = burstLen + pauseLen;
                if (ac.t >= cycle) ac.t = std::fmod(ac.t, cycle);
                if (ac.t < burstLen) {
                    float in = std::fmod(ac.t, clickGap);
                    if (in < pressDur) {
                        if (!st.virtualDown) { st.virtualDown = true; dispatchButton(true, st.button, st.isPlayer1); }
                    } else {
                        if (st.virtualDown) { st.virtualDown = false; dispatchButton(false, st.button, st.isPlayer1); }
                    }
                } else {
                    if (st.virtualDown) { st.virtualDown = false; dispatchButton(false, st.button, st.isPlayer1); }
                }
            }
        }

        // cleanup accumulators for keys no longer held
        for (auto it = accs.begin(); it != accs.end(); ) {
            auto sit = m_fields->states.find(it->first);
            if (sit == m_fields->states.end() || !sit->second.physicalHeld) it = accs.erase(it);
            else ++it;
        }

        if (!anyHeld) {
            cocos2d::CCDirector::sharedDirector()->getScheduler()->unscheduleSelector(schedule_selector(SmoothInputLayer::pulseUpdate), this);
            m_fields->pulseScheduled = false;
            accumMap.erase(this);
        }
    }

    void ensureScheduled() {
        auto sched = cocos2d::CCDirector::sharedDirector()->getScheduler();
        if (!m_fields->pulseScheduled) {
            // Run every frame for precise timing
            sched->scheduleSelector(schedule_selector(SmoothInputLayer::pulseUpdate), this, 0.f, false);
            m_fields->pulseScheduled = true;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Pass through emulated events and when disabled or when in recursion guard
        if (m_fields->suppressHook || !isEnabled()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int key = makeKey(button, isPlayer1);
        auto& st = m_fields->states[key];
        st.button = button;
        st.isPlayer1 = isPlayer1;

        if (down) {
            // Physical press: start/continue pulsing instead of holding continuously
            if (!st.physicalHeld) {
                st.physicalHeld = true;
                st.virtualDown = false;
                // Immediate first micro-click for responsiveness
                st.virtualDown = true;
                dispatchButton(true, button, isPlayer1);
            }
            ensureScheduled();
            // Do NOT pass original down directly (we manage pulses)
            return;
        } else {
            // Physical release: stop immediately and guarantee release
            if (st.physicalHeld) {
                st.physicalHeld = false;
                // Final release if virtually held
                if (st.virtualDown) {
                    st.virtualDown = false;
                    dispatchButton(false, button, isPlayer1);
                } else {
                    // Ensure game sees a release state even if we were in off-phase
                    dispatchButton(false, button, isPlayer1);
                }
            } else {
                // No known hold; forward release to be safe
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
            }
            // pulseUpdate will unschedule itself when nothing held
            return;
        }
    }
};
