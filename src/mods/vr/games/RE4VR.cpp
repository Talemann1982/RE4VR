// =====================================================================
// RE4VR - Traeger-Mod + gemeinsame Helfer. Siehe RE4VR.hpp.
// =====================================================================
#if defined(RE4)

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <cstring>
#include <fstream>
#include <span>
#include <set>
#include <algorithm>
#include <cmath>   // [TAUNT-WAV] std::pow/std::abs fuer die dB-Umrechnung
#include <mutex>
#include <chrono>

#include <sdk/RETypeDB.hpp>
#include <sdk/SystemArray.hpp>
#include <sdk/RETypes.hpp>

// Reminder aus RE8VR.cpp, woertlich uebernommen, weil es hier genauso gilt:
// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"
#include "../../VR.hpp"
#include "../../../REFramework.hpp"

#include "RE4VRArmChain.hpp"
#include "RE4VRFirstPerson.hpp"
#include "../../../VigemPad.hpp"
#include "RE4VR.hpp"

// [TAUNT-WAV 2026-09-12] PlaySound fuer die eingebetteten Sprach-WAVs.
// Erst selbst deklariert (um die Makroflut von <mmsystem.h> zu meiden) -- das
// gab aber C2733 "cannot overload a function with extern \"C\" linkage": die
// Deklaration steht ueber REFramework.hpp -> windows.h schon im Bau, und
// HMODULE ist struct HINSTANCE__*, nicht void*. Also der echte Header.
#include <mmsystem.h>

namespace re4vr {
namespace {
// Der aktive Lua-State oder nullptr -- MIT der Sperre des ScriptRunners.
//
// Die Null-Pruefung ist Pflicht, nicht Vorsicht: reset_scripts() gibt den State
// frei und baut ihn neu; dazwischen ist der Zeiger null (Muster 1:1 aus
// APIProxy.cpp).
//
// [ABSTURZ 04.09.2026] Frueher stand hier die Annahme, alle Aufrufer kaemen aus
// dem Game-Thread. Das ist FALSCH: on_pre_gui_draw_element und die GUI-Hooks
// laufen im Render-Thread. Der ScriptRunner nimmt fuer JEDEN Lua-Callback zwei
// Sperren (ScriptRunner::on_pre_gui_draw_element -> m_access_mutex,
// ScriptState::on_pre_gui_draw_element -> m_execution_mutex); unsere Ports
// griffen ohne beide auf denselben State zu. Ergebnis war eine
// Zugriffsverletzung mitten in der Lua-VM (luaV_finishget) und danach sol-Panic
// -> std::terminate -> abort (0xC0000409).
//
// ScriptRunner::lock() nimmt m_access_mutex UND sperrt jeden State; beide
// Mutexe sind rekursiv, Wiedereintritt aus demselben Thread ist also sicher.
LuaRef lua_state() {
    return LuaRef{};
}
} // namespace

LuaRef::LuaRef() {
    auto runner = ScriptRunner::get();

    if (runner == nullptr) {
        return;
    }

    runner->lock();
    m_locked = true;

    auto& state = runner->get_state();

    if (state == nullptr || state->lua().lua_state() == nullptr) {
        return;
    }

    m_state = &state->lua();
}

LuaRef::~LuaRef() {
    if (m_locked) {
        if (auto runner = ScriptRunner::get(); runner != nullptr) {
            runner->unlock();
        }
    }
}

std::optional<double> lua_get_number_opt(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return std::nullopt;
    }

    sol::optional<double> v = (*lua)[name];
    return v.has_value() ? std::optional<double>{*v} : std::nullopt;
}

double lua_get_number(const char* name, double def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::optional<double> v = (*lua)[name];
    return v.has_value() ? *v : def;
}

bool lua_get_bool(const char* name, bool def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::optional<bool> v = (*lua)[name];
    return v.has_value() ? *v : def;
}

bool lua_table_is_truthy(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || v.get_type() == sol::type::lua_nil) {
        return false;
    }

    if (v.get_type() == sol::type::boolean) {
        return v.as<bool>();
    }

    return true;   // Zahlen (auch 0), Strings (auch "") sind in Lua WAHR
}

bool lua_is_truthy(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object v = (*lua)[name];

    if (!v.valid() || v.get_type() == sol::type::lua_nil) {
        return false;
    }

    // Nur ein echtes false ist unwahr. Zahlen (auch 0), Strings (auch ""),
    // Tabellen und Funktionen sind in Lua WAHR.
    if (v.get_type() == sol::type::boolean) {
        return v.as<bool>();
    }

    return true;
}

int lua_get_tribool(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return -1;
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() == sol::type::lua_nil) {
        return -1;
    }

    if (o.get_type() == sol::type::boolean) {
        return o.as<bool>() ? 1 : 0;
    }

    // Alles andere ist in Lua "wahr", ausser false/nil -- aber die Aufrufer hier
    // vergleichen im Original strikt mit == true, also gilt: kein bool -> false.
    return 0;
}

bool lua_module_call_bool(const char* module, const char* fn, bool def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    // Das Modul muss bereits per require geladen sein -- andere Scripte tun das.
    // Wir laden es NICHT selbst nach: ein require aus dem Hook heraus wuerde
    // fremden Lua-Code an einer Stelle ausfuehren, an der das Original das nie tut.
    sol::object loaded = (*lua)["package"]["loaded"][module];

    if (!loaded.valid() || loaded.get_type() != sol::type::table) {
        return def;
    }

    sol::table t = loaded.as<sol::table>();
    sol::object f = t[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return def;
    }

    // protected_function: ein Fehler im Lua-Modul darf uns nicht reissen --
    // im Original steht an genau dieser Stelle ein pcall.
    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf();

    if (!result.valid()) {
        return def;
    }

    sol::object r = result;

    if (r.get_type() == sol::type::boolean) {
        return r.as<bool>();
    }

    // nil/andere Typen: das Original prueft `if o and v then` -- also nur ein
    // echtes true zaehlt.
    return def;
}

int lua_module_call_tribool(const char* module, const char* fn) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return -1;
    }

    sol::object loaded = (*lua)["package"]["loaded"][module];

    if (!loaded.valid() || loaded.get_type() != sol::type::table) {
        return -1;
    }

    sol::object f = loaded.as<sol::table>()[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return 1;   // Funktion fehlt -> das Original ueberspringt die Pruefung
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf();

    if (!result.valid()) {
        return 0;   // pcall geworfen -> Lua: `not (ok and ...)` -> false
    }

    sol::object r = result;
    return (r.get_type() == sol::type::boolean && r.as<bool>()) ? 1 : 0;
}

namespace {
// Gemeinsamer Kern fuer die lua_module_call_*-Familie: liefert das Ergebnis eines
// parameterlosen Aufrufs aus einem per require geladenen Modul, oder nichts.
sol::object module_call(const char* module, const char* fn) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return sol::lua_nil;
    }

    sol::object loaded = (*lua)["package"]["loaded"][module];

    if (!loaded.valid() || loaded.get_type() != sol::type::table) {
        return sol::lua_nil;
    }

    sol::object f = loaded.as<sol::table>()[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return sol::lua_nil;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf();

    if (!result.valid()) {
        return sol::lua_nil;
    }

    return result;
}
} // namespace

