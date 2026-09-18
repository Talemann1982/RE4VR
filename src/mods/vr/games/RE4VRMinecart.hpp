// ============================================================================
// RE4VRMinecart -- 1:1-Portierung von re4_vr_minecart.lua (663 Zeilen).
//
// Minecart/RailCar-spezifische VR-Fixes. Laeuft NUR im railcar_mode (Killswitch
// AN, aber motion + arm_chain + firstperson bewusst weiter). Hier steht alles,
// was auf dem Schienenwagen anders sein muss, OHNE im movement-Script zu
// wuehlen.
//
// Fuenf Bausteine (die Nummerierung ist die des Originals):
// (1) __vr_anim_l0-Export -> arm_chain nutzt den Layer0-Anim-Namen fuer seinen
//     Schulter-Capture-Slot.
// (2) Crosshair-Force -> die Cart-Gun (4005) setzt is_aim/IsReticleDisp nie.
// (3) Spine-/Torso-Pin -> stabile Oberkoerper-Basis, damit arm_chains
//     Schulter-Reach-Follow nicht von der nativen AutoMove-Pose rechnet.
// (4) Kipp-Ausgleich per Kopfneigung, Auto-Recenter, Yaw-Follow.
// (5) Zug-Rumble.
//
// Spezifikation: I:\LUATRANS\PORT_MINECART_SPEC.md
// **Es gilt der NACHTRAG ab Zeile 1279, nicht der Rumpf davor.**
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- nicht verschieben (belegt, nicht geraten):
//
//   ... RE4VRFirstPerson -> **RE4VRMinecart** -> RE4VRArmChain -> ... -> RE4VRCrosshair ...
//
// * **vor RE4VRArmChain**: dessen phase_entry() ruft den Spine-Pin VOR seiner
//   2-Bone-IK. Steht Minecart dahinter, landet der Pin nach der IK -- genau der
//   Fehler, gegen den der [FLICKER-FIX] im Original geschrieben wurde. Ebenso
//   liest ArmChain __vr_anim_l0 und __re4_railcar_reloading.
// * **vor RE4VRCrosshair**: Crosshair schreibt is_aim ausschliesslich in
//   on_pre_lock_scene und ueberschreibt unseren LockScene-Write dort sofort
//   wieder. Genau dieser Zustand ist getestet -- wirksam ist im Original nur
//   der LateUpdateBehavior-Write. Stuende Minecart dahinter, gewaenne ploetzlich
//   der LockScene-Write: eine neue, ungetestete Lage.
// ----------------------------------------------------------------------------
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"

class RE4VRMinecart : public Mod {
public:
    static std::shared_ptr<RE4VRMinecart>& get();

