// ============================================================================
// RE4VRMaterials -- 1:1-Portierung von re4_vr_materials.lua
// Spezifikation: I:\LUATRANS\PORT_MATERIALS_SPEC.md (Fassung 2)
//
// Zeilenverweise in den Kommentaren beziehen sich auf die Lua-Datei, Stand
// 02.09.2026 23:01 (1162 Zeilen).
// ============================================================================

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/SystemArray.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../VR.hpp"
#include "RE4VR.hpp"
#include "RE4VRMaterials.hpp"

#if defined(RE4)

// ============================================================================
// Lokale Helfer
// ============================================================================
namespace {

// Lua: os.clock(). Das Lua dieser Mod ist in dieselbe DLL gelinkt, os.clock ist
// dort clock()/CLOCKS_PER_SEC -- identische Epoche, direkt vergleichbar.
double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

sdk::REMethodDefinition* find_method(::REManagedObject* obj, std::string_view name) {
    if (obj == nullptr) {
        return nullptr;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return nullptr;
    }

    return def->get_method(name);
}

bool clear_pending(sdk::VMContext* context, bool ok) {
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        return false;
    }

    return ok;
}

// via.vec3 ist groesser als ein Register und braucht den versteckten
// Out-Zeiger PLUS 16-Byte-Ausrichtung -- genau wie in RE4VRHolster.
bool get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{0.0f, 0.0f, 0.0f, 0.0f};
    bool ok = false;

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj);
        ok = true;
    } catch (...) {
        ok = false;
    }

    ok = clear_pending(context, ok);

    if (ok) {
        out = glm::vec3{buf.x, buf.y, buf.z};
    }

    return ok;
}

// getMaterialName(mi). Leerer String = Lua-nil (der Aufrufer behandelt beides
// gleich, weil ein Materialname nie leer ist).
std::string mat_name(::REManagedObject* renderer, int32_t mi) {
    auto* n = re4vr::call_safe<::REManagedObject*>(renderer, "getMaterialName", mi);

    if (n == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
    } catch (...) {
        return {};
    }
}

std::string to_lower(const std::string& s) {
    std::string r = s;

    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return r;
}

// Lua: nm:match("^wp%d")
bool starts_wp_digit(const std::string& nm) {
    return nm.size() >= 3 && nm[0] == 'w' && nm[1] == 'p'
           && nm[2] >= '0' && nm[2] <= '9';
}

// Lua: nm:match("^ac%d%d%d%d_%d%d$")
bool is_ac_pattern(const std::string& nm) {
    if (nm.size() != 9) {
        return false;
    }

    if (nm[0] != 'a' || nm[1] != 'c' || nm[6] != '_') {
        return false;
    }

    for (const int i : {2, 3, 4, 5, 7, 8}) {
        if (nm[i] < '0' || nm[i] > '9') {
            return false;
        }
    }

    return true;
}

// Lua: nm:find(needle, 1, true) -- Teilstring an BELIEBIGER Stelle, keine
// Praefix-Pruefung. Ein starts_with waere hier nicht 1:1 (Spec 7).
bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Der Lua-State. re4vr::lua_state() liegt in RE4VR.cpp in einem anonymen
// Namensraum und ist von hier nicht erreichbar -- deshalb dieselbe Fassung
// noch einmal. Die Null-Pruefung ist Pflicht: reset_scripts() gibt den State
// frei und baut ihn neu, dazwischen ist der Zeiger null.
re4vr::LuaRef materials_lua_state() {
    // [ABSTURZ 04.09.2026] Sperre des ScriptRunners halten -- s. re4vr::LuaRef.
    return re4vr::LuaRef{};
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_materials.json";

// Lua Z.720: SCAN_NODES_PER_FRAME
constexpr int SCAN_NODES_PER_FRAME = 64;

// Lua Z.919-928
constexpr int32_t GONDOLA_STAGE = 60850;
constexpr const char* GONDOLA_PREFIX = "gm81_303_00_0_";
constexpr const char* GONDOLA_KEEP = "PLGondola";

} // namespace

// ============================================================================
// Namenstabellen -- Gross-/Kleinschreibung ist BEDEUTUNGSTRAGEND und woertlich
// aus dem Lua uebernommen (Spec 1.1). Nicht vereinheitlichen: Leon schreibt
// Hair00_Mat gross, Ada Hair00_mat klein; Leon EyeWet_mat, Ashley/Ada
// Eyewet_mat.
// ============================================================================

const std::unordered_set<std::string> RE4VRMaterials::PLAYER_BODY_NAMES = {
    "ch0a0z0_body",   // Leon
    "ch0a1z0_body",   // Ashley
    "ch3a8z0_body",   // Ada (Separate Ways)
};

// [ADA_NPC_SCHUTZ] Ada steht BEWUSST nicht drin: in Leons Kampagne ist sie NPC
// mit demselben GO-Namen. Der Primaerpfad fragt den CharacterManager nach dem
// GESTEUERTEN Body und kann sie nie erwischen -- dieser Fallback sucht die
// Szene dagegen stumpf nach Namen ab und wuerde ihr bei einem Aussetzer des
// Primaerpfads mitten in Leons Kampagne den Kopf wegblenden.
const std::unordered_set<std::string> RE4VRMaterials::FALLBACK_BODY_NAMES = {
    "ch0a0z0_body",
    "ch0a1z0_body",
};

const std::unordered_set<std::string> RE4VRMaterials::HIDE_MATERIALS_LEON = {
    // Head
    "EyeAO_mat", "EyeOut_mat", "Face_mat", "EyeWet_mat", "BrowsEyeLashes_mat",
    "Eye_inside_mat", "Mouth_mat",
    // Hair
    "Hair00_Mat", "Hair01_Mat",
    // [PINSTRIPE-HUT] liegt als Submaterial im grossen 'body'-Mesh
    "Hat00_Mat", "Hat01_Mat",
    // [PINSTRIPE-HAAR] eigene Haar-Materialien des Kostuems (Hinterkopf)
    "pl0074_Hair_Mat", "pl0074_Hair2_Mat",
};

const std::unordered_set<std::string> RE4VRMaterials::HIDE_MATERIALS_ASHLEY = {
    // Head (GO "head")
    "Eye_out_mat", "Face_mat", "Mouth_mat", "Ao_mat", "EyeLash_mat",
    "Eye_in_mat", "Eyebrows_mat", "Eyewet_mat",
    // Hair (GO "hair")
    "Hair_A_Mat", "Hair_B_Mat", "Hair_C_Mat",
};

const std::unordered_set<std::string> RE4VRMaterials::HIDE_MATERIALS_ADA = {
    // Head
    "Ao_mat", "Blow_mat", "EyeLash_mat", "Eye_in_mat", "Eye_out_mat",
    "Eyewet_mat", "Face_mat", "Mouth_mat", "Lens_Inside_mat",
    // Hair -- ACHTUNG kleines "mat", Leon hat Hair00_Mat (gross)
    "Hair00_mat", "Hair01_mat", "Hair02_mat",
};

// [PROPS_MAT 16.09.2026 -- Ansage "nur auf Leon in der Kampagne gaten"]
// "Props_Mat" liegt als Submaterial im grossen 'body'-Mesh unter
// ch0a0z0_body -> children -> body -> Materials. Es gehoert ins reine
// Gameplay AUS und in der Cutscene wieder AN -- also genau das Verhalten,
// das die HIDE_MATERIALS-Liste ohnehin hat: hide_mats_on() schreibt jedes
// Material dieser Liste mit m_mat_set_enable, und das ist nur in KS1
// (echte Cutscene, Killswitch aktiv ohne KS2..KS5) true.
//
// Eigene Liste statt eines Eintrags in HIDE_MATERIALS_LEON, weil die dort
// auch in den Mercenaries gilt -- Leon traegt dort denselben Bodynamen.
// Zusammengesetzt aus der Leon-Liste, damit es keine zweite Pflegestelle
// gibt; die Reihenfolge der Definitionen in dieser Datei stellt sicher,
// dass HIDE_MATERIALS_LEON hier bereits steht.
const std::unordered_set<std::string> RE4VRMaterials::HIDE_MATERIALS_LEON_CAMPAIGN = [] {
    auto s = RE4VRMaterials::HIDE_MATERIALS_LEON;
    s.insert("Props_Mat");

    return s;
}();

// Materialien, die im Voll-Aus NICHT auf DrawDefault hoeren.
const std::unordered_set<std::string> RE4VRMaterials::FULLHIDE_EXTRA_MATS = {
    "JacketFur_Mat",   // Leon, Jacken-Kostuem (Fell am Kragen)
    "Jacket_Mat",      // Leon, Jacke
};

// [TOTE EINTRAEGE -- 1:1 uebernommen] Abgefragt wird mit nm:lower() (Lua Z.642),
// deshalb koennen HookShot_Rope und HookShot_Gun NIE treffen. Beides bleibt so:
// wer den Lookup "repariert", blendet Adas Greifhaken neu aus -- ein
// Verhaltenswechsel, kein Bugfix.
const std::unordered_set<std::string> RE4VRMaterials::FULLHIDE_GO_NAMES = {
    "body",          // Leon + Ashley
    "body_armor",    // Leon
    "headhair",      // Leon (kombiniertes Head/Hair-Mesh)
    "cloth",         // Ashley (Jacke/Pants_Front)
    "cha200_00",     // Ada Koerper
    "cha200_10",     // Ada Kopf
    "cha200_20",     // Ada Haare
    "sm61_342_00",   // Ada Ausruestungsteil
    "HookShot_Rope", // Ada Greifhaken: Seil   -- tot, s.o.
    "HookShot_Gun",  // Ada Greifhaken: Geraet -- tot, s.o.
};

