// =====================================================================
// RE4VRFirstPerson -- 1:1-Portierung von re4_vr_firstperson.lua.
// Siehe RE4VRFirstPerson.hpp und I:\LUATRANS\PORT_FIRSTPERSON_SPEC.md.
//
// Grundregel: KEINE Vereinfachung. Auch die Eigenheiten des Originals sind
// uebernommen und mit [WIE_LUA] vermerkt.
// =====================================================================
#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/REArray.hpp>
#include <utility/String.hpp>

// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRFirstPerson.hpp"
// [FP-MESSUNG 04.09.] nur fuer die Diagnose unten -- raus, sobald der
// 3rd-Person-Befund geklaert ist.
#include "RE4VRKillswitch.hpp"

#include <fstream>

#undef min
#undef max

namespace {

constexpr const char* CFG_PATH = "re4_vr/re4_vr_firstperson.json"; // Lua Z.21

constexpr float CAPY_TAU = 0.12f;            // Lua Z.547
constexpr double CROUCH_CAM_WINDOW = 0.8;    // Lua Z.733
constexpr double STANDUP_WINDOW = 0.5;       // Lua Z.734
constexpr double SD_SCAN_INTERVAL = 0.5;     // Lua Z.1565

double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// =====================================================================
// Geschuetzte Managed-Zugriffe mit Erfolgs-Flag und 16-Byte-Puffern --
// Bauform wie in RE4VRArmChain.cpp (dort steht die ausfuehrliche
// Begruendung: via.vec3/via.quat sind fuer die Engine 16 Byte, und
// re4vr::call_safe verschluckt den Unterschied zwischen "0" und "ging nicht").
// =====================================================================
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
    // [M3] Luas build_args packt Vector3f als (x, y, z, 0.0f).
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, 0.0f};
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// Luas Vector3f:to_quat() -- ScriptRunner.cpp:210 baut
// glm::quat(rowMajor4(lookAtLH({0,0,0}, v, {0,1,0}))): eine ROLLFREIE
// Orientierung mit Up gegen +Y, NICHT die Minimalbogen-Drehung von +Z.
// Der Unterschied schlaegt bei exakt (0,0,-1) durch: glm gibt dort NaN.
glm::quat lua_to_quat(const glm::vec3& dir) {
    const auto mat = glm::rowMajor4(
        glm::lookAtLH(glm::vec3{0.0f, 0.0f, 0.0f}, dir, glm::vec3{0.0f, 1.0f, 0.0f}));

    return glm::quat{mat};
}

// [K3] via.vec2 ist 8 Byte -- MSVC-x64 gibt das in RAX zurueck, es gibt
// KEINEN versteckten Out-Zeiger. Ein sret-Aufruf haette den Puffer nach RCX
// gelegt, wo die Engine den VMContext erwartet.
// Lua ist davon nicht betroffen: REMethodDefinition::invoke nimmt fuer jeden
// nicht-primitiven ValueType einen Out-Puffer, unabhaengig von der Groesse.
// Deshalb hier der ABI-freie Weg ueber das FELD -- so macht es der Fork an
// seinen eigenen Stellen auch (VR.cpp:3813, FreeCam.cpp:225).
bool get_vec2_field(::REManagedObject* obj, const char* field, glm::vec2& out) {
    if (obj == nullptr) {
        return false;
    }

    // Derselbe Weg, den der Fork an seinen eigenen Stellen nimmt
    // (VR.cpp:3813, FreeCam.cpp:225): das Feld liefert einen Zeiger IN das
    // Objekt, keine Rueckgabe ueber die Aufrufkonvention.
    auto* p = utility::re_managed_object::get_field<Vector3f*>(obj, field);

    if (p == nullptr) {
        return false;
    }

    out = glm::vec2{p->x, p->y};
    return true;
}

// Lua Z.330: btf:call("getJointByName", "Head") -- Managed-Weg, nicht der
// native Array-Scan, damit der Mechanismus derselbe bleibt.
::REJoint* get_joint_by_name(::REManagedObject* transform, const wchar_t* name) {
    if (transform == nullptr) {
        return nullptr;
    }

    auto str = sdk::VM::create_managed_string(name);

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REJoint*>(transform, "getJointByName", str);
}

// Lua: ctf:get_position() / ctf:set_position(v, true) -- das sind die
// RETransform-Usertypes, die auf sdk::get/set_transform_position abbilden
// (Sdk.cpp Z.1871/1873). Die benutzen intern den UNGESCHUETZTEN call<>-Pfad
// und lassen eine Engine-Exception im VMContext stehen -> danach aufraeumen.
bool transform_get_position(::REManagedObject* tf, glm::vec4& out) {
    if (tf == nullptr) {
        return false;
    }

    bool ok = false;

    try {
        out = sdk::get_transform_position(reinterpret_cast<::RETransform*>(tf));
        ok = true;
    } catch (...) {
        ok = false;
    }

    re4vr::clear_vm_exception();
    return ok;
}

void transform_set_position(::REManagedObject* tf, const glm::vec4& v) {
    if (tf == nullptr) {
        return;
    }

    try {
        sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf), v, true);
    } catch (...) {
    }

    re4vr::clear_vm_exception();
}

// Lua Z.401: busy:get_field("_CameraRotation").
bool field_get_quat(::REManagedObject* obj, const char* name, glm::quat& out) {
    if (obj == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    const auto f = def->get_field(name);

    if (f == nullptr) {
        return false;
    }

    try {
        out = f->get_data<glm::quat>(obj);
        return true;
    } catch (...) {
        return false;
    }
}

bool type_is_a(::REManagedObject* obj, sdk::RETypeDefinition* other) {
    if (obj == nullptr || other == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    try {
        return def->is_a(other);
    } catch (...) {
        return false;
    }
}

// Lua Z.1235-1245: Yaw aus einem Quaternion und zurueck.
std::optional<float> yaw_of_quat(const glm::quat& q) {
    const glm::vec3 f = q * glm::vec3{0.0f, 0.0f, 1.0f};
    const float len = std::sqrt(f.x * f.x + f.z * f.z);

    if (len < 1e-4f) {
        return std::nullopt;
    }

    return std::atan2(f.x / len, f.z / len);
}

glm::quat yaw_quat(float y) {
    const float h = y * 0.5f;
    return glm::quat{std::cos(h), 0.0f, std::sin(h), 0.0f};
}

// Lua Z.403-408 / 521-526: flacher Yaw aus einer Rotation.
std::optional<glm::quat> flat_yaw_from_rot(const glm::quat& rot) {
    glm::vec3 fwd = rot * glm::vec3{0.0f, 0.0f, 1.0f};
    fwd.y = 0.0f;

    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);

    if (len < 0.0001f) {
        return std::nullopt;
    }

    // [K1] Luas Vector3f:to_quat() -- ScriptRunner.cpp:210, zeichengenau.
    // NICHT der glm-Zwei-Vektor-Konstruktor: der liefert bei exakt (0,0,-1)
    // normalize(quat(0,0,0,0)) = NaN, lookAtLH dagegen eine gueltige
    // 180-Grad-Drehung. Ein NaN ginge hier direkt in vr_camera_fix.camera_rot
    // und von dort nach motion und scope.
    const glm::vec3 n{fwd.x / len, 0.0f, fwd.z / len};
    return lua_to_quat(n);
}

float json_number(const nlohmann::json& j, const char* key, float def) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }

    const auto& v = j[key];
    return v.is_number() ? v.get<float>() : def;
}

bool json_present(const nlohmann::json& j, const char* key) {
    return j.is_object() && j.contains(key) && !j[key].is_null();
}

// Lua `d.X == true`: nur der echte Boolesche Wahrwert.
bool json_eq_true(const nlohmann::json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

ImVec4 abgr_to_vec4(uint32_t c) {
    return ImVec4{static_cast<float>(c & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 24) & 0xFF) / 255.0f};
}

} // namespace

// =====================================================================
// Lebenszyklus
// =====================================================================
std::shared_ptr<RE4VRFirstPerson>& RE4VRFirstPerson::get() {
    static auto inst = std::make_shared<RE4VRFirstPerson>();
    return inst;
}

void RE4VRFirstPerson::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRFirstPerson", "on_lua_state_destroyed");
    on_script_reset();
}

void RE4VRFirstPerson::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRFirstPerson", "on_pre_application_entry");
    if (hash == "LockScene"_fnv) {
        on_pre_lock_scene();
    } else if (hash == "UnlockScene"_fnv) {
        on_pre_unlock_scene();
    }
}

void RE4VRFirstPerson::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRFirstPerson", "on_application_entry");
    if (hash == "LateUpdateBehavior"_fnv) {
        on_late_update_behavior();
    } else if (hash == "BeginRendering"_fnv) {
        on_begin_rendering();
    } else if (hash == "UpdateMotion"_fnv) {
        on_update_motion();
    }
}

std::optional<std::string> RE4VRFirstPerson::on_initialize() {
    load_cfg();
    publish_bino(); // Lua Z.213: sofort, auch ohne UI
    return std::nullopt;
}

void RE4VRFirstPerson::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRFirstPerson", "on_lua_state_created");
    // Lua Z.213: __re4_bino_cfg muss nach jedem Reset wieder stehen, sonst
    // faellt re4_vr_binding.lua auf seine eigenen Defaults zurueck.
    publish_bino();
}

void RE4VRFirstPerson::on_script_reset() {
    // Lua Z.1876-1900 -- plus das, was in Lua ein Datei-Neuladen erledigt hat.
    reset_runtime_state();
    load_cfg();
    publish_bino();
}

void RE4VRFirstPerson::reset_runtime_state() {
    m_bob = BobFilter{};
    m_capy = Capy{};
    m_surge = Surge{};
    m_crouch_cam = CrouchCam{};
    m_move = MoveState{};

    store_head_joint(nullptr);

    // [WIE_LUA] on_script_reset setzt NUR den lokalen Singleton-Cache auf nil
    // (Lua Z.1894). Der globale, den fp_busy_cached benutzt, ueberlebt --
    // siehe [QUIRK 1] in der Spezifikation.
    m_camera_system = nullptr;

    // Lua Z.1895: `if _G.vr_camera_fix then _G.vr_camera_fix.active = false end`.
    // NUR `.active` -- camera_pos und camera_rot bleiben ausdruecklich stehen.
    re4vr::lua_ensure_table("vr_camera_fix");
    re4vr::lua_set_table_bool("vr_camera_fix", "active", false);

    sd_clear_cache();
    m_sd_last_scan = 0.0;

    m_rc_was_active = false;
    re4vr::lua_set_bool("__vr_recenter_hold", false);

    // NICHT zurueckgesetzt (wie im Original): m_headpin_fade, m_fp_frame,
    // m_fp_stage/_busy, m_ftw*, m_camera_system_global.
}

// =====================================================================
// Konfiguration
// =====================================================================
void RE4VRFirstPerson::publish_bino() {
    // Lua Z.136-139.
    re4vr::lua_ensure_table("__re4_bino_cfg");
    re4vr::lua_set_table_number("__re4_bino_cfg", "start", m_cfg.bino_start);
    re4vr::lua_set_table_number("__re4_bino_cfg", "min", m_cfg.bino_min);
    re4vr::lua_set_table_number("__re4_bino_cfg", "max", m_cfg.bino_max);
    re4vr::lua_set_table_number("__re4_bino_cfg", "speed", m_cfg.bino_speed);
}

