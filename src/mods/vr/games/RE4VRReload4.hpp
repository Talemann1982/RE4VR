// ============================================================================
// RE4VRReload4 -- 1:1-Portierung von re4_vr_reload.lua (7.007 Zeilen).
//
// Leons Handfeuerwaffen (Pistolen / SMGs / Shotguns) UND die Basis des ganzen
// Reload-Blocks: der Ladeweg, der Reserve-Abzug, der Pose-Store und der
// Accessor auf die einzige persistente WeaponItem-Instanz liegen hier.
// Die anderen vier Reload-Teile greifen an rund 250 Stellen darauf zu.
//
// Spezifikation: I:\LUATRANS\PORT_RELOAD1_SPEC.md
//
// REIHENFOLGE: laeuft als ERSTES der sechs Teile (Lua: "reload" < "reload2").
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"

class RE4VRReloadAdv;
class RE4VRReloadMain;

class RE4VRReload4 {
public:
    // ------------------------------------------------------------------
    // Konfiguration (Lua: CFG, Z.143-252)
    // ------------------------------------------------------------------
    struct Cfg {
        bool enabled{true};   // Master -- gibt es nicht mehr, bleibt immer true

        // [SKULL_INDEX] Zeigefinger-Beugung der Ruhepose "skull-default" (Grad,
        // additiv auf die Pose; 0 = Pose unveraendert).
        float skull_idx1{0.0f}, skull_idx2{0.0f}, skull_idx3{0.0f};
        // [SKULL_LADE] Lade (joint_02) waehrend der Dreh-Bewegung.
        float skull_open_deg{-30.0f};
        // [SPIN_DUR] Dauer der Flick-Drehung in Sekunden.
        float skull_spin_dur{0.55f};

        bool pistols_enabled{true};
        bool smgs_enabled{true};
        bool shotguns_enabled{true};
        bool magnum_enabled{true};
        bool rifles_enabled{true};

        float gravity{9.8f};                  // Mag-Drop: freier Fall
        std::string mag_hold_pose{"MAG"};     // Hand-Pose, waehrend das Mag in der Hand ist

        float insert_dur{0.18f};
        float insert_distance{0.15f};         // Legacy-Startwert fuer INSERT_DIST.pistols

        // [INSERT-PUNCH] rein optisch/haptisch
        bool  insert_punch{true};
        float insert_overshoot{0.005f};
        float insert_settle{0.07f};
        float insert_haptic{0.85f};
        float insert_snd_at{0.80f};

        // [MANUAL_INSERT] Mag von Hand hochschieben
        bool  insert_manual{true};
        float insert_travel{0.09f};
        float insert_snap_at{0.80f};
        float insert_back_out{0.02f};
        float insert_redock{0.01f};

        bool reload_ammo{true};

        // Slide-Rack
        bool  rack_enabled{true};
        float rack_grab_dist{0.18f};
        float rack_pull_dist{0.09f};          // unbenutzt seit 1:1-Zug
        float pump_start_pull{0.04f};
        float pump_push_frac{0.5f};
        bool  rack_haptic{true};
        std::string rack_pose{"rack-slide"};
        // [RACK_POSE_ANGLE] zweite Pose bei seitlicher Annaeherung (nur Pistolen)
        std::string rack_pose_side{"MAGRack"};
        float rack_pose_side_deg{45.0f};

        // [SND]
        bool  sound_enabled{true};
        float mag_floor_delay{0.45f};

        int32_t shotgun_ratio{2};
        float   pump_haptic_delay{0.0f};
    } cfg{};

    // Mag-in-Hand-Pose pro Waffe (Offset im Hand-Frame + Rotation + Daumen)
    struct MagHand {
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
        float t_rx{}, t_ry{}, t_rz{};
    };

    // [SHELL_EJECT] Chamber-Startposition + Flug
    struct ShellEject {
        float sx{0.0f}, sy{0.0f}, sz{0.0f};
        float srx{0.0f}, sry{0.0f}, srz{0.0f};
        float vx{0.8f}, vy{0.5f}, vz{0.0f};
        float grav{4.0f}, dur{0.7f}, spin{540.0f};
    };