    std::string_view get_name() const override { return "RE4VRMinecart"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // [SPINE-PIN] Oeffentlich, weil RE4VRArmChain::railcar_pin_spine() das hier
    // direkt ruft. Im Lua lief das ueber das Global
    // _G.__re4_minecart_apply_spine_pin; ArmChain war dessen EINZIGER Aufrufer
    // (Live-Set und C++-Baum durchsucht), deshalb ist der native Direktaufruf
    // die exakte Entsprechung und kein Lua-Export mehr noetig.
    void apply_spine_pin();

private:
    // ------------------------------------------------------------------
    // Config -- 24 Schluessel werden gespeichert, aber nur 11 wieder geladen.
    // Das ist ein FEHLER DES ORIGINALS (Z.73 vs. Z.74-90) und wandert 1:1 mit:
    // yaw_follow und alle zwoelf rumble_* stehen nach jedem Start wieder auf
    // ihrem Compile-Default, egal was in der JSON steht.
    // ------------------------------------------------------------------
    struct Cfg {
        float body_down{0.0f};        // [SITZEN] Oberkoerper absenken (m)
        float body_back{0.0f};        // [ZURUECK] Oberkoerper in Hip-Local-Z schieben (m)
        bool  yaw_follow{false};      // deprecated: verursachte Stick-Yaw-Anschlag -- NIE geladen
        float yaw_sign{1.0f};
        bool  lean_enabled{true};
        float lean_threshold{0.10f};  // sin(Roll) ~ 6 Grad
        float lean_full{0.28f};       // sin(Roll) ~ 16 Grad
        float lean_sign{1.0f};
        float lean_tilt_min{0.15f};
        bool  recenter_enabled{true};
        float recenter_dead{0.08f};   // m
        float recenter_rate{0.35f};   // m/s
        // --- ab hier: gespeichert, aber NIE geladen ---
        bool  rumble_enabled{true};
        float rumble_amp{0.22f};
        float rumble_rate{0.09f};
        float rumble_dur{0.07f};
        float rumble_freq{45.0f};
        bool  rumble_clack{true};
        float rumble_clack_every{1.7f};
        float rumble_clack_amp{0.50f};
        bool  rumble_intro{true};
        float rumble_speed_min{3.0f};
        float rumble_speed_full{9.0f};
        bool  rumble_speed_scale{true};
    };

    // Engine-Handle, das ueber Frames gehalten wird. Rohzeiger reichen NICHT:
    // sol_lua_push macht in Lua add_ref unsichtbar mit, der Port muss das von
    // Hand tun, sonst zeigt der Cache nach einem Levelwechsel ins Leere.
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    void load_cfg();
    void save_cfg();

    bool railcar_active() const;

    void ensure_t_motion();

    // (1) Layer0-Anim-Name exportieren
    void update_anim_export();
    // [RAILCAR_RELOAD] native Reload-Anim an der WAFFE erkennen
    void update_reload_flag();
    // (2) Crosshair-Force
    void force_crosshair();
    // (3) Spine-Pin
    bool load_pin_pose();
    bool resolve_spine(::REManagedObject* tf);
    // [YAW-FOLLOW]
    void update_yaw_follow();
    // [MINECART KNIFE]
    void cart_knife_swap();
    // [KIPP-AUSGLEICH]
    void update_cart_lean();
    // [AUTO-RECENTER]
    void update_cart_recenter();
    // [ZUG-RUMBLE]
    bool  cart_ride_now() const;
    float cart_speed_now();
    void  update_cart_rumble();

    ::REManagedObject* get_player_railcar();
    // Ersatz fuer re4.weapon_gameobject (Feld der Lua-Modultabelle utility/RE4,
    // von C++ nicht erreichbar): dieselbe Kette nativ nachgebaut.
    ::REManagedObject* weapon_game_object();

    void store(Handle& h, ::REManagedObject* o);
    void drop(Handle& h);

    Cfg m_cfg{};
    bool m_cfg_loaded{false};

    // --- (1) Anim-Export ---
    Handle m_bw_go{};
    Handle m_bw_motion{};

    // --- Reload-Erkennung an der Waffe ---
    Handle m_wep_go{};
    Handle m_wep_motion{};

    // --- (3) Spine-Pin ---
    // pin_map wird in Lua EINMAL geladen und nie invalidiert (Z.173) -- 1:1.
    struct PinRel {
        glm::vec3 p{};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::unordered_map<std::string, PinRel> m_pin_map{};
    bool m_pin_map_loaded{false};
    bool m_pin_map_valid{false};

    Handle m_pin_tf{};
    std::vector<Handle> m_pin_joints{};
    std::vector<std::string> m_pin_names{};

    // --- [YAW-FOLLOW] ---
    std::optional<float> m_yf_entry_body{};
    std::optional<float> m_yf_entry_hmd{};

    // --- [MINECART KNIFE] ---
    // Bewusst 0.0 und NICHT "nicht gesetzt": der erste Aufruf soll sofort
    // feuern (Original Z.301 `local _ck_last = 0`).
    double m_ck_last{0.0};

    // --- [KIPP-AUSGLEICH] Live-Anzeige der UI ---
    struct LeanDbg {
        bool  active{false};
        float tilt{0.0f};
        float roll{0.0f};
        float out{0.0f};
    } m_lean_dbg{};

    // --- [AUTO-RECENTER] ---
    // nil-Sentinel des Originals: os.clock() kann 0.0 liefern, deshalb optional
    // und nicht 0.0 als "nicht gesetzt".
    std::optional<double> m_rc_last_t{};
    struct RcDbg {
        float off{0.0f};
        float moved{0.0f};
    } m_rc_dbg{};

    // --- [ZUG-RUMBLE] ---
    std::optional<double> m_rum_t0{};
    double m_rum_next_t{0.0};
    double m_rum_next_clack{0.0};
    int    m_rum_side{0};
    struct RumDbg {
        bool  on{false};
        float amp{0.0f};
    } m_rum_dbg{};

    // Tempo-Messung
    std::optional<double> m_spd_t{};
    glm::vec3 m_spd_p{};
    float m_spd_v{0.0f};
    std::string m_spd_src{"-"};

    Handle m_rail_manager{};
    // sdk.typeof("via.motion.Motion") -- im Original ZWEIMAL deklariert
    // (Z.112 und Z.136); die zweite Deklaration verdeckt die erste, beide
    // liefern denselben Typ. Hier genuegt einer.
    ::REManagedObject* m_t_motion{nullptr};
};

#endif // RE4