// 18 von Leon + 14 von Ada, am 21.08. LIVE durchprobiert -- keine geratenen aus
// der Engine-Enum. Alles, was hier nicht steht, bleibt sichtbar: lieber ein
// Accessoire zu viel im Bild als noch mal ein Werkzeug unsichtbar.
const std::unordered_set<std::string> RE4VRMaterials::HIDE_ACCESSORY_GO = {
    "ac0400_10", "ac0401_10", "ac0402_10", "ac0501_10", "ac0600_10",
    "ac0601_10", "ac0602_10", "ac0603_10", "ac0700_10", "ac0701_10",
    "ac0702_10", "ac0800_10", "ac0900_10", "ac0901_10",
    "ac1000_10",   // Huehnerkopf
    "ac1100_20", "ac1200_10",
    "ac1300_10",   // Leons Katzenohren (traegt bei ihm die unendliche Reserve)
    // Ada (Separate Ways)
    "ac3000_10", "ac3001_10", "ac3002_10", "ac3100_10", "ac3101_10",
    "ac3102_10", "ac3300_10", "ac3400_10", "ac3600_30", "ac3601_30",
    "ac3700_10", "ac3800_20",
    "ac3900_10",   // Adas Katzenohren
    "ac4000_10",
};

// Handgehaltenes WERKZEUG, niemals ausblenden -- auch nicht von der
// Ashley-Wildcard. Leons Taschenlampe hat uns das schon einmal gekostet
// ("FL-GO weg, nur der Kegel da").
const std::unordered_set<std::string> RE4VRMaterials::LAMP_GO_NEVER_HIDE = {
    "ac0000_00",   // Leons Taschenlampe
    "ac0300_00",   // Ashleys Oellampe (eigener KS3-Pfad)
    "ac0301_00", "ac0302_00",
};

// ============================================================================
// Aufbau
// ============================================================================

std::shared_ptr<RE4VRMaterials>& RE4VRMaterials::get() {
    static auto inst = std::make_shared<RE4VRMaterials>();
    return inst;
}

const std::unordered_set<std::string>* RE4VRMaterials::materials_for_body(
    const std::string& body_name) const {
    // Lua Z.98-102 + Z.1017 (`or HIDE_MATERIALS_LEON`).
    if (body_name == "ch0a0z0_body") {
        return &HIDE_MATERIALS_LEON;
    }

    if (body_name == "ch0a1z0_body") {
        return &HIDE_MATERIALS_ASHLEY;
    }

    if (body_name == "ch3a8z0_body") {
        return &HIDE_MATERIALS_ADA;
    }

    return &HIDE_MATERIALS_LEON;
}

std::optional<std::string> RE4VRMaterials::on_initialize() {
    m_scene_td = sdk::find_type_definition("via.SceneManager");

    // [NIL-TYPE-GUARD] Fehlt ein Typ, bleibt er nullptr und wird ueberall
    // uebersprungen -- ein getComponent(nil) wirft eine Game-Exception, die
    // REFramework bei JEDEM der ~400 Baumknoten auf die Platte schreibt und
    // damit den Script-Thread abwuergt. via.render.SkinnedMesh existiert in RE4
    // gar nicht, war genau deshalb einmal die Ursache einer Log-Flut.
    m_t_mesh = re4vr::runtime_type("via.render.Mesh");
    m_t_skin = re4vr::runtime_type("via.render.SkinnedMesh");
    m_t_oillamp = re4vr::runtime_type("chainsaw.OilLampController");
    m_t_fur = re4vr::runtime_type("via.render.Fur");
    m_t_shellfur = re4vr::runtime_type("via.render.ShellFurMesh");

    // [REF] Die System.Type-Objekte werden ueber die gesamte Laufzeit gehalten.
    // Dauerhaft gehalten, nie freigegeben -- deshalb hier ohne reffed-Flag.
    for (auto* t : {m_t_mesh, m_t_skin, m_t_oillamp, m_t_fur, m_t_shellfur}) {
        (void)keep(t);
    }

    gondola_load();
    return Mod::on_initialize();
}

void RE4VRMaterials::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRMaterials", "on_lua_state_created");
    // Nichts zu exportieren -- diese Datei schreibt nur __re4_fur_dbg, und das
    // passiert im Frame (apply_extra_mats).
}

void RE4VRMaterials::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRMaterials", "on_lua_state_destroyed");
    // Lua Z.1002: re.on_script_reset -> gondola_unhide.
    gondola_unhide();

    // Darueber hinaus das, was in Lua das NEU-AUSFUEHREN der Datei erledigt:
    // saemtliche Caches und Zustaende entstehen dort frisch. Ohne das ueberlebt
    // z.B. ein toter Mesh-Cache den Reset (dieselbe Falle wie in RE4VRHolster).
    clear_hits();
    clear_scan_list();    // [K2]
    clear_scan_stack();   // [K1]
    m_st_w.clear();
    m_st_a.clear();
    m_scan_busy = false;
    m_force_full = true;
    m_last_body_addr = 0;
    m_have_last_body_addr = false;

    m_sig_init = false;

    clear_extra_cache();
    m_extra_scan_t = 0.0;

    store_cached_body(nullptr);
    store_lamp_go(nullptr);
    store_fl_go(nullptr);
    m_lamp_hidden = false;
    m_fl_mesh_hidden = false;

    m_mat_set_enable = false;
    m_fp_only_now = false;
    m_scope_body_hide = false;
    m_holster_hide_now = false;
    m_ashley_now = false;
    m_choke_victim = 0;
    m_hide_materials = &HIDE_MATERIALS_LEON;

    m_gondola_next_t = 0.0;
    gondola_load();
}

// ============================================================================
// Referenzzaehlung
//
// [REF] Was ueber Frames hinweg gehalten wird, braucht eine eigene Referenz --
// in Lua erledigt das sol beim Ablegen im Table (add_ref, aber nur wenn
// referenceCount > 0). Ohne das zeigt der Cache nach einem Savegame-Load auf
// freigegebenen Speicher.
// ============================================================================

bool RE4VRMaterials::keep(::REManagedObject* o) {
    if (o == nullptr || !re4vr::obj_ok(o)) {
        return false;
    }

    // Dieselbe Heuristik wie sol_lua_push: "local objects" mit
    // referenceCount <= 0 werden NICHT geref't.
    if (static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        return true;
    }

    return false;
}

void RE4VRMaterials::drop(::REManagedObject* o, bool reffed) {
    // Nur zurueckgeben, was wir auch genommen haben -- sonst Refcount-Unterlauf
    // und Use-after-free beim naechsten Zugriff.
    if (!reffed || o == nullptr || !re4vr::obj_ok(o)) {
        return;
    }

    utility::re_managed_object::release(o);
}

void RE4VRMaterials::clear_hits() {
    for (auto& h : m_hits) {
        drop(h.go, h.go_reffed);
        drop(h.mesh, h.mesh_reffed);
        drop(h.skin, h.skin_reffed);
    }

    m_hits.clear();
}

// [K2] Eine halbfertige Runde wird verworfen, wenn ein Zustandswechsel mitten
// hinein eine volle Runde erzwingt (KS-Uebergang, Choke-Flanke, Bodywechsel).
// Ihre Eintraege sind in classify_go schon geref't -- in Lua sammelt der GC die
// Tabelle ein, nativ muessen wir die Referenzen selbst zurueckgeben, sonst
// leckt JEDER Zustandswechsel bis zu drei Referenzen je klassifiziertem GO.
// [K1] Den Suchstapel leeren und die gehaltenen Referenzen zurueckgeben.
// In Lua erledigt das `st_tf[st_n] = nil` beim Abarbeiten bzw. das Neuanlegen
// der Tabelle in scan_start.
void RE4VRMaterials::clear_scan_stack() {
    for (size_t i = 0; i < m_st_tf.size(); ++i) {
        drop(m_st_tf[i], m_st_tf_reffed[i] != 0);
    }

    m_st_tf.clear();
    m_st_tf_reffed.clear();
}

void RE4VRMaterials::clear_scan_list() {
    for (auto& h : m_scan_list) {
        drop(h.go, h.go_reffed);
        drop(h.mesh, h.mesh_reffed);
        drop(h.skin, h.skin_reffed);
    }

    m_scan_list.clear();
}

void RE4VRMaterials::store_cached_body(::REManagedObject* go) {
    if (m_cached_body == go) {
        return;
    }

    drop(m_cached_body, m_cached_body_reffed);
    m_cached_body = go;
    m_cached_body_reffed = keep(m_cached_body);
}

void RE4VRMaterials::store_lamp_go(::REManagedObject* go) {
    if (m_lamp_go_cache == go) {
        return;
    }

    drop(m_lamp_go_cache, m_lamp_go_reffed);
    m_lamp_go_cache = go;
    m_lamp_go_reffed = keep(m_lamp_go_cache);
}

void RE4VRMaterials::store_fl_go(::REManagedObject* go) {
    if (m_fl_go_cache == go) {
        return;
    }

    drop(m_fl_go_cache, m_fl_go_reffed);
    m_fl_go_cache = go;
    m_fl_go_reffed = keep(m_fl_go_cache);
}

void RE4VRMaterials::clear_extra_cache() {
    for (auto& e : m_extra_mats) {
        drop(e.mesh, e.reffed);
    }

    m_extra_mats.clear();

    for (auto& f : m_extra_furs) {
        drop(f.obj, f.reffed);
    }

    m_extra_furs.clear();
    m_extra_mats_off = false;
}

// ============================================================================
// Szene und Body
// ============================================================================