void RE4VRFirstPerson::load_cfg() {
    // Lua Z.141-212. Ein Feld pro Zeile, mit exakt derselben Typpruefung.
    const auto d = re4vr::json_load(CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    // [WIE_LUA] off_x/y/z werden OHNE Typtest uebernommen (Lua Z.144-146).
    if (json_present(d, "off_x")) { m_cfg.off_x = json_number(d, "off_x", m_cfg.off_x); }
    if (json_present(d, "off_y")) { m_cfg.off_y = json_number(d, "off_y", m_cfg.off_y); }
    if (json_present(d, "off_z")) { m_cfg.off_z = json_number(d, "off_z", m_cfg.off_z); }

    m_cfg.cart_off_x = json_number(d, "cart_off_x", m_cfg.cart_off_x);
    m_cfg.cart_off_y = json_number(d, "cart_off_y", m_cfg.cart_off_y);
    m_cfg.cart_off_z = json_number(d, "cart_off_z", m_cfg.cart_off_z);

    m_cfg.headpin_fade_dur = json_number(d, "headpin_fade_dur", m_cfg.headpin_fade_dur);

    // Lua Z.151: `d.X ~= nil` -> `d.X and true or false` (truthy, nicht == true).
    if (json_present(d, "block_force_twirler")) {
        const auto& v = d["block_force_twirler"];
        m_cfg.block_force_twirler = v.is_boolean() ? v.get<bool>() : true;
    }

    m_cfg.event_off_x = json_number(d, "event_off_x", m_cfg.event_off_x);
    m_cfg.event_off_y = json_number(d, "event_off_y", m_cfg.event_off_y);
    m_cfg.event_off_z = json_number(d, "event_off_z", m_cfg.event_off_z);
    m_cfg.event2_off_x = json_number(d, "event2_off_x", m_cfg.event2_off_x);
    m_cfg.event2_off_y = json_number(d, "event2_off_y", m_cfg.event2_off_y);
    m_cfg.event2_off_z = json_number(d, "event2_off_z", m_cfg.event2_off_z);
    m_cfg.event3_off_x = json_number(d, "event3_off_x", m_cfg.event3_off_x);
    m_cfg.event3_off_y = json_number(d, "event3_off_y", m_cfg.event3_off_y);
    m_cfg.event3_off_z = json_number(d, "event3_off_z", m_cfg.event3_off_z);
    m_cfg.event4_off_x = json_number(d, "event4_off_x", m_cfg.event4_off_x);
    m_cfg.event4_off_y = json_number(d, "event4_off_y", m_cfg.event4_off_y);
    m_cfg.event4_off_z = json_number(d, "event4_off_z", m_cfg.event4_off_z);

    // Lua Z.164: hier ausdruecklich type() == "boolean".
    if (d.contains("event5_mono") && d["event5_mono"].is_boolean()) {
        m_cfg.event5_mono = d["event5_mono"].get<bool>();
    }

    m_cfg.event5_off_x = json_number(d, "event5_off_x", m_cfg.event5_off_x);
    m_cfg.event5_off_y = json_number(d, "event5_off_y", m_cfg.event5_off_y);
    m_cfg.event5_off_z = json_number(d, "event5_off_z", m_cfg.event5_off_z);
    m_cfg.event6_off_x = json_number(d, "event6_off_x", m_cfg.event6_off_x);
    m_cfg.event6_off_y = json_number(d, "event6_off_y", m_cfg.event6_off_y);
    m_cfg.event6_off_z = json_number(d, "event6_off_z", m_cfg.event6_off_z);
    m_cfg.turret_off_x = json_number(d, "turret_off_x", m_cfg.turret_off_x);
    m_cfg.turret_off_y = json_number(d, "turret_off_y", m_cfg.turret_off_y);
    m_cfg.turret_off_z = json_number(d, "turret_off_z", m_cfg.turret_off_z);
    m_cfg.jetski_off_x = json_number(d, "jetski_off_x", m_cfg.jetski_off_x);
    m_cfg.jetski_off_y = json_number(d, "jetski_off_y", m_cfg.jetski_off_y);
    m_cfg.jetski_off_z = json_number(d, "jetski_off_z", m_cfg.jetski_off_z);
    m_cfg.boat_off_x = json_number(d, "boat_off_x", m_cfg.boat_off_x);
    m_cfg.boat_off_y = json_number(d, "boat_off_y", m_cfg.boat_off_y);
    m_cfg.boat_off_z = json_number(d, "boat_off_z", m_cfg.boat_off_z);
    m_cfg.begcrouch_off_x = json_number(d, "begcrouch_off_x", m_cfg.begcrouch_off_x);
    m_cfg.begcrouch_off_y = json_number(d, "begcrouch_off_y", m_cfg.begcrouch_off_y);
    m_cfg.begcrouch_off_z = json_number(d, "begcrouch_off_z", m_cfg.begcrouch_off_z);
    m_cfg.ada_fc_off_x = json_number(d, "ada_fc_off_x", m_cfg.ada_fc_off_x);
    m_cfg.ada_fc_off_y = json_number(d, "ada_fc_off_y", m_cfg.ada_fc_off_y);
    m_cfg.ada_fc_off_z = json_number(d, "ada_fc_off_z", m_cfg.ada_fc_off_z);
    m_cfg.ada_box_off_x = json_number(d, "ada_box_off_x", m_cfg.ada_box_off_x);
    m_cfg.ada_box_off_y = json_number(d, "ada_box_off_y", m_cfg.ada_box_off_y);
    m_cfg.ada_box_off_z = json_number(d, "ada_box_off_z", m_cfg.ada_box_off_z);
    m_cfg.leon_fc_off_x = json_number(d, "leon_fc_off_x", m_cfg.leon_fc_off_x);
    m_cfg.leon_fc_off_y = json_number(d, "leon_fc_off_y", m_cfg.leon_fc_off_y);
    m_cfg.leon_fc_off_z = json_number(d, "leon_fc_off_z", m_cfg.leon_fc_off_z);
    m_cfg.leon_evt_off_x = json_number(d, "leon_evt_off_x", m_cfg.leon_evt_off_x);
    m_cfg.leon_evt_off_y = json_number(d, "leon_evt_off_y", m_cfg.leon_evt_off_y);
    m_cfg.leon_evt_off_z = json_number(d, "leon_evt_off_z", m_cfg.leon_evt_off_z);
    m_cfg.acrouch_off_x = json_number(d, "acrouch_off_x", m_cfg.acrouch_off_x);
    m_cfg.acrouch_off_y = json_number(d, "acrouch_off_y", m_cfg.acrouch_off_y);
    m_cfg.acrouch_off_z = json_number(d, "acrouch_off_z", m_cfg.acrouch_off_z);

    // Lua Z.198-206: `d.X ~= nil` -> `d.X == true`.
    if (json_present(d, "movement_stabilization")) {
        m_cfg.movement_stabilization = json_eq_true(d, "movement_stabilization");
    }

    if (json_present(d, "movement_follows_hmd")) {
        m_cfg.movement_follows_hmd = json_eq_true(d, "movement_follows_hmd");
    }

    m_cfg.bob_tau = json_number(d, "bob_tau", m_cfg.bob_tau);
    m_cfg.surge_tau = json_number(d, "surge_tau", m_cfg.surge_tau);

    if (json_present(d, "crouch_cam_lerp")) {
        m_cfg.crouch_cam_lerp = json_eq_true(d, "crouch_cam_lerp");
    }

    m_cfg.crouch_cam_tau = json_number(d, "crouch_cam_tau", m_cfg.crouch_cam_tau);

    if (json_present(d, "standup_cam_track")) {
        m_cfg.standup_cam_track = json_eq_true(d, "standup_cam_track");
    }

    if (json_present(d, "hide_streaming_dummy")) {
        m_cfg.hide_streaming_dummy = json_eq_true(d, "hide_streaming_dummy");
    }

    if (json_present(d, "recenter_on_killswitch")) {
        m_cfg.recenter_on_killswitch = json_eq_true(d, "recenter_on_killswitch");
    }

    m_cfg.bino_start = json_number(d, "bino_start", m_cfg.bino_start);
    m_cfg.bino_min = json_number(d, "bino_min", m_cfg.bino_min);
    m_cfg.bino_max = json_number(d, "bino_max", m_cfg.bino_max);
    m_cfg.bino_speed = json_number(d, "bino_speed", m_cfg.bino_speed);
}

void RE4VRFirstPerson::save_cfg() {
    // Lua Z.140: json.dump_file(CFG_PATH, cfg) -- Default-Indent ist 4.
    nlohmann::json j = nlohmann::json::object();

    j["off_x"] = m_cfg.off_x; j["off_y"] = m_cfg.off_y; j["off_z"] = m_cfg.off_z;
    j["event_off_x"] = m_cfg.event_off_x; j["event_off_y"] = m_cfg.event_off_y; j["event_off_z"] = m_cfg.event_off_z;
    j["event2_off_x"] = m_cfg.event2_off_x; j["event2_off_y"] = m_cfg.event2_off_y; j["event2_off_z"] = m_cfg.event2_off_z;
    j["event3_off_x"] = m_cfg.event3_off_x; j["event3_off_y"] = m_cfg.event3_off_y; j["event3_off_z"] = m_cfg.event3_off_z;
    j["event4_off_x"] = m_cfg.event4_off_x; j["event4_off_y"] = m_cfg.event4_off_y; j["event4_off_z"] = m_cfg.event4_off_z;
    j["event5_mono"] = m_cfg.event5_mono;
    j["event5_off_x"] = m_cfg.event5_off_x; j["event5_off_y"] = m_cfg.event5_off_y; j["event5_off_z"] = m_cfg.event5_off_z;
    j["event6_off_x"] = m_cfg.event6_off_x; j["event6_off_y"] = m_cfg.event6_off_y; j["event6_off_z"] = m_cfg.event6_off_z;
    j["turret_off_x"] = m_cfg.turret_off_x; j["turret_off_y"] = m_cfg.turret_off_y; j["turret_off_z"] = m_cfg.turret_off_z;
    j["jetski_off_x"] = m_cfg.jetski_off_x; j["jetski_off_y"] = m_cfg.jetski_off_y; j["jetski_off_z"] = m_cfg.jetski_off_z;
    j["boat_off_x"] = m_cfg.boat_off_x; j["boat_off_y"] = m_cfg.boat_off_y; j["boat_off_z"] = m_cfg.boat_off_z;
    j["begcrouch_off_x"] = m_cfg.begcrouch_off_x; j["begcrouch_off_y"] = m_cfg.begcrouch_off_y; j["begcrouch_off_z"] = m_cfg.begcrouch_off_z;
    j["ada_fc_off_x"] = m_cfg.ada_fc_off_x; j["ada_fc_off_y"] = m_cfg.ada_fc_off_y; j["ada_fc_off_z"] = m_cfg.ada_fc_off_z;
    j["leon_fc_off_x"] = m_cfg.leon_fc_off_x; j["leon_fc_off_y"] = m_cfg.leon_fc_off_y; j["leon_fc_off_z"] = m_cfg.leon_fc_off_z;
    j["ada_box_off_x"] = m_cfg.ada_box_off_x; j["ada_box_off_y"] = m_cfg.ada_box_off_y; j["ada_box_off_z"] = m_cfg.ada_box_off_z;
    j["leon_evt_off_x"] = m_cfg.leon_evt_off_x; j["leon_evt_off_y"] = m_cfg.leon_evt_off_y; j["leon_evt_off_z"] = m_cfg.leon_evt_off_z;
    j["acrouch_off_x"] = m_cfg.acrouch_off_x; j["acrouch_off_y"] = m_cfg.acrouch_off_y; j["acrouch_off_z"] = m_cfg.acrouch_off_z;
    j["cart_off_x"] = m_cfg.cart_off_x; j["cart_off_y"] = m_cfg.cart_off_y; j["cart_off_z"] = m_cfg.cart_off_z;
    j["headpin_fade_dur"] = m_cfg.headpin_fade_dur;
    j["movement_stabilization"] = m_cfg.movement_stabilization;
    j["movement_follows_hmd"] = m_cfg.movement_follows_hmd;
    j["bob_tau"] = m_cfg.bob_tau;
    j["surge_tau"] = m_cfg.surge_tau;
    j["crouch_cam_lerp"] = m_cfg.crouch_cam_lerp;
    j["crouch_cam_tau"] = m_cfg.crouch_cam_tau;
    j["standup_cam_track"] = m_cfg.standup_cam_track;
    j["hide_streaming_dummy"] = m_cfg.hide_streaming_dummy;
    j["recenter_on_killswitch"] = m_cfg.recenter_on_killswitch;
    j["bino_start"] = m_cfg.bino_start;
    j["bino_min"] = m_cfg.bino_min;
    j["bino_max"] = m_cfg.bino_max;
    j["bino_speed"] = m_cfg.bino_speed;
    j["block_force_twirler"] = m_cfg.block_force_twirler;

    re4vr::json_save(CFG_PATH, j, 4);
}

// =====================================================================
// Getter
// =====================================================================
::REManagedObject* RE4VRFirstPerson::get_player_ctx() {
    // Lua Z.220-226 fragt zuerst den Lua-Frame-Cache. Nativ waere ein Ruecksprung
    // nach Lua teurer als der Weg selbst -- und beide liefern DASSELBE Objekt.
    // Deshalb hier der direkte Weg; der Frame-Cache-Gedanke steckt in
    // m_fp_frame (unten in fp_stage_cached/fp_busy_cached).
    const auto cm = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CharacterManager"));

    if (cm == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");
}

int32_t RE4VRFirstPerson::fp_stage_cached() {
    // Lua Z.252-261.
    const auto ctx = get_player_ctx();

    if (ctx == nullptr) {
        m_fp_stage_valid = false;
        return -1;
    }

    const bool perf_on = !re4vr::lua_get_bool("__re4_fp_perf_off", false);

    if (!perf_on) {
        return re4vr::call_safe<int32_t>(ctx, "get_CurrentStageID");
    }

    if (m_fp_stage_f == m_fp_frame && m_fp_stage_valid) {
        return m_fp_stage;
    }

    m_fp_stage = re4vr::call_safe<int32_t>(ctx, "get_CurrentStageID");
    m_fp_stage_valid = true;
    m_fp_stage_f = m_fp_frame;
    return m_fp_stage;
}

::REManagedObject* RE4VRFirstPerson::fp_busy_cached() {
    // Lua Z.263-274.
    // [WIE_LUA -- QUIRK 1] Diese Funktion steht im Original VOR der Deklaration
    // `local camera_system` (Z.371) und bindet deshalb an das GLOBALE
    // camera_system. Es gibt also zwei getrennte Singleton-Caches, und
    // on_script_reset leert nur den lokalen. Genau so nachgebaut.
    if (m_camera_system_global == nullptr) {
        m_camera_system_global =
            sdk::get_managed_singleton<::REManagedObject>(game_namespace("CameraSystem"));
    }

    const bool perf_on = !re4vr::lua_get_bool("__re4_fp_perf_off", false);

    if (!perf_on) {
        const auto main = m_camera_system_global != nullptr
            ? re4vr::call_safe<::REManagedObject*>(m_camera_system_global, "get_MainCameraController")
            : nullptr;
        return main != nullptr ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
                               : nullptr;
    }

    if (m_fp_busy_f == m_fp_frame) {
        return m_fp_busy;
    }

    const auto main = m_camera_system_global != nullptr
        ? re4vr::call_safe<::REManagedObject*>(m_camera_system_global, "get_MainCameraController")
        : nullptr;
    m_fp_busy = main != nullptr ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
                                : nullptr;
    m_fp_busy_f = m_fp_frame;
    return m_fp_busy;
}

bool RE4VRFirstPerson::is_ada_now() {
    // Lua Z.280-285.
    const auto ctx = get_player_ctx();
    const auto body = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject") : nullptr;

    if (body == nullptr) {
        return false;
    }

    const auto name = re4vr::call_safe<::REManagedObject*>(body, "get_Name");

    if (name == nullptr) {
        return false;
    }

    return utility::re_string::get_string(reinterpret_cast<::SystemString*>(name)) == "ch3a8z0_body";
}

bool RE4VRFirstPerson::is_ashley_now() {
    // Lua Z.288-293.
    const auto ctx = get_player_ctx();
    const auto body = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject") : nullptr;

    if (body == nullptr) {
        return false;
    }

    const auto name = re4vr::call_safe<::REManagedObject*>(body, "get_Name");

    if (name == nullptr) {
        return false;
    }

    return utility::re_string::get_string(reinterpret_cast<::SystemString*>(name)) == "ch0a1z0_body";
}

::REManagedObject* RE4VRFirstPerson::get_body_transform() {
    // Lua Z.295-301.
    const auto ctx = get_player_ctx();

    if (ctx == nullptr) {
        return nullptr;
    }

    const auto body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(body, "get_Transform");
}

::REJoint* RE4VRFirstPerson::get_camera_joint() {
    // Lua Z.303-312: primary camera -> GameObject -> Transform -> Joints[0].
    const auto cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());

    if (cam == nullptr) {
        return nullptr;
    }

    const auto go = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject");

    if (go == nullptr) {
        return nullptr;
    }

    const auto tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return nullptr;
    }

    const auto joints = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints");

    if (joints == nullptr) {
        return nullptr;
    }

    return utility::re_array::get_element<::REJoint>(reinterpret_cast<::REArrayBase*>(joints), 0);
}

