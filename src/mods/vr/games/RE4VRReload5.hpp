// ============================================================================
// RE4VRReload5 -- 1:1-Portierung von re4_vr_reload2.lua (6.790 Zeilen).
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

class RE4VRReload5 {
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

private:
    RE4VRReloadMain* m_main{nullptr};
    RE4VRReloadAdv* m_adv{nullptr};

    // ---- geteilte Helfer (Datei-Locals in Lua) ----------------------
    ::REManagedObject* get_ctx();
    std::optional<int32_t> get_equip_wid();
    ::REManagedObject* body_tf();
    ::REManagedObject* get_pe();
    ::REManagedObject* find_weapon(int32_t wid);
    ::REManagedObject* get_live_wi();
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
        std::string pmode{};   // "hand" oder "weapon" (wie RE4VRReload2)
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
    int32_t r9_ratio() const;
    void r9_publish_ratio();
    void red9_load_cfg();
    void red9_save_cfg();
    void red9_on_frame();
    void red9_apply_pass();
    void red9_on_script_reset();
    void red9_ui();
    void draw_public_red9_ratio();
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
    // [BAHN KLEBT AN DER WAFFE] Umhaengen zwischen L_Hand und Waffe,
    // 1:1 wie RE4VRReload2::r9_clip_parent_mode.
    void r9_clip_parent_mode(const char* mode);
    void r9_follow_to_hand();
    std::optional<glm::vec3> r9_dockport_world();
    static std::optional<int32_t> r9_equip_type_main();
    void r9_fill_to_cap();
    void r9_add_single();
    bool r9_grab_allowed();
    void r9_check_insert();
    bool r9_update_insert();
    void r9_update(bool show);

    // ==================================================================
    // 5 -- BLAST CROSSBOW wp6102 (Lua Z.3180-4094)
    // ==================================================================
    // [TOTER CODE -- NICHT PORTIERT] Drei Zustaende der Lua-Fassung werden
    // NIE gesetzt und sind damit wirkungslos (nachgezaehlt: kein einziges
    // Vorkommen einer Zuweisung auf true):
    //   * bclone.in_gun -- seit dem SOFORT-TAUSCH 02.09. bleibt kein Klon
    //     mehr in der Waffe; alle "not in_gun"-Zweige sind konstant wahr.
    //   * bkf.active    -- die Keyframe-Uebergabebahn wird nirgends gestartet
    //     (der Einlege-Zweig setzt sie ausdruecklich auf false).
    //   * bclone.from / bclone.blend -- der Einleg-Blend haengt an in_gun.
    // Die zugehoerigen Schleifen sind hier weggelassen; die CFG-Felder
    // (gun_*, insert_blend) bleiben, weil sie in der JSON stehen und die UI
    // sie weiter anbietet.
    struct BowCfg {
        bool enabled{true};
        bool reload_ammo{true};
        bool sound_enabled{true};
        float draw_smooth{0.5f};
        float dock_blend_speed{0.12f};
        float latch_hold_sec{0.35f};
        float insert_distance{0.15f};
        float insert_x{0.0f}, insert_y{0.040f}, insert_z{0.118f};
        float draw_grab_dist{0.12f};
        float draw_z{-0.37094f};
        float draw_need{0.85f};
        float arrow_x{0.0f}, arrow_y{0.0f}, arrow_z{0.0f};
        float arrow_rx{0.0f}, arrow_ry{0.0f}, arrow_rz{0.0f};
        int32_t arrow_part{5};
        bool arrow_preview{false};
        std::string arrow_pose{"ADAbowbolt"};
        float arrow_trx{0.0f}, arrow_try{0.0f}, arrow_trz{0.0f};
        float gun_x{0.0f}, gun_y{0.0f}, gun_z{0.0f};
        float gun_rx{0.0f}, gun_ry{0.0f}, gun_rz{0.0f};
        float insert_blend{0.18f};
        std::string string_pose{};
        bool string_pose_near{true};
        float dock_x{0.0f}, dock_y{0.0f}, dock_z{0.0f};
        float dock_rx{0.0f}, dock_ry{0.0f}, dock_rz{0.0f};
        bool dock_preview{false};
        float snd_seg{0.12f};
        float snd_drop_delay{0.45f};
    } m_bowcfg{};

    struct BowWep {
        std::optional<int32_t> wid{};
        ::REManagedObject* tf{nullptr};
        ::REManagedObject* sj{nullptr};
        ::REManagedObject* aj{nullptr};
        std::optional<glm::vec3> s_rest{};
        std::optional<glm::vec3> a_rest{};
    } m_bowwep{};

    struct BowState {
        bool arrow_in_hand{false};
        bool loaded{false};
        bool drawn{false};
        float frac{0.0f};
        bool grab{false};
        std::optional<glm::vec3> anchor{};
        std::optional<glm::vec3> wanchor{};
        bool prev_grip{false};
        float dock_blend{0.0f};
        std::optional<double> latch_t{};
        bool prev_dmg{false};
    } m_bowst{};

    // [PART-KLON] sichtbarer Bolzen in der Hand (Mesh-Kopie, Muster wie r9_spawn)
    struct BowClone {
        ::REManagedObject* obj{nullptr};
        ::REManagedObject* mesh{nullptr};
        bool parented{false};
    } m_bclone{};

    struct BowSndState {
        double next_seg{0.0};
        double drop_at{0.0};
        bool dry_prev{false};
    } m_bsnd{};

    // nur fuer die UI-Anzeige (in Lua __re4_bow_insert_dist)
    std::optional<float> m_bow_insert_dist{};

    std::optional<int32_t> m_bow_prev_wid{};
    std::optional<int32_t> m_bow_prev_loaded{};

    static bool is_xbow(int32_t wid);
    void xbow_load_cfg();
    void xbow_save_cfg();
    void xbow_refresh();
    ::REManagedObject* xbow_gun_mesh();
    void xbow_clone_destroy();
    bool xbow_clone_spawn();
    void xbow_clone_pose();
    ::REManagedObject* xbow_wi();
    int32_t xbow_loaded_ammo();
    int32_t xbow_reserve();
    void xbow_snd(uint32_t id);
    void xbow_book_one();
    bool xbow_set_bolt_in_hand(bool active);
    void xbow_apply_pass();
    void xbow_on_frame();
    void xbow_on_script_reset();
    void xbow_ui();

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
    // [FIRE-GATE DLC5] die Bedingungen einmal pro Frame gespiegelt.
    bool m_fg5_block{false};
    double m_fg5_t{0.0};
    void update_fire_gate5();

    // [NACKTES UI] Anmeldung beim Menue-Dispatcher.
    bool m_public_ui_registered{false};
    void ensure_public_ui_registered();

    // Zugang fuer die Hook-Lambdas (in Lua sind das Upvalues der Closures).
    static RE4VRReload5* s_instance;

public:
    static RE4VRReload5* instance() { return s_instance; }
};

#endif
