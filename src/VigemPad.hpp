// ============================================================================
// VigemPad -- virtuelles Xbox-360-Pad, nativ im Fork.
//
// [XINPUT_PAD 30.09.2026] KEIN ViGEm mehr: der Report geht an unsere
// xinput1_4.dll im Spielordner (Quelle I:\LUATRANS\re4vr_xinput), die ihn dem
// Spiel als Slot 0 liefert. Kein Treiber, kein Slot-Fix. Name und Schnittstelle
// der Klasse bleiben, damit kein Aufrufer angefasst werden muss.
//
// HERKUNFT: Diese Schicht lag bisher ausschliesslich im Plugin
// `reframework/plugins/re_vr.dll` (dort `re_vr.cpp`, Lua-Tabelle `vigem`).
// Damit war re4_vr_binding.lua nicht nach C++ portierbar: jeder Knopf-, Achsen-
// und Trigger-Schreibzugriff waere ein Lua-Aufruf gewesen, jeder mit der
// ScriptRunner-Sperre -- rund 20 pro Frame, also genau die Kosten, die die
// Portierung beseitigen soll.
//
// Die Semantik ist 1:1 aus dem Plugin uebernommen, inklusive der Achsen-Locks
// und der Reihenfolge "Wert setzen -> Locks anwenden -> Report senden".
//
// ----------------------------------------------------------------------------
// ZWEI PADS VERMEIDEN
//
// Das Plugin bleibt geladen (es macht auch OpenVR-Input, Costume Force und
// Arm-Chain-IK). Sein Pad entsteht aber erst bei `vigem.init()` aus Lua, und
// das ruft ausschliesslich re4_vr_binding.lua. Solange diese Datei noch als
// Lua laeuft, wird DIESES Pad hier NICHT initialisiert -- init() wird bewusst
// nur vom nativen binding-Port gerufen. Sobald der steht, faellt der Lua-Aufruf
// weg und es gibt weiterhin genau ein Pad.
//
// Deshalb wird hier auch KEINE Lua-Tabelle `vigem` registriert: sie wuerde je
// nach Ladereihenfolge die des Plugins verdecken oder umgekehrt.
//
// ----------------------------------------------------------------------------
// HAPTIK
//
// Der Vibrations-Callback ist mitportiert, aber nicht verdrahtet: der Fork
// macht seine Haptik laengst selbst ueber VR::trigger_haptic_vibration
// (RE4VRHolster, RE4VRRecoil). Die Rumble-Kette des Plugins (`rumble.tick`)
// ruft kein aktives Script mehr auf. Die Werte werden hier nur gespeichert und
// ueber rumble_large()/rumble_small() bereitgestellt -- falls die native
// Pad-Vibration des Spiels spaeter doch an die Controller soll.
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

class VigemPad {
public:
    static VigemPad& get();

    // [XINPUT_PAD 30.09.2026] Meldet das Pad bei unserer xinput1_4.dll an
    // (Slot 0 = VR-Pad). Mehrfachaufrufe sind unschaedlich (idempotent).
    // Liefert false, wenn die DLL im Spielordner fehlt -- dann bleibt alles
    // Weitere wirkungslos, genau wie frueher ohne ViGEmBus-Treiber.
    bool init();
    void shutdown();

    // [XINPUT_PAD] Prueft NUR, ob unsere xinput1_4.dll geladen ist
    // (frueher: ViGEmBus erreichbar). Dient der Startdiagnose.
    static bool probe();

    bool ready() const { return m_ready.load(); }

    // Namen 1:1 wie in der Lua-Fassung:
    //   Buttons: A B X Y LB RB BACK START LS RS DPAD_UP/DOWN/LEFT/RIGHT
    //   Achsen : LX LY RX RY        (-1.0 .. 1.0)
    //   Trigger: LT RT              ( 0.0 .. 1.0)
    // Ein unbekannter Name ist folgenlos (kein Fehler, kein Report).
    void set_button(std::string_view btn, bool down);
    void set_axis(std::string_view axis, double value);
    void set_trigger(std::string_view trig, double value);

    // Achsen-Lock: gesperrte Achsen werden bei JEDEM Report auf 0 gezwungen,
    // auch wenn set_axis sie vorher gesetzt hat.
    void block_axis(std::string_view axis, bool enabled);

    // [XINPUT_PAD] Bei jedem Senden mit der Vibration gerufen, die das Spiel an
    // Slot 0 geschickt hat (frueher der ViGEm-Callback) -- ueber Atomics.
    // ACHTUNG: die Parameter heissen NICHT large/small -- `small` ist ein
    // MAKRO aus windows.h (rpcndr.h: #define small char), genau wie `far`
    // und min/max. Als Parametername zerlegt es die Signatur.
    void on_rumble(uint8_t large_motor, uint8_t small_motor);

    uint8_t rumble_large() const { return m_rumble_large.load(); }
    uint8_t rumble_small() const { return m_rumble_small.load(); }

    VigemPad(const VigemPad&) = delete;
    VigemPad& operator=(const VigemPad&) = delete;

private:
    VigemPad() = default;
    ~VigemPad();

    // Erwartet, dass m_mtx gehalten wird.
    void apply_axis_locks_locked();
    void send_locked();

    std::mutex m_mtx{};
    std::atomic<bool> m_ready{false};

    // Pad-Report (Layout XINPUT_GAMEPAD), undurchsichtig, damit windows.h in
    // der .cpp bleibt.
    void* m_report{nullptr};

    bool m_lock_lx{false};
    bool m_lock_ly{false};
    bool m_lock_rx{false};
    bool m_lock_ry{false};

    std::atomic<uint8_t> m_rumble_large{0};
    std::atomic<uint8_t> m_rumble_small{0};
};
