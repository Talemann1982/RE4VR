// ============================================================================
// RE4VRKillswitch -- 1:1-Portierung von re4vr/re4_vr_killswitch.lua (2.966 Z.).
//
// Zentrale Steuerung: alle VR-Module fragen is_active ab und pausieren, wenn
// das Spiel die Kontrolle hat (Cutscenes/Events/Nicht-Gameplay-Kamera).
//
// Spezifikation: I:\LUATRANS\PORT_KILLSWITCH_SPEC.md
//
// ----------------------------------------------------------------------------
// BAUFORM -- das ist KEIN autorun-Script, sondern ein require-MODUL.
//
// Die Lua liegt in autorun/re4vr/ und wird per
// require("re4vr/re4_vr_killswitch") geholt. Sie hat damit keinen Platz in der
// alphabetischen Ladefolge -- die Reihenfolge-Falle der anderen Ports greift
// hier nicht.
//
// Der Port geht deshalb den Weg des Frame-Cache: native Implementierung, in
// on_lua_state_created als Tabelle unter
//   package.loaded["re4vr/re4_vr_killswitch"]
// hinterlegt. Die verbliebenen Lua-Dateien (binding, weapons, reload*) requiren
// unveraendert weiter und merken nichts.
//
// PHASE: pre-UpdateScene, NICHT on_frame. Das ist die einzige treibende Stelle
// der Lua (re.on_pre_application_entry("UpdateScene")); ein on_frame-Port waere
// eine Phase zu spaet und die 40 Globals kaemen einen Frame verzoegert.
// ----------------------------------------------------------------------------
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RE4VR.hpp"

class RE4VRKillswitch : public Mod {
public:
    // Vom tryUse-Hook gesetzt (schraege Leiter bestiegen).
    bool m_leaning_ladder_mounted{false};

    static std::shared_ptr<RE4VRKillswitch>& get();

