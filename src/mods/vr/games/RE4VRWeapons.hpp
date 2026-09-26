// ============================================================================
// RE4VRWeapons -- 1:1-Portierung von re4_vr_weapons.lua (5.188 Zeilen).
//
// Spezifikation: I:\LUATRANS\PORT_WEAPONS_SPEC.md
//
// Die Lua hat KEINE IIFE-Kapselung -- sie ist EIN Chunk am 200-Local-Limit.
// Deshalb sind dort ~12 Funktionen bewusst als _G-Globals definiert, nur um
// Locals zu sparen. Hier werden daraus normale Member; die Globals BLEIBEN
// zusaetzlich exportiert, solange der Reload-Block Lua ist.
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- hinter dem ScriptRunner, direkt VOR
// RE4VRWeapons2 (Lua-Reihenfolge weapons < weapons2).
//
// Die Platzierung ist unkritisch, und das ist gemessen: die Kopplung zum
// Reload-Block besteht in BEIDE Richtungen nur aus zwei Schaltern
// (__re4_fc_off, __re4_logging_on). Die 29 echten Leser sitzen in den bereits
// portierten Modulen -- und motion steht VOR dem ScriptRunner, sieht die
// weapons-Globals also weiterhin einen Pass alt, genau wie in Lua (m < w).
// ----------------------------------------------------------------------------
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RE4VR.hpp"

class RE4VRWeapons : public Mod {
public:
    static std::shared_ptr<RE4VRWeapons>& get();

