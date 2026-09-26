// =====================================================================
// RE4VR - Traeger-Mod fuer die nach C++ portierten RE4-VR-Lua-Scripte.
//
// Aufbau: Diese Klasse ist NUR das Gerippe. Jedes portierte Lua-Script wird ein
// eigenes Submodul (eigene .cpp/.hpp) und wird von hier aus aufgerufen -- eine
// C++-Einheit je Lua-Datei, damit die 1:1-Zuordnung erhalten bleibt:
//
//   re4_vr_recoil.lua     -> RE4VRRecoil      (fertig)
//   re4_vr_materials.lua  -> RE4VRMaterials   (offen)
//   re4_vr_movement.lua   -> RE4VRMovement    (offen)
//   re4_vr_arm_chain.lua  -> RE4VRArmChain    (offen)
//   re4_vr_motion.lua     -> RE4VRMotion      (offen)
//
// Vorbild ist praydogs RE8VR (gleicher Ordner): Mod-Basisklasse, Phasen ueber
// on_(pre_)application_entry, Lua-Anbindung ueber on_lua_state_created.
//
// WICHTIG, sonst zeichnet das Menue nichts: der Name aus get_name() muss in die
// Liste `visible_mods` in src/Mods.cpp eingetragen sein -- dieser Fork ruft
// on_draw_ui() nur fuer die dort gelisteten Mods auf.
// =====================================================================
#pragma once

#if defined(RE4)

#include <filesystem>
#include <string>
#include <vector>   // [TAUNT-WAV] taunt_wav_next nimmt den Shuffle-Beutel
#include <functional>
#include <memory>
#include <unordered_map>   // [WPOSE]

#include <json.hpp>

#include "../../../Mod.hpp"

// -----------------------------------------------------------------------------
// Gemeinsame Helfer fuer alle portierten Module.
//
// Alle sol-/Lua-Zugriffe liegen bewusst in der .cpp: ScriptRunner.hpp darf nur
// dort eingebunden werden (die sol_lua_push-Deklaration blaeht sonst das Log auf
// und kann das Spiel reissen -- so steht es woertlich ueber den Includes von
// RE8VR.cpp). Deshalb hier nur schlanke Deklarationen ohne sol im Header.
// -----------------------------------------------------------------------------
namespace sol {
class state;
}

namespace re4vr {

// [ABSTURZ 04.09.2026] Zugriff auf den Lua-State NUR ueber dieses RAII-Objekt.
//
// Der ScriptRunner nimmt fuer jeden Lua-Callback zwei rekursive Sperren
// (m_access_mutex + je State m_execution_mutex). Unsere Ports griffen ohne
// beide auf denselben State zu -- und on_pre_gui_draw_element sowie die
// GUI-Hooks laufen im RENDER-Thread. Folge: Zugriffsverletzung in der Lua-VM
// und sol-Panic bis abort(). LuaRef sperrt im Konstruktor und gibt im
// Destruktor wieder frei; die Sperre haelt also ueber die GANZE Nutzung.
class LuaRef {
public:
    LuaRef();
    ~LuaRef();

    LuaRef(const LuaRef&) = delete;
    LuaRef& operator=(const LuaRef&) = delete;

    sol::state& operator*() const { return *m_state; }
    sol::state* operator->() const { return m_state; }
    explicit operator bool() const { return m_state != nullptr; }

    bool operator==(std::nullptr_t) const { return m_state == nullptr; }
    bool operator!=(std::nullptr_t) const { return m_state != nullptr; }

private:
    sol::state* m_state = nullptr;
    bool m_locked = false;
};

// =====================================================================
// [VM_SAFE 2026-08-29 -- CRASH-URSACHE, teuer bezahlt] Jeder Managed-Call MUSS
// hierueber laufen, nicht ueber sdk::call_object_func_easy.
//
// Der Unterschied zu Lua, und warum der Port crashte: `obj:call(...)` in Lua
// geht durch REMethodDefinition::invoke -- das faengt eine Engine-Exception ab
// UND raeumt den Exception-Zeiger im VMContext wieder weg (RETypeDB.cpp:667).
// `sdk::call_object_func_easy` ruft dagegen den Funktionszeiger DIREKT auf
// (RETypeDB.hpp:1260, `get_function_t<...>()(args...)`) -- ohne jeden Schutz.
//
// Wirft die Engine dort eine Managed-Exception (z.B. wenn beim Enemy-Grab ein
// Objekt kurz in einem ungueltigen Zustand ist), bleibt sie im VMContext STEHEN.
// Das Spiel laeuft weiter, bis irgendein beliebiger naechster Call darueber
// stolpert -- im Log vom 29.08. war das `GetLength` aus dem SPIELCODE, mit
// "VMContext was already corrupted before this call". Der Crash passiert also
// weit entfernt von der Stelle, die ihn ausgeloest hat.
//
// Dieser Wrapper macht beides: call_safe (faengt SEH/C++-Exceptions) und danach
// den VMContext-Exception-Zeiger loeschen -- genau das, was invoke auch tut.
// Ein nullptr-Objekt oder eine fehlende Methode liefert T{} statt zu crashen,
// wie das `safe(...)`/`sc(...)` der Lua-Fassungen.
// =====================================================================
template <typename T = void*, typename... Args>
T call_safe(::REManagedObject* obj, std::string_view name, Args... args) {
    if (obj == nullptr) {
        return T{};
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return T{};
    }

    const auto method = def->get_method(name);

    if (method == nullptr) {
        return T{};
    }

    auto context = sdk::get_thread_context();

    // Haengengebliebene Managed-Exception wegraeumen -- sonst reisst sie den
    // naechsten Call der Engine mit.
    const auto clear_pending = [context]() {
        if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
            context->unkPtr->unkPtr = nullptr;
        }
    };

    // [WIRFT 2026-08-29 -- die eigentliche Crash-Ursache]
    // sdk::VMContext::safe_wrap raeumt den VMContext zwar auf, WIRFT danach aber
    // eine std::runtime_error (REContext.cpp:645 und :651). Ungefangen fliegt die
    // durch den Mod-Callback bis zum UnhandledExceptionFilter -> Spiel weg.
    //
    // Lua kennt das Problem nicht: `obj:call(...)` geht durch
    // REMethodDefinition::invoke, und das setzt intern nur `exception_thrown`
    // und gibt nil zurueck, statt zu werfen. Genau dieses Verhalten bilden wir
    // hier nach -- Fehlschlag = T{}, wie Luas nil.
    try {
        if constexpr (sizeof(T) > sizeof(void*)) {
            T out{};
            method->call_safe<T*>(&out, context, obj, args...);
            clear_pending();
            return out;
        } else {
            T result = method->call_safe<T>(context, obj, args...);
            clear_pending();
            return result;
        }
    } catch (...) {
        clear_pending();
        return T{};
    }
}

