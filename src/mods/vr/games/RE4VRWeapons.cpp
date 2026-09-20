// ============================================================================
// RE4VRWeapons -- 1:1-Portierung von re4_vr_weapons.lua (5.188 Zeilen).
// Spezifikation: I:\LUATRANS\PORT_WEAPONS_SPEC.md
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/MotionFsm2Layer.hpp>

#include <utility/String.hpp>

// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRKillswitch.hpp"
#include "RE4VRChoke.hpp"
#include "RE4VRWeapons.hpp"

// windows.h definiert min/max als MAKROS -- der Fork setzt kein NOMINMAX.
#undef min
#undef max

namespace {

// Lua: os.clock(). Fremde Scripte schreiben Zeitstempel in dieselben Globals,
// deshalb MUSS die Zeitbasis std::clock()/CLOCKS_PER_SEC sein.
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

sdk::REMethodDefinition* find_method(::REManagedObject* obj, std::string_view name) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    return td != nullptr ? td->get_method(name) : nullptr;
}

bool clear_pending(sdk::VMContext* context, bool ok) {
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        return false;
    }

    return ok;
}

// ValueType-Rueckgaben brauchen den sret-Puffer PLUS ein Erfolgs-Flag.
// Siehe [[reference_re4_cpp_valuetype_rueckgabe_sret]].
bool get_vec4(::REManagedObject* obj, std::string_view name, glm::vec4& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{};
    bool ok = false;

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj);
        ok = true;
    } catch (...) {
        ok = false;
    }

    ok = clear_pending(context, ok);

    if (ok) {
        out = buf;
    }

    return ok;
}

bool get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out) {
    glm::vec4 v{};

    if (!get_vec4(obj, name, v)) {
        return false;
    }

    out = glm::vec3{v.x, v.y, v.z};

    return true;
}

bool get_quat(::REManagedObject* obj, std::string_view name, glm::quat& out) {
    glm::vec4 v{};

    if (!get_vec4(obj, name, v)) {
        return false;
    }

    // via.Quaternion liegt als x,y,z,w im Speicher; glm::quat ist w,x,y,z.
    out = glm::quat{v.w, v.x, v.y, v.z};

    return true;
}

bool set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{v.x, v.y, v.z, 0.0f};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
        return false;
    }

    clear_pending(context, true);

    return true;
}

bool set_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{q.x, q.y, q.z, q.w};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
        return false;
    }

    clear_pending(context, true);

    return true;
}

// Enum-GETTER als Zahl lesen. Lua: `if type(v)=="number" then v else v.value__`.
// Als OBJEKTZEIGER gelesen ist ein Enum toedlich (der Choke-Absturz).
std::optional<int32_t> enum_as_int(::REManagedObject* obj, std::string_view getter) {
    if (!re4vr::obj_ok(obj)) {
        return std::nullopt;
    }

    int32_t v = 0;

    if (re4vr::try_call<int32_t>(obj, getter, v)) {
        return v;
    }

    auto* w = re4vr::call_safe<::REManagedObject*>(obj, getter);

    return w != nullptr ? re4vr::get_field_int(w, "value__") : std::nullopt;
}

std::string obj_name_of(::REManagedObject* obj) {
    if (!re4vr::obj_ok(obj)) {
        return {};
    }

    return re4vr::obj_name(obj);
}

std::optional<uintptr_t> addr_of(::REManagedObject* o) {
    if (o == nullptr) {
        return std::nullopt;
    }

    return reinterpret_cast<uintptr_t>(o);
}




// ---------------------------------------------------------------------------
// Transform-Parent-Joint. Luas get_ParentJoint liefert ein via.Joint ODER
// einen String; gebraucht wird der NAME. set_ParentJoint will einen managed
// String.
// ---------------------------------------------------------------------------
std::string transform_parent_joint_name(::REManagedObject* tf) {
    if (!re4vr::obj_ok(tf)) {
        return {};
    }

    auto* j = re4vr::call_safe<::REManagedObject*>(tf, "get_ParentJoint");

    if (j == nullptr) {
        return {};
    }

    return re4vr::obj_name(j);
}

void transform_set_parent_joint(::REManagedObject* tf, const std::string& name) {
    if (!re4vr::obj_ok(tf)) {
        return;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(name));

    if (str == nullptr) {
        return;
    }

    auto* m = find_method(tf, "set_ParentJoint");

    if (m == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();

    try {
        m->call_safe<void*>(context, tf, str);
    } catch (...) {
    }

    clear_pending(context, true);
}

::REManagedObject* transform_joint_by_name(::REManagedObject* tf, const std::string& name) {
    if (!re4vr::obj_ok(tf) || name.empty()) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(name));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str);
}

// Ein Feld aus einer LUA-TABELLE lesen (managed object / vec3).
::REManagedObject* lua_table_pointer(const char* table, const char* field) {
    re4vr::LuaRef lua;

    if (lua == nullptr) {
        return nullptr;
    }

    try {
        sol::object t = (*lua)[table];

        if (!t.valid() || t.get_type() != sol::type::table) {
            return nullptr;
        }

        sol::object o = t.as<sol::table>()[field];

        if (!o.valid() || o.get_type() != sol::type::userdata) {
            return nullptr;
        }

        return o.as<::REManagedObject*>();
    } catch (...) {
    }

    return nullptr;
}

std::optional<glm::vec3> lua_table_vec3(const char* table, const char* field) {
    re4vr::LuaRef lua;

    if (lua == nullptr) {
        return std::nullopt;
    }

    try {
        sol::object t = (*lua)[table];

        if (!t.valid() || t.get_type() != sol::type::table) {
            return std::nullopt;
        }

        sol::object o = t.as<sol::table>()[field];

        if (!o.valid() || !o.is<glm::vec3>()) {
            return std::nullopt;
        }

        return o.as<glm::vec3>();
    } catch (...) {
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// get_WorldAABB -- ValueType-Rueckgabe ueber den sret-Puffer.
// Muster 1:1 aus RE4VRWhitelist. Die Feld-Offsets MUESSEN ueber
// get_offset_from_fieldptr() kommen: sdk::REField::get_data reicht
// is_value_type gar nicht durch und rechnet immer mit get_offset_from_base().
// !! via.AABB:getCenter NICHT BENUTZEN !! -- liefert PLAUSIBLEN Muell.
// ---------------------------------------------------------------------------
bool mesh_world_aabb(::REManagedObject* mesh, glm::vec3& mn_out, glm::vec3& mx_out) {
    if (mesh == nullptr) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(mesh);
    auto* method = td != nullptr ? td->get_method("get_WorldAABB") : nullptr;

    if (method == nullptr) {
        return false;
    }

    __declspec(align(16)) uint8_t aabb_buf[64]{};
    auto context = sdk::get_thread_context();
    bool ok_call = false;

    try {
        method->call_safe<uint8_t*>(aabb_buf, context, mesh);
        ok_call = true;
    } catch (...) {
        ok_call = false;
    }

    re4vr::clear_vm_exception();

    if (!ok_call) {
        return false;
    }

    auto* aabb_td = sdk::find_type_definition("via.AABB");
    auto* f_min = aabb_td != nullptr ? aabb_td->get_field("minpos") : nullptr;
    auto* f_max = aabb_td != nullptr ? aabb_td->get_field("maxpos") : nullptr;

    if (f_min == nullptr || f_max == nullptr) {
        return false;
    }

    const auto off_min = f_min->get_offset_from_fieldptr();
    const auto off_max = f_max->get_offset_from_fieldptr();

    if (off_min + sizeof(glm::vec3) > sizeof(aabb_buf)
        || off_max + sizeof(glm::vec3) > sizeof(aabb_buf)) {
        return false;
    }

    std::memcpy(&mn_out, aabb_buf + off_min, sizeof(mn_out));
    std::memcpy(&mx_out, aabb_buf + off_max, sizeof(mx_out));

    return true;
}

// setRay(via.vec3, via.vec3) -- zwei 16-Byte-Puffer (Muster aus RE4VRCrosshair).
void cast_ray_set(::REManagedObject* q, const glm::vec3& from, const glm::vec3& to) {
    auto* m = find_method(q, "setRay(via.vec3, via.vec3)");

    if (m == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 a{from.x, from.y, from.z, 0.0f};
    __declspec(align(16)) glm::vec4 b{to.x, to.y, to.z, 0.0f};

    // [PORTFIX 2026-09-06] ueber invoke, wie Lua. Der rohe Weg uebergibt die
    // beiden via.vec3 nach C-Konvention; build_args legt stattdessen ZEIGER auf
    // je einen Vector4f in die Argumentliste. Traegt die Query den Strahl
    // nicht, meldet sie sauber "fertig" und NULL Kontakte -- genau das Bild
    // aus jedem Wurf (auch der Terrain-Strahl blieb blind).
    try {
        std::array<void*, 2> args{static_cast<void*>(&a), static_cast<void*>(&b)};
        m->invoke(q, std::span<void*>(args));
    } catch (...) {
    }

    clear_pending(context, true);
}

// Ein float-Feld lesen (Luas `cp:get_field("Distance")`).
std::optional<float> field_float(::REManagedObject* obj, const char* name) {
    if (!re4vr::obj_ok(obj)) {
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* f = td != nullptr ? td->get_field(name) : nullptr;

    if (f == nullptr) {
        return std::nullopt;
    }

    try {
        auto* raw = f->get_data_raw(obj, false);

        if (raw == nullptr) {
            return std::nullopt;
        }

        return *reinterpret_cast<float*>(raw);
    } catch (...) {
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Generischer Aufruf einer globalen LUA-Funktion.
//
// Gebraucht, solange Geber und Nehmer gemischt sind: weapons2, whitelist und
// reload stellen Funktionen bereit, die hier gerufen werden. Faellt weg, sobald
// diese Dateien portiert sind.
// ---------------------------------------------------------------------------
template <typename Ret, typename... Args>
std::optional<Ret> lua_call_global(const char* name, Args&&... args) {
    re4vr::LuaRef lua;

    if (lua == nullptr) {
        return std::nullopt;
    }

    try {
        sol::object f = (*lua)[name];

        if (!f.valid() || f.get_type() != sol::type::function) {
            return std::nullopt;
        }

        sol::protected_function fn = f;
        auto r = fn(std::forward<Args>(args)...);

        if (!r.valid()) {
            return std::nullopt;
        }

        sol::object o = r;

        if (!o.valid() || !o.is<Ret>()) {
            return std::nullopt;
        }

        return o.as<Ret>();
    } catch (...) {
    }

    return std::nullopt;
}

template <typename... Args>
void lua_call_global_void(const char* name, Args&&... args) {
    re4vr::LuaRef lua;

    if (lua == nullptr) {
        return;
    }

    try {
        sol::object f = (*lua)[name];

        if (!f.valid() || f.get_type() != sol::type::function) {
            return;
        }

        sol::protected_function fn = f;
        (void)fn(std::forward<Args>(args)...);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// MotionFsm2-Baum -- NATIVE Klassen statt der REFramework-Lua-Bindings.
// Muster 1:1 aus RE4VRMovement (dort ausfuehrlich begruendet).
//
// INDEX-FALLE: `get_actions()` liefert einen std::vector, den sol als
// EINS-basierte Lua-Tabelle zeigt. Luas `actions[4]` ist also NATIV Index 3.
// ---------------------------------------------------------------------------
sdk::behaviortree::TreeObject* fsm_tree_layer(::REManagedObject* mfsm, int32_t layer_idx) {
    if (mfsm == nullptr) {
        return nullptr;
    }

    auto* layer = re4vr::call_safe<sdk::MotionFsm2Layer*>(mfsm, "getLayer", layer_idx);

    if (layer == nullptr) {
        return nullptr;
    }

    return layer->get_tree_object();
}

// Luas `act._StartFrame` (lesen).
std::optional<double> act_get_num(::REManagedObject* act, const char* name) {
    if (act == nullptr) {
        return std::nullopt;
    }

    auto def = utility::re_managed_object::get_type_definition(act);
    auto* f = def != nullptr ? def->get_field(name) : nullptr;
    auto* ft = f != nullptr ? f->get_type() : nullptr;

    if (ft == nullptr) {
        return std::nullopt;
    }

    try {
        auto* raw = f->get_data_raw(act, false);

        if (raw == nullptr) {
            return std::nullopt;
        }

        const auto sz = ft->get_valuetype_size();

        if (sz == 4) {
            return static_cast<double>(*reinterpret_cast<float*>(raw));
        }

        if (sz == 8) {
            return *reinterpret_cast<double*>(raw);
        }
    } catch (...) {
    }

    return std::nullopt;
}

// Luas `act._StartFrame = x` / `act._OverwriteInterpolation = b`.
bool act_set_num(::REManagedObject* act, const char* name, double value) {
    if (act == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(act);
    auto* f = def != nullptr ? def->get_field(name) : nullptr;

    if (f == nullptr) {
        return false;
    }

    auto* ft = f->get_type();
    const std::string tn = ft != nullptr ? ft->get_full_name() : std::string{};

    try {
        auto* raw = f->get_data_raw(act, false);

        if (raw == nullptr) {
            return false;
        }

        if (tn == "System.Single") {
            *reinterpret_cast<float*>(raw) = static_cast<float>(value);
        } else if (tn == "System.Double") {
            *reinterpret_cast<double*>(raw) = value;
        } else if (tn == "System.Boolean") {
            *reinterpret_cast<bool*>(raw) = value != 0.0;
        } else {
            *reinterpret_cast<int32_t*>(raw) = static_cast<int32_t>(value);
        }

        return true;
    } catch (...) {
    }

    return false;
}

// ---------------------------------------------------------------------------
// Konstanten aus der Lua
// ---------------------------------------------------------------------------

bool is_knife_id(int32_t wid) {
    switch (wid) {
    case 5000:
    case 5001:
    case 5002:
    case 5003:
    case 5006:
    case 6107:
    case 6108:
    case 6305:   // Hot Dogger (Mercenaries)
        return true;
    default:
        return false;
    }
}

// [SCOPE_KILLSWITCH] Scope-faehige Waffen.
bool is_scope_weapon(int32_t wid) {
    switch (wid) {
    case 4400:
    case 4401:
    case 4402:
    case 4202:
    case 6105:
    case 6114:
        return true;
    default:
        return false;
    }
}

// [IRON_SIGHT_NATIVE] Die 3 Rifles + die zwei SW-Klone.
bool is_iron_rifle(int32_t wid) {
    return wid == 4400 || wid == 4401 || wid == 4402 || wid == 6105 || wid == 6114;
}

// [BOLT_CYCLE] Nur die Repetierer.
bool is_bolt_rifle(int32_t wid) {
    return wid == 4400 || wid == 6114;
}

// [SCOPE_ID] Stingray sm72-Scope-Familie.
const char* scope_item_name(int32_t id) {
    switch (id) {
    case 116000000:
        return "normal";
    case 116004800:
        return "thermal";
    case 116003200:
        return "hipower";
    default:
        return nullptr;
    }
}

bool is_iron_body_go(const std::string& n) {
    return n == "body" || n == "body_armor" || n == "hair" || n == "head";
}

constexpr const char* HIDE_BODY_CFG_PATH = "re4_vr/re4_vr_hide_body.json";
constexpr const char* BOLT_CFG_PATH = "re4_vr/re4_vr_bolt.json";
constexpr const char* SCOPE_PROTO_CFG_PATH = "re4_vr/re4_vr_scope_proto.json";
constexpr const char* SNAPPY_CFG_PATH = "re4_vr/re4_vr_snappy.json";
constexpr const char* THROW_CFG_PATH = "re4_vr/re4_vr_throw.json";

constexpr const char* HEAD_UPDATER_LEON = "chainsaw.Ch0a0z0HeadUpdater";
constexpr const char* HEAD_UPDATER_ADA = "chainsaw.Ch3a8z0HeadUpdater";

constexpr int32_t GRENADE_ID_MIN = 5400;
constexpr int32_t GRENADE_ID_MAX = 5410;

}   // namespace

// ============================================================================
// Boilerplate
// ============================================================================

std::shared_ptr<RE4VRWeapons>& RE4VRWeapons::get() {
    static auto inst = std::make_shared<RE4VRWeapons>();

    return inst;
}

void RE4VRWeapons::store(Handle& h, ::REManagedObject* obj, bool unconditional) {
    drop(h);

    if (obj == nullptr) {
        return;
    }

    h.obj = obj;

    // Selbst erzeugte Objekte: die refcount-Heuristik verankert sie NICHT.
    if (unconditional) {
        try {
            utility::re_managed_object::add_ref(obj);
            h.reffed = true;
        } catch (...) {
            h.reffed = false;
        }

        return;
    }

    try {
        if (utility::re_managed_object::is_managed_object(obj)) {
            utility::re_managed_object::add_ref(obj);
            h.reffed = true;
        }
    } catch (...) {
        h.reffed = false;
    }
}

void RE4VRWeapons::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        try {
            utility::re_managed_object::release(h.obj);
        } catch (...) {
        }
    }

    h.obj = nullptr;
    h.reffed = false;
}

// ============================================================================
// Baustein 1 -- Helfer + Spieler-Kette (Lua Z.1-160)
// ============================================================================

namespace {

::REManagedObject* get_player_ctx_w() {
    // Lua haengt am Frame-Cache (require "re4vr/re4_vr_frame_cache") und faellt
    // sonst auf die volle Kette zurueck -- inklusive der [STALE-SINGLETON]-
    // Wiederholung, die ein zweites Mal fragt.
    return re4vr::fc::on() ? re4vr::fc::ctx() : re4vr::player_context();
}

}   // namespace

std::optional<int32_t> RE4VRWeapons::get_equip_weapon_id() {
    if (re4vr::fc::on()) {
        return re4vr::fc::equip_wid();
    }

    auto* ctx = get_player_ctx_w();
    auto* h = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                             : nullptr;

    if (!re4vr::obj_ok(h)) {
        return std::nullopt;
    }

    return enum_as_int(h, "get_EquipWeaponID");
}

// Pause menu + attache case: same checks as firstperson/binding.
bool RE4VRWeapons::is_any_menu_open() {
    if (m_attache_mgr.obj == nullptr) {
        if (auto* m = sdk::get_managed_singleton<::REManagedObject>(
                "chainsaw.AttacheCaseManager")) {
            store(m_attache_mgr, m);
        }
    }

    if (m_attache_mgr.obj != nullptr) {
        bool busy = false;

        if (re4vr::try_call<bool>(m_attache_mgr.obj, "get_IsAttacheCaseBusy", busy) && busy) {
            return true;
        }
    }

    if (m_gui_mgr.obj == nullptr) {
        if (auto* m = sdk::get_managed_singleton<::REManagedObject>("chainsaw.GuiManager")) {
            store(m_gui_mgr, m);
        }
    }

    if (m_gui_mgr.obj != nullptr) {
        bool lock = false;

        if (re4vr::try_call<bool>(m_gui_mgr.obj, "get_hasOccupiedPauseMenuSystemLock", lock)
            && lock) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Baustein 2 -- SCOPE (Lua Z.164-538)
// ============================================================================

bool RE4VRWeapons::check_via_scope() {
    const auto wid = get_equip_weapon_id();

    if (!wid.has_value() || !is_scope_weapon(*wid)) {
        return false;
    }

    auto* ctx = get_player_ctx_w();

    if (ctx == nullptr) {
        return false;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (!re4vr::obj_ok(hu)) {
        return false;
    }

    auto* cc = re4vr::call_safe<::REManagedObject*>(hu, "get_CameraController");

    if (!re4vr::obj_ok(cc)) {
        return false;
    }

    // Lua nimmt hier get_field, nicht get_*: _BusyCameraController / _IsViaScope
    auto* busy = re4vr::get_field_object(cc, "_BusyCameraController");

    if (!re4vr::obj_ok(busy)) {
        return false;
    }

    // [FELDBREITE 05.09.2026] Hier stand get_field_int: ueber ein 1-Byte-Bool
    // wurden 4 Byte gelesen, die drei Nachbarbytes gingen mit ein. Die
    // Scope-Erkennung konnte dadurch dauerhaft "an" melden -- genau der
    // erfundene Zustand, den [HOLD_NUR_NACH_ECHTEM_SCOPE] verhindern soll.
    const auto v = re4vr::get_field_bool(busy, "_IsViaScope");

    return v.value_or(false);
}

// [SCOPE_ID] Welches Scope ist montiert? Ground-Truth = equippte
// Scope-Attachment-ItemID (aktualisiert SOFORT beim Wechsel; die
// ScopeController-Felder sind stale bis zum Durchzielen).
void RE4VRWeapons::detect_scope_id() {
    auto* ctx = get_player_ctx_w();

    if (ctx == nullptr) {
        return;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (!re4vr::obj_ok(hu)) {
        return;
    }

    auto* gun = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");

    if (!re4vr::obj_ok(gun)) {
        // keine Gun (Messer/Holster) -> nichts erkannt
        re4vr::lua_set_nil("__re4_scope_id");

        return;
    }

    auto* pc = re4vr::call_safe<::REManagedObject*>(gun, "get_WeaponPartsCustom");
    auto* datas = pc != nullptr ? re4vr::get_field_object(pc, "_Datas") : nullptr;

    if (!re4vr::obj_ok(datas)) {
        re4vr::lua_set_nil("__re4_scope_id");

        return;
    }

    // Lua: get_Count, sonst _size.
    int32_t n = 0;

    if (!re4vr::try_call<int32_t>(datas, "get_Count", n)) {
        n = re4vr::get_field_int(datas, "_size").value_or(0);
    }

    // [ARRAY-BINDING] Lua: `items[i]` auf _items -- ein REFramework-Binding.
    // Als managed Call gaebe es stumm nichts. Fallback der Lua ist
    // get_Item(i); List.get_Item liefert hier laut Kommentar NULL, deshalb
    // steht das Array VORNE.
    auto* items = re4vr::get_field_object(datas, "_items");

    for (int32_t i = 0; i < n; ++i) {
        ::REManagedObject* sd = nullptr;

        if (items != nullptr) {
            sd = re4vr::array_element(items, i);
        }

        if (sd == nullptr) {
            sd = re4vr::call_safe<::REManagedObject*>(datas, "get_Item", i);
        }

        if (sd == nullptr) {
            continue;
        }

        const auto iid = enum_as_int(sd, "get_ItemId");

        if (!iid.has_value()) {
            continue;
        }

        if (const char* name = scope_item_name(*iid)) {
            re4vr::lua_set_string("__re4_scope_id", name);

            return;
        }
    }

    // Waffe ohne (bekanntes) Scope montiert
    re4vr::lua_set_nil("__re4_scope_id");
}

// [BOLT_CYCLE] Laeuft auf der GUN-MotionFsm2 (Layer 0) gerade die native
// Bolt-Cycle-/Nachlade-Anim "Aim_Fire.PumpAction"? Robusteres Gate als der
// CameraState (CAM=PumpAction flackert nur 1 Frame).
bool RE4VRWeapons::gun_bolt_cycle_active(int32_t ewid) {
    if (!is_bolt_rifle(ewid)) {
        return false;
    }

    auto* ctx = get_player_ctx_w();
    auto* hu = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                              : nullptr;
    auto* gun = hu != nullptr ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
                              : nullptr;
    auto* go = gun != nullptr ? re4vr::call_safe<::REManagedObject*>(gun, "get_GameObject")
                              : nullptr;

    if (!re4vr::obj_ok(go)) {
        return false;
    }

    auto* m = re4vr::get_component(go, "via.motion.MotionFsm2");

    if (m == nullptr) {
        return false;
    }

    auto* n = re4vr::call_safe<::REManagedObject*>(m, "getCurrentNodeName", 0);

    if (n == nullptr) {
        return false;
    }

    std::string s{};

    try {
        s = utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
    } catch (...) {
        return false;
    }

    return s.find("PumpAction") != std::string::npos;
}

void RE4VRWeapons::scope_killswitch_tick() {
    bool on = check_via_scope();

    // [SCOPE_WID] equippte Scope-Waffen-ID IMMER publishen (nicht nur im Scope).
    const auto ewid_o = get_equip_weapon_id();
    const int32_t ewid = ewid_o.value_or(-1);
    const bool scope_wid_ok = ewid_o.has_value() && is_scope_weapon(ewid);

    if (scope_wid_ok) {
        re4vr::lua_set_number("__re4_scope_wid", static_cast<double>(ewid));
    } else {
        re4vr::lua_set_nil("__re4_scope_wid");
    }

    // [SCOPE_HOLD 2026-08-08] Das SPIEL setzt `_IsViaScope` zurueck, sobald man
    // sich bewegt, und baut es erst Sekunden spaeter wieder auf. Solange die
    // Aim-Taste gehalten wird UND eine Scope-Waffe in der Hand ist, halten wir
    // den Zustand selbst.
    // [HOLD_NUR_NACH_ECHTEM_SCOPE 2026-08-09] Der Hold darf einen Aussetzer
    // UEBERBRUECKEN, aber den Zustand niemals ERFINDEN.
    if (re4vr::lua_get_tribool("__vr_aim_input") != 1) {
        m_scope_via_seen = false;
    } else if (on) {
        m_scope_via_seen = true;
    }

    if (!on && m_scope_via_seen && re4vr::lua_get_tribool("__re4_fork_ok") == 1
        && re4vr::lua_get_tribool("__re4_scope_hold_enable") != 0
        && re4vr::lua_get_tribool("__vr_aim_input") == 1 && scope_wid_ok) {
        on = true;
    }

    // [BOLT-SCHUSS 2026-08-09] Erkennung steht HIER OBEN: der Aim-Cut im
    // Binding muss im SELBEN Frame greifen, und das Bolt-Fenster weiter unten
    // nutzt denselben Stempel statt die Body-FSM ein zweites Mal abzufragen.
    if (is_bolt_rifle(ewid)) {
        auto* ctx = get_player_ctx_w();
        auto* body = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;
        auto* fsm = body != nullptr ? re4vr::get_component(body, "via.motion.MotionFsm2")
                                    : nullptr;
        auto* n5 = fsm != nullptr
            ? re4vr::call_safe<::REManagedObject*>(fsm, "getCurrentNodeName", 5)
            : nullptr;

        std::string s5{};

        if (n5 != nullptr) {
            try {
                s5 = utility::re_string::get_string(reinterpret_cast<::SystemString*>(n5));
            } catch (...) {
                s5.clear();
            }
        }

        if (s5.find("Shoot") != std::string::npos) {
            const double now = clock_now();
            re4vr::lua_set_number("__re4_bolt_shoot_t", now);

            // [BOLT_REAIM] Startschuss fuer den kurzen Aim-Cut in binding.
            // Wird NUR hier gesetzt und dort nach Ablauf sofort geloescht --
            // sonst existiert die Globale gar nicht.
            if (re4vr::lua_get_tribool("__re4_bolt_reaim") != 0
                && re4vr::lua_get_tribool("__vr_aim_input") == 1
                && !re4vr::lua_get_number_opt("__re4_bolt_aim_cut_t").has_value()) {
                re4vr::lua_set_number("__re4_bolt_aim_cut_t", now);
            }
        }
    }

    // [SCOPE_ID] frisch ermitteln (entscheidet Iron-Sight vs Scope).
    if (on) {
        detect_scope_id();
    } else {
        m_scope_id_throttle = (m_scope_id_throttle + 1) % 20;

        if (m_scope_id_throttle == 0) {
            detect_scope_id();
        }
    }

    // [IRON_SIGHT_SPLIT] Nur die Rifles OHNE montiertes Scope = Iron-Sight ->
    // KEIN Killswitch-Scope. Alles andere = scope_aim wie bisher.
    const bool iron_sight =
        on && is_iron_rifle(ewid) && re4vr::lua_get_string("__re4_scope_id").empty();
    const bool scope_aim = on && !iron_sight;

    // LockScene-Pass forct dann Body/Mesh/Parent -- NUR fuer Iron-Sight.
    if (iron_sight) {
        m_iron_active_wid = ewid;
    } else {
        m_iron_active_wid.reset();
    }

    // [BOLTCYCLE_PIN 2026-07-18] Waehrend der nativen PumpAction NICHT
    // killswitchen. Fallback falls die Anim gegen den Pin kaempft:
    // __re4_force_killswitch_bolt wieder auf (bolt_cycle and iron_sight).
    re4vr::lua_set_bool("__re4_force_killswitch_bolt", false);

    // [SCOPE_KILLSWITCH] Rechten Stick-Y (Pitch) nur im echten Scope freigeben.
    re4vr::lua_set_bool("__vr_unlock_ry", scope_aim);

    if (!scope_aim) {
        re4vr::lua_set_bool("vr_scope_active", false);
        re4vr::lua_set_nil("vr_scope_aim_pos");
        re4vr::lua_set_nil("vr_scope_aim_dir");

        return;
    }

    // [SCOPE_AIM_PITCH RAUS 2026-09-02] Der Messwert fy wird in Lua noch
    // berechnet, aber nicht mehr veroeffentlicht -- der einzige Leser sass in
    // movement.lua und ist ausgebaut. Hier entfaellt die Rechnung ersatzlos;
    // sie hatte keinen Nebeneffekt.

    // [SCOPE_BULLET] Kugel auf die SCOPE-KAMERA-Achse (= Crosshair-Mitte)
    // umlenken statt Muendung. Crosshairs Bullet-Hook nutzt vr_scope_aim_pos/dir
    // wenn vr_scope_active -> NUR im Scope aktiv.
    auto* ctx = get_player_ctx_w();
    auto* hu = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                              : nullptr;
    auto* gun = hu != nullptr ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
                              : nullptr;
    auto* sctrl = gun != nullptr
        ? re4vr::call_safe<::REManagedObject*>(gun, "get_ScopeController")
        : nullptr;
    auto* cam = sctrl != nullptr ? re4vr::get_field_object(sctrl, "_CameraRef") : nullptr;
    auto* cgo = cam != nullptr ? re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject")
                               : nullptr;
    auto* ctf = cgo != nullptr ? re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform")
                               : nullptr;

    glm::vec3 cpos{};
    glm::quat crot{};

    if (ctf == nullptr || !get_vec3(ctf, "get_Position", cpos)
        || !get_quat(ctf, "get_Rotation", crot)) {
        return;
    }

    const glm::vec3 cf = crot * glm::vec3{0.0f, 0.0f, SCOPE_BULLET_ZSIGN};

    re4vr::lua_set_bool("vr_scope_active", true);
    re4vr::lua_set_vec3("vr_scope_aim_pos", cpos);
    re4vr::lua_set_vec3("vr_scope_aim_dir", cf);
}

// ============================================================================
// Baustein 3 -- IRON_SIGHT (Lua Z.219-304)
// ============================================================================

::REManagedObject* RE4VRWeapons::iron_find_weapon_go(int32_t wid) {
    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    char base[16];
    std::snprintf(base, sizeof(base), "wp%04d", wid);

    for (const std::string name :
         {std::string{base}, std::string{base} + "_AO", std::string{base} + "_MC"}) {
        auto* str = sdk::VM::create_managed_string(utility::widen(name));

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

    return nullptr;
}

// Punkt 1: Body/Arme/Haar/Kopf wieder sichtbar (Spiel blendet sie beim Aim aus)
void RE4VRWeapons::iron_walk_body_visible(::REManagedObject* tf, int32_t depth) {
    if (tf == nullptr || depth > 8) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

    if (go != nullptr && is_iron_body_go(obj_name_of(go))) {
        re4vr::call_safe<void*>(go, "set_UpdateSelf", true);
        re4vr::call_safe<void*>(go, "set_DrawSelf", true);

        // Lua: go:write_byte(0x13, 1)
        try {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(go) + 0x13) = 1;
        } catch (...) {
        }
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int32_t guard = 0;

    while (child != nullptr && guard < 256) {
        ++guard;
        iron_walk_body_visible(child, depth + 1);
        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }
}

void RE4VRWeapons::iron_force_body_visible() {
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (re4vr::obj_ok(tf)) {
        iron_walk_body_visible(tf, 0);
    }
}

// Punkt 2: Waffen-Mesh-Part 0 wieder an (Spiel versteckt es beim Aim)
void RE4VRWeapons::iron_force_weapon_mesh(int32_t wid) {
    auto* go = iron_find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    auto* mesh = re4vr::get_component(go, "via.render.Mesh");

    if (mesh == nullptr) {
        return;
    }

    const auto m = find_method(mesh, "setPartsEnable");

    if (m == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();

    try {
        m->call_safe<void*>(context, mesh, 0, true);
    } catch (...) {
    }

    clear_pending(context, true);
}

// Punkt 3: Waffe an den Body zurueckhaengen, wenn der Parent verloren ging.
// [KEIN BODY-CACHE 2026-08-09] Body-Transform jedes Mal FRISCH holen -- ein
// gemerkter ist nach Save-Load eine Leiche, und set_Parent darauf ist eine AV.
void RE4VRWeapons::iron_ensure_parented(int32_t wid) {
    auto* go = iron_find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    auto* wp_tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (!re4vr::obj_ok(wp_tf)) {
        return;
    }

    auto* parent = re4vr::call_safe<::REManagedObject*>(wp_tf, "get_Parent");

    if (parent != nullptr) {
        return;   // Parent sitzt noch -> nichts zu tun
    }

    auto* btf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (re4vr::obj_ok(btf)) {
        re4vr::call_safe<void*>(wp_tf, "set_Parent", btf);
    }
}

void RE4VRWeapons::iron_sight_tick(int32_t wid) {
    iron_force_body_visible();
    iron_force_weapon_mesh(wid);
    iron_ensure_parented(wid);
}

// ============================================================================
// Baustein 4 -- SCOPE_PROTO (Lua Z.541-602)
// ============================================================================

void RE4VRWeapons::save_scope_proto() {
    nlohmann::json d{};
    d["scale"] = m_scope_proto.scale;
    d["ox"] = m_scope_proto.ox;
    d["oy"] = m_scope_proto.oy;
    d["oz"] = m_scope_proto.oz;
    re4vr::json_save(SCOPE_PROTO_CFG_PATH, d);
}

::REManagedObject* RE4VRWeapons::scope_get_gun_go() {
    auto* ctx = get_player_ctx_w();
    auto* hu = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                              : nullptr;
    auto* gun = hu != nullptr ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
                              : nullptr;

    return gun != nullptr ? re4vr::call_safe<::REManagedObject*>(gun, "get_GameObject")
                          : nullptr;
}

void RE4VRWeapons::scope_proto_tick() {
    // Laeuft nur, wenn tatsaechlich durch ein Scope gezielt wird UND ein Regler
    // von der Grundstellung abweicht -- sonst sofort raus, kostet dann nichts.
    if (re4vr::lua_get_tribool("vr_scope_active") != 1) {
        return;
    }

    if (m_scope_proto.scale == 1.0f && m_scope_proto.ox == 0.0f && m_scope_proto.oy == 0.0f
        && m_scope_proto.oz == 0.0f) {
        return;
    }

    auto* go = scope_get_gun_go();

    if (go == nullptr) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (!re4vr::obj_ok(tf)) {
        return;
    }

    if (m_scope_proto.scale != 1.0f) {
        set_vec3(tf, "set_LocalScale",
                 glm::vec3{m_scope_proto.scale, m_scope_proto.scale, m_scope_proto.scale});
    }

    if (m_scope_proto.ox != 0.0f || m_scope_proto.oy != 0.0f || m_scope_proto.oz != 0.0f) {
        glm::vec3 lp{};

        if (get_vec3(tf, "get_LocalPosition", lp)) {
            set_vec3(tf, "set_LocalPosition",
                     glm::vec3{lp.x + m_scope_proto.ox, lp.y + m_scope_proto.oy,
                               lp.z + m_scope_proto.oz});
        }
    }
}

// ============================================================================
// Baustein 5 -- hide_body_weapons (Lua Z.604-666)
// ============================================================================

void RE4VRWeapons::save_hide_body_cfg() {
    nlohmann::json d{};
    d["enabled"] = m_hide_body_enabled;
    re4vr::json_save(HIDE_BODY_CFG_PATH, d);
}

void RE4VRWeapons::hide_body_weapons_tick() {
    if (!m_hide_body_enabled) {
        return;
    }

    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return;
    }

    const auto cur_wid = get_equip_weapon_id();

    // [KEINE ID -> NICHTS VERSTECKEN 2026-09-08] Ohne lesbare Waffen-ID liefert
    // is_current() unten fuer JEDE Waffe false -- der Walk versteckte dann auch
    // die gerade gehaltene. Genau das passierte im Fenster nach einer Cutscene,
    // in dem die Engine die Waffe neu setzt: man konnte schiessen, sah die Waffe
    // aber nicht mehr, bis man von Hand wechselte (set_DrawSelf(false) bleibt
    // stehen, bis es jemand zuruecksetzt). Ohne ID also gar nicht erst
    // entscheiden.
    if (!cur_wid.has_value()) {
        return;
    }

    // [CUTSCENE 2026-09-08 -> JEDER KILLSWITCH 2026-09-09] Waehrend einer
    // Event-Kamera raeumt die Engine die Waffen selbst um (equippen, wegstecken,
    // Anim-Requisiten). Wer da mitverstecken will, arbeitet gegen einen Zustand,
    // der sich jeden Frame aendert.
    //
    // Der Filter stand auf dem GRUND "IsEventCamera" -- damit lief dieser Tick
    // in jedem anderen Killswitch weiter. Im Killswitch gehoert die Sichtbarkeit
    // aber dem Spiel: wir verstecken dort nichts und machen dort nichts
    // sichtbar. Insbesondere greift die [SELBSTHEILUNG] unten nicht mehr, die
    // eine vom Spiel versteckte Waffe im selben Frame wieder eingeblendet hat.
    if (auto& ks = RE4VRKillswitch::get(); ks != nullptr && ks->is_active()) {
        return;
    }

    // [NUR ECHTES GAMEPLAY 2026-09-09] Der Killswitch-Ausstieg allein hat ein
    // Loch: zwischen "gameplay ist aus" und "Killswitch steht" liegen ein paar
    // Frames (gemessen 0,06 s: 77.38 -> 77.44). Versteckt die Engine die Waffe
    // genau dort, blendet die [SELBSTHEILUNG] unten sie im selben Frame wieder
    // ein und sie bleibt die ganze Szene sichtbar. Beim zweiten Laden desselben
    // Saves lag das Verstecken einen Frame spaeter -- und die Waffe blieb weg.
    // Genau diese Abhaengigkeit von der Ladezeit faellt hier weg.
    //
    // Folge, bewusst in Kauf genommen: eine ausserhalb des Gameplays faelschlich
    // versteckte Waffe wird erst geheilt, wenn das Gameplay zurueck ist -- also
    // Sekundenbruchteile spaeter, nicht dauerhaft.
    if (!re4vr::lua_get_bool("__re4_frame_is_gameplay", false)) {
        return;
    }

    std::string cur{};

    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "wp%04d", *cur_wid);
        cur = buf;
    }

    // [GO_SUFFIX 2026-07-19] Der GO der equippten Waffe heisst nicht immer exakt
    // "wp####" (Ada: "wp6112_AO"). MAINGAME-SICHERHEIT: die Suffix-Variante
    // zaehlt NUR, wenn es KEIN plain "wp####" am Koerper gibt.
    bool plain_exists = false;

    if (!cur.empty()) {
        auto* c0 = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
        int32_t n0 = 0;

        while (c0 != nullptr && n0 < 64) {
            ++n0;

            auto* g0 = re4vr::call_safe<::REManagedObject*>(c0, "get_GameObject");

            if (obj_name_of(g0) == cur) {
                plain_exists = true;

                break;
            }

            c0 = re4vr::call_safe<::REManagedObject*>(c0, "get_Next");
        }
    }

    const auto is_current = [&](const std::string& nm) {
        if (cur.empty()) {
            return false;
        }

        if (nm == cur) {
            return true;
        }

        if (plain_exists) {
            return false;   // altes Verhalten, Maingame unveraendert
        }

        return nm == cur + "_AO" || nm == cur + "_MC";
    };

    // [DOPPELGAENGER 15.09.2026 -- in Lua gemessen, zzz_re4_granate_probe]
    // Das Spiel haelt pro Granatentyp im Inventar ZWEI gleichnamige Objekte am
    // Koerper (gemessen: sechs Stueck bei drei Granatensorten). Beim Ziehen
    // macht es BEIDE des gezogenen Typs sichtbar -- eines sitzt in der Hand,
    // das andere zieht beim Laufen hinterher.
    //
    // Der Unterschied steht in `UpdateSelf`, nicht in der Bewegung: ueber die
    // ganze Messung hatte immer nur EIN Objekt UpdateSelf=true (dieselbe
    // Adresse, in allen 35442 Messpaaren genau eines von beiden), alle
    // unsichtbaren hatten durchweg false. Das trailende wird schlicht nicht
    // aktualisiert -- daher das Nachziehen.
    //
    // Der erste Versuch hat stattdessen den zurueckgelegten WEG verglichen.
    // Das musste scheitern: die Handabstaende der beiden unterscheiden sich im
    // Median um 0,000 m. Er hat deshalb zeitweise das ECHTE Objekt versteckt --
    // Granate unsichtbar, ziehen ging nur noch im Pausemenue.
    ::REManagedObject* update_owner = nullptr;
    int32_t current_count = 0;

    {
        auto* c = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
        int32_t n1 = 0;

        while (c != nullptr && n1 < 64) {
            ++n1;

            auto* g = re4vr::call_safe<::REManagedObject*>(c, "get_GameObject");

            if (const auto nm = obj_name_of(g); !nm.empty() && is_current(nm)) {
                ++current_count;

                bool upd = false;

                if (re4vr::try_call<bool>(g, "get_UpdateSelf", upd) && upd) {
                    update_owner = g;
                }
            }

            c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
        }
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int32_t n = 0;

    while (child != nullptr && n < 64) {
        ++n;

        auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");
        const std::string name = obj_name_of(go);

        // ac0000_00 (Taschenlampe) NICHT mehr verstecken: motion treibt sie an
        // die linke Hand + steuert ihre Sichtbarkeit selbst.
        if (!name.empty() && name.rfind("wp", 0) == 0) {
            bool vis = false;
            const bool have_vis = re4vr::try_call<bool>(go, "get_DrawSelf", vis);

            if (!is_current(name)) {
                if (have_vis && vis) {
                    re4vr::call_safe<void*>(go, "set_DrawSelf", false);
                }
            } else if (current_count > 1 && update_owner == nullptr) {
                // Mehrere gleichnamige, aber KEINES ist das gefuehrte (z.B.
                // mitten im Wechsel): nichts anfassen. Ein sichtbarer Trail ist
                // allemal besser als eine unsichtbare Granate in der Hand.
            } else if (current_count > 1 && go != update_owner) {
                // Gleicher Name, aber nicht das gefuehrte Objekt -> weg damit.
                // Entschieden wird das JEDEN Frame neu; es wird sich nichts
                // gemerkt, damit eine Fehlentscheidung nicht festfrieren kann.
                if (have_vis && vis) {
                    re4vr::call_safe<void*>(go, "set_DrawSelf", false);
                }
            } else if (have_vis && !vis) {
                // [SELBSTHEILUNG 2026-09-08] Die GEHALTENE Waffe gehoert
                // sichtbar. set_DrawSelf(false) bleibt stehen, bis es jemand
                // zuruecknimmt -- wurde sie in einem Fenster ohne lesbare ID
                // einmal versteckt, blieb sie es bis zum Waffenwechsel.
                re4vr::call_safe<void*>(go, "set_DrawSelf", true);
            }
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }
}

// ============================================================================
// Baustein 6 -- Messer-Holster / KKO / Quick-Knife / STOW-GUARD
// (Lua Z.668-1054)
// ============================================================================

::REManagedObject* RE4VRWeapons::get_head_updater() {
    auto* head = re4vr::fc::on() ? re4vr::fc::head_go() : re4vr::head_game_object();

    if (head == nullptr) {
        return nullptr;
    }

    if (auto* u = re4vr::get_component(head, HEAD_UPDATER_LEON)) {
        return u;
    }

    return re4vr::get_component(head, HEAD_UPDATER_ADA);
}

::REManagedObject* RE4VRWeapons::get_knife_timer() {
    auto* updater = get_head_updater();

    if (updater == nullptr) {
        return nullptr;
    }

    return re4vr::get_field_object(updater, "<KnifeCloseTimer>k__BackingField");
}

bool RE4VRWeapons::holster_knife() {
    auto* timer = get_knife_timer();

    if (timer == nullptr) {
        return false;
    }

    if (!m_kh.original_time_limit.has_value()) {
        float orig = 0.0f;
        const bool ok = re4vr::try_call<float>(timer, "get__TimeLimit", orig);

        // Lua liest das FELD direkt (timer._TimeLimit).
        if (!ok) {
            // [FELDTYP 05.09.2026] `_TimeLimit` ist System.Single -- belegt durch
            // set_timer_field in dieser Datei, das es ausdruecklich als Single
            // schreibt. Hier stand get_field_int: get_data<int32_t> ist ein roher
            // Reinterpret-Cast, aus 5.0f (0x40A00000) wurde 1084227584. Genau
            // dieser Wert ging nach 30 Frames wieder in den Timer zurueck.
            if (const auto v = re4vr::get_field_float(timer, "_TimeLimit"); v.has_value()) {
                orig = *v;
            } else {
                orig = 5.0f;
            }
        }

        m_kh.original_time_limit = orig > 0.0f ? orig : 5.0f;
    }

    m_kh.force_frames = 30;

    return true;
}

namespace {

// Luas `timer._TimeLimit = x` / `_TransitTime` / `_Completed` -- Feldzugriff.
bool set_timer_field(::REManagedObject* obj, const char* name, double value) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* f = td != nullptr ? td->get_field(name) : nullptr;

    if (f == nullptr) {
        return false;
    }

    auto* p = f->get_data_raw(obj, false);

    if (p == nullptr) {
        return false;
    }

    auto* ft = f->get_type();
    const std::string tn = ft != nullptr ? ft->get_full_name() : std::string{};

    if (tn == "System.Single") {
        *reinterpret_cast<float*>(p) = static_cast<float>(value);
    } else if (tn == "System.Boolean") {
        *reinterpret_cast<bool*>(p) = value != 0.0;
    } else {
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(value);
    }

    return true;
}

}   // namespace

void RE4VRWeapons::update_knife_holster_timer() {
    if (m_kh.force_frames <= 0) {
        return;
    }

    auto* timer = get_knife_timer();

    if (timer != nullptr) {
        set_timer_field(timer, "_TimeLimit", 0.0);
    }

    --m_kh.force_frames;

    if (m_kh.force_frames == 0 && m_kh.original_time_limit.has_value()) {
        if (timer != nullptr) {
            set_timer_field(timer, "_TimeLimit",
                            static_cast<double>(*m_kh.original_time_limit));
        }

        m_kh.original_time_limit.reset();
    }
}

// [KNIFE_KEEP_OUT] Auto-Holster verhindern.
//
// [KS-LUECKE 2026-07-17 -- teuer bezahlt] Hier stand ein Ausstieg
// "kein Messer equippt -> return". Genau der war der Bug: im Killswitch/Stagger
// nimmt die ENGINE das Messer kurz weg -> eid = -1 -> KKO stieg aus -> der
// KnifeCloseTimer lief WAEHREND des KS ungebremst auf sein TimeLimit.
// Der Timer wird jetzt IMMER zurueckgehalten. Ohne Messer in der Hand ist das
// ein No-op. **NICHT wieder auf "nur wenn eid ein Messer ist" gaten.**
void RE4VRWeapons::keep_knife_out() {
    if (!KNIFE_KEEP_OUT) {
        return;
    }

    if (m_kh.force_frames > 0) {
        return;   // gewollter Holster laeuft -> nicht stoeren
    }

    auto* timer = get_knife_timer();

    if (timer == nullptr) {
        return;
    }

    // [KKO] Der frueher hier stehende timer:call("reset") ist raus: er braucht
    // Argumente (Log-Flut) und ist redundant.
    set_timer_field(timer, "_TransitTime", 0.0);
    set_timer_field(timer, "_Completed", 0.0);
}

// ----------------------------------------------------------------------------
// Die fuenf Equip-Hooks
// ----------------------------------------------------------------------------

namespace {

bool s_stow_skipped = false;
bool s_cwa_skipped = false;

// "Eigener Wechsel?" -- holster setzt __re4_our_equip_until bei JEDEM eigenen Zug.
bool our_equip_running() {
    const auto u = re4vr::lua_get_number_opt("__re4_our_equip_until");

    return u.has_value() && clock_now() < *u;
}

// Killswitch-Grund enthaelt "grappl"? Dort wehrt man sich mit dem Messer.
bool ks_reason_is_grapple() {
    auto& ks = RE4VRKillswitch::get();

    if (ks == nullptr) {
        return false;
    }

    std::string r = ks->get_activating_reason();

    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return r.find("grappl") != std::string::npos;
}

// Nahkampf-/Finisher-Prompt offen -> RT ist dort die gewollte Aktion.
bool finisher_prompt_open() {
    return re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false);
}

}   // namespace

// [KNIFE_KEEP_OUT / ENGINE-WEGNAHME 2026-07-17] DER Weg, auf dem das Spiel das
// Messer versteckt: requestChangeWeaponAction. Der Call taucht ausschliesslich
// beim ungewollten Wechsel auf -> er laesst sich gefahrlos genau dort blocken.
bool RE4VRWeapons::cb_change_weapon_action() {
    if (!KNIFE_KEEP_OUT) {
        return false;
    }

    if (m_kh.force_frames > 0) {
        return false;   // gewollter Holster laeuft
    }

    // Menue/Inventar: NIE blocken -- dort nutzt die Engine denselben Call fuer
    // den normalen Waffenwechsel.
    if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") != 1) {
        return false;
    }

    // [EIGENER WECHSEL] Unser draw_last_pistol/rifle/grenade ruft
    // requestChangeActiveWeapon, und die Engine ruft daraufhin INTERN dieses
    // requestChangeWeaponAction. Ohne diesen Guard liesse sich KEINE Waffe mehr
    // aus dem Holster ziehen, solange ein Messer in der Hand war.
    if (our_equip_running()) {
        return false;
    }

    const auto eid = get_equip_weapon_id();

    return eid.has_value() && is_knife_id(*eid);
}

// [QUICK-KNIFE 2026-08-18] requestEquipKnife verwerfen, wenn er die Signatur
// des Quick-Knife hat.
bool RE4VRWeapons::cb_request_equip_knife() {
    if (re4vr::lua_get_tribool("__re4_qk_block") != 1) {
        return false;
    }

    // [KS-GATE 2026-08-18 -- aus dem Watcher-Log] Der Quick-Knife entsteht
    // AUSSCHLIESSLICH im Killswitch; ein `gameplay ~= true -> return` liess den
    // Block seit dem 17.07. wirkungslos. Der Killswitch allein ist deshalb kein
    // Ausschlussgrund mehr.
    const bool gameplay = re4vr::lua_get_tribool("__re4_frame_is_gameplay") == 1;
    const std::string why = re4vr::lua_get_string("__re4_gameplay_why");

    if (!gameplay && why != "killswitch") {
        return false;
    }

    if (!gameplay && ks_reason_is_grapple()) {
        return false;
    }

    if (our_equip_running()) {
        return false;
    }

    // Die Quick-Knife-Signatur: Trigger gezogen, aber nicht gezielt.
    if (re4vr::lua_get_tribool("__vr_raw_r_trigger") != 1) {
        return false;
    }

    if (re4vr::lua_get_tribool("__vr_aim_input") == 1) {
        return false;
    }

    if (finisher_prompt_open()) {
        return false;
    }

    re4vr::lua_set_number("__re4_qk_blocked",
                          re4vr::lua_get_number("__re4_qk_blocked", 0.0) + 1.0);

    return true;
}

// [STOW-GUARD 2026-08-18] Wegstecken ist eine ENTSCHEIDUNG DES SPIELERS.
// Verwirft die beiden NATIVEN Nachzieh-Aufrufe -- und NUR sie.
bool RE4VRWeapons::cb_stow_guard() {
    if (re4vr::lua_get_tribool("__re4_stow_block") != 1) {
        return false;
    }

    // unsere eigene Kette (laeuft aus)
    if (const auto o = re4vr::lua_get_number_opt("__re4_stow_ours_until");
        o.has_value() && clock_now() < *o) {
        return false;
    }

    const auto u = re4vr::lua_get_number_opt("__re4_stow_guard_until");

    if (!u.has_value() || clock_now() >= *u) {
        return false;
    }

    re4vr::lua_set_number("__re4_stow_blocked",
                          re4vr::lua_get_number("__re4_stow_blocked", 0.0) + 1.0);

    return true;
}

// [QUICK-KNIFE 2. WEG 2026-08-18] Die Engine equippt das Messer auch OHNE
// requestEquipKnife -- hier an equipWeapon abgefangen.
bool RE4VRWeapons::cb_equip_weapon(uintptr_t raw_wid) {
    // Lua: `wid = sdk.to_int64(args[4]) & 0xFFFFFFFF` -- die Maske ist noetig,
    // sonst kommt bei einem Enum-Argument der obere Muell mit.
    const int32_t wid = static_cast<int32_t>(raw_wid & 0xFFFFFFFFull);

    // ------------------------------------------------------------------
    // [ENGER RIEGEL 13.09.2026 -- Ansage "das Spiel equippt uns nichts in die
    // Hand", Variante ENG]
    //
    // Gemessen am 13.09. um 02:04:07: das Spiel steckte erst weg
    // (requestEquipBareHand [SPIEL]) und zog dann von sich aus eine Waffe
    // (execChangeWeapon -> equipWeapon -> onEquipChange, alle [SPIEL]).
    // Die vorhandene Sperre darunter greift nur fuer MESSER-IDs bei gedruecktem
    // RT (Quick-Knife) -- eine Pistole lief glatt durch.
    //
    // Geblockt wird deshalb jeder Wechsel, der NICHT von uns kommt, aber NUR:
    //   * in REINEM Gameplay (`__re4_frame_pure_gameplay`) -- also ohne
    //     Cutscene, Killswitch, Menue, Turret und Jetski, wo die Engine die
    //     Waffen legitim selbst umbaut,
    //   * erst EINE SEKUNDE, nachdem dieses reine Gameplay begonnen hat --
    //     sonst faengt der Riegel genau den Wechsel ab, den der Spieler eben
    //     im Inventar gewaehlt hat und den das Spiel im ersten Frame danach
    //     ausfuehrt,
    //   * und nicht bei offenem Finisher-Prompt, wo der Griff zur Waffe die
    //     gewollte Aktion ist.
    // ------------------------------------------------------------------
    if (!our_equip_running() && !finisher_prompt_open()
        && re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1
        && m_pure_since > 0.0 && (clock_now() - m_pure_since) > 1.0) {
        re4vr::lua_set_number("__re4_game_equip_blocked",
                              re4vr::lua_get_number("__re4_game_equip_blocked", 0.0) + 1.0);

        return true;
    }

    if (re4vr::lua_get_tribool("__re4_qk_block") != 1) {
        return false;
    }

    if (!is_knife_id(wid)) {
        return false;
    }

    if (our_equip_running()) {
        return false;
    }

    if (re4vr::lua_get_tribool("__vr_raw_r_trigger") != 1) {
        return false;
    }

    if (re4vr::lua_get_tribool("__vr_aim_input") == 1) {
        return false;
    }

    if (finisher_prompt_open()) {
        return false;
    }

    // Im Killswitch nur blocken, wenn es kein Grapple ist.
    if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") != 1) {
        const std::string why = re4vr::lua_get_string("__re4_gameplay_why");

        if (why != "killswitch") {
            return false;
        }

        if (ks_reason_is_grapple()) {
            return false;
        }
    }

    re4vr::lua_set_number("__re4_qk_equip_blocked",
                          re4vr::lua_get_number("__re4_qk_equip_blocked", 0.0) + 1.0);

    return true;
}

void RE4VRWeapons::install_equip_hooks() {
    auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment");

    if (td == nullptr) {
        return;
    }

    // ---- requestChangeWeaponAction: Engine-Wegnahme blocken --------------
    // ACHTUNG: dieselbe Methode wird UNTEN vom STOW-GUARD ein zweites Mal
    // gehookt. In Lua sind das zwei getrennte sdk.hook-Registrierungen; der
    // HookManager kombiniert Pre-Callbacks mit "jedes SKIP gewinnt", also
    // bleibt das Verhalten identisch.
    if (auto* m = td->get_method("requestChangeWeaponAction")) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                bool skip = false;

                try {
                    auto& self = RE4VRWeapons::get();

                    if (self != nullptr) {
                        skip = self->cb_change_weapon_action();
                    }
                } catch (...) {
                    skip = false;
                }

                if (!skip) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                s_cwa_skipped = true;

                return HookManager::PreHookResult::SKIP_ORIGINAL;
            },
            [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
                // [REGISTERMUELL] requestChangeWeaponAction gibt Boolean
                // zurueck -- ein geskippter Call ohne gesetzten Rueckgabewert
                // laesst stehen, was zufaellig in RAX stand.
                if (s_cwa_skipped) {
                    s_cwa_skipped = false;
                    ret = 0;   // false
                }
            });
    }

    // ---- requestEquipKnife: Quick-Knife ----------------------------------
    if (auto* m = td->get_method("requestEquipKnife")) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                bool skip = false;

                try {
                    auto& self = RE4VRWeapons::get();

                    if (self != nullptr) {
                        skip = self->cb_request_equip_knife();
                    }
                } catch (...) {
                    skip = false;
                }

                return skip ? HookManager::PreHookResult::SKIP_ORIGINAL
                            : HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }

    // ---- STOW-GUARD: zwei Methoden ---------------------------------------
    // requestChangeActiveWeapon ist VOID, requestChangeWeaponAction ist BOOL.
    for (const auto& [name, is_bool] :
         std::array<std::pair<const char*, bool>, 2>{
             {{"requestChangeActiveWeapon", false}, {"requestChangeWeaponAction", true}}}) {
        auto* m = td->get_method(name);

        if (m == nullptr) {
            continue;
        }

        const bool bool_ret = is_bool;

        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                bool skip = false;

                try {
                    auto& self = RE4VRWeapons::get();

                    if (self != nullptr) {
                        skip = self->cb_stow_guard();
                    }
                } catch (...) {
                    skip = false;
                }

                if (!skip) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                s_stow_skipped = true;

                return HookManager::PreHookResult::SKIP_ORIGINAL;
            },
            [bool_ret](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
                if (s_stow_skipped) {
                    s_stow_skipped = false;

                    if (bool_ret) {
                        ret = 0;   // false statt Registermuell
                    }
                }
            });
    }

    // ---- equipWeapon: Quick-Knife 2. Weg ---------------------------------
    if (auto* m = td->get_method("equipWeapon")) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                bool skip = false;

                try {
                    auto& self = RE4VRWeapons::get();

                    // Lua: args[4] = WeaponID (1-basiert) -> hier args[3].
                    if (self != nullptr && args.size() >= 4) {
                        skip = self->cb_equip_weapon(args[3]);
                    }
                } catch (...) {
                    skip = false;
                }

                return skip ? HookManager::PreHookResult::SKIP_ORIGINAL
                            : HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }
}