::REManagedObject* RE4VRMaterials::get_scene() {
    // Lua Z.357-364
    const auto sm = sdk::get_native_singleton("via.SceneManager");

    if (sm == nullptr || m_scene_td == nullptr) {
        return nullptr;
    }

    const auto method = m_scene_td->get_method("get_CurrentScene");

    if (method == nullptr) {
        return nullptr;
    }

    ::REManagedObject* scene = nullptr;

    try {
        scene = method->call_safe<::REManagedObject*>(sdk::get_thread_context(), sm);
    } catch (...) {
        scene = nullptr;
    }

    re4vr::clear_vm_exception();
    return scene;
}

::REManagedObject* RE4VRMaterials::get_body_go() {
    // Lua Z.366-399.
    // Primaer: CharacterManager -> der AKTUELL GESTEUERTE Body. Wichtig, falls
    // Leon und Ashley gleichzeitig in der Szene sind (Escort): so treffen wir
    // den Player, nicht den NPC.
    if (auto* cm = re4vr::character_manager()) {
        auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");
        auto* body = (ctx != nullptr)
                         ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
                         : nullptr;

        if (body != nullptr) {
            const auto name = re4vr::obj_name(body);

            // [MERCS] Die Namensliste kannte nur Leon/Ashley/Ada. In
            // Mercenaries heisst der Body ch6i0z0_body ... ch6i5z0_body -- ohne
            // das Muster lieferte diese Funktion dort nil und materials
            // verwaltete GAR NICHTS. Der ADA_NPC_SCHUTZ bleibt unberuehrt: der
            // betrifft nur den Fallback unten.
            const bool mercs = name.size() >= 5 && name.compare(0, 4, "ch6i") == 0
                               && name[4] >= '0' && name[4] <= '9';

            if (!name.empty() && (PLAYER_BODY_NAMES.count(name) > 0 || mercs)) {
                return body;
            }
        }
    }

    // Fallback: Szene nach den bekannten Player-Body-Namen absuchen.
    auto* scene = get_scene();

    if (scene != nullptr) {
        for (const auto& nm : FALLBACK_BODY_NAMES) {
            auto* str = sdk::VM::create_managed_string(utility::widen(nm));

            if (str == nullptr) {
                continue;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(
                scene, "findGameObject(System.String)", str);

            if (go == nullptr) {
                continue;
            }

            bool valid = false;

            if (re4vr::try_call<bool>(go, "get_Valid", valid) && valid) {
                return go;
            }
        }
    }

    return nullptr;
}

::REManagedObject* RE4VRMaterials::get_body_go_cached() {
    // Lua Z.846-862. WICHTIG: hier laeuft get_body_go() KOMPLETT, also
    // Primaerpfad PLUS Szenen-Fallback. Erst wenn auch der leer bleibt, greift
    // der Cache.
    //
    // Jeden Frame neu bestimmen: bei Charakterwechsel Leon<->Ashley folgt der
    // Hide SOFORT dem gespielten Character, auch wenn BEIDE Bodies gueltig in
    // der Szene sind (Escort). Sonst blieb der Cache am alten (NPC-)Body
    // haengen und dessen Kopf/Haare fehlten statt der des Players.
    if (auto* fresh = get_body_go()) {
        store_cached_body(fresh);
        return fresh;
    }

    if (m_cached_body != nullptr) {
        bool valid = false;

        if (re4vr::try_call<bool>(m_cached_body, "get_Valid", valid) && valid) {
            return m_cached_body;
        }

        store_cached_body(nullptr);
    }

    return nullptr;
}

// ============================================================================
// Renderer-Setter
// ============================================================================

void RE4VRMaterials::set_mat(::REManagedObject* renderer, int32_t mi, bool enable) {
    // Lua Z.402-407: BEIDE Aufrufformen. Die kurze kann bei ueberladenen
    // Methoden die falsche Ueberladung treffen und dann lautlos nichts tun --
    // die signierte steht bewusst DANACH und hat damit das letzte Wort.
    //
    // Die Signatur enthaelt KEIN Leerzeichen nach dem Komma. Die aeltere
    // Portierung im Nachbar-Fork hat dort eines stehen; das trifft nicht.
    re4vr::call_safe<void*>(renderer, "setMaterialsEnable", mi, enable);
    re4vr::call_safe<void*>(renderer, "setMaterialsEnable(System.Int32,System.Boolean)", mi, enable);
}

void RE4VRMaterials::set_mesh_draw(::REManagedObject* renderer, bool draw_color, int shadow) {
    // Lua Z.410-413. draw_color=false -> unsichtbar im Bild, shadow=true ->
    // wirft trotzdem Schatten. shadow < 0 entspricht Luas nil: nicht anfassen.
    re4vr::call_safe<void*>(renderer, "set_DrawDefault", draw_color);

    if (shadow >= 0) {
        re4vr::call_safe<void*>(renderer, "set_DrawShadowCast", shadow != 0);
    }
}

RE4VRMaterials::MatInfo RE4VRMaterials::scan_renderer_mats(::REManagedObject* renderer) const {
    // Lua Z.428-441. Materialnamen EINMAL lesen statt jeden Frame -- das war
    // der teuerste Einzelposten im alten Walk. Die Namen eines Renderers
    // aendern sich zur Laufzeit nicht.
    MatInfo info{};

    if (renderer == nullptr) {
        return info;   // Lua: nil
    }

    int32_t mcount = 0;

    if (!re4vr::try_call<int32_t>(renderer, "get_MaterialNum", mcount)) {
        return info;   // Lua: type(mcount) ~= "number" -> nil
    }

    // [M3, bewusste Abweichung] Ein Mesh hat nie annaehernd so viele
    // Materialien; ein solcher Wert kommt nur von einem sterbenden Mesh, das
    // noch antwortet (Lua-Kommentar Z.161-167). Lua liefe dort in eine sehr
    // lange Schleife, nativ waere es ein bad_alloc mitten im Suchlauf.
    if (mcount > 4096) {
        return info;   // wie Lua-nil: dieser Renderer wird nicht angefasst
    }

    info.valid = true;
    info.count = mcount;
    info.hh_only = (mcount >= 1);   // mcount == 0 -> false
    const auto slots = static_cast<size_t>(mcount > 0 ? mcount : 0);
    info.hh.assign(slots, false);
    info.extra.assign(slots, false);

    for (int32_t mi = 0; mi < mcount; ++mi) {
        const auto name = mat_name(renderer, mi);
        const bool is_hh = !name.empty() && m_hide_materials->count(name) > 0;

        info.hh[static_cast<size_t>(mi)] = is_hh;
        info.extra[static_cast<size_t>(mi)] =
            !name.empty() && FULLHIDE_EXTRA_MATS.count(name) > 0;

        if (!is_hh) {
            info.hh_only = false;
        }
    }

    return info;
}

void RE4VRMaterials::hide_mats_on(::REManagedObject* renderer, bool fullhide_go,
                                  const MatInfo& minfo) {
    // Lua Z.446-506.
    if (renderer == nullptr || !minfo.valid) {
        return;
    }

    const int32_t mcount = minfo.count;

    // [SHADOW] Reines head/hair-Mesh -> Materialien AN lassen (Geometrie fuer
    // den Schatten), statt dessen mesh-weit den Farb-Pass schalten.
    if (minfo.hh_only) {
        for (int32_t mi = 0; mi < mcount; ++mi) {
            set_mat(renderer, mi, true);
        }

        if (m_fp_only_now || m_scope_body_hide) {
            set_mesh_draw(renderer, false, 1);
        } else {
            // Gameplay (mat_set_enable=false): unsichtbar + Schatten an.
            // Cutscene (mat_set_enable=true): normal sichtbar.
            set_mesh_draw(renderer, m_mat_set_enable, 1);
        }

        return;
    }

    // [SHADOW BODY] Gemischtes Body-Mesh, aber der GANZE GO soll weg: mesh-weit
    // unsichtbar ABER Schatten AN -- statt per-Material, denn
    // setMaterialsEnable(false) nahm den Schatten mit.
    if (fullhide_go && (m_fp_only_now || m_scope_body_hide)) {
        // [EXTRA_MATS IM WALK] Genau diese Materialien AUS, der Rest bleibt an.
        // Frueher wurden hier alle auf true gesetzt -- das ueberstimmte den
        // Szenen-Weg (apply_extra_mats, der nur auf der Flanke schaltet) in
        // jedem Frame, und die Jacke blieb im KS3 stehen.
        for (int32_t mi = 0; mi < mcount; ++mi) {
            set_mat(renderer, mi, !minfo.extra[static_cast<size_t>(mi)]);
        }

        set_mesh_draw(renderer, false, 1);
        return;
    }

    // Gemischtes Mesh: Farb-Pass normal an lassen, head/hair per Material.
    set_mesh_draw(renderer, true, -1);

    for (int32_t mi = 0; mi < mcount; ++mi) {
        const auto i = static_cast<size_t>(mi);

        // [EXTRA_MATS IM WALK] Gegenstueck zum Block oben: ausserhalb des
        // Voll-Ausblendens gehoeren diese Materialien IMMER sichtbar. Ohne das
        // bliebe die Jacke nach dem KS3-Austritt weg -- 'jacket' steht nicht in
        // FULLHIDE_GO_NAMES, dieser Pfad faende sie also nie wieder an.
        if (minfo.extra[i]) {
            set_mat(renderer, mi, true);
        } else if (fullhide_go) {
            bool enable;

            if (m_fp_only_now || m_scope_body_hide) {
                enable = false;
            } else if (minfo.hh[i]) {
                enable = m_mat_set_enable;
            } else {
                enable = true;
            }

            set_mat(renderer, mi, enable);
        } else if (minfo.hh[i]) {
            set_mat(renderer, mi, m_mat_set_enable);
        }
        // KEIN else -- Materialien, die weder extra noch (bei Nicht-Fullhide)
        // head/hair sind, werden bewusst gar nicht geschrieben (Lua Z.486-505).
    }
}

// ============================================================================
// Namensregeln
// ============================================================================

bool RE4VRMaterials::is_weapon_name(const std::string& nm) {
    // Lua Z.514-516. Jede getragene Waffe haengt als Child unter dem Body und
    // heisst "wp####" -- Namensmuster statt ID-Liste erwischt dynamisch genau
    // das aktuelle Inventar.
    return starts_wp_digit(nm);
}

bool RE4VRMaterials::is_accessory_name(const std::string& nm) const {
    // Lua Z.585-591. Reihenfolge ist verbindlich: die Lampen haben Vorrang vor
    // ALLEM, sonst erwischt die Ashley-Wildcard das handgehaltene Werkzeug.
    if (nm.empty()) {
        return false;
    }

    if (LAMP_GO_NEVER_HIDE.count(nm) > 0) {
        return false;
    }

    if (HIDE_ACCESSORY_GO.count(nm) > 0) {
        return true;
    }

    // [ASHLEY] Von ihr kennen wir die Accessoire-IDs nicht vollstaendig, und
    // ihre Sachen stoeren nur, solange man sie selbst spielt -- deshalb bei ihr
    // wieder das Muster, aber eben nur dann.
    if (m_ashley_now) {
        return is_ac_pattern(nm);
    }

    return false;
}

// ============================================================================
// Suchen
// ============================================================================

bool RE4VRMaterials::classify_go(::REManagedObject* go, const std::string& nm, bool in_weapon,
                                 bool in_accessory, Hit& out) {
    // Lua Z.612-647. Rueckgabe false = dieses GO hat gar keinen Renderer, es
    // gab hier auch vorher nichts zu tun.
    if (go == nullptr) {
        return false;
    }

    // [NIL-TYPE-GUARD] getComponent NIE mit nil-Type, s. on_initialize.
    auto* mesh = (m_t_mesh != nullptr)
                     ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_mesh)
                     : nullptr;
    auto* skin = (m_t_skin != nullptr)
                     ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_skin)
                     : nullptr;

    if (mesh == nullptr && skin == nullptr) {
        return false;
    }

    out = Hit{};
    out.go = go;
    out.mesh = mesh;
    out.skin = skin;

    // [KS4_HOLSTER] Die Holster-Klone ("vr_holster_*", per set_Parent im
    // Body-Baum). Nur das FLAG steht hier; ob sie ausgeblendet werden,
    // entscheidet holster_hide_now beim Anwenden.
    out.is_holster = nm.compare(0, 11, "vr_holster_") == 0;
    out.is_weapon = in_weapon;
    out.is_accessory = in_accessory;

    // Materialbefund nur fuer die GOs, die ueberhaupt in den Material-Pfad
    // laufen koennen. Waffen und Accessoires werden ausschliesslich ueber
    // DrawDefault geschaltet -> brauchen ihn nie.
    if (!(out.is_weapon || out.is_accessory)) {
        // [KOSTUEMFEST] Der Namenstreffer ist nur EIN Weg in fullhide: im
        // Voll-Ausblenden gilt JEDES Mesh unter dem gesteuerten Body als
        // fullhide, unabhaengig vom GO-Namen (die Namensliste war
        // kostuemabhaengig).
        out.fullhide_by_name = !nm.empty() && FULLHIDE_GO_NAMES.count(to_lower(nm)) > 0;
        out.mesh_mats = scan_renderer_mats(mesh);
        out.skin_mats = scan_renderer_mats(skin);
    }

    // [REF] Der Eintrag haelt DREI Handles ueber Frames hinweg.
    out.go_reffed = keep(out.go);
    out.mesh_reffed = keep(out.mesh);
    out.skin_reffed = keep(out.skin);

    return true;
}

