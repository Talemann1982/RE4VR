// ============================================================================
// RE4VRReload3 -- 1:1-Portierung von re4_vr_reload3.lua (4.113 Zeilen).
//
// VIER voellig gekapselte Waffen-Maschinen in EINER Datei. In Lua lebt jede in
// einem eigenen do...end-Block mit eigenem Local-Budget, eigener JSON, eigenen
// Pose-Daten und eigenen Callbacks; geteilt werden nur die Datei-Helfer.
//
//   1 Chicago Sweeper  4201   ..._reload3_chicago.json
//   2 Handcannon       4502   ..._reload3_handcannon.json
//   3 Rocket Launcher  4900   ..._reload3_rl.json
//   4 Flamethrower     4701   ..._reload3_flamethrower.json
//
// REIHENFOLGE: laeuft als DRITTES der sechs Reload-Teile (nach reload2, vor
// reload4_dlc). Innerhalb der Datei ist die Reihenfolge der vier Maschinen je
// Pass ebenfalls festgelegt -- sie entspricht der Registrierungsreihenfolge in
// Lua.
//
// HOLSTER-KETTE in Lua: FT -> RL -> Handcannon -> Chicago -> reload2 -> reload.
// Der spaeter geladene Wrapper wird zuerst gefragt, also von hinten nach vorn.
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"
#include "RE4VRReloadAdv.hpp"

class RE4VRReloadMain;

class RE4VRReload3 {
public:
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

    void set_siblings(RE4VRReloadMain* main, RE4VRReloadAdv* adv) {
        m_main = main;
        m_adv = adv;
    }

    // Die Holster-Kette dieser Datei (vier Wrapper in Lua). std::nullopt =
    // keine unserer Waffen -> der Aufrufer reicht an reload2 weiter.
    std::optional<bool> set_mag_in_hand(bool active);

    static RE4VRReload3* instance() { return s_instance; }

private:
    static RE4VRReload3* s_instance;

    RE4VRReloadMain* m_main{nullptr};
    RE4VRReloadAdv* m_adv{nullptr};

    // ---- geteilte Helfer (Datei-Locals in Lua) ----------------------
    ::REManagedObject* get_ctx();
    std::optional<int32_t> get_equip_wid();
    ::REManagedObject* body_tf();
    ::REManagedObject* get_pe();
    ::REManagedObject* find_weapon(int32_t wid);
    static bool right_b_down();

    ::REManagedObject* m_character_manager{nullptr};
    ::REManagedObject* m_pe_cache{nullptr};

    using Bones = std::unordered_map<std::string, glm::quat>;

    // Gemeinsame Pose-Map (in Lua hat jeder Block seine eigene, inhaltlich
    // identische Kopie -- hier reicht eine, das Ergebnis ist dasselbe).
    ::REManagedObject* m_pmap_tf{nullptr};
    std::unordered_map<std::string, ::REManagedObject*> m_pmap{};
    const std::unordered_map<std::string, ::REManagedObject*>& pose_map();
    bool pose_apply(const Bones& bones, float blend);

    // [POSE_FADE] dateiweit, wie in reload2.
    struct PoseFade {
        std::string name{};
        std::optional<double> release_t{};
        bool flag{false};
    };

    static constexpr double POSE_FADE_DUR = 0.10;
    static bool pose_fade_step(PoseFade& f, const std::string& want, float& blend_out);

    // ==================================================================
    // 1 -- CHICAGO SWEEPER (Lua Z.130-1114)
    // ==================================================================
    struct CSlide {
        // [OPEN-BOLT] gemessen an joint_01: hinten/ready = -0.02336,
        // vorne/leer = +0.08664 (Schuss-Peak). rest_z = back_z = hinten
        // (chambered + Zieh-Ende); park_z = vorne (Leer-Zustand). Hub ~0.11.
        float rest_z{-0.02336f}, park_z{0.08664f}, back_z{-0.02336f};
        float empty_x{0.0f};
        float dock_x{0.045f}, dock_y{0.032f}, dock_z{-0.192f};
        float rack_rx{53.7f}, rack_ry{285.7f}, rack_rz{-86.1f};
        float st_rx{}, st_ry{}, st_rz{};
    } m_cslide{};