std::optional<double> lua_module_call_number(const char* module, const char* fn) {
    sol::object r = module_call(module, fn);

    if (r.valid() && r.get_type() == sol::type::number) {
        return r.as<double>();
    }

    // Das Original schiebt den Wert durch tonumber() -- eine Zahl als String
    // zaehlt dort also mit.
    if (r.valid() && r.get_type() == sol::type::string) {
        try {
            return std::stod(r.as<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::string lua_module_call_string(const char* module, const char* fn) {
    sol::object r = module_call(module, fn);

    if (r.valid() && r.get_type() == sol::type::string) {
        return r.as<std::string>();
    }

    return {};
}

::REManagedObject* lua_get_pointer(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return nullptr;
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() != sol::type::userdata) {
        return nullptr;
    }

    return o.as<::REManagedObject*>();
}

::REManagedObject* lua_call_global_obj(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return nullptr;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return nullptr;
    }

    try {
        sol::protected_function fn = f;
        auto r = fn();

        if (!r.valid()) {
            return nullptr;
        }

        sol::object o = r;

        if (!o.valid() || o.get_type() != sol::type::userdata) {
            return nullptr;
        }

        return o.as<::REManagedObject*>();
    } catch (...) {
        return nullptr;
    }
}

::REManagedObject* lua_call_global_obj_arg(const char* name, ::REManagedObject* arg) {
    auto lua = lua_state();

    if (lua == nullptr || arg == nullptr) {
        return nullptr;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return nullptr;
    }

    try {
        sol::protected_function fn = f;
        auto r = fn(arg);

        if (!r.valid()) {
            return nullptr;
        }

        sol::object o = r;

        if (!o.valid() || o.get_type() != sol::type::userdata) {
            return nullptr;
        }

        return o.as<::REManagedObject*>();
    } catch (...) {
        return nullptr;
    }
}

std::optional<double> lua_call_bone_dist(const char* name, ::REManagedObject* tf,
                                         const glm::vec3& pos) {
    auto lua = lua_state();

    if (lua == nullptr || tf == nullptr) {
        return std::nullopt;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return std::nullopt;
    }

    try {
        sol::protected_function fn = f;
        // Der Fork bindet Vector3f auf glm::vec3 -- ein direkter Push erzeugt
        // genau das, was die Lua-Seite erwartet.
        auto r = fn(tf, pos);

        if (!r.valid()) {
            return std::nullopt;
        }

        sol::object o = r;

        if (!o.valid() || o.get_type() != sol::type::number) {
            return std::nullopt;
        }

        return o.as<double>();
    } catch (...) {
        return std::nullopt;
    }
}

std::string lua_tostring_obj(::REManagedObject* obj) {
    auto lua = lua_state();

    if (lua == nullptr || obj == nullptr) {
        return {};
    }

    try {
        sol::object f = (*lua)["tostring"];

        if (!f.valid() || f.get_type() != sol::type::function) {
            return {};
        }

        sol::protected_function fn = f;
        auto r = fn(obj);

        if (!r.valid()) {
            return {};
        }

        sol::object o = r;

        return (o.valid() && o.get_type() == sol::type::string) ? o.as<std::string>()
                                                                : std::string{};
    } catch (...) {
        return {};
    }
}

std::optional<double> lua_call_global_num_arg(const char* name, ::REManagedObject* arg) {
    auto lua = lua_state();

    if (lua == nullptr || arg == nullptr) {
        return std::nullopt;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return std::nullopt;
    }

    try {
        sol::protected_function fn = f;
        auto r = fn(arg);

        if (!r.valid()) {
            return std::nullopt;
        }

        sol::object o = r;

        if (!o.valid() || o.get_type() != sol::type::number) {
            return std::nullopt;
        }

        return o.as<double>();
    } catch (...) {
        return std::nullopt;
    }
}

void lua_set_hit_record(const char* name, ::REManagedObject* go, const glm::vec3& pos,
                        double t) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    try {
        auto tbl = lua->create_table();

        if (go != nullptr) {
            tbl["go"] = go;
        }

        tbl["pos"] = pos;
        tbl["t"] = t;

        (*lua)[name] = tbl;
    } catch (...) {
    }
}

std::optional<double> call_num(::REManagedObject* obj, std::string_view name,
                               std::string* type_out) {
    if (!obj_ok(obj)) {
        return std::nullopt;
    }

    auto* def = utility::re_managed_object::get_type_definition(obj);
    auto* fn = def != nullptr ? def->get_method(name) : nullptr;

    if (fn == nullptr) {
        return std::nullopt;
    }

    auto* rt = fn->get_return_type();

    if (rt == nullptr) {
        return std::nullopt;
    }

    // Enum -> auf seinen Grundtyp herunterbrechen (so macht es parse_data).
    auto* eff = rt->is_enum() ? rt->get_underlying_type() : rt;

    if (eff == nullptr) {
        return std::nullopt;
    }

    // [FALLE] get_full_name() liefert einen std::string PER WERT -- ein
    // .c_str() darauf zeigt sofort ins Leere. Deshalb die Kopie halten.
    const std::string tn = eff->get_full_name();

    if (type_out != nullptr) {
        *type_out = tn;
    }

    try {
        std::array<void*, 1> no_args{};
        const auto ret = fn->invoke(obj, std::span<void*>(no_args.data(), 0));

        if (ret.exception_thrown) {
            return std::nullopt;
        }

        const void* d = ret.bytes.data();

        // [WICHTIG] Ueber invoke kommt System.Single als DOUBLE zurueck -- die
        // Wrapper-Konvertierung der Engine. Genau so steht es in parse_data.
        if (tn == "System.Single") {
            return *reinterpret_cast<const double*>(d);
        }

        if (tn == "System.Double") {
            return *reinterpret_cast<const double*>(d);
        }

        if (tn == "System.Boolean") {
            return *reinterpret_cast<const bool*>(d) ? 1.0 : 0.0;
        }

        if (tn == "System.SByte") {
            return static_cast<double>(*reinterpret_cast<const int8_t*>(d));
        }

        if (tn == "System.Byte") {
            return static_cast<double>(*reinterpret_cast<const uint8_t*>(d));
        }

        if (tn == "System.Int16") {
            return static_cast<double>(*reinterpret_cast<const int16_t*>(d));
        }

        if (tn == "System.UInt16") {
            return static_cast<double>(*reinterpret_cast<const uint16_t*>(d));
        }

        if (tn == "System.Int32") {
            return static_cast<double>(*reinterpret_cast<const int32_t*>(d));
        }

        if (tn == "System.UInt32") {
            return static_cast<double>(*reinterpret_cast<const uint32_t*>(d));
        }

        if (tn == "System.Int64") {
            return static_cast<double>(*reinterpret_cast<const int64_t*>(d));
        }

        if (tn == "System.UInt64") {
            return static_cast<double>(*reinterpret_cast<const int64_t*>(d));
        }
    } catch (...) {
    }

    // Rueckfall auf den Weg, der in RE4VRChoke seit dem Port funktioniert:
    // erst int32, dann float. Greift, wenn der Rueckgabetyp keiner der oben
    // behandelten ist oder invoke nicht durchkommt -- damit liefert dieser
    // Helfer nie GAR NICHTS, wo Lua eine Zahl bekommt.
    if (int32_t iv = 0; try_call<int32_t>(obj, name, iv)) {
        return static_cast<double>(iv);
    }

    if (float fv = 0.0f; try_call<float>(obj, name, fv)) {
        return static_cast<double>(fv);
    }

    return std::nullopt;
}

bool call_cmd(::REManagedObject* obj, std::string_view name, std::span<void*> args) {
    if (!obj_ok(obj)) {
        return false;
    }

    auto* def = utility::re_managed_object::get_type_definition(obj);
    auto* fn = def != nullptr ? def->get_method(name) : nullptr;

    if (fn == nullptr) {
        return false;
    }

    try {
        const auto ret = fn->invoke(obj, args);

        return !ret.exception_thrown;
    } catch (...) {
    }

    return false;
}

std::string lua_get_string(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return {};
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() != sol::type::string) {
        return {};
    }

    return o.as<std::string>();
}

void lua_set_bool(const char* name, bool value) {
    if (auto lua = lua_state(); lua != nullptr) {
        (*lua)[name] = value;
    }
}

void lua_set_number(const char* name, double value) {
    if (auto lua = lua_state(); lua != nullptr) {
        (*lua)[name] = value;
    }
}

void lua_set_string(const char* name, const std::string& value) {
    if (auto lua = lua_state(); lua != nullptr) {
        (*lua)[name] = value;
    }
}

void lua_set_nil(const char* name) {
    if (auto lua = lua_state(); lua != nullptr) {
        (*lua)[name] = sol::lua_nil;
    }
}

void lua_set_vec3(const char* name, const glm::vec3& value) {
    if (auto lua = lua_state(); lua != nullptr) {
        (*lua)[name] = value;
    }
}

void lua_set_vec3_table(const char* name, const glm::vec3& value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    auto t = lua->create_table();
    t["x"] = value.x;
    t["y"] = value.y;
    t["z"] = value.z;
    (*lua)[name] = t;
}

bool lua_call_global_bool(const char* name, bool def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return def;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf();

    if (!result.valid()) {
        return def;
    }

    sol::object r = result;
    return (r.get_type() == sol::type::boolean) ? r.as<bool>() : def;
}

// Wie lua_call_global_bool, nur fuer Funktions-Globals, die einen STRING
// liefern (z.B. __re4_char_now -> "leon"/"ada"). Leerer String = nicht da,
// keine Funktion, Fehler oder kein String -- der Aufrufer faellt dann auf
// seinen eigenen Weg zurueck, genau wie Luas pcall-Muster.
std::string lua_call_global_string(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return {};
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return {};
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf();

    if (!result.valid()) {
        return {};
    }

    sol::object r = result;

    return (r.get_type() == sol::type::string) ? r.as<std::string>() : std::string{};
}

// Funktion in einer Lua-TABELLE rufen -- fuer Plugin-APIs, die nur ueber Lua
// erreichbar sind (overlay.* aus re_vr.dll). Bewusst nur fuer SELTENE Aufrufe:
// jeder Zugriff nimmt die ScriptRunner-Sperre.
void lua_call_table_fn_bool(const char* table, const char* fn, bool arg) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    sol::object f = t.as<sol::table>()[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto r = pf(arg);
    (void)r;
}

void lua_call_table_fn_void(const char* table, const char* fn) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    sol::object f = t.as<sol::table>()[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto r = pf();
    (void)r;
}

// Luas `rawget(_G, name) ~= nil`. Gebraucht dort, wo ein Global als
// VORHANDEN/ABWESEND gelesen wird statt als Wert (z.B. __re4_bolt_aim_cut_t).
bool lua_has_value(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object v = (*lua)[name];

    return v.valid() && v.get_type() != sol::type::nil;
}

bool lua_call_pose_bones(const char* name,
                         const std::unordered_map<std::string, glm::quat>& bones,
                         float blend) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    // Je Bone ein Viererarray {w, x, y, z} -- so liest es reload.lua.
    sol::table tbl = lua->create_table();

    for (const auto& [bone, q] : bones) {
        sol::table e = lua->create_table();
        e[1] = q.w;
        e[2] = q.x;
        e[3] = q.y;
        e[4] = q.z;
        tbl[bone] = e;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    sol::protected_function_result result;

    try {
        result = pf(tbl, blend);
    } catch (...) {
        return false;
    }

    if (!result.valid()) {
        return false;
    }

    sol::object out = result;

    // Lua: `okr = ap(rp, blend) == true`
    return out.get_type() == sol::type::boolean && out.as<bool>();
}

// ===========================================================================
// [FRAMETIME-MESSUNG] s. RE4VR.hpp
// ===========================================================================
namespace perf {
namespace {
struct Acc {
    double frame_us{0.0};    // Summe im laufenden Frame
    double avg_us{0.0};      // gleitender Mittelwert
    double max_us{0.0};      // groesster je gemessener Frame-Anteil
    double last_us{0.0};     // letzter abgeschlossener Frame
};

std::mutex g_mutex;
std::unordered_map<std::string, Acc> g_acc;
// [KEIN HAKEN FUER DIAGNOSE -- Ansage des Users] Die Messung laeuft von
// selbst, sonst steht sie im Zweifel aus, wenn man sie braucht. Sie kostet
// ausgeschaltet wie eingeschaltet fast nichts (ein Zeitstempel je Modul und
// Phase); geschrieben wird nur bei einem Buckel, und das hoechstens 60-mal.
bool g_enabled = true;
double g_frame_avg = 0.0;
double g_frame_max = 0.0;
// Schwelle, ab der ein Frame als Buckel gilt und mitgeschrieben wird.
double g_spike_us = 20000.0;   // 20 ms -- 8 ms fing normale Frames (Median 8,6)
int g_spikes_written = 0;
constexpr int SPIKE_MAX = 60; // danach ist die Datei aussagekraeftig genug
} // namespace

void set_enabled(bool on) {
    std::scoped_lock _{g_mutex};

    if (g_enabled == on) {
        return;
    }

    g_enabled = on;
    g_acc.clear();
    g_frame_avg = 0.0;
    g_frame_max = 0.0;
    g_spikes_written = 0;
}

bool enabled() {
    return g_enabled;
}

void add(std::string_view name, const char* phase, double us) {
    if (!g_enabled) {
        return;
    }

    std::string key{name};
    key += "  [";
    key += phase;
    key += "]";

    std::scoped_lock _{g_mutex};
    g_acc[key].frame_us += us;
}

void frame_end() {
    if (!g_enabled) {
        return;
    }

    std::scoped_lock _{g_mutex};

    double total = 0.0;

    for (auto& [k, a] : g_acc) {
        a.last_us = a.frame_us;
        total += a.frame_us;

        if (a.frame_us > a.max_us) {
            a.max_us = a.frame_us;
        }

        // Gleitender Mittelwert ueber ~2 Sekunden bei 90 fps.
        a.avg_us = a.avg_us * 0.994 + a.frame_us * 0.006;
    }

    g_frame_avg = g_frame_avg * 0.994 + total * 0.006;

    if (total > g_frame_max) {
        g_frame_max = total;
    }

    // [FRAMETIME-MITSCHNITT AUSGEBAUT 2026-09-08] Buckel- und
    // Dauerstand-Zeilen nach re4_frametimes.txt sind raus; die Mittelwerte
    // oben bleiben, sie speisen den ImGui-Tree ueber rows().

    for (auto& [k, a] : g_acc) {
        a.frame_us = 0.0;
    }
}

std::vector<Row> rows() {
    std::scoped_lock _{g_mutex};
    std::vector<Row> out;
    out.reserve(g_acc.size());

    for (const auto& [k, a] : g_acc) {
        out.push_back(Row{k, a.avg_us, a.max_us, a.last_us});
    }

    std::sort(out.begin(), out.end(),
              [](const Row& a, const Row& b) { return a.avg_us > b.avg_us; });

    return out;
}

double frame_avg_us() {
    return g_frame_avg;
}

double frame_max_us() {
    return g_frame_max;
}

void reset() {
    std::scoped_lock _{g_mutex};
    g_acc.clear();
    g_frame_avg = 0.0;
    g_frame_max = 0.0;
    g_spikes_written = 0;
}

Scope::Scope(std::string_view name, const char* phase)
    : m_name{name}, m_phase{phase}, m_t0{0} {
    if (g_enabled) {
        m_t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::high_resolution_clock::now().time_since_epoch())
                   .count();
    }
}