bool RE4VRFirstPerson::is_joint_valid(::REJoint* j) {
    // Lua Z.319-323. [WIE_LUA] OHNE die 0,5-s-Drosselung, die arm_chain hat.
    if (j == nullptr) {
        return false;
    }

    // [REF] Wache wie in RE4VRArmChain: ein freigegebener Zeiger wuerde in
    // get_type_definition eine Access Violation ausloesen, keine Exception.
    if (!utility::re_managed_object::is_managed_object(j)) {
        return false;
    }

    glm::vec3 dummy{};
    return get_vec3(reinterpret_cast<::REManagedObject*>(j), "get_Position", dummy);
}

void RE4VRFirstPerson::store_head_joint(::REJoint* j) {
    // [REF] Gecachte Engine-Objekte brauchen add_ref -- in Lua macht das
    // sol_lua_push unsichtbar mit (Sdk.cpp Z.49-68).
    if (m_head_joint == j) {
        return;
    }

    if (m_head_joint != nullptr && m_head_joint_reffed) {
        utility::re_managed_object::release(reinterpret_cast<::REManagedObject*>(m_head_joint));
    }

    m_head_joint = nullptr;
    m_head_joint_reffed = false;

    if (j == nullptr || !utility::re_managed_object::is_managed_object(j)) {
        return;
    }

    auto obj = reinterpret_cast<::REManagedObject*>(j);

    if (static_cast<int32_t>(obj->referenceCount) > 0) {
        utility::re_managed_object::add_ref(obj);
        m_head_joint_reffed = true;
    }

    m_head_joint = j;
}

::REJoint* RE4VRFirstPerson::get_head_joint() {
    // Lua Z.325-336.
    if (is_joint_valid(m_head_joint)) {
        return m_head_joint;
    }

    store_head_joint(nullptr);

    const auto btf = get_body_transform();

    if (btf == nullptr) {
        return nullptr;
    }

    const auto j = get_joint_by_name(btf, L"Head");

    if (j != nullptr && is_joint_valid(j)) {
        store_head_joint(j);
        return m_head_joint;
    }

    return nullptr;
}

// =====================================================================
// Gates
// =====================================================================
bool RE4VRFirstPerson::active() {
    // Lua Z.347-361.
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    if (re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false)) {
        bool keep_fp = false;

        const bool ks2 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks2", false);
        const bool ks3 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks3", false);
        const bool ks4 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks4", false);
        const bool ks5 = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks5", false);

        keep_fp = ks2 || ks3 || ks4 || ks5;

        // [FP-MESSUNG AUSGEBAUT 2026-09-08] Der Mitschnitt nach
        // re4_fp_events.txt (Killswitch-Grund je Fall einmal) ist raus.

        if (!keep_fp) {
            return false;
        }
    }

    return true;
}

bool RE4VRFirstPerson::is_crouch_now() {
    // Lua Z.736-740.
    return re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_crouch_active", false);
}

bool RE4VRFirstPerson::on_ladder_climb() {
    // Lua Z.793-802.
    bool is_fp = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks2", false);

    if (!is_fp) {
        is_fp = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks4", false);
    }

    if (!is_fp) {
        return false;
    }

    const auto ctx = get_player_ctx();

    if (ctx == nullptr) {
        return false;
    }

    bool v = false;
    return re4vr::try_call<bool>(ctx, "get_IsLadder", v) && v;
}

bool RE4VRFirstPerson::is_gimmick_ks3_now() {
    // Lua Z.806-810.
    return re4vr::lua_module_call_string("re4vr/re4_vr_killswitch", "get_activating_controller")
           == "ks3_gimmick";
}

// =====================================================================
// Kamera-Basis
// =====================================================================
std::optional<glm::quat> RE4VRFirstPerson::get_game_cam_yaw() {
    // Lua Z.387-409.
    if (m_camera_system == nullptr) {
        m_camera_system = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CameraSystem"));
    }

    const auto main = m_camera_system != nullptr
        ? re4vr::call_safe<::REManagedObject*>(m_camera_system, "get_MainCameraController")
        : nullptr;
    const auto busy = main != nullptr
        ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
        : nullptr;

    if (busy == nullptr) {
        return std::nullopt;
    }

    if (!type_is_a(busy, m_player_cam_td)) {
        return std::nullopt;
    }

    glm::quat cam_rot{};

    if (!field_get_quat(busy, "_CameraRotation", cam_rot)) {
        return std::nullopt;
    }

    return flat_yaw_from_rot(cam_rot);
}

std::optional<glm::quat> RE4VRFirstPerson::get_primary_cam_flat_yaw() {
    // Lua Z.516-527.
    const auto cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());

    if (cam == nullptr) {
        return std::nullopt;
    }

    const auto go = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject");

    if (go == nullptr) {
        return std::nullopt;
    }

    const auto tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return std::nullopt;
    }

    glm::quat rot{};

    if (!get_quat(tf, "get_Rotation", rot)) {
        return std::nullopt;
    }

    return flat_yaw_from_rot(rot);
}

// =====================================================================
// Filter
// =====================================================================
glm::vec3 RE4VRFirstPerson::apply_bob_filter(glm::vec3 hp, bool frame_tick) {
    // Lua Z.549-601.
    const float tau = m_cfg.bob_tau;

    if (tau <= 0.001f) {
        m_bob.ema.reset();
        return hp;
    }

    // [STANDUP_TRACK] Lua Z.559-562.
    if (m_cfg.standup_cam_track && m_crouch_cam.standup_until.has_value()
        && clock_now() < *m_crouch_cam.standup_until) {
        m_bob.ema.reset();
    }

    const auto btf = get_body_transform();
    glm::vec3 bp{};

    if (btf == nullptr || !get_vec3(btf, "get_Position", bp)) {
        return hp;
    }

    // [CAPY] Lua Z.567-577: die EMA nur bei frame_tick ...
    if (frame_tick) {
        const double nowc = clock_now();
        const double dtc = m_capy.last_t.has_value() ? std::min(nowc - *m_capy.last_t, 0.1) : 0.016;
        m_capy.last_t = nowc;

        if (!m_capy.ema.has_value() || std::abs(bp.y - *m_capy.ema) > 0.5f) {
            m_capy.ema = bp.y; // Teleport/Spawn: hart aufsetzen
        } else {
            const float ac = 1.0f - std::exp(static_cast<float>(-dtc) / CAPY_TAU);
            m_capy.ema = *m_capy.ema + (bp.y - *m_capy.ema) * ac;
        }
    }

    // ... die ANWENDUNG dagegen in jeder Phase (Lua Z.578 steht ausserhalb
    // des frame_tick-Blocks).
    if (m_capy.ema.has_value()) {
        bp = glm::vec3{bp.x, *m_capy.ema, bp.z};
    }

    const glm::vec3 rel = hp - bp;

    if (!m_bob.ema.has_value()) {
        m_bob.ema = rel;
        return hp;
    }

    glm::vec3 e = *m_bob.ema;
    const float dx = rel.x - e.x;
    const float dy = rel.y - e.y;
    const float dz = rel.z - e.z;

    // Teleport/Cutscene/Stance-Sprung (>0.5 m): hart re-latchen.
    if ((dx * dx + dy * dy + dz * dz) > 0.25f) {
        m_bob.ema = rel;
        return hp;
    }

    if (frame_tick) {
        const double now = clock_now();
        const double dt = m_bob.last_t.has_value() ? std::min(now - *m_bob.last_t, 0.1) : 0.016;
        m_bob.last_t = now;
        const float a = 1.0f - std::exp(static_cast<float>(-dt) / tau);
        m_bob.ema = glm::vec3{e.x + dx * a, e.y + dy * a, e.z + dz * a};
        e = *m_bob.ema;
    }

    return glm::vec3{bp.x + e.x, bp.y + e.y, bp.z + e.z};
}

