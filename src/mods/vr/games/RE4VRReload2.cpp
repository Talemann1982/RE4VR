// ============================================================================
// RE4VRReload2 -- 1:1-Portierung von re4_vr_reload2.lua (6.790 Zeilen).
// Spezifikation: I:\LUATRANS\PORT_RELOAD2_SPEC.md
// ============================================================================

#if defined(RE4)

#include <fstream>
#include <span>
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
#include "../../../REFramework.hpp"   // g_framework->draw_menu_heading
#include "../../../HookManager.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRWeapons2.hpp"
#include "RE4VRReloadAdv.hpp"
#include "RE4VRReloadMain.hpp"
#include "RE4VRReload2.hpp"

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

// [SOUND] IDs des Revolver-Blocks
constexpr uint32_t SND_CYLINDER = 942865223u;    // Trommel auf/zu
constexpr uint32_t SND_COCK = 938556079u;        // Hahn spannen
constexpr uint32_t SND_INSERT = 942865223u;      // Patrone rastet ein
constexpr uint32_t SND_DROP = 1351699582u;       // fallengelassene Patrone
constexpr uint32_t SND_MAG_HOLSTER = 1839787494u;
constexpr uint32_t SND_DRY_FIRE = 812850326u;

constexpr float REV_GRAVITY = 9.8f;
constexpr float REV_DROP_DUR = 1.0f;

// [EJECT] Kennzahlen
constexpr float EJECT_DOWN = 0.6f;
constexpr float EJECT_ZSIGN = -1.0f;
constexpr float EJECT_SLIDE_DUR = 0.13f;
constexpr float EJECT_SLIDE_DIST = 0.055f;
constexpr float EJECT_STAGGER = 0.06f;

// [WRIST-FLICK CLOSE]
constexpr float FLICK_VEL = 5.0f;
constexpr double FLICK_REVERSAL = 0.30;
constexpr double FLICK_OPEN_GRACE = 0.35;
constexpr double FLICK_SND_DELAY = 0.18;

// [HAND-RAMPE] Die Hand faehrt ueber die ersten HAND_RAMP der Phase in den
// cockhand-Offset, steht dann still (nur die Daumen-Keys laufen) und faehrt
// ueber die letzten HAND_RAMP wieder zurueck.
constexpr float HAND_RAMP = 0.12f;

float hand_frac_at(float ph) {
    float u;

    if (ph <= HAND_RAMP) {
        u = ph / HAND_RAMP;
    } else if (ph >= 1.0f - HAND_RAMP) {
        u = (1.0f - ph) / HAND_RAMP;
    } else {
        return 1.0f;
    }

    u = clamp01(u);

    return ease(u);
}

}   // namespace

// ============================================================================
// Geteilte Helfer
// ============================================================================

::REManagedObject* RE4VRReload2::get_ctx() {
    if (re4vr::fc::on()) {
        return re4vr::fc::ctx();
    }

    if (!re4vr::obj_ok(m_character_manager)) {
        m_character_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

std::optional<int32_t> RE4VRReload2::get_equip_wid() {
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

::REManagedObject* RE4VRReload2::body_tf() {
    if (re4vr::fc::on()) {
        return re4vr::fc::body_tf();
    }

    auto* ctx = get_ctx();
    auto* b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    return re4vr::call_safe<::REManagedObject*>(b, "get_Transform");
}

::REManagedObject* RE4VRReload2::get_pe() {
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
::REManagedObject* RE4VRReload2::find_weapon(int32_t wid) {
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
bool RE4VRReload2::right_b_down() {
    return re4vr::lua_get_tribool("__vr_raw_r_bbutton") == 1;
}

// [POSE_FADE] s. Header.
bool RE4VRReload2::pose_fade_step(PoseFade& f, const std::string& want, float& blend_out) {
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

const std::unordered_map<std::string, ::REManagedObject*>& RE4VRReload2::pose_map() {
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

bool RE4VRReload2::pose_apply(const Bones& bones, float blend) {
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
// 1 -- REVOLVER (Lua Z.1-1582)
// ============================================================================
namespace {

constexpr const char* REV_CFG_PATH = "re4_vr/re4_vr_reload2.json";

// wp5001, wp4500 Broken Butterfly. (wp4502 Handcannon -> reload3.)
bool rev_is(int32_t wid) {
    return wid == 5001 || wid == 4500;
}

// Joints je Waffe. [4500] TOP-BREAK: _04 = Hinge/Treiber, _05 (Trommel+Kugeln)
// klappt als Kind mit hoch, _06 = Kugelpaket (Anker fuer die Einlege-Distanz),
// _07.._12 = die sechs Einzelkugeln.
struct RevJoints {
    const char* cylinder;
    const char* bullet;
    const char* insert_ref;
    const char* spin;
    const char* hand_cartridge;
    const char* hammer;
    bool has_bullets;
};

std::optional<RevJoints> rev_joints(int32_t wid) {
    if (wid == 5001) {
        return RevJoints{"_06", "_07", nullptr, nullptr, nullptr, nullptr, false};
    }

    if (wid == 4500) {
        return RevJoints{"_04", "_07", "_06", "_05", "_101", "_02", true};
    }

    return std::nullopt;
}

const char* const REV_BULLETS[6] = {"_07", "_08", "_09", "_10", "_11", "_12"};

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

bool key_to_wid(const std::string& k, int32_t& out) {
    try {
        size_t used = 0;
        const long v = std::stol(k, &used);

        if (used != k.size()) {
            return false;
        }

        out = static_cast<int32_t>(v);

        return true;
    } catch (...) {
        return false;
    }
}

}   // namespace

bool RE4VRReload2::is_revolver(int32_t wid) {
    return rev_is(wid);
}

RE4VRReload2::Cyl& RE4VRReload2::cyl_cfg(int32_t wid) {
    auto it = m_cyl.find(wid);

    if (it == m_cyl.end()) {
        it = m_cyl.emplace(wid, Cyl{}).first;
    }

    return it->second;
}

RE4VRReload2::Shell& RE4VRReload2::shell_cfg(int32_t wid) {
    auto it = m_shell.find(wid);

    if (it == m_shell.end()) {
        it = m_shell.emplace(wid, Shell{}).first;
    }

    return it->second;
}

RE4VRReload2::HammerCfg& RE4VRReload2::hammer_cfg(int32_t wid) {
    auto it = m_hammer_cfg.find(wid);

    if (it == m_hammer_cfg.end()) {
        it = m_hammer_cfg.emplace(wid, HammerCfg{}).first;
    }

    return it->second;
}

std::vector<RE4VRReload2::ThumbKey>& RE4VRReload2::thumb_keys(int32_t wid, bool aim) {
    return aim ? m_tkeys_aim[wid] : m_tkeys[wid];
}

// [DAUMEN-KEYS] Kurve an der Phase p (0..100) abtasten. Vor dem ersten Key wird
// von NEUTRAL hochgeblendet, nach dem letzten wieder auf NEUTRAL runter -- der
// Daumen laeuft also von selbst sauber aus dem Griff heraus und wieder hinein.
bool RE4VRReload2::thumb_key_sample(const std::vector<ThumbKey>& list, float p,
                                    std::array<ThumbJoint, 3>& out) {
    for (auto& o : out) {
        o = ThumbJoint{};
    }

    const size_t n = list.size();

    if (n == 0) {
        return false;
    }

    const ThumbKey* lo = nullptr;
    const ThumbKey* hi = nullptr;
    std::optional<float> t{};

    if (p <= list[0].p) {
        hi = &list[0];
        t = (list[0].p > 0.0001f) ? (p / list[0].p) : 1.0f;   // neutral -> erster Key
    } else if (p >= list[n - 1].p) {
        lo = &list[n - 1];
        const float span = 100.0f - list[n - 1].p;
        t = (span > 0.0001f) ? ((p - list[n - 1].p) / span) : 1.0f;   // letzter -> neutral
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
        const ThumbJoint a = (lo != nullptr) ? lo->j[i] : ThumbJoint{};
        const ThumbJoint b = (hi != nullptr) ? hi->j[i] : ThumbJoint{};
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

void RE4VRReload2::rev_load_cfg() {
    const auto data = re4vr::json_load(REV_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    const nlohmann::json& c = (data.contains("cfg") && data["cfg"].is_object())
                              ? data["cfg"] : data;

    m_rcfg.revolver_enabled = jbool(c, "revolver_enabled", m_rcfg.revolver_enabled);
    m_rcfg.reload_ammo = jbool(c, "reload_ammo", m_rcfg.reload_ammo);
    m_rcfg.sound_enabled = jbool(c, "sound_enabled", m_rcfg.sound_enabled);
    m_rcfg.insert_distance = jnum(c, "insert_distance", m_rcfg.insert_distance);
    m_rcfg.support_cooldown = jnum(c, "support_cooldown", m_rcfg.support_cooldown);
    m_rcfg.cock_press = jnum(c, "cock_press", m_rcfg.cock_press);
    m_rcfg.cock_hold = jnum(c, "cock_hold", m_rcfg.cock_hold);
    m_rcfg.cock_return = jnum(c, "cock_return", m_rcfg.cock_return);
    m_rcfg.cock_fall = jnum(c, "cock_fall", m_rcfg.cock_fall);
    m_rcfg.cock_dur = jnum(c, "cock_dur", m_rcfg.cock_dur);

    // [CYLINDER] getunte Ausschwenk-Werte pro Waffe
    if (const auto it = data.find("cyl"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& rr = cyl_cfg(wid);
            rr.rx = jnum(v, "rx", rr.rx);
            rr.ry = jnum(v, "ry", rr.ry);
            rr.rz = jnum(v, "rz", rr.rz);
            rr.px = jnum(v, "px", rr.px);
            rr.py = jnum(v, "py", rr.py);
            rr.pz = jnum(v, "pz", rr.pz);
            rr.lerp = jnum(v, "lerp", rr.lerp);
            rr.shot_deg = jnum(v, "shot_deg", rr.shot_deg);
            rr.shot_lerp = jnum(v, "shot_lerp", rr.shot_lerp);
        }
    }

    // [SHELL-IN-HAND] Offsets + Pose-Name pro Waffe
    if (const auto it = data.find("shell"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& ss = shell_cfg(wid);
            ss.pose = jstr(v, "pose", ss.pose);
            ss.parts = jstr(v, "parts", ss.parts);
            ss.x = jnum(v, "x", ss.x);
            ss.y = jnum(v, "y", ss.y);
            ss.z = jnum(v, "z", ss.z);
            ss.rx = jnum(v, "rx", ss.rx);
            ss.ry = jnum(v, "ry", ss.ry);
            ss.rz = jnum(v, "rz", ss.rz);
            ss.t_rx = jnum(v, "t_rx", ss.t_rx);
            ss.t_ry = jnum(v, "t_ry", ss.t_ry);
            ss.t_rz = jnum(v, "t_rz", ss.t_rz);
            ss.i_rx = jnum(v, "i_rx", ss.i_rx);
            ss.i_ry = jnum(v, "i_ry", ss.i_ry);
            ss.i_rz = jnum(v, "i_rz", ss.i_rz);
            ss.scale = jnum(v, "scale", ss.scale);
        }
    }

    // [SINGLE-ACTION HAMMER]
    if (const auto it = data.find("hammer"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& hh = hammer_cfg(wid);
            hh.idle_rx = jnum(v, "idle_rx", hh.idle_rx);
            hh.idle_ry = jnum(v, "idle_ry", hh.idle_ry);
            hh.idle_rz = jnum(v, "idle_rz", hh.idle_rz);
            hh.rx = jnum(v, "rx", hh.rx);
            hh.ry = jnum(v, "ry", hh.ry);
            hh.rz = jnum(v, "rz", hh.rz);
            hh.lerp = jnum(v, "lerp", hh.lerp);
        }
    }

    // [COCK-HAND] + [COCK-HAND AIM]: fehlt der Aim-Satz -> Kopie des No-Aim-Satzes.
    const auto read_off = [](const nlohmann::json& v, HammerState::Off& o) {
        o.rx = jnum(v, "rx", o.rx);
        o.ry = jnum(v, "ry", o.ry);
        o.rz = jnum(v, "rz", o.rz);
        o.px = jnum(v, "px", o.px);
        o.py = jnum(v, "py", o.py);
        o.pz = jnum(v, "pz", o.pz);
    };

    if (const auto it = data.find("cockhand"); it != data.end() && it->is_object()) {
        read_off(*it, m_ham.hand);
    }

    if (const auto it = data.find("cockhand_aim"); it != data.end() && it->is_object()) {
        read_off(*it, m_ham.hand_aim);
    } else {
        m_ham.hand_aim = m_ham.hand;
    }

    // [DAUMEN-VERSATZ] + Aim-Kopie
    if (const auto it = data.find("thumbpos"); it != data.end() && it->is_object()) {
        m_ham.thumb_pos = glm::vec3{jnum(*it, "x", m_ham.thumb_pos.x),
                                    jnum(*it, "y", m_ham.thumb_pos.y),
                                    jnum(*it, "z", m_ham.thumb_pos.z)};
    }

    if (const auto it = data.find("thumbpos_aim"); it != data.end() && it->is_object()) {
        m_ham.thumb_pos_aim = glm::vec3{jnum(*it, "x", m_ham.thumb_pos_aim.x),
                                        jnum(*it, "y", m_ham.thumb_pos_aim.y),
                                        jnum(*it, "z", m_ham.thumb_pos_aim.z)};
    } else {
        m_ham.thumb_pos_aim = m_ham.thumb_pos;
    }

    // [SINGLE-ACTION THUMB] die alten 2-Punkt-Saetze (heute wirkungslos, s.
    // apply_thumb_pass -- der Daumen kommt ausschliesslich aus den Keyframes).
    const auto read_tcfg = [&](const nlohmann::json& src,
                               std::unordered_map<int32_t, std::array<ThumbJoint, 3>>& dst) {
        for (const auto& item : src.items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_array()) {
                continue;
            }

            auto& tt = dst[wid];

            for (size_t i = 0; i < 3 && i < v.size(); ++i) {
                if (!v[i].is_object()) {
                    continue;
                }

                tt[i].x = jnum(v[i], "ix", tt[i].x);
                tt[i].y = jnum(v[i], "iy", tt[i].y);
                tt[i].z = jnum(v[i], "iz", tt[i].z);
                tt[i].px = jnum(v[i], "cx", tt[i].px);
                tt[i].py = jnum(v[i], "cy", tt[i].py);
                tt[i].pz = jnum(v[i], "cz", tt[i].pz);
            }
        }
    };

    if (const auto it = data.find("thumb"); it != data.end() && it->is_object()) {
        read_tcfg(*it, m_tcfg);
    }

    if (const auto it = data.find("thumb_aim"); it != data.end() && it->is_object()) {
        read_tcfg(*it, m_tcfg_aim);
    } else {
        m_tcfg_aim = m_tcfg;
    }

    // [DAUMEN-KEYS] Stuetzpunkte, getrennt Aim / No-Aim, nach Phase sortiert.
    const auto read_keys = [&](const nlohmann::json& src,
                               std::unordered_map<int32_t, std::vector<ThumbKey>>& dst) {
        for (const auto& item : src.items()) {
            int32_t wid = 0;
            const auto& list = item.value();

            if (!key_to_wid(item.key(), wid) || !list.is_array()) {
                continue;
            }

            std::vector<ThumbKey> out;

            for (const auto& key : list) {
                if (!key.is_object() || !key.contains("p") || !key.contains("j")) {
                    continue;
                }

                ThumbKey k{};
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
                      [](const ThumbKey& a, const ThumbKey& b) { return a.p < b.p; });
            dst[wid] = out;
        }
    };

    if (const auto it = data.find("thumbkeys"); it != data.end() && it->is_object()) {
        read_keys(*it, m_tkeys);
    }

    if (const auto it = data.find("thumbkeys_aim"); it != data.end() && it->is_object()) {
        read_keys(*it, m_tkeys_aim);
    }
}

void RE4VRReload2::rev_save_cfg() {
    nlohmann::json cylo = nlohmann::json::object();

    for (const auto& e : m_cyl) {
        const auto& v = e.second;
        cylo[std::to_string(e.first)] = {
            {"rx", v.rx}, {"ry", v.ry}, {"rz", v.rz},
            {"px", v.px}, {"py", v.py}, {"pz", v.pz},
            {"lerp", v.lerp}, {"shot_deg", v.shot_deg}, {"shot_lerp", v.shot_lerp},
        };
    }

    nlohmann::json shello = nlohmann::json::object();

    for (const auto& e : m_shell) {
        const auto& v = e.second;
        shello[std::to_string(e.first)] = {
            {"pose", v.pose}, {"x", v.x}, {"y", v.y}, {"z", v.z},
            {"rx", v.rx}, {"ry", v.ry}, {"rz", v.rz},
            {"t_rx", v.t_rx}, {"t_ry", v.t_ry}, {"t_rz", v.t_rz},
            {"i_rx", v.i_rx}, {"i_ry", v.i_ry}, {"i_rz", v.i_rz},
            {"parts", v.parts}, {"scale", v.scale},
        };
    }

    nlohmann::json hammero = nlohmann::json::object();

    for (const auto& e : m_hammer_cfg) {
        const auto& v = e.second;
        hammero[std::to_string(e.first)] = {
            {"idle_rx", v.idle_rx}, {"idle_ry", v.idle_ry}, {"idle_rz", v.idle_rz},
            {"rx", v.rx}, {"ry", v.ry}, {"rz", v.rz}, {"lerp", v.lerp},
        };
    }

    const auto write_tcfg =
        [](const std::unordered_map<int32_t, std::array<ThumbJoint, 3>>& src) {
        nlohmann::json o = nlohmann::json::object();

        for (const auto& e : src) {
            nlohmann::json arr = nlohmann::json::array();

            for (const auto& j : e.second) {
                arr.push_back({{"ix", j.x}, {"iy", j.y}, {"iz", j.z},
                               {"cx", j.px}, {"cy", j.py}, {"cz", j.pz}});
            }

            o[std::to_string(e.first)] = arr;
        }

        return o;
    };

    const auto ser_keys =
        [](const std::unordered_map<int32_t, std::vector<ThumbKey>>& m) {
        nlohmann::json o = nlohmann::json::object();

        for (const auto& e : m) {
            nlohmann::json arr = nlohmann::json::array();

            for (const auto& key : e.second) {
                nlohmann::json j = nlohmann::json::array();

                for (const auto& s : key.j) {
                    j.push_back({{"x", s.x}, {"y", s.y}, {"z", s.z},
                                 {"px", s.px}, {"py", s.py}, {"pz", s.pz}});
                }

                arr.push_back({{"p", key.p}, {"j", j}});
            }

            o[std::to_string(e.first)] = arr;
        }

        return o;
    };

    const auto& ch = m_ham.hand;
    const auto& ca = m_ham.hand_aim;

    nlohmann::json d = {
        {"cfg", {
            {"revolver_enabled", m_rcfg.revolver_enabled},
            {"insert_distance", m_rcfg.insert_distance},
            {"reload_ammo", m_rcfg.reload_ammo},
            {"sound_enabled", m_rcfg.sound_enabled},
            {"cock_press", m_rcfg.cock_press}, {"cock_hold", m_rcfg.cock_hold},
            {"cock_return", m_rcfg.cock_return}, {"cock_fall", m_rcfg.cock_fall},
            {"cock_dur", m_rcfg.cock_dur},
            {"support_cooldown", m_rcfg.support_cooldown},
        }},
        {"cyl", cylo},
        {"shell", shello},
        {"hammer", hammero},
        {"thumb", write_tcfg(m_tcfg)},
        {"thumb_aim", write_tcfg(m_tcfg_aim)},
        {"cockhand", {{"rx", ch.rx}, {"ry", ch.ry}, {"rz", ch.rz},
                      {"px", ch.px}, {"py", ch.py}, {"pz", ch.pz}}},
        {"cockhand_aim", {{"rx", ca.rx}, {"ry", ca.ry}, {"rz", ca.rz},
                          {"px", ca.px}, {"py", ca.py}, {"pz", ca.pz}}},
        {"thumbpos", {{"x", m_ham.thumb_pos.x}, {"y", m_ham.thumb_pos.y},
                      {"z", m_ham.thumb_pos.z}}},
        {"thumbpos_aim", {{"x", m_ham.thumb_pos_aim.x}, {"y", m_ham.thumb_pos_aim.y},
                          {"z", m_ham.thumb_pos_aim.z}}},
        {"thumbkeys", ser_keys(m_tkeys)},
        {"thumbkeys_aim", ser_keys(m_tkeys_aim)},
    };

    re4vr::json_save(REV_CFG_PATH, d);
}

void RE4VRReload2::rev_play_sound(std::optional<uint32_t> id) {
    if (!m_rcfg.sound_enabled || !id.has_value()) {
        return;
    }

    if (m_rwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_rwep.tf, "get_GameObject"), *id);
}

void RE4VRReload2::rev_refresh_weapon() {
    const auto ewid = get_equip_wid();
    const bool managed = m_rcfg.revolver_enabled && ewid.has_value() && rev_is(*ewid);

    if (!managed) {
        cart_destroy();
        m_rwep = RevWep{};

        return;
    }

    if (m_rwep.wid.has_value() && *m_rwep.wid == *ewid && m_rwep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_rwep.tf, "get_Position", p)) {
            return;
        }
    }

    m_rwep = RevWep{};

    auto* tf = find_weapon(*ewid);

    if (tf == nullptr) {
        return;
    }

    m_rwep.wid = *ewid;
    m_rwep.tf = tf;

    const auto jc = rev_joints(*ewid);

    if (!jc.has_value()) {
        return;
    }

    m_rwep.cyl_joint = joint_by_name(tf, jc->cylinder);

    if (m_rwep.cyl_joint != nullptr) {
        glm::quat r{};
        glm::vec3 p{};

        if (get_quat(m_rwep.cyl_joint, "get_LocalRotation", r)) {
            m_rwep.cyl_rest_rot = r;   // Ruhe-Rotation (zu)
        }

        if (get_vec3(m_rwep.cyl_joint, "get_LocalPosition", p)) {
            m_rwep.cyl_rest_pos = p;   // Ruhe-Position (zu)
        }
    }

    // [BULLETS] Einzel-Kugel-Joints + Ruhe-Scale (nur als "sichtbar"-Wert,
    // Guard gegen 0).
    if (jc->has_bullets) {
        for (const char* bn : REV_BULLETS) {
            auto* bj = joint_by_name(tf, bn);

            if (bj == nullptr) {
                continue;
            }

            RevWep::Bullet b{};
            b.joint = bj;
            b.name = bn;
            glm::vec3 rs{};

            if (get_vec3(bj, "get_LocalScale", rs) && rs.x > 0.01f) {
                b.vis = rs;
            }

            m_rwep.bullets.push_back(b);
        }
    }

    if (jc->insert_ref != nullptr) {
        m_rwep.insert_joint = joint_by_name(tf, jc->insert_ref);
    }

    // [SPIN-LOCK] _05 + Ruhe-Rotation -> native Chamber-Spin-Anim unterdruecken.
    if (jc->spin != nullptr) {
        m_rwep.spin_joint = joint_by_name(tf, jc->spin);

        if (m_rwep.spin_joint != nullptr) {
            glm::quat r{};

            if (get_quat(m_rwep.spin_joint, "get_LocalRotation", r)) {
                m_rwep.spin_rest_rot = r;
            }
        }
    }

    if (jc->hand_cartridge != nullptr) {
        m_rwep.hand_cart_joint = joint_by_name(tf, jc->hand_cartridge);
    }

    // [SINGLE-ACTION HAMMER] Ruhe = BIND-Pose (get_BaseLocalRotation), eine
    // Modell-Konstante, die NIE von unserem Override kontaminiert wird.
    if (jc->hammer != nullptr) {
        m_rwep.hammer_joint = joint_by_name(tf, jc->hammer);

        if (m_rwep.hammer_joint != nullptr) {
            glm::quat r{};

            if (get_quat(m_rwep.hammer_joint, "get_BaseLocalRotation", r)) {
                m_rwep.hammer_rest_rot = r;
            }
        }
    }
}

// [ACCESSOR] zuerst die ECHTE, persistente Instanz; alles darunter sind KOPIEN.
::REManagedObject* RE4VRReload2::get_live_wi() {
    if (m_main != nullptr) {
        if (auto* rw = m_main->real_wi(); rw != nullptr) {
            return rw;
        }
    }

    // Fallback: getEquipWeaponItem -- robust ohne vorher zu schiessen und nach
    // Save-Load. WAR die Ursache fuer "keine Patrone".
    auto* p = get_pe();
    auto* ewi = re4vr::call_safe<::REManagedObject*>(p, "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

// [FULL-CHECK] Engine-eigene Wahrheit bevorzugt (deckt Upgrades ab), Fallback
// loaded >= get_CurrentAmmoMax (Live-Max INKL. Ausbau).
bool RE4VRReload2::gun_is_full(::REManagedObject* wi) {
    bool bf = false;

    if (re4vr::try_call<bool>(wi, "get_IsBulletFull", bf)) {
        return bf;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    const int32_t maxc = call_enum(wi, "get_CurrentAmmoMax").value_or(0);

    return maxc > 0 && loaded >= maxc;
}

bool RE4VRReload2::insert_one_round() {
    if (!m_rcfg.reload_ammo || m_main == nullptr) {
        return false;
    }

    auto* wi = get_live_wi();

    if (wi == nullptr) {
        return false;
    }

    const int32_t loaded = call_enum(wi, "get_CurrentAmmoCount").value_or(0);

    if (gun_is_full(wi)) {
        return false;
    }

    auto* p = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    const auto read_reserve = [&]() -> int32_t {
        return (inv != nullptr && ammo_id.has_value())
            ? m_main->item_count_sum(inv, *ammo_id) : 0;
    };

    const int32_t r_b4 = read_reserve();

    if (r_b4 <= 0) {
        return false;   // keine Reserve -> kein Gratis-Ammo
    }

    const auto gun_ammo = [&]() -> std::optional<int32_t> {
        return m_main->gun_ammo();
    };

    // [BUTTERFLY-AMMO] write_dword @0x44 greift bei diesem Revolver NICHT (die
    // Engine deckelt/ignoriert). Stattdessen der Ladeweg -- exakt der, der bei
    // Shotguns/Pistolen funktioniert.
    if (m_rwep.wid.value_or(0) == 4500) {
        std::optional<int32_t> et{};

        if (auto* td = sdk::find_type_definition("chainsaw.EquipType"); td != nullptr) {
            if (auto* f = td->get_field("Main"); f != nullptr) {
                et = f->get_data<int32_t>(nullptr);
            }
        }

        const int32_t before = gun_ammo().value_or(loaded);

        if (inv != nullptr && et.has_value()) {
            m_main->load_and_book(inv, *et, 1, false);
        }

        return gun_ammo().value_or(before) > before;
    }

    const int32_t base = gun_ammo().value_or(loaded);

    if (auto* td = utility::re_managed_object::get_type_definition(wi); td != nullptr) {
        uint32_t off = 0x44;

        if (auto* f = td->get_field("_CurrentAmmoCount"); f != nullptr) {
            off = f->get_offset_from_base();
        }

        *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(wi) + off) = loaded + 1;
    }

    int32_t af = gun_ammo().value_or(base);

    if (af <= base) {
        re4vr::call_safe<void*>(wi, "addAmmoCount", 1, true);
        af = gun_ammo().value_or(base);
    }

    if (af <= base) {
        return false;   // nichts geladen -> auch nichts abziehen
    }

    // [VERLUSTSICHER] Reserve NUR manuell ziehen, wenn der Ladeweg sie nicht
    // selbst gezogen hat.
    const int32_t gained = af - base;

    if (read_reserve() >= r_b4 && inv != nullptr && ammo_id.has_value()) {
        m_main->safe_reduce(inv, *ammo_id, gained);
    }

    return true;
}

// Sperrt (Holster buzzt) bei Reserve == 0 ODER voller Trommel.
bool RE4VRReload2::revolver_can_grab() {
    auto* wi = get_live_wi();

    if (wi == nullptr || m_main == nullptr) {
        return false;
    }

    auto* p = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");
    const int32_t reserve = (inv != nullptr && ammo_id.has_value())
        ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return false;
    }

    // [FULL-BLOCK] Trommel voll -> ebenfalls sperren+buzzen (Engine-Check ist
    // Upgrade-aware; der alte harte 6er-Cap war falsch).
    return !gun_is_full(wi);
}

bool RE4VRReload2::revolver_set_mag_in_hand(bool active) {
    if (active) {
        if (m_rev_st.cart) {
            return true;
        }

        // [SHELL-KEYFRAMES] Diese Greif-Session hat schon eine Bahn gefahren
        // und eingelegt -> KEINE neue Patrone, erst loslassen + neu greifen.
        if (m_rev_st.kf_used) {
            return false;
        }

        if (!revolver_can_grab()) {
            return false;
        }

        m_rev_st.cart = true;
        m_drop2.active = false;   // evtl. laufenden Fall abbrechen
        rev_play_sound(SND_MAG_HOLSTER);

        return true;
    }

    // [SHELL-KEYFRAMES] Laeuft die Bahn, ist die Patrone bereits "committed":
    // Loslassen NICHT als Drop werten -- sonst faellt sie UND die Bahn bricht ab.
    if (m_rev_st.kf_active) {
        m_rev_st.kf_used = false;

        return true;
    }

    // losgelassen ohne Einlegen -> den Mesh-Klon von seiner aktuellen Position
    // fallen lassen. Die Trommel bleibt unberuehrt (kein Loch).
    if (m_rev_st.cart && m_cart.obj != nullptr) {
        auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");
        glm::vec3 p{};

        if (tf != nullptr && get_vec3(tf, "get_Position", p)) {
            m_drop2.active = true;
            m_drop2.snd = false;
            m_drop2.sx = p.x;
            m_drop2.sy = p.y;
            m_drop2.sz = p.z;
            m_drop2.t0 = clock_now();

            // [NO_LAG] Klon war ans L_Hand geparentet -> fuer den Welt-Freifall
            // entkoppeln.
            if (m_cart.parented) {
                re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                m_cart.parented = false;
            }
        }
    }

    m_rev_st.cart = false;
    m_rev_st.kf_used = false;

    return true;
}

// Weltposition der Patrone IN DER HAND -- damit messen wir die Einlege-Distanz
// von der echten Patrone, nicht vom Hand-Joint.
std::optional<glm::vec3> RE4VRReload2::held_cartridge_pos() {
    auto* bt = body_tf();
    auto* lhj = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lhj == nullptr || !get_vec3(lhj, "get_Position", hp)) {
        return std::nullopt;
    }

    glm::quat hr{};
    const auto& s = shell_cfg(m_rwep.wid.value_or(0));

    if (get_quat(lhj, "get_Rotation", hr)) {
        return hp + (hr * glm::vec3{s.x, s.y, s.z});
    }

    return hp;
}

// [SHELL FLOW] Einlegen: Patrone in der Hand + Trommel OFFEN + nah am
// Kugel-Paket (_06) -> +1.
void RE4VRReload2::update_reload() {
    if (!(m_rcfg.revolver_enabled && m_rcfg.reload_ammo && m_rwep.wid.has_value()
          && m_rwep.insert_joint != nullptr)) {
        return;
    }

    if (!m_rev_st.cart) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_revolver_cyl_open") != 1) {
        return;   // nur bei OFFENER Trommel
    }

    const auto cp = held_cartridge_pos();

    if (!cp.has_value()) {
        return;
    }

    glm::vec3 ip{};

    if (!get_vec3(m_rwep.insert_joint, "get_Position", ip)) {
        return;
    }

    if (vec_len(vec_sub(*cp, ip)) > m_rcfg.insert_distance) {
        return;
    }

    const int32_t wid = *m_rwep.wid;

    if (m_adv != nullptr && m_adv->has_shell_keys(wid)) {
        // [SHELL-KEYFRAMES] Statt sofort einlegen: Keyframe-Bahn starten -- der
        // Klon gleitet Hand -> Trommel (reposition_cart_late faehrt die Bahn).
        if (!m_rev_st.kf_active && !m_rev_st.kf_used) {
            m_rev_st.kf_active = true;
            m_rev_st.kf_hold_t.reset();
            m_rev_st.kf_used = true;       // EINE Bahn pro gegriffener Patrone
            m_rev_st.kf_inserted = false;
            m_rev_st.kf_t0 = clock_now();

            // Parenting loesen -> die Bahn setzt Welt-Positionen.
            if (m_cart.obj != nullptr && m_cart.parented) {
                auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");

                if (tf != nullptr) {
                    re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                }

                m_cart.parented = false;
            }
        }

        // +1 JEDEN Frame versuchen (RETRY), bis getCurrentGunAmmo die Schreibung
        // reflektiert -- ein Einmal-Versuch scheitert am Timing.
        if (m_rev_st.cart && !m_rev_st.kf_inserted && insert_one_round()) {
            m_rev_st.kf_inserted = true;
            m_rev_st._insert_t = clock_now();
            rev_play_sound(SND_INSERT);
        }

        return;
    }

    if (insert_one_round()) {
        m_rev_st.cart = false;                 // Patrone verbraucht
        m_rev_st._insert_t = clock_now();      // [SUPPORT-COOLDOWN]
        rev_play_sound(SND_INSERT);
    }
}

// [CYLINDER] Trommel ausschwenken: Right-B togglet auf/zu, gelerped.
void RE4VRReload2::update_cylinder() {
    if (!(m_rwep.wid.has_value() && m_rwep.cyl_joint != nullptr)) {
        return;
    }

    if (m_cyl_st.preview) {
        re4vr::lua_set_bool("__vr_revolver_cyl_open", m_cyl_st.prog > 0.15f);

        return;   // UI: Slider treibt prog
    }

    const auto& r = cyl_cfg(*m_rwep.wid);
    const bool b = right_b_down();

    if (b && !m_cyl_st._prev_b) {
        m_cyl_st.open = !m_cyl_st.open;
        // [SOUND] OEFFNEN = Trommel-ID, SCHLIESSEN = Cock-ID (getauscht).
        rev_play_sound(m_cyl_st.open ? SND_CYLINDER : SND_COCK);
    }

    m_cyl_st._prev_b = b;

    const float target = m_cyl_st.open ? 1.0f : 0.0f;

    if (m_cyl_st.prog < target) {
        m_cyl_st.prog = std::min(target, m_cyl_st.prog + r.lerp);
    } else if (m_cyl_st.prog > target) {
        m_cyl_st.prog = std::max(target, m_cyl_st.prog - r.lerp);
    }

    re4vr::lua_set_bool("__vr_revolver_cyl_open",
                        m_cyl_st.open || m_cyl_st.prog > 0.15f);
}

// [CHAMBER-ADVANCE] Nach jedem Schuss die Trommel um shot_deg weiterdrehen.
void RE4VRReload2::update_cyl_spin() {
    if (!m_rwep.wid.has_value()) {
        return;
    }

    const auto& r = cyl_cfg(*m_rwep.wid);
    const int32_t seq = static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

    if (!m_spin.prev_seq.has_value()) {
        m_spin.prev_seq = seq;
    } else if (seq > *m_spin.prev_seq) {
        m_spin.target += r.shot_deg * static_cast<float>(seq - *m_spin.prev_seq);
        m_spin.prev_seq = seq;
    }

    const float diff = m_spin.target - m_spin.current;

    if (std::abs(diff) <= 0.5f) {
        m_spin.current = m_spin.target;
    } else {
        m_spin.current += diff * r.shot_lerp;
    }
}

// [SPANNEN-POSE] Cock = EINMALIGE getimte Geste (Daumen drueckt den Sporn hoch,
// KLEBT kurz oben, federt zurueck), NICHT gehaltene Pose. Stick-FLANKE startet.
void RE4VRReload2::update_hammer() {
    if (m_rwep.wid.value_or(0) != 4500) {
        m_ham.hand_frac = 0.0f;
        m_ham.ham_frac = 0.0f;
        m_ham.cocked = false;
        m_ham.cock_running = false;
        m_ham.phase = 0.0f;
        m_ham.key_blend = 0.0f;

        return;
    }

    const float ry = static_cast<float>(re4vr::lua_get_number("__vr_right_stick_y", 0.0));
    const bool stick = (ry <= m_ham.cock_stick_y);

    // [DAUMEN-KEYS] Vorschau am Phasen-Regler hat Vorrang: Daumen UND Hahn
    // stehen exakt da, wo sie in der echten Geste bei dieser Phase stuenden.
    if (m_ham.kprev) {
        const float ph = clamp01(m_ham.kphase / 100.0f);
        m_ham.phase = ph;
        m_ham.key_blend = 1.0f;
        // [EIN SLIDER] Hahn faehrt ueber die erste Haelfte hoch und rastet ein.
        m_ham.ham_frac = ease(std::min(ph / 0.5f, 1.0f));
        m_ham.hand_frac = hand_frac_at(ph);
        m_ham.cock_running = false;

        return;
    }

    // Schuss-Flanke -> entspannen (Hahn faellt)
    const int32_t seq = static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

    if (!m_ham.prev_seq.has_value()) {
        m_ham.prev_seq = seq;
    } else if (seq > *m_ham.prev_seq) {
        m_ham.cocked = false;
        m_ham.cock_running = false;
        m_ham.prev_seq = seq;
    }

    // Stick-Flanke startet die Geste
    if (stick && !m_ham.cock_prev && !m_ham.cocked && !m_ham.cock_running) {
        m_ham.cock_running = true;
        m_ham.cock_t0 = clock_now();
        m_ham.cock_snd = false;
        m_ham.phase = 0.0f;
    }

    m_ham.cock_prev = stick;

    if (m_ham.cock_running) {
        // [EIN SLIDER] EINE Dauer = Phase 0..100 %. Die Daumenform macht die
        // Key-Kurve; hier laufen nur Hahn und Hand-Offset mit.
        const float dur = std::max(m_rcfg.cock_dur, 0.05f);
        const float e = static_cast<float>(clock_now() - m_ham.cock_t0);
        const float ph = std::min(e / dur, 1.0f);
        m_ham.phase = ph;
        m_ham.key_blend = 1.0f;
        m_ham.ham_frac = ease(std::min(ph / 0.5f, 1.0f));

        if (ph >= 0.5f) {
            if (!m_ham.cock_snd) {
                m_ham.cock_snd = true;
                rev_play_sound(SND_COCK);
            }

            m_ham.cocked = true;
        }

        m_ham.hand_frac = hand_frac_at(ph);

        if (ph >= 1.0f) {   // fertig: Daumen am Griff, Hahn bleibt hinten
            m_ham.cock_running = false;
            m_ham.hand_frac = 0.0f;
            m_ham.cocked = true;
            m_ham.ham_frac = 1.0f;
        }

        return;
    }

    // Keine Geste aktiv: Daumen am Griff; Hahn folgt dem Latch.
    m_ham.hand_frac += (0.0f - m_ham.hand_frac) * 0.30f;
    m_ham.key_blend += (0.0f - m_ham.key_blend) * 0.30f;

    const float mt = m_ham.cocked ? 1.0f : 0.0f;
    const float fall = std::clamp(m_rcfg.cock_fall, 0.05f, 1.0f);
    const float hl = m_ham.cocked ? 0.20f : fall;   // Fall schneller als das Spannen
    m_ham.ham_frac += (mt - m_ham.ham_frac) * hl;
}

// [EJECT] Cooler Move: offene Trommel + Waffe gekippt (Muendung hoch -> Kammern
// nach unten) -> die geladenen Kugeln fallen raus (rein visuell, KEIN
// Ammo-Verlust). Solange offen bleiben sie weg, beim Zuklappen sind sie wieder
// da. Wir kennen die geladenen Kugeln via Ammo-Count.
void RE4VRReload2::update_eject() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.has_value()
          && !m_rwep.bullets.empty() && m_rwep.spin_joint != nullptr)) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_revolver_cyl_open") != 1) {
        // Trommel zu -> reset (Kugeln wieder da)
        m_eject.active = false;
        m_eject.items.clear();
        m_eject.armed = true;

        return;
    }

    if (m_eject.active) {
        return;   // schon ausgeworfen (bis Trommel zu)
    }

    // Bohrungs-/Oeffnungs-Achse der Trommel = _05 Welt-Z. Auswurf nur wenn die
    // nach UNTEN zeigt (Gravity).
    glm::quat rot{};

    if (!get_quat(m_rwep.spin_joint, "get_Rotation", rot)) {
        return;
    }

    const glm::vec3 bore = rot * glm::vec3{0.0f, 0.0f, EJECT_ZSIGN};

    if (bore.y < -EJECT_DOWN && m_eject.armed) {
        auto* wi = get_live_wi();
        const int32_t loaded = (wi != nullptr)
            ? call_enum(wi, "get_CurrentAmmoCount").value_or(0) : 0;
        const int32_t n = std::min<int32_t>(loaded,
                                            static_cast<int32_t>(m_rwep.bullets.size()));

        if (n > 0) {
            const float bl = vec_len(bore);
            m_eject.exit = (bl > 1e-6f) ? (bore / bl) : glm::vec3{0.0f, -1.0f, 0.0f};

            // Boden-Hoehe = Spieler-Fusspunkt (body-Position Y).
            auto* bt = body_tf();
            glm::vec3 bp{};
            m_eject.floor_y = (bt != nullptr && get_vec3(bt, "get_Position", bp))
                ? bp.y : -9999.0f;
            m_eject.items.clear();

            for (int32_t i = 1; i <= n; ++i) {
                const auto& b = m_rwep.bullets[static_cast<size_t>(i - 1)];
                glm::vec3 p{};

                if (b.joint == nullptr || !get_vec3(b.joint, "get_Position", p)) {
                    continue;
                }

                EjectItem it{};
                it.joint = b.joint;
                it.vis = b.vis;
                it.sx = p.x;
                it.sy = p.y;
                it.sz = p.z;
                glm::quat r{};

                if (get_quat(b.joint, "get_Rotation", r)) {
                    it.rest_rot = r;
                }

                it.delay = static_cast<double>(m_eject.items.size()) * EJECT_STAGGER;
                it.landed = false;
                // Grad/s Tumble, pro Kugel variiert
                it.wx = 220.0f + static_cast<float>(i) * 47.0f;
                it.wy = 130.0f + static_cast<float>(i) * 29.0f;
                it.wz = 170.0f + static_cast<float>(i) * 61.0f;
                m_eject.items.push_back(it);
            }

            if (!m_eject.items.empty()) {
                m_eject.active = true;
                m_eject.t0 = clock_now();
            }
        }

        m_eject.armed = false;
    } else if (bore.y >= -EJECT_DOWN) {
        m_eject.armed = true;
    }
}

// [EJECT] Pro Kugel: Stagger-Wartezeit -> entlang der Bohrung rausschieben ->
// freier Fall + Taumeln -> am "Boden" Drop-Sound + liegen bleiben. Laeuft NACH
// apply_bullet_visibility (ueberschreibt deren Anzeige).
void RE4VRReload2::apply_eject() {
    if (!(m_eject.active && m_eject.exit.has_value())) {
        return;
    }

    const double now = clock_now();
    const glm::vec3 ex = *m_eject.exit;

    for (auto& it : m_eject.items) {
        set_vec3(it.joint, "set_LocalScale", it.vis);
        const double lt = (now - m_eject.t0) - it.delay;

        if (lt <= 0.0) {
            // noch in der Kammer (Stagger)
            set_vec3(it.joint, "set_Position", glm::vec3{it.sx, it.sy, it.sz});

            continue;
        }

        // entlang Bohrung raus
        const float s = (std::min(static_cast<float>(lt), EJECT_SLIDE_DUR)
                         / EJECT_SLIDE_DUR) * EJECT_SLIDE_DIST;
        float px = it.sx + ex.x * s;
        float py = it.sy + ex.y * s;
        const float pz = it.sz + ex.z * s;

        if (lt > EJECT_SLIDE_DUR) {
            const float ft = static_cast<float>(lt) - EJECT_SLIDE_DUR;
            py -= 0.5f * REV_GRAVITY * ft * ft;   // gerade fallen

            if (py <= m_eject.floor_y) {          // am Boden -> liegen + Sound
                py = m_eject.floor_y;

                if (!it.landed) {
                    it.landed = true;
                    it.land_lt = lt;
                    rev_play_sound(SND_DROP);
                }
            }
        }

        set_vec3(it.joint, "set_Position", glm::vec3{px, py, pz});

        if (it.rest_rot.has_value()) {   // Taumeln (eingefroren beim Aufkommen)
            const float tt = static_cast<float>(it.land_lt.value_or(lt));
            const glm::quat q = quat_from_euler(it.wx * tt, it.wy * tt, it.wz * tt);
            set_quat(it.joint, "set_Rotation", glm::normalize(*it.rest_rot * q));
        }
    }
}

// [WRIST-FLICK CLOSE] Trommel per schnellem Hoch/Runter-Handgelenk-Flick
// ZUSCHNAPPEN (nur schliessen, nicht oeffnen). Misst die
// Pitch-Geschwindigkeit (forward.y der rechten Hand __vr_rh_rot). Grace nach dem
// Aufklappen (Anhebe-Bewegung ignorieren) + Cooldown gegen Doppeltrigger.
void RE4VRReload2::update_close_flick() {
    const double now = clock_now();

    // [SOUND-DELAY] geplanten Zuschnapp-Sound nach fester Zeit spielen
    if (m_flick.snd_at.has_value() && now >= *m_flick.snd_at) {
        rev_play_sound(SND_COCK);   // [SOUND] Flick = Schliessen -> Close-Sound
        m_flick.snd_at.reset();
    }

    const bool open = m_cyl_st.open;

    if (open && !m_flick.was_open) {
        m_flick.open_t = now;   // gerade aufgeklappt
    }

    m_flick.was_open = open;

    if (!(m_rwep.wid.has_value() && open)) {
        m_flick.prev_fy.reset();
        m_flick.peak_dir = 0;

        return;
    }

    const auto rot = re4vr::lua_get_quat("__vr_rh_rot");

    if (!rot.has_value()) {
        m_flick.prev_fy.reset();

        return;
    }

    const glm::vec3 fwd = *rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const float fy = fwd.y;

    // gemerkten Hinschlag vergessen, wenn der Rueckschlag zu spaet kommt
    // (= war ein langsames Kippen)
    if (m_flick.peak_dir != 0 && (now - m_flick.peak_t) > FLICK_REVERSAL) {
        m_flick.peak_dir = 0;
    }

    if (m_flick.prev_fy.has_value() && m_flick.prev_t.has_value()) {
        const double dt = now - *m_flick.prev_t;

        if (dt > 0.001 && dt < 0.2) {
            const float vel = (fy - *m_flick.prev_fy) / static_cast<float>(dt);
            const float thr = static_cast<float>(
                re4vr::lua_get_number("__re4_cyl_flick_vel", FLICK_VEL));

            if (std::abs(vel) > thr) {
                const int dir = (vel > 0.0f) ? 1 : -1;

                if (m_flick.peak_dir != 0 && dir != m_flick.peak_dir   // Richtungs-UMKEHR
                    && (now - m_flick.open_t) > FLICK_OPEN_GRACE
                    && (now - m_flick.last_close) > 0.5) {
                    m_cyl_st.open = false;   // zuschnappen
                    m_flick.snd_at = now + FLICK_SND_DELAY;
                    m_flick.last_close = now;
                    m_flick.peak_dir = 0;
                } else {
                    m_flick.peak_dir = dir;   // Hinschlag merken, auf Rueckschlag warten
                    m_flick.peak_t = now;
                }
            }
        }
    }

    m_flick.prev_fy = fy;
    m_flick.prev_t = now;
}

void RE4VRReload2::rev_on_frame() {
    rev_refresh_weapon();

    if (!m_rwep.wid.has_value()) {
        m_cyl_st._prev_b = false;
        re4vr::lua_set_number("__vr_rev_cock_frac", 0.0);
        // [SHELL-KEYFRAMES] Bahn-Flags beim Ablegen loesen
        m_rev_st.kf_active = false;
        m_rev_st.kf_used = false;

        // [SUPPORT-COOLDOWN] Flag freigeben beim Ablegen
        if (m_rev_st._mih_owned) {
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            m_rev_st._mih_owned = false;
        }

        return;
    }

    // [B-CONSUME] Right-B fuer den nativen Engine-Reload abfangen. Im
    // CAPTURE-Modus EXPLIZIT false -> native Anim laeuft.
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", !m_capture);

    // [SHELL FLOW] Holster-Gate: true = nichts zu greifen (Holster sperrt +
    // buzzt). Nur fuer den Revolver setzen -> ueberschreibt reload's Stale-Wert.
    re4vr::lua_set_bool("__re4_reload_grab_empty", !revolver_can_grab());

    update_cylinder();
    update_close_flick();   // [WRIST-FLICK] offene Trommel per Flick zuschnappen
    update_eject();         // [EJECT] gekippte offene Trommel -> Kugeln fallen raus
    update_cyl_spin();      // [CHAMBER-ADVANCE] Trommel pro Schuss weiterdrehen
    update_hammer();        // [SINGLE-ACTION] Hahn _02 spannen (nur wp4500)

    // [COCK-HAND] Der Hand-Roll/Versatz beim Spannen bleibt (motion liest
    // __vr_rev_cock_frac + __vr_rev_cock_off), raus ist nur die alte Daumenpose.
    // [AIM-HAND] Zwei Offset-Saetze, ausgewaehlt ueber __vr_aim_input (der ECHTE
    // VR-Zielzustand) -- NICHT ueber `is_aim` (get_IsShootEnable, mit gezogener
    // Waffe fast immer true). Zum Tunen haelt hold_hand den Offset.
    if (*m_rwep.wid == 4500) {
        if (m_ham.hold_hand) {
            m_ham.use_aim = m_ham.edit_aim;
        } else {
            m_ham.use_aim = re4vr::lua_get_tribool("__vr_aim_input") == 1;
        }

        const float hf = m_ham.hold_hand ? 1.0f : m_ham.hand_frac;
        re4vr::lua_set_number("__vr_rev_cock_frac", hf);
        const auto& off = m_ham.use_aim ? m_ham.hand_aim : m_ham.hand;
        re4vr::lua_set_cock_off(glm::vec3{off.px, off.py, off.pz},
                                glm::vec3{off.rx, off.ry, off.rz});
    } else {
        re4vr::lua_set_number("__vr_rev_cock_frac", 0.0);
        m_ham.use_aim = false;
    }

    update_reload();   // [RELOAD] Shell-by-Shell: Hand mit Patrone an offene Trommel

    // [SHELL-KEYFRAMES] Waffe fuer die Keyframe-UI exponieren -- reload2 laeuft
    // NACH reload -> ueberschreibt dessen Stale-Global fuer den Revolver.
    // shell_joint BEWUSST NICHT setzen: reload2 positioniert den Clone selbst
    // (reposition_cart_late), sonst zoegen zwei Schreiber am selben Clone.
    if (is_revolver(*m_rwep.wid)) {
        re4vr::lua_set_number("__re4_reload_ui_wid", *m_rwep.wid);
    }

    // [SUPPORT-HAND + COOLDOWN] Patrone in der Hand -> motion Support-Hand AUS;
    // nach dem Einsetzen noch support_cooldown s oben halten. NUR wenn BB
    // gemanagt -> sonst clobbern wir andere Waffen nicht.
    if (*m_rwep.wid == 4500) {
        const bool cd_ok = m_rev_st._insert_t.has_value()
            && (clock_now() - *m_rev_st._insert_t) < m_rcfg.support_cooldown;
        re4vr::lua_set_bool("__vr_mag_in_hand", m_rev_st.cart || cd_ok);
        m_rev_st._mih_owned = true;
    } else if (m_rev_st._mih_owned) {
        re4vr::lua_set_bool("__vr_mag_in_hand", false);   // BB abgelegt -> freigeben
        m_rev_st._mih_owned = false;
    }

    // [FIRE-BLOCK / SINGLE-ACTION] binding liest __vr_block_fire_when_empty ->
    // blockt RT und setzt __re4_empty_trigger_held bei RT-Druck. Regeln NUR
    // wp4500: Trommel offen -> gesperrt; Hahn nicht gespannt -> gesperrt (nur
    // Klick); gespannt + Ammo>0 -> Schuss; gespannt + Ammo==0 -> Dry-Fire.
    const bool cyl_open = re4vr::lua_get_tribool("__vr_revolver_cyl_open") == 1;
    // [MERCS] dort keine Single-Action -> kein "Hahn nicht gespannt"-Feuerblock
    const bool single = (*m_rwep.wid == 4500);
    const bool cocked = m_ham.cocked;

    // [LIVE-EMPTY] Leer NUR aus der Engine (pe:isGunAmmoEmpty, jeden Frame
    // frisch), NIE aus dem gecachten get_live_wi (nach Save-Load stale 0).
    // Plus Unlimited-Bypass (voll ausgebaute Broken Butterfly).
    bool empty4500 = false;
    bool unlimited4500 = false;

    if (single) {
        auto* pe = get_pe();
        bool e = false;
        empty4500 = re4vr::try_call<bool>(pe, "isGunAmmoEmpty", e) && e;
        unlimited4500 = RE4VRWeapons2::get()->is_unlimited();
    }

    if (single) {
        re4vr::lua_set_bool("__vr_block_fire_when_empty",
                            cyl_open || ((!cocked) && !unlimited4500)
                            || (empty4500 && !unlimited4500));
        // [BF-DIAG] wer hat den Feuer-Block zuletzt gesetzt?
        re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:897");
    } else {
        re4vr::lua_set_bool("__vr_block_fire_when_empty", cyl_open);
        re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:899");
    }

    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_rev_st._dry_prev) {
        if (single && cocked && !cyl_open) {
            // Leerer Single-Action-Schuss: Klick + Trommel einen Schritt weiter
            // + Hahn entspannen.
            rev_play_sound(SND_DRY_FIRE);
            m_spin.target += cyl_cfg(*m_rwep.wid).shot_deg;
            m_ham.cocked = false;
        } else if (cyl_open) {
            rev_play_sound(SND_DRY_FIRE);   // offene Trommel -> Klick
        } else if (single) {
            // Hahn nicht gespannt + Trommel zu: nur Klick (kein Spin/Uncock).
            rev_play_sound(SND_DRY_FIRE);
        }
    }

    m_rev_st._dry_prev = et;
}

// [CYLINDER] Override-Pass (voller Stack, nach der Engine-Anim).
void RE4VRReload2::apply_cylinder_pass() {
    if (m_capture) {
        return;   // [CAPTURE] native Rotation NICHT ueberschreiben
    }

    if (!(m_rcfg.revolver_enabled && m_rwep.cyl_joint != nullptr
          && m_rwep.cyl_rest_rot.has_value())) {
        return;
    }

    const auto& r = cyl_cfg(m_rwep.wid.value_or(0));
    const bool has_pos = (r.px != 0.0f || r.py != 0.0f || r.pz != 0.0f);
    const float p = m_cyl_st.prog;

    if (p <= 0.0001f) {
        // zu: Rotation der Engine ueberlassen; Position aber explizit auf Ruhe
        // zuruecksetzen, sonst bliebe der Translations-Offset haengen
        // (LocalPosition wird nicht zwingend pro Frame animiert).
        if (has_pos && m_rwep.cyl_rest_pos.has_value()) {
            set_vec3(m_rwep.cyl_joint, "set_LocalPosition", *m_rwep.cyl_rest_pos);
        }

        return;
    }

    // [ROT] Crane-Dreh-Revolver: Trommel-Joint um (rx,ry,rz)*prog drehen.
    const glm::quat q = quat_from_euler(r.rx * p, r.ry * p, r.rz * p);
    set_quat(m_rwep.cyl_joint, "set_LocalRotation",
             glm::normalize(*m_rwep.cyl_rest_rot * q));

    // [POS] Translations-Revolver (Broken Butterfly 4500): Trommel per
    // LocalPosition-Offset rausfahren.
    if (has_pos && m_rwep.cyl_rest_pos.has_value()) {
        const glm::vec3 rp = *m_rwep.cyl_rest_pos;
        set_vec3(m_rwep.cyl_joint, "set_LocalPosition",
                 glm::vec3{rp.x + r.px * p, rp.y + r.py * p, rp.z + r.pz * p});
    }
}

// [BULLETS] Aktuellen Trommel-Ammo-Stand lesen. KONSISTENZ: bevorzugt das
// Live-WeaponItem (dasselbe, auf das der Insert schreibt), Fallback
// PlayerEquipment.getCurrentGunAmmo.
std::optional<int32_t> RE4VRReload2::get_gun_ammo() {
    if (auto* wi = get_live_wi(); wi != nullptr) {
        if (const auto a = call_enum(wi, "get_CurrentAmmoCount"); a.has_value()) {
            return a;
        }
    }

    auto* pe = get_pe();

    if (pe == nullptr) {
        return std::nullopt;
    }

    return call_enum(pe, "getCurrentGunAmmo");
}

// [BULLETS] Nur so viele Kugel-Joints zeigen wie Ammo da ist. Versteckt via
// LocalScale 0 (sichtbar = gemerkter Ruhe-Scale). MaxCap > Kammerzahl -> auf
// die Kammerzahl geclamped. Native zeigt sonst immer alle.
void RE4VRReload2::apply_bullet_visibility() {
    if (m_capture) {
        return;
    }

    if (!(m_rcfg.revolver_enabled && !m_rwep.bullets.empty())) {
        return;
    }

    const int32_t cap = static_cast<int32_t>(m_rwep.bullets.size());
    const auto a = get_gun_ammo();

    if (!a.has_value()) {
        return;
    }

    const int32_t ammo = std::clamp(*a, 0, cap);

    for (int32_t i = 1; i <= cap; ++i) {
        const auto& b = m_rwep.bullets[static_cast<size_t>(i - 1)];
        set_vec3(b.joint, "set_LocalScale",
                 (i <= ammo) ? b.vis : glm::vec3{0.0f, 0.0f, 0.0f});
    }
}

// [SHELL-IN-HAND] Finger-Pose (aus reload.json POSES) auf die linke Hand legen,
// solange die Trommel OFFEN ist (Reload-Phase). Plus additive Daumen-Spreizung.
// reload loescht __vr_mag_hand_pose fuer den Revolver (unmanaged) -> wir wenden
// die Pose direkt an.
void RE4VRReload2::apply_shell_pose() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.has_value())) {
        return;
    }

    const auto sit = m_shell.find(*m_rwep.wid);
    const bool have = (sit != m_shell.end());
    std::string want{};

    if (have && (m_rev_st.cart || m_rev_st.preview) && !sit->second.pose.empty()) {
        want = sit->second.pose;
    }

    // [POSE_FADE] beim Loslassen ueber POSE_FADE_DUR zurueckblenden statt snappen.
    // Der Lua-Cache der Shell-Daten steckt hier in m_shellfade.flag: er merkt
    // sich, dass beim Ausblenden weiter DIESE Waffe gemeint war.
    float b = 0.0f;

    if (!pose_fade_step(m_shellfade, want, b)) {
        return;
    }

    if (!have) {
        return;
    }

    const auto& s = sit->second;

    if (m_main != nullptr) {
        m_main->apply_pose(m_shellfade.name, b);
    }

    auto* bt = body_tf();

    // Daumen-Spreizung additiv auf L_Thumb1 (NACH der Pose), per-Waffe.
    if (s.t_rx != 0.0f || s.t_ry != 0.0f || s.t_rz != 0.0f) {
        auto* thumb = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (thumb != nullptr && get_quat(thumb, "get_LocalRotation", cur)) {
            const glm::quat q = quat_from_euler(s.t_rx * b, s.t_ry * b, s.t_rz * b);
            set_quat(thumb, "set_LocalRotation", glm::normalize(cur * q));
        }
    }

    // [ZEIGEFINGER] additiv auf L_IndexF1/2/3 (Greif-Pose der Patrone).
    if (s.i_rx != 0.0f || s.i_ry != 0.0f || s.i_rz != 0.0f) {
        static const char* const IDX[3] = {"L_IndexF1", "L_IndexF2", "L_IndexF3"};

        for (const char* jn : IDX) {
            auto* jt = (bt != nullptr) ? joint_by_name(bt, jn) : nullptr;
            glm::quat cur{};

            if (jt != nullptr && get_quat(jt, "get_LocalRotation", cur)) {
                const glm::quat q = quat_from_euler(s.i_rx * b, s.i_ry * b, s.i_rz * b);
                set_quat(jt, "set_LocalRotation", glm::normalize(cur * q));
            }
        }
    }
}

// [SHELL-IN-HAND MESH-CLONE] Patrone in der Hand = eigenstaendiger MESH-CLONE
// der Patronen-Parts (Standard 20=Spitze + 30=Patrone) auf einem eigenen
// GameObject -> die echte Trommel bleibt unberuehrt (KEIN Loch, MIT Spitze).
// Technik wie Red9: via.motion.Motion (baut Skelett) + via.render.Mesh +
// setMesh(lebender Gun-Holder) + Gun-Material + Parts isolieren.
::REManagedObject* RE4VRReload2::rev_gun_mesh() {
    if (m_rwep.tf == nullptr) {
        return nullptr;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_rwep.tf, "get_GameObject");

    return (go != nullptr) ? re4vr::get_component(go, "via.render.Mesh") : nullptr;
}

void RE4VRReload2::cart_destroy() {
    if (m_cart.obj != nullptr) {
        re4vr::destroy_game_object(m_cart.obj);
    }

    m_cart.obj = nullptr;
    m_cart.mesh = nullptr;
    m_cart.wid.reset();
    m_cart.parts_sig.clear();
    m_cart.parented = false;
}

bool RE4VRReload2::cart_spawn() {
    if (m_cart.obj != nullptr) {
        return true;
    }

    auto* gmesh = rev_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_revolver_cart");

    if (go == nullptr) {
        return false;
    }

    auto* gom = reinterpret_cast<::REManagedObject*>(go);

    // KERN: baut das Skelett -- ohne die Motion-Komponente bleibt der Mesh leer.
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

    m_cart.obj = gom;
    m_cart.mesh = mesh;
    m_cart.wid = m_rwep.wid;

    // [NO_LAG] nativ ans L_Hand-Joint parenten -> Engine propagiert die
    // Transform VOR dem Skinning -> kein Render-Versatz beim Laufen. Danach nur
    // noch LOKALE Pose setzen.
    m_cart.parented = false;
    auto* ctf = re4vr::call_safe<::REManagedObject*>(gom, "get_Transform");
    auto* bt2 = body_tf();

    if (ctf != nullptr && bt2 != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", bt2);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_ParentJoint", jn);
            m_cart.parented = true;
        }
    }

    return true;
}

void RE4VRReload2::cart_isolate() {
    if (m_cart.mesh == nullptr) {
        return;
    }

    const auto sit = m_shell.find(m_rwep.wid.value_or(0));
    const std::string parts = (sit != m_shell.end()) ? sit->second.parts : "20,30";
    const std::string sig = "parts:" + parts;

    if (m_cart.parts_sig == sig) {
        return;
    }

    const auto keep = parse_parts(parts);
    bool applied = true;

    for (int32_t i = 0; i <= 48; ++i) {
        const bool on = std::find(keep.begin(), keep.end(), i) != keep.end();

        if (!re4vr::obj_ok(m_cart.mesh)) {
            applied = false;

            break;
        }

        re4vr::call_safe<void*>(m_cart.mesh, "setPartsEnable", i, on);
    }

    if (applied) {
        m_cart.parts_sig = sig;
    }
}

// [BAHN KLEBT AN DER WAFFE 2026-09-07] Wortgleich zu
// RE4VRReload3::hc_cart_parent_mode, wo dieser Umbau seit 01.09. laeuft
// ("der Patronen-Clone haengt im Keyframe-Modus an der WAFFEN-Transform statt
// frei in der Welt -- vorher zog er beim Laufen sichtbar nach").
// Waehrend der Keyframe-Bahn haengt der Klon an der WAFFE (die Keyframes sind
// waffenrelativ), sonst wie bisher am L_Hand-Joint.
namespace {
// [STALE-PARENT-CRASH] via.Transform.set_Parent / set_ParentJoint mit einer
// freigegebenen Transform loest eine native Access Violation (c0000005) aus --
// try/catch faengt die NICHT. Deshalb vor jedem Umhaengen BEIDE Seiten pruefen.
// Siehe [[feedback_set_parent_stale_av_crash]].
bool tf_valid(::REManagedObject* o) {
    if (!re4vr::obj_ok(o)) {
        return false;
    }

    const auto v = re4vr::call_num(o, "get_Valid");

    return v.has_value() && *v != 0.0;
}
}   // namespace

void RE4VRReload2::cart_parent_mode(const char* mode) {
    if (m_cart.pmode == mode) {
        return;
    }

    auto* tf = (m_cart.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform") : nullptr;

    if (!tf_valid(tf)) {
        return;
    }

    if (std::string{mode} == "weapon") {
        if (tf_valid(m_rwep.tf)) {
            re4vr::call_safe<void*>(tf, "set_Parent", m_rwep.tf);
            m_cart.pmode = "weapon";
            m_cart.parented = false;
        }

        return;
    }

    auto* bt3 = body_tf();

    if (!tf_valid(bt3)) {
        return;
    }

    re4vr::call_safe<void*>(tf, "set_Parent", bt3);

    // [FALLE] set_ParentJoint braucht einen MANAGED String.
    auto* jn = sdk::VM::create_managed_string(L"L_Hand");

    if (jn != nullptr) {
        re4vr::call_safe<void*>(tf, "set_ParentJoint", jn);
        m_cart.pmode = "hand";
        m_cart.parented = true;
    }
}

// [BAHN KLEBT AN DER WAFFE 2026-09-07] Wie cart_parent_mode, aber fuer den
// Red9-Clip. Beim Tragen haengt er am L_Hand-Joint; fuer die Keyframe-Bahn
// gehoert er an die WAFFE, weil die Keyframes waffenrelativ sind.
void RE4VRReload2::r9_clip_parent_mode(const char* mode) {
    if (m_r9clip.pmode == mode) {
        return;
    }

    auto* tf = (m_r9clip.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform") : nullptr;

    if (!tf_valid(tf)) {
        return;
    }

    if (std::string{mode} == "weapon") {
        auto* gtf = rget_gun_tf();

        if (tf_valid(gtf)) {
            re4vr::call_safe<void*>(tf, "set_Parent", gtf);
            m_r9clip.pmode = "weapon";
            m_r9clip.parented = false;
        }

        return;
    }

    auto* bt3 = body_tf();

    if (!tf_valid(bt3)) {
        return;
    }

    re4vr::call_safe<void*>(tf, "set_Parent", bt3);

    auto* jn = sdk::VM::create_managed_string(L"L_Hand");

    if (jn != nullptr) {
        re4vr::call_safe<void*>(tf, "set_ParentJoint", jn);
        m_r9clip.pmode = "hand";
        m_r9clip.parented = true;
    }
}

void RE4VRReload2::cart_set_tf(::REManagedObject* tf, const glm::vec3& p,
                               const std::optional<glm::quat>& rot, float scl) {
    // no_dirty gegen Jitter (wie Red9)
    sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf),
                                Vector4f{p.x, p.y, p.z, 1.0f}, true);

    if (rot.has_value()) {
        sdk::set_transform_rotation(reinterpret_cast<::RETransform*>(tf), *rot);
    }

    set_vec3(tf, "set_LocalScale", glm::vec3{scl, scl, scl});
}

void RE4VRReload2::apply_held_cartridge() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.has_value())) {
        return;
    }

    const int32_t wid = *m_rwep.wid;

    // Anzeigen wenn Patrone in der Hand ODER UI-Vorschau. Der Fall wird in
    // apply_cart_drop bewegt.
    // [SHELL-KEYFRAMES] Keyframe-Preview an (reload_adv, DIREKT abgefragt) ->
    // Clone spawnen, damit man ihn OHNE Patrone in der Hand tunen kann.
    const bool kfp = (m_adv != nullptr) && m_adv->shell_preview
                     && m_adv->is_keyframe_insert(wid);

    if (!(m_rev_st.cart || m_rev_st.preview || m_drop2.active || kfp)) {
        if (m_cart.obj != nullptr) {
            cart_destroy();
        }

        return;
    }

    if (m_drop2.active) {
        return;   // Drop hat eigene Bewegung
    }

    if (m_cart.obj != nullptr && m_cart.wid.value_or(0) != wid) {
        cart_destroy();   // Waffenwechsel -> neu
    }

    if (m_cart.obj == nullptr && !cart_spawn()) {
        return;
    }

    cart_isolate();

    const auto sit = m_shell.find(wid);
    const Shell s = (sit != m_shell.end()) ? sit->second : Shell{};

    // [SHELL-KEYFRAMES] Bahn ODER Keyframe-Preview -> die Clone-Position macht
    // der Keyframe-Pass (reposition_cart_late), hier NICHT an die Hand ziehen.
    if (m_rev_st.kf_active || kfp) {
        // Preview: Clone entkoppeln + sichtbar skalieren (der Keyframe-Pass
        // setzt nur Position/Rotation, nicht die Scale).
        if (kfp) {
            auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");

            if (tf != nullptr) {
                if (m_cart.parented) {
                    re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                    m_cart.parented = false;
                }

                set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});
            }
        }

        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    // [NO_LAG BAHN 2026-09-07] aus dem Waffen-Parent zurueck an die Hand --
    // wie RE4VRReload3 es an derselben Stelle tut.
    cart_parent_mode("hand");

    // [NO_LAG] geparentet ans L_Hand: nur LOKALE Pose (der Offset war schon
    // hand-relativ = jetzt lokal).
    if (m_cart.parented) {
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

    cart_set_tf(tf, w, rot, s.scale);
}

// [DROP] Fallengelassene Patrone: den Mesh-Clone von seiner Loslass-Position
// frei fallen lassen (Deko, ~1 s), danach zerstoeren. Trommel bleibt unberuehrt.
void RE4VRReload2::apply_cart_drop() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.has_value() && m_drop2.active)) {
        return;
    }

    if (m_cart.obj == nullptr) {
        m_drop2.active = false;

        return;
    }

    const float t = static_cast<float>(clock_now() - m_drop2.t0);

    if (t > REV_DROP_DUR) {
        m_drop2.active = false;
        cart_destroy();

        return;
    }

    if (!m_drop2.snd && t >= 0.45f) {   // Boden-Aufprall (0.45 s ~ Falldauer)
        m_drop2.snd = true;
        rev_play_sound(SND_DROP);
    }

    const float fall = 0.5f * REV_GRAVITY * t * t;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    const auto sit = m_shell.find(*m_rwep.wid);
    const float scl = (sit != m_shell.end()) ? sit->second.scale : 1.0f;
    cart_set_tf(tf, glm::vec3{m_drop2.sx, m_drop2.sy - fall, m_drop2.sz},
                std::nullopt, scl);
}

// [WOBBLE-FIX] Patrone NACH motions finalem L_Hand-Write nachziehen.
// apply_held_cartridge laeuft im BeginRendering-PRE-Pass -> liest die Hand BEVOR
// motion sie auf die VR-Endlage setzt (attach_left_hand im POST) -> die Patrone
// haengt einen Pass hinterher = Wabbeln. Diese schlanke Variante laeuft im POST
// und repositioniert NUR (kein Spawn/Isolate) auf die finale Hand.
void RE4VRReload2::reposition_cart_late() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.has_value() && m_cart.obj != nullptr)) {
        return;
    }

    if (m_drop2.active) {
        return;
    }

    const int32_t wid = *m_rwep.wid;
    const auto sit = m_shell.find(wid);
    const Shell s = (sit != m_shell.end()) ? sit->second : Shell{};
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_cart.obj, "get_Transform");

    // [SHELL-KEYFRAMES] Bahn aktiv -> Clone entlang der Keyframes fahren
    // (relativ zur Waffe), statt ihn an der Hand zu halten.
    if (m_rev_st.kf_active) {
        RE4VRReloadAdv::Key k{};

        if (m_adv != nullptr && tf != nullptr && m_rwep.tf != nullptr) {
            const float dur = std::max(m_adv->shell_dur, 0.01f);
            float tt = static_cast<float>(clock_now() - m_rev_st.kf_t0) / dur;

            if (tt > 1.0f) {
                tt = 1.0f;
            }

            // [KF_LEAD] Eingangspunkt: erste Frames fest auf Keyframe #1
            // (kein Sprung beim Uebergang).
            const double lead = re4vr::lua_get_number("__re4_kf_lead_dur", 0.035);

            if ((clock_now() - m_rev_st.kf_t0) < lead) {
                tt = 0.0f;
            }

            glm::vec3 gp{};
            glm::quat gr{};

            if (m_adv->shell_pose_at(wid, tt, k)
                && get_vec3(m_rwep.tf, "get_Position", gp)
                && get_quat(m_rwep.tf, "get_Rotation", gr)) {
                // [AN DER WAFFE KLEBEN 2026-09-07] Nicht mehr jeden Frame eine
                // Weltposition schreiben -- die ist beim Rendern schon ueberholt,
                // sobald man laeuft, und auf dem an L_Hand geparenteten Klon
                // schiebt die Engine sie zusaetzlich mit der Hand mit.
                // Stattdessen den Klon fuer die Bahn an die WAFFEN-Transform
                // haengen und die Keyframe-Lage als LOKALE Pose setzen: die
                // Keyframes sind ohnehin waffenrelativ, also klebt er vom ersten
                // bis zum letzten Keyframe an der Waffe und die Engine zieht ihn
                // mit -- genau wie einen Waffen-Joint.
                cart_parent_mode("weapon");

                set_vec3(tf, "set_LocalPosition", glm::vec3{k.x, k.y, k.z});
                set_quat(tf, "set_LocalRotation",
                         glm::normalize(quat_from_euler(k.rx, k.ry, k.rz)));
                set_vec3(tf, "set_LocalScale", glm::vec3{s.scale, s.scale, s.scale});

            }

            if (tt >= 1.0f) {
                // [KF_HOLD] Nicht im selben Frame abschalten: sonst blitzt der
                // Clone einmal an seiner alten Lage auf (Befund von der
                // Armbrust). Erst nach dem Haltefenster beenden -- bis dahin
                // steht er auf dem LETZTEN Keyframe.
                if (!m_rev_st.kf_hold_t.has_value()) {
                    m_rev_st.kf_hold_t = clock_now()
                        + re4vr::lua_get_number("__re4_kf_hold_dur", 0.035);
                }

                if (clock_now() >= *m_rev_st.kf_hold_t) {
                    m_rev_st.kf_hold_t.reset();
                    m_rev_st.kf_active = false;
                    // Bahn (Deko) fertig -> Clone entfernen; die +1 sass schon
                    // beim Bahn-Start.
                    m_rev_st.cart = false;
                }
            }
        } else {
            m_rev_st.kf_active = false;
        }

        return;
    }

    // [SHELL-KEYFRAMES] Keyframe-Preview: den Clone-Pfad NACHBAUEN, nur relativ
    // zur WAFFE + Tuning-Lage statt zur Hand -> der Clone wird garantiert
    // sichtbar gesetzt.
    if (m_adv != nullptr && m_adv->shell_preview && m_adv->is_keyframe_insert(wid)) {
        const auto& sl = m_adv->shell_live;
        glm::vec3 gp{};
        glm::quat gr{};

        if (tf != nullptr && m_rwep.tf != nullptr
            && get_vec3(m_rwep.tf, "get_Position", gp)
            && get_quat(m_rwep.tf, "get_Rotation", gr)) {
            const glm::vec3 pos = gp + (gr * glm::vec3{sl.x, sl.y, sl.z});
            const glm::quat rot = glm::normalize(gr * quat_from_euler(sl.rx, sl.ry, sl.rz));
            cart_set_tf(tf, pos, rot, s.scale);
        }

        return;
    }

    if (!(m_rev_st.cart || m_rev_st.preview)) {
        return;
    }

    if (tf == nullptr) {
        return;
    }

    // [NO_LAG] geparentet ans L_Hand: nur LOKALE Pose.
    if (m_cart.parented) {
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

    cart_set_tf(tf, w, rot, s.scale);
}

// [SPIN-LOCK] Native Chamber-Spin-Animation der Trommel (_05 Eigenrotation)
// unterdruecken: lokale Rotation jeden Pass auf die gemerkte Ruhe halten.
// Stoppt das "Selbstdrehen" nach Empty-Reload (stale Anim). Der Aufklapp-Swing
// bleibt, weil der ueber den PARENT _04 laeuft (nicht _05 lokal).
void RE4VRReload2::apply_cylinder_spin_lock() {
    if (!(m_rcfg.revolver_enabled && m_rwep.spin_joint != nullptr
          && m_rwep.spin_rest_rot.has_value())) {
        return;
    }

    glm::quat rot = *m_rwep.spin_rest_rot;

    if (m_spin.current != 0.0f) {
        // [CHAMBER-ADVANCE] Schuss-Drehung additiv um Z auf die Ruhe
        rot = glm::normalize(*m_rwep.spin_rest_rot
                             * quat_from_euler(0.0f, 0.0f, m_spin.current));
    }

    set_quat(m_rwep.spin_joint, "set_LocalRotation", rot);
}

// [SPANNEN-POSE] Hahn _02 (NUR wp4500): Winkel kommen aus hammer_cfg
// (idle <-> cocked, je 3 Achsen), gelerpt mit frac.
void RE4VRReload2::apply_hammer_pass() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.value_or(0) == 4500
          && m_rwep.hammer_joint != nullptr && m_rwep.hammer_rest_rot.has_value())) {
        return;
    }

    const auto& h = hammer_cfg(4500);
    const float f = m_ham.ham_frac;
    const float ax = h.idle_rx + (h.rx - h.idle_rx) * f;
    const float ay = h.idle_ry + (h.ry - h.idle_ry) * f;
    const float az = h.idle_rz + (h.rz - h.idle_rz) * f;
    set_quat(m_rwep.hammer_joint, "set_LocalRotation",
             glm::normalize(*m_rwep.hammer_rest_rot * quat_from_euler(ax, ay, az)));
}

// [SPANNEN-POSE] Daumen R_Thumb1/2/3 (NUR wp4500). Der Daumen kommt
// AUSSCHLIESSLICH aus den Keyframes; greifen die nicht, bleibt die
// Engine-Animation stehen (der alte 2-Punkt-Pfad ist 2026-08-04 raus).
void RE4VRReload2::apply_thumb_pass() {
    if (!(m_rcfg.revolver_enabled && m_rwep.wid.value_or(0) == 4500)) {
        return;
    }

    auto* bt = body_tf();

    if (bt == nullptr) {
        return;
    }

    const float kb = m_ham.key_blend;

    // [ZWEI KEY-SAETZE] HUEFTE und AIM haben je eine Key-Liste. Live entscheidet
    // __vr_aim_input (der ECHTE VR-Zielzustand) -- NICHT `is_aim`
    // (get_IsShootEnable, mit gezogener Waffe fast immer true). In der Vorschau
    // gilt der im UI gewaehlte Satz. Leerer Satz -> der andere greift.
    const bool kaim = m_ham.kprev ? m_ham.key_aim
                                  : (re4vr::lua_get_tribool("__vr_aim_input") == 1);
    const std::vector<ThumbKey>* KL = &thumb_keys(4500, kaim);

    if (KL->empty()) {
        KL = &thumb_keys(4500, !kaim);
    }

    // [VORSCHAU] Vorschau an = die REGLER stehen am Daumen, immer.
    const bool klive = m_ham.kprev;

    if (!(kb > 0.0001f && (klive || !KL->empty()))) {
        return;
    }

    auto& out = m_ham.kout;

    if (klive) {
        out = m_ham.kedit;
    } else {
        thumb_key_sample(*KL, m_ham.phase * 100.0f, out);
    }

    static const char* const TJ[3] = {"R_Thumb1", "R_Thumb2", "R_Thumb3"};

    for (size_t i = 0; i < 3; ++i) {
        auto* jt = joint_by_name(bt, TJ[i]);
        glm::quat cur{};

        if (jt == nullptr || !get_quat(jt, "get_LocalRotation", cur)) {
            continue;
        }

        const auto& o = out[i];
        // [DAUMEN-KEYS ABSOLUT] Keyframes muessen die Anim ERSETZEN: gegen die
        // BIND-Pose rechnen (get_BaseLocalRotation, konstant, kein Engine-State)
        // und mit kb von der Anim dorthin slerpen. kb = 1 -> der Daumen steht
        // exakt auf dem Key, die Anim ist fuer die Dauer der Geste egal.
        glm::quat rest{};

        if (!get_quat(jt, "get_BaseLocalRotation", rest)) {
            rest = cur;
        }

        const glm::quat tgt = glm::normalize(rest * quat_from_euler(o.x, o.y, o.z));
        const glm::quat nr = (kb < 0.999f) ? glm::slerp(cur, tgt, kb) : tgt;
        set_quat(jt, "set_LocalRotation", nr);

        // [DAUMEN-KEYS] Versatz PRO GLIED (nicht nur an der Basis) -> "strecken"
        // ist keyframebar. Ebenfalls absolut: Ziel = BIND-Position + Key-Versatz,
        // mit kb eingeblendet.
        glm::vec3 restp{};
        glm::vec3 lp{};

        if (get_vec3(jt, "get_BaseLocalPosition", restp)
            && get_vec3(jt, "get_LocalPosition", lp)) {
            const glm::vec3 t{restp.x + o.px, restp.y + o.py, restp.z + o.pz};
            set_vec3(jt, "set_LocalPosition", lp + (t - lp) * kb);
        }
    }
}

void RE4VRReload2::apply_revolver_pass() {
    apply_cylinder_pass();
    apply_cylinder_spin_lock();   // _05 Eigenrotation auf Ruhe -> kein Selbstdrehen
    apply_hammer_pass();          // [SINGLE-ACTION] Hahn _02 (nur wp4500)
    apply_thumb_pass();           // [SINGLE-ACTION] rechter Daumen synchron
    apply_bullet_visibility();
    apply_held_cartridge();       // geliehene Kammer-Kugel an der Hand
    apply_cart_drop();            // fallengelassene Patrone (freier Fall)
    apply_eject();                // [EJECT] geladene Kugeln rausfallen lassen
    // hide_hand_cartridge_when_idle() ist in Lua ein no-op -- apply_held_cartridge
    // zerstoert den Clone bereits, wenn nichts gehalten wird.
    apply_shell_pose();
}

void RE4VRReload2::rev_on_script_reset() {
    cart_destroy();
    m_eject.active = false;
    m_eject.items.clear();
    m_drop2.active = false;
    m_rev_st = Rev2State{};
}

// ---------------------------------------------------------------------
// UI -- Revolver. In Lua stehen alle sechs Baeume in EINEM on_draw_ui unter
// "RE4VR - Reload2"; hier ist jede Maschine eine eigene Funktion, die
// on_draw_ui in derselben Reihenfolge aufruft.
// ---------------------------------------------------------------------
namespace {

std::string wp_label(const std::optional<int32_t>& wid) {
    if (!wid.has_value()) {
        return "-";
    }

    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "wp%04d", *wid);

    return buf;
}

}   // namespace

void RE4VRReload2::rev_ui() {
    if (ImGui::Checkbox("##rev_en", &m_rcfg.revolver_enabled)) {
        rev_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Revolver Reload");

    if (!ImGui::TreeNode("Revolver -- Einstellungen")) {
        return;
    }

    const auto awid = m_rwep.wid.has_value() ? m_rwep.wid : get_equip_wid();
    ImGui::Text("Equippt: %s", wp_label(awid).c_str());
    const bool known = awid.has_value() && is_revolver(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Revolver erkannt - verwaltet)"
                             : "  (kein eingerichteter Revolver equippt)");

    const auto jc = awid.has_value() ? rev_joints(*awid) : std::nullopt;
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Joints: Trommel=%s  Patrone=%s  cyl_joint=%s",
                       jc.has_value() ? jc->cylinder : "nil",
                       jc.has_value() ? jc->bullet : "nil",
                       m_rwep.cyl_joint != nullptr ? "ok" : "nil");

    if (ImGui::SliderFloat("Einlege-Distanz m (Zylinder)##revdist",
                           &m_rcfg.insert_distance, 0.03f, 0.50f)) {
        rev_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen", &m_rcfg.reload_ammo)) {
        rev_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an", &m_rcfg.sound_enabled)) {
        rev_save_cfg();
    }

    // [CYLINDER] Ausschwenk-Tuning (Trommel-Joint, RIGHT-B togglet)
    if (ImGui::TreeNode("Trommel ausschwenken (RIGHT-B)")) {
        auto& r = cyl_cfg(awid.value_or(5001));
        bool pc = false;
        ImGui::Checkbox("Vorschau: Trommel per Regler##cylprev", &m_cyl_st.preview);

        if (m_cyl_st.preview) {
            ImGui::SliderFloat("Vorschau 0..1##cylprog", &m_cyl_st.prog, 0.0f, 1.0f);
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Rotation (Crane-Dreh, z.B. 4502):");
        pc |= ImGui::SliderFloat("Auf RotX (Grad)##cylrx", &r.rx, -180.0f, 180.0f);
        pc |= ImGui::SliderFloat("Auf RotY (Grad)##cylry", &r.ry, -180.0f, 180.0f);
        pc |= ImGui::SliderFloat("Auf RotZ (Grad)##cylrz", &r.rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Position (Trommel faehrt raus, Broken Butterfly 4500):");
        pc |= ImGui::SliderFloat("Auf PosX (m)##cylpx", &r.px, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Auf PosY (m)##cylpy", &r.py, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Auf PosZ (m)##cylpz", &r.pz, -0.15f, 0.15f);
        pc |= ImGui::SliderFloat("Ausschwenk-Geschwindigkeit (lerp)##cyllerp",
                                 &r.lerp, 0.01f, 0.30f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Chamber-Advance (Trommel-Dreh pro Schuss):");
        pc |= ImGui::SliderFloat("Grad pro Schuss##cylshotdeg", &r.shot_deg, -90.0f, 90.0f);
        pc |= ImGui::SliderFloat("Dreh-Geschw. (lerp)##cylshotlerp", &r.shot_lerp, 0.02f, 1.0f);

        if (ImGui::Button("Ruhe-Rotation neu erfassen##cylrest") && m_rwep.cyl_joint != nullptr) {
            glm::quat q{};

            if (get_quat(m_rwep.cyl_joint, "get_LocalRotation", q)) {
                m_rwep.cyl_rest_rot = q;
            }
        }

        if (pc) {
            rev_save_cfg();
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  rest=%s prog=%.2f open=%s  (RIGHT-B togglet)",
                           m_rwep.cyl_rest_rot.has_value() ? "ok" : "nil", m_cyl_st.prog,
                           re4vr::lua_get_tribool("__vr_revolver_cyl_open") == 1
                               ? "true" : "false");
        ImGui::TreePop();
    }

    // [SPANNEN-POSE] EINE komplette rechte-Hand-Pose fuer "spannen noetig" (NUR
    // wp4500). Hier wird ALLES getunt.
    if (ImGui::TreeNode("Hahn spannen (nur Broken Butterfly)")) {
        auto& hh = hammer_cfg(4500);
        bool ch = false;
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Spann-Geste Timing (einmalige Bewegung):");

        // [EIN SLIDER] EINE Dauer fuer die ganze Geste; die Daumenform kommt aus
        // den Keys.
        if (ImGui::SliderFloat("Dauer der ganzen Spann-Geste (s)##cockdur",
                               &m_rcfg.cock_dur, 0.10f, 2.00f)) {
            rev_save_cfg();
        }

        if (ImGui::SliderFloat("Hahn-Fall Tempo (lerp)##cockfall",
                               &m_rcfg.cock_fall, 0.05f, 1.0f)) {
            rev_save_cfg();
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hahn _02 ENTSPANNT (vorne, idle) -- Grad:");
        ch |= ImGui::DragFloat("Idle Hahn X##hidx", &hh.idle_rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Idle Hahn Y##hidy", &hh.idle_ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Idle Hahn Z##hidz", &hh.idle_rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hahn _02 GESPANNT (hinten, cocked) -- Grad:");
        ch |= ImGui::DragFloat("Cocked Hahn X##hcx", &hh.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Cocked Hahn Y##hcy", &hh.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("Cocked Hahn Z##hcz", &hh.rz, 0.5f, -180.0f, 180.0f, "%.1f");

        // [HAND-OFFSET] Hand-Lage beim Spannen. Zwei Saetze (HUEFTE/AIM); welcher
        // unter den Reglern liegt, sagt die Checkbox. Im Spiel (Vorschau aus)
        // gilt der Satz zum echten Zielzustand (__vr_aim_input).
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Hand-Offset beim Spannen:");
        ImGui::Checkbox("Hand-Offset dauerhaft zeigen (zum Tunen)##holdhand", &m_ham.hold_hand);
        ImGui::Checkbox("AIM-Werte##editaim", &m_ham.edit_aim);
        const bool ea = m_ham.edit_aim;
        ImGui::TextColored(ea ? ImVec4{1.0f, 0.65f, 0.0f, 1.0f}
                              : ImVec4{0.4f, 1.0f, 0.4f, 1.0f},
                           ea ? ">>> es werden die AIM-Werte verstellt <<<"
                              : ">>> es werden die HUEFT-Werte verstellt <<<");

        auto& hs = ea ? m_ham.hand_aim : m_ham.hand;
        const std::string tag = ea ? "AIM" : "HUEFTE";
        ch |= ImGui::DragFloat((tag + " Roll X##ckrx").c_str(), &hs.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat((tag + " Roll Y##ckry").c_str(), &hs.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat((tag + " Roll Z##ckrz").c_str(), &hs.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat((tag + " Pos X##ckpx").c_str(), &hs.px, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat((tag + " Pos Y##ckpy").c_str(), &hs.py, 0.002f, -0.30f, 0.30f, "%.4f");
        ch |= ImGui::DragFloat((tag + " Pos Z##ckpz").c_str(), &hs.pz, 0.002f, -0.30f, 0.30f, "%.4f");

        if (ImGui::Button("AIM-Werte aus HUEFTE kopieren##aimcopy")) {
            m_ham.hand_aim = m_ham.hand;
            ch = true;
        }

        if (ch) {
            rev_save_cfg();
        }

        ImGui::TreePop();
    }

    // [DAUMEN-KEYS] Der Daumen als KURVE ueber die Spann-Geste statt Anfang/Ende:
    // Phase anfahren -> die drei Glieder stellen -> "Key setzen". Der Hahn steht
    // in der Vorschau immer da, wo er in der echten Geste bei dieser Phase
    // stuende -> Kuppe sauber an den Sporn legen.
    if (ImGui::TreeNode("Daumen-Keyframes Spannen (nur Broken Butterfly)")) {
        // [ZWEI SAETZE] Die Checkbox unten waehlt den Satz.
        const bool aim_e = m_ham.key_aim;
        auto& KL = thumb_keys(4500, aim_e);
        ImGui::Checkbox("VORSCHAU: Phase per Regler (Daumen + Hahn stehen still)##tkprev",
                        &m_ham.kprev);

        if (!m_ham.kprev) {
            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                               "   >> Ohne diese Vorschau bewegt sich der Daumen NUR "
                               "waehrend der echten Spann-Geste.");
        }

        ImGui::SliderFloat("Phase % (0 = Griff, 100 = wieder am Griff)##tkph",
                           &m_ham.kphase, 0.0f, 100.0f, "%.0f");
        ImGui::Text("   Keys: %d   Phase live: %.0f %%   Hahn: %.0f %%   Kurve aktiv: %.0f %%",
                    static_cast<int>(KL.size()), m_ham.phase * 100.0f,
                    m_ham.ham_frac * 100.0f, m_ham.key_blend * 100.0f);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "   Keys gelten fuer beides -- gezielt wie aus der Huefte.");

        ImGui::Checkbox("AIM-Keys##tkaim", &m_ham.key_aim);
        ImGui::TextColored(aim_e ? ImVec4{1.0f, 0.65f, 0.0f, 1.0f}
                                 : ImVec4{0.4f, 1.0f, 0.4f, 1.0f},
                           aim_e ? ">>> bearbeitet: AIM-Keys <<<"
                                 : ">>> bearbeitet: HUEFT-Keys <<<");
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "   HUEFTE: %d Keys / AIM: %d Keys   zuletzt geladen: %s",
                           static_cast<int>(thumb_keys(4500, false).size()),
                           static_cast<int>(thumb_keys(4500, true).size()),
                           m_ham.kloaded > 0
                               ? ("Key " + std::to_string(m_ham.kloaded)).c_str() : "-");

        if (ImGui::Button("AIM-Keys aus HUEFTE kopieren##tkcopy")) {
            m_tkeys_aim[4500] = m_tkeys[4500];
            rev_save_cfg();
        }

        for (int i = 1; i <= 3; ++i) {
            char lbl[48]{};
            std::snprintf(lbl, sizeof(lbl), "R_Thumb%d%s##tkj%d", i,
                          (m_ham.kjoint == i) ? "  <<" : "", i);

            if (ImGui::Button(lbl)) {
                m_ham.kjoint = i;
            }

            if (i < 3) {
                ImGui::SameLine();
            }
        }

        auto& ke = m_ham.kedit[static_cast<size_t>(std::clamp(m_ham.kjoint, 1, 3) - 1)];
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "R_Thumb%d -- Beugen (Grad, additiv): Thumb1 beugt um X, "
                           "Thumb2/3 um Y", m_ham.kjoint);
        ImGui::DragFloat("Winkel X##tkx", &ke.x, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("Winkel Y##tky", &ke.y, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::DragFloat("Winkel Z##tkz", &ke.z, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Versatz (m, additiv NUR auf dieses Glied) -- damit laesst "
                           "sich strecken:");
        ImGui::DragFloat("Versatz X##tkpx", &ke.px, 0.001f, -0.10f, 0.10f, "%.4f");
        ImGui::DragFloat("Versatz Y##tkpy", &ke.py, 0.001f, -0.10f, 0.10f, "%.4f");
        ImGui::DragFloat("Versatz Z##tkpz", &ke.pz, 0.001f, -0.10f, 0.10f, "%.4f");

        if (ImGui::Button("Key hier setzen##tkset")) {
            const float p2 = std::clamp(m_ham.kphase, 0.0f, 100.0f);
            ThumbKey* hit = nullptr;

            for (auto& k : KL) {
                if (std::abs(k.p - p2) < 3.0f) {   // 3 % Toleranz
                    hit = &k;

                    break;
                }
            }

            if (hit == nullptr) {
                KL.push_back(ThumbKey{});
                hit = &KL.back();
            }

            hit->p = p2;
            hit->j = m_ham.kedit;
            std::sort(KL.begin(), KL.end(),
                      [](const ThumbKey& m, const ThumbKey& q) { return m.p < q.p; });
            rev_save_cfg();
        }

        ImGui::SameLine();

        if (ImGui::Button("Alle Keys loeschen##tkclr")) {
            KL.clear();
            rev_save_cfg();
        }

        ImGui::SameLine();

        if (ImGui::Button("Regler auf 0##tkzero")) {
            m_ham.kedit = std::array<ThumbJoint, 3>{};
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
            std::snprintf(b1, sizeof(b1), "x##tkdel%d", i);

            if (ImGui::Button(b1)) {
                KL.erase(KL.begin() + (i - 1));
                rev_save_cfg();

                break;
            }

            ImGui::SameLine();
            char b2[24]{};
            std::snprintf(b2, sizeof(b2), "laden##tkld%d", i);

            if (ImGui::Button(b2)) {
                m_ham.kphase = k.p;
                m_ham.kedit = k.j;
                // [SICHTBAR] Der Klick muss sich sofort am Daumen zeigen; der
                // Kopf-Zaehler macht ihn auch im UI sichtbar.
                m_ham.kloaded = i;
            }
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "   Vor dem ersten und nach dem letzten Key blendet der "
                           "Daumen selbst gegen den Griff.");
        ImGui::TreePop();
    }

    // [SHELL-IN-HAND] Pose + Offset + Daumen tunen (per Waffe)
    if (ImGui::TreeNode("Shell in Hand (Pose + Daumen + Offset)")) {
        auto& s = shell_cfg(awid.value_or(4500));
        bool sch = false;
        ImGui::Checkbox("Vorschau: Patrone in der Hand (zum Tunen)##shellprev",
                        &m_rev_st.preview);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Vorschau erzwingt die Patrone+Pose dauerhaft -> Offset "
                           "live justierbar)");

        char pose_buf[128]{};
        std::snprintf(pose_buf, sizeof(pose_buf), "%s", s.pose.c_str());

        if (ImGui::InputText("Pose-Name (aus reload.json POSES)##shellpose",
                             pose_buf, sizeof(pose_buf))) {
            s.pose = pose_buf;
            sch = true;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "Pose wird bei OFFENER Trommel auf die linke Hand gelegt.");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen-Spreizung (additiv auf die Pose, Grad):");
        sch |= ImGui::SliderFloat("Daumen RotX##shtrx", &s.t_rx, -90.0f, 90.0f);
        sch |= ImGui::SliderFloat("Daumen RotY##shtry", &s.t_ry, -90.0f, 90.0f);
        sch |= ImGui::SliderFloat("Daumen RotZ##shtrz", &s.t_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Zeigefinger-Greifpose (additiv auf L_IndexF1/2/3, Grad):");
        sch |= ImGui::SliderFloat("Zeigefinger RotX##shirx", &s.i_rx, -180.0f, 180.0f);
        sch |= ImGui::SliderFloat("Zeigefinger RotY##shiry", &s.i_ry, -180.0f, 180.0f);
        sch |= ImGui::SliderFloat("Zeigefinger RotZ##shirz", &s.i_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Clone-Offset zur Hand (m) -- bewegt den Patronen-Clone:");
        sch |= ImGui::DragFloat("PosX##shx", &s.x, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("PosY##shy", &s.y, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("PosZ##shz", &s.z, 0.001f, -0.50f, 0.50f, "%.4f");
        sch |= ImGui::DragFloat("RotX##shrx", &s.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        sch |= ImGui::DragFloat("RotY##shry", &s.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        sch |= ImGui::DragFloat("RotZ##shrz", &s.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Patronen-Mesh-Clone (Trommel bleibt unberuehrt -- kein Loch):");

        char parts_buf[64]{};
        std::snprintf(parts_buf, sizeof(parts_buf), "%s", s.parts.c_str());

        if (ImGui::InputText("Mesh-Part-Indizes (z.B. 20,30)##shparts",
                             parts_buf, sizeof(parts_buf))) {
            s.parts = parts_buf;
            sch = true;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (20=Spitze, 30=Patrone; per Parts-Finder ermittelt)");
        sch |= ImGui::SliderFloat("Scale##shscale", &s.scale, 0.1f, 3.0f);

        if (sch) {
            rev_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Patrone in der Hand = Mesh-Clone (Parts 20+30), Trommel "
                       "unberuehrt. Auswurf separat.");
    ImGui::TreePop();
}

// ============================================================================
// 2 -- RIFLE (Lua Z.1598-2796)
//
// wp4401 "Stingray", wp4402 "CQBR". Logik = LE5-SMG (Mag-Drop _04 + Slide-Rack
// _02 + Verstellschalter _05). In Lua ein eigener do...end-Block mit eigenem
// Local-Budget; hier eigene Member/Methoden. Reload (Teil 1) ignoriert Rifles
// (kein JOINTS-Eintrag) -> _managed=false -> schreibt NICHTS an der Stingray.
// Wir publishen dieselben Globals wie reload fuer die gemanagte SMG
// (motion/arm_chain/binding/holster lesen sie waffen-agnostisch); reload2 laeuft
// NACH reload -> unsere Passes gewinnen.
// Eigenes JSON: reframework/data/re4_vr/re4_vr_reload2_rifle.json
// ============================================================================
namespace {

constexpr const char* RIF_CFG_PATH = "re4_vr/re4_vr_reload2_rifle.json";

// Luas `wi:write_dword(0x44, n)`: das Feld ueber die TypeDB suchen und den
// Offset nur als Rueckfall benutzen (er stimmt bei allen bekannten Builds).
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

// ---- Per-Waffe Joints (CODE-Konstanten, NIE aus JSON) ----
struct RifJoints {
    const char* mag;
    const char* slide;
    const char* sw;
};

std::optional<RifJoints> rif_joints(int32_t wid) {
    if (wid == 4401) {
        return RifJoints{"_04", "_02", "_05"};   // Stingray (Verstellschalter _05)
    }

    if (wid == 4402) {
        return RifJoints{"_04", "_02", "_06"};   // CQBR (Klappschalter _06)
    }

    return std::nullopt;
}

const char* rif_mag_pose(int32_t wid) {
    (void)wid;

    return "StingrayMag";   // CQBR seedet die Stingray-Pose
}

const char* rif_rack_pose(int32_t wid) {
    return (wid == 4402) ? "CqbrSlide" : "StingraySlide";
}

const char* rif_switch_pose(int32_t wid) {
    (void)wid;

    return "StingraySwitch";
}

// Engine schliesst den Slide selbst nach dem Reload? (Stingray = true) -> wir
// forcen rest_z NICHT. CQBR NICHT: voller manueller Rack.
bool rif_engine_closes(int32_t wid) {
    return wid == 4401;
}

// Sperrt Schalter-Stufe 1 das Feuern? Stingray ja (bewusster Dry-Fire-Stand);
// CQBR NEIN -- dort feuert Stufe 1 (seit 08.09. ein Schuss pro Zug).
bool rif_switch_blocks_fire(int32_t wid) {
    return wid == 4401;
}

// Schalter-Stufe 1 = Burst? Wert = Schuss pro Trigger-Zug.
// [2026-09-08] CQBR auf EINEN Schuss pro Zug (vorher 2). Der Weg bleibt
// derselbe: begrenzt wird am nativen isEnableFire, RT liefert nur die Flanke.
std::optional<int32_t> rif_switch_burst(int32_t wid) {
    if (wid == 4402) {
        return 1;
    }

    return std::nullopt;
}

// ---- Sounds (per-wid) ----
struct RifSnd {
    uint32_t dry_fire, mag_eject, mag_insert, mag_floor;
    uint32_t slide_back, slide_forward, mag_holster, sw;
};

std::optional<RifSnd> rif_snd_set(int32_t wid) {
    if (wid == 4401) {
        return RifSnd{812850326u, 1466005368u, 943565871u, 3042341191u,
                      2254736731u, 2254736731u, 1839787494u, 3805002294u};
    }

    if (wid == 4402) {
        return RifSnd{812850326u, 1466005368u, 943565871u, 3042341191u,
                      611689939u, 611689939u, 1839787494u, 3805002294u};
    }

    return std::nullopt;
}

constexpr float RIF_GRAVITY = 9.8f;
constexpr float RIF_DROP_FALL_DUR = 1.0f;
// [FLOOR-SND] gerader Hand-Fall vs. Modul-Slide (Schaft-Slide + kontrollierter Fall)
constexpr double RIF_FLOOR_DELAY = 0.45;
constexpr double RIF_FLOOR_DELAY_MODULE = 0.78;
constexpr float RACK_GRAB_DIST = 0.14f;
constexpr float DOCK_BLEND_SPEED = 0.10f;

// Die Welt-Position der linken Hand -- Prioritaet wie in Lua.
std::optional<glm::vec3> left_hand_world_g() {
    return re4vr::lua_get_vec3_any({"__vr_lh_world", "__vr_unified_lh_pos",
                                    "__vr_lh_joint_pos"});
}

std::optional<glm::vec3> right_hand_raw_g() {
    return re4vr::lua_get_vec3_any({"__vr_rh_ctrl_raw", "__vr_rh_world",
                                    "__vr_unified_rh_pos"});
}

}   // namespace

bool RE4VRReload2::is_rifle(int32_t wid) {
    return wid == 4401 || wid == 4402;
}

RE4VRReload2::RSlide& RE4VRReload2::rslide(int32_t wid) {
    auto it = m_rslide.find(wid);

    if (it == m_rslide.end()) {
        RSlide s{};

        if (wid == 4402) {
            s.empty_x = 0.020f;   // CQBR faehrt bei Ammo=0 sichtbar in X raus
        }

        it = m_rslide.emplace(wid, s).first;
    }

    return it->second;
}

RE4VRReload2::RDock& RE4VRReload2::rdock(int32_t wid) {
    auto it = m_rdock.find(wid);

    if (it == m_rdock.end()) {
        it = m_rdock.emplace(wid, RDock{}).first;
    }

    return it->second;
}

RE4VRReload2::RMagHand& RE4VRReload2::rmaghand(int32_t wid) {
    auto it = m_rmaghand.find(wid);

    if (it == m_rmaghand.end()) {
        it = m_rmaghand.emplace(wid, RMagHand{}).first;
    }

    return it->second;
}

RE4VRReload2::RSwitch& RE4VRReload2::rswitch(int32_t wid) {
    auto it = m_rswitch.find(wid);

    if (it == m_rswitch.end()) {
        it = m_rswitch.emplace(wid, RSwitch{}).first;
    }

    return it->second;
}

// Slide-Idle-/Pull-Basis: empty_x-Waffen (CQBR) bleiben auf empty in Z VORNE
// (rest_z) und fahren nur in X aus -> der Z-Pull ist der manuelle Rack.
// Stingray/SMG: park_z (Slide rutscht mittig).
float RE4VRReload2::park_ref(const RSlide& sp) {
    return (sp.empty_x != 0.0f) ? sp.rest_z : sp.park_z;
}

bool RE4VRReload2::rifle_flow() const {
    return m_rifdrop.active || m_rif_mag_hand || m_rifins.active || m_rif_mag_tune;
}

void RE4VRReload2::rifle_load_cfg() {
    const auto data = re4vr::json_load(RIF_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    if (const auto it = data.find("cfg"); it != data.end() && it->is_object()) {
        m_rifcfg.rifle_enabled = jbool(*it, "rifle_enabled", m_rifcfg.rifle_enabled);
        m_rifcfg.reload_ammo = jbool(*it, "reload_ammo", m_rifcfg.reload_ammo);
        m_rifcfg.sound_enabled = jbool(*it, "sound_enabled", m_rifcfg.sound_enabled);
        m_rifcfg.insert_distance = jnum(*it, "insert_distance", m_rifcfg.insert_distance);
    }

    if (const auto it = data.find("slide"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& sp = rslide(wid);
            sp.rest_z = jnum(v, "rest_z", sp.rest_z);
            sp.park_z = jnum(v, "park_z", sp.park_z);
            sp.back_z = jnum(v, "back_z", sp.back_z);
            sp.empty_x = jnum(v, "empty_x", sp.empty_x);
            sp.dock_x = jnum(v, "dock_x", sp.dock_x);
            sp.dock_y = jnum(v, "dock_y", sp.dock_y);
            sp.dock_z = jnum(v, "dock_z", sp.dock_z);
            sp.rack_rx = jnum(v, "rack_rx", sp.rack_rx);
            sp.rack_ry = jnum(v, "rack_ry", sp.rack_ry);
            sp.rack_rz = jnum(v, "rack_rz", sp.rack_rz);
            sp.st_rx = jnum(v, "st_rx", sp.st_rx);
            sp.st_ry = jnum(v, "st_ry", sp.st_ry);
            sp.st_rz = jnum(v, "st_rz", sp.st_rz);
        }
    }

    if (const auto it = data.find("dock"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& d = rdock(wid);
            d.joint = jstr(v, "joint", d.joint);

            // [DISTANZ PRO WAFFE] Einlege-Distanz gehoert zur Waffe, nicht in
            // die Skalar-Config: Stingray und CQBR sind verschieden lang.
            if (const auto f = v.find("insert"); f != v.end() && f->is_number()) {
                d.insert = f->get<float>();
                d.ins_loaded = true;
            }

            d.x = jnum(v, "x", d.x);
            d.y = jnum(v, "y", d.y);
            d.z = jnum(v, "z", d.z);
        }
    }

    // [DISTANZ PRO WAFFE] Waffen ohne eigenen gespeicherten Wert erben den alten
    // gemeinsamen insert_distance.
    for (const int32_t wid : {4401, 4402}) {
        auto& d = rdock(wid);

        if (!d.ins_loaded) {
            d.insert = m_rifcfg.insert_distance;
        }
    }

    if (const auto it = data.find("maghand"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& m = rmaghand(wid);
            m.x = jnum(v, "x", m.x);
            m.y = jnum(v, "y", m.y);
            m.z = jnum(v, "z", m.z);
            m.rx = jnum(v, "rx", m.rx);
            m.ry = jnum(v, "ry", m.ry);
            m.rz = jnum(v, "rz", m.rz);
            m.t_rx = jnum(v, "t_rx", m.t_rx);
            m.t_ry = jnum(v, "t_ry", m.t_ry);
            m.t_rz = jnum(v, "t_rz", m.t_rz);
        }
    }

    if (const auto it = data.find("switch"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& s = rswitch(wid);
            s.rx = jnum(v, "rx", s.rx);
            s.ry = jnum(v, "ry", s.ry);
            s.rz = jnum(v, "rz", s.rz);
            s.lerp = jnum(v, "lerp", s.lerp);
            s.grab_dist = jnum(v, "grab_dist", s.grab_dist);
            s.dx = jnum(v, "dx", s.dx);
            s.dy = jnum(v, "dy", s.dy);
            s.dz = jnum(v, "dz", s.dz);
            s.hrx = jnum(v, "hrx", s.hrx);
            s.hry = jnum(v, "hry", s.hry);
            s.hrz = jnum(v, "hrz", s.hrz);
        }
    }
}

void RE4VRReload2::rifle_save_cfg() {
    nlohmann::json slideo = nlohmann::json::object();
    nlohmann::json docko = nlohmann::json::object();
    nlohmann::json mho = nlohmann::json::object();
    nlohmann::json swo = nlohmann::json::object();

    for (const int32_t wid : {4401, 4402}) {
        const auto key = std::to_string(wid);
        const auto& sp = rslide(wid);
        slideo[key] = {
            {"rest_z", sp.rest_z}, {"park_z", sp.park_z}, {"back_z", sp.back_z},
            {"empty_x", sp.empty_x}, {"dock_x", sp.dock_x}, {"dock_y", sp.dock_y},
            {"dock_z", sp.dock_z}, {"rack_rx", sp.rack_rx}, {"rack_ry", sp.rack_ry},
            {"rack_rz", sp.rack_rz}, {"st_rx", sp.st_rx}, {"st_ry", sp.st_ry},
            {"st_rz", sp.st_rz},
        };

        const auto& d = rdock(wid);
        docko[key] = {{"joint", d.joint}, {"x", d.x}, {"y", d.y}, {"z", d.z},
                      {"insert", d.insert}};

        const auto& m = rmaghand(wid);
        mho[key] = {{"x", m.x}, {"y", m.y}, {"z", m.z},
                    {"rx", m.rx}, {"ry", m.ry}, {"rz", m.rz},
                    {"t_rx", m.t_rx}, {"t_ry", m.t_ry}, {"t_rz", m.t_rz}};

        const auto& s = rswitch(wid);
        swo[key] = {{"rx", s.rx}, {"ry", s.ry}, {"rz", s.rz}, {"lerp", s.lerp},
                    {"grab_dist", s.grab_dist}, {"dx", s.dx}, {"dy", s.dy},
                    {"dz", s.dz}, {"hrx", s.hrx}, {"hry", s.hry}, {"hrz", s.hrz}};
    }

    nlohmann::json d = {
        {"cfg", {{"rifle_enabled", m_rifcfg.rifle_enabled},
                 {"reload_ammo", m_rifcfg.reload_ammo},
                 {"sound_enabled", m_rifcfg.sound_enabled},
                 {"insert_distance", m_rifcfg.insert_distance}}},
        {"slide", slideo},
        {"dock", docko},
        {"maghand", mho},
        {"switch", swo},
    };

    re4vr::json_save(RIF_CFG_PATH, d);
}

// ---- Sound am Waffen-SoundContainer (eigene tf, nicht die Revolver-tf) ----
void RE4VRReload2::rf_snd(uint32_t id) {
    if (!m_rifcfg.sound_enabled || id == 0 || m_rifwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_rifwep.tf, "get_GameObject"), id);
}

void RE4VRReload2::rifle_refresh() {
    const auto ewid = get_equip_wid();
    const bool managed = m_rifcfg.rifle_enabled && ewid.has_value() && is_rifle(*ewid);

    if (!managed) {
        m_rifwep = RifleWep{};

        return;
    }

    if (m_rifwep.wid.has_value() && *m_rifwep.wid == *ewid
        && m_rifwep.mag_joint != nullptr && m_rifwep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_rifwep.tf, "get_Position", p)) {
            return;
        }
    }

    // [SAVE_LOAD] gleiche WeaponId, aber tf ungueltig -> neue Instanz
    // (Save-Load/Respawn). Der Waffenwechsel-Reset greift hier NICHT (wid
    // unveraendert) -> separat signalisieren.
    const bool same_wid = m_rifwep.wid.has_value() && *m_rifwep.wid == *ewid;
    m_rifwep = RifleWep{};

    const auto jc = rif_joints(*ewid);

    if (!jc.has_value()) {
        return;
    }

    auto* tf = find_weapon(*ewid);

    if (tf == nullptr) {
        return;
    }

    auto* mj = joint_by_name(tf, jc->mag);

    if (mj == nullptr) {
        return;
    }

    m_rifwep.wid = *ewid;
    m_rifwep.tf = tf;
    m_rifwep.mag_joint = mj;
    m_rifwep.slide_joint = joint_by_name(tf, jc->slide);

    if (m_rifwep.slide_joint != nullptr) {
        glm::vec3 lp{};

        // Basis-X (Slide zu); fuer das empty_x-Ausfahren
        if (get_vec3(m_rifwep.slide_joint, "get_LocalPosition", lp)) {
            m_rifwep.slide_rest_lp = lp;
        }
    }

    m_rifwep.switch_joint = joint_by_name(tf, jc->sw);

    if (m_rifwep.switch_joint != nullptr) {
        glm::quat r{};

        if (get_quat(m_rifwep.switch_joint, "get_LocalRotation", r)) {
            m_rifwep.switch_rest_rot = r;
        }
    }

    if (same_wid) {
        m_rif_reacquired = true;
    }
}

// ---- chainsaw.Gun (Leer-Erkennung + Slide-Entriegeln nach Rack) ----
::REManagedObject* RE4VRReload2::rf_get_gun() {
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

// aus AmmoEmpty holen -> Engine entriegelt den Slide
void RE4VRReload2::rf_gun_chamber() {
    auto* g = rf_get_gun();

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

bool RE4VRReload2::rf_gun_ammo_empty() {
    bool v = false;

    return re4vr::try_call<bool>(get_pe(), "isGunAmmoEmpty", v) && v;
}

// ---- Live-WeaponItem. EIGENES rf_get_wi (NICHT get_live_wi!): das validiert
// gegen die Revolver-wid und gaebe bei einer Rifle ein evtl. stale Revolver-Item
// zurueck. Hier gegen die EQUIPPTE wid validieren.
::REManagedObject* RE4VRReload2::rf_get_wi() {
    const auto ewid = get_equip_wid();

    // [ACCESSOR] ZUERST die einzige PERSISTENTE Instanz; alles darunter sind
    // KOPIEN (Schreiben wirkt dort nur im selben Tick).
    if (m_main != nullptr) {
        if (auto* real = m_main->real_wi(ewid); real != nullptr) {
            return real;
        }
    }

    auto* pe = get_pe();
    auto* ewi = re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

std::optional<int32_t> RE4VRReload2::rf_loaded() {
    auto* wi = rf_get_wi();

    if (wi == nullptr) {
        return std::nullopt;
    }

    return call_enum(wi, "get_CurrentAmmoCount");
}

int32_t RE4VRReload2::rf_cap() {
    auto* wi = rf_get_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload2::rf_reserve() {
    auto* wi = rf_get_wi();

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
// Mag-Drop / Mag-in-Hand / Insert (RE9-/LE5-Muster)
// =====================================================================
void RE4VRReload2::rf_capture_mag_rest() {
    if (m_rifwep.mag_joint == nullptr || rifle_flow() || m_rif_mag_out) {
        return;
    }

    glm::vec3 lp{};
    glm::quat lr{};

    if (get_vec3(m_rifwep.mag_joint, "get_LocalPosition", lp)) {
        m_rifwep.rest_lp = lp;
    }

    if (get_quat(m_rifwep.mag_joint, "get_LocalRotation", lr)) {
        m_rifwep.rest_lr = lr;
    }
}

void RE4VRReload2::rf_stop_drop() {
    if (m_rifdrop.use_module && m_adv != nullptr) {
        m_adv->cancel();
    }

    m_rifdrop.active = false;
    m_rifdrop.joint = nullptr;
    m_rifdrop.use_module = false;
}

// [ADV-SLIDE] Mag gleitet erst entlang des Schachts aus der Waffe, dann Fall zum
// Boden. use_module=true nutzt das gemeinsame Mag-Slide-Modul (reload_adv);
// use_module=false = simpler Geradeaus-Fall (Hand-Loslassen).
bool RE4VRReload2::rf_start_drop_from(const std::optional<glm::vec3>& p, bool use_module) {
    if (use_module && m_rifwep.mag_joint != nullptr && m_adv != nullptr) {
        if (m_adv->begin_drop(m_rifwep.mag_joint, m_rifwep.wid.value_or(0),
                              m_rifins.dur, std::nullopt)) {
            m_rifdrop.active = true;
            m_rifdrop.use_module = true;
            m_rifdrop.joint = m_rifwep.mag_joint;

            return true;
        }
    }

    if (!p.has_value()) {
        return false;
    }

    m_rifdrop.joint = m_rifwep.mag_joint;
    m_rifdrop.use_module = false;
    m_rifdrop.sx = p->x;
    m_rifdrop.sy = p->y;
    m_rifdrop.sz = p->z;
    m_rifdrop.t0 = clock_now();
    m_rifdrop.active = true;

    return true;
}

bool RE4VRReload2::rf_force_eject() {
    // [UNLIMITED] B stillgelegt
    if (RE4VRWeapons2::get()->is_unlimited()) {
        return false;
    }

    rifle_refresh();

    if (m_rifwep.mag_joint == nullptr) {
        return false;
    }

    if (m_rif_mag_out) {
        return false;   // schon draussen (Anti-Doppeldrop)
    }

    // [KEIN DROP OHNE RESERVE] Ohne Nachschub wird das Magazin gar nicht erst
    // ausgeworfen -- gilt fuer JEDE Waffe.
    if (rf_reserve() <= 0) {
        return false;
    }

    // [LIVE-EMPTY] Frischer Engine-Read (getCurrentGunAmmo, kein Cache) statt
    // rf_loaded (nach Save-Load stale 0 -> empty_when_dropped faelschlich true ->
    // rack.needs latcht -> Dauer-Dry-Fire).
    if (const auto fga = call_enum(get_pe(), "getCurrentGunAmmo"); fga.has_value()) {
        m_rifrack.empty_when_dropped = (*fga <= 0);
    } else {
        m_rifrack.empty_when_dropped = (rf_loaded().value_or(0) <= 0);
    }

    // [KAMMER] Der Read oben kann bereits UNSERE 0 sehen (mehrere Stellen nullen
    // die Waffe beim Auswurf). War beim Nullen etwas im Magazin, war die Kammer
    // NICHT leer -> kein Rack verlangen. Quelle ist der gemerkte Magazinrest.
    // [BESITZER PRUEFEN] Der Rest zaehlt nur, wenn er zu DIESER Waffe gehoert;
    // fehlt der Vermerk (nil), gilt er wie bisher.
    if (re4vr::lua_get_number("__re4_mag_carry", 0.0) > 0.0) {
        const auto cw = re4vr::lua_get_number_opt("__re4_mag_carry_wid");
        const auto ew = get_equip_wid();

        if (!cw.has_value()
            || (m_rifwep.wid.has_value()
                && static_cast<int32_t>(*cw) == *m_rifwep.wid)
            || (ew.has_value() && static_cast<int32_t>(*cw) == *ew)) {
            m_rifrack.empty_when_dropped = false;
        }
    }

    m_rifrack._chambered_hold = false;
    m_rif_mag_hand = false;
    m_rifins.active = false;

    if (m_rifcfg.reload_ammo) {
        // [1:1] In Lua steht hier `mag_retained = loaded` -- `loaded` ist an
        // dieser Stelle KEIN Local dieses Blocks, sondern ein GLOBAL und damit
        // immer nil. Der Magazinrest ist beim Rifle-Auswurf also faktisch 0, und
        // `__re4_mag_carry` wird geloescht. Genau so uebernommen: alles andere
        // waere eine Verhaltensaenderung.
        m_rif_mag_retained = 0;
        re4vr::lua_set_nil("__re4_mag_carry");
        re4vr::lua_set_number("__re4_mag_carry_wid",
                              static_cast<double>(m_rifwep.wid.value_or(0)));

        if (auto* wi = rf_get_wi(); wi != nullptr) {
            if (m_main != nullptr) {
                m_main->carry_capture(wi, "re4_vr_reload2.lua:1987", std::nullopt);
            }

            field_i32_write(wi, "_CurrentAmmoCount", 0x44, 0);   // UI auf 0
        }

        // [ACCESSOR-FOLGE] Diese 0 ist UNSER Werk und wirkt seit dem
        // Accessor-Umbau wirklich. `rack.empty` (= isGunAmmoEmpty) darf sie beim
        // Einsetzen nicht als leere Kammer werten -- sonst rackt jede
        // Magazinwaffe nach JEDEM Reload.
        m_rifrack._zeroed_by_us = true;
    }

    // Mag-Joint in Ruhe, dann frisch droppen
    if (m_rifwep.rest_lp.has_value()) {
        set_vec3(m_rifwep.mag_joint, "set_LocalPosition", *m_rifwep.rest_lp);
    }

    if (m_rifwep.rest_lr.has_value()) {
        set_quat(m_rifwep.mag_joint, "set_LocalRotation", *m_rifwep.rest_lr);
    }

    glm::vec3 p{};
    const bool have_p = get_vec3(m_rifwep.mag_joint, "get_Position", p);
    // [ADV-SLIDE] B-Eject: erst aus dem Schacht sliden, dann fallen
    const bool started = rf_start_drop_from(
        have_p ? std::optional<glm::vec3>{p} : std::nullopt, true);

    if (started) {
        m_rif_mag_out = true;

        if (const auto s = rif_snd_set(m_rifwep.wid.value_or(0)); s.has_value()) {
            rf_snd(s->mag_eject);
        }

        // [FLOOR-SND]
        m_rif_mag_floor_at = clock_now()
            + (m_rifdrop.use_module ? RIF_FLOOR_DELAY_MODULE : RIF_FLOOR_DELAY);
    }

    return started;
}

void RE4VRReload2::rf_update_drop() {
    if (!m_rifdrop.active) {
        return;
    }

    if (m_rifdrop.use_module) {
        // [ADV-SLIDE] Slide+Fall treibt das Modul; es haelt am Boden, bis
        // Grab/Reset cancelt.
        if (m_adv != nullptr) {
            m_adv->tick();
        } else {
            rf_stop_drop();
        }

        return;
    }

    if (m_rifdrop.joint == nullptr) {
        return;
    }

    const float t = static_cast<float>(clock_now() - m_rifdrop.t0);

    if (t > RIF_DROP_FALL_DUR) {   // Auto-Clear (kein Soft-Lock)
        m_rifdrop.active = false;
        m_rifdrop.joint = nullptr;

        return;
    }

    const float fall = 0.5f * RIF_GRAVITY * t * t;
    set_vec3(m_rifdrop.joint, "set_Position",
             glm::vec3{m_rifdrop.sx, m_rifdrop.sy - fall, m_rifdrop.sz});
}

// Holster-Grab: Mag in die linke Hand (nur wenn Mag draussen + was zu laden da)
bool RE4VRReload2::rf_can_grab() {
    if (m_rif_mag_hand || m_rifins.active) {
        return false;
    }

    if (!m_rif_mag_out) {
        return false;
    }

    return (m_rif_mag_retained + rf_reserve()) > 0;
}

bool RE4VRReload2::rifle_set_mag_in_hand(bool active) {
    if (active) {
        if (!rf_can_grab()) {
            return false;
        }

        // [ADV-SLIDE] laufenden Modul-Slide sauber abbrechen, sonst kaempft er
        // gegen die Hand-Pose
        rf_stop_drop();
        m_rif_mag_hand = true;

        if (const auto s = rif_snd_set(m_rifwep.wid.value_or(0)); s.has_value()) {
            rf_snd(s->mag_holster);
        }

        return true;
    }

    // losgelassen ohne Einlegen -> Mag faellt von der Hand-Position auf den Boden
    if (m_rif_mag_hand) {
        m_rif_mag_hand = false;
        glm::vec3 p{};
        const bool have = m_rifwep.mag_joint != nullptr
            && get_vec3(m_rifwep.mag_joint, "get_Position", p);

        if (rf_start_drop_from(have ? std::optional<glm::vec3>{p} : std::nullopt, false)) {
            m_rif_mag_floor_at = clock_now() + RIF_FLOOR_DELAY;   // gerader Fall
        }
    }

    return true;
}

// Mag folgt der linken Hand (die Hand-Pose setzt der Render-Pass)
void RE4VRReload2::rf_update_mag_in_hand() {
    auto* joint = ((m_rif_mag_hand || m_rif_mag_tune) ? m_rifwep.mag_joint : nullptr);

    if (joint == nullptr) {
        return;
    }

    const auto& m = rmaghand(m_rifwep.wid.value_or(0));
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

// Dock-Port-Welt (fester Gun-Joint + Offset) fuer die Einlege-Naehe
std::optional<glm::vec3> RE4VRReload2::rf_dock_port_world() {
    const auto& d = rdock(m_rifwep.wid.value_or(0));

    if (m_rifwep.tf == nullptr) {
        return std::nullopt;
    }

    auto* j = joint_by_name(m_rifwep.tf, d.joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{d.x, d.y, d.z});
}

// [KEYFRAME-ANKER] Sobald eine Waffe eine Shell-/Mag-Bahn hat, IST Keyframe #1
// der Ladepunkt -- also messen wir den Handabstand gegen ihn, waffenrelativ
// zurueckgerechnet. UNTERSCHIED zu reload/reload4_dlc: dort steht der Anker
// HINTER dem Dock-Port; hier hat er VORRANG vor RDOCK (der Stingray-Port stammt
// aus der Zeit vor den Keyframes). Rueckbau: __re4_insert_kf_anchor = false
std::optional<glm::vec3> RE4VRReload2::rf_kf_anchor_world() {
    if (m_adv == nullptr || m_rifwep.tf == nullptr || !m_rifwep.wid.has_value()) {
        return std::nullopt;
    }

    if (re4vr::lua_get_tribool("__re4_insert_kf_anchor") == 0) {
        return std::nullopt;
    }

    const int32_t wid = *m_rifwep.wid;

    if (!m_adv->has_shell_keys(wid) || m_adv->uses_rev_insert(wid)) {
        return std::nullopt;
    }

    RE4VRReloadAdv::Key k{};

    if (!m_adv->shell_pose_at(wid, 0.0f, k)) {
        return std::nullopt;
    }

    glm::vec3 wp0{};
    glm::quat wr0{};

    if (!get_vec3(m_rifwep.tf, "get_Position", wp0)
        || !get_quat(m_rifwep.tf, "get_Rotation", wr0)) {
        return std::nullopt;
    }

    return wp0 + (wr0 * glm::vec3{k.x, k.y, k.z});
}

// Insert: Mag von der Hand-Lokalpose in die Chamber-Ruhepose lerpen
bool RE4VRReload2::rf_start_insert() {
    if (!(m_rifwep.mag_joint != nullptr && m_rifwep.rest_lp.has_value())) {
        return false;
    }

    glm::vec3 lp{};

    if (!get_vec3(m_rifwep.mag_joint, "get_LocalPosition", lp)) {
        return false;
    }

    m_rifins.slp = lp;
    glm::quat lr{};
    m_rifins.slr = get_quat(m_rifwep.mag_joint, "get_LocalRotation", lr)
        ? std::optional<glm::quat>{lr} : std::nullopt;

    // [SHELL-KEYFRAMES] Hat die Waffe eine Bahn, faehrt update_insert die
    // geordneten Keyframes ab statt linear von der Handlage in die Ruhepose zu
    // lerpen. Die Bahn hat ihre EIGENE Dauer aus reload_adv; ohne Bahn bleibt es
    // bei der Rifle-Dauer -- darum die Ausgangsdauer einmal merken.
    if (!m_rifins.dur_base.has_value()) {
        m_rifins.dur_base = m_rifins.dur;
    }

    const int32_t wid = m_rifwep.wid.value_or(0);
    m_rifins.keyframe = (m_adv != nullptr) && m_adv->has_shell_keys(wid);
    m_rifins.dur = *m_rifins.dur_base;

    if (m_rifins.keyframe && m_adv != nullptr) {
        const auto d = m_adv->kf_insert_dur(wid);
        m_rifins.dur = d.value_or(m_adv->shell_dur);
    }

    m_rifins.t0 = clock_now();
    m_rifins.active = true;
    m_rifins.snd = false;

    return true;
}

void RE4VRReload2::rf_check_insert_proximity() {
    if (!m_rif_mag_hand) {
        return;
    }

    const auto hp = left_hand_world_g();

    if (!hp.has_value()) {
        return;
    }

    auto gp = rf_kf_anchor_world();

    if (!gp.has_value()) {
        gp = rf_dock_port_world();
    }

    if (!gp.has_value() && m_rifwep.tf != nullptr) {
        glm::vec3 wp{};

        if (get_vec3(m_rifwep.tf, "get_Position", wp)) {
            gp = wp;
        }
    }

    if (!gp.has_value()) {
        return;
    }

    const float lim = m_rifwep.wid.has_value() ? rdock(*m_rifwep.wid).insert
                                               : m_rifcfg.insert_distance;

    if (vec_len(vec_sub(*hp, *gp)) <= lim) {
        m_rif_mag_hand = false;
        rf_start_insert();
    }
}

void RE4VRReload2::rf_reload_ammo_on_insert() {
    if (!m_rifcfg.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* wi = rf_get_wi();

    if (wi == nullptr) {
        return;
    }

    const int32_t cap = rf_cap();
    const int32_t reserve = rf_reserve();
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");
    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");
    const int32_t target = std::min(cap, m_rif_mag_retained + reserve);
    const int32_t used = std::max(0, target - m_rif_mag_retained);

    // [RUNTIME-FIX 0/0 + MAG-RETAIN] zwei Quellen: (1) Reserve-Anteil `used` ueber
    // den Engine-Reload (greift aufs echte Gun-Item, zieht Reserve),
    // (2) Retained-Anteil (gedropptes Mag, schon bezahlt) frei auf target
    // auffuellen OHNE Reserve-Abzug.
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
        // [CRASH-HARDEN] Engine-Gate enableReloadItem gegen den null-Item-AV
        m_main->load_and_book(inv, *et, used, false);
    }

    const int32_t af = m_main->gun_ammo().value_or(b4);
    const int32_t got = std::max(0, af - b4);

    if (got > 0 && inv != nullptr && ammo_id.has_value() && read_rsv() >= r_b4) {
        m_main->safe_reduce(inv, *ammo_id, got);
    }

    // (2) Retained-Anteil auffuellen (frei, kein Reserve-Abzug)
    if (m_main->gun_ammo().value_or(af) < target) {
        field_i32_write(wi, "_CurrentAmmoCount", 0x44, target);
        const int32_t now = m_main->gun_ammo().value_or(b4 + got);

        if (now < target) {
            re4vr::call_safe<void*>(wi, "addAmmoCount", target - now, false);
        }
    }

    m_rif_mag_retained = 0;
    // verbraucht: Spiegel mit loeschen
    re4vr::lua_set_nil("__re4_mag_carry");
    re4vr::lua_set_nil("__re4_mag_carry_wid");
}

void RE4VRReload2::rf_update_insert() {
    if (!(m_rifins.active && m_rifwep.mag_joint != nullptr
          && m_rifins.slp.has_value() && m_rifwep.rest_lp.has_value())) {
        return;
    }

    float t = static_cast<float>(clock_now() - m_rifins.t0)
              / std::max(m_rifins.dur, 0.01f);

    if (t > 1.0f) {
        t = 1.0f;
    }

    if (!m_rifins.snd && t >= 0.75f) {
        m_rifins.snd = true;

        if (const auto s = rif_snd_set(m_rifwep.wid.value_or(0)); s.has_value()) {
            rf_snd(s->mag_insert);
        }
    }

    // [SHELL-KEYFRAMES] Waffen mit Bahn: Mag-Joint entlang der geordneten
    // Keyframes (Position + Rotation, relativ zur WAFFE) -- Start = Keyframe #1
    // (der Andockpunkt, gegen den check_insert_proximity misst), Ende = letzter
    // Keyframe (Kammer). Die Handlage geht NICHT mehr ein. Schlaegt es fehl,
    // faellt es unveraendert auf den linearen Weg darunter zurueck.
    bool kf_done = false;

    if (m_rifins.keyframe && m_adv != nullptr && m_rifwep.tf != nullptr) {
        kf_done = m_adv->apply_shell_keys(m_rifwep.tf, m_rifwep.mag_joint,
                                          m_rifwep.wid.value_or(0), t);
    }

    if (!kf_done) {
        const float u = ease(t);
        const glm::vec3 a = *m_rifins.slp;
        const glm::vec3 b = *m_rifwep.rest_lp;
        set_vec3(m_rifwep.mag_joint, "set_LocalPosition", a + (b - a) * u);

        if (m_rifins.slr.has_value() && m_rifwep.rest_lr.has_value()) {
            const glm::quat s = *m_rifins.slr;
            glm::quat r = *m_rifwep.rest_lr;

            if (glm::dot(s, r) < 0.0f) {
                r = glm::quat{-r.w, -r.x, -r.y, -r.z};
            }

            glm::quat q{s.w + (r.w - s.w) * u, s.x + (r.x - s.x) * u,
                        s.y + (r.y - s.y) * u, s.z + (r.z - s.z) * u};
            const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);

            if (len > 1e-6f) {
                set_quat(m_rifwep.mag_joint, "set_LocalRotation",
                         glm::quat{q.w / len, q.x / len, q.y / len, q.z / len});
            }
        }
    }

    if (t < 1.0f) {
        return;
    }

    m_rifins.active = false;
    m_rifrack._ammo_input_t = clock_now();
    m_rif_mag_out = false;

    // [LEERE KAMMER] Nach einem Waffenwechsel ist `empty_when_dropped` geloescht,
    // die Kammer aber trotzdem leer. `rack.empty` (jeden Frame frisch aus
    // isGunAmmoEmpty) sagt es unabhaengig davon -- NUR mit vorhandenem
    // Slide-Joint, sonst gaebe es keinen Rack und die Waffe waere feuergesperrt.
    // [ACCESSOR-FOLGE] `rack.empty` zaehlt nur noch, wenn die 0 NICHT von unserem
    // eigenen Mag-Drop-Leeren stammt -- sonst rackt es nach jedem Reload.
    if (m_rifrack.empty_when_dropped
        || (m_rifrack.empty && m_rifwep.slide_joint != nullptr
            && !m_rifrack._zeroed_by_us)) {
        m_rifrack.needs = true;
    } else {
        m_rifrack.needs = false;
        m_rifrack._zeroed_by_us = false;   // Merker verbraucht

        if (m_rifwep.slide_joint != nullptr
            && !rif_engine_closes(m_rifwep.wid.value_or(0))) {
            glm::vec3 cur{};

            if (get_vec3(m_rifwep.slide_joint, "get_LocalPosition", cur)) {
                const auto& sp = rslide(m_rifwep.wid.value_or(0));
                set_vec3(m_rifwep.slide_joint, "set_LocalPosition",
                         glm::vec3{cur.x, cur.y, sp.rest_z});
            }
        }
    }

    m_rifrack.empty_when_dropped = false;
    rf_reload_ammo_on_insert();
}

// =====================================================================
// Slide-Rack (Pull-Geste) + Switch
// =====================================================================
void RE4VRReload2::rf_clear_rack() {
    m_rifrack.needs = false;
    m_rifrack.grab_active = false;
    m_rifrack.pulled = false;
    m_rifrack.frac = 0.0f;
    rf_gun_chamber();   // Engine aus AmmoEmpty -> Slide entriegelt

    if (rif_engine_closes(m_rifwep.wid.value_or(0))) {
        m_rifrack._chambered_hold = false;

        return;
    }

    m_rifrack._chambered_hold = true;

    if (m_rifwep.slide_joint != nullptr) {
        glm::vec3 cur{};

        if (get_vec3(m_rifwep.slide_joint, "get_LocalPosition", cur)) {
            const auto& sp = rslide(m_rifwep.wid.value_or(0));
            set_vec3(m_rifwep.slide_joint, "set_LocalPosition",
                     glm::vec3{cur.x, cur.y, sp.rest_z});
        }
    }
}

void RE4VRReload2::rf_update_rack_gesture() {
    if (m_rifrack.tuning) {
        return;
    }

    if (!m_rifrack.needs) {
        m_rifrack.grab_active = false;
        m_rifrack.pulled = false;
        m_rifrack.frac = 0.0f;
        m_rifrack.armed = false;

        return;
    }

    auto* sj = m_rifwep.slide_joint;

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

    if (!m_rifrack.grab_active) {
        if (m_rif_mag_hand) {
            m_rifrack.armed = false;

            return;
        }

        if (m_rifrack._ammo_input_t.has_value()
            && (clock_now() - *m_rifrack._ammo_input_t) < 0.2) {
            m_rifrack.armed = false;

            return;
        }

        if (!grip) {
            m_rifrack.armed = true;   // Grip erst loslassen -> scharf

            return;
        }

        if (!m_rifrack.armed) {
            return;
        }

        if (dist <= RACK_GRAB_DIST) {
            m_rifrack.grab_active = true;
            m_rifrack.armed = false;
            m_rifrack.pulled = false;
            m_rifrack.frac = 0.0f;
            m_rifrack.gx = hp->x;
            m_rifrack.gy = hp->y;
            m_rifrack.gz = hp->z;

            // [LAUFEN] Zweiter Anker: die Waffenhand. Ohne ihn steckt die
            // Fortbewegung im Zug (Gehen/Drehen bewegt BEIDE Haende) und der
            // Slide ging nur im Stand.
            if (const auto rhr = right_hand_raw_g(); rhr.has_value()) {
                m_rifrack.rgx = rhr->x;
                m_rifrack.rgy = rhr->y;
                m_rifrack.rgz = rhr->z;
            } else {
                m_rifrack.rgx.reset();
                m_rifrack.rgy.reset();
                m_rifrack.rgz.reset();
            }

            haptic_left(0.25f, 0.03f);
        }

        return;
    }

    if (grip) {
        const auto& sp_pose = rslide(m_rifwep.wid.value_or(0));
        const float travel = std::max(std::abs(sp_pose.back_z - park_ref(sp_pose)), 0.005f);
        float px = hp->x - m_rifrack.gx;
        float py = hp->y - m_rifrack.gy;
        float pz = hp->z - m_rifrack.gz;

        // [LAUFEN] Bewegung der Waffenhand abziehen -> uebrig bleibt die
        // Bewegung der Ziehhand GEGEN die Waffe.
        // Rueckbau: __re4_rack_relative = false
        if (m_rifrack.rgx.has_value()
            && re4vr::lua_get_tribool("__re4_rack_relative") != 0) {
            if (const auto rhn = right_hand_raw_g(); rhn.has_value()) {
                px -= (rhn->x - *m_rifrack.rgx);
                py -= (rhn->y - *m_rifrack.rgy);
                pz -= (rhn->z - *m_rifrack.rgz);
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
        m_rifrack.frac = (f > 1.0f) ? 1.0f : f;

        if (m_rifrack.frac >= 1.0f && !m_rifrack.pulled) {
            m_rifrack.pulled = true;

            if (const auto s = rif_snd_set(m_rifwep.wid.value_or(0)); s.has_value()) {
                rf_snd(s->slide_back);
            }
        }

        return;
    }

    // Grip losgelassen
    if (m_rifrack.pulled) {
        rf_clear_rack();
        haptic_left(0.95f, 0.07f);

        if (const auto s = rif_snd_set(m_rifwep.wid.value_or(0)); s.has_value()) {
            rf_snd(s->slide_forward);
        }
    } else {
        m_rifrack.grab_active = false;
        m_rifrack.frac = 0.0f;
        m_rifrack.pulled = false;
        m_rifrack.armed = true;
    }
}

// Dock-Ziel (Hand folgt dem Slide) publizieren. advance=true (on_frame): Blend
// EINMAL pro Frame vorruecken. advance=false (Render-Pass): nur mit dem
// aktuellen Blend frisch re-publishen (arm_chain liest pro Pass) OHNE doppelten
// Ramp -> kein Snap.
void RE4VRReload2::rf_update_dock_publish(bool advance) {
    // Quelle waehlen: Verstellschalter hat VORRANG vor dem Slide-Rack (wie LE5 in
    // motion). Beide docken die linke Hand ueber DIESELBEN Globals.
    ::REManagedObject* src_joint = nullptr;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    float rrx = 0.0f, rry = 0.0f, rrz = 0.0f;
    float want = 0.0f;

    if (m_switch_hand && m_rifwep.switch_joint != nullptr) {
        const auto& s = rswitch(m_rifwep.wid.value_or(0));
        src_joint = m_rifwep.switch_joint;
        ox = s.dx;
        oy = s.dy;
        oz = s.dz;
        rrx = s.hrx;
        rry = s.hry;
        rrz = s.hrz;
        want = 1.0f;
    } else if ((m_rifrack.grab_active || m_rifrack.dock_tune)
               && m_rifwep.slide_joint != nullptr) {
        const auto& sd = rslide(m_rifwep.wid.value_or(0));
        src_joint = m_rifwep.slide_joint;
        ox = sd.dock_x;
        oy = sd.dock_y;
        oz = sd.dock_z;
        rrx = sd.rack_rx;
        rry = sd.rack_ry;
        rrz = sd.rack_rz;
        want = 1.0f;
    }

    float b = m_rifrack.dock_blend;

    if (advance) {
        if (b < want) {
            b = std::min(b + DOCK_BLEND_SPEED, want);
        } else if (b > want) {
            b = std::max(b - DOCK_BLEND_SPEED, want);
        }

        m_rifrack.dock_blend = b;
    }

    glm::vec3 p{};
    glm::quat r{};

    if (b > 0.001f && src_joint != nullptr
        && get_vec3(src_joint, "get_Position", p)
        && get_quat(src_joint, "get_Rotation", r)) {
        if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
            p += r * glm::vec3{ox, oy, oz};
        }

        if (rrx != 0.0f || rry != 0.0f || rrz != 0.0f) {
            r = glm::normalize(r * quat_from_euler(rrx, rry, rrz));
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

// Slide-Joint-Z treiben (apply_slide_park-Aequivalent)
void RE4VRReload2::rf_apply_slide_park() {
    if (m_rifwep.slide_joint == nullptr) {
        return;
    }

    if (!(m_rifrack.empty || m_rif_mag_out || m_rifrack.needs || m_rifrack.grab_active
          || m_rifrack.tuning || m_rifrack._chambered_hold)) {
        return;
    }

    glm::vec3 cur{};

    if (!get_vec3(m_rifwep.slide_joint, "get_LocalPosition", cur)) {
        return;
    }

    const int32_t wid = m_rifwep.wid.value_or(0);
    const auto& sp = rslide(wid);
    // Slide bei leer/Nachladen/Ziehen in X ausgefahren (CQBR empty_x)
    bool extended = false;
    const float pz = park_ref(sp);   // Idle-/Pull-Basis
    float z = 0.0f;

    if (m_rifrack.tuning) {
        z = pz + (sp.back_z - pz) * m_rifrack.tune_frac;
        extended = true;
    } else if (m_rifrack.grab_active) {
        z = pz + (sp.back_z - pz) * m_rifrack.frac;
        extended = true;
    } else if (m_rifrack._chambered_hold) {
        if (rif_engine_closes(wid)) {
            return;
        }

        z = sp.rest_z;
    } else if (m_rifrack.needs || m_rifrack.empty_when_dropped || m_rifrack.empty) {
        z = pz;
        extended = true;
    } else {
        if (rif_engine_closes(wid)) {
            return;
        }

        z = sp.rest_z;
    }

    float nx = cur.x;

    if (sp.empty_x != 0.0f && m_rifwep.slide_rest_lp.has_value()) {
        nx = m_rifwep.slide_rest_lp->x + (extended ? sp.empty_x : 0.0f);
    }

    set_vec3(m_rifwep.slide_joint, "set_LocalPosition", glm::vec3{nx, cur.y, z});
}

// Verstellschalter: [SWITCH-GRIP-LATCH] wie LE5 (motion). Die Grip-FLANKE latcht
// NUR, wenn die linke Hand im Schalter-Radius ist -> faellt sie nicht rein,
// passiert NICHTS. Gelatcht bleibt es bis Grip losgelassen -> zum Wechsel
// Schalter<->Schaft IMMER neu greifen. Schalter hat VORRANG vor dem Slide.
void RE4VRReload2::rf_update_switch() {
    if (m_rifwep.switch_joint == nullptr) {
        m_switch_hand = false;
        m_rifsw.latched = false;
        m_rifsw._prev_grip = false;

        return;
    }

    const auto& s = rswitch(m_rifwep.wid.value_or(0));

    if (m_rifsw.preview) {
        m_switch_hand = true;
    } else {
        const bool grip = left_grip_down();

        // [SCHALTER VOR CHOKE 2026-09-17 -- Ansage des Users] Hand am Schalter
        // (oder haelt ihn schon) -> die Grip-Flanke gehoert dem Schalter und
        // NICHT dem Choke. Zeitstempel wie bei Bolt/Bogen; laeuft im Choke nach
        // 0,15 s von selbst aus. Gelesen in RE4VRChoke.cpp.
        if (!m_rifrack.needs) {
            glm::vec3 swp{};
            const auto swh = left_hand_world_g();

            if (swh.has_value()
                && get_vec3(m_rifwep.switch_joint, "get_Position", swp)
                && (vec_len(vec_sub(*swh, swp)) <= s.grab_dist || m_rifsw.latched)) {
                re4vr::lua_set_number("__re4_lh_switch_near_t", clock_now());
            }
        }

        // Schalter gesperrt, solange der Slide gezogen werden muss (Schalter und
        // Slide liegen zu nah beieinander).
        if (grip && !m_rifsw._prev_grip && !m_rifrack.needs) {
            glm::vec3 sjp{};
            const auto hp = left_hand_world_g();
            const bool have = get_vec3(m_rifwep.switch_joint, "get_Position", sjp)
                              && hp.has_value();
            const float d = have ? vec_len(vec_sub(*hp, sjp)) : 1e9f;
            m_rifsw.latched = (d <= s.grab_dist);
        }

        if (!grip || m_rifrack.needs) {
            m_rifsw.latched = false;
        }

        m_rifsw._prev_grip = grip;
        m_switch_hand = grip && m_rifsw.latched;

        const bool trig = left_trigger_down();

        if (m_switch_hand && trig && !m_rifsw._prev_trig) {
            m_rifsw.stage = 1 - m_rifsw.stage;

            if (const auto sn = rif_snd_set(m_rifwep.wid.value_or(0)); sn.has_value()) {
                rf_snd(sn->sw);
            }
        }

        m_rifsw._prev_trig = trig;
    }

    // prog lerpt zur Ziel-Stufe; die Stufe wird publiziert.
    const float target = static_cast<float>(m_rifsw.stage);

    if (m_rifsw.prog < target) {
        m_rifsw.prog = std::min(target, m_rifsw.prog + s.lerp);
    } else if (m_rifsw.prog > target) {
        m_rifsw.prog = std::max(target, m_rifsw.prog - s.lerp);
    }

    // Stufe 0 (default) = normal feuern. Stufe 1 = Feuer gesperrt -> Dry-Fire.
    re4vr::lua_set_number("__vr_rifle_fire_mode", m_rifsw.stage);
}

void RE4VRReload2::rf_apply_switch() {
    if (!(m_rifwep.switch_joint != nullptr && m_rifwep.switch_rest_rot.has_value())) {
        return;
    }

    const float p = m_rifsw.prog;

    if (p <= 0.0001f) {
        set_quat(m_rifwep.switch_joint, "set_LocalRotation", *m_rifwep.switch_rest_rot);

        return;
    }

    const auto& s = rswitch(m_rifwep.wid.value_or(0));
    set_quat(m_rifwep.switch_joint, "set_LocalRotation",
             glm::normalize(*m_rifwep.switch_rest_rot
                            * quat_from_euler(s.rx * p, s.ry * p, s.rz * p)));
}

// Mag-Mesh aus der Kammer halten, solange das Mag draussen ist
void RE4VRReload2::rf_apply_mag_out_hidden() {
    const bool should_hide = m_rif_mag_out && m_rifwep.mag_joint != nullptr
                             && !rifle_flow();

    if (should_hide) {
        set_vec3(m_rifwep.mag_joint, "set_LocalScale", glm::vec3{0.0f, 0.0f, 0.0f});
        m_rif_mag_hidden = true;
    } else if (m_rif_mag_hidden) {
        if (m_rifwep.mag_joint != nullptr) {
            set_vec3(m_rifwep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
        }

        m_rif_mag_hidden = false;
    }
}

// Hand-Pose direkt anwenden (Mag-Halten / Slide-Rack / Verstellschalter) ueber
// die reload2-EIGENEN Pose-Daten (kein Quer-Laden aus reload/gestures).
void RE4VRReload2::rf_apply_hand_pose() {
    const int32_t wid = m_rifwep.wid.value_or(0);
    std::string name{};
    glm::vec3 thumb{0.0f, 0.0f, 0.0f};

    if (m_rif_mag_hand || m_rifins.active || m_rif_mag_tune) {
        name = rif_mag_pose(wid);                       // 1) Mag in Hand
        const auto& m = rmaghand(wid);
        thumb = glm::vec3{m.t_rx, m.t_ry, m.t_rz};
    } else if (m_rifrack.dock_tune
               || (m_rifrack.needs && (m_rifrack.grab_active || m_rack_near))) {
        name = rif_rack_pose(wid);                      // 2) Slide ziehen
        const auto& sp = rslide(wid);
        thumb = glm::vec3{sp.st_rx, sp.st_ry, sp.st_rz};
    } else if (m_switch_hand || m_rifsw.preview) {
        name = rif_switch_pose(wid);                    // 3) Verstellschalter
    }

    // In Lua traegt pose_fade_step die Daumen-Daten mit; hier haelt der letzte
    // gehaltene Satz sie fest, damit das Ausblenden denselben Daumen benutzt.
    if (!name.empty()) {
        m_hand_fade_thumb = thumb;
    }

    // [POSE_FADE] beim Loslassen ueber POSE_FADE_DUR zurueckblenden statt snappen
    float b = 0.0f;

    if (!pose_fade_step(m_hand_fade, name, b)) {
        return;
    }

    const auto pit = m_rposes.find(m_hand_fade.name);

    if (pit == m_rposes.end()) {
        return;
    }

    pose_apply(pit->second, b);

    const glm::vec3 ft = m_hand_fade_thumb;

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

// Gemeinsamer interner Reset (Waffenwechsel / Save-Load / Script-Reset). Nur
// State, KEINE Globals.
    // [MAG_CARRY UEBERLEBT SAVE 07.09.2026 -- gemessen]
    // keep_carry = true kommt ausschliesslich vom [SAVE_LOAD]-Zweig: dort ist
    // die Waffe DIESELBE, sie wurde nur neu instanziiert (Speichern). Der
    // Rifle-State muss dann weg, der ausgeworfene Magazinrest aber NICHT --
    // er ist Buchhaltung ueber Munition, kein Waffenzustand.
    // Gemessen in reframework/data/re4_magcarry.txt (Killer 7, wid 4501):
    //   carry=12 -> speichern -> wid kurz -1 -> wid 4501 zurueck -> carry=nil.
    // Die 12 Schuss waren damit verloren, das Nachladen nahm nur die Reserve.
    // Beim echten Waffenwechsel (anderes wid) wird weiter geloescht, ebenso
    // beim Script-Reset -- dort ist der Merker wertlos bzw. gehoert der
    // anderen Waffe.
void RE4VRReload2::rifle_soft_reset(bool keep_carry) {
    rf_stop_drop();
    m_rif_mag_hand = false;
    m_rifins.active = false;
    m_rif_mag_tune = false;
    m_rif_mag_out = false;
    m_rif_mag_retained = 0;
    // Reset: der Spiegel darf nicht ueberleben
    if (!keep_carry) {
        re4vr::lua_set_nil("__re4_mag_carry");
        re4vr::lua_set_nil("__re4_mag_carry_wid");
    }

    m_rifrack.needs = false;
    m_rifrack.grab_active = false;
    m_rifrack.armed = false;
    m_rifrack.frac = 0.0f;
    m_rifrack.pulled = false;
    m_rifrack._chambered_hold = false;
    m_rifrack.dock_blend = 0.0f;
    m_rifrack.empty_when_dropped = false;
    m_rifrack._ammo_input_t.reset();
    m_rifrack.dock_tune = false;
    m_rifrack.tuning = false;
    // [ACCESSOR-FOLGE] Merker darf einen Wechsel/Reset nicht ueberleben
    m_rifrack._zeroed_by_us = false;

    m_rifsw.stage = 0;
    m_rifsw.prog = 0.0f;
    m_rifsw._prev_trig = false;
    m_rifsw.preview = false;
    m_rifsw.latched = false;
    m_rifsw._prev_grip = false;
    m_switch_hand = false;
}

void RE4VRReload2::rifle_on_frame() {
    rifle_refresh();

    if (m_rifwep.wid != m_rif_prev_wid) {
        // Waffenwechsel: internen Rifle-State IMMER abraeumen. War vorher eine
        // Rifle managed und jetzt nicht mehr -> auch die Rifle-Globals freigeben,
        // sonst zieht ein stale Slide-Dock die linke Hand fehl.
        const bool was_rifle = m_rif_prev_wid.has_value();
        rifle_soft_reset();

        if (was_rifle && !m_rifwep.wid.has_value()) {
            re4vr::lua_set_bool("__vr_needs_rack", false);
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
            re4vr::lua_set_nil("__vr_rack_hand_pose");
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            re4vr::lua_set_bool("__vr_burst_active", false);
        }

        // [SAVE_LOAD] Frisch ins/zurueck ins Rifle gewechselt -> evtl. stale
        // Item-Cache raus (falls die Waffe beim Load kurz auf nil ging).
        if (m_rifwep.wid.has_value()) {
            re4vr::lua_set_nil("__re4_live_wi");
        }

        m_rif_prev_wid = m_rifwep.wid;
    }

    if (!m_rifwep.wid.has_value()) {
        // nicht-Rifle equippt: nichts publishen (Revolver/reload regeln ihre
        // Faelle selbst)
        return;
    }

    if (m_rif_reacquired) {
        // [SAVE_LOAD] gleiche Waffe neu instanziiert (kein wid-Wechsel) -> stale
        // Item-Cache + State abraeumen.
        m_rif_reacquired = false;
        re4vr::lua_set_nil("__re4_live_wi");
        rifle_soft_reset(true);   // Magazinrest ueberlebt das Speichern
    }

    rf_capture_mag_rest();
    rf_check_insert_proximity();

    // Binding faengt den rechten B ab (manueller Reload statt nativem)
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);

    // Slide-/Empty-State pflegen
    const auto loaded = rf_loaded();
    m_rifrack.empty = rf_gun_ammo_empty();
    m_rifrack.has_mag = loaded.has_value() && *loaded > 0;

    // Nach-Rack-Halt beim Schuss loesen (bei ENGINE_CLOSES nie aktiv)
    if (m_rifrack._chambered_hold) {
        if (m_rifrack._prev_ga.has_value() && loaded.has_value()
            && *loaded < *m_rifrack._prev_ga) {
            m_rifrack._chambered_hold = false;
        }

        m_rifrack._prev_ga = loaded;
    }

    // UI auf 0 halten, solange das Mag draussen ist (gegen Engine-Re-Sync);
    // nicht waehrend des Inserts.
    if (m_rif_mag_out && !m_rifins.active) {
        auto* wi = rf_get_wi();

        if (wi != nullptr && call_enum(wi, "get_CurrentAmmoCount").value_or(0) > 0) {
            if (m_main != nullptr) {
                // [MAG-REST] merken, bevor genullt wird
                m_main->carry_capture(wi, "re4_vr_reload2.lua:2451", std::nullopt);
            }

            field_i32_write(wi, "_CurrentAmmoCount", 0x44, 0);
        }
    }

    // Holster-Gate: nur SPERREN/buzzen, wenn das Mag DRAUSSEN ist und es NICHTS
    // zu laden gibt. Mag noch drin -> KEIN Buzz (SMG-Verhalten).
    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        m_rif_mag_out && !m_rif_mag_hand && !m_rifins.active
                        && !((m_rif_mag_retained + rf_reserve()) > 0));

    rf_update_rack_gesture();
    rf_update_switch();

    // Rack-Naehe fuer die Hand-Pose merken
    {
        auto* sj = m_rifwep.slide_joint;
        const auto hp = (sj != nullptr) ? left_hand_world_g() : std::nullopt;
        glm::vec3 sp{};
        m_rack_near = hp.has_value() && get_vec3(sj, "get_Position", sp)
                      && vec_len(vec_sub(*hp, sp)) <= RACK_GRAB_DIST;
    }

    // Globals fuer motion/arm_chain/binding publishen
    const int32_t wid = *m_rifwep.wid;
    re4vr::lua_set_bool("__vr_needs_rack", m_rifrack.needs);
    re4vr::lua_set_bool("__vr_slide_rack_active", m_rifrack.grab_active);
    re4vr::lua_set_bool("__vr_mag_in_hand",
                        m_rif_mag_hand || m_rifins.active
                        || (m_rifrack._ammo_input_t.has_value()
                            && (clock_now() - *m_rifrack._ammo_input_t) < 0.2));

    if (m_rifrack.needs && (m_rifrack.grab_active || m_rack_near)) {
        re4vr::lua_set_string("__vr_rack_hand_pose", rif_rack_pose(wid));
    } else {
        re4vr::lua_set_nil("__vr_rack_hand_pose");
    }

    // [KRITISCHES GATE -- LIVE] Feuer-Block NUR aus frischen Engine-Quellen, NIE
    // aus gecachtem Ammo (rf_loaded -> nach Save-Load stale 0 -> Dauer-Dry-Fire).
    // rack.empty = pe:isGunAmmoEmpty, jeden Frame frisch. Zusaetzlich:
    // Verstellschalter Stufe 1 sperrt das Feuern bewusst (Stufe 0 feuert normal).
    re4vr::lua_set_bool("__vr_block_fire_when_empty",
                        m_rifrack.needs || m_rif_mag_out || rifle_flow()
                        || m_rifrack.empty
                        || (rif_switch_blocks_fire(wid) && m_rifsw.stage == 1));
    // [BF-DIAG] wer hat den Feuer-Block zuletzt gesetzt?
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:2133");

    // [BURST] Schalter-Stufe 1 auf Burst-Waffen (CQBR) -> binding feuert nur N
    // Schuss pro Trigger-Zug.
    if (const auto bn = rif_switch_burst(wid); bn.has_value() && m_rifsw.stage == 1) {
        re4vr::lua_set_bool("__vr_burst_active", true);
        re4vr::lua_set_number("__vr_burst_count", *bn);
    } else {
        re4vr::lua_set_bool("__vr_burst_active", false);
    }

    re4vr::lua_set_bool("__vr_rack_block_left_knife", m_rifrack.needs);

    rf_update_dock_publish(true);   // Blend hier EINMAL pro Frame vorruecken

    // Mag-Boden-Sound: beim Drop-Start geplant, hier feuern
    if (m_rif_mag_floor_at > 0.0 && clock_now() >= m_rif_mag_floor_at) {
        m_rif_mag_floor_at = 0.0;

        if (const auto s = rif_snd_set(wid); s.has_value()) {
            rf_snd(s->mag_floor);
        }
    }

    // Dry-Fire bei gesperrtem Trigger
    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_rif_dry_prev) {
        if (const auto s = rif_snd_set(wid); s.has_value()) {
            rf_snd(s->dry_fire);
        }
    }

    m_rif_dry_prev = et;

    // B-Flanke -> Mag-Auswurf
    const bool bd = right_b_down();
    // [MAG_B_GUARD 2026-09-10] Nur Karte, Typewriter und Inventar: dort ist der
    // rechte B Zentrieren bzw. Zurueck und darf die Waffe nicht anfassen.
    // Geblockt wird nur die AKTION: die Flanke wird weiter gepflegt, sonst
    // feuert ein beim Schliessen noch gehaltener B sofort den Auswurf.
    const bool b_menu = re4vr::lua_get_tribool("__re4_mag_block") == 1;

    if (bd && !m_rif_rb_prev && !b_menu) {
        rf_force_eject();
    }

    m_rif_rb_prev = bd;
}

// Render-Pass (voller Override-Stack; NACH reload + Revolver -> gewinnt)
void RE4VRReload2::rifle_apply_pass() {
    if (!(m_rifcfg.rifle_enabled && m_rifwep.wid.has_value())) {
        return;
    }

    rf_update_drop();
    rf_update_mag_in_hand();
    rf_update_insert();
    rf_apply_slide_park();
    rf_apply_mag_out_hidden();
    rf_apply_switch();
    // Render-Pass: nur frisch re-publishen, den Blend NICHT nochmal vorruecken
    rf_update_dock_publish(false);
    rf_apply_hand_pose();
}

void RE4VRReload2::rifle_on_script_reset() {
    // Joints in Ruhe zuruecksetzen, solange wir die echte Ruhe noch kennen
    if (m_rifwep.switch_joint != nullptr && m_rifwep.switch_rest_rot.has_value()) {
        set_quat(m_rifwep.switch_joint, "set_LocalRotation", *m_rifwep.switch_rest_rot);
    }

    if (m_rifwep.mag_joint != nullptr) {
        set_vec3(m_rifwep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
    }

    m_rifwep = RifleWep{};
    rifle_soft_reset();
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_slide_rack_active", false);
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    re4vr::lua_set_nil("__vr_rack_hand_pose");
}

// ---------------------------------------------------------------------
// UI -- Rifle. In Lua haengt sie als _G.__re4_reload2_rifle_ui unter demselben
// Header wie der Revolver (EIN Menue-Eintrag, Gattungen darin gestapelt).
// ---------------------------------------------------------------------
void RE4VRReload2::rifle_ui() {
    if (ImGui::Checkbox("##rifle_en", &m_rifcfg.rifle_enabled)) {
        rifle_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Rifle Reload");

    if (!ImGui::TreeNode("Rifle -- Einstellungen")) {
        return;
    }

    const auto awid = m_rifwep.wid.has_value() ? m_rifwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_rifle(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Rifle erkannt - verwaltet)"
                             : "  (keine eingerichtete Rifle equippt)");

    const auto jc = awid.has_value() ? rif_joints(*awid) : std::nullopt;
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Joints: Mag=%s  Slide=%s  Schalter=%s  (mag_joint=%s)",
                       jc.has_value() ? jc->mag : "nil",
                       jc.has_value() ? jc->slide : "nil",
                       jc.has_value() ? jc->sw : "nil",
                       m_rifwep.mag_joint != nullptr ? "ok" : "nil");

    const auto ld = rf_loaded();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  loaded=%s reserve=%d mag_out=%s needs_rack=%s empty=%s",
                       ld.has_value() ? std::to_string(*ld).c_str() : "nil",
                       rf_reserve(),
                       m_rif_mag_out ? "true" : "false",
                       m_rifrack.needs ? "true" : "false",
                       m_rifrack.empty ? "true" : "false");

    {   // [DISTANZ PRO WAFFE] Der Slider zeigt/aendert IMMER die gezogene Waffe.
        const int32_t dwid = m_rifwep.wid.value_or(4401);
        char lbl[64]{};
        std::snprintf(lbl, sizeof(lbl), "Einlege-Distanz m (Mag) -- wp%d##rifdist", dwid);

        if (ImGui::SliderFloat(lbl, &rdock(dwid).insert, 0.03f, 0.50f)) {
            rifle_save_cfg();
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Stingray %.3f m   |   CQBR %.3f m",
                           rdock(4401).insert, rdock(4402).insert);
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen", &m_rifcfg.reload_ammo)) {
        rifle_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an", &m_rifcfg.sound_enabled)) {
        rifle_save_cfg();
    }

    const int32_t wid = awid.value_or(4401);

    if (ImGui::TreeNode("Slide-Pose (Z + Hand-Dock)")) {
        auto& sp = rslide(wid);
        bool ch = false;
        ImGui::Checkbox("Vorschau: Slide-Z per Regler##riftune", &m_rifrack.tuning);

        if (m_rifrack.tuning) {
            ImGui::SliderFloat("Vorschau 0..1##riftunef", &m_rifrack.tune_frac, 0.0f, 1.0f);
        }

        ImGui::Checkbox("Vorschau: Hand ans Slide-Dock zwingen##rifdocktune",
                        &m_rifrack.dock_tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Dock-Vorschau erzwingt die Hand an den Slide -> "
                           "dock_x/y/z + rack_rx/ry/rz live justierbar)");
        ch |= ImGui::SliderFloat("rest_z (vorne/gechambert)##rifrz", &sp.rest_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("park_z (leer/MITTEL)##rifpz", &sp.park_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("back_z (Rack-Endpunkt)##rifbz", &sp.back_z, -0.20f, 0.50f);
        ch |= ImGui::SliderFloat("empty_x (X-Ausfahren bei leer, CQBR)##rifex",
                                 &sp.empty_x, -0.10f, 0.10f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Hand-Dock am Slide:");
        ch |= ImGui::SliderFloat("dock_x##rifdx", &sp.dock_x, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_y##rifdy", &sp.dock_y, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_z##rifdz", &sp.dock_z, -0.40f, 0.40f);
        ch |= ImGui::SliderFloat("rack_rx##rifrrx", &sp.rack_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("rack_ry##rifrry", &sp.rack_ry, -180.0f, 360.0f);
        ch |= ImGui::SliderFloat("rack_rz##rifrrz", &sp.rack_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen der Slide-Zieh-Pose (additiv):");
        ch |= ImGui::SliderFloat("Daumen RotX##rifstx", &sp.st_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##rifsty", &sp.st_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##rifstz", &sp.st_rz, -90.0f, 90.0f);

        if (ch) {
            rifle_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Dock-Port (Mag-Einlegepunkt)")) {
        auto& d = rdock(wid);
        bool ch = false;
        char jb[64]{};
        std::snprintf(jb, sizeof(jb), "%s", d.joint.c_str());

        if (ImGui::InputText("Port-Joint##rifdj", jb, sizeof(jb))) {
            d.joint = jb;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Port X##rifpx", &d.x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Port Y##rifpy", &d.y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Port Z##rifpz2", &d.z, -0.20f, 0.20f);

        if (ch) {
            rifle_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Mag in Hand (Offset + Daumen)")) {
        auto& m = rmaghand(wid);
        bool ch = false;
        ImGui::Checkbox("Vorschau: Mag in der Hand zwingen##rifmagtune", &m_rif_mag_tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (erzwingt Mag+Pose dauerhaft in der linken Hand -> "
                           "Offset live justierbar)");
        ch |= ImGui::SliderFloat("PosX##rifmx", &m.x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosY##rifmy", &m.y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosZ##rifmz", &m.z, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("RotX##rifmrx", &m.rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotY##rifmry", &m.ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotZ##rifmrz", &m.rz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Daumen RotX##riftx", &m.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##rifty", &m.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##riftz", &m.t_rz, -90.0f, 90.0f);

        if (ch) {
            rifle_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Verstellschalter _05 (2 Stufen)")) {
        auto& s = rswitch(wid);
        bool ch = false;
        ImGui::Checkbox("Vorschau: Schalter umlegen##rifswprev", &m_rifsw.preview);

        if (m_rifsw.preview && ImGui::Button("Stufe 0/1 togglen##rifswtog")) {
            m_rifsw.stage = 1 - m_rifsw.stage;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Stufe: %d  (Greifen: Hand an _05 + frischer Grip + "
                           "Left-Trigger)", m_rifsw.stage);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Vorschau erzwingt die Hand an den Schalter -> "
                           "dock_x/y/z + Hand-Rot live justierbar)");
        ch |= ImGui::SliderFloat("Stufe1 RotX##rifswrx", &s.rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Stufe1 RotY##rifswry", &s.ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Stufe1 RotZ##rifswrz", &s.rz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Dreh-Geschw. (lerp)##rifswl", &s.lerp, 0.02f, 1.0f);
        ch |= ImGui::SliderFloat("Greif-Distanz##rifswg", &s.grab_dist, 0.05f, 0.30f);
        ImGui::Separator();
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Hand-Dock am Schalter (Hand-Position/-Rotation "
                           "relativ zu _05):");
        ch |= ImGui::SliderFloat("dock_x##rifswdx", &s.dx, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_y##rifswdy", &s.dy, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_z##rifswdz", &s.dz, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Hand RotX##rifswhrx", &s.hrx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotY##rifswhry", &s.hry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotZ##rifswhrz", &s.hrz, -180.0f, 180.0f);

        if (ch) {
            rifle_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Stingray = LE5-Logik (Mag _04 + Slide-Rack _02 + Schalter "
                       "_05). Werte von LE5 geseedet -> visuell nachtunen.");
    ImGui::TreePop();
}

// ============================================================================
// 3 -- BOLT-ACTION (wp4400 SR M1903, Lua Z.2821-3917)
//
// joint_01 = Bolt-Slide, joint_10 = Patrone.
// Oeffnen: Hand an _01 + Grip -> roll links ~70 Grad, DANN Z nach hinten.
// Laden (Bolt offen): ins Mag-Holster greifen -> Patrone (_10) in die Hand
// (Pose Shotgunshell) -> an die Kammer fuehren -> ammocount +1 (einzeln, keine
// Ratio), Reserve -1. Beliebig oft bis Cap/Reserve leer.
// Schliessen: Z ganz nach vorne, DANN roll rechts -> chambern.
// Feuern bleibt NATIV; der manuelle Bolt ist NUR das Nachladen. Feuer gesperrt,
// solange offen. Eigenes JSON: re4_vr/re4_vr_reload2_bolt.json
// ============================================================================
namespace {

constexpr const char* BOLT_CFG_PATH = "re4_vr/re4_vr_reload2_bolt.json";
constexpr float BOLT_DOCK_BLEND_SPEED = 0.10f;

// ---- Joints (CODE-Konstanten) ----
constexpr const char* BOLT_J_BOLT = "_01";
constexpr const char* BOLT_J_CART = "_10";

// ---- Sounds (von der Stingray geseedet) ----
struct BoltSnd {
    uint32_t bolt_open, bolt_close, cart_grab, insert, cart_floor, dry_fire;
};

constexpr BoltSnd BSND{3388506884u, 3505191890u, 1839787494u, 403849506u,
                       1302378315u, 812850326u};

// [CYCLE-SUPPRESS] der Node der Schuss-Nachanimation
constexpr const char* BOLT_CYCLE_NODE = "wp4400_general_0513_Aim_Fire_after";
// s Nachlauf der Erkennung (nur fuer das Flag, nicht fuers Spulen)
constexpr double BOLT_IN_CYCLE_HOLD = 0.10;

}   // namespace

bool RE4VRReload2::is_bolt(int32_t wid) {
    return wid == 4400;
}

RE4VRReload2::BoltCfg& RE4VRReload2::bcfg(int32_t wid) {
    auto it = m_bcfg.find(wid);

    if (it == m_bcfg.end()) {
        it = m_bcfg.emplace(wid, BoltCfg{}).first;
    }

    return it->second;
}

void RE4VRReload2::bolt_load_cfg() {
    const auto data = re4vr::json_load(BOLT_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    if (const auto it = data.find("cfg"); it != data.end() && it->is_object()) {
        m_bscalar.bolt_enabled = jbool(*it, "bolt_enabled", m_bscalar.bolt_enabled);
        m_bscalar.reload_ammo = jbool(*it, "reload_ammo", m_bscalar.reload_ammo);
        m_bscalar.sound_enabled = jbool(*it, "sound_enabled", m_bscalar.sound_enabled);
        m_bscalar.insert_distance = jnum(*it, "insert_distance", m_bscalar.insert_distance);
    }

    if (const auto it = data.find("tune"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& c = bcfg(wid);
            c.back_off = jnum(v, "back_off", c.back_off);
            c.travel = jnum(v, "travel", c.travel);
            c.open_lerp = jnum(v, "open_lerp", c.open_lerp);
            c.brx = jnum(v, "brx", c.brx);
            c.bry = jnum(v, "bry", c.bry);
            c.brz = jnum(v, "brz", c.brz);
            c.grab_dist = jnum(v, "grab_dist", c.grab_dist);
            c.dock_x = jnum(v, "dock_x", c.dock_x);
            c.dock_y = jnum(v, "dock_y", c.dock_y);
            c.dock_z = jnum(v, "dock_z", c.dock_z);
            c.hrx = jnum(v, "hrx", c.hrx);
            c.hry = jnum(v, "hry", c.hry);
            c.hrz = jnum(v, "hrz", c.hrz);
            c.cx = jnum(v, "cx", c.cx);
            c.cy = jnum(v, "cy", c.cy);
            c.cz = jnum(v, "cz", c.cz);
            c.crx = jnum(v, "crx", c.crx);
            c.cry = jnum(v, "cry", c.cry);
            c.crz = jnum(v, "crz", c.crz);
            c.t_rx = jnum(v, "t_rx", c.t_rx);
            c.t_ry = jnum(v, "t_ry", c.t_ry);
            c.t_rz = jnum(v, "t_rz", c.t_rz);
            c.cd_x = jnum(v, "cd_x", c.cd_x);
            c.cd_y = jnum(v, "cd_y", c.cd_y);
            c.cd_z = jnum(v, "cd_z", c.cd_z);
            c.cd_joint = jstr(v, "cd_joint", c.cd_joint);
        }
    }
}

void RE4VRReload2::bolt_save_cfg() {
    nlohmann::json tune = nlohmann::json::object();
    const auto& c = bcfg(4400);
    tune["4400"] = {
        {"back_off", c.back_off}, {"travel", c.travel}, {"open_lerp", c.open_lerp},
        {"brx", c.brx}, {"bry", c.bry}, {"brz", c.brz}, {"grab_dist", c.grab_dist},
        {"dock_x", c.dock_x}, {"dock_y", c.dock_y}, {"dock_z", c.dock_z},
        {"hrx", c.hrx}, {"hry", c.hry}, {"hrz", c.hrz},
        {"cx", c.cx}, {"cy", c.cy}, {"cz", c.cz},
        {"crx", c.crx}, {"cry", c.cry}, {"crz", c.crz},
        {"t_rx", c.t_rx}, {"t_ry", c.t_ry}, {"t_rz", c.t_rz},
        {"cd_x", c.cd_x}, {"cd_y", c.cd_y}, {"cd_z", c.cd_z},
        {"cd_joint", c.cd_joint},
    };

    nlohmann::json d = {
        {"cfg", {{"bolt_enabled", m_bscalar.bolt_enabled},
                 {"reload_ammo", m_bscalar.reload_ammo},
                 {"sound_enabled", m_bscalar.sound_enabled},
                 {"insert_distance", m_bscalar.insert_distance}}},
        {"tune", tune},
    };

    re4vr::json_save(BOLT_CFG_PATH, d);
}

// ---- Live-Weapon-Item + Ammo (eigene Helfer; gegen die EQUIPPTE wid validiert)
// Der Bolt-Block benutzt bewusst NICHT den Accessor-Weg der Rifle, sondern
// __re4_live_wi + getEquipWeaponItem -- 1:1 uebernommen.
::REManagedObject* RE4VRReload2::bget_wi() {
    const auto ewid = get_equip_wid();
    auto* wi = re4vr::lua_get_pointer("__re4_live_wi");

    if (wi != nullptr && call_enum(wi, "get_CurrentAmmoCount").has_value()) {
        // STRIKT: nur benutzen, wenn es dieselbe Waffe ist (kein stale)
        const auto cwid = call_enum(wi, "get_WeaponId");

        if (cwid.has_value() && ewid.has_value() && *cwid == *ewid) {
            return wi;
        }
    }

    auto* ewi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

std::optional<int32_t> RE4VRReload2::bloaded() {
    auto* wi = bget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
}

int32_t RE4VRReload2::bcap() {
    auto* wi = bget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload2::breserve() {
    auto* wi = bget_wi();

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

// ---- chainsaw.Gun (Chambern), 1:1 wie die Stingray ----
void RE4VRReload2::bolt_chamber() {
    rf_gun_chamber();   // identischer Code; der Bolt-Block hat in Lua nur eine Kopie
}

void RE4VRReload2::bsnd(uint32_t id) {
    if (!m_bscalar.sound_enabled || id == 0 || m_bwep.tf == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_bwep.tf, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    // [BOLT_MUTE SELBST-AUSNAHME] Merker setzen, damit der Mute-Hook UNSERE
    // eigenen Laute durchlaesst. Ohne ihn schluckt das Fenster nach dem Schuss
    // auch Dry-Fire, bolt_open/close und die Nachlade-Sounds.
    re4vr::lua_set_bool("__re4_bolt_snd_self", true);
    m_bolt_snd_self = true;
    trigger_sound(go, id);
    m_bolt_snd_self = false;
    re4vr::lua_set_bool("__re4_bolt_snd_self", false);
}

void RE4VRReload2::bolt_refresh() {
    const auto ewid = get_equip_wid();
    const bool managed = m_bscalar.bolt_enabled && ewid.has_value() && is_bolt(*ewid);

    if (!managed) {
        m_bwep.wid.reset();
        m_bwep.tf = nullptr;
        m_bwep.bolt_joint = nullptr;
        m_bwep.cart_joint = nullptr;
        m_bwep.bolt_rest_rot.reset();

        return;
    }

    if (m_bwep.wid.has_value() && *m_bwep.wid == *ewid
        && m_bwep.bolt_joint != nullptr && m_bwep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_bwep.tf, "get_Position", p)) {
            return;
        }
    }

    const bool same_wid = m_bwep.wid.has_value() && *m_bwep.wid == *ewid;
    m_bwep.wid.reset();
    m_bwep.tf = nullptr;
    m_bwep.bolt_joint = nullptr;
    m_bwep.cart_joint = nullptr;
    m_bwep.bolt_rest_rot.reset();

    auto* tf = find_weapon(*ewid);

    if (tf == nullptr) {
        return;
    }

    auto* bj = joint_by_name(tf, BOLT_J_BOLT);

    if (bj == nullptr) {
        return;
    }

    m_bwep.wid = *ewid;
    m_bwep.tf = tf;
    m_bwep.bolt_joint = bj;
    m_bwep.cart_joint = joint_by_name(tf, BOLT_J_CART);

    // [NULLLAGE] Die Ruhelage wurde frueher bei JEDEM Neu-Greifen frisch
    // gemessen. Passiert das, waehrend der Bolt hinten steht (Stagger,
    // Waffen-Respawn, Engine-Anim), wird die ZURUECKGEZOGENE Position zur neuen
    // "Ruhe" -- und unser Offset kommt danach obendrauf. Deshalb pro Waffe
    // EINMAL merken und nie wieder ueberschreiben.
    const int32_t wid = *ewid;

    if (const auto it = m_bolt_rest_rot.find(wid); it != m_bolt_rest_rot.end()) {
        m_bwep.bolt_rest_rot = it->second;
    } else {
        glm::quat r{};

        if (get_quat(bj, "get_LocalRotation", r)) {
            m_bwep.bolt_rest_rot = r;
            m_bolt_rest_rot[wid] = r;
        }
    }

    if (const auto it = m_bolt_rest_lp.find(wid); it != m_bolt_rest_lp.end()) {
        m_bwep.bolt_rest_lp = it->second;
    } else {
        glm::vec3 lp{};

        if (get_vec3(bj, "get_LocalPosition", lp)) {
            m_bolt_rest_lp[wid] = lp;
            m_bwep.bolt_rest_lp = lp;
        }
    }

    if (same_wid) {
        m_bolt_reacquired = true;
    }
}

void RE4VRReload2::bolt_capture_cart_rest() {
    if (m_bwep.cart_joint == nullptr) {
        return;
    }

    if (m_bcart.active || m_bcart.insert || m_bcart.tune) {
        return;
    }

    glm::vec3 lp{};
    glm::quat lr{};

    if (get_vec3(m_bwep.cart_joint, "get_LocalPosition", lp)) {
        m_bwep.cart_rest_lp = lp;
    }

    if (get_quat(m_bwep.cart_joint, "get_LocalRotation", lr)) {
        m_bwep.cart_rest_lr = lr;
    }
}

// ---- Settle: roll/zf sanft zu troll/tzf lerpen (Snap-frei) ----
void RE4VRReload2::bolt_settle_step(const BoltCfg& c) {
    const float sp = c.open_lerp;

    if (m_bolt.roll != m_bolt.troll) {
        m_bolt.roll = (m_bolt.roll < m_bolt.troll)
            ? std::min(m_bolt.troll, m_bolt.roll + sp)
            : std::max(m_bolt.troll, m_bolt.roll - sp);
    }

    if (m_bolt.zf != m_bolt.tzf) {
        m_bolt.zf = (m_bolt.zf < m_bolt.tzf)
            ? std::min(m_bolt.tzf, m_bolt.zf + sp)
            : std::max(m_bolt.tzf, m_bolt.zf - sp);
    }
}

// ---- Bolt-Gesten (Oeffnen: roll dann Z; Schliessen: Z dann roll) ----
// EIN Griff = EINE Transition: ist offen/zu erreicht -> locked, weitere Bewegung
// im selben Griff ignoriert (kein Z-Wiggle ohne Rotation). Unvollstaendiges
// Loslassen lerpt sanft in die Ausgangslage der Geste zurueck.
void RE4VRReload2::update_bolt_gesture() {
    if (m_bolt.preview) {
        return;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(4400));
    auto* bj = m_bwep.bolt_joint;

    if (bj == nullptr) {
        m_bolt.grab = false;
        bolt_settle_step(c);

        return;
    }

    const auto hp = left_hand_world_g();
    glm::vec3 sp{};

    if (!hp.has_value() || !get_vec3(bj, "get_Position", sp)) {
        bolt_settle_step(c);

        return;
    }

    const bool grip = left_grip_down();
    const float dist = vec_len(vec_sub(*hp, sp));

    // [NACHLADEN VOR CHOKE 2026-09-17 -- Ansage des Users] Wie Bogen-Hebel und
    // Red9-Klappe: steht die linke Hand am Bolt-Greifpunkt (oder haelt sie ihn),
    // gehoert die Grip-Flanke dem Bolt und NICHT dem Choke. Zeitstempel statt
    // Flag: tickt das hier nicht mehr (Waffe gewechselt), laeuft die Sperre im
    // Choke nach 0,15 s von selbst aus. Gelesen in RE4VRChoke.cpp.
    if (dist <= c.grab_dist || m_bolt.grab) {
        re4vr::lua_set_number("__re4_lh_reload_grab_t", clock_now());
    }

    if (!m_bolt.grab) {
        bolt_settle_step(c);   // nicht gegriffen -> ggf. zuruecklerpen

        if (m_bcart.active || m_bcart.insert) {
            m_bolt.armed = false;

            return;
        }

        if (!grip) {
            m_bolt.armed = true;   // Grip erst loslassen -> scharf

            return;
        }

        if (!m_bolt.armed) {
            return;
        }

        if (dist <= c.grab_dist) {
            m_bolt.grab = true;
            m_bolt.armed = false;
            m_bolt.locked = false;
            m_bolt.gx = hp->x;
            m_bolt.gy = hp->y;
            m_bolt.gz = hp->z;

            // [STAGGER] Zweiter Anker: die Waffenhand. Der Zug misst sonst gegen
            // einen WELTpunkt -- ein Stagger schiebt Koerper und Waffe, der Anker
            // bleibt stehen, und die Differenz landet als zusaetzlicher Zug im
            // Bolt.
            if (const auto r = right_hand_raw_g(); r.has_value()) {
                m_bolt.rgx = r->x;
                m_bolt.rgy = r->y;
                m_bolt.rgz = r->z;
            } else {
                m_bolt.rgx.reset();
                m_bolt.rgy.reset();
                m_bolt.rgz.reset();
            }

            m_bolt.zf_anchor = m_bolt.zf;
            m_bolt.troll = m_bolt.roll;
            m_bolt.tzf = m_bolt.zf;
            m_bolt.mode = m_bolt.open ? "close" : "open";
            haptic_left(0.25f, 0.03f);
        }

        return;
    }

    if (grip) {
        if (m_bolt.locked) {
            bolt_settle_step(c);   // Transition fertig -> sanft aufs Ziel lerpen

            // [ONE_GRAB_CYCLE] Kein Re-Grab noetig: nach dem Auf-Lock kurz
            // EINRASTEN, dann im SELBEN Griff auf "close" umschalten (Anker =
            // aktuelle Hand) -> Zurueckdruecken schliesst direkt. Nur Auf->Zu;
            // Zu bleibt gelockt bis Grip-Release.
            if (m_bolt.open && m_bolt.mode == "open"
                && (clock_now() - m_bolt.lock_t) >= 0.12) {
                m_bolt.locked = false;
                m_bolt.mode = "close";
                m_bolt.gx = hp->x;
                m_bolt.gy = hp->y;
                m_bolt.gz = hp->z;

                if (const auto r = right_hand_raw_g(); r.has_value()) {
                    m_bolt.rgx = r->x;
                    m_bolt.rgy = r->y;
                    m_bolt.rgz = r->z;
                } else {
                    m_bolt.rgx.reset();
                    m_bolt.rgy.reset();
                    m_bolt.rgz.reset();
                }

                m_bolt.zf_anchor = m_bolt.zf;
                m_bolt.troll = m_bolt.roll;
                m_bolt.tzf = m_bolt.zf;
                haptic_left(0.35f, 0.03f);   // Einrast-Klick
            }

            return;
        }

        const float travel = std::max(c.travel, 0.01f);
        float px = hp->x - m_bolt.gx;
        float py = hp->y - m_bolt.gy;
        float pz = hp->z - m_bolt.gz;

        // [STAGGER] Bewegung der Waffenhand abziehen -> uebrig bleibt die
        // Bewegung der Ziehhand GEGEN die Waffe.
        if (m_bolt.rgx.has_value()
            && re4vr::lua_get_tribool("__re4_rack_relative") != 0) {
            if (const auto rn = right_hand_raw_g(); rn.has_value()) {
                px -= (rn->x - *m_bolt.rgx);
                py -= (rn->y - *m_bolt.rgy);
                pz -= (rn->z - *m_bolt.rgz);
            }
        }

        glm::quat brot{};
        float pull = pz;

        if (get_quat(bj, "get_Rotation", brot)) {
            glm::vec3 bd = brot * glm::vec3{0.0f, 0.0f, -1.0f};
            const float bl = vec_len(bd);

            if (bl > 1e-6f) {
                bd /= bl;
            }

            pull = px * bd.x + py * bd.y + pz * bd.z;
        }

        if (m_bolt.mode == "open") {
            if (m_bolt.roll < 1.0f) {
                m_bolt.roll = std::min(1.0f, m_bolt.roll + c.open_lerp);
            }

            if (m_bolt.roll >= 0.999f) {
                m_bolt.zf = clamp01(m_bolt.zf_anchor + pull / travel);
            }

            // weit genug offen -> lock (der Rest lerpt voll auf)
            if (m_bolt.roll >= 0.999f && m_bolt.zf >= 0.6f) {
                m_bolt.open = true;
                m_bolt.locked = true;
                // [ONE_GRAB_CYCLE] Einrast-Start (Auf->Zu ohne Re-Grab)
                m_bolt.lock_t = clock_now();
                m_bolt.troll = 1.0f;
                m_bolt.tzf = 1.0f;
                haptic_left(0.6f, 0.05f);
                bsnd(BSND.bolt_open);
            }
        } else {
            m_bolt.zf = clamp01(m_bolt.zf_anchor + pull / travel);

            if (m_bolt.zf <= 0.001f && m_bolt.roll > 0.0f) {
                m_bolt.roll = std::max(0.0f, m_bolt.roll - c.open_lerp);
            }

            if (m_bolt.zf <= 0.001f && m_bolt.roll <= 0.001f) {   // voll zu -> lock + chamber
                m_bolt.open = false;
                m_bolt.locked = true;
                m_bolt.roll = 0.0f;
                m_bolt.zf = 0.0f;
                m_bolt.troll = 0.0f;
                m_bolt.tzf = 0.0f;
                // [MANUAL_CYCLE] Bolt durchgezogen -> Feuer frei
                m_bolt.needs_cycle = false;
                bolt_chamber();
                haptic_left(0.85f, 0.06f);
                bsnd(BSND.bolt_close);
            }
        }

        // waehrend aktivem Zug folgt das Ziel der Hand; NICHT nach dem Lock
        // (sonst klobbert es das 1.0-Ziel)
        if (!m_bolt.locked) {
            m_bolt.troll = m_bolt.roll;
            m_bolt.tzf = m_bolt.zf;
        }

        return;
    }

    // Grip losgelassen
    m_bolt.grab = false;
    m_bolt.armed = true;

    if (m_bolt.locked) {
        m_bolt.locked = false;   // Transition bestaetigt, schon am Ziel
    } else {                     // unvollstaendig -> sanft zur Startlage zurueck
        if (m_bolt.mode == "open") {
            m_bolt.open = false;
            m_bolt.troll = 0.0f;
            m_bolt.tzf = 0.0f;
        } else {
            m_bolt.open = true;
            m_bolt.troll = 1.0f;
            m_bolt.tzf = 1.0f;
        }
    }
}

// ---- Bolt-Joint treiben (nur wenn aktiv -> die Engine-Auto-Cycle-Anim sonst in
// Ruhe lassen) ----
void RE4VRReload2::apply_bolt_joint() {
    auto* bj = m_bwep.bolt_joint;

    if (bj == nullptr) {
        return;
    }

    const bool active = m_bolt.grab || m_bolt.open || m_bolt.preview
                        || m_bcart.active || m_bcart.insert;

    // [MANUAL_CYCLE] needs_cycle (nach Schuss, noch nicht durchgezogen) -> _01
    // NICHT der Engine ueberlassen, sondern auf die Zu-Ruhe zwingen -> die native
    // PumpAction (Ghost-Bolt) ist unterdrueckt.
    if (!active && m_bolt.roll <= 0.001f && m_bolt.zf <= 0.001f && !m_bolt.needs_cycle) {
        return;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(4400));
    const float roll = m_bolt.preview ? (m_bolt.preview_open ? 1.0f : 0.0f) : m_bolt.roll;
    const float zf = m_bolt.preview ? (m_bolt.preview_open ? 1.0f : 0.0f) : m_bolt.zf;

    if (m_bwep.bolt_rest_rot.has_value()) {
        set_quat(bj, "set_LocalRotation",
                 glm::normalize(*m_bwep.bolt_rest_rot
                                * quat_from_euler(c.brx * roll, c.bry * roll, c.brz * roll)));
    }

    if (m_bwep.bolt_rest_lp.has_value()) {
        const glm::vec3 rp = *m_bwep.bolt_rest_lp;
        set_vec3(bj, "set_LocalPosition",
                 glm::vec3{rp.x, rp.y, rp.z + c.back_off * zf});
    }
}

// ---- Hand folgt dem Bolt (Dock-Publish; arm_chain liest die Globals) ----
void RE4VRReload2::update_bolt_dock(bool advance) {
    ::REManagedObject* src = nullptr;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    float rrx = 0.0f, rry = 0.0f, rrz = 0.0f;
    float want = 0.0f;

    if ((m_bolt.grab || m_bolt.preview) && m_bwep.bolt_joint != nullptr) {
        const auto& c = bcfg(m_bwep.wid.value_or(4400));
        src = m_bwep.bolt_joint;
        ox = c.dock_x;
        oy = c.dock_y;
        oz = c.dock_z;
        rrx = c.hrx;
        rry = c.hry;
        rrz = c.hrz;
        want = 1.0f;
    }

    float b = m_bolt.dock_blend;

    if (advance) {
        if (b < want) {
            b = std::min(b + BOLT_DOCK_BLEND_SPEED, want);
        } else if (b > want) {
            b = std::max(b - BOLT_DOCK_BLEND_SPEED, want);
        }

        m_bolt.dock_blend = b;
    }

    glm::vec3 p{};
    glm::quat r{};

    if (b > 0.001f && src != nullptr && get_vec3(src, "get_Position", p)
        && get_quat(src, "get_Rotation", r)) {
        if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
            p += r * glm::vec3{ox, oy, oz};
        }

        if (rrx != 0.0f || rry != 0.0f || rrz != 0.0f) {
            r = glm::normalize(r * quat_from_euler(rrx, rry, rrz));
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

// ---- Patrone in Hand (joint_10 folgt der linken Hand) ----
void RE4VRReload2::update_cart_in_hand() {
    // [KEYFRAME-PROGRAMM] Die reload_adv-Preview deckt nur
    // LateUpdate/BeginRendering ab -> die Engine setzt den leeren Cart-Joint auf
    // den anderen Paessen zurueck (unsichtbar). Deshalb HIER, im vollen
    // Bolt-Pass-Stack, den nativen Cart-Joint an die waffenrelative
    // Keyframe-Tuning-Lage + Scale 1 zwingen.
    const int32_t kfp = static_cast<int32_t>(
        re4vr::lua_get_number("__re4_shell_kf_preview", 0.0));

    if (m_bwep.wid.has_value() && kfp == *m_bwep.wid
        && m_bwep.cart_joint != nullptr && m_bwep.tf != nullptr && m_adv != nullptr) {
        const auto& s = m_adv->shell_live;
        glm::vec3 gp{};
        glm::quat gr{};

        if (get_vec3(m_bwep.tf, "get_Position", gp)
            && get_quat(m_bwep.tf, "get_Rotation", gr)) {
            set_vec3(m_bwep.cart_joint, "set_Position",
                     gp + (gr * glm::vec3{s.x, s.y, s.z}));
            set_quat(m_bwep.cart_joint, "set_Rotation",
                     glm::normalize(gr * quat_from_euler(s.rx, s.ry, s.rz)));
            set_vec3(m_bwep.cart_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});

            return;
        }
    }

    auto* cj = (m_bcart.active || m_bcart.tune) ? m_bwep.cart_joint : nullptr;

    if (cj == nullptr) {
        return;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(4400));
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
        w = hp + (hr * glm::vec3{c.cx, c.cy, c.cz});
    }

    set_vec3(cj, "set_Position", w);

    if (have_hr) {
        set_quat(cj, "set_Rotation",
                 glm::normalize(hr * quat_from_euler(c.crx, c.cry, c.crz)));
    }

    set_vec3(cj, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
}

// ---- Kammer-Dock-Welt (Einlege-Naehe) ----
std::optional<glm::vec3> RE4VRReload2::bolt_chamber_world() {
    if (m_bwep.tf == nullptr) {
        return std::nullopt;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(4400));
    auto* j = joint_by_name(m_bwep.tf, c.cd_joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{c.cd_x, c.cd_y, c.cd_z});
}

// +1 laden: write_dword(0x44)/addAmmoCount werden bei wp4400 von der Engine
// RE-SYNCT (Item-Feld ging 7->8 direkt nach dem Write, getCurrentGunAmmo blieb 7
// -> einen Frame spaeter zurueckgesetzt, das HUD lud nie). Einziger Pfad, der
// WIRKLICH laedt = der Engine-Reload, additiv +1. Die Reserve zieht die Engine
// selbst -> KEIN 0x44, KEIN manueller Abzug. Der Engine-Reload ist VERZOEGERT --
// getCurrentGunAmmo direkt danach kann noch alt sein.
void RE4VRReload2::bolt_add_one() {
    if (!m_bscalar.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return;
    }

    auto* wi = bget_wi();
    const int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(0);
    const int32_t cap = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;

    if (cap > 0 && loaded >= cap) {
        return;
    }

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    const int32_t reserve = ammo_id.has_value() ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return;
    }

    std::optional<int32_t> et{};

    if (auto* td = sdk::find_type_definition("chainsaw.EquipType"); td != nullptr) {
        if (auto* f = td->get_field("Main"); f != nullptr) {
            et = f->get_data<int32_t>(nullptr);
        }
    }

    if (et.has_value()) {
        // [CRASH-HARDEN] enableReloadItem-Gate gegen den null-Item-AV
        m_main->load_and_book(inv, *et, 1, false);
    }
}

// ---- Patrone-Einlegen (joint_10 von der Hand-Lokalpose zurueck in die Kammer)
bool RE4VRReload2::start_cart_insert() {
    auto* cj = m_bwep.cart_joint;

    if (cj == nullptr || !m_bwep.cart_rest_lp.has_value()) {
        return false;
    }

    glm::vec3 lp{};
    glm::quat lr{};
    m_bcart.slp = get_vec3(cj, "get_LocalPosition", lp)
        ? std::optional<glm::vec3>{lp} : std::nullopt;
    m_bcart.slr = get_quat(cj, "get_LocalRotation", lr)
        ? std::optional<glm::quat>{lr} : std::nullopt;
    m_bcart.t0 = clock_now();
    m_bcart.insert = true;
    m_bcart.active = false;
    m_bcart.snd = false;

    return true;
}

void RE4VRReload2::bolt_check_insert_proximity() {
    if (!m_bcart.active) {
        return;
    }

    const auto hp = left_hand_world_g();
    const auto gp = bolt_chamber_world();

    if (!hp.has_value() || !gp.has_value()) {
        return;
    }

    if (vec_len(vec_sub(*hp, *gp)) <= m_bscalar.insert_distance) {
        start_cart_insert();
    }
}

void RE4VRReload2::update_cart_insert() {
    if (!(m_bcart.insert && m_bwep.cart_joint != nullptr && m_bcart.slp.has_value()
          && m_bwep.cart_rest_lp.has_value())) {
        return;
    }

    // [KEYFRAME-PROGRAMM] Hat wp4400 eine Keyframe-Bahn (reload_adv)? Dann den
    // nativen Cart-Joint entlang der geordneten Bahn fahren (waffenrelativ) --
    // der lineare Slide unten gilt dann NICHT. Eigene Bahn-Dauer.
    const int32_t wid = m_bwep.wid.value_or(0);
    const bool kf = m_adv != nullptr && m_adv->has_shell_keys(wid) && m_bwep.tf != nullptr;
    const float dur = kf ? m_adv->shell_dur : m_bcart.dur;
    float t = static_cast<float>(clock_now() - m_bcart.t0) / std::max(dur, 0.01f);

    if (t > 1.0f) {
        t = 1.0f;
    }

    if (!m_bcart.snd && t >= 0.6f) {
        m_bcart.snd = true;
        bsnd(BSND.insert);
    }

    if (kf) {
        m_adv->apply_shell_keys(m_bwep.tf, m_bwep.cart_joint, wid, t);
        set_vec3(m_bwep.cart_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});

        if (t >= 1.0f) {
            m_bcart.insert = false;
            bolt_add_one();
        }

        return;
    }

    const float u = ease(t);
    const glm::vec3 a = *m_bcart.slp;
    const glm::vec3 b2 = *m_bwep.cart_rest_lp;
    set_vec3(m_bwep.cart_joint, "set_LocalPosition", a + (b2 - a) * u);

    if (m_bcart.slr.has_value() && m_bwep.cart_rest_lr.has_value()) {
        const glm::quat s = *m_bcart.slr;
        glm::quat r = *m_bwep.cart_rest_lr;

        if (glm::dot(s, r) < 0.0f) {
            r = glm::quat{-r.w, -r.x, -r.y, -r.z};
        }

        const glm::quat q{s.w + (r.w - s.w) * u, s.x + (r.x - s.x) * u,
                          s.y + (r.y - s.y) * u, s.z + (r.z - s.z) * u};
        const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);

        if (len > 1e-6f) {
            set_quat(m_bwep.cart_joint, "set_LocalRotation",
                     glm::quat{q.w / len, q.x / len, q.y / len, q.z / len});
        }
    }

    if (t >= 1.0f) {
        m_bcart.insert = false;
        bolt_add_one();
    }
}

// ---- Holster-Grab -> Patrone in die linke Hand (nur bei offenem Bolt + ladbar)
bool RE4VRReload2::bolt_can_load() {
    if (m_bcart.active || m_bcart.insert) {
        return false;
    }

    if (!m_bolt.open) {
        return false;
    }

    return bloaded().value_or(0) < bcap() && breserve() > 0;
}

bool RE4VRReload2::bolt_set_cart_in_hand(bool active) {
    if (active) {
        if (!bolt_can_load()) {
            return false;
        }

        m_bcart.active = true;
        bsnd(BSND.cart_grab);

        return true;
    }

    if (m_bcart.active) {
        m_bcart.active = false;
        // aus der Hand gelassen ohne Einstecken -> faellt zu Boden
        bsnd(BSND.cart_floor);

        // geliehene Patrone zurueck in die Kammer-Ruhe (kein Drop, Einzelpatrone)
        if (m_bwep.cart_joint != nullptr && m_bwep.cart_rest_lp.has_value()) {
            set_vec3(m_bwep.cart_joint, "set_LocalPosition", *m_bwep.cart_rest_lp);

            if (m_bwep.cart_rest_lr.has_value()) {
                set_quat(m_bwep.cart_joint, "set_LocalRotation", *m_bwep.cart_rest_lr);
            }
        }
    }

    return true;
}

// ---- Hand-Pose (Default RiotSLide / Bolt TMPSUpport / Patrone Shotgunshell) --
void RE4VRReload2::apply_bolt_pose() {
    std::string name = "RiotSLide";
    const BoltCfg* thumb = nullptr;

    if (m_bcart.active || m_bcart.insert || m_bcart.tune) {
        name = "Shotgunshell";
        thumb = &bcfg(m_bwep.wid.value_or(4400));
    } else if (m_bolt.grab || m_bolt.preview) {
        name = "TMPSUpport";
    }

    if (const auto it = m_bposes.find(name); it != m_bposes.end()) {
        // Der Bolt-Block blendet NICHT (kein POSE_FADE) -- Blend fest 1.0.
        pose_apply(it->second, 1.0f);
    }

    if (thumb != nullptr
        && (thumb->t_rx != 0.0f || thumb->t_ry != 0.0f || thumb->t_rz != 0.0f)) {
        auto* bt = body_tf();
        auto* tj = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (tj != nullptr && get_quat(tj, "get_LocalRotation", cur)) {
            set_quat(tj, "set_LocalRotation",
                     glm::normalize(cur * quat_from_euler(thumb->t_rx, thumb->t_ry,
                                                          thumb->t_rz)));
        }
    }
}

void RE4VRReload2::bolt_soft_reset() {
    m_bolt.open = false;
    m_bolt.grab = false;
    m_bolt.armed = false;
    m_bolt.mode = "open";
    // [MANUAL_CYCLE] Cycle-Pflicht + Schuss-Tracker aus
    m_bolt.needs_cycle = false;
    m_bolt._prev_loaded.reset();
    m_bolt.roll = 0.0f;
    m_bolt.zf = 0.0f;
    m_bolt.zf_anchor = 0.0f;
    m_bolt.dock_blend = 0.0f;
    m_bolt.preview = false;
    m_bolt.preview_open = false;
    m_bcart.active = false;
    m_bcart.insert = false;
    m_bcart.tune = false;
}

// [CYCLE-SUPPRESS (A)] Der Cycle-Node wird per set_Frame(End) + set_Speed(100)
// sofort ans Ende gespult -> der Bolt bewegt sich nicht sichtbar.
//
// Zwei frueher eingebaute Luecken sind hier bewusst geschlossen: es werden die
// Layer 0..7 durchgesehen (die Schuss-Nachanimation liegt nicht zwingend auf
// Layer 0), und das Erkennungs-Flag laeuft kurz nach, weil
// get_HighestWeightMotionNode waehrend eines Blends kurz eine ANDERE Anim
// liefert. Gespult wird weiterhin NUR bei exakt passendem Node-Namen.
//
// (B) -- das Ueberspringen von chainsaw.Gun.callbackTracks -- ist in Lua seit
// 2026-08-31 durch ein `do return end` am Anfang des Pre-Hooks stillgelegt: der
// Hook wird zwar noch installiert, tut aber nichts. Er ist deshalb hier NICHT
// nachgebaut; das Verhalten ist identisch.
void RE4VRReload2::bolt_cycle_suppress() {
    const double now = clock_now();
    // Das Flag NICHT hart nullen, sondern auslaufen lassen: callbackTracks feuert
    // im Engine-Update, dieses on_frame kann danach liegen.
    re4vr::lua_set_bool("__re4_bolt_in_cycle", now < m_bolt_in_cycle_until);

    if (!m_bwep.wid.has_value()) {
        m_bolt_in_cycle_until = 0.0;
        re4vr::lua_set_bool("__re4_bolt_in_cycle", false);

        return;
    }

    auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");
    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");
    auto* gun = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
    auto* go = re4vr::call_safe<::REManagedObject*>(gun, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    auto* mc = re4vr::get_component(go, "via.motion.Motion");

    if (mc == nullptr) {
        return;
    }

    for (int32_t li = 0; li <= 7; ++li) {
        auto* layer = re4vr::call_safe<::REManagedObject*>(mc, "getLayer", li);

        if (layer == nullptr) {
            continue;
        }

        auto* node = re4vr::call_safe<::REManagedObject*>(
            layer, "get_HighestWeightMotionNode");

        if (node == nullptr) {
            continue;
        }

        auto* nm = re4vr::call_safe<::REManagedObject*>(node, "get_MotionName");

        if (nm == nullptr) {
            continue;
        }

        if (utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm))
            != BOLT_CYCLE_NODE) {
            continue;
        }

        re4vr::lua_set_bool("__re4_bolt_in_cycle", true);
        m_bolt_in_cycle_until = now + BOLT_IN_CYCLE_HOLD;
        float ef = 0.0f;

        if (re4vr::try_call<float>(node, "get_EndFrame", ef) && ef > 0.0f) {
            re4vr::call_safe<void*>(node, "set_Frame", ef);
        }

        re4vr::call_safe<void*>(layer, "set_Speed", 100.0f);

        break;   // der Node laeuft genau einmal
    }
}

void RE4VRReload2::bolt_on_frame() {
    bolt_refresh();
    // [SOUND_PROBE] Gate fuer den Sound-Hook (nur wenn wp4400 equippt)
    re4vr::lua_set_bool("__re4_bolt_equipped", m_bwep.wid.has_value());

    if (m_bwep.wid != m_bolt_prev_wid) {
        const bool was_bolt = m_bolt_prev_wid.has_value();
        bolt_soft_reset();

        if (was_bolt && !m_bwep.wid.has_value()) {
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
            re4vr::lua_set_nil("__vr_rack_hand_pose");
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
            re4vr::lua_set_bool("__vr_needs_rack", false);
            re4vr::lua_set_bool("__vr_block_aim", false);
        }

        if (m_bwep.wid.has_value()) {
            re4vr::lua_set_nil("__re4_live_wi");
        }

        m_bolt_prev_wid = m_bwep.wid;
    }

    // [CYCLE-SUPPRESS] laeuft in Lua als EIGENER on_frame -- er prueft bwep.wid
    // selbst und muss deshalb auch dann laufen, wenn hier gleich abgebrochen wird.
    bolt_cycle_suppress();

    if (!m_bwep.wid.has_value()) {
        return;
    }

    const int32_t wid = *m_bwep.wid;

    // [KEYFRAME-PROGRAMM] Cart-Joint (_10) + Waffen-Transform + wid fuer die
    // reload_adv-Keyframe-UI/Preview exponieren. Nur bei equippter Bolt-Waffe ->
    // kein Clobber sonst.
    re4vr::lua_set_managed_object("__re4_reload_shell_joint", m_bwep.cart_joint);
    re4vr::lua_set_managed_object("__re4_reload_weapon_tf", m_bwep.tf);
    re4vr::lua_set_number("__re4_reload_ui_wid", wid);

    // [KEYFRAME-PROGRAMM] Der reload_adv-Preview-Toggle soll GENAU das tun wie
    // der Holster-Grab: das Mesh in die Hand holen (tune = wie active, nur ohne
    // Ammo-Logik). Nur WIR verwalten dieses erzwungene tune (_kf_forced) -> die
    // manuelle Vorschau-Checkbox bleibt unberuehrt.
    if (static_cast<int32_t>(re4vr::lua_get_number("__re4_shell_kf_preview", 0.0)) == wid) {
        m_bcart.tune = true;
        m_bcart._kf_forced = true;
    } else if (m_bcart._kf_forced) {
        m_bcart.tune = false;
        m_bcart._kf_forced = false;
    }

    if (m_bolt_reacquired) {
        m_bolt_reacquired = false;
        re4vr::lua_set_nil("__re4_live_wi");
        bolt_soft_reset();
    }

    bolt_capture_cart_rest();
    bolt_check_insert_proximity();

    // Native Reload abfangen (manueller Bolt statt Engine-Reload)
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);

    update_bolt_gesture();

    // Holster-Gate: buzzen nur, wenn NICHTS zu laden geht
    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        !(m_bolt.open && breserve() > 0 && bloaded().value_or(0) < bcap()));

    // Globals fuer motion/arm_chain/binding
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_slide_rack_active", m_bolt.grab);
    re4vr::lua_set_bool("__vr_mag_in_hand", m_bcart.active || m_bcart.insert);

    if (m_bolt.grab) {
        re4vr::lua_set_string("__vr_rack_hand_pose", "TMPSUpport");
    } else {
        re4vr::lua_set_nil("__vr_rack_hand_pose");
    }

    // [BOLT_IDLE_POSE] Gate fuer die Ruhepose der RECHTEN Hand in motion. Bewusst
    // ein ZEITSTEMPEL, kein true/false: dieser Block laeuft nur bei equippter
    // Bolt-Waffe, also altert der Wert von selbst bei Waffenwechsel, Save-Load,
    // bare hands oder totem Script -> die Pose kann nicht haengen.
    re4vr::lua_set_number("__re4_bolt_wid_active", clock_now());

    // [BOLT_MUTE] Adresse des Waffen-GO fuer den trigger-Hook (dort ist kein
    // Suchen erlaubt -- der Hook feuert fuer JEDEN SoundContainer im Spiel).
    auto* bgo = re4vr::call_safe<::REManagedObject*>(m_bwep.tf, "get_GameObject");
    m_bolt_gun_addr = reinterpret_cast<uintptr_t>(bgo);

    if (bgo != nullptr) {
        re4vr::lua_set_number("__re4_bolt_gun_addr",
                              static_cast<double>(m_bolt_gun_addr));
    } else {
        re4vr::lua_set_nil("__re4_bolt_gun_addr");
    }

    // [MANUAL_CYCLE] Schuss erkennen (getCurrentGunAmmo gesunken) ->
    // Cycle-Pflicht: bis der Bolt einmal manuell auf+zu ist, bleibt Feuern
    // gesperrt UND apply_bolt_joint zwingt _01 auf die Zu-Ruhe (native
    // PumpAction/Ghost-Bolt unterdrueckt). Reload und offener Bolt sind
    // ausgenommen -> keine Falsch-Erkennung.
    const auto bl = call_enum(get_pe(), "getCurrentGunAmmo");

    if (bl.has_value() && m_bolt._prev_loaded.has_value() && *bl < *m_bolt._prev_loaded
        && !m_bolt.open && !m_bcart.insert) {
        m_bolt.needs_cycle = true;
        // [SOUND_PROBE] Schuss-Zeitpunkt fuer das Post-Schuss-Fenster
        re4vr::lua_set_number("__re4_bolt_shot_t", clock_now());
        // [BOLT_MUTE] Nach JEDEM Schuss 1 s lang den Waffen-SoundContainer stumm
        // schalten: der native Reload-Sound gehoert zu einer Anim, die wir gar
        // nicht mehr spielen.
        const double until = clock_now()
            + re4vr::lua_get_number("__re4_bolt_mute_dur", 1.0);
        m_bolt_mute_until = until;
        re4vr::lua_set_number("__re4_bolt_mute_until", until);
    }

    if (bl.has_value()) {
        m_bolt._prev_loaded = bl;
    }

    // [MESH FORCE] In Lua wird hier __re4_force_weapon_visible gerufen -- der
    // Helfer ist dort seit 2026-08-15 mit einem `do return end` komplett
    // stillgelegt (er nahm beim Bolt-Griff die VR-Sitzung mit). Deshalb hier
    // nichts; das Verhalten ist identisch.

    // [KRITISCHES GATE -- LIVE] Feuer-Block: offener Bolt + echter Leerstand +
    // Cycle-Pflicht nach dem Schuss.
    const bool ammo_empty = rf_gun_ammo_empty();
    re4vr::lua_set_bool("__vr_block_fire_when_empty",
                        m_bolt.open || ammo_empty || m_bolt.needs_cycle);
    // [BF-DIAG] Nur Anzeige fuer die Feuer-Messung: welcher der drei Gruende blockt?
    {
        char buf[96]{};
        std::snprintf(buf, sizeof(buf), "open=%s leer=%s cycle=%s",
                      m_bolt.open ? "true" : "false",
                      ammo_empty ? "true" : "false",
                      m_bolt.needs_cycle ? "true" : "false");
        re4vr::lua_set_string("__re4_bolt_bf", buf);
    }

    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:2998");
    // [COCK_MUTE] Fenster fuer den native-Auto-Cock-Sound-Blocker
    re4vr::lua_set_bool("__re4_bolt_suppress_cock", m_bolt.needs_cycle);
    re4vr::lua_set_bool("__vr_rack_block_left_knife", m_bolt.open);
    // offener Bolt -> Aimen gesperrt (binding liest)
    re4vr::lua_set_bool("__vr_block_aim", m_bolt.open);

    update_bolt_dock(true);

    // Dry-Fire bei gesperrtem Trigger
    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_bolt_dry_prev) {
        bsnd(BSND.dry_fire);
    }

    m_bolt_dry_prev = et;
}

void RE4VRReload2::bolt_apply_pass() {
    if (!(m_bscalar.bolt_enabled && m_bwep.wid.has_value())) {
        return;
    }

    // [KEYFRAME-PROGRAMM] ui_wid/shell_joint/weapon_tf AUCH auf den
    // App-Entry-Paessen publizieren (nicht nur im on_frame). Sonst ueberschreibt
    // reload sie hier mit nil (kennt 4400 nicht), BEVOR reload_advs
    // shell_preview_apply sie liest -> der Preview zuendete nie.
    re4vr::lua_set_managed_object("__re4_reload_shell_joint", m_bwep.cart_joint);
    re4vr::lua_set_managed_object("__re4_reload_weapon_tf", m_bwep.tf);
    re4vr::lua_set_number("__re4_reload_ui_wid", *m_bwep.wid);

    update_cart_in_hand();
    update_cart_insert();
    apply_bolt_joint();
    update_bolt_dock(false);
    apply_bolt_pose();
}

void RE4VRReload2::bolt_on_script_reset() {
    if (m_bwep.bolt_joint != nullptr) {
        if (m_bwep.bolt_rest_rot.has_value()) {
            set_quat(m_bwep.bolt_joint, "set_LocalRotation", *m_bwep.bolt_rest_rot);
        }

        if (m_bwep.bolt_rest_lp.has_value()) {
            set_vec3(m_bwep.bolt_joint, "set_LocalPosition", *m_bwep.bolt_rest_lp);
        }
    }

    if (m_bwep.cart_joint != nullptr && m_bwep.cart_rest_lp.has_value()) {
        set_vec3(m_bwep.cart_joint, "set_LocalPosition", *m_bwep.cart_rest_lp);

        if (m_bwep.cart_rest_lr.has_value()) {
            set_quat(m_bwep.cart_joint, "set_LocalRotation", *m_bwep.cart_rest_lr);
        }
    }

    m_bwep.wid.reset();
    m_bwep.tf = nullptr;
    m_bwep.bolt_joint = nullptr;
    m_bwep.cart_joint = nullptr;
    m_bwep.bolt_rest_rot.reset();
    bolt_soft_reset();
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    re4vr::lua_set_nil("__vr_rack_hand_pose");
    re4vr::lua_set_bool("__vr_slide_rack_active", false);
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_block_aim", false);
}

// ---------------------------------------------------------------------
// UI -- Bolt-Action
// ---------------------------------------------------------------------
void RE4VRReload2::bolt_ui() {
    if (ImGui::Checkbox("##bolt_en", &m_bscalar.bolt_enabled)) {
        bolt_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Bolt-Action Reload (SR M1903)");

    if (!ImGui::TreeNode("Bolt-Action -- Einstellungen")) {
        return;
    }

    const auto awid = m_bwep.wid.has_value() ? m_bwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_bolt(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Bolt-Action erkannt - verwaltet)"
                             : "  (keine eingerichtete Bolt-Waffe equippt)");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Joints: Bolt=%s  Patrone=%s  (bolt_joint=%s cart_joint=%s)",
                       known ? BOLT_J_BOLT : "nil", known ? BOLT_J_CART : "nil",
                       m_bwep.bolt_joint != nullptr ? "ok" : "nil",
                       m_bwep.cart_joint != nullptr ? "ok" : "nil");

    const auto ld = bloaded();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  loaded=%s cap=%d reserve=%d bolt_open=%s cart=%s empty=%s",
                       ld.has_value() ? std::to_string(*ld).c_str() : "nil",
                       bcap(), breserve(),
                       m_bolt.open ? "true" : "false",
                       (m_bcart.active || m_bcart.insert) ? "true" : "false",
                       rf_gun_ammo_empty() ? "true" : "false");

    if (ImGui::SliderFloat("Einlege-Distanz m (Patrone)##bdist",
                           &m_bscalar.insert_distance, 0.03f, 0.50f)) {
        bolt_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen (+1)", &m_bscalar.reload_ammo)) {
        bolt_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an", &m_bscalar.sound_enabled)) {
        bolt_save_cfg();
    }

    auto& cc = bcfg(awid.value_or(4400));

    if (ImGui::TreeNode("Bolt-Bewegung (Roll + Z + Hand-Dock)")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Bolt zwingen##bprev", &m_bolt.preview);

        if (m_bolt.preview) {
            ImGui::Checkbox("offen (an) / zu (aus)##bprevo", &m_bolt.preview_open);
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Roll (joint_01 Dreh bei voll offen, Grad):");
        ch |= ImGui::SliderFloat("Roll RotX##bbrx", &cc.brx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Roll RotY##bbry", &cc.bry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Roll RotZ##bbrz", &cc.brz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Roll-Geschw. (lerp)##bol", &cc.open_lerp, 0.02f, 1.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Z (Bolt zurueck):");
        ch |= ImGui::SliderFloat("Z-Versatz zurueck (m)##bbo", &cc.back_off, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Hand-Pull-Distanz (m)##btv", &cc.travel, 0.03f, 0.40f);
        ch |= ImGui::SliderFloat("Greif-Distanz##bgd", &cc.grab_dist, 0.05f, 0.30f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hand-Dock am Bolt (Pos/Rot relativ zu _01):");
        ch |= ImGui::SliderFloat("dock_x##bdx", &cc.dock_x, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_y##bdy", &cc.dock_y, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("dock_z##bdz", &cc.dock_z, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Hand RotX##bhrx", &cc.hrx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotY##bhry", &cc.hry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotZ##bhrz", &cc.hrz, -180.0f, 180.0f);

        if (ch) {
            bolt_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Patrone in Hand (joint_10 Offset + Daumen)")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Patrone in der Hand zwingen##bcttune", &m_bcart.tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (erzwingt Patrone+Pose dauerhaft in der linken Hand)");
        ch |= ImGui::SliderFloat("PosX##bcx", &cc.cx, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosY##bcy", &cc.cy, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("PosZ##bcz", &cc.cz, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("RotX##bcrx", &cc.crx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotY##bcry", &cc.cry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("RotZ##bcrz", &cc.crz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Daumen RotX##bctx", &cc.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##bcty", &cc.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##bctz", &cc.t_rz, -90.0f, 90.0f);

        if (ch) {
            bolt_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Kammer-Dock (Einlegepunkt)")) {
        bool ch = false;
        char jb[64]{};
        std::snprintf(jb, sizeof(jb), "%s", cc.cd_joint.c_str());

        if (ImGui::InputText("Dock-Joint##bcdj", jb, sizeof(jb))) {
            cc.cd_joint = jb;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Dock X##bcdx", &cc.cd_x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Dock Y##bcdy", &cc.cd_y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Dock Z##bcdz", &cc.cd_z, -0.20f, 0.20f);

        if (ch) {
            bolt_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Bolt: Hand an _01 + Grip -> roll, dann Z zurueck (offen). "
                       "Holster -> Patrone (Shotgunshell) -> Kammer = +1. Zu: Z vor, "
                       "dann roll.");
    ImGui::TreePop();
}

// ============================================================================
// 4 -- ARMBRUST (wp4600, Lua Z.3930-4631)
//
// Der Pfeil ist ein Mesh-PART, kein Joint -- und der Mesh-Part-Trick wurde
// verworfen (ISOLATE blendete die ganze Armbrust aus). Der Hand-Bolzen ist
// stattdessen ein eigener DUMMY (chainsaw.ShellDummyBase ueber den
// ArrowShellGenerator der Waffe): er rendert mit Skelett und schiesst nicht.
// Abgefangen wird er ueber einen persistenten Hook auf ShellDummyBase.requestStart.
//
// Right-B macht NICHTS. Reload/Holster IMMER moeglich, solange Reserve > 0.
// Feuern bleibt NATIV. Eigenes JSON: re4_vr/re4_vr_reload2_xbow.json
// ============================================================================
namespace {

constexpr const char* XBOW_CFG_PATH = "re4_vr/re4_vr_reload2_xbow.json";
constexpr float XDROP_DUR = 1.8f;
constexpr float XGRAV = 4.5f;
// s (~2-3 Frames); zur Laufzeit ueber __re4_xbow_kf_hold drehbar
constexpr double XKF_HOLD_DUR = 0.035;

// [SOUND] Armbrust-IDs
constexpr uint32_t XSND_GRAB = 3511992014u;    // Pfeil aus dem Ammo-Holster
constexpr uint32_t XSND_DROP = 288425943u;     // losgelassen ohne Einlegen
constexpr uint32_t XSND_INSERT = 37656823u;    // echtes +1

}   // namespace

bool RE4VRReload2::is_xbow(int32_t wid) {
    return wid == 4600;
}

RE4VRReload2::XbowCfg& RE4VRReload2::xcfg(int32_t wid) {
    auto it = m_xcfg.find(wid);

    if (it == m_xcfg.end()) {
        it = m_xcfg.emplace(wid, XbowCfg{}).first;
    }

    return it->second;
}

void RE4VRReload2::xbow_load_cfg() {
    const auto data = re4vr::json_load(XBOW_CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    if (const auto it = data.find("cfg"); it != data.end() && it->is_object()) {
        m_xscalar.enabled = jbool(*it, "enabled", m_xscalar.enabled);
        m_xscalar.reload_ammo = jbool(*it, "reload_ammo", m_xscalar.reload_ammo);
        m_xscalar.insert_distance = jnum(*it, "insert_distance", m_xscalar.insert_distance);
        m_xscalar.hand_parts = jstr(*it, "hand_parts", m_xscalar.hand_parts);
    }

    if (const auto it = data.find("tune"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& c = xcfg(wid);
            c.t_rx = jnum(v, "t_rx", c.t_rx);
            c.t_ry = jnum(v, "t_ry", c.t_ry);
            c.t_rz = jnum(v, "t_rz", c.t_rz);
            c.cd_x = jnum(v, "cd_x", c.cd_x);
            c.cd_y = jnum(v, "cd_y", c.cd_y);
            c.cd_z = jnum(v, "cd_z", c.cd_z);
            c.dx = jnum(v, "dx", c.dx);
            c.dy = jnum(v, "dy", c.dy);
            c.dz = jnum(v, "dz", c.dz);
            c.drx = jnum(v, "drx", c.drx);
            c.dry = jnum(v, "dry", c.dry);
            c.drz = jnum(v, "drz", c.drz);
            c.dscale = jnum(v, "dscale", c.dscale);
            c.cd_joint = jstr(v, "cd_joint", c.cd_joint);
        }
    }
}

void RE4VRReload2::xbow_save_cfg() {
    const auto& c = xcfg(4600);
    nlohmann::json tune = nlohmann::json::object();
    tune["4600"] = {
        {"t_rx", c.t_rx}, {"t_ry", c.t_ry}, {"t_rz", c.t_rz},
        {"cd_x", c.cd_x}, {"cd_y", c.cd_y}, {"cd_z", c.cd_z},
        {"dx", c.dx}, {"dy", c.dy}, {"dz", c.dz},
        {"drx", c.drx}, {"dry", c.dry}, {"drz", c.drz},
        {"dscale", c.dscale}, {"cd_joint", c.cd_joint},
    };

    nlohmann::json d = {
        {"cfg", {{"enabled", m_xscalar.enabled},
                 {"reload_ammo", m_xscalar.reload_ammo},
                 {"insert_distance", m_xscalar.insert_distance},
                 {"hand_parts", m_xscalar.hand_parts}}},
        {"tune", tune},
    };

    re4vr::json_save(XBOW_CFG_PATH, d);
}

// ---- Gun / Transform ----
// EIGENER wid-Check: NUR die Armbrust wp4600 anfassen, NIE andere Waffen.
::REManagedObject* RE4VRReload2::xget_gun() {
    auto* ctx = get_ctx();
    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return nullptr;
    }

    if (call_enum(hu, "get_EquipWeaponID").value_or(0) != 4600) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
}

::REManagedObject* RE4VRReload2::xget_gun_tf() {
    auto* g = xget_gun();
    auto* go = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");

    return re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
}

// [SOUND] EIGENE Sound-Funktion: die Revolver-Variante haengt an wep.tf und ist
// fuer die Armbrust nil.
void RE4VRReload2::xplay_sound(uint32_t id) {
    if (id == 0) {
        return;
    }

    auto* g = xget_gun();
    auto* go = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");

    if (go != nullptr) {
        trigger_sound(go, id);
    }
}

// ---- Ammo (gegen die EQUIPPTE Waffe) ----
::REManagedObject* RE4VRReload2::xget_wi() {
    auto* ewi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    if (ewi != nullptr && call_enum(ewi, "get_CurrentAmmoCount").has_value()) {
        return ewi;
    }

    return nullptr;
}

std::optional<int32_t> RE4VRReload2::xloaded() {
    auto* wi = xget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
}

int32_t RE4VRReload2::xcap() {
    auto* wi = xget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload2::xreserve() {
    auto* wi = xget_wi();

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

// ==== HAND-DUMMY-BOLZEN (chainsaw.ShellDummyBase via ArrowShellGenerator) ====
// requestGenerateDummy am Generator der Armbrust -> visueller Bolzen (eigenes GO
// mit Skelett). Abgefangen per Hook auf ShellDummyBase.requestStart.
::REManagedObject* RE4VRReload2::xfind_child(::REManagedObject* go, const char* name) {
    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
    auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

    while (ch != nullptr) {
        auto* cgo = re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject");

        if (cgo != nullptr && obj_name(cgo) == name) {
            return cgo;
        }

        ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
    }

    return nullptr;
}

::REManagedObject* RE4VRReload2::xget_generator() {
    auto* g = xget_gun();
    auto* go = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");

    if (go == nullptr) {
        return nullptr;
    }

    auto* agg = xfind_child(go, "ArrowShellGenerator");

    return (agg != nullptr) ? re4vr::get_component(agg, "chainsaw.ArrowShellGenerator")
                            : nullptr;
}

void RE4VRReload2::xrequest_dummy() {
    auto* gen = xget_generator();

    if (gen == nullptr) {
        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{0.0f, 0.0f, 0.0f};

    if (lh != nullptr) {
        get_vec3(lh, "get_Position", hp);
    }

    m_xdummy_await = true;
    const Vector4f pos{hp.x, hp.y, hp.z, 1.0f};
    const glm::quat ident{1.0f, 0.0f, 0.0f, 0.0f};
    re4vr::call_safe<void*>(gen, "requestGenerateDummy", &pos, &ident, nullptr);
}

// [NO_LAG XBOW] Der Hand-Bolzen hing der Hand beim Laufen hinterher -- dasselbe
// Symptom wie damals die Mag-/Trommel-Klone. Der Fix dort war NICHT mehr Paesse,
// sondern das NATIVE PARENTEN ans L_Hand-Joint: die Engine propagiert die
// Transform des Kindes VOR dem Skinning -> kein Render-Versatz. Danach wird nur
// noch die LOKALE Pose gesetzt; die Offsets waren ohnehin schon hand-relativ
// gerechnet und gelten damit 1:1 als lokale Werte.
// ENTKOPPELT wird ueberall dort, wo die Pose NICHT hand-relativ ist: Drop (freier
// Fall in Weltkoordinaten), Keyframe-Bahn (waffenrelativ) und beim Beenden.
// RUECKBAU: __re4_xbow_dummy_nolag = false -> exakt das alte Welt-Setzen.
bool RE4VRReload2::xdummy_set_parent(::REManagedObject* dummy, bool on) {
    auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

    if (tf == nullptr) {
        return false;
    }

    if (on) {
        if (m_xdummy_parented) {
            return true;
        }

        if (re4vr::lua_get_tribool("__re4_xbow_dummy_nolag") == 0) {
            return false;
        }

        auto* bt = body_tf();

        if (bt == nullptr) {
            return false;
        }

        re4vr::call_safe<void*>(tf, "set_Parent", bt);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(tf, "set_ParentJoint", jn);
            m_xdummy_parented = true;

            return true;
        }

        // halb geparentet nicht stehen lassen
        re4vr::call_safe<void*>(tf, "set_Parent", nullptr);

        return false;
    }

    if (m_xdummy_parented) {
        re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
    }

    m_xdummy_parented = false;

    return false;
}

// show=true: Dummy an L_Hand+Offset (+Scale), Lebenszeit halten.
// show=false: laeuft ein Drop -> frei fallen + drehen lassen; sonst beenden.
void RE4VRReload2::xupdate_dummy(bool show) {
    auto* dummy = m_xdummy_obj;

    // [PORTFIX 2026-09-06] typrichtig lesen -- der Lua-Weg
    // (`dummy:call("get_Valid") == true`, reload2.lua Z.4185). Vorher
    // try_call<bool>: geratene Signatur, geratene Rueckgabebreite. Liefert das
    // false, obwohl der Dummy lebt, wird der `if (alive)`-Zweig unten
    // uebersprungen -- also KEIN requestEnd, KEIN setLifeTime(0) -> der
    // Bolzen bleibt nach dem Nachladen in der Luft stehen. Genau so gemeldet.
    const bool alive =
        dummy != nullptr && re4vr::call_num(dummy, "get_Valid").value_or(0.0) != 0.0;

    if (!show) {
        if (m_xdrop.active && alive) {
            // [NO_LAG XBOW] Der Fall laeuft in WELT-Koordinaten
            xdummy_set_parent(dummy, false);
            const float t = static_cast<float>(clock_now() - m_xdrop.t0);

            if (t <= XDROP_DUR) {
                // waehrend des Falls am Leben halten
                {
                    std::array<void*, 1> a{re4vr::arg_num(999.0)};
                    re4vr::call_cmd(dummy, "setLifeTime", std::span<void*>(a));
                }
                // v0 = 0.7 m/s -> sofort gerade, aber gemaechlich
                const float fall = 0.7f * t + 0.5f * XGRAV * t * t;
                const float s = xcfg(4600).dscale;
                auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

                if (tf != nullptr) {
                    set_vec3(tf, "set_Position",
                             glm::vec3{m_xdrop.sx, m_xdrop.sy - fall, m_xdrop.sz});
                    // sanftes Taumeln, eine Achse
                    set_quat(tf, "set_Rotation",
                             glm::normalize(quat_from_euler(t * 220.0f, 0.0f, 0.0f)));
                    set_vec3(tf, "set_LocalScale", glm::vec3{s, s, s});
                }

                return;
            }

            m_xdrop.active = false;   // Fall ausgelaufen -> beenden
        }

        if (alive) {
            // [NO_LAG XBOW] vor dem Beenden vom Joint loesen
            xdummy_set_parent(dummy, false);
            // sauber beenden (sonst schwebt er weiter)
            // [PORTFIX 2026-09-06] ueber den LUA-WEG rufen. Roh gerufen war
            // requestEnd nachweislich WIRKUNGSLOS: get_Valid stand danach
            // weiter auf 1 (re4_xdummy_diag.txt), der Bolzen blieb in der Luft
            // und der Port verlor ihn direkt danach mit m_xdummy_obj=nullptr.
            re4vr::call_cmd(dummy, "requestEnd");
            {
                std::array<void*, 1> a{re4vr::arg_num(0.0)};
                re4vr::call_cmd(dummy, "setLifeTime", std::span<void*>(a));
            }
        }

        // [DUMMY-DIAG AUSGEBAUT 2026-09-08] Der Mitschnitt nach
        // re4_xdummy_diag.txt ist raus.

        m_xdummy_obj = nullptr;
        m_xdummy_await = false;
        // [NO_LAG XBOW] der naechste Dummy faengt ungeparentet an
        m_xdummy_parented = false;

        return;
    }

    m_xdrop.active = false;   // neuer Griff/Vorschau -> laufenden Fall abbrechen

    if (!alive) {
        m_xdummy_obj = nullptr;
        m_xdummy_parented = false;
        ++m_xdummy_req;

        if ((m_xdummy_req % 20) == 0) {
            xrequest_dummy();
        }

        return;
    }

    re4vr::call_safe<void*>(dummy, "setLifeTime", 999.0f);
    const auto& c = xcfg(4600);

    // [NO_LAG XBOW] Geparentet ans L_Hand -> nur LOKALE Pose setzen; die
    // Welt-Position macht die Engine, und zwar VOR dem Skinning.
    if (xdummy_set_parent(dummy, true)) {
        auto* tfp = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

        if (tfp == nullptr) {
            return;
        }

        set_vec3(tfp, "set_LocalPosition", glm::vec3{c.dx, c.dy, c.dz});
        set_quat(tfp, "set_LocalRotation", quat_from_euler(c.drx, c.dry, c.drz));
        set_vec3(tfp, "set_LocalScale", glm::vec3{c.dscale, c.dscale, c.dscale});

        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};
    glm::quat hr{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)
        || !get_quat(lh, "get_Rotation", hr)) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    set_vec3(tf, "set_Position", hp + (hr * glm::vec3{c.dx, c.dy, c.dz}));
    set_quat(tf, "set_Rotation",
             glm::normalize(hr * quat_from_euler(c.drx, c.dry, c.drz)));
    set_vec3(tf, "set_LocalScale", glm::vec3{c.dscale, c.dscale, c.dscale});
}

// Drop von der aktuellen Dummy-Position starten (aus dem Holster-Loslassen).
void RE4VRReload2::xstart_drop() {
    auto* dummy = m_xdummy_obj;
    auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");
    glm::vec3 p{};

    if (tf == nullptr || !get_vec3(tf, "get_Position", p)) {
        return;
    }

    m_xdrop.active = true;
    m_xdrop.sx = p.x;
    m_xdrop.sy = p.y;
    m_xdrop.sz = p.z;
    m_xdrop.t0 = clock_now();
    // [NO_LAG XBOW] Startpunkt ist als WELT-Position gelesen -> jetzt vom Joint
    // loesen, sonst faellt der Bolzen relativ zur mitlaufenden Hand.
    xdummy_set_parent(dummy, false);
}

void RE4VRReload2::xbow_refresh() {
    const auto wid = get_equip_wid();

    if (!(m_xscalar.enabled && wid.has_value() && is_xbow(*wid))) {
        m_xwep.wid.reset();
        m_xwep.tf = nullptr;

        return;
    }

    m_xwep.wid = wid;
    glm::vec3 p{};

    if (!(m_xwep.tf != nullptr && get_vec3(m_xwep.tf, "get_Position", p))) {
        m_xwep.tf = xget_gun_tf();
    }
}

bool RE4VRReload2::xcan_load() {
    if (m_arrow.active || m_arrow.insert) {
        return false;
    }

    return xloaded().value_or(0) < xcap() && xreserve() > 0;
}

// [KEYFRAME-PROGRAMM] Der sichtbare Pfeil ist der Hand-Dummy -- statt an der Hand
// wird er im Keyframe-Modus an die WAFFENRELATIVE Bahn gesetzt.
// [KF_HOLD] Nach der Uebergabe blitzte der Bolzen einen Frame lang auf: die Bahn
// endet mit arrow.insert, danach faellt er bis zum Beenden aus unserer Kontrolle.
// Deshalb nach dem Insert-Ende noch kurz auf dem LETZTEN Keyframe halten.
bool RE4VRReload2::xkf_active(RE4VRReloadAdv::Key& out) {
    if (m_adv == nullptr) {
        return false;
    }

    const bool kf_prev =
        static_cast<int32_t>(re4vr::lua_get_number("__re4_shell_kf_preview", 0.0)) == 4600;
    const bool kf_ins = m_arrow.insert && m_adv->has_shell_keys(4600);

    // Nachlauf: die Bahn ist durch, wir halten den Bolzen auf dem letzten Keyframe.
    if (!(kf_prev || kf_ins) && clock_now() < m_xkf_hold && m_adv->has_shell_keys(4600)) {
        return m_adv->shell_pose_at(4600, 1.0f, out);
    }

    if (!(kf_prev || kf_ins)) {
        return false;
    }

    bool have = false;

    if (kf_ins) {
        const float dur = std::max(m_adv->shell_dur, 0.01f);
        float t = static_cast<float>(clock_now() - m_arrow.t0) / dur;

        if (t > 1.0f) {
            t = 1.0f;
        }

        have = m_adv->shell_pose_at(4600, t, out);
    }

    if (!have && kf_prev) {
        out = m_adv->shell_live;
        have = true;
    }

    return have;
}

bool RE4VRReload2::xkf_place_dummy() {
    RE4VRReloadAdv::Key k{};

    if (!xkf_active(k)) {
        return false;
    }

    auto* dummy = m_xdummy_obj;

    // [PORTFIX 2026-09-06] typrichtig lesen -- s. xupdate_dummy.
    if (!(dummy != nullptr
          && re4vr::call_num(dummy, "get_Valid").value_or(0.0) != 0.0)) {
        return false;
    }

    // [MIT DER WAFFE WANDERN 2026-09-07] Vorher wurde der Dummy fuer die Bahn nur
    // vom L_Hand-Joint GELOEST und dann jeden Frame in WELTkoordinaten gesetzt.
    // Beim Laufen ist diese Weltlage zum Renderzeitpunkt schon ueberholt -> die
    // Bahn wandert nicht parallel zur Waffe mit (gemeldet fuer wp4600, dasselbe
    // Bild wie bei der Red9). Jetzt haengt der Dummy fuer die Dauer der Bahn an
    // der WAFFE und bekommt die Keyframe-Lage als LOKALE Pose -- die Keyframes
    // sind ohnehin waffenrelativ, also zieht die Engine ihn mit.
    auto* wtf = (m_xwep.tf != nullptr) ? m_xwep.tf : xget_gun_tf();

    if (!tf_valid(wtf)) {
        return false;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

    if (!tf_valid(tf)) {
        return false;
    }

    // Umhaengen nur wenn noetig, und nur auf gueltige Transforms -- sonst native
    // Access Violation (siehe [[feedback_set_parent_stale_av_crash]]).
    if (re4vr::call_safe<::REManagedObject*>(tf, "get_Parent") != wtf) {
        re4vr::call_safe<void*>(tf, "set_Parent", wtf);
        m_xdummy_parented = false;   // haengt jetzt an der Waffe, nicht an der Hand
    }

    const float s = xcfg(4600).dscale;
    set_vec3(tf, "set_LocalPosition", glm::vec3{k.x, k.y, k.z});
    set_quat(tf, "set_LocalRotation",
             glm::normalize(quat_from_euler(k.rx, k.ry, k.rz)));
    set_vec3(tf, "set_LocalScale", glm::vec3{s, s, s});

    return true;
}

// ---- Kammer-Dock (Einlege-Naehe an der Waffe) ----
std::optional<glm::vec3> RE4VRReload2::xchamber_world() {
    if (m_xwep.tf == nullptr) {
        return std::nullopt;
    }

    const auto& c = xcfg(m_xwep.wid.value_or(4600));
    auto* j = joint_by_name(m_xwep.tf, c.cd_joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{c.cd_x, c.cd_y, c.cd_z});
}

// +1 laden. Bei der Armbrust werden write_dword/addAmmoCount von der Engine
// RE-SYNCT (greifen nicht) -- gebucht wird ausschliesslich ueber load_and_book,
// das bei gestiegener Waffen-Munition den Reserve-Abzug nachzieht.
void RE4VRReload2::xadd_one() {
    if (!m_xscalar.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return;
    }

    auto* wi = get_live_wi();
    const int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(0);
    int32_t cap = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;

    // [CRASH-GUARD] Unbekanntes/0-cap NIE als "Platz frei" werten. Die Armbrust
    // ist eine 1-Schuss-Waffe: rutschte cap=0 durch (wi kurz nicht lesbar), lief
    // der Ladeweg auf die VOLLE Waffe -> nativer AV. Fallback cap=1.
    if (cap <= 0) {
        cap = 1;
    }

    if (loaded >= cap) {
        return;
    }

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    const int32_t r_b4 = ammo_id.has_value() ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (r_b4 <= 0) {
        return;
    }

    std::optional<int32_t> et{};

    if (auto* td = sdk::find_type_definition("chainsaw.EquipType"); td != nullptr) {
        if (auto* f = td->get_field("Main"); f != nullptr) {
            et = f->get_data<int32_t>(nullptr);
        }
    }

    if (et.has_value()) {
        // [CRASH-HARDEN] enableReloadItem-Gate gegen den null-Item-AV
        m_main->load_and_book(inv, *et, 1, false);
    }

    const int32_t loaded_af = call_enum(pe, "getCurrentGunAmmo").value_or(loaded);

    if (loaded_af > loaded) {
        xplay_sound(XSND_INSERT);   // [SOUND] nur bei echtem +1
    }
}

// ---- Einlegen: Naehe Hand->Kammer -> kurzes Fenster, dann +1 ----
void RE4VRReload2::xcheck_insert_proximity() {
    // nur beim echten Holster-Griff einlegen, nicht in der Vorschau
    if (!m_arrow.active || m_arrow.insert) {
        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)) {
        return;
    }

    const auto gp = xchamber_world();

    if (!gp.has_value()) {
        return;
    }

    if (vec_len(vec_sub(hp, *gp)) <= m_xscalar.insert_distance) {
        m_arrow.t0 = clock_now();
        m_arrow.insert = true;
        m_arrow.active = false;
    }
}

void RE4VRReload2::xupdate_arrow_insert() {
    if (!m_arrow.insert) {
        return;
    }

    const float t = static_cast<float>(clock_now() - m_arrow.t0)
                    / std::max(m_arrow.dur, 0.01f);

    if (t >= 1.0f) {
        m_arrow.insert = false;
        xadd_one();
    }
}

// ---- Hand-Pose XbowBolt + Daumen-Tuning ----
void RE4VRReload2::apply_xbow_pose() {
    const std::string want =
        (m_arrow.active || m_arrow.insert || m_arrow.tune || m_xprev) ? "XbowBolt" : "";
    // [POSE_FADE] beim Loslassen ueber POSE_FADE_DUR zurueckblenden statt snappen
    float b = 0.0f;

    if (!pose_fade_step(m_xbow_fade, want, b)) {
        return;
    }

    pose_apply(m_xbow_pose, b);

    const auto& c = xcfg(m_xwep.wid.value_or(4600));

    if (c.t_rx != 0.0f || c.t_ry != 0.0f || c.t_rz != 0.0f) {
        auto* bt = body_tf();
        auto* tj = (bt != nullptr) ? joint_by_name(bt, "L_Thumb1") : nullptr;
        glm::quat cur{};

        if (tj != nullptr && get_quat(tj, "get_LocalRotation", cur)) {
            set_quat(tj, "set_LocalRotation",
                     glm::normalize(cur * quat_from_euler(c.t_rx * b, c.t_ry * b,
                                                          c.t_rz * b)));
        }
    }
}

bool RE4VRReload2::xbow_set_arrow_in_hand(bool active) {
    if (active) {
        if (!xcan_load()) {
            return false;
        }

        m_arrow.active = true;
        xplay_sound(XSND_GRAB);   // [SOUND] Pfeil aus dem Ammo-Holster gegriffen

        return true;
    }

    if (m_arrow.active) {   // losgelassen ohne Einlegen -> faellt + Sound
        xstart_drop();
        xplay_sound(XSND_DROP);
    }

    m_arrow.active = false;

    return true;
}

void RE4VRReload2::xbow_soft_reset() {
    m_arrow.active = false;
    m_arrow.insert = false;
    m_arrow.tune = false;
    xupdate_dummy(false);   // Hand-Dummy-Bolzen freigeben
}

void RE4VRReload2::xbow_on_frame() {
    // [HAND-DUMMY] Hand-Bolzen anzeigen bei Vorschau ODER echtem Griff ODER wenn
    // der reload_adv-Keyframe-Preview fuer 4600 an ist.
    const bool kf_prev =
        static_cast<int32_t>(re4vr::lua_get_number("__re4_shell_kf_preview", 0.0)) == 4600;
    xupdate_dummy(m_arrow.active || m_arrow.insert || m_arrow.tune || m_xprev || kf_prev);
    xbow_refresh();

    if (m_xwep.wid != m_xprev_wid) {
        const bool was_xbow = m_xprev_wid.has_value();
        xbow_soft_reset();

        if (was_xbow && !m_xwep.wid.has_value()) {
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
        }

        m_xprev_wid = m_xwep.wid;
    }

    if (!m_xwep.wid.has_value()) {
        return;
    }

    // Right-B abfangen -> KEIN nativer Reload. B macht nichts.
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);
    xcheck_insert_proximity();

    // Holster-Gate: buzzen nur, wenn NICHTS nachladbar ist
    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        !(xreserve() > 0 && xloaded().value_or(0) < xcap()));
    // Globals fuer motion/arm_chain
    re4vr::lua_set_bool("__vr_mag_in_hand", m_arrow.active || m_arrow.insert);
}

void RE4VRReload2::xbow_apply_pass() {
    const auto ewid = get_equip_wid();

    if (!(m_xscalar.enabled && ewid.has_value() && is_xbow(*ewid))) {
        return;
    }

    // [KEYFRAME-PROGRAMM] ui_wid + weapon_tf fuer die reload_adv-Keyframe-UI AUF
    // DEM APP-ENTRY-PASS publizieren, sonst ueberschreibt reload sie mit nil,
    // bevor shell_preview_apply liest. shell_joint = nil: der Pfeil ist ein
    // Dummy-GO, KEIN Joint -> die Native-Preview soll nichts anfassen.
    re4vr::lua_set_number("__re4_reload_ui_wid",
                          static_cast<double>(m_xwep.wid.value_or(0)));
    re4vr::lua_set_managed_object("__re4_reload_weapon_tf", m_xwep.tf);
    re4vr::lua_set_nil("__re4_reload_shell_joint");

    const bool kf_prev =
        static_cast<int32_t>(re4vr::lua_get_number("__re4_shell_kf_preview", 0.0)) == 4600;

    // [KF_HOLD] Insert gerade beendet -> Haltefenster oeffnen.
    if (m_xkf_prev_ins && !m_arrow.insert) {
        m_xkf_hold = clock_now()
            + re4vr::lua_get_number("__re4_xbow_kf_hold", XKF_HOLD_DUR);
    }

    m_xkf_prev_ins = m_arrow.insert;

    // Waehrend des Haltefensters bleibt der Bolzen bestehen.
    const bool cond = m_arrow.active || m_arrow.insert || m_arrow.tune || m_xprev
                      || kf_prev || (clock_now() < m_xkf_hold);

    if (cond) {
        xupdate_arrow_insert();   // Einlege-Timer -> +1
    }

    // IMMER aufrufen: [POSE_FADE] tickt auch nach dem Loslassen weiter
    apply_xbow_pose();
    // SPAET setzen (gewinnt gegen ShellDummy.lateUpdate); treibt auch den Drop
    xupdate_dummy(cond);
    // [KEYFRAME-PROGRAMM] im Keyframe-Modus den Dummy an die Bahn ueberschreiben
    xkf_place_dummy();
}

// [WOBBLE-FIX] Bolzen-Dummy NACH motions BeginRendering-POST (attach_left_hand)
// nochmal auf die finale VR-Hand setzen -> klebt, statt einen Pass hinterher zu
// haengen. Nur Reposition (kein Spawn/Drop/Lifetime).
void RE4VRReload2::xbow_reposition_late() {
    const auto ewid = get_equip_wid();

    if (!(m_xscalar.enabled && ewid.has_value() && is_xbow(*ewid))) {
        return;
    }

    // [KEYFRAME-PROGRAMM] Im Keyframe-Modus den Dummy an die waffenrelative Bahn.
    if (xkf_place_dummy()) {
        return;
    }

    if (!(m_arrow.active || m_arrow.insert || m_arrow.tune || m_xprev)) {
        return;
    }

    if (m_xdrop.active) {
        return;
    }

    auto* dummy = m_xdummy_obj;

    // [PORTFIX 2026-09-06] typrichtig lesen -- s. xupdate_dummy.
    if (!(dummy != nullptr
          && re4vr::call_num(dummy, "get_Valid").value_or(0.0) != 0.0)) {
        return;
    }

    const auto& c = xcfg(4600);
    auto* tf = re4vr::call_safe<::REManagedObject*>(dummy, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    // [NO_LAG XBOW] Geparentet -> hier nur die LOKALE Pose nachziehen (falls im
    // UI geschoben wird); die Welt-Position rechnet die Engine aus dem
    // L_Hand-Joint. Der Welt-Weg darunter bleibt der Fallback.
    if (m_xdummy_parented) {
        set_vec3(tf, "set_LocalPosition", glm::vec3{c.dx, c.dy, c.dz});
        set_quat(tf, "set_LocalRotation", quat_from_euler(c.drx, c.dry, c.drz));

        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};
    glm::quat hr{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)
        || !get_quat(lh, "get_Rotation", hr)) {
        return;
    }

    set_vec3(tf, "set_Position", hp + (hr * glm::vec3{c.dx, c.dy, c.dz}));
    set_quat(tf, "set_Rotation",
             glm::normalize(hr * quat_from_euler(c.drx, c.dry, c.drz)));
}

void RE4VRReload2::xbow_on_script_reset() {
    m_xwep.wid.reset();
    m_xwep.tf = nullptr;
    xbow_soft_reset();
    m_character_manager = nullptr;
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
}

// ---------------------------------------------------------------------
// UI -- Armbrust
// ---------------------------------------------------------------------
void RE4VRReload2::xbow_ui() {
    if (ImGui::Checkbox("##xbow_en", &m_xscalar.enabled)) {
        xbow_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Crossbow Reload (Armbrust wp4600)");

    if (!ImGui::TreeNode("Armbrust -- Einstellungen")) {
        return;
    }

    const auto awid = m_xwep.wid.has_value() ? m_xwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_xbow(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Armbrust erkannt - verwaltet)"
                             : "  (keine Armbrust equippt)");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Hand-Pfeil = Mesh-Parts [%s] (werden sichtbar geforced; "
                       "KEIN Joint)", m_xscalar.hand_parts.c_str());

    const auto ld = xloaded();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  loaded=%s cap=%d reserve=%d  arrow=%s",
                       ld.has_value() ? std::to_string(*ld).c_str() : "nil",
                       xcap(), xreserve(),
                       (m_arrow.active || m_arrow.insert) ? "true" : "false");

    if (ImGui::SliderFloat("Einlege-Distanz m##xdist",
                           &m_xscalar.insert_distance, 0.03f, 0.50f)) {
        xbow_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen (+1)##xammo", &m_xscalar.reload_ammo)) {
        xbow_save_cfg();
    }

    auto& cc = xcfg(awid.value_or(4600));

    if (ImGui::TreeNode("Hand-Pfeil (Mesh-Parts) + Pose")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Hand-Pfeil dauerhaft sichtbar zwingen##xtune",
                        &m_arrow.tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (forced die Mesh-Parts unten sichtbar + XbowBolt-Pose "
                           "der linken Hand)");
        char hb[64]{};
        std::snprintf(hb, sizeof(hb), "%s", m_xscalar.hand_parts.c_str());

        if (ImGui::InputText("Hand-Pfeil Parts (Komma)##xhandparts", hb, sizeof(hb))) {
            m_xscalar.hand_parts = hb;
            xbow_save_cfg();
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  Bsp: 20,21,22,23,24  (Vorschau AN -> Nummern aendern "
                           "bis der Pfeil in der Hand sitzt)");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Daumen (additiv, XbowBolt-Pose):");
        ch |= ImGui::SliderFloat("Daumen RotX##xtx", &cc.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##xty", &cc.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##xtz", &cc.t_rz, -90.0f, 90.0f);

        if (ch) {
            xbow_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Hand-Bolzen Position (Dummy)##xdummy")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Bolzen an der Hand anzeigen##xprev", &m_xprev);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Offset relativ zur linken Hand (Vorschau hier aktivieren):");
        ch |= ImGui::SliderFloat("Pos X##xddx", &cc.dx, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Y##xddy", &cc.dy, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Z##xddz", &cc.dz, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Rot X##xddrx", &cc.drx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Y##xddry", &cc.dry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Z##xddrz", &cc.drz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Scale##xdsc", &cc.dscale, 0.05f, 2.0f);

        if (ch) {
            xbow_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Kammer-Dock (Einlegepunkt)##xcd")) {
        bool ch = false;
        char jb[64]{};
        std::snprintf(jb, sizeof(jb), "%s", cc.cd_joint.c_str());

        if (ImGui::InputText("Dock-Joint##xcdj", jb, sizeof(jb))) {
            cc.cd_joint = jb;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Dock X##xcdx", &cc.cd_x, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Dock Y##xcdy", &cc.cd_y, -0.20f, 0.20f);
        ch |= ImGui::SliderFloat("Dock Z##xcdz", &cc.cd_z, -0.20f, 0.20f);

        if (ch) {
            xbow_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Holster greifen (Reserve>0) -> Hand-Pfeil-Parts sichtbar -> "
                       "zur Kammer fuehren = +1. Right-B macht nichts, Feuern nativ.");
    ImGui::TreePop();
}

// ============================================================================
// 5 -- BOWDRAW (wp4600 Bolt Thrower, Lua Z.4652-5388)
//
// Die Engine spannte den Bogen automatisch, sobald AIM gedrueckt wurde. Das ist
// hier ersetzt: die LINKE Hand greift den Knopf hinten (Joint _01) und zieht ihn
// nach hinten; Sehne und Wurfarme folgen dem Zug 1:1.
//
// DER ANDOCKPUNKT IST EINE EINSTELLUNG, KEIN MESSWERT -- er wird im UI gesetzt
// (Regler + SET) und liegt RELATIV ZUM JOINT _01, wandert also mit, wenn der
// Knopf nach hinten faehrt. UI-ORT: REFramework-Fenster am DESKTOP ->
// "RE4VR - Reload2" -> "Bowdraw". Werte in
// reframework/data/re4_vr/re4_vr_bow_draw.json.
//
// GEMESSEN 2026-09-01 (drei Aufnahmen Zahl fuer Zahl gleich). Beim automatischen
// Spannen bewegt die Engine LOKAL (Eltern = _00):
//   _06  Sehne/Schlitten   lp(0, 0.06708, 0.25208) -> dZ -0.19997, dY -0.00240
//   _04  Wurfarm rechts    ry  67 Grad -> 0
//   _05  Wurfarm links     ry -67 Grad -> 0
//   _01  Spannhebel        dreht -93.4 Grad um X und zurueck (Transient), die
//        Position bleibt UNVERAENDERT -- der Zug nach hinten ist unsere Zutat.
//   _07  Kind von _01, +81.3 Grad um X, ebenfalls nur Transient.
//   _00/_100/_101 springen in EINEM Frame auf die Aim-Pose -> gehoeren NICHT zum
//        Spannen und werden hier NICHT angefasst.
//
// ZWEI GETRENNTE GROESSEN, das ist der Kern:
//   p    = Stellung des HEBELS (_01). Folgt der Hand und schnellt beim Loslassen
//          IMMER in die Ruhelage zurueck -- wie ein echter Spannhebel.
//   draw = Spannzustand des BOGENS (Sehne _06, Wurfarme _04/_05, Mesh-Part). Der
//          bleibt stehen, wo der Hebel ihn hingebracht hat.
//
// ZUGMESSUNG am HANDABSTAND (linke gegen rechte Hand), nicht gegen die Waffe --
// gleiche Lehre wie der Pump: kein Waffenbezug, kein Weltanker, richtungslos,
// selbstnachziehend.
// NOT-AUS: __re4_bow_draw = false
// ============================================================================
namespace {

constexpr int32_t BOW_WID = 4600;
constexpr const char* BOW_JSON = "re4_vr/re4_vr_bow_draw.json";
// ab hier gilt der Bogen beim Loslassen als gespannt
constexpr float BOW_LATCH_AT = 0.85f;

// Ruhelagen der Spann-Joints, HARTKODIERT (gemessen). Bewusst nicht live
// abgegriffen: eine live gemessene Nulllage faengt den Engine-Zustand des
// Messmoments mit ein.
constexpr glm::vec3 REST_01{0.00000f, -0.06975f, -0.06405f};
constexpr glm::vec3 REST_04{0.01164f, 0.04197f, 0.33266f};
constexpr glm::vec3 REST_05{-0.01164f, 0.04197f, 0.33266f};
constexpr glm::vec3 REST_06{0.00000f, 0.06708f, 0.25208f};
constexpr glm::vec3 REST_07{0.00000f, 0.11664f, 0.06980f};
constexpr glm::vec3 DRAW_06{0.00000f, -0.00240f, -0.19997f};   // Sehnenweg bei p = 1
constexpr float LIMB_RY = 67.0f;   // Wurfarm-Ruhewinkel (+ rechts, - links)

// MESH-PARTS des Bogens (Parts-Probe 2026-09-01: beim automatischen Spannen ist
// immer GENAU EINER an, der Zyklus lief zweimal identisch 2 -> 1 -> 3 -> 2).
// Der Bogen klappt also NICHT ueber Joints auf, sondern ueber diesen Part-Tausch.
constexpr int32_t PART_REST = 2;   // entspannt (Ruhe)
constexpr int32_t PART_DRAW = 1;   // gespannt
constexpr int32_t PART_SNAP = 3;   // das kurze Zurueckschnellen -- hier noch ungenutzt

constexpr uint32_t SND_PULL = 2042637912u;      // Zug am Hebel
constexpr uint32_t SND_DRYFIRE = 3388506884u;   // RT mit 0 Pfeilen
constexpr uint32_t SND_SNAP = 153932128u;       // Hebel schnellt nach vorn
constexpr float SND_ARM_P = 0.03f;              // ab diesem Hebelweg loest er aus

glm::quat quat_y(float deg) {
    const float h = glm::radians(deg) * 0.5f;

    return glm::normalize(glm::quat{std::cos(h), 0.0f, std::sin(h), 0.0f});
}

}   // namespace

void RE4VRReload2::bow_load_cfg() {
    const auto d = re4vr::json_load(BOW_JSON);

    if (!d.is_object()) {
        return;
    }

    m_bow.dx = jnum(d, "dx", m_bow.dx);
    m_bow.dy = jnum(d, "dy", m_bow.dy);
    m_bow.dz = jnum(d, "dz", m_bow.dz);
    m_bow.rx = jnum(d, "rx", m_bow.rx);
    m_bow.ry = jnum(d, "ry", m_bow.ry);
    m_bow.rz = jnum(d, "rz", m_bow.rz);
    m_bow.adx = jnum(d, "adx", m_bow.adx);
    m_bow.ady = jnum(d, "ady", m_bow.ady);
    m_bow.adz = jnum(d, "adz", m_bow.adz);
    m_bow.arx = jnum(d, "arx", m_bow.arx);
    m_bow.ary = jnum(d, "ary", m_bow.ary);
    m_bow.arz = jnum(d, "arz", m_bow.arz);
    m_bow.grab_dist = jnum(d, "grab_dist", m_bow.grab_dist);
    m_bow.pull_travel = jnum(d, "pull_travel", m_bow.pull_travel);
    m_bow.part_at = jnum(d, "part_at", m_bow.part_at);
    m_bow.snd_at = jnum(d, "snd_at", m_bow.snd_at);
    m_bow.snap_dur = jnum(d, "snap_dur", m_bow.snap_dur);
    m_bow.aim_mute = jnum(d, "aim_mute", m_bow.aim_mute);
    m_bow.dock_lerp = jnum(d, "dock_lerp", m_bow.dock_lerp);
}

void RE4VRReload2::bow_save_cfg() {
    nlohmann::json t = {
        {"dx", m_bow.dx}, {"dy", m_bow.dy}, {"dz", m_bow.dz},
        {"rx", m_bow.rx}, {"ry", m_bow.ry}, {"rz", m_bow.rz},
        {"adx", m_bow.adx}, {"ady", m_bow.ady}, {"adz", m_bow.adz},
        {"arx", m_bow.arx}, {"ary", m_bow.ary}, {"arz", m_bow.arz},
        {"grab_dist", m_bow.grab_dist}, {"pull_travel", m_bow.pull_travel},
        {"part_at", m_bow.part_at}, {"snd_at", m_bow.snd_at},
        {"snap_dur", m_bow.snap_dur}, {"aim_mute", m_bow.aim_mute},
        {"dock_lerp", m_bow.dock_lerp},
    };

    re4vr::json_save(BOW_JSON, t);
}

// Der Bowdraw-Block hat in Lua eigene Kopien von ctx/live_wid/body_tf -- inhaltlich
// identisch mit den Datei-Helfern; hier reichen die geteilten.
std::optional<int32_t> RE4VRReload2::bow_live_wid() {
    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");

    return (hu != nullptr) ? call_enum(hu, "get_EquipWeaponID") : std::nullopt;
}

void RE4VRReload2::bow_play_sound(uint32_t id) {
    if (id == 0) {
        return;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");
    auto* g = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
    auto* go = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    // Merker, damit der Mute-Hook UNSERE eigenen Sounds durchlaesst
    re4vr::lua_set_bool("__re4_bow_snd_self", true);
    m_bow_snd_self = true;
    trigger_sound(go, id);
    m_bow_snd_self = false;
    re4vr::lua_set_bool("__re4_bow_snd_self", false);
}

// Geladene Munition der gefuehrten Waffe (nullopt = nicht lesbar).
std::optional<int32_t> RE4VRReload2::bow_loaded_ammo() {
    auto* wi = re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
}

// Mesh der gefuehrten Waffe (dieselbe Kette wie der Parts-Klon: EquipWeapon -> get_Mesh).
::REManagedObject* RE4VRReload2::bow_weapon_mesh() {
    int32_t cnt = 0;

    if (m_bow_st.mesh != nullptr
        && re4vr::try_call<int32_t>(m_bow_st.mesh, "getPartsEnableCount", cnt)) {
        return m_bow_st.mesh;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");
    auto* g = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
    m_bow_st.mesh = re4vr::call_safe<::REManagedObject*>(g, "get_Mesh");

    return m_bow_st.mesh;
}

// Genau EINEN der drei Bogen-Parts anzeigen.
// [LIVE PRUEFEN] Ein Merker "welchen Part habe ich zuletzt gesetzt" reicht NICHT:
// die Engine faehrt ihren eigenen Spann-Zyklus (1 -> 3 -> 2) weiter und schaltet
// den Part hinter unserem Ruecken zurueck. Der Merker sagt dann "steht schon
// richtig" und wir schreiben nie wieder -- Symptom: der Bogen klappt auf und
// faellt gleich wieder zusammen. Also jeden Pass am Mesh NACHSEHEN.
void RE4VRReload2::bow_set_part(int32_t idx) {
    auto* m = bow_weapon_mesh();

    if (m == nullptr) {
        return;
    }

    for (const int32_t i : {PART_REST, PART_DRAW, PART_SNAP}) {
        const bool want = (i == idx);
        bool cur = false;

        if (!re4vr::try_call<bool>(m, "getPartsEnable", cur, i) || cur != want) {
            re4vr::call_safe<void*>(m, "setPartsEnable", i, want);
        }
    }

    m_bow_st.part_now = idx;
}

void RE4VRReload2::bow_drop_refs() {
    m_bow_st.tf = nullptr;
    m_bow_st.j01 = nullptr;
    m_bow_st.j04 = nullptr;
    m_bow_st.j05 = nullptr;
    m_bow_st.j06 = nullptr;
    m_bow_st.j07 = nullptr;
    m_bow_st.mesh = nullptr;
    m_bow_st.part_now.reset();
    m_bow_st.grabbed = false;
    m_bow_st.cocked = false;
    m_bow_st.p = 0.0f;
    m_bow_st.draw = 0.0f;
    m_bow_st.anchor.reset();
    m_bow_st.snap_t.reset();
}

bool RE4VRReload2::bow_resolve() {
    if (bow_live_wid().value_or(0) != BOW_WID) {
        if (m_bow_st.tf != nullptr) {
            bow_drop_refs();
        }

        return false;
    }

    glm::vec3 p{};

    if (!(m_bow_st.tf != nullptr && get_vec3(m_bow_st.tf, "get_Position", p))) {
        bow_drop_refs();
        m_bow_st.tf = find_weapon(BOW_WID);

        if (m_bow_st.tf == nullptr) {
            return false;
        }
    }

    if (!(m_bow_st.j01 != nullptr && m_bow_st.j04 != nullptr
          && m_bow_st.j05 != nullptr && m_bow_st.j06 != nullptr)) {
        m_bow_st.j01 = joint_by_name(m_bow_st.tf, "_01");
        m_bow_st.j04 = joint_by_name(m_bow_st.tf, "_04");
        m_bow_st.j05 = joint_by_name(m_bow_st.tf, "_05");
        m_bow_st.j06 = joint_by_name(m_bow_st.tf, "_06");
        m_bow_st.j07 = joint_by_name(m_bow_st.tf, "_07");
    }

    return m_bow_st.j01 != nullptr && m_bow_st.j04 != nullptr
           && m_bow_st.j05 != nullptr && m_bow_st.j06 != nullptr;
}

// ------------------------------------------------------- Der EINE Andockpunkt
// Position des Joints _01 plus dem eingestellten Versatz, gedreht in die
// Waffenachsen. Weil der Bezug der JOINT ist, wandert der Punkt beim Spannen von
// selbst mit. Welcher Satz gilt: im Spiel der echte Zielzustand, in der Vorschau
// der im UI gewaehlte (sonst muesste man am Desktop LT halten).
bool RE4VRReload2::bow_use_aim() {
    if (m_bow_st.preview) {
        return m_bow_st.edit_aim;
    }

    return re4vr::lua_get_tribool("__vr_aim_input") == 1;
}

std::optional<glm::vec3> RE4VRReload2::bow_dock_pos() {
    glm::vec3 jp{};
    glm::quat gr{};

    if (!get_vec3(m_bow_st.j01, "get_Position", jp)
        || !get_quat(m_bow_st.tf, "get_Rotation", gr)) {
        return std::nullopt;
    }

    const bool a = bow_use_aim();
    const glm::vec3 off{a ? m_bow.adx : m_bow.dx, a ? m_bow.ady : m_bow.dy,
                        a ? m_bow.adz : m_bow.dz};

    return jp + (gr * off);
}

std::optional<glm::quat> RE4VRReload2::bow_dock_rot() {
    glm::quat gr{};

    if (!get_quat(m_bow_st.tf, "get_Rotation", gr)) {
        return std::nullopt;
    }

    const bool a = bow_use_aim();

    return glm::normalize(gr * quat_from_euler(a ? m_bow.arx : m_bow.rx,
                                               a ? m_bow.ary : m_bow.ry,
                                               a ? m_bow.arz : m_bow.rz));
}

std::optional<float> RE4VRReload2::bow_hand_dist() {
    const auto lh = re4vr::lua_get_vec3("__vr_lh_world");
    const auto rh = re4vr::lua_get_vec3_any({"__vr_rh_world", "__vr_unified_rh_pos"});

    if (!lh.has_value() || !rh.has_value()) {
        return std::nullopt;
    }

    return vec_len(vec_sub(*lh, *rh));
}

// [DOCK-LERP] Blend EINMAL pro Frame rampen -- NICHT in publish_dock: das laeuft
// im on_frame UND in den fuenf Render-Paessen, die Rampe liefe also sechsfach zu
// schnell.
void RE4VRReload2::bow_update_dock_blend() {
    const double now = clock_now();
    double dt = m_bow_st.blend_t.has_value() ? (now - *m_bow_st.blend_t) : 0.0;
    m_bow_st.blend_t = now;

    if (dt <= 0.0 || dt > 0.25) {
        dt = 0.0;   // Ladepausen nicht als Zeit zaehlen
    }

    const float want = (re4vr::lua_get_tribool("__re4_bow_draw") != 0
                        && (m_bow_st.grabbed || m_bow_st.preview)) ? 1.0f : 0.0f;
    const float step = static_cast<float>(dt) / std::max(m_bow.dock_lerp, 0.01f);

    if (m_bow_st.blend < want) {
        m_bow_st.blend = std::min(want, m_bow_st.blend + step);
    } else if (m_bow_st.blend > want) {
        m_bow_st.blend = std::max(want, m_bow_st.blend - step);
    }
}

// Selbst veroeffentlicht: reload steigt fuer wp4600 vorher aus ("Waffe nicht
// verwaltet"), sein publish_dock laeuft hier nie -- also raeumt die Globals auch
// niemand weg. Geleert wird nur, was wir selbst gesetzt haben.
bool RE4VRReload2::bow_publish_dock() {
    const auto kp = (m_bow_st.blend > 0.001f) ? bow_dock_pos() : std::nullopt;

    if (kp.has_value()) {
        const float b = m_bow_st.blend;
        re4vr::lua_set_vec3("__vr_slide_hand_world_pos", *kp);

        if (const auto r = bow_dock_rot(); r.has_value()) {
            re4vr::lua_set_quat("__vr_slide_hand_world_rot", *r);
        } else {
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
        }

        // smoothstep, wie in reload
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", b * b * (3.0f - 2.0f * b));
        m_bow_st.pushed_dock = true;

        return true;
    }

    if (m_bow_st.pushed_dock) {
        re4vr::lua_set_nil("__vr_slide_hand_world_pos");
        re4vr::lua_set_nil("__vr_slide_hand_world_rot");
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
        m_bow_st.pushed_dock = false;
    }

    return false;
}

void RE4VRReload2::bow_update_gesture() {
    if (re4vr::lua_get_tribool("__re4_bow_draw") == 0) {
        m_bow_st.msg = "aus (__re4_bow_draw)";

        return;
    }

    if (!bow_resolve()) {
        m_bow_st.grabbed = false;
        m_bow_st.msg = "kein wp4600";

        return;
    }

    // [NACHLADEN VOR CHOKE 16.09.2026 -- Ansage des Users] Hand am Hebel (oder
    // haelt ihn) -> der Grip gehoert dem Bogen, nicht dem Choke. Bewusst VOR
    // der Grip-Abfrage: der Choke entscheidet an der Grip-Flanke und muss die
    // Zone da schon kennen.
    if (m_bow_st.grabbed) {
        re4vr::lua_set_number("__re4_lh_reload_grab_t", clock_now());
    } else if (const auto zl = re4vr::lua_get_vec3("__vr_lh_world"); zl.has_value()) {
        if (const auto zk = bow_dock_pos();
            zk.has_value() && vec_len(vec_sub(*zl, *zk)) <= m_bow.grab_dist) {
            re4vr::lua_set_number("__re4_lh_reload_grab_t", clock_now());
        }
    }

    if (!left_grip_down()) {
        if (m_bow_st.grabbed) {
            // Losgelassen. Durchgezogen -> der Bogen BLEIBT gespannt; sonst
            // faellt er zurueck. Der HEBEL schnellt in jedem Fall nach vorn.
            if (m_bow_st.p >= BOW_LATCH_AT) {
                m_bow_st.draw = 1.0f;
                m_bow_st.cocked = true;
            } else {
                m_bow_st.draw = 0.0f;
                m_bow_st.cocked = false;
            }

            m_bow_st.grabbed = false;
            m_bow_st.anchor.reset();
            m_bow_st.snap_from = m_bow_st.p;
            m_bow_st.snap_t = clock_now();

            if (m_bow_st.snap_from > SND_ARM_P) {   // nur wenn wirklich was zurueckfaehrt
                bow_play_sound(SND_SNAP);
            }
        }

        // Ruecklauf des Hebels abspulen
        if (m_bow_st.snap_t.has_value()) {
            const float f = static_cast<float>(clock_now() - *m_bow_st.snap_t)
                            / std::max(m_bow.snap_dur, 0.005f);

            if (f >= 1.0f) {
                m_bow_st.p = 0.0f;
                m_bow_st.snap_t.reset();
            } else {
                m_bow_st.p = m_bow_st.snap_from * (1.0f - f);
            }

            if (m_bow_st.p <= SND_ARM_P) {
                m_bow_st.snd_armed = true;   // Hebel vorn -> Sound wieder scharf
            }
        }

        m_bow_st.msg = m_bow_st.cocked ? "gespannt (Hebel vorn)" : "entspannt";

        return;
    }

    m_bow_st.snap_t.reset();   // wieder gegriffen -> kein Ruecklauf mehr

    const auto lh = re4vr::lua_get_vec3("__vr_lh_world");

    if (!lh.has_value()) {
        m_bow_st.msg = "keine Handposition";

        return;
    }

    if (!m_bow_st.grabbed) {
        const auto kp = bow_dock_pos();

        if (!kp.has_value()) {
            m_bow_st.msg = "Andockpunkt nicht lesbar";

            return;
        }

        const float d0 = vec_len(vec_sub(*lh, *kp));

        if (d0 > m_bow.grab_dist) {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "Hand %.2f m vom Punkt", d0);
            m_bow_st.msg = buf;

            return;
        }

        const auto d = bow_hand_dist();

        if (!d.has_value()) {
            m_bow_st.msg = "kein Handabstand";

            return;
        }

        m_bow_st.grabbed = true;
        m_bow_st.anchor = *d;
        m_bow_st.p0 = m_bow_st.p;
    }

    const auto d = bow_hand_dist();

    if (!d.has_value()) {
        return;
    }

    // Zug = die Haende entfernen sich voneinander. Selbstnachziehend: solange
    // noch nicht gezogen wurde und der Abstand SCHRUMPFT, wandert der Nullpunkt
    // mit -- man darf also in Ruhe hingreifen, ohne den Zyklus zu verbrennen.
    float along = *d - m_bow_st.anchor.value_or(*d);

    if (along < 0.0f && m_bow_st.p0 <= 0.0f) {
        m_bow_st.anchor = *d;
        along = 0.0f;
    }

    m_bow_st.p = std::clamp(m_bow_st.p0 + along / m_bow.pull_travel, 0.0f, 1.0f);

    // Der Bogen wird vom Hebel MITGENOMMEN, faellt aber nicht wieder zurueck,
    // solange gegriffen ist -- sonst wuerde ein kurzes Nachfassen ihn entspannen.
    if (!m_bow_st.cocked && m_bow_st.p > m_bow_st.draw) {
        m_bow_st.draw = m_bow_st.p;
    }

    // [SOUND SPAETER] Der Zug-Sound kam frueher schon bei SND_ARM_P (0.03) --
    // also praktisch im Moment des Anpackens. Er haengt jetzt an snd_at
    // (Default 0.70). BEWUSST EIN EIGENER WERT: SND_ARM_P steuert daneben noch,
    // ob nach dem Loslassen ein Snap-Sound kommt und ab wann der Zug-Sound wieder
    // scharf wird -- beides gehoert an den Hebel-Nullpunkt.
    if (m_bow_st.snd_armed && m_bow_st.p >= m_bow.snd_at) {
        m_bow_st.snd_armed = false;
        bow_play_sound(SND_PULL);
    }

    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "Hebel %.0f%%   Bogen %.0f%%",
                  m_bow_st.p * 100.0f, m_bow_st.draw * 100.0f);
    m_bow_st.msg = buf;
}

// Darf jetzt geschossen werden? Nur mit voll gespanntem Bogen UND vorn stehendem
// Hebel. Die Sperre haengt an einem ZEITSTEMPEL, nicht an einem Flag: tickt das
// hier nicht mehr (Waffenwechsel, Save-Load), loest sie sich nach 0.3 s von
// selbst auf -- eine haengende Feuersperre waere gamebreaking.
void RE4VRReload2::bow_publish_fire_gate() {
    if (bow_live_wid().value_or(0) != BOW_WID
        || re4vr::lua_get_tribool("__re4_bow_draw") == 0) {
        return;
    }

    const bool ok = (m_bow_st.draw >= 1.0f) && !m_bow_st.grabbed && (m_bow_st.p <= 0.02f);
    m_bow_fire_block = !ok;
    m_bow_gate_t = clock_now();
    re4vr::lua_set_bool("__re4_bow_fire_block", !ok);
    re4vr::lua_set_number("__re4_bow_gate_t", m_bow_gate_t);
}

// [LEER ENTSPANNEN] Mit 0 Pfeilen faellt kein Schuss, also kommt auch kein
// Schuss-Zaehler -- der Bogen bliebe fuer immer gespannt. Darum bei leerem Bogen
// schon auf den RT-DRUCK selbst entspannen.
void RE4VRReload2::bow_watch_rt_empty() {
    // 0 als Startwert, NICHT aussteigen: der Zaehler existiert erst ab dem ersten
    // Druck -- sonst wird genau dieser erste Druck verbraucht.
    const int32_t id =
        static_cast<int32_t>(re4vr::lua_get_number("__vr_burst_press_id", 0.0));

    if (!m_bow_st.rt_press.has_value()) {
        m_bow_st.rt_press = id;

        return;
    }

    if (id == *m_bow_st.rt_press) {
        return;
    }

    m_bow_st.rt_press = id;
    // [SCHUSS NICHT SCHLUCKEN] Ein RT-Druck heisst: gleich faellt ein Schuss. Das
    // Mute-Fenster vom Zielbeginn wuerde dessen Sound mitnehmen -- also sofort zu.
    m_bow_mute_until = 0.0;
    re4vr::lua_set_nil("__re4_bow_mute_until");

    if (bow_live_wid().value_or(0) != BOW_WID || m_bow_st.draw <= 0.0f) {
        return;
    }

    if (const auto n = bow_loaded_ammo(); n.has_value() && *n <= 0) {
        m_bow_st.draw = 0.0f;
        m_bow_st.cocked = false;
        bow_play_sound(SND_DRYFIRE);
    }
}

// Ist ein Schuss gefallen, ist der Bogen wieder entspannt.
void RE4VRReload2::bow_watch_shot() {
    // 0 als Startwert: __vr_shot_seq legt crosshair erst beim ersten Schuss an.
    const int32_t n = static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

    if (!m_bow_st.shot_seq.has_value()) {
        m_bow_st.shot_seq = n;

        return;
    }

    if (n == *m_bow_st.shot_seq) {
        return;
    }

    m_bow_st.shot_seq = n;

    if (bow_live_wid().value_or(0) == BOW_WID) {
        m_bow_st.draw = 0.0f;
        m_bow_st.cocked = false;
        m_bow_mute_until = 0.0;
        re4vr::lua_set_nil("__re4_bow_mute_until");
    }
}

// [AIM-SOUND MUTEN] Beim Zielen spielt die Engine ihren nativen Spann-/
// Reload-Sound. Der gehoert zu einer Bewegung, die wir gar nicht mehr fahren.
void RE4VRReload2::bow_update_aim_mute() {
    const bool on = bow_live_wid().value_or(0) == BOW_WID
                    && re4vr::lua_get_tribool("__re4_bow_draw") != 0;
    m_bow_st.has_bow = on;

    if (!on) {
        m_bow_gun_addr = 0;
        m_bow_mute_until = 0.0;
        re4vr::lua_set_nil("__re4_bow_gun_addr");
        re4vr::lua_set_nil("__re4_bow_mute_until");
        m_bow_st.aim_prev = false;

        return;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");
    auto* g = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
    auto* go = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");
    m_bow_gun_addr = reinterpret_cast<uintptr_t>(go);

    if (go != nullptr) {
        re4vr::lua_set_number("__re4_bow_gun_addr", static_cast<double>(m_bow_gun_addr));
    } else {
        re4vr::lua_set_nil("__re4_bow_gun_addr");
    }

    const bool aim = re4vr::lua_get_tribool("__vr_aim_input") == 1;

    if (aim && !m_bow_st.aim_prev) {
        m_bow_mute_until = clock_now() + m_bow.aim_mute;
        re4vr::lua_set_number("__re4_bow_mute_until", m_bow_mute_until);
    }

    m_bow_st.aim_prev = aim;
}

// --------------------------------------------------------------- Joint-Write
// Muss im vollen Override-Stack laufen, sonst schreibt die Engine im selben Frame
// zurueck.
void RE4VRReload2::bow_apply_joints() {
    if (re4vr::lua_get_tribool("__re4_bow_draw") == 0) {
        return;
    }

    if (!bow_resolve()) {
        return;
    }

    const float p = m_bow_st.p;      // Hebel
    const float w = m_bow_st.draw;   // Spannzustand des Bogens
    const glm::quat ident{1.0f, 0.0f, 0.0f, 0.0f};

    // _01: der gegriffene Knopf. Die Position ist unsere Zutat (die Engine bewegt
    // ihn nie), die Rotation wird auf die Ruhelage genagelt -- damit ist der
    // Engine-Transient (-93.4 Grad um X) mit erledigt.
    set_vec3(m_bow_st.j01, "set_LocalPosition",
             glm::vec3{REST_01.x, REST_01.y, REST_01.z - m_bow.pull_travel * p});
    set_quat(m_bow_st.j01, "set_LocalRotation", ident);

    // _07: haengt am Hebel und wandert als dessen Kind von selbst mit. Nur
    // festnageln -- sonst kippt die Engine ihn beim Zielen und nach jedem Schuss
    // um +81.3 Grad und wieder zurueck.
    if (m_bow_st.j07 != nullptr) {
        set_vec3(m_bow_st.j07, "set_LocalPosition", REST_07);
        set_quat(m_bow_st.j07, "set_LocalRotation", ident);
    }

    // _06: Sehne/Schlitten auf der gemessenen Bahn.
    set_vec3(m_bow_st.j06, "set_LocalPosition", REST_06 + DRAW_06 * w);
    set_quat(m_bow_st.j06, "set_LocalRotation", ident);

    // _04 / _05: die beiden Wurfarme klappen von +-67 Grad auf 0.
    set_vec3(m_bow_st.j04, "set_LocalPosition", REST_04);
    set_quat(m_bow_st.j04, "set_LocalRotation", quat_y(LIMB_RY * (1.0f - w)));
    set_vec3(m_bow_st.j05, "set_LocalPosition", REST_05);
    set_quat(m_bow_st.j05, "set_LocalRotation", quat_y(-LIMB_RY * (1.0f - w)));

    bow_set_part((w >= m_bow.part_at) ? PART_DRAW : PART_REST);

    re4vr::lua_set_number("__re4_bow_draw_p", p);
    re4vr::lua_set_number("__re4_bow_draw_draw", w);
    re4vr::lua_set_bool("__re4_bow_draw_cocked", m_bow_st.cocked);
    re4vr::lua_set_bool("__re4_bow_draw_grabbed", m_bow_st.grabbed);
    bow_publish_dock();   // in JEDEM Pass mitziehen
}

// Handpose. motion ruft das an BEIDEN Pose-Stellen auf (tick-Pass UND
// BeginRendering) -- nur eine Stelle bedient hiesse: im Pausemenue sitzt sie, im
// Gameplay ueberschreibt die Anim sie jeden Frame.
void RE4VRReload2::apply_bowknob_pose() {
    if (re4vr::lua_get_tribool("__re4_bow_draw") == 0 || m_main == nullptr) {
        return;
    }

    // LINKE Hand: am Knopf
    if (m_bow_st.grabbed || m_bow_st.preview) {
        m_main->apply_pose_bones(m_bowknob, 1.0f);
    }

    // RECHTE Hand: beim Zielen die native Reload-Bewegung ueberspielen
    if (m_bow_st.has_bow && re4vr::lua_get_tribool("__vr_aim_input") == 1) {
        m_main->apply_pose_bones(m_bow_aim, 1.0f);
    }
}

void RE4VRReload2::bow_on_frame() {
    // [MAG-HOLSTER SPERREN] In Lua wickelt der Block hier
    // __re4_reload_set_mag_in_hand ein weiteres Mal ein. Nativ steht dieselbe
    // Sperre direkt in set_mag_in_hand (die Kette liegt in EINER Funktion), das
    // Wrappen entfaellt.
    bow_update_gesture();
    bow_watch_shot();
    bow_watch_rt_empty();
    bow_update_aim_mute();
    bow_update_dock_blend();
    bow_publish_fire_gate();
    bow_publish_dock();
}

void RE4VRReload2::bow_on_script_reset() {
    re4vr::lua_set_nil("__re4_bow_draw_p");
    re4vr::lua_set_nil("__re4_bow_draw_cocked");
    re4vr::lua_set_nil("__re4_bow_draw_grabbed");
    // Feuersperre nie stehen lassen
    m_bow_fire_block = false;
    m_bow_gate_t = 0.0;
    re4vr::lua_set_nil("__re4_bow_fire_block");
    re4vr::lua_set_nil("__re4_bow_gate_t");
    // Sound nie stumm zuruecklassen
    m_bow_mute_until = 0.0;
    m_bow_gun_addr = 0;
    re4vr::lua_set_nil("__re4_bow_mute_until");
    re4vr::lua_set_nil("__re4_bow_gun_addr");
    bow_set_part(PART_REST);   // Waffe nicht gespannt zuruecklassen
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
}

// ------------------------------------------------------------------------- UI
// DESKTOP: REFramework-Fenster -> "RE4VR - Reload2" -> "Bowdraw". Im Headset
// nicht lesbar, darum bleiben die Werte in der JSON.
void RE4VRReload2::bow_ui() {
    if (!ImGui::TreeNode("Bowdraw")) {
        return;
    }

    ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                       "  %s   gegriffen: %s   gespannt: %s",
                       m_bow_st.msg.c_str(),
                       m_bow_st.grabbed ? "ja" : "nein",
                       m_bow_st.cocked ? "ja" : "nein");
    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Hebel %.0f%%   Bogen %.0f%%",
                       m_bow_st.p * 100.0f, m_bow_st.draw * 100.0f);

    ImGui::Checkbox("Vorschau: Hand andocken (zum Einstellen, ohne Grip)",
                    &m_bow_st.preview);

    if (m_bow_st.preview) {
        ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f},
                           "  VORSCHAU AKTIV -- die Hand haengt am Punkt und folgt "
                           "den Reglern.");
    }

    ImGui::Checkbox("Regler bearbeiten den AIM-Satz (sonst: ohne AIM)",
                    &m_bow_st.edit_aim);
    ImGui::TextColored(ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       "  bearbeitet: %s     im Spiel gerade aktiv: %s",
                       m_bow_st.edit_aim ? "MIT AIM" : "ohne AIM",
                       (re4vr::lua_get_tribool("__vr_aim_input") == 1) ? "MIT AIM"
                                                                       : "ohne AIM");

    const bool a = m_bow_st.edit_aim;
    float* dx = a ? &m_bow.adx : &m_bow.dx;
    float* dy = a ? &m_bow.ady : &m_bow.dy;
    float* dz = a ? &m_bow.adz : &m_bow.dz;
    float* rx = a ? &m_bow.arx : &m_bow.rx;
    float* ry = a ? &m_bow.ary : &m_bow.ry;
    float* rz = a ? &m_bow.arz : &m_bow.rz;

    ImGui::Text("-- Andockpunkt, relativ zum Joint _01 --");
    ImGui::DragFloat("X##bowdx", dx, 0.001f, -0.5f, 0.5f, "%.4f");
    ImGui::DragFloat("Y##bowdy", dy, 0.001f, -0.5f, 0.5f, "%.4f");
    ImGui::DragFloat("Z##bowdz", dz, 0.001f, -0.5f, 0.5f, "%.4f");
    ImGui::Text("-- Handdrehung, relativ zur Waffe --");
    ImGui::DragFloat("Rot X##bowrx", rx, 0.5f, -180.0f, 180.0f, "%.1f");
    ImGui::DragFloat("Rot Y##bowry", ry, 0.5f, -180.0f, 180.0f, "%.1f");
    ImGui::DragFloat("Rot Z##bowrz", rz, 0.5f, -180.0f, 180.0f, "%.1f");

    if (ImGui::Button("Ohne-AIM-Satz in den AIM-Satz kopieren##bowcp")) {
        m_bow.adx = m_bow.dx;
        m_bow.ady = m_bow.dy;
        m_bow.adz = m_bow.dz;
        m_bow.arx = m_bow.rx;
        m_bow.ary = m_bow.ry;
        m_bow.arz = m_bow.rz;
        m_bow_st.msg = "AIM-Satz vom Ohne-AIM-Satz uebernommen";
    }

    ImGui::Text("-- Rest --");
    ImGui::DragFloat("Greifradius (m)##bowgd", &m_bow.grab_dist, 0.005f, 0.02f, 0.40f, "%.3f");
    ImGui::DragFloat("Zugweg (m)##bowpt", &m_bow.pull_travel, 0.005f, 0.05f, 0.40f, "%.3f");
    ImGui::DragFloat("Bogen klappt auf ab Zug##bowpa", &m_bow.part_at, 0.01f, 0.05f, 1.00f, "%.2f");
    ImGui::DragFloat("Spann-Sound ab Hebelweg##bowsa", &m_bow.snd_at, 0.01f, 0.05f, 1.00f, "%.2f");
    ImGui::DragFloat("Ruecklauf des Hebels (s)##bowsd", &m_bow.snap_dur, 0.005f, 0.005f, 0.40f, "%.3f");
    ImGui::DragFloat("Sound stumm nach AIM (s)##bowam", &m_bow.aim_mute, 0.05f, 0.00f, 3.00f, "%.2f");
    ImGui::DragFloat("Hand faehrt an den Knopf (s)##bowdl", &m_bow.dock_lerp, 0.01f, 0.01f, 1.00f, "%.2f");

    if (ImGui::Button("        S E T   (in die JSON schreiben)        ##bowset")) {
        bow_save_cfg();
        m_bow_st.msg = std::string{"gespeichert nach data/"} + BOW_JSON;
    }

    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Datei: reframework/data/%s", BOW_JSON);
    ImGui::TreePop();
}

// ============================================================================
// 6 -- RED9 (wp4002, Lua Z.5719-6764)
//
// KOMPLETT eigenstaendig -- reload fasst 4002 nicht mehr an. Die Munition
// (Stripper-Clip + Kugeln = Parts 5,20,21) ist KEIN Joint, also zeigen wir sie
// als MESH-KLON in der Hand: eigenes GameObject + via.motion.Motion (baut das
// Skelett!) + via.render.Mesh + setMesh(lebender Gun-Holder) +
// set_Material(Gun-Material) + Parts isolieren.
// Right-B: nativer Reload geblockt (consume_b).
// Eigenes JSON: re4_vr/re4_vr_reload2_red9.json
// ============================================================================
namespace {

constexpr int32_t R9 = 4002;
constexpr const char* R9_CFG_PATH = "re4_vr/re4_vr_reload2_red9.json";

// ---- Sounds (Wwise-IDs, am Waffen-GameObject getriggert) ----
constexpr uint32_t R9SND_MAG_GRAB = 1839787494u;     // Clip aus dem Holster gezogen
constexpr uint32_t R9SND_SLIDE_PULL = 2611874302u;   // Slide voll nach HINTEN
constexpr uint32_t R9SND_SLIDE_CLOSE = 2611874302u;  // Slide voll nach VORNE
constexpr uint32_t R9SND_DROP = 1351699582u;         // Clip faellt auf den Boden
constexpr uint32_t R9SND_MAG_INSERT = 943565871u;    // Clip in die Waffe
constexpr uint32_t R9SND_DRY_FIRE = 812850326u;      // RT gezogen, Feuern gesperrt

constexpr float R9_CLOSE_DUR = 0.12f;    // s: Settle-Gleiten nach dem Loslassen
constexpr float R9_UNDOCK_OVER = 0.05f;  // m ueber den Slide-Weg hinaus -> Hand loest sich
constexpr float RDROP_DUR = 1.0f;
constexpr float RGRAV = 9.8f;
constexpr float RDROP_SND_DELAY = 0.45f;

}   // namespace

bool RE4VRReload2::is_red9(int32_t wid) {
    return wid == R9;
}

// [RATIO GETEILT] Der Ratio-Schalter gilt fuer BEIDE Red9s -- Leons wp4002 (hier)
// und Adas Samurai Edge wp6113. Besitzer des Werts bleibt DIESE Maschine: sie hat
// die UI und die JSON. Ada liest ihn nur ueber das Global.
void RE4VRReload2::r9_publish_ratio() {
    re4vr::lua_set_number("__re4_red9_shell_ratio", std::max(1, m_r9cfg.shell_ratio));
}

void RE4VRReload2::red9_load_cfg() {
    const auto d = re4vr::json_load(R9_CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    auto& c = m_r9cfg;
    c.enabled = jbool(d, "enabled", c.enabled);
    c.reload_ammo = jbool(d, "reload_ammo", c.reload_ammo);
    c.parts = jstr(d, "parts", c.parts);
    c.single_part = jstr(d, "single_part", c.single_part);
    c.dx = jnum(d, "dx", c.dx);
    c.dy = jnum(d, "dy", c.dy);
    c.dz = jnum(d, "dz", c.dz);
    c.drx = jnum(d, "drx", c.drx);
    c.dry = jnum(d, "dry", c.dry);
    c.drz = jnum(d, "drz", c.drz);
    c.dscale = jnum(d, "dscale", c.dscale);
    c.sdx = jnum(d, "sdx", c.sdx);
    c.sdy = jnum(d, "sdy", c.sdy);
    c.sdz = jnum(d, "sdz", c.sdz);
    c.sdrx = jnum(d, "sdrx", c.sdrx);
    c.sdry = jnum(d, "sdry", c.sdry);
    c.sdrz = jnum(d, "sdrz", c.sdrz);
    c.sdscale = jnum(d, "sdscale", c.sdscale);
    c.st_rx = jnum(d, "st_rx", c.st_rx);
    c.st_ry = jnum(d, "st_ry", c.st_ry);
    c.st_rz = jnum(d, "st_rz", c.st_rz);
    c.sti_rx = jnum(d, "sti_rx", c.sti_rx);
    c.sti_ry = jnum(d, "sti_ry", c.sti_ry);
    c.sti_rz = jnum(d, "sti_rz", c.sti_rz);
    c.s_thumb_str = jnum(d, "s_thumb_str", c.s_thumb_str);
    c.s_index_str = jnum(d, "s_index_str", c.s_index_str);
    c.s_insert_dist = jnum(d, "s_insert_dist", c.s_insert_dist);
    c.t_rx = jnum(d, "t_rx", c.t_rx);
    c.t_ry = jnum(d, "t_ry", c.t_ry);
    c.t_rz = jnum(d, "t_rz", c.t_rz);
    c.i_rx = jnum(d, "i_rx", c.i_rx);
    c.i_ry = jnum(d, "i_ry", c.i_ry);
    c.i_rz = jnum(d, "i_rz", c.i_rz);
    c.si_rx = jnum(d, "si_rx", c.si_rx);
    c.si_ry = jnum(d, "si_ry", c.si_ry);
    c.si_rz = jnum(d, "si_rz", c.si_rz);
    c.rack_z = jnum(d, "rack_z", c.rack_z);
    c.rack_grab = jnum(d, "rack_grab", c.rack_grab);
    c.dock_x = jnum(d, "dock_x", c.dock_x);
    c.dock_y = jnum(d, "dock_y", c.dock_y);
    c.dock_z = jnum(d, "dock_z", c.dock_z);
    c.rack_rx = jnum(d, "rack_rx", c.rack_rx);
    c.rack_ry = jnum(d, "rack_ry", c.rack_ry);
    c.rack_rz = jnum(d, "rack_rz", c.rack_rz);
    c.insert_dist = jnum(d, "insert_dist", c.insert_dist);
    c.insert_dur = jnum(d, "insert_dur", c.insert_dur);
    c.ip_joint = jstr(d, "ip_joint", c.ip_joint);
    c.ip_x = jnum(d, "ip_x", c.ip_x);
    c.ip_y = jnum(d, "ip_y", c.ip_y);
    c.ip_z = jnum(d, "ip_z", c.ip_z);
    c.insert_drop = jnum(d, "insert_drop", c.insert_drop);
    c.slide_joint = jstr(d, "slide_joint", c.slide_joint);
    c.shell_ratio = static_cast<int32_t>(jnum(d, "shell_ratio",
                                              static_cast<float>(c.shell_ratio)));
}

void RE4VRReload2::red9_save_cfg() {
    const auto& c = m_r9cfg;
    nlohmann::json o = {
        {"enabled", c.enabled}, {"reload_ammo", c.reload_ammo},
        {"parts", c.parts}, {"single_part", c.single_part},
        {"dx", c.dx}, {"dy", c.dy}, {"dz", c.dz},
        {"drx", c.drx}, {"dry", c.dry}, {"drz", c.drz}, {"dscale", c.dscale},
        {"sdx", c.sdx}, {"sdy", c.sdy}, {"sdz", c.sdz},
        {"sdrx", c.sdrx}, {"sdry", c.sdry}, {"sdrz", c.sdrz}, {"sdscale", c.sdscale},
        {"st_rx", c.st_rx}, {"st_ry", c.st_ry}, {"st_rz", c.st_rz},
        {"sti_rx", c.sti_rx}, {"sti_ry", c.sti_ry}, {"sti_rz", c.sti_rz},
        {"s_thumb_str", c.s_thumb_str}, {"s_index_str", c.s_index_str},
        {"s_insert_dist", c.s_insert_dist},
        {"t_rx", c.t_rx}, {"t_ry", c.t_ry}, {"t_rz", c.t_rz},
        {"i_rx", c.i_rx}, {"i_ry", c.i_ry}, {"i_rz", c.i_rz},
        {"si_rx", c.si_rx}, {"si_ry", c.si_ry}, {"si_rz", c.si_rz},
        {"rack_z", c.rack_z}, {"rack_grab", c.rack_grab},
        {"dock_x", c.dock_x}, {"dock_y", c.dock_y}, {"dock_z", c.dock_z},
        {"rack_rx", c.rack_rx}, {"rack_ry", c.rack_ry}, {"rack_rz", c.rack_rz},
        {"insert_dist", c.insert_dist}, {"insert_dur", c.insert_dur},
        {"ip_joint", c.ip_joint}, {"ip_x", c.ip_x}, {"ip_y", c.ip_y}, {"ip_z", c.ip_z},
        {"insert_drop", c.insert_drop}, {"slide_joint", c.slide_joint},
        {"shell_ratio", c.shell_ratio},
    };

    re4vr::json_save(R9_CFG_PATH, o);
    r9_publish_ratio();
}

::REManagedObject* RE4VRReload2::rget_gun() {
    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");

    if (hu == nullptr) {
        return nullptr;
    }

    if (call_enum(hu, "get_EquipWeaponID").value_or(0) != R9) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
}

::REManagedObject* RE4VRReload2::rget_gun_mesh() {
    return re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_Mesh");
}

::REManagedObject* RE4VRReload2::rget_gun_tf() {
    auto* go = re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_GameObject");

    return re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
}

void RE4VRReload2::r9_play_sound(uint32_t id) {
    if (id == 0) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_GameObject");

    if (go != nullptr) {
        trigger_sound(go, id);
    }
}

// ---- Hand-Pose (EIGENE Daten, gestures-unabhaengig) ----
void RE4VRReload2::r9_apply_pose(const std::string& name, bool with_thumb, float blend) {
    const auto it = m_r9poses.find(name);

    if (it == m_r9poses.end() || blend <= 0.0f) {
        return;
    }

    if (!pose_apply(it->second, blend)) {
        return;
    }

    const auto& map = pose_map();
    const auto joint = [&](const char* n) -> ::REManagedObject* {
        const auto j = map.find(n);

        return (j != map.end()) ? j->second : nullptr;
    };

    const auto& c = m_r9cfg;

    // Daumen-/Zeigefinger-Spreizung additiv (Clip- und Einzelpatrone-Pose)
    if (with_thumb) {
        const bool single = (name == "Red9Single");

        // [STRECKEN] Einzelpatrone: Fingergelenke Richtung GERADE (Identitaet)
        // lerpen. 0 = Pose-Kruemmung, 1 = ganz gestreckt. Der Daumen wirkt auf
        // ALLE 3 Gelenke (das Euler-st_* dreht nur L_Thumb1 -> die Spitze blieb
        // krumm). Danach laeuft das Euler-Feintuning additiv obendrauf.
        if (single) {
            const auto straighten = [&](const char* bn, float amt) {
                amt *= blend;

                if (amt <= 0.0f) {
                    return;
                }

                auto* j = joint(bn);
                glm::quat cur{};

                if (j == nullptr || !get_quat(j, "get_LocalRotation", cur)) {
                    return;
                }

                glm::quat q{cur.w + (1.0f - cur.w) * amt, cur.x - cur.x * amt,
                            cur.y - cur.y * amt, cur.z - cur.z * amt};
                const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);

                if (len > 1e-6f) {
                    set_quat(j, "set_LocalRotation",
                             glm::quat{q.w / len, q.x / len, q.y / len, q.z / len});
                }
            };

            for (const char* bn : {"L_Thumb1", "L_Thumb2", "L_Thumb3"}) {
                straighten(bn, c.s_thumb_str);
            }

            for (const char* bn : {"L_IndexF1", "L_IndexF2", "L_IndexF3"}) {
                straighten(bn, c.s_index_str);
            }
        }

        // Daumen: die Einzelpatrone hat EIGENE Werte (st_*), sonst Clip (t_*)
        const float trx = (single ? c.st_rx : c.t_rx) * blend;
        const float try_ = (single ? c.st_ry : c.t_ry) * blend;
        const float trz = (single ? c.st_rz : c.t_rz) * blend;

        if (trx != 0.0f || try_ != 0.0f || trz != 0.0f) {
            auto* th = joint("L_Thumb1");
            glm::quat cur{};

            if (th != nullptr && get_quat(th, "get_LocalRotation", cur)) {
                set_quat(th, "set_LocalRotation",
                         glm::normalize(cur * quat_from_euler(trx, try_, trz)));
            }
        }

        // Zeigefinger: Clip nutzt i_*, die Einzelpatrone EIGENE sti_*
        const float irx = single ? c.sti_rx : c.i_rx;
        const float iry = single ? c.sti_ry : c.i_ry;
        const float irz = single ? c.sti_rz : c.i_rz;

        if (irx != 0.0f || iry != 0.0f || irz != 0.0f) {
            const glm::quat q = quat_from_euler(irx * blend, iry * blend, irz * blend);

            for (const char* bn : {"L_IndexF1", "L_IndexF2", "L_IndexF3"}) {
                auto* ix = joint(bn);
                glm::quat cur{};

                if (ix != nullptr && get_quat(ix, "get_LocalRotation", cur)) {
                    set_quat(ix, "set_LocalRotation", glm::normalize(cur * q));
                }
            }
        }
    }

    // Zeigefinger-Tuning der SLIDE-Pose (additiv auf alle 3 Glieder)
    if (name == "Red9Slide" && (c.si_rx != 0.0f || c.si_ry != 0.0f || c.si_rz != 0.0f)) {
        const glm::quat q = quat_from_euler(c.si_rx * blend, c.si_ry * blend,
                                            c.si_rz * blend);

        for (const char* bn : {"L_IndexF1", "L_IndexF2", "L_IndexF3"}) {
            auto* ix = joint(bn);
            glm::quat cur{};

            if (ix != nullptr && get_quat(ix, "get_LocalRotation", cur)) {
                set_quat(ix, "set_LocalRotation", glm::normalize(cur * q));
            }
        }
    }
}

// ---- Mesh-Klon (Munition) ----
// mode "strip" (Stripper-Clip, Parts 5/20/21, fuellt auf Max-Cap) bei loaded == 0,
// mode "single" (Einzelpatrone, Part 22, +1) bei loaded > 0. Gesetzt beim Griff.
void RE4VRReload2::r9_destroy() {
    if (m_r9clip.obj != nullptr) {
        re4vr::destroy_game_object(m_r9clip.obj);
    }

    m_r9clip.obj = nullptr;
    m_r9clip.mesh = nullptr;
    m_r9clip.parts_sig.clear();
    m_r9clip.parented = false;
}

bool RE4VRReload2::r9_spawn() {
    if (m_r9clip.obj != nullptr) {
        return true;
    }

    auto* gmesh = rget_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_red9_clip");

    if (go == nullptr) {
        return false;
    }

    auto* gom = reinterpret_cast<::REManagedObject*>(go);

    // KERN: baut das Skelett
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
    m_r9clip.obj = gom;
    m_r9clip.mesh = mesh;

    // [NO_LAG] nativ ans L_Hand-Joint parenten -> die Engine propagiert die
    // Transform VOR dem Skinning -> kein Render-Versatz beim Laufen.
    m_r9clip.parented = false;
    auto* ctf = re4vr::call_safe<::REManagedObject*>(gom, "get_Transform");
    auto* bt2 = body_tf();

    if (ctf != nullptr && bt2 != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", bt2);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_ParentJoint", jn);
            m_r9clip.parented = true;
        }
    }

    return true;
}

void RE4VRReload2::r9_isolate() {
    if (m_r9clip.mesh == nullptr) {
        return;
    }

    const bool single = (m_r9clip.mode == "single");
    const std::string src = single ? m_r9cfg.single_part : m_r9cfg.parts;
    const std::string sig = m_r9clip.mode + ":" + src;

    if (m_r9clip.parts_sig == sig) {
        return;
    }

    const auto keep = parse_parts(src);
    bool applied = true;

    for (int32_t i = 0; i <= 40; ++i) {
        if (!re4vr::obj_ok(m_r9clip.mesh)) {
            applied = false;

            break;
        }

        const bool on = std::find(keep.begin(), keep.end(), i) != keep.end();
        re4vr::call_safe<void*>(m_r9clip.mesh, "setPartsEnable", i, on);
    }

    if (applied) {
        m_r9clip.parts_sig = sig;
    }
}

// [DROP] reiner freier Fall beim Loslassen (wie die Pistolen: s = 1/2 g t^2,
// KEINE Drehung).
bool RE4VRReload2::r9_apply_drop() {
    if (!(m_r9drop.active && m_r9clip.obj != nullptr)) {
        return false;
    }

    const float t = static_cast<float>(clock_now() - m_r9drop.t0);

    if (t > RDROP_DUR) {
        m_r9drop.active = false;
        r9_destroy();

        return false;
    }

    if (!m_r9drop.snd && t >= RDROP_SND_DELAY) {   // Boden-Aufprall (verzoegert)
        m_r9drop.snd = true;
        r9_play_sound(R9SND_DROP);
    }

    const float fall = 0.5f * RGRAV * t * t;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform");

    if (tf == nullptr) {
        return true;
    }

    set_vec3(tf, "set_Position", glm::vec3{m_r9drop.sx, m_r9drop.sy - fall, m_r9drop.sz});
    const float s = m_r9cfg.dscale;
    set_vec3(tf, "set_LocalScale", glm::vec3{s, s, s});

    return true;
}

void RE4VRReload2::r9_start_drop() {
    auto* tf = (m_r9clip.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform") : nullptr;
    glm::vec3 p{};

    if (tf == nullptr || !get_vec3(tf, "get_Position", p)) {
        return;
    }

    m_r9drop.active = true;
    m_r9drop.snd = false;
    m_r9drop.sx = p.x;
    m_r9drop.sy = p.y;
    m_r9drop.sz = p.z;
    m_r9drop.t0 = clock_now();

    // [NO_LAG] Klon war ans L_Hand geparentet -> fuer den Welt-Freifall entkoppeln.
    if (m_r9clip.parented) {
        re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
        m_r9clip.parented = false;
    }
}

// ---- Slide-Rack (Joint _01): jederzeit ziehbar ----
::REManagedObject* RE4VRReload2::rget_slide() {
    auto* tf = rget_gun_tf();

    if (tf == nullptr) {
        m_r9rack.joint = nullptr;

        return nullptr;
    }

    auto* j = joint_by_name(tf, m_r9cfg.slide_joint);

    if (j != nullptr && !m_r9rack.rest_z.has_value()) {
        // [REST-KONSTANTE] Die Nulllage NIE live messen. Grund: rest_z wurde bei
        // jedem Unequip/Reset genullt und beim naechsten Ziehen aus dem
        // Live-Joint gelesen -> beim LEER-Ziehen steht der Slide (Engine-Empty-
        // Lock) schon hinten -> falsche Nulllage. Fester Wert im Code, BEWUSST
        // nicht ueber JSON kalibrierbar.
        m_r9rack.rest_z = R9_REST_Z;
    }

    m_r9rack.joint = j;

    return j;
}

void RE4VRReload2::rset_slide_z(float z) {
    auto* j = m_r9rack.joint;
    glm::vec3 lp{};

    if (j == nullptr || !get_vec3(j, "get_LocalPosition", lp)) {
        return;
    }

    set_vec3(j, "set_LocalPosition", glm::vec3{lp.x, lp.y, z});
}

// Hand an den Slide docken (arm_chain liest __vr_slide_hand_world_*). Publisht
// NUR den aktuellen Blend (kein Ramp hier -> kann pro Pass gerufen werden).
void RE4VRReload2::r9_publish_dock(::REManagedObject* j) {
    const float bl = m_r9rack.dock_blend;
    glm::vec3 p{};
    glm::quat r{};

    if (bl > 0.001f && j != nullptr && get_vec3(j, "get_Position", p)
        && get_quat(j, "get_Rotation", r)) {
        const auto& c = m_r9cfg;

        if (c.dock_x != 0.0f || c.dock_y != 0.0f || c.dock_z != 0.0f) {
            p += r * glm::vec3{c.dock_x, c.dock_y, c.dock_z};
        }

        if (c.rack_rx != 0.0f || c.rack_ry != 0.0f || c.rack_rz != 0.0f) {
            r = glm::normalize(r * quat_from_euler(c.rack_rx, c.rack_ry, c.rack_rz));
        }

        re4vr::lua_set_vec3("__vr_slide_hand_world_pos", p);
        re4vr::lua_set_quat("__vr_slide_hand_world_rot", r);
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", bl * bl * (3.0f - 2.0f * bl));

        return;
    }

    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
}

// Hand-Position in WAFFEN-LOKALEN Koordinaten (Z = Slide-Achse) -> immun gegen
// die Bewegung der Waffe in der Welt.
std::optional<glm::vec3> RE4VRReload2::r9_hand_gun_local(const glm::vec3& p) {
    auto* gtf = rget_gun_tf();
    glm::vec3 gpos{};
    glm::quat grot{};

    if (gtf == nullptr || !get_vec3(gtf, "get_Position", gpos)
        || !get_quat(gtf, "get_Rotation", grot)) {
        return std::nullopt;
    }

    // Konjugat (Einheits-Quat) = Inverse
    const glm::quat ginv{grot.w, -grot.x, -grot.y, -grot.z};

    return ginv * (p - gpos);
}

void RE4VRReload2::update_slide_rack() {
    // pro Frame frisch; nur waehrend Zug/Anim gesetzt -> im spaeten Pass geschrieben
    m_r9_apply_z.reset();
    auto* j = rget_slide();

    if (!(j != nullptr && m_r9rack.rest_z.has_value())) {
        m_r9rack.grabbed = false;
        m_r9rack.dock_blend = 0.0f;
        r9_publish_dock(nullptr);

        return;
    }

    const float rest_z = *m_r9rack.rest_z;
    const float maxpull = std::abs(m_r9cfg.rack_z);
    const float sgn = (m_r9cfg.rack_z < 0.0f) ? -1.0f : 1.0f;
    const float open_z = rest_z + sgn * maxpull;

    // [SETTLE] nach dem Loslassen: gleitet von der losgelassenen Position auf den
    // Latch-Wert (open_z/rest_z).
    if (m_r9rack.settling) {
        m_r9rack.grabbed = false;
        const float t = static_cast<float>(clock_now() - m_r9rack.settle_t0) / R9_CLOSE_DUR;

        if (t >= 1.0f) {
            m_r9rack.settling = false;
            m_r9_apply_z = m_r9rack.settle_to_z;
        } else {
            m_r9_apply_z = m_r9rack.settle_from_z
                + (m_r9rack.settle_to_z - m_r9rack.settle_from_z) * t;
        }

        m_r9rack.dock_blend = std::max(m_r9rack.dock_blend - 0.10f, 0.0f);
        m_r9rack._last_apply_z = m_r9_apply_z;
        r9_publish_dock(j);

        return;
    }

    // [CLIP-IN-HAND / INSERT] Clip in der Hand ODER Einleg-Animation laeuft ->
    // KEIN Slide-Grab/Dock. Wichtig: waehrend insert.active ist state.active
    // schon false, der Grip aber noch gehalten -> ohne diesen Skip wuerde der
    // Slide-Grab sofort waehrend des Einlegens zupacken.
    if (m_r9state.active || m_r9ins.active) {
        m_r9rack.grabbed = false;
        m_r9rack.dock_blend = 0.0f;

        if (m_r9rack.open) {
            m_r9_apply_z = open_z;
        }

        r9_publish_dock(nullptr);

        return;
    }

    const auto hp = left_hand_world_g();
    glm::vec3 sp{};
    const bool have_sp = get_vec3(j, "get_Position", sp);
    const bool grip = left_grip_down();

    // [RE-GRIP nach Insert] Grip-Loslassen loescht die Sperre (eine Variante).
    // Das Distanz-Clear unten ist die robuste: es greift auch bei DURCHGEHEND
    // gehaltenem Grip (Zwei-Hand-Aim) -> sonst haengt die Sperre.
    if (!grip) {
        m_r9rack._need_regrip = false;
    }

    if (hp.has_value() && have_sp) {
        const float dist = vec_len(vec_sub(*hp, sp));
        const float grab = m_r9cfg.rack_grab;

        // [NACHLADEN VOR CHOKE 16.09.2026 -- Ansage des Users] Steht die linke
        // Hand am Klappen-Greifpunkt (oder haelt sie ihn), gehoert der Grip der
        // Klappe. Zeitstempel statt Flag: tickt das hier nicht mehr (Waffe
        // gewechselt), laeuft die Sperre im Choke nach 0,15 s von selbst aus.
        if (dist <= grab || m_r9rack.grabbed) {
            re4vr::lua_set_number("__re4_lh_reload_grab_t", clock_now());
        }

        // [RE-GRIP Distanz-Clear] Die Sperre faellt, sobald die Hand den Slide
        // verlaesst. Direkt nach dem Einlegen ist die Hand noch am Port -> die
        // Sperre bleibt -> kein Sofort-Snap.
        if (m_r9rack._need_regrip && dist > grab * 1.5f) {
            m_r9rack._need_regrip = false;
        }

        if (!m_r9rack.grabbed) {
            // [SUPPORT VOR SLIDE] Haengt die linke Hand als Support-Hand an der
            // Waffe, darf ein Left-Grip NICHT zusaetzlich den Slide greifen --
            // sonst reisst dieselbe Hand gleichzeitig am Griff und am Verschluss.
            // Kein Latch: motion setzt __vr_support_hand_active jeden Frame neu.
            if (grip && dist <= grab && !m_r9rack._need_regrip
                && clock_now() >= m_r9rack._grab_cd
                && re4vr::lua_get_tribool("__vr_support_hand_active") != 1) {
                m_r9rack.grabbed = true;
                const auto l = r9_hand_gun_local(*hp);
                m_r9rack.anchor_z = l.has_value() ? std::optional<float>{l->z}
                                                  : std::nullopt;
                // Slide folgt der Hand AB der aktuellen Offenheit
                m_r9rack.grab_base = m_r9rack.open ? maxpull : 0.0f;
                m_r9rack._armed_up = !m_r9rack.open;   // zu -> Oeffnungs-Sound scharf
                m_r9rack._armed_dn = m_r9rack.open;    // offen -> Schliess-Sound scharf
            }
        } else if (grip && dist > grab * 3.0f) {
            // [HARD-SAFETY] Hand extrem weit -> Grab komplett loslassen
            m_r9rack.grabbed = false;
            m_r9_apply_z = m_r9rack.open ? open_z : rest_z;
        } else if (grip) {
            // Slide folgt der Hand live: target = Greif-Offenheit + Handzug
            // entlang der Waffen-Z-Achse.
            const auto l = r9_hand_gun_local(*hp);
            const float raw = (l.has_value() && m_r9rack.anchor_z.has_value())
                ? ((l->z - *m_r9rack.anchor_z) * sgn) : 0.0f;
            const float target = m_r9rack.grab_base + raw;
            const float amt = std::clamp(target, 0.0f, maxpull);
            const float frac = (maxpull > 0.0f) ? (amt / maxpull) : 0.0f;

            // Sounds als Flanken (jede Voll-Bewegung) -> auch bei mehrfachem
            // Rackern im selben Griff.
            if (frac >= 0.90f && m_r9rack._armed_up) {
                m_r9rack._armed_up = false;
                m_r9rack._armed_dn = true;
                r9_play_sound(R9SND_SLIDE_PULL);
            }

            if (frac <= 0.10f && m_r9rack._armed_dn) {
                m_r9rack._armed_dn = false;
                m_r9rack._armed_up = true;
                r9_play_sound(R9SND_SLIDE_CLOSE);
            }

            m_r9rack.open = (frac >= 0.5f);   // live: offen/zu (treibt den Fire-Block)
            m_r9_apply_z = rest_z + sgn * amt;
            // [UNDOCK bei Ueber-Reise] Controller weiter als der Slide kann ->
            // die Hand loest sich und geht zum Controller zurueck.
            const float over = std::max(0.0f, target - maxpull) + std::max(0.0f, -target);
            m_r9rack._dock_want = (over > R9_UNDOCK_OVER) ? 0.0f : 1.0f;
        } else {
            // LOSLASSEN -> auf den naechsten Latch-Wert gleiten (kein Snap)
            m_r9rack.grabbed = false;
            m_r9rack.settling = true;
            m_r9rack.settle_t0 = clock_now();
            m_r9rack.settle_from_z = m_r9rack._last_apply_z.value_or(
                m_r9rack.open ? open_z : rest_z);
            m_r9rack.settle_to_z = m_r9rack.open ? open_z : rest_z;
            m_r9_apply_z = m_r9rack.settle_from_z;
        }

        // OFFEN-Latch halten (ohne Griff)
        if (m_r9rack.open && !m_r9rack.grabbed && !m_r9rack.settling) {
            m_r9_apply_z = open_z;
        }
    }

    // OFFEN-Latch absichern (Hand-Welt diesen Frame nicht gelesen)
    if (m_r9rack.open && !m_r9rack.settling && !m_r9_apply_z.has_value()) {
        m_r9_apply_z = open_z;
    }

    // [PREVIEW] Tune-Modus
    if (m_r9rack.tune && !m_r9rack.grabbed) {
        m_r9_apply_z = open_z;
    }

    // [SLIDE-OWNERSHIP] Wir besitzen _01 VOLLSTAENDIG: solange unser Latch ZU ist
    // (apply_z noch leer = idle), forcen wir rest_z. Das ueberschreibt JEDE
    // Engine-Slide-Anim (Empty-Lock-Back bei 0 UND das offen-Haengen nach dem
    // Reload). Trade-off: die per-Schuss Slide-Recoil-Anim wird mit unterdrueckt.
    if (!m_r9_apply_z.has_value() && !m_r9rack.open) {
        m_r9_apply_z = rest_z;
    }

    // Dock-Blend: voll bei Griff (ausser Ueber-Reise -> 0); Tune -> voll; sonst 0
    const float want = m_r9rack.tune ? 1.0f
        : (m_r9rack.grabbed ? m_r9rack._dock_want : 0.0f);
    float bl = m_r9rack.dock_blend;

    if (bl < want) {
        bl = std::min(bl + 0.10f, want);
    } else if (bl > want) {
        bl = std::max(bl - 0.10f, want);
    }

    m_r9rack.dock_blend = bl;
    m_r9rack._last_apply_z = m_r9_apply_z;
    r9_publish_dock(j);
}

// no_dirty: eine selbst erzeugte Transform bleibt sonst in den gelockten Paessen
// haengen -> Zittern.
void RE4VRReload2::r9_set_tf(::REManagedObject* tf, const glm::vec3& p,
                             const std::optional<glm::quat>& rot, float s) {
    sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf),
                                Vector4f{p.x, p.y, p.z, 1.0f}, true);

    if (rot.has_value()) {
        sdk::set_transform_rotation(reinterpret_cast<::RETransform*>(tf), *rot);
    }

    set_vec3(tf, "set_LocalScale", glm::vec3{s, s, s});
}

void RE4VRReload2::r9_follow_to_hand() {
    if (m_r9clip.obj == nullptr) {
        return;
    }

    // Die Einzelpatrone (mode "single") hat EIGENE Offsets (sd*), sonst gilt der
    // Stripper-Clip (d*).
    const bool single = (m_r9clip.mode == "single");
    const auto& c = m_r9cfg;
    const glm::vec3 off{single ? c.sdx : c.dx, single ? c.sdy : c.dy,
                        single ? c.sdz : c.dz};
    const float orx = single ? c.sdrx : c.drx;
    const float ory = single ? c.sdry : c.dry;
    const float orz = single ? c.sdrz : c.drz;
    const float osc = single ? c.sdscale : c.dscale;
    auto* tf = re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    // [NO_LAG BAHN 2026-09-07] aus dem Waffen-Parent zurueck an die Hand.
    r9_clip_parent_mode("hand");

    // [NO_LAG] geparentet ans L_Hand: nur LOKALE Pose (der Offset war schon
    // hand-relativ = jetzt lokal).
    if (m_r9clip.parented) {
        set_vec3(tf, "set_LocalPosition", off);
        set_quat(tf, "set_LocalRotation", quat_from_euler(orx, ory, orz));
        set_vec3(tf, "set_LocalScale", glm::vec3{osc, osc, osc});

        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};
    glm::quat hr{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)
        || !get_quat(lh, "get_Rotation", hr)) {
        return;
    }

    r9_set_tf(tf, hp + (hr * off),
              glm::normalize(hr * quat_from_euler(orx, ory, orz)), osc);
}

// Dockport-Weltpos (Joint ip_joint + lokaler ip-Versatz) -- NUR fuer die
// Insert-Naehe (Trigger), nicht fuer die Bewegung.
std::optional<glm::vec3> RE4VRReload2::r9_dockport_world() {
    auto* tf = rget_gun_tf();

    if (tf == nullptr) {
        return std::nullopt;
    }

    auto* j = joint_by_name(tf, m_r9cfg.ip_joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{m_r9cfg.ip_x, m_r9cfg.ip_y, m_r9cfg.ip_z});
}

std::optional<int32_t> RE4VRReload2::r9_equip_type_main() {
    static const auto et = []() -> std::optional<int32_t> {
        auto* td = sdk::find_type_definition("chainsaw.EquipType");

        if (td == nullptr) {
            return std::nullopt;
        }

        auto* f = td->get_field("Main");

        return (f != nullptr) ? std::optional<int32_t>{f->get_data<int32_t>(nullptr)}
                              : std::nullopt;
    }();

    return et;
}

// STRIPPER-CLIP: fuellt die Waffe AUF MAX-CAP (need = cap - loaded), begrenzt
// durch die Reserve.
void RE4VRReload2::r9_fill_to_cap() {
    if (!m_r9cfg.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return;
    }

    auto* wi = get_live_wi();
    const int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(0);
    const int32_t cap = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;

    if (cap <= 0 || loaded >= cap) {
        return;
    }

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    const int32_t reserve = ammo_id.has_value() ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return;
    }

    // [NATIVER DECKEL -- GEMESSEN, das war der Bug] Bei unendlicher Munition
    // (Katzenohren) melden die beiden Reserve-Zaehler VERSCHIEDENE Werte:
    //   item_count_sum -> 999 (der Unendlich-Wert)
    //   getItemCountSum ->  12 (was die Engine wirklich zaehlt)
    // need = cap - loaded = 16 wurde nur gegen die 999 gedeckelt und blieb 16.
    // Der Ladeweg prueft aber selbst gegen getItemCountSum und steigt bei
    // `cnt < n` aus -> gar kein Laden und (weil der Sound an loaded_af > loaded
    // haengt) auch kein Einlegesound. Der Einzelpatronen-Weg fragt mit n = 1 an
    // und faellt deshalb nie auf.
    std::optional<int32_t> cnt_nat{};

    if (ammo_id.has_value()) {
        int32_t v = 0;

        if (re4vr::try_call<int32_t>(inv, "getItemCountSum(chainsaw.ItemID)", v, *ammo_id)) {
            cnt_nat = v;
        }
    }

    int32_t need = cap - loaded;

    if (need > reserve) {
        need = reserve;   // nicht mehr als Reserve da ist
    }

    if (cnt_nat.has_value() && *cnt_nat > 0 && need > *cnt_nat) {
        need = *cnt_nat;   // und nicht mehr, als der native Zaehler hergibt
    }

    if (const auto et = r9_equip_type_main(); et.has_value()) {
        // [CRASH-HARDEN] enableReloadItem-Gate gegen den null-Item-AV
        m_main->load_and_book(inv, *et, need, false);
    }

    if (call_enum(pe, "getCurrentGunAmmo").value_or(loaded) > loaded) {
        r9_play_sound(R9SND_MAG_INSERT);   // Sound nur bei echtem Nachladen
    }
}

// EINZELPATRONE: laedt WIRKLICH nur +1 (bzw. das Ratio). Genutzt bei loaded > 0.
void RE4VRReload2::r9_add_single() {
    if (!m_r9cfg.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return;
    }

    auto* wi = get_live_wi();
    const int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(0);
    const int32_t cap = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;

    if (cap > 0 && loaded >= cap) {
        return;
    }

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    const int32_t reserve = ammo_id.has_value() ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return;
    }

    // [SHELL-RATIO] Eine eingelegte Patrone bringt shell_ratio Schuss (1:1 / 1:2),
    // gedeckelt auf freie Kapazitaet und Reserve -- dasselbe Modell wie das
    // Shotgun-Ratio.
    int32_t want = std::max(1, m_r9cfg.shell_ratio);

    if (cap > 0 && want > (cap - loaded)) {
        want = cap - loaded;
    }

    if (want > reserve) {
        want = reserve;
    }

    if (want < 1) {
        want = 1;
    }

    if (const auto et = r9_equip_type_main(); et.has_value()) {
        m_main->load_and_book(inv, *et, want, false);
    }

    if (call_enum(pe, "getCurrentGunAmmo").value_or(loaded) > loaded) {
        r9_play_sound(R9SND_MAG_INSERT);
    }
}

// Holster nur greifbar, wenn der Slide OFFEN ist + Reserve da + nicht voll.
bool RE4VRReload2::r9_grab_allowed() {
    if (!m_r9rack.open || m_main == nullptr) {
        return false;
    }

    auto* pe = get_pe();
    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return false;
    }

    auto* wi = get_live_wi();
    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    const int32_t reserve = ammo_id.has_value() ? m_main->item_count_sum(inv, *ammo_id) : 0;

    if (reserve <= 0) {
        return false;
    }

    const int32_t loaded = call_enum(pe, "getCurrentGunAmmo").value_or(0);
    const int32_t cap = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;

    return !(cap > 0 && loaded >= cap);
}

// Naehe Clip-Hand -> Dockport bei OFFENEM Slide -> Slide-In starten.
void RE4VRReload2::r9_check_insert() {
    if (m_r9ins.active || !m_r9state.active || !m_r9rack.open) {
        return;
    }

    auto* bt = body_tf();
    auto* lh = (bt != nullptr) ? joint_by_name(bt, "L_Hand") : nullptr;
    glm::vec3 hp{};

    if (lh == nullptr || !get_vec3(lh, "get_Position", hp)) {
        return;
    }

    const auto dp = r9_dockport_world();

    if (!dp.has_value()) {
        return;
    }

    // [INSERT-DIST] Die Einzelpatrone hat einen EIGENEN Abstand.
    const float thr = (m_r9clip.mode == "single") ? m_r9cfg.s_insert_dist
                                                  : m_r9cfg.insert_dist;

    if (vec_len(vec_sub(hp, *dp)) > thr) {
        return;
    }

    // Start = die aktuelle Clip-Position (Hand) -> KEIN Sprung.
    auto* tf = (m_r9clip.obj != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform") : nullptr;
    glm::vec3 p = hp;

    if (tf != nullptr) {
        glm::vec3 q{};

        if (get_vec3(tf, "get_Position", q)) {
            p = q;
        }
    }

    m_r9ins.sx = p.x;   // Welt-Fallback
    m_r9ins.sy = p.y;
    m_r9ins.sz = p.z;

    // Start in WAFFEN-LOKALEN Koordinaten -> das Slide-In folgt der Waffen-Achse
    // (kippt mit der Waffe, nicht stur Welt-unten). Sonst faellt der Clip bei
    // geneigter Waffe daneben.
    if (const auto l = r9_hand_gun_local(p); l.has_value()) {
        m_r9ins.lx = l->x;
        m_r9ins.ly = l->y;
        m_r9ins.lz = l->z;
        m_r9ins.local_ok = true;
    } else {
        m_r9ins.local_ok = false;
    }

    m_r9ins.active = true;
    m_r9ins.t0 = clock_now();
    m_r9ins.kf_hold_t.reset();   // [KF_HOLD] neue Bahn
    // Clip nicht mehr an der Hand -> gleitet jetzt von oben in die Kammer
    m_r9state.active = false;

    // [NO_LAG] Klon war ans L_Hand geparentet -> fuer die Welt-Animation entkoppeln.
    if (m_r9clip.parented && tf != nullptr) {
        re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
        m_r9clip.parented = false;
    }

    // SOFORT: der Grip ist noch gehalten -> Slide-Grab erst nach Loslassen+Neugreifen
    m_r9rack._need_regrip = true;
}

// Slide-In-Animation: der Clip gleitet von der Start-Position gerade nach unten
// (waffen-lokal -Y), dann +1 und er verschwindet.
bool RE4VRReload2::r9_update_insert() {
    if (!m_r9ins.active) {
        return false;
    }

    if (m_r9clip.obj == nullptr) {
        m_r9ins.active = false;

        return false;
    }

    r9_isolate();

    // [SHELL-KEYFRAMES] Red9 mit Keyframe-Bahn? Zwei Modi = zwei virtuelle wids
    // (Einzelpatrone = 40021, Stripper-Clip = 4002).
    const int32_t kfwid = (m_r9clip.mode == "single") ? 40021 : 4002;
    const bool kf = (m_adv != nullptr) && m_adv->has_shell_keys(kfwid);
    const float dur = kf ? m_adv->shell_dur : m_r9cfg.insert_dur;
    float t = static_cast<float>(clock_now() - m_r9ins.t0) / std::max(dur, 0.01f);

    // [KF_LEAD] Im ersten Moment der Bahn blitzte der Clone woanders auf. Also
    // die ersten Frames fest auf Keyframe #1 stehen lassen -- Zeit auf 0 geklemmt
    // UND der Anlauf uebersprungen (der wuerde sonst genau hier von der HAND-Lage
    // aus blenden, also wieder von woanders).
    m_r9ins.kf_lead = false;

    if (kf && (clock_now() - m_r9ins.t0) < re4vr::lua_get_number("__re4_kf_lead_dur", 0.035)) {
        t = 0.0f;
        m_r9ins.kf_lead = true;
    }

    // [KF_HOLD] Bei Keyframe-Bahn den Clip nach dem Bahn-Ende noch kurz auf dem
    // LETZTEN Keyframe stehen lassen, bevor r9_destroy ihn wegnimmt -- sonst
    // blitzt er einen Frame an der alten Lage auf.
    bool fin = (t >= 1.0f);

    if (fin && kf) {
        if (!m_r9ins.kf_hold_t.has_value()) {
            m_r9ins.kf_hold_t = clock_now()
                + re4vr::lua_get_number("__re4_kf_hold_dur", 0.035);
        }

        if (clock_now() < *m_r9ins.kf_hold_t) {
            fin = false;
            t = 1.0f;
        } else {
            m_r9ins.kf_hold_t.reset();
        }
    }

    if (fin) {
        // Stripper-Clip -> auf Max-Cap fuellen; Einzelpatrone -> nur +1
        m_r9ins.active = false;

        if (m_r9clip.mode == "single") {
            r9_add_single();
        } else {
            r9_fill_to_cap();
        }

        r9_destroy();
        // [RE-GRIP -> COOLDOWN] Frueher blieb _need_regrip true bis Grip los/Hand
        // weg -> bei durchgehend gehaltenem Grip + Hand am Slide war der
        // Slide-Grab DAUERHAFT gesperrt. Die Einleg-Phase selbst ist schon ueber
        // den insert.active-Skip geschuetzt.
        m_r9rack._need_regrip = false;
        m_r9rack._grab_cd = clock_now() + m_r9cfg.regrip_cd;

        return true;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform");

    if (kf) {
        // Clip entlang der Keyframe-Bahn (relativ zur Waffe), inkl. Rotation. Die
        // Mesh-Parts haengen am selben Objekt -> ein Transform bewegt alle.
        auto* gtf = rget_gun_tf();
        glm::vec3 gp{};
        glm::quat gr{};

        if (tf != nullptr && gtf != nullptr && get_vec3(gtf, "get_Position", gp)
            && get_quat(gtf, "get_Rotation", gr)) {
            // [ANLAUF] Red9-Einzelpatrone (40021): weicher Anlauf von der ECHTEN
            // Start-Pos (Hand, waffen-lokal) zu Keyframe #1 -> kein Sprung.
            float anl = (kfwid == 40021) ? m_adv->r9_anlauf : 0.0f;

            if (m_r9ins.kf_lead) {
                anl = 0.0f;   // [KF_LEAD] im Vorlauf direkt Keyframe #1
            }

            RE4VRReloadAdv::Key k{};
            bool have = false;

            if (anl > 0.001f && m_r9ins.local_ok) {
                if (t < anl) {
                    RE4VRReloadAdv::Key k1{};

                    if (m_adv->shell_pose_at(kfwid, 0.0f, k1)) {
                        const float bf = t / anl;
                        k.x = m_r9ins.lx + (k1.x - m_r9ins.lx) * bf;
                        k.y = m_r9ins.ly + (k1.y - m_r9ins.ly) * bf;
                        k.z = m_r9ins.lz + (k1.z - m_r9ins.lz) * bf;
                        k.rx = k1.rx;
                        k.ry = k1.ry;
                        k.rz = k1.rz;
                        have = true;
                    }
                } else {
                    have = m_adv->shell_pose_at(kfwid, (t - anl) / std::max(1.0f - anl, 0.01f), k);
                }
            } else {
                have = m_adv->shell_pose_at(kfwid, t, k);
            }

            if (have) {
                // [MIT DER WAFFE WANDERN 2026-09-07] Vorher stand hier ein
                // WELT-Write auf den Clip -- und der haengt beim Tragen am
                // L_Hand-Joint. Die Engine leitet daraus eine lokale Pose ab und
                // schiebt den Clip anschliessend zusaetzlich mit der Hand mit;
                // ausserdem ist die Weltlage beim Rendern schon ueberholt, sobald
                // man laeuft. Genau das gemeldete Bild: "jeder Frame, den die
                // Keyframebahn nutzt, wandert nicht parallel zur Waffe mit".
                // Jetzt haengt der Clip fuer die Dauer der Bahn an der WAFFE und
                // bekommt die Keyframe-Lage als LOKALE Pose -- die Keyframes sind
                // ohnehin waffenrelativ, also wandert er von Keyframe 1 bis zum
                // letzten exakt mit ihr mit.
                r9_clip_parent_mode("weapon");

                set_vec3(tf, "set_LocalPosition", glm::vec3{k.x, k.y, k.z});
                set_quat(tf, "set_LocalRotation",
                         glm::normalize(quat_from_euler(k.rx, k.ry, k.rz)));
                set_vec3(tf, "set_LocalScale",
                         glm::vec3{m_r9cfg.dscale, m_r9cfg.dscale, m_r9cfg.dscale});
            }
        }

        return true;
    }

    // Gleitet entlang der WAFFEN-LOKALEN -Y-Achse nach unten (kippt mit der
    // Waffe) -> immer in die Kammer. Kein Bogen, kein Vorwaerts. Fallback = Welt.
    const float u = ease(t);
    const float drop = m_r9cfg.insert_drop * u;

    if (tf == nullptr) {
        return true;
    }

    std::optional<glm::vec3> pos{};

    if (m_r9ins.local_ok) {
        auto* gtf = rget_gun_tf();
        glm::vec3 gpos{};
        glm::quat grot{};

        if (gtf != nullptr && get_vec3(gtf, "get_Position", gpos)
            && get_quat(gtf, "get_Rotation", grot)) {
            pos = gpos + (grot * glm::vec3{m_r9ins.lx, m_r9ins.ly - drop, m_r9ins.lz});
        }
    }

    if (!pos.has_value()) {
        pos = glm::vec3{m_r9ins.sx, m_r9ins.sy - drop, m_r9ins.sz};
    }

    r9_set_tf(tf, *pos, std::nullopt, m_r9cfg.dscale);

    return true;
}

// ---- Hauptlogik: zeigt den Clip, solange aktiv (Griff) oder Vorschau ----
void RE4VRReload2::r9_update(bool show) {
    if (r9_update_insert()) {
        return;   // Slide-In laeuft -> Vorrang
    }

    if (show) {
        m_r9drop.active = false;

        if (m_r9clip.obj == nullptr && !r9_spawn()) {
            return;
        }

        r9_isolate();
        r9_follow_to_hand();
        r9_check_insert();   // nah am Dockport + Slide offen -> Slide-In starten

        return;
    }

    if (r9_apply_drop()) {
        return;   // laeuft ein Fall -> animieren
    }

    if (m_r9clip.obj != nullptr && !m_r9drop.active) {
        r9_destroy();
    }
}

bool RE4VRReload2::red9_set_in_hand(bool active) {
    if (active) {
        if (!m_r9state.active) {
            r9_play_sound(R9SND_MAG_GRAB);   // Flanke: einmal beim Greifen
            // Modus beim Greifen festlegen: leer (0) -> Stripper-Clip; sonst ->
            // Einzelpatrone.
            const int32_t loaded = call_enum(get_pe(), "getCurrentGunAmmo").value_or(0);
            m_r9clip.mode = (loaded <= 0) ? "strip" : "single";
        }

        m_r9state.active = true;

        return true;
    }

    if (m_r9state.active) {
        r9_start_drop();
    }

    m_r9state.active = false;

    return true;
}

void RE4VRReload2::red9_on_frame() {
    const auto ewid = get_equip_wid();

    if (!(m_r9cfg.enabled && ewid.has_value() && is_red9(*ewid))) {
        if (m_r9_prev_wid.has_value()) {
            m_r9state.active = false;
            m_r9state.preview = false;
            m_r9drop.active = false;
            m_r9ins.active = false;
            r9_destroy();

            if (m_r9rack.joint != nullptr && m_r9rack.rest_z.has_value()) {
                rset_slide_z(*m_r9rack.rest_z);
            }

            m_r9rack = Red9Rack{};
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
            re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
            re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:4593");
            re4vr::lua_set_bool("__vr_block_two_hand", false);
            // [FL_RACK] Flag beim Wegwechseln von Red9 freigeben
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
            // [LH_KNIFE-KILL] Clip-Flag freigeben (sonst bleibt der
            // Messer-Klon-Kill / Two-Hand-Yield haengen)
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            re4vr::lua_set_bool("__re4_reload_grab_empty", false);
            m_r9_prev_wid.reset();
        }

        return;
    }

    m_r9_prev_wid = ewid;
    // nativer Reload geblockt (B macht nichts)
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", true);
    update_slide_rack();   // Slide _01 jederzeit ziehbar

    // feuern nur bei GESCHLOSSENEM Slide
    re4vr::lua_set_bool("__vr_block_fire_when_empty", m_r9rack.open);
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:4607");

    // [SUPPORT-YIELD] Solange Red9 die linke Hand besitzt (Slide gegriffen/
    // Settle/Clip/Insert), weicht die geteilte Support-Hand -- sonst greift sie
    // die Hand zurueck an die Waffe und das Slide-Undock ist machtlos.
    re4vr::lua_set_bool("__vr_block_two_hand",
                        m_r9rack.grabbed || m_r9rack.settling || m_r9state.active
                        || m_r9ins.active);

    // [LH_KNIFE-KILL] Clip in der linken Hand ODER Insert-Anim -> das gemeinsame
    // "Mag in Hand"-Flag setzen, damit der linke Messer-Klon sofort verschwindet.
    re4vr::lua_set_bool("__vr_mag_in_hand", m_r9state.active || m_r9ins.active);

    // [FL_RACK] Slide gegriffen/Settle -> Flashlight ans HMD + FL-Mesh aus.
    re4vr::lua_set_bool("__vr_slide_rack_active", m_r9rack.grabbed || m_r9rack.settling);

    // [HOLSTER-GATE] Das Mag kommt NUR bei offenem Slide (+ Reserve, nicht voll)
    // aus dem Holster; sonst Sperr-Puls.
    const bool ga = r9_grab_allowed();
    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        !m_r9state.active && !m_r9ins.active && !ga);

    // [DRY-FIRE] RT gezogen waehrend gesperrt -> Klick auf der Flanke
    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_r9_dry_prev) {
        r9_play_sound(R9SND_DRY_FIRE);
    }

    m_r9_dry_prev = et;

    // [SHELL-KEYFRAMES] Keyframe-Tuning: der "einblenden"-Toggle (reload_adv)
    // zeigt den Clip an der Tuning-Lage. Der Modus kommt aus __re4_r9_kf_mode.
    // ui_wid = mode-abhaengige virtuelle wid (40021 single / 4002 strip).
    if (m_adv != nullptr && m_adv->shell_preview
        && (m_adv->is_keyframe_insert(4002) || m_adv->is_keyframe_insert(40021))
        && !m_r9state.active && !m_r9ins.active) {
        m_r9clip.mode = (re4vr::lua_get_string("__re4_r9_kf_mode") == "single")
            ? "single" : "strip";
        m_r9state.preview = true;
        m_r9_kf_preview = true;
        re4vr::lua_set_bool("__re4_r9_kf_preview", true);
    } else if (m_r9_kf_preview) {
        m_r9state.preview = false;
        m_r9_kf_preview = false;
        re4vr::lua_set_bool("__re4_r9_kf_preview", false);
    }

    re4vr::lua_set_number("__re4_reload_ui_wid",
                          (m_r9clip.mode == "single") ? 40021 : 4002);

    r9_update(m_r9state.active || m_r9state.preview);
}

// Spaeter Pass: Clip-Transform + Handpose setzen -> gewinnt gegen die
// Engine-Finger-Anim.
void RE4VRReload2::red9_apply_pass() {
    const auto ewid = get_equip_wid();

    if (!(m_r9cfg.enabled && ewid.has_value() && is_red9(*ewid))) {
        return;
    }

    // Hand-Dock pro Pass frisch (arm_chain liest hier)
    r9_publish_dock(m_r9rack.joint);

    // [LATE-PASS] Slide-Z hier schreiben -> die Engine clobbert nicht mehr
    if (m_r9_apply_z.has_value()) {
        rset_slide_z(*m_r9_apply_z);
    }

    std::string want{};
    bool wthumb = false;

    if (m_r9rack.grabbed || m_r9rack.tune) {
        want = "Red9Slide";   // linke Hand greift den Slide
        wthumb = false;
    } else if ((m_r9state.active || m_r9state.preview) && !m_r9drop.active) {
        // [SHELL-KEYFRAMES] Keyframe-Preview -> Clip an die Tuning-Lage (relativ
        // zur Waffe) statt an die Hand, damit man die Bahn am Desktop ausrichtet.
        if (m_r9_kf_preview && m_adv != nullptr && m_r9clip.obj != nullptr) {
            const auto& sl = m_adv->shell_live;
            auto* gtf = rget_gun_tf();
            glm::vec3 gp{};
            glm::quat gr{};
            auto* tf = re4vr::call_safe<::REManagedObject*>(m_r9clip.obj, "get_Transform");

            if (tf != nullptr && gtf != nullptr && get_vec3(gtf, "get_Position", gp)
                && get_quat(gtf, "get_Rotation", gr)) {
                if (m_r9clip.parented) {
                    re4vr::call_safe<void*>(tf, "set_Parent", nullptr);
                    m_r9clip.parented = false;
                }

                r9_set_tf(tf, gp + (gr * glm::vec3{sl.x, sl.y, sl.z}),
                          glm::normalize(gr * quat_from_euler(sl.rx, sl.ry, sl.rz)),
                          m_r9cfg.dscale);
            }
        } else {
            r9_follow_to_hand();
        }

        // Die Einzelpatrone nutzt EIGENE Pose Red9Single + st_*-Daumen
        want = (m_r9clip.mode == "single") ? "Red9Single" : "Red9Clip";
        wthumb = true;
    }

    // [POSE_FADE] beim Loslassen ueber POSE_FADE_DUR zurueckblenden statt snappen
    // (with_thumb ist in Lua die mitgefuehrte `data`).
    if (!want.empty()) {
        m_r9_fade.flag = wthumb;
    }

    float b = 0.0f;

    if (pose_fade_step(m_r9_fade, want, b)) {
        r9_apply_pose(m_r9_fade.name, m_r9_fade.flag, b);
    }
}

void RE4VRReload2::red9_on_script_reset() {
    m_r9state.active = false;
    m_r9state.preview = false;
    m_r9drop.active = false;
    m_r9ins.active = false;

    if (m_r9rack.joint != nullptr && m_r9rack.rest_z.has_value()) {
        rset_slide_z(*m_r9rack.rest_z);
    }

    m_r9rack = Red9Rack{};
    re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    re4vr::lua_set_string("__re4_bf_who", "re4_vr_reload2.lua:4674");
    re4vr::lua_set_bool("__vr_block_two_hand", false);
    re4vr::lua_set_bool("__vr_slide_rack_active", false);
    re4vr::lua_set_bool("__re4_reload_grab_empty", false);
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    r9_destroy();
    m_character_manager = nullptr;
}

// [NACKTES UI] Kopie des Ratio-Schalters fuers Public-Menue, direkt unter dem
// Shotgun-Ratio (das haengt auf Prioritaet 60, deshalb hier 61). Gleiche Wirkung
// wie der Schalter im Dev-Tree -- beide schreiben shell_ratio und speichern.
void RE4VRReload2::draw_public_red9_ratio() {
    g_framework->draw_menu_heading("Red9 Shell Ratio", true);   // [UEBERSCHRIFT 11.09.2026] war orangerot, linksbuendig
    const int32_t rr = std::max(1, m_r9cfg.shell_ratio);

    // [LINKSBUENDIG 11.09.2026] Reihe linksbuendig (war mittig), MENU_BUTTON_GAP dazwischen.
    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt die Wahl, die eckigen Klammern um die gewaehlte entfallen.
    const char* const label_1 = "1:1##nak_r9ratio1";
    const char* const label_2 = "1:2##nak_r9ratio2";

    if (g_framework->draw_menu_radio(label_1, rr == 1)) {
        m_r9cfg.shell_ratio = 1;
        red9_save_cfg();
    }

    ImGui::SameLine(0.0f, g_framework->menu_px(REFramework::MENU_BUTTON_GAP));

    if (g_framework->draw_menu_radio(label_2, rr == 2)) {
        m_r9cfg.shell_ratio = 2;
        red9_save_cfg();
    }
}

// ---------------------------------------------------------------------
// UI -- Red9
// ---------------------------------------------------------------------
void RE4VRReload2::red9_ui() {
    if (ImGui::Checkbox("##red9_en", &m_r9cfg.enabled)) {
        red9_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Red9 Reload (wp4002)");

    if (!ImGui::TreeNode("Red9 -- Einstellungen")) {
        return;
    }

    const auto awid = get_equip_wid();
    ImGui::Text("Equippt: %s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_red9(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Red9 erkannt - verwaltet)" : "  (keine Red9 equippt)");

    // [KLON-STATUS] Stripper-Clip und Einzelpatrone sind DASSELBE Klon-Objekt,
    // nur mit anderem Modus. Der einzige Unterschied, der ein Trailing erklaeren
    // kann, ist `parented`: true = nativ am L_Hand-Joint (die Engine propagiert
    // vor dem Skinning, kein Versatz), false = wir setzen jeden Frame die
    // WELT-Pose und das Mesh hinkt 3-4 Frames nach.
    ImGui::TextColored((m_r9clip.obj != nullptr && !m_r9clip.parented)
                           ? ImVec4{1.0f, 0.0f, 0.0f, 1.0f}
                           : ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "  Klon: %s | mode=%s | geparentet=%s",
                       m_r9clip.obj != nullptr ? "da" : "keiner",
                       m_r9clip.mode.c_str(),
                       m_r9clip.parented ? "true" : "false");

    auto& c = m_r9cfg;

    if (ImGui::TreeNode("Munition (Mesh-Klon) Position")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Clip an der Hand anzeigen##rprev", &m_r9state.preview);
        char pb[64]{};
        std::snprintf(pb, sizeof(pb), "%s", c.parts.c_str());

        if (ImGui::InputText("Stripper-Clip Parts##rparts", pb, sizeof(pb))) {
            c.parts = pb;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Default 5,20,21 = Clip + Kugeln; erscheint nur bei Ammo 0)");
        char sb[32]{};
        std::snprintf(sb, sizeof(sb), "%s", c.single_part.c_str());

        if (ImGui::InputText("Einzelpatrone Part##rsingp", sb, sizeof(sb))) {
            c.single_part = sb;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Default 22 = einzelne Patrone; erscheint bei Ammo > 0, "
                           "laedt +1)");

        if (ImGui::Checkbox("Vorschau zeigt Einzelpatrone (statt Clip)##rprevs",
                            &m_r9state.preview_single)) {
            m_r9clip.mode = m_r9state.preview_single ? "single" : "strip";
        }

        ch |= ImGui::SliderFloat("Pos X##rdx", &c.dx, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Y##rdy", &c.dy, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Z##rdz", &c.dz, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Rot X##rrx", &c.drx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Y##rry", &c.dry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Z##rrz", &c.drz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Scale##rsc", &c.dscale, 0.05f, 3.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Daumen (additiv, Pose):");
        ch |= ImGui::SliderFloat("Daumen RotX##rtx", &c.t_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##rty", &c.t_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##rtz", &c.t_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Zeigefinger (additiv, Pose):");
        ch |= ImGui::SliderFloat("Zeige RotX##rix", &c.i_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Zeige RotY##riy", &c.i_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Zeige RotZ##riz", &c.i_rz, -90.0f, 90.0f);

        if (ch) {
            red9_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Einzelpatrone (Part 22) Position")) {
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Eigener Offset + Daumen-Tuning (mode 'single', Ammo > 0).");
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "Vorschau: Checkbox 'zeigt Einzelpatrone' im Munitions-Tree.");
        bool ch = false;
        ch |= ImGui::SliderFloat("Pos X##rsdx", &c.sdx, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Y##rsdy", &c.sdy, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Pos Z##rsdz", &c.sdz, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Rot X##rsdrx", &c.sdrx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Y##rsdry", &c.sdry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Rot Z##rsdrz", &c.sdrz, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Scale##rsdsc", &c.sdscale, 0.05f, 3.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Daumen (additiv, Pose):");
        ch |= ImGui::SliderFloat("Daumen RotX##rstx", &c.st_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotY##rsty", &c.st_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Daumen RotZ##rstz", &c.st_rz, -90.0f, 90.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Zeigefinger (additiv, Pose):");
        ch |= ImGui::SliderFloat("Zeigefinger RotX##rstix", &c.sti_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Zeigefinger RotY##rstiy", &c.sti_ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Zeigefinger RotZ##rstiz", &c.sti_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Strecken (0 = Pose-Kruemmung, 1 = gerade):");
        ch |= ImGui::SliderFloat("Daumen strecken##rsthstr", &c.s_thumb_str, 0.0f, 1.0f);
        ch |= ImGui::SliderFloat("Zeigefinger strecken##rsixstr", &c.s_index_str, 0.0f, 1.0f);

        if (ch) {
            red9_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Slide-Rack (Joint _01)")) {
        bool ch = false;
        ImGui::Checkbox("Vorschau: Hand am Slide andocken (zum Tunen)##rdocktune",
                        &m_r9rack.tune);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "  (Slide gezogen + Hand angedockt OHNE Greifen -> "
                           "Dock/Rot-Slider justieren)");
        char jb[32]{};
        std::snprintf(jb, sizeof(jb), "%s", c.slide_joint.c_str());

        if (ImGui::InputText("Slide-Joint##rslj", jb, sizeof(jb))) {
            c.slide_joint = jb;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Z-Hub (negativ=zurueck)##rrz2", &c.rack_z, -0.10f, 0.10f);
        ch |= ImGui::SliderFloat("Greif-Distanz m##rrg", &c.rack_grab, 0.05f, 0.30f);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "Linke Hand nah an _01 + Grip -> Slide-Pose + Zurueckziehen.");
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Hand-Dock am Slide (Position relativ zum Joint):");
        ch |= ImGui::SliderFloat("Dock X##rdkx", &c.dock_x, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Dock Y##rdky", &c.dock_y, -0.30f, 0.30f);
        ch |= ImGui::SliderFloat("Dock Z##rdkz", &c.dock_z, -0.30f, 0.30f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Hand-Rotation am Slide (Grad):");
        ch |= ImGui::SliderFloat("Hand RotX##rkrx", &c.rack_rx, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotY##rkry", &c.rack_ry, -180.0f, 180.0f);
        ch |= ImGui::SliderFloat("Hand RotZ##rkrz", &c.rack_rz, -180.0f, 180.0f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "Zeigefinger Slide-Pose (additiv):");
        ch |= ImGui::SliderFloat("Zeige RotX##rsix", &c.si_rx, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Zeige RotY##rsiy", &c.si_ry, -90.0f, 90.0f);
        ch |= ImGui::SliderFloat("Zeige RotZ##rsiz", &c.si_rz, -90.0f, 90.0f);

        if (ch) {
            red9_save_cfg();
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Mag einlegen (Insert)")) {
        bool ch = ImGui::Checkbox("Laedt echte Munition##rrlda", &c.reload_ammo);
        ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                           "Slide OFFEN + nah am Dockport -> Muni gleitet in die Kammer.");
        ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                           "Ammo 0 -> Stripper-Clip fuellt auf Max-Cap. "
                           "Ammo > 0 -> Einzelpatrone +1.");
        // [SHELL-RATIO] Wie beim Shotgun-Ratio: eine Einzelpatrone zaehlt 1 oder 2.
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Red9 Shell Ratio (Schuss pro eingelegter Einzelpatrone):");
        const int32_t rr = std::max(1, c.shell_ratio);

        if (ImGui::Button((rr == 1) ? "[1:1]##r9ratio1" : " 1:1 ##r9ratio1")) {
            c.shell_ratio = 1;
            ch = true;
        }

        ImGui::SameLine();

        if (ImGui::Button((rr == 2) ? "[1:2]##r9ratio2" : " 1:2 ##r9ratio2")) {
            c.shell_ratio = 2;
            ch = true;
        }

        ch |= ImGui::SliderFloat("Insert-Distanz Clip m##rinsd", &c.insert_dist, 0.03f, 0.30f);
        ch |= ImGui::SliderFloat("Insert-Distanz Einzelpatrone m##rinsds",
                                 &c.s_insert_dist, 0.03f, 0.30f);
        ch |= ImGui::SliderFloat("Slide-In-Dauer s##rinsdur", &c.insert_dur, 0.10f, 2.0f);
        char ib[32]{};
        std::snprintf(ib, sizeof(ib), "%s", c.ip_joint.c_str());

        if (ImGui::InputText("Dock-Joint##ripj", ib, sizeof(ib))) {
            c.ip_joint = ib;
            ch = true;
        }

        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Dockport = Joint + lokaler Versatz (Default _03, "
                           "Z=0.054 = Muni-Eintritt):");
        ch |= ImGui::SliderFloat("Dockport X##ripx", &c.ip_x, -0.50f, 0.50f);
        ch |= ImGui::SliderFloat("Dockport Y##ripy", &c.ip_y, -0.50f, 0.50f);
        ch |= ImGui::SliderFloat("Dockport Z##ripz", &c.ip_z, -0.50f, 0.50f);
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "TOP-LOADER: Clip gleitet GERADE nach unten (Welt -Y), kein Bogen.");
        ch |= ImGui::SliderFloat("Slide-In Strecke runter m##rinsdrop",
                                 &c.insert_drop, 0.02f, 0.40f);

        if (ch) {
            red9_save_cfg();
        }

        ImGui::TreePop();
    }

    ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                       "Nativer Reload ist geblockt. Holster greifen -> Clip in der Hand.");
    ImGui::TreePop();
}

// ============================================================================
// Init / Hooks / Lua-Zustand / Dispatch
// ============================================================================
RE4VRReload2* RE4VRReload2::s_instance{nullptr};

void RE4VRReload2::on_initialize() {
    s_instance = this;

    // ---- 1 REVOLVER: Daten pro Waffe (CODE-Konstanten) ----------------
    // [CYLINDER] ZWEI Mechaniken, je Waffe die passende: rx/ry/rz = Rotations-
    // Offset (Crane-Dreh-Revolver), px/py/pz = LocalPosition-Offset (die Trommel
    // faehrt translatorisch raus).
    {
        auto& c = cyl_cfg(5001);
        c.ry = -45.0f;
        c.lerp = 0.08f;
    }
    {
        // Broken Butterfly: TOP-BREAK, _04 klappt nach OBEN auf (Pitch/X).
        auto& c = cyl_cfg(4500);
        c.rx = 52.0f;
        c.lerp = 0.08f;
    }
    {
        auto& h = hammer_cfg(4500);
        h.idle_rx = -25.5f;
        h.rx = -41.5f;
        h.lerp = 0.25f;
    }
    {
        // [SHELL-IN-HAND] Eigene Pose "RevolverShell" (unabhaengige Kopie von
        // LE5SWITCH in reload.json, damit das Tuning NIE die LE5 trifft).
        // Greifpose vom Handcannon uebernommen (dort besser getunt).
        auto& s = shell_cfg(4500);
        s.pose = "RevolverShell";
        s.x = 0.022f;
        s.y = -0.078f;
        s.z = 0.067f;
        s.rx = 34.0f;
        s.ry = 114.0f;
        s.rz = 0.0f;
        s.t_rx = 26.471f;
        s.t_ry = 13.614f;
        s.t_rz = 14.370f;
        s.i_rx = -9.076f;
        s.i_ry = 13.613f;
        s.i_rz = 30.252f;
        s.parts = "20,30";
        s.scale = 1.0f;
    }

    // ---- 2 RIFLE: Hand-Posen (EIGENE Daten-Kopie, gestures-unabhaengig) --
    m_rposes["StingrayMag"] = {
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

    m_rposes["StingraySlide"] = {
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

    m_rposes["StingraySwitch"] = {
        {"L_IndexF1", glm::quat{0.924900f, 0.000000f, -0.015000f, -0.380100f}},
        {"L_IndexF2", glm::quat{0.819200f, 0.000000f, 0.000000f, -0.573600f}},
        {"L_IndexF3", glm::quat{0.866000f, 0.000000f, 0.000000f, -0.500000f}},
        {"L_MiddleF1", glm::quat{0.896271f, -0.104853f, 0.085188f, -0.422431f}},
        {"L_MiddleF2", glm::quat{0.705958f, 0.000000f, 0.000000f, -0.708254f}},
        {"L_MiddleF3", glm::quat{0.890564f, 0.000000f, 0.000000f, -0.454858f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_PinkyF1", glm::quat{0.922581f, -0.182263f, 0.075602f, -0.331525f}},
        {"L_PinkyF2", glm::quat{0.722647f, 0.000000f, 0.000000f, -0.691217f}},
        {"L_PinkyF3", glm::quat{0.896982f, 0.000000f, 0.000000f, -0.442068f}},
        {"L_RingF1", glm::quat{0.879959f, -0.133028f, 0.090023f, -0.447069f}},
        {"L_RingF2", glm::quat{0.710167f, 0.000000f, 0.000000f, -0.704033f}},
        {"L_RingF3", glm::quat{0.892185f, 0.000000f, 0.000000f, -0.451670f}},
        {"L_Thumb1", glm::quat{0.918569f, 0.252866f, -0.108586f, -0.283724f}},
        {"L_Thumb2", glm::quat{0.999635f, 0.000000f, -0.027026f, 0.000000f}},
        {"L_Thumb3", glm::quat{0.923391f, -0.000011f, 0.383861f, -0.000004f}},
    };

    m_rposes["CqbrSlide"] = {
        {"L_IndexF1", glm::quat{0.986822f, 0.041360f, 0.006551f, -0.156297f}},
        {"L_IndexF2", glm::quat{0.800622f, 0.0f, 0.0f, -0.599170f}},
        {"L_IndexF3", glm::quat{0.976009f, 0.0f, 0.0f, -0.217732f}},
        {"L_MiddleF1", glm::quat{0.924938f, -0.023124f, 0.000451f, -0.379414f}},
        {"L_MiddleF2", glm::quat{0.833468f, 0.0f, 0.0f, -0.552567f}},
        {"L_MiddleF3", glm::quat{0.957340f, 0.0f, 0.0f, -0.288964f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.864541f, -0.060455f, -0.034802f, -0.497697f}},
        {"L_PinkyF2", glm::quat{0.905133f, 0.0f, 0.0f, -0.425128f}},
        {"L_PinkyF3", glm::quat{0.935618f, 0.0f, 0.0f, -0.353014f}},
        {"L_RingF1", glm::quat{0.877944f, -0.024523f, -0.013350f, -0.477948f}},
        {"L_RingF2", glm::quat{0.893334f, 0.0f, 0.0f, -0.449392f}},
        {"L_RingF3", glm::quat{0.933580f, 0.0f, 0.0f, -0.358368f}},
        {"L_Thumb1", glm::quat{0.991438f, 0.108579f, -0.021318f, -0.069329f}},
        {"L_Thumb2", glm::quat{0.935394f, 0.0f, -0.353608f, 0.0f}},
        {"L_Thumb3", glm::quat{1.0f, 0.0f, -0.000215f, 0.0f}},
    };

    // ---- 3 BOLT-ACTION: Hand-Posen ------------------------------------
    m_bposes["TMPSUpport"] = {
        {"L_IndexF1", glm::quat{0.804279f, -0.055429f, -0.040679f, -0.590261f}},
        {"L_IndexF2", glm::quat{0.788875f, 0.0f, 0.0f, -0.614554f}},
        {"L_IndexF3", glm::quat{0.922189f, 0.0f, 0.0f, -0.386738f}},
        {"L_MiddleF1", glm::quat{0.797521f, -0.117887f, -0.086517f, -0.585302f}},
        {"L_MiddleF2", glm::quat{0.788875f, 0.0f, 0.0f, -0.614554f}},
        {"L_MiddleF3", glm::quat{0.922189f, 0.0f, 0.0f, -0.386738f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.840226f, -0.199592f, -0.116520f, -0.490516f}},
        {"L_PinkyF2", glm::quat{0.931932f, 0.0f, 0.0f, -0.362634f}},
        {"L_PinkyF3", glm::quat{0.814746f, 0.0f, 0.0f, -0.579818f}},
        {"L_RingF1", glm::quat{0.796371f, -0.132487f, -0.096843f, -0.582119f}},
        {"L_RingF2", glm::quat{0.843908f, 0.0f, 0.0f, -0.536488f}},
        {"L_RingF3", glm::quat{0.843411f, 0.0f, 0.0f, -0.537269f}},
        {"L_Thumb1", glm::quat{0.949122f, 0.196826f, -0.043902f, -0.241868f}},
        {"L_Thumb2", glm::quat{0.996365f, 0.0f, -0.085182f, 0.0f}},
        {"L_Thumb3", glm::quat{0.941245f, 0.0f, 0.337725f, 0.0f}},
    };

    m_bposes["Shotgunshell"] = {
        {"L_IndexF1", glm::quat{0.986822f, 0.041360f, 0.006551f, -0.156297f}},
        {"L_IndexF2", glm::quat{0.800622f, 0.0f, 0.0f, -0.599170f}},
        {"L_IndexF3", glm::quat{0.970822f, 0.0f, 0.0f, -0.239802f}},
        {"L_MiddleF1", glm::quat{0.973517f, -0.022758f, 0.004124f, -0.227444f}},
        {"L_MiddleF2", glm::quat{0.752896f, 0.0f, 0.0f, -0.658139f}},
        {"L_MiddleF3", glm::quat{0.949174f, 0.0f, 0.0f, -0.314753f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.900387f, -0.062961f, -0.030031f, -0.429462f}},
        {"L_PinkyF2", glm::quat{0.861629f, 0.0f, 0.0f, -0.507538f}},
        {"L_PinkyF3", glm::quat{0.934950f, 0.0f, 0.0f, -0.354780f}},
        {"L_RingF1", glm::quat{0.939326f, -0.026238f, -0.009550f, -0.341887f}},
        {"L_RingF2", glm::quat{0.819152f, 0.0f, 0.0f, -0.573577f}},
        {"L_RingF3", glm::quat{0.921309f, 0.0f, 0.0f, -0.388830f}},
        {"L_Thumb1", glm::quat{0.991438f, 0.108579f, -0.021318f, -0.069329f}},
        {"L_Thumb2", glm::quat{0.935394f, 0.0f, -0.353608f, 0.0f}},
        {"L_Thumb3", glm::quat{0.998456f, 0.0f, -0.055552f, 0.0f}},
    };

    m_bposes["RiotSLide"] = {
        {"L_IndexF1", glm::quat{0.986822f, 0.041360f, 0.006551f, -0.156297f}},
        {"L_IndexF2", glm::quat{0.800622f, 0.0f, 0.0f, -0.599170f}},
        {"L_IndexF3", glm::quat{0.976009f, 0.0f, 0.0f, -0.217732f}},
        {"L_MiddleF1", glm::quat{0.924938f, -0.023124f, 0.000451f, -0.379414f}},
        {"L_MiddleF2", glm::quat{0.833468f, 0.0f, 0.0f, -0.552567f}},
        {"L_MiddleF3", glm::quat{0.957340f, 0.0f, 0.0f, -0.288964f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.864541f, -0.060455f, -0.034802f, -0.497697f}},
        {"L_PinkyF2", glm::quat{0.905133f, 0.0f, 0.0f, -0.425128f}},
        {"L_PinkyF3", glm::quat{0.935618f, 0.0f, 0.0f, -0.353014f}},
        {"L_RingF1", glm::quat{0.877944f, -0.024523f, -0.013350f, -0.477948f}},
        {"L_RingF2", glm::quat{0.893334f, 0.0f, 0.0f, -0.449392f}},
        {"L_RingF3", glm::quat{0.933580f, 0.0f, 0.0f, -0.358368f}},
        {"L_Thumb1", glm::quat{0.991438f, 0.108579f, -0.021318f, -0.069329f}},
        {"L_Thumb2", glm::quat{0.935394f, 0.0f, -0.353608f, 0.0f}},
        {"L_Thumb3", glm::quat{1.0f, 0.0f, -0.000215f, 0.0f}},
    };

    // ---- 4 ARMBRUST: XbowBolt = unabhaengige Kopie der LE5SWITCH-Pose ---
    m_xbow_pose = {
        {"L_IndexF1", glm::quat{0.924900f, 0.000000f, -0.015000f, -0.380100f}},
        {"L_IndexF2", glm::quat{0.819200f, 0.0f, 0.0f, -0.573600f}},
        {"L_IndexF3", glm::quat{0.866000f, 0.0f, 0.0f, -0.500000f}},
        {"L_MiddleF1", glm::quat{0.896271f, -0.104853f, 0.085188f, -0.422431f}},
        {"L_MiddleF2", glm::quat{0.705958f, 0.0f, 0.0f, -0.708254f}},
        {"L_MiddleF3", glm::quat{0.890564f, 0.0f, 0.0f, -0.454858f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_PinkyF1", glm::quat{0.922581f, -0.182263f, 0.075602f, -0.331525f}},
        {"L_PinkyF2", glm::quat{0.722647f, 0.0f, 0.0f, -0.691217f}},
        {"L_PinkyF3", glm::quat{0.896982f, 0.0f, 0.0f, -0.442068f}},
        {"L_RingF1", glm::quat{0.879959f, -0.133028f, 0.090023f, -0.447069f}},
        {"L_RingF2", glm::quat{0.710167f, 0.0f, 0.0f, -0.704033f}},
        {"L_RingF3", glm::quat{0.892185f, 0.0f, 0.0f, -0.451670f}},
        {"L_Thumb1", glm::quat{0.918569f, 0.252866f, -0.108586f, -0.283724f}},
        {"L_Thumb2", glm::quat{0.999635f, 0.000000f, -0.027026f, 0.000000f}},
        {"L_Thumb3", glm::quat{0.923391f, -0.000011f, 0.383861f, -0.000004f}},
    };
    // ---- 5 BOWDRAW: "bow-aim" (RECHTE Hand) + "bowknob" (LINKE Hand) ----
    m_bow_aim = {
        {"R_Thumb1", glm::quat{0.814636f, 0.551252f, 0.094716f, 0.153359f}},
        {"R_Thumb2", glm::quat{0.999996f, 0.000000f, -0.002895f, 0.000000f}},
        {"R_Thumb3", glm::quat{0.954990f, 0.000000f, -0.296638f, 0.000000f}},
        {"R_IndexF1", glm::quat{0.997332f, -0.055451f, -0.010740f, 0.046255f}},
        {"R_IndexF2", glm::quat{0.991201f, 0.000000f, 0.000000f, 0.132368f}},
        {"R_IndexF3", glm::quat{0.965995f, 0.000000f, 0.000000f, 0.258561f}},
        {"R_MiddleF1", glm::quat{0.935744f, -0.083707f, -0.134808f, 0.314966f}},
        {"R_MiddleF2", glm::quat{0.854313f, 0.000000f, 0.000000f, 0.519758f}},
        {"R_MiddleF3", glm::quat{0.894890f, 0.000000f, 0.000000f, 0.446287f}},
        {"R_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"R_RingF1", glm::quat{0.927954f, -0.114565f, -0.146346f, 0.323047f}},
        {"R_RingF2", glm::quat{0.862690f, 0.000000f, 0.000000f, 0.505733f}},
        {"R_RingF3", glm::quat{0.937282f, 0.000000f, 0.000000f, 0.348572f}},
        {"R_PinkyF1", glm::quat{0.933248f, -0.171540f, -0.140775f, 0.282495f}},
        {"R_PinkyF2", glm::quat{0.901650f, 0.000000f, 0.000000f, 0.432467f}},
        {"R_PinkyF3", glm::quat{0.933580f, 0.000000f, 0.000000f, 0.358368f}},
    };

    m_bowknob = {
        {"L_Thumb1", glm::quat{0.895934f, 0.383155f, -0.135232f, -0.179464f}},
        {"L_Thumb2", glm::quat{0.995837f, 0.000000f, 0.091154f, 0.000000f}},
        {"L_Thumb3", glm::quat{0.980407f, 0.000000f, 0.196984f, 0.000000f}},
        {"L_IndexF1", glm::quat{0.818077f, 0.034288f, 0.024040f, -0.573582f}},
        {"L_IndexF2", glm::quat{0.815552f, 0.000000f, 0.000000f, -0.578684f}},
        {"L_IndexF3", glm::quat{0.939552f, 0.000000f, 0.000000f, -0.342406f}},
        {"L_MiddleF1", glm::quat{0.764046f, -0.038817f, 0.007399f, -0.643951f}},
        {"L_MiddleF2", glm::quat{0.829961f, 0.000000f, 0.000000f, -0.557821f}},
        {"L_MiddleF3", glm::quat{0.840210f, 0.000000f, 0.000000f, -0.542261f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_RingF1", glm::quat{0.767537f, -0.059700f, 0.024085f, -0.637764f}},
        {"L_RingF2", glm::quat{0.815125f, 0.000000f, 0.000000f, -0.579286f}},
        {"L_RingF3", glm::quat{0.848766f, 0.000000f, 0.000000f, -0.528768f}},
        {"L_PinkyF1", glm::quat{0.760233f, -0.119328f, -0.002517f, -0.638592f}},
        {"L_PinkyF2", glm::quat{0.877568f, 0.000000f, 0.000000f, -0.479453f}},
        {"L_PinkyF3", glm::quat{0.830947f, 0.000000f, 0.000000f, -0.556351f}},
    };

    // ---- 6 RED9: Hand-Posen -------------------------------------------
    m_r9poses["Red9Clip"] = {
        {"L_IndexF1", glm::quat{0.924900f, 0.000000f, -0.015000f, -0.380100f}},
        {"L_IndexF2", glm::quat{0.819200f, 0.0f, 0.0f, -0.573600f}},
        {"L_IndexF3", glm::quat{0.866000f, 0.0f, 0.0f, -0.500000f}},
        {"L_MiddleF1", glm::quat{0.896271f, -0.104853f, 0.085188f, -0.422431f}},
        {"L_MiddleF2", glm::quat{0.705958f, 0.0f, 0.0f, -0.708254f}},
        {"L_MiddleF3", glm::quat{0.890564f, 0.0f, 0.0f, -0.454858f}},
        {"L_Palm", glm::quat{1.000000f, 0.000000f, 0.000000f, 0.000000f}},
        {"L_PinkyF1", glm::quat{0.922581f, -0.182263f, 0.075602f, -0.331525f}},
        {"L_PinkyF2", glm::quat{0.722647f, 0.0f, 0.0f, -0.691217f}},
        {"L_PinkyF3", glm::quat{0.896982f, 0.0f, 0.0f, -0.442068f}},
        {"L_RingF1", glm::quat{0.879959f, -0.133028f, 0.090023f, -0.447069f}},
        {"L_RingF2", glm::quat{0.710167f, 0.0f, 0.0f, -0.704033f}},
        {"L_RingF3", glm::quat{0.892185f, 0.0f, 0.0f, -0.451670f}},
        {"L_Thumb1", glm::quat{0.918569f, 0.252866f, -0.108586f, -0.283724f}},
        {"L_Thumb2", glm::quat{0.999635f, 0.000000f, -0.027026f, 0.000000f}},
        {"L_Thumb3", glm::quat{0.923391f, -0.000011f, 0.383861f, -0.000004f}},
    };

    m_r9poses["Red9Slide"] = {
        {"L_IndexF1", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_IndexF2", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_IndexF3", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_MiddleF1", glm::quat{0.896271f, -0.104853f, 0.085188f, -0.422431f}},
        {"L_MiddleF2", glm::quat{0.705958f, 0.0f, 0.0f, -0.708254f}},
        {"L_MiddleF3", glm::quat{0.890564f, 0.0f, 0.0f, -0.454858f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.922581f, -0.182263f, 0.075602f, -0.331525f}},
        {"L_PinkyF2", glm::quat{0.722647f, 0.0f, 0.0f, -0.691217f}},
        {"L_PinkyF3", glm::quat{0.896982f, 0.0f, 0.0f, -0.442068f}},
        {"L_RingF1", glm::quat{0.879959f, -0.133028f, 0.090023f, -0.447069f}},
        {"L_RingF2", glm::quat{0.710167f, 0.0f, 0.0f, -0.704033f}},
        {"L_RingF3", glm::quat{0.892185f, 0.0f, 0.0f, -0.451670f}},
        {"L_Thumb1", glm::quat{0.956054f, 0.293178f, -0.002559f, -0.001295f}},
        {"L_Thumb2", glm::quat{0.983266f, 0.016017f, -0.181468f, -0.000566f}},
        {"L_Thumb3", glm::quat{0.990157f, 0.0f, 0.139960f, 0.0f}},
    };

    m_r9poses["Red9Single"] = {
        {"L_IndexF1", glm::quat{0.9249f, 0.0f, -0.015f, -0.3801f}},
        {"L_IndexF2", glm::quat{0.8192f, 0.0f, 0.0f, -0.5736f}},
        {"L_IndexF3", glm::quat{0.866f, 0.0f, 0.0f, -0.5f}},
        {"L_MiddleF1", glm::quat{0.8962705731391907f, -0.10485319048166275f, 0.08518750220537186f, -0.42243102192878723f}},
        {"L_MiddleF2", glm::quat{0.7059580683708191f, 0.0f, 0.0f, -0.7082536220550537f}},
        {"L_MiddleF3", glm::quat{0.8905639052391052f, 0.0f, 0.0f, -0.45485809445381165f}},
        {"L_Palm", glm::quat{1.0f, 0.0f, 0.0f, 0.0f}},
        {"L_PinkyF1", glm::quat{0.9225811958312988f, -0.18226279318332672f, 0.0756017193198204f, -0.3315245509147644f}},
        {"L_PinkyF2", glm::quat{0.722647488117218f, 0.0f, 0.0f, -0.6912167072296143f}},
        {"L_PinkyF3", glm::quat{0.8969815373420715f, 0.0f, 0.0f, -0.4420679807662964f}},
        {"L_RingF1", glm::quat{0.8799594640731812f, -0.13302844762802124f, 0.09002332389354706f, -0.4470687806606293f}},
        {"L_RingF2", glm::quat{0.710166871547699f, 0.0f, 0.0f, -0.7040334343910217f}},
        {"L_RingF3", glm::quat{0.8921849131584167f, 0.0f, 0.0f, -0.4516703188419342f}},
        {"L_Thumb1", glm::quat{0.9185688495635986f, 0.2528655230998993f, -0.10858584940433502f, -0.28372427821159363f}},
        {"L_Thumb2", glm::quat{0.9996347427368164f, 0.0f, -0.027026206254959106f, 0.0f}},
        {"L_Thumb3", glm::quat{0.9233907461166382f, -1.1019408702850342e-05f, 0.38386136293411255f, -4.132278263568878e-06f}},
    };

    // ---- JSON zuletzt: sie ueberschreibt die Code-Defaults ------------
    rev_load_cfg();
    rifle_load_cfg();
    bolt_load_cfg();
    xbow_load_cfg();
    bow_load_cfg();
    red9_load_cfg();
    r9_publish_ratio();
}

// ============================================================================
// Hooks. In Lua stehen sie hinter Global-Guards (__re4_*_hooked), weil ein
// sdk.hook einen GAME-Neustart braucht; nativ reicht der eine Aufruf beim ersten
// Initialisieren.
//
// NICHT nachgebaut: der Lua-Hook auf chainsaw.Gun.callbackTracks. Sein Pre-Hook
// beginnt seit 2026-08-31 mit einem `do return end` -- er wird zwar noch
// installiert, tut aber nichts. Ihn hier auszulassen ist verhaltensgleich und
// spart einen Hook, der bei JEDEM Track-Callback feuert.
// ============================================================================
void RE4VRReload2::install_hooks() {
    // [ARMBRUST HAND-DUMMY] requestGenerateDummy liefert das erzeugte Objekt
    // nicht zurueck -- wir fangen es an seinem Start ab, waehrend `await` steht.
    if (auto* td = sdk::find_type_definition("chainsaw.ShellDummyBase"); td != nullptr) {
        auto* m = td->get_method("requestStart");

        if (m == nullptr) {
            m = td->get_method("start");
        }

        if (m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload2::instance();

                    if (s != nullptr && s->m_xdummy_await && args.size() >= 2) {
                        s->m_xdummy_obj = reinterpret_cast<::REManagedObject*>(args[1]);
                        s->m_xdummy_await = false;
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // [BOLT_MUTE] + [BOW-AIM-MUTE] Alle Sounds des WAFFEN-SoundContainers im
    // jeweiligen Fenster verschlucken. Bewusst ein Hook und KEIN
    // set_Enabled(false)/stopTriggered: ein Hook hinterlaesst keinen Zustand --
    // stirbt das Modul mitten im Fenster, ist die Waffe trotzdem wieder hoerbar,
    // waehrend ein abgeschalteter Container stumm haengen bliebe.
    // In Lua sind das ZWEI Hooks mit eigenen Guards; hier reicht einer, das
    // Ergebnis ist identisch (beide liefern im Trefferfall SKIP_ORIGINAL).
    // Der Fenstertest steht GANZ VORNE: der Hook feuert fuer jeden
    // SoundContainer im Spiel.
    if (auto* td = sdk::find_type_definition("soundlib.SoundContainer"); td != nullptr) {
        if (auto* m = td->get_method("trigger(System.UInt32)"); m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload2::instance();

                    if (s == nullptr || args.size() < 2) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const double now = clock_now();
                    const bool bolt_win = now < s->m_bolt_mute_until;
                    const bool bow_win = now < s->m_bow_mute_until;

                    if (!bolt_win && !bow_win) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    auto* self = reinterpret_cast<::REManagedObject*>(args[1]);
                    auto* go = re4vr::call_safe<::REManagedObject*>(self, "get_GameObject");

                    if (go == nullptr) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const auto addr = reinterpret_cast<uintptr_t>(go);

                    // [SELBST-AUSNAHME] eigene Laute nie verschlucken.
                    if (bolt_win && addr == s->m_bolt_gun_addr && s->m_bolt_gun_addr != 0
                        && !s->m_bolt_snd_self
                        && re4vr::lua_get_tribool("__re4_bolt_snd_self") != 1) {
                        return HookManager::PreHookResult::SKIP_ORIGINAL;
                    }

                    if (bow_win && addr == s->m_bow_gun_addr && s->m_bow_gun_addr != 0
                        && !s->m_bow_snd_self
                        && re4vr::lua_get_tribool("__re4_bow_snd_self") != 1) {
                        return HookManager::PreHookResult::SKIP_ORIGINAL;
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // [BOWDRAW FEUERSPERRE] Eigener Hook statt __vr_block_fire_when_empty: das
    // Flag gehoert reload, und dessen "Waffe nicht verwaltet"-Zweig setzt es fuer
    // wp4600 jeden Frame wieder auf false. Die Sperre haengt an einem
    // ZEITSTEMPEL: tickt dieses Modul nicht mehr, loest sie sich nach 0.3 s von
    // selbst auf -- eine haengende Feuersperre waere gamebreaking.
    if (auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment"); td != nullptr) {
        if (auto* m = td->get_method("isEnableFire"); m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) { return HookManager::PreHookResult::CALL_ORIGINAL; },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
                    auto* s = RE4VRReload2::instance();

                    if (s == nullptr || !s->m_bow_fire_block) {
                        return;
                    }

                    // alt -> die Sperre loest sich
                    if ((clock_now() - s->m_bow_gate_t) >= 0.3) {
                        return;
                    }

                    if (!s->m_frame_is_gameplay || s->m_bow_draw_off) {
                        return;
                    }

                    ret = 0;
                });
        }
    }
}

// ============================================================================
// Lua-Zustand
// ============================================================================
void RE4VRReload2::on_lua_state_created() {
    re4vr::LuaRef lua{};

    if (lua == nullptr) {
        return;
    }

    // motion ruft das an BEIDEN Pose-Stellen auf (tick-Pass UND BeginRendering).
    (*lua)["__re4_apply_bowknob_pose"] = []() {
        if (auto* s = RE4VRReload2::instance(); s != nullptr) {
            s->apply_bowknob_pose();
        }
    };

    // [NACKTES UI] Der Dispatcher ruft diese Funktion, wenn er da ist.
    (*lua)["__re4_reload2_draw_public_r9_ratio"] = []() {
        if (auto* s = RE4VRReload2::instance(); s != nullptr) {
            s->draw_public_red9_ratio();
        }
    };

    m_public_ui_registered = false;
    r9_publish_ratio();
}

void RE4VRReload2::on_lua_state_destroyed() {
    // Reset Scripts wiped nur Lua, NICHT die Szene -> die Joints blieben in ihrer
    // letzten Override-Lage stehen. Deshalb HIER, solange wir die echte Ruhe noch
    // kennen, alles zuruecksetzen -- 1:1 die sechs re.on_script_reset-Bloecke.
    rev_on_script_reset();
    rifle_on_script_reset();
    bolt_on_script_reset();
    xbow_on_script_reset();
    bow_on_script_reset();
    red9_on_script_reset();

    m_pmap.clear();
    m_pmap_tf = nullptr;
    m_character_manager = nullptr;
    m_pe_cache = nullptr;
    m_public_ui_registered = false;
}

// [TRAEGE ANMELDUNG] ##re4_vr_menu.lua setzt bei seinem Start
// __re4_ui_entries = {} -- eine dort schon eingetragene Anmeldung waere sofort
// wieder weg. Deshalb pro Frame nachsehen.
void RE4VRReload2::ensure_public_ui_registered() {
    if (m_public_ui_registered) {
        return;
    }

    re4vr::LuaRef lua{};

    if (lua == nullptr) {
        return;
    }

    sol::object add = (*lua)["__re4_ui_add"];

    if (!add.valid() || add.get_type() != sol::type::function) {
        return;   // ohne Dispatcher zeichnet unser eigenes on_draw_ui
    }

    sol::object entries = (*lua)["__re4_ui_entries"];

    if (!entries.valid() || entries.get_type() != sol::type::table) {
        return;
    }

    if (entries.as<sol::table>()["reload_red9_ratio_naked"].valid()) {
        m_public_ui_registered = true;

        return;
    }

    try {
        auto fn = add.as<sol::protected_function>();
        // Prioritaet 61 = direkt unter dem Shotgun-Ratio (das haengt auf 60).
        fn(61, "reload_red9_ratio_naked",
           (*lua)["__re4_reload2_draw_public_r9_ratio"]);
        m_public_ui_registered = true;
    } catch (...) {
    }
}

// ============================================================================
// Holster-Kette. In Lua wickelt jeder der sechs Bloecke seine eigene Fassung um
// __re4_reload_set_mag_in_hand; der ZULETZT geladene Wrapper wird als erster
// gefragt. Reihenfolge in der Datei: Revolver, Rifle, Bolt, Armbrust, Bowdraw,
// Red9 -- also wird hier von hinten nach vorn geprueft.
// std::nullopt = keine unserer Waffen -> der Aufrufer reicht an reload weiter.
// ============================================================================
std::optional<bool> RE4VRReload2::set_mag_in_hand(bool active) {
    const auto wid = get_equip_wid();

    if (wid.has_value() && is_red9(*wid)) {
        return red9_set_in_hand(active);
    }

    // [MAG-HOLSTER SPERREN] Solange der Bogen-Hebel gegriffen ist, darf das
    // Munitions-Holster keinen Pfeil in die Hand geben -- sonst holt man beim
    // Spannen versehentlich einen heraus.
    if (active && m_bow_st.grabbed && bow_live_wid().value_or(0) == BOW_WID
        && re4vr::lua_get_tribool("__re4_bow_draw") != 0) {
        return false;
    }

    if (wid.has_value() && is_xbow(*wid)) {
        return xbow_set_arrow_in_hand(active);
    }

    if (wid.has_value() && is_bolt(*wid)) {
        return bolt_set_cart_in_hand(active);
    }

    if (wid.has_value() && is_rifle(*wid)) {
        return rifle_set_mag_in_hand(active);
    }

    if (wid.has_value() && is_revolver(*wid)) {
        return revolver_set_mag_in_hand(active);
    }

    return std::nullopt;
}

// ============================================================================
// Dispatch. Die Reihenfolge ist die Registrierungsreihenfolge in Lua: die
// on_frame-Callbacks laufen in Ladereihenfolge, also Revolver -> Rifle -> Bolt
// -> Armbrust -> Bowdraw -> Red9. Wer zuletzt schreibt, gewinnt bei den
// geteilten Globals -- genau wie vorher.
// ============================================================================
void RE4VRReload2::on_frame() {
    // Fuer die Hooks gespiegelt: sie duerfen pro Aufruf nicht in den Lua-State
    // greifen (isEnableFire und SoundContainer.trigger feuern sehr oft).
    m_frame_is_gameplay = re4vr::lua_get_tribool("__re4_frame_is_gameplay") == 1;
    m_bow_draw_off = re4vr::lua_get_tribool("__re4_bow_draw") == 0;

    // [MESSUNG 2026-09-07 -- WEGWERF, faellt mit #re4_kf_mess.lua wieder raus]
    // Der Port publiziert bisher NUR aus RE4VRReloadMain (m_wep.mag_joint /
    // m_wep.tf). Die Red9 haengt aber an DIESER Datei und ist ausserdem
    // Top-Loader ohne _14-Mag -> beide Globals sind bei ihr leer, und kein
    // Lua-Script kommt an die Bahn heran. Diese drei Zeilen reichen die
    // Waffen-Transform und die beiden Mesh-Part-Klone durch, damit der Versatz
    // von aussen messbar wird. Kein Verhalten haengt daran.
    re4vr::lua_set_managed_object("__re4_mess_wep_tf", m_rwep.tf);
    re4vr::lua_set_managed_object("__re4_mess_clip", m_r9clip.obj);
    re4vr::lua_set_managed_object("__re4_mess_cart", m_cart.obj);

    ensure_public_ui_registered();
    tick_merc_round();

    rev_on_frame();
    rifle_on_frame();
    bolt_on_frame();
    xbow_on_frame();
    bow_on_frame();
    red9_on_frame();
}

// [RUNDEN-RESET] Neue Mercenaries-Runde -> Waffenzustand wegwerfen.
// SYMPTOM: die letzte Runde mit leerem Magazin verlassen -> in der neuen Runde
// ist die Waffe voll, das Modul will aber trotzdem nachladen/durchladen.
// URSACHE: der Zustand wird nur bei Waffenwechsel verworfen. Ein Rundenwechsel
// ist das nicht: das Spiel laedt die Map neu, die alten Objekte bleiben
// ansprechbar (Schreiben verpufft lautlos).
// TRIGGER: __re4_merc_round -- merc zaehlt es in der Ladeluecke hoch, in der der
// Body kurz gar nichts meldet. Ausserhalb Mercenaries aendert sich der Token nie.
void RE4VRReload2::tick_merc_round() {
    const auto t = re4vr::lua_get_number_opt("__re4_merc_round");
    const auto v = t.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*t)}
                                 : std::nullopt;

    if (v == m_round_seen) {
        return;
    }

    m_round_seen = v;
    m_pe_cache = nullptr;
}

// Die Render-Paesse. Jede Maschine haengt in Lua an ihrem eigenen Satz:
//   Revolver / Rifle / Bolt / Red9: LockScene(pre), LateUpdateBehavior,
//       UpdateJointExpression, BeginRendering(pre)
//   Armbrust / Bowdraw: zusaetzlich UpdateMotion (voller 5-Hook-Stack, sonst
//       trailed das Mesh der Hand nach)
//   Revolver + Armbrust: dazu BeginRendering(POST) fuer den Wobble-Fix
void RE4VRReload2::on_lock_scene_pre() {
    apply_revolver_pass();
    rifle_apply_pass();
    bolt_apply_pass();
    xbow_apply_pass();
    bow_apply_joints();
    red9_apply_pass();
}

void RE4VRReload2::on_update_motion() {
    xbow_apply_pass();
    bow_apply_joints();
}

void RE4VRReload2::on_late_update() {
    apply_revolver_pass();
    rifle_apply_pass();
    bolt_apply_pass();
    xbow_apply_pass();
    bow_apply_joints();
    red9_apply_pass();
}

void RE4VRReload2::on_update_joint_expression() {
    apply_revolver_pass();
    rifle_apply_pass();
    bolt_apply_pass();
    xbow_apply_pass();
    bow_apply_joints();
    red9_apply_pass();

    // [VERSATZ BEIM LAUFEN 2026-09-07] reposition_cart_late faehrt den
    // Mesh-Part-Klon (Patrone, Stripper-Clip) entlang der Keyframe-Bahn --
    // und stand bisher NUR im BeginRendering(POST). Die gemessene
    // Pass-Reihenfolge ist aber LockScene -> BeginRendering(pre) ->
    // BeginRendering(post) -> LateUpdateBehavior -> UpdateJointExpression:
    // danach bewegt die Engine Hand und Waffe noch zweimal, der Klon bleibt
    // auf der alten Lage stehen -> beim Laufen sitzt er um eine konstante
    // Frame-Strecke versetzt (User: "gehen mit, aber immer um denselben
    // Abstand versetzt"). Waffen-JOINTS trifft das nicht, die haengen im
    // Skelett und werden von der Engine mitpropagiert -- genau deshalb sind
    // die Magazinwaffen unauffaellig.
    // Hier im LETZTEN Pass nachziehen, wie beim Compound Bow (05.08.) und
    // beim rechten Messer. Das reale Einlegen am Bahn-Ende kann dadurch nicht
    // doppelt feuern: es haengt an kf_inserted und laeuft genau einmal.
    reposition_cart_late();
    xbow_reposition_late();
}

void RE4VRReload2::on_begin_rendering_pre() {
    apply_revolver_pass();
    rifle_apply_pass();
    bolt_apply_pass();
    xbow_apply_pass();
    bow_apply_joints();
    red9_apply_pass();
}

// [WOBBLE-FIX] NACH motions BeginRendering-POST (attach_left_hand): die geliehene
// Patrone bzw. der Bolzen-Dummy nochmal auf die finale VR-Hand.
void RE4VRReload2::on_begin_rendering() {
    reposition_cart_late();
    xbow_reposition_late();
}

// ============================================================================
// UI-Wurzel. In Lua EIN on_draw_ui mit dem Header "RE4VR - Reload2", darin die
// sechs Gattungen gestapelt.
// ============================================================================
void RE4VRReload2::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Reload2")) {
        return;
    }

    rev_ui();

    ImGui::Separator();
    rifle_ui();

    ImGui::Separator();
    bolt_ui();

    ImGui::Separator();
    xbow_ui();
    bow_ui();   // [UMZUG] Bowdraw unter der Armbrust

    ImGui::Separator();
    red9_ui();
    ImGui::TreePop();
}

#endif