    // Slide-Rack-Posen, ABSOLUTE lokale Z (wie RE9 SLIDE_BIND_POSE).
    struct SlidePose {
        float rest_z{0.06174f};   // gechambert / Slide vorne
        float park_z{0.04674f};   // Mag drin, noch nicht gerackt
        float back_z{0.03174f};   // leer / Rack-Endpunkt
        // Pose 1 -- Annaeherung von HINTEN
        float dock_x{0.0f}, dock_y{0.0f}, dock_z{0.0f};
        float rack_rx{0.0f}, rack_ry{0.0f}, rack_rz{0.0f};
        // [POSE2_OFFSETS] Pose 2 -- Annaeherung SEITLICH (MAG-Rack)
        float sdock_x{0.077f}, sdock_y{-0.008f}, sdock_z{-0.056f};
        float srack_rx{4.7f}, srack_ry{153.7f}, srack_rz{-37.6f};
    };

    // [DOCK-PORT] fester Gun-Joint + Offset in dessen lokalen Achsen
    struct DockPort {
        std::string joint{};
        float x{0.0f}, y{0.0f}, z{0.0f};
    };

    // [ROTARY/BREAK] Dreh-/Klapp-Konfiguration
    struct Rotary {
        float rx{0.0f}, ry{0.0f}, rz{15.0f};
        float grab_dist{0.15f};
        float lerp{0.06f};
        float pitch_range{55.0f};
    };

    // [POSE-STORE] POSES[name] = { hand, bones }
    struct Pose {
        std::string hand{};
        std::unordered_map<std::string, glm::quat> bones{};
    };

    // ------------------------------------------------------------------
    // Lebenszyklus + Pass-Einstiege
    // ------------------------------------------------------------------
    void on_initialize();
    void install_hooks();
    void on_lua_state_created();
    void on_lua_state_destroyed();

    void on_frame();
    void on_lock_scene_pre();
    void on_late_update();
    void on_update_joint_expression();
    void on_begin_rendering_pre();
    void on_begin_rendering();
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Der Geschwister-Teil, an den der Mag-Drop delegiert.
    void set_adv(RE4VRReloadAdv* adv) { m_adv = adv; }

    // [DLC] Der Main-Teil liefert die geteilten Werte (Shotgun-Ratio,
    // Push-Abstand): in Lua ruft diese Datei sie ueber dessen Globals.
    void set_main(RE4VRReloadMain* main) { m_main = main; }


    // Zugang fuer die Hook-Lambdas und die Lua-Exporte (in Lua sind das
    // Upvalues der Datei-Closures). Gesetzt in on_initialize.
    static RE4VRReload4* instance() { return s_instance; }

    // ------------------------------------------------------------------
    // Was die anderen Reload-Teile benutzen (in Lua die _G.__re4_*-Globals)
    // ------------------------------------------------------------------

    // [ACCESSOR] DIE EINZIGE PERSISTENTE WeaponItem-INSTANZ.
    ::REManagedObject* real_wi(std::optional<int32_t> want_wid = std::nullopt);

    // Reserve-Abzug ohne den nativen reduce (der crasht).
    bool safe_reduce(::REManagedObject* inv, int32_t ammo_id, int32_t n);

    // Laden + buchen. `refill == true` = gibt Munition (kein Reserve-Abzug).
    bool load_and_book(::REManagedObject* inv, int32_t et, int32_t n, bool refill);
    bool safe_inv_reload(::REManagedObject* inv, int32_t et, int32_t n, bool refill);

    // Inventar-Summe einer Munitionssorte. `_num` vergleicht ueber die Zahl --
    // in C++ ist das derselbe Weg, weil die TypeDB Enums als Integer liefert.
    int32_t item_count_sum(::REManagedObject* inv, int32_t id);

