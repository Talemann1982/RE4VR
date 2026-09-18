// ============================================================================
// RE4VREquipLock -- 1:1-Portierung von re4_vr_equip_lock.lua
// Spezifikation: I:\LUATRANS\PORT_EQUIPLOCK_CAPACITIVE_SPEC.md (Teil A)
// ============================================================================

#if defined(RE4)

#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "RE4VR.hpp"
#include "RE4VREquipLock.hpp"
#include "RE4VRKillswitch.hpp"

namespace {

double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

} // namespace

std::shared_ptr<RE4VREquipLock>& RE4VREquipLock::get() {
    static auto inst = std::make_shared<RE4VREquipLock>();
    return inst;
}

bool RE4VREquipLock::blocked() {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return false;
    }
    if (!LOCK) {
        return false;
    }

    // [A-M1] Ohne erreichbaren Lua-State NICHT blocken.
    // Der native Hook lebt bis zum Prozessende, auch waehrend reset_scripts()
    // den Lua-State abreisst. In diesem Fenster lieferten die beiden
    // Tor-Globals ihre Defaults -- also "nicht unser Wechsel" UND "nicht im
    // Gameplay" -- und wir haetten JEDEN Waffenwechsel gesperrt. Im Original
    // war der sdk.hook zu diesem Zeitpunkt schlicht weg (Lua Z.20: "sdk.hook
    // ueberlebt Reset Scripts NICHT"). Dasselbe gilt fuer die Frames zwischen
    // Framework-Init und dem ersten Schreiben von __re4_frame_is_gameplay.
    {
        auto runner = ScriptRunner::get();

        if (runner == nullptr) {
            return false;
        }

        auto& state = runner->get_state();

        if (state == nullptr || state->lua().lua_state() == nullptr) {
            return false;
        }
    }

    // Unser eigener Wechsel? Das Zeitfenster legt der Holster um seine Aufrufe
    // (auch der native RE4VRHolster setzt __re4_our_equip_until an vier Stellen),
    // dazu binding und choke.
    // tonumber(nil) ist in Lua nil -> die Pruefung faellt dann aus; fehlt das
    // Global, gilt hier also "nicht unser Wechsel".
    const double u = re4vr::lua_get_number("__re4_our_equip_until", 0.0);

    if (u > 0.0 && now_clock() < u) {
        m_block_since.reset();

        return false;
    }

    // Im normalen Gameplay nichts anfassen -- nur im Killswitch/Event greifen
    // wir ein. Strikt gegen true, wie im Original.
    if (re4vr::lua_get_bool("__re4_frame_is_gameplay", false)) {
        m_block_since.reset();

        return false;
    }

    // [PWPAUSE-STAGES 15.09.2026] In 47400/47410 steht `frame_is_gameplay` nach
    // einem Save-Load minutenlang auf false (haengendes PlayWorkGimmick: hudOff,
    // PauseMenuSystemLock, InGameMenuOccupiedModule). Dieser Lock schluckt dann
    // ALLE Waffenwechsel der Engine, und die Waffe bleibt in Gun.State None --
    // gemessen re4_kein_schuss.txt 12:39/12:45: "wid=4102 ammo=30
    // Gun.State=0(None)", nur Zielen und Feuern tot, geheilt erst durch einen
    // Waffenwechsel beim Typewriter. Ein Re-Equip von aussen hat das NICHT
    // repariert (12:45: our_equip lief, State blieb None) -- also darf der
    // Schaden hier gar nicht erst entstehen.
    //
    // Der Lock ist fuer Treffer/Stagger gebaut; in diesen zwei Stages gibt es
    // nichts zu schuetzen, was den Preis wert waere.
    if (auto* c = re4vr::fc::ctx(); c != nullptr) {
        int32_t sid = 0;

        if (re4vr::try_call<int32_t>(c, "get_CurrentStageID", sid)
            && (sid == 47400 || sid == 47410)) {
            m_block_since.reset();

            return false;
        }
    }

    // [CUTSCENE 2026-09-08 -- gemessen] Waehrend einer Event-Kamera ruft die
    // Engine execChangeWeapon/requestChangeActiveWeapon JEDEN Frame, um die
    // Waffe zu setzen. Bisher wurde jeder dieser Aufrufe geschluckt; danach
    // blieb die Waffe in Gun.State None(0) haengen -- Schiessen ging nicht
    // mehr, bis der Spieler von Hand wechselte oder neu lud. Alles andere lief
    // weiter, deshalb sah es wie ein reiner "RT-Blocker" aus.
    // (Log 08.09., 23:23:33: KS=1 grund=IsEventCamera, gameplay=0, der
    // Block-Zeitstempel lief lueckenlos hoch, gun blieb None.)
    // Hier gibt es nichts zu schuetzen: das Wiederherstellen der Waffe durch
    // die Engine ist genau richtig. Der Lock ist fuer Treffer/Stagger gebaut.
    if (auto& ks = RE4VRKillswitch::get();
        ks != nullptr && ks->get_activating_reason() == "IsEventCamera") {
        m_block_since.reset();

        return false;
    }

    // [DAUERBLOCK-BREMSE] Auffangnetz fuer jeden weiteren Dauerfall, den wir
    // noch nicht kennen: Der Stagger, fuer den der Lock gebaut ist, ist nach
    // Sekundenbruchteilen vorbei. Wer laenger blockt, blockt falsch.
    const double now = now_clock();

    if (!m_block_since.has_value()) {
        m_block_since = now;
    } else if ((now - *m_block_since) > MAX_BLOCK_SEC) {
        return false;
    }

    return true;
}

std::optional<std::string> RE4VREquipLock::on_initialize() {
    // Beide Enden der Kette: der Request allein wuerde spaeter erneut
    // ausgefuehrt. Jede Methode einzeln geholt, nur bei Erfolg gehookt.
    auto* td = sdk::find_type_definition(game_namespace("PlayerEquipment"));

    if (td == nullptr) {
        return Mod::on_initialize();
    }

    const char* const sigs[2] = {
        "execChangeWeapon",
        "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
    };

    for (const char* sig : sigs) {
        auto* m = td->get_method(sig);

        if (m == nullptr) {
            continue;
        }

        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                if (RE4VREquipLock::get()->blocked()) {
                    // Reine Diagnose, ohne Leser -- 1:1 erhalten.
                    re4vr::lua_set_number("__re4_equip_lock_last", now_clock());
                    return HookManager::PreHookResult::SKIP_ORIGINAL;
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            // [FALLE] SKIP_ORIGINAL ohne sauberen Rueckgabewert hinterlaesst
            // Registermuell. Das Original setzt im Post-Hook ausdruecklich
            // `return retval` -- der leere Post-Hook hier ist das Gegenstueck:
            // er laesst ret_val unveraendert stehen, genau wie Luas Durchreiche.
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }

    return Mod::on_initialize();
}

void RE4VREquipLock::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VREquipLock", "on_lua_state_created");
    // Der Doppel-Hook-Guard des Originals. Nativ entbehrlich (HookManager::add
    // laeuft einmal in on_initialize), aber 1:1 gesetzt.
    lua["__re4_equip_lock_installed"] = true;
}

#endif // RE4