// [TRUTHY 2026-08-29] Managed-Call, der zwischen "hat false geliefert" und
// "ging nicht" unterscheidet -- genau der Unterschied, den Lua macht:
// `sc(obj,"get_DrawSelf") ~= false` ist bei einem gescheiterten Call WAHR, denn
// Lua liefert dann nil, und nil ist nicht false. Ein blosses call_safe<bool>
// macht daraus faelschlich ein false und kehrt die Bedingung um.
// Rueckgabe: true = Call lief durch (Ergebnis steht in `out`).
template <typename T, typename... Args>
bool try_call(::REManagedObject* obj, std::string_view name, T& out, Args... args) {
    if (obj == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    const auto method = def->get_method(name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    bool ok = false;

    // safe_wrap wirft bei einer Engine-Exception (s. call_safe) -- fangen, sonst
    // reisst ein einzelner fehlgeschlagener Call das Spiel.
    try {
        sdk::VMContext::safe_wrap(name, [&]() {
            out = method->get_function_t<T (*)(sdk::VMContext*, ::REManagedObject*, Args...)>()(
                context, obj, args...);
            ok = true;
        });
    } catch (...) {
        ok = false;
    }

    // Wie in call_safe: eine haengengebliebene Managed-Exception wegraeumen.
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        ok = false;
    }

    return ok;
}

// [VM_CLEAN 2026-08-29] Eine haengengebliebene Managed-Exception aus dem
// VMContext raeumen.
//
// WARUM ZENTRAL: die sdk::-Getter/Setter (shared/sdk/RETransform.cpp) rufen an
// 24 Stellen den UNGESCHUETZTEN method->call<>-Pfad. Die werfen nicht -- sie
// lassen die Exception im VMContext STEHEN. Der Kontext gehoert aber dem
// ganzen Thread, also stolpert der naechste Call darueber, und das ist
// regelmaessig ein LUA-Call: in re4_vr_choke.lua haengt `is_live()` an fuenf
// Managed-Calls hintereinander (get_Valid, get_IsEliminated, get_HitPoint,
// get_IsLive, get_CurrentHitPoint). Schlaegt einer davon fehl, ist
// held_usable() false, apply_hold() laeuft nie -- der Griff kommt gar nicht
// zustande, waehrend das set_Parent aus grab() den Gegner trotzdem an den Body
// haengt. Genau das Bild: Gegner klebt, folgt dem Yaw, Hand haelt ihn nicht.
//
// Unsere Schreibzugriffe laufen bis zu fuenfmal pro Frame -- deshalb nach JEDER
// nativen Phase aufraeumen, nicht nur im Fehlerfall.
inline void clear_vm_exception() {
    auto context = sdk::get_thread_context();

    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
    }
}

// `sc(obj, name) ~= false` aus Lua: ein gescheiterter Call zaehlt als WAHR.
inline bool call_bool_not_false(::REManagedObject* obj, std::string_view name) {
    bool v = false;
    return try_call<bool>(obj, name, v) ? v : true;
}

// ---- Lua-Globals (die Schnittstelle zu den weiterhin laufenden Scripten) ----
// Liefern `def`, wenn kein Lua-State da ist (waehrend "Reset Scripts" ist er
// kurzzeitig null) oder das Global nicht existiert. Wichtig: in Lua ist ein
// fehlendes Global `nil` und NICHT `false` -- wo das einen Unterschied macht,
// gibt es die *_tribool-Variante.
double lua_get_number(const char* name, double def);
// Wie lua_get_number, aber unterscheidet "nicht gesetzt" von einem echten Wert.
// Noetig, wo das Original `tonumber(rawget(...))` gegen nil prueft und bei nil
// einen ANDEREN Weg geht (nicht bloss einen Default nimmt).
std::optional<double> lua_get_number_opt(const char* name);
bool   lua_get_bool(const char* name, bool def);

// Dreiwertig: 1 = true, 0 = false, -1 = nicht vorhanden (nil).
// Wird z.B. fuer __vr_support_hand_docked gebraucht, wo nil den Fallback auf
// __vr_support_blend_factor ausloest.
int    lua_get_tribool(const char* name);

// Luas nackte Wahrheitspruefung `if x then`: WAHR ist alles ausser nil und
// false -- ausdruecklich auch 0 und der leere String. Wer hier lua_get_bool
// nimmt, aendert das Verhalten, sobald die Gegenseite eine Zahl schreibt.
bool lua_is_truthy(const char* name);
// Dasselbe fuer ein Tabellenfeld: Luas `if t.feld then`.
bool lua_table_is_truthy(const char* table, const char* field);

// Ruft eine parameterlose Funktion aus einem per require geladenen Lua-Modul auf
// und erwartet einen Wahrheitswert. `module` ist der require-Pfad, also z.B.
// "re4vr/re4_vr_killswitch". Fehlt das Modul oder die Funktion, kommt `def`.
bool   lua_module_call_bool(const char* module, const char* fn, bool def);

// Dreiwertig fuer das Lua-Muster
//   if type(m.fn) == "function" then
//       local ok, v = pcall(m.fn); if not (ok and v == true) then return false end
//   end
// -> Funktion FEHLT: Pruefung uebersprungen (1). Call ok und true: 1.
//    Call wirft ODER liefert nicht-true: 0. Modul/State fehlt: -1.
// Ein einfaches lua_module_call_bool mit def=true kann das nicht abbilden --
// es wuerfe "Call geworfen" und "Funktion fehlt" in denselben Topf.
int    lua_module_call_tribool(const char* module, const char* fn);

// Wie oben, aber fuer Zahl bzw. Zeichenkette. Ohne Wert (Modul/Funktion fehlt,
// Fehler im Lua-Code, falscher Typ) kommt std::nullopt bzw. ein leerer String.
std::optional<double> lua_module_call_number(const char* module, const char* fn);
std::string           lua_module_call_string(const char* module, const char* fn);

// Ein von Lua gehaltenes Engine-Objekt lesen (z.B. das von motion gecachte
// __re4_fl_mesh). REManagedObject ist im ScriptRunner als sol-Usertype
// registriert, deshalb ist das ein normaler get<>.
::REManagedObject* lua_get_pointer(const char* name);

// Eine globale LUA-FUNKTION rufen, die ein MANAGED OBJECT liefert.
// Gebraucht, solange Geber und Nehmer gemischt sind: __re4_find_knife_hc und
// __re4_knife_get_attack_ud stehen in weapons.lua (noch Lua), gelesen werden
// sie aus RE4VRWeapons2 (nativ). Faellt weg, sobald weapons portiert ist.
::REManagedObject* lua_call_global_obj(const char* name);
::REManagedObject* lua_call_global_obj_arg(const char* name, ::REManagedObject* arg);

// __re4_nearest_bone_dist(transform, vec3) -> Zahl. Liefert nullopt, wenn es
// die Funktion nicht gibt oder sie nichts Brauchbares zurueckgibt.
std::optional<double> lua_call_bone_dist(const char* name, ::REManagedObject* tf,
                                         const glm::vec3& pos);

// _G.<name> = { go = <obj>, pos = <vec3>, t = <zahl> }
// Fuer __re4_knife_last_hit: die Flugmaschine in weapons.lua leitet daraus ab,
// in wen sie das geworfene Messer stecken soll. Faellt weg, sobald weapons
// portiert ist.
void lua_set_hit_record(const char* name, ::REManagedObject* go, const glm::vec3& pos,
                        double t);

