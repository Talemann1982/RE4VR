// =====================================================================
// RE4VRHolster -- 1:1-Portierung von re4_vr_holster.lua.
// Siehe RE4VRHolster.hpp und I:\LUATRANS\PORT_HOLSTER_SPEC.md (Fassung 2).
//
// Grundregel: KEINE Vereinfachung. Eigenheiten des Originals sind mit
// [WIE_LUA] vermerkt, bewusste Abweichungen mit [ABWEICHUNG].
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
#include <sdk/SystemArray.hpp>
#include <utility/String.hpp>

// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRHolster.hpp"

#undef min
#undef max

namespace {

// ---- Konstanten aus dem Original ------------------------------------------
constexpr double CROUCH_WINDOW_UNUSED = 0.0; // (Platzhalter, hier nicht genutzt)

constexpr const char* KNIFE_CFG_PATH     = "re4_vr/re4_vr_knife.json";       // Lua Z.1361
constexpr const char* KNIFE_CFG_PATH_ADA = "re4_vr/re4_vr_knife_ada.json";   // Lua Z.1397
constexpr const char* PISTOL_CFG_PATH    = "re4_vr/re4_vr_pistol_holster.json";
constexpr const char* GRENADE_CFG_PATH   = "re4_vr/re4_vr_grenade_holster.json";
constexpr const char* SHOULDER_CFG_PATH  = "re4_vr/re4_vr_shoulder_holster.json";
constexpr const char* MAG_CFG_PATH       = "re4_vr/re4_vr_mag_holster.json";
constexpr const char* TAP_CFG_PATH       = "re4_vr/re4_vr_holster_tap.json";

constexpr int32_t KNIFE_ONLY_STAGE = 55302;   // Lua Z.40
constexpr int32_t KRAUSER_KIND     = 200011;  // Lua Z.41
constexpr double  CAL_DELAY        = 5.0;     // Lua Z.1857

constexpr uint32_t HOLSTER_GRAB_SND = 1839787494u;  // Lua Z.1709
constexpr uint32_t GRENADE_GRAB_SND = 3388506884u;  // Lua Z.1711

// Lua Z.149.
const char* const CHEST_JOINT_CANDIDATES[] = {
    "Spine_1", "Spine1", "Spine_2", "Spine2", "Chest", "Kammer", "Spine", "Hip",
};

// Lua Z.329.
bool is_no_weapon_stage(int32_t s) {
    return s == 40500 || s == 40501 || s == 40502 || s == 40510;
}

// Der Lua-State. re4vr::lua_state() liegt in RE4VR.cpp im anonymen Namensraum
// und ist von hier nicht erreichbar -- dieselbe Fassung noch einmal. Die
// Null-Pruefung ist Pflicht: reset_scripts() gibt den State frei und baut ihn
// neu, dazwischen ist der Zeiger null.
re4vr::LuaRef hol_lua_state() {
    // [ABSTURZ 04.09.2026] Sperre des ScriptRunners halten -- s. re4vr::LuaRef.
    return re4vr::LuaRef{};
}

double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// =====================================================================
// Geschuetzte Managed-Zugriffe -- Bauform wie in RE4VRArmChain.cpp:
// 16-Byte-Puffer, weil die Engine via.vec3/via.quat als 16 Byte behandelt,
// und ein Erfolgs-Flag, weil re4vr::call_safe den Unterschied zwischen
// "hat 0 geliefert" und "ging nicht" verschluckt.
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

bool set_vec4(::REManagedObject* obj, std::string_view name, int a, int b, const glm::vec4& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf = v;
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, a, b, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

bool get_vec4(::REManagedObject* obj, std::string_view name, int a, int b, glm::vec4& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{0.0f, 0.0f, 0.0f, 0.0f};
    bool ok = false;

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj, a, b);
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

// =====================================================================
// [FIX 2] System.Guid ist 16 Byte. call_safe<T> nimmt wegen sizeof(T)==8 NICHT
// den Out-Parameter-Zweig -- die Engine schriebe die Guid dann in den als
// Rueckgabepuffer missverstandenen VMContext. Deshalb hier ein eigener,
// 16-Byte-ausgerichteter Puffer, und der Guid wird als WERT weitergereicht
// (inv:equip nimmt ihn ebenfalls per Zeiger auf 16 Byte entgegen).
// =====================================================================
struct alignas(16) GuidBuf {
    uint32_t a{}, b{}, c{}, d{};

    bool operator==(const GuidBuf& o) const {
        return a == o.a && b == o.b && c == o.c && d == o.d;
    }

    bool is_zero() const { return a == 0 && b == 0 && c == 0 && d == 0; }
};

std::string guid_key(const GuidBuf& g) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%u-%u-%u-%u", g.a, g.b, g.c, g.d);
    return buf;
}

template <typename... Args>
bool get_guid(::REManagedObject* obj, std::string_view name, GuidBuf& out, Args... args) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    GuidBuf buf{};
    bool ok = false;

