// ============================================================================
// VigemPad -- Implementierung. Kopfkommentar: VigemPad.hpp
// Semantik 1:1 aus re_vr.cpp (Plugin), inklusive der Lock-Reihenfolge.
// ============================================================================

#include <Windows.h>
#include <ViGEm/Client.h>

#include <cstring>
#include <fstream>
#include <filesystem>

#include <safetyhook.hpp>

#include "VigemPad.hpp"

namespace {

// Die Namen sind die der Lua-Fassung -- sie stehen so in re4_vr_binding.lua.
WORD button_mask(std::string_view btn) {
    if (btn == "A") return XUSB_GAMEPAD_A;
    if (btn == "B") return XUSB_GAMEPAD_B;
    if (btn == "X") return XUSB_GAMEPAD_X;
    if (btn == "Y") return XUSB_GAMEPAD_Y;
    if (btn == "LB") return XUSB_GAMEPAD_LEFT_SHOULDER;
    if (btn == "RB") return XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (btn == "BACK") return XUSB_GAMEPAD_BACK;
    if (btn == "START") return XUSB_GAMEPAD_START;
    if (btn == "LS") return XUSB_GAMEPAD_LEFT_THUMB;
    if (btn == "RS") return XUSB_GAMEPAD_RIGHT_THUMB;
    if (btn == "DPAD_UP") return XUSB_GAMEPAD_DPAD_UP;
    if (btn == "DPAD_DOWN") return XUSB_GAMEPAD_DPAD_DOWN;
    if (btn == "DPAD_LEFT") return XUSB_GAMEPAD_DPAD_LEFT;
    if (btn == "DPAD_RIGHT") return XUSB_GAMEPAD_DPAD_RIGHT;

    return 0;
}

SHORT clamp_axis(double v) {
    if (v < -1.0) {
        v = -1.0;
    }

    if (v > 1.0) {
        v = 1.0;
    }

    return static_cast<SHORT>(v * 32767.0);
}

BYTE clamp_trigger(double v) {
    if (v < 0.0) {
        v = 0.0;
    }

    if (v > 1.0) {
        v = 1.0;
    }

    return static_cast<BYTE>(v * 255.0);
}

// [HAPTIK] Der Callback des Plugins, mitportiert. Er speichert nur -- die
// Weiterleitung an die Controller macht der Fork bereits an anderer Stelle
// (VR::trigger_haptic_vibration in Holster/Recoil), s. Kopfkommentar.
VOID CALLBACK vibration_callback(PVIGEM_CLIENT client, PVIGEM_TARGET target,
                                 UCHAR large_motor, UCHAR small_motor, UCHAR led,
                                 LPVOID user) {
    (void)client;
    (void)target;
    (void)led;
    (void)user;

    VigemPad::get().on_rumble(large_motor, small_motor);
}

}   // namespace

VigemPad& VigemPad::get() {
    static VigemPad inst;

    return inst;
}

VigemPad::~VigemPad() {
    shutdown();
}

void VigemPad::on_rumble(uint8_t large_motor, uint8_t small_motor) {
    m_rumble_large.store(large_motor);
    m_rumble_small.store(small_motor);
}

bool VigemPad::probe() {
    auto* client = vigem_alloc();

    if (client == nullptr) {
        return false;
    }

    const bool ok = VIGEM_SUCCESS(vigem_connect(client));

    if (ok) {
        vigem_disconnect(client);
    }

    vigem_free(client);

    return ok;
}