Scope::~Scope() {
    if (!g_enabled || m_t0 == 0) {
        return;
    }

    const auto t1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch())
                        .count();
    add(m_name, m_phase, static_cast<double>(t1 - m_t0) / 1000.0);
}
} // namespace perf

// ===========================================================================
// [FRAME-CACHE] s. RE4VR.hpp
// ===========================================================================
namespace fc {
namespace {
struct State {
    int32_t frame{-1};
    bool off{false};

    ::REManagedObject* cm{nullptr};
    bool cm_done{false};

    ::REManagedObject* ctx{nullptr};
    bool ctx_done{false};

    ::REManagedObject* body_go{nullptr};
    bool body_go_done{false};

    ::REManagedObject* body_tf{nullptr};
    bool body_tf_done{false};

    ::REManagedObject* head_go{nullptr};
    bool head_go_done{false};

    ::REManagedObject* pe{nullptr};
    bool pe_done{false};

    std::optional<int32_t> wid{};
    bool wid_done{false};
};

State g_s;

// Der Cache haelt Zeiger ueber Aufrufe INNERHALB eines Frames. Ein Objekt, das
// die Engine im selben Frame freigibt, waere gefaehrlich -- deshalb vor jeder
// Rueckgabe dieselbe Wache wie ueberall sonst.
::REManagedObject* checked(::REManagedObject* o) {
    return re4vr::obj_ok(o) ? o : nullptr;
}
} // namespace

void reset() {
    g_s = State{};
}

bool on() {
    const auto* vr = VR::get().get();
    const int32_t f = vr != nullptr ? vr->get_frame_count() : -1;

    if (f != g_s.frame) {
        const int32_t keep = f;
        g_s = State{};
        g_s.frame = keep;

        // [NOTAUS] Wie in Lua: __re4_fc_off = true schaltet ihn ab.
        //
        // [FRAMETIME 2026-09-04] Der Griff nach Lua steht seit heute INNERHALB
        // der Frame-Schranke. Vorher lief er bei JEDEM Cache-Zugriff -- und
        // jeder Lua-Zugriff nimmt die ScriptRunner-Sperre. Bei den dutzenden
        // fc::ctx()/fc::body_go()-Aufrufen, die ein Frame ueber alle Module
        // zusammenkommen, war das dutzendfaches Sperren pro Frame fuer einen
        // Wert, der sich hoechstens einmal im Leben aendert.
        // Verhaltensunterschied: der Notaus wirkt ab dem naechsten Frame statt
        // sofort -- bei einem Schalter, den man von Hand umlegt, folgenlos.
        g_s.off = lua_get_tribool("__re4_fc_off") == 1;
    }

    return !g_s.off;
}

::REManagedObject* managed_singleton(const char* name) {
    // Nur der CharacterManager wird gehalten -- er ist der, der in der
    // Spieler-Kette steckt. Alles andere geht unveraendert durch.
    if (std::string_view{name}.find("CharacterManager") == std::string_view::npos) {
        return sdk::get_managed_singleton<::REManagedObject>(name);
    }

    if (!on()) {
        return sdk::get_managed_singleton<::REManagedObject>(name);
    }

    if (!g_s.cm_done) {
        g_s.cm = sdk::get_managed_singleton<::REManagedObject>(name);
        g_s.cm_done = true;
    }

    return checked(g_s.cm);
}

::REManagedObject* ctx() {
    if (!on()) {
        auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
        return cm != nullptr ? call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
                             : nullptr;
    }

    if (!g_s.ctx_done) {
        auto* cm = managed_singleton("chainsaw.CharacterManager");
        g_s.ctx = cm != nullptr ? call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
                                : nullptr;
        g_s.ctx_done = true;
    }

    return checked(g_s.ctx);
}

::REManagedObject* body_go() {
    if (!on()) {
        auto* c = ctx();
        return c != nullptr ? call_safe<::REManagedObject*>(c, "get_BodyGameObject") : nullptr;
    }

    if (!g_s.body_go_done) {
        auto* c = ctx();
        g_s.body_go = c != nullptr ? call_safe<::REManagedObject*>(c, "get_BodyGameObject")
                                   : nullptr;
        g_s.body_go_done = true;
    }

    return checked(g_s.body_go);
}

::REManagedObject* body_tf() {
    if (!on()) {
        auto* go = body_go();
        return go != nullptr ? call_safe<::REManagedObject*>(go, "get_Transform") : nullptr;
    }

    if (!g_s.body_tf_done) {
        auto* go = body_go();
        g_s.body_tf = go != nullptr ? call_safe<::REManagedObject*>(go, "get_Transform")
                                    : nullptr;
        g_s.body_tf_done = true;
    }

    return checked(g_s.body_tf);
}

::REManagedObject* head_go() {
    if (!on()) {
        auto* c = ctx();
        return c != nullptr ? call_safe<::REManagedObject*>(c, "get_HeadGameObject") : nullptr;
    }

    if (!g_s.head_go_done) {
        auto* c = ctx();
        g_s.head_go = c != nullptr ? call_safe<::REManagedObject*>(c, "get_HeadGameObject")
                                   : nullptr;
        g_s.head_go_done = true;
    }

    return checked(g_s.head_go);
}

::REManagedObject* pe() {
    const auto lookup = [&]() -> ::REManagedObject* {
        auto* head = head_go();

        if (head == nullptr) {
            return nullptr;
        }

        auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment");
        auto* t = td != nullptr ? (::REManagedObject*)td->get_runtime_type() : nullptr;

        // [NIL-TYPE-GUARD] getComponent NIE mit nil-Type.
        return t != nullptr
            ? call_safe<::REManagedObject*>(head, "getComponent(System.Type)", t)
            : nullptr;
    };

    if (!on()) {
        return lookup();
    }

    if (!g_s.pe_done) {
        g_s.pe = lookup();
        g_s.pe_done = true;
    }

    return checked(g_s.pe);
}

std::optional<int32_t> equip_wid() {
    const auto lookup = [&]() -> std::optional<int32_t> {
        auto* c = ctx();

        if (c == nullptr) {
            return std::nullopt;
        }

        auto* hu = call_safe<::REManagedObject*>(c, "get_HeadUpdater");

        if (hu == nullptr) {
            return std::nullopt;
        }

        int32_t v = 0;

        if (try_call<int32_t>(hu, "get_EquipWeaponID", v)) {
            return v;
        }

        // Lua faellt auf das value__-Feld zurueck, falls ein Enum-Objekt kommt.
        auto* w = call_safe<::REManagedObject*>(hu, "get_EquipWeaponID");

        return w != nullptr ? get_field_int(w, "value__") : std::nullopt;
    };

    if (!on()) {
        return lookup();
    }

    if (!g_s.wid_done) {
        g_s.wid = lookup();
        g_s.wid_done = true;
    }

    return g_s.wid;
}
} // namespace fc

bool lua_is_executing() {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    lua_State* L = lua->lua_state();

    if (L == nullptr) {
        return false;
    }

    lua_Debug ar{};

    // Ein Frame auf Ebene 0 heisst: wir stehen INNERHALB eines Lua-Aufrufs.
    return lua_getstack(L, 0, &ar) != 0;
}

int32_t array_size(::REManagedObject* arr) {
    if (arr == nullptr || !utility::re_managed_object::is_managed_object(arr)) {
        return 0;
    }

    // sdk::SystemArray ist die im Fork etablierte Abstraktion (so machen es
    // RE4VRFirstPerson und RE4VRMaterials bereits) -- sie kapselt inline- und
    // Zeiger-Elemente.
    try {
        return static_cast<int32_t>(reinterpret_cast<sdk::SystemArray*>(arr)->get_size());
    } catch (...) {
        return 0;
    }
}

::REManagedObject* array_element(::REManagedObject* arr, int32_t idx) {
    if (arr == nullptr || idx < 0 || idx >= array_size(arr)) {
        return nullptr;
    }

    try {
        return reinterpret_cast<sdk::SystemArray*>(arr)->get_element(idx);
    } catch (...) {
        return nullptr;
    }
}

bool lua_call_global_pos_radius_bool_with_field(const char* name, const glm::vec3& p, float r,
                                                const char* vals, const char* field,
                                                double value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    // Die ORIGINALREFERENZ merken -- sie kommt am Ende unveraendert zurueck.
    sol::object old = (*lua)[vals];

    // Flache Kopie anlegen; alle Felder ausser `field` wandern mit.
    sol::table copy = lua->create_table();

    if (old.valid() && old.get_type() == sol::type::table) {
        sol::table src = old.as<sol::table>();

        for (auto&& kv : src) {
            if (kv.first.get_type() == sol::type::string
                && kv.first.as<std::string>() == field) {
                continue;
            }

            copy[kv.first] = kv.second;
        }
    }

    copy[field] = value;
    (*lua)[vals] = copy;

    bool done = false;

    {
        sol::protected_function pf = f.as<sol::protected_function>();
        sol::protected_function_result result;

        try {
            result = pf(p, r);
        } catch (...) {
            result = sol::protected_function_result{};
        }

        if (result.valid()) {
            sol::object out = result;
            // Lua: `done = safe(...) == true` -- nur ein echtes true zaehlt.
            done = out.get_type() == sol::type::boolean && out.as<bool>();
        }
    }

    // IMMER zurueck, auch wenn der Treffer nichts fand.
    (*lua)[vals] = old;

    return done;
}

// [RADIUS OPTIONAL 05.09.2026] Ohne Radius aufrufen heisst in Lua: das zweite
// Argument ist NIL, und die Gegenseite nimmt ihren eigenen Default. Ein
// uebergebenes 0.0f ist etwas voellig anderes -- __re4_break_nearby verwirft
// damit jede Kiste.
bool lua_call_global_pos_bool(const char* name, const glm::vec3& p) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    sol::protected_function_result result;

    try {
        result = pf(p);
    } catch (...) {
        return false;
    }

    if (!result.valid()) {
        return false;
    }

    sol::object r0 = result;

    if (r0.get_type() == sol::type::boolean) {
        return r0.as<bool>();
    }

    if (r0.get_type() == sol::type::number) {
        return r0.as<double>() != 0.0;
    }

    return false;
}

bool lua_call_global_pos_radius_bool(const char* name, const glm::vec3& p, float r) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    sol::protected_function pf = f.as<sol::protected_function>();

    // Der Fork bindet Vector3f auf glm::vec3 -- ein direkter Push erzeugt also
    // genau das, was die Lua-Seite erwartet. pcall-Aequivalent: protected_function
    // liefert bei einem Fehler ein ungueltiges Ergebnis statt zu werfen.
    sol::protected_function_result result;

    try {
        result = pf(p, r);
    } catch (...) {
        return false;
    }

    if (!result.valid()) {
        return false;
    }

    sol::object out = result;

    // Lua: `done = safe(...) == true` -- nur ein echtes true zaehlt.
    return out.get_type() == sol::type::boolean && out.as<bool>();
}