// Luas tostring(<managed object>) -- die Form, in der reload.lua die
// Munitionssorte vergleicht. Geht ueber den Lua-State, damit exakt dieselbe
// Zeichenkette entsteht wie bisher.
std::string lua_tostring_obj(::REManagedObject* obj);

// Eine globale Lua-Funktion mit EINEM managed-object-Argument rufen, die eine
// Zahl liefert (__re4_id_num). Faellt weg, sobald reload portiert ist.
std::optional<double> lua_call_global_num_arg(const char* name, ::REManagedObject* arg);

// [PORTFIX 2026-09-06] Einen ZAHL-Getter typrichtig lesen -- genau das, was
// REFramework fuer Lua tut (Sdk.cpp: invoke + parse_data ueber den
// Rueckgabetyp aus der TDB), und was `try_call<float>`/`try_call<int32_t>`
// NICHT tun: die lesen blind ein Register in der angenommenen Breite.
//
// Teuer bezahlt: chainsaw.HitPoint.get_CurrentHitPoint wurde in RE4VRWeapons
// als float gelesen. Kommt der Wert als Ganzzahl zurueck, steht in der
// gelesenen Stelle 0 -> `cur <= 0` -> JEDER Gegner galt als tot -> das
// Messer fand nie ein Ziel, weder im Nahkampf noch im Wurf. In Lua kann das
// nicht passieren, weil dort der TDB-Typ ueber die Auswertung entscheidet.
//
// Enums werden ueber ihren Underlying-Typ gelesen; ValueType-Rueckgaben
// (Vektoren, AABB) gehoeren NICHT hierher -- die brauchen den sret-Puffer.
// type_out (optional) nimmt den Namen des Rueckgabetyps auf -- zur Diagnose.
std::optional<double> call_num(::REManagedObject* obj, std::string_view name,
                               std::string* type_out = nullptr);


// ---------------------------------------------------------------------------
// [LUA-WEG 2026-09-06] Argumente exakt so aufbereiten wie REFrameworks
// build_args (Sdk.cpp:1200) und die Methode ueber invoke rufen -- also genau
// das, was `obj:call(name, ...)` in Lua tut.
//
// Warum das noetig ist: call_safe/try_call rufen den ROHEN Funktionszeiger mit
// geratener Signatur. Fuer GETTER geht das gut (gemessen: alle ok). Fuer
// KOMMANDOS geht es schief, weil build_args anders uebergibt als eine
// C-Aufrufkonvention:
//   * Ganzzahlen als WERT im Zeiger-Slot, nicht im Register
//   * Fliesskomma als DOUBLE-BITS im Slot, nicht als float im xmm-Register
//   * via.vec3 als Zeiger auf einen Vector4f (x,y,z,0)
// Belegt am Armbrust-Dummy: `requestEnd` roh gerufen liess get_Valid auf 1
// stehen -- der Bolzen blieb in der Luft.
// ---------------------------------------------------------------------------
inline void* arg_int(int64_t v) { return reinterpret_cast<void*>(static_cast<intptr_t>(v)); }
inline void* arg_bool(bool b) { return reinterpret_cast<void*>(static_cast<intptr_t>(b ? 1 : 0)); }

inline void* arg_num(double d) {
    intptr_t n{};
    std::memcpy(&n, &d, sizeof(n));

    return reinterpret_cast<void*>(n);
}

// Rueckgabe: true = Aufruf lief durch (keine Engine-Exception).
bool call_cmd(::REManagedObject* obj, std::string_view name, std::span<void*> args = {});

// Eine Zeichenkette lesen (leer, wenn nicht vorhanden oder kein String).
std::string lua_get_string(const char* name);

// Globals schreiben, die andere Scripte lesen (z.B. __vr_surge_bridged,
// __re4_ub_z_delta). Ohne Lua-State passiert nichts.
void lua_set_bool(const char* name, bool value);
void lua_set_number(const char* name, double value);
void lua_set_string(const char* name, const std::string& value);
void lua_set_nil(const char* name);
void lua_set_vec3(const char* name, const glm::vec3& value);

// Eine {x,y,z}-TABELLE schreiben (nicht das Vector3f-Userdata) -- manche
// Scripte erwarten genau das, z.B. vr_knife_swing_dir.
void lua_set_vec3_table(const char* name, const glm::vec3& value);

// Eine global hinterlegte Lua-Funktion parameterlos aufrufen und einen
// Wahrheitswert erwarten. Fehlt sie, kommt `def`.
bool lua_call_global_bool(const char* name, bool def);

// Funktions-Global, das einen String liefert (__re4_char_now). Leer =
// nicht vorhanden/kein String -- der Aufrufer nimmt dann seinen Fallback.
std::string lua_call_global_string(const char* name);

// Funktion in einer Lua-TABELLE rufen (Plugin-APIs wie overlay.* aus
// re_vr.dll). Nur fuer SELTENE Aufrufe -- jeder nimmt die ScriptRunner-Sperre.
void lua_call_table_fn_bool(const char* table, const char* fn, bool arg);
void lua_call_table_fn_void(const char* table, const char* fn);

// Luas `rawget(_G, name) ~= nil` -- fuer Globals, die als VORHANDEN oder
// ABWESEND gelesen werden statt als Wert.
bool lua_has_value(const char* name);

// Wie lua_call_global_pos_radius_bool, tauscht aber fuer die Dauer GENAU
// DIESES Aufrufs die Tabelle `vals` gegen eine flache KOPIE mit geaendertem
// Zahlenfeld `field` -- und legt danach die URSPRUENGLICHE Referenz zurueck.
//
// Das muss in einem Stueck laufen: Lua baut in re4_vr_choke.lua eine echte
// Kopie (`sv_neu`) und setzt hinterher `sv_alt` zurueck, die Originaltabelle
// wird also nie angefasst. Wer stattdessen das Feld in der bestehenden Tabelle
// aendert und zurueckschreibt, laesst bei einer Tabelle OHNE dieses Feld den
// Wert dauerhaft stehen -- und fremder Lua-Code saehe waehrend des Aufrufs
// dieselbe Instanz mit unserem Wert.
bool lua_call_global_pos_radius_bool_with_field(const char* name, const glm::vec3& p, float r,
                                                const char* vals, const char* field,
                                                double value);

// Eine global hinterlegte Lua-Funktion mit (Vector3f, float) aufrufen und
// einen Wahrheitswert erwarten -- gebraucht fuer die Trefferkette aus
// re4_vr_weapons2.lua (__re4_knife_direct_damage_at). Fehlt sie, kommt false.
// Wie in Lua zaehlt NUR ein echtes true als Erfolg.
// Ohne Radius -- die Gegenseite nimmt ihren eigenen Default (nicht 0.0f).
bool lua_call_global_pos_bool(const char* name, const glm::vec3& p);
bool lua_call_global_pos_radius_bool(const char* name, const glm::vec3& p, float r);