    try {
        method->call_safe<GuidBuf*>(&buf, context, obj, args...);
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

bool call_with_guid(::REManagedObject* obj, std::string_view name, const GuidBuf& g) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    GuidBuf buf = g;
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// [FIX 1] Enum-Rueckgaben kommen als Integer im Rueckgaberegister, NICHT als
// Objektzeiger. Sie an get_type_definition weiterzureichen dereferenziert eine
// Enum-Zahl -> Access Violation beim ersten Inventar-Scan.
std::optional<int32_t> call_enum(::REManagedObject* obj, std::string_view name) {
    int32_t v = 0;
    return re4vr::try_call<int32_t>(obj, name, v) ? std::optional<int32_t>{v} : std::nullopt;
}

::REJoint* joint_by_name(::REManagedObject* transform, const char* name) {
    if (transform == nullptr || name == nullptr) {
        return nullptr;
    }

    auto str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REJoint*>(transform, "getJointByName", str);
}

// Lua Z.725-730: Quaternion aus drei Winkeln in RADIANT, Reihenfolge
// exakt wie dort ausgeschrieben (NICHT der glm-Euler-Konstruktor).
glm::quat quat_from_euler(float rx, float ry, float rz) {
    const float cx = std::cos(rx * 0.5f), sx = std::sin(rx * 0.5f);
    const float cy = std::cos(ry * 0.5f), sy = std::sin(ry * 0.5f);
    const float cz = std::cos(rz * 0.5f), sz = std::sin(rz * 0.5f);

    return glm::quat{cx * cy * cz + sx * sy * sz,
                     sx * cy * cz - cx * sy * sz,
                     cx * sy * cz + sx * cy * sz,
                     cx * cy * sz - sx * sy * cz};
}

float vlen(const glm::vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

ImVec4 abgr_to_vec4(uint32_t c) {
    return ImVec4{static_cast<float>(c & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 24) & 0xFF) / 255.0f};
}

} // namespace

// =====================================================================
// Waffen-ID-Tabellen (Lua Z.108-147) -- Anzahl maschinell geprueft:
// KNIFE 8, PISTOL 14, GRENADE 3, SHOULDER 28.
// =====================================================================
namespace {
const std::unordered_set<int32_t> KNIFE_IDS{
    5000, 5001, 5002, 5003, 5006, 6107, 6108, 6305,
};

const std::unordered_set<int32_t> PISTOL_IDS{
    4000, 4001, 4002, 4003, 4004,
    4500, 4501, 4502, 6000, 6103, 6112, 6113, 6300, 6301,
};

const std::unordered_set<int32_t> GRENADE_IDS{
    5400, 5401, 5402,
};

const std::unordered_set<int32_t> SHOULDER_IDS{
    4100, 4101, 4102,             // W-870, Riot Gun, Striker
    4200, 4201, 4202,             // TMP, Chicago Sweeper, LE 5
    4400, 4401, 4402,             // SR M1903, Stingray, CQBR
    4600,                         // Bolt Thrower
    4701, 4702,                   // Flamethrower, P.R.L. 9412
    4900, 4901, 4902,             // Rocket Launcher x3
    6001,                         // DLC Skull Shaker
    6100, 6101, 6102, 6104, 6105, 6106, 6111, 6114,
    6304,                         // MC EJF-338 Compound Bow
    4800, 4801, 6109,             // unreleased, Lua Z.144-146
};
} // namespace

std::shared_ptr<RE4VRHolster>& RE4VRHolster::get() {
    static auto inst = std::make_shared<RE4VRHolster>();
    return inst;
}

// =====================================================================
// Konfiguration
// =====================================================================
void RE4VRHolster::default_cfg(nlohmann::json& c) {
    // Lua Z.783-806.
    c = nlohmann::json::object();
    c["enabled"] = true;
    c["off_x"] = 0.10f; c["off_y"] = 0.10f; c["off_z"] = 0.12f;
    c["rx"] = 0.0f; c["ry"] = 0.0f; c["rz"] = 0.0f;
    c["scale"] = 1.0f;
    c["smooth"] = 0.8f;
    c["grab_trigger"] = 0.16f;
    c["grab_release"] = 0.24f;
    c["grab_haptic"] = true;
    c["grab_haptic_delay"] = 0.0f;
    c["pl_x"] = 0.0f; c["pl_y"] = 0.0f; c["pl_z"] = 0.0f;
    c["md_x"] = 0.0f; c["md_y"] = 0.0f; c["md_z"] = 0.0f;
    c["md_x_wid"] = nlohmann::json::object();   // KEYS SIND STRINGS (Lua Z.795)
    c["dim_in_use"] = true;
    c["dim_factor"] = 0.12f;
    c["joint"] = "";
    c["zx"] = 0.18f; c["zy"] = 0.20f; c["zz"] = -0.20f;
}

void RE4VRHolster::load_slot_cfg(nlohmann::json& cfg, const std::string& path) {
    // Lua Z.808-815: Migration alter chest_*-Keys, danach nur Keys uebernehmen,
    // die es in der Vorlage gibt UND typgleich sind.
    const auto d = re4vr::json_load(path);

    if (!d.is_object()) {
        return;
    }

    static const std::pair<const char*, const char*> MAP[] = {
        {"chest_enabled", "enabled"}, {"chest_off_x", "off_x"}, {"chest_off_y", "off_y"},
        {"chest_off_z", "off_z"},     {"chest_rx", "rx"},       {"chest_ry", "ry"},
        {"chest_rz", "rz"},           {"chest_scale", "scale"}, {"chest_smooth", "smooth"},
        {"chest_joint", "joint"},
    };

    const auto same_kind = [](const nlohmann::json& a, const nlohmann::json& b) {
        if (a.is_boolean() && b.is_boolean()) { return true; }
        if (a.is_number() && b.is_number()) { return true; }
        if (a.is_string() && b.is_string()) { return true; }
        if (a.is_object() && b.is_object()) { return true; }
        return false;
    };

    for (const auto& [oldk, newk] : MAP) {
        if (d.contains(oldk) && !d[oldk].is_null() && cfg.contains(newk)
            && same_kind(d[oldk], cfg[newk])) {
            cfg[newk] = d[oldk];
        }
    }

    for (auto it = d.begin(); it != d.end(); ++it) {
        if (cfg.contains(it.key()) && !cfg[it.key()].is_null()
            && same_kind(it.value(), cfg[it.key()])) {
            cfg[it.key()] = it.value();
        }
    }
}

void RE4VRHolster::save_slot_cfg(const nlohmann::json& cfg, const std::string& path) {
    // [KNIFE_ADA HARD-GUARD] Lua Z.823-828: waehrend Ada gesteuert wird, darf
    // Leons Messer-Datei physisch nicht geschrieben werden -- und umgekehrt.
    // Pfade bewusst als LITERALE verglichen, genau wie im Original.
    const std::string ch = re4vr::lua_get_string("__re4_knife_char");

    if (ch == "ada" && path == KNIFE_CFG_PATH) {
        return;
    }

    if (ch == "leon" && path == KNIFE_CFG_PATH_ADA) {
        return;
    }

    re4vr::json_save(path, cfg, 4);
}

void RE4VRHolster::load_tap_cfg() {
    // Lua Z.1692-1699. Die beiden Werte werden zusaetzlich als Member gehalten,
    // damit sie einen Script-Reset ueberleben (siehe publish_state_globals).
    m_crouch_gain = 0.0;
    m_ada_mesh_z = 0.0;

    for (auto& e : m_merc_mesh_z) {
        e.z = 0.0;
    }

    const auto d = re4vr::json_load(TAP_CFG_PATH);

    if (d.is_object()) {
        if (d.contains("aim_hold") && d["aim_hold"].is_number()) {
            m_aim_hold = d["aim_hold"].get<float>();
        }

        if (d.contains("crouch_gain") && d["crouch_gain"].is_number()) {
            m_crouch_gain = d["crouch_gain"].get<double>();
        }

        if (d.contains("ada_mesh_z") && d["ada_mesh_z"].is_number()) {
            m_ada_mesh_z = d["ada_mesh_z"].get<double>();
        }

        // [MERCS MESH-Z PRO CHARAKTER 17.09.2026] Objekt {Body-GO-Name: Wert}.
        // Der alte gemeinsame Skalar "merc_mesh_z" wird BEWUSST nicht mehr
        // gelesen -- er galt fuer alle Charaktere zugleich und wuerde die
        // frisch getrennten Werte wieder gleichziehen.
        if (d.contains("merc_mesh_z_body") && d["merc_mesh_z_body"].is_object()) {
            const auto& mb = d["merc_mesh_z_body"];

            for (auto& e : m_merc_mesh_z) {
                if (mb.contains(e.body) && mb[e.body].is_number()) {
                    e.z = mb[e.body].get<double>();
                }
            }
        }
    }

    re4vr::lua_set_number("__re4_holster_crouch_gain", m_crouch_gain);
    re4vr::lua_set_number("__re4_holster_ada_mesh_z", m_ada_mesh_z);
}

void RE4VRHolster::save_tap_cfg() {
    nlohmann::json j = nlohmann::json::object();
    j["aim_hold"] = m_aim_hold;
    j["crouch_gain"] = m_crouch_gain;
    j["ada_mesh_z"] = m_ada_mesh_z;

    // [MERCS MESH-Z PRO CHARAKTER] Nur noch das Objekt -- der alte Skalar
    // "merc_mesh_z" wird nicht mehr geschrieben und verschwindet beim
    // naechsten Speichern aus der Datei.
    nlohmann::json mb = nlohmann::json::object();

    for (const auto& e : m_merc_mesh_z) {
        mb[e.body] = e.z;
    }

    j["merc_mesh_z_body"] = mb;
    re4vr::json_save(TAP_CFG_PATH, j, 4);
}

// =====================================================================
// Spieler / Inventar
// =====================================================================
::REManagedObject* RE4VRHolster::get_ctx() {
    // Lua Z.160-165. Der Lua-Frame-Cache wird nativ nicht befragt -- ein
    // Ruecksprung nach Lua waere teurer als der Weg selbst und liefert
    // dasselbe Objekt.
    if (m_character_manager == nullptr || !re4vr::obj_ok(m_character_manager)) {
        m_character_manager = re4vr::character_manager();
    }

    if (m_character_manager == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

::REManagedObject* RE4VRHolster::body_tf() {
    // Lua Z.166-171.
    const auto ctx = get_ctx();

    if (ctx == nullptr) {
        return nullptr;
    }

    const auto b = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
    return b != nullptr ? re4vr::call_safe<::REManagedObject*>(b, "get_Transform") : nullptr;
}

std::optional<int32_t> RE4VRHolster::get_equip_wid() {
    // Lua Z.172-181: userdata -> value__, sonst direkt die Zahl.
    const auto ctx = get_ctx();

    if (ctx == nullptr) {
        return std::nullopt;
    }

    const auto hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return std::nullopt;
    }

    int32_t wid = 0;

    if (!re4vr::try_call<int32_t>(hu, "get_EquipWeaponID", wid)) {
        return std::nullopt;
    }

    return wid;
}

::REManagedObject* RE4VRHolster::get_pe() {
    // Lua Z.253-259.
    if (m_pe != nullptr && re4vr::obj_ok(m_pe)
        && re4vr::call_safe<::REManagedObject*>(m_pe, "get_Context") != nullptr) {
        return m_pe;
    }

    const auto head = re4vr::head_game_object();

    if (head == nullptr) {
        return nullptr;
    }

    m_pe = re4vr::get_component(head, m_pe_td);
    return m_pe;
}

::REManagedObject* RE4VRHolster::get_inventory() {
    const auto pe = get_pe();
    return pe != nullptr ? re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController")
                         : nullptr;
}

RE4VRHolster::InHand RE4VRHolster::weapon_actually_in_hand() {
    // Lua Z.187-223. Die vier Rueckgabewerte samt BARE_FIX exakt nachgebaut,
    // siehe Spec 4.
    InHand r{};

    const auto ctx = get_ctx();

    if (ctx == nullptr) {
        return r; // valid = false  (Lua: nil)
    }

    const auto hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return r;
    }

    bool gun = false, knife = false, gren = false, melee = false;
    const bool has_gun   = re4vr::try_call<bool>(hu, "get_IsEquipGun", gun);
    const bool has_knife = re4vr::try_call<bool>(hu, "get_IsEquipKnife", knife);
    const bool has_gren  = re4vr::try_call<bool>(hu, "get_IsEquipGrenade", gren);
    const bool has_melee = re4vr::try_call<bool>(hu, "get_IsEquipMelee", melee);

    // [BARE_FIX] Lua Z.209-218: nur wenn ALLE VIER Getter nil liefern.
    if (!has_gun && !has_knife && !has_gren && !has_melee) {
        int32_t wid = 0;

        // [WIE_LUA / FIX 11] Hier bewusst OHNE das value__-Auspacken aus
        // get_equip_wid: liefert der Getter ein OBJEKT, ist `tonumber` in Lua
        // nil und die Funktion gibt "wirklich unbekannt" zurueck. Nativ kommt
        // immer eine Zahl an -- der Pfad waere sonst unerreichbar. Deshalb den
        // Rueckgabetyp einmal pruefen: ist er kein Wertetyp, gilt derselbe
        // "unbekannt"-Fall wie in Lua.
        if (!m_equip_wid_is_value.has_value()) {
            m_equip_wid_is_value = false;

            if (auto* def = utility::re_managed_object::get_type_definition(hu)) {
                if (const auto m = def->get_method("get_EquipWeaponID")) {
                    if (auto* rt = m->get_return_type()) {
                        m_equip_wid_is_value = rt->is_value_type();
                    }
                }
            }
        }

        if (!*m_equip_wid_is_value) {
            return r;   // Objekt-Rueckgabe -> in Lua nil -> nicht anfassen
        }

        if (!re4vr::try_call<int32_t>(hu, "get_EquipWeaponID", wid)) {
            return r; // nil
        }

        if (wid < 0) {
            // Lua gibt hier NUR DREI Werte zurueck -- der vierte ist nil,
            // nicht false. Daran haengt der Snapshot (`not in_hand_ambiguous`).
            r.valid = true;
            r.in_hand = false;
            r.knife = false;
            r.grenade = false;
            r.ambiguous_valid = false;
            r.ambiguous = false;
            return r;
        }

        r.valid = true;
        r.in_hand = true;
        r.knife = false;
        r.grenade = false;
        r.ambiguous_valid = true;
        r.ambiguous = true;
        return r;
    }

    // [WIE_LUA] IsEquipMelee wird abgefragt, zaehlt aber NICHT als "in Hand"
    // (bare-hands Faust-Stance) -- es wirkt nur im Test oben.
    r.valid = true;
    r.in_hand = (has_gun && gun) || (has_knife && knife) || (has_gren && gren);
    r.knife = has_knife && knife;
    r.grenade = has_gren && gren;
    r.ambiguous_valid = true;
    r.ambiguous = false;
    return r;
}

std::string RE4VRHolster::row_guid_key(::REManagedObject* row) {
    // [FIX 2] get_ID liefert eine 16-Byte-Guid -- als WERT holen, nicht als Zeiger.
    GuidBuf g{};

    if (row == nullptr || !get_guid(row, "get_ID", g) || g.is_zero()) {
        return {};
    }

    return guid_key(g);
}

std::vector<::REManagedObject*> RE4VRHolster::inventory_weapon_rows(::REManagedObject* inv) {
    // Lua Z.301-309.
    std::vector<::REManagedObject*> out;

    if (inv == nullptr) {
        return out;
    }

    const auto list = re4vr::call_safe<::REManagedObject*>(inv, "getInventoryItemList");

    if (list == nullptr) {
        return out;
    }

    int32_t n = 0;

    if (!re4vr::try_call<int32_t>(list, "get_Count", n)) {
        return out;
    }

    for (int32_t i = 0; i < n; ++i) {
        const auto row = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (row == nullptr) {
            continue;
        }

        const auto wid = call_enum(row, "get_WeaponId");

        if (wid.has_value() && *wid != 0) {
            out.push_back(row);
        }
    }

    return out;
}

bool RE4VRHolster::has_weapon_in_inventory(const std::unordered_set<int32_t>& ids) {
    // Lua Z.312-319.
    const auto inv = get_inventory();

    if (inv == nullptr) {
        return false;
    }

    for (auto* row : inventory_weapon_rows(inv)) {
        const auto wid = call_enum(row, "get_WeaponId");

        if (wid.has_value() && ids.count(*wid) != 0) {
            return true;
        }
    }

    return false;
}

::REManagedObject* RE4VRHolster::find_row_for_weapon(::REManagedObject* inv, int32_t wid,
                                                     const std::string& guid) {
    // Lua Z.343-351.
    if (inv == nullptr || wid == 0) {
        return nullptr;
    }

    std::string want;

    if (!guid.empty()) {
        want = guid;
        std::transform(want.begin(), want.end(), want.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    const auto rows = inventory_weapon_rows(inv);

    if (!want.empty()) {
        for (auto* row : rows) {
            const auto gid = row_guid_key(row);

            if (!gid.empty() && gid == want) {
                return row;
            }
        }
    }

    for (auto* row : rows) {
        const auto w = call_enum(row, "get_WeaponId");

        if (w.has_value() && *w == wid) {
            return row;
        }
    }

    return nullptr;
}

std::optional<int32_t> RE4VRHolster::get_equip_type_main() {
    // Lua Z.352-358. [FIX 3] Ein statisches Enum-Literal liefert ueber
    // get_data_raw die ADRESSE der Ablage -- gebraucht wird der WERT.
    if (m_equip_type_main.has_value()) {
        return m_equip_type_main;
    }

    auto* td = sdk::find_type_definition(game_namespace("EquipType"));
    const auto f = td != nullptr ? td->get_field("Main") : nullptr;

    if (f == nullptr) {
        return std::nullopt;
    }

    try {
        m_equip_type_main = *reinterpret_cast<int32_t*>(f->get_data_raw(nullptr, false));
    } catch (...) {
        m_equip_type_main.reset();
    }

    return m_equip_type_main;
}

std::optional<int32_t> RE4VRHolster::weaponid_enum(int32_t wid) {
    // Lua Z.415-419. [FIX 3] wie oben: Wert, nicht Adresse.
    if (m_wid_td == nullptr) {
        return std::nullopt;
    }

    const auto f = m_wid_td->get_field(("wp" + std::to_string(wid)).c_str());

    if (f == nullptr) {
        return std::nullopt;
    }

    try {
        return *reinterpret_cast<int32_t*>(f->get_data_raw(nullptr, false));
    } catch (...) {
        return std::nullopt;
    }
}

std::string RE4VRHolster::get_equipped_main_guid(::REManagedObject* inv) {
    // Lua Z.444-447.
    const auto et = get_equip_type_main();

    if (inv == nullptr || !et.has_value()) {
        return {};
    }

    GuidBuf g{};

    if (!get_guid(inv, "getEquippedID", g, *et) || g.is_zero()) {
        return {};
    }

    return guid_key(g);
}

::REManagedObject* RE4VRHolster::find_weapon_go(const std::unordered_set<int32_t>& ids,
                                                std::optional<int32_t> want_wid,
                                                std::optional<int32_t>* out_wid) {
    // Lua Z.226-248: Baum ab der Body-Transform, Tiefe <= 5, Namen "wpXXXX".
    const auto tf = body_tf();

    if (tf == nullptr) {
        return nullptr;
    }

    ::REManagedObject* any_go = nullptr;
    std::optional<int32_t> any_wid{};
    ::REManagedObject* want_go = nullptr;
    std::optional<int32_t> want_go_wid{};

    // Rekursion wie im Original; want_go bricht die Suche ab.
    const std::function<void(::REManagedObject*, int)> walk =
        [&](::REManagedObject* t, int depth) {
            if (t == nullptr || depth > 5 || want_go != nullptr) {
                return;
            }

            const auto go = re4vr::call_safe<::REManagedObject*>(t, "get_GameObject");
            const std::string nm = go != nullptr ? re4vr::obj_name(go) : std::string{};

            if (nm.rfind("wp", 0) == 0 && nm.size() > 2) {
                size_t end = 2;

                while (end < nm.size() && nm[end] >= '0' && nm[end] <= '9') {
                    ++end;
                }

                if (end > 2) {
                    const int32_t id = std::atoi(nm.substr(2, end - 2).c_str());

                    // [VALID] Lua Z.237: tote wpXXXX-Instanzen nach Save-Load
                    // ueberspringen, sonst klont das Holster ein totes Mesh.
                    bool valid = true;
                    bool v = false;

                    if (re4vr::try_call<bool>(go, "get_Valid", v)) {
                        valid = v;
                    }

                    if (ids.count(id) != 0 && valid) {
                        if (any_go == nullptr) {
                            any_go = go;
                            any_wid = id;
                        }

                        if (want_wid.has_value() && id == *want_wid) {
                            want_go = go;
                            want_go_wid = id;
                        }
                    }
                }
            }

            auto c = re4vr::call_safe<::REManagedObject*>(t, "get_Child");

            while (c != nullptr && want_go == nullptr) {
                walk(c, depth + 1);
                c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
            }
        };

    walk(tf, 0);

    if (want_wid.has_value() && want_go != nullptr) {
        if (out_wid != nullptr) {
            *out_wid = want_go_wid;
        }

        return want_go;
    }

    if (out_wid != nullptr) {
        *out_wid = any_wid;
    }

    return any_go;
}

// =====================================================================
// Slot-Mechanik
// =====================================================================
void RE4VRHolster::store_slot_joint(Slot& s, ::REJoint* j) {
    // [REF] Gecachte Engine-Objekte brauchen add_ref -- in Lua macht das
    // sol_lua_push unsichtbar mit (Sdk.cpp Z.49-68).
    if (s.joint == j) {
        return;
    }

    if (s.joint != nullptr && s.joint_reffed) {
        utility::re_managed_object::release(reinterpret_cast<::REManagedObject*>(s.joint));
    }

    s.joint = nullptr;
    s.joint_reffed = false;

    if (j == nullptr || !utility::re_managed_object::is_managed_object(j)) {
        return;
    }

    auto obj = reinterpret_cast<::REManagedObject*>(j);

    if (static_cast<int32_t>(obj->referenceCount) > 0) {
        utility::re_managed_object::add_ref(obj);
        s.joint_reffed = true;
    }

    s.joint = j;
}

void RE4VRHolster::store_clone(Slot& s, ::REManagedObject* go, ::REManagedObject* mesh,
                               ::REManagedObject* tf) {
    // [REF] Klon, Mesh und Transform leben ueber Frames hinweg und brauchen
    // deshalb ALLE DREI eine eigene Referenz. Im Original steht fuer das
    // GameObject ein ausdrueckliches go:add_ref() (Lua Z.873) -- das ist
    // BEDINGUNGSLOS, anders als die sol-Heuristik. Mesh und Transform haelt
    // dort die sol-Referenz am Leben.
    if (s.clone_obj != nullptr && s.clone_reffed && re4vr::obj_ok(s.clone_obj)) {
        utility::re_managed_object::release(s.clone_obj);
    }

    if (s.clone_mesh != nullptr && s.clone_mesh_reffed && re4vr::obj_ok(s.clone_mesh)) {
        utility::re_managed_object::release(s.clone_mesh);
    }

    if (s.clone_tf != nullptr && s.clone_tf_reffed && re4vr::obj_ok(s.clone_tf)) {
        utility::re_managed_object::release(s.clone_tf);
    }

    s.clone_obj = nullptr;
    s.clone_mesh = nullptr;
    s.clone_tf = nullptr;
    s.clone_reffed = false;
    s.clone_mesh_reffed = false;
    s.clone_tf_reffed = false;

    const auto keep = [](::REManagedObject* o, bool& reffed, bool unconditional) {
        if (o == nullptr || !re4vr::obj_ok(o)) {
            return;
        }

        if (unconditional || static_cast<int32_t>(o->referenceCount) > 0) {
            utility::re_managed_object::add_ref(o);
            reffed = true;
        }
    };

    keep(go, s.clone_reffed, true);        // Lua: go:add_ref() bedingungslos
    keep(mesh, s.clone_mesh_reffed, false);
    keep(tf, s.clone_tf_reffed, false);

    s.clone_obj = go;
    s.clone_mesh = mesh;
    s.clone_tf = tf;
}

::REJoint* RE4VRHolster::slot_joint(Slot& s) {
    // Lua Z.838-855.
    const auto tf = body_tf();

    if (tf == nullptr) {
        return nullptr;
    }

    const bool perf_on = !re4vr::lua_get_bool("__re4_hol_perf_off", false);

    // [PERF 3] Ein Joint kann innerhalb eines Frames nicht ungueltig werden.
    if (perf_on && s.joint != nullptr && s.joint_ok_frame == m_hol_frame) {
        return s.joint;
    }

    bool valid = false;

    if (s.joint != nullptr && utility::re_managed_object::is_managed_object(s.joint)) {
        bool v = false;

        if (re4vr::try_call<bool>(reinterpret_cast<::REManagedObject*>(s.joint), "get_Valid", v)) {
            valid = v;
        }
    }

    if (valid) {
        s.joint_ok_frame = m_hol_frame;
    } else {
        store_slot_joint(s, nullptr);

        for (const char* nm : CHEST_JOINT_CANDIDATES) {
            const auto j = joint_by_name(tf, nm);

            if (j != nullptr) {
                store_slot_joint(s, j);
                s.cfg["joint"] = std::string{nm};
                break;
            }
        }
    }

    return s.joint;
}

void RE4VRHolster::destroy_slot(Slot& s) {
    // Lua Z.856-867.
    if (s.clone_obj != nullptr) {
        re4vr::destroy_game_object(s.clone_obj);
    }

    store_clone(s, nullptr, nullptr, nullptr);
    s.clone_wid.reset();
    s.scale_written.reset();
    s.mat_built = false;
    s.mat_dim.clear();
    s.mat_zero.clear();
    s.dim_applied.reset();
    s.src_addr = 0;
    s.part0_done = false;
    s.parented = false;
    s.sm_has = false;
}

bool RE4VRHolster::spawn_slot(Slot& s, ::REManagedObject* gmesh) {
    // Lua Z.868-907.
    if (gmesh == nullptr) {
        return false;
    }

    const auto holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    const auto gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");

    const auto go = reinterpret_cast<::REManagedObject*>(
        re4vr::create_game_object("vr_holster_" + s.name));

    if (go == nullptr) {
        return false;
    }

    // [ABWEICHUNG, bewusst] Das Original setzt go:add_ref() SOFORT (Lua Z.873)
    // und verlaesst die Funktion bei fehlschlagendem Mesh mit `return false`,
    // OHNE das GO zu zerstoeren -- in Lua faengt das der GC auf, mit einem
    // echten Referenzzaehler waere es ein Leck pro Frame. Deshalb wird hier
    // erst nach dem erfolgreichen Mesh gezaehlt; das Verhalten ist identisch.
    if (auto* motion_rt = re4vr::runtime_type("via.motion.Motion")) {
        re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", motion_rt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* mesh_rt = re4vr::runtime_type("via.render.Mesh")) {
        mesh = re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", mesh_rt);
    }

    if (mesh == nullptr) {
        re4vr::destroy_game_object(go);
        return false;
    }

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (gmat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", gmat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    // [KEIN_SCHATTEN] Holster-Klone werfen keinen Schatten (Lua Z.882).
    re4vr::call_safe<void*>(mesh, "set_DrawShadowCast", false);

    slot_joint(s);   // loest cfg.joint auf

    const auto ctf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    // [CRASH-FIX] Lua Z.888-897: set_Parent auf einen STALE Body-Transform
    // loest eine native Access Violation aus, die pcall NICHT faengt. Deshalb
    // Body-GO frisch holen und BEIDE Gueltigkeiten pruefen.
    bool go_valid = true;
    {
        bool v = false;

        if (re4vr::try_call<bool>(go, "get_Valid", v)) {
            go_valid = v;
        }
    }

    const auto bctx = get_ctx();
    const auto bgo = bctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(bctx, "get_BodyGameObject")
        : nullptr;

    bool bgo_valid = false;

    if (bgo != nullptr) {
        bgo_valid = true;
        bool v = false;

        if (re4vr::try_call<bool>(bgo, "get_Valid", v)) {
            bgo_valid = v;
        }
    }

    const auto btf = (bgo != nullptr && bgo_valid)
        ? re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform")
        : nullptr;

    s.parented = false;

    if (ctf != nullptr && btf != nullptr && go_valid && bgo_valid) {
        re4vr::call_safe<void*>(ctf, "set_Parent", btf);

        std::string jn = re4vr::j_str(s.cfg, "joint", "");

        if (jn.empty()) {
            jn = "Spine_1";
        }

        auto str = sdk::VM::create_managed_string(utility::widen(jn));

        if (str != nullptr) {
            const auto m = find_method(ctf, "set_ParentJoint");

            if (m != nullptr) {
                auto context = sdk::get_thread_context();
                bool ok = false;

                try {
                    m->call_safe<void*>(context, ctf, str);
                    ok = true;
                } catch (...) {
                    ok = false;
                }

                s.parented = clear_pending(context, ok);
            }
        }
    }

    store_clone(s, go, mesh, ctf);
    s.mat_built = false;
    s.mat_dim.clear();
    s.mat_zero.clear();
    s.dim_applied.reset();
    return true;
}

void RE4VRHolster::build_mat_dim(Slot& s) {
    // Lua Z.908-929.
    if (s.clone_mesh == nullptr) {
        return;
    }

    int32_t mnum = 0;

    if (!re4vr::try_call<int32_t>(s.clone_mesh, "get_MaterialNum", mnum) || mnum == 0) {
        return;
    }

    s.mat_dim.clear();
    s.mat_zero.clear();
    s.mat_built = true;

    for (int32_t mi = 0; mi < mnum; ++mi) {
        int32_t vnum = 0;

        if (!re4vr::try_call<int32_t>(s.clone_mesh, "getMaterialVariableNum", vnum, mi)) {
            continue;
        }

        for (int32_t vi = 0; vi < vnum; ++vi) {
            const auto vn = re4vr::call_safe<::REManagedObject*>(
                s.clone_mesh, "getMaterialVariableName", mi, vi);

            if (vn == nullptr) {
                continue;
            }

            std::string low;

            try {
                low = utility::re_string::get_string(reinterpret_cast<::SystemString*>(vn));
            } catch (...) {
                continue;
            }

            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            const bool is_color = low.find("color") != std::string::npos
                || low.find("albedo") != std::string::npos
                || low.find("diffuse") != std::string::npos
                || low.find("basecol") != std::string::npos;

            if (is_color) {
                glm::vec4 f4{};

                if (get_vec4(s.clone_mesh, "getMaterialFloat4", mi, vi, f4)) {
                    s.mat_dim.push_back(Slot::Mat4{mi, vi, f4.x, f4.y, f4.z, f4.w});
                }
            } else if (low == "metallic" || low == "cavity") {
                float v = 0.0f;

                if (re4vr::try_call<float>(s.clone_mesh, "getMaterialFloat", v, mi, vi)) {
                    s.mat_zero.push_back(Slot::Mat1{mi, vi, v});
                }
            }
        }
    }
}

bool RE4VRHolster::apply_dim(Slot& s, bool dark) {
    // Lua Z.930-941.
    if (s.clone_mesh == nullptr) {
        return false;
    }

    if (!s.mat_built) {
        build_mat_dim(s);
    }

    if (!s.mat_built) {
        return false;
    }

    const float d = dark ? re4vr::j_num(s.cfg, "dim_factor", 0.12f) : 1.0f;

    for (const auto& e : s.mat_dim) {
        set_vec4(s.clone_mesh, "setMaterialFloat4", e.mi, e.vi,
                 glm::vec4{e.x * d, e.y * d, e.z * d, e.w});
    }

    for (const auto& e : s.mat_zero) {
        const auto m = find_method(s.clone_mesh, "setMaterialFloat");

        if (m == nullptr) {
            continue;
        }

        auto context = sdk::get_thread_context();

        try {
            m->call_safe<void*>(context, s.clone_mesh, e.mi, e.vi, dark ? 0.0f : e.orig);
        } catch (...) {
        }

        clear_pending(context, true);
    }

    return true;
}

bool RE4VRHolster::isolate_part0(Slot& s) {
    // Lua Z.946-959.
    if (s.clone_mesh == nullptr) {
        return false;
    }

    bool ready = false;

    if (!re4vr::try_call<bool>(s.clone_mesh, "get_MeshReady", ready) || !ready) {
        return false;
    }

    // [KILLER7_PARTS] wp4501 traegt ihren Laseraufsatz als eigenen Part und
    // zeigt deshalb IMMER alle Parts (Lua Z.951-956).
    const bool all = s.all_parts
        || (s.clone_wid.has_value() && *s.clone_wid == 4501);

    for (int32_t i = 0; i < 64; ++i) {
        const auto m = find_method(s.clone_mesh, "setPartsEnable");

        if (m == nullptr) {
            break;
        }

        auto context = sdk::get_thread_context();

        try {
            m->call_safe<void*>(context, s.clone_mesh, i, all || i == 0);
        } catch (...) {
        }

        clear_pending(context, true);
    }

    return true;
}

// [MERCS MESH-Z PRO CHARAKTER] Der Wert des gerade gespielten Mercs-Charakters.
// Den Index setzt on_frame einmal pro Frame (bei m_in_mercs_now), hier wird nur
// gelesen -- crouch_opt_z laeuft je Slot und Pass.
double RE4VRHolster::merc_mesh_z_now() const {
    if (m_merc_mesh_z_idx < 0 || m_merc_mesh_z_idx >= MERC_MESH_Z_COUNT) {
        return 0.0;
    }

    return m_merc_mesh_z[m_merc_mesh_z_idx].z;
}

float RE4VRHolster::crouch_opt_z() const {
    // Lua Z.965-973.
    // [FIX 18] Nur EIN Lua-Zugriff statt drei je Slot und Pass -- gain, ada_mesh_z
    // und der Charakter liegen als Member vor.
    const double d = re4vr::lua_get_number("__re4_ub_z_delta", 0.0);
    const double g = m_crouch_gain;
    // [MERCS MESH-Z PRO CHARAKTER 17.09.2026] In Mercenaries gilt NUR der Wert
    // des dort gespielten Charakters -- auch wenn m_knife_char noch "ada" von
    // vorher traegt. Der Kampagnen-Zweig darunter ist UNVERAENDERT: ausserhalb
    // von Mercenaries entscheidet weiterhin allein m_knife_char ueber Adas Wert.
    const double az = m_in_mercs_now ? merc_mesh_z_now()
                                     : ((m_knife_char == "ada") ? m_ada_mesh_z : 0.0);

    return static_cast<float>(-d * g + az);
}

bool RE4VRHolster::compute_target(Slot& s, glm::vec3& out_pos,
                                  std::optional<glm::quat>& out_rot) {
    // Lua Z.974-989.
    const auto j = slot_joint(s);

    if (j == nullptr) {
        return false;
    }

    auto jobj = reinterpret_cast<::REManagedObject*>(j);
    glm::vec3 jp{};

    if (!get_vec3(jobj, "get_Position", jp)) {
        return false;
    }

    glm::quat jr{};
    const bool have_jr = get_quat(jobj, "get_Rotation", jr);

    out_pos = jp;
    out_rot.reset();

    if (have_jr) {
        // [SHIFT_X] gilt fuer BEIDE Anker-Schreiber (Lua Z.983-986).
        const glm::vec3 local{re4vr::j_num(s.cfg, "off_x", 0.0f) + re4vr::j_num(s.cfg, "shift_x", 0.0f),
                              re4vr::j_num(s.cfg, "off_y", 0.0f),
                              re4vr::j_num(s.cfg, "off_z", 0.0f)};
        const glm::vec3 off = jr * local;
        out_pos = jp + off;

        out_rot = glm::normalize(jr * quat_from_euler(re4vr::j_num(s.cfg, "rx", 0.0f),
                                                      re4vr::j_num(s.cfg, "ry", 0.0f),
                                                      re4vr::j_num(s.cfg, "rz", 0.0f)));
    }

    return true;
}

bool RE4VRHolster::compute_zone_anchor(Slot& s, glm::vec3& out) {
    // Lua Z.993-1000.
    glm::vec3 hmd{}, right{}, up{}, fwd{};

    if (!hmd_pose_yaw(hmd, right, up, fwd)) {
        return false;
    }

    const float ox = re4vr::j_num(s.cfg, "zx", 0.0f);
    const float oy = re4vr::j_num(s.cfg, "zy", 0.0f);
    const float oz = re4vr::j_num(s.cfg, "zz", 0.0f);

    out = glm::vec3{hmd.x + ox * right.x + oy * up.x + oz * fwd.x,
                    hmd.y + ox * right.y + oy * up.y + oz * fwd.y,
                    hmd.z + ox * right.z + oy * up.z + oz * fwd.z};
    return true;
}

void RE4VRHolster::update_smooth(Slot& s) {
    // Lua Z.1016-1049.
    // [DETACHED_ZONE] Der Anker wird ZUERST gesetzt -- vor dem Early-Return.
    if (s.detached_zone) {
        glm::vec3 za{};

        if (compute_zone_anchor(s, za)) {
            re4vr::lua_set_vec3(s.anchor_g.c_str(), za);
        }
    }

    glm::vec3 tp{};
    std::optional<glm::quat> tr{};

    if (!compute_target(s, tp, tr)) {
        return;
    }

    const float a = 1.0f - std::max(0.0f, std::min(0.98f, re4vr::j_num(s.cfg, "smooth", 0.0f)));

    if (!s.sm_has) {
        s.sm_p = tp;

        if (tr.has_value()) {
            s.sm_r = *tr;
        }

        s.sm_has = true;
    } else {
        s.sm_p = s.sm_p + (tp - s.sm_p) * a;

        if (tr.has_value()) {
            const glm::quat t = *tr;
            const float dot = s.sm_r.x * t.x + s.sm_r.y * t.y + s.sm_r.z * t.z + s.sm_r.w * t.w;
            const float sg = dot < 0.0f ? -1.0f : 1.0f;

            s.sm_r.x = s.sm_r.x + (t.x * sg - s.sm_r.x) * a;
            s.sm_r.y = s.sm_r.y + (t.y * sg - s.sm_r.y) * a;
            s.sm_r.z = s.sm_r.z + (t.z * sg - s.sm_r.z) * a;
            s.sm_r.w = s.sm_r.w + (t.w * sg - s.sm_r.w) * a;

            const float n = std::sqrt(s.sm_r.x * s.sm_r.x + s.sm_r.y * s.sm_r.y
                                      + s.sm_r.z * s.sm_r.z + s.sm_r.w * s.sm_r.w);

            if (n > 1e-6f) {
                s.sm_r.x /= n; s.sm_r.y /= n; s.sm_r.z /= n; s.sm_r.w /= n;
            }
        }
    }

    glm::vec3 anchor = s.sm_p;
    const float plx = re4vr::j_num(s.cfg, "pl_x", 0.0f);
    const float ply = re4vr::j_num(s.cfg, "pl_y", 0.0f);
    const float plz = re4vr::j_num(s.cfg, "pl_z", 0.0f);

    if (plx != 0.0f || ply != 0.0f || plz != 0.0f) {
        anchor = s.sm_p + s.sm_r * glm::vec3{plx, ply, plz};
    }

    if (!s.detached_zone) {
        re4vr::lua_set_vec3(s.anchor_g.c_str(), anchor);
    }
}

void RE4VRHolster::write_scale(Slot& s, ::REManagedObject* tf, float value) {
    // Lua Z.1057-1066: eigener Not-Aus, weil hier als einzigem Punkt ein
    // Schreibvorgang entfaellt.
    const bool perf_on = !re4vr::lua_get_bool("__re4_hol_perf_off", false)
        && !re4vr::lua_get_bool("__re4_hol_scale_perf_off", false);

    if (!perf_on) {
        set_vec3(tf, "set_LocalScale", glm::vec3{value, value, value});
        return;
    }

    if (!s.scale_written.has_value() || *s.scale_written != value) {
        set_vec3(tf, "set_LocalScale", glm::vec3{value, value, value});
        s.scale_written = value;
    }
}

void RE4VRHolster::apply_slot(Slot& s) {
    // Lua Z.1071-1156.
    if (s.detached_zone) {
        glm::vec3 za{};

        if (compute_zone_anchor(s, za)) {
            re4vr::lua_set_vec3(s.anchor_g.c_str(), za);
        }
    }

    if (s.clone_obj == nullptr) {
        return;
    }

    const bool perf_on = !re4vr::lua_get_bool("__re4_hol_perf_off", false);
    ::REManagedObject* tf = nullptr;

    if (perf_on) {
        tf = s.clone_tf;

        if (tf == nullptr) {
            tf = re4vr::call_safe<::REManagedObject*>(s.clone_obj, "get_Transform");
            s.clone_tf = tf;
        }
    } else {
        tf = re4vr::call_safe<::REManagedObject*>(s.clone_obj, "get_Transform");
    }

    if (tf == nullptr) {
        return;
    }

    const float sc = re4vr::j_num(s.cfg, "scale", 1.0f);

    if (s.parented) {
        // [PER-WAFFE X] Zusatz-Offset der aktuell im Slot liegenden Waffe,
        // Tabelle md_x_wid mit STRING-Keys (Lua Z.1101-1106).
        float wx = 0.0f;

        if (s.cfg.contains("md_x_wid") && s.cfg["md_x_wid"].is_object() && s.clone_wid.has_value()) {
            wx = re4vr::j_num(s.cfg["md_x_wid"], std::to_string(*s.clone_wid).c_str(), 0.0f);
        }

        const float lx = re4vr::j_num(s.cfg, "off_x", 0.0f) + re4vr::j_num(s.cfg, "md_x", 0.0f)
            + re4vr::j_num(s.cfg, "shift_x", 0.0f) + wx;
        const float ly = re4vr::j_num(s.cfg, "off_y", 0.0f) + re4vr::j_num(s.cfg, "md_y", 0.0f);
        const float lz = re4vr::j_num(s.cfg, "off_z", 0.0f) + re4vr::j_num(s.cfg, "md_z", 0.0f);

        // [CROUCH_OPTIK] NUR Optik -- der Greif-Anker unten rechnet sie heraus.
        const float cz = crouch_opt_z();

        set_vec3(tf, "set_LocalPosition", glm::vec3{lx, ly, lz + cz});
        set_quat(tf, "set_LocalRotation", quat_from_euler(re4vr::j_num(s.cfg, "rx", 0.0f),
                                                          re4vr::j_num(s.cfg, "ry", 0.0f),
                                                          re4vr::j_num(s.cfg, "rz", 0.0f)));
        write_scale(s, tf, sc);

        glm::vec3 wp{};
        glm::quat wr{};
        const bool have_wp = get_vec3(tf, "get_Position", wp);
        const bool have_wr = get_quat(tf, "get_Rotation", wr);

        if (have_wp) {
            glm::vec3 a = wp;

            // Optik-Verschiebung wieder herausrechnen (Lua Z.1129-1133).
            if (cz != 0.0f && s.joint != nullptr) {
                glm::quat jr{};

                if (get_quat(reinterpret_cast<::REManagedObject*>(s.joint), "get_Rotation", jr)) {
                    const glm::vec3 d = jr * glm::vec3{0.0f, 0.0f, cz};
                    a -= d;
                }
            }

            const float plx = re4vr::j_num(s.cfg, "pl_x", 0.0f);
            const float ply = re4vr::j_num(s.cfg, "pl_y", 0.0f);
            const float plz = re4vr::j_num(s.cfg, "pl_z", 0.0f);

            if (have_wr && (plx != 0.0f || ply != 0.0f || plz != 0.0f)) {
                a += wr * glm::vec3{plx, ply, plz};
            }

            if (!s.detached_zone) {
                re4vr::lua_set_vec3(s.anchor_g.c_str(), a);
            }
        }

        return;
    }

    // Fallback: Welt-Transform frisch aus dem Joint (Lua Z.1144-1155).
    glm::vec3 tp{};
    std::optional<glm::quat> tr{};

    if (!compute_target(s, tp, tr)) {
        return;
    }

    set_vec3(tf, "set_Position", tp);

    if (tr.has_value()) {
        set_quat(tf, "set_Rotation", *tr);
    }

    write_scale(s, tf, sc);

    glm::vec3 a = tp;
    const float plx = re4vr::j_num(s.cfg, "pl_x", 0.0f);
    const float ply = re4vr::j_num(s.cfg, "pl_y", 0.0f);
    const float plz = re4vr::j_num(s.cfg, "pl_z", 0.0f);

    if (tr.has_value() && (plx != 0.0f || ply != 0.0f || plz != 0.0f)) {
        a = tp + (*tr) * glm::vec3{plx, ply, plz};
    }

    if (!s.detached_zone) {
        re4vr::lua_set_vec3(s.anchor_g.c_str(), a);
    }
}

bool RE4VRHolster::slot_dormant(const Slot& s) const {
    // Lua Z.76-79.
    if (re4vr::lua_get_bool("__re4_holster_killswitch", false)) {
        return true;
    }

    return re4vr::lua_get_bool("__re4_holster_knife_only", false) && s.name != "knife";
}

// Lua: jeder Slot hat sein eigenes get_source() mit eigenem want_wid
// (Z.1448-1452, 1489-1493, 1510-1513, 1538-1542).
std::optional<int32_t> RE4VRHolster::source_want_wid(const Slot& s) const {
    if (s.name == "knife") {
        return m_last_knife_wid != 0 ? std::optional<int32_t>{m_last_knife_wid} : std::nullopt;
    }

    if (s.name == "pistol") {
        return m_last_pistol.wid != 0 ? std::optional<int32_t>{m_last_pistol.wid} : std::nullopt;
    }

    if (s.name == "grenade") {
        return std::optional<int32_t>{m_last_grenade.wid};   // Default 5400
    }

    if (s.name == "shoulder") {
        return m_last_rifle.wid != 0 ? std::optional<int32_t>{m_last_rifle.wid} : std::nullopt;
    }

    return std::nullopt;
}

void RE4VRHolster::manage_slot(Slot& s) {
    // Lua Z.1157-1197, neun Schritte in dieser Reihenfolge.
    if (slot_dormant(s)) {
        if (s.clone_obj != nullptr) {
            destroy_slot(s);
        }

        return;
    }

    if (!re4vr::j_bool(s.cfg, "enabled", true)) {
        if (s.clone_obj != nullptr) {
            destroy_slot(s);
        }

        return;
    }

    const double nowi = clock_now();

    if (nowi - s.inv_check_t > 0.5) {
        s.inv_check_t = nowi;
        s.has_inv = has_weapon_in_inventory(s.ids);
    }

    if (!s.has_inv) {
        if (s.clone_obj != nullptr) {
            destroy_slot(s);
        }

        return;
    }

    const auto ew = get_equip_wid();
    s.in_use = ew.has_value() && s.ids.count(*ew) != 0;

    if (s.clone_obj != nullptr) {
        bool v = false;
        const bool ok = re4vr::try_call<bool>(s.clone_obj, "get_Valid", v);

        if (!ok || !v) {
            destroy_slot(s);
        }
    }

    if (s.clone_obj != nullptr) {
        const double now = clock_now();

        if (now - s.last_check > 0.5) {
            s.last_check = now;
            // [FIX 4] Dasselbe want_wid wie in Schritt 8 -- sonst liefert
            // find_weapon_go das ERSTE Treffer-GO, die Adresse weicht dauerhaft
            // ab und der Klon wird alle 0,5 s neu gebaut.
            std::optional<int32_t> dummy{};
            const auto go = find_weapon_go(s.ids, source_want_wid(s), &dummy);
            const uintptr_t addr = go != nullptr ? reinterpret_cast<uintptr_t>(go) : 0;

            if (addr != 0 && addr != s.src_addr) {
                destroy_slot(s);
            }
        }
    }

    if (s.clone_obj == nullptr) {
        std::optional<int32_t> kwid{};
        const auto go = find_weapon_go(s.ids, source_want_wid(s), &kwid);

        if (go != nullptr) {
            const auto mesh = re4vr::get_component(go, m_mesh_td);

            if (mesh != nullptr && spawn_slot(s, mesh)) {
                s.clone_wid = kwid;
                s.src_addr = reinterpret_cast<uintptr_t>(go);
            }
        }
    }

    if (s.clone_obj != nullptr) {
        if (!s.part0_done && isolate_part0(s)) {
            s.part0_done = true;
        }

        bool dark = re4vr::j_bool(s.cfg, "dim_in_use", true) && s.in_use;

        // [LH_CLONE] Messer als Klon in der LINKEN Hand -> Brust-Klon dimmen.
        if (s.name == "knife" && re4vr::lua_get_bool("__re4_knife_left_clone", false)) {
            dark = true;
        }

        if (!s.dim_applied.has_value() || *s.dim_applied != dark) {
            if (apply_dim(s, dark)) {
                s.dim_applied = dark;
            }
        }
    }
}

void RE4VRHolster::update_knife_lh_zone(Slot& s) {
    // Lua Z.1205-1228. Laeuft IMMER zuerst, vor jedem Early-Return in update_grab.
    if (s.name != "knife") {
        return;
    }

    // [EIN RADIUS] Die LINKE Hand (weapons2) nutzt dieselben Werte.
    re4vr::lua_set_number("__re4_knife_grab_trigger", re4vr::j_num(s.cfg, "grab_trigger", 0.16f));
    re4vr::lua_set_number("__re4_knife_grab_release", re4vr::j_num(s.cfg, "grab_release", 0.24f));

    std::optional<glm::vec3> anchor{};

    if (!slot_dormant(s) && re4vr::j_bool(s.cfg, "enabled", true)) {
        anchor = re4vr::lua_get_vec3(s.anchor_g.c_str());
    }

    std::optional<glm::vec3> lhp{};

    if (anchor.has_value()) {
        lhp = re4vr::lua_get_vec3_any({"__vr_lh_ctrl_raw", "__vr_lh_world"});

        if (!lhp.has_value()) {
            lhp = lh_world();
        }
    }

    if (!anchor.has_value() || !lhp.has_value()) {
        re4vr::lua_set_nil("__re4_knife_lh_dist");
        m_knife_lh_in_zone = false;
        re4vr::lua_set_bool("__re4_knife_lh_in_zone", false);
        return;
    }

    const float ld = vlen(*lhp - *anchor);
    re4vr::lua_set_number("__re4_knife_lh_dist", ld);

    if (!m_knife_lh_in_zone) {
        if (ld <= re4vr::j_num(s.cfg, "grab_trigger", 0.16f)) {
            m_knife_lh_in_zone = true;
        }
    } else {
        if (ld > re4vr::j_num(s.cfg, "grab_release", 0.24f)) {
            m_knife_lh_in_zone = false;
        }
    }

    re4vr::lua_set_bool("__re4_knife_lh_in_zone", m_knife_lh_in_zone);
}

void RE4VRHolster::update_grab(Slot& s) {
    // Lua Z.1229-1269.
    update_knife_lh_zone(s);   // IMMER zuerst

    const auto reset = [&]() {
        s.grab_in_zone = false;
        s.grab_last_dist = 99.0f;
        re4vr::lua_set_bool(s.zone_g.c_str(), false);
    };

    if (slot_dormant(s)) {
        reset();
        return;
    }

    // [WURF-FENSTER] nur der Schulter-Slot.
    if (s.name == "shoulder"
        && re4vr::lua_get_number("__vr_throw_windup_until", 0.0) > clock_now()) {
        reset();
        return;
    }

    // [LH_CLONE] Messer links -> rechter Messer-Holster aus.
    if (s.name == "knife" && re4vr::lua_get_bool("__re4_knife_left_clone", false)) {
        reset();
        return;
    }

    const auto anchor = re4vr::lua_get_vec3(s.anchor_g.c_str());
    std::optional<glm::vec3> rh{};

    if (anchor.has_value()) {
        // [ROHE CONTROLLER-POS] Alle Holster messen mit der unverschobenen
        // Controller-Position (Lua Z.1250-1255).
        rh = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

        if (!rh.has_value()) {
            rh = rh_world();
        }
    }

    if (!re4vr::j_bool(s.cfg, "enabled", true) || !anchor.has_value() || !rh.has_value()) {
        reset();
        return;
    }

    const float d = vlen(*rh - *anchor);
    s.grab_last_dist = d;

    if (!s.grab_in_zone) {
        if (d <= re4vr::j_num(s.cfg, "grab_trigger", 0.16f)) {
            s.grab_in_zone = true;
        }
    } else {
        if (d > re4vr::j_num(s.cfg, "grab_release", 0.24f)) {
            s.grab_in_zone = false;
        }
    }

    re4vr::lua_set_bool(s.zone_g.c_str(), s.grab_in_zone);
}

void RE4VRHolster::tick_slot(Slot& s) {
    // Lua Z.1332.
    manage_slot(s);
    update_smooth(s);
    update_grab(s);
}

void RE4VRHolster::apply_all() {
    // Lua Z.2224.
    for (auto& s : m_slots) {
        apply_slot(s);
    }
}

// =====================================================================
// VR-Helfer (Lua Z.683-748)
// =====================================================================
bool RE4VRHolster::right_grip_pressed() {
    auto& vr = VR::get();

    if (vr == nullptr) {
        return false;
    }

    const auto act = vr->get_action_grip();

    if (!act) {
        return false;
    }

    return vr->is_action_active(act, vr->get_right_joystick());
}

bool RE4VRHolster::left_grip_pressed() {
    auto& vr = VR::get();

    if (vr == nullptr) {
        return false;
    }

    const auto act = vr->get_action_grip();

    if (!act) {
        return false;
    }

    return vr->is_action_active(act, vr->get_left_joystick());
}

std::optional<glm::vec3> RE4VRHolster::rh_world() {
    // Lua Z.691-696.
    if (const auto p = re4vr::lua_get_vec3("__vr_rh_world")) {
        return p;
    }

    const auto tf = body_tf();

    if (tf == nullptr) {
        return std::nullopt;
    }

    auto j = joint_by_name(tf, "R_Hand");

    if (j == nullptr) {
        j = joint_by_name(tf, "R_Arm_Hand");
    }

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 p{};

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(j), "get_Position", p)) {
        return std::nullopt;
    }

    return p;
}

std::optional<glm::vec3> RE4VRHolster::lh_world() {
    // Lua Z.706-711.
    if (const auto p = re4vr::lua_get_vec3_any({"__vr_lh_joint_pos", "__vr_lh_world"})) {
        return p;
    }

    const auto tf = body_tf();

    if (tf == nullptr) {
        return std::nullopt;
    }

    auto j = joint_by_name(tf, "L_Hand");

    if (j == nullptr) {
        j = joint_by_name(tf, "L_Arm_Hand");
    }

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 p{};

    if (!get_vec3(reinterpret_cast<::REManagedObject*>(j), "get_Position", p)) {
        return std::nullopt;
    }

    return p;
}

bool RE4VRHolster::hmd_pose_yaw(glm::vec3& pos, glm::vec3& right, glm::vec3& up, glm::vec3& fwd) {
    // Lua Z.735-748: HMD-Weltposition + koerper-yaw-orientierte Basis.
    const auto cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());

    if (cam == nullptr) {
        return false;
    }

    const auto m = find_method(cam, "get_WorldMatrix");

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::mat4 wm{1.0f};
    bool ok = false;

    try {
        m->call_safe<glm::mat4*>(&wm, context, cam);
        ok = true;
    } catch (...) {
        ok = false;
    }

    if (!clear_pending(context, ok)) {
        return false;
    }

    pos = glm::vec3{wm[3].x, wm[3].y, wm[3].z};

    float fx = 0.0f, fz = 0.0f;
    bool have = false;

    const auto tf = body_tf();

    if (tf != nullptr) {
        glm::quat rot{};

        if (get_quat(tf, "get_Rotation", rot)) {
            const glm::vec3 fv = rot * glm::vec3{0.0f, 0.0f, 1.0f};
            fx = fv.x;
            fz = fv.z;
            have = true;
        }
    }

    if (!have) {
        fx = wm[2].x;
        fz = wm[2].z;
    }

    const float len = std::sqrt(fx * fx + fz * fz);

    if (len < 1e-6f) {
        fx = 0.0f;
        fz = 1.0f;
    } else {
        fx /= len;
        fz /= len;
    }

    right = glm::vec3{fz, 0.0f, -fx};
    up = glm::vec3{0.0f, 1.0f, 0.0f};
    fwd = glm::vec3{fx, 0.0f, fz};
    return true;
}

void RE4VRHolster::haptic_pulse(float dur, float freq, float amp) {
    auto& vr = VR::get();

    if (vr == nullptr) {
        return;
    }

    vr->trigger_haptic_vibration(0.0f, dur, freq, amp, vr->get_right_joystick());
}

// =====================================================================
// Zieh-Wege (Lua Z.480-627)
// =====================================================================
void RE4VRHolster::draw_last_pistol(::REManagedObject* pe) {
    // Lua Z.480-551.
    const auto inv = pe != nullptr
        ? re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController")
        : nullptr;
    const auto et = get_equip_type_main();
    ::REManagedObject* row = nullptr;

    if (inv != nullptr) {
        if (m_last_pistol.wid != 0) {
            row = find_row_for_weapon(inv, m_last_pistol.wid, m_last_pistol.guid);
        }

        if (row == nullptr) {
            for (auto* r : inventory_weapon_rows(inv)) {
                const auto w = call_enum(r, "get_WeaponId");

                if (w.has_value() && PISTOL_IDS.count(*w) != 0) {
                    row = r;
                    m_last_pistol.wid = *w;
                    break;
                }
            }
        }

        if (row != nullptr) {
            GuidBuf g{};

            if (get_guid(row, "get_ID", g) && !g.is_zero()) {
                call_with_guid(inv, "equip", g);
            }
        }

        // [URSACHE] equippen, NACHMESSEN, und nur bei Erfolg aktivieren
        // (Lua Z.494-542). pcall beweist gar nichts.
        const auto equipped_ok = [&]() {
            if (!et.has_value()) {
                return true;   // ohne EquipType nicht pruefbar -> wie frueher weiter
            }

            // [K2] Lua Z.512-516: `return (not ok_eq) or (eq ~= nil)` -- ein
            // FEHLGESCHLAGENER Call heisst dort "ok, weitermachen".
            // call_safe verschluckt "ging nicht" zu nullptr; ein blosses
            // `eq != nullptr` machte daraus faelschlich "nicht ok" und brach
            // genau in den Momenten ab, fuer die der Weg gebaut wurde
            // (Stagger, Inventar-Umbau).
            ::REManagedObject* eq = nullptr;
            const bool ok_eq =
                re4vr::try_call<::REManagedObject*>(inv, "getEquippedWeapon", eq, *et);

            return !ok_eq || (eq != nullptr);
        };

        if (!equipped_ok()) {
            // Zweiter Anlauf mit FRISCH geholter Zeile.
            ::REManagedObject* row2 = nullptr;

            if (m_last_pistol.wid != 0) {
                row2 = find_row_for_weapon(inv, m_last_pistol.wid, m_last_pistol.guid);
            }

            if (row2 == nullptr) {
                for (auto* r : inventory_weapon_rows(inv)) {
                    const auto w = call_enum(r, "get_WeaponId");

                    if (w.has_value() && PISTOL_IDS.count(*w) != 0) {
                        row2 = r;
                        m_last_pistol.wid = *w;
                        break;
                    }
                }
            }

            if (row2 != nullptr) {
                GuidBuf g2{};

                if (get_guid(row2, "get_ID", g2) && !g2.is_zero()) {
                    call_with_guid(inv, "equip", g2);
                }
            }
        }

        // [KEIN RATEN] Schlaegt es weiterhin fehl: NICHT aktivieren.
        if (!equipped_ok()) {
            re4vr::lua_set_string("__re4_holster_warn",
                                  "draw_last_pistol: equip wirkungslos -> NICHT aktiviert");
            return;
        }
    }

    bool ok = false;

    if (et.has_value()) {
        // [FIX 13] Lua faellt auch dann auf requestEquipGun zurueck, wenn der
        // Call selbst scheitert -- nicht nur bei fehlendem EquipType.
        bool dummy = false;
        ok = re4vr::try_call<bool>(
            pe, "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
            dummy, *et, false, false);
    }

    if (!ok) {
        re4vr::call_safe<void*>(pe, "requestEquipGun");
    }
}

void RE4VRHolster::draw_last_grenade(::REManagedObject* pe) {
    // Lua Z.563-587.
    const auto inv = pe != nullptr
        ? re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController")
        : nullptr;

    if (inv == nullptr) {
        return;
    }

    auto row = find_row_for_weapon(inv, m_last_grenade.wid, m_last_grenade.guid);

    if (row == nullptr) {
        for (auto* r : inventory_weapon_rows(inv)) {
            const auto w = call_enum(r, "get_WeaponId");

            if (w.has_value() && GRENADE_IDS.count(*w) != 0) {
                row = r;
                break;
            }
        }
    }

    if (row != nullptr) {
        GuidBuf g{};

        if (get_guid(row, "get_ID", g) && !g.is_zero()) {
            call_with_guid(inv, "equip", g);
        }
    }

    const auto et = get_equip_type_main();

    if (et.has_value()) {
        re4vr::call_safe<void*>(
            pe, "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
            *et, false, false);
    }
}

void RE4VRHolster::draw_last_rifle(::REManagedObject* pe) {
    // Lua Z.599-627.
    const auto inv = pe != nullptr
        ? re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController")
        : nullptr;

    if (inv != nullptr) {
        ::REManagedObject* row = nullptr;

        if (m_last_rifle.wid != 0) {
            row = find_row_for_weapon(inv, m_last_rifle.wid, m_last_rifle.guid);
        }

        if (row == nullptr) {
            for (auto* r : inventory_weapon_rows(inv)) {
                const auto w = call_enum(r, "get_WeaponId");

                if (w.has_value() && SHOULDER_IDS.count(*w) != 0) {
                    row = r;
                    m_last_rifle.wid = *w;
                    break;
                }
            }
        }

        // [KEINE_LANGWAFFE] Kein Nachweis -> gar nichts tun (Lua Z.616).
        if (row == nullptr) {
            return;
        }

        GuidBuf g{};

        if (get_guid(row, "get_ID", g) && !g.is_zero()) {
            call_with_guid(inv, "equip", g);
        }
    }

    const auto et = get_equip_type_main();
    bool ok = false;

    if (et.has_value()) {
        bool dummy = false;
        ok = re4vr::try_call<bool>(
            pe, "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
            dummy, *et, false, false);
    }

    if (!ok) {
        re4vr::call_safe<void*>(pe, "requestEquipGun");
    }
}

void RE4VRHolster::track_last_weapons() {
    // Lua Z.451-470, 555-562, 591-598.
    const auto ew = get_equip_wid();

    if (ew.has_value()) {
        const auto remember = [&](LastWep& lw) {
            lw.wid = *ew;
            const auto inv = get_inventory();

            if (inv != nullptr) {
                const auto g = get_equipped_main_guid(inv);

                if (!g.empty()) {
                    lw.guid = g;
                }
            }
        };

        if (PISTOL_IDS.count(*ew) != 0) {
            remember(m_last_pistol);
        }

        if (GRENADE_IDS.count(*ew) != 0) {
            remember(m_last_grenade);
        }

        if (SHOULDER_IDS.count(*ew) != 0) {
            remember(m_last_rifle);
        }
    }

    // Messer nur, wenn wirklich ein Messer gezogen ist (Lua Z.463-470).
    const auto ctx = get_ctx();

    if (ctx == nullptr) {
        return;
    }

    const auto hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return;
    }