glm::vec3 RE4VRFirstPerson::apply_surge_filter(glm::vec3 hp, const std::optional<glm::vec3>& bp_opt,
                                               bool frame_tick) {
    // Lua Z.610-725.
    const float tau = m_cfg.surge_tau;

    if (tau <= 0.001f || !bp_opt.has_value()) {
        m_surge.px.reset();
        re4vr::lua_set_nil("__vr_surge_dx");
        re4vr::lua_set_nil("__vr_surge_dz");
        return hp;
    }

    const glm::vec3 bp = *bp_opt;

    if (frame_tick) {
        const double now = clock_now();
        const double dt = m_surge.last_t.has_value() ? std::min(now - *m_surge.last_t, 0.1) : 0.016;
        m_surge.last_t = now;

        if (!m_surge.px.has_value() || !m_surge.lx.has_value()) {
            m_surge.px = bp.x;
            m_surge.pz = bp.z;
            m_surge.vx = 0.0f;
            m_surge.vz = 0.0f;
        } else {
            const float fdt = static_cast<float>(dt);
            const float rvx = (bp.x - *m_surge.lx) / fdt;
            const float rvz = (bp.z - *m_surge.lz) / fdt;
            const float rspeed = std::sqrt(rvx * rvx + rvz * rvz);
            const float ex = bp.x - *m_surge.px;
            const float ez = bp.z - *m_surge.pz;

            if ((ex * ex + ez * ez) > 1.0f) {
                // Teleport/Cutscene: hart neu aufsetzen.
                m_surge.px = bp.x;
                m_surge.pz = bp.z;
                m_surge.vx = 0.0f;
                m_surge.vz = 0.0f;
            } else if (rspeed < 0.3f) {
                // Stillstand: schnell einrasten.
                const float a = 1.0f - std::exp(-fdt / 0.07f);
                m_surge.px = *m_surge.px + ex * a;
                m_surge.pz = *m_surge.pz + ez * a;
                m_surge.vx = 0.0f;
                m_surge.vz = 0.0f;
            } else {
                // Drehen am BODY-YAW erkennen, nicht an der Velocity.
                float yaw_rate = 0.0f;
                const auto btf2 = get_body_transform();
                glm::quat br{};

                if (btf2 != nullptr && get_quat(btf2, "get_Rotation", br)) {
                    const glm::vec3 f = br * glm::vec3{0.0f, 0.0f, 1.0f};
                    const float yl = std::sqrt(f.x * f.x + f.z * f.z);

                    if (yl > 0.0001f) {
                        const float yaw = std::atan2(f.x / yl, f.z / yl);

                        if (m_surge.lyaw.has_value()) {
                            float dyw = yaw - *m_surge.lyaw;

                            if (dyw > 3.14159265358979323846f) {
                                dyw -= 2.0f * 3.14159265358979323846f;
                            } else if (dyw < -3.14159265358979323846f) {
                                dyw += 2.0f * 3.14159265358979323846f;
                            }

                            yaw_rate = std::abs(dyw) / fdt;
                        }

                        m_surge.lyaw = yaw;
                    }
                    // [WIE_LUA] Faellt yl durch, bleibt yaw_rate 0 UND lyaw
                    // wird NICHT fortgeschrieben -- der naechste gueltige Frame
                    // rechnet gegen den alten Yaw.
                }

                // Entprellen: erst ab 3 Frames anhaltendem Drehen.
                if (yaw_rate > 1.0471975512f) { // rad(60)
                    m_surge.turn_n = m_surge.turn_n + 1;
                } else {
                    m_surge.turn_n = 0;
                }

                if (m_surge.turn_n >= 3) {
                    const float a = 1.0f - std::exp(-fdt / 0.07f);
                    m_surge.px = *m_surge.px + ex * a;
                    m_surge.pz = *m_surge.pz + ez * a;
                    m_surge.vx = rvx;
                    m_surge.vz = rvz;
                } else {
                    const float av = 1.0f - std::exp(-fdt / tau);
                    m_surge.vx = m_surge.vx + (rvx - m_surge.vx) * av;
                    m_surge.vz = m_surge.vz + (rvz - m_surge.vz) * av;
                    m_surge.px = *m_surge.px + m_surge.vx * fdt;
                    m_surge.pz = *m_surge.pz + m_surge.vz * fdt;

                    // Drift-Bindung PROGRESSIV (unter 0.10 m gar kein Pull).
                    const float fx0 = bp.x - *m_surge.px;
                    const float fz0 = bp.z - *m_surge.pz;
                    const float fl0 = std::sqrt(fx0 * fx0 + fz0 * fz0);

                    if (fl0 > 0.10f) {
                        float t = (fl0 - 0.10f) / 0.15f;

                        if (t > 1.0f) {
                            t = 1.0f;
                        }

                        const float ap = (1.0f - std::exp(-fdt / 0.15f)) * t * t;
                        m_surge.px = *m_surge.px + fx0 * ap;
                        m_surge.pz = *m_surge.pz + fz0 * ap;
                    }
                }

                // Harter Fehler-Deckel: nie weiter als 0.25 m von der Kapsel.
                const float fx = bp.x - *m_surge.px;
                const float fz = bp.z - *m_surge.pz;
                const float fl = std::sqrt(fx * fx + fz * fz);

                if (fl > 0.25f) {
                    const float s = 1.0f - (0.25f / fl);
                    m_surge.px = *m_surge.px + fx * s;
                    m_surge.pz = *m_surge.pz + fz * s;
                }
            }
        }

        m_surge.lx = bp.x;
        m_surge.lz = bp.z;
    }

    if (!m_surge.px.has_value()) {
        re4vr::lua_set_nil("__vr_surge_dx");
        re4vr::lua_set_nil("__vr_surge_dz");
        return hp;
    }

    // Export fuer movement.lua (SPINE_PIN-Bridge).
    re4vr::lua_set_number("__vr_surge_dx", *m_surge.px - bp.x);
    re4vr::lua_set_number("__vr_surge_dz", *m_surge.pz - bp.z);

    return glm::vec3{hp.x + (*m_surge.px - bp.x), hp.y, hp.z + (*m_surge.pz - bp.z)};
}

float RE4VRFirstPerson::apply_crouch_cam_lerp(float y, bool frame_tick) {
    // Lua Z.742-788.
    const bool crouching = is_crouch_now();
    const double now = clock_now();

    // Flanken IMMER auswerten -- auch bei abgeschaltetem Lerp.
    if (crouching && !m_crouch_cam.was) {
        m_crouch_cam.until_t = now + CROUCH_CAM_WINDOW;
        m_crouch_cam.ema = y;
        m_crouch_cam.last_t = now;
    } else if (!crouching && m_crouch_cam.was) {
        m_crouch_cam.standup_until = now + STANDUP_WINDOW;
        m_crouch_cam.until_t.reset();
    }

    m_crouch_cam.was = crouching;

    const float tau = m_cfg.crouch_cam_tau;

    if (!m_cfg.crouch_cam_lerp || tau <= 0.001f) {
        m_crouch_cam.ema = y;
        m_crouch_cam.until_t.reset();
        return y;
    }

    const bool in_window = m_crouch_cam.until_t.has_value() && now < *m_crouch_cam.until_t && crouching;

    if (!in_window) {
        m_crouch_cam.ema = y;
        m_crouch_cam.until_t.reset();
        return y;
    }

    if (!m_crouch_cam.ema.has_value()) {
        m_crouch_cam.ema = y;
    }

    if (frame_tick) {
        const double dt = m_crouch_cam.last_t.has_value() ? std::min(now - *m_crouch_cam.last_t, 0.1) : 0.016;
        m_crouch_cam.last_t = now;

        if (y < *m_crouch_cam.ema) {
            const float a = 1.0f - std::exp(static_cast<float>(-dt) / tau);
            m_crouch_cam.ema = *m_crouch_cam.ema + (y - *m_crouch_cam.ema) * a;
        } else {
            m_crouch_cam.ema = y; // nie nach oben laggen
        }

        if (std::abs(*m_crouch_cam.ema - y) < 0.005f) {
            m_crouch_cam.until_t.reset();
        }
    }

    return *m_crouch_cam.ema;
}

