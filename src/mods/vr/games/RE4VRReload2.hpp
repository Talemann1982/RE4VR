// ============================================================================
// RE4VRReload2 -- 1:1-Portierung von re4_vr_reload2.lua (6.790 Zeilen).
//
// SECHS voellig gekapselte Waffen-Maschinen in EINER Datei. In Lua lebt jede in
// einem eigenen do...end-Block mit eigenem Local-Budget, eigener JSON, eigenen
// Pose-Daten und eigenen Callbacks; geteilt werden nur die Datei-Helfer.
//
//   1 Revolver   4500 Broken Butterfly (+5001)   re4_vr_reload2.json
//   2 Rifle      4401 Stingray, 4402 CQBR        ..._rifle.json
//   3 Bolt       4400 SR M1903                   ..._bolt.json
//   4 Armbrust   4600 Bolt Thrower               ..._xbow.json
//   5 Bowdraw    4600 (von Hand spannen)         re4_vr_bow_draw.json
//   6 Red9       4002                            ..._red9.json
//
// Spezifikation: I:\LUATRANS\PORT_RELOAD2_SPEC.md
//
// REIHENFOLGE: laeuft als ZWEITES der sechs Reload-Teile (nach reload, vor
// reload3). Innerhalb der Datei ist die Reihenfolge der sechs Maschinen je Pass
// ebenfalls festgelegt -- sie entspricht der Registrierungsreihenfolge in Lua.
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
// Voll eingebunden (nicht nur vorwaerts deklariert): die Armbrust deklariert
// RE4VRReloadAdv::Key in ihrer Signatur.
#include "RE4VRReloadAdv.hpp"

class RE4VRReloadMain;

class RE4VRReload2 {
public:
    void on_initialize();
    void install_hooks();
    void on_lua_state_created();
    void on_lua_state_destroyed();

    void on_frame();
    void on_lock_scene_pre();
    void on_update_motion();
    void on_late_update();
    void on_update_joint_expression();
    void on_begin_rendering_pre();
    void on_begin_rendering();
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Public-Block (order 61), gezeichnet von RE4VRMenu ueber RE4VRReload.
    void draw_public_red9_ratio();

    void set_siblings(RE4VRReloadMain* main, RE4VRReloadAdv* adv) {
        m_main = main;
        m_adv = adv;
    }

    // Die Holster-Kette dieser Datei (fuenf Wrapper in Lua). Rueckgabe
    // std::nullopt = keine unserer Waffen -> der Aufrufer reicht an reload
    // weiter.
    std::optional<bool> set_mag_in_hand(bool active);

    // [POSE_FADE] dateiweit: solange `want` gesetzt ist -> (want, 1.0), danach
    // ueber POSE_FADE_DUR auf 0 blenden und dabei den ZULETZT gehaltenen Namen
    // behalten. os.clock-basiert, weil die Apply-Funktionen mehrfach pro Frame
    // laufen.
    struct PoseFade {
        std::string name{};
        std::optional<double> release_t{};
        // `data` der Lua-Fassung: hier je Nutzer ein eigenes Feld.
        bool flag{false};
    };

    static constexpr double POSE_FADE_DUR = 0.10;
    static bool pose_fade_step(PoseFade& f, const std::string& want, float& blend_out);

    // Von Bowdraw nach Lua exportiert: motion ruft es an BEIDEN Pose-Stellen.
    void apply_bowknob_pose();

private:
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

    // Eine Bone-Pose (die Bloecke bringen ihre Daten selbst mit).
    using Bones = std::unordered_map<std::string, glm::quat>;

    // Gemeinsame Pose-Map (in Lua hat JEDER Block seine eigene, inhaltlich
    // identische Kopie -- hier reicht eine, das Ergebnis ist dasselbe).
    ::REManagedObject* m_pmap_tf{nullptr};
    std::unordered_map<std::string, ::REManagedObject*> m_pmap{};
    const std::unordered_map<std::string, ::REManagedObject*>& pose_map();
    bool pose_apply(const Bones& bones, float blend);

    // ==================================================================
    // 1 -- REVOLVER (Lua Z.1-1582)
    // ==================================================================
    struct RevCfg {
        bool revolver_enabled{true};
        float insert_distance{0.15f};
        bool reload_ammo{true};
        bool sound_enabled{true};
        float cock_press{0.18f}, cock_hold{0.14f}, cock_return{0.16f}, cock_fall{0.40f};
        float cock_dur{0.48f};        // [EIN SLIDER] Gesamtdauer der Spann-Geste
        float support_cooldown{0.45f};
    } m_rcfg{};

