// ============================================================================
// RE4VRCapacitive -- 1:1-Portierung von re4_vr_capacitive.lua (95 Zeilen).
// GRIP-EMPFINDLICHKEIT, NUR OpenXR.
//
// Unter OpenVR liest das mitgelieferte Index-Profil den Griff als force_sensor
// und schaltet bei 0.30/0.25 -- das ist dort schon richtig und darf NICHT
// angefasst werden. Unter OpenXR hing der Griff an squeeze/value, dem
// KAPAZITIVEN Wert ("wie geschlossen ist die Hand"); der steht schon beim
// blossen Halten ueber 0.30, der Griff loeste bei der kleinsten Bewegung aus.
//
// RUNTIME-SPERRE: set_grip_settings wird AUSSCHLIESSLICH unter OpenXR gerufen.
// Vorher hat der 2-Sekunden-Takt die Einstellung auch unter OpenVR
// ueberschrieben -- genau das war der Fehler.
//
// Spezifikation: I:\LUATRANS\PORT_EQUIPLOCK_CAPACITIVE_SPEC.md (Teil B)
// ============================================================================

#pragma once

#if defined(RE4)

#include <optional>
#include <string>

#include "RE4VR.hpp"

class RE4VRCapacitive : public Mod {
public:
    static std::shared_ptr<RE4VRCapacitive>& get();

    std::string_view get_name() const override { return "RE4VRCapacitive"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;

    // [UI 2026-09-09] Der Regler sitzt im Binding-Tree ("Capacitive"), nicht in
    // einem eigenen Baum. Die Werte gehoeren aber weiter DIESEM Modul: es
    // schiebt sie alle zwei Sekunden nach, weil REFramework seine eigene Config
    // beim Neuladen sonst wieder ueberschreibt. Wer sie von Hand im VR-Tree des
    // Frameworks verstellt, verliert die Aenderung deshalb nach dem naechsten
    // Takt -- ueber diese Setter bleibt sie erhalten (JSON + sofortiges apply).
    double get_press() const { return m_cfg.press; }
    double get_release() const { return m_cfg.release; }
    bool get_use_analog() const { return m_cfg.use_analog; }
    bool get_prefer_force() const { return m_cfg.prefer_force; }

    void set_cfg(bool use_analog, bool prefer_force, double press, double release);

private:
    // Defaults = die Werte, die das OpenVR-Index-Profil seit jeher benutzt.
    struct Cfg {
        bool use_analog{true};      // eigene Schwelle statt der Runtime-Entscheidung
        bool prefer_force{true};    // Kraftsensor vor kapazitivem Wert (Index)
        double press{0.30};         // ab hier gilt der Griff als zu
        double release{0.25};       // darunter wieder als offen (Hysterese)
    };

    void load_cfg();
    void save_cfg();
    void sane();
    bool is_openxr() const;
    void apply();

    Cfg m_cfg{};
    double m_next_push{0.0};
};

#endif // RE4
