#include <algorithm>
#include <array>
#include <functional>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "mods/BackBufferRenderer.hpp"
#include "mods/APIProxy.hpp"
#include "mods/Camera.hpp"
#include "mods/Graphics.hpp"
#include "mods/DeveloperTools.hpp"
#include "mods/FirstPerson.hpp"
#include "mods/FreeCam.hpp"
#include "mods/Hooks.hpp"
#include "mods/IntegrityCheckBypass.hpp"
#include "mods/ManualFlashlight.hpp"
#include "mods/PluginLoader.hpp"
#include "mods/REFrameworkConfig.hpp"
#include "mods/Scene.hpp"
#include "mods/ScriptRunner.hpp"
#include "mods/VR.hpp"
#include "mods/LooseFileLoader.hpp"
#include "mods/vr/games/RE8VR.hpp"
#include "mods/vr/games/RE4VR.hpp"
#include "mods/vr/games/RE4VRArmChain.hpp"
#include "mods/vr/games/RE4VRFirstPerson.hpp"
#include "mods/vr/games/RE4VRHolster.hpp"
#include "mods/vr/games/RE4VRMaterials.hpp"
#include "mods/vr/games/RE4VRCrosshair.hpp"
#include "mods/vr/games/RE4VRRecoil.hpp"
#include "mods/vr/games/RE4VRCapacitive.hpp"
#include "mods/vr/games/RE4VRWhitelist.hpp"
#include "mods/vr/games/RE4VRObjects.hpp"
#include "mods/vr/games/RE4VRScope.hpp"
#include "mods/vr/games/RE4VRMovement.hpp"
#include "mods/vr/games/RE4VRMenu.hpp"
#include "mods/vr/games/RE4VRUi.hpp"
#include "mods/vr/games/RE4VRMinecart.hpp"
#include "mods/vr/games/RE4VRMotion.hpp"
#include "mods/vr/games/RE4VRChoke.hpp"
#include "mods/vr/games/RE4VRGuestures.hpp"
#include "mods/vr/games/RE4VRMerc.hpp"
#include "mods/vr/games/RE4VRKillswitch.hpp"
#include "mods/vr/games/RE4VRBinding.hpp"
#include "mods/vr/games/RE4VREquipLock.hpp"
#include "mods/vr/games/RE4VRWeapons.hpp"
#include "mods/vr/games/RE4VRWeapons2.hpp"
#include "mods/vr/games/RE4VRReload.hpp"
#include "mods/TemporalUpscaler.hpp"

#include "Mods.hpp"

