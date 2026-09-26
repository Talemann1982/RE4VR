// ============================================================================
// RE4VRUi -- 1:1-Portierung von re4_vr_ui.lua (1.240 Zeilen).
//
// Sechs Bausteine:
//   (1) Fork-Erkennung   __re4_fork_ok/_mono/_gui_matrix/_canvas + __re4_fork_check
//   (2) Mono-Broker      die EINZIGE Stelle, die set_mono_rendering ruft
//   (3) Karten-Fix       gui_matrix, gui_elem, mono, canvas, suspend, mapglue
//   (4) GUI-Ausblenden   global, Karte, Koffer, Hauptmenue
//   (5) Waldmenue        Stage 48000 als Menue-Kulisse
//   (6) Zeigestrahl      Fork-Wert aus der JSON setzen
//
// Spezifikation: I:\LUATRANS\PORT_UI_SPEC.md
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- ans ENDE, zu den uebrigen portierten Modulen.
//
// * Der Mono-Broker muss die Anmeldung von RE4VRFirstPerson (Raetsel-Mono)
//   frisch sehen -- das steht HINTER dem ScriptRunner, also muessen wir noch
//   weiter hinten stehen. In Lua galt dasselbe (firstperson `f` < ui `u`).
// * Dafuer sehen wir die Anmeldung aus re4_vr_weapons.lua kuenftig frisch
//   statt einen Pass alt. Folgenlos: der Broker ist idempotent und zieht Mono
//   JEDEN Frame nach; ein Frame frueher oder spaeter ist bei einem Zustand wie
//   Mono-Rendering nicht wahrnehmbar. Dasselbe gilt fuer __re4_scope_id.
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

class RE4VRUi : public Mod {
public:
    static std::shared_ptr<RE4VRUi>& get();

    std::string_view get_name() const override { return "RE4VRUi"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)
    void draw_public_2d_cutscenes();   // [CUT2D] "Enable 2D Cutscenes" (MOD OPTIONS)

    bool on_pre_gui_draw_element(::REComponent* element, void* context) override;

    // [MONO-BROKER] _G.__re4_mono_request(id, on). Die EINZIGE Stelle, die
    // set_mono_rendering ruft -- sonst nehmen sich Karte, Raetsel und andere
    // Nutzer den Zustand gegenseitig weg (Doppelregler-Falle).
    // RE4VRFirstPerson ruft es fuer "symbol_riddle", re4_vr_weapons.lua ebenso.
    void mono_request(const std::string& id, bool on);

    // [FORK-CHECK] _G.__re4_fork_check() -- stoesst die Erkennung an und
    // liefert das Ergebnis ueber die Globals zurueck.
    void on_fork_check();

private:
    // ------------------------------------------------------------------
    // Zustand der Fork-Schalter -- "haben WIR ihn gerade gesetzt?"
    // ------------------------------------------------------------------
    struct State {
        bool applied{false};    // GUI-Projektions-Override abgeschaltet?
        bool elem{false};       // Element-Override abgeschaltet?
        bool mono{false};       // Mono eingeschaltet?
        bool canvas{false};     // Flatscreen-Leinwand?
        bool suspend{false};    // VR ausgesetzt?
        bool mapglue{false};    // Karten-Pin?
        bool map_open{false};
        bool inv_open{false};
        bool menu_open{false};
    } m_state{};

    // ------------------------------------------------------------------
    // Config (re4_vr/re4_vr_ui.json). VERZOEGERT gespeichert.
    // ------------------------------------------------------------------
    struct Opt {
        bool gui_matrix{true};   // bisheriges Verhalten
        // [2026-08-09] Startwert AUS -- gehoert sachlich zu gui_matrix, hat die
        // driftenden Icons aber nicht repariert.
        bool gui_elem{false};
        bool mono{false};        // brachte bei der Karte nichts
        bool canvas{false};      // dito
        bool suspend{false};     // [2026-08-10] getestet, bringt nichts
        bool binoglue{false};
        bool mapglue{false};
        // [CUT2D 26.09.2026] Echte Cutscenes auf der Flatscreen-Leinwand. Default AN (User 26.09.,
        // gilt, wenn die JSON den Schluessel nicht hat); aus = kein einziger zusaetzlicher Aufruf,
        // der Canvas-Zustand bleibt dann bitgleich zum bisherigen Stand.
        bool cutscene_2d{true};
    } m_opt{};

    bool m_cut2d_active{false};   // [CUT2D] Leinwand gerade wegen einer Cutscene an
    // [CUT2D_KICK 26.09.2026] Kurz nach Cutscene-Start die Leinwand 2 Frames aus und wieder an.
    // Belegt vom User: in 2D ist die Cutscene sonst reingezoomt, Aus/An (bzw. Reset Scripts) heilt.
    double m_cut2d_kick_at{0.0};   // 0 = nichts geplant
    int m_cut2d_kick_frames{0};    // > 0 = Leinwand gerade fuer den Kick aus
    bool cut2d_show() const { return m_cut2d_active && m_cut2d_kick_frames == 0; }
    void cutscene_2d_tick();

    float m_glue_distance{1.5f};
    // Bei exakt 0 liegen alle Ebenen ineinander, dann entscheidet die
    // Zeichenreihenfolge -- und eine deckende Ebene schluckt den Umriss.
    float m_glue_gap{0.002f};
    float m_canvas_width{2.5f};
    float m_canvas_distance{2.0f};
    float m_ui3101_scale{1.0f};