    bool is_knife = false;

    if (re4vr::try_call<bool>(hu, "get_IsEquipKnife", is_knife) && is_knife) {
        int32_t wid = 0;

        if (re4vr::try_call<int32_t>(hu, "get_EquipWeaponID", wid) && KNIFE_IDS.count(wid) != 0) {
            m_last_knife_wid = wid;
        }
    }
}

// =====================================================================
// Mag-Holster (Lua Z.1587-1667)
// =====================================================================
// [BODY-EPOCH 2026-09-22] Nach Save-Load/Tod bleibt der alte Body ansprechbar
// (get_Valid bleibt wahr) -- die Joint-Caches haengen sonst an der Leiche.
// NUR Zeiger verwerfen: die eigenen add_ref-Referenzen werden freigegeben (wie
// store_slot_joint es tut), keine Engine-Aufrufe auf den alten Objekten.
// Klone, Snapshot (m_ar) und Ladezustaende bleiben unberuehrt.
void RE4VRHolster::drop_body_caches() {
    m_pe = nullptr;

    for (auto& s : m_slots) {
        store_slot_joint(s, nullptr);   // gibt joint_reffed frei
        s.joint_ok_frame = UINT64_MAX;
    }

    if (m_mag.joint != nullptr && m_mag.joint_reffed) {
        utility::re_managed_object::release(reinterpret_cast<::REManagedObject*>(m_mag.joint));
    }

    m_mag.joint = nullptr;
    m_mag.joint_reffed = false;
}

