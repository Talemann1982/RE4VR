// ============================================================================
// RE4VRReloadAdv -- 1:1-Portierung von re4_vr_reload_adv.lua (1.291 Zeilen).
//
// Der ADVANCED Mag-Slide. In Lua ein MODUL `M`, veroeffentlicht als
// _G.__re4_reload_mag_slide -- die fuenf Reload-Dateien greifen an rund 250
// Stellen auf 26 seiner Member zu (has_shell_keys 38x, shell_pose_at 32x,
// dazu VERAENDERLICHE Tabellen wie shell_live, push, KEYFRAME_INSERT).
//
// Spezifikation: I:\LUATRANS\PORT_RELOAD_STRATEGIE.md
//
// ----------------------------------------------------------------------------
// KEINE Lua-Tabelle mehr.
//
// Solange die fuenf Reload-Dateien Lua waren, haette dieser Port eine komplette
// Tabellen-Bruecke mit 26 Membern gebraucht -- inklusive der veraenderlichen
// (die UI von reload2 schreibt `ms.shell_live.x` direkt). Genau deshalb wurde
// reload_adv im September ans ENDE der Reihenfolge geschoben und der ganze
// Block als EINES ausgeliefert: hier rufen die Geschwister-Teile direkt.
//
// REIHENFOLGE: laeuft als LETZTES der sechs Teile (ASCII '_' > '2'), obwohl es
// die Bibliothek ist. Das geht, weil die fuenf es ausschliesslich zur LAUFZEIT
// ansprechen, nie zur Ladezeit.
// ----------------------------------------------------------------------------
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"

class RE4VRReloadAdv {
public:
    // Ein Keyframe der Shell-/Mag-Bahn: lokale Pose relativ zur Waffe.
    struct Key {
        float x{}, y{}, z{};
        float rx{}, ry{}, rz{};
    };

    // Per-Waffe-Config des Mag-Drops.
    struct WCfg {
        glm::vec3 exit{0.0f, -0.10f, 0.0f};
        float slide_dur{0.18f};
        float gravity{9.8f};
        float fall_dist{0.85f};
        float fall_dur{0.55f};

        // [LANDE_POSE] Ziel-Weltrotation (Euler-Grad), in die das Mag WAEHREND
        // des Falls hineindreht. Default = an der Punisher getunte Liege-Pose.
        bool land_on{true};
        float land_rx{89.5f}, land_ry{-157.5f}, land_rz{125.0f};
    };

    // [EINLEIT-PUNKT] Kammereingang pro Waffe als JOINT + Versatz.
    struct Dock {
        std::string joint{"_03"};
        float x{0.0f}, y{-0.092f}, z{-0.061f};
    };

    void on_initialize();
    void on_lua_state_destroyed();

    // Pass-Einstiege (Lua: on_application_entry / on_pre_application_entry)
    void on_lock_scene_pre();
    void on_late_update();
    void on_begin_rendering_pre();
    void on_begin_rendering();
    void on_update_joint_expression();

    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // ------------------------------------------------------------------
    // Was die fuenf Reload-Teile benutzen (in Lua die Member von `M`).
    // ------------------------------------------------------------------

    // -- Einleit-Punkt --
    const Dock* dock(int32_t wid) const;            // NIE auto-anlegen
    Dock& dock_or_create(int32_t wid);              // nur fuer die UI
    std::optional<glm::vec3> dock_world(::REManagedObject* weapon_tf, int32_t wid);
    std::optional<glm::vec3> dock_local(::REManagedObject* weapon_tf, int32_t wid);

    // -- Per-Waffe-Config --
    WCfg& wcfg(int32_t wid);

    // -- Shell-Insert-Keyframes --
    bool has_shell_keys(int32_t wid);
    bool shell_pose_at(int32_t wid, float tt, Key& out);
    bool apply_shell_keys(::REManagedObject* weapon_tf, ::REManagedObject* joint,
                          int32_t wid, float tt);
    std::vector<Key>& shell_keys(int32_t wid);
    void shell_add_key(int32_t wid);

    // -- Mag-Eject-Keyframes --
    bool has_eject_keys(int32_t wid);
    bool eject_pose_at(int32_t wid, float tt, Key& out);
    bool apply_eject_keys(::REManagedObject* weapon_tf, ::REManagedObject* joint,
                          int32_t wid, float tt);
    std::vector<Key>& eject_keys(int32_t wid);
    void eject_add_key(int32_t wid);
    bool eject_grab_rest();