void lua_set_managed_object(const char* name, ::REManagedObject* obj) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    // [ABSTURZ 04.09.2026] Der Push eines rohen Engine-Zeigers geht NICHT
    // geradeaus nach Lua: sol_lua_push ruft api::re_managed_object::add_ref
    // (Sdk.cpp), und das WIRFT eine sol::error, wenn is_managed_object(obj)
    // false liefert -- ausserdem greift es dabei ueber sv[...] in die Lua-VM.
    // Aus einem Hook heraus faengt diese Exception niemand: sie lief bis
    // std::terminate und riss das Spiel mit abort() um (0xC0000409).
    //
    // Die Lua-Fassung konnte das nicht treffen: dort war das Objekt bereits
    // ein von der Engine gereichter Lua-Wert, nie ein frisch gepushter Zeiger.
    // Also erst pruefen, dann pushen -- und den Push zusaetzlich absichern.
    if (obj == nullptr || !utility::re_managed_object::is_managed_object(obj)) {
        (*lua)[name] = sol::nil;
        return;
    }

    try {
        (*lua)[name] = obj;
    } catch (...) {
        (*lua)[name] = sol::nil;
    }
}

bool lua_get_table_bool(const char* table, const char* field, bool def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return def;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || v.get_type() != sol::type::boolean) {
        return def;
    }

    return v.as<bool>();
}

double lua_get_table_number(const char* table, const char* field, double def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return def;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || v.get_type() != sol::type::number) {
        return def;
    }

    return v.as<double>();
}

double lua_get_table_number2(const char* table, const char* sub, const char* field, double def) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return def;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return def;
    }

    sol::object s2 = t.as<sol::table>()[sub];

    if (!s2.valid() || s2.get_type() != sol::type::table) {
        return def;
    }

    sol::object v = s2.as<sol::table>()[field];

    if (!v.valid() || v.get_type() != sol::type::number) {
        return def;
    }

    return v.as<double>();
}

std::optional<glm::vec3> lua_get_table_vec3(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return std::nullopt;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return std::nullopt;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid()) {
        return std::nullopt;
    }

    // Die Scripte legen dort mal ein Vector3f-Userdata ab, mal eine schlichte
    // {x,y,z}-Tabelle -- beides muss gelesen werden.
    if (v.is<glm::vec3>()) {
        return v.as<glm::vec3>();
    }

    if (v.get_type() == sol::type::table) {
        sol::table e = v.as<sol::table>();
        glm::vec3 out{};
        out.x = e["x"].get_or(0.0f);
        out.y = e["y"].get_or(0.0f);
        out.z = e["z"].get_or(0.0f);
        return out;
    }

    return std::nullopt;
}

bool lua_table_exists(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[name];
    return t.valid() && t.get_type() == sol::type::table;
}

void lua_ensure_table(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[name];

    if (!t.valid() || t.get_type() != sol::type::table) {
        (*lua)[name] = lua->create_table();
    }
}

void lua_set_table_number(const char* table, const char* field, double value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    t.as<sol::table>()[field] = value;
}

bool lua_get_cock_off(glm::vec3& pos, glm::vec3& rot) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)["__vr_rev_cock_off"];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::table tt = t.as<sol::table>();
    const auto num = [&tt](const char* f) -> float {
        sol::object o = tt[f];
        return (o.valid() && o.get_type() == sol::type::number) ? static_cast<float>(o.as<double>()) : 0.0f;
    };

    pos = glm::vec3{num("px"), num("py"), num("pz")};
    rot = glm::vec3{num("rx"), num("ry"), num("rz")};
    return true;
}

void lua_set_cock_off(const glm::vec3& pos, const glm::vec3& rot) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)["__vr_rev_cock_off"];

    if (!t.valid() || t.get_type() != sol::type::table) {
        (*lua)["__vr_rev_cock_off"] = lua->create_table();
        t = (*lua)["__vr_rev_cock_off"];

        if (!t.valid() || t.get_type() != sol::type::table) {
            return;
        }
    }

    sol::table tt = t.as<sol::table>();
    tt["px"] = pos.x;
    tt["py"] = pos.y;
    tt["pz"] = pos.z;
    tt["rx"] = rot.x;
    tt["ry"] = rot.y;
    tt["rz"] = rot.z;
}

bool lua_get_lh_off(int32_t wid, glm::vec3& pos, glm::vec3& rot) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)["__re4_knife_lh_off_map"];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::object e = t.as<sol::table>()[wid];

    if (!e.valid() || e.get_type() != sol::type::table) {
        return false;
    }

    sol::table et = e.as<sol::table>();
    const auto num = [&et](const char* f) -> float {
        sol::object o = et[f];
        return (o.valid() && o.get_type() == sol::type::number) ? static_cast<float>(o.as<double>()) : 0.0f;
    };

    pos = glm::vec3{num("px"), num("py"), num("pz")};
    rot = glm::vec3{num("rx"), num("ry"), num("rz")};
    return true;
}

bool lua_get_xyz_at(const char* table, int32_t key, glm::vec3& out) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::object e = t.as<sol::table>()[key];

    if (!e.valid() || e.get_type() != sol::type::table) {
        return false;
    }

    sol::table et = e.as<sol::table>();
    const auto num = [&et](const char* f) -> float {
        sol::object o = et[f];
        return (o.valid() && o.get_type() == sol::type::number) ? static_cast<float>(o.as<double>()) : 0.0f;
    };

    out = glm::vec3{num("x"), num("y"), num("z")};
    return true;
}

bool lua_get_xyz_map(const char* table, nlohmann::json& out) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    out = nlohmann::json::object();

    // Lua speichert unter tostring(wid) -- der Schluessel der Lua-Tabelle ist
    // dagegen eine ZAHL.
    for (const auto& kv : t.as<sol::table>()) {
        if (kv.second.get_type() != sol::type::table) {
            continue;
        }

        long long id = 0;

        if (kv.first.get_type() == sol::type::number) {
            id = static_cast<long long>(kv.first.as<double>());
        } else if (kv.first.get_type() == sol::type::string) {
            id = std::atoll(kv.first.as<std::string>().c_str());
        } else {
            continue;
        }

        sol::table e = kv.second.as<sol::table>();
        const auto num = [&e](const char* f) -> double {
            sol::object o = e[f];
            return (o.valid() && o.get_type() == sol::type::number) ? o.as<double>() : 0.0;
        };

        out[std::to_string(id)] = {{"x", num("x")}, {"y", num("y")}, {"z", num("z")}};
    }

    return true;
}

bool lua_get_number_map(const char* table, std::initializer_list<const char*> fields,
                        nlohmann::json& out) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::table tt = t.as<sol::table>();
    out = nlohmann::json::object();

    // Fehlende Felder werden NICHT geschrieben: in Lua sind sie nil und fallen
    // damit aus der JSON heraus.
    for (const auto* f : fields) {
        sol::object o = tt[f];

        if (o.valid() && o.get_type() == sol::type::number) {
            out[f] = o.as<double>();
        }
    }

    return true;
}

int lua_get_table_tribool(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return -1;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return -1;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || v.get_type() != sol::type::boolean) {
        return -1;
    }

    return v.as<bool>() ? 1 : 0;
}

void lua_seed_table_number(const char* table, const char* field, double value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    sol::table tt = t.as<sol::table>();
    sol::object cur = tt[field];

    // Nur anlegen, wenn wirklich nichts da ist. NICHT "wenn 0", denn in Lua ist
    // 0 wahr und ueberlebt das `or` des Originals.
    if (!cur.valid() || cur.get_type() == sol::type::lua_nil) {
        tt[field] = value;
    }
}

void lua_set_xyz_at(const char* table, int32_t key, float x, float y, float z) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    sol::table tt = t.as<sol::table>();
    sol::table e = lua->create_table();
    e["x"] = x;
    e["y"] = y;
    e["z"] = z;
    tt[key] = e;
}

void lua_set_table_bool(const char* table, const char* field, bool value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return;
    }

    t.as<sol::table>()[field] = value;
}

bool lua_table_has(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::object v = t.as<sol::table>()[field];
    return v.valid() && v.get_type() != sol::type::nil;
}

void lua_set_quat(const char* name, const glm::quat& value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    (*lua)[name] = value;
}

std::string lua_get_table_string(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return {};
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return {};
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || v.get_type() != sol::type::string) {
        return {};
    }

    return v.as<std::string>();
}

std::optional<glm::quat> lua_get_table_quat(const char* table, const char* field) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return std::nullopt;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return std::nullopt;
    }

    sol::object v = t.as<sol::table>()[field];

    if (!v.valid() || !v.is<glm::quat>()) {
        return std::nullopt;
    }

    return v.as<glm::quat>();
}

void lua_set_table_vec3(const char* table, const char* field, const glm::vec3& value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (t.valid() && t.get_type() == sol::type::table) {
        t.as<sol::table>()[field] = value;
    }
}

void lua_set_table_quat(const char* table, const char* field, const glm::quat& value) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object t = (*lua)[table];

    if (t.valid() && t.get_type() == sol::type::table) {
        t.as<sol::table>()[field] = value;
    }
}

bool lua_has_function(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];
    return f.valid() && f.get_type() == sol::type::function;
}

void lua_call_global_str_bool(const char* name, const char* text, bool flag) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    pf(text, flag);
}

void lua_call_global_bool_arg(const char* name, bool flag) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto r = pf(flag);
    (void)r;
}

bool lua_call_global_pose(const char* name, const std::string& pose, float blend) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    sol::protected_function pf = f.as<sol::protected_function>();
    auto result = pf(pose, blend);

    if (!result.valid()) {
        return false;
    }

    sol::object r = result;

    // Lua gibt hier je nach Pfad true/false oder gar nichts zurueck -- alles,
    // was nicht ausdruecklich false/nil ist, gilt wie in Lua als wahr.
    return r.valid() && r.get_type() != sol::type::nil
        && !(r.get_type() == sol::type::boolean && r.as<bool>() == false);
}

bool lua_call_module_pose(const char* table, const char* fn,
                          const std::string& pose, float blend) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object t = (*lua)[table];

    if (!t.valid() || t.get_type() != sol::type::table) {
        return false;
    }

    sol::object f = t.as<sol::table>()[fn];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    auto result = f.as<sol::protected_function>()(pose, blend);

    if (!result.valid()) {
        return false;
    }

    sol::object r = result;
    return r.valid() && r.get_type() != sol::type::nil
        && !(r.get_type() == sol::type::boolean && r.as<bool>() == false);
}

int lua_get_map_fields(const char* map, int32_t key,
                       std::initializer_list<const char*> fields, float* out) {
    size_t n = 0;

    for ([[maybe_unused]] auto f : fields) {
        out[n++] = 0.0f;
    }

    auto lua = lua_state();

    if (lua == nullptr) {
        return 0;
    }

    sol::object m = (*lua)[map];

    if (!m.valid() || m.get_type() != sol::type::table) {
        return 0;
    }

    sol::object e = m.as<sol::table>()[key];

    if (!e.valid() || e.get_type() != sol::type::table) {
        return 1;
    }

    sol::table t = e.as<sol::table>();
    n = 0;

    for (auto f : fields) {
        sol::object v = t[f];

        if (v.valid() && v.get_type() == sol::type::number) {
            out[n] = v.as<float>();
        }

        ++n;
    }

    return 2;
}