bool VigemPad::init() {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (m_ready.load()) {
        return true;
    }

    auto* client = vigem_alloc();

    if (client == nullptr) {
        return false;
    }

    if (!VIGEM_SUCCESS(vigem_connect(client))) {
        // Kein ViGEmBus-Treiber installiert -- sauber aufraeumen und melden.
        vigem_free(client);

        return false;
    }

    auto* target = vigem_target_x360_alloc();

    if (target == nullptr) {
        vigem_disconnect(client);
        vigem_free(client);

        return false;
    }

    if (!VIGEM_SUCCESS(vigem_target_add(client, target))) {
        vigem_target_free(target);
        vigem_disconnect(client);
        vigem_free(client);

        return false;
    }

    // Rueckgabewert bewusst ignoriert (wie im Plugin): scheitert die
    // Registrierung, laeuft das Pad ohne Vibrations-Rueckmeldung weiter.
    const auto vret =
        vigem_target_x360_register_notification(client, target, vibration_callback, nullptr);
    (void)vret;

    auto* report = new XUSB_REPORT{};
    XUSB_REPORT_INIT(report);

    m_client = client;
    m_target = target;
    m_report = report;
    m_ready.store(true);

    // [SLOT-MESSUNG AUSGEBAUT 2026-09-08] Die Startzeile nach
    // re4_vigem_slot.txt ist raus; der Slot-Fix selbst bleibt unveraendert.

    return true;
}

void VigemPad::shutdown() {
    std::lock_guard<std::mutex> lock{m_mtx};

    m_ready.store(false);

    if (m_target != nullptr) {
        auto* target = static_cast<PVIGEM_TARGET>(m_target);
        vigem_target_x360_unregister_notification(target);

        if (m_client != nullptr) {
            vigem_target_remove(static_cast<PVIGEM_CLIENT>(m_client), target);
        }

        vigem_target_free(target);
        m_target = nullptr;
    }

    if (m_client != nullptr) {
        auto* client = static_cast<PVIGEM_CLIENT>(m_client);
        vigem_disconnect(client);
        vigem_free(client);
        m_client = nullptr;
    }

    if (m_report != nullptr) {
        delete static_cast<XUSB_REPORT*>(m_report);
        m_report = nullptr;
    }
}

// Gesperrte Achsen werden bei JEDEM Report auf 0 gezwungen -- auch wenn
// set_axis sie kurz zuvor gesetzt hat. Genau so haelt es das Plugin.
void VigemPad::apply_axis_locks_locked() {
    if (m_report == nullptr) {
        return;
    }

    auto* r = static_cast<XUSB_REPORT*>(m_report);

    if (m_lock_lx) {
        r->sThumbLX = 0;
    }

    if (m_lock_ly) {
        r->sThumbLY = 0;
    }

    if (m_lock_rx) {
        r->sThumbRX = 0;
    }

    if (m_lock_ry) {
        r->sThumbRY = 0;
    }
}

void VigemPad::send_locked() {
    if (m_client == nullptr || m_target == nullptr || m_report == nullptr || !m_ready.load()) {
        return;
    }

    // [SLOT-FIX] Waehrend der Identifikation traegt das Pad den Fingerabdruck --
    // der normale Frame-Stand wuerde ihn sofort ueberschreiben.
    if (m_fp_active) {
        return;
    }

    vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                             static_cast<PVIGEM_TARGET>(m_target),
                             *static_cast<XUSB_REPORT*>(m_report));
}

void VigemPad::set_button(std::string_view btn, bool down) {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (!m_ready.load() || m_client == nullptr || m_target == nullptr) {
        return;
    }

    const WORD mask = button_mask(btn);

    if (mask == 0) {
        return;
    }

    auto* r = static_cast<XUSB_REPORT*>(m_report);

    if (down) {
        r->wButtons |= mask;
    } else {
        r->wButtons &= static_cast<WORD>(~mask);
    }

    apply_axis_locks_locked();
    send_locked();
}

void VigemPad::set_axis(std::string_view axis, double value) {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (!m_ready.load() || m_client == nullptr || m_target == nullptr) {
        return;
    }

    auto* r = static_cast<XUSB_REPORT*>(m_report);
    const SHORT out = clamp_axis(value);

    // Eine gesperrte Achse wird gar nicht erst beschrieben (nicht nur beim
    // Senden genullt) -- 1:1 wie im Plugin.
    if (axis == "LX") {
        if (!m_lock_lx) {
            r->sThumbLX = out;
        }
    } else if (axis == "LY") {
        if (!m_lock_ly) {
            r->sThumbLY = out;
        }
    } else if (axis == "RX") {
        if (!m_lock_rx) {
            r->sThumbRX = out;
        }
    } else if (axis == "RY") {
        if (!m_lock_ry) {
            r->sThumbRY = out;
        }
    } else {
        return;
    }

    apply_axis_locks_locked();
    send_locked();
}