    // [CYLINDER] Ausschwenken pro Waffe: rx/ry/rz = Crane-Dreh, px/py/pz =
    // Translation (Broken Butterfly faehrt die Trommel raus).
    struct Cyl {
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float lerp{0.08f};
        float shot_deg{-45.0f};       // [CHAMBER-ADVANCE] Grad pro Schuss
        float shot_lerp{0.20f};
    };

    // [SHELL-IN-HAND] Pose + Offsets der Patrone in der Hand.
    struct Shell {
        std::string pose{};
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
        float t_rx{}, t_ry{}, t_rz{};
        float i_rx{}, i_ry{}, i_rz{};
        std::string parts{"20,30"};
        float scale{1.0f};
    };

    // [SINGLE-ACTION] Hahn-Winkel je Waffe (idle <-> cocked).
    struct HammerCfg {
        float idle_rx{}, idle_ry{}, idle_rz{};
        float rx{}, ry{}, rz{};
        float lerp{0.25f};
    };

    // [DAUMEN-KEYS] ein Stuetzpunkt: Phase + drei Glieder (Winkel + Versatz).
    struct ThumbJoint {
        float x{}, y{}, z{};
        float px{}, py{}, pz{};
    };

    struct ThumbKey {
        float p{};
        std::array<ThumbJoint, 3> j{};
    };

    std::unordered_map<int32_t, Cyl> m_cyl{};
    std::unordered_map<int32_t, Shell> m_shell{};
    std::unordered_map<int32_t, HammerCfg> m_hammer_cfg{};
    std::unordered_map<int32_t, std::array<ThumbJoint, 3>> m_tcfg{};      // idle/cocked (2-Punkt, tot)
    std::unordered_map<int32_t, std::array<ThumbJoint, 3>> m_tcfg_aim{};
    std::unordered_map<int32_t, std::vector<ThumbKey>> m_tkeys{};
    std::unordered_map<int32_t, std::vector<ThumbKey>> m_tkeys_aim{};

    Cyl& cyl_cfg(int32_t wid);
    Shell& shell_cfg(int32_t wid);
    HammerCfg& hammer_cfg(int32_t wid);
    std::vector<ThumbKey>& thumb_keys(int32_t wid, bool aim);
    static bool thumb_key_sample(const std::vector<ThumbKey>& list, float p,
                                 std::array<ThumbJoint, 3>& out);

    struct HammerState {
        float hand_frac{0.0f};
        float ham_frac{0.0f};
        bool cocked{false};
        std::optional<int32_t> prev_seq{};
        bool cock_prev{false};
        bool cock_running{false};
        double cock_t0{0.0};
        bool cock_snd{false};
        float cock_stick_y{-0.70f};
        bool use_aim{false};
        bool hold_hand{false};
        bool edit_aim{false};
        // [COCK-HAND] NUR die rechte Hand rollen/versetzen (die Waffe bleibt stehen)
        struct Off { float rx{}, ry{}, rz{}, px{}, py{}, pz{}; };
        Off hand{}, hand_aim{};
        glm::vec3 thumb_pos{}, thumb_pos_aim{};
        float phase{0.0f};
        float key_blend{0.0f};
        // UI
        bool kprev{false};
        bool key_aim{false};
        float kphase{0.0f};
        int kjoint{1};
        int kloaded{0};   // 0 = "-" (welcher Key zuletzt geladen wurde)
        std::array<ThumbJoint, 3> kedit{};
        std::array<ThumbJoint, 3> kout{};
    } m_ham{};

    struct CylState {
        bool open{false};
        float prog{0.0f};
        bool _prev_b{false};
        bool preview{false};
    } m_cyl_st{};

    struct SpinState {
        float target{0.0f};
        float current{0.0f};
        std::optional<int32_t> prev_seq{};
    } m_spin{};

    struct Rev2State {
        bool cart{false};
        bool _dry_prev{false};
        bool preview{false};
        bool kf_active{false};
        bool kf_used{false};
        bool kf_inserted{false};
        double kf_t0{0.0};
        std::optional<double> kf_hold_t{};
        std::optional<double> _insert_t{};
        bool _mih_owned{false};
    } m_rev_st{};

