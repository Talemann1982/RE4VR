// =====================================================================
// RE4VRArmChain -- 1:1-Portierung von re4_vr_arm_chain.lua. Siehe
// RE4VRArmChain.hpp und I:\LUATRANS\PORT_ARMCHAIN_SPEC.md.
//
// Grundregel dieser Datei: KEINE Vereinfachung. Auch die drei bekannten
// Fehler des Originals sind uebernommen und an Ort und Stelle vermerkt
// ([WIE_LUA]-Kommentare). Wer hier "aufraeumt", aendert das Spielgefuehl.
// =====================================================================
#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <utility/String.hpp>

// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"

#include "RE4VR.hpp"
#include "RE4VRMinecart.hpp"
#include "RE4VRArmChain.hpp"

// windows.h macht aus min/max Makros und zerlegt jedes std::min/std::max.
// Der Fork setzt kein NOMINMAX -- deshalb nach ALLEN Includes weg damit.
#undef min
#undef max

namespace {

// =====================================================================
// Konstanten -- Werte und Namen wortgleich aus dem Lua.
// =====================================================================
constexpr float IK_REACH_SLACK = 0.018f;              // Lua Z.253, NUR im Solver
constexpr float IK_LOWER_POLE_MIX = 0.64f;            // Lua Z.255
constexpr float IK_POLE_OUTWARD_MUL = 1.0f;           // Lua Z.257
constexpr float SHOULDER_REACH_FOLLOW_SLACK = 0.018f; // Lua Z.265, NUR ausserhalb des Solvers
constexpr float ARM_SEG_CAP = 0.40f;                  // Lua Z.647
constexpr float AUTORESET_DIST = 0.15f;               // Lua Z.649
constexpr double AUTORESET_COOLDOWN = 1.0;            // Lua Z.650
constexpr float DEG2RAD = 0.0174532925f;              // Lua Z.602 (NICHT pi/180)

constexpr const char* CONFIG_FILE = "re4_vr/re4_vr_arm_chain.json"; // Lua Z.430

// Reihenfolge = RE4VRArmChain::Bone.
constexpr const char* BONE_NAMES[] = {
    "R_Hand", "R_Forearm", "R_UpperArm", "R_Shoulder",
    "L_Hand", "L_Forearm", "L_UpperArm", "L_Shoulder",
};

constexpr int PART_HAND = 0;
constexpr int PART_FOREARM = 1;
constexpr int PART_UPPERARM = 2;
constexpr int PART_SHOULDER = 3;

constexpr const char* SIDE_NAMES[] = {"R", "L"};

// Sentinel fuer "das Lua-Global existiert nicht" bei Zahlen.
constexpr double LUA_NUMBER_ABSENT = -1.0e300;

// os.clock() aus Lua. MSVC liefert mit clock() die Wanduhr seit Prozessstart,
// also dasselbe Verhalten wie die Lua-Fassung.
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// =====================================================================
// Geschuetzte Managed-Calls mit Erfolgs-Flag.
//
// WARUM NICHT re4vr::call_safe: das liefert bei Fehlschlag T{} und
// verschluckt damit den Unterschied zwischen "hat (0,0,0) geliefert" und
// "ging nicht". Das Lua unterscheidet ihn (`safe()` gibt nil zurueck), und
// an Stellen wie `if not shoulder_world then return end` haengt daran der
// ganze Solve.
//
// WARUM 16-BYTE-PUFFER: die Engine behandelt via.vec3/via.quat als 16 Byte.
// shared/sdk/RETransform.cpp benutzt fuer JEDEN Positions-Zugriff
// `__declspec(align(16)) Vector4f`. Ein 12-Byte-glm::vec3 als
// Rueckgabepuffer wuerde den Stack zerschreiben.
// =====================================================================
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

// Haengengebliebene Managed-Exception wegraeumen (wie in re4vr::call_safe).
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

// Lua Z.731: pt:call("getJointByName", name) -- der Managed-Weg, nicht der
// native Array-Scan, damit der Mechanismus derselbe bleibt.
::REJoint* get_joint_by_name(::REManagedObject* transform, const char* name) {
    if (transform == nullptr || name == nullptr) {
        return nullptr;
    }

    auto str = sdk::VM::create_managed_string(utility::widen(name));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REJoint*>(transform, "getJointByName", str);
}

// =====================================================================
// Mathematik -- Formel fuer Formel wie im Lua, inklusive Epsilon-Werten.
// =====================================================================
float clampf(float x, float lo, float hi) {
    if (x < lo) {
        return lo;
    }

    if (x > hi) {
        return hi;
    }

    return x;
}

// Lua Z.94: Laenge < 1e-8 -> nil.
std::optional<glm::vec3> vec3_normalize(const glm::vec3& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);

    if (len < 1e-8f) {
        return std::nullopt;
    }

    return glm::vec3{v.x / len, v.y / len, v.z / len};
}

float vec3_length(const glm::vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Lua Z.156: Restlaenge < 1e-5 -> nil.
std::optional<glm::vec3> vec3_reject_normalize(const glm::vec3& v, const glm::vec3& from_unit) {
    const float d = glm::dot(from_unit, v);
    const glm::vec3 r = v - from_unit * d;
    const float rl = vec3_length(r);

    if (rl < 1e-5f) {
        return std::nullopt;
    }

    return r * (1.0f / rl);
}

// Lua Z.604: Quaternion.new(Vector3f):normalized() -- der glm-Euler-
// Konstruktor. ACHTUNG: re4_vr_motion.lua benutzt fuer denselben Zweck ein
// Achsenprodukt Y*X*Z, das ist eine ANDERE Drehung.
glm::quat quat_from_euler_deg(float x, float y, float z) {
    return glm::normalize(glm::quat{glm::vec3{x * DEG2RAD, y * DEG2RAD, z * DEG2RAD}});
}

// Lua Z.195-250: Basis aus Knochenrichtung (lokales +X) und Pole-Hinweis,
// danach der klassische Vier-Fall-Trace-Zweig.
std::optional<glm::quat> quat_bone_x_along_dir(const glm::vec3& bone_dir, const glm::vec3& pole_hint) {
    const auto x_opt = vec3_normalize(bone_dir);

    if (!x_opt.has_value()) {
        return std::nullopt;
    }

    const glm::vec3 x = *x_opt;
    glm::vec3 p = vec3_normalize(pole_hint).value_or(glm::vec3{0.0f, 1.0f, 0.0f});

    glm::vec3 z = glm::cross(p, x);
    float zl = vec3_length(z);

    if (zl < 1e-5f) {
        z = glm::cross(glm::vec3{0.0f, 1.0f, 0.0f}, x);
        zl = vec3_length(z);
    }

    if (zl < 1e-5f) {
        return std::nullopt;
    }

    z = z * (1.0f / zl);

    glm::vec3 y = glm::cross(z, x);
    const float yl = vec3_length(y);

    if (yl < 1e-5f) {
        return std::nullopt;
    }

    y = y * (1.0f / yl);

    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    const float trace = m00 + m11 + m22;

    float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;

    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        qw = 0.25f / s;
        qx = (m21 - m12) * s;
        qy = (m02 - m20) * s;
        qz = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        qw = (m21 - m12) / s;
        qx = 0.25f * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25f * s;
        qz = (m12 + m21) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25f * s;
    }

    return glm::normalize(glm::quat{qw, qx, qy, qz});
}

// Lua Z.621-639.
glm::vec3 arm_pole_direction_world(const glm::quat* char_rot, bool is_right_arm) {
    const glm::vec3 world_down{0.0f, -1.0f, 0.0f};

    if (char_rot == nullptr) {
        return world_down;
    }

    const glm::vec3 char_right = *char_rot * glm::vec3{1.0f, 0.0f, 0.0f};
    const glm::vec3 char_fwd = *char_rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::vec3 char_down = *char_rot * glm::vec3{0.0f, -1.0f, 0.0f};
    const glm::vec3 char_back = char_fwd * -1.0f;
    const glm::vec3 outward = is_right_arm ? (char_right * -1.0f) : char_right;

    const float out_w = 0.46f * clampf(IK_POLE_OUTWARD_MUL, 0.0f, 2.5f);
    const glm::vec3 blend = (outward * out_w + char_down * 0.36f) + (char_back * 0.12f + world_down * 0.06f);

    return vec3_normalize(blend).value_or(char_down);
}