// ---------------------------------------------------------------------------
// System.Array-Zugriff (via.Joint[], via.render.Mesh[] ...)
//
// [TEUER BEZAHLT 04.09.2026] `arr:call("get_Count")` und `arr:call("get_Item", i)`
// funktionieren auf einem System.Array NICHT -- die Methoden existieren dort
// gar nicht, und `try_call`/`call_safe` liefern stumm nichts. Genau davor warnt
// re4_vr_weapons.lua:3018 bereits: `get_elements`/`get_size`/`get_element` sind
// LUA-BINDINGS von REFramework auf das Array-Objekt, keine managed Methoden.
// Wer sie beim Portieren als Methodenaufrufe uebersetzt, bekommt IMMER 0
// Elemente -- ohne Fehler, ohne Log.
//
// Der native Weg geht ueber REArrayBase::numElements und
// utility::re_array::get_element (shared/sdk/REArray.hpp).
// ---------------------------------------------------------------------------
// [FRAMETIME-MESSUNG 04.09.2026] Wer kostet wieviel -- pro MODUL, nicht pro
// Phase (das misst der Fork schon selbst, s. Hooks.cpp m_profiling_enabled).
//
// Gebraucht fuer die Frage "woher kommen die CPU-Buckel": eine Rangliste allein
// reicht nicht, weil der Buckel selten ist und man nicht danebenstehen kann.
// Deshalb haelt der Sammler zusaetzlich den TEUERSTEN Frame fest und schreibt
// dessen komplette Verteilung nach reframework/data/re4_frametimes.txt.
//
// Der ScriptRunner ist selbst ein Mod und wird damit mitgemessen -- die zehn
// verbliebenen Lua-Dateien tauchen also als eine Zeile auf.
namespace perf {
// Schaltet die Messung an/aus. Aus = ein Vergleich pro Modul, sonst nichts.
void set_enabled(bool on);
bool enabled();

// Eine Messung fuer `name` in `phase` verbuchen (Dauer in Mikrosekunden).
void add(std::string_view name, const char* phase, double us);

// Am Ende jedes Frames: Summen abschliessen, Spitze pruefen, ggf. schreiben.
void frame_end();

// Fuer die UI: Rangliste (Name, Mittelwert us, Maximum us) nach Mittelwert.
struct Row {
    std::string name;
    double avg_us;
    double max_us;
    double last_us;
};
std::vector<Row> rows();
double frame_avg_us();
double frame_max_us();
void reset();

// RAII -- misst den umschlossenen Block.
class Scope {
public:
    Scope(std::string_view name, const char* phase);
    ~Scope();

private:
    std::string_view m_name;
    const char* m_phase;
    long long m_t0;
};
} // namespace perf

// ---------------------------------------------------------------------------
// [FRAME-CACHE] Frame-lokaler Zwischenspeicher fuer die Spieler-Kette.
//
// Ohne ihn laeuft fuer JEDES player_body_tf() die volle Strecke:
//   get_managed_singleton("CharacterManager") -> getPlayerContextRef
//   -> get_BodyGameObject -> get_Transform
// also vier Engine-Aufrufe ueber den VMContext, jeder mit Ausnahmeabsicherung.
// Das passiert in apply_hold & Co. FUENFMAL pro Frame (einmal je Phase), und
// das in jedem Modul -- Motion, Choke, Merc, Materials, Holster, ArmChain.
//
// Genau dagegen gibt es seit jeher `autorun/re4vr/re4_vr_frame_cache.lua`, das
// alle Lua-Dateien benutzen. Die Ports haben es umgangen und sich eigene
// Helfer gebaut, die jedes Mal neu fragen -- das ist der Grund, warum der
// weiter portierte puredark-Fork bessere CPU-Zeiten liefert (er hat den Cache
// als RE4VRFrameCache nativ).
//
// Der Cache haelt NICHTS ueber den Frame hinaus. `__re4_fc_off = true` schaltet
// ihn ab, dann nimmt jeder Aufrufer wieder den vollen Weg.
namespace fc {
// Frame-Wechsel erkennen und ggf. leeren. Rueckgabe: ist der Cache aktiv?
bool on();

::REManagedObject* ctx();
::REManagedObject* body_go();
::REManagedObject* body_tf();
::REManagedObject* pe();
::REManagedObject* head_go();
std::optional<int32_t> equip_wid();

// Gecachtes get_managed_singleton -- der teuerste Teil der Kette.
::REManagedObject* managed_singleton(const char* name);

// Beim Script-Reset und bei jedem Frame-Wechsel.
void reset();
} // namespace fc

// [WPOSE 2026-09-24] Handposen PRO WAFFE. Jede Stelle, die eine Handpose
// schreibt, holt ihre Bones ueber pick(name, basis): hat die ausgeruestete
// Waffe eine eigene Fassung dieser Pose, kommt die, sonst die Basis. Ablage
// in re4_vr_reload.json unter "weapon_poses" (liest/schreibt RE4VRReloadMain).
namespace wpose {
using Bones = std::unordered_map<std::string, glm::quat>;

Bones pick(const std::string& name, const Bones& base);

// Menue-Zugriff (alle Aufrufe sperren intern).
std::vector<std::string> seen(int32_t wid);                  // benutzte + eigene Namen, sortiert
std::vector<int32_t> wids_with_own();
bool own(int32_t wid, const std::string& name, Bones* out = nullptr);
void set_own(int32_t wid, const std::string& name, const Bones& b);
void drop_own(int32_t wid, const std::string& name);
bool base(const std::string& name, Bones& out);              // zuletzt gesehene Basis
std::vector<std::string> base_names();
void copy_all(int32_t from, int32_t to);   // Posen + End-Einstellung

// [END_POSE] Nach dem Einlegen (load_and_book) die linke Hand kurz an den
// Vordergriff holen (Support-Dock), `hold` s halten, ueber `out` s ausblenden.
struct EndCfg {
    bool  on{false};
    float hold{0.115f};
    float out{0.20f};
};
EndCfg end_cfg(int32_t wid);
void set_end_cfg(int32_t wid, const EndCfg& c);
nlohmann::json end_to_json();
void end_from_json(const nlohmann::json& j);

// "Erzwingen": haelt eine Pose der Waffe auf der Hand (zum Einstellen).
void set_force(int32_t wid, const std::string& name);        // leerer Name = aus
bool force(int32_t& wid, std::string& name);

nlohmann::json to_json();
void from_json(const nlohmann::json& j);
} // namespace wpose

// [BODY-EPOCH 2026-09-22] Zaehler "Spieler-Body gewechselt" (Save-Load/Tod).
// Nach dem Neuaufbau bleiben die alten Objekte ansprechbar (obj_ok true,
// Getter liefern alte Werte) -- ueber Frames gemerkte Zeiger haengen dann an
// der Leiche. Einmal je Frame wird die Adresse von fc::body_go() verglichen;
// der Zaehler steigt, wenn sie wechselt ODER der Body zwischendurch weg war
// und wiederkommt. Der allererste Body zaehlt NICHT (Start = 0, Module
// starten mit m_body_epoch 0 und verwerfen beim Start nichts).
// Modul-Muster: if (re4vr::body_epoch() != m_body_epoch) {
//                   m_body_epoch = re4vr::body_epoch(); drop_body_caches(); }
// Thread-sicher (atomar); die Pruefung laeuft nur einmal je Frame.
uint64_t body_epoch();

