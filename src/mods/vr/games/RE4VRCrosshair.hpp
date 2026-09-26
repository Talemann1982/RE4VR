// ============================================================================
// RE4VRCrosshair -- 1:1-Portierung von re4_vr_crosshair.lua (1494 Zeilen).
// Zeilenangaben "Lua Z.xxx" beziehen sich auf diese Datei.
//
// WARUM AUSGERECHNET DIESE DATEI: sie ist der mit Abstand teuerste Posten aller
// Lua-Scripte. Gemessen im Stillstand kosten ALLE Scripte zusammen 0,731 ms --
// davon entfallen 0,549 ms auf einen einzigen Callback, naemlich
// re4_vr_crosshair.lua @ on_pre_application_entry("LockScene") (Lua Z.464).
// Alle uebrigen Module liegen im Bereich 0,01-0,05 ms.
//
// Was das Script tut: Muendungsposition aus dem Waffen-Joint holen, per
// asynchronem Raycast den Trefferpunkt bestimmen, daraus den Reticle-Punkt
// setzen, den nativen Laserstrahl an die Muendung tackern, die HUD-Anzeigen an
// die rechte Hand haengen und Kugeln wie Raketen am VR-Muzzle in VR-Richtung
// starten lassen.
//
// Spezifikation: I:\LUATRANS\PORT_CROSSHAIR_SPEC.md (Fassung 2)
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

class RE4VRCrosshair : public Mod {
public:
    static std::shared_ptr<RE4VRCrosshair>& get();

    std::string_view get_name() const override { return "RE4VRCrosshair"; }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    bool on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Aus den Hooks gerufen (public, weil die Hook-Lambdas frei stehen).
    void hook_pre_request_fire(std::vector<uintptr_t>& args);
    void hook_pre_rocket_generate(std::vector<uintptr_t>& args);
    void hook_pre_concentrate_ratio(std::vector<uintptr_t>& args);
    void hook_pre_apply_concentrate(std::vector<uintptr_t>& args);
    void hook_pre_update_laser(std::vector<uintptr_t>& args);
    void hook_post_update_laser();