// Lua Z.304-369. Rueckgabe false = das Lua-"return nil, nil".
bool solve_arm_ik(const glm::vec3& shoulder_pos, const glm::vec3& hand_pos,
                  float upper_len, float lower_len, const glm::vec3& pole_vector,
                  float bend_sign, glm::vec3& out_upper_dir, glm::vec3& out_fore) {
    if (upper_len < 1e-4f || lower_len < 1e-4f) {
        return false;
    }

    const glm::vec3 w = hand_pos - shoulder_pos;
    const float dist = vec3_length(w);

    if (dist < 1e-6f) {
        return false;
    }

    float max_reach = upper_len + lower_len - IK_REACH_SLACK;

    if (max_reach < 1e-3f) {
        max_reach = upper_len + lower_len - 1e-4f;
    }

    const float reach = std::min(dist, max_reach - 1e-4f);

    if (reach < 1e-4f) {
        return false;
    }

    const auto to_target_opt = vec3_normalize(w * (1.0f / dist));

    if (!to_target_opt.has_value()) {
        return false;
    }

    const glm::vec3 to_target = *to_target_opt;
    const glm::vec3 effector = shoulder_pos + to_target * reach;

    float cos_shoulder =
        (upper_len * upper_len + reach * reach - lower_len * lower_len) / (2.0f * upper_len * reach);
    cos_shoulder = clampf(cos_shoulder, -1.0f, 1.0f);
    const float sin_shoulder = std::sqrt(std::max(0.0f, 1.0f - cos_shoulder * cos_shoulder));

    // Lua Z.327: schlaegt die Projektion fehl, wird das ROHE pole_vector genommen.
    const glm::vec3 pole_planar = vec3_reject_normalize(pole_vector, to_target).value_or(pole_vector);

    glm::vec3 n = glm::cross(pole_planar, to_target);
    float nl = vec3_length(n);

    if (nl < 1e-5f) {
        n = glm::cross(pole_planar, glm::vec3{0.0f, 1.0f, 0.0f});
        nl = vec3_length(n);
    }

    if (nl < 1e-5f) {
        return false;
    }

    n = n * (1.0f / nl);

    glm::vec3 perp = glm::cross(n, to_target);
    const float pl = vec3_length(perp);

    if (pl < 1e-5f) {
        return false;
    }

    perp = perp * (bend_sign / pl);

    // Lua Z.342-352: dirs_from_perp.
    const auto dirs_from_perp = [&](const glm::vec3& pu, glm::vec3& ud_out, glm::vec3& fr_out) -> bool {
        const auto ud = vec3_normalize(to_target * cos_shoulder + pu * sin_shoulder);

        if (!ud.has_value()) {
            return false;
        }

        const glm::vec3 elbow_guess = shoulder_pos + (*ud) * upper_len;
        const auto fr = vec3_normalize(effector - elbow_guess);

        if (!fr.has_value()) {
            return false;
        }

        ud_out = *ud;
        fr_out = *fr;
        return true;
    };

    glm::vec3 upper_dir{};
    glm::vec3 fore{};

    if (!dirs_from_perp(perp, upper_dir, fore)) {
        return false;
    }

    // Lua Z.357-366: Ellbogen zu hoch beim Griff nach unten -> andere Loesung.
    const glm::vec3 elbow_pos = shoulder_pos + upper_dir * upper_len;
    const bool reaching_low = hand_pos.y < shoulder_pos.y - 0.06f;
    const bool elbow_too_high = elbow_pos.y > shoulder_pos.y - 0.03f;

    if (reaching_low && elbow_too_high) {
        glm::vec3 ud2{};
        glm::vec3 fr2{};

        if (dirs_from_perp(perp * -1.0f, ud2, fr2)) {
            upper_dir = ud2;
            fore = fr2;
        }
    }

    out_upper_dir = upper_dir;
    out_fore = fore;
    return true;
}

// Lua Z.375-395.
bool ik_world_rotations_from_dirs(const glm::vec3& upper_dir, const glm::vec3& fore,
                                  const glm::vec3& pole_vector, float bone_axis_flip,
                                  glm::quat& out_upper, glm::quat& out_lower) {
    const glm::vec3 ud = upper_dir * bone_axis_flip;
    const glm::vec3 fd = fore * bone_axis_flip;

    const auto q_upper = quat_bone_x_along_dir(ud, pole_vector);

    if (!q_upper.has_value()) {
        return false;
    }

    glm::vec3 pole_lower = pole_vector;
    const auto elbow_axis = vec3_normalize(glm::cross(ud, fd));

    if (elbow_axis.has_value()) {
        const float w_el = clampf(IK_LOWER_POLE_MIX, 0.0f, 1.0f);
        const auto pole_mix = vec3_normalize((*elbow_axis) * w_el + pole_vector * (1.0f - w_el));

        if (pole_mix.has_value()) {
            pole_lower = *pole_mix;
        }
    }

    const auto q_lower = quat_bone_x_along_dir(fd, pole_lower);

    if (!q_lower.has_value()) {
        return false;
    }

    out_upper = *q_upper;
    out_lower = *q_lower;
    return true;
}

// Lua Z.610-619.
glm::vec3 joint_tweak_scale_vec(const RE4VRArmChain* /*unused*/, float sx, float sy, float sz) {
    return glm::vec3{std::max(sx, 0.001f), std::max(sy, 0.001f), std::max(sz, 0.001f)};
}

// Lua Z.641-644.
float segment_length_from_offset(float px, float py, float pz) {
    return std::sqrt(px * px + py * py + pz * pz);
}

// ImGui: die Lua-Farben sind ABGR (0xAABBGGRR).
ImVec4 abgr_to_vec4(uint32_t c) {
    const float r = static_cast<float>(c & 0xFF) / 255.0f;
    const float g = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    const float a = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
    return ImVec4{r, g, b, a};
}

// Lua: json-Feld als Zahl lesen, sonst der Vorgabewert (tonumber(...) or x).
float json_number(const nlohmann::json& j, const char* key, float def) {
    if (!j.is_object() || !j.contains(key)) {
        return def;
    }

    const auto& v = j[key];
    return v.is_number() ? v.get<float>() : def;
}

// Lua `x == true`: NUR der echte Boolesche Wahrwert zaehlt.
bool json_is_true(const nlohmann::json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

// Lua `if x ~= nil then`: ein JSON-null erzeugt in der Lua-Tabelle gar keinen
// Eintrag, das Feld bleibt also auf seinem Default. `contains` allein wuerde
// ein null faelschlich als "vorhanden" werten.
bool json_present(const nlohmann::json& j, const char* key) {
    return j.is_object() && j.contains(key) && !j[key].is_null();
}

// Lua `if x ~= nil then y = x end` mit anschliessender Wahrheitspruefung:
// alles ausser false und nil ist wahr.
bool json_truthy(const nlohmann::json& v) {
    if (v.is_boolean()) {
        return v.get<bool>();
    }

    return !v.is_null();
}

} // namespace

// =====================================================================
// Mod-Anbindung -- eigener Mod, in Mods.cpp HINTER dem ScriptRunner.
// =====================================================================
std::shared_ptr<RE4VRArmChain>& RE4VRArmChain::get() {
    static auto inst = std::make_shared<RE4VRArmChain>();
    return inst;
}

void RE4VRArmChain::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRArmChain", "on_lua_state_destroyed");
    on_script_reset();
}

void RE4VRArmChain::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRArmChain", "on_pre_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LockScene"_fnv) {
        on_pre_lock_scene();
    } else if (hash == "BeginRendering"_fnv) {
        on_pre_begin_rendering();
    }
}

void RE4VRArmChain::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRArmChain", "on_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        on_late_update_behavior();
    } else if (hash == "UpdateJointExpression"_fnv) {
        on_update_joint_expression();
    }
}

// =====================================================================
// Lebenszyklus
// =====================================================================
// Alle konfigurierbaren Werte auf die Lua-Defaults. In Lua passiert das
// implizit, weil "Reset Scripts" die Datei neu laedt und damit jedes local neu
// initialisiert -- unter anderem gehen dabei ungespeicherte UI-Aenderungen
// verloren. Ohne diesen Schritt wuerde der Port sie ueber den Reset retten.
void RE4VRArmChain::reset_to_defaults() {
    m_enable_arm_chain = true;                 // Lua Z.397
    m_shoulder_reach_follow = true;            // Lua Z.264
    m_shoulder_reach_follow_max = 0.45f;       // Lua Z.269
    m_hand_clamp = true;                       // Lua Z.276
    m_shoulder_pin = true;                     // Lua Z.287
    m_shoulder_pin_rel[SIDE_R].reset();        // Lua Z.288
    m_shoulder_pin_rel[SIDE_L].reset();
    m_hand_extra_rot.fill(std::nullopt);       // Fremd-Drehung (Choke) faellt weg
    m_wrist_y = {0.0f, 0.0f};                  // Lua Z.296
    m_wrist_x = {0.0f, 0.0f};                  // Lua Z.301
    m_chain_offset.fill(ChainOffset{});        // Lua Z.401-410
    m_arm_segment_enabled.fill(true);          // Lua Z.412-421
    m_current_key = "default";                 // Lua Z.431
    m_all_configs = nlohmann::json::object();  // Lua Z.432
    m_debug_sync = DebugSync{};                // Lua Z.981-998
}

std::optional<std::string> RE4VRArmChain::on_initialize() {
    clear_joint_cache();
    reset_to_defaults();

    // Lua Z.599-600: beides laeuft bei jedem Script-Load.
    load_all_configs();
    apply_key_config(resolve_config_key());

    return std::nullopt;
}