void VigemPad::set_trigger(std::string_view trig, double value) {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (!m_ready.load() || m_client == nullptr || m_target == nullptr) {
        return;
    }

    auto* r = static_cast<XUSB_REPORT*>(m_report);
    const BYTE out = clamp_trigger(value);

    if (trig == "LT") {
        r->bLeftTrigger = out;
    } else if (trig == "RT") {
        r->bRightTrigger = out;
    } else {
        return;
    }

    apply_axis_locks_locked();
    send_locked();
}

void VigemPad::block_axis(std::string_view axis, bool enabled) {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (!m_ready.load() || m_client == nullptr || m_target == nullptr) {
        return;
    }

    if (axis == "LX") {
        m_lock_lx = enabled;
    } else if (axis == "LY") {
        m_lock_ly = enabled;
    } else if (axis == "RX") {
        m_lock_rx = enabled;
    } else if (axis == "RY") {
        m_lock_ry = enabled;
    } else {
        return;
    }

    apply_axis_locks_locked();
    send_locked();
}

// ============================================================================
// [SLOT-FIX 08.09.2026] "Xbox-Pad an -> VR-Controller tot"
//
// GEMESSEN am 08.09. (re4_vigem_slot.txt): mit eingeschaltetem Xbox-Pad sitzt
// das ECHTE Pad auf XInput-Slot 0 (Stick-Drift sichtbar) und unser virtuelles
// auf Slot 1. RE4 pollt Slot 0 -- unsere Eingaben kommen also nie an.
// ViGEms eigene Auskunft taugt dafuer NICHT: vigem_target_x360_get_user_index
// meldete 0, obwohl das Pad nachweislich auf 1 lag.
//
// Deshalb wird der Slot per FINGERABDRUCK bestimmt: wir schreiben auf unser
// Pad zwei Stick-Werte, die kein echtes Geraet zufaellig exakt trifft, lesen
// alle vier Slots und nehmen den, der genau diese Zahlen meldet.
//
// Danach wird XInputGetState so umgelenkt, dass Index 0 unseren Slot liest.
// KEIN Tausch: alle anderen Indizes gehen unveraendert durch, damit ein Spiel,
// das direkt unseren Slot pollt, weiter funktioniert.
//
// Sicherheitsnetze: kein eindeutiger Treffer, Slot 0, fehlendes xinput1_4.dll
// oder ein fehlgeschlagener Hook -> es passiert GAR NICHTS, alles bleibt wie
// zuvor. Jeder Schritt landet in reframework/data/re4_vigem_slot.txt.
// ============================================================================

namespace {

struct XiGamepad {
    WORD buttons;
    BYTE lt;
    BYTE rt;
    SHORT lx;
    SHORT ly;
    SHORT rx;
    SHORT ry;
};

struct XiState {
    DWORD packet;
    XiGamepad pad;
};

using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XiState*);

safetyhook::InlineHook g_xigs_hook{};
std::atomic<int> g_our_slot{-1};

// Werte, die kein echter Stick zufaellig exakt liefert.
constexpr SHORT FP_LX = -12345;
constexpr SHORT FP_LY = 23456;

DWORD WINAPI xigs_hooked(DWORD index, XiState* state) {
    const int our = g_our_slot.load(std::memory_order_relaxed);

    if (index == 0 && our > 0) {
        index = static_cast<DWORD>(our);
    }

    return g_xigs_hook.call<DWORD>(index, state);
}

