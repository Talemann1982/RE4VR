// =====================================================================
// RE4VRHolster -- 1:1-Portierung von reframework/autorun/re4_vr_holster.lua
// (2522 Zeilen). Zeilenangaben "Lua Z.xxx" beziehen sich auf diese Datei.
// Vollstaendige Spezifikation: I:\LUATRANS\PORT_HOLSTER_SPEC.md (Fassung 2).
//
// WAS DAS MODUL TUT: Vier Holster-Slots (Messer, Pistole, Granate, Langwaffe)
// zeigen die jeweils zuletzt genutzte Waffe als sichtbaren Mesh-KLON an einem
// Body-Joint. Greifen an der Zone + Grip = ziehen/wegstecken. Dazu ein
// fuenfter, mesh-loser Slot fuer das Magazin an der linken Huefte, ein
// Auto-Redraw, der nach Stagger/Killswitch genau den Zustand von vorher
// wiederherstellt, und zwei Gates, die ungewollte Messer-Zuege der Engine
// verwerfen.
//
// WARUM ES TEUER IST: apply_all haengt an SECHS Engine-Phasen und laeuft dort
// ueber alle vier Slots -- rund 150 Engine-Calls pro Frame. Die Datei beziffert
// ihren eigenen Preis mit rund 10 fps (Lua Z.753).
//
// VORSICHT, teuer bezahlt (steht so im Original):
//  * set_Parent auf einen STALE Body-Transform loest eine native Access
//    Violation aus, die pcall NICHT faengt -> die Gueltigkeitspruefungen in
//    spawn_slot sind nicht optional (Lua Z.888-902).
//  * equip_slot_guard hat hart gecrasht und bleibt stillgelegt (Lua Z.1934).
// =====================================================================
#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RE4VR.hpp"

class RE4VRHolster : public Mod {
public:
    static std::shared_ptr<RE4VRHolster>& get();

