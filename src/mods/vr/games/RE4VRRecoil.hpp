// ============================================================================
// RE4VRRecoil -- 1:1-Portierung von re4_vr_recoil.lua (783 Zeilen).
//
// ZWEI MODULE IN EINER DATEI:
//  1. Recoil -- hookt chainsaw.PlayerCameraController und unterdrueckt den
//     nativen Kamera-Rueckstoss vollstaendig; an seiner Stelle laeuft eine
//     eigene Attack-Ramp plus Feder-Daempfer-Simulation, deren Ergebnis als
//     _G.vr_recoil exportiert wird (einziger Leser: re4_vr_motion.lua).
//  2. Schuss-Haptik -- urspruenglich re4_vr_haptic.lua, im Lua in eine IIFE
//     gekapselt. Ein Puls pro Schuss, getriggert ueber _G.__vr_shot_seq aus
//     crosshair. Laeuft UNABHAENGIG von enable_recoil.
//
// Spezifikation: I:\LUATRANS\PORT_RECOIL_SPEC.md (Fassung 2)
// ============================================================================

#pragma once

#if defined(RE4)

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"

class RE4VRRecoil : public Mod {
public:
    static std::shared_ptr<RE4VRRecoil>& get();

    std::string_view get_name() const override { return "RE4VRRecoil"; }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Aus den Hooks gerufen.
    bool hook_pre_request_recoil();     // true = SKIP_ORIGINAL
    bool hook_pre_update_recoil();
    bool hook_pre_update_handshake();

    // Vom nach Lua exportierten Public-Zeichner gerufen.
    void draw_public_recoil_level();

private:
    // ------------------------------------------------------------- Konfiguration
    // Nur diese vier Posten werden geladen und gespeichert (CONFIG_KEYS + die
    // beiden Override-Tabellen). Alles andere sind FESTE Code-Konstanten und
    // steht bewusst nicht in der JSON -- alte Eintraege werden ignoriert.
    struct Cfg {
        bool enable_recoil{false};
        double recoil_intensity_multiplier{1.0};
    };

    // [UI-DIAET] Feste Werte. Zum Nachjustieren hier aendern; die Rechnung
    // bleibt unveraendert.
    static constexpr double RECOIL_POSITION_INTENSITY = 0.008;
    static constexpr double RECOIL_ROTATION_INTENSITY = 0.055;
    static constexpr double RECOIL_HORIZONTAL_SPREAD = 0.024;
    static constexpr double RECOIL_VERTICAL_SPREAD = 0.016;
    static constexpr double RECOIL_RANDOMNESS = 0.35;
    static constexpr double RECOIL_MULT_EXPONENT = 0.35;
    static constexpr double RECOIL_STACK_CAP = 2.0;
    static constexpr double RECOIL_SPRING_STIFFNESS = 120.0;
    static constexpr double RECOIL_SPRING_DAMPING = 18.0;
    static constexpr double RECOIL_ATTACK_DURATION = 0.022;
    static constexpr double RECOIL_AUTO_SCALE = 0.15;
    static constexpr double RECOIL_SUSTAINED_DAMPING = 28.0;
    static constexpr double RECOIL_SUSTAINED_WINDOW = 0.12;
    static constexpr double RECOIL_SUPPORTED_IMPULSE_MULT = 0.70;
    static constexpr double RECOIL_UNSUPPORTED_LIGHT_MULT = 1.18;
    static constexpr double RECOIL_UNSUPPORTED_HEAVY_MULT = 1.90;

    struct HapticCfg {
        bool enabled{true};
        double amp_solo{1.00};
        double amp_right_sup{0.70};
        double amp_left_sup{0.30};
        double dur{0.05};
        double freq{180.0};
        double delay{0.00};
    };

    void load_config();
    void save_config();
    bool apply_config_table(const nlohmann::json& d);   // Rueckgabe: "found"
    void load_haptic_config();
    void save_haptic_config();

    // ------------------------------------------------------------- Waffen
    static const std::unordered_map<int32_t, double> WEAPON_RECOIL_MULTIPLIERS;   // 42
    static const std::unordered_map<int32_t, bool> WEAPON_AUTO_FLAGS;             // 5

    void build_weapon_id_catalog_sorted();
    std::optional<int32_t> get_current_weapon_id();
    static std::string weapon_key_from_id(int32_t wid);
    // liefert eff, base, per, global, key
    double get_effective_weapon_recoil_multiplier(double& base, double& per, double& global,
                                                  std::string& key);

    // ------------------------------------------------------------- Tore
    bool fp_active() const;
    bool hmd_active() const;

    // ------------------------------------------------------------- Simulation
    void update_spring_and_export();

    // ------------------------------------------------------------- Haptik
    void haptic_tick();
    void haptic_fire(bool support);
    void haptic_pulse(uint64_t handle, double amp);

    // ------------------------------------------------------------- UI
    void ensure_public_ui_registered();
    bool m_public_ui_registered{false};
    bool m_dispatcher_present{false};

    // ------------------------------------------------------------- Zustand
    Cfg m_cfg{};
    HapticCfg m_haptic{};

    std::unordered_map<std::string, double> m_weapon_intensity_overrides{};
    // [SUPPORT_PER_WEAPON] Pro Waffe: auf wie viel faellt der Recoil, sobald
    // die linke (Stuetz-)Hand an der Waffe ist. Kein Eintrag -> globaler Default.
    std::unordered_map<std::string, double> m_weapon_support_overrides{};

    std::vector<int32_t> m_weapon_id_catalog_sorted{};
    bool m_catalog_built{false};

    // Lua: state = { ... }
    struct State {
        bool recoil_attack_active{false};
        double recoil_attack_t{0.0};
        double recoil_attack_pos_y{0.0};
        double recoil_attack_pos_z{0.0};
        double recoil_attack_pitch{0.0};
        double recoil_attack_yaw{0.0};

        bool has_last_t{false};       // Lua: recoil_last_t == nil
        double recoil_last_t{0.0};
        bool has_last_shot_t{false};  // wird NIE zurueckgesetzt, s. Spec 13.6
        double recoil_last_shot_t{0.0};

        bool recoil_active{false};

        double spring_pos_y{0.0}, spring_pos_z{0.0};
        double spring_pitch{0.0}, spring_yaw{0.0};
        double spring_vel_y{0.0}, spring_vel_z{0.0};
        double spring_vel_pitch{0.0}, spring_vel_yaw{0.0};
    };

    State m_state{};

    // Haptik
    struct Pending {
        double at{0.0};
        bool support{false};
    };

    std::vector<Pending> m_pending{};
    bool m_have_last_seq{false};
    double m_last_seq{0.0};
};

#endif // RE4
