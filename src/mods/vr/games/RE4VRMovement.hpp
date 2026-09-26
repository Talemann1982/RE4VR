// ============================================================================
// RE4VRMovement -- 1:1-Portierung von re4_vr_movement.lua (3001 Zeilen)
// Spezifikation: I:\LUATRANS\PORT_MOVEMENT_SPEC.md (samt NACHTRAG 03.09.2026)
//
// EINREIHUNG (Nachtrag F): ans ENDE des RE4VR-Blocks in Mods.cpp, hinter
// RE4VREquipLock -- vier unabhaengige Gruende:
//   1. apply_lag_fix muss nach RE4VRFirstPerson::apply_movement_stabilization
//   2. RE4VRHolster liest __re4_ub_z_delta (in Lua sah es den Vorframe-Wert)
//   3. RE4VRArmChain liest __vr_anim_l0   (in Lua sah es den Vorframe-Wert)
//   4. RE4VRCrosshair setzt is_aim, movement liest es in derselben Phase
//
// RESET-SEMANTIK (Nachtrag A2/A3/A4): _G ueberlebt "Reset Scripts" NICHT, und
// Lua-Locals auch nicht. Alles, was hier Member ist und in Lua ein `local` war,
// MUSS in on_lua_state_destroyed zurueck -- sonst wird nachgebaut, was der
// Lua-KOMMENTAR behauptet, statt was Lua TUT. Besonders `grav_orig`.
// ============================================================================
#pragma once

#if defined(RE4)

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <json.hpp>

#include "../../../Mod.hpp"

class RE4VRMovement : public Mod {
public:
    static std::shared_ptr<RE4VRMovement>& get();

