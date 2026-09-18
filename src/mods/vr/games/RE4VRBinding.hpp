// ============================================================================
// RE4VRBinding -- 1:1-Portierung von re4_vr_binding.lua (3.627 Zeilen).
//
// DAS IST DER EINGABEPFAD. Ein Fehler hier macht das Spiel unbedienbar.
// Merksatz: Eingabepfade nie blind gaten -- lieber den nativen Call blocken.
//
// Spezifikation: I:\LUATRANS\PORT_BINDING_SPEC.md
//
// ----------------------------------------------------------------------------
// DIE VIER FREMDEN APIs
//
//   vigem.*      -> VigemPad (seit 04.09. nativ im Fork, war Plugin re_vr.dll)
//   vrmod:*      -> VR::get()
//   killswitch.* -> RE4VRKillswitch::get()  (direkt, ohne den Umweg ueber Lua)
//   overlay.*    -> bleibt ein Lua-Aufruf: Plugin-API, aber nur drei Stellen
//                   (Sync, alle 30 Frames ein tick, Toggle) -- kein heisser Pfad
//
// Die Lua stieg in Zeile 34 aus, wenn `vigem` fehlte, und in Zeile 54, wenn der
// killswitch-require scheiterte. Beide Wege entfallen; dafuer haengt jetzt alles
// an VigemPad::init(). Schlaegt das fehl (kein ViGEmBus-Treiber), geht kein
// einziger Knopf raus -- deshalb meldet VigemPad::probe() den Treiberzustand
// beim Start in __re4_vigem_ready.
//
// ----------------------------------------------------------------------------
// AUFBAU
//
// Ein grosser on_frame mit einer Branch-KETTE, deren Reihenfolge tragend ist:
//   in_boat -> in_throwsight -> in_menu -> in_turret -> in_jetski
//           -> ks_active -> in_binoculars -> mercs -> ada -> GAMEPLAY
// turret/jetski stehen VOR ks_active (beide SIND KS4 und wuerden sonst vom
// KS-Zweig geschluckt); alle stehen NACH in_menu; mercs steht VOR ada (in Mercs
// ist Ada ebenfalls spielbar).
//
// Danach die Post-Branch-Bloecke, die das letzte Wort haben, und apply_frame
// mit seinen 15 weiteren Eingriffen -- auch dort ist die Reihenfolge tragend
// (Grapple/Battle/Gigante setzen RT branchunabhaengig auf 1.0, deshalb steht
// der Map-/Gondel-Block als LETZTE Zeile vor dem Versand).
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

class RE4VRBinding : public Mod {
public:
    static std::shared_ptr<RE4VRBinding>& get();

