// ============================================================================
// RE4VRMaterials -- 1:1-Portierung von re4_vr_materials.lua (1162 Zeilen,
// Stand 02.09.2026 23:01).
//
// Blendet am GESTEUERTEN Spielerkoerper Kopf und Haare aus, in bestimmten
// Zustaenden den ganzen Koerper, die Waffen und die Kostuem-Accessoires.
// Ausgeblendet wird ueber den FARB-Pass -- der Schattenwurf bleibt.
//
// Dazu drei Sonderwege, die alle teuer erkaempft sind:
//   * Jacke + Kragenfell, die auf DrawDefault nicht hoeren (Szenen-Suche)
//   * Leons Taschenlampe und Ashleys Oellampe (set_Enabled statt DrawDefault)
//   * die glitchenden Gondeln in Stage 60850
//
// Spezifikation: I:\LUATRANS\PORT_MATERIALS_SPEC.md (Fassung 2)
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RE4VR.hpp"

class RE4VRMaterials : public Mod {
public:
    static std::shared_ptr<RE4VRMaterials>& get();

    std::string_view get_name() const override { return "RE4VRMaterials"; }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

private:
    // ---------------------------------------------------------------- Typen

    // Der gecachte Materialbefund eines Renderers (Lua: scan_renderer_mats).
    // Wird beim SUCHEN einmal ermittelt; das ANWENDEN kommt danach ohne einen
    // einzigen Engine-Read aus.
    struct MatInfo {
        bool valid{false};              // Lua-nil, wenn get_MaterialNum keine Zahl liefert
        int32_t count{0};
        bool hh_only{false};
        std::vector<bool> hh;           // Material mi steht in HIDE_MATERIALS
        std::vector<bool> extra;        // Material mi steht in FULLHIDE_EXTRA_MATS
    };

    // Ein Trefferlisten-Eintrag (Lua: classify_go). Enthaelt NUR
    // Unveraenderliches; alles Zustandsabhaengige wird erst beim Anwenden
    // gelesen.
    // [REF] Ein Handle plus die Auskunft, ob WIR darauf eine Referenz halten.
    // keep() ref't nur bei referenceCount > 0 (so macht es sol auch), also darf
    // drop() nicht bedingungslos releasen -- sonst Refcount-Unterlauf.
    struct RefHandle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    struct Hit {
        ::REManagedObject* go{nullptr};
        ::REManagedObject* mesh{nullptr};
        ::REManagedObject* skin{nullptr};
        bool go_reffed{false};
        bool mesh_reffed{false};
        bool skin_reffed{false};
        bool is_holster{false};
        bool is_weapon{false};
        bool is_accessory{false};
        bool fullhide_by_name{false};
        MatInfo mesh_mats{};
        MatInfo skin_mats{};
    };

    // Ein gefundenes Extra-Material (Jacke). Lua: { mesh = , idx = , name = }.
    struct ExtraMat {
        ::REManagedObject* mesh{nullptr};
        int32_t idx{0};
        std::string name;
        bool reffed{false};
    };

    // -------------------------------------------------------- Namenstabellen
    static const std::unordered_set<std::string> PLAYER_BODY_NAMES;
    static const std::unordered_set<std::string> FALLBACK_BODY_NAMES;
    static const std::unordered_set<std::string> HIDE_MATERIALS_LEON;

    // [PROPS_MAT 16.09.2026] Leon-Liste + "Props_Mat" -- gilt NUR in der
    // Kampagne (s. on_frame). In den Mercenaries traegt Leon denselben
    // Bodynamen, dort bleibt es bei HIDE_MATERIALS_LEON.
    static const std::unordered_set<std::string> HIDE_MATERIALS_LEON_CAMPAIGN;
    static const std::unordered_set<std::string> HIDE_MATERIALS_ASHLEY;
    static const std::unordered_set<std::string> HIDE_MATERIALS_ADA;
    static const std::unordered_set<std::string> FULLHIDE_EXTRA_MATS;
    static const std::unordered_set<std::string> FULLHIDE_GO_NAMES;
    static const std::unordered_set<std::string> HIDE_ACCESSORY_GO;
    static const std::unordered_set<std::string> LAMP_GO_NEVER_HIDE;

    // Lua: HIDE_MATERIALS_BY_BODY -- Body-Name auf die passende Liste.
    // Unbekannter Body -> Leon-Liste (Lua Z.1017 `or HIDE_MATERIALS_LEON`).
    const std::unordered_set<std::string>* materials_for_body(const std::string& body_name) const;

    // ------------------------------------------------------------- Helfer
    ::REManagedObject* get_scene();
    ::REManagedObject* get_body_go();
    ::REManagedObject* get_body_go_cached();

    void set_mat(::REManagedObject* renderer, int32_t mi, bool enable);
    // shadow: -1 = nicht anfassen (Lua nil), 0 = false, 1 = true
    void set_mesh_draw(::REManagedObject* renderer, bool draw_color, int shadow);

    MatInfo scan_renderer_mats(::REManagedObject* renderer) const;
    void hide_mats_on(::REManagedObject* renderer, bool fullhide_go, const MatInfo& minfo);

    static bool is_weapon_name(const std::string& nm);
    bool is_accessory_name(const std::string& nm) const;

