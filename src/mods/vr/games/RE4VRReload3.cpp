// ============================================================================
// RE4VRReload3 -- 1:1-Portierung von re4_vr_reload3.lua (4.113 Zeilen).
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <imgui.h>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>

#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRWeapons2.hpp"
#include "RE4VRReloadAdv.hpp"
#include "RE4VRReloadMain.hpp"
#include "RE4VRReload3.hpp"

#undef min
#undef max

// ============================================================================
// Lokale Helfer (in Lua die Datei-Locals safe/sc/sf/quat_from_euler)
// ============================================================================
namespace {

double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

float ease(float t) {   // smoothstep -- reload2 hat kein globales ease, jeder
    return t * t * (3.0f - 2.0f * t);   // Block bringt sein eigenes mit.
}

float clamp01(float v) {
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
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

    out = glm::quat{v.w, v.x, v.y, v.z};

    return true;
}

void set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{v.x, v.y, v.z, 0.0f};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
    }

    clear_pending(context, true);
}

void set_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{q.x, q.y, q.z, q.w};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
    }

    clear_pending(context, true);
}

// Quaternion aus Euler-Grad -- qz*qy*qx, NICHT normalisiert (wie reload2).
glm::quat quat_from_euler(float rx, float ry, float rz) {
    const float hx = glm::radians(rx) * 0.5f;
    const float hy = glm::radians(ry) * 0.5f;
    const float hz = glm::radians(rz) * 0.5f;

    const glm::quat qx{std::cos(hx), std::sin(hx), 0.0f, 0.0f};
    const glm::quat qy{std::cos(hy), 0.0f, std::sin(hy), 0.0f};
    const glm::quat qz{std::cos(hz), 0.0f, 0.0f, std::sin(hz)};

    return qz * qy * qx;
}

glm::vec3 vec_sub(const glm::vec3& a, const glm::vec3& b) {
    return glm::vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

float vec_len(const glm::vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

::REManagedObject* joint_by_name(::REManagedObject* tf, const std::string& name) {
    if (tf == nullptr || name.empty()) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(name));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str);
}

std::optional<int32_t> call_enum(::REManagedObject* obj, std::string_view name) {
    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(obj, name, v)) {
        return std::nullopt;
    }

    return v;
}

std::string obj_name(::REManagedObject* o) {
    if (o == nullptr) {
        return {};
    }

    auto* nm = re4vr::call_safe<::REManagedObject*>(o, "get_Name");

    if (nm == nullptr) {
        return {};
    }

    return utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm));
}

// "20,30" -> {20,30}; toleriert Leerzeichen/Muell (Luas parse_parts).
std::vector<int32_t> parse_parts(const std::string& s) {
    std::vector<int32_t> t;
    size_t i = 0;

    while (i < s.size()) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
            ++i;
            continue;
        }

        size_t j = i;

        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) {
            ++j;
        }

        t.push_back(std::stoi(s.substr(i, j - i)));
        i = j;
    }

    return t;
}

// VR-Eingaben. Handle UND Joystick JEDEN Frame frisch holen (Stale-Handle-Fix).
bool vr_action(bool left, bool weapon_dial) {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto act = weapon_dial ? vr->get_action_weapon_dial() : vr->get_action_grip();
    const auto js = left ? vr->get_left_joystick() : vr->get_right_joystick();

    if (act == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active(act, js);
    } catch (...) {
        return false;
    }
}

bool left_grip_down() {
    return vr_action(true, false);
}

bool left_trigger_down() {
    return vr_action(true, true);
}

void haptic_left(float amp, float dur) {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return;
    }

    vr->trigger_haptic_vibration(0.0f, dur, 169.385f, amp, vr->get_left_joystick());
}

// Sound am SoundContainer eines GameObjects.
void trigger_sound(::REManagedObject* go, uint32_t id) {
    if (go == nullptr || id == 0) {
        return;
    }

    static auto* td_snd = sdk::find_type_definition("soundlib.SoundContainer");
    auto* scn = re4vr::get_component(go, td_snd);

    if (scn != nullptr) {
        re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", id);
    }
}


// JSON-Leser: `if type(x) == "number"` in Lua -- fehlt der Schluessel oder hat
// er den falschen Typ, bleibt der bisherige Wert stehen.
float jnum(const nlohmann::json& j, const char* key, float def) {
    if (!j.is_object()) {
        return def;
    }

    const auto it = j.find(key);

    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

bool jbool(const nlohmann::json& j, const char* key, bool cur) {
    if (!j.is_object()) {
        return cur;
    }

    const auto it = j.find(key);

    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : cur;
}

std::string jstr(const nlohmann::json& j, const char* key, const std::string& cur) {
    if (!j.is_object()) {
        return cur;
    }

    const auto it = j.find(key);

    return (it != j.end() && it->is_string()) ? it->get<std::string>() : cur;
}

}   // namespace
// ============================================================================
// Geteilte Helfer
// ============================================================================

::REManagedObject* RE4VRReload3::get_ctx() {
    if (re4vr::fc::on()) {
        return re4vr::fc::ctx();
    }

    if (!re4vr::obj_ok(m_character_manager)) {
        m_character_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

std::optional<int32_t> RE4VRReload3::get_equip_wid() {
    if (re4vr::fc::on()) {
        return re4vr::fc::equip_wid();
    }

    auto* ctx = get_ctx();

    if (ctx == nullptr) {
        return std::nullopt;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return std::nullopt;
    }

    return call_enum(hu, "get_EquipWeaponID");
}

::REManagedObject* RE4VRReload3::body_tf() {
    if (re4vr::fc::on()) {
        return re4vr::fc::body_tf();
    }

    auto* ctx = get_ctx();
    auto* b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    return re4vr::call_safe<::REManagedObject*>(b, "get_Transform");
}

::REManagedObject* RE4VRReload3::get_pe() {
    if (re4vr::fc::on()) {
        return re4vr::fc::pe();
    }

    if (re4vr::obj_ok(m_pe_cache)
        && re4vr::call_safe<::REManagedObject*>(m_pe_cache, "get_Context") != nullptr) {
        return m_pe_cache;
    }

    auto* ctx = get_ctx();
    auto* head = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadGameObject");

    if (head == nullptr) {
        m_pe_cache = nullptr;

        return nullptr;
    }

    static auto* td_pe = sdk::find_type_definition("chainsaw.PlayerEquipment");
    m_pe_cache = re4vr::get_component(head, td_pe);

    return m_pe_cache;
}

// [GO_SUFFIX] GO kann "wp####", "_AO" oder "_MC" heissen. Reihenfolge wichtig --
// wo ein plain-GO existiert, ist "_AO" nur der Schatten-Proxy.
::REManagedObject* RE4VRReload3::find_weapon(int32_t wid) {
    auto* bt = body_tf();

    if (bt == nullptr) {
        return nullptr;
    }

    char base[16]{};
    std::snprintf(base, sizeof(base), "wp%04d", wid);

    const std::function<::REManagedObject*(::REManagedObject*, const std::string&, int)> rec =
        [&](::REManagedObject* tf, const std::string& target, int depth) -> ::REManagedObject* {
        if (tf == nullptr || depth < 0) {
            return nullptr;
        }

        auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
        int n = 0;

        while (child != nullptr && n < 128) {
            ++n;
            auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

            if (go != nullptr && obj_name(go) == target
                && re4vr::call_bool_not_false(go, "get_DrawSelf")) {
                return child;
            }

            if (auto* f = rec(child, target, depth - 1); f != nullptr) {
                return f;
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }

        return nullptr;
    };

    for (const char* suffix : {"", "_AO", "_MC"}) {
        if (auto* tf = rec(bt, std::string{base} + suffix, 5); tf != nullptr) {
            return tf;
        }
    }

    return nullptr;
}

// Right-Controller-B (rohe Flanke, vom Binding publiziert)
bool RE4VRReload3::right_b_down() {
    return re4vr::lua_get_tribool("__vr_raw_r_bbutton") == 1;
}

// [POSE_FADE] s. Header.
bool RE4VRReload3::pose_fade_step(PoseFade& f, const std::string& want, float& blend_out) {
    if (!want.empty()) {
        f.name = want;
        f.release_t.reset();
        blend_out = 1.0f;

        return true;
    }

    if (f.name.empty()) {
        blend_out = 0.0f;

        return false;
    }

    if (!f.release_t.has_value()) {
        f.release_t = clock_now();
    }

    const double el = clock_now() - *f.release_t;

    if (el >= POSE_FADE_DUR) {
        f.name.clear();
        blend_out = 0.0f;

        return false;
    }

    blend_out = 1.0f - static_cast<float>(el / POSE_FADE_DUR);

    return true;
}

const std::unordered_map<std::string, ::REManagedObject*>& RE4VRReload3::pose_map() {
    auto* tf = body_tf();

    if (tf == nullptr) {
        m_pmap.clear();

        return m_pmap;
    }

    if (tf == m_pmap_tf && !m_pmap.empty()) {
        return m_pmap;
    }

    m_pmap.clear();

    // [ARRAY-BINDING] Lua liest get_Count als Call, die Elemente aber ueber
    // joints[i] -- das ist das Index-Binding.
    auto* joints = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints");

    if (joints != nullptr) {
        const int32_t count = re4vr::array_size(joints);

        for (int32_t i = 0; i < count; ++i) {
            auto* j = re4vr::array_element(joints, i);
            const auto name = obj_name(j);

            if (j != nullptr && !name.empty()) {
                m_pmap[name] = j;
            }
        }
    }

    m_pmap_tf = tf;

    return m_pmap;
}

bool RE4VRReload3::pose_apply(const Bones& bones, float blend) {
    const auto& map = pose_map();

    if (map.empty()) {
        return false;
    }

    if (blend <= 0.0f) {
        return true;
    }

    for (const auto& e : bones) {
        const auto it = map.find(e.first);

        if (it == map.end() || it->second == nullptr) {
            continue;
        }

        auto* j = it->second;

        if (blend >= 0.9999f) {
            set_quat(j, "set_LocalRotation", e.second);
            continue;
        }

        glm::quat c{};

        if (!get_quat(j, "get_LocalRotation", c)) {
            continue;
        }

        glm::quat t = e.second;

        if ((c.w * t.w + c.x * t.x + c.y * t.y + c.z * t.z) < 0.0f) {
            t = glm::quat{-t.w, -t.x, -t.y, -t.z};
        }

        const glm::quat r{c.w + (t.w - c.w) * blend, c.x + (t.x - c.x) * blend,
                          c.y + (t.y - c.y) * blend, c.z + (t.z - c.z) * blend};
        const float len = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);

        if (len > 1e-6f) {
            set_quat(j, "set_LocalRotation",
                     glm::quat{r.w / len, r.x / len, r.y / len, r.z / len});
        }
    }

    return true;
}


// ============================================================================
// 1 -- CHICAGO SWEEPER (wp4201, Lua Z.130-1114)
//
// Logik = LE5-SMG (Mag-Drop + Slide-Rack), 1:1 vom Rifle-Muster aus reload2,
// aber OHNE Verstellschalter: der VERSTELLSCHALTER + BURST der Chicago bleiben
// in motion (gemeinsam mit der LE5, SWITCH_DOCK_WEAPONS/FIRE_MODE_CYCLE) und
// werden hier NICHT dupliziert.
// reload managed 4201 nicht mehr -> kein Konflikt. Wir publishen dieselben
// Slide-/Rack-/Mag-Globals wie die gemanagte SMG in reload.
// Eigenes JSON: re4_vr/re4_vr_reload3_chicago.json
// ============================================================================
namespace {

constexpr const char* CHI_CFG_PATH = "re4_vr/re4_vr_reload3_chicago.json";

// Magazin = joint_04, Slide = joint_01 (wird beim Leer-Reload in Z
// zurueckgezogen, wie LE5; beim normalen Reload nicht).
constexpr const char* CHI_J_MAG = "_04";
constexpr const char* CHI_J_SLIDE = "_01";

// Engine schliesst den Slide selbst nach dem Reload (SMG/LE5) -> wir forcen
// rest_z NICHT.
constexpr bool CHI_ENGINE_CLOSES = true;

// [TEMP MEASURE] true = Slide NICHT forcen (Mess-Modus). Werte gemessen und
// geseedet -> aus.
constexpr bool CHI_MEASURE_SLIDE = false;

// ---- Sounds (1:1 von reload's SOUNDS[4201]) ----
constexpr uint32_t CSND_DRY_FIRE = 812850326u;
constexpr uint32_t CSND_MAG_EJECT = 1466005368u;
constexpr uint32_t CSND_MAG_INSERT = 1757452382u;
constexpr uint32_t CSND_MAG_FLOOR = 3140689763u;
constexpr uint32_t CSND_SLIDE_BACK = 943565871u;
constexpr uint32_t CSND_SLIDE_FORWARD = 943565871u;
constexpr uint32_t CSND_MAG_HOLSTER = 1839787494u;

constexpr float CHI_GRAVITY = 9.8f;
constexpr float CHI_DROP_FALL_DUR = 1.0f;
constexpr double CHI_FLOOR_DELAY = 0.45;
constexpr double CHI_FLOOR_DELAY_MODULE = 0.78;
constexpr float CHI_RACK_GRAB_DIST = 0.14f;
constexpr float CHI_DOCK_BLEND_SPEED = 0.10f;

std::optional<glm::vec3> left_hand_world_g() {
    return re4vr::lua_get_vec3_any({"__vr_lh_world", "__vr_unified_lh_pos",
                                    "__vr_lh_joint_pos"});
}

std::optional<glm::vec3> right_hand_raw_g() {
    return re4vr::lua_get_vec3_any({"__vr_rh_ctrl_raw", "__vr_rh_world",
                                    "__vr_unified_rh_pos"});
}

// Luas `wi:write_dword(0x44, n)`: das Feld ueber die TypeDB suchen, den Offset
// nur als Rueckfall benutzen.
void field_i32_write(::REManagedObject* obj, const char* field, uint32_t fallback,
                     int32_t value) {
    if (obj == nullptr) {
        return;
    }

    uint32_t off = fallback;

    if (auto* td = utility::re_managed_object::get_type_definition(obj); td != nullptr) {
        if (auto* f = td->get_field(field); f != nullptr) {
            off = f->get_offset_from_base();
        }
    }

    *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(obj) + off) = value;
}

}   // namespace

bool RE4VRReload3::is_chicago(int32_t wid) {
    return wid == 4201;
}

float RE4VRReload3::c_park_ref(const CSlide& sp) {
    return (sp.empty_x != 0.0f) ? sp.rest_z : sp.park_z;
}

bool RE4VRReload3::chicago_flow() const {
    return m_cdrop.active || m_c_mag_hand || m_cins.active || m_c_mag_tune;
}

void RE4VRReload3::chicago_load_cfg() {
    const auto data = re4vr::json_load(CHI_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    if (const auto it = data.find("cfg"); it != data.end() && it->is_object()) {
        m_ccfg.chicago_enabled = jbool(*it, "chicago_enabled", m_ccfg.chicago_enabled);
        m_ccfg.reload_ammo = jbool(*it, "reload_ammo", m_ccfg.reload_ammo);
        m_ccfg.sound_enabled = jbool(*it, "sound_enabled", m_ccfg.sound_enabled);
        m_ccfg.insert_distance = jnum(*it, "insert_distance", m_ccfg.insert_distance);
    }

    // In Lua sind das Tabellen ueber die wid; es gibt aber nur die 4201.
    if (const auto it = data.find("slide"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4201"); v != it->end() && v->is_object()) {
            auto& sp = m_cslide;
            sp.rest_z = jnum(*v, "rest_z", sp.rest_z);
            sp.park_z = jnum(*v, "park_z", sp.park_z);
            sp.back_z = jnum(*v, "back_z", sp.back_z);
            sp.empty_x = jnum(*v, "empty_x", sp.empty_x);
            sp.dock_x = jnum(*v, "dock_x", sp.dock_x);
            sp.dock_y = jnum(*v, "dock_y", sp.dock_y);
            sp.dock_z = jnum(*v, "dock_z", sp.dock_z);
            sp.rack_rx = jnum(*v, "rack_rx", sp.rack_rx);
            sp.rack_ry = jnum(*v, "rack_ry", sp.rack_ry);
            sp.rack_rz = jnum(*v, "rack_rz", sp.rack_rz);
            sp.st_rx = jnum(*v, "st_rx", sp.st_rx);
            sp.st_ry = jnum(*v, "st_ry", sp.st_ry);
            sp.st_rz = jnum(*v, "st_rz", sp.st_rz);
        }
    }

    if (const auto it = data.find("dock"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4201"); v != it->end() && v->is_object()) {
            m_cdock.joint = jstr(*v, "joint", m_cdock.joint);
            m_cdock.x = jnum(*v, "x", m_cdock.x);
            m_cdock.y = jnum(*v, "y", m_cdock.y);
            m_cdock.z = jnum(*v, "z", m_cdock.z);
        }
    }

    if (const auto it = data.find("maghand"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4201"); v != it->end() && v->is_object()) {
            auto& m = m_cmaghand;
            m.x = jnum(*v, "x", m.x);
            m.y = jnum(*v, "y", m.y);
            m.z = jnum(*v, "z", m.z);
            m.rx = jnum(*v, "rx", m.rx);
            m.ry = jnum(*v, "ry", m.ry);
            m.rz = jnum(*v, "rz", m.rz);
            m.t_rx = jnum(*v, "t_rx", m.t_rx);
            m.t_ry = jnum(*v, "t_ry", m.t_ry);
            m.t_rz = jnum(*v, "t_rz", m.t_rz);
        }
    }
}

void RE4VRReload3::chicago_save_cfg() {
    const auto& sp = m_cslide;
    const auto& d = m_cdock;
    const auto& m = m_cmaghand;

    nlohmann::json j = {
        {"cfg", {{"chicago_enabled", m_ccfg.chicago_enabled},
                 {"reload_ammo", m_ccfg.reload_ammo},
                 {"sound_enabled", m_ccfg.sound_enabled},
                 {"insert_distance", m_ccfg.insert_distance}}},
        {"slide", {{"4201", {
            {"rest_z", sp.rest_z}, {"park_z", sp.park_z}, {"back_z", sp.back_z},
            {"empty_x", sp.empty_x}, {"dock_x", sp.dock_x}, {"dock_y", sp.dock_y},
            {"dock_z", sp.dock_z}, {"rack_rx", sp.rack_rx}, {"rack_ry", sp.rack_ry},
            {"rack_rz", sp.rack_rz}, {"st_rx", sp.st_rx}, {"st_ry", sp.st_ry},
            {"st_rz", sp.st_rz}}}}},
        {"dock", {{"4201", {{"joint", d.joint}, {"x", d.x}, {"y", d.y}, {"z", d.z}}}}},
        {"maghand", {{"4201", {
            {"x", m.x}, {"y", m.y}, {"z", m.z},
            {"rx", m.rx}, {"ry", m.ry}, {"rz", m.rz},
            {"t_rx", m.t_rx}, {"t_ry", m.t_ry}, {"t_rz", m.t_rz}}}}},
    };

    re4vr::json_save(CHI_CFG_PATH, j);
}

void RE4VRReload3::cf_snd(uint32_t id) {
    if (!m_ccfg.sound_enabled || id == 0 || m_cwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_cwep.tf, "get_GameObject"), id);
}

void RE4VRReload3::chicago_refresh() {
    const auto ewid = get_equip_wid();
    const bool managed = m_ccfg.chicago_enabled && ewid.has_value() && is_chicago(*ewid);

    if (!managed) {
        m_cwep = ChicagoWep{};

        return;
    }

    if (m_cwep.wid.has_value() && *m_cwep.wid == *ewid
        && m_cwep.mag_joint != nullptr && m_cwep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_cwep.tf, "get_Position", p)) {
            return;
        }
    }

    const bool same_wid = m_cwep.wid.has_value() && *m_cwep.wid == *ewid;
    m_cwep = ChicagoWep{};

    auto* tf = find_weapon(*ewid);

    if (tf == nullptr) {
        return;
    }

    auto* mj = joint_by_name(tf, CHI_J_MAG);

    if (mj == nullptr) {
        return;
    }

    m_cwep.wid = *ewid;
    m_cwep.tf = tf;
    m_cwep.mag_joint = mj;
    m_cwep.slide_joint = joint_by_name(tf, CHI_J_SLIDE);

    if (m_cwep.slide_joint != nullptr) {
        glm::vec3 lp{};

        if (get_vec3(m_cwep.slide_joint, "get_LocalPosition", lp)) {
            m_cwep.slide_rest_lp = lp;
        }
    }

    // [SAVE_LOAD] gleiche WeaponId, aber tf ungueltig -> neue Instanz.
    if (same_wid) {
        m_c_reacquired = true;
    }
}

// ---- chainsaw.Gun (Leer-Erkennung + Slide-Entriegeln nach Rack) ----
::REManagedObject* RE4VRReload3::cf_get_gun() {
    auto* pe = get_pe();

    if (pe == nullptr) {
        return nullptr;
    }

    auto* wl = re4vr::get_field_object(pe, "WeaponList");

    if (wl == nullptr) {
        return nullptr;
    }

    const auto ewid = get_equip_wid();

    if (!ewid.has_value()) {
        return nullptr;
    }

    std::vector<int32_t> keys{};

    if (const auto k = call_enum(pe, "get_EquipWeaponID"); k.has_value()) {
        keys.push_back(*k);
    }

    keys.push_back(*ewid);

    for (const int32_t key : keys) {
        auto* a = re4vr::call_safe<::REManagedObject*>(wl, "get_Item", key);

        if (a != nullptr && call_enum(a, "get_CurrentState").has_value()) {
            return a;
        }
    }

    return nullptr;
}

void RE4VRReload3::cf_gun_chamber() {
    auto* g = cf_get_gun();

    if (g == nullptr) {
        return;
    }

    static const auto holding = []() -> std::optional<int32_t> {
        auto* td = sdk::find_type_definition("chainsaw.Gun.State");

        if (td == nullptr) {
            return std::nullopt;
        }

        auto* f = td->get_field("Holding");

        return (f != nullptr) ? std::optional<int32_t>{f->get_data<int32_t>(nullptr)}
                              : std::nullopt;
    }();

    if (!holding.has_value()) {
        return;
    }

    re4vr::call_safe<void*>(g, "set_CurrentState(chainsaw.Gun.State)", *holding);
}

bool RE4VRReload3::cf_gun_ammo_empty() {
    bool v = false;

    return re4vr::try_call<bool>(get_pe(), "isGunAmmoEmpty", v) && v;
}

