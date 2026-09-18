// ============================================================================
// RE4VRReload -- Traeger des Reload-Blocks. Siehe RE4VRReload.hpp.
// ============================================================================

#if defined(RE4)

#include <sdk/RETypeDB.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRReload.hpp"

std::shared_ptr<RE4VRReload>& RE4VRReload::get() {
    static std::shared_ptr<RE4VRReload> inst{std::make_shared<RE4VRReload>()};

    return inst;
}

std::optional<std::string> RE4VRReload::on_initialize() {
    // Reihenfolge wie die Ladereihenfolge in Lua: reload zuerst, reload_adv
    // zuletzt. reload_adv laedt dabei seine eigene JSON und befuellt die
    // Keyframe-Saetze -- der Main-Teil fragt sie erst zur Laufzeit ab.
    m_main.on_initialize();
    m_r2.on_initialize();
    m_r3.on_initialize();
    m_r4.on_initialize();
    m_r5.on_initialize();
    m_adv.on_initialize();

    // Der Mag-Drop delegiert an den Advanced-Teil (Lua:
    // _G.__re4_reload_mag_slide).
    m_main.set_adv(&m_adv);
    m_r2.set_siblings(&m_main, &m_adv);
    m_r3.set_siblings(&m_main, &m_adv);
    m_r4.set_adv(&m_adv);
    m_r4.set_main(&m_main);
    m_r5.set_siblings(&m_main, &m_adv);

    // [HOLSTER-KETTE] reload2 wickelt in Lua fuenfmal um die Holster-Funktion
    // des Main-Teils und wird deshalb ZUERST gefragt.
    // Die spaeter geladenen Teile sind die AEUSSEREN Wrapper -> reload3 wird vor
    // reload2 gefragt, genau wie in Lua.
    m_main.set_mag_chain([this](bool active) -> std::optional<bool> {
        if (const auto r = m_r5.set_mag_in_hand(active); r.has_value()) {
            return r;
        }

        if (const auto r = m_r4.set_mag_chain_probe(active); r.has_value()) {
            return r;
        }

        if (const auto r = m_r3.set_mag_in_hand(active); r.has_value()) {
            return r;
        }

        return m_r2.set_mag_in_hand(active);
    });

    // sdk.hook -> braucht in Lua einen GAME-Neustart; nativ reicht ein Aufruf
    // beim ersten Initialisieren.
    if (!m_initialized) {
        m_initialized = true;
        m_main.install_hooks();
        m_r2.install_hooks();
        m_r3.install_hooks();
        m_r4.install_hooks();
        m_r5.install_hooks();
    }

    return Mod::on_initialize();
}

void RE4VRReload::on_lua_state_created(sol::state& lua) {
    (void)lua;
    m_main.on_lua_state_created();
    m_r2.on_lua_state_created();
    m_r3.on_lua_state_created();
    m_r4.on_lua_state_created();
    m_r5.on_lua_state_created();
}

void RE4VRReload::on_lua_state_destroyed(sol::state& lua) {
    (void)lua;
    m_main.on_lua_state_destroyed();
    m_r2.on_lua_state_destroyed();
    m_r3.on_lua_state_destroyed();
    m_r4.on_lua_state_destroyed();
    m_r5.on_lua_state_destroyed();
    m_adv.on_lua_state_destroyed();
}

void RE4VRReload::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waeren seine
    // Lua-Dateien nicht geladen.
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LockScene"_fnv) {
        m_main.on_lock_scene_pre();
        m_r2.on_lock_scene_pre();
        m_r3.on_lock_scene_pre();
        m_r4.on_lock_scene_pre();
        m_r5.on_lock_scene_pre();
        m_adv.on_lock_scene_pre();

        return;
    }

    if (hash == "BeginRendering"_fnv) {
        m_main.on_begin_rendering_pre();
        m_r2.on_begin_rendering_pre();
        m_r3.on_begin_rendering_pre();
        m_r4.on_begin_rendering_pre();
        m_r5.on_begin_rendering_pre();
        m_adv.on_begin_rendering_pre();
    }
}

void RE4VRReload::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        m_main.on_late_update();
        m_r2.on_late_update();
        m_r3.on_late_update();
        m_r4.on_late_update();
        m_r5.on_late_update();
        m_adv.on_late_update();

        return;
    }

    if (hash == "UpdateJointExpression"_fnv) {
        m_main.on_update_joint_expression();
        m_r2.on_update_joint_expression();
        m_r3.on_update_joint_expression();
        m_r4.on_update_joint_expression();
        m_r5.on_update_joint_expression();
        m_adv.on_update_joint_expression();

        return;
    }

    // [5-HOOK-STACK] Armbrust und Bowdraw brauchen diese Stufe zusaetzlich --
    // genau in dieser Luecke schreibt die Engine die Hand-Joints neu, waehrend
    // der Bolzen noch auf der alten L_Hand-Pose sitzt.
    if (hash == "UpdateMotion"_fnv) {
        m_r2.on_update_motion();

        return;
    }

    if (hash == "BeginRendering"_fnv) {
        m_main.on_begin_rendering();
        m_r2.on_begin_rendering();
        m_r3.on_begin_rendering();
        m_r4.on_begin_rendering();
        m_r5.on_begin_rendering();
        m_adv.on_begin_rendering();
    }
}

void RE4VRReload::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    // reload_adv hat keinen eigenen on_frame -- seine Arbeit haengt an den
    // Render-Paessen.
    m_main.on_frame();
    m_r2.on_frame();
    m_r3.on_frame();
    m_r4.on_frame();
    m_r5.on_frame();
}

// [MENUE-REIHENFOLGE 2026-09-07] Wird nicht mehr von REFramework gerufen: die
// sechs Reload-Trees sortiert RE4VRMenu einzeln zwischen die uebrigen ein
// (Reload, Reload Adv, Reload2, Reload3, Reload4, Reload 5). Die Sammelfassung
// bleibt als Notnagel stehen, falls man sie je wieder als Block braucht.
void RE4VRReload::draw_dev_ui() {
    m_main.draw_dev_ui();
    m_r2.draw_dev_ui();
    m_r3.draw_dev_ui();
    m_r4.draw_dev_ui();
    m_r5.draw_dev_ui();
    m_adv.draw_dev_ui();
}

#endif