// =====================================================================
// Kern -- Lua Z.819-1116
// =====================================================================
void RE4VRFirstPerson::compute_and_set(bool frame_tick) {
    const auto set_fix_inactive = []() {
        re4vr::lua_ensure_table("vr_camera_fix");
        re4vr::lua_set_table_bool("vr_camera_fix", "active", false);
    };

    if (!active()) {
        set_fix_inactive();
        return;
    }

    const auto hj = get_head_joint();

    if (hj == nullptr) {
        set_fix_inactive();
        return;
    }

    auto hj_obj = reinterpret_cast<::REManagedObject*>(hj);

    // ---- Zustands-Erhebung (Lua Z.850-905) ----
    const bool grappled_now = re4vr::lua_get_bool("__re4_grappled_active", false);
    const bool boxbreak_now = re4vr::lua_get_bool("__re4_boxbreak_active", false);
    const bool ashley_ev_now = is_gimmick_ks3_now();
    const bool ladder_now = on_ladder_climb();
    const bool fatalkick_now = re4vr::lua_get_bool("__re4_fatalkick_active", false);
    const bool forcecrouch_now = re4vr::lua_get_bool("__re4_forcecrouch_active", false);
    const bool gondola_now = re4vr::lua_get_bool("__re4_gondola_active", false);
    const bool jetski_now = re4vr::lua_get_bool("__re4_jetski_active", false);
    const bool boat_now = re4vr::lua_get_bool("__re4_boat_active", false);
    const bool begcrouch_now = re4vr::lua_get_bool("__re4_forcecrouch_ks4_active", false);
    const bool ks2_now = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks2", false);
    const bool ks4_now = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_ks4", false);

    const bool headpin_now = grappled_now || boxbreak_now || ashley_ev_now || ladder_now || ks2_now
        || fatalkick_now || forcecrouch_now || gondola_now || jetski_now || boat_now
        || begcrouch_now || ks4_now;

    const bool cart_now = re4vr::lua_get_bool("__re4_railcar_mode", false)
        || re4vr::lua_get_bool("__re4_minecart_ks4_active", false)
        || re4vr::lua_get_bool("__re4_minecart2_ks4_active", false);

    // ---- Head-Nagel-Zweig (Lua Z.906-1007) ----
    if (cart_now || headpin_now) {
        glm::vec3 head{};

        if (!get_vec3(hj_obj, "get_Position", head)) {
            set_fix_inactive();
            return;
        }

        glm::vec3 cam_pos = head;
        float ax = 0.0f, ay = 0.0f, az = 0.0f;

        // [LOREN-VORRANG] Reihenfolge ist inhaltlich begruendet -- nicht umsortieren.
        if (cart_now) {
            ax = m_cfg.cart_off_x; ay = m_cfg.cart_off_y; az = m_cfg.cart_off_z;
        } else if (jetski_now) {
            ax = m_cfg.jetski_off_x; ay = m_cfg.jetski_off_y; az = m_cfg.jetski_off_z;
        } else if (boat_now) {
            ax = m_cfg.boat_off_x; ay = m_cfg.boat_off_y; az = m_cfg.boat_off_z;
        } else if (begcrouch_now) {
            ax = m_cfg.begcrouch_off_x; ay = m_cfg.begcrouch_off_y; az = m_cfg.begcrouch_off_z;
        } else if (forcecrouch_now && is_ada_now()) {
            ax = m_cfg.ada_fc_off_x; ay = m_cfg.ada_fc_off_y; az = m_cfg.ada_fc_off_z;
        } else if (forcecrouch_now) {
            ax = m_cfg.leon_fc_off_x; ay = m_cfg.leon_fc_off_y; az = m_cfg.leon_fc_off_z;
        } else if (is_ada_now()) {
            ax = m_cfg.ada_box_off_x; ay = m_cfg.ada_box_off_y; az = m_cfg.ada_box_off_z;
        } else {
            ax = m_cfg.leon_evt_off_x; ay = m_cfg.leon_evt_off_y; az = m_cfg.leon_evt_off_z;
        }

        // [HEADPIN_FADE] Trennlinie ist KAMPF vs. RUHE, nicht der KS-Grad.
        if (headpin_now && !grappled_now && !fatalkick_now && !cart_now) {
            m_headpin_fade.was = true;
            m_headpin_fade.ax = ax;
            m_headpin_fade.ay = ay;
            m_headpin_fade.az = az;
        } else {
            m_headpin_fade.was = false;
        }

        const float ox = m_cfg.off_x + ax;
        const float oy = m_cfg.off_y + ay;
        const float oz = m_cfg.off_z + az;

        if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
            const auto btf = get_body_transform();
            glm::quat rr{};

            if (btf != nullptr && get_quat(btf, "get_Rotation", rr)) {
                const glm::vec3 fwd = rr * glm::vec3{0.0f, 0.0f, 1.0f};
                const glm::vec3 right = rr * glm::vec3{1.0f, 0.0f, 0.0f};
                cam_pos = glm::vec3{head.x + right.x * ox + fwd.x * oz,
                                    head.y + oy,
                                    head.z + right.z * ox + fwd.z * oz};
            }
        }

        const auto cj = get_camera_joint();

        if (cj == nullptr) {
            return; // [WIE_LUA] ohne active zu aendern
        }

        set_vec3(reinterpret_cast<::REManagedObject*>(cj), "set_Position", cam_pos);

        // [HAND-ROLL FIX] Railcar-Kamera ist ein VehicleCameraController ->
        // get_game_cam_yaw nil -> flacher primary-cam-Yaw statt active=false.
        auto yaw = get_game_cam_yaw();

        if (!yaw.has_value()) {
            yaw = get_primary_cam_flat_yaw();
        }

        re4vr::lua_ensure_table("vr_camera_fix");

        if (yaw.has_value()) {
            re4vr::lua_set_table_vec3("vr_camera_fix", "camera_pos", cam_pos);
            re4vr::lua_set_table_quat("vr_camera_fix", "camera_rot", *yaw);
            re4vr::lua_set_table_bool("vr_camera_fix", "active", true);
        } else {
            re4vr::lua_set_table_bool("vr_camera_fix", "active", false);
        }

        return;
    }

    // ---- Normale Kette (Lua Z.1009-1116) ----
    if (m_headpin_fade.was) {
        m_headpin_fade.was = false;
        m_headpin_fade.t = clock_now();
    }

    glm::vec3 hp{};

    if (!get_vec3(hj_obj, "get_Position", hp)) {
        return;
    }

    // [PIN_ANCHOR] Lua Z.1026-1030.
    if (re4vr::lua_get_bool("__vr_surge_bridged", false)) {
        const auto btf0 = get_body_transform();
        glm::vec3 bp0{};

        if (btf0 != nullptr && get_vec3(btf0, "get_Position", bp0)) {
            hp = glm::vec3{bp0.x, hp.y, bp0.z};
        }
    }

    hp = apply_bob_filter(hp, frame_tick);

    {
        const auto btf = get_body_transform();
        std::optional<glm::vec3> bp;
        glm::vec3 tmp{};

        if (btf != nullptr && get_vec3(btf, "get_Position", tmp)) {
            bp = tmp;
        }

        hp = apply_surge_filter(hp, bp, frame_tick);
    }

    glm::vec3 cam_pos = hp;

    float ox = m_cfg.off_x;
    float oy = m_cfg.off_y;
    float oz = m_cfg.off_z;

    // [ASHLEY_CROUCH] ERSETZT die Basis-Offsets (Lua Z.1045-1051).
    if (is_crouch_now() && is_ashley_now()) {
        ox = m_cfg.acrouch_off_x;
        oy = m_cfg.acrouch_off_y;
        oz = m_cfg.acrouch_off_z;
    }

    if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
        const auto btf = get_body_transform();
        glm::quat rr{};

        if (btf != nullptr && get_quat(btf, "get_Rotation", rr)) {
            const glm::vec3 fwd = rr * glm::vec3{0.0f, 0.0f, 1.0f};
            const glm::vec3 right = rr * glm::vec3{1.0f, 0.0f, 0.0f};
            cam_pos = glm::vec3{hp.x + right.x * ox + fwd.x * oz,
                                hp.y + oy,
                                hp.z + right.z * ox + fwd.z * oz};
        }
    }

    cam_pos = glm::vec3{cam_pos.x, apply_crouch_cam_lerp(cam_pos.y, frame_tick), cam_pos.z};

    // [HEADPIN_FADE] Lua Z.1073-1101.
    if (m_headpin_fade.t > 0.0) {
        const double dur = static_cast<double>(m_cfg.headpin_fade_dur);
        const double el = clock_now() - m_headpin_fade.t;

        if (dur <= 0.001 || el >= dur) {
            m_headpin_fade.t = 0.0;
        } else {
            glm::vec3 pin = hp;
            const float px = m_cfg.off_x + m_headpin_fade.ax;
            const float py = m_cfg.off_y + m_headpin_fade.ay;
            const float pz = m_cfg.off_z + m_headpin_fade.az;

            if (px != 0.0f || py != 0.0f || pz != 0.0f) {
                const auto btf2 = get_body_transform();
                glm::quat rr2{};

                if (btf2 != nullptr && get_quat(btf2, "get_Rotation", rr2)) {
                    const glm::vec3 fwd2 = rr2 * glm::vec3{0.0f, 0.0f, 1.0f};
                    const glm::vec3 right2 = rr2 * glm::vec3{1.0f, 0.0f, 0.0f};
                    pin = glm::vec3{hp.x + right2.x * px + fwd2.x * pz,
                                    hp.y + py,
                                    hp.z + right2.z * px + fwd2.z * pz};
                }
            }

            const float f = static_cast<float>(el / dur);
            cam_pos = glm::vec3{pin.x + (cam_pos.x - pin.x) * f,
                                pin.y + (cam_pos.y - pin.y) * f,
                                pin.z + (cam_pos.z - pin.z) * f};
        }
    }

    const auto cj = get_camera_joint();

    if (cj == nullptr) {
        return; // [WIE_LUA] ohne active zu aendern
    }

    set_vec3(reinterpret_cast<::REManagedObject*>(cj), "set_Position", cam_pos);

    // [WIE_LUA] Hier OHNE den Flat-Yaw-Fallback (den gibt es nur im Nagel-Zweig).
    const auto yaw = get_game_cam_yaw();

    re4vr::lua_ensure_table("vr_camera_fix");

    if (yaw.has_value()) {
        re4vr::lua_set_table_vec3("vr_camera_fix", "camera_pos", cam_pos);
        re4vr::lua_set_table_quat("vr_camera_fix", "camera_rot", *yaw);
        re4vr::lua_set_table_bool("vr_camera_fix", "active", true);
    } else {
        re4vr::lua_set_table_bool("vr_camera_fix", "active", false);
    }
}

// =====================================================================
// Movement follows HMD -- Lua Z.1121-1224
// =====================================================================
glm::vec2 RE4VRFirstPerson::get_left_input_axis() {
    auto& vr = VR::get();

    if (vr != nullptr && vr->is_using_controllers()) {
        const auto axis = vr->get_left_stick_axis();

        if (glm::length(axis) > 0.0f) {
            return axis;
        }
    }

    const auto gp = sdk::get_native_singleton("via.hid.GamePad");

    if (gp == nullptr || m_gamepad_td == nullptr) {
        return glm::vec2{0.0f, 0.0f};
    }

    const auto method = m_gamepad_td->get_method("get_LastInputDevice");

    if (method == nullptr) {
        return glm::vec2{0.0f, 0.0f};
    }

    ::REManagedObject* pad = nullptr;

    try {
        pad = method->call_safe<::REManagedObject*>(sdk::get_thread_context(), gp);
    } catch (...) {
        pad = nullptr;
    }

    re4vr::clear_vm_exception();

    if (pad == nullptr) {
        return glm::vec2{0.0f, 0.0f};
    }

    glm::vec2 axis{0.0f, 0.0f};

    // [K3] Feld statt Getter -- s. get_vec2_field.
    if (!get_vec2_field(pad, "AxisL", axis)) {
        return glm::vec2{0.0f, 0.0f};
    }

    return axis;
}

void RE4VRFirstPerson::apply_movement_stabilization() {
    if (!m_cfg.movement_stabilization) {
        m_move.has_valid_position = false;
        return;
    }

    // [GAMEPLAY_ONLY] Killswitch oder Throwsight -> der Stick darf den Body
    // nicht aus der nativen Animation ziehen.
    const bool ks_now = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false);

    if (ks_now || re4vr::lua_get_bool("__re4_throwsight_active", false)) {
        m_move.has_valid_position = false;
        m_move.last_time.reset();
        return;
    }

    if (!active()) {
        m_move.has_valid_position = false;
        m_move.last_time.reset();
        return;
    }

    const auto body_tr = get_body_transform();

    if (body_tr == nullptr) {
        m_move.has_valid_position = false;
        return;
    }

    const auto cam_joint = get_camera_joint();

    if (cam_joint == nullptr) {
        return; // [WIE_LUA] ohne has_valid_position zu aendern
    }

    const double now = clock_now();

    if (!m_move.last_time.has_value()) {
        m_move.last_time = now;
    }

    double dt = now - *m_move.last_time;
    m_move.last_time = now;

    if (dt < 0.001) { dt = 0.001; }
    if (dt > 0.1) { dt = 0.1; }

    glm::vec3 cur{};

    if (!get_vec3(body_tr, "get_Position", cur)) {
        return;
    }

    if (m_move.has_valid_position && m_move.last_player_position.has_value()) {
        glm::vec3 delta = cur - *m_move.last_player_position;
        delta.y = 0.0f;
        float speed = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        speed = std::min(speed, 1.0f);

        glm::quat camera_rot{};

        if (get_quat(reinterpret_cast<::REManagedObject*>(cam_joint), "get_Rotation", camera_rot)) {
            auto& vr = VR::get();

            // Lua Z.1190: Config-Gate UND vrmod-Pruefung.
            if (m_cfg.movement_follows_hmd && vr != nullptr) {
                const auto t0 = vr->get_transform(0);
                const glm::quat hmd_quat = glm::quat(t0);

                // Lua Z.1193: `if hmd_quat and hmd_quat.w then` -- in Lua eine
                // Existenzpruefung; nativ kann der Wert nur ungueltig sein.
                if (std::isfinite(hmd_quat.w)) {
                    const glm::quat rot_offset = vr->get_rotation_offset();
                    const glm::quat combined = rot_offset * hmd_quat;
                    const float siny = 2.0f * (combined.w * combined.y + combined.z * combined.x);
                    const float cosy = 1.0f - 2.0f * (combined.y * combined.y + combined.x * combined.x);
                    const float hmd_yaw = std::atan2(siny, cosy);
                    const float half = hmd_yaw * 0.5f;
                    const glm::quat hmd_flat{std::cos(half), 0.0f, std::sin(half), 0.0f};
                    camera_rot = camera_rot * hmd_flat;
                }
            }

            const glm::vec3 camera_dir = camera_rot * glm::vec3{0.0f, 0.0f, 1.0f};
            const glm::vec2 axis_l = get_left_input_axis();

            if (glm::length(axis_l) > 0.0f) {
                const glm::vec3 flat_raw{camera_dir.x, 0.0f, camera_dir.z};
                const float flat_len = std::sqrt(flat_raw.x * flat_raw.x + flat_raw.z * flat_raw.z);

                if (flat_len > 0.0f) {
                    const glm::vec3 flat_camera_dir = flat_raw / flat_len;
                    // [K1] s. lua_to_quat -- der glm-Zwei-Vektor-Konstruktor
                    // liefert bei exakt (0,0,-1) NaN, und dann faellt die
                    // Bewegung unten ganz aus (raw_len > 0 ist bei NaN falsch),
                    // waehrend Lua den Spieler bewegt.
                    const glm::quat flat_camera_rot = lua_to_quat(flat_camera_dir);
                    const glm::vec3 raw = flat_camera_rot * glm::vec3{axis_l.x, 0.0f, -axis_l.y};
                    const float raw_len = std::sqrt(raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);

                    // Lua Z.1213: zweite Schranke vor dem Schreiben.
                    if (raw_len > 0.0f) {
                        const glm::vec3 axis_l_dir = raw / raw_len;
                        glm::vec3 new_pos = *m_move.last_player_position + axis_l_dir * speed;
                        new_pos.y = cur.y;
                        set_vec3(body_tr, "set_Position", new_pos);
                    }
                }
            }
        }
    }

    glm::vec3 after{};

    if (get_vec3(body_tr, "get_Position", after)) {
        m_move.last_player_position = after;
    } else {
        m_move.last_player_position.reset();
    }

    m_move.has_valid_position = true;
}