// ---- Live-WeaponItem (gegen die EQUIPPTE wid validiert) ----
// [ACCESSOR] ZUERST die einzige PERSISTENTE Instanz; alles darunter sind
// KOPIEN: Schreiben wirkt dort nur im selben Tick und ist danach weg.
::REManagedObject* RE4VRReload3::cf_get_wi() {
    const auto ewid = get_equip_wid();

    if (m_main != nullptr) {
        if (auto* real = m_main->real_wi(ewid); real != nullptr) {
            return real;
        }
    }

    auto* ewi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

std::optional<int32_t> RE4VRReload3::cf_loaded() {
    auto* wi = cf_get_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
}

int32_t RE4VRReload3::cf_cap() {
    auto* wi = cf_get_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload3::cf_reserve() {
    auto* wi = cf_get_wi();

    if (wi == nullptr || m_main == nullptr) {
        return 0;
    }

    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    if (!ammo_id.has_value()) {
        return 0;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(get_pe(), "get_InventoryController");

    return (inv != nullptr) ? m_main->item_count_sum(inv, *ammo_id) : 0;
}

// =====================================================================
// Mag-Drop / Mag-in-Hand / Insert
// =====================================================================
void RE4VRReload3::cf_capture_mag_rest() {
    if (m_cwep.mag_joint == nullptr || chicago_flow() || m_c_mag_out) {
        return;
    }

    glm::vec3 lp{};
    glm::quat lr{};

    if (get_vec3(m_cwep.mag_joint, "get_LocalPosition", lp)) {
        m_cwep.rest_lp = lp;
    }

    if (get_quat(m_cwep.mag_joint, "get_LocalRotation", lr)) {
        m_cwep.rest_lr = lr;
    }
}

void RE4VRReload3::cf_stop_drop() {
    if (m_cdrop.use_module && m_adv != nullptr) {
        m_adv->cancel();
    }

    m_cdrop.active = false;
    m_cdrop.joint = nullptr;
    m_cdrop.use_module = false;
}

bool RE4VRReload3::cf_start_drop_from(const std::optional<glm::vec3>& p, bool use_module) {
    if (use_module && m_cwep.mag_joint != nullptr && m_adv != nullptr) {
        if (m_adv->begin_drop(m_cwep.mag_joint, m_cwep.wid.value_or(0),
                              m_cins.dur, std::nullopt)) {
            m_cdrop.active = true;
            m_cdrop.use_module = true;
            m_cdrop.joint = m_cwep.mag_joint;

            return true;
        }
    }

    if (!p.has_value()) {
        return false;
    }

    m_cdrop.joint = m_cwep.mag_joint;
    m_cdrop.use_module = false;
    m_cdrop.sx = p->x;
    m_cdrop.sy = p->y;
    m_cdrop.sz = p->z;
    m_cdrop.t0 = clock_now();
    m_cdrop.active = true;

    return true;
}

bool RE4VRReload3::cf_force_eject() {
    // [UNLIMITED] B stillgelegt
    if (RE4VRWeapons2::get()->is_unlimited()) {
        return false;
    }

    chicago_refresh();

    if (m_cwep.mag_joint == nullptr) {
        return false;
    }

    if (m_c_mag_out) {
        return false;
    }

    // [KEIN DROP OHNE RESERVE] Ohne Nachschub wird das Magazin gar nicht erst
    // ausgeworfen -- gilt fuer JEDE Waffe.
    if (cf_reserve() <= 0) {
        return false;
    }

    // [LIVE-EMPTY] Frischer Engine-Read (getCurrentGunAmmo, kein Cache) statt
    // cf_loaded (nach Save-Load stale 0 -> Dauer-Dry-Fire).
    if (const auto fga = call_enum(get_pe(), "getCurrentGunAmmo"); fga.has_value()) {
        m_crack.empty_when_dropped = (*fga <= 0);
    } else {
        m_crack.empty_when_dropped = (cf_loaded().value_or(0) <= 0);
    }

    // [KAMMER] Der Read oben kann bereits UNSERE 0 sehen. War beim Nullen etwas
    // im Magazin, war die Kammer NICHT leer -> kein Rack verlangen.
    // [BESITZER PRUEFEN] Diese Maschine LIEST __re4_mag_carry nur; gesetzt und
    // geloescht wird er von reload (Leon) und reload2. Ein dort liegengebliebener
    // Wert einer FREMDEN Waffe hat hier die Rack-Pflicht abgeschaltet -- der Rest
    // zaehlt nur fuer DIESE Waffe; ohne Vermerk gilt er wie bisher.
    if (re4vr::lua_get_number("__re4_mag_carry", 0.0) > 0.0) {
        const auto cw = re4vr::lua_get_number_opt("__re4_mag_carry_wid");
        const auto ew = get_equip_wid();

        if (!cw.has_value()
            || (ew.has_value() && static_cast<int32_t>(*cw) == *ew)) {
            m_crack.empty_when_dropped = false;
        }
    }

    m_crack._chambered_hold = false;
    m_c_mag_hand = false;
    m_cins.active = false;

    if (m_ccfg.reload_ammo) {
        // [1:1] In Lua steht hier `mag_retained = loaded` -- `loaded` ist an
        // dieser Stelle KEIN Local dieses Blocks, sondern ein GLOBAL und damit
        // immer nil. Der Magazinrest ist beim Auswurf also faktisch 0. Genau so
        // uebernommen; alles andere waere eine Verhaltensaenderung.
        m_c_mag_retained = 0;

        if (auto* wi = cf_get_wi(); wi != nullptr) {
            if (m_main != nullptr) {
                m_main->carry_capture(wi, "re4_vr_reload3.lua:481", std::nullopt);
            }

            field_i32_write(wi, "_CurrentAmmoCount", 0x44, 0);
        }

        // [ACCESSOR-FOLGE] Diese 0 ist UNSER Werk und wirkt seit dem
        // Accessor-Umbau wirklich. `rack.empty` darf sie beim Einsetzen nicht als
        // leere Kammer werten.
        m_crack._zeroed_by_us = true;
    }

    if (m_cwep.rest_lp.has_value()) {
        set_vec3(m_cwep.mag_joint, "set_LocalPosition", *m_cwep.rest_lp);
    }

    if (m_cwep.rest_lr.has_value()) {
        set_quat(m_cwep.mag_joint, "set_LocalRotation", *m_cwep.rest_lr);
    }

    glm::vec3 p{};
    const bool have_p = get_vec3(m_cwep.mag_joint, "get_Position", p);
    const bool started = cf_start_drop_from(
        have_p ? std::optional<glm::vec3>{p} : std::nullopt, true);

    if (started) {
        m_c_mag_out = true;
        cf_snd(CSND_MAG_EJECT);
        m_c_mag_floor_at = clock_now()
            + (m_cdrop.use_module ? CHI_FLOOR_DELAY_MODULE : CHI_FLOOR_DELAY);
    }

    return started;
}

void RE4VRReload3::cf_update_drop() {
    if (!m_cdrop.active) {
        return;
    }

    if (m_cdrop.use_module) {
        if (m_adv != nullptr) {
            m_adv->tick();
        } else {
            cf_stop_drop();
        }

        return;
    }

    if (m_cdrop.joint == nullptr) {
        return;
    }

    const float t = static_cast<float>(clock_now() - m_cdrop.t0);

    if (t > CHI_DROP_FALL_DUR) {
        m_cdrop.active = false;
        m_cdrop.joint = nullptr;

        return;
    }

    const float fall = 0.5f * CHI_GRAVITY * t * t;
    set_vec3(m_cdrop.joint, "set_Position",
             glm::vec3{m_cdrop.sx, m_cdrop.sy - fall, m_cdrop.sz});
}

bool RE4VRReload3::cf_can_grab() {
    if (m_c_mag_hand || m_cins.active) {
        return false;
    }

    if (!m_c_mag_out) {
        return false;
    }

    return (m_c_mag_retained + cf_reserve()) > 0;
}

bool RE4VRReload3::chicago_set_mag_in_hand(bool active) {
    if (active) {
        if (!cf_can_grab()) {
            return false;
        }

        cf_stop_drop();
        m_c_mag_hand = true;
        cf_snd(CSND_MAG_HOLSTER);

        return true;
    }

    if (m_c_mag_hand) {
        m_c_mag_hand = false;
        glm::vec3 p{};
        const bool have = m_cwep.mag_joint != nullptr
            && get_vec3(m_cwep.mag_joint, "get_Position", p);

        if (cf_start_drop_from(have ? std::optional<glm::vec3>{p} : std::nullopt, false)) {
            m_c_mag_floor_at = clock_now() + CHI_FLOOR_DELAY;
        }
    }

    return true;
}

void RE4VRReload3::cf_update_mag_in_hand() {
    auto* joint = ((m_c_mag_hand || m_c_mag_tune) ? m_cwep.mag_joint : nullptr);

    if (joint == nullptr) {
        return;
    }

    const auto& m = m_cmaghand;
    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)) {
        return;
    }

    glm::quat hr{};
    const bool have_hr = get_quat(lh, "get_Rotation", hr);
    glm::vec3 w = hp;

    if (have_hr) {
        w = hp + (hr * glm::vec3{m.x, m.y, m.z});
    }

    set_vec3(joint, "set_Position", w);

    if (have_hr) {
        set_quat(joint, "set_Rotation",
                 glm::normalize(hr * quat_from_euler(m.rx, m.ry, m.rz)));
    }
}

std::optional<glm::vec3> RE4VRReload3::cf_dock_port_world() {
    if (m_cwep.tf == nullptr) {
        return std::nullopt;
    }

    auto* j = joint_by_name(m_cwep.tf, m_cdock.joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{m_cdock.x, m_cdock.y, m_cdock.z});
}

// [KEYFRAME-ANKER] Sobald Punkte da sind, IST Keyframe #1 der Ladepunkt --
// Vorrang vor dem Dock-Port (der stammt aus der Zeit vor den Keyframes).
// Waffenrelativ zurueckgerechnet, dreht also mit der Waffe mit.
// Rueckbau: __re4_insert_kf_anchor = false
std::optional<glm::vec3> RE4VRReload3::cf_kf_anchor_world() {
    if (m_adv == nullptr || m_cwep.tf == nullptr || !m_cwep.wid.has_value()) {
        return std::nullopt;
    }

    if (re4vr::lua_get_tribool("__re4_insert_kf_anchor") == 0) {
        return std::nullopt;
    }

    const int32_t wid = *m_cwep.wid;

    if (!m_adv->has_shell_keys(wid) || m_adv->uses_rev_insert(wid)) {
        return std::nullopt;
    }

    RE4VRReloadAdv::Key k{};

    if (!m_adv->shell_pose_at(wid, 0.0f, k)) {
        return std::nullopt;
    }

    glm::vec3 wp0{};
    glm::quat wr0{};

    if (!get_vec3(m_cwep.tf, "get_Position", wp0)
        || !get_quat(m_cwep.tf, "get_Rotation", wr0)) {
        return std::nullopt;
    }

    return wp0 + (wr0 * glm::vec3{k.x, k.y, k.z});
}

bool RE4VRReload3::cf_start_insert() {
    if (!(m_cwep.mag_joint != nullptr && m_cwep.rest_lp.has_value())) {
        return false;
    }

    glm::vec3 lp{};

    if (!get_vec3(m_cwep.mag_joint, "get_LocalPosition", lp)) {
        return false;
    }

    m_cins.slp = lp;
    glm::quat lr{};
    m_cins.slr = get_quat(m_cwep.mag_joint, "get_LocalRotation", lr)
        ? std::optional<glm::quat>{lr} : std::nullopt;

    // [SHELL-KEYFRAMES] Hat der Sweeper eine Bahn, faehrt update_insert sie ab
    // statt linear von der Handlage in die Ruhepose zu lerpen. Eigene Bahn-Dauer
    // aus reload_adv; ohne Bahn bleibt die Chicago-Dauer -- darum die
    // Ausgangsdauer einmal merken.
    if (!m_cins.dur_base.has_value()) {
        m_cins.dur_base = m_cins.dur;
    }

    const int32_t wid = m_cwep.wid.value_or(0);
    m_cins.keyframe = (m_adv != nullptr) && m_adv->has_shell_keys(wid);
    m_cins.dur = *m_cins.dur_base;

    if (m_cins.keyframe && m_adv != nullptr) {
        const auto d = m_adv->kf_insert_dur(wid);
        m_cins.dur = d.value_or(m_adv->shell_dur);
    }

    m_cins.t0 = clock_now();
    m_cins.active = true;
    m_cins.snd = false;

    return true;
}

void RE4VRReload3::cf_check_insert_proximity() {
    if (!m_c_mag_hand) {
        return;
    }

    const auto hp = left_hand_world_g();

    if (!hp.has_value()) {
        return;
    }

    auto gp = cf_kf_anchor_world();

    if (!gp.has_value()) {
        gp = cf_dock_port_world();
    }

    if (!gp.has_value() && m_cwep.tf != nullptr) {
        glm::vec3 wp{};

        if (get_vec3(m_cwep.tf, "get_Position", wp)) {
            gp = wp;
        }
    }

    if (!gp.has_value()) {
        return;
    }

    if (vec_len(vec_sub(*hp, *gp)) <= m_ccfg.insert_distance) {
        m_c_mag_hand = false;
        cf_start_insert();
    }
}

void RE4VRReload3::cf_reload_ammo_on_insert() {
    if (!m_ccfg.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* wi = cf_get_wi();

    if (wi == nullptr) {
        return;
    }

    const int32_t cap = cf_cap();
    const int32_t reserve = cf_reserve();
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");
    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");
    const int32_t target = std::min(cap, m_c_mag_retained + reserve);
    const int32_t used = std::max(0, target - m_c_mag_retained);

    // [RUNTIME-FIX 0/0 + MAG-RETAIN] zwei Quellen: (1) Reserve-Anteil `used` ueber
    // den Engine-Reload (zieht Reserve), (2) Retained-Anteil (gedropptes Mag,
    // schon bezahlt) frei auf target auffuellen OHNE Reserve-Abzug.
    std::optional<int32_t> et{};

    if (auto* td = sdk::find_type_definition("chainsaw.EquipType"); td != nullptr) {
        if (auto* f = td->get_field("Main"); f != nullptr) {
            et = f->get_data<int32_t>(nullptr);
        }
    }

    const int32_t r_b4 = reserve;
    const auto read_rsv = [&]() -> int32_t {
        return (inv != nullptr && ammo_id.has_value())
            ? m_main->item_count_sum(inv, *ammo_id) : r_b4;
    };

    const int32_t b4 = m_main->gun_ammo().value_or(0);

    if (et.has_value() && inv != nullptr && used > 0) {
        m_main->load_and_book(inv, *et, used, false);
    }

    const int32_t af = m_main->gun_ammo().value_or(b4);
    const int32_t got = std::max(0, af - b4);

    if (got > 0 && inv != nullptr && ammo_id.has_value() && read_rsv() >= r_b4) {
        m_main->safe_reduce(inv, *ammo_id, got);
    }

    if (m_main->gun_ammo().value_or(af) < target) {
        field_i32_write(wi, "_CurrentAmmoCount", 0x44, target);
        const int32_t now = m_main->gun_ammo().value_or(b4 + got);

        if (now < target) {
            re4vr::call_safe<void*>(wi, "addAmmoCount", target - now, false);
        }
    }

    m_c_mag_retained = 0;
}

void RE4VRReload3::cf_update_insert() {
    if (!(m_cins.active && m_cwep.mag_joint != nullptr && m_cins.slp.has_value()
          && m_cwep.rest_lp.has_value())) {
        return;
    }

    float t = static_cast<float>(clock_now() - m_cins.t0) / std::max(m_cins.dur, 0.01f);

    if (t > 1.0f) {
        t = 1.0f;
    }

    if (!m_cins.snd && t >= 0.75f) {
        m_cins.snd = true;
        cf_snd(CSND_MAG_INSERT);
    }

    // [SHELL-KEYFRAMES] Mit Bahn: Mag-Joint entlang der Keyframes (Start = #1 =
    // der Punkt, gegen den die Naehe misst, Ende = Kammer) -- die Handlage geht
    // nicht mehr ein. Ohne Bahn / bei Fehlschlag der lineare Weg darunter.
    bool kf_done = false;

    if (m_cins.keyframe && m_adv != nullptr && m_cwep.tf != nullptr) {
        kf_done = m_adv->apply_shell_keys(m_cwep.tf, m_cwep.mag_joint,
                                          m_cwep.wid.value_or(0), t);
    }

    if (!kf_done) {
        const float u = ease(t);
        const glm::vec3 a = *m_cins.slp;
        const glm::vec3 b = *m_cwep.rest_lp;
        set_vec3(m_cwep.mag_joint, "set_LocalPosition", a + (b - a) * u);

        if (m_cins.slr.has_value() && m_cwep.rest_lr.has_value()) {
            const glm::quat s = *m_cins.slr;
            glm::quat r = *m_cwep.rest_lr;

            if (glm::dot(s, r) < 0.0f) {
                r = glm::quat{-r.w, -r.x, -r.y, -r.z};
            }

            const glm::quat q{s.w + (r.w - s.w) * u, s.x + (r.x - s.x) * u,
                              s.y + (r.y - s.y) * u, s.z + (r.z - s.z) * u};
            const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);

            if (len > 1e-6f) {
                set_quat(m_cwep.mag_joint, "set_LocalRotation",
                         glm::quat{q.w / len, q.x / len, q.y / len, q.z / len});
            }
        }
    }

    if (t < 1.0f) {
        return;
    }

    m_cins.active = false;
    m_crack._ammo_input_t = clock_now();
    m_c_mag_out = false;

    // [NO-SLIDE-GUARD] Ohne Slide-Joint gibt es keinen Rack -> NIE rack.needs
    // setzen (sonst Feuer-Softlock), stattdessen direkt chambern.
    // [LEERE KAMMER] zusaetzlich `rack.empty` -- nach einem Waffenwechsel ist
    // empty_when_dropped geloescht, die Kammer aber leer.
    // [ACCESSOR-FOLGE] `rack.empty` zaehlt nur, wenn die 0 NICHT von unserem
    // eigenen Mag-Drop-Leeren stammt.
    if ((m_crack.empty_when_dropped || (m_crack.empty && !m_crack._zeroed_by_us))
        && m_cwep.slide_joint != nullptr) {
        m_crack.needs = true;
    } else {
        m_crack.needs = false;
        m_crack._zeroed_by_us = false;   // Merker verbraucht

        if (m_crack.empty_when_dropped && m_cwep.slide_joint == nullptr) {
            cf_gun_chamber();
        }

        if (m_cwep.slide_joint != nullptr && !CHI_ENGINE_CLOSES) {
            glm::vec3 cur{};

            if (get_vec3(m_cwep.slide_joint, "get_LocalPosition", cur)) {
                set_vec3(m_cwep.slide_joint, "set_LocalPosition",
                         glm::vec3{cur.x, cur.y, m_cslide.rest_z});
            }
        }
    }

    m_crack.empty_when_dropped = false;
    cf_reload_ammo_on_insert();
}

// =====================================================================
// Slide-Rack (Pull-Geste)
// =====================================================================
void RE4VRReload3::cf_clear_rack() {
    m_crack.needs = false;
    m_crack.grab_active = false;
    m_crack.pulled = false;
    m_crack.frac = 0.0f;
    cf_gun_chamber();

    if (CHI_ENGINE_CLOSES) {
        m_crack._chambered_hold = false;

        return;
    }

    m_crack._chambered_hold = true;

    if (m_cwep.slide_joint != nullptr) {
        glm::vec3 cur{};

        if (get_vec3(m_cwep.slide_joint, "get_LocalPosition", cur)) {
            set_vec3(m_cwep.slide_joint, "set_LocalPosition",
                     glm::vec3{cur.x, cur.y, m_cslide.rest_z});
        }
    }
}

void RE4VRReload3::cf_update_rack_gesture() {
    if (m_crack.tuning) {
        return;
    }

    if (!m_crack.needs) {
        m_crack.grab_active = false;
        m_crack.pulled = false;
        m_crack.frac = 0.0f;
        m_crack.armed = false;

        return;
    }

    auto* sj = m_cwep.slide_joint;

    if (sj == nullptr) {
        return;
    }

    const auto hp = left_hand_world_g();
    glm::vec3 sp{};

    if (!hp.has_value() || !get_vec3(sj, "get_Position", sp)) {
        return;
    }

    const bool grip = left_grip_down();
    const float dist = vec_len(vec_sub(*hp, sp));

    if (!m_crack.grab_active) {
        if (m_c_mag_hand) {
            m_crack.armed = false;

            return;
        }

        if (m_crack._ammo_input_t.has_value()
            && (clock_now() - *m_crack._ammo_input_t) < 0.2) {
            m_crack.armed = false;

            return;
        }

        if (!grip) {
            m_crack.armed = true;

            return;
        }

        if (!m_crack.armed) {
            return;
        }

        if (dist <= CHI_RACK_GRAB_DIST) {
            m_crack.grab_active = true;
            m_crack.armed = false;
            m_crack.pulled = false;
            m_crack.frac = 0.0f;
            m_crack.gx = hp->x;
            m_crack.gy = hp->y;
            m_crack.gz = hp->z;

            // [LAUFEN] Zweiter Anker: die Waffenhand. Ohne ihn steckt die
            // Fortbewegung im Zug und der Slide ging nur im Stand.
            if (const auto rhr = right_hand_raw_g(); rhr.has_value()) {
                m_crack.rgx = rhr->x;
                m_crack.rgy = rhr->y;
                m_crack.rgz = rhr->z;
            } else {
                m_crack.rgx.reset();
                m_crack.rgy.reset();
                m_crack.rgz.reset();
            }

            haptic_left(0.25f, 0.03f);
        }

        return;
    }

    if (grip) {
        const float travel = std::max(std::abs(m_cslide.back_z - c_park_ref(m_cslide)),
                                      0.005f);
        float px = hp->x - m_crack.gx;
        float py = hp->y - m_crack.gy;
        float pz = hp->z - m_crack.gz;

        // [LAUFEN] Bewegung der Waffenhand abziehen -> uebrig bleibt die Bewegung
        // der Ziehhand GEGEN die Waffe. Rueckbau: __re4_rack_relative = false
        if (m_crack.rgx.has_value()
            && re4vr::lua_get_tribool("__re4_rack_relative") != 0) {
            if (const auto rhn = right_hand_raw_g(); rhn.has_value()) {
                px -= (rhn->x - *m_crack.rgx);
                py -= (rhn->y - *m_crack.rgy);
                pz -= (rhn->z - *m_crack.rgz);
            }
        }

        glm::quat srot{};
        float pull = 0.0f;

        if (get_quat(sj, "get_Rotation", srot)) {
            glm::vec3 bd = srot * glm::vec3{0.0f, 0.0f, -1.0f};
            const float bl = vec_len(bd);

            if (bl > 1e-6f) {
                bd /= bl;
            }

            pull = px * bd.x + py * bd.y + pz * bd.z;

            if (pull < 0.0f) {
                pull = 0.0f;
            }
        } else {
            pull = std::sqrt(px * px + py * py + pz * pz);
        }

        const float f = pull / travel;
        m_crack.frac = (f > 1.0f) ? 1.0f : f;

        if (m_crack.frac >= 1.0f && !m_crack.pulled) {
            m_crack.pulled = true;
            cf_snd(CSND_SLIDE_BACK);
        }

        return;
    }

    if (m_crack.pulled) {
        cf_clear_rack();
        haptic_left(0.95f, 0.07f);
        cf_snd(CSND_SLIDE_FORWARD);
    } else {
        m_crack.grab_active = false;
        m_crack.frac = 0.0f;
        m_crack.pulled = false;
        m_crack.armed = true;
    }
}

// Dock-Ziel (Hand folgt dem Slide) publizieren. NUR Slide-Rack -- der Switch
// liegt in motion.
void RE4VRReload3::cf_update_dock_publish(bool advance) {
    ::REManagedObject* src_joint = nullptr;
    float want = 0.0f;

    if ((m_crack.grab_active || m_crack.dock_tune) && m_cwep.slide_joint != nullptr) {
        src_joint = m_cwep.slide_joint;
        want = 1.0f;
    }

    float b = m_crack.dock_blend;

    if (advance) {
        if (b < want) {
            b = std::min(b + CHI_DOCK_BLEND_SPEED, want);
        } else if (b > want) {
            b = std::max(b - CHI_DOCK_BLEND_SPEED, want);
        }

        m_crack.dock_blend = b;
    }

    glm::vec3 p{};
    glm::quat r{};

    if (b > 0.001f && src_joint != nullptr && get_vec3(src_joint, "get_Position", p)
        && get_quat(src_joint, "get_Rotation", r)) {
        const auto& sd = m_cslide;

        if (sd.dock_x != 0.0f || sd.dock_y != 0.0f || sd.dock_z != 0.0f) {
            p += r * glm::vec3{sd.dock_x, sd.dock_y, sd.dock_z};
        }

        if (sd.rack_rx != 0.0f || sd.rack_ry != 0.0f || sd.rack_rz != 0.0f) {
            r = glm::normalize(r * quat_from_euler(sd.rack_rx, sd.rack_ry, sd.rack_rz));
        }

        re4vr::lua_set_vec3("__vr_slide_hand_world_pos", p);
        re4vr::lua_set_quat("__vr_slide_hand_world_rot", r);
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", b * b * (3.0f - 2.0f * b));
    } else {
        re4vr::lua_set_nil("__vr_slide_hand_world_pos");
        re4vr::lua_set_nil("__vr_slide_hand_world_rot");
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    }
}

// [OPEN-BOLT] Die Engine laesst den Bolzen bei LEER hinten -> wir halten ihn
// vorne (park_z). Force NUR bei echtem Ammo-Count == 0 + Reload-Wartezustand +
// manuellem Zug/Tuning. NICHT an gun_ammo_empty haengen (das flackert beim
// Feuern und killt die Schnalz-Animation). Normal feuern (loaded > 0) -> NIE
// geforct -> der Engine-Schnalz bleibt erhalten.
void RE4VRReload3::cf_apply_slide_park() {
    if (CHI_MEASURE_SLIDE) {
        return;
    }

    if (m_cwep.slide_joint == nullptr) {
        return;
    }

    const bool empty_now = m_crack._loaded.has_value() && *m_crack._loaded == 0;

    if (!(m_crack.grab_active || m_crack.tuning || empty_now || m_crack.needs)) {
        return;
    }

    glm::vec3 cur{};

    if (!get_vec3(m_cwep.slide_joint, "get_LocalPosition", cur)) {
        return;
    }

    const float pz = c_park_ref(m_cslide);   // park_z = vorne (Leer-Position)
    float z = pz;                            // leer: Bolzen vorne halten

    if (m_crack.tuning) {
        z = pz + (m_cslide.back_z - pz) * m_crack.tune_frac;
    } else if (m_crack.grab_active) {
        z = pz + (m_cslide.back_z - pz) * m_crack.frac;   // Zug: vorne -> hinten
    }

    set_vec3(m_cwep.slide_joint, "set_LocalPosition", glm::vec3{cur.x, cur.y, z});
}

void RE4VRReload3::cf_apply_mag_out_hidden() {
    const bool should_hide = m_c_mag_out && m_cwep.mag_joint != nullptr && !chicago_flow();

    if (should_hide) {
        set_vec3(m_cwep.mag_joint, "set_LocalScale", glm::vec3{0.0f, 0.0f, 0.0f});
        m_c_mag_hidden = true;
    } else if (m_c_mag_hidden) {
        if (m_cwep.mag_joint != nullptr) {
            set_vec3(m_cwep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
        }

        m_c_mag_hidden = false;
    }
}

// Hand-Pose direkt anwenden (Mag-Halten / Slide-Rack). KEIN Switch (motion).
void RE4VRReload3::cf_apply_hand_pose() {
    std::string name{};
    glm::vec3 thumb{0.0f, 0.0f, 0.0f};

    if (m_c_mag_hand || m_cins.active || m_c_mag_tune) {
        name = "ChicagoMag";
        thumb = glm::vec3{m_cmaghand.t_rx, m_cmaghand.t_ry, m_cmaghand.t_rz};
    } else if (m_crack.dock_tune
               || (m_crack.needs && (m_crack.grab_active || m_c_rack_near))) {
        name = "ChicagoSlide";
        thumb = glm::vec3{m_cslide.st_rx, m_cslide.st_ry, m_cslide.st_rz};
    }

    // In Lua traegt pose_fade_step die Daumen-Daten mit; hier haelt der letzte
    // gehaltene Satz sie fest.
    if (!name.empty()) {
        m_c_hand_fade_thumb = thumb;
    }

    // [POSE_FADE] beim Loslassen ueber POSE_FADE_DUR zurueckblenden statt snappen
    float b = 0.0f;

    if (!pose_fade_step(m_c_hand_fade, name, b)) {
        return;
    }

    const auto pit = m_cposes.find(m_c_hand_fade.name);

    if (pit == m_cposes.end()) {
        return;
    }

    pose_apply(pit->second, b);

    const glm::vec3 ft = m_c_hand_fade_thumb;

    if (ft.x != 0.0f || ft.y != 0.0f || ft.z != 0.0f) {
        auto* bt = body_tf();
        auto* tj = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (tj != nullptr && get_quat(tj, "get_LocalRotation", cur)) {
            set_quat(tj, "set_LocalRotation",
                     glm::normalize(cur * quat_from_euler(ft.x * b, ft.y * b, ft.z * b)));
        }
    }
}

void RE4VRReload3::chicago_soft_reset() {
    cf_stop_drop();
    m_c_mag_hand = false;
    m_cins.active = false;
    m_c_mag_tune = false;
    m_c_mag_out = false;
    m_c_mag_retained = 0;
    m_crack.needs = false;
    m_crack.grab_active = false;
    m_crack.armed = false;
    m_crack.frac = 0.0f;
    m_crack.pulled = false;
    m_crack._chambered_hold = false;
    m_crack.dock_blend = 0.0f;
    m_crack.empty_when_dropped = false;
    m_crack._ammo_input_t.reset();
    m_crack.dock_tune = false;
    m_crack.tuning = false;
    // [ACCESSOR-FOLGE] Merker darf einen Wechsel/Reset nicht ueberleben
    m_crack._zeroed_by_us = false;
}

void RE4VRReload3::chicago_on_frame() {
    chicago_refresh();

    if (m_cwep.wid != m_c_prev_wid) {
        const bool was_chicago = m_c_prev_wid.has_value();
        chicago_soft_reset();

        if (was_chicago && !m_cwep.wid.has_value()) {
            re4vr::lua_set_bool("__vr_needs_rack", false);
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
            re4vr::lua_set_nil("__vr_rack_hand_pose");
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
        }

        if (m_cwep.wid.has_value()) {
            re4vr::lua_set_nil("__re4_live_wi");
        }

        m_c_prev_wid = m_cwep.wid;
    }

    if (!m_cwep.wid.has_value()) {
        return;
    }

    if (m_c_reacquired) {
        m_c_reacquired = false;
        re4vr::lua_set_nil("__re4_live_wi");
        chicago_soft_reset();
    }

    cf_capture_mag_rest();
    cf_check_insert_proximity();

    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);

    const auto loaded = cf_loaded();
    m_crack.empty = cf_gun_ammo_empty();
    // [OPEN-BOLT] echter Count fuer apply_slide_park (Force NUR bei loaded == 0)
    m_crack._loaded = loaded;
    m_crack.has_mag = loaded.has_value() && *loaded > 0;

    if (m_crack._chambered_hold) {
        if (m_crack._prev_ga.has_value() && loaded.has_value()
            && *loaded < *m_crack._prev_ga) {
            m_crack._chambered_hold = false;
        }

        m_crack._prev_ga = loaded;
    }

    if (m_c_mag_out && !m_cins.active) {
        auto* wi = cf_get_wi();

        if (wi != nullptr && call_enum(wi, "get_CurrentAmmoCount").value_or(0) > 0) {
            if (m_main != nullptr) {
                // [MAG-REST] merken, bevor genullt wird
                m_main->carry_capture(wi, "re4_vr_reload3.lua:860", std::nullopt);
            }

            field_i32_write(wi, "_CurrentAmmoCount", 0x44, 0);
        }
    }

    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        m_c_mag_out && !m_c_mag_hand && !m_cins.active
                        && !((m_c_mag_retained + cf_reserve()) > 0));

    cf_update_rack_gesture();

    {
        auto* sj = m_cwep.slide_joint;
        const auto hp = (sj != nullptr) ? left_hand_world_g() : std::nullopt;
        glm::vec3 sp{};
        m_c_rack_near = hp.has_value() && get_vec3(sj, "get_Position", sp)
                        && vec_len(vec_sub(*hp, sp)) <= CHI_RACK_GRAB_DIST;
    }

    re4vr::lua_set_bool("__vr_needs_rack", m_crack.needs);
    re4vr::lua_set_bool("__vr_slide_rack_active", m_crack.grab_active);
    re4vr::lua_set_bool("__vr_mag_in_hand",
                        m_c_mag_hand || m_cins.active
                        || (m_crack._ammo_input_t.has_value()
                            && (clock_now() - *m_crack._ammo_input_t) < 0.2));

    if (m_crack.needs && (m_crack.grab_active || m_c_rack_near)) {
        re4vr::lua_set_string("__vr_rack_hand_pose", "ChicagoSlide");
    } else {
        re4vr::lua_set_nil("__vr_rack_hand_pose");
    }

    // [KRITISCHES GATE -- LIVE] Feuer-Block NUR aus frischen Engine-Quellen. Den
    // Switch-/Burst-Fire-Block macht motion selbst -> hier NUR Reload-Gruende.
    re4vr::lua_set_bool("__vr_block_fire_when_empty",
                        m_crack.needs || m_c_mag_out || chicago_flow() || m_crack.empty);
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload3.lua:856");
    re4vr::lua_set_bool("__vr_rack_block_left_knife", m_crack.needs);

    cf_update_dock_publish(true);

    if (m_c_mag_floor_at > 0.0 && clock_now() >= m_c_mag_floor_at) {
        m_c_mag_floor_at = 0.0;
        cf_snd(CSND_MAG_FLOOR);
    }

    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_c_dry_prev) {
        cf_snd(CSND_DRY_FIRE);
    }

    m_c_dry_prev = et;

    const bool bd = right_b_down();
    // [MAG_B_GUARD 2026-09-10] Nur Karte, Typewriter und Inventar: dort ist der
    // rechte B Zentrieren bzw. Zurueck und darf die Waffe nicht anfassen.
    // Geblockt wird nur die AKTION: die Flanke wird weiter gepflegt, sonst
    // feuert ein beim Schliessen noch gehaltener B sofort den Auswurf.
    const bool b_menu = re4vr::lua_get_tribool("__re4_mag_block") == 1;

    if (bd && !m_c_rb_prev && !b_menu) {
        cf_force_eject();
    }

    m_c_rb_prev = bd;
}

// Render-Pass (voller Override-Stack; NACH reload + reload2 -> gewinnt)
void RE4VRReload3::chicago_apply_pass() {
    if (!(m_ccfg.chicago_enabled && m_cwep.wid.has_value())) {
        return;
    }

    cf_update_drop();
    cf_update_mag_in_hand();
    cf_update_insert();
    cf_apply_slide_park();
    cf_apply_mag_out_hidden();
    cf_update_dock_publish(false);
    cf_apply_hand_pose();
}