void RE4VRMaterials::scan_start(::REManagedObject* root_tf) {
    // Lua Z.735-743.
    clear_scan_stack();   // [K1] gibt die gehaltenen Transforms zurueck
    m_st_w.clear();
    m_st_a.clear();
    clear_scan_list();   // [K2]
    m_scan_busy = false;

    if (root_tf == nullptr) {
        // Lua: scan_busy bleibt false -> hits wird nie ersetzt, die alte Liste
        // bleibt aktiv.
        return;
    }

    // [NUR UNSER CHARAKTER 14.09.2026 -- Ansage "bei anderen nichts
    // ausblenden"] Praefix des eigenen Bodys merken. Damit bleiben Kopf, Haare
    // und Mantel (ch0a0z0_head, ...) drin -- sie teilen das Praefix -- und
    // jeder fremde Charakter faellt raus, egal ob Gegner, Begleitung oder Tier.
    m_scan_root_prefix.clear();

    {
        auto* rgo = re4vr::call_safe<::REManagedObject*>(root_tf, "get_GameObject");

        if (rgo != nullptr) {
            const std::string rn = re4vr::obj_name(rgo);

            if (rn.size() >= 5) {
                m_scan_root_prefix = rn.substr(0, 5);
            }
        }
    }

    m_st_tf.push_back(root_tf);
    m_st_tf_reffed.push_back(keep(root_tf) ? 1 : 0);
    m_st_w.push_back(0);
    m_st_a.push_back(0);
    m_scan_busy = true;
}

void RE4VRMaterials::scan_step(int budget) {
    // Lua Z.745-788. budget < 0 = bis zum Ende durchlaufen (Lua: nil).
    if (!m_scan_busy) {
        return;
    }

    int done = 0;

    while (!m_st_tf.empty()) {
        // Pruefung steht VOR dem Hochzaehlen -> exakt `budget` Knoten pro Frame.
        if (budget >= 0 && done >= budget) {
            return;
        }

        ++done;

        auto* tf = m_st_tf.back();
        const bool tf_reffed = m_st_tf_reffed.back() != 0;
        const bool in_w = m_st_w.back() != 0;
        const bool in_a = m_st_a.back() != 0;

        m_st_tf.pop_back();
        m_st_tf_reffed.pop_back();
        m_st_w.pop_back();
        m_st_a.pop_back();

        auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

        // [CHOKE] Der gehaltene Gegner (und alles unter ihm) faellt komplett
        // aus dem Suchlauf heraus -- sonst waere er im Voll-Ausblenden
        // unsichtbar, weil re4_vr_choke.lua ihn an L_Palm parentet.
        // Der done-Zaehler zaehlt uebersprungene Knoten bewusst mit.
        bool skip = false;

        if (m_choke_victim != 0 && go != nullptr) {
            if (reinterpret_cast<uintptr_t>(go) == m_choke_victim) {
                skip = true;
            }
        }

        // [NUR UNSER CHARAKTER 14.09.2026] Das Spiel haengt fremde Figuren in
        // unseren Baum (Angriffs- und Greifanimationen). Frueher fiel darunter
        // JEDES Mesh dem Voll-Ausblenden zum Opfer -- die Ausnahme oben galt
        // nur fuer den gechokten Gegner. Jetzt gilt sie fuer jeden Fremden:
        // gleiche 5 Zeichen Praefix wie unser Body = unser Teil, sonst raus.
        // Waffen (wp...) und Effekte heissen nicht wie ein Charakter und
        // bleiben davon unberuehrt.
        if (!skip && go != nullptr && !m_scan_root_prefix.empty()) {
            const std::string n_here = re4vr::obj_name(go);

            const bool wie_charakter =
                (n_here.size() >= 5)
                && ((n_here[0] == 'c' && n_here[1] == 'h' && std::isdigit(static_cast<unsigned char>(n_here[2])))
                    || n_here.compare(0, 2, "em") == 0
                    || n_here.compare(0, 2, "gm") == 0);

            if (wie_charakter && n_here.compare(0, 5, m_scan_root_prefix) != 0) {
                skip = true;
            }
        }

        if (!skip) {
            const std::string nm_here = (go != nullptr) ? re4vr::obj_name(go) : std::string{};

            bool here_weapon = in_w;

            if (!here_weapon) {
                here_weapon = is_weapon_name(nm_here);
            }

            // [ACCESSOIRE] Genau wie bei den Waffen vererbt: unter ac1300_10
            // haengen noch SwingWind_L/R, die den ac-Namen nicht mehr tragen,
            // aber dazugehoeren.
            bool here_acc = in_a;

            if (!here_acc) {
                here_acc = is_accessory_name(nm_here);
            }

            if (go != nullptr) {
                Hit e{};

                if (classify_go(go, nm_here, here_weapon, here_acc, e)) {
                    m_scan_list.push_back(e);
                }
            }

            // Kinder werden OHNE Umkehr gestapelt -- die Besuchsreihenfolge
            // dreht sich damit gegenueber der alten Rekursion um, was egal ist:
            // jedes GO wird unabhaengig von den anderen behandelt.
            auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

            while (child != nullptr) {
                m_st_tf.push_back(child);
                m_st_tf_reffed.push_back(keep(child) ? 1 : 0);
                m_st_w.push_back(here_weapon ? 1 : 0);
                m_st_a.push_back(here_acc ? 1 : 0);
                child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
            }
        }

        // [K1] Dieses Transform ist abgearbeitet -- Referenz zurueck, genau wie
        // Luas `st_tf[st_n] = nil`.
        drop(tf, tf_reffed);
    }

    // (Das jeweils abgearbeitete Transform wird am Schleifenende freigegeben,
    // s. drop(tf, tf_reffed) -- Luas `st_tf[st_n] = nil`.)
    //
    // Doppelpufferung: erst wenn der Stapel leer ist, ersetzt die neue Liste
    // die alte -- so wird nie eine halbfertige Liste angewendet und tote
    // Eintraege fallen von selbst raus.
    clear_hits();
    m_hits = std::move(m_scan_list);
    m_scan_list.clear();   // die Referenzen sind mit nach m_hits umgezogen
    m_scan_busy = false;
}