// =====================================================================
// Recenter -- Lua Z.1235-1270
// =====================================================================
bool RE4VRFirstPerson::recenter_neutralize_headset() {
    auto& vr = VR::get();

    if (vr == nullptr) {
        return false;
    }

    const glm::quat hq = glm::quat(vr->get_transform(0));
    const auto h = yaw_of_quat(hq);

    if (!h.has_value()) {
        return false;
    }

    vr->set_rotation_offset(yaw_quat(-(*h)));
    return true;
}

void RE4VRFirstPerson::recenter_tick() {
    if (!m_cfg.recenter_on_killswitch) {
        if (m_rc_was_active) {
            re4vr::lua_set_bool("__vr_recenter_hold", false);
            m_rc_was_active = false;
        }

        return;
    }

    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return; // [WIE_LUA] ohne Flankenauswertung
    }

    const bool ks = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false);

    if (ks && !m_rc_was_active) {
        recenter_neutralize_headset();
        re4vr::lua_set_bool("__vr_recenter_hold", true);
    } else if (!ks && m_rc_was_active) {
        re4vr::lua_set_bool("__vr_recenter_hold", false);
    }

    m_rc_was_active = ks;
}

// =====================================================================
// Event-Offsets -- Lua Z.1280-1521
// =====================================================================
void RE4VRFirstPerson::apply_riddle_offset(float ox, float oy, float oz,
                                           int32_t stage_a, int32_t stage_b,
                                           float px, float py, float pz, float radius_sq) {
    // Gate-Reihenfolge exakt wie im Original.
    if (ox == 0.0f && oy == 0.0f && oz == 0.0f) {
        return;
    }

    if (get_player_ctx() == nullptr) {
        return;
    }

    const int32_t stage = fp_stage_cached();

    if (stage != stage_a && (stage_b < 0 || stage != stage_b)) {
        return;
    }

    const auto busy = fp_busy_cached();

    if (busy == nullptr) {
        return;
    }

    if (m_gimmickfix_cam_td == nullptr || !type_is_a(busy, m_gimmickfix_cam_td)) {
        return;
    }

    const auto btf = get_body_transform();
    glm::vec3 bp{};

    if (btf == nullptr || !get_vec3(btf, "get_Position", bp)) {
        return;
    }

    const float dx = bp.x - px;
    const float dy = bp.y - py;
    const float dz = bp.z - pz;

    if ((dx * dx + dy * dy + dz * dz) > radius_sq) {
        return;
    }

    const auto cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());

    if (cam == nullptr) {
        return;
    }

    const auto cgo = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject");

    if (cgo == nullptr) {
        return;
    }

    const auto ctf = re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform");

    if (ctf == nullptr) {
        return;
    }

    glm::vec4 p{};

    if (!transform_get_position(ctf, p)) {
        return;
    }

    transform_set_position(ctf, glm::vec4{p.x + ox, p.y + oy, p.z + oz, p.w});
}

void RE4VRFirstPerson::apply_event_hmd_offset() {
    apply_riddle_offset(m_cfg.event_off_x, m_cfg.event_off_y, m_cfg.event_off_z,
                        51503, 51502, -8.61f, 26.87f, -43.75f, 25.0f);
}

void RE4VRFirstPerson::apply_event2_hmd_offset() {
    apply_riddle_offset(m_cfg.event2_off_x, m_cfg.event2_off_y, m_cfg.event2_off_z,
                        61400, -1, 115.55f, 18.50f, -93.56f, 25.0f);
}

void RE4VRFirstPerson::apply_event3_hmd_offset() {
    apply_riddle_offset(m_cfg.event3_off_x, m_cfg.event3_off_y, m_cfg.event3_off_z,
                        61305, -1, 90.83f, 18.50f, -91.04f, 25.0f);
}

void RE4VRFirstPerson::apply_event4_hmd_offset() {
    apply_riddle_offset(m_cfg.event4_off_x, m_cfg.event4_off_y, m_cfg.event4_off_z,
                        63108, -1, -13.89f, 21.77f, -122.40f, 25.0f);
}

void RE4VRFirstPerson::apply_event5_hmd_offset() {
    // Radius 2 m -- MUSS < ~4 m bleiben, sonst greift die GimmickFix eine
    // Etage tiefer (10.81/8.88/109.02) mit.
    apply_riddle_offset(m_cfg.event5_off_x, m_cfg.event5_off_y, m_cfg.event5_off_z,
                        44110, -1, 10.75f, 12.87f, 107.76f, 4.0f);
}

void RE4VRFirstPerson::apply_event6_hmd_offset() {
    apply_riddle_offset(m_cfg.event6_off_x, m_cfg.event6_off_y, m_cfg.event6_off_z,
                        45401, -1, 106.80f, 11.63f, 100.55f, 25.0f);
}

void RE4VRFirstPerson::apply_event5_mono() {
    // Lua Z.1451-1478. Der Broker liegt in re4_vr_ui.lua.
    if (!re4vr::lua_has_function("__re4_mono_request")) {
        return; // kein Broker -> gar nichts
    }

    bool on = false;

    if (m_cfg.event5_mono) {
        const auto ctx = get_player_ctx();
        const int32_t stage = ctx != nullptr ? re4vr::call_safe<int32_t>(ctx, "get_CurrentStageID") : -1;

        if (stage == 44110) {
            // [WIE_LUA -- QUIRK 2] Diese Funktion benutzt die Perf-Caches NICHT,
            // sondern loest Stage und Busy-Kamera selbst auf.
            if (m_camera_system == nullptr) {
                m_camera_system = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CameraSystem"));
            }

            const auto main = m_camera_system != nullptr
                ? re4vr::call_safe<::REManagedObject*>(m_camera_system, "get_MainCameraController")
                : nullptr;
            const auto busy = main != nullptr
                ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
                : nullptr;

            if (busy != nullptr && m_gimmickfix_cam_td != nullptr && type_is_a(busy, m_gimmickfix_cam_td)) {
                const auto btf = get_body_transform();
                glm::vec3 bp{};

                if (btf != nullptr && get_vec3(btf, "get_Position", bp)) {
                    const float dx = bp.x - 10.75f;
                    const float dy = bp.y - 12.87f;
                    const float dz = bp.z - 107.76f;
                    on = (dx * dx + dy * dy + dz * dz) <= 4.0f;
                }
            }
        }
    }

    // [WIE_LUA] IMMER melden -- auch false, sonst bleibt Mono haengen.
    re4vr::lua_call_global_str_bool("__re4_mono_request", "symbol_riddle", on);
}

void RE4VRFirstPerson::apply_turret_hmd_offset() {
    // Lua Z.1506-1521. Einfacheres Gate: nur der Gimmick-Typ, kein Stage,
    // kein Radius -- und als einzige der sieben OHNE get_player_ctx().
    if (m_cfg.turret_off_x == 0.0f && m_cfg.turret_off_y == 0.0f && m_cfg.turret_off_z == 0.0f) {
        return;
    }

    const auto busy = fp_busy_cached();

    if (busy == nullptr) {
        return;
    }

    auto busy_def = utility::re_managed_object::get_type_definition(busy);

    if (busy_def == nullptr) {
        return;
    }

    const auto sp_field = busy_def->get_field("_CurrentStateParam");

    if (sp_field == nullptr) {
        return;
    }

    void* sp_ptr = nullptr;
    bool sp_is_value = false;

    try {
        auto sp_type = sp_field->get_type();
        sp_is_value = sp_type != nullptr && sp_type->is_value_type();

        if (sp_is_value) {
            sp_ptr = sp_field->get_data_raw(busy, false);
        } else {
            sp_ptr = sp_field->get_data<::REManagedObject*>(busy);
        }
    } catch (...) {
        sp_ptr = nullptr;
    }

    if (sp_ptr == nullptr) {
        return;
    }

    sdk::RETypeDefinition* sp_def = nullptr;

    if (sp_is_value) {
        sp_def = sp_field->get_type();
    } else {
        sp_def = utility::re_managed_object::get_type_definition(
            reinterpret_cast<::REManagedObject*>(sp_ptr));
    }

    if (sp_def == nullptr) {
        return;
    }

    const auto gt_field = sp_def->get_field("<GimmickType>k__BackingField");

    if (gt_field == nullptr) {
        return;
    }

    int32_t gt = -1;

    try {
        // Enums kommen ueber die TypeDB direkt als Integer -- das
        // value__-Auspacken der Lua-Fassung entfaellt.
        gt = *reinterpret_cast<int32_t*>(gt_field->get_data_raw(sp_ptr, sp_is_value));
    } catch (...) {
        return;
    }

    if (gt != m_turret_gimmick) {
        return;
    }

    const auto cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());

    if (cam == nullptr) {
        return;
    }

    const auto cgo = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject");

    if (cgo == nullptr) {
        return;
    }

    const auto ctf = re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform");

    if (ctf == nullptr) {
        return;
    }

    glm::vec4 p{};

    if (!transform_get_position(ctf, p)) {
        return;
    }

    transform_set_position(ctf, glm::vec4{p.x + m_cfg.turret_off_x,
                                          p.y + m_cfg.turret_off_y,
                                          p.z + m_cfg.turret_off_z, p.w});
}

void RE4VRFirstPerson::apply_all_event_offsets() {
    apply_event_hmd_offset();
    apply_event2_hmd_offset();
    apply_event3_hmd_offset();
    apply_event4_hmd_offset();
    apply_event5_hmd_offset();
    apply_event6_hmd_offset();
    apply_turret_hmd_offset();
}

// =====================================================================
// ForceTwirler -- Lua Z.430-508
// =====================================================================
::REManagedObject* RE4VRFirstPerson::get_player_cam() {
    if (m_camera_system == nullptr) {
        m_camera_system = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CameraSystem"));
    }

    const auto main = m_camera_system != nullptr
        ? re4vr::call_safe<::REManagedObject*>(m_camera_system, "get_MainCameraController")
        : nullptr;
    const auto busy = main != nullptr
        ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
        : nullptr;

    if (busy == nullptr) {
        return nullptr;
    }

    return type_is_a(busy, m_player_cam_td) ? busy : nullptr;
}