    // [INSERT = EJECT RUECKWAERTS] Statt einer eigenen Insert-Bahn faehrt der
    // Einschub die AUSWURF-Bahn rueckwaerts ab.
    bool uses_rev_insert(int32_t wid);
    std::optional<float> kf_insert_dur(int32_t wid);

    // -- Drop-Maschine --
    bool begin_drop(::REManagedObject* joint, int32_t wid,
                    const std::optional<float>& dur_override,
                    const std::optional<glm::vec3>& exit_local);
    void cancel();
    bool is_active() const { return m_drop.active; }
    void tick();

    // -- Push-Pose (linke Hand drueckt das Mag rein) --
    void start_push(int32_t wid);
    void stop_push();
    void begin_push_hold(int32_t wid);
    void end_push_hold();
    void start_push_test();
    float push_blend();
    void push_pos(std::optional<int32_t> wid, float& x, float& y, float& z);
    float push_y_extra(std::optional<int32_t> wid);
    float time_mult();

    // -- [KFH 2026-09-24] Keyframe-Handposen (Nachbau RE9 Push/End pose) --
    // Pro Waffe, Default AUS: ohne "on" laeuft alles exakt wie vorher.
    // Keyframe 1: ab dem Andocken klemmt die linke Hand an der Shell/Patrone
    // (Versatz relativ zum Objekt auf der Bahn), Finger "(kf1)".
    // End: Hand am letzten Keyframe (Versatz relativ zur WAFFE), Finger
    // "(kfend)", Ueberblenden im Bahn-Fenster fade_from..fade_to, danach
    // end_hold halten und ueber out_dur ausblenden.
    struct KfHand {
        bool on{false};
        float in_dur{0.10f};
        float px{0.0f}, py{0.0f}, pz{0.0f};
        float rx{0.0f}, ry{0.0f}, rz{0.0f};
        bool end_on{false};
        float epx{0.0f}, epy{0.0f}, epz{0.0f};
        float erx{0.0f}, ery{0.0f}, erz{0.0f};
        float fade_from{0.6f}, fade_to{1.0f};
        float end_hold{0.15f};
        float out_dur{0.20f};
    };
    static bool kfh_in_scope(int32_t wid);
    static int32_t kfh_path_id(int32_t kfid);   // [KFH ADA] Keyframe-Bahn zur Handpose-ID
    const KfHand* kfh_cfg(int32_t wid) const;   // nullptr = nichts eingestellt
    void kfh_begin(int32_t wid);                // Einlegen startet an Keyframe 1
    void kfh_prog(float t);                     // Bahn-Fortschritt 0..1
    void kfh_end();                             // eingerastet
    void kfh_cancel();                          // abgebrochen
    bool kfh_publish();                         // Hand-Dock-Ziel (aus publish_dock)
    bool kfh_force_on() const { return m_kfh_force != 0; }   // Shell sichtbar halten
    // [KFH R9] Module mit eigener Waffen-Transform / eigenen Posen (Reload2 ...)
    void kfh_set_weapon(::REManagedObject* tf);                 // jeden Frame, solange die Waffe fuehrt
    bool kfh_force_key(int32_t kfid, Key& out);                 // Force-Lage auf der Bahn
    void kfh_set_base(int32_t kfid, const re4vr::wpose::Bones& b);
    const re4vr::wpose::Bones* kfh_base(int32_t kfid) const;
    int32_t kfh_ui_id();                                        // 4002 -> 40021 im Einzelpatronen-Modus
    void draw_kf_ui();                          // Dev-Baum "RE4VR - Keyframes"

    // Allowlisten, die die fuenf Reload-Teile direkt abfragen
    // (ms.KEYFRAME_INSERT 14x, ms.PUSH_WIDS 4x, ms.SHELL_CLONE).
    bool is_keyframe_insert(int32_t wid) const;
    bool is_keyframe_eject(int32_t wid) const;
    bool is_push_wid(int32_t wid) const;
    bool is_shell_clone(int32_t wid) const;

    // -- Live-Preview --
    void start_preview(float seconds);
    void tick_preview();
    void set_current_mag_joint(::REManagedObject* j) { m_current_mag_joint = j; }

