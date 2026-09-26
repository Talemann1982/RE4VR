// ============================================================================
// RE4VRMerc -- 1:1-Portierung von re4_vr_merc.lua (1.981 Zeilen).
//
// ALLES Mercenaries-Spezifische (ausdruecklicher Wunsch des Users) -- die
// Kampagnen-Scripte bleiben unberuehrt. Neun Bausteine:
//   (1) Moduserkennung        detect + Body-Name-Gegenprobe
//   (2) Kopf/Haare aus        collect_hh / apply_hide
//   (3) Voll-Aus in KS3/KS5   apply_full_hide (+ Jacke/Fell ueber die Szene)
//   (4) Waffen-Offset         __re4_merc_wep_apply (motion ruft es)
//   (5) Compound-Bow-Posen    __re4_merc_apply_bow_pose (motion ruft es)
//   (6) Bow-Pin               wp6304 nativ an R_Hand
//   (7) HUD                   Groesse/Position + Ausblenden im Scope
//   (8) BulletRush            Weskers Ragemodus als KS4 melden
//   (9) Laser-Dot             der fehlende rote Punkt an drei Mercs-Waffen
//
// Spezifikation: I:\LUATRANS\PORT_MERC_SPEC.md
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- **zwischen RE4VRChoke und RE4VRMotion**:
//
//   RE4VR -> RE4VRChoke -> **RE4VRMerc** -> RE4VRMotion -> ScriptRunner -> ...
//
// Alphabetisch gilt re4_vr_choke.lua < re4_vr_merc.lua < re4_vr_motion.lua
// (c < me < mo) -- die drei Module bilden diese Kette 1:1 ab.
// * merc las die motion-Globals (__vr_dbg_wep_id, __re4_knife_hand) im
//   VORFRAME-Stand -- bleibt so, weil motion dahinter steht.
// * motion las die merc-Globals FRISCH (__re4_in_mercs, __re4_merc_bow_pinned)
//   und ruft __re4_merc_wep_apply / __re4_merc_apply_bow_pose direkt -- bleibt so.
// * Von den Lua-Dateien hinter uns kippt nur binding, das ausschliesslich in
//   on_frame schreibt -- dieselbe Abwaegung wie bei motion und choke.
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

class RE4VRMerc : public Mod {
public:
    static std::shared_ptr<RE4VRMerc>& get();

