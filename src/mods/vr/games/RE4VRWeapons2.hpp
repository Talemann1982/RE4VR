// ============================================================================
// RE4VRWeapons2 -- 1:1-Portierung von re4_vr_weapons2.lua (3.500 Zeilen).
//
// Die Lua ist eine SAMMELDATEI aus sieben IIFE-Bloecken (jeder mit eigenem
// Local-Budget). Die Reihenfolge im File bildet die alte alphabetische
// Load-Reihenfolge der zusammengelegten Einzelscripte nach und wird hier
// unveraendert uebernommen:
//
//   1) Z.  13- 378  re4_vr_knife.lua          Finisher-Shake + Parry + Wesker-Armblock
//   2) Z. 381-1644  re4_vr_knife_lefthand.lua Messer LINKS (Mesh-Klon, Greifen, Melee)
//   3) Z.1647-2351  re4_vr_knife_lh_damage.lua Klon-Schaden (calcInfo/hitSetting)
//   4) Z.2354-3163  re4_vr_wildwest.lua       Gunslinger-Twirl
//   5) Z.3182-3227  re4_vr_cannon.lua         Kanone gm84_572, Yaw erweitern
//   6) Z.3260-3437  re4_vr_unlimited.lua      Unlimited-Ammo + Katzenohren
//   7) Z.3463-3500  (inline)                  PARRY_KEEP_GUN
//
// Spezifikation: I:\LUATRANS\PORT_WEAPONS2_SPEC.md
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- HINTER dem ScriptRunner, so weit hinten wie
// moeglich. re4_vr_weapons2.lua ist die alphabetisch LETZTE autorun-Datei, und
// zwei Stellen haengen unmittelbar daran (Spec §1):
//
//   (a) Der Wrap auf __re4_reload_set_mag_in_hand faengt zur LADEZEIT ein, was
//       reload/2/3/4_dlc/5_dlc vorher gesetzt haben. Sechs Dateien exportieren
//       das Global, weapons2 gewinnt, weil es zuletzt laedt. Der Wrap darf
//       deshalb NICHT in on_lua_state_created installiert werden -- dort haben
//       die Reload-Luas noch nichts gesetzt. Er gehoert in den einmaligen
//       Late-Init im ersten on_frame (ensure_init).
//
//   (b) Das on_frame des Unlimited-Blocks muss NACH allen on_frame der
//       Reload-Luas laufen, weil es deren grab_empty-Zuweisungen ueberschreibt.
//
// Alle uebrigen Konsumenten greifen zur LAUFZEIT per rawget zu; fuer sie ist
// die Platzierung neutral.
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

class RE4VRWeapons2 : public Mod {
public:
    static std::shared_ptr<RE4VRWeapons2>& get();

