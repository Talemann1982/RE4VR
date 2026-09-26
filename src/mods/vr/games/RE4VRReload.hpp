// ============================================================================
// RE4VRReload -- TRAEGER des ganzen Reload-Blocks.
//
// In Lua sind das sechs Dateien, deren Callbacks die ENGINE in
// Registrierungsreihenfolge abarbeitet, und die ergibt sich aus der
// alphabetischen Ladereihenfolge des ScriptRunners:
//
//     reload  <  reload2  <  reload3  <  reload4_dlc  <  reload5_dlc  <  reload_adv
//
// (ASCII: '.' 46 < '2' 50 < '_' 95 -- reload_adv laeuft als LETZTES, obwohl es
// die Bibliothek ist. Das geht, weil die fuenf sie ausschliesslich zur LAUFZEIT
// ansprechen, nie zur Ladezeit.)
//
// Deshalb sind die sechs Teile hier KEINE eigenen Mods: dann entschiede die
// Reihenfolge im Mods-Vektor, und ein Umsortieren waere ein stiller
// Verhaltenswechsel. Stattdessen ruft dieser eine Traeger sie je Pass in genau
// dieser Kette -- damit ist die Reihenfolge im Code fixiert und lesbar.
//
// Spezifikation: I:\LUATRANS\PORT_RELOAD_STRATEGIE.md
// ============================================================================

#pragma once

#if defined(RE4)

#include <memory>
#include <string>

#include "RE4VR.hpp"
#include "RE4VRReloadAdv.hpp"
#include "RE4VRReloadMain.hpp"
#include "RE4VRReload2.hpp"
#include "RE4VRReload3.hpp"
#include "RE4VRReload4.hpp"
#include "RE4VRReload5.hpp"

class RE4VRReload : public Mod {
public:
    static std::shared_ptr<RE4VRReload>& get();

    std::string_view get_name() const override { return "RE4VRReload"; }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    // [MENUE-REIHENFOLGE 2026-09-07] Einzelzugriff auf die sechs Reload-Trees.
    // Sie sind Member dieses Traegers, keine eigenen Mods -- ohne diese
    // Weiterleitungen liessen sie sich nicht alphabetisch zwischen die uebrigen
    // Trees einsortieren, sondern nur als Block.
    void draw_dev_main() { m_main.draw_dev_ui(); }
    void draw_dev_r2()   { m_r2.draw_dev_ui(); }
    void draw_dev_r3()   { m_r3.draw_dev_ui(); }
    void draw_dev_r4()   { m_r4.draw_dev_ui(); }
    void draw_dev_r5()   { m_r5.draw_dev_ui(); }
    void draw_dev_adv()  { m_adv.draw_dev_ui(); }
    void draw_dev_kf()   { m_adv.draw_kf_ui(); }   // [KFH] "RE4VR - Keyframes"

    // Public-Block (Red9 Shell Ratio, order 61) -- liegt in RE4VRReload2.
    void draw_public_red9_ratio() { m_r2.draw_public_red9_ratio(); }

    // Public-Block (Shotgun Shell Ratio, order 60) -- liegt in RE4VRReloadMain.
    void draw_public_shotgun_ratio() { m_main.draw_public_shotgun_ratio(); }

    // Zugriff fuer die Geschwister-Module (weapons2 ruft z.B. den Mag-Slide).
    RE4VRReloadMain& main() { return m_main; }
    RE4VRReloadAdv& adv() { return m_adv; }
    RE4VRReload2& part2() { return m_r2; }
    RE4VRReload3& part3() { return m_r3; }
    RE4VRReload4& part4() { return m_r4; }
    RE4VRReload5& part5() { return m_r5; }

private:
    RE4VRReloadMain m_main{};
    RE4VRReload2 m_r2{};
    RE4VRReload3 m_r3{};
    RE4VRReload4 m_r4{};
    RE4VRReload5 m_r5{};
    RE4VRReloadAdv m_adv{};

    bool m_initialized{false};
};

#endif