void RE4VRReload3::chicago_on_script_reset() {
    if (m_cwep.mag_joint != nullptr) {
        set_vec3(m_cwep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
    }

    m_cwep = ChicagoWep{};
    chicago_soft_reset();
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_slide_rack_active", false);
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    re4vr::lua_set_nil("__vr_rack_hand_pose");
}

// ---------------------------------------------------------------------
// UI -- Chicago Sweeper
// ---------------------------------------------------------------------
void RE4VRReload3::chicago_ui() {
    if (!ImGui::TreeNode("Chicago Sweeper -- Einstellungen")) {
        return;
    }

    if (ImGui::Checkbox("##chicago_en", &m_ccfg.chicago_enabled)) {
        chicago_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Chicago Sweeper Reload");

    const auto awid = m_cwep.wid.has_value() ? m_cwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_chicago(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Chicago erkannt - verwaltet)"
                             : "  (keine Chicago Sweeper equippt)");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Joints: Mag=%s  Slide=%s  (mag_joint=%s)",
                       known ? CHI_J_MAG : "nil", known ? CHI_J_SLIDE : "nil",
                       m_cwep.mag_joint != nullptr ? "ok" : "nil");

    const auto ld = cf_loaded();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  loaded=%s reserve=%d cap=%d mag_out=%s needs_rack=%s empty=%s",
                       ld.has_value() ? std::to_string(*ld).c_str() : "nil",
                       cf_reserve(), cf_cap(),
                       m_c_mag_out ? "true" : "false",
                       m_crack.needs ? "true" : "false",
                       m_crack.empty ? "true" : "false");
    ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                       "  Ausbaustufe: Box-Mag (joint_04) wird gemanagt; "
                       "Trommel-Stufe faellt automatisch durch (spaeter).");
    ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                       "  Switch + Burst bleiben in motion.lua (gemeinsam mit LE5).");

    if (ImGui::SliderFloat("Einlege-Distanz m (Mag)##chidist",
                           &m_ccfg.insert_distance, 0.03f, 0.50f)) {
        chicago_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen", &m_ccfg.reload_ammo)) {
        chicago_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an", &m_ccfg.sound_enabled)) {
        chicago_save_cfg();
    }

    if (ImGui::TreeNode("Slide-Pose (Z + Hand-Dock)")) {
        auto& sp = m_cslide;
        bool ch = false;
        ImGui::Checkbox("Vorschau: Slide-Z per Regler##chitune", &m_crack.tuning);

        if (m_crack.tuning) {
            ImGui::SliderFloat("Vorschau 0..1##chitunef", &m_crack.tune_frac, 0.0f, 1.0f);
        }

        ImGui::Checkbox("Vorschau: Hand ans Slide-Dock zwingen##chidocktune",
                        &m_crack.dock_tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Dock-Vorschau erzwingt die Hand an den Slide -> "
                           "dock_x/y/z + rack_rx/ry/rz live justierbar)");
        ch |= ImGui::SliderFloat("rest_z (vorne/gechambert)##chirz", &sp.rest_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("park_z (leer/MITTEL)##chipz", &sp.park_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("back_z (Rack-Endpunkt)##chibz", &sp.back_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("empty_x (X-Ausfahren bei leer)##chiex", &sp.empty_x, -0.10f, 0.10f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Hand-Dock am Slide:");
        ch |= ImGui::SliderFloat("dock_x##chidx", &sp.dock_x, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_y##chidy", &sp.dock_y, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_z##chidz", &sp.dock_z, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("rack_rx##chirrx", &sp.rack_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("rack_ry##chirry", &sp.rack_ry, -180.0f, 360.0f);
        ch |= ImGui::SliderFloat("rack_rz##chirrz", &sp.rack_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen der Slide-Zieh-Pose (additiv):");
        ch |= ImGui::SliderFloat("Daumen RotX##chistx", &sp.st_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##chisty", &sp.st_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##chistz", &sp.st_rz, -90.0f, 90.0f);

        if (ch) {
            chicago_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Dock-Port (Mag-Einlegepunkt)")) {
        bool ch = false;
        char jb[64]{};
        std::snprintf(jb, sizeof(jb), "%s", m_cdock.joint.c_str());

        if (ImGui::InputText("Port-Joint##chidj", jb, sizeof(jb))) {
            m_cdock.joint = jb;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Port X##chipx", &m_cdock.x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Port Y##chipy", &m_cdock.y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Port Z##chipz2", &m_cdock.z, -0.20f, 0.20f);

        if (ch) {
            chicago_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Mag in Hand (Offset + Daumen)")) {
        auto& m = m_cmaghand;
        bool ch = false;
        ImGui::Checkbox("Vorschau: Mag in der Hand zwingen##chimagtune", &m_c_mag_tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (erzwingt Mag+Pose dauerhaft in der linken Hand -> "
                           "Offset live justierbar)");
        ch |= ImGui::SliderFloat("PosX##chimx", &m.x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosY##chimy", &m.y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosZ##chimz", &m.z, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("RotX##chimrx", &m.rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotY##chimry", &m.ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotZ##chimrz", &m.rz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Daumen RotX##chitx", &m.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##chity", &m.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##chitz", &m.t_rz, -90.0f, 90.0f);

        if (ch) {
            chicago_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Chicago = LE5-Reload-Logik (Mag _04 + Slide-Rack _02). "
                       "Werte von LE5 geseedet.");
    ImGui::TreePop();
}

// ============================================================================
// 2 -- HANDCANNON-REVOLVER (wp4502, Lua Z.1115-2682)
//
// 1:1-Port der Broken-Butterfly-Logik, aber EIGENSTAENDIG: eigene Daten, eigenes
// JSON, eigene UI. reload2 fasst 4502 nicht mehr an -> kein Konflikt. Tunen der
// Handcannon trifft NIE die Broken Butterfly.
//
// JOINTS (bestaetigt): _02 = Spannhahn. _04 = TROMMEL (Chambers + Patronen,
// schwenkt zur Seite raus). _05 = die Trommel selbst, bohrungszentriert -> sie
// dreht sauber an Ort um das lokale Z (BB-konform). Swing UND Chamber-Spin
// brauchen getrennte Joints, sonst kollidieren die Overrides:
//   cyl_joint (Swing, Right-B) = _04
//   spin/index + Eject + Patronen-Referenz = _05
// _07.._11 = 5 Patronen (fuer Kipp-und-Fallen-Eject).
// Eigenes JSON: re4_vr/re4_vr_reload3_handcannon.json
// ============================================================================
namespace {

constexpr int32_t HC = 4502;
constexpr const char* HC_CFG_PATH = "re4_vr/re4_vr_reload3_handcannon.json";

constexpr const char* HC_J_CYL = "_04";
constexpr const char* HC_J_BULLET = "_07";
constexpr const char* HC_J_INSERT = "_04";
constexpr const char* HC_J_SPIN = "_05";
constexpr const char* HC_J_HAND_CART = "_101";
constexpr const char* HC_J_HAMMER = "_02";
const char* const HC_BULLETS[5] = {"_07", "_08", "_09", "_10", "_11"};

constexpr uint32_t HSND_CYLINDER = 942865223u;
constexpr uint32_t HSND_COCK = 938556079u;
constexpr uint32_t HSND_INSERT = 942865223u;
constexpr uint32_t HSND_DROP = 1351699582u;
constexpr uint32_t HSND_MAG_HOLSTER = 1839787494u;
constexpr uint32_t HSND_DRY_FIRE = 812850326u;

constexpr float HC_GRAVITY = 9.8f;
constexpr float HC_DROP_DUR = 1.0f;

constexpr float HC_EJECT_DOWN = 0.6f;
constexpr float HC_EJECT_ZSIGN = -1.0f;
constexpr float HC_EJECT_SLIDE_DUR = 0.13f;
constexpr float HC_EJECT_SLIDE_DIST = 0.055f;
constexpr float HC_EJECT_STAGGER = 0.06f;

constexpr float HC_FLICK_VEL = 8.0f;
constexpr double HC_FLICK_REVERSAL = 0.30;
constexpr double HC_FLICK_OPEN_GRACE = 0.35;
constexpr double HC_FLICK_SND_DELAY = 0.18;

// [SLERP] kuerzester Bogen; verhindert den Euler-"Kreis" (die Achse wandert) bei
// grossen Winkeln.
glm::quat qslerp(const glm::quat& a, glm::quat b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;

    if (dot < 0.0f) {
        b = glm::quat{-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }

    if (dot > 0.9995f) {
        return glm::normalize(glm::quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                                        a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
    }

    const float theta = std::acos(dot);
    const float s = std::sin(theta);
    const float w1 = std::sin((1.0f - t) * theta) / s;
    const float w2 = std::sin(t * theta) / s;

    return glm::normalize(glm::quat{a.w * w1 + b.w * w2, a.x * w1 + b.x * w2,
                                    a.y * w1 + b.y * w2, a.z * w1 + b.z * w2});
}

}   // namespace

bool RE4VRReload3::is_handcannon(int32_t wid) {
    return wid == HC;
}

std::vector<RE4VRReload3::HcThumbKey>& RE4VRReload3::hc_thumb_keys(bool aim) {
    return aim ? m_htkeys_aim : m_htkeys;
}

// [DAUMEN-KEYS] Kurve an der Phase p (0..100) abtasten. Vor dem ersten Key von
// NEUTRAL hoch, nach dem letzten wieder auf NEUTRAL -> kein Key bei 0/100 noetig.
bool RE4VRReload3::hc_thumb_key_sample(const std::vector<HcThumbKey>& list, float p,
                                       std::array<HcThumbKeyJ, 3>& out) {
    for (auto& o : out) {
        o = HcThumbKeyJ{};
    }

    const size_t n = list.size();

    if (n == 0) {
        return false;
    }

    const HcThumbKey* lo = nullptr;
    const HcThumbKey* hi = nullptr;
    std::optional<float> t{};

    if (p <= list[0].p) {
        hi = &list[0];
        t = (list[0].p > 0.0001f) ? (p / list[0].p) : 1.0f;
    } else if (p >= list[n - 1].p) {
        lo = &list[n - 1];
        const float span = 100.0f - list[n - 1].p;
        t = (span > 0.0001f) ? ((p - list[n - 1].p) / span) : 1.0f;
    } else {
        for (size_t i = 0; i + 1 < n; ++i) {
            if (p >= list[i].p && p <= list[i + 1].p) {
                lo = &list[i];
                hi = &list[i + 1];
                const float span = hi->p - lo->p;
                t = (span > 0.0001f) ? ((p - lo->p) / span) : 0.0f;
                break;
            }
        }
    }

    if (!t.has_value()) {
        return false;
    }

    const float tt = clamp01(*t);

    for (size_t i = 0; i < 3; ++i) {
        const HcThumbKeyJ a = (lo != nullptr) ? lo->j[i] : HcThumbKeyJ{};
        const HcThumbKeyJ b = (hi != nullptr) ? hi->j[i] : HcThumbKeyJ{};
        auto& o = out[i];
        o.x = a.x + (b.x - a.x) * tt;
        o.y = a.y + (b.y - a.y) * tt;
        o.z = a.z + (b.z - a.z) * tt;
        o.px = a.px + (b.px - a.px) * tt;
        o.py = a.py + (b.py - a.py) * tt;
        o.pz = a.pz + (b.pz - a.pz) * tt;
    }

    return true;
}

void RE4VRReload3::hc_load_cfg() {
    const auto data = re4vr::json_load(HC_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    const nlohmann::json& c = (data.contains("cfg") && data["cfg"].is_object())
                              ? data["cfg"] : data;
    m_hcfg.revolver_enabled = jbool(c, "revolver_enabled", m_hcfg.revolver_enabled);
    m_hcfg.reload_ammo = jbool(c, "reload_ammo", m_hcfg.reload_ammo);
    m_hcfg.sound_enabled = jbool(c, "sound_enabled", m_hcfg.sound_enabled);
    m_hcfg.insert_distance = jnum(c, "insert_distance", m_hcfg.insert_distance);
    m_hcfg.support_cooldown = jnum(c, "support_cooldown", m_hcfg.support_cooldown);
    m_hcfg.cock_press = jnum(c, "cock_press", m_hcfg.cock_press);
    m_hcfg.cock_hold = jnum(c, "cock_hold", m_hcfg.cock_hold);
    m_hcfg.cock_return = jnum(c, "cock_return", m_hcfg.cock_return);
    m_hcfg.cock_fall = jnum(c, "cock_fall", m_hcfg.cock_fall);

    if (const auto it = data.find("cyl"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4502"); v != it->end() && v->is_object()) {
            auto& r = m_hcyl;
            r.rx = jnum(*v, "rx", r.rx);
            r.ry = jnum(*v, "ry", r.ry);
            r.rz = jnum(*v, "rz", r.rz);
            r.px = jnum(*v, "px", r.px);
            r.py = jnum(*v, "py", r.py);
            r.pz = jnum(*v, "pz", r.pz);
            r.lerp = jnum(*v, "lerp", r.lerp);
            r.shot_deg = jnum(*v, "shot_deg", r.shot_deg);
            r.shot_lerp = jnum(*v, "shot_lerp", r.shot_lerp);
        }
    }

    if (const auto it = data.find("shell"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4502"); v != it->end() && v->is_object()) {
            auto& s = m_hshell;
            s.pose = jstr(*v, "pose", s.pose);
            s.parts = jstr(*v, "parts", s.parts);
            s.x = jnum(*v, "x", s.x);
            s.y = jnum(*v, "y", s.y);
            s.z = jnum(*v, "z", s.z);
            s.rx = jnum(*v, "rx", s.rx);
            s.ry = jnum(*v, "ry", s.ry);
            s.rz = jnum(*v, "rz", s.rz);
            s.t_rx = jnum(*v, "t_rx", s.t_rx);
            s.t_ry = jnum(*v, "t_ry", s.t_ry);
            s.t_rz = jnum(*v, "t_rz", s.t_rz);
            s.i_rx = jnum(*v, "i_rx", s.i_rx);
            s.i_ry = jnum(*v, "i_ry", s.i_ry);
            s.i_rz = jnum(*v, "i_rz", s.i_rz);
            s.scale = jnum(*v, "scale", s.scale);
        }
    }

    if (const auto it = data.find("hammer"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4502"); v != it->end() && v->is_object()) {
            auto& h = m_hham_cfg;
            h.idle_rx = jnum(*v, "idle_rx", h.idle_rx);
            h.idle_ry = jnum(*v, "idle_ry", h.idle_ry);
            h.idle_rz = jnum(*v, "idle_rz", h.idle_rz);
            h.rx = jnum(*v, "rx", h.rx);
            h.ry = jnum(*v, "ry", h.ry);
            h.rz = jnum(*v, "rz", h.rz);
            h.lerp = jnum(*v, "lerp", h.lerp);
        }
    }

    const auto read_off = [](const nlohmann::json& v, HcHammerState::Off& o) {
        o.rx = jnum(v, "rx", o.rx);
        o.ry = jnum(v, "ry", o.ry);
        o.rz = jnum(v, "rz", o.rz);
        o.px = jnum(v, "px", o.px);
        o.py = jnum(v, "py", o.py);
        o.pz = jnum(v, "pz", o.pz);
    };

    if (const auto it = data.find("cockhand"); it != data.end() && it->is_object()) {
        read_off(*it, m_hham.hand);
    }

    if (const auto it = data.find("cockhand_aim"); it != data.end() && it->is_object()) {
        read_off(*it, m_hham.hand_aim);
    } else {
        const bool pv = m_hham.hand_aim.preview;
        m_hham.hand_aim = m_hham.hand;
        m_hham.hand_aim.preview = pv;
    }

    if (const auto it = data.find("thumbpos"); it != data.end() && it->is_object()) {
        m_hham.thumb_pos = glm::vec3{jnum(*it, "x", m_hham.thumb_pos.x),
                                     jnum(*it, "y", m_hham.thumb_pos.y),
                                     jnum(*it, "z", m_hham.thumb_pos.z)};
    }

    if (const auto it = data.find("thumbpos_aim"); it != data.end() && it->is_object()) {
        m_hham.thumb_pos_aim = glm::vec3{jnum(*it, "x", m_hham.thumb_pos_aim.x),
                                         jnum(*it, "y", m_hham.thumb_pos_aim.y),
                                         jnum(*it, "z", m_hham.thumb_pos_aim.z)};
    } else {
        m_hham.thumb_pos_aim = m_hham.thumb_pos;
    }

    const auto read_tcfg = [](const nlohmann::json& src,
                              std::array<HcThumbJoint, 3>& dst) {
        const auto v = src.find("4502");

        if (v == src.end() || !v->is_array()) {
            return false;
        }

        for (size_t i = 0; i < 3 && i < v->size(); ++i) {
            const auto& e = (*v)[i];

            if (!e.is_object()) {
                continue;
            }

            dst[i].ix = jnum(e, "ix", dst[i].ix);
            dst[i].iy = jnum(e, "iy", dst[i].iy);
            dst[i].iz = jnum(e, "iz", dst[i].iz);
            dst[i].cx = jnum(e, "cx", dst[i].cx);
            dst[i].cy = jnum(e, "cy", dst[i].cy);
            dst[i].cz = jnum(e, "cz", dst[i].cz);
        }

        return true;
    };

    if (const auto it = data.find("thumb"); it != data.end() && it->is_object()) {
        read_tcfg(*it, m_htcfg);
    }

    // Fehlt der Aim-Satz, ist er eine Kopie des No-Aim-Satzes.
    bool aim_loaded = false;

    if (const auto it = data.find("thumb_aim"); it != data.end() && it->is_object()) {
        aim_loaded = read_tcfg(*it, m_htcfg_aim);
    }

    if (!aim_loaded) {
        m_htcfg_aim = m_htcfg;
    }

    // [DAUMEN-KEYS] Stuetzpunkte laden, nach Phase sortiert.
    const auto read_keys = [](const nlohmann::json& src, std::vector<HcThumbKey>& dst) {
        const auto list = src.find("4502");

        if (list == src.end() || !list->is_array()) {
            return;
        }

        std::vector<HcThumbKey> out;

        for (const auto& key : *list) {
            if (!key.is_object() || !key.contains("p") || !key.contains("j")) {
                continue;
            }

            HcThumbKey k{};
            k.p = jnum(key, "p", 0.0f);
            const auto& j = key["j"];

            for (size_t n = 0; n < 3 && j.is_array() && n < j.size(); ++n) {
                if (!j[n].is_object()) {
                    continue;
                }

                k.j[n].x = jnum(j[n], "x", 0.0f);
                k.j[n].y = jnum(j[n], "y", 0.0f);
                k.j[n].z = jnum(j[n], "z", 0.0f);
                k.j[n].px = jnum(j[n], "px", 0.0f);
                k.j[n].py = jnum(j[n], "py", 0.0f);
                k.j[n].pz = jnum(j[n], "pz", 0.0f);
            }

            out.push_back(k);
        }

        std::sort(out.begin(), out.end(),
                  [](const HcThumbKey& a, const HcThumbKey& b) { return a.p < b.p; });
        dst = out;
    };

    if (const auto it = data.find("thumbkeys"); it != data.end() && it->is_object()) {
        read_keys(*it, m_htkeys);
    }

    if (const auto it = data.find("thumbkeys_aim"); it != data.end() && it->is_object()) {
        read_keys(*it, m_htkeys_aim);
    }
}

void RE4VRReload3::hc_save_cfg() {
    const auto& r = m_hcyl;
    const auto& s = m_hshell;
    const auto& h = m_hham_cfg;

    const auto ser_tcfg = [](const std::array<HcThumbJoint, 3>& src) {
        nlohmann::json arr = nlohmann::json::array();

        for (const auto& j : src) {
            arr.push_back({{"ix", j.ix}, {"iy", j.iy}, {"iz", j.iz},
                           {"cx", j.cx}, {"cy", j.cy}, {"cz", j.cz}});
        }

        return nlohmann::json{{"4502", arr}};
    };

    const auto ser_keys = [](const std::vector<HcThumbKey>& m) {
        nlohmann::json arr = nlohmann::json::array();

        for (const auto& key : m) {
            nlohmann::json j = nlohmann::json::array();

            for (const auto& e : key.j) {
                j.push_back({{"x", e.x}, {"y", e.y}, {"z", e.z},
                             {"px", e.px}, {"py", e.py}, {"pz", e.pz}});
            }

            arr.push_back({{"p", key.p}, {"j", j}});
        }

        return nlohmann::json{{"4502", arr}};
    };

    const auto& ch = m_hham.hand;
    const auto& ca = m_hham.hand_aim;

    nlohmann::json d = {
        {"cfg", {{"revolver_enabled", m_hcfg.revolver_enabled},
                 {"insert_distance", m_hcfg.insert_distance},
                 {"reload_ammo", m_hcfg.reload_ammo},
                 {"sound_enabled", m_hcfg.sound_enabled},
                 {"cock_press", m_hcfg.cock_press}, {"cock_hold", m_hcfg.cock_hold},
                 {"cock_return", m_hcfg.cock_return}, {"cock_fall", m_hcfg.cock_fall},
                 {"support_cooldown", m_hcfg.support_cooldown}}},
        {"cyl", {{"4502", {{"rx", r.rx}, {"ry", r.ry}, {"rz", r.rz},
                           {"px", r.px}, {"py", r.py}, {"pz", r.pz},
                           {"lerp", r.lerp}, {"shot_deg", r.shot_deg},
                           {"shot_lerp", r.shot_lerp}}}}},
        {"shell", {{"4502", {{"pose", s.pose}, {"x", s.x}, {"y", s.y}, {"z", s.z},
                             {"rx", s.rx}, {"ry", s.ry}, {"rz", s.rz},
                             {"t_rx", s.t_rx}, {"t_ry", s.t_ry}, {"t_rz", s.t_rz},
                             {"i_rx", s.i_rx}, {"i_ry", s.i_ry}, {"i_rz", s.i_rz},
                             {"parts", s.parts}, {"scale", s.scale}}}}},
        {"hammer", {{"4502", {{"idle_rx", h.idle_rx}, {"idle_ry", h.idle_ry},
                              {"idle_rz", h.idle_rz}, {"rx", h.rx}, {"ry", h.ry},
                              {"rz", h.rz}, {"lerp", h.lerp}}}}},
        {"thumb", ser_tcfg(m_htcfg)},
        {"thumb_aim", ser_tcfg(m_htcfg_aim)},
        {"cockhand", {{"rx", ch.rx}, {"ry", ch.ry}, {"rz", ch.rz},
                      {"px", ch.px}, {"py", ch.py}, {"pz", ch.pz}}},
        {"cockhand_aim", {{"rx", ca.rx}, {"ry", ca.ry}, {"rz", ca.rz},
                          {"px", ca.px}, {"py", ca.py}, {"pz", ca.pz}}},
        {"thumbpos", {{"x", m_hham.thumb_pos.x}, {"y", m_hham.thumb_pos.y},
                      {"z", m_hham.thumb_pos.z}}},
        {"thumbpos_aim", {{"x", m_hham.thumb_pos_aim.x}, {"y", m_hham.thumb_pos_aim.y},
                          {"z", m_hham.thumb_pos_aim.z}}},
        {"thumbkeys", ser_keys(m_htkeys)},
        {"thumbkeys_aim", ser_keys(m_htkeys_aim)},
    };

    re4vr::json_save(HC_CFG_PATH, d);
}

void RE4VRReload3::hc_play_sound(uint32_t id) {
    if (!m_hcfg.sound_enabled || id == 0 || m_hwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_hwep.tf, "get_GameObject"), id);
}

void RE4VRReload3::hc_refresh_weapon() {
    const auto ewid = get_equip_wid();
    const bool managed = m_hcfg.revolver_enabled && ewid.has_value()
                         && is_handcannon(*ewid);

    if (!managed) {
        hc_cart_destroy();
        m_hwep = HcWep{};

        return;
    }

    if (m_hwep.wid.has_value() && m_hwep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_hwep.tf, "get_Position", p)) {
            return;
        }
    }

    m_hwep = HcWep{};

    auto* tf = find_weapon(*ewid);

    if (tf == nullptr) {
        return;
    }

    m_hwep.wid = *ewid;
    m_hwep.tf = tf;
    m_hwep.cyl_joint = joint_by_name(tf, HC_J_CYL);

    if (m_hwep.cyl_joint != nullptr) {
        glm::quat r{};
        glm::vec3 p{};

        if (get_quat(m_hwep.cyl_joint, "get_LocalRotation", r)) {
            m_hwep.cyl_rest_rot = r;
        }

        if (get_vec3(m_hwep.cyl_joint, "get_LocalPosition", p)) {
            m_hwep.cyl_rest_pos = p;
        }
    }

    for (const char* bn : HC_BULLETS) {
        auto* bj = joint_by_name(tf, bn);

        if (bj == nullptr) {
            continue;
        }

        HcWep::Bullet b{};
        b.joint = bj;
        b.name = bn;
        glm::vec3 rs{};

        if (get_vec3(bj, "get_LocalScale", rs) && rs.x > 0.01f) {
            b.vis = rs;
        }

        m_hwep.bullets.push_back(b);
    }

    m_hwep.insert_joint = joint_by_name(tf, HC_J_INSERT);
    m_hwep.spin_joint = joint_by_name(tf, HC_J_SPIN);

    if (m_hwep.spin_joint != nullptr) {
        glm::quat r{};

        if (get_quat(m_hwep.spin_joint, "get_LocalRotation", r)) {
            m_hwep.spin_rest_rot = r;
        }
    }

    m_hwep.hand_cart_joint = joint_by_name(tf, HC_J_HAND_CART);
    m_hwep.hammer_joint = joint_by_name(tf, HC_J_HAMMER);

    // [SAVE-LOAD-FEST] Ruhe = BIND-Pose (get_BaseLocalRotation), eine
    // Modell-Konstante, die NIE von unserem Override kontaminiert wird.
    if (m_hwep.hammer_joint != nullptr) {
        glm::quat r{};

        if (get_quat(m_hwep.hammer_joint, "get_BaseLocalRotation", r)) {
            m_hwep.hammer_rest_rot = r;
        }
    }
}

// [ACCESSOR] zuerst die ECHTE, persistente Instanz. Alles darunter sind KOPIEN
// -> Schreiben verpufft.
::REManagedObject* RE4VRReload3::hc_get_live_wi() {
    if (m_main != nullptr) {
        if (auto* rw = m_main->real_wi(); rw != nullptr) {
            return rw;
        }
    }

    auto* ewi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

bool RE4VRReload3::hc_gun_is_full(::REManagedObject* wi) {
    bool bf = false;

    if (re4vr::try_call<bool>(wi, "get_IsBulletFull", bf)) {
        return bf;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    const int32_t maxc = call_enum(wi, "get_CurrentAmmoMax").value_or(0);

    return maxc > 0 && loaded >= maxc;
}

// [HANDCANNON-AMMO] Baugleicher Revolver wie die Broken Butterfly: write_dword
// @0x44 greift bei OFFENER Trommel NICHT (getCurrentGunAmmo bleibt stehen) ->
// der Ladeweg (+1), exakt der, der bei Butterfly/Shotguns/Pistolen funktioniert.
// Er zieht die Reserve selbst.
bool RE4VRReload3::hc_insert_one_round() {
    if (!m_hcfg.reload_ammo || m_main == nullptr) {
        return false;
    }

    auto* wi = hc_get_live_wi();

    if (wi == nullptr) {
        return false;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);

    if (hc_gun_is_full(wi)) {
        return false;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");
    const int32_t r_b4 = (inv != nullptr && ammo_id.has_value())
        ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (r_b4 <= 0) {
        return false;
    }

    std::optional<int32_t> et{};

    if (auto* td = sdk::find_type_definition("chainsaw.EquipType"); td != nullptr) {
        if (auto* f = td->get_field("Main"); f != nullptr) {
            et = f->get_data<int32_t>(nullptr);
        }
    }

    const int32_t before = call_enum(pe, "getCurrentGunAmmo").value_or(loaded);

    if (inv != nullptr && et.has_value()) {
        m_main->load_and_book(inv, *et, 1, false);
    }

    return call_enum(pe, "getCurrentGunAmmo").value_or(before) > before;
}

bool RE4VRReload3::hc_can_grab() {
    auto* wi = hc_get_live_wi();

    if (wi == nullptr || m_main == nullptr) {
        return false;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(get_pe(), "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");
    const int32_t reserve = (inv != nullptr && ammo_id.has_value())
        ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return false;
    }

    return !hc_gun_is_full(wi);
}

bool RE4VRReload3::hc_set_mag_in_hand(bool active) {
    if (active) {
        if (m_hrev.cart) {
            return true;
        }

        // [SHELL-KEYFRAMES] Diese Greif-Session hat schon eine Bahn gefahren ->
        // erst loslassen und neu greifen.
        if (m_hrev.kf_used) {
            return false;
        }

        if (!hc_can_grab()) {
            return false;
        }

        m_hrev.cart = true;
        m_hdrop.active = false;
        hc_play_sound(HSND_MAG_HOLSTER);

        return true;
    }

    // [SHELL-KEYFRAMES] Bahn laeuft -> Loslassen NICHT als Drop werten (sonst
    // faellt die Patrone und die Bahn bricht ab = kein Insert).
    if (m_hrev.kf_active) {
        m_hrev.kf_used = false;

        return true;
    }

    auto* tf = (m_hcart.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform") : nullptr;
    glm::vec3 p{};

    if (m_hrev.cart && tf != nullptr && get_vec3(tf, "get_Position", p)) {
        m_hdrop.active = true;
        m_hdrop.snd = false;
        m_hdrop.sx = p.x;
        m_hdrop.sy = p.y;
        m_hdrop.sz = p.z;
        m_hdrop.t0 = clock_now();

        // [NO_LAG] Klon war ans L_Hand geparentet -> fuer den Welt-Freifall
        // entkoppeln.
        if (m_hcart.parented) {
            re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
            m_hcart.parented = false;
            m_hcart.pmode.clear();
        }
    }

    m_hrev.cart = false;
    m_hrev.kf_used = false;

    return true;
}

std::optional<glm::vec3> RE4VRReload3::hc_held_cartridge_pos() {
    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return std::nullopt;
    }

    glm::quat hr{};

    if (get_quat(lhj, "get_Rotation", hr)) {
        return hp + (hr * glm::vec3{m_hshell.x, m_hshell.y, m_hshell.z});
    }

    return hp;
}

void RE4VRReload3::hc_update_reload() {
    if (!(m_hcfg.revolver_enabled && m_hcfg.reload_ammo && m_hwep.wid.has_value()
          && m_hwep.insert_joint != nullptr)) {
        return;
    }

    if (!m_hrev.cart) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_revolver_cyl_open") != 1) {
        return;
    }

    const auto cp = hc_held_cartridge_pos();

    if (!cp.has_value()) {
        return;
    }

    glm::vec3 ip{};

    if (!get_vec3(m_hwep.insert_joint, "get_Position", ip)) {
        return;
    }

    if (vec_len(vec_sub(*cp, ip)) > m_hcfg.insert_distance) {
        return;
    }

    const int32_t wid = *m_hwep.wid;

    if (m_adv != nullptr && m_adv->has_shell_keys(wid)) {
        // [SHELL-KEYFRAMES] Bahn (visuelle Deko) IMMER starten, entkoppelt vom
        // Insert.
        if (!m_hrev.kf_active && !m_hrev.kf_used) {
            m_hrev.kf_active = true;
            m_hrev.kf_hold_t.reset();
            m_hrev.kf_used = true;
            m_hrev.kf_inserted = false;
            m_hrev.kf_t0 = clock_now();

            if (m_hcart.obj != nullptr && m_hcart.parented) {
                auto* tf = re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform");

                if (tf != nullptr) {
                    re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                }

                m_hcart.parented = false;
                m_hcart.pmode.clear();
            }
        }

        // +1 jeden Frame versuchen (RETRY), bis der Ladeweg greift.
        if (m_hrev.cart && !m_hrev.kf_inserted && hc_insert_one_round()) {
            m_hrev.kf_inserted = true;
            m_hrev._insert_t = clock_now();
            hc_play_sound(HSND_INSERT);
        }

        return;
    }

    if (hc_insert_one_round()) {
        m_hrev.cart = false;
        // [SUPPORT-COOLDOWN] Support-Hand nach dem Insert kurz NICHT andocken
        m_hrev._insert_t = clock_now();
        hc_play_sound(HSND_INSERT);
    }
}

void RE4VRReload3::hc_update_cylinder() {
    if (!(m_hwep.wid.has_value() && m_hwep.cyl_joint != nullptr)) {
        return;
    }

    if (m_hcyl_st.preview) {
        re4vr::lua_set_bool("__vr_revolver_cyl_open", m_hcyl_st.prog > 0.15f);

        return;
    }

    const bool b = right_b_down();

    if (b && !m_hcyl_st._prev_b) {
        m_hcyl_st.open = !m_hcyl_st.open;
        // [SOUND] OEFFNEN = Trommel-ID, SCHLIESSEN = Cock-ID (getauscht)
        hc_play_sound(m_hcyl_st.open ? HSND_CYLINDER : HSND_COCK);
    }

    m_hcyl_st._prev_b = b;

    const float target = m_hcyl_st.open ? 1.0f : 0.0f;

    if (m_hcyl_st.prog < target) {
        m_hcyl_st.prog = std::min(target, m_hcyl_st.prog + m_hcyl.lerp);
    } else if (m_hcyl_st.prog > target) {
        m_hcyl_st.prog = std::max(target, m_hcyl_st.prog - m_hcyl.lerp);
    }

    re4vr::lua_set_bool("__vr_revolver_cyl_open",
                        m_hcyl_st.open || m_hcyl_st.prog > 0.15f);
}

void RE4VRReload3::hc_update_cyl_spin() {
    if (!m_hwep.wid.has_value()) {
        return;
    }

    const int32_t seq = static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

    if (!m_hspin.prev_seq.has_value()) {
        m_hspin.prev_seq = seq;
    } else if (seq > *m_hspin.prev_seq) {
        m_hspin.target += m_hcyl.shot_deg * static_cast<float>(seq - *m_hspin.prev_seq);
        m_hspin.prev_seq = seq;
    }

    const float diff = m_hspin.target - m_hspin.current;

    if (std::abs(diff) <= 0.5f) {
        m_hspin.current = m_hspin.target;
    } else {
        m_hspin.current += diff * m_hcyl.shot_lerp;
    }
}

// [KEIN COCKBACK] Die Single-Action der Handcannon ist in ALLEN Modi AUS
// (Leon-Kampagne, Ada-DLC, Mercs) -- sie feuert wie jede andere Waffe. In Lua
// steht dafuer am Anfang von update_hammer ein `if true or ...`: die ganze
// Spann-Geste ist damit toter Code, uebrig bleibt genau dieses Nullen. Der
// Feuer-Block unten prueft deshalb auch nicht mehr auf `cocked`.
void RE4VRReload3::hc_update_hammer() {
    m_hham.hand_frac = 0.0f;
    m_hham.ham_frac = 0.0f;
    m_hham.cocked = false;
    m_hham.cock_running = false;
    m_hham.phase = 0.0f;
    m_hham.key_blend = 0.0f;
}

// [EJECT] offene + gekippte Trommel -> geladene Kugeln fallen visuell raus.
void RE4VRReload3::hc_update_eject() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.has_value() && !m_hwep.bullets.empty()
          && m_hwep.spin_joint != nullptr)) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_revolver_cyl_open") != 1) {
        m_heject.active = false;
        m_heject.items.clear();
        m_heject.armed = true;

        return;
    }

    if (m_heject.active) {
        return;
    }

    glm::quat rot{};

    if (!get_quat(m_hwep.spin_joint, "get_Rotation", rot)) {
        return;
    }

    const glm::vec3 bore = rot * glm::vec3{0.0f, 0.0f, HC_EJECT_ZSIGN};

    if (bore.y < -HC_EJECT_DOWN && m_heject.armed) {
        auto* wi = hc_get_live_wi();
        const int32_t loaded = (wi != nullptr)
            ? call_enum(wi, "get_CurrentAmmoCount").value_or(0) : 0;
        const int32_t n = std::min<int32_t>(loaded,
                                            static_cast<int32_t>(m_hwep.bullets.size()));

        if (n > 0) {
            const float bl = vec_len(bore);
            m_heject.exit = (bl > 1e-6f) ? (bore / bl) : glm::vec3{0.0f, -1.0f, 0.0f};
            auto* bt = body_tf();
            glm::vec3 bp{};
            m_heject.floor_y = (bt != nullptr && get_vec3(bt, "get_Position", bp))
                ? bp.y : -9999.0f;
            m_heject.items.clear();

            for (int32_t i = 1; i <= n; ++i) {
                const auto& b = m_hwep.bullets[static_cast<size_t>(i - 1)];
                glm::vec3 p{};

                if (b.joint == nullptr || !get_vec3(b.joint, "get_Position", p)) {
                    continue;
                }

                HcEjectItem it{};
                it.joint = b.joint;
                it.vis = b.vis;
                it.sx = p.x;
                it.sy = p.y;
                it.sz = p.z;
                glm::quat r{};

                if (get_quat(b.joint, "get_Rotation", r)) {
                    it.rest_rot = r;
                }

                it.delay = static_cast<double>(m_heject.items.size()) * HC_EJECT_STAGGER;
                it.landed = false;
                it.wx = 220.0f + static_cast<float>(i) * 47.0f;
                it.wy = 130.0f + static_cast<float>(i) * 29.0f;
                it.wz = 170.0f + static_cast<float>(i) * 61.0f;
                m_heject.items.push_back(it);
            }

            if (!m_heject.items.empty()) {
                m_heject.active = true;
                m_heject.t0 = clock_now();
            }
        }

        m_heject.armed = false;
    } else if (bore.y >= -HC_EJECT_DOWN) {
        m_heject.armed = true;
    }
}

void RE4VRReload3::hc_apply_eject() {
    if (!(m_heject.active && m_heject.exit.has_value())) {
        return;
    }

    const double now = clock_now();
    const glm::vec3 ex = *m_heject.exit;

    for (auto& it : m_heject.items) {
        set_vec3(it.joint, "set_LocalScale", it.vis);
        const double lt = (now - m_heject.t0) - it.delay;

        if (lt <= 0.0) {
            set_vec3(it.joint, "set_Position", glm::vec3{it.sx, it.sy, it.sz});

            continue;
        }

        const float s = (std::min(static_cast<float>(lt), HC_EJECT_SLIDE_DUR)
                         / HC_EJECT_SLIDE_DUR) * HC_EJECT_SLIDE_DIST;
        float px = it.sx + ex.x * s;
        float py = it.sy + ex.y * s;
        const float pz = it.sz + ex.z * s;

        if (lt > HC_EJECT_SLIDE_DUR) {
            const float ft = static_cast<float>(lt) - HC_EJECT_SLIDE_DUR;
            py -= 0.5f * HC_GRAVITY * ft * ft;

            if (py <= m_heject.floor_y) {
                py = m_heject.floor_y;

                if (!it.landed) {
                    it.landed = true;
                    it.land_lt = lt;
                    hc_play_sound(HSND_DROP);
                }
            }
        }

        set_vec3(it.joint, "set_Position", glm::vec3{px, py, pz});

        if (it.rest_rot.has_value()) {
            const float tt = static_cast<float>(it.land_lt.value_or(lt));
            const glm::quat q = quat_from_euler(it.wx * tt, it.wy * tt, it.wz * tt);
            set_quat(it.joint, "set_Rotation", glm::normalize(*it.rest_rot * q));
        }
    }
}

// [WRIST-FLICK CLOSE] Die Trommel der Handcannon schwenkt seitlich nach LINKS
// (X) raus -> der Schliess-Flick ist eine horizontale Hand-Bewegung, also messen
// wir fwd.x (Yaw), nicht fwd.y wie beim Top-Break der Broken Butterfly.
void RE4VRReload3::hc_update_close_flick() {
    const double now = clock_now();

    if (m_hflick.snd_at.has_value() && now >= *m_hflick.snd_at) {
        hc_play_sound(HSND_COCK);
        m_hflick.snd_at.reset();
    }

    const bool open = m_hcyl_st.open;

    if (open && !m_hflick.was_open) {
        m_hflick.open_t = now;
    }

    m_hflick.was_open = open;

    if (!(m_hwep.wid.has_value() && open)) {
        m_hflick.prev_fy.reset();
        m_hflick.peak_dir = 0;

        return;
    }

    const auto rot = re4vr::lua_get_quat("__vr_rh_rot");

    if (!rot.has_value()) {
        m_hflick.prev_fy.reset();

        return;
    }

    const glm::vec3 fwd = *rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const float fy = fwd.x;   // [HANDCANNON] horizontaler (seitlicher) Flick

    if (m_hflick.peak_dir != 0 && (now - m_hflick.peak_t) > HC_FLICK_REVERSAL) {
        m_hflick.peak_dir = 0;
    }

    if (m_hflick.prev_fy.has_value() && m_hflick.prev_t.has_value()) {
        const double dt = now - *m_hflick.prev_t;

        if (dt > 0.001 && dt < 0.2) {
            const float vel = (fy - *m_hflick.prev_fy) / static_cast<float>(dt);

            if (std::abs(vel) > HC_FLICK_VEL) {
                const int dir = (vel > 0.0f) ? 1 : -1;

                if (m_hflick.peak_dir != 0 && dir != m_hflick.peak_dir
                    && (now - m_hflick.open_t) > HC_FLICK_OPEN_GRACE
                    && (now - m_hflick.last_close) > 0.5) {
                    m_hcyl_st.open = false;
                    m_hflick.snd_at = now + HC_FLICK_SND_DELAY;
                    m_hflick.last_close = now;
                    m_hflick.peak_dir = 0;
                } else {
                    m_hflick.peak_dir = dir;
                    m_hflick.peak_t = now;
                }
            }
        }
    }

    m_hflick.prev_fy = fy;
    m_hflick.prev_t = now;
}

void RE4VRReload3::hc_on_frame() {
    hc_refresh_weapon();

    if (!m_hwep.wid.has_value()) {
        m_hcyl_st._prev_b = false;

        // NICHT jeden Frame __vr_rev_cock_frac = 0 setzen! reload3 laeuft NACH
        // reload2 -> das wuerde den Spann-Wert der Broken Butterfly clobbern.
        // Nur EINMAL beim Ablegen raeumen.
        if (m_hc_was_managed) {
            re4vr::lua_set_number("__vr_rev_cock_frac", 0.0);
            m_hrev.cart = false;
            m_hrev.kf_active = false;
            m_hrev.kf_used = false;
            m_hdrop.active = false;
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            m_hc_was_managed = false;
        }

        return;
    }

    m_hc_was_managed = true;
    // [SHELL-KEYFRAMES] Die Keyframe-UI zeigt die Handcannon (auch ohne Clone).
    re4vr::lua_set_number("__re4_reload_ui_wid", *m_hwep.wid);
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);
    re4vr::lua_set_bool("__re4_reload_grab_empty", !hc_can_grab());

    // [SUPPORT-HAND] Patrone in der Hand -> motion Support-Hand + Two-Hand AUS.
    // [SUPPORT-COOLDOWN] Nach dem Einsetzen noch kurz oben halten, damit die
    // Support-Hand nicht SOFORT andockt.
    const bool cd_ok = m_hrev._insert_t.has_value()
        && (clock_now() - *m_hrev._insert_t) < m_hcfg.support_cooldown;
    re4vr::lua_set_bool("__vr_mag_in_hand", m_hrev.cart || cd_ok);

    hc_update_cylinder();
    hc_update_close_flick();
    hc_update_eject();
    hc_update_cyl_spin();
    hc_update_hammer();

    re4vr::lua_set_number("__vr_rev_cock_frac", m_hham.hand_frac);
    bool use_aim = false;

    if (m_hham.hand_aim.preview) {
        use_aim = true;
    } else if (m_hham.hand.preview) {
        use_aim = false;
    } else {
        // In Lua steht hier `rawget(_G, "is_aim")` -- ein Global ohne Praefix.
        use_aim = re4vr::lua_get_tribool("is_aim") == 1;
    }

    m_hham.use_aim = use_aim;
    const auto& off = use_aim ? m_hham.hand_aim : m_hham.hand;
    re4vr::lua_set_cock_off(glm::vec3{off.px, off.py, off.pz},
                            glm::vec3{off.rx, off.ry, off.rz});

    hc_update_reload();

    // [FIRE-BLOCK] offene Trommel ODER leer -> gesperrt.
    // [LIVE-EMPTY] Der Ladezustand kommt FRISCH aus der Engine
    // (getCurrentGunAmmo, kein Cache) -- der gecachte Wert war nach Save-Load
    // stale 0 -> Dry-Fire trotz Munition.
    const bool cyl_open = re4vr::lua_get_tribool("__vr_revolver_cyl_open") == 1;
    auto* wi = hc_get_live_wi();
    auto* pe = get_pe();
    int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(-1);

    if (loaded < 0) {
        loaded = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount").value_or(0) : 0;
    }

    // [UNLIMITED] Das Upgrade-/Infinite-Script meldet AmmoCount = 0, OBWOHL
    // unendlich Munition da ist -> nur dann faellt die Leer-Sperre weg.
    // [KEIN COCKBACK] `cocked` faellt als Sperrgrund weg (die Handcannon spannt
    // nicht mehr); es sperren nur noch offene Trommel und leer.
    const bool unlimited = RE4VRWeapons2::get()->is_unlimited();
    re4vr::lua_set_bool("__vr_block_fire_when_empty",
                        cyl_open || (loaded <= 0 && !unlimited));
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload3.lua:1689");

    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_hrev._dry_prev && !unlimited) {
        // [KEIN COCKBACK] frueher "nur wenn gespannt" -- sonst gaebe es beim
        // Trockenschuss keinen Trommel-Weiterdreh mehr.
        hc_play_sound(HSND_DRY_FIRE);

        if (!cyl_open) {
            m_hspin.target += m_hcyl.shot_deg;
            m_hham.cocked = false;
        }
    }

    m_hrev._dry_prev = et;
}

// ---- Apply-Pass (voller Stack, nach der Engine-Anim) ----
// _04 macht NUR den Swing. Der Chamber-Spin laeuft separat auf _05.
void RE4VRReload3::hc_apply_cylinder_pass() {
    if (!(m_hcfg.revolver_enabled && m_hwep.cyl_joint != nullptr
          && m_hwep.cyl_rest_rot.has_value())) {
        return;
    }

    const auto& r = m_hcyl;
    const bool has_pos = (r.px != 0.0f || r.py != 0.0f || r.pz != 0.0f);
    const float p = m_hcyl_st.prog;

    if (p <= 0.0001f) {
        if (has_pos && m_hwep.cyl_rest_pos.has_value()) {
            set_vec3(m_hwep.cyl_joint, "set_LocalPosition", *m_hwep.cyl_rest_pos);
        }

        return;
    }

    set_quat(m_hwep.cyl_joint, "set_LocalRotation",
             glm::normalize(*m_hwep.cyl_rest_rot
                            * quat_from_euler(r.rx * p, r.ry * p, r.rz * p)));

    if (has_pos && m_hwep.cyl_rest_pos.has_value()) {
        const glm::vec3 rp = *m_hwep.cyl_rest_pos;
        set_vec3(m_hwep.cyl_joint, "set_LocalPosition",
                 glm::vec3{rp.x + r.px * p, rp.y + r.py * p, rp.z + r.pz * p});
    }
}

std::optional<int32_t> RE4VRReload3::hc_get_gun_ammo() {
    if (auto* wi = hc_get_live_wi(); wi != nullptr) {
        if (const auto a = call_enum(wi, "get_CurrentAmmoCount"); a.has_value()) {
            return a;
        }
    }

    auto* pe = get_pe();

    return (pe != nullptr) ? call_enum(pe, "getCurrentGunAmmo") : std::nullopt;
}

void RE4VRReload3::hc_apply_bullet_visibility() {
    if (!(m_hcfg.revolver_enabled && !m_hwep.bullets.empty())) {
        return;
    }

    const int32_t cap = static_cast<int32_t>(m_hwep.bullets.size());
    const auto a = hc_get_gun_ammo();

    if (!a.has_value()) {
        return;
    }

    const int32_t ammo = std::clamp(*a, 0, cap);

    for (int32_t i = 1; i <= cap; ++i) {
        const auto& b = m_hwep.bullets[static_cast<size_t>(i - 1)];
        set_vec3(b.joint, "set_LocalScale",
                 (i <= ammo) ? b.vis : glm::vec3{0.0f, 0.0f, 0.0f});
    }
}

void RE4VRReload3::hc_apply_shell_pose() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.has_value())) {
        return;
    }

    std::string want{};

    if ((m_hrev.cart || m_hrev.preview) && !m_hshell.pose.empty()) {
        want = m_hshell.pose;
    }

    float b = 0.0f;

    if (!pose_fade_step(m_hshell_fade, want, b)) {
        return;
    }

    const auto& s = m_hshell;

    if (m_main != nullptr) {
        m_main->apply_pose(m_hshell_fade.name, b);
    }

    auto* bt = body_tf();

    if (s.t_rx != 0.0f || s.t_ry != 0.0f || s.t_rz != 0.0f) {
        auto* thumb = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (thumb != nullptr && get_quat(thumb, "get_LocalRotation", cur)) {
            set_quat(thumb, "set_LocalRotation",
                     glm::normalize(cur * quat_from_euler(s.t_rx * b, s.t_ry * b,
                                                          s.t_rz * b)));
        }
    }

    // [ZEIGEFINGER] additiv auf L_IndexF1/2/3 (eigenstaendige Greif-Pose).
    if (s.i_rx != 0.0f || s.i_ry != 0.0f || s.i_rz != 0.0f) {
        static const char* const IDX[3] = {"L_IndexF1", "L_IndexF2", "L_IndexF3"};

        for (const char* jn : IDX) {
            auto* jt = (bt != nullptr) ? joint_by_name(bt, jn) : nullptr;
            glm::quat cur{};

            if (jt != nullptr && get_quat(jt, "get_LocalRotation", cur)) {
                set_quat(jt, "set_LocalRotation",
                         glm::normalize(cur * quat_from_euler(s.i_rx * b, s.i_ry * b,
                                                              s.i_rz * b)));
            }
        }
    }
}

// ---- [MESH-CLONE] Patrone in der Hand (die Trommel bleibt unberuehrt) ----
::REManagedObject* RE4VRReload3::hc_gun_mesh() {
    if (m_hwep.tf == nullptr) {
        return nullptr;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_hwep.tf, "get_GameObject");

    return (go != nullptr) ? re4vr::get_component(go, "via.render.Mesh") : nullptr;
}

void RE4VRReload3::hc_cart_destroy() {
    if (m_hcart.obj != nullptr) {
        re4vr::destroy_game_object(m_hcart.obj);
    }

    m_hcart.obj = nullptr;
    m_hcart.mesh = nullptr;
    m_hcart.wid.reset();
    m_hcart.parts_sig.clear();
    m_hcart.parented = false;
    m_hcart.pmode.clear();
}

bool RE4VRReload3::hc_cart_spawn() {
    if (m_hcart.obj != nullptr) {
        return true;
    }

    auto* gmesh = hc_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_handcannon_cart");

    if (go == nullptr) {
        return false;
    }

    auto* gom = reinterpret_cast<::REManagedObject*>(go);

    if (auto* mt = re4vr::runtime_type("via.motion.Motion"); mt != nullptr) {
        re4vr::call_safe<::REManagedObject*>(gom, "createComponent(System.Type)", mt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* rt = re4vr::runtime_type("via.render.Mesh"); rt != nullptr) {
        mesh = re4vr::call_safe<::REManagedObject*>(gom, "createComponent(System.Type)", rt);
    }

    if (mesh == nullptr) {
        return false;
    }

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (gmat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", gmat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    m_hcart.obj = gom;
    m_hcart.mesh = mesh;
    m_hcart.wid = m_hwep.wid;

    // [NO_LAG] nativ ans L_Hand-Joint parenten.
    m_hcart.parented = false;
    auto* ctf = re4vr::call_safe<::REManagedObject*>(gom, "get_Transform");
    auto* bt2 = body_tf();

    if (ctf != nullptr && bt2 != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", bt2);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_ParentJoint", jn);
            m_hcart.parented = true;
            m_hcart.pmode = "hand";
        }
    }

    return true;
}

void RE4VRReload3::hc_cart_isolate() {
    if (m_hcart.mesh == nullptr) {
        return;
    }

    const std::string sig = "parts:" + m_hshell.parts;

    if (m_hcart.parts_sig == sig) {
        return;
    }

    const auto keep = parse_parts(m_hshell.parts);
    bool applied = true;

    for (int32_t i = 0; i <= 48; ++i) {
        if (!re4vr::obj_ok(m_hcart.mesh)) {
            applied = false;

            break;
        }

        const bool on = std::find(keep.begin(), keep.end(), i) != keep.end();
        re4vr::call_safe<void*>(m_hcart.mesh, "setPartsEnable", i, on);
    }

    if (applied) {
        m_hcart.parts_sig = sig;
    }
}

// [NO_LAG BAHN] Im Keyframe-Modus (Bahn UND Preview) hing der Clone an NICHTS
// und wurde per WELT-Pose gesetzt -> beim Laufen zieht er 3-4 Frames nach.
// Derselbe Trick wie beim Armbrust-Dummy: an die WAFFEN-Transform parenten und
// nur noch LOKAL setzen. Der Rueckweg ist das L_Hand am Body (gemerkt, nicht
// geraten).
namespace {
// [STALE-PARENT-CRASH] set_Parent/set_ParentJoint auf einer freigegebenen
// Transform = native Access Violation (c0000005), die try/catch NICHT faengt.
// Siehe [[feedback_set_parent_stale_av_crash]].
bool tf_valid3(::REManagedObject* o) {
    if (!re4vr::obj_ok(o)) {
        return false;
    }

    const auto v = re4vr::call_num(o, "get_Valid");

    return v.has_value() && *v != 0.0;
}
}   // namespace

void RE4VRReload3::hc_cart_parent_mode(const char* mode) {
    if (m_hcart.pmode == mode) {
        return;
    }

    auto* tf = (m_hcart.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform") : nullptr;

    if (!tf_valid3(tf)) {
        return;
    }

    if (std::string{mode} == "weapon") {
        if (tf_valid3(m_hwep.tf)) {
            re4vr::call_safe<void*>(tf, "set_Parent", m_hwep.tf);
            m_hcart.pmode = "weapon";
            m_hcart.parented = false;
        }

        return;
    }

    auto* bt3 = body_tf();

    if (!tf_valid3(bt3)) {
        return;
    }

    re4vr::call_safe<void*>(tf, "set_Parent", bt3);
    auto* jn = sdk::VM::create_managed_string(L"L_Hand");

    if (jn != nullptr) {
        re4vr::call_safe<void*>(tf, "set_ParentJoint", jn);
        m_hcart.pmode = "hand";
        m_hcart.parented = true;
    }
}

void RE4VRReload3::hc_cart_set_tf(::REManagedObject* tf, const glm::vec3& p,
                                  const std::optional<glm::quat>& rot, float scl) {
    sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf),
                                Vector4f{p.x, p.y, p.z, 1.0f}, true);

    if (rot.has_value()) {
        sdk::set_transform_rotation(reinterpret_cast<::RETransform*>(tf), *rot);
    }

    set_vec3(tf, "set_LocalScale", glm::vec3{scl, scl, scl});
}

void RE4VRReload3::hc_apply_held_cartridge() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.has_value())) {
        return;
    }

    const int32_t wid = *m_hwep.wid;
    // [SHELL-KEYFRAMES] Keyframe-Preview an -> Clone spawnen (zum Tunen ohne
    // Patrone in der Hand).
    const bool kfp = (m_adv != nullptr) && m_adv->shell_preview
                     && m_adv->is_keyframe_insert(wid);

    if (!(m_hrev.cart || m_hrev.preview || m_hdrop.active || kfp)) {
        if (m_hcart.obj != nullptr) {
            hc_cart_destroy();
        }

        return;
    }

    if (m_hdrop.active) {
        return;
    }

    if (m_hcart.obj != nullptr && m_hcart.wid.value_or(0) != wid) {
        hc_cart_destroy();
    }

    if (m_hcart.obj == nullptr && !hc_cart_spawn()) {
        return;
    }

    hc_cart_isolate();

    const auto& s = m_hshell;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform");

    // [SHELL-KEYFRAMES] Bahn ODER Preview -> die Clone-Position macht
    // reposition_cart_late; hier nur Scale/entkoppeln.
    if (m_hrev.kf_active || kfp) {
        if (kfp && tf != nullptr) {
            if (m_hcart.parented) {
                re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                m_hcart.parented = false;
                m_hcart.pmode.clear();
            }

            set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});
        }

        return;
    }

    if (tf == nullptr) {
        return;
    }

    // [NO_LAG] geparentet ans L_Hand: nur LOKALE Pose.
    if (m_hcart.parented) {
        set_vec3(tf, "set_LocalPosition", glm::vec3{s.x, s.y, s.z});
        set_quat(tf, "set_LocalRotation", quat_from_euler(s.rx, s.ry, s.rz));
        set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});

        return;
    }

    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return;
    }

    glm::quat hr{};
    const bool have_hr = get_quat(lhj, "get_Rotation", hr);
    glm::vec3 w = hp;
    std::optional<glm::quat> rot{};

    if (have_hr) {
        w = hp + (hr * glm::vec3{s.x, s.y, s.z});
        rot = glm::normalize(hr * quat_from_euler(s.rx, s.ry, s.rz));
    }

    hc_cart_set_tf(tf, w, rot, s.scale);
}

