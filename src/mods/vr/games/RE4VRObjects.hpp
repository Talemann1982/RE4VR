// ============================================================================
// RE4VRObjects -- 1:1-Portierung von re4_vr_objects.lua (456 Zeilen).
//
// Orte, an denen ALLE unsere Scripte wirklich AUS sein muessen.
//
// WARUM ES DIESE DATEI GIBT (gemessen 02.09.2026, teuer bezahlt): An Adas
// Gondel (Stage 60850) friert die Kabine ein, sobald unsere Scripte geladen
// sind -- mit leerem autorun-Ordner faehrt sie immer. Killswitch, STILLZONE,
// ein zentraler Waechter um die Callbacks und derselbe Waechter zusaetzlich um
// sdk.hook wurden alle durchgemessen und reichen NICHT. Der Grund: ein
// Lua-Killswitch stoppt nur, was ein Callback in Zukunft TUT -- der Datei-Rumpf
// ist beim Laden schon gelaufen, und sdk.hook setzt Trampoline PHYSISCH in die
// Engine. "Datei nicht geladen" ist etwas grundsaetzlich anderes als "Datei
// stillgestellt".
//
// FUER DIE NATIVEN MODULE: set_all_scripts_enabled kennt nur autorun\*.lua.
// Der Gegenpart ist re4vr::set_mods_gated -- s. RE4VR.hpp.
//
// Spezifikation: I:\LUATRANS\PORT_OBJECTS_SPEC.md
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "RE4VR.hpp"

class RE4VRObjects : public Mod {
public:
    static std::shared_ptr<RE4VRObjects>& get();

    std::string_view get_name() const override { return "RE4VRObjects"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;

private:
    struct Zone {
        int32_t stage;
        float x, y, z, r;
        const char* name;
    };

    static const std::array<Zone, 2> ZONEN;
    static const std::array<const char*, 4> BLEIBT;

    static bool is_our_stage(int32_t s) { return s == 60850 || s == 56100; }
    static bool is_hide_stage(int32_t s) { return s == 60850 || s == 56100; }
    // Nur Ada wartet auf die Kamera-Flanke; Leon schaltet sofort.
    static bool is_flanke_stage(int32_t s) { return s == 60850; }

    bool faehig();
    ::REManagedObject* player_ctx();
    // Rueckgabe: false = kein Context (Luas nil)
    bool lage(std::optional<int32_t>& stage, std::optional<glm::vec3>& pos, bool& am_gimmick);
    bool in_zone(int32_t stage, const glm::vec3& pos, const char*& name) const;

    bool camints();
    std::optional<int32_t> camstate();
    bool phase_b_tick(bool drin, int32_t stage);

    void invinc(bool an);

    // --------------------------------------------------------------- Zustand
    bool m_kann{false};
    bool m_kann_geprueft{false};
    bool m_gemeldet{false};

    // CamState-Enum, einmal aufgeloest.
    bool m_cam_ints_ok{false};
    int32_t m_cam_gimmick{0};
    int32_t m_cam_battle_normal{0};

    bool m_gimmick_gesehen{false};
    bool m_phase_b{false};

    // In Lua sind das Locals, die den Reset nicht ueberleben -- der Port
    // raeumt sie deshalb in on_lua_state_destroyed. Wichtig fuer 1:1: nach dem
    // Reset merkt sich das Original den von UNS gesetzten Zustand neu
    // (true/true/true) und schreibt beim Verlassen TRUE zurueck.
    std::optional<bool> m_invinc_was{};
    bool m_hp_was_valid{false};
    bool m_hp_was_invincible{false};
    bool m_hp_was_immortal{false};
    bool m_hp_was_nodamage{false};
    bool m_invinc_logged{false};

    bool m_reset_laeuft{false};
    double m_reset_t{-99.0};
    double m_rueckweg_t{-99.0};

    // [TOT] geladen_um und KARENZ = 6.0 sind im Original definiert und werden
    // NIRGENDS benutzt (maschinell geprueft). Der Kommentar darueber beschreibt
    // eine Schutzfrist, die es im Code nicht gibt. 1:1 als toter Code
    // uebernommen -- NICHT "nachruesten".
    double m_geladen_um{0.0};
    static constexpr double KARENZ = 6.0;

    static constexpr double RESET_TIMEOUT = 3.0;
};

#endif // RE4