    std::string_view get_name() const override { return "RE4VRKillswitch"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;

    void draw_public_ui();   // Firstperson-Events, gezeichnet von RE4VRMenu

    // ------------------------------------------------------------------
    // Oeffentliche API -- 1:1 die 30 Funktionen der Lua-return-Tabelle.
    // Native Nutzer rufen sie direkt, Lua-Nutzer ueber package.loaded.
    // ------------------------------------------------------------------
    bool is_active() const { return m_killswitch_active; }
    bool is_pin_release() const { return m_pin_release_active; }

    // [FP_GATE] Diese drei sagen "First-Person-Darstellung an". Toggle AUS ->
    // sie melden false, die Szene bleibt in 3rd-Person. Die Stufen selbst
    // laufen unveraendert weiter, insbesondere schaltet KS4 weiterhin alle
    // Scripte ab (__re4_ks4_active). KS5 ist bewusst NICHT gegated.
    bool is_ks2() const { return m_ks2_active && m_fp_enabled; }
    bool is_ks3() const { return m_ks3_active && m_fp_enabled; }
    bool is_ks4() const;
    bool is_ks5() const { return m_ks5_active; }
    bool is_fp_only() const { return m_ks3_active; }   // Alias (= KS3)

    bool just_activated() const { return m_killswitch_active && !m_was_active; }
    bool just_deactivated() const { return !m_killswitch_active && m_was_active; }

    bool is_cutscene_active() const { return m_killswitch_active; }
    bool is_real_cutscene_pub() { return is_real_cutscene().first; }
    bool is_crouch_active();
    bool is_player_camera_active();
    bool is_pure_gameplay();

    ::REManagedObject* get_busy_controller_pub() { return get_busy_controller(); }
    std::string get_controller() const { return m_current_controller; }
    std::string get_previous_controller() const { return m_previous_controller; }
    std::optional<int32_t> get_cam_state() const { return m_current_cam_state; }
    std::optional<int32_t> get_stage_name() const { return m_current_stage; }
    std::optional<int32_t> get_space_id() const { return m_current_space; }
    double get_anim_blend_back() const { return m_anim_blend_back; }
    std::string get_activating_reason() const { return m_activating_reason; }

    // [FP-MESSUNG 2026-09-06] Nur fuer die Diagnosezeile in RE4VRFirstPerson:
    // welche OccupiedInfo.Priority und welcher CamState lagen an, als die
    // Szene auf 3rd Person fiel. Der Finisher laeuft ueber
    // ActionCameraController -- dort ist der CamState nil und NUR die
    // Priority entscheidet.
    int32_t dbg_priority() { return occupied_priority().value_or(-1); }
    int32_t dbg_camstate() const { return m_current_cam_state.value_or(-1); }

    bool get_fp_enabled() const { return m_fp_enabled; }
    void set_fp_enabled(bool v);

    // [ZONES] Vom Monitor bedient. mark liefert (ok, zone|grund).
    struct Zone {
        int32_t stage{0};
        int32_t space{0};
        float x{0.0f}, y{0.0f}, z{0.0f};
        float r{3.0f};
        int32_t level{2};
        std::optional<int32_t> camstate{};
        std::string name{};
    };

    struct Entry {
        int32_t stage{0};
        int32_t space{0};
        std::optional<int32_t> camstate{};
        bool has_pos{false};
        glm::vec3 pos{};
    };

    // Laufendes Nicht-Gameplay-Event, sonst das letzte beendete (der
    // Monitor-Button ist auch NACH dem Event noch drueckbar).
    const std::optional<Entry>& current_entry() const {
        return m_cur_episode_entry.has_value() ? m_cur_episode_entry : m_last_episode_entry;
    }

    std::pair<bool, std::string> mark_current_zone(int32_t level, std::optional<float> radius);
    std::pair<bool, std::string> remove_last_zone();
    int32_t reload_zones();
    const std::vector<Zone>& get_zones() const { return m_zones; }
    size_t get_zone_count() const { return m_zones.size(); }

    // Kompat-Helper (arm_chain & Co. nutzen die vom alten Killswitch)
    // Das StateParam-Lesen ist hier erprobt (ValueType-Container-Flag) --
    // RE4VRBinding braucht denselben GimmickType fuer die Turret-Erkennung
    // und soll ihn nicht ein zweites Mal nachbauen.
    std::optional<int32_t> read_gimmick_type_pub(::REManagedObject* busy) {
        return read_gimmick_type(busy);
    }

    ::REManagedObject* get_player_context() { return re4vr::fc::ctx(); }
    ::REManagedObject* get_player_body() { return re4vr::fc::body_go(); }

private:
    // ------------------------------------------------------------------
    // Zustand (1:1 die Top-Level-Locals der Lua)
    // ------------------------------------------------------------------
    bool m_killswitch_active{false};
    bool m_pin_release_active{false};
    double m_jumpdown_until{0.0};
    bool m_jumpdown_confirmed{false};
    bool m_ks2_active{false};
    bool m_ks3_active{false};
    bool m_ks4_active{false};
    bool m_ks5_active{false};
    bool m_boxbreak_active{false};

    // [FP_LATCH] Sobald das Token EINMAL waehrend einer Nicht-Gameplay-Episode
    // (gleicher State) auftaucht, halten wir die Stufe fuer die GANZE Episode
    // -> kein Flackern. Reset, sobald der State wechselt.
    bool m_fp_latch{false};
    std::optional<int32_t> m_fp_latch_state{};
    int32_t m_fp_latch_level{0};

    bool m_was_active{false};
    bool m_prev_full_off{false};

    std::string m_current_controller{};
    std::string m_previous_controller{};
    std::optional<int32_t> m_current_cam_state{};
    std::optional<int32_t> m_current_stage{};
    std::optional<int32_t> m_current_space{};
    std::string m_activating_reason{};

    bool m_force_killswitch{false};

    double m_anim_blend_back{0.0};
    double m_anim_blend_back_t{0.0};

    bool m_prev_ks4_exit{false};
    bool m_prev_ks3_exit{false};
    bool m_prev_ks2_exit{false};

    // Config-Globals (aus der JSON)
    bool m_fp_enabled{true};
    bool m_ks2_as_ks4{true};

    // Halte-Fenster / Timer
    double m_fc_ks4_until{0.0};
    std::optional<double> m_evt60874_t0{};
    double m_damage_until{0.0};
    double m_hookshot_seen_t{0.0};

    // [LATCHES] In der Lua als Globals gefuehrt, WEIL die Datei am 200er-
    // Local-Limit stand -- nicht, weil sie jemand von aussen liest (per grep
    // ueber alle Scripte und alle nativen Module belegt: kein einziger fremder
    // Setzer). Nativ sind es deshalb Member; veroeffentlicht werden sie
    // trotzdem, damit Diagnose-Werkzeuge sie weiterhin sehen.
    double m_squeeze_latch_t{-1e9};
    double m_minidemo_latch_t{-1e9};
    double m_gfix_latch_t{-1e9};
    std::optional<double> m_gang3rd_t{};
    std::optional<double> m_evt40510_t{};

    // ------------------------------------------------------------------
    // Zonen
    // ------------------------------------------------------------------
    std::vector<Zone> m_zones{};
    std::optional<Entry> m_cur_episode_entry{};
    std::optional<Entry> m_last_episode_entry{};

    void load_zones();
    void save_zones();
    void load_ks_cfg();
    void save_ks_cfg();

    std::optional<int32_t> zone_level_for(const std::optional<Entry>& e) const;

    // ------------------------------------------------------------------
    // Enum-Aufloesung (lazy, alle Listen in einem Pass)
    // ------------------------------------------------------------------
    bool m_state_vals_ok{false};
    std::unordered_set<int32_t> m_gameplay_vals{};
    std::unordered_set<int32_t> m_pinrelease_vals{};
    std::unordered_set<int32_t> m_ks2_vals{};
    std::unordered_set<int32_t> m_ks3_vals{};
    std::unordered_set<int32_t> m_ks4_vals{};
    std::optional<int32_t> m_damage_int{};
    std::optional<int32_t> m_hookshot_int{};
    std::optional<int32_t> m_gimmick_int{};
    std::optional<int32_t> m_landing_int{};
    std::optional<int32_t> m_forcecrouch_int{};
    void resolve_state_vals();

    bool is_gameplay_camstate(std::optional<int32_t> st);
    bool is_pinrelease_camstate(std::optional<int32_t> st);
    bool is_airborne_camstate(std::optional<int32_t> st);
    bool is_ks2_camstate(std::optional<int32_t> st);
    bool is_ks3_camstate(std::optional<int32_t> st);
    bool is_ks4_camstate(std::optional<int32_t> st);

    // Lazy aufgeloeste Prioritaets-/GimmickType-Mengen
    std::optional<int32_t> m_coop_jacked_prio{};
    std::optional<std::unordered_set<int32_t>> m_grappled_prio{};
    std::optional<std::unordered_set<int32_t>> m_gondola_vals{};
    std::optional<std::unordered_set<int32_t>> m_legtrap_vals{};
    std::optional<std::unordered_set<int32_t>> m_elevtrouble_vals{};
    std::optional<std::unordered_set<int32_t>> m_demo_prios{};
    std::optional<int32_t> m_squeeze_high_prio{};
    std::optional<int32_t> m_minidemo_prio{};
    std::optional<int32_t> m_gfix_low_prio{};
    std::optional<uint64_t> m_parentgimmick_bit{};

    std::optional<int32_t> coop_jacked_prio();
    const std::unordered_set<int32_t>* grappled_prio();
    const std::unordered_set<int32_t>* gondola_vals();
    const std::unordered_set<int32_t>* legtrap_vals();
    const std::unordered_set<int32_t>* elevtrouble_vals();
    const std::unordered_set<int32_t>* demo_prios();
    std::optional<int32_t> squeeze_high_prio();
    std::optional<int32_t> minidemo_prio();
    std::optional<int32_t> gfix_low_prio();
    void resolve_parentgimmick_bit();

    // ------------------------------------------------------------------
    // Getter / Zustandspruefer
    // ------------------------------------------------------------------
    ::REManagedObject* get_camera_system();
    ::REManagedObject* get_gui_manager();
    ::REManagedObject* get_busy_controller();
    std::optional<glm::vec3> get_player_pos();

    std::pair<bool, std::string> is_real_cutscene();
    std::optional<int32_t> read_cam_state(::REManagedObject* busy);
    std::optional<int32_t> read_gimmick_type(::REManagedObject* busy);
    std::optional<int32_t> read_live_cam_state();
    std::pair<std::optional<int32_t>, std::optional<int32_t>> read_stage_space();

    bool player_gimmick_active();
    bool player_is_coop_jacked();
    bool player_is_grappled();
    bool player_is_boxbreak();
    bool player_is_ladder();
    bool player_is_ladder_exit(bool cam_is_gameplay);
    bool player_node_has(const char* token);
    bool player_jack_has(const char* token);
    std::optional<int32_t> occupied_priority();

    // MotionFsm2 / Motion -- Component-Handles gecacht, Body-Wechsel
    // (Save-Load/Tod) invalidiert sie.
    ::REManagedObject* m_mfsm2_comp{nullptr};
    ::REManagedObject* m_mfsm2_body{nullptr};
    ::REManagedObject* m_motion_comp{nullptr};
    ::REManagedObject* m_motion_body{nullptr};
    ::REManagedObject* find_mfsm2(::REManagedObject* body);
    ::REManagedObject* mfsm2_for_body();
    ::REManagedObject* motion_for_body();

    bool m_ladder_exit_latched{false};

    // ------------------------------------------------------------------
    // Spezial-Erkennungen
    // ------------------------------------------------------------------
    bool has_parent_gimmick();
    bool is_on_gondola();
    bool is_in_legholdtrap();
    bool is_gimmick_ks3_spot();
    bool is_minidemo_ks4_spot();
    bool is_gimmickfix_ks4_spot();
    bool is_gimmickfix_ks5_spot();
    bool is_gimmick_motion_now();
    bool is_demo_priority_now();
    bool is_carrying();
    bool is_riding_elevator_59100();
    bool is_jetski_stage();
    bool is_on_jetski();
    bool is_on_boat();
    bool is_on_railcar();
    bool is_throwsight_stage();
    bool is_elevator_trouble();
    bool is_in_elevator();
    bool is_in_elevator2_zone();
    bool is_in_elevator3_zone();
    bool is_parented_to_elevator();
    bool is_in_elevator5_cabin();
    bool elevator5_update();
    const char* minecart_ks4_kind();

    ::REManagedObject* m_elev59100_comp{nullptr};
    ::REManagedObject* m_railcar_mgr{nullptr};

    struct Elev5 {
        bool latch{false};
        bool prev_gim{false};
        std::optional<float> y{};
        double t{0.0};
        double still_since{0.0};
        std::optional<float> target{};
        bool moved_once{false};
    } m_elev5{};

    // ------------------------------------------------------------------
    // Auswertung
    // ------------------------------------------------------------------
    void evaluate_core();
    void evaluate_player_camera(::REManagedObject* busy);
    void resolve_type_defs();
    void evaluate();
    void publish_globals();

    // Live ueber Globals einstellbar (die Lua liest sie per tonumber mit
    // Default). Kein Script setzt sie -- die Defaults sind der Normalfall.
    double hookshot_grace_sec() const { return 4.0; }
    double hookshot_ks4_sec() const { return 1.5; }


    // ------------------------------------------------------------------
    // Info-Flags -- werden in evaluate gesetzt und am Ende GESAMMELT nach
    // Lua veroeffentlicht (publish_globals). Innerhalb eines Durchlaufs
    // liest sie niemand, das Sammeln ist also verhaltensgleich und spart
    // rund 40 Lua-Zugriffe pro Frame.
    // ------------------------------------------------------------------
    bool m_p_ks_active{false};
    bool m_p_ks4_active_global{false};
    std::string m_p_ks_reason{};
    bool m_p_ks_level_is_4{false};
    bool m_p_boxbreak_active{false};
    bool m_p_fatalkick_active{false};
    bool m_p_forcecrouch_active{false};
    bool m_p_forcecrouch_ks4_active{false};
    bool m_p_leaning_ladder_active{false};
    bool m_p_minecart_ks4_active{false};
    bool m_p_minecart2_ks4_active{false};
    bool m_p_grappled_active{false};
    bool m_p_gondola_active{false};
    bool m_p_gondola_ada_active{false};
    bool m_p_railcar_mode{false};
    bool m_p_jetski_active{false};
    bool m_p_boat_active{false};
    bool m_p_in_squeeze{false};
    bool m_p_ks_keep_movement{false};
    bool m_p_damage_active{false};
    std::optional<double> m_p_damage_end_t{};
    std::optional<double> m_p_ks4_exit_t{};
    std::optional<double> m_p_hookshot_recent_until{};

    // Diese beiden stehen BEWUSST nicht im Default-Block von evaluate_core:
    // die Lua setzt sie nur in einzelnen Zweigen, sie halten also ueber
    // Frames hinweg ihren Wert.
    bool m_p_throwsight_active{false};
    bool m_p_evt60874_fullhide{false};

    bool m_err_published{false};

    bool m_cfg_loaded{false};
    bool m_type_defs_ok{false};
    bool m_hook_installed{false};
    void install_ladder_hook();
};

#endif // RE4