// Steht die Lua-VM gerade IN einem Aufruf? Gebraucht als nativer Ersatz fuer
// Luas `debug.traceback`-Test in re4_vr_merc.lua: dort wird ein nativer Zug von
// einem Aufruf aus einem unserer Scripte unterschieden. Ein Aufruf aus einem
// Lua-Callback hat einen Stackframe, ein rein nativer Zug hat keinen.
bool lua_is_executing();

int32_t array_size(::REManagedObject* arr);
::REManagedObject* array_element(::REManagedObject* arr, int32_t idx);

// Ein Engine-Objekt nach Lua durchreichen (z.B. __re4_fl_mesh). nullptr -> nil.
void lua_set_managed_object(const char* name, ::REManagedObject* obj);

// Ein Feld aus einer globalen Lua-TABELLE lesen (z.B. vr_camera_fix.active).
// Fehlt die Tabelle oder das Feld, kommt `def` bzw. std::nullopt.
bool lua_get_table_bool(const char* table, const char* field, bool def);
std::optional<glm::quat> lua_get_table_quat(const char* table, const char* field);
std::string lua_get_table_string(const char* table, const char* field);
double lua_get_table_number(const char* table, const char* field, double def);
std::optional<glm::vec3> lua_get_table_vec3(const char* table, const char* field);

// Ein VERSCHACHTELTES Zahlenfeld: _G.<table>.<sub>.<field> -- so liegt
// `__re4_reload_mag_slide.push.release_dur`.
double lua_get_table_number2(const char* table, const char* sub, const char* field, double def);

// Schreibt eine Quaternion als Global (Gegenstueck zu lua_set_vec3).
void lua_set_quat(const char* name, const glm::quat& value);

// Eine globale Lua-TABELLE sicherstellen und Felder darin schreiben. Wird fuer
// `__re4_knife_fly_cfg` und `__re4_knife_land_rot` gebraucht: die gehoeren
// motion UND weapons.lua gemeinsam, liegen also weiterhin in Lua.
void lua_ensure_table(const char* name);
bool lua_table_exists(const char* name);
void lua_set_table_number(const char* table, const char* field, double value);

// [MOTION-PORT] Luas `_G.T = _G.T or { k = default, ... }`: den Schluessel NUR
// anlegen, wenn er noch fehlt. Ein vorhandener Wert (auch 0) bleibt stehen --
// in Lua ist 0 WAHR, `or` greift ausschliesslich bei nil.
void lua_seed_table_number(const char* table, const char* field, double value);

// [MOTION-PORT] Verschachtelte Map mit ZAHLEN-Schluessel:
// `_G.map[wid] = { x = ..., y = ..., z = ... }`. Genau diese Form lesen die
// Fremd-Scripte (weapons2) mit der Waffen-ID als Index.
void lua_set_xyz_at(const char* table, int32_t key, float x, float y, float z);
// Gegenstueck: einen {x,y,z}-Eintrag mit ZAHLEN-Schluessel lesen.
// false = Tabelle oder Eintrag fehlt (Luas `m[kid]` ist dann nil).
bool lua_get_xyz_at(const char* table, int32_t key, glm::vec3& out);
// [KNIFE_HAND] Pro-Messer Links-Feinschliff aus __re4_knife_lh_off_map[wid]:
// px/py/pz (Position im Hand-Frame) und rx/ry/rz (Grad, lokal an die Waffe).
// false = kein Eintrag -> reine Spiegelung ohne Feinschliff.
bool lua_get_lh_off(int32_t wid, glm::vec3& pos, glm::vec3& rot);

// [REVOLVER COCK] __vr_rev_cock_off: px/py/pz + rx/ry/rz, gesetzt von reload2.
// false = Tabelle fehlt (Luas `type(o) == "table"`-Pruefung).
bool lua_get_cock_off(glm::vec3& pos, glm::vec3& rot);

// [REVOLVER COCK] Gegenstueck: reload2 (C++) schreibt die Tabelle. Sie wird
// angelegt, falls sie noch nicht existiert -- in Lua legte `_G.__vr_rev_cock_off
// = hammer_st.hand` sie ebenfalls jeden Frame neu.
void lua_set_cock_off(const glm::vec3& pos, const glm::vec3& rot);

// [MOTION-PORT] Gegenstuecke fuers Speichern: die Global-Tabellen wieder
// einsammeln. Rueckgabe false = Tabelle existiert nicht (Luas
// `if type(m) == "table"`), dann bleibt der bisherige JSON-Inhalt stehen.
bool lua_get_xyz_map(const char* table, nlohmann::json& out);
bool lua_get_number_map(const char* table, std::initializer_list<const char*> fields,
                        nlohmann::json& out);
// -1 = Feld fehlt/kein bool, 0 = false, 1 = true.
int lua_get_table_tribool(const char* table, const char* field);
void lua_set_table_bool(const char* table, const char* field, bool value);
void lua_set_table_vec3(const char* table, const char* field, const glm::vec3& value);
void lua_set_table_quat(const char* table, const char* field, const glm::quat& value);

// Gibt es dieses Global und ist es eine Funktion? (Lua: type(x) == "function")
bool lua_has_function(const char* name);

// Ruft `name(text, flag)` -- der Broker __re4_mono_request nimmt genau das.
void lua_call_global_str_bool(const char* name, const char* text, bool flag);

// Ruft `name(flag)` -- so nimmt es __re4_reload_set_mag_in_hand.
void lua_call_global_bool_arg(const char* name, bool flag);
bool lua_table_has(const char* table, const char* field);

// Eine globale Lua-Funktion mit (string, number) rufen -- das ist die Signatur
// von __re4_reload_apply_pose(name, blend). Rueckgabe: true, wenn die Funktion
// existierte UND ohne Fehler einen wahren Wert lieferte.
bool lua_call_global_pose(const char* name, const std::string& pose, float blend);

// __re4_reload_apply_pose_bones(bones, blend) -- `bones` ist eine Lua-Tabelle
// { ["L_Thumb1"] = {w, x, y, z}, ... }, also je Bone ein VIERERARRAY in der
// Reihenfolge w,x,y,z. Gebraucht fuer die Compound-Bow-Posen aus merc.
// Rueckgabe: true, wenn die Funktion existierte UND true lieferte.
bool lua_call_pose_bones(const char* name,
                         const std::unordered_map<std::string, glm::quat>& bones,
                         float blend);

// Wie oben, aber die Funktion liegt in einer globalen TABELLE -- das ist der
// Fallback-Weg ueber das gestures-Modul: _G.__re4_gestures.apply_pose(name, blend).
bool lua_call_module_pose(const char* table, const char* fn,
                          const std::string& pose, float blend);