    std::string_view get_name() const override { return "RE4VRMerc"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_application_entry(void* entry, const char* name, size_t hash) override;
    bool on_pre_gui_draw_element(::REComponent* element, void* context) override;

    // [BOW_POSE] _G.__re4_merc_apply_bow_pose -- RE4VRMotion ruft es im
    // BeginRendering-POST-Pass (im Pre-Pass wuerde die Engine-Anim die Pose
    // jeden Frame ueberschreiben).
    void apply_bow_pose();

    // [MERC_WEP_OFFSET] _G.__re4_merc_wep_apply(wpos, wrot, wid, hand_rot).
    // Rueckgabe false = "nicht zustaendig" -> motion behaelt seine Werte.
    bool wep_apply(glm::vec3& pos, glm::quat& rot, int32_t wid, const glm::quat& hand_rot);

    // [MERCS_HUD] Der HUD-Block muss die einmal gelesene Nulllage von
    // Gui_ui2710/2764 sofort sichern -- sonst wird sie beim naechsten Start neu
    // gemessen und dann vom eigenen Offset verfaelscht.
    void save_cfg();

private:
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    void store(Handle& h, ::REManagedObject* o);
    void drop(Handle& h);

    // ------------------------------------------------------------------
    // Charaktere
    // ------------------------------------------------------------------
    struct MercChar {
        int32_t kind;
        const char* body;
        const char* name;
        const std::unordered_set<std::string>* hide;
        const std::unordered_set<std::string>* extra;   // darf nullptr sein
        const char* arm_key;
    };

    static const std::array<MercChar, 6>& merc_chars();
    static const MercChar* merc_char(int32_t kind);
    static bool is_merc_body(const std::string& name);

    // ------------------------------------------------------------------
    // (1) Erkennung
    // ------------------------------------------------------------------
    ::REManagedObject* get_merc_mgr();
    ::REManagedObject* get_campaign_mgr();
    ::REManagedObject* get_char_mgr();

    // Lua: `return (gui ~= nil), cid`
    bool detect(std::optional<int32_t>& cid_out);

    // Lua liefert drei Werte: kind, name, tf. Ein blankes `return nil` waere
    // die KEIN-WERT-FALLE (merc.lua:1082) -- deshalb immer alle drei.
    struct PlayerBody {
        std::optional<int32_t> kind{};
        std::string name{};
        bool name_ok{false};
        ::REManagedObject* tf{nullptr};
    };
    PlayerBody get_player_body();

    // ------------------------------------------------------------------
    // (2)+(3) Kopf/Haar und Voll-Aus
    // ------------------------------------------------------------------
    struct HhEntry {
        Handle mesh{};
        std::optional<int32_t> idx{};   // gesetzt = nur DIESER Materialslot
        std::string name{};
    };

    bool collect_hh(::REManagedObject* tf, const std::unordered_set<std::string>* hide,
                    const std::unordered_set<std::string>* extra);
    bool collect_all_meshes(::REManagedObject* tf);
    void clear_hh();
    void clear_all_meshes();

    bool show_head_now();
    bool full_hide_now();
    bool grapple_head_exception();

    void apply_hide();
    void apply_full_hide();

    // [EXTRA_MATS] Materialien, die im Voll-Aus nicht auf DrawDefault hoeren.
    // Gesucht wird SZENENWEIT, deshalb die Kampagne-Sperre in apply_full_hide.
    struct ExtraMat {
        Handle mesh{};
        int32_t idx{0};
        std::string name{};
    };

    // [SZENENWEG 2026-09-09] Eigener, kleiner Satz neben den ExtraMats.
    // Grund s. scan_scene_mats() in der .cpp.
    bool scene_cache_alive();
    void scan_scene_mats(int32_t kind);
    void apply_scene_mats(bool off);
    bool under_player_body(::REManagedObject* go, const std::string& body);

    bool extra_cache_alive();
    void scan_extra_mats();
    void apply_extra_mats(bool off,
                          const std::unordered_set<std::string>* only = nullptr);

    // ------------------------------------------------------------------
    // (4)+(5)+(6) Waffe, Posen, Pin
    // ------------------------------------------------------------------
    struct WepOff {
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
    };

    WepOff& wep_off_for(int32_t wid);

    // Posen: [name] -> { bone -> quat }. Rohdaten bleiben unveraendert in der
    // JSON, gespiegelt wird zur Laufzeit.
    using PoseBones = std::unordered_map<std::string, glm::quat>;
    const PoseBones* bow_mirrored(const std::string& name);
    PoseBones bow_mirror_bones(const PoseBones& src) const;

    void update_bow_pin();
    void bow_unpin();
    // Sucht wp6304 / wp6304_MC / wp6304_AO -- nur unter den DIREKTEN Kindern.
    ::REManagedObject* bow_find_go(::REManagedObject* btf, ::REManagedObject** tf_out);

    // ------------------------------------------------------------------
    // (7) HUD
    // ------------------------------------------------------------------
    struct HudCfg {
        bool on{true};
        float scale{1.0f};
        float x{0.0f};
        float y{0.0f};
        // Nulllage. Fuer die Behavior-Eintraege FEST im Code (ueber 12
        // Messungen bitgenau konstant), fuer die Namens-Eintraege zunaechst
        // leer und beim ERSTEN Zeichnen einmalig gelesen -- nie nachmessen.
        std::optional<float> bx{};
        std::optional<float> by{};
        const char* type{nullptr};    // Behavior-Typ (nullptr bei Namens-Eintrag)
        const char* field{nullptr};
        bool to_parent{false};
        const char* go{nullptr};      // GameObject-Name statt Behavior
    };

    void init_hud();
    HudCfg* hud_get(const char* key);
    void hud_apply();
    void hud_ui();
    ::REManagedObject* find_ctrl(const HudCfg& cfg);
    ::REManagedObject* root_ctrl(::REManagedObject* go);
    static ::REManagedObject* child_by_name(::REManagedObject* ctrl, const char* want);

    // ------------------------------------------------------------------
    // (8) BulletRush
    // ------------------------------------------------------------------
    void update_bulletrush();

    // [BODY-EPOCH 2026-09-22] Spieler-Body gewechselt (Save-Load/Tod) ->
    // gemerkte Body-Zeiger weg, s. re4vr::body_epoch().
    void drop_body_caches();
    uint64_t m_body_epoch{0};

    // ------------------------------------------------------------------
    // (9) Laser-Dot
    // ------------------------------------------------------------------
    ::REManagedObject* dot_gun();
    ::REManagedObject* dot_find(::REManagedObject* go, int depth);
    void dot_tick();
    std::optional<uintptr_t> player_body_addr();

    // ------------------------------------------------------------------
    // [BOW_KEEP] Der Hook auf requestEquipBareHand
    // ------------------------------------------------------------------
    void install_bow_keep_hook();
    bool bow_keep_should_skip();

    void load_cfg();

    // ==================================================================
    // Zustand
    // ==================================================================
    Handle m_merc_mgr{};
    Handle m_campaign_mgr{};
    Handle m_char_mgr{};

    int m_frames{0};
    std::optional<bool> m_last_state{};
    std::optional<int32_t> m_last_kind{};
    // [SW_VS_MERCS] haelt den Zustand, wenn der Body gerade nicht lesbar ist
    bool m_merc_body_ok{false};
    bool m_round_gap{false};

    bool m_hide_enabled{true};   // Desktop-Haken, NICHT in der Config

    std::vector<HhEntry> m_hh_meshes{};
    bool m_hh_set{false};
    std::string m_hh_body{};

    std::vector<Handle> m_all_meshes{};
    bool m_all_set{false};
    bool m_full_hidden{false};

    // [SZENENWEG 2026-09-09]
    std::vector<ExtraMat> m_kr_mats{};
    bool m_kr_set{false};
    bool m_kr_off{false};
    double m_kr_scan_t{0.0};
    int32_t m_kr_kind{-1};   // fuer wen der Satz eingesammelt wurde

    std::vector<ExtraMat> m_extra_mats{};
    bool m_extra_set{false};
    bool m_extra_mats_off{false};
    // [RAGE-BARETT 2026-09-09] zuletzt benutzter Material-Filter.
    const std::unordered_set<std::string>* m_extra_only{nullptr};
    std::vector<Handle> m_extra_furs{};
    bool m_extra_furs_set{false};
    double m_extra_scan_t{0.0};

    // Grapple-Messung (laeuft weiter, auch wenn GRAB_HEAD_ON aus ist)
    bool m_grab_hit_logged{false};
    Handle m_grab_holder_mo{};
    bool m_grab_holder_searched{false};
    std::string m_grab_holder_name{"-"};

    // Config
    bool m_wep_off_on{true};
    std::unordered_map<int32_t, WepOff> m_wep_off{};
    bool m_bow_pose_on{true};
    bool m_hide_2770{true};
    bool m_grab_head_on{false};   // [26.08.] Ausnahme widerlegt -> aus
    bool m_hide_timer_bg{true};
    int m_bow_mirror_mode{1};
    float m_bow_pose_blend{1.0f};

    std::unordered_map<std::string, PoseBones> m_bow_poses{};
    // [1:1] Lua schreibt beim Speichern `poses = bow_poses` -- also die GANZE
    // geladene Tabelle unveraendert zurueck, inklusive Feldern, die es selbst
    // nie liest (`src_hand`, `hand`). Wer nur `bones` neu aufbaut, radiert die
    // beim ersten Slider-Klick aus der Datei. Deshalb wird die Rohstruktur
    // gehalten und wortgleich zurueckgeschrieben.
    nlohmann::json m_bow_poses_raw{};
    std::unordered_map<std::string, PoseBones> m_bow_mirror_cache{};
    std::string m_bow_dbg{"noch nichts geschrieben"};

    // Bow-Pin
    bool m_bow_pin_on{true};
    Handle m_bowp_go{};
    Handle m_bowp_tf{};
    bool m_bowp_pinned{false};

    // HUD
    std::vector<std::pair<std::string, HudCfg>> m_hud{};
    std::unordered_map<std::string, Handle> m_hud_cache{};
    std::unordered_set<std::string> m_hud_names{};
    // [HUD-MESSUNG] je Eintrag nur einmal schreiben
    // [HUD-MESSUNG 04.09.] Letzter geloggter Gate-Zustand. Der alte Logger
    // deckelte auf 10 Eintraege a 1 s -- er verstummte also nach ~10 Sekunden,
    // waehrend man noch im Hauptmenue steht, und hat ueber den Mercs-Moment
    // nichts ausgesagt (gemessen: nur "gate=0"-Zeilen). Jetzt wird bei jedem
    // WECHSEL geloggt, also genau beim Betreten von Mercenaries.
    // -2 = noch nie geloggt (echte Werte sind -1, 0, 1).
    double m_hud_scan_t{0.0};

    // BulletRush
    Handle m_br_hu{};
    double m_br_since{0.0};
    // [RAGE-FLANKE 2026-09-09] letzter Rage-Zustand -- an der Flanke werden
    // Kopf/Haar/Barett neu eingesammelt (s. update_bulletrush).
    bool m_br_on_prev{false};

    // Laser-Dot
    struct Dot {
        Handle lsc{};
        std::optional<uintptr_t> key{};
        int tries{0};
        double next_scan{0.0};
        bool body_weg{false};
        bool aimed{false};
        int draw_frames{0};
    } m_dot{};

    // Typen
    ::REManagedObject* m_t_mesh{nullptr};
    ::REManagedObject* m_t_fur{nullptr};
    ::REManagedObject* m_t_shellfur{nullptr};
    ::REManagedObject* m_t_motion{nullptr};
    ::REManagedObject* m_t_lsc{nullptr};
    ::REManagedObject* m_t_gui{nullptr};
    bool m_types_ready{false};
    void ensure_types();

    bool m_cfg_loaded{false};
    bool m_hook_installed{false};
};

#endif // RE4
