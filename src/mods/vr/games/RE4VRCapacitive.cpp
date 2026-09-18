// ============================================================================
// RE4VRCapacitive -- 1:1-Portierung von re4_vr_capacitive.lua
// Spezifikation: I:\LUATRANS\PORT_EQUIPLOCK_CAPACITIVE_SPEC.md (Teil B)
// ============================================================================

#if defined(RE4)

#include <ctime>

#include "../../VR.hpp"
#include "RE4VR.hpp"
#include "RE4VRCapacitive.hpp"

namespace {

constexpr const char* JSON_PATH = "re4_vr/re4_vr_capacitive.json";

double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

double clamp_val(double v, double lo, double hi) {
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

} // namespace

std::shared_ptr<RE4VRCapacitive>& RE4VRCapacitive::get() {
    static auto inst = std::make_shared<RE4VRCapacitive>();
    return inst;
}

void RE4VRCapacitive::load_cfg() {
    m_cfg = Cfg{};

    try {
        const auto d = re4vr::json_load(JSON_PATH);

        if (!d.is_object()) {
            return;
        }

        // Die beiden Booleans werden im Original mit `~= nil` geprueft und dann
        // ueber `and true or false` normalisiert -- jeder Wert ausser false/nil
        // wird true.
        if (d.contains("use_analog") && !d["use_analog"].is_null()) {
            m_cfg.use_analog = !(d["use_analog"].is_boolean() && !d["use_analog"].get<bool>());
        }

        if (d.contains("prefer_force") && !d["prefer_force"].is_null()) {
            m_cfg.prefer_force =
                !(d["prefer_force"].is_boolean() && !d["prefer_force"].get<bool>());
        }

        // Die Zahlen nur bei type(...) == "number".
        if (d.contains("press") && d["press"].is_number()) {
            m_cfg.press = d["press"].get<double>();
        }

        if (d.contains("release") && d["release"].is_number()) {
            m_cfg.release = d["release"].get<double>();
        }
    } catch (...) {
        m_cfg = Cfg{};
    }
}

void RE4VRCapacitive::save_cfg() {
    nlohmann::json d{};
    d["use_analog"] = m_cfg.use_analog;
    d["prefer_force"] = m_cfg.prefer_force;
    d["press"] = m_cfg.press;
    d["release"] = m_cfg.release;

    re4vr::json_save(JSON_PATH, d);
}

// Aus dem Binding-Tree gerufen: setzen, geraderuecken, sofort scharf schalten
// und in die JSON schreiben -- sonst holt der 2-Sekunden-Takt den alten Wert
// zurueck bzw. der naechste Start faengt wieder bei den Defaults an.
void RE4VRCapacitive::set_cfg(bool use_analog, bool prefer_force, double press,
                              double release) {
    m_cfg.use_analog = use_analog;
    m_cfg.prefer_force = prefer_force;
    m_cfg.press = press;
    m_cfg.release = release;

    sane();
    apply();
    save_cfg();
}

void RE4VRCapacitive::sane() {
    // Ein Loslassen-Punkt OBERHALB des Greif-Punktes wuerde den Griff fuer
    // immer festhalten.
    m_cfg.press = clamp_val(m_cfg.press, 0.05, 0.95);
    m_cfg.release = clamp_val(m_cfg.release, 0.05, 0.95);

    if (m_cfg.release > m_cfg.press) {
        m_cfg.release = m_cfg.press;
    }
}

bool RE4VRCapacitive::is_openxr() const {
    // Solange die Runtime noch nicht steht (VR nicht initialisiert), ist das
    // Ergebnis false -- dann wird schlicht nichts gesetzt und beim naechsten
    // Takt neu gefragt.
    try {
        return VR::get()->is_openxr_loaded();
    } catch (...) {
        return false;
    }
}

void RE4VRCapacitive::apply() {
    // [RUNTIME-SPERRE] OpenVR / unbekannt: Finger weg. Dort liest das
    // Index-Profil den Kraftsensor bereits richtig; der 2-Sekunden-Takt hat
    // diese Einstellung frueher ueberschrieben -- genau das war der Fehler.
    // Die Runtime wird bei JEDEM Takt neu geprueft, damit ein VR-Neustart mit
    // gewechselter Runtime sofort richtig liegt.
    if (!is_openxr()) {
        return;
    }

    try {
        VR::get()->set_grip_settings(m_cfg.use_analog, m_cfg.prefer_force,
                                     static_cast<float>(m_cfg.press),
                                     static_cast<float>(m_cfg.release));
    } catch (...) {
    }
}

std::optional<std::string> RE4VRCapacitive::on_initialize() {
    load_cfg();
    sane();
    apply();   // einmal beim Laden
    return Mod::on_initialize();
}

void RE4VRCapacitive::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRCapacitive", "on_lua_state_destroyed");
    // [SCRIPTGATE] Loest die Gondel-Abschaltung diesen Reset aus, ist
    // capacitive.lua im Original abgeschaltet -- ihr Rumpf laeuft dort NICHT.
    if (re4vr::mods_gated()) {
        return;
    }

    // Ein "Reset Scripts" wuerde das Original neu ausfuehren: Config neu lesen,
    // sane(), einmal apply(), Takt von vorn.
    load_cfg();
    sane();
    m_next_push = 0.0;
    apply();
}

void RE4VRCapacitive::on_frame() {
    re4vr::trace("RE4VRCapacitive", "on_frame");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    // Das Framework laedt seine eigene Config auch nach einem VR-Neustart
    // (Runtime-Wechsel, Save-Load der Session) -- dann stuenden dort wieder die
    // Fork-Defaults. Deshalb wird der Satz regelmaessig nachgeschoben; ein
    // Aufruf alle zwei Sekunden kostet nichts.
    const double t = now_clock();

    if (t < m_next_push) {
        return;
    }

    m_next_push = t + 2.0;
    apply();
}

#endif // RE4