// Gemeinsamer Kern der beiden Transform-Hooks: nur wenn BEIDE Rueckgabewerte da
// sind, wird uebernommen -- genau wie `if mp and mr then`.
namespace {
bool take_transform_result(const sol::protected_function_result& r, glm::vec3& pos, glm::quat& rot) {
    if (!r.valid() || r.return_count() < 2) {
        return false;
    }

    sol::object p = r[0];
    sol::object q = r[1];

    if (!p.valid() || !q.valid() || !p.is<glm::vec3>() || !q.is<glm::quat>()) {
        return false;
    }

    pos = p.as<glm::vec3>();
    rot = q.as<glm::quat>();
    return true;
}
} // namespace

bool lua_call_transform_hook(const char* name, glm::vec3& pos, glm::quat& rot) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    return take_transform_result(f.as<sol::protected_function>()(pos, rot), pos, rot);
}

bool lua_call_merc_wep_apply(glm::vec3& pos, glm::quat& rot, int32_t wid,
                             const glm::quat& hand_rot) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return false;
    }

    sol::object f = (*lua)["__re4_merc_wep_apply"];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return false;
    }

    return take_transform_result(
        f.as<sol::protected_function>()(pos, rot, wid, hand_rot), pos, rot);
}

std::optional<glm::vec3> lua_get_vec3(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return std::nullopt;
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() != sol::type::userdata) {
        return std::nullopt;
    }

    // Die Scripte legen dort Vector3f (glm::vec3) ab; manche Engine-Werte
    // kommen als Vector4f zurueck, deshalb beide Formen zulassen.
    if (o.is<glm::vec3>()) {
        return o.as<glm::vec3>();
    }

    if (o.is<Vector4f>()) {
        const auto v = o.as<Vector4f>();
        return glm::vec3{v.x, v.y, v.z};
    }

    return std::nullopt;
}

std::optional<glm::quat> lua_get_quat(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return std::nullopt;
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() != sol::type::userdata || !o.is<glm::quat>()) {
        return std::nullopt;
    }

    return o.as<glm::quat>();
}

std::optional<glm::vec3> lua_get_vec3_any(std::initializer_list<const char*> names) {
    for (const auto n : names) {
        if (auto v = lua_get_vec3(n); v.has_value()) {
            return v;
        }
    }

    return std::nullopt;
}

void lua_call_global(const char* name) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object f = (*lua)[name];

    if (!f.valid() || f.get_type() != sol::type::function) {
        return;
    }

    // protected_function: im Original steht an dieser Stelle ein pcall.
    sol::protected_function pf = f.as<sol::protected_function>();
    (void)pf();
}

void set_joint_local_scale(::REJoint* joint, const glm::vec3& scale) {
    if (joint == nullptr) {
        return;
    }

    // ValueType-Parameter werden im Fork per Zeiger uebergeben.
    static auto* const method = []() -> sdk::REMethodDefinition* {
        auto* const td = sdk::find_type_definition("via.Joint");
        return td != nullptr ? td->get_method("set_LocalScale") : nullptr;
    }();

    if (method == nullptr) {
        return;
    }

    auto s = scale;
    auto context = sdk::get_thread_context();

    // [WIRFT] wie in call_safe: der geschuetzte Pfad wirft bei einer
    // Engine-Exception, und eine haengengebliebene muss weg.
    try {
        method->call_safe<void*>(context, joint, &s);
    } catch (...) {
    }

    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
    }
}

void lua_set_vr_recoil(const glm::vec3& position, const glm::quat& rotation, bool active) {
    auto lua = lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object existing = (*lua)["vr_recoil"];
    sol::table t;

    if (existing.valid() && existing.get_type() == sol::type::table) {
        // Vorhandene Tabelle weiterbenutzen. Das Lua-Original legt sie mit
        // `_G.vr_recoil = _G.vr_recoil or {...}` an, damit Konsumenten, die sich
        // die Tabelle gemerkt haben, ueber einen Script-Reset hinweg dieselbe
        // Referenz behalten. Wuerden wir hier jedes Mal eine neue Tabelle setzen,
        // schriebe man in eine, die keiner mehr liest.
        t = existing.as<sol::table>();
    } else {
        t = lua->create_table();
        (*lua)["vr_recoil"] = t;
    }

    t["position"] = position;
    t["rotation"] = rotation;
    t["active"] = active;
}

std::filesystem::path datadir() {
    return REFramework::get_persistent_dir() / "reframework" / "data";
}

nlohmann::json json_load(const std::string& relative_path) {
    try {
        const auto path = datadir() / relative_path;

        if (!std::filesystem::exists(path)) {
            return nlohmann::json::object();
        }

        std::ifstream f{path};

        if (!f.good()) {
            return nlohmann::json::object();
        }

        auto j = nlohmann::json::parse(f, nullptr, false);

        if (j.is_discarded()) {
            return nlohmann::json::object();
        }

        // [PORTFIX 2026-09-06] Hier stand zusaetzlich `|| !j.is_object()` --
        // damit fiel JEDE Datei durch, deren Wurzel ein ARRAY ist. Das trifft
        // genau eine, aber die wichtigste: re4_vr_killswitch_zones.json ist
        // eine Liste (451 Eintraege). load_zones() bekam ein leeres Objekt,
        // sein `if (!d.is_array()) return;` griff, m_zones blieb LEER -- und
        // damit war JEDE gespeicherte KS-Zone wirkungslos. Die Events fielen
        // auf die CamState-Listen zurueck, wo CamState 15 (Gimmick) nicht
        // steht: KS1 = volle 3rd-Person. Jeder Aufrufer prueft selbst mit
        // is_object()/is_array(), das Durchreichen ist also gefahrlos.
        return j;
    } catch (...) {
        // Wie der pcall im Original: kaputte Datei -> Defaults, kein Absturz.
        return nlohmann::json::object();
    }
}

bool json_save(const std::string& relative_path, const nlohmann::json& j, int indent) {
    try {
        const auto path = datadir() / relative_path;
        std::filesystem::create_directories(path.parent_path());

        std::ofstream f{path};

        if (!f.good()) {
            return false;
        }

        f << j.dump(indent);
        return true;
    } catch (...) {
        return false;
    }
}

int32_t get_current_weapon_id() {
    // Kette 1:1 wie in Lua. Den Frame-Cache-Zweig (__re4_frame_cache) hat das
    // Lua-Original nur, um Lua-Aufrufkosten zu sparen; nativ ist der direkte Weg
    // ohnehin billiger, und beide liefern im selben Frame denselben Wert.
    const auto cm = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CharacterManager"));

    if (cm == nullptr) {
        return -1;
    }

    const auto ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return -1;
    }

    const auto hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return -1;
    }

    return re4vr::call_safe<int32_t>(hu, "get_EquipWeaponID");
}

bool obj_ok(::REManagedObject* o) {
    return o != nullptr && utility::re_managed_object::is_managed_object(o);
}

::REGameObject* create_game_object(std::string_view name) {
    auto* td = sdk::find_type_definition("via.GameObject");
    auto* m = td != nullptr ? td->get_method("create(System.String)") : nullptr;

    if (m == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return nullptr;
    }

    try {
        auto* go = m->call_safe<::REGameObject*>(sdk::get_thread_context(), str);
        clear_vm_exception();
        return go;
    } catch (...) {
        clear_vm_exception();
        return nullptr;
    }
}

void destroy_game_object(::REManagedObject* go) {
    if (!obj_ok(go)) {
        return;
    }

    auto* td = sdk::find_type_definition("via.GameObject");
    auto* m = td != nullptr ? td->get_method("destroy(via.GameObject)") : nullptr;

    if (m == nullptr) {
        return;
    }

    try {
        m->call_safe<void*>(sdk::get_thread_context(), go);
    } catch (...) {
    }

    clear_vm_exception();
}

::REManagedObject* get_field_object(::REManagedObject* obj, const char* name) {
    if (!obj_ok(obj)) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* fld = td != nullptr ? td->get_field(name) : nullptr;

    if (fld == nullptr) {
        return nullptr;
    }

    try {
        return fld->get_data<::REManagedObject*>(obj);
    } catch (...) {
        return nullptr;
    }
}