    ::REManagedObject* pe();
    ::REManagedObject* live_gun();
    std::optional<int32_t> gun_ammo();
    void sync_gun_ammo();
    ::REManagedObject* equip_gun();
    bool is_loop_reload();

    // [MAG-REST] Vor dem Nullen den Magazinrest merken.
    void carry_capture(::REManagedObject* wi, const char* where, std::optional<int32_t> wid);

    // [POSE-STORE] Joint-Writer. `apply_pose` ueber den Namen, `apply_pose_bones`
    // mit einem fertigen Bone-Dict (Messer-Links-Pose, Push-Pose).
    bool apply_pose(const std::string& name, float blend);
    bool apply_pose_bones(const std::unordered_map<std::string, glm::quat>& bones, float blend);
    std::vector<std::string> pose_names() const;

    // [SHELL-RATIO-SYNC] EIN Schalter fuer Leon UND Ada.
    int32_t shotgun_ratio_get(int32_t wid);

    // Vom Holster gerufen: Mag in der Hand AN (Grip) / AUS (Grip los).
    bool set_mag_in_hand(bool active);

    // Der Platz dieses Teils in der Holster-Kette. In Lua steht ganz oben im
    // Wrapper: `if category_of(get_equip_wid()) == nil then durchreichen end`.
    // std::nullopt = nicht unsere Waffe -> der Aufrufer fragt reload3 weiter.
    std::optional<bool> set_mag_chain_probe(bool active);

    // [MANUAL_INSERT] Messwert des Handschubs (Hoehe des linken Controllers).
    std::optional<float> mag_push_dist();

    // [CUTSCENE] true, solange wir NICHT im Gameplay sind.
    bool reset_open_after_cutscene();

    // [SKULL_FLICK] Wrist-Flick cockt den Skull Shaker.
    void break_cock_flick();

private:
    static RE4VRReload4* s_instance;

    RE4VRReloadAdv* m_adv{nullptr};
    RE4VRReloadMain* m_main{nullptr};

    // [DLC LET-GO] hielten wir die geteilten Globals gerade?
    bool m_r4_had{false};
    void r4_release();

    // ---- Datentabellen (Lua: Datei-Locals) --------------------------
    std::unordered_map<int32_t, MagHand>    m_maghand{};
    std::unordered_map<int32_t, ShellEject> m_shell_eject{};
    std::unordered_map<int32_t, SlidePose>  m_slide_pose{};
    std::unordered_map<int32_t, SlidePose>  m_slide_pose2{};   // [EMPTY-RELOAD SLIDE] _08
    std::unordered_map<int32_t, DockPort>   m_dock_port{};
    std::unordered_map<int32_t, DockPort>   m_lever_port{};
    std::unordered_map<int32_t, Rotary>     m_rotary_cfg{};
    std::unordered_map<int32_t, std::string> m_joint_mag{};
    std::unordered_map<int32_t, std::string> m_joint_slide{};
    std::unordered_map<int32_t, std::string> m_mag_pose{};
    std::unordered_map<int32_t, std::string> m_rack_pose{};
    std::unordered_map<int32_t, float>      m_insert_dist_wid{};
    std::unordered_map<int32_t, int32_t>    m_shotgun_ratio_wid{};
    std::unordered_map<int32_t, std::vector<int32_t>> m_shell_parts{};
    std::unordered_map<std::string, Pose>   m_poses{};
    std::unordered_map<std::string, float>  m_insert_dist{};   // pro GATTUNG

    MagHand&    maghand(int32_t wid);
    ShellEject& shell_eject_cfg(int32_t wid);
    SlidePose&  slide_pose(int32_t wid);
    SlidePose&  slide_pose2(int32_t wid);
    Rotary&     rotary_cfg(int32_t wid);