// Ein Feld-Buendel aus `_G.<map>[<numerischer Key>]` lesen (z.B.
// __re4_knife_lh_off_map[5001] -> px,py,pz,rx,ry,rz). `out` bekommt fuer jedes
// nicht vorhandene Feld eine 0.
//
// Rueckgabe DREIWERTIG, und das ist kein Luxus: attach_weapon unterscheidet in
// der Lua-Fassung ueber `is_left and MAP or ANDERE_MAP` zwischen "die Map fehlt"
// (dann greift die andere Map) und "die Map ist da, hat aber keinen Eintrag"
// (dann greift der Nullwert).
//   0 = Map fehlt   1 = Map da, kein Eintrag   2 = Eintrag gelesen
int lua_get_map_fields(const char* map, int32_t key,
                       std::initializer_list<const char*> fields, float* out);

// Die beiden Fremd-Hooks aus attach_weapon. Sie ersetzen Position UND Rotation
// und werden nur wirksam, wenn das jeweilige Script sie gesetzt hat.
//   __re4_wildwest_apply(pos, rot)                 -> pos, rot
//   __re4_merc_wep_apply(pos, rot, wid, hand_rot)  -> pos, rot
bool lua_call_transform_hook(const char* name, glm::vec3& pos, glm::quat& rot);
bool lua_call_merc_wep_apply(glm::vec3& pos, glm::quat& rot, int32_t wid,
                             const glm::quat& hand_rot);

// Vektoren/Quaternionen aus Lua lesen. Leer, wenn nicht vorhanden oder falscher Typ.
std::optional<glm::vec3> lua_get_vec3(const char* name);
std::optional<glm::quat> lua_get_quat(const char* name);

// Prioritaetskette: den ersten Namen nehmen, der einen Vektor liefert.
// (Die Scripte lesen ihre Hand-Targets so: joint_pos -> unified -> world.)
std::optional<glm::vec3> lua_get_vec3_any(std::initializer_list<const char*> names);

// Eine global hinterlegte Lua-Funktion parameterlos aufrufen (z.B. den
// Spine-Pin aus re4_vr_minecart.lua). Fehlt sie, passiert nichts.
void lua_call_global(const char* name);

// LocalScale eines Joints setzen -- dafuer gibt es im SDK keinen Helfer.
void set_joint_local_scale(::REJoint* joint, const glm::vec3& scale);

// Setzt die globale Tabelle `vr_recoil` (position/rotation/active), die
// re4_vr_motion.lua liest. Existiert die Tabelle schon, werden nur die Felder
// ueberschrieben -- exakt wie das Lua-Original mit
// `_G.vr_recoil = _G.vr_recoil or {...}`.
//
// [KORREKTUR 03.09.2026] Dieses Muster rettet die Tabelle NICHT ueber einen
// Script-Reset: reset_scripts() zerstoert den sol::state und legt einen neuen an
// (ScriptRunner.cpp), _G ist danach leer. Es schuetzt nur gegen ein zweites
// Laden im SELBEN State. Am Verhalten dieser Funktion aendert das nichts.
void   lua_set_vr_recoil(const glm::vec3& position, const glm::quat& rotation, bool active);

// ---- Konfiguration -------------------------------------------------------
// Verzeichnis reframework/data. Der Fork hat dafuer KEINE oeffentliche Funktion;
// die private Fassung in Json.cpp/FS.cpp baut denselben Pfad aus
// REFramework::get_persistent_dir(). Wir bauen ihn hier genauso.
std::filesystem::path datadir();

// Laedt/schreibt eine JSON unter reframework/data (relativer Pfad wie in Lua,
// z.B. "re4_vr/re4_vr_recoil.json"). Beim Laden kommt bei jedem Fehler ein
// leeres Objekt zurueck -- nie eine Exception, genau wie der pcall in Lua.
nlohmann::json json_load(const std::string& relative_path);
bool           json_save(const std::string& relative_path, const nlohmann::json& j, int indent = 4);

// ---- Spielzustand --------------------------------------------------------
// Aktuelle Waffen-ID oder -1. Weg wie in Lua: CharacterManager ->
// getPlayerContextRef -> get_HeadUpdater -> get_EquipWeaponID. Das value__-
// Auspacken der Lua-Fassung entfaellt: ueber die TypeDB kommt der Enum in C++
// direkt als Integer zurueck.
int32_t get_current_weapon_id();

// "wp%04d" wie in Lua -- das ist der Schluessel in den JSON-Override-Tabellen.
std::string weapon_key_from_id(int32_t wid);

// ---- Engine-Objekte ------------------------------------------------------
// Bauform 1:1 aus der Referenz-Portierung im Nachbar-Fork uebernommen
// (puredark/src/mods/vr/games/re4/RE4VRShared) -- dieselben nativen Aufrufe,
// damit hier keine eigene, ungetestete Variante entsteht.

// Ist der Zeiger ueberhaupt ein gueltiges Managed-Objekt?
bool obj_ok(::REManagedObject* o);

// via.GameObject.create(System.String) / destroy(via.GameObject)
::REGameObject* create_game_object(std::string_view name);
void destroy_game_object(::REManagedObject* go);

// getComponent(System.Type) ueber den Runtime-Type.
::REManagedObject* get_component(::REManagedObject* go, const char* type_name);
::REManagedObject* get_component(::REManagedObject* go, sdk::RETypeDefinition* td);

// Runtime-Type-Objekt (das Gegenstueck zu sdk.typeof in Lua).
::REManagedObject* runtime_type(const char* type_name);

// Enum-Konstante live aufloesen statt hartzukodieren (Luas
// find_type_definition -> get_field -> get_data(nil)). Leer = nicht gefunden.
// [PORTFIX 2026-09-06] Enum-Literal typrichtig lesen (1/2/4/8 Byte). Ein
// festes get_data<int32_t> liest bei einem Byte-Enum drei Nachbarwerte mit.
// [PORTFIX 2026-09-06] ValueType-Getter (via.vec3 / via.Quaternion) ueber
// einen 16-Byte-ausgerichteten sret-Puffer. NIE try_call<glm::vec3> dafuer.
// [CONTACTPOINT 20.09.2026 -- gemessen, zzz_re4_contact_sonde]
// via.physics.ContactPoint ist ein ValueType von 64 Byte. Ein sret-Puffer
// enthaelt die ROHE Struct, es gelten also die fieldptr-Offsets der TDB (ohne
// Objektkopf) -- NICHT die base-Offsets, die 0x10 groesser sind. Wer base
// nimmt, liest bei 0x34 den mUserDataPtr statt der Distanz: genau das liess
// das Fadenkreuz auf festem Abstand kleben und lieferte dem Messer nie eine
// Trefferdistanz.
namespace contact_point {
constexpr size_t SIZE     = 64;
constexpr size_t POSITION = 0x00;   // via.vec3
constexpr size_t NORMAL   = 0x10;   // via.vec3
constexpr size_t TIME     = 0x20;   // float TimeOfImpact
constexpr size_t DISTANCE = 0x24;   // float
}   // namespace contact_point