::REJoint* RE4VRHolster::mag_joint() {
    const auto tf = body_tf();

    if (tf == nullptr) {
        return nullptr;
    }

    bool valid = false;

    if (m_mag.joint != nullptr && utility::re_managed_object::is_managed_object(m_mag.joint)) {
        bool v = false;

        if (re4vr::try_call<bool>(reinterpret_cast<::REManagedObject*>(m_mag.joint), "get_Valid", v)) {
            valid = v;
        }
    }

    if (!valid) {
        if (m_mag.joint != nullptr && m_mag.joint_reffed) {
            utility::re_managed_object::release(reinterpret_cast<::REManagedObject*>(m_mag.joint));
        }

        m_mag.joint = nullptr;
        m_mag.joint_reffed = false;

        for (const char* nm : CHEST_JOINT_CANDIDATES) {
            const auto j = joint_by_name(tf, nm);

            if (j == nullptr) {
                continue;
            }

            auto obj = reinterpret_cast<::REManagedObject*>(j);

            if (utility::re_managed_object::is_managed_object(obj)
                && static_cast<int32_t>(obj->referenceCount) > 0) {
                utility::re_managed_object::add_ref(obj);
                m_mag.joint_reffed = true;
            }

            m_mag.joint = j;
            m_mag_cfg["joint"] = std::string{nm};
            break;
        }
    }

    return m_mag.joint;
}