Mods::Mods() {
    m_mods.emplace_back(BackBufferRenderer::get());
    m_mods.emplace_back(REFrameworkConfig::get());

#if defined(REENGINE_AT)
    m_mods.emplace_back(std::make_unique<IntegrityCheckBypass>());
#endif

#ifndef BAREBONES
    m_mods.emplace_back(Hooks::get());
    m_mods.emplace_back(LooseFileLoader::get());

    m_mods.emplace_back(VR::get());
    m_mods.emplace_back(TemporalUpscaler::get());

#if defined(RE8) || defined(RE7)
    m_mods.emplace_back(RE8VR::get());
#endif


#ifndef RE8
#if defined(RE2) || defined(RE3)
    m_mods.emplace_back(FirstPerson::get());
#endif
#endif

    // All games!!!
    m_mods.emplace_back(Camera::get());
    m_mods.emplace_back(Graphics::get());

#if defined(RE2) || defined(RE3) || defined(RE8)
    m_mods.emplace_back(std::make_unique<ManualFlashlight>());
#endif

    m_mods.emplace_back(std::make_unique<FreeCam>());

#if TDB_VER > 49
    m_mods.emplace_back(std::make_unique<SceneMods>());
#endif

#endif

#ifdef DEVELOPER
    auto dev_tools = std::make_shared<DeveloperTools>();
    m_mods.emplace_back(dev_tools);

    for (auto& tool : dev_tools->get_tools()) {
        m_mods.emplace_back(tool);
    }
#endif

    m_mods.emplace_back(APIProxy::get());
    m_mods.emplace_back(PluginLoader::get());

#if defined(RE4)
    // Traeger fuer die nach C++ portierten RE4-VR-Scripte. Aktuell haelt er genau
    // EINES: re4_vr_crosshair.lua.
    //
    // Die Position hier ist nur der FALLBACK. Im Normalfall ruft ein winziges
    // Stub-Script mit dem Originalnamen die Phasen auf -- nur so steht das Modul
    // an seiner alphabetischen Stelle im Callback-Strom. Ueber den Mod-Vektor
    // ginge das nicht: die Phasen laufen in Vektor-Reihenfolge (Hooks.cpp), und
    // der ScriptRunner ist EIN einziger Mod -- alle Lua-Scripte liegen damit
    // komplett vor oder komplett hinter uns, nie mittendrin.
    m_mods.emplace_back(RE4VR::get());

    // [KILLSWITCH GANZ VORNE -- zwingend vor dem ScriptRunner]
    // Er ist kein autorun-Script, sondern ein require-MODUL: die verbliebenen
    // Lua-Dateien holen ihn mit require("re4vr/re4_vr_killswitch"). Seine
    // Tabelle muss deshalb in package.loaded stehen, BEVOR der ScriptRunner die
    // erste Lua-Datei laedt -- sonst faellt jedes require auf den Fehlerpfad
    // und binding/weapons/reload laufen ohne Killswitch.
    //
    // Die Reihenfolge im Mod-Vektor entscheidet hier NICHT ueber die
    // Auswertung: die laeuft in einer eigenen Phase (pre-UpdateScene), nicht in
    // on_frame. Die alphabetische Falle der anderen Ports greift also nicht.
    m_mods.emplace_back(RE4VRKillswitch::get());

    // [BINDING VOR DEM SCRIPTRUNNER] binding schreibt 57 Globals und liest rund
    // 60 fremde. Solange weapons/weapons2/reload* noch als Lua laufen, muessen
    // sie seine Werte wie bisher FRISCH sehen -- also steht es vor ihnen.
    // Sobald diese Dateien portiert sind, gehoert der Platz neu geprueft.
    m_mods.emplace_back(RE4VRBinding::get());

    // [CHOKE VOR MOTION -- und damit ebenfalls vor dem ScriptRunner]
    // Alphabetisch laedt re4_vr_choke.lua VOR re4_vr_motion.lua: choke sah die
    // Motion-Globals (__vr_lh_world, __vr_rh_world, vr_knife_swing,
    // vr_knife_velocity, vr_knife_velocity_peak) im VORFRAME-Stand, und motion
    // sah die Choke-Globals FRISCH. Genau daran haengt die Wuergehoehe der
    // Hand (__re4_choke_hand_y/_play/_dy/_y_lerp), der Herzschlag
    // __re4_choke_seen und __re4_is_choking(). Stuende Choke dahinter, kaeme
    // jede dieser Groessen bei motion einen Frame zu spaet.
    m_mods.emplace_back(RE4VRChoke::get());

    // [GUESTURES ZWISCHEN CHOKE UND MERC] choke < guestures < merc < motion.
    m_mods.emplace_back(RE4VRGuestures::get());

    // [MERC ZWISCHEN CHOKE UND MOTION] Alphabetisch gilt
    // re4_vr_choke.lua < re4_vr_merc.lua < re4_vr_motion.lua (c < me < mo) --
    // die drei Module bilden diese Kette 1:1 ab. merc las die motion-Globals
    // (__vr_dbg_wep_id, __re4_knife_hand) im VORFRAME-Stand, motion die
    // merc-Globals FRISCH und ruft __re4_merc_wep_apply /
    // __re4_merc_apply_bow_pose direkt.
    m_mods.emplace_back(RE4VRMerc::get());

    // [MOTION VOR DEM SCRIPTRUNNER -- gemessen, nicht geraten]
    // Als einziges portiertes Modul steht RE4VRMotion VOR ScriptRunner::get().
    // Grund: motion LIEST 52 Globals, die spaeter ladende Lua-Dateien schreiben
    // (reload, reload2-5_dlc, reload_adv, weapons, movement). Dahinter saehe es
    // sie frisch statt wie bisher einen Pass alt -- jede darauf kalibrierte
    // Rampe und Flanke (Slide-Dock-Blend, Pump-Progress, __vr_motion_paused)
    // wuerde sich verschieben.
    // Davor kippen jetzt nur noch die Globals von binding, das ausschliesslich
    // in on_frame schreibt -- die Phasenlage ist dort egal. choke und merc
    // waren die beiden anderen Quellen und stehen seit dem 04.09. selbst als
    // RE4VRChoke/RE4VRMerc darueber, an genau ihrer Lua-Stelle.
    // Sobald KEIN Lua mehr laeuft, ist die Position gleichgueltig und das Modul
    // gehoert der Ordnung halber zu den anderen hinter den ScriptRunner.
    // [FIRSTPERSON VOR MOTION -- korrigiert 05.09.2026]
    // firstperson schreibt die Tabelle `vr_camera_fix` (camera_pos/camera_rot),
    // und RE4VRMotion liest sie an zwei Stellen. Alphabetisch gilt
    // re4_vr_firstperson.lua < re4_vr_motion.lua (f < mo): in Lua lief
    // firstperson im SELBEN Pass zuerst, motion sah den FRISCHEN Wert.
    // Im Port stand FirstPerson hinter dem ScriptRunner und damit HINTER
    // motion -- motion nahm camera_pos/camera_rot aus dem vorherigen Pass, und
    // Haende und Waffe hinkten der Kamera nach (Drift beim Gehen und
    // Kopfdrehen). Der Kommentar an der Lesestelle behauptete sogar das
    // Gegenteil. Die Richtschnur weiter unten nennt "firstperson vor
    // motion/movement" -- hier steht sie jetzt auch so im Code.
    m_mods.emplace_back(RE4VRFirstPerson::get());

    // [MINECART VOR MOTION -- korrigiert 07.09.2026]
    // Alphabetisch gilt re4_vr_minecart.lua < re4_vr_motion.lua (mi < mo):
    // motion sah die Minecart-Globals im SELBEN Pass FRISCH. Im Port stand
    // Minecart hinter dem ScriptRunner und damit HINTER RE4VRMotion -- motion
    // las __re4_railcar_reloading, __re4_railcar_reload_fade und die beiden
    // Hand-Ease-Werte __re4_reload_hand_fp/_fr aus dem VORHERIGEN Pass, und
    // bei __re4_reload_hand_fp/_fr kam dazu, dass Minecarts nil-Ruecksetzer
    // erst NACH Motions Capture lief (in Lua umgekehrt).
    // Gefunden mit der Phasen-Analyse ueber alle Globals; es war der einzige
    // verbliebene Fall dieser Art.
    //
    // Die bisherigen Gruende bleiben erfuellt: Minecart steht weiter VOR
    // RE4VRArmChain (dessen phase_entry ruft apply_spine_pin VOR der 2-Bone-IK,
    // und ArmChain steht ganz am Ende) und VOR RE4VRCrosshair.
    m_mods.emplace_back(RE4VRMinecart::get());

    m_mods.emplace_back(RE4VRMotion::get());
#endif

    m_mods.emplace_back(ScriptRunner::get());

#if defined(RE4)
    // Nach dem ScriptRunner. EIN eingebauter Mod je portierter Lua-Datei --
    // die Reihenfolge HIER ist die Ausfuehrungsreihenfolge der Phasen.
    // Richtschnur (wie im Original-Alphabet): firstperson vor motion/movement,
    // motion vor arm_chain, movement zuletzt.
    // [VERSCHOBEN 05.09.2026] RE4VRFirstPerson steht jetzt weiter oben, direkt
    // VOR RE4VRMotion -- s. dort. Hier bleibt nur der Hinweis, damit die
    // Richtschnur oben nicht in die Irre fuehrt.
    // [ARM_CHAIN ANS ENDE 2026-09-06] steht jetzt ganz unten -- s. dort.
    m_mods.emplace_back(RE4VRHolster::get());
    // [SCOPE-REIHENFOLGE] RE4VRScope schreibt das Global vr_scope_active und
    // muss deshalb VOR RE4VRMaterials und RE4VRCrosshair stehen -- beide lesen
    // es (RE4VRCrosshair.cpp, RE4VRMaterials.cpp) und saehen es sonst erst
    // einen Frame spaeter.
    //
    // ZUR EHRLICHKEIT: in reinem Lua lief re4_vr_crosshair.lua alphabetisch VOR
    // re4_vr_scope.lua, las den Flankenwert also ohnehin einen Frame verzoegert.
    // Seit crosshair nativ hinter dem ScriptRunner laeuft, sieht es den frischen
    // Wert -- und genau dieser Zustand ist getestet. Die Einreihung hier haelt
    // ihn fest, statt eine neue Verzoegerung einzufuehren.
    m_mods.emplace_back(RE4VRScope::get());
    m_mods.emplace_back(RE4VRMaterials::get());
    m_mods.emplace_back(RE4VRCrosshair::get());
    m_mods.emplace_back(RE4VRRecoil::get());
    m_mods.emplace_back(RE4VRCapacitive::get());
    m_mods.emplace_back(RE4VRWhitelist::get());
    m_mods.emplace_back(RE4VRObjects::get());
    m_mods.emplace_back(RE4VREquipLock::get());
    // [MOVEMENT ZULETZT] Vier unabhaengige Gruende (Spec-Nachtrag F):
    //  1. apply_lag_fix muss nach RE4VRFirstPerson::apply_movement_stabilization
    //  2. RE4VRHolster liest __re4_ub_z_delta (sah in Lua den Vorframe-Wert)
    //  3. RE4VRArmChain liest __vr_anim_l0   (sah in Lua den Vorframe-Wert)
    //  4. RE4VRCrosshair setzt is_aim, movement liest es in derselben Phase
    m_mods.emplace_back(RE4VRMovement::get());
    // [RELOAD-BLOCK HINTER MOVEMENT -- verschoben 07.09.2026]
    // Vorher stand der Block VOR dem ScriptRunner. Die Begruendung dafuer war,
    // dass reload2..5 noch als Lua liefen und in Lua der spaetere Schreiber der
    // 18 gemeinsamen Globals gewinnt -- seit alle sechs Dateien portiert sind
    // und in DIESEM Mod stecken, gilt sie nicht mehr.
    // Der Preis war ein Trailing: die Shell-Keyframes setzen ueber
    // apply_shell_keys eine WELT-Position (Waffen-Position + Waffen-Rotation *
    // Offset, RE4VRReloadAdv.cpp). Vor RE4VRMovement gerechnet, entsteht sie aus
    // dem Stand VOR der Bewegungskorrektur -- beim Laufen hinken die einlegenden
    // Huelsen sichtbar um eine Frame-Laufstrecke hinterher (User, Red9, Shell wie
    // Stripper-Clip). Dieselbe Ursache wie bei den trailenden Armen am 06.09.,
    // als RE4VRArmChain vor Movement stand.
    // Lua-Reihenfolge stuetzt die neue Position: re4_vr_movement.lua laedt vor
    // re4_vr_reload.lua ("mov" < "rel"), reload lief dort also ebenfalls NACH der
    // Bewegungskorrektur. Vor RE4VRWeapons, wie im Alphabet ("rel" < "wea").
    m_mods.emplace_back(RE4VRReload::get());
    // [WEAPONS2 ZULETZT] re4_vr_weapons2.lua ist die alphabetisch LETZTE
    // autorun-Datei. Zwei Stellen haengen daran (s. RE4VRWeapons2.hpp):
    // der Ladezeit-Wrap auf __re4_reload_set_mag_in_hand (weapons2 gewinnt
    // gegen alle sechs Schreiber) und der Unlimited-Tick, der die
    // grab_empty-Zuweisungen der Reload-Dateien ueberschreiben MUSS.
    // [WEAPONS VOR WEAPONS2] Lua-Reihenfolge weapons < weapons2. Die
    // Platzierung ist unkritisch (gemessen): die Kopplung zum Reload-Block
    // besteht in BEIDE Richtungen nur aus zwei Schaltern.
    m_mods.emplace_back(RE4VRWeapons::get());
    m_mods.emplace_back(RE4VRWeapons2::get());

    // [UI ANS ENDE] Der Mono-Broker muss die Anmeldung von RE4VRFirstPerson
    // (Raetsel-Mono) frisch sehen -- das steht weiter vorn. In Lua galt
    // dasselbe (firstperson `f` < ui `u`).
    // [MENUE] Zeichnet nur -- keine Phase, keine Hooks. Die Position im Vektor
    // ist fuer die UI egal (Mods::on_draw_ui ruft ihn gezielt), er steht hier
    // nur, damit er ueberhaupt initialisiert wird.
    m_mods.emplace_back(RE4VRMenu::get());
    m_mods.emplace_back(RE4VRUi::get());

    // [ARM_CHAIN GANZ ZULETZT 2026-09-06] Vorher stand es direkt hinter
    // RE4VRMinecart -- und damit VOR RE4VRHolster, RE4VRCrosshair, RE4VRRecoil
    // und vor allem VOR RE4VRMovement. Alle vier haengen in denselben Phasen
    // (LockScene / LateUpdateBehavior / UpdateJointExpression / BeginRendering)
    // wie die Arm-IK.
    //
    // Im Original ist arm_chain in JEDER dieser Phasen der LETZTE Callback --
    // nicht wegen des Alphabets, sondern weil re4_vr_arm_chain.lua seine
    // Phasen-Hooks als einzige Datei verzoegert im ersten on_frame anmeldet
    // (arm_chain.lua Z.1301-1307), also nach allen anderen, die sich beim Laden
    // eintragen. Genau darauf beruht das gemessene Verhalten: die 2-Bone-IK
    // loest auf die Hand-/Body-Werte, die movement in DIESER Phase bereits
    // geschrieben hat.
    //
    // Stand arm_chain davor, loeste die IK auf den Body VOR der
    // Bewegungskorrektur -- im Stand unsichtbar, beim Gehen/Laufen genau das
    // Bild: die Arme haengen um die Bewegung eines Frames hinterher und die
    // Haende loesen sich von der Waffe. Vgl.
    // [[reference-re4-cpp-hookreihenfolge-scriptrunner]]: "nur arm_chain
    // dahinter".
    //
    // Unkritisch fuer alles andere: RE4VRArmChain::on_frame setzt einzig das
    // Flag m_phase_hooks_registered, es haengt an der Vektorposition also keine
    // zweite Reihenfolge. RE4VRMinecart bleibt davor (phase_entry ruft
    // apply_spine_pin VOR der IK).
    m_mods.emplace_back(RE4VRArmChain::get());
#endif
}

