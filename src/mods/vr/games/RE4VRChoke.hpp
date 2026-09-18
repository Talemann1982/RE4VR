// ============================================================================
// RE4VRChoke -- 1:1-Portierung von re4_vr_choke.lua (2.970 Zeilen).
//
// Gegner im Parry-/Damage-Fenster mit der LINKEN Hand am Hals greifen, halten
// und messern. Fuenf Bausteine:
//   (1) Zielsuche + Zugriff        pick_target / grab
//   (2) Halten ueber die Phasen    apply_hold / torso_enforce / apply_choke_pose
//   (3) Stich ueber die SPITZE     tip_tick  (alter Weg via TIP.on abschaltbar)
//   (4) Messer steckenlassen       stick_into_victim / stick_return + Abdunkeln
//   (5) Loslassen + Rueckwechsel   release
//
// Spezifikation: I:\LUATRANS\PORT_CHOKE_SPEC.md
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- **VOR RE4VRMotion**, damit auch vor dem
// ScriptRunner. Nicht verschieben (belegt, nicht geraten):
//
//   RE4VR -> **RE4VRChoke** -> RE4VRMotion -> ScriptRunner -> ...
//
// * Alphabetisch laedt re4_vr_choke.lua VOR re4_vr_motion.lua. Choke sah die
//   Motion-Globals (__vr_lh_world, __vr_rh_world, vr_knife_swing,
//   vr_knife_velocity, vr_knife_velocity_peak) also im VORFRAME-Stand, und
//   Motion sah die Choke-Globals FRISCH.
// * RE4VRMotion liest __re4_choke_seen/_hand_y/_hand_play/_dy/_y_lerp/
//   _swing_threshold/_knife_stuck und ruft __re4_is_choking(). RE4VRMaterials
//   liest __re4_choke_seen + __re4_choke_victim, RE4VRCrosshair ruft
//   __re4_is_choking(). Stuende Choke dahinter, saehe jeder dieser Leser die
//   Werte einen Frame zu spaet -- die Wuergehoehe der Hand haengt daran.
// * Die weapons2-Globals (__re4_knife_direct_damage_at, __re4_knife_reach,
//   __re4_knife_saved_vals) kommen weiter aus dem ScriptRunner, also von
//   HINTER uns -- genau wie in Lua.
// ----------------------------------------------------------------------------
// ============================================================================

#pragma once

#if defined(RE4)

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "RE4VR.hpp"

class RE4VRChoke : public Mod {
public:
    static std::shared_ptr<RE4VRChoke>& get();

    // [KLON-DIM 2026-09-12 -- Ansage "der muss bei allen Klonen gleich gelten,
    // sobald sie im Gegner stecken"] Der Regler "Steckendes Messer abdunkeln"
    // sass bisher NUR am alten Steckweg, in dem die echte Waffe in den Gegner
    // wandert. Der Regelfall ist seit dem [STICK-KLON] aber eine KOPIE -- und
    // der Wurf hat seit dem 12.09. seine eigenen (bis zu fuenf). Beide sahen
    // den Regler nie.
    //
    // Diese Fassung dunkelt ein Mesh EINMAL ab und merkt sich nichts: ein Klon
    // wird zerstoert, nicht zurueckgesetzt. Der Rueckweg (dim_apply(false))
    // bleibt dem echten Messer vorbehalten.
    static void dim_mesh_once(::REManagedObject* mesh, float f);

    bool knife_dim_on() const { return m_cfg.knife_dim; }
    float knife_dim_factor() const { return m_cfg.knife_dim_f; }