    struct CDock {
        std::string joint{"_03"};
        float x{0.0f}, y{0.040f}, z{0.060f};
    } m_cdock{};

    struct CMagHand {
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
        float t_rx{}, t_ry{}, t_rz{};
    } m_cmaghand{};

    struct ChicagoCfg {
        bool chicago_enabled{true};
        bool reload_ammo{true};
        bool sound_enabled{true};
        float insert_distance{0.15f};
    } m_ccfg{};

    std::unordered_map<std::string, Bones> m_cposes{};

    struct ChicagoWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* mag_joint{nullptr};
        ::REManagedObject* slide_joint{nullptr};
        std::optional<glm::vec3> slide_rest_lp{};
        std::optional<glm::vec3> rest_lp{};
        std::optional<glm::quat> rest_lr{};
    } m_cwep{};

    struct ChicagoRack {
        bool needs{false};
        bool empty_when_dropped{false};
        bool grab_active{false};
        bool armed{false};
        float frac{0.0f};
        bool pulled{false};
        float gx{}, gy{}, gz{};
        std::optional<float> rgx{}, rgy{}, rgz{};
        float dock_blend{0.0f};
        std::optional<double> _ammo_input_t{};
        bool empty{false};
        bool _chambered_hold{false};
        std::optional<int32_t> _prev_ga{};
        std::optional<int32_t> _loaded{};
        bool tuning{false};
        float tune_frac{0.0f};
        bool dock_tune{false};
        bool has_mag{false};
        bool _zeroed_by_us{false};
    } m_crack{};

    struct ChicagoDrop {
        bool active{false};
        bool use_module{false};
        ::REManagedObject* joint{nullptr};
        float sx{}, sy{}, sz{};
        double t0{0.0};
    } m_cdrop{};

    struct ChicagoInsert {
        bool active{false};
        double t0{0.0};
        float dur{0.18f};
        std::optional<float> dur_base{};
        bool keyframe{false};
        bool snd{false};
        std::optional<glm::vec3> slp{};
        std::optional<glm::quat> slr{};
    } m_cins{};

    bool m_c_mag_hand{false};
    bool m_c_mag_tune{false};
    bool m_c_mag_out{false};
    bool m_c_mag_hidden{false};
    int32_t m_c_mag_retained{0};
    bool m_c_reacquired{false};

    // ---- Save-Load-Reset (Body-Adresse springt) -----------------------
    std::optional<uintptr_t> m_sl_body{};
    void tick_saveload_reset();
    bool m_c_rack_near{false};
    double m_c_mag_floor_at{0.0};
    std::optional<int32_t> m_c_prev_wid{};
    bool m_c_rb_prev{false};
    bool m_c_dry_prev{false};
    PoseFade m_c_hand_fade{};
    glm::vec3 m_c_hand_fade_thumb{0.0f, 0.0f, 0.0f};

    static bool is_chicago(int32_t wid);
    static float c_park_ref(const CSlide& sp);
    void chicago_load_cfg();
    void chicago_save_cfg();
    void chicago_refresh();
    void chicago_on_frame();
    void chicago_apply_pass();
    void chicago_soft_reset();
    void chicago_on_script_reset();
    void chicago_ui();
    bool chicago_set_mag_in_hand(bool active);
    bool chicago_flow() const;

    void cf_snd(uint32_t id);
    ::REManagedObject* cf_get_gun();
    void cf_gun_chamber();
    bool cf_gun_ammo_empty();
    ::REManagedObject* cf_get_wi();
    std::optional<int32_t> cf_loaded();
    int32_t cf_cap();
    int32_t cf_reserve();
    void cf_capture_mag_rest();
    void cf_stop_drop();
    bool cf_start_drop_from(const std::optional<glm::vec3>& p, bool use_module);
    bool cf_force_eject();
    void cf_update_drop();
    bool cf_can_grab();
    void cf_update_mag_in_hand();
    std::optional<glm::vec3> cf_dock_port_world();
    std::optional<glm::vec3> cf_kf_anchor_world();
    bool cf_start_insert();
    void cf_check_insert_proximity();
    void cf_reload_ammo_on_insert();
    void cf_update_insert();
    void cf_clear_rack();
    void cf_update_rack_gesture();
    void cf_update_dock_publish(bool advance);
    void cf_apply_slide_park();
    void cf_apply_mag_out_hidden();
    void cf_apply_hand_pose();

    // ==================================================================
    // 2 -- HANDCANNON-REVOLVER (wp4502, Lua Z.1115-2682)
    // ==================================================================
    struct HcCyl {
        // Seed = seitlicher Swing-Out (rz).
        float rx{0.0f}, ry{0.0f}, rz{-62.0f};
        float px{}, py{}, pz{};
        float lerp{0.08f};
        float shot_deg{-45.0f};
        float shot_lerp{0.20f};
    } m_hcyl{};

    struct HcShell {
        std::string pose{"RevolverShell"};
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
        float t_rx{}, t_ry{}, t_rz{};
        float i_rx{}, i_ry{}, i_rz{};
        std::string parts{"20,30"};
        float scale{1.0f};
    } m_hshell{};

    struct HcHammerCfg {
        float idle_rx{14.5f}, idle_ry{}, idle_rz{};
        float rx{}, ry{}, rz{};
        float lerp{0.25f};
    } m_hham_cfg{};

    struct HcThumbJoint {
        float ix{}, iy{}, iz{};
        float cx{}, cy{}, cz{};
    };

    struct HcThumbKeyJ {
        float x{}, y{}, z{};
        float px{}, py{}, pz{};
    };

    struct HcThumbKey {
        float p{};
        std::array<HcThumbKeyJ, 3> j{};
    };

    struct HcCfg {
        bool revolver_enabled{true};
        float insert_distance{0.15f};
        bool reload_ammo{true};
        bool sound_enabled{true};
        // Spann-Geste: Daumen hoch / KLEBT oben / zurueck (s) + Hahn-Fall-Tempo
        float cock_press{0.18f}, cock_hold{0.14f}, cock_return{0.16f}, cock_fall{0.40f};
        float support_cooldown{0.45f};
    } m_hcfg{};

    struct HcHammerState {
        float hand_frac{0.0f};
        float ham_frac{0.0f};
        bool cocked{false};
        std::optional<int32_t> prev_seq{};
        bool cock_prev{false};
        bool cock_running{false};
        double cock_t0{0.0};
        bool cock_snd{false};
        float cock_stick_y{-0.55f};
        bool use_aim{false};
        struct Off { float rx{}, ry{}, rz{}, px{}, py{}, pz{}; bool preview{false}; };
        Off hand{}, hand_aim{};
        glm::vec3 thumb_pos{}, thumb_pos_aim{};
        float phase{0.0f};
        float key_blend{0.0f};
        // UI
        bool kprev{false};
        bool klive{true};
        float kphase{0.0f};
        int kjoint{1};
        std::array<HcThumbKeyJ, 3> kedit{};
        std::array<HcThumbKeyJ, 3> kout{};
    } m_hham{};

    std::array<HcThumbJoint, 3> m_htcfg{};
    std::array<HcThumbJoint, 3> m_htcfg_aim{};
    std::vector<HcThumbKey> m_htkeys{};
    std::vector<HcThumbKey> m_htkeys_aim{};

    struct HcCylState {
        bool open{false};
        float prog{0.0f};
        bool _prev_b{false};
        bool preview{false};
    } m_hcyl_st{};

    struct HcSpin {
        float target{0.0f};
        float current{0.0f};
        std::optional<int32_t> prev_seq{};
    } m_hspin{};

    struct HcRevState {
        bool cart{false};
        bool _dry_prev{false};
        bool preview{false};
        bool kf_active{false};
        bool kf_used{false};
        bool kf_inserted{false};
        double kf_t0{0.0};
        std::optional<double> kf_hold_t{};
        std::optional<double> _insert_t{};
    } m_hrev{};

    struct HcDrop {
        bool active{false};
        float sx{}, sy{}, sz{};
        double t0{0.0};
        bool snd{false};
    } m_hdrop{};

    struct HcWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* cyl_joint{nullptr};
        std::optional<glm::quat> cyl_rest_rot{};
        std::optional<glm::vec3> cyl_rest_pos{};
        ::REManagedObject* insert_joint{nullptr};
        ::REManagedObject* spin_joint{nullptr};
        std::optional<glm::quat> spin_rest_rot{};
        ::REManagedObject* hand_cart_joint{nullptr};
        ::REManagedObject* hammer_joint{nullptr};
        std::optional<glm::quat> hammer_rest_rot{};

        struct Bullet {
            ::REManagedObject* joint{nullptr};
            std::string name{};
            glm::vec3 vis{1.0f, 1.0f, 1.0f};
        };

        std::vector<Bullet> bullets{};
    } m_hwep{};

    struct HcEjectItem {
        ::REManagedObject* joint{nullptr};
        glm::vec3 vis{};
        float sx{}, sy{}, sz{};
        std::optional<glm::quat> rest_rot{};
        double delay{0.0};
        bool landed{false};
        std::optional<double> land_lt{};
        float wx{}, wy{}, wz{};
    };

    struct HcEject {
        bool active{false};
        double t0{0.0};
        std::vector<HcEjectItem> items{};
        bool armed{true};
        std::optional<glm::vec3> exit{};
        float floor_y{-9999.0f};
    } m_heject{};

    struct HcFlick {
        std::optional<float> prev_fy{};
        std::optional<double> prev_t{};
        double last_close{0.0};
        bool was_open{false};
        double open_t{0.0};
        std::optional<double> snd_at{};
        int peak_dir{0};
        double peak_t{0.0};
    } m_hflick{};

    struct HcCart {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::optional<int32_t> wid{};
        std::string parts_sig{};
        bool parented{false};
        std::string pmode{};   // "hand" oder "weapon"
    } m_hcart{};

    PoseFade m_hshell_fade{};
    bool m_hc_was_managed{false};

    static bool is_handcannon(int32_t wid);
    std::vector<HcThumbKey>& hc_thumb_keys(bool aim);
    static bool hc_thumb_key_sample(const std::vector<HcThumbKey>& list, float p,
                                    std::array<HcThumbKeyJ, 3>& out);
    void hc_load_cfg();
    void hc_save_cfg();
    void hc_refresh_weapon();
    void hc_play_sound(uint32_t id);
    ::REManagedObject* hc_get_live_wi();
    bool hc_gun_is_full(::REManagedObject* wi);
    bool hc_insert_one_round();
    bool hc_can_grab();
    bool hc_set_mag_in_hand(bool active);
    std::optional<glm::vec3> hc_held_cartridge_pos();
    void hc_update_reload();
    void hc_update_cylinder();
    void hc_update_cyl_spin();
    void hc_update_hammer();
    void hc_update_eject();
    void hc_apply_eject();
    void hc_update_close_flick();
    void hc_on_frame();
    void hc_apply_cylinder_pass();
    std::optional<int32_t> hc_get_gun_ammo();
    void hc_apply_bullet_visibility();
    void hc_apply_shell_pose();
    ::REManagedObject* hc_gun_mesh();
    void hc_cart_destroy();
    bool hc_cart_spawn();
    void hc_cart_isolate();
    void hc_cart_parent_mode(const char* mode);
    void hc_cart_set_tf(::REManagedObject* tf, const glm::vec3& p,
                        const std::optional<glm::quat>& rot, float scl);
    void hc_apply_held_cartridge();
    void hc_reposition_cart_late();
    void hc_apply_cart_drop();
    void hc_apply_cylinder_spin_lock();
    void hc_apply_hammer_pass();
    void hc_apply_thumb_pass();
    void hc_apply_pass();
    void hc_on_script_reset();
    void hc_ui();

    // ==================================================================
    // 3 -- ROCKET LAUNCHER (wp4900/4901/4902, Lua Z.2683-3205)
    // ==================================================================
    struct RlCfg {
        bool rl_enabled{true};
        float insert_distance{0.15f};
        bool reload_ammo{true};
        bool sound_enabled{true};
        std::string dock_joint{"_03"};
        float dock_x{0.0f}, dock_y{0.057f}, dock_z{0.155f};
        // Warhead-in-Hand = Mesh-Clone von Part 03 an L_Hand
        std::string parts{"03"};
        float wx{}, wy{}, wz{};
        float wrx{}, wry{}, wrz{};
        float wscale{1.0f};
    } m_rlcfg{};

    struct RlPose {
        std::string pose{"WarheadHold"};
        float t_rx{}, t_ry{}, t_rz{};
        float i_rx{}, i_ry{}, i_rz{};
    } m_rlpose{};

    Bones m_rl_warhead_pose{};

    struct RlWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* warhead_joint{nullptr};
        ::REManagedObject* dock_joint{nullptr};
    } m_rlwep{};

    struct RlState {
        bool cart{false};
        bool preview{false};
    } m_rlst{};

    struct RlClone {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::optional<int32_t> wid{};
        std::string parts_sig{};
        bool parented{false};
    } m_whclone{};

    std::optional<int32_t> m_rl_prev_wid{};
    PoseFade m_rl_fade{};

    static bool is_rl(int32_t wid);
    void rl_load_cfg();
    void rl_save_cfg();
    void rl_play_sound(uint32_t id);
    ::REManagedObject* rl_get_wi();
    bool rl_gun_is_full(::REManagedObject* wi);
    int32_t rl_reserve();
    bool rl_can_grab();
    bool rl_load_one();
    void rl_refresh();
    bool rl_set_mag_in_hand(bool active);
    std::optional<glm::vec3> rl_dock_world();
    void rl_update_insert();
    void rl_apply_pose();
    void rl_on_frame();
    void rl_apply_pass();
    ::REManagedObject* rl_gun_mesh();
    void wh_destroy();
    bool wh_spawn();
    void wh_isolate();
    void wh_place(::REManagedObject* tf);
    void wh_follow();
    void wh_reposition_late();
    void rl_on_script_reset();
    void rl_ui();

    // ==================================================================
    // 4 -- FLAMETHROWER (wp4701, Lua Z.3206-4113)
    // ==================================================================
    struct FtCfg {
        bool ft_enabled{true};
        bool sound_enabled{true};
        // Sicherung _05 (Right-B swing-out, seitlich wie die Handcannon-Trommel)
        float saf_rx{0.0f}, saf_ry{0.0f}, saf_rz{-62.0f}, saf_lerp{0.08f};
        // Tank _04: Dock-Punkt + Einlege-Distanz + Hand-Offset (am Holster)
        float dock_x{}, dock_y{}, dock_z{};
        float insert_distance{0.15f};
        float dock_catch{0.30f};
        float hand_x{}, hand_y{}, hand_z{};
        float hand_rx{}, hand_ry{}, hand_rz{};
        // [KANISTER] Engine-Cap (CurrentAmmoMax) = 1000, aber der "volle"
        // Kanister im Spiel = 100. Also NICHT auf den Engine-Max auffuellen.
        int32_t mag_size{100};
        // Feuersound Tap/Hold
        bool burst_enabled{true};
        float hold_threshold{0.12f};
        // virtuelle Reserve (das Spiel hat keine) + unlimited-Loop
        int32_t reserve{99};
        bool unlimited{true};
        // Damage-Boost
        bool damage_enabled{true};
        float damage_mult{5.0f};
        float damage_floor{30.0f};
        bool damage_native{true};
        // Referenz MP5/TMP (wid 4202): dmg ~240 stop ~495 wince ~400 break
        // ~340-675 -> daran orientiert sich der Attack-Abbruch.
        float damage_stopping{495.0f}, damage_wince{400.0f}, damage_break{400.0f};
        bool damage_hp_fallback{false};
        // Hand-Posen-Offsets
        float fh_rx{}, fh_ry{}, fh_rz{};
        float ca_trx{}, ca_try{}, ca_trz{};
    } m_ftcfg{};

    struct FtWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* safety_joint{nullptr};
        std::optional<glm::quat> safety_rest_rot{};
        ::REManagedObject* tank_joint{nullptr};
    } m_ftwep{};

    struct FtSafety {
        bool open{false};
        float prog{0.0f};
        bool _prev_b{false};
        bool preview{false};
    } m_saf{};

    // phase: idle (Tank in der Kammer, Engine) | dropping | floor | in_hand
    struct FtTank {
        std::string phase{"idle"};
        double t0{0.0};
        float sx{}, sy{}, sz{};
        float floor_y{0.0f};
        bool snd{false};
        std::optional<double> _insert_t{};
        bool preview{false};
        bool _drop_armed{false};
    } m_tank{};

    struct FtFireSnd {
        bool on{false};
        ::REManagedObject* scn{nullptr};
        ::REManagedObject* go{nullptr};
    } m_fire_snd{};

    Bones m_pose_flamehand{}, m_pose_flamesupport{}, m_pose_flamecanister{};

    struct FtPosePrev {
        bool support{false};
        bool canister{false};
    } m_ft_pose_prev{};

    std::optional<int32_t> m_fire_prev_ammo{};
    std::optional<double> m_fire_active_t{};
    std::string m_fire_state{"idle"};
    double m_fire_start_t{0.0};
    bool m_ft_dryfire_prev{false};
    bool m_ft_tank_hidden{false};
    std::optional<int32_t> m_ft_prev_wid{};
    bool m_ft_was_managed{false};
    PoseFade m_ft_fade{};

    // [DAMAGE-BOOST] Spieler-/Partner-HitPoints cachen (nicht selbst grillen).
    double m_ft_pc_t{-999.0};
    std::vector<uintptr_t> m_ft_pc_list{};
    void ft_refresh_player_hp();
    bool ft_is_player_hp(uintptr_t addr);
    static ::REManagedObject* ft_hp_from_go(::REManagedObject* go, int32_t& depth_out);

    static bool is_ft(int32_t wid);
    void ft_load_cfg();
    void ft_save_cfg();
    void ft_play_sound(uint32_t id);
    ::REManagedObject* ft_sound_container(::REManagedObject** go_out);
    std::vector<uint32_t> ft_event_ids_for(::REManagedObject* scn, uint32_t trigid);
    void ft_fire_sound_start();
    void ft_fire_sound_stop();
    void ft_play_burst();
    ::REManagedObject* ft_get_wi();
    int32_t ft_ammo();
    bool ft_reserve_ok() const;
    bool ft_refill();
    void ft_refresh();
    void ft_update_safety();
    void ft_apply_safety();
    void ft_start_drop();
    std::optional<glm::vec3> ft_dock_world();
    void ft_follow_hand(::REManagedObject* j);
    void ft_apply_tank();
    void ft_apply_tank_visibility();
    float ft_hand_dock_dist();
    void ft_do_insert();
    bool ft_set_mag_in_hand(bool active);
    void ft_pose_apply(const Bones& pose,
                       const std::unordered_map<std::string, glm::quat>& offsets,
                       float blend);
    void ft_apply_poses();
    void ft_on_frame();
    void ft_apply_pass();
    void ft_late();
    void ft_on_script_reset();
    void ft_ui();

    // [DAMAGE-BOOST] Handoff PRE -> POST (in Lua ein _stack; hier reichen zwei
    // Zeiger, weil callbackCalculateDamage synchron im selben Thread laeuft).
    ::REManagedObject* m_ft_hit_dv{nullptr};
    ::REManagedObject* m_ft_hit_hp{nullptr};
};

#endif