std::optional<std::string> Mods::on_initialize() const {
    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize()", mod->get_name().data());

        if (auto e = mod->on_initialize(); e != std::nullopt) {
            spdlog::info("{:s}::on_initialize() has failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    utility::Config cfg{ (REFramework::get_persistent_dir() / REFrameworkConfig::REFRAMEWORK_CONFIG_NAME).string() };

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_config_load()", mod->get_name().data());
        mod->on_config_load(cfg);
    }

    return std::nullopt;
}


std::optional<std::string> Mods::on_initialize_d3d_thread() const {
    auto do_not_hook_d3d = g_framework->acquire_do_not_hook_d3d();

    utility::Config cfg{ (REFramework::get_persistent_dir() / REFrameworkConfig::REFRAMEWORK_CONFIG_NAME).string() };

    // once here to at least setup the values
    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_config_load()", mod->get_name().data());
        mod->on_config_load(cfg);
    }

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize_d3d_thread()", mod->get_name().data());

        if (auto e = mod->on_initialize_d3d_thread(); e != std::nullopt) {
            spdlog::info("{:s}::on_initialize_d3d_thread() has failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_config_load()", mod->get_name().data());
        mod->on_config_load(cfg);
    }

    return std::nullopt;
}

void Mods::on_pre_imgui_frame() const {
    for (auto& mod : m_mods) {
        mod->on_pre_imgui_frame();
    }
}