// ============================================================================
// Anwenden
// ============================================================================

void RE4VRMaterials::apply_entry(const Hit& e) {
    // Lua Z.650-695. Die Reihenfolge der Zweige ist verbindlich; jeder schreibt
    // auf Mesh UND Skin.
    auto* mesh = e.mesh;
    auto* skin = e.skin;

    // [KS4_HOLSTER] Holster-Klone in KS4 aus dem Farb-Pass nehmen. Schatten ist
    // bei ihnen ohnehin aus (holster spawnt sie mit DrawShadowCast=false).
    if (e.is_holster && m_holster_hide_now) {
        if (mesh != nullptr) {
            re4vr::call_safe<void*>(mesh, "set_DrawDefault", false);
        }

        if (skin != nullptr) {
            re4vr::call_safe<void*>(skin, "set_DrawDefault", false);
        }

        return;
    }

    // [ACCESSOIRE] Accessoires sollen im Spiel IMMER weg (auch in KS2/KS4, wo
    // der Body sichtbar bleibt) -- nur in KS1, der echten Cutscene, duerfen sie
    // an bleiben; genau das ist mat_set_enable.
    // Angefasst wird ausschliesslich DrawDefault, nie DrawSelf: das GO bleibt
    // stehen und mit ihm die Erkennung fuer __re4_infinite_reserve.
    // DrawShadowCast wird bewusst NICHT gesetzt.
    if (e.is_accessory) {
        const bool want = m_mat_set_enable;

        if (mesh != nullptr) {
            re4vr::call_safe<void*>(mesh, "set_DrawDefault", want);
        }

        if (skin != nullptr) {
            re4vr::call_safe<void*>(skin, "set_DrawDefault", want);
        }

        return;
    }

    if (e.is_weapon) {
        // [WEAPON_HIDE] NUR den Renderer-DrawDefault toggeln, nicht
        // GO-DrawSelf -- die Holster/Equip-Logik der Engine bleibt unberuehrt.
        // [SCOPE_WEAPON_SICHTBAR] Im Scope muss die Waffe sichtbar bleiben;
        // gegen unser DrawDefault kommt kein setPartsEnable an.
        const bool want = !m_fp_only_now || re4vr::lua_get_bool("vr_scope_active", false);

        if (mesh != nullptr) {
            re4vr::call_safe<void*>(mesh, "set_DrawDefault", want);
            re4vr::call_safe<void*>(mesh, "set_DrawShadowCast", true);
        }

        if (skin != nullptr) {
            re4vr::call_safe<void*>(skin, "set_DrawDefault", want);
            re4vr::call_safe<void*>(skin, "set_DrawShadowCast", true);
        }

        return;
    }

    // [KOSTUEMFEST] Ausgeblendet wird ueber DrawDefault=false +
    // DrawShadowCast=true -> unsichtbar, SCHATTEN BLEIBT.
    const bool fullhide_go = m_fp_only_now || m_scope_body_hide || e.fullhide_by_name;

    if (mesh != nullptr) {
        hide_mats_on(mesh, fullhide_go, e.mesh_mats);
    }

    if (skin != nullptr) {
        hide_mats_on(skin, fullhide_go, e.skin_mats);
    }
}

bool RE4VRMaterials::apply_hits() {
    // Lua Z.795-806. Rueckgabe false = mindestens ein GO ist gestorben (Waffe
    // abgelegt, Kostuem gewechselt, Szenenwechsel) -> die naechste Runde muss
    // eine VOLLE sein.
    // [CACHE-LEBENDTEST] Komponenten-Handles ueberleben keinen Savegame-Load.
    bool alive = true;

    for (const auto& e : m_hits) {
        bool valid = false;

        if (re4vr::try_call<bool>(e.go, "get_Valid", valid) && valid) {
            apply_entry(e);
        } else {
            alive = false;
        }
    }

    return alive;
}

bool RE4VRMaterials::state_changed() {
    // Lua Z.813-822. Aendert sich einer dieser fuenf Werte, laeuft die Suche
    // ausnahmsweise komplett in diesem einen Frame durch -- damit ist z.B. der
    // Cutscene-Eintritt sofort vollstaendig, inklusive einer Waffe, die im
    // selben Moment gezogen wurde.
    const bool ch = (m_choke_victim != 0);

    // [PROPS_MAT 16.09.2026] Die Materialliste gehoert mit in die Signatur:
    // welche Materialien als head/hair gelten, steckt gecacht in den Hits
    // (MatInfo). Ein Wechsel Kampagne <-> Mercenaries muss die Suche darum
    // komplett neu laufen lassen, sonst bliebe "Props_Mat" am alten Befund.
    if (!m_sig_init || m_fp_only_now != m_sig_fp || m_scope_body_hide != m_sig_sc
        || m_holster_hide_now != m_sig_ho || m_mat_set_enable != m_sig_ms || ch != m_sig_ch
        || m_hide_materials != m_sig_hm) {
        m_sig_init = true;
        m_sig_fp = m_fp_only_now;
        m_sig_sc = m_scope_body_hide;
        m_sig_ho = m_holster_hide_now;
        m_sig_ms = m_mat_set_enable;
        m_sig_ch = ch;
        m_sig_hm = m_hide_materials;
        return true;
    }

    return false;
}

void RE4VRMaterials::update_choke_victim() {
    // Lua Z.705-714. Herzschlag: nur solange das Choke wirklich haelt. Stirbt
    // es mitten im Griff, faellt die Markierung nach 0.2 s von selbst weg.
    const double seen = re4vr::lua_get_number("__re4_choke_seen", 0.0);

    if (seen > 0.0 && (now_clock() - seen) < 0.2) {
        m_choke_victim =
            static_cast<uintptr_t>(re4vr::lua_get_number("__re4_choke_victim", 0.0));
    } else {
        m_choke_victim = 0;
    }
}

void RE4VRMaterials::materials_tick(::REManagedObject* body, ::REManagedObject* tf) {
    // Lua Z.828-842.
    const auto addr = reinterpret_cast<uintptr_t>(body);

    // Bodywechsel (Leon <-> Ashley <-> Ada, Szenenwechsel) ueber die Adresse:
    // ein neuer Baum UND eine neue HIDE_MATERIALS-Liste -> die alten Eintraege
    // sind samt Materialbefund wertlos.
    if (!m_have_last_body_addr || addr != m_last_body_addr) {
        m_last_body_addr = addr;
        m_have_last_body_addr = true;
        m_force_full = true;
    }

    // ZWEI getrennte Pruefungen, bewusst nicht kurzgeschlossen: state_changed()
    // aktualisiert beim Aufruf die Signatur. Ein `if (addr_changed || ...)`
    // wuerde sie bei einem Bodywechsel nicht nachziehen.
    if (state_changed()) {
        m_force_full = true;
    }

    if (m_force_full) {
        m_force_full = false;
        scan_start(tf);
        scan_step(-1);   // komplette Runde JETZT
    } else {
        if (!m_scan_busy) {
            scan_start(tf);   // Dauerlauf: neue/entfernte Objekte einsammeln
        }

        scan_step(SCAN_NODES_PER_FRAME);
    }

    if (!apply_hits()) {
        m_force_full = true;
    }
}

// ============================================================================
// Jacke und Fell
// ============================================================================

bool RE4VRMaterials::extra_mats_alive() {
    // Lua Z.156-185. Nach Save-Load, Stage- oder Kostuemwechsel zeigt der Cache
    // auf Leichen -- und setMaterialsEnable WIRFT dabei nicht, es verpufft nur.
    // Genau daran blieb das Fell am Kragen stehen.
    if (m_extra_mats.empty()) {
        return false;
    }

    for (const auto& e : m_extra_mats) {
        if (mat_name(e.mesh, e.idx) != e.name) {
            return false;
        }

        // [LEICHENTEST] Der Materialname allein reicht NICHT: nach einem
        // Neuaufbau des Charakters lieferte das tote Mesh weiter denselben
        // Namen und get_DrawDefault weiter true/false -- der Cache galt als
        // gesund, waehrend sein GameObject schon weg war. Das GameObject ist
        // der einzige der drei Werte, der den Tod nicht ueberlebt.
        if (re4vr::call_safe<::REManagedObject*>(e.mesh, "get_GameObject") == nullptr) {
            return false;
        }
    }

    // [FUR] Die Fur-Komponenten haben keinen Materialnamen zum Vergleichen --
    // ein Getter reicht als Lebendtest, er schlaegt an einer Leiche fehl.
    for (auto& f : m_extra_furs) {
        bool dummy = false;

        if (!re4vr::try_call<bool>(f.obj, "get_DrawDefault", dummy)) {
            return false;
        }

        if (re4vr::call_safe<::REManagedObject*>(f.obj, "get_GameObject") == nullptr) {
            return false;
        }
    }

    return true;
}

