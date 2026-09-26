// =====================================================================
// RE4VRFirstPerson -- 1:1-Portierung von reframework/autorun/re4_vr_firstperson.lua
// (1901 Zeilen). Zeilenangaben "Lua Z.xxx" beziehen sich auf diese Datei.
// Vollstaendige Spezifikation: I:\LUATRANS\PORT_FIRSTPERSON_SPEC.md.
//
// WAS DAS MODUL TUT: klemmt die Spielkamera an den Head-Joint (NUR Position,
// die Rotation macht REFramework), glaettet gegen Kopf-Bob und Kapsel-Puls,
// kennt rund zwanzig Sonderzustaende mit eigenem Offset, blendet beim
// Event-Ende zurueck, exportiert die Kamera-Basis fuer re4_vr_motion.lua,
// uebersteuert die Stick-Bewegung Richtung Blickrichtung, bricht erzwungene
// Kameraschwenks ab, neutralisiert den Headset-Yaw bei Killswitch-Eintritt
// und blendet den pinken StreamingDummy-Cube aus.
//
// WARUM ES TEUER WAR: compute_and_set() und die SIEBEN Event-Offset-Funktionen
// laufen in VIER Phasen pro Frame, und in der ausgelieferten Config sind alle
// sieben Offsets gesetzt -- die Gate-Ketten laufen also wirklich alle.
// Der Kommentar in der Quelle beziffert allein das mit 0,395 ms/Frame.
//
// REIHENFOLGE: re4_vr_firstperson.lua liegt alphabetisch MITTEN im Lua-Strom
// (nach binding/capacitive/choke/crosshair/equip_lock, vor guestures/holster/
// motion/movement/...). Ueber den Mod-Vektor ist das nicht abbildbar -- der
// ScriptRunner ist EIN Mod. Deshalb laeuft dieses Modul ueber den
// [PHASENSTUB] des Traegers: ein winziges Lua-Script mit dem Originalnamen
// ruft __re4_native_phase(20..25) und trifft die Position exakt.
//
// UMSCHALTER: _G.__re4_firstperson_native = false -> dieses Modul haelt still.
// =====================================================================
#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "RE4VR.hpp"

// Builtin-Port von reframework/autorun/re4_vr_firstperson.lua.
// Eigener Mod, in Mods.cpp HINTER dem ScriptRunner registriert.
class RE4VRFirstPerson : public Mod {
public:
    static std::shared_ptr<RE4VRFirstPerson>& get();