void Mods::on_frame() const {
    for (auto& mod : m_mods) {
#if defined(RE4)
        // [FRAMETIME-MESSUNG] s. RE4VR.hpp -- der ScriptRunner ist selbst ein
        // Mod und erscheint damit als eine Zeile fuer alle Lua-Dateien.
        re4vr::perf::Scope _p{mod->get_name(), "on_frame"};
#endif
        mod->on_frame();
    }

#if defined(RE4)
    // Frame abschliessen: Summen bilden, Buckel erkennen, ggf. mitschreiben.
    re4vr::perf::frame_end();
#endif
}

void Mods::on_present() const {
    for (auto& mod : m_mods) {
        mod->on_early_present();
    }

    for (auto& mod : m_mods) {
        mod->on_present();
    }
}

void Mods::on_post_frame() const {
    for (auto& mod : m_mods) {
        mod->on_post_frame();
    }
}

// [MENUE-KATEGORIEN 11.09.2026] Frueher zeichnete EIN on_draw_ui() alles
// untereinander: Upscaler-Tree, "Mod Options" des ScriptRunners, dann
// RE4VRMenu (Public-UI, "REFramework Options", Dev-Trees). Jetzt hat das
// Hauptfenster links Kategorien, und jede holt sich hier ihren Teil.
//
// Die ZEICHENreihenfolge haengt weiter nicht am Mod-Vektor -- dessen
// Reihenfolge ist Phasen-Reihenfolge (Hooks.cpp) und darf nie fuer die Optik
// verbogen werden.
//
// Fuer einen Public-Release bleibt es bei EINER Zeile:
// `constexpr bool RE4VR_DEV_UI = false;` (RE4VRMenu.hpp) -- dann fehlt die
// Kategorie "Developer" ganz.
void Mods::draw_upscaler() const {
    if (const auto& upscaler = TemporalUpscaler::get(); upscaler != nullptr) {
        upscaler->draw_settings();
    }
}