void RE4VRArmChain::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRArmChain", "on_lua_state_created");
    // =================================================================
    // [LOAD_BEARING] Lua Z.20-44. Lua 5.4 kennt math.atan2 nicht mehr, und
    // DREI Live-Scripte rufen es ungeschuetzt auf:
    //   re4_vr_choke.lua   Z.1662, 1877, 1949
    //   re4_vr_motion.lua  Z.3533
    //   re4_vr_weapons.lua Z.4352 (zweimal in einer Zeile)
    // re4_vr_arm_chain.lua war der EINZIGE Setzer. Faellt der Polyfill weg,
    // werfen die drei Scripte zur Laufzeit -- ohne dass irgendetwas auf
    // arm_chain zeigt.
    //
    // Abweichung, bewusst und gemeldet: das Original setzt den Polyfill erst
    // NACH dem Killswitch-require (Lua Z.13-18, return bei Fehlschlag).
    // Scheitert das require, ist math.atan2 schon heute nicht gesetzt. Hier
    // steht er unbedingt und frueher.
    // =================================================================
    try {
        sol::table math = lua["math"];

        if (math.valid() && !math["atan2"].valid()) {
            // std::atan2 ist rechnerisch identisch mit dem 2-Argument-math.atan,
            // das die Lua-Fassung bevorzugt, und mit ihrem Handzweig darunter.
            // Lua Z.21-22 macht `tonumber(y) or 0.0` -- ein nil-Argument
            // liefert dort einen Wert statt eines Fehlers. Mit zwei nackten
            // double-Parametern wuerde sol stattdessen einen Lua-Fehler werfen.
            math["atan2"] = [](sol::object y, sol::object x) -> double {
                const auto to_num = [](const sol::object& o) -> double {
                    if (o.is<double>()) {
                        return o.as<double>();
                    }

                    if (o.is<std::string>()) {
                        try {
                            return std::stod(o.as<std::string>());
                        } catch (...) {
                            return 0.0;
                        }
                    }

                    return 0.0;
                };

                return std::atan2(to_num(y), to_num(x));
            };
        }
    } catch (...) {
    }

    // Lua Z.1044. [WIE_LUA] Die Funktion leert den Joint-Cache NICHT --
    // siehe flush_all_arm_caches().
    try {
        lua["__vr_flush_arm_joint_cache"] = []() {};
    } catch (...) {
    }
}

void RE4VRArmChain::on_script_reset() {
    // In Lua wird die Datei nach "Reset Scripts" komplett neu geladen: Cache
    // leer, Config frisch von der Platte, erste Runde faellt aus.
    clear_joint_cache();
    m_jv_bad.clear();
    m_axis_verify_done = {false, false};
    m_cached_player_tf = nullptr;
    m_chain_first_load = true;
    m_phase_hooks_registered = false;
    m_autoreset_last_t = 0.0;
    m_debug_last_frame_applied = -1.0;
    m_debug_last_motion_tick_seen = -1.0;

    reset_to_defaults();
    load_all_configs();
    apply_key_config(resolve_config_key());
}

void RE4VRArmChain::on_frame() {
    re4vr::trace("RE4VRArmChain", "on_frame");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    // Lua Z.1301-1307: die Phasen-Hooks werden erst im ersten on_frame
    // angemeldet. Bis dahin feuert keine Phase.
    m_phase_hooks_registered = true;
}

// =====================================================================
// Konfiguration
// =====================================================================
void RE4VRArmChain::load_all_configs() {
    // Lua Z.434-442: jeder Fehler -> all_configs bleibt leer.
    m_all_configs = re4vr::json_load(CONFIG_FILE);
}