void RE4VRFirstPerson::force_twirler_tick() {
    if (clock_now() < m_ftw_block_until) {
        return;
    }

    const auto cam = get_player_cam();

    if (cam == nullptr) {
        // Keine Spielerkamera -> kurz Ruhe geben (sonst Exception-Flut auf
        // Objekte im Umbau).
        m_ftw.twirl = false;
        m_ftw_block_until = clock_now() + 0.20;
        return;
    }

    bool tw = false;

    if (!re4vr::try_call<bool>(cam, "get_IsForceTwirler", tw)) {
        // Controller gerade ungueltig -> progressiver Backoff 0.5/1/2 s.
        m_ftw.twirl = false;
        m_ftw_fail_streak = std::min(m_ftw_fail_streak + 1, 3);
        m_ftw_block_until = clock_now()
            + std::min(0.5 * std::pow(2.0, static_cast<double>(m_ftw_fail_streak - 1)), 2.0);
        return;
    }

    m_ftw_fail_streak = 0;
    m_ftw.twirl = tw;

    // [NUR ANZEIGE] kostet sonst einen zweiten Call pro Frame.
    if (m_ftw_ui_open) {
        m_ftw.control = re4vr::call_bool_not_false(cam, "get_IsControlEnable");
    }

    if (!m_cfg.block_force_twirler) {
        return;
    }

    if (!m_ftw.twirl) {
        return;
    }

    re4vr::call_safe<void*>(cam, "stopForceTwirler");
    m_ftw.stopped = m_ftw.stopped + 1;
}

// =====================================================================
// StreamingDummy-Hider -- Lua Z.1558-1631
// =====================================================================
void RE4VRFirstPerson::sd_clear_cache() {
    // [REF] Nur zurueckgeben, was wir auch genommen haben -- der Zaehlerstand
    // allein taugt nicht als Wachposten, die Engine kann ihn zwischendurch
    // selbst hochgesetzt haben.
    for (size_t i = 0; i < m_sd_cache.size(); ++i) {
        auto* m = m_sd_cache[i];
        const bool reffed = (i < m_sd_cache_reffed.size()) && m_sd_cache_reffed[i] != 0;

        if (reffed && m != nullptr && utility::re_managed_object::is_managed_object(m)) {
            utility::re_managed_object::release(m);
        }
    }

    m_sd_cache.clear();
    m_sd_cache_reffed.clear();
}