bool RE4VRHolster::mag_anchor(glm::vec3& out) {
    const auto j = mag_joint();

    if (j == nullptr) {
        return false;
    }

    auto jobj = reinterpret_cast<::REManagedObject*>(j);
    glm::vec3 jp{};

    if (!get_vec3(jobj, "get_Position", jp)) {
        return false;
    }

    glm::quat jr{};

    if (get_quat(jobj, "get_Rotation", jr)) {
        const glm::vec3 off = jr * glm::vec3{re4vr::j_num(m_mag_cfg, "off_x", 0.0f),
                                             re4vr::j_num(m_mag_cfg, "off_y", 0.0f),
                                             re4vr::j_num(m_mag_cfg, "off_z", 0.0f)};
        out = jp + off;
        return true;
    }

    out = jp;
    return true;
}

void RE4VRHolster::mag_set_holding(bool v) {
    if (v == m_mag.holding) {
        return;
    }

    m_mag.holding = v;
    re4vr::lua_call_global_bool_arg("__re4_reload_set_mag_in_hand", v);
}

void RE4VRHolster::mag_haptic(float amp) {
    // [MAG_RUMBLE] laenger und tiefer als der Waffen-Rumble (Lua Z.1618-1623).
    auto& vr = VR::get();

    if (vr == nullptr || !re4vr::j_bool(m_mag_cfg, "grab_haptic", true)) {
        return;
    }

    vr->trigger_haptic_vibration(0.0f, 0.16f, 80.0f, std::max(amp, 1.0f), vr->get_left_joystick());
}

void RE4VRHolster::mag_tick() {
    // Lua Z.1624-1652.
    if (re4vr::lua_get_bool("__re4_holster_killswitch", false)
        || re4vr::lua_get_bool("__re4_holster_knife_only", false)) {
        mag_set_holding(false);
        re4vr::lua_set_bool("__vr_in_mag_holster_zone", false);
        return;
    }

    if (!re4vr::j_bool(m_mag_cfg, "enabled", true)) {
        mag_set_holding(false);
        re4vr::lua_set_bool("__vr_in_mag_holster_zone", false);
        return;
    }

    glm::vec3 anchor{};
    const bool have_anchor = mag_anchor(anchor);

    auto lh = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");

    if (!lh.has_value()) {
        lh = lh_world();
    }

    if (!have_anchor || !lh.has_value()) {
        re4vr::lua_set_bool("__vr_in_mag_holster_zone", m_mag.holding);
        return;
    }

    const float d = vlen(*lh - anchor);
    m_mag.last_dist = d;

    if (!m_mag.in_zone) {
        if (d <= re4vr::j_num(m_mag_cfg, "grab_trigger", 0.16f)) {
            m_mag.in_zone = true;
        }
    } else {
        if (d > re4vr::j_num(m_mag_cfg, "grab_release", 0.24f)) {
            m_mag.in_zone = false;
        }
    }

    const bool lgrip = left_grip_pressed();

    if (!m_mag.holding) {
        if (m_mag.in_zone && lgrip) {
            if (re4vr::lua_get_bool("__re4_reload_grab_empty", false)) {
                if (!m_mag_empty_latched) {
                    m_mag_empty_latched = true;
                    mag_haptic(1.0f);   // nichts zu greifen
                }
            } else {
                mag_set_holding(true);
                mag_haptic(0.9f);
            }
        }

        if (!lgrip) {
            m_mag_empty_latched = false;
        }
    } else if (!lgrip) {
        mag_set_holding(false);
    }

    re4vr::lua_set_bool("__vr_in_mag_holster_zone", m_mag.in_zone || m_mag.holding);
}

bool RE4VRHolster::calibrate_mag(const glm::vec3& P) {
    // Lua Z.1654-1666.
    const auto j = mag_joint();

    if (j == nullptr) {
        return false;
    }

    auto jobj = reinterpret_cast<::REManagedObject*>(j);
    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(jobj, "get_Position", jp) || !get_quat(jobj, "get_Rotation", jr)) {
        return false;
    }

    const glm::quat jrc{jr.w, -jr.x, -jr.y, -jr.z};
    const glm::vec3 off = jrc * (P - jp);

    m_mag_cfg["off_x"] = off.x;
    m_mag_cfg["off_y"] = off.y;
    m_mag_cfg["off_z"] = off.z;
    save_slot_cfg(m_mag_cfg, MAG_CFG_PATH);
    return true;
}

// =====================================================================
// Kalibrierung (Lua Z.1857-1883)
// =====================================================================
void RE4VRHolster::start_calibration(Slot* slot, bool mag) {
    m_cal_slot = slot;
    m_cal_mag = mag;
    m_cal_deadline = clock_now() + CAL_DELAY;
    m_cal_last_beep = -1;
    haptic_pulse(0.10f, 200.0f, 0.9f);
}

void RE4VRHolster::calibration_tick() {
    if (m_cal_slot == nullptr && !m_cal_mag) {
        return;
    }

    const double remaining = m_cal_deadline - clock_now();

    if (remaining <= 0.0) {
        // [ROHE CONTROLLER-POS] derselbe Bezug wie beim Messen.
        std::optional<glm::vec3> P{};

        if (m_cal_mag) {
            P = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");

            if (!P.has_value()) {
                P = lh_world();
            }
        } else {
            P = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

            if (!P.has_value()) {
                P = rh_world();
            }
        }

        if (P.has_value()) {
            if (m_cal_mag) {
                calibrate_mag(*P);
            } else if (m_cal_slot != nullptr) {
                calibrate_slot(*m_cal_slot, *P);
            }
        }

        haptic_pulse(0.30f, 200.0f, 1.0f);
        m_cal_slot = nullptr;
        m_cal_mag = false;
        m_cal_last_beep = -1;
        return;
    }

    const int int_s = static_cast<int>(std::ceil(remaining));

    if (int_s != m_cal_last_beep) {
        m_cal_last_beep = int_s;
        haptic_pulse(0.09f, 200.0f, 0.9f);
    }
}

bool RE4VRHolster::calibrate_slot(Slot& s, const glm::vec3& P) {
    // Lua Z.1278-1329.
    if (s.detached_zone) {
        // Nur die GREIFZONE kalibrieren, Mesh bleibt unberuehrt.
        glm::vec3 hmd{}, right{}, up{}, fwd{};

        if (!hmd_pose_yaw(hmd, right, up, fwd)) {
            return false;
        }

        const glm::vec3 d = P - hmd;
        s.cfg["zx"] = d.x * right.x + d.y * right.y + d.z * right.z;
        s.cfg["zy"] = d.x * up.x + d.y * up.y + d.z * up.z;
        s.cfg["zz"] = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        save_slot_cfg(s.cfg, s.path);
        return true;
    }

    // [CALIBRATE->MESH] Rotation setzen, Welt auf P setzen, Engine die lokale
    // Pose zurueckrechnen lassen (Lua Z.1298-1313).
    if (s.parented && s.clone_obj != nullptr) {
        const auto tf = re4vr::call_safe<::REManagedObject*>(s.clone_obj, "get_Transform");

        if (tf == nullptr) {
            return false;
        }

        set_quat(tf, "set_LocalRotation", quat_from_euler(re4vr::j_num(s.cfg, "rx", 0.0f),
                                                          re4vr::j_num(s.cfg, "ry", 0.0f),
                                                          re4vr::j_num(s.cfg, "rz", 0.0f)));
        set_vec3(tf, "set_Position", P);

        glm::vec3 lp{};

        if (!get_vec3(tf, "get_LocalPosition", lp)) {
            return false;
        }

        s.cfg["off_x"] = lp.x;
        s.cfg["off_y"] = lp.y;
        // [CROUCH_OPTIK] im Crouch steckt der Zuschlag nicht in lp -- abziehen.
        s.cfg["off_z"] = lp.z - crouch_opt_z();
        s.cfg["pl_x"] = 0.0f;
        s.cfg["pl_y"] = 0.0f;
        s.cfg["pl_z"] = 0.0f;
        s.sm_has = false;
        save_slot_cfg(s.cfg, s.path);
        return true;
    }

    const auto j = slot_joint(s);

    if (j == nullptr) {
        return false;
    }

    auto jobj = reinterpret_cast<::REManagedObject*>(j);
    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(jobj, "get_Position", jp) || !get_quat(jobj, "get_Rotation", jr)) {
        return false;
    }

    const glm::quat q = glm::normalize(jr * quat_from_euler(re4vr::j_num(s.cfg, "rx", 0.0f),
                                                            re4vr::j_num(s.cfg, "ry", 0.0f),
                                                            re4vr::j_num(s.cfg, "rz", 0.0f)));
    const glm::vec3 qpl = q * glm::vec3{re4vr::j_num(s.cfg, "pl_x", 0.0f),
                                        re4vr::j_num(s.cfg, "pl_y", 0.0f),
                                        re4vr::j_num(s.cfg, "pl_z", 0.0f)};
    const glm::vec3 tgt = P - jp - qpl;
    const glm::quat jrc{jr.w, -jr.x, -jr.y, -jr.z};
    const glm::vec3 off = jrc * tgt;

    s.cfg["off_x"] = off.x;
    s.cfg["off_y"] = off.y;
    s.cfg["off_z"] = off.z;
    s.sm_has = false;
    save_slot_cfg(s.cfg, s.path);
    return true;
}