std::string RE4VRArmChain::resolve_config_key() const {
    // Lua Z.444-450.
    std::string g = re4vr::lua_get_string("__vr_active_char");

    if (!g.empty()) {
        std::transform(g.begin(), g.end(), g.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return g;
    }

    return "leon";
}

void RE4VRArmChain::apply_key_config(const std::string& resolved_key) {
    // Lua Z.452-506.
    const nlohmann::json* data = nullptr;

    if (m_all_configs.contains(resolved_key) && m_all_configs[resolved_key].is_object()) {
        data = &m_all_configs[resolved_key];
    } else if (m_all_configs.contains("default") && m_all_configs["default"].is_object()) {
        data = &m_all_configs["default"];
    } else if (m_all_configs.contains("leon") && m_all_configs["leon"].is_object()) {
        data = &m_all_configs["leon"];
    }

    if (data == nullptr) {
        // [WIE_LUA] current_key bleibt dann UNVERAENDERT -- Lua Z.454/456.
        // Folge: der Vergleich in apply_body_chain schlaegt jeden Durchlauf an.
        return;
    }

    m_current_key = resolved_key;

    if (json_present(*data, "enabled")) {
        m_enable_arm_chain = json_truthy((*data)["enabled"]);
    }

    if (data->contains("chain") && (*data)["chain"].is_object()) {
        const auto& chain = (*data)["chain"];

        for (int i = 0; i < BONE_COUNT; ++i) {
            if (!chain.contains(BONE_NAMES[i]) || !chain[BONE_NAMES[i]].is_object()) {
                continue;
            }

            // [WIE_LUA] Lua kopiert JEDEN Schluessel aus der JSON in die
            // Tabelle, auch unbekannte. Unbekannte werden nirgends gelesen,
            // deshalb hat der Port nur die neun echten Felder.
            const auto& o = chain[BONE_NAMES[i]];
            auto& off = m_chain_offset[i];
            off.pos_x = json_number(o, "pos_x", off.pos_x);
            off.pos_y = json_number(o, "pos_y", off.pos_y);
            off.pos_z = json_number(o, "pos_z", off.pos_z);
            off.rot_x = json_number(o, "rot_x", off.rot_x);
            off.rot_y = json_number(o, "rot_y", off.rot_y);
            off.rot_z = json_number(o, "rot_z", off.rot_z);
            off.scale_x = json_number(o, "scale_x", off.scale_x);
            off.scale_y = json_number(o, "scale_y", off.scale_y);
            off.scale_z = json_number(o, "scale_z", off.scale_z);
        }
    }

    if (data->contains("segments") && (*data)["segments"].is_object()) {
        const auto& segs = (*data)["segments"];

        for (int i = 0; i < BONE_COUNT; ++i) {
            if (json_present(segs, BONE_NAMES[i])) {
                m_arm_segment_enabled[i] = json_truthy(segs[BONE_NAMES[i]]);
            }
        }
    }

    // Lua Z.471-479: tonumber(...) or 0.0 -- ein fehlendes Feld wird also 0,
    // nicht der bisherige Wert.
    if (data->contains("wrist_y") && (*data)["wrist_y"].is_object()) {
        m_wrist_y[SIDE_L] = json_number((*data)["wrist_y"], "L", 0.0f);
        m_wrist_y[SIDE_R] = json_number((*data)["wrist_y"], "R", 0.0f);
    }

    if (data->contains("wrist_x") && (*data)["wrist_x"].is_object()) {
        m_wrist_x[SIDE_L] = json_number((*data)["wrist_x"], "L", 0.0f);
        m_wrist_x[SIDE_R] = json_number((*data)["wrist_x"], "R", 0.0f);
    }

    if (json_present(*data, "shoulder_reach_follow")) {
        m_shoulder_reach_follow = json_is_true(*data, "shoulder_reach_follow");
    }

    if (data->contains("shoulder_reach_follow_max") && (*data)["shoulder_reach_follow_max"].is_number()) {
        m_shoulder_reach_follow_max = (*data)["shoulder_reach_follow_max"].get<float>();
    }

    if (json_present(*data, "hand_clamp")) {
        m_hand_clamp = json_is_true(*data, "hand_clamp");
    }

    if (json_present(*data, "shoulder_pin")) {
        m_shoulder_pin = json_is_true(*data, "shoulder_pin");
    }

    if (data->contains("shoulder_pin_pose") && (*data)["shoulder_pin_pose"].is_object()) {
        const auto& pose = (*data)["shoulder_pin_pose"];

        for (int s = 0; s < SIDE_COUNT; ++s) {
            const char* key = SIDE_NAMES[s];

            if (!pose.contains(key) || !pose[key].is_object()) {
                continue;
            }

            const auto& p = pose[key];

            // [WIE_LUA] Lua prueft NUR px und rw auf "number" (Z.496); die
            // uebrigen Felder gehen ungeprueft in die Konstruktoren. Hier
            // werden sie mit 0 vorbelegt, damit ein halbes JSON nicht crasht.
            if (!p.contains("px") || !p["px"].is_number()) {
                continue;
            }

            if (!p.contains("rw") || !p["rw"].is_number()) {
                continue;
            }

            PinPose rel{};
            rel.p = glm::vec3{json_number(p, "px", 0.0f), json_number(p, "py", 0.0f),
                              json_number(p, "pz", 0.0f)};
            rel.r = glm::quat{json_number(p, "rw", 1.0f), json_number(p, "rx", 0.0f),
                              json_number(p, "ry", 0.0f), json_number(p, "rz", 0.0f)};
            m_shoulder_pin_rel[s] = rel;
        }
    }
}

void RE4VRArmChain::save_config() {
    // Lua Z.559-597: speichert unter resolve_config_key(), NICHT unter current_key.
    const std::string save_key = resolve_config_key();

    nlohmann::json chain = nlohmann::json::object();
    nlohmann::json segments = nlohmann::json::object();

    for (int i = 0; i < BONE_COUNT; ++i) {
        const auto& o = m_chain_offset[i];
        chain[BONE_NAMES[i]] = {
            {"pos_x", o.pos_x}, {"pos_y", o.pos_y}, {"pos_z", o.pos_z},
            {"rot_x", o.rot_x}, {"rot_y", o.rot_y}, {"rot_z", o.rot_z},
            {"scale_x", o.scale_x}, {"scale_y", o.scale_y}, {"scale_z", o.scale_z},
        };
        segments[BONE_NAMES[i]] = m_arm_segment_enabled[i];
    }

    nlohmann::json pin_pose = nlohmann::json::object();

    for (int s = 0; s < SIDE_COUNT; ++s) {
        if (!m_shoulder_pin_rel[s].has_value()) {
            continue;
        }

        const auto& r = *m_shoulder_pin_rel[s];
        pin_pose[SIDE_NAMES[s]] = {
            {"px", r.p.x}, {"py", r.p.y}, {"pz", r.p.z},
            {"rw", r.r.w}, {"rx", r.r.x}, {"ry", r.r.y}, {"rz", r.r.z},
        };
    }

    nlohmann::json entry = nlohmann::json::object();
    entry["enabled"] = m_enable_arm_chain;
    entry["chain"] = chain;
    entry["segments"] = segments;
    entry["wrist_y"] = {{"L", m_wrist_y[SIDE_L]}, {"R", m_wrist_y[SIDE_R]}};
    entry["wrist_x"] = {{"L", m_wrist_x[SIDE_L]}, {"R", m_wrist_x[SIDE_R]}};
    entry["shoulder_reach_follow"] = m_shoulder_reach_follow;
    entry["shoulder_reach_follow_max"] = m_shoulder_reach_follow_max;
    entry["hand_clamp"] = m_hand_clamp;
    entry["shoulder_pin"] = m_shoulder_pin;
    entry["shoulder_pin_pose"] = pin_pose;

    if (!m_all_configs.is_object()) {
        m_all_configs = nlohmann::json::object();
    }

    m_all_configs[save_key] = entry;

    // Indent -1: json.dump_string schreibt in Lua kompakt (Json.cpp Z.114).
    re4vr::json_save(CONFIG_FILE, m_all_configs, -1);
}

// =====================================================================
// Spielzustand
// =====================================================================
::REManagedObject* RE4VRArmChain::get_player_transform() {
    // Lua Z.52-60. Kein Cache -- genau wie im Original.
    const auto char_mgr = sdk::get_managed_singleton<::REManagedObject>(game_namespace("CharacterManager"));

    if (char_mgr == nullptr) {
        return nullptr;
    }

    const auto ctx = re4vr::call_safe<::REManagedObject*>(char_mgr, "getPlayerContextRef");

    if (ctx == nullptr) {
        return nullptr;
    }

    const auto body_go = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body_go == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(body_go, "get_Transform");
}

RE4VRArmChain::RootPose RE4VRArmChain::get_player_root_pose(::REManagedObject* player_tf) {
    // Lua Z.62-76.
    RootPose out{};

    if (player_tf == nullptr) {
        return out;
    }

    glm::vec3 pos{};
    glm::quat rot{};

    if (!get_vec3(player_tf, "get_Position", pos)) {
        return out;
    }

    if (!get_quat(player_tf, "get_Rotation", rot)) {
        return out;
    }

    out.pos = pos;
    out.rot = rot;
    out.inv_rot = glm::inverse(rot);
    out.inv_valid = true;
    out.parented = re4vr::call_safe<::REManagedObject*>(player_tf, "get_Parent") != nullptr;
    out.valid = true;
    return out;
}

// =====================================================================
// Joints
// =====================================================================
bool RE4VRArmChain::is_joint_valid(::REJoint* joint) {
    // Lua Z.689-701 inklusive der 0,5-s-Drosselung gegen die Log-Flut.
    if (joint == nullptr) {
        return false;
    }

    const auto addr = reinterpret_cast<uintptr_t>(joint);
    const double now = clock_now();

    const auto it = m_jv_bad.find(addr);

    if (it != m_jv_bad.end() && (now - it->second) < 0.5) {
        return false;
    }

    // [REF] Dieselbe Wache, die der Lua-Weg hat: sol_lua_push prueft jedes
    // Engine-Objekt mit is_managed_object, BEVOR es in Lua landet
    // (Sdk.cpp Z.54). get_vec3 wuerde sonst ueber get_type_definition auf
    // freigegebenem Speicher landen -- das wird keine Exception, das wird eine
    // Access Violation.
    if (!utility::re_managed_object::is_managed_object(joint)) {
        m_jv_bad[addr] = now;
        return false;
    }

    glm::vec3 dummy{};
    const bool ok = get_vec3(reinterpret_cast<::REManagedObject*>(joint), "get_Position", dummy);

    if (ok) {
        m_jv_bad.erase(addr);
    } else {
        m_jv_bad[addr] = now;
    }

    return ok;
}

::REJoint* RE4VRArmChain::get_chain_joint(int bone) {
    // Lua Z.725-733.
    if (m_chain_joints[bone] != nullptr && is_joint_valid(m_chain_joints[bone])) {
        return m_chain_joints[bone];
    }

    const auto pt = get_player_transform();

    if (pt == nullptr) {
        // [WIE_LUA] Der Cache-Eintrag bleibt hier stehen, er wird NICHT genullt.
        return nullptr;
    }

    store_joint(bone, get_joint_by_name(pt, BONE_NAMES[bone]));
    return m_chain_joints[bone];
}

// =====================================================================
// [REF] Joint-Handles referenzzaehlen -- exakt das, was der Lua-Weg umsonst
// mitbringt.
//
// In Lua kommt jeder von getJointByName zurueckgegebene REJoint* durch
// sol_lua_push -> api::re_managed_object::detail::add_ref (Sdk.cpp Z.49-68):
// das Objekt wird auf is_managed_object geprueft UND ref-counted (wenn sein
// referenceCount > 0 ist). Ein gecachter Joint bleibt dadurch am Leben,
// solange der Cache-Eintrag lebt, und is_joint_valid bekommt bei einem toten
// Joint sauber eine Engine-Exception statt einer Access Violation.
//
// Der Port muss das nachbauen, denn flush_all_arm_caches ist 1:1 als No-Op
// portiert: ein Eintrag ueberlebt Charakter- und Levelwechsel und wird real
// nur an zwei Stellen geleert.
// =====================================================================
void RE4VRArmChain::store_joint(int bone, ::REJoint* joint) {
    const auto old = m_chain_joints[bone];

    if (old == joint) {
        return;
    }

    if (old != nullptr && m_chain_joints_reffed[bone]) {
        utility::re_managed_object::release(reinterpret_cast<::REManagedObject*>(old));
    }

    m_chain_joints[bone] = nullptr;
    m_chain_joints_reffed[bone] = false;

    if (joint == nullptr) {
        return;
    }

    // Wie sol_lua_push: erst pruefen, dann zaehlen -- und zaehlen nur bei
    // referenceCount > 0 ("local objects seem buggy", Sdk.cpp Z.65).
    if (!utility::re_managed_object::is_managed_object(joint)) {
        return;
    }

    auto obj = reinterpret_cast<::REManagedObject*>(joint);

    if (static_cast<int32_t>(obj->referenceCount) > 0) {
        utility::re_managed_object::add_ref(obj);
        m_chain_joints_reffed[bone] = true;
    }

    m_chain_joints[bone] = joint;
}

// Der ECHTE Cache-Flush. Lua erreicht ihn nur an zwei Stellen (Z.1088 und
// Z.1096) -- flush_all_arm_caches() gehoert ausdruecklich NICHT dazu.
void RE4VRArmChain::clear_joint_cache() {
    for (int i = 0; i < BONE_COUNT; ++i) {
        store_joint(i, nullptr);
    }
}

void RE4VRArmChain::flush_all_arm_caches() {
    // =================================================================
    // [WIE_LUA -- FEHLER 1:1 UEBERNOMMEN] Lua Z.423-426 schreibt
    // `chain_joints = {}`, aber `local chain_joints` steht erst in Z.428.
    // Die Zuweisung trifft damit das GLOBALE chain_joints, nicht den
    // spaeter benutzten Local -- die Funktion leert den Joint-Cache also
    // NICHT. Betroffen sind alle vier Bezugsstellen: check_player_changed,
    // check_autoreset, _G.__vr_flush_arm_joint_cache und
    // motion_exports.clear_joint_cache.
    //
    // Real geleert wird der Cache nur an zwei Stellen, beide in
    // apply_body_chain: chain_first_load (Lua Z.1088) und Key-Wechsel
    // (Lua Z.1096). Wer diese Funktion "repariert", aendert das Verhalten
    // beim Charakterwechsel und beim Autoreset.
    // =================================================================
}

void RE4VRArmChain::apply_ik_rotation_to_joint(int bone, const glm::quat& world_rot, ::REJoint* joint) {
    // Lua Z.735-774.
    if (joint == nullptr) {
        joint = get_chain_joint(bone);
    }

    if (joint == nullptr) {
        return;
    }

    const auto& off = m_chain_offset[bone];
    auto obj = reinterpret_cast<::REManagedObject*>(joint);

    std::optional<glm::quat> rot_off;

    if (off.rot_x != 0.0f || off.rot_y != 0.0f || off.rot_z != 0.0f) {
        rot_off = quat_from_euler_deg(off.rot_x, off.rot_y, off.rot_z);
    }

    const auto parent = re4vr::call_safe<::REManagedObject*>(obj, "get_Parent");

    if (parent != nullptr) {
        glm::quat parent_rot{};

        if (get_quat(parent, "get_Rotation", parent_rot)) {
            glm::quat local_rot = glm::normalize(glm::inverse(parent_rot) * world_rot);

            if (rot_off.has_value()) {
                local_rot = glm::normalize(local_rot * (*rot_off));
            }

            set_quat(obj, "set_LocalRotation", local_rot);
        }
    } else {
        glm::quat wr = world_rot;

        if (rot_off.has_value()) {
            wr = glm::normalize(world_rot * (*rot_off));
        }

        set_quat(obj, "set_Rotation", wr);
    }

    // Lua Z.772-773: laeuft IMMER, auch wenn oben nichts geschrieben wurde.
    set_vec3(obj, "set_LocalScale",
             joint_tweak_scale_vec(this, off.scale_x, off.scale_y, off.scale_z));
}

void RE4VRArmChain::apply_shoulder_pin(Side side) {
    // Lua Z.779-800.
    if (!m_shoulder_pin) {
        // [WIE_LUA] Loescht die aus der Config geladene Pose.
        m_shoulder_pin_rel[side].reset();
        return;
    }

    const auto sj = get_chain_joint(bone_of(side, PART_SHOULDER));

    if (sj == nullptr) {
        return;
    }

    if (!m_shoulder_pin_rel[side].has_value()) {
        // Capture passiert im Solve, nicht hier (Lua Z.788-793).
        return;
    }

    const auto& rel = *m_shoulder_pin_rel[side];
    auto obj = reinterpret_cast<::REManagedObject*>(sj);

    // Lua: beide Writes stehen in EINEM pcall -- wirft der erste, unterbleibt
    // der zweite.
    if (set_vec3(obj, "set_LocalPosition", rel.p)) {
        set_quat(obj, "set_LocalRotation", rel.r);
    }
}

void RE4VRArmChain::arm_ik_segment_lengths(int bone_upper, int bone_lower,
                                           float& upper_len, float& lower_len) const {
    // Lua Z.658-664: NUR aus der Config, nie aus Live-Knochen.
    const auto& u = m_chain_offset[bone_upper];
    const auto& l = m_chain_offset[bone_lower];

    const float u_json = segment_length_from_offset(u.pos_x, u.pos_y, u.pos_z);
    const float l_json = segment_length_from_offset(l.pos_x, l.pos_y, l.pos_z);

    upper_len = std::min(std::max(std::max(u_json, 0.26f), 0.24f), ARM_SEG_CAP);
    lower_len = std::min(std::max(std::max(l_json, 0.24f), 0.22f), ARM_SEG_CAP);
}

void RE4VRArmChain::maybe_log_bone_axis_once(Side side, ::REJoint* upper_joint,
                                             ::REJoint* lower_joint, bool have_upper_dir) {
    // Lua Z.705-723. Rechnet und verwirft -- der if/else-Zweig ist leer.
    // Uebrig bleibt genau ein Effekt: das Merken pro Seite.
    if (!re4vr::lua_get_bool("__vr_re4_arm_chain_verify_bone_axis", false)) {
        return;
    }

    if (m_axis_verify_done[side]) {
        return;
    }

    if (upper_joint == nullptr || lower_joint == nullptr || !have_upper_dir) {
        return;
    }

    glm::quat urot{};
    glm::vec3 p0{};
    glm::vec3 p1{};

    if (!get_quat(reinterpret_cast<::REManagedObject*>(upper_joint), "get_Rotation", urot)) {
        return;
    }

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(upper_joint), "get_Position", p0)) {
        return;
    }

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(lower_joint), "get_Position", p1)) {
        return;
    }

    m_axis_verify_done[side] = true;
}

