// ============================================================================
// RE4VRMotion -- 1:1-Portierung von re4_vr_motion.lua (6295 Zeilen).
//
// RE4 VR MOTION CONTROLS -- Pure Hand-Joint Binding (RE9-Style Pipeline).
// Bindet R_Hand und L_Hand des Spieler-Bodys an die VR-Controller:
//   * sc/sf-Helfer (variadische pcall-Wrapper)
//   * controller_to_world als einheitliche Pose-Pipeline
//   * Runtime- und Controller-abhaengige OpenXR-Korrektur
//   * Standing-Origin-Cache + Phasen-Stack (LockScene/LateUpdateBehavior/
//     BeginRendering/UpdateJointExpression)
//   * Smoothing-State + Cross-Script-Globals (__vr_rh_world etc.)
//   * Init-Gate auf die vrmod-Controller-Bereitschaft
//
// Spezifikation: I:\LUATRANS\PORT_MOTION_SPEC{,_TEIL1,_TEIL2,_TEIL3}.md
// **Es gilt der ZWEITE NACHTRAG (04.09.2026) am Ende jeder Datei** -- er
// korrigiert zwei sachlich falsche Aussagen des ersten Nachtrags (V1: `or`
// greift in Lua NUR bei nil, nicht bei 0; V2: two_hand.pitch/yaw/roll WERDEN
// geladen).
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- **VOR** dem ScriptRunner (gemessen, nicht geraten)
//
// Anders als jeder bisherige Port steht dieses Modul VOR ScriptRunner::get().
// Grund: motion LIEST 52 Globals, die spaeter ladende Lua-Dateien schreiben
// (reload, reload2-5_dlc, reload_adv, weapons, movement). Stuende es dahinter,
// saehe es sie frisch statt wie bisher einen Pass alt -- jede darauf
// kalibrierte Rampe und Flanke (Slide-Dock-Blend, Pump-Progress,
// __vr_motion_paused) wuerde sich verschieben.
//
// Davor gemessen: nur 22 Globals kippen, aus binding (5, schreibt aber
// ausschliesslich in on_frame -> Phasenlage irrelevant), choke (12) und
// merc (5). Beide sind selbst bald portiert, dann loest sich auch das auf.
//
// Sobald KEIN Lua mehr laeuft, ist die Position gleichgueltig und dieses Modul
// gehoert der Ordnung halber zu den anderen hinter den ScriptRunner.
// ----------------------------------------------------------------------------
// PUBLIKATIONSPFLICHT (zweiter Nachtrag, V7)
// 27 der 32 write-only Globals sind Schnittstelle -- neun davon werden bereits
// von C++-Modulen aus Luas _G gelesen (ArmChain, Holster, Crosshair, Recoil,
// Materials, Objects, Movement). Sie MUESSEN weiterhin nach Lua publiziert
// werden; als reine C++-Member waeren sie fuer diese Module unsichtbar.
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

class RE4VRMotion : public Mod {
public:
    static std::shared_ptr<RE4VRMotion>& get();