    struct Drop2 {
        bool active{false};
        float sx{}, sy{}, sz{};
        double t0{0.0};
        bool snd{false};
    } m_drop2{};

    struct RevWep {
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
    } m_rwep{};

    // [EJECT]
    struct EjectItem {
        ::REManagedObject* joint{nullptr};
        glm::vec3 vis{};
        float sx{}, sy{}, sz{};
        std::optional<glm::quat> rest_rot{};
        double delay{0.0};
        bool landed{false};
        std::optional<double> land_lt{};
        float wx{}, wy{}, wz{};
    };

    struct EjectState {
        bool active{false};
        double t0{0.0};
        std::vector<EjectItem> items{};
        bool armed{true};
        std::optional<glm::vec3> exit{};
        float floor_y{-9999.0f};
    } m_eject{};

    // [WRIST-FLICK CLOSE]
    struct Flick {
        std::optional<float> prev_fy{};
        std::optional<double> prev_t{};
        double last_close{0.0};
        bool was_open{false};
        double open_t{0.0};
        std::optional<double> snd_at{};
        int peak_dir{0};
        double peak_t{0.0};
    } m_flick{};

    // Mesh-Klon der Patrone in der Hand
    struct CartClone {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::optional<int32_t> wid{};
        std::string parts_sig{};
        bool parented{false};
        std::string pmode{};   // "hand" oder "weapon" (wie RE4VRReload3)
    } m_cart{};

    PoseFade m_shellfade{};
    bool m_capture{false};   // [CAPTURE] toter Schalter, 1:1 uebernommen

    void rev_load_cfg();
    void rev_save_cfg();
    void rev_refresh_weapon();
    static bool is_revolver(int32_t wid);
    void rev_play_sound(std::optional<uint32_t> id);

    ::REManagedObject* get_live_wi();
    bool gun_is_full(::REManagedObject* wi);
    bool insert_one_round();
    bool revolver_can_grab();
    bool revolver_set_mag_in_hand(bool active);
    std::optional<glm::vec3> held_cartridge_pos();
    void update_reload();
    void update_cylinder();
    void update_cyl_spin();
    void update_hammer();
    void update_eject();
    void apply_eject();
    void update_close_flick();
    void rev_on_frame();

    void apply_cylinder_pass();
    std::optional<int32_t> get_gun_ammo();
    void apply_bullet_visibility();
    void apply_shell_pose();
    ::REManagedObject* rev_gun_mesh();
    void cart_destroy();
    bool cart_spawn();
    void cart_isolate();
    // [BAHN KLEBT AN DER WAFFE] Umhaengen zwischen L_Hand und Waffe,
    // 1:1 wie RE4VRReload3::hc_cart_parent_mode.
    void cart_parent_mode(const char* mode);
    // Dasselbe fuer den Red9-Clip (Stripper/Einzelpatrone).
    void r9_clip_parent_mode(const char* mode);
    void cart_set_tf(::REManagedObject* tf, const glm::vec3& p,
                     const std::optional<glm::quat>& rot, float scl);
    void apply_held_cartridge();
    void apply_cart_drop();
    void reposition_cart_late();
    void apply_cylinder_spin_lock();
    void apply_hammer_pass();
    void apply_thumb_pass();
    void apply_revolver_pass();
    void rev_on_script_reset();
    void rev_ui();

    // ==================================================================
    // 2 -- RIFLE (Lua Z.1598-2796)
    // ==================================================================
    struct RSlide {
        float rest_z{0.10042f}, park_z{0.08542f}, back_z{0.06042f};
        float empty_x{0.0f};
        float dock_x{0.045f}, dock_y{0.032f}, dock_z{-0.192f};
        float rack_rx{53.7f}, rack_ry{285.7f}, rack_rz{-86.1f};
        float st_rx{0.0f}, st_ry{0.0f}, st_rz{0.0f};
    };

    struct RDock {
        std::string joint{"_03"};
        float x{0.0f}, y{0.0f}, z{0.090f};
        float insert{0.15f};
        bool ins_loaded{false};
    };

    struct RMagHand {
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
        float t_rx{}, t_ry{}, t_rz{};
    };