// =====================================================================
// Grab-Dispatch (Lua Z.1722-1849)
// =====================================================================
void RE4VRHolster::play_go_sound(::REManagedObject* go, uint32_t id) {
    // Lua Z.1713-1717.
    if (go == nullptr || id == 0 || m_snd_container_td == nullptr) {
        return;
    }

    const auto scn = re4vr::get_component(go, m_snd_container_td);

    if (scn == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", id);
}

bool RE4VRHolster::slot_grabbable(Slot& s) {
    // Lua Z.1737-1739.
    if (s.clone_obj != nullptr || s.detached_zone) {
        return true;
    }

    // [KLONLOS GREIFBAR] nur der Messer-Slot, und nur mit Messer im Inventar.
    return s.name == "knife"
        && re4vr::lua_get_tribool("__re4_knife_clonless_grab") != 0
        && s.has_inv;
}

RE4VRHolster::Slot* RE4VRHolster::nearest_with_clone() {
    // Lua Z.1722-1743.
    Slot* best = nullptr;
    float bestd = 1e9f;

    for (auto& s : m_slots) {
        if (slot_dormant(s) || !re4vr::j_bool(s.cfg, "enabled", true) || !slot_grabbable(s)) {
            continue;
        }

        if (s.grab_last_dist < bestd) {
            best = &s;
            bestd = s.grab_last_dist;
        }
    }

    return best;
}

void RE4VRHolster::do_grab(Slot& best) {
    // Lua Z.1744-1789.
    // [KNIFE_HAND] Messer links -> die rechte Hand macht am Messer-Holster nichts.
    if (best.name == "knife"
        && (re4vr::lua_get_string("__re4_knife_hand") == "left"
            || re4vr::lua_get_bool("__re4_knife_left_clone", false))) {
        return;
    }

    const auto ew = get_equip_wid();
    const auto really = weapon_actually_in_hand();
    const bool in_hand = really.valid && really.in_hand && ew.has_value()
        && best.ids.count(*ew) != 0;

    if (in_hand) {
        // [POST_STOW] Aim kurz sperren, sonst zieht der Auto-Draw sofort zurueck.
        re4vr::lua_set_number("__vr_post_stow_until", clock_now() + 0.6);
    }

    // [ERST LOSLASSEN] Nach JEDEM Holster-Grab muss der Grip einmal offen sein.
    re4vr::lua_set_bool("__re4_aim_relatch", true);

    // on_grab: stellt die deferred Aktion ein (Lua Z.1454-1570).
    const std::string name = best.name;

    m_pending_action = [this, name, in_hand]() {
        const auto pe = get_pe();

        if (pe == nullptr) {
            return;
        }

        re4vr::call_safe<void*>(pe, "clearRequest");

        if (name == "knife") {
            // [STOW-GUARD] / [HOLSTER-GUARD]: die Engine nimmt beide Richtungen
            // binnen ~30-125 ms zurueck (Lua Z.1459-1475).
            re4vr::lua_set_number("__re4_stow_guard_until", clock_now() + 0.5);
            re4vr::lua_set_number("__re4_stow_ours_until", clock_now() + 0.2);
        }

        if (in_hand) {
            // [BAREHAND-MARKE 04.09.2026] re4_vr_merc.lua blockt native
            // requestEquipBareHand-Aufrufe, damit die Engine den Compound Bow
            // nicht selbst wegsteckt. In Lua erkannte es unsere Aufrufe am
            // `autorun`-Eintrag im debug.traceback -- holster ist jetzt nativ,
            // steht dort also nicht mehr. Deshalb die Marke, die der Port von
            // merc statt des Stack-Tests liest.
            re4vr::lua_set_number("__re4_barehand_ours_t", clock_now());
            re4vr::call_safe<void*>(pe, "requestEquipBareHand", false, false);
            m_ar.suppress = true;
            m_ar.stow_until = clock_now() + 0.5;
        } else if (name == "knife") {
            re4vr::lua_set_number("__re4_knife_draw_ours_t", clock_now());
            re4vr::call_safe<void*>(pe, "requestEquipKnife");
            m_ar.suppress = false;
            re4vr::lua_set_bool("__re4_knife_left_intent", false);
            re4vr::lua_set_bool("__re4_knife_left_clone", false);
        } else if (name == "pistol") {
            draw_last_pistol(pe);
            m_ar.suppress = false;
            re4vr::lua_set_bool("__re4_clone_no_autogun", false);
        } else if (name == "grenade") {
            draw_last_grenade(pe);
            m_ar.suppress = false;
            re4vr::lua_set_bool("__re4_clone_no_autogun", false);
        } else {
            // [KEINE_LANGWAFFE] Zweiter, unabhaengiger Riegel VOR draw_last_rifle
            // (Lua Z.1551-1565): ohne Langwaffe im Inventar gar nichts tun,
            // sonst holen suppress=false + execChangeWeapon die letzte Pistole.
            const auto inv_s = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");
            bool has_long = (inv_s == nullptr);

            if (inv_s != nullptr) {
                for (auto* r : inventory_weapon_rows(inv_s)) {
                    const auto w = call_enum(r, "get_WeaponId");

                    if (w.has_value() && SHOULDER_IDS.count(*w) != 0) {
                        has_long = true;
                        break;
                    }
                }
            }

            if (!has_long) {
                return;
            }

            draw_last_rifle(pe);
            m_ar.suppress = false;
            re4vr::lua_set_bool("__re4_clone_no_autogun", false);
        }

        re4vr::call_safe<void*>(pe, "execChangeWeapon");

        if (name == "knife") {
            re4vr::lua_set_number("__re4_stow_ours_until", 0.0);
        }
    };

    // [HOLSTER_SND] bei JEDEM Grab, Granate mit eigenem Sound (Lua Z.1772-1775).
    const uint32_t snd = (best.name == "grenade") ? GRENADE_GRAB_SND : HOLSTER_GRAB_SND;
    play_go_sound(find_weapon_go(best.ids, std::nullopt), snd);

    if (re4vr::j_bool(best.cfg, "grab_haptic", true)) {
        const float delay = re4vr::j_num(best.cfg, "grab_haptic_delay", 0.0f);

        if (delay > 0.0f) {
            m_grab_haptic_at = clock_now() + delay;
        } else {
            haptic_pulse(0.06f, 200.0f, 0.9f);
        }
    }
}

void RE4VRHolster::grab_dispatch() {
    // Lua Z.1790-1849.
    if (re4vr::lua_get_bool("__re4_holster_killswitch", false)) {
        m_grip_prev = right_grip_pressed();
        m_press_armed = false;
        m_tap_mode = false;
        re4vr::lua_set_bool("__vr_holster_grab_armed", false);
        return;
    }

    if (m_grab_haptic_at > 0.0 && clock_now() >= m_grab_haptic_at) {
        m_grab_haptic_at = 0.0;
        haptic_pulse(0.06f, 200.0f, 0.9f);
    }

    const bool grip = right_grip_pressed();

    if (grip && !m_grip_prev) {
        m_grip_t0 = clock_now();

        const auto s = nearest_with_clone();
        const bool in_zone = s != nullptr
            && s->grab_last_dist <= re4vr::j_num(s->cfg, "grab_trigger", 0.16f);

        const auto wih = weapon_actually_in_hand();

        // [AIM-vs-DRAW] Nur bei EINDEUTIG in der Hand in den Tipp-Modus.
        if (in_zone && wih.valid && wih.in_hand) {
            m_tap_mode = true;
            m_press_armed = false;
        } else {
            m_tap_mode = false;
            m_press_armed = in_zone;
        }
    }

    if (m_grip_prev && !grip) {
        const double held = clock_now() - m_grip_t0;
        Slot* s = nullptr;

        if (m_tap_mode) {
            s = (held <= static_cast<double>(m_aim_hold)) ? nearest_with_clone() : nullptr;
        } else {
            s = m_press_armed ? nearest_with_clone() : nullptr;
        }

        const bool fire = s != nullptr
            && s->grab_last_dist <= re4vr::j_num(s->cfg, "grab_release", 0.24f);

        if (fire) {
            do_grab(*s);
        }

        m_press_armed = false;
        m_tap_mode = false;
    }

    m_grip_prev = grip;

    // [SCOPE_GRAB] Bei Scope-Waffen im Tipp-Modus das Aim waehrend des
    // Tipp-Fensters unterdruecken (Lua Z.1844-1848).
    if (m_tap_mode && re4vr::lua_get_number("__re4_scope_wid", -1.0e300) != -1.0e300) {
        re4vr::lua_set_bool("__vr_holster_grab_armed",
                            grip && (clock_now() - m_grip_t0) < static_cast<double>(m_aim_hold));
    } else {
        re4vr::lua_set_bool("__vr_holster_grab_armed", m_press_armed);
    }
}

// =====================================================================
// Stage-Gates (Lua Z.45-69, 331-342)
// =====================================================================
bool RE4VRHolster::knife_only_stage() {
    const double now = clock_now();

    if (now - m_knife_only_t < 0.5) {
        return m_knife_only_val;
    }

    m_knife_only_t = now;
    m_knife_only_val = false;   // [WIE_LUA] VOR dem Scan zuruecksetzen

    const auto stage = re4vr::lua_module_call_number("re4vr/re4_vr_killswitch", "get_stage_name");

    if (!stage.has_value() || static_cast<int32_t>(*stage) != KNIFE_ONLY_STAGE) {
        return false;
    }

    const auto cm = re4vr::character_manager();

    if (cm == nullptr) {
        return false;
    }

    const auto list = re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList");

    if (list == nullptr) {
        return false;
    }

    int32_t count = 0;

    if (!re4vr::try_call<int32_t>(list, "get_Count", count)) {
        return false;
    }

    for (int32_t i = 0; i < count; ++i) {
        const auto ctx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (ctx == nullptr) {
            continue;   // null-Slots = Pool
        }

        int32_t kind = 0;

        if (re4vr::try_call<int32_t>(ctx, "get_KindID", kind) && kind == KRAUSER_KIND) {
            m_knife_only_val = true;
            break;
        }
    }

    return m_knife_only_val;
}

bool RE4VRHolster::no_weapons_yet() {
    const double now = clock_now();

    if (now - m_no_weapon_t < 0.3) {
        return m_no_weapon_val;
    }

    m_no_weapon_t = now;

    const auto stage = re4vr::lua_module_call_number("re4vr/re4_vr_killswitch", "get_stage_name");

    if (!stage.has_value() || !is_no_weapon_stage(static_cast<int32_t>(*stage))) {
        m_no_weapon_val = false;
        return false;
    }

    const auto ctx = get_ctx();

    if (ctx == nullptr) {
        // [WIE_LUA] Spieler nicht geladen -> sicher waffenlos.
        m_no_weapon_val = true;
        return true;
    }

    bool v = false;
    m_no_weapon_val = re4vr::try_call<bool>(ctx, "get_IsRestrictionWeaponShortcut", v) && v;
    return m_no_weapon_val;
}

// =====================================================================
// Messer-Charakter (Lua Z.1398-1443)
// =====================================================================
std::string RE4VRHolster::knife_char_now() {
    const auto ctx = get_ctx();
    const auto b = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
        : nullptr;

    if (b == nullptr) {
        return {};   // UNBEKANNT -- NICHT auf Leon zurueckfallen
    }

    const std::string n = re4vr::obj_name(b);

    if (n.empty()) {
        return {};
    }

    if (n == "ch3a8z0_body") {
        return "ada";
    }

    if (n == "ch0a0z0_body" || n == "ch0a1z0_body") {
        return "leon";
    }

    return {};   // fremder Body -> lieber nichts umschalten
}

const std::string& RE4VRHolster::knife_active_path() const {
    static const std::string ada{KNIFE_CFG_PATH_ADA};
    static const std::string leon{KNIFE_CFG_PATH};
    return (m_knife_char == "ada") ? ada : leon;
}

void RE4VRHolster::knife_char_tick() {
    const std::string want = knife_char_now();

    if (want.empty() || want == m_knife_char) {
        return;
    }

    m_knife_char = want;
    re4vr::lua_set_string("__re4_knife_char", want);   // speist den Schreibschutz

    const std::string path = knife_active_path();

    // Adas Datei einmalig aus Leons aktuellem Stand vorbelegen.
    if (want == "ada" && !re4vr::json_load(path).is_object()) {
        save_slot_cfg(knife().cfg, path);
    } else if (want == "ada") {
        const auto probe = re4vr::json_load(path);

        if (probe.empty()) {
            save_slot_cfg(knife().cfg, path);
        }
    }

    load_slot_cfg(knife().cfg, path);
    knife().path = path;
}

// =====================================================================
// Auto-Redraw (Lua Z.1986-2165) -- siehe Spec 6
// =====================================================================
void RE4VRHolster::auto_redraw_tick(const InHand& ih) {
    auto& ar = m_ar;

    // killswitch.is_pure_gameplay() -- im Original OHNE pcall.
    bool pg = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_pure_gameplay", false);

    // [HOOKSHOT-AUSNAHME] Lua Z.1992-1993.
    if (!pg && re4vr::lua_get_number("__re4_hookshot_recent_until", 0.0) > clock_now()
        && !re4vr::lua_get_bool("__re4_holster_killswitch", false)) {
        pg = true;
    }

    if (pg) {
        if (ar.pure_since == 0.0) {
            ar.pure_since = clock_now();
        }
    } else {
        ar.pure_since = 0.0;
        ar.left_pure_t = clock_now();
    }

    // ---- Snapshot ----
    if (pg && ih.valid && !ih.ambiguous) {
        if (ih.in_hand) {
            // [STOW_VERFAELLT] Lua Z.2037.
            if (ar.suppress && clock_now() >= ar.stow_until) {
                ar.suppress = false;
            }

            const auto w = get_equip_wid();

            if (w.has_value() && *w >= 0 && (!ar.snap.has_value() || *ar.snap != *w || ar.snap_bare)) {
                ar.snap = *w;
                ar.snap_bare = false;
            }
        } else if (ar.suppress) {
            // [LEER NUR BEI EIGENEM STOW]
            if (!ar.snap_bare) {
                ar.snap.reset();
                ar.snap_bare = true;
            }
        }
        // [WICHTIG] Leere Haende OHNE eigenen Stow: Snapshot NICHT anfassen.
    }

    // [MERCS-LEVELSTART] Lua Z.2092-2101.
    if (re4vr::lua_get_bool("__re4_in_mercs", false)) {
        const bool body_da = re4vr::body_game_object() != nullptr;

        if (!body_da) {
            m_merc_body_weg = true;
        } else if (m_merc_body_weg) {
            m_merc_body_weg = false;
            ar.snap.reset();
            ar.snap_bare = false;
            ar.suppress = false;
            re4vr::lua_set_number("__re4_merc_level_bare",
                                  re4vr::lua_get_number("__re4_merc_level_bare", 0.0) + 1.0);
        }
    }

    // [SAVE-LOAD WIE MERCS 19.09.2026] Belegt in der Messer-Sonde 23:46:52:
    // beim Laden ruestete das Spiel die Granate aus dem Spielstand aus
    // (equipWeapon 6108), 0,44 s spaeter holte die Wiederbewaffnung unten die
    // Pistole von VOR dem Tod zurueck (7x equipWeapon 6103 im 0,2-s-Takt).
    // Nach Tod/Laden gilt der alte Snapshot nicht mehr -- genau wie beim
    // Mercs-Levelstart. Signal: Body-Adresse springt (Sonde
    // re4_saveload_sonde: nur bei Tod/Laden, nie im laufenden Spiel).
    if (auto* sl_body = re4vr::body_game_object(); sl_body != nullptr) {
        const auto a = reinterpret_cast<uintptr_t>(sl_body);

        if (m_sl_body_addr.has_value() && *m_sl_body_addr != a) {
            ar.snap.reset();
            ar.snap_bare = false;
            ar.suppress = false;
        }

        m_sl_body_addr = a;
    }

    // ---- Diagnose-Exporte (kein Live-Konsument, 1:1) ----
    if (ar.snap.has_value()) {
        re4vr::lua_set_number("__re4_ar_snap", *ar.snap);
    } else if (ar.snap_bare) {
        re4vr::lua_set_bool("__re4_ar_snap", false);
    } else {
        re4vr::lua_set_nil("__re4_ar_snap");
    }

    re4vr::lua_set_bool("__re4_ar_suppress", ar.suppress);

    if (ih.valid) {
        re4vr::lua_set_bool("__re4_ar_inhand", ih.in_hand);
    } else {
        re4vr::lua_set_nil("__re4_ar_inhand");
    }

    re4vr::lua_set_bool("__re4_ar_pg", pg);
    re4vr::lua_set_number("__re4_ar_pure_for", pg ? (clock_now() - ar.pure_since) : 0.0);
    re4vr::lua_set_number("__re4_ar_stow_left", std::max(0.0, ar.stow_until - clock_now()));

    // ---- Restore ----
    double stable_need = 0.4;

    if (clock_now() - re4vr::lua_get_number("__re4_finisher_prompt_seen", -999.0) < 5.0) {
        stable_need = 0.05;
    }

    const bool disarm_ok = (clock_now() - ar.left_pure_t) < 2.0
        || re4vr::lua_get_number("__vr_stagger_recent_until", 0.0) > clock_now()
        || re4vr::lua_get_number("__re4_wesker_parry_recent_until", 0.0) > clock_now();

    if (ih.valid && !ih.in_hand && !ar.suppress && ar.snap.has_value()
        && !re4vr::lua_get_bool("__re4_holster_knife_only", false)
        && pg && (clock_now() - ar.pure_since) >= stable_need
        && clock_now() >= ar.next_try
        && disarm_ok
        && !no_weapons_yet()) {
        ar.next_try = clock_now() + 0.20;

        const auto pe = get_pe();

        if (pe != nullptr) {
            re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
            re4vr::call_safe<void*>(pe, "clearRequest");

            const int32_t snap = *ar.snap;

            if (KNIFE_IDS.count(snap) != 0) {
                re4vr::lua_set_number("__re4_knife_draw_ours_t", clock_now());
                re4vr::call_safe<void*>(pe, "requestEquipKnife");
            } else if (GRENADE_IDS.count(snap) != 0) {
                draw_last_grenade(pe);
            } else if (SHOULDER_IDS.count(snap) != 0) {
                draw_last_rifle(pe);
            } else if (PISTOL_IDS.count(snap) != 0) {
                draw_last_pistol(pe);
            } else {
                re4vr::call_safe<void*>(pe, "requestEquipGun");
            }

            re4vr::call_safe<void*>(pe, "execChangeWeapon");
        }
    }

    // ---- [KNIFE_ONLY ZUG] Lua Z.2180-2197 ----
    if (re4vr::lua_get_bool("__re4_holster_knife_only", false)
        && !re4vr::lua_get_bool("__re4_holster_killswitch", false)
        && ih.valid && !ih.in_hand && !ar.suppress
        && re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_pure_gameplay", false)
        && (clock_now() - ar.pure_since) >= 0.4
        && clock_now() >= ar.next_try) {
        ar.next_try = clock_now() + 0.5;

        const auto pe = get_pe();

        if (pe != nullptr) {
            re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
            re4vr::lua_set_number("__re4_knife_draw_ours_t", clock_now());
            re4vr::call_safe<void*>(pe, "clearRequest");
            re4vr::call_safe<void*>(pe, "requestEquipKnife");
            re4vr::call_safe<void*>(pe, "execChangeWeapon");
        }
    }
}

// =====================================================================
// Die drei Hooks (Lua Z.670-680, 2416-2522)
// =====================================================================
void RE4VRHolster::install_gate_hooks() {
    if (m_gates_installed) {
        return;
    }

    m_gates_installed = true;

    // Not-Aus-Flags wie im Original (Lua Z.2418, 2478).
    re4vr::lua_set_bool("__re4_knife_gate", true);
    re4vr::lua_set_bool("__re4_melee_gate", true);

    // [1:1] Der geteilte Guid-Zwischenspeicher. re4_vr_reload.lua und
    // re4_vr_reload4_dlc.lua lesen ihn (`_G.__re4_guid_sets or {defaults}`) --
    // dieselben Vorgaben, das Ergebnis ist also gleich; die Tabelle soll aber
    // dastehen wie im Original.
    // __re4_guid_fields wird BEWUSST NICHT gesetzt: das ist das Ergebnis der
    // Feldsuche, und dieser Port liest die Guid direkt aus ihren 16 Byte, sucht
    // also nie. Ein geratener Wert wuerde reload.lua in die Irre fuehren.
    if (auto lua = hol_lua_state()) {
        try {
            if (!(*lua)["__re4_guid_sets"].valid()) {
                sol::table sets = lua->create_table();
                sol::table a = lua->create_table();
                a[1] = "mData1"; a[2] = "mData2"; a[3] = "mData3"; a[4] = "mData4";
                sol::table b = lua->create_table();
                b[1] = "_a"; b[2] = "_b"; b[3] = "_c"; b[4] = "_d";
                sets[1] = a;
                sets[2] = b;
                (*lua)["__re4_guid_sets"] = sets;
            }
        } catch (...) {
        }
    }

    // Reine Diagnose ohne Leser -- 1:1 vorhanden.
    re4vr::lua_set_string("__re4_equip_guard_log", "");

    // ---- Hook 1: share.Startup.updateOnFrameHead (post) ----
    // Der EINZIGE Ort, an dem ein per pending_action geplanter Waffenwechsel
    // greift -- ausserhalb dieses Hooks verpufft er (Lua Z.629, 670-680).
    if (auto* td = sdk::find_type_definition("share.Startup")) {
        if (auto* m = td->get_method("updateOnFrameHead")) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {
                    // [SCRIPTGATE] Riegel zu = dieser Hook existiert nicht.
                    if (re4vr::mods_gated()) {
                        return;
                    }

                    RE4VRHolster::get()->holster_exec();
                });
        }
    }

    auto* pe_td = sdk::find_type_definition(game_namespace("PlayerEquipment"));

    if (pe_td == nullptr) {
        return;
    }

    // ---- Hook 2: requestEquipKnife -- 9 Abbruchzweige (Spec 10) ----
    const auto knife_gate = [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&,
                               uintptr_t) -> HookManager::PreHookResult {
        const auto pass = HookManager::PreHookResult::CALL_ORIGINAL;

        // [SCRIPTGATE] Riegel zu = dieser Hook existiert nicht. In Lua entfernt
        // der Script-Reset bei abgeschalteter Datei ALLE Hook-Callbacks; ohne
        // das wuerde der Port in Adas Gondel-Stillzone weiter native
        // Messerzuege verwerfen, das Original nicht.
        if (re4vr::mods_gated()) { return pass; }

        if (!re4vr::lua_get_bool("__re4_knife_gate", false)) { return pass; }
        if (clock_now() - re4vr::lua_get_number("__re4_knife_draw_ours_t", -999.0) < 1.0) { return pass; }
        if (re4vr::lua_get_bool("__re4_holster_killswitch", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_ks4_active", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_holster_knife_only", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_equipped", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_left_clone", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_left_intent", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_flying", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_clone_finisher_restore", false)) { return pass; }

        return HookManager::PreHookResult::SKIP_ORIGINAL;
    };

    // ---- Hook 3: requestEquipMelee -- 11 Abbruchzweige ----
    // ZWEI Bedingungen mehr als Hook 2: der Finisher-Prompt (ein FUNKTIONS-
    // aufruf, kein Flag) und __re4_ks_active.
    const auto melee_gate = [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&,
                               uintptr_t) -> HookManager::PreHookResult {
        const auto pass = HookManager::PreHookResult::CALL_ORIGINAL;

        // [SCRIPTGATE] s. knife_gate.
        if (re4vr::mods_gated()) { return pass; }

        if (!re4vr::lua_get_bool("__re4_melee_gate", false)) { return pass; }
        if (re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false)) { return pass; }
        if (clock_now() - re4vr::lua_get_number("__re4_knife_draw_ours_t", -999.0) < 1.0) { return pass; }
        if (re4vr::lua_get_bool("__re4_holster_killswitch", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_ks4_active", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_ks_active", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_holster_knife_only", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_equipped", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_left_clone", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_left_intent", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_knife_flying", false)) { return pass; }
        if (re4vr::lua_get_bool("__re4_clone_finisher_restore", false)) { return pass; }

        return HookManager::PreHookResult::SKIP_ORIGINAL;
    };

    // [ALLE_UEBERLADUNGEN] Nicht auf get_method verlassen -- der Call, der das
    // Messer bringt, hat einen Parameter (Lua Z.2422-2427, 2482-2489).
    for (auto& m : pe_td->get_methods()) {
        const char* mn = m.get_name();

        if (mn == nullptr) {
            continue;
        }

        const std::string_view n{mn};

        if (n == "requestEquipKnife") {
            g_hookman.add(&m, knife_gate,
                          [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
        } else if (n == "requestEquipMelee") {
            g_hookman.add(&m, melee_gate,
                          [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }
}

// =====================================================================
// Die nach Lua exportierten Einstiegspunkte
// =====================================================================
void RE4VRHolster::defer(std::function<void()> fn) {
    if (fn) {
        m_pending_action = std::move(fn);
    }
}

void RE4VRHolster::set_suppress(bool v) {
    // Lua Z.651-654.
    m_ar.suppress = v;

    if (v) {
        m_ar.stow_until = clock_now() + 0.5;
    }
}

void RE4VRHolster::holster_exec() {
    // Lua Z.631-642.
    if (!m_pending_action) {
        return;
    }

    auto pa = std::move(m_pending_action);
    m_pending_action = nullptr;

    // [EIGENER WECHSEL] Freifahrt fuer den KNIFE_KEEP_OUT-Hook in weapons.lua.
    re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);

    try {
        pa();
    } catch (...) {
    }
}

void RE4VRHolster::holster_bare() {
    // Lua Z.659-669.
    m_ar.suppress = true;
    m_ar.stow_until = clock_now() + 0.5;
    re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);

    m_pending_action = [this]() {
        const auto pe = get_pe();

        if (pe == nullptr) {
            return;
        }

        re4vr::call_safe<void*>(pe, "clearRequest");
        // [BAREHAND-MARKE 04.09.2026] s. oben.
        re4vr::lua_set_number("__re4_barehand_ours_t", clock_now());
        re4vr::call_safe<void*>(pe, "requestEquipBareHand", false, false);
        re4vr::call_safe<void*>(pe, "execChangeWeapon");
    };
}

bool RE4VRHolster::force_change_to_main() {
    // Lua Z.425-443: die Minecart-Handgun wp4005 direkt in den Main-Slot.
    const auto pe = get_pe();

    if (pe == nullptr) {
        return false;
    }

    const auto cur = call_enum(pe, "get_EquipWeaponID");

    if (cur.has_value() && *cur == 4005) {
        return true;   // schon die Cart-Gun
    }

    const auto wid = weaponid_enum(4005);
    const auto et = get_equip_type_main();

    re4vr::call_safe<void*>(pe, "clearRequest");

    if (wid.has_value() && et.has_value()) {
        re4vr::call_safe<void*>(pe, "equipWeapon", *et, *wid, false, false);
    }

    if (et.has_value()) {
        re4vr::call_safe<void*>(
            pe, "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
            *et, false, false);
    }

    re4vr::call_safe<void*>(pe, "execChangeWeapon");
    return true;
}

void RE4VRHolster::play_knife_grab_sound() {
    // Lua Z.1720 -- hand-neutral, ueber den SoundContainer des Messer-GO.
    play_go_sound(find_weapon_go(KNIFE_IDS, std::nullopt), HOLSTER_GRAB_SND);
}

// =====================================================================
// Lebenszyklus
// =====================================================================
std::optional<std::string> RE4VRHolster::on_initialize() {
    // Slots anlegen (Lua Z.1445-1571).
    knife().name = "knife";       knife().ids = KNIFE_IDS;
    knife().anchor_g = "__vr_knife_chest_pos";
    knife().zone_g = "__vr_knife_holster_zone";
    knife().path = KNIFE_CFG_PATH;

    pistol().name = "pistol";     pistol().ids = PISTOL_IDS;
    pistol().anchor_g = "__vr_pistol_holster_pos";
    pistol().zone_g = "__vr_pistol_holster_zone";
    pistol().path = PISTOL_CFG_PATH;

    grenade().name = "grenade";   grenade().ids = GRENADE_IDS;
    grenade().anchor_g = "__vr_grenade_holster_pos";
    grenade().zone_g = "__vr_grenade_holster_zone";
    grenade().path = GRENADE_CFG_PATH;

    shoulder().name = "shoulder"; shoulder().ids = SHOULDER_IDS;
    shoulder().anchor_g = "__vr_shoulder_holster_pos";
    shoulder().zone_g = "__vr_shoulder_holster_zone";
    shoulder().path = SHOULDER_CFG_PATH;
    shoulder().detached_zone = true;
    shoulder().all_parts = true;

    for (auto& s : m_slots) {
        default_cfg(s.cfg);
    }

    // [SHIFT_X] nur die Pistole hat diesen Key (Lua Z.1371).
    pistol().cfg["shift_x"] = 0.0f;

    // Schulter-Vorgaben (Lua Z.1374-1380).
    shoulder().cfg["off_x"] = 0.0f;
    shoulder().cfg["off_y"] = 0.08f;
    shoulder().cfg["off_z"] = 0.40f;
    shoulder().cfg["scale"] = 0.15f;
    shoulder().cfg["zx"] = 0.20f;
    shoulder().cfg["zy"] = -0.15f;
    shoulder().cfg["zz"] = -0.30f;
    shoulder().cfg["grab_trigger"] = 0.22f;
    shoulder().cfg["grab_release"] = 0.32f;

    for (auto& s : m_slots) {
        load_slot_cfg(s.cfg, s.path);
    }

    // Mag-Slot (Lua Z.1587-1590).
    default_cfg(m_mag_cfg);
    m_mag_cfg["off_x"] = -0.16f;
    m_mag_cfg["off_y"] = -0.28f;
    m_mag_cfg["off_z"] = 0.06f;
    load_slot_cfg(m_mag_cfg, MAG_CFG_PATH);

    load_tap_cfg();
    return Mod::on_initialize();
}

// [FIX 5+6] Diese Globals leben im Lua-State und sind nach "Reset Scripts"
// weg. Im Original wird die Datei neu ausgefuehrt und setzt sie wieder; hier
// muessen sie ausdruecklich neu gesetzt werden -- sonst ist der Ada/Leon-
// Schreibschutz aus, beide Gates lassen alles durch, und der erste UI-Zug
// schreibt crouch_gain/ada_mesh_z als 0 in die JSON.
void RE4VRHolster::publish_state_globals() {
    re4vr::lua_set_bool("__re4_knife_gate", true);
    re4vr::lua_set_bool("__re4_melee_gate", true);
    re4vr::lua_set_bool("__re4_knife_holster_hook", true);
    re4vr::lua_set_bool("__re4_knife_gate_hook", true);
    re4vr::lua_set_bool("__re4_melee_gate_hook", true);

    if (!m_knife_char.empty()) {
        re4vr::lua_set_string("__re4_knife_char", m_knife_char);
    }

    re4vr::lua_set_number("__re4_holster_crouch_gain", m_crouch_gain);
    re4vr::lua_set_number("__re4_holster_ada_mesh_z", m_ada_mesh_z);
}

void RE4VRHolster::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRHolster", "on_lua_state_created");
    publish_state_globals();

    // Die sechs Exporte (Spec 9.1) -- fuenf davon haben Live-Konsumenten in
    // weapons.lua, weapons2.lua und minecart.lua.
    lua["__re4_knife_defer"] = [](sol::protected_function fn) {
        if (!fn.valid()) {
            return;
        }

        RE4VRHolster::get()->defer([fn]() {
            auto r = fn();
            (void)r;
        });
    };

    lua["__re4_knife_set_suppress"] = [](bool b) { RE4VRHolster::get()->set_suppress(b); };
    lua["__re4_knife_holster_exec"] = []() { RE4VRHolster::get()->holster_exec(); };
    lua["__re4_knife_holster_bare"] = []() { RE4VRHolster::get()->holster_bare(); };
    lua["__re4_force_change_to_main"] = []() { return RE4VRHolster::get()->force_change_to_main(); };
    lua["__re4_knife_play_grab_sound"] = []() { RE4VRHolster::get()->play_knife_grab_sound(); };
}

void RE4VRHolster::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRHolster", "on_lua_state_destroyed");
    // Lua Z.2235-2248 -- plus das, was in Lua das Neu-Ausfuehren der Datei
    // erledigt: [FIX 15] auto_redraw, die gemerkten Waffen, der Grip-Zustand
    // und alle Configs werden dort frisch angelegt. Ohne das ueberlebt z.B.
    // ein Snapshot den Reset und der Wiederhersteller zieht eine Waffe von
    // VOR dem Reset zurueck.
    for (auto& s : m_slots) {
        destroy_slot(s);
    }

    m_character_manager = nullptr;
    m_pe = nullptr;
    m_pending_action = nullptr;

    m_ar = AutoRedraw{};
    m_last_pistol = LastWep{};
    m_last_grenade = LastWep{5400, ""};
    m_last_rifle = LastWep{};
    m_last_knife_wid = 0;
    m_merc_body_weg = false;

    m_grip_prev = false;
    m_grip_t0 = 0.0;
    m_press_armed = false;
    m_tap_mode = false;
    m_grab_haptic_at = 0.0;

    m_cal_slot = nullptr;
    m_cal_mag = false;
    m_cal_last_beep = -1;

    m_knife_only_t = 0.0;
    m_knife_only_val = false;
    m_no_weapon_t = 0.0;
    m_no_weapon_val = false;
    m_knife_lh_in_zone = false;
    m_knife_char.clear();

    m_mag.in_zone = false;

    for (auto& s : m_slots) {
        s.has_inv = false;
        s.inv_check_t = 0.0;
        s.last_check = 0.0;
        s.grab_in_zone = false;
        s.grab_last_dist = 99.0f;
    }

    // Configs frisch von der Platte, wie das Neu-Ausfuehren in Lua.
    knife().path = KNIFE_CFG_PATH;

    for (auto& s : m_slots) {
        load_slot_cfg(s.cfg, s.path);
    }

    load_slot_cfg(m_mag_cfg, MAG_CFG_PATH);
    load_tap_cfg();

    re4vr::lua_set_bool("__vr_holster_left_chest_rgrip_as_left_grip", false);
    re4vr::lua_set_bool("__vr_knife_holster_zone", false);
    re4vr::lua_set_bool("__vr_pistol_holster_zone", false);
    re4vr::lua_set_bool("__vr_grenade_holster_zone", false);
    re4vr::lua_set_bool("__vr_shoulder_holster_zone", false);
    re4vr::lua_set_bool("__vr_in_mag_holster_zone", false);
    re4vr::lua_set_bool("__vr_bare_hands", false);
    re4vr::lua_set_bool("__vr_knife_in_hand", false);
    re4vr::lua_set_bool("__vr_grenade_in_hand", false);
    mag_set_holding(false);
}

// =====================================================================
// Frame-Tick (Lua Z.1885-2221)
// =====================================================================
void RE4VRHolster::on_frame() {
    re4vr::trace("RE4VRHolster", "on_frame");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    // [PERF] Frame-Grenze fuer die Joint-Pruefung. Steht ganz oben.
    ++m_hol_frame;

    // [BODY-EPOCH 2026-09-22] Body gewechselt (Save-Load/Tod): gemerkte
    // Zeiger an der Leiche verwerfen, die Neu-Hol-Zweige greifen dann.
    if (const auto ep = re4vr::body_epoch(); ep != m_body_epoch) {
        m_body_epoch = ep;
        drop_body_caches();
    }

    if (!m_types_resolved) {
        m_types_resolved = true;
        m_mesh_td = sdk::find_type_definition("via.render.Mesh");
        m_pe_td = sdk::find_type_definition(game_namespace("PlayerEquipment"));
        m_wid_td = sdk::find_type_definition(game_namespace("WeaponID"));
        m_snd_container_td = sdk::find_type_definition("soundlib.SoundContainer");
        install_gate_hooks();
    }

    // Killswitch-Sammelflag (Lua Z.1920-1927).
    const bool ks = re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false)
        || re4vr::lua_get_bool("__re4_throwsight_active", false)
        || no_weapons_yet()
        || re4vr::lua_get_bool("__re4_stillzone_holster", false);
    re4vr::lua_set_bool("__re4_holster_killswitch", ks);

    // [MERCS MESH-Z PRO CHARAKTER 17.09.2026] Einmal pro Frame, damit
    // crouch_opt_z je Slot keinen Lua-Zugriff und keine Objektsuche braucht
    // (vgl. FIX 18). Der Body-Name wird NUR in Mercenaries geholt -- ausserhalb
    // bleibt der Index -1 und crouch_opt_z nimmt den unveraenderten
    // Kampagnen-Zweig.
    m_in_mercs_now = re4vr::lua_get_bool("__re4_in_mercs", false);
    m_merc_mesh_z_idx = -1;

    if (m_in_mercs_now) {
        const auto ctx = get_ctx();
        const auto b = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;

        m_merc_body_now = (b != nullptr) ? re4vr::obj_name(b) : std::string{};

        if (!m_merc_body_now.empty()) {
            int fallback = -1;

            for (int i = 0; i < MERC_MESH_Z_COUNT; ++i) {
                if (m_merc_body_now == m_merc_mesh_z[i].body) {
                    m_merc_mesh_z_idx = i;
                    break;
                }

                if (m_merc_mesh_z[i].body[0] == '*') {
                    fallback = i;
                }
            }

            // Unbekannter Body -> Auffang-Eintrag, damit er einstellbar bleibt.
            if (m_merc_mesh_z_idx < 0) {
                m_merc_mesh_z_idx = fallback;
            }
        }
    } else {
        m_merc_body_now.clear();
    }

    // Einmal pro Frame, VOR allen ticks (Lua Z.1930).
    re4vr::lua_set_bool("__re4_holster_knife_only", knife_only_stage());

    knife_char_tick();

    // [SLOT-WACHHUND STILLGELEGT] equip_slot_guard bleibt aus (Lua Z.1934-1940).

    calibration_tick();
    track_last_weapons();

    // [BARE_HANDS] Lua Z.1949-1957.
    const auto ih = weapon_actually_in_hand();

    if (ih.valid) {
        re4vr::lua_set_bool("__vr_bare_hands", !ih.in_hand);
        re4vr::lua_set_bool("__vr_knife_in_hand", ih.knife);
        re4vr::lua_set_bool("__vr_grenade_in_hand", ih.grenade);
    }

    // [MERCS-RAGE: MESSER WEG 2026-09-09] Im Ragemodus kaempft der Charakter
    // mit den Powerarmen, RT geht ueber den KS4 nativ durch. Ein beim Zuenden
    // noch GEZOGENES Messer bleibt aber in der Hand -- man kann damit weiter
    // herumfuchteln, waehrend RT gleichzeitig die Arme fuehrt. Also einstecken.
    //
    // Nicht nur an der Flanke, sondern rate-limitiert nachgefasst: die Engine
    // setzt die Waffe im Rage selbst um (Sonde: 4200 -> -1 -> 4200 innerhalb
    // von 3 s), ein einmaliger Griff koennte also ueberschrieben werden. Die
    // Bedingung `ih.knife` beendet das Nachfassen von selbst, sobald das Messer
    // weg ist.
    //
    // Character-frei ueber `ih.knife` statt ueber eine KindID: Wesker hat in
    // Mercenaries nie ein Messer, fuer ihn aendert sich damit nichts.
    //
    // NACH der Phase machen wir NICHTS: faellt der KS4, holt der normale
    // Auto-Zug die letzte Waffe wie immer zurueck.
    const bool rush_now = re4vr::lua_get_bool("__re4_force_ks4_bulletrush", false);

    if (rush_now) {
        if (ih.valid && ih.knife && clock_now() >= m_rush_bare_next) {
            m_rush_bare_next = clock_now() + 0.3;
            holster_bare();
        }
    } else {
        m_rush_bare_next = 0.0;
    }

    // [STAGGER_DRAW] Lua Z.1961-1982 -- und dieser Block ist im Original
    // NACHWEISLICH TOT. Er wird deshalb 1:1 als tot uebernommen.
    //
    // BEWEIS: chainsaw.PlayerDefine.State ist ein Enum. REFramework bildet
    // Enums in parse_data (Sdk.cpp:860-868) auf ihren UNDERLYING-Typ ab -- nach
    // Lua kommt also eine ZAHL, kein ValueType. Luas `ev:get_field("value__")`
    // ist damit ein Index-Zugriff auf eine Zahl -> Fehler im pcall -> `m`
    // bleibt false -> `__re4_state_damage_mask = false` -> `if mask then` ist
    // FALSCH -> der Block laeuft NIE.
    // Gegenprobe im selben Projekt: re4_vr_killswitch.lua behandelt get_State
    // und f:get_data(nil) ausdruecklich zuerst als number.
    //
    // WARUM DAS WICHTIG IST: `__vr_stagger_recent_until` hat im Original GENAU
    // EINEN Schreiber -- diese tote Stelle. Im Restore-Disarm-Gate (Lua Z.2142)
    // und in binding.lua (Right-Grip-Auto-Draw) ist der Ausdruck also immer 0.
    // Wer den Block aktiviert, oeffnet das Disarm-Gate und der Auto-Redraw
    // zieht die letzte Waffe auch nach reinem Leerschiessen zurueck.
    //
    // Dazu: PlayerDefine.State ist 64-bittig (ParentGimmick = 2^53). Ein
    // int32-Lesen wuerde die Maske auf 0 kuerzen, und `(sv & 0) == 0` ist
    // IMMER wahr -- der Block feuerte dann jeden Frame.
    {
        if (!m_state_damage_mask.has_value()) {
            m_state_damage_mask = 0;
            m_state_damage_ok = false;   // wie im Original: bleibt false
            re4vr::lua_set_bool("__re4_state_damage_mask", false);
        }

        // Der Rumpf des Originals haengt an `if mask then`, und `mask` ist dort
        // immer `false`. Es passiert also nichts -- absichtlich.
    }

    auto_redraw_tick(ih);

    for (auto& s : m_slots) {
        tick_slot(s);
    }

    mag_tick();
    grab_dispatch();

    // [NO_WEAPON_STAGES] Bare Hands erzwingen (Lua Z.2213-2220).
    if (no_weapons_yet() && ih.valid && ih.in_hand) {
        const auto pe = get_pe();

        if (pe != nullptr) {
            re4vr::call_safe<void*>(pe, "clearRequest");
            // [BAREHAND-MARKE 04.09.2026] s. oben.
            re4vr::lua_set_number("__re4_barehand_ours_t", clock_now());
            re4vr::call_safe<void*>(pe, "requestEquipBareHand", false, false);
            re4vr::call_safe<void*>(pe, "execChangeWeapon");
        }
    }

    re4vr::clear_vm_exception();
}

// =====================================================================
// Die sechs Phasen (Lua Z.2228-2233)
// =====================================================================
void RE4VRHolster::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRHolster", "on_pre_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LockScene"_fnv || hash == "BeginRendering"_fnv) {
        apply_all();
        re4vr::clear_vm_exception();
    }
}

