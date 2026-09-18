// ============================================================================
// RE4VRScope -- 1:1-Portierung von re4_vr_scope.lua
// Spezifikation: I:\LUATRANS\PORT_SCOPE_SPEC.md (samt NACHTRAG 03.09.2026)
//
// Zeilenverweise in den Kommentaren beziehen sich auf die Lua-Datei, Stand
// 03.09.2026 (872 Zeilen).
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../VR.hpp"
#include "RE4VR.hpp"
#include "RE4VRScope.hpp"

#if defined(RE4)

namespace {

// Lua: os.clock(). [Nachtrag L3] Das ist CPU-Zeit, NICHT die Wanduhr. Luas
// os.clock ist in derselben DLL clock()/CLOCKS_PER_SEC -- identische Epoche,
// direkt vergleichbar. Wer hier steady_clock nimmt, laesst die beiden
// 0,5-s-Drosseln unter Last anders feuern als das Original.
double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

sdk::REMethodDefinition* find_method(::REManagedObject* obj, std::string_view name) {
    if (obj == nullptr) {
        return nullptr;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);
    return def != nullptr ? def->get_method(name) : nullptr;
}

bool clear_pending(sdk::VMContext* context, bool ok) {
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        return false;
    }

    return ok;
}

// via.vec3 / via.Quaternion sind ValueTypes > 8 Byte -> sret-Pfad mit
// 16-Byte-ausgerichtetem Puffer. Ein call_safe<T> mit falschem T legte den
// Puffer nach RCX, wo die Engine den VMContext erwartet.
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