    std::string_view get_name() const override { return "RE4VRBinding"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // [BINO_PHASEN 11.09.2026] Nur fuer den Fernglas-Offset (bino_apply_offset),
    // sonst nichts -- der Eingabepfad bleibt im on_frame.
    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // [UI-REIHENFOLGE 05.09.2026] Der nackte Teil des Menues (180-Grad-Haken
    // + Snapturn) steht OBEN, der "RE4VR - Binding"-Tree unten bei den
    // anderen. Deshalb zwei Funktionen; Mods::on_draw_ui ruft beide.
    void draw_public_ui();

    bool on_pre_gui_draw_element(::REComponent* element, void* context) override;

private:
    // [RT-SPERRE IM STAGGER 15.09.2026 -- Idee des Users] Waehrend eines
    // Staggers nimmt das Spiel die Waffe aus der Hand (Gun.State = None) --
    // wer in dieser Zeit RT haelt, feuert beim Zurueckkommen im 2-Frame-Takt
    // (gemessen: 14 Schuesse, erlaubt waeren 0,167 s). Solange die Waffe nicht
    // da ist, geht RT gar nicht erst raus.
    //
    // ROCK SOLID heisst hier: die Sperre ist FAIL-OPEN. Sie greift nur, wenn
    // der Waffenzustand LESBAR ist UND ausdruecklich nicht Holding lautet.
    // Ladevorgang, Reset Scripts, unbekannte Waffe -> keine Sperre. Dazu eine
    // Notbremse: laenger als RT_WAIT_MAX am Stueck wird nie gesperrt, danach
    // gilt sie als aufgegeben, bis Holding wirklich wieder gesehen wurde.
    static constexpr double RT_WAIT_MAX = 1.5;

    std::optional<double> m_rt_wait_since{};
    bool m_rt_wait_gaveup{false};



    // ------------------------------------------------------------------
    // PREFS (re4_vr/re4_vr_bindings.json)
    // ------------------------------------------------------------------
    struct Prefs {
        bool hide_ref_overlay{false};
        float long_press_sec{1.5f};
        float short_min_sec{0.0f};
        // [QUICKTURN_180] DEFAULT AUS -- bewusst: ein versehentlicher
        // Doppel-Tipp wuerde sonst mitten im Gefecht die Blickrichtung drehen.
        bool enable_180_rotation{false};
        float turn180_window_sec{0.17f};
        float turn180_ly_sec{0.10f};
        float turn180_rb_sec{0.35f};
        float turn180_sec{0.15f};
        // [TRACKPAD-SCROLL 15.09.2026] Faktor auf das Stick-Scrolltempo.
        float trackpad_scroll{2.5f};
        bool enable_snapturn{false};
        int32_t snapturn_deg{45};
        float snapturn_thresh{0.75f};
    } m_prefs{};

    void load_prefs();
    void save_prefs();
    void draw_snapturn_ui(const char* sfx);

    // ------------------------------------------------------------------
    // Der Frame-Zustand -- 1:1 new_frame_state()
    // Buttons sind BOOL, Trigger FLOAT. Nicht vermischen: die Lua traegt an
    // Z.1598 ausdruecklich den Hinweis "0.0 waere truthy -> wuerde X DRUECKEN
    // statt muten".
    // ------------------------------------------------------------------
    struct Frame {
        bool a{false}, b{false}, x{false}, y{false};
        bool lb{false}, rb{false};
        bool ls{false}, rs{false};
        bool back{false}, start{false};
        bool dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};
        float lt{0.0f}, rt{0.0f};
        float lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};
    };

    void apply_frame(Frame& f);

    // ------------------------------------------------------------------
    // Init / VR-Handles
    // ------------------------------------------------------------------
    bool ensure_init();
    bool m_inited{false};
    bool m_was_active{false};

    // ------------------------------------------------------------------
    // Fernglas
    // ------------------------------------------------------------------
    struct BinoZoom {
        bool active{false};
        float offset{0.0f};
        float start_offset{2.5f};
        float min_offset{-7.0f};
        float max_offset{2.5f};
        float stick_speed{3.0f};
    } m_bino{};

    float bino_val(const char* key, float fallback);
    bool is_binoculars_active();
    bool is_binoculars_active_this_frame();
    int32_t m_bino_scan_frame{-1};
    bool m_bino_scan_active{false};
    void bino_tick();

    // [BINO_PHASEN 11.09.2026] Offset auf die Spielkamera legen. Wird aus on_frame
    // UND aus spaeteren Phasen gerufen: Adas Fernglas-Kamera (Separate Ways) setzt
    // ihre Position vor dem Rendern selbst neu und loeschte den Offset aus dem
    // on_frame (per Lua-Messung belegt: +9 m nach LateUpdateBehavior, vor
    // BeginRendering wieder weg). Steht die Kamera noch auf dem zuletzt
    // geschriebenen Wert, wird NICHT nochmal addiert -- bei Leon bleibt es damit 1:1.
    void bino_apply_offset();
    std::optional<glm::vec3> m_bino_last_written{};

    // ------------------------------------------------------------------
    // Zustands-Erkennungen
    // ------------------------------------------------------------------
    bool stage_parent_chain_has(const char* prefix);
    bool is_throwsight_stage() { return stage_parent_chain_has("gm02_500_00_1"); }
    bool is_boat_stage() { return stage_parent_chain_has("gm02_500_00_2"); }
    bool is_ada_raw_a_zone();
    bool is_symbol_riddle();
    bool is_turret_mounted();
    bool is_any_menu_open(bool ignore_hud_off = false);
    bool is_map_open_now();
    bool is_chapter_result_gui_open();
    bool is_file_reader_gui_open();
    bool is_in_inventory_menu();
    bool is_ada_active();
    bool is_mercs_active();
    bool player_in_grapple();
    bool player_in_battle();
    bool player_on_gigante();
    bool is_dodge_gimmick_now();
    bool is_rect_prompt_now();
    bool is_leon_lb_prompt_now();
    bool is_dodge_prompt_now();
    bool ada_pitch_free();

    std::optional<int32_t> binding_equip_weapon_id();
    bool binding_is_grenade_equipped_live();

    // Enum-Werte, lazy aufgeloest
    std::optional<int32_t> m_turret_gimmick{};
    std::vector<int32_t> m_chapter_gui_vals{};
    std::vector<int32_t> m_file_reader_gui_vals{};
    int32_t m_render_default_val{0};
    bool m_chapter_enums_done{false};
    bool m_file_enums_done{false};
    void resolve_chapter_gui_enums();
    void resolve_file_reader_enums();

    // ------------------------------------------------------------------
    // Prompt-Stempel (aus dem GUI-Draw-Hook)
    // ------------------------------------------------------------------
    double m_rect_prompt_last{-999.0};
    double m_leon_lb_last{-999.0};
    double m_dodge_circle_last{-999.0};
    double m_ada_pitch_gui_last{-999.0};
    double m_inventory_shown_t{0.0};

    // [PROMPT_GRIP] EINE Prioritaetskette (Rect > Ausweichen > LB > Container >
    // Gimmick), pro Prompt-Fenster genau EIN Impuls -- ein Level-Trigger
    // schickte den Knopf jeden Frame raus, und ein B, das nach dem Ausweichen
    // noch anliegt, ist im Normalzustand CROUCH.
    struct PGrip {
        bool fired{false};
        const char* btn{nullptr};
        int32_t frames{0};
        double until_t{0.0};
        double win_t{-999.0};
    } m_pgrip{};

    void apply_prompt_grip(Frame& f, bool grip_down);

    // [PROMPT_RT] Dasselbe Muster fuer den Finisher-RT.
    struct RtPrompt {
        bool fired{false};
        int32_t frames{0};
        double until_t{0.0};
        double win_t{-999.0};
    } m_rtp{};

    void apply_finisher_rt(Frame& f, bool window_on, bool trigger_down);

    // ------------------------------------------------------------------
    // Kombos / Timer
    // ------------------------------------------------------------------
    std::optional<double> m_dual_trigger_start_clock{};
    bool m_dual_trigger_fired{false};
    std::optional<double> m_dual_trigger_gap_clock{};
    int32_t m_start_hold_timer{0};

    std::optional<double> m_ee_flame_clock{};
    bool m_ee_flame_fired{false};
    std::optional<double> m_ee_flame_gap_clock{};
    void grant_flamethrower_to_armoury();

    struct LaHold {
        bool pressed{false};
        int32_t frames{0};
        bool fired_long{false};
        int32_t short_timer{0};
        bool short_is_ada{false};
        int32_t long_timer{0};
        bool long_is_ada{false};
        bool long_consumed{false};
        bool back_guard{false};
        bool ks_x_guard{false};
    } m_la{};

    // [QUICKTURN_180 / SNAPTURN]
    struct QuickTurn {
        static constexpr float DOWN = -0.7f;
        static constexpr float RELEASE = -0.3f;
        static constexpr float WINDOW = 0.17f;
        static constexpr int32_t POST_FRAMES = 60;

        int32_t phase{0};
        float window_time{0.0f};
        int32_t seq{0};
        bool cooldown{false};
        int32_t post{0};
        double t0{0.0};
        bool st_armed{true};
        float turn_left{0.0f};
        double turn_last{0.0};
    } m_qt{};

    void qt_reset();
    void qt_update(float ly, float dt);
    void add_camera_yaw(float delta);

    // ------------------------------------------------------------------
    // Flanken-Erkennung mit Halte-Timer
    // ------------------------------------------------------------------
    struct Edge {
        bool prev{false};
        int32_t timer{0};
    };

    Edge m_edge_r_a{}, m_edge_r_b{}, m_edge_r_jc{}, m_edge_l_a_b{};
    bool edge_detect(Edge& e, bool pressed);

    // ------------------------------------------------------------------
    // Granate / Messer
    // ------------------------------------------------------------------
    bool m_prev_l_grip{false};
    int32_t m_grenade_throw_cooldown{0};
    int32_t m_grenade_rt_pulse_frames{0};
    bool m_prev_vr_grenade_throw{false};

    // ------------------------------------------------------------------
    // REF-Overlay (Plugin-API ueber Lua)
    // ------------------------------------------------------------------
    bool m_ref_overlay_synced{false};
    int32_t m_ref_overlay_tick{0};
    bool m_lt_b_overlay_was{false};
    void overlay_set_enabled(bool on);
    void overlay_tick();
    void play_body_sound(uint32_t id);
    void play_gui_sound(int32_t enum_val);

    // ------------------------------------------------------------------
    // Ada: rechtes A kurz = A, lang = RB
    // ------------------------------------------------------------------
    struct AdaRa {
        bool prev{false};
        std::optional<double> down_t{};
        bool fired_rb{false};
        int32_t a_timer{0};
        int32_t rb_timer{0};
        double a_until{0.0};
        double rb_until{0.0};
        double last_a{-999.0};
    } m_ada_ra{};

    // [SCOPE_GRIP_DELAY] Der Zustand MUSS den Frame ueberdauern -- ein local im
    // Frame-Handler sah jedes Mal eine neue Druckflanke und sperrte das Aim
    // dauerhaft (genau das ist beim ersten Versuch passiert).
    std::optional<double> m_scope_grip_edge_t{};
    bool m_scope_grip_prev{false};
    bool scope_grip_aim_blocked(bool grip_now);

    // [CHOKE-ACHSENSPERRE] Drei unabhaengige Riegel gegen Haengenbleiben.
    std::optional<double> m_choke_axis_block_t{};
    bool m_choke_axis_block_warned{false};

    // [LT_FLIP] Messer links / Taschenlampe: kurzer Tap = Flip, Halten = DPAD
    bool m_lt_flip_prev{false};
    bool m_lt_flip_hold{false};
    double m_lt_flip_press_t{0.0};

    // [BURST] Trigger-Flanke fuer das native Feuer-Gate
    bool m_burst_prev_rt{false};

    // ------------------------------------------------------------------
    // Native Gates (sdk.hook) -- brauchen einen GAME-NEUSTART
    // ------------------------------------------------------------------
    bool m_hooks_installed{false};
    void install_hooks();

    // [PLAYWORK-PAUSE NOTAUS] Fuenf Riegel, getickt statt gehookt.
    struct PlayWork {
        std::optional<double> paused_since{};
        double last_check{-999.0};
        bool already_freed{false};
        bool had_ctx{false};
        std::optional<double> check_at{};
        std::optional<double> load_t{};
    } m_pw{};

    void playwork_pause_tick();

    // Steht Mendez (Ch1f4z1Context / Ch1f5z1Context) in der Szene? Dann
    // bleibt alles normal und der Pause-Notaus haelt sich raus.
    bool mendez_in_scene();

    // [MERCHANT-PAUSE 15.09.2026] Haengender Eintrag der NORMALEN Pause-Liste
    // (Owner InGameMenuOccupiedModule) nach dem Haendler -- eigener Notaus.
    void merchant_pause_tick();


    double m_mp_last_check{0.0};
    double m_mp_next{0.0};
    std::optional<double> m_mp_since{};
    int32_t m_mp_freed{0};

    bool m_cfg_loaded{false};
};

#endif // RE4