    std::string_view get_name() const override {
        return "RE4VRMovement";
    }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Public-Block: Roomscale + Lehn-/Recenter-/Duck-Regler (RE4VRMenu
    // zeichnet ihn ueber RE4VRBinding::draw_public_ui, direkt unter Snapturn).
    void draw_public_roomscale();

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // Aus dem updateCameraPosition-Hook (richtige Instanz + Timing).
    void hook_capture_pcc(::REManagedObject* pcc);
    void hook_drive_hmd_yaw();

    // Der Jack-Block aus [SQUEEZE_ANTIBEAM]. Rueckgabe: true = SKIP_ORIGINAL,
    // dann steht der Ersatz-Rueckgabewert in `out_ret`.
    bool sqab_setup_jack_pre(::REManagedObject* holder, int32_t& out_ret);

private:
    // ---- Referenzzaehlung -------------------------------------------------
    struct RefHandle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    static bool keep(::REManagedObject* o, RefHandle& out);
    static void drop(RefHandle& h);

    // ---- Konfiguration (Lua Z.41-186) -------------------------------------
    struct PinZ {
        double Hip{0.0};
        double Spine_0{0.0};
        double Spine_1{0.0};
        double Spine_2{0.0};
        double Neck_0{0.0};
        double Neck_1{0.0};
        double Head{0.0};
    };

    struct PinX {
        double Spine_0{0.0};
        double Spine_1{0.0};
        double Spine_2{0.0};
        double Neck_0{0.0};
        double Neck_1{0.0};
        double Head{0.0};
    };

    struct Cfg {
        bool   enabled{true};
        bool   hmd_yaw_drive{true};
        double yaw_offset_deg{0.0};
        bool   roomscale{false};
        bool   rs_lean{true};
        double rs_lean_radius{0.10};
        bool   rs_lean_migrated{false};
        bool   rs_recenter{true};
        bool   rs_crouch{false};
        double rs_crouch_pct{0.20};
        double rs_stand_height{0.0};
        bool   hmd_follow{true};
        double hmd_follow_deadzone{0.005};
        double hmd_follow_alpha{0.25};
        bool   hip_follow{true};
        double spine_yaw_deg{0.0};
        bool   ub_lock{false};
        double ub_trim_deg{0.0};
        bool   ub_lock_world{true};
        bool   spine_pin{true};
        PinZ   pin_z{};
        double pin_z_hip_walk{0.0};
        // [nil-SENTINEL] Lua-Default ist `nil` -- NICHT 0.0. Der Verbraucher
        // macht `cfg.pin_z_hip_crouch or cfg.pin_z.Hip`, und in Lua ist 0.0
        // WAHR: ein 0.0 statt nil schaltet das Erben ab.
        std::optional<double> pin_z_hip_crouch{};
        double pin_x_hip{0.0};
        PinX   pin_x{};
        double pin_ub_z{0.0};
        double pin_ub_x{0.0};
        double pin_ub_x_crouch{0.0};
        double pin_ub_yaw{0.0};
        double pin_ub_z_crouch{0.0};
        double pin_hip_yaw{0.0};
        double pin_spine1_roll{0.0};
        double body_yaw_speed{12.0};
        bool   yaw_hmd_target{false};
        // Wird nach dem Laden HART auf false gesetzt (Lua Z.254). Der ganze
        // Zweig ist beim Start unerreichbar, per UI aber reaktivierbar --
        // und NIE persistent, weil Z.254 beim naechsten Start wieder greift.
        bool   cam_body_rotate{true};
        bool   stop_skip{true};
        double stop_skip_frames{30.0};
        bool   start_skip{true};
        double start_skip_frames{30.0};
        bool   no_pivot{false};
        bool   grav_fix{true};
        double grav_value{150.0};
        bool   grav_in_elevator{false};   // [TOT] geladen, gespeichert, nie gelesen
        bool   lag_boost_on{true};
        double lag_boost_target{2.01};
        double lag_boost_max{2.41};
        double lag_boost_window{0.40};
        bool   lag_brake_on{true};
        double lag_brake_gain{0.00};
        double lag_brake_window{1.50};

        // Die beiden gespeicherten Posen. In Lua sind das rohe Tabellen in cfg;
        // ein fehlendes Feld verschwindet beim json.dump_file KOMPLETT (also
        // weder null noch 0.0 schreiben).
        nlohmann::json pin_pose{};
        nlohmann::json crouch_pose{};
    };

    Cfg m_cfg{};
    // [K1] Erst nach load_cfg() darf save_cfg() schreiben.
    bool m_cfg_loaded{false};
    void load_cfg();
    void save_cfg();

    // ---- Pose-Daten -------------------------------------------------------
    struct PoseEntry {
        glm::vec3 p{0.0f, 0.0f, 0.0f};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    };

    struct Pose {
        std::vector<PoseEntry> rel{};
        std::vector<std::optional<glm::quat>> par_inv{};
        std::optional<PoseEntry> null_rel{};
        bool valid{false};
    };

    // ---- Getter -----------------------------------------------------------
    ::REManagedObject* get_body_transform();
    ::REManagedObject* get_player_cam_controller();
    bool is_ks_active();
    bool is_pin_release();
    bool is_crouch_active();
    bool pure_gameplay_only();

    // ---- Yaw --------------------------------------------------------------
    std::optional<double> flat_yaw_of(const glm::quat& rot) const;
    std::optional<glm::quat> yaw_quat_of(const glm::quat& rot) const;

    // ---- [BODY-EPOCH 2026-09-22] -------------------------------------------
    // Spieler-Body gewechselt (Save-Load/Tod) -> gemerkte Body-Zeiger weg.
    void check_body_epoch();
    void drop_body_caches();

    // ---- Hip-Follow -------------------------------------------------------
    ::REManagedObject* get_hip_joint(::REManagedObject* tf);
    bool is_user_turning();
    void apply_hip_follow(::REManagedObject* tf, const glm::quat& root_yaw_quat);

    // ---- Spine/Pin --------------------------------------------------------
    ::REManagedObject* get_spine_joint(::REManagedObject* tf, const char* name);
    bool spinepin_resolve(::REManagedObject* tf);
    bool spinepin_capture(::REManagedObject* tf, Pose& out);
    void spinepin_store(const Pose& pose, const char* cfg_key);
    bool spinepin_restore(const char* cfg_key, Pose& out);
    void apply_spine_pin(bool can_capture);
    void apply_crouch_pin(bool can_capture);
    void apply_crouch_ub_z(bool capture_base);
    void apply_ub_lock(bool can_capture);
    void apply_spine_yaw(::REManagedObject* tf);

    // ---- FSM-Eingriffe ----------------------------------------------------
    ::REManagedObject* get_motion_fsm(::REManagedObject** out_ctx);
    bool apply_skip_nodes(const std::vector<const char*>& nodes, double frames, bool enable,
                          bool overwrite_interp, ::REManagedObject** out_ctx);
    bool apply_stop_skip(bool enable);
    bool apply_start_skip(bool enable);
    bool apply_no_pivot(bool disable);
    void update_stop_skip();

    // ---- Stillzone --------------------------------------------------------
    bool mv_in_zone() const;
    void mv_zone_tick();

    void update_anim_export();

    // ---- Harter Yaw -------------------------------------------------------
    void apply_hard_yaw(bool sample);
    void enforce_hard_yaw();

    // ---- Roomscale --------------------------------------------------------
    void apply_auto_center();
    void roomscale_recenter();
    void roomscale_recenter_events();
    void roomscale_crouch();
    void apply_hmd_body_follow();

    // [ROOMSCALE 1:1 PRAYDOG 16.09.2026] FirstPerson::update_player_roomscale,
    // hinter m_cfg.roomscale. Laeuft im PRE-Hook von UnlockScene.
    void roomscale();

    // Zeitpunkt des letzten verfehlten Gates -- danach 1 s Pause, wie bei
    // praydog (m_last_roomscale_failure).
    double m_rs_last_failure{0.0};

    // [DUCKEN PER IK 16.09.2026] Bein-IK fuer das Ducken (s. roomscale()).
    ::REManagedObject* rs_ikleg();
    void rs_ik_restore();

    // true, solange roomscale() das IK beschrieben hat -- dann ist beim
    // naechsten verfehlten Gate EINMAL zurueckzustellen.
    bool    m_rs_ik_active{false};

    // Die Werte des Spiels, eingelesen vor dem ersten Ueberschreiben.
    float   m_rs_ik_ground_up{0.8f};
    int32_t m_rs_ik_ctrl{0};

    // [FUSS-SNAP 16.09.2026] Pruefintervall und "IK steht gerade fuer einen
    // Frame aus" (dann im naechsten Frame -- oder beim Restore -- wieder an).
    int     m_rs_snap_frame{0};
    bool    m_rs_snap_pending{false};
    void roomscale_flush_body();

    // ---- HMD-Yaw-Drive ----------------------------------------------------
    std::optional<double> hmd_raw_yaw();
    bool hmd_yaw_active();
    void drive_hmd_yaw();

    // ---- Gravitation / Lag ------------------------------------------------
    ::REManagedObject* ground_adsorber(::REManagedObject** out_ctx);
    void apply_grav_fix();
    double lag_pad_axis();
    void apply_lag_fix();

    // ---- Squeeze-Antibeam -------------------------------------------------
    ::REManagedObject* sqab_body_tf();
    bool sqab_in_gimmick();
    void sqab_tick();
    bool sqab_is_retrigger();

    void publish_globals(sol::state& lua);
    void reset_state();

    // ========================================================================
    // Zustand
    // ========================================================================
    RefHandle m_character_manager{};
    RefHandle m_camera_system{};
    sdk::RETypeDefinition* m_player_cam_td{nullptr};
    sdk::RETypeDefinition* m_pad_td{nullptr};
    sdk::RETypeDefinition* m_sqab_gm_td{nullptr};
    bool m_types_resolved{false};

    std::optional<glm::quat> m_last_yaw_quat{};
    std::optional<double>    m_owned_yaw{};
    std::optional<double>    m_body_yaw_last_t{};

    // Hip
    RefHandle m_hip_joint{};
    std::optional<glm::quat> m_hip_ref_offset{};
    double m_hipjv_bad{-999.0};

    // Spine
    std::map<std::string, RefHandle> m_spine_joint_cache{};
    std::map<std::string, glm::quat> m_spine_last_written{};

    // Spine-Pin
    ::REManagedObject* m_spinepin_tf{nullptr};   // nur Vergleichsmarke
    std::vector<RefHandle> m_spinepin_joints{};
    std::vector<std::string> m_spinepin_names{};
    RefHandle m_spinepin_null_off{};
    Pose m_spinepin_pose{};
    bool m_spinepin_cfg_tried{false};
    bool m_spinepin_provisional{false};
    double m_run_blend{0.0};
    double m_walk_blend{0.0};

    // Crouch-Pin
    Pose m_crouchpin_pose{};
    bool m_crouchpin_cfg_tried{false};
    bool m_crouchpin_capture_req{false};
    std::map<int, glm::vec3> m_crouch_ubz_base{};
    bool m_crouch_ubz_base_valid{false};

    // UB-Lock
    std::optional<std::map<std::string, glm::quat>> m_ub_lock_pose{};
    std::optional<std::map<std::string, glm::quat>> m_ub_lock_rel{};
    struct UbBase {
        glm::quat raw{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat parent{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat world{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::optional<UbBase> m_ub_lock_base{};
    std::optional<double> m_ub_trim_applied{};

    // FSM
    struct SkipState {
        bool applied{false};
        ::REManagedObject* last_ctx{nullptr};   // nur Vergleichsmarke
    };
    SkipState m_stop_skip{};
    SkipState m_start_skip{};
    SkipState m_no_pivot{};

    // Stillzone
    bool m_zone_prev{false};
    std::optional<double> m_zone_t{};
    double m_zone_win{0.5};

    RefHandle m_bw_motion{};

    uint64_t m_body_epoch{0};   // [BODY-EPOCH] s. re4vr::body_epoch()

    // Roomscale
    std::optional<double> m_hmd_follow_last_t{};
    ::REManagedObject* m_auto_center_ctx{nullptr};   // nur Vergleichsmarke
    bool m_rs_prev_ks{false};
    std::optional<glm::vec2> m_rs_prev_pos{};
    std::optional<glm::vec2> m_rs_prev_hmd{};
    glm::vec2 m_rs_pending{0.0f, 0.0f};
    double m_rs_crouch_next_t{0.0};

    // HMD-Yaw-Drive
    RefHandle m_hmd_yaw_pcc{};
    std::optional<double> m_hmd_yaw_prev{};

    // Gravitation
    std::optional<double> m_grav_orig{};
    bool m_grav_active{false};

    RefHandle m_pg_pause{};
    RefHandle m_pg_gui{};

    // Lag
    struct Lagm {
        std::optional<glm::vec3> last_pos{};
        std::optional<double> last_t{};
        double spd{0.0};
        double pad_was{0.0};
        std::optional<double> edge_t{};
        int edge_kind{0};     // 0 = keine, 1 = START, 2 = STOP
        double boost_now{0.0};
        bool brake_now{false};
    };
    Lagm m_lagm{};

    // Squeeze-Antibeam
    bool m_sqab_active{false};
    std::optional<glm::vec4> m_sqab_last{};
    bool m_sqab_pin{false};
    std::optional<glm::vec4> m_sqab_pin_pos{};
    bool m_jack_hooked{false};
    int32_t m_sqab_skip_ret{0};
    bool m_sqab_skip_valid{false};
};

#endif // RE4
