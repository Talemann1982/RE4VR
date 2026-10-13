// ============================================================================
// VigemPad -- Implementierung. Kopfkommentar: VigemPad.hpp
// Semantik 1:1 aus re_vr.cpp (Plugin), inklusive der Lock-Reihenfolge.
//
// [XINPUT_PAD 30.09.2026] ViGEm ist raus. Der Report geht nicht mehr an ein
// virtuelles Geraet, sondern an unsere xinput1_4.dll im Spielordner
// (RE4VR_SetPad), die ihn dem Spiel als Slot 0 liefert. Alles andere --
// Namen, Masken, Wertebereiche, Achsen-Locks, "setzen -> Locks -> senden"
// bei JEDEM Aufruf -- ist unveraendert. Der Slot-Fix entfaellt ersatzlos:
// unser Pad IST Slot 0. Alte Fassung: VigemPad.cpp.bak_2026-09-30_xinput.
// ============================================================================

#include <Windows.h>

#include <cstring>

#include "VigemPad.hpp"

namespace {

// Genau das Layout von XINPUT_GAMEPAD / XUSB_REPORT (beide identisch).
struct PadReport {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

// Knopf-Bits: dieselben Werte wie XUSB_GAMEPAD_* bzw. XINPUT_GAMEPAD_*.
constexpr WORD PAD_DPAD_UP        = 0x0001;
constexpr WORD PAD_DPAD_DOWN      = 0x0002;
constexpr WORD PAD_DPAD_LEFT      = 0x0004;
constexpr WORD PAD_DPAD_RIGHT     = 0x0008;
constexpr WORD PAD_START          = 0x0010;
constexpr WORD PAD_BACK           = 0x0020;
constexpr WORD PAD_LEFT_THUMB     = 0x0040;
constexpr WORD PAD_RIGHT_THUMB    = 0x0080;
constexpr WORD PAD_LEFT_SHOULDER  = 0x0100;
constexpr WORD PAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD PAD_A              = 0x1000;
constexpr WORD PAD_B              = 0x2000;
constexpr WORD PAD_X              = 0x4000;
constexpr WORD PAD_Y              = 0x8000;

using SetPadFn = void(WINAPI*)(const PadReport*);
using ReleasePadFn = void(WINAPI*)();
using GetRumbleFn = void(WINAPI*)(WORD*, WORD*);

SetPadFn g_set_pad = nullptr;
ReleasePadFn g_release_pad = nullptr;
GetRumbleFn g_get_rumble = nullptr;

// Unsere xinput1_4.dll ist vom Spiel schon geladen (statischer Import von
// re4.exe). Die System32-Fassung hat diese Exporte nicht -> dann false.
bool find_exports() {
    auto* mod = GetModuleHandleA("xinput1_4.dll");

    if (mod == nullptr) {
        return false;
    }

    g_set_pad = reinterpret_cast<SetPadFn>(GetProcAddress(mod, "RE4VR_SetPad"));
    g_release_pad = reinterpret_cast<ReleasePadFn>(GetProcAddress(mod, "RE4VR_ReleasePad"));
    g_get_rumble = reinterpret_cast<GetRumbleFn>(GetProcAddress(mod, "RE4VR_GetRumble"));

    return g_set_pad != nullptr && g_release_pad != nullptr && g_get_rumble != nullptr;
}

// Die Namen sind die der Lua-Fassung -- sie stehen so in re4_vr_binding.lua.
WORD button_mask(std::string_view btn) {
    if (btn == "A") return PAD_A;
    if (btn == "B") return PAD_B;
    if (btn == "X") return PAD_X;
    if (btn == "Y") return PAD_Y;
    if (btn == "LB") return PAD_LEFT_SHOULDER;
    if (btn == "RB") return PAD_RIGHT_SHOULDER;
    if (btn == "BACK") return PAD_BACK;
    if (btn == "START") return PAD_START;
    if (btn == "LS") return PAD_LEFT_THUMB;
    if (btn == "RS") return PAD_RIGHT_THUMB;
    if (btn == "DPAD_UP") return PAD_DPAD_UP;
    if (btn == "DPAD_DOWN") return PAD_DPAD_DOWN;
    if (btn == "DPAD_LEFT") return PAD_DPAD_LEFT;
    if (btn == "DPAD_RIGHT") return PAD_DPAD_RIGHT;

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

// Meldet, ob unsere xinput1_4.dll geladen ist (frueher: ViGEmBus erreichbar).
bool VigemPad::probe() {
    return find_exports();
}

bool VigemPad::init() {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (m_ready.load()) {
        return true;
    }

    if (!find_exports()) {
        return false;
    }

    auto* report = new PadReport{};
    std::memset(report, 0, sizeof(PadReport));   // wie XUSB_REPORT_INIT

    m_report = report;
    m_ready.store(true);

    // Neutral melden: ab jetzt ist Slot 0 unser Pad (wie das frisch
    // angelegte ViGEm-Target).
    g_set_pad(report);

    return true;
}

void VigemPad::shutdown() {
    std::lock_guard<std::mutex> lock{m_mtx};

    const bool was_ready = m_ready.load();
    m_ready.store(false);

    // Slot 0 zurueck an die echte DLL (wie frueher vigem_target_remove).
    if (was_ready && g_release_pad != nullptr) {
        g_release_pad();
    }

    if (m_report != nullptr) {
        delete static_cast<PadReport*>(m_report);
        m_report = nullptr;
    }
}

// Gesperrte Achsen werden bei JEDEM Report auf 0 gezwungen -- auch wenn
// set_axis sie kurz zuvor gesetzt hat. Genau so haelt es das Plugin.
void VigemPad::apply_axis_locks_locked() {
    if (m_report == nullptr) {
        return;
    }

    auto* r = static_cast<PadReport*>(m_report);

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
    if (m_report == nullptr || !m_ready.load() || g_set_pad == nullptr) {
        return;
    }

    g_set_pad(static_cast<PadReport*>(m_report));

    // Rumble des Spiels abholen (frueher der ViGEm-Callback). XInput liefert
    // 0..65535, ViGEm lieferte 0..255 -> oberes Byte.
    if (g_get_rumble != nullptr) {
        WORD l = 0;
        WORD r = 0;
        g_get_rumble(&l, &r);
        on_rumble(static_cast<uint8_t>(l >> 8), static_cast<uint8_t>(r >> 8));
    }
}

void VigemPad::set_button(std::string_view btn, bool down) {
    std::lock_guard<std::mutex> lock{m_mtx};

    if (!m_ready.load() || m_report == nullptr) {
        return;
    }

    const WORD mask = button_mask(btn);

    if (mask == 0) {
        return;
    }

    auto* r = static_cast<PadReport*>(m_report);

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

    if (!m_ready.load() || m_report == nullptr) {
        return;
    }

    auto* r = static_cast<PadReport*>(m_report);
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

    if (!m_ready.load() || m_report == nullptr) {
        return;
    }

    auto* r = static_cast<PadReport*>(m_report);
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

    if (!m_ready.load() || m_report == nullptr) {
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
