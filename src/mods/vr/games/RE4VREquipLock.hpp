// ============================================================================
// RE4VREquipLock -- 1:1-Portierung von re4_vr_equip_lock.lua (59 Zeilen).
//
// Die ENGINE soll die Waffe nicht mehr selbst wechseln -- wir bestimmen, was in
// der Hand liegt. Anlass (im Watch-Log belegt, 14:09:18-19): waehrend eines
// Killswitch-Fensters (Treffer/Stagger) ruft das Spiel von sich aus
// requestChangeActiveWeapon -> execChangeWeapon -> equipWeapon und holt die
// vorher aktive Waffe zurueck -- der Spieler hatte gerade das Messer gezogen.
//
// BEWUSST ENG: geblockt wird NUR, wenn der Aufruf NICHT von uns kommt UND wir
// gerade NICHT im normalen Gameplay sind.
//
// Spezifikation: I:\LUATRANS\PORT_EQUIPLOCK_CAPACITIVE_SPEC.md (Teil A)
// ============================================================================

#pragma once

#if defined(RE4)

#include <optional>
#include <string>

#include "RE4VR.hpp"

class RE4VREquipLock : public Mod {
public:
    static std::shared_ptr<RE4VREquipLock>& get();

    std::string_view get_name() const override { return "RE4VREquipLock"; }

    std::optional<std::string> on_initialize() override;
    void on_lua_state_created(sol::state& lua) override;

    // Aus beiden Hooks gerufen. true = SKIP_ORIGINAL.
    bool blocked();

private:
    // Ausschalter des Originals. Dort ein Datei-Local, hier eine Konstante --
    // es gibt weder UI noch Config dafuer.
    static constexpr bool LOCK = true;

    // [DAUERBLOCK-BREMSE 2026-09-08] Der Lock ist fuer ein Fenster von
    // Sekundenbruchteilen gedacht (Treffer/Stagger). Blockt er laenger am
    // Stueck, ist das kein Stagger mehr -- dann laesst er los, statt einen
    // Zustand dauerhaft zu zerschiessen.
    static constexpr double MAX_BLOCK_SEC = 1.0;
    std::optional<double> m_block_since{};
};

#endif // RE4
