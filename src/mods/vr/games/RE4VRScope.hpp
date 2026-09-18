// ============================================================================
// RE4VRScope -- 1:1-Portierung von re4_vr_scope.lua (872 Zeilen)
// Spezifikation: I:\LUATRANS\PORT_SCOPE_SPEC.md  (samt NACHTRAG vom 03.09.2026)
//
// EINREIHUNG (Nachtrag L1): Dieser Mod MUSS in Mods.cpp VOR RE4VRMaterials und
// VOR RE4VRCrosshair stehen -- beide lesen `vr_scope_active` und wuerden den
// Flankenwert sonst eine Phase zu spaet sehen.
//
// DAS GLOBAL (Nachtrag L2): `vr_scope_active` wird ueber re4vr::lua_set_bool in
// Luas _G geschrieben, NICHT in einem C++-Flag gehalten -- RE4VRCrosshair.cpp
// (2098, 2326) und RE4VRMaterials.cpp (1013) lesen es dort ab.
// ============================================================================
#pragma once

#if defined(RE4)

#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../Mod.hpp"

class RE4VRScope : public Mod {
public:
    static std::shared_ptr<RE4VRScope>& get();

    std::string_view get_name() const override {
        return "RE4VRScope";
    }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;

private:
    // ---- Konfiguration (Lua Z.50-114) ------------------------------------
    struct Vec3Cfg {
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    struct RotCfg {
        double yaw{0.0};
        double pitch{0.0};
        double roll{0.0};
    };

    struct WeaponCfg {
        Vec3Cfg pos_offset{};
        RotCfg  rot_offset{};
        Vec3Cfg scope_pos_offset{};
        RotCfg  scope_rot_offset{};
        Vec3Cfg scope_cam_pos{};
        RotCfg  scope_cam_rot{};
        double  scope_fov_min{0.0};   // 0 = don't override
        double  scope_fov_max{0.0};   // 0 = don't override
        double  scope_lerp_speed{8.0};
    };

    // [REF] Alles, was ueber Frames hinweg gehalten wird, braucht eine eigene
    // Referenz -- in Lua macht das sol beim Ablegen in einer Variablen
    // (add_ref, aber nur bei referenceCount > 0). Ohne das zeigt der Cache nach
    // einem Save-Load auf freigegebenen Speicher.
    struct RefHandle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    static bool keep(::REManagedObject* o, RefHandle& out);
    static void drop(RefHandle& h);

    // ---- Konfigurationspfad / Laden --------------------------------------
    std::string get_controller_preference() const;
    std::string get_config_path() const;
    void load_config();
    void save_config();                 // [TOT] kein Aufrufer -- s. Lua Z.200
    WeaponCfg& get_weapon_cfg(int32_t wid);
    void store_active_to_config();
    void load_active_from_config(int32_t wid);
    void switch_weapon_config(int32_t wid);

    // ---- Engine-Zugriffe --------------------------------------------------
    ::REManagedObject* get_scene();
    ::REManagedObject* find_go_by_name(const char* name);
    ::REManagedObject* find_weapon_go(int32_t wid);
    bool check_scope_aim();
    bool is_scope_zoomed();
    ::REManagedObject* get_scope_camera_transform(int32_t wid);
    ::REManagedObject* get_scope_camera_joint(int32_t wid);          // [TOT]
    ::REManagedObject* get_muzzle_joint_for_weapon(int32_t wid);     // [TOT]
    ::REManagedObject* get_body_transform();

    // ---- Wirkteile --------------------------------------------------------
    void apply_ironsight_offset();
    void find_body_children();
    void force_body_visible();
    void apply_scope_fov_override(int32_t wid);
    void apply_scope_cam_offset(int32_t wid);
    void ensure_weapon_parented(int32_t wid);
    void force_weapon_mesh_part0(int32_t wid);

    // ---- Lens-Entspiegelung (Lua Z.680-784) -------------------------------
    void lens_resolve(int32_t wid);
    void lens_write(const char* name, double value);
    void apply_lens_fix(bool aiming);

    void reset_state();

    // ---- Zustand ----------------------------------------------------------
    bool m_is_scoped{false};
    bool m_is_zoomed{false};

    std::map<std::string, WeaponCfg> m_weapon_configs{};
    std::optional<int32_t> m_active_wid{};

    Vec3Cfg m_pos_offset{};
    RotCfg  m_rot_offset{};
    Vec3Cfg m_scope_pos_offset{};
    RotCfg  m_scope_rot_offset{};
    Vec3Cfg m_scope_cam_pos{};
    RotCfg  m_scope_cam_rot{};
    double  m_scope_fov_min{0.0};
    double  m_scope_fov_max{0.0};
    double  m_scope_lerp_speed{8.0};
    double  m_scope_lerp_t{0.0};
    double  m_slider_range_pos{1.0};
    double  m_slider_range_rot{180.0};

    // [L7] body_children haelt GameObjects ueber die GANZE Scope-Phase --
    // find_body_children laeuft nur auf der steigenden Flanke.
    std::vector<RefHandle> m_body_children{};
    RefHandle m_cached_body_tf{};

    // Lua Z.696: lens = { wid, mesh, midx, vidx, orig, was_aiming, restore,
    //                     addr, check_t }
    struct Lens {
        std::optional<int32_t> wid{};
        RefHandle mesh{};
        int32_t midx{0};
        std::unordered_map<std::string, int32_t> vidx{};
        std::unordered_map<std::string, double> orig{};
        bool was_aiming{false};
        int32_t restore{0};
        uintptr_t addr{0};
        bool addr_valid{false};
        double check_t{0.0};
    };

    Lens m_lens{};
    void lens_clear();

    // sdk.typeof(...) aus Lua. re4vr::get_component nimmt die TypeDefinition und
    // holt sich den Runtime-Type selbst -- derselbe getComponent(System.Type).
    sdk::RETypeDefinition* m_scene_td{nullptr};
    sdk::RETypeDefinition* m_scope_ctrl_t{nullptr};      // chainsaw.ScopeController
    sdk::RETypeDefinition* m_via_mesh_type{nullptr};     // via.render.Mesh
    sdk::RETypeDefinition* m_wcc_register_type{nullptr}; // chainsaw.WeaponCustomCatalogRegister

    // Einmalige Aufloesung der sdk.typeof-Caches plus load_config() -- in Lua
    // passiert beides beim Laden der Datei (Z.212).
    bool m_types_resolved{false};

    // [M4] Einmal-Aufraeumen an der Riegel-Flanke.
    bool m_gate_cleared{false};
};

#endif // RE4