    std::string_view get_name() const override { return "RE4VRMotion"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // Public-Menue (Dispatcher, Platz 10).
    void draw_public_headset();

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // [CHOKE-FLIP] Den Reverse-Grip SOFORT und ohne Lerp stellen -- dasselbe
    // Muster, das Motion nach einem Killswitch fuer __re4_knife_flip_pre_ks
    // benutzt. RE4VRChoke ruft das beim Zugriff direkt auf, statt ueber das
    // Global __vr_knife_flip zu gehen: das gehoert dem RT-Toggle in
    // re4_vr_binding.lua, und binding laeuft seit dem Port NACH uns und wuerde
    // es im Zugriffsframe wieder loeschen.
    // Die Flip-OFFSETS je Messer (knife_flip_pos) zieht attach_weapon
    // unveraendert aus der Config -- hier wird nur der Zustand gesetzt.
    void force_knife_flip();

private:
    // ======================================================================
    // Engine-Handle ueber Frames. Rohzeiger reichen NICHT: sol_lua_push macht
    // in Lua das add_ref unsichtbar mit.
    //
    // [ZWEITER NACHTRAG V3] Zwei Tabellen des Originals halten Handles, die
    // on_script_reset NICHT nullt -- `fl_state.light_comp`/`scope_hidden` und
    // das ganze `__re4_ada_fl`. In Lua war das folgenlos (ref-counted), im Port
    // ist es eine Absturzquelle nach Levelwechsel. Beide werden hier gehalten
    // UND beim Reset freigegeben.
    // ======================================================================
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    void store(Handle& h, ::REManagedObject* o);
    void drop(Handle& h);

    // [BODY-EPOCH 2026-09-22] Body gewechselt (Save-Load/Tod): ueber Frames
    // gemerkte Zeiger verwerfen, der vorhandene Neu-Hol-Zweig holt sie neu.
    uint64_t m_body_epoch{0};
    void drop_body_caches();

    // ======================================================================
    // Offset-Tabellen. Key = Waffen-ID als STRING, "none" = keine Waffe.
    // get_* liefert nullptr, wenn nicht angelegt; ensure_* legt an und seedet
    // aus dem jeweils allgemeineren Offset. Der Unterschied ist tragend:
    // existiert kein Aim-Offset, benutzt die Waffe IMMER das Idle-Offset --
    // es gibt dann keinen Aim-Blend.
    // ======================================================================
    struct WeaponOffset {   // wirkt auf den R_Hand-JOINT, nicht auf das Waffen-GO
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
    };

    // [WEP_REL_PERSIST] Auto-kalibrierter Hand->Waffe-Offset, EINGEFROREN:
    // jede Waffe wird einmal im Ruhezustand kalibriert und danach nie wieder
    // gesampelt -- sonst wandert sie, weil Skip/Pin in die Messung funken.
    struct WeaponRel {
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float qx{0.0f}, qy{0.0f}, qz{0.0f}, qw{1.0f};
    };

    struct SupportOffset {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
        float dock_threshold{0.110f}, undock_threshold{0.150f};
        // [PUMP_GRIP] Nur Pump-Shotguns: dort ist der Vordergriff der bewegliche
        // Slide. Ist grip_on gesetzt, haengt der Zielpunkt AM JOINT.
        bool  grip_on{true};
        float grip_x{0.0f}, grip_y{0.0f}, grip_z{0.0f};
        // [GRIP_SLIDE] Gleitbereich entlang der Waffenachse: reicht der Arm
        // nicht, rutscht die Hand am Rohr zurueck statt neben die Waffe.
        float grip_back{0.0f}, grip_fwd{0.0f};
        bool  grip_anchor{false};   // [GRIP_ANCHOR_RH] gegen Einfrieren beim Laufen
        bool  grip_noroll{false};   // [GRIP_NO_ROLL] gegen Kreisen beim Rollen
    };

    struct SupportOffsetAim {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
        float blend_in{0.18f}, blend_out{0.18f};
    };

    struct SupportOffsetSwitch {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
        float dock_dist{0.060f};      // eng halten -> kein Hin-und-Her Schalter/Schaft
        float blend_speed{0.150f};    // eigener Lerp; Vordergriff nutzt support.blend_speed
        float idx_rx{0.0f}, idx_ry{0.0f}, idx_rz{0.0f};   // Zeigefinger-Curl der "OK"-Geste
        float burst_rot{0.0f};        // [BURST] Hebelwinkel Burst-Stellung (Grad, Pitch)
        float burst_count{3.0f};      // [BURST] Schuss pro Trigger-Zug
        float single_rot{0.0f};       // [FIRE_MODE] Hebelwinkel Single-Stellung
        float lever_lerp{0.18f};      // Lerp der Hebel-Schwenk-Anim
    };

    struct SupportOffsetSwitch2 {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
        float lerp{0.15f};            // Pose1<->Pose2 Blend-Speed
    };

    struct SupportOffsetSwitch2Aim {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
    };

    // ======================================================================
    // Zustand (Lua Z.113-180)
    // ======================================================================
    struct Cache {
        std::optional<glm::vec3> rh_world{}, lh_world{};
        std::optional<glm::quat> rh_rot{}, lh_rot{};
        std::optional<glm::vec4> standing_origin{};
        bool standing_origin_set{false};
        // [MATILDA_STOCK] Laufzeit: haengt der Schaft an der Matilda?
        bool matilda_stock{false};
        // Rohe Controller-Rotation vor den Offsets (Aim-Richtung fuer andere
        // Scripte). [Zweiter Nachtrag] __vr_rh_aim_rot hat live KEINEN Leser.
        std::optional<glm::quat> rh_aim_rot{};
        // Was write_joint_pose zuletzt geschrieben hat.
        std::optional<glm::vec3> rh_joint_pos{}, lh_joint_pos{};
        std::optional<glm::quat> rh_joint_rot{}, lh_joint_rot{};
        std::string weapon_key{};
    } m_cache{};

    // [CHOKE: HAND STARR 2026-09-11] Rotation der linken Hand, eingefroren beim
    // ersten Frame des Wuergegriffs. Fuer die POSITION gibt es die Klemme in
    // attach_left_hand; die Rotation kam bis dahin ungefiltert vom Controller,
    // liess sich im Griff also frei weiterdrehen.
    std::optional<glm::quat> m_choke_lh_rot{};

    struct HandSlot {
        Handle joint{};
        bool enabled{true};
    };

    HandSlot m_right_hand{};
    HandSlot m_left_hand{};

    // [KNIFE_RH_PIN 2026-09-08] Das Messer in der RECHTEN Hand wird an den
    // R_Hand-Joint GEPARENTET (set_Parent + set_ParentJoint) und bekommt danach
    // nur noch seine LOKALE Pose -- exakt wie der linke Klon in
    // RE4VRWeapons2::clone_spawn/clone_apply_pose. Grund: eine Weltpose weiss
    // nichts davon, dass RE4VRMovement beim Stick-Drehen danach den Hip-Yaw
    // nachzieht; der Joint dreht als Teil des Skeletts mit, ein GO in
    // Weltkoordinaten bleibt stehen (Messer sass neben der Faust).
    // Der Heimatparent wird VOR dem ersten Umhaengen gemerkt und beim Loesen
    // wiederhergestellt -- sonst faellt das GO aus dem Player-Body-Baum und
    // namensbasierte Sound-/Joint-Suchen laufen leer.
    struct KnifePin {
        // [LADEN 2026-09-09] Der Body-Transform, unter dem gepinnt wurde. Dient
        // NUR dem Vergleich: nach einem Ladevorgang ist er ein anderer, und
        // dann muessen die alten Handles weg, BEVOR sie jemand anfasst.
        // Bewusst KEIN Handle -- wir wollen ihn nie benutzen, nur vergleichen.
        uintptr_t body_at_pin{0};
        Handle tf{};             // die aktuell gepinnte Waffen-Transform
        Handle home_parent{};    // wohin sie zurueckgehoert
        bool active{false};
    };

    KnifePin m_knife_pin{};


    void knife_pin_apply(::REManagedObject* wep_tf, const glm::vec3& wpos,
                         const glm::quat& wrot);
    void knife_pin_release();

    // [WURF/STECKEN] Zustand nur VERGESSEN, ohne zurueckzuparenten: Wurf und
    // Choke haengen das GO selbst um (Flug bzw. Hals-Joint des Gegners). Ein
    // set_Parent waere dort ein Kampf um dieselbe Transform. Beim naechsten
    // regulaeren Durchlauf pinnt knife_pin_apply frisch -- genau wie links
    // clone_manage den Klon nach dem Flug wieder an L_Hand haengt.
    void knife_pin_forget();

    float m_rot_smooth_hands{0.0f};
    // Positions-Glaettung gegen Controller-Mikrozittern (0 = roh wie RE9).
    float m_pos_smooth_hands{0.0f};

    // [TWO_HAND_IK] Beide Haende an der Waffe + beide Grips -> die Waffe folgt
    // der Linie rechte Hand -> linke Hand (look-at).
    struct TwoHand {
        bool  enabled{true};
        float min_dist{0.04f};      // darunter kein Blend (Haende zu nah)
        float max_dist{0.90f};      // darueber kein Blend (linke Hand zu weit)
        float blend_speed{0.05f};
        float pitch{0.0f}, yaw{0.0f}, roll{0.0f};
        float blend{0.0f};          // Laufzeit 0..1
        bool  active{false};        // greift gerade -> Support-Dock erzwingen
        float _dbg_dist{-1.0f};
        // [PUMP_NO_Z] Waffenposition + Kamera im ersten Pump-Frame
        std::optional<glm::vec3> _pump_ref_pos{}, _pump_ref_cam{};
        // [SNAP_SOFTEN] zuletzt ausgegebene, geglaettete Waffenrotation
        std::optional<glm::quat> _smooth_rot{};
        // [REL_ENGAGE] Schwenk-Nullpunkt beim Greifen
        std::optional<glm::quat> _engage_swing0{};
        int _rack_release{0};       // [RACK_RELEASE] Frames-Countdown
    } m_two_hand{};

    struct Smoothing {
        std::optional<glm::vec3> right_pos{}, left_pos{};
        std::optional<glm::quat> right_rot{}, left_rot{};
    } m_smoothing{};

    struct InitState {
        bool initialized{false};
        int  frame_counter{0};
    } m_init{};

    struct BodyCache {
        Handle go{};
        Handle transform{};
    } m_body_cache{};

    // Runtime + Controller. vr_runtime wird NICHT persistiert, sondern im
    // Init-Gate von tick() gesetzt (Lua 4843/4845).
    std::string m_vr_runtime{"openvr"};            // "openvr" | "openxr"
    std::string m_selected_controller{"steamvr"};  // "steamvr" | "metavr"

    struct Correction {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
    };

    // Von RE9 uebernommen, immer gleich fuer die OpenXR-Runtime.
    Correction m_openxr_correction{0.015f, -0.006f, -0.104f, -16.341f, -2.011f, -0.754f};
    // Quest/Touch: kommt NUR bei selected_controller == "metavr" pauschal
    // obendrauf; Index/SteamVR ist die Baseline.
    Correction m_ctrl_correction{-0.004f, -0.002f, -0.002f, 0.0f, 0.0f, 5.606f};

    // Layoutgleich mit WeaponOffset -- die UI-Slider arbeiten direkt darauf.
    struct HandOffsetL {   // linke Hand, global (Position im Hand-Frame + lokale Rotation in Grad)
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
    } m_hand_offset_l{};

    std::unordered_map<std::string, WeaponOffset> m_weapon_offset{};
    std::unordered_map<std::string, WeaponRel> m_weapon_rel{};
    std::unordered_map<std::string, SupportOffset> m_support_offset{};
    std::unordered_map<std::string, SupportOffsetAim> m_support_offset_aim{};
    std::unordered_map<std::string, SupportOffsetSwitch> m_support_offset_switch{};
    std::unordered_map<std::string, SupportOffsetAim> m_support_offset_switch_aim{};
    std::unordered_map<std::string, SupportOffsetSwitch2> m_support_offset_switch2{};
    std::unordered_map<std::string, SupportOffsetSwitch2Aim> m_support_offset_switch2_aim{};

    // [SUPPORT_HAND] Linke Hand dockt an den Waffen-Vordergriff.
    struct Support {
        bool  enabled{true};
        bool  force_dock{false};
        bool  docked{false};
        bool  end_active{false};   // [END_POSE] Stuetzhand nach dem Einlegen laeuft
        // [GRIP_LATCH_REACH] ANTEIL der echten Armreichweite
        // (__vr_arm_chain_L_maxreach), bewusst kein Meterwert: Ada und Leon
        // haben unterschiedlich lange Arme.
        // [ARMLAENGE 10.09.2026] War 0.75 -- der Griff brach damit schon bei
        // 75 % der Armreichweite, also mitten im normalen Halten am Vordergriff.
        // 0.95 laesst den Arm fast ganz ausfahren; die letzten 5 % bleiben
        // bewusst uebrig, weil die IK bei exakt gestrecktem Arm mehrdeutig wird
        // (Ellbogen kippt). ACHTUNG: re4_vr_motion.json sticht diesen Wert
        // (support_cfg/grip_latch_reach) -- dort muss er ebenfalls stehen.
        float grip_latch_reach{0.95f};
        float blend_speed{0.050f};
        float blend_factor{0.0f};
        float target_blend{0.0f};
        float aim_blend{0.0f};              // 0 = Idle-Dock, 1 = Aim-Dock
        bool  aim_preview{false};
        bool  switch_docked{false};         // Hand am Verstell-Schalter statt Vordergriff
        bool  switch_preview{false};
        bool  switch_aim_preview{false};
        bool  switch2_preview{false};
        bool  switch2_aim_preview{false};
        float switch2_blend{0.0f};          // 0 = Full-Auto-Pose, 1 = Single/Burst-Pose
        bool  switch_blend_lock{false};     // kein Schaft-Flackern beim Grip-Loslassen
        int   fire_mode{0};                 // 0=Full, 1=Burst, 2=Single
        int   prev_fire_mode{0};
        bool  burst_active{false};
        bool  burst_preview{false};
        bool  single_preview{false};
        float burst_anim{0.0f};             // sanft gelerpter Hebel-Winkel
        bool  prev_switch_trigger{false};
        // Grip-Flanke am Schalter: einmalig latchen, damit die Hand nicht
        // zwischen Schalter und Schaft springt.
        bool  switch_latched{false};
        bool  prev_left_grip{false};
        // [SWITCH ONE-SHOT] Sperrt nach EINEM Umlegen zusaetzlich den
        // Vordergriff-Dock, bis der Grip physisch losgelassen wird.
        bool  sw_consumed{false};
        // [REPIN_LEFT] Notausgang -- nur ueber die UI setzbar.
        bool  repin_off{false};
        // [F6 TEIL2] write-only, nie gelesen -- 1:1 mitgenommen:
        float switch_dbg_d{0.0f};
        std::optional<float> burst_neutral_fy{};
        // [TEIL2/F6] Der NAME des Pump-Joints, nur geschrieben, nie gelesen --
        // es gibt keine UI-Statuszeile dafuer. 1:1 mitgenommen.
        std::string pump_jn{};
        // [GRIP_ANCHOR_RH] Anker gehoert zu EINER Waffe -- bei Wechsel
        // wegwerfen, sonst klebt der Griff der vorigen Waffe an der neuen.
        std::optional<glm::vec3> grip_off{};
        std::optional<glm::vec3> grip_rhprev{};
        std::optional<int32_t> grip_off_wid{};
        // [WEAPON_GIVE] Weich ein-/ausgeblendeter Rueckzug der WAFFE, wenn der
        // Pumpgriff ausserhalb der linken Armreichweite liegt. Nicht die Hand
        // gibt nach, sondern die Waffe -- sonst reisst die Hand vom Griff ab.
        std::optional<glm::vec3> give{};
        // [DOCK_PROXIMITY] Ist die freie Hand wirklich am Vordergriff? Mit
        // Hysterese (dock_threshold rein, undock_threshold raus).
        bool free_near{false};

        // [GRIFF-AUSSETZER 10.09.2026] Gemessen: faellt die Waffenerkennung fuer
        // ~2,5 s aus (wid=nil), rampt der Dock aus und rastet danach NIE wieder
        // ein -- die Hand haengt dann einen halben Meter neben dem Griff. Ein
        // Aussetzer ist aber kein Loslassen. Solange der Grip gehalten wird und
        // dieselbe Waffe zurueckkommt, wird der Dock-Zustand eingefroren.
        double hold_t0{0.0};                  // Beginn des Aussetzers (0 = keiner)
        std::optional<int32_t> dock_wid{};    // Waffe, an der gedockt wurde
    } m_support{};

    // ======================================================================
    // Waffen-Cache und Kalibrier-Zustandsmaschine (Lua Z.1182)
    // [TEIL2/L9 + zweiter Nachtrag] ZWOELF Felder, nicht sechs: settle,
    // frozen, stock_go, mesh_td, stock_mesh und _parry_last_gun entstehen
    // erst zur Laufzeit.
    // ======================================================================
    struct WepCache {
        std::optional<int32_t> id{};
        Handle go{};
        Handle tf{};
        std::optional<glm::vec3> rel_pos{};
        std::optional<glm::quat> rel_rot{};
        // Kalibrier-Fenster: erst `wait` Frames warten, dann `sample` Frames
        // messen. Existiert der Eintrag nicht, laeuft keine Kalibrierung.
        struct Calib {
            int wait{0};
            int sample{0};
        };
        std::optional<Calib> calib{};

        // Settle-Phase nach einem Waffenwechsel: weiter nativ + sampeln, bis der
        // rel-Offset KONVERGIERT. Ohne das lockt man mitten im Blend und die
        // Hand steht neben dem Griff (Log wp4402/4902: Werte pendelten
        // 0.31-0.41 ohne Ende).
        struct Settle {
            int stable{0};
            int timeout{0};
        };
        std::optional<Settle> settle{};
        bool frozen{false};
        // [MATILDA_STOCK 08.09.2026] Zustand der Stock-Erkennung. Sie fragt die
        // ENGINE-DATEN (isExistsParts), nicht mehr den Renderzustand eines
        // gecachten via.render.Mesh -- deshalb hier nur noch Wert + Zeitpunkt.
        double stock_t{-1.0};
        bool stock_on{false};
        std::optional<double> _parry_last_gun{};
    } m_wep_cache{};

    struct SkullSpin {
        std::optional<double> t0{};
    } m_skull_spin{};

    struct KnifeFlip {
        float lerp{0.0f};
        float prev_target{0.0f};
    } m_knife_flip{};

    // [FLIP-MESSUNG 04.09.] Wieviele Paesse ein voller Flip braucht -- die eine
    // Zahl, die "zu langsam" beweist. Raus, sobald geklaert.
    int m_flip_diag_passes{0};
    int m_flip_diag_logs{0};

    // native_reload_active-Cache (Lua Z.1843)
    struct NativeReload {
        Handle go{};
        Handle comp{};
    } m_nr{};

    // ======================================================================
    // FLASHLIGHT (Lua Z.3658-4201)
    // ======================================================================
    struct FlOffset {
        float pos_x{0.0f}, pos_y{0.0f}, pos_z{0.0f};
        float rot_pitch{0.0f}, rot_yaw{0.0f}, rot_roll{0.0f};
    };

    bool  m_fl_enabled{true};
    bool  m_fl_keep_on_knife{true};
    // [TEIL2/L5] geladen, gespeichert, in der UI editiert -- aber NIRGENDS
    // gelesen. Funktional tot, wandert 1:1 mit.
    std::string m_fl_knife_pose{"pose1"};
    FlOffset m_fl_offset{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    FlOffset m_fl_light_offset{0.0f, 0.0f, 0.0f, 0.0f, 180.0f, 0.0f};

    struct FlState {
        Handle flashlight_tf{}, light_tf{}, body_tf{};
        double last_check{0.0};
        Handle flashlight_mesh{};
        bool   mesh_hidden{false};
        FlOffset docked_offset{0.0f, 0.15f, -0.1f, -45.0f, 0.0f, 0.0f};
        FlOffset docked_light_offset{0.0f, 0.0f, 0.0f, 0.0f, 180.0f, 0.0f};
        // [FL_FLIP] rot_* ist eine NACHGESCHALTETE Drehung auf die fertige
        // Lampen-Rotation, nicht auf die Offset-Winkel addiert.
        FlOffset flip{0.0f, 0.0f, 0.0f, 180.0f, 0.0f, 0.0f};
        float  flip_finger_deg{-35.0f};
        // Zeitbasierter Lerp: apply_flashlight laeuft 2x pro Frame.
        float  flip_lerp{0.0f};
        double flip_last_t{-1.0};
        float  flip_time{0.15f};
        float  flip_prev_target{0.0f};
        std::optional<int32_t> flip_snd{};
        // [TEIL3/L1] Diese beiden nullt on_script_reset NICHT -- im Port
        // trotzdem halten und freigeben, sonst Absturz nach Levelwechsel.
        Handle light_comp{};
        bool   scope_hidden{false};
    } m_fl_state{};

    // Lua merkt sich ein erfolgloses Suchen der Light-Component als
    // `light_comp = false`, damit nicht jeden Frame neu gesucht wird. Nativ
    // trennt das ein eigenes Flag vom Handle.
    bool m_fl_light_comp_searched{false};
    bool m_fl_snd_resolved{false};

    // [ADA_FL] Eigener Pfad, eigene Handles. Im Original die Global-Tabelle
    // __re4_ada_fl, die den Script-Reset ueberlebt (zweiter Nachtrag V3).
    Handle m_ada_fl_tf{};
    Handle m_ada_fl_light_tf{};
    double m_ada_fl_last_check{0.0};

    // [ADA_LAZY] Body-Gate, 2x/s geprueft.
    bool   m_ada_body_cache_is_ada{false};
    double m_ada_body_cache_t{-1.0};

    // ======================================================================
    // Pose-Fades und Wurf/Schwung (Lua Z.4214-4600)
    // ======================================================================
    struct PoseFade {
        std::string name{};
        std::optional<double> release_t{};
    } m_rack_pose_fade{};

    struct MagPoseFade {
        std::string name{};
        float trx{0.0f}, try_{0.0f}, trz{0.0f};
        std::optional<double> release_t{};
    } m_mag_pose_fade{};

    struct SkullOpen {
        bool prev{false};
        double start_t{-1.0};
    } m_skullopen{};

    struct KnifeSwing {
        std::optional<glm::vec3> last_wp{};
        double last_time{0.0};
        float  velocity{0.0f};
        double swing_end{0.0};
        double last_swing{0.0};
        float  threshold{3.5f};   // m/s -- nur ECHTE Schwuenge (1.0 loeste beim Gehen aus)
        float  hold_time{0.15f};
        float  cooldown{0.2f};
        // [KNIFE_HAND] Welcher Controller zuletzt gemessen wurde -- ein Wechsel
        // muss last_wp verwerfen, sonst gibt es einen Phantom-Swing.
        std::optional<int> last_cidx{};
        // [PEAK] Maximum im laufenden Schwung-Fenster (fuer choke).
        float  peak{0.0f};
        // [RICHTUNG] Bewegungsvektor des Ausloese-Frames (fuer choke).
        glm::vec3 d{};
    } m_knife_swing{};

    // Joint-Gueltigkeitscache (Lua Z.1120).
    // [TEIL1/L7] Wird NIE geleert; Schluessel ist die Rohadresse. Im Port ein
    // langsames Leck plus die Gefahr recycelter Adressen nach Save/Load.
    // 1:1 uebernommen.
    std::unordered_map<uintptr_t, double> m_jv_bad{};

    // ======================================================================
    // Config
    // ======================================================================
    void load_config();
    void save_config();
    void fl_load_cfg();
    void fl_save_cfg();
    nlohmann::json read_config_raw();
    bool write_config_raw(const nlohmann::json& data);

    bool m_cfg_loaded{false};

    // Offset-Zugriff: get_* = nur lesen (nullptr wenn nicht angelegt),
    // ensure_* = anlegen und aus dem allgemeineren Offset seeden.
    WeaponOffset&        get_weapon_offset(const std::string& key);   // legt an (Ausnahme, s. K2)
    SupportOffset&       get_support_offset(const std::string& key);  // legt an (Ausnahme)
    SupportOffsetAim*    get_support_offset_aim(const std::string& key);
    SupportOffsetAim&    ensure_support_offset_aim(const std::string& key);
    SupportOffsetSwitch* get_support_offset_switch(const std::string& key);
    SupportOffsetSwitch& ensure_support_offset_switch(const std::string& key);
    SupportOffsetAim*    get_support_offset_switch_aim(const std::string& key);
    SupportOffsetAim&    ensure_support_offset_switch_aim(const std::string& key);
    SupportOffsetSwitch2*    get_support_offset_switch2(const std::string& key);
    SupportOffsetSwitch2&    ensure_support_offset_switch2(const std::string& key);
    SupportOffsetSwitch2Aim* get_support_offset_switch2_aim(const std::string& key);
    SupportOffsetSwitch2Aim& ensure_support_offset_switch2_aim(const std::string& key);

    // Die drei ADA_KNIFE_RELFIX-Migrationen (Lua Z.897-960). Ohne ihre Marker
    // wuerde die 180-Grad-Drehung von Adas Messer-Kalibrierung bei jedem
    // Script-Reset erneut laufen und sich damit selbst zurueckdrehen.
    void ada_knife_relfix();

    // ======================================================================
    // Feuerwahlschalter (LE5 4202 / Chicago Sweeper 4201)
    // ======================================================================
    static bool is_switch_dock_weapon(int32_t wid);
    static bool is_switch_oneshot(int32_t wid);
    static const std::vector<int>& fire_mode_cycle(int32_t wid);
    static bool fire_mode_has_single(int32_t wid);
    static int  fire_mode_next(int32_t wid, int cur);

    // ======================================================================
    // Kern
    // ======================================================================
    // Kalibrierung pro Equip: erst CALIB_WAIT Frames (Draw-Anim ausklingen
    // lassen), dann CALIB_SAMPLE Frames die rechte Hand NICHT schreiben (native
    // Engine-Verkettung pur) und den rigid Offset exakt sampeln -> danach
    // EINGEFROREN.
    static constexpr int CALIB_WAIT = 25;
    static constexpr int CALIB_SAMPLE = 5;

    // [SNAP_SOFTEN] Nur Spruenge GROESSER als ~15 Grad werden eingeblendet;
    // kleine Aenderungen gehen 1:1 durch, damit keine Ziel-Latenz entsteht.
    static constexpr float TWO_HAND_SNAP_COS = 0.966f;   // cos(~15 Grad)
    static constexpr float TWO_HAND_SNAP_EASE = 0.25f;   // Einblend-Anteil pro Frame
    // [TOT] RACK_RELEASE_FRAMES = 12 im Original -- der Rack-Freeze ist
    // ersatzlos gestrichen, der Zaehler wird nie mehr gesetzt (nur noch
    // heruntergezaehlt). Bewusst nicht als Konstante uebernommen.

    static constexpr float SETTLE_EPS = 0.002f;      // m, Frame zu Frame
    static constexpr int SETTLE_STABLE_FRAMES = 3;
    static constexpr int SETTLE_TIMEOUT = 40;

    bool m_wep_attach_enabled{true};
    bool m_was_weapon_changing{false};

    void update_weapon_calibration();
    bool weapon_calib_suspends_hand() const;
    bool is_weapon_changing();
    bool update_weapon_changing_gate();
    void update_weapon_settle();

    glm::quat skullshaker_cock_spin(const glm::quat& wrot);
    glm::quat knife_flip_spin(const glm::quat& wrot);
    void knife_play_sound(int32_t id);
    void knife_ks_restore_native();
    void attach_weapon();

    // VR-Rohdaten eines Passes (Lua get_vr_data, Z.1908-1916). Die Rotationen
    // sind Matrizen -- Matrix4x4f:to_quat() == glm::quat(m).
    struct VrData {
        glm::vec3 hmd_pos{};
        glm::vec3 right_pos{}, left_pos{};
        glm::mat4 right_rot{1.0f}, left_rot{1.0f};
    };

    bool get_vr_data(VrData& out);
    bool get_camera_data(glm::vec3& out_pos, glm::quat& out_rot);
    // ctrl_rot_raw == nullptr entspricht Luas nil -> nur Position, Rotation
    // bleibt die Kamerarotation.
    bool controller_to_world(const glm::vec3& ctrl_pos_raw, const glm::mat4* ctrl_rot_raw,
                             const glm::vec3& cam_pos, const glm::quat& cam_rot,
                             glm::vec3& out_pos, glm::quat& out_rot);

    static void write_joint_pose(::REManagedObject* joint, const glm::vec3& pos,
                                 const glm::quat* rot);
    void apply_hand_offset(glm::vec3& pos, glm::quat& rot, const WeaponOffset& off);
    glm::vec3 clamp_hand_to_arm_reach(const glm::vec3& hand_pos, const char* side);

    bool is_two_hand_aim_weapon() const;
    static bool is_two_hand_aim_weapon_id(int32_t wid);

    static bool grip_held(bool left);
    static bool is_left_grip_held() { return grip_held(true); }
    static bool is_right_grip_held() { return grip_held(false); }
    static bool is_left_trigger_held();

    static glm::quat rotation_between(const glm::vec3& a, const glm::vec3& b);

    // [KS4_EXIT_FADE] / [RELOAD_LEXIT_FADE]
    // Eingefrorene Startposen. Im Original liegen sie in den Global-Tabellen
    // __re4_ks4fade.from und __re4_reload_lexit_from -- beide haben KEINEN
    // externen Leser (zweiter Nachtrag V8), also C++-Member.
    struct FadeFrom {
        std::optional<glm::vec3> p{};
        std::optional<glm::quat> r{};
    };

    FadeFrom m_ks4_from_r{};
    FadeFrom m_ks4_from_l{};
    FadeFrom m_reload_lexit_from{};

    std::optional<float> ks4_exit_progress();
    void ks4_exit_apply(::REManagedObject* joint, glm::vec3& pos, glm::quat& rot, bool left);
    void reload_lexit_apply(glm::vec3& pos, glm::quat& rot);

    // [TWO_HAND_IK] Kernstueck -- modifiziert m_cache.rh_rot.
    void apply_two_hand_aim(const VrData& vr_data, const glm::vec3& cam_pos, const glm::quat& cam_rot);

    void attach_right_hand(const glm::vec3& cam_pos, const glm::quat& cam_rot, const VrData& vr_data);

    static bool knife_is_left();
    bool is_support_hand_weapon() const;
    ::REManagedObject* get_pump_joint();
    ::REManagedObject* get_switch_joint();
    // Dock-Pose der Stuetzhand. false = keine Pose (rh fehlt).
    bool get_support_pose(glm::vec3& out_pos, glm::quat& out_rot);
    void play_le5_switch_sound();
    // Dock/Undock-Entscheid + Blend-Fortschritt. NUR im LockScene-Pass rufen.
    void update_support_dock(const glm::vec3& free_pos, const glm::vec3* support_pos);
    // update_dock == true nur im LockScene-Pass.
    void attach_left_hand(const glm::vec3& cam_pos, const glm::quat& cam_rot,
                          const VrData& vr_data, bool update_dock);
    void repin_left();

    // --- Flashlight ---
    static bool apply_pose_runtime(const std::string& name, float blend);
    bool fl_knife_equipped();
    bool get_flashlight_camera(glm::vec3& out_pos, glm::quat& out_rot);
    bool fl_find();
    void fl_set_mesh_visible(bool visible);
    void fl_set_cone_visible(bool visible);
    bool fl_scope_update();
    const char* fl_busy_reason();
    bool fl_left_hand_busy() { return fl_busy_reason() != nullptr; }
    void apply_flashlight(const glm::vec3& hand_pos, const glm::quat& hand_rot);
    void apply_ada_lazy_pose();
    bool ada_fl_find();
    void ada_fl_apply();
    void fl_dispatch(const glm::vec3& lh_world, const glm::quat& lh_rot);
    void fl_apply_hold_pose();
    // Ein Offset-Block als eigener Tree (Pos XYZ + Pitch/Yaw/Roll).
    bool draw_offset_tree(const char* label, FlOffset& t);
    void draw_offset_sliders(const char* prefix, WeaponOffset& off,
                             float pr = 0.5f, float sp = 0.001f);
    void draw_support_sliders(const char* suffix, float& px, float& py, float& pz,
                              float& pitch, float& yaw, float& roll);
    bool lua_table_slider(const char* table, const char* field, const char* label,
                          double def, float lo, float hi, const char* fmt = "%.3f");
    bool lua_global_slider(const char* name, const char* label, double def, float lo, float hi,
                           const char* fmt = "%.3f");

    // --- Post-Anim-Posen ---
    void apply_rack_hand_pose_from_reload();
    void apply_switch_hand_pose();
    void knife_flip_finger_open();
    static void add_local_rotation(::REManagedObject* owner_joint,
                                   const std::vector<std::string>& bones, const glm::quat& add);
    void apply_mag_hand_pose_from_reload();
    void apply_skullshaker_open_pose();
    void apply_bolt_idle_pose();
    void update_knife_swing();
    void elevator_unparent();
    void apply_pistol_support_pose();
    void apply_switch_rotation();
    void fl_flip_finger_open();

    // --- [STILLZONE] ---
    std::optional<int32_t> stillzone_cam();
    bool stillzone_camints(int32_t& gimmick, int32_t& battle_normal);
    static bool stillzone_ks();
    void stillzone_tick();

    void on_begin_rendering();
    void on_update_joint_expression();

    void ensure_public_ui_registered();
    bool m_public_ui_registered{false};
    bool m_dispatcher_present{false};

    bool   m_sz_halt{false};
    bool   m_sz_phase_b{false};
    bool   m_sz_prev{false};
    bool   m_sz_gimmick_seen{false};
    bool   m_sz_ks_seen{false};
    double m_sz_logt{-9.0};
    bool   m_sz_cam_ints_ok{false};
    int32_t m_sz_cam_gimmick{0};
    int32_t m_sz_cam_battle{0};
    // Rueckgabe in Phase B. motion kennt drei Stufen: 0 = still, 1 = "haende"
    // (nur der Render-Pass frei), 2 = ganz frei. ENDSTAND: alle drei bleiben die
    // ganze Fahrt still -- die Schalter existieren nur noch als Reserve.
    int  m_sz_back_motion{0};
    bool m_sz_back_movement{false};
    bool m_sz_back_holster{false};

    // Charakter-Erkennung. Fremd-Schnittstelle: wird als __re4_char_now nach
    // Lua exportiert (binding, reload_adv, weapons2, RE4VRCrosshair lesen sie).
    std::string char_now();

    std::optional<int32_t> get_equip_weapon_id();

    // [MATILDA_STOCK] Sitzt die Schulterstuetze auf der Matilda? Antwort aus den
    // Waffendaten der Engine, gecacht (s. RE4VRMotion.cpp).
    bool stock_mounted_data();
    std::string current_weapon_key();
    bool is_krauser_body();   // [KNIFE_KRAUSER 17.09.2026] ch6i2z0_body

    // [KNIFE_THROW_OFFSET 17.09.2026] Krausers Messer bekommt nach einem Wurf
    // eigene Offsetwerte: der Respawn landet nachweislich anders als der Zug aus
    // dem Holster, also wird er getrennt eingestellt statt weiter gesucht.
    // Gesetzt, sobald ein Wurf flog; zurueckgesetzt beim naechsten Waffenwechsel
    // (auch Wegstecken + neu Ziehen zaehlt, die wid wechselt dabei).
    bool m_knife_after_throw{false};
    std::optional<int32_t> m_knife_last_wid{};
    std::string knife_wep_key(int32_t wid);
    std::string rel_key(int32_t wid);
    static bool is_knife_rel_split_id(int32_t wid);

    void store_weapon_rel();
    void find_weapon();
    bool sample_weapon_rel_direct();

    ::REManagedObject* find_player_body();
    bool is_joint_valid(::REManagedObject* joint);
    void find_joints();
    void restore_hands_native();        // [KILLSWITCH_RESTORE]
    bool is_killswitch_active();
    bool native_reload_active();
    void release_motion_targets();

    void tick(bool do_weapon_sample);
    void publish_globals();

    Handle m_character_manager{};
    ::REManagedObject* m_t_motion{nullptr};
    ::REManagedObject* m_t_snd{nullptr};    // soundlib.SoundContainer
    ::REManagedObject* m_t_mesh{nullptr};   // via.render.Mesh
    ::REManagedObject* m_t_elevator{nullptr};   // chainsaw.GmElevator
    ::REManagedObject* m_t_motion_fsm2{nullptr};   // via.motion.MotionFsm2

    // [BURST] Gecachter NAME des Schalter-Joints (nicht das Joint selbst).
    std::string m_switch_joint_name{};
    int32_t m_switch_joint_wid{0};

    // Zeitbasis: MUSS os.clock() sein -- fremde Lua-Dateien schreiben
    // Zeitstempel in dieselben Globals (__re4_parry_keep_gun_until,
    // __re4_choke_knife_stuck). Eine eigene Uhr braeche sie lautlos.
    static double clock_now();
};

#endif // RE4