    // [CUT2D_UI0200 26.09.2026 -- Ansage des Users] Gui_ui0200 (Position + Groesse) NUR solange die
    // Cutscene-Leinwand laeuft (m_cut2d_active). Sonst wird Gui_ui0200 nicht angefasst; beim Ende
    // der Cutscene kommt der gemerkte Originalstand zurueck.
    float m_cut_ui0200_x{0.0f};
    float m_cut_ui0200_y{0.0f};
    float m_cut_ui0200_scale{1.0f};
    bool m_cut_ui0200_applied{false};
    ::REManagedObject* m_cut_ui0200_ctrl{nullptr};
    glm::vec3 m_cut_ui0200_orig_pos{};
    glm::vec3 m_cut_ui0200_orig_scale{1.0f, 1.0f, 1.0f};
    void cut_ui0200_apply(::REManagedObject* go);

    // [GAME_LOGO_VR 26.09.2026] "VR" auf der VR-Menuetafel, solange das Spiel Gui_ui1001 zeigt.
    float m_game_logo_x{0.5f};
    float m_game_logo_y{0.5f};
    float m_game_logo_scale{2.0f};
    // Unser Default; der Fork-Default waere -35.0.
    float m_ptr_pitch{-45.0f};

    bool m_hide_3121{true};
    bool m_hide_bg{true};
    bool m_forest_menu{true};

    // [KAPUTTE LOCALS -- s. PORT_UI_SPEC Abschnitt 1] map_hide, glue_on und
    // bino_distance stehen in Lua in save_cfg/load_cfg VOR ihrer
    // local-Deklaration und treffen dort ein GLOBAL. Sie werden deshalb NIE
    // gespeichert und NIE geladen -- belegt durch die Live-JSON, in der genau
    // diese drei Schluessel fehlen. Reine Laufzeitwerte, kein Config-Anschluss.
    std::unordered_map<std::string, bool> m_map_hide{};
    std::unordered_map<std::string, bool> m_glue_on{};
    float m_bino_distance{1.0f};

    std::unordered_map<std::string, bool> m_global_hide{};
    std::unordered_map<std::string, bool> m_vt_on{};

    void load_cfg();
    void save_cfg();
    std::optional<double> m_cfg_dirty_t{};

    // ------------------------------------------------------------------
    // (1) Fork-Erkennung
    // ------------------------------------------------------------------
    void run_probe();
    bool m_probe_done{false};

    // ------------------------------------------------------------------
    // (2) Mono-Broker
    // ------------------------------------------------------------------
    std::unordered_set<std::string> m_mono_reqs{};
    bool m_mono_state{false};
    bool m_mono_fail{false};
    void mono_apply();

    // ------------------------------------------------------------------
    // (3) Die Schalter -- idempotent, jeder mit eigener Faehigkeitspruefung
    // ------------------------------------------------------------------
    bool is_supported();
    void apply_override(bool want);
    void apply_elem(bool want);
    void apply_mono(bool want);
    void apply_canvas(bool want);
    void apply_suspend(bool want);
    void apply_mapglue(bool want);
    void push_glue_layers();

    std::optional<bool> m_supported{};
    std::optional<bool> m_elem_supported{};
    std::optional<bool> m_suspend_supported{};
    std::optional<bool> m_glue_supported{};
    std::optional<bool> m_ptr_pitch_ok{};
    std::optional<float> m_ptr_pitch_sent{};
    void apply_ptr_pitch();

    // ------------------------------------------------------------------
    // Zustandsabfragen -- Singletons NICHT dauerhaft halten
    // ------------------------------------------------------------------
    bool is_map_gui_open();
    bool is_inventory_open();
    bool is_main_menu_open();

    ::REManagedObject* m_map_manager{nullptr};
    ::REManagedObject* m_case_manager{nullptr};
    ::REManagedObject* m_gui_manager{nullptr};

    // ------------------------------------------------------------------
    // (4) GUI-Zugriff
    // ------------------------------------------------------------------
    ::REManagedObject* gui_view(::REManagedObject* go);
    ::REManagedObject* gui_root_control(::REManagedObject* go);

    void apply_viewtype(::REManagedObject* go, const std::string& name, bool want);
    bool force_viewtype(::REManagedObject* go, const std::string& name);

    // Nur solange umgestellt: der urspruengliche ViewType je Ebene.
    std::unordered_map<std::string, int32_t> m_vt_orig{};
    // Zuletzt GELESENER ViewType -- Diagnose fuer die Statuszeile.
    std::unordered_map<std::string, std::string> m_vt_seen{};
    // Notaus: absolute Zielwerte, die beim naechsten Zeichnen erzwungen werden.
    std::unordered_map<std::string, int32_t> m_vt_force{};

    // ------------------------------------------------------------------
    // (5) Waldmenue
    // ------------------------------------------------------------------
    void forest_apply(bool force);
    struct ForestOrig {
        int32_t village{0};
        int32_t startup{0};
    };
    std::optional<ForestOrig> m_forest_orig{};
    double m_forest_last_t{0.0};
    std::optional<bool> m_forest_applied{};

    // ------------------------------------------------------------------
    // Fernglas
    // ------------------------------------------------------------------
    std::optional<double> m_bino_seen_t{};

    // [SCOPE_HIDE] GameObject-Namen der Mercs-/Karten-HUDs, die beim Suchen
    // anfallen -- der Draw-Hook blendet genau diese im Scope aus.
    bool m_scopehide_seen{false};
    bool scopehide_attached();

    bool m_cfg_loaded{false};
};

#endif // RE4