bool get_quat(::REManagedObject* obj, std::string_view name, glm::quat& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::quat buf{1.0f, 0.0f, 0.0f, 0.0f};
    bool ok = false;

    try {
        method->call_safe<glm::quat*>(&buf, context, obj);
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

bool set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, 1.0f};
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

bool set_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::quat buf = q;
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// get_Name eines GameObjects als std::string. Leerer String = Luas nil.
std::string name_of(::REManagedObject* o) {
    if (o == nullptr) {
        return {};
    }

    auto* n = re4vr::call_safe<::REManagedObject*>(o, "get_Name");

    if (n == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
    } catch (...) {
        return {};
    }
}

// [CONTAINER-FLAG] Das zweite Argument von get_data_raw beschreibt den
// CONTAINER, nicht den Feldtyp (RETypeDB.cpp:344-363):
//   true  -> get_offset_from_fieldptr()  (roher Struct-Zeiger)
//   false -> get_offset_from_base()      (Managed Object, i.d.R. +0x10)
// Diese Kette liefert BEIDES, deshalb wird das Flag durchgereicht.
bool field_target(void* obj, sdk::RETypeDefinition* obj_td, const char* name, void*& out_ptr,
                  sdk::RETypeDefinition*& out_td, bool container_is_value, bool& out_is_value) {
    if (obj == nullptr || obj_td == nullptr) {
        return false;
    }

    auto* f = obj_td->get_field(name);

    if (f == nullptr) {
        return false;
    }

    auto* ft = f->get_type();

    if (ft == nullptr) {
        return false;
    }

    auto* raw = f->get_data_raw(obj, container_is_value);

    if (raw == nullptr) {
        return false;
    }

    if (ft->is_value_type()) {
        out_ptr = raw;             // die Struct liegt IM Objekt
        out_td = ft;
        out_is_value = true;
        return true;
    }

    auto* obj2 = *reinterpret_cast<::REManagedObject**>(raw);

    if (obj2 == nullptr) {
        return false;
    }

    out_ptr = obj2;
    out_td = utility::re_managed_object::get_type_definition(obj2);
    out_is_value = false;
    return out_td != nullptr;
}

// Ein ganzzahliges Feld lesen, das auch ein Enum sein kann. REFramework packt
// Enums nach Lua auf ihren Underlying-Typ aus (Sdk.cpp:860-868), get_field
// liefert dort also direkt eine Zahl -- genau das hier nachgebaut, aber mit der
// tatsaechlichen Groesse statt fest int32.
std::optional<int64_t> read_int_field(void* obj, sdk::RETypeDefinition* def, const char* name,
                                      bool container_is_value) {
    if (obj == nullptr || def == nullptr) {
        return std::nullopt;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return std::nullopt;
    }

    auto* ft = f->get_type();

    if (ft == nullptr) {
        return std::nullopt;
    }

    auto* raw = f->get_data_raw(obj, container_is_value);

    if (raw == nullptr) {
        return std::nullopt;
    }

    const auto size = ft->get_valuetype_size();

    try {
        switch (size) {
        case 1:
            return static_cast<int64_t>(*reinterpret_cast<int8_t*>(raw));
        case 2:
            return static_cast<int64_t>(*reinterpret_cast<int16_t*>(raw));
        case 4:
            return static_cast<int64_t>(*reinterpret_cast<int32_t*>(raw));
        case 8:
            return *reinterpret_cast<int64_t*>(raw);
        default:
            return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}

// Ein bool-Feld lesen. Dreiwertig, weil Lua zwischen "false" und "nicht da"
// unterscheidet: in Lua ist x == true bei nil FALSCH, x ~= false dagegen WAHR.
std::optional<bool> read_bool_field(::REManagedObject* obj, const char* name) {
    if (obj == nullptr) {
        return std::nullopt;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return std::nullopt;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return std::nullopt;
    }

    // Container ist ein Managed Object -> false.
    auto* raw = f->get_data_raw(obj, false);

    if (raw == nullptr) {
        return std::nullopt;
    }

    try {
        return *reinterpret_cast<uint8_t*>(raw) != 0;
    } catch (...) {
        return std::nullopt;
    }
}

// Ein float-Feld schreiben -- Luas sp:set_field("_FOVMin", v).
void write_field_float(void* obj, sdk::RETypeDefinition* obj_td, const char* name, float v,
                       bool container_is_value) {
    if (obj == nullptr || obj_td == nullptr) {
        return;
    }

    auto* f = obj_td->get_field(name);

    if (f == nullptr) {
        return;
    }

    auto* raw = f->get_data_raw(obj, container_is_value);

    if (raw == nullptr) {
        return;
    }

    try {
        *reinterpret_cast<float*>(raw) = v;
    } catch (...) {
    }
}

// Lua Z.348-362 / 583-597: Quaternion aus Yaw/Pitch/Roll. Die Reihenfolge der
// Terme ist die des Originals und wird NICHT durch glm::quat(euler) ersetzt --
// das rechnet eine andere Achsenfolge.
// Gerechnet wird in double, weil Lua-Zahlen doubles sind.
struct DQuat {
    double w{1.0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

DQuat quat_from_ypr(double yaw, double pitch, double roll) {
    const double rad = 3.14159265358979323846 / 180.0;
    const double hy = yaw * rad * 0.5;
    const double hp = pitch * rad * 0.5;
    const double hr = roll * rad * 0.5;

    const double cy = std::cos(hy);
    const double sy = std::sin(hy);
    const double cp = std::cos(hp);
    const double sp = std::sin(hp);
    const double cr = std::cos(hr);
    const double sr = std::sin(hr);

    DQuat o{};
    o.w = cy * cp * cr + sy * sp * sr;
    o.x = cy * sp * cr + sy * cp * sr;
    o.y = sy * cp * cr - cy * sp * sr;
    o.z = cy * cp * sr - sy * sp * cr;
    return o;
}

// Hamilton-Produkt o * cur, Term fuer Term wie im Original. KEINE
// Normalisierung -- das Original normalisiert auch nicht.
glm::quat mul_onto(const DQuat& o, const glm::quat& cur) {
    const double cw = static_cast<double>(cur.w);
    const double cx = static_cast<double>(cur.x);
    const double cyy = static_cast<double>(cur.y);
    const double cz = static_cast<double>(cur.z);

    const double nw = o.w * cw - o.x * cx - o.y * cyy - o.z * cz;
    const double nx = o.w * cx + o.x * cw + o.y * cz - o.z * cyy;
    const double ny = o.w * cyy - o.x * cz + o.y * cw + o.z * cx;
    const double nz = o.w * cz + o.x * cyy - o.y * cx + o.z * cw;

    return glm::quat{static_cast<float>(nw), static_cast<float>(nx), static_cast<float>(ny),
                     static_cast<float>(nz)};
}

// Lua Z.32-39.
const std::array<int32_t, 6> SCOPE_WEAPONS = {4400, 4401, 4402, 4202, 6105, 6114};

bool is_scope_weapon(int32_t wid) {
    return std::find(SCOPE_WEAPONS.begin(), SCOPE_WEAPONS.end(), wid) != SCOPE_WEAPONS.end();
}

// Lua Z.379-382.
struct BodyDef {
    const char* root;
    std::array<const char*, 4> children;   // nullptr = Ende
};

const std::array<BodyDef, 2> CHARACTER_BODY = {
    BodyDef{"ch0a0z0_body", {"body", "hair", "head", nullptr}},
    BodyDef{"ch3a8z0_body", {"cha200_00", "cha200_10", "cha200_20", nullptr}},
};

// Lua Z.688-694. Die Werte gelten immer, sobald eine Scope-Waffe in der Hand
// ist; LENS_ON_AIM nur waehrend des Zielens.
struct LensVar {
    const char* name;
    double value;
};

const std::array<LensVar, 3> LENS_ALWAYS = {
    LensVar{"Vignette_Range", -0.845},
    LensVar{"Vignette_Intensity_Max", -0.860},
    LensVar{"Roughness", -0.070},
};

const std::array<LensVar, 1> LENS_ON_AIM = {
    LensVar{"Visibility", 1.710},
};

// Die Kindkette der Engine ist endlich; der Zaehler ist nur eine Notbremse
// gegen einen zerstoerten Ring und liegt weit ueber jeder realen Kindzahl.
constexpr int CHILD_GUARD = 4096;

} // namespace

std::shared_ptr<RE4VRScope>& RE4VRScope::get() {
    static std::shared_ptr<RE4VRScope> inst = std::make_shared<RE4VRScope>();
    return inst;
}

// ============================================================================
// Referenzzaehlung
//
// [REF] Was ueber Frames hinweg gehalten wird, braucht eine eigene Referenz --
// in Lua erledigt das sol beim Ablegen in einer Variablen (add_ref, aber nur
// bei referenceCount > 0). Ohne das zeigt der Cache nach einem Save-Load auf
// freigegebenen Speicher. [Nachtrag L7] Betrifft hier vor allem
// m_body_children: die Liste wird NUR auf der steigenden Flanke gefuellt und
// ueberlebt einen Save-Load mitten in der Scope-Phase.
// ============================================================================

bool RE4VRScope::keep(::REManagedObject* o, RefHandle& out) {
    drop(out);

    if (o == nullptr || !re4vr::obj_ok(o)) {
        return false;
    }

    out.obj = o;

    // Dieselbe Heuristik wie sol_lua_push: "local objects" mit
    // referenceCount <= 0 werden NICHT geref't.
    if (static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        out.reffed = true;
    } else {
        out.reffed = false;
    }

    return true;
}

void RE4VRScope::drop(RefHandle& h) {
    // Nur zurueckgeben, was wir auch genommen haben -- sonst Refcount-Unterlauf
    // und Use-after-free beim naechsten Zugriff.
    if (h.reffed && h.obj != nullptr && re4vr::obj_ok(h.obj)) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

std::optional<std::string> RE4VRScope::on_initialize() {
    return Mod::on_initialize();
}

// ============================================================================
// Konfiguration (Lua Z.50-212)
// ============================================================================

// Lua Z.119-132.
std::string RE4VRScope::get_controller_preference() const {
    const auto j = re4vr::json_load("re4_vr/re4_vr_controller_pref.json");

    if (j.is_object()) {
        const auto it = j.find("controller");

        if (it != j.end() && it->is_string()) {
            return it->get<std::string>();   // "oculus" oder "index"
        }
    }

    return "oculus";   // default fallback
}

// Lua Z.135-138.
std::string RE4VRScope::get_config_path() const {
    return "re4_vr/re4_vr_scoped_" + get_controller_preference() + ".json";
}

// Lua Z.66-72.
RE4VRScope::WeaponCfg& RE4VRScope::get_weapon_cfg(int32_t wid) {
    char key[16]{};
    std::snprintf(key, sizeof(key), "%d", wid);
    // operator[] legt bei Bedarf einen Eintrag mit den Default-Werten an --
    // genau wie Luas `if not weapon_configs[key] then ... default ... end`.
    return m_weapon_configs[std::string{key}];
}

// Lua Z.85-97.
void RE4VRScope::store_active_to_config() {
    if (!m_active_wid.has_value()) {
        return;
    }

    auto& cfg = get_weapon_cfg(*m_active_wid);
    cfg.pos_offset = m_pos_offset;
    cfg.rot_offset = m_rot_offset;
    cfg.scope_pos_offset = m_scope_pos_offset;
    cfg.scope_rot_offset = m_scope_rot_offset;
    cfg.scope_cam_pos = m_scope_cam_pos;
    cfg.scope_cam_rot = m_scope_cam_rot;
    cfg.scope_fov_min = m_scope_fov_min;
    cfg.scope_fov_max = m_scope_fov_max;
    cfg.scope_lerp_speed = m_scope_lerp_speed;
}

// Lua Z.99-111.
void RE4VRScope::load_active_from_config(int32_t wid) {
    const auto& cfg = get_weapon_cfg(wid);
    m_pos_offset = cfg.pos_offset;
    m_rot_offset = cfg.rot_offset;
    m_scope_pos_offset = cfg.scope_pos_offset;
    m_scope_rot_offset = cfg.scope_rot_offset;
    m_scope_cam_pos = cfg.scope_cam_pos;
    m_scope_cam_rot = cfg.scope_cam_rot;
    m_scope_fov_min = cfg.scope_fov_min;
    m_scope_fov_max = cfg.scope_fov_max;
    m_scope_lerp_speed = cfg.scope_lerp_speed;
    m_active_wid = wid;
}

// Lua Z.113-118.
void RE4VRScope::switch_weapon_config(int32_t wid) {
    if (m_active_wid.has_value() && *m_active_wid == wid) {
        return;
    }

    store_active_to_config();
    load_active_from_config(wid);
    m_scope_lerp_t = 0.0;
}

namespace {

double jnum(const nlohmann::json& d, const char* key, double def) {
    if (!d.is_object()) {
        return def;
    }

    const auto it = d.find(key);

    if (it == d.end() || !it->is_number()) {
        return def;
    }

    return it->get<double>();
}

} // namespace

// Lua Z.140-198.
void RE4VRScope::load_config() {
    const auto path = get_config_path();
    const auto f = re4vr::json_load(path);

    if (!f.is_object() || f.empty()) {
        return;   // Luas `if f then`
    }

    // Luas `if f.slider_range_pos then` -- eine 0 ist dort WAHR, deshalb wird
    // hier nur auf "vorhanden und Zahl" geprueft, nicht auf "ungleich 0".
    if (f.contains("slider_range_pos") && f["slider_range_pos"].is_number()) {
        m_slider_range_pos = f["slider_range_pos"].get<double>();
    }

    if (f.contains("slider_range_rot") && f["slider_range_rot"].is_number()) {
        m_slider_range_rot = f["slider_range_rot"].get<double>();
    }

    const auto read_v3 = [](const nlohmann::json& w, const char* key, Vec3Cfg& out) {
        if (!w.contains(key) || !w[key].is_object()) {
            return;
        }

        const auto& v = w[key];
        out.x = jnum(v, "x", 0.0);
        out.y = jnum(v, "y", 0.0);
        out.z = jnum(v, "z", 0.0);
    };

    const auto read_rot = [](const nlohmann::json& w, const char* key, RotCfg& out) {
        if (!w.contains(key) || !w[key].is_object()) {
            return;
        }

        const auto& v = w[key];
        out.yaw = jnum(v, "yaw", 0.0);
        out.pitch = jnum(v, "pitch", 0.0);
        out.roll = jnum(v, "roll", 0.0);
    };

    const bool has_weapons = f.contains("weapons") && f["weapons"].is_object();

    if (has_weapons) {
        for (const auto& [key, wcfg] : f["weapons"].items()) {
            if (!wcfg.is_object()) {
                continue;
            }

            WeaponCfg cfg{};   // default_weapon_cfg()
            read_v3(wcfg, "pos_offset", cfg.pos_offset);
            read_rot(wcfg, "rot_offset", cfg.rot_offset);
            read_v3(wcfg, "scope_pos_offset", cfg.scope_pos_offset);
            read_rot(wcfg, "scope_rot_offset", cfg.scope_rot_offset);

            if (wcfg.contains("scope_lerp_speed") && wcfg["scope_lerp_speed"].is_number()) {
                cfg.scope_lerp_speed = wcfg["scope_lerp_speed"].get<double>();
            }

            read_v3(wcfg, "scope_cam_pos", cfg.scope_cam_pos);
            read_rot(wcfg, "scope_cam_rot", cfg.scope_cam_rot);

            if (wcfg.contains("scope_fov_min") && wcfg["scope_fov_min"].is_number()) {
                cfg.scope_fov_min = wcfg["scope_fov_min"].get<double>();
            }

            if (wcfg.contains("scope_fov_max") && wcfg["scope_fov_max"].is_number()) {
                cfg.scope_fov_max = wcfg["scope_fov_max"].get<double>();
            }

            m_weapon_configs[key] = cfg;
        }
    }

    // Lua Z.184-197: der alte, waffenlose Aufbau. Nur wenn es KEINEN
    // weapons-Block gibt.
    //
    // [1:1-HINWEIS] Im Original bekommen alle sechs Waffen DIESELBE Tabelle
    // (Referenz), hier bekommt jede eine Kopie. Das ist nicht beobachtbar: die
    // aktiven Werte werden ausschliesslich von load_active_from_config gesetzt
    // (eine UI gibt es in dieser Datei nicht mehr), store_active_to_config
    // schreibt also immer genau die Werte zurueck, die es gerade gelesen hat.
    if (!has_weapons && f.contains("pos_offset") && f["pos_offset"].is_object()) {
        WeaponCfg cfg{};
        read_v3(f, "pos_offset", cfg.pos_offset);
        read_rot(f, "rot_offset", cfg.rot_offset);
        read_v3(f, "scope_pos_offset", cfg.scope_pos_offset);
        read_rot(f, "scope_rot_offset", cfg.scope_rot_offset);

        if (f.contains("scope_lerp_speed") && f["scope_lerp_speed"].is_number()) {
            cfg.scope_lerp_speed = f["scope_lerp_speed"].get<double>();
        }

        for (const auto wid : SCOPE_WEAPONS) {
            char key[16]{};
            std::snprintf(key, sizeof(key), "%d", wid);
            m_weapon_configs[std::string{key}] = cfg;
        }
    }
}

// Lua Z.200-210.
//
// [TOT] Diese Funktion hat im Original KEINEN Aufrufer -- der Regler, der sie
// gerufen hat, ist mit der Scope-UI entfernt worden. Sie wird 1:1 mitportiert
// und ebenso wenig gerufen; die JSON wird also nur von Hand geaendert.
void RE4VRScope::save_config() {
    store_active_to_config();

    nlohmann::json weapons = nlohmann::json::object();

    for (const auto& [key, cfg] : m_weapon_configs) {
        nlohmann::json w = nlohmann::json::object();
        w["pos_offset"] = {{"x", cfg.pos_offset.x}, {"y", cfg.pos_offset.y},
                           {"z", cfg.pos_offset.z}};
        w["rot_offset"] = {{"yaw", cfg.rot_offset.yaw}, {"pitch", cfg.rot_offset.pitch},
                           {"roll", cfg.rot_offset.roll}};
        w["scope_pos_offset"] = {{"x", cfg.scope_pos_offset.x}, {"y", cfg.scope_pos_offset.y},
                                 {"z", cfg.scope_pos_offset.z}};
        w["scope_rot_offset"] = {{"yaw", cfg.scope_rot_offset.yaw},
                                 {"pitch", cfg.scope_rot_offset.pitch},
                                 {"roll", cfg.scope_rot_offset.roll}};
        w["scope_cam_pos"] = {{"x", cfg.scope_cam_pos.x}, {"y", cfg.scope_cam_pos.y},
                              {"z", cfg.scope_cam_pos.z}};
        w["scope_cam_rot"] = {{"yaw", cfg.scope_cam_rot.yaw},
                              {"pitch", cfg.scope_cam_rot.pitch},
                              {"roll", cfg.scope_cam_rot.roll}};
        w["scope_fov_min"] = cfg.scope_fov_min;
        w["scope_fov_max"] = cfg.scope_fov_max;
        w["scope_lerp_speed"] = cfg.scope_lerp_speed;
        weapons[key] = w;
    }

    nlohmann::json out = nlohmann::json::object();
    out["weapons"] = weapons;
    out["slider_range_pos"] = m_slider_range_pos;
    out["slider_range_rot"] = m_slider_range_rot;
    re4vr::json_save(get_config_path(), out);
}

// ============================================================================
// Engine-Zugriffe
// ============================================================================

// Lua Z.8-18.
::REManagedObject* RE4VRScope::get_scene() {
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

::REManagedObject* RE4VRScope::find_go_by_name(const char* name) {
    auto* scene = get_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(scene, "findGameObject(System.String)", str);
}

// Lua Z.229-243. Reihenfolge: wp####, wp####_AO, wp####_MC.
::REManagedObject* RE4VRScope::find_weapon_go(int32_t wid) {
    char base[16]{};
    std::snprintf(base, sizeof(base), "wp%04d", wid);

    const std::string b{base};
    const std::string candidates[3] = {b, b + "_AO", b + "_MC"};

    for (const auto& name : candidates) {
        auto* go = find_go_by_name(name.c_str());

        if (go == nullptr) {
            continue;
        }

        // Luas `if safe(get_Valid) then` -- ein GESCHEITERTER Call ist dort nil
        // und damit falsch, ein false ebenso.
        bool valid = false;

        if (re4vr::try_call<bool>(go, "get_Valid", valid) && valid) {
            return go;
        }
    }

    return nullptr;
}

// Lua Z.246-265.
bool RE4VRScope::check_scope_aim() {
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return false;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return false;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return false;
    }

    const auto wid = re4vr::get_current_weapon_id();

    if (wid < 0 || !is_scope_weapon(wid)) {
        return false;
    }

    auto* cc = re4vr::call_safe<::REManagedObject*>(hu, "get_CameraController");

    if (cc == nullptr) {
        return false;
    }

    // Lua Z.262 liest das ueber get_field, NICHT ueber einen Getter.
    ::REManagedObject* busy = nullptr;

    if (auto def = utility::re_managed_object::get_type_definition(cc)) {
        if (auto* f = def->get_field("_BusyCameraController")) {
            try {
                // [M2] NICHT get_data<T> -- das macht *(T*)get_data_raw(obj) OHNE
                // Nullpruefung. Lua bekommt an derselben Stelle ueber parse_data
                // sauber nil. Also selbst pruefen.
                auto* raw = f->get_data_raw(cc, false);
                busy = raw != nullptr ? *reinterpret_cast<::REManagedObject**>(raw) : nullptr;
            } catch (...) {
                busy = nullptr;
            }
        }
    }

    if (busy == nullptr) {
        return false;
    }

    // Luas `... == true`: ein gescheiterter Zugriff (nil) ist NICHT true.
    const auto v = read_bool_field(busy, "_IsViaScope");
    return v.has_value() && *v;
}

// Lua Z.443-460. [TOT] kein Aufrufer.
::REManagedObject* RE4VRScope::get_muzzle_joint_for_weapon(int32_t wid) {
    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return nullptr;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return nullptr;
    }

    for (const wchar_t* jn : {L"vfx_muzzle", L"vfx_muzzle1"}) {
        auto* s = sdk::VM::create_managed_string(jn);

        if (s == nullptr) {
            continue;
        }

        auto* joint = re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", s);

        if (joint != nullptr) {
            return joint;
        }
    }

    return nullptr;
}

// Lua Z.462-480.
::REManagedObject* RE4VRScope::get_scope_camera_transform(int32_t wid) {
    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return nullptr;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return nullptr;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int guard = 0;

    while (child != nullptr && guard < CHILD_GUARD) {
        ++guard;

        auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

        if (cgo != nullptr && name_of(cgo) == "ScopeCamera") {
            return child;   // die Transform der ScopeCamera
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return nullptr;
}

// Lua Z.482-490. [TOT] kein Aufrufer.
::REManagedObject* RE4VRScope::get_scope_camera_joint(int32_t wid) {
    auto* sc_tf = get_scope_camera_transform(wid);

    if (sc_tf == nullptr) {
        return nullptr;
    }

    auto* joints = re4vr::call_safe<::REManagedObject*>(sc_tf, "get_Joints");

    if (joints == nullptr) {
        return nullptr;
    }

    // [ARRAY, KEINE LISTE -- Fund 04.09.2026] Lua Z.486 schreibt `joints[0]`,
    // also das REFramework-Array-Binding. get_Item existiert auf einem
    // System.Array NICHT: der Aufruf lieferte stumm nullptr, der Scope-Joint
    // kam nie an. Siehe re4vr::array_element.
    return re4vr::array_element(joints, 0);
}

// Lua Z.268-298.
bool RE4VRScope::is_scope_zoomed() {
    const auto wid = re4vr::get_current_weapon_id();

    if (wid < 0 || !is_scope_weapon(wid)) {
        return false;
    }

    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return false;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return false;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int guard = 0;

    while (child != nullptr && guard < CHILD_GUARD) {
        ++guard;

        auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

        if (cgo != nullptr && name_of(cgo) == "ScopeCamera") {
            if (m_scope_ctrl_t != nullptr) {
                // [TOTE PRUEFUNG] Luas `safe(get_Valid) ~= false` ist IMMER
                // wahr: safe() liefert bei false wie bei Fehlschlag nil, und
                // nil ~= false. Die Pruefung filtert also nichts -- hier
                // deshalb ebenfalls nicht.
                auto* ctrl = re4vr::get_component(cgo, m_scope_ctrl_t);

                if (ctrl != nullptr) {
                    const auto act = read_bool_field(ctrl, "_IsActive");
                    return act.has_value() && *act;
                }
            }

            return false;
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return false;
}

// Lua Z.617-630.
::REManagedObject* RE4VRScope::get_body_transform() {
    for (const char* name : {"ch0a0z0_body", "ch3a8z0_body"}) {
        auto* go = find_go_by_name(name);

        if (go == nullptr) {
            continue;
        }

        bool valid = false;

        if (re4vr::try_call<bool>(go, "get_Valid", valid) && valid) {
            return re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        }
    }

    return nullptr;
}

// ============================================================================
// Wirkteile
// ============================================================================

// Lua Z.303-372.
void RE4VRScope::apply_ironsight_offset() {
    const double target_t = m_is_zoomed ? 1.0 : 0.0;
    const double dt = 1.0 / 60.0;   // approximate frame time (Lua Z.307)

    if (m_scope_lerp_t < target_t) {
        m_scope_lerp_t = (std::min)(m_scope_lerp_t + dt * m_scope_lerp_speed, 1.0);
    } else if (m_scope_lerp_t > target_t) {
        m_scope_lerp_t = (std::max)(m_scope_lerp_t - dt * m_scope_lerp_speed, 0.0);
    }

    const double t = m_scope_lerp_t;

    const double eff_px = m_pos_offset.x + m_scope_pos_offset.x * t;
    const double eff_py = m_pos_offset.y + m_scope_pos_offset.y * t;
    const double eff_pz = m_pos_offset.z + m_scope_pos_offset.z * t;

    const double eff_yaw = m_rot_offset.yaw * (1.0 - t) + m_scope_rot_offset.yaw * t;
    const double eff_pitch = m_rot_offset.pitch * (1.0 - t) + m_scope_rot_offset.pitch * t;
    const double eff_roll = m_rot_offset.roll * (1.0 - t) + m_scope_rot_offset.roll * t;

    // [TOT MIT DER AUSGELIEFERTEN JSON] Alle sechs Werte stehen dort auf 0,
    // die Funktion kehrt hier immer zurueck. Der Weg dahinter bleibt trotzdem
    // vollstaendig portiert -- die JSON ist von Hand aenderbar.
    if (eff_px == 0.0 && eff_py == 0.0 && eff_pz == 0.0 && eff_yaw == 0.0 && eff_pitch == 0.0
        && eff_roll == 0.0) {
        return;
    }

    const auto wid = re4vr::get_current_weapon_id();

    if (wid < 0 || !is_scope_weapon(wid)) {
        return;
    }

    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    if (eff_px != 0.0 || eff_py != 0.0 || eff_pz != 0.0) {
        glm::vec3 cur{};

        if (get_vec3(tf, "get_LocalPosition", cur)) {
            set_vec3(tf, "set_LocalPosition",
                     glm::vec3{static_cast<float>(static_cast<double>(cur.x) + eff_px),
                               static_cast<float>(static_cast<double>(cur.y) + eff_py),
                               static_cast<float>(static_cast<double>(cur.z) + eff_pz)});
        }
    }

    if (eff_yaw != 0.0 || eff_pitch != 0.0 || eff_roll != 0.0) {
        glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(tf, "get_LocalRotation", cur)) {
            const auto o = quat_from_ypr(eff_yaw, eff_pitch, eff_roll);
            set_quat(tf, "set_LocalRotation", mul_onto(o, cur));
        }
    }
}

// Lua Z.386-425.
void RE4VRScope::find_body_children() {
    for (auto& h : m_body_children) {
        drop(h);
    }

    m_body_children.clear();

    auto* scene = get_scene();

    if (scene == nullptr) {
        return;
    }

    // [MERCS 2026-08-28, gemessen] Die Kind-Namen stimmen ueberall
    // (body/hair/head), nur der ROOT heisst je nach Charakter anders. Deshalb
    // den Namen des aktuellen Bodys aus dem PlayerContext davorstellen -- die
    // feste Liste bleibt als Rueckfall dahinter stehen.
    std::vector<BodyDef> roots{};
    std::string dyn_root{};

    {
        auto* cm = re4vr::character_manager();
        auto* ctx = cm != nullptr ? re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
                                  : nullptr;
        auto* bgo = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;
        dyn_root = name_of(bgo);

        if (!dyn_root.empty()) {
            roots.push_back(BodyDef{dyn_root.c_str(), {"body", "body_armor", "hair", "head"}});
        }
    }

    for (const auto& c : CHARACTER_BODY) {
        roots.push_back(c);
    }

    for (const auto& ch : roots) {
        auto* go = find_go_by_name(ch.root);

        if (go == nullptr) {
            continue;
        }

        bool valid = false;

        if (!re4vr::try_call<bool>(go, "get_Valid", valid) || !valid) {
            continue;
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

        if (tf != nullptr) {
            auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
            int guard = 0;

            while (child != nullptr && guard < CHILD_GUARD) {
                ++guard;

                auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

                if (cgo != nullptr) {
                    const auto nm = name_of(cgo);

                    if (!nm.empty()) {
                        for (const char* want : ch.children) {
                            if (want != nullptr && nm == want) {
                                RefHandle h{};

                                if (keep(cgo, h)) {
                                    m_body_children.push_back(h);
                                }

                                break;
                            }
                        }
                    }
                }

                child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
            }
        }

        // Lua Z.424: der erste GUELTIGE Root gewinnt -- auch dann, wenn er
        // keine passenden Kinder hatte oder die Transform fehlte.
        return;
    }
}

// Lua Z.427-435.
void RE4VRScope::force_body_visible() {
    for (auto& h : m_body_children) {
        if (h.obj == nullptr || !re4vr::obj_ok(h.obj)) {
            continue;
        }

        re4vr::call_safe<void*>(h.obj, "set_UpdateSelf", true);
        re4vr::call_safe<void*>(h.obj, "set_DrawSelf", true);

        // Luas go:write_byte(0x13, 1). [M3] REFrameworks write_memory prueft
        // vorher is_valid_offset gegen get_size() und ueberspringt den Write
        // bei kaputtem Typ STILL -- dieselbe Pruefung hier, sonst schreibt der
        // Port, wo Lua nichts tut.
        if (auto gdef = utility::re_managed_object::get_type_definition(h.obj)) {
            if (0x13 + sizeof(uint8_t) <= gdef->get_size()) {
                try {
                    *(reinterpret_cast<uint8_t*>(h.obj) + 0x13) = 1;
                } catch (...) {
                }
            }
        }
    }
}

// Lua Z.494-560.
//
// [TOT MIT DER AUSGELIEFERTEN JSON] scope_fov_min/max stehen fuer alle sechs
// Waffen auf 0.0, die Funktion kehrt in der ersten Zeile zurueck. Sie wird
// trotzdem vollstaendig portiert, weil die JSON von Hand aenderbar ist.
void RE4VRScope::apply_scope_fov_override(int32_t wid) {
    if (m_scope_fov_min <= 0.0 && m_scope_fov_max <= 0.0) {
        return;
    }

    if (get_scene() == nullptr) {
        return;
    }

    ::REManagedObject* wcc = nullptr;

    for (const char* n : {"WeaponCustomCatalog", "WeaponCustomCatalog_AO",
                          "WeaponCustomCatalog_MC"}) {
        wcc = find_go_by_name(n);

        if (wcc != nullptr) {
            break;
        }
    }

    if (wcc == nullptr || m_wcc_register_type == nullptr) {
        return;
    }

    // Lua Z.512 prueft `safe(get_Valid) == false`. safe() liefert bei einem
    // false wie bei einem Fehlschlag nil -- die Pruefung ist also NIE wahr und
    // filtert nichts. Hier deshalb ebenfalls keine Pruefung.
    auto* reg = re4vr::get_component(wcc, m_wcc_register_type);

    if (reg == nullptr) {
        return;
    }

    auto* ud = re4vr::call_safe<::REManagedObject*>(reg, "get_WeaponDetailCustomUserdata");

    if (ud == nullptr) {
        return;
    }

    void* stages = nullptr;
    sdk::RETypeDefinition* stages_td = nullptr;
    bool stages_is_value = false;

    // [M1] Lua liest die drei Listenfelder (_WeaponDetailStages,
    // _WeaponDetailCustom, _AttachmentCustoms) ohne Typpruefung. Waeren sie
    // WERTtypen, arbeitete Lua auf der Kopie weiter -- hier wird stattdessen
    // abgebrochen. Das ist unerreichbar: alle drei sind List<T>, also
    // Referenztypen; und der ganze Weg haengt ohnehin hinter der
    // scope_fov-Schwelle, die mit der ausgelieferten JSON nie faellt.
    // Bewusst so gelassen -- ein Methodenaufruf auf einer Struct-Kopie waere
    // hier Unsinn, kein 1:1.
    if (!field_target(ud, utility::re_managed_object::get_type_definition(ud),
                      "_WeaponDetailStages", stages, stages_td, false, stages_is_value)
        || stages_is_value) {
        return;
    }

    auto* stages_obj = reinterpret_cast<::REManagedObject*>(stages);
    int32_t count = 0;

    if (!re4vr::try_call<int32_t>(stages_obj, "get_Count", count)) {
        count = 0;   // Luas `or 0`
    }

    for (int32_t i = 0; i < count; ++i) {
        auto* wd = re4vr::call_safe<::REManagedObject*>(stages_obj, "get_Item", i);

        if (wd == nullptr) {
            continue;
        }

        auto wd_td = utility::re_managed_object::get_type_definition(wd);
        const auto stage_wid = read_int_field(wd, wd_td, "_WeaponID", false);

        if (!stage_wid.has_value() || *stage_wid != static_cast<int64_t>(wid)) {
            continue;
        }

        void* wdc = nullptr;
        sdk::RETypeDefinition* wdc_td = nullptr;
        bool wdc_is_value = false;

        if (field_target(wd, wd_td, "_WeaponDetailCustom", wdc, wdc_td, false, wdc_is_value)
            && !wdc_is_value) {
            void* acs = nullptr;
            sdk::RETypeDefinition* acs_td = nullptr;
            bool acs_is_value = false;

            if (field_target(wdc, wdc_td, "_AttachmentCustoms", acs, acs_td, wdc_is_value,
                             acs_is_value)
                && !acs_is_value) {
                auto* acs_obj = reinterpret_cast<::REManagedObject*>(acs);
                int32_t ac_count = 0;

                if (!re4vr::try_call<int32_t>(acs_obj, "get_Count", ac_count)) {
                    ac_count = 0;
                }

                for (int32_t ai = 0; ai < ac_count; ++ai) {
                    auto* item_data =
                        re4vr::call_safe<::REManagedObject*>(acs_obj, "get_Item", ai);

                    if (item_data == nullptr) {
                        continue;
                    }

                    void* aps = nullptr;
                    sdk::RETypeDefinition* aps_td = nullptr;
                    bool aps_is_value = false;

                    if (!field_target(item_data,
                                      utility::re_managed_object::get_type_definition(item_data),
                                      "_AttachmentParams", aps, aps_td, false, aps_is_value)
                        || aps_is_value) {
                        continue;
                    }

                    auto* aps_obj = reinterpret_cast<::REManagedObject*>(aps);
                    int32_t ap_count = 0;

                    if (!re4vr::try_call<int32_t>(aps_obj, "get_Count", ap_count)) {
                        ap_count = 0;
                    }

                    for (int32_t pi = 0; pi < ap_count; ++pi) {
                        auto* ap = re4vr::call_safe<::REManagedObject*>(aps_obj, "get_Item", pi);

                        if (ap == nullptr) {
                            continue;
                        }

                        void* sp = nullptr;
                        sdk::RETypeDefinition* sp_td = nullptr;
                        bool sp_is_value = false;

                        if (!field_target(ap, utility::re_managed_object::get_type_definition(ap),
                                          "_ScopeParam", sp, sp_td, false, sp_is_value)) {
                            continue;
                        }

                        // [1:1, teuer belegt] Ist _ScopeParam ein WERTtyp, dann
                        // liefert REFrameworks get_field nach Lua eine KOPIE
                        // (Sdk.cpp:989-995 memcpy'd in einen eigenen Puffer) --
                        // Luas sp:set_field(...) schreibt dann in diese Kopie
                        // und ist wirkungslos. Genau das bilden wir nach: nur
                        // bei einem Referenztyp wird wirklich geschrieben.
                        if (sp_is_value) {
                            continue;
                        }

                        if (m_scope_fov_min > 0.0) {
                            write_field_float(sp, sp_td, "_FOVMin",
                                              static_cast<float>(m_scope_fov_min), false);
                        }

                        if (m_scope_fov_max > 0.0) {
                            write_field_float(sp, sp_td, "_FOVMax",
                                              static_cast<float>(m_scope_fov_max), false);
                        }
                    }
                }
            }
        }

        break;   // found our weapon, done (Lua Z.556)
    }
}

// Lua Z.562-605.
void RE4VRScope::apply_scope_cam_offset(int32_t wid) {
    if (m_scope_cam_pos.x == 0.0 && m_scope_cam_pos.y == 0.0 && m_scope_cam_pos.z == 0.0
        && m_scope_cam_rot.yaw == 0.0 && m_scope_cam_rot.pitch == 0.0
        && m_scope_cam_rot.roll == 0.0) {
        return;
    }

    auto* sc_tf = get_scope_camera_transform(wid);

    if (sc_tf == nullptr) {
        return;
    }

    if (m_scope_cam_pos.x != 0.0 || m_scope_cam_pos.y != 0.0 || m_scope_cam_pos.z != 0.0) {
        glm::vec3 cur{};

        if (get_vec3(sc_tf, "get_LocalPosition", cur)) {
            set_vec3(sc_tf, "set_LocalPosition",
                     glm::vec3{
                         static_cast<float>(static_cast<double>(cur.x) + m_scope_cam_pos.x),
                         static_cast<float>(static_cast<double>(cur.y) + m_scope_cam_pos.y),
                         static_cast<float>(static_cast<double>(cur.z) + m_scope_cam_pos.z)});
        }
    }

    if (m_scope_cam_rot.yaw != 0.0 || m_scope_cam_rot.pitch != 0.0
        || m_scope_cam_rot.roll != 0.0) {
        glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(sc_tf, "get_LocalRotation", cur)) {
            const auto o = quat_from_ypr(m_scope_cam_rot.yaw, m_scope_cam_rot.pitch,
                                         m_scope_cam_rot.roll);
            set_quat(sc_tf, "set_LocalRotation", mul_onto(o, cur));
        }
    }
}

// Lua Z.633-651. WIRKT -- als einzige Transform-Manipulation dieser Datei mit
// der ausgelieferten JSON.
void RE4VRScope::ensure_weapon_parented(int32_t wid) {
    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    auto* wp_tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (wp_tf == nullptr) {
        return;
    }

    auto* parent = re4vr::call_safe<::REManagedObject*>(wp_tf, "get_Parent");

    if (parent != nullptr) {
        return;   // already has parent, done
    }

    // No parent (NONE) -> reparent to body
    if (m_cached_body_tf.obj == nullptr || !re4vr::obj_ok(m_cached_body_tf.obj)) {
        keep(get_body_transform(), m_cached_body_tf);
    }

    if (m_cached_body_tf.obj == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(wp_tf, "set_Parent", m_cached_body_tf.obj);
}

// Lua Z.655-670.
void RE4VRScope::force_weapon_mesh_part0(int32_t wid) {
    if (m_via_mesh_type == nullptr) {
        return;
    }

    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    // [EXCEPTIONS 2026-08-28] Der Kommentar im Original nennt get_Valid hier
    // PFLICHT -- die Pruefung selbst (Z.662) ist wegen safe() aber nie wahr.
    // find_weapon_go hat aber bereits auf get_Valid gefiltert, und
    // re4vr::get_component prueft zusaetzlich obj_ok. Damit steht der Schutz,
    // den der Kommentar meint, ohne das Verhalten zu aendern.
    auto* mesh = re4vr::get_component(go, m_via_mesh_type);

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

// ============================================================================
// Lens-Entspiegelung (Lua Z.680-784)
//
// TEUER BEZAHLT: get-/setMaterialFloat wollen den Variablen-INDEX, NICHT den
// Namen. Mit dem Namen liefert die Engine Muell und jeder Write verpufft
// lautlos. Der Index wird darum einmal pro Waffe ueber getMaterialVariableName
// aufgeloest und danach nur noch der benutzt.
// ============================================================================

void RE4VRScope::lens_clear() {
    drop(m_lens.mesh);
    m_lens = Lens{};
}

// Lua Z.698-729.
void RE4VRScope::lens_resolve(int32_t wid) {
    // Lua legt die Tabelle komplett neu an -- inklusive check_t = os.clock().
    lens_clear();
    m_lens.wid = wid;
    m_lens.check_t = now_clock();

    if (m_via_mesh_type == nullptr) {
        return;
    }

    auto* go = find_weapon_go(wid);

    if (go == nullptr) {
        return;
    }

    // Lua Z.702: `== false` -- wegen safe() nie wahr, also keine Pruefung.

    // Adresse des GameObjects merken -- daran erkennen wir spaeter, ob das
    // Spiel die Waffe neu gebaut hat (Save-Load, Rundenwechsel).
    m_lens.addr = reinterpret_cast<uintptr_t>(go);
    m_lens.addr_valid = true;

    auto* mesh = re4vr::get_component(go, m_via_mesh_type);

    if (mesh == nullptr) {
        return;
    }

    int32_t n = 0;

    // Luas `if type(n) ~= "number" then return end`.
    if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", n)) {
        return;
    }

    for (int32_t mi = 0; mi < n; ++mi) {
        auto* mn_obj = re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialName", mi);

        if (mn_obj == nullptr) {
            continue;
        }

        std::string mn{};

        try {
            mn = utility::re_string::get_string(reinterpret_cast<::SystemString*>(mn_obj));
        } catch (...) {
            continue;
        }

        std::string low = mn;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (low.find("lens") == std::string::npos) {
            continue;
        }

        int32_t vn = 0;

        if (re4vr::try_call<int32_t>(mesh, "getMaterialVariableNum", vn, mi)) {
            for (int32_t vi = 0; vi < vn; ++vi) {
                auto* vname_obj =
                    re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialVariableName", mi, vi);

                if (vname_obj == nullptr) {
                    continue;
                }

                std::string vname{};

                try {
                    vname =
                        utility::re_string::get_string(reinterpret_cast<::SystemString*>(vname_obj));
                } catch (...) {
                    continue;
                }

                m_lens.vidx[vname] = vi;

                float cur = 0.0f;

                if (re4vr::try_call<float>(mesh, "getMaterialFloat", cur, mi, vi)) {
                    m_lens.orig[vname] = static_cast<double>(cur);
                }
            }
        }

        // Material beschreibbar machen, sonst verpuffen die Writes.
        // [L6] GENAU EINMAL je Aufloesung, nicht pro Frame.
        re4vr::call_safe<void*>(mesh, "updatableMaterial");

        keep(mesh, m_lens.mesh);
        m_lens.midx = mi;
        return;
    }
}

// Lua Z.731-735.
void RE4VRScope::lens_write(const char* name, double value) {
    const auto it = m_lens.vidx.find(name);

    if (it == m_lens.vidx.end() || m_lens.mesh.obj == nullptr) {
        return;
    }

    const auto m = find_method(m_lens.mesh.obj, "setMaterialFloat");

    if (m == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();

    try {
        m->call_safe<void*>(context, m_lens.mesh.obj, m_lens.midx, it->second,
                            static_cast<float>(value));
    } catch (...) {
    }

    clear_pending(context, true);
}

// Lua Z.737-784.
void RE4VRScope::apply_lens_fix(bool aiming) {
    const auto wid = re4vr::get_current_weapon_id();

    if (wid < 0 || !is_scope_weapon(wid)) {
        m_lens.wid.reset();
        drop(m_lens.mesh);
        return;
    }

    // Waffenwechsel -> sofort neu aufloesen. Ist die Aufloesung fehlgeschlagen
    // (Waffe direkt nach dem Laden noch nicht fertig gebaut), NICHT jeden Frame
    // den ganzen Suchlauf neu fahren -- alle 0,5 s reicht.
    if (!m_lens.wid.has_value() || *m_lens.wid != wid) {
        lens_resolve(wid);
    } else if (m_lens.mesh.obj == nullptr && (now_clock() - m_lens.check_t) > 0.5) {
        lens_resolve(wid);
    }

    // [SAVE-LOAD 2026-08-28] Ein Wechsel der Waffen-ID reicht als Ausloeser
    // NICHT: nach einem Save-Load baut das Spiel dieselbe Waffe neu auf --
    // gleiche ID, aber ein anderes GameObject. Die gemerkte Mesh-Komponente ist
    // dann eine Leiche, und das Schreiben verpufft LAUTLOS. Deshalb alle 0,5 s
    // das Waffen-GameObject frisch nachschlagen und seine ADRESSE vergleichen.
    const double now = now_clock();

    if (m_lens.mesh.obj != nullptr && (now - m_lens.check_t) > 0.5) {
        m_lens.check_t = now;
        auto* go = find_weapon_go(wid);
        const uintptr_t addr = reinterpret_cast<uintptr_t>(go);

        // Luas `addr ~= lens.addr` -- ohne Waffe ist addr nil, und nil ist
        // ungleich jeder Adresse, also wird ebenfalls neu aufgeloest.
        if (!m_lens.addr_valid || addr != m_lens.addr) {
            lens_resolve(wid);
        }
    }

    if (m_lens.mesh.obj == nullptr) {
        return;
    }

    for (const auto& v : LENS_ALWAYS) {
        lens_write(v.name, v.value);
    }

    if (aiming) {
        for (const auto& v : LENS_ON_AIM) {
            lens_write(v.name, v.value);
        }

        m_lens.was_aiming = true;
    } else {
        // Beim Verlassen des Zielens den Engine-Wert einmal AKTIV
        // zurueckschreiben: blosses "nicht mehr schreiben" stellt nichts wieder
        // her.
        if (m_lens.was_aiming) {
            m_lens.was_aiming = false;
            m_lens.restore = 10;
        }

        if (m_lens.restore > 0) {
            m_lens.restore = m_lens.restore - 1;

            for (const auto& v : LENS_ON_AIM) {
                const auto it = m_lens.orig.find(v.name);

                // Luas `if lens.orig[name] then` -- eine 0.0 ist dort WAHR.
                if (it != m_lens.orig.end()) {
                    lens_write(v.name, it->second);
                }
            }
        }
    }

    // [L8] Die drei LENS_ALWAYS-Werte werden NIE restauriert -- auch nicht bei
    // Waffenwechsel, Austritt oder Reset. Das ist im Original so und bleibt so.
}

// ============================================================================
// Hauptschleife (Lua Z.816-862)
// ============================================================================

void RE4VRScope::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRScope", "on_pre_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        // [M4] Aber NICHT einfach einfrieren: objects setzt den Riegel sofort
        // und fordert den Script-Reset erst danach an. In dem Fenster lief die
        // Lua-Datei noch und haette beim Verlassen des Scopes `false`
        // geschrieben; nach dem Reset ist das Global ohnehin nil (~ false).
        // Ein stehengebliebenes `true` wuerde materials und crosshair fuer
        // einige Frames belogen. Also einmal auf den Endzustand raeumen.
        if (!m_gate_cleared) {
            m_gate_cleared = true;
            re4vr::lua_set_bool("vr_scope_active", false);
            m_is_scoped = false;
            m_is_zoomed = false;
            drop(m_cached_body_tf);

            for (auto& h : m_body_children) {
                drop(h);
            }

            m_body_children.clear();
        }

        return;
    }

    m_gate_cleared = false;

    if (hash != "LockScene"_fnv) {
        return;
    }

    if (!m_types_resolved) {
        m_types_resolved = true;
        m_scene_td = sdk::find_type_definition("via.SceneManager");
        m_scope_ctrl_t = sdk::find_type_definition(game_namespace("ScopeController"));
        m_via_mesh_type = sdk::find_type_definition("via.render.Mesh");
        m_wcc_register_type =
            sdk::find_type_definition(game_namespace("WeaponCustomCatalogRegister"));
        load_config();
    }

    const bool scoped = check_scope_aim();

    // [LENS_ENTSPIEGELUNG] Laeuft unabhaengig vom Zielen: das Glas spiegelt
    // auch ohne Aim.
    apply_lens_fix(scoped);

    // ENTERING scope (ironsight or zoom)
    if (scoped && !m_is_scoped) {
        m_is_scoped = true;
        // [L2] Muss in Luas _G landen: RE4VRCrosshair und RE4VRMaterials lesen
        // es dort ueber re4vr::lua_get_bool.
        re4vr::lua_set_bool("vr_scope_active", true);
        find_body_children();
    }

    // DURING scope
    if (scoped && m_is_scoped) {
        // Force body/hands visible (game hides them during aim)
        force_body_visible();

        const auto wid = re4vr::get_current_weapon_id();
        const bool wid_ok = wid >= 0 && is_scope_weapon(wid);

        if (wid_ok) {
            switch_weapon_config(wid);
            ensure_weapon_parented(wid);
        }

        m_is_zoomed = is_scope_zoomed();

        // Lua Z.851: `if zoomed and wid then` -- geprueft wird NUR auf eine
        // vorhandene Waffen-ID, NICHT erneut auf SCOPE_WEAPONS.
        if (m_is_zoomed && wid >= 0) {
            force_weapon_mesh_part0(wid);
            apply_scope_cam_offset(wid);
            apply_scope_fov_override(wid);
        }

        apply_ironsight_offset();
    }

    // LEAVING scope
    if (!scoped && m_is_scoped) {
        re4vr::lua_set_bool("vr_scope_active", false);
        m_is_scoped = false;
        m_is_zoomed = false;
        drop(m_cached_body_tf);
        m_scope_lerp_t = 0.0;

        for (auto& h : m_body_children) {
            drop(h);
        }

        m_body_children.clear();
    }

    re4vr::clear_vm_exception();
}

// ============================================================================
// Lebenszyklus
// ============================================================================

void RE4VRScope::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRScope", "on_lua_state_created");
    (void)lua;
}

// Lua Z.864-871 (on_script_reset) PLUS das, was der Neuladen der Datei
// mitbringt: alle Locals stehen danach wieder auf ihren Anfangswerten und
// load_config() laeuft erneut (Z.212).
//
// [L10] load_config liest ueber get_controller_preference die Pref-JSON neu --
// der Konfigurationspfad kann sich dadurch aendern.
void RE4VRScope::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRScope", "on_lua_state_destroyed");
    (void)lua;
    reset_state();
}

void RE4VRScope::reset_state() {
    // Die fuenf ausdruecklichen Zuweisungen aus on_script_reset. Das Global
    // selbst faellt mit dem Lua-State weg (es ist danach nil, nicht false) --
    // die Leser behandeln nil und false gleich, und der naechste LockScene
    // setzt es ohnehin neu.
    m_is_scoped = false;
    m_is_zoomed = false;
    drop(m_cached_body_tf);

    for (auto& h : m_body_children) {
        drop(h);
    }

    m_body_children.clear();

    // Was das Neuladen der Datei zusaetzlich zuruecksetzt (alles Locals):
    lens_clear();
    m_scope_lerp_t = 0.0;
    m_active_wid.reset();
    m_pos_offset = Vec3Cfg{};
    m_rot_offset = RotCfg{};
    m_scope_pos_offset = Vec3Cfg{};
    m_scope_rot_offset = RotCfg{};
    m_scope_cam_pos = Vec3Cfg{};
    m_scope_cam_rot = RotCfg{};
    m_scope_fov_min = 0.0;
    m_scope_fov_max = 0.0;
    m_scope_lerp_speed = 8.0;
    m_slider_range_pos = 1.0;
    m_slider_range_rot = 180.0;
    m_weapon_configs.clear();

    load_config();
}

#endif // RE4