    std::string_view get_name() const override { return "RE4VRFirstPerson"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;
    void on_frame() override;                  // Lua Z.465 UND Z.1609
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

private:
    // ---- Einstiegspunkte (Lua Z.1525-1550) ----------------------------
    void on_pre_lock_scene();            // Lua Z.1525
    void on_pre_unlock_scene();          // Lua Z.1545
    void on_late_update_behavior();      // Lua Z.1546
    void on_begin_rendering();           // Lua Z.1547
    void on_update_motion();             // Lua Z.1550
    void on_script_reset();

    // =================================================================
    // Konfiguration -- 67 Schluessel, flach, wie die JSON.
    // Defaults exakt wie Lua Z.22-133.
    // =================================================================
    struct Config {
        float off_x{0.0f}, off_y{0.0f}, off_z{0.0f};

        float event_off_x{0.0f}, event_off_y{0.0f}, event_off_z{0.0f};
        float event2_off_x{0.0f}, event2_off_y{0.0f}, event2_off_z{0.0f};
        float event3_off_x{0.0f}, event3_off_y{0.0f}, event3_off_z{0.0f};
        float event4_off_x{0.0f}, event4_off_y{0.0f}, event4_off_z{0.0f};
        bool  event5_mono{true};
        float event5_off_x{0.0f}, event5_off_y{0.0f}, event5_off_z{0.0f};
        float event6_off_x{0.0f}, event6_off_y{0.0f}, event6_off_z{0.0f};
        float turret_off_x{0.0f}, turret_off_y{0.0f}, turret_off_z{0.0f};

        float jetski_off_x{0.0f}, jetski_off_y{0.0f}, jetski_off_z{0.0f};
        float boat_off_x{0.0f}, boat_off_y{0.0f}, boat_off_z{0.0f};
        float begcrouch_off_x{0.0f}, begcrouch_off_y{0.0f}, begcrouch_off_z{0.0f};

        // Lua Z.70-72 / Z.86-88: die beiden einzigen Defaults != 0.
        float ada_fc_off_x{0.0f}, ada_fc_off_y{0.0f}, ada_fc_off_z{-0.08f};
        float leon_fc_off_x{0.0f}, leon_fc_off_y{0.0f}, leon_fc_off_z{0.0f};
        float ada_box_off_x{0.0f}, ada_box_off_y{0.0f}, ada_box_off_z{0.0f};
        float leon_evt_off_x{0.0f}, leon_evt_off_y{0.0f}, leon_evt_off_z{-0.08f};

        float acrouch_off_x{0.0f}, acrouch_off_y{0.0f}, acrouch_off_z{0.0f};
        float cart_off_x{0.0f}, cart_off_y{0.0f}, cart_off_z{0.0f};

        float headpin_fade_dur{0.25f};
        bool  movement_stabilization{true};
        bool  movement_follows_hmd{true};
        float bob_tau{0.5f};
        float surge_tau{0.35f};
        bool  crouch_cam_lerp{true};
        float crouch_cam_tau{0.18f};
        bool  standup_cam_track{true};
        bool  hide_streaming_dummy{true};
        bool  recenter_on_killswitch{true};

        float bino_start{2.5f};
        float bino_min{-7.0f};
        float bino_max{2.5f};
        float bino_speed{3.0f};

        bool  block_force_twirler{true};
    };

    // Lua Z.535: Bob-Restfilter (EMA auf Head - Kapsel).
    struct BobFilter {
        std::optional<glm::vec3> ema{};
        std::optional<double> last_t{};
    };

    // Lua Z.546: Kapsel-Y-Glaettung, feste Zeitkonstante.
    struct Capy {
        std::optional<float> ema{};
        std::optional<double> last_t{};
    };

    // Lua Z.608: Dead-Reckoning gegen den Kapsel-Tempo-Puls.
    struct Surge {
        std::optional<float> px{}, pz{};
        float vx{0.0f}, vz{0.0f};
        std::optional<float> lx{}, lz{};
        std::optional<double> last_t{};
        std::optional<float> lyaw{};
        int turn_n{0};
    };

    // Lua Z.539: Crouch-Absacken + Aufsteh-Fenster.
    struct CrouchCam {
        std::optional<float> ema{};
        bool was{false};
        std::optional<double> until_t{};
        std::optional<double> last_t{};
        std::optional<double> standup_until{};
    };

    // Lua Z.816: Ausblenden des Head-Nagels.
    struct HeadPinFade {
        bool was{false};
        double t{0.0};
        float ax{0.0f}, ay{0.0f}, az{0.0f};
    };

    // Lua Z.1133: Stick-Movement-Override.
    struct MoveState {
        bool has_valid_position{false};
        std::optional<glm::vec3> last_player_position{};
        std::optional<double> last_time{};
    };

    // Lua Z.422: ForceTwirler-Anzeige.
    struct Ftw {
        bool twirl{false};
        bool control{true};
        int stopped{0};
    };

    // ---- Konfiguration (Lua Z.140-213) --------------------------------
    void publish_bino();
    void save_cfg();
    void load_cfg();
    void reset_runtime_state();

    // ---- Getter (Lua Z.220-336) ---------------------------------------
    ::REManagedObject* get_player_ctx();
    int32_t fp_stage_cached();
    ::REManagedObject* fp_busy_cached();
    bool is_ada_now();
    bool is_ashley_now();
    ::REManagedObject* get_body_transform();
    ::REJoint* get_camera_joint();
    ::REJoint* get_head_joint();
    bool is_joint_valid(::REJoint* j);
    void store_head_joint(::REJoint* j);

    // [BODY-EPOCH 2026-09-22] Body gewechselt (Save-Load/Tod) -> m_head_joint
    // loslassen (store_head_joint(nullptr): nur release); get_head_joint holt neu.
    uint64_t m_body_epoch{0};
    void drop_body_caches() { store_head_joint(nullptr); }

    // ---- Gates (Lua Z.347-361, 736-810) -------------------------------
    bool active();
    bool is_crouch_now();
    bool on_ladder_climb();
    bool is_gimmick_ks3_now();

    // ---- Kamera-Basis (Lua Z.387-409, 516-527) ------------------------
    std::optional<glm::quat> get_game_cam_yaw();
    std::optional<glm::quat> get_primary_cam_flat_yaw();

    // ---- Filter (Lua Z.549-788) ---------------------------------------
    glm::vec3 apply_bob_filter(glm::vec3 hp, bool frame_tick);
    glm::vec3 apply_surge_filter(glm::vec3 hp, const std::optional<glm::vec3>& bp, bool frame_tick);
    float apply_crouch_cam_lerp(float y, bool frame_tick);

    // ---- Kern (Lua Z.819-1116) ----------------------------------------
    void compute_and_set(bool frame_tick);

    // ---- Movement (Lua Z.1121-1224) -----------------------------------
    glm::vec2 get_left_input_axis();
    void apply_movement_stabilization();

    // ---- Recenter (Lua Z.1235-1270) -----------------------------------
    bool recenter_neutralize_headset();
    void recenter_tick();

    // ---- Event-Offsets (Lua Z.1280-1521) ------------------------------
    // Die sechs Riddles sind im Original bewusst dupliziert; hier eine
    // gemeinsame Rumpffunktion mit denselben Parametern -- die Gate-Logik
    // ist Zeichen fuer Zeichen dieselbe, nur die Zahlen wechseln.
    void apply_riddle_offset(float ox, float oy, float oz,
                             int32_t stage_a, int32_t stage_b,
                             float px, float py, float pz, float radius_sq);
    void apply_event_hmd_offset();
    void apply_event2_hmd_offset();
    void apply_event3_hmd_offset();
    void apply_event4_hmd_offset();
    void apply_event5_hmd_offset();
    void apply_event6_hmd_offset();
    void apply_event5_mono();
    void apply_turret_hmd_offset();
    void apply_all_event_offsets();

    // ---- ForceTwirler + StreamingDummy (Lua Z.465, 1558-1631) ---------
    ::REManagedObject* get_player_cam();
    void force_twirler_tick();
    ::REManagedObject* sd_get_scene();
    void sd_hide_subtree(::REManagedObject* tf);
    void sd_scan();
    void sd_tick();
    void sd_clear_cache();

    // =================================================================
    // Zustand
    // =================================================================
    Config m_cfg{};

    BobFilter m_bob{};
    Capy m_capy{};
    Surge m_surge{};
    CrouchCam m_crouch_cam{};
    HeadPinFade m_headpin_fade{};
    MoveState m_move{};
    Ftw m_ftw{};

    // Lua Z.317: Head-Joint-Cache. [REF] wie in RE4VRArmChain: gecachte
    // Engine-Objekte brauchen add_ref, sonst Access Violation nach einem
    // Level-/Charakterwechsel.
    ::REJoint* m_head_joint{nullptr};
    // [FP-MESSUNG 04.09.] Je Grund+Ausgang nur EINE Zeile -- active() laeuft
    // pro Frame. Raus, sobald der 3rd-Person-Befund geklaert ist.
    std::unordered_set<std::string> m_fp_diag_seen{};

    bool m_head_joint_reffed{false};

    // Lua Z.371 -- der LOKALE Singleton-Cache. Siehe [QUIRK 1]: die Lua-Fassung
    // hat daneben noch einen GLOBALEN, den fp_busy_cached benutzt.
    ::REManagedObject* m_camera_system{nullptr};
    ::REManagedObject* m_camera_system_global{nullptr};

    // Lua Z.248-250: Perf-Caches, Frame-Grenze aus dem ersten on_frame.
    uint64_t m_fp_frame{0};
    int32_t m_fp_stage{-1};
    bool m_fp_stage_valid{false};
    uint64_t m_fp_stage_f{UINT64_MAX};
    ::REManagedObject* m_fp_busy{nullptr};
    uint64_t m_fp_busy_f{UINT64_MAX};

    // Lua Z.426, 462-463: ForceTwirler-Backoff.
    bool m_ftw_ui_open{false};
    double m_ftw_block_until{0.0};
    int m_ftw_fail_streak{0};

    // Lua Z.1255: Recenter-Flanke.
    bool m_rc_was_active{false};

    // Lua Z.1558: StreamingDummy-Hider.
    std::vector<::REManagedObject*> m_sd_cache{};
    // [M2] Lua legt JEDE Komponente in die Tabelle -- auch die mit
    // referenceCount == 0. Gezaehlt wird nur, was sich zaehlen laesst.
    std::vector<char> m_sd_cache_reffed{};
    double m_sd_last_scan{0.0};

    // Zur Ladezeit aufgeloest (Lua Z.372-385).
    sdk::RETypeDefinition* m_player_cam_td{nullptr};
    sdk::RETypeDefinition* m_gimmickfix_cam_td{nullptr};
    sdk::RETypeDefinition* m_gamepad_td{nullptr};
    sdk::RETypeDefinition* m_scene_td{nullptr};
    ::REManagedObject* m_sd_ctrl_t{nullptr};
    ::REManagedObject* m_sd_mesh_t{nullptr};
    ::REManagedObject* m_sd_skin_t{nullptr};
    int32_t m_turret_gimmick{6};
    bool m_types_resolved{false};
};

#endif // RE4