std::optional<int32_t> get_field_int(::REManagedObject* obj, const char* name) {
    if (!obj_ok(obj)) {
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* fld = td != nullptr ? td->get_field(name) : nullptr;

    if (fld == nullptr) {
        return std::nullopt;
    }

    try {
        return fld->get_data<int32_t>(obj);
    } catch (...) {
        return std::nullopt;
    }
}

// [FELDZUGRIFF 05.09.2026 -- BEWIESEN] Es gibt im Fork ZWEI Wege an ein Feld:
//
//   utility::re_managed_object::get_field<T>(obj, "name")
//       -> get_field_desc -> utility::re_type::get_field_desc
//       -> laeuft ueber REType->fields->variables, also die NATIVE via.*-
//          Reflexion. Dort stehen NUR Engine-Felder (Valid, Chain, DeltaTime).
//          Ein managed C#-Feld findet dieser Weg NIE -- er liefert dann still
//          T{} , also nullptr/false/0. Keine Meldung, kein Fehler.
//
//   td->get_field(name) + get_data<T>   (die Helfer hier)
//       -> laeuft ueber die TDB und findet BEIDE Arten.
//          Genau das macht auch Luas `obj:get_field("name")`
//          (Sdk.cpp -> api::sdk::get_native_field -> type_def->get_field).
//
// Gemessen am 05.09. in reframework/data/re4_merc_hud.txt: findComponents
// lieferte das richtige Behavior (n=1), aber `_Main` kam als NULL zurueck --
// deshalb sassen die Mercenaries-Anzeigen nicht an ihren Offsets.
//
// Merke: Jedes Feld, dessen Name aus dem C#-Code stammt (fuehrender
// Unterstrich, `<Name>k__BackingField`, `_items`, `_size`), MUSS ueber diese
// Helfer laufen -- nie ueber utility::re_managed_object::get_field<T>.
std::optional<bool> get_field_bool(::REManagedObject* obj, const char* name) {
    if (!obj_ok(obj)) {
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* fld = td != nullptr ? td->get_field(name) : nullptr;

    if (fld == nullptr) {
        return std::nullopt;
    }

    try {
        return fld->get_data<bool>(obj);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<float> get_field_float(::REManagedObject* obj, const char* name) {
    if (!obj_ok(obj)) {
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* fld = td != nullptr ? td->get_field(name) : nullptr;

    if (fld == nullptr) {
        return std::nullopt;
    }

    try {
        return fld->get_data<float>(obj);
    } catch (...) {
        return std::nullopt;
    }
}

// Wert-Varianten: liefern bei fehlendem Feld denselben Nullwert wie der alte
// utility::re_managed_object::get_field<T>. So bleiben die Aufrufstellen
// unveraendert -- nur der SUCHWEG wechselt von der nativen Reflexion auf die
// TDB, und damit werden managed C#-Felder ueberhaupt erst gefunden.
bool get_field_bool_v(::REManagedObject* obj, const char* name) {
    return get_field_bool(obj, name).value_or(false);
}

int32_t get_field_int_v(::REManagedObject* obj, const char* name) {
    return get_field_int(obj, name).value_or(0);
}

std::optional<uint64_t> get_state_bits(::REManagedObject* ctx) {
    if (!obj_ok(ctx)) {
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(ctx);
    auto* m = td != nullptr ? td->get_method("get_State") : nullptr;

    if (m == nullptr) {
        return std::nullopt;
    }

    auto context = sdk::get_thread_context();

    try {
        return m->call_safe<uint64_t>(context, ctx);
    } catch (...) {
        return std::nullopt;
    }
}

// [PORTFIX 2026-09-06 BYTE-ENUM] Ein statisches Enum-Literal liegt in einem
// DICHT GEPACKTEN Block. get_data<int32_t> liest dort immer 4 Byte -- bei
// einem 1-Byte-Enum also den gesuchten Wert PLUS drei Nachbarwerte als
// High-Bytes. Betraf chainsaw.OccupiedMediatorPriority (System.Byte):
// GRAPPLED wurde nie GRAPPLED, wodurch Grapple, Raeuberleiter,
// Durchquetschen, Minidemo und die GimmickFix-Stufen allesamt stumm
// durchfielen und die Szene auf 3rd Person kippte. Lua liest ueber
// parse_data(field_type) typrichtig -- deshalb steht dort ueberall der
// value__-Fallback. Wir holen die Breite jetzt aus dem Feldtyp.
bool obj_get_vec4(::REManagedObject* obj, std::string_view name, glm::vec4& out) {
    if (obj == nullptr) {
        return false;
    }

    auto* def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    const auto method = def->get_method(name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{0.0f, 0.0f, 0.0f, 0.0f};

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj);
    } catch (...) {
        if (context != nullptr && context->unkPtr != nullptr
            && context->unkPtr->unkPtr != nullptr) {
            context->unkPtr->unkPtr = nullptr;
        }

        return false;
    }

    if (context != nullptr && context->unkPtr != nullptr
        && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
    }

    out = buf;

    return true;
}

// [PORTFIX 2026-09-06 VALUETYPE] via.vec3/via.Quaternion kommen als
// ValueType per sret zurueck -- die Engine schreibt SIMD-ausgerichtete
// 16 Byte. `try_call<glm::vec3>` reicht dafuer einen 12 Byte grossen,
// NICHT 16-fach ausgerichteten Puffer: der Wert wird falsch und es werden
// 4 Byte darueber hinaus geschrieben. Der Rest des Projekts (ArmChain,
// Choke, Weapons2 ...) nutzt laengst diesen 16-Byte-Weg; nur vier Stellen
// taten es nicht -- darunter get_player_pos() des Killswitch, das die
// handmarkierten KS-ZONEN fuettert. Damit matchte KEINE der 451 Zonen mehr.
bool obj_get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out) {
    glm::vec4 v{};

    if (!obj_get_vec4(obj, name, v)) {
        return false;
    }

    out = glm::vec3{v.x, v.y, v.z};

    return true;
}

bool obj_get_quat(::REManagedObject* obj, std::string_view name, glm::quat& out) {
    glm::vec4 v{};

    if (!obj_get_vec4(obj, name, v)) {
        return false;
    }

    // via.Quaternion liegt als x,y,z,w im Speicher; glm::quat ist w,x,y,z.
    out = glm::quat{v.w, v.x, v.y, v.z};

    return true;
}

std::optional<int64_t> enum_field_value(sdk::REField* fld, void* obj) {
    if (fld == nullptr) {
        return std::nullopt;
    }

    try {
        void* p = fld->get_data_raw(obj);

        if (p == nullptr) {
            return std::nullopt;
        }

        uint32_t sz = 4;

        if (auto* ft = fld->get_type(); ft != nullptr) {
            const auto vs = ft->get_valuetype_size();

            if (vs == 1 || vs == 2 || vs == 4 || vs == 8) {
                sz = vs;
            }
        }

        switch (sz) {
        case 1:
            return static_cast<int64_t>(*static_cast<uint8_t*>(p));
        case 2:
            return static_cast<int64_t>(*static_cast<uint16_t*>(p));
        case 8:
            return *static_cast<int64_t*>(p);
        default:
            return static_cast<int64_t>(*static_cast<int32_t*>(p));
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int32_t> enum_value(const char* type_name, const char* field_name) {
    auto* td = sdk::find_type_definition(type_name);

    if (td == nullptr) {
        return std::nullopt;
    }

    // Statisches Feld -> get_data_raw mit nullptr, wie Luas get_data(nil).
    const auto v = enum_field_value(td->get_field(field_name), nullptr);

    return v.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*v)}
                         : std::nullopt;
}

::REManagedObject* runtime_type(const char* type_name) {
    auto* td = sdk::find_type_definition(type_name);
    return td != nullptr ? td->get_runtime_type() : nullptr;
}

::REManagedObject* get_component(::REManagedObject* go, sdk::RETypeDefinition* td) {
    if (!obj_ok(go) || td == nullptr) {
        return nullptr;
    }

    auto* rt = td->get_runtime_type();

    if (rt == nullptr) {
        return nullptr;
    }

    return call_safe<::REManagedObject*>(go, "getComponent(System.Type)", rt);
}

::REManagedObject* get_component(::REManagedObject* go, const char* type_name) {
    if (!obj_ok(go) || type_name == nullptr) {
        return nullptr;
    }

    return get_component(go, sdk::find_type_definition(type_name));
}

// [FRAME-CACHE 04.09.2026] Die ganze Spielerkette laeuft ueber re4vr::fc.
// Vorher machte JEDER Aufruf die volle Strecke -- get_managed_singleton,
// getPlayerContextRef, get_BodyGameObject, get_Transform -- und das in
// apply_hold & Co. fuenfmal pro Frame, in jedem Modul. Genau dafuer gibt es
// seit jeher autorun/re4vr/re4_vr_frame_cache.lua, das alle Lua-Dateien
// benutzen; die Ports hatten es umgangen.
// Der Cache haelt nichts ueber den Frame hinaus (__re4_fc_off schaltet ihn ab).
::REManagedObject* character_manager() {
    return fc::managed_singleton(game_namespace("CharacterManager").c_str());
}

::REManagedObject* player_context() {
    return fc::ctx();
}

::REManagedObject* body_game_object() {
    return fc::body_go();
}

::REManagedObject* body_transform() {
    return fc::body_tf();
}

::REManagedObject* head_game_object() {
    return fc::head_go();
}

::REManagedObject* camera_system_singleton() {
    return sdk::get_managed_singleton<::REManagedObject>(game_namespace("CameraSystem"));
}

// [SCRIPTGATE] s. RE4VR.hpp. Nur ein Flag -- gesetzt wird es ausschliesslich
// von RE4VRObjects, gelesen von jedem gegateten Modul.
namespace {
bool g_mods_gated = false;
}

// [CRASH-HANDLER 04.09.2026, Aufzeichnung entfernt 16.09.2026 -- goldene Regel:
// der Fork schreibt keine Logs] Bis hierher sammelten ein Ringpuffer (trace), ein
// Vectored Exception Handler (bei JEDER Exception im Prozess, eigene Wuerfe samt
// Stack-Capture) und ein Stack-Overflow-Report Daten, die beim Absturz ins
// Framework-Log bzw. nach re4_stackoverflow.txt geschrieben wurden. Das ist alles
// raus -- der VEH und die String-Operationen in trace() kosteten auch im
// Normalbetrieb.
//
// GEBLIEBEN ist nur, WIE ein Absturz endet, damit sich daran nichts aendert:
// still per _exit(3) statt Windows-Fehlerdialog, abort-Meldung und Fehlerbericht
// bleiben unterdrueckt. Im Normalbetrieb kostet das nichts.
namespace {
std::terminate_handler g_prev_terminate = nullptr;

void re4vr_terminate_handler() {
    if (g_prev_terminate != nullptr && g_prev_terminate != &re4vr_terminate_handler) {
        g_prev_terminate();
    }

    abort();
}

void re4vr_sigabrt_handler(int) {
    ::_exit(3);
}

void re4vr_invalid_parameter_handler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                     uintptr_t) {
    ::_exit(3);
}

void re4vr_purecall_handler() {
    ::_exit(3);
}
} // namespace

void install_crash_diagnostics() {
    static bool installed = false;

    if (installed) {
        return;
    }

    installed = true;
    g_prev_terminate = std::set_terminate(&re4vr_terminate_handler);
    ::signal(SIGABRT, &re4vr_sigabrt_handler);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::_set_invalid_parameter_handler(&re4vr_invalid_parameter_handler);
    ::_set_purecall_handler(&re4vr_purecall_handler);
}

void set_mods_gated(bool gated) {
    g_mods_gated = gated;
}

bool mods_gated() {
    return g_mods_gated;
}

std::string obj_name(::REManagedObject* o) {
    if (!obj_ok(o)) {
        return {};
    }

    auto* n = call_safe<::REManagedObject*>(o, "get_Name");

    if (n == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
    } catch (...) {
        return {};
    }
}

bool j_bool(const nlohmann::json& d, const char* key, bool def) {
    if (!d.is_object() || !d.contains(key) || d[key].is_null()) {
        return def;
    }

    return d[key].is_boolean() ? d[key].get<bool>() : def;
}

float j_num(const nlohmann::json& d, const char* key, float def) {
    if (!d.is_object() || !d.contains(key) || !d[key].is_number()) {
        return def;
    }

    return d[key].get<float>();
}

std::string j_str(const nlohmann::json& d, const char* key, const char* def) {
    if (!d.is_object() || !d.contains(key) || !d[key].is_string()) {
        return def != nullptr ? std::string{def} : std::string{};
    }

    return d[key].get<std::string>();
}

std::string weapon_key_from_id(int32_t wid) {
    if (wid < 0) {
        return {};
    }

    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "wp%04d", wid);
    return std::string{buf};
}

// ---------------------------------------------------------------------------
// [TAUNT-WAV 2026-09-12] Eigene Sprach-WAVs aus der DLL. Siehe RE4VR.hpp.
// ---------------------------------------------------------------------------
namespace {

// Reihenfolge und Namen 1:1 wie in resources/re4_taunts.rc.
const char* const TAUNT_WAV_NAMES[TAUNT_WAV_COUNT] = {
    "TAUNT01", "TAUNT02", "TAUNT03", "TAUNT04", "TAUNT05",
    "TAUNT06", "TAUNT07", "TAUNT08", "TAUNT09", "TAUNT10",
    "TAUNT11", "TAUNT12", "TAUNT13", "TAUNT14", "TAUNT15",
    // [SEPARIERT 13.09.2026] Ab hier NUR Gesten -- der Tiergriff kommt an diese
    // Indizes nicht heran (taunt_wav_next fuellt nur bis TAUNT_WAV_CHOKE_COUNT).
    "TAUNT16", "TAUNT17", "TAUNT18", "TAUNT19", "TAUNT20",
    "TAUNT21", "TAUNT22", "TAUNT23", "TAUNT24", "TAUNT25",
    "TAUNT26", "TAUNT27",
    // [ADA 17.09.2026] Adas eigene -- nur ueber ihren Pool in TAUNT_TABLE.
    "TAUNT28", "TAUNT29", "TAUNT30", "TAUNT31", "TAUNT32",
    "TAUNT33", "TAUNT34", "TAUNT35", "TAUNT36", "TAUNT37",
    "TAUNT38", "TAUNT39", "TAUNT40", "TAUNT41",
};
}   // namespace

// Lautstaerke in dB, 0 = Datei unveraendert. Liegt hier und nicht im Choke,
// weil BEIDE Verwender (Tiergriff und Mittelfinger-Geste) denselben Pegel
// benutzen sollen -- gestellt wird er aus der Choke-Config.
float g_taunt_gain_db = 0.0f;

void set_taunt_wav_gain_db(float db) {
    g_taunt_gain_db = std::clamp(db, -40.0f, 12.0f);
}

float taunt_wav_gain_db() {
    return g_taunt_gain_db;
}

// [ADA LAUTER 17.09.2026] Nur Adas Zuschlag -- der allgemeine Regler bleibt.
float g_ada_extra_db = 8.0f;

void set_ada_wav_extra_db(float db) {
    g_ada_extra_db = std::clamp(db, -20.0f, 20.0f);
}

float ada_wav_extra_db() {
    return g_ada_extra_db;
}

// [ACHIEVEMENT 13.09.2026] Der eigentliche Abspielweg, seit dem Achievement-
// Jingle mit dem Resource-NAMEN statt einem Taunt-Index. Inhalt unveraendert --
// play_taunt_wav reicht nur noch TAUNT_WAV_NAMES[index] hier herein, damit der
// dB-Regler fuer JEDES eigene WAV derselbe bleibt.
// [EIGENE LAUTSTAERKE 16.09.2026] db_override: dieser eine Aufruf spielt mit
// dem uebergebenen Pegel statt mit dem globalen Regler. Gebraucht fuer Ashleys
// Chat-Sprueche, deren Lautstaerke an IHREM ABSTAND haengt -- der allgemeine
// dB-Regler soll davon unberuehrt bleiben.
// [WAV-SERIE 20.09.2026] Jeder gestartete eigene Ton zaehlt eine Nummer hoch.
// Wer eine Mundbewegung an einen Ton haengt, merkt sich die Nummer und stoppt,
// sobald ein anderer Ton dazwischenfunkt -- sonst redet der Mund weiter,
// waehrend der Ton laengst abgeschnitten ist (SND_PURGE).
uint64_t g_wav_serial = 0;

uint64_t wav_serial() {
    return g_wav_serial;
}

static bool play_wav_resource(const char* res_name, const float* db_override = nullptr) {
    if (res_name == nullptr) {
        return false;
    }

    ++g_wav_serial;

    const float gain_db = db_override != nullptr ? *db_override : g_taunt_gain_db;

    // Die Resource liegt in UNSERER DLL, nicht im Spielmodul.
    auto* module_handle = REFramework::get_reframework_module();

    if (module_handle == nullptr) {
        return false;
    }

    // [LAUTSTAERKE 2026-09-12 -- Ansage "slider, nicht jedes Mal fragen"]
    // PlaySound hat KEINEN Volume-Parameter, und das Session-Volume zu drehen
    // wuerde den ganzen Spielton mitregeln. Also die Samples selbst skalieren
    // und per SND_MEMORY aus dem Speicher spielen statt per SND_RESOURCE.
    //
    // [IMMER ZUERST STOPPEN 20.09.2026 -- Ansage] Jeder neue eigene Ton
    // schneidet den laufenden ab, egal von welchem Modul er kommt. Vorher tat
    // das nur der skalierte Weg (SND_MEMORY); der Resource-Weg bei 0 dB lief
    // daneben weiter, dadurch klangen zwei WAVs gleichzeitig.
    PlaySoundA(nullptr, nullptr, SND_PURGE);

    // Bei 0 dB wird nichts gerechnet -- dann direkt aus der Resource.
    if (std::abs(gain_db) < 0.01f) {
        return PlaySoundA(res_name, module_handle,
                          SND_RESOURCE | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    auto* res = FindResourceA(module_handle, res_name, "WAVE");

    if (res == nullptr) {
        return false;
    }

    auto* handle = LoadResource(module_handle, res);
    const auto* src = handle != nullptr
        ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
    const DWORD size = SizeofResource(module_handle, res);

    if (src == nullptr || size < 44) {
        return false;
    }

    // EIN Puffer reicht: es spielt ohnehin nur ein WAV, der naechste schneidet
    // ab. Er muss aber statisch sein -- SND_ASYNC liest noch daraus, nachdem
    // PlaySound zurueckgekehrt ist.
    static std::vector<uint8_t> buf;

    // (Gestoppt wurde oben schon -- der Puffer darf erst danach neu befuellt
    // werden, sonst liest der laufende Ton aus ihm und es knackt.)
    buf.assign(src, src + size);

    // RIFF-Chunks durchlaufen und nur den data-Block skalieren. Header
    // anzufassen waere fatal, und der data-Chunk steht nicht immer bei 44.
    const float gain = std::pow(10.0f, gain_db / 20.0f);
    size_t off = 12;   // "RIFF" + Groesse + "WAVE"

    while (off + 8 <= buf.size()) {
        uint32_t csize = 0;
        std::memcpy(&csize, buf.data() + off + 4, sizeof(csize));

        const bool is_data = std::memcmp(buf.data() + off, "data", 4) == 0;
        const size_t body = off + 8;
        const size_t avail = buf.size() - body;
        const size_t n = csize <= avail ? csize : avail;

        if (is_data) {
            // 16 bit signed, wie in allen unseren Dateien (Vorgabe mono/48k/16).
            auto* smp = reinterpret_cast<int16_t*>(buf.data() + body);

            for (size_t k = 0; k + 1 < n; k += 2, ++smp) {
                const float v = static_cast<float>(*smp) * gain;
                *smp = static_cast<int16_t>(
                    v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }

            break;
        }

        off = body + n + (n & 1);   // Chunks sind auf gerade Laenge gepolstert
    }

    return PlaySoundA(reinterpret_cast<const char*>(buf.data()), nullptr,
                      SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

// [JIGGLE 19.09.2026] Klatscher ASHLEYSLAP01..03 -- mit eigenem Pegel (db =
// globaler Regler + Slap-Zuschlag, gerechnet in RE4VRJiggle).
bool play_slap_wav(int index, float db) {
    if (index < 0 || index >= SLAP_WAV_COUNT) {
        return false;
    }

    char name[16]{};
    std::snprintf(name, sizeof(name), "ASHLEYSLAP%02d", index + 1);

    const float gain = std::clamp(db, -40.0f, 12.0f);

    return play_wav_resource(name, &gain);
}

bool play_taunt_wav(int index) {
    if (index < 0 || index >= TAUNT_WAV_COUNT) {
        return false;
    }

    return play_wav_resource(TAUNT_WAV_NAMES[index]);
}

bool play_taunt_wav_db(int index, float db) {
    if (index < 0 || index >= TAUNT_WAV_COUNT) {
        return false;
    }

    const float gain = std::clamp(db, -40.0f, 12.0f);

    return play_wav_resource(TAUNT_WAV_NAMES[index], &gain);
}

// [HUELLKURVE ZUR LAUFZEIT 16.09.2026 -- Ansage des Users: "ich adde morgen
// noch ein paar WAVs"] Die Lautstaerke-Kurve einer Ashley-WAV wird NICHT
// vorher erzeugt, sondern beim ersten Abspielen aus der eingebetteten Datei
// selbst gerechnet und dann behalten. Neue WAVs sind damit automatisch dabei:
// rein in die .rc, ASHLEY_WAV_COUNT hoch, fertig.
//
// Ein Wert je 25 ms, 0..255. Aus dem RIFF-Block wird nur gelesen, was gebraucht
// wird: 16-Bit-PCM, erster Kanal.
std::vector<uint8_t> wav_envelope(const char* res_name) {
    std::vector<uint8_t> env{};

    auto* module_handle = REFramework::get_reframework_module();

    if (module_handle == nullptr || res_name == nullptr) {
        return env;
    }

    auto* res = FindResourceA(module_handle, res_name, "WAVE");
    auto* handle = res != nullptr ? LoadResource(module_handle, res) : nullptr;
    const auto* data = handle != nullptr
        ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
    const auto size = res != nullptr ? SizeofResource(module_handle, res) : 0u;

    if (data == nullptr || size < 44) {
        return env;
    }

    // RIFF durchgehen: fmt fuer Kanaele/Rate, data fuer die Samples.
    uint32_t pos = 12;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t rate = 48000;
    const uint8_t* pcm = nullptr;
    uint32_t pcm_size = 0;

    while (pos + 8 <= size) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(data + pos);
        const uint32_t len = *reinterpret_cast<const uint32_t*>(data + pos + 4);

        if (id == 0x20746d66u && pos + 8 + 16 <= size) {          // 'fmt '
            channels = *reinterpret_cast<const uint16_t*>(data + pos + 10);
            rate = *reinterpret_cast<const uint32_t*>(data + pos + 12);
            bits = *reinterpret_cast<const uint16_t*>(data + pos + 22);
        } else if (id == 0x61746164u) {                            // 'data'
            pcm = data + pos + 8;
            pcm_size = (pos + 8 + len <= size) ? len : (size - pos - 8);

            break;
        }

        pos += 8 + len + (len & 1);
    }

    if (pcm == nullptr || pcm_size == 0 || bits != 16 || channels == 0 || rate == 0) {
        return env;
    }

    const auto* samples = reinterpret_cast<const int16_t*>(pcm);
    const uint32_t count = pcm_size / 2;
    const uint32_t step = (rate / 40) * channels;   // 25 ms

    if (step == 0) {
        return env;
    }

    std::vector<double> rms{};
    double peak = 0.0;

    for (uint32_t i = 0; i + 1 < count; i += step) {
        double summe = 0.0;
        uint32_t n = 0;

        for (uint32_t k = i; k < i + step && k < count; k += channels) {
            const double s = static_cast<double>(samples[k]);

            summe += s * s;
            ++n;
        }

        const double v = (n > 0) ? std::sqrt(summe / n) : 0.0;

        rms.push_back(v);

        if (v > peak) {
            peak = v;
        }
    }

    if (peak <= 0.0) {
        return env;
    }

    env.reserve(rms.size());

    for (double v : rms) {
        // Leicht angehoben (Wurzelkennlinie): so wirkt Sprache im Mund
        // lebendiger als bei linearer Lautstaerke.
        const double u = std::pow(v / peak, 0.6);

        env.push_back(static_cast<uint8_t>(std::clamp(u * 255.0, 0.0, 255.0)));
    }

    return env;
}

// [ASHLEY 13.09.2026] Ihre eigenen Sprueche (Resourcen ASHLEY01..08). Eigener
// Namensraum aus demselben Grund wie beim Jingle: kein fremder Beutel, keine
// Geste kommt daran. Lautstaerke wieder ueber denselben dB-Regler.
bool play_ashley_wav(int index) {
    if (index < 0 || index >= ASHLEY_WAV_COUNT) {
        return false;
    }

    char name[16]{};
    std::snprintf(name, sizeof(name), "ASHLEY%02d", index + 1);

    return play_wav_resource(name);
}

// [ASHLEY ANTWORTET 16.09.2026] Ihre zwei Antwort-Poels (ASHLEYPOINTnn und
// ASHLEYFUCKnn). Eigener Namensraum wie bei allen anderen, und eine EIGENE
// Lautstaerke je Aufruf -- die haengt an ihrem Abstand und ist jedes Mal anders.
int ashley_reply_count(bool fuck) {
    return fuck ? ASHLEY_FUCK_WAV_COUNT : ASHLEY_POINT_WAV_COUNT;
}

void ashley_reply_res_name(bool fuck, int index, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    std::snprintf(out, out_size, fuck ? "ASHLEYFUCK%02d" : "ASHLEYPOINT%02d", index + 1);
}

bool play_ashley_reply_wav(bool fuck, int index, float db) {
    if (index < 0 || index >= ashley_reply_count(fuck)) {
        return false;
    }

    char name[24]{};
    ashley_reply_res_name(fuck, index, name, sizeof(name));

    const float gain = std::clamp(db, -40.0f, 12.0f);

    return play_wav_resource(name, &gain);
}

// [AUSSORTIERT 13.09.2026 -- Ansage "taunt07_imgoing komplett rausnehmen"]
// NULL-basierte Indizes, die nicht mehr gezogen werden. Die WAVs bleiben in der
// DLL (die Namen TAUNT01..15 und damit jeder andere Index bleiben stabil) --
// sie kommen nur nicht mehr vor. Aus dem Mittelfinger-Pool ist der Eintrag
// getrennt davon aus TAUNT_TABLE entfernt.
constexpr int TAUNT_WAV_SKIP[] = {
    6,   // taunt07_imgoing
};

bool taunt_wav_skipped(int i) {
    for (int s : TAUNT_WAV_SKIP) {
        if (s == i) {
            return true;
        }
    }

    return false;
}

int taunt_wav_next(std::vector<int>& bag, int* last) {
    if (bag.empty()) {
        bag.clear();
        // [SEPARIERT 13.09.2026] NUR die ersten 15: einziger Aufrufer ist der
        // Tiergriff (Griff-Flanke und Probe-Knopf in RE4VRChoke), und der
        // bleibt unveraendert. Die WAVs ab 16 gehoeren allein den Gesten.
        bag.reserve(TAUNT_WAV_CHOKE_COUNT);

        for (int i = 0; i < TAUNT_WAV_CHOKE_COUNT; ++i) {
            if (!taunt_wav_skipped(i)) {
                bag.push_back(i);
            }
        }

        if (bag.empty()) {
            return -1;   // alles aussortiert -> nichts zu spielen
        }

        // Fisher-Yates von hinten -- dieselbe Schleife wie RE4VRGuestures::taunt_next.
        for (size_t i = bag.size(); i >= 2; --i) {
            const size_t j = static_cast<size_t>(std::rand() % static_cast<int>(i));
            std::swap(bag[i - 1], bag[j]);
        }

        // [KEINE WIEDERHOLUNG 13.09.2026] Gezogen wird von HINTEN. Faellt dort
        // derselbe Spruch wie der zuletzt gespielte, kaeme er zweimal
        // nacheinander -- die einzige Stelle, an der das ueberhaupt passieren
        // kann, denn innerhalb eines Beutels ist jeder genau einmal dran.
        // Getauscht wird mit dem vordersten Platz: eine Vertauschung, keine
        // neue Ziehung, damit der Beutel vollstaendig bleibt.
        if (last != nullptr && *last >= 0 && bag.size() > 1 && bag.back() == *last) {
            std::swap(bag.back(), bag.front());
        }
    }

    const int idx = bag.back();
    bag.pop_back();

    if (last != nullptr) {
        *last = idx;
    }

    return idx;
}

// [ASHLEY 13.09.2026] Derselbe Shuffle-Beutel, nur ueber ihre acht Sprueche.
// Bewusst eine eigene Funktion statt eines Parameters an taunt_wav_next: dort
// haengt die Fuellung an TAUNT_WAV_CHOKE_COUNT und an der Skip-Liste, beides
// gilt fuer sie nicht.
int ashley_wav_next(std::vector<int>& bag, int* last) {
    if (bag.empty()) {
        bag.reserve(ASHLEY_WAV_COUNT);

        for (int i = 0; i < ASHLEY_WAV_COUNT; ++i) {
            bag.push_back(i);
        }

        for (size_t i = bag.size(); i >= 2; --i) {
            const size_t j = static_cast<size_t>(std::rand() % static_cast<int>(i));
            std::swap(bag[i - 1], bag[j]);
        }

        // Wie beim Tiergriff: an der Beutelgrenze nie zweimal derselbe.
        if (last != nullptr && *last >= 0 && bag.size() > 1 && bag.back() == *last) {
            std::swap(bag.back(), bag.front());
        }
    }

    const int idx = bag.back();
    bag.pop_back();

    if (last != nullptr) {
        *last = idx;
    }

    return idx;
}

// [ASHLEY ANTWORTET 16.09.2026] Beutel ueber einen der beiden Antwort-Poels.
// Gleiche Bauform wie ashley_wav_next; leerer Pool (noch keine Dateien) gibt -1
// zurueck, der Aufrufer spielt dann nichts.
int ashley_reply_wav_next(bool fuck, std::vector<int>& bag, int* last) {
    const int anzahl = ashley_reply_count(fuck);

    if (anzahl <= 0) {
        return -1;
    }

    if (bag.empty()) {
        bag.reserve(static_cast<size_t>(anzahl));

        for (int i = 0; i < anzahl; ++i) {
            bag.push_back(i);
        }

        for (size_t i = bag.size(); i >= 2; --i) {
            const size_t j = static_cast<size_t>(std::rand() % static_cast<int>(i));
            std::swap(bag[i - 1], bag[j]);
        }

        if (last != nullptr && *last >= 0 && bag.size() > 1 && bag.back() == *last) {
            std::swap(bag.back(), bag.front());
        }
    }

    const int idx = bag.back();
    bag.pop_back();

    if (last != nullptr) {
        *last = idx;
    }

    return idx;
}

} // namespace re4vr

std::shared_ptr<RE4VR>& RE4VR::get() {
    static auto inst = std::make_shared<RE4VR>();
    return inst;
}

std::optional<std::string> RE4VR::on_initialize() {
    // [DIAGNOSE 04.09.2026] Muss als Erstes laufen: der Traeger steht vor allen
    // anderen RE4VR-Mods im Vektor.
    re4vr::install_crash_diagnostics();

    return Mod::on_initialize();
}

void RE4VR::on_lua_state_created(sol::state& lua) {
    // [PHASENSTUB] Das Stub-Script ruft genau das hier.

    // [FRAME-CACHE] Das Lua-Modul `re4vr/re4_vr_frame_cache` wird durch die
    // NATIVE Fassung ersetzt -- dieselbe API, aber ohne Lua-Aufrufe. Die neun
    // verbliebenen autorun-Dateien (binding, reload*, weapons*) bekommen sie
    // ueber ihr unveraendertes `require`, weil require zuerst in
    // package.loaded nachsieht. Damit kann die .lua-Datei mit den anderen nach
    // c++/ wandern, ohne dass ein einziges Script angefasst werden muss.
    auto t = lua.create_table();
    t["on"] = []() { return re4vr::fc::on(); };
    t["ctx"] = []() { return re4vr::fc::ctx(); };
    t["body_go"] = []() { return re4vr::fc::body_go(); };
    t["body_tf"] = []() { return re4vr::fc::body_tf(); };
    t["pe"] = []() { return re4vr::fc::pe(); };
    t["equip_wid"] = []() { return re4vr::fc::equip_wid(); };
    t["get_managed_singleton"] = [](const std::string& name) {
        return re4vr::fc::managed_singleton(name.c_str());
    };

    // [VIGEM 2026-09-04] Einmal beim Start melden, ob der ViGEmBus-Treiber
    // erreichbar ist. Legt KEIN Pad an (probe verbindet nur). Der native
    // binding-Port braucht diese Schicht; ohne Treiber bliebe er wirkungslos,
    // und dann soll das hier stehen statt es im Spiel zu suchen.
    lua["__re4_vigem_ready"] = VigemPad::probe();

    lua["__re4_frame_cache"] = t;
    lua["package"]["loaded"]["re4vr/re4_vr_frame_cache"] = t;
}

void RE4VR::on_lua_state_destroyed(sol::state& lua) {
    // "Reset Scripts" nimmt die Lua-Seite mit -- dieses Modul ueberlebt es. Was
    // in der Lua-Fassung ein Neu-Laden der Datei erledigt hat, muss deshalb hier
    // passieren: Handles weg, Caches leer, Config neu lesen.
    //
    // [FRAME-CACHE] Die gehaltenen Zeiger sind nach einem Reset nicht mehr
    // verlaesslich -- leeren.
    re4vr::fc::reset();
}

void RE4VR::on_frame() {
    re4vr::clear_vm_exception();
}

void RE4VR::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    // ======================================================================
    // [FRAMETIME-MESSUNG 04.09.2026] Wer kostet wieviel -- pro MODUL.
    // Der Fork misst selbst nur pro PHASE; das benennt keinen Verursacher.
    //
    // [2026-09-08] Der Mitschnitt in eine Datei ist ausgebaut -- die Werte
    // stehen nur noch hier im Tree.
    // ======================================================================
    if (!ImGui::TreeNode("RE4VR - Frametimes")) {
        return;
    }

    ImGui::Text("Die Messung laeuft IMMER -- kein Haken noetig.");
    ImGui::Text("Die Werte stehen nur hier; es wird keine Datei mehr geschrieben.");

    if (ImGui::Button("Zaehler zuruecksetzen##perf_reset")) {
        re4vr::perf::reset();
    }

    ImGui::Separator();
    ImGui::Text("Summe unserer Module:  Mittel %.3f ms   Spitze %.3f ms",
                re4vr::perf::frame_avg_us() / 1000.0, re4vr::perf::frame_max_us() / 1000.0);
    ImGui::Separator();
    ImGui::Text("%-34s %10s %10s %10s", "Modul [Phase]", "Mittel ms", "Spitze ms", "letzter");

    for (const auto& r : re4vr::perf::rows()) {
        // Alles unter 0,01 ms im Mittel ist Rauschen und macht die Liste nur lang.
        if (r.avg_us < 10.0 && r.max_us < 1000.0) {
            continue;
        }

        ImGui::Text("%-34s %10.3f %10.3f %10.3f", r.name.c_str(), r.avg_us / 1000.0,
                    r.max_us / 1000.0, r.last_us / 1000.0);
    }

    ImGui::TreePop();
}

void RE4VR::on_pre_application_entry(void* entry, const char* name, size_t hash) {
}

void RE4VR::on_application_entry(void* entry, const char* name, size_t hash) {
}

#endif // RE4