void RE4VRMaterials::scan_extra_mats() {
    // Lua Z.187-256.
    // Cache verwerfen, sobald er nicht mehr traegt. Die Zeitsperre wird HIER
    // mit zurueckgesetzt -- sonst stuende das Fell bis zu 5 s sichtbar da, weil
    // der Szenen-Scan noch in der Drosselung des letzten Laufs haengt.
    if (!m_extra_mats.empty() && !extra_mats_alive()) {
        clear_extra_cache();
        m_extra_scan_t = 0.0;
    }

    if (!m_extra_mats.empty()) {
        return;
    }

    if ((now_clock() - m_extra_scan_t) < 5.0) {
        return;   // Szenen-Scan ist teuer -> gedrosselt
    }

    m_extra_scan_t = now_clock();

    auto* scene = get_scene();

    if (scene == nullptr || m_t_mesh == nullptr) {
        return;
    }

    // [NUR LEONS JACKE 13.09.2026 -- Ansage "beim Damage/KS5 verschwinden auch
    // Meshes von Gegnern"] Der Scan unten laeuft ueber die GANZE Szene und nahm
    // jedes Mesh mit, dessen Material Jacket_Mat/JacketFur_Mat heisst -- und
    // genau die tragen auch Ganados. Im Voll-Ausblenden (KS3/KS5) gingen ihre
    // Jacken deshalb mit aus.
    //
    // Dieselbe Lehre steht seit dem Fur-Block weiter unten schon da ("via.
    // render.Fur haengt auch an Gegnern"); der Mesh-Scan hatte sie noch nicht.
    //
    // Gegatet wird auf den SPIELER-Body -- get_body_go_cached liefert ihn in
    // der Kampagne (ch0a0z0_body) genauso wie in Mercenaries (ch6i?z0_body),
    // die Namen muessen hier also nirgends stehen. Ohne Body wird gar nicht
    // gesammelt: lieber keine Jacke ausblenden als eine fremde.
    auto* body_go = get_body_go_cached();
    auto* body_tf = body_go != nullptr
        ? re4vr::call_safe<::REManagedObject*>(body_go, "get_Transform") : nullptr;

    if (body_tf == nullptr) {
        return;
    }

    // Haengt das Mesh im Baum des Spielers? Acht Ebenen reichen: die Jacke
    // sitzt am Body-GO selbst oder ein bis zwei Kinder tiefer.
    const auto gehoert_dem_spieler = [&](::REManagedObject* mesh) {
        auto* go = re4vr::call_safe<::REManagedObject*>(mesh, "get_GameObject");

        if (go == nullptr) {
            return false;
        }

        if (go == body_go) {
            return true;
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

        for (int depth = 0; depth < 8 && tf != nullptr; ++depth) {
            if (tf == body_tf) {
                return true;
            }

            tf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
        }

        return false;
    };

    auto* arr = re4vr::call_safe<sdk::SystemArray*>(scene, "findComponents(System.Type)", m_t_mesh);

    if (arr == nullptr) {
        return;
    }

    std::vector<::REManagedObject*> list;

    try {
        list = arr->get_elements();
    } catch (...) {
        return;
    }

    std::vector<ExtraMat> found;

    for (auto* mesh : list) {
        if (mesh == nullptr) {
            continue;
        }

        // [NUR LEONS JACKE 13.09.2026] Fremde Traeger derselben Materialnamen
        // (Ganados) fallen hier heraus.
        if (!gehoert_dem_spieler(mesh)) {
            continue;
        }

        int32_t n = 0;

        if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", n)) {
            n = 0;
        }

        for (int32_t i = 0; i < n; ++i) {
            const auto mn = mat_name(mesh, i);

            if (!mn.empty() && FULLHIDE_EXTRA_MATS.count(mn) > 0) {
                found.push_back(ExtraMat{mesh, i, mn});
            }
        }
    }

    // Ein leeres Ergebnis laesst den Cache nil UND die 5-s-Sperre stehen.
    if (!found.empty()) {
        for (auto& e : found) {
            e.reffed = keep(e.mesh);
        }

        m_extra_mats = std::move(found);
        }

    // [FUR] Zu jedem gefundenen Jacken-Mesh die Fur-Komponenten einsammeln.
    // Gesucht wird NUR LOKAL: am GO des Meshes, an dessen Eltern-GO und an
    // dessen direkten Kindern -- via.render.Fur haengt auch an Gegnern, ein
    // Szenen-Scan wuerde im Damage-Moment fremde Felle mit ausblenden.
    // Doppelte Eintraege sind unkritisch: set_DrawDefault zweimal zu setzen
    // kostet nichts.
    if (!m_extra_mats.empty() && m_extra_furs.empty()) {
        std::vector<RefHandle> furs;

        const auto take = [&](::REManagedObject* go) {
            if (go == nullptr) {
                return;
            }

            for (auto* t : {m_t_fur, m_t_shellfur}) {
                if (t == nullptr) {
                    continue;   // [NIL-TYPE-GUARD]
                }

                auto* c = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", t);

                if (c != nullptr) {
                    furs.push_back(RefHandle{c, false});
                }
            }
        };

        for (const auto& e : m_extra_mats) {
            auto* go = re4vr::call_safe<::REManagedObject*>(e.mesh, "get_GameObject");

            if (go == nullptr) {
                continue;
            }

            take(go);

            auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

            if (tf == nullptr) {
                continue;
            }

            auto* par = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");

            if (par != nullptr) {
                take(re4vr::call_safe<::REManagedObject*>(par, "get_GameObject"));
            }

            auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

            while (ch != nullptr) {
                take(re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject"));
                ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
            }
        }

        if (!furs.empty()) {
            for (auto& f : furs) {
                f.reffed = keep(f.obj);
            }

            m_extra_furs = std::move(furs);
                }
    }
}

void RE4VRMaterials::apply_extra_mats(bool off) {
    // Lua Z.262-290. Im AUS-Zustand wird jeden Tick nachgedrueckt, nicht nur
    // auf der Flanke: baut die Engine das Mesh neu auf (neue Runde,
    // Kostuemwechsel), waehrend unser Flag noch "aus" sagt, wuerde nie wieder
    // geschrieben. Beim Wiedereinschalten reicht die Flanke.
    if (m_extra_mats.empty() && m_extra_furs.empty()) {
        return;
    }

    if (!off && off == m_extra_mats_off) {
        return;
    }

    // [FUR] Gleiche Regel wie bei den Meshes: nur den Farb-Pass abschalten,
    // Schattenwurf anlassen. Steht VOR der Material-Schleife, weil die bei
    // einem toten Mesh mit return aussteigt -- sonst bliebe das Fell in genau
    // dem Fall stehen.
    for (auto& f : m_extra_furs) {
        re4vr::call_safe<void*>(f.obj, "set_DrawDefault", !off);
        re4vr::call_safe<void*>(f.obj, "set_DrawShadowCast", true);
    }

    // Nur-Fur-Fall: __re4_fur_dbg wird hier bewusst NICHT geschrieben.
    if (m_extra_mats.empty()) {
        m_extra_mats_off = off;
        return;
    }

    for (const auto& e : m_extra_mats) {
        // BEIDE Aufrufformen, s. set_mat. Der Erfolg der ERSTEN entscheidet, ob
        // das Mesh noch lebt.
        bool ok = false;
        {
            const auto method = find_method(e.mesh, "setMaterialsEnable");

            if (method != nullptr) {
                auto context = sdk::get_thread_context();

                try {
                    method->call_safe<void*>(context, e.mesh, e.idx, !off);
                    ok = true;
                } catch (...) {
                    ok = false;
                }

                ok = clear_pending(context, ok);
            }
        }

        re4vr::call_safe<void*>(e.mesh, "setMaterialsEnable(System.Int32,System.Boolean)", e.idx,
                                !off);

        if (!ok) {
            // Mesh tot -> neu suchen. extra_scan_t wird hier BEWUSST NICHT
            // zurueckgesetzt (Gegensatz zu scan_extra_mats): bis zu 5 s ohne
            // Fell, genau wie im Original.
            clear_extra_cache();
            return;
        }
    }

    m_extra_mats_off = off;

    // [FUR-DIAG] NUR Export, kein Verhalten. Hat aktuell keinen Konsumenten
    // (der Wegwerf-Logger ist geloescht), bleibt aber 1:1 erhalten, weil es
    // nichts kostet. re4_vr_merc.lua schreibt dieselbe Global mit
    // quelle = "mercs" -- deshalb steht hier "kampagne".
    if (auto lua = materials_lua_state()) {
        try {
            // [1:1] Bei JEDEM Durchlauf eine komplett neue Tabelle -- genau wie
            // Lua Z.289. Ein frueherer Sparweg patchte nur das off-Feld der
            // vorgefundenen Tabelle; re4_vr_merc.lua schreibt dieselbe Global
            // aber mit quelle = "mercs", und der Port haette damit fortlaufend
            // MERCS' Tabelle veraendert statt der eigenen.
            sol::table t = lua->create_table();
            t["off"] = m_extra_mats_off;
            t["quelle"] = "kampagne";

            sol::table mats = lua->create_table();

            for (size_t i = 0; i < m_extra_mats.size(); ++i) {
                sol::table entry = lua->create_table();
                entry["mesh"] = m_extra_mats[i].mesh;
                entry["idx"] = m_extra_mats[i].idx;
                entry["name"] = m_extra_mats[i].name;
                mats[i + 1] = entry;
            }

            t["mats"] = mats;

            sol::table furs = lua->create_table();

            for (size_t i = 0; i < m_extra_furs.size(); ++i) {
                furs[i + 1] = m_extra_furs[i].obj;
            }

            t["furs"] = furs;

            (*lua)["__re4_fur_dbg"] = t;
        } catch (...) {
            // Diagnose darf nie etwas kaputtmachen.
        }
    }
}

// ============================================================================
// Lampen
// ============================================================================

::REManagedObject* RE4VRMaterials::get_lamp_go() {
    // Lua Z.866-887. GO ueber Frames cachen (findComponents nur wenn ungueltig).
    if (m_lamp_go_cache != nullptr) {
        bool valid = false;

        if (re4vr::try_call<bool>(m_lamp_go_cache, "get_Valid", valid) && valid) {
            return m_lamp_go_cache;
        }

        store_lamp_go(nullptr);
    }

    auto* scene = get_scene();

    if (scene == nullptr || m_t_oillamp == nullptr) {
        return nullptr;
    }

    auto* comps =
        re4vr::call_safe<sdk::SystemArray*>(scene, "findComponents(System.Type)", m_t_oillamp);

    if (comps == nullptr) {
        return nullptr;
    }

    size_t n = 0;

    try {
        n = comps->get_size();
    } catch (...) {
        n = 0;
    }

    if (n > 0) {
        ::REManagedObject* c = nullptr;

        try {
            c = comps->get_element(0);
        } catch (...) {
            c = nullptr;
        }

        if (c != nullptr) {
            store_lamp_go(re4vr::call_safe<::REManagedObject*>(c, "get_GameObject"));
        }
    }

    return m_lamp_go_cache;
}

::REManagedObject* RE4VRMaterials::get_flashlight_go() {
    // Lua Z.892-908. Fallback, wenn motions __re4_fl_mesh nicht gecacht ist.
    // Sucht FEST unter "ch0a0z0_body" -- greift also nur bei Leon (1:1 wie
    // motions fl_find) -- und geht ueber Transform:find, nicht ueber
    // findGameObject.
    if (m_fl_go_cache != nullptr) {
        bool valid = false;

        if (re4vr::try_call<bool>(m_fl_go_cache, "get_Valid", valid) && valid) {
            return m_fl_go_cache;
        }

        store_fl_go(nullptr);
    }

    auto* scene = get_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    auto* body_str = sdk::VM::create_managed_string(L"ch0a0z0_body");

    if (body_str == nullptr) {
        return nullptr;
    }

    auto* body =
        re4vr::call_safe<::REManagedObject*>(scene, "findGameObject(System.String)", body_str);

    if (body == nullptr) {
        return nullptr;
    }

    auto* btf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    if (btf == nullptr) {
        return nullptr;
    }

    auto* fl_str = sdk::VM::create_managed_string(L"ac0000_00");

    if (fl_str == nullptr) {
        return nullptr;
    }

    auto* fltf = re4vr::call_safe<::REManagedObject*>(btf, "find", fl_str);

    if (fltf == nullptr) {
        return nullptr;
    }

    store_fl_go(re4vr::call_safe<::REManagedObject*>(fltf, "get_GameObject"));
    return m_fl_go_cache;
}

::REManagedObject* RE4VRMaterials::fl_mesh_now() {
    // Lua Z.1119-1124. Motions gecachten Mesh-Handle bevorzugen; ist der beim
    // Event-Eintritt nil (motion war dormant), die FL selbst finden.
    if (auto* flm = re4vr::lua_get_pointer("__re4_fl_mesh")) {
        return flm;
    }

    auto* go = get_flashlight_go();

    if (go == nullptr || m_t_mesh == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_mesh);
}

// ============================================================================
// Gondeln
// ============================================================================

void RE4VRMaterials::gondola_load() {
    // Lua Z.931-937: nur uebernehmen, wenn wirklich ein Boolean drinsteht.
    m_gondola_hide_cfg = true;

    try {
        const auto d = re4vr::json_load(CFG_PATH);

        if (d.is_object() && d.contains("gondola_hide") && d["gondola_hide"].is_boolean()) {
            m_gondola_hide_cfg = d["gondola_hide"].get<bool>();
        }
    } catch (...) {
        m_gondola_hide_cfg = true;
    }
}

void RE4VRMaterials::gondola_save() {
    nlohmann::json j;
    j["gondola_hide"] = m_gondola_hide_cfg;
    re4vr::json_save(CFG_PATH, j);
}

std::optional<int32_t> RE4VRMaterials::gondola_stage() {
    // Lua Z.943-947. ANDERE Quelle als die Overrides im Frame: die lesen
    // killswitch.get_stage_name(), hier laeuft es ueber den PlayerContext.
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return std::nullopt;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return std::nullopt;
    }

    int32_t stage = 0;

    if (!re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", stage)) {
        return std::nullopt;
    }

    return stage;
}