// =====================================================================
// Der Solve pro Seite -- Lua Z.802-975
// =====================================================================
void RE4VRArmChain::apply_arm_ik_side(Side side, glm::vec3 hand_pos, const glm::quat* char_rot,
                                      const RootPose& root_pose) {
    const int bone_upper = bone_of(side, PART_UPPERARM);
    const int bone_lower = bone_of(side, PART_FOREARM);
    const int bone_hand = bone_of(side, PART_HAND);
    const bool is_right = (side == SIDE_R);

    // 1. Lua Z.806-808.
    if (!m_arm_segment_enabled[bone_upper] && !m_arm_segment_enabled[bone_lower]) {
        return;
    }

    // 2. [WRIST_Y] Lua Z.812-815.
    const float wy = m_wrist_y[side];

    if (wy != 0.0f) {
        hand_pos = glm::vec3{hand_pos.x, hand_pos.y + wy, hand_pos.z};
    }

    // 3. [WRIST_X] Lua Z.818-824, char-relativ.
    const float wx = m_wrist_x[side];

    if (wx != 0.0f) {
        const glm::vec3 off_world = (char_rot != nullptr) ? (*char_rot * glm::vec3{wx, 0.0f, 0.0f})
                                                          : glm::vec3{wx, 0.0f, 0.0f};
        hand_pos = hand_pos + off_world;
    }

    // 4. Lua Z.826-829. hand_joint wird nicht benutzt, das Aufloesen ist aber
    //    LOAD-BEARING: nur so liegt <p>_Hand im Cache, den check_autoreset
    //    direkt liest.
    const auto upper_joint = get_chain_joint(bone_upper);
    const auto lower_joint = get_chain_joint(bone_lower);
    (void)get_chain_joint(bone_hand);

    if (upper_joint == nullptr) {
        return;
    }

    // 5. [SHOULDER_PIN] vor dem Lesen der Schulterposition (Lua Z.833).
    apply_shoulder_pin(side);

    // 6. Lua Z.835: die IK-"Schulter" ist der UpperArm-Joint.
    glm::vec3 shoulder_world{};

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(upper_joint), "get_Position", shoulder_world)) {
        return;
    }

    // 7.
    float upper_len = 0.0f;
    float lower_len = 0.0f;
    arm_ik_segment_lengths(bone_upper, bone_lower, upper_len, lower_len);

    // 8. [HAND_CLAMP] Anker fuer motion veroeffentlichen (Lua Z.846-854).
    char gname[64]{};

    if (m_hand_clamp) {
        const float maxreach = upper_len + lower_len - SHOULDER_REACH_FOLLOW_SLACK +
                               (m_shoulder_reach_follow ? m_shoulder_reach_follow_max : 0.0f);

        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_root", SIDE_NAMES[side]);
        re4vr::lua_set_vec3(gname, shoulder_world);
        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_maxreach", SIDE_NAMES[side]);
        re4vr::lua_set_number(gname, maxreach);
    } else {
        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_root", SIDE_NAMES[side]);
        re4vr::lua_set_nil(gname);
        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_maxreach", SIDE_NAMES[side]);
        re4vr::lua_set_nil(gname);
    }

    // 9. [SHOULDER_PIN] Capture-Slot (Lua Z.859-872).
    if (m_shoulder_pin && !m_shoulder_pin_rel[side].has_value()) {
        const float max_r0 = upper_len + lower_len - SHOULDER_REACH_FOLLOW_SLACK;
        const float d0 = vec3_length(hand_pos - shoulder_world);

        std::string anim = re4vr::lua_get_string("__vr_anim_l0");

        if (!anim.empty()) {
            std::transform(anim.begin(), anim.end(), anim.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (anim.find("stand") != std::string::npos && max_r0 > 0.05f && d0 < max_r0) {
                const auto sj = get_chain_joint(bone_of(side, PART_SHOULDER));

                if (sj != nullptr) {
                    auto sobj = reinterpret_cast<::REManagedObject*>(sj);
                    glm::vec3 lp{};
                    glm::quat lr{};

                    if (get_vec3(sobj, "get_LocalPosition", lp) &&
                        get_quat(sobj, "get_LocalRotation", lr)) {
                        PinPose rel{};
                        rel.p = lp;
                        rel.r = lr;
                        m_shoulder_pin_rel[side] = rel;
                        save_config(); // Lua Z.869: Pose persistent
                    }
                }
            }
        }
    }

    const bool railcar = re4vr::lua_get_bool("__re4_railcar_mode", false);

    // 10. [SHOULDER_REACH_FOLLOW] Lua Z.876-898.
    if (m_shoulder_reach_follow) {
        const float max_r = upper_len + lower_len - SHOULDER_REACH_FOLLOW_SLACK;
        const glm::vec3 w = hand_pos - shoulder_world;
        const float d = vec3_length(w);

        if (max_r > 0.05f && d > max_r) {
            const auto dir = vec3_normalize(w);

            if (dir.has_value()) {
                const auto sj = get_chain_joint(bone_of(side, PART_SHOULDER));
                glm::vec3 sp{};

                if (sj != nullptr &&
                    get_vec3(reinterpret_cast<::REManagedObject*>(sj), "get_Position", sp)) {
                    float excess = d - max_r;

                    // [RAILCAR] Auf dem Cart die Schulter VOLL mitgehen lassen.
                    if (!railcar && excess > m_shoulder_reach_follow_max) {
                        excess = m_shoulder_reach_follow_max;
                    }

                    const glm::vec3 delta = (*dir) * excess;
                    set_vec3(reinterpret_cast<::REManagedObject*>(sj), "set_Position", sp + delta);
                    shoulder_world = shoulder_world + delta;
                }
            }
        }
    }

    // 11. [HAND_CLAMP] Lua Z.904-912.
    if (m_hand_clamp && !railcar) {
        const float max_r = upper_len + lower_len - SHOULDER_REACH_FOLLOW_SLACK;
        const glm::vec3 w = hand_pos - shoulder_world;
        const float d = vec3_length(w);

        if (max_r > 0.05f && d > max_r + 1e-4f) {
            const auto dir = vec3_normalize(w);

            if (dir.has_value()) {
                hand_pos = shoulder_world + (*dir) * max_r;
            }
        }
    }

    // 12. Local-Space-Solve, wenn der Spieler-Root dynamisch geparentet ist
    //     (Aufzug, Gondel). Lua Z.917-930.
    const bool do_local_space = (root_pose.valid && root_pose.parented && root_pose.inv_valid);

    glm::vec3 shoulder_solve = shoulder_world;
    glm::vec3 hand_solve = hand_pos;

    if (do_local_space) {
        shoulder_solve = root_pose.inv_rot * (shoulder_world - root_pose.pos);
        hand_solve = root_pose.inv_rot * (hand_pos - root_pose.pos);
    }

    // 13. Lua Z.934-939. [WIE_LUA] Im Local-Space-Fall wird ein LOKALER
    //     Vektor veroeffentlicht, kein Weltvektor.
    const glm::vec3 hand_pos_clamped = hand_solve;
    re4vr::lua_set_vec3(is_right ? "__vr_arm_chain_rh_clamped_pos" : "__vr_arm_chain_lh_clamped_pos",
                        hand_pos_clamped);

    // 14. Pole. pole_smoothed ist an beiden Aufrufstellen nil, der
    //     else-Zweig des Originals ist also unerreichbar.
    glm::vec3 pole_vec = arm_pole_direction_world(char_rot, is_right);

    if (do_local_space) {
        pole_vec = root_pose.inv_rot * pole_vec;
    }

    constexpr float bend_sign = -1.0f;
    const float bone_axis_flip = is_right ? -1.0f : 1.0f;

    // 15.
    glm::vec3 upper_dir{};
    glm::vec3 fore_dir{};

    if (!solve_arm_ik(shoulder_solve, hand_pos_clamped, upper_len, lower_len, pole_vec, bend_sign,
                      upper_dir, fore_dir)) {
        return;
    }

    // 16. Zurueck in den Weltraum.
    if (do_local_space) {
        upper_dir = root_pose.rot * upper_dir;
        fore_dir = root_pose.rot * fore_dir;
        pole_vec = root_pose.rot * pole_vec;
    }

    // 17.
    maybe_log_bone_axis_once(side, upper_joint, lower_joint, true);

    // 18.
    glm::quat q_upper{};
    glm::quat q_lower{};

    if (!ik_world_rotations_from_dirs(upper_dir, fore_dir, pole_vec, bone_axis_flip, q_upper, q_lower)) {
        return;
    }

    // 19./20.
    if (m_arm_segment_enabled[bone_upper]) {
        apply_ik_rotation_to_joint(bone_upper, q_upper, upper_joint);
    }

    if (m_arm_segment_enabled[bone_lower]) {
        apply_ik_rotation_to_joint(bone_lower, q_lower, lower_joint);
    }
}