    struct RSwitch {
        float rx{0.0f}, ry{0.0f}, rz{-45.0f};
        float lerp{0.18f};
        float grab_dist{0.14f};
        float dx{}, dy{}, dz{};
        float hrx{}, hry{}, hrz{};
    };

    struct RifleCfg {
        bool rifle_enabled{true};
        bool reload_ammo{true};
        bool sound_enabled{true};
        float insert_distance{0.15f};
    } m_rifcfg{};

    std::unordered_map<int32_t, RSlide> m_rslide{};
    std::unordered_map<int32_t, RDock> m_rdock{};
    std::unordered_map<int32_t, RMagHand> m_rmaghand{};
    std::unordered_map<int32_t, RSwitch> m_rswitch{};
    std::unordered_map<std::string, Bones> m_rposes{};

    RSlide& rslide(int32_t wid);
    RDock& rdock(int32_t wid);
    RMagHand& rmaghand(int32_t wid);
    RSwitch& rswitch(int32_t wid);
    static float park_ref(const RSlide& sp);
    static bool is_rifle(int32_t wid);

    struct RifleWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* mag_joint{nullptr};
        ::REManagedObject* slide_joint{nullptr};
        ::REManagedObject* switch_joint{nullptr};
        std::optional<glm::quat> switch_rest_rot{};
        std::optional<glm::vec3> slide_rest_lp{};
        std::optional<glm::vec3> rest_lp{};
        std::optional<glm::quat> rest_lr{};
    } m_rifwep{};

    struct RifleRack {
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
        bool tuning{false};
        float tune_frac{0.0f};
        bool dock_tune{false};
        bool has_mag{false};
        bool _zeroed_by_us{false};
    } m_rifrack{};

    struct RifleDrop {
        bool active{false};
        bool use_module{false};
        ::REManagedObject* joint{nullptr};
        float sx{}, sy{}, sz{};
        double t0{0.0};
    } m_rifdrop{};

    struct RifleInsert {
        bool active{false};
        double t0{0.0};
        float dur{0.18f};
        std::optional<float> dur_base{};
        bool keyframe{false};
        bool snd{false};
        std::optional<glm::vec3> slp{};
        std::optional<glm::quat> slr{};
    } m_rifins{};

    struct RifleSwitchState {
        int stage{0};
        float prog{0.0f};
        bool _prev_trig{false};
        bool _prev_grip{false};
        bool latched{false};
        bool preview{false};
    } m_rifsw{};

    bool m_rif_mag_hand{false};
    bool m_rif_mag_tune{false};
    bool m_rif_mag_out{false};
    int32_t m_rif_mag_retained{0};
    bool m_rif_reacquired{false};
    bool m_rack_near{false};
    bool m_switch_hand{false};
    double m_rif_mag_floor_at{0.0};
    std::optional<int32_t> m_rif_prev_wid{};
    bool m_rif_rb_prev{false};
    bool m_rif_dry_prev{false};
    PoseFade m_hand_fade{};

    bool m_rif_mag_hidden{false};
    glm::vec3 m_hand_fade_thumb{0.0f, 0.0f, 0.0f};

    void rifle_load_cfg();
    void rifle_save_cfg();
    void rifle_refresh();
    void rifle_on_frame();
    void rifle_apply_pass();
    // keep_carry: __re4_mag_carry stehen lassen (s. .cpp)
    void rifle_soft_reset(bool keep_carry = false);
    void rifle_on_script_reset();
    void rifle_ui();
    bool rifle_set_mag_in_hand(bool active);
    bool rifle_flow() const;

    void rf_snd(uint32_t id);
    ::REManagedObject* rf_get_gun();
    void rf_gun_chamber();
    bool rf_gun_ammo_empty();
    ::REManagedObject* rf_get_wi();
    std::optional<int32_t> rf_loaded();
    int32_t rf_cap();
    int32_t rf_reserve();
    void rf_capture_mag_rest();
    void rf_stop_drop();
    bool rf_start_drop_from(const std::optional<glm::vec3>& p, bool use_module);
    bool rf_force_eject();
    void rf_update_drop();
    bool rf_can_grab();
    void rf_update_mag_in_hand();
    std::optional<glm::vec3> rf_dock_port_world();
    std::optional<glm::vec3> rf_kf_anchor_world();
    bool rf_start_insert();
    void rf_check_insert_proximity();
    void rf_reload_ammo_on_insert();
    void rf_update_insert();
    void rf_clear_rack();
    void rf_update_rack_gesture();
    void rf_update_dock_publish(bool advance);
    void rf_apply_slide_park();
    void rf_update_switch();
    void rf_apply_switch();
    void rf_apply_mag_out_hidden();
    void rf_apply_hand_pose();

    // ==================================================================
    // 3 -- BOLT-ACTION (Lua Z.2821-3917)
    // ==================================================================
    struct BoltCfg {
        float back_off{-0.060f};
        float travel{0.12f};
        float open_lerp{0.15f};
        float brx{0.0f}, bry{0.0f}, brz{-70.0f};
        float grab_dist{0.16f};
        float dock_x{}, dock_y{}, dock_z{};
        float hrx{}, hry{}, hrz{};
        float cx{}, cy{}, cz{};
        float crx{}, cry{}, crz{};
        float t_rx{}, t_ry{}, t_rz{};
        std::string cd_joint{"_01"};
        float cd_x{0.0f}, cd_y{0.0f}, cd_z{0.05f};
    };

    struct BoltScalar {
        bool bolt_enabled{true};
        bool reload_ammo{true};
        bool sound_enabled{true};
        float insert_distance{0.15f};
    } m_bscalar{};

    std::unordered_map<int32_t, BoltCfg> m_bcfg{};
    std::unordered_map<std::string, Bones> m_bposes{};
    BoltCfg& bcfg(int32_t wid);
    static bool is_bolt(int32_t wid);

    struct BoltWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* bolt_joint{nullptr};
        ::REManagedObject* cart_joint{nullptr};
        std::optional<glm::quat> bolt_rest_rot{};
        std::optional<glm::vec3> bolt_rest_lp{};
        std::optional<glm::vec3> cart_rest_lp{};
        std::optional<glm::quat> cart_rest_lr{};
    } m_bwep{};

    // [NULLLAGE] pro Waffe EINMAL gemerkt -- nie neu messen.
    std::unordered_map<int32_t, glm::vec3> m_bolt_rest_lp{};
    std::unordered_map<int32_t, glm::quat> m_bolt_rest_rot{};

    struct BoltState {
        bool open{false}, grab{false}, armed{false};
        std::string mode{"open"};
        bool locked{false};
        float roll{0.0f}, zf{0.0f}, troll{0.0f}, tzf{0.0f};
        float gx{}, gy{}, gz{};
        std::optional<float> rgx{}, rgy{}, rgz{};
        float zf_anchor{0.0f};
        float dock_blend{0.0f};
        bool preview{false}, preview_open{false};
        double lock_t{0.0};
        bool needs_cycle{false};
        std::optional<int32_t> _prev_loaded{};
    } m_bolt{};

    struct BoltCart {
        bool active{false}, insert{false}, tune{false};
        double t0{0.0};
        float dur{0.18f};
        bool snd{false};
        bool _kf_forced{false};
        std::optional<glm::vec3> slp{};
        std::optional<glm::quat> slr{};
    } m_bcart{};

    bool m_bolt_reacquired{false};
    std::optional<int32_t> m_bolt_prev_wid{};
    bool m_bolt_dry_prev{false};
    double m_bolt_in_cycle_until{0.0};

    // [BOLT_MUTE] Spiegel der Lua-Globals -- der trigger-Hook darf pro Aufruf
    // nicht in den Lua-State greifen (er feuert fuer JEDEN SoundContainer).
    double m_bolt_mute_until{0.0};
    uintptr_t m_bolt_gun_addr{0};
    bool m_bolt_snd_self{false};

    void bolt_load_cfg();
    void bolt_save_cfg();
    void bolt_refresh();
    void bolt_on_frame();
    void bolt_cycle_suppress();
    void bolt_apply_pass();
    void bolt_soft_reset();
    void bolt_on_script_reset();
    void bolt_ui();
    bool bolt_set_cart_in_hand(bool active);

    void bsnd(uint32_t id);
    ::REManagedObject* bget_wi();
    std::optional<int32_t> bloaded();
    int32_t bcap();
    int32_t breserve();
    void bolt_chamber();
    void bolt_capture_cart_rest();
    void bolt_settle_step(const BoltCfg& c);
    void update_bolt_gesture();
    void apply_bolt_joint();
    void update_bolt_dock(bool advance);
    void update_cart_in_hand();
    std::optional<glm::vec3> bolt_chamber_world();
    void bolt_add_one();
    bool start_cart_insert();
    void bolt_check_insert_proximity();
    void update_cart_insert();
    bool bolt_can_load();
    void apply_bolt_pose();

    // ==================================================================
    // 4 -- ARMBRUST (Lua Z.3930-4631)
    // ==================================================================
    struct XbowCfg {
        float t_rx{}, t_ry{}, t_rz{};
        std::string cd_joint{"_03"};
        float cd_x{0.0f}, cd_y{0.0f}, cd_z{0.138f};
        float dx{}, dy{}, dz{};
        float drx{}, dry{}, drz{};
        float dscale{1.0f};
    };

    struct XbowScalar {
        bool enabled{true};
        bool reload_ammo{true};
        float insert_distance{0.15f};
        std::string hand_parts{"5"};
    } m_xscalar{};

    std::unordered_map<int32_t, XbowCfg> m_xcfg{};
    Bones m_xbow_pose{};
    XbowCfg& xcfg(int32_t wid);
    static bool is_xbow(int32_t wid);

    struct XbowState {
        bool active{false}, insert{false}, tune{false};
        double t0{0.0};
        float dur{0.40f};
    } m_arrow{};

    struct XbowDrop {
        bool active{false};
        float sx{}, sy{}, sz{};
        double t0{0.0};
    } m_xdrop{};

    struct XbowWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
    } m_xwep{};

    bool m_xprev{false};
    int m_xdummy_req{0};
    double m_xkf_hold{0.0};
    bool m_xkf_prev_ins{false};
    std::optional<int32_t> m_xprev_wid{};
    PoseFade m_xbow_fade{};

    // [HAND-DUMMY] Der Bolzen. In Lua Globals (__re4_xbow_dummy_*), damit sie
    // "Reset Scripts" ueberleben -- hier Member; der Hook liegt im selben Objekt.
    ::REManagedObject* m_xdummy_obj{nullptr};
    bool m_xdummy_await{false};
    bool m_xdummy_parented{false};

    void xbow_load_cfg();
    void xbow_save_cfg();
    void xbow_refresh();
    void xbow_on_frame();
    void xbow_apply_pass();
    void xbow_reposition_late();
    void xbow_soft_reset();
    void xbow_on_script_reset();
    void xbow_ui();
    bool xbow_set_arrow_in_hand(bool active);

    ::REManagedObject* xget_gun();
    ::REManagedObject* xget_gun_tf();
    void xplay_sound(uint32_t id);
    ::REManagedObject* xget_wi();
    std::optional<int32_t> xloaded();
    int32_t xcap();
    int32_t xreserve();
    static ::REManagedObject* xfind_child(::REManagedObject* go, const char* name);
    ::REManagedObject* xget_generator();
    void xrequest_dummy();
    bool xdummy_set_parent(::REManagedObject* dummy, bool on);
    void xupdate_dummy(bool show);
    void xstart_drop();
    bool xcan_load();
    bool xkf_active(RE4VRReloadAdv::Key& out);
    bool xkf_place_dummy();
    std::optional<glm::vec3> xchamber_world();
    void xadd_one();
    void xcheck_insert_proximity();
    void xupdate_arrow_insert();
    void apply_xbow_pose();

    // ==================================================================
    // 5 -- BOWDRAW (Lua Z.4652-5388)
    // ==================================================================
    struct BowCfg {
        float dx{-0.06609f}, dy{0.18747f}, dz{-0.11905f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
        float adx{-0.06609f}, ady{0.18747f}, adz{-0.11905f};
        float arx{0.0f}, ary{0.0f}, arz{0.0f};
        float grab_dist{0.12f};
        float pull_travel{0.19997f};
        float part_at{0.50f};
        float snd_at{0.70f};
        float snap_dur{0.04f};
        float aim_mute{1.00f};
        float dock_lerp{0.15f};
    } m_bow{};

    struct BowState {
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* j01{nullptr};
        ::REManagedObject* j04{nullptr};
        ::REManagedObject* j05{nullptr};
        ::REManagedObject* j06{nullptr};
        ::REManagedObject* j07{nullptr};
        bool grabbed{false}, cocked{false};
        float p{0.0f}, draw{0.0f};
        std::optional<float> anchor{};
        float p0{0.0f};
        std::optional<double> snap_t{};
        float snap_from{0.0f};
        std::optional<int32_t> shot_seq{};
        bool snd_armed{true};
        std::optional<int32_t> rt_press{};
        bool edit_aim{false};
        bool has_bow{false};
        bool aim_prev{false};
        float blend{0.0f};
        std::optional<double> blend_t{};
        bool preview{false};
        bool pushed_dock{false};
        std::string msg{"-"};
        ::REManagedObject* mesh{nullptr};
        std::optional<int32_t> part_now{};
    } m_bow_st{};

    Bones m_bowknob{}, m_bow_aim{};

    // [AIM-SOUND MUTEN] + Feuersperre: Spiegel der Lua-Globals fuer die Hooks.
    double m_bow_mute_until{0.0};
    uintptr_t m_bow_gun_addr{0};
    bool m_bow_snd_self{false};
    bool m_bow_fire_block{false};
    double m_bow_gate_t{0.0};

    void bow_load_cfg();
    void bow_save_cfg();
    bool bow_resolve();
    void bow_drop_refs();
    std::optional<int32_t> bow_live_wid();
    void bow_play_sound(uint32_t id);
    std::optional<int32_t> bow_loaded_ammo();
    bool bow_use_aim();
    std::optional<glm::vec3> bow_dock_pos();
    std::optional<glm::quat> bow_dock_rot();
    std::optional<float> bow_hand_dist();
    void bow_update_gesture();
    void bow_publish_fire_gate();
    void bow_watch_rt_empty();
    void bow_watch_shot();
    void bow_update_aim_mute();
    void bow_update_dock_blend();
    bool bow_publish_dock();
    void bow_apply_joints();
    void bow_set_part(int32_t idx);
    ::REManagedObject* bow_weapon_mesh();
    void bow_on_frame();
    void bow_on_script_reset();
    void bow_ui();

    // ==================================================================
    // 6 -- RED9 (Lua ~Z.5790-6764)
    // ==================================================================
    struct Red9Cfg {
        bool enabled{true};
        bool reload_ammo{true};
        std::string parts{"5,20,21"};
        std::string single_part{"22"};
        float dx{}, dy{}, dz{};
        float drx{}, dry{}, drz{};
        float dscale{1.0f};
        float t_rx{}, t_ry{}, t_rz{};
        float i_rx{}, i_ry{}, i_rz{};
        // Einzelpatrone (eigener Satz)
        float sdx{}, sdy{}, sdz{};
        float sdrx{}, sdry{}, sdrz{};
        float sdscale{1.0f};
        float st_rx{}, st_ry{}, st_rz{};
        float sti_rx{}, sti_ry{}, sti_rz{};
        float s_thumb_str{0.0f}, s_index_str{0.0f};
        // Slide-Rack
        std::string slide_joint{"_01"};
        float rack_z{-0.030f};
        float rack_grab{0.14f};
        // Dock-Versatz der Hand relativ zum Slide (Punisher-Start)
        float dock_x{0.045f}, dock_y{0.032f}, dock_z{-0.192f};
        float rack_rx{53.7f}, rack_ry{285.7f}, rack_rz{-86.1f};
        float si_rx{}, si_ry{}, si_rz{};
        // Insert
        float insert_dist{0.12f};
        float s_insert_dist{0.12f};
        float insert_dur{0.40f};
        float insert_drop{0.12f};
        std::string ip_joint{"_03"};
        float ip_x{}, ip_y{}, ip_z{0.054f};
        int32_t shell_ratio{1};
        float regrip_cd{0.25f};
    } m_r9cfg{};

    struct Red9Clip {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        std::string parts_sig{};
        std::string mode{"strip"};
        bool parented{false};
        std::string pmode{};   // "hand" oder "weapon"
    } m_r9clip{};

    struct Red9State {
        bool active{false};
        bool preview{false};
        bool preview_single{false};
    } m_r9state{};

    struct Red9Rack {
        ::REManagedObject* joint{nullptr};
        std::optional<float> rest_z{};
        bool grabbed{false};
        bool tune{false};
        bool open{false};
        bool settling{false};
        float dock_blend{0.0f};
        float _dock_want{1.0f};
        bool _armed_up{true};
        bool _armed_dn{false};
        std::optional<float> _last_apply_z{};
        bool _need_regrip{false};
        double _grab_cd{0.0};
        float grab_base{0.0f};
        std::optional<float> anchor_z{};
        double settle_t0{0.0};
        float settle_from_z{0.0f}, settle_to_z{0.0f};
    } m_r9rack{};

    struct Red9Insert {
        bool active{false};
        double t0{0.0};
        float sx{}, sy{}, sz{};
        float lx{}, ly{}, lz{};
        bool local_ok{false};
        std::optional<double> kf_hold_t{};
        bool kf_lead{false};
    } m_r9ins{};

    struct Red9Drop {
        bool active{false};
        bool snd{false};
        float sx{}, sy{}, sz{};
        double t0{0.0};
    } m_r9drop{};

    std::unordered_map<std::string, Bones> m_r9poses{};
    std::optional<int32_t> m_r9_prev_wid{};
    bool m_r9_dry_prev{false};
    PoseFade m_r9_fade{};

    // [REST-KONSTANTE] feste Slide-Nulllage (Slide ZU) am wp4002-_01, live
    // verifiziert. BEWUSST nicht ueber JSON kalibrierbar -- eine live gemessene
    // Nulllage faengt beim Leer-Ziehen die Empty-Lock-Position ein.
    static constexpr float R9_REST_Z = 0.0267f;

    std::optional<float> m_r9_apply_z{};
    bool m_r9_kf_preview{false};

    static bool is_red9(int32_t wid);
    void r9_publish_ratio();
    void red9_load_cfg();
    void red9_save_cfg();
    void red9_on_frame();
    void red9_apply_pass();
    void red9_on_script_reset();
    void red9_ui();
    bool red9_set_in_hand(bool active);

    ::REManagedObject* rget_gun();
    ::REManagedObject* rget_gun_mesh();
    ::REManagedObject* rget_gun_tf();
    void r9_play_sound(uint32_t id);
    void r9_apply_pose(const std::string& name, bool with_thumb, float blend);
    void r9_destroy();
    bool r9_spawn();
    void r9_isolate();
    bool r9_apply_drop();
    void r9_start_drop();
    ::REManagedObject* rget_slide();
    void rset_slide_z(float z);
    void r9_publish_dock(::REManagedObject* j);
    std::optional<glm::vec3> r9_hand_gun_local(const glm::vec3& p);
    void update_slide_rack();
    void r9_set_tf(::REManagedObject* tf, const glm::vec3& p,
                   const std::optional<glm::quat>& rot, float s);
    void r9_follow_to_hand();
    std::optional<glm::vec3> r9_dockport_world();
    static std::optional<int32_t> r9_equip_type_main();
    void r9_fill_to_cap();
    void r9_add_single();
    bool r9_grab_allowed();
    void r9_check_insert();
    bool r9_update_insert();
    void r9_update(bool show);

    // ---- Mercs-Runden-Reset ------------------------------------------
    std::optional<int32_t> m_round_seen{};
    void tick_merc_round();

    // ---- Save-Load-Reset (Body-Adresse springt) -----------------------
    std::optional<uintptr_t> m_sl_body{};
    void tick_saveload_reset();

    // ---- [BODY-EPOCH 2026-09-22] re4vr::body_epoch() ------------------
    // Ergaenzt den Save-Load-Reset: faengt auch "Body weg, gleiche Adresse
    // kommt wieder" und die Waffen-Zeiger, die der Reset oben nicht leert.
    uint64_t m_body_epoch{0};
    void tick_body_epoch();
    void drop_body_caches();

    // ---- Fuer die Hooks gespiegelte Lua-Globals -----------------------
    // isEnableFire und SoundContainer.trigger feuern sehr oft; ein Lua-Zugriff
    // pro Aufruf waere dutzendfaches Sperren des sol-States.
    bool m_frame_is_gameplay{false};
    bool m_bow_draw_off{false};

    // [NACKTES UI] Anmeldung beim Menue-Dispatcher.
    bool m_public_ui_registered{false};
    void ensure_public_ui_registered();

    // Zugang fuer die Hook-Lambdas (in Lua sind das Upvalues der Closures).
    static RE4VRReload2* s_instance;

public:
    static RE4VRReload2* instance() { return s_instance; }
};

#endif