// [WOBBLE-FIX] Patrone NACH motions finalem L_Hand-Write nachziehen.
void RE4VRReload3::hc_reposition_cart_late() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.has_value() && m_hcart.obj != nullptr)) {
        return;
    }

    if (m_hdrop.active) {
        return;
    }

    const int32_t wid = *m_hwep.wid;
    const auto& s = m_hshell;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform");

    // [SHELL-KEYFRAMES] Bahn aktiv -> Clone entlang der Keyframes fahren
    // (relativ zur Waffe), am Bahn-Ende entfernen (+1 sass beim Bahn-Start).
    if (m_hrev.kf_active) {
        if (m_adv != nullptr && tf != nullptr && m_hwep.tf != nullptr) {
            const float dur = std::max(m_adv->shell_dur, 0.01f);
            float tt = static_cast<float>(clock_now() - m_hrev.kf_t0) / dur;

            if (tt > 1.0f) {
                tt = 1.0f;
            }

            // [KF_LEAD] Eingangspunkt: die ersten Frames fest auf Keyframe #1.
            if ((clock_now() - m_hrev.kf_t0)
                < re4vr::lua_get_number("__re4_kf_lead_dur", 0.035)) {
                tt = 0.0f;
            }

            RE4VRReloadAdv::Key k{};

            if (m_adv->shell_pose_at(wid, tt, k)) {
                // [NO_LAG BAHN] An der Waffe haengend -> die lokale
                // Keyframe-Pose direkt setzen (identische Mathematik wie der
                // alte Welt-Weg, nur ohne Nachziehen).
                hc_cart_parent_mode("weapon");

                if (m_hcart.pmode == "weapon") {
                    set_vec3(tf, "set_LocalPosition", glm::vec3{k.x, k.y, k.z});
                    set_quat(tf, "set_LocalRotation", quat_from_euler(k.rx, k.ry, k.rz));
                    set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});
                } else {
                    glm::vec3 gp{};
                    glm::quat gr{};

                    if (get_vec3(m_hwep.tf, "get_Position", gp)
                        && get_quat(m_hwep.tf, "get_Rotation", gr)) {
                        hc_cart_set_tf(tf, gp + (gr * glm::vec3{k.x, k.y, k.z}),
                                       glm::normalize(gr * quat_from_euler(k.rx, k.ry, k.rz)),
                                       s.scale);
                    }
                }
            }

            if (tt >= 1.0f) {
                // [KF_HOLD] Nicht im selben Frame abschalten: sonst blitzt der
                // Clone einmal an seiner alten Lage auf.
                if (!m_hrev.kf_hold_t.has_value()) {
                    m_hrev.kf_hold_t = clock_now()
                        + re4vr::lua_get_number("__re4_kf_hold_dur", 0.035);
                }

                if (clock_now() >= *m_hrev.kf_hold_t) {
                    m_hrev.kf_hold_t.reset();
                    m_hrev.kf_active = false;
                    m_hrev.cart = false;
                }
            }
        } else {
            m_hrev.kf_active = false;
        }

        return;
    }

    // [SHELL-KEYFRAMES] Keyframe-Preview: Clone an der Waffe + Tuning-Lage.
    if (m_adv != nullptr && m_adv->shell_preview && m_adv->is_keyframe_insert(wid)) {
        const auto& sl = m_adv->shell_live;

        if (tf != nullptr) {
            // [NO_LAG BAHN] auch beim Einstellen kein Nachziehen
            hc_cart_parent_mode("weapon");

            if (m_hcart.pmode == "weapon") {
                set_vec3(tf, "set_LocalPosition", glm::vec3{sl.x, sl.y, sl.z});
                set_quat(tf, "set_LocalRotation", quat_from_euler(sl.rx, sl.ry, sl.rz));
                set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});
            } else {
                glm::vec3 gp{};
                glm::quat gr{};

                if (m_hwep.tf != nullptr && get_vec3(m_hwep.tf, "get_Position", gp)
                    && get_quat(m_hwep.tf, "get_Rotation", gr)) {
                    hc_cart_set_tf(tf, gp + (gr * glm::vec3{sl.x, sl.y, sl.z}),
                                   glm::normalize(gr * quat_from_euler(sl.rx, sl.ry, sl.rz)),
                                   s.scale);
                }
            }
        }

        return;
    }

    if (!(m_hrev.cart || m_hrev.preview)) {
        return;
    }

    // [NO_LAG BAHN] aus dem Waffen-Parent zurueck an die Hand
    hc_cart_parent_mode("hand");

    if (tf == nullptr) {
        return;
    }

    if (m_hcart.parented) {
        set_vec3(tf, "set_LocalPosition", glm::vec3{s.x, s.y, s.z});
        set_quat(tf, "set_LocalRotation", quat_from_euler(s.rx, s.ry, s.rz));
        set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});

        return;
    }

    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return;
    }

    glm::quat hr{};
    const bool have_hr = get_quat(lhj, "get_Rotation", hr);
    glm::vec3 w = hp;
    std::optional<glm::quat> rot{};

    if (have_hr) {
        w = hp + (hr * glm::vec3{s.x, s.y, s.z});
        rot = glm::normalize(hr * quat_from_euler(s.rx, s.ry, s.rz));
    }

    hc_cart_set_tf(tf, w, rot, s.scale);
}

