// ============================================================================
// RE4VRReload5 -- 1:1-Portierung von re4_vr_reload5_dlc.lua (4.189 Zeilen).
// Spezifikation: I:\LUATRANS\PORT_RELOAD2_SPEC.md
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
#include "RE4VRReload5.hpp"

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

::REManagedObject* RE4VRReload5::get_ctx() {
    if (re4vr::fc::on()) {
        return re4vr::fc::ctx();
    }

    if (!re4vr::obj_ok(m_character_manager)) {
        m_character_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

std::optional<int32_t> RE4VRReload5::get_equip_wid() {
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

::REManagedObject* RE4VRReload5::body_tf() {
    if (re4vr::fc::on()) {
        return re4vr::fc::body_tf();
    }

    auto* ctx = get_ctx();
    auto* b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    return re4vr::call_safe<::REManagedObject*>(b, "get_Transform");
}

::REManagedObject* RE4VRReload5::get_pe() {
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
::REManagedObject* RE4VRReload5::find_weapon(int32_t wid) {
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
bool RE4VRReload5::right_b_down() {
    return re4vr::lua_get_tribool("__vr_raw_r_bbutton") == 1;
}

// [POSE_FADE] s. Header.
bool RE4VRReload5::pose_fade_step(PoseFade& f, const std::string& want, float& blend_out) {
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

const std::unordered_map<std::string, ::REManagedObject*>& RE4VRReload5::pose_map() {
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

bool RE4VRReload5::pose_apply(const Bones& bones, float blend) {
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

constexpr const char* RIF_CFG_PATH = "re4_vr/re4_vr_reload5_dlc_rifle.json";

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
    // [DLC] Anti-Materiel Rifle = 1:1 Leons Stingray (4401).
    if (wid == 6105) {
        return RifJoints{"_04", "_02", "_05"};
    }

    return std::nullopt;
}

const char* rif_mag_pose(int32_t wid) {
    (void)wid;

    return "StingrayMag";
}

const char* rif_rack_pose(int32_t wid) {
    (void)wid;

    return "StingraySlide";
}

const char* rif_switch_pose(int32_t wid) {
    (void)wid;

    return "StingraySwitch";
}

// Engine schliesst den Slide selbst nach dem Reload? (Stingray = true) -> wir
// forcen rest_z NICHT. CQBR NICHT: voller manueller Rack.
bool rif_engine_closes(int32_t wid) {
    return wid == 6105;
}

// Sperrt Schalter-Stufe 1 das Feuern? Stingray ja (bewusster Dry-Fire-Stand);
// CQBR NEIN -- dort ist Stufe 1 = 2-Schuss-Burst und muss feuern.
bool rif_switch_blocks_fire(int32_t wid) {
    return wid == 6105;
}

// Schalter-Stufe 1 = Burst? Wert = Schuss pro Trigger-Zug.
std::optional<int32_t> rif_switch_burst(int32_t wid) {
    (void)wid;   // [DLC] leer (war nur der CQBR 4402, der gehoert Leon)

    return std::nullopt;
}

// ---- Sounds (per-wid) ----
struct RifSnd {
    uint32_t dry_fire, mag_eject, mag_insert, mag_floor;
    uint32_t slide_back, slide_forward, mag_holster, sw;
};

std::optional<RifSnd> rif_snd_set(int32_t wid) {
    // [DLC] mag_eject/insert/slide_back und der Verstellschalter sind bestaetigt.
    if (wid == 6105) {
        return RifSnd{812850326u, 1466005368u, 943565871u, 3042341191u,
                      2254736731u, 2254736731u, 1839787494u, 3805002294u};
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

float jnum(const nlohmann::json& j, const char* key, float def) {
    const auto it = j.find(key);

    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

bool jbool(const nlohmann::json& j, const char* key, bool cur) {
    const auto it = j.find(key);

    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : cur;
}

std::string jstr(const nlohmann::json& j, const char* key, const std::string& cur) {
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

bool RE4VRReload5::is_rifle(int32_t wid) {
    // [DLC] wp6105 Anti-Materiel Rifle (= Leons Stingray 4401). Leons 4401/4402
    // werden NIE angefasst.
    return wid == 6105;
}

RE4VRReload5::RSlide& RE4VRReload5::rslide(int32_t wid) {
    auto it = m_rslide.find(wid);

    if (it == m_rslide.end()) {
        it = m_rslide.emplace(wid, RSlide{}).first;
    }

    return it->second;
}

RE4VRReload5::RDock& RE4VRReload5::rdock(int32_t wid) {
    auto it = m_rdock.find(wid);

    if (it == m_rdock.end()) {
        it = m_rdock.emplace(wid, RDock{}).first;
    }

    return it->second;
}

RE4VRReload5::RMagHand& RE4VRReload5::rmaghand(int32_t wid) {
    auto it = m_rmaghand.find(wid);

    if (it == m_rmaghand.end()) {
        it = m_rmaghand.emplace(wid, RMagHand{}).first;
    }

    return it->second;
}

RE4VRReload5::RSwitch& RE4VRReload5::rswitch(int32_t wid) {
    auto it = m_rswitch.find(wid);

    if (it == m_rswitch.end()) {
        it = m_rswitch.emplace(wid, RSwitch{}).first;
    }

    return it->second;
}

// Slide-Idle-/Pull-Basis: empty_x-Waffen (CQBR) bleiben auf empty in Z VORNE
// (rest_z) und fahren nur in X aus -> der Z-Pull ist der manuelle Rack.
// Stingray/SMG: park_z (Slide rutscht mittig).
float RE4VRReload5::park_ref(const RSlide& sp) {
    return (sp.empty_x != 0.0f) ? sp.rest_z : sp.park_z;
}

bool RE4VRReload5::rifle_flow() const {
    return m_rifdrop.active || m_rif_mag_hand || m_rifins.active || m_rif_mag_tune;
}

void RE4VRReload5::rifle_load_cfg() {
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

            d.x = jnum(v, "x", d.x);
            d.y = jnum(v, "y", d.y);
            d.z = jnum(v, "z", d.z);
        }
    }

    // [DLC] Diese Datei kennt keinen Wert pro Waffe -- es gilt der
    // Gattungs-Wert aus der Skalar-Config.
    rdock(6105).insert = m_rifcfg.insert_distance;

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

void RE4VRReload5::rifle_save_cfg() {
    nlohmann::json slideo = nlohmann::json::object();
    nlohmann::json docko = nlohmann::json::object();
    nlohmann::json mho = nlohmann::json::object();
    nlohmann::json swo = nlohmann::json::object();

    for (const int32_t wid : {6105}) {
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
        docko[key] = {{"joint", d.joint}, {"x", d.x}, {"y", d.y}, {"z", d.z}};

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
void RE4VRReload5::rf_snd(uint32_t id) {
    if (!m_rifcfg.sound_enabled || id == 0 || m_rifwep.tf == nullptr) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_rifwep.tf, "get_GameObject"), id);
}

void RE4VRReload5::rifle_refresh() {
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
::REManagedObject* RE4VRReload5::rf_get_gun() {
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
void RE4VRReload5::rf_gun_chamber() {
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

bool RE4VRReload5::rf_gun_ammo_empty() {
    bool v = false;

    return re4vr::try_call<bool>(get_pe(), "isGunAmmoEmpty", v) && v;
}

// ---- Live-WeaponItem. EIGENES rf_get_wi (NICHT get_live_wi!): das validiert
// gegen die Revolver-wid und gaebe bei einer Rifle ein evtl. stale Revolver-Item
// zurueck. Hier gegen die EQUIPPTE wid validieren.
::REManagedObject* RE4VRReload5::rf_get_wi() {
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

std::optional<int32_t> RE4VRReload5::rf_loaded() {
    auto* wi = rf_get_wi();

    if (wi == nullptr) {
        return std::nullopt;
    }

    return call_enum(wi, "get_CurrentAmmoCount");
}

int32_t RE4VRReload5::rf_cap() {
    auto* wi = rf_get_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload5::rf_reserve() {
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
void RE4VRReload5::rf_capture_mag_rest() {
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

void RE4VRReload5::rf_stop_drop() {
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
bool RE4VRReload5::rf_start_drop_from(const std::optional<glm::vec3>& p, bool use_module) {
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

bool RE4VRReload5::rf_force_eject() {
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
        // [DLC-FASSUNG 05.09.2026] Hier stand Reload2s Kommentar samt Bug
        // ("`loaded` ist ein Global und damit immer nil -> faktisch 0").
        // re4_vr_reload5_dlc.lua hat genau das REPARIERT (Z.556-558) und nennt
        // auch die Folge: "droppt man ein VOLLES Mag ohne Reserve, meldet
        // rf_can_grab (0 + 0 > 0 = false) und man kann kein neues Mag holen --
        // obwohl 18 Schuss im gedroppten Mag waren." Also der echte, frische
        // Engine-Read wie in der DLC.
        {
            const auto ld = call_enum(get_pe(), "getCurrentGunAmmo");
            m_rif_mag_retained = ld.has_value() ? *ld : rf_loaded().value_or(0);
        }
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

void RE4VRReload5::rf_update_drop() {
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
bool RE4VRReload5::rf_can_grab() {
    if (m_rif_mag_hand || m_rifins.active) {
        return false;
    }

    if (!m_rif_mag_out) {
        return false;
    }

    return (m_rif_mag_retained + rf_reserve()) > 0;
}

bool RE4VRReload5::rifle_set_mag_in_hand(bool active) {
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
void RE4VRReload5::rf_update_mag_in_hand() {
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
std::optional<glm::vec3> RE4VRReload5::rf_dock_port_world() {
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
std::optional<glm::vec3> RE4VRReload5::rf_kf_anchor_world() {
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
bool RE4VRReload5::rf_start_insert() {
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

void RE4VRReload5::rf_check_insert_proximity() {
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

void RE4VRReload5::rf_reload_ammo_on_insert() {
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

void RE4VRReload5::rf_update_insert() {
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
void RE4VRReload5::rf_clear_rack() {
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

void RE4VRReload5::rf_update_rack_gesture() {
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
void RE4VRReload5::rf_update_dock_publish(bool advance) {
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
void RE4VRReload5::rf_apply_slide_park() {
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
void RE4VRReload5::rf_update_switch() {
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

void RE4VRReload5::rf_apply_switch() {
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
void RE4VRReload5::rf_apply_mag_out_hidden() {
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
void RE4VRReload5::rf_apply_hand_pose() {
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
void RE4VRReload5::rifle_soft_reset(bool keep_carry) {
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

void RE4VRReload5::rifle_on_frame() {
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
void RE4VRReload5::rifle_apply_pass() {
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

void RE4VRReload5::rifle_on_script_reset() {
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
void RE4VRReload5::rifle_ui() {
    if (ImGui::Checkbox("##rifle_en", &m_rifcfg.rifle_enabled)) {
        rifle_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Anti-Materiel Rifle Reload");

    if (!ImGui::TreeNode("Rifle -- Einstellungen")) {
        return;
    }

    const auto awid = m_rifwep.wid.has_value() ? m_rifwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_rifle(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Anti-Materiel Rifle erkannt - verwaltet)"
                             : "  (keine Anti-Materiel Rifle equippt)");

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

    if (ImGui::SliderFloat("Einlege-Distanz m (Mag)##rifdist",
                           &m_rifcfg.insert_distance, 0.03f, 0.50f)) {
        rdock(6105).insert = m_rifcfg.insert_distance;
        rifle_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Insert nachladen", &m_rifcfg.reload_ammo)) {
        rifle_save_cfg();
    }

    if (ImGui::Checkbox("Sounds an", &m_rifcfg.sound_enabled)) {
        rifle_save_cfg();
    }

    const int32_t wid = awid.value_or(6105);

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
                       "Anti-Materiel = Stingray-Logik (Mag _04 + Slide-Rack _02 + "
                       "Schalter _05). Werte von Leon geseedet, ab hier unabhaengig.");
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

constexpr const char* BOLT_CFG_PATH = "re4_vr/re4_vr_reload5_dlc_bolt.json";
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
constexpr const char* BOLT_CYCLE_NODE = "wp6114_general_0513_Aim_Fire_after";
// s Nachlauf der Erkennung (nur fuer das Flag, nicht fuers Spulen)
constexpr double BOLT_IN_CYCLE_HOLD = 0.10;

}   // namespace

bool RE4VRReload5::is_bolt(int32_t wid) {
    // [DLC] wp6114 Hunting Rifle (= Leons SR M1903 4400).
    return wid == 6114;
}

RE4VRReload5::BoltCfg& RE4VRReload5::bcfg(int32_t wid) {
    auto it = m_bcfg.find(wid);

    if (it == m_bcfg.end()) {
        it = m_bcfg.emplace(wid, BoltCfg{}).first;
    }

    return it->second;
}

void RE4VRReload5::bolt_load_cfg() {
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

void RE4VRReload5::bolt_save_cfg() {
    nlohmann::json tune = nlohmann::json::object();
    const auto& c = bcfg(6114);
    tune["6114"] = {
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
::REManagedObject* RE4VRReload5::bget_wi() {
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

std::optional<int32_t> RE4VRReload5::bloaded() {
    auto* wi = bget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
}

int32_t RE4VRReload5::bcap() {
    auto* wi = bget_wi();

    return (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
}

int32_t RE4VRReload5::breserve() {
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
void RE4VRReload5::bolt_chamber() {
    rf_gun_chamber();   // identischer Code; der Bolt-Block hat in Lua nur eine Kopie
}

void RE4VRReload5::bsnd(uint32_t id) {
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

void RE4VRReload5::bolt_refresh() {
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

void RE4VRReload5::bolt_capture_cart_rest() {
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
void RE4VRReload5::bolt_settle_step(const BoltCfg& c) {
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
void RE4VRReload5::update_bolt_gesture() {
    if (m_bolt.preview) {
        return;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(6114));
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
void RE4VRReload5::apply_bolt_joint() {
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

    const auto& c = bcfg(m_bwep.wid.value_or(6114));
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
void RE4VRReload5::update_bolt_dock(bool advance) {
    ::REManagedObject* src = nullptr;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    float rrx = 0.0f, rry = 0.0f, rrz = 0.0f;
    float want = 0.0f;

    if ((m_bolt.grab || m_bolt.preview) && m_bwep.bolt_joint != nullptr) {
        const auto& c = bcfg(m_bwep.wid.value_or(6114));
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
void RE4VRReload5::update_cart_in_hand() {
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

    const auto& c = bcfg(m_bwep.wid.value_or(6114));
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
std::optional<glm::vec3> RE4VRReload5::bolt_chamber_world() {
    if (m_bwep.tf == nullptr) {
        return std::nullopt;
    }

    const auto& c = bcfg(m_bwep.wid.value_or(6114));
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
void RE4VRReload5::bolt_add_one() {
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
bool RE4VRReload5::start_cart_insert() {
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

void RE4VRReload5::bolt_check_insert_proximity() {
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

void RE4VRReload5::update_cart_insert() {
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
bool RE4VRReload5::bolt_can_load() {
    if (m_bcart.active || m_bcart.insert) {
        return false;
    }

    if (!m_bolt.open) {
        return false;
    }

    return bloaded().value_or(0) < bcap() && breserve() > 0;
}

bool RE4VRReload5::bolt_set_cart_in_hand(bool active) {
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
void RE4VRReload5::apply_bolt_pose() {
    std::string name = "RiotSLide";
    const BoltCfg* thumb = nullptr;

    if (m_bcart.active || m_bcart.insert || m_bcart.tune) {
        name = "Shotgunshell";
        thumb = &bcfg(m_bwep.wid.value_or(6114));
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

void RE4VRReload5::bolt_soft_reset() {
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
void RE4VRReload5::bolt_cycle_suppress() {
    const double now = clock_now();
    // Das Flag NICHT hart nullen, sondern auslaufen lassen: callbackTracks feuert
    // im Engine-Update, dieses on_frame kann danach liegen.
    re4vr::lua_set_bool("__re4_bolt_in_cycle_dlc", now < m_bolt_in_cycle_until);

    if (!m_bwep.wid.has_value()) {
        m_bolt_in_cycle_until = 0.0;
        re4vr::lua_set_bool("__re4_bolt_in_cycle_dlc", false);

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

        re4vr::lua_set_bool("__re4_bolt_in_cycle_dlc", true);
        m_bolt_in_cycle_until = now + BOLT_IN_CYCLE_HOLD;
        float ef = 0.0f;

        if (re4vr::try_call<float>(node, "get_EndFrame", ef) && ef > 0.0f) {
            re4vr::call_safe<void*>(node, "set_Frame", ef);
        }

        re4vr::call_safe<void*>(layer, "set_Speed", 100.0f);

        break;   // der Node laeuft genau einmal
    }
}

void RE4VRReload5::bolt_on_frame() {
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

    // [NICHT IN DER DLC 05.09.2026] Hier stand ein Stempel auf
    // __re4_bolt_wid_active. Dieses Global gehoert re4_vr_reload2.lua (Leons
    // Bolt Thrower, Z.3754) und wird von motion als Gate fuer Leons Bolt-
    // Ruhepose der rechten Hand gelesen. In re4_vr_reload5_dlc.lua kommt der
    // Name KEIN EINZIGES MAL vor -- der Port hat ihn aus Reload2 mitkopiert und
    // aktivierte damit Leons Ruhepose an Adas Hand, solange sie die Hunting
    // Rifle (6114) fuehrt.

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

void RE4VRReload5::bolt_apply_pass() {
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

void RE4VRReload5::bolt_on_script_reset() {
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
void RE4VRReload5::bolt_ui() {
    if (ImGui::Checkbox("##bolt_en", &m_bscalar.bolt_enabled)) {
        bolt_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Bolt-Action Reload (Hunting Rifle)");

    if (!ImGui::TreeNode("Bolt-Action -- Einstellungen")) {
        return;
    }

    const auto awid = m_bwep.wid.has_value() ? m_bwep.wid : get_equip_wid();
    ImGui::Text("Equippt: wp%s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_bolt(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Hunting Rifle erkannt - verwaltet)"
                             : "  (keine Hunting Rifle equippt)");
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

    auto& cc = bcfg(awid.value_or(6114));

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

constexpr int32_t R9 = 6113;
constexpr const char* R9_CFG_PATH = "re4_vr/re4_vr_reload5_dlc_samuraiedge.json";

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

bool RE4VRReload5::is_red9(int32_t wid) {
    return wid == R9;
}

// [RATIO GETEILT] Der Ratio-Schalter gilt fuer BEIDE Red9s. Besitzer des Werts
// ist reload2 (dort liegen UI und JSON) -- diese Maschine LIEST ihn nur ueber das
// Global. Faellt reload2 aus, gilt der Default 1 (1:1).
int32_t RE4VRReload5::r9_ratio() const {
    const auto v = re4vr::lua_get_number("__re4_red9_shell_ratio", 1.0);

    return std::max(1, static_cast<int32_t>(v));
}

void RE4VRReload5::red9_load_cfg() {
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

void RE4VRReload5::red9_save_cfg() {
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
}

::REManagedObject* RE4VRReload5::rget_gun() {
    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");

    if (hu == nullptr) {
        return nullptr;
    }

    if (call_enum(hu, "get_EquipWeaponID").value_or(0) != R9) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");
}

::REManagedObject* RE4VRReload5::rget_gun_mesh() {
    return re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_Mesh");
}

::REManagedObject* RE4VRReload5::rget_gun_tf() {
    auto* go = re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_GameObject");

    return re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
}

void RE4VRReload5::r9_play_sound(uint32_t id) {
    if (id == 0) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(rget_gun(), "get_GameObject");

    if (go != nullptr) {
        trigger_sound(go, id);
    }
}

// ---- Hand-Pose (EIGENE Daten, gestures-unabhaengig) ----
void RE4VRReload5::r9_apply_pose(const std::string& name, bool with_thumb, float blend) {
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
void RE4VRReload5::r9_destroy() {
    if (m_r9clip.obj != nullptr) {
        re4vr::destroy_game_object(m_r9clip.obj);
    }

    m_r9clip.obj = nullptr;
    m_r9clip.mesh = nullptr;
    m_r9clip.parts_sig.clear();
    m_r9clip.parented = false;
}

bool RE4VRReload5::r9_spawn() {
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
    auto* go = re4vr::create_game_object("vr_samurai_clip");

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

void RE4VRReload5::r9_isolate() {
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
bool RE4VRReload5::r9_apply_drop() {
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

void RE4VRReload5::r9_start_drop() {
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
::REManagedObject* RE4VRReload5::rget_slide() {
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

void RE4VRReload5::rset_slide_z(float z) {
    auto* j = m_r9rack.joint;
    glm::vec3 lp{};

    if (j == nullptr || !get_vec3(j, "get_LocalPosition", lp)) {
        return;
    }

    set_vec3(j, "set_LocalPosition", glm::vec3{lp.x, lp.y, z});
}

// Hand an den Slide docken (arm_chain liest __vr_slide_hand_world_*). Publisht
// NUR den aktuellen Blend (kein Ramp hier -> kann pro Pass gerufen werden).
void RE4VRReload5::r9_publish_dock(::REManagedObject* j) {
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
std::optional<glm::vec3> RE4VRReload5::r9_hand_gun_local(const glm::vec3& p) {
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

void RE4VRReload5::update_slide_rack() {
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
void RE4VRReload5::r9_set_tf(::REManagedObject* tf, const glm::vec3& p,
                             const std::optional<glm::quat>& rot, float s) {
    sdk::set_transform_position(reinterpret_cast<::RETransform*>(tf),
                                Vector4f{p.x, p.y, p.z, 1.0f}, true);

    if (rot.has_value()) {
        sdk::set_transform_rotation(reinterpret_cast<::RETransform*>(tf), *rot);
    }

    set_vec3(tf, "set_LocalScale", glm::vec3{s, s, s});
}

void RE4VRReload5::r9_follow_to_hand() {
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
std::optional<glm::vec3> RE4VRReload5::r9_dockport_world() {
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

std::optional<int32_t> RE4VRReload5::r9_equip_type_main() {
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
void RE4VRReload5::r9_fill_to_cap() {
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
void RE4VRReload5::r9_add_single() {
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
    int32_t want = r9_ratio();

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
bool RE4VRReload5::r9_grab_allowed() {
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
void RE4VRReload5::r9_check_insert() {
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
bool RE4VRReload5::r9_update_insert() {
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
    // [KEYFRAME-IDs 05.09.2026] Die DLC-Lua (Z.2782) nimmt hier ausdruecklich
    // Leons Red9-Keyframes: "Samurai Edge (6113) = 1:1 Red9 -> NUTZT DIESELBEN
    // Red9-Keyframes (single=40021, Stripper-Clip=4002)". Der Port hatte auf
    // 61131/6113 umgestellt -- IDs, die RE4VRReloadAdv::is_keyframe_insert gar
    // nicht kennt. has_shell_keys lieferte damit immer false: die Keyframe-Bahn
    // des Clip-/Einzelpatronen-Einschubs und die Preview waren komplett tot.
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
                r9_set_tf(tf, gp + (gr * glm::vec3{k.x, k.y, k.z}),
                          glm::normalize(gr * quat_from_euler(k.rx, k.ry, k.rz)),
                          m_r9cfg.dscale);
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
void RE4VRReload5::r9_update(bool show) {
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

bool RE4VRReload5::red9_set_in_hand(bool active) {
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

void RE4VRReload5::red9_on_frame() {
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
void RE4VRReload5::red9_apply_pass() {
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

void RE4VRReload5::red9_on_script_reset() {
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

// ---------------------------------------------------------------------
// UI -- Red9
// ---------------------------------------------------------------------
void RE4VRReload5::red9_ui() {
    if (ImGui::Checkbox("##samurai_en", &m_r9cfg.enabled)) {
        red9_save_cfg();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Enable");
    ImGui::SameLine();
    ImGui::Text("Manual Samurai Edge Reload (wp6113)");

    if (!ImGui::TreeNode("Red9 -- Einstellungen")) {
        return;
    }

    const auto awid = get_equip_wid();
    ImGui::Text("Equippt: %s",
                awid.has_value() ? std::to_string(*awid).c_str() : "nil");
    const bool known = awid.has_value() && is_red9(*awid);
    ImGui::TextColored(known ? ImVec4{0.0f, 1.0f, 0.0f, 1.0f}
                             : ImVec4{1.0f, 0.65f, 0.0f, 1.0f},
                       known ? "  (Samurai Edge erkannt - verwaltet)"
                             : "  (keine Samurai Edge equippt)");

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
        // [SHELL-RATIO] Der Schalter steht in reload2 (dort liegen UI und JSON);
        // beide Red9s teilen sich EINEN Wert. Hier nur die Anzeige.
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "Shell Ratio (Schuss pro Einzelpatrone): %d:%d  "
                           "-- umschalten im Red9-Baum von Reload2.", 1, r9_ratio());

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
RE4VRReload5* RE4VRReload5::s_instance{nullptr};

void RE4VRReload5::on_initialize() {
    s_instance = this;

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

    // ---- 4 SAMURAI EDGE: Hand-Posen (EIGENE Daten-Kopie) ---------------
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
    rifle_load_cfg();
    bolt_load_cfg();
    red9_load_cfg();
    xbow_load_cfg();
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
void RE4VRReload5::install_hooks() {
    // [BOLT_MUTE] Alle Sounds des WAFFEN-SoundContainers im Zyklus-Fenster
    // verschlucken. Bewusst ein Hook und KEIN
    // set_Enabled(false)/stopTriggered: ein Hook hinterlaesst keinen Zustand --
    // stirbt das Modul mitten im Fenster, ist die Waffe trotzdem wieder hoerbar,
    // waehrend ein abgeschalteter Container stumm haengen bliebe.
    // Der Fenstertest steht GANZ VORNE: der Hook feuert fuer jeden
    // SoundContainer im Spiel.
    if (auto* td = sdk::find_type_definition("soundlib.SoundContainer"); td != nullptr) {
        if (auto* m = td->get_method("trigger(System.UInt32)"); m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload5::instance();

                    if (s == nullptr || args.size() < 2) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const double now = clock_now();
                    const bool bolt_win = now < s->m_bolt_mute_until;

                    if (!bolt_win) {
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

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // ========================================================================
    // [FIRE-GATE DLC5 2026-07-20 -- per Log belegt]
    // PROBLEM: Unser Feuer-Block (f.RT weglassen) erreicht die Engine bei ADAS
    // Waffen NICHT. Beweis aus re4_rack_diag.log: "BF block_fire false -> true
    // gesetzt von: re4_vr_reload5_dlc.lua" und trotzdem "SHOT von_uns=false
    // block_fire=true empty_trigger=true" -> der Schuss kommt am Binding vorbei
    // direkt aus dem rohen Controller-Trigger. Bei Leon bremst zusaetzlich die
    // Engine selbst; die DLC-Waffen chambern dagegen selbst weiter.
    //
    // LOESUNG (identisch zu reload4): nicht den Trigger abfangen, sondern der
    // Engine IHRE EIGENE Frage beantworten -- isEnableFire -> false, solange
    // dieses Modul die equippte Waffe verwaltet UND seinen Block gesetzt hat.
    //
    // ABSICHERUNG (haengende Sperre = tote Waffe = gamebreaking): siehe
    // update_fire_gate5(). Dazu der ZEITSTEMPEL: tickt on_frame nicht mehr,
    // loest sich die Sperre nach 0.3 s von selbst auf.
    // ========================================================================
    if (auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment"); td != nullptr) {
        if (auto* m = td->get_method("isEnableFire"); m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) { return HookManager::PreHookResult::CALL_ORIGINAL; },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
                    auto* s = RE4VRReload5::instance();

                    if (s == nullptr || !s->m_fg5_block) {
                        return;
                    }

                    // alt -> die Sperre loest sich
                    if ((clock_now() - s->m_fg5_t) >= 0.3) {
                        return;
                    }

                    // false -> Engine feuert nicht (spielt ihren eigenen Dry-Fire)
                    ret = 0;
                });
        }
    }
}

// Die Bedingungen des Fire-Gates -- EINMAL pro Frame, weil der Hook nicht in den
// Lua-State greifen darf (isEnableFire feuert sehr oft).
// * nur bei den 4 hier verwalteten DLC-Waffen (6105/6114/6113/6102), nie bei Leon
// * nur wenn __vr_block_fire_when_empty gerade true ist
// * nicht im Killswitch/KS4, nicht waehrend eines Mag-/Patronen-Flows
// * Not-Aus jederzeit: _G.__re4_fire_gate5 = false
void RE4VRReload5::update_fire_gate5() {
    m_fg5_block = false;

    if (re4vr::lua_get_tribool("__re4_fire_gate5") != 1) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_block_fire_when_empty") != 1) {
        return;
    }

    const auto wid = get_equip_wid();

    if (!wid.has_value()) {
        return;
    }

    if (!(is_rifle(*wid) || is_bolt(*wid) || is_red9(*wid) || is_xbow(*wid))) {
        return;
    }

    if (re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
        || re4vr::lua_get_tribool("__re4_ks4_active") == 1) {
        return;
    }

    if (re4vr::lua_get_tribool("__vr_mag_in_hand") == 1) {
        return;
    }

    m_fg5_block = true;
    m_fg5_t = clock_now();
}

// ============================================================================
// Lua-Zustand
// ============================================================================
void RE4VRReload5::on_lua_state_created() {
    re4vr::LuaRef lua{};

    if (lua == nullptr) {
        return;
    }

    // Dieses Teil exportiert nichts nach Lua: die Samurai Edge liest den
    // Shell-Ratio ueber __re4_red9_shell_ratio, und den setzt reload2 (dort
    // liegen UI und JSON). Faellt reload2 aus, gilt hier der Default 1 (1:1).
    (void)lua;

    // [FIRE-GATE DLC5] Not-Aus-Schalter anlegen: _G.__re4_fire_gate5 = false
    // schaltet den isEnableFire-Riegel jederzeit ab.
    re4vr::lua_set_bool("__re4_fire_gate5", true);
}

void RE4VRReload5::on_lua_state_destroyed() {
    // Reset Scripts wiped nur Lua, NICHT die Szene -> die Joints blieben in ihrer
    // letzten Override-Lage stehen. Deshalb HIER, solange wir die echte Ruhe noch
    // kennen, alles zuruecksetzen -- 1:1 die sechs re.on_script_reset-Bloecke.
    rifle_on_script_reset();
    bolt_on_script_reset();
    red9_on_script_reset();
    xbow_on_script_reset();

    m_pmap.clear();
    m_pmap_tf = nullptr;
    m_character_manager = nullptr;
    m_pe_cache = nullptr;
    m_public_ui_registered = false;
}

// ============================================================================
// 5 -- BLAST CROSSBOW wp6102 (Lua Z.3180-4094)
// ============================================================================
// Ablauf: ZUERST die Sehne spannen (vorher gibt der Mag-Holster nichts her),
// DANN einen Bolzen greifen (Mesh-Part-Klon an der linken Hand) und ihn an den
// Einlegepunkt fuehren. Dort wird SOFORT getauscht: der Klon verschwindet, die
// Munition wird gebucht, und der native Bolzen _04 steht ohnehin genau da.
// ============================================================================

namespace {

constexpr int32_t BOW_WID = 6102;
constexpr const char* J_STRING = "_01";   // Sehne (wird gespannt)
constexpr const char* J_ARROW = "_04";    // Bolzen in der Waffe
constexpr const char* J_INSERT = "_03";   // Bezug des Einlegepunkts

// [GEMESSEN 2026-07-20] feste Anschlaege der Sehne (LocalPosition Z)
constexpr float STRING_REST_Z = 0.35594f;     // entspannt (nach dem Schuss)
constexpr float STRING_DRAWN_Z = -0.01500f;   // voll gespannt
// [GEMESSEN] Bolzen _04 in der Waffe bei geladener Waffe; 0/0/0 = nicht gesetzt.
constexpr float BOLT_LOADED_Z = 0.32000f;
constexpr float BOLT_LOADED_Y = 0.11360f;

constexpr const char* BCFG_PATH = "re4_vr/re4_vr_reload5_dlc_bow.json";

// [SOUND 2026-07-20] IDs ausgemessen; gespielt auf dem SoundContainer der Waffe.
constexpr uint32_t BSND_DRY_FIRE = 812850326u;       // RT ohne Bolzen/ohne Spannung
constexpr uint32_t BSND_DROP_FLOOR = 3732631409u;    // fallengelassener Bolzen
constexpr uint32_t BSND_INSERT = 1839787494u;        // Bolzen wird eingelegt
constexpr uint32_t BSND_DRAW_SEG = 3331890328u;      // Segment beim Spannen
constexpr uint32_t BSND_HOLSTER_GRAB = 3042341191u;  // Bolzen aus dem Holster

}   // namespace

bool RE4VRReload5::is_xbow(int32_t wid) {
    return wid == BOW_WID;
}

void RE4VRReload5::xbow_load_cfg() {
    const auto d = re4vr::json_load(BCFG_PATH);

    if (!d.is_object()) {
        return;
    }

    auto& c = m_bowcfg;
    c.enabled = jbool(d, "enabled", c.enabled);
    c.reload_ammo = jbool(d, "reload_ammo", c.reload_ammo);
    c.sound_enabled = jbool(d, "sound_enabled", c.sound_enabled);
    c.draw_smooth = jnum(d, "draw_smooth", c.draw_smooth);
    c.dock_blend_speed = jnum(d, "dock_blend_speed", c.dock_blend_speed);
    c.latch_hold_sec = jnum(d, "latch_hold_sec", c.latch_hold_sec);
    c.insert_distance = jnum(d, "insert_distance", c.insert_distance);
    c.insert_x = jnum(d, "insert_x", c.insert_x);
    c.insert_y = jnum(d, "insert_y", c.insert_y);
    c.insert_z = jnum(d, "insert_z", c.insert_z);
    c.draw_grab_dist = jnum(d, "draw_grab_dist", c.draw_grab_dist);
    c.draw_z = jnum(d, "draw_z", c.draw_z);
    c.draw_need = jnum(d, "draw_need", c.draw_need);
    c.arrow_x = jnum(d, "arrow_x", c.arrow_x);
    c.arrow_y = jnum(d, "arrow_y", c.arrow_y);
    c.arrow_z = jnum(d, "arrow_z", c.arrow_z);
    c.arrow_rx = jnum(d, "arrow_rx", c.arrow_rx);
    c.arrow_ry = jnum(d, "arrow_ry", c.arrow_ry);
    c.arrow_rz = jnum(d, "arrow_rz", c.arrow_rz);
    c.arrow_part = static_cast<int32_t>(jnum(d, "arrow_part",
                                             static_cast<float>(c.arrow_part)));
    c.arrow_preview = jbool(d, "arrow_preview", c.arrow_preview);
    c.arrow_pose = jstr(d, "arrow_pose", c.arrow_pose);
    c.arrow_trx = jnum(d, "arrow_trx", c.arrow_trx);
    c.arrow_try = jnum(d, "arrow_try", c.arrow_try);
    c.arrow_trz = jnum(d, "arrow_trz", c.arrow_trz);
    c.gun_x = jnum(d, "gun_x", c.gun_x);
    c.gun_y = jnum(d, "gun_y", c.gun_y);
    c.gun_z = jnum(d, "gun_z", c.gun_z);
    c.gun_rx = jnum(d, "gun_rx", c.gun_rx);
    c.gun_ry = jnum(d, "gun_ry", c.gun_ry);
    c.gun_rz = jnum(d, "gun_rz", c.gun_rz);
    c.insert_blend = jnum(d, "insert_blend", c.insert_blend);
    c.string_pose = jstr(d, "string_pose", c.string_pose);
    c.string_pose_near = jbool(d, "string_pose_near", c.string_pose_near);
    c.dock_x = jnum(d, "dock_x", c.dock_x);
    c.dock_y = jnum(d, "dock_y", c.dock_y);
    c.dock_z = jnum(d, "dock_z", c.dock_z);
    c.dock_rx = jnum(d, "dock_rx", c.dock_rx);
    c.dock_ry = jnum(d, "dock_ry", c.dock_ry);
    c.dock_rz = jnum(d, "dock_rz", c.dock_rz);
    c.dock_preview = jbool(d, "dock_preview", c.dock_preview);
    c.snd_seg = jnum(d, "snd_seg", c.snd_seg);
    c.snd_drop_delay = jnum(d, "snd_drop_delay", c.snd_drop_delay);
}

void RE4VRReload5::xbow_save_cfg() {
    const auto& c = m_bowcfg;
    nlohmann::json o = {
        {"enabled", c.enabled}, {"reload_ammo", c.reload_ammo},
        {"sound_enabled", c.sound_enabled},
        {"draw_smooth", c.draw_smooth}, {"dock_blend_speed", c.dock_blend_speed},
        {"latch_hold_sec", c.latch_hold_sec}, {"insert_distance", c.insert_distance},
        {"insert_x", c.insert_x}, {"insert_y", c.insert_y}, {"insert_z", c.insert_z},
        {"draw_grab_dist", c.draw_grab_dist}, {"draw_z", c.draw_z},
        {"draw_need", c.draw_need},
        {"arrow_x", c.arrow_x}, {"arrow_y", c.arrow_y}, {"arrow_z", c.arrow_z},
        {"arrow_rx", c.arrow_rx}, {"arrow_ry", c.arrow_ry}, {"arrow_rz", c.arrow_rz},
        {"arrow_part", c.arrow_part}, {"arrow_preview", c.arrow_preview},
        {"arrow_pose", c.arrow_pose},
        {"arrow_trx", c.arrow_trx}, {"arrow_try", c.arrow_try},
        {"arrow_trz", c.arrow_trz},
        {"gun_x", c.gun_x}, {"gun_y", c.gun_y}, {"gun_z", c.gun_z},
        {"gun_rx", c.gun_rx}, {"gun_ry", c.gun_ry}, {"gun_rz", c.gun_rz},
        {"insert_blend", c.insert_blend},
        {"string_pose", c.string_pose}, {"string_pose_near", c.string_pose_near},
        {"dock_x", c.dock_x}, {"dock_y", c.dock_y}, {"dock_z", c.dock_z},
        {"dock_rx", c.dock_rx}, {"dock_ry", c.dock_ry}, {"dock_rz", c.dock_rz},
        {"dock_preview", c.dock_preview},
        {"snd_seg", c.snd_seg}, {"snd_drop_delay", c.snd_drop_delay},
    };

    re4vr::json_save(BCFG_PATH, o);
}

// ---- [PART-KLON] Bolzen als Mesh-Kopie an der linken Hand ------------------
::REManagedObject* RE4VRReload5::xbow_gun_mesh() {
    auto* hu = re4vr::call_safe<::REManagedObject*>(get_ctx(), "get_HeadUpdater");
    auto* gun = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");

    return re4vr::call_safe<::REManagedObject*>(gun, "get_Mesh");
}

void RE4VRReload5::xbow_clone_destroy() {
    if (m_bclone.obj != nullptr) {
        re4vr::destroy_game_object(m_bclone.obj);
    }

    m_bclone.obj = nullptr;
    m_bclone.mesh = nullptr;
    m_bclone.parented = false;
}

bool RE4VRReload5::xbow_clone_spawn() {
    if (m_bclone.obj != nullptr) {
        return true;
    }

    auto* gmesh = xbow_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_bow_bolt");

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
    m_bclone.obj = gom;
    m_bclone.mesh = mesh;

    // [NO_LAG] nativ ans L_Hand parenten -> kein Render-Versatz beim Laufen,
    // danach nur noch die lokale Pose setzen.
    m_bclone.parented = false;
    auto* ctf = re4vr::call_safe<::REManagedObject*>(gom, "get_Transform");
    auto* bt2 = body_tf();

    if (ctf != nullptr && bt2 != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", bt2);
        auto* jn = sdk::VM::create_managed_string(L"L_Hand");

        if (jn != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_ParentJoint", jn);
            m_bclone.parented = true;
        }
    }

    // nur den Bolzen-Part sichtbar lassen
    for (int32_t i = 0; i <= 40; ++i) {
        re4vr::call_safe<void*>(mesh, "setPartsEnable", i, i == m_bowcfg.arrow_part);
    }

    return true;
}

void RE4VRReload5::xbow_clone_pose() {
    if (m_bclone.obj == nullptr) {
        return;
    }

    auto* ctf = re4vr::call_safe<::REManagedObject*>(m_bclone.obj, "get_Transform");

    if (ctf == nullptr) {
        return;
    }

    const auto& c = m_bowcfg;
    set_vec3(ctf, "set_LocalPosition", glm::vec3{c.arrow_x, c.arrow_y, c.arrow_z});
    set_quat(ctf, "set_LocalRotation",
             quat_from_euler(c.arrow_rx, c.arrow_ry, c.arrow_rz));
    set_vec3(ctf, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
}

void RE4VRReload5::xbow_refresh() {
    const auto wid = get_equip_wid();

    if (!wid.has_value() || !is_xbow(*wid)) {
        m_bowwep.wid = std::nullopt;
        m_bowwep.tf = nullptr;
        m_bowwep.sj = nullptr;
        m_bowwep.aj = nullptr;

        return;
    }

    if (m_bowwep.wid != wid || m_bowwep.tf == nullptr) {
        // [PORTFEHLER 18.09.2026 -- GEMESSEN] find_weapon() gibt hier die
        // TRANSFORM zurueck (return child), nicht das GameObject. Das
        // zusaetzliche get_Transform lieferte darum immer nullptr -> tf und in
        // der Folge der Sehnen-Joint blieben leer, near_str war nie wahr und die
        // Sehne liess sich NIE greifen (Spur: "sj=0 dist=-1.000 near=0").
        // In der Lua-Vorlage gab find_weapon zwei Werte zurueck (go, tf) und das
        // get_Transform stand dort auf dem GO -- beim Port ist es mitgewandert.
        // Die beiden anderen Aufrufer (Z. 1003, 2917) nehmen den Rueckgabewert
        // bereits direkt als Transform.
        m_bowwep.wid = wid;
        m_bowwep.tf = find_weapon(*wid);
        m_bowwep.sj = nullptr;
        m_bowwep.aj = nullptr;
        m_bowwep.s_rest = std::nullopt;
        m_bowwep.a_rest = std::nullopt;
    }

    if (m_bowwep.tf == nullptr) {
        return;
    }

    if (m_bowwep.sj == nullptr) {
        m_bowwep.sj = joint_by_name(m_bowwep.tf, J_STRING);
        glm::vec3 lp{};

        // [RUHE HARTKODIERT 2026-07-20] Die Ruhelage NICHT live uebernehmen:
        // wird sie erfasst, waehrend die Sehne schon gespannt ist (z = -0.015),
        // rechnet der Zug von dort nochmal den vollen Weg -> die Sehne landete
        // bei -0.386, weit hinter dem Anschlag. x/y bleiben live.
        if (m_bowwep.sj != nullptr && get_vec3(m_bowwep.sj, "get_LocalPosition", lp)) {
            m_bowwep.s_rest = glm::vec3{lp.x, lp.y, STRING_REST_Z};
        }
    }

    if (m_bowwep.aj == nullptr) {
        m_bowwep.aj = joint_by_name(m_bowwep.tf, J_ARROW);
        glm::vec3 lp{};

        if (m_bowwep.aj != nullptr && get_vec3(m_bowwep.aj, "get_LocalPosition", lp)) {
            m_bowwep.a_rest = lp;
        }
    }
}

// [ACCESSOR] zuerst die ECHTE, persistente Instanz; alles darunter sind KOPIEN.
::REManagedObject* RE4VRReload5::get_live_wi() {
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

::REManagedObject* RE4VRReload5::xbow_wi() {
    return re4vr::call_safe<::REManagedObject*>(get_pe(), "getEquipWeaponItem");
}

int32_t RE4VRReload5::xbow_loaded_ammo() {
    return call_enum(get_pe(), "getCurrentGunAmmo").value_or(0);
}

int32_t RE4VRReload5::xbow_reserve() {
    auto* wi = xbow_wi();

    if (wi == nullptr || m_main == nullptr) {
        return 0;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(get_pe(), "get_InventoryController");
    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    if (inv == nullptr || !ammo_id.has_value()) {
        return 0;
    }

    return m_main->item_count_sum(inv, *ammo_id);
}

void RE4VRReload5::xbow_snd(uint32_t id) {
    if (!m_bowcfg.sound_enabled || id == 0) {
        return;
    }

    trigger_sound(re4vr::call_safe<::REManagedObject*>(m_bowwep.tf, "get_GameObject"), id);
}

// Eine Einheit nachbuchen (Muster wie ueberall: nur ueber die echte Liste).
void RE4VRReload5::xbow_book_one() {
    if (!m_bowcfg.reload_ammo || m_main == nullptr) {
        return;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(get_pe(), "get_InventoryController");
    const auto et = r9_equip_type_main();

    if (inv != nullptr && et.has_value()) {
        // [CRASH-HARDEN] enableReloadItem-Gate gegen den null-Item-AV
        m_main->load_and_book(inv, *et, 1, false);
    }
}

// ---- Holster: Bolzen in die Hand / wieder weg ------------------------------
bool RE4VRReload5::xbow_set_bolt_in_hand(bool active) {
    if (!(m_bowcfg.enabled && m_bowwep.wid.has_value())) {
        return false;
    }

    if (active) {
        if (m_bowst.arrow_in_hand || m_bowst.loaded) {
            return false;
        }

        // [ERST SPANNEN 2026-07-20] Die gespannte Sehne ist das Signal, dass ein
        // Bolzen geholt werden darf -- vorher gibt der Mag-Holster nichts her.
        if (!m_bowst.drawn) {
            return false;
        }

        if (xbow_reserve() <= 0) {
            return false;   // keine Reserve -> nichts zu holen
        }

        m_bowst.arrow_in_hand = true;
        xbow_clone_spawn();   // [PART-KLON] sichtbarer Bolzen an die Hand
        xbow_snd(BSND_HOLSTER_GRAB);

        return true;
    }

    // [DROP-SOUND] Losgelassen, ohne eingelegt zu haben -> der Bolzen faellt zu
    // Boden. Der Aufprall kommt verzoegert (Fallzeit), sonst klingt er direkt an
    // der Hand.
    if (m_bowst.arrow_in_hand) {
        m_bsnd.drop_at = clock_now() + m_bowcfg.snd_drop_delay;
    }

    m_bowst.arrow_in_hand = false;
    xbow_clone_destroy();

    return true;
}

// ---- Render-Pass: Sehne halten, Bolzen setzen ------------------------------
void RE4VRReload5::xbow_apply_pass() {
    if (!(m_bowcfg.enabled && m_bowwep.wid.has_value() && m_bowwep.tf != nullptr)) {
        return;
    }

    // [SEHNE HALTEN 2026-07-20] Der Bolzen-in-Hand-Zweig ist frueher mit return
    // ausgestiegen -> die gespannte Sehne wurde nicht mehr geschrieben und die
    // Engine hat sie nach vorne zurueckgestellt (sichtbar: Sehne snappt beim
    // Griff zum Holster auf). Jetzt wird der Bolzen an die Hand gesetzt UND die
    // Sehne weiter auf ihrer Position gehalten.
    if (m_bowst.arrow_in_hand || (m_bowcfg.arrow_preview && m_bclone.obj != nullptr)) {
        // [PART-KLON] Der Klon haengt nativ am L_Hand-Joint -> nur die LOKALE
        // Pose setzen.
        xbow_clone_pose();

        if (m_bowwep.sj != nullptr && m_bowwep.s_rest.has_value()
            && m_bowst.frac > 0.001f) {
            float zz = STRING_REST_Z + (STRING_DRAWN_Z - STRING_REST_Z) * m_bowst.frac;
            zz = std::clamp(zz, STRING_DRAWN_Z, STRING_REST_Z);
            set_vec3(m_bowwep.sj, "set_LocalPosition",
                     glm::vec3{m_bowwep.s_rest->x, m_bowwep.s_rest->y, zz});
        }

        return;
    }

    // [NUR WENN WIR DRAN SIND 2026-07-19] Solange wir weder einen Bolzen in der
    // Hand halten noch am Spannen sind, GAR NICHTS schreiben -- die Engine
    // stellt Sehne und Bolzen selbst korrekt dar (HUD zeigt geladen -> nativ ist
    // gespannt). Vorher hat dieser Pass beide Joints jeden Frame auf die beim
    // ersten Frame erfassten Ruhelagen gezwungen.
    if (!m_bowst.grab && m_bowst.frac <= 0.001f) {
        return;
    }

    // [ANSCHLAG 2026-07-20] Zielposition zwischen den beiden GEMESSENEN
    // Anschlaegen interpolieren und hart klemmen -- so kann die Sehne
    // konstruktiv nie hinter den gespannten Punkt rutschen.
    float zt = STRING_REST_Z + (STRING_DRAWN_Z - STRING_REST_Z) * m_bowst.frac;
    zt = std::clamp(zt, STRING_DRAWN_Z, STRING_REST_Z);

    if (m_bowwep.sj != nullptr && m_bowwep.s_rest.has_value()) {
        set_vec3(m_bowwep.sj, "set_LocalPosition",
                 glm::vec3{m_bowwep.s_rest->x, m_bowwep.s_rest->y, zt});
    }

    // [BOLZEN SELBST SETZEN 2026-07-20] Die Engine stellt den geladenen Bolzen
    // NICHT dar: _04 bleibt auf 0/0/0, weil sie ihn nur ueber ihre native
    // Ladeanimation setzt -- und die spielen wir nie ab. (Der Bolzen in der HAND
    // ist ein eigener Mesh-Klon und davon unberuehrt.)
    if (m_bowst.loaded) {
        // [KEIN CACHE 2026-07-20] Den Bolzen-Joint JEDEN Pass frisch aufloesen:
        // er existiert erst wieder, wenn ein Bolzen geladen ist -- ein einmal
        // gecachter (toter) Zeiger zeigte ins Leere.
        auto* aj = joint_by_name(m_bowwep.tf, J_ARROW);

        if (aj == nullptr) {
            aj = m_bowwep.aj;
        }

        if (aj != nullptr) {
            m_bowwep.aj = aj;
            set_vec3(aj, "set_LocalPosition",
                     glm::vec3{0.0f, BOLT_LOADED_Y, BOLT_LOADED_Z});
        }

        // ... UND den Mesh-Part wieder einschalten: nach dem Schuss blendet die
        // Engine den Bolzen-Part aus, deshalb blieb die Rinne leer, obwohl Ammo
        // 1 war.
        if (auto* gm = xbow_gun_mesh(); gm != nullptr) {
            re4vr::call_safe<void*>(gm, "setPartsEnable", m_bowcfg.arrow_part, true);
        }
    }
}

void RE4VRReload5::xbow_on_frame() {
    xbow_refresh();

    if (!(m_bowcfg.enabled && m_bowwep.wid.has_value())) {
        if (m_bow_prev_wid.has_value()) {
            re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
            // [BF-DIAG] wer hat den Feuer-Block zuletzt gesetzt?
            re4vr::lua_set_string("__re4_bf_who", "reload5/Bogen aus");
            re4vr::lua_set_bool("__vr_needs_rack", false);
            re4vr::lua_set_bool("__vr_mag_in_hand", false);
            re4vr::lua_set_bool("__re4_reload_grab_empty", false);
            re4vr::lua_set_nil("__vr_rack_hand_pose");
            re4vr::lua_set_nil("__vr_mag_hand_pose");
            re4vr::lua_set_nil("__vr_mag_hand_trx");
            re4vr::lua_set_nil("__vr_mag_hand_try");
            re4vr::lua_set_nil("__vr_mag_hand_trz");
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
            m_bowst.arrow_in_hand = false;
            xbow_clone_destroy();
            m_bowst.loaded = false;
            m_bowst.drawn = false;
            m_bowst.frac = 0.0f;
            m_bow_prev_wid = std::nullopt;
        }

        return;
    }

    m_bow_prev_wid = m_bowwep.wid;

    const int32_t ld = xbow_loaded_ammo();

    if (m_bow_prev_loaded.has_value() && ld < *m_bow_prev_loaded) {
        m_bowst.loaded = false;
        m_bowst.drawn = false;
        m_bowst.frac = 0.0f;
        xbow_clone_destroy();   // verschossen -> Klon weg
    }

    m_bow_prev_loaded = ld;
    // [ENGINE IST DIE WAHRHEIT 2026-07-20] strikt aus getCurrentGunAmmo
    m_bowst.loaded = (ld > 0);

    // ---- Einlegen ---------------------------------------------------------
    if (m_bowst.arrow_in_hand && m_bowwep.tf != nullptr) {
        // [EINLEGEPUNKT] Gemessen wird die Position des BOLZEN-KLONS (nicht die
        // Handmitte) gegen den Einlegepunkt an der Waffe. Beides Weltpositionen.
        std::optional<glm::vec3> hp{};

        if (m_bclone.obj != nullptr) {
            auto* ctf = re4vr::call_safe<::REManagedObject*>(m_bclone.obj, "get_Transform");
            glm::vec3 p{};

            if (ctf != nullptr && get_vec3(ctf, "get_Position", p)) {
                hp = p;
            }
        }

        if (!hp.has_value()) {
            hp = left_hand_world_g();
        }

        // [KEYFRAME-ANKER 2026-09-02] Gemessen wird gegen KEYFRAME #1 --
        // denselben Punkt, an dem die Uebergabe beginnt. Vorher lag hier ein
        // handgemessener Offset auf Joint _03, der mit dem Ziel des Bolzens
        // NICHTS zu tun hatte: der Klon kam nie naeher als ~0.176 an ihn heran,
        // das Limit stand auf 0.180 -- jede Uebergabe war ein Zufallstreffer,
        // und ein groesseres Limit machte es schlimmer. Jetzt ist Messpunkt ==
        // Bahnstart, der Abstand geht beim Zielen also wirklich gegen 0.
        std::optional<glm::vec3> wp{};

        {
            glm::vec3 gp{};
            glm::quat gr{};

            if (m_adv != nullptr && get_vec3(m_bowwep.tf, "get_Position", gp)
                && get_quat(m_bowwep.tf, "get_Rotation", gr)) {
                RE4VRReloadAdv::Key k{};

                if (m_adv->shell_pose_at(BOW_WID, 0.0f, k)) {   // 0.0 = Keyframe #1
                    wp = gp + (gr * glm::vec3{k.x, k.y, k.z});
                }
            }
        }

        // Kein Keyframe lesbar -> ALTER Weg (Joint _03 + Offset), damit nichts
        // still ausfaellt.
        if (!wp.has_value()) {
            auto* ij = joint_by_name(m_bowwep.tf, J_INSERT);
            glm::vec3 jp{};
            glm::quat jr{};

            if (ij != nullptr && get_vec3(ij, "get_Position", jp)) {
                if (get_quat(ij, "get_Rotation", jr)) {
                    wp = jp + (jr * glm::vec3{m_bowcfg.insert_x, m_bowcfg.insert_y,
                                              m_bowcfg.insert_z});
                } else {
                    wp = jp;
                }
            }
        }

        if (!wp.has_value()) {
            glm::vec3 p{};

            if (get_vec3(m_bowwep.tf, "get_Position", p)) {
                wp = p;
            }
        }

        if (hp.has_value() && wp.has_value()) {
            const float dist = glm::length(*hp - *wp);
            m_bow_insert_dist = dist;
            re4vr::lua_set_number("__re4_bow_insert_dist", dist);

            if (dist <= m_bowcfg.insert_distance) {
                m_bowst.arrow_in_hand = false;
                // [SOFORT-TAUSCH 2026-09-02 -- GEMESSEN] Kein Animieren des
                // Klons mehr. Messung: der Klon haengt ueber set_ParentJoint an
                // "L_Hand", und dieser ParentJoint laesst sich NICHT loesen --
                // pjoint=JA in allen 149 Messzeilen, obwohl set_ParentJoint(nil)
                // gerufen wurde. Die Engine zieht ihn dadurch in
                // UpdateMotion/LateUpdateBehavior mit der Hand mit (rund 0.075 m
                // pro Frame), waehrend unsere Bahn ihn auf die Weltpose setzte --
                // zwei Schreiber, sichtbar als Zappeln. Deshalb: nicht bewegen,
                // sondern TAUSCHEN. Der Klon wird sofort zerstoert, die Munition
                // gebucht; der native Bolzen _04 steht ohnehin auf genau dieser
                // Stelle (Keyframe #2 = BOLT_LOADED_Y/Z).
                xbow_book_one();
                xbow_clone_destroy();
                xbow_snd(BSND_INSERT);
                // [ENGINE IST DIE WAHRHEIT 2026-07-20] loaded NICHT hart setzen:
                // es wurde auch dann true, wenn die Ammo-Buchung fehlschlug ->
                // Holster sperrte ("schon geladen"), geschossen werden konnte
                // mangels Munition trotzdem nicht.
                // [SPANNUNG BLEIBT] drawn/frac bleiben unangetastet -- zuerst
                // spannen, dann einlegen.
            }
        } else {
            m_bow_insert_dist = std::nullopt;
            re4vr::lua_set_nil("__re4_bow_insert_dist");
        }
    }

    // [STAGGER_HEAL 2026-07-20] Wird man mitten im Spannen getroffen, bricht der
    // Zug ab und die Sehne bliebe auf einem Zwischenwert stehen ("halb acht") --
    // dasselbe Problem, das die Pistolen-Slides hatten. Auf der FALLENDEN Flanke
    // von __re4_damage_active daher definiert aufraeumen: nicht fertig gespannt
    // -> zurueck in die Ruhe; fertig gespannt -> bleibt gespannt.
    {
        const bool dmg = re4vr::lua_get_tribool("__re4_damage_active") == 1;

        if (m_bowst.prev_dmg && !dmg) {
            if (!m_bowst.drawn) {
                m_bowst.frac = 0.0f;
                m_bowst.grab = false;
                m_bowst.anchor = std::nullopt;
                m_bowst.wanchor = std::nullopt;
            } else {
                m_bowst.frac = 1.0f;
            }
        }

        // Waehrend des Staggers keinen neuen Zug starten und den laufenden
        // einfrieren.
        if (dmg) {
            m_bowst.grab = false;
            m_bowst.anchor = std::nullopt;
            m_bowst.wanchor = std::nullopt;
        }

        m_bowst.prev_dmg = dmg;
    }

    // [SOUND-TICK] (a) faelliger Boden-Aufprall des fallengelassenen Bolzens,
    // (b) Spann-Segment: solange die Sehne unter Zug WAECHST, das kurze Segment
    // im Intervall neu anspielen (es ist kein echter Loop) -- Muster wie der
    // Twirl-Sound.
    {
        const double now = clock_now();

        if (m_bsnd.drop_at > 0.0 && now >= m_bsnd.drop_at) {
            m_bsnd.drop_at = 0.0;
            xbow_snd(BSND_DROP_FLOOR);
        }

        if (m_bowst.grab && !m_bowst.drawn) {
            if (now >= m_bsnd.next_seg) {
                xbow_snd(BSND_DRAW_SEG);
                m_bsnd.next_seg = now + std::max(0.03f, m_bowcfg.snd_seg);
            }
        } else {
            m_bsnd.next_seg = 0.0;   // naechster Zug spielt sofort
        }
    }

    // [DRY-FIRE] Kein Bolzen drin ODER Sehne nicht gespannt -> Feuern ist
    // gesperrt (__vr_block_fire_when_empty weiter unten), binding meldet den
    // gezogenen Trigger.
    {
        const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

        if (et && !m_bsnd.dry_prev) {
            xbow_snd(BSND_DRY_FIRE);
        }

        m_bsnd.dry_prev = et;
    }

    // ---- Spann-Geste ------------------------------------------------------
    const bool grip = left_grip_down();
    std::optional<glm::vec3> sp{};

    if (m_bowwep.sj != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_bowwep.sj, "get_Position", p)) {
            sp = p;
        }
    }

    // [ZWEI POSITIONEN 2026-07-20] Bewusst getrennt:
    // near -> SICHTBARE Hand: sie steht am Griff, danach richtet sich, ob man
    //   ueberhaupt greifen darf. Mit der rohen Controller-Pos war der Abstand
    //   ein ganz anderer -> near wurde nie wahr und das Greifen war tot.
    // Zug  -> ROHER Controller: sobald das Dock greift, ist die sichtbare Hand
    //   unsere eigene Vorgabe und wuerde sich selbst messen (snappy). Gleiche
    //   Ursache wie beim Messer-Holster.
    const auto hvis = left_hand_world_g();
    const auto hp = re4vr::lua_get_vec3_any({"__vr_lh_ctrl_raw", "__vr_lh_world",
                                             "__vr_unified_lh_pos", "__vr_lh_joint_pos"});
    bool near_str = false;

    if (sp.has_value() && hvis.has_value()) {
        near_str = glm::length(*hvis - *sp) <= m_bowcfg.draw_grab_dist;
    }

    // [NACHGREIFEN ERLAUBT 2026-07-20] Frueher sperrte drawn das Greifen
    // komplett -- damit liess sich die Sehne nach dem ersten Spannen NIE wieder
    // anfassen. Gegen das Nach-vorne-Schieben schuetzt stattdessen die
    // Einbahn-Regel weiter unten: frac kann nur WACHSEN.
    const bool grip_edge = grip && !m_bowst.prev_grip;
    // [KEIN SPANNEN OHNE BOLZEN 2026-07-20] 0 geladen UND 0 Reserve -> die Sehne
    // laesst sich gar nicht erst greifen. Beide Werte live aus der Engine.
    const bool bow_has_ammo = (xbow_loaded_ammo() > 0) || (xbow_reserve() > 0);

    if (grip_edge && near_str && !m_bowst.drawn && !m_bowst.arrow_in_hand && bow_has_ammo) {
        m_bowst.grab = true;
        m_bowst.anchor = hp;
        // [LAUFEN 03.09.2026] Zweiter Anker: die WAFFENHAND. Der Anker oben ist
        // ein fester WELTpunkt -- laeuft man beim Spannen vorwaerts, wandert die
        // Ziehhand mit dem Koerper mit, der Weltpunkt bleibt stehen, und die
        // Fortbewegung steckt als Gegenzug in der Rechnung: pull wird negativ
        // und auf 0 geklemmt -> Spannen ging NUR im Stand. Exakt dieselbe Falle
        // und derselbe Fix wie beim Slide-Rack.
        m_bowst.wanchor = right_hand_raw_g();
    } else if (!grip) {
        m_bowst.grab = false;
        m_bowst.anchor = std::nullopt;
        m_bowst.wanchor = std::nullopt;
    }

    m_bowst.prev_grip = grip;

    if (m_bowst.drawn && !m_bowst.grab) {
        m_bowst.frac = 1.0f;   // gespannt bleibt gespannt (nur der Schuss loest sie)
    } else if (m_bowst.grab && m_bowst.anchor.has_value() && hp.has_value()) {
        // [ZUG ENTLANG DER WAFFE 2026-07-19] Vorher: 3D-Distanz zum Greifpunkt
        // -> JEDE Handbewegung (auch seitlich/hoch) hat gespannt, das wirkte
        // snappy und unkontrollierbar. Jetzt wird die Handbewegung auf die
        // WAFFENACHSE projiziert (Skalarprodukt): nur der Anteil nach hinten
        // entlang der Waffe zaehlt -> die Sehne folgt 1:1 dem Controller.
        glm::vec3 d = *hp - *m_bowst.anchor;

        // [LAUFEN 03.09.2026] Bewegung der Waffenhand abziehen -> uebrig bleibt
        // die Bewegung der Ziehhand GEGEN die Waffe, unabhaengig davon ob man
        // steht, laeuft oder sich dreht. Rueckbau: _G.__re4_bow_relative = false
        if (m_bowst.wanchor.has_value()
            && re4vr::lua_get_tribool("__re4_bow_relative") != 0) {
            if (const auto rhn = right_hand_raw_g(); rhn.has_value()) {
                d -= (*rhn - *m_bowst.wanchor);
            }
        }

        float pull = 0.0f;
        glm::quat wr{};

        if (get_quat(m_bowwep.tf, "get_Rotation", wr)) {
            const glm::vec3 ax = wr * glm::vec3{0.0f, 0.0f, 1.0f};
            const float al = glm::length(ax);

            if (al > 1e-6f) {
                // Sehne faehrt in -Z (gespannt z=-0.015 gegen Ruhe z=+0.356) ->
                // Zug nach hinten ist die NEGATIVE Achsrichtung; darum das Minus.
                pull = -(glm::dot(d, ax) / al);
            }
        } else {
            pull = glm::length(d);   // Fallback wie bisher
        }

        if (pull < 0.0f) {
            pull = 0.0f;
        }

        const float full = std::abs(m_bowcfg.draw_z);
        const float target = std::min(1.0f, (full > 0.001f) ? (pull / full) : 0.0f);

        // [EINBAHN] nur nach hinten: ein kleinerer Zielwert wird ignoriert -> die
        // Sehne laesst sich nicht nach vorne schieben (das macht allein der Schuss).
        if (target > m_bowst.frac) {
            m_bowst.frac += (target - m_bowst.frac) * m_bowcfg.draw_smooth;
        }

        // [EINRASTEN 2026-07-20] NICHT an loaded knuepfen: bei der Armbrust wird
        // ZUERST gespannt und DANN der Bolzen eingelegt. Mit der alten Bedingung
        // rastete die Sehne ohne Bolzen nie ein und schnellte beim Loslassen
        // zurueck.
        if (m_bowst.frac >= m_bowcfg.draw_need) {
            m_bowst.drawn = true;
            m_bowst.frac = 1.0f;
        }
    } else if (!m_bowst.drawn) {
        m_bowst.frac = 0.0f;
    } else {
        m_bowst.frac = 1.0f;
    }

    // [KRITISCHES GATE -- LIVE 2026-07-20] Feuer-Block NUR bei echt leerer Waffe.
    // Vorher standen hier SCRIPT-Flags: stand die Sehne aus Sicht des Scripts
    // nicht auf "drawn" (oder loaded stale), war trotz Bolzen im Lauf
    // Dauer-Dry-Fire. Regel: steht in der aktuellen Munition (nicht Reserve) eine
    // 1, wird NIE gesperrt. Quelle daher die frische Engine-Ammo, kein
    // Script-Zustand.
    re4vr::lua_set_bool("__vr_block_fire_when_empty", xbow_loaded_ammo() <= 0);
    re4vr::lua_set_string("__re4_bf_who", "reload5/Bogen leer");
    // [KEIN needs_rack 2026-07-19] motion reisst damit die Support-Hand sofort
    // vom Dock (dort ist es das Signal "Hand muss zum Slide"). Bei der Armbrust
    // spannt dieselbe linke Hand ohnehin an Ort und Stelle.
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_mag_in_hand", m_bowst.arrow_in_hand);

    // [SETUP-VORSCHAU] Klon auch ohne Holster-Griff zeigen, solange die Vorschau
    // an ist.
    if (m_bowcfg.arrow_preview && !m_bowst.arrow_in_hand) {
        xbow_clone_spawn();
    } else if (!m_bowcfg.arrow_preview && !m_bowst.arrow_in_hand
               && m_bclone.obj != nullptr) {
        xbow_clone_destroy();
    }

    // [SEHNEN-POSE] motion wendet den publizierten Namen im POST-ANIM-Pass an
    // (wie bei den Rack-Posen der anderen Waffen). Nur wenn kein Bolzen in der
    // Hand ist -- der hat Vorrang.
    {
        bool want = false;

        if (!m_bowcfg.string_pose.empty() && !m_bowst.arrow_in_hand) {
            want = m_bowst.grab || m_bowcfg.dock_preview
                   || (m_bowcfg.string_pose_near && near_str);
        }

        // [BOLZEN-HANDPOSE] Bolzen in der Hand (oder Vorschau) laeuft ueber den
        // MAG-Pfad: der traegt in motion das additive Daumen-Tuning
        // (__vr_mag_hand_trx/try/trz). Die Sehnen-Pose bleibt am Rack-Pfad -- so
        // kommen sich beide nie ins Gehege.
        const bool holding = m_bowst.arrow_in_hand
                             || (m_bowcfg.arrow_preview && m_bclone.obj != nullptr);

        if (holding && !m_bowcfg.arrow_pose.empty()) {
            re4vr::lua_set_string("__vr_mag_hand_pose", m_bowcfg.arrow_pose);
            re4vr::lua_set_number("__vr_mag_hand_trx", m_bowcfg.arrow_trx);
            re4vr::lua_set_number("__vr_mag_hand_try", m_bowcfg.arrow_try);
            re4vr::lua_set_number("__vr_mag_hand_trz", m_bowcfg.arrow_trz);
            re4vr::lua_set_nil("__vr_rack_hand_pose");
        } else {
            re4vr::lua_set_nil("__vr_mag_hand_pose");
            re4vr::lua_set_nil("__vr_mag_hand_trx");
            re4vr::lua_set_nil("__vr_mag_hand_try");
            re4vr::lua_set_nil("__vr_mag_hand_trz");

            if (want) {
                re4vr::lua_set_string("__vr_rack_hand_pose", m_bowcfg.string_pose);
            } else {
                re4vr::lua_set_nil("__vr_rack_hand_pose");
            }
        }
    }

    // [SEHNEN-DOCK] Hand-Ziel an den Sehnen-Joint publizieren. Dieselben Globals,
    // die auch die Slide-Rack-Hand der anderen Waffen benutzt -> motion/arm_chain
    // ziehen die linke Hand dorthin. Aktiv beim echten Zug (grab) und im
    // Setup-Vorschaumodus; sonst freigeben.
    // [EINRAST-HALTEN] Zeitpunkt des Einrastens merken (siehe latch_hold_sec).
    if (m_bowst.drawn && !m_bowst.latch_t.has_value()) {
        m_bowst.latch_t = clock_now();
    }

    if (!m_bowst.drawn) {
        m_bowst.latch_t = std::nullopt;
    }

    const bool latching = m_bowst.latch_t.has_value()
                          && (clock_now() - *m_bowst.latch_t) < m_bowcfg.latch_hold_sec;
    bool docked = false;

    if (m_bowwep.sj != nullptr && (m_bowst.grab || latching || m_bowcfg.dock_preview)) {
        glm::vec3 jp{};
        glm::quat jr{};

        if (get_vec3(m_bowwep.sj, "get_Position", jp)
            && get_quat(m_bowwep.sj, "get_Rotation", jr)) {
            const glm::vec3 off = jr * glm::vec3{m_bowcfg.dock_x, m_bowcfg.dock_y,
                                                 m_bowcfg.dock_z};
            re4vr::lua_set_vec3("__vr_slide_hand_world_pos", jp + off);
            re4vr::lua_set_quat("__vr_slide_hand_world_rot",
                                glm::normalize(jr * quat_from_euler(m_bowcfg.dock_rx,
                                                                    m_bowcfg.dock_ry,
                                                                    m_bowcfg.dock_rz)));
            // [DOCK-LERP 2026-07-20] Blend hochlerpen statt hart auf 1.0 -> die
            // Hand wandert weich an den Sehnengriff, statt hinzuspringen.
            m_bowst.dock_blend = std::min(1.0f,
                                          m_bowst.dock_blend + m_bowcfg.dock_blend_speed);
            re4vr::lua_set_number("__vr_slide_dock_blend_factor", m_bowst.dock_blend);
            re4vr::lua_set_bool("__vr_slide_rack_active", true);
            docked = true;
        }
    }

    if (!docked) {
        // Ausblenden ebenfalls weich (gleiches Tempo), erst dann Ziel freigeben.
        m_bowst.dock_blend = std::max(0.0f,
                                      m_bowst.dock_blend - m_bowcfg.dock_blend_speed);
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", m_bowst.dock_blend);

        if (m_bowst.dock_blend <= 0.001f) {
            re4vr::lua_set_nil("__vr_slide_hand_world_pos");
            re4vr::lua_set_nil("__vr_slide_hand_world_rot");
            re4vr::lua_set_bool("__vr_slide_rack_active", false);
        }
    }

    re4vr::lua_set_bool("__re4_reload_grab_empty",
                        (xbow_reserve() <= 0) && !m_bowst.arrow_in_hand);
}

void RE4VRReload5::xbow_on_script_reset() {
    xbow_clone_destroy();   // [PART-KLON] kein Waise beim Script-Reset
    re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    re4vr::lua_set_string("__re4_bf_who", "reload5/Bogen reset");
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
    re4vr::lua_set_bool("__re4_reload_grab_empty", false);
}

// ---- UI (Menue: "RE4VR - Reload 5 (Separate Ways)" -> "Blast Crossbow") ----
void RE4VRReload5::xbow_ui() {
    auto& c = m_bowcfg;

    if (ImGui::Checkbox("Enable##bow", &c.enabled)) {
        xbow_save_cfg();
    }

    ImGui::Text("Equippt: %s | Bolzen in Hand: %s | geladen: %s | gespannt: %s (%.2f)",
                m_bowwep.wid.has_value() ? std::to_string(*m_bowwep.wid).c_str() : "nil",
                m_bowst.arrow_in_hand ? "true" : "false",
                m_bowst.loaded ? "true" : "false",
                m_bowst.drawn ? "true" : "false", m_bowst.frac);
    ImGui::Text("Joints gefunden: Sehne=%s  Bolzen=%s",
                (m_bowwep.sj != nullptr) ? "true" : "false",
                (m_bowwep.aj != nullptr) ? "true" : "false");

    if (ImGui::SliderFloat("Einlege-Distanz m (Einrasten)##bow", &c.insert_distance,
                           0.03f, 0.50f, "%.3f")) {
        xbow_save_cfg();
    }

    if (m_bow_insert_dist.has_value()) {
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "   aktueller Abstand Hand -> Einlegepunkt: %.3f m",
                           *m_bow_insert_dist);
    } else {
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                           "   (Bolzen in die Hand nehmen, dann zeigt sich der Abstand)");
    }

    ImGui::Text("Einlegepunkt (Offset ab Joint _03, ausgemessen):");

    if (ImGui::SliderFloat("Einlege X##bow", &c.insert_x, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Einlege Y##bow", &c.insert_y, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Einlege Z##bow", &c.insert_z, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    // [EINLEG-BLEND] Dauer der weichen Uebergabe Hand -> Laufrinne. Seit dem
    // SOFORT-TAUSCH ohne Wirkung, der Regler bleibt fuer die JSON stehen.
    if (ImGui::SliderFloat("Einleg-Blend (s)  [0 = hart]##bow", &c.insert_blend,
                           0.0f, 0.60f, "%.2f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Sehne Greif-Distanz m##bow", &c.draw_grab_dist,
                           0.03f, 0.40f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Sehnen-Zug Z (m)##bow", &c.draw_z, -0.40f, 0.0f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Gespannt ab Anteil##bow", &c.draw_need, 0.30f, 1.0f, "%.2f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Zug-Glaettung (1 = direkt)##bow", &c.draw_smooth,
                           0.05f, 1.0f, "%.2f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Hand-Andock-Tempo##bow", &c.dock_blend_speed,
                           0.02f, 1.0f, "%.2f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Einrasten: Hand haelt noch (s)##bow", &c.latch_hold_sec,
                           0.0f, 1.5f, "%.2f")) {
        xbow_save_cfg();
    }

    ImGui::Text("-- Sehnen-Handpose --");

    {
        char sb[64]{};
        std::snprintf(sb, sizeof(sb), "%s", c.string_pose.c_str());

        if (ImGui::InputText("Pose-Name (Sehne)##bow", sb, sizeof(sb))) {
            c.string_pose = sb;
            xbow_save_cfg();
        }
    }

    if (ImGui::Checkbox("Pose schon bei Naehe (sonst erst beim Ziehen)##bow",
                        &c.string_pose_near)) {
        xbow_save_cfg();
    }

    ImGui::Text("-- Sehnen-Dock (wohin die Hand greift) --");

    if (ImGui::Checkbox("Vorschau: Hand dauerhaft ans Dock##bow", &c.dock_preview)) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock X##bow", &c.dock_x, -0.30f, 0.30f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock Y##bow", &c.dock_y, -0.30f, 0.30f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock Z##bow", &c.dock_z, -0.30f, 0.30f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock RotX##bow", &c.dock_rx, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock RotY##bow", &c.dock_ry, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Dock RotZ##bow", &c.dock_rz, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    // [MESSUNG] Live-Werte des Sehnen-Joints.
    {
        glm::vec3 lp{};

        if (m_bowwep.sj != nullptr && get_vec3(m_bowwep.sj, "get_LocalPosition", lp)) {
            char rest[32]{};

            if (m_bowwep.s_rest.has_value()) {
                std::snprintf(rest, sizeof(rest), "z=%.4f", m_bowwep.s_rest->z);
            } else {
                std::snprintf(rest, sizeof(rest), "-");
            }

            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                               "Sehne _01 LocalPos: x=%.4f y=%.4f z=%.4f   (Ruhe erfasst: %s)",
                               lp.x, lp.y, lp.z, rest);

            if (ImGui::Button("Aktuelle Sehnen-Z als RUHE merken##bow")) {
                m_bowwep.s_rest = lp;
            }
        }
    }

    ImGui::Text("-- Bolzen in der Hand (Mesh-Part-Klon) --");

    if (ImGui::Checkbox("Vorschau: Bolzen dauerhaft in der Hand##bow", &c.arrow_preview)) {
        xbow_save_cfg();
    }

    {
        char ab[64]{};
        std::snprintf(ab, sizeof(ab), "%s", c.arrow_pose.c_str());

        if (ImGui::InputText("Bolzen-Handpose##bow", ab, sizeof(ab))) {
            c.arrow_pose = ab;
            xbow_save_cfg();
        }
    }

    if (ImGui::SliderInt("Bolzen Mesh-Part##bow", &c.arrow_part, 0, 40)) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen X##bow", &c.arrow_x, -0.30f, 0.30f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen Y##bow", &c.arrow_y, -0.30f, 0.30f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen Z##bow", &c.arrow_z, -0.30f, 0.30f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen RotX##bow", &c.arrow_rx, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen RotY##bow", &c.arrow_ry, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen RotZ##bow", &c.arrow_rz, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    // [BOLZEN IN DER WAFFE] Seit dem SOFORT-TAUSCH bleibt kein Klon mehr in der
    // Waffe; die Regler bleiben fuer die JSON stehen.
    ImGui::Text("-- Bolzen IN DER WAFFE (Offset ab Sehnen-Joint) --");

    if (ImGui::SliderFloat("In-Gun X##bow", &c.gun_x, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("In-Gun Y##bow", &c.gun_y, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("In-Gun Z##bow", &c.gun_z, -0.50f, 0.50f, "%.4f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("In-Gun RotX##bow", &c.gun_rx, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("In-Gun RotY##bow", &c.gun_ry, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("In-Gun RotZ##bow", &c.gun_rz, -180.0f, 180.0f, "%.1f")) {
        xbow_save_cfg();
    }

    ImGui::Text("Bolzen-Daumen (additiv auf die Pose, Grad):");

    if (ImGui::SliderFloat("Daumen RotX##bow", &c.arrow_trx, -90.0f, 90.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Daumen RotY##bow", &c.arrow_try, -90.0f, 90.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Daumen RotZ##bow", &c.arrow_trz, -90.0f, 90.0f, "%.1f")) {
        xbow_save_cfg();
    }

    if (ImGui::Checkbox("Ammo beim Einlegen nachladen##bow", &c.reload_ammo)) {
        xbow_save_cfg();
    }

    ImGui::Text("-- Sounds --");

    if (ImGui::Checkbox("Sounds an##bow", &c.sound_enabled)) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Spann-Segment Laenge (s)##bow", &c.snd_seg,
                           0.03f, 1.00f, "%.3f")) {
        xbow_save_cfg();
    }

    if (ImGui::SliderFloat("Bolzen-Aufprall Verzoegerung (s)##bow", &c.snd_drop_delay,
                           0.0f, 2.0f, "%.2f")) {
        xbow_save_cfg();
    }
}

// ============================================================================
// Holster-Kette. In Lua wickelt jeder Block seine eigene Fassung um
// __re4_reload_set_mag_in_hand; der ZULETZT geladene Wrapper wird als erster
// gefragt. Reihenfolge in der Datei: Rifle, Bolt, Samurai Edge, Crossbow --
// also wird hier von hinten nach vorn geprueft.
// std::nullopt = keine unserer Waffen -> der Aufrufer reicht an reload4 weiter.
// ============================================================================
std::optional<bool> RE4VRReload5::set_mag_in_hand(bool active) {
    const auto wid = get_equip_wid();

    if (!wid.has_value()) {
        return std::nullopt;
    }

    if (is_xbow(*wid)) {
        return xbow_set_bolt_in_hand(active);
    }

    if (is_red9(*wid)) {
        return red9_set_in_hand(active);
    }

    if (is_bolt(*wid)) {
        return bolt_set_cart_in_hand(active);
    }

    if (is_rifle(*wid)) {
        return rifle_set_mag_in_hand(active);
    }

    return std::nullopt;
}

// ============================================================================
// Dispatch. Die Reihenfolge ist die Registrierungsreihenfolge in Lua: die
// on_frame-Callbacks laufen in Ladereihenfolge, also Revolver -> Rifle -> Bolt
// -> Armbrust -> Bowdraw -> Red9. Wer zuletzt schreibt, gewinnt bei den
// geteilten Globals -- genau wie vorher.
// ============================================================================
void RE4VRReload5::on_frame() {
    // Fuer die Hooks gespiegelt: sie duerfen pro Aufruf nicht in den Lua-State
    // greifen (isEnableFire und SoundContainer.trigger feuern sehr oft).
    tick_merc_round();
    tick_saveload_reset();

    rifle_on_frame();
    bolt_on_frame();
    red9_on_frame();
    xbow_on_frame();

    update_fire_gate5();
}

// [RUNDEN-RESET] Neue Mercenaries-Runde -> Waffenzustand wegwerfen.
// SYMPTOM: die letzte Runde mit leerem Magazin verlassen -> in der neuen Runde
// ist die Waffe voll, das Modul will aber trotzdem nachladen/durchladen.
// URSACHE: der Zustand wird nur bei Waffenwechsel verworfen. Ein Rundenwechsel
// ist das nicht: das Spiel laedt die Map neu, die alten Objekte bleiben
// ansprechbar (Schreiben verpufft lautlos).
// TRIGGER: __re4_merc_round -- merc zaehlt es in der Ladeluecke hoch, in der der
// Body kurz gar nichts meldet. Ausserhalb Mercenaries aendert sich der Token nie.
// [SAVE_LOAD-RESET 19.09.2026] Sonde re4_saveload_sonde: die Body-Adresse
// springt NUR bei Save-Load/Tod (0,77 s ohne Body davor), nie im Spiel. Die
// alte Waffe bleibt danach oft noch lesbar -> der tf-Test im Refresh sah keinen
// Grund zum Neuholen. tf wegwerfen zwingt den vorhandenen [SAVE_LOAD]-Zweig.
void RE4VRReload5::tick_saveload_reset() {
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
    m_rifwep.tf = nullptr;
    m_bwep.tf = nullptr;
}

void RE4VRReload5::tick_merc_round() {
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
void RE4VRReload5::on_lock_scene_pre() {
    rifle_apply_pass();
    bolt_apply_pass();
    red9_apply_pass();
    xbow_apply_pass();
}

void RE4VRReload5::on_late_update() {
    rifle_apply_pass();
    bolt_apply_pass();
    red9_apply_pass();
    xbow_apply_pass();
}

void RE4VRReload5::on_update_joint_expression() {
    rifle_apply_pass();
    bolt_apply_pass();
    red9_apply_pass();

    // [VERSATZ BEIM LAUFEN 2026-09-07] Dieselbe Sache wie in RE4VRReload2:
    // die *_late-Funktionen fahren gespawnte Mesh-Part-Klone entlang ihrer
    // Bahn und standen bisher NUR im BeginRendering(POST). Danach bewegt die
    // Engine Hand und Waffe noch zweimal (LateUpdateBehavior,
    // UpdateJointExpression) -- der Klon bleibt stehen und sitzt beim Laufen
    // um eine konstante Frame-Strecke versetzt. Hier im LETZTEN Pass
    // nachziehen; Waffen-Joints brauchen das nicht, die propagiert die Engine.
    xbow_apply_pass();
}

void RE4VRReload5::on_begin_rendering_pre() {
    rifle_apply_pass();
    bolt_apply_pass();
    red9_apply_pass();
}

// [WOBBLE-FIX] NACH motions BeginRendering-POST (attach_left_hand): die geliehene
// Patrone bzw. der Bolzen-Dummy nochmal auf die finale VR-Hand.
// [BOGEN] In Lua haengt bow_apply zusaetzlich am BeginRendering-POST.
void RE4VRReload5::on_begin_rendering() {
    xbow_apply_pass();
}

// ============================================================================
// UI-Wurzel. In Lua EIN on_draw_ui mit dem Header "RE4VR - Reload2", darin die
// sechs Gattungen gestapelt.
// ============================================================================
// [EIN HAUPTTREE] Vorher zeichnete jede Gattung ihren eigenen Top-Level-Baum --
// mehrere Baeume fuer eine Datei, dazu Namen, die nicht zum Dateinamen passten.
// Jetzt: EIN Baum, alle vier Waffen als Untertrees darunter.
void RE4VRReload5::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Reload 5 (Separate Ways)")) {
        return;
    }

    if (ImGui::TreeNode("Anti-Materiel Rifle (wp6105)")) {
        rifle_ui();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Hunting Rifle (wp6114)")) {
        bolt_ui();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Samurai Edge (wp6113)")) {
        red9_ui();
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Blast Crossbow (wp6102)")) {
        xbow_ui();
        ImGui::TreePop();
    }

    ImGui::TreePop();
}

#endif