bool obj_get_vec4(::REManagedObject* obj, std::string_view name, glm::vec4& out);
bool obj_get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out);
bool obj_get_quat(::REManagedObject* obj, std::string_view name, glm::quat& out);

std::optional<int64_t> enum_field_value(sdk::REField* fld, void* obj);

std::optional<int32_t> enum_value(const char* type_name, const char* field_name);

// Feldzugriffe (Luas sf()). ACHTUNG: get_field auf einem WERTtyp liefert eine
// KOPIE -- hier wird nur gelesen, das ist unkritisch.
::REManagedObject* get_field_object(::REManagedObject* obj, const char* name);
std::optional<int32_t> get_field_int(::REManagedObject* obj, const char* name);

// [FELDZUGRIFF 05.09.2026] TDB-Weg (wie Luas obj:get_field). Fuer JEDES managed
// C#-Feld zwingend -- utility::re_managed_object::get_field<T> kennt nur die
// native via.*-Reflexion und liefert dort still nullptr/false/0.
std::optional<bool> get_field_bool(::REManagedObject* obj, const char* name);
std::optional<float> get_field_float(::REManagedObject* obj, const char* name);

// Wert-Varianten (fehlendes Feld -> false / 0), damit Aufrufstellen, die frueher
// utility::re_managed_object::get_field<T> nutzten, unveraendert bleiben.
bool get_field_bool_v(::REManagedObject* obj, const char* name);
int32_t get_field_int_v(::REManagedObject* obj, const char* name);
// chainsaw.PlayerDefine.State als Rohbits (fuer die Bit-53-Pruefung).
std::optional<uint64_t> get_state_bits(::REManagedObject* ctx);

// Die Spielerkette, jeweils mit Gueltigkeitspruefung.
::REManagedObject* character_manager();
::REManagedObject* player_context();
::REManagedObject* body_game_object();
::REManagedObject* body_transform();
::REManagedObject* head_game_object();
::REManagedObject* camera_system_singleton();

// get_Name eines GameObjects als std::string (leer bei Fehlschlag).
std::string obj_name(::REManagedObject* o);

// =====================================================================
// [SCRIPTGATE] Der native Gegenpart zu re4_vr_objects.lua.
//
// In Lua heisst "Script wirklich aus" NICHT "Callback tut nichts", sondern
// "Datei nicht geladen": reset_scripts() wirft den Lua-State weg, und
// ScriptState::~ScriptState ruft fuer JEDEN registrierten Hook
// g_hookman.remove(fn, id) (ScriptRunner.cpp:424-428). Danach steht der
// Umleiter zwar noch physisch in der Engine, seine Rueckrufliste ist aber
// leer -- und die abgeschaltete Datei traegt sich nicht neu ein.
//
// Genau diesen Zustand bildet dieser Schalter fuer die nativen Module nach:
// jeder Phasen-, Frame- und Draw-Callback UND jeder Hook-Rumpf steigt sofort
// aus. Anders als der frueher gemessene LUA-Waechter kostet das hier keinen
// Sprung in die VM, sondern das Lesen eines bool.
//
// Wer NICHT gegated wird, steht in der BLEIBT-Liste von objects: materials
// und firstperson (dazu binding, das Lua bleibt und ueber
// set_script_enabled laeuft).
// =====================================================================
// [DIAGNOSE 04.09.2026, entfernt 16.09.2026] Frueher Spur in einen Ringpuffer fuer
// den Absturz-Dump. Der Fork schreibt keine Logs mehr -- trace() ist leer und
// inline, der Compiler laesst die rund 50 Aufrufstellen damit ganz weg.
inline void trace(const char*, const char*) {}

// Setzt nur noch, WIE ein Absturz endet (still, ohne Fehlerdialog) -- keine
// Aufzeichnung mehr.
void install_crash_diagnostics();

void set_mods_gated(bool gated);
bool mods_gated();

// JSON-Felder mit Vorgabewert -- Lua-Semantik: fehlt oder null -> Vorgabe.
bool  j_bool(const nlohmann::json& d, const char* key, bool def);
float j_num(const nlohmann::json& d, const char* key, float def);
std::string j_str(const nlohmann::json& d, const char* key, const char* def);

// ---------------------------------------------------------------------------
// [TAUNT-WAV 2026-09-12] Eigene Sprach-WAVs, die als WAVE-Resource IN der DLL
// liegen (resources/re4_taunts.rc, Namen TAUNT01..TAUNT15). Gespielt ueber
// PlaySound(SND_RESOURCE|SND_ASYNC) aus winmm.
//
// Warum nicht ueber die Engine: soundlib.SoundManager.postRequestInfo ist der
// globale Sound-Funnel, durch den ALLE Spielsounds laufen (belegt am
// Shotgun-Pump-Mute, RE4VRReload4.cpp) -- dort gehen nur Wwise-IDs durch, nie
// ein Dateipfad. Die Engine kann eine eigene Datei also nicht annehmen.
//
// Eigenschaften, die man kennen muss:
//  * Ein laufendes WAV wird vom naechsten ABGESCHNITTEN. So gewollt (Ansage
//    12.09.2026) -- und genau das tut PlaySound von sich aus.
//  * Der Ton laeuft NICHT durch die Wwise-Busse: die Lautstaerkeregler des
//    Spiels und Occlusion greifen nicht. Dafuer braucht er kein Spielobjekt,
//    keinen SoundContainer und keinen Charakter-Pool.
//  * Ausgabe ist das Windows-Standardgeraet. Ein eingeschaltetes VR-Headset
//    wird genau das (Ansage 12.09.), der Ton landet also im Headset.
constexpr int TAUNT_WAV_COUNT = 41;

// [SEPARIERT 13.09.2026 -- Ansage "bitte separieren, choke bleibt so"]
// Der Tiergriff zieht AUSSCHLIESSLICH aus den ersten 15 (TAUNT01..15). Die ab
// 16 nachgelegten WAVs gehoeren allein den Gesten: 16..20 Mittelfinger,
// 21..27 Zeigefinger -- welche Geste welche bekommt, steht als "gest" in
// TAUNT_TABLE (RE4VRGuestures.cpp), nicht hier.
constexpr int TAUNT_WAV_CHOKE_COUNT = 15;

// Spielt das WAV mit diesem Index (0..TAUNT_WAV_COUNT-1).
// false = Index daneben oder Resource nicht in der DLL.
bool play_taunt_wav(int index);

// [ASHLEY 13.09.2026] Ihre eigenen Sprueche beim Choke (ASHLEY01..08).
constexpr int ASHLEY_WAV_COUNT = 11;

// Spielt einen davon (0..ASHLEY_WAV_COUNT-1); false = Index daneben oder
// Resource nicht in der DLL. Lautstaerke ueber denselben dB-Regler.
bool play_ashley_wav(int index);

// [HUELLKURVE 16.09.2026] Lautstaerke-Verlauf einer eingebetteten WAV, ein Wert
// je 25 ms (0..255). Wird zur Laufzeit aus der Resource gerechnet -- neue WAVs
// brauchen also keinen Extra-Schritt.
std::vector<uint8_t> wav_envelope(const char* res_name);