// ============================================================================
// Baustein 7 -- Assist Light (Lua Z.1056-1160)
// ============================================================================

bool RE4VRWeapons::get_assist_light_go_tf(::REManagedObject*& go_out,
                                          ::REManagedObject*& tf_out) {
    go_out = nullptr;
    tf_out = nullptr;

    auto* bg = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();

    if (bg == nullptr) {
        return false;
    }

    const auto body_addr = addr_of(bg);

    if (!body_addr.has_value()) {
        return false;
    }

    if (m_al.body_addr == body_addr && m_al.tf.obj != nullptr) {
        auto* go_cached = re4vr::call_safe<::REManagedObject*>(m_al.tf.obj, "get_GameObject");

        if (go_cached != nullptr) {
            store(m_al.go, go_cached);
            go_out = go_cached;
            tf_out = m_al.tf.obj;

            return true;
        }
    }

    m_al.body_addr = body_addr;
    drop(m_al.go);
    drop(m_al.tf);

    auto* tf = re4vr::call_safe<::REManagedObject*>(bg, "get_Transform");

    if (!re4vr::obj_ok(tf)) {
        return false;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

    while (child != nullptr) {
        auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

        if (cgo != nullptr && obj_name_of(cgo) == "assist_Light") {
            auto* alf_tf = re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform");

            store(m_al.go, cgo);
            store(m_al.tf, alf_tf);

            go_out = cgo;
            tf_out = alf_tf;

            return true;
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return false;
}

namespace {

// Lua: set_mesh_enabled -- rekursiv ueber alle Kinder.
void set_mesh_enabled(::REManagedObject* go, bool enabled) {
    if (go == nullptr) {
        return;
    }

    if (auto* m = re4vr::get_component(go, "via.render.Mesh")) {
        re4vr::call_safe<void*>(m, "set_Enabled", enabled);
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    int32_t cnt = 0;
    re4vr::try_call<int32_t>(tf, "get_ChildCount", cnt);

    for (int32_t i = 0; i < cnt; ++i) {
        auto* ctf = re4vr::call_safe<::REManagedObject*>(tf, "get_Child", i);

        if (ctf == nullptr) {
            continue;
        }

        auto* cgo = re4vr::call_safe<::REManagedObject*>(ctf, "get_GameObject");

        if (cgo != nullptr) {
            set_mesh_enabled(cgo, enabled);
        }
    }
}

}   // namespace

void RE4VRWeapons::hide_assist_light() {
    if (!m_hide_assist_light) {
        return;
    }

    ::REManagedObject* cgo = nullptr;
    ::REManagedObject* tf = nullptr;

    if (!get_assist_light_go_tf(cgo, tf) || cgo == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(cgo, "set_DrawSelf", false);
    re4vr::call_safe<void*>(cgo, "set_UpdateSelf", false);
    set_mesh_enabled(cgo, false);
}

// Match re4_vr_motion docked flashlight: world view rot = game yaw baseline
// (right stick) * raw HMD quat. camera_rot_full alone is only headset in VR
// space and lags body / stick yaw.
void RE4VRWeapons::sync_assist_light_rotation_to_vr_cam() {
    if (!m_assist_light_follow_vr_cam) {
        return;
    }

    if (m_hide_assist_light) {
        return;
    }

    {
        auto& ks = RE4VRKillswitch::get();

        if (ks != nullptr && ks->is_active()) {
            return;
        }
    }

    if (is_any_menu_open()) {
        return;
    }

    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return;
    }

    if (re4vr::lua_get_table_tribool("vr_camera_fix", "active") != 1) {
        return;
    }

    const auto cam_rot_full = re4vr::lua_get_table_quat("vr_camera_fix", "camera_rot_full");

    if (!cam_rot_full.has_value()) {
        return;
    }

    ::REManagedObject* go = nullptr;
    ::REManagedObject* alf_tf = nullptr;

    if (!get_assist_light_go_tf(go, alf_tf) || alf_tf == nullptr) {
        return;
    }

    glm::quat world = *cam_rot_full;

    if (const auto cam_rot = re4vr::lua_get_table_quat("vr_camera_fix", "camera_rot");
        cam_rot.has_value()) {
        const glm::quat hmd_quat{vr->get_rotation(0)};
        world = glm::normalize(*cam_rot * hmd_quat);
    }

    set_quat(alf_tf, "set_Rotation", world);
}

// ============================================================================
// Baustein 8 -- SNAPPY_WEP + WEAPON_SWITCH_SKIP (Lua Z.1162-1310)
// ============================================================================

void RE4VRWeapons::save_snappy_cfg() {
    nlohmann::json d{};
    d["enabled"] = m_snappy.enabled;
    d["frameskip"] = m_snappy.frameskip;
    d["fast_re_aim"] = m_snappy.fast_re_aim;
    d["direct_snap"] = m_snappy.direct_snap;
    re4vr::json_save(SNAPPY_CFG_PATH, d);
}

::REManagedObject* RE4VRWeapons::get_mfsm2() {
    auto* ctx = get_player_ctx_w();

    if (ctx == nullptr) {
        return nullptr;
    }

    auto* bu = re4vr::get_field_object(ctx, "_BodyUpdater");

    if (bu == nullptr) {
        return nullptr;
    }

    return re4vr::get_field_object(bu, "<MotionFsm>k__BackingField");
}

// Setzt BEIDE Snappy-Features auf ihren Soll-Zustand (Layer 4).
// revert_all=true -> beide auf Original zurueck (fuer Script-Reset).
// Gibt true zurueck, sobald der Tree erreichbar ist (= angewandt, FSM bereit).
bool RE4VRWeapons::apply_snappy(bool revert_all) {
    auto* mfsm2 = get_mfsm2();

    if (mfsm2 == nullptr) {
        return false;
    }

    auto* tree = fsm_tree_layer(mfsm2, 4);

    if (tree == nullptr) {
        return false;
    }

    const bool done = true;   // FSM bereit (Original gated nur auf layer4)

    // ---- Schneller Waffenwechsel ----
    if (auto* put_away = tree->get_node_by_name(std::string_view{"wp4000H_0551_put_away"})) {
        auto actions = put_away->get_actions();

        // [INDEX-FALLE] Luas `actions[4]` ist nativ Index 3.
        if (actions.size() > 3 && actions[3] != nullptr) {
            auto* act = reinterpret_cast<::REManagedObject*>(actions[3]);
            const bool on = !revert_all && m_snappy.enabled;
            double sf = 0.0;

            if (on) {
                if (m_snappy.direct_snap) {
                    // Direkt-Snap: StartFrame ans Anim-Ende -> instant.
                    // _EndFrame wenn vorhanden, sonst hoher Wert (Engine
                    // clamped auf Ende).
                    const auto ef = act_get_num(act, "_EndFrame");
                    sf = (ef.has_value() && *ef > 1.0) ? *ef : 9999.0;
                } else {
                    sf = static_cast<double>(m_snappy.frameskip);
                }
            }

            act_set_num(act, "_StartFrame", sf);
            act_set_num(act, "_OverwriteInterpolation", on ? 1.0 : 0.0);
        }
    }

    // ---- Fast Re-Aim: Inhibit-Hold-Action an HOLD_END ----
    if (auto* h_end = tree->get_node_by_name(std::string_view{"HOLD_END"})) {
        auto acts = h_end->get_actions();
        void* inhibit = acts.size() > 3 ? acts[3] : nullptr;

        if (inhibit == nullptr) {
            auto un = h_end->get_unloaded_actions();
            inhibit = un.size() > 3 ? un[3] : nullptr;
        }

        if (inhibit != nullptr) {
            const bool on = !revert_all && m_snappy.fast_re_aim;
            // enabled=AN heisst Feature AUS
            re4vr::call_safe<void*>(reinterpret_cast<::REManagedObject*>(inhibit),
                                    "set_Enabled", !on);
        }
    }

    return done;
}

void RE4VRWeapons::snappy_wep_tick() {
    // [WSW_PIN] motion pinnt Hand+Waffe waehrend des Wechsels, wenn dieses Flag
    // gesetzt ist -> keine sichtbare native Draw/Holster-Anim.
    re4vr::lua_set_bool("__vr_wsw_pin", m_snappy.enabled && m_snappy.direct_snap);

    auto* body = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
    const auto addr = addr_of(body);

    if (addr != m_snappy_last_body) {
        m_snappy_last_body = addr;
        m_snappy_applied = false;   // frischer Body -> Tree neu -> re-apply
    }

    if (!m_snappy_applied) {
        if (apply_snappy(false)) {
            m_snappy_applied = true;
        }
    }
}

bool RE4VRWeapons::wsw_get_comps(::REManagedObject*& fsm_out,
                                 ::REManagedObject*& motion_out) {
    fsm_out = nullptr;
    motion_out = nullptr;

    auto* body = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();

    if (body == nullptr) {
        drop(m_wsw.body);
        drop(m_wsw.fsm);
        drop(m_wsw.motion);

        return false;
    }

    if (m_wsw.body.obj != body) {
        store(m_wsw.body, body);

        drop(m_wsw.fsm);
        drop(m_wsw.motion);

        if (auto* f = re4vr::get_component(body, "via.motion.MotionFsm2")) {
            store(m_wsw.fsm, f);
        }

        if (auto* m = re4vr::get_component(body, "via.motion.Motion")) {
            store(m_wsw.motion, m);
        }
    }

    fsm_out = m_wsw.fsm.obj;
    motion_out = m_wsw.motion.obj;

    return fsm_out != nullptr && motion_out != nullptr;
}

// [WEAPON_SWITCH_SKIP] Waffenwechsel-Anim komplett skippen (ALLE Waffen).
void RE4VRWeapons::weapon_switch_skip_tick() {
    if (!(m_snappy.enabled && m_snappy.direct_snap)) {
        return;
    }

    ::REManagedObject* fsm = nullptr;
    ::REManagedObject* motion = nullptr;

    if (!wsw_get_comps(fsm, motion)) {
        return;
    }

    auto* node = re4vr::call_safe<::REManagedObject*>(fsm, "getCurrentNodeName", WSW_LAYER);

    if (node == nullptr) {
        return;
    }

    std::string s{};

    try {
        s = utility::re_string::get_string(reinterpret_cast<::SystemString*>(node));
    } catch (...) {
        return;
    }

    if (s.find("ChangeWeapon") == std::string::npos) {
        return;
    }

    auto* layer = re4vr::call_safe<::REManagedObject*>(motion, "getLayer", WSW_LAYER);

    if (layer == nullptr) {
        return;
    }

    // 1) Transition-Pose unsichtbar machen (kein 1-Frame-Flash): Blendgewicht 0.
    re4vr::call_safe<void*>(layer, "set_BlendRate", 0.0f);
    re4vr::call_safe<void*>(layer, "set_Weight", 0.0f);

    // 2) Trotzdem ans Ende fahren -> FSM transitioniert weiter, Equip-Events
    //    feuern.
    float ef = 0.0f;
    re4vr::try_call<float>(layer, "get_EndFrame", ef);

    if (ef > 1.0f) {
        re4vr::call_safe<void*>(layer, "set_Frame", ef - 0.5f);
    }
}

// ============================================================================
// Baustein 11-14 -- KNIFE_MELEE (Lua Z.1471-1902)
// ============================================================================

::REManagedObject* RE4VRWeapons::get_hc(::REManagedObject* go) {
    if (m_hitmgr.obj == nullptr) {
        if (auto* h = sdk::get_managed_singleton<::REManagedObject>("chainsaw.HitManager")) {
            store(m_hitmgr, h);
        }
    }

    if (m_hitmgr.obj == nullptr || go == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(m_hitmgr.obj, "getHitController", go);
}

std::optional<int32_t> RE4VRWeapons::weapon_id_num(::REManagedObject* hc) {
    return enum_as_int(hc, "get_WeaponID");
}

// Messer-HC ueber den Player-Body finden (GO-Name wpXXXX, gefiltert gegen
// KNIFE_IDS). allow_holstered=true nimmt auch ein GEHOLSTERTES Messer
// (Collider aus) -> gibt dem Klon einen ECHTEN Messer-HC fuer requestAttack.
::REManagedObject* RE4VRWeapons::find_wp_hc(::REManagedObject* tf, int32_t depth,
                                            std::optional<int32_t> want_id,
                                            bool allow_holstered) {
    if (tf == nullptr || depth > 12) {
        return nullptr;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int32_t guard = 0;

    while (child != nullptr && guard < 400) {
        ++guard;

        auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");
        const std::string s = obj_name_of(go);

        // Bewaehrter Namensfilter: wp5000-5006 (^wp5), wp6107/6108 (SW),
        // wp6305 (Hot Dogger).
        if (!s.empty()
            && (s.rfind("wp5", 0) == 0 || s.rfind("wp6107", 0) == 0
                || s.rfind("wp6108", 0) == 0 || s.rfind("wp6305", 0) == 0)) {
            auto* hc = get_hc(go);

            if (hc != nullptr) {
                bool valid = true;
                re4vr::try_call<bool>(go, "get_Valid", valid);

                bool collider = allow_holstered;

                if (!allow_holstered) {
                    auto* rsc =
                        re4vr::call_safe<::REManagedObject*>(hc, "get_RequestSetCollider");
                    collider = rsc != nullptr;
                }

                if (valid && collider) {
                    if (!want_id.has_value() || weapon_id_num(hc) == want_id) {
                        return hc;
                    }
                }
            }
        }

        if (auto* r = find_wp_hc(child, depth + 1, want_id, allow_holstered)) {
            return r;
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return nullptr;
}

// [CACHE_SOLID] get_RequestSetCollider allein reicht NICHT: eine nach
// Save-Load ausgetauschte (tote) Instanz behaelt ihren Collider im Speicher ->
// stale Zeiger. get_Valid erkennt die tote Instanz.
bool RE4VRWeapons::hc_still_active(::REManagedObject* hc, std::optional<int32_t> want_id) {
    if (hc == nullptr) {
        return false;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(hc, "get_GameObject");

    if (go == nullptr) {
        return false;
    }

    bool valid = true;

    if (re4vr::try_call<bool>(go, "get_Valid", valid) && !valid) {
        return false;   // tote Instanz nach Save-Load
    }

    if (re4vr::call_safe<::REManagedObject*>(hc, "get_RequestSetCollider") == nullptr) {
        return false;   // geholstert/entschaerft
    }

    if (want_id.has_value() && weapon_id_num(hc) != want_id) {
        return false;   // andere Waffe gewechselt -> neu suchen
    }

    return true;
}

::REManagedObject* RE4VRWeapons::find_knife_hc() {
    const double now = clock_now();
    const auto want_id = get_equip_weapon_id();

    // Cache nur behalten, wenn er aktiv/valid ist, die AKTUELL equippte
    // WeaponID traegt UND juenger als 1 s ist.
    if (m_knife_hc_cache.obj != nullptr && hc_still_active(m_knife_hc_cache.obj, want_id)
        && (now - m_knife_hc_refresh_t) < 1.0) {
        return m_knife_hc_cache.obj;
    }

    drop(m_knife_hc_cache);
    m_knife_hc_refresh_t = now;

    // [LH_REAL 2026-07-08] Klon-Zustand: das an die LINKE Hand GEPINNTE echte
    // Messer hat VORRANG -- sein Collider sitzt am Gegner. NICHT mit einem
    // zufaelligen gemounteten Messer ueberschreiben.
    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
        auto* c = re4vr::lua_get_pointer("__re4_knife_hc_cache");
        auto* cgo = c != nullptr ? re4vr::call_safe<::REManagedObject*>(c, "get_GameObject")
                                 : nullptr;

        if (cgo != nullptr) {
            bool valid = true;

            if (!re4vr::try_call<bool>(cgo, "get_Valid", valid) || valid) {
                store(m_knife_hc_cache, c);

                return c;
            }
        }

        // [LH_CLONE HC] Kein gepinntes Messer + im Klon ist eine GUN equippt ->
        // die normale Suche findet KEIN Messer. Deshalb ein GEHOLSTERTES
        // zulassen.
        auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();
        auto* kh = tf != nullptr ? find_wp_hc(tf, 0, std::nullopt, true) : nullptr;

        if (kh != nullptr) {
            store(m_knife_hc_cache, kh);

            return kh;
        }
    }

    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    // exakt die equippte Waffe
    auto* hc = tf != nullptr ? find_wp_hc(tf, 0, want_id, false) : nullptr;

    // Fallback: irgendein valides Messer
    if (hc == nullptr && tf != nullptr) {
        hc = find_wp_hc(tf, 0, std::nullopt, false);
    }

    store(m_knife_hc_cache, hc);

    return m_knife_hc_cache.obj;
}

// [KNIFE_SND] Messer-Aktions-Sounds ueber den SoundContainer der Waffe.
// [TON-AUSFALL 2026-08-26] Haengt das Messer gerade NICHT am Body (im Flug, im
// Gegner steckend, Waffenwechsel), lieferte die Suche nil und der Ton entfiel
// ersatzlos -- deshalb der gemerkte Rueckfall.
void RE4VRWeapons::play_knife_sound(int32_t id) {
    // [GROSSE IDs 2026-09-08] Wwise-IDs sind UInt32 -- alles ab 2^31 (Wurf:
    // 3788596668) ist als int32_t negativ und fiel mit `id <= 0` lautlos raus.
    // Unten wird nach uint32_t zurueckgecastet, der Bitwert stimmt.
    if (id == 0) {
        return;
    }

    ::REManagedObject* scn = nullptr;

    auto* hc = find_knife_hc();
    auto* go = hc != nullptr ? re4vr::call_safe<::REManagedObject*>(hc, "get_GameObject")
                             : nullptr;

    if (go != nullptr) {
        scn = re4vr::get_component(go, "soundlib.SoundContainer");
    }

    if (scn != nullptr) {
        store(m_knife_snd_last, scn);
    } else if (m_knife_snd_last.obj != nullptr) {
        bool valid = true;

        if (!re4vr::try_call<bool>(m_knife_snd_last.obj, "get_Valid", valid) || valid) {
            scn = m_knife_snd_last.obj;   // derselbe Messer-Container wie zuletzt
        }
    }

    if (scn == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", static_cast<uint32_t>(id));
}

// Ziel: bevorzugt anvisierter Gegner (get_AimTargetEnemy), sonst naechster
// Gegner. proximity_only: den Aim-Shortcut UEBERSPRINGEN (fuer den Wurf-Flug).
::REManagedObject* RE4VRWeapons::knife_pick_target(const std::optional<glm::vec3>& kpos_in,
                                                   bool proximity_only,
                                                   const std::optional<float>& reach_in) {
    const float reach = reach_in.value_or(static_cast<float>(
        re4vr::lua_get_number("__re4_knife_reach", KNIFE_REACH_DEF)));

    auto* ctx = get_player_ctx_w();

    if (!proximity_only && ctx != nullptr) {
        if (auto* aim = re4vr::call_safe<::REManagedObject*>(ctx, "get_AimTargetEnemy")) {
            return aim;
        }
    }

    auto* cm = re4vr::character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList")
        : nullptr;

    int32_t count = 0;
    bool count_ok = false;

    if (list != nullptr) {
        count_ok = re4vr::try_call<int32_t>(list, "get_Count", count);
    }

    // [MESSER-MESSUNG 2026-09-06] Zwischenstaende fuer die Melee-Zeile.

    // kpos-Fallback: Player-Position, falls die VR-Hand-Welt noch nicht da ist
    glm::vec3 kpos{};
    bool have_kpos = kpos_in.has_value();

    if (have_kpos) {
        kpos = *kpos_in;
    } else if (ctx != nullptr) {
        have_kpos = get_vec3(ctx, "get_Position", kpos);
    }

    ::REManagedObject* bestctx = nullptr;
    float bestd = 9999.0f;

    // [MESSUNG 2026-09-06] pro Filter zaehlen, welcher die Gegner verwirft.
    int32_t d_item = 0, d_hp = 0, d_dead = 0, d_hpv = 0, d_pos = 0, d_ok = 0;
    double d_first_hp = -1.0;

    if (have_kpos && count > 0) {
        for (int32_t i = 0; i < count; ++i) {
            auto* ectx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

            if (ectx == nullptr) {
                ++d_item;
                continue;
            }

            // TOTE Gegner (Leichen) ueberspringen -> kein Stich in die Leiche
            auto* hp = re4vr::call_safe<::REManagedObject*>(ectx, "get_HitPoint");

            if (hp == nullptr) {
                ++d_hp;
                continue;
            }

            bool dead = false;

            if (re4vr::try_call<bool>(hp, "get_IsDead", dead) && dead) {
                ++d_dead;
                continue;
            }

            // [PORTFIX 2026-09-06] typrichtig aus der TDB lesen -- als blindes
            // float kam hier 0 an und JEDER Gegner galt als tot.
            std::string hp_type{};
            const auto cur = re4vr::call_num(hp, "get_CurrentHitPoint", &hp_type);

            if (d_first_hp < 0.0) {
                d_first_hp = cur.value_or(-99.0);
            }

            if (!cur.has_value() || *cur <= 0.0) {
                ++d_hpv;
                continue;
            }

            // CharacterContext hat get_Position DIREKT
            glm::vec3 pos{};

            if (!get_vec3(ectx, "get_Position", pos)) {
                ++d_pos;
                continue;
            }

            ++d_ok;

            // Distanz Hand -> Gegner-KOERPERMITTE (Root + Hoehe)
            const float dx = pos.x - kpos.x;
            const float dy = (pos.y + BODY_CENTER_Y) - kpos.y;
            const float dz = pos.z - kpos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (d < bestd) {
                bestd = d;
                bestctx = ectx;
            }
        }
    }


    if (bestctx != nullptr && bestd <= reach) {
        // Ziel-GO fuer requestAttack
        auto* go = re4vr::call_safe<::REManagedObject*>(bestctx, "get_BodyGameObject");

        re4vr::lua_set_managed_object("__re4_knife_last_target_ctx", bestctx);


        return go;
    }

    return nullptr;
}

// AttackUserData beschaffen: IMMER FRISCH vom Collider (gecachte/wiederverwendete
// UD staggert nur, macht keinen Schaden).
::REManagedObject* RE4VRWeapons::knife_get_attack_ud(::REManagedObject* hc) {
    auto* rsc = re4vr::call_safe<::REManagedObject*>(hc, "get_RequestSetCollider");

    if (rsc == nullptr) {
        return nullptr;
    }

    int32_t nrs = 0;
    re4vr::try_call<int32_t>(rsc, "get_NumRequestSetIds", nrs);

    ::REManagedObject* first = nullptr;
    ::REManagedObject* want = nullptr;

    const int32_t lim = std::min(nrs, 12);

    for (int32_t i = 0; i < lim; ++i) {
        auto* ud = re4vr::call_safe<::REManagedObject*>(rsc, "getRequestSetUserData", i);

        if (ud == nullptr) {
            continue;
        }

        auto* td = utility::re_managed_object::get_type_definition(ud);
        const std::string tn = td != nullptr ? td->get_full_name() : std::string{};

        if (tn.find("AttackUserData") == std::string::npos) {
            continue;
        }

        if (first == nullptr) {
            first = ud;
        }

        if (i == KNIFE_ATK_IDX) {
            want = ud;
        }
    }

    if (want != nullptr) {
        re4vr::lua_set_managed_object("__re4_knife_atk_cache", want);

        return want;
    }

    if (first != nullptr) {
        re4vr::lua_set_managed_object("__re4_knife_atk_cache", first);

        return first;
    }

    // [5002-FIX] Dieses Messer liefert keine eigene AttackUD -> leihen.
    return borrow_knife_atk();
}

// ============================================================================
// [ADA-TIER 17.09.2026 -- Ansage des Users] Messer ohne eigene Angriffsdaten
// ============================================================================
// Gemessen (zzz_re4_maus_melee_probe.lua): Adas Messer wp6108 traegt nur zwei
// via.physics.RequestSetColliderUserData und KEINE AttackUserData -- der
// Tiertreffer (requestAttack) fand dadurch nie Angriffsdaten, Melee UND Wurf
// zogen der Ratte nichts ab. Ihr KOERPER (ch3a8z0_body) hat 16 AttackUserData.
// Genommen werden deshalb Koerper-HitController und dessen ERSTE
// AttackUserData -- derselbe Koerper-HitController, ueber den auch die
// Gegnertreffer laufen (RE4VRWeapons2::knife_direct_damage_at).
// Nur ein Rueckfall: hat das Messer eigene Angriffsdaten (Leon), wird das hier
// nie gerufen.
bool RE4VRWeapons::body_attack_fallback(::REManagedObject*& hc_out, ::REManagedObject*& atk_out) {
    hc_out = nullptr;
    atk_out = nullptr;

    auto* body = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
    auto* hc = body != nullptr ? get_hc(body) : nullptr;
    auto* rsc = hc != nullptr ? re4vr::call_safe<::REManagedObject*>(hc, "get_RequestSetCollider")
                              : nullptr;

    if (rsc == nullptr) {
        return false;
    }

    int32_t nrs = 0;
    re4vr::try_call<int32_t>(rsc, "get_NumRequestSetIds", nrs);

    const int32_t lim = std::min(nrs, 32);

    for (int32_t i = 0; i < lim; ++i) {
        auto* ud = re4vr::call_safe<::REManagedObject*>(rsc, "getRequestSetUserData", i);

        if (ud == nullptr) {
            continue;
        }

        auto* td = utility::re_managed_object::get_type_definition(ud);
        const std::string tn = td != nullptr ? td->get_full_name() : std::string{};

        if (tn.find("AttackUserData") == std::string::npos) {
            continue;
        }

        hc_out = hc;
        atk_out = ud;

        return true;
    }

    return false;
}

// Ueber alle registrierten HitController-GameObjects iterieren.
// [ARRAY-BINDING] Lua: `entries:get_Length()` + `entries[i]` -- beides
// REFramework-Bindings auf dem _entries-Array, KEINE managed Calls.
void RE4VRWeapons::iter_hitctrl_gos(const std::function<void(::REManagedObject*)>& cb) {
    if (m_hitmgr.obj == nullptr) {
        if (auto* h = sdk::get_managed_singleton<::REManagedObject>("chainsaw.HitManager")) {
            store(m_hitmgr, h);
        }
    }

    if (m_hitmgr.obj == nullptr) {
        return;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(m_hitmgr.obj, "get_HitControllerList");

    if (list == nullptr) {
        return;
    }

    auto* entries = re4vr::get_field_object(list, "_entries");

    if (entries == nullptr) {
        return;
    }

    const int32_t n = std::min(re4vr::array_size(entries), 4096);

    for (int32_t i = 0; i < n; ++i) {
        auto* e = re4vr::array_element(entries, i);

        if (e == nullptr) {
            continue;
        }

        // Value-Type-Entry -> das Feld "key" traegt das GameObject.
        auto* go = re4vr::get_field_object(e, "key");

        if (go != nullptr) {
            cb(go);
        }
    }
}

// [5002-FIX / WP5002-STAGE] Eine AttackUserData leihen. Die UD ist nur der
// AUSLOESER (der Schaden kommt aus dem Damage-Hook) -> es taugt JEDE Waffen-UD,
// auch die eines GEGNERS. Messer bevorzugt, sonst irgendeine Waffe.
::REManagedObject* RE4VRWeapons::borrow_knife_atk() {
    if (auto* c = re4vr::lua_get_pointer("__re4_knife_atk_cache")) {
        return c;
    }

    ::REManagedObject* found_knife = nullptr;
    ::REManagedObject* found_any = nullptr;

    iter_hitctrl_gos([&](::REManagedObject* go) {
        if (found_knife != nullptr) {
            return;   // Messer gefunden -> fertig
        }

        const std::string nm = obj_name_of(go);

        if (nm.rfind("wp", 0) != 0) {
            return;   // nur Waffen (wp*), aber JEDE
        }

        size_t p = 2;
        std::string digits{};

        while (p < nm.size() && std::isdigit(static_cast<unsigned char>(nm[p]))) {
            digits.push_back(nm[p]);
            ++p;
        }

        if (digits.empty()) {
            return;
        }

        const bool is_knife = is_knife_id(std::atoi(digits.c_str()));

        auto* hc = get_hc(go);
        auto* rsc = hc != nullptr
            ? re4vr::call_safe<::REManagedObject*>(hc, "get_RequestSetCollider")
            : nullptr;

        if (rsc == nullptr) {
            return;
        }

        int32_t nrs = 0;
        re4vr::try_call<int32_t>(rsc, "get_NumRequestSetIds", nrs);

        // Cap auf 32 (manche Waffen tragen die AttackUD hoeher als 12).
        const int32_t lim = std::min(nrs, 32);

        for (int32_t i = 0; i < lim; ++i) {
            auto* ud = re4vr::call_safe<::REManagedObject*>(rsc, "getRequestSetUserData", i);

            if (ud == nullptr) {
                continue;
            }

            auto* td = utility::re_managed_object::get_type_definition(ud);
            const std::string tn = td != nullptr ? td->get_full_name() : std::string{};

            if (tn.find("AttackUserData") == std::string::npos) {
                continue;
            }

            if (is_knife) {
                found_knife = ud;
            } else if (found_any == nullptr) {
                found_any = ud;
            }

            return;
        }
    });

    auto* found = found_knife != nullptr ? found_knife : found_any;

    if (found != nullptr) {
        re4vr::lua_set_managed_object("__re4_knife_atk_cache", found);
    }

    return found;
}

// [KNIFE_HAND] Weltposition der Hand, die das Messer GERADE haelt.
std::optional<glm::vec3> RE4VRWeapons::knife_hand_world() {
    // [LH_CLONE] Klon-Modus = Messer in der LINKEN Hand (nicht equippt).
    if (re4vr::lua_get_string("__re4_knife_hand") == "left"
        || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
        if (const auto l = re4vr::lua_get_vec3("__vr_lh_world"); l.has_value()) {
            return l;
        }
    }

    return re4vr::lua_get_vec3("__vr_rh_world");
}

// [BREAKABLES] Beruehrungs-Position fuer den In-Hand-Melee. Die Messer-Objekt-
// Root ist NICHT die sichtbare Klinge (entkoppelt/stale) -> die echte
// Hand-Weltposition nutzen; nur als Fallback die Objekt-Root.
std::optional<glm::vec3> RE4VRWeapons::knife_obj_pos() {
    if (const auto rh = knife_hand_world(); rh.has_value()) {
        return rh;
    }

    auto* hc = find_knife_hc();

    if (hc == nullptr) {
        return std::nullopt;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(hc, "get_GameObject");
    auto* tf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
                             : nullptr;

    if (tf == nullptr) {
        return std::nullopt;
    }

    glm::vec3 p{};

    return get_vec3(tf, "get_Position", p) ? std::optional<glm::vec3>{p} : std::nullopt;
}

// ============================================================================
// Baustein 15 -- BREAKABLES (Lua Z.1904-2328)
// ============================================================================

// [PARENT-WOODBOX] Die zerstoerbaren Kisten haengen als "Before"/"After"-
// Collider-KINDER unter der WoodBox: die Kinder sitzen an der ECHTEN Position,
// die WoodBox-Logik + ihr VERSETZTER Origin am PARENT.
bool RE4VRWeapons::wb_of(::REManagedObject* go, ::REManagedObject*& wb_out,
                         ::REManagedObject*& dur_out, ::REManagedObject*& holder_out) {
    wb_out = nullptr;
    dur_out = nullptr;
    holder_out = nullptr;

    if (go == nullptr) {
        return false;
    }

    wb_out = re4vr::get_component(go, "chainsaw.GmWoodBoxBase");
    dur_out = re4vr::get_component(go, "chainsaw.IGimmickDurability");

    if (wb_out != nullptr || dur_out != nullptr) {
        holder_out = go;

        return true;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
    auto* ptf = tf != nullptr ? re4vr::call_safe<::REManagedObject*>(tf, "get_Parent")
                              : nullptr;
    auto* pgo = ptf != nullptr ? re4vr::call_safe<::REManagedObject*>(ptf, "get_GameObject")
                               : nullptr;

    if (pgo != nullptr) {
        wb_out = re4vr::get_component(pgo, "chainsaw.GmWoodBoxBase");
        dur_out = re4vr::get_component(pgo, "chainsaw.IGimmickDurability");

        if (wb_out != nullptr || dur_out != nullptr) {
            holder_out = pgo;

            return true;
        }
    }

    return false;
}

std::optional<int32_t> RE4VRWeapons::get_break_routine() {
    // Lua cacht das in __re4_rt_break, inkl. `or false` -- ein Enum-Wert 0
    // waere dort FALSE und der Zweig liefe nie. 1:1 uebernommen.
    static std::optional<int32_t> cached{};
    static bool looked_up = false;

    if (!looked_up) {
        looked_up = true;

        auto* rt = sdk::find_type_definition("chainsaw.GmWoodBoxBase.RoutineType");
        auto* f = rt != nullptr ? rt->get_field("Break") : nullptr;

        if (f != nullptr) {
            try {
                cached = f->get_data<int32_t>(nullptr, false);
            } catch (...) {
                cached.reset();
            }
        }
    }

    return cached;
}

// [LH_CLONE BRUCHSOUND 2026-07-11] Der Klon hat keinen aktiven Collider -> die
// native Damage->Break-Kette laeuft nicht. Den SoundContainer per Parent-Walk
// finden (bei der Vase sitzt er am PARENT, nicht am Collider-Kind) und seine
// EIGENE erste Trigger-ID feuern. VOR set_Routine, solange der Emitter lebt.
void RE4VRWeapons::clone_break_sound(::REManagedObject* start_go) {
    if (start_go == nullptr) {
        return;
    }

    auto* t = re4vr::call_safe<::REManagedObject*>(start_go, "get_Transform");
    ::REManagedObject* sc = nullptr;

    for (int32_t i = 0; i <= 3; ++i) {
        if (t == nullptr) {
            break;
        }

        auto* g = re4vr::call_safe<::REManagedObject*>(t, "get_GameObject");
        sc = g != nullptr ? re4vr::get_component(g, "soundlib.SoundContainer") : nullptr;

        if (sc != nullptr) {
            break;
        }

        t = re4vr::call_safe<::REManagedObject*>(t, "get_Parent");
    }

    if (sc == nullptr) {
        return;
    }

    auto* lst = re4vr::get_field_object(sc, "_TriggerInfoList");

    if (lst == nullptr) {
        return;
    }

    int32_t cnt = 0;

    if (!re4vr::try_call<int32_t>(lst, "get_Count", cnt) || cnt <= 0) {
        return;
    }

    auto* info = re4vr::call_safe<::REManagedObject*>(lst, "get_Item", 0);
    const auto id = info != nullptr ? re4vr::get_field_int(info, "_TriggerId") : std::nullopt;

    if (id.has_value()) {
        re4vr::call_safe<void*>(sc, "trigger(System.UInt32)", static_cast<uint32_t>(*id));
    }
}

// [BRUCHSOUND IN DER HAND 2026-08-24] Ein Ausloesen pro Objekt und 0,6 s --
// sonst rattert der In-Hand-Stich (der pro Frame in Reichweite ist).
void RE4VRWeapons::break_sound_once(::REManagedObject* go) {
    if (go == nullptr) {
        return;
    }

    // Klon und Wurf klingen weiter wie bisher, auch wenn der Schalter aus ist.
    if (re4vr::lua_get_tribool("__re4_break_snd_inhand") != 1
        && re4vr::lua_get_tribool("__re4_knife_left_clone") != 1
        && re4vr::lua_get_tribool("__re4_knife_flying") != 1) {
        return;
    }

    const auto k = addr_of(go).value_or(0);
    const double now = clock_now();

    if (const auto it = m_brk_snd_t.find(k);
        it != m_brk_snd_t.end() && (now - it->second) < 0.6) {
        return;
    }

    m_brk_snd_t[k] = now;

    // Aufraeumen, damit die Tabelle ueber eine lange Sitzung nicht waechst.
    if (++m_brk_snd_n > 200) {
        for (auto it = m_brk_snd_t.begin(); it != m_brk_snd_t.end();) {
            if ((now - it->second) > 10.0) {
                it = m_brk_snd_t.erase(it);
            } else {
                ++it;
            }
        }

        m_brk_snd_n = 0;
    }

    clone_break_sound(go);
}

// [PRAEZISER TREFFER 2026-08-12] Sitzt die Klinge wirklich AM Objekt? Geprueft
// gegen die echte Mesh-Box (WorldAABB + Rand), nicht gegen eine feste Kugel.
// Das Mesh sitzt oft am ELTERN-GO -> bis zu zwei Ebenen hoch.
// Rueckgabe nullopt, wenn gar kein Mesh gefunden wurde -> der Aufrufer faellt
// auf seinen Radius zurueck, damit nichts lautlos untreffbar wird.
//
// AABB per get_field lesen: getCenter & Co. liefern bei ValueTypes plausiblen
// Muell ([[reference_re4_reframework_valuetype_getcenter_muell]]).
std::optional<bool> RE4VRWeapons::hit_in_mesh_box(::REManagedObject* go, const glm::vec3& kp,
                                                  float margin) {
    if (go == nullptr) {
        return std::nullopt;
    }

    auto* node = go;

    for (int32_t i = 0; i <= 2; ++i) {
        if (node == nullptr) {
            break;
        }

        auto* mesh = re4vr::get_component(node, "via.render.Mesh");

        if (mesh != nullptr) {
            // [VALUETYPE-RUECKGABE] get_WorldAABB liefert 32 Byte ueber den
            // sret-Puffer -- als Objektzeiger gerufen schreibt die Engine in
            // den VMContext ([[reference_re4_cpp_valuetype_rueckgabe_sret]]).
            glm::vec3 mn{};
            glm::vec3 mx{};

            if (mesh_world_aabb(mesh, mn, mx)) {
                if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) {
                    return std::nullopt;   // leere AABB
                }

                return kp.x >= mn.x - margin && kp.x <= mx.x + margin
                    && kp.y >= mn.y - margin && kp.y <= mx.y + margin
                    && kp.z >= mn.z - margin && kp.z <= mx.z + margin;
            }
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(node, "get_Transform");
        auto* pt = tf != nullptr ? re4vr::call_safe<::REManagedObject*>(tf, "get_Parent")
                                 : nullptr;
        node = pt != nullptr ? re4vr::call_safe<::REManagedObject*>(pt, "get_GameObject")
                             : nullptr;
    }

    return std::nullopt;
}

// [BREAKABLES] Auf Schwung: alles in Beruehr-Reichweite des Messer-OBJEKTS
// zerbrechen. max_dy (optional) schaltet die PRAEZISE Pruefung ein -- nur der
// Flug-Scan setzt ihn.
int32_t RE4VRWeapons::break_nearby(const std::optional<glm::vec3>& kp_override,
                                   const std::optional<float>& radius_override,
                                   const std::optional<float>& max_dy) {
    const auto kp_o = kp_override.has_value() ? kp_override : knife_obj_pos();

    if (!kp_o.has_value()) {
        return 0;
    }

    const glm::vec3 kp = *kp_o;

    auto* hc = find_knife_hc();
    auto* atk = hc != nullptr ? knife_get_attack_ud(hc) : nullptr;

    // Angreifer-HitController fuer die Klon-Wiedergabe (weapons2)
    re4vr::lua_set_managed_object("__re4_knife_atk_hc", hc);

    // [FEHLER GEFUNDEN 2026-07-23] Der Ausgangszustand MUSS gemerkt werden:
    // HitController stehen von Haus aus auf AttackEnable=true. Bedingungsloses
    // Zuruecksetzen liess den Messer-Collider entschaerft liegen -> Nahkampf,
    // Wurf und Kisten waren tot.
    bool was_enabled = false;

    if (hc != nullptr) {
        re4vr::try_call<bool>(hc, "get_AttackEnable", was_enabled);
    }

    const float touch = radius_override.value_or(
        static_cast<float>(re4vr::lua_get_number("__re4_knife_touch", 0.90)));

    // [PROP-RADIUS 2026-07-23] Haengende Whitelist-Props (Muenze) haben ihren
    // Ursprung oben am Pendel-Drehpunkt. Fest verdrahtet, gilt NUR fuer den
    // Whitelist-Zweig.
    constexpr float PROP_TOUCH = 1.00f;

    bool scharf = false;
    int32_t broke = 0;

    iter_hitctrl_gos([&](::REManagedObject* go) {
        const std::string nm = obj_name_of(go);

        if (nm.size() >= 2) {
            const std::string p2 = nm.substr(0, 2);

            // Player/Gegner/Waffen aus
            if (p2 == "ch" || p2 == "em" || p2 == "wp") {
                return;
            }
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        glm::vec3 pos{};

        if (tf == nullptr || !get_vec3(tf, "get_Position", pos)) {
            return;
        }

        // [RE9-LEHRE] NUR horizontale Distanz (X/Z), Y ignorieren: der GO-Origin
        // sitzt bei einem Fass am Boden, bei einer Regalkiste in der Mesh-Mitte.
        const float dx = pos.x - kp.x;
        const float dz = pos.z - kp.z;
        const float d2 = dx * dx + dz * dz;
        const float scan_t = PROP_TOUCH > touch ? PROP_TOUCH : touch;

        if (d2 > scan_t * scan_t) {
            return;
        }

        // Im FLUG entscheidet die echte Mesh-Box; kein Mesh -> Hoehenfenster.
        if (max_dy.has_value()) {
            const float margin = static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_fly_cfg", "hit_margin", 0.15));
            const auto inbox = hit_in_mesh_box(go, kp, margin);

            if (inbox.has_value() && !*inbox) {
                return;
            }

            if (!inbox.has_value() && std::abs(pos.y - kp.y) > *max_dy) {
                return;
            }
        }

        // ItemDisp (Item-Anzeige / Pickup) ist KEIN Breakable.
        if (nm.rfind("ItemDis", 0) == 0) {
            return;
        }

        // Lebende Tiere (GmAnimal): TOTE ignorieren, LEBENDE bekommen HIER
        // ihren eigenen requestAttack (sie haben KEINE WoodBox/Durability).
        if (auto* animal = re4vr::get_component(go, "chainsaw.GmAnimal")) {
            // [TIER-RADIUS 2026-08-12] eigener, engerer Radius.
            const float at = static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_fly_cfg", "animal_touch", 0.35));

            if (d2 > at * at) {
                return;
            }

            bool dead = false;

            if (re4vr::try_call<bool>(animal, "get_IsDead", dead) && dead) {
                return;
            }

            // [ADA-TIER 17.09.2026] Hat das Messer keine eigenen Angriffsdaten
            // (Adas wp6108), greift der Koerper ein -- s. body_attack_fallback.
            // Mit eigenen Daten (Leon) bleibt alles wie bisher.
            ::REManagedObject* a_hc = hc;
            ::REManagedObject* a_atk = atk;
            bool body_path = false;

            if (a_atk == nullptr) {
                body_path = body_attack_fallback(a_hc, a_atk);
            }

            if (a_hc != nullptr && a_atk != nullptr) {
                auto* dmg = sdk::create_instance<::REManagedObject>(
                    "chainsaw.collision.DamageUserData", true);

                if (dmg == nullptr) {
                    dmg = sdk::create_instance<::REManagedObject>(
                        "chainsaw.collision.DamageUserData");
                }

                if (dmg != nullptr) {
                    // Der Koerper-HitController wird nur fuer DIESEN Aufruf scharf
                    // gemacht und gleich wieder so hinterlassen, wie er war.
                    bool body_was = true;

                    if (body_path) {
                        re4vr::try_call<bool>(a_hc, "get_AttackEnable", body_was);

                        if (!body_was) {
                            re4vr::call_safe<void*>(a_hc, "set_AttackEnable", true);
                        }
                    } else if (!scharf) {
                        re4vr::call_safe<void*>(hc, "set_AttackEnable", true);
                        scharf = true;
                    }

                    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
                    re4vr::call_safe<void*>(a_hc, "requestAttack", go, a_atk, dmg);

                    if (body_path && !body_was) {
                        re4vr::call_safe<void*>(a_hc, "set_AttackEnable", false);
                    }

                    // [KLON 2026-07-23] Ohne Klingen-Collider wird requestAttack
                    // nie aufgeloest -> den Empfang selbst nachbauen.
                    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
                        lua_call_global_void("__re4_knife_onhit_replay", go, a_atk);

                        // [TIER-SOUND 2026-08-12] Tiere waren der einzige
                        // Trefferfall, den wir selbst herstellen, ohne ihn
                        // hoerbar zu machen. NUR linkes Messer, NUR Tiere.
                        if (re4vr::lua_get_tribool("__re4_knife_flying") == 1) {
                            play_knife_sound(static_cast<int32_t>(re4vr::lua_get_table_number(
                                "__re4_knife_snd", "flesh", 238304172.0)));
                        } else {
                            lua_call_global_void(
                                "__re4_knife_lh_play_sound",
                                std::floor(re4vr::lua_get_number("__re4_knife_lh_hit_snd",
                                                                 238304172.0)));
                        }
                    }

                    ++broke;
                }
            }

            return;   // Huhn behandelt -> nicht als WoodBox weiterpruefen
        }

        ::REManagedObject* woodbox = nullptr;
        ::REManagedObject* dur = nullptr;
        ::REManagedObject* holder = nullptr;
        wb_of(go, woodbox, dur, holder);

        // Kisten/Faesser: unveraendert der enge touch-Radius
        if ((dur != nullptr || woodbox != nullptr) && d2 > touch * touch) {
            return;
        }

        if (dur != nullptr) {
            // [PORTFIX 2026-09-06] typrichtig lesen -- s. oben.
            const auto cur_d = re4vr::call_num(dur, "get_CurrentDurability");
            const float cur = cur_d.has_value() ? static_cast<float>(*cur_d) : 0.0f;

            if (cur_d.has_value() && cur > 0.0f) {
                // [BRUCHSOUND] Der Sound gehoert dem OBJEKT. Objekt-eigenen
                // Trigger VOR addDurability feuern, solange der Emitter lebt.
                break_sound_once(go);
                re4vr::call_safe<void*>(dur, "addDurability", -(cur + 1.0f));
                ++broke;
            }

            return;
        }

        if (woodbox != nullptr) {
            bool broken = false;
            re4vr::try_call<bool>(woodbox, "get_IsBroken", broken);

            const auto br = get_break_routine();

            if (broken || !br.has_value()) {
                return;
            }

            break_sound_once(go);

            // [ZITTERN 2026-07-23] Beim Wurf laeuft break_nearby PRO FRAME, und
            // get_IsBroken springt nicht sofort um -> set_Routine(Break) wurde
            // mehrfach gesetzt und die Bruch-Animation neu gestartet.
            // Ein Ausloesen pro Objekt und Sekunde.
            {
                const auto key = addr_of(go).value_or(0);
                const double now = clock_now();

                if (const auto it = m_brk_snd_t.find(key | 0x1ull);
                    it != m_brk_snd_t.end() && (now - it->second) < 1.0) {
                    return;
                }

                m_brk_snd_t[key | 0x1ull] = now;
            }

            re4vr::call_safe<void*>(woodbox, "set_Routine", *br);

            // [NATIVE-FLAG] set_Routine(Break) allein liess einen inkonsistenten
            // Zustand -> zusaetzlich echten Schaden via requestAttack, damit die
            // Engine ihre Break-Completion faehrt (IsBroken + Loot-Drop).
            if (hc != nullptr && atk != nullptr) {
                auto* dmg = sdk::create_instance<::REManagedObject>(
                    "chainsaw.collision.DamageUserData", true);

                if (dmg == nullptr) {
                    dmg = sdk::create_instance<::REManagedObject>(
                        "chainsaw.collision.DamageUserData");
                }

                if (dmg != nullptr) {
                    if (!scharf) {
                        re4vr::call_safe<void*>(hc, "set_AttackEnable", true);
                        scharf = true;
                    }

                    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
                    re4vr::call_safe<void*>(hc, "requestAttack", go, atk, dmg);
                }
            }

            // [LH_CLONE COMPLETION] Klon: requestAttack landet mangels Collider
            // nicht -> Box-Break ueber hitSetting am Box-HitController.
            if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
                lua_call_global_void("__re4_knife_hitset_box", go, pos);
            }

            ++broke;

            return;
        }

        // Schadenbasierte Gimmicks OHNE Durability/WoodBox (z.B. GmOilDrum).
        if (hc == nullptr || atk == nullptr) {
            return;
        }

        if (!lua_call_global<bool>("__re4_is_real_breakable_prop", go).value_or(false)) {
            return;
        }

        if (d2 > PROP_TOUCH * PROP_TOUCH) {
            return;
        }

        // [DURCH DIE WAND 2026-07-23] Der Prop-Radius misst nur horizontal und
        // kennt keine Geometrie -> Sichtlinie pruefen.
        if (!los_clear(kp, pos)) {
            return;
        }

        auto* dmg = sdk::create_instance<::REManagedObject>(
            "chainsaw.collision.DamageUserData", true);

        if (dmg == nullptr) {
            dmg = sdk::create_instance<::REManagedObject>(
                "chainsaw.collision.DamageUserData");
        }

        if (dmg == nullptr) {
            return;
        }

        if (!scharf) {
            re4vr::call_safe<void*>(hc, "set_AttackEnable", true);
            scharf = true;
        }

        re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
        re4vr::call_safe<void*>(hc, "requestAttack", go, atk, dmg);

        if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
            lua_call_global_void("__re4_knife_onhit_replay", go, atk);
            lua_call_global_void("__re4_knife_hitset_box", go, pos);
        }
    });

    // Nur zurueckentschaerfen, wenn er vorher AUCH aus war -- sonst bleibt er
    // scharf, wie vorgefunden.
    if (scharf && !was_enabled && hc != nullptr) {
        re4vr::call_safe<void*>(hc, "set_AttackEnable", false);
    }

    return broke;
}

// ============================================================================
// Baustein 16 -- LOS + Zielhilfe (Lua Z.2330-2517)
// ============================================================================

// [LOS 2026-07-09] Synchrone Sichtlinien-Pruefung ueber via.physics.System.
// castRay (SYNCHRON) auf Layer 10 = Welt-Geometrie. Fail-open: fehlt die
// Methode oder ein Fehler -> true (kein Filter, keine Regression).
bool RE4VRWeapons::los_clear(const glm::vec3& from, const glm::vec3& to) {
    static sdk::REMethodDefinition* s_method = nullptr;
    static bool looked_up = false;

    if (!looked_up) {
        looked_up = true;

        auto* td = sdk::find_type_definition("via.physics.System");
        s_method = td != nullptr
            ? td->get_method("castRay(via.physics.CastRayQuery, via.physics.CastRayResult)")
            : nullptr;
    }

    if (s_method == nullptr) {
        return true;
    }

    auto* sys = sdk::get_native_singleton("via.physics.System");

    if (sys == nullptr) {
        return true;
    }

    // Persistentes, gepinntes Result (kein Alloc-Spam).
    if (m_los_res.obj == nullptr) {
        if (auto* r = sdk::create_instance<::REManagedObject>("via.physics.CastRayResult")) {
            store(m_los_res, r, true);
        }
    }

    if (m_los_res.obj == nullptr) {
        return true;
    }

    const glm::vec3 d = to - from;
    const float dist = glm::length(d);

    if (dist < 0.05f) {
        return true;
    }

    // [PORTFIX 2026-09-06] Die Query wurde hier als EINZIGE Raycast-Stelle im
    // Baum weder gepinnt noch gehalten. `sdk::create_instance` liefert ein
    // Objekt mit refcount 0; in Lua macht REFramework auf dem Weg nach Lua ein
    // `add_ref` mit, hier nicht. Ein refcount-0-Objekt darf die Engine beim
    // naechsten managed Call sofort recyceln -- und genau das passiert, weil
    // zwischen create_instance und castRay noch setRay/clearOptions/
    // enableAllHits/enableNearSort/get_FilterInfo laufen. Danach zeigt `q` in
    // fremden Speicher: "Exception thrown in call to castRay" (c0000005).
    // los_clear ist fail-open, deshalb blieb das unsichtbar -- aber es laeuft
    // in `pick_nearest_enemy` EINMAL PRO GEGNER-KANDIDAT, also mehrfach pro
    // Frame direkt vor jedem Schwung und jedem Wurf. Dieselbe Fehlerklasse wie
    // bei Messer-Flug und Crosshair (dort am 06.09. behoben), diese dritte
    // Stelle war uebersehen. Persistent halten ist hier gefahrlos, weil castRay
    // SYNCHRON ist -- der Auftrag ist zurueck, bevor die Query wiederverwendet
    // wird (anders als beim asynchronen Wurf-Strahl).
    if (m_los_query.obj == nullptr) {
        if (auto* nq = sdk::create_instance<::REManagedObject>("via.physics.CastRayQuery")) {
            store(m_los_query, nq, true);
        }
    }

    auto* q = m_los_query.obj;

    if (q == nullptr) {
        return true;
    }

    bool clear = true;

    try {
        cast_ray_set(q, from, to);

        re4vr::call_safe<void*>(q, "clearOptions");
        re4vr::call_safe<void*>(q, "enableAllHits");
        re4vr::call_safe<void*>(q, "enableNearSort");

        if (auto* fi = re4vr::call_safe<::REManagedObject*>(q, "get_FilterInfo")) {
            re4vr::call_safe<void*>(fi, "set_Group", 0);
            re4vr::call_safe<void*>(fi, "set_MaskBits",
                                    static_cast<uint32_t>(0xFFFFFFFFu & ~1u));
            re4vr::call_safe<void*>(fi, "set_Layer", 10);
            re4vr::call_safe<void*>(q, "set_FilterInfo", fi);
        }

        // [PORTFIX 2026-09-06] `invoke` statt `call_safe` -- das ist der Weg,
        // den Lua nimmt (`method:call` -> Sdk.cpp `method_call` ->
        // `def->invoke(real_obj, build_args(va))`).
        //
        // Der Unterschied steht als Kommentar ueber `call_safe` im Framework
        // selbst: "Does what invoke does WITHOUT ALL THE STUPID SETUP
        // beforehand". `invoke` ruft nicht den rohen Funktionszeiger, sondern
        // den Invoke-Wrapper der Engine ueber einen StackFrame; die Argumente
        // gehen als void*-ARRAY (`in_data`) hinein und der Wrapper legt sie
        // ABI-korrekt an. `call_safe` schiebt sie stattdessen roh in die
        // Register, in der Reihenfolge des Aufrufers. Bei den Physik-Casts
        // passt das nicht: c0000005.
        //
        // Beweis der Zuordnung: "Exception thrown in call to castRay" im
        // Framework-Log kommt aus `VMContext::safe_wrap(get_name(), ...)` --
        // und safe_wrap steckt AUSSCHLIESSLICH in `call_safe`. `invoke` meldet
        // sich anders ("Internal game exception thrown in ...::invoke").
        const auto ret = s_method->invoke(sys, (void*)q, (void*)m_los_res.obj);

        if (ret.exception_thrown) {
            return true;   // fail-open wie bisher
        }

        // [PORTFIX 2026-09-06] typrichtig lesen -- dieselbe Falle wie in
        // knife_read_ray. Hier blieb sie unsichtbar, weil los_clear
        // fail-open ist: n=0 heisst "keine Kontakte" = Sicht frei.
        const auto nc = re4vr::call_num(m_los_res.obj, "get_NumContactPoints");
        const int32_t n = nc.has_value() ? static_cast<int32_t>(*nc) : 0;

        if (n > 0) {
            auto* cp = re4vr::call_safe<::REManagedObject*>(
                m_los_res.obj, "getContactPoint(System.UInt32)", static_cast<uint32_t>(0));

            if (cp != nullptr) {
                const auto hd = field_float(cp, "Distance");

                // Marge 0.35 m: die Ziel-Oberflaeche selbst darf nicht als
                // "Wand" zaehlen.
                if (hd.has_value() && *hd < (dist - 0.35f)) {
                    clear = false;
                }
            }
        }
    } catch (...) {
        clear = true;
    }

    return clear;
}

// [ZIEL-CENTER] Adaptiver Zielpunkt eines Gegners: Mittelpunkt zwischen Root
// (Fuesse) und Kopf-GO -> passt sich der Groesse an, statt festem +0.95.
bool RE4VRWeapons::enemy_aim_point(::REManagedObject* ctx, glm::vec3& out) {
    glm::vec3 pos{};

    if (ctx == nullptr || !get_vec3(ctx, "get_Position", pos)) {
        return false;
    }

    float off = static_cast<float>(
        re4vr::lua_get_table_number("__re4_knife_fly_cfg", "assist_target_off_y", 0.0));

    // [SCHLANGE] enemy-spezifischer Y-Versatz aus whitelist.lua.
    off += static_cast<float>(
        lua_call_global<double>("__re4_enemy_off_y", ctx).value_or(0.0));

    auto* hgo = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadGameObject");
    auto* htf = hgo != nullptr ? re4vr::call_safe<::REManagedObject*>(hgo, "get_Transform")
                               : nullptr;

    glm::vec3 hp{};

    // [KOPF-SANITY] Kopf nur nutzen, wenn er HORIZONTAL nah am Root sitzt
    // (< 1.5 m). get_HeadGameObject lieferte fuer manche Gegner einen FERNEN
    // Kopf -> die "Mitte" lag 30 m weg.
    if (htf != nullptr && get_vec3(htf, "get_Position", hp) && (hp.y - pos.y) > 0.3f) {
        const float hdx = hp.x - pos.x;
        const float hdz = hp.z - pos.z;

        if ((hdx * hdx + hdz * hdz) < (1.5f * 1.5f)) {
            out = glm::vec3{(pos.x + hp.x) * 0.5f, (pos.y + hp.y) * 0.5f + off,
                            (pos.z + hp.z) * 0.5f};

            return true;
        }
    }

    out = glm::vec3{pos.x, pos.y + 0.9f + off, pos.z};   // Fallback

    return true;
}

// [ZIELHILFE] Wurf-Aim-Assist: den Gegner finden, dessen Richtung am naechsten
// an der Wurfrichtung liegt UND innerhalb des Einfang-Kegels ist.
bool RE4VRWeapons::knife_assist_target(const glm::vec3& from, const glm::vec3& dir,
                                       float max_dist, float cone_deg, glm::vec3& out,
                                       ::REManagedObject*& ctx_out) {
    ctx_out = nullptr;

    auto* cm = re4vr::character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList")
        : nullptr;

    if (list == nullptr) {
        re4vr::lua_set_string("__re4_assist_dbg", "no-list");

        return false;
    }

    int32_t count = 0;
    re4vr::try_call<int32_t>(list, "get_Count", count);

    const float cone_cos = std::cos(cone_deg * glm::pi<float>() / 180.0f);

    bool found = false;
    float best_score = 1e9f;
    float seen_best = -2.0f;
    float nearest = 999.0f;
    int32_t npos = 0;

    for (int32_t i = 0; i < count; ++i) {
        auto* ctx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (ctx == nullptr) {
            continue;
        }

        // EnemyContext hat die Position DIREKT (Body-Center).
        glm::vec3 pos{};

        if (!get_vec3(ctx, "get_Position", pos)) {
            continue;
        }

        // [LIVE-FIX] Lebend-Check wie im Melee: get_IsDead + HP>0. Der fruehere
        // get_IsLive-Check schloss NAHE Gegner faelschlich aus.
        auto* hp = re4vr::call_safe<::REManagedObject*>(ctx, "get_HitPoint");

        if (hp == nullptr) {
            continue;
        }

        bool dead = false;

        if (re4vr::try_call<bool>(hp, "get_IsDead", dead) && dead) {
            continue;
        }

        // [PORTFIX 2026-09-06] typrichtig aus der TDB lesen -- s. knife_pick_target.
        const auto cur_o = re4vr::call_num(hp, "get_CurrentHitPoint");
        const float cur = cur_o.has_value() ? static_cast<float>(*cur_o) : 0.0f;

        if (!cur_o.has_value() || cur <= 0.0f) {
            continue;
        }

        ++npos;

        glm::vec3 c{};

        if (!enemy_aim_point(ctx, c)) {
            continue;
        }

        const float dx = c.x - from.x;
        const float dy = c.y - from.y;
        const float dz = c.z - from.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (d <= 0.2f) {
            continue;
        }

        if (d < nearest) {
            nearest = d;
        }

        if (d > max_dist) {
            continue;
        }

        // [KEGEL HORIZONTAL 2026-07-10] Dot NUR aus X/Z: ein NAHER Gegner sitzt
        // mit der Brust deutlich UNTER der Augenlinie -> der 3D-Dot kippte ihn
        // aus dem engen Kegel, obwohl man horizontal genau draufzielt.
        const float hlen = std::sqrt(dx * dx + dz * dz);
        const float chl = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        const float dot = (hlen > 0.001f && chl > 0.001f)
            ? ((dx * dir.x + dz * dir.z) / (hlen * chl))
            : -2.0f;

        if (dot > seen_best) {
            seen_best = dot;
        }

        // [ZIELWAHL 2026-08-12] Nicht "der NAECHSTE im Kegel", sondern "der, auf
        // den du am genauesten zeigst": Score = seitliche Ablage + kleiner
        // Distanzzuschlag (0.05/m), kleinster gewinnt. Beide Extreme sind
        // belegt falsch (nur Distanz / nur Winkel).
        const float lat = hlen * std::sqrt(std::max(0.0f, 1.0f - dot * dot));
        const float mlat = static_cast<float>(
            re4vr::lua_get_table_number("__re4_knife_fly_cfg", "assist_min_lat", 0.0));
        const float score = lat + d * 0.05f;

        // [NAHFANG 2026-08-12] Zugelassen ueber den Winkel ODER die seitliche
        // Ablage unter dem Mindest-Schlauch -- mit hartem Winkeldeckel von
        // 30 Grad (cos 30 = 0.866).
        if (!(dot > cone_cos || (dot > 0.866f && lat <= mlat))) {
            continue;
        }

        if (score >= best_score) {
            continue;
        }

        // [LOS 2026-07-11] Freie Sicht vom Auge zum Gegner? Ein Gegner im Stock
        // DRUEBER/DRUNTER ist durch Decke/Boden verdeckt -> raus.
        if (!los_clear(from, c)) {
            continue;
        }

        best_score = score;
        out = c;
        ctx_out = ctx;
        found = true;
    }

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "count=%d withpos=%d nearest=%.1f bestdot=%.2f need>%.2f",
                      count, npos, nearest, seen_best, cone_cos);
        re4vr::lua_set_string("__re4_assist_dbg", buf);
    }

    return found;
}

// [KAPUTT-MARKER 2026-07-06] Zuverlaessig ob eine WoodBox tot ist: get_IsBroken
// ist bei UNSEREN Breaks oft nil -> zusaetzlich get_Routine pruefen.
// Wait(0) = intakt/targetbar, alles andere = kaputt.
bool RE4VRWeapons::wb_dead(::REManagedObject* wb) {
    if (wb == nullptr) {
        return true;
    }

    bool broken = false;


    if (re4vr::try_call<bool>(wb, "get_IsBroken", broken) && broken) {
        return true;
    }

    const auto ri = enum_as_int(wb, "get_Routine");

    return ri.has_value() && *ri != 0;
}

// [ZIELHILFE BREAKABLES] Naechste zerschlagbare Kiste/Fass im Kegel um die
// Wurfrichtung. [HOMING = NUR WHITELIST 2026-07-09] Das Homing zieht
// AUSSCHLIESSLICH zu Objekten aus re4_vr_whitelist.lua -- das BRECHEN
// (break_nearby) ist davon UNBERUEHRT.
bool RE4VRWeapons::knife_assist_breakable(const glm::vec3& from, const glm::vec3& dir,
                                          float max_dist, float cone_deg, glm::vec3& out,
                                          float& near_out) {
    const float cone_cos = std::cos(cone_deg * glm::pi<float>() / 180.0f);

    bool have_best = false;
    float best_near = 1e9f;
    glm::vec3 best{};

    // [ANIMAL-PRIO] eigener Tier-Topf; Tiere schlagen Breakables.
    bool have_a = false;
    float best_a_near = 1e9f;
    glm::vec3 best_a{};

    iter_hitctrl_gos([&](::REManagedObject* go) {
        const std::string nm = obj_name_of(go);

        if (nm.size() >= 2) {
            const std::string p2 = nm.substr(0, 2);

            if (p2 == "ch" || p2 == "em" || p2 == "wp") {
                return;
            }
        }

        if (nm.rfind("ItemDis", 0) == 0) {
            return;   // Item-Pickup ist kein Breakable
        }

        ::REManagedObject* woodbox = nullptr;
        ::REManagedObject* dur = nullptr;
        ::REManagedObject* wb_go = nullptr;
        wb_of(go, woodbox, dur, wb_go);

        bool ok = false;

        if (lua_call_global<bool>("__re4_is_real_breakable_prop", go).value_or(false)) {
            ok = true;

            // [KAPUTT-FILTER 2026-07-09] Bereits zerbrochene NICHT anpeilen.
            if (woodbox != nullptr && wb_dead(woodbox)) {
                ok = false;
            }

            if (ok && dur != nullptr) {
                // [PORTFIX 2026-09-06] typrichtig lesen -- s. oben.
                const auto cur_d = re4vr::call_num(dur, "get_CurrentDurability");
                const float cur = cur_d.has_value() ? static_cast<float>(*cur_d) : 0.0f;

                if (!cur_d.has_value() || cur <= 0.0f) {
                    ok = false;
                }
            }
        }

        // [ANIMAL-HOMING 2026-07-10] Lebende Tiere zusaetzlich ueber den TYP.
        ::REManagedObject* is_animal = nullptr;

        if (!ok) {
            if (auto* animal = re4vr::get_component(go, "chainsaw.GmAnimal")) {
                bool dead = false;

                if (!re4vr::try_call<bool>(animal, "get_IsDead", dead) || !dead) {
                    ok = true;
                    is_animal = animal;
                }
            }
        }

        if (!ok) {
            return;
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        glm::vec3 pos{};

        if (tf == nullptr || !get_vec3(tf, "get_Position", pos)) {
            return;
        }

        // [MUENZE/ANIMAL/MAUS] Y-Zielpunkt-Versatz je Objektart.
        float yoff = 0.0f;

        if (is_animal != nullptr) {
            auto* td = utility::re_managed_object::get_type_definition(is_animal);
            const std::string tn = td != nullptr ? td->get_full_name() : std::string{};

            if (tn == "chainsaw.GmMouse") {
                yoff = static_cast<float>(re4vr::lua_get_number("__re4_mouse_off_y", -0.5));
            } else {
                yoff = static_cast<float>(re4vr::lua_get_number("__re4_animal_off_y", -0.3));
            }
        } else {
            yoff = static_cast<float>(
                lua_call_global<double>("__re4_breakable_yoff", go).value_or(0.5));
        }

        glm::vec3 c{pos.x, pos.y + yoff, pos.z};

        // [ANIMAL-CENTER 2026-07-16] Tiere: echter Mesh-Mittelpunkt. Die feste
        // Absenkung zielte INS Gelaender, auf dem der Rabe sitzt -> los_clear
        // failte -> Tier wurde nie gepickt.
        if (is_animal != nullptr) {
            glm::vec3 mn{};
            glm::vec3 mx{};

            if (auto* mesh = re4vr::get_component(go, "via.render.Mesh")) {
                if (mesh_world_aabb(mesh, mn, mx) && !(mn.x > mx.x || mn.y > mx.y
                                                       || mn.z > mx.z)) {
                    c = (mn + mx) * 0.5f;
                }
            }
        }

        const float dx = c.x - from.x;
        const float dy = c.y - from.y;
        const float dz = c.z - from.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (!(d > 0.2f && d <= max_dist)) {
            return;
        }

        // [KEGEL] Dot HORIZONTAL messen -- caxis MIT normalisieren, sonst
        // verkleinert die y-Komponente den Dot kuenstlich.
        const float hlen = std::sqrt(dx * dx + dz * dz);
        const float chl = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        const float dot = (hlen > 0.001f && chl > 0.001f)
            ? ((dx * dir.x + dz * dir.z) / (hlen * chl))
            : -2.0f;

        if (dot <= cone_cos) {
            return;
        }

        if (is_animal != nullptr) {
            // [ANIMAL-GATE 2026-08-11] Zwei Zusatzhuerden NUR fuer Tiere: der
            // horizontale Dot sieht die Hoehen-Ablage nicht, also zaehlte ein
            // Wurf METERWEIT DRUEBER als "genau draufgezielt".
            const float arange = static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_fly_cfg", "animal_range", 8.0));
            const float amaxdy = static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_fly_cfg", "animal_max_dy", 1.0));

            // Achse selbst normalisieren: dir kommt als HMD-Blick und ist nicht
            // garantiert 1 lang.
            const float alen = glm::length(dir);
            const float axis_y = alen > 1e-6f ? (from.y + (dir.y / alen) * d) : from.y;
            const float a_dy = std::abs(c.y - axis_y);

            if (d <= arange && a_dy <= amaxdy && d < best_a_near && los_clear(from, c)) {
                best_a_near = d;
                best_a = c;
                have_a = true;
            }

            return;
        }

        if (d < best_near && los_clear(from, c)) {
            best_near = d;
            best = c;
            have_best = true;
        }
    });

    // [ANIMAL-PRIO] Tier > Breakable: lebendes Tier im Kegel gewinnt, auch wenn
    // eine Kiste naeher ist.
    if (have_a) {
        out = best_a;
        near_out = best_a_near;

        return true;
    }

    if (have_best) {
        out = best;
        near_out = best_near;

        return true;
    }

    return false;
}

// ============================================================================
// Baustein 13 -- Daten-Caching aus nativen Treffern (Lua Z.1737-1791)
// ============================================================================

void RE4VRWeapons::cb_pool_damage(uintptr_t atk, uintptr_t dmg) {
    // poolDamage(CollisionInfo, AttackUserData[4], DamageUserData[5], bool)
    m_lp_atk = atk;
    m_lp_dmg = dmg;
}

void RE4VRWeapons::cb_attack_hit(::REManagedObject* hc) {
    const auto wid = weapon_id_num(hc);

    if (!wid.has_value() || !is_knife_id(*wid)) {
        return;
    }

    // Cache atk/dmg (nur EINMAL) fuer den bootstrap-freien requestAttack.
    if (m_knife_atk_ud.obj != nullptr) {
        return;
    }

    auto* atk = reinterpret_cast<::REManagedObject*>(m_lp_atk);
    auto* dmg = reinterpret_cast<::REManagedObject*>(m_lp_dmg);

    if (re4vr::obj_ok(atk) && re4vr::obj_ok(dmg)) {
        store(m_knife_atk_ud, atk, true);   // Lua: add_ref
        store(m_knife_dmg_ud, dmg, true);

        re4vr::lua_set_managed_object("__re4_knife_atkUD", atk);
        re4vr::lua_set_managed_object("__re4_knife_dmgUD", dmg);
    }
}

// [DMG-CALC] Feuert auf dem OPFER-HC bei jedem Treffer. NUR unsere
// requestAttack-Treffer korrigieren (Marker-Fenster) -- native RT unberuehrt.
void RE4VRWeapons::cb_calculate_damage(::REManagedObject* dv, ::REManagedObject* ci) {
    if (ci == nullptr || dv == nullptr) {
        return;
    }

    const auto wid = enum_as_int(ci, "get_WeaponID");

    if (!wid.has_value() || !is_knife_id(*wid)) {
        return;
    }

    if (re4vr::lua_get_number("__re4_knife_our_until", 0.0) <= clock_now()) {
        return;
    }

    if (const auto ov = re4vr::lua_get_number_opt("__re4_knife_dmg_override"); ov.has_value()) {
        re4vr::call_safe<void*>(dv, "set_Damage", static_cast<int32_t>(std::floor(*ov)));
    }

    if (const auto wv = re4vr::lua_get_number_opt("__re4_knife_wince_override");
        wv.has_value()) {
        re4vr::call_safe<void*>(dv, "set_Wince", static_cast<float>(*wv));
    }
}

void RE4VRWeapons::install_hitctrl_hooks() {
    auto* td = sdk::find_type_definition("chainsaw.HitController");

    if (td == nullptr) {
        return;
    }

    if (auto* mp = td->get_method("poolDamage")) {
        g_hookman.add(
            mp,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                try {
                    auto& self = RE4VRWeapons::get();

                    // Lua: args[4]/args[5] (1-basiert) -> hier args[3]/args[4].
                    if (self != nullptr && args.size() >= 5) {
                        self->cb_pool_damage(args[3], args[4]);
                    }
                } catch (...) {
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }

    if (auto* mc = td->get_method("callbackAttackHit")) {
        g_hookman.add(
            mc,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                try {
                    auto& self = RE4VRWeapons::get();

                    // this = Angreifer-HC = Luas args[2] -> hier args[1].
                    if (self != nullptr && args.size() >= 2) {
                        self->cb_attack_hit(reinterpret_cast<::REManagedObject*>(args[1]));
                    }
                } catch (...) {
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }

    if (auto* md = td->get_method("callbackCalculateDamage")) {
        g_hookman.add(
            md,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                try {
                    auto& self = RE4VRWeapons::get();

                    // Lua: args[3]=DamageValue, args[4]=CalculateInfo.
                    if (self != nullptr && args.size() >= 4) {
                        self->cb_calculate_damage(
                            reinterpret_cast<::REManagedObject*>(args[2]),
                            reinterpret_cast<::REManagedObject*>(args[3]));
                    }
                } catch (...) {
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }
}

// ============================================================================
// Baustein 17 -- do_knife_melee (Lua Z.2519-2556)
// ============================================================================

void RE4VRWeapons::do_knife_melee() {
    const double now = clock_now();

    if (now - m_last_knife_melee_t < 0.2) {
        return;
    }

    {
        auto& ks = RE4VRKillswitch::get();

        if (ks != nullptr && ks->is_active()) {
            return;
        }
    }

    m_last_knife_melee_t = now;

    // [BREAKABLES] Kisten/Faesser in Klingen-Reichweite zerbrechen (auch ohne
    // Gegner). ABSICHTLICH VOR der Stichrichtungs-Pruefung.
    const int32_t broke = break_nearby(std::nullopt, std::nullopt, std::nullopt);

    auto* hc = find_knife_hc();

    if (hc == nullptr) {
        return;
    }

    auto* target = knife_pick_target(knife_hand_world(), false, std::nullopt);
    auto* atk = knife_get_attack_ud(hc);

    if (target == nullptr || atk == nullptr) {
        return;
    }

    // [STICHRICHTUNG 2026-08-26] Bis hierher traf JEDER Schwung, auch das
    // Ausholen und das Zurueckziehen. Die Klinge muss zum Ziel zeigen UND die
    // Hand sich dorthin bewegen.
    if (!knife_dir_ok(target)) {
        return;
    }

    auto* dmg = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.DamageUserData", true);

    if (dmg == nullptr) {
        dmg = sdk::create_instance<::REManagedObject>("chainsaw.collision.DamageUserData");
    }

    if (dmg == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(hc, "set_AttackEnable", true);

    // Marker: die naechste Schadensberechnung gehoert UNS -> nur die korrigiert
    // der Damage-Hook. Native RT-Treffer bleiben unberuehrt.
    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);

    re4vr::call_safe<void*>(hc, "requestAttack", target, atk, dmg);


    // sofort wieder entschaerfen (nicht dauerhaft scharf)
    re4vr::call_safe<void*>(hc, "set_AttackEnable", false);
}

// ============================================================================
// Baustein 24 -- STICHRICHTUNG (Lua Z.5095-5188)
// ============================================================================

void RE4VRWeapons::knife_dir_tick() {
    const auto p = knife_hand_world();

    if (!p.has_value()) {
        return;
    }

    ++m_dir_ri;

    if (m_dir_ri > 5) {
        m_dir_ri = 0;
    }

    m_dir_ring[static_cast<size_t>(m_dir_ri)] = DirEntry{*p, clock_now()};
}

// Das Messer-Transform: im Koerperbaum haengen MEHRERE wpXXXX, genommen wird
// das der aktiven Messerhand naechste. Kein Cache: ein Transform-Handle
// ueberlebt keinen Savegame-Load.
::REManagedObject* RE4VRWeapons::knife_tf_near_hand(const glm::vec3& hp) {
    auto* btf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(btf)) {
        return nullptr;
    }

    auto* ch = re4vr::call_safe<::REManagedObject*>(btf, "get_Child");
    ::REManagedObject* best = nullptr;
    float bestd = 9e9f;

    while (ch != nullptr) {
        auto* go = re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject");
        const std::string nm = obj_name_of(go);

        if (nm.size() > 2 && nm.rfind("wp", 0) == 0
            && std::isdigit(static_cast<unsigned char>(nm[2]))) {
            glm::vec3 p2{};

            if (get_vec3(ch, "get_Position", p2)) {
                const float d = glm::length(p2 - hp);

                if (d < bestd) {
                    bestd = d;
                    best = ch;
                }
            }
        }

        ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
    }

    // Weiter als eine Armlaenge weg = nicht das Messer in der Hand.
    if (best != nullptr && bestd > 0.60f) {
        return nullptr;
    }

    return best;
}

// true = darf treffen. Im Zweifel IMMER true: lieber ein Treffer zu viel als
// ein verschluckter -- ein blockierter Stich waere nicht als solcher erkennbar.
bool RE4VRWeapons::knife_dir_ok(::REManagedObject* target) {
    if (re4vr::lua_get_tribool("__re4_knife_dir_check") == 0) {
        return true;
    }

    glm::vec3 tp{};

    if (target == nullptr || !get_vec3(target, "get_Position", tp)) {
        return true;
    }

    const auto hp_o = knife_hand_world();

    if (!hp_o.has_value()) {
        return true;
    }

    const glm::vec3 hp = *hp_o;

    // Zielrichtung ab der Hand
    const glm::vec3 t = tp - hp;
    const float tl = glm::length(t);

    if (tl < 0.001f) {
        return true;
    }

    // 1) Bewegt sich die Hand ueberhaupt auf das Ziel zu?
    //    Ein einzelner Frame ist zu verrauscht -> gegen die aelteste Position
    //    im Fenster 40-300 ms vergleichen (derselbe Weg wie im Choke).
    const double now = clock_now();
    const DirEntry* ref = nullptr;
    double oldest = -1.0;

    for (const auto& e : m_dir_ring) {
        if (!e.has_value()) {
            continue;
        }

        const double age = now - e->t;

        if (age >= 0.04 && age <= 0.30 && age > oldest) {
            oldest = age;
            ref = &*e;
        }
    }

    if (ref != nullptr) {
        const glm::vec3 m = hp - ref->p;
        const float ml = glm::length(m);

        if (ml >= 0.02f) {
            if ((glm::dot(m, t) / (ml * tl)) <= 0.0f) {
                return false;
            }
        }
    }

    // 2) Zeigt die KLINGE zum Ziel? get_AxisZ ist die Klingenachse.
    auto* ktf = knife_tf_near_hand(hp);

    if (ktf == nullptr) {
        return true;
    }

    glm::vec3 ax{};

    if (!get_vec3(ktf, "get_AxisZ", ax)) {
        return true;
    }

    const float al = glm::length(ax);

    if (al < 0.001f) {
        return true;
    }

    // [VORZEICHEN 2026-08-26, live belegt] get_AxisZ ist die GRIFF-Richtung,
    // die Spitze ist -AxisZ. Ohne das Minus liess der Test die Schwuenge durch,
    // bei denen der Griff zeigte.
    return (-glm::dot(ax, t) / (al * tl)) > KLINGE_MIN;
}

// ============================================================================
// Baustein 18 -- KNIFE_THROW / FLIGHT (Lua Z.2559-3886)
// ============================================================================

namespace {

glm::quat kfly_axis_angle(const glm::vec3& ax, float a) {
    const float h = a * 0.5f;
    const float s = std::sin(h);

    return glm::quat{std::cos(h), ax.x * s, ax.y * s, ax.z * s};
}

// [LANDE_POSE] Euler (rad) -> Quaternion.
glm::quat kfly_euler(float rx, float ry, float rz) {
    const float cx = std::cos(rx * 0.5f), sx = std::sin(rx * 0.5f);
    const float cy = std::cos(ry * 0.5f), sy = std::sin(ry * 0.5f);
    const float cz = std::cos(rz * 0.5f), sz = std::sin(rz * 0.5f);

    return glm::quat{cx * cy * cz + sx * sy * sz, sx * cy * cz - cx * sy * sz,
                     cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz};
}

float kfc(const char* key, double def) {
    return static_cast<float>(re4vr::lua_get_table_number("__re4_knife_fly_cfg", key, def));
}

}   // namespace

// Nur echte, lebende Gegner: kein Corpse, nicht eliminiert, HitPoint live.
bool RE4VRWeapons::is_live_enemy(::REManagedObject* ectx) {
    if (ectx == nullptr) {
        return false;
    }

    bool b = false;

    if (re4vr::try_call<bool>(ectx, "get_Valid", b) && !b) {
        return false;
    }

    if (re4vr::try_call<bool>(ectx, "get_IsEliminated", b) && b) {
        return false;
    }

    if (re4vr::try_call<bool>(ectx, "get_IsProcessedCharacterOnDead", b) && b) {
        return false;
    }

    auto* hp = re4vr::call_safe<::REManagedObject*>(ectx, "get_HitPoint");

    if (hp == nullptr) {
        return false;
    }

    if (!re4vr::try_call<bool>(hp, "get_IsLive", b) || !b) {
        return false;
    }

    float cur = 0.0f;

    // [PORTFIX 2026-09-06] typrichtig aus der TDB lesen -- s. knife_pick_target.
    const auto cur_o = re4vr::call_num(hp, "get_CurrentHitPoint");
    cur = cur_o.has_value() ? static_cast<float>(*cur_o) : 0.0f;

    return cur_o.has_value() && cur > 0.0f;
}

// Kleinster Abstand von kp zu irgendeinem Knochen unter root_tf.
// = "beruehrt das Messer wirklich den Koerper?".
// [GEMEINSAM 2026-08-14] Der LINKE Klon benutzt DIESE Funktion mit -- ein
// Knochen hat keinen Zielkonflikt (Arme, Kopf und Beine bringen eigene Joints
// mit), anders als Kugel/Zylinder/Kasten.
float RE4VRWeapons::nearest_bone_dist(::REManagedObject* root_tf, const glm::vec3& kp) {
    if (root_tf == nullptr) {
        return 999.0f;
    }

    float best = 1e9f;
    int32_t checked = 0;

    const std::function<void(::REManagedObject*, int32_t)> walk =
        [&](::REManagedObject* t, int32_t depth) {
            if (t == nullptr || depth > 4 || checked >= 120) {
                return;
            }

            ++checked;

            glm::vec3 pos{};

            if (get_vec3(t, "get_Position", pos)) {
                const glm::vec3 d = kp - pos;
                const float d2 = glm::dot(d, d);

                if (d2 < best) {
                    best = d2;
                }
            }

            auto* child = re4vr::call_safe<::REManagedObject*>(t, "get_Child");
            int32_t s = 0;

            while (child != nullptr && s < 32 && checked < 120) {
                walk(child, depth + 1);
                child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
                ++s;
            }
        };

    walk(root_tf, 0);

    return std::sqrt(best);
}

// Naechster LEBENDER Gegner, dessen KOERPER (Knochen) innerhalb reach liegt.
::REManagedObject* RE4VRWeapons::knife_flight_pick_enemy(const glm::vec3& kp, float reach) {
    auto* cm = re4vr::character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList")
        : nullptr;

    int32_t count = 0;

    if (list != nullptr) {
        re4vr::try_call<int32_t>(list, "get_Count", count);
    }

    const float pre = reach * 4.0f;   // Vorfilter: nur grob nahe Gegner scannen
    ::REManagedObject* best_go = nullptr;
    float best_d = reach;

    for (int32_t i = 0; i < count; ++i) {
        auto* ectx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (!is_live_enemy(ectx)) {
            continue;
        }

        glm::vec3 rpos{};

        if (!get_vec3(ectx, "get_Position", rpos)) {
            continue;
        }

        auto* go = re4vr::call_safe<::REManagedObject*>(ectx, "get_BodyGameObject");
        auto* tf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
                                 : nullptr;

        if (tf == nullptr) {
            continue;
        }

        const glm::vec3 dv = rpos - kp;
        const float cd = glm::length(dv);

        if (cd < m_kfly.min_enemy_d) {
            m_kfly.min_enemy_d = cd;
        }

        if (glm::dot(dv, dv) > pre * pre) {
            continue;
        }

        const float bd = nearest_bone_dist(tf, kp);

        if (bd < m_kfly.min_bone_d) {
            m_kfly.min_bone_d = bd;
        }

        if (bd <= best_d) {
            best_d = bd;
            best_go = go;
        }
    }

    return best_go;
}

// ---- Raycast ---------------------------------------------------------------
// STRIKT nach dem crosshair-Muster: EIN persistentes, gepinntes Result je
// Strahl. Es wird NUR ein neuer Strahl gefeuert, wenn der vorige get_Finished
// meldet -> KEINE ueberlappenden async-Strahlen. Genau das (jeden Frame neu
// feuern ohne Finished-Check) hatte vorher gecrasht.
bool RE4VRWeapons::knife_ray_fire(const glm::vec3& from, const glm::vec3& to) {
    static sdk::REMethodDefinition* s_kray = nullptr;
    static bool looked_up = false;

    if (!looked_up) {
        looked_up = true;

        // [SOFORT-AUSWERTUNG 2026-09-07] SYNCHRON casten und im selben Aufruf
        // lesen. Belegt: der Kontrollstrahl tat genau das und meldete 3
        // Kontakte am Gegner, waehrend der Wurf-Strahl -- gleiche Query,
        // gleicher Filter, gleiche Bauform, aber einen Frame spaeter gelesen --
        // in JEDEM Wurf 0 Kontakte hatte. Das Result ist einen Frame spaeter leer.
        auto* td = sdk::find_type_definition("via.physics.System");
        s_kray = td != nullptr
            ? td->get_method(
                  "castRay(via.physics.CastRayQuery, "
                  "via.physics.CastRayResult)")
            : nullptr;
    }

    if (s_kray == nullptr) {
        return false;
    }

    auto* sys = sdk::get_native_singleton("via.physics.System");

    if (sys == nullptr) {
        return false;
    }

    if (m_kfly.ray.obj == nullptr) {
        if (auto* r = sdk::create_instance<::REManagedObject>("via.physics.CastRayResult")) {
            store(m_kfly.ray, r, true);
        }
    }

    if (m_kfly.ray_wall.obj == nullptr) {
        if (auto* r = sdk::create_instance<::REManagedObject>("via.physics.CastRayResult")) {
            store(m_kfly.ray_wall, r, true);
        }
    }

    if (m_kfly.ray.obj == nullptr || m_kfly.ray_wall.obj == nullptr) {
        return false;
    }

    // [ZURUECK AUF DAS ORIGINAL 2026-09-06] Das Original erzeugt die Query PRO
    // AUFRUF frisch (weapons.lua Z.2701). Eine dauerhaft wiederverwendete Query
    // war meine eigene Zutat -- und der asynchrone Auftrag wurde damit nie
    // fertig (get_Finished blieb 0), wodurch der GESAMTE Flug-Block zublieb.
    // Gegen die Lebensdauer-Falle steht stattdessen dasselbe wie in Lua: eine
    // echte Referenz, die erst EINEN Aufruf spaeter faellt -- so lange braucht
    // die Engine, um den Auftrag abzuarbeiten.
    auto* q = sdk::create_instance<::REManagedObject>("via.physics.CastRayQuery");

    if (q == nullptr) {
        return false;
    }

    store(m_kfly.q_prev, q, true);

    cast_ray_set(q, from, to);
    re4vr::call_cmd(q, "clearOptions");
    re4vr::call_cmd(q, "enableAllHits");
    re4vr::call_cmd(q, "enableNearSort");

    // [PORTFIX 2026-09-06] Der Zahlen-Weg fliegt hier raus. `set_FilterInfo`
    // erwartet einen via.physics.FilterInfo-WERTTYP; in Lua baut REFramework
    // das Argument passend zum Parametertyp (build_args), nativ landet dagegen
    // eine nackte Zahl im Register -> die Query traegt Muell -> der ASYNCHRONE
    // Strahl greift beim Abarbeiten daneben: "Exception thrown in call to
    // castRayAsync" (Access Violation, im Framework-Log und in der Messung als
    // rayfail=ausnahme_strahl1 belegt). Strahl 2 wurde dadurch nie abgesetzt.
    //
    // Stattdessen der Weg, den das Original als Alternative selbst vorsieht
    // (weapons.lua Z.2707: `else` -> Group/MaskBits/Layer) und den der
    // Crosshair-Raycast seit jeher fehlerfrei benutzt. Layer 5 ist derselbe
    // Damage-Layer, den der Lua-Zweig dort setzt.
    // [PORTFIX 2026-09-06] DAS ist der Unterschied zu Gegnern.
    // Die Lua waehlt den Filter zweistufig (weapons.lua Z.2704-2710):
    //   if __re4_dmg_filter then set_FilterInfo(<DamageCheckOtherThanPlayer>)
    //   else                     get_FilterInfo + Group/MaskBits/Layer 5
    // Das Feld EXISTIERT (gemessen: via.physics.FilterInfo, ein Objekt), in Lua
    // laeuft also IMMER der erste Zweig und der Layer-5-Ersatz NIE. Beim Port
    // hatte ich nur den Ersatzzweig uebernommen -- der trifft Faesser und
    // Terrain, aber nicht die Gegner-Hurtboxen. Genau das Symptom: Tiere und
    // Faesser ja (die laufen ohnehin ueber den Radius-Scan), Gegner nie.
    // [FEHLER 2026-09-06] Hier stand `m_dmg_filter_done = true` VOR dem Lesen --
    // schlaegt es einmal fehl (die statische Tabelle des Typs existiert erst,
    // wenn die Klasse benutzt wurde), bleibt es die GANZE SITZUNG beim
    // Layer-5-Ersatz. Jetzt wird der Versuch erst als erledigt vermerkt, wenn
    // der Filter wirklich da ist.
    if (!m_dmg_filter_done) {
        auto* ftd = sdk::find_type_definition(game_namespace("CollisionUtil.Filter"));

        if (ftd == nullptr) {
            m_dmg_filter_how = "TYP_FEHLT";
        }

        if (ftd != nullptr) {
            auto* f = ftd->get_field("DamageCheckOtherThanPlayer");

            if (f == nullptr) {
                m_dmg_filter_how = "FELD_FEHLT";
            }

            if (f != nullptr) {
                // [MESSUNG 2026-09-06] Der Filter ist per Lua nachweislich der
                // richtige (re4_filtertest: 1-4 Kontakte am Gegner, Layer 5
                // null). Trotzdem blieb kontakt=0 -- also kommt er hier gar
                // nicht an. Beide Lesearten versuchen und festhalten, welche
                // greift, statt es noch einmal zu raten.
                try {
                    if (auto* fo = f->get_data<::REManagedObject*>(nullptr)) {
                        store(m_dmg_filter, fo, true);
                        m_dmg_filter_how = "get_data";
                    }
                } catch (...) {
                    m_dmg_filter_how = "get_data_warf";
                }

                if (m_dmg_filter.obj == nullptr) {
                    // Der Weg, den Luas field:get_data(nil) intern nimmt:
                    // get_data_raw + selbst dereferenzieren (kein Werttyp).
                    try {
                        if (auto* raw = f->get_data_raw(nullptr, false)) {
                            if (auto* fo = *reinterpret_cast<::REManagedObject**>(raw)) {
                                store(m_dmg_filter, fo, true);
                                m_dmg_filter_how = "get_data_raw";
                            }
                        }
                    } catch (...) {
                        m_dmg_filter_how = "raw_warf";
                    }
                }
            }
        }

        if (m_dmg_filter.obj != nullptr) {
            m_dmg_filter_done = true;   // erst jetzt nicht mehr versuchen
        }
    }

    if (m_dmg_filter.obj != nullptr) {
        std::array<void*, 1> fa{static_cast<void*>(m_dmg_filter.obj)};
        re4vr::call_cmd(q, "set_FilterInfo", std::span<void*>(fa));
    } else if (auto* fi = re4vr::call_safe<::REManagedObject*>(q, "get_FilterInfo")) {
        // Ersatzzweig -- nur, wenn das Feld fehlt (in Lua der else-Zweig).
        // [PORTFIX 2026-09-06] Setter ueber den Lua-Weg -- roh gerufen
        // uebergibt build_args die Zahl anders als die C-Konvention.
        std::array<void*, 1> g{re4vr::arg_int(0)};
        re4vr::call_cmd(fi, "set_Group", std::span<void*>(g));
        std::array<void*, 1> mb{re4vr::arg_int(0xFFFFFFFFu & ~1u)};
        re4vr::call_cmd(fi, "set_MaskBits", std::span<void*>(mb));
        std::array<void*, 1> ly{re4vr::arg_int(5)};
        re4vr::call_cmd(fi, "set_Layer", std::span<void*>(ly));
        std::array<void*, 1> fa{static_cast<void*>(fi)};
        re4vr::call_cmd(q, "set_FilterInfo", std::span<void*>(fa));
    }

    {
        auto context = sdk::get_thread_context();

        // [PORTFIX 2026-09-06] `invoke` statt `call_safe` -- der Lua-Weg.
        // Begruendung ausfuehrlich in `los_clear`.
        const auto ret = s_kray->invoke(sys, (void*)q, (void*)m_kfly.ray.obj);

        if (ret.exception_thrown) {
            return false;
        }

        clear_pending(context, true);
    }

    // ------------------------------------------------------------------

    // [TERRAIN] Strahl 2: Welt-Geometrie (Layer 10) -> Waende/Boden.
    auto* q2 = sdk::create_instance<::REManagedObject>("via.physics.CastRayQuery");

    if (q2 == nullptr) {
        return false;
    }

    store(m_kfly.q_wall_prev, q2, true);

    cast_ray_set(q2, from, to);
    re4vr::call_cmd(q2, "clearOptions");
    re4vr::call_cmd(q2, "enableAllHits");
    re4vr::call_cmd(q2, "enableNearSort");

    if (auto* fi2 = re4vr::call_safe<::REManagedObject*>(q2, "get_FilterInfo")) {
        // [PORTFIX 2026-09-06] Setter ueber den Lua-Weg -- roh gerufen
        // uebergibt build_args die Zahl anders als die C-Konvention.
        std::array<void*, 1> g{re4vr::arg_int(0)};
        re4vr::call_cmd(fi2, "set_Group", std::span<void*>(g));
        std::array<void*, 1> mb{re4vr::arg_int(0xFFFFFFFFu & ~1u)};
        re4vr::call_cmd(fi2, "set_MaskBits", std::span<void*>(mb));
        std::array<void*, 1> ly{re4vr::arg_int(10)};
        re4vr::call_cmd(fi2, "set_Layer", std::span<void*>(ly));
        std::array<void*, 1> fa{static_cast<void*>(fi2)};
        re4vr::call_cmd(q2, "set_FilterInfo", std::span<void*>(fa));
    }

    {
        auto context = sdk::get_thread_context();

        // [PORTFIX 2026-09-06] `invoke` statt `call_safe` -- der Lua-Weg.
        const auto ret = s_kray->invoke(sys, (void*)q2, (void*)m_kfly.ray_wall.obj);

        if (ret.exception_thrown) {
            return false;
        }

        clear_pending(context, true);
    }


    // [SOFORT-AUSWERTUNG 2026-09-07] Der Cast ist SYNCHRON -- das Ergebnis steht
    // JETZT im Result und ist einen Frame spaeter weg. Genau hier lag der
    // Unterschied zum Kontrollstrahl, der als einziger Strahl im Port je
    // Kontakte gemeldet hat (3 am Gegner): der castete und las sofort.
    // Der Wurf las erst im naechsten Frame -> in jedem Wurf kontakt=0.
    m_kfly.rr_have = false;
    m_kfly.rr_hd = 0.0f;
    m_kfly.rr_ny = 0.0f;
    m_kfly.rr_wall = false;
    store(m_kfly.rr_go, nullptr, false);

    {
        float hd = 0.0f, ny = 0.0f;
        ::REManagedObject* go = nullptr;
        bool wall = false;

        if (knife_ray_hit_dist(hd, ny, go, wall)) {
            m_kfly.rr_have = true;
            m_kfly.rr_hd = hd;
            m_kfly.rr_ny = ny;
            m_kfly.rr_wall = wall;

            if (go != nullptr) {
                store(m_kfly.rr_go, go, true);
            }
        }
    }

    return true;
}

bool RE4VRWeapons::knife_ray_finished() {
    if (m_kfly.ray.obj == nullptr || m_kfly.ray_wall.obj == nullptr) {
        return false;
    }

    // [PORTFIX 2026-09-06] typrichtig lesen (re4vr::call_num) statt blind als
    // bool. Meldet get_Finished nie true, bleibt der GESAMTE Flug-Block
    // dauerhaft zu -- er haengt an genau dieser Bedingung. Folge: kein
    // Kollisions-Strahl, keine Wurf-Sounds, kein Treffer beim Rechtswurf.
    const auto a = re4vr::call_num(m_kfly.ray.obj, "get_Finished");
    const auto b = re4vr::call_num(m_kfly.ray_wall.obj, "get_Finished");


    return a.value_or(0.0) != 0.0 && b.value_or(0.0) != 0.0;
}

namespace {

// Ein CastRayResult auswerten: Distanz + Normale-Y + getroffenes GameObject.
bool knife_read_ray(::REManagedObject* res, float& d_out, float& ny_out,
                    ::REManagedObject*& go_out) {
    go_out = nullptr;

    if (res == nullptr) {
        return false;
    }

    // [PORTFIX 2026-09-06] typrichtig lesen (re4vr::call_num) statt blind als
    // int32. Derselbe Objekttyp (via.physics.CastRayResult) und derselbe
    // Aufrufweg wie get_Finished 30 Zeilen weiter oben, das aus genau diesem
    // Grund schon umgestellt wurde. Blind gelesen kam hier IMMER 0 -> jeder
    // Strahl galt als kontaktlos -> BEIDE Strahlen (Damage UND Terrain)
    // meldeten nie einen Treffer, obwohl sie sauber gefeuert und fertig
    // wurden. In der Messung: rays=98 ausgewertet=97 kontakt=0 bei jedem der
    // 14 Fluege, auch der Terrain-Strahl, der ueber ~100 Frames zwangslaeufig
    // Boden oder Wand treffen muss. Lua kann das nicht passieren, weil dort
    // der TDB-Rueckgabetyp ueber die Auswertung entscheidet (weapons.lua
    // Z.2737: `res:call("get_NumContactPoints")`).
    const auto nc = re4vr::call_num(res, "get_NumContactPoints");

    if (!nc.has_value() || *nc <= 0.0) {
        return false;
    }

    // [DIE URSACHE, 2026-09-07] `via.physics.ContactPoint` ist ein VALUETYPE
    // (80 Byte, per TDB nachgesehen) -- die Engine schreibt ihn in einen
    // sret-Puffer, den der Aufrufer stellt. Der Port holte ihn als
    // Objektzeiger ab (`call_safe<REManagedObject*>`) und bekam damit Muell;
    // `field_float(cp, "Distance")` scheiterte, knife_read_ray meldete false --
    // bei JEDEM Strahl, obwohl get_NumContactPoints > 0 war. Genau das war das
    // Bild: Query, Filter, Geometrie und Cast in Ordnung, trotzdem kontakt=0
    // fuer den Damage- UND den Terrain-Strahl. In Lua kann das nicht passieren,
    // weil REFramework den Puffer beim ValueType-Rueckgabetyp selbst stellt
    // (weapons.lua Z.2740: `res:call("getContactPoint(System.UInt32)", 0)`).
    // Siehe [[reference_re4_cpp_valuetype_rueckgabe_sret]] -- dieselbe Falle
    // wie bei get_WorldAABB, hier nur ueber eine Methode mit Argument.
    auto* mcp = find_method(res, "getContactPoint(System.UInt32)");

    if (mcp == nullptr) {
        return false;
    }

    struct alignas(16) ContactPointBuf {
        uint8_t b[re4vr::contact_point::SIZE];
    };

    auto context = sdk::get_thread_context();
    ContactPointBuf cpb{};
    bool cp_ok = false;

    try {
        mcp->call_safe<ContactPointBuf*>(&cpb, context, res, static_cast<uint32_t>(0));
        cp_ok = true;
    } catch (...) {
        cp_ok = false;
    }

    cp_ok = clear_pending(context, cp_ok);

    if (!cp_ok) {
        return false;
    }

    // Feld-Offsets zentral in RE4VR.hpp (re4vr::contact_point) -- vorher
    // standen hier die base-Werte der TDB (0x34 / 0x20+4). 0x34 ist in
    // Wahrheit mUserDataPtr; dadurch kam nie eine Trefferdistanz an.
    d_out = *reinterpret_cast<const float*>(cpb.b + re4vr::contact_point::DISTANCE);
    ny_out = *reinterpret_cast<const float*>(cpb.b + re4vr::contact_point::NORMAL
                                             + sizeof(float));

    if (auto* col = re4vr::call_safe<::REManagedObject*>(res, "getContactCollidable", 0)) {
        go_out = re4vr::call_safe<::REManagedObject*>(col, "get_GameObject");
    }

    return true;
}

}   // namespace

// Beide Strahlen auswerten -> der NAEHERE Treffer gewinnt.
bool RE4VRWeapons::knife_ray_hit_dist(float& d_out, float& ny_out,
                                      ::REManagedObject*& go_out, bool& is_wall_out) {
    float dd = 0.0f, dny = 0.0f;
    ::REManagedObject* dgo = nullptr;
    const bool have_d = knife_read_ray(m_kfly.ray.obj, dd, dny, dgo);

    float wd = 0.0f, wny = 0.0f;
    ::REManagedObject* wgo = nullptr;
    const bool have_w = knife_read_ray(m_kfly.ray_wall.obj, wd, wny, wgo);

    if (have_d && (!have_w || dd <= wd)) {
        d_out = dd;
        ny_out = dny;
        go_out = dgo;
        is_wall_out = false;

        return true;
    }

    if (have_w) {
        d_out = wd;
        ny_out = wny;
        go_out = wgo;
        is_wall_out = true;

        return true;
    }

    return false;
}

// [MESH_HIDE] Sichtbarkeit des fliegenden Messer-GO schalten.
void RE4VRWeapons::knife_mesh_vis(bool vis) {
    if (m_kfly.go.obj == nullptr) {
        return;
    }

    // via.render.Mesh: set_Enabled (KEIN set_DrawSelf!)
    if (auto* m = re4vr::get_component(m_kfly.go.obj, "via.render.Mesh")) {
        re4vr::call_safe<void*>(m, "set_Enabled", vis);
    }
}

// ---- Grip der messerhaltenden Hand -----------------------------------------
// [1:1 zu re4_vr_weapons.lua Z.3945-3956] Links, wenn das Messer links ist oder
// ein Links-Klon liegt -- sonst rechts. Die Lua fing jeden Schritt in pcall;
// nativ koennen die Getter nur nullptr liefern, das faengt der Nullcheck ab.
bool RE4VRWeapons::knife_grip_held() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto act = vr->get_action_grip();

    if (!act) {
        return false;
    }

    const bool left = re4vr::lua_get_string("__re4_knife_hand") == "left"
        || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;

    return vr->is_action_active(act, left ? vr->get_left_joystick()
                                          : vr->get_right_joystick());
}

// ---- Wurf starten ----------------------------------------------------------
void RE4VRWeapons::knife_throw_launch(const glm::vec3& dir,
                                      const std::optional<float>& speed) {
    if (m_kfly.active) {
        return;
    }

    ::REManagedObject* go = nullptr;
    ::REManagedObject* tf = nullptr;

    // [LH_CLONE WURF] Im Klon-Modus fliegt das KLON-GO. Von L_Hand loesen.
    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
        go = re4vr::lua_get_pointer("__re4_knife_lh_clone_go");

        if (go == nullptr) {
            return;
        }

        tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

        if (tf == nullptr) {
            return;
        }

        re4vr::call_safe<void*>(tf, "set_Parent(via.Transform)", nullptr);
        m_kfly.clone_throw = true;
    } else {
        auto* hc = find_knife_hc();

        if (hc == nullptr) {
            return;
        }

        go = re4vr::call_safe<::REManagedObject*>(hc, "get_GameObject");

        if (go == nullptr) {
            return;
        }

        tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

        if (tf == nullptr) {
            return;
        }

        m_kfly.clone_throw = false;
    }

    // [SALTO_L] Links-Wurf merken -> eigene Salto-Achse im Flug.
    m_kfly.is_left =
        m_kfly.clone_throw || re4vr::lua_get_string("__re4_knife_hand") == "left";

    glm::vec3 p{};

    if (!get_vec3(tf, "get_Position", p)) {
        return;
    }

    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    const bool have_r = get_quat(tf, "get_Rotation", r);

    const float spd = speed.value_or(kfc("speed", 12.0));

    store(m_kfly.go, go);
    store(m_kfly.tf, tf);

    // [START_POS] Die Objekt-Root ist NICHT die sichtbare Klinge (das Mesh
    // haengt am Hand-Joint, die Root behaelt unseren letzten Flug-/Boden-Wert).
    // Deshalb IMMER von der echten Hand-Weltposition starten.
    if (const auto rh = knife_hand_world(); rh.has_value()) {
        m_kfly.pos = *rh;
    } else {
        m_kfly.pos = p;   // Fallback (nur wenn Hand-Pose fehlt)
    }

    m_kfly.start_pos = m_kfly.pos;   // [ARM_CLEAR] Wurf-Ursprung (Hand)
    m_kfly.vel = dir * spd;

    // Tumble-Achse = senkrecht zu Flugrichtung und Welt-Hoch.
    float axx = -dir.z;
    float axy = 0.0f;
    float axz = dir.x;
    float al = std::sqrt(axx * axx + axy * axy + axz * axz);

    if (al < 0.001f) {
        axx = 1.0f;
        axy = 0.0f;
        axz = 0.0f;
        al = 1.0f;
    }

    m_kfly.axis = glm::vec3{axx / al, axy / al, axz / al};

    // [SALTO] wilde Taumel-Achse fuer die Fall-Phase nach dem Treffer.
    float fx = axx / al + dir.x * 0.7f;
    float fy = axy / al + dir.y * 0.7f + 0.4f;
    float fz = axz / al + dir.z * 0.7f;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz);

    if (fl < 0.001f) {
        fx = 0.0f;
        fy = 1.0f;
        fz = 0.0f;
        fl = 1.0f;
    }

    m_kfly.axis_fall = glm::vec3{fx / fl, fy / fl, fz / fl};
    m_kfly.base_rot = have_r ? r : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};

    m_kfly.ang = 0.0f;
    m_kfly.active = true;
    m_kfly.phase = "fly";
    m_kfly.hit_done = false;
    m_kfly.hit_t.reset();

    m_kfly.flat = static_cast<float>(re4vr::lua_get_number("__re4_knife_assist_flat", 0.0));

    if (const auto h = re4vr::lua_get_vec3("__re4_knife_home"); h.has_value()) {
        m_kfly.home = *h;
    } else {
        m_kfly.home.reset();
    }

    store(m_kfly.home_ctx, re4vr::lua_get_pointer("__re4_knife_home_ctx"));
    m_kfly.home_str = static_cast<float>(re4vr::lua_get_number("__re4_knife_home_str", 0.0));

    m_kfly.ray_pending = false;
    m_kfly.wall_hit = false;
    m_kfly.min_enemy_d = 1e9f;
    m_kfly.min_bone_d = 1e9f;
    m_kfly.stuck = false;

    drop(m_kfly.stick_parent);
    m_kfly.stick_joint.clear();
    drop(m_kfly.stick_etf);
    m_kfly.stick_bone.clear();
    m_kfly.stick_ip.reset();
    m_kfly.check_at.reset();
    m_kfly.stick_ok.reset();
    m_kfly.stick_raw_d.reset();

    // [KLINGENLAENGE JETZT MESSEN] Das Messer liegt in diesem Moment noch ruhig
    // in der Hand -- die einzige Gelegenheit, an der die achsparallele WorldAABB
    // die Klinge eng umschliesst. Im FLUG (Salto) kam Unsinn heraus
    // (0.250/0.343/0.298/0.185 m fuer dasselbe Messer).
    m_kfly.blade_len.reset();

    // [WAISE 05.09.2026] Hier stand `lua_call_global<double>("__re4_blade_tip_len")`
    // -- diesen Namen gibt es nirgends. Exportiert wird `__re4_blade_tip`
    // (RE4VRChoke), und zwar als Tupel (tip, ax, ay, az, len); der Lua-Aufrufer
    // nahm den LETZTEN Rueckgabewert. Zweiter Fehler: lua_call_global<Ret>
    // liefert ohnehin nur den ERSTEN Wert -- selbst mit richtigem Namen waere
    // die Spitze statt der Laenge angekommen. Folge: m_kfly.blade_len blieb
    // immer leer und knife_stick_in_enemy rechnete fuer JEDES Messer mit der
    // festen stick_blade_len (0.22 m) -- die Klinge sass zu tief oder schwebte.
    // Nativ ueber denselben Rechner, den auch der Lua-Export benutzt.
    if (auto& ck = RE4VRChoke::get(); ck != nullptr && tf != nullptr) {
        glm::vec3 btip{};
        glm::vec3 baxis{};
        float blen = 0.0f;

        if (ck->blade_tip_public(tf, std::string{}, btip, baxis, blen) && blen > 0.05f) {
            m_kfly.blade_len = blen;
        }
    }

    // kein Treffer aus dem letzten Wurf mit in diesen schleppen
    re4vr::lua_set_nil("__re4_knife_last_hit");

    m_kfly.detached = false;
    m_kfly.hidden = false;
    knife_mesh_vis(true);
    drop(m_kfly.saved_parent);

    // [RUECKWEG 2026-08-26] Der EINZIGE verlaessliche Heimatparent ist der, den
    // das Messer JETZT hat -- vor Detach und vor dem Stecken im Gegner. Alles
    // spaeter Gemerkte kann schon der Gegner sein (genau das war der Fehler:
    // reattach hing das Messer an den Gegner, das GO war aus dem Player-Baum
    // verschwunden und JEDE Sound-Container-Suche lief ins Leere).
    store(m_kfly.home_parent, re4vr::call_safe<::REManagedObject*>(tf, "get_Parent"));

    m_kfly.home_joint = transform_parent_joint_name(tf);

    // Boden-Referenz. WICHTIG: floor_y NIE ueber der Wurf-Startposition --
    // sonst rastet das Messer sofort ein ("Messer fliegt nicht").
    auto* pctx = get_player_ctx_w();
    glm::vec3 ppos{};
    const bool have_p = pctx != nullptr && get_vec3(pctx, "get_Position", ppos);
    const float start_y = m_kfly.pos.y;
    const float foot = have_p ? ppos.y : (start_y - 1.5f);

    // [FLOOR] Nur ein tiefer NOTFALL-Boden. Der ECHTE Landeboden kommt vom
    // Vorwaerts-Raycast (is_floor), damit das Messer auch auf ABFALLENDEM
    // Gelaende auf der echten Geometrie landet.
    m_kfly.floor_y = foot - 3.0f;

    const double now = clock_now();
    m_kfly.last_t = now;
    m_kfly.min_fly_until = now + 0.15;   // [MIN_FLY] kein Sofort-rest
    m_kfly.end_t = now + kfc("max_time", 3.0);
    m_kfly.return_t = 0.0;
    m_kfly.snd_return = false;
}

// ---- Treffer markieren -----------------------------------------------------
void RE4VRWeapons::knife_flight_mark_hit(::REManagedObject* ego,
                                         const std::optional<glm::vec3>& ip) {
    m_kfly.hit_done = true;
    m_kfly.hit_t = clock_now();   // [SICHTBARKEIT] Startpunkt fuer hit_visible
    m_kfly.vel.x = 0.0f;
    m_kfly.vel.z = 0.0f;

    // [WURF STECKEN 2026-08-26] Nur bei einem GEGNER-Treffer -- Kisten,
    // Faesser, Waende und Tiere rufen dieselbe Funktion ohne Gegner auf.
    if (ego != nullptr || re4vr::lua_has_value("__re4_knife_last_hit")) {
        knife_stick_in_enemy(ego, ip);
    }

    if (m_kfly.phase != "rest") {
        m_kfly.phase = "rest";

        // [GLEICHZUG 2026-08-14] Beide Messer nehmen denselben Wert.
        m_kfly.return_t = clock_now() + kfc("hit_return_delay", 0.7);
    }
}

// ---- [WURF STECKEN] Das Messer bleibt im Gegner stecken --------------------
// [WURF-KLON 12.09.2026 -- Ansage "das Messer soll stecken bleiben"] Bisher
// steckte das ECHTE Messer im Gegner und musste deshalb wieder heraus. Eine
// reine Anzeige-Kopie loest beides: sie bleibt im Koerper, waehrend das
// Original (rechts das equippte, links der lefthand-Klon) ganz normal nach
// hit_return_delay in die Hand zurueckkehrt.
//
// Bauplan 1:1 aus RE4VRChoke::stick_clone_make (dort erprobt):
//   create_game_object -> SOFORT pinnen -> via.motion.Motion (ohne Skelett
//   bleibt das Mesh unsichtbar) -> via.render.Mesh -> setMesh + set_Material
//   vom Original. Die Kopie kann NICHTS ausser stecken: kein Schaden, kein
//   Sound, keine Kollision -- der Schaden laeuft weiter ueber requestAttack
//   am echten HitController.
constexpr double KSTICK_CLONE_MAX_S = 60.0;

bool RE4VRWeapons::knife_stick_clone_make(::REManagedObject* ktf, ::REManagedObject* etf,
                                          const std::string& joint, const glm::vec3& lp,
                                          const std::optional<glm::quat>& lr) {
    if (ktf == nullptr || etf == nullptr) {
        return false;
    }

    // [MEHRERE 12.09.2026] Freien Platz suchen; ist keiner frei, weicht der
    // AELTESTE. So bleiben bis zu KSTICK_MAX Messer gleichzeitig stecken, ohne
    // dass die Szene unbegrenzt volllaeuft.
    KStick* slot = nullptr;

    for (auto& s : m_kstick) {
        if (s.go.obj == nullptr) {
            slot = &s;

            break;
        }
    }

    if (slot == nullptr) {
        for (auto& s : m_kstick) {
            if (slot == nullptr || s.born < slot->born) {
                slot = &s;
            }
        }

        knife_stick_clone_drop_slot(*slot);
    }

    auto* kgo = re4vr::call_safe<::REManagedObject*>(ktf, "get_GameObject");

    if (kgo == nullptr) {
        return false;
    }

    // Quelle ist das FLIEGENDE Messer -- egal ob echtes rechtes oder der linke
    // Klon: gelesen werden nur Mesh und Material, das Original bleibt heil.
    auto* src_mesh = re4vr::get_component(kgo, "via.render.Mesh");

    if (src_mesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(src_mesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* src_mat = re4vr::call_safe<::REManagedObject*>(src_mesh, "get_Material");

    auto* go = reinterpret_cast<::REManagedObject*>(
        re4vr::create_game_object("vr_throw_stick_knife"));

    if (go == nullptr) {
        return false;
    }

    // Selbst erzeugt -> BEDINGUNGSLOS pinnen: ein frisches GameObject hat
    // referenceCount 0, die Heuristik in store() wuerde es NICHT halten.
    store(slot->go, go, true);

    if (auto* motion_rt = re4vr::runtime_type("via.motion.Motion")) {
        re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", motion_rt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* mesh_rt = re4vr::runtime_type("via.render.Mesh")) {
        mesh = re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", mesh_rt);
    }

    if (mesh == nullptr) {
        re4vr::destroy_game_object(go);
        drop(slot->go);

        return false;
    }

    store(slot->mesh, mesh);

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (src_mat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", src_mat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    re4vr::call_safe<void*>(mesh, "set_DrawShadowCast", false);

    // [KLON-DIM 2026-09-12 -- Ansage "bei allen Klonen gleich, sobald sie im
    // Gegner stecken"] Dieser Klon entsteht erst beim Einschlag, steckt also ab
    // seiner Geburt. Faktor und Schalter kommen aus der Choke-Config -- es ist
    // EIN Regler fuer alle Steckwege, kein zweiter daneben.
    if (auto ck = RE4VRChoke::get(); ck != nullptr && ck->knife_dim_on()) {
        RE4VRChoke::dim_mesh_once(mesh, ck->knife_dim_factor());
    }

    auto* ctf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (ctf == nullptr) {
        re4vr::destroy_game_object(go);
        drop(slot->mesh);
        drop(slot->go);

        return false;
    }

    store(slot->tf, ctf);

    // Ab hier exakt der Weg, den vorher das echte Messer ging: an den Gegner,
    // an den getroffenen Joint, und die LOKALE Pose setzen. Weltkoordinaten
    // wirken bei gesetztem ParentJoint nicht (teuer belegt am 24.08.).
    re4vr::call_safe<void*>(ctf, "set_Parent(via.Transform)", etf);
    transform_set_parent_joint(ctf, joint);
    set_vec3(ctf, "set_LocalPosition", lp);

    if (lr.has_value()) {
        set_quat(ctf, "set_LocalRotation", *lr);
    }

    // Obergrenze, damit nie etwas stehenbleibt, wenn der Gegner nie despawnt.
    slot->until = clock_now() + KSTICK_CLONE_MAX_S;
    slot->born = clock_now();

    return true;
}

void RE4VRWeapons::knife_stick_clone_drop_slot(KStick& s) {
    if (s.go.obj != nullptr) {
        re4vr::destroy_game_object(s.go.obj);
    }

    drop(s.tf);
    drop(s.mesh);
    drop(s.go);
    s.until = 0.0;
    s.born = 0.0;
}

// Alle Kopien weg -- Script-Reset, Levelwechsel.
void RE4VRWeapons::knife_stick_clone_drop() {
    for (auto& s : m_kstick) {
        knife_stick_clone_drop_slot(s);
    }
}

// Die Kopien leben laenger als der Flug -- ihr Ende haengt deshalb am Frame und
// nicht an der Rueckkehr des echten Messers. Jede prueft ihren EIGENEN Traeger:
// weg kommt sie, wenn ihre Zeit um ist oder der Gegner abgeraeumt wurde. Der Tod
// allein reicht nicht -- in der Leiche darf das Messer stecken bleiben.
void RE4VRWeapons::knife_stick_clone_tick() {
    for (auto& s : m_kstick) {
        if (s.go.obj == nullptr) {
            continue;
        }

        bool weg = clock_now() >= s.until;

        if (!weg && s.tf.obj != nullptr) {
            bool v = true;

            if (re4vr::try_call<bool>(s.tf.obj, "get_Valid", v) && !v) {
                weg = true;
            } else if (auto* par = re4vr::call_safe<::REManagedObject*>(s.tf.obj, "get_Parent");
                       par == nullptr || (re4vr::try_call<bool>(par, "get_Valid", v) && !v)) {
                weg = true;
            }
        }

        if (weg) {
            knife_stick_clone_drop_slot(s);
        }
    }
}

void RE4VRWeapons::knife_stick_in_enemy(::REManagedObject* ego_in,
                                        std::optional<glm::vec3> ip_in) {
    if (m_kfly.stuck || re4vr::lua_get_tribool("__re4_knife_stick_on") == 0) {
        return;
    }

    ::REManagedObject* ego = ego_in;
    std::optional<glm::vec3> ip = ip_in;

    // Der Klon-Weg meldet Treffer und Punkt nur ueber das Global. FRISCH muss
    // es sein: ein Treffer von vor Sekunden gehoert zu einem anderen Wurf.
    if (const auto lt = re4vr::lua_get_table_number("__re4_knife_last_hit", "t", -999.0);
        (clock_now() - lt) <= 0.5) {
        if (ego == nullptr) {
            ego = lua_table_pointer("__re4_knife_last_hit", "go");
        }

        if (!ip.has_value()) {
            ip = lua_table_vec3("__re4_knife_last_hit", "pos");
        }
    }

    if (!ip.has_value()) {
        ip = m_kfly.pos;
    }

    if (ego == nullptr || m_kfly.tf.obj == nullptr) {
        return;
    }

    // [CRASH-FIX] set_Parent auf einen STALEN Transform loest eine native
    // Access Violation aus, die pcall NICHT faengt. Beide Seiten pruefen.
    bool v = true;


    if (re4vr::try_call<bool>(m_kfly.tf.obj, "get_Valid", v) && !v) {
        return;
    }

    auto* etf = re4vr::call_safe<::REManagedObject*>(ego, "get_Transform");

    if (etf == nullptr) {
        return;
    }

    if (re4vr::try_call<bool>(etf, "get_Valid", v) && !v) {
        return;
    }

    // [GETROFFEN WIRD OFT EIN COLLIDER-KIND] Der Ray liefert das GO, das der
    // Strahl traf -- haeufig ein Collider ohne Skelett. Also nach oben, bis
    // eine Transform mit Joints da ist (hoechstens drei Stufen).
    //
    // [ARRAY-BINDING -- GEMESSEN 2026-08-26, der eigentliche Fehler]
    // get_size/get_element sind LUA-BINDINGS von REFramework auf das
    // Array-Objekt, KEINE managed Methoden -- ein :call() darauf liefert stumm
    // nil. Ergebnis im Log: `joints=0` bei JEDEM Gegner, obwohl jeder ein
    // volles Skelett hat.
    std::string jname{};
    std::optional<glm::vec3> jbone{};

    auto* arr = re4vr::call_safe<::REManagedObject*>(etf, "get_Joints");
    int32_t n = arr != nullptr ? re4vr::array_size(arr) : 0;

    for (int32_t i = 0; i < 3 && n <= 0; ++i) {
        auto* up = re4vr::call_safe<::REManagedObject*>(etf, "get_Parent");

        if (up == nullptr) {
            break;
        }

        bool uv = true;

        if (re4vr::try_call<bool>(up, "get_Valid", uv) && !uv) {
            break;
        }

        etf = up;
        arr = re4vr::call_safe<::REManagedObject*>(etf, "get_Joints");
        n = arr != nullptr ? re4vr::array_size(arr) : 0;
    }

    if (n > 0) {
        float bestd = 1e9f;

        for (int32_t i = 0; i < n; ++i) {
            auto* jt = re4vr::array_element(arr, i);

            if (jt == nullptr) {
                continue;
            }

            glm::vec3 jp{};

            if (!get_vec3(jt, "get_Position", jp)) {
                continue;
            }

            const glm::vec3 dd = jp - *ip;
            const float d = glm::dot(dd, dd);

            if (d < bestd) {
                bestd = d;
                jname = obj_name_of(jt);
                jbone = jp;
            }
        }
    }

    // [AN DEN KNOCHEN ZIEHEN 2026-08-26] Die beiden RADIUS-Trefferwege liefern
    // die FLUGPOSITION des Messers, nicht den Einschlagpunkt -- der Punkt kann
    // einen halben Meter vor dem Gegner liegen. Darum auf den naechsten
    // KNOCHEN ziehen, hoechstens `stick_snap` davon entfernt.
    // [0 HEISST AM KNOCHEN] Vorher war 0 = AUS -- genau der Wert, den man
    // einstellt, wenn man "so nah wie moeglich" will. Jetzt woertlich.
    const float snap = kfc("stick_snap", 0.10);

    if (jbone.has_value() && snap >= 0.0f) {
        const glm::vec3 dd = *ip - *jbone;
        const float d = glm::length(dd);
        m_kfly.stick_raw_d = d;

        if (d > snap && d > 0.0001f) {
            ip = *jbone + dd * (snap / d);
        }
    }

    // 2) Richtung, in die die Spitze zeigen soll: waagerecht auf die
    //    Koerperachse zu.
    glm::vec3 rp{};
    float ux = 0.0f, uy = 0.0f, uz = 0.0f;

    if (get_vec3(etf, "get_Position", rp)) {
        ux = rp.x - ip->x;
        uz = rp.z - ip->z;
        const float l = std::sqrt(ux * ux + uz * uz);

        if (l > 0.01f) {
            ux /= l;
            uz /= l;
        } else {
            ux = 0.0f;
            uz = 0.0f;
        }
    }

    if (ux == 0.0f && uz == 0.0f) {
        // Genau auf der Achse getroffen: dann die Flugrichtung nehmen.
        const float l = glm::length(m_kfly.vel);

        if (l > 0.01f) {
            ux = m_kfly.vel.x / l;
            uy = m_kfly.vel.y / l;
            uz = m_kfly.vel.z / l;
        } else {
            return;
        }
    }

    // 3) Klingenlaenge -- beim WURFSTART gemessen.
    float L = m_kfly.blade_len.value_or(0.0f);

    if (!(L > 0.05f)) {
        L = kfc("stick_blade_len", 0.22);
    }

    // 4) Rotation: die aktuelle minimal nachdrehen, bis +AxisZ (die Klinge) auf
    //    u zeigt. "Minimal" heisst: nur die Spitze wird ausgerichtet, die
    //    Drehung um die eigene Achse bleibt -- sonst saehe jeder Treffer
    //    gleich aus.
    glm::quat wr{1.0f, 0.0f, 0.0f, 0.0f};
    const bool have_wr = get_quat(m_kfly.tf.obj, "get_Rotation", wr);
    glm::vec3 ax{};

    if (have_wr && get_vec3(m_kfly.tf.obj, "get_AxisZ", ax)) {
        const float al = glm::length(ax);

        if (al > 0.001f) {
            const glm::vec3 axn = ax / al;
            const glm::vec3 u{ux, uy, uz};
            float dot = glm::clamp(glm::dot(axn, u), -1.0f, 1.0f);

            if (dot < 0.9995f) {
                glm::vec3 c = glm::cross(axn, u);
                float cl = glm::length(c);

                if (cl < 1e-6f) {
                    // genau entgegengesetzt -> irgendeine Senkrechte
                    c = glm::vec3{-axn.y, axn.x, 0.0f};
                    cl = glm::length(c);

                    if (cl < 1e-6f) {
                        c = glm::vec3{0.0f, -axn.z, axn.y};
                        cl = 1.0f;
                    }
                }

                const float a2 = std::acos(dot) * 0.5f;
                const float sh = std::sin(a2);
                const glm::quat dq{std::cos(a2), c.x / cl * sh, c.y / cl * sh,
                                   c.z / cl * sh};
                wr = glm::normalize(dq * wr);
            }
        }
    }

    // 5) Ursprung so setzen, dass die SPITZE im Einschlagpunkt sitzt (plus
    //    Einschubtiefe).
    const float inn = kfc("stick_in", 0.05);
    const float back = L - inn;
    const glm::vec3 wp{ip->x - ux * back, ip->y - uy * back, ip->z - uz * back};

    // [CRASH-FIX 2026-08-28] `etf` ist hier moeglicherweise gar nicht mehr
    // dasselbe Objekt (die Schleife oben steigt bis zu drei Stufen hoch).
    // Direkt vor dem Parenten noch einmal BEIDE Seiten pruefen.
    bool tv = true;
    bool ev = true;
    re4vr::try_call<bool>(m_kfly.tf.obj, "get_Valid", tv);
    re4vr::try_call<bool>(etf, "get_Valid", ev);

    if (!tv || !ev) {
        return;
    }

    // 6) LOKALE Pose am getroffenen Joint ausrechnen, nicht die Welt-Pose
    //    (teuer gelernt am 24.08.): bei gesetztem ParentJoint leitet die Engine
    //    die Weltlage jeden Frame aus Joint-Matrix x LocalPosition ab.
    auto* jref = !jname.empty() ? transform_joint_by_name(etf, jname) : nullptr;
    glm::vec3 jp{};
    glm::quat jr{};
    glm::vec3 lp = wp;
    std::optional<glm::quat> lr{};

    if (jref != nullptr && get_vec3(jref, "get_Position", jp)
        && get_quat(jref, "get_Rotation", jr)) {
        lp = glm::conjugate(jr) * (wp - jp);

        if (have_wr) {
            lr = glm::conjugate(jr) * wr;
        }
    } else if (have_wr) {
        // Kein Joint -> es haengt an der Wurzel; dort zaehlt die Welt-Pose.
        lr = wr;
    }

    // 7) [WURF-KLON 12.09.2026] Stecken bleibt die KOPIE. Das echte Messer wird
    //    dafuer nicht mehr angefasst -- es fliegt seinen Weg zu Ende, wird wie
    //    gehabt nach hit_visible ausgeblendet und kehrt nach hit_return_delay in
    //    die Hand zurueck. Deshalb wird m_kfly.stuck hier BEWUSST nicht mehr
    //    gesetzt: genau daran haengen Detach, Unstick und die Ruhephase.
    //    Scheitert der Klon (kein Mesh, kein GameObject), greift darunter
    //    unveraendert der ALTE Weg -- deshalb bleiben knife_unstick(), der
    //    Detach-Schutz und check_at in Betrieb statt toter Code zu werden.
    if (!knife_stick_clone_make(m_kfly.tf.obj, etf, jname, lp, lr)) {
        store(m_kfly.stick_parent,
              re4vr::call_safe<::REManagedObject*>(m_kfly.tf.obj, "get_Parent"));
        m_kfly.stick_joint = transform_parent_joint_name(m_kfly.tf.obj);

        // [ENTSCHEIDEND 2026-08-26] Ohne Joint-Namen trueg das Messer weiter
        // den HAND-Joint mit sich. Also: Joint setzen oder ausdruecklich LEEREN.
        re4vr::call_safe<void*>(m_kfly.tf.obj, "set_Parent(via.Transform)", etf);
        transform_set_parent_joint(m_kfly.tf.obj, jname);

        if (jref != nullptr) {
            set_vec3(m_kfly.tf.obj, "set_LocalPosition", lp);

            if (lr.has_value()) {
                set_quat(m_kfly.tf.obj, "set_LocalRotation", *lr);
            }
        } else {
            // Kein Joint -> es haengt an der Wurzel; dort zaehlt die Welt-Pose.
            set_vec3(m_kfly.tf.obj, "set_Position", wp);

            if (lr.has_value()) {
                set_quat(m_kfly.tf.obj, "set_Rotation", *lr);
            }
        }

        m_kfly.stuck = true;
        knife_mesh_vis(true);
        m_kfly.hidden = false;   // steckt = sichtbar, egal was vorher war

        store(m_kfly.stick_etf, etf);
        m_kfly.stick_bone = jname;
        m_kfly.stick_ip = ip;
        m_kfly.check_at = clock_now() + 0.20;

        // [GEGENGELESEN] Ob set_Parent wirklich gegriffen hat, sagt nur das
        // Zurueckfragen -- dieselbe Falle wie im Choke.
        auto* pnow = re4vr::call_safe<::REManagedObject*>(m_kfly.tf.obj, "get_Parent");
        m_kfly.stick_ok = pnow != nullptr && addr_of(pnow) == addr_of(etf);

        return;
    }

    m_kfly.stick_bone = jname;
    m_kfly.stick_ip = ip;
    m_kfly.stick_ok = true;
}

// Parent + Joint zurueck, bevor das Messer in die Hand schnappt.
void RE4VRWeapons::knife_unstick() {
    if (!m_kfly.stuck) {
        return;
    }

    m_kfly.stuck = false;

    auto* tf = m_kfly.tf.obj;
    auto* par = m_kfly.stick_parent.obj;
    const std::string jnt = m_kfly.stick_joint;

    drop(m_kfly.stick_parent);
    m_kfly.stick_joint.clear();

    if (tf == nullptr) {
        return;
    }

    bool v = true;

    if (re4vr::try_call<bool>(tf, "get_Valid", v) && !v) {
        return;
    }

    // [RUECKWEG 2026-08-26] Beim KLON gehoert das Messer an keinen Parent
    // (lefthand haengt es selbst wieder an L_Hand). Beim equippten Wurf an den
    // Heimatparent vom Wurfstart; stick_parent bleibt nur Rueckfall.
    if (m_kfly.clone_throw) {
        re4vr::call_safe<void*>(tf, "set_Parent(via.Transform)", nullptr);
        transform_set_parent_joint(tf, "");
    } else {
        auto* home = m_kfly.home_parent.obj;
        bool hv = true;

        if (home != nullptr && re4vr::try_call<bool>(home, "get_Valid", hv) && !hv) {
            home = nullptr;
        }

        if (home == nullptr) {
            home = par;
        }

        bool pv = true;

        if (home != nullptr && (!re4vr::try_call<bool>(home, "get_Valid", pv) || pv)) {
            re4vr::call_safe<void*>(tf, "set_Parent(via.Transform)", home);
            transform_set_parent_joint(
                tf, !m_kfly.home_joint.empty() ? m_kfly.home_joint : jnt);
        } else {
            re4vr::call_safe<void*>(tf, "set_Parent(via.Transform)", nullptr);
            transform_set_parent_joint(tf, jnt);
        }
    }

    set_vec3(tf, "set_LocalPosition", glm::vec3{0.0f, 0.0f, 0.0f});
}

// [ZITTER_FIX] Ab dem Einschlag den Parent LOESEN -> das Messer steht frei in
// Weltposition, keine Kopplung an Hand/Headset.
void RE4VRWeapons::knife_detach() {
    if (m_kfly.detached || m_kfly.tf.obj == nullptr) {
        return;
    }

    // [STECKT 2026-08-26] Steckt es im Gegner, ist der Parent bewusst der
    // GEGNER. Ein Detach wuerde es abreissen UND sich den Gegner als Rueckweg
    // merken. Beides falsch.
    if (m_kfly.stuck) {
        return;
    }

    bool v = true;

    if (re4vr::try_call<bool>(m_kfly.tf.obj, "get_Valid", v) && !v) {
        return;
    }

    store(m_kfly.saved_parent,
          re4vr::call_safe<::REManagedObject*>(m_kfly.tf.obj, "get_Parent"));
    re4vr::call_safe<void*>(m_kfly.tf.obj, "set_Parent(via.Transform)", nullptr);
    m_kfly.detached = true;
}

void RE4VRWeapons::knife_reattach() {
    if (!m_kfly.detached) {
        return;
    }

    // [RUECKWEG 2026-08-26] Zurueck geht es an den beim WURFSTART gemerkten
    // Heimatparent -- NICHT an das, was beim Detach zufaellig dranhing.
    auto* par = m_kfly.home_parent.obj;
    bool v = true;

    if (par != nullptr && re4vr::try_call<bool>(par, "get_Valid", v) && !v) {
        par = nullptr;
    }

    if (par == nullptr) {
        par = m_kfly.saved_parent.obj;
    }

    bool tv = true;
    bool pv = true;

    if (m_kfly.tf.obj != nullptr
        && (!re4vr::try_call<bool>(m_kfly.tf.obj, "get_Valid", tv) || tv) && par != nullptr
        && (!re4vr::try_call<bool>(par, "get_Valid", pv) || pv)) {
        re4vr::call_safe<void*>(m_kfly.tf.obj, "set_Parent(via.Transform)", par);
        transform_set_parent_joint(m_kfly.tf.obj, m_kfly.home_joint);
    }

    m_kfly.detached = false;
    drop(m_kfly.saved_parent);
}

// ---- Treffer im Flug -------------------------------------------------------
bool RE4VRWeapons::knife_try_hit_enemy(const glm::vec3& pos, float reach) {
    if (m_kfly.hit_done) {
        return false;
    }

    // [LH_CLONE WURF] Klon: kein aktiver Messer-HC -> direkter Schaden.
    // [TREFFERDISTANZ 2026-08-14] Vorher `math.max(reach, 1.5)`: der Klon-Wurf
    // buchte den Treffer, sobald das Messer 1,5 m von der KOERPERMITTE weg war
    // -- es blieb sichtbar einen halben Meter VOR dem Gegner stehen. Jetzt
    // regelt allein cfg.hit_radius.
    if (m_kfly.clone_throw) {
        if (lua_call_global<bool>("__re4_knife_direct_damage_at", pos, kfc("hit_radius", 0.5))
                .value_or(false)) {
            knife_flight_mark_hit(nullptr, std::nullopt);

            return true;
        }

        return false;
    }

    auto* hc = find_knife_hc();

    if (hc == nullptr) {
        return false;
    }

    auto* target = knife_flight_pick_enemy(pos, reach);

    if (target == nullptr) {
        return false;
    }

    auto* atk = knife_get_attack_ud(hc);

    if (atk == nullptr) {
        return false;
    }

    auto* dmg = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.DamageUserData", true);

    if (dmg == nullptr) {
        dmg = sdk::create_instance<::REManagedObject>("chainsaw.collision.DamageUserData");
    }

    if (dmg == nullptr) {
        return false;
    }

    re4vr::call_safe<void*>(hc, "set_AttackEnable", true);
    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
    re4vr::call_safe<void*>(hc, "requestAttack", target, atk, dmg);
    re4vr::call_safe<void*>(hc, "set_AttackEnable", false);

    // [WURF STECKEN] Gegner UND Trefferpunkt mitgeben -- `pos` ist die
    // Flugposition des Messers im Treffermoment.
    knife_flight_mark_hit(target, pos);

    return true;
}

// [FLUGBAHN-SCAN 2026-07-20] Breakables ENTLANG der Bahn einsammeln (Vasen/
// Kisten/Faesser sind fuer den Raycast unsichtbar). Der Gegner-Fang bleibt
// bewusst DRAUSSEN: er schnitt den Bogen ab, sobald ein Gegner NEBEN der Bahn
// stand.
void RE4VRWeapons::knife_flight_break_scan() {
    // [HOEHENFENSTER] nur im Flug und fuer ALLES gleich: ein Wurf ueber eine
    // Kiste hinweg darf sie nicht mehr zerlegen. 0 = wieder aus.
    const float mdy = kfc("hit_max_dy", 1.0);

    break_nearby(m_kfly.pos,
                 kfc("break_radius", re4vr::lua_get_number("__re4_knife_touch", 0.9)),
                 mdy > 0.0f ? std::optional<float>{mdy} : std::nullopt);
}

// [OBJEKT-KOLLISION] Das vom Raycast getroffene GameObject behandeln.
// "enemy" / "break" / "" (statische Geometrie -> Aufrufer entscheidet).
std::string RE4VRWeapons::knife_hit_object(::REManagedObject* hitgo,
                                           const std::optional<glm::vec3>& ip) {
    if (hitgo == nullptr) {
        return {};
    }

    ::REManagedObject* wbx = nullptr;
    ::REManagedObject* dur = nullptr;
    ::REManagedObject* holder = nullptr;
    wb_of(hitgo, wbx, dur, holder);

    // [RECHTS-WURF STUMM 2026-07-20] Im Flug steuern WIR die Transform, der
    // Messer-Collider haengt nicht mehr am Trefferpunkt -> es blieb still. Der
    // Hit-Sound laeuft daher bei JEDEM Wurf ueber den Flug-Weg. Doppelt kann er
    // nicht kommen: dieser Zweig feuert genau einmal pro Raycast-Treffer.
    if (wbx != nullptr || dur != nullptr) {
        play_knife_sound(static_cast<int32_t>(
            re4vr::lua_get_table_number("__re4_knife_snd", "hit", 686504397.0)));
    }

    if (wbx != nullptr) {
        bool broken = false;
        re4vr::try_call<bool>(wbx, "get_IsBroken", broken);

        if (!broken) {
            if (const auto br = get_break_routine(); br.has_value()) {
                re4vr::call_safe<void*>(wbx, "set_Routine", *br);
            }

            // [NATIVE-FLAG] echten Schaden nachschieben, sonst bleibt die Box
            // inkonsistent und der "zertreten"-Prompt haengt.
            auto* mhc = find_knife_hc();
            auto* atk = mhc != nullptr ? knife_get_attack_ud(mhc) : nullptr;

            if (mhc != nullptr && atk != nullptr) {
                auto* dmg = sdk::create_instance<::REManagedObject>(
                    "chainsaw.collision.DamageUserData", true);

                if (dmg == nullptr) {
                    dmg = sdk::create_instance<::REManagedObject>(
                        "chainsaw.collision.DamageUserData");
                }

                if (dmg != nullptr) {
                    re4vr::call_safe<void*>(mhc, "set_AttackEnable", true);
                    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
                    re4vr::call_safe<void*>(mhc, "requestAttack", hitgo, atk, dmg);
                    re4vr::call_safe<void*>(mhc, "set_AttackEnable", false);
                }
            }
        }

        return "break";
    }

    // [FENSTER/VASE] Durability-Breakable -> Haltbarkeit auf 0.
    if (dur != nullptr) {
        // [PORTFIX 2026-09-06] typrichtig lesen -- Durability ist der Wert,
        // ueber den ein Breakable ueberhaupt erst zerstoert wird.
        const auto cur_d = re4vr::call_num(dur, "get_CurrentDurability");
        const float cur = cur_d.has_value() ? static_cast<float>(*cur_d) : 0.0f;

        if (cur_d.has_value() && cur > 0.0f) {
            re4vr::call_safe<void*>(dur, "addDurability", -(cur + 1.0f));
        }

        knife_flight_mark_hit(nullptr, std::nullopt);   // [SOFORT-RUECKKEHR]

        return "break";
    }

    // [GEGNER] requestAttack DIREKT auf DAS getroffene GO -> praezise, KEIN
    // Radius. Genau das getroffene Objekt nimmt Schaden (wie ein Bullet).
    if (!m_kfly.hit_done) {
        if (m_kfly.clone_throw) {
            // [KLON-WURF 2026-08-14] Der Ray hatte den Gegner laengst -- nur der
            // Schaden fehlte: requestAttack ist beim Klon wirkungslos. Deshalb
            // hier der eigene Schadensweg, am ECHTEN Einschlagpunkt des Strahls.
            if (ip.has_value()
                && lua_call_global<bool>("__re4_knife_direct_damage_at", *ip,
                                         kfc("hit_radius", 0.5))
                       .value_or(false)) {
                // Gegner = Fleisch; im Flug ist der lh-Container stumm.
                play_knife_sound(static_cast<int32_t>(
                    re4vr::lua_get_table_number("__re4_knife_snd", "flesh", 238304172.0)));
                knife_flight_mark_hit(nullptr, std::nullopt);

                return "enemy";
            }

            // Kein Gegner am Einschlagpunkt: bewusst NICHT in den
            // requestAttack-Zweig fallen -- der wuerde beim Klon "enemy" melden
            // und das Messer stecken lassen, OHNE Schaden zu machen.
        } else {
            auto* mhc = find_knife_hc();
            auto* thc = mhc != nullptr ? get_hc(hitgo) : nullptr;
            auto* atk = thc != nullptr ? knife_get_attack_ud(mhc) : nullptr;

            if (atk != nullptr) {
                auto* dmg = sdk::create_instance<::REManagedObject>(
                    "chainsaw.collision.DamageUserData", true);

                if (dmg == nullptr) {
                    dmg = sdk::create_instance<::REManagedObject>(
                        "chainsaw.collision.DamageUserData");
                }

                if (dmg != nullptr) {
                    re4vr::call_safe<void*>(mhc, "set_AttackEnable", true);
                    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
                    re4vr::call_safe<void*>(mhc, "requestAttack", hitgo, atk, dmg);
                    re4vr::call_safe<void*>(mhc, "set_AttackEnable", false);

                    // `ip` ist der echte Einschlagpunkt des Strahls -- genauer
                    // geht es nicht.
                    knife_flight_mark_hit(hitgo, ip);

                    return "enemy";
                }
            }
        }
    }

    // [SAMMEL-COLLIDER] Der Damage-Strahl traf KEIN direktes Breakable/Gegner-GO
    // -> meist ein Container ("Before"/"After"/PropsCompound). Am Einschlagpunkt
    // suchen.
    if (ip.has_value()) {
        const int32_t broke = break_nearby(*ip, kfc("break_radius", 0.9), std::nullopt);

        if (broke > 0) {
            knife_flight_mark_hit(nullptr, std::nullopt);

            return "break";
        }
    }

    return {};
}

// ---- Physik pro Frame ------------------------------------------------------
void RE4VRWeapons::knife_flight_tick() {
    if (!m_kfly.active) {
        return;
    }


    const double now = clock_now();
    double dt = now - m_kfly.last_t;
    m_kfly.last_t = now;

    if (dt <= 0.0) {
        dt = 0.016;
    } else if (dt > 0.1) {
        dt = 0.1;
    }

    const float dtf = static_cast<float>(dt);

    if (m_kfly.phase != "rest") {
        m_kfly.pos += m_kfly.vel * dtf;

        // [LH_CLONE DAMAGE 2026-07-17] Der praezise Ray trifft beim KLON oft nur
        // einen Sub-Collider -> kein "enemy" -> kein Schaden. Deshalb hier der
        // bewaehrte Weg direct_damage_at.
        if (m_kfly.clone_throw && !m_kfly.hit_done && m_kfly.phase == "fly") {
            if (lua_call_global<bool>("__re4_knife_direct_damage_at", m_kfly.pos,
                                      kfc("hit_radius", 0.5))
                    .value_or(false)) {
                play_knife_sound(static_cast<int32_t>(
                    re4vr::lua_get_table_number("__re4_knife_snd", "flesh", 238304172.0)));
                knife_flight_mark_hit(nullptr, std::nullopt);
            }
        }

        if (m_kfly.phase == "fly" && !m_kfly.hit_done) {
            knife_flight_break_scan();
        }

        // [MAX_RANGE] Harte Wurfweiten-Grenze: horizontal weiter als max_range
        // -> Homing + Bogen-Ausgleich aus und horizontale Velocity gestoppt.
        if (m_kfly.phase == "fly" && !m_kfly.wall_hit) {
            const float rx = m_kfly.pos.x - m_kfly.start_pos.x;
            const float rz = m_kfly.pos.z - m_kfly.start_pos.z;
            const float mr = kfc("max_range", 12.0);

            if ((rx * rx + rz * rz) >= (mr * mr)) {
                m_kfly.flat = 0.0f;
                m_kfly.home_str = 0.0f;
                m_kfly.home.reset();
                m_kfly.vel.x = 0.0f;
                m_kfly.vel.z = 0.0f;
            }
        }

        // [HOMING] waehrend des Flugs die Velocity zum Ziel lenken.
        if (m_kfly.phase == "fly" && !m_kfly.wall_hit && m_kfly.home.has_value()
            && m_kfly.home_str > 0.0f) {
            // [LIVE TARGET] Ziel-Position jeden Frame nachfuehren.
            if (m_kfly.home_ctx.obj != nullptr) {
                glm::vec3 a{};

                if (enemy_aim_point(m_kfly.home_ctx.obj, a)) {
                    m_kfly.home = a;
                }
            }

            const glm::vec3 d = *m_kfly.home - m_kfly.pos;
            const float dl = glm::length(d);

            if (m_kfly.home_ctx.obj == nullptr && dl < 0.7f) {
                // [KEIN-ORBIT 2026-07-06] Breakable-Ziel ist ein FIXER Punkt.
                // Sobald das Messer ankommt, Homing AUS -> es fliegt gerade
                // durch, statt drueber zu kreisen.
                m_kfly.home.reset();
                m_kfly.home_str = 0.0f;
                m_kfly.flat = 0.0f;
            } else if (dl > 0.05f) {
                float spd = glm::length(m_kfly.vel);

                if (spd < 0.1f) {
                    spd = kfc("speed", 12.0);
                }

                const glm::vec3 w = d / dl * spd;
                const float b = std::min(1.0f, m_kfly.home_str);

                m_kfly.vel.x += (w.x - m_kfly.vel.x) * b;
                m_kfly.vel.z += (w.z - m_kfly.vel.z) * b;

                // [BOGEN] vertikale Lenkung deutlich schwaecher (35%): die
                // Gravity darf den Bogen bilden, das Homing zieht primaer
                // horizontal -> sichtbarer Wurfbogen statt gerader Linie.
                m_kfly.vel.y += (w.y - m_kfly.vel.y) * b * 0.35f;
            }
        }

        // [BOGEN] Gravity NUR noch vom Bogen-Ausgleich beeinflusst, NICHT mehr
        // vom Homing -- vorher kappte max(flat, home_str) die Gravity und
        // starkes Homing ergab eine Laserlinie ohne Bogen.
        const float gmul =
            (m_kfly.phase == "fly" && !m_kfly.wall_hit) ? (1.0f - m_kfly.flat) : 1.0f;
        m_kfly.vel.y -= kfc("gravity", 7.0) * gmul * dtf;

        // [SALTO] vor dem Treffer sauberer Ueberschlag, nach dem Einschlag
        // volle, wilde Rotation.
        const float sp = m_kfly.wall_hit ? kfc("spin_fall", 22.0) : kfc("spin_fly", 5.0);
        m_kfly.ang += sp * dtf;

        // [KNIFE_RAY] Kollision in FLUGRICHTUNG.
        if (m_kfly.ray.obj == nullptr || knife_ray_finished()) {

            if (m_kfly.ray_pending) {
                m_kfly.ray_pending = false;

                float hd = 0.0f, ny = 0.0f;
                ::REManagedObject* hitgo = nullptr;
                bool is_wall = false;


                // [SOFORT-AUSWERTUNG 2026-09-07] Nicht mehr das Result
                // lesen -- das ist einen Frame nach dem Cast leer. Genommen
                // wird, was knife_ray_fire beim Casten SOFORT gelesen hat.
                const bool ray_hit = m_kfly.rr_have;
                hd = m_kfly.rr_hd;
                ny = m_kfly.rr_ny;
                is_wall = m_kfly.rr_wall;
                hitgo = m_kfly.rr_go.obj;


                if (ray_hit) {

                    if (hd > m_kfly.ray_len) {
                    }
                }

                if (ray_hit && hd <= m_kfly.ray_len && !m_kfly.wall_hit) {
                    const glm::vec3 ipv = m_kfly.ray_from + m_kfly.ray_dir * hd;

                    const std::string kind =
                        !is_wall ? knife_hit_object(hitgo, ipv) : std::string{};


                    if (kind == "break") {
                        // Breakable zerstoert -> das Messer fliegt WEITER durch.
                        // [KEIN-ORBIT] ABER das Homing zeigte auf das jetzt
                        // zerstoerte Breakable -> es kreiste zurueck.
                        m_kfly.home.reset();
                        drop(m_kfly.home_ctx);
                        m_kfly.home_str = 0.0f;
                        m_kfly.flat = 0.0f;
                    } else if (kind == "enemy") {
                        // [ENEMY_HIT] sofort STOPPEN, nicht mehr rotieren.
                        m_kfly.wall_hit = true;
                        m_kfly.pos = ipv;
                        m_kfly.vel = glm::vec3{0.0f};
                        m_kfly.phase = "rest";

                        // [STECKZEIT 2026-08-14] Hier stand `return_t = jetzt +
                        // return_delay` und hat die Steckzeit aus mark_hit still
                        // ueberschrieben -- zwei Stellen fuer dieselbe Groesse.
                        // return_delay gilt weiterhin fuer den Fall OHNE Treffer.
                        play_knife_sound(static_cast<int32_t>(re4vr::lua_get_table_number(
                            "__re4_knife_snd", "flesh", 238304172.0)));
                    } else if (!is_wall) {
                        // [DMG-DURCHFLUG] Damage-Strahl traf KEIN Breakable/
                        // Gegner (Sammel-Collider) -> NICHT stoppen. Frueher
                        // stoppte hier JEDER Nicht-Ziel-Treffer den Flug direkt
                        // vor der Hand.
                    } else {
                        m_kfly.wall_hit = true;

                        // BODEN nur bei Terrain-Treffer mit horizontaler Flaeche.
                        const bool is_floor = std::abs(ny) > 0.6f;

                        if (is_floor) {
                            m_kfly.pos = ipv;
                            m_kfly.vel = glm::vec3{0.0f};
                            m_kfly.floor_y = ipv.y;
                            play_knife_sound(static_cast<int32_t>(
                                re4vr::lua_get_table_number("__re4_knife_snd", "floor",
                                                            643584649.0)));
                        } else {
                            // Wand/Objekt: knapp davor stoppen, dann faellt es.
                            const float back = std::max(0.0f, hd - 0.05f);
                            m_kfly.pos = m_kfly.ray_from + m_kfly.ray_dir * back;
                            m_kfly.vel.x = 0.0f;
                            m_kfly.vel.z = 0.0f;

                            if (m_kfly.vel.y > 0.0f) {
                                m_kfly.vel.y = 0.0f;
                            }

                            play_knife_sound(static_cast<int32_t>(
                                re4vr::lua_get_table_number("__re4_knife_snd", "hit",
                                                            686504397.0)));
                        }
                    }
                }
            }

            // [ARM_CLEAR] Erst, wenn das Messer den eigenen Koerper verlassen
            // hat -- sonst trifft der Strahl sofort den eigenen Arm.
            const glm::vec3 a = m_kfly.pos - m_kfly.start_pos;
            const float ac = kfc("arm_clear", 0.7);
            const bool cleared = glm::dot(a, a) >= ac * ac;

            // [TOTZONE-TREFFER 2026-08-12] Solange der Fuehler wegen arm_clear
            // aus ist, greift NUR hier und NUR auf Gegner der Radius-Weg. Der
            // Klon-Wurf bleibt aussen vor (eigener Radius-Schaden oben).
            if (!cleared && m_kfly.phase == "fly" && !m_kfly.wall_hit && !m_kfly.hit_done
                && !m_kfly.clone_throw) {
                const float cr = kfc("close_hit_radius", 0.5);

                if (cr > 0.0f) {
                    knife_try_hit_enemy(m_kfly.pos, cr);
                }
            }

            if (!cleared) {
            }

            if (m_kfly.phase == "rest") {
            }

            if (m_kfly.wall_hit) {
            }

            if (m_kfly.phase != "rest" && !m_kfly.wall_hit && cleared) {
                const float vlen = glm::length(m_kfly.vel);

                if (vlen <= 0.01f) {
                }

                if (vlen > 0.01f) {
                    const glm::vec3 d = m_kfly.vel / vlen;

                    // [RAY_LEN] Vorschau = knapp die Bewegung DIESES Frames
                    // (dt hart auf 0.03 gedeckelt) -> der Strahl trifft eine
                    // Wand erst, wenn das Messer WIRKLICH davor ist.
                    const float rl =
                        std::max(0.15f, vlen * std::min(dtf, 0.03f) * 1.5f);

                    m_kfly.ray_from = m_kfly.pos;
                    m_kfly.ray_dir = d;
                    m_kfly.ray_len = rl;

                    if (knife_ray_fire(m_kfly.ray_from, m_kfly.pos + d * rl)) {
                        m_kfly.ray_pending = true;
                    }
                }
            }
        }

        // Boden bzw. Not-Timer -> Messer landet.
        const float floor_y = m_kfly.floor_y;
        const bool flew_enough = now >= m_kfly.min_fly_until;

        if ((flew_enough && m_kfly.pos.y <= floor_y) || now >= m_kfly.end_t) {
            if (m_kfly.pos.y < floor_y) {
                m_kfly.pos.y = floor_y;
            }

            m_kfly.phase = "rest";
            m_kfly.return_t = now + kfc("return_delay", 0.4);
            play_knife_sound(static_cast<int32_t>(
                re4vr::lua_get_table_number("__re4_knife_snd", "floor", 643584649.0)));
        }

        return;
    }

    // ---- Ruhephase ----
    // [MESH_HIDE] JEDEN Frame forcieren: die Engine setzt Enabled sonst
    // periodisch selbst wieder auf sichtbar (Culling/LOD) -> das war das
    // Flackern.
    // [WURF STECKEN] Steckt es im Gegner, bleibt es die VOLLE Steckzeit
    // sichtbar -- genau das will man sehen.
    if (!m_kfly.hit_done) {
        m_kfly.hidden = true;
        knife_mesh_vis(false);
    } else if (!m_kfly.stuck && m_kfly.hit_t.has_value()
               && now >= (*m_kfly.hit_t + kfc("hit_visible", 0.15))) {
        m_kfly.hidden = true;
        knife_mesh_vis(false);
    }

    // Die [NACHMESSUNG] 0.2 s nach dem Einschlag entfaellt: sie war reine
    // Diagnose ueber log.info, und `log` ist in dieser Datei stumm ueberdeckt.
    if (m_kfly.stuck && m_kfly.check_at.has_value() && now >= *m_kfly.check_at) {
        m_kfly.check_at.reset();
    }

    if (now >= m_kfly.return_t) {
        // Erst aus dem Gegner loesen, dann zurueck in die Hand.
        if (m_kfly.stuck) {
            knife_unstick();
        }

        m_kfly.active = false;   // Override AUS -> Engine snappt es zurueck
        knife_mesh_vis(true);
        m_kfly.hidden = false;

        // [RUECKKEHR-SOUND] Nur VORMERKEN -- gespielt wird in
        // knife_flight_apply, NACH dem Reattach. Hier haengt das Messer noch
        // frei an der Boden-Position, der Sound kaeme vom Boden.
        m_kfly.snd_return = true;
    }
}

// ---- Transform-Uebernahme im spaeten Pass ----------------------------------
void RE4VRWeapons::knife_flight_apply() {
    // [KNIFE_THROW] Flag fuer motion: Waffen-Pin aussetzen solange es fliegt.
    re4vr::lua_set_bool("__re4_knife_flying", m_kfly.active);

    if (!m_kfly.active) {
        // [LH_CLONE WURF] Nur der equipped-Wurf reattacht hier -- den Klon
        // re-parentet lefthand selbst an L_Hand.
        if (m_kfly.detached && !m_kfly.clone_throw) {
            knife_reattach();
        }

        // [RUECKKEHR-SOUND] Erst nach dem Reattach sitzt das Messer-GO wieder
        // an der Hand. Flanke ueber snd_return: dieser Zweig laeuft jeden
        // Frame, ohne das Flag waere es Dauerfeuer.
        if (m_kfly.snd_return) {
            m_kfly.snd_return = false;
            lua_call_global_void("__re4_knife_play_grab_sound");
        }

        return;
    }

    if (m_kfly.tf.obj == nullptr) {
        return;
    }

    // [CRASH-HAERTUNG 2026-07-22] Ein pcall faengt eine native AV NICHT ab --
    // deshalb VOR jedem Transform-Write pruefen, ob das Objekt noch lebt. NUR
    // bei explizit false abbrechen.
    bool valid = true;

    if (re4vr::try_call<bool>(m_kfly.tf.obj, "get_Valid", valid) && !valid) {
        m_kfly.active = false;

        return;
    }

    // [WURF STECKEN] Steckt es im Gegner, ist die Flugmaschine hier fertig: es
    // haengt an seinem Joint und folgt ihm von selbst. Weder loesen noch eine
    // Weltpose schreiben -- letzteres wuerde bei gesetztem ParentJoint ohnehin
    // nichts bewirken.
    if (m_kfly.stuck) {
        return;
    }

    // Ab dem Einschlag bzw. sobald es liegt: vom Parent loesen -> kein Zittern.
    if ((m_kfly.wall_hit || m_kfly.phase == "rest") && !m_kfly.detached
        && !m_kfly.clone_throw) {
        knife_detach();
    }

    if (!set_vec3(m_kfly.tf.obj, "set_Position", m_kfly.pos)) {
        m_kfly.active = false;   // Transform ungueltig (Waffe weg)

        return;
    }

    glm::quat newrot{1.0f, 0.0f, 0.0f, 0.0f};

    if (m_kfly.phase == "rest") {
        // [LANDE_POSE] Feste, tunebare Welt-Rotation statt der eingefrorenen
        // Salto-Rotation -> das Messer liegt sauber.
        newrot = kfly_euler(
            static_cast<float>(re4vr::lua_get_table_number("__re4_knife_land_rot", "rx",
                                                           1.5708)),
            static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_land_rot", "ry", 0.0)),
            static_cast<float>(
                re4vr::lua_get_table_number("__re4_knife_land_rot", "rz", 0.0)));
    } else if (m_kfly.wall_hit) {
        // NACH dem Einschlag: wilde, verkippte Welt-Achse -> Segeln.
        const glm::quat q = kfly_axis_angle(m_kfly.axis_fall, m_kfly.ang);
        newrot = glm::normalize(q * m_kfly.base_rot);
    } else {
        // [SALTO IM FLUG] Sauberer Ueberschlag um die LOKALE Achse.
        // [SALTO_L] Links-Wurf nutzt AUSSCHLIESSLICH die eigenen Links-Achsen.
        float ax2, ay2, az2;

        if (m_kfly.is_left) {
            ax2 = kfc("spin_ax_l", 1.0);
            ay2 = kfc("spin_ay_l", 0.0);
            az2 = kfc("spin_az_l", 0.0);
        } else {
            ax2 = kfc("spin_ax", 1.0);
            ay2 = kfc("spin_ay", 0.0);
            az2 = kfc("spin_az", 0.0);
        }

        float al2 = std::sqrt(ax2 * ax2 + ay2 * ay2 + az2 * az2);

        if (al2 < 0.001f) {
            ax2 = 1.0f;
            ay2 = 0.0f;
            az2 = 0.0f;
            al2 = 1.0f;
        }

        const glm::quat salto =
            kfly_axis_angle(glm::vec3{ax2 / al2, ay2 / al2, az2 / al2}, m_kfly.ang);
        newrot = glm::normalize(m_kfly.base_rot * salto);
    }

    set_quat(m_kfly.tf.obj, "set_Rotation", newrot);
}

// ============================================================================
// Baustein 19 -- THROWABLES / Granaten (Lua Z.3888-4507)
// ============================================================================

void RE4VRWeapons::save_throw_cfg() {
    nlohmann::json d{};
    d["enabled"] = m_tcfg.enabled;
    d["hand_speed_min"] = m_tcfg.hand_speed_min;
    d["hand_speed_max"] = m_tcfg.hand_speed_max;
    d["throw_fixed_speed"] = m_tcfg.throw_fixed_speed;
    d["throw_speed_min"] = m_tcfg.throw_speed_min;
    d["throw_speed_max"] = m_tcfg.throw_speed_max;
    d["cooldown"] = m_tcfg.cooldown;
    d["forward_dot_min"] = m_tcfg.forward_dot_min;
    d["sensitivity_min"] = m_tcfg.sensitivity_min;
    d["sensitivity_max"] = m_tcfg.sensitivity_max;
    d["y_offset"] = m_tcfg.y_offset;
    d["x_offset"] = m_tcfg.x_offset;
    d["release_window"] = m_tcfg.release_window;
    d["knife_pitch"] = m_tcfg.knife_pitch;
    d["grenade_pitch"] = m_tcfg.grenade_pitch;
    d["knife_yaw"] = m_tcfg.knife_yaw;
    d["throw_pitch"] = m_tcfg.throw_pitch;
    d["gravity"] = m_tcfg.gravity;
    d["throw_speed_mult"] = m_tcfg.throw_speed_mult;

    re4vr::json_save(THROW_CFG_PATH, d);
}

bool RE4VRWeapons::is_grenade_equipped() {
    const auto id = get_equip_weapon_id();

    return id.has_value() && *id >= GRENADE_ID_MIN && *id <= GRENADE_ID_MAX;
}

bool RE4VRWeapons::is_knife_equipped() {
    const auto id = get_equip_weapon_id();

    return id.has_value() && is_knife_id(*id);
}

std::optional<glm::vec3> RE4VRWeapons::get_player_world_pos() {
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();
    glm::vec3 p{};

    return (tf != nullptr && get_vec3(tf, "get_Position", p)) ? std::optional<glm::vec3>{p}
                                                              : std::nullopt;
}

std::optional<glm::vec3> RE4VRWeapons::get_char_forward() {
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();
    glm::quat rot{};

    if (tf == nullptr || !get_quat(tf, "get_Rotation", rot)) {
        return std::nullopt;
    }

    return rot * glm::vec3{0.0f, 0.0f, 1.0f};
}

// [GRANATE MISST RECHTS 2026-08-30 -- GEMESSEN] `force_right` sagt, welche Hand
// gemessen wird. Vorher stand hier fest knife_hand_world() -- und die liefert
// bei aktivem Links-Klon die LINKE Hand. Der Granatenwurf hat also die ruhende
// linke Hand gemessen, solange das Klon-Messer draussen war.
// Die Historie teilen sich beide Wuerfe -> beim Quellwechsel LEEREN, sonst
// liegt ein Sprung von einer Armlaenge in einem Frame darin (Phantom-Peak).
void RE4VRWeapons::update_throw_velocity(bool force_right) {
    const auto pos_o = force_right ? re4vr::lua_get_vec3("__vr_rh_world") : knife_hand_world();

    const std::string src = force_right ? "R" : "L?";

    if (m_tstate.src != src) {
        m_tstate.src = src;
        m_tstate = ThrowState{};
        m_tstate.src = src;
    }

    if (!pos_o.has_value()) {
        const std::string keep = m_tstate.src;
        m_tstate = ThrowState{};
        m_tstate.src = keep;

        return;
    }

    const glm::vec3 pos = *pos_o;
    const auto player_pos = get_player_world_pos();
    const double now = clock_now();

    m_tstate.pos_idx = (m_tstate.pos_idx % POS_HISTORY_SIZE) + 1;
    const size_t idx = static_cast<size_t>(m_tstate.pos_idx - 1);

    m_tstate.pos_history[idx] = pos;
    m_tstate.player_history[idx] = player_pos;
    m_tstate.pos_times[idx] = now;

    int32_t lookback = m_tstate.pos_idx - VELOCITY_SMOOTH_FRAMES;

    while (lookback <= 0) {
        lookback += POS_HISTORY_SIZE;
    }

    const size_t lb = static_cast<size_t>(lookback - 1);

    if (m_tstate.pos_history[lb].has_value() && (now - m_tstate.pos_times[lb]) > 0.001) {
        glm::vec3 d = pos - *m_tstate.pos_history[lb];

        // Spielerbewegung herausrechnen
        if (m_tstate.player_history[lb].has_value() && player_pos.has_value()) {
            d -= (*player_pos - *m_tstate.player_history[lb]);
        }

        m_tstate.velocity =
            glm::length(d) / static_cast<float>(now - m_tstate.pos_times[lb]);
    } else if (m_tstate.prev_pos.has_value() && (now - m_tstate.prev_time) > 0.001) {
        const glm::vec3 d = pos - *m_tstate.prev_pos;
        m_tstate.velocity = glm::length(d) / static_cast<float>(now - m_tstate.prev_time);
    }

    m_tstate.velocity_history[idx] = m_tstate.velocity;
    m_tstate.prev_pos = pos;
    m_tstate.prev_time = now;
}

float RE4VRWeapons::compute_release_window_peak() {
    if (m_tstate.pos_idx == 0) {
        return 0.0f;
    }

    const double now_time = m_tstate.pos_times[static_cast<size_t>(m_tstate.pos_idx - 1)];

    if (now_time <= 0.0) {
        return 0.0f;
    }

    const double cutoff = now_time - RELEASE_PEAK_WINDOW_SEC;
    float peak = 0.0f;

    for (int32_t i = 0; i < POS_HISTORY_SIZE; ++i) {
        int32_t idx = m_tstate.pos_idx - i;

        while (idx <= 0) {
            idx += POS_HISTORY_SIZE;
        }

        const size_t k = static_cast<size_t>(idx - 1);
        const double t = m_tstate.pos_times[k];

        if (t <= 0.0 || t < cutoff) {
            break;
        }

        if (m_tstate.velocity_history[k] > peak) {
            peak = m_tstate.velocity_history[k];
        }
    }

    return peak;
}

bool RE4VRWeapons::get_throw_direction(float extra_pitch, float extra_yaw, glm::vec3& out) {
    glm::vec3 d{0.0f};
    bool have_motion = false;

    {
        // Aeltester Eintrag im Ringpuffer.
        int32_t oldest_idx = (m_tstate.pos_idx % POS_HISTORY_SIZE) + 1;

        if (!m_tstate.pos_history[static_cast<size_t>(oldest_idx - 1)].has_value()) {
            oldest_idx = 1;
        }

        const size_t o = static_cast<size_t>(oldest_idx - 1);
        const size_t n = static_cast<size_t>(std::max(m_tstate.pos_idx, 1) - 1);

        if (m_tstate.pos_history[o].has_value() && m_tstate.pos_history[n].has_value()) {
            d = *m_tstate.pos_history[n] - *m_tstate.pos_history[o];

            if (m_tstate.player_history[o].has_value()
                && m_tstate.player_history[n].has_value()) {
                d -= (*m_tstate.player_history[n] - *m_tstate.player_history[o]);
            }

            const float total = glm::length(d);

            if (total > 0.001f) {
                d /= total;
                have_motion = true;
            }
        }
    }

    if (!have_motion) {
        const auto fwd = get_char_forward();

        if (!fwd.has_value()) {
            return false;
        }

        const float h = std::sqrt(fwd->x * fwd->x + fwd->z * fwd->z);

        if (h < 0.001f) {
            return false;
        }

        d = glm::vec3{fwd->x / h, 0.0f, fwd->z / h};
    }

    // [X_KORREKTUR] globaler x_offset (Yaw) + wurf-spezifischer Extra-Yaw.
    const float yaw = m_tcfg.x_offset + extra_yaw;

    if (yaw != 0.0f) {
        const float ca = std::cos(yaw);
        const float sa = std::sin(yaw);
        const float rx = d.x * ca - d.z * sa;
        const float rz = d.x * sa + d.z * ca;
        d.x = rx;
        d.z = rz;
    }

    // Gemeinsamer y_offset + wurf-spezifischer Extra-Pitch.
    // Positiv = Wurfrichtung nach OBEN (gegen den Gravitations-Bogen).
    const float pitch = m_tcfg.y_offset + extra_pitch;

    if (pitch != 0.0f) {
        d.y += pitch;
        const float len = glm::length(d);

        if (len > 0.001f) {
            d /= len;
        }
    }

    out = d;

    return true;
}

// [GEN_SAVELOAD 2026-08-06] Nach einem Save-Load zeigen Generator UND Szene auf
// tote Objekte. Der vorhandene Gueltigkeits-Check ist blind: get_field auf einem
// toten managed Zeiger wirft nicht, es kommt nur nil -> der tote Generator wird
// weiterbenutzt und requestFire laeuft ins Leere.
// Erkennung wie im Messer-Pfad: Adresse des Player-Body-GO.
void RE4VRWeapons::grenade_cache_guard() {
    const double now = clock_now();

    if (now < m_gen_next_check) {
        return;
    }

    m_gen_next_check = now + 0.25;

    auto* body = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
    const auto a = addr_of(body);

    if (!a.has_value()) {
        return;   // nicht lesbar (Ladephase) -> nichts verwerfen, nichts merken
    }

    if (!m_gen_body_addr.has_value()) {
        m_gen_body_addr = a;

        return;
    }

    if (*a == *m_gen_body_addr) {
        return;
    }

    m_gen_body_addr = a;

    // Kein release: nach dem Load kann der Generator tot sein, ein release
    // waere genau der Deref, den wir vermeiden. Loslassen genuegt.
    m_grenade_gen.obj = nullptr;
    m_grenade_gen.reffed = false;
    m_scene_cache.obj = nullptr;
    m_scene_cache.reffed = false;

    re4vr::lua_set_number("__re4_gren_cache_dropped_t", now);
}

::REManagedObject* RE4VRWeapons::find_grenade_generator() {
    if (m_grenade_gen.obj != nullptr) {
        if (re4vr::get_field_object(m_grenade_gen.obj, "throwRot") != nullptr
            || re4vr::obj_ok(m_grenade_gen.obj)) {
            return m_grenade_gen.obj;
        }

        drop(m_grenade_gen);
    }

    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    auto* t = re4vr::runtime_type("chainsaw.ThrowingGrenadeGenerator");

    if (t == nullptr) {
        return nullptr;
    }

    auto* comps = re4vr::call_safe<::REManagedObject*>(
        scene, "findComponents(System.Type)", t);

    if (comps == nullptr) {
        return nullptr;
    }

    // [ARRAY-BINDING] Lua: `comps:get_elements()` -- ein REFramework-Binding.
    if (re4vr::array_size(comps) > 0) {
        if (auto* first = re4vr::array_element(comps, 0)) {
            store(m_grenade_gen, first, true);   // Lua: add_ref

            return m_grenade_gen.obj;
        }
    }

    return nullptr;
}

// DIREKT-SPAWN: Granate an der HAND spawnen. Frueher Fehlschlag = kalte
// _GeneratePos(=0) -> Ursprung. Jetzt Hand-Position als requestFire-Pos.
bool RE4VRWeapons::do_vr_throw() {
    auto* gen = find_grenade_generator();

    if (gen == nullptr) {
        return false;
    }

    const auto pos = re4vr::lua_get_vec3("__vr_rh_world");

    if (!pos.has_value()) {
        return false;
    }

    // _GeneratePos ist ein via.vec3-FELD.
    if (auto* td = utility::re_managed_object::get_type_definition(gen)) {
        if (auto* f = td->get_field("_GeneratePos")) {
            try {
                if (auto* raw = f->get_data_raw(gen, false)) {
                    auto* v = reinterpret_cast<float*>(raw);
                    v[0] = pos->x;
                    v[1] = pos->y;
                    v[2] = pos->z;
                }
            } catch (...) {
            }
        }
    }

    // Identitaet; _CurrentMoveVec bestimmt die Flugbahn.
    const auto m = find_method(gen, "requestFire(via.vec3, via.Quaternion)");

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 p{pos->x, pos->y, pos->z, 0.0f};
    __declspec(align(16)) glm::vec4 q{0.0f, 0.0f, 0.0f, 1.0f};

    try {
        m->call_safe<void*>(context, gen, &p, &q);
    } catch (...) {
        return false;
    }

    clear_pending(context, true);

    return true;
}

// [GRENADE FAST-FORWARD] Native Wurf-Anim vorspulen: solange die Wurf-Motion
// laeuft + Frame < Spawn, Layer-Speed hochsetzen -> Shell kommt fast sofort
// statt nach ~1.1 s.
void RE4VRWeapons::grenade_ff_tick() {
    if (clock_now() > m_grenade_ff_until) {
        return;
    }

    if (re4vr::lua_get_table_tribool("__re4_gren_ff", "enabled") == 0) {
        return;
    }

    const int32_t want_id =
        static_cast<int32_t>(re4vr::lua_get_table_number("__re4_gren_ff", "id", 1400.0));
    const float want_frame =
        static_cast<float>(re4vr::lua_get_table_number("__re4_gren_ff", "frame", 57.0));
    const float speed =
        static_cast<float>(re4vr::lua_get_table_number("__re4_gren_ff", "speed", 6.0));

    auto* body = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
    auto* m = body != nullptr ? re4vr::get_component(body, "via.motion.Motion") : nullptr;

    if (m == nullptr) {
        return;
    }

    int32_t lc = 0;
    re4vr::try_call<int32_t>(m, "getLayerCount", lc);

    for (int32_t i = 0; i < lc; ++i) {
        auto* layer = re4vr::call_safe<::REManagedObject*>(m, "getLayer", i);

        if (layer == nullptr) {
            continue;
        }

        int32_t mid = 0;

        if (!re4vr::try_call<int32_t>(layer, "get_MotionID", mid) || mid != want_id) {
            continue;
        }

        float frame = 0.0f;
        re4vr::try_call<float>(layer, "get_Frame", frame);

        if (frame < want_frame) {
            re4vr::call_safe<void*>(layer, "set_Speed", speed);
        } else {
            // Shell da -> Rest normal
            re4vr::call_safe<void*>(layer, "set_Speed", 1.0f);
            m_grenade_ff_until = 0.0;
        }

        return;
    }
}

// ============================================================================
// Baustein 20 -- SHELL_EJECT (Lua Z.4509-4597)
// ============================================================================

void RE4VRWeapons::shell_refresh_muzzle() {
    auto* ctx = get_player_ctx_w();
    auto* hu = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                              : nullptr;
    auto* gun = hu != nullptr ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
                              : nullptr;
    auto* go = gun != nullptr ? re4vr::call_safe<::REManagedObject*>(gun, "get_GameObject")
                              : nullptr;
    auto* tf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
                             : nullptr;

    if (tf == nullptr) {
        return;
    }

    // Emit-Joint laut BulletCaseUserData (+Fallback)
    ::REManagedObject* j = nullptr;

    for (const char* name : {"vfx_muzzle2", "vfx_muzzle1"}) {
        j = transform_joint_by_name(tf, name);

        if (j != nullptr) {
            break;
        }
    }

    if (j == nullptr) {
        return;
    }

    glm::vec3 p{};
    glm::quat r{};

    if (get_vec3(j, "get_Position", p)) {
        re4vr::lua_set_vec3("__re4_shell_pos", p);
    }

    if (get_quat(j, "get_Rotation", r)) {
        re4vr::lua_set_quat("__re4_shell_rot", r);
    }

    if (const auto w = get_equip_weapon_id(); w.has_value()) {
        re4vr::lua_set_number("__re4_shell_wid", static_cast<double>(*w));
    }

    // [ENEMY-FIX 2026-07-16] Adressen der SPIELER-Gun cachen. Der Hook leitet
    // die Huelsen NUR dann um, wenn genau DIESE gunObj feuert -> Turret-Gegner
    // bleiben nativ.
    if (const auto a = addr_of(gun); a.has_value()) {
        re4vr::lua_set_number("__re4_shell_gun_addr", static_cast<double>(*a));
    }

    if (const auto a = addr_of(go); a.has_value()) {
        re4vr::lua_set_number("__re4_shell_go_addr", static_cast<double>(*a));
    }
}

namespace {

bool s_shell_skipped = false;
sdk::REMethodDefinition* s_shell_m_pos = nullptr;

}   // namespace

// ============================================================================
// [GRANATENFLUG 10.09.2026 -- PORT-WAISE geschlossen]
//
// Gemeldet: "egal was ich an den Slidern einstelle, es passiert nichts, und
// nach oben werfen aendert die Hoehe nicht". Gemessen: der Wurf-Block oben
// (Z.7301-7345) rechnet Richtung und Tempo korrekt aus und legt sie in den
// Globals __re4_throw_vel / __re4_throw_pending ab -- aber NIEMAND holt sie
// mehr ab. In Lua taten das zwei Hooks auf chainsaw.GrenadeShell
// (re4_vr_weapons.lua Z.4275-4310), die das Feld _CurrentMoveVec der
// gespawnten Shell schreiben. Im Port fehlten sie komplett, also flog die
// Granate rein nativ.
//
// Hier stehen sie nativ. Zwei Stellen, wie im Original:
//   1. activateRigidbody (POST) -- EINMAL pro Wurf der Startimpuls.
//   2. updateMove_Throwing / _Launch / updateMove (POST) -- jeden Frame
//      nachsetzen, sonst ueberschreibt der native Mover sofort wieder. Die
//      Schwerkraft muss dabei SELBST auf den Vektor gelegt werden: ein
//      konstanter Override verhindert die native Gravitation, die Granate
//      floege sonst schnurgerade.
//
// Bewusst OHNE Lua-Zugriff: diese Callbacks laufen im Spiel-Thread mitten im
// Flug, und ein lua_get/set braucht dort die ScriptRunner-Sperre. Die Werte
// stehen deshalb zusaetzlich in Membern -- die Globals bleiben erhalten, weil
// andere Module sie lesen.
// ============================================================================
namespace {
bool set_shell_move_vec(::REManagedObject* shell, const glm::vec3& v) {
    if (shell == nullptr) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(shell);

    if (td == nullptr) {
        return false;
    }

    auto* f = td->get_field("_CurrentMoveVec");

    if (f == nullptr) {
        return false;
    }

    try {
        // via.vec3-Feld auf einem managed object -> Container-Flag false
        if (auto* raw = f->get_data_raw(shell, false)) {
            auto* p = reinterpret_cast<float*>(raw);
            p[0] = v.x;
            p[1] = v.y;
            p[2] = v.z;

            return true;
        }
    } catch (...) {
    }

    return false;
}
} // namespace

void RE4VRWeapons::grenade_shell_launch(::REManagedObject* shell) {
    if (!m_throw_vel_pending) {
        return;   // nativer Wurf (kein eigener Schwung) -> nicht anfassen
    }

    if (!set_shell_move_vec(shell, m_throw_vel)) {
        return;
    }

    m_throw_vel_pending = false;
    m_throw_vel_last_t = clock_now();
}

void RE4VRWeapons::grenade_shell_move(::REManagedObject* shell) {
    const double now = clock_now();

    if (now > m_throw_apply_until) {
        return;   // Fenster zu -> ab hier gehoert die Shell wieder dem Spiel
    }

    double dt = now - m_throw_vel_last_t;

    if (dt < 0.0 || dt > 0.1) {
        dt = 1.0 / 60.0;
    }

    m_throw_vel_last_t = now;
    m_throw_vel.y -= static_cast<float>(m_tcfg.gravity * dt);

    set_shell_move_vec(shell, m_throw_vel);
}

void RE4VRWeapons::install_grenade_throw_hooks() {
    auto* td = sdk::find_type_definition("chainsaw.GrenadeShell");

    if (td == nullptr) {
        return;
    }

    // this liegt in args[1] (args[0] ist der VM-Kontext) -- in Lua war das
    // args[2], weil dort ab 1 gezaehlt wird.
    if (auto* m = td->get_method("activateRigidbody")) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                try {
                    if (auto& self = RE4VRWeapons::get(); self != nullptr && args.size() > 1) {
                        self->gren_activate_pre(reinterpret_cast<::REManagedObject*>(args[1]));
                    }
                } catch (...) {
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {
                try {
                    if (auto& self = RE4VRWeapons::get(); self != nullptr) {
                        self->gren_activate_post();
                    }
                } catch (...) {
                }
            });
    }

    // Alle drei Namen versuchen -- welcher existiert, entscheidet das Spiel.
    for (const char* mn : {"updateMove_Throwing", "updateMove_Launch", "updateMove"}) {
        auto* mm = td->get_method(mn);

        if (mm == nullptr) {
            continue;
        }

        g_hookman.add(
            mm,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                try {
                    if (auto& self = RE4VRWeapons::get(); self != nullptr && args.size() > 1) {
                        self->gren_move_pre(reinterpret_cast<::REManagedObject*>(args[1]));
                    }
                } catch (...) {
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {
                try {
                    if (auto& self = RE4VRWeapons::get(); self != nullptr) {
                        self->gren_move_post();
                    }
                } catch (...) {
                }
            });
    }
}

void RE4VRWeapons::install_shell_hook() {
    auto* td = sdk::find_type_definition("chainsaw.BulletCaseManager");

    if (td == nullptr) {
        return;
    }

    sdk::REMethodDefinition* m_gunobj = nullptr;

    // (WeaponID, gunObj) [vom Spiel genutzt] / (WeaponID, vec3, Quaternion)
    for (auto& m : td->get_methods()) {
        if (m.get_name() != std::string_view{"requestGenerateBulletCase"}) {
            continue;
        }

        const auto n = m.get_num_params();

        if (n == 2) {
            m_gunobj = &m;
        } else if (n == 3) {
            s_shell_m_pos = &m;
        }
    }

    if (m_gunobj == nullptr || s_shell_m_pos == nullptr) {
        return;
    }

    g_hookman.add(
        m_gunobj,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            bool handled = false;

            try {
                // [ENEMY-FIX 2026-07-16] NUR umleiten, wenn die SPIELER-Gun
                // feuert. Sonst native Huelse durchlassen -- sonst springen
                // Gegner-Huelsen aus der Spieler-Waffe. Index-robust ueber
                // args[0..3], weil die this-Position hier unklar ist.
                const auto ga = re4vr::lua_get_number("__re4_shell_gun_addr", 0.0);
                const auto goa = re4vr::lua_get_number("__re4_shell_go_addr", 0.0);

                bool mine = false;

                if (ga != 0.0 || goa != 0.0) {
                    for (size_t i = 0; i < 4 && i < args.size(); ++i) {
                        const auto a = static_cast<double>(args[i]);

                        if ((ga != 0.0 && a == ga) || (goa != 0.0 && a == goa)) {
                            mine = true;

                            break;
                        }
                    }
                }

                if (!mine) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                const auto p = re4vr::lua_get_vec3("__re4_shell_pos");
                const auto r = re4vr::lua_get_quat("__re4_shell_rot");
                const auto w = re4vr::lua_get_number_opt("__re4_shell_wid");

                // kein Cache -> native Huelse durchlassen
                if (!p.has_value() || !r.has_value() || !w.has_value()) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                auto* mgr = sdk::get_managed_singleton<::REManagedObject>(
                    "chainsaw.BulletCaseManager");

                if (mgr == nullptr) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                auto context = sdk::get_thread_context();
                __declspec(align(16)) glm::vec4 pv{p->x, p->y, p->z, 0.0f};
                __declspec(align(16)) glm::vec4 qv{r->x, r->y, r->z, r->w};

                s_shell_m_pos->call_safe<void*>(context, mgr, static_cast<int32_t>(*w), &pv,
                                                &qv);
                handled = true;
            } catch (...) {
                handled = false;
            }

            // Redirect ok -> native Brust-Huelse unterdruecken.
            if (!handled) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            s_shell_skipped = true;

            return HookManager::PreHookResult::SKIP_ORIGINAL;
        },
        [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) { s_shell_skipped = false; });
}

// ============================================================================
// Baustein 21 -- THROWSIGHT (Lua Z.4599-4715)
// ============================================================================

bool RE4VRWeapons::ts_suppress_now() {
    return re4vr::lua_get_tribool("__re4_suppress_throwsight") == 1
        && re4vr::lua_get_tribool("__re4_throwsight_active") != 1
        && re4vr::lua_get_tribool("__re4_boat_active") != 1;
}

// [THROWSIGHT SAVE-LOAD 19.09.2026] s. RE4VRWeapons.hpp
void RE4VRWeapons::ts_saveload_tick() {
    auto* body = re4vr::fc::body_go();

    if (body == nullptr) {
        return;
    }

    const auto a = reinterpret_cast<uintptr_t>(body);

    if (m_ts_body_addr.has_value() && *m_ts_body_addr != a) {
        drop(m_ts_ctrl);
    }

    m_ts_body_addr = a;
}

// Netz 2: aktiv herunterfahren, solange die Granate in der Hand ist. Holt auch
// die Linie herunter, die schon lief, bevor die Granate gezogen wurde.
void RE4VRWeapons::throwsight_force_off() {
    if (!ts_suppress_now()) {
        return;
    }

    if (m_ts_ctrl.obj == nullptr) {
        return;
    }

    if (!re4vr::obj_ok(m_ts_ctrl.obj)) {
        drop(m_ts_ctrl);

        return;
    }

    re4vr::call_safe<void*>(m_ts_ctrl.obj, "requestDeactivate");
}

void RE4VRWeapons::install_throwsight_hooks() {
    // ---- Netz 0: die breite Methodenliste --------------------------------
    // [GEMESSEN 20.08.] ThrowSightController hat gar kein `update`/`draw` --
    // real greift von dieser Liste nur `lateUpdate`, und dessen SKIP verhindert
    // nur das Nachrechnen der Shader-Parameter, nicht das Zeichnen einer
    // bereits EINGESCHALTETEN Linie. Die Liste bleibt trotzdem 1:1 stehen.
    for (const char* tn : {"chainsaw.ThrowSightController", "chainsaw.ThrowSight",
                           "chainsaw.ThrowSightManager"}) {
        auto* td = sdk::find_type_definition(tn);

        if (td == nullptr) {
            continue;
        }

        for (const char* mn : {"update", "onUpdate", "lateUpdate", "draw", "onDraw",
                               "doUpdate", "doLateUpdate"}) {
            auto* m = td->get_method(mn);

            if (m == nullptr) {
                continue;
            }

            g_hookman.add(
                m,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    // [THROWSIGHT-STAGE] Im Del-Lago-Abschnitt und im Boot wird
                    // nativ ueber die Linie gezielt -> NICHT unterdruecken.
                    auto& self = RE4VRWeapons::get();

                    if (self != nullptr && self->ts_suppress_now()) {
                        return HookManager::PreHookResult::SKIP_ORIGINAL;
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // ---- Netz 1: der echte Schalter --------------------------------------
    auto* td = sdk::find_type_definition("chainsaw.ThrowSightController");

    if (td == nullptr) {
        return;
    }

    auto* m_act = td->get_method("requestActivate");
    auto* m_de = td->get_method("requestDeactivate");

    // [REGISTERMUELL] SKIP_ORIGINAL auf einer Methode MIT Rueckgabewert liefert
    // dem Aufrufer Registermuell (teuer bezahlt am 01.08. beim
    // Durchquetsch-Antibeam). Deshalb wird der Rueckgabetyp zur LAUFZEIT
    // geprueft statt geraten.
    if (m_act != nullptr) {
        auto* rt = m_act->get_return_type();
        const std::string rn = rt != nullptr ? rt->get_full_name() : std::string{"?"};
        m_ts_skip_ok = rn == "System.Void";

        g_hookman.add(
            m_act,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                auto& self = RE4VRWeapons::get();

                if (self == nullptr) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                // Die Instanz merken: ueber sie rufen wir requestDeactivate.
                if (args.size() >= 2) {
                    self->set_ts_ctrl(reinterpret_cast<::REManagedObject*>(args[1]));
                }

                if (self->ts_skip_ok() && self->ts_suppress_now()) {
                    return HookManager::PreHookResult::SKIP_ORIGINAL;
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }

    // Auch die Abschaltung mitschneiden: liefert die Instanz selbst dann, wenn
    // requestActivate nie durch unseren Hook laeuft.
    if (m_de != nullptr) {
        g_hookman.add(
            m_de,
            [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                auto& self = RE4VRWeapons::get();

                if (self != nullptr && args.size() >= 2) {
                    self->set_ts_ctrl(reinterpret_cast<::REManagedObject*>(args[1]));
                }

                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    }
}

// ============================================================================
// Baustein 22 -- Haupt-on_frame (Lua Z.4717-5075)
// ============================================================================


// ============================================================================
// [MESSER-HEIMAT 2026-09-12] Wachhund gegen das verlorene Messer
// ============================================================================
//
// BEFUND (Sonde zzz_re4_knife_weg_probe.lua, 12.09.2026, nach vielen
// Tier-Chokes und vielen Wuerfen):
//   wp5001  DrawSelf=true  MeshEnabled=true  BaseColor=(1 1 1)
//           pos=(-0.09 -0.03 0.02)   haengt an: wp5001
// Also Mesh heil, Sichtbarkeit heil, Materialien heil -- aber das Transform
// hatte KEINEN Parent mehr und stand am Weltnullpunkt, waehrend
// get_IsEquipKnife weiter true meldete. Daher war es unsichtbar, nicht aus dem
// Holster zu holen und nicht zu flippen: Holster und Flip schalten Zustaende an
// einem Objekt, das im Nirgendwo haengt.
//
// WARUM DAS KEIN EINZELFALL IST: gleich drei Wege setzen den Parent absichtlich
// auf nullptr --
//   * knife_detach()             [ZITTER_FIX] ab dem Einschlag,
//   * knife_return()             Klon-Zweig ("lefthand haengt es selbst an"),
//   * RE4VRChoke::stick_return() Notweg bei stalem Rueckweg.
// Jeder hat einen Rueckweg; faellt EINER aus (stales Handle, Save-Load, Gegner
// stirbt im falschen Frame), bleibt das Messer bis zum Levelwechsel weg. Statt
// jeden Weg einzeln zu haerten, faengt dieser Wachhund den ZUSTAND ab -- er
// gilt damit auch fuer Wege, die es heute noch gar nicht gibt.
//
// BAUFORM, bewusst so:
//   * Er LERNT nur von gesunden Messern (Parent vorhanden) und erschafft
//     nichts. Gibt es gar kein Messer, hat er nichts zu tun -- genau die Ansage
//     "es sei denn wir haben gar keins".
//   * Er fuehrt eine LISTE statt eines Einzelplatzes: linke und rechte Hand,
//     Original und Klon haben jeweils ihre eigene Heimat (Ansage "fuer beide
//     Seiten").
//   * KULANZ von 3 s vor dem Eingriff. Jedes legitime Detach-Fenster (Flug,
//     Einschlag, Umhaengen im Choke) ist kuerzer -- so muss er die Sonderwege
//     nicht kennen, um ihnen nicht in die Quere zu kommen.
//   * Zusaetzlich schweigt er, solange ein Wurf dieses Transform haelt, das
//     Messer im Gegner steckt, der Choke greift oder ein Links-Klon gemeldet
//     ist.
//   * Hoechstens eine Reparatur pro Sekunde, und jede wird geloggt -- so bleibt
//     sichtbar, WIE OFT es passiert, auch wenn der Spieler nichts mehr merkt.
namespace {

bool is_knife_go_name(const std::string& nm) {
    static const char* KNIFE_WP[] = {
        "wp5000", "wp5001", "wp5002", "wp5003", "wp5006", "wp6107", "wp6108", "wp6305",
    };

    if (nm.size() < 6) {
        return false;
    }

    // Der Name kann ein _AO/_MC-Suffix tragen -> Praefixvergleich ueber sechs
    // Zeichen, wie ueberall sonst im Haus.
    const std::string pre = nm.substr(0, 6);

    for (const char* k : KNIFE_WP) {
        if (nm == k || pre == k) {
            return true;
        }
    }

    return false;
}

}   // namespace

// [RELOAD/TOD 12.09.2026] Alles Gemerkte fallen lassen und 3 s Ruhe geben.
void RE4VRWeapons::knife_home_forget(double now) {
    if (!m_knife_home.empty()) {
        for (auto& e : m_knife_home) {
            drop(e.tf);
            drop(e.parent);
        }

        m_knife_home.clear();
    }

    m_knife_home_body = nullptr;
    m_knife_home_ready = now + 3.0;
}

void RE4VRWeapons::knife_home_guard() {
    const double now = clock_now();

    // [RELOAD/TOD 12.09.2026 -- gemessen, nicht vermutet] Ausserhalb des
    // Gameplays raeumt die Engine die Waffen selbst um; dort ist "parentlos"
    // ein Zwischenstand. Entscheidend ist aber, was DANACH passiert: im Log vom
    // 23:17 lernte der Wachhund unmittelbar nach einem Reload ein halbfertiges
    // wp5001, schrieb set_Parent auf ein Objekt aus dem abgeraeumten Zustand
    // ("GRIFF NICHT") und wiederholte das im Sekundentakt -- elf Sekunden
    // spaeter begann REFramework, den D3D-Hook neu zu setzen: das Bild stand.
    // set_Parent auf einen stalen Transform ist nativ toedlich, das ist im Haus
    // teuer bezahlt. Also: beim Verlassen des Gameplays ALLES vergessen.
    if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") != 1) {
        knife_home_forget(now);

        return;
    }

    auto* btf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(btf)) {
        knife_home_forget(now);

        return;
    }

    // Ein anderer Koerper heisst: andere Szene, andere Objekte. Alles Gemerkte
    // zeigt dann auf Leichen.
    if (btf != m_knife_home_body) {
        knife_home_forget(now);
        m_knife_home_body = btf;
    }

    // Karenz nach jedem Wechsel: die Engine haengt Waffen in den ersten
    // Sekunden selbst um. Wer da mitschreibt, kaempft gegen einen Zustand, der
    // sich jeden Frame aendert.
    if (now < m_knife_home_ready) {
        return;
    }

    // ---- 1. Heimat lernen -------------------------------------------------
    // Nur von Messern, die GESUND am Koerper haengen. Zweimal pro Sekunde
    // genuegt -- der Verlust selbst haelt ja an.
    if (now - m_knife_home_scan >= 0.5) {
        m_knife_home_scan = now;

        const std::function<void(::REManagedObject*, int)> walk =
            [&](::REManagedObject* tf, int d) {
                if (!re4vr::obj_ok(tf) || d < 0) {
                    return;
                }

                auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
                const std::string nm = obj_name_of(go);

                if (is_knife_go_name(nm)) {
                    auto* par = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");

                    if (re4vr::obj_ok(par)) {
                        KnifeHome* e = nullptr;

                        for (auto& k : m_knife_home) {
                            if (k.tf.obj == tf) {
                                e = &k;

                                break;
                            }
                        }

                        if (e == nullptr && m_knife_home.size() < 6) {
                            m_knife_home.emplace_back();
                            e = &m_knife_home.back();
                            store(e->tf, tf);
                        }

                        if (e != nullptr) {
                            store(e->parent, par);
                            e->joint = transform_parent_joint_name(tf);
                            e->name = nm;

                            // [SPRECHEN 12.09.2026] Ein Wachhund, der nur im
                            // Erfolgsfall schreibt, ist von einem toten nicht zu
                            // unterscheiden -- genau das kostete den ersten
                            // Anlauf. Also einmal je Eintrag melden, dass er
                            // lebt und was er gelernt hat.
                            if (!e->learned) {
                                e->learned = true;
                            }


                            e->lost_since = 0.0;
                            e->alien_since = 0.0;
                        }
                    }
                }

                auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
                int32_t n = 0;

                while (ch != nullptr && n < 128) {
                    ++n;
                    walk(ch, d - 1);
                    ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
                }
            };

        walk(btf, 8);
    }

    if (m_knife_home.empty()) {
        return;
    }

    // Laeuft gerade etwas, das ein loses Messer ERWARTET? Dann Finger weg.
    // __re4_choke_knife_stuck ist ein Zeitstempel, den der Choke setzt, solange
    // das Messer im Gegner steckt, und beim Rueckholen auf nil raeumt -- schon
    // seine ANWESENHEIT ist die Antwort, ein Zeitvergleich waere ueberfluessig.
    const bool busy = re4vr::lua_get_number_opt("__re4_choke_knife_stuck").has_value()
        || re4vr::lua_call_global_bool("__re4_is_choking", false)
        || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;

    // [DIE EIGENTLICHE REGEL 12.09.2026 -- Ansage "es darf nicht vorkommen, dass
    // ich nach einem Messer greife und keins kommt, obwohl ich eins im Inventar
    // habe"] Sagt das Spiel "Messer gezogen", dann MUSS es sichtbar in der Hand
    // sein -- Parent unter dem Spieler, Scale 1, DrawSelf an. Ist das verletzt,
    // ist es egal, welcher der Wege es kaputtgemacht hat: es wird sofort
    // geradegezogen, nicht erst nach Kulanz.
    bool knife_equipped = false;

    {
        auto* ctx = get_player_ctx_w();
        auto* hu = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
            : nullptr;

        if (re4vr::obj_ok(hu)) {
            bool v = false;

            if (re4vr::try_call<bool>(hu, "get_IsEquipKnife", v)) {
                knife_equipped = v;
            }
        }
    }

    // ---- 2. Heimatlose zurueckhaengen -------------------------------------
    for (auto it = m_knife_home.begin(); it != m_knife_home.end();) {
        auto& e = *it;

        if (!re4vr::obj_ok(e.tf.obj)) {
            // Save-Load/Levelwechsel: das GO ist weg, der Eintrag wertlos.
            drop(e.tf);
            drop(e.parent);
            it = m_knife_home.erase(it);

            continue;
        }

        // ------------------------------------------------------------------
        // [SCALE 0 -- 12.09.2026, Ansage "Huhn erstochen, Messer bleibt weg,
        // kommt erst beim Griff zum Holster"] Der zweite, voellig eigene Weg,
        // auf dem das Messer verschwindet -- und der haeufigere:
        //
        // RE4VRChoke::stick_hide_real() setzt beim Stich das ECHTE Messer auf
        // set_LocalScale(0,0,0) und laesst eine Kopie im Opfer stecken (Regel-
        // fall seit dem [STICK-KLON]). Zurueck auf (1,1,1) geht es an GENAU
        // EINER Stelle: stick_return(). Faellt die aus -- Opfer verschwindet,
        // Handle wird stale, Script-Reset mitten im Nachlauf -- bleibt das
        // Messer in der Hand liegen und ist unsichtbar. Parent und DrawSelf
        // sind dabei voellig in Ordnung, deshalb sah der Wachhund bisher nichts.
        // ------------------------------------------------------------------
        {
            glm::vec3 sc{};

            if (get_vec3(e.tf.obj, "get_LocalScale", sc)) {
                const bool winzig = std::fabs(sc.x) < 0.01f && std::fabs(sc.y) < 0.01f
                    && std::fabs(sc.z) < 0.01f;

                if (!winzig) {
                    e.tiny_since = 0.0;
                } else {
                    if (e.tiny_since == 0.0) {
                        e.tiny_since = now;

                    }

                    // Waehrend der Stich-Nachlauf laeuft, SOLL es unsichtbar
                    // sein -- aber keine Sekunde laenger: __re4_choke_stick_until
                    // ist der Zeitpunkt, zu dem der Choke das echte Messer
                    // zurueckholt und selbst auf Scale 1 setzt. Ein halbe
                    // Sekunde Zugabe, damit sein eigener Weg den Vortritt hat;
                    // danach ist Scale 0 schlicht ein Fehler.
                    const double dauer_klein = now - e.tiny_since;
                    const auto stick_until = re4vr::lua_get_number_opt("__re4_choke_stick_until");
                    const bool nachlauf =
                        stick_until.has_value() && now < (*stick_until + 0.5);

                    // Ein GEZOGENES Messer wartet auf nichts: dann ist Scale 0
                    // schlicht falsch, auch waehrend eines Nachlaufs -- der galt
                    // dem Messer, das gerade NICHT in der Hand sein sollte.
                    if ((knife_equipped && dauer_klein >= 0.3)
                        || (!nachlauf && dauer_klein >= 0.5)) {
                        set_vec3(e.tf.obj, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
                        e.tiny_since = 0.0;

                    }
                }
            }
        }

        // [DRAWSELF-ZWEIG AUSGEBAUT 13.09.2026 -- er hat die Pistole blockiert]
        // Hier stand eine Heilung, die ein GEZOGENES Messer bedingungslos wieder
        // einblendete. Waehrend eines Waffenwechsels meldet der HeadUpdater aber
        // noch "Messer", obwohl das Spiel dessen Mesh schon korrekt versteckt
        // hat -- der Wachhund schaltete es also jeden Frame zurueck auf sichtbar
        // und kaempfte gegen hide_body_weapons_tick. Im Log stand das im
        // 12-ms-Takt, und der Griff zur Pistole im Holster kam nicht mehr durch.
        //
        // Die Sichtbarkeit gehoert dem, der die aktuelle Waffen-ID kennt: das
        // ist hide_body_weapons_tick mit seiner eigenen [SELBSTHEILUNG]. Dieser
        // Wachhund kuemmert sich nur noch um Parent, Scale und fremden Baum --
        // Zustaende, die sonst NIEMAND repariert.

        auto* par = re4vr::call_safe<::REManagedObject*>(e.tf.obj, "get_Parent");

        if (re4vr::obj_ok(par)) {
            e.lost_since = 0.0;

            // ------------------------------------------------------------------
            // [FREMDER BAUM 12.09.2026 -- Ansage "das darf nicht passieren"] Der
            // dritte Weg, auf dem das Messer wegbleibt: es haengt am GEGNER --
            // Parent gesetzt, Scale 1, alles "heil", nur eben in der Leiche
            // statt in der Hand. So endet der alte Steckweg, wenn
            // RE4VRChoke::stick_return() ausfaellt, und so sieht es auch nach
            // einem Wurf aus, dessen knife_unstick() nie lief.
            //
            // Kriterium ist BEWUSST nicht "Parent != Heimatparent": Hand,
            // Holster und Ruecken sind verschiedene JOINTS am selben Parent, ein
            // Vergleich des Parents allein wuerde dort staendig Fehlalarm geben.
            // Gefragt wird stattdessen, ob das Messer ueberhaupt noch UNTER DEM
            // SPIELER haengt -- alles andere ist ein fremdes Objekt.
            // ------------------------------------------------------------------
            bool unter_spieler = false;

            {
                auto* p = par;
                int hop = 0;

                while (re4vr::obj_ok(p) && hop < 12) {
                    ++hop;

                    if (p == btf) {
                        unter_spieler = true;

                        break;
                    }

                    p = re4vr::call_safe<::REManagedObject*>(p, "get_Parent");
                }
            }

            if (unter_spieler) {
                e.alien_since = 0.0;
                ++it;

                continue;
            }

            if (e.alien_since == 0.0) {
                e.alien_since = now;

                ++it;

                continue;
            }

            // Solange es ABSICHTLICH steckt, bleibt es stecken: laufender Wurf,
            // gemeldetes Stecken, Choke-Nachlauf. Auch hier gilt die harte
            // Obergrenze -- ein haengengebliebenes Flag darf den Wachhund nicht
            // wieder auf Dauer stilllegen (die Lehre vom ersten Anlauf).
            const auto stick_until = re4vr::lua_get_number_opt("__re4_choke_stick_until");
            const bool steckt_absicht =
                (m_kfly.active && m_kfly.tf.obj == e.tf.obj) || m_kfly.stuck || busy
                || (stick_until.has_value() && now < (*stick_until + 0.5));
            const double dauer_fremd = now - e.alien_since;

            if (knife_equipped ? (dauer_fremd < 0.3)
                               : (steckt_absicht ? (dauer_fremd < 15.0)
                                                 : (dauer_fremd < 1.5))) {
                ++it;

                continue;
            }

            if (now - m_knife_home_fix < 1.0) {
                ++it;

                continue;
            }

            auto* heim = re4vr::obj_ok(e.parent.obj) ? e.parent.obj : btf;

            re4vr::call_safe<void*>(e.tf.obj, "set_Parent(via.Transform)", heim);

            if (!e.joint.empty()) {
                transform_set_parent_joint(e.tf.obj, e.joint);
            }

            set_vec3(e.tf.obj, "set_LocalPosition", glm::vec3{0.0f, 0.0f, 0.0f});
            m_knife_home_fix = now;

            auto* jetzt = re4vr::call_safe<::REManagedObject*>(e.tf.obj, "get_Parent");
            const bool heim_ok = re4vr::obj_ok(jetzt) && jetzt == heim;

            if (heim_ok) {
                e.alien_since = 0.0;
                e.fails = 0;
            } else if (++e.fails >= 2) {

                drop(e.tf);
                drop(e.parent);
                it = m_knife_home.erase(it);

                continue;
            }

            ++it;

            continue;
        }

        // Die Uhr laeuft AB DEM VERLUST -- unabhaengig davon, ob gerade ein
        // Schutzgrund gilt. Sonst stellt ein einziges haengengebliebenes Flag
        // den Wachhund fuer immer still, und genau das ist am 12.09. passiert:
        // die alte Sperre fragte `m_kfly.tf == tf` ohne `m_kfly.active`, und
        // m_kfly.tf wird nach einem Wurf nie freigegeben -- kein drop, erst der
        // naechste Wurf ueberschreibt es. Im ganzen Lauf stand keine einzige
        // [KNIFEHOME]-Zeile im Log.
        if (e.lost_since == 0.0) {
            e.lost_since = now;

            ++it;

            continue;
        }

        const double dauer_los = now - e.lost_since;

        // Schutzgruende bremsen, sie sperren NICHT MEHR AUF DAUER:
        //   * laufender Flug mit genau diesem Transform,
        //   * Messer steckt im Gegner / Choke greift / Links-Klon gemeldet.
        // Sie kaufen 10 s. Danach gilt: was so lange parentlos ist, ist verloren
        // und nicht "in Arbeit" -- ein Flug dauert unter zwei Sekunden.
        const bool schutz = (m_kfly.active && m_kfly.tf.obj == e.tf.obj) || busy;

        if (knife_equipped ? (dauer_los < 0.3)
                           : (schutz ? (dauer_los < 10.0) : (dauer_los < 1.5))) {
            ++it;

            continue;
        }

        if (now - m_knife_home_fix < 1.0) {
            ++it;

            continue;
        }

        // Heimatparent bevorzugen, Koerper als Rueckfall -- ein stales Handle
        // waere eine native AV, darum beide gegengeprueft.
        auto* home = re4vr::obj_ok(e.parent.obj) ? e.parent.obj : btf;
        const bool eigen = re4vr::obj_ok(e.parent.obj);
        const double dauer = dauer_los;

        re4vr::call_safe<void*>(e.tf.obj, "set_Parent(via.Transform)", home);

        if (!e.joint.empty()) {
            transform_set_parent_joint(e.tf.obj, e.joint);
        }

        set_vec3(e.tf.obj, "set_LocalPosition", glm::vec3{0.0f, 0.0f, 0.0f});

        m_knife_home_fix = now;

        // [GEGENGELESEN] Ob set_Parent gegriffen hat, sagt nur das Nachlesen --
        // call_safe meldet einen Fehlschlag nicht.
        const bool ok =
            re4vr::obj_ok(re4vr::call_safe<::REManagedObject*>(e.tf.obj, "get_Parent"));

        if (ok) {
            e.lost_since = 0.0;
            e.fails = 0;
        } else if (++e.fails >= 2) {
            // Zweimal nicht gegriffen heisst: das Ziel taugt nicht mehr. Weiter
            // draufzuschreiben ist genau der Weg, der am 12.09. das Bild zum
            // Stehen brachte -- also Eintrag weg und beim naechsten gesunden
            // Messer neu lernen.

            drop(e.tf);
            drop(e.parent);
            it = m_knife_home.erase(it);

            continue;
        }

        ++it;
    }
}

void RE4VRWeapons::on_frame() {
    // [SCRIPTGATE -- ergaenzt 07.09.2026] Riegel zu = dieses Modul ist so still,
    // als waere seine Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    // Fehlte hier komplett: waehrend der Gondelfahrt schaltet RE4VRObjects alle
    // Scripte ab AUSSER objects/binding/materials/firstperson (BLEIBT-Liste) --
    // re4_vr_weapons.lua und re4_vr_weapons2.lua gehoerten nie dazu, liefen im
    // Port aber weiter.
    if (re4vr::mods_gated()) {
        return;
    }

    if (!ensure_init()) {
        return;
    }

    // [THROWSIGHT SAVE-LOAD 19.09.2026] Vor allem anderen: nach Tod/Laden den
    // alten Wurflinien-Controller wegwerfen, bevor ihn jemand anfasst.
    ts_saveload_tick();

    // Reihenfolge = Lua Z.4720-4727.
    shell_refresh_muzzle();
    hide_assist_light();
    grenade_ff_tick();
    weapon_switch_skip_tick();

    update_knife_holster_timer();
    keep_knife_out();

    // Der Ringpuffer der Stichrichtung (Lua: eigenes on_frame am Dateiende).
    knife_dir_tick();

    // [WURF-KLON 12.09.2026] BEWUSST hier und nicht in knife_flight_tick: die
    // steckende Kopie lebt weiter, wenn das Messer laengst zurueck in der Hand
    // ist -- der Flug-Tick laeuft dann nicht mehr.
    knife_stick_clone_tick();

    // [ENGER RIEGEL 13.09.2026] Uhr fuer den Riegel unten in cb_equip_weapon.
    if (re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1) {
        if (m_pure_since == 0.0) {
            m_pure_since = clock_now();
        }
    } else {
        m_pure_since = 0.0;
    }

    // [MESSER-HEIMAT AUSGEBAUT 13.09.2026, 01:49] Der Wachhund ist STILLGELEGT.
    //
    // Grund, gemessen: "Exception thrown in call to set_Parent" mitsamt
    // Stacktrace im Framework-Log. Er haengt heimatlose Messer zurueck, und
    // set_Parent auf einen stalen Transform ist nativ toedlich -- get_Valid
    // meldete vorher true, das genuegt hier nachweislich NICHT. Dazu hatte
    // schon sein DrawSelf-Zweig den Waffenwechsel blockiert.
    //
    // Die Abwaegung ist damit eindeutig: ein Messer, das selten einmal haengt
    // und nach einem Raumwechsel wieder da ist, ist besser als ein Wachhund,
    // der das Spiel zerlegt. Die eigentlichen Verlustwege gehoeren ohnehin dort
    // repariert, wo sie entstehen (stick_return, knife_return, knife_unstick) --
    // nicht hinterher von aussen.
    //
    // knife_home_guard() bleibt als Code stehen, wird aber NICHT mehr gerufen.

    // [GEN_SAVELOAD] eigener Tick (Lua Z.4454).
    grenade_cache_guard();

    // [WAISE 05.09.2026] Hier stand `m_vr_holster_knife` -- ein Member, das
    // NIRGENDS auf true gesetzt wird. Lua Z.4729 liest das GLOBAL
    // `vr_holster_knife`, und genau das setzt RE4VRBinding.cpp:2098 beim
    // Loslassen des linken Grips. Der Port hat den Namen zum Member gemacht,
    // ohne den Schreiber mitzunehmen: das Messer liess sich per Grip nicht mehr
    // wegstecken, und das Lua-Flag blieb dauerhaft auf true stehen, weil es
    // niemand mehr loeschte.
    if (re4vr::lua_get_tribool("vr_holster_knife") == 1) {
        re4vr::lua_set_bool("vr_holster_knife", false);
        m_vr_holster_knife = false;

        if (const auto id = get_equip_weapon_id(); id.has_value() && is_knife_id(*id)) {
            holster_knife();
        }
    }

    // ---- [KNIFE_THROW] ---------------------------------------------------
    const bool knife_equ = is_knife_equipped();

    // fuer binding.lua (R-Grip -> Wurf statt Aim)
    re4vr::lua_set_bool("__re4_knife_equipped", knife_equ);

    // [KNIFE_HAND] Tri-State. Hier NUR ABLEITEN, nie schreiben:
    // __re4_knife_left_intent gehoert re4_vr_knife_lefthand (weapons2).
    if (!knife_equ) {
        re4vr::lua_set_string("__re4_knife_hand", "none");
    } else {
        re4vr::lua_set_string("__re4_knife_hand",
                              re4vr::lua_get_tribool("__re4_knife_left_intent") == 1
                                  ? "left"
                                  : "right");
    }

    const double now = clock_now();

    // [LH_CLONE WURF] auch der Links-Klon (nicht equippt) soll werfen.
    // [FLUG-WAISE 10.09.2026 -- gemessen: "Twirl-Sound kommt, Waffe dreht sich
    // nicht"] Bis hierher hing knife_flight_tick() allein am Messer in der Hand
    // -- und es ist die EINZIGE Stelle, die den Flug beendet (m_kfly.active =
    // false bei return_t). Verschwand das Messer waehrend des Flugs aus der Hand
    // (Waffenwechsel, Cutscene, Holster), lief der Tick nie wieder -> active
    // blieb fuer immer true. knife_flight_apply() schreibt __re4_knife_flying
    // dagegen UNGEGATED in fuenf Render-Paessen weiter, und motion/attach_weapon
    // setzt bei diesem Flag den Waffen-Pin fuer JEDE Waffe aus (why=knife_fly)
    // -> kein Twirl mehr, waehrend Sound und Finger-Blend an einem anderen Zweig
    // munter weiterliefen. Deshalb den laufenden Flug selbst mit ins Gate: er
    // muss sich beenden duerfen, egal was gerade in der Hand liegt.
    if (knife_equ || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
        || m_kfly.active) {
        knife_flight_tick();

        // Kein Wurf-Windup waehrend Flug ODER waehrend die aktive Hand am
        // Holster-Dummy ist (dort = ziehen).
        const bool left_knife = re4vr::lua_get_string("__re4_knife_hand") == "left"
            || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;

        // [WAISE 05.09.2026 -- gemeldet: "das komplette Messer werfen geht nicht
        // mehr"] Hier stand `lua_call_global<bool>("__re4_knife_grip_held")`.
        // Diese Funktion war in re4_vr_weapons.lua Z.3945 definiert -- also in
        // GENAU DER DATEI, die dieses Modul portiert. Mit ihrem Port ist sie
        // verschwunden; der Aufruf lief ins Leere, `.value_or(false)` machte
        // daraus ein hartes false, `gripping` wurde nie wahr und damit gab es
        // weder Wurf-Windup noch Abwurf. Der Rest der Wurfkette war intakt.
        // 1:1 nachgebaut: Grip der Hand, die das Messer GERADE haelt.
        const bool grip_raw = knife_grip_held();

        const bool hz = left_knife
            ? re4vr::lua_get_tribool("__vr_knife_lh_holster_zone") == 1
            : re4vr::lua_get_tribool("__vr_knife_holster_zone") == 1;

        // [KNIFE_FLIP] In Flipped-Position KEIN Wurf-Windup.
        const bool flipped = re4vr::lua_get_tribool("__vr_knife_flip") == 1;

        const bool gripping = !m_kfly.active && !hz && !flipped && grip_raw;

        // Melee-Gate: kein Stich waehrend Wurf-Windup
        re4vr::lua_set_bool("__re4_knife_throw_gripping", gripping);

        if (gripping) {
            update_throw_velocity(false);

            glm::vec3 d{};

            if (get_throw_direction(m_tcfg.knife_pitch, m_tcfg.knife_yaw, d)) {
                re4vr::lua_set_vec3("__re4_knife_throw_dir", d);
            }

            if (get_throw_direction(0.0f, 0.0f, d)) {
                re4vr::lua_set_vec3("__re4_knife_throw_dir_raw", d);
            }

            m_kthrow.peak = compute_release_window_peak();
        }

        if (m_kthrow.was_gripping && !gripping) {
            auto dir_o = re4vr::lua_get_vec3("__re4_knife_throw_dir");

            // [ANTI-NOSEDIVE 2026-07-06] Ein natuerlicher Wurf flickt nach
            // vorne-UNTEN und schickt das Messer in den Boden. Die HORIZONTALE
            // Zielrichtung bleibt exakt, nur der Runter-Anteil wird auf ~14 Grad
            // begrenzt. Nach oben bleibt frei.
            if (dir_o.has_value()) {
                constexpr float MIN_Y = -0.25f;

                if (dir_o->y < MIN_Y) {
                    const float h = std::sqrt(dir_o->x * dir_o->x + dir_o->z * dir_o->z);

                    if (h > 0.001f) {
                        const float hscale =
                            std::sqrt(std::max(0.0001f, 1.0f - MIN_Y * MIN_Y)) / h;
                        dir_o = glm::vec3{dir_o->x * hscale, MIN_Y, dir_o->z * hscale};
                        re4vr::lua_set_vec3("__re4_knife_throw_dir", *dir_o);
                    }
                }
            }

            const bool hmd_cal =
                re4vr::lua_get_table_tribool("__re4_knife_fly_cfg", "hmd_force") == 1;

            // [VORWAERTS-CHECK] Ein schneller Rueckzug zum Koerper hat hohe
            // Velocity, zeigt aber nach HINTEN -> KEIN Wurf.
            bool fwd_ok = true;

            {
                const auto draw = re4vr::lua_get_vec3("__re4_knife_throw_dir_raw");
                const auto fwd = get_char_forward();

                if (draw.has_value() && fwd.has_value()) {
                    const float fl = glm::length(*fwd);

                    if (fl > 0.001f) {
                        fwd_ok = glm::dot(*draw, *fwd) / fl > 0.15f;
                    }
                }
            }

            if (hmd_cal) {
                fwd_ok = true;   // im Kalibrier-Modus nicht blocken
            }

            const float thr = static_cast<float>(re4vr::lua_get_number(
                "__re4_knife_throw_threshold", m_tcfg.hand_speed_min));

            if (fwd_ok && m_kthrow.peak >= thr && (now - m_last_knife_throw_t) >= 0.4
                && dir_o.has_value()) {
                m_last_knife_throw_t = now;

                glm::vec3 dir = *dir_o;

                // [RICHTUNGS-CLAMP] Wurfrichtung optional in einen Kegel um
                // char_forward zwingen (default 90 = AUS).
                {
                    const float maxd = kfc("dir_max_deg", 90.0);
                    const auto fwd = maxd < 89.0f ? get_char_forward()
                                                  : std::optional<glm::vec3>{};
                    const float fl = fwd.has_value() ? glm::length(*fwd) : 0.0f;

                    if (fl > 0.001f) {
                        const glm::vec3 f = *fwd / fl;
                        const float dot = glm::clamp(glm::dot(dir, f), -1.0f, 1.0f);
                        const float ang = std::acos(dot);
                        const float maxa = maxd * glm::pi<float>() / 180.0f;

                        if (ang > maxa && ang > 0.001f) {
                            const float t = maxa / ang;
                            const float s = std::sin(ang);
                            const float a = std::sin((1.0f - t) * ang) / s;
                            const float b = std::sin(t * ang) / s;
                            const glm::vec3 n = f * a + dir * b;
                            const float nl = glm::length(n);

                            if (nl > 0.001f) {
                                dir = n / nl;
                            }
                        }
                    }
                }

                // [ZIELHILFE-KEGEL] Der Kegel macht NUR das Homing; der
                // Basis-Flug bleibt die Handrichtung.
                {
                    const float strg = kfc("assist_strength", 0.0);
                    const float homing = kfc("assist_homing", 0.05);

                    re4vr::lua_set_number("__re4_knife_assist_flat", 0.0);
                    re4vr::lua_set_nil("__re4_knife_home");
                    re4vr::lua_set_nil("__re4_knife_home_ctx");
                    re4vr::lua_set_number("__re4_knife_home_str", 0.0);

                    if (strg > 0.0f || homing > 0.0f) {
                        auto origin = knife_hand_world();
                        std::optional<glm::vec3> caxis{};

                        // [HMD-KEGEL] Ziel-Erkennung um die ECHTE
                        // Blickrichtung: primaere Kamera get_AxisZ. NICHT
                        // Handbewegung/char_forward -- die zeigen in VR oft fast
                        // ENTGEGENGESETZT zum Blick, wodurch die Kiste GENAU vor
                        // dir als "hinten" galt.
                        {
                            auto* cam = sdk::get_primary_camera();
                            auto* cgo = cam != nullptr
                                ? re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject")
                                : nullptr;
                            auto* ctf = cgo != nullptr
                                ? re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform")
                                : nullptr;

                            glm::vec3 fwd{};

                            if (ctf != nullptr && get_vec3(ctf, "get_AxisZ", fwd)) {
                                // [FORWARD-VORZEICHEN] get_AxisZ zeigt bei dieser
                                // Kamera NACH HINTEN -> negieren.
                                const float l = glm::length(fwd);

                                if (l > 1e-6f) {
                                    caxis = -fwd / l;
                                }
                            }

                            // [BLICK-URSPRUNG] Der Kegel geht vom AUGE aus.
                            glm::vec3 cpos{};

                            if (caxis.has_value() && ctf != nullptr
                                && get_vec3(ctf, "get_Position", cpos)) {
                                origin = cpos;
                            }

                            if (!caxis.has_value()) {
                                caxis = dir;   // Fallback: Handrichtung
                            }
                        }

                        const float outer = kfc("assist_cone_deg", 20.0);
                        const float inner = kfc("assist_cone_inner_deg", 8.0);

                        glm::vec3 tp{};
                        ::REManagedObject* tctx = nullptr;
                        bool have_tp = false;

                        if (origin.has_value() && caxis.has_value()) {
                            // [ZIELWAHL] Gegner haben IMMER Vorrang.
                            have_tp = knife_assist_target(*origin, *caxis, 30.0f, outer, tp,
                                                          tctx);

                            if (!have_tp) {
                                float bn = 0.0f;
                                have_tp = knife_assist_breakable(*origin, *caxis, 30.0f,
                                                                 outer, tp, bn);
                                tctx = nullptr;
                            }
                        }

                        // [GRADUELL/NAHFANG] Der Falloff rechnet MIT DERSELBEN
                        // Metrik wie die Zielwahl -- sonst waehlt der
                        // Mindest-Schlauch den nahen Gegner aus und der
                        // Winkel-Falloff gibt ihm fall=0.
                        float fall = 0.0f;

                        if (have_tp && origin.has_value() && caxis.has_value()) {
                            const float dx = tp.x - origin->x;
                            const float dz = tp.z - origin->z;
                            const float dl = std::sqrt(dx * dx + dz * dz);
                            const float cl =
                                std::sqrt(caxis->x * caxis->x + caxis->z * caxis->z);

                            if (dl > 1e-6f && cl > 1e-6f) {
                                const float tdot = (dx * caxis->x + dz * caxis->z) / (dl * cl);

                                // [NUR GEGNER 2026-08-12] Der Schlauch gilt
                                // ausschliesslich fuer Gegner -- auf Breakables
                                // angewandt bekam jede nahe Kiste sofort fall=1.
                                const float mlat =
                                    tctx != nullptr ? kfc("assist_min_lat", 0.0) : 0.0f;

                                if (mlat > 0.0f) {
                                    const float tlat =
                                        dl * std::sqrt(std::max(0.0f, 1.0f - tdot * tdot));
                                    const float ro = std::max(
                                        mlat, dl * std::tan(outer * glm::pi<float>() / 180.0f));
                                    const float ri = std::max(
                                        mlat * (inner / std::max(1e-6f, outer)),
                                        dl * std::tan(inner * glm::pi<float>() / 180.0f));

                                    if (tdot <= 0.0f) {
                                        fall = 0.0f;
                                    } else if (tlat <= ri) {
                                        fall = 1.0f;
                                    } else if (tlat >= ro) {
                                        fall = 0.0f;
                                    } else {
                                        fall = (ro - tlat) / std::max(1e-6f, ro - ri);
                                    }
                                } else {
                                    const float ic =
                                        std::cos(inner * glm::pi<float>() / 180.0f);
                                    const float oc =
                                        std::cos(outer * glm::pi<float>() / 180.0f);

                                    if (tdot >= ic) {
                                        fall = 1.0f;
                                    } else if (tdot <= oc) {
                                        fall = 0.0f;
                                    } else {
                                        fall = (tdot - oc) / std::max(1e-6f, ic - oc);
                                    }
                                }
                            }
                        }

                        if (have_tp && fall > 0.0f) {
                            re4vr::lua_set_number("__re4_knife_assist_flat",
                                                  kfc("assist_flatten", 0.0) * fall);
                            re4vr::lua_set_vec3("__re4_knife_home", tp);
                            re4vr::lua_set_managed_object("__re4_knife_home_ctx", tctx);
                            re4vr::lua_set_number("__re4_knife_home_str", homing * fall);

                            // [HAND-URSPRUNG] Der Abwurf-Nudge lenkt die
                            // FLUGRICHTUNG -> ab der HAND rechnen, NICHT ab dem
                            // Auge: sonst zeigt dir durch die Hoehendifferenz
                            // nach unten und das Messer geht sofort zu Boden.
                            const glm::vec3 hand = knife_hand_world().value_or(*origin);
                            glm::vec3 t = tp - hand;
                            const float tl = glm::length(t);

                            if (tl > 0.001f) {
                                t /= tl;
                                const float s = glm::clamp(strg * fall, 0.0f, 1.0f);
                                const glm::vec3 n = dir + (t - dir) * s;
                                const float nl = glm::length(n);

                                if (nl > 0.001f) {
                                    dir = n / nl;
                                }
                            }
                        }
                    }
                }

                // [WURF-SOUND LINKS 2026-09-08] play_knife_sound geht ueber den
                // SoundContainer der ECHTEN Waffe -- den hat der linke Klon nicht
                // (RE4VRWeapons2: "Der Klon hat keinen eigenen SoundContainer"),
                // deshalb blieb der Wurf links stumm, waehrend Flip und Melee
                // ueber __re4_knife_lh_play_sound hoerbar waren. Gleiche ID, nur
                // ueber den Klon-Weg -- dasselbe Muster wie beim Treffer-Sound
                // weiter oben.
                const double throw_snd =
                    re4vr::lua_get_table_number("__re4_knife_snd", "throw", 3788596668.0);

                // Beide Wege ueber uint32_t: die Wurf-ID liegt ueber 2^31.
                if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
                    lua_call_global_void("__re4_knife_lh_play_sound", std::floor(throw_snd));
                } else {
                    play_knife_sound(
                        static_cast<int32_t>(static_cast<uint32_t>(throw_snd)));
                }

                knife_throw_launch(dir, std::nullopt);   // fester cfg.speed
            }

            m_kthrow.peak = 0.0f;
        }

        m_kthrow.was_gripping = gripping;
    } else {
        re4vr::lua_set_bool("__re4_knife_throw_gripping", false);
        m_kthrow.was_gripping = false;
    }

    // ---- [KNIFE_MELEE] auf die vr_knife_swing-Flanke ---------------------
    // [CHOKE IST GETRENNT 2026-08-29] Waehrend eines Wuergegriffs gehoert der
    // Stich AUSSCHLIESSLICH re4_vr_choke. Das freie Melee lief bisher PARALLEL
    // mit -- und weil es keine Abwaertsbedingung kennt, machte es auch beim
    // Hochziehen Schaden (gemessen: drei Treffer a -225 in 0,33 s).
    // __re4_choke_seen ist der Herzschlag; stirbt das Choke-Script mitten im
    // Griff, verfaellt die Sperre nach 0,2 s von selbst.
    const bool choke_live =
        (now - re4vr::lua_get_number("__re4_choke_seen", -999.0)) < 0.2;

    if (re4vr::lua_get_tribool("vr_knife_swing") == 1) {
        if (!m_knife_melee_prev) {
            m_knife_melee_prev = true;

            if (knife_equ && !m_kfly.active && !choke_live) {
                play_knife_sound(static_cast<int32_t>(re4vr::lua_get_table_number(
                    "__re4_knife_snd", "swing", 1800445513.0)));
                do_knife_melee();
            }
        }
    } else {
        m_knife_melee_prev = false;
    }

    // ---- [THROWABLES] RE9 Release-to-Throw --------------------------------
    if (m_tcfg.enabled && is_grenade_equipped()) {
        re4vr::lua_set_bool("__re4_throw_apply", true);

        // native Granaten-Ziellinie aus (wir werfen selbst)
        re4vr::lua_set_bool("__re4_suppress_throwsight", true);
        throwsight_force_off();

        // [HOLSTER] Hand am Granaten-Dummy -> R-Grip = grab, NICHT Wurf-Windup.
        // grip/zone einzeln halten: kommt die Hand beim Ausholen in die
        // Dummy-Zone, fiele aiming MITTEN im Windup weg.
        const bool grip_now = [] {
            auto& vr = VR::get();

            if (vr == nullptr || !vr->is_hmd_active()) {
                return false;
            }

            const auto act = vr->get_action_grip();
            const auto rj = vr->get_right_joystick();

            if (act == vr::k_ulInvalidActionHandle) {
                return false;
            }

            try {
                return vr->is_action_active(act, rj);
            } catch (...) {
                return false;
            }
        }();

        const bool zone_now = re4vr::lua_get_tribool("__vr_grenade_holster_zone") == 1;
        const bool aiming = grip_now && !zone_now;

        if (aiming) {
            update_throw_velocity(true);   // [GRANATE] immer die rechte Hand

            glm::vec3 d{};

            if (get_throw_direction(m_tcfg.grenade_pitch, 0.0f, d)) {
                re4vr::lua_set_vec3("__re4_throw_dir", d);
            }

            if (get_throw_direction(0.0f, 0.0f, d)) {
                re4vr::lua_set_vec3("__re4_throw_dir_raw", d);
            }

            m_tfsm_peak = compute_release_window_peak();

            // [WURF-FENSTER 2026-07-23] Schulter-Holster-Grab sperren.
            // ZEITBASIERT -> schliesst sich von selbst, kann nicht haengen.
            re4vr::lua_set_number("__vr_throw_windup_until", now + 0.40);
        }

        // LOSLASSEN = Wurf
        if (m_tfsm_was_aiming && !aiming) {
            const auto dir = re4vr::lua_get_vec3("__re4_throw_dir");

            bool fwd_ok = true;

            {
                const auto draw = re4vr::lua_get_vec3("__re4_throw_dir_raw");
                const auto fwd = get_char_forward();

                if (draw.has_value() && fwd.has_value()) {
                    const float fl = glm::length(*fwd);

                    if (fl > 0.001f) {
                        fwd_ok = glm::dot(*draw, *fwd) / fl > 0.15f;
                    }
                }
            }

            if (fwd_ok && m_tfsm_peak >= m_tcfg.hand_speed_min
                && (now - m_last_grenade_throw_t) >= m_tcfg.cooldown && dir.has_value()) {
                // [FESTER WURF] feste Geschwindigkeit wie das Messer.
                const float speed = m_tcfg.throw_fixed_speed;

                re4vr::lua_set_vec3("__re4_throw_vel", *dir * speed);
                re4vr::lua_set_bool("__re4_throw_pending", true);
                re4vr::lua_set_number("__re4_throw_apply_until", now + 3.0);
                re4vr::lua_set_number("__re4_throw_last_t", now);

                // [GRANATENFLUG] Dieselben Werte nativ -- die GrenadeShell-
                // Hooks lesen sie im Spiel-Thread und duerfen dort nicht an
                // den Lua-State (ScriptRunner-Sperre).
                m_throw_vel = *dir * speed;
                m_throw_vel_pending = true;
                m_throw_apply_until = now + 3.0;
                m_throw_vel_last_t = now;

                // RT -> nativer Wurf spawnt die Shell (einziger Weg)
                re4vr::lua_set_bool("vr_grenade_throw", true);

                m_last_grenade_throw_t = now;
                m_grenade_throw_reset_t = now + 0.20;
                m_grenade_ff_until = now + 1.5;
            }

            m_tfsm_peak = 0.0f;
        }

        m_tfsm_was_aiming = aiming;

        if (!aiming) {
            re4vr::lua_set_nil("__re4_throw_dir");
        }
    } else {
        re4vr::lua_set_nil("__re4_throw_apply");
        re4vr::lua_set_nil("__re4_throw_dir");
        re4vr::lua_set_bool("__re4_suppress_throwsight", false);
        m_tfsm_was_aiming = false;
    }

    if (re4vr::lua_get_tribool("vr_grenade_throw") == 1 && m_grenade_throw_reset_t > 0.0
        && now >= m_grenade_throw_reset_t) {
        re4vr::lua_set_bool("vr_grenade_throw", false);
        m_grenade_throw_reset_t = 0.0;
    }
}

// ============================================================================
// Pass-Registrierungen (Lua Z.1430-1469 + 3881-3885)
// ============================================================================

void RE4VRWeapons::on_pre_application_entry(void*, const char* name, size_t) {
    // [SCRIPTGATE -- ergaenzt 07.09.2026] Riegel zu = dieses Modul ist so still,
    // als waere seine Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    // Fehlte hier komplett: waehrend der Gondelfahrt schaltet RE4VRObjects alle
    // Scripte ab AUSSER objects/binding/materials/firstperson (BLEIBT-Liste) --
    // re4_vr_weapons.lua und re4_vr_weapons2.lua gehoerten nie dazu, liefen im
    // Port aber weiter.
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr || !m_inited) {
        return;
    }

    const std::string_view n{name};

    if (n == "LockScene") {
        hide_body_weapons_tick();
        scope_proto_tick();

        // [IRON_SIGHT_NATIVE] Body/Mesh/Parent NACH dem Game-Hiding forcen.
        if (m_iron_active_wid.has_value()) {
            iron_sight_tick(*m_iron_active_wid);
        }

        knife_flight_apply();

        return;
    }

    if (n == "UpdateScene") {
        // Force-Flag jeden Frame frisch setzen (dort wertet auch der
        // Killswitch aus) -> Scope-Austritt wird sicher erkannt.
        scope_killswitch_tick();

        return;
    }

    if (n == "BeginRendering") {
        weapon_switch_skip_tick();
        knife_flight_apply();
    }
}

void RE4VRWeapons::on_application_entry(void*, const char* name, size_t) {
    // [SCRIPTGATE -- ergaenzt 07.09.2026] Riegel zu = dieses Modul ist so still,
    // als waere seine Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    // Fehlte hier komplett: waehrend der Gondelfahrt schaltet RE4VRObjects alle
    // Scripte ab AUSSER objects/binding/materials/firstperson (BLEIBT-Liste) --
    // re4_vr_weapons.lua und re4_vr_weapons2.lua gehoerten nie dazu, liefen im
    // Port aber weiter.
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr || !m_inited) {
        return;
    }

    const std::string_view n{name};

    if (n == "LateUpdateBehavior") {
        // After behavior / motion, damit body-driven assist rotation die
        // HMD-Ausrichtung nicht ueberschreibt.
        sync_assist_light_rotation_to_vr_cam();
        snappy_wep_tick();
        weapon_switch_skip_tick();
        knife_flight_apply();

        return;
    }

    if (n == "UpdateMotion") {
        weapon_switch_skip_tick();

        return;
    }

    if (n == "UpdateJointExpression") {
        weapon_switch_skip_tick();
        knife_flight_apply();

        return;
    }

    if (n == "BeginRendering") {
        // BeginRendering-POST ist der WIRKSAME Pass des Override-Stacks.
        knife_flight_apply();
    }
}

// ============================================================================
// Rahmen: Init, Lua-Exporte, Reset, UI
// ============================================================================

std::optional<std::string> RE4VRWeapons::on_initialize() {
    // ---- Configs laden (Lua: beim Chunk-Load) ----
    if (const auto d = re4vr::json_load(HIDE_BODY_CFG_PATH); d.is_object()) {
        if (const auto it = d.find("enabled"); it != d.end() && !it->is_null()) {
            m_hide_body_enabled = it->is_boolean() && it->get<bool>();
        }
    }

    // [PORTFIX 2026-09-06] Wert nur MERKEN, nicht hier nach Lua schreiben:
    // on_initialize laeuft auf dem Init-Thread, der Lua-State entsteht erst
    // im on_frame des ScriptRunners -- lua_set_* ist hier ein stiller No-Op
    // und __re4_bolt_reaim existierte nie. Veroeffentlicht wird in
    // on_lua_state_created. Gleiche Falle wie in RE4VRWeapons2 (LCFG) und
    // RE4VRMotion (load_config).
    m_bolt_reaim = true;

    if (const auto d = re4vr::json_load(BOLT_CFG_PATH); d.is_object()) {
        if (const auto it = d.find("reaim"); it != d.end() && it->is_boolean()) {
            m_bolt_reaim = it->get<bool>();
        }
    }

    if (const auto d = re4vr::json_load(SCOPE_PROTO_CFG_PATH); d.is_object()) {
        const auto n = [&](const char* k, float& dst) {
            if (const auto it = d.find(k); it != d.end() && it->is_number()) {
                dst = it->get<float>();
            }
        };

        n("scale", m_scope_proto.scale);
        n("ox", m_scope_proto.ox);
        n("oy", m_scope_proto.oy);
        n("oz", m_scope_proto.oz);
    }

    if (const auto d = re4vr::json_load(SNAPPY_CFG_PATH); d.is_object()) {
        if (const auto it = d.find("enabled"); it != d.end() && !it->is_null()) {
            m_snappy.enabled = it->is_boolean() && it->get<bool>();
        }

        if (const auto it = d.find("frameskip"); it != d.end() && it->is_number()) {
            m_snappy.frameskip = it->get<float>();
        }

        if (const auto it = d.find("fast_re_aim"); it != d.end() && !it->is_null()) {
            m_snappy.fast_re_aim = it->is_boolean() && it->get<bool>();
        }

        if (const auto it = d.find("direct_snap"); it != d.end() && !it->is_null()) {
            m_snappy.direct_snap = it->is_boolean() && it->get<bool>();
        }
    }

    // TCFG: Lua uebernimmt nur Werte GLEICHEN TYPS.
    if (const auto d = re4vr::json_load(THROW_CFG_PATH); d.is_object()) {
        const auto n = [&](const char* k, float& dst) {
            if (const auto it = d.find(k); it != d.end() && it->is_number()) {
                dst = it->get<float>();
            }
        };

        if (const auto it = d.find("enabled"); it != d.end() && it->is_boolean()) {
            m_tcfg.enabled = it->get<bool>();
        }

        n("hand_speed_min", m_tcfg.hand_speed_min);
        n("hand_speed_max", m_tcfg.hand_speed_max);
        n("throw_fixed_speed", m_tcfg.throw_fixed_speed);
        n("throw_speed_min", m_tcfg.throw_speed_min);
        n("throw_speed_max", m_tcfg.throw_speed_max);
        n("cooldown", m_tcfg.cooldown);
        n("forward_dot_min", m_tcfg.forward_dot_min);
        n("sensitivity_min", m_tcfg.sensitivity_min);
        n("sensitivity_max", m_tcfg.sensitivity_max);
        n("y_offset", m_tcfg.y_offset);
        n("x_offset", m_tcfg.x_offset);
        n("release_window", m_tcfg.release_window);
        n("knife_pitch", m_tcfg.knife_pitch);
        n("grenade_pitch", m_tcfg.grenade_pitch);
        n("knife_yaw", m_tcfg.knife_yaw);
        n("throw_pitch", m_tcfg.throw_pitch);
        n("gravity", m_tcfg.gravity);
        n("throw_speed_mult", m_tcfg.throw_speed_mult);
    }

    return Mod::on_initialize();
}

bool RE4VRWeapons::ensure_init() {
    if (m_inited) {
        return true;
    }

    m_inited = true;

    if (!m_hooks_installed) {
        m_hooks_installed = true;

        install_equip_hooks();
        install_hitctrl_hooks();
        install_shell_hook();
        install_throwsight_hooks();
        install_grenade_throw_hooks();   // [GRANATENFLUG] s. dort
    }

    return true;
}

void RE4VRWeapons::on_lua_state_created(sol::state& lua) {
    // [PORTFIX 2026-09-06] s. on_initialize -- erst hier lebt der Lua-State.
    re4vr::lua_set_bool("__re4_bolt_reaim", m_bolt_reaim);

    // ---- Werte-Globals, die ein "Reset Scripts" ueberleben sollen ----
    // Lua: `_G.x = _G.x or <default>` -- also nur setzen, wenn nichts da ist.
    const auto seed_num = [&](const char* name, double def) {
        if (!re4vr::lua_get_number_opt(name).has_value()) {
            re4vr::lua_set_number(name, def);
        }
    };

    seed_num("__re4_knife_reach", KNIFE_REACH_DEF);
    seed_num("__re4_knife_touch", 0.90);
    seed_num("__re4_knife_dmg_override", 150.0);
    seed_num("__re4_knife_wince_override", 90.0);
    seed_num("__re4_knife_throw_threshold", 4.0);

    // [KNIFE_SND] IDs tunebar. swing = beim Schwingen (nicht Wurf).
    re4vr::lua_seed_table_number("__re4_knife_snd", "swing", 1800445513.0);
    re4vr::lua_seed_table_number("__re4_knife_snd", "throw", 3788596668.0);
    re4vr::lua_seed_table_number("__re4_knife_snd", "hit", 686504397.0);
    re4vr::lua_seed_table_number("__re4_knife_snd", "floor", 643584649.0);

    // [FLEISCH 2026-08-26] Bis dahin lief JEDER Wurf-Gegnertreffer ueber `hit`
    // -- den Wand/Objekt-Einschlag. `flesh` ist dieselbe ID, die das linke
    // Messer beim Stich benutzt.
    re4vr::lua_seed_table_number("__re4_knife_snd", "flesh", 238304172.0);

    // [KNIFE_THROW / FLIGHT] Die komplette Flug-Config. Alle Werte ueberleben
    // einen Reset (Lua: `cfg.x = cfg.x or ...`).
    {
        const auto s = [&](const char* k, double v) {
            re4vr::lua_seed_table_number("__re4_knife_fly_cfg", k, v);
        };

        s("speed", 12.0);
        s("gravity", 7.0);
        s("spin", 12.0);
        s("max_time", 1.5);
        s("return_delay", 0.4);
        s("hit_radius", 0.5);
        s("break_radius", 0.9);
        s("v_min", 4.0);
        s("v_max", 14.0);
        s("spin_fly", 5.0);
        s("spin_fall", 22.0);
        s("arm_clear", 0.7);
        s("close_hit_radius", 0.5);
        s("spin_ax", 1.0);
        s("spin_ay", 0.0);
        s("spin_az", 0.0);
        s("spin_ax_l", 1.0);
        s("spin_ay_l", 0.0);
        s("spin_az_l", 0.0);
        s("dir_max_deg", 90.0);
        s("hmd_yaw", 0.0);
        s("hmd_pitch", 0.0);
        s("assist_cone_inner_deg", 8.0);
        s("assist_target_off_y", 0.0);
        s("assist_strength", 0.0);
        s("assist_homing", 0.05);
        s("assist_cone_deg", 20.0);
        s("assist_min_lat", 0.6);
        s("hit_return_delay", 0.7);
        s("hit_visible", 0.15);
        s("stick_in", 0.05);
        s("stick_blade_len", 0.22);
        s("stick_snap", 0.10);
        s("assist_flatten", 0.0);
        s("max_range", 12.0);
        s("animal_range", 8.0);
        s("animal_max_dy", 1.0);
        s("hit_max_dy", 1.0);
        s("hit_margin", 0.15);
        s("animal_touch", 0.35);

        // [MIGRATION] Der Zwischenstand 0.12 wird nachgezogen; ein selbst
        // eingestellter Wert bleibt unangetastet.
        if (re4vr::lua_get_table_number("__re4_knife_fly_cfg", "hit_return_delay", 0.7)
            == 0.12) {
            re4vr::lua_set_table_number("__re4_knife_fly_cfg", "hit_return_delay", 0.7);
        }

        if (re4vr::lua_get_table_tribool("__re4_knife_fly_cfg", "hmd_force") == -1) {
            re4vr::lua_set_table_bool("__re4_knife_fly_cfg", "hmd_force", false);
        }
    }

    // [LANDE_POSE] Welt-Rotation des gelandeten Messers (rad).
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "rx", 1.5708);
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "ry", 0.0);
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "rz", 0.0);

    // [GRENADE FAST-FORWARD]
    re4vr::lua_seed_table_number("__re4_gren_ff", "id", 1400.0);
    re4vr::lua_seed_table_number("__re4_gren_ff", "frame", 57.0);
    re4vr::lua_seed_table_number("__re4_gren_ff", "speed", 6.0);

    if (re4vr::lua_get_table_tribool("__re4_gren_ff", "enabled") == -1) {
        re4vr::lua_set_table_bool("__re4_gren_ff", "enabled", true);
    }

    // Schalter, die in Lua beim Laden gesetzt werden.
    if (re4vr::lua_get_tribool("__re4_knife_stick_on") == -1) {
        re4vr::lua_set_bool("__re4_knife_stick_on", true);
    }

    if (re4vr::lua_get_tribool("__re4_break_snd_inhand") == -1) {
        re4vr::lua_set_bool("__re4_break_snd_inhand", true);
    }

    if (re4vr::lua_get_tribool("__re4_knife_dir_check") == -1) {
        re4vr::lua_set_bool("__re4_knife_dir_check", true);
    }

    re4vr::lua_set_bool("__re4_qk_block", true);
    re4vr::lua_set_number("__re4_qk_blocked", 0.0);
    re4vr::lua_set_number("__re4_qk_equip_blocked", 0.0);
    re4vr::lua_set_bool("__re4_stow_block", true);
    re4vr::lua_set_number("__re4_stow_blocked", 0.0);

    // ---- Funktions-Globals, die weapons2 und der Reload-Block noch rufen ----
    lua["__re4_find_knife_hc"] = [&lua]() -> sol::object {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr) {
            return sol::nil;
        }

        auto* hc = s->find_knife_hc();

        return hc != nullptr ? sol::make_object(lua, hc) : sol::nil;
    };

    lua["__re4_knife_get_attack_ud"] = [&lua](sol::object hc) -> sol::object {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr || !hc.valid() || hc.get_type() != sol::type::userdata) {
            return sol::nil;
        }

        auto* ud = s->knife_get_attack_ud(hc.as<::REManagedObject*>());

        return ud != nullptr ? sol::make_object(lua, ud) : sol::nil;
    };

    lua["__re4_break_nearby"] = [](sol::object kp, sol::object radius,
                                   sol::object max_dy) -> int32_t {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr) {
            return 0;
        }

        std::optional<glm::vec3> p{};
        std::optional<float> r{};
        std::optional<float> m{};

        if (kp.is<glm::vec3>()) {
            p = kp.as<glm::vec3>();
        }

        if (radius.is<double>()) {
            r = static_cast<float>(radius.as<double>());
        }

        if (max_dy.is<double>()) {
            m = static_cast<float>(max_dy.as<double>());
        }

        return s->break_nearby(p, r, m);
    };

    lua["__re4_nearest_bone_dist"] = [](sol::object tf, sol::object kp) -> double {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr || !tf.valid() || tf.get_type() != sol::type::userdata
            || !kp.is<glm::vec3>()) {
            return 999.0;
        }

        return static_cast<double>(
            s->nearest_bone_dist(tf.as<::REManagedObject*>(), kp.as<glm::vec3>()));
    };

    lua["__re4_do_knife_melee"] = []() {
        if (auto& s = RE4VRWeapons::get(); s != nullptr) {
            s->do_knife_melee();
        }
    };

    lua["__re4_los_clear"] = [](sol::object from, sol::object to) -> bool {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr || !from.is<glm::vec3>() || !to.is<glm::vec3>()) {
            return true;   // fail-open
        }

        return s->los_clear(from.as<glm::vec3>(), to.as<glm::vec3>());
    };

    lua["__re4_knife_hand_world"] = [&lua]() -> sol::object {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr) {
            return sol::nil;
        }

        const auto p = s->knife_hand_world();

        return p.has_value() ? sol::make_object(lua, *p) : sol::nil;
    };

    lua["__re4_knife_dir_ok"] = [](sol::object target) -> bool {
        auto& s = RE4VRWeapons::get();

        if (s == nullptr || !target.valid() || target.get_type() != sol::type::userdata) {
            return true;
        }

        return s->knife_dir_ok(target.as<::REManagedObject*>());
    };
}

void RE4VRWeapons::on_lua_state_destroyed(sol::state&) {
    // [SNAPPY_WEP] FSM-Tree-Edits zuruecknehmen (der Tree ueberlebt einen
    // Script-Reload).
    if (m_snappy_applied) {
        apply_snappy(true);
    }

    m_snappy_applied = false;
    m_snappy_last_body.reset();

    drop(m_character_manager);
    m_al.body_addr.reset();
    drop(m_al.go);
    drop(m_al.tf);

    m_kh.force_frames = 0;
    m_kh.original_time_limit.reset();

    // [WURF-KLON] Selbst erzeugtes GameObject -- es ueberlebt den Reset sonst
    // sichtbar in der Szene, ohne dass noch jemand einen Griff daran hat.
    knife_stick_clone_drop();

    m_inited = false;
}

// ============================================================================
// [PUBLIC-UI] Derselbe Schalter wie "Hide Assist Light" im Dev-Tree, nur unter
// dem Public-Namen "Disable Assist Light" -- geschrieben wird dieselbe Variable.
// 1:1 aus re4_vr_weapons.lua Z.1324-1330. Hinweis wie dort: der Wert wird
// nirgends gespeichert (auch im Dev-Tree nicht) und steht nach einem Neustart
// wieder auf AUS.
// ============================================================================
void RE4VRWeapons::draw_public_assist_light() {
    g_framework->draw_menu_checkbox("Disable Assist Light", &m_hide_assist_light);
}

void RE4VRWeapons::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Weapons")) {
        return;
    }

    {
        bool v = re4vr::lua_get_tribool("__re4_bolt_reaim") != 0;

        if (ImGui::Checkbox("Repetierer: nach dem Schuss kurz neu anvisieren", &v)) {
            re4vr::lua_set_bool("__re4_bolt_reaim", v);

            nlohmann::json d{};
            d["reaim"] = v;
            re4vr::json_save(BOLT_CFG_PATH, d);
        }
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Ruecken-/Holster-Waffen + Messertasche verstecken",
                        &m_hide_body_enabled)) {
        save_hide_body_cfg();
    }

    if (ImGui::Checkbox("Schneller Waffenwechsel (Snappy)", &m_snappy.enabled)) {
        m_snappy_applied = false;   // naechster Tick wendet an/revertiert
        save_snappy_cfg();
    }

    if (m_snappy.enabled) {
        if (ImGui::Checkbox("Direkt-Snap (instant, alle Waffen)", &m_snappy.direct_snap)) {
            m_snappy_applied = false;
            save_snappy_cfg();
        }

        if (!m_snappy.direct_snap) {
            if (ImGui::SliderFloat("Weapon Frameskip", &m_snappy.frameskip, 0.0f, 60.0f,
                                   "%.0f")) {
                m_snappy_applied = false;
                save_snappy_cfg();
            }
        }
    }

    if (ImGui::Checkbox("Fast Re-Aim (schnelleres Wieder-Anvisieren)",
                        &m_snappy.fast_re_aim)) {
        m_snappy_applied = false;
        save_snappy_cfg();
    }

    ImGui::Checkbox("Hide Assist Light", &m_hide_assist_light);
    ImGui::Checkbox("Assist light follows VR view (HMD)", &m_assist_light_follow_vr_cam);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Aim-assist spotlight matches VR view: game camera yaw (right stick) x headset "
            "rotation, same basis as docked flashlight in motion.");
    }

    // [SCOPE_PROTO] Scope-Linsen-Test (nur im Scope wirksam)
    ImGui::Separator();
    ImGui::TextColored(ImVec4{1.0f, 1.0f, 0.67f, 1.0f},
                       "Scope-Test (Weg A): Waffen-Mesh im Scope skalieren/versetzen");

    bool sp_c = false;
    sp_c |= ImGui::DragFloat("Scope Mesh-Skalierung##spsc", &m_scope_proto.scale, 0.01f, 0.2f,
                             12.0f, "%.2f");
    sp_c |= ImGui::DragFloat("Scope Offset X##spox", &m_scope_proto.ox, 0.001f, -1.0f, 1.0f,
                             "%.4f");
    sp_c |= ImGui::DragFloat("Scope Offset Y##spoy", &m_scope_proto.oy, 0.001f, -1.0f, 1.0f,
                             "%.4f");
    sp_c |= ImGui::DragFloat("Scope Offset Z##spoz", &m_scope_proto.oz, 0.001f, -1.0f, 1.0f,
                             "%.4f");

    if (sp_c) {
        save_scope_proto();
    }

    if (ImGui::Button("Scope-Test zuruecksetzen##spreset")) {
        m_scope_proto = ScopeProto{};
        save_scope_proto();
    }

    // [FESTER WURF] Granaten-Regler
    ImGui::Separator();

    bool tc = false;
    tc |= ImGui::SliderFloat("Granate: Wurfgeschwindigkeit (m/s)", &m_tcfg.throw_fixed_speed,
                             1.0f, 25.0f, "%.2f");
    tc |= ImGui::SliderFloat(
        "Granate: Wurf-Empfindlichkeit (Schwung m/s, klein=leichter)",
        &m_tcfg.hand_speed_min, 1.0f, 12.0f, "%.2f");
    tc |= ImGui::SliderFloat("Granate: Y-Korrektur (auf/ab)", &m_tcfg.grenade_pitch, -0.8f,
                             0.8f, "%.3f");

    if (tc) {
        save_throw_cfg();
    }

    ImGui::TreePop();
}

#endif