void Mods::draw_mod_options() const {
#if defined(RE4)
    // Das nackte Public-UI in fester Reihenfolge.
    if (const auto& menu = RE4VRMenu::get(); menu != nullptr) {
        menu->draw_public();
    }
#endif
}

void Mods::draw_developer() const {
#if defined(RE4)
    if constexpr (RE4VR_DEV_UI) {
        if (const auto& menu = RE4VRMenu::get(); menu != nullptr) {
            menu->draw_dev();
        }

        // Die UIs geladener Lua-Scripte (z. B. "# RE4SoundPlayer") -- frueher
        // im Tree "Mod Options" unter dem nackten UI, jetzt hier und damit im
        // Public-Release weg.
        //
        // [EIGENER TREE 16.09.2026 -- Ansage des Users] Sie zeichneten hier
        // ohne Ueberschrift direkt zwischen die Dev-Trees. Jetzt stehen sie
        // gesammelt unter "Script Generated UI" -- ein Baum, zugeklappt ist
        // Ruhe, aufgeklappt steht darin, was die geladenen Scripte anbieten.
        if (const auto& sr = ScriptRunner::get(); sr != nullptr) {
            // Zuerst "Scripts" (laden/neu laden), darunter das, was die
            // geladenen Scripte an Oberflaeche mitbringen -- zwei verschiedene
            // Dinge, beide hier.
            sr->draw_scripts_tree();

            if (ImGui::TreeNode("Script Generated UI")) {
                sr->on_draw_ui();
                ImGui::TreePop();
            }
        }
    }
#endif
}

