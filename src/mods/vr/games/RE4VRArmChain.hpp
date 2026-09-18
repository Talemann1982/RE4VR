// =====================================================================
// RE4VRArmChain -- 1:1-Portierung von reframework/autorun/re4_vr_arm_chain.lua
// (1508 Zeilen). Zeilenangaben "Lua Z.xxx" beziehen sich auf diese Datei.
// Die vollstaendige Spezifikation liegt in I:\LUATRANS\PORT_ARMCHAIN_SPEC.md.
//
// WAS DAS MODUL TUT: Zwei-Knochen-IK (Oberarm + Unterarm) fuer beide Arme.
// Hand-Ziele kommen als Weltposition aus den Globals von re4_vr_motion.lua.
// Pro Phase: Schulter pinnen -> Schulter bei Ueberreichweite nachziehen ->
// Hand-Ziel klemmen -> IK loesen -> Welt-Rotationen auf UpperArm/Forearm
// schreiben -> Hand-Joint wieder aufs Motion-Target pinnen.
//
// REIHENFOLGE (teuer bezahlt, siehe Spec 14): re4_vr_arm_chain.lua registrierte
// seine vier Phasen-Hooks absichtlich erst im ERSTEN on_frame (Lua Z.1301-1307),
// damit es HINTER allen anderen Lua-Scripten laeuft, insbesondere hinter
// re4_vr_motion.lua. Gemessen: es ist das EINZIGE Live-Script, das seine
// Phasen-Hooks verzoegert anmeldet. Deshalb haengt dieses Modul nicht am Traeger
// RE4VR (der steht VOR dem ScriptRunner), sondern am Mini-Mod RE4VRLate, der
// im Mod-Vektor HINTER dem ScriptRunner steht.
//
// UMSCHALTER: _G.__re4_armchain_native = false -> dieses Modul haelt in allen
// vier Phasen still, die Lua-Fassung uebernimmt (Datei aus autorun\c++\
// zurueck nach autorun\).
// =====================================================================
#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "RE4VR.hpp"

// Builtin-Port von reframework/autorun/re4_vr_arm_chain.lua.
// Eigener Mod, in Mods.cpp HINTER dem ScriptRunner registriert -- dort wird
// auch die Reihenfolge der portierten Module untereinander festgelegt.
class RE4VRArmChain : public Mod {
public:
    static std::shared_ptr<RE4VRArmChain>& get();

    std::string_view get_name() const override { return "RE4VRArmChain"; }

    std::optional<std::string> on_initialize() override;

    // Setzt math.atan2 (Lua Z.41-44) -- LOAD-BEARING, siehe Spec 15.1.
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    // Der Frame-Zaehler, der die verzoegerte Hook-Registrierung nachbildet
    // (Lua Z.1302: vor dem ersten on_frame feuert KEINE Phase).
    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // [FREMD-DREHUNG LINKE HAND 2026-09-12] Zusatzdrehung, die GANZ AM ENDE der
    // Kette auf die linke Hand gelegt wird -- im HAND_REPIN, also nach dem
    // IK-Solve und nach set_Rotation(__vr_lh_joint_rot).
    //
    // WARUM HIER UND NICHT IM AUFRUFER: arm_chain ist in JEDER Phase der
    // LETZTE Callback (s. Mods.cpp), und das HAND_REPIN setzt die
    // WELT-Rotation der Hand neu. Jede Drehung, die ein anderes Modul vorher
    // auf L_Hand schreibt, ist danach weg -- genau daran scheiterte der
    // Choke-Regler "Hand Yaw/Pitch/Roll" (12.09.2026).
    //
    // std::nullopt = aus. Der Aufrufer setzt den Wert JEDEN Frame neu, damit
    // ein vergessenes Zuruecksetzen die Hand nicht dauerhaft verdreht.
    void set_left_hand_extra_rot(const std::optional<glm::quat>& q) {
        m_hand_extra_rot[SIDE_L] = q;
    }

private:
    // ---- Die vier Phasen (Lua Z.1242-1299) ----------------------------
    void on_pre_lock_scene();
    void on_late_update_behavior();
    void on_update_joint_expression();
    void on_pre_begin_rendering();
    void on_script_reset();

    // =================================================================
    // Die acht Joints der Armkette. Reihenfolge wie in chain_offset
    // (Lua Z.401-410) -- der Index ersetzt den Lua-Tabellenschluessel.
    // =================================================================
    enum Bone : int {
        R_Hand = 0,
        R_Forearm,
        R_UpperArm,
        R_Shoulder,
        L_Hand,
        L_Forearm,
        L_UpperArm,
        L_Shoulder,
        BONE_COUNT
    };

    // Seite: 0 = R, 1 = L. In Lua der String-Praefix "R"/"L".
    enum Side : int { SIDE_R = 0, SIDE_L = 1, SIDE_COUNT };

    static constexpr int bone_of(Side s, int part) {
        return (s == SIDE_R ? 0 : 4) + part;
    }

    // Lua Z.401-410: pos_* dient NUR als Laengen-Hinweis fuer die IK,
    // rot_* ist ein additiver lokaler Euler-Offset NACH der IK,
    // scale_* die lokale Skalierung.
    struct ChainOffset {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_x{0.0f}, rot_y{0.0f}, rot_z{0.0f};
        float scale_x{1.0f}, scale_y{1.0f}, scale_z{1.0f};
    };