    // ---- Allowlisten (Code-Konstanten) ------------------------------
    static bool is_shotgun(int32_t wid);
    static bool is_pistol(int32_t wid);
    static bool is_smg(int32_t wid);
    static const char* category_of(int32_t wid);   // nullptr = keine
    bool category_enabled(const char* cat) const;
    static bool no_cycle_after_shot(int32_t wid);
    static bool no_reload_cycle(int32_t wid);
    static bool dryfire_only_when_empty(int32_t wid);
    static bool rotary_cycle(int32_t wid);
    static bool break_action(int32_t wid);
    static bool chamber_hold_persist(int32_t wid);
    static bool engine_closes_slide(int32_t wid);
    static bool parent_space_dock(int32_t wid);
    static const char* empty_reload_joint(int32_t wid);   // nullptr = keiner
    static const char* rack_pose_empty(int32_t wid);
    static std::optional<float> shotgun_chamber_z(int32_t wid);
    static std::optional<uint32_t> auto_pump_mute(int32_t wid);
    static const char* weapon_name(int32_t wid);
    static std::string wp_label(int32_t wid);
    static std::optional<uint32_t> snd_id(int32_t wid, const char* key);

    // ---- Persistenz -------------------------------------------------
    void load_cfg();
    void save_cfg();

    // ---- Player / Waffe ---------------------------------------------
    ::REManagedObject* get_ctx();
    std::optional<int32_t> get_equip_wid();
    ::REManagedObject* get_body();
    ::REManagedObject* body_tf();
    ::REManagedObject* get_weapon_item();
    ::REManagedObject* get_live_weapon_item();
    ::REManagedObject* get_inv_row_weapon_item();
    std::optional<int32_t> get_equip_type_main();
    ::REManagedObject* find_weapon(int32_t wid);

    ::REManagedObject* m_character_manager{nullptr};
    ::REManagedObject* m_pe_cache{nullptr};
    std::optional<int32_t> m_equip_type_main{};

    // [POSE-STORE] Joint-Map der Body-Transform
    ::REManagedObject* m_pmap_tf{nullptr};
    std::unordered_map<std::string, ::REManagedObject*> m_pmap{};
    const std::unordered_map<std::string, ::REManagedObject*>& pose_build_map();