void RE4VRReload3::hc_apply_cart_drop() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.has_value() && m_hdrop.active)) {
        return;
    }

    if (m_hcart.obj == nullptr) {
        m_hdrop.active = false;

        return;
    }

    const float t = static_cast<float>(clock_now() - m_hdrop.t0);

    if (t > HC_DROP_DUR) {
        m_hdrop.active = false;
        hc_cart_destroy();

        return;
    }

    if (!m_hdrop.snd && t >= 0.45f) {
        m_hdrop.snd = true;
        hc_play_sound(HSND_DROP);
    }

    const float fall = 0.5f * HC_GRAVITY * t * t;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_hcart.obj, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    hc_cart_set_tf(tf, glm::vec3{m_hdrop.sx, m_hdrop.sy - fall, m_hdrop.sz},
                   std::nullopt, m_hshell.scale);
}

// _05 Eigenrotation: haelt die Ruhe + den Chamber-Advance pro Schuss.
void RE4VRReload3::hc_apply_cylinder_spin_lock() {
    if (!(m_hcfg.revolver_enabled && m_hwep.spin_joint != nullptr
          && m_hwep.spin_rest_rot.has_value())) {
        return;
    }

    glm::quat rot = *m_hwep.spin_rest_rot;

    if (m_hspin.current != 0.0f) {
        rot = glm::normalize(*m_hwep.spin_rest_rot
                             * quat_from_euler(0.0f, 0.0f, m_hspin.current));
    }

    set_quat(m_hwep.spin_joint, "set_LocalRotation", rot);
}

void RE4VRReload3::hc_apply_hammer_pass() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.value_or(0) == HC
          && m_hwep.hammer_joint != nullptr && m_hwep.hammer_rest_rot.has_value())) {
        return;
    }

    const auto& h = m_hham_cfg;
    const float f = m_hham.ham_frac;
    const float ax = h.idle_rx + (h.rx - h.idle_rx) * f;
    const float ay = h.idle_ry + (h.ry - h.idle_ry) * f;
    const float az = h.idle_rz + (h.rz - h.idle_rz) * f;
    set_quat(m_hwep.hammer_joint, "set_LocalRotation",
             glm::normalize(*m_hwep.hammer_rest_rot * quat_from_euler(ax, ay, az)));
}

void RE4VRReload3::hc_apply_thumb_pass() {
    if (!(m_hcfg.revolver_enabled && m_hwep.wid.value_or(0) == HC)) {
        return;
    }

    auto* bt = body_tf();

    if (bt == nullptr) {
        return;
    }

    static const char* const TJ[3] = {"R_Thumb1", "R_Thumb2", "R_Thumb3"};

    // [DAUMEN-KEYS] Sind Stuetzpunkte gesetzt, kommen Winkel UND Versatz je Glied
    // aus der Kurve ueber die Phase. Geschrieben wird ABSOLUT gegen die
    // BIND-Pose, sonst bleibt die Spiel-Anim der Chef.
    const float kb = m_hham.key_blend;
    const std::vector<HcThumbKey>* KL = &hc_thumb_keys(m_hham.use_aim);

    if (KL->empty()) {
        KL = &hc_thumb_keys(!m_hham.use_aim);
    }

    const bool klive = m_hham.kprev && (m_hham.klive || KL->empty());

    if (kb > 0.0001f && (klive || !KL->empty())) {
        auto& out = m_hham.kout;

        if (klive) {
            out = m_hham.kedit;
        } else {
            hc_thumb_key_sample(*KL, m_hham.phase * 100.0f, out);
        }

        for (size_t i = 0; i < 3; ++i) {
            auto* jt = joint_by_name(bt, TJ[i]);
            glm::quat cur{};

            if (jt == nullptr || !get_quat(jt, "get_LocalRotation", cur)) {
                continue;
            }

            const auto& o = out[i];
            glm::quat rest{};

            if (!get_quat(jt, "get_BaseLocalRotation", rest)) {
                rest = cur;
            }

            const glm::quat tgt = glm::normalize(rest * quat_from_euler(o.x, o.y, o.z));
            // qslerp (nicht slerp): der kuerzeste Bogen, sonst nimmt der Daumen
            // bei grossen Key-Winkeln den Umweg ueber 360 Grad.
            const glm::quat nr = (kb < 0.999f) ? qslerp(cur, tgt, kb) : tgt;
            set_quat(jt, "set_LocalRotation", nr);

            glm::vec3 restp{};
            glm::vec3 lp{};

            if (get_vec3(jt, "get_BaseLocalPosition", restp)
                && get_vec3(jt, "get_LocalPosition", lp)) {
                const glm::vec3 t{restp.x + o.px, restp.y + o.py, restp.z + o.pz};
                set_vec3(jt, "set_LocalPosition", lp + (t - lp) * kb);
            }
        }

        return;
    }

    const float f = m_hham.hand_frac;

    if (f <= 0.0001f) {
        return;
    }

    const auto& cfg = m_hham.use_aim ? m_htcfg_aim : m_htcfg;
    static const glm::quat IDENT_Q{1.0f, 0.0f, 0.0f, 0.0f};

    for (size_t i = 0; i < 3; ++i) {
        auto* jt = joint_by_name(bt, TJ[i]);
        glm::quat cur{};

        if (jt == nullptr || !get_quat(jt, "get_LocalRotation", cur)) {
            continue;
        }

        const auto& c = cfg[i];

        if (c.cx != 0.0f || c.cy != 0.0f || c.cz != 0.0f) {
            // Ziel-Rotation einmal aus Euler bauen, dann per f slerpen
            // (kuerzester Bogen, kein Kreis).
            const glm::quat q = qslerp(IDENT_Q, quat_from_euler(c.cx, c.cy, c.cz), f);
            set_quat(jt, "set_LocalRotation", glm::normalize(cur * q));
        }

        if (i == 0) {
            const glm::vec3 tp = m_hham.use_aim ? m_hham.thumb_pos_aim : m_hham.thumb_pos;

            if (tp.x != 0.0f || tp.y != 0.0f || tp.z != 0.0f) {
                glm::vec3 lp{};

                if (get_vec3(jt, "get_LocalPosition", lp)) {
                    set_vec3(jt, "set_LocalPosition", lp + tp * f);
                }
            }
        }
    }
}

void RE4VRReload3::hc_apply_pass() {
    hc_apply_cylinder_pass();
    hc_apply_cylinder_spin_lock();
    hc_apply_hammer_pass();
    hc_apply_thumb_pass();
    hc_apply_bullet_visibility();
    hc_apply_held_cartridge();
    hc_apply_cart_drop();
    hc_apply_eject();
    hc_apply_shell_pose();
}

void RE4VRReload3::hc_on_script_reset() {
    m_hrev.cart = false;
    m_hdrop.active = false;
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
    hc_cart_destroy();

    if (m_hwep.hammer_joint != nullptr && m_hwep.hammer_rest_rot.has_value()) {
        set_quat(m_hwep.hammer_joint, "set_LocalRotation", *m_hwep.hammer_rest_rot);
    }

    m_hham.hand_frac = 0.0f;
    m_hham.ham_frac = 0.0f;
    m_hham.cocked = false;
    m_hham.prev_seq.reset();
    re4vr::lua_set_number("__vr_rev_cock_frac", 0.0);
    m_heject.active = false;
    m_heject.items.clear();
    m_heject.armed = true;
    m_hspin.target = 0.0f;
    m_hspin.current = 0.0f;
    m_hspin.prev_seq.reset();
}