bool Mods::has_developer() const {
#if defined(RE4)
    return RE4VR_DEV_UI;
#else
    return false;
#endif
}

// [REF OPTIONS 10.09.2026] Alle REFramework-eigenen Trees, gesammelt in EINEM
// Baum. Negativliste statt Aufzaehlung: gezeichnet wird jeder Mod AUSSER den
// dreien, die oben schon ihren festen Platz haben. Damit taucht auch ein
// kuenftiger Upstream-Mod automatisch hier auf, statt still unsichtbar zu sein.
// Die portierten RE4VR-Module stehen zwar ebenfalls im Vektor, ueberschreiben
// aber kein on_draw_ui (einzige Ausnahme ist RE4VRMenu) -- sie zeichnen hier
// also nichts.
void Mods::draw_ref_trees() const {
    // [ALPHABETISCH 16.09.2026 -- Ansage des Users] Wie die Developer-Trees:
    // erst alle Eintraege sammeln, dann nach ANGEZEIGTEM Namen sortieren und
    // zeichnen. Der Name ist meist get_name(); zwei Mods beschriften ihren
    // Header anders (Hooks -> "Performance", REFrameworkConfig ->
    // "Configuration"), die stehen deshalb ausdruecklich hier. "About" zeichnet
    // REFramework::draw_ref_options davor -- steht alphabetisch ohnehin vorn.
    struct Entry {
        std::string name;
        std::function<void()> draw;
    };

    std::vector<Entry> entries{};

    for (const auto& mod : m_mods) {
        const auto name = mod->get_name();

        if (name == "TemporalUpscaler" || name == "ScriptRunner"
            || name == "RE4VRMenu") {
            continue;
        }

        std::string label{name};

        if (name == "Hooks") {
            label = "Performance";
        } else if (name == "REFrameworkConfig") {
            label = "Configuration";
        }

        entries.push_back({std::move(label), [m = mod.get()] { m->on_draw_ui(); }});
    }

    // [SCRIPTS NACH DEVELOPER 16.09.2026 -- Ansage des Users] Der "Scripts"-Baum
    // (Laden/Neuladen der Lua-Dateien) gehoert zu den Scripten und steht auch
    // in "Developer" direkt UEBER "Script Generated UI".
    //
    // [SCRIPTS AUCH HIER 16.09.2026 -- Ansage des Users] Dieselben zwei Punkte
    // stehen zusaetzlich hier. Gleiche Labels wie dort sind unkritisch: die
    // Kategorien des Hauptfensters werden nie im selben Frame gezeichnet.
    //
    // [RAHMEN 16.09.2026 -- Ansage des Users] Hier als CollapsingHeader mit
    // Rahmen wie alle anderen Eintraege dieser Kategorie; unter "Developer"
    // bleiben es TreeNodes.
    if (const auto& sr = ScriptRunner::get(); sr != nullptr) {
        auto* const runner = sr.get();

        entries.push_back({"Scripts", [runner] { runner->draw_scripts_tree(true); }});
        entries.push_back({"Script Generated UI", [runner] {
            ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_Once);

            if (ImGui::CollapsingHeader("Script Generated UI")) {
                runner->on_draw_ui();
            }
        }});
    }

    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });

    for (const auto& e : entries) {
        e.draw();
    }
}

void Mods::on_device_reset() const {
    for (auto& mod : m_mods) {
        mod->on_device_reset();
    }
}
