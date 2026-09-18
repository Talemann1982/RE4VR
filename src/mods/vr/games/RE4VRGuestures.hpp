// ============================================================================
// RE4VRGuestures -- 1:1-Portierung von re4_vr_guestures.lua (519 Zeilen).
//
// Hand-Gesten der RECHTEN Hand, Port des RE9-Musters. Vier Bausteine:
//   (1) Zwei Posen  point (LT+R.A) / fuck_you (LT+R.B) mit Blend-Kurve
//   (2) Stagger     Stinkefinger GEHALTEN staggert alle Gegner ringsum
//   (3) Taunt       je Geste ein zufaelliger Spruch, Shuffle-Bag je Charakter
//   (4) UI          Fingerkruemmung je Pose, Vorschau-Modus
//
// Ausgeloest wird von re4_vr_binding.lua (setzt __re4_gesture_fire genau einmal
// pro Tastendruck). Geschrieben wird ueber __re4_reload_apply_pose_bones aus
// reload.lua -- KEIN eigener Joint-Code, kein Konflikt mit POSES.
//
// Spezifikation: I:\LUATRANS\PORT_GUESTURES_SPEC.md
//
// ----------------------------------------------------------------------------
// REIHENFOLGE IM MOD-VEKTOR -- zwischen RE4VRChoke und RE4VRMerc:
//
//   RE4VR -> RE4VRChoke -> **RE4VRGuestures** -> RE4VRMerc -> RE4VRMotion -> ...
//
// Alphabetisch gilt choke < guestures < merc < motion.
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

class RE4VRGuestures : public Mod {
public:
    static std::shared_ptr<RE4VRGuestures>& get();