// [LOG AUSGEBAUT 2026-09-08] Schreibt nicht mehr nach re4_vigem_slot.txt. Der
// leere Stream ist nie `good()`, damit laufen alle `if (f)`-Zweige der
// Slot-Fix-Logik still ins Leere -- an der Logik selbst aendert sich nichts.
std::ofstream slot_log() {
    return std::ofstream{};
}

PFN_XInputGetState raw_get_state() {
    auto* mod = GetModuleHandleA("xinput1_4.dll");

    if (mod == nullptr) {
        mod = LoadLibraryA("xinput1_4.dll");
    }

    if (mod == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<PFN_XInputGetState>(GetProcAddress(mod, "XInputGetState"));
}

}   // namespace

void VigemPad::tick_slot_fix() {
    if (m_fix_phase >= 2 || !m_ready.load()) {
        return;
    }

    const auto now = GetTickCount64();

    // --- Phase 0: Fingerabdruck aufs Pad legen -----------------------------
    if (m_fix_phase == 0) {
        std::lock_guard<std::mutex> lock{m_mtx};

        if (m_client == nullptr || m_target == nullptr) {
            return;
        }

        m_fp_active = true;   // ab jetzt sendet der normale Pfad nicht mehr

        XUSB_REPORT fp{};
        XUSB_REPORT_INIT(&fp);
        fp.sThumbLX = FP_LX;
        fp.sThumbLY = FP_LY;

        vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                                 static_cast<PVIGEM_TARGET>(m_target), fp);

        m_fix_t = now;
        m_fix_phase = 1;

        return;
    }

    // --- Phase 1: Slots lesen, bis der Fingerabdruck auftaucht -------------
    if (now - m_fix_t < 200) {
        return;
    }

    auto* get_state = raw_get_state();
    int found = -1;
    int hits = 0;

    if (get_state != nullptr) {
        for (DWORD i = 0; i < 4; ++i) {
            XiState st{};

            if (get_state(i, &st) != 0) {
                continue;
            }

            if (st.pad.lx == FP_LX && st.pad.ly == FP_LY) {
                ++hits;
                found = static_cast<int>(i);
            }
        }
    }

    // Noch nicht sichtbar? Bis zu 3 s weiter versuchen (der Fingerabdruck
    // bleibt dabei liegen), danach aufgeben und alles so lassen wie es war.
    if (hits != 1 && (now - m_fix_t) < 3000) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock{m_mtx};
        m_fp_active = false;

        if (m_client != nullptr && m_target != nullptr && m_report != nullptr) {
            XUSB_REPORT_INIT(static_cast<XUSB_REPORT*>(m_report));
            vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                                     static_cast<PVIGEM_TARGET>(m_target),
                                     *static_cast<XUSB_REPORT*>(m_report));
        }
    }

    m_fix_phase = 2;

    auto f = slot_log();

    if (hits != 1) {
        if (f) {
            f << "slot-fix: KEIN eindeutiger Treffer (hits=" << hits
              << ") -- nichts geaendert" << std::endl;
        }

        return;
    }

    if (found == 0) {
        if (f) {
            f << "slot-fix: unser pad sitzt auf slot 0 -- kein Hook noetig" << std::endl;
        }

        return;
    }

    auto* target_fn = raw_get_state();

    if (target_fn == nullptr) {
        if (f) {
            f << "slot-fix: XInputGetState nicht gefunden -- nichts geaendert" << std::endl;
        }

        return;
    }

    g_our_slot.store(found, std::memory_order_relaxed);
    g_xigs_hook = safetyhook::create_inline(reinterpret_cast<void*>(target_fn),
                                            reinterpret_cast<void*>(&xigs_hooked));

    if (f) {
        f << "slot-fix: unser pad = slot " << found
          << " | hook " << (g_xigs_hook ? "AKTIV (Index 0 liest jetzt slot "
                                              + std::to_string(found) + ")"
                                        : "FEHLGESCHLAGEN -- nichts geaendert")
          << std::endl;
    }

    if (!g_xigs_hook) {
        g_our_slot.store(-1, std::memory_order_relaxed);
    }
}