// =====================================================================
// Ablauf
// =====================================================================
bool RE4VRArmChain::should_pause() const {
    // Lua Z.1003-1012.
    // [RAILCAR] Auf dem Schienenwagen laeuft arm_chain TROTZ Killswitch
    // weiter -- ausser waehrend eines nativen Reloads.
    if (re4vr::lua_get_bool("__re4_railcar_mode", false)) {
        if (!re4vr::lua_get_bool("__re4_railcar_reloading", false)) {
            return false;
        }
    }

    // ks.is_active() aus re4vr/re4_vr_killswitch.lua. Fehlt das Modul, wird
    // pausiert -- in Lua kaeme das Script ohne den require gar nicht erst hoch.
    if (re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", true)) {
        return true;
    }

    if (re4vr::lua_get_bool("__vr_motion_paused", false)) {
        return true;
    }

    return false;
}

bool RE4VRArmChain::should_apply_body_chain_now() {
    // Lua Z.1046-1074. Beide Optionen sind default aus.
    // Lua liest das Global NUR in den beiden Debug-Zweigen (Z.1048/1061), und
    // beide sind default aus -- also auch hier erst bei Bedarf nachsehen, sonst
    // kostet jede der vier Phasen einen sol-Lookup umsonst.
    if (!m_debug_sync.require_motion_tick && !m_debug_sync.apply_once_per_frame) {
        return true;
    }

    const double mt_raw = re4vr::lua_get_number("__vr_motion_tick_id", LUA_NUMBER_ABSENT);
    const bool mt_present = (mt_raw != LUA_NUMBER_ABSENT);
    const double mt = mt_present ? mt_raw : -1.0;

    if (m_debug_sync.require_motion_tick) {
        if (mt <= m_debug_last_motion_tick_seen) {
            return false;
        }
    }

    if (m_debug_sync.apply_once_per_frame) {
        // [WIE_LUA] re.get_frame_count gibt es im Fork nicht (die re-Tabelle in
        // ScriptRunner.cpp kennt es nicht), und __vr_motion_tick_id hat keinen
        // Setzer. Der Wert ist damit IMMER last + 1 -- Option B blockt nie.
        double f = -1.0;

        if (f == -1.0) {
            f = mt_present ? mt_raw : (m_debug_last_frame_applied + 1.0);
        }

        if (f == m_debug_last_frame_applied) {
            return false;
        }

        m_debug_last_frame_applied = f;
    }

    if (m_debug_sync.require_motion_tick) {
        m_debug_last_motion_tick_seen = mt_present ? mt_raw : m_debug_last_motion_tick_seen;
    }

    return true;
}

void RE4VRArmChain::check_player_changed() {
    // Lua Z.1017-1024.
    const auto tf = get_player_transform();

    if (tf == nullptr) {
        return;
    }

    if (m_cached_player_tf != nullptr && tf != m_cached_player_tf) {
        flush_all_arm_caches(); // [WIE_LUA] wirkungslos
    }

    m_cached_player_tf = tf;
}

void RE4VRArmChain::check_autoreset(Side side, const glm::vec3& hand_target) {
    // Lua Z.1026-1042. EIN gemeinsamer Zaehler fuer beide Seiten.
    const double now = clock_now();

    if ((now - m_autoreset_last_t) < AUTORESET_COOLDOWN) {
        return;
    }

    // Lua liest hier DIREKT aus chain_joints, nicht ueber get_chain_joint.
    const auto hand_joint = m_chain_joints[bone_of(side, PART_HAND)];

    if (hand_joint == nullptr) {
        return;
    }

    glm::vec3 hand_actual{};

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(hand_joint), "get_Position", hand_actual)) {
        return;
    }

    const float dx = hand_actual.x - hand_target.x;
    const float dy = hand_actual.y - hand_target.y;
    const float dz = hand_actual.z - hand_target.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (dist > AUTORESET_DIST) {
        flush_all_arm_caches(); // [WIE_LUA] wirkungslos
        m_autoreset_last_t = now;
    }
}

void RE4VRArmChain::apply_body_chain() {
    // Lua Z.1076-1185.
    re4vr::lua_set_bool("__vr_re4_two_bone_ik_active", false);

    if (!m_enable_arm_chain) {
        return;
    }

    if (should_pause()) {
        return;
    }

    if (!should_apply_body_chain_now()) {
        return;
    }

    if (m_chain_first_load) {
        m_chain_first_load = false;
        clear_joint_cache();
        return;
    }

    check_player_changed();

    const std::string new_key = resolve_config_key();

    if (new_key != m_current_key) {
        clear_joint_cache();
        apply_key_config(new_key);
    }

    const auto player_tf = get_player_transform();

    if (player_tf == nullptr) {
        return;
    }

    re4vr::lua_set_bool("__vr_re4_two_bone_ik_active", true);

    const RootPose root_pose = get_player_root_pose(player_tf);

    glm::quat char_rot_val{};
    const bool have_char_rot = get_quat(player_tf, "get_Rotation", char_rot_val);
    const glm::quat* char_rot = have_char_rot ? &char_rot_val : nullptr;

    // Rechte Seite (Lua Z.1121-1126).
    const auto rh_pos = re4vr::lua_get_vec3_any({"__vr_rh_joint_pos", "__vr_unified_rh_pos", "__vr_rh_world"});

    if (rh_pos.has_value()) {
        apply_arm_ik_side(SIDE_R, *rh_pos, char_rot, root_pose);
        check_autoreset(SIDE_R, *rh_pos);
    }

    // Linke Seite (Lua Z.1128-1180).
    const auto lh_raw = re4vr::lua_get_vec3_any({"__vr_lh_joint_pos", "__vr_unified_lh_pos", "__vr_lh_world"});
    std::optional<glm::vec3> lh_pos = lh_raw;

    {
        // [SLIDE_DOCK] / [LDOCK_RH] Lua Z.1134-1176.
        auto dock_pos = re4vr::lua_get_vec3("__vr_slide_hand_world_pos");
        const double sblend = re4vr::lua_get_number("__vr_slide_dock_blend_factor", 0.0);

        if (dock_pos.has_value() && sblend > 0.001) {
            const auto rh_rot = re4vr::lua_get_quat("__vr_rh_joint_rot");

            if (rh_pos.has_value() && rh_rot.has_value()) {
                const auto prev = re4vr::lua_get_vec3("__ldock_rhprev");
                double moved = 999.0;

                if (prev.has_value()) {
                    const float dx = rh_pos->x - prev->x;
                    const float dy = rh_pos->y - prev->y;
                    const float dz = rh_pos->z - prev->z;
                    moved = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
                }

                re4vr::lua_set_vec3("__ldock_rhprev", *rh_pos);

                auto ldock_off = re4vr::lua_get_vec3("__ldock_off");

                // Offset jeden Frame neu merken, solange die R-Hand ~ruhig ist.
                if (!ldock_off.has_value() || moved < 0.006) {
                    const glm::quat inv = glm::inverse(*rh_rot);
                    const glm::vec3 no = inv * glm::vec3{dock_pos->x - rh_pos->x, dock_pos->y - rh_pos->y,
                                                         dock_pos->z - rh_pos->z};
                    re4vr::lua_set_vec3("__ldock_off", no);
                    ldock_off = no;
                }

                if (ldock_off.has_value()) {
                    const glm::vec3 wo = (*rh_rot) * (*ldock_off);
                    dock_pos = glm::vec3{rh_pos->x + wo.x, rh_pos->y + wo.y, rh_pos->z + wo.z};
                }
            }

            if (lh_raw.has_value() && sblend < 0.999) {
                const float b = static_cast<float>(sblend);
                lh_pos = glm::vec3{lh_raw->x + (dock_pos->x - lh_raw->x) * b,
                                   lh_raw->y + (dock_pos->y - lh_raw->y) * b,
                                   lh_raw->z + (dock_pos->z - lh_raw->z) * b};
            } else {
                lh_pos = dock_pos;
            }

            // [LDOCK_RH] Das FERTIG gelerpte Ziel veroeffentlichen -- motion
            // uebernimmt es 1:1, sonst lerpen beide aus verschiedenen
            // Startpunkten und es flackert.
            re4vr::lua_set_vec3("__vr_ldock_anchored", *lh_pos);
        }
    }

    if (lh_pos.has_value()) {
        apply_arm_ik_side(SIDE_L, *lh_pos, char_rot, root_pose);
        check_autoreset(SIDE_L, *lh_pos);
    }

    // [HAND_REPIN] Ganz am Ende: nichts darf die Hand nach dem IK mehr
    // verschieben (Lua Z.1113-1119, 1183-1184).
    const auto repin_hand = [this](Side side, const glm::vec3& pos, const std::optional<glm::quat>& rot) {
        const auto hj = get_chain_joint(bone_of(side, PART_HAND));

        if (hj == nullptr) {
            return;
        }

        auto obj = reinterpret_cast<::REManagedObject*>(hj);
        set_vec3(obj, "set_Position", pos);

        if (rot.has_value()) {
            // [FREMD-DREHUNG 2026-09-12] Die Zusatzdrehung eines anderen Moduls
            // (derzeit: Choke am Tiergriff) kommt RECHTS dazu, also um die
            // eigenen Achsen der Hand -- dieselbe Bedeutung wie ein additives
            // set_LocalRotation, nur an der einzigen Stelle, die ueberlebt.
            const auto& ex = m_hand_extra_rot[side];

            set_quat(obj, "set_Rotation",
                     ex.has_value() ? glm::normalize((*rot) * (*ex)) : *rot);
        }
    };

    if (rh_pos.has_value()) {
        repin_hand(SIDE_R, *rh_pos, re4vr::lua_get_quat("__vr_rh_joint_rot"));
    }

    if (lh_pos.has_value()) {
        repin_hand(SIDE_L, *lh_pos, re4vr::lua_get_quat("__vr_lh_joint_rot"));
    }
}