// ============================================================================
// [ASHLEY ANTWORTET 16.09.2026] Zwei WEITERE Poels: ihre Antwort, wenn der
// Spieler IHR eine Geste zeigt. Getrennt von ASHLEY01..08 (die gehoeren zum
// Griff) und getrennt VONEINANDER -- auf den Zeigefinger antwortet sie anders
// als auf den Stinkefinger (Ansage 16.09.2026).
//
//   REPLY_POINT -> "point"    (Zeigefinger, LT + R.A)   Resource ASHLEYPOINTnn
//   REPLY_FUCK  -> "fuck_you" (Stinkefinger, LT + R.B)  Resource ASHLEYFUCKnn
//
// Neue Datei: WAV nach resources/sounds, Zeile in re4_taunts.rc, den passenden
// COUNT hochzaehlen. Sonst nichts -- Beutel, Huellkurve und Mundbewegung ziehen
// automatisch nach.
//
// Beide stehen auf 0, solange keine Dateien da sind: der Weg bleibt dann still,
// ohne dass irgendwo eine Sperre gesetzt werden muesste.
// ============================================================================
constexpr int ASHLEY_POINT_WAV_COUNT = 8;
constexpr int ASHLEY_FUCK_WAV_COUNT = 8;

// true = Stinkefinger-Pool, false = Zeigefinger-Pool. Ein Schalter statt zweier
// Funktionssaetze: der Weg ist bis auf den Resource-Namen derselbe.
int ashley_reply_count(bool fuck);

// Spielt einen davon mit EIGENER Lautstaerke (dB, 0 = Datei unveraendert).
// Bewusst ein Parameter statt des globalen Reglers: der Pegel haengt hier an
// ihrem Abstand zum Spieler und wechselt mit jeder Antwort.
bool play_ashley_reply_wav(bool fuck, int index, float db);

// Shuffle-Beutel ueber einen der beiden Poels -- gleiche Mechanik wie
// ashley_wav_next. Jeder Pool braucht seinen EIGENEN Beutel.
int ashley_reply_wav_next(bool fuck, std::vector<int>& bag, int* last = nullptr);

// Resource-Name einer Antwort ("ASHLEYPOINT01"), fuer die Huellkurve.
// Puffer muss mindestens 24 Byte fassen.
void ashley_reply_res_name(bool fuck, int index, char* out, size_t out_size);

// Naechster Spruch aus IHREM Shuffle-Beutel -- Mechanik wie taunt_wav_next,
// aber ueber ihre acht Dateien und ohne Skip-Liste.
int ashley_wav_next(std::vector<int>& bag, int* last = nullptr);

// Lautstaerke der eigenen WAVs in dB, 0 = Datei unveraendert, geklemmt auf
// -40..+12. Gilt fuer BEIDE Verwender (Tiergriff und Mittelfinger-Geste) --
// gestellt wird er ueber den Regler in der Choke-UI und liegt in der
// Choke-JSON. Umgesetzt wird er, indem die Samples vor dem Abspielen skaliert
// und per SND_MEMORY gespielt werden; PlaySound selbst kennt keine Lautstaerke,
// und am Session-Volume zu drehen wuerde den ganzen Spielton mitnehmen.
// [JIGGLE 19.09.2026] Klatscher beim Kopf-Schubs (ASHLEYSLAP01..03), globaler dB-Regler.
constexpr int SLAP_WAV_COUNT = 3;
bool play_slap_wav(int index, float db);

// [WAV-SERIE 20.09.2026] Nummer des zuletzt gestarteten eigenen WAVs.
uint64_t wav_serial();

void  set_taunt_wav_gain_db(float db);
float taunt_wav_gain_db();

// [ADA LAUTER 17.09.2026] ZUSCHLAG in dB nur fuer Adas eigene WAVs (ihre
// Aufnahmen sind rund 10 dB leiser als Leons). Kommt AUF den allgemeinen
// Regler; gestellt in der Choke-UI, liegt in der Choke-JSON.
void  set_ada_wav_extra_db(float db);
float ada_wav_extra_db();

// Wie play_taunt_wav, aber mit eigenem Pegel (dB) statt des globalen Reglers.
bool play_taunt_wav_db(int index, float db);

// Zieht den naechsten Index aus einem Shuffle-Bag: ist der Beutel leer, wird er
// mit allen Indizes neu gefuellt, gemischt, und dann vom ENDE genommen -- exakt
// die Mechanik der Wwise-Taunts in RE4VRGuestures (Fisher-Yates von hinten,
// pop_back). Jeder Aufrufer haelt seinen EIGENEN Beutel, damit Tiergriff und
// Mittelfinger-Geste getrennte Reihenfolgen haben (Ansage 12.09.2026).
// Gefuellt wird NUR mit 0..TAUNT_WAV_CHOKE_COUNT-1: einziger Aufrufer ist der
// Tiergriff, und der soll die Gesten-WAVs ab 16 nie ziehen.
// `last` (optional) traegt den zuletzt gezogenen Index und verhindert die
// EINZIGE Stelle, an der sich ein Spruch wiederholen kann: die Beutelgrenze.
// Innerhalb eines Beutels kommt ohnehin jeder genau einmal.
int taunt_wav_next(std::vector<int>& bag, int* last = nullptr);

} // namespace re4vr
class RE4VRCrosshair;
class RE4VRArmChain;
class RE4VRFirstPerson;

// =====================================================================
// Traeger-Mod. Haelt genau EIN portiertes Modul: re4_vr_crosshair.lua.
//
// Die fuenf frueheren Portierungen (motion, movement, arm_chain, recoil,
// materials) und firstperson sind am 29.08.2026 vollstaendig ENTFERNT worden --
// sie liefen fehleranfaellig und brachten laut Messung praktisch keine
// Bildrate: alle Lua-Scripte zusammen kosten im Stillstand 0,731 ms, davon
// entfaellt mit 0,549 ms der groesste Teil auf einen einzigen Callback,
// re4_vr_crosshair.lua @ LockScene. Genau der wird hier portiert.
//
// WICHTIG, sonst zeichnet das Menue nichts: der Name aus get_name() muss in der
// Liste `visible_mods` in src/Mods.cpp stehen.
// =====================================================================
class RE4VR : public Mod {
public:
    static std::shared_ptr<RE4VR>& get();

    std::string_view get_name() const override {
        return "RE4VR";
    }

    std::optional<std::string> on_initialize() override;

    void on_lua_state_created(sol::state& lua) override;
    void on_lua_state_destroyed(sol::state& lua) override;

    void on_frame() override;
    void draw_dev_ui();   // frueher on_draw_ui (s. RE4VRMenu)

    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

    // Der Traeger haelt nur noch den re4vr::-Helferraum. Die frueheren
    // [PHASENSTUB]-Wege und die eigene Crosshair-Instanz sind ausgebaut:
    // jede portierte Lua-Datei ist ein eigener Mod, die Reihenfolge steht in
    // Mods.cpp.
};

#endif // RE4
