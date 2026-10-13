// ============================================================================
// RE4VRJiggle -- Portierung von re4_vr_body_physics.lua (19.09.2026).
//
// Die VR-Haende wirken auf Ashley:
//   * Kopf:   Hand-Tempo + Richtung -> Drall um den Hals (w = (r x v) / |r|^2),
//             dazu Wegdruecken nach Eindringtiefe; eine Feder zieht den Kopf
//             zurueck in die Animation. Geschrieben NACH der Animation
//             (UpdateJointExpression + BeginRendering pre, wie der Lipsync).
//   * Ton:    Klatscher beim Kopftreffer, danach ein Spruch von Ashley.
// [JIGGLE RAUS 02.10.2026] Stoff (GPUClothCharacter/GpuCloth), Ketten
// (via.motion.Chain) und Wind sind entfernt: die gemerkten Komponenten hingen
// an Kind-Objekten (z.B. Ashleys Lampe beim Raetsel) -- abgestellt = Objekt
// weg, Zeiger blieb -> AV in getGroupCount (Tester-Dump 02.10.).
//
// Bonus des dritten Achievements ("LOTS OF MOVEMENT"). Die Bedingung dafuer
// steht noch nicht fest -- bis dahin gilt es als freigeschaltet (Ansage).
// Schreibt KEINE Logs (goldene Regel).
// ============================================================================

#pragma once

#if defined(RE4)

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "RE4VR.hpp"

class RE4VRJiggle : public Mod {
public:
    static std::shared_ptr<RE4VRJiggle>& get();

    std::string_view get_name() const override { return "RE4VRJiggle"; }

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // Developer-Tree "RE4VR - Jiggle" (gezeichnet von RE4VRMenu).
    void draw_dev_ui();

private:
    struct Cfg {
        bool enabled{true};
        bool drive_head{true};

        // Kopf -- Werte vom User eingestellt (19.09.2026, Screenshot slap.png).
        float head_radius{0.14f};
        float head_gain{9.0f};
        float head_max_deg{45.0f};
        float head_drag{12.0f};
        float head_max_spin{15.0f};
        float head_stiffness{242.0f};
        float head_damping{13.0f};
        float head_neck_share{0.4f};
        // [SENSITIVITY 19.09.2026] Master: skaliert ALLES, was von der Hand
        // kommt (Eindringtiefe, Aufprall-Schwung, Mitfuehren) gleichmaessig --
        // die Verhaeltnisse der Einzelwerte oben bleiben dabei unveraendert.
        float head_sensitivity{1.0f};

        // Ton: Klatscher beim Kopftreffer, danach ein Spruch aus Ashleys
        // Mittelfinger-Pool. Lautstaerke = der globale dB-Regler.
        bool sounds{true};
        // [PEAK AM SLAP 19.09.2026] Hand-Tempo an dieser Schwelle = voller
        // Kopfausschlag (Head max) + Klatscher. Darunter anteilig schwaecher
        // und kein Klatscher. Ersetzt den frueheren "Head kick".
        float slap_min_speed{0.5f};   // m/s
        float reply_delay{0.8f};      // s nach dem LETZTEN Klatscher
        float slap_gain_db{0.0f};     // ZUSCHLAG auf den globalen dB-Regler, nur Klatscher
    } cfg{};

    // ---- Ashley --------------------------------------------------------
    ::REManagedObject* m_body{nullptr};
    uintptr_t m_body_addr{0};
    ::REManagedObject* m_head_j{nullptr};
    ::REManagedObject* m_neck_j{nullptr};

    // Findet Ashley und vergleicht die ADRESSE des Koerpers: Save-Load/Tod
    // liefern einen neuen Koerper, der alte bleibt lesbar -> alles verwerfen.
    void refresh_body();
    ::REManagedObject* resolve_body();
    void discover(::REManagedObject* body);
    void drop_all();

    // ---- Kopf (UpdateJointExpression + BeginRendering pre) ----------------
    glm::vec3 m_th{0.0f};   // Neigung als Achse*Winkel (Welt)
    glm::vec3 m_om{0.0f};   // Drehtempo (Welt, rad/s)
    double m_head_last_t{-1.0};
    std::optional<glm::vec3> m_head_prev[2]{};
    bool m_head_inzone[2]{false, false};
    bool m_contact_head{false};
    float m_head_angle{0.0f};

    struct JointWrite {
        std::optional<glm::quat> base{};
        std::optional<glm::quat> written{};
    } m_jw_head{}, m_jw_neck{};

    void head_step();
    void head_apply();
    void head_apply_joint(JointWrite& jw, ::REManagedObject* j, float share);

    static void hands(std::optional<glm::vec3>& lh, std::optional<glm::vec3>& rh);

    // ---- Ton ------------------------------------------------------------
    double m_slap_last_t{-99.0};
    int m_slap_last_idx{-1};
    double m_reply_due{0.0};
    std::vector<int> m_reply_bag{};
    int m_reply_last{-1};

    static bool choking();
    static bool hands_bare();
    double m_hold_until{0.0};
    // Spruch nach dem Schlag nur bei jedem 2. bis 3. Klaps.
    int m_slap_count{0};
    int m_slap_next{2};
    std::optional<glm::vec3> m_player_prev{};   // fuer das Tempo relativ zum Koerper
    void on_slap(float speed);
    void tick_reply();

    // ---- Config: reframework/data/re4_vr/re4_vr_jiggle.json ---------------
    // Die Werte oben sind nur die Vorgabe. Beim ersten Zugriff gelesen, danach
    // komplett zurueckgeschrieben; jede Regler-Aenderung wird gespeichert.
    bool m_cfg_loaded{false};
    bool m_cfg_dirty{false};
    void cfg_ensure();
    void cfg_save();

    // Anfangs-Drehtempo, mit dem der Kopf genau bis Head max schwingt.
    float peak_spin() const;
};

#endif // RE4