void RE4VRHolster::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRHolster", "on_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "UpdateMotion"_fnv || hash == "UpdateJointExpression"_fnv
        || hash == "LateUpdateBehavior"_fnv || hash == "BeginRendering"_fnv) {
        apply_all();
        re4vr::clear_vm_exception();
    }
}

// =====================================================================
// Oberflaeche (Lua Z.2253-2394)
// =====================================================================
void RE4VRHolster::draw_slot_ui(const char* title, const char* id, Slot& s,
                                const SlotUiOpts& opts) {
    if (!ImGui::TreeNode(title)) {
        return;
    }

    const auto num = [&](const char* key, float def) { return re4vr::j_num(s.cfg, key, def); };

    char buf[192]{};

    // Enable-Checkbox speichert SOFORT (Lua Z.2257-2259).
    {
        std::snprintf(buf, sizeof(buf), "##en%s", id);
        bool v = re4vr::j_bool(s.cfg, "enabled", true);

        if (ImGui::Checkbox(buf, &v)) {
            s.cfg["enabled"] = v;
            save_slot_cfg(s.cfg, s.path);
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(abgr_to_vec4(0xFF00FF00u), "%s", "Enable");
    ImGui::SameLine();
    ImGui::Text("%s", title);

    // Lua Z.2260-2261.
    {
        const std::string jn = re4vr::j_str(s.cfg, "joint", "");
        ImGui::Text("Joint: %s   Klon: %s", jn.empty() ? "?" : jn.c_str(),
                    s.clone_obj != nullptr ? "aktiv" : "-");
    }

    if (opts.note != nullptr) {
        ImGui::TextColored(abgr_to_vec4(0xFFAAAAAAu), "%s", opts.note);
    }

    // [CALIBRATE] Waehrend des Countdowns KEIN Knopf, sondern die Restzeit
    // samt Hinweis (Lua Z.2263-2271).
    {
        const char* hint = opts.cal_hint != nullptr
            ? opts.cal_hint
            : "rechte Hand DIREKT ans sichtbare Modell halten (= Greifpunkt)!";

        if (m_cal_slot == &s) {
            std::snprintf(buf, sizeof(buf), "KALIBRIEREN... %.1fs  -> %s",
                          std::max(0.0, m_cal_deadline - clock_now()), hint);
            ImGui::TextColored(abgr_to_vec4(0xFF00FFFFu), "%s", buf);
        } else {
            std::snprintf(buf, sizeof(buf), "Kalibrieren (5s, Haptik-Countdown)##cal%s", id);

            if (ImGui::Button(buf)) {
                start_calibration(&s, false);
            }
        }
    }

    bool ch = false;

    const auto slider = [&](const char* label, const char* key, float lo, float hi, float def) {
        char lbl[128]{};
        std::snprintf(lbl, sizeof(lbl), "%s##%s", label, id);
        float v = num(key, def);

        if (ImGui::SliderFloat(lbl, &v, lo, hi)) {
            s.cfg[key] = v;
            ch = true;
        }
    };

    // [SHIFT_X] nur die Pistole (Lua Z.2276-2278).
    if (opts.shift_x) {
        slider("Holster X (Mesh + Greifpunkt)", "shift_x", -0.120f, 0.026f, 0.0f);
    }

    slider("Mesh-Korrektur X (rechts)", "md_x", -0.4f, 0.4f, 0.0f);

    // [PER-WAFFE X] nur die Langwaffe (Lua Z.2284-2298).
    if (opts.per_weapon_x) {
        if (!s.cfg["md_x_wid"].is_object()) {
            s.cfg["md_x_wid"] = nlohmann::json::object();
        }

        if (s.clone_wid.has_value()) {
            const std::string key = std::to_string(*s.clone_wid);
            std::snprintf(buf, sizeof(buf), "Mesh X NUR wp%s (aktuelle Waffe)##pw%s", key.c_str(), id);
            float v = re4vr::j_num(s.cfg["md_x_wid"], key.c_str(), 0.0f);

            if (ImGui::SliderFloat(buf, &v, -0.6f, 0.6f)) {
                s.cfg["md_x_wid"][key] = v;
                ch = true;
            }

            ImGui::Text("   (gespeichert fuer wp%s: %.3f -- jede Waffe hat ihren eigenen Wert)",
                        key.c_str(), re4vr::j_num(s.cfg["md_x_wid"], key.c_str(), 0.0f));
        } else {
            ImGui::Text("   (keine Langwaffe im Holster -- Per-Waffe-X erscheint, sobald eine drin liegt)");
        }
    }

    slider("Mesh-Korrektur Y (hoch)", "md_y", -0.4f, 0.4f, 0.0f);
    slider("Mesh-Korrektur Z (vorne)", "md_z", -0.4f, 0.4f, 0.0f);
    slider("Rot X", "rx", -3.15f, 3.15f, 0.0f);
    slider("Rot Y", "ry", -3.15f, 3.15f, 0.0f);
    slider("Rot Z", "rz", -3.15f, 3.15f, 0.0f);
    slider("Scale", "scale", opts.scale_min, 2.0f, 1.0f);
    slider("Glaettung", "smooth", 0.0f, 0.98f, 0.8f);

    // Der Greif-Radius wird eigens gefuehrt: NUR seine Aenderung zieht den
    // Loslass-Radius nach (Lua Z.2316).
    bool trigger_changed = false;
    {
        std::snprintf(buf, sizeof(buf), "Greif-Radius (m)##%s", id);
        float v = num("grab_trigger", 0.16f);

        if (ImGui::SliderFloat(buf, &v, 0.05f, 0.80f)) {
            s.cfg["grab_trigger"] = v;
            ch = true;
            trigger_changed = true;
        }
    }

    slider("Loslass-Radius (m)", "grab_release", 0.06f, 0.95f, 0.24f);

    {
        std::snprintf(buf, sizeof(buf), "Grab-Rumble##%s", id);
        bool v = re4vr::j_bool(s.cfg, "grab_haptic", true);

        if (ImGui::Checkbox(buf, &v)) {
            s.cfg["grab_haptic"] = v;
            ch = true;
        }

        if (v) {
            slider("Rumble-Delay (s)", "grab_haptic_delay", 0.0f, 1.0f, 0.0f);
        }
    }

    // [WIE_LUA] d1 ODER d2 verwerfen dim_applied; d2 wird ERST DANACH in ch
    // gemischt (Lua Z.2312-2315).
    bool d1 = false;
    {
        std::snprintf(buf, sizeof(buf), "Abdunkeln bei Nutzung##%s", id);
        bool v = re4vr::j_bool(s.cfg, "dim_in_use", true);

        if (ImGui::Checkbox(buf, &v)) {
            s.cfg["dim_in_use"] = v;
            d1 = true;
            ch = true;
        }
    }

    bool d2 = false;
    {
        std::snprintf(buf, sizeof(buf), "Helligkeit bei Nutzung##%s", id);
        float v = num("dim_factor", 0.12f);

        if (ImGui::SliderFloat(buf, &v, 0.0f, 1.0f)) {
            s.cfg["dim_factor"] = v;
            d2 = true;
        }
    }

    if (d1 || d2) {
        s.dim_applied.reset();
    }

    ch = ch || d2;

    if (trigger_changed) {
        const float t = num("grab_trigger", 0.16f);

        if (num("grab_release", 0.24f) < t + 0.03f) {
            s.cfg["grab_release"] = t + 0.03f;
        }
    }

    if (ch) {
        save_slot_cfg(s.cfg, s.path);
    }

    ImGui::Text("R-Hand->Dummy: %.3f m   %s", s.grab_last_dist, s.grab_in_zone ? "[IN ZONE]" : "");
    ImGui::TreePop();
}

void RE4VRHolster::draw_mag_ui() {
    if (!ImGui::TreeNode("Magazin-Holster (linke Huefte, linke Hand)")) {
        return;
    }

    char buf[192]{};

    {
        bool v = re4vr::j_bool(m_mag_cfg, "enabled", true);

        if (ImGui::Checkbox("##enmag", &v)) {
            m_mag_cfg["enabled"] = v;
            save_slot_cfg(m_mag_cfg, MAG_CFG_PATH);
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(abgr_to_vec4(0xFF00FF00u), "%s", "Enable");
    ImGui::SameLine();
    ImGui::Text("%s", "Magazin (linke Hand + linker Grip)");

    if (m_cal_mag) {
        std::snprintf(buf, sizeof(buf),
                      "KALIBRIEREN... %.1fs  -> LINKEN Controller an die Huefte halten!",
                      std::max(0.0, m_cal_deadline - clock_now()));
        ImGui::TextColored(abgr_to_vec4(0xFF00FFFFu), "%s", buf);
    } else {
        if (ImGui::Button("Kalibrieren (5s, LINKE Hand)##calmag")) {
            start_calibration(nullptr, true);
        }
    }

    bool ch = false;

    const auto slider = [&](const char* label, const char* key, float lo, float hi, float def) {
        float v = re4vr::j_num(m_mag_cfg, key, def);

        if (ImGui::SliderFloat(label, &v, lo, hi)) {
            m_mag_cfg[key] = v;
            ch = true;
        }
    };

    slider("Offset X (rechts)##mag", "off_x", -0.6f, 0.6f, -0.16f);
    slider("Offset Y (hoch)##mag", "off_y", -0.6f, 0.6f, -0.28f);
    slider("Offset Z (vorne)##mag", "off_z", -0.6f, 0.6f, 0.06f);

    bool trigger_changed = false;
    {
        float v = re4vr::j_num(m_mag_cfg, "grab_trigger", 0.16f);

        if (ImGui::SliderFloat("Greif-Radius (m)##mag", &v, 0.05f, 0.60f)) {
            m_mag_cfg["grab_trigger"] = v;
            ch = true;
            trigger_changed = true;
        }
    }

    slider("Loslass-Radius (m)##mag", "grab_release", 0.06f, 0.70f, 0.24f);

    {
        bool v = re4vr::j_bool(m_mag_cfg, "grab_haptic", true);

        if (ImGui::Checkbox("Grab-Rumble##mag", &v)) {
            m_mag_cfg["grab_haptic"] = v;
            ch = true;
        }
    }

    if (trigger_changed) {
        const float t = re4vr::j_num(m_mag_cfg, "grab_trigger", 0.16f);

        if (re4vr::j_num(m_mag_cfg, "grab_release", 0.24f) < t + 0.03f) {
            m_mag_cfg["grab_release"] = t + 0.03f;
        }
    }

    if (ch) {
        save_slot_cfg(m_mag_cfg, MAG_CFG_PATH);
    }

    ImGui::Text("L-Hand->Huefte: %.3f m   %s%s", m_mag.last_dist,
                m_mag.in_zone ? "[IN ZONE] " : "", m_mag.holding ? "[HALTEND]" : "");
    ImGui::TreePop();
}

void RE4VRHolster::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRHolster", "on_draw_ui");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (!ImGui::TreeNode("RE4VR - Holster")) {
        return;
    }

    if (ImGui::SliderFloat("Aim-Halteschwelle (s)", &m_aim_hold, 0.15f, 0.80f)) {
        save_tap_cfg();
    }

    ImGui::TextColored(abgr_to_vec4(0xFFAAAAAAu), "%s",
                       "Waffe in Hand: Grip HALTEN = Aim. Kurzer TIPP am Holster = "
                       "ziehen/wegstecken. Leere Haende = wie bisher.");

    {
        const bool is_ada = (m_knife_char == "ada");
        float v = static_cast<float>(m_ada_mesh_z);

        if (ImGui::DragFloat("Ada: alle Holster-Meshes Z (nur Ada, + = naeher)", &v, 0.002f,
                             -0.30f, 0.30f, "%.4f")) {
            m_ada_mesh_z = v;
            re4vr::lua_set_number("__re4_holster_ada_mesh_z", v);
            save_tap_cfg();
        }

        ImGui::TextColored(abgr_to_vec4(is_ada ? 0xFF00FF00u : 0xFF888888u), "%s",
                           is_ada ? "   aktiv (Ada wird gerade gespielt)"
                                  : "   inaktiv (nur bei Ada wirksam)");

        // [MERCS MESH-Z PRO CHARAKTER 17.09.2026] Ein Regler je Mercs-Charakter.
        // Der gerade gespielte steht gruen; ausserhalb von Mercenaries wirkt
        // keiner davon.
        ImGui::Separator();
        ImGui::Text("Mercenaries: Holster-Meshes Z je Charakter (+ = naeher)");

        for (int i = 0; i < MERC_MESH_Z_COUNT; ++i) {
            auto& e = m_merc_mesh_z[i];
            float mz = static_cast<float>(e.z);

            ImGui::PushID(i);

            if (ImGui::DragFloat(e.label, &mz, 0.002f, -0.30f, 0.30f, "%.4f")) {
                e.z = mz;
                save_tap_cfg();
            }

            ImGui::PopID();

            if (m_in_mercs_now && m_merc_mesh_z_idx == i) {
                ImGui::SameLine();
                ImGui::TextColored(abgr_to_vec4(0xFF00FF00u), "aktiv");
            }
        }

        ImGui::TextColored(abgr_to_vec4(m_in_mercs_now ? 0xFF00FF00u : 0xFF888888u), "   Body: %s",
                           m_in_mercs_now
                               ? (m_merc_body_now.empty() ? "?" : m_merc_body_now.c_str())
                               : "nicht in Mercenaries");
        ImGui::Separator();

        float cg = static_cast<float>(m_crouch_gain);

        if (ImGui::DragFloat("Crouch: Holster-Optik nachziehen (Gain, 0=aus)", &cg, 0.02f,
                             -2.0f, 2.0f, "%.2f")) {
            m_crouch_gain = cg;
            re4vr::lua_set_number("__re4_holster_crouch_gain", cg);
            save_tap_cfg();
        }

        const double ub = re4vr::lua_get_number("__re4_ub_z_delta", 0.0);
        char buf[224]{};
        std::snprintf(buf, sizeof(buf),
                      "  Oberkoerper-Abweichung gerade: %.3f m (Referenz = normales Gehen)"
                      "  ->  Mesh-Versatz %.3f m",
                      ub, -ub * m_crouch_gain);
        ImGui::TextColored(abgr_to_vec4(0xFF888888u), "%s", buf);
        ImGui::TextColored(abgr_to_vec4(0xFF888888u), "%s",
                           "  Nur Optik: Greifpunkte bleiben, wo sie kalibriert wurden. "
                           "1.0 = volle Korrektur, -1.0 = Gegenrichtung.");
    }

    ImGui::Separator();

    {
        char title[96]{};
        std::snprintf(title, sizeof(title), "Messer (Chest-Holster) [%s]",
                      m_knife_char.empty() ? "leon" : m_knife_char.c_str());
        draw_slot_ui(title, "knife", knife(), SlotUiOpts{});
    }

    {
        SlotUiOpts o{};
        o.shift_x = true;
        draw_slot_ui("Pistole / 1-Hand (daneben)", "pistol", pistol(), o);
    }

    draw_slot_ui("Granate (daneben)", "grenade", grenade(), SlotUiOpts{});

    {
        SlotUiOpts o{};
        o.scale_min = 0.02f;
        o.per_weapon_x = true;
        o.note = "Offset/Scale = schwebendes Anzeige-Mesh VORNE. Greifzone ist entkoppelt "
                 "(hinter der Schulter) -> per Kalibrieren setzen.";
        o.cal_hint = "rechten Controller HINTER die rechte Schulter halten (Greifzone)!";
        draw_slot_ui("Langwaffe (Schulter-Holster)", "shoulder", shoulder(), o);
    }

    draw_mag_ui();
    ImGui::TreePop();
}

#endif // RE4
