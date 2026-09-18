// ============================================================================
// RE4VRWhitelist -- 1:1-Portierung von re4_vr_whitelist.lua (368 Zeilen).
//
// "DIE BIBEL": die gemeinsame Breakable-Whitelist fuers Messer. Was hier
// drinsteht, darf das Messer (a) per Melee/Wurf ZERSTOEREN und (b) per
// Wurf-HOMING ANPEILEN. Alles ausserhalb wird von BEIDEN ignoriert -- Leitern
// und sonstige HitController-Props fallen raus. Gegner laufen SEPARAT
// (EnemyContextList) und sind hier unberuehrt.
//
// Kern-Breakables (WoodBox, Durability, lebende Tiere GmAnimal) sind in
// weapons.lua fest verdrahtet und immer Ziel -- die stehen bewusst NICHT hier.
//
// Kein Callback, kein Hook, kein UI: die Datei legt Daten und vier Funktionen
// nach _G und ist fertig. Alle Konsumenten sind Lua und rufen sie INLINE auf,
// weil sie selbst am 200-Local-Limit sitzen.
//
// Spezifikation: I:\LUATRANS\PORT_WHITELIST_SPEC.md
// ============================================================================

#pragma once

#if defined(RE4)

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "RE4VR.hpp"

class RE4VRWhitelist : public Mod {
public:
    static std::shared_ptr<RE4VRWhitelist>& get();

    std::string_view get_name() const override { return "RE4VRWhitelist"; }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    // Die vier nach Lua exportierten Funktionen.
    bool is_real_breakable_prop(::REManagedObject* go);
    float breakable_yoff(::REManagedObject* go);
    float enemy_off_y(::REManagedObject* ctx);
    std::tuple<float, float, float> animal_center(::REManagedObject* anim, float fx, float fy,
                                                  float fz);

private:
    void resolve_types();
    void load_capture();
    void publish(sol::state& lua);

    // sdk.typeof-Ergebnisse. Werden bei JEDEM Laden frisch geholt.
    std::vector<::REManagedObject*> m_tds{};
    std::vector<bool> m_tds_reffed{};
    void clear_types();

    // Die aus dem Capture zusammengetragenen Staemme. Beim Laden LEER -- die
    // Liste kommt ausschliesslich aus der JSON und den beiden Alt-TXT.
    std::vector<std::string> m_name_prefixes{};

    // [M3] Zur Laufzeit aus der LUA-Tabelle gelesen, damit der Live-Append des
    // Capture-Tools sofort greift. Puffer, damit pro Aufruf nichts alloziert
    // bleibt.
    const std::vector<std::string>& live_prefixes();
    std::vector<std::string> m_live_prefixes{};
};

#endif // RE4