void RE4VRMaterials::gondola_set_tree(::REManagedObject* tf, bool enable) {
    // Lua Z.949-962. Rekursiv, fasst nur via.render.Mesh an. Wird
    // ausschliesslich mit enable = false gerufen.
    if (tf == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
    auto* mesh = (go != nullptr && m_t_mesh != nullptr)
                     ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_mesh)
                     : nullptr;

    if (mesh != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Enabled", enable);

        const auto it = std::find_if(m_gondola_hidden.begin(), m_gondola_hidden.end(),
                                     [mesh](const RefHandle& r) { return r.obj == mesh; });

        if (enable) {
            if (it != m_gondola_hidden.end()) {
                drop(it->obj, it->reffed);
                m_gondola_hidden.erase(it);
            }
        } else if (it == m_gondola_hidden.end()) {
            RefHandle r{mesh, false};
            r.reffed = keep(mesh);
            m_gondola_hidden.push_back(r);
        }
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

    while (child != nullptr) {
        gondola_set_tree(child, enable);
        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }
}

void RE4VRMaterials::gondola_unhide() {
    // Lua Z.964-967. Es wird exakt das zurueckgesetzt, was WIR versteckt haben
    // -- es kann nichts unsichtbar kleben bleiben.
    for (auto& r : m_gondola_hidden) {
        re4vr::call_safe<void*>(r.obj, "set_Enabled", true);
        drop(r.obj, r.reffed);
    }

    m_gondola_hidden.clear();
}

void RE4VRMaterials::gondola_tick() {
    // Lua Z.969-989. Die Drossel wird VOR jedem moeglichen Early-Return
    // gesetzt: auch ein fehlender GimmickManager verbraucht den Slot.
    const double now = now_clock();

    if (now < m_gondola_next_t) {
        return;
    }

    m_gondola_next_t = now + 1.0;

    const auto stage = gondola_stage();

    if (stage.has_value() && *stage == GONDOLA_STAGE && m_gondola_hide_cfg) {
        // Quelle: chainsaw.GimmickManager._MoveArray (bewegliche Gimmicks) --
        // kein Scene-Walk ueber die Map.
        auto* gm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.GimmickManager");

        if (gm == nullptr) {
            return;
        }

        auto def = utility::re_managed_object::get_type_definition(gm);

        if (def == nullptr) {
            return;
        }

        auto* field = def->get_field("_MoveArray");

        if (field == nullptr) {
            return;
        }

        sdk::SystemArray* arr = nullptr;

        try {
            arr = field->get_data<sdk::SystemArray*>(gm, false);
        } catch (...) {
            arr = nullptr;
        }

        if (arr == nullptr) {
            return;
        }

        size_t n = 0;

        try {
            n = arr->get_size();
        } catch (...) {
            n = 0;
        }

        for (size_t i = 0; i < n; ++i) {
            ::REManagedObject* core = nullptr;

            try {
                core = arr->get_element(static_cast<int32_t>(i));
            } catch (...) {
                core = nullptr;
            }

            if (core == nullptr) {
                continue;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(core, "get_GameObject");

            if (go == nullptr) {
                continue;
            }

            const auto nm = re4vr::obj_name(go);

            // Teilstring-Suche an beliebiger Stelle, kein starts_with -- 1:1
            // wie Luas nm:find(needle, 1, true).
            // [EIGENE GONDEL BLEIBT] Die Kabine, auf der man STEHT, darf nicht
            // verschwinden -- sonst steht man sichtbar in der Luft.
            if (!nm.empty() && contains(nm, GONDOLA_PREFIX) && !contains(nm, GONDOLA_KEEP)) {
                gondola_set_tree(re4vr::call_safe<::REManagedObject*>(go, "get_Transform"), false);
            }
        }
    } else if (!m_gondola_hidden.empty()) {
        gondola_unhide();   // Stage verlassen oder Schalter aus
    }
}

// ============================================================================
// UI
// ============================================================================

void RE4VRMaterials::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRMaterials", "on_draw_ui");
    // Lua Z.991-1000.
    if (!ImGui::TreeNode("RE4VR - Materials")) {
        return;   // wie Lua: return OHNE tree_pop
    }

    ImGui::Text("-- Gondeln (Stage 60850): fremde Kabinen ausblenden --");
    ImGui::Text("Die Kabinen glitchen dort engine-seitig; Ausblenden ist die Notloesung.");

    bool v = m_gondola_hide_cfg;

    if (ImGui::Checkbox("Fremde Gondel-Meshes ausblenden (eigene bleibt)", &v)) {
        m_gondola_hide_cfg = v;
        gondola_save();
    }

    ImGui::Text("aktuell versteckt: %d Meshes", static_cast<int>(m_gondola_hidden.size()));
    ImGui::TreePop();
}

// ============================================================================
// Frame
// ============================================================================

void RE4VRMaterials::on_frame() {
    re4vr::trace("RE4VRMaterials", "on_frame");
    // Lua Z.1004-1160.
    // Luas Spielpruefung (Z.8, reframework:get_game_name() ~= "re4") ist eine
    // EINMALIGE Pruefung beim Laden, kein HMD-Test -- hier erledigt sie der
    // RE4-Build-Guard um die Registrierung in Mods.cpp.
    gondola_tick();   // [GONDELN] Stage-gated, throttled (1x/s)

    // Killswitch-Zustaende. Jede dieser Funktionen kann fehlen (das Modul
    // faellt bei einem Ladefehler auf einen No-op zurueck) -- deshalb im
    // Original je ein type(...)=="function"-Test plus pcall.
    const bool ks2 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks2", false);
    const bool ks3 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks3", false);
    const bool ks4 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks4", false);
    const bool ks5 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks5", false);

    // Body + Character FRUEH bestimmen: fuer die Material-Liste.
    auto* body = get_body_go_cached();

    if (body != nullptr) {
        const auto body_name = re4vr::obj_name(body);

        // [ADA] 3-Wege statt Ashley-Ternaer. Unbekannter Body -> Leon-Liste.
        m_hide_materials = materials_for_body(body_name);

        // [PROPS_MAT 16.09.2026] Leon in der KAMPAGNE bekommt die um
        // "Props_Mat" erweiterte Liste. Der Bodyname schliesst Ada (ch3a8z0)
        // und Ashley (ch0a1z0) aus, __re4_in_mercs die Mercenaries -- dort
        // heisst Leons Body genauso.
        if (body_name == "ch0a0z0_body"
            && re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
            m_hide_materials = &HIDE_MATERIALS_LEON_CAMPAIGN;
        }
    }

    // [KS3]+[KS5] gesamtes Mesh aus. [KS2]+[KS4] nur Head/Hair aus.
    m_fp_only_now = ks3 || ks5;
    m_holster_hide_now = ks4;   // [KS4_HOLSTER] nur die Klone, Body bleibt

    // [THROWSIGHT] Del-Lago-Harpunen-Stage: gesamtes Mesh aus wie KS3, obwohl
    // der Killswitch fuer die Kamera bewusst inaktiv bleibt.
    if (re4vr::lua_get_bool("__re4_throwsight_active", false)) {
        m_fp_only_now = true;
    }

    // [STILLZONE] Adas Gondel (Stage 60850): waehrend der Fahrt stehen motion,
    // movement und holster still -- die Arme haengen nicht mehr an den
    // Controllern, und ein steif dastehender Koerper sieht in VR schlimmer aus
    // als gar keiner. Bewusst ueber diesen Weg statt ueber einen eigenen
    // Subtree-Walk in motion: nur hier gilt jedes Mesh unter dem gesteuerten
    // Body als fullhide, und die Renderkomponenten, die nicht von
    // via.render.Mesh erben, sind mit abgedeckt.
    if (re4vr::lua_get_bool("__re4_stillzone_hide_now", false)) {
        m_fp_only_now = true;
    }

    // [LEON] Eigenes Flag, bewusst NICHT __re4_stillzone_hide_now: an dem
    // haengt in binding.lua die Eingabesperre fuer ADAS Fahrt. Leons Fahrt soll
    // voll bedienbar bleiben.
    if (re4vr::lua_get_bool("__re4_gondel_hide_now", false)) {
        m_fp_only_now = true;
    }

    // [STAGE_FULLHIDE 55300] Loren-FAHRT: gesamtes Mesh + Waffe aus, ohne
    // Killswitch/Scripte anzufassen.
    // [railcar-Gate] Eine LEITER in Stage 55300 ist AUCH KS4 -- ohne diese
    // Bedingung blendete der Override den Body die ganze Leiter hoch aus.
    if (ks4 && re4vr::lua_get_bool("__re4_railcar_mode", false)) {
        const auto st =
            re4vr::lua_module_call_number("re4vr/re4_vr_killswitch", "get_stage_name");

        if (st.has_value() && static_cast<int32_t>(*st) == 55300) {
            m_fp_only_now = true;
        }
    }

    // [EVT60874 FULLHIDE] Mid-Event-Umschalter in Stage 60874: die ersten 1.0 s
    // laufen bewusst in 3rd-Person, danach KS4 + ganzes Mesh aus. Das Flag
    // setzt der killswitch genau im Umschalt-Zweig.
    // [3RD-PERSON-TOGGLE] Steht der First-Person-Toggle auf AUS, laufen alle
    // Events bewusst in 3rd-Person -- dann MUSS der Koerper sichtbar bleiben.
    // `~= false` ist ein Tri-State: nil gilt als AN.
    if (re4vr::lua_get_bool("__re4_evt60874_fullhide", false)
        && re4vr::lua_get_tribool("__re4_ks_fp_enabled") != 0) {
        m_fp_only_now = true;
    }

    // [STAGE_FULLHIDE 60880 -- DURCHQUETSCHEN] Monitordump 14:54:34: Stage
    // 60880, KS4, Reason "ks3_gimmick", Position 64.33 / -3.66 / 237.35.
    // Eng gegatet, damit NUR dieses eine Event den Koerper ausblendet: eine
    // Leiter oder ein anderes KS4 derselben Stage bleibt unberuehrt, ebenso die
    // zweite Quetschstelle ~28 m weiter vorne.
    // Der Kommentar im Lua nennt zusaetzlich __re4_in_squeeze -- der Code
    // prueft es NICHT, und wir folgen dem Code.
    if (ks4 && re4vr::lua_get_tribool("__re4_ks_fp_enabled") != 0) {
        const auto st =
            re4vr::lua_module_call_number("re4vr/re4_vr_killswitch", "get_stage_name");
        const auto rs =
            re4vr::lua_module_call_string("re4vr/re4_vr_killswitch", "get_activating_controller");

        if (st.has_value() && static_cast<int32_t>(*st) == 60880 && rs == "ks3_gimmick") {
            auto* btf = (body != nullptr)
                            ? re4vr::call_safe<::REManagedObject*>(body, "get_Transform")
                            : nullptr;
            glm::vec3 p{};

            if (btf != nullptr && get_vec3(btf, "get_Position", p)) {
                const float dx = p.x - 64.33f;
                const float dy = p.y - (-3.66f);
                const float dz = p.z - 237.35f;

                if ((dx * dx + dy * dy + dz * dz) <= 36.0f) {
                    m_fp_only_now = true;
                }
            }
        }
    }

    // [SCOPE_BODY_HIDE] Der Koerper bleibt im Scope SICHTBAR -- die Scope-Datei
    // holt Body und Haende aktiv zurueck. Deshalb hier bewusst false; das Feld
    // bleibt fuer den Fall, dass es wieder gebraucht wird.
    m_scope_body_hide = false;

    // [EXTRA_MATS] Suchen (inkl. Lebendtest des Caches) laeuft NUR, wenn wir
    // gerade verstecken wollen -- ausserhalb kostet es nichts.
    if (m_fp_only_now || m_scope_body_hide) {
        scan_extra_mats();
    }

    apply_extra_mats(m_fp_only_now || m_scope_body_hide);

    // Head/Hair SICHTBAR nur in KS1. Gameplay + KS2..KS5 -> versteckt.
    // is_active() wird im Original als einzige Killswitch-Funktion OHNE
    // type()-Test und OHNE pcall gerufen.
    m_mat_set_enable = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false)
                       && !ks2 && !ks3 && !ks4 && !ks5;

    // [KS3_FLASHLIGHT] In KS3 ist der GANZE Body aus -> die separat in der Hand
    // gehaltene Flashlight wuerde sonst schweben. motion ist in KS3 dormant und
    // managed sie nicht mehr. Beim Austritt uebernimmt motion wieder.
    if (ks3 || ks5) {
        if (auto* flm = fl_mesh_now()) {
            re4vr::call_safe<void*>(flm, "set_Enabled", false);
            m_fl_mesh_hidden = true;
        }
    } else if (m_fl_mesh_hidden) {
        if (auto* flm = fl_mesh_now()) {
            re4vr::call_safe<void*>(flm, "set_Enabled", true);
        }

        m_fl_mesh_hidden = false;
    }

    // [ASHLEY_LAMP] In KS3 nur das ROOT-Mesh (ac0300_00) verstecken, Licht und
    // Kegel bleiben. Waehrend KS3 jeden Frame erzwingen (gegen das Re-Enable
    // des Spiels), beim Austritt EINMAL zuruecksetzen.
    // set_Enabled statt set_DrawDefault: das GO hat ParamCurveAnimator /
    // GameObjectStateController, die DrawDefault ueberschreiben.
    // Das GO wird gecacht, das Mesh bewusst NICHT -- es wird jeden Frame frisch
    // geholt.
    if (ks3 || ks5) {
        auto* go = get_lamp_go();
        auto* mesh = (go != nullptr && m_t_mesh != nullptr)
                         ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)",
                                                                m_t_mesh)
                         : nullptr;

        if (mesh != nullptr) {
            re4vr::call_safe<void*>(mesh, "set_Enabled", false);
            m_lamp_hidden = true;
        }
    } else if (m_lamp_hidden) {
        auto* go = get_lamp_go();
        auto* mesh = (go != nullptr && m_t_mesh != nullptr)
                         ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)",
                                                                m_t_mesh)
                         : nullptr;

        if (mesh != nullptr) {
            re4vr::call_safe<void*>(mesh, "set_Enabled", true);
        }

        m_lamp_hidden = false;
    }

    if (body == nullptr) {
        return;
    }

    // [ASHLEY] Einmal pro Frame statt einmal pro GO. Liest get_Name bewusst ein
    // zweites Mal, genau wie das Original.
    m_ashley_now = (re4vr::obj_name(body) == "ch0a1z0_body");

    auto* tf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    update_choke_victim();   // [CHOKE] einmal pro Frame

    // Anwenden laeuft jeden Frame ueber die Trefferliste; der teure
    // Baum-Suchlauf nur gechunked im Hintergrund -- ausser bei einem
    // Zustandswechsel, dann komplett in diesem Frame.
    materials_tick(body, tf);
}

#endif // RE4