    // Direkter Zugriff auf die veraenderlichen Werte, die in Lua Tabellen-
    // Member von `M` sind (shell_live, push, shell_dur, ...).
    float shell_dur{0.40f};
    float r9_anlauf{0.30f};
    float eject_dur{0.35f};
    float rev_insert_dur{0.35f};

    bool shell_preview{false};
    float shell_prev_t{0.0f};
    Key shell_live{};

    bool eject_preview{false};
    float eject_prev_t{0.0f};
    Key eject_live{};

    int32_t shell_clone_part{1};
    float shell_clone_scale{1.0f};

    bool r9_single{false};

    // [PUSH_POSE] universell, nicht pro Waffe.
    struct Push {
        bool on{true};
        float in_dur{0.07f};
        float hold{0.10f};
        float out_dur{0.14f};
        float curl{85.0f};
        float thumb{15.0f};

        // [DOCK-RELEASE] Ausfaden der Hand vom Magazin-Dock zum Controller.
        float release_dur{0.20f};

        // [MASTER-TEMPO] Zeitfaktor fuers gesamte Reinladen.
        float reload_speed{1.0f};

        // [FALL-GEWICHT] globaler Gravity-Faktor fuer den gedroppten Mag-Fall.
        float fall_grav_mult{2.5f};

        // [DROP_MOMENTUM 2026-09-09] Anteil der Bewegung, die das gedroppte Mag
        // beim Loslassen mitnimmt. 0 = senkrecht fallen wie bisher.
        float drop_momentum{0.0f};

        float rx{0.0f}, ry{0.0f}, rz{0.0f};   // Hand-Rotations-Offset (Grad)
        float px{0.0f}, py{0.0f}, pz{0.0f};   // Hand-Positions-Offset (m)

        // [ADA] ZUSAETZLICHER Offset, der NUR bei ihr oben drauf kommt.
        float ada_px{0.0f}, ada_py{0.0f}, ada_pz{0.0f};
    } push{};

    // [TUNING] Dauer-Toggle: haelt die Pose permanent auf Blend 1.0. Wird
    // BEWUSST nicht gespeichert und beim Script-Reset geloescht.
    bool push_tune{false};

    // [MANUAL_INSERT] Beim Einschieben VON HAND dauert der Push so lange, wie
    // der Spieler braucht -> die Uhr wird angehalten.
    bool push_hold{false};

    std::unordered_map<int32_t, float> push_y_by_wid{};

    // M.push_wid: die Waffe, mit der die laufende Push-Geste gestartet wurde.
    std::optional<int32_t> push_wid{};

    void save_cfg();
    void load_cfg();

private:
    // [KEIN AUTO-ANLEGEN 2026-07-23] Ohne ausdruecklichen Eintrag gibt es
    // KEINEN Einleit-Punkt -- frueher bekam z.B. die LE 5 automatisch den
    // Standardpunkt und ihr Magazin schwebte heran.
    std::unordered_map<int32_t, Dock> m_docks{};
    std::unordered_map<int32_t, WCfg> m_weapons{};

    // Keyframe-Stores. In Lua GLOBALE Tabellen
    // (__re4_shell_keys_by_wid / __re4_mag_eject_keys_by_wid).
    std::unordered_map<int32_t, std::vector<Key>> m_shell_keys{};
    std::unordered_map<int32_t, std::vector<Key>> m_eject_keys{};

    // [INSERT = EJECT RUECKWAERTS] Schalter pro Waffe. Der Code-Default ist die
    // BASIS -- load_cfg legt die JSON nur DRUEBER, statt die Tabelle zu
    // ersetzen. So kann ein save_cfg den Satz nicht leerraeumen, und ein
    // bewusst abgeschalteter Eintrag (false) gewinnt trotzdem gegen den Default.
    std::unordered_map<int32_t, bool> m_rev_insert{};

    void seed_eject_keys();
    void shell_preview_apply();
    void eject_preview_apply();
    void push_apply();
    void wpose_force();   // [WPOSE]