    // ---------------------------------------------------- Suchen / Anwenden
    bool classify_go(::REManagedObject* go, const std::string& nm, bool in_weapon,
                     bool in_accessory, Hit& out);
    void apply_entry(const Hit& e);
    bool apply_hits();
    void scan_start(::REManagedObject* root_tf);
    // budget < 0 = bis zum Ende durchlaufen (Lua: budget = nil)
    void scan_step(int budget);
    bool state_changed();
    void update_choke_victim();
    void materials_tick(::REManagedObject* body, ::REManagedObject* tf);

    // ------------------------------------------------------ Jacke und Fell
    bool extra_mats_alive();
    void scan_extra_mats();
    void apply_extra_mats(bool off);
    void clear_extra_cache();

    // ---------------------------------------------------------- Lampen
    ::REManagedObject* get_lamp_go();
    ::REManagedObject* get_flashlight_go();
    ::REManagedObject* fl_mesh_now();

    // ---------------------------------------------------------- Gondeln
    std::optional<int32_t> gondola_stage();
    void gondola_set_tree(::REManagedObject* tf, bool enable);
    void gondola_unhide();
    void gondola_tick();
    void gondola_load();
    void gondola_save();

    // ------------------------------------------------- Referenzzaehlung
    // [REF] Alles, was ueber Frames gehalten wird, braucht eine eigene
    // Referenz -- in Lua erledigt das sol beim Ablegen im Table.
    // Rueckgabe: ob wirklich eine Referenz genommen wurde.
    static bool keep(::REManagedObject* o);
    static void drop(::REManagedObject* o, bool reffed);
    void clear_hits();
    void clear_scan_list();
    void clear_scan_stack();
    void store_cached_body(::REManagedObject* go);
    void store_lamp_go(::REManagedObject* go);
    void store_fl_go(::REManagedObject* go);

    // [BODY-EPOCH 2026-09-22] Spieler-Body gewechselt (Save-Load/Tod) ->
    // gemerkte Body-Zeiger weg, s. re4vr::body_epoch().
    void drop_body_caches();
    uint64_t m_body_epoch{0};

    // ---------------------------------------------------------- Typen (TDB)
    sdk::RETypeDefinition* m_scene_td{nullptr};
    ::REManagedObject* m_t_mesh{nullptr};
    ::REManagedObject* m_t_skin{nullptr};      // in RE4 IMMER nullptr, s. Spec 14.2
    ::REManagedObject* m_t_oillamp{nullptr};
    ::REManagedObject* m_t_fur{nullptr};
    ::REManagedObject* m_t_shellfur{nullptr};

    // ---------------------------------------------------------- Zustand
    bool m_mat_set_enable{false};
    bool m_fp_only_now{false};
    bool m_scope_body_hide{false};    // Lua Z.1102: fest false
    bool m_holster_hide_now{false};
    bool m_ashley_now{false};
    const std::unordered_set<std::string>* m_hide_materials{&HIDE_MATERIALS_LEON};

    ::REManagedObject* m_cached_body{nullptr};
    bool m_cached_body_reffed{false};
    // [NUR UNSER CHARAKTER 14.09.2026] Fuenf-Zeichen-Praefix des Bodys, an dem
    // der Suchlauf startet -- "ch0a0" (Leon Kampagne), "ch6i2" (Krauser Mercs),
    // "ch3a8" (Ada) usw. Alles, was wie ein Charakter heisst und ein ANDERES
    // Praefix hat, wird samt Unterbaum uebersprungen. Leer = Gate aus.
    std::string m_scan_root_prefix;

    uintptr_t m_choke_victim{0};      // 0 = kein Griff (Lua: nil)

    // Suchlauf
    // [REF] Der Stapel ueberlebt Frames -- in Lua haelt sol_lua_push jedes
    // gestapelte Transform am Leben, solange es in der Tabelle steht.
    std::vector<::REManagedObject*> m_st_tf;
    std::vector<char> m_st_tf_reffed;
    std::vector<char> m_st_w;
    std::vector<char> m_st_a;
    std::vector<Hit> m_scan_list;
    std::vector<Hit> m_hits;
    bool m_scan_busy{false};
    bool m_force_full{true};
    uintptr_t m_last_body_addr{0};
    bool m_have_last_body_addr{false};

    // Signatur fuer state_changed(). Lua startet mit nil -> der erste Aufruf
    // meldet IMMER eine Aenderung; das bildet m_sig_init ab.
    bool m_sig_init{false};
    bool m_sig_fp{false};
    bool m_sig_sc{false};
    bool m_sig_ho{false};
    bool m_sig_ms{false};
    bool m_sig_ch{false};

    // [PROPS_MAT 16.09.2026] Zuletzt benutzte Materialliste -- ein Wechsel
    // zwingt die Suche zu einer vollen Runde (s. state_changed).
    const std::unordered_set<std::string>* m_sig_hm{nullptr};

    // Jacke und Fell
    std::vector<ExtraMat> m_extra_mats;
    std::vector<RefHandle> m_extra_furs;
    bool m_extra_mats_off{false};
    double m_extra_scan_t{0.0};

    // Lampen
    ::REManagedObject* m_lamp_go_cache{nullptr};
    bool m_lamp_go_reffed{false};
    bool m_lamp_hidden{false};
    ::REManagedObject* m_fl_go_cache{nullptr};
    bool m_fl_go_reffed{false};
    bool m_fl_mesh_hidden{false};

    // Gondeln
    bool m_gondola_hide_cfg{true};
    std::vector<RefHandle> m_gondola_hidden;
    double m_gondola_next_t{0.0};
};

#endif // RE4