    // ---- Waffen-Cache -----------------------------------------------
    struct Wep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* mag_joint{nullptr};
        ::REManagedObject* slide_joint{nullptr};
        ::REManagedObject* slide_joint2{nullptr};   // [EMPTY-RELOAD SLIDE] _08
        std::optional<glm::quat> cycle_rest_rot{};  // [ROTARY/BREAK] Ruhe-Rotation
        std::optional<glm::vec3> rest_lp{};
        std::optional<glm::quat> rest_lr{};
        std::optional<glm::vec3> chamber_off{};     // [SHOTGUN CHAMBER] waffen-lokal
    } m_wep{};

    bool m_weapon_reacquired{false};
    void refresh_weapon();
    ::REManagedObject* rack_joint();
    SlidePose& rack_slide_pose();

    // ---- Sounds ------------------------------------------------------
    bool m_sg_our_sound{false};
    void play_weapon_sound(std::optional<uint32_t> id);

    // ---- Zustand -----------------------------------------------------
    // Slide-Rack (RE9-Muster)
    struct Rack {
        bool needs{false};
        bool grab_active{false};
        bool armed{false};
        float gx{}, gy{}, gz{};
        std::optional<float> rgx{}, rgy{}, rgz{};   // [LAUFEN] Anker der Waffenhand
        float frac{0.0f};
        bool pulled{false};
        bool pushed{false};
        bool has_mag{false};
        bool empty{false};
        bool empty_when_dropped{false};
        bool empty_reload{false};
        bool tuning{false};
        float tune_frac{0.0f};
        bool dock_tune{false};
        float dock_blend{0.0f};
        bool _last_grip{false};
        float _last_dist{-1.0f};

        // Felder, die in Lua wegen des 200-Local-Limits auf `rack` liegen
        bool _zeroed_by_us{false};
        bool _chambered_hold{false};
        std::optional<int32_t> _prev_gun_ammo{};
        std::optional<int32_t> _prev_shot_seq{};
        std::optional<double> _ammo_input_t{};
        std::optional<float> g_relz{};
        std::optional<glm::vec3> pump_off{};
        std::optional<float> _pump_ref_z{};
        std::optional<float> _pump_init_dist{};
        float _pump_max{0.0f};
        bool pose_side{false};
        bool pose_side_preview{false};
        std::optional<float> _pose_ang{};
        bool pushdock_was_active{false};
        std::optional<double> _heal_done_t{};
        std::unordered_map<int32_t, bool> _needs_store{};
        std::unordered_map<int32_t, int32_t> _retained_store{};
        std::optional<int32_t> _gone_wid{};
    } m_rack{};

    struct Drop {
        bool active{false};
        bool use_module{false};
        ::REManagedObject* joint{nullptr};
        float sx{}, sy{}, sz{};
        std::optional<glm::quat> srot{};
        double t0{0.0};
    } m_drop{};

    struct MagHandState {
        bool active{false};
        ::REManagedObject* joint{nullptr};
        std::optional<int32_t> wid{};
        float dist{0.0f};
        bool grip_held{false};
        // Sentinel: `true` = Abstand beim naechsten Durchlauf einmalig merken
        std::optional<float> redock_d{};
        bool redock_pending{false};
        bool want_drop{false};
    } m_mag_hand{};

    struct MagInsert {
        bool active{false};
        ::REManagedObject* joint{nullptr};
        double t0{0.0};
        float dur{0.18f};
        glm::vec3 slp{};                    // Start-Lokalpos
        std::optional<glm::quat> slr{};
        glm::vec3 rlp{};                    // Ziel (Ruhelage / Chamber-Port)
        std::optional<glm::quat> rlr{};
        std::optional<glm::vec3> olp{};     // [INSERT-PUNCH] Overshoot-Ziel
        bool settle{false};
        double settle_t0{0.0};
        bool keyframe{false};
        bool snd_played{false};
        bool visual{false};                 // [SS-FLASH] reiner Sicht-Pass
        // [MANUAL_INSERT]
        bool manual{false};
        std::optional<float> d0{};
        float prog{0.0f};
        std::optional<float> p0{};
        std::optional<double> wd_t0{};
        float wd_max{0.0f};
        std::optional<glm::vec3> wd_lp0{};
    } m_mag_insert{};

    struct MagTune { bool active{false}; } m_mag_tune{};

    // [CHAMBER] Top-Loader-Zustand (Red9 ist ausgezogen -> Tabelle leer)
    struct Chamber {
        bool open{false};
        float blend{0.0f};
        std::optional<glm::vec3> base{};
        std::string base_joint{};
    } m_chamber{};

    // [ROTARY_CYCLE] Laufzeit
    struct RotaryState {
        float prog{0.0f};
        int dir{0};
        bool _prev_trig{false};
        bool _prev_grip{false};
        bool _grip_latched{false};
        bool preview{false};
        float _last_dist{-1.0f};
        bool pending{false};
        bool _was_grab{false};
    } m_rotary{};

    // [BREAK_ACTION] Klapphebel
    struct BreakState {
        float prog{0.0f};
        bool open{false};
        bool _prev_b{false};
        bool _was_open{false};
        bool preview{false};
    } m_break{};

    // [BREAK_PUMP_SND] / [SKULL_FLICK]
    struct PumpSnd {
        std::optional<double> t0{};
        std::optional<int32_t> prev_seq{};
        std::optional<double> first_at{};
        std::optional<double> second_at{};
        bool cock_pending{false};
        int pk_dir{0};
        double pk_t{0.0};
        double last_flick{0.0};
        std::optional<float> fy{};
        std::optional<double> fy_t{};
    } m_pumpsnd{};

    // [SHELL_EJECT] Laufzeit
    struct ShellEjectState {
        bool preview{false};
        bool flying{false};
        float t{0.0f};
        double last_clock{0.0};
        bool _prev_pulled{false};
    } m_shell_eject_st{};

    // Joint-Finder (UI-Werkzeug)
    struct Finder {
        bool active{false};
        std::string name{"_01"};
        int idx{1};
        float x{0.0f}, y{0.0f}, z{0.0f};
        std::optional<glm::vec3> base{};
        std::string base_name{};
    } m_finder{};

    bool m_managed{false};

    // [FIRE-GATE] die Bedingungen einmal pro Frame gespiegelt.
    bool m_fg_block{false};
    double m_fg_t{0.0};
    void update_fire_gate();
    bool m_mag_out{false};
    std::unordered_map<int32_t, bool> m_mag_out_store{};
    int32_t m_mag_retained{0};
    std::optional<int32_t> m_last_handled_wid{};
    bool m_red9_prev_reloading{false};
    bool m_b_prev{false};
    double m_mag_floor_at{0.0};
    bool m_drop_prev{false};
    bool m_empty_trig_prev{false};
    bool m_mag_hidden_applied{false};

    // Haptik-Warteschlange
    struct Haptic { double at{}; float amp{}; float dur{}; };
    std::vector<Haptic> m_haptic_queue{};

    // ---- Funktionen (in Lua Datei-Locals) ---------------------------
    std::optional<int32_t> handled();
    bool mag_is_present() const;

    bool drain_to_zero(::REManagedObject* wi);
    bool set_gun_loaded(int32_t n);

    bool start_mag_drop();
    void stop_mag_drop();
    void update_mag_drop();

    ::REManagedObject* get_left_hand();
    ::REManagedObject* get_thumb_joint();
    ::REManagedObject* m_lhand_joint{nullptr};
    ::REManagedObject* m_thumb_joint{nullptr};

    void update_mag_in_hand();
    void capture_mag_rest();
    bool start_mag_insert();
    void update_mag_insert();

    std::optional<glm::vec3> shotgun_chamber_world();
    std::optional<glm::vec3> dock_port_world();
    std::optional<glm::vec3> lever_port_world();
    void check_mag_insert_proximity();

    bool mag_to_hand();
    bool drop_mag_simple();

    int32_t current_reserve();
    bool pump_one_out();
    bool force_eject();
    bool right_b_down();

    std::optional<int32_t> read_loaded_of(::REManagedObject* wi,
                                          std::optional<int32_t> want_wid);
    std::optional<int32_t> live_loaded_count();

    ::REManagedObject* get_gun();
    std::optional<int32_t> gun_state_val();
    bool gun_is_empty();
    void gun_chamber();
    std::optional<int32_t> m_ammoempty_num{};
    std::optional<int32_t> m_gun_state_holding{};
    std::unordered_map<int32_t, std::string> m_state_names{};

    void rack_haptic(float amp, float dur);
    void queue_haptic(float amp, float dur);
    void service_haptics();

    void rotary_do_load();
    void clear_rack();
    void update_rack_state();

    bool left_grip_down();
    bool left_trigger_down();
    std::optional<float> left_ctrl_pitch_deg();
    std::optional<glm::vec3> get_left_controller_pos();

    void update_rack_gesture();
    void update_rotary_cycle();
    void update_break_action();
    void update_break_pump_sound();

    void update_dock_blend();
    void publish_dock();
    bool publish_push_dock();

    void apply_slide_park();
    void apply_rack_hand_pose();

    ::REManagedObject* finder_tf();
    const std::vector<std::string>& weapon_joint_names();
    std::optional<int32_t> m_jn_cache_wid{};
    std::vector<std::string> m_jn_cache{};
    void apply_finder();

    void toggle_chamber();
    void apply_shell_eject();
    void update_shell_eject();
    void apply_chamber();

    void reset_reload_state();
    void apply_mag_out_hidden();

    // [SHELL_PART] Sub-Mesh-Parts der Shotgun-Huelse
    struct SgParts {
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::optional<std::vector<int32_t>> e1{};
        std::vector<int32_t> shell{};
        bool empty{false};
    } m_sg{};

    ::REManagedObject* sg_mesh();
    bool sg_part_on(::REManagedObject* m, int32_t i);
    void sg_update_parts(std::optional<int32_t> loaded);
    void sg_force_shell_parts(bool on);

    // [SHELL_SPAWN] (Tabelle ist leer -> der Weg ist heute unerreichbar)
    struct Spawn {
        ::REManagedObject* go{nullptr};
        ::REManagedObject* tf{nullptr};
        std::optional<int32_t> wid{};
    } m_spawn{};

    void destroy_spawn_go();
    ::REManagedObject* find_shell_generator();
    ::REManagedObject* ensure_shell_spawn();
    void update_shell_spawn();

    // [SHELL_CLONE] Skull Shaker: Patrone in der Hand als Mesh-Klon
    struct SsClone {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::string parts_sig{};
        bool parented{false};
    } m_ssc{};

    ::REManagedObject* ss_gun_mesh();
    void ss_destroy();
    bool ss_spawn();
    void ss_isolate();
    void ss_place(float px, float py, float pz, float rx, float ry, float rz);
    void ss_apply();
    void ss_repos_late();

    // Pass-Rumpfe
    void apply_drop_pass();
    void apply_drop_joint_pass();
    void apply_slide_pass();
    void ss_flash_pass();

    // Die drei getakteten on_frame-Teile aus Lua
    void tick_sortenfix();
    void tick_saveload_guard();
    void tick_merc_round();
    void tick_dryfire_watch();

    double m_sortefix_next{0.0};
    std::string m_sortefix_last{};
    double m_lw_next{0.0};
    std::optional<uintptr_t> m_lw_body_addr{};
    std::optional<int32_t> m_round_seen{};
    double m_dfw_t{0.0};
    std::optional<int32_t> m_dfw_run{}, m_dfw_item{};

    // [MAG-CARRY] Merker aus dem Auswurf
    std::optional<int32_t> m_mag_carry{};
    std::optional<int32_t> m_mag_carry_wid{};

    // Von safe_inv_reload gesetzt, vom Abzug gelesen (Lua: __re4_reserve_aid*)
    std::optional<int32_t> m_reserve_aid{};
    std::optional<int32_t> m_reserve_aid_wid{};
    bool m_reserve_aid_ok{false};
    bool m_addwi_alive{true};
    ::REManagedObject* m_live_wi{nullptr};
    double m_live_wi_t{-1.0};
    int32_t m_reload_direct_ok{0};
    int32_t m_reload_direct_fail{0};

    // [SHELL_CLONE] Offsets (in Lua das Global __re4_ss_clone)
    struct SsCloneCfg {
        int32_t part{1};
        float x{0.0f}, y{0.0f}, z{0.0f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
        float scale{1.0f};
        bool preview{false};
    } m_ss_clone{};

    // [CUTSCENE] Merker fuer den Gameplay-Wiedereintritt
    bool m_gp_was_out{false};

    // [MANUAL_INSERT] Ausnahme-Liste: [wid] = false nimmt eine Waffe heraus
    std::unordered_map<int32_t, bool> m_insert_manual_wid{};

    // Die aktuell gefuehrte Waffe (Lua: __re4_reload_ui_wid). Wird jeden Frame
    // gesetzt UND nach Lua gespiegelt -- reload_adv liest sie fuer Preview + UI.
    std::optional<int32_t> m_reload_ui_wid{};

    // ------------------------------------------------------------------
    // Rueckbau-Schalter. In Lua sind das Globals, die NIE gesetzt werden
    // (`rawget(_G, "...") ~= false`) -- reine Notausgaenge aus der Entwicklung.
    // Als Member statt Lua-Lesungen: sonst kostet jeder Frame ein Dutzend
    // ScriptRunner-Sperren fuer Werte, die sich nie aendern.
    // ------------------------------------------------------------------
    bool m_use_accessor_item{true};    // __re4_use_accessor_item
    bool m_inv_remove_empty{true};     // __re4_inv_remove_empty
    bool m_mag_11{true};               // __re4_mag_11 (1:1-Kopplung)
    bool m_rack_relative{true};        // __re4_rack_relative
    bool m_insert_kf_anchor{true};     // __re4_insert_kf_anchor
};

#endif