    // Lua Z.288: die gespeicherte Stand-Pose der Schulter, LOKAL.
    struct PinPose {
        glm::vec3 p{0.0f, 0.0f, 0.0f};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    };

    // Lua Z.62-76: get_player_root_pose.
    struct RootPose {
        glm::vec3 pos{0.0f, 0.0f, 0.0f};
        glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat inv_rot{1.0f, 0.0f, 0.0f, 0.0f};
        bool parented{false};
        bool valid{false};      // pos UND rot konnten gelesen werden
        bool inv_valid{false};  // rot:inverse() lieferte etwas
    };

    // Lua Z.981-998: debug_sync.
    struct DebugSync {
        bool require_motion_tick{false};
        bool apply_once_per_frame{false};
        bool hook_lockscene{true};
        bool hook_lateupdate{true};
        bool hook_update_jointexpr{true};
        bool hook_beginrender{true};
        bool show_status{true};
    };

    // ---- Konfiguration (Lua Z.430-600) --------------------------------
    void reset_to_defaults();
    void load_all_configs();
    std::string resolve_config_key() const;
    void apply_key_config(const std::string& resolved_key);
    void save_config();

    // ---- Spielzustand -------------------------------------------------
    ::REManagedObject* get_player_transform();
    RootPose get_player_root_pose(::REManagedObject* player_tf);

    // ---- Joints -------------------------------------------------------
    bool is_joint_valid(::REJoint* joint);
    ::REJoint* get_chain_joint(int bone);
    void store_joint(int bone, ::REJoint* joint);  // [REF] mit add_ref/release
    void clear_joint_cache();                      // der ECHTE Flush (Lua Z.1088/1096)
    void flush_all_arm_caches();   // Lua Z.423: trifft den Cache NICHT (Spec 15.2)

    void apply_ik_rotation_to_joint(int bone, const glm::quat& world_rot, ::REJoint* joint);
    void apply_shoulder_pin(Side side);
    void apply_arm_ik_side(Side side, glm::vec3 hand_pos, const glm::quat* char_rot,
                           const RootPose& root_pose);

    // ---- Ablauf -------------------------------------------------------
    bool should_pause() const;
    bool should_apply_body_chain_now();
    void apply_body_chain();
    void check_player_changed();
    void check_autoreset(Side side, const glm::vec3& hand_target);
    void publish_clamp_anchors();
    void railcar_pin_spine() const;
    static bool stillzone_aus();
    void phase_entry(bool hook_enabled);

    void maybe_log_bone_axis_once(Side side, ::REJoint* upper_joint,
                                  ::REJoint* lower_joint, bool have_upper_dir);

    void arm_ik_segment_lengths(int bone_upper, int bone_lower,
                                float& upper_len, float& lower_len) const;

    // =================================================================
    // Zustand -- Namen und Defaults exakt wie die Lua-Locals.
    // =================================================================

    // Lua Z.397
    bool m_enable_arm_chain{true};
    // Lua Z.264/269/276/287
    bool m_shoulder_reach_follow{true};
    float m_shoulder_reach_follow_max{0.45f};
    bool m_hand_clamp{true};
    bool m_shoulder_pin{true};

    // Lua Z.288: nil = noch keine Pose.
    std::array<std::optional<PinPose>, SIDE_COUNT> m_shoulder_pin_rel{};

    // [FREMD-DREHUNG 2026-09-12] s. set_left_hand_extra_rot. Gehoert KEINER
    // Config und wird nicht gespeichert -- der Setzer haelt sie.
    std::array<std::optional<glm::quat>, SIDE_COUNT> m_hand_extra_rot{};

    // Lua Z.296/301
    std::array<float, SIDE_COUNT> m_wrist_y{0.0f, 0.0f};
    std::array<float, SIDE_COUNT> m_wrist_x{0.0f, 0.0f};

    // Lua Z.401-421
    std::array<ChainOffset, BONE_COUNT> m_chain_offset{};
    std::array<bool, BONE_COUNT> m_arm_segment_enabled{};

    // Lua Z.428/689/703
    std::array<::REJoint*, BONE_COUNT> m_chain_joints{};
    // [REF] Haben wir auf diesen Eintrag wirklich ein add_ref gesetzt?
    std::array<bool, BONE_COUNT> m_chain_joints_reffed{};
    std::unordered_map<uintptr_t, double> m_jv_bad{};
    std::array<bool, SIDE_COUNT> m_axis_verify_done{false, false};

    // Lua Z.431/432
    std::string m_current_key{"default"};
    nlohmann::json m_all_configs{nlohmann::json::object()};

    // Lua Z.651/652/977
    double m_autoreset_last_t{0.0};
    ::REManagedObject* m_cached_player_tf{nullptr};
    bool m_chain_first_load{true};

    // Lua Z.981-1001
    DebugSync m_debug_sync{};
    double m_debug_last_frame_applied{-1.0};
    double m_debug_last_motion_tick_seen{-1.0};

    // Lua Z.1301: vor dem ersten on_frame ist keine Phase registriert.
    bool m_phase_hooks_registered{false};
};

#endif // RE4