    std::string_view get_name() const override { return "RE4VRWeapons2"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // ------------------------------------------------------------------
    // Von anderen nativen Modulen direkt gerufen (die Lua-Globals bleiben
    // zusaetzlich exportiert, solange weapons.lua und der Reload-Block Lua
    // sind).
    // ------------------------------------------------------------------
    bool knife_direct_damage(float reach);
    bool knife_direct_damage_at(const glm::vec3& pos, float reach);
    void knife_lh_play_sound(int32_t id);
    void apply_left_knife_pose();
    bool wildwest_apply(glm::vec3& wpos, glm::quat& wrot);
    void wildwest_fingers();
    bool is_unlimited();
    bool infinite_reserve();
    bool infinite_ammo_id(std::string& s_out, std::optional<double>& n_out);
    std::string accessory_now();
    bool knife_onhit_replay(::REManagedObject* target_go, ::REManagedObject* atk_ud);
    bool knife_hitset_box(::REManagedObject* box_go, const std::optional<glm::vec3>& pos);

    // nur fuer die UI-Anzeige (__re4_ww_stock_now)
    bool stock_now() const;

    // Der gewrappte __re4_reload_set_mag_in_hand ruft hierueber das Original --
    // m_orig_smih ist privat, die Lua-Closure kommt sonst nicht heran.
    bool smih_call(sol::object active);

private:
    // ------------------------------------------------------------------
    // Gemeinsam
    // ------------------------------------------------------------------
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    // `unconditional` = Luas pin(o), das add_ref BEDINGUNGSLOS ruft. Ein frisch
    // per create_instance erzeugtes Objekt kann refcount 0 haben -- die uebliche
    // refcount-Heuristik verankert es dann NICHT.
    // Siehe [[reference_re4_cpp_selbst_erzeugte_objekte_pinnen]].
    static void store(Handle& h, ::REManagedObject* obj, bool unconditional = false);
    static void drop(Handle& h);

    bool ensure_init();

    bool m_inited{false};

    // ==================================================================
    // Block 1 -- Finisher-Shake + Parry (Lua Z.13-378)
    // ==================================================================
    struct ShakeCfg {
        float speed{0.80f};       // m/s Mindestgeschwindigkeit pro Halbschwung
        int32_t reversals{1};     // EIN Richtungswechsel reicht
        float window{0.80f};      // s Sammelfenster
        float fire{0.12f};        // s Dauer des RT-Pulses
        float cooldown{0.50f};    // s Sperre nach Ausloesung
        float vert_dom{0.50f};    // |dy| > vert_dom * horizontal
    };

    struct ParryCfg {
        float pos_tol{0.30f};
        float rot_tol{0.70f};
        float window{0.50f};
    };

    // Kalibrierte Referenz-Pose (rechter Controller RELATIV zum HMD),
    // Snapshot 2026-07-04.
    struct PoseRef {
        float px{}, py{}, pz{};
        float qw{}, qx{}, qy{}, qz{};
    };

    void knife_tick();
    void parry_hook_install();
    void install_equip_hook();
    void install_capture_hook();

    // Rueckgabe: true = SKIP_ORIGINAL.
    bool equipweapon_cb(uintptr_t raw_arg);

    bool m_hooks_installed{false};

    ShakeCfg m_shake{};
    ParryCfg m_pcfg{};
    float m_parry_tol{1.0f};   // KCFG.tol, persistent in re4_vr_knife_parry.json

    void load_parry_cfg();
    void save_parry_cfg();

    struct ShakeState {
        std::optional<glm::vec3> last_p{};
        double last_time{0.0};
        int32_t last_dir{0};
        int32_t reversals{0};
        double window_end{0.0};
        double fire_until{0.0};
        double cooldown_end{0.0};
        bool gate_prev{false};
        bool parry_pose_prev{false};
        bool wesker_hold_prev{false};
        int32_t last_cidx{-1};
    } m_st{};

    // ==================================================================
    // Block 2 -- Messer LINKE Hand (Lua Z.381-1644)
    // ==================================================================
    struct LhCfg {
        float trigger{0.22f};
        float release{0.30f};
        float tap_sec{0.18f};
        float flip_speed{0.3f};
        float swing_speed{3.0f};
        bool blood_on{true};
    };

    void lh_char_tick();
    void lh_tick();

    bool lcfg_load(const char* path);
    void lcfg_save();
    const char* lcfg_path() const;
    const char* lh_path() const;
    const char* lh_flip_path() const;
    bool lh_write_allowed(const char* path) const;
    void lh_off_save();
    void lh_flip_save();

    // Klon-Lebenszyklus
    void clone_manage();
    bool clone_spawn();
    void clone_destroy();
    // [LH_CLONE RESET 20.09.2026] wie clone_destroy, aber OHNE
    // destroy_game_object -- fuer den Fall, dass das alte GO nach
    // Tod/Laden/Levelstart schon abgebaut ist.
    void clone_forget();
    bool clone_isolate_part0();
    void clone_apply_pose();
    std::optional<int32_t> destroy_orphan_clones(::REManagedObject* keep);

    bool defer_draw_left();
    bool defer_stow_left();
    bool player_is_reloading();
    void exec_native_melee();

    std::optional<int32_t> get_selected_knife_wid();
    std::optional<int32_t> poll_inventory_knife_wid();
    ::REManagedObject* find_knife_mesh(std::optional<int32_t>& wid_out);
    ::REManagedObject* lh_body_knife_soundcontainer();

    LhCfg m_lcfg{};
    std::string m_lh_char{};   // "leon" | "ada"; leer = noch nie gesetzt
    std::optional<float> m_last_d{};

    struct Clone {
        Handle obj{};
        Handle mesh{};
        std::optional<int32_t> wid{};
        bool parented{false};
        bool part0{false};
        bool was_flying{false};
        std::optional<bool> vis{};
        std::optional<double> fin_kick{};
    } m_clone{};

    double m_orphan_check_t{0.0};

    // Pro-Messer-Offsets. In Lua GLOBALE Tabellen (__re4_knife_lh_off_map,
    // __re4_knife_flip_lh_map) -- sie bleiben exportiert, weil die UI und
    // motion.lua sie lesen.
    // [WAISE 05.09.2026] Spiegelt m_lh_off/m_lh_flip_off nach Lua -- motion
    // liest beide Tabellen von dort.
    void lh_publish();

public:
    // [WAISE 05.09.2026] Schaden fuer GENAU EINEN Aufruf von
    // knife_direct_damage_at ueberschreiben. In Lua tauschte re4_vr_choke.lua
    // dafuer die Tabelle __re4_knife_saved_vals gegen eine Kopie mit eigenem
    // `damage` und legte danach die Originalreferenz zurueck. Diese Tabelle
    // gibt es seit dem Port nicht mehr (die Werte liegen in m_saved_vals),
    // wodurch beide Choke-Schadensregler (Stich 150 / Stecken-Stich 250)
    // wirkungslos waren -- jeder Stich machte den generischen Links-Messer-
    // Schaden. Der Override bildet den Tausch 1:1 nach: setzen, rufen,
    // zuruecksetzen.
    void set_damage_override(std::optional<float> v) { m_damage_override = v; }

private:
    std::optional<float> m_damage_override{};

    struct LhOff {
        float px{}, py{}, pz{}, rx{}, ry{}, rz{};
    };

    std::unordered_map<int32_t, LhOff> m_lh_off{};
    std::unordered_map<int32_t, glm::vec3> m_lh_flip_off{};

    float m_flip_lerp{0.0f};
    float m_flip_prev_target{-1.0f};
    Handle m_lh_snd_last{};

    // Frame-Zustand des Haupt-Ticks
    bool m_prev_lgrip{false};
    bool m_armed{false};
    std::optional<glm::vec3> m_prev_lh{};
    double m_lh_t{0.0};
    double m_last_swing{0.0};
    double m_last_hit{0.0};

    // MotionFsm2-Cache fuer player_is_reloading
    Handle m_mfsm_go{};
    Handle m_mfsm_comp{};

    // ==================================================================
    // Block 3 -- Klon-Schaden (Lua Z.1647-2351)
    // ==================================================================
    struct Target {
        ::REManagedObject* body{nullptr};
        ::REManagedObject* hc{nullptr};
        glm::vec3 pos{};
    };

    void capture_cb(::REManagedObject* attacker_hc, ::REManagedObject* info);
    void capture_dmginfo(::REManagedObject* real_info, bool is_knife);
    void knife_drop_caches(const char* grund);
    void knife_di_guard();
    bool pick_nearest_enemy(const glm::vec3& pos, float reach, Target& out);
    ::REManagedObject* build_dmginfo(::REManagedObject* victim_hc,
                                     ::REManagedObject* victim_body,
                                     const std::optional<glm::vec3>& contact_pos,
                                     float& dmg_out);
    void saved_vals_tick();

    // Gespeicherte Messer-Schadenswerte (re4_vr/re4_vr_knife_dmg.json).
    struct SavedVals {
        float damage{};
        float wince{};
        float brk{};
        float stop{};
        std::optional<int32_t> wid{};
    };

    std::optional<SavedVals> m_saved_vals{};
    bool save_vals_load();
    void save_vals_store(const SavedVals& v);

    Handle m_dmginfo{};           // copy der Vorlage
    Handle m_dmginfo_raw{};       // das ECHTE, gepoolte Objekt der Engine
    bool m_dmginfo_raw_knife{false};
    Handle m_native_di{};         // unsere Bau-DamageInfo
    Handle m_dud{};               // DamageUserData

    std::optional<uintptr_t> m_di_body_addr{};
    double m_di_next_check{0.0};
    // [LH_CLONE RESET 20.09.2026] Body war zwischendurch GANZ weg
    // (Mercs-Levelstart / Tod / Laden) -- zaehlt wie ein Adress-Sprung.
    bool m_di_body_weg{false};

    std::optional<int32_t> m_melee_combat{};
    bool m_melee_combat_looked_up{false};

    // ==================================================================
    // Block 4 -- Wild West Twirl (Lua Z.2354-3163)
    // ==================================================================
    struct OKey {
        float a{};   // Salto-Fortschritt 0..360
        float x{}, y{}, z{};
    };

    void ww_tick();
    void ww_save_cfg();
    void ww_load_cfg();
    bool ww_rt_gate();
    void ww_reset_all();
    void ww_reset_bob();
    bool stock_mounted(std::optional<int32_t> wid);
    std::optional<glm::vec3> compute_local_pivot(::REManagedObject* tf);
    bool offset_at(float phase, glm::vec3& out);
    void save_keyframe(float phase, const glm::vec3& off);
    void ww_sound_tick(::REManagedObject* hu, double now);
    void publish_lcfg_globals();
    void twirl_voice_tick();
    void twirl_voice_end();
    ::REManagedObject* twirl_voice_container();

    // Finger-Pose PRO JOINT (additive Euler-Grad).
    std::unordered_map<std::string, glm::vec3> m_fing{};

    // Keyframes pro Waffe. In Lua ein GLOBAL (__re4_ww_okeys_by_wid); OKEYS ist
    // ein ZEIGER auf das Array der aktuellen Waffe -- im Port darf das kein
    // Kopieren werden, sonst gehen Keyframe-Edits verloren.
    std::unordered_map<int32_t, std::vector<OKey>> m_okeys{};
    std::vector<OKey>* m_cur_okeys{nullptr};

    struct Spin {
        bool active{false};
        bool finishing{false};
        float deg{0.0f};
        float finish_target{0.0f};
        bool by_rt{false};
    } m_spin{};

    float m_finger_blend{0.0f};
    std::unordered_map<int32_t, glm::vec3> m_pivot_cache{};
    std::optional<glm::vec3> m_active_lp{};

    std::optional<float> m_prev_y{};
    std::optional<double> m_prev_t{};
    int32_t m_prev_vsign{0};
    double m_prev_vsign_t{0.0};
    std::optional<double> m_bob_started{};
    double m_last_flip_t{0.0};

    bool m_rt_gate_prev{false};
    bool m_rt_press_armed{false};
    bool m_rt_prev_raw{false};
    double m_last_shot_t{-999.0};
    std::optional<double> m_last_shot_seq{};

    struct StockCache {
        std::optional<int32_t> wid{};
        double t{0.0};
        bool on{false};
    } m_stock_c{};

    struct TwirlSnd {
        double next_t{0.0};
    } m_twirl_snd{};

    struct TwirlVoice {
        std::optional<double> t0{};
        float dur{0.0f};
        int32_t count{0};
        int32_t end_count{0};
        int32_t last_id{0};
        bool fired{false};
    } m_tvoice{};

    // ==================================================================
    // Block 5 -- Kanone (Lua Z.3182-3227)
    // ==================================================================
    void cannon_tick();

    Handle m_c_go{};
    Handle m_c_cc{};

    // ==================================================================
    // Block 6 -- Unlimited + Katzenohren (Lua Z.3260-3437)
    // ==================================================================
    void unlimited_tick();
    ::REManagedObject* get_equipped_wi();

    struct UnlimCache {
        double t{-1.0};
        bool val{false};
    } m_unlim_c{};

    struct AccCache {
        double t{-1.0};
        std::string id{};
    } m_acc_c{};

    struct AmmoIdCache {
        double t{-1.0};
        std::string s{};
        std::optional<double> n{};
        bool valid{false};
    } m_amid{};

    // Der urspruengliche __re4_reload_set_mag_in_hand, eingefangen im
    // Late-Init (s. Kopfkommentar (a)).
    sol::protected_function m_orig_smih{};
    bool m_smih_wrapped{false};

    // ==================================================================
    // Block 7 -- PARRY_KEEP_GUN (Lua Z.3463-3500)
    // ==================================================================
    void parry_keep_gun_tick();

    // ==================================================================
    // UI
    // ==================================================================
    void draw_parry_ui();
    void draw_lh_ui();
    void draw_ww_ui();

    bool m_tree_open{false};

    // Tuning-Zustand der Wild-West-UI
    float m_ww_prev_angle_ui{90.0f};
};

#endif