    std::string_view get_name() const override { return "RE4VRChoke"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // [CHOKE_UI] Entspricht _G.__re4_is_choking -- Frische-Fenster 0.15 s auf
    // dem Herzschlag __re4_choke_seen. Wird als Lua-Global exportiert (Motion,
    // Crosshair, binding.lua und reload.lua rufen es) und intern direkt benutzt.
    bool is_choking() const;

    // [WIEDERVERWENDUNG] _G.__re4_blade_tip -- re4_vr_weapons.lua braucht fuer
    // seinen Messerwurf dieselbe selbstgemessene Klingenlaenge. Rueckgabe wie
    // im Original: Spitze, Einheitsvektor der Klinge, Laenge.
    bool blade_tip_public(::REManagedObject* ktf, const std::string& kname,
                          glm::vec3& tip, glm::vec3& axis, float& len);

private:
    // ------------------------------------------------------------------
    // Engine-Handle ueber Frames. Rohzeiger reichen NICHT: sol_lua_push macht
    // in Lua add_ref unsichtbar mit.
    // ------------------------------------------------------------------
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    // force_ref: fuer SELBST ERZEUGTE Objekte Pflicht. Ein frisches
    // GameObject hat referenceCount 0, die Heuristik unten wuerde es also
    // NICHT pinnen -- und beim naechsten GC-Lauf greifen wir ins Leere.
    void store(Handle& h, ::REManagedObject* o, bool force_ref = false);
    void drop(Handle& h);

    // ------------------------------------------------------------------
    // Stellschrauben. Namen und Vorgaben 1:1 aus dem Kopf der Lua-Datei.
    // ------------------------------------------------------------------
    struct Cfg {
        bool  choke_on{true};
        // [HUHN 2026-09-10] Huehner greifen wie Gegner. Gegner haben immer
        // Vorrang; ein Huhn kommt nur dran, wenn keiner in Reichweite ist.
        bool  chicken_grab{true};
        bool  leon_campaign_only{false};
        bool  require_parry{true};
        bool  debug{false};

        float grip_away{0.18f};
        float grip_side{0.0f};
        float grip_up{0.0f};

        float grab_dist{1.30f};
        float hand_min_y{0.45f};
        float hand_max_y{2.20f};
        float hold_max{2.0f};
        // [HUHN 2026-09-11] Eigene Haltedauer -- ein Huhn darf laenger
        // gehalten werden als ein Gegner.
        float chicken_hold{5.0f};
        // Wwise-ID, die Leon von sich gibt, wenn sich das Huhn nach Ablauf
        // der Zeit losreisst. 0 = kein Sound. BEWUSST uint32: IDs ab 2^31
        // sterben an einem signed-Vergleich (teuer bezahlt, s. Notiz
        // "Wwise-IDs sind UInt32").
        uint32_t chicken_break_snd{0u};

        // [TAUNT-WAV 2026-09-12] Lautstaerke der EIGENEN Sprach-WAVs in dB.
        // Wirkt auf beide Verwender (Tiergriff und Mittelfinger-Geste), nicht
        // auf Spielsounds. Vorbesetzt mit -3: der Ansage nach lagen die
        // Aufnahmen "2-3 dB zu laut" gegenueber dem Spielton.
        float taunt_vol_db{-3.0f};

        // [ADA LAUTER 17.09.2026] Zuschlag nur fuer Adas eigene Gesten-WAVs,
        // AUF taunt_vol_db. +8: ihre Dateien liegen ~10 dB unter Leons, so
        // bleibt die lauteste (-9,9 dB) noch unter Vollaussteuerung.
        float ada_gain_db{8.0f};

        // [TAUNT-DELAY 2026-09-12 -- Ansage "nur beim Griff an TIEREN"]
        // Wartezeit in Sekunden zwischen dem Zupacken am Tier und dem
        // Abspielen des eigenen Spruchs. 0 = sofort wie bisher.
        // Gilt BEWUSST NICHT fuer die Mittelfinger-Geste: die loest in
        // RE4VRGuestures aus und bleibt unveraendert (Ansage 12.09.2026).
        float taunt_delay_s{0.0f};

        // [HAND-DREHUNG 12.09.2026 -- Ansage "nicht das HUHN drehen, sondern
        // meine linke Hand"] Dreht den L_Hand-Joint der HALTENDEN (linken) Hand,
        // solange ein Opfer im Griff ist. ADDITIV auf die Controller-Ausrichtung,
        // nicht absolut -- sonst kaempft es gegen die ArmChain.
        float hand_yaw_deg{0.0f};
        float hand_pitch_deg{0.0f};
        float hand_roll_deg{0.0f};

        // [TIER-EINSCHUB 2026-09-12 -- Ansage "stecken zu weit drinn, die
        // braeuchten einen eigenen slider"] Verschiebung des Steckpunkts bei
        // TIEREN, gerechnet AB DER KOERPERMITTE entlang der Klingenachse.
        // Vorzeichen wie bei knife_stick_in: positiv = tiefer hinein,
        // NEGATIV = heraus. 0 = genau die Mitte.
        //
        // Eigener Wert, damit knife_stick_in (Gegner) unangetastet bleibt --
        // der ist bei Tieren ohnehin wirkungslos, weil die Mitte ihn
        // ueberschreibt.
        float animal_stick_in{0.0f};
        float cooldown{1.0f};
        float parry_window{3.0f};
        bool  parry_target_only{true};

        bool  knife_to_right{true};
        bool  knife_stick{true};
        float knife_stick_t{1.0f};
        float knife_stick_in{0.06f};
        // [SNAP 2026-09-10 -- Testerbefund "steckt neben dem Gegner"]
        // Gemessen lag der Einstichpunkt 0.22-0.33 m vom naechsten von 70
        // Joints entfernt -- er lag also AUSSERHALB des Koerpers, und das
        // Messer steckte voellig korrekt gerechnet einfach dort. Der
        // Schadenspfad zieht seinen Kontaktpunkt laengst auf den Koerper
        // (knife_hit_radius), der Steckpfad tat es nie. 0 = altes Verhalten.
        float stick_snap{0.08f};
        float knife_stick_v{0.0f};
        float knife_stick_w{0.15f};
        float choke_swing_t{0.0f};
        bool  fwd_only{true};

        bool  knife_dim{true};
        float knife_dim_f{0.65f};
        bool  use_native_hit{true};
        float knife_dmg{150.0f};
        float knife_dmg_stick{250.0f};

        float min_neck_y{0.70f};
        float max_neck_y{1.85f};

        bool  freeze_yaw{true};
        float face_yaw_deg{0.0f};
        // [TIER-DREHUNG 12.09.2026 -- Ansage "stell die Slider auf Maus, Bat
        // und Crow um, auf keinen Fall auf alle Tiere oder Gegner"] Frueher die
        // Huhn-Regler, die dort nie gebraucht wurden.
        //
        // Sie wirken auf genau die Rigs, die KEIN Neck_1 haben -- gemessen sind
        // das GmMouse, GmCrow und GmBat, die seit dem 12.09. ueber die Huefte
        // gegriffen werden und deshalb anders in der Hand liegen. Das Huhn
        // (hat Neck_1) und JEDER Gegner bleiben unberuehrt.
        //
        // Additiv auf die berechnete Blickrichtung, alle 0 = exakt wie ohne.
        float animal_yaw_deg{0.0f};
        float animal_pitch_deg{0.0f};
        float animal_roll_deg{0.0f};

        // [TIER-VERSATZ 12.09.2026] Dasselbe fuer die LAGE: X seitlich,
        // Y hoch/runter, Z vor/zurueck -- ADDITIV auf die drei Griff-Regler und
        // in derselben Bezugsrichtung (Linie Koerper -> Hand), damit sie sich
        // beim Drehen des Spielers mitdrehen. Gilt wieder nur fuer Rigs ohne
        // Neck_1: Maus, Kraehe, Fledermaus.
        float animal_off_x{0.0f};
        float animal_off_y{0.0f};
        float animal_off_z{0.0f};

        // [EIGENE SAETZE JE ART 13.09.2026 -- Ansage "fuer Crows brauche ich
        // eigene Offsets als fuer eine Maus, die jetzigen sind fuer eine Maus"]
        // Die animal_*-Werte oben BLEIBEN die Maus und behalten ihre
        // JSON-Schluessel -- an eingestellten Werten aendert sich nichts.
        // Kraehe und Fledermaus bekommen daneben ihre eigenen.
        float crow_yaw_deg{0.0f};
        float crow_pitch_deg{0.0f};
        float crow_roll_deg{0.0f};
        float crow_off_x{0.0f};
        float crow_off_y{0.0f};
        float crow_off_z{0.0f};

        float bat_yaw_deg{0.0f};
        float bat_pitch_deg{0.0f};
        float bat_roll_deg{0.0f};
        float bat_off_x{0.0f};
        float bat_off_y{0.0f};
        float bat_off_z{0.0f};

        // [ASHLEY 13.09.2026 -- Ansage "eigene Slider, exklusiv fuer sie"]
        // Versatz NUR fuer die gehaltene Begleitung, zusaetzlich zum
        // allgemeinen Griff-Versatz (grip_side/up/away). Kein Yaw/Pitch/Roll:
        // sie hat einen Neck_1 und laeuft damit ueber den GEGNER-Weg, der die
        // Pose selbst bestimmt -- Drehregler waeren dort ohne Wirkung.
        // [ASHLEY MUND 16.09.2026] Sie bewegt den Mund zu UNSEREN Spruechen.
        // Geblendet wird zwischen zwei Posen, die aus ihrer eigenen
        // Sprechbewegung gemessen sind (RE4VRAshleyMouth.hpp); wie weit, sagt
        // die Huellkurve der gerade laufenden WAV.
        // [ASHLEY ANTWORTET 16.09.2026] Ihre Antwort auf eine Geste.
        //   reply_gain_db  ZUSCHLAG auf den allgemeinen dB-Regler, nur fuer
        //                  diese Sprueche -- sie kommen aus der Entfernung und
        //                  sind sonst zu leise. Der Abstandsabfall kommt danach.
        //   reply_delay_s  Wartezeit zwischen Geste und Antwort. 0 = sofort.
        float reply_gain_db{6.0f};
        float reply_delay_s{0.6f};

        bool  mouth_on{true};
        float mouth_scale{3.0f};    // 1 = wie gemessen, hoeher = weiter aufgerissen
        float mouth_speed{1.0f};    // Tempo der Huellkurve

        float ashley_off_x{0.0f};
        float ashley_off_y{0.0f};
        float ashley_off_z{0.0f};

        // [ASHLEY IMMER GLEICH 13.09.2026] Seit sie ueber denselben
        // HAND-Bezug laeuft wie die necklosen Tiere, wirken die drei Winkel
        // auch bei ihr -- sie sind der feste Versatz zur Handrichtung.
        //
        // 180 als Vorgabe, GEMESSEN im Spiel: die Hand-Basis zeigt VON dir weg,
        // ohne Drehung schaut das Opfer damit in dieselbe Richtung wie du
        // ("sie schaut von mir weg"). Eine halbe Drehung stellt sie zu dir hin.
        float ashley_yaw_deg{180.0f};
        float ashley_pitch_deg{0.0f};
        float ashley_roll_deg{0.0f};

        // [HUHN + GEGNER 13.09.2026 -- Ansage "kannst du direkt machen, jeweils
        // eigener Tree"] Dieselbe Mechanik, eigene Werte. Das Huhn hat ein
        // echtes Neck_1 und lief deshalb bis heute ueber den alten Weg; der
        // Gegner ebenso. Der Gegner bekommt dieselbe 180 wie Ashley -- gleiches
        // Menschen-Rig, gleiche Blickrichtung.
        float chick_yaw_deg{0.0f};
        float chick_pitch_deg{0.0f};
        float chick_roll_deg{0.0f};
        float chick_off_x{0.0f};
        float chick_off_y{0.0f};
        float chick_off_z{0.0f};

        float ene_yaw_deg{180.0f};
        float ene_pitch_deg{0.0f};
        float ene_roll_deg{0.0f};
        float ene_off_x{0.0f};
        float ene_off_y{0.0f};
        float ene_off_z{0.0f};

        // [HANDDREHUNG JE ART 13.09.2026 -- Ansage "die Handrotation brauche
        // ich immer anders, je nachdem wie ich choke"] Bis heute gab es EINEN
        // Satz (hand_yaw/pitch/roll_deg), und der wirkte nur am TIER. Der bleibt
        // die MAUS bzw. der allgemeine Tiersatz; Kraehe, Fledermaus, Huhn,
        // Gegner und Ashley bekommen eigene Winkel.
        float crow_hand_yaw{0.0f};
        float crow_hand_pitch{0.0f};
        float crow_hand_roll{0.0f};

        float bat_hand_yaw{0.0f};
        float bat_hand_pitch{0.0f};
        float bat_hand_roll{0.0f};

        float chick_hand_yaw{0.0f};
        float chick_hand_pitch{0.0f};
        float chick_hand_roll{0.0f};

        // [MAUS 14.09.2026 -- Ansage "die braucht auch einen eigenen Satz"] Sie
        // war die letzte Art ohne eigene Handdrehung und fiel auf den
        // allgemeinen Satz (hand_*_deg) zurueck. Der bleibt als Rueckfall fuer
        // jedes KUENFTIGE Rig stehen -- nur die Maus hoert jetzt auf ihre
        // eigenen Werte. Die KOERPER-Werte der Maus bleiben bewusst beim
        // allgemeinen Tiersatz (animal_*), damit nichts anderes verstellt wird.
        float mouse_hand_yaw{0.0f};
        float mouse_hand_pitch{0.0f};
        float mouse_hand_roll{0.0f};

        // [ADA-MAUS 17.09.2026 -- Ansage des Users] Ada (Separate Ways) greift
        // die Maus mit eigenen Werten: Koerperlage, Versatz, Handdrehung und
        // Dauergriff. Gilt NUR fuer die Maus und NUR, wenn beim Zupacken Adas
        // Koerper (ch3a8z0_body) der Spieler ist. Fehlen die Eintraege in der
        // JSON, starten sie bei Leons Maus-Werten.
        float ada_mouse_yaw_deg{0.0f};
        float ada_mouse_pitch_deg{0.0f};
        float ada_mouse_roll_deg{0.0f};
        float ada_mouse_off_x{0.0f};
        float ada_mouse_off_y{0.0f};
        float ada_mouse_off_z{0.0f};
        float ada_mouse_hand_yaw{0.0f};
        float ada_mouse_hand_pitch{0.0f};
        float ada_mouse_hand_roll{0.0f};

        float ene_hand_yaw{0.0f};
        float ene_hand_pitch{0.0f};
        float ene_hand_roll{0.0f};

        float ashley_hand_yaw{0.0f};
        float ashley_hand_pitch{0.0f};
        float ashley_hand_roll{0.0f};

        // [TUNE-PIN 13.09.2026 -- Ansage "die einzige Szene im Spiel, in der
        // ich eine Fledermaus aus dem Flug greifen kann"] Haken je Art: der
        // Griff bleibt offen, auch wenn der linke Grip losgelassen wird und die
        // Haltedauer ablaeuft. Nur zum EINSTELLEN gedacht -- anders kommt man
        // an eine Fledermaus nie lange genug heran.
        // Beendet wird er durch erneutes Zupacken (Grip-Flanke) oder durch das
        // Abhaken. Der Tod des Opfers loest weiter sofort (is_live).
        // [FLEDERMAUS-RADIUS 13.09.2026] Eigene Greifweite NUR fuer die
        // Fledermaus -- sie fliegt vorbei, alle anderen Tiere stehen. Gilt
        // zusaetzlich zu grab_dist: genommen wird der groessere Wert.
        float bat_grab_dist{2.50f};

        // [AUSBRUCH 14.09.2026 -- Ansage "2 Sekunden spaeter haut sie ab"] So
        // lange bleibt eine Fledermaus in der Hand, dann reisst sie sich los --
        // mit Leons Loslass-Laut. Der DAUERGRIFF-Haken sticht das weiterhin,
        // sonst waere sie nicht mehr einstellbar.
        float bat_hold_s{3.00f};

        bool animal_pin{false};   // Maus bzw. Tier ohne eigenen Haken
        bool ada_mouse_pin{false};   // [ADA-MAUS 17.09.2026]
        bool crow_pin{false};
        bool bat_pin{false};
        bool chick_pin{false};
        bool ene_pin{false};
        bool ashley_pin{false};
        bool  lock_on_grab{false};
        bool  torso_pin{true};
        bool  torso_upright{true};
        bool  grapple_flags{true};
        bool  leg_ik_off{true};
        bool  parent_to_body{true};

        bool  pose_on{true};
        bool  pose_force{false};

        bool  y_clamp_on{true};
        float y_clamp{0.20f};

        // [HAND IMMER GLEICH 13.09.2026 -- Ansage "sollte die Hand nicht immer
        // gleich sein, egal was passiert?"] Seitliche Lage der linken Hand im
        // Griff, gemessen vom Koerpermittelpunkt (+ = rechts). Bis heute wurde
        // sie beim ERSTEN Choke-Frame uebernommen, also aus der Zufallslage
        // beim Zupacken -- die Hand-Sonde zeigte ueber drei Griffe -0,281 /
        // -0,227 / +0,016 m. Damit war der Griff nicht einstellbar.
        // Vor/zurueck bleibt frei: geklemmt wird nur links/rechts und die Hoehe.
        float hand_lat{0.0f};
        bool  reequip_after{true};
        bool  fix_height{true};
        float neck_up{0.08f};
        float y_lerp{0.25f};

        // TIP
        bool  tip_on{true};
        float tip_r{0.22f};
        // [ZUPACK-SPERRE 14.09.2026] Beim Zupacken wird das Opfer in die Hand
        // GESETZT -- seine Trefferkugeln wandern dabei quer ueber die stehende
        // Klinge, und sphere_entry sieht darin einen echten Eintritt. Der Stich
        // kam also vom Opfer, nicht vom Messer. So lange nach dem Griff bleibt
        // der Spitzen-Erkenner deshalb scharf-geschaltet, aber stumm.
        float tip_arm{0.35f};

        float tip_speed{0.80f};
        float tip_len{0.25f};

        // Daumen-Feinjustage, additiv auf CHOKE_POSE (Grad)
        glm::vec3 thumb_t1{0.0f, 0.0f, 0.0f};
        glm::vec3 thumb_t2{0.0f, 0.0f, 0.0f};
        glm::vec3 thumb_t3{0.0f, 0.0f, 0.0f};
    };

    void load_cfg();
    void save_cfg();

    // ------------------------------------------------------------------
    // Helfer
    // ------------------------------------------------------------------
    ::REManagedObject* player_body_tf();
    ::REManagedObject* player_body_go();
    ::REManagedObject* player_equipment();
    std::optional<int32_t> equip_type_main();

    bool have_knife();
    void reequip_last_weapon();

    // [BAREHANDS 2026-09-12] Gegenstueck dazu: wieder leere Haende, ueber
    // denselben Weg, den holster nimmt (requestEquipBareHand + Marke).
    void restore_bare_hands();

    // [TIER-HALS 12.09.2026 -- gemessen mit zzz_re4_tier_joints_probe.lua]
    // Der Griff haengt am Joint "Neck_1". Den hat NUR das menschliche Rig (und
    // das Huhn). Die Messung am lebenden Objekt:
    //   Maus       32 Joints, alles KLEIN: neck_0/neck_1/neck_2, head, spine_2
    //   Kraehe     59 Joints: "Neck" OHNE Index, Head, Chest, Hip
    //   Fledermaus 21 Joints: GAR KEIN Nacken -- nur Hip, Spine_0, Head
    // Darum hing die Maus nicht in der Hand, obwohl Pose und Zustand liefen:
    // getJointByName("Neck_1") lieferte nullptr und der Griff hatte nichts,
    // woran er haette haengen koennen.
    //
    // Diese Kette sucht case-insensitiv nach Rang. GEGNER sind unberuehrt: bei
    // ihnen trifft der erste, exakte Versuch, die Kette laeuft gar nicht an.
    // [ANIM AUF 0 -- 13.09.2026, Ansage "waere dann nicht der Schluessel, die
    // Anime die grade gespielt hat, immer auf 0 zu setzen bei jedem Grab?"]
    // Genau das: die LAUFENDE Animation bleibt, nur ihre PHASE wird beim
    // Zupacken auf den Anfang gestellt. Das Tier zappelt unveraendert weiter
    // ("das sieht geil aus"), sieht aber in jedem Griff gleich aus.
    //
    // Belegt ist, dass es die Phase sein MUSS und nicht die Ausrichtung:
    // REL_HAND_HUEFTE lag ueber alle Griffe bei 105.2 Grad (+-0.2), Position
    // ebenso konstant -- nur der Rumpf drehte sich je nach Schleifenphase
    // anders um die festgehaltene Huefte (ANIM 81..99 Grad).
    void animal_anim_rewind(::REManagedObject* mo);


    std::string neck_joint_name_of(::REManagedObject* tf);
    ::REManagedObject* neck_joint_of(::REManagedObject* tf);

    // Rueckgabe wie in Lua: transform, name, abstand_zur_hand, anzahl
    struct KnifePick {
        ::REManagedObject* tf{nullptr};
        std::string nm{};
        std::optional<float> dist{};
        int count{0};
    };
    KnifePick knife_transform();
    ::REManagedObject* knife_sound_container();

    void play_grab_sound();
    void play_body_sound(uint32_t id);
    void play_enemy_grab_sound(::REManagedObject* ego);
    int  stop_enemy_sounds(::REManagedObject* ego, ::REManagedObject* con);

    void poll_parry();
    bool parry_open() const;
    bool is_leon_campaign();
    std::optional<glm::vec3> hand_pos();
    bool is_live(::REManagedObject* ectx);

    int32_t root_none_value();
    std::optional<int32_t> cat_damage_value(::REManagedObject* cat_obj, bool had_value);

    // ------------------------------------------------------------------
    // Klinge / Spitze
    // ------------------------------------------------------------------
    float blade_len(::REManagedObject* ktf, const std::string& kname, const glm::vec3& u);
    bool  blade_tip(::REManagedObject* ktf, const std::string& kname,
                    glm::vec3& tip, glm::vec3& u, float& len);

    struct Sphere {
        glm::vec3 c{};
        float r{0.0f};
    };
    bool victim_spheres(std::vector<Sphere>& out);
    static bool sphere_entry(const glm::vec3& p0, const glm::vec3& p1, const Sphere& s,
                             glm::vec3& contact, float& t);

    void tip_tick(double now);

    // Was `st` in Lua transportiert; ohne Durchstosspunkt ist `has_c` false.
    struct StickInfo {
        bool has_c{false};
        glm::vec3 c{};
        ::REManagedObject* ktf{nullptr};
        std::string nm{};
        std::optional<float> d{};
        int n{0};
        float len{0.0f};
        float v{0.0f};
    };
    void knife_hit(const StickInfo* st);

    // ------------------------------------------------------------------
    // Stecken
    // ------------------------------------------------------------------
    ::REManagedObject* nearest_joint_of(::REManagedObject* htf, const glm::vec3& p,
                                        std::string& name_out, float& dist_out, int& n_out);
    void stick_into_victim(::REManagedObject* vctx, ::REManagedObject* vtf,
                           const glm::vec3* pwp, const glm::quat* pwr,
                           ::REManagedObject* pktf, const std::string& pkname,
                           std::optional<float> pkdist, int pkcount,
                           const glm::vec3* pcontact, std::optional<float> plen);
    // [HUHN 2026-09-11] Ein GmChicken hat KEIN get_HitPoint und steht nicht
    // in der EnemyContextList -- beide Schadenswege des Stichs laufen dort
    // leer. Der Messerwurf trifft es trotzdem, weil das Homing fuer Tiere
    // einen eigenen Weg hat (requestAttack + DamageUserData). Genau den
    // holen wir uns hier.
    bool chicken_damage();

    void stick_return();

    // [STICK-KLON] Erzeugt die Anzeige-Kopie und haengt sie an den Gegner.
    // Liefert false, wenn irgendetwas fehlt -- dann faellt stick_into_victim
    // auf den alten Weg (echtes Messer umhaengen) zurueck.
    bool stick_clone_make(::REManagedObject* ktf, ::REManagedObject* htf,
                          const std::string& joint, const glm::vec3& lp,
                          const std::optional<glm::quat>& lr);
    void stick_clone_drop();
    void stick_clone_tick();
    void stick_hide_real(::REManagedObject* ktf, bool hide);
    // [HUHN-DREHUNG 12.09.2026] Beide Schreibstellen der Pose gehen hier
    // durch: Gegner unveraendert, Huhn plus die drei Korrekturwinkel.
    glm::quat held_rot(const glm::quat& base) const;

    // Abdunkeln
    bool dim_build(::REManagedObject* mesh);
    void dim_apply(bool dark);
    void dim_start(::REManagedObject* ktf);

    // ------------------------------------------------------------------
    // Griff
    // ------------------------------------------------------------------
    bool held_usable();
    void torso_capture(::REManagedObject* etf);
    void torso_enforce();
    void apply_choke_pose();

    // [ASHLEY MUND 16.09.2026] Transform ihres Gesichts-Rigs (Kind "head") und
    // der Mund-Tick, der die gemessenen Posen nach der Huellkurve blendet.
    ::REManagedObject* face_transform_of_held();

    // [MUND OHNE GRIFF 16.09.2026] Ihr Gesicht ueber die PartnerContextList
    // statt ueber m_held -- damit laeuft der Mund auch dann weiter, wenn der
    // Griff vor dem Ende des Spruchs zu Ende ist.
    ::REManagedObject* ashley_face_transform();

    // [ASHLEY ANTWORTET 16.09.2026] Ihr PartnerContext (fuer die Position) und
    // die Antwort selbst. fuck = Stinkefinger, sonst Zeigefinger. Gerufen wird
    // sie von RE4VRGuestures, sobald eine Geste zuendet.
    ::REManagedObject* ashley_partner_ctx();

public:
    void ashley_reply(bool fuck);

private:
    // Spielt die wartende Antwort, sobald ihre Zeit da ist (aus on_frame).
    void reply_tick();

public:

private:
    void mouth_tick();

    // Laeuft gerade einer ihrer Sprueche? Index und Startzeit.
    // [ASHLEY ANTWORTET 16.09.2026] Welcher Pool gerade den Mund fuehrt:
    // 0 = ihre Choke-Sprueche (ASHLEYnn), 1 = Antwort auf den Zeigefinger
    // (ASHLEYPOINTnn), 2 = Antwort auf den Stinkefinger (ASHLEYFUCKnn). Der
    // Resource-Name und damit die Huellkurve haengen daran.
    int    m_mouth_kind{0};

    int    m_mouth_wav{-1};
    double m_mouth_t0{0.0};

    // Huellkurve je WAV-Index, beim ersten Abspielen aus der Resource gerechnet.
    std::unordered_map<int, std::vector<uint8_t>> m_mouth_env{};

    void apply_hold();
    void hold_last();
    float clamp_hold_y(float y);
    void neck_measure();
    void joints_tick();

    ::REManagedObject* pick_target(const glm::vec3& hp);

    // [TIER 2026-09-10/12] Zweite Kandidatenquelle. Tiere stehen NICHT in der
    // EnemyContextList -- sie sind Gimmick-Objekte und werden ueber die Szene
    // gefunden. Gegriffen werden GmChicken und GmMouse; Kraehe (GmCrow) und
    // Fledermaus (GmBat) bleiben draussen, die fliegen.
    // chicken_out meldet, ob es ein Huhn wurde -- s. Definition.
    // [ART 13.09.2026] Welche Tierart haengt in der Hand.
    enum Species : int {
        SPECIES_NONE = 0,   // Gegner, Huhn, nichts
        SPECIES_MOUSE = 1,
        SPECIES_CROW = 2,
        SPECIES_BAT = 3,
    };

    // [ART 13.09.2026] Die sechs Regler des gehaltenen Tieres. Kein Tier oder
    // Huhn -> alles 0, also exakt das Verhalten ohne Regler.
    struct AnimalTune {
        float yaw{0.0f};
        float pitch{0.0f};
        float roll{0.0f};
        float ox{0.0f};
        float oy{0.0f};
        float oz{0.0f};
    };

    AnimalTune tune_of_held() const;

    // [HANDDREHUNG JE ART 13.09.2026] Welche drei Winkel die LINKE HAND
    // gerade bekommt. Dieselbe Auswahl wie tune_of_held, nur fuer die Hand --
    // und anders als frueher nicht mehr auf Tiere beschraenkt.
    struct HandTune {
        float yaw{0.0f};
        float pitch{0.0f};
        float roll{0.0f};

        bool any() const { return yaw != 0.0f || pitch != 0.0f || roll != 0.0f; }
    };

    HandTune hand_tune_of_held() const;

    // [EIN BEZUG FUER ALLES 14.09.2026] Yaw der HAND (L_Palm, waagerechteste
    // Achse). Drehung (held_rot) und Versatz (apply_hold) rechnen beide damit.
    std::optional<float> hand_basis_yaw() const;

    // [TUNE-PIN 13.09.2026] Gilt fuer das, was gerade in der Hand haengt, der
    // Dauergriff? Dieselbe Auswahl wie tune_of_held.
    bool tune_pin_of_held() const;

    ::REManagedObject* pick_animal(const glm::vec3& hp, float max_d, int* species_out,
                                   bool* chicken_out);

    // [ASHLEY 13.09.2026] Die Begleitung als Ziel -- nur wenn das Achievement
    // schon freigeschaltet ist (achievement_unlocked). Sie steht NICHT in der
    // EnemyContextList, sondern allein in der PartnerContextList.
    ::REManagedObject* pick_ashley(const glm::vec3& hp);

    // Ist "WHAT A BAT JOKE" freigeschaltet? Liest die JSON EINMAL pro
    // Spielstart -- danach steht die Antwort in m_achievement_seen.
    bool achievement_unlocked();

    // [TIER-MITTE 2026-09-12] Mittelpunkt der Welt-AABB des Tier-Meshes.
    // Damit findet der Steckpunkt bei JEDEM Tier die Koerpermitte, ohne dass
    // wir die Joint-Namen der einzelnen Arten kennen muessen (ein "Hip" ist
    // bei Huhn, Maus, Kraehe und Fledermaus nicht garantiert gleich benannt
    // -- oder ueberhaupt vorhanden).
    std::optional<glm::vec3> animal_center(::REManagedObject* htf);

    // [TIER-OBERFLAECHE 14.09.2026] Dieselbe Welt-AABB wie animal_center, aber
    // min und max statt nur der Mitte -- gebraucht, um die Klingenspitze auf
    // die HAUT zu setzen statt in die Koerpermitte. Bewusst eine eigene
    // Funktion: animal_center bleibt unangetastet, das Messer-Homing haengt
    // daran.
    bool animal_aabb(::REManagedObject* htf, glm::vec3& mn, glm::vec3& mx);
    void grab(::REManagedObject* ectx);
    void release();

    // [FLIP-NACHZUG] s. Definition -- haelt __vr_knife_flip im Zugriffsframe
    // gegen re4_vr_binding.lua, das seit dem Port NACH uns laeuft.
    void flip_carry();

    void rh_track();
    bool swing_towards_victim();

    // ------------------------------------------------------------------
    // Zustand
    // ------------------------------------------------------------------
    Cfg m_cfg{};
    bool m_cfg_loaded{false};

    // [KAPUTTE LOCALS -- s. Spec Abschnitt 2] Reine Laufzeitwerte OHNE
    // Config-Anbindung, weil load_cfg/save_cfg in Lua auf das GLOBAL greifen
    // und der uebrige Code auf das (spaeter deklarierte) local.
    bool m_stop_enemy_sounds{true};

    bool m_prev_grip{false};
    double m_last_release{0.0};

    // [TAUNT-WAV 2026-09-12] Eigener Shuffle-Beutel fuer die eingebetteten
    // Sprach-WAVs beim TIERgriff. Bewusst NICHT geteilt mit der
    // Mittelfinger-Geste in RE4VRGuestures: beide sollen eine eigene
    // Reihenfolge laufen (Ansage 12.09.2026).
    std::vector<int> m_taunt_wav_bag{};

    // [KEINE WIEDERHOLUNG 13.09.2026] Zuletzt gespielter Spruch -- nur dafuer da,
    // dass an der Beutelgrenze nicht zweimal derselbe kommt.
    int m_taunt_wav_last{-1};

    // [TAUNT-DELAY 2026-09-12] Zeitstempel, wann der beim Griff ausgewuerfelte
    // Spruch faellig wird. 0 = nichts steht an. BEWUSST der Zeitstempel und
    // NICHT der Restbetrag: der Tick laeuft am Frame, ein heruntergezaehlter
    // Rest haengt damit an der Framerate.
    double m_taunt_due{0.0};

    // Der Wurf faellt beim GRIFF, nicht beim Abspielen -- sonst zoege ein
    // spaeterer Griff waehrend der Wartezeit den Beutel zweimal.
    int m_taunt_pending_index{-1};

    // ========================================================================
    // [ASHLEY-SPRUECHE 13.09.2026] Eigener Beutel ueber ihre acht WAVs. Sie
    // laeuft ueber DIESELBE Wartezeit wie der Tiergriff (m_taunt_due und der
    // Regler taunt_delay_s) -- m_taunt_pending_ashley entscheidet beim
    // Faelligwerden nur, aus welchem Namensraum gespielt wird.
    // ========================================================================
    std::vector<int> m_ashley_wav_bag{};
    int m_ashley_wav_last{-1};

    // [ASHLEY ANTWORTET 16.09.2026] Je Geste ein EIGENER Beutel, sonst zoege
    // der Zeigefinger die Sprueche des Stinkefingers mit ab.
    std::vector<int> m_reply_bag_point{};
    std::vector<int> m_reply_bag_fuck{};
    int m_reply_last_point{-1};
    int m_reply_last_fuck{-1};

    // [NICHT JEDES MAL 16.09.2026 -- Ansage des Users] Wie bei ihren
    // Choke-Spruechen antwortet sie erst beim 4. bis 6. Mal, sonst ist der
    // Vorrat in zwei Minuten durch. Je Geste ein eigener Zaehler, passend zu
    // den getrennten Poels. Start bei 1: die allererste Geste bekommt eine
    // Antwort, damit man hoert, dass es geht.
    int m_reply_grabs_point{0};
    int m_reply_grabs_fuck{0};
    int m_reply_next_point{1};
    int m_reply_next_fuck{1};

    // [WARTEZEIT 16.09.2026] Die Antwort wird beim Zuenden AUSGEWUERFELT und
    // erst spaeter gespielt -- sonst faellt sie dem eigenen Taunt ins Wort.
    // Bauform wie m_taunt_due: Zeitstempel, nicht heruntergezaehlter Rest.
    // Der Pegel steht schon fest, er wurde am Abstand zur Geste gemessen.
    double m_reply_due{0.0};
    int    m_reply_pending{-1};
    bool   m_reply_pending_fuck{false};
    float  m_reply_pending_db{0.0f};
    bool m_taunt_pending_ashley{false};

    // [NICHT JEDES MAL 13.09.2026, Takt 16.09.2026 auf "alle 4-6 mal"] Sonst
    // ist der Vorrat nach acht Griffen durch und es wirkt gehetzt. Gezaehlt
    // werden die Griffe an ihr; m_ashley_next_at ist die Zahl, bei der unser
    // eigener WAV-Spruch wieder dran ist (danach neu ausgewuerfelt, 4 bis 6).
    // Start bei 1: der allererste Griff spricht, damit man hoert, dass es geht.
    // Dazwischen bleibt sie NICHT stumm -- dann laeuft eine ihrer eigenen
    // Spielzeilen aus ASHLEY_CHOKE_LINES2 (s. m_ashley_line_q).
    int m_ashley_grabs{0};
    int m_ashley_next_at{1};

    // [ASHLEY-LINE 16.09.2026] Ihre Sprachzeilen AUS DEM SPIEL, auf ihrem
    // eigenen SoundContainer. Warteschlange statt eines einzelnen Zeitpunkts,
    // weil pro Griff bis zu zwei Zeilen anstehen (die feste und die Fuellzeile).
    // Das Handle haelt ihr BodyGameObject, damit die Zeilen auch dann noch
    // kommen, wenn der Griff inzwischen los ist -- die Wartezeit haengt am
    // GRIFF, genau wie beim Taunt.
    struct AshleyLine {
        double at{0.0};
        uint32_t id{0};
    };

    Handle m_ashley_line_go{};
    std::vector<AshleyLine> m_ashley_line_q{};

    // Shuffle-Beutel ueber ASHLEY_CHOKE_LINES2 (Indizes, nicht IDs) --
    // m_ashley_line_last verhindert an der Beutelgrenze die Wiederholung.
    std::vector<int> m_ashley_line_bag{};
    int m_ashley_line_last{-1};

    // ========================================================================
    // [ACHIEVEMENT 13.09.2026] "WHAT A BAT JOKE" -- die Tafel zur ERSTEN
    // gegriffenen Fledermaus. Gemerkt wird das in einer eigenen JSON
    // (re4_vr/re4_vr_achievement.json): fehlt der Schluessel, steht sie noch aus.
    // Eigene Datei, weil re4_vr_splash.json beim Schreiben ganz ueberschrieben
    // wird (RE4VRMenu::splash_tick) -- ein zweiter Schluessel darin waere weg.
    // ========================================================================
    // Faelligkeit der Tafel (Zeitstempel wie m_taunt_due, 0 = nichts steht an).
    double m_achievement_due{0.0};
    // Schon einmal gezeigt? Nur EINMAL pro Spielstart von der Platte gelesen.
    bool m_achievement_seen{false};
    bool m_achievement_checked{false};
    // [TEST 13.09.2026] Merker fuer die einmalige Testanzeige beim Start.
    bool m_achievement_test_armed{false};

    // Erste gegriffene Fledermaus: merken und die Tafel anstellen.
    void achievement_arm();

    // [ASHLEY-MESSER 15.09.2026] Ein Stich in die gehaltene Begleitung. Zaehlt
    // ueber Spielstarts hinweg (re4_vr_achievement2.json) und stellt beim
    // dritten die zweite Tafel an.
    void ashley_stab();

    // Steht das zweite Achievement (drittes Messer in Ashley)?
    bool reply_unlocked();

    // -1 = noch nicht aus der Datei gelesen.
    int32_t m_ashley_stabs{-1};

    bool m_snake_checked{false};
    bool m_snake_unlocked{false};

    // Welche Tafel als naechstes gezeigt wird: 1 = Fledermaus, 2 = drei Messer.
    int32_t m_achievement_which{1};
    double m_last_parry{-99.0};
    std::optional<uintptr_t> m_parry_victim{};

    struct Held {
        Handle ctx{};
        Handle tf{};
        double t0{0.0};
        Handle mo{};
        std::optional<int32_t> rm_was{};
        std::optional<bool> mo_was{};   // wird nie gesetzt -- s. FREEZE_MOTION
        bool sw_prev{false};
        std::optional<float> yaw{};
        std::optional<glm::vec3> last_target{};
        std::optional<glm::vec3> neck_off{};
        std::optional<float> dy0{};
        std::optional<float> py0{};
        std::optional<float> hold_base{};
        std::optional<bool> knife_was{};

        // [TIER-DREHUNG 12.09.2026] Hat das gehaltene Tier KEIN Neck_1, haengt
        // es an der Huefte und liegt anders in der Hand -- nur dann greifen die
        // drei Tier-Regler. Das Merkmal statt einer Artenliste: ein kuenftiges
        // Rig ohne Hals ist damit automatisch mit dabei.
        bool no_neck{false};

        // [ART 13.09.2026] Welches Tier haengt gerade in der Hand -- entscheidet,
        // welcher der drei Reglersaetze gilt. 0 = Huhn/kein Tier, 1 = Maus,
        // 2 = Kraehe, 3 = Fledermaus.
        int species{0};

        // [LAGE AM JOINT 13.09.2026] Der Joint, an dem dieses Opfer haengt --
        // bei den necklosen Tieren die Huefte. Er ist der Bezug, gegen den die
        // Lage ausgerichtet wird; ohne gemerkten Namen muesste jeder Frame die
        // ganze Kette neu durchsucht werden.
        std::string grab_joint{};

        // [BAREHANDS 2026-09-12] Waren die Haende beim Zupacken LEER? Dann ist
        // der Rueckwechsel auf die Hauptwaffe falsch -- gehoert wieder leer.
        // knife_was allein sagt das NICHT: es ist bei leeren Haenden genauso
        // false wie mit gezogener Pistole.
        std::optional<bool> bare_was{};

        struct IkEntry {
            Handle c{};
            std::optional<bool> was{};
        };
        std::vector<IkEntry> ik{};
        bool ik_set{false};

        // Gehaltenes Tier statt Gegner: dann gibt es weder ActionState noch
        // Grapple-Flags, und das Body-GameObject ist das Objekt selbst.
        // [TIER 2026-09-12] ZWEI Flags, mit Absicht:
        //  * is_animal -> es ist ein Gimmick-TIER (Huhn oder Maus). Daran haengt
        //    alles, was Tiere von Gegnern unterscheidet: get_IsDead statt
        //    IsEliminated, chicken_damage, get_GameObject statt
        //    get_BodyGameObject, keine Grapple-Setter, die Tier-Haltezeit.
        //  * is_chicken -> es ist ein HUHN. Haengt nur noch an held_rot: die drei
        //    Korrekturwinkel sind am Huhn gemessen und wuerden jede andere
        //    Tierpose verdrehen.
        // Bei einem Huhn sind BEIDE true -- der Huhn-Pfad bleibt damit genau so,
        // wie er vor dem Maus-Umbau war.
        bool is_animal{false};
        bool is_chicken{false};

        // [ADA-MAUS 17.09.2026] Beim Zupacken war Ada der Spieler (Koerper
        // ch3a8z0_body). Waehlt fuer die Maus Adas eigenen Reglersatz.
        bool ada_player{false};

        // [ASHLEY 13.09.2026] Gehalten wird die BEGLEITUNG, nicht ein Gegner.
        // Ihr Kontext (chainsaw.PartnerBaseContext) kann fast alles, was der
        // Griff braucht -- gemessen am 13.09.: get_Position,
        // get_BodyGameObject, get_HitPoint, get_Valid, get_IsEliminated sind
        // da, Rig mit Neck_1/Spine_2/L_Palm, Motion + IkLeg2 + GroundAdsorber.
        // NICHT da sind get_ActionState und die drei Grapple-Setter -- deshalb
        // ueberspringt der Griff sie bei ihr.
        bool is_ashley{false};

        // [FLEDERMAUS-FAHRER 13.09.2026] Ihre Flugbahn kommt aus
        // chainsaw.SequenceTrackUpdater (gemessen mit
        // zzz_re4_fledermaus_check.lua: enabled=true, dazu MotionFsm2). Solange
        // sie in der Hand haengt, sind beide aus -- sonst schreibt die Bahn
        // ihre Position NACH uns, und sie fliegt einfach weiter (8 m in zwei
        // Sekunden, im Griff-Log belegt). Beim Loslassen kommt der alte Zustand
        // zurueck, damit sie normal weiterfliegt.
        // Liste, weil mehrere Komponenten an ihrer Bewegung haengen -- welche
        // genau, steht in BAT_DRIVERS (RE4VRChoke.cpp).
        std::vector<IkEntry> drivers{};

        // [ZURUECK AUF DIE BAHN 14.09.2026 -- Ansage "danach kriecht sie am
        // Boden herum"] Wo die Fledermaus stand, als wir sie gegriffen haben.
        // Ihre Flugbahn (SequenceTrackUpdater) steht waehrend des Griffs still;
        // laesst man sie irgendwo anders los, passt die Bahn nicht mehr zur
        // Lage und sie krabbelt am Boden. Deshalb kommt sie beim Loslassen
        // zuerst an ihren Ausgangspunkt zurueck, DANN laufen ihre Komponenten
        // wieder an.
        std::optional<glm::vec3> bat_home_pos{};
        std::optional<glm::quat> bat_home_rot{};

        struct TorsoEntry {
            Handle j{};
            glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
            std::optional<glm::vec3> p{};
        };
        std::vector<TorsoEntry> torso{};
        bool torso_set{false};
    } m_held{};

    // Steckendes Messer
    struct Stick {
        Handle tf{};
        Handle parent{};
        std::string joint{};
        double until_t{0.0};
        Handle vic_tf{};
        Handle vic_ctx{};
        Handle mo{};
        std::optional<int32_t> rm_was{};
        std::optional<glm::vec3> root0{};
        std::optional<double> check_at{};
        bool parented{false};
        std::string joint_used{};

        // [RUECKBAU-POSE 2026-09-10 -- Testerbefund "Messer haengt neben dem
        // Gesicht"] Gemerkt wurden bisher nur Parent und Joint-NAME; beim
        // Zurueckholen setzte stick_return() deshalb pauschal
        // LocalPosition(0,0,0) und schrieb gar keine Rotation. War der gemerkte
        // Parent der GEGNER, blieb das Messer im Ursprung dieses Joints stehen
        // -- auf Kieferhoehe neben dem Kopf (Messung 23:03, wp5801:
        // lokal=0/0/0, lokrot=Identitaet, 0.40 m vom Jaw). Dasselbe Muster wie
        // rest_lp/rest_lr beim Magazin im Reload.
        std::optional<glm::vec3> rest_lp{};
        std::optional<glm::quat> rest_lr{};

        // [STICK-KLON 2026-09-10] Gesteckt wird eine reine Anzeige-KOPIE, nicht
        // mehr die echte Waffe: dann kann sie im Gegner bleiben, waehrend das
        // Original ganz normal in die Hand zurueckkehrt. Bewusst ein EIGENER
        // Klon -- der Klon des linken Messers (RE4VRWeapons2 "vr_lh_knife")
        // wird NICHT mitbenutzt, sonst nimmt ihm der Choke das linke Messer weg.
        Handle clone{};
        Handle clone_mesh{};
        Handle clone_tf{};
        double clone_until{0.0};

        // Der Gegner, in dem die Kopie steckt -- EIGENER Griff, nicht vic_ctx:
        // der gehoert dem Griff und wird mit ihm ungueltig, die Kopie lebt
        // laenger. Ueber ihn wird der Tod erkannt.
        Handle clone_vic{};

        // Laeuft dieser Stich ueber die Kopie? Dann darf der Herzschlag
        // __re4_choke_knife_stuck NICHT gesetzt werden -- die echte Waffe
        // haengt in der Hand und motion soll sie ganz normal fuehren.
        bool clone_way{false};

        // [GRAPPLE-FLAGS 2026-09-10] Beim Loslassen wurden
        // IsConstOnGrapple und IgnoreTerrainCorrectOnGrapple SOFORT
        // zurueckgestellt -- die Gelaende-Korrektur war damit wieder
        // scharf, waehrend der Koerper noch an der Stelle stand, an die
        // WIR ihn gezogen hatten. Gemessen 23:35: 0.45 s spaeter ein rein
        // horizontaler Ein-Frame-Sprung von 2.53 m bei y=konstant.
        // Jetzt warten sie bis zum Ende des Nachlaufs -- dasselbe Muster,
        // das die Root Motion seit 24.08. schon faehrt.
        bool grapple_pending{false};

        // Das echte Messer wird fuer die Steckdauer nur unsichtbar gemacht
        // (LocalScale 0, wie das Magazin im Reload) -- sonst haelt Leon es
        // sichtbar in der Hand, waehrend die Kopie im Gegner steckt.
        Handle real_tf{};
        bool real_hidden{false};
    } m_stick{};

    // Abdunkeln
    struct DimVar4 {
        int mi{0};
        int vi{0};
        glm::vec4 v{};
    };
    struct DimVar1 {
        int mi{0};
        int vi{0};
        float orig{0.0f};
    };
    struct Dim {
        Handle mesh{};
        std::vector<DimVar4> cols{};
        std::vector<DimVar1> zeros{};
        bool on{false};
        bool built{false};
    } m_dim{};

    // Laufender Stich, dessen Wucht noch nicht feststeht
    struct Pend {
        double at{0.0};
        float v0{0.0f};
        Handle ctx{};
        Handle tf{};
        std::optional<glm::vec3> wp{};
        std::optional<glm::quat> wr{};
        Handle ktf{};
        std::string kname{};
        std::optional<float> kdist{};
        int kcount{0};
    } m_pend{};

    // Die letzten Stiche zum Ablesen im Tree (max. 6, aeltester vorn)
    struct VLog {
        float v0{0.0f};
        float pk{0.0f};
        bool st{false};
    };
    std::vector<VLog> m_vlog{};
    void vlog_add(float v0, float pk, bool stuck);

    // Spitze des letzten Frames
    struct TipLast {
        std::optional<glm::vec3> p{};
        double t{0.0};
        std::string nm{};
        bool has_nm{false};
    } m_tip_last{};
    double m_tip_hit_t{-999.0};

    // [CRASH-SPUR] Wievielter nativer Treffer auf DENSELBEN Gegner
    ::REManagedObject* m_nat_last_ctx{nullptr};
    int m_nat_n{0};

    // Klingenlaengen je GO-Name (Minimum ueber die Frames)
    std::unordered_map<std::string, float> m_blade_len_seen{};

    // Handhistorie der rechten Hand (Ring, 12 Plaetze)
    struct RhEntry {
        glm::vec3 p{};
        double t{0.0};
        bool set{false};
    };
    std::array<RhEntry, 12> m_rh_ring{};
    int m_rh_ri{0};

    // Verzoegerter Gegnerlaut
    struct SndRetry {
        Handle ego{};
        uint32_t id{0};
        std::vector<double> at{};
    } m_snd_retry{};

    // Rueckwechsel
    double m_reequip_at{0.0};
    double m_reequip_until{0.0};
    // [STAGGER-FENSTER 13.09.2026] Harte Obergrenze fuers Nachziehen von
    // m_reequip_until, gesetzt beim Loslassen.
    double m_reequip_dead{0.0};
    // [ABSTURZ BEIM LADEN 14.09.2026] Seit wann am Stueck auf einen
    // Gameplay-Frame gewartet wird. Laenger als eine halbe Sekunde heisst:
    // Laden oder Tod -- dann wird der Wechsel verworfen.
    double m_reequip_wait_seit{0.0};

    // [HAND-BEZUG 14.09.2026 -- Ansage "immer gleich. IMMER"] Welche Achse des
    // L_Palm-Joints als Blickrichtung der Hand gilt. EINMAL pro Spielstart
    // bestimmt (die waagerechteste beim ersten Griff) und danach nie wieder --
    // so kann die Wahl zwischen zwei Griffen nicht kippen. Bewusst KEIN
    // Mitglied von m_held: das wird bei jedem Loslassen geleert.
    // [SWING-TWIST 14.09.2026] Die frueheren Merker m_palm_axis / m_palm_sign /
    // m_pose_done sind ersatzlos weg: hand_basis_yaw waehlt keine Achse mehr,
    // es gibt also nichts mehr zu merken. Siehe die Begruendung dort.

    // [BAREHANDS 2026-09-12] Soll der faellige Rueckwechsel auf LEERE HAENDE
    // gehen statt auf die Hauptwaffe? Eigenes Member, weil m_held beim
    // Loslassen geleert wird, der Wechsel aber erst danach faellig ist.
    bool m_reequip_bare{false};

    // Flacker-Messung je Phase


    // Damage-Kategorie (einmalige TDB-Suche)
    std::optional<int32_t> m_cat_damage{};
    bool m_cat_tried{false};

    // via.motion.RootPlayMode.None -- einmalig
    std::optional<int32_t> m_root_none{};
    // chainsaw.EquipType.Main -- einmalig
    std::optional<int32_t> m_et_main{};

    // Typen
    ::REManagedObject* m_t_motion{nullptr};
    ::REManagedObject* m_t_sndc{nullptr};
    ::REManagedObject* m_t_mesh{nullptr};
    std::array<::REManagedObject*, 3> m_t_ik{nullptr, nullptr, nullptr};
    bool m_types_ready{false};
    void ensure_types();

    // [CHOKE-RUHE] Der Block am Dateiende. Steht drin, zuendet aber nicht --
    // _G.__re4_choke_ruhe wird beim Laden auf false gesetzt.
    void choke_ruhe_tick();
    bool m_ruhe_aktiv{false};
    Handle m_ruhe_hc{};
    std::optional<bool> m_ruhe_hc_was{};
    double m_ruhe_seit{0.0};
};

#endif // RE4