void RE4VRArmChain::publish_clamp_anchors() {
    // [RESUME_CLAMP] Lua Z.1195-1213: auch waehrend der Pause die Anker frisch
    // halten, sonst schnappt die Hand beim Killswitch-Austritt einen Frame lang
    // 1-2 m raus. Billig: nur get_Position + Config-Laengen, KEIN IK-Solve.
    char gname[64]{};

    if (!m_hand_clamp) {
        re4vr::lua_set_nil("__vr_arm_chain_R_root");
        re4vr::lua_set_nil("__vr_arm_chain_R_maxreach");
        re4vr::lua_set_nil("__vr_arm_chain_L_root");
        re4vr::lua_set_nil("__vr_arm_chain_L_maxreach");
        return;
    }

    for (int s = 0; s < SIDE_COUNT; ++s) {
        const auto side = static_cast<Side>(s);
        const auto uj = get_chain_joint(bone_of(side, PART_UPPERARM));

        if (uj == nullptr) {
            continue;
        }

        glm::vec3 sw{};

        if (!get_vec3(reinterpret_cast<::REManagedObject*>(uj), "get_Position", sw)) {
            // [WIE_LUA] Alter Wert bleibt stehen, es wird NICHT genullt.
            continue;
        }

        float ul = 0.0f;
        float ll = 0.0f;
        arm_ik_segment_lengths(bone_of(side, PART_UPPERARM), bone_of(side, PART_FOREARM), ul, ll);

        const float maxreach = ul + ll - SHOULDER_REACH_FOLLOW_SLACK +
                               (m_shoulder_reach_follow ? m_shoulder_reach_follow_max : 0.0f);

        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_root", SIDE_NAMES[s]);
        re4vr::lua_set_vec3(gname, sw);
        std::snprintf(gname, sizeof(gname), "__vr_arm_chain_%s_maxreach", SIDE_NAMES[s]);
        re4vr::lua_set_number(gname, maxreach);
    }
}

void RE4VRArmChain::railcar_pin_spine() const {
    // [RAILCAR_SPINE_PIN] Lua Z.1224-1229.
    if (!re4vr::lua_get_bool("__re4_railcar_mode", false)) {
        return;
    }

    if (re4vr::lua_get_bool("__re4_railcar_reloading", false)) {
        return; // Reload: native Anim durchlassen
    }

    // [MINECART-PORT 04.09.2026] Frueher:
    //     re4vr::lua_call_global("__re4_minecart_apply_spine_pin");
    // Das Global kam aus re4_vr_minecart.lua und ist mit dessen Port
    // verschwunden. lua_call_global haette still nichts getan (es prueft nur auf
    // sol::type::function und kehrt sonst wortlos zurueck) -- der Spine-Pin
    // waere im Kart ersatzlos ausgefallen: arm_chain haette die Schulter aus
    // der nativen AutoMove-Pose gerechnet, Arm am Anschlag, Hand-Flackern.
    // ArmChain war der EINZIGE Aufrufer (Live-Set und C++-Baum durchsucht),
    // deshalb ist der Direktaufruf die exakte Entsprechung.
    // Luas Aufrufer stand in einem pcall (arm_chain Z.1224-1229): ein Fehler im
    // Pin durfte arm_chain NICHT mitreissen. Die inneren Engine-Helfer fangen
    // zwar selbst, die Container-/String-Operationen darum herum aber nicht --
    // deshalb hier dasselbe Netz.
    try {
        if (auto mc = RE4VRMinecart::get(); mc != nullptr) {
            mc->apply_spine_pin();
        }
    } catch (...) {
    }
}

bool RE4VRArmChain::stillzone_aus() {
    // [STILLZONE] Lua Z.1238-1240: Adas Gondel (Stage 60850).
    return re4vr::lua_get_bool("__re4_stillzone_motion", false);
}

void RE4VRArmChain::phase_entry(bool hook_enabled) {
    // [BODY-EPOCH 2026-09-22] Body gewechselt -> gecachte Arm-Joints verwerfen.
    // Laeuft in jeder Phase, greift aber nur einmal je Wechsel.
    if (const auto ep = re4vr::body_epoch(); ep != m_body_epoch) {
        m_body_epoch = ep;
        drop_body_caches();
    }

    // Lua Z.1302: vor dem ersten on_frame ist keine Phase angemeldet.
    if (!m_phase_hooks_registered) {
        return;
    }

    // Lua Z.1244 usw. -- der harte Ausstieg steht VOR allem anderen, also
    // bleiben in der Gondel Anker und Aktiv-Flag auf ihrem letzten Wert.
    if (stillzone_aus()) {
        return;
    }

    // check_cutscene_end() -- Rumpf ist leer (Lua Z.1014).

    if (should_pause()) {
        re4vr::lua_set_bool("__vr_re4_two_bone_ik_active", false);
        publish_clamp_anchors();
        re4vr::clear_vm_exception();
        return;
    }

    railcar_pin_spine();

    if (hook_enabled) {
        apply_body_chain();
    }

    re4vr::clear_vm_exception();
}

void RE4VRArmChain::on_pre_lock_scene() {
    phase_entry(m_debug_sync.hook_lockscene);
}

void RE4VRArmChain::on_late_update_behavior() {
    phase_entry(m_debug_sync.hook_lateupdate);
}

void RE4VRArmChain::on_update_joint_expression() {
    phase_entry(m_debug_sync.hook_update_jointexpr);
}

void RE4VRArmChain::on_pre_begin_rendering() {
    phase_entry(m_debug_sync.hook_beginrender);
}