    std::string_view get_name() const override { return "RE4VRGuestures"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

private:
    struct Handle {
        ::REManagedObject* obj{nullptr};
        bool reffed{false};
    };

    // `unconditional` = Luas `pin(o)`, das add_ref BEDINGUNGSLOS ruft. Ein
    // frisch per create_instance erzeugtes Objekt kann refcount 0 haben -- die
    // uebliche refcount-Heuristik wuerde es dann NICHT verankern, und genau das
    // ist Falle 1 (der GC raeumt es ab, der zweite Burst greift in
    // freigegebenen Speicher). Dieselbe Unterscheidung wie in RE4VRHolster.
    void store(Handle& h, ::REManagedObject* o, bool unconditional = false);
    void drop(Handle& h);

    // ------------------------------------------------------------------
    // Posen -- intern in GRAD, damit die UI-Slider direkt darauf arbeiten.
    // Beim Anwenden und Speichern wird daraus wieder ein Quaternion gebaut.
    // ------------------------------------------------------------------
    using PoseBones = std::unordered_map<std::string, glm::quat>;

    // DEG[pose][joint] = Grad (Beugung je Glied)
    // ROT[pose][finger] = Grad (Drehung des ganzen Fingers)
    // POSES[pose] = fertige Quaternionen, aus DEG/ROT gebaut
    std::unordered_map<std::string, std::unordered_map<std::string, float>> m_deg{};
    std::unordered_map<std::string, std::unordered_map<std::string, float>> m_rot{};
    std::unordered_map<std::string, PoseBones> m_poses{};
    bool m_loaded{false};

    void load_poses();
    void save_poses();
    void rebuild(const std::string& name);

    // ------------------------------------------------------------------
    // Ablauf
    // ------------------------------------------------------------------
    bool hands_free();
    void start(const std::string& name);
    std::optional<float> blend_now();
    void apply();

    // ------------------------------------------------------------------
    // (2) Stagger
    // ------------------------------------------------------------------
    ::REManagedObject* flash_ud();
    ::REManagedObject* flash_table();
    ::REManagedObject* dmg_ud();
    bool is_own_table(::REManagedObject* t);
    void fire_stagger();

    // ------------------------------------------------------------------
    // (3) Taunt
    // ------------------------------------------------------------------
    // [TAUNT-WAV 2026-09-12] Ein Eintrag im Spruch-Pool ist jetzt entweder ein
    // Wwise-Spielsound ODER ein eingebettetes eigenes WAV. Beide liegen im
    // GLEICHEN Pool und damit im GLEICHEN Shuffle-Beutel -- nur so mischen sich
    // die bestehenden Sprueche und die eigenen Aufnahmen bei der Geste, statt
    // zwei getrennte Reihenfolgen zu laufen (Ansage 12.09.2026).
    //
    // In TAUNT_TABLE (RE4VRGuestures.cpp) traegt ein Eintrag dafuer "wav" statt "id".
    //
    // [SEPARIERT 13.09.2026 -- Ansage "ein paar fuer die mittelfingergeste und
    // ein paar fuer die zeigefingergeste, bitte separieren"] Ein Eintrag darf
    // zusaetzlich "gest" tragen: "fuck_you" (Mittelfinger) oder "point"
    // (Zeigefinger). Fehlt das Feld, gilt der Eintrag wie bisher fuer BEIDE
    // Gesten -- alle vorhandenen Wwise-Sprueche bleiben damit unangetastet.
    struct TauntEntry {
        uint32_t    id{0};        // Wwise-ID -- gilt, wenn wav_index < 0 ist
        int         wav_index{-1};   // 0..re4vr::TAUNT_WAV_COUNT-1, sonst -1
        std::string gest{};       // leer = beide Gesten
    };

    void taunt_load();
    // Der Beutel haengt an BODY UND GESTE: jede Geste mischt ihre eigene
    // Reihenfolge, und ein auf die andere Geste festgelegter Eintrag kommt gar
    // nicht erst in den Beutel (frueher wurde er beim Ziehen uebersprungen und
    // war fuer die Runde verbraucht).
    std::optional<TauntEntry> taunt_next(const std::string& body,
                                         const std::string& gest);
    // `gest` ist der Name der gefeuerten Geste, also "fuck_you" oder "point".
    void play_taunt(const std::string& gest);

    // ------------------------------------------------------------------
    // Zustand
    // ------------------------------------------------------------------
    struct Active {
        const PoseBones* bones{nullptr};
        double t0{0.0};
    };

    std::optional<Active> m_active{};

    // Haltezustand der Geste. Wird bei JEDEM Abbruchgrund geleert -> kein
    // Nachzuenden nach einem Branch-Wechsel.
    struct Hold {
        double t0{0.0};
        bool fired{false};
    };

    std::optional<Hold> m_hold{};

    // Vorschau: haelt die Pose dauerhaft zum Tunen (UI), ignoriert die Zeitkurve
    std::optional<std::string> m_preview{};

    // Alles genau einmal gebaut und verankert -- selbst erzeugte Managed
    // Objects sind ohne add_ref nach dem ersten Einsatz weg.
    Handle m_made_ud{};
    Handle m_made_tbl{};
    Handle m_made_dmg{};
    // Die beiden Bestandteile der Tabelle brauchen ihren EIGENEN Anker: das
    // Array-Element bleibt null (set_element wirft), das Array haengt also nur
    // ueber `_AttackDataList` am ahu und die AttackData an gar nichts. In Lua
    // macht das `pin(ad); pin(arr)`.
    Handle m_made_ad{};
    Handle m_made_arr{};

    // Taunt-Pools je Body-GO-Name, dazu der Shuffle-Bag. Der BAG ist feiner
    // geschluesselt: "<body>|<geste>" (siehe taunt_next).
    std::unordered_map<std::string, std::vector<TauntEntry>> m_taunt_pools{};
    std::unordered_map<std::string, std::vector<TauntEntry>> m_taunt_bags{};
    bool m_taunt_loaded{false};
    bool m_taunt_seeded{false};

    ::REManagedObject* m_t_sndc{nullptr};
    bool m_types_ready{false};
    void ensure_types();
};

#endif // RE4