// ---------------------------------------------------------------------
// UI -- Handcannon. Haengt UNTER dem Reload3-Header (gestapelt, wie reload2).
// ---------------------------------------------------------------------
void RE4VRReload3::hc_ui() {
    if (!ImGui::TreeNode("Handcannon Revolver (wp4502)")) {
        return;
    }

    if (ImGui::Checkbox("##hc_en", &m_hcfg.revolver_enabled)) {
        hc_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Handcannon Revolver Reload");

    const auto awid = m_hwep.wid.has_value() ? m_hwep.wid : get_equip_wid();
    char lbl[16]{};

    if (awid.has_value()) {
        std::snprintf(lbl, sizeof(lbl), "wp%04d", *awid);
    } else {
        std::snprintf(lbl, sizeof(lbl), "-");
    }

    ImGui::Text("Equippt: %s", lbl);
    const bool known = awid.has_value() && is_handcannon(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Handcannon erkannt - verwaltet)"
                             : "  (keine Handcannon equippt)");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Joints: Trommel=%s  Patrone=%s  cyl_joint=%s",
                       known ? HC_J_CYL : "nil", known ? HC_J_BULLET : "nil",
                       m_hwep.cyl_joint != nullptr ? "ok" : "nil");

    if (ImGui::SliderFloat("Einlege-Distanz m (Zylinder)##hcdist",
                           &m_hcfg.insert_distance, 0.03f, 0.50f)) {
        hc_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen##hcammo", &m_hcfg.reload_ammo)) {
        hc_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an##hcsnd", &m_hcfg.sound_enabled)) {
        hc_save_cfg();
    }

    if (ImGui::TreeNode("Trommel ausschwenken (RIGHT-B)##hccyl")) {
        auto& r = m_hcyl;
        bool pc = false;
        ImGui::Checkbox("Vorschau: Trommel per Regler##hccylprev", &m_hcyl_st.preview);

        if (m_hcyl_st.preview) {
            ImGui::SliderFloat("Vorschau 0..1##hccylprog", &m_hcyl_st.prog, 0.0f, 1.0f);
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Rotation (Crane-Dreh / Top-Break):");
        pc |= ImGui::SliderFloat("Auf RotX (Grad)##hccylrx", &r.rx, -180.0f, 180.0f);
        pc |= ImGui::SliderFloat("Auf RotY (Grad)##hccylry", &r.ry, -180.0f, 180.0f);
        pc |= ImGui::SliderFloat("Auf RotZ (Grad)##hccylrz", &r.rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Position (Trommel faehrt translatorisch raus):");
        pc |= ImGui::SliderFloat("Auf PosX (m)##hccylpx", &r.px, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Auf PosY (m)##hccylpy", &r.py, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Auf PosZ (m)##hccylpz", &r.pz, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Ausschwenk-Geschwindigkeit (lerp)##hccyllerp",
                                 &r.lerp, 0.01f, 0.30f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Chamber-Advance (Trommel-Dreh pro Schuss):");
        pc |= ImGui::SliderFloat("Grad pro Schuss##hccylshotdeg", &r.shot_deg, -90.0f, 90.0f);
        pc |= ImGui::SliderFloat("Dreh-Geschw. (lerp)##hccylshotlerp", &r.shot_lerp, 0.02f, 1.0f);

        // Der Spin laeuft auf _05 um das lokale Z. Test-Buttons zum Pruefen ohne
        // zu schiessen.
        if (ImGui::Button("Spin-Test (+1 Schuss)##hccylspintest")) {
            m_hspin.target += r.shot_deg;
        }

        ImGui::SameLine();

        if (ImGui::Button("Spin nullen##hccylspinzero")) {
            m_hspin.target = 0.0f;
            m_hspin.current = 0.0f;
        }

        if (ImGui::Button("Ruhe-Rotation neu erfassen##hccylrest")
            && m_hwep.cyl_joint != nullptr) {
            glm::quat q{};

            if (get_quat(m_hwep.cyl_joint, "get_LocalRotation", q)) {
                m_hwep.cyl_rest_rot = q;
            }
        }

        if (pc) {
            hc_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Spannen-Pose rechte Hand (Handcannon)##hcspann")) {
        auto& hd = m_hham.hand;
        auto& ha = m_hham.hand_aim;
        auto& hh = m_hham_cfg;
        bool ch = false;

        if (ImGui::Checkbox("Vorschau AN = NO-AIM Hand-Pose (zum Tunen)##hcspannprev",
                            &hd.preview) && hd.preview) {
            ha.preview = false;
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Spann-Geste Timing (einmalige Bewegung):");

        if (ImGui::SliderFloat("Daumen HOCH (s)##hccockpress", &m_hcfg.cock_press, 0.04f, 0.60f)) {
            hc_save_cfg();
        }

        if (ImGui::SliderFloat("Daumen KLEBT oben (s)##hccockhold", &m_hcfg.cock_hold, 0.00f, 0.60f)) {
            hc_save_cfg();
        }

        if (ImGui::SliderFloat("Daumen ZURUECK (s)##hccockret", &m_hcfg.cock_return, 0.04f, 0.60f)) {
            hc_save_cfg();
        }

        if (ImGui::SliderFloat("Hahn-Fall Tempo (lerp)##hccockfall", &m_hcfg.cock_fall, 0.05f, 1.0f)) {
            hc_save_cfg();
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hahn _02 ENTSPANNT (vorne, idle) -- Grad:");
        ch |= ImGui::DragFloat("Idle Hahn X##hchidx", &hh.idle_rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Idle Hahn Y##hchidy", &hh.idle_ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Idle Hahn Z##hchidz", &hh.idle_rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hahn _02 GESPANNT (hinten, cocked) -- Grad:");
        ch |= ImGui::DragFloat("Cocked Hahn X##hchcx", &hh.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Cocked Hahn Y##hchcy", &hh.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Cocked Hahn Z##hchcz", &hh.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "--- NO-AIM Hand-Lage -- Handgelenk-Roll (Grad):");
        ch |= ImGui::DragFloat("Roll X##hcckrx", &hd.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Roll Y##hcckry", &hd.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Roll Z##hcckrz", &hd.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "NO-AIM Hand-Lage -- Versatz (m, hand-lokal):");
        ch |= ImGui::DragFloat("Pos X##hcckpx", &hd.px, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat("Pos Y##hcckpy", &hd.py, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat("Pos Z##hcckpz", &hd.pz, 0.002f, -0.30f, 0.30f, "%.4f");

        ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                           "=== AIM-Zustand (Waffe gezielt) -- eigene Hand-Pose ===");

        if (ImGui::Checkbox("Vorschau AN = Aim-Pose zeigen (zum Tunen)##hcaimprev",
                            &ha.preview) && ha.preview) {
            hd.preview = false;
        }

        if (ImGui::Button("Aim-Werte aus No-Aim kopieren (Hand + Daumen)##hcaimcopy")) {
            ha.rx = hd.rx;
            ha.ry = hd.ry;
            ha.rz = hd.rz;
            ha.px = hd.px;
            ha.py = hd.py;
            ha.pz = hd.pz;
            m_htcfg_aim = m_htcfg;
            m_hham.thumb_pos_aim = m_hham.thumb_pos;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "AIM Hand-Lage -- Handgelenk-Roll (Grad):");
        ch |= ImGui::DragFloat("Aim Roll X##hcackrx", &ha.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Aim Roll Y##hcackry", &ha.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Aim Roll Z##hcackrz", &ha.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "AIM Hand-Lage -- Versatz (m, hand-lokal):");
        ch |= ImGui::DragFloat("Aim Pos X##hcackpx", &ha.px, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat("Aim Pos Y##hcackpy", &ha.py, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat("Aim Pos Z##hcackpz", &ha.pz, 0.002f, -0.30f, 0.30f, "%.4f");

        const bool edit_aim = ha.preview;
        auto& tt_e = edit_aim ? m_htcfg_aim : m_htcfg;
        auto& tp = edit_aim ? m_hham.thumb_pos_aim : m_hham.thumb_pos;
        ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                           edit_aim ? "Daumen am Spannhahn -- bearbeite: AIM-Satz "
                                      "(additiv, Grad):"
                                    : "Daumen am Spannhahn -- bearbeite: NO-AIM-Satz "
                                      "(additiv, Grad):");

        for (int i = 1; i <= 3; ++i) {
            auto& j = tt_e[static_cast<size_t>(i - 1)];
            char ix[32]{}, iy[32]{}, iz[32]{};
            std::snprintf(ix, sizeof(ix), "Daumen%d X##hctcx%d", i, i);
            std::snprintf(iy, sizeof(iy), "Daumen%d Y##hctcy%d", i, i);
            std::snprintf(iz, sizeof(iz), "Daumen%d Z##hctcz%d", i, i);
            ch |= ImGui::DragFloat(ix, &j.cx, 0.5f, -180.0f, 180.0f, "%.1f");
            ch |= ImGui::DragFloat(iy, &j.cy, 0.5f, -180.0f, 180.0f, "%.1f");
            ch |= ImGui::DragFloat(iz, &j.cz, 0.5f, -180.0f, 180.0f, "%.1f");
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen-Versatz (m) -- schiebt den ganzen Daumen:");
        ch |= ImGui::DragFloat("Daumen zurueck Z##hctpz", &tp.z, 0.001f, -0.10f, 0.10f, "%.4f");
        ch |= ImGui::DragFloat("Daumen Versatz X##hctpx", &tp.x, 0.001f, -0.10f, 0.10f, "%.4f");
        ch |= ImGui::DragFloat("Daumen Versatz Y##hctpy", &tp.y, 0.001f, -0.10f, 0.10f, "%.4f");

        if (ch) {
            hc_save_cfg();
        }

        ImGui::TreePop();
    }

    // [DAUMEN-KEYS] Wie beim Broken Butterfly: Phase anfahren -> die drei Glieder
    // stellen -> "Key setzen". Eigene Listen, eigenes JSON.
    if (ImGui::TreeNode("Daumen-Keyframes Spannen (Handcannon)##hctkeys")) {
        const bool aim_e = m_hham.hand_aim.preview;
        auto& KL = hc_thumb_keys(aim_e);
        ImGui::Checkbox("VORSCHAU: Phase per Regler (Daumen + Hahn stehen still)##hctkprev",
                        &m_hham.kprev);
        ImGui::Checkbox("   dabei die REGLER-Werte zeigen (aus = fertige Kurve pruefen)##hctklive",
                        &m_hham.klive);

        if (!m_hham.kprev) {
            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                               "   >> Ohne diese Vorschau bewegt sich der Daumen NUR "
                               "waehrend der echten Spann-Geste.");
        }

        ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                           aim_e ? "   bearbeitet: AIM-Satz (weil die Aim-Vorschau oben an ist)"
                                 : "   bearbeitet: NO-AIM-Satz");
        ImGui::SliderFloat("Phase % (0 = Griff, 100 = wieder am Griff)##hctkph",
                           &m_hham.kphase, 0.0f, 100.0f, "%.0f");
        ImGui::Text("   Keys hier: %d   (NO-AIM: %d / AIM: %d)   Phase live: %.0f %%   "
                    "Hahn: %.0f %%   Kurve aktiv: %.0f %%",
                    static_cast<int>(KL.size()),
                    static_cast<int>(m_htkeys.size()),
                    static_cast<int>(m_htkeys_aim.size()),
                    m_hham.phase * 100.0f, m_hham.ham_frac * 100.0f,
                    m_hham.key_blend * 100.0f);

        for (int i = 1; i <= 3; ++i) {
            char b[48]{};
            std::snprintf(b, sizeof(b), "R_Thumb%d%s##hctkj%d", i,
                          (m_hham.kjoint == i) ? "  <<" : "", i);

            if (ImGui::Button(b)) {
                m_hham.kjoint = i;
            }

            if (i < 3) {
                ImGui::SameLine();
            }
        }

        auto& ke = m_hham.kedit[static_cast<size_t>(std::clamp(m_hham.kjoint, 1, 3) - 1)];
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "R_Thumb%d -- Beugen (Grad, absolut ab Bind-Pose): "
                           "Thumb1 um X, Thumb2/3 um Y", m_hham.kjoint);
        ImGui::DragFloat("Winkel X##hctkx", &ke.x, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("Winkel Y##hctky", &ke.y, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("Winkel Z##hctkz", &ke.z, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Versatz (m, NUR auf dieses Glied) -- damit laesst sich strecken:");
        ImGui::DragFloat("Versatz X##hctkpx", &ke.px, 0.001f, -0.10f, 0.10f, "%.4f");
        ImGui::DragFloat("Versatz Y##hctkpy", &ke.py, 0.001f, -0.10f, 0.10f, "%.4f");
        ImGui::DragFloat("Versatz Z##hctkpz", &ke.pz, 0.001f, -0.10f, 0.10f, "%.4f");

        if (ImGui::Button("Key hier setzen##hctkset")) {
            const float p2 = std::clamp(m_hham.kphase, 0.0f, 100.0f);
            HcThumbKey* hit = nullptr;

            for (auto& k : KL) {
                if (std::abs(k.p - p2) < 3.0f) {
                    hit = &k;

                    break;
                }
            }

            if (hit == nullptr) {
                KL.push_back(HcThumbKey{});
                hit = &KL.back();
            }

            hit->p = p2;
            hit->j = m_hham.kedit;
            std::sort(KL.begin(), KL.end(),
                      [](const HcThumbKey& m, const HcThumbKey& q) { return m.p < q.p; });
            hc_save_cfg();
        }

        ImGui::SameLine();

        if (ImGui::Button("Alle Keys loeschen##hctkclr")) {
            KL.clear();
            hc_save_cfg();
        }

        ImGui::SameLine();

        if (ImGui::Button("Regler auf 0##hctkzero")) {
            m_hham.kedit = std::array<HcThumbKeyJ, 3>{};
        }

        if (ImGui::Button("Aim-Keys aus No-Aim kopieren##hctkcopy")) {
            m_htkeys_aim = m_htkeys;
            hc_save_cfg();
        }

        for (int i = static_cast<int>(KL.size()); i >= 1; --i) {
            const auto& k = KL[static_cast<size_t>(i - 1)];
            const auto& j1 = k.j[0];
            const auto& j2 = k.j[1];
            const auto& j3 = k.j[2];
            // [KEIN PROZENTZEICHEN] Der Lua-Grund (text_colored ist ein
            // FORMAT-String) gilt hier genauso -- darum "Pct" statt "%".
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f},
                               "   %2d.  %3.0f Pct   T1 %+0.0f/%+0.0f/%+0.0f   "
                               "T2 %+0.0f/%+0.0f/%+0.0f   T3 %+0.0f/%+0.0f/%+0.0f",
                               i, k.p, j1.x, j1.y, j1.z, j2.x, j2.y, j2.z,
                               j3.x, j3.y, j3.z);
            ImGui::SameLine();
            char b1[24]{};
            std::snprintf(b1, sizeof(b1), "x##hctkdel%d", i);

            if (ImGui::Button(b1)) {
                KL.erase(KL.begin() + (i - 1));
                hc_save_cfg();

                break;
            }

            ImGui::SameLine();
            char b2[24]{};
            std::snprintf(b2, sizeof(b2), "laden##hctkld%d", i);

            if (ImGui::Button(b2)) {
                m_hham.kphase = k.p;
                m_hham.kedit = k.j;
            }
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "   Vor dem ersten und nach dem letzten Key blendet der "
                           "Daumen selbst gegen den Griff.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Shell in Hand (Pose + Daumen + Offset)##hcshell")) {
        auto& s = m_hshell;
        bool sch = false;
        ImGui::Checkbox("Vorschau: Patrone in der Hand (zum Tunen)##hcshellprev",
                        &m_hrev.preview);
        char pb[128]{};
        std::snprintf(pb, sizeof(pb), "%s", s.pose.c_str());

        if (ImGui::InputText("Pose-Name (aus reload.json POSES)##hcshellpose",
                             pb, sizeof(pb))) {
            s.pose = pb;
            sch = true;
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen-Spreizung (additiv, Grad):");
        sch |= ImGui::SliderFloat("Daumen RotX##hcshtrx", &s.t_rx, -90.0f, 90.0f);
        sch |= ImGui::SliderFloat("Daumen RotY##hcshtry", &s.t_ry, -90.0f, 90.0f);
        sch |= ImGui::SliderFloat("Daumen RotZ##hcshtrz", &s.t_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Zeigefinger (additiv auf L_IndexF1/2/3, Grad; "
                           "negativ = strecken/zurueck):");
        sch |= ImGui::SliderFloat("Zeigefinger RotX##hcshirx", &s.i_rx, -180.0f, 180.0f);
        sch |= ImGui::SliderFloat("Zeigefinger RotY##hcshiry", &s.i_ry, -180.0f, 180.0f);
        sch |= ImGui::SliderFloat("Zeigefinger RotZ##hcshirz", &s.i_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Clone-Offset zur Hand (m):");
        sch |= ImGui::DragFloat("PosX##hcshx", &s.x, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("PosY##hcshy", &s.y, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("PosZ##hcshz", &s.z, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("RotX##hcshrx", &s.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        sch |= ImGui::DragFloat("RotY##hcshry", &s.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        sch |= ImGui::DragFloat("RotZ##hcshrz", &s.rz, 0.5f, -180.0f, 180.0f, "%.1f");

        char qb[64]{};
        std::snprintf(qb, sizeof(qb), "%s", s.parts.c_str());

        if (ImGui::InputText("Mesh-Part-Indizes (z.B. 20,30)##hcshparts", qb, sizeof(qb))) {
            s.parts = qb;
            sch = true;
        }

        sch |= ImGui::SliderFloat("Scale##hcshscale", &s.scale, 0.1f, 3.0f);

        if (sch) {
            hc_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

// ============================================================================
// 3 -- ROCKET LAUNCHER (wp4900/4901/4902, Lua Z.2683-3205)
//
// Reload denkbar einfach: Warhead = joint _07. Schuss -> die Engine entfernt den
// Warhead (Ammo 0). Griff ins Mag-Holster -> wenn Reserve > 0: Warhead "in der
// Hand" (Pose aus reload.json POSES) -> Hand nah an die Waffe (Dock-Naehe) ->
// +1 Ammo (die Engine zeigt den Warhead wieder). Kein Mag-Drop, kein Slide.
//
// Der Warhead in der Hand ist ein MESH-KLON von Part 03: das Umhaengen des
// Joints _07 ging NICHT (der Warhead ist part-basiert -> bei Ammo 0 weg -> es
// waere nichts zu sehen).
// Alle drei Varianten teilen sich EIN Setup. Eigenes JSON: ..._reload3_rl.json
// ============================================================================
namespace {

constexpr int32_t RL_KEY = 4900;   // "primaerer" Key fuer geteilte Config/Pose
constexpr const char* RL_CFG_PATH = "re4_vr/re4_vr_reload3_rl.json";
constexpr const char* RL_J_WARHEAD = "_07";

constexpr uint32_t RLSND_GRAB = 1839787494u;     // Griff ins Holster
constexpr uint32_t RLSND_INSERT = 942865223u;    // Warhead eingesetzt

}   // namespace

// 4900 Rocket Launcher, 4901 RL (Special), 4902 Infinite RL
bool RE4VRReload3::is_rl(int32_t wid) {
    return wid == 4900 || wid == 4901 || wid == 4902;
}

void RE4VRReload3::rl_load_cfg() {
    const auto data = re4vr::json_load(RL_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    const nlohmann::json& c = (data.contains("cfg") && data["cfg"].is_object())
                              ? data["cfg"] : data;
    auto& g = m_rlcfg;
    g.rl_enabled = jbool(c, "rl_enabled", g.rl_enabled);
    g.reload_ammo = jbool(c, "reload_ammo", g.reload_ammo);
    g.sound_enabled = jbool(c, "sound_enabled", g.sound_enabled);
    g.insert_distance = jnum(c, "insert_distance", g.insert_distance);
    g.dock_joint = jstr(c, "dock_joint", g.dock_joint);
    g.parts = jstr(c, "parts", g.parts);
    g.dock_x = jnum(c, "dock_x", g.dock_x);
    g.dock_y = jnum(c, "dock_y", g.dock_y);
    g.dock_z = jnum(c, "dock_z", g.dock_z);
    g.wx = jnum(c, "wx", g.wx);
    g.wy = jnum(c, "wy", g.wy);
    g.wz = jnum(c, "wz", g.wz);
    g.wrx = jnum(c, "wrx", g.wrx);
    g.wry = jnum(c, "wry", g.wry);
    g.wrz = jnum(c, "wrz", g.wrz);
    g.wscale = jnum(c, "wscale", g.wscale);

    if (const auto it = data.find("pose"); it != data.end() && it->is_object()) {
        if (const auto v = it->find("4900"); v != it->end() && v->is_object()) {
            auto& p = m_rlpose;
            p.pose = jstr(*v, "pose", p.pose);
            p.t_rx = jnum(*v, "t_rx", p.t_rx);
            p.t_ry = jnum(*v, "t_ry", p.t_ry);
            p.t_rz = jnum(*v, "t_rz", p.t_rz);
            p.i_rx = jnum(*v, "i_rx", p.i_rx);
            p.i_ry = jnum(*v, "i_ry", p.i_ry);
            p.i_rz = jnum(*v, "i_rz", p.i_rz);
        }
    }
}

void RE4VRReload3::rl_save_cfg() {
    const auto& g = m_rlcfg;
    const auto& p = m_rlpose;

    nlohmann::json d = {
        {"cfg", {{"rl_enabled", g.rl_enabled}, {"insert_distance", g.insert_distance},
                 {"reload_ammo", g.reload_ammo}, {"sound_enabled", g.sound_enabled},
                 {"dock_joint", g.dock_joint}, {"dock_x", g.dock_x},
                 {"dock_y", g.dock_y}, {"dock_z", g.dock_z},
                 {"parts", g.parts}, {"wx", g.wx}, {"wy", g.wy}, {"wz", g.wz},
                 {"wrx", g.wrx}, {"wry", g.wry}, {"wrz", g.wrz},
                 {"wscale", g.wscale}}},
        {"pose", {{"4900", {{"pose", p.pose},
                            {"t_rx", p.t_rx}, {"t_ry", p.t_ry}, {"t_rz", p.t_rz},
                            {"i_rx", p.i_rx}, {"i_ry", p.i_ry}, {"i_rz", p.i_rz}}}}},
    };

    re4vr::json_save(RL_CFG_PATH, d);
}

void RE4VRReload3::rl_play_sound(uint32_t id) {
    if (!m_rlcfg.sound_enabled || id == 0 || m_rlwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_rlwep.tf, "get_GameObject"), id);
}

// [ACCESSOR] zuerst die ECHTE, persistente Instanz. Alles darunter sind KOPIEN.
::REManagedObject* RE4VRReload3::rl_get_wi() {
    if (m_main != nullptr) {
        if (auto* rw = m_main->real_wi(); rw != nullptr) {
            return rw;
        }
    }

    auto* ewi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

bool RE4VRReload3::rl_gun_is_full(::REManagedObject* wi) {
    bool bf = false;

    if (re4vr::try_call<bool>(wi, "get_IsBulletFull", bf)) {
        return bf;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    const int32_t maxc = call_enum(wi, "get_CurrentAmmoMax").value_or(0);

    return maxc > 0 && loaded >= maxc;
}

int32_t RE4VRReload3::rl_reserve() {
    auto* wi = rl_get_wi();

    if (wi == nullptr || m_main == nullptr) {
        return 0;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(get_pe(), "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    return (inv != nullptr && ammo_id.has_value())
        ? m_main->item_count_sum(inv, *ammo_id) : 0;
}

bool RE4VRReload3::rl_can_grab() {
    auto* wi = rl_get_wi();

    if (wi == nullptr) {
        return false;
    }

    // HART: nur 1 Warhead im Launcher -> bei loaded >= 1 ist das Holster gesperrt.
    if (call_enum(wi, "get_CurrentAmmoCount").value_or(0) >= 1) {
        return false;
    }

    if (rl_gun_is_full(wi)) {
        return false;
    }

    return rl_reserve() > 0;
}

// +1 verlustsicher: write_dword 0x44, Fallback addAmmoCount; die Reserve nur
// ziehen, wenn der Pfad sie nicht selbst gezogen hat.
bool RE4VRReload3::rl_load_one() {
    if (!m_rlcfg.reload_ammo || m_main == nullptr) {
        return false;
    }

    auto* wi = rl_get_wi();

    if (wi == nullptr || rl_gun_is_full(wi)) {
        return false;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    const auto read_reserve = [&]() -> int32_t {
        return (inv != nullptr && ammo_id.has_value())
            ? m_main->item_count_sum(inv, *ammo_id) : 0;
    };

    const int32_t r_b4 = read_reserve();

    if (r_b4 <= 0) {
        return false;
    }

    // [RUNTIME-FIX 0/0] Erfolg gegen getCurrentGunAmmo (Laufzeit) pruefen, NICHT
    // gegen wi:get_CurrentAmmoCount (= evtl. ein Spiegel -> der Write scheint zu
    // gelingen, addAmmoCount wird uebersprungen und die Reserve ist weg).
    const int32_t base = call_enum(pe, "getCurrentGunAmmo").value_or(loaded);
    field_i32_write(wi, "_CurrentAmmoCount", 0x44, loaded + 1);
    int32_t af = call_enum(pe, "getCurrentGunAmmo").value_or(base);

    if (af <= base) {
        re4vr::call_safe<void*>(wi, "addAmmoCount", 1, true);
        af = call_enum(pe, "getCurrentGunAmmo").value_or(base);
    }

    if (af <= base) {
        return false;
    }

    const int32_t gained = af - base;

    if (read_reserve() >= r_b4 && inv != nullptr && ammo_id.has_value()) {
        m_main->safe_reduce(inv, *ammo_id, gained);
    }

    return true;
}

void RE4VRReload3::rl_refresh() {
    const auto wid = get_equip_wid();

    if (!(m_rlcfg.rl_enabled && wid.has_value() && is_rl(*wid))) {
        m_rlwep = RlWep{};

        return;
    }

    m_rlwep.wid = wid;
    glm::vec3 p{};

    if (!(m_rlwep.tf != nullptr && get_vec3(m_rlwep.tf, "get_Position", p))) {
        m_rlwep.tf = find_weapon(*wid);
    }

    if (m_rlwep.tf == nullptr) {
        return;
    }

    m_rlwep.warhead_joint = joint_by_name(m_rlwep.tf, RL_J_WARHEAD);
    auto* dj = m_rlcfg.dock_joint.empty() ? nullptr
                                          : joint_by_name(m_rlwep.tf, m_rlcfg.dock_joint);
    m_rlwep.dock_joint = (dj != nullptr) ? dj : m_rlwep.warhead_joint;
}

bool RE4VRReload3::rl_set_mag_in_hand(bool active) {
    if (active) {
        if (m_rlst.cart) {
            return true;
        }

        if (!rl_can_grab()) {
            return false;
        }

        m_rlst.cart = true;
        rl_play_sound(RLSND_GRAB);

        return true;
    }

    m_rlst.cart = false;

    return true;
}

std::optional<glm::vec3> RE4VRReload3::rl_dock_world() {
    auto* dj = m_rlwep.dock_joint;
    glm::vec3 p{};

    if (dj == nullptr || !get_vec3(dj, "get_Position", p)) {
        return std::nullopt;
    }

    glm::quat r{};

    if (get_quat(dj, "get_Rotation", r)) {
        return p + (r * glm::vec3{m_rlcfg.dock_x, m_rlcfg.dock_y, m_rlcfg.dock_z});
    }

    return p;
}

void RE4VRReload3::rl_update_insert() {
    if (!m_rlst.cart) {
        return;
    }

    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return;
    }

    const auto dp = rl_dock_world();

    if (!dp.has_value()) {
        return;
    }

    if (vec_len(vec_sub(hp, *dp)) > m_rlcfg.insert_distance) {
        return;
    }

    if (rl_load_one()) {
        m_rlst.cart = false;
        rl_play_sound(RLSND_INSERT);
    }
}

// Hand-Pose (Warhead halten): absolute Pose, dann additiv Daumen/Zeigefinger.
void RE4VRReload3::rl_apply_pose() {
    if (!(m_rlcfg.rl_enabled && m_rlwep.wid.has_value())) {
        return;
    }

    // geteilt: alle RL-Varianten nutzen dieselbe Pose
    const auto& p = m_rlpose;
    std::string want{};

    if ((m_rlst.cart || m_rlst.preview) && !p.pose.empty()) {
        want = p.pose;
    }

    float b = 0.0f;

    if (!pose_fade_step(m_rl_fade, want, b)) {
        return;
    }

    pose_apply(m_rl_warhead_pose, b);

    auto* bt = body_tf();

    if (p.t_rx != 0.0f || p.t_ry != 0.0f || p.t_rz != 0.0f) {
        auto* tj = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (tj != nullptr && get_quat(tj, "get_LocalRotation", cur)) {
            set_quat(tj, "set_LocalRotation",
                     glm::normalize(cur * quat_from_euler(p.t_rx * b, p.t_ry * b,
                                                          p.t_rz * b)));
        }
    }

    if (p.i_rx != 0.0f || p.i_ry != 0.0f || p.i_rz != 0.0f) {
        static const char* const IDX[3] = {"L_IndexF1", "L_IndexF2", "L_IndexF3"};

        for (const char* jn : IDX) {
            auto* jt = (bt != nullptr) ? joint_by_name(bt, jn) : nullptr;
            glm::quat cur{};

            if (jt != nullptr && get_quat(jt, "get_LocalRotation", cur)) {
                set_quat(jt, "set_LocalRotation",
                         glm::normalize(cur * quat_from_euler(p.i_rx * b, p.i_ry * b,
                                                              p.i_rz * b)));
            }
        }
    }
}

void RE4VRReload3::rl_on_frame() {
    rl_refresh();

    if (m_rlwep.wid != m_rl_prev_wid) {
        const bool was_rl = m_rl_prev_wid.has_value();
        m_rlst.cart = false;

        if (was_rl && !m_rlwep.wid.has_value()) {
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
        }

        m_rl_prev_wid = m_rlwep.wid;
    }

    if (!m_rlwep.wid.has_value()) {
        // [5. PUNKT] wh_follow haengt in Lua zusaetzlich am on_frame -- es raeumt
        // den Klon auf, wenn keine RL mehr gefuehrt wird.
        wh_follow();

        return;
    }

    // kein nativer Reload; B macht nichts
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);
    rl_update_insert();
    // Holster-Gate: buzzen nur, wenn NICHTS einsetzbar ist
    re4vr::lua_set_bool("__re4_reload_grab_empty", !m_rlst.cart && !rl_can_grab());
    // Support/Two-Hand weichen, solange der Warhead in der linken Hand ist
    re4vr::lua_set_bool("__vr_mag_in_hand", m_rlst.cart);
    wh_follow();
}

void RE4VRReload3::rl_apply_pass() {
    if (!(m_rlcfg.rl_enabled && m_rlwep.wid.has_value())) {
        return;
    }

    rl_apply_pose();
}

// ---- [WARHEAD-IN-HAND] Mesh-Klon von Part 03 an der linken Hand ----
::REManagedObject* RE4VRReload3::rl_gun_mesh() {
    if (m_rlwep.tf == nullptr) {
        return nullptr;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_rlwep.tf, "get_GameObject");

    return (go != nullptr) ? re4vr::get_component(go, "via.render.Mesh") : nullptr;
}

void RE4VRReload3::wh_destroy() {
    if (m_whclone.obj != nullptr) {
        re4vr::destroy_game_object(m_whclone.obj);
    }

    m_whclone.obj = nullptr;
    m_whclone.mesh = nullptr;
    m_whclone.wid.reset();
    m_whclone.parts_sig.clear();
    m_whclone.parented = false;
}

bool RE4VRReload3::wh_spawn() {
    if (m_whclone.obj != nullptr) {
        return true;
    }

    auto* gmesh = rl_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_rl_warhead");

    if (go == nullptr) {
        return false;
    }

    auto* gom = reinterpret_cast<::REManagedObject*>(go);

    if (auto* mt = re4vr::runtime_type("via.motion.Motion"); mt != nullptr) {
        re4vr::call_safe<::REManagedObject*>(gom, "createComponent(System.Type)", mt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* rt = re4vr::runtime_type("via.render.Mesh"); rt != nullptr) {
        mesh = re4vr::call_safe<::REManagedObject*>(gom, "createComponent(System.Type)", rt);
    }

    if (mesh == nullptr) {
        return false;
    }

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (gmat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", gmat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    m_whclone.obj = gom;
    m_whclone.mesh = mesh;
    m_whclone.wid = m_rlwep.wid;

    // [NO_LAG] nativ ans L_Hand-Joint parenten.
    m_whclone.parented = false;
    auto* ctf = re4vr::call_safe<::REManagedObject*>(gom, "get_Transform");
    auto* bt2 = body_tf();

    if (ctf != nullptr && bt2 != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", bt2);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_ParentJoint", jn);
            m_whclone.parented = true;
        }
    }

    return true;
}

void RE4VRReload3::wh_isolate() {
    if (m_whclone.mesh == nullptr) {
        return;
    }

    const std::string sig = "parts:" + m_rlcfg.parts;

    if (m_whclone.parts_sig == sig) {
        return;
    }

    const auto keep = parse_parts(m_rlcfg.parts);
    bool applied = true;

    for (int32_t i = 0; i <= 48; ++i) {
        if (!re4vr::obj_ok(m_whclone.mesh)) {
            applied = false;

            break;
        }

        const bool on = std::find(keep.begin(), keep.end(), i) != keep.end();
        re4vr::call_safe<void*>(m_whclone.mesh, "setPartsEnable", i, on);
    }

    if (applied) {
        m_whclone.parts_sig = sig;
    }
}

void RE4VRReload3::wh_place(::REManagedObject* tf) {
    const auto& g = m_rlcfg;

    // [NO_LAG] geparentet ans L_Hand: nur LOKALE Pose (der Offset war schon
    // hand-relativ = jetzt lokal).
    if (m_whclone.parented) {
        set_vec3(tf, "set_LocalPosition", glm::vec3{g.wx, g.wy, g.wz});
        set_quat(tf, "set_LocalRotation", quat_from_euler(g.wrx, g.wry, g.wrz));
        set_vec3(tf, "set_LocalScale", glm::vec3{g.wscale, g.wscale, g.wscale});

        return;
    }

    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return;
    }

    glm::quat hr{};
    const bool have_hr = get_quat(lhj, "get_Rotation", hr);
    glm::vec3 w = hp;
    std::optional<glm::quat> rot{};

    if (have_hr) {
        w = hp + (hr * glm::vec3{g.wx, g.wy, g.wz});
        rot = glm::normalize(hr * quat_from_euler(g.wrx, g.wry, g.wrz));
    }

    sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf),
                                Vector4f{w.x, w.y, w.z, 1.0f}, true);

    if (rot.has_value()) {
        sdk::set_transform_rotation(reinterpret_cast<::RETransform*>(tf), *rot);
    }

    set_vec3(tf, "set_LocalScale", glm::vec3{g.wscale, g.wscale, g.wscale});
}

void RE4VRReload3::wh_follow() {
    if (!(m_rlcfg.rl_enabled && m_rlwep.wid.has_value())) {
        if (m_whclone.obj != nullptr) {
            wh_destroy();
        }

        return;
    }

    if (!(m_rlst.cart || m_rlst.preview)) {
        if (m_whclone.obj != nullptr) {
            wh_destroy();
        }

        return;
    }

    if (m_whclone.obj != nullptr && m_whclone.wid != m_rlwep.wid) {
        wh_destroy();
    }

    if (m_whclone.obj == nullptr && !wh_spawn()) {
        return;
    }

    wh_isolate();
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_whclone.obj, "get_Transform");

    if (tf != nullptr) {
        wh_place(tf);
    }
}

// [WOBBLE-FIX] NACH motions BeginRendering-POST (attach_left_hand) nochmal nur
// repositionieren -> klebt sauber.
void RE4VRReload3::wh_reposition_late() {
    if (!(m_rlcfg.rl_enabled && m_rlwep.wid.has_value() && m_whclone.obj != nullptr)) {
        return;
    }

    if (!(m_rlst.cart || m_rlst.preview)) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(m_whclone.obj, "get_Transform");

    if (tf != nullptr) {
        wh_place(tf);
    }
}

void RE4VRReload3::rl_on_script_reset() {
    m_rlst.cart = false;
    wh_destroy();
    m_rlwep = RlWep{};
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
}

// ---------------------------------------------------------------------
// UI -- Rocket Launcher
// ---------------------------------------------------------------------
void RE4VRReload3::rl_ui() {
    if (!ImGui::TreeNode("Rocket Launcher (wp4900 / 4901 Special / 4902 Infinite)")) {
        return;
    }

    if (ImGui::Checkbox("##rl_en", &m_rlcfg.rl_enabled)) {
        rl_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Rocket Launcher Reload");

    const auto awid = m_rlwep.wid.has_value() ? m_rlwep.wid : get_equip_wid();
    char lbl[16]{};

    if (awid.has_value()) {
        std::snprintf(lbl, sizeof(lbl), "wp%04d", *awid);
    } else {
        std::snprintf(lbl, sizeof(lbl), "-");
    }

    ImGui::Text("Equippt: %s", lbl);
    const bool known = awid.has_value() && is_rl(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Rocket Launcher erkannt - verwaltet)"
                             : "  (kein Rocket Launcher equippt)");

    auto* wi = rl_get_wi();
    const auto ld = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Warhead-Joint=%s  loaded=%s reserve=%d  in_hand=%s",
                       known ? RL_J_WARHEAD : "nil",
                       ld.has_value() ? std::to_string(*ld).c_str() : "nil",
                       rl_reserve(), m_rlst.cart ? "true" : "false");

    if (ImGui::SliderFloat("Einlege-Distanz m##rldist",
                           &m_rlcfg.insert_distance, 0.03f, 0.50f)) {
        rl_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Einsetzen nachladen (+1)##rlammo", &m_rlcfg.reload_ammo)) {
        rl_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an##rlsnd", &m_rlcfg.sound_enabled)) {
        rl_save_cfg();
    }

    if (ImGui::TreeNode("Dock (Einlege-Punkt)##rldock")) {
        bool ch = false;
        char jb[64]{};
        std::snprintf(jb, sizeof(jb), "%s", m_rlcfg.dock_joint.c_str());

        if (ImGui::InputText("Dock-Joint##rldj", jb, sizeof(jb))) {
            m_rlcfg.dock_joint = jb;
            rl_save_cfg();
        }

        ch |= ImGui::SliderFloat("Dock X (m)##rldx", &m_rlcfg.dock_x, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Dock Y (m)##rldy", &m_rlcfg.dock_y, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Dock Z (m)##rldz", &m_rlcfg.dock_z, -0.30f, 0.30f);

        if (ch) {
            rl_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Hand-Pose (Warhead halten)##rlpose")) {
        auto& p = m_rlpose;
        bool ch = false;
        ImGui::Checkbox("Vorschau: Warhead-Pose dauerhaft zeigen##rlprev", &m_rlst.preview);
        char pb[128]{};
        std::snprintf(pb, sizeof(pb), "%s", p.pose.c_str());

        if (ImGui::InputText("Pose-Name (gebacken in reload3: WarheadHold)##rlposename",
                             pb, sizeof(pb))) {
            p.pose = pb;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  WarheadHold = LE5MAG-Kopie (gestures-unabhaengig).");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen (additiv L_Thumb1, Grad):");
        ch |= ImGui::SliderFloat("Daumen RotX##rltx", &p.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##rlty", &p.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##rltz", &p.t_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Zeigefinger (additiv L_IndexF1/2/3, Grad):");
        ch |= ImGui::SliderFloat("Zeigefinger RotX##rlix", &p.i_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Zeigefinger RotY##rliy", &p.i_ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Zeigefinger RotZ##rliz", &p.i_rz, -180.0f, 180.0f);

        if (ch) {
            rl_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Warhead in der Hand (Mesh-Clone)##rlwh")) {
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Vorschau-Checkbox oben (Hand-Pose) zeigt Pose + "
                           "Warhead zusammen.");
        bool ch = false;
        char pb[64]{};
        std::snprintf(pb, sizeof(pb), "%s", m_rlcfg.parts.c_str());

        if (ImGui::InputText("Mesh-Part-Indizes (z.B. 03)##rlwhparts", pb, sizeof(pb))) {
            m_rlcfg.parts = pb;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Position (m, hand-lokal):");
        ch |= ImGui::SliderFloat("Warhead X##rlwx", &m_rlcfg.wx, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Warhead Y##rlwy", &m_rlcfg.wy, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Warhead Z##rlwz", &m_rlcfg.wz, -0.30f, 0.30f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Rotation (Grad):");
        ch |= ImGui::SliderFloat("Warhead RotX##rlwrx", &m_rlcfg.wrx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Warhead RotY##rlwry", &m_rlcfg.wry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Warhead RotZ##rlwrz", &m_rlcfg.wrz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Warhead Scale##rlwsc", &m_rlcfg.wscale, 0.1f, 3.0f);

        if (ch) {
            rl_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

// ============================================================================
// 4 -- FLAMETHROWER (wp4701, Lua Z.3206-4113)
//
// Reload-Mechanik (die Waffe existiert nur im Code, sie ist nie ins Spiel
// gekommen): _05 = Sicherung -> Right-B togglet sie auf/zu, schwenkt SEITLICH
// raus (wie die Handcannon-Trommel). _04 = Tank (= die Ammo). Bei LEER +
// Sicherung OFFEN wird der Tank aus der Kammer genommen und faellt zu Boden.
// Dann ist das Mag-Holster frei -> Griff -> DERSELBE Joint _04 erscheint an der
// Hand -> an die Waffe (Dock-Naehe) -> einclippen -> Ammo voll. Loop.
// KEINE Engine-Reserve -> virtuell (CFG.reserve).
// JSON: re4_vr/re4_vr_reload3_flamethrower.json
// ============================================================================
namespace {

constexpr int32_t FT = 4701;
constexpr const char* FT_CFG_PATH = "re4_vr/re4_vr_reload3_flamethrower.json";
constexpr const char* FT_J_SAFETY = "_05";
constexpr const char* FT_J_TANK = "_04";
constexpr int32_t FLAME_FUEL_ID = 112814400;
constexpr float FT_GRAV = 9.8f;

constexpr uint32_t FT_FIRE_LOOP_SND = 961885189u;   // gehaltener Feuer-Strahl
constexpr uint32_t FT_BURST_SND = 1470667332u;      // kurzer Tap = Feuer-Burst
// kein Verbrauch so lange -> das Feuern hat gestoppt (Tap-Entscheid, eng)
constexpr double FT_CLASSIFY_GAP = 0.05;
// Loop-Nachlauf nach dem Loslassen (robuster gegen Frame-Haenger)
constexpr double FT_HOLD_STOP_TAIL = 0.10;

constexpr uint32_t FTSND_SAFETY = 3042341191u;   // Sicherung oeffnen
constexpr uint32_t FTSND_DROP = 3511992014u;     // Kanister kommt auf dem Boden auf
constexpr uint32_t FTSND_GRAB = 1839787494u;     // Tank aus dem Holster gegriffen
constexpr uint32_t FTSND_INSERT = 2698018700u;   // neuen Kanister einstecken
constexpr uint32_t FTSND_DRY = 812850326u;       // Dry-Fire

// Flammen-Familie: 4701 = direkter Tick (Basis nur 1!), 5801 = Flammen-Treffer,
// 5803/5804 = Entzuendungs-Burst (Basis 210-490).
bool is_flame_id(int32_t wid) {
    return wid == 4701 || wid == 5801 || wid == 5803 || wid == 5804;
}

// Rechter Controller-Trigger gedrueckt? (roh, unabhaengig vom Consume)
bool right_trigger_down() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto act = vr->get_action_trigger();
    const auto js = vr->get_right_joystick();

    if (act == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active(act, js);
    } catch (...) {
        return false;
    }
}

}   // namespace

bool RE4VRReload3::is_ft(int32_t wid) {
    return wid == FT;
}

void RE4VRReload3::ft_load_cfg() {
    const auto data = re4vr::json_load(FT_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    const nlohmann::json& c = (data.contains("cfg") && data["cfg"].is_object())
                              ? data["cfg"] : data;
    auto& g = m_ftcfg;
    g.ft_enabled = jbool(c, "ft_enabled", g.ft_enabled);
    g.sound_enabled = jbool(c, "sound_enabled", g.sound_enabled);
    g.saf_rx = jnum(c, "saf_rx", g.saf_rx);
    g.saf_ry = jnum(c, "saf_ry", g.saf_ry);
    g.saf_rz = jnum(c, "saf_rz", g.saf_rz);
    g.saf_lerp = jnum(c, "saf_lerp", g.saf_lerp);
    g.dock_x = jnum(c, "dock_x", g.dock_x);
    g.dock_y = jnum(c, "dock_y", g.dock_y);
    g.dock_z = jnum(c, "dock_z", g.dock_z);
    g.insert_distance = jnum(c, "insert_distance", g.insert_distance);
    g.dock_catch = jnum(c, "dock_catch", g.dock_catch);
    g.hand_x = jnum(c, "hand_x", g.hand_x);
    g.hand_y = jnum(c, "hand_y", g.hand_y);
    g.hand_z = jnum(c, "hand_z", g.hand_z);
    g.hand_rx = jnum(c, "hand_rx", g.hand_rx);
    g.hand_ry = jnum(c, "hand_ry", g.hand_ry);
    g.hand_rz = jnum(c, "hand_rz", g.hand_rz);
    g.mag_size = static_cast<int32_t>(jnum(c, "mag_size", static_cast<float>(g.mag_size)));
    g.burst_enabled = jbool(c, "burst_enabled", g.burst_enabled);
    g.hold_threshold = jnum(c, "hold_threshold", g.hold_threshold);
    g.reserve = static_cast<int32_t>(jnum(c, "reserve", static_cast<float>(g.reserve)));
    g.unlimited = jbool(c, "unlimited", g.unlimited);
    g.damage_enabled = jbool(c, "damage_enabled", g.damage_enabled);
    g.damage_mult = jnum(c, "damage_mult", g.damage_mult);
    g.damage_floor = jnum(c, "damage_floor", g.damage_floor);
    g.damage_native = jbool(c, "damage_native", g.damage_native);
    g.damage_stopping = jnum(c, "damage_stopping", g.damage_stopping);
    g.damage_wince = jnum(c, "damage_wince", g.damage_wince);
    g.damage_break = jnum(c, "damage_break", g.damage_break);
    g.damage_hp_fallback = jbool(c, "damage_hp_fallback", g.damage_hp_fallback);
    g.fh_rx = jnum(c, "fh_rx", g.fh_rx);
    g.fh_ry = jnum(c, "fh_ry", g.fh_ry);
    g.fh_rz = jnum(c, "fh_rz", g.fh_rz);
    g.ca_trx = jnum(c, "ca_trx", g.ca_trx);
    g.ca_try = jnum(c, "ca_try", g.ca_try);
    g.ca_trz = jnum(c, "ca_trz", g.ca_trz);
}

void RE4VRReload3::ft_save_cfg() {
    const auto& g = m_ftcfg;
    nlohmann::json d = {{"cfg", {
        {"ft_enabled", g.ft_enabled}, {"sound_enabled", g.sound_enabled},
        {"saf_rx", g.saf_rx}, {"saf_ry", g.saf_ry}, {"saf_rz", g.saf_rz},
        {"saf_lerp", g.saf_lerp},
        {"dock_x", g.dock_x}, {"dock_y", g.dock_y}, {"dock_z", g.dock_z},
        {"insert_distance", g.insert_distance}, {"dock_catch", g.dock_catch},
        {"hand_x", g.hand_x}, {"hand_y", g.hand_y}, {"hand_z", g.hand_z},
        {"hand_rx", g.hand_rx}, {"hand_ry", g.hand_ry}, {"hand_rz", g.hand_rz},
        {"mag_size", g.mag_size},
        {"burst_enabled", g.burst_enabled}, {"hold_threshold", g.hold_threshold},
        {"reserve", g.reserve}, {"unlimited", g.unlimited},
        {"damage_enabled", g.damage_enabled}, {"damage_mult", g.damage_mult},
        {"damage_floor", g.damage_floor}, {"damage_native", g.damage_native},
        {"damage_stopping", g.damage_stopping}, {"damage_wince", g.damage_wince},
        {"damage_break", g.damage_break},
        {"damage_hp_fallback", g.damage_hp_fallback},
        {"fh_rx", g.fh_rx}, {"fh_ry", g.fh_ry}, {"fh_rz", g.fh_rz},
        {"ca_trx", g.ca_trx}, {"ca_try", g.ca_try}, {"ca_trz", g.ca_trz},
    }}};

    re4vr::json_save(FT_CFG_PATH, d);
}

void RE4VRReload3::ft_play_sound(uint32_t id) {
    if (!m_ftcfg.sound_enabled || id == 0 || m_ftwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_ftwep.tf, "get_GameObject"), id);
}

// =====================================================================
// FEUER-LOOP-SOUND: laeuft, solange gefeuert wird, und stoppt beim Loslassen.
// Die Waffe selbst gibt KEINEN Schuss-Sound aus -> wir spielen ihn manuell.
// ERKENNUNG am Munitions-Verbrauch (die Engine zaehlt beim Feuern runter, ~54/s)
// plus kurze Nachlauf-Toleranz -> kein unsicheres Engine-Flag noetig.
// STOP: Wwise stopEvent(go, EventId, fade) ueber die statische SoundManager --
// ein Loop laesst sich NUR per EventId stoppen, nicht per TriggerId -- plus
// stopTriggered mit dem RICHTIGEN (Waffen-)GO.
// =====================================================================
::REManagedObject* RE4VRReload3::ft_sound_container(::REManagedObject** go_out) {
    if (go_out != nullptr) {
        *go_out = nullptr;
    }

    if (m_ftwep.tf == nullptr) {
        return nullptr;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_ftwep.tf, "get_GameObject");

    if (go == nullptr) {
        return nullptr;
    }

    auto* scn = re4vr::get_component(go, "soundlib.SoundContainer");

    if (scn == nullptr) {
        return nullptr;
    }

    if (go_out != nullptr) {
        *go_out = go;
    }

    return scn;
}

// EventId(s) zu einer TriggerId aus der _TriggerInfoList des Containers holen.
std::vector<uint32_t> RE4VRReload3::ft_event_ids_for(::REManagedObject* scn,
                                                     uint32_t trigid) {
    std::vector<uint32_t> out;
    auto* list = re4vr::get_field_object(scn, "_TriggerInfoList");

    if (list == nullptr) {
        return out;
    }

    const int32_t cnt = call_enum(list, "get_Count").value_or(0);
    auto* items = re4vr::get_field_object(list, "_items");

    for (int32_t i = 0; i < cnt; ++i) {
        auto* e = (items != nullptr) ? re4vr::array_element(items, i) : nullptr;

        if (e == nullptr) {
            e = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);
        }

        if (e == nullptr) {
            continue;
        }

        const auto tid = re4vr::get_field_int(e, "_TriggerId");

        if (!tid.has_value() || static_cast<uint32_t>(*tid) != trigid) {
            continue;
        }

        if (const auto eid = re4vr::get_field_int(e, "_EventId"); eid.has_value()) {
            out.push_back(static_cast<uint32_t>(*eid));
        }
    }

    return out;
}

void RE4VRReload3::ft_fire_sound_start() {
    if (m_fire_snd.on || !m_ftcfg.sound_enabled) {
        return;
    }

    ::REManagedObject* go = nullptr;
    auto* scn = ft_sound_container(&go);

    if (scn == nullptr || go == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", FT_FIRE_LOOP_SND);
    m_fire_snd.on = true;
    m_fire_snd.scn = scn;
    m_fire_snd.go = go;
}

void RE4VRReload3::ft_fire_sound_stop() {
    if (!m_fire_snd.on) {
        return;
    }

    m_fire_snd.on = false;
    auto* scn = m_fire_snd.scn;
    auto* go = m_fire_snd.go;

    if (scn == nullptr || go == nullptr) {
        scn = ft_sound_container(&go);
    }

    m_fire_snd.scn = nullptr;
    m_fire_snd.go = nullptr;

    if (scn == nullptr || go == nullptr) {
        return;
    }

    static auto* sm_td = sdk::find_type_definition("soundlib.SoundManager");
    static auto* stop_event = (sm_td != nullptr)
        ? sm_td->get_method("stopEvent(via.GameObject, System.UInt32, System.UInt32)")
        : nullptr;

    if (stop_event != nullptr) {
        for (const uint32_t ev : ft_event_ids_for(scn, FT_FIRE_LOOP_SND)) {
            if (ev > 0) {
                stop_event->call<void*>(sdk::get_thread_context(), nullptr, go, ev, 0u);
            }
        }
    }

    re4vr::call_safe<void*>(
        scn, "stopTriggered(System.UInt32, via.GameObject, System.UInt32)",
        FT_FIRE_LOOP_SND, go, 0u);
}

// Einmaliger Tap-Burst (One-Shot, kein Stop noetig)
void RE4VRReload3::ft_play_burst() {
    ::REManagedObject* go = nullptr;
    auto* scn = ft_sound_container(&go);

    if (scn == nullptr || go == nullptr || !m_ftcfg.sound_enabled) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", FT_BURST_SND);
}

// ---- Ammo (virtuelle Reserve; Engine-Fuel ueber das Waffen-Item) ----
// WICHTIG: getEquipWeaponItem liefert eine SPIEGEL-Kopie -> Lesen zeigt den
// echten Wert, aber Schreiben VERPUFFT (die Live-Gun schreibt 0 zurueck).
::REManagedObject* RE4VRReload3::ft_get_wi() {
    // [ACCESSOR] zuerst die ECHTE, persistente Instanz.
    if (m_main != nullptr) {
        if (auto* rw = m_main->real_wi(); rw != nullptr) {
            return rw;
        }
    }

    const auto ewid = get_equip_wid();
    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    // (1) ECHTES Item direkt aus dem Inventar -> robust gegen "Reset Scripts".
    const auto et_main = re4vr::enum_value("chainsaw.EquipType", "Main");

    if (inv != nullptr && et_main.has_value()) {
        auto* rwi = re4vr::call_safe<::REManagedObject*>(inv, "getEquippedWeapon", *et_main);

        if (rwi != nullptr && call_enum(rwi, "get_CurrentAmmoCount").has_value()) {
            const auto cwid = call_enum(rwi, "get_WeaponId");

            // STRIKT: nur benutzen, wenn es dieselbe Waffe ist (kein stale)
            if (cwid.has_value() && ewid.has_value() && *cwid == *ewid) {
                return rwi;
            }
        }
    }

    // (2) Live-Handle aus reload (nur gueltig, solange dessen Hook frisch ist)
    auto* wi = re4vr::lua_get_pointer("__re4_live_wi");

    if (wi != nullptr && call_enum(wi, "get_CurrentAmmoCount").has_value()) {
        const auto cwid = call_enum(wi, "get_WeaponId");

        if (cwid.has_value() && ewid.has_value() && *cwid == *ewid) {
            return wi;
        }
    }

    // (3) Spiegel-Kopie (nur fuers Lesen brauchbar)
    auto* ewi = re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

int32_t RE4VRReload3::ft_ammo() {
    auto* wi = ft_get_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount").value_or(0) : 0;
}

bool RE4VRReload3::ft_reserve_ok() const {
    return m_ftcfg.unlimited || m_ftcfg.reserve > 0;
}

// Fake-Reload: das Magazin (Feld 0x44) auf mag_size setzen, geclampt auf den
// Engine-Max. KERN: die Engine haelt das Magazin nur, wenn echte
// flame_fuel-Reserve im Inventar liegt -> Reserve auffuellen, dann laden.
bool RE4VRReload3::ft_refill() {
    auto* wi = ft_get_wi();

    if (wi == nullptr || m_main == nullptr) {
        return false;
    }

    const int32_t engine_max = call_enum(wi, "get_CurrentAmmoMax").value_or(0);
    int32_t target = m_ftcfg.mag_size;

    if (engine_max > 0 && target > engine_max) {
        target = engine_max;
    }

    if (target < 0) {
        target = 0;
    }

    // Ammo-Typ der Waffe (UsableAmmoList[0] = flame_fuel) ermitteln.
    // [ARRAY-BINDING] get_size/[0] sind Lua-BINDINGS, keine managed Calls.
    // [ARRAY-BINDING] `ual:get_size()` und `ual[0]` sind Lua-BINDINGS, keine
    // managed Calls -- als get_Count/get_Item portiert liefern sie stumm nichts.
    auto* ual = re4vr::call_safe<::REManagedObject*>(wi, "get_UsableAmmoList");
    std::optional<int32_t> ammo0{};

    if (ual != nullptr && re4vr::array_size(ual) > 0) {
        if (auto* e = re4vr::array_element(ual, 0); e != nullptr) {
            ammo0 = re4vr::get_field_int(e, "value__");
        }
    }

    const auto amidv = call_enum(wi, "get_CurrentAmmo");

    // Keine gueltige Ammo-ID gesetzt? -> setzen, sonst zwingt die Engine den
    // Count auf 0.
    if ((!amidv.has_value() || *amidv == 0) && ammo0.has_value()) {
        re4vr::call_safe<void*>(wi, "setAmmoId", *ammo0);
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    // KERN: der Ladeweg ist ADDITIV und verzoegert (Endwert = Magazin + count).
    // Damit das Magazin GENAU auf target landet (nicht target + Rest), laden wir
    // nur die Differenz nach.
    const int32_t current = call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    const int32_t need = target - current;

    if (current > target) {
        // zu viel drin -> die ECHTE Engine-Methode reduceAmmoCount (ein
        // forceSet verpufft, per Log bewiesen).
        re4vr::call_safe<void*>(wi, "reduceAmmoCount", current - target);

        return true;
    }

    if (need <= 0) {
        return true;
    }

    // echte flame_fuel-Reserve fuer die Differenz sicherstellen
    if (inv != nullptr && ammo0.has_value()) {
        const int32_t rsv_b = m_main->item_count_sum(inv, *ammo0);

        if (rsv_b < need) {
            static auto* gui_td = sdk::find_type_definition("chainsaw.ChainsawGuiUtil");
            static auto* gen_item = (gui_td != nullptr) ? gui_td->get_method("generateItem")
                                                        : nullptr;

            if (gen_item != nullptr) {
                auto* newitem = gen_item->call<::REManagedObject*>(
                    sdk::get_thread_context(), nullptr, FLAME_FUEL_ID, 1000, -1, -1, 0, 0);

                if (newitem != nullptr) {
                    re4vr::call_safe<void*>(inv, "pickupItem(chainsaw.Item)", newitem);
                }
            }
        }
    }

    // nur die Differenz nachladen -> die Engine addiert (current + need) = target
    const auto et_main = re4vr::enum_value("chainsaw.EquipType", "Main");

    if (inv != nullptr && et_main.has_value()) {
        if ((!amidv.has_value() || *amidv == 0) && ammo0.has_value()) {
            re4vr::call_safe<void*>(wi, "setAmmoId", *ammo0);
        }

        // [CRASH-HARDEN] Engine-Gate enableReloadItem gegen den null-Item-AV.
        // refill = true -> gibt Munition, ohne Reserve zu buchen.
        m_main->load_and_book(inv, *et_main, need, true);
    }

    return true;
}

void RE4VRReload3::ft_refresh() {
    const auto wid = get_equip_wid();

    if (!(m_ftcfg.ft_enabled && wid.has_value() && is_ft(*wid))) {
        m_ftwep.wid.reset();
        m_ftwep.tf = nullptr;
        m_ftwep.safety_joint = nullptr;
        m_ftwep.tank_joint = nullptr;

        return;
    }

    m_ftwep.wid = wid;
    glm::vec3 p{};

    if (!(m_ftwep.tf != nullptr && get_vec3(m_ftwep.tf, "get_Position", p))) {
        m_ftwep.tf = find_weapon(*wid);
    }

    if (m_ftwep.tf == nullptr) {
        return;
    }

    m_ftwep.safety_joint = joint_by_name(m_ftwep.tf, FT_J_SAFETY);
    m_ftwep.tank_joint = joint_by_name(m_ftwep.tf, FT_J_TANK);

    // [SAVE-LOAD-FEST] Die Sicherungs-Ruhe ist die BIND-Pose -- eine Konstante,
    // die unsere eigenen Writes nie beruehren.
    if (m_ftwep.safety_joint != nullptr && !m_ftwep.safety_rest_rot.has_value()) {
        glm::quat r{};

        if (get_quat(m_ftwep.safety_joint, "get_BaseLocalRotation", r)) {
            m_ftwep.safety_rest_rot = r;
        }
    }
}

// ---- Sicherung _05 (Right-B Toggle, seitlicher Swing) ----
void RE4VRReload3::ft_update_safety() {
    if (m_ftwep.safety_joint == nullptr) {
        return;
    }

    const bool b = right_b_down();

    if (b && !m_saf._prev_b && !m_saf.preview) {
        m_saf.open = !m_saf.open;

        if (m_saf.open) {
            // Drop des alten Kanisters einmalig pro Oeffnen
            m_tank._drop_armed = true;
        }

        ft_play_sound(FTSND_SAFETY);
    }

    m_saf._prev_b = b;

    const float target = (m_saf.open || m_saf.preview) ? 1.0f : 0.0f;

    if (m_saf.prog < target) {
        m_saf.prog = std::min(target, m_saf.prog + m_ftcfg.saf_lerp);
    } else if (m_saf.prog > target) {
        m_saf.prog = std::max(target, m_saf.prog - m_ftcfg.saf_lerp);
    }
}

void RE4VRReload3::ft_apply_safety() {
    if (!(m_ftwep.safety_joint != nullptr && m_ftwep.safety_rest_rot.has_value())) {
        return;
    }

    const float p = m_saf.prog;
    set_quat(m_ftwep.safety_joint, "set_LocalRotation",
             glm::normalize(*m_ftwep.safety_rest_rot
                            * quat_from_euler(m_ftcfg.saf_rx * p, m_ftcfg.saf_ry * p,
                                              m_ftcfg.saf_rz * p)));
}

// ---- Tank _04: Drop / Boden / in der Hand (Welt-Override; idle = Engine) ----
void RE4VRReload3::ft_start_drop() {
    auto* j = m_ftwep.tank_joint;
    glm::vec3 p{};

    if (j == nullptr || !get_vec3(j, "get_Position", p)) {
        return;
    }

    m_tank.sx = p.x;
    m_tank.sy = p.y;
    m_tank.sz = p.z;
    auto* bt = body_tf();
    glm::vec3 bp{};
    m_tank.floor_y = (bt != nullptr && get_vec3(bt, "get_Position", bp)) ? bp.y : 0.0f;
    m_tank.t0 = clock_now();
    m_tank.snd = false;
    m_tank.phase = "dropping";
}

std::optional<glm::vec3> RE4VRReload3::ft_dock_world() {
    auto* sj = m_ftwep.safety_joint;
    glm::vec3 p{};

    if (sj == nullptr || !get_vec3(sj, "get_Position", p)) {
        return std::nullopt;
    }

    glm::quat gr{};

    if (m_ftwep.tf != nullptr && get_quat(m_ftwep.tf, "get_Rotation", gr)) {
        return p + (gr * glm::vec3{m_ftcfg.dock_x, m_ftcfg.dock_y, m_ftcfg.dock_z});
    }

    return p;
}

// Kanister (Joint _04) der linken Hand folgen lassen, mit dem hand_*-Offset.
void RE4VRReload3::ft_follow_hand(::REManagedObject* j) {
    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};
    glm::quat hr{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)
        || !get_quat(lh, "get_Rotation", hr)) {
        return;
    }

    set_vec3(j, "set_Position",
             hp + (hr * glm::vec3{m_ftcfg.hand_x, m_ftcfg.hand_y, m_ftcfg.hand_z}));
    set_quat(j, "set_Rotation",
             glm::normalize(hr * quat_from_euler(m_ftcfg.hand_rx, m_ftcfg.hand_ry,
                                                 m_ftcfg.hand_rz)));
}

void RE4VRReload3::ft_apply_tank() {
    auto* j = m_ftwep.tank_joint;

    if (j == nullptr) {
        return;
    }

    if (m_tank.preview) {
        // UI-Vorschau: Kanister an die Hand (zum Offset-Tunen)
        ft_follow_hand(j);

        return;
    }

    if (m_tank.phase == "dropping") {
        const float t = static_cast<float>(clock_now() - m_tank.t0);
        float y = m_tank.sy - 0.5f * FT_GRAV * t * t;

        if (y <= m_tank.floor_y) {
            y = m_tank.floor_y;
            m_tank.phase = "floor";

            if (!m_tank.snd) {
                m_tank.snd = true;
                ft_play_sound(FTSND_DROP);
            }
        }

        set_vec3(j, "set_Position", glm::vec3{m_tank.sx, y, m_tank.sz});

        return;
    }

    if (m_tank.phase == "floor") {
        set_vec3(j, "set_Position", glm::vec3{m_tank.sx, m_tank.floor_y, m_tank.sz});

        return;
    }

    if (m_tank.phase == "in_hand") {
        ft_follow_hand(j);
    }

    // idle: NICHT anfassen -> die Engine skinnt _04 zurueck in die Kammer.
}

// Der gedroppte (alte/leere) Kanister wird in der "floor"-Phase AUSGEBLENDET --
// weggeworfen darf er nicht sichtbar liegenbleiben. Joint _04 ist dasselbe Mesh
// wie der neue Kanister, darum beim Greifen / nach dem Einsetzen wieder
// einblenden. Die Scale muss in JEDEM Render-Pass gesetzt werden, sonst skinnt
// die Engine sie zurueck (wie beim Chicago-Mag).
void RE4VRReload3::ft_apply_tank_visibility() {
    auto* j = m_ftwep.tank_joint;
    const bool should_hide = (m_tank.phase == "floor") && !m_tank.preview && j != nullptr;

    if (should_hide) {
        set_vec3(j, "set_LocalScale", glm::vec3{0.0f, 0.0f, 0.0f});
        m_ft_tank_hidden = true;
    } else if (m_ft_tank_hidden) {
        if (j != nullptr) {
            set_vec3(j, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
        }

        m_ft_tank_hidden = false;
    }
}

float RE4VRReload3::ft_hand_dock_dist() {
    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};
    const auto dp = ft_dock_world();

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp) || !dp.has_value()) {
        return 999.0f;
    }

    return vec_len(vec_sub(hp, *dp));
}

// Kanister in die Waffe einsetzen (auftanken, angedockt bleiben). Die Sicherung
// bleibt offen -> das Feuer bleibt gesperrt, bis man sie mit Right-B schliesst.
void RE4VRReload3::ft_do_insert() {
    ft_refill();

    if (!m_ftcfg.unlimited) {
        m_ftcfg.reserve = std::max(0, m_ftcfg.reserve - 1);
    }

    m_tank.phase = "idle";
    m_tank._insert_t = clock_now();
    ft_play_sound(FTSND_INSERT);
}

// Holster-Griff: nur wenn der Tank auf dem Boden liegt + Reserve da ist.
bool RE4VRReload3::ft_set_mag_in_hand(bool active) {
    if (active) {
        if (m_tank.phase != "floor" || !ft_reserve_ok()) {
            return false;
        }

        m_tank.phase = "in_hand";
        ft_play_sound(FTSND_GRAB);

        return true;
    }

    // Loslassen: nah an der Waffe -> EINSETZEN (bleibt angedockt); weit weg ->
    // fallen lassen.
    if (m_tank.phase == "in_hand") {
        if (ft_hand_dock_dist() <= m_ftcfg.dock_catch) {
            ft_do_insert();
        } else {
            ft_start_drop();
        }
    }

    return true;
}

// pose_apply mit additivem Offset je Bone (rechts-multipliziert). Der
// Flamethrower-Block hat in Lua eine EIGENE Fassung mit genau diesem Zusatz --
// deshalb steht sie hier neben der geteilten.
void RE4VRReload3::ft_pose_apply(const Bones& pose,
                                 const std::unordered_map<std::string, glm::quat>& offsets,
                                 float blend) {
    const auto& map = pose_map();

    if (map.empty() || blend <= 0.0f) {
        return;
    }

    for (const auto& e : pose) {
        const auto it = map.find(e.first);

        if (it == map.end() || it->second == nullptr) {
            continue;
        }

        auto* j = it->second;
        glm::quat q = e.second;

        if (const auto o = offsets.find(e.first); o != offsets.end()) {
            q = glm::normalize(q * o->second);
        }

        if (blend < 0.9999f) {
            // [POSE_FADE] nlerp von der aktuellen (nativen) Rotation zum Ziel
            glm::quat c{};

            if (get_quat(j, "get_LocalRotation", c)) {
                glm::quat t = q;

                if ((c.w * t.w + c.x * t.x + c.y * t.y + c.z * t.z) < 0.0f) {
                    t = glm::quat{-t.w, -t.x, -t.y, -t.z};
                }

                const glm::quat r{c.w + (t.w - c.w) * blend, c.x + (t.x - c.x) * blend,
                                  c.y + (t.y - c.y) * blend, c.z + (t.z - c.z) * blend};
                const float len = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);

                if (len > 1e-6f) {
                    q = glm::quat{r.w / len, r.x / len, r.y / len, r.z / len};
                }
            }
        }

        set_quat(j, "set_LocalRotation", q);
    }
}

void RE4VRReload3::ft_apply_poses() {
    if (!(m_ftcfg.ft_enabled && m_ftwep.wid.has_value())) {
        return;
    }

    if (body_tf() == nullptr) {
        return;
    }

    // RECHTE Hand: IMMER flamehand (no-aim + aim), mit Palm-Offset.
    ft_pose_apply(m_pose_flamehand,
                  {{"R_Palm", quat_from_euler(m_ftcfg.fh_rx, m_ftcfg.fh_ry, m_ftcfg.fh_rz)}},
                  1.0f);

    // LINKE Hand: der Tank-Griff schlaegt den Support; Support nur, wenn
    // gedockt / 2-Hand.
    // [POSE_FADE] Canister-Pose beim Loslassen (Tank eingesetzt) zurueckblenden.
    const std::string want =
        (m_tank.phase == "in_hand" || m_ft_pose_prev.canister) ? "canister" : "";
    float b = 0.0f;

    if (pose_fade_step(m_ft_fade, want, b)) {
        ft_pose_apply(m_pose_flamecanister,
                      {{"L_Thumb1", quat_from_euler(m_ftcfg.ca_trx * b, m_ftcfg.ca_try * b,
                                                    m_ftcfg.ca_trz * b)}},
                      b);
    } else if (m_ft_pose_prev.support
               || re4vr::lua_get_number("__vr_support_blend_factor", 0.0) > 0.05) {
        ft_pose_apply(m_pose_flamesupport, {}, 1.0f);
    }
}

void RE4VRReload3::ft_on_frame() {
    ft_refresh();

    if (m_ftwep.wid != m_ft_prev_wid) {
        // Waffenwechsel -> den Loop nicht haengen lassen
        ft_fire_sound_stop();
        m_fire_prev_ammo.reset();
        m_fire_active_t.reset();
        m_fire_state = "idle";
        m_tank.phase = "idle";
        m_tank._drop_armed = false;
        m_saf.open = false;
        m_saf.prog = 0.0f;

        if (m_ftwep.wid.has_value()) {
            // das echte Live-Item beim Equippen neu greifen
            re4vr::lua_set_nil("__re4_live_wi");
        }

        m_ft_prev_wid = m_ftwep.wid;
    }

    if (!m_ftwep.wid.has_value()) {
        if (m_ft_was_managed) {
            ft_fire_sound_stop();
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            re4vr::lua_set_bool("__re4_reload_grab_empty", false);
            m_ft_was_managed = false;
        }

        return;
    }

    m_ft_was_managed = true;
    // Right-B = unsere Sicherung -> kein nativer Reload
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);
    ft_update_safety();

    // Feuersound Tap vs. Hold (am Munitions-Verbrauch erkannt): kurzer Druck
    // (< hold_threshold) -> einmaliger Burst; gehalten -> Loop.
    {
        const double now = clock_now();
        const int32_t cur = ft_ammo();

        if (m_fire_prev_ammo.has_value() && cur < *m_fire_prev_ammo) {
            if (m_fire_state == "idle") {
                m_fire_state = "pending";
                m_fire_start_t = now;
            }

            m_fire_active_t = now;
        }

        m_fire_prev_ammo = cur;
        const double since = m_fire_active_t.has_value() ? (now - *m_fire_active_t) : 999.0;

        if (m_fire_state == "pending") {
            if (since < FT_CLASSIFY_GAP
                && (now - m_fire_start_t) >= m_ftcfg.hold_threshold) {
                // noch am Feuern + lang genug -> Hold/Loop
                m_fire_state = "hold";
                ft_fire_sound_start();
            } else if (since >= FT_CLASSIFY_GAP) {
                if (m_ftcfg.burst_enabled) {
                    ft_play_burst();   // vorher gestoppt -> Tap/Burst
                }

                m_fire_state = "idle";
                m_fire_active_t.reset();
            }
        } else if (m_fire_state == "hold") {
            if (since >= FT_HOLD_STOP_TAIL) {
                ft_fire_sound_stop();
                m_fire_state = "idle";
                m_fire_active_t.reset();
            }
        }
    }

    // Dry-Fire: immer wenn das Feuer gesperrt ist (Sicherung OFFEN ODER leer) +
    // frischer Trigger-Druck (steigende Flanke) -> ein Klick pro Abdruecken.
    {
        const bool trig = right_trigger_down();

        if (trig && !m_ft_dryfire_prev && (m_saf.open || ft_ammo() <= 0)) {
            ft_play_sound(FTSND_DRY);
        }

        m_ft_dryfire_prev = trig;
    }

    // Auto-Drop: NUR einmal pro Sicherung-Oeffnen (armed), Tank in der Kammer.
    // KEIN Ammo-Gate -> der Kanister kann IMMER gewechselt werden (auch
    // halbvoll); jeder neue bringt beim Einsetzen die volle Fake-Menge.
    if (!m_tank.preview && m_tank._drop_armed && m_saf.open && m_tank.phase == "idle") {
        ft_start_drop();
        m_tank._drop_armed = false;
    }

    // Auto-Einsetzen: Tank in der Hand SEHR nah am Dock -> einrasten.
    if (m_tank.phase == "in_hand" && ft_hand_dock_dist() <= m_ftcfg.insert_distance) {
        ft_do_insert();
    }

    // Holster frei NUR, wenn der Tank auf dem Boden liegt + Reserve da ist.
    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        !(m_tank.phase == "floor" && ft_reserve_ok()));

    // Feuer gesperrt, solange die Sicherung offen ODER leer ist.
    // [LIVE-EMPTY] Tank-Leer FRISCH aus der Engine (getCurrentGunAmmo, kein
    // Cache); ft_ammo geht ueber ein evtl. gecachtes Item -> nur als Fallback.
    const auto ftlive = call_enum(get_pe(), "getCurrentGunAmmo");
    const bool ft_empty = (ftlive.has_value() ? *ftlive : ft_ammo()) <= 0;
    re4vr::lua_set_bool("__vr_block_fire_when_empty", m_saf.open || ft_empty);
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload3.lua:3403");

    // Support-Hand AUS, solange der Tank in der Hand ist; nach dem Insert noch
    // kurz (Cooldown), wie bei den Revolvern.
    const bool cd = m_tank._insert_t.has_value()
        && (clock_now() - *m_tank._insert_t) < 0.45;
    re4vr::lua_set_bool("__vr_mag_in_hand", m_tank.phase == "in_hand" || cd);

    // [5. PUNKT] ft_apply_pass haengt in Lua zusaetzlich am on_frame.
    ft_apply_pass();
}

void RE4VRReload3::ft_apply_pass() {
    if (!(m_ftcfg.ft_enabled && m_ftwep.wid.has_value())) {
        return;
    }

    ft_apply_safety();
    ft_apply_tank();
    ft_apply_tank_visibility();
    ft_apply_poses();
}

// [WOBBLE-FIX] NACH motions BeginRendering-POST (und nach dem Engine-Skinning)
// nachziehen: den Tank _04 fuer ALLE aktiven Phasen (sonst skinnt die Engine ihn
// zurueck in die Kammer) und die Hand-Posen IMMER (sonst ueberschreibt die Anim
// die Finger).
void RE4VRReload3::ft_late() {
    if (!(m_ftcfg.ft_enabled && m_ftwep.wid.has_value())) {
        return;
    }

    if (m_tank.phase != "idle" || m_tank.preview) {
        ft_apply_tank();
    }

    ft_apply_tank_visibility();
    ft_apply_poses();
}

void RE4VRReload3::ft_on_script_reset() {
    ft_fire_sound_stop();
    m_fire_prev_ammo.reset();
    m_fire_active_t.reset();
    m_fire_state = "idle";
    m_tank.phase = "idle";
    m_tank._insert_t.reset();
    m_tank.preview = false;
    m_tank._drop_armed = false;
    m_saf.open = false;
    m_saf.prog = 0.0f;
    m_ftwep.wid.reset();
    m_ftwep.tf = nullptr;
    m_ftwep.safety_joint = nullptr;
    m_ftwep.tank_joint = nullptr;
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
}

// ---------------------------------------------------------------------
// UI -- Flamethrower
// ---------------------------------------------------------------------
void RE4VRReload3::ft_ui() {
    if (!ImGui::TreeNode("Flamethrower (wp4701)")) {
        return;
    }

    if (ImGui::Checkbox("##ft_en", &m_ftcfg.ft_enabled)) {
        ft_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Flamethrower Reload");

    const auto awid = m_ftwep.wid.has_value() ? m_ftwep.wid : get_equip_wid();
    char lbl[16]{};

    if (awid.has_value()) {
        std::snprintf(lbl, sizeof(lbl), "wp%04d", *awid);
    } else {
        std::snprintf(lbl, sizeof(lbl), "-");
    }

    ImGui::Text("Equippt: %s", lbl);
    const bool known = awid.has_value() && is_ft(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Flamethrower erkannt - verwaltet)"
                             : "  (kein Flamethrower equippt)");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Sicherung=%s  Tank=%s  ammo=%d  phase=%s  reserve=%d%s",
                       known ? FT_J_SAFETY : "nil", known ? FT_J_TANK : "nil",
                       ft_ammo(), m_tank.phase.c_str(), m_ftcfg.reserve,
                       m_ftcfg.unlimited ? " (unlimited)" : "");

    if (ImGui::SliderInt("Kanister-Fuellmenge (Fake-Reload -> Tank)##ftmag",
                         &m_ftcfg.mag_size, 1, 1000)) {
        ft_save_cfg();
    }

    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  (Engine-Cap=1000, voller Kanister im Spiel=100)");

    if (ImGui::Checkbox("Tap-Burst (kurzer Druck = Burst statt Loop)##ftburst",
                        &m_ftcfg.burst_enabled)) {
        ft_save_cfg();
    }

    if (m_ftcfg.burst_enabled) {
        if (ImGui::SliderFloat("Tap/Hold-Schwelle (s)##fthold",
                               &m_ftcfg.hold_threshold, 0.04f, 0.40f)) {
            ft_save_cfg();
        }
    }

    if (ImGui::Checkbox("Unlimited (Loop ohne Reserve-Limit)##ftunl", &m_ftcfg.unlimited)) {
        ft_save_cfg();
    }

    if (!m_ftcfg.unlimited) {
        if (ImGui::SliderInt("Reserve (virtuell)##ftres", &m_ftcfg.reserve, 0, 99)) {
            ft_save_cfg();
        }
    }

    if (ImGui::Checkbox("Sounds an##ftsnd", &m_ftcfg.sound_enabled)) {
        ft_save_cfg();
    }

    if (ImGui::TreeNode("Schaden (Flamethrower-Boost)##ftdmg")) {
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Boostet nur Flammen-Treffer (WeaponID 5801/4701), "
                           "Wert pro Treffer-Tick.");
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Mindestschaden greift, falls die Engine-Basis 0 ist (Burn-DoT).");
        bool ch = ImGui::Checkbox("Damage-Boost an##ftdmgen", &m_ftcfg.damage_enabled);
        ch |= ImGui::SliderFloat("Schaden-Faktor (xMult)##ftdmgmult",
                                 &m_ftcfg.damage_mult, 1.0f, 50.0f, "x%.1f");
        ch |= ImGui::SliderFloat("Mindestschaden pro Treffer##ftdmgfloor",
                                 &m_ftcfg.damage_floor, 0.0f, 500.0f, "%.0f");
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Reaktion (nativ, vor applyTo): bricht Gegner-Attacken ab "
                           "+ Flinch/Stagger.");
        ch |= ImGui::Checkbox("Nativer Boost (set_Damage im PRE)##ftdmgnat",
                              &m_ftcfg.damage_native);
        ch |= ImGui::SliderFloat("Stopping (Attack-Abbruch)##ftdmgstop",
                                 &m_ftcfg.damage_stopping, 0.0f, 500.0f, "%.0f");
        ch |= ImGui::SliderFloat("Wince (Flinch)##ftdmgwince",
                                 &m_ftcfg.damage_wince, 0.0f, 500.0f, "%.0f");
        ch |= ImGui::SliderFloat("Break (Stagger)##ftdmgbreak",
                                 &m_ftcfg.damage_break, 0.0f, 500.0f, "%.0f");
        ch |= ImGui::Checkbox("HP-Fallback (POST direkt abziehen)##ftdmghpfb",
                              &m_ftcfg.damage_hp_fallback);

        if (ch) {
            ft_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Sicherung _05 (Right-B Swing)##ftsaf")) {
        ImGui::Checkbox("Vorschau: Sicherung offen zwingen##ftsafprev", &m_saf.preview);
        bool ch = false;
        ch |= ImGui::SliderFloat("Swing RotX (Grad)##ftsrx", &m_ftcfg.saf_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Swing RotY (Grad)##ftsry", &m_ftcfg.saf_ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Swing RotZ (Grad)##ftsrz", &m_ftcfg.saf_rz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Swing-Geschw. (lerp)##ftslerp", &m_ftcfg.saf_lerp, 0.01f, 0.30f);

        if (ch) {
            ft_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Tank _04 (Dock + Hand + Drop)##fttank")) {
        bool ch = false;

        if (ImGui::Button("Test: Tank droppen##ftdroptest")) {
            ft_start_drop();
        }

        ImGui::SameLine();

        if (ImGui::Button("Test: Tank zurueck (idle)##ftidletest")) {
            m_tank.phase = "idle";
        }

        ImGui::SameLine();

        if (ImGui::Button("Test: Refill 100##ftrefilltest")) {
            ft_refill();
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Dock (Einlege-Punkt, rel. Sicherung _05):");
        ch |= ImGui::SliderFloat("Dock X##ftdx", &m_ftcfg.dock_x, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Dock Y##ftdy", &m_ftcfg.dock_y, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Dock Z##ftdz", &m_ftcfg.dock_z, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Auto-Einrast-Distanz m##ftid",
                                 &m_ftcfg.insert_distance, 0.03f, 0.50f);
        ch |= ImGui::SliderFloat("Loslass-Fang-Distanz m (sonst faellt)##ftdc",
                                 &m_ftcfg.dock_catch, 0.05f, 0.80f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Tank in der Hand (am Holster) -- Offset:");
        ImGui::Checkbox("Vorschau: Kanister an die Hand zwingen##ftprevhand", &m_tank.preview);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (zum Offset-Tunen; Auto-Drop ist waehrend der Vorschau gesperrt)");
        ch |= ImGui::SliderFloat("Hand X##fthx", &m_ftcfg.hand_x, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Hand Y##fthy", &m_ftcfg.hand_y, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Hand Z##fthz", &m_ftcfg.hand_z, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("Hand RotX##fthrx", &m_ftcfg.hand_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotY##fthry", &m_ftcfg.hand_ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotZ##fthrz", &m_ftcfg.hand_rz, -180.0f, 180.0f);

        if (ch) {
            ft_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Hand-Posen (gebacken)##ftpose")) {
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "Rechte Hand: flamehand (immer)  |  Links: flamesupport "
                           "(Dock/2-Hand) / flamecanister (Tank)");
        bool ch = false;
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "flamehand-Offset (rechte Hand, additiv auf R_Palm):");
        ch |= ImGui::SliderFloat("flamehand RotX##ftfhrx", &m_ftcfg.fh_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("flamehand RotY##ftfhry", &m_ftcfg.fh_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("flamehand RotZ##ftfhrz", &m_ftcfg.fh_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "flamecanister Daumen-Tuning (L_Thumb1):");
        ImGui::Checkbox("Vorschau: flamecanister (links) zwingen##ftprevcan",
                        &m_ft_pose_prev.canister);
        ch |= ImGui::SliderFloat("Daumen RotX##ftcatrx", &m_ftcfg.ca_trx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##ftcatry", &m_ftcfg.ca_try, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##ftcatrz", &m_ftcfg.ca_trz, -90.0f, 90.0f);
        ImGui::Checkbox("Vorschau: flamesupport (links) zwingen##ftprevsup",
                        &m_ft_pose_prev.support);

        if (ch) {
            ft_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

// ============================================================================
// [DAMAGE-BOOST] Spieler/Partner-HitPoint cachen -- die Flammen-IDs treffen auch
// den Spieler (eigener Feuer-Splash / Gegner-Feuer), und dessen Schaden DUERFEN
// wir nie boosten.
// ============================================================================
void RE4VRReload3::ft_refresh_player_hp() {
    const double now = clock_now();

    if ((now - m_ft_pc_t) < 0.5 && !m_ft_pc_list.empty()) {
        return;
    }

    m_ft_pc_t = now;
    m_ft_pc_list.clear();

    auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");

    if (cm == nullptr) {
        return;
    }

    for (const char* getter : {"getPlayerContextRef", "getPartnerContextRef"}) {
        auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, getter);
        auto* hp = re4vr::call_safe<::REManagedObject*>(ctx, "get_HitPoint");

        if (hp != nullptr) {
            m_ft_pc_list.push_back(reinterpret_cast<uintptr_t>(hp));
        }
    }
}

bool RE4VRReload3::ft_is_player_hp(uintptr_t addr) {
    if (addr == 0) {
        return false;
    }

    ft_refresh_player_hp();

    return std::find(m_ft_pc_list.begin(), m_ft_pc_list.end(), addr)
           != m_ft_pc_list.end();
}

// Opfer-HitPoint aus einem (evtl. Koerperteil-)GameObject: den HitController
// suchen und dabei die Eltern-Hierarchie hochlaufen -- das DamageGameObject ist
// oft ein Collider-Child ohne HitController.
::REManagedObject* RE4VRReload3::ft_hp_from_go(::REManagedObject* go, int32_t& depth_out) {
    depth_out = -1;
    auto* rt = re4vr::runtime_type("chainsaw.HitController");

    if (rt == nullptr) {
        return nullptr;
    }

    auto* cur = go;

    for (int32_t depth = 0; depth <= 6 && cur != nullptr; ++depth) {
        auto* hcc = re4vr::call_safe<::REManagedObject*>(cur, "getComponent(System.Type)", rt);

        if (hcc != nullptr) {
            auto* ctx = re4vr::call_safe<::REManagedObject*>(hcc, "get_Context");
            auto* hp = re4vr::call_safe<::REManagedObject*>(ctx, "get_HitPoint");

            if (hp != nullptr) {
                depth_out = depth;

                return hp;
            }
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(cur, "get_Transform");
        auto* ptf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
        cur = re4vr::call_safe<::REManagedObject*>(ptf, "get_GameObject");
    }

    return nullptr;
}

// ============================================================================
// Init / Hooks / Lua-Zustand / Dispatch
// ============================================================================
RE4VRReload3* RE4VRReload3::s_instance{nullptr};

void RE4VRReload3::on_initialize() {
    s_instance = this;

    // ---- 1 CHICAGO: Hand-Posen (EIGENE Daten-Kopie, gestures-unabhaengig).
    // reload hatte fuer 4201 KEINE Mag-/Rack-Pose -> die Stingray-Greifposen
    // sind der Start.
    m_cposes["ChicagoMag"] = {
        {"L_IndexF1", glm::quat{0.993446f, 0.042820f, 0.010770f, -0.105429f}},
        {"L_IndexF2", glm::quat{0.868657f, 0.000000f, 0.000000f, -0.495415f}},
        {"L_IndexF3", glm::quat{0.955468f, 0.000000f, 0.000000f, -0.295095f}},
        {"L_MiddleF1", glm::quat{0.964366f, -0.034839f, 0.015712f, -0.261795f}},
        {"L_MiddleF2", glm::quat{0.812551f, 0.000000f, 0.000000f, -0.582890f}},
        {"L_MiddleF3", glm::quat{0.929199f, 0.000000f, 0.000000f, -0.369579f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_PinkyF1", glm::quat{0.886025f, -0.206433f, 0.097919f, -0.403432f}},
        {"L_PinkyF2", glm::quat{0.655536f, 0.000000f, 0.000000f, -0.755164f}},
        {"L_PinkyF3", glm::quat{0.800316f, 0.000000f, 0.000000f, -0.599579f}},
        {"L_RingF1", glm::quat{0.910646f, -0.152227f, 0.095372f, -0.372096f}},
        {"L_RingF2", glm::quat{0.820979f, 0.000000f, 0.000000f, -0.570959f}},
        {"L_RingF3", glm::quat{0.918430f, 0.000000f, 0.000000f, -0.395584f}},
        {"L_Thumb1", glm::quat{0.954216f, 0.092758f, -0.147124f, -0.243356f}},
        {"L_Thumb2", glm::quat{0.928704f, 0.047384f, -0.316781f, -0.186854f}},
        {"L_Thumb3", glm::quat{0.976686f, -0.002277f, 0.208568f, 0.050784f}},
    };

    m_cposes["ChicagoSlide"] = {
        {"L_IndexF1", glm::quat{0.962506f, 0.038342f, -0.001037f, -0.268536f}},
        {"L_IndexF2", glm::quat{0.847117f, 0.000000f, 0.000000f, -0.531406f}},
        {"L_IndexF3", glm::quat{0.952203f, 0.000000f, 0.000000f, -0.305466f}},
        {"L_MiddleF1", glm::quat{0.938745f, -0.015258f, -0.024061f, -0.343433f}},
        {"L_MiddleF2", glm::quat{0.764522f, 0.000000f, 0.000000f, -0.644598f}},
        {"L_MiddleF3", glm::quat{0.964690f, 0.000000f, 0.000000f, -0.263388f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_PinkyF1", glm::quat{0.933727f, -0.083950f, 0.015837f, -0.347642f}},
        {"L_PinkyF2", glm::quat{0.881599f, 0.000000f, 0.000000f, -0.471999f}},
        {"L_PinkyF3", glm::quat{0.919535f, 0.000000f, 0.000000f, -0.393009f}},
        {"L_RingF1", glm::quat{0.938012f, -0.058541f, 0.018805f, -0.341106f}},
        {"L_RingF2", glm::quat{0.813982f, 0.000000f, 0.000000f, -0.580890f}},
        {"L_RingF3", glm::quat{0.962837f, 0.000000f, 0.000000f, -0.270083f}},
        {"L_Thumb1", glm::quat{0.964133f, 0.166059f, -0.014706f, -0.206532f}},
        {"L_Thumb2", glm::quat{0.992006f, 0.012628f, -0.122927f, 0.025579f}},
        {"L_Thumb3", glm::quat{0.982769f, -0.006119f, 0.182809f, -0.026611f}},
    };

    // ---- 3 ROCKET LAUNCHER: "WarheadHold" = 1:1-Kopie der LE5MAG-Pose ---
    m_rl_warhead_pose = {
        {"L_IndexF1", glm::quat{0.894645f, 0.048644f, -0.003804f, -0.444105f}},
        {"L_IndexF2", glm::quat{0.807797f, 0.0f, 0.0f, -0.589461f}},
        {"L_IndexF3", glm::quat{0.939637f, 0.0f, 0.0f, -0.342172f}},
        {"L_MiddleF1", glm::quat{0.918571f, -0.004702f, -0.034397f, -0.393728f}},
        {"L_MiddleF2", glm::quat{0.676934f, 0.0f, 0.0f, -0.736044f}},
        {"L_MiddleF3", glm::quat{0.957338f, 0.0f, 0.0f, -0.288970f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.966747f, -0.031850f, 0.005328f, -0.253687f}},
        {"L_PinkyF2", glm::quat{0.700924f, 0.0f, 0.0f, -0.713236f}},
        {"L_PinkyF3", glm::quat{0.923357f, 0.0f, 0.0f, -0.383943f}},
        {"L_RingF1", glm::quat{0.921790f, -0.022723f, -0.003569f, -0.387007f}},
        {"L_RingF2", glm::quat{0.710747f, 0.0f, 0.0f, -0.703447f}},
        {"L_RingF3", glm::quat{0.933578f, 0.0f, 0.0f, -0.358375f}},
        {"L_Thumb1", glm::quat{0.918569f, 0.252866f, -0.108586f, -0.283724f}},
        {"L_Thumb2", glm::quat{0.999635f, 0.0f, -0.027026f, 0.0f}},
        {"L_Thumb3", glm::quat{0.923391f, -0.000011f, 0.383861f, -0.000004f}},
    };
    // ---- 4 FLAMETHROWER: flamehand (rechts) / flamesupport + flamecanister
    m_pose_flamehand = {
        {"R_IndexF1", glm::quat{0.994111f, 0.000000f, 0.108364f, 0.000000f}},
        {"R_IndexF2", glm::quat{0.927257f, 0.000000f, 0.000000f, 0.374425f}},
        {"R_IndexF3", glm::quat{0.955189f, 0.000000f, 0.000000f, 0.295997f}},
        {"R_MiddleF1", glm::quat{0.951202f, 0.026301f, 0.090222f, 0.293909f}},
        {"R_MiddleF2", glm::quat{0.766392f, 0.000000f, 0.000000f, 0.642373f}},
        {"R_MiddleF3", glm::quat{0.913677f, 0.000000f, 0.000000f, 0.406440f}},
        {"R_Palm", glm::quat{0.997250f, 0.000000f, 0.000000f, -0.074108f}},
        {"R_PinkyF1", glm::quat{0.957255f, -0.079671f, -0.035414f, 0.275792f}},
        {"R_PinkyF2", glm::quat{0.853292f, 0.000000f, 0.000000f, 0.521434f}},
        {"R_PinkyF3", glm::quat{0.946328f, 0.000000f, 0.000000f, 0.323209f}},
        {"R_RingF1", glm::quat{0.928187f, -0.025978f, -0.012711f, 0.370988f}},
        {"R_RingF2", glm::quat{0.888385f, 0.000000f, 0.000000f, 0.459099f}},
        {"R_RingF3", glm::quat{0.853651f, 0.000000f, 0.000000f, 0.520846f}},
        {"R_Thumb1", glm::quat{0.861574f, 0.463423f, 0.082424f, 0.190092f}},
        {"R_Thumb2", glm::quat{0.999996f, 0.000000f, -0.002644f, 0.000000f}},
        {"R_Thumb3", glm::quat{0.863236f, 0.000000f, -0.504801f, 0.000000f}},
    };

    m_pose_flamesupport = {
        {"L_IndexF1", glm::quat{0.993561f, -0.083543f, -0.065129f, -0.040180f}},
        {"L_IndexF2", glm::quat{0.954616f, 0.000000f, 0.000000f, -0.297838f}},
        {"L_IndexF3", glm::quat{0.950483f, 0.000000f, 0.000000f, -0.310778f}},
        {"L_MiddleF1", glm::quat{0.977877f, -0.109369f, -0.088266f, -0.154935f}},
        {"L_MiddleF2", glm::quat{0.850457f, 0.000000f, 0.000000f, -0.526044f}},
        {"L_MiddleF3", glm::quat{0.980999f, 0.000000f, 0.000000f, -0.194012f}},
        {"L_Palm", glm::quat{1.000000f, -0.000000f, -0.000000f, -0.000000f}},
        {"L_PinkyF1", glm::quat{0.920692f, -0.191583f, -0.107047f, -0.322742f}},
        {"L_PinkyF2", glm::quat{0.941538f, 0.000000f, 0.000000f, -0.336906f}},
        {"L_PinkyF3", glm::quat{0.973633f, 0.000000f, 0.000000f, -0.228118f}},
        {"L_RingF1", glm::quat{0.952338f, -0.170963f, -0.048996f, -0.247838f}},
        {"L_RingF2", glm::quat{0.876829f, 0.000000f, 0.000000f, -0.480803f}},
        {"L_RingF3", glm::quat{0.981566f, 0.000000f, 0.000000f, -0.191126f}},
        {"L_Thumb1", glm::quat{0.966148f, 0.257212f, 0.011939f, 0.016055f}},
        {"L_Thumb2", glm::quat{0.941147f, -0.083984f, -0.323802f, 0.048387f}},
        {"L_Thumb3", glm::quat{0.998527f, 0.000000f, 0.054267f, 0.000000f}},
    };

    m_pose_flamecanister = {
        {"L_IndexF1", glm::quat{0.999912f, 0.002479f, -0.012574f, 0.003377f}},
        {"L_IndexF2", glm::quat{0.992097f, 0.000000f, 0.000000f, -0.125471f}},
        {"L_IndexF3", glm::quat{0.991860f, 0.000000f, 0.000000f, -0.127334f}},
        {"L_MiddleF1", glm::quat{0.999422f, 0.001461f, 0.011326f, -0.032021f}},
        {"L_MiddleF2", glm::quat{0.941170f, 0.000000f, 0.000000f, -0.337933f}},
        {"L_MiddleF3", glm::quat{0.965377f, 0.000000f, 0.000000f, -0.260859f}},
        {"L_Palm", glm::quat{1.000000f, -0.000000f, -0.000000f, -0.000000f}},
        {"L_PinkyF1", glm::quat{0.999196f, -0.000822f, 0.026396f, -0.030168f}},
        {"L_PinkyF2", glm::quat{0.925399f, 0.000000f, 0.000000f, -0.378994f}},
        {"L_PinkyF3", glm::quat{0.970562f, 0.000000f, 0.000000f, -0.240852f}},
        {"L_RingF1", glm::quat{0.999773f, -0.001175f, 0.018148f, -0.011068f}},
        {"L_RingF2", glm::quat{0.928340f, 0.000000f, 0.000000f, -0.371733f}},
        {"L_RingF3", glm::quat{0.950664f, 0.000000f, 0.000000f, -0.310224f}},
        {"L_Thumb1", glm::quat{0.990954f, 0.114399f, 0.006076f, -0.069907f}},
        {"L_Thumb2", glm::quat{0.982677f, 0.007076f, -0.185176f, 0.002216f}},
        {"L_Thumb3", glm::quat{0.990361f, 0.000000f, 0.138508f, 0.000000f}},
    };

    // ---- JSON zuletzt: sie ueberschreibt die Code-Defaults ------------
    chicago_load_cfg();
    hc_load_cfg();
    rl_load_cfg();
    ft_load_cfg();
}

// ============================================================================
// Hooks. In Lua haengen sie hinter Global-Guards (__re4_ft_dmg_hook_installed),
// weil ein sdk.hook einen GAME-Neustart braucht; nativ reicht der eine Aufruf
// beim ersten Initialisieren.
// ============================================================================
void RE4VRReload3::install_hooks() {
    // [FLAMETHROWER DAMAGE-BOOST] Der echte Schaden steckt in
    // HitController.DamageValue.get_Damage (Int32), NICHT in WwiseDamage (= 0,
    // nur Audio). set_Damage im POST wirkte NICHT (DamageValue wird intern via
    // applyTo schon angewendet) -> wir boosten im PRE, VOR applyTo.
    auto* td = sdk::find_type_definition("chainsaw.HitController");
    auto* m = (td != nullptr) ? td->get_method("callbackCalculateDamage") : nullptr;

    if (m == nullptr) {
        return;
    }

    g_hookman.add(
        m,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
           uintptr_t) {
            auto* s = RE4VRReload3::instance();

            if (s == nullptr) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            s->m_ft_hit_dv = nullptr;
            s->m_ft_hit_hp = nullptr;

            const auto& g = s->m_ftcfg;

            // Die Argumente scannen: HitController / DamageValue / CalculateInfo
            ::REManagedObject* hc = nullptr;
            ::REManagedObject* dv = nullptr;
            ::REManagedObject* ci = nullptr;

            for (size_t idx = 1; idx <= 5 && idx < args.size(); ++idx) {
                auto* o = reinterpret_cast<::REManagedObject*>(args[idx]);

                if (!re4vr::obj_ok(o)) {
                    continue;
                }

                auto* otd = utility::re_managed_object::get_type_definition(o);

                if (otd == nullptr) {
                    continue;
                }

                const auto nm = otd->get_full_name();

                if (nm == "chainsaw.HitController") {
                    hc = o;
                } else if (nm == "chainsaw.HitController.DamageValue") {
                    dv = o;
                } else if (nm == "chainsaw.HitController.CalculateInfo") {
                    ci = o;
                }
            }

            if (hc == nullptr) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            const auto wid = call_enum(hc, "get_WeaponID");

            if (!wid.has_value() || !is_flame_id(*wid)) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            // ===== Das Opfer ZUERST bestimmen (vor jedem Boost!) =====
            // SCHUTZ: Schaden gegen Spieler/Partner -> NIE boosten. NUR ueber die
            // HitPoint-Adresse pruefen: get_IsPlayerDamage ist NICHT "Opfer =
            // Spieler", sondern "Schaden VOM Spieler ausgeteilt" -- beim
            // Flammenwerfer gegen Gegner also immer true.
            ::REManagedObject* hp = nullptr;

            if (ci != nullptr) {
                auto* vgo = re4vr::call_safe<::REManagedObject*>(ci, "get_DamageGameObject");

                if (vgo != nullptr) {
                    int32_t depth = -1;
                    hp = ft_hp_from_go(vgo, depth);
                }
            }

            const bool victim_is_player =
                (hp != nullptr) && s->ft_is_player_hp(reinterpret_cast<uintptr_t>(hp));

            // ===== NATIVER BOOST (vor applyTo!) am DamageValue =====
            // applyTo laeuft gleich danach und wendet die Werte nativ an ->
            // echter Schaden + Attack-Abbruch (Stopping) + Flinch.
            if (dv != nullptr && g.damage_enabled && g.damage_native && !victim_is_player) {
                const int32_t obase = call_enum(dv, "get_Damage").value_or(0);
                const auto get_f = [&](const char* n) -> float {
                    float v = 0.0f;

                    return re4vr::try_call<float>(dv, n, v) ? v : 0.0f;
                };

                const float ostop = get_f("get_Stopping");
                const float owince = get_f("get_Wince");
                const float obrk = get_f("get_Break");

                int32_t nd = static_cast<int32_t>(
                    std::floor(static_cast<float>(obase) * g.damage_mult + 0.5f));

                if (g.damage_floor > static_cast<float>(nd)) {
                    nd = static_cast<int32_t>(g.damage_floor);
                }

                if (nd > obase) {
                    re4vr::call_safe<void*>(dv, "set_Damage", nd);
                }

                // Reaktion: die nativen Originalwerte NICHT verkleinern (max),
                // sonst flammt es schwaecher.
                if (g.damage_stopping > ostop) {
                    re4vr::call_safe<void*>(dv, "set_Stopping", g.damage_stopping);
                }

                if (g.damage_wince > owince) {
                    re4vr::call_safe<void*>(dv, "set_Wince", g.damage_wince);
                }

                if (g.damage_break > obrk) {
                    re4vr::call_safe<void*>(dv, "set_Break", g.damage_break);
                }
            }

            s->m_ft_hit_dv = dv;
            s->m_ft_hit_hp = hp;

            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
            auto* s = RE4VRReload3::instance();

            if (s == nullptr) {
                return;
            }

            auto* dv = s->m_ft_hit_dv;
            auto* hp = s->m_ft_hit_hp;
            s->m_ft_hit_dv = nullptr;
            s->m_ft_hit_hp = nullptr;

            const auto& g = s->m_ftcfg;

            // Der HP-Direktabzug ist NUR noch ein Fallback -- im Standard traegt
            // der native Boost im PRE den Schaden.
            if (!(g.damage_enabled && g.damage_hp_fallback) || hp == nullptr) {
                return;
            }

            if (s->ft_is_player_hp(reinterpret_cast<uintptr_t>(hp))) {
                return;
            }

            for (const char* q : {"get_Invincible", "get_Immortal", "get_NoDamage",
                                  "get_IsDead"}) {
                bool v = false;

                if (re4vr::try_call<bool>(hp, q, v) && v) {
                    return;
                }
            }

            const int32_t base = (dv != nullptr)
                ? call_enum(dv, "get_Damage").value_or(0) : 0;
            // extra = max(base*(mult-1), floor): mult skaliert grosse Bursts,
            // floor garantiert spuerbaren Schaden pro Tick (die Basis ist oft 1).
            int32_t extra = static_cast<int32_t>(
                std::floor(static_cast<float>(base) * (g.damage_mult - 1.0f) + 0.5f));

            if (g.damage_floor > static_cast<float>(extra)) {
                extra = static_cast<int32_t>(g.damage_floor);
            }

            if (extra <= 0) {
                return;
            }

            const int32_t cur = call_enum(hp, "get_CurrentHitPoint").value_or(0);
            const int32_t newhp = std::max(0, cur - extra);
            re4vr::call_safe<void*>(hp, "set_CurrentHitPoint", newhp);

            if (newhp <= 0) {
                re4vr::call_safe<void*>(hp, "dead");
            }
        });
}

void RE4VRReload3::on_lua_state_created() {
    // reload3 exportiert in Lua keine Funktions-Globals -- alles, was fremde
    // Scripte lesen, sind Wert-Globals, und die setzen die Frame-Passes.
}

void RE4VRReload3::on_lua_state_destroyed() {
    // Reset Scripts wiped nur Lua, NICHT die Szene -> die Joints blieben in ihrer
    // letzten Override-Lage stehen. Deshalb HIER zuruecksetzen -- 1:1 die vier
    // re.on_script_reset-Bloecke.
    chicago_on_script_reset();
    hc_on_script_reset();
    rl_on_script_reset();
    ft_on_script_reset();

    m_pmap.clear();
    m_pmap_tf = nullptr;
    m_character_manager = nullptr;
    m_pe_cache = nullptr;
    m_ft_pc_t = -999.0;
    m_ft_pc_list.clear();
}

// ============================================================================
// Holster-Kette. In Lua wickelt jeder der vier Bloecke seine eigene Fassung um
// __re4_reload_set_mag_in_hand; der ZULETZT geladene Wrapper wird zuerst
// gefragt. Reihenfolge in der Datei: Chicago, Handcannon, RL, FT -- also wird
// hier von hinten nach vorn geprueft (FT -> RL -> HC -> Chicago).
// std::nullopt = keine unserer Waffen -> der Aufrufer reicht an reload2 weiter.
// ============================================================================
std::optional<bool> RE4VRReload3::set_mag_in_hand(bool active) {
    const auto wid = get_equip_wid();

    if (!wid.has_value()) {
        return std::nullopt;
    }

    if (is_ft(*wid)) {
        return ft_set_mag_in_hand(active);
    }

    if (is_rl(*wid)) {
        return rl_set_mag_in_hand(active);
    }

    if (is_handcannon(*wid)) {
        return hc_set_mag_in_hand(active);
    }

    if (is_chicago(*wid)) {
        return chicago_set_mag_in_hand(active);
    }

    return std::nullopt;
}

// ============================================================================
// Dispatch. Die Reihenfolge ist die Registrierungsreihenfolge in Lua:
// Chicago -> Handcannon -> RL -> Flamethrower.
// ============================================================================
// [SAVE_LOAD-RESET 19.09.2026] Sonde re4_saveload_sonde: die Body-Adresse
// springt NUR bei Save-Load/Tod (0,77 s ohne Body davor), nie im Spiel. Die
// alte Waffe bleibt danach oft noch lesbar -> der tf-Test im Refresh sah keinen
// Grund zum Neuholen. tf wegwerfen zwingt den vorhandenen [SAVE_LOAD]-Zweig.
void RE4VRReload3::tick_saveload_reset() {
    auto* body = re4vr::fc::body_go();

    if (body == nullptr) {
        return;
    }

    const auto a = reinterpret_cast<uintptr_t>(body);

    if (!m_sl_body.has_value()) {
        m_sl_body = a;

        return;
    }

    if (a == *m_sl_body) {
        return;
    }

    m_sl_body = a;
    m_pe_cache = nullptr;
    m_cwep.tf = nullptr;
}

void RE4VRReload3::on_frame() {
    tick_saveload_reset();
    chicago_on_frame();
    hc_on_frame();
    rl_on_frame();
    ft_on_frame();
}

// Die Render-Paesse. Jede Maschine haengt in Lua an ihrem eigenen Satz:
//   Chicago / Handcannon / RL / FT: LockScene(pre), LateUpdateBehavior,
//       UpdateJointExpression, BeginRendering(pre)
//   Handcannon + RL + FT: dazu BeginRendering(POST) fuer den Wobble-Fix
void RE4VRReload3::on_lock_scene_pre() {
    chicago_apply_pass();
    hc_apply_pass();
    rl_apply_pass();
    wh_follow();
    ft_apply_pass();
}

void RE4VRReload3::on_late_update() {
    chicago_apply_pass();
    hc_apply_pass();
    rl_apply_pass();
    wh_follow();
    ft_apply_pass();
}

void RE4VRReload3::on_update_joint_expression() {
    chicago_apply_pass();
    hc_apply_pass();
    rl_apply_pass();
    wh_follow();
    ft_apply_pass();

    // [VERSATZ BEIM LAUFEN 2026-09-07] Dieselbe Sache wie in RE4VRReload2:
    // die *_late-Funktionen fahren gespawnte Mesh-Part-Klone entlang ihrer
    // Bahn und standen bisher NUR im BeginRendering(POST). Danach bewegt die
    // Engine Hand und Waffe noch zweimal (LateUpdateBehavior,
    // UpdateJointExpression) -- der Klon bleibt stehen und sitzt beim Laufen
    // um eine konstante Frame-Strecke versetzt. Hier im LETZTEN Pass
    // nachziehen; Waffen-Joints brauchen das nicht, die propagiert die Engine.
    hc_reposition_cart_late();
    wh_reposition_late();
    ft_late();
}

void RE4VRReload3::on_begin_rendering_pre() {
    chicago_apply_pass();
    hc_apply_pass();
    rl_apply_pass();
    wh_follow();
    ft_apply_pass();
}

// [WOBBLE-FIX] NACH motions BeginRendering-POST (attach_left_hand).
void RE4VRReload3::on_begin_rendering() {
    hc_reposition_cart_late();
    wh_reposition_late();
    ft_late();
}

// ============================================================================
// UI-Wurzel. In Lua EIN on_draw_ui mit dem Header "RE4VR - Reload3", darin die
// vier Gattungen gestapelt.
// ============================================================================
void RE4VRReload3::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Reload3")) {
        return;
    }

    chicago_ui();

    ImGui::Separator();
    hc_ui();

    ImGui::Separator();
    rl_ui();

    ImGui::Separator();
    ft_ui();
    ImGui::TreePop();
}

#endif
