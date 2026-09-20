// ============================================================================
// RE4VRMotion -- 1:1-Portierung von re4_vr_motion.lua. Siehe RE4VRMotion.hpp
// fuer Bausteine, Reihenfolge im Mod-Vektor und Publikationspflicht.
//
// Spezifikation: I:\LUATRANS\PORT_MOTION_SPEC{,_TEIL1,_TEIL2,_TEIL3}.md
// **Es gilt der ZWEITE NACHTRAG (04.09.2026)** am Ende jeder Spec-Datei.
// ============================================================================
#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VRReload.hpp"
#include "RE4VRWeapons.hpp"
#include "RE4VRMotion.hpp"

// windows.h (ueber die Includes oben) definiert min/max als MAKROS und zerlegt
// jedes std::min/std::max. Der Fork setzt kein NOMINMAX.
#undef min
#undef max

namespace {
constexpr const char* CONFIG_FILE = "re4_vr/re4_vr_motion.json";
constexpr const char* FL_CFG_PATH = "re4_vr/re4_vr_flashlight.json";
constexpr const char* WEAPON_NONE_KEY = "none";

// [FEUERWAHLSCHALTER] LE5 und Chicago Sweeper -- gleiches System, eigene
// Offsets und Burst-Werte je Waffe.
constexpr int32_t WID_LE5 = 4202;
constexpr int32_t WID_SWEEPER = 4201;

// Lua: `type(x) == "number"`. nlohmann unterscheidet Integer/Float, Lua nicht.
bool j_is_num(const nlohmann::json& d, const char* key) {
    return d.is_object() && d.contains(key) && d[key].is_number();
}

bool j_is_bool(const nlohmann::json& d, const char* key) {
    return d.is_object() && d.contains(key) && d[key].is_boolean();
}

bool j_is_table(const nlohmann::json& d, const char* key) {
    return d.is_object() && d.contains(key) && d[key].is_object();
}

bool j_is_str(const nlohmann::json& d, const char* key) {
    return d.is_object() && d.contains(key) && d[key].is_string();
}

// Lua: `if type(s[k]) == "number" then dst = s[k] end` -- fehlt der Schluessel
// oder hat er den falschen Typ, bleibt der bisherige Wert stehen.
void num_into(float& dst, const nlohmann::json& d, const char* key) {
    if (j_is_num(d, key)) {
        dst = d[key].get<float>();
    }
}

void num_into(double& dst, const nlohmann::json& d, const char* key) {
    if (j_is_num(d, key)) {
        dst = d[key].get<double>();
    }
}

// Geschuetzte Managed-Zugriffe -- Bauform wie in RE4VRArmChain/RE4VRMovement:
// 16-Byte-Puffer (die Engine behandelt via.vec3/via.quat als 16 Byte) und ein
// Erfolgs-Flag, weil re4vr::call_safe "hat 0 geliefert" und "ging nicht" nicht
// unterscheidet.
sdk::REMethodDefinition* find_method(::REManagedObject* obj, std::string_view name) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    return td != nullptr ? td->get_method(name) : nullptr;
}

// set_ParentJoint(name) braucht einen managed String -- 1:1 wie
// RE4VRWeapons2::set_parent_joint, das den linken Klon an L_Hand haengt.
bool set_parent_joint(::REManagedObject* ctf, const char* joint);

bool clear_pending(sdk::VMContext* context, bool ok) {
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        return false;
    }

    return ok;
}

bool set_parent_joint(::REManagedObject* ctf, const char* joint) {
    auto* str = sdk::VM::create_managed_string(utility::widen(joint));

    if (str == nullptr) {
        return false;
    }

    const auto m = find_method(ctf, "set_ParentJoint");

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();

    try {
        m->call_safe<void*>(context, ctf, str);
    } catch (...) {
        return false;
    }

    clear_pending(context, true);

    return true;
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

bool get_mat4(::REManagedObject* obj, std::string_view name, glm::mat4& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::mat4 buf{1.0f};
    bool ok = false;

    try {
        method->call_safe<glm::mat4*>(&buf, context, obj);
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

bool set_vec4(::REManagedObject* obj, std::string_view name, const glm::vec4& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf = v;
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

// euler(deg) -> Quaternion (W,X,Y,Z). Reihenfolge exakt wie im Original
// (Lua Z.212-221): yaw(Y) * pitch(X) * roll(Z), danach normalisiert.
glm::quat quat_from_euler_deg(float px, float py, float pz) {
    const auto ax = [](float a, float x, float y, float z) {
        const float h = glm::radians(a) * 0.5f;
        const float s = std::sin(h);
        return glm::quat{std::cos(h), x * s, y * s, z * s};
    };

    const glm::quat q = ax(py, 0.0f, 1.0f, 0.0f) * ax(px, 1.0f, 0.0f, 0.0f) * ax(pz, 0.0f, 0.0f, 1.0f);
    return glm::normalize(q);
}

// getJointByName(System.String)
::REManagedObject* joint_by_name(::REManagedObject* tf, const char* name) {
    if (tf == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    return str != nullptr ? re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str) : nullptr;
}

// [PORTFIX 2026-09-06] via.Transform.find(System.String) braucht -- wie
// getJointByName -- einen MANAGED String. call_safe reicht die Argumente
// ungewandelt durch; ein roher const char* wird als SystemString-Header
// gelesen. Folge war: die Taschenlampe "ac0000_00" wurde NIE gefunden
// (Leon und Ada), damit blieb __re4_fl_active false und der ganze
// Lampen-Zweig tot. In Lua wandelt das Binding den String automatisch.
::REManagedObject* find_by_name(::REManagedObject* tf, const wchar_t* name) {
    if (tf == nullptr || name == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(name);

    return str != nullptr ? re4vr::call_safe<::REManagedObject*>(tf, "find", str)
                          : nullptr;
}

void bool_into(bool& dst, const nlohmann::json& d, const char* key) {
    if (j_is_bool(d, key)) {
        dst = d[key].get<bool>();
    }
}
} // namespace

std::shared_ptr<RE4VRMotion>& RE4VRMotion::get() {
    static auto inst = std::make_shared<RE4VRMotion>();
    return inst;
}

// [ZEITBASIS -- PFLICHT] os.clock(), nicht steady_clock.
// __re4_parry_keep_gun_until und __re4_choke_knife_stuck werden von ANDEREN
// Lua-Dateien geschrieben und hier gelesen. Eine eigene Uhr braeche
// [PARRY_KEEP_GUN] und [CHOKE_KNIFE_STUCK] lautlos.
double RE4VRMotion::clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// ============================================================================
// Handles
// ============================================================================

void RE4VRMotion::store(Handle& h, ::REManagedObject* o) {
    if (h.obj == o) {
        return;
    }

    drop(h);

    if (o == nullptr) {
        return;
    }

    h.obj = o;
    h.reffed = false;

    if (utility::re_managed_object::is_managed_object(o)
        && static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        h.reffed = true;
    }
}

void RE4VRMotion::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

// ============================================================================
// Offset-Zugriff (Lua Z.224-452)
//
// [TEIL1/K2, berichtigt im zweiten Nachtrag] ZWEI Ausnahmen von der
// get_/ensure_-Konvention legen selbst an: get_weapon_offset (Z.237) und
// get_support_offset (Z.293). Alle uebrigen get_* liefern nil, wenn nichts
// angelegt ist -- und genau das ist tragend: ohne Aim-Offset benutzt die Waffe
// IMMER das Idle-Offset, es gibt dann keinen Aim-Blend.
// ============================================================================

RE4VRMotion::WeaponOffset& RE4VRMotion::get_weapon_offset(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    return m_weapon_offset[k];   // legt bei Bedarf mit lauter 0.0 an
}

RE4VRMotion::SupportOffset& RE4VRMotion::get_support_offset(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    return m_support_offset[k];  // legt bei Bedarf mit den Defaults an
}

RE4VRMotion::SupportOffsetAim* RE4VRMotion::get_support_offset_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_aim.find(k);
    return it == m_support_offset_aim.end() ? nullptr : &it->second;
}

RE4VRMotion::SupportOffsetAim& RE4VRMotion::ensure_support_offset_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_aim.find(k);

    if (it != m_support_offset_aim.end()) {
        return it->second;
    }

    // [TEIL1/L4] NEBENWIRKUNG, 1:1 uebernommen: das Seeden ruft
    // get_support_offset, und das LEGT AN. Eine JSON mit
    // support_offset_aim-Eintraegen ohne passenden support_offset
    // materialisiert damit beim Laden stille Idle-Eintraege.
    const SupportOffset& i = get_support_offset(k);
    SupportOffsetAim a{};
    a.pos_x = i.pos_x;
    a.pos_y = i.pos_y;
    a.pos_z = i.pos_z;
    a.rot_pitch = i.rot_pitch;
    a.rot_yaw = i.rot_yaw;
    a.rot_roll = i.rot_roll;
    a.blend_in = 0.18f;
    a.blend_out = 0.18f;
    return m_support_offset_aim.emplace(k, a).first->second;
}

RE4VRMotion::SupportOffsetSwitch* RE4VRMotion::get_support_offset_switch(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch.find(k);
    return it == m_support_offset_switch.end() ? nullptr : &it->second;
}

RE4VRMotion::SupportOffsetSwitch& RE4VRMotion::ensure_support_offset_switch(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    return m_support_offset_switch[k];   // Defaults stehen im Struct
}

RE4VRMotion::SupportOffsetAim* RE4VRMotion::get_support_offset_switch_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch_aim.find(k);
    return it == m_support_offset_switch_aim.end() ? nullptr : &it->second;
}

RE4VRMotion::SupportOffsetAim& RE4VRMotion::ensure_support_offset_switch_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch_aim.find(k);

    if (it != m_support_offset_switch_aim.end()) {
        return it->second;
    }

    // Lua: `local i = get_support_offset_switch(key) or {}` -- der get_*-Weg
    // legt hier NICHT an; fehlt der Switch-Eintrag, wird aus lauter Nullen
    // geseedet (Luas leere Tabelle mit `or 0.0` je Feld).
    SupportOffsetAim a{};
    const auto* i = get_support_offset_switch(k);

    if (i != nullptr) {
        a.pos_x = i->pos_x;
        a.pos_y = i->pos_y;
        a.pos_z = i->pos_z;
        a.rot_pitch = i->rot_pitch;
        a.rot_yaw = i->rot_yaw;
        a.rot_roll = i->rot_roll;
    }

    a.blend_in = 0.18f;
    a.blend_out = 0.18f;
    return m_support_offset_switch_aim.emplace(k, a).first->second;
}

RE4VRMotion::SupportOffsetSwitch2* RE4VRMotion::get_support_offset_switch2(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch2.find(k);
    return it == m_support_offset_switch2.end() ? nullptr : &it->second;
}

RE4VRMotion::SupportOffsetSwitch2& RE4VRMotion::ensure_support_offset_switch2(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch2.find(k);

    if (it != m_support_offset_switch2.end()) {
        return it->second;
    }

    SupportOffsetSwitch2 s{};
    const auto* i = get_support_offset_switch(k);

    if (i != nullptr) {
        s.pos_x = i->pos_x;
        s.pos_y = i->pos_y;
        s.pos_z = i->pos_z;
        s.rot_pitch = i->rot_pitch;
        s.rot_yaw = i->rot_yaw;
        s.rot_roll = i->rot_roll;
    }

    s.lerp = 0.15f;
    return m_support_offset_switch2.emplace(k, s).first->second;
}

RE4VRMotion::SupportOffsetSwitch2Aim* RE4VRMotion::get_support_offset_switch2_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch2_aim.find(k);
    return it == m_support_offset_switch2_aim.end() ? nullptr : &it->second;
}

RE4VRMotion::SupportOffsetSwitch2Aim& RE4VRMotion::ensure_support_offset_switch2_aim(const std::string& key) {
    const std::string k = key.empty() ? WEAPON_NONE_KEY : key;
    const auto it = m_support_offset_switch2_aim.find(k);

    if (it != m_support_offset_switch2_aim.end()) {
        return it->second;
    }

    SupportOffsetSwitch2Aim a{};
    const auto* i = get_support_offset_switch2(k);

    if (i != nullptr) {
        a.pos_x = i->pos_x;
        a.pos_y = i->pos_y;
        a.pos_z = i->pos_z;
        a.rot_pitch = i->rot_pitch;
        a.rot_yaw = i->rot_yaw;
        a.rot_roll = i->rot_roll;
    }

    return m_support_offset_switch2_aim.emplace(k, a).first->second;
}

// ============================================================================
// Feuerwahlschalter (Lua Z.362-397)
// ============================================================================

bool RE4VRMotion::is_switch_dock_weapon(int32_t wid) {
    return wid == WID_LE5 || wid == WID_SWEEPER;
}

// Nach JEDEM Umlegen loest die Hand sofort -- Sweeper ja, LE5 nein (dort ist
// mehrfaches Umlegen pro gehaltenem Grip moeglich).
bool RE4VRMotion::is_switch_oneshot(int32_t wid) {
    return wid == WID_SWEEPER;
}

const std::vector<int>& RE4VRMotion::fire_mode_cycle(int32_t wid) {
    // 0 = Full, 1 = Burst, 2 = Single.
    static const std::vector<int> le5{0, 1, 2};        // Full -> Burst -> Single -> Full
    static const std::vector<int> sweeper{0, 1};       // Burst = Single via burst_count 1
    static const std::vector<int> fallback{0, 1, 2};

    if (wid == WID_LE5) {
        return le5;
    }

    if (wid == WID_SWEEPER) {
        return sweeper;
    }

    return fallback;
}

bool RE4VRMotion::fire_mode_has_single(int32_t wid) {
    for (const auto m : fire_mode_cycle(wid)) {
        if (m == 2) {
            return true;
        }
    }

    return false;
}

int RE4VRMotion::fire_mode_next(int32_t wid, int cur) {
    const auto& cyc = fire_mode_cycle(wid);

    for (size_t i = 0; i < cyc.size(); ++i) {
        if (cyc[i] == cur) {
            return cyc[(i + 1) % cyc.size()];
        }
    }

    return cyc.empty() ? 0 : cyc[0];
}

// ============================================================================
// Config-Rohzugriff (Lua Z.454-473)
//
// BEWUSST io.open + json.load_string/dump_string, NICHT json.load_file:
// save_config liest die Datei erst neu ein (Read-Modify-Write), damit fremde
// Schluessel erhalten bleiben. re4vr::json_load/json_save gehen denselben Weg
// ueber reframework/data/.
// ============================================================================

nlohmann::json RE4VRMotion::read_config_raw() {
    const auto d = re4vr::json_load(CONFIG_FILE);
    return d.is_object() ? d : nlohmann::json::object();
}

bool RE4VRMotion::write_config_raw(const nlohmann::json& data) {
    return re4vr::json_save(CONFIG_FILE, data);
}

// ============================================================================
// load_config (Lua Z.475-702) -- 229 Zeilen reine Typpruefung.
// Feld fuer Feld uebernommen; eine generische Schleife traefe die Sonderfaelle
// nicht.
// ============================================================================

void RE4VRMotion::load_config() {
    const auto data = read_config_raw();

    // [ADA_KNIFE_RELFIX] Marker: wurde Adas Messer-Kalibrierung schon einmal um
    // 180 Grad gedreht? Ohne den wuerde die Migration bei jedem Script-Reset
    // erneut drehen -- und damit zurueckdrehen.
    if (j_is_bool(data, "knife_relfix_ada") && data["knife_relfix_ada"].get<bool>()) {
        re4vr::lua_set_bool("__re4_ada_relfix", true);
    }

    if (j_is_bool(data, "knife_relfix_ada2") && data["knife_relfix_ada2"].get<bool>()) {
        re4vr::lua_set_bool("__re4_ada_relfix2", true);
    }

    if (j_is_bool(data, "knife_relfix_ada3") && data["knife_relfix_ada3"].get<bool>()) {
        re4vr::lua_set_bool("__re4_ada_relfix3", true);
    }

    // Die folgenden Werte leben als Lua-Globals weiter (Publikationspflicht):
    // die UI-Slider und mehrere Fremd-Scripte lesen sie dort.
    if (j_is_num(data, "knife_swing_threshold")) {
        re4vr::lua_set_number("__re4_knife_swing_threshold", data["knife_swing_threshold"].get<double>());
    }

    if (j_is_num(data, "pose_fade_dur")) {
        re4vr::lua_set_number("__re4_pose_fade_dur", data["pose_fade_dur"].get<double>());
    }

    if (j_is_num(data, "knife_touch")) {
        re4vr::lua_set_number("__re4_knife_touch", data["knife_touch"].get<double>());
    }

    if (j_is_num(data, "knife_reach")) {
        re4vr::lua_set_number("__re4_knife_reach", data["knife_reach"].get<double>());
    }

    if (j_is_num(data, "knife_throw_threshold")) {
        re4vr::lua_set_number("__re4_knife_throw_threshold", data["knife_throw_threshold"].get<double>());
    }

    if (j_is_num(data, "knife_flip_speed")) {
        re4vr::lua_set_number("__re4_knife_flip_speed", data["knife_flip_speed"].get<double>());
    }

    // [SKULL_SPIN_ENTFERNT] skull_spread_from/to und skull_spin_keys werden
    // nicht mehr geladen -- Spin, Keyframes und Griffpose sind ausgebaut.

    if (j_is_num(data, "knife_flip_finger_deg")) {
        re4vr::lua_set_number("__re4_knife_flip_finger_deg", data["knife_flip_finger_deg"].get<double>());
    }

    if (j_is_num(data, "knife_flip_pos_x")) {
        re4vr::lua_set_number("__re4_knife_flip_pos_x", data["knife_flip_pos_x"].get<double>());
    }

    if (j_is_num(data, "knife_flip_pos_y")) {
        re4vr::lua_set_number("__re4_knife_flip_pos_y", data["knife_flip_pos_y"].get<double>());
    }

    if (j_is_num(data, "knife_flip_pos_z")) {
        re4vr::lua_set_number("__re4_knife_flip_pos_z", data["knife_flip_pos_z"].get<double>());
    }

    // [KNIFE_FLIP] PRO-MESSER Griff-Offset (Map wid -> {x,y,z}).
    // Lua baut hier eine Lua-Tabelle mit ZAHLEN-Schluesseln; die Fremdleser
    // (weapons2) indizieren sie mit der Waffen-ID.
    if (j_is_table(data, "knife_flip_pos")) {
        re4vr::lua_ensure_table("__re4_knife_flip_pos_map");

        for (const auto& [k, v] : data["knife_flip_pos"].items()) {
            if (!v.is_object()) {
                continue;
            }

            const auto id = std::atoi(k.c_str());

            if (id == 0 && k != "0") {
                continue;   // Lua: `tonumber(k)` liefert nil -> Eintrag uebersprungen
            }

            re4vr::lua_set_xyz_at("__re4_knife_flip_pos_map", id,
                                          j_is_num(v, "x") ? v["x"].get<float>() : 0.0f,
                                          j_is_num(v, "y") ? v["y"].get<float>() : 0.0f,
                                          j_is_num(v, "z") ? v["z"].get<float>() : 0.0f);
        }
    }

    if (j_is_table(data, "knife_flip_pos_ada")) {
        re4vr::lua_ensure_table("__re4_knife_flip_pos_map_ada");

        for (const auto& [k, v] : data["knife_flip_pos_ada"].items()) {
            if (!v.is_object()) {
                continue;
            }

            const auto id = std::atoi(k.c_str());

            if (id == 0 && k != "0") {
                continue;
            }

            re4vr::lua_set_xyz_at("__re4_knife_flip_pos_map_ada", id,
                                          j_is_num(v, "x") ? v["x"].get<float>() : 0.0f,
                                          j_is_num(v, "y") ? v["y"].get<float>() : 0.0f,
                                          j_is_num(v, "z") ? v["z"].get<float>() : 0.0f);
        }
    }

    // [KNIFE_THROW] Flug-Tuning. Die Tabelle lebt als Global weiter, weil
    // weapons/weapons2 sie lesen; sie hat ausserdem eine or-Initialisierung und
    // ueberlebt damit einen Script-Reset.
    if (j_is_table(data, "knife_fly")) {
        const auto& d = data["knife_fly"];
        re4vr::lua_ensure_table("__re4_knife_fly_cfg");

        // Defaults nur, wo die Tabelle frisch angelegt wurde (Lua-Fallback
        // `or { speed = 12.0, gravity = 7.0, spin = 18.0, max_time = 1.5,
        // return_delay = 0.4 }`).
        re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "speed", 12.0);
        re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "gravity", 7.0);
        re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "spin", 18.0);
        re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "max_time", 1.5);
        re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "return_delay", 0.4);

        static const char* FLY_NUM_KEYS[] = {
            "max_time", "return_delay", "hit_return_delay", "stick_in", "stick_snap",
            "speed", "hit_radius", "break_radius", "spin", "spin_fly", "spin_fall",
            "dir_max_deg", "hmd_yaw", "hmd_pitch", "assist_strength", "assist_homing",
            "assist_cone_deg", "assist_cone_inner_deg", "assist_min_lat", "close_hit_radius",
            "assist_target_off_y", "assist_flatten", "max_range",
            "spin_ax", "spin_ay", "spin_az", "spin_ax_l", "spin_ay_l", "spin_az_l",
            "v_min", "v_max",
        };

        for (const auto* k : FLY_NUM_KEYS) {
            if (j_is_num(d, k)) {
                re4vr::lua_set_table_number("__re4_knife_fly_cfg", k, d[k].get<double>());
            }
        }

        if (j_is_bool(d, "hmd_force")) {
            re4vr::lua_set_table_bool("__re4_knife_fly_cfg", "hmd_force", d["hmd_force"].get<bool>());
        }
    }

    // [LANDE_POSE] persistierte Lande-Rotation.
    if (j_is_table(data, "knife_land")) {
        const auto& dl = data["knife_land"];
        re4vr::lua_ensure_table("__re4_knife_land_rot");
        re4vr::lua_seed_table_number("__re4_knife_land_rot", "rx", 1.5708);
        re4vr::lua_seed_table_number("__re4_knife_land_rot", "ry", 0.0);
        re4vr::lua_seed_table_number("__re4_knife_land_rot", "rz", 0.0);

        for (const auto* k : {"rx", "ry", "rz"}) {
            if (j_is_num(dl, k)) {
                re4vr::lua_set_table_number("__re4_knife_land_rot", k, dl[k].get<double>());
            }
        }
    }

    if (j_is_str(data, "controller_type")) {
        const auto c = data["controller_type"].get<std::string>();

        if (c == "steamvr" || c == "metavr") {
            m_selected_controller = c;
        }
    }

    if (j_is_table(data, "openxr_correction")) {
        const auto& o = data["openxr_correction"];
        num_into(m_openxr_correction.pos_x, o, "pos_x");
        num_into(m_openxr_correction.pos_y, o, "pos_y");
        num_into(m_openxr_correction.pos_z, o, "pos_z");
        num_into(m_openxr_correction.rot_pitch, o, "rot_pitch");
        num_into(m_openxr_correction.rot_yaw, o, "rot_yaw");
        num_into(m_openxr_correction.rot_roll, o, "rot_roll");
    }

    if (j_is_table(data, "ctrl_correction")) {
        const auto& o = data["ctrl_correction"];
        num_into(m_ctrl_correction.pos_x, o, "pos_x");
        num_into(m_ctrl_correction.pos_y, o, "pos_y");
        num_into(m_ctrl_correction.pos_z, o, "pos_z");
        num_into(m_ctrl_correction.rot_pitch, o, "rot_pitch");
        num_into(m_ctrl_correction.rot_yaw, o, "rot_yaw");
        num_into(m_ctrl_correction.rot_roll, o, "rot_roll");
    }

    // Linke Hand (global)
    if (j_is_table(data, "left_hand_offset")) {
        const auto& o = data["left_hand_offset"];
        num_into(m_hand_offset_l.px, o, "px");
        num_into(m_hand_offset_l.py, o, "py");
        num_into(m_hand_offset_l.pz, o, "pz");
        num_into(m_hand_offset_l.rx, o, "rx");
        num_into(m_hand_offset_l.ry, o, "ry");
        num_into(m_hand_offset_l.rz, o, "rz");
    }

    // [SUPPORT_HAND] Offset pro Waffe (ungetunte Waffen bleiben auf 0)
    if (j_is_table(data, "support_offset")) {
        for (const auto& [key, s] : data["support_offset"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = get_support_offset(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
            num_into(off.dock_threshold, s, "dock_threshold");
            num_into(off.undock_threshold, s, "undock_threshold");
            num_into(off.grip_x, s, "grip_x");
            num_into(off.grip_y, s, "grip_y");
            num_into(off.grip_z, s, "grip_z");
            num_into(off.grip_back, s, "grip_back");
            num_into(off.grip_fwd, s, "grip_fwd");
            bool_into(off.grip_on, s, "grip_on");
            bool_into(off.grip_anchor, s, "grip_anchor");
            bool_into(off.grip_noroll, s, "grip_noroll");
        }
    }

    // [SUPPORT_AIM] optionale Aim-Offsets -- nur falls vorhanden
    if (j_is_table(data, "support_offset_aim")) {
        for (const auto& [key, s] : data["support_offset_aim"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = ensure_support_offset_aim(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
            num_into(off.blend_in, s, "blend_in");
            num_into(off.blend_out, s, "blend_out");
        }
    }

    // [SWITCH_DOCK] Schalter-Dock-Offsets
    if (j_is_table(data, "support_offset_switch")) {
        for (const auto& [key, s] : data["support_offset_switch"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = ensure_support_offset_switch(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
            num_into(off.dock_dist, s, "dock_dist");
            num_into(off.blend_speed, s, "blend_speed");
            num_into(off.idx_rx, s, "idx_rx");
            num_into(off.idx_ry, s, "idx_ry");
            num_into(off.idx_rz, s, "idx_rz");
            num_into(off.burst_rot, s, "burst_rot");
            num_into(off.burst_count, s, "burst_count");
            num_into(off.single_rot, s, "single_rot");
            num_into(off.lever_lerp, s, "lever_lerp");
        }
    }

    if (j_is_table(data, "support_offset_switch_aim")) {
        for (const auto& [key, s] : data["support_offset_switch_aim"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = ensure_support_offset_switch_aim(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
            num_into(off.blend_in, s, "blend_in");
            num_into(off.blend_out, s, "blend_out");
        }
    }

    // [SWITCH_DOCK2] zweites Offset-Paar (Single/Burst-Stellung)
    if (j_is_table(data, "support_offset_switch2")) {
        for (const auto& [key, s] : data["support_offset_switch2"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = ensure_support_offset_switch2(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
            num_into(off.lerp, s, "lerp");
        }
    }

    if (j_is_table(data, "support_offset_switch2_aim")) {
        for (const auto& [key, s] : data["support_offset_switch2_aim"].items()) {
            if (!s.is_object()) {
                continue;
            }

            auto& off = ensure_support_offset_switch2_aim(key);
            num_into(off.pos_x, s, "pos_x");
            num_into(off.pos_y, s, "pos_y");
            num_into(off.pos_z, s, "pos_z");
            num_into(off.rot_pitch, s, "rot_pitch");
            num_into(off.rot_yaw, s, "rot_yaw");
            num_into(off.rot_roll, s, "rot_roll");
        }
    }

    if (j_is_table(data, "support_cfg")) {
        const auto& sc = data["support_cfg"];

        // Lua: `if data.support_cfg.enabled ~= nil then support.enabled =
        // (... == true) end` -- jeder Nicht-true-Wert schaltet AUS, nicht nur
        // false. Deshalb bewusst nicht bool_into.
        if (sc.is_object() && sc.contains("enabled") && !sc["enabled"].is_null()) {
            m_support.enabled = sc["enabled"].is_boolean() && sc["enabled"].get<bool>();
        }

        num_into(m_support.blend_speed, sc, "blend_speed");
        num_into(m_support.grip_latch_reach, sc, "grip_latch_reach");
    }

    num_into(m_rot_smooth_hands, data, "hand_smooth_rot");
    num_into(m_pos_smooth_hands, data, "hand_smooth_pos");

    // [KS4_EXIT_FADE] Die Tabelle ist ein Global mit or-Initialisierung und
    // ueberlebt den Script-Reset.
    if (j_is_num(data, "ks4_exit_fade")) {
        re4vr::lua_ensure_table("__re4_ks4fade");
        re4vr::lua_set_table_number("__re4_ks4fade", "dur", data["ks4_exit_fade"].get<double>());
    }

    // Rechte Hand pro Waffe. Null-Default-Eintraege werden NICHT geladen, damit
    // die JSON nicht mit Nullen vollaeuft.
    if (j_is_table(data, "weapon_offset")) {
        for (const auto& [key, s] : data["weapon_offset"].items()) {
            if (!s.is_object()) {
                continue;
            }

            WeaponOffset tmp{};
            num_into(tmp.px, s, "px");
            num_into(tmp.py, s, "py");
            num_into(tmp.pz, s, "pz");
            num_into(tmp.rx, s, "rx");
            num_into(tmp.ry, s, "ry");
            num_into(tmp.rz, s, "rz");

            const bool is_default = tmp.px == 0 && tmp.py == 0 && tmp.pz == 0
                                    && tmp.rx == 0 && tmp.ry == 0 && tmp.rz == 0;

            if (!is_default) {
                m_weapon_offset[key] = tmp;
            }
        }
    }

    // [MATILDA_STOCK] alter Hand-Key aus einem fruehen Ansatz -> aufraeumen
    // (heute "4004_stockwep", waffe-only).
    m_weapon_offset.erase("4004_stock");

    // [WEP_REL_PERSIST] eingefrorene Kalibrierungen laden
    if (j_is_table(data, "weapon_rel")) {
        for (const auto& [key, s] : data["weapon_rel"].items()) {
            if (!s.is_object() || !j_is_num(s, "px") || !j_is_num(s, "qw")) {
                continue;
            }

            WeaponRel r{};
            num_into(r.px, s, "px");
            num_into(r.py, s, "py");
            num_into(r.pz, s, "pz");
            num_into(r.qx, s, "qx");
            num_into(r.qy, s, "qy");
            num_into(r.qz, s, "qz");
            num_into(r.qw, s, "qw");
            m_weapon_rel[key] = r;
        }
    }

    // [TWO_HAND_IK] Tunings.
    // [ZWEITER NACHTRAG V2] pitch/yaw/roll WERDEN geladen -- der erste Nachtrag
    // (L8) behauptete das Gegenteil und haette getunte Winkel verworfen.
    if (j_is_table(data, "two_hand_cfg")) {
        const auto& th = data["two_hand_cfg"];

        if (th.is_object() && th.contains("enabled") && !th["enabled"].is_null()) {
            m_two_hand.enabled = th["enabled"].is_boolean() && th["enabled"].get<bool>();
        }

        num_into(m_two_hand.min_dist, th, "min_dist");
        num_into(m_two_hand.max_dist, th, "max_dist");
        num_into(m_two_hand.blend_speed, th, "blend_speed");
        num_into(m_two_hand.pitch, th, "pitch");
        num_into(m_two_hand.yaw, th, "yaw");
        num_into(m_two_hand.roll, th, "roll");

        // [MIGRATION] alter blend_speed-Default (0.10, nie per UI gesetzt)
        if (m_two_hand.blend_speed == 0.10f) {
            m_two_hand.blend_speed = 0.05f;
        }
    }

    m_cfg_loaded = true;
}

// ============================================================================
// save_config (Lua Z.704-893) -- Read-Modify-Write.
//
// Liest die Datei ERST NEU ein, damit fremde Schluessel erhalten bleiben, und
// schreibt dann die eigenen darueber.
//
// [ZWEITER NACHTRAG V1 -- WICHTIG] Die `rawget(_G,"x") or <default>`-Zeilen
// (Lua 707-712) greifen NUR bei nil. Ein per Slider auf 0 gestellter Wert
// bleibt 0 -- in Lua ist 0 WAHR. Der erste Audit-Nachtrag behauptete das
// Gegenteil; wer ihm folgt, baut hier `x == 0 ? default : x` und damit einen
// Fehler ein, den das getestete Lua nie hatte.
// ============================================================================

void RE4VRMotion::save_config() {
    // [DATENVERLUST-RIEGEL] Vor dem ersten Laden nie schreiben.
    if (!m_cfg_loaded) {
        return;
    }

    auto data = read_config_raw();

    data["controller_type"] = m_selected_controller;

    // NUR bei nil den Default -- s. V1 oben.
    data["knife_swing_threshold"] = re4vr::lua_get_number("__re4_knife_swing_threshold", 3.5);
    data["pose_fade_dur"] = re4vr::lua_get_number("__re4_pose_fade_dur", 0.10);
    data["knife_touch"] = re4vr::lua_get_number("__re4_knife_touch", 0.90);
    data["knife_reach"] = re4vr::lua_get_number("__re4_knife_reach", 0.90);
    data["knife_throw_threshold"] = re4vr::lua_get_number("__re4_knife_throw_threshold", 4.0);
    data["knife_flip_speed"] = re4vr::lua_get_number("__re4_knife_flip_speed", 0.18);

    // [SKULL_SPIN_ENTFERNT] skull_spread_from/to + skull_spin_keys werden nicht
    // mehr geschrieben; alte Eintraege fallen beim naechsten Speichern raus.

    data["knife_flip_finger_deg"] = re4vr::lua_get_number("__re4_knife_flip_finger_deg", -35.0);
    data["knife_flip_pos_x"] = re4vr::lua_get_number("__re4_knife_flip_pos_x", 0.0);
    data["knife_flip_pos_y"] = re4vr::lua_get_number("__re4_knife_flip_pos_y", 0.0);
    data["knife_flip_pos_z"] = re4vr::lua_get_number("__re4_knife_flip_pos_z", 0.0);

    // [KNIFE_FLIP] Pro-Messer-Griff-Offsets: JSON-Key ist tostring(wid).
    // Nur schreiben, wenn die Global-Tabelle existiert (Lua: `if type(m) ==
    // "table"`), sonst bleibt der bisherige JSON-Inhalt unangetastet.
    {
        nlohmann::json out;

        if (re4vr::lua_get_xyz_map("__re4_knife_flip_pos_map", out)) {
            data["knife_flip_pos"] = out;
        }
    }

    {
        nlohmann::json out;

        if (re4vr::lua_get_xyz_map("__re4_knife_flip_pos_map_ada", out)) {
            data["knife_flip_pos_ada"] = out;
        }
    }

    data["knife_relfix_ada"] = re4vr::lua_get_tribool("__re4_ada_relfix") == 1;
    data["knife_relfix_ada2"] = re4vr::lua_get_tribool("__re4_ada_relfix2") == 1;
    data["knife_relfix_ada3"] = re4vr::lua_get_tribool("__re4_ada_relfix3") == 1;

    // [KNIFE_THROW] Flug-Tuning persistieren. Lua schreibt die Felder EINZELN
    // aus der Global-Tabelle; fehlende bleiben nil und verschwinden damit aus
    // der JSON -- genau so nachgebaut.
    {
        nlohmann::json fly;

        if (re4vr::lua_get_number_map("__re4_knife_fly_cfg",
                                      {"max_time", "return_delay", "hit_return_delay", "speed",
                                       "stick_in", "stick_snap", "hit_radius", "break_radius",
                                       "spin", "spin_fly", "spin_fall", "dir_max_deg",
                                       "spin_ax", "spin_ay", "spin_az",
                                       "spin_ax_l", "spin_ay_l", "spin_az_l",
                                       "hmd_yaw", "hmd_pitch", "assist_strength", "assist_homing",
                                       "assist_cone_deg", "assist_cone_inner_deg",
                                       "assist_target_off_y", "assist_flatten", "max_range",
                                       "assist_min_lat", "close_hit_radius", "v_min", "v_max"},
                                      fly)) {
            const auto hf = re4vr::lua_get_table_tribool("__re4_knife_fly_cfg", "hmd_force");

            if (hf >= 0) {
                fly["hmd_force"] = hf == 1;
            }

            data["knife_fly"] = fly;
        }

        // [LANDE_POSE] persistieren
        nlohmann::json land;

        if (re4vr::lua_get_number_map("__re4_knife_land_rot", {"rx", "ry", "rz"}, land)) {
            data["knife_land"] = land;
        }
    }

    data["openxr_correction"] = {
        {"pos_x", m_openxr_correction.pos_x},
        {"pos_y", m_openxr_correction.pos_y},
        {"pos_z", m_openxr_correction.pos_z},
        {"rot_pitch", m_openxr_correction.rot_pitch},
        {"rot_yaw", m_openxr_correction.rot_yaw},
        {"rot_roll", m_openxr_correction.rot_roll},
    };

    data["ctrl_correction"] = {
        {"pos_x", m_ctrl_correction.pos_x},
        {"pos_y", m_ctrl_correction.pos_y},
        {"pos_z", m_ctrl_correction.pos_z},
        {"rot_pitch", m_ctrl_correction.rot_pitch},
        {"rot_yaw", m_ctrl_correction.rot_yaw},
        {"rot_roll", m_ctrl_correction.rot_roll},
    };

    data["left_hand_offset"] = {
        {"px", m_hand_offset_l.px}, {"py", m_hand_offset_l.py}, {"pz", m_hand_offset_l.pz},
        {"rx", m_hand_offset_l.rx}, {"ry", m_hand_offset_l.ry}, {"rz", m_hand_offset_l.rz},
    };

    // Null-Default-Eintraege NICHT schreiben (Cleanup). Der Grip gehoert
    // ausdruecklich in die Pruefung, sonst faellt ein getunter Griff heraus.
    {
        nlohmann::json so = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset) {
            const bool is_default =
                off.pos_x == 0 && off.pos_y == 0 && off.pos_z == 0
                && off.rot_pitch == 0 && off.rot_yaw == 0 && off.rot_roll == 0
                && off.dock_threshold == 0.11f && off.undock_threshold == 0.15f
                && off.grip_x == 0 && off.grip_y == 0 && off.grip_z == 0
                && off.grip_back == 0 && off.grip_fwd == 0
                && off.grip_anchor != true && off.grip_noroll != true
                && off.grip_on != false;

            if (is_default) {
                continue;
            }

            so[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
                {"dock_threshold", off.dock_threshold}, {"undock_threshold", off.undock_threshold},
                {"grip_on", off.grip_on},
                {"grip_x", off.grip_x}, {"grip_y", off.grip_y}, {"grip_z", off.grip_z},
                {"grip_back", off.grip_back}, {"grip_fwd", off.grip_fwd},
                {"grip_anchor", off.grip_anchor}, {"grip_noroll", off.grip_noroll},
            };
        }

        data["support_offset"] = so;
    }

    // Die optionalen Tabellen werden OHNE Default-Filter geschrieben -- wer
    // angelegt ist, steht drin.
    {
        nlohmann::json soa = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset_aim) {
            soa[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
                {"blend_in", off.blend_in}, {"blend_out", off.blend_out},
            };
        }

        data["support_offset_aim"] = soa;
    }

    {
        nlohmann::json sos = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset_switch) {
            sos[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
                {"dock_dist", off.dock_dist}, {"blend_speed", off.blend_speed},
                {"idx_rx", off.idx_rx}, {"idx_ry", off.idx_ry}, {"idx_rz", off.idx_rz},
                {"burst_rot", off.burst_rot},
                {"burst_count", off.burst_count},
                {"single_rot", off.single_rot},
                {"lever_lerp", off.lever_lerp},
            };
        }

        data["support_offset_switch"] = sos;
    }

    {
        nlohmann::json ssa = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset_switch_aim) {
            ssa[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
                {"blend_in", off.blend_in}, {"blend_out", off.blend_out},
            };
        }

        data["support_offset_switch_aim"] = ssa;
    }

    {
        nlohmann::json ss2 = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset_switch2) {
            ss2[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
                {"lerp", off.lerp},
            };
        }

        data["support_offset_switch2"] = ss2;
    }

    {
        nlohmann::json ss2a = nlohmann::json::object();

        for (const auto& [key, off] : m_support_offset_switch2_aim) {
            ss2a[key] = {
                {"pos_x", off.pos_x}, {"pos_y", off.pos_y}, {"pos_z", off.pos_z},
                {"rot_pitch", off.rot_pitch}, {"rot_yaw", off.rot_yaw}, {"rot_roll", off.rot_roll},
            };
        }

        data["support_offset_switch2_aim"] = ss2a;
    }

    data["support_cfg"] = {
        {"enabled", m_support.enabled},
        {"blend_speed", m_support.blend_speed},
        {"grip_latch_reach", m_support.grip_latch_reach},
    };

    data["hand_smooth_rot"] = m_rot_smooth_hands;
    data["hand_smooth_pos"] = m_pos_smooth_hands;
    data["ks4_exit_fade"] = re4vr::lua_get_table_number("__re4_ks4fade", "dur", 0.35);

    {
        nlohmann::json wo = nlohmann::json::object();

        for (const auto& [key, off] : m_weapon_offset) {
            const bool is_default = off.px == 0 && off.py == 0 && off.pz == 0
                                    && off.rx == 0 && off.ry == 0 && off.rz == 0;

            if (is_default) {
                continue;   // Null-Default-Eintraege NICHT schreiben
            }

            wo[key] = {
                {"px", off.px}, {"py", off.py}, {"pz", off.pz},
                {"rx", off.rx}, {"ry", off.ry}, {"rz", off.rz},
            };
        }

        data["weapon_offset"] = wo;
    }


    {
        nlohmann::json wr = nlohmann::json::object();

        for (const auto& [key, r] : m_weapon_rel) {
            wr[key] = {
                {"px", r.px}, {"py", r.py}, {"pz", r.pz},
                {"qx", r.qx}, {"qy", r.qy}, {"qz", r.qz}, {"qw", r.qw},
            };
        }

        data["weapon_rel"] = wr;
    }

    data["two_hand_cfg"] = {
        {"enabled", m_two_hand.enabled},
        {"min_dist", m_two_hand.min_dist}, {"max_dist", m_two_hand.max_dist},
        {"blend_speed", m_two_hand.blend_speed},
        {"pitch", m_two_hand.pitch}, {"yaw", m_two_hand.yaw}, {"roll", m_two_hand.roll},
    };

    // Alt-Feld aus dem frueheren Schema entfernen, falls noch vorhanden.
    data.erase("hand_offset");

    write_config_raw(data);
}

// ============================================================================
// Die drei ADA_KNIFE_RELFIX-Migrationen (Lua Z.897-960)
//
// BEFUND: weapon_rel["6108@ada"] -- die EINGEFRORENE Hand->Waffe-Kalibrierung --
// wurde in verdrehter Lage eingefangen (die dokumentierte Falle "live gemessene
// Nulllagen fangen Engine-State ein"). Folge: der Ruhezustand zeigte das Messer
// verkehrt herum, erst der 180-Grad-Flip drehte es richtig. Daraus entstanden
// DREI Symptome aus EINER Wurzel -- die Flip-Slider wirkten auf den "normalen"
// Zustand, normale und Flip-Offsets koppelten sich, und DER WURF WAR TOT
// (weapons.lua sperrt das Wurf-Windup im Flip, also nie eine Loslass-Flanke).
//
// FIX: rel_rot einmalig mit dem Flip verrechnen. Der Flip wird POST-multipliziert
// (wrot = (hand_rot * rel_rot) * flip), also rel_rot_neu = rel_rot_alt * flip.
// Fuer flip = 180 Grad um X = Quaternion(w=0, x=1, y=0, z=0) reduziert sich das
// exakt auf: (w, x, y, z) -> (-x, w, z, -y). RECHNERISCH, nicht neu eingemessen
// -- neu messen wuerde denselben Engine-State wieder einfangen.
//
// Jede Stufe hat einen EIGENEN Marker und dreht nur ihre eigenen Keys genau
// EINMAL; zweimal 180 Grad waere wieder verdreht. Deshalb pro Nachzuegler ein
// neuer Marker statt die Liste zu erweitern.
// LEON-NEUTRAL: ausschliesslich "@ada"-Schluessel.
// ============================================================================

void RE4VRMotion::ada_knife_relfix() {
    const auto turn = [this](const char* key) {
        const auto it = m_weapon_rel.find(key);

        if (it == m_weapon_rel.end()) {
            return;
        }

        auto& r = it->second;
        const float w = r.qw;
        const float x = r.qx;
        const float y = r.qy;
        const float z = r.qz;
        r.qw = -x;
        r.qx = w;
        r.qy = z;
        r.qz = -y;
    };

    // Marker-Kette: relfix = 6108, relfix2 = 5003, relfix3 = 5002 (Kitchen Knife).
    if (re4vr::lua_get_tribool("__re4_ada_relfix") != 1) {
        turn("6108@ada");
        re4vr::lua_set_bool("__re4_ada_relfix", true);
        save_config();   // Marker + gedrehten Wert sofort festschreiben
    }

    if (re4vr::lua_get_tribool("__re4_ada_relfix2") != 1) {
        turn("5003@ada");
        re4vr::lua_set_bool("__re4_ada_relfix2", true);
        save_config();
    }

    if (re4vr::lua_get_tribool("__re4_ada_relfix3") != 1) {
        turn("5002@ada");
        re4vr::lua_set_bool("__re4_ada_relfix3", true);
        save_config();
    }
}

// ============================================================================
// Player-Body-Discovery (Lua Z.966-1033)
//
// [TEIL1/L8] Der Utility-Pfad validiert NICHT, waehrend Cache-Hit und
// Szenen-Pfad es tun -- und genau dieser unvalidierte Pfad ist der einzige, der
// ADA traegt. 1:1 uebernommen, inklusive dieser Asymmetrie.
//
// Das Original liest `re4.body or re4.player` aus der Lua-Modultabelle
// utility/RE4. Die ist von C++ nicht erreichbar (global liegt dort nur
// _RE4Lib); die Kette dahinter ist aber exakt
// CharacterManager -> getPlayerContextRef -> get_BodyGameObject, also
// re4vr::body_game_object(). Der `or re4.player`-Zweig faellt praktisch weg:
// RE4.player ist der CONTEXT, und get_Transform darauf liefert ohnehin nichts.
// ============================================================================

::REManagedObject* RE4VRMotion::find_player_body() {
    if (m_body_cache.go.obj != nullptr) {
        bool valid = false;

        try {
            valid = re4vr::call_safe<bool>(m_body_cache.go.obj, "get_Valid");
        } catch (...) {
            valid = false;
        }

        if (valid && m_body_cache.transform.obj != nullptr) {
            return m_body_cache.transform.obj;
        }
    }

    drop(m_body_cache.go);
    drop(m_body_cache.transform);

    // Utility-Pfad -- ohne Gueltigkeitspruefung, s.o.
    if (auto* body = re4vr::body_game_object(); body != nullptr) {
        if (auto* tf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform"); tf != nullptr) {
            store(m_body_cache.go, body);
            store(m_body_cache.transform, tf);
            return tf;
        }
    }

    // Szenen-Pfad -- MIT Gueltigkeitspruefung.
    if (auto* scene = sdk::get_current_scene(); scene != nullptr) {
        auto* str = sdk::VM::create_managed_string(utility::widen(std::string{"ch0a0z0_body"}));
        auto* go = str != nullptr
                       ? re4vr::call_safe<::REManagedObject*>(scene, "findGameObject(System.String)", str)
                       : nullptr;

        if (go != nullptr) {
            bool valid = false;

            try {
                valid = re4vr::call_safe<bool>(go, "get_Valid");
            } catch (...) {
                valid = false;
            }

            if (valid) {
                if (auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform"); tf != nullptr) {
                    store(m_body_cache.go, go);
                    store(m_body_cache.transform, tf);
                    return tf;
                }
            }
        }
    }

    return nullptr;
}

// ============================================================================
// Joint-Discovery + is_joint_valid (Lua Z.1107-1147)
//
// [LOGFLUT] Die Gueltigkeit wird geprueft, indem absichtlich get_Position
// gerufen wird. REFramework schreibt aber JEDE Engine-Exception SYNCHRON ins
// Framework-Log -- in der Invoke-Schicht, also BEVOR unser Schutz sie abfaengt.
// Bei einem dauerhaft toten Joint sind das Hunderte Zeilen pro Sekunde
// (gemessen: 665 in einer Sekunde, Log auf 10 MB), und waehrend die Platte
// beschrieben wird, kommt der Script-Thread nicht hinterher -> Eingaben laufen
// ins Leere. Deshalb die Drosselung: ein eben noch ungueltiger Joint wird 0,5 s
// lang nicht erneut angefasst.
//
// [TEIL1/L7] m_jv_bad wird NIE geleert, und der Schluessel ist die ROHADRESSE.
// Nativ ist das ein langsames Leck plus die Gefahr recycelter Adressen nach
// Save/Load (falsches "bad" fuer bis zu 0,5 s). 1:1 uebernommen.
// ============================================================================

bool RE4VRMotion::is_joint_valid(::REManagedObject* joint) {
    if (joint == nullptr) {
        return false;
    }

    const auto addr = reinterpret_cast<uintptr_t>(joint);
    const double now = clock_now();
    const auto it = m_jv_bad.find(addr);

    if (it != m_jv_bad.end() && (now - it->second) < 0.5) {
        return false;
    }

    // Der Aufruf SELBST ist der Test -- get_Position ist ein ValueType-Getter
    // und braucht den sret-Puffer; als Objektzeiger gerufen schriebe die Engine
    // 32 Byte in den VMContext.
    glm::vec4 probe{};
    const bool ok = get_vec4(joint, "get_Position", probe);

    if (ok) {
        m_jv_bad.erase(addr);
    } else {
        m_jv_bad[addr] = now;
    }

    return ok;
}

// [KILLSWITCH_RESTORE] Lua Z.1149-1169.
//
// Beim Killswitch hoeren motion und arm_chain auf zu schreiben. ABER:
// write_joint_pose setzt das Hand-Joint per set_Position (Welt), und die Engine
// speichert das als LOCAL-Position. Die native Anim (z.B. ch0_357_JUMPDOWN)
// animiert nur Knochen-ROTATIONEN, NIE die Translation -> unsere verbogene
// Hand-Local-Position bleibt kleben und der Unterarm streckt sich gummiartig
// zur eingefrorenen Hand (gemessen: konstant ~0.4 m statt ~0.23 m Knochenlaenge,
// eingefroren ueber die ganze Nicht-Gameplay-Episode).
//
// Fix: solange der Killswitch aktiv ist, die Hand-Local-Position hart auf die
// Bind-Pose zuruecksetzen. Gefahrlos, weil die Anim die Translation nie
// schreibt -> macht NUR unsere eigene Verformung rueckgaengig; die ROTATION
// laesst die native Anim frei treiben.
void RE4VRMotion::find_joints() {
    auto* pt = find_player_body();

    if (pt == nullptr) {
        drop(m_right_hand.joint);
        drop(m_left_hand.joint);
        return;
    }

    if (!is_joint_valid(m_right_hand.joint.obj)) {
        store(m_right_hand.joint, joint_by_name(pt, "R_Hand"));
    }

    if (!is_joint_valid(m_left_hand.joint.obj)) {
        store(m_left_hand.joint, joint_by_name(pt, "L_Hand"));
    }
}

void RE4VRMotion::restore_hands_native() {
    find_joints();

    const auto restore = [](::REManagedObject* h) {
        if (h == nullptr) {
            return;
        }

        glm::vec4 bp{};

        // get_BaseLocalPosition ist ein ValueType-Getter -- ueber den
        // sret-Puffer holen, nicht als Objektzeiger.
        if (get_vec4(h, "get_BaseLocalPosition", bp)) {
            set_vec4(h, "set_LocalPosition", bp);
        }
    };

    restore(m_right_hand.joint.obj);
    restore(m_left_hand.joint.obj);
}

// ============================================================================
// Charakter-Erkennung (Lua Z.1216-1240) -- __re4_char_now
//
// [ZWEITER NACHTRAG V8] Eine von nur ZWEI Funktions-Globals, die echte
// Fremd-Schnittstelle sind: gelesen von binding, reload_adv, weapons2 und von
// RE4VRCrosshair.cpp (call_global_string). weapons2 haelt eine or-geschuetzte,
// byte-gleiche Zweitdefinition -- im Lua gewinnt, wer zuerst laedt.
// Der Port exportiert sie in on_lua_state_created, das VOR dem Datei-Ladeloop
// laeuft; weapons2' `or`-Guard laesst unsere Fassung damit stehen. Genau das
// Lua-Verhalten "erste Definition gewinnt".
//
// [CHAR_STICKY -- ZURUECKGENOMMEN] Der Rueckgabewert ist bei unbekanntem Body
// bewusst LEER, nicht der letzte bekannte Charakter. __re4_char_last wird
// trotzdem fortgeschrieben.
// ============================================================================

std::string RE4VRMotion::char_now() {
    std::string n{};

    auto* ctx = re4vr::player_context();

    if (ctx != nullptr) {
        if (auto* b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject"); b != nullptr) {
            n = re4vr::obj_name(b);
        }
    }

    std::string c{};

    // [ADA_MERCS_BODY] In Mercenaries heisst Adas Body "ch3a8z0_MC_body"
    // (SW-KindID 380000 + _MC-Suffix) -- das MODELL ist dasselbe wie in
    // Separate Ways. Ohne diese Zeile lieferte die Erkennung dort nichts:
    // knife_wep_key fiel in LEONS Namensraum zurueck und die Slider "machten
    // nichts". Bewusst derselbe Wert "ada" -- das Tuning gilt in beiden Modi.
    if (n == "ch3a8z0_body" || n == "ch3a8z0_MC_body") {
        c = "ada";
    // [LEON_MERCS_BODY 20.09.2026] In Mercenaries heisst Leons Body
    // "ch6i0z0_body" (KindID 600000, auch Leon2) -- ohne diese Zeile lieferte
    // char_now() dort LEER, lh_char_tick stieg aus ("unbekannt -> NICHT
    // umschalten") und m_lh_off blieb leer: clone_apply_pose setzte das linke
    // Messer auf LocalPosition(0,0,0)+Identity, also nackt aufs Handgelenk.
    // Sichtbar nur beim DIREKTEN Start in Mercs -- aus der Kampagne kommend
    // stand die geladene Map noch im Speicher.
    } else if (n == "ch0a0z0_body" || n == "ch0a1z0_body" || n == "ch6i0z0_body") {
        c = "leon";
    }

    if (!c.empty()) {
        re4vr::lua_set_string("__re4_char_last", c);
        return c;
    }

    return {};
}

// ============================================================================
// [MATILDA_STOCK -- DATENQUELLE 08.09.2026]
// Bis hierher las die Erkennung `getPartsEnable(11)` an einem via.render.Mesh,
// das GENAU EINMAL pro Waffen-GO geholt wurde. Der Renderzustand haengt aber am
// Mesh-Objekt, das die Engine bei jedem Neuaufbau wegwirft: ging der eine
// getComponent-Versuch daneben, blieb das Flag bis zum naechsten Waffen-GO auf
// false -- die Matilda lief dann ohne den "4004_stockwep"-Versatz und sass
// schief in der Hand, bis ein Waffenwechsel ein neues GO brachte.
// GEMESSEN 08.09. (reframework/data/re4_stock_diag.txt): Hand->Waffe 0.087
// (nur weapon_rel) im schiefen Zustand gegen 0.090 (mit Zuschlag) im geraden,
// waehrend `isExistsParts` durchgehend true meldete -- auch ueber GO-Wechsel.
//
// Deshalb jetzt die DATENFRAGE an die Engine: chainsaw.Arms::isExistsParts mit
// der Stock-ItemID (dieselbe, die die Twirl-Sperre in RE4VRWeapons2 benutzt --
// live gemessen am 12.08.). Kein Baum-Scan, kein Mesh, kein Objekt-Cache.
//
// Zwei Haerten, damit das Flag nicht mehr flackern kann:
//   * Ein FEHLGESCHLAGENER Aufruf laesst den letzten bekannten Wert stehen
//     (nur ein ehrliches false schaltet ab) -- `try_call` unterscheidet beides.
//   * Ausgewertet wird nur, solange die Engine wirklich die Matilda als
//     equippt meldet; waehrend eines Parry-Fensters equippt sie kurz das
//     Messer, und dessen "kein Stock" darf den Zustand nicht kippen.
// ============================================================================

bool RE4VRMotion::stock_mounted_data() {
    // Stock wird im Menue an-/abgebaut -- viermal pro Sekunde fragen reicht.
    const double now = clock_now();

    if (m_wep_cache.stock_t >= 0.0 && (now - m_wep_cache.stock_t) < 0.25) {
        return m_wep_cache.stock_on;
    }

    m_wep_cache.stock_t = now;

    const auto wid = get_equip_weapon_id();

    if (!wid.has_value() || *wid != 4004) {
        return m_wep_cache.stock_on;
    }

    auto* ctx = re4vr::player_context();

    if (ctx == nullptr) {
        return m_wep_cache.stock_on;
    }

    auto* h_updater = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (h_updater == nullptr) {
        return m_wep_cache.stock_on;
    }

    auto* gun = re4vr::call_safe<::REManagedObject*>(h_updater, "get_EquipWeapon");

    if (gun == nullptr) {
        return m_wep_cache.stock_on;
    }

    // chainsaw.ItemID des Matilda-Stocks (RE4VRWeapons2 is_stock_item).
    constexpr int32_t STOCK_ITEM_MATILDA = 116009600;
    bool exists = false;

    if (re4vr::try_call<bool>(gun, "isExistsParts", exists, STOCK_ITEM_MATILDA)) {
        m_wep_cache.stock_on = exists;
    }

    return m_wep_cache.stock_on;
}

// ============================================================================
// Equipped Weapon ID (Lua Z.1039-1101)
// ============================================================================

std::optional<int32_t> RE4VRMotion::get_equip_weapon_id() {
    auto* ctx = re4vr::player_context();

    if (ctx == nullptr) {
        return std::nullopt;
    }

    auto* h_updater = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (h_updater == nullptr) {
        return std::nullopt;
    }

    // Lua bekommt hier je nach Lage eine Zahl ODER ein Enum-Userdata und liest
    // dann dessen Backing-Feld "value__". Nativ ist es immer der Rohwert.
    const auto method = find_method(h_updater, "get_EquipWeaponID");

    if (method == nullptr) {
        return std::nullopt;
    }

    auto context = sdk::get_thread_context();
    int32_t wid = 0;

    try {
        wid = method->call_safe<int32_t>(context, h_updater);
    } catch (...) {
        return std::nullopt;
    }

    if (!clear_pending(context, true)) {
        return std::nullopt;
    }

    return wid;
}

// [KNIFE_WEP_ADA] Key fuer den WAFFE-only Messer-Offset (Messer relativ zur
// Hand). Getrennt von "5001" (= Hand-Offset) und von "5001@ada" in weapon_rel
// (= Kalibrierung).
// [KNIFE_KRAUSER 17.09.2026] Krausers Body in Mercenaries. GEMESSEN mit
// zzz_re4_mercs_bodys_probe.lua: die sechs Mercs-Bodys sind ch6i0z0 (Leon, auch
// Leon2), ch6i1z0 (Luis), ch6i2z0 (Krauser), ch6i3z0 (HUNK), ch6i5z0 (Wesker)
// und ch3a8z0_MC (Ada, auch Ada2) -- ch6i4z0 gibt es nicht.
bool RE4VRMotion::is_krauser_body() {
    auto* ctx = re4vr::player_context();

    if (ctx == nullptr) {
        return false;
    }

    auto* b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (b == nullptr) {
        return false;
    }

    return re4vr::obj_name(b) == "ch6i2z0_body";
}

// [KNIFE_KRAUSER 17.09.2026] Krauser bekommt einen EIGENEN Messer-Waffenoffset.
//
// Belegt am 17.09.: das Messer haengt bei ihm korrekt an R_Hand (die Sonde fand
// den Joint, attach_weapon meldete pin_why=ok) -- nur die Lage IN der Hand war
// falsch. Grund: char_now() kennt keinen einzigen Mercs-Body und liefert dort
// leer, also griffen ALLE Mercs-Charaktere denselben Eintrag "<wid>_knifewep"
// ab -- den, der an LEONS Hand kalibriert ist. Bei Leon passt er, bei Krausers
// anders proportioniertem Rig nicht.
//
// LEON BLEIBT UNBERUEHRT: ohne Treffer faellt der Key exakt auf den alten Wert
// zurueck, und Adas Zweig steht bewusst zuerst, damit sich an ihr nichts
// aendert. Krausers Key ist neu und startet daher bei 0 -- also an der nativen
// Lage des Messers, von der aus eingestellt wird.
//
// Weitere Mercs-Charaktere lassen sich hier mit je einer Zeile nachziehen;
// bewusst NICHT vorab gebaut, weil dort bisher nichts gemessen wurde.
std::string RE4VRMotion::knife_wep_key(int32_t wid) {
    const std::string base = std::to_string(wid) + "_knifewep";

    if (char_now() == "ada") {
        return base + "@ada";
    }

    if (is_krauser_body()) {
        // [KNIFE_THROW_OFFSET 17.09.2026] Nach einem Wurf ein EIGENER Satz Werte.
        // Gemessen: der Respawn landet an der nativen Lage (Abstand zur VR-Hand
        // 0,027) statt auf dem eingestellten Offset (0,094) -- und zwar bei
        // identischer Kalibrierung, identischem Key und ohne Mercs-Hook. Statt
        // weiter nach der Quelle zu suchen, wird dieser Fall getrennt eingestellt.
        const std::string nkey = base + (m_knife_after_throw ? "@krauser_throw" : "@krauser");

        // [KNIFE_FLIP_OFFSET 18.09.2026] Geflippt sass das Messer in beiden
        // Faellen (frisch aus dem Holster UND nach einem Wurf) nicht ganz richtig
        // -> je ein eigener Flip-Satz. Beim ersten Mal mit der Drehung des
        // ungeflippten Satzes angelegt, damit die Lage exakt die bisherige ist
        // und von dort aus eingestellt wird.
        if (m_knife_flip.lerp > 0.5f) {
            const std::string fkey = base + (m_knife_after_throw ? "@krauser_throw_flip" : "@krauser_flip");

            if (m_weapon_offset.find(fkey) == m_weapon_offset.end()) {
                const auto& n = get_weapon_offset(nkey);
                auto& f = get_weapon_offset(fkey);
                f.rx = n.rx;
                f.ry = n.ry;
                f.rz = n.rz;
            }

            return fkey;
        }

        return nkey;
    }

    return base;
}

std::string RE4VRMotion::current_weapon_key() {
    const auto wid = get_equip_weapon_id();

    if (!wid.has_value()) {
        return WEAPON_NONE_KEY;
    }

    // [RL_SHARE] Alle Rocket-Launcher-Varianten sind physisch identisch -> EINE
    // gemeinsame Offset-Config. 4901 (RL Special) und 4902 (Infinite RL) werden
    // auf 4900 normalisiert; Tuning einer gilt damit fuer alle, und es gibt
    // keinen JSON-Clobber durch fehlende per-Waffe-Eintraege.
    int32_t w = *wid;

    if (w == 4901 || w == 4902) {
        w = 4900;
    }

    // [KNIFE_ADA] Hier BEWUSST KEIN Charakter-Suffix: dieser Key verschluesselt
    // weapon_offset, und das wirkt auf die HAND, nicht auf die Waffe. Ein Suffix
    // haette zwei Schaeden angerichtet -- es loest das eigentliche Problem nicht
    // (Messer sitzt falsch IN der Hand), und ein neuer Key startet bei 0, womit
    // der getunte Hand-Offset als Ada schlagartig weg waere.
    // Die charakter-getrennte Messer-Lage gehoert an den WAFFEN-Offset.
    return std::to_string(w);
}

// [KNIFE_ADA] Kalibrierungs-Schluessel. Messer werden pro Charakter getrennt,
// alle anderen Waffen bewusst gemeinsam gehalten -- der Umfang wurde
// ausdruecklich als "nur Messer" gewaehlt. Leons Schluessel werden dadurch NIE
// angefasst.
std::string RE4VRMotion::rel_key(int32_t wid) {
    const std::string k = std::to_string(wid);

    if (!is_knife_rel_split_id(wid)) {
        return k;
    }

    return char_now() == "ada" ? k + "@ada" : k;
}

bool RE4VRMotion::is_knife_rel_split_id(int32_t wid) {
    // Deckungsgleich mit FL_KNIFE_IDS und KNIFE_IDS_SWING -- alle drei Listen
    // muessen synchron bleiben.
    switch (wid) {
    case 5000: case 5001: case 5002: case 5003:
    case 5006: case 6107: case 6108: case 6305:
        return true;
    default:
        return false;
    }
}

// [WEP_REL_PERSIST] Lua Z.1256-1276.
void RE4VRMotion::store_weapon_rel() {
    if (!m_wep_cache.id.has_value() || !m_wep_cache.rel_pos.has_value()
        || !m_wep_cache.rel_rot.has_value()) {
        return;
    }

    const auto key = rel_key(*m_wep_cache.id);

    // [PERSIST-FIX] Existiert schon eine gespeicherte Basis? -> NICHT
    // ueberschreiben, sondern die gespeicherte in den Cache laden. Der
    // Live-Sample koennte im Blend gemessen sein und waere damit schlechter.
    // So bleibt der Nullpunkt ueber Save-Load und Sessions stabil; nur die
    // ERSTE, saubere Kalibrierung wird gespeichert.
    if (const auto it = m_weapon_rel.find(key); it != m_weapon_rel.end()) {
        const auto& ex = it->second;
        m_wep_cache.rel_pos = glm::vec3{ex.px, ex.py, ex.pz};
        m_wep_cache.rel_rot = glm::quat{ex.qw, ex.qx, ex.qy, ex.qz};
        m_wep_cache.frozen = true;
        return;
    }

    const glm::vec3 p = *m_wep_cache.rel_pos;
    const glm::quat q = *m_wep_cache.rel_rot;
    WeaponRel r{};
    r.px = p.x;
    r.py = p.y;
    r.pz = p.z;
    r.qx = q.x;
    r.qy = q.y;
    r.qz = q.z;
    r.qw = q.w;
    m_weapon_rel[key] = r;

    m_wep_cache.frozen = true;   // ab jetzt nicht mehr neu sampeln

    // [TEIL1/L9] Ein Datei-Write MITTEN IM GAMEPLAY-TICK -- und jeder Write ist
    // ein Read-Modify-Write der kompletten JSON. 1:1 uebernommen.
    save_config();
}

// ============================================================================
// find_weapon (Lua Z.1277-1380)
// ============================================================================

void RE4VRMotion::find_weapon() {
    auto wid_opt = get_equip_weapon_id();

    // [PARRY_KEEP_GUN] Waehrend eines Links-Klon-Parrys equippt die Engine das
    // Messer; ohne das hier wuerde find_weapon dem Messer folgen und die
    // Schusswaffe aus der rechten Hand nehmen. Streng gegatet: das Fenster
    // setzt AUSSCHLIESSLICH re4_vr_weapons2.lua im Zweig __re4_knife_left_clone.
    // Ein Parry mit dem ECHTEN Messer rechts laeuft hier nie durch.
    {
        const int32_t w = wid_opt.value_or(0);
        const bool kn = is_knife_rel_split_id(w) && w != 0;

        // [FIX, Log-Beweis] `wid > 0` ist entscheidend: nach dem Parry steht die
        // equippte Waffe auf -1 ("gar nichts", equipWeapon mit 0xFFFFFFFF). Ohne
        // diese Bedingung galt -1 als gueltige Nicht-Messer-Waffe und hat den
        // Merker sofort ueberschrieben -> es gab nichts mehr zurueckzuholen.
        if (wid_opt.has_value() && w > 0 && !kn) {
            m_wep_cache._parry_last_gun = static_cast<double>(w);
        } else {
            const double ut = re4vr::lua_get_number("__re4_parry_keep_gun_until", 0.0);

            // Zeitbasiert -> laeuft von selbst ab, kann nicht haengenbleiben.
            // Die Uhr MUSS os.clock() sein: den Zeitstempel schreibt weapons2.
            if (ut > 0.0 && clock_now() < ut && m_wep_cache._parry_last_gun.has_value()) {
                wid_opt = static_cast<int32_t>(*m_wep_cache._parry_last_gun);
            }
        }
    }

    if (!wid_opt.has_value() || *wid_opt == 0) {
        m_wep_cache.id.reset();
        drop(m_wep_cache.go);
        drop(m_wep_cache.tf);
        m_wep_cache.rel_pos.reset();
        m_wep_cache.rel_rot.reset();
        m_wep_cache.calib.reset();
        m_wep_cache.settle.reset();
        return;
    }

    const int32_t wid = *wid_opt;

    if (m_wep_cache.id.has_value() && *m_wep_cache.id == wid && m_wep_cache.tf.obj != nullptr) {
        // [SAVE-LOAD -- ACHTUNG, TOTER SCHUTZ]
        // Das Original schreibt:
        //     local ok = pcall(... get_Position)
        //     local valid = ok and (sc(wep_cache.tf, "get_Valid") ~= false)
        // sc() liefert `ok and r or nil` -- ein echtes `false` kommt als NIL an,
        // und `nil ~= false` ist WAHR. Der get_Valid-Teil ist damit strukturell
        // immer wahr; es entscheidet ALLEIN der get_Position-Aufruf.
        // Ein ehrliches optional<bool> wuerde hier ploetzlich Waffen ablehnen
        // und Caches invalidieren -- etwas, das das getestete Lua NIE getan hat.
        glm::vec4 probe{};
        const bool valid = get_vec4(m_wep_cache.tf.obj, "get_Position", probe);

        if (valid) {
            return;
        }
    }

    m_wep_cache.id.reset();
    drop(m_wep_cache.go);
    drop(m_wep_cache.tf);
    m_wep_cache.rel_pos.reset();
    m_wep_cache.rel_rot.reset();
    m_wep_cache.settle.reset();

    // [WEP_REL_PERSIST] gespeicherten Offset wiederverwenden -> NICHT neu
    // kalibrieren, damit weder Skip noch Pin den Wert je wieder veraendern.
    if (const auto it = m_weapon_rel.find(rel_key(wid)); it != m_weapon_rel.end()) {
        const auto& s = it->second;
        m_wep_cache.rel_pos = glm::vec3{s.px, s.py, s.pz};
        m_wep_cache.rel_rot = glm::quat{s.qw, s.qx, s.qy, s.qz};
        m_wep_cache.calib.reset();
        m_wep_cache.frozen = true;   // eingefroren -> NIE neu sampeln
    } else {
        m_wep_cache.calib = WepCache::Calib{CALIB_WAIT, CALIB_SAMPLE};
        m_wep_cache.frozen = false;
    }

    auto* pt = find_player_body();

    if (pt == nullptr) {
        return;
    }

    // [GO_SUFFIX] Waffen-GOs heissen NICHT immer exakt "wp####". Bei Ada haengen
    // z.B. Punisher MC und Rocket Launcher als "wp6112_AO"/"wp6111_AO" im Baum.
    // Der fruehere exakte Vergleich fand sie nie -> die Waffe wurde nie an die
    // VR-Hand gehaengt und blieb nativ stehen. Das sah aus wie "unsichtbar", war
    // aber ein Fund-Problem.
    // REIHENFOLGE IST WICHTIG: bei anderen Waffen ist "_AO" ein SCHATTEN-PROXY
    // neben dem echten GO. Nur wenn es kein plain "wp####" gibt, darf der
    // Suffix-Treffer genommen werden -- sonst haengt die Hand am Schatten.
    char base[16]{};
    std::snprintf(base, sizeof(base), "wp%04d", wid);

    const auto scan = [this, pt](const std::string& name,
                                 ::REManagedObject*& out_go,
                                 ::REManagedObject*& out_tf) {
        out_go = nullptr;
        out_tf = nullptr;

        auto* child = re4vr::call_safe<::REManagedObject*>(pt, "get_Child");
        int count = 0;

        while (child != nullptr && count < 64) {
            ++count;
            auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

            if (go != nullptr && re4vr::obj_name(go) == name) {
                // [TOTER DUPLIKAT-FILTER] Original: `if draw ~= false then`.
                // sc() macht aus einem echten false ein nil, und `nil ~= false`
                // ist wahr -- der Filter verwirft NIE etwas, der erste
                // Namenstreffer gewinnt. Genau so nachgebaut: get_DrawSelf wird
                // gar nicht erst abgefragt.
                out_go = go;
                out_tf = child;
                return;
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }
    };

    for (const auto* suffix : {"", "_AO", "_MC"}) {
        ::REManagedObject* go = nullptr;
        ::REManagedObject* tf = nullptr;
        scan(std::string{base} + suffix, go, tf);

        if (go != nullptr) {
            m_wep_cache.id = wid;
            store(m_wep_cache.go, go);
            store(m_wep_cache.tf, tf);
            return;
        }
    }
}

// Rigid-Offset Hand->Waffe aus der NATIVEN Engine-Verkettung lesen -- nur
// waehrend des Kalibrier-Fensters, in dem die Hand nicht geschrieben wird
// (Lua Z.1382-1403).
bool RE4VRMotion::sample_weapon_rel_direct() {
    if (m_wep_cache.tf.obj == nullptr) {
        return false;
    }

    if (!is_joint_valid(m_right_hand.joint.obj)) {
        return false;
    }

    glm::vec3 hp{};
    glm::quat hr{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 wp{};
    glm::quat wr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_vec3(m_right_hand.joint.obj, "get_Position", hp)
        || !get_quat(m_right_hand.joint.obj, "get_Rotation", hr)
        || !get_vec3(m_wep_cache.tf.obj, "get_Position", wp)
        || !get_quat(m_wep_cache.tf.obj, "get_Rotation", wr)) {
        return false;
    }

    const glm::quat hri = glm::conjugate(hr);
    m_wep_cache.rel_pos = hri * (wp - hp);
    m_wep_cache.rel_rot = glm::normalize(hri * wr);
    return true;
}

// ============================================================================
// Kalibrier-Zustandsmaschine (Lua Z.1401-1483)
// Laeuft nur im LateUpdateBehavior-Pass.
// ============================================================================

void RE4VRMotion::update_weapon_calibration() {
    if (!m_wep_cache.calib.has_value() || m_wep_cache.tf.obj == nullptr) {
        return;
    }

    // [RECOIL_ON_HAND] Nicht waehrend eines Kicks sampeln: seit der Recoil auf
    // der Hand liegt, wuerde eine im Kick gemessene Nulllage die Verkippung
    // DAUERHAFT einfrieren (frozen + save_config). Ein paar Frames warten
    // kostet nichts.
    if (re4vr::lua_table_is_truthy("vr_recoil", "active")) {
        return;
    }

    auto& c = *m_wep_cache.calib;

    if (c.wait > 0) {
        --c.wait;
        return;
    }

    if (sample_weapon_rel_direct()) {
        --c.sample;

        if (c.sample <= 0) {
            m_wep_cache.calib.reset();   // rel ist eingefroren
            store_weapon_rel();          // [WEP_REL_PERSIST] einfrieren + speichern
        }
    }
}

// Waehrend Sample-Fenster ODER Settle-Phase rechte Hand + Waffe NICHT schreiben.
bool RE4VRMotion::weapon_calib_suspends_hand() const {
    if (m_wep_cache.settle.has_value()) {
        return true;
    }

    return m_wep_cache.calib.has_value() && m_wep_cache.calib->wait <= 0;
}

// Waffenwechsel laeuft? -> Draw-/Holster-Anim NATIV durchlaufen lassen (rechte
// Hand frei), danach frisch kalibrieren.
bool RE4VRMotion::is_weapon_changing() {
    auto* ctx = re4vr::player_context();

    if (ctx == nullptr) {
        return false;
    }

    // Lua: `v == true` -- strikt. Ein fehlgeschlagener Aufruf zaehlt als false.
    return re4vr::call_safe<bool>(ctx, "get_IsWeaponChanging") == true;
}

bool RE4VRMotion::update_weapon_changing_gate() {
    const bool changing = is_weapon_changing();

    if (m_was_weapon_changing && !changing && !m_wep_cache.frozen) {
        // Wechsel beendet -> Settle-Phase starten (lockt bei Konvergenz).
        // Bei eingefrorenem (gespeichertem) Offset NICHT -> Wert bleibt stabil.
        m_wep_cache.settle = WepCache::Settle{0, SETTLE_TIMEOUT};
        m_wep_cache.calib.reset();
    }

    m_was_weapon_changing = changing;
    return changing;
}

void RE4VRMotion::update_weapon_settle() {
    if (!m_wep_cache.settle.has_value()) {
        return;
    }

    if (m_wep_cache.tf.obj == nullptr) {
        m_wep_cache.settle.reset();
        return;
    }

    auto& s = *m_wep_cache.settle;
    const auto prev = m_wep_cache.rel_pos;

    if (sample_weapon_rel_direct() && prev.has_value()) {
        const glm::vec3 cur = *m_wep_cache.rel_pos;
        const float dx = cur.x - prev->x;
        const float dy = cur.y - prev->y;
        const float dz = cur.z - prev->z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (d < SETTLE_EPS) {
            ++s.stable;
        } else {
            s.stable = 0;
        }
    }

    --s.timeout;

    if (s.stable >= SETTLE_STABLE_FRAMES || s.timeout <= 0) {
        m_wep_cache.settle.reset();
        store_weapon_rel();
    }
}

// ============================================================================
// [SKULL_SHAKER_COCK] Lua Z.1492-1518
//
// Waehrend die native Cock-Anim laeuft (__vr_pump_anim_active, nur wp6001) die
// Waffe in Pitch loopen lassen. Pivot = rechte Hand; Two-Hand ist im Fenster aus.
// Nur die Keyframes/Vorschau sind ausgebaut -- die Drehung selbst ist exakt der
// bewaehrte Stand davor (feste Formel, genau 1x 360 Grad ueber die Anim).
// ============================================================================

glm::quat RE4VRMotion::skullshaker_cock_spin(const glm::quat& wrot) {
    constexpr float SKULL_SPIN_PERIOD = 0.40f;   // s pro Umdrehung (nur Fallback)
    constexpr float SKULL_SPIN_DIR = -1.0f;      // Drehrichtung, so gewollt
    constexpr float SKULL_SPIN_TURNS = 1.0f;
    constexpr float PI = 3.14159265358979323846f;

    // [NUR wp6001 -- 13.09.2026, gemessen beim Tester]
    // Der Spin haing allein an __vr_pump_anim_active, OHNE Waffencheck. Das Flag
    // ueberlebt aber den Waffenwechsel: im Log (re4_wrot_ursache.txt, 16:32:22)
    // steht spin=true, waehrend die wid schon 4002 ist -- die RED9 bekam also den
    // Cock-Spin der Skull Shaker aufgesetzt.
    //
    // Warum das genau das gemeldete Bild erzeugt:
    //   * Es dreht NUR, die Position bleibt korrekt (dPos war bitgenau 0.0000).
    //   * Der Zeit-Fallback unten laesst `frac` weiterlaufen -> der Winkel
    //     WAECHST, daher die gemessenen 31 / 94 / 150 / 168 Grad.
    //   * Es wird jeden Frame frisch aufgerechnet -> der Zustand "bleibt", und
    //     kein Frame-Guard konnte dagegen helfen.
    //   * Nur nach einem Wechsel VON der Skull Shaker, weil nur sie das Flag
    //     setzt -- die Punisher war nie betroffen.
    const int32_t spin_wid = m_wep_cache.id.value_or(0);

    if (spin_wid != 6001 || re4vr::lua_get_tribool("__vr_pump_anim_active") != 1) {
        m_skull_spin.t0.reset();
        return wrot;
    }

    // Fortschritt 0..1 aus reload.lua (NormalizeTime der Waffen-Anim).
    // Lua: `tonumber(rawget(...))` -- fehlt der Wert, greift der Zeit-Fallback.
    float frac = 0.0f;
    const auto prog = re4vr::lua_get_number_opt("__vr_pump_anim_progress");

    if (prog.has_value()) {
        frac = *prog < 0.0 ? 0.0f : static_cast<float>(*prog);
    } else {
        // [TEIL1/L2] Zeit-Fallback: attach_weapon laeuft MEHRFACH pro Frame.
        const double now = clock_now();

        if (!m_skull_spin.t0.has_value()) {
            m_skull_spin.t0 = now;
        }

        frac = static_cast<float>((now - *m_skull_spin.t0) / SKULL_SPIN_PERIOD);
    }

    const float h = (SKULL_SPIN_DIR * frac * SKULL_SPIN_TURNS * (2.0f * PI)) * 0.5f;
    const glm::quat pitch{std::cos(h), std::sin(h), 0.0f, 0.0f};   // um lokale X
    return glm::normalize(wrot * pitch);
}

// ============================================================================
// [KNIFE_FLIP] Lua Z.1520-1554
//
// 180-Grad-Reverse-Grip-Flip des Messers, per RT getoggelt (binding setzt
// __vr_knife_flip). Sanft in die Flip-Stellung lerpen und dem Messer-Transform
// eine Rotation um bis zu 180 Grad ueberlagern. Achse = lokale X (Pitch).
// ============================================================================

void RE4VRMotion::knife_play_sound(int32_t id) {
    auto* go = m_wep_cache.go.obj;

    if (go == nullptr || m_t_snd == nullptr) {
        return;
    }

    auto* scn = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_snd);

    if (scn != nullptr) {
        re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", static_cast<uint32_t>(id));
    }
}

// [CHOKE-FLIP] s. Header. Setzt den Reverse-Grip sofort auf die Endstellung --
// kein Lerp, kein Flip-Sound (prev_target wandert mit), damit sich das Messer
// beim Zugriff nicht erst sichtbar dreht.
void RE4VRMotion::force_knife_flip() {
    // Ohne Messer in der Hand gibt es nichts zu flippen; knife_flip_spin
    // steigt dort ohnehin aus und nullt den Lerp.
    if (re4vr::lua_get_tribool("__re4_knife_equipped") != 1) {
        return;
    }

    re4vr::lua_set_bool("__vr_knife_flip", true);
    m_knife_flip.lerp = 1.0f;
    m_knife_flip.prev_target = 1.0f;
}

glm::quat RE4VRMotion::knife_flip_spin(const glm::quat& wrot) {
    constexpr float KNIFE_FLIP_SPEED = 0.18f;        // Lerp/Frame (~0.15 s bei 90 fps)
    constexpr float KNIFE_FLIP_EULER_X = 180.0f;     // welche Achse flippt
    constexpr int32_t KNIFE_FLIP_SND = 1007228839;   // Sound bei JEDEM Flip, hin UND zurueck

    if (re4vr::lua_get_tribool("__re4_knife_equipped") != 1) {
        m_knife_flip.lerp = 0.0f;
        m_knife_flip.prev_target = 0.0f;
        return wrot;
    }

    const float target = re4vr::lua_get_tribool("__vr_knife_flip") == 1 ? 1.0f : 0.0f;

    // [KNIFE_FLIP SND] Flanke des ZIELS (Tastendruck), nicht des Lerps.
    // prev_target wird bei erster Detektion gesetzt -> kein Mehrfach-Trigger in
    // den mehreren attach_weapon-Paessen pro Frame.
    if (m_knife_flip.prev_target != target) {
        m_knife_flip.prev_target = target;
        knife_play_sound(KNIFE_FLIP_SND);
    }

    // [TEIL1/L2 -- WICHTIG] Der Lerp laeuft PRO PASS, nicht pro Frame: die
    // Flip-Dauer haengt damit an der Anzahl der Engine-Phasen, nicht an der
    // Frametime. Wer hier auf einmal pro Frame umbaut, macht den Flip sichtbar
    // langsamer.
    const float spd = static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_speed", KNIFE_FLIP_SPEED));

    // [FLIP-MESSUNG 04.09. -- live gemeldet: "der knife flip mit der rechten
    // Hand folgt nicht mehr einem gewissen speed"]
    // Der Lerp laeuft PRO PASS. Wieviele Paesse ein voller Flip braucht, ist
    // deshalb die eine Zahl, die "zu langsam" beweist oder widerlegt:
    // bei spd 0.18 sind rund 6 Paesse zu erwarten. Kommen deutlich mehr
    // heraus, laeuft attach_weapon seltener als in Lua; kommt spd falsch an,
    // steht es in derselben Zeile. Raus, sobald geklaert.
    const float lerp_before = m_knife_flip.lerp;

    if (m_knife_flip.lerp < target) {
        m_knife_flip.lerp = std::min(target, m_knife_flip.lerp + spd);
    } else if (m_knife_flip.lerp > target) {
        m_knife_flip.lerp = std::max(target, m_knife_flip.lerp - spd);
    }

    if (lerp_before != m_knife_flip.lerp) {
        ++m_flip_diag_passes;
    } else if (m_flip_diag_passes > 0) {
        // Ziel erreicht -- der Flip ist durch.
        // [LOG AUSGEBAUT 2026-09-08] Der Mitschnitt nach re4_knife_flip.txt ist
        // raus; der Zaehler bleibt nur noch als Zustand stehen.
        m_flip_diag_passes = 0;
    }

    if (m_knife_flip.lerp <= 0.0001f) {
        return wrot;
    }

    const glm::quat flip = quat_from_euler_deg(KNIFE_FLIP_EULER_X * m_knife_flip.lerp, 0.0f, 0.0f);
    return glm::normalize(wrot * flip);
}

// ============================================================================
// [KNIFE_FLIP KS] Lua Z.1556-1584
//
// Bei JEDEM Killswitch (Cutscene, z.B. Finisher-Kill) MUSS das Messer nativ
// stehen. motion pinnt im KS nicht (attach_weapon uebersprungen) -> das Messer
// behielte sonst seine geflippte LOKALE Rotation und folgte der Hand-Bone
// verkehrt (die native Anim sticht dann mit dem Griff zu).
// ============================================================================

void RE4VRMotion::knife_ks_restore_native() {
    if (re4vr::lua_get_tribool("__re4_knife_equipped") != 1) {
        return;
    }

    // [FLIP_MERKEN] Der Flip MUSS im KS visuell aus sein -- aber der
    // WUNSCH-Zustand darf dabei nicht verloren gehen. Vorher wurde
    // __vr_knife_flip hart auf false gesetzt und weggeworfen, wodurch JEDER
    // Treffer/Stagger (KS3/Damage) den Reverse-Grip gekillt hat, obwohl nur die
    // Finisher-Cutscene gemeint war. Nur beim Wechsel true->false merken; die
    // Folgeframes im KS sehen bereits false und duerfen den Merker nicht
    // ueberschreiben.
    if (re4vr::lua_get_tribool("__vr_knife_flip") == 1) {
        re4vr::lua_set_bool("__re4_knife_flip_pre_ks", true);
    }

    m_knife_flip.lerp = 0.0f;
    m_knife_flip.prev_target = 0.0f;
    re4vr::lua_set_bool("__vr_knife_flip", false);

    // [KNIFE_KS_FOLLOW] Die VR-Hand (cache.rh_world) ist im KS aus -> das Messer
    // an den NATIVEN R_Hand-Joint pinnen, mit demselben Mount-Offset, aber OHNE
    // Flip. So folgt es der nativen Anim, statt in der Luft einzufrieren.
    if (m_right_hand.joint.obj == nullptr || m_wep_cache.tf.obj == nullptr
        || !m_wep_cache.rel_pos.has_value() || !m_wep_cache.rel_rot.has_value()) {
        return;
    }

    glm::vec3 hw{};
    glm::quat hr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_vec3(m_right_hand.joint.obj, "get_Position", hw)
        || !get_quat(m_right_hand.joint.obj, "get_Rotation", hr)) {
        return;
    }

    const glm::vec3 wpos = hw + (hr * (*m_wep_cache.rel_pos));
    const glm::quat wrot = glm::normalize(hr * (*m_wep_cache.rel_rot));

    set_vec4(m_wep_cache.tf.obj, "set_Position", glm::vec4{wpos.x, wpos.y, wpos.z, 1.0f});
    set_quat(m_wep_cache.tf.obj, "set_Rotation", wrot);
}


// ============================================================================
// [KNIFE_RH_PIN 2026-09-08] Messer rechts an den R_Hand-Joint haengen und nur
// noch lokal posieren -- dasselbe Rezept wie der linke Klon
// (RE4VRWeapons2::clone_spawn: set_Parent + set_ParentJoint, danach
// clone_apply_pose mit set_LocalPosition/-Rotation).
//
// Die lokale Pose IST rel_pos/rel_rot: beide sind bereits relativ zur Hand
// gerechnet, und der Parent ist genau dieser Hand-Joint.
// ============================================================================

void RE4VRMotion::knife_pin_release() {
    if (!m_knife_pin.active) {
        return;
    }

    // [MAP-RELOAD 13.09.2026 -- Haenger in Mercenaries, im Log belegt]
    // 21:56:57 "Exception thrown in call to set_Parent", Stack tick ->
    // attach_weapon -> knife_pin_release; sechs Sekunden spaeter kam kein Bild
    // mehr (der D3D-Wachhund rehookte danach 14x ins Leere).
    //
    // Der Pin ueberlebte das Neuladen der Map: home_parent zeigte in die
    // abgeraeumte Szene. obj_ok unten faengt das NICHT -- get_Valid schuetzt
    // hier nicht (s. Messer-Notiz vom 12.09.).
    //
    // Dieselbe Pruefung steht seit dem 09.09. schon in knife_pin_apply, aber
    // eben nur DORT: der Weg ueber attach_weapon (Waffe ist kein rechtes
    // Messer mehr) lief ungeprueft hier herein. Jetzt haengt sie an der
    // Freigabe selbst und gilt damit fuer JEDEN Aufrufer.
    {
        auto* btf_now = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

        if (m_knife_pin.body_at_pin != 0
            && m_knife_pin.body_at_pin != reinterpret_cast<uintptr_t>(btf_now)) {
            knife_pin_forget();   // fallen lassen, OHNE set_Parent auf die Leiche

            return;
        }
    }

    // Heimatparent zurueck, solange beide Seiten noch gueltig sind.
    if (re4vr::obj_ok(m_knife_pin.tf.obj) && re4vr::obj_ok(m_knife_pin.home_parent.obj)) {
        re4vr::call_safe<void*>(m_knife_pin.tf.obj, "set_Parent", m_knife_pin.home_parent.obj);
    }

    drop(m_knife_pin.tf);
    drop(m_knife_pin.home_parent);
    m_knife_pin.active = false;
}

void RE4VRMotion::knife_pin_forget() {
    if (!m_knife_pin.active) {
        return;
    }

    // KEIN set_Parent hier: Wurf (knife_detach/knife_reattach) und Choke haengen
    // das GO selbst um. Zwei Mechaniken, die sich denselben Parent merken,
    // ueberschreiben sich gegenseitig -- deshalb hier nur den eigenen Zustand
    // fallen lassen und beim naechsten regulaeren Durchlauf frisch pinnen.
    drop(m_knife_pin.tf);
    drop(m_knife_pin.home_parent);
    m_knife_pin.body_at_pin = 0;
    m_knife_pin.active = false;
}

void RE4VRMotion::knife_pin_apply(::REManagedObject* wep_tf, const glm::vec3& wpos,
                                  const glm::quat& wrot) {
    // [LADEN 2026-09-09] obj_ok reicht hier NICHT als Schutz: es prueft nur, ob
    // der Speicher noch aussieht wie ein verwaltetes Objekt -- nach einem
    // Ladevorgang steht dort in aller Regel noch etwas Passendes, das Objekt
    // gehoert aber zur alten Szene. Der Hand-Joint wurde zudem bisher nur auf
    // nullptr geprueft.
    //
    // Belegt am 09.09.2026 mit Symbolen: Tod im Krauser-Messerkampf, neu laden,
    // dann warf attach_weapon -> knife_pin_apply -> set_Parent jeden Frame.
    // Davor standen im Log 28 fehlgeschlagene get_Position auf genau dem
    // Hand-Joint hier unten.
    if (!re4vr::obj_ok(wep_tf) || !re4vr::obj_ok(m_right_hand.joint.obj)) {
        return;
    }

    auto* btf_now = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    // [LADEN] Anderer Koerper als beim Pinnen -> es lag ein Ladevorgang oder
    // Charakterwechsel dazwischen. Zustand fallen lassen, OHNE die alten
    // Handles anzufassen (knife_pin_release wuerde genau das tun und dabei
    // set_Parent auf eine Leiche rufen). Dasselbe Muster wie in arm_chain: der
    // gemerkte Transform dient nur dem Vergleich und loest den Flush aus.
    if (m_knife_pin.active
        && m_knife_pin.body_at_pin != 0
        && m_knife_pin.body_at_pin != (uintptr_t)btf_now) {
        knife_pin_forget();
    }

    // Andere Transform als beim letzten Pin (Waffenwechsel) -> erst loesen.
    if (m_knife_pin.active && m_knife_pin.tf.obj != wep_tf) {
        knife_pin_release();
    }

    if (!m_knife_pin.active) {
        auto* btf = btf_now;

        if (!re4vr::obj_ok(btf)) {
            return;
        }

        // Rueckweg VOR dem ersten Umhaengen merken.
        store(m_knife_pin.home_parent,
              re4vr::call_safe<::REManagedObject*>(wep_tf, "get_Parent"));

        re4vr::call_safe<void*>(wep_tf, "set_Parent", btf);

        if (!set_parent_joint(wep_tf, "R_Hand")) {
            // Joint nicht gesetzt -> Pin gilt nicht, Heimatparent sofort zurueck.
            if (re4vr::obj_ok(m_knife_pin.home_parent.obj)) {
                re4vr::call_safe<void*>(wep_tf, "set_Parent", m_knife_pin.home_parent.obj);
            }

            drop(m_knife_pin.home_parent);

            return;
        }

        store(m_knife_pin.tf, wep_tf);
        m_knife_pin.body_at_pin = (uintptr_t)btf;
        m_knife_pin.active = true;
    }

    // Die FERTIGE Weltpose in den Raum des Joints umrechnen -- so bleiben alle
    // Stufen davor (Recoil, Zwei-Hand-IK, Offsets, Mercs-Hook) unveraendert
    // erhalten; nur der Bezug wechselt von Welt auf Joint.
    glm::vec3 jpos{};
    glm::quat jrot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_vec3(m_right_hand.joint.obj, "get_Position", jpos)
        || !get_quat(m_right_hand.joint.obj, "get_Rotation", jrot)) {
        return;
    }

    const glm::quat jinv = glm::conjugate(glm::normalize(jrot));
    const glm::vec3 lp = jinv * (wpos - jpos);

    set_vec4(wep_tf, "set_LocalPosition", glm::vec4{lp.x, lp.y, lp.z, 1.0f});
    set_quat(wep_tf, "set_LocalRotation", glm::normalize(jinv * wrot));
}

// ============================================================================
// attach_weapon (Lua Z.1586-1793)
//
// Die equippte wpXXXX-GO wird pro Phase DIREKT auf die Hand-Pose geschrieben,
// statt auf die Engine-Verkettung zu warten -- sonst zieht die Waffe nach.
//
// [TEIL1/L2] Diese Funktion laeuft MEHRFACH pro Frame (einmal je Phase). Das
// ist tragend: der Knife-Flip-Lerp haengt daran (s. knife_flip_spin).
// ============================================================================

void RE4VRMotion::attach_weapon() {
    // [PIN_WHY] Reine Diagnose: WARUM haengt die Waffe in diesem Frame nicht an
    // der Hand? Gemessen ist, dass sie beim Shell-Einlegen 1-2 Frames an ihre
    // native Lage springt; welcher Ausstieg das verursacht, war lange unbekannt
    // -- der Killswitch war es nachweislich nicht.
    re4vr::lua_set_string("__re4_pin_why", "?");

    // [KNIFE_THROW_OFFSET 17.09.2026] Merken, ob das Messer gerade aus einem Wurf
    // zurueckkommt -- knife_wep_key waehlt danach den zweiten Wertesatz.
    // AN: sobald ein Wurf fliegt. AUS: beim naechsten Wechsel der equippten
    // Waffe; Wegstecken und neu Ziehen faellt darunter, weil die wid dabei
    // zwischendurch wechselt.
    if (re4vr::lua_get_tribool("__re4_knife_flying") == 1) {
        m_knife_after_throw = true;
    }

    if (m_wep_cache.id != m_knife_last_wid) {
        m_knife_last_wid = m_wep_cache.id;

        if (re4vr::lua_get_tribool("__re4_knife_flying") != 1) {
            m_knife_after_throw = false;
        }
    }

    if (!m_wep_attach_enabled) {
        re4vr::lua_set_string("__re4_pin_why", "attach_aus");
        return;
    }

    // [MERC_BOW_PIN] Krausers Compound Bow haengt NATIV am Hand-Joint (set_Parent
    // + set_ParentJoint, wie die Magazin-/Shell-Klone), erledigt von
    // re4_vr_merc.lua. Hier NUR aussteigen, damit sich beide nicht um die
    // Transform pruegeln. Das Flag setzt ausschliesslich das Mercs-Script.
    if (re4vr::lua_get_tribool("__re4_merc_bow_pinned") == 1) {
        re4vr::lua_set_string("__re4_pin_why", "merc_bow");
        return;
    }

    // [KNIFE_THROW] Waehrend das geworfene Messer fliegt NICHT an die Hand
    // pinnen -> sonst kaempfen Flug-Override und Hand-Pin um die Transform.
    if (re4vr::lua_get_tribool("__re4_knife_flying") == 1) {
        // [KNIFE_RH_PIN] Waehrend des Flugs gehoert das GO dem Wurf-Code
        // (knife_detach/knife_reattach). Unseren Pin-Zustand fallen lassen,
        // sonst haelt er sich fuer angehaengt, obwohl der Wurf laengst
        // umgeparentet hat -- danach kam das Messer unsichtbar zurueck.
        knife_pin_forget();
        re4vr::lua_set_string("__re4_pin_why", "knife_fly");
        return;
    }

    // [CHOKE_KNIFE_STUCK] Solange das Messer im gechokten Gegner steckt (choke
    // haengt es an dessen Hals-Joint), darf der Hand-Pin nicht dagegenhalten.
    // BEWUSST ein Zeitstempel und kein Flag: das Choke frischt ihn jeden Frame
    // auf. Stirbt das Script mitten drin, ist der Ausstieg nach 0,2 s von selbst
    // zu -- ein Flag koennte haengenbleiben und das Messer dauerhaft ungepinnt
    // lassen. Die Uhr MUSS os.clock() sein, den Stempel schreibt choke.
    if (const auto cks = re4vr::lua_get_number_opt("__re4_choke_knife_stuck");
        cks.has_value() && (clock_now() - *cks) < 0.2) {
        re4vr::lua_set_string("__re4_pin_why", "choke");
        return;
    }

    if (m_wep_cache.tf.obj == nullptr || !m_wep_cache.rel_pos.has_value()
        || !m_wep_cache.rel_rot.has_value()) {
        char why[64]{};
        std::snprintf(why, sizeof(why), "cache tf=%s rel=%s rot=%s",
                      m_wep_cache.tf.obj != nullptr ? "ok" : "NIL",
                      m_wep_cache.rel_pos.has_value() ? "ok" : "NIL",
                      m_wep_cache.rel_rot.has_value() ? "ok" : "NIL");
        re4vr::lua_set_string("__re4_pin_why", why);
        return;
    }

    // [KNIFE_HAND] Die linke Hand wird im Render-Pass ERST NACH attach_weapon
    // berechnet und ist hier meist leer -> Fallback auf die publizierten
    // Globals (max. 1 Frame alt, fuer den Pin unkritisch). Ohne den fiele der
    // Zweig faelschlich auf den rechten zurueck.
    const auto lw = m_cache.lh_world.has_value() ? m_cache.lh_world : re4vr::lua_get_vec3("__vr_lh_world");
    const auto lr = m_cache.lh_rot.has_value() ? m_cache.lh_rot : re4vr::lua_get_quat("__vr_lh_rot");

    // [KNIFE_RH_PIN 2026-09-08] Nur das Messer in der RECHTEN Hand wird unten an
    // den R_Hand-Joint GEPARENTET und lokal posiert -- genau wie der linke Klon
    // (Weapons2: set_Parent + set_ParentJoint, dann set_LocalPosition/-Rotation).
    // Ein erster Versuch NUR mit lokaler Pose (ohne Parenting) liess das Messer
    // verschwinden: ohne Parent gibt es keinen Bezug, in dem die Werte stimmen.
    // Die Waffen bleiben bei der Weltschreibung, die sitzt felsenfest.
    bool knife_right_local = false;

    glm::vec3 hand_world{};
    glm::quat hand_rot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 rel_pos{};
    glm::quat rel_rot{1.0f, 0.0f, 0.0f, 0.0f};

    // Additiver WAFFE-only Offset: Position im Hand-Frame, Rotation lokal an die
    // Waffe. Genau dieses Muster gibt es FUENFMAL (Matilda-Stock, knifewep,
    // 6102_bowwep, 5403, 5405) -- s. TEIL1/K4.
    const auto apply_wep_only = [this](glm::vec3& rp, glm::quat& rr, const std::string& key) {
        const auto& o = get_weapon_offset(key);
        rp = glm::vec3{rp.x + o.px, rp.y + o.py, rp.z + o.pz};

        if (o.rx != 0.0f || o.ry != 0.0f || o.rz != 0.0f) {
            rr = glm::normalize(rr * quat_from_euler_deg(o.rx, o.ry, o.rz));
        }
    };

    if (re4vr::lua_get_string("__re4_knife_hand") == "left" && lw.has_value() && lr.has_value()) {
        hand_world = *lw;
        hand_rot = *lr;

        // Basis = sagittal gespiegelter rechter Offset (Pos-X negiert,
        // Rot (w,-x,y,z)); darauf der PRO-MESSER Links-Feinschliff aus
        // __re4_knife_lh_off_map. Fehlt ein Eintrag -> 0, also reine Spiegelung.
        const glm::vec3 rp = *m_wep_cache.rel_pos;
        const glm::quat rr = *m_wep_cache.rel_rot;
        glm::vec3 o{};
        glm::vec3 orot{};
        bool have_o = false;

        if (m_wep_cache.id.has_value()) {
            have_o = re4vr::lua_get_lh_off(*m_wep_cache.id, o, orot);
        }

        rel_pos = glm::vec3{-rp.x + (have_o ? o.x : 0.0f),
                            rp.y + (have_o ? o.y : 0.0f),
                            rp.z + (have_o ? o.z : 0.0f)};
        rel_rot = glm::quat{rr.w, -rr.x, rr.y, rr.z};

        if (have_o && (orot.x != 0.0f || orot.y != 0.0f || orot.z != 0.0f)) {
            rel_rot = glm::normalize(rel_rot * quat_from_euler_deg(orot.x, orot.y, orot.z));
        }
    } else {
        if (!m_cache.rh_world.has_value() || !m_cache.rh_rot.has_value()) {
            return;
        }

        hand_world = *m_cache.rh_world;
        hand_rot = *m_cache.rh_rot;
        rel_pos = *m_wep_cache.rel_pos;
        rel_rot = *m_wep_cache.rel_rot;

        // [MATILDA_STOCK] Bewegt NUR die Waffe relativ zur Hand; die HAND bleibt
        // 1:1 wie ohne Stock.
        if (m_cache.matilda_stock) {
            apply_wep_only(rel_pos, rel_rot, "4004_stockwep");
        }

        // [KNIFE_WEP_ADA] Messer WAFFE-only relativ zur Hand. Grund: der
        // per-Waffe-Handoffset verschiebt die HAND, das Messer folgt starr --
        // gebraucht wird aber die Lage des Messers IN der Hand. Eigener Key je
        // Messer, fuer Ada mit "@ada"-Suffix -> Leons Wert bleibt unberuehrt.
        if (m_wep_cache.id.has_value() && is_knife_rel_split_id(*m_wep_cache.id)) {
            apply_wep_only(rel_pos, rel_rot, knife_wep_key(*m_wep_cache.id));
            knife_right_local = true;   // [KNIFE_RH_PIN] unten an R_Hand haengen
        }

        // [BOW_6102] Die Blast Crossbow traegt in ihrer eingefrorenen
        // Kalibrierung einen festen ~15-cm-Versatz. Der ist NICHT einmalig
        // falsch gemessen -- er kommt nach dem Loeschen reproduzierbar zurueck,
        // ist also der Modell-Ursprung. EIGENER Key "6102_bowwep", weil "6102"
        // bereits der getunte HAND-Offset ist und doppelt wirken wuerde.
        if (m_wep_cache.id.has_value() && *m_wep_cache.id == 6102) {
            apply_wep_only(rel_pos, rel_rot, "6102_bowwep");
        }

        // [SKULL_SPIN_ENTFERNT] Hier lagen die Skull-Shaker-Keyframes -- wp6001
        // bekommt in attach_weapon keine Sonderbehandlung mehr.

        // [EGG_5403] Der Offset wirkt fuer das Ei nicht auf die Hand ->
        // stattdessen WAFFE-only. Nutzt DENSELBEN Key wie der bestehende Slider.
        if (m_wep_cache.id.has_value() && *m_wep_cache.id == 5403) {
            apply_wep_only(rel_pos, rel_rot, "5403");
        }

        // [EGG_5405 ADA] Adas Ei, exakt derselbe Fall. Eigener Key -> Leons
        // 5403-Wert bleibt unberuehrt.
        if (m_wep_cache.id.has_value() && *m_wep_cache.id == 5405) {
            apply_wep_only(rel_pos, rel_rot, "5405");
        }
    }

    glm::vec3 wpos = hand_world + (hand_rot * rel_pos);
    glm::quat wrot = glm::normalize(hand_rot * rel_rot);

    wrot = skullshaker_cock_spin(wrot);   // [SKULL_SHAKER_COCK]
    wrot = knife_flip_spin(wrot);         // [KNIFE_FLIP]

    // [WILDWEST] Twirl + Pivot um den Trigger-Joint (Pos+Rot).
    re4vr::lua_call_transform_hook("__re4_wildwest_apply", wpos, wrot);

    // [KNIFE_FLIP] Nach dem Flip das Messer verschieben, damit der Griff sauber
    // in der Hand sitzt. Offset im HAND-Frame (wie die Basis-Waffenpose) ->
    // konsistente Achsen, kein Welt-Y-Koppeln. Skaliert mit dem Flip-Lerp.
    if (re4vr::lua_get_tribool("__re4_knife_equipped") == 1 && m_knife_flip.lerp > 0.0001f) {
        const auto kid = m_wep_cache.id;
        const bool is_left = re4vr::lua_get_string("__re4_knife_hand") == "left";

        // Rechts: charakter-abhaengige Map (links ist ueber eigene Dateien schon
        // getrennt). Das Original geht dafuer ueber __re4_knife_flip_map_r().
        const char* map = is_left ? "__re4_knife_flip_lh_map"
                                  : (char_now() == "ada" ? "__re4_knife_flip_pos_map_ada"
                                                         : "__re4_knife_flip_pos_map");

        glm::vec3 p{};
        const bool have_p = kid.has_value() && re4vr::lua_get_xyz_at(map, *kid, p);

        // Fallback = alter globaler Wert (Preservation), bis das jeweilige
        // Messer im UI getunt wird. Links hat bewusst keinen Fallback.
        const float px = have_p ? p.x : (is_left ? 0.0f : static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_x", 0.0)));
        const float py = have_p ? p.y : (is_left ? 0.0f : static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_y", 0.0)));
        const float pz = have_p ? p.z : (is_left ? 0.0f : static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_z", 0.0)));

        std::optional<glm::quat> frame_rot;

        if (is_left) {
            frame_rot = m_cache.lh_rot.has_value() ? m_cache.lh_rot : re4vr::lua_get_quat("__vr_lh_rot");
        } else {
            frame_rot = m_cache.rh_rot;
        }

        float ox = px * m_knife_flip.lerp;
        float oy = py * m_knife_flip.lerp;
        float oz = pz * m_knife_flip.lerp;

        // [FLIP_ENTKOPPELT] Der WAFFE-only Messer-Offset (knifewep, oben auf
        // rel_pos) wirkte in BEIDEN Zustaenden, der Flip-Offset nur in einem ->
        // jedes Tuning am einen verschob zwangslaeufig auch den anderen. Genau
        // das war der Bug "die EINEN Slider verfaelschen die ANDEREN".
        // Fix: seinen POSITIONS-Anteil mit dem Flip-Lerp wieder herausrechnen.
        //   lerp = 0 -> nur knifewep-Position
        //   lerp = 1 -> nur Flip-Position
        // Die ROTATION bleibt in beiden gleich -- gewollt.
        // LEON-NEUTRAL: greift nur ueber den knifewep-Key, und der ist
        // ausschliesslich fuer Ada und (seit 17.09.) Krauser gesetzt; fuer Leon
        // ist das exakt ein No-Op.
        if (!is_left && frame_rot.has_value() && kid.has_value() && is_knife_rel_split_id(*kid)) {
            const std::string kkey = knife_wep_key(*kid);
            const auto& ko = get_weapon_offset(kkey);

            // [KNIFE_FLIP_OFFSET 18.09.2026] Krausers Flip-Satz gilt NUR geflippt
            // -- seine Position wird deshalb nicht herausgerechnet, sonst waeren
            // seine Positions-Slider wirkungslos.
            const bool own_flip_set = kkey.ends_with("@krauser_flip") || kkey.ends_with("@krauser_throw_flip");

            if (!own_flip_set && (ko.px != 0.0f || ko.py != 0.0f || ko.pz != 0.0f)) {
                const float l = m_knife_flip.lerp;
                ox -= ko.px * l;
                oy -= ko.py * l;
                oz -= ko.pz * l;
            }
        }

        if ((ox != 0.0f || oy != 0.0f || oz != 0.0f) && frame_rot.has_value()) {
            wpos += (*frame_rot) * glm::vec3{ox, oy, oz};
        }
    }

    // [RECOIL] Liegt seit [RECOIL_ON_HAND] in attach_right_hand auf
    // cache.rh_world/rh_rot, aus denen wpos/wrot hier gebaut werden -> Hand und
    // Waffe kicken gemeinsam. NICHT wieder hier einbauen, sonst doppelt.

    // [MERC_WEP_HOOK] Letzte Station vor dem Schreiben: das Mercenaries-Script
    // darf Pos/Rot einer EIGENEN Waffe komplett ersetzen. In der Kampagne
    // existiert die Funktion nicht -> reiner nil-Check, nichts aendert sich.
    // hand_rot MUSS mitgegeben werden: __vr_rh_rot wird erst am Ende des Ticks
    // publiziert, der Hook rechnete sonst mit der Rotation des VORIGEN Passes
    // und die Waffe wabbelt um die Differenz.
    re4vr::lua_call_merc_wep_apply(wpos, wrot, m_wep_cache.id.value_or(0), hand_rot);

    // [KNIFE_RH_PIN] Messer rechts: an den R_Hand-Joint gehaengt und lokal
    // posiert (wie der linke Klon), damit es beim Stick-Drehen mitdreht.
    // Alles andere -- alle Waffen -- bleibt bei der Weltschreibung.
    if (knife_right_local) {
        knife_pin_apply(m_wep_cache.tf.obj, wpos, wrot);
        re4vr::lua_set_string("__re4_pin_why", "ok");

        return;
    }

    knife_pin_release();

    set_vec4(m_wep_cache.tf.obj, "set_Position", glm::vec4{wpos.x, wpos.y, wpos.z, 1.0f});
    set_quat(m_wep_cache.tf.obj, "set_Rotation", wrot);

    re4vr::lua_set_string("__re4_pin_why", "ok");
}

// ============================================================================
// Killswitch (Lua Z.1807-1837)
// ============================================================================

bool RE4VRMotion::is_killswitch_active() {
    // __vr_motion_paused schreiben reload und reload4_dlc.
    if (re4vr::lua_get_tribool("__vr_motion_paused") == 1) {
        return true;
    }

    // [RAILCAR] Auf dem Schienenwagen laeuft motion TROTZ Killswitch weiter --
    // AUSSER waehrend eines nativen Reloads der Mounted-Gun: dann motion aus,
    // damit die native Nachlade-Anim sauber durchspielt.
    if (re4vr::lua_get_tribool("__re4_railcar_mode") == 1) {
        if (re4vr::lua_get_tribool("__re4_railcar_reloading") != 1) {
            return false;
        }
        // Reload aktiv -> NICHT force-on; faellt durch zu killswitch.is_active.
    }

    // [THROWSIGHT] Del-Lago-Harpunen-Stage: die Kamera bleibt First-Person (KS
    // aus), aber die Motion-Manipulation soll pausieren -- sieht sonst kaputt
    // aus. NUR diese Stage; materials und firstperson bleiben unberuehrt.
    if (re4vr::lua_get_tribool("__re4_throwsight_active") == 1) {
        return true;
    }

    // [GONDEL-ZONE] Kein Killswitch, aber die Motion-Manipulation muss trotzdem
    // pausieren. In Adas Gondel (Stage 60850) faehrt die Kabine sonst nicht los:
    // chainsaw.GmCargoGondola2 prueft ueber checkAdoptCharacterFootLock, ob der
    // aufgenommene Charakter sauber an Bord ist, und updateGondolaTransform gibt
    // einen Boolean zurueck -- meldet der einmal "nicht in Ordnung", bewegt der
    // Verwalter danach KEINE Kabine mehr. Der Killswitch taugt hier nicht, weil
    // er erst mit der Einsteige-Kamerafahrt scharf wird, also NACHDEM die Gondel
    // geprueft hat. Die Stillzone loest frueher aus.
    // Seit dem harten Ausstieg an den vier Einstiegspunkten strenggenommen
    // redundant -- bleibt als Guertel und Hosentraeger. Leon ist nicht betroffen.
    if (re4vr::lua_get_tribool("__re4_stillzone_motion") == 1) {
        return true;
    }

    // Das Killswitch-Modul selbst (re4vr/re4_vr_killswitch.lua).
    return re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false);
}

// ============================================================================
// [NATIVE_ANIM] native_reload_active (Lua Z.1839-1868)
//
// Solange die native Red9-Reload-Anim laeuft, ist dieses Modul komplett aus --
// als waere es disabled. wid und MotionFsm2 jeden Frame frisch aus dem Context
// (robust nach Save/Load). Nur wp4002 und ein Node, der "RELOAD" enthaelt.
// ============================================================================

bool RE4VRMotion::native_reload_active() {
    bool r = false;

    do {
        auto* ctx = re4vr::player_context();

        if (ctx == nullptr) {
            break;
        }

        const auto wid = get_equip_weapon_id();

        if (!wid.has_value() || *wid != 4002) {
            break;
        }

        auto* go = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

        if (go == nullptr) {
            break;
        }

        if (m_nr.go.obj != go) {
            store(m_nr.go, go);
            drop(m_nr.comp);
        }

        if (m_nr.comp.obj == nullptr && m_t_motion_fsm2 != nullptr) {
            store(m_nr.comp,
                  re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_motion_fsm2));
        }

        if (m_nr.comp.obj == nullptr) {
            break;
        }

        for (int layer = 0; layer <= 6; ++layer) {
            auto* n = re4vr::call_safe<::REManagedObject*>(m_nr.comp.obj, "getCurrentNodeName", layer);

            if (n == nullptr) {
                continue;
            }

            std::string name{};

            try {
                name = utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
            } catch (...) {
                name.clear();
            }

            if (!name.empty() && name.find("RELOAD") != std::string::npos) {
                r = true;
                break;
            }
        }
    } while (false);

    // [RED9_RELOAD_AIM] Flag fuer binding.lua: waehrend dieser Anim Aim (LT)
    // forcen, sonst spielt die Engine die Linke-Hand-Reload-Anim nicht (sie
    // haengt am Aim-/Hold-Zustand).
    re4vr::lua_set_bool("__vr_red9_reloading", r);
    return r;
}

// [NATIVE_ANIM] Beim Aussteigen die veroeffentlichten Hand-ZIELE loeschen ->
// exakt der "motion nicht geladen"-Zustand. Dann hat die Arm-IK kein Ziel mehr
// (sie liest genau diese Globals) und laesst die Arme der nativen Anim. Ohne das
// blieben die alten Ziele stehen -> Arme steif.
void RE4VRMotion::release_motion_targets() {
    for (const auto* n : {"__vr_lh_world", "__vr_lh_rot", "__vr_lh_joint_pos",
                          "__vr_lh_joint_rot", "__vr_unified_lh_pos",
                          "__vr_rh_world", "__vr_rh_rot", "__vr_rh_joint_pos",
                          "__vr_rh_joint_rot", "__vr_unified_rh_pos"}) {
        re4vr::lua_set_nil(n);
    }
}

// ============================================================================
// Kamera + VR-Daten (Lua Z.1881-1916)
// ============================================================================

bool RE4VRMotion::get_camera_data(glm::vec3& out_pos, glm::quat& out_rot) {
    auto* cam = sdk::get_primary_camera();

    if (cam == nullptr) {
        return false;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject");

    if (go == nullptr) {
        return false;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return false;
    }

    // get_WorldMatrix ist ein ValueType (via.mat4) -- Position steht in Spalte 3.
    glm::mat4 wm{1.0f};

    if (!get_mat4(cam, "get_WorldMatrix", wm)) {
        return false;
    }

    glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", rot)) {
        return false;
    }

    out_pos = glm::vec3{wm[3].x, wm[3].y, wm[3].z};
    out_rot = rot;

    // Optionale Integration mit re4_vr_firstperson CameraFix. RE4VRFirstPerson
    // steht VOR uns im Mod-Vektor -> wir lesen den frischen Wert.
    if (re4vr::lua_get_table_bool("vr_camera_fix", "active", false)) {
        if (const auto p = re4vr::lua_get_table_vec3("vr_camera_fix", "camera_pos"); p.has_value()) {
            out_pos = *p;
        }

        if (const auto r = re4vr::lua_get_table_quat("vr_camera_fix", "camera_rot"); r.has_value()) {
            out_rot = *r;
        }
    }

    return true;
}

// ============================================================================
// controller_to_world (Lua Z.1922-2008) -- die einheitliche Pose-Pipeline
//
// [TEIL1/K5] Das Original schreibt an vier Stellen `if ok then X = <ergebnis>
// end` OHNE `and`. Ein erfolgreicher pcall mit nil-Rueckgabe setzt ctrl_quat
// bzw. world_rot damit auf NIL -- eine Fehlerklasse, die es in C++ nicht gibt
// (glm::normalize liefert immer einen Wert). Das ist die einzige bewusste
// Abweichung hier und macht den Port an dieser Stelle robuster, nicht anders.
//
// [TEIL1/L6] Der HMD-Fallback dereferenziert in Lua ungeprueft; nativ liefert
// get_position immer einen Wert.
// ============================================================================

bool RE4VRMotion::controller_to_world(const glm::vec3& ctrl_pos_raw,
                                      const glm::mat4* ctrl_rot_raw,
                                      const glm::vec3& cam_pos, const glm::quat& cam_rot,
                                      glm::vec3& out_pos, glm::quat& out_rot) {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    if (!m_cache.standing_origin_set) {
        const auto so = vr->get_standing_origin();
        m_cache.standing_origin = so;
        m_cache.standing_origin_set = true;
    }

    const glm::vec3 origin{m_cache.standing_origin->x, m_cache.standing_origin->y,
                           m_cache.standing_origin->z};
    glm::vec3 ctrl_relative = ctrl_pos_raw - origin;

    const glm::quat rot_off = vr->get_rotation_offset();
    ctrl_relative = rot_off * ctrl_relative;

    out_pos = cam_pos + (cam_rot * ctrl_relative);
    out_rot = cam_rot;

    if (ctrl_rot_raw != nullptr) {
        // Matrix4x4f:to_quat() == glm::quat(m). NICHT die lookAtLH-Falle von
        // Vector3f:to_quat.
        glm::quat ctrl_quat{*ctrl_rot_raw};
        ctrl_quat = glm::normalize(rot_off * ctrl_quat);
        out_rot = glm::normalize(cam_rot * ctrl_quat);

        // OpenXR-Rotationskorrektur (aus RE9, immer gleich fuer OpenXR).
        if (m_vr_runtime == "openxr"
            && (m_openxr_correction.rot_pitch != 0.0f || m_openxr_correction.rot_yaw != 0.0f
                || m_openxr_correction.rot_roll != 0.0f)) {
            out_rot = glm::normalize(out_rot * quat_from_euler_deg(m_openxr_correction.rot_pitch,
                                                                  m_openxr_correction.rot_yaw,
                                                                  m_openxr_correction.rot_roll));
        }

        // Quest/Touch-Rotationskorrektur -- NUR bei metavr; Index/SteamVR ist
        // die Baseline.
        if (m_selected_controller == "metavr"
            && (m_ctrl_correction.rot_pitch != 0.0f || m_ctrl_correction.rot_yaw != 0.0f
                || m_ctrl_correction.rot_roll != 0.0f)) {
            out_rot = glm::normalize(out_rot * quat_from_euler_deg(m_ctrl_correction.rot_pitch,
                                                                  m_ctrl_correction.rot_yaw,
                                                                  m_ctrl_correction.rot_roll));
        }
    }

    // OpenXR-Positionskorrektur: lokal rotiert, danach addiert.
    if (m_vr_runtime == "openxr"
        && (m_openxr_correction.pos_x != 0.0f || m_openxr_correction.pos_y != 0.0f
            || m_openxr_correction.pos_z != 0.0f)) {
        out_pos += out_rot * glm::vec3{m_openxr_correction.pos_x, m_openxr_correction.pos_y,
                                       m_openxr_correction.pos_z};
    }

    if (m_selected_controller == "metavr"
        && (m_ctrl_correction.pos_x != 0.0f || m_ctrl_correction.pos_y != 0.0f
            || m_ctrl_correction.pos_z != 0.0f)) {
        out_pos += out_rot * glm::vec3{m_ctrl_correction.pos_x, m_ctrl_correction.pos_y,
                                       m_ctrl_correction.pos_z};
    }

    return true;
}

// ============================================================================
// Joint-Write / Hand-Offset / Arm-Clamp (Lua Z.2013-2058)
// ============================================================================

void RE4VRMotion::write_joint_pose(::REManagedObject* joint, const glm::vec3& pos,
                                   const glm::quat* rot) {
    if (joint == nullptr) {
        return;
    }

    set_vec4(joint, "set_Position", glm::vec4{pos.x, pos.y, pos.z, 1.0f});

    if (rot != nullptr) {
        set_quat(joint, "set_Rotation", *rot);
    }
}

// Position im HAND-Frame verschieben, Rotation lokal nachdrehen.
void RE4VRMotion::apply_hand_offset(glm::vec3& pos, glm::quat& rot, const WeaponOffset& off) {
    if (off.px != 0.0f || off.py != 0.0f || off.pz != 0.0f) {
        pos += rot * glm::vec3{off.px, off.py, off.pz};
    }

    if (off.rx != 0.0f || off.ry != 0.0f || off.rz != 0.0f) {
        rot = glm::normalize(rot * quat_from_euler_deg(off.rx, off.ry, off.rz));
    }
}

// [HAND_CLAMP] Hand-Pin auf die Arm-Reichweite begrenzen. Die Daten kommen von
// arm_chain: __vr_arm_chain_<side>_root ist der Arm-Ursprung (Schulter),
// _maxreach die Armlaenge plus erlaubtes Schulter-Nachziehen. Liegt der
// Controller weiter weg, wird das HAND-Joint auf den Radius geklemmt -> kein
// Wrist-Stretch, die Hand bleibt am Arm-Ende statt zum Controller gezogen zu
// werden. Kein Eintrag (arm_chain aus oder pausiert) -> unveraendert.
glm::vec3 RE4VRMotion::clamp_hand_to_arm_reach(const glm::vec3& hand_pos, const char* side) {
    // [MINECART] Im railcar AUS: der Cart schiebt Body und Schulter, dadurch
    // reisst der Controller-Abstand oefter ueber die Armlaenge -- der Clamp
    // hielt die Hand/Waffe dann am Arm-Ende zurueck ("haut ab"). Im Minecart
    // soll die Waffe exakt am Controller sitzen.
    if (re4vr::lua_get_tribool("__re4_railcar_mode") == 1) {
        return hand_pos;
    }

    const std::string root_name = std::string{"__vr_arm_chain_"} + side + "_root";
    const std::string maxr_name = std::string{"__vr_arm_chain_"} + side + "_maxreach";

    const auto root = re4vr::lua_get_vec3(root_name.c_str());
    const auto maxr = re4vr::lua_get_number_opt(maxr_name.c_str());

    if (!root.has_value() || !maxr.has_value() || *maxr <= 0.05) {
        return hand_pos;
    }

    const float dx = hand_pos.x - root->x;
    const float dy = hand_pos.y - root->y;
    const float dz = hand_pos.z - root->z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (d <= static_cast<float>(*maxr) || d < 1e-6f) {
        return hand_pos;
    }

    const float k = static_cast<float>(*maxr) / d;
    return glm::vec3{root->x + dx * k, root->y + dy * k, root->z + dz * k};
}

// ============================================================================
// [TWO_HAND_IK] Waffenliste (Lua Z.2064-2079)
//
// Welche Waffen zweihaendig gezielt werden. Pistolen sind einhaendig.
// [TEIL2/F2] Achtung: 6300 (MC XM96E1) steht NICHT hier, 4701 (Flammenwerfer)
// schon -- der Lua-Kommentar darueber ist selbst falsch und die Spec hatte ihn
// uebernommen.
//
// [PISTOLEN RAUS 2026-09-09] 6000 (Sentinel Nine), 6112 (Punisher MC) und 6113
// (Samurai Edge) standen hier, obwohl es Pistolen sind -- schon im Lua-Original
// (re4_vr_motion.lua Z.2072-2074). Beleg fuer das Versehen: 6103 (Blacktail AC)
// ist dieselbe Klasse und fehlte korrekt; in is_support_group_a und
// is_knife_no_support_id stehen 6000/6103/6112/6113 gemeinsam als Pistolen.
// [MAGNUMS RAUS 2026-09-09] 4500 (Broken Butterfly), 4501 (Killer7) und 4502
// (Handcannon) standen ebenfalls hier ("zweihaendig anlegbar" im Lua) -- sie
// sind aber Einhandwaffen und stehen in PISTOL_IDS (RE4VRHolster.cpp). Die
// Zweihand-IK bekommen ab jetzt NUR echte Zweihandwaffen. Die Stuetzhand
// (is_support_group_a) bleibt bei ihnen unveraendert.
// ============================================================================

bool RE4VRMotion::is_two_hand_aim_weapon() const {
    return m_wep_cache.id.has_value() && is_two_hand_aim_weapon_id(*m_wep_cache.id);
}

bool RE4VRMotion::is_two_hand_aim_weapon_id(int32_t wid) {
    switch (wid) {
    case 4100: case 4101: case 4102:                       // Shotguns
    case 4200: case 4201: case 4202:                       // SMGs
    case 4400: case 4401: case 4402:                       // Rifles
    case 4600:                                             // Bolt Thrower
    case 4701:                                             // Flammenwerfer
    case 4900: case 4901: case 4902:                       // Rocket Launcher
    case 6001:                                             // DLC Skull Shaker
    case 6100: case 6101: case 6102: case 6104: case 6105: case 6106:   // SW
    case 6111: case 6114:
        return true;
    default:
        return false;
    }
}

// [TOT -- NICHT PORTIERT] `look_at_rotation` (Lua Z.2082-2118) und
// `quat_conjugate` (Z.2154-2160) haben im gesamten File KEINEN Aufrufer
// (TEIL2/F1 und L4, maschinell bestaetigt). 37 Zeilen Shepperd-Matrix bzw. ein
// Wrapper, die niemand ruft -- sie wandern bewusst NICHT mit.
// Ebenso tot: `deg2rad` (Z.60, zweiter Nachtrag V6).

// ============================================================================
// Grip-/Trigger-Reads (Lua Z.2120-2152)
// ============================================================================

bool RE4VRMotion::grip_held(bool left) {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    // [STALE_HANDLE_FIX] Action-Handle JEDEN Frame frisch holen, nicht cachen.
    // Nach SteamVR-Szenenwechsel, Kapitel-Load oder Save-Load geht ein
    // gecachtes Handle stale -> is_action_active liefert lautlos false -> der
    // Grip ist tot bis "Reset Scripts". Frisch holen heilt sich in-place.
    const auto act = vr->get_action_grip();
    const auto joy = left ? vr->get_left_joystick() : vr->get_right_joystick();

    return vr->is_action_active(act, joy);
}

// [BURST] Linker Trigger = weapon_dial-Action auf dem linken Controller (so
// liest binding.lua ihn; get_action_trigger ist nur der RECHTE Trigger).
// Nur LESEN -> die normale Funktion des linken Triggers bleibt erhalten.
bool RE4VRMotion::is_left_trigger_held() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    return vr->is_action_active(vr->get_action_weapon_dial(), vr->get_left_joystick());
}

// ============================================================================
// rotation_between (Lua Z.2162-2185)
//
// Kuerzeste-Bogen-Rotation, die den Einheitsvektor a auf b dreht. KEIN
// Up-Vektor -> kein Roll-Kippen und kein Flip beim Ueberkreuzen der Haende
// (anders als look-at). a und b muessen normalisiert sein.
// ============================================================================

glm::quat RE4VRMotion::rotation_between(const glm::vec3& a, const glm::vec3& b) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z;

    if (d >= 0.999999f) {
        return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};   // gleich -> Identitaet
    }

    if (d <= -0.999999f) {
        // antiparallel -> 180 Grad um eine Senkrechte
        glm::vec3 ax{1.0f, 0.0f, 0.0f};

        if (std::fabs(a.x) > 0.9f) {
            ax = glm::vec3{0.0f, 1.0f, 0.0f};
        }

        const float cx = a.y * ax.z - a.z * ax.y;
        const float cy = a.z * ax.x - a.x * ax.z;
        const float cz = a.x * ax.y - a.y * ax.x;
        const float cl = std::sqrt(cx * cx + cy * cy + cz * cz);

        if (cl < 1e-6f) {
            return glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        }

        return glm::quat{0.0f, cx / cl, cy / cl, cz / cl};
    }

    const float cx = a.y * b.z - a.z * b.y;
    const float cy = a.z * b.x - a.x * b.z;
    const float cz = a.x * b.y - a.y * b.x;
    const float s = std::sqrt((1.0f + d) * 2.0f);
    const float inv = 1.0f / s;

    return glm::normalize(glm::quat{s * 0.5f, cx * inv, cy * inv, cz * inv});
}

// ============================================================================
// VR-Rohdaten (Lua Z.1908-1916)
// ============================================================================

bool RE4VRMotion::get_vr_data(VrData& out) {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    const auto& controllers = vr->get_controllers();

    if (controllers.size() < 2) {
        return false;
    }

    const auto v3 = [](const Vector4f& v) { return glm::vec3{v.x, v.y, v.z}; };

    out.hmd_pos = v3(vr->get_position(0));
    out.right_pos = v3(vr->get_position(controllers[1]));
    out.right_rot = vr->get_rotation(controllers[1]);
    out.left_pos = v3(vr->get_position(controllers[0]));
    out.left_rot = vr->get_rotation(controllers[0]);
    return true;
}

// ============================================================================
// [TWO_HAND_IK] apply_two_hand_aim (Lua Z.2206-2305) -- Kernstueck
//
// Beide Haende an der Waffe + beide Grips -> die Waffe folgt der Linie rechte
// Hand -> linke Hand. Modifiziert m_cache.rh_rot; Aufruf NACH dem Setzen von
// rh_rot und VOR dem Joint-Write, damit Waffe und RH-Hand gemeinsam drehen.
//
// Das Diagnose-Log th_log (Lua Z.2191-2204) ist im Original dauerhaft AUS
// (_th_log.on = false) und wird deshalb nicht mitportiert.
// ============================================================================

void RE4VRMotion::apply_two_hand_aim(const VrData& vr_data, const glm::vec3& cam_pos,
                                     const glm::quat& cam_rot) {
    // [REL_ENGAGE] Disengaged -> Nullpunkt loeschen, das naechste Greifen
    // baselined neu.
    if (m_two_hand.blend <= 0.0f) {
        m_two_hand._engage_swing0.reset();
    }

    const auto bail = [this]() {
        m_two_hand.blend = 0.0f;
        m_two_hand.active = false;
        m_two_hand._dbg_dist = -1.0f;
    };

    if (!m_two_hand.enabled || !is_two_hand_aim_weapon()) {
        bail();
        return;
    }

    // [SWITCH_DOCK] Am Feuerwahl-Schalter gedockt -> KEINE Two-Hand-IK. Der
    // Switch-Grab ist linker Grip + Hand nah an der Waffe, also exakt die
    // Two-Hand-Bedingung; ohne dieses Gate wuerde die Waffe beim Umstellen
    // mitrotieren.
    if (m_support.switch_docked) {
        bail();
        return;
    }

    // [RACK_FREEZE RAUS] Frueher wurde beim Slide-/Pump-/Bolt-Rack die
    // Waffenrotation eingefroren. Das fuehlte sich bei jeder Waffe steif an --
    // es gibt KEINEN Freeze und KEIN Ausrampen mehr. Der Rack-Zustand wird nur
    // noch gebraucht, um den Blend zu HALTEN (s. [RACK_HOLD]).
    // Die Z-Sperre der Pump-Shotgun sitzt nicht hier, sondern als
    // Positions-Korrektur in attach_right_hand ([PUMP_NO_Z]).
    const bool rack_on = re4vr::lua_get_tribool("__vr_slide_rack_active") == 1;

    // [MAG-DOCK-LOCK] Mag/Shell in der linken Hand (oder <1.2 s nach Insert) ->
    // keine Two-Hand-IK, sonst dreht die Geste die Waffe zur "vollen" Hand.
    // ACHTUNG: Luas `if rawget(...)` ist eine reine Wahrheitspruefung -- JEDER
    // Wert ausser nil/false zaehlt, auch 0 und "".
    if (re4vr::lua_is_truthy("__vr_mag_in_hand")) {
        bail();
        return;
    }

    // [SWITCH-GRIP-LATCH] Striker: der Grip am Drehschalter darf nicht
    // Two-Hand ausloesen (Schalter und Vordergriff liegen zu nah beieinander).
    if (re4vr::lua_is_truthy("__vr_block_two_hand")) {
        bail();
        return;
    }

    // [SKULL_SHAKER_COCK] Cock-Anim laeuft (nur wp6001) -> Two-Hand aus, die
    // Waffe folgt allein der rechten Hand; darauf setzt skullshaker_cock_spin
    // seinen Pitch-Loop. Die linke Hand ist vom Schaft geloest.
    if (re4vr::lua_is_truthy("__vr_pump_anim_active")) {
        bail();
        return;
    }

    if (!m_cache.rh_world.has_value() || !m_cache.rh_rot.has_value()) {
        m_two_hand.active = false;
        return;
    }

    // Rohe linke Controller-Welt -- NICHT die gedockte sichtbare Hand, das
    // waere zirkulaer.
    glm::vec3 lh_raw{};
    glm::quat lh_raw_rot{1.0f, 0.0f, 0.0f, 0.0f};
    const bool have_lh = controller_to_world(vr_data.left_pos, &vr_data.left_rot, cam_pos, cam_rot,
                                             lh_raw, lh_raw_rot);

    float target = 0.0f;

    if (have_lh) {
        const glm::vec3 d = lh_raw - *m_cache.rh_world;
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        m_two_hand._dbg_dist = dist;

        const bool lg = is_left_grip_held();
        const bool rg = is_right_grip_held();

        // [DOCK_PROXIMITY] support.free_near verlangt, dass die freie Hand
        // wirklich am Vordergriff ist -> Aim-Two-Hand dockt nicht mehr ueberall,
        // sondern respektiert den Dock-Dist-Slider. Vorframe-Wert; 1 Frame
        // Latenz beim Engage ist unkritisch.
        if (lg && rg && m_support.free_near
            && dist >= m_two_hand.min_dist && dist <= m_two_hand.max_dist) {
            target = 1.0f;
        }
    }

    // [RACK_HOLD] Waehrend Rack/Pump den Blend HALTEN statt neu zu bewerten:
    // sobald der Slide gegriffen ist, faellt support.free_near weg
    // (SLIDE_RACK-PRIORITY in update_support_dock) -> target waere 0 und die
    // Two-Hand-IK rampte mitten im Zug aus, die Waffe haenge starr an der
    // rechten Hand. Halten ist unkritisch, weil der Schwenk-Ansatz nur die
    // RICHTUNG rechts->links auswertet: ein Zug ENTLANG der Laufachse aendert
    // die kaum.
    if (rack_on) {
        target = m_two_hand.blend;
    }

    // Beim Rack NICHT aktiv melden: die linke Hand gehoert dem Slide, nicht dem
    // Vordergriff.
    m_two_hand.active = (target == 1.0f) && !rack_on;

    if (m_two_hand.blend < target) {
        m_two_hand.blend = std::min(m_two_hand.blend + m_two_hand.blend_speed, target);
    } else if (m_two_hand.blend > target) {
        m_two_hand.blend = std::max(m_two_hand.blend - m_two_hand.blend_speed, target);
    }

    if (m_two_hand.blend <= 0.0f || !have_lh) {
        return;
    }

    // [TWO_HAND_IK] Schwenk-Ansatz, robust gegen Hand-Ueberkreuzen und OHNE
    // Up-Vektor -> kein Roll-Flip: die aktuelle Lauf-Weltrichtung (einhaendig)
    // auf die gewuenschte Richtung (rechte Hand -> linke Hand) drehen, nur die
    // minimale Schwenk-Rotation. Der Roll bleibt der der rechten Hand.
    if (!m_wep_cache.rel_rot.has_value()) {
        return;
    }

    const glm::quat wrot = glm::normalize((*m_cache.rh_rot) * (*m_wep_cache.rel_rot));
    glm::vec3 cur_fwd = wrot * glm::vec3{0.0f, 0.0f, 1.0f};   // Lauf = +Z der Waffe
    const float cl = std::sqrt(cur_fwd.x * cur_fwd.x + cur_fwd.y * cur_fwd.y + cur_fwd.z * cur_fwd.z);

    if (cl < 1e-6f) {
        return;
    }

    cur_fwd /= cl;

    const glm::vec3 df = lh_raw - *m_cache.rh_world;
    const float dl = std::sqrt(df.x * df.x + df.y * df.y + df.z * df.z);

    if (dl < 1e-5f) {
        return;
    }

    const glm::vec3 des_fwd = df / dl;

    // Volle Schwenkung cur_fwd -> des_fwd (Lauf auf die Hand-Linie).
    const glm::quat swing_full = rotation_between(cur_fwd, des_fwd);

    // [REL_ENGAGE] Beim Greifen den Schwenk als Nullpunkt einfrieren, danach nur
    // die AENDERUNG dagegen anwenden. Beim Anlegen ist das die Identitaet -> 0
    // Richtungsaenderung, kein Hochkippen; bewegt sich die Hand-Linie danach,
    // schwenkt es relativ mit und re-alignt sich. Der Nullpunkt wird bei
    // Disengage geloescht -> jedes Greifen beginnt neu.
    if (!m_two_hand._engage_swing0.has_value()) {
        m_two_hand._engage_swing0 = swing_full;
    }

    const glm::quat swing_rel = glm::normalize(swing_full * glm::conjugate(*m_two_hand._engage_swing0));
    const glm::quat swing = glm::slerp(glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, swing_rel, m_two_hand.blend);

    // Welt-Pre-Multiplikation.
    m_cache.rh_rot = glm::normalize(swing * (*m_cache.rh_rot));
}

// ============================================================================
// [KS4_EXIT_FADE] Lua Z.2307-2345
//
// Nach dem Verlassen eines KS4 BEIDE Haende weich von der nativen Engine-Pose
// zum Controller lerpen statt hart zu snappen. Muster 1:1 vom erprobten
// [RELOAD-FADE] im Minecart. Der Killswitch setzt __re4_ks4_exit_t bei der
// KS4-Austritts-Flanke; dur kommt aus dem Slider (0 = hart/aus).
//
// __re4_ks4_exit_t wird BEWUSST nicht auf nil gesetzt: beide Haende lesen es im
// selben Frame, der erste Aufruf braechte den zweiten sonst um den Fade. Der
// Stempel wird bei der naechsten Exit-Flanke ueberschrieben.
// ============================================================================

std::optional<float> RE4VRMotion::ks4_exit_progress() {
    const auto t = re4vr::lua_get_number_opt("__re4_ks4_exit_t");

    if (!t.has_value()) {
        return std::nullopt;
    }

    const double dur = re4vr::lua_get_table_number("__re4_ks4fade", "dur", 0.35);

    if (dur <= 0.001) {
        return std::nullopt;
    }

    const double el = clock_now() - *t;

    if (el < 0.0 || el >= dur) {
        return std::nullopt;
    }

    return static_cast<float>(el / dur);
}

// Lerpt (pos, rot) von der EINMAL eingefrorenen nativen Startpose zum Ziel;
// ausserhalb des Fades unveraendert. joint liefert die Startpose im ersten
// Frame.
void RE4VRMotion::ks4_exit_apply(::REManagedObject* joint, glm::vec3& pos, glm::quat& rot,
                                 bool left) {
    auto& from = left ? m_ks4_from_l : m_ks4_from_r;
    const auto rf = ks4_exit_progress();

    if (!rf.has_value()) {
        from.p.reset();
        from.r.reset();
        return;
    }

    if (!from.p.has_value() && joint != nullptr) {
        glm::vec3 cp{};
        glm::quat cr{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_vec3(joint, "get_Position", cp)) {
            from.p = cp;
        }

        if (get_quat(joint, "get_Rotation", cr)) {
            from.r = cr;
        }
    }

    if (from.p.has_value()) {
        const glm::vec3 fp = *from.p;
        pos = glm::vec3{fp.x + (pos.x - fp.x) * (*rf), fp.y + (pos.y - fp.y) * (*rf),
                        fp.z + (pos.z - fp.z) * (*rf)};
    }

    if (from.r.has_value()) {
        rot = glm::slerp(*from.r, rot, *rf);
    }
}

// ============================================================================
// [RELOAD_LEXIT_FADE] Lua Z.2353-2371
//
// Wie der KS4-Austritt, aber NUR fuer die LINKE Hand und getriggert vom Ende
// des Mag-Push-Docks (reload.lua/reload4_dlc.lua setzen __re4_reload_lexit_t bei
// der fallenden Flanke). Lerpt die linke Hand von der letzten Magazin-Dock-Pose
// weich zum Controller-Ziel, statt hart zu snappen. Die Dauer ist der geteilte
// Slider ms.push.release_dur aus reload_adv. Eigener Store, voellig getrennt vom
// KS4-Fade -- nur die linke Hand ruft das auf, die rechte bleibt roh.
// ============================================================================

void RE4VRMotion::reload_lexit_apply(glm::vec3& pos, glm::quat& rot) {
    const auto t = re4vr::lua_get_number_opt("__re4_reload_lexit_t");

    if (!t.has_value()) {
        return;
    }

    // [WAISE 05.09.2026] Hier stand ein Lua-Lookup auf
    // __re4_reload_mag_slide.push.release_dur. Diese Tabelle hat RE4VRReloadAdv
    // beim Port ersatzlos gestrichen ("KEINE Lua-Tabelle mehr") -- der Lookup
    // lieferte damit IMMER 0.0 und der Block stieg sofort wieder aus: die linke
    // Hand snappte nach dem Magazin-Reindruecken hart zum Controller, statt
    // auszufaden, und der Slider "Snap-Ausfaden s" in reload_adv war tot.
    // Der Wert liegt nativ in RE4VRReloadAdv::push.release_dur.
    double dur = 0.0;

    if (auto& rl = RE4VRReload::get(); rl != nullptr) {
        dur = static_cast<double>(rl->adv().push.release_dur);
    }

    if (dur <= 0.001) {
        return;
    }

    const double el = clock_now() - *t;

    if (el < 0.0 || el >= dur) {
        return;
    }

    if (!m_reload_lexit_from.p.has_value()) {
        return;
    }

    const float rf = static_cast<float>(el / dur);
    const glm::vec3 fp = *m_reload_lexit_from.p;
    pos = glm::vec3{fp.x + (pos.x - fp.x) * rf, fp.y + (pos.y - fp.y) * rf,
                    fp.z + (pos.z - fp.z) * rf};

    if (m_reload_lexit_from.r.has_value()) {
        rot = glm::slerp(*m_reload_lexit_from.r, rot, rf);
    }
}

// ============================================================================
// attach_right_hand (Lua Z.2373-2659)
//
// RE9-Muster: controller_to_world -> Offset -> Smoothing -> Joint-Write.
// ============================================================================

void RE4VRMotion::attach_right_hand(const glm::vec3& cam_pos, const glm::quat& cam_rot,
                                    const VrData& vr_data) {
    if (!m_right_hand.enabled || m_right_hand.joint.obj == nullptr) {
        return;
    }

    glm::vec3 ctrl_pos{};
    glm::quat ctrl_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!controller_to_world(vr_data.right_pos, &vr_data.right_rot, cam_pos, cam_rot,
                             ctrl_pos, ctrl_rot)) {
        return;
    }

    // Rohe Controller-Rotation (pre-Offset) fuer andere Scripte (Aim-Richtung).
    m_cache.rh_aim_rot = ctrl_rot;

    // [ROHE CONTROLLER-POS] Fuer Zonen-Messungen (Holster-Grab) wird die
    // UNVERSCHOBENE Position gebraucht: hand_pos bekommt gleich per-Hand- und
    // per-Waffe-Offsets, links andere als rechts -- dadurch lagen die
    // publizierten Positionen beider Haende unterschiedlich weit vom
    // Holster-Anker, obwohl die Haende physisch am selben Ort waren.
    re4vr::lua_set_vec3("__vr_rh_ctrl_raw", ctrl_pos);

    // Basis-Korrektur = Barehands-Offset (Key "-1", die Engine-ID bei
    // unequipped), gilt IMMER (Controller -> Hand-Bone). Die Waffe sitzt damit
    // von selbst in der Hand.
    glm::vec3 hand_pos = ctrl_pos;
    glm::quat hand_rot = ctrl_rot;
    apply_hand_offset(hand_pos, hand_rot, get_weapon_offset("-1"));

    // Per-Waffe-Offset obendrauf: bewegt Hand PLUS Waffe zusammen (RE9-Logik).
    // AUSNAHME 5403 (Leons Ei) und 5405 (Adas Ei): deren Offset soll NUR das Ei
    // bewegen -- er wird stattdessen in attach_weapon WAFFE-only angewandt.
    // Ohne diese Ausnahme wirkte derselbe Key ZWEIMAL (Hand + Waffe), genau das
    // beobachtete Doppelverhalten.
    if (!m_cache.weapon_key.empty() && m_cache.weapon_key != "-1"
        && m_cache.weapon_key != WEAPON_NONE_KEY
        && m_cache.weapon_key != "5403" && m_cache.weapon_key != "5405") {
        apply_hand_offset(hand_pos, hand_rot, get_weapon_offset(m_cache.weapon_key));
    }

    // Smoothing: Rotation (RE9-Muster) + optional Position (Anti-Zitter).
    if (m_rot_smooth_hands > 0.0f && m_smoothing.right_rot.has_value()) {
        hand_rot = glm::slerp(*m_smoothing.right_rot, hand_rot, 1.0f - m_rot_smooth_hands);
    }

    if (m_pos_smooth_hands > 0.0f && m_smoothing.right_pos.has_value()) {
        const float k = 1.0f - m_pos_smooth_hands;
        const glm::vec3 sp = *m_smoothing.right_pos;
        hand_pos = glm::vec3{sp.x + (hand_pos.x - sp.x) * k, sp.y + (hand_pos.y - sp.y) * k,
                             sp.z + (hand_pos.z - sp.z) * k};
    }

    m_smoothing.right_pos = hand_pos;
    m_smoothing.right_rot = hand_rot;

    // [HAND_CLAMP] Nach dem Smoothing, vor dem Joint-Write.
    hand_pos = clamp_hand_to_arm_reach(hand_pos, "R");

    m_cache.rh_world = hand_pos;
    m_cache.rh_rot = hand_rot;

    // [TWO_HAND_IK] modifiziert m_cache.rh_rot.
    apply_two_hand_aim(vr_data, cam_pos, cam_rot);

    // [WEAPON_GIVE] Der Pumpgriff sitzt ~45 cm vor der rechten Hand und liegt
    // damit oft ausserhalb der linken Armreichweite. Frueher gab die HAND nach
    // (clamp_hand_to_arm_reach) -> sie riss vom Griff ab. Jetzt gibt die WAFFE
    // nach: sie weicht um genau den Reichweiten-Ueberschuss Richtung linke
    // Schulter zurueck, der Griff kommt in Reichweite, die Hand bleibt drauf.
    // NICHT waehrend des Pumpens -- dort hat PUMP_NO_Z allein das Wort, sonst
    // wabbelt die Waffe zum Koerper. Weich ein- und ausgeblendet, sonst ruckt
    // sie im Moment des Reissens.
    {
        const auto pull = re4vr::lua_get_vec3("__re4_grip_pull");
        const bool pumping = re4vr::lua_get_tribool("__vr_shotgun_pump_active") == 1
                             || re4vr::lua_get_tribool("__vr_slide_rack_active") == 1;
        const bool want = pull.has_value() && m_wep_cache.id.has_value()
                          && *m_wep_cache.id == 4100 && !pumping;

        const bool has_give = m_support.give.has_value()
                              && (std::fabs(m_support.give->x) > 1e-5f
                                  || std::fabs(m_support.give->y) > 1e-5f
                                  || std::fabs(m_support.give->z) > 1e-5f);

        if (want || has_give) {
            glm::vec3 g = m_support.give.value_or(glm::vec3{0.0f, 0.0f, 0.0f});
            const glm::vec3 t = want ? *pull : glm::vec3{0.0f, 0.0f, 0.0f};
            constexpr float k = 0.25f;   // Ein-/Ausblendrate pro Aufruf
            g.x += (t.x - g.x) * k;
            g.y += (t.y - g.y) * k;
            g.z += (t.z - g.z) * k;
            m_support.give = g;

            if (m_cache.rh_world.has_value()) {
                m_cache.rh_world = *m_cache.rh_world + g;
                // hand_pos MUSS mit -- sonst wandert nur die Waffe und die
                // rechte Hand bleibt am Controller stehen.
                hand_pos = *m_cache.rh_world;
            }
        }
    }

    // [PUMP_NO_Z] Waehrend eines Slide-Racks darf die Waffe nicht entlang ihrer
    // LAENGSACHSE (waffen-lokales Z) nach hinten zum Koerper wandern; beim Zug
    // zieht die haltende Hand instinktiv mit. X/Y und die komplette Rotation
    // bleiben frei, kein Freeze. Referenz = Waffenposition im ersten Rack-Frame;
    // danach wird jeden Frame nur der Anteil entlang der AKTUELLEN Laengsachse
    // herausgerechnet (Achse live gelesen -> Drehen erzeugt keinen Sprung).
    {
        const bool rack_no_z = re4vr::lua_get_tribool("__vr_shotgun_pump_active") == 1
                               || (re4vr::lua_get_tribool("__vr_slide_rack_active") == 1
                                   && is_two_hand_aim_weapon());

        if (rack_no_z && m_cache.rh_world.has_value() && m_cache.rh_rot.has_value()) {
            glm::quat wq = *m_cache.rh_rot;

            if (m_wep_cache.rel_rot.has_value()) {
                wq = glm::normalize((*m_cache.rh_rot) * (*m_wep_cache.rel_rot));
            }

            const glm::vec3 ax = wq * glm::vec3{0.0f, 0.0f, 1.0f};

            // [PUMP_NO_Z MITWANDERN] Die Referenz war frueher eine feste
            // WELT-Position. Beim Laufen wandert der Spieler weg, der Anker
            // blieb stehen -> die Sperre zog die Waffe auf den alten Punkt
            // zurueck ("Gummiarme beim Rueckwaertslaufen"). Jetzt wird der Anker
            // um die Spielerbewegung seit dem Rack-Start mitverschoben:
            // gesperrt bleibt nur die ZIEHBEWEGUNG DER HAND.
            if (!m_two_hand._pump_ref_pos.has_value()) {
                m_two_hand._pump_ref_pos = *m_cache.rh_world;
                m_two_hand._pump_ref_cam = cam_pos;
            } else {
                const glm::vec3 r0 = *m_two_hand._pump_ref_pos;
                glm::vec3 o{0.0f, 0.0f, 0.0f};

                if (m_two_hand._pump_ref_cam.has_value()) {
                    o = cam_pos - *m_two_hand._pump_ref_cam;
                }

                const glm::vec3 rw = *m_cache.rh_world;
                const float dx = rw.x - (r0.x + o.x);
                const float dy = rw.y - (r0.y + o.y);
                const float dz = rw.z - (r0.z + o.z);
                const float d = dx * ax.x + dy * ax.y + dz * ax.z;

                // [PUMP_NO_X] Zusaetzlich die QUERachse sperren -- aber NUR
                // wp4100 und NUR waehrend sich das Pump-Joint wirklich bewegt
                // (__vr_shotgun_pump_active wird erst ab echtem Zug gesetzt,
                // nicht schon beim Greifen). Y bleibt bewusst frei.
                float exd = 0.0f;
                bool have_sx = false;
                glm::vec3 sx{};

                if (m_wep_cache.id.has_value() && *m_wep_cache.id == 4100
                    && re4vr::lua_get_tribool("__vr_shotgun_pump_active") == 1) {
                    sx = wq * glm::vec3{1.0f, 0.0f, 0.0f};
                    have_sx = true;
                    exd = dx * sx.x + dy * sx.y + dz * sx.z;
                }

                if (d != 0.0f || exd != 0.0f) {
                    glm::vec3 n{rw.x - ax.x * d, rw.y - ax.y * d, rw.z - ax.z * d};

                    if (have_sx && exd != 0.0f) {
                        n -= sx * exd;
                    }

                    m_cache.rh_world = n;
                    hand_pos = n;   // Haltehand bleibt an der Waffe
                }
            }
        } else {
            m_two_hand._pump_ref_pos.reset();   // naechster Pump misst frisch
            m_two_hand._pump_ref_cam.reset();
        }
    }

    // [SNAP_SOFTEN] Nur GROSSE 1-Frame-Spruenge der Waffenrotation sanft
    // einblenden; kleine Aenderungen (normales Zielen) 1:1 durchlassen -> keine
    // Aim-Latenz. Nur Two-Hand-Waffen; Pistolen haben keinen Schwenk.
    if (is_two_hand_aim_weapon() && m_two_hand._smooth_rot.has_value()
        && m_cache.rh_rot.has_value()) {
        const glm::quat a = *m_two_hand._smooth_rot;
        const glm::quat b = *m_cache.rh_rot;
        float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;

        if (dot < 0.0f) {
            dot = -dot;
        }

        // [RACK_RELEASE] Im Release-Fenster IMMER lerpen (auch kleine Spruenge
        // frozen->live), sonst nur grosse.
        const bool releasing = m_two_hand._rack_release > 0;

        if (releasing || dot < TWO_HAND_SNAP_COS) {
            m_cache.rh_rot = glm::slerp(a, b, TWO_HAND_SNAP_EASE);
        }

        if (releasing) {
            --m_two_hand._rack_release;
        }
    }

    m_two_hand._smooth_rot = m_cache.rh_rot;

    // [RECOIL_ON_HAND] Der Recoil sitzt auf der HAND, nicht auf der Waffe. Die
    // Waffe wird aus rh_world/rh_rot abgeleitet (Pin) -> ein Kick HINTER dem Pin
    // bewegte nur die Waffe, die Hand blieb stehen. Hier davor: die Hand kippt,
    // die Waffe erbt den Kick ueber den Pin, Stuetzhand und arm_chain folgen.
    // Drehpunkt ist damit das Handgelenk statt des Waffen-Origins.
    // Die POSITION steht bewusst NACH dem SNAP_SOFTEN-Baseline-Write -- davor
    // saehe die Glaettung den Kick als "grossen Sprung" und daempfte ihn weg.
    if (re4vr::lua_table_is_truthy("vr_recoil", "active") && m_cache.rh_rot.has_value()
        && m_cache.rh_world.has_value()) {
        if (const auto rp = re4vr::lua_get_table_vec3("vr_recoil", "position"); rp.has_value()) {
            glm::quat wq = *m_cache.rh_rot;

            if (m_wep_cache.rel_rot.has_value()) {
                wq = glm::normalize((*m_cache.rh_rot) * (*m_wep_cache.rel_rot));
            }

            m_cache.rh_world = *m_cache.rh_world + (wq * (*rp));
            hand_pos = *m_cache.rh_world;   // hj_pos wird unten daraus gebaut
        }

        if (const auto rr = re4vr::lua_get_table_quat("vr_recoil", "rotation"); rr.has_value()) {
            // rc.rotation ist ein Delta im WAFFEN-Frame. Damit die Waffe exakt
            // dieselbe Drehung bekommt, wird es in den Hand-Frame konjugiert
            // (x = R*d*R^-1 mit R = rel_rot); W' = H*x*R = H*R*d.
            glm::quat d = *rr;

            if (m_wep_cache.rel_rot.has_value()) {
                const glm::quat R = *m_wep_cache.rel_rot;
                d = glm::normalize((R * (*rr)) * glm::conjugate(R));
            }

            m_cache.rh_rot = glm::normalize((*m_cache.rh_rot) * d);
        }
    }

    // [REVOLVER COCK] wp4500/4502: NUR die rechte Hand (Wrist-Roll + Versatz)
    // zum Cocken bewegen, die WAFFE bleibt stehen. Die Waffe wird aus
    // rh_world/rh_rot platziert -> die bleiben UNBERUEHRT; der Offset geht nur
    // in die separate Hand-Joint-Pose. So rollt die Hand um den Griff (der
    // Daumen erreicht den Hahn), ohne das Zielen zu verschieben.
    glm::vec3 hj_pos = hand_pos;
    glm::quat hj_rot = m_cache.rh_rot.value_or(hand_rot);

    {
        const double cf = re4vr::lua_get_number("__vr_rev_cock_frac", 0.0);

        if (cf > 0.0 && m_wep_cache.id.has_value()
            && (*m_wep_cache.id == 4500 || *m_wep_cache.id == 4502)) {
            glm::vec3 op{};
            glm::vec3 orot{};

            if (re4vr::lua_get_cock_off(op, orot)) {
                // [SLERP] Ziel-Rotation einmal bauen, dann per cf slerpen
                // (kuerzester Bogen) -> kein Euler-"Kreis" bei grossen
                // Cock-Hand-Winkeln. Die Endpose bei cf = 1 ist identisch.
                const glm::quat qfull = quat_from_euler_deg(orot.x, orot.y, orot.z);
                const glm::quat rq = glm::slerp(glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, qfull,
                                                static_cast<float>(cf));
                hj_rot = glm::normalize(hj_rot * rq);
                hj_pos += hj_rot * glm::vec3{op.x * static_cast<float>(cf),
                                             op.y * static_cast<float>(cf),
                                             op.z * static_cast<float>(cf)};
            }
        }
    }

    // [RELOAD-FADE] Kart: nach dem nativen Reload Hand und Waffe nicht hart zum
    // Controller snappen, sondern von der erfassten nativen Reload-End-Pose ueber
    // __re4_railcar_reload_fade weich hinlerpen. Absolute Lerp vom EINMAL
    // gecachten "from" -> mehrere Ticks pro Frame konvergieren identisch.
    // Die from-Werte liegen als Globals vor, weil RE4VRMinecart sie loescht.
    {
        const double rf = re4vr::lua_get_number("__re4_railcar_reload_fade", 1.0);

        if (re4vr::lua_get_tribool("__re4_railcar_mode") == 1 && rf < 1.0
            && m_right_hand.joint.obj != nullptr) {
            if (!re4vr::lua_get_vec3("__re4_reload_hand_fp").has_value()) {
                glm::vec3 cp{};
                glm::quat cr{1.0f, 0.0f, 0.0f, 0.0f};

                if (get_vec3(m_right_hand.joint.obj, "get_Position", cp)) {
                    re4vr::lua_set_vec3("__re4_reload_hand_fp", cp);
                }

                if (get_quat(m_right_hand.joint.obj, "get_Rotation", cr)) {
                    re4vr::lua_set_quat("__re4_reload_hand_fr", cr);
                }
            }

            const auto fp = re4vr::lua_get_vec3("__re4_reload_hand_fp");
            const auto fr = re4vr::lua_get_quat("__re4_reload_hand_fr");
            const float rff = static_cast<float>(rf);

            if (fp.has_value()) {
                hj_pos = glm::vec3{fp->x + (hj_pos.x - fp->x) * rff,
                                   fp->y + (hj_pos.y - fp->y) * rff,
                                   fp->z + (hj_pos.z - fp->z) * rff};

                if (m_cache.rh_world.has_value()) {
                    const glm::vec3 w = *m_cache.rh_world;
                    m_cache.rh_world = glm::vec3{fp->x + (w.x - fp->x) * rff,
                                                 fp->y + (w.y - fp->y) * rff,
                                                 fp->z + (w.z - fp->z) * rff};
                }
            }

            if (fr.has_value()) {
                hj_rot = glm::slerp(*fr, hj_rot, rff);

                if (m_cache.rh_rot.has_value()) {
                    m_cache.rh_rot = glm::slerp(*fr, *m_cache.rh_rot, rff);
                }
            }
        } else if (re4vr::lua_get_vec3("__re4_reload_hand_fp").has_value()) {
            re4vr::lua_set_nil("__re4_reload_hand_fp");
            re4vr::lua_set_nil("__re4_reload_hand_fr");
        }
    }

    // [KS4_EXIT_FADE] Die WAFFE haengt NICHT am Joint, sondern an
    // rh_world/rh_rot (attach_weapon liest die spaeter) -> sie MUSS mitgefadet
    // werden, sonst steht die Waffe schon am Controller, waehrend die Hand noch
    // lerpt. Zweiter Aufruf nutzt dieselbe eingefrorene from und dasselbe rf ->
    // Hand und Waffe bewegen sich deckungsgleich.
    ks4_exit_apply(m_right_hand.joint.obj, hj_pos, hj_rot, false);

    if (m_cache.rh_world.has_value() && m_cache.rh_rot.has_value()) {
        glm::vec3 w = *m_cache.rh_world;
        glm::quat r = *m_cache.rh_rot;
        ks4_exit_apply(m_right_hand.joint.obj, w, r, false);
        m_cache.rh_world = w;
        m_cache.rh_rot = r;
    }

    write_joint_pose(m_right_hand.joint.obj, hj_pos, &hj_rot);
    m_cache.rh_joint_pos = hj_pos;
    m_cache.rh_joint_rot = hj_rot;
}

// ============================================================================
// [SUPPORT_HAND] Waffenlisten (Lua Z.2665-2734)
//
// Welche Waffen ueberhaupt eine Stuetzhand haben. ALLE anderen (Eier, Granaten,
// PRL 4702, Boegen 4800/4801, ...) bekommen KEINE.
//
// [TEIL2/F2 -- der Lua-Kommentar ist selbst falsch] 6300 (MC XM96E1) STEHT in
// der Liste, und 4701 (Flammenwerfer) ist Group B, nicht ausgeschlossen. Die
// Spec hatte den falschen Kommentar uebernommen.
// ============================================================================

namespace {
// Group A dockt AUTOMATISCH bei Naehe (kein Grip noetig), Group B nur mit
// gehaltenem Left-Grip.
bool is_support_group_a(int32_t wid) {
    switch (wid) {
    case 4000: case 4001: case 4002: case 4003: case 4004: case 4005:   // Pistolen + Don Quixote
    case 4500: case 4501: case 4502:                                    // Magnums
    case 6000:                                                          // DLC Sentinel Nine
    case 6103: case 6112: case 6113:                                    // SW Pistolen
    case 6300: case 6301:                                               // MC XM96E1 + Handcannon
        return true;
    default:
        return false;
    }
}

// [GRIP_DOCK] Group B -- dockt NUR mit gehaltenem Left-Grip und in Range.
bool is_grip_dock_weapon(int32_t wid) {
    switch (wid) {
    case 4100: case 4101: case 4102:                                    // Shotguns
    case 4200: case 4201: case 4202:                                    // SMGs
    case 4400: case 4401: case 4402:                                    // Rifles
    case 4600:                                                          // Bolt Thrower
    case 4701:                                                          // Flammenwerfer
    case 4900: case 4901: case 4902:                                    // Rocket Launcher
    case 6001:                                                          // DLC Skull Shaker
    case 6100: case 6101: case 6102: case 6104: case 6105: case 6106:   // SW Langwaffen
    case 6111: case 6114:
    case 6304:                                                          // MC Compound Bow
        return true;
    default:
        return false;
    }
}

// [KNIFE_LEFT_NO_SUPPORT] Einhandwaffen (Pistolen/Magnums, ID-Liste = PISTOL_IDS
// aus re4_vr_holster.lua): liegt ein Messer in der LINKEN Hand, gibt es fuer sie
// KEINE Stuetzhand -- der Support-Dock killt sonst den Links-Klon
// ([LH_CLONE SUPPORT-KILL] in weapons2). Zweihandwaffen stehen bewusst NICHT
// hier, dort bleibt alles wie bisher inklusive Klon-Kill.
bool is_knife_no_support_id(int32_t wid) {
    switch (wid) {
    case 4000: case 4001: case 4002: case 4003: case 4004:   // SG-09 R, Punisher, Red9, Blacktail, Matilda
    case 4500: case 4501: case 4502:                          // Broken Butterfly, Killer7, Handcannon
    case 6000: case 6103: case 6112: case 6113:               // Sentinel Nine, Blacktail AC, Punisher MC, Samurai Edge
    case 6300: case 6301:                                     // XM96E1 (MC), Blacktail AC (MC)
        return true;
    default:
        return false;
    }
}

// [PUMP_GRIP] Nur W-870 (4100) und Riot Gun (4101). Die Striker (4102) bleibt
// bewusst draussen -- sie hatte nie Probleme.
// [GRIFF KLEMMT 10.09.2026] Die beiden Shotguns mit Vordergriff: Leons W-870
// und Adas Sawed-off. Solange der Left-Grip gehalten wird, bleibt die Hand am
// Griff -- weder Distanz noch Armreichweite noch der Zweihand-Clamp duerfen sie
// davon wegziehen. Losgelassen wird ueber den Grip, sonst gar nicht.
bool is_grip_locked_shotgun(int32_t wid) {
    return wid == 4100 || wid == 6100;
}

bool is_pump_grip_weapon(int32_t wid) {
    // [6100 WIEDER RAUS 10.09.2026] Adas Sawed-off war kurz mit drin. Gemessen:
    // ihr Griff ist ueber support_offset pos_x/y/z eingestellt (-0.457 /
    // -0.037 / 0.012), die Joint-Werte grip_x/y/z stehen dagegen auf 0. Der
    // Joint-Anker zielte damit auf den Joint-URSPRUNG statt auf den
    // Vorderschaft -> Ziel 15-24 cm neben der Hand, die Distanzregel riss
    // dauernd, die Hand hielt vorne gar nicht mehr.
    // Erst wenn grip_x/y/z fuer 6100 eingestellt sind (Slider im Dev-Tree
    // "RE4VR - Motion"), darf sie hier wieder dazu.
    return wid == 4100 || wid == 4101;
}
} // namespace

// Messer in der linken Hand? Gleiche Abfrage wie an den bestehenden Stellen
// (equipped ODER Klon).
bool RE4VRMotion::knife_is_left() {
    return re4vr::lua_get_string("__re4_knife_hand") == "left"
           || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
}

bool RE4VRMotion::is_support_hand_weapon() const {
    if (!m_wep_cache.id.has_value()) {
        return false;
    }

    const int32_t wid = *m_wep_cache.id;

    if (!is_support_group_a(wid) && !is_grip_dock_weapon(wid)) {
        return false;
    }

    // [KNIFE_LEFT_NO_SUPPORT] Pistole/Magnum + Messer links -> gesperrt.
    if (is_knife_no_support_id(wid) && knife_is_left()) {
        return false;
    }

    return true;
}

// [PUMP_GRIP] Bei W-870 und Riot Gun ist der Vordergriff das bewegliche
// Slide-Teil. WELCHES Joint sagt re4_vr_reload.lua selbst
// (__re4_rack_joint_name aus dessen JOINTS-Tabelle, dort slide = "_01") -> es
// gibt keinen zweiten Ort, an dem der Name gepflegt werden muesste. Nur der NAME
// wandert; das Joint wird live geholt, denn ein Joint-Objekt ueberlebt keinen
// Savegame-Load.
::REManagedObject* RE4VRMotion::get_pump_joint() {
    if (m_wep_cache.tf.obj == nullptr || !m_wep_cache.id.has_value()) {
        return nullptr;
    }

    if (!is_pump_grip_weapon(*m_wep_cache.id)) {
        return nullptr;
    }

    const std::string want = re4vr::lua_get_string("__re4_rack_joint_name");

    if (want.empty()) {
        return nullptr;
    }

    // Der Name gehoert zu einer anderen Waffe?
    if (static_cast<int32_t>(re4vr::lua_get_number("__re4_rack_joint_wid", -1.0)) != *m_wep_cache.id) {
        return nullptr;
    }

    auto* j = joint_by_name(m_wep_cache.tf.obj, want.c_str());

    if (j != nullptr) {
        m_support.pump_jn = want;
    }

    return j;
}

// ============================================================================
// [BURST] Feuerwahl-Schalter-Joint (Lua Z.4368-4388)
//
// Der Name wird gecacht, das JOINT immer live geholt -- ein Joint-Objekt
// ueberlebt keinen Savegame-Load. (Das ist die Stelle, die TEIL3s Checkliste
// mit "nur Joint-NAMEN werden gecacht" meint; fuer alle anderen Handles gilt
// das ausdruecklich NICHT.)
// ============================================================================

::REManagedObject* RE4VRMotion::get_switch_joint() {
    if (m_wep_cache.tf.obj == nullptr || !m_wep_cache.id.has_value()) {
        return nullptr;
    }

    const int32_t wid = *m_wep_cache.id;
    std::vector<const char*> cands;

    if (wid == WID_LE5) {
        cands = {"_05", "joint_05"};
    } else if (wid == WID_SWEEPER) {
        cands = {"_06", "joint_06"};
    } else {
        return nullptr;
    }

    if (!m_switch_joint_name.empty() && m_switch_joint_wid == wid) {
        if (auto* j = joint_by_name(m_wep_cache.tf.obj, m_switch_joint_name.c_str()); j != nullptr) {
            return j;
        }
    }

    m_switch_joint_name.clear();

    for (const auto* nm : cands) {
        if (auto* j = joint_by_name(m_wep_cache.tf.obj, nm); j != nullptr) {
            m_switch_joint_name = nm;
            m_switch_joint_wid = wid;
            return j;
        }
    }

    return nullptr;
}

// ============================================================================
// get_support_pose (Lua Z.2763-3064)
//
// Dock-Pose = RH-Pose + Offset. Idle-Offset, optional gegen ein Aim-Offset
// geblendet (nur wenn fuer die Waffe angelegt) -> die Stuetzhand kippt beim Aim
// mit der Waffe mit.
//
// [TEIL2/L3] NICHT seiteneffektfrei: schreibt __re4_grip_pull und laeuft
// DREIMAL pro Frame.
// ============================================================================

bool RE4VRMotion::get_support_pose(glm::vec3& out_pos, glm::quat& out_rot) {
    if (!m_cache.rh_world.has_value() || !m_cache.rh_rot.has_value()) {
        return false;
    }

    const glm::vec3 rh_world = *m_cache.rh_world;
    const glm::quat rh_rot = *m_cache.rh_rot;

    // Offset-Paar idle->aim per aim_blend aufloesen.
    const auto resolve = [this](float& px, float& py, float& pz, float& pitch, float& yaw,
                                float& roll, const SupportOffsetAim* aimoff) {
        if (aimoff != nullptr && m_support.aim_blend > 0.001f) {
            const float b = m_support.aim_blend;
            px += (aimoff->pos_x - px) * b;
            py += (aimoff->pos_y - py) * b;
            pz += (aimoff->pos_z - pz) * b;
            pitch += (aimoff->rot_pitch - pitch) * b;
            yaw += (aimoff->rot_yaw - yaw) * b;
            roll += (aimoff->rot_roll - roll) * b;
        }
    };

    // [SWITCH_DOCK] Am Schalter gedockt -> Switch-Offset statt Vordergriff.
    // Auch waehrend der Dock noch AUSBLENDET (switch_blend_lock) -> kein
    // Schaft-Flackern beim Loslassen.
    if (m_support.switch_docked
        || (m_support.switch_blend_lock && m_support.blend_factor > 0.0f)) {
        if (auto* sw = get_support_offset_switch(m_cache.weapon_key); sw != nullptr) {
            float px = sw->pos_x;
            float py = sw->pos_y;
            float pz = sw->pos_z;
            float pitch = sw->rot_pitch;
            float yaw = sw->rot_yaw;
            float roll = sw->rot_roll;
            resolve(px, py, pz, pitch, yaw, roll, get_support_offset_switch_aim(m_cache.weapon_key));

            // [SWITCH_DOCK2] Zweites Paar fuer die Single/Burst-Hebelstellung
            // (nur wenn angelegt): per Hebel-Fortschritt zwischen den beiden
            // handgesetzten Posen blenden. Dann KEIN 1:1-Pivot-Follow, das waere
            // doppelt.
            auto* sw2 = get_support_offset_switch2(m_cache.weapon_key);
            const bool use_endpoints = sw2 != nullptr;

            if (use_endpoints) {
                float bx = sw2->pos_x;
                float by = sw2->pos_y;
                float bz = sw2->pos_z;
                float bpitch = sw2->rot_pitch;
                float byaw = sw2->rot_yaw;
                float broll = sw2->rot_roll;

                if (auto* a2 = get_support_offset_switch2_aim(m_cache.weapon_key);
                    a2 != nullptr && m_support.aim_blend > 0.001f) {
                    const float b = m_support.aim_blend;
                    bx += (a2->pos_x - bx) * b;
                    by += (a2->pos_y - by) * b;
                    bz += (a2->pos_z - bz) * b;
                    bpitch += (a2->rot_pitch - bpitch) * b;
                    byaw += (a2->rot_yaw - byaw) * b;
                    broll += (a2->rot_roll - broll) * b;
                }

                // Eigener geglaetteter Blend, entkoppelt von der Hebel-Anim.
                float t = m_support.switch2_blend;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                px += (bx - px) * t;
                py += (by - py) * t;
                pz += (bz - pz) * t;
                pitch += (bpitch - pitch) * t;
                yaw += (byaw - yaw) * t;
                roll += (broll - roll) * t;
            }

            glm::vec3 pos = rh_world + (rh_rot * glm::vec3{px, py, pz});
            glm::quat rot = rh_rot;

            if (pitch != 0.0f || yaw != 0.0f || roll != 0.0f) {
                rot = glm::normalize(rh_rot * quat_from_euler_deg(pitch, yaw, roll));
            }

            // [BURST] Hand folgt dem Hebel 1:1 (ein Offset-Paar, kein separates
            // Burst-Offset): die fertige Hand-Pose um den Joint-WELT-Pivot um
            // burst_anim drehen, Achse = Joint-Welt-X. NUR wenn KEIN zweites
            // Paar angelegt ist, sonst handgesetzte Endpunkte -> doppelt.
            if (!use_endpoints && std::fabs(m_support.burst_anim) > 0.05f) {
                if (auto* j = get_switch_joint(); j != nullptr) {
                    glm::vec3 pivot{};
                    glm::quat jr{1.0f, 0.0f, 0.0f, 0.0f};

                    if (get_vec3(j, "get_Position", pivot) && get_quat(j, "get_Rotation", jr)) {
                        const glm::mat4 jm = glm::mat4_cast(jr);
                        glm::vec3 ax{jm[0].x, jm[0].y, jm[0].z};
                        const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);

                        if (al >= 1e-5f) {
                            ax /= al;
                            const float hh = glm::radians(m_support.burst_anim) * 0.5f;
                            const float s = std::sin(hh);
                            const glm::quat rq{std::cos(hh), ax.x * s, ax.y * s, ax.z * s};
                            pos = pivot + (rq * (pos - pivot));
                            rot = glm::normalize(rq * rot);
                        }
                    }
                }
            }

            out_pos = pos;
            out_rot = rot;
            return true;
        }
    }

    const auto& a = get_support_offset(m_cache.weapon_key);
    float pos_x = a.pos_x;
    float pos_y = a.pos_y;
    float pos_z = a.pos_z;
    float pitch = a.rot_pitch;
    float yaw = a.rot_yaw;
    float roll = a.rot_roll;
    resolve(pos_x, pos_y, pos_z, pitch, yaw, roll, get_support_offset_aim(m_cache.weapon_key));

    // [PUMP_GRIP] Vordergriff AM PUMP-JOINT verankern statt an der rechten Hand.
    // Nur fuer die Pump-Waffen und nur mit grip_on -- schlaegt der Joint-Lookup
    // fehl, faellt es lautlos auf den normalen Pfad zurueck (nie schlechter als
    // vorher). Bewusst NUR die Position: die Rotation bleibt an der rechten
    // Hand, damit die eingestellte Handhaltung unveraendert bleibt.
    if (m_wep_cache.id.has_value() && is_pump_grip_weapon(*m_wep_cache.id) && a.grip_on != false) {
        auto* pj = get_pump_joint();

        // [GRIP_ANCHOR_RH] Anker gehoert zu EINER Waffe.
        if (m_support.grip_off_wid != m_wep_cache.id) {
            m_support.grip_off.reset();
            m_support.grip_rhprev.reset();
            m_support.grip_off_wid = m_wep_cache.id;
        }

        if (pj != nullptr) {
            glm::vec3 jp{};
            glm::quat jr{1.0f, 0.0f, 0.0f, 0.0f};

            if (get_vec3(pj, "get_Position", jp) && get_quat(pj, "get_Rotation", jr)) {
                // [GRIP_SLIDE] Z ist die Laengsachse im Joint-Frame (dieselbe,
                // auf der reload den Pump-Zug misst). Ist ein Gleitbereich
                // eingestellt, wird die Hand nicht auf grip_z festgenagelt,
                // sondern folgt dem Controller ENTLANG dieser Achse, begrenzt
                // auf [grip_z - grip_back, grip_z + grip_fwd]. X/Y bleiben fest,
                // die Hand bleibt also immer auf dem Rohr. Beide 0 -> exakt das
                // bisherige Verhalten.
                float gz = a.grip_z;

                if (a.grip_back > 0.0f || a.grip_fwd > 0.0f) {
                    // roh, im selben Frame vorher gesetzt
                    if (const auto lc = re4vr::lua_get_vec3("__vr_lh_ctrl_world"); lc.has_value()) {
                        const glm::vec3 rl = glm::conjugate(jr) * (*lc - jp);
                        const float lo = a.grip_z - a.grip_back;
                        const float hi = a.grip_z + a.grip_fwd;
                        gz = rl.z < lo ? lo : (rl.z > hi ? hi : rl.z);
                    }
                }

                const glm::vec3 gv{a.grip_x, a.grip_y, gz};

                // [GRIP_ANCHOR_RH] Zwei GEGENLAEUFIGE Fehler:
                // (1) Beim LAUFEN friert die WELT-Position des Pump-Joints ein,
                //     waehrend die rechte Hand die Waffe 1:1 weitertraegt.
                // (2) Beim ROTIEREN im Aim ist ein starrer rh-Anker falsch:
                //     gemessen sprang der Griffpunkt bis 15 cm in 10 ms, weil
                //     ein 45-cm-Hebel bei 20 Grad genau diesen Kreisbogen
                //     beschreibt.
                // Loesung: nur die POSITION ueber die rechte Hand stabilisieren,
                // die RICHTUNG live vom Joint nehmen.
                // [ZURUECK AUF EINFACH] Die reine Joint-Bindung IST das
                // Parenting und funktionierte. Die rh-Stabilisierung ist nur ein
                // optionaler Zusatz -- AUS, bis einzeln nachgewiesen ist, dass
                // sie mehr hilft als schadet.
                glm::vec3 jps = jp;

                if (a.grip_anchor) {
                    const float mv = m_support.grip_rhprev.has_value()
                                         ? glm::length(rh_world - *m_support.grip_rhprev)
                                         : 999.0f;
                    m_support.grip_rhprev = rh_world;

                    if (!m_support.grip_off.has_value() || mv < 0.006f) {   // Schwelle wie [LDOCK_RH]
                        m_support.grip_off = glm::inverse(rh_rot) * (jp - rh_world);
                    }

                    if (m_support.grip_off.has_value()) {
                        jps = rh_world + (rh_rot * (*m_support.grip_off));
                    }
                }

                // Griff-Feinversatz mit der LIVE Joint-Rotation, nicht mit der
                // rechten Hand: dreht sich die Waffe, dreht der Griff korrekt
                // mit dem Rohr, statt auf einem Kreisbogen um die Faust zu
                // schleudern.
                //
                // [GRIP_NO_ROLL] Gemessen: die Hand steht still (0,0-0,1 cm),
                // der Griffpunkt springt bis 13,7 cm pro Frame. Reine Geometrie:
                // grip_x/grip_y setzen den Griff 6,7 cm neben die Rohrachse, ein
                // ROLL zieht diesen Versatz auf einem Kreis mit 13,4 cm
                // Durchmesser herum -- exakt der gemessene Wert. Fix: den Twist
                // um die Rohr-Laengsachse (lokales Z) entfernen, bevor der
                // Versatz angewandt wird. Nur auf ausdruecklichen Wunsch.
                glm::quat jrg = jr;

                if (a.grip_noroll) {
                    const float n = std::sqrt(jr.w * jr.w + jr.z * jr.z);

                    if (n >= 1e-6f) {   // 180 Grad um X/Y: kein Twist bestimmbar
                        const glm::quat tw{jr.w / n, 0.0f, 0.0f, jr.z / n};
                        jrg = glm::normalize(jr * glm::conjugate(tw));
                    }
                }

                const glm::vec3 gp = jps + (jrg * gv);

                // [Z_CLAMP] Der Pumpgriff sitzt fest 13,3 cm vor der rechten
                // Hand; ob die linke Hand drankommt, entscheidet allein der
                // Abstand zur linken Schulter. Ab der Grenze zog frueher
                // clamp_hand_to_arm_reach die HAND zurueck -> sie riss vom Griff.
                // Jetzt wird stattdessen die WAFFE entlang ihrer LAENGSACHSE
                // zurueckgehalten, bis der Griff wieder auf der
                // Reichweitenkugel liegt -- Hand und Waffe stoppen gemeinsam.
                // NUR wp4100; die Riot Gun bleibt vorerst aussen vor.
                if (m_wep_cache.id.has_value() && *m_wep_cache.id == 4100 && m_two_hand.active) {
                    const auto lr = re4vr::lua_get_vec3("__vr_arm_chain_L_root");
                    auto lm = re4vr::lua_get_number_opt("__vr_arm_chain_L_maxreach");

                    // [ACHSEN-FIX] rh_rot ist die HAND-Rotation. Die Laengsachse
                    // der WAFFE ist rh_rot * rel_rot -- genau so bildet
                    // PUMP_NO_Z sie. Mit der Handachse zeigte die Ruecknahme
                    // teils nach vorn statt zum Koerper.
                    glm::quat wq = rh_rot;

                    if (m_wep_cache.rel_rot.has_value()) {
                        wq = glm::normalize(rh_rot * (*m_wep_cache.rel_rot));
                    }

                    const glm::vec3 ax = wq * glm::vec3{0.0f, 0.0f, 1.0f};

                    // [IK_RESERVE] Gemessen: der Griffpunkt selbst ist ruhig --
                    // das Zittern entsteht in der Arm-Kette. Der Clamp haelt den
                    // Griff exakt auf der Reichweitenkugel, der Arm steht damit
                    // auf 100 % Streckung, und dort ist Zwei-Knochen-IK am
                    // instabilsten. Kleine Reserve = Arm nie ganz durchgestreckt.
                    if (lm.has_value()) {
                        lm = *lm - 0.015;
                    }

                    // [1:1] Lua nullt __re4_grip_pull NUR in den inneren
                    // Zweigen. Fehlen die arm_chain-Anker oder ist die Achse
                    // entartet, bleibt der ALTE Wert stehen -- faellt arm_chain
                    // kurz aus, haelt das Original die Waffe auf der zuletzt
                    // berechneten Ruecknahme, statt sie nach vorn laufen zu
                    // lassen. Deshalb hier kein pauschales Aufraeumen.
                    if (lr.has_value() && lm.has_value() && *lm > 0.05) {
                        const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);

                        if (al > 1e-6f) {
                            const glm::vec3 n = ax / al;

                            // [SCHLEIFE GEBROCHEN] Gemessen werden MUSS die Lage
                            // OHNE die eigene Korrektur. Sonst misst der Clamp
                            // seinen eigenen Effekt: gp kommt aus dem Joint, der
                            // Joint sitzt an der bereits zurueckgezogenen Waffe
                            // -> Ueberschuss schrumpft -> Clamp laesst nach ->
                            // Waffe vor -> Ueberschuss waechst. Dieses Pendeln
                            // dreht ueber die Two-Hand-IK die Waffe mit = das
                            // Kippen.
                            const glm::vec3 sg = m_support.give.value_or(glm::vec3{0.0f, 0.0f, 0.0f});
                            const glm::vec3 m = gp - sg;
                            const glm::vec3 e = m - *lr;
                            const float ee = e.x * e.x + e.y * e.y + e.z * e.z;
                            const float lmf = static_cast<float>(*lm);

                            if (ee > lmf * lmf) {   // ausserhalb der Reichweite
                                const float ea = e.x * n.x + e.y * n.y + e.z * n.z;
                                const float disc = ea * ea - ee + lmf * lmf;

                                if (disc >= 0.0f) {
                                    float t = ea - std::sqrt(disc);   // kleinste Ruecknahme

                                    if (t > 0.0f) {
                                        if (t > 0.5f) {
                                            t = 0.5f;   // Sicherheitsdeckel
                                        }

                                        // Nur veroeffentlichen; gp bleibt
                                        // unangetastet, sonst wirkt die
                                        // Ruecknahme doppelt.
                                        re4vr::lua_set_vec3("__re4_grip_pull", glm::vec3{-n.x * t, -n.y * t, -n.z * t});
                                    } else {
                                        re4vr::lua_set_nil("__re4_grip_pull");
                                    }
                                } else {
                                    re4vr::lua_set_nil("__re4_grip_pull");   // Achse verfehlt die Kugel
                                }
                            } else {
                                re4vr::lua_set_nil("__re4_grip_pull");
                            }
                        }
                    }
                } else {
                    // [TEIL2/L2] Ausserhalb dieses Zweigs wird __re4_grip_pull
                    // NIE aufgeraeumt -- bei jeder anderen Waffe bleibt der
                    // letzte Wert stale stehen. Dass das folgenlos ist, liegt
                    // allein am zusaetzlichen id == 4100-Gate beim Konsumenten
                    // in attach_right_hand. 1:1 uebernommen.
                    re4vr::lua_set_nil("__re4_grip_pull");
                }

                glm::quat grot = rh_rot;

                if (pitch != 0.0f || yaw != 0.0f || roll != 0.0f) {
                    grot = glm::normalize(rh_rot * quat_from_euler_deg(pitch, yaw, roll));
                }

                out_pos = gp;
                out_rot = grot;
                return true;
            }
        }
    }

    out_pos = rh_world + (rh_rot * glm::vec3{pos_x, pos_y, pos_z});
    out_rot = rh_rot;

    if (pitch != 0.0f || yaw != 0.0f || roll != 0.0f) {
        out_rot = glm::normalize(rh_rot * quat_from_euler_deg(pitch, yaw, roll));
    }

    return true;
}

// [BURST] LE5-Schalter-Umleg-Sound (Wwise-Trigger auf dem Weapon-SoundContainer).
void RE4VRMotion::play_le5_switch_sound() {
    constexpr int32_t LE5_SWITCH_SOUND_ID = 812850326;

    if (m_wep_cache.go.obj == nullptr || m_t_snd == nullptr) {
        return;
    }

    auto* scn = re4vr::call_safe<::REManagedObject*>(m_wep_cache.go.obj, "getComponent(System.Type)",
                                                     m_t_snd);

    if (scn != nullptr) {
        re4vr::call_safe<void*>(scn, "trigger(System.UInt32)",
                                static_cast<uint32_t>(LE5_SWITCH_SOUND_ID));
    }
}

// ============================================================================
// update_support_dock (Lua Z.3077-3371)
//
// Dock/Undock-Entscheid + Blend-Fortschritt. Laeuft NUR im LockScene-Pass.
// [TEIL2/F4] SECHS explizite Returns plus der Durchfall ans Funktionsende.
// ============================================================================

// support_pos == nullptr entspricht Luas nil. Der Aufruf erfolgt TROTZDEM --
// die Nil-Pruefung sitzt im Original erst in `allowed`, also NACH Aim-Ramp,
// Switch2-Ramp, dem Loeschen von switch_blend_lock und dem SLIDE_RACK-Block.
void RE4VRMotion::update_support_dock(const glm::vec3& free_pos, const glm::vec3* support_pos) {
    // [HARTER BREAK 10.09.2026] Loslassen und Wegziehen sind ein BRUCH, kein
    // Ausblenden: blend_factor sofort auf 0, damit die Hand im selben Frame auf
    // der Controller-Pose sitzt. Das weiche ramp_out darunter bleibt fuer alle
    // anderen Faelle (Rack, Mag in der Hand, Waffenwechsel) unveraendert -- dort
    // ist der Uebergang gewollt.
    //
    // Nebenwirkung mit Absicht: __vr_support_hand_active haengt an `docked`
    // (Z.4688) -- mit dem harten Break faellt also auch die Greifpose der Finger
    // im selben Frame weg, statt in der Luft stehen zu bleiben.
    const auto break_now = [this]() {
        m_support.docked = false;
        m_support.target_blend = 0.0f;
        m_support.blend_factor = 0.0f;
        m_support.dock_wid.reset();
        m_support.hold_t0 = 0.0;
    };

    // Gemeinsames Ausblenden -- im Original an fuenf Stellen ausgeschrieben.
    const auto ramp_out = [this]() {
        if (m_support.docked || m_support.blend_factor > 0.0f) {
            m_support.docked = false;
            m_support.target_blend = 0.0f;

            if (m_support.blend_factor > 0.0f) {
                m_support.blend_factor = std::max(m_support.blend_factor - m_support.blend_speed, 0.0f);
            }
        }
    };

    // [SUPPORT_AIM] Aim-Dock-Blend rampen. __vr_aim_input fuehrt dem
    // Kamera-Sprung voraus.
    {
        const bool aiming = re4vr::lua_get_tribool("__vr_aim_input") == 1 || m_support.aim_preview
                            || m_support.switch_aim_preview || m_support.switch2_aim_preview;
        const float tgt = aiming ? 1.0f : 0.0f;
        float sp_in = 0.18f;
        float sp_out = 0.18f;

        if (m_support.switch_docked) {
            if (const auto* swa = get_support_offset_switch_aim(m_cache.weapon_key); swa != nullptr) {
                sp_in = swa->blend_in;
                sp_out = swa->blend_out;
            }
        } else {
            if (const auto* aimcfg = get_support_offset_aim(m_cache.weapon_key); aimcfg != nullptr) {
                sp_in = aimcfg->blend_in;
                sp_out = aimcfg->blend_out;
            }
        }

        if (m_support.aim_blend < tgt) {
            m_support.aim_blend = std::min(m_support.aim_blend + sp_in, tgt);
        } else if (m_support.aim_blend > tgt) {
            m_support.aim_blend = std::max(m_support.aim_blend - sp_out, tgt);
        }
    }

    // [SWITCH_DOCK2] Pose1<->Pose2 mit EIGENEM Lerp rampen, entkoppelt von der
    // Hebel-Anim. Ziel 1 in Single/Burst-Stellung, sonst 0. Gilt no-aim + aim.
    {
        const float b_tgt = (m_support.burst_active || m_support.burst_preview
                             || m_support.switch2_preview || m_support.switch2_aim_preview)
                                ? 1.0f
                                : 0.0f;
        const auto* sw2 = get_support_offset_switch2(m_cache.weapon_key);
        const float lsp = sw2 != nullptr ? sw2->lerp : 0.15f;

        if (m_support.switch2_blend < b_tgt) {
            m_support.switch2_blend = std::min(m_support.switch2_blend + lsp, b_tgt);
        } else if (m_support.switch2_blend > b_tgt) {
            m_support.switch2_blend = std::max(m_support.switch2_blend - lsp, b_tgt);
        }
    }

    // [SWITCH_DOCK] Pose-Latch loeschen, sobald der Dock vollstaendig
    // ausgeblendet ist. Solange er > 0 ist, behaelt get_support_pose die
    // Switch-Pose -- kein Schaft-Flackern.
    if (m_support.blend_factor <= 0.0f) {
        m_support.switch_blend_lock = false;
    }

    // [SLIDE_RACK PRIORITY] Der Slide-Rack hat VORRANG vor allem hier: solange
    // ein Rack noetig ist oder die Hand an den Slide gezogen wird, docken
    // Support UND Switch NICHT -- die linke Hand gehoert dem Rack.
    const bool needs_rack = re4vr::lua_get_tribool("__vr_needs_rack") == 1;
    const bool blk_2h = re4vr::lua_get_tribool("__vr_block_two_hand") == 1;
    const bool pump_anim = re4vr::lua_get_tribool("__vr_pump_anim_active") == 1;
    const bool slide_dock = re4vr::lua_get_number("__vr_slide_dock_blend_factor", 0.0) > 0.001;

    if (needs_rack || blk_2h || pump_anim || slide_dock) {
        m_support.switch_docked = false;
        m_support.switch_latched = false;
        m_support.prev_left_grip = false;
        m_support.burst_neutral_fy.reset();
        m_support.free_near = false;   // [DOCK_PROXIMITY] kein Two-Hand-Engage im Rack

        // [PUMP_SEAMLESS] Ist das Slide-Dock der EINZIGE Grund, die Support-Hand
        // NICHT loslassen: blend_factor OBEN halten -> die Hand bleibt am
        // Vordergriff, das Slide-Dock reitet obendrauf und zieht 1:1 mit dem
        // Pump. Sonst rampt blend_factor runter, die Basis faellt auf die rohe
        // Controller-Hand und die Hand hebt fuer 1-2 Frames vom Griff ab -- bei
        // Engage UND Release.
        if (slide_dock && !(needs_rack || blk_2h || pump_anim)) {
            // [KEIN PING-PONG 10.09.2026] Hier stand `docked = false` als
            // Clamp-Bypass. Der Bypass laeuft inzwischen ueber blend_factor
            // (s. Clamp-Zeile in attach_left_hand), also ist das Nullen nicht
            // mehr noetig -- und es war schaedlich: der Slide-Dock schaltet
            // waehrend des Pumpens im Frametakt an und aus, "der Grip haelt den
            // Dock" setzte sofort wieder true -> gemessen 42 Dock-Wechsel, viele
            // im Abstand von 0,2 s, und der Blend kam nie ueber 0,55-0,89
            // hinaus. Das war das Flackern zwischen zwei Zustaenden.
            //
            // Solange der Grip gehalten wird, bleibt der Dock deshalb stehen und
            // der Blend oben. Ohne Grip gilt das Alte.
            if (m_support.docked && is_left_grip_held()) {
                m_support.target_blend = 1.0f;

                if (m_support.blend_factor < 1.0f) {
                    m_support.blend_factor = std::min(m_support.blend_factor + m_support.blend_speed, 1.0f);
                }
            } else {
                m_support.docked = false;
            }
        } else {
            ramp_out();
        }

        return;   // (1)
    }

    // [1:1] support_pos gehoert MIT in die Pruefung -- fehlt die Dock-Pose,
    // faellt das Original hier auf ramp_out() durch, statt den Dock-Zustand
    // einzufrieren. Genau das passiert bei JEDEM Waffenwechsel und im
    // Kalibrierfenster, weil dort attach_right_hand nicht laeuft und rh_world
    // damit leer ist.
    const bool allowed = m_support.enabled && is_support_hand_weapon()
                         && m_cache.rh_world.has_value() && support_pos != nullptr;

    if (!allowed) {
        // [GRIFF-AUSSETZER 10.09.2026 -- gemessen] Hier landete der Fall, der
        // den Griff zerstoerte: die Waffenerkennung setzte fuer 2,5 s aus
        // (wid=nil, rh_world leer), der Dock rampte aus und rastete danach nie
        // wieder ein -- die Hand hing 51 cm neben dem Griff, weil zum
        // Wiedereinrasten dock_threshold (11 cm) noetig waere.
        //
        // Ein Aussetzer ist aber kein Loslassen. Wird der Left-Grip weiter
        // gehalten und war der Dock gerade aktiv, bleibt der Zustand
        // EINGEFROREN, bis eines davon eintritt:
        //   - der Grip wird losgelassen        -> normaler Weg unten
        //   - HOLD_MAX Sekunden sind vorbei    -> es war doch kein Aussetzer
        //   - eine ANDERE Waffe kommt zurueck  -> echter Waffenwechsel
        // Der Waffenwechsel loest also weiterhin sofort; nur die Luecke
        // derselben Waffe wird ueberbrueckt.
        constexpr double HOLD_MAX = 4.0;

        const bool had_dock = m_support.docked || m_support.blend_factor > 0.0f;
        const bool same_or_unknown = !m_wep_cache.id.has_value()
                                     || !m_support.dock_wid.has_value()
                                     || *m_wep_cache.id == *m_support.dock_wid;

        if (had_dock && same_or_unknown && is_left_grip_held()) {
            const double now = clock_now();

            if (m_support.hold_t0 <= 0.0) {
                m_support.hold_t0 = now;
            }

            if ((now - m_support.hold_t0) < HOLD_MAX) {
                return;   // (2a) Zustand halten -- NICHT ausrampen
            }
        }

        m_support.hold_t0 = 0.0;
        m_support.switch_docked = false;
        m_support.switch_latched = false;
        m_support.prev_left_grip = false;
        m_support.free_near = false;
        ramp_out();
        return;   // (2)
    }

    // Waffe ist wieder da -> Aussetzer-Uhr zuruecksetzen.
    m_support.hold_t0 = 0.0;

    const glm::vec3 rh_world = *m_cache.rh_world;
    const glm::quat rh_rot = m_cache.rh_rot.value_or(glm::quat{1.0f, 0.0f, 0.0f, 0.0f});

    const auto dist = [](const glm::vec3& a, const glm::vec3& b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    // [DOCK_PROXIMITY] Einmal pro Frame: ist die freie Hand wirklich am
    // Vordergriff? Gilt fuer ALLE Dock-Pfade und ist vor allem fuer die
    // Two-Hand-IK wichtig -- die nutzt absichtlich ein weites max_dist fuer den
    // Schwenk und wuerde sonst beim Aimen UEBERALL docken. Mit Hysterese.
    {
        const auto& off0 = get_support_offset(m_cache.weapon_key);
        const float d = std::min(dist(free_pos, *support_pos), dist(free_pos, rh_world));
        const float thr = m_support.free_near ? off0.undock_threshold : off0.dock_threshold;
        m_support.free_near = d < thr;

        // [SUPPORT VOR CHOKE 2026-09-17 -- Ansage des Users] Wie Bolt, Bogen und
        // Red9-Klappe: ist die linke Hand am Vordergriff, gehoert die Grip-Flanke
        // dem Zwei-Hand-Dock und NICHT dem Choke. Zeitstempel statt Flag: tickt es
        // nicht mehr (Waffe weg, Support aus), laeuft die Sperre im Choke nach
        // 0,15 s von selbst aus. Gelesen in RE4VRChoke.cpp.
        if (m_support.free_near) {
            re4vr::lua_set_number("__re4_lh_support_near_t", clock_now());
        }
    }

    // [MAG-DOCK-LOCK] Linke Hand haelt ein Mag/Shell (oder <1.2 s nach dem
    // Insert) -> NICHT an den Schaft docken, sonst springt die volle Hand vorne
    // auf die Waffe. Vorrang vor ALLEN Dock-Pfaden.
    if (re4vr::lua_is_truthy("__vr_mag_in_hand")) {
        m_support.switch_docked = false;
        m_support.switch_latched = false;
        m_support.prev_left_grip = false;
        ramp_out();
        return;   // (3)
    }

    // [SWITCH_DOCK] Schalter-Dock-Entscheid. Hat VORRANG vor Vordergriff,
    // two_hand und Proximity.
    auto* sw = (m_wep_cache.id.has_value() && is_switch_dock_weapon(*m_wep_cache.id))
                   ? get_support_offset_switch(m_cache.weapon_key)
                   : nullptr;

    if (sw != nullptr) {
        const bool grip = is_left_grip_held();
        const glm::vec3 switch_pos = rh_world + (rh_rot * glm::vec3{sw->pos_x, sw->pos_y, sw->pos_z});
        const float d_switch = dist(free_pos, switch_pos);
        m_support.switch_dbg_d = d_switch;   // [DIAG] Statuszeile

        // Grip-Druck-FLANKE: einmalig latchen (Schalter, wenn im Radius, sonst
        // Schaft).
        if (grip && !m_support.prev_left_grip) {
            m_support.switch_latched = d_switch < sw->dock_dist;

            // Frischer Grip am SCHAFT -> Switch-Pose-Latch sofort aufheben,
            // sonst klebte die Hand bei schnellem Re-Grip waehrend des
            // Ausblendens an der Switch-Pose.
            if (!m_support.switch_latched) {
                m_support.switch_blend_lock = false;
            }
        }

        if (!grip) {
            m_support.switch_latched = false;
            m_support.sw_consumed = false;   // [ONE-SHOT] Release hebt die Sperre auf
        }

        m_support.prev_left_grip = grip;

        if (m_support.switch_preview || m_support.switch_aim_preview
            || m_support.switch2_preview || m_support.switch2_aim_preview) {
            m_support.switch_docked = true;
        } else {
            m_support.switch_docked = grip && m_support.switch_latched;
        }

        // [FIRE_MODE] Am Schalter + Grip zykelt die Left-Trigger-FLANKE
        // Full->Burst->Single->Full. Der linke Trigger behaelt seine normale
        // Funktion, er wird nur gelesen.
        if (m_support.switch_docked && grip) {
            const bool trig = is_left_trigger_held();

            if (trig && !m_support.prev_switch_trigger) {
                m_support.fire_mode = fire_mode_next(*m_wep_cache.id, m_support.fire_mode);

                // [SWITCH ONE-SHOT] Chicago Sweeper: nach EINEM Umlegen ist der
                // Left-Grip sofort wirkungslos -- weder Schalter noch
                // Vordergriff docken -- bis er physisch losgelassen und neu
                // gedrueckt wird.
                if (is_switch_oneshot(*m_wep_cache.id)) {
                    m_support.switch_latched = false;
                    m_support.sw_consumed = true;
                }
            }

            m_support.prev_switch_trigger = trig;
        } else {
            m_support.prev_switch_trigger = false;
        }
    } else {
        m_support.switch_docked = false;
        m_support.switch_latched = false;
        m_support.prev_left_grip = false;
        m_support.sw_consumed = false;
    }

    // [FIRE_MODE] Gegen den Zyklus der aktuellen Waffe validieren: nach einem
    // Waffenwechsel steht z.B. die LE5 auf Single=2, der Sweeper hat aber nur
    // {0,1} -> auf Full zuruecksetzen.
    {
        bool ok_m = false;

        for (const auto m : fire_mode_cycle(m_wep_cache.id.value_or(0))) {
            if (m == m_support.fire_mode) {
                ok_m = true;
                break;
            }
        }

        if (!ok_m) {
            m_support.fire_mode = 0;
        }
    }

    // burst_active ist abgeleitet (Hebel weg von Full) -- Feuer-Block und
    // switch2 nutzen das.
    m_support.burst_active = m_support.fire_mode != 0;

    // Sound bei JEDEM Moduswechsel (Flanke von fire_mode).
    if (m_support.fire_mode != m_support.prev_fire_mode) {
        play_le5_switch_sound();
        m_support.prev_fire_mode = m_support.fire_mode;
    }

    if (m_support.switch_docked) {
        // Latch: beim Loslassen die Switch-Pose ausblenden, NICHT auf den Schaft
        // springen.
        m_support.switch_blend_lock = true;
        m_support.docked = true;
        m_support.target_blend = 1.0f;
        const float sbs = sw != nullptr ? sw->blend_speed : 0.150f;   // zackigerer Switch-Lerp

        if (m_support.blend_factor < m_support.target_blend) {
            m_support.blend_factor = std::min(m_support.blend_factor + sbs, m_support.target_blend);
        } else if (m_support.blend_factor > m_support.target_blend) {
            m_support.blend_factor = std::max(m_support.blend_factor - sbs, m_support.target_blend);
        }

        return;   // (4)
    }

    // [TWO_HAND_IK] aktiv -> linke Hand an den Vordergriff pinnen. Blend mit
    // blend_speed REINLERPEN statt hart auf 1.0 zu snappen.
    if (m_support.force_dock || m_two_hand.active) {
        m_support.docked = true;
        m_support.target_blend = 1.0f;

        if (m_support.blend_factor < m_support.target_blend) {
            m_support.blend_factor = std::min(m_support.blend_factor + m_support.blend_speed,
                                              m_support.target_blend);
        }

        return;   // (5)
    }

    // [GRIP_DOCK] Group-B-Waffen docken NUR mit gehaltenem Left-Grip. Group A
    // ist hier nicht gelistet und dockt weiterhin automatisch bei Naehe.
    if (m_wep_cache.id.has_value() && is_grip_dock_weapon(*m_wep_cache.id)
        && (!is_left_grip_held() || m_support.sw_consumed)) {
        // [HARTER BREAK] Losgelassen ist losgelassen -- kein Nachblenden.
        break_now();
        return;   // (6)
    }

    const auto& off = get_support_offset(m_cache.weapon_key);
    const float distance_to_support = dist(free_pos, *support_pos);
    const float distance_to_weapon = dist(free_pos, rh_world);
    const float effective_distance = std::min(distance_to_support, distance_to_weapon);

    if (!m_support.docked) {
        // [GRIP HAELT DEN DOCK 10.09.2026] Zweiter Weg zurueck in den Dock:
        // wurde nie losgelassen und der Blend steht noch, war das kein echtes
        // Loesen, sondern der Slide-Rack-Block, der `docked` beim Pumpen
        // absichtlich auf false setzt. Gemessen: danach wartete der Dock 4,5 s
        // auf die 11-cm-Distanz, waehrend der Abstand um 10-12 cm pendelte --
        // genau die Phase, in der die Hand neben dem Griff hing.
        // Der Weg ueber die Distanz bleibt fuer den echten Erstkontakt.
        const bool still_holding = m_wep_cache.id.has_value()
                                   && is_grip_dock_weapon(*m_wep_cache.id)
                                   && is_left_grip_held()
                                   && m_support.blend_factor > 0.001f
                                   && m_support.dock_wid.has_value()
                                   && *m_support.dock_wid == *m_wep_cache.id;

        if (still_holding || effective_distance < off.dock_threshold) {
            m_support.docked = true;
            m_support.target_blend = 1.0f;
            m_support.dock_wid = m_wep_cache.id;   // [GRIFF-AUSSETZER] s. Pfad (2a)
        }
    } else {
        // [GRIP_LATCH] Ist die Hand ueber den LEFT GRIP angedockt (Group-B),
        // haelt der GRIP das Dock -- nicht die Distanz. Vorher konnte die
        // IK-Hand mitten im Halten abreissen, sobald man ueber
        // undock_threshold hinauskam (Arm gestreckt, Waffe bewegt sich,
        // Recoil-Nachlauf). Das Loslassen loest weiterhin sauber, das macht der
        // GRIP_DOCK-Block oben. BEWUSST nur fuer Group B: Group A dockt ohne
        // Grip allein ueber Naehe -- dort MUSS die Distanz weiter loesen, sonst
        // klebt die Hand fest.
        bool grip_latched = m_wep_cache.id.has_value() && is_grip_dock_weapon(*m_wep_cache.id)
                            && is_left_grip_held();

        // [GRIP_LATCH_REACH] ...aber nicht unbegrenzt: haelt man die Waffe mit
        // der rechten Hand weit weg, zieht der Shoulder-Follow in arm_chain die
        // Schulter mit und der linke Arm wird zu Gummi. Der Latch bricht daher,
        // wenn der DOCKPUNKT weiter von der linken Oberarm-Wurzel entfernt ist
        // als grip_latch_reach * echte Armreichweite.
        // WARUM Schulter->Dockpunkt und nicht Controller->Waffe: gestreckt wird
        // der ARM, und der haengt an der Schulter -- wo der linke Controller
        // gerade ist, ist dafuer egal.
        if (grip_latched) {
            const auto l_root = re4vr::lua_get_vec3("__vr_arm_chain_L_root");
            const auto l_max = re4vr::lua_get_number_opt("__vr_arm_chain_L_maxreach");

            if (l_root.has_value() && l_max.has_value()) {
                const float reach = dist(*l_root, *support_pos);

                if (!is_grip_locked_shotgun(m_wep_cache.id.value_or(0))
                    && reach > static_cast<float>(*l_max) * m_support.grip_latch_reach) {
                    // [ARMLAENGE = BRUCH -- ZURUECKGEBAUT 10.09.2026]
                    // Hier stand kurzzeitig break_now(): "Arm am Ende = Griff
                    // zu Ende". In Lua nachgemessen war das falsch -- 35 von 35
                    // Abbruechen kamen aus dieser Regel, bei gedruecktem Grip
                    // und einem Controller-Abstand zum Griff von 2,3 bis 15 cm.
                    // Die Hand lag also AUF dem Griff.
                    //
                    // Grund: reach misst Schulterwurzel -> Griffpunkt gegen
                    // __vr_arm_chain_L_maxreach (hier 0,657 m). Der Vordergriff
                    // einer angelegten Shotgun liegt schlicht weiter weg, ohne
                    // dass der echte Arm ueberstreckt waere. Die Groesse taugt
                    // fuer den Latch (weiche Regel), nicht fuer einen Bruch.
                    grip_latched = false;   // ueberstreckt -> Distanz-Regel loest
                }
            }
        }

        // [RECOIL-GATE RAUS] Hier stand `and not recoil_active` -- solange der
        // Recoil lief, wurde das Dock NICHT geloest und die Entfernung gar nicht
        // mehr geprueft. Beim Feuern ist der Recoil praktisch durchgehend aktiv
        // -> die linke Hand klebte an der Waffe, egal wie weit sie weg war.
        // Jetzt entscheidet allein die Distanz.
        if (!grip_latched && distance_to_support > off.undock_threshold
            && distance_to_weapon > off.undock_threshold) {
            // [HARTER BREAK] Zu weit weggezogen -- sichtbar abreissen statt
            // die Hand noch ein paar Frames am Griff nachhaengen zu lassen.
            break_now();
        }
    }

    if (m_support.blend_factor < m_support.target_blend) {
        m_support.blend_factor = std::min(m_support.blend_factor + m_support.blend_speed,
                                          m_support.target_blend);
    } else if (m_support.blend_factor > m_support.target_blend) {
        m_support.blend_factor = std::max(m_support.blend_factor - m_support.blend_speed,
                                          m_support.target_blend);
    }

    // [SUPPORT-FLAG] Zustand nach aussen melden: die Red9-Bloecke (reload2
    // wp4002 / reload5_dlc wp6113) sperren damit den Slide-Grab, solange die
    // linke Hand am Griff haengt -- sonst reisst dieselbe Hand gleichzeitig am
    // Support und am Verschluss.
    // QUELLE IST docked, NICHT two_hand.active (das steht bei Pistolen nie) und
    // NICHT blend_factor (der rampt beim Loesen aus; gefordert ist "Hand weg ->
    // sofort frei"). docked faellt an der Undock-Schwelle sofort.
    re4vr::lua_set_bool("__vr_support_hand_active", m_support.docked);
}

// ============================================================================
// attach_left_hand (Lua Z.3373-3612)
// ============================================================================

void RE4VRMotion::attach_left_hand(const glm::vec3& cam_pos, const glm::quat& cam_rot,
                                   const VrData& vr_data, bool update_dock) {
    if (!m_left_hand.enabled || m_left_hand.joint.obj == nullptr) {
        return;
    }

    glm::vec3 ctrl_pos{};
    glm::quat ctrl_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!controller_to_world(vr_data.left_pos, &vr_data.left_rot, cam_pos, cam_rot,
                             ctrl_pos, ctrl_rot)) {
        return;
    }

    re4vr::lua_set_vec3("__vr_lh_ctrl_raw", ctrl_pos);   // [ROHE CONTROLLER-POS]

    glm::vec3 hand_pos = ctrl_pos;
    glm::quat hand_rot = ctrl_rot;

    // Die linke Hand hat EINEN globalen Offset, keinen per-Waffe.
    {
        WeaponOffset l{};
        l.px = m_hand_offset_l.px;
        l.py = m_hand_offset_l.py;
        l.pz = m_hand_offset_l.pz;
        l.rx = m_hand_offset_l.rx;
        l.ry = m_hand_offset_l.ry;
        l.rz = m_hand_offset_l.rz;
        apply_hand_offset(hand_pos, hand_rot, l);
    }

    if (m_rot_smooth_hands > 0.0f && m_smoothing.left_rot.has_value()) {
        hand_rot = glm::slerp(*m_smoothing.left_rot, hand_rot, 1.0f - m_rot_smooth_hands);
    }

    if (m_pos_smooth_hands > 0.0f && m_smoothing.left_pos.has_value()) {
        const float k = 1.0f - m_pos_smooth_hands;
        const glm::vec3 sp = *m_smoothing.left_pos;
        hand_pos = glm::vec3{sp.x + (hand_pos.x - sp.x) * k, sp.y + (hand_pos.y - sp.y) * k,
                             sp.z + (hand_pos.z - sp.z) * k};
    }

    m_smoothing.left_pos = hand_pos;
    m_smoothing.left_rot = hand_rot;

    // [PUMP-ZUG] Rohe, UN-gedockte linke Hand-Welt publizieren -> der
    // Shotgun-Pump liest sie fuer die Zug-Erkennung. MUSS vor der
    // Dock-Ueberschreibung stehen, sonst waere sie gepinnt (Zug = 0). So kann
    // der Pump nahtlos aus der Two-Hand-Haltung starten.
    re4vr::lua_set_vec3("__vr_lh_ctrl_world", hand_pos);

    // [SUPPORT_HAND] Das Smoothing trackt die FREIE Hand; der Dock-Blend kommt
    // danach.
    glm::vec3 support_pos{};
    glm::quat support_rot{1.0f, 0.0f, 0.0f, 0.0f};
    bool have_support = false;

    if ((m_support.enabled && is_support_hand_weapon()) || m_support.blend_factor > 0.0f) {
        have_support = get_support_pose(support_pos, support_rot);
    }

    // [PUMP] Live Support-Hand-Weltpose exportieren: reload nutzt sie als
    // Pump-Hand-Ziel, damit beim Pumpen der getunte Support-Offset gilt statt
    // der rohen Slide-Joint-Pose.
    if (have_support) {
        re4vr::lua_set_vec3("__vr_support_hand_world_pos", support_pos);
        re4vr::lua_set_quat("__vr_support_hand_world_rot", support_rot);
    } else {
        re4vr::lua_set_nil("__vr_support_hand_world_pos");
        re4vr::lua_set_nil("__vr_support_hand_world_rot");
    }

    if (update_dock) {
        // BEWUSST auch ohne Dock-Pose rufen -- s. Kommentar an der Funktion.
        update_support_dock(hand_pos, have_support ? &support_pos : nullptr);
    }

    if (m_support.blend_factor > 0.0f && have_support) {
        if (m_support.blend_factor >= 1.0f) {
            hand_pos = support_pos;
            hand_rot = support_rot;
        } else {
            hand_pos = glm::mix(hand_pos, support_pos, m_support.blend_factor);
            hand_rot = glm::slerp(hand_rot, support_rot, m_support.blend_factor);
        }
    }

    // [HAND_CLAMP] Vor cache und Joint-Write, damit auch das publizierte
    // __vr_lh_world geclampt ist und arm_chain aufs gleiche Ziel solvt.
    // ABER: bei GEDOCKTER Stuetzhand NICHT clampen -- sonst zieht der Clamp die
    // Hand bei weit ausgeschwenkter Waffe vom Vordergriff weg (Y-Drop Richtung
    // linke Schulter). Dock hat Vorrang, der Arm darf strecken ("prefer dock").
    // [PUMP_NO_CLAMP] Auch waehrend das Slide-Dock ein-/ausblendet nicht
    // clampen: der SLIDE_RACK-PRIORITY-Block setzt docked=false BEVOR das
    // Slide-Dock voll greift -> sonst yankt der Clamp die gestreckte Hand fuer
    // 1-2 Frames zur Schulter.
    const double slide_dock_b = re4vr::lua_get_number("__vr_slide_dock_blend_factor", 0.0);

    // [GRIFF HAT VORRANG 10.09.2026] Zusaetzlich blend_factor pruefen. Gemessen:
    // nach jedem Pump steht `docked` fuer Sekunden auf false, waehrend blend
    // noch 1.00 ist -- die Hand gehoert also dem Griff, der Clamp zog sie aber
    // trotzdem 3-9 cm weg (47 von 99 Zeilen ueber 2 cm daneben). Solange zum
    // Griff geblendet wird, hat der Griff Vorrang.
    //
    // Ueberstrecken kann daraus nicht werden: der Arm bricht vorher ueber
    // [ARMLAENGE = BRUCH] in update_support_dock, und danach ist blend 0 --
    // ab dem Moment clampt diese Zeile wieder ganz normal.
    if (!m_support.docked && m_support.blend_factor <= 0.001f && slide_dock_b <= 0.001) {
        hand_pos = clamp_hand_to_arm_reach(hand_pos, "L");
    } else if (m_two_hand.active
               && !(m_support.blend_factor > 0.001f
                    && is_grip_locked_shotgun(m_wep_cache.id.value_or(0)))) {
        // [TWO_HAND_CLAMP] Ausnahme vom "prefer dock": im ZWEIHAND-IK wird
        // trotzdem geclampt. Drueckt man die Waffe von sich weg, ist die linke
        // Hand vorne am Lauf und damit zuerst am Reichweiten-Ende -> ohne Clamp
        // streckt sichtbar das Handgelenk.
        //
        // [DOPPELREGLER] Bei wp4100 haelt der Z-Clamp die WAFFE bereits so, dass
        // der Pumpgriff in Reichweite bleibt. Laeuft der Hand-Clamp zusaetzlich,
        // regeln ZWEI Regler dieselbe Groesse an derselben Grenze -- beim Laufen
        // wandert die Schulter, die Reichweitenkugel wandert mit, und beide
        // schaukeln sich auf = Zittern des ganzen Arms.
        if (!(m_wep_cache.id.has_value() && *m_wep_cache.id == 4100)) {
            hand_pos = clamp_hand_to_arm_reach(hand_pos, "L");
        }
    }

    // [CHOKE_HAND_Y] Solange wir jemanden wuergen, die Hoehe der linken Hand
    // klemmen -- BEWUSST HIER, eine Zeile vor cache.lh_world: von hier speisen
    // sich Joint-Write, arm_chain UND das publizierte __vr_lh_world. Klemmt man
    // stattdessen weiter unten die geschriebene Joint-Pose, laufen Quelle und
    // sichtbare Hand auseinander und der Arm sitzt im Bauch des Gegners. Der
    // Gegner haengt am L_Palm, folgt also derselben geklemmten Kette.
    {
        const auto cp = re4vr::lua_get_number_opt("__re4_choke_hand_play");
        const bool choking = re4vr::lua_call_global_bool("__re4_is_choking", false);

        if (cp.has_value() && choking) {
            const float cpf = static_cast<float>(*cp);

            // Vorrang hat die FESTE Wuergehoehe aus choke (am eigenen Halsjoint,
            // Rig-Versatz schon herausgerechnet). Fehlt sie, gilt die beim
            // ersten Choke-Frame eingefrorene Hoehe plus unser eigener Drift.
            auto cy = re4vr::lua_get_number_opt("__re4_choke_hand_y");

            if (!cy.has_value()) {
                if (!re4vr::lua_get_number_opt("__re4_choke_hand_base").has_value()) {
                    re4vr::lua_set_number("__re4_choke_hand_base", hand_pos.y);
                }

                cy = re4vr::lua_get_number("__re4_choke_hand_base", hand_pos.y)
                     + re4vr::lua_get_number("__re4_choke_dy", 0.0);
            }

            const float cyf = static_cast<float>(*cy);
            float y = hand_pos.y;

            if (y > cyf + cpf) {
                y = cyf + cpf;
            } else if (y < cyf - cpf) {
                y = cyf - cpf;
            }

            // [Y WEICH] Im Spielraum nicht 1:1 folgen, sondern nachziehen.
            // Regler in choke; fehlt er oder steht auf 1, bleibt es hart.
            const auto yl = re4vr::lua_get_number_opt("__re4_choke_y_lerp");

            if (yl.has_value() && *yl < 0.999) {
                if (const auto pv = re4vr::lua_get_number_opt("__re4_choke_y_prev"); pv.has_value()) {
                    const float f = *yl > 0.0 ? static_cast<float>(*yl) : 0.0f;
                    y = static_cast<float>(*pv) + (y - static_cast<float>(*pv)) * f;
                }
            }

            re4vr::lua_set_number("__re4_choke_y_prev", y);

            // [X GENAU WIE Y] Klemmfenster UND Nachziehen, gleicher Regler.
            // Geklemmt wird aber NICHT die Welt-X: die haengt an der
            // Blickrichtung -- nach Osten schauend liegt "vorne" komplett in X,
            // ein Welt-X-Fenster wuerde den ausgestreckten Arm zusammenreissen.
            // Gemessen wird SEITLICH im Kopf-Frame, Yaw-only, damit die Hoehe
            // unberuehrt bleibt. Vor/zurueck bleibt frei.
            glm::vec3 cpos{};
            glm::quat crot{1.0f, 0.0f, 0.0f, 0.0f};
            bool have_ref = false;

            if (auto* c = sdk::get_primary_camera(); c != nullptr) {
                if (auto* cgo = re4vr::call_safe<::REManagedObject*>(c, "get_GameObject");
                    cgo != nullptr) {
                    if (auto* ctf = re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform");
                        ctf != nullptr) {
                        const bool okp = get_vec3(ctf, "get_Position", cpos);
                        const bool okr = get_quat(ctf, "get_Rotation", crot);
                        have_ref = okp && okr;
                    }
                }
            }

            // [BEZUGSPUNKT -- GEMESSEN, das war die Ursache] get_primary_camera
            // ist in VR NICHT der Kopf, sondern RE4s Spielkamera: sie sitzt rund
            // 2 m entfernt und KREIST beim Kopfdrehen um den Spieler (gemessen:
            // 1,15 m in x und 1,69 m in z ueber einen Kopfschwenk). Die Rechnung
            // unten setzt die Hand von genau diesem wandernden Punkt aus neu
            // zusammen -> der Gegner wanderte 37 cm mit. Richtig ist der SPIELER
            // als Bezug: der steht beim Kopfdrehen still.
            //
            // [V4 DES ZWEITEN NACHTRAGS] Das Original holt die Body-Position mit
            // `select(2, pcall(...))`. Schlaegt der Aufruf fehl, ist das Ergebnis
            // ein FEHLER-STRING (truthy!) -> `cpos = <string>` -> `cpos.x` ist
            // nil -> Arithmetik-Fehler, der die Funktion ungeschuetzt abbricht.
            // Hier wird sauber geprueft; das ist die einzige bewusste Abweichung
            // in diesem Block und macht den Port robuster, nicht anders.
            if (auto* ctxp = re4vr::player_context(); ctxp != nullptr) {
                if (auto* bgo = re4vr::call_safe<::REManagedObject*>(ctxp, "get_BodyGameObject");
                    bgo != nullptr) {
                    if (auto* btf2 = re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform");
                        btf2 != nullptr) {
                        glm::vec3 bp2{};

                        if (get_vec3(btf2, "get_Position", bp2)) {
                            cpos = bp2;
                        }
                    }
                }
            }

            if (have_ref) {
                // Blickwinkel EINMAL beim Zugriff einfrieren -- sonst dreht das
                // Fenster "links/rechts vor mir" mit dem Kopf weiter und die
                // Klemme zoege die Hand mit. Im Griff sperrt binding das
                // Stick-Drehen, es gibt also keine gewollte Drehung.
                const float yw_live = std::atan2(2.0f * (crot.w * crot.y + crot.x * crot.z),
                                                 1.0f - 2.0f * (crot.y * crot.y + crot.x * crot.x));

                if (!re4vr::lua_get_number_opt("__re4_choke_yaw_base").has_value()) {
                    re4vr::lua_set_number("__re4_choke_yaw_base", yw_live);
                }

                const float yw = static_cast<float>(re4vr::lua_get_number("__re4_choke_yaw_base", yw_live));
                const float sn = std::sin(yw);
                const float cs = std::cos(yw);
                const float dx = hand_pos.x - cpos.x;
                const float dz = hand_pos.z - cpos.z;

                // zurueckdrehen (inverse Yaw): lat = links/rechts, fwd = vor/zurueck
                float lat = cs * dx - sn * dz;
                const float fwd = sn * dx + cs * dz;

                // [HAND IMMER GLEICH 13.09.2026 -- Ansage "sollte die Hand
                // nicht immer gleich sein, egal was passiert?"]
                // Vorrang hat die FESTE seitliche Lage aus der Choke-Config
                // (__re4_choke_hand_lat, Regler "Wuergehand seitlich").
                //
                // Frueher stand hier nur der beim ERSTEN Choke-Frame
                // eingefrorene Messwert -- also die Zufallslage der Hand im
                // Moment des Zupackens. Die Hand-Sonde zeigte ueber drei Griffe
                // -0.281 / -0.227 / +0.016 m: bis zu 30 cm Unterschied, und
                // damit war der Griff nicht einstellbar.
                //
                // Der eingefrorene Weg bleibt als Rueckfall, falls die Global
                // fehlt (Choke aus einer aelteren Fassung, Lua-Reset).
                const auto lat_fix = re4vr::lua_get_number_opt("__re4_choke_hand_lat");

                if (!lat_fix.has_value()
                    && !re4vr::lua_get_number_opt("__re4_choke_lat_base").has_value()) {
                    re4vr::lua_set_number("__re4_choke_lat_base", lat);
                }

                const float lb = lat_fix.has_value()
                    ? static_cast<float>(*lat_fix)
                    : static_cast<float>(re4vr::lua_get_number("__re4_choke_lat_base", lat));

                if (lat > lb + cpf) {
                    lat = lb + cpf;
                } else if (lat < lb - cpf) {
                    lat = lb - cpf;
                }

                if (yl.has_value() && *yl < 0.999) {
                    if (const auto pv2 = re4vr::lua_get_number_opt("__re4_choke_lat_prev");
                        pv2.has_value()) {
                        const float f = *yl > 0.0 ? static_cast<float>(*yl) : 0.0f;
                        lat = static_cast<float>(*pv2) + (lat - static_cast<float>(*pv2)) * f;
                    }
                }

                re4vr::lua_set_number("__re4_choke_lat_prev", lat);

                // wieder in die Welt drehen
                hand_pos = glm::vec3{cpos.x + (cs * lat + sn * fwd), y,
                                     cpos.z + (-sn * lat + cs * fwd)};
            } else {
                hand_pos = glm::vec3{hand_pos.x, y, hand_pos.z};
            }
        } else if (re4vr::lua_get_number_opt("__re4_choke_hand_base").has_value()
                   || re4vr::lua_get_number_opt("__re4_choke_y_prev").has_value()
                   || re4vr::lua_get_number_opt("__re4_choke_lat_base").has_value()) {
            // Fenster zu -> beim naechsten Griff neu einfrieren bzw. weich
            // anfangen. Der Blickwinkel wird mit zurueckgesetzt, sonst gilt beim
            // naechsten Griff der Winkel des vorigen.
            for (const auto* n : {"__re4_choke_hand_base", "__re4_choke_y_prev",
                                  "__re4_choke_lat_base", "__re4_choke_lat_prev",
                                  "__re4_choke_yaw_base"}) {
                re4vr::lua_set_nil(n);
            }
        }
    }

    // cache.lh_world = ROHER Controller: __vr_lh_world -> reload misst daran den
    // Rack-Zug. ROH lassen!
    m_cache.lh_world = hand_pos;
    m_cache.lh_rot = hand_rot;

    // [SLIDE_DOCK] Beim Slide-Grab die sichtbare Hand ans Dock: Rotation UND
    // Position. [LDOCK_RH] Die Position kommt auf das von arm_chain ANGEKERTE
    // Ziel (an der soliden rechten Hand) statt auf den Controller -> kein
    // Flackern zwischen zwei Schreibern. Der Rack-Zug bleibt heil, weil
    // lh_world oben roh bleibt.
    glm::quat wrot = hand_rot;
    glm::vec3 write_pos = hand_pos;

    if (slide_dock_b > 0.001) {
        if (const auto sdock_rot = re4vr::lua_get_quat("__vr_slide_hand_world_rot");
            sdock_rot.has_value()) {
            wrot = slide_dock_b >= 0.999
                       ? *sdock_rot
                       : glm::slerp(hand_rot, *sdock_rot, static_cast<float>(slide_dock_b));
        }

        // arm_chain hat das L-Ziel schon fertig gelerpt -> 1:1 uebernehmen,
        // KEIN zweiter Lerp hier (sonst zwei Startpunkte = Flackern).
        if (const auto anch = re4vr::lua_get_vec3("__vr_ldock_anchored"); anch.has_value()) {
            write_pos = *anch;
        }

        // [RELOAD_LEXIT_FADE] Solange das Push-Dock aktiv ist, die AKTUELLE
        // Magazin-Pose mitschreiben -> beim Dock-Ende ist das die "from"-Pose
        // fuer den weichen Ausfade.
        m_reload_lexit_from.p = write_pos;
        m_reload_lexit_from.r = wrot;
    }

    // [KS4_EXIT_FADE] linke Hand.
    // [CHOKE: HAND STARR 2026-09-11] Fuer die POSITION gibt es die Klemme
    // weiter oben (hand_play/lat_base/yaw_base). Fuer die ROTATION gab es
    // NICHTS -- wrot kam ungefiltert vom Controller, und damit liess sich die
    // Hand im Wuergegriff frei weiterdrehen. Beim ersten Choke-Frame wird sie
    // deshalb eingefroren und bis zum Loslassen gehalten.
    // Zurueckgesetzt wird zusammen mit den uebrigen Choke-Basiswerten unten.
    {
        const bool choking_now = re4vr::lua_call_global_bool("__re4_is_choking", false);

        if (choking_now) {
            if (!m_choke_lh_rot.has_value()) {
                m_choke_lh_rot = wrot;
            }

            wrot = *m_choke_lh_rot;
        } else if (m_choke_lh_rot.has_value()) {
            m_choke_lh_rot.reset();
        }
    }

    ks4_exit_apply(m_left_hand.joint.obj, write_pos, wrot, true);
    // [RELOAD_LEXIT_FADE] nach dem Mag-Push weich von der Magazin-Pose zum
    // Controller ausfaden statt hart zu snappen.
    reload_lexit_apply(write_pos, wrot);

    write_joint_pose(m_left_hand.joint.obj, write_pos, &wrot);
    m_cache.lh_joint_pos = write_pos;
    m_cache.lh_joint_rot = wrot;

    // [ARM_SYNC] __vr_lh_joint_pos (das IK-Ziel von arm_chain) wurde frueher NUR
    // am Ende des normalen Ticks publiziert. attach_left_hand laeuft aber in
    // mehreren Paessen -- in den uebrigen blieb das Global stehen, waehrend die
    // Hand schon neu geschrieben war. Gemessen: bis 155 mm Unterschied im Ziel
    // INNERHALB eines Frames, 32 % der Frames betroffen -> die Kette loeste den
    // Arm mehrfach verschieden = Zappeln beim Laufen.
    // Gegated auf die zwei Pump-Shotguns, damit keine andere Waffe ihr Verhalten
    // aendert.
    if (m_wep_cache.id.has_value() && is_pump_grip_weapon(*m_wep_cache.id)) {
        re4vr::lua_set_vec3("__vr_lh_joint_pos", write_pos);
        re4vr::lua_set_quat("__vr_lh_joint_rot", wrot);
    }
}

// ============================================================================
// [REPIN_LEFT] Lua Z.3631-3655
//
// URSACHE: der Pass "UpdateJointExpression" ruft attach_right_hand und
// attach_weapon, aber KEIN attach_left_hand. Die Waffe (und damit jedes Joint an
// ihr) wird dort neu positioniert, die linke Hand bleibt auf dem Stand des
// vorherigen Passes. Im Stehen unsichtbar; beim Laufen wandert die Waffe pro
// Pass mehrere Millimeter -> die Hand springt zwischen "mitgezogen" und
// "hinterher" = Zittern.
//
// ABSICHTLICH SCHMAL: nur die sichtbare Joint-Pose wird nachgezogen. KEIN
// update_support_dock, KEIN Smoothing, KEIN Slide-Dock-Blend, keine Fades --
// die haben ihren Pass schon gehabt und duerfen nicht zweimal pro Frame laufen.
//
// [TEIL2/L3] Trotz "schmal" schreibt der Aufruf ueber get_support_pose weiterhin
// __re4_grip_pull mit -- und zwar NACH dem Konsum in attach_right_hand.
// ============================================================================

void RE4VRMotion::repin_left() {
    if (m_support.repin_off) {
        return;
    }

    if (m_left_hand.joint.obj == nullptr || !m_left_hand.enabled) {
        return;
    }

    // [SCOPE] Lief kurz fuer ALLE Zweihandwaffen. Zurueckgezogen auf die zwei
    // Pumpguns: die eigentliche Ursache war nicht das Timing dieses Passes,
    // sondern die einfrierende Joint-Weltposition (s. GRIP_ANCHOR_RH) -- und die
    // gibt es nur dort, wo der Griffpunkt ueberhaupt aus einem Joint kommt.
    if (!m_wep_cache.id.has_value() || !is_pump_grip_weapon(*m_wep_cache.id)) {
        return;
    }

    if (m_support.blend_factor < 0.999f) {
        return;   // nur im voll gedockten Zustand
    }

    if (re4vr::lua_get_number("__vr_slide_dock_blend_factor", 0.0) > 0.001) {
        return;   // Slide-Dock schreibt selbst
    }

    if (re4vr::lua_get_string("__re4_knife_hand") == "left") {
        return;   // Messerhand gehoert dem Messer
    }

    glm::vec3 p{};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_support_pose(p, r)) {
        return;
    }

    write_joint_pose(m_left_hand.joint.obj, p, &r);
    m_cache.lh_joint_pos = p;
    m_cache.lh_joint_rot = r;

    // [ARM_SYNC] arm_chain nimmt ihr IK-Ziel aus __vr_lh_joint_pos, und das wird
    // sonst NUR am Ende des normalen Ticks publiziert. Zieht dieser Pass die
    // Hand nach, ohne das Global mitzufuehren, loest die Arm-Kette weiter auf die
    // alte Handposition -> Hand und Arm laufen auseinander, beim Laufen mit
    // jedem Frame anders = Zittern der ganzen Kette. arm_chain rechnet danach
    // noch in pre-BeginRendering, greift den frischen Wert also ab.
    re4vr::lua_set_vec3("__vr_lh_joint_pos", p);
    re4vr::lua_set_quat("__vr_lh_joint_rot", r);
}

// ============================================================================
// [FLASHLIGHT] Lua Z.3658-4201
//
// ac0000_00 (Taschenlampen-GO) folgt der LINKEN Hand; sein "light"-Child
// (Lichtkegel) zeigt in Lampenrichtung. Bei Support-Hand wandert der Kegel ans
// HMD und das Lampen-Mesh wird ausgeblendet. Geht die Support-Hand weg -> Lampe
// wieder in die linke Hand.
// ============================================================================

// [POSE-STORE] Pose anwenden ueber reload.lua (Besitzer der Pose-Daten);
// gestures nur als Fallback. Ziel: gestures.lua loeschbar.
bool RE4VRMotion::apply_pose_runtime(const std::string& name, float blend) {
    if (name.empty()) {
        return false;
    }

    return re4vr::lua_call_global_pose("__re4_reload_apply_pose", name, blend);
}

bool RE4VRMotion::fl_knife_equipped() {
    const auto id = get_equip_weapon_id();
    return id.has_value() && is_knife_rel_split_id(*id);
}

// Kamera fuer den Docked-Fall (Kegel am HMD). WICHTIG: die Cam-Transform-Rotation
// ist in VR nur der flache Game-Yaw -- die HMD-Neigung legt die Runtime separat
// drauf. Volle Blickrotation = Game-Cam-Yaw mal rohes Headset.
bool RE4VRMotion::get_flashlight_camera(glm::vec3& out_pos, glm::quat& out_rot) {
    auto* cam = sdk::get_primary_camera();

    if (cam == nullptr) {
        return false;
    }

    glm::mat4 wm{1.0f};

    if (!get_mat4(cam, "get_WorldMatrix", wm)) {
        return false;
    }

    // Basis-Yaw: bevorzugt der firstperson-Export (geflatteter Game-Cam-Yaw),
    // sonst die Cam-Transform-Rotation.
    std::optional<glm::quat> base;

    if (re4vr::lua_get_table_bool("vr_camera_fix", "active", false)) {
        base = re4vr::lua_get_table_quat("vr_camera_fix", "camera_rot");
    }

    if (!base.has_value()) {
        if (auto* go = re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject"); go != nullptr) {
            if (auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform"); tf != nullptr) {
                glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};

                if (get_quat(tf, "get_Rotation", r)) {
                    base = r;
                }
            }
        }
    }

    if (!base.has_value()) {
        return false;
    }

    // Headset OHNE Yaw draufmultiplizieren (nur Pitch/Roll): camera_rot enthaelt
    // den Kopf-Yaw bereits. Der rotation_offset ist -hmd_yaw -> (offset * rohes
    // Headset) hebt den Headset-Yaw auf, sodass er NICHT doppelt zaehlt --
    // sonst laeuft der Kegel voraus.
    glm::quat rot = *base;

    if (auto* vr = VR::get().get(); vr != nullptr) {
        const glm::quat hmd_quat{vr->get_rotation(0)};
        const glm::quat off_quat = vr->get_rotation_offset();
        rot = glm::normalize((*base) * (off_quat * hmd_quat));
    }

    out_pos = glm::vec3{wm[3].x, wm[3].y, wm[3].z};
    out_rot = rot;
    return true;
}

// [FL_ON_DETECT] Die Engine NIMMT ac0000_00 aus der Szene, wenn die Lampe aus
// ist (verifiziert per Probe: GO weg + ActiveLightType = NONE). "GO gefunden"
// heisst also "Lampe an". Darum bei JEDEM Fehlschlag die Handles nullen, sonst
// bliebe flashlight_tf nach dem Ausschalten stale haengen. Die Zeitdrossel gilt
// auch fuer Fehlschlaege -> kein Scene-Walk pro Frame, waehrend die Lampe aus ist.
bool RE4VRMotion::fl_find() {
    const double now = clock_now();

    if (now - m_fl_state.last_check < 1.0) {
        return m_fl_state.flashlight_tf.obj != nullptr;
    }

    m_fl_state.last_check = now;
    drop(m_fl_state.flashlight_mesh);
    // [1:1] body_tf wird VOR den Checks geleert -- die Fehlschlagpfade lassen es
    // damit leer, statt den alten Transform gehalten zu behalten.
    drop(m_fl_state.body_tf);

    const auto clear = [this]() {
        drop(m_fl_state.flashlight_tf);
        drop(m_fl_state.light_tf);
        drop(m_fl_state.flashlight_mesh);
        drop(m_fl_state.light_comp);
        m_fl_light_comp_searched = false;
        re4vr::lua_set_nil("__re4_fl_mesh");   // [KS3] Referenz fuer materials mit aufraeumen

        // [FL_FLIP] Lampe ist weg (= aus) -> binding darf LT nicht mehr als
        // Flip-Tap deuten (LT ist sonst DPAD-Shift), und der Flip-Wunsch
        // verfaellt. Beim naechsten Anschalten startet die Lampe damit
        // garantiert im normalen Griff.
        re4vr::lua_set_bool("__re4_fl_active", false);
        re4vr::lua_set_bool("__vr_fl_flip", false);
        m_fl_state.flip_lerp = 0.0f;
        m_fl_state.flip_prev_target = 0.0f;   // kein Klick beim naechsten Anschalten
    };

    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        clear();
        return false;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{"ch0a0z0_body"}));
    auto* body = str != nullptr
                     ? re4vr::call_safe<::REManagedObject*>(scene, "findGameObject(System.String)", str)
                     : nullptr;

    if (body == nullptr) {
        clear();
        return false;
    }

    auto* btf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    if (btf == nullptr) {
        clear();
        return false;
    }

    store(m_fl_state.body_tf, btf);

    auto* fl_tf = find_by_name(btf, L"ac0000_00");

    if (fl_tf == nullptr) {
        clear();
        return false;
    }

    store(m_fl_state.flashlight_tf, fl_tf);
    store(m_fl_state.light_tf, find_by_name(fl_tf, L"light"));

    // [FL_SCOPE] Light-Component bei jedem Re-Find neu aufloesen.
    drop(m_fl_state.light_comp);
    m_fl_light_comp_searched = false;

    // [FL_FLIP] GO da = Lampe an -> binding gibt LT den Flip-Tap frei.
    re4vr::lua_set_bool("__re4_fl_active", true);
    return true;
}

void RE4VRMotion::fl_set_mesh_visible(bool visible) {
    if (m_fl_state.flashlight_tf.obj == nullptr) {
        return;
    }

    if (m_fl_state.flashlight_mesh.obj == nullptr && m_t_mesh != nullptr) {
        if (auto* fl_go = re4vr::call_safe<::REManagedObject*>(m_fl_state.flashlight_tf.obj,
                                                              "get_GameObject");
            fl_go != nullptr) {
            store(m_fl_state.flashlight_mesh,
                  re4vr::call_safe<::REManagedObject*>(fl_go, "getComponent(System.Type)", m_t_mesh));
        }
    }

    if (m_fl_state.flashlight_mesh.obj != nullptr) {
        // [KS3] materials.lua blendet die Lampe im fp_only aus (motion ist dort
        // dormant) -- die Referenz muss dafuer als Global stehen.
        re4vr::lua_set_managed_object("__re4_fl_mesh", m_fl_state.flashlight_mesh.obj);
        re4vr::call_safe<void*>(m_fl_state.flashlight_mesh.obj, "set_Enabled", visible);
        m_fl_state.mesh_hidden = !visible;
    }
}

// [FL_SCOPE] Kegel (light-Child) hart an/aus: Draw und Update des GO plus die
// Light-Component selbst togglen -> der Kegel verschwindet wirklich (das Mesh
// laeuft separat).
void RE4VRMotion::fl_set_cone_visible(bool visible) {
    if (m_fl_state.light_tf.obj == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_fl_state.light_tf.obj, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(go, "set_DrawSelf", visible);
    re4vr::call_safe<void*>(go, "set_UpdateSelf", visible);

    // Lua merkt sich ein fehlgeschlagenes Suchen als `light_comp = false`, damit
    // nicht jedes Mal erneut gesucht wird. Nativ trennt das ein eigenes Flag.
    if (m_fl_state.light_comp.obj == nullptr && !m_fl_light_comp_searched) {
        for (const auto* tn : {"via.render.SpotLight", "via.render.PointLight", "via.render.Light"}) {
            auto* t = re4vr::runtime_type(tn);
            auto* c = t != nullptr
                          ? re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", t)
                          : nullptr;

            if (c != nullptr) {
                store(m_fl_state.light_comp, c);
                break;
            }
        }

        m_fl_light_comp_searched = true;
    }

    if (m_fl_state.light_comp.obj != nullptr) {
        re4vr::call_safe<void*>(m_fl_state.light_comp.obj, "set_Enabled", visible);
    }
}

// [FL_SCOPE_HIDE] Beim Zielen durch ein montiertes Scope die Lampe KOMPLETT
// verstecken -- Mesh aus, Kegel aus -- sonst haengt sie im Scope-Bild. Laeuft
// AUCH im Scope-Killswitch (der Tick ist dort sonst aus) und wird deshalb
// separat aus dem Killswitch-Zweig gerufen.
// Rueckgabe true = gerade versteckt (apply_flashlight bricht dann ab).
bool RE4VRMotion::fl_scope_update() {
    if (!m_fl_enabled) {
        return false;
    }

    if (!fl_find()) {
        return false;
    }

    if (re4vr::lua_get_tribool("vr_scope_active") == 1) {
        fl_set_mesh_visible(false);
        fl_set_cone_visible(false);
        m_fl_state.scope_hidden = true;
        return true;
    }

    if (m_fl_state.scope_hidden) {
        fl_set_cone_visible(true);   // Scope vorbei -> Kegel zurueck (Mesh holt FL_FORCE_ON)
        m_fl_state.scope_hidden = false;
    }

    return false;
}

// [FL_HANDS_FREE] Die linke Hand hat "was Besseres zu tun" -> Lampe ans HMD,
// Mesh aus, damit die Hand frei fuer Support/Switch/Rack/Mag-Reload ist.
//
// [FL_BUSY] Haengt am DOCK-Zustand (blend_factor > 0), NICHT am gehaltenen Grip.
// Sonst fiele busy auf nichts, sobald man den Grip loslaesst, obwohl die Hand
// noch voll gedockt ist -> die "fl"-Pose kaeme zurueck.
const char* RE4VRMotion::fl_busy_reason() {
    if (m_support.blend_factor > 0.0f) {
        return "support_dock";
    }

    if (re4vr::lua_is_truthy("__vr_slide_rack_active")) {
        return "slide_rack_active";
    }

    if (re4vr::lua_is_truthy("__vr_mag_in_hand")) {
        return "mag_in_hand";
    }

    // [KNIFE_HAND] Messer LINKS (equipped ODER Klon) -> Kegel ans HMD, Mesh aus.
    if (knife_is_left()) {
        return "knife_left";
    }

    return nullptr;
}

// ============================================================================
// [ADA_LAZY] Default-Pose der LINKEN Hand, wenn sie nichts zu tun hat.
// NUR ADA (Separate Ways) -- hartes Body-Namen-Gate, Leon bleibt unberuehrt.
//
// WANN NICHT: exakt die Faelle aus fl_busy_reason -- die im Projekt etablierte
// Definition von "Hand hat was Besseres zu tun". Keine eigene Regel erfunden.
// Der SUPPORT-DOCK wird bewusst NICHT hart abgeschaltet, sondern ueber
// (1 - blend_factor) ausgeblendet: ein hartes Aus wuerde beim Andocken kurz auf
// die native Anim springen (Lazy -> nativ -> Support-Pose).
// ============================================================================

void RE4VRMotion::apply_ada_lazy_pose() {
    // Ada-Gate, 2x/s neu geprueft -- die Funktion laeuft mehrfach pro Frame.
    const double now = clock_now();

    if ((now - m_ada_body_cache_t) >= 0.5) {
        m_ada_body_cache_t = now;
        std::string nm{};

        if (auto* ctx = re4vr::player_context(); ctx != nullptr) {
            if (auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
                body != nullptr) {
                nm = re4vr::obj_name(body);
            }
        }

        m_ada_body_cache_is_ada = nm == "ch3a8z0_body";
    }

    if (!m_ada_body_cache_is_ada) {
        return;
    }

    // [LAZY NUR BEI ZWEIHAND] Die Lazy-Pose lief bei Ada fuer JEDE Waffe.
    // Gewollt ist sie nur, wo die linke Hand ohne Aufgabe herunterhaengt -- also
    // bei den Zweihandwaffen. Bewusst dieselbe Liste wie TWO_HAND_AIM_WEAPONS:
    // eine Quelle, kein zweiter Satz IDs, der beim naechsten Waffenzuwachs
    // vergessen wird.
    //
    // [1:1] Die ID kommt LIVE aus get_equip_weapon_id, NICHT aus wep_cache:
    // der Cache wird nur beschrieben, wenn das Waffen-GO in der Szene gefunden
    // wurde, und faellt beim Ablegen der Waffe nie zurueck -- die Lazy-Pose
    // liefe sonst mit blossen Haenden und um jeden Waffenwechsel herum weiter.
    if (!is_two_hand_aim_weapon_id(get_equip_weapon_id().value_or(-1))) {
        return;
    }

    // Busy-Gruende AUSSER Support-Dock: Hand gehoert jemand anderem.
    if (re4vr::lua_is_truthy("__vr_slide_rack_active")) {
        return;
    }

    if (re4vr::lua_is_truthy("__vr_mag_in_hand")) {
        return;
    }

    if (knife_is_left()) {
        return;
    }

    // Support-Dock gegenlaeufig zur Support-Pose ausblenden.
    const float blend = 1.0f - m_support.blend_factor;

    if (blend <= 0.001f) {
        return;
    }

    apply_pose_runtime("ADAlazypose", blend);
}

// ============================================================================
// apply_flashlight (Lua Z.3975-4102)
// ============================================================================

void RE4VRMotion::apply_flashlight(const glm::vec3& hand_pos, const glm::quat& hand_rot) {
    if (!m_fl_enabled) {
        return;
    }

    if (!fl_find()) {
        return;
    }

    if (fl_scope_update()) {
        return;   // [FL_SCOPE] beim Scopen versteckt -> nicht an die Hand setzen
    }

    // [GRAPPLE 2026-09-08] Beim Niedergerungen-Werden zeigt das Spiel den ganzen
    // Koerper in 3rd Person und animiert ihn selbst. Schreiben wir die Lampe
    // dabei weiter an die VR-Hand, haengt sie sichtbar neben dem Charakter.
    // Also fuer die Dauer des Grapples nicht anfassen -- sie sitzt dann dort,
    // wo das Spiel sie haben will. Danach greift diese Funktion wieder wie
    // vorher und holt sie zurueck in die Hand.
    {
        bool grappled = false;

        if (re4vr::try_call<bool>(re4vr::fc::ctx(), "get_IsInGrappleDamage", grappled)
            && grappled) {
            return;
        }
    }

    const bool docked = fl_left_hand_busy();

    // [FL_FLIP] Fuer binding: hat die linke Hand was Besseres zu tun, gehoert LT
    // NICHT dem Lampen-Flip -- sonst schluckt der Tap den DPAD-Shift, waehrend
    // die Lampe unsichtbar am HMD haengt.
    re4vr::lua_set_bool("__re4_fl_hand_busy", docked);

    const FlOffset* active_fl_offset = &m_fl_offset;
    const FlOffset* active_light_offset = &m_fl_light_offset;
    glm::vec3 base_pos = hand_pos;
    glm::quat base_rot = hand_rot;

    if (docked) {
        // [SUPPORT] Kegel ans HMD, Lampen-Mesh aus.
        active_fl_offset = &m_fl_state.docked_offset;
        active_light_offset = &m_fl_state.docked_light_offset;

        glm::vec3 cpos{};
        glm::quat crot{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_flashlight_camera(cpos, crot)) {
            base_pos = cpos;
            base_rot = crot;
        }

        // [FL_FORCE_OFF] Mesh JEDEN Frame zwangs-aus, nicht nur beim Uebergang.
        // Sonst: laesst man Aim los, waehrend die Support-Hand noch dockt,
        // re-enabled die Engine die FL-Accessory -> das Mesh taucht an der
        // HMD-Position (= am Kopf) wieder auf. Der Kegel bleibt unberuehrt.
        fl_set_mesh_visible(false);
    } else {
        // [FL_FORCE_ON] Solange die linke Hand nichts Besseres tut: Lampe JEDEN
        // Frame zwangs-sichtbar halten. Das Spiel blendet die FL-Accessory beim
        // Aimen selbst aus; wir forcen DrawSelf/UpdateSelf und Mesh-Enable
        // zurueck. Ausnahme: Knife gezogen UND keep_on_knife aus -> der Engine
        // ihr Verstecken lassen.
        if (!(fl_knife_equipped() && !m_fl_keep_on_knife)) {
            if (auto* go = re4vr::call_safe<::REManagedObject*>(m_fl_state.flashlight_tf.obj,
                                                               "get_GameObject");
                go != nullptr) {
                re4vr::call_safe<void*>(go, "set_DrawSelf", true);
                re4vr::call_safe<void*>(go, "set_UpdateSelf", true);
            }

            fl_set_mesh_visible(true);
        }

        // [FL_FLIP] Flip-Lerp ticken. Nur in diesem Zweig -- docked (Lampe am
        // HMD) bleibt unveraendert. ZEITbasiert, weil apply_flashlight zweimal
        // pro Frame laeuft: ein Schritt pro Aufruf wie beim Messer liefe hier je
        // nach Pfad unterschiedlich schnell.
        float ftime = m_fl_state.flip_time;

        if (ftime < 0.01f) {
            ftime = 0.01f;
        }

        const double fnow = clock_now();
        double fdt = fnow - m_fl_state.flip_last_t;
        m_fl_state.flip_last_t = fnow;

        if (fdt < 0.0 || fdt > 0.25) {
            fdt = 0.0;   // Pause/Ladebildschirm: kein Sprung
        }

        const float ftgt = re4vr::lua_get_tribool("__vr_fl_flip") == 1 ? 1.0f : 0.0f;

        // [FL_FLIP SND] Klick bei jedem Tastendruck, hin UND zurueck -- wie beim
        // Messer ueber die Flanke des ZIELS, sonst feuert es in beiden
        // Frame-Paessen. Die Lampe hat KEINEN eigenen SoundContainer (live
        // geprueft: weder ac0000_00 noch sein light-Kind) -> System-Sound ueber
        // den GuiSoundManager. Der Enum-Wert wird EINMAL live aufgeloest statt
        // hartkodiert; nicht gefunden -> der Flip bleibt stumm.
        if (m_fl_state.flip_prev_target != ftgt) {
            m_fl_state.flip_prev_target = ftgt;

            if (!m_fl_snd_resolved) {
                m_fl_snd_resolved = true;
                m_fl_state.flip_snd = re4vr::enum_value("chainsaw.gui.GuiSoundType",
                                                        "CH_GUI_FILE_DECIDE");
            }

            if (m_fl_state.flip_snd.has_value()) {
                if (auto* gsm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.GuiSoundManager");
                    gsm != nullptr) {
                    re4vr::call_safe<void*>(gsm, "wwiseTriggerTarget(chainsaw.gui.GuiSoundType)",
                                            *m_fl_state.flip_snd);
                }
            }
        }

        const float step = static_cast<float>(fdt) / ftime;

        if (m_fl_state.flip_lerp < ftgt) {
            m_fl_state.flip_lerp = std::min(ftgt, m_fl_state.flip_lerp + step);
        } else if (m_fl_state.flip_lerp > ftgt) {
            m_fl_state.flip_lerp = std::max(ftgt, m_fl_state.flip_lerp - step);
        }
    }

    glm::vec3 fl_pos = base_pos + (base_rot * glm::vec3{active_fl_offset->pos_x,
                                                        active_fl_offset->pos_y,
                                                        active_fl_offset->pos_z});
    glm::quat fl_rot = base_rot;

    if (active_fl_offset->rot_pitch != 0.0f || active_fl_offset->rot_yaw != 0.0f
        || active_fl_offset->rot_roll != 0.0f) {
        fl_rot = glm::normalize(base_rot * quat_from_euler_deg(active_fl_offset->rot_pitch,
                                                               active_fl_offset->rot_yaw,
                                                               active_fl_offset->rot_roll));
    }

    // [FL_FLIP] Genau wie knife_flip_spin: die Flip-Drehung wird HINTEN
    // angehaengt (fl_rot * flip), nicht in die Offset-Winkel gerechnet. Der
    // Versatz liegt im Hand-Frame, damit er sich beim Drehen der Hand mitdreht.
    if (m_fl_state.flip_lerp > 0.0001f && !docked) {
        const float ft = m_fl_state.flip_lerp;
        fl_rot = glm::normalize(fl_rot * quat_from_euler_deg(m_fl_state.flip.rot_pitch * ft,
                                                             m_fl_state.flip.rot_yaw * ft,
                                                             m_fl_state.flip.rot_roll * ft));

        if (m_fl_state.flip.pos_x != 0.0f || m_fl_state.flip.pos_y != 0.0f
            || m_fl_state.flip.pos_z != 0.0f) {
            fl_pos += base_rot * glm::vec3{m_fl_state.flip.pos_x * ft, m_fl_state.flip.pos_y * ft,
                                           m_fl_state.flip.pos_z * ft};
        }
    }

    if (m_fl_state.flashlight_tf.obj != nullptr) {
        set_vec4(m_fl_state.flashlight_tf.obj, "set_Position",
                 glm::vec4{fl_pos.x, fl_pos.y, fl_pos.z, 1.0f});
        set_quat(m_fl_state.flashlight_tf.obj, "set_Rotation", fl_rot);
    }

    if (m_fl_state.light_tf.obj != nullptr) {
        const glm::vec3 light_pos = fl_pos + (fl_rot * glm::vec3{active_light_offset->pos_x,
                                                                 active_light_offset->pos_y,
                                                                 active_light_offset->pos_z});
        glm::quat light_rot = fl_rot;

        if (active_light_offset->rot_pitch != 0.0f || active_light_offset->rot_yaw != 0.0f
            || active_light_offset->rot_roll != 0.0f) {
            light_rot = glm::normalize(fl_rot * quat_from_euler_deg(active_light_offset->rot_pitch,
                                                                    active_light_offset->rot_yaw,
                                                                    active_light_offset->rot_roll));
        }

        set_vec4(m_fl_state.light_tf.obj, "set_Position",
                 glm::vec4{light_pos.x, light_pos.y, light_pos.z, 1.0f});
        set_quat(m_fl_state.light_tf.obj, "set_Rotation", light_rot);
    }
}

// ============================================================================
// [ADA_FL] Ada: Kegel IMMER ans HMD (wie Leons docked-Fall).
// KOMPLETT EIGENER PFAD -- Leons fl_find/apply_flashlight bleiben unangetastet.
// Struktur identisch, nur der Body heisst anders: ch3a8z0_body > ac0000_00 >
// light. Ada hat kein FL-Mesh, es gibt also nichts aus- oder einzublenden.
//
// [TEIL2/L6] Drei Folgen dieses Sonderpfads, alle 1:1 uebernommen:
//   1. Er ruft KEIN fl_scope_update -> bei Ada wird die Lampe im Scope NICHT
//      versteckt (Leons Pfad tut das).
//   2. Er wendet KEINEN Flip an.
//   3. Weil fl_find() bei Ada nie erfolgreich laeuft (es sucht ch0a0z0_body),
//      bleibt flashlight_tf leer -> fl_apply_hold_pose und fl_flip_finger_open
//      sind bei Ada dauerhaft No-Ops.
//
// [ZWEITER NACHTRAG V3] __re4_ada_fl haelt Engine-Transform-Handles, die
// on_script_reset im Original NICHT nullt, und validiert nur alle 1,0 s. Als
// C++-Rohzeiger ist das eine Absturzquelle nach Levelwechsel -- hier gehalten
// UND beim Reset freigegeben.
// ============================================================================

bool RE4VRMotion::ada_fl_find() {
    const double now = clock_now();

    if (now - m_ada_fl_last_check < 1.0) {
        return m_ada_fl_tf.obj != nullptr;
    }

    m_ada_fl_last_check = now;

    const auto clear = [this]() {
        drop(m_ada_fl_tf);
        drop(m_ada_fl_light_tf);
    };

    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        clear();
        return false;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{"ch3a8z0_body"}));
    auto* body = str != nullptr
                     ? re4vr::call_safe<::REManagedObject*>(scene, "findGameObject(System.String)", str)
                     : nullptr;

    if (body == nullptr) {
        clear();
        return false;
    }

    auto* btf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    if (btf == nullptr) {
        clear();
        return false;
    }

    auto* fl_tf = find_by_name(btf, L"ac0000_00");

    if (fl_tf == nullptr) {
        clear();
        return false;
    }

    store(m_ada_fl_tf, fl_tf);
    store(m_ada_fl_light_tf, find_by_name(fl_tf, L"light"));
    return true;
}

// Nutzt BEWUSST Leons docked-Offsets -- dieselben Slider wie sein
// Support-Hand-Fall, nur gelesen.
void RE4VRMotion::ada_fl_apply() {
    if (!m_fl_enabled) {
        return;
    }

    if (!ada_fl_find()) {
        return;
    }

    glm::vec3 base_pos{};
    glm::quat base_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_flashlight_camera(base_pos, base_rot)) {
        return;
    }

    const FlOffset& o = m_fl_state.docked_offset;
    const FlOffset& lo = m_fl_state.docked_light_offset;

    const glm::vec3 fl_pos = base_pos + (base_rot * glm::vec3{o.pos_x, o.pos_y, o.pos_z});
    glm::quat fl_rot = base_rot;

    if (o.rot_pitch != 0.0f || o.rot_yaw != 0.0f || o.rot_roll != 0.0f) {
        fl_rot = glm::normalize(base_rot * quat_from_euler_deg(o.rot_pitch, o.rot_yaw, o.rot_roll));
    }

    set_vec4(m_ada_fl_tf.obj, "set_Position", glm::vec4{fl_pos.x, fl_pos.y, fl_pos.z, 1.0f});
    set_quat(m_ada_fl_tf.obj, "set_Rotation", fl_rot);

    if (m_ada_fl_light_tf.obj != nullptr) {
        const glm::vec3 light_pos = fl_pos + (fl_rot * glm::vec3{lo.pos_x, lo.pos_y, lo.pos_z});
        glm::quat light_rot = fl_rot;

        if (lo.rot_pitch != 0.0f || lo.rot_yaw != 0.0f || lo.rot_roll != 0.0f) {
            light_rot = glm::normalize(fl_rot * quat_from_euler_deg(lo.rot_pitch, lo.rot_yaw,
                                                                     lo.rot_roll));
        }

        set_vec4(m_ada_fl_light_tf.obj, "set_Position",
                 glm::vec4{light_pos.x, light_pos.y, light_pos.z, 1.0f});
        set_quat(m_ada_fl_light_tf.obj, "set_Rotation", light_rot);
    }
}

// Dispatch: Ada -> eigener HMD-Pfad, alles andere -> unveraendert Leons Pfad.
void RE4VRMotion::fl_dispatch(const glm::vec3& lh_world, const glm::quat& lh_rot) {
    if (char_now() == "ada") {
        ada_fl_apply();
    } else {
        apply_flashlight(lh_world, lh_rot);
    }
}

// [FL_HOLD_POSE] Solange die Lampe AN ist (ac0000_00 in der Szene) UND die Hand
// nichts Besseres tut: die GECAPTURTE native Lampen-Haltung "fl" forcen -- fuer
// ALLE Waffen UND mit Messer, in aim und non-aim. Sonst kippt die native
// Aim-/Schwung-Anim die linke Hand, die die Lampe haelt. Loest sich unter
// fl_left_hand_busy -> die Aktions-Pose uebernimmt.
void RE4VRMotion::fl_apply_hold_pose() {
    if (!m_fl_enabled) {
        return;
    }

    if (m_fl_state.flashlight_tf.obj == nullptr) {
        return;   // Lampe aus (GO weg) -> keine Pose, sie klebt also nicht
    }

    if (fl_knife_equipped() && !m_fl_keep_on_knife) {
        return;   // Knife + nativ erlaubt
    }

    if (fl_left_hand_busy()) {
        return;
    }

    apply_pose_runtime("fl", 1.0f);
}

// ============================================================================
// Post-Anim-Posen (Lua Z.4204-4330)
//
// Werden im POST-ANIM-Pass angewandt, damit die Finger-Pose die
// Engine-Animation ueberschreibt.
// ============================================================================

// [POSE_FADE] Geteilte Fade-Dauer fuer Insert- UND Rack-Handpose. 0 = hart.
void RE4VRMotion::apply_rack_hand_pose_from_reload() {
    constexpr float POSE_FADE_DUR = 0.10f;   // nur Fallback, falls das Global fehlt
    const std::string name = re4vr::lua_get_string("__vr_rack_hand_pose");
    float blend = 1.0f;

    if (!name.empty()) {
        m_rack_pose_fade.name = name;
        m_rack_pose_fade.release_t.reset();
    } else {
        if (m_rack_pose_fade.name.empty()) {
            return;
        }

        if (!m_rack_pose_fade.release_t.has_value()) {
            m_rack_pose_fade.release_t = clock_now();
        }

        const double el = clock_now() - *m_rack_pose_fade.release_t;
        const double fd = re4vr::lua_get_number("__re4_pose_fade_dur", POSE_FADE_DUR);

        if (fd <= 0.001 || el >= fd) {
            m_rack_pose_fade.name.clear();
            return;
        }

        blend = static_cast<float>(1.0 - (el / fd));
    }

    apply_pose_runtime(m_rack_pose_fade.name, blend);
}

// Additive Rotation auf eine Liste von Finger-Joints -- das Muster teilen sich
// der Switch-Zeigefinger und der Knife-Flip-Oeffner.
void RE4VRMotion::add_local_rotation(::REManagedObject* owner_joint,
                                     const std::vector<std::string>& bones, const glm::quat& add) {
    if (owner_joint == nullptr) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(owner_joint, "get_Owner");

    if (tf == nullptr) {
        return;
    }

    for (const auto& bn : bones) {
        auto* j = joint_by_name(tf, bn.c_str());

        if (j == nullptr) {
            continue;
        }

        glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(j, "get_LocalRotation", cur)) {
            set_quat(j, "set_LocalRotation", glm::normalize(cur * add));
        }
    }
}

// [SWITCH_DOCK] Am Verstell-Schalter gedockt -> eigene Finger-Pose (Faust,
// Zeigefinger und Daumen frei zum filigranen Umstellen). Blend = Dock-
// Fortschritt mit dem zackigeren Switch-Lerp.
void RE4VRMotion::apply_switch_hand_pose() {
    if (!m_support.switch_docked) {
        return;
    }

    apply_pose_runtime("LE5SWITCH", m_support.blend_factor);

    // Additiver Zeigefinger-Curl NACH der Pose (schliesst die "OK"-Geste),
    // per-Waffe getunt. Gleicher Mechanismus wie der Mag-Daumen-Offset, auf
    // alle drei Index-Joints.
    const auto* sw = get_support_offset_switch(m_cache.weapon_key);

    if (sw == nullptr) {
        return;
    }

    if ((sw->idx_rx == 0.0f && sw->idx_ry == 0.0f && sw->idx_rz == 0.0f)
        || m_left_hand.joint.obj == nullptr) {
        return;
    }

    const float hx = glm::radians(sw->idx_rx) * 0.5f;
    const float hy = glm::radians(sw->idx_ry) * 0.5f;
    const float hz = glm::radians(sw->idx_rz) * 0.5f;
    const glm::quat qx{std::cos(hx), std::sin(hx), 0.0f, 0.0f};
    const glm::quat qy{std::cos(hy), 0.0f, std::sin(hy), 0.0f};
    const glm::quat qz{std::cos(hz), 0.0f, 0.0f, std::sin(hz)};

    add_local_rotation(m_left_hand.joint.obj, {"L_IndexF1", "L_IndexF2", "L_IndexF3"},
                       qz * qy * qx);
}

// [KNIFE_FLIP] Waehrend der Flip durchdreht, die Finger kurz OEFFNEN, damit das
// Messer durchpasst -- keine Pose noetig, additive Streck-Rotation auf alle
// Finger-Joints. Betrag = 4*x*(1-x): 0 an beiden Enden (fest gegriffen), Peak
// bei halbem Flip -> automatisch synchron zum Flip-Tempo.
void RE4VRMotion::knife_flip_finger_open() {
    if (re4vr::lua_get_tribool("__re4_knife_equipped") != 1) {
        return;
    }

    // [KNIFE_FLIP LINKS] Die Finger der HAND oeffnen, die das Messer haelt --
    // sonst oeffnete beim Links-Flip faelschlich die rechte. Die L-Bones werden
    // aus den R_-Namen abgeleitet; die L-Flexion nutzt -X (Spiegelung der
    // +X-Streckung rechts).
    const bool is_left = re4vr::lua_get_string("__re4_knife_hand") == "left";
    auto& hand = is_left ? m_left_hand : m_right_hand;

    if (hand.joint.obj == nullptr) {
        return;
    }

    const float lp = m_knife_flip.lerp;
    const float bump = 4.0f * lp * (1.0f - lp);

    if (bump <= 0.001f) {
        return;
    }

    const float deg = static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_finger_deg", -35.0));
    float h = glm::radians(deg * bump) * 0.5f;

    if (is_left) {
        h = -h;
    }

    // Additive Rotation um lokales X (Strecken).
    const glm::quat add{std::cos(h), std::sin(h), 0.0f, 0.0f};

    static const std::vector<std::string> R_FINGERS{
        "R_Thumb1", "R_Thumb2", "R_Thumb3",
        "R_IndexF1", "R_IndexF2", "R_IndexF3",
        "R_MiddleF1", "R_MiddleF2", "R_MiddleF3",
        "R_RingF1", "R_RingF2", "R_RingF3",
        "R_PinkyF1", "R_PinkyF2", "R_PinkyF3",
    };

    std::vector<std::string> bones = R_FINGERS;

    if (is_left) {
        for (auto& b : bones) {
            b[0] = 'L';
        }
    }

    add_local_rotation(hand.joint.obj, bones, add);
}

// [MAG-POSE] Lua Z.4424-4477. ZEITBASIERT, nicht pro Frame: die Funktion laeuft
// mehrfach pro Frame (LockScene + BeginRendering), ein Pro-Frame-Dekrement waere
// passabhaengig zu schnell.
void RE4VRMotion::apply_mag_hand_pose_from_reload() {
    constexpr float POSE_FADE_DUR = 0.10f;
    const std::string name = re4vr::lua_get_string("__vr_mag_hand_pose");
    float blend = 1.0f;

    if (!name.empty()) {
        m_mag_pose_fade.name = name;
        m_mag_pose_fade.trx = static_cast<float>(re4vr::lua_get_number("__vr_mag_hand_trx", 0.0));
        m_mag_pose_fade.try_ = static_cast<float>(re4vr::lua_get_number("__vr_mag_hand_try", 0.0));
        m_mag_pose_fade.trz = static_cast<float>(re4vr::lua_get_number("__vr_mag_hand_trz", 0.0));
        m_mag_pose_fade.release_t.reset();
    } else {
        if (m_mag_pose_fade.name.empty()) {
            return;
        }

        // [POSE_CROSSFADE] Laeuft gerade die Druecken-Pose (reload_adv
        // veroeffentlicht ihren Blend als __re4_push_blend), dann GEGENLAEUFIG
        // dazu ausblenden statt zeitbasiert gegen die native Animation. Grund:
        // diese Funktion schreibt im spaeteren Pass, ueberdeckte also die
        // laengst fertige Push-Pose -- und wenn der Zeit-Fade endete, erschien
        // sie schlagartig. So ergibt sich ein echter Uebergang.
        const auto pb = re4vr::lua_get_number_opt("__re4_push_blend");

        if (pb.has_value() && *pb > 0.0) {
            m_mag_pose_fade.release_t.reset();   // ein spaeterer Zeit-Fade faengt sauber bei 0 an
            blend = static_cast<float>(1.0 - *pb);

            if (blend <= 0.001f) {
                m_mag_pose_fade.name.clear();
                return;
            }
        } else {
            if (!m_mag_pose_fade.release_t.has_value()) {
                m_mag_pose_fade.release_t = clock_now();
            }

            const double el = clock_now() - *m_mag_pose_fade.release_t;
            const double fd = re4vr::lua_get_number("__re4_pose_fade_dur", POSE_FADE_DUR);

            if (fd <= 0.001 || el >= fd) {
                m_mag_pose_fade.name.clear();
                return;
            }

            blend = static_cast<float>(1.0 - (el / fd));
        }
    }

    apply_pose_runtime(m_mag_pose_fade.name, blend);

    // Daumen-Spreizung additiv auf L_Thumb1 NACH der Pose, mit blend skaliert
    // (fadet also mit).
    const float trx = m_mag_pose_fade.trx * blend;
    const float try_ = m_mag_pose_fade.try_ * blend;
    const float trz = m_mag_pose_fade.trz * blend;

    if ((trx != 0.0f || try_ != 0.0f || trz != 0.0f) && m_left_hand.joint.obj != nullptr) {
        const float hx = glm::radians(trx) * 0.5f;
        const float hy = glm::radians(try_) * 0.5f;
        const float hz = glm::radians(trz) * 0.5f;
        const glm::quat qx{std::cos(hx), std::sin(hx), 0.0f, 0.0f};
        const glm::quat qy{std::cos(hy), 0.0f, std::sin(hy), 0.0f};
        const glm::quat qz{std::cos(hz), 0.0f, 0.0f, std::sin(hz)};
        add_local_rotation(m_left_hand.joint.obj, {"L_Thumb1"}, qz * qy * qx);
    }
}

// ============================================================================
// [SKULL_SHAKER_OPEN] Lua Z.4487-4560
//
// Drei Zustaende, Reihenfolge = Vorrang:
//  1. Flick-Fenster (die Waffe dreht sich) -> Dreh-Pose. Frueher lag hier
//     NICHTS: die Handbewegung nach dem Schuss war die native AfterShoot-Anim
//     (Body-Layer 5, gemessen), nicht unsere.
//  2. break_open (Nachladen per X) -> Dreieck-Puls, unveraendert.
//  3. sonst, solange eine Break-Action in der Hand ist -> Ruhepose
//     "skull-default", die die native Fingeranim nach dem Schuss ueberstimmt.
//
// [SKULL_SPIN_ENTFERNT] Hier lag apply_skullshaker_cock_pose (Griffpose +
// Finger-Spreizung waehrend der Cock-Drehung). Zusammen mit der Drehung
// komplett ausgebaut; die Hebel-Auf-Pose bleibt.
// ============================================================================

void RE4VRMotion::apply_skullshaker_open_pose() {
    constexpr float SKULLSHAKER_OPEN_DUR = 0.2f;   // Snap-Haltezeit, KEIN Lerp beim Reingehen
    constexpr float SKULLSHAKER_OPEN_REL = 0.15f;  // Release: weiches Zurueck-Lerp

    const bool open = re4vr::lua_get_tribool("__vr_break_open") == 1;
    const double now = clock_now();

    if (open != m_skullopen.prev) {   // JEDE Flanke = Hebel geht auf ODER zu
        m_skullopen.start_t = now;
    }

    m_skullopen.prev = open;

    // Gate: reload.lua frischt __re4_break_wid_active jeden Frame als
    // Zeitstempel auf, solange eine Break-Action LIVE in der Hand ist. Alt =
    // Waffe weg (Wechsel, Save-Load, bare hands) -> wir schreiben nichts mehr
    // und die Hand ist sofort wieder nativ.
    const auto bw = re4vr::lua_get_number_opt("__re4_break_wid_active");
    const bool has = bw.has_value() && (now - *bw) < 0.3;

    if (!has) {
        m_skullopen.start_t = -1.0;
        m_skullopen.prev = false;
        return;
    }

    if (re4vr::lua_get_tribool("__vr_pump_anim_active") == 1) {
        m_skullopen.start_t = -1.0;   // kein Nachlade-Puls waehrend der Drehung
        const std::string p = re4vr::lua_get_string("__re4_skull_spin_pose");
        apply_pose_runtime(p.empty() ? "skullbreak" : p, 1.0f);
        return;
    }

    if (m_skullopen.start_t < 0.0) {
        const std::string p = re4vr::lua_get_string("__re4_skull_idle_pose");
        apply_pose_runtime(p.empty() ? "skull-default" : p, 1.0f);

        // [SKULL_INDEX] Zeigefinger zusaetzlich beugen, ADDITIV auf die Pose,
        // damit die Pose-DATEN in re4_vr_reload.json unangetastet bleiben. Die
        // rechte Hand beugt um +Z.
        if (m_right_hand.joint.obj != nullptr) {
            static const char* IDX[] = {"R_IndexF1", "R_IndexF2", "R_IndexF3"};

            for (int i = 0; i < 3; ++i) {
                const std::string key = "__re4_skull_idx" + std::to_string(i + 1);
                const double d = re4vr::lua_get_number(key.c_str(), 0.0);

                if (d != 0.0) {
                    add_local_rotation(m_right_hand.joint.obj, {IDX[i]},
                                       quat_from_euler_deg(0.0f, 0.0f, static_cast<float>(d)));
                }
            }
        }

        return;
    }

    const double dt = now - m_skullopen.start_t;

    if (dt >= SKULLSHAKER_OPEN_DUR + SKULLSHAKER_OPEN_REL) {
        m_skullopen.start_t = -1.0;   // vorbei -> nichts schreiben -> original
        return;
    }

    float blend = 1.0f;

    if (dt > SKULLSHAKER_OPEN_DUR) {
        // Nur der RUECKgang lerpt; rein geht es per Snap.
        blend = 1.0f - static_cast<float>((dt - SKULLSHAKER_OPEN_DUR) / SKULLSHAKER_OPEN_REL);
    }

    apply_pose_runtime("skullbreak", blend);
}

// [BOLT_IDLE_POSE] Dasselbe Muster wie skull-default, fuer die Bolt-Rifle: die
// Engine fuehrt nach JEDEM Schuss kurz ihre eigene Fingeranim aus (kosmetischer
// Bolt-Cycle). Solange die Waffe LIVE in der Hand ist, halten wir die rechte
// Hand auf "Bolt-Default". Gate = Zeitstempel aus dem Bolt-Block in reload2;
// wird er alt, schreiben wir nichts mehr -> Hand sofort nativ.
void RE4VRMotion::apply_bolt_idle_pose() {
    const auto ts = re4vr::lua_get_number_opt("__re4_bolt_wid_active");

    if (!ts.has_value()) {
        return;
    }

    if ((clock_now() - *ts) >= 0.3) {
        return;
    }

    const std::string p = re4vr::lua_get_string("__re4_bolt_idle_pose");
    apply_pose_runtime(p.empty() ? "Bolt-Default" : p, 1.0f);
}

// ============================================================================
// [KNIFE_SWING] Lua Z.4589-4706 -- 1:1-Port aus RE9 `update_axe_swing`.
//
// Gemessen wird die ROHE Controller-Position, NICHT die geglaettete rh_world:
// Smoothing und Weapon-Offset verfaelschen die Velocity.
// ============================================================================

void RE4VRMotion::update_knife_swing() {
    const double now = clock_now();
    auto* vr = VR::get().get();

    const auto reset_all = [this]() {
        re4vr::lua_set_bool("vr_knife_swing", false);
        m_knife_swing.velocity = 0.0f;
        m_knife_swing.last_wp.reset();
    };

    // [KNIFE_FLIP] Melee im Reverse-Grip erlaubt -- AUSSER bei sichtbarem
    // Finisher-Prompt: dann hat die Shake-Finisher-Geste Vorrang (Shake VOR
    // Melee), kein Stich. Ohne Prompt liefe das Shake ohnehin ins Leere.
    // last_wp nullen -> kein Velocity-Spike, wenn der Prompt verschwindet.
    if (re4vr::lua_get_tribool("__vr_knife_flip") == 1
        && re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false)) {
        reset_all();
        return;
    }

    if (re4vr::lua_is_truthy("vr_knife_swing") && now >= m_knife_swing.swing_end) {
        re4vr::lua_set_bool("vr_knife_swing", false);
    }

    if (vr == nullptr || !vr->is_hmd_active()) {
        m_knife_swing.velocity = 0.0f;
        m_knife_swing.last_wp.reset();
        return;
    }

    const auto& controllers = vr->get_controllers();

    if (controllers.size() < 2) {
        m_knife_swing.velocity = 0.0f;
        return;
    }

    // [KNIFE_HAND] Velocity von der Hand, die das Messer HAELT.
    const int cidx = re4vr::lua_get_string("__re4_knife_hand") == "left" ? 0 : 1;

    // Wechselt die aktive Hand (z.B. Draw links: none -> left), ist last_wp noch
    // von der ANDEREN Hand -> riesiger FALSCHER Velocity-Spike -> Phantom-Swing
    // (Melee-Sound beim Greifen). Bei Wechsel zuruecksetzen.
    if (m_knife_swing.last_cidx.has_value() && *m_knife_swing.last_cidx != cidx) {
        m_knife_swing.last_wp.reset();
        m_knife_swing.velocity = 0.0f;
        // Cooldown-Fenster: auch die Greif-Restbewegung nach dem Draw feuert nicht.
        m_knife_swing.last_swing = now;
    }

    m_knife_swing.last_cidx = cidx;

    const auto wp4 = vr->get_position(controllers[cidx]);
    const glm::vec3 wp{wp4.x, wp4.y, wp4.z};

    // Lua bricht hier ab, wenn get_position nichts liefert. Nativ gibt es den
    // nil-Fall nicht -- ein Nullvektor waere aber ein riesiger falscher Sprung,
    // also derselbe Ausstieg.
    if (wp.x == 0.0f && wp.y == 0.0f && wp.z == 0.0f) {
        m_knife_swing.velocity = 0.0f;
        m_knife_swing.last_wp.reset();
        return;
    }

    const double dt = now - m_knife_swing.last_time;

    if (m_knife_swing.last_wp.has_value() && dt > 0.001 && dt < 0.2) {
        const glm::vec3 d = wp - *m_knife_swing.last_wp;
        const float horiz = std::sqrt(d.x * d.x + d.z * d.z);

        if (d.y > 0.0f && d.y > horiz) {
            m_knife_swing.velocity = 0.0f;   // reines Anheben zaehlt nicht
        } else {
            m_knife_swing.velocity =
                static_cast<float>(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / dt);
        }

        // [RICHTUNG] Frueher wurde dieser Vektor weggeworfen und nur sein BETRAG
        // exportiert. Der Choke musste die Richtung darum aus einer 40-300 ms
        // alten Handhistorie schaetzen -- bei schnellem Vor-und-Zurueck steckt
        // darin noch die Hinbewegung, und das Zurueckziehen zaehlte als Stich.
        // Hier ist die Richtung exakt die des Ausloese-Frames.
        m_knife_swing.d = d;
    } else {
        m_knife_swing.velocity = 0.0f;
    }

    m_knife_swing.last_wp = wp;
    m_knife_swing.last_time = now;

    // [PEAK] Solange das Schwung-Fenster steht, das MAXIMUM mitschreiben.
    // vr_knife_velocity ist nur der Moment, in dem die Schwelle gerissen wurde
    // -- die eigentliche Wucht des Stichs kommt erst in den Frames danach. Der
    // Choke unterscheidet daran sanft/kraeftig.
    if (re4vr::lua_is_truthy("vr_knife_swing") && m_knife_swing.velocity > m_knife_swing.peak) {
        m_knife_swing.peak = m_knife_swing.velocity;
        re4vr::lua_set_number("vr_knife_velocity_peak", m_knife_swing.peak);
    }

    // cache.weapon_key ist ein String -> tonumber. Nicht-numerisch ("none")
    // liefert in Lua nil und entwaffnet damit.
    int32_t wid = 0;
    bool wid_ok = false;

    try {
        size_t used = 0;
        wid = std::stoi(m_cache.weapon_key, &used);
        wid_ok = used == m_cache.weapon_key.size();
    } catch (...) {
        wid_ok = false;
    }

    const bool armed = wid_ok && is_knife_rel_split_id(wid) && !m_support.docked;

    // [CHOKE-SCHWELLE] Waehrend eines Choke-Griffs darf eine EIGENE, niedrigere
    // Schwelle gelten: dort soll auch ein sanfter Stich ankommen, damit choke an
    // der Wucht entscheiden kann, ob das Messer steckenbleibt. Ausserhalb des
    // Griffs aendert sich NICHTS.
    // __re4_choke_seen ist der Herzschlag, den choke im Griff jeden Frame setzt:
    // stirbt das Script mitten im Griff, verfaellt die Sonderschwelle nach 0,2 s
    // von selbst -- ein Flag koennte haengenbleiben.
    float swing_thr = static_cast<float>(
        re4vr::lua_get_number("__re4_knife_swing_threshold", m_knife_swing.threshold));
    const auto choke_thr = re4vr::lua_get_number_opt("__re4_choke_swing_threshold");
    const auto choke_seen = re4vr::lua_get_number_opt("__re4_choke_seen");

    if (choke_thr.has_value() && *choke_thr > 0.0 && choke_seen.has_value()
        && (now - *choke_seen) < 0.2) {
        swing_thr = static_cast<float>(*choke_thr);
    }

    if (armed && m_knife_swing.velocity >= swing_thr) {
        if (now - m_knife_swing.last_swing >= m_knife_swing.cooldown) {
            re4vr::lua_set_bool("vr_knife_swing", true);
            re4vr::lua_set_number("vr_knife_velocity", m_knife_swing.velocity);

            // [RICHTUNG] Der normierte Bewegungsvektor GENAU dieses Frames.
            const glm::vec3 s = m_knife_swing.d;
            const float sl = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);

            if (sl > 1e-6f) {
                re4vr::lua_set_vec3_table("vr_knife_swing_dir", s / sl);
            } else {
                re4vr::lua_set_nil("vr_knife_swing_dir");
            }

            re4vr::lua_set_number("__vr_knife_swing_t", now);

            // [PEAK] Fenster beginnt hier: Startwert = der Ausloesewert selbst.
            m_knife_swing.peak = m_knife_swing.velocity;
            re4vr::lua_set_number("vr_knife_velocity_peak", m_knife_swing.peak);

            m_knife_swing.swing_end = now + m_knife_swing.hold_time;
            m_knife_swing.last_swing = now;
        }
    }
}

// [FL_FLIP] Beim Durchdrehen der Lampe die Finger der LINKEN Hand kurz oeffnen
// -- dasselbe Muster wie beim Messer, nur gespiegelt (-X statt +X).
void RE4VRMotion::fl_flip_finger_open() {
    if (!m_fl_enabled) {
        return;
    }

    if (m_fl_state.flashlight_tf.obj == nullptr) {
        return;   // Lampe aus -> nichts
    }

    if (fl_left_hand_busy()) {
        return;
    }

    const float lp = m_fl_state.flip_lerp;
    const float bump = 4.0f * lp * (1.0f - lp);

    if (bump <= 0.001f) {
        return;
    }

    if (m_left_hand.joint.obj == nullptr) {
        return;
    }

    const float h = -glm::radians(m_fl_state.flip_finger_deg * bump) * 0.5f;
    const glm::quat add{std::cos(h), std::sin(h), 0.0f, 0.0f};

    add_local_rotation(m_left_hand.joint.obj,
                       {"L_Thumb1", "L_Thumb2", "L_Thumb3",
                        "L_IndexF1", "L_IndexF2", "L_IndexF3",
                        "L_MiddleF1", "L_MiddleF2", "L_MiddleF3",
                        "L_RingF1", "L_RingF2", "L_RingF3",
                        "L_PinkyF1", "L_PinkyF2", "L_PinkyF3"},
                       add);
}

// ============================================================================
// [PISTOL_SUPPORT] Lua Z.4330-4360
//
// Solange die linke Hand am Schaft/Griff gedockt ist, die gecapturte
// Finger-Pose auf sie schreiben. Blend = Dock-Fortschritt. Gilt fuer ALLE
// einhaendig gehaltenen Support-Hand-Waffen -> nur die Zweihand-/Foregrip-
// Waffen (Group B) bekommen sie NICHT, ausser mit ausdruecklichem Override.
// Reihenfolge: laeuft VOR Rack/Mag/Switch-Pose, die haben beim Reload Vorrang.
// ============================================================================

void RE4VRMotion::apply_pistol_support_pose() {
    if (!m_wep_cache.id.has_value() || !is_support_hand_weapon()) {
        return;
    }

    if (m_support.blend_factor <= 0.0f) {
        return;   // nur waehrend und solange gedockt
    }

    const int32_t wid = *m_wep_cache.id;
    const char* pose = nullptr;

    // [SUPPORT_POSE PER WAFFE] Gesetzt = gilt AUCH fuer Grip-Dock-Waffen.
    // [ADArocket] Der SW Rocket Launcher hatte ohne Aim eine voellig
    // danebenliegende Pose, weil er Group B ist und ohne Override GAR KEINE
    // Pose bekommt -> es lief die native Anim.
    switch (wid) {
    case 4200:
        pose = "TMPSUpport";   // TMP: eigene Vordergriff-Pose
        break;
    case 6106:
    case 6111:
        pose = "ADArocket";    // SW Rocket Launcher (beide dasselbe Modell)
        break;
    default:
        break;
    }

    if (pose == nullptr) {
        if (is_grip_dock_weapon(wid)) {
            return;   // Zweihand ohne Override -> keine Pose
        }

        pose = "pose1";
    }

    apply_pose_runtime(pose, m_support.blend_factor);
}

// ============================================================================
// [SWITCH_DOCK] Feuerwahl-Hebel drehen (Lua Z.4389-4412)
//
// Nur die LocalRotation wird ueberschrieben -> der Hebel dreht um seinen
// eigenen Pivot, die Position bleibt. Additiv auf die Engine-Anim im
// Post-Anim-Pass, solange die Waffe equippt ist (unabhaengig vom Hand-Dock).
// ============================================================================

void RE4VRMotion::apply_switch_rotation() {
    if (m_wep_cache.tf.obj == nullptr || !m_wep_cache.id.has_value()
        || !is_switch_dock_weapon(*m_wep_cache.id)) {
        return;
    }

    const auto* sw = get_support_offset_switch(m_cache.weapon_key);

    if (sw == nullptr) {
        return;
    }

    // [FIRE_MODE] Zielwinkel je Stellung: Full = 0, Burst = burst_rot,
    // Single = single_rot. Die Previews zeigen den jeweiligen Tuning-Winkel.
    float target = 0.0f;

    if (m_support.fire_mode == 2 || m_support.single_preview) {
        target = sw->single_rot;
    } else if (m_support.fire_mode == 1 || m_support.burst_preview
               || m_support.switch2_preview || m_support.switch2_aim_preview) {
        target = sw->burst_rot;
    }

    m_support.burst_anim += (target - m_support.burst_anim) * sw->lever_lerp;

    if (std::fabs(m_support.burst_anim - target) < 0.05f) {
        m_support.burst_anim = target;
    }

    const float deg = m_support.burst_anim;

    if (std::fabs(deg) < 0.05f) {
        return;
    }

    auto* j = get_switch_joint();

    if (j == nullptr) {
        return;
    }

    glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(j, "get_LocalRotation", cur)) {
        return;
    }

    const float h = glm::radians(deg) * 0.5f;
    const glm::quat add{std::cos(h), std::sin(h), 0.0f, 0.0f};   // lokale X-Achse (Pitch)
    set_quat(j, "set_LocalRotation", glm::normalize(cur * add));
}

// ============================================================================
// [ELEVATOR] elevator_unparent (Lua Z.4711-4784)
//
// Aufzug 2 parentet den Player jeden Frame an seine Hierarchie -> der
// VR-Playspace faehrt mit = Clipping. Loesung: jeden Frame den Body vom Aufzug
// UN-parenten, wenn eine GmElevator-Komponente in der Parent-Kette haengt.
// Laeuft VOR dem Killswitch-Ausstieg.
//
// [ELEVATOR6] Positiver Nachweis per KOMPONENTE statt Namensraten: Aufzug
// gm81_501_00_0 heisst weder エレベータ noch リフト -- der Namenstest war dort
// blind, es gab kein Unparent, und Player, Ashley und Plattform blieben optisch
// stehen. getComponent trifft JEDEN Aufzug, auch kuenftige, und nimmt
// Unterklassen mit. Der Namenstest bleibt als Fallback.
// ============================================================================

void RE4VRMotion::elevator_unparent() {
    // Default aus, jeden Frame frisch (kein stale). Unten true, sobald der
    // Aufzug-Parent erkannt ist -> stabiler Trigger fuer den Killswitch.
    re4vr::lua_set_bool("__re4_on_elevator2", false);

    auto* ctx = re4vr::player_context();

    if (ctx == nullptr) {
        return;
    }

    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body == nullptr) {
        return;
    }

    auto* btf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    if (btf == nullptr) {
        return;
    }

    if (m_t_elevator == nullptr) {
        m_t_elevator = re4vr::runtime_type("chainsaw.GmElevator");
    }

    auto* t = re4vr::call_safe<::REManagedObject*>(btf, "get_Parent");
    bool on_elevator = false;

    for (int i = 0; i < 5; ++i) {
        if (t == nullptr) {
            break;
        }

        if (auto* go = re4vr::call_safe<::REManagedObject*>(t, "get_GameObject"); go != nullptr) {
            if (m_t_elevator != nullptr
                && re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)",
                                                        m_t_elevator) != nullptr) {
                on_elevator = true;
                break;
            }

            // Namenstest als Fallback: greift die Komponente mal nicht,
            // verhalten sich エレベータ/リフト wie bisher.
            const std::string nm = re4vr::obj_name(go);

            if (nm.find("\xE3\x82\xA8\xE3\x83\xAC\xE3\x83\x99\xE3\x83\xBC\xE3\x82\xBF") != std::string::npos
                || nm.find("\xE3\x83\xAA\xE3\x83\x95\xE3\x83\x88") != std::string::npos) {
                on_elevator = true;
                break;
            }
        }

        t = re4vr::call_safe<::REManagedObject*>(t, "get_Parent");
    }

    if (!on_elevator) {
        return;
    }

    // Flag VOR dem Unparent setzen: der Killswitch liest dieses Flag, nicht die
    // Parent-Kette, die wir gleich wegraeumen -> kein Race.
    re4vr::lua_set_bool("__re4_on_elevator2", true);

    // [STILLZONE-AUSNAHME -- GEMESSEN WIRKUNGSLOS, bleibt als Notiz]
    // Verdacht war: die Gondel wird als Aufzug erkannt und wir reissen den
    // Spieler jeden Frame heraus. WIDERLEGT -- die Spur zeigte in Stage 60850
    // durchgehend on_elevator = false, GmCargoGondola2 ist also KEINE
    // GmElevator-Unterklasse. Die Zeile feuert dort nie, bleibt aber stehen:
    // sie kostet nichts und schuetzt, falls je ein Gimmick in der Zone doch als
    // Aufzug zaehlt. Der echte Ausloeser war die Arbeit, die motion im
    // Killswitch-Zweig weiterlaufen liess.
    if (re4vr::lua_get_tribool("__re4_stillzone_motion") == 1) {
        return;
    }

    // set_Parent auf einer stale Transform ist ein Absturz -> get_Valid davor.
    if (re4vr::call_safe<bool>(btf, "get_Valid") == true) {
        re4vr::call_safe<void*>(btf, "set_Parent", nullptr);
    }
}

// ============================================================================
// [STILLZONE] Lua Z.5000-5360
//
// Ortszone plus Killswitch-Zuender, die motion, movement und holster in der
// Gondel wirklich stilllegen. "Alle Scripte aus" reicht nicht -- die
// KS-ZWEIGE der Dateien arbeiten weiter (motion: restore_hands_native,
// arm_chain: publish_clamp_anchors). Erst der harte Ausstieg legt sie still.
//
// Die Zone ist nur der AUSLOESER; gehalten wird ueber das ParentGimmick-Bit im
// PlayerDefine.State (Bit 53, per Modulo getestet wie im Killswitch), weil die
// Kabine einen sofort aus dem Radius traegt.
// ============================================================================


// [CAM-FLANKE] Der Einstieg endet dort, wo der CamState von Gimmick auf
// BattleNormal umspringt -- ab da ist wieder normales Gameplay.
std::optional<int32_t> RE4VRMotion::stillzone_cam() {
    auto* csys = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CameraSystem");
    auto* main = csys != nullptr
                     ? re4vr::call_safe<::REManagedObject*>(csys, "get_MainCameraController")
                     : nullptr;
    auto* busy = main != nullptr
                     ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
                     : nullptr;

    if (busy == nullptr) {
        return std::nullopt;
    }

    // _CurrentStateParam ist ein FELD, kein Getter.
    auto* sp = re4vr::get_field_object(busy, "_CurrentStateParam");

    if (sp == nullptr) {
        return std::nullopt;
    }

    return re4vr::get_field_int(sp, "<State>k__BackingField");
}

// Enum-Ints einmalig aufloesen. Kommt nichts zurueck (TDB noch nicht bereit),
// wird es beim naechsten Frame neu versucht -- NICHT cachen, sonst bleibt die
// Flanke fuer die ganze Sitzung tot.
bool RE4VRMotion::stillzone_camints(int32_t& gimmick, int32_t& battle_normal) {
    if (m_sz_cam_ints_ok) {
        gimmick = m_sz_cam_gimmick;
        battle_normal = m_sz_cam_battle;
        return true;
    }

    const auto g = re4vr::enum_value("chainsaw.CameraDefine.PlayerCameraState", "Gimmick");
    const auto b = re4vr::enum_value("chainsaw.CameraDefine.PlayerCameraState", "BattleNormal");

    if (!g.has_value() || !b.has_value()) {
        return false;
    }

    m_sz_cam_gimmick = *g;
    m_sz_cam_battle = *b;
    m_sz_cam_ints_ok = true;
    gimmick = *g;
    battle_normal = *b;
    return true;
}

// [ROHER KILLSWITCH] Bewusst NICHT is_killswitch_active() dieser Klasse: die
// meldet in der Zone selbst true -- das waere ein Kreisschluss, der Halt koennte
// nie enden.
bool RE4VRMotion::stillzone_ks() {
    return re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false);
}

// Einmal pro Spiel-Frame, als ERSTES im LockScene-Einstieg (vor dem harten
// Ausstieg und vor allen Schreibphasen). Alle anderen Stellen lesen nur die
// Ergebnis-Flags.
void RE4VRMotion::stillzone_tick() {
    bool drin = false;
    bool am_gimmick = false;

    if (re4vr::lua_get_tribool("__re4_stillzone_zones_aus") != 1) {
        auto* ctx = re4vr::player_context();
        std::optional<int32_t> stage;

        if (ctx != nullptr) {
            stage = re4vr::call_safe<int32_t>(ctx, "get_CurrentStageID");

            // Haengt der Spieler an einem Gimmick? Nur das haelt die Stille
            // ueber die Fahrt. Bit 53, per Modulo wie im Killswitch.
            if (const auto st = re4vr::get_state_bits(ctx); st.has_value()) {
                constexpr uint64_t B54 = 18014398509481984ull;   // 2^54
                constexpr uint64_t B53 = 9007199254740992ull;    // 2^53
                am_gimmick = (*st % B54) >= B53;
            }
        }

        if (stage.has_value() && ctx != nullptr) {
            auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
            auto* tf = body != nullptr
                           ? re4vr::call_safe<::REManagedObject*>(body, "get_Transform")
                           : nullptr;
            glm::vec3 pos{};

            if (tf != nullptr && get_vec3(tf, "get_Position", pos)) {
                // [ADA -- ORTSZONE] Genau EIN Eintrag. Der Killswitch-Zuender
                // allein reicht bei Ada NICHT: sie hat keinen Knopfdruck, man
                // LAEUFT in den Einstieg hinein. Der ParentGimmick-Zustand steht
                // damit erst, wenn die Gondel den Charakter schon geprueft hat
                // -- zu spaet, die Kabine haengt. Eine Ortszone steht dagegen
                // fest, BEVOR man in die Naehe kommt.
                // Mittelpunkt = Sitzplatz (Zonen-Spur 87.51/20.95/12.99), r 2.5
                // deckt den Anlaufpunkt mit ab.
                // [LEONS ORTSZONE RAUS] Er zuendet ueber den Tastendruck; ein
                // Ortspunkt legte ihn schon beim Herumstehen davor stlll.
                constexpr int32_t Z_STAGE = 60850;
                constexpr float Z_X = 87.51f, Z_Y = 20.95f, Z_Z = 12.99f, Z_R = 2.5f;

                if (*stage == Z_STAGE) {
                    const float dx = pos.x - Z_X;
                    const float dy = pos.y - Z_Y;
                    const float dz = pos.z - Z_Z;
                    const float d2 = dx * dx + dy * dy + dz * dz;

                    if (d2 <= Z_R * Z_R) {
                        drin = true;
                    }

                }
            }
        }
    }

    // [LEON] Zweiter Zuender: der Killswitch setzt __re4_gondola_active jeden
    // Frame frisch und erkennt die Gondel ueber den GimmickType, nicht ueber
    // Stage/Position -- praeziser geht es nicht.
    if (re4vr::lua_get_tribool("__re4_gondola_active") == 1) {
        drin = true;
    }

    // [ADA] Zweiter Killswitch-Zuender, gleiches Muster; ersetzt die fruehere
    // zusaetzliche Ortszone.
    if (re4vr::lua_get_tribool("__re4_gondola_ada_active") == 1) {
        drin = true;
    }

    // Zone betreten -> halten (Phase A). Der Halt endet erst, wenn wir weder in
    // der Zone noch am Gimmick sind -- also beim Aussteigen. Die CamState-Flanke
    // beendet NICHT den Halt, sie schaltet nur auf Phase B um.
    if (drin) {
        m_sz_halt = true;
    }

    const bool ks_now = stillzone_ks();

    if (m_sz_halt) {
        if (ks_now) {
            m_sz_ks_seen = true;
        }

        if (!m_sz_phase_b && re4vr::lua_get_tribool("__re4_stillzone_camflanke_aus") != 1) {
            int32_t gimmick = 0;
            int32_t battle = 0;

            if (stillzone_camints(gimmick, battle)) {
                if (const auto cam = stillzone_cam(); cam.has_value()) {
                    // REIHENFOLGE IST PFLICHT: erst muss Gimmick GESEHEN worden
                    // sein, dann zaehlt BattleNormal -- sonst waere Phase B
                    // schon vor dem Einsteigen erreicht.
                    if (*cam == gimmick) {
                        m_sz_gimmick_seen = true;
                    }

                    if (m_sz_gimmick_seen && *cam == battle) {
                        m_sz_phase_b = true;
                    }
                }
            }

            // [KS-WEG] Zweiter Weg in Phase B, fuer Zonen, die ueber den
            // Killswitch zuenden (Leon): dort gibt es keine
            // Gimmick->BattleNormal-Flanke, der Start-KS laeuft einfach ab.
            if (!m_sz_phase_b && m_sz_ks_seen && !ks_now) {
                m_sz_phase_b = true;
            }
        }

        // Der Halt endet erst, wenn NICHTS mehr traegt: weder Zone, noch
        // Gimmick-Parenting, noch Killswitch. Das dritte Kriterium ist fuer Leon
        // noetig -- ob seine Gondel den Spieler parentet, ist ungemessen.
        if (!drin && !am_gimmick && !ks_now) {
            m_sz_halt = false;
            m_sz_phase_b = false;
            m_sz_gimmick_seen = false;
            m_sz_ks_seen = false;
        }
    }

    // Ergebnis pro Datei: in Phase A sind alle drei still, in Phase B nur die,
    // die nicht freigegeben sind.
    //
    // [ENDSTAND] ALLE DREI bleiben die GANZE FAHRT still. Gemessen wurde in
    // dieser Reihenfolge, jeder Lauf fror die Kabine an derselben Stelle ein:
    //   motion = true     + holster = true -> Freeze
    //   motion = "haende" + holster = true -> Freeze (der Render-Pass reicht schon)
    // Weil ohne motion kein Controller-Tracking existiert, wird die Fahrt
    // stattdessen ertraeglich gemacht (Meshes aus, unverwundbar).
    // Die drei Rueckgabe-Werte bleiben als Schalter erhalten, falls es je wieder
    // gebraucht wird: motion kennt false / "haende" / true.
    const bool halt = m_sz_halt;
    const bool b = halt && m_sz_phase_b;

    re4vr::lua_set_bool("__re4_stillzone_motion", halt && !(b && m_sz_back_motion == 2));
    re4vr::lua_set_bool("__re4_stillzone_render", halt && !(b && m_sz_back_motion >= 1));
    re4vr::lua_set_bool("__re4_stillzone_movement", halt && !(b && m_sz_back_movement));
    re4vr::lua_set_bool("__re4_stillzone_holster", halt && !(b && m_sz_back_holster));

    // [MESHES] Der Koerper darf ERST verschwinden, wenn der Einstiegs-
    // Killswitch durch ist -- also in Phase B, nicht schon beim Betreten der
    // Zone. Waehrend der Kamerafahrt sieht man sich selbst einsteigen, das soll
    // so bleiben.
    // [LEONS GONDEL] Bei ihm ist die Kamera waehrend der Fahrt nicht an den Kopf
    // gekoppelt; der Versuch, dafuer movement in einer eigenen Phase B
    // zurueckzugeben, brachte nichts. Stattdessen derselbe Weg wie bei Ada.
    // FENSTER: __re4_gondola_active -- es steht NUR waehrend seiner Fahrt.
    // Bewusst NICHT an der Ortszone, sonst waere Leon schon unsichtbar, waehrend
    // er noch davorsteht.
    re4vr::lua_set_bool("__re4_stillzone_hide_now",
                        (halt && b) || (halt && re4vr::lua_get_tribool("__re4_gondola_active") == 1));


    m_sz_prev = halt;

    // [GONDEL-KOMFORT] Die Meshes haengen am HALT (materials liest
    // __re4_stillzone_motion). Die Unverwundbarkeit ist nach RE4VRObjects
    // gewandert und wird dort am eigenen Zonen-Zustand gefuehrt -- hier steht
    // bewusst kein Aufruf mehr, sonst gaebe es zwei Schreiber fuer dieselben
    // HitPoint-Flags.
}

// ============================================================================
// Flashlight-Config (Lua Z.3693-3725) -- eigene Datei, eigener Lade-/Speicherweg
// (json.load_file/dump_file, NICHT der Read-Modify-Write der motion-JSON).
// ============================================================================

void RE4VRMotion::fl_load_cfg() {
    const auto d = re4vr::json_load(FL_CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    // Lua: `if d.enabled ~= nil then fl_enabled = (d.enabled == true) end` --
    // jeder Nicht-true-Wert schaltet AUS, nicht nur false.
    if (d.contains("enabled") && !d["enabled"].is_null()) {
        m_fl_enabled = d["enabled"].is_boolean() && d["enabled"].get<bool>();
    }

    if (d.contains("keep_on_knife") && !d["keep_on_knife"].is_null()) {
        m_fl_keep_on_knife = d["keep_on_knife"].is_boolean() && d["keep_on_knife"].get<bool>();
    }

    if (j_is_str(d, "knife_pose")) {
        m_fl_knife_pose = d["knife_pose"].get<std::string>();
    }

    const auto load_off = [](FlOffset& dst, const nlohmann::json& src) {
        if (!src.is_object()) {
            return;
        }

        num_into(dst.pos_x, src, "pos_x");
        num_into(dst.pos_y, src, "pos_y");
        num_into(dst.pos_z, src, "pos_z");
        num_into(dst.rot_pitch, src, "rot_pitch");
        num_into(dst.rot_yaw, src, "rot_yaw");
        num_into(dst.rot_roll, src, "rot_roll");
    };

    const auto sub = [&d](const char* k) {
        return d.contains(k) ? d[k] : nlohmann::json{};
    };

    load_off(m_fl_offset, sub("flashlight"));
    load_off(m_fl_light_offset, sub("light"));
    load_off(m_fl_state.docked_offset, sub("docked"));
    load_off(m_fl_state.docked_light_offset, sub("docked_light"));
    load_off(m_fl_state.flip, sub("flip"));   // [FL_FLIP]
}

void RE4VRMotion::fl_save_cfg() {
    const auto dump = [](const FlOffset& o) {
        return nlohmann::json{
            {"pos_x", o.pos_x}, {"pos_y", o.pos_y}, {"pos_z", o.pos_z},
            {"rot_pitch", o.rot_pitch}, {"rot_yaw", o.rot_yaw}, {"rot_roll", o.rot_roll},
        };
    };

    nlohmann::json j;
    j["enabled"] = m_fl_enabled;
    j["keep_on_knife"] = m_fl_keep_on_knife;
    // [TEIL2/L5] knife_pose wird geladen, gespeichert und in der UI editiert --
    // aber NIRGENDS gelesen. Funktional tot, wandert 1:1 mit.
    j["knife_pose"] = m_fl_knife_pose;
    j["flashlight"] = dump(m_fl_offset);
    j["light"] = dump(m_fl_light_offset);
    j["docked"] = dump(m_fl_state.docked_offset);
    j["docked_light"] = dump(m_fl_state.docked_light_offset);
    j["flip"] = dump(m_fl_state.flip);

    re4vr::json_save(FL_CFG_PATH, j);
}

// ============================================================================
// tick (Lua Z.4786-4991) -- Herzstueck
//
// do_weapon_sample == false bedeutet LockScene-Pass (dort laeuft der
// Dock-Update und der Knife-Swing genau 1x pro Frame); true bedeutet
// LateUpdateBehavior, wo die Kalibrierung sampelt.
// ============================================================================

void RE4VRMotion::tick(bool do_weapon_sample) {
    // [ELEVATOR] jeden Frame vom Aufzug loesen -- VOR allen Ausstiegen.
    elevator_unparent();

    // [NATIVE_ANIM] Red9-Reload: motion aus und Hand-Ziele loeschen -> native
    // Arme. Ein stale Hand-Ziel hielte die Arme sonst steif.
    if (native_reload_active()) {
        re4vr::lua_set_string("__re4_pin_why", "tick:native_reload");
        release_motion_targets();
        m_smoothing.right_pos.reset();
        m_smoothing.right_rot.reset();
        m_smoothing.left_pos.reset();
        m_smoothing.left_rot.reset();
        return;
    }

    if (is_killswitch_active()) {
        re4vr::lua_set_string("__re4_pin_why", "tick:killswitch");

        // [KNIFE_FLIP KS] Messer nativ -- kein Griff-Stich im Finisher.
        knife_ks_restore_native();


        // [FL_SCOPE] Lampe beim Scopen verstecken; der tick ist hier sonst aus.
        fl_scope_update();

        // [NATIVE_ARMS] Wie im native_reload-Zweig: publizierte Hand-Ziele
        // NILEN, sonst haelt arm_chain die Arme an der letzten VR-Position
        // steif. Red9-Lehre: nil Hand-Ziel = native Arme.
        release_motion_targets();
        m_smoothing.right_pos.reset();
        m_smoothing.right_rot.reset();
        m_smoothing.left_pos.reset();
        m_smoothing.left_rot.reset();

        // [KILLSWITCH_RESTORE] verbogene Hand-Local-Position auf Bind zurueck.
        restore_hands_native();

        return;
    }

    // [FLIP_RESTORE] Der KS ist vorbei -> war das Messer VOR dem KS umgedreht,
    // wieder umdrehen. Damit ueberlebt der Reverse-Grip Treffer, Stagger und
    // Cutscene; vorher musste man nach jedem Treffer neu flippen.
    // SOFORT, KEIN LERP: nicht nur das Global setzen, sondern den Lerp direkt
    // auf die Endstellung -- sonst dreht sich das Messer nach JEDEM Stagger
    // sichtbar zurueck. prev_target = 1.0 unterdrueckt zugleich den Flip-Sound,
    // denn knife_flip_spin spielt ihn auf der Ziel-FLANKE.
    if (re4vr::lua_get_tribool("__re4_knife_flip_pre_ks") == 1) {
        re4vr::lua_set_nil("__re4_knife_flip_pre_ks");

        if (re4vr::lua_get_tribool("__re4_knife_equipped") == 1) {
            re4vr::lua_set_bool("__vr_knife_flip", true);
            m_knife_flip.lerp = 1.0f;
            m_knife_flip.prev_target = 1.0f;
        }
    }

    // Init-Gate: warten, bis die Runtime Controller liefert.
    if (!m_init.initialized) {
        ++m_init.frame_counter;
        auto* vr = VR::get().get();

        if (vr != nullptr && vr->get_controllers().size() >= 2) {
            m_init.initialized = true;
            drop(m_right_hand.joint);
            drop(m_left_hand.joint);
            m_cache.standing_origin_set = false;

            m_vr_runtime = vr->is_openxr_loaded() ? "openxr" : "openvr";

            // controller_type von binding uebernehmen, falls dort geaendert.
            load_config();
        }

        return;
    }

    glm::vec3 cam_pos{};
    glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_camera_data(cam_pos, cam_rot)) {
        return;
    }

    VrData vr_data{};

    if (!get_vr_data(vr_data)) {
        return;
    }

    // Standing-Origin pro Frame nachziehen (Recenter-Support).
    if (auto* vr = VR::get().get(); vr != nullptr) {
        m_cache.standing_origin = vr->get_standing_origin();
        m_cache.standing_origin_set = true;
    }

    find_joints();
    find_weapon();

    // Kalibrierung nur im LateUpdateBehavior-Pass weiterzaehlen -- dort ist die
    // native Verkettung Hand<->Waffe konsistent.
    if (do_weapon_sample) {
        update_weapon_calibration();
    }

    m_cache.weapon_key = current_weapon_key();

    // [MATILDA_STOCK] Setzt nur ein FLAG; die HAND bleibt unveraendert und nutzt
    // weiter den 4004-Offset, der Stock-Versatz wirkt WAFFE-ONLY in attach_weapon.
    m_cache.matilda_stock = (m_cache.weapon_key == "4004") && stock_mounted_data();

    const bool is_lock_pass = !do_weapon_sample;

    m_cache.rh_world.reset();
    m_cache.rh_rot.reset();
    m_cache.rh_aim_rot.reset();
    m_cache.lh_world.reset();
    m_cache.lh_rot.reset();
    m_cache.rh_joint_pos.reset();
    m_cache.rh_joint_rot.reset();
    m_cache.lh_joint_pos.reset();
    m_cache.lh_joint_rot.reset();

    // Waffenwechsel: Draw-/Holster-Anim nativ zeigen (rechte Hand frei).
    // Kalibrier-Fenster: rechte Hand und Waffe der Engine ueberlassen.
    bool changing = false;

    if (do_weapon_sample) {
        changing = update_weapon_changing_gate();

        // Waehrend des Wechsels kontinuierlich samplen (alles nativ ->
        // konsistentes Paar; der letzte Sample ist die fertige Hand+Waffe).
        if (changing && m_wep_cache.tf.obj != nullptr && !m_wep_cache.frozen) {
            sample_weapon_rel_direct();
            m_wep_cache.calib.reset();
            m_wep_cache.settle.reset();
        } else if (!changing) {
            update_weapon_settle();
        }

    } else {
        changing = m_was_weapon_changing;
    }

    // [WSW_PIN] Ist der Waffenwechsel-Skip aktiv (weapons setzt das Flag), Hand
    // und Waffe AUCH waehrend des Wechsels an den Controller pinnen -> keine
    // sichtbare native Draw/Holster-Anim. Das Sampling oben lief schon (liest
    // nativ VOR dem Pin), die Kalibrierung bleibt also korrekt.
    // NUR waehrend des aktiven Wechsels pinnen -- die Settle-/Kalibrierphase
    // NICHT, dort sampelt motion den Offset nativ.
    const bool pin_during_change = re4vr::lua_get_tribool("__vr_wsw_pin") == 1;
    bool did_left = false;

    if ((!changing && !weapon_calib_suspends_hand()) || (pin_during_change && changing)) {
        attach_right_hand(cam_pos, cam_rot, vr_data);

        // [KNIFE_HAND] Messer LINKS: die linke Hand VOR dem Waffen-Pin frisch
        // berechnen, sonst pinnt attach_weapon mit 1-Frame-alten lh-Daten und
        // das Messer schleudert beim Rotieren weg.
        if (re4vr::lua_get_string("__re4_knife_hand") == "left") {
            attach_left_hand(cam_pos, cam_rot, vr_data, is_lock_pass);
            did_left = true;
        }

        attach_weapon();
    }

    // [SUPPORT_HAND] Dock-State nur im LockScene-Pass updaten (1x pro Frame);
    // wenn oben schon fuers Messer gelaufen -> ueberspringen.
    if (!did_left) {
        attach_left_hand(cam_pos, cam_rot, vr_data, is_lock_pass);
    }

    // [FLASHLIGHT] FL-GO direkt nach der linken Hand.
    if (m_cache.lh_world.has_value()) {
        fl_dispatch(*m_cache.lh_world,
                    m_cache.lh_rot.value_or(glm::quat{1.0f, 0.0f, 0.0f, 0.0f}));
    }

    // Pose-Block -- NUR im Post-Anim-Pass. Reihenfolge ist tragend: die
    // Ada-Lazy-Pose laeuft als ERSTES, alles danach ueberschreibt sie mit
    // blend 1.0 und kann ihr damit nicht ins Gehege kommen.
    if (!is_lock_pass) {
        apply_ada_lazy_pose();
        fl_apply_hold_pose();
        apply_pistol_support_pose();
        apply_rack_hand_pose_from_reload();
        apply_mag_hand_pose_from_reload();
        apply_switch_hand_pose();
        apply_switch_rotation();
        apply_skullshaker_open_pose();
        apply_bolt_idle_pose();
        knife_flip_finger_open();
        fl_flip_finger_open();

        // Fremd-Posen aus anderen Scripten -- existieren sie nicht, sind die
        // Aufrufe stille No-Ops (lua_call_global prueft auf Funktion).
        re4vr::lua_call_global("__re4_wildwest_fingers");
        re4vr::lua_call_global("__re4_apply_left_knife_pose");
        re4vr::lua_call_global("__re4_merc_apply_bow_pose");
        re4vr::lua_call_global("__re4_apply_bowknob_pose");
    }

    // [KNIFE_SWING] Velocity nur 1x pro Frame (LockScene-Pass), nach rh_world.
    if (is_lock_pass) {
        update_knife_swing();
    }

    publish_globals();
}

// ============================================================================
// Cross-Script-Globals (Lua Z.4947-4990)
//
// [ZWEITER NACHTRAG V7] Diese Werte MUESSEN nach Lua publiziert werden -- neun
// davon lesen bereits C++-Module aus _G (ArmChain, Holster, Crosshair, Recoil).
// Ein nicht gesetzter Wert wird GENULLT, nicht ausgelassen: die Leser
// unterscheiden "kein Ziel" von "altes Ziel".
// ============================================================================

void RE4VRMotion::publish_globals() {
    const auto pub_v3 = [](const char* n, const std::optional<glm::vec3>& v) {
        if (v.has_value()) {
            re4vr::lua_set_vec3(n, *v);
        } else {
            re4vr::lua_set_nil(n);
        }
    };

    const auto pub_q = [](const char* n, const std::optional<glm::quat>& q) {
        if (q.has_value()) {
            re4vr::lua_set_quat(n, *q);
        } else {
            re4vr::lua_set_nil(n);
        }
    };

    pub_v3("__vr_rh_world", m_cache.rh_world);
    pub_q("__vr_rh_rot", m_cache.rh_rot);
    pub_q("__vr_rh_aim_rot", m_cache.rh_aim_rot);
    pub_v3("__vr_lh_world", m_cache.lh_world);
    pub_q("__vr_lh_rot", m_cache.lh_rot);

    re4vr::lua_set_bool("__vr_support_hand_docked", m_support.docked);
    re4vr::lua_set_number("__vr_support_blend_factor", m_support.blend_factor);

    // [KNIFE_LEFT_NO_SUPPORT] Jeden Frame frisch, nie stale: "diese Waffe ist
    // eine Einhandwaffe UND links liegt ein Messer". weapons2 laesst daraufhin
    // seinen Support-Kill des Links-Klons aus -- noetig, weil der Klon im
    // SELBEN Frame entsteht, in dem die Hand oben noch als gedockt galt.
    const bool knife_block = m_wep_cache.id.has_value()
                             && is_knife_no_support_id(*m_wep_cache.id) && knife_is_left();
    re4vr::lua_set_bool("__vr_support_knife_block", knife_block);

    // [DIAG] Reine Sichtbarmachung des Schalter-Zustands -- keine Logik.
    // __vr_dbg_wep_id wird von binding und merc wirklich gelesen.
    re4vr::lua_set_number("__vr_dbg_fire_mode", m_support.fire_mode);
    re4vr::lua_set_bool("__vr_dbg_switch_docked", m_support.switch_docked);

    if (m_wep_cache.id.has_value()) {
        re4vr::lua_set_number("__vr_dbg_wep_id", *m_wep_cache.id);
    } else {
        re4vr::lua_set_nil("__vr_dbg_wep_id");
    }

    // [BURST] Burst-Status fuer binding (RT-Begrenzung). NUR fuer die
    // Schalter-Waffen mit Hebel auf Burst, sonst false -- sonst traefe der
    // Burst-Block andere Waffen.
    {
        const bool le5 = m_wep_cache.id.has_value() && is_switch_dock_weapon(*m_wep_cache.id)
                         && m_support.burst_active;

        if (le5) {
            re4vr::lua_set_bool("__vr_burst_active", true);
            const auto* swc = get_support_offset_switch(m_cache.weapon_key);

            // Single (fire_mode == 2) -> nach 1 Schuss blocken; Burst -> Slider.
            if (m_support.fire_mode == 2) {
                re4vr::lua_set_number("__vr_burst_count", 1.0);
            } else {
                re4vr::lua_set_number("__vr_burst_count", swc != nullptr ? swc->burst_count : 3.0);
            }

            re4vr::lua_set_bool("__vr_motion_owns_burst", true);
        } else if (re4vr::lua_is_truthy("__vr_motion_owns_burst")) {
            // NUR das von motion gesetzte true zuruecknehmen; sonst das Flag
            // NICHT anfassen -- reload2/CQBR-Burst darf es besitzen, sonst
            // flackert es und binding blockt RT faelschlich.
            re4vr::lua_set_bool("__vr_burst_active", false);
            re4vr::lua_set_bool("__vr_motion_owns_burst", false);
        }
    }

    pub_v3("__vr_rh_joint_pos", m_cache.rh_joint_pos);
    pub_q("__vr_rh_joint_rot", m_cache.rh_joint_rot);
    pub_v3("__vr_lh_joint_pos", m_cache.lh_joint_pos);
    pub_q("__vr_lh_joint_rot", m_cache.lh_joint_rot);
}

std::optional<std::string> RE4VRMotion::on_initialize() {
    // [KEIN CONFIG-LADEN HIER -- Review-Fund 04.09.2026, KRITISCH]
    // Mods::on_initialize laeuft auf dem Init-Thread; der Lua-State entsteht
    // erst spaeter im on_frame des ScriptRunners. re4vr::LuaRef liefert hier
    // also nullptr, und das haette drei Schaeden angerichtet:
    //   (a) ada_knife_relfix haette seine Marker nie lesen koennen -> Adas
    //       Messer waere bei JEDEM Spielstart erneut um 180 Grad gedreht
    //       worden, also abwechselnd richtig und verkehrt herum -- samt des
    //       toten Wurfs, gegen den die Migration ueberhaupt gebaut wurde;
    //   (b) save_config haette alle Lua-eigenen Skalare mit ihren
    //       Compile-Defaults ueberschrieben (Schwung-Schwelle, Fade-Dauer,
    //       Wurf-Werte, Flip-Tempo ...) -- getuntes Verhalten waere nach dem
    //       ersten Start weg gewesen;
    //   (c) saemtliche lua_set_*-Aufrufe in load_config waeren verpufft.
    // Das Original laedt im DATEIRUMPF, also zwangslaeufig mit lebendem State.
    // Der entsprechende Platz ist hier on_lua_state_created.
    return Mod::on_initialize();
}

void RE4VRMotion::on_lua_state_created(sol::state& lua) {
    // ========================================================================
    // Exporte, die VOR allen Lua-Scripten stehen muessen. on_lua_state_created
    // laeuft vor dem Datei-Ladeloop des ScriptRunners -- genau der Platz, an
    // dem im Original der Dateirumpf seine Globals setzt.
    // ========================================================================

    // [ZWEITER NACHTRAG V8] Eine von zwei Funktions-Globals mit echter
    // Fremd-Schnittstelle: gelesen von binding, reload_adv, weapons2 und von
    // RE4VRCrosshair. weapons2 haelt eine or-geschuetzte, byte-gleiche
    // Zweitdefinition -- im Lua gewinnt, wer zuerst laedt. Weil wir vor jedem
    // Script stehen, laesst deren or-Guard unsere Fassung stehen.
    lua["__re4_char_now"] = []() -> std::string {
        auto mo = RE4VRMotion::get();
        return mo != nullptr ? mo->char_now() : std::string{};
    };

    // Die vier Log-Rueempfe (Lua Z.8-11), geteilt mit re4_vr_reload4_dlc.lua.
    // [KEIN LOG IM FORK 16.09.2026] Sie bleiben als FUNKTIONEN erhalten -- ein Lua,
    // das sie ruft, darf nicht an "attempt to call nil" sterben --, tun aber nichts
    // mehr: keine Ausgabe, auch keine Abfrage von __re4_logging_on.
    const auto log_shim = [](sol::variadic_args) {};

    lua["__re4_log_info"] = log_shim;
    lua["__re4_log_warn"] = log_shim;
    lua["__re4_log_debug"] = log_shim;
    lua["__re4_log_error"] = log_shim;

    // Default-Globals aus dem Dateirumpf. or-Semantik: nur setzen, wenn noch
    // nichts da ist -- sie ueberleben im Original einen Script-Reset.
    re4vr::lua_ensure_table("__re4_ks4fade");
    re4vr::lua_seed_table_number("__re4_ks4fade", "dur", 0.35);

    re4vr::lua_ensure_table("__re4_knife_fly_cfg");
    re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "speed", 12.0);
    re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "gravity", 7.0);
    re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "spin", 18.0);
    re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "max_time", 1.5);
    re4vr::lua_seed_table_number("__re4_knife_fly_cfg", "return_delay", 0.4);

    re4vr::lua_ensure_table("__re4_knife_land_rot");
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "rx", 1.5708);
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "ry", 0.0);
    re4vr::lua_seed_table_number("__re4_knife_land_rot", "rz", 0.0);

    if (!re4vr::lua_get_number_opt("__re4_pose_fade_dur").has_value()) {
        re4vr::lua_set_number("__re4_pose_fade_dur", 0.10);
    }

    if (!re4vr::lua_get_number_opt("__re4_knife_swing_threshold").has_value()) {
        re4vr::lua_set_number("__re4_knife_swing_threshold", m_knife_swing.threshold);
    }

    // Die Zeichenfunktion des Public-Menues -- der Dispatcher haelt sie als
    // Lua-Funktion, deshalb muss sie dort liegen.
    lua["__re4_motion_public_draw"] = []() {
        if (auto mo = RE4VRMotion::get(); mo != nullptr) {
            mo->draw_public_headset();
        }
    };

    m_public_ui_registered = false;
    m_dispatcher_present = false;

    // ========================================================================
    // Erst JETZT die Config laden -- hier ist der Lua-State garantiert da.
    // Reihenfolge wie im Dateirumpf des Originals: Defaults (oben), dann
    // load_config (ueberschreibt sie aus der JSON), dann die Migrationen.
    // Laeuft auch bei jedem "Reset Scripts" erneut -- genau wie das
    // Neu-Ausfuehren der Lua-Datei.
    // ========================================================================
    load_config();
    fl_load_cfg();
    ada_knife_relfix();
}

// [PUBLIC-UI] Ohne Tree im nackten Hauptmenue: die erkannte Runtime (damit der
// Spieler sieht, ob OpenVR oder OpenXR laeuft) und die Headset-Auswahl. Genau
// dieser Block bleibt beim Release erhalten, waehrend die Dev-Trees fliegen --
// und er ist das Einzige, was der VR-Spieler im Headset bedienen kann.
void RE4VRMotion::draw_public_headset() {
    // [FARBEN 10.09.2026] Nur die BESCHRIFTUNGEN blau (dieselbe Farbe wie die
    // aktiven Auswahl-Knoepfe unten), die erkannten Werte bleiben weiss --
    // deshalb vier Aufrufe mit SameLine statt einer Format-Zeile.
    //
    // [UEBERSCHRIFTEN 11.09.2026] "Status" ist die oberste Ueberschrift der
    // Kategorie "Mod Options" -- mittig, rot, groesser, Absatz darunter
    // (REFramework::draw_menu_heading). Alle weiteren bekommen den Absatz
    // zusaetzlich DAVOR.
    g_framework->draw_menu_heading("Status");

    // [LINKSBUENDIG 11.09.2026] Die Zeile stand mittig -- jetzt linksbuendig wie
    // die Ueberschrift.
    // [ROT 11.09.2026] Beschriftungen knallrot statt blau, wie die Haken im Menue.
    ImGui::TextColored(REFramework::MENU_CHECKMARK_COLOR, "Runtime:");
    ImGui::SameLine();
    ImGui::TextUnformatted(m_vr_runtime.c_str());
    ImGui::SameLine();
    ImGui::TextColored(REFramework::MENU_CHECKMARK_COLOR, "  Controller:");
    ImGui::SameLine();
    ImGui::TextUnformatted(m_selected_controller.c_str());

    // [RECENTER 20.09.2026] Kopien der beiden Knoepfe aus dem VR-Tree, der im
    // Public-UI ausgeblendet ist (VR.cpp:5042/5046) -- dieselben Aufrufe, hier
    // nebeneinander direkt unter Runtime/Controller. Ohne geladene Runtime
    // zeichnen sie nichts (wie VR::draw_recenter_button).
    if (auto& vr = VR::get(); vr != nullptr && vr->get_runtime() != nullptr
        && vr->get_runtime()->loaded) {
        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(6.0f)));

        if (ImGui::Button("Recenter View")) {
            vr->recenter_view();
        }

        ImGui::SameLine(0.0f, g_framework->menu_px(REFramework::MENU_BUTTON_GAP));

        if (ImGui::Button("Set Standing Origin")) {
            vr->set_standing_origin(vr->get_position(0));
        }
    }

    g_framework->draw_menu_heading("Select Headset", true);   // [UEBERSCHRIFT 11.09.2026] war orangerot, linksbuendig

    struct Opt {
        const char* label;
        const char* value;
    };

    static const Opt hs[2] = {{"SteamVR", "steamvr"}, {"MetaVR", "metavr"}};

    // [LINKSBUENDIG 11.09.2026] Beide nebeneinander linksbuendig (war mittig),
    // mit MENU_BUTTON_GAP dazwischen.
    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt die Wahl, die blaue Schrift der aktiven Wahl entfaellt.
    const auto button_gap = g_framework->menu_px(REFramework::MENU_BUTTON_GAP);

    for (int i = 0; i < 2; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, button_gap);
        }

        if (g_framework->draw_menu_radio((std::string{hs[i].label} + "##public").c_str(),
                                         m_selected_controller == hs[i].value)) {
            if (m_selected_controller != hs[i].value) {
                m_selected_controller = hs[i].value;
                save_config();
            }
        }
    }
}

// [TRAEGE ANMELDUNG] on_lua_state_created laeuft VOR dem Laden der
// Autorun-Scripte, und ##re4_vr_menu.lua leert __re4_ui_entries bei seinem
// Start -- deshalb pro Frame nachsehen. Bewusste Abweichung: das Original
// meldet einmalig beim Laden an und faellt sonst auf ein eigenes on_draw_ui
// zurueck. Muster 1:1 aus RE4VRRecoil.
void RE4VRMotion::ensure_public_ui_registered() {
    if (m_public_ui_registered) {
        return;
    }

    auto lua = re4vr::LuaRef{};

    if (lua == nullptr) {
        return;
    }

    sol::object add = (*lua)["__re4_ui_add"];

    if (!add.valid() || add.get_type() != sol::type::function) {
        m_dispatcher_present = false;
        return;
    }

    sol::object entries = (*lua)["__re4_ui_entries"];

    if (!entries.valid() || entries.get_type() != sol::type::table) {
        // Ohne Eintragsliste ist der Dispatcher unbrauchbar -> eigener Fallback.
        m_dispatcher_present = false;
        return;
    }

    m_dispatcher_present = true;

    if (entries.as<sol::table>()["motion_headset"].valid()) {
        m_public_ui_registered = true;
        return;
    }

    try {
        auto fn = add.as<sol::protected_function>();
        // Platz 10 = ganz oben.
        auto r = fn(10, "motion_headset", (*lua)["__re4_motion_public_draw"]);

        // protected_function WIRFT nicht -- das Ergebnis muss geprueft werden,
        // sonst gilt die Anmeldung als geglueckt und der Block ist bis zum
        // naechsten Reset nirgends zu sehen.
        if (r.valid()) {
            m_public_ui_registered = true;
        }
    } catch (...) {
    }
}

void RE4VRMotion::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    // Typen einmal aufloesen -- im Original passiert das im Dateirumpf.
    if (m_t_motion == nullptr) {
        m_t_motion = re4vr::runtime_type("via.motion.Motion");
    }

    if (m_t_motion_fsm2 == nullptr) {
        m_t_motion_fsm2 = re4vr::runtime_type("via.motion.MotionFsm2");
    }

    if (m_t_snd == nullptr) {
        m_t_snd = re4vr::runtime_type("soundlib.SoundContainer");
    }

    if (m_t_mesh == nullptr) {
        m_t_mesh = re4vr::runtime_type("via.render.Mesh");
    }

    ensure_public_ui_registered();
}

void RE4VRMotion::on_lua_state_destroyed(sol::state& lua) {
    // ========================================================================
    // Gegenstueck zu re.on_script_reset (Lua Z.6266-6295) -- die 27
    // Zuweisungen dort, plus die Handles, die das Original NICHT raeumt.
    //
    // [ZWEITER NACHTRAG V3] fl_state.light_comp, fl_state.scope_hidden und die
    // ganze __re4_ada_fl-Tabelle ueberleben im Original den Reset, obwohl das
    // Nullen der uebrigen fl_state-Handles ausdruecklich als Save-Load-Schutz
    // gedacht ist. Als ref-counteter Lua-Wert war das folgenlos; als
    // C++-Rohzeiger ist es eine Absturzquelle nach Levelwechsel. Sie werden
    // hier deshalb mit freigegeben -- die einzige bewusste Abweichung in diesem
    // Block, und sie kann kein Verhalten aendern, weil beide ohnehin bei jedem
    // Re-Find neu aufgeloest werden.
    // ========================================================================
    drop(m_right_hand.joint);
    drop(m_left_hand.joint);
    drop(m_body_cache.go);
    drop(m_body_cache.transform);

    m_wep_cache.id.reset();
    drop(m_wep_cache.go);
    drop(m_wep_cache.tf);
    m_wep_cache.rel_pos.reset();
    m_wep_cache.rel_rot.reset();
    m_wep_cache.calib.reset();
    m_wep_cache.settle.reset();

    m_cache.standing_origin_set = false;

    m_smoothing.right_pos.reset();
    m_smoothing.right_rot.reset();
    m_smoothing.left_pos.reset();
    m_smoothing.left_rot.reset();

    m_init.initialized = false;
    m_init.frame_counter = 0;

    m_support.docked = false;
    m_support.force_dock = false;
    m_support.blend_factor = 0.0f;
    m_support.target_blend = 0.0f;
    m_support.aim_blend = 0.0f;
    m_support.switch2_blend = 0.0f;

    // [FLASHLIGHT] Cache invalidieren (der Body kann gewechselt haben).
    drop(m_fl_state.flashlight_tf);
    drop(m_fl_state.light_tf);
    drop(m_fl_state.body_tf);
    drop(m_fl_state.flashlight_mesh);
    m_fl_state.mesh_hidden = false;
    m_fl_state.last_check = 0.0;

    // --- ab hier: was das Original stehen laesst, der Port aber freigeben MUSS ---
    drop(m_fl_state.light_comp);
    m_fl_light_comp_searched = false;
    drop(m_ada_fl_tf);
    drop(m_ada_fl_light_tf);

    // Weitere Handles, die es in Lua gar nicht als Handle gab (dort ueber die
    // Modultabelle bzw. den State gehalten) -- ohne Freigabe waeren es Lecks.
    m_wep_cache.stock_t = -1.0;
    m_wep_cache.stock_on = false;
    drop(m_nr.go);
    drop(m_nr.comp);
    drop(m_character_manager);
    m_support.pump_jn.clear();

    m_t_motion = nullptr;
    m_t_snd = nullptr;
    m_t_motion_fsm2 = nullptr;
    m_t_mesh = nullptr;
    m_t_elevator = nullptr;
}

void RE4VRMotion::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr || std::string_view{name} != "LockScene") {
        return;
    }

    // ZUERST: setzt die Stillzone-Flags fuer diesen Frame.
    stillzone_tick();

    if (re4vr::lua_get_tribool("__re4_stillzone_motion") == 1) {
        return;
    }

    tick(false);
}

void RE4VRMotion::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr) {
        return;
    }

    const std::string_view n{name};

    if (n == "LateUpdateBehavior") {
        if (re4vr::lua_get_tribool("__re4_stillzone_motion") == 1) {
            return;
        }

        tick(true);
        return;
    }

    if (n == "BeginRendering") {
        on_begin_rendering();
        return;
    }

    if (n == "UpdateJointExpression") {
        on_update_joint_expression();
        return;
    }
}

// Der Render-Pass: schlanker als der Tick -- kein Cache-Reset, kein
// Weapon-Key, keine Kalibrierung.
void RE4VRMotion::on_begin_rendering() {
    if (re4vr::lua_get_tribool("__re4_stillzone_render") == 1) {
        return;
    }

    if (!m_init.initialized) {
        return;
    }

    // [NATIVE_ANIM] Red9-Reload: motion aus, Ziele loeschen.
    if (native_reload_active()) {
        release_motion_targets();
        return;
    }

    // [KILLSWITCH_RESTORE] + Messer nativ (kein Griff-Stich).
    if (is_killswitch_active()) {
        knife_ks_restore_native();
        restore_hands_native();
        return;
    }

    glm::vec3 cam_pos{};
    glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};
    VrData vr_data{};

    if (!get_camera_data(cam_pos, cam_rot) || !get_vr_data(vr_data)) {
        return;
    }

    bool did_left = false;

    if ((!m_was_weapon_changing && !weapon_calib_suspends_hand())
        || (re4vr::lua_get_tribool("__vr_wsw_pin") == 1 && m_was_weapon_changing)) {
        attach_right_hand(cam_pos, cam_rot, vr_data);

        // [KNIFE_HAND] Messer LINKS: linke Hand VOR dem Pin frisch -- gleiche
        // Lektion wie im Haupt-Tick.
        if (re4vr::lua_get_string("__re4_knife_hand") == "left") {
            attach_left_hand(cam_pos, cam_rot, vr_data, false);
            did_left = true;
        }

        attach_weapon();
    }

    if (!did_left) {
        attach_left_hand(cam_pos, cam_rot, vr_data, false);
    }

    if (m_cache.lh_world.has_value()) {
        fl_dispatch(*m_cache.lh_world, m_cache.lh_rot.value_or(glm::quat{1.0f, 0.0f, 0.0f, 0.0f}));
    }

    // [BESTAETIGT] Die Pose-Listen sind NICHT identisch: hier fehlen gegenueber
    // dem tick-Block apply_ada_lazy_pose und __re4_wildwest_fingers.
    fl_apply_hold_pose();
    apply_pistol_support_pose();
    apply_rack_hand_pose_from_reload();
    apply_mag_hand_pose_from_reload();
    apply_switch_hand_pose();
    apply_switch_rotation();
    apply_skullshaker_open_pose();
    apply_bolt_idle_pose();
    knife_flip_finger_open();
    fl_flip_finger_open();

    re4vr::lua_call_global("__re4_apply_left_knife_pose");

    // [MERC_BOW_POSE] Muss HIER stehen (POST-Pass), sonst ueberschreibt die
    // Engine-Anim die Finger jeden Frame.
    re4vr::lua_call_global("__re4_merc_apply_bow_pose");

    // [BOW_DRAW] Handpose fuer den von Hand gespannten Bolt Thrower. Muss an
    // BEIDEN Pose-Stellen stehen (hier und im tick-Pass).
    re4vr::lua_call_global("__re4_apply_bowknob_pose");
}

// ============================================================================
// [MERC_BOW_LATE] Lua Z.5424-5442
//
// Der Hook-Stack endet bei BeginRendering(post) -- im LETZTEN Pass bewegt die
// Engine die Hand noch einmal, die Waffe nicht mehr: genau der Nachlauf, wegen
// dem der Bogen der rechten Hand hinterhertrailte. Deshalb hier ein schlankes
// Nachziehen ganz am Ende, KEIN voller Tick (kein Cache-Reset, kein Weapon-Key).
//
// HART GEGATET auf Mercenaries + Compound Bow: fuer Leon, Ada und jede andere
// Waffe ist das ein sofortiger Ausstieg -- an der Kampagne aendert sich nichts.
// ============================================================================

void RE4VRMotion::on_update_joint_expression() {
    if (re4vr::lua_get_tribool("__re4_stillzone_render") == 1) {
        return;
    }

    // [NACHZIEHEN 2026-08-05 / erweitert 07.09.2026]
    // Die gemessene Pass-Reihenfolge endet mit UpdateJointExpression. In diesem
    // LETZTEN Pass bewegt die Engine die HAND noch einmal, die Waffe aber nicht
    // mehr -- die Waffe laeuft der Hand also nach. Fuer Krausers Compound Bow
    // (6304) war das schon 2026-08-05 belegt ("der Bogen trailed der rechten
    // Hand hinterher") und mit diesem schlanken Nachzieh-Pass geloest.
    //
    // Dasselbe trifft das MESSER in der rechten Hand: der User sieht es beim
    // Drehen nachlaufen, das linke Messer nicht -- letzteres ist ein Klon an der
    // linken Hand und folgt ihr ohnehin. Das Gate deckt jetzt beide Faelle ab.
    // Alles andere bleibt wie bisher ein sofortiger Return; faellt eines der
    // beiden je aus dem Tritt, reicht es, seinen Zweig hier zu streichen.
    {
        const bool mercs_bow =
            re4vr::lua_get_tribool("__re4_in_mercs") == 1
            && static_cast<int32_t>(re4vr::lua_get_number("__vr_dbg_wep_id", -1.0)) == 6304;

        const bool knife_right =
            re4vr::lua_get_tribool("__re4_knife_equipped") == 1
            && re4vr::lua_get_string("__re4_knife_hand") != "left";

        if (!mercs_bow && !knife_right) {
            return;
        }
    }

    if (!m_init.initialized) {
        return;
    }

    if (native_reload_active()) {
        return;
    }

    if (is_killswitch_active()) {
        return;
    }

    if (m_was_weapon_changing && re4vr::lua_get_tribool("__vr_wsw_pin") != 1) {
        return;
    }

    if (weapon_calib_suspends_hand()) {
        return;
    }

    glm::vec3 cam_pos{};
    glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};
    VrData vr_data{};

    if (!get_camera_data(cam_pos, cam_rot) || !get_vr_data(vr_data)) {
        return;
    }

    attach_right_hand(cam_pos, cam_rot, vr_data);
    attach_weapon();

    // [REPIN_LEFT] Dieser Pass bewegt die Waffe OHNE attach_left_hand -> die
    // linke Hand bliebe auf dem vorherigen Stand stehen und zittert beim Laufen.
    repin_left();
}

// ============================================================================
// [DEV-UI] Lua Z.5485-6264
//
// Beim Release fliegen die Dev-Bloecke raus, der Public-Block oben bleibt.
// ============================================================================

namespace {
// Luas imgui-Farben sind ImU32 im Format ABGR.
ImVec4 col_abgr(uint32_t c) {
    return ImVec4{static_cast<float>(c & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 24) & 0xFF) / 255.0f};
}
} // namespace

// Ein Offset-Block (Pos XYZ + Pitch/Yaw/Roll) als eigener Tree -- im Original
// die lokale Hilfsfunktion fl_off_ui, hier auch fuer die Korrekturtabellen
// benutzt.
bool RE4VRMotion::draw_offset_tree(const char* label, FlOffset& t) {
    if (!ImGui::TreeNode(label)) {
        return false;
    }

    bool dirty = false;
    const std::string sfx = std::string{"##"} + label;

    dirty |= ImGui::DragFloat(("Pos X" + sfx).c_str(), &t.pos_x, 0.001f, -0.5f, 0.5f, "%.3f");
    dirty |= ImGui::DragFloat(("Pos Y" + sfx).c_str(), &t.pos_y, 0.001f, -0.5f, 0.5f, "%.3f");
    dirty |= ImGui::DragFloat(("Pos Z" + sfx).c_str(), &t.pos_z, 0.001f, -0.5f, 0.5f, "%.3f");
    dirty |= ImGui::DragFloat(("Pitch" + sfx).c_str(), &t.rot_pitch, 0.1f, -180.0f, 180.0f, "%.1f");
    dirty |= ImGui::DragFloat(("Yaw" + sfx).c_str(), &t.rot_yaw, 0.1f, -180.0f, 180.0f, "%.1f");
    dirty |= ImGui::DragFloat(("Roll" + sfx).c_str(), &t.rot_roll, 0.1f, -180.0f, 180.0f, "%.1f");

    ImGui::TreePop();
    return dirty;
}

// Sechs Slider fuer einen WeaponOffset. pr = Pos-Grenze (Default 0.5, groesser
// fuer weit sitzende Waffen), sp = Drag-Speed passend zur Range.
void RE4VRMotion::draw_offset_sliders(const char* prefix, WeaponOffset& off, float pr, float sp) {
    const std::string p{prefix};
    bool changed = false;

    changed |= ImGui::DragFloat((p + " Pos X").c_str(), &off.px, sp, -pr, pr, "%.3f");
    changed |= ImGui::DragFloat((p + " Pos Y").c_str(), &off.py, sp, -pr, pr, "%.3f");
    changed |= ImGui::DragFloat((p + " Pos Z").c_str(), &off.pz, sp, -pr, pr, "%.3f");
    changed |= ImGui::DragFloat((p + " Rot X").c_str(), &off.rx, 0.2f, -180.0f, 180.0f, "%.1f");
    changed |= ImGui::DragFloat((p + " Rot Y").c_str(), &off.ry, 0.2f, -180.0f, 180.0f, "%.1f");
    changed |= ImGui::DragFloat((p + " Rot Z").c_str(), &off.rz, 0.2f, -180.0f, 180.0f, "%.1f");

    // Das Original speichert nach JEDEM geaenderten Slider sofort -- beim Ziehen
    // also pro Frame ein kompletter Read-Modify-Write der JSON. 1:1 uebernommen.
    if (changed) {
        save_config();
    }
}

// Slider auf ein Feld einer Lua-Global-Tabelle. Fehlt das Feld, gilt der
// Default -- genau Luas `fc.x or <default>`. Geschrieben wird nur bei
// Aenderung, und dann sofort gespeichert (wie im Original).
bool RE4VRMotion::lua_table_slider(const char* table, const char* field, const char* label,
                                   double def, float lo, float hi, const char* fmt) {
    float v = static_cast<float>(re4vr::lua_get_table_number(table, field, def));

    if (!ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
        return false;
    }

    re4vr::lua_set_table_number(table, field, v);
    save_config();
    return true;
}

// Slider auf ein einfaches Lua-Global (kein Tabellenfeld).
bool RE4VRMotion::lua_global_slider(const char* name, const char* label, double def, float lo,
                                    float hi, const char* fmt) {
    float v = static_cast<float>(re4vr::lua_get_number(name, def));

    if (!ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
        return false;
    }

    re4vr::lua_set_number(name, v);
    save_config();
    return true;
}

// Sechs Slider fuer ein Support-Offset. Die Felder liegen in verschiedenen
// Structs (SupportOffset / SupportOffsetAim / ...), deshalb per Referenz.
void RE4VRMotion::draw_support_sliders(const char* suffix, float& px, float& py, float& pz,
                                       float& pitch, float& yaw, float& roll) {
    const std::string s{suffix};
    bool changed = false;

    changed |= ImGui::SliderFloat(("Sup X" + s).c_str(), &px, -0.5f, 0.5f);
    changed |= ImGui::SliderFloat(("Sup Y" + s).c_str(), &py, -0.5f, 0.5f);
    changed |= ImGui::SliderFloat(("Sup Z" + s).c_str(), &pz, -0.5f, 0.5f);
    changed |= ImGui::SliderFloat(("Sup Pitch" + s).c_str(), &pitch, -180.0f, 180.0f);
    changed |= ImGui::SliderFloat(("Sup Yaw" + s).c_str(), &yaw, -180.0f, 180.0f);
    changed |= ImGui::SliderFloat(("Sup Roll" + s).c_str(), &roll, -180.0f, 180.0f);

    if (changed) {
        save_config();
    }
}

void RE4VRMotion::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (re4vr::mods_gated()) {
        return;
    }

    if (!ImGui::TreeNode("RE4VR - Motion")) {
        // [MENUE-REIHENFOLGE 2026-09-07] Hier stand der Fallback "kein Lua-Dispatcher
    // -> Public-Block selbst zeichnen". Genau der liess die nackten Optionen
    // zwischen den Entwickler-Trees auftauchen, seit ##re4_vr_menu.lua mit dem
    // Port abgeschaltet ist. Gezeichnet wird jetzt zentral in
    // RE4VRMenu::draw_public, in fester Reihenfolge und ganz oben.
        return;
    }

    // --- Status ---
    ImGui::Text("Runtime: %s   Controller: %s", m_vr_runtime.c_str(), m_selected_controller.c_str());
    ImGui::Text("Init: %s  R_Hand: %s  L_Hand: %s", m_init.initialized ? "true" : "false",
                m_right_hand.joint.obj != nullptr ? "ok" : "-",
                m_left_hand.joint.obj != nullptr ? "ok" : "-");

    // --- Headset-Auswahl (dieselbe Variable wie im Public-Block) ---
    ImGui::Text("Headset:");
    {
        struct Opt {
            const char* label;
            const char* value;
        };

        static const Opt hs[2] = {{"SteamVR", "steamvr"}, {"MetaVR", "metavr"}};

        for (int i = 0; i < 2; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }

            const bool active = m_selected_controller == hs[i].value;

            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Text, col_abgr(0xFFD0E040));
            }

            if (ImGui::Button(hs[i].label)) {
                if (m_selected_controller != hs[i].value) {
                    m_selected_controller = hs[i].value;
                    save_config();
                }
            }

            if (active) {
                ImGui::PopStyleColor(1);
            }
        }
    }

    ImGui::Separator();

    // --- Hand-Toggles + Kalibrierung ---
    ImGui::Checkbox("Right Hand", &m_right_hand.enabled);
    ImGui::Checkbox("Left Hand", &m_left_hand.enabled);
    ImGui::Checkbox("Weapon Attach (Waffe folgt Hand direkt)", &m_wep_attach_enabled);

    {
        const char* calib_txt = "fixiert";

        if (m_wep_cache.calib.has_value()) {
            calib_txt = m_wep_cache.calib->wait > 0 ? "warte..." : "sample...";
        } else if (!m_wep_cache.rel_pos.has_value()) {
            calib_txt = "-";
        }

        char wep[16]{};

        if (m_wep_cache.id.has_value()) {
            std::snprintf(wep, sizeof(wep), "wp%04d", *m_wep_cache.id);
        } else {
            std::snprintf(wep, sizeof(wep), "-");
        }

        ImGui::Text("Weapon: %s  rel: %s", wep, calib_txt);
        ImGui::SameLine();

        // [KNIFE_ADA] Sichtbar machen, WEN man kalibriert: bei Messern ist
        // weapon_rel pro Charakter getrennt, bei allen anderen gemeinsam. Ohne
        // diese Zeile sieht man dem Knopf nicht an, in welchen Datensatz er
        // schreibt.
        const std::string ch = char_now();
        const bool is_knife = m_wep_cache.id.has_value() && is_knife_rel_split_id(*m_wep_cache.id);
        const std::string what =
            is_knife ? ("Messer, getrennt (" + rel_key(*m_wep_cache.id) + ")")
                     : std::string{"gemeinsam (nicht charakter-getrennt)"};

        ImGui::TextColored(col_abgr(ch == "ada" ? 0xFF44FF44 : 0xFFAAAAAA),
                           "Charakter: %s   |   Kalibrierung: %s",
                           ch.empty() ? "unbekannt" : ch.c_str(), what.c_str());
    }

    if (ImGui::Button("Re-Kalibrieren##wep_recal")) {
        // [WEP_REL_PERSIST] gespeicherten Wert verwerfen -> frisch kalibrieren
        // und neu speichern. [KNIFE_ADA] loescht bei Ada NUR den @ada-Schluessel,
        // Leons Kalibrierung bleibt stehen.
        if (m_wep_cache.id.has_value()) {
            m_weapon_rel.erase(rel_key(*m_wep_cache.id));
        }

        m_wep_cache.rel_pos.reset();
        m_wep_cache.rel_rot.reset();
        m_wep_cache.calib = WepCache::Calib{5, CALIB_SAMPLE};
        m_wep_cache.frozen = false;
    }

    if (ImGui::SliderFloat("Hand Smoothing (Rotation)", &m_rot_smooth_hands, 0.0f, 0.95f)) {
        save_config();
    }

    if (ImGui::SliderFloat("Hand Smoothing (Position, Anti-Zitter)", &m_pos_smooth_hands, 0.0f, 0.95f)) {
        save_config();
    }

    // [KS4_EXIT_FADE] Dauer, ueber die beide Haende nach dem KS4-Austritt von
    // der nativen Pose zum Controller einblenden. 0 = hart/aus.
    {
        float dur = static_cast<float>(re4vr::lua_get_table_number("__re4_ks4fade", "dur", 0.35));

        if (ImGui::SliderFloat("KS4-Austritt: Haende einblenden (s, 0=hart)", &dur, 0.0f, 1.0f)) {
            re4vr::lua_set_table_number("__re4_ks4fade", "dur", dur);
            save_config();
        }
    }

    ImGui::Separator();

    // --- OpenXR-Korrektur (immer gleich, von RE9 uebernommen) ---
    if (ImGui::TreeNode("OpenXR Correction (applies when runtime=openxr)")) {
        bool changed = false;
        changed |= ImGui::DragFloat("Pos X", &m_openxr_correction.pos_x, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Pos Y", &m_openxr_correction.pos_y, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Pos Z", &m_openxr_correction.pos_z, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Rot Pitch", &m_openxr_correction.rot_pitch, 0.1f, -180.0f, 180.0f, "%.2f");
        changed |= ImGui::DragFloat("Rot Yaw", &m_openxr_correction.rot_yaw, 0.1f, -180.0f, 180.0f, "%.2f");
        changed |= ImGui::DragFloat("Rot Roll", &m_openxr_correction.rot_roll, 0.1f, -180.0f, 180.0f, "%.2f");

        if (ImGui::Button("Reset to RE9 defaults")) {
            m_openxr_correction = Correction{0.015f, -0.006f, -0.104f, -16.341f, -2.011f, -0.754f};
            changed = true;
        }

        ImGui::SameLine();

        if (ImGui::Button("Save")) {
            save_config();
        }

        if (changed) {
            save_config();
        }

        ImGui::TreePop();
    }

    // --- [FLASHLIGHT] Offsets ---
    if (ImGui::TreeNode("Flashlight (linke Hand / docked = HMD)")) {
        if (ImGui::Checkbox("Enabled##fl", &m_fl_enabled)) {
            fl_save_cfg();
        }

        if (ImGui::Checkbox("FL beim Knife in der Hand lassen + Pose", &m_fl_keep_on_knife)) {
            fl_save_cfg();
        }

        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%s", m_fl_knife_pose.c_str());

            if (ImGui::InputText("Knife-Pose (Gestures-Name)", buf, sizeof(buf))) {
                m_fl_knife_pose = buf;
                fl_save_cfg();
            }
        }

        bool dirty = false;
        dirty |= draw_offset_tree("FL frei (Hand)", m_fl_offset);
        dirty |= draw_offset_tree("Licht frei", m_fl_light_offset);
        dirty |= draw_offset_tree("FL docked (HMD)", m_fl_state.docked_offset);
        dirty |= draw_offset_tree("Licht docked (HMD)", m_fl_state.docked_light_offset);
        dirty |= draw_offset_tree("FL Flip", m_fl_state.flip);

        // [1:1] flip_finger_deg (-35.0) und flip_time (0.15) sind im Original
        // FESTE Werte ohne UI -- und fl_save_cfg speichert sie in beiden
        // Fassungen nicht. Ein Regler waere hier also nicht nur neu, er waere
        // nach dem naechsten Start auch wieder zurueckgesprungen.

        if (dirty) {
            fl_save_cfg();
        }

        ImGui::TreePop();
    }

    // --- Quest/Touch-Korrektur (nur bei metavr wirksam) ---
    if (ImGui::TreeNode("Controller Correction (applies when Headset=MetaVR)")) {
        bool changed = false;
        changed |= ImGui::DragFloat("Pos X##ctrl", &m_ctrl_correction.pos_x, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Pos Y##ctrl", &m_ctrl_correction.pos_y, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Pos Z##ctrl", &m_ctrl_correction.pos_z, 0.001f, -0.5f, 0.5f, "%.3f");
        changed |= ImGui::DragFloat("Rot Pitch##ctrl", &m_ctrl_correction.rot_pitch, 0.1f, -180.0f, 180.0f, "%.2f");
        changed |= ImGui::DragFloat("Rot Yaw##ctrl", &m_ctrl_correction.rot_yaw, 0.1f, -180.0f, 180.0f, "%.2f");
        changed |= ImGui::DragFloat("Rot Roll##ctrl", &m_ctrl_correction.rot_roll, 0.1f, -180.0f, 180.0f, "%.2f");

        if (ImGui::Button("Reset to RE9 defaults##ctrl")) {
            m_ctrl_correction = Correction{-0.004f, -0.002f, -0.002f, 0.0f, 0.0f, 5.606f};
            changed = true;
        }

        if (changed) {
            save_config();
        }

        ImGui::TreePop();
    }

    // --- Rechte Hand: per Waffe (die Slider editieren die equippte Waffe) ---
    if (ImGui::TreeNode("Right Hand Offset (per Weapon)")) {
        const std::string key = m_cache.weapon_key.empty() ? WEAPON_NONE_KEY : m_cache.weapon_key;
        ImGui::Text("Current Weapon: %s", key.c_str());

        auto& off = get_weapon_offset(key);

        // [EGG_5403] Das Ei sitzt weit von der Hand (kaputter Kalibrier-Versatz
        // ~1 m) -> groessere Pos-Range und schnelleres Ziehen, damit man es ganz
        // heranholen kann. Nur fuers Ei; [EGG_5405] Adas braucht dasselbe.
        if (key == "5403" || key == "5405") {
            draw_offset_sliders("R", off, 2.0f, 0.005f);
        } else {
            draw_offset_sliders("R", off);
        }

        if (ImGui::Button("Reset this weapon to 0")) {
            off = WeaponOffset{};
            save_config();
        }

        // [KNIFE_WEP_ADA] Messer: WAFFE-only Versatz. Die Slider oben bewegen
        // die HAND -- diese hier NUR das Messer. Pro Charakter getrennt.
        if (m_wep_cache.id.has_value() && is_knife_rel_split_id(*m_wep_cache.id)) {
            const std::string ch = char_now();
            ImGui::Separator();
            ImGui::TextColored(col_abgr(ch == "ada" ? 0xFF44FF44 : 0xFFAAAAAA),
                               "Messer im Griff - Waffe-only (Hand bleibt 1:1)  |  Charakter: %s",
                               ch.empty() ? "unbekannt" : ch.c_str());

            const std::string kkey = knife_wep_key(*m_wep_cache.id);
            ImGui::Text("Key: %s", kkey.c_str());

            // [FLIP_LERP_ANZEIGE] Nur Anzeige: bei lerp > 0 wird der
            // POSITIONS-Anteil dieser Slider bewusst herausgerechnet
            // ([FLIP_ENTKOPPELT]) -- dann tunt man die Lage ueber den
            // Flip-Griff-Offset im Messer-Tree. Die ROTATION wirkt immer.
            if (m_knife_flip.lerp > 0.5f && !kkey.ends_with("_flip")) {
                ImGui::TextColored(col_abgr(0xFFFFAA00),
                                   "Flip-Lerp %.3f -> POS hier wirkungslos, Lage ueber 'Flip "
                                   "Griff-Offset' tunen. ROT wirkt.",
                                   m_knife_flip.lerp);
            } else {
                ImGui::TextColored(col_abgr(0xFF88CCFF),
                                   "Flip-Lerp %.3f -> Pos UND Rot wirken hier.", m_knife_flip.lerp);
            }

            // [KNIFE_THROW_OFFSET 17.09.2026] Krauser in Mercenaries stellt zwei
            // Faelle getrennt ein; sonst bleibt es beim einen Slider wie bisher.
            const bool krauser_mercs =
                re4vr::lua_get_bool("__re4_in_mercs", false) && is_krauser_body();

            if (krauser_mercs) {
                const std::string kbase = std::to_string(*m_wep_cache.id) + "_knifewep";
                ImGui::Text("Messer Krauser in Mercenaries");

                if (ImGui::TreeNode("Normal")) {
                    auto& o = get_weapon_offset(kbase + "@krauser");
                    draw_offset_sliders("Normal", o);

                    if (ImGui::Button("Reset Normal to 0")) {
                        o = WeaponOffset{};
                        save_config();
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Nach Wurf")) {
                    auto& o = get_weapon_offset(kbase + "@krauser_throw");
                    draw_offset_sliders("Nach Wurf", o);

                    if (ImGui::Button("Reset Nach Wurf to 0")) {
                        o = WeaponOffset{};
                        save_config();
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Geflippt aus Holster")) {
                    auto& o = get_weapon_offset(kbase + "@krauser_flip");
                    draw_offset_sliders("Geflippt", o);

                    if (ImGui::Button("Reset Geflippt to 0")) {
                        o = WeaponOffset{};
                        save_config();
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Geflippt nach Wurf")) {
                    auto& o = get_weapon_offset(kbase + "@krauser_throw_flip");
                    draw_offset_sliders("Geflippt nach Wurf", o);

                    if (ImGui::Button("Reset Geflippt nach Wurf to 0")) {
                        o = WeaponOffset{};
                        save_config();
                    }

                    ImGui::TreePop();
                }

                const bool flip_set = kkey.ends_with("_flip");
                const char* aktiv = m_knife_after_throw
                                        ? (flip_set ? "   gerade aktiv: Geflippt nach Wurf" : "   gerade aktiv: Nach Wurf")
                                        : (flip_set ? "   gerade aktiv: Geflippt aus Holster" : "   gerade aktiv: Normal");
                ImGui::TextColored(col_abgr((m_knife_after_throw || flip_set) ? 0xFF44FF44 : 0xFF88CCFF), "%s", aktiv);
            } else {
                auto& koff = get_weapon_offset(kkey);
                draw_offset_sliders("Messer-Wpn", koff);

                if (ImGui::Button("Reset Messer-Waffe to 0")) {
                    koff = WeaponOffset{};
                    save_config();
                }
            }

        }

        // [BOW_6102] Blast Crossbow: WAFFE-only Versatz gegen den festen
        // ~15-cm-Versatz ihrer Kalibrierung. Eigener Key -> unabhaengig vom
        // Hand-Offset darueber.
        if (m_wep_cache.id.has_value() && *m_wep_cache.id == 6102) {
            ImGui::Separator();
            ImGui::TextColored(col_abgr(0xFF66CCFF),
                               "Blast Crossbow im Griff - Waffe-only (Hand bleibt 1:1)  |  Key: 6102_bowwep");
            auto& boff = get_weapon_offset("6102_bowwep");
            draw_offset_sliders("Bow-Wpn", boff);

            if (ImGui::Button("Reset Bow-Waffe to 0")) {
                boff = WeaponOffset{};
                save_config();
            }
        }

        // [MATILDA_STOCK] Nur wenn die Matilda MIT Schulteraufsatz equippt ist.
        // Bewegt NUR die Waffe relativ zur Hand -> die Hand bleibt exakt wie
        // ohne Stock.
        if (m_cache.matilda_stock) {
            ImGui::Separator();
            ImGui::Text("Matilda MIT Stock - Waffe-only Versatz (Hand bleibt 1:1):");
            auto& woff = get_weapon_offset("4004_stockwep");
            draw_offset_sliders("Stock-Wpn", woff);

            if (ImGui::Button("Reset Stock-Waffe to 0")) {
                woff = WeaponOffset{};
                save_config();
            }
        }

        ImGui::TreePop();
    }

    // --- Linke Hand: global ---
    if (ImGui::TreeNode("Left Hand Offset")) {
        // Die Slider arbeiten DIREKT auf den Feldern. Eine Kopie waere falsch:
        // draw_offset_sliders speichert am eigenen Ende, also bevor die Kopie
        // zurueckgeschrieben werden koennte -- gespeichert wuerde der Wert des
        // Vorframes, und ein einzelner Nudge ginge ganz verloren.
        static_assert(sizeof(HandOffsetL) == sizeof(WeaponOffset),
                      "HandOffsetL und WeaponOffset muessen layoutgleich bleiben");
        draw_offset_sliders("L", reinterpret_cast<WeaponOffset&>(m_hand_offset_l));
        ImGui::TreePop();
    }

    // [SKULL_SPIN_ENTFERNT] Hier lag der Tree "Skull Shaker - Spin-Keyframes"
    // (Vorschau, Winkel-/Offset-Slider, Key-Liste, Finger-Spreiz-Slider).
    // Ersatzlos raus.

    // --- [SUPPORT_HAND] ---
    if (ImGui::TreeNode("Weapon Support (linke Hand dockt an Waffe)")) {
        if (ImGui::Checkbox("Enable Support", &m_support.enabled)) {
            save_config();
        }

        ImGui::Checkbox("Force Dock (Setup)", &m_support.force_dock);

        // [GRIP_LATCH_REACH] ANTEIL der echten Armreichweite, KEIN Meterwert ->
        // gilt fuer Ada und Leon gleichermassen, weil arm_chain die
        // Knochenlaengen des aktiven Charakters live misst. Die Anzeige darunter
        // zeigt die aktuelle Auslastung, damit man beim Tunen sieht, wann es
        // abreisst.
        if (ImGui::SliderFloat("Grip-Latch: max. Armreichweite (Anteil)",
                               &m_support.grip_latch_reach, 0.40f, 1.00f, "%.2f")) {
            save_config();
        }

        {
            const auto l_root = re4vr::lua_get_vec3("__vr_arm_chain_L_root");
            const auto l_max = re4vr::lua_get_number_opt("__vr_arm_chain_L_maxreach");

            if (l_root.has_value() && l_max.has_value() && *l_max > 0.01) {
                ImGui::Text("   Armreichweite gemessen: %.2f m  ->  Latch bricht bei %.2f m",
                            *l_max, *l_max * m_support.grip_latch_reach);
            } else {
                ImGui::TextColored(col_abgr(0xFF8888FF),
                                   "   (Armreichweite unbekannt - arm_chain hand_clamp aus? Dann "
                                   "haelt nur die Distanz-Regel.)");
            }
        }

        ImGui::Text("Weapon supported: %s", is_support_hand_weapon() ? "yes" : "no");
        ImGui::Text("Status: %s", m_support.docked ? "DOCKED" : "FREE");
        ImGui::Text("Blend: %.0f%%", m_support.blend_factor * 100.0f);

        if (ImGui::SliderFloat("Blend Speed (global)", &m_support.blend_speed, 0.05f, 0.5f)) {
            save_config();
        }

        // [RACK_FREEZE RAUS / PUMP_NO_Z] Es gibt keinen Rack-Freeze mehr -- die
        // Waffe bleibt beim Racken voll beweglich. Was bleibt: waehrend eines
        // Slide-Racks wandert eine ZWEIHANDWAFFE nicht mehr entlang ihrer
        // Laengsachse zum Koerper.
        {
            const bool rack = re4vr::lua_get_tribool("__vr_slide_rack_active") == 1;
            const bool pump = re4vr::lua_get_tribool("__vr_shotgun_pump_active") == 1;
            ImGui::TextColored(col_abgr(0xFF888888),
                               "   Rack aktiv: %s   Z-Sperre aktiv: %s   (Shotgun-Pump: %s)",
                               rack ? "true" : "false",
                               (pump || (rack && is_two_hand_aim_weapon())) ? "true" : "false",
                               pump ? "true" : "false");
        }

        const std::string skey = m_cache.weapon_key.empty() ? WEAPON_NONE_KEY : m_cache.weapon_key;
        ImGui::Text("-- Support Offset (Current Weapon: %s) --", skey.c_str());

        auto& soff = get_support_offset(skey);
        draw_support_sliders("", soff.pos_x, soff.pos_y, soff.pos_z, soff.rot_pitch, soff.rot_yaw,
                             soff.rot_roll);

        if (ImGui::SliderFloat("Dock Dist", &soff.dock_threshold, 0.05f, 0.5f)) {
            save_config();
        }

        if (ImGui::SliderFloat("Undock Dist", &soff.undock_threshold, 0.1f, 0.6f)) {
            save_config();
        }

        // [PUMP_GRIP] Nur bei den Pump-Shotguns sichtbar: dort ist der
        // Vordergriff das bewegliche Pump-Teil. AN = die Hand klebt am
        // Pump-Joint und wandert mit; die drei "Sup Pos"-Slider darueber sind
        // dann WIRKUNGSLOS (die gelten fuer den Hand-Anker). Getunt wird
        // stattdessen mit Griff X/Y/Z -- Nullpunkt ist das Joint selbst.
        if (m_wep_cache.id.has_value() && is_pump_grip_weapon(*m_wep_cache.id)) {
            ImGui::Text("-- Pump-Griff (Hand klebt am Pump-Teil) --");
            const std::string jn = re4vr::lua_get_string("__re4_rack_joint_name");
            ImGui::Text("Joint aus reload.lua: %s", jn.empty() ? "noch nicht gemeldet" : jn.c_str());

            if (ImGui::Checkbox("Hand ans Pump-Joint kleben", &soff.grip_on)) {
                save_config();
            }

            if (soff.grip_on) {
                if (ImGui::SliderFloat("Griff X (seitlich)", &soff.grip_x, -0.30f, 0.30f, "%.3f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Griff Y (hoch/runter)", &soff.grip_y, -0.30f, 0.30f, "%.3f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Griff Z (laengs)", &soff.grip_z, -0.30f, 0.30f, "%.3f")) {
                    save_config();
                }

                // [GRIP_SLIDE] Gleitbereich um Griff Z. 0/0 = fester Punkt.
                // Aufdrehen = die Hand darf am Rohr wandern, statt Richtung
                // Schulter gezogen zu werden.
                ImGui::Text("Gleitbereich am Rohr (0 = fester Punkt)");

                if (ImGui::SliderFloat("Gleiten nach hinten (m)", &soff.grip_back, 0.0f, 0.25f, "%.3f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Gleiten nach vorn (m)", &soff.grip_fwd, 0.0f, 0.25f, "%.3f")) {
                    save_config();
                }

                // [WEAPON_GIVE] Die Waffe weicht zurueck, wenn der Griff
                // ausserhalb der linken Armreichweite liegt (statt dass die Hand
                // abreisst). Beim Pumpen bewusst inaktiv.
                if (m_support.give.has_value()) {
                    const glm::vec3 g = *m_support.give;
                    ImGui::Text("   Ruecknahme aktuell: %.3f m",
                                std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z));
                }
            }
        }

        // [SUPPORT_AIM] Zweites Offset fuer den Aim-Zustand (nur "Kipp"-Waffen).
        // Aim halten -> die Stuetzhand blendet auf diese Werte, loslassen ->
        // zurueck zu Idle.
        ImGui::Text("-- Aim Support Offset (Kipp-Waffen) --");
        ImGui::Text("Aim-Blend: %.0f%%  (0%% = Idle, 100%% = Aim gehalten)",
                    m_support.aim_blend * 100.0f);
        ImGui::Checkbox("Preview Aim (zum Tunen, erzwingt Aim-Pose)", &m_support.aim_preview);

        {
            auto* aoff = get_support_offset_aim(skey);
            bool has_aim = aoff != nullptr;

            if (ImGui::Checkbox("Separate Aim Offset", &has_aim)) {
                if (has_aim) {
                    ensure_support_offset_aim(skey);
                } else {
                    m_support_offset_aim.erase(skey.empty() ? WEAPON_NONE_KEY : skey);
                }

                save_config();
                aoff = get_support_offset_aim(skey);
            }

            if (aoff != nullptr) {
                draw_support_sliders(" Aim", aoff->pos_x, aoff->pos_y, aoff->pos_z,
                                     aoff->rot_pitch, aoff->rot_yaw, aoff->rot_roll);

                if (ImGui::SliderFloat("Blend no-aim -> aim", &aoff->blend_in, 0.02f, 1.0f, "%.3f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Blend aim -> no-aim", &aoff->blend_out, 0.02f, 1.0f, "%.3f")) {
                    save_config();
                }

                if (ImGui::Button("Copy Idle -> Aim")) {
                    aoff->pos_x = soff.pos_x;
                    aoff->pos_y = soff.pos_y;
                    aoff->pos_z = soff.pos_z;
                    aoff->rot_pitch = soff.rot_pitch;
                    aoff->rot_yaw = soff.rot_yaw;
                    aoff->rot_roll = soff.rot_roll;
                    save_config();
                }
            }
        }

        // --- [SWITCH_DOCK] Hand dockt an den Feuerwahl-Schalter statt an den Schaft ---
        const bool has_switch_weapon = m_wep_cache.id.has_value()
                                       && is_switch_dock_weapon(*m_wep_cache.id);

        if (has_switch_weapon && ImGui::TreeNode("full-Auto")) {
            ImGui::Text("-- LE5 Schalter-Dock (joint_05) --");

            auto* sw = get_support_offset_switch(skey);
            bool sw_on = sw != nullptr;

            if (ImGui::Checkbox("Schalter-Dock aktiv", &sw_on)) {
                if (sw_on) {
                    ensure_support_offset_switch(skey);
                } else {
                    m_support_offset_switch.erase(skey.empty() ? WEAPON_NONE_KEY : skey);
                    m_support.switch_docked = false;
                }

                save_config();
            }

            sw = get_support_offset_switch(skey);

            if (sw != nullptr) {
                ImGui::Checkbox("Preview Switch (zum Tunen, erzwingt Schalter-Pose)",
                                &m_support.switch_preview);

                if (ImGui::SliderFloat("Schalter-Distanz (Hand -> Schalter)", &sw->dock_dist,
                                       0.02f, 0.30f, "%.3f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Schalter-Lerp (hoeher = zackiger)", &sw->blend_speed,
                                       0.02f, 0.5f, "%.3f")) {
                    save_config();
                }

                draw_support_sliders(" Switch", sw->pos_x, sw->pos_y, sw->pos_z, sw->rot_pitch,
                                     sw->rot_yaw, sw->rot_roll);

                // Additiver Zeigefinger-Curl zum Schliessen der "OK"-Geste.
                if (ImGui::SliderFloat("Zeigefinger Rot X##swidx", &sw->idx_rx, -120.0f, 120.0f, "%.1f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Zeigefinger Rot Y##swidx", &sw->idx_ry, -120.0f, 120.0f, "%.1f")) {
                    save_config();
                }

                if (ImGui::SliderFloat("Zeigefinger Rot Z (Curl)##swidx", &sw->idx_rz, -120.0f, 120.0f, "%.1f")) {
                    save_config();
                }

                // Eigener Aim-Zustand am Schalter: eigene Offsets und eigener
                // Lerp, die Pose selbst bleibt gleich.
                ImGui::Text("- Switch Aim Offset (eigener Aim-Zustand) -");
                auto* swa = get_support_offset_switch_aim(skey);
                bool swa_on = swa != nullptr;

                if (ImGui::Checkbox("Separate Aim Offset (Switch)", &swa_on)) {
                    if (swa_on) {
                        ensure_support_offset_switch_aim(skey);
                    } else {
                        m_support_offset_switch_aim.erase(skey.empty() ? WEAPON_NONE_KEY : skey);
                    }

                    save_config();
                }

                swa = get_support_offset_switch_aim(skey);

                if (swa != nullptr) {
                    ImGui::Checkbox("Preview Switch Aim (Schalter + Aim in einem)",
                                    &m_support.switch_aim_preview);
                    draw_support_sliders(" SwAim", swa->pos_x, swa->pos_y, swa->pos_z,
                                         swa->rot_pitch, swa->rot_yaw, swa->rot_roll);

                    if (ImGui::SliderFloat("Switch Blend no-aim -> aim", &swa->blend_in, 0.02f, 1.0f, "%.3f")) {
                        save_config();
                    }

                    if (ImGui::SliderFloat("Switch Blend aim -> no-aim", &swa->blend_out, 0.02f, 1.0f, "%.3f")) {
                        save_config();
                    }

                    if (ImGui::Button("Copy Switch Idle -> Aim")) {
                        swa->pos_x = sw->pos_x;
                        swa->pos_y = sw->pos_y;
                        swa->pos_z = sw->pos_z;
                        swa->rot_pitch = sw->rot_pitch;
                        swa->rot_yaw = sw->rot_yaw;
                        swa->rot_roll = sw->rot_roll;
                        save_config();
                    }
                }

                // [SWITCH_DOCK2] Zweites Hand-Offset-Paar fuer die ZWEITE
                // Hebelstellung. Manche Waffen brauchen je Stellung eine eigene
                // Pose, weil der rigide 1:1-Pivot-Follow sonst komisch aussieht.
                ImGui::Text("- Switch Offset 2 (Single/Burst-Hebelstellung) -");
                auto* sw2 = get_support_offset_switch2(skey);
                bool sw2_on = sw2 != nullptr;

                if (ImGui::Checkbox("Zweites Offset-Paar (zweite Hebelstellung)", &sw2_on)) {
                    const std::string k = skey.empty() ? WEAPON_NONE_KEY : skey;

                    if (sw2_on) {
                        ensure_support_offset_switch2(skey);
                    } else {
                        m_support_offset_switch2.erase(k);
                        m_support_offset_switch2_aim.erase(k);
                    }

                    save_config();
                }

                sw2 = get_support_offset_switch2(skey);

                if (sw2 != nullptr) {
                    ImGui::Checkbox("Preview Switch 2 (Schalter + Hebel auf Burst-Stellung)",
                                    &m_support.switch2_preview);

                    if (ImGui::SliderFloat("Pose1<->Pose2 Lerp (no-aim + aim)", &sw2->lerp,
                                           0.02f, 0.5f, "%.3f")) {
                        save_config();
                    }

                    draw_support_sliders(" Sw2", sw2->pos_x, sw2->pos_y, sw2->pos_z,
                                         sw2->rot_pitch, sw2->rot_yaw, sw2->rot_roll);

                    if (ImGui::Button("Copy Switch Idle -> Sw2")) {
                        sw2->pos_x = sw->pos_x;
                        sw2->pos_y = sw->pos_y;
                        sw2->pos_z = sw->pos_z;
                        sw2->rot_pitch = sw->rot_pitch;
                        sw2->rot_yaw = sw->rot_yaw;
                        sw2->rot_roll = sw->rot_roll;
                        save_config();
                    }

                    auto* sw2a = get_support_offset_switch2_aim(skey);
                    bool sw2a_on = sw2a != nullptr;

                    if (ImGui::Checkbox("Separate Aim Offset (Switch 2)", &sw2a_on)) {
                        if (sw2a_on) {
                            ensure_support_offset_switch2_aim(skey);
                        } else {
                            m_support_offset_switch2_aim.erase(skey.empty() ? WEAPON_NONE_KEY : skey);
                        }

                        save_config();
                    }

                    sw2a = get_support_offset_switch2_aim(skey);

                    if (sw2a != nullptr) {
                        ImGui::Checkbox("Preview Switch 2 Aim (Schalter + Burst-Stellung + Aim)",
                                        &m_support.switch2_aim_preview);
                        draw_support_sliders(" Sw2Aim", sw2a->pos_x, sw2a->pos_y, sw2a->pos_z,
                                             sw2a->rot_pitch, sw2a->rot_yaw, sw2a->rot_roll);

                        if (ImGui::Button("Copy Switch2 Idle -> Sw2 Aim")) {
                            sw2a->pos_x = sw2->pos_x;
                            sw2a->pos_y = sw2->pos_y;
                            sw2a->pos_z = sw2->pos_z;
                            sw2a->rot_pitch = sw2->rot_pitch;
                            sw2a->rot_yaw = sw2->rot_yaw;
                            sw2a->rot_roll = sw2->rot_roll;
                            save_config();
                        }
                    }
                }
            }

            ImGui::TreePop();
        }

        // --- [FIRE_MODE] Feuerwahl-Hebel: Full / Burst / Single ---
        if (has_switch_weapon && ImGui::TreeNode("burst-mode")) {
            auto& swb = ensure_support_offset_switch(skey);
            const char* mode_name = m_support.fire_mode == 1   ? "Burst"
                                    : m_support.fire_mode == 2 ? "Single"
                                                               : "Full (default)";
            ImGui::Text("Aktueller Modus: %s", mode_name);
            ImGui::Text("Zyklus: Full -> Burst -> Single -> Full (Left-Grip am Schalter + "
                        "Left-Trigger antippen)");
            ImGui::Separator();

            if (ImGui::SliderFloat("Hebel-Schwenk-Lerp (kleiner = geschmeidiger)", &swb.lever_lerp,
                                   0.03f, 0.5f, "%.3f")) {
                save_config();
            }

            ImGui::Separator();
            ImGui::Checkbox("Preview Burst (Hebel auf Burst-Winkel)", &m_support.burst_preview);

            if (ImGui::SliderFloat("Burst Fire Position (Schalter-Rotation)", &swb.burst_rot,
                                   -180.0f, 180.0f, "%.1f")) {
                save_config();
            }

            int bc = static_cast<int>(swb.burst_count);

            if (ImGui::SliderInt("Burst-Schuss-Anzahl (1 = Single)", &bc, 1, 5)) {
                swb.burst_count = static_cast<float>(bc);
                save_config();
            }

            // SINGLE-Stellung -- nur bei Waffen, deren Zyklus sie kennt.
            if (fire_mode_has_single(*m_wep_cache.id)) {
                ImGui::Separator();
                ImGui::Checkbox("Preview Single (Hebel auf Single-Winkel)", &m_support.single_preview);

                if (ImGui::SliderFloat("Single Fire Position (Schalter-Rotation)", &swb.single_rot,
                                       -180.0f, 180.0f, "%.1f")) {
                    save_config();
                }
            }

            ImGui::TreePop();
        }

        if (ImGui::Button("Force Undock")) {
            m_support.docked = false;
            m_support.force_dock = false;
            m_support.blend_factor = 0.0f;
            m_support.target_blend = 0.0f;
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset this weapon support")) {
            auto& off = get_support_offset(skey);
            off.pos_x = 0.0f;
            off.pos_y = 0.0f;
            off.pos_z = 0.0f;
            off.rot_pitch = 0.0f;
            off.rot_yaw = 0.0f;
            off.rot_roll = 0.0f;
            off.dock_threshold = 0.110f;
            off.undock_threshold = 0.150f;
            save_config();
        }

        ImGui::TreePop();
    }

    // [POSE_FADE] Globale Rueckblend-Dauer der Hand-Posen (Slide-Rack UND
    // Mag-Insert): wie weich die Finger nach dem Loslassen auf die native
    // Animation zurueckgehen. Gilt fuer ALLE Waffen und BEIDE Charaktere.
    // 0 = hart/sofort (das Verhalten vor dem Regler war fest 0.10).
    {
        float pf = static_cast<float>(re4vr::lua_get_number("__re4_pose_fade_dur", 0.10));

        if (ImGui::SliderFloat("Hand-Pose: Rueckblenden (s)  [0 = hart]", &pf, 0.0f, 0.60f, "%.2f")) {
            re4vr::lua_set_number("__re4_pose_fade_dur", pf);
            save_config();
        }
    }

    // =========================================================================
    // [KNIFE_UI] Messer: alle Regler gebuendelt, Nahkampf und Wurf getrennt.
    // =========================================================================
    if (ImGui::TreeNode("Messer")) {
        ImGui::TextColored(col_abgr(0xFF88CCFF), "-- Reverse-Grip-Flip (RT) --");

        {
            float cur = static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_speed", 0.18));

            if (ImGui::SliderFloat("Flip: Tempo (kleiner = langsamer/weicher)", &cur, 0.02f, 0.5f, "%.3f")) {
                re4vr::lua_set_number("__re4_knife_flip_speed", cur);
                save_config();
            }
        }

        {
            float cur = static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_finger_deg", -35.0));

            if (ImGui::SliderFloat("Flip: Finger-Oeffnung (Grad, Vorzeichen=Richtung)", &cur,
                                   -80.0f, 80.0f, "%.1f")) {
                re4vr::lua_set_number("__re4_knife_flip_finger_deg", cur);
                save_config();
            }
        }

        // [KNIFE_FLIP] Griff-Offset PRO MESSER: die drei Slider wirken auf das
        // GERADE equippte Messer. Verschiedene Messer haben verschiedene
        // Groessen -> je eigener Offset.
        {
            ImGui::TextColored(col_abgr(0xFF88CCFF), "-- Flip Griff-Offset (PRO MESSER) --");

            const bool have_knife = re4vr::lua_get_tribool("__re4_knife_equipped") == 1
                                    && m_wep_cache.id.has_value();

            if (!have_knife) {
                ImGui::Text("(kein Messer in der Hand - Messer ziehen, dann pro Messer tunen)");
            } else {
                // [KNIFE_FLIP_ADA] Die Slider bearbeiten die Map des AKTIVEN
                // Charakters.
                const std::string ch = char_now();
                const bool is_ada = ch == "ada";
                const char* map = is_ada ? "__re4_knife_flip_pos_map_ada" : "__re4_knife_flip_pos_map";

                ImGui::TextColored(col_abgr(is_ada ? 0xFF44FF44 : 0xFFAAAAAA), "Charakter: %s",
                                   is_ada ? "ada" : "leon");

                const int32_t kid = *m_wep_cache.id;
                ImGui::Text("Messer-ID: %d", kid);

                glm::vec3 p{};
                const bool have_entry = re4vr::lua_get_xyz_at(map, kid, p);

                // [SEED-GUARD] Nur bei SICHER erkanntem Charakter einen Eintrag
                // anlegen -- und zwar SOFORT beim Zeichnen, aus den bisherigen
                // globalen Werten (Preservation, kein Sprung). Setzt die
                // Erkennung gerade aus, faellt die Map-Wahl auf Leon zurueck
                // (fuers LESEN richtig) -- ein Seeding wuerde dann aber Adas
                // Messer-ID in LEONS Tabelle schreiben. Genau so kam
                // "knife_flip_pos/6108" dort hinein.
                const bool known = !ch.empty();

                if (!have_entry && known) {
                    p = glm::vec3{
                        static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_x", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_y", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_pos_z", 0.0))};
                    re4vr::lua_ensure_table(map);
                    re4vr::lua_set_xyz_at(map, kid, p.x, p.y, p.z);
                }

                if (!known) {
                    ImGui::TextColored(col_abgr(0xFFFFAA00),
                                       "Charakter gerade nicht erkannt - kein Tuning (Schutz fuer "
                                       "Leons Werte)");
                    p = glm::vec3{0.0f, 0.0f, 0.0f};
                }

                // [FLIP_ENTKOPPELT] Anzeige, woher die Lage gerade kommt.
                if (m_knife_flip.lerp > 0.5f) {
                    ImGui::TextColored(col_abgr(0xFFFFAA00),
                                       "Flip-Lerp jetzt: %.3f -> Position kommt aus DIESEN Slidern.",
                                       m_knife_flip.lerp);
                } else {
                    ImGui::TextColored(col_abgr(0xFF88CCFF),
                                       "Flip-Lerp jetzt: %.3f -> Position kommt aus 'Messer-Wpn'.",
                                       m_knife_flip.lerp);
                }

                ImGui::Text("   Rotation kommt IMMER aus 'Messer-Wpn' (Right Hand Offset per Weapon).");

                bool changed = false;
                changed |= ImGui::SliderFloat("Flip: X-Versatz (Hand-Frame)", &p.x, -0.15f, 0.15f, "%.3f");
                changed |= ImGui::SliderFloat("Flip: Y-Versatz (Hand-Frame)", &p.y, -0.15f, 0.15f, "%.3f");
                changed |= ImGui::SliderFloat("Flip: Z-Versatz (Hand-Frame)", &p.z, -0.15f, 0.15f, "%.3f");

                if (changed && known) {
                    re4vr::lua_ensure_table(map);
                    re4vr::lua_set_xyz_at(map, kid, p.x, p.y, p.z);
                    save_config();
                }
            }
        }

        // --- Nahkampf (Stechen) ---
        ImGui::TextColored(col_abgr(0xFF88CCFF), "-- Nahkampf (Stechen) --");
        lua_global_slider("__re4_knife_swing_threshold", "Stich: Schwung-Schwelle (m/s)", 3.5, 0.5f, 8.0f);
        lua_global_slider("__re4_knife_reach", "Stich: Trefferradius Gegner (m)", 0.90, 0.0f, 2.0f);
        lua_global_slider("__re4_knife_touch", "Stich: Trefferradius Objekte/Kisten (m)", 0.90, 0.0f, 2.0f);

        // --- Wurf ---
        ImGui::TextColored(col_abgr(0xFF88CCFF), "-- Wurf --");
        lua_global_slider("__re4_knife_throw_threshold", "Wurf: Ausloese-Schwelle (m/s)", 4.0, 0.0f, 8.0f);

        {
            constexpr const char* FC = "__re4_knife_fly_cfg";

            // [FESTER WURF] Feste Geschwindigkeit, velocity-unabhaengig -> gutes,
            // vorhersehbares Verhalten.
            lua_table_slider(FC, "speed", "Wurf: Geschwindigkeit (m/s)", 12.0, 4.0f, 30.0f);
            lua_table_slider(FC, "hit_radius", "Wurf: Trefferradius Gegner (m)", 0.5, 0.0f, 1.2f);

            // [WURF-OBJEKTRADIUS] NUR Breakables -- eigener Wert, damit man ihn
            // hochdrehen kann, ohne den Stich-Radius zu verbiegen.
            lua_table_slider(FC, "break_radius", "Wurf: Trefferradius Objekte/Kisten (m)", 0.9, 0.0f, 2.5f);
            lua_table_slider(FC, "max_time", "Wurf: Flugdauer (s)", 1.5, 0.3f, 4.0f);

            // [WURF_RANGE] Danach faellt das Messer IMMER zu Boden (Homing aus).
            lua_table_slider(FC, "max_range", "Wurf: Max Reichweite (m, faellt danach)", 12.0, 2.0f, 40.0f);
            lua_table_slider(FC, "return_delay", "Wurf: Rueckkehr-Zeit (s)", 0.4, 0.0f, 3.0f);

            // [STECKZEIT] Getrennt von der Zeile darueber: DIESER Wert gilt nur,
            // wenn das Messer wirklich getroffen hat.
            lua_table_slider(FC, "hit_return_delay", "Wurf LINKS: Steckzeit nach Treffer (s)", 0.7, 0.0f, 3.0f);

            // [WURF STECKEN] Wie tief die Klinge ueber den Einschlagpunkt hinaus
            // geschoben wird. 0.000 = die Spitze sitzt exakt im Einschlagpunkt,
            // die Klinge steht also noch ganz heraus; ab etwa Klingenlaenge
            // verschwindet auch der Griff. Gilt fuer BEIDE Haende.
            lua_table_slider(FC, "stick_in", "Wurf: Einschub in den Gegner (m)", 0.05, 0.0f, 0.30f, "%.3f");

            // [AN DEN KNOCHEN ZIEHEN] Zwei der vier Trefferwege melden als
            // "Einschlagpunkt" die Flugposition -- die kann bis zu hit_radius vor
            // dem Gegner liegen, dann schwebt die Klinge davor. Dieser Wert
            // begrenzt den Abstand zum naechsten Knochen. 0.000 = aus.
            lua_table_slider(FC, "stick_snap", "Wurf: max Abstand zum Knochen (m)", 0.10, 0.0f, 0.50f, "%.3f");

            lua_table_slider(FC, "spin_fly", "Wurf: Salto vor Treffer (rad/s)", 5.0, 0.0f, 60.0f);
            lua_table_slider(FC, "spin_fall", "Wurf: Salto beim Fallen (rad/s)", 22.0, 0.0f, 40.0f);

            // [RICHTUNGS-CLAMP] Wurfrichtung in einen Kegel um die Blickrichtung
            // zwingen. 90 = frei, klein = immer grob geradeaus.
            lua_table_slider(FC, "dir_max_deg", "Wurf: Richtung geradeaus (Grad, 90=frei)", 90.0, 5.0f, 90.0f);

            // [ZIELHILFE A] Einmalige Korrektur beim Loslassen.
            lua_table_slider(FC, "assist_strength",
                             "Wurf: Zielhilfe A - Anfangs-Korrektur (0=roh, 1=direkt)", 0.0, 0.0f, 1.0f);

            // [ZIELHILFE B] In-Flug-Homing pro Frame (kompoundiert).
            lua_table_slider(FC, "assist_homing", "Wurf: Zielhilfe B - Flug-Homing (0=aus, hoch=Lock)",
                             0.0, 0.0f, 0.5f, "%.3f");
            lua_table_slider(FC, "assist_cone_deg", "Wurf: Zielhilfe Winkel (Grad)", 20.0, 5.0f, 60.0f);

            // [NAHFANG] Der Winkel oben ist auf 2 m nur ein paar Zentimeter breit
            // -- ein naher Gegner faellt raus, ein ferner bleibt drin und wird
            // gewaehlt. Dieser Wert ist die seitliche Mindest-Fangbreite in
            // METERN. 0 = alt.
            lua_table_slider(FC, "assist_min_lat", "Wurf: Zielwahl - Nahbereich-Breite (m, 0=aus)",
                             0.6, 0.0f, 2.0f, "%.2f");

            // [TOTZONE] Der Kollisions-Fuehler ist die ersten Meter aus, damit er
            // nicht den eigenen Arm trifft -- in dieser Strecke wurde ein Gegner
            // direkt vor der Nase NIE getroffen. 0 = aus.
            lua_table_slider(FC, "close_hit_radius", "Wurf: Nahtreffer-Radius in der Totzone (m, 0=aus)",
                             0.5, 0.0f, 1.5f, "%.2f");

            // [ANIMAL-GATE] Nur fuer Tiere: eigene kurze Reichweite und eine
            // HOEHEN-Grenze. Noetig, weil der Zielhilfe-Kegel bewusst nur
            // horizontal misst -- ohne diese Grenze zog ein Wurf meterweit ueber
            // dem Tier es trotzdem an. Gegner unberuehrt.
            lua_table_slider(FC, "animal_range", "Wurf: Tiere - Reichweite (m)", 8.0, 1.0f, 30.0f, "%.1f");
            lua_table_slider(FC, "animal_max_dy", "Wurf: Tiere - max. Hoehen-Ablage (m)", 1.0, 0.1f, 5.0f, "%.2f");

            // [KEGEL] Innen-Winkel = Voll-Lock. Bis hier 100 % Homing, dann
            // linear runter bis 0 am Aussen-Winkel.
            lua_table_slider(FC, "assist_cone_inner_deg", "Wurf: Zielhilfe Innen-Winkel (Voll-Lock, Grad)",
                             8.0, 0.0f, 60.0f);

            // [CENTER] Ziel = Mitte zwischen Fuessen und Kopf (adaptiv).
            lua_table_slider(FC, "assist_target_off_y",
                             "Wurf: Zielhilfe Ziel-Hoehe Offset (m, +hoch/-runter)", 0.0, -1.5f, 1.5f);

            // [ZIELHILFE] Bogen-Ausgleich, subtil gegen den Reichweiten-Abfall.
            lua_table_slider(FC, "assist_flatten", "Wurf: Zielhilfe Bogen-Ausgleich (0=Bogen, 1=gerade)",
                             0.0, 0.0f, 1.0f);

            // [FLUGSALTO-ACHSE] Freie lokale Ueberschlag-Achse feintunen.
            if (ImGui::TreeNode("Flugsalto-Achse (Feintuning)")) {
                ImGui::Text("Achse drehen bis das Messer im Flug sauber geradeaus ueberschlaegt.");
                ImGui::Text("RECHTS-Wurf:");
                lua_table_slider(FC, "spin_ax", "Salto-Achse X (rechts)", 1.0, -1.0f, 1.0f, "%.3f");
                lua_table_slider(FC, "spin_ay", "Salto-Achse Y (rechts)", 0.0, -1.0f, 1.0f, "%.3f");
                lua_table_slider(FC, "spin_az", "Salto-Achse Z (rechts)", 0.0, -1.0f, 1.0f, "%.3f");

                // [SALTO_L] Eigene Achse fuer den LINKS-Wurf (Klon + equipptes
                // Links-Messer). Laesst die Rechts-Werte komplett in Ruhe.
                ImGui::Text("LINKS-Wurf / Clone (eigenstaendig, Rechts bleibt unberuehrt):");
                lua_table_slider(FC, "spin_ax_l", "Salto-Achse X (links)", 1.0, -1.0f, 1.0f, "%.3f");
                lua_table_slider(FC, "spin_ay_l", "Salto-Achse Y (links)", 0.0, -1.0f, 1.0f, "%.3f");
                lua_table_slider(FC, "spin_az_l", "Salto-Achse Z (links)", 0.0, -1.0f, 1.0f, "%.3f");
                ImGui::TreePop();
            }

            // [Y_KORREKTUR] Der Wert liegt im throw_cfg (weapons.lua), wird hier
            // aber bei den anderen Messer-Slidern angezeigt. Gespeichert wird
            // ueber dessen eigenen Speicher-Aufruf, NICHT ueber save_config.
            // [WAISE 05.09.2026] Hier wurde die Lua-Tabelle __re4_throw_cfg
            // abgefragt und ueber __re4_throw_save gespeichert. Beide kamen aus
            // re4_vr_weapons.lua und sind mit dessen Port verschwunden:
            // lua_table_exists lieferte false, die zwei Slider wurden gar nicht
            // mehr gezeichnet. Die Werte liegen jetzt nativ in RE4VRWeapons.
            if (auto& w = RE4VRWeapons::get(); w != nullptr) {
                float kp = w->throw_knife_pitch();

                if (ImGui::SliderFloat("Wurf: Y-Korrektur (auf/ab)", &kp, -0.8f, 0.8f, "%.3f")) {
                    w->set_throw_knife_pitch(kp);
                    w->save_throw_cfg_public();
                }

                float ky = w->throw_knife_yaw();

                if (ImGui::SliderFloat("Wurf: X-Korrektur (links/rechts)", &ky, -0.8f, 0.8f, "%.3f")) {
                    w->set_throw_knife_yaw(ky);
                    w->save_throw_cfg_public();
                }
            }
        }

        // [LANDE_POSE] Rotation des gelandeten Messers -- flach und sauber statt
        // schraeg. Die Werte liegen in RADIANT, die Slider zeigen Grad.
        if (re4vr::lua_table_exists("__re4_knife_land_rot")) {
            constexpr float DEG = 57.2957795130823f;

            for (const auto* ax : {"rx", "ry", "rz"}) {
                float deg = static_cast<float>(re4vr::lua_get_table_number("__re4_knife_land_rot", ax, 0.0)) * DEG;
                const std::string label = std::string{"Wurf: Lande-Rot "} + static_cast<char>(std::toupper(ax[1]))
                                          + " (Grad)";

                if (ImGui::SliderFloat(label.c_str(), &deg, -180.0f, 180.0f)) {
                    re4vr::lua_set_table_number("__re4_knife_land_rot", ax, deg / DEG);
                    save_config();
                }
            }
        }

        ImGui::TreePop();
    }

    // [MENUE-REIHENFOLGE 2026-09-07] Hier stand der Fallback "kein Lua-Dispatcher
    // -> Public-Block selbst zeichnen". Genau der liess die nackten Optionen
    // zwischen den Entwickler-Trees auftauchen, seit ##re4_vr_menu.lua mit dem
    // Port abgeschaltet ist. Gezeichnet wird jetzt zentral in
    // RE4VRMenu::draw_public, in fester Reihenfolge und ganz oben.
    ImGui::TreePop();
}

#endif // RE4