    std::string_view get_name() const override { return "RE4VRWeapons"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Public-Block: "Disable Assist Light" (RE4VRMenu).
    void draw_public_assist_light();

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // ------------------------------------------------------------------
    // Von RE4VRWeapons2 (und anderen nativen Modulen) direkt gerufen.
    // Die Lua-Globals bleiben zusaetzlich exportiert, solange der
    // Reload-Block Lua ist.
    // ------------------------------------------------------------------
    ::REManagedObject* find_knife_hc();
    ::REManagedObject* knife_get_attack_ud(::REManagedObject* hc);
    // [ADA-TIER 17.09.2026] Fuer Messer OHNE eigene AttackUserData (Adas wp6108):
    // HitController des Spielerkoerpers + dessen erste AttackUserData -- derselbe
    // Koerper-HitController, ueber den auch die Gegnertreffer laufen.
    bool body_attack_fallback(::REManagedObject*& hc_out, ::REManagedObject*& atk_out);
    int32_t break_nearby(const std::optional<glm::vec3>& kp_override,
                         const std::optional<float>& radius_override,
                         const std::optional<float>& max_dy);
    float nearest_bone_dist(::REManagedObject* root_tf, const glm::vec3& kp);
    void do_knife_melee();
    bool los_clear(const glm::vec3& from, const glm::vec3& to);
    std::optional<glm::vec3> knife_hand_world();

private:
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    static void store(Handle& h, ::REManagedObject* obj, bool unconditional = false);
    static void drop(Handle& h);

    bool ensure_init();
    bool m_inited{false};
    bool m_hooks_installed{false};

    // ==================================================================
    // Bausteine 1-2: Helfer + SCOPE (Lua Z.1-538)
    // ==================================================================
    std::optional<int32_t> get_equip_weapon_id();
    bool is_any_menu_open();
    bool check_via_scope();
    void detect_scope_id();
    void scope_killswitch_tick();
    bool gun_bolt_cycle_active(int32_t ewid);

    Handle m_attache_mgr{};
    Handle m_gui_mgr{};

    bool m_scope_via_seen{false};
    int32_t m_scope_id_throttle{0};

    // [SCOPE_BULLET] Vorzeichen der Scope-Kamera-Forward-Achse.
    static constexpr float SCOPE_BULLET_ZSIGN = -1.0f;

    // ==================================================================
    // Baustein 3: IRON_SIGHT (Lua Z.219-304)
    // ==================================================================
    void iron_sight_tick(int32_t wid);
    void iron_force_body_visible();
    void iron_force_weapon_mesh(int32_t wid);
    void iron_ensure_parented(int32_t wid);
    ::REManagedObject* iron_find_weapon_go(int32_t wid);
    void iron_walk_body_visible(::REManagedObject* tf, int32_t depth);

    std::optional<int32_t> m_iron_active_wid{};

    // ==================================================================
    // Baustein 4: SCOPE_PROTO (Lua Z.541-602)
    // ==================================================================
    struct ScopeProto {
        float scale{1.0f};
        float ox{0.0f}, oy{0.0f}, oz{0.0f};
    } m_scope_proto{};

    void scope_proto_tick();
    void save_scope_proto();
    ::REManagedObject* scope_get_gun_go();

    // ==================================================================
    // Baustein 5: hide_body_weapons (Lua Z.604-666)
    // ==================================================================
    void hide_body_weapons_tick();
    void save_hide_body_cfg();

    bool m_hide_body_enabled{true};

    // ==================================================================
    // Baustein 6: Messer-Holster / KKO / Quick-Knife / STOW-GUARD
    // (Lua Z.668-1054)
    // ==================================================================
    ::REManagedObject* get_head_updater();
    ::REManagedObject* get_knife_timer();
    bool holster_knife();
    void update_knife_holster_timer();
    void keep_knife_out();

    void install_equip_hooks();

    // Callbacks der fuenf Hooks (Rueckgabe true = SKIP_ORIGINAL)
    bool cb_change_weapon_action();
    bool cb_request_equip_knife();
    bool cb_stow_guard();
    bool cb_equip_weapon(uintptr_t raw_wid);

    struct KnifeHolsterState {
        int32_t force_frames{0};
        std::optional<float> original_time_limit{};
    } m_kh{};

    // [KNIFE_KEEP_OUT] Toggle, im Original eine Code-Konstante.
    static constexpr bool KNIFE_KEEP_OUT = true;

    // ==================================================================
    // Baustein 7: Assist Light (Lua Z.1056-1160)
    // ==================================================================
    void hide_assist_light();
    void sync_assist_light_rotation_to_vr_cam();
    bool get_assist_light_go_tf(::REManagedObject*& go_out, ::REManagedObject*& tf_out);

    struct AssistLightCache {
        std::optional<uintptr_t> body_addr{};
        Handle go{};
        Handle tf{};
    } m_al{};

    bool m_hide_assist_light{false};
    bool m_assist_light_follow_vr_cam{true};

    // ==================================================================
    // Baustein 8: SNAPPY_WEP + WEAPON_SWITCH_SKIP (Lua Z.1162-1310)
    // ==================================================================
    struct SnappyCfg {
        bool enabled{true};
        float frameskip{35.0f};
        bool fast_re_aim{true};
        bool direct_snap{true};
    } m_snappy{};

    void save_snappy_cfg();
    bool apply_snappy(bool revert_all);
    void snappy_wep_tick();
    void weapon_switch_skip_tick();
    ::REManagedObject* get_mfsm2();
    bool wsw_get_comps(::REManagedObject*& fsm_out, ::REManagedObject*& motion_out);

    bool m_snappy_applied{false};
    std::optional<uintptr_t> m_snappy_last_body{};

    struct WswCache {
        Handle body{};
        Handle fsm{};
        Handle motion{};
    } m_wsw{};

    static constexpr int32_t WSW_LAYER = 4;

    // ==================================================================
    // Baustein 11-14: KNIFE_MELEE (Lua Z.1471-1902)
    // ==================================================================
    ::REManagedObject* get_hc(::REManagedObject* go);
    std::optional<int32_t> weapon_id_num(::REManagedObject* hc);

    // Grip der Hand, die das Messer gerade haelt (Lua Z.3945). Ersetzt das mit
    // dem Port verschwundene Global __re4_knife_grip_held.
    bool m_bolt_reaim{true};

    bool knife_grip_held();

public:
    // [WAISE 05.09.2026] Die beiden Wurf-Korrekturen lagen in Lua in der
    // Tabelle _G.__re4_throw_cfg, und re4_vr_motion.lua zeichnete ihre Slider
    // daraus. Nativ liegen sie in m_tcfg -- ohne diese Zugriffe faende
    // RE4VRMotion die Tabelle nicht und zeichnete die Slider gar nicht erst.
    float throw_knife_pitch() const { return m_tcfg.knife_pitch; }
    float throw_knife_yaw() const { return m_tcfg.knife_yaw; }
    void set_throw_knife_pitch(float v) { m_tcfg.knife_pitch = v; }
    void set_throw_knife_yaw(float v) { m_tcfg.knife_yaw = v; }
    void save_throw_cfg_public() { save_throw_cfg(); }

private:
    ::REManagedObject* find_wp_hc(::REManagedObject* tf, int32_t depth,
                                  std::optional<int32_t> want_id, bool allow_holstered);
    bool hc_still_active(::REManagedObject* hc, std::optional<int32_t> want_id);
    void play_knife_sound(int32_t id);
    ::REManagedObject* knife_pick_target(const std::optional<glm::vec3>& kpos,
                                         bool proximity_only,
                                         const std::optional<float>& reach);
    std::optional<glm::vec3> knife_obj_pos();
    ::REManagedObject* borrow_knife_atk();
    void iter_hitctrl_gos(const std::function<void(::REManagedObject*)>& cb);

    Handle m_hitmgr{};
    Handle m_knife_hc_cache{};
    double m_knife_hc_refresh_t{0.0};
    Handle m_knife_snd_last{};

    bool m_knife_melee_prev{false};
    double m_last_knife_melee_t{0.0};

    // [KNIFE_REACH] / [B] / [KNIFE_DMG] -- Code-Defaults, die Globals
    // ueberleben einen Reset.
    static constexpr float KNIFE_REACH_DEF = 0.90f;
    static constexpr float BODY_CENTER_Y = 0.95f;
    static constexpr int32_t KNIFE_ATK_IDX = 5;

    // ==================================================================
    // Baustein 12/16: ZIELHILFE (Lua Z.1612-1735, 2330-2517)
    // ==================================================================
    bool enemy_aim_point(::REManagedObject* ctx, glm::vec3& out);
    bool knife_assist_target(const glm::vec3& from, const glm::vec3& dir, float max_dist,
                             float cone_deg, glm::vec3& out, ::REManagedObject*& ctx_out);
    bool knife_assist_breakable(const glm::vec3& from, const glm::vec3& dir, float max_dist,
                                float cone_deg, glm::vec3& out, float& near_out);
    bool wb_dead(::REManagedObject* wb);
    bool wb_of(::REManagedObject* go, ::REManagedObject*& wb_out, ::REManagedObject*& dur_out,
               ::REManagedObject*& holder_out);

    // [PORTFIX 2026-09-06] Der vorgefertigte Filter
    // CollisionUtil.Filter.DamageCheckOtherThanPlayer -- den nimmt die Lua fuer
    // den Damage-Strahl (weapons.lua Z.2705). Gepinnt, weil er aus einem
    // statischen Feld kommt und sonst recycelt werden darf.
    Handle m_dmg_filter{};
    bool m_dmg_filter_done{false};
    std::string m_dmg_filter_how{"-"};   // [MESSUNG] welcher Leseweg gegriffen hat

    Handle m_los_res{};     // persistentes, gepinntes CastRayResult
    Handle m_los_query{};   // dito Query -- s. [PORTFIX 2026-09-06] in los_clear

    // ==================================================================
    // Baustein 13: Daten-Caching aus nativen Treffern (Lua Z.1737-1791)
    // ==================================================================
    void install_hitctrl_hooks();
    void cb_pool_damage(uintptr_t atk, uintptr_t dmg);
    void cb_attack_hit(::REManagedObject* hc);
    void cb_calculate_damage(::REManagedObject* dv, ::REManagedObject* ci);

    uintptr_t m_lp_atk{0};
    uintptr_t m_lp_dmg{0};
    Handle m_knife_atk_ud{};
    Handle m_knife_dmg_ud{};

    // ==================================================================
    // Baustein 15: BREAKABLES (Lua Z.1904-2328)
    // ==================================================================
    void clone_break_sound(::REManagedObject* start_go);
    void break_sound_once(::REManagedObject* go);
    std::optional<bool> hit_in_mesh_box(::REManagedObject* go, const glm::vec3& kp, float margin);
    std::optional<int32_t> get_break_routine();

    std::unordered_map<uintptr_t, double> m_brk_snd_t{};
    int32_t m_brk_snd_n{0};

    // ==================================================================
    // Baustein 18: KNIFE_THROW / FLIGHT (Lua Z.2559-3886)
    // ==================================================================
    struct KFly {
        bool active{false};
        Handle go{};
        Handle tf{};

        glm::vec3 pos{};
        glm::vec3 vel{};
        glm::vec3 start_pos{};
        glm::vec3 axis{};
        glm::vec3 axis_fall{};
        glm::quat base_rot{1.0f, 0.0f, 0.0f, 0.0f};

        float ang{0.0f};
        std::string phase{};   // "fly" | "rest"
        bool hit_done{false};
        std::optional<double> hit_t{};
        float flat{0.0f};

        std::optional<glm::vec3> home{};
        Handle home_ctx{};
        float home_str{0.0f};

        bool clone_throw{false};
        bool is_left{false};

        // Raycast
        Handle ray{};
        Handle ray_wall{};
        // [A/B-CAST 2026-09-06] Zweiter Damage-Strahl, identisch aufgebaut, aber
        // ueber den ROHEN Aufruf (call_safe) statt ueber invoke gefeuert -- so
        // wie RE4VRCrosshair es seit jeher macht. Nur zur Messung.
        Handle ray_alt{};
        Handle ctrl_res{};   // [KONTROLL-STRAHL] Lua-Test-Geometrie
        Handle ctrl_q{};
        // [PORTFIX 2026-09-06] Die beiden CastRayQuery-Objekte werden jetzt
        // GEHALTEN und gepinnt. Vorher entstanden sie pro Schuss frisch und
        // ungepinnt -- bei einem ASYNCHRONEN Ray arbeitet die Engine sie erst
        // spaeter ab, und ein nicht verankertes Objekt ist bis dahin schon
        // eingesammelt: Access Violation in castRayAsync (im Log belegt).
        // In Lua haelt die lokale Variable eine echte Referenz (sol_lua_push
        // macht add_ref unsichtbar mit), dort kann das nicht passieren.
        // Die jeweils ZULETZT benutzte Query -- sie wird erst freigegeben,
        // wenn die naechste sie ersetzt. Damit ueberlebt sie den asynchronen
        // Auftrag, ohne dass eine Query mehrfach benutzt wird (das Original
        // erzeugt sie pro Aufruf frisch und laesst den Lua-GC aufraeumen).
        Handle q_prev{};
        Handle q_wall_prev{};
        Handle q_alt_prev{};
        bool ray_pending{false};
        // [SOFORT-AUSWERTUNG 2026-09-07] Das Ergebnis des synchronen Casts wird
        // im SELBEN Aufruf gelesen und hier abgelegt. Einen Frame spaeter ist
        // das CastRayResult leer -- der Kontrollstrahl (casten + sofort lesen)
        // war der einzige Strahl im Port, der je Kontakte gemeldet hat.
        bool rr_have{false};
        float rr_hd{0.0f};
        float rr_ny{0.0f};
        bool rr_wall{false};
        Handle rr_go{};
        glm::vec3 ray_from{};
        glm::vec3 ray_dir{};
        float ray_len{0.0f};

        bool wall_hit{false};
        float min_enemy_d{1e9f};
        float min_bone_d{1e9f};

        // [WURF STECKEN]
        bool stuck{false};
        Handle stick_parent{};
        std::string stick_joint{};
        Handle stick_etf{};
        std::string stick_bone{};
        std::optional<glm::vec3> stick_ip{};
        std::optional<double> check_at{};
        std::optional<bool> stick_ok{};
        std::optional<float> stick_raw_d{};
        std::optional<float> blade_len{};

        bool detached{false};
        bool hidden{false};
        Handle saved_parent{};
        Handle home_parent{};
        std::string home_joint{};

        float floor_y{0.0f};
        double last_t{0.0};
        double min_fly_until{0.0};
        double end_t{0.0};
        double return_t{0.0};
        bool snd_return{false};
    } m_kfly{};

    // [MESSER-HEIMAT 2026-09-12] s. knife_home_guard.
    struct KnifeHome {
        Handle tf{};        // das Messer-Transform selbst
        Handle parent{};    // wo es zuletzt GESUND hing
        std::string joint{};// dessen Parent-Joint (leer = direkt am Parent)
        std::string name{}; // GO-Name, nur fuers Log
        double lost_since{0.0};   // seit wann parentlos (0 = haengt)
        double tiny_since{0.0};   // seit wann auf Scale ~0 (0 = normal gross)
        bool learned{false};      // Heimat schon einmal gemeldet
        int fails{0};             // vergebliche Rueckhaeng-Versuche in Folge
        double alien_since{0.0};  // seit wann in einem FREMDEN Baum (0 = daheim)
    };

    std::vector<KnifeHome> m_knife_home{};
    double m_knife_home_scan{0.0};
    double m_knife_home_fix{0.0};

    // [RELOAD/TOD 12.09.2026] Der Koerper-Transform der Sitzung, an dem gelernt
    // wurde -- ROHER Vergleichszeiger, BEWUSST kein Handle: er wird nie
    // dereferenziert, nur verglichen. Wechselt er (Save-Load, Tod, Raumwechsel),
    // zeigen alle gemerkten Messer auf die alte Szene und muessen weg.
    ::REManagedObject* m_knife_home_body{nullptr};

    // Ab wann nach so einem Wechsel wieder gelernt werden darf.
    double m_knife_home_ready{0.0};

    // [WURF-KLON 12.09.2026] Die Kopie, die im Gegner stecken bleibt. BEWUSST
    // ausserhalb von KFly: sie ueberlebt den Flug um Minuten, waehrend m_kfly
    // beim naechsten Wurf neu belegt wird. Und bewusst ein EIGENER Klon statt
    // des Choke-Klons -- sonst loescht ein Wurf das Messer aus dem gewuergten
    // Gegner (dieselbe Kollision, vor der RE4VRChoke.cpp:2443 warnt).
    struct KStick {
        Handle go{};
        Handle mesh{};
        Handle tf{};
        double until{0.0};
        double born{0.0};   // fuer "der aelteste weicht", wenn alle Plaetze voll sind
    };

    // [MEHRERE 12.09.2026 -- Ansage "es reichen locker 5 messer gleichzeitig"]
    // FESTE Plaetze statt std::vector: Handle traegt einen gepinnten Zeiger und
    // wird nur ueber store()/drop() freigegeben. Ein Vektor wuerde beim Wachsen
    // oder Loeschen Handles KOPIEREN -- danach stuende derselbe Zeiger zweimal
    // im Spiel und das zweite drop() waere ein Release zu viel.
    static constexpr size_t KSTICK_MAX = 5;
    std::array<KStick, KSTICK_MAX> m_kstick{};

    bool knife_stick_clone_make(::REManagedObject* ktf, ::REManagedObject* etf,
                                const std::string& joint, const glm::vec3& lp,
                                const std::optional<glm::quat>& lr);
    void knife_stick_clone_drop_slot(KStick& s);
    void knife_stick_clone_drop();
    void knife_stick_clone_tick();

    // [MESSER-HEIMAT 2026-09-12 -- Ansage "egal ob choken oder werfen, das darf
    // nicht passieren"] Wachhund gegen das verlorene Messer: gemessen am
    // 12.09. stand wp5001 mit PARENT = nullptr am Weltnullpunkt, waehrend das
    // Spiel es weiter fuer gezogen hielt -- unsichtbar, nicht holsterbar, nicht
    // flippbar. Mehrere Wege setzen den Parent bewusst auf nullptr (Wurf-Detach
    // ab dem Einschlag, der Klon-Zweig in knife_return, der Choke-Notweg bei
    // stalem Rueckweg); faellt genau einer dieser Rueckwege aus, bleibt das
    // Messer fuer den Rest der Sitzung heimatlos.
    //
    // Der Wachhund merkt sich fuer JEDES gesunde Messer-GO -- linke wie rechte
    // Hand, Original wie Klon -- seinen Parent und Parent-Joint, und haengt ein
    // heimatlos gewordenes dorthin zurueck. Er erschafft NICHTS: kennt er ein
    // Messer nicht, bleibt er still (kein Messer im Spiel = nichts zu heilen).
    void knife_home_guard();

    // [ENGER RIEGEL 13.09.2026] Seit wann laeuft ununterbrochen REINES Gameplay?
    // Der Riegel gegen fremde Waffenwechsel greift erst eine Sekunde danach --
    // sonst faengt er den Wechsel ab, den der Spieler gerade im INVENTAR
    // gewaehlt hat und den das Spiel im ersten Gameplay-Frame ausfuehrt.
    double m_pure_since{0.0};
    void knife_home_forget(double now);

    void knife_throw_launch(const glm::vec3& dir, const std::optional<float>& speed);
    void knife_flight_tick();
    void knife_flight_apply();
    void knife_flight_break_scan();
    bool knife_try_hit_enemy(const glm::vec3& pos, float reach);
    ::REManagedObject* knife_flight_pick_enemy(const glm::vec3& kp, float reach);
    std::string knife_hit_object(::REManagedObject* hitgo, const std::optional<glm::vec3>& ip);
    void knife_flight_mark_hit(::REManagedObject* ego, const std::optional<glm::vec3>& ip);
    void knife_stick_in_enemy(::REManagedObject* ego, std::optional<glm::vec3> ip);
    void knife_unstick();
    void knife_detach();
    void knife_reattach();
    void knife_mesh_vis(bool vis);
    bool is_live_enemy(::REManagedObject* ectx);

    // Raycast-Helfer
    bool knife_ray_fire(const glm::vec3& from, const glm::vec3& to);
    bool knife_ray_finished();
    bool knife_ray_hit_dist(float& d_out, float& ny_out, ::REManagedObject*& go_out,
                            bool& is_wall_out);

    // ==================================================================
    // Baustein 19: THROWABLES / Granaten (Lua Z.3888-4507)
    // ==================================================================
    struct ThrowCfg {
        bool enabled{true};
        float hand_speed_min{5.8f}, hand_speed_max{9.2f};
        float throw_fixed_speed{10.0f};
        float throw_speed_min{4.0f}, throw_speed_max{12.0f};   // alt/ungenutzt
        float cooldown{1.0f}, forward_dot_min{1.0f};
        float sensitivity_min{0.67f}, sensitivity_max{1.29f};
        float y_offset{0.0f}, x_offset{0.0f}, release_window{0.111f};
        float knife_pitch{0.0f}, grenade_pitch{0.0f};
        float knife_yaw{0.0f};
        float throw_pitch{0.6f};
        float gravity{9.8f};
        float throw_speed_mult{2.0f};
    } m_tcfg{};

    void save_throw_cfg();
    void update_throw_velocity(bool force_right);
    float compute_release_window_peak();
    bool get_throw_direction(float extra_pitch, float extra_yaw, glm::vec3& out);
    bool is_grenade_equipped();
    bool is_knife_equipped();
    bool do_vr_throw();
    void grenade_ff_tick();
    void grenade_cache_guard();
    ::REManagedObject* find_grenade_generator();
    std::optional<glm::vec3> get_char_forward();
    std::optional<glm::vec3> get_player_world_pos();

    void install_grenade_hooks();

    static constexpr int32_t POS_HISTORY_SIZE = 12;
    static constexpr int32_t VELOCITY_SMOOTH_FRAMES = 3;
    static constexpr float RELEASE_PEAK_WINDOW_SEC = 0.15f;

    struct ThrowState {
        std::optional<glm::vec3> prev_pos{};
        double prev_time{0.0};
        float velocity{0.0f};
        std::array<std::optional<glm::vec3>, POS_HISTORY_SIZE> pos_history{};
        std::array<std::optional<glm::vec3>, POS_HISTORY_SIZE> player_history{};
        std::array<float, POS_HISTORY_SIZE> velocity_history{};
        std::array<double, POS_HISTORY_SIZE> pos_times{};
        int32_t pos_idx{0};
        std::string src{};
    } m_tstate{};

    struct KThrow {
        bool was_gripping{false};
        float peak{0.0f};
    } m_kthrow{};

    double m_last_knife_throw_t{0.0};
    double m_last_grenade_throw_t{0.0};

    // [GRANATENFLUG] Der Wurf nativ statt ueber Lua-Globals: dieselben Werte,
    // die weiter unter __re4_throw_* veroeffentlicht werden (andere Module
    // lesen sie), nur ohne den Umweg ueber den Lua-State im Hook.
    glm::vec3 m_throw_vel{};
    bool m_throw_vel_pending{false};
    double m_throw_apply_until{0.0};
    double m_throw_vel_last_t{0.0};
    ::REManagedObject* m_gren_act_shell{nullptr};
    ::REManagedObject* m_gren_move_shell{nullptr};
    double m_grenade_throw_reset_t{0.0};
    double m_grenade_ff_until{0.0};
    bool m_tfsm_was_aiming{false};
    float m_tfsm_peak{0.0f};

    Handle m_grenade_gen{};
    Handle m_scene_cache{};
    std::optional<uintptr_t> m_gen_body_addr{};
    double m_gen_next_check{0.0};

    // ==================================================================
    // Baustein 20: SHELL_EJECT (Lua Z.4509-4597)
    // ==================================================================
    void shell_refresh_muzzle();
    void install_shell_hook();

    // ==================================================================
    // Baustein 21: THROWSIGHT (Lua Z.4599-4715)
    // ==================================================================
    void install_throwsight_hooks();

    // [GRANATENFLUG 10.09.2026 -- PORT-WAISE] Die beiden Hooks auf
    // chainsaw.GrenadeShell, die den selbst gerechneten Wurf ueberhaupt erst
    // wirksam machen. Sie standen in re4_vr_weapons.lua (Z.4275-4310) und
    // fehlten im Port komplett -- gemessen 10.09.: der Rechner schrieb seine
    // Velocity in Globals, die niemand mehr abholte.
    void install_grenade_throw_hooks();
    void grenade_shell_launch(::REManagedObject* shell);   // Startimpuls, 1x
    void grenade_shell_move(::REManagedObject* shell);     // pro Frame + Gravitation
    void throwsight_force_off();

    Handle m_ts_ctrl{};
    bool m_ts_skip_ok{false};

    // [THROWSIGHT SAVE-LOAD 19.09.2026] Absturz im Dump re4.exe.20440.dmp:
    // nach Tod/Laden rief throwsight_force_off requestDeactivate auf den
    // Controller von VOR dem Laden (obj_ok laesst ihn durch, sein Inneres ist
    // abgebaut) -> AV im Spielcode. Springt die Body-Adresse, wird der
    // gemerkte Controller verworfen (derselbe Ausloeser wie beim Reload-Reset).
    std::optional<uintptr_t> m_ts_body_addr{};
    void ts_saveload_tick();

    // [BODY-EPOCH 2026-09-22] Zuletzt gesehener re4vr::body_epoch() und das
    // Verwerfen der gemerkten Body-Zeiger (s. RE4VR.hpp).
    uint64_t m_body_epoch{0};
    void drop_body_caches();

public:
    // [GRANATENFLUG] Von den GrenadeShell-Hooks gerufen (freie Lambdas kommen
    // an private Member nicht heran).
    void gren_activate_pre(::REManagedObject* shell) { m_gren_act_shell = shell; }
    void gren_activate_post() { grenade_shell_launch(m_gren_act_shell); }
    void gren_move_pre(::REManagedObject* shell) { m_gren_move_shell = shell; }
    void gren_move_post() { grenade_shell_move(m_gren_move_shell); }

    // Von den ThrowSight-Hooks gerufen.
    void set_ts_ctrl(::REManagedObject* c) { store(m_ts_ctrl, c); }
    bool ts_skip_ok() const { return m_ts_skip_ok; }
    bool ts_suppress_now();

private:

    // ==================================================================
    // Baustein 24: STICHRICHTUNG (Lua Z.5095-5188)
    // ==================================================================
    bool knife_dir_ok(::REManagedObject* target);
    void knife_dir_tick();
    ::REManagedObject* knife_tf_near_hand(const glm::vec3& hp);

    struct DirEntry {
        glm::vec3 p{};
        double t{0.0};
    };

    std::array<std::optional<DirEntry>, 6> m_dir_ring{};
    int32_t m_dir_ri{0};

    static constexpr float KLINGE_MIN = 0.6f;

    // ==================================================================
    // Sonstiges
    // ==================================================================
    bool m_vr_holster_knife{false};

    Handle m_character_manager{};
};

#endif