// =====================================================================
// ImGui -- Lua Z.1309-1478, Widget fuer Widget.
//
// FALLE: imgui.text_colored ist in Lua ein FORMAT-String. Deshalb hier
// ueberall "%s" und der Text als Argument.
// =====================================================================
void RE4VRArmChain::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRArmChain", "on_draw_ui");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (!ImGui::TreeNode("RE4VR - Arm Chain##re4_vr_arm_chain_root")) {
        return; // Lua: kein tree_pop in diesem Zweig
    }

    // Schulter-Pin-Status prominent ganz oben (gelb = Problem).
    if (m_shoulder_pin) {
        const bool l_ok = m_shoulder_pin_rel[SIDE_L].has_value();
        const bool r_ok = m_shoulder_pin_rel[SIDE_R].has_value();

        char txt[128]{};
        std::snprintf(txt, sizeof(txt), "SCHULTER-PIN  L: %s   R: %s", l_ok ? "AKTIV" : "WARTET!",
                      r_ok ? "AKTIV" : "WARTET!");

        ImGui::TextColored(abgr_to_vec4((l_ok && r_ok) ? 0xFF00FF00u : 0xFF00D7FFu), "%s", txt);
    }

    ImGui::Checkbox("Enable two-bone arm IK##re4_ac_enable", &m_enable_arm_chain);

    ImGui::Separator();
    ImGui::Text("Preset: %s", m_current_key.c_str());
    ImGui::Text("RH joint pos: %s",
                re4vr::lua_get_vec3("__vr_rh_joint_pos").has_value() ? "true" : "false");
    ImGui::Text("LH joint pos: %s",
                re4vr::lua_get_vec3("__vr_lh_joint_pos").has_value() ? "true" : "false");
    ImGui::Text("Dev: set _G.__vr_re4_arm_chain_verify_bone_axis = true to log elbow-axis alignment "
                "once per side.");

    ImGui::Separator();
    ImGui::Text("Segments:");

    // Lua Z.1334: R_Forearm, R_UpperArm, L_Forearm, L_UpperArm -- und alle
    // vier teilen sich die ID "arm_chain_seg" (1:1 uebernommen).
    static constexpr int SEG_ORDER[] = {R_Forearm, R_UpperArm, L_Forearm, L_UpperArm};

    for (const int b : SEG_ORDER) {
        char label[64]{};
        std::snprintf(label, sizeof(label), "%s##arm_chain_seg", BONE_NAMES[b]);
        bool v = m_arm_segment_enabled[b];

        if (ImGui::Checkbox(label, &v)) {
            m_arm_segment_enabled[b] = v;
        }
    }

    ImGui::Separator();
    ImGui::Text("Forearm End Height (Wrist-Y Offset, IK-Ziel):");
    ImGui::DragFloat("Left (m)##wristy_l", &m_wrist_y[SIDE_L], 0.001f, -0.2f, 0.2f, "%.3f");
    ImGui::DragFloat("Right (m)##wristy_r", &m_wrist_y[SIDE_R], 0.001f, -0.2f, 0.2f, "%.3f");

    ImGui::Text("Forearm End Side (Wrist-X Offset, IK-Ziel, + = rechts):");
    ImGui::DragFloat("Left (m)##wristx_l", &m_wrist_x[SIDE_L], 0.001f, -0.2f, 0.2f, "%.3f");
    ImGui::DragFloat("Right (m)##wristx_r", &m_wrist_x[SIDE_R], 0.001f, -0.2f, 0.2f, "%.3f");

    ImGui::Spacing();

    if (ImGui::Button("Save arm IK config##re4_ac_save")) {
        save_config();
    }

    ImGui::SameLine();

    if (ImGui::Button("Reset offsets##re4_ac_reset")) {
        // Lua Z.1355-1367: nur die vier Armknochen, KEIN Save.
        for (const int b : SEG_ORDER) {
            m_chain_offset[b] = ChainOffset{};
        }
    }

    ImGui::Separator();

    {
        bool v = m_shoulder_pin;

        if (ImGui::Checkbox("Shoulder pin (Stand-Pose, vor IK-Solve)##re4_ac_spin", &v)) {
            m_shoulder_pin = v;
            // Lua Z.1373: bei JEDEM Umschalten frisch capturen lassen.
            m_shoulder_pin_rel[SIDE_L].reset();
            m_shoulder_pin_rel[SIDE_R].reset();
        }
    }

    if (m_shoulder_pin) {
        ImGui::Text("Pin L: %s  R: %s",
                    m_shoulder_pin_rel[SIDE_L].has_value() ? "AKTIV" : "warte auf Stand\xE2\x80\xA6",
                    m_shoulder_pin_rel[SIDE_R].has_value() ? "AKTIV" : "warte auf Stand\xE2\x80\xA6");
    }

    ImGui::Checkbox("Shoulder reach follow (RE9, exakt, omnidirektional)##re4_ac_srf",
                    &m_shoulder_reach_follow);

    if (ImGui::DragFloat("Reach-Follow Max (m)##re4_ac_srfmax", &m_shoulder_reach_follow_max, 0.001f,
                         0.0f, 1.0f, "%.3f")) {
        save_config();
    }

    {
        bool v = m_hand_clamp;

        if (ImGui::Checkbox("Hand clamp am Reichweiten-Limit (kein Wegfliegen)##re4_ac_hclamp", &v)) {
            m_hand_clamp = v;
            save_config();
        }
    }

    ImGui::Separator();

    if (ImGui::TreeNode("Debug / Sync (arm chain vs VR motion)##re4_ac_dbg")) {
        ImGui::TextColored(abgr_to_vec4(0xFFCCCC88u), "%s",
                           "Use these toggles to diagnose erratic twisting (esp. high-yaw / running "
                           "in circles).");

        {
            bool v = m_debug_sync.require_motion_tick;

            if (ImGui::Checkbox("Option A: require VR motion tick (uses _G.__vr_motion_tick_id)"
                                "##re4_ac_dbg_mt",
                                &v)) {
                m_debug_sync.require_motion_tick = v;
                m_debug_last_motion_tick_seen = -1.0;
            }
        }

        {
            bool v = m_debug_sync.apply_once_per_frame;

            if (ImGui::Checkbox("Option B: apply arm IK once per game frame##re4_ac_dbg_once", &v)) {
                m_debug_sync.apply_once_per_frame = v;
                m_debug_last_frame_applied = -1.0;
            }
        }

        ImGui::Separator();
        ImGui::Text("Option C: enable hook phases");
        ImGui::Checkbox("LockScene##re4_ac_dbg_h_ls", &m_debug_sync.hook_lockscene);
        ImGui::Checkbox("LateUpdateBehavior##re4_ac_dbg_h_lu", &m_debug_sync.hook_lateupdate);
        ImGui::Checkbox("UpdateJointExpression##re4_ac_dbg_h_uje", &m_debug_sync.hook_update_jointexpr);
        ImGui::Checkbox("BeginRendering##re4_ac_dbg_h_br", &m_debug_sync.hook_beginrender);

        ImGui::Separator();
        ImGui::Checkbox("Show status##re4_ac_dbg_stat", &m_debug_sync.show_status);

        if (m_debug_sync.show_status) {
            ImGui::TextColored(abgr_to_vec4(0xFF888888u), "%s", "Status:");

            const double mt = re4vr::lua_get_number("__vr_motion_tick_id", LUA_NUMBER_ABSENT);

            if (mt == LUA_NUMBER_ABSENT) {
                ImGui::Text("motion_tick_id=nil  last_seen=%g", m_debug_last_motion_tick_seen);
            } else {
                ImGui::Text("motion_tick_id=%g  last_seen=%g", mt, m_debug_last_motion_tick_seen);
            }

            ImGui::Text("last_frame_applied=%g", m_debug_last_frame_applied);
        }

        ImGui::Spacing();

        if (ImGui::Button("Preset: Motion-tick + once-per-frame + UpdateJointExpression only"
                          "##re4_ac_dbg_p1")) {
            m_debug_sync.require_motion_tick = true;
            m_debug_sync.apply_once_per_frame = true;
            m_debug_sync.hook_lockscene = false;
            m_debug_sync.hook_lateupdate = false;
            m_debug_sync.hook_update_jointexpr = true;
            m_debug_sync.hook_beginrender = false;
            m_debug_last_frame_applied = -1.0;
            m_debug_last_motion_tick_seen = -1.0;
        }

        ImGui::SameLine();

        if (ImGui::Button("Preset: Only BeginRendering (single phase)##re4_ac_dbg_p2")) {
            m_debug_sync.require_motion_tick = true;
            m_debug_sync.apply_once_per_frame = true;
            m_debug_sync.hook_lockscene = false;
            m_debug_sync.hook_lateupdate = false;
            m_debug_sync.hook_update_jointexpr = false;
            m_debug_sync.hook_beginrender = true;
            m_debug_last_frame_applied = -1.0;
            m_debug_last_motion_tick_seen = -1.0;
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text("Per-joint IK tweaks (saved in JSON chain.*)");

    // Lua Z.1449: ANDERE Reihenfolge als bei den Segments.
    static constexpr int TWEAK_ORDER[] = {R_UpperArm, R_Forearm, L_UpperArm, L_Forearm};

    for (const int b : TWEAK_ORDER) {
        char label[64]{};
        std::snprintf(label, sizeof(label), "%s##arm_chain_tweak", BONE_NAMES[b]);

        if (!ImGui::TreeNode(label)) {
            continue;
        }

        char pid[64]{};
        std::snprintf(pid, sizeof(pid), "ac_tw_%s", BONE_NAMES[b]);
        ImGui::PushID(pid);

        auto& off = m_chain_offset[b];
        ImGui::DragFloat("pos_x (IK length hint)", &off.pos_x, 0.001f, -0.5f, 0.5f, "%.4f");
        ImGui::DragFloat("pos_y", &off.pos_y, 0.001f, -0.5f, 0.5f, "%.4f");
        ImGui::DragFloat("pos_z", &off.pos_z, 0.001f, -0.5f, 0.5f, "%.4f");
        ImGui::DragFloat("rot tweak pitch", &off.rot_x, 0.25f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("rot tweak yaw", &off.rot_y, 0.25f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("rot tweak roll", &off.rot_z, 0.25f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("scale_x", &off.scale_x, 0.01f, 0.01f, 3.0f, "%.2f");
        ImGui::DragFloat("scale_y", &off.scale_y, 0.01f, 0.01f, 3.0f, "%.2f");
        ImGui::DragFloat("scale_z", &off.scale_z, 0.01f, 0.01f, 3.0f, "%.2f");

        ImGui::PopID();
        ImGui::TreePop();
    }

    ImGui::TreePop();
}

#endif // RE4