    std::string_view get_name() const override { return "RE4VRHolster"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;
    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // ---- die nach Lua exportierten Einstiegspunkte (Spec 9.1) ---------
    void defer(std::function<void()> fn);   // __re4_knife_defer
    void set_suppress(bool v);              // __re4_knife_set_suppress
    void holster_exec();                    // __re4_knife_holster_exec
    void holster_bare();                    // __re4_knife_holster_bare

    // [JIGGLE 20.09.2026] Ist die Hand WIRKLICH leer? Am Mesh gemessen, nicht
    // an get_EquipWeaponID (das luegt bei leeren Haenden). Nativ, damit andere
    // Module nicht ueber das Lua-Global __vr_bare_hands gehen muessen.
    bool hands_are_bare() { return !weapon_actually_in_hand().in_hand; }
    bool force_change_to_main();            // __re4_force_change_to_main
    void play_knife_grab_sound();           // __re4_knife_play_grab_sound

private:
    // [MERCS-RAGE 2026-09-09] naechster erlaubter Griff nach dem Messer,
    // solange der Mercs-Ragemodus laeuft (s. on_frame).
    double m_rush_bare_next{0.0};

    // =================================================================
    // Ein Holster-Slot. Die Lua-Fassung baut ihn ueber die Fabrik
    // make_slot (Z.831-1356) als Closure-Satz; hier ist es eine Struktur
    // mit denselben Feldern, damit die 1:1-Zuordnung sichtbar bleibt.
    // =================================================================
    struct Slot {
        std::string name;
        std::unordered_set<int32_t> ids;
        std::string path;          // aktive Config-Datei (Messer: char-abhaengig)
        std::string anchor_g;      // Ziel-Global fuer den Greif-Anker
        std::string zone_g;        // Ziel-Global fuer "Hand in der Zone"
        bool detached_zone{false}; // Zone am HMD statt am Mesh (Schulter)
        bool all_parts{false};     // ganzes Mesh statt nur Part 0

        nlohmann::json cfg{nlohmann::json::object()};

        // ---- Klon-Zustand (Lua S.clone) ----
        ::REManagedObject* clone_obj{nullptr};
        ::REManagedObject* clone_mesh{nullptr};
        ::REManagedObject* clone_tf{nullptr};
        bool clone_reffed{false};              // [REF] haelt add_ref auf clone_obj
        bool clone_mesh_reffed{false};
        bool clone_tf_reffed{false};
        std::optional<int32_t> clone_wid{};
        uintptr_t src_addr{0};
        bool parented{false};
        bool in_use{false};
        bool part0_done{false};
        std::optional<bool> dim_applied{};
        std::optional<float> scale_written{};

        struct Mat4 { int mi{}, vi{}; float x{}, y{}, z{}, w{}; };
        struct Mat1 { int mi{}, vi{}; float orig{}; };
        bool mat_built{false};
        std::vector<Mat4> mat_dim{};
        std::vector<Mat1> mat_zero{};

        // ---- Glaettung (Lua S.sm) ----
        bool sm_has{false};
        glm::vec3 sm_p{0.0f, 0.0f, 0.0f};
        glm::quat sm_r{1.0f, 0.0f, 0.0f, 0.0f};

        // ---- Greifzone (Lua S.grab) ----
        bool grab_in_zone{false};
        float grab_last_dist{99.0f};

        // ---- sonstiger Slot-Zustand ----
        ::REJoint* joint{nullptr};
        bool joint_reffed{false};
        uint64_t joint_ok_frame{UINT64_MAX};
        double last_check{0.0};
        double inv_check_t{0.0};
        bool has_inv{false};
    };

    // Lua Z.450/554/590: zuletzt genutzte Waffe je Gattung.
    struct LastWep {
        int32_t wid{0};
        std::string guid;
    };

    // Lua Z.87 -- vier Felder; pure_since und left_pure_t entstehen erst zur
    // Laufzeit und werden mit `or 0` bzw. `or -999` gelesen (Spec 6).
    struct AutoRedraw {
        std::optional<int32_t> snap{};  // Zahl = Waffen-ID
        bool snap_bare{false};          // Lua: snap == false ("leer ist gewollt")
        bool suppress{false};
        double stow_until{0.0};
        double next_try{0.0};
        double pure_since{0.0};
        double left_pure_t{-999.0};
    };

    // Lua Z.1591: Mag-Holster (linke Huefte, kein Mesh).
    struct MagState {
        bool in_zone{false};
        bool holding{false};
        ::REJoint* joint{nullptr};
        bool joint_reffed{false};
        float last_dist{99.0f};
    };

    // ---- Konfiguration ------------------------------------------------
    static void default_cfg(nlohmann::json& c);
    void load_slot_cfg(nlohmann::json& cfg, const std::string& path);
    void save_slot_cfg(const nlohmann::json& cfg, const std::string& path);
    void load_tap_cfg();
    void save_tap_cfg();

    // ---- Spieler / Inventar (Lua Z.90-458) ----------------------------
    ::REManagedObject* get_ctx();
    ::REManagedObject* body_tf();
    std::optional<int32_t> get_equip_wid();
    ::REManagedObject* get_pe();
    ::REManagedObject* get_inventory();

    struct InHand {
        bool valid{false};      // Lua: Rueckgabe war nicht nil
        bool in_hand{false};
        bool knife{false};
        bool grenade{false};
        bool ambiguous_valid{false}; // vierter Wert vorhanden?
        bool ambiguous{false};
    };
    InHand weapon_actually_in_hand();

    ::REManagedObject* find_weapon_go(const std::unordered_set<int32_t>& ids,
                                      std::optional<int32_t> want_wid,
                                      std::optional<int32_t>* out_wid = nullptr);
    std::vector<::REManagedObject*> inventory_weapon_rows(::REManagedObject* inv);
    bool has_weapon_in_inventory(const std::unordered_set<int32_t>& ids);
    std::string row_guid_key(::REManagedObject* row);
    ::REManagedObject* find_row_for_weapon(::REManagedObject* inv, int32_t wid,
                                           const std::string& guid);
    std::optional<int32_t> get_equip_type_main();
    std::optional<int32_t> weaponid_enum(int32_t wid);
    std::string get_equipped_main_guid(::REManagedObject* inv);

    void track_last_weapons();
    void draw_last_pistol(::REManagedObject* pe);
    void draw_last_grenade(::REManagedObject* pe);
    void draw_last_rifle(::REManagedObject* pe);

    // ---- Slot-Mechanik (Lua make_slot) --------------------------------
    ::REJoint* slot_joint(Slot& s);
    void store_slot_joint(Slot& s, ::REJoint* j);
    void store_clone(Slot& s, ::REManagedObject* go, ::REManagedObject* mesh,
                     ::REManagedObject* tf);
    void destroy_slot(Slot& s);
    bool spawn_slot(Slot& s, ::REManagedObject* gmesh);
    void build_mat_dim(Slot& s);
    bool apply_dim(Slot& s, bool dark);
    bool isolate_part0(Slot& s);
    float crouch_opt_z() const;
    bool compute_target(Slot& s, glm::vec3& out_pos, std::optional<glm::quat>& out_rot);
    bool compute_zone_anchor(Slot& s, glm::vec3& out);
    void update_smooth(Slot& s);
    void write_scale(Slot& s, ::REManagedObject* tf, float value);
    void apply_slot(Slot& s);
    void manage_slot(Slot& s);
    void update_knife_lh_zone(Slot& s);
    void update_grab(Slot& s);
    bool calibrate_slot(Slot& s, const glm::vec3& P);
    bool slot_dormant(const Slot& s) const;
    std::optional<int32_t> source_want_wid(const Slot& s) const;
    bool slot_grabbable(Slot& s);
    void tick_slot(Slot& s);
    void apply_all();

    // [BODY-EPOCH 2026-09-22] Gemerkte Body-Zeiger verwerfen (s. re4vr::body_epoch).
    void drop_body_caches();

    // ---- Mag-Holster ---------------------------------------------------
    ::REJoint* mag_joint();
    bool mag_anchor(glm::vec3& out);
    void mag_set_holding(bool v);
    void mag_haptic(float amp);
    void mag_tick();
    bool calibrate_mag(const glm::vec3& P);

    // ---- Grab-Dispatch --------------------------------------------------
    Slot* nearest_with_clone();
    void do_grab(Slot& best);
    void grab_dispatch();
    void play_go_sound(::REManagedObject* go, uint32_t id);

    // ---- Kalibrierung ---------------------------------------------------
    void haptic_pulse(float dur, float freq, float amp);
    void start_calibration(Slot* slot, bool mag);
    void calibration_tick();

    // ---- Zustandsmaschinen ----------------------------------------------
    bool knife_only_stage();
    bool no_weapons_yet();
    void knife_char_tick();
    std::string knife_char_now();
    const std::string& knife_active_path() const;
    void auto_redraw_tick(const InHand& ih);
    void install_gate_hooks();
    void publish_state_globals();   // nach jedem Script-Reset noetig

    // ---- VR-Helfer -------------------------------------------------------
    bool right_grip_pressed();
    bool left_grip_pressed();
    std::optional<glm::vec3> rh_world();
    std::optional<glm::vec3> lh_world();
    bool hmd_pose_yaw(glm::vec3& pos, glm::vec3& right, glm::vec3& up, glm::vec3& fwd);

    // ---- UI ---------------------------------------------------------------
    struct SlotUiOpts {
        float scale_min{0.2f};
        bool shift_x{false};
        bool per_weapon_x{false};
        const char* note{nullptr};
        const char* cal_hint{nullptr};
    };
    void draw_slot_ui(const char* title, const char* id, Slot& s, const SlotUiOpts& opts);
    void draw_mag_ui();

    // =================================================================
    // Zustand
    // =================================================================
    std::array<Slot, 4> m_slots{};   // knife, pistol, grenade, shoulder
    Slot& knife()    { return m_slots[0]; }
    Slot& pistol()   { return m_slots[1]; }
    Slot& grenade()  { return m_slots[2]; }
    Slot& shoulder() { return m_slots[3]; }

    nlohmann::json m_mag_cfg{nlohmann::json::object()};
    MagState m_mag{};
    bool m_mag_empty_latched{false};

    LastWep m_last_pistol{};
    LastWep m_last_grenade{5400, ""};   // Lua Z.554: Default Hand Grenade
    LastWep m_last_rifle{};
    int32_t m_last_knife_wid{0};

    AutoRedraw m_ar{};
    bool m_merc_body_weg{false};

    // [SAVE-LOAD WIE MERCS 19.09.2026] Body-Adresse beim letzten Blick. Springt
    // sie (Tod/Laden in der Kampagne), wird der Waffen-Snapshot geloescht --
    // dasselbe wie MERCS-LEVELSTART.
    std::optional<uintptr_t> m_sl_body_addr{};

    // [BODY-EPOCH 2026-09-22] Zuletzt gesehener re4vr::body_epoch().
    uint64_t m_body_epoch{0};

    // Grab-Dispatch (Lua Z.1674-1704)
    bool m_grip_prev{false};
    double m_grip_t0{0.0};
    bool m_press_armed{false};
    bool m_tap_mode{false};
    double m_grab_haptic_at{0.0};
    float m_aim_hold{0.35f};

    // Kalibrierung (Lua Z.1857-1883)
    Slot* m_cal_slot{nullptr};
    bool m_cal_mag{false};
    double m_cal_deadline{0.0};
    int m_cal_last_beep{-1};

    // Drosseln (Lua Z.44, 330)
    double m_knife_only_t{0.0};
    bool m_knife_only_val{false};
    double m_no_weapon_t{0.0};
    bool m_no_weapon_val{false};

    // Messer-Charakter (Lua Z.1398)
    std::string m_knife_char;   // "" = noch nie gesetzt

    // Deferred Aktion (Lua Z.630)
    std::function<void()> m_pending_action{};

    // Frame-Grenze fuer die Joint-Pruefung (Lua Z.778)
    uint64_t m_hol_frame{0};

    // Cache-Handles
    ::REManagedObject* m_character_manager{nullptr};
    ::REManagedObject* m_pe{nullptr};
    bool m_gates_installed{false};

    // Zur Ladezeit aufgeloest
    sdk::RETypeDefinition* m_mesh_td{nullptr};
    sdk::RETypeDefinition* m_pe_td{nullptr};
    sdk::RETypeDefinition* m_wid_td{nullptr};
    sdk::RETypeDefinition* m_snd_container_td{nullptr};
    std::optional<int32_t> m_equip_type_main{};
    std::optional<int32_t> m_state_damage_mask{};
    bool m_state_damage_ok{false};
    std::optional<bool> m_equip_wid_is_value{};
    double m_crouch_gain{0.0};
    double m_ada_mesh_z{0.0};
    // [MERCS MESH-Z PRO CHARAKTER 17.09.2026] Der frueher gemeinsame Mercs-Wert
    // ist RAUS -- jeder Mercs-Charakter hat seinen eigenen, weil die Panzerung
    // je Modell unterschiedlich weit vorsteht (Krauser deckt das Mesh zu,
    // Leon nicht). Schluessel ist der Body-GO-Name, damit hier nichts geraten
    // werden muss; ein unbekannter Body landet im Auffang-Eintrag "*".
    // Gilt AUSSCHLIESSLICH in Mercenaries -- Kampagne und Separate Ways
    // (inkl. Adas eigenem m_ada_mesh_z) sehen diese Werte nie.
    struct MercMeshZ {
        const char* body;    // Body-GO-Name = JSON-Schluessel ("*" = Auffang)
        const char* label;   // Regler-Beschriftung
        double      z;
    };

    static constexpr int MERC_MESH_Z_COUNT = 7;

    MercMeshZ m_merc_mesh_z[MERC_MESH_Z_COUNT]{
        {"ch6i0z0_body",    "Leon",    0.0},
        {"ch6i1z0_body",    "Luis",    0.0},
        {"ch6i2z0_body",    "Krauser", 0.0},
        {"ch6i3z0_body",    "HUNK",    0.0},
        {"ch6i5z0_body",    "Wesker",  0.0},
        {"ch3a8z0_MC_body", "Ada",     0.0},
        {"*",               "andere",  0.0},
    };

    double merc_mesh_z_now() const;

    std::string m_merc_body_now;   // einmal pro Frame, nur in Mercenaries
    int m_merc_mesh_z_idx{-1};     // Index dazu, -1 = kein Eintrag aktiv
    bool m_in_mercs_now{false};    // einmal pro Frame aus __re4_in_mercs
    bool m_types_resolved{false};

    // Hysterese der LINKEN Messer-Zone (Lua Z.718, Datei-Local)
    bool m_knife_lh_in_zone{false};
};

#endif // RE4