    // Von den nach Lua exportierten Zeichen-Funktionen gerufen (Public-Menue).
    void draw_public_crosshair_off();
    // [DOT_CROSSHAIR] RE4VRUi fragt das fuer Gui_ui2041 (Mittel-Dot / Scope-Ausblendung).
    bool dot_crosshair() const { return m_cfg.dot_crosshair; }
    void draw_public_laser_color();
    void draw_public_reticle_color();
    void draw_public_reticle_size();

private:
    // ------------------------------------------------------------- Referenzen
    // [REF] keep() ref't nur bei referenceCount > 0 (so macht es sol auch),
    // also darf drop() nicht bedingungslos releasen -- sonst Refcount-Unterlauf
    // und Use-after-free. Deshalb je Handle ein reffed-Flag.
    struct RefHandle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};

        void reset() { obj = nullptr; reffed = false; }
    };

    static bool keep(::REManagedObject* o);
    // [K3] Wie Luas ausdrueckliches obj:add_ref() -- nimmt die Referenz AUCH
    // bei referenceCount == 0. Nur fuer die beiden Ray-Results.
    static bool keep_forced(::REManagedObject* o);
    static void drop(::REManagedObject* o, bool reffed);
    static void store(RefHandle& h, ::REManagedObject* o);
    static void store_forced(RefHandle& h, ::REManagedObject* o);

    // ---------------------------------------------------------------- Configs
    struct Cfg {
        bool bullet_hook{true};
        bool crosshair_off{false};
        bool dot_crosshair{false};   // [DOT_CROSSHAIR 26.09.2026] Gui_ui2041 statt Gui_ui2040
        float dot_size{1.0f};        // [DOT_CROSSHAIR] eigene Groesse, NICHT je Waffe
        std::unordered_map<std::string, float> reticle_scale{};
        bool reticle_color{false};
        float reticle_r{1.0f}, reticle_g{0.0f}, reticle_b{0.0f};
        bool force_reticle_concentrate{false};
        float concentrate_ratio{1.0f};
    };

    struct HudCfg {
        bool enabled{true};
        bool hide_hud{false};
        float dx{-0.143f}, dy{-0.094f}, dz{0.10f}, scale{0.289f};
        float rx{-180.0f}, ry{55.0f}, rz{102.0f};
        bool ada_rot{false};
        float ada_rx{-180.0f}, ada_ry{55.0f}, ada_rz{102.0f};
        float ada_dx{-0.143f}, ada_dy{-0.094f}, ada_dz{0.10f};
        std::unordered_map<std::string, bool> guis{};   // GUI-Name -> enabled
    };

    struct LaserCfg {
        bool enabled{true};
        float width{1.5f}, length{2.4f};
        bool force_color{false};
        float r{0.0f}, g{1.0f}, b{0.0f};
        bool tune_glow{true};
        float glow{8.0f}, alpha{0.03f};
        bool smoke{true};
        float smoke_amt{0.28f}, smoke_speed{30.0f};
        bool dot_raycast{true};
        float dot_dist{5.0f}, dot_size{1.0f};
    };

    void load_cfg();
    void save_cfg();
    void load_hud_cfg();
    void save_hud_cfg();
    void load_laser_cfg();
    void save_laser_cfg();

    // ---------------------------------------------------------------- Helfer
    bool ks_active() const;
    ::REManagedObject* get_scene();

    // ------------------------------------------------------------- Raycast
    ::REManagedObject* cast_ray_async(RefHandle& query_slot, ::REManagedObject* ray_result,
                                      const glm::vec3& start, const glm::vec3& end, int32_t layer,
                                      std::optional<int32_t> filter_value);
    void update_crosshair_world_pos(const glm::vec3& start, const glm::vec3& end);

    // ------------------------------------------------------------- Muendung
    ::REManagedObject* find_weapon_on_player_body(const std::string& weapon_name);
    void update_muzzle_data();

    // ------------------------------------------------------- Reticle-Params
    void apply_reticle_params();
    // param_ptr zeigt entweder auf ein Managed Object oder direkt auf eine
    // eingebettete Struct (ValueType-Feld) -- s. field_target().
    void apply_point_range(void* param_ptr, sdk::RETypeDefinition* param_td,
                           bool container_is_value);
    void process_weapon_data(::REManagedObject* weapon_data);

    // --------------------------------------------------------------- GUI
    void apply_hand_hud(::REManagedObject* game_object);
    static void write_vec4(::REManagedObject* obj, const glm::vec4& v, uint32_t offset);
    static glm::quat hud_quat_from_euler_deg(float dxg, float dyg, float dzg);

    // --------------------------------------------------------------- Laser
    void laser_apply(::REManagedObject* self);
    static uint32_t laser_rgba(float r, float g, float b, float a);

    // ------------------------------------------------------------- Phasen
    void on_pre_lock_scene();
    void on_late_update_behavior();

    // ------------------------------------------------------------- UI-Anbindung
    // Die vier Public-Bloecke laufen ueber den Lua-Dispatcher aus
    // ##re4_vr_menu.lua -- der bestimmt die Reihenfolge im Hauptmenue und
    // bleibt bewusst Lua. Anmeldung MUSS traege erfolgen, s. .cpp.
    void ensure_public_ui_registered();
    bool m_public_ui_registered{false};
    bool m_dispatcher_present{false};

    // Zeichnet die drei Dev-Trees.
    void draw_dev_trees();

    // ---------------------------------------------------------- TDB-Handles
    sdk::RETypeDefinition* m_scene_td{nullptr};
    sdk::REMethodDefinition* m_cast_ray_async{nullptr};
    sdk::REMethodDefinition* m_joint_get_position{nullptr};
    sdk::REMethodDefinition* m_joint_get_rotation{nullptr};   // [TOT] nie benutzt, s. Spec 16.2
    sdk::REMethodDefinition* m_set_item_vec3{nullptr};
    sdk::REMethodDefinition* m_set_item_quat{nullptr};

    ::REManagedObject* m_t_player_equipment{nullptr};
    ::REManagedObject* m_t_arms{nullptr};
    ::REManagedObject* m_t_gui{nullptr};
    ::REManagedObject* m_t_catalog_register{nullptr};
    ::REManagedObject* m_t_custom_catalog_register{nullptr};

    int32_t m_layer_bullet{10};
    int32_t m_filter_damage_check_other_than_player{0};
    bool m_have_filter{false};

    // --------------------------------------------------------------- Zustand
    Cfg m_cfg{};
    HudCfg m_hud{};

    // [HUD LEER 2026-09-07] Messer oder leere Haende -> die Hand-HUD-Gruppe
    // nicht zeichnen. Ergebnis kurz gecacht, der Draw-Hook feuert oft.
    bool hud_state_empty();
    double m_hud_empty_t{0.0};
    bool   m_hud_empty_val{false};

    // [HUD INVENTAR 2026-09-25] Koffer offen -> Hand-HUD-Gruppe loslassen.
    bool hud_inventory_open();
    double m_hud_inv_t{0.0};
    bool   m_hud_inv_val{false};
    LaserCfg m_laser{};

    RefHandle m_scene{};
    RefHandle m_attack_ray{};
    RefHandle m_bullet_ray{};
    // [PORTFIX 2026-09-06] Die CastRayQuery wird GEHALTEN und gepinnt. Vorher
    // entstand sie pro Aufruf frisch und ungepinnt -- castRayAsync arbeitet sie
    // aber erst spaeter ab, und ein nicht verankertes Objekt ist bis dahin
    // eingesammelt (Access Violation, im Framework-Log belegt: dieselbe Stelle
    // im Messer-Flug). In Lua haelt die lokale Variable eine echte Referenz,
    // weil sdk.create_instance beim Weg nach Lua add_ref mitmacht.
    // [ZWEI QUERYS 07.09.2026] Eine Query fuer BEIDE Strahlen war falsch: der
    // Bullet-Cast schreibt Ray und Layer um, waehrend der Attack-Cast noch
    // asynchron laeuft. In Lua erzeugt jeder Aufruf seine eigene Query
    // (crosshair.lua Z.172). Jeder Strahl bekommt deshalb seine eigene --
    // gehalten und gepinnt wie bisher.
    RefHandle m_ray_query_attack{};
    RefHandle m_ray_query_bullet{};
    // [FILTER 07.09.2026] CollisionUtil.Filter.DamageCheckOtherThanPlayer --
    // einmal beschafft und gehalten (s. cast_ray_async im .cpp).
    RefHandle m_dmg_filter{};
    bool m_dmg_filter_done{false};
    RefHandle m_cached_pl_head{};
    RefHandle m_cached_gun_obj{};
    RefHandle m_current_muzzle_joint{};
    RefHandle m_last_muzzle_joint{};
    RefHandle m_laser_this{};
    // [M7] Getrennt vom laser_this: __re4_laser_ctrl behaelt im Original den
    // letzten GUELTIGEN Controller, auch wenn laser_this genullt wird.
    RefHandle m_laser_ctrl{};

    std::optional<int32_t> m_current_weapon_id{};
    std::optional<int32_t> m_cached_weapon_id{};
    double m_cache_refresh_time{0.0};
    bool m_current_laser_active{false};

    // [DOT_ARM] s. Spec 3.
    bool m_dot_armed{false};
    std::optional<int32_t> m_dot_stage{};
    bool m_dot_body_weg{false};

    bool m_reticle_params_applied{false};

    // Die neun Felder der geteilten re4-Tabelle. Kein Fremdleser im Live-Set
    // (maschinell geprueft) -> reine Member. Sie ueberleben in Lua einen
    // Script-Reset, weil utility/RE4 modul-gecacht ist -> hier ebenso.
    glm::vec3 m_crosshair_pos{0.0f, 0.0f, 0.0f};
    glm::vec3 m_crosshair_dir{0.0f, 0.0f, 1.0f};
    glm::vec3 m_crosshair_normal{0.0f, 0.0f, 0.0f};
    std::optional<float> m_crosshair_distance{};   // Lua: startet nil
    glm::vec3 m_last_muzzle_pos{0.0f, 0.0f, 0.0f};
    glm::vec3 m_last_muzzle_forward{0.0f, 0.0f, 1.0f};
    bool m_have_muzzle{false};

    // Namenslisten -- Anzahl in der Spec nachgezaehlt.
    static const std::array<const char*, 7> CHARACTER_IDS;
    static const std::array<const char*, 8> PLAYER_BODY_NAMES;
    static const std::unordered_set<std::string> PLAYER_CH_PREFIX;
    static const std::unordered_set<int32_t> NPC_SHARED_WEAPONS;
    static const std::unordered_set<int32_t> RETICLE_SHAPE_WEAPONS;
    static const std::array<std::pair<const char*, const char*>, 6> HUD_TARGETS;
};

#endif // RE4