::REManagedObject* RE4VRFirstPerson::sd_get_scene() {
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

void RE4VRFirstPerson::sd_hide_subtree(::REManagedObject* tf) {
    if (tf == nullptr) {
        return;
    }

    const auto go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

    if (go != nullptr) {
        ::REManagedObject* const types[2] = {m_sd_mesh_t, m_sd_skin_t};

        for (auto* t : types) {
            if (t == nullptr) {
                continue;
            }

            const auto m = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", t);

            if (m == nullptr) {
                continue;
            }

            re4vr::call_safe<void*>(m, "set_DrawDefault", false);

            // [REF] Der Cache haelt die Komponenten ueber Frames hinweg.
            // [M2] Lua legt sie IMMER in die Tabelle -- auch die mit
            // referenceCount == 0. Wer sie hier verwirft, blendet sie einmal
            // aus und haelt sie danach nicht mehr jeden Frame still, obwohl die
            // Engine sie reaktiviert. Also immer aufnehmen; ge-add_ref't wird
            // nur, was sich zaehlen laesst (die sol-Heuristik).
            bool reffed = false;

            if (utility::re_managed_object::is_managed_object(m)
                && static_cast<int32_t>(m->referenceCount) > 0) {
                utility::re_managed_object::add_ref(m);
                reffed = true;
            }

            m_sd_cache.push_back(m);
            m_sd_cache_reffed.push_back(reffed ? 1 : 0);
        }
    }

    auto child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

    while (child != nullptr) {
        sd_hide_subtree(child);
        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }
}

void RE4VRFirstPerson::sd_scan() {
    const auto scene = sd_get_scene();

    if (scene == nullptr || m_sd_ctrl_t == nullptr) {
        return;
    }

    const auto comps = re4vr::call_safe<sdk::SystemArray*>(scene, "findComponents(System.Type)", m_sd_ctrl_t);

    if (comps == nullptr) {
        return;
    }

    sd_clear_cache();

    size_t n = 0;

    try {
        n = comps->get_size();
    } catch (...) {
        n = 0;
    }

    for (size_t i = 0; i < n; ++i) {
        ::REManagedObject* c = nullptr;

        try {
            c = comps->get_element(static_cast<int32_t>(i));
        } catch (...) {
            c = nullptr;
        }

        const auto go = c != nullptr ? re4vr::call_safe<::REManagedObject*>(c, "get_GameObject") : nullptr;
        const auto tf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform") : nullptr;
        sd_hide_subtree(tf);
    }
}

void RE4VRFirstPerson::sd_tick() {
    if (!m_cfg.hide_streaming_dummy) {
        return;
    }

    bool stale = false;

    for (auto* m : m_sd_cache) {
        bool valid = false;

        if (m != nullptr && utility::re_managed_object::is_managed_object(m)) {
            re4vr::try_call<bool>(m, "get_Valid", valid);
        }

        if (valid) {
            re4vr::call_safe<void*>(m, "set_DrawDefault", false);
        } else {
            stale = true;
        }
    }

    if (stale) {
        sd_clear_cache();
        m_sd_last_scan = 0.0;
    }

    const double now = clock_now();

    if ((now - m_sd_last_scan) >= SD_SCAN_INTERVAL) {
        m_sd_last_scan = now;
        sd_scan();
    }
}

// =====================================================================
// Einstiegspunkte
// =====================================================================
void RE4VRFirstPerson::on_frame() {
    re4vr::trace("RE4VRFirstPerson", "on_frame");
    // Lua Z.466: Frame-Grenze fuer die Perf-Caches. Steht VOR allen Ausstiegen.
    ++m_fp_frame;

    if (!m_types_resolved) {
        m_types_resolved = true;
        m_player_cam_td = sdk::find_type_definition(game_namespace("PlayerCameraController"));
        m_gimmickfix_cam_td = sdk::find_type_definition(game_namespace("GimmickFixCameraController"));
        m_gamepad_td = sdk::find_type_definition("via.hid.GamePad");
        m_scene_td = sdk::find_type_definition("via.SceneManager");

        if (auto* t = sdk::find_type_definition(game_namespace("StreamingDummyController"))) {
            m_sd_ctrl_t = t->get_runtime_type();
        }

        if (auto* t = sdk::find_type_definition("via.render.Mesh")) {
            m_sd_mesh_t = t->get_runtime_type();
        }

        if (auto* t = sdk::find_type_definition("via.render.SkinnedMesh")) {
            m_sd_skin_t = t->get_runtime_type();
        }

        // Lua Z.376-385: Enum-Wert zur Ladezeit, Fallback 6.
        if (auto* t = sdk::find_type_definition(game_namespace("CameraDefine.GimmickType"))) {
            for (auto f : t->get_fields()) {
                if (f == nullptr || !f->is_static()) {
                    continue;
                }

                if (std::string_view{f->get_name()} != "InstalledMachineGun") {
                    continue;
                }

                try {
                    m_turret_gimmick = *reinterpret_cast<int32_t*>(f->get_data_raw(nullptr, false));
                } catch (...) {
                }

                break;
            }
        }
    }

    force_twirler_tick();
    sd_tick();
    re4vr::clear_vm_exception();
}

void RE4VRFirstPerson::on_pre_lock_scene() {
    // [RECENTER] Killswitch-Flanke vor allem anderen.
    recenter_tick();

    // Movement: Frame-Basis VOR der Engine-Bewegung setzen.
    if (m_cfg.movement_stabilization && active()) {
        const auto body_tr = get_body_transform();

        if (body_tr != nullptr) {
            glm::vec3 p{};

            if (get_vec3(body_tr, "get_Position", p)) {
                m_move.last_player_position = p;
                m_move.has_valid_position = true;
            }
        }
    }

    compute_and_set(true); // frame_tick: EMA-Update 1x pro Game-Frame
    apply_all_event_offsets();
    apply_event5_mono();   // nur HIER, einmal pro Game-Frame
    re4vr::clear_vm_exception();
}

void RE4VRFirstPerson::on_pre_unlock_scene() {
    compute_and_set(false);
    apply_all_event_offsets();
    re4vr::clear_vm_exception();
}

void RE4VRFirstPerson::on_late_update_behavior() {
    compute_and_set(false);
    apply_all_event_offsets();
    re4vr::clear_vm_exception();
}

void RE4VRFirstPerson::on_begin_rendering() {
    compute_and_set(false);
    apply_all_event_offsets();
    re4vr::clear_vm_exception();
}

void RE4VRFirstPerson::on_update_motion() {
    apply_movement_stabilization();
    re4vr::clear_vm_exception();
}

// =====================================================================
// ImGui -- Lua Z.1634-1874
// Falle: imgui.text_colored ist in Lua ein FORMAT-String.
// =====================================================================
void RE4VRFirstPerson::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRFirstPerson", "on_draw_ui");
    if (!ImGui::TreeNode("RE4VR - FirstPerson")) {
        m_ftw_ui_open = false; // Tree zu -> get_IsControlEnable gar nicht mehr fragen
        return;                // [WIE_LUA] ohne tree_pop
    }

    m_ftw_ui_open = true;

    ImGui::Text("Root-Joint: %s   Aktiv: %s", m_head_joint != nullptr ? "ok" : "-",
                active() ? "true" : "false");

    if (ImGui::Checkbox("Erzwungene Kameraschwenks abbrechen (ForceTwirler)",
                        &m_cfg.block_force_twirler)) {
        save_cfg();
    }

    {
        char buf[192]{};
        std::snprintf(buf, sizeof(buf),
                      "   Live: IsForceTwirler = %s | IsControlEnable = %s | abgebrochen: %d",
                      m_ftw.twirl ? "true" : "false", m_ftw.control ? "true" : "false", m_ftw.stopped);
        ImGui::TextColored(abgr_to_vec4(m_ftw.twirl ? 0xFF00FFFFu : 0xFF888888u), "%s", buf);
    }

    ImGui::Separator();

    const auto slider = [this](const char* label, float* v, float step, float lo, float hi) {
        if (ImGui::DragFloat(label, v, step, lo, hi, "%.3f")) {
            save_cfg();
        }
    };

    slider("HMD Offset Right (X)", &m_cfg.off_x, 0.005f, -1.0f, 1.0f);
    slider("HMD Offset Up (Y)", &m_cfg.off_y, 0.005f, -1.0f, 2.5f);
    slider("HMD Offset Forward (Z)", &m_cfg.off_z, 0.005f, -1.0f, 1.0f);

    if (ImGui::TreeNode("Riddle-Offsets (WELT-Koordinaten, je eine feste Stage)")) {
        ImGui::Text("-- Stone-Riddle Offset (WELT-Koordinaten, NUR Stage 51503 an fixer Position) --");
        slider("Stone-Riddle HMD Offset World X", &m_cfg.event_off_x, 0.01f, -10.0f, 10.0f);
        slider("Stone-Riddle HMD Offset World Y", &m_cfg.event_off_y, 0.01f, -10.0f, 10.0f);
        slider("Stone-Riddle HMD Offset World Z", &m_cfg.event_off_z, 0.01f, -10.0f, 10.0f);

        ImGui::Text("-- Power-Riddle Offset (WELT-Koordinaten, NUR Stage 61400 an fixer Position) --");
        slider("Power-Riddle HMD Offset World X", &m_cfg.event2_off_x, 0.01f, -10.0f, 10.0f);
        slider("Power-Riddle HMD Offset World Y", &m_cfg.event2_off_y, 0.01f, -10.0f, 10.0f);
        slider("Power-Riddle HMD Offset World Z", &m_cfg.event2_off_z, 0.01f, -10.0f, 10.0f);

        ImGui::Text("-- Power-Riddle2 Offset (WELT-Koordinaten, NUR Stage 61305 an fixer Position) --");
        slider("Power-Riddle2 HMD Offset World X", &m_cfg.event3_off_x, 0.01f, -10.0f, 10.0f);
        slider("Power-Riddle2 HMD Offset World Y", &m_cfg.event3_off_y, 0.01f, -10.0f, 10.0f);
        slider("Power-Riddle2 HMD Offset World Z", &m_cfg.event3_off_z, 0.01f, -10.0f, 10.0f);

        ImGui::Text("-- Dumpster-Riddle Offset (WELT-Koordinaten, NUR Stage 63108 an fixer Position) --");
        slider("Dumpster-Riddle HMD Offset World X", &m_cfg.event4_off_x, 0.01f, -10.0f, 10.0f);
        slider("Dumpster-Riddle HMD Offset World Y", &m_cfg.event4_off_y, 0.01f, -10.0f, 10.0f);
        slider("Dumpster-Riddle HMD Offset World Z", &m_cfg.event4_off_z, 0.01f, -10.0f, 10.0f);

        ImGui::Text("-- Symbol-Riddle Offset (WELT-Koordinaten, NUR Stage 44110 an fixer Position) --");

        if (ImGui::Checkbox("Symbol-Riddle: Mono-Rendering (beide Augen dasselbe Bild)",
                            &m_cfg.event5_mono)) {
            save_cfg();
        }

        slider("Symbol-Riddle HMD Offset World X", &m_cfg.event5_off_x, 0.01f, -10.0f, 10.0f);
        slider("Symbol-Riddle HMD Offset World Y", &m_cfg.event5_off_y, 0.01f, -10.0f, 10.0f);
        slider("Symbol-Riddle HMD Offset World Z", &m_cfg.event5_off_z, 0.01f, -10.0f, 10.0f);

        ImGui::Text("-- Church-Riddle Offset (WELT-Koordinaten, NUR Stage 45401 an fixer Position) --");
        slider("Church-Riddle HMD Offset World X", &m_cfg.event6_off_x, 0.01f, -10.0f, 10.0f);
        slider("Church-Riddle HMD Offset World Y", &m_cfg.event6_off_y, 0.01f, -10.0f, 10.0f);
        slider("Church-Riddle HMD Offset World Z", &m_cfg.event6_off_z, 0.01f, -10.0f, 10.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Leon-Offsets (Nagel-Events, Minecart, Jetski/Boot)")) {
        ImGui::Text("-- Leon: ALLE Head-Nagel-Events ohne eigenen Offset "
                    "(Grapple/Kistentritt/Tritte/Ashley/Zonen) --");
        slider("Leon Nagel-Events HMD Offset Right (X)", &m_cfg.leon_evt_off_x, 0.005f, -2.0f, 2.0f);
        slider("Leon Nagel-Events HMD Offset Up (Y)", &m_cfg.leon_evt_off_y, 0.005f, -2.0f, 2.0f);
        slider("Leon Nagel-Events HMD Offset Forward (Z)", &m_cfg.leon_evt_off_z, 0.005f, -2.0f, 2.0f);

        ImGui::Text("-- Leon: NUR normaler ForceCrouch (Kriech-Gang; "
                    "40501-Durchquetsch-Event ausgenommen) --");
        slider("Leon ForceCrouch HMD Offset Right (X)", &m_cfg.leon_fc_off_x, 0.005f, -2.0f, 2.0f);
        slider("Leon ForceCrouch HMD Offset Up (Y)", &m_cfg.leon_fc_off_y, 0.005f, -2.0f, 2.0f);
        slider("Leon ForceCrouch HMD Offset Forward (Z)", &m_cfg.leon_fc_off_z, 0.005f, -2.0f, 2.0f);

        ImGui::Text("-- Minecart (Intro-KS + RailCar-Fahrt): Cam am gepinnten Head + "
                    "additiver Kart-Offset --");
        slider("Kart HMD Offset Right (X, additiv)", &m_cfg.cart_off_x, 0.005f, -1.0f, 1.0f);
        slider("Kart HMD Offset Up (Y, additiv)", &m_cfg.cart_off_y, 0.005f, -1.0f, 2.5f);
        slider("Kart HMD Offset Forward (Z, additiv)", &m_cfg.cart_off_z, 0.005f, -1.0f, 1.0f);

        ImGui::Text("-- Jetski Offset (BODY-relativ = Right/Up/Forward, "
                    "Jetski-Fahr-Stages 592xx) --");
        slider("Jetski HMD Offset Right (X)", &m_cfg.jetski_off_x, 0.005f, -2.0f, 2.0f);
        slider("Jetski HMD Offset Up (Y)", &m_cfg.jetski_off_y, 0.005f, -2.0f, 2.0f);
        slider("Jetski HMD Offset Forward (Z)", &m_cfg.jetski_off_z, 0.005f, -2.0f, 2.0f);
        slider("Boat HMD Offset Right (X)", &m_cfg.boat_off_x, 0.005f, -2.0f, 2.0f);
        slider("Boat HMD Offset Up (Y)", &m_cfg.boat_off_y, 0.005f, -2.0f, 2.0f);
        slider("Boat HMD Offset Forward (Z)", &m_cfg.boat_off_z, 0.005f, -2.0f, 2.0f);

        ImGui::Text("-- Turret Offset (WELT-Koordinaten, NUR auf der MG-Turret / "
                    "InstalledMachineGun) --");
        slider("Turret HMD Offset World X", &m_cfg.turret_off_x, 0.01f, -10.0f, 10.0f);
        slider("Turret HMD Offset World Y", &m_cfg.turret_off_y, 0.01f, -10.0f, 10.0f);
        slider("Turret HMD Offset World Z", &m_cfg.turret_off_z, 0.01f, -10.0f, 10.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Ada-Offsets (ForceCrouch, Nagel-Events)")) {
        ImGui::Text("-- Ada: ForceCrouch (enger Gang) --");
        slider("Ada ForceCrouch Offset Right (X)", &m_cfg.ada_fc_off_x, 0.005f, -2.0f, 2.0f);
        slider("Ada ForceCrouch Offset Up (Y)", &m_cfg.ada_fc_off_y, 0.005f, -2.0f, 2.0f);
        slider("Ada ForceCrouch Offset Forward (Z)", &m_cfg.ada_fc_off_z, 0.005f, -2.0f, 2.0f);

        ImGui::Text("-- Ada: ALLE Head-Nagel-Events ohne eigenen Offset "
                    "(Grapple/Kistentritt/Tritte/Ashley/Zonen) --");
        slider("Ada Nagel-Events HMD Offset Right (X)", &m_cfg.ada_box_off_x, 0.005f, -2.0f, 2.0f);
        slider("Ada Nagel-Events HMD Offset Up (Y)", &m_cfg.ada_box_off_y, 0.005f, -2.0f, 2.0f);
        slider("Ada Nagel-Events HMD Offset Forward (Z)", &m_cfg.ada_box_off_z, 0.005f, -2.0f, 2.0f);
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Ashley-Offsets (Crouch)")) {
        ImGui::Text("-- Ashley im Crouch (NUR Ashley ch0a1z0 + Crouch; "
                    "ersetzt die Basis-Offsets) --");
        slider("Ashley-Crouch HMD Offset Right (X)", &m_cfg.acrouch_off_x, 0.005f, -1.0f, 1.0f);
        slider("Ashley-Crouch HMD Offset Up (Y)", &m_cfg.acrouch_off_y, 0.005f, -1.0f, 2.5f);
        slider("Ashley-Crouch HMD Offset Forward (Z)", &m_cfg.acrouch_off_z, 0.005f, -1.0f, 1.0f);
        ImGui::TreePop();
    }

    slider("Beginning Crouch HMD Offset Right (X)", &m_cfg.begcrouch_off_x, 0.005f, -2.0f, 2.0f);
    slider("Beginning Crouch HMD Offset Up (Y)", &m_cfg.begcrouch_off_y, 0.005f, -2.0f, 2.0f);
    slider("Beginning Crouch HMD Offset Forward (Z)", &m_cfg.begcrouch_off_z, 0.005f, -2.0f, 2.0f);

    ImGui::Text("-- Event-Ende: Kamera zurueck ins Gameplay blenden --");
    ImGui::TextColored(abgr_to_vec4(0xFFAAAAAAu), "%s",
                       "   (KS2-Zonen, Leiter, Tritt, Ashley-Event -- nicht Grapple, nicht Lore)");

    if (ImGui::DragFloat("Event Ausblendzeit (s, 0=hart)", &m_cfg.headpin_fade_dur, 0.01f, 0.0f, 1.5f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Bob-Filter (s, 0=aus)", &m_cfg.bob_tau, 0.01f, 0.0f, 2.0f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Surge-Filter (s, 0=aus)", &m_cfg.surge_tau, 0.01f, 0.0f, 1.0f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::Checkbox("Crouch: Kamera weich absacken", &m_cfg.crouch_cam_lerp)) {
        save_cfg();
    }

    if (m_cfg.crouch_cam_lerp) {
        if (ImGui::DragFloat("Crouch-Kamera Lerp (s)", &m_cfg.crouch_cam_tau, 0.01f, 0.02f, 0.6f, "%.2f")) {
            save_cfg();
        }
    }

    if (ImGui::Checkbox("Aufstehen: Kamera folgt Kopf zuegig (Body nicht im Weg)",
                        &m_cfg.standup_cam_track)) {
        save_cfg();
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Movement Stabilization", &m_cfg.movement_stabilization)) {
        save_cfg();
    }

    if (ImGui::Checkbox("Movement follows HMD", &m_cfg.movement_follows_hmd)) {
        save_cfg();
    }

    ImGui::Separator();

    if (ImGui::Checkbox("StreamingDummy (pinker Cube) ausblenden", &m_cfg.hide_streaming_dummy)) {
        save_cfg();
    }

    ImGui::Text("  Dummy-Meshes gefunden: %d", static_cast<int>(m_sd_cache.size()));

    ImGui::Separator();

    if (ImGui::TreeNode("Fernglas (Zoom)")) {
        ImGui::TextColored(abgr_to_vec4(0xFF888888u), "%s",
                           "  Schiebt die Kamera entlang der Blickrichtung. + = naeher ran, - = zurueck.");
        ImGui::TextColored(abgr_to_vec4(0xFF888888u), "%s",
                           "  Gezoomt wird im Fernglas mit dem LINKEN Stick (hoch/runter).");

        const auto bino = [this](const char* label, float* v, float lo, float hi) {
            if (ImGui::DragFloat(label, v, 0.05f, lo, hi, "%.2f")) {
                publish_bino();
                save_cfg();
            }
        };

        bino("Start-Zoom beim Anlegen", &m_cfg.bino_start, -10.0f, 10.0f);
        bino("Zoom-Grenze zurueck (min)", &m_cfg.bino_min, -20.0f, 0.0f);
        bino("Zoom-Grenze vorne (max)", &m_cfg.bino_max, 0.0f, 20.0f);
        bino("Zoom-Geschwindigkeit (Stick)", &m_cfg.bino_speed, 0.1f, 15.0f);
        ImGui::TreePop();
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Recenter bei Killswitch (Headset neutralisieren)",
                        &m_cfg.recenter_on_killswitch)) {
        if (!m_cfg.recenter_on_killswitch) {
            re4vr::lua_set_bool("__vr_recenter_hold", false);
        }

        save_cfg();
    }

    if (ImGui::Button("Jetzt manuell recentern")) {
        recenter_neutralize_headset();
    }

    ImGui::TreePop();
}

#endif // RE4