    // [KFH] Laufzeit + Ablage
    std::unordered_map<int32_t, KfHand> m_kfh{};
    struct KfhRun {
        bool active{false};          // Bahn laeuft
        int32_t wid{0};
        double t0{0.0};
        float prog{0.0f};
        double prog_t{0.0};               // letzte Fortschritts-Meldung (Abbruch-Erkennung)
        std::optional<double> end_t0{};   // End-Halten/Ausblenden laeuft
        bool end_mode{false};             // true = End-Pose, false = K1 einfrieren
        glm::vec3 frz_p{};                // eingefrorene Hand (waffenlokal)
        glm::quat frz_r{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 last_p{};               // letzte Hand-Weltlage
        glm::quat last_r{1.0f, 0.0f, 0.0f, 0.0f};
        bool have_last{false};
    } m_kfr{};
    int m_kfh_force{0};              // 0 aus, 1 = Keyframe 1, 2 = End (nie gespeichert)
    ::REManagedObject* m_kfh_wtf{nullptr};   // [KFH R9] Waffe vom fuehrenden Modul
    double m_kfh_wtf_t{-100.0};
    std::unordered_map<int32_t, re4vr::wpose::Bones> m_kfh_base{};
    bool m_kfh_pub_prev{false};              // Dock im letzten Aufruf veroeffentlicht
    void kfh_publish_adv();                  // Adv-Paesse: nach ALLEN Modulen
    float m_kfh_fblend{0.0f};        // Finger-Blend dieses Frames
    float m_kfh_fend{0.0f};          // Anteil End-Finger (0..1)
    int32_t m_kfh_fwid{0};
    bool kfh_target(glm::vec3& p, glm::quat& r, float& b);
    void kfh_fingers();
    void kfh_force_park();
    nlohmann::json kfh_to_json() const;
    void kfh_from_json(const nlohmann::json& j);

    std::unordered_map<std::string, glm::quat> push_bones();

    // ---- Drop-State (ein Mag-Joint) ----
    struct Drop {
        bool active{false};
        std::string phase{};   // "slide" | "fall"
        ::REManagedObject* joint{nullptr};
        int32_t wid{0};
        double t0{0.0};

        float lx0{}, ly0{}, lz0{};   // Ruhe-Lokalpos
        float ex{}, ey{}, ez{};      // Exit-Lokalpos
        float sx{}, sy{}, sz{};      // Fall-Start (Welt)

        // [ENTKOPPEL_ROT] Welt-Rotation im Moment des Fall-Starts.
        std::optional<glm::quat> srot{};

        float slide_dur{0.18f};
        float gravity{9.8f};
        float fall_dist{0.85f};
        float fall_dur{0.55f};

        bool use_keys{false};
        std::optional<float> floor_y{};

        // [DROP_MOMENTUM 2026-09-09] Ein kleiner Rest der Bewegung, die das Mag
        // im Moment des Loslassens hatte -- sonst bleibt es beim Laufen sofort
        // zuruck und wirkt gewichtslos. Gemessen wird sie an der letzten
        // Bewegung des Magazins selbst, es braucht also keinen neuen Getter.
        // Anteil kommt aus dem Lua-Global __re4_mag_drop_momentum, Default 0
        // = exakt das bisherige Verhalten.
        float vx{0.0f}, vz{0.0f};                 // uebernommene Welt-Geschwindigkeit
        std::optional<glm::vec3> prev_p{};        // letzte Weltposition der WAFFE
        double prev_t{0.0};
        std::optional<float> land_t{};            // Zeit des Aufschlags -> Schwung endet
    } m_drop{};

    // ---- Preview ----
    struct Preview {
        bool active{false};
        double until_t{0.0};
        ::REManagedObject* joint{nullptr};
        float lx0{}, ly0{}, lz0{};
        std::optional<int32_t> wid{};
    } m_preview{};

    ::REManagedObject* m_current_mag_joint{nullptr};

    std::optional<double> m_push_t0{};
    std::optional<int32_t> m_push_wid{};

    // [POSE1] Die gecapturte Nachdrueck-Pose aus der JSON (push.bones).
    // Fehlt sie, faellt es auf die generierte flache Hand zurueck.
    std::unordered_map<std::string, glm::quat> m_push_bones_json{};

    // [FLOOR] Boden-Y unter dem Spieler. Eigener Singleton-Cache (`floor_cm` in
    // Lua) -- diese Stelle geht bewusst NICHT ueber den Frame-Cache.
    std::optional<float> get_floor_y();
    ::REManagedObject* m_floor_cm{nullptr};
    ::REManagedObject* m_character_manager{nullptr};

    ::REManagedObject* get_ctx();
    std::optional<int32_t> get_equip_wid();

    // Quelle wie in Lua: __re4_reload_ui_wid, sonst __vr_equip_wid, sonst 0.
    int32_t push_wid_now();
};

#endif
