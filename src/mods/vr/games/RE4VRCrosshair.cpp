// ============================================================================
// RE4VRCrosshair -- 1:1-Portierung von re4_vr_crosshair.lua
// Spezifikation: I:\LUATRANS\PORT_CROSSHAIR_SPEC.md (Fassung 2)
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <array>
#include <span>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/SystemArray.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"   // g_framework->draw_menu_heading
#include "../../../HookManager.hpp"
#include "../../VR.hpp"
#include "RE4VR.hpp"
#include <chrono>

#include "RE4VRCrosshair.hpp"

// ============================================================================
// Lokale Helfer
// ============================================================================
namespace {

// Lua: os.clock(). Lua ist in dieselbe DLL gelinkt (clock()/CLOCKS_PER_SEC),
// identische Epoche.
double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

re4vr::LuaRef ch_lua_state() {
    // [ABSTURZ 04.09.2026] Sperre des ScriptRunners halten -- s. re4vr::LuaRef.
    return re4vr::LuaRef{};
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

// via.vec3 / via.Quaternion sind groesser als ein Register -> versteckter
// Out-Zeiger PLUS 16-Byte-Ausrichtung.
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

// Setter mit via.vec3-Argument (set_Position, set_LocalScale ...).
void call_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();
    // [M8] Luas build_args schiebt fuer Vector3f (x, y, z, 0.0f).
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, 0.0f};

    try {
        method->call_safe<void*>(context, obj, &buf);
    } catch (...) {
    }

    clear_pending(context, true);
}

void call_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::quat buf = q;

    try {
        method->call_safe<void*>(context, obj, &buf);
    } catch (...) {
    }

    clear_pending(context, true);
}

// Luas Vector3f:to_quat() -- ScriptRunner.cpp:210, ZEICHENGENAU nachgebaut.
// Das ist NICHT die Minimalbogen-Drehung von (0,0,1) auf dir: es ist eine
// ROLLFREIE Orientierung (Up gegen +Y) aus lookAtLH. Vorwaertsachse gleich,
// Roll voellig anders -- der Unterschied ist am Reticle sichtbar und trifft
// ausserdem Scope-Schuss, Kugel und Rakete.
// Bei dir == +/-(0,1,0) liefert lookAtLH NaN; das Original tut das auch, also
// bleibt es 1:1.
glm::quat dir_to_quat(const glm::vec3& dir) {
    const auto mat = glm::rowMajor4(
        glm::lookAtLH(glm::vec3{0.0f, 0.0f, 0.0f}, dir, glm::vec3{0.0f, 1.0f, 0.0f}));

    return glm::quat{mat};
}

// Lua: string.pack("<f") + string.unpack("<I4") -- die Bitmuster eines float.
uint32_t float_bits(float f) {
    uint32_t b = 0x3F800000u;   // Fallback = 1.0f, wie im Original
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

std::string obj_name_of(::REManagedObject* go) {
    return re4vr::obj_name(go);
}

// Eine Lua-GLOBAL-Funktion ohne Argumente rufen und ihren String holen.
// re4vr:: hat nur lua_module_call_string (fuer Module) -- __re4_char_now ist
// aber eine freie Global aus motion/weapons2.
std::string call_global_string(const char* name) {
    auto lua = ch_lua_state();

    if (lua == nullptr) {
        return {};
    }

    sol::object o = (*lua)[name];

    if (!o.valid() || o.get_type() != sol::type::function) {
        return {};
    }

    try {
        sol::protected_function fn = o.as<sol::protected_function>();
        auto r = fn();

        if (!r.valid()) {
            return {};
        }

        sol::object v = r;

        if (v.get_type() != sol::type::string) {
            return {};
        }

        return v.as<std::string>();
    } catch (...) {
        return {};
    }
}

// [M2] Ein Feld kann eine Referenz ODER eine eingebettete Struct sein. Bei
// einem ValueType-Feld ist der Feldinhalt KEIN Zeiger -- ihn als solchen zu
// lesen und durch ihn hindurchzuschreiben ist ein Absturz. Lua unterscheidet
// das automatisch, hier muss es von Hand passieren.
// container_is_value sagt, ob `obj` ein ROHER Struct-Zeiger ist (true) oder ein
// Managed Object (false) -- s. Kommentar oben. out_is_value gibt dasselbe fuer
// das Ergebnis zurueck, damit sich die Kette fortsetzen laesst.
bool field_target(void* obj, sdk::RETypeDefinition* obj_td, const char* name, void*& out_ptr,
                  sdk::RETypeDefinition*& out_td, bool container_is_value, bool& out_is_value) {
    out_ptr = nullptr;
    out_td = nullptr;

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
        out_ptr = raw;          // die Struct liegt IM Objekt
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

// [M4] Zahl in ein Feld schreiben und dabei den DEKLARIERTEN Typ beachten --
// Luas set_field tut das; ein rohes int32-Write waere bei einem Enum mit
// anderer Breite oder einem Single-Feld falsch.
void write_field_number(void* obj, sdk::RETypeDefinition* obj_td, const char* name, double v,
                        bool container_is_value) {
    if (obj == nullptr || obj_td == nullptr) {
        return;
    }

    auto* f = obj_td->get_field(name);

    if (f == nullptr) {
        return;
    }

    // [WERTTYP = KOPIE] wie in apply_point_range: war der Container in Lua eine
    // ValueType-Kopie, verpuffte set_field dort lautlos.
    if (container_is_value) {
        return;
    }

    auto* raw = reinterpret_cast<uint8_t*>(f->get_data_raw(obj, container_is_value));

    if (raw == nullptr) {
        return;
    }

    auto* ft = f->get_type();
    std::string tn = (ft != nullptr) ? ft->get_full_name() : std::string{};

    // Enums schreiben in ihrem Grundtyp.
    if (ft != nullptr && ft->is_enum()) {
        if (auto* ut = ft->get_underlying_type()) {
            tn = ut->get_full_name();
        } else {
            tn = "System.Int32";
        }
    }

    if (tn == "System.Single") {
        *reinterpret_cast<float*>(raw) = static_cast<float>(v);
    } else if (tn == "System.Double") {
        *reinterpret_cast<double*>(raw) = v;
    } else if (tn == "System.Int16" || tn == "System.UInt16") {
        *reinterpret_cast<int16_t*>(raw) = static_cast<int16_t>(v);
    } else if (tn == "System.SByte" || tn == "System.Byte") {
        *raw = static_cast<uint8_t>(v);
    } else if (tn == "System.Int64" || tn == "System.UInt64") {
        *reinterpret_cast<int64_t*>(raw) = static_cast<int64_t>(v);
    } else {
        *reinterpret_cast<int32_t*>(raw) = static_cast<int32_t>(v);
    }
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_crosshair.json";
constexpr const char* HUD_CFG_PATH = "re4_vr/re4_vr_hand_huds.json";
constexpr const char* LASER_CFG_PATH = "re4_vr/re4_vr_laser.json";

constexpr const char* RETICLE_DOT = "Gui_ui2040";
constexpr const char* RETICLE_CENTER_DOT = "Gui_ui2041";   // [DOT_CROSSHAIR] "Mittel-Dot"
constexpr const char* RETICLE_HIDE = "Gui_ui2042";   // reticle_hide, 1 Eintrag
constexpr float MIN_SCALE = 0.3f;
constexpr float MAX_SCALE = 0.94f;
constexpr int32_t RETICLE_VALUE = 4;

} // namespace

// ============================================================================
// Namenslisten
// ============================================================================

const std::array<const char*, 7> RE4VRCrosshair::CHARACTER_IDS = {
    "ch3a8z0_head", "ch6i0z0_head", "ch6i1z0_head", "ch6i2z0_head",
    "ch6i3z0_head", "ch3a8z0_MC_head", "ch6i5z0_head",
};

// [MERCS] Auch hier muessen die Mercs-Bodies rein: bei den NPC_SHARED_WEAPONS
// (Red9) wird das Waffen-GO NUR unter diesen Bodies gesucht. Luis spielt in
// Mercs eine Red9 -- ohne ch6i1z0_body fand das Script seine Waffe nicht, also
// gab es weder Crosshair noch Muzzle fuer den Bullet-Hook.
const std::array<const char*, 8> RE4VRCrosshair::PLAYER_BODY_NAMES = {
    "ch0a0z0_body",      // Leon (Kampagne)
    "ch3a8z0_body",      // Ada (Separate Ways)
    "ch3a8z0_MC_body",   // Ada (Mercs)
    "ch6i0z0_body",      // Leon (Mercs)
    "ch6i1z0_body",      // Luis
    "ch6i2z0_body",      // Krauser
    "ch6i3z0_body",      // HUNK
    "ch6i5z0_body",      // Wesker
};

// Praefixe (5 Zeichen) aller SPIELBAREN Characters -- gebraucht vom NPC-Filter
// im Bullet-Hook. Ohne die Mercs-Eintraege galt dort jeder Mercs-Charakter als
// NPC und der Hook stieg VOR dem Umlenken aus (Leon in Mercs = ch6i0z0_body).
const std::unordered_set<std::string> RE4VRCrosshair::PLAYER_CH_PREFIX = {
    "ch0a0",   // Leon (Kampagne)
    "ch3a8",   // Ada (Separate Ways + Mercs)
    "ch6i0",   // Leon (Mercs)
    "ch6i1",   // Luis
    "ch6i2",   // Krauser
    "ch6i3",   // HUNK
    "ch6i5",   // Wesker
};

// [LUIS-FIX] wp4002 (Red9) traegt auch Luis -> findGameObject liefert evtl.
// SEINE Waffe (erster Szenen-Treffer).
const std::unordered_set<int32_t> RE4VRCrosshair::NPC_SHARED_WEAPONS = {4002};

// Waffen, deren Default-Shape kein Punkt ist (Dev-Liste).
const std::unordered_set<int32_t> RE4VRCrosshair::RETICLE_SHAPE_WEAPONS = {
    4005, 4400, 4401, 4402, 6105, 6114, 6304, 6102, 4501,
};

const std::array<std::pair<const char*, const char*>, 6> RE4VRCrosshair::HUD_TARGETS = {{
    {"Gui_ui2030", "Gun (Munition)"},
    {"Gui_ui2032_default", "Energy/Health-Meter"},
    // [ADA] Ada zeichnet den Energie-Ring als "Gui_ui2032" OHNE das
    // _default-Suffix -> mit nur dem Leon-Namen blieb ihre Leiste als einziges
    // HUD-Element am Kopf haengen. Eigener Eintrag statt Praefix-Match.
    {"Gui_ui2032", "Energy/Health-Meter (Ada)"},
    {"Gui_ui2180", "Body Armor"},
    {"Gui_ui2190", "Knife"},
    {"Gui_ui2083", "Gui_ui2083"},
}};

// ============================================================================
// Referenzzaehlung
// ============================================================================

std::shared_ptr<RE4VRCrosshair>& RE4VRCrosshair::get() {
    static auto inst = std::make_shared<RE4VRCrosshair>();
    return inst;
}

bool RE4VRCrosshair::keep(::REManagedObject* o) {
    if (o == nullptr || !re4vr::obj_ok(o)) {
        return false;
    }

    if (static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        return true;
    }

    return false;
}

bool RE4VRCrosshair::keep_forced(::REManagedObject* o) {
    // Gegenstueck zu Luas obj:add_ref() (force = true): nimmt die Referenz
    // bedingungslos, auch bei referenceCount == 0.
    if (o == nullptr || !re4vr::obj_ok(o)) {
        return false;
    }

    utility::re_managed_object::add_ref(o);
    return true;
}

void RE4VRCrosshair::drop(::REManagedObject* o, bool reffed) {
    if (!reffed || o == nullptr || !re4vr::obj_ok(o)) {
        return;
    }

    utility::re_managed_object::release(o);
}

void RE4VRCrosshair::store(RefHandle& h, ::REManagedObject* o) {
    if (h.obj == o) {
        return;
    }

    drop(h.obj, h.reffed);
    h.obj = o;
    h.reffed = keep(o);
}

void RE4VRCrosshair::store_forced(RefHandle& h, ::REManagedObject* o) {
    if (h.obj == o) {
        return;
    }

    drop(h.obj, h.reffed);
    h.obj = o;
    h.reffed = keep_forced(o);
}

// ============================================================================
// Konfiguration
// ============================================================================

void RE4VRCrosshair::load_cfg() {
    m_cfg = Cfg{};

    try {
        const auto d = re4vr::json_load(CFG_PATH);

        if (!d.is_object()) {
            return;
        }

        // Lua Z.39-46: uebernehmen nur bei type(d[k]) == type(cfg[k]).
        const auto b = [&](const char* k, bool& dst) {
            if (d.contains(k) && d[k].is_boolean()) {
                dst = d[k].get<bool>();
            }
        };
        const auto f = [&](const char* k, float& dst) {
            if (d.contains(k) && d[k].is_number()) {
                dst = d[k].get<float>();
            }
        };

        b("bullet_hook", m_cfg.bullet_hook);
        b("crosshair_off", m_cfg.crosshair_off);
        b("dot_crosshair", m_cfg.dot_crosshair);
        b("reticle_color", m_cfg.reticle_color);
        b("force_reticle_concentrate", m_cfg.force_reticle_concentrate);
        f("reticle_r", m_cfg.reticle_r);
        f("dot_size", m_cfg.dot_size);
        f("reticle_g", m_cfg.reticle_g);
        f("reticle_b", m_cfg.reticle_b);
        f("concentrate_ratio", m_cfg.concentrate_ratio);

        if (d.contains("reticle_scale") && d["reticle_scale"].is_object()) {
            for (auto it = d["reticle_scale"].begin(); it != d["reticle_scale"].end(); ++it) {
                if (it.value().is_number()) {
                    m_cfg.reticle_scale[it.key()] = it.value().get<float>();
                }
            }
        }
    } catch (...) {
        m_cfg = Cfg{};
    }
}

void RE4VRCrosshair::save_cfg() {
    nlohmann::json j;
    j["bullet_hook"] = m_cfg.bullet_hook;
    j["crosshair_off"] = m_cfg.crosshair_off;
    j["dot_crosshair"] = m_cfg.dot_crosshair;
    j["dot_size"] = m_cfg.dot_size;
    j["reticle_color"] = m_cfg.reticle_color;
    j["reticle_r"] = m_cfg.reticle_r;
    j["reticle_g"] = m_cfg.reticle_g;
    j["reticle_b"] = m_cfg.reticle_b;
    j["force_reticle_concentrate"] = m_cfg.force_reticle_concentrate;
    j["concentrate_ratio"] = m_cfg.concentrate_ratio;

    nlohmann::json rs = nlohmann::json::object();

    for (const auto& [k, v] : m_cfg.reticle_scale) {
        rs[k] = v;
    }

    j["reticle_scale"] = rs;
    re4vr::json_save(CFG_PATH, j);
}

void RE4VRCrosshair::load_hud_cfg() {
    m_hud = HudCfg{};

    for (const auto& [name, label] : HUD_TARGETS) {
        m_hud.guis[name] = true;
    }

    try {
        const auto d = re4vr::json_load(HUD_CFG_PATH);

        if (!d.is_object()) {
            return;
        }

        // Lua Z.83-99: DREI Booleans, DREIZEHN Zahlen, und die guis-Map nur
        // fuer bereits bekannte Namen.
        const auto b = [&](const char* k, bool& dst) {
            if (d.contains(k) && d[k].is_boolean()) {
                dst = d[k].get<bool>();
            }
        };
        const auto f = [&](const char* k, float& dst) {
            if (d.contains(k) && d[k].is_number()) {
                dst = d[k].get<float>();
            }
        };

        b("enabled", m_hud.enabled);
        b("hide_hud", m_hud.hide_hud);
        b("ada_rot", m_hud.ada_rot);

        f("dx", m_hud.dx);
        f("dy", m_hud.dy);
        f("dz", m_hud.dz);
        f("scale", m_hud.scale);
        f("rx", m_hud.rx);
        f("ry", m_hud.ry);
        f("rz", m_hud.rz);
        f("ada_rx", m_hud.ada_rx);
        f("ada_ry", m_hud.ada_ry);
        f("ada_rz", m_hud.ada_rz);
        f("ada_dx", m_hud.ada_dx);
        f("ada_dy", m_hud.ada_dy);
        f("ada_dz", m_hud.ada_dz);

        if (d.contains("guis") && d["guis"].is_object()) {
            for (auto it = d["guis"].begin(); it != d["guis"].end(); ++it) {
                auto found = m_hud.guis.find(it.key());

                if (found != m_hud.guis.end() && it.value().is_object()
                    && it.value().contains("enabled") && it.value()["enabled"].is_boolean()) {
                    found->second = it.value()["enabled"].get<bool>();
                }
            }
        }
    } catch (...) {
    }
}

void RE4VRCrosshair::save_hud_cfg() {
    nlohmann::json j;
    j["enabled"] = m_hud.enabled;
    j["hide_hud"] = m_hud.hide_hud;
    j["ada_rot"] = m_hud.ada_rot;
    j["dx"] = m_hud.dx;
    j["dy"] = m_hud.dy;
    j["dz"] = m_hud.dz;
    j["scale"] = m_hud.scale;
    j["rx"] = m_hud.rx;
    j["ry"] = m_hud.ry;
    j["rz"] = m_hud.rz;
    j["ada_rx"] = m_hud.ada_rx;
    j["ada_ry"] = m_hud.ada_ry;
    j["ada_rz"] = m_hud.ada_rz;
    j["ada_dx"] = m_hud.ada_dx;
    j["ada_dy"] = m_hud.ada_dy;
    j["ada_dz"] = m_hud.ada_dz;

    nlohmann::json guis = nlohmann::json::object();

    for (const auto& [name, label] : HUD_TARGETS) {
        nlohmann::json g = nlohmann::json::object();
        g["enabled"] = m_hud.guis[name];
        guis[name] = g;
    }

    j["guis"] = guis;
    re4vr::json_save(HUD_CFG_PATH, j);
}

void RE4VRCrosshair::load_laser_cfg() {
    m_laser = LaserCfg{};

    try {
        const auto d = re4vr::json_load(LASER_CFG_PATH);

        if (!d.is_object()) {
            return;
        }

        const auto b = [&](const char* k, bool& dst) {
            if (d.contains(k) && d[k].is_boolean()) {
                dst = d[k].get<bool>();
            }
        };
        const auto f = [&](const char* k, float& dst) {
            if (d.contains(k) && d[k].is_number()) {
                dst = d[k].get<float>();
            }
        };

        b("enabled", m_laser.enabled);
        b("force_color", m_laser.force_color);
        b("tune_glow", m_laser.tune_glow);
        b("smoke", m_laser.smoke);
        b("dot_raycast", m_laser.dot_raycast);
        f("width", m_laser.width);
        f("length", m_laser.length);
        f("r", m_laser.r);
        f("g", m_laser.g);
        f("b", m_laser.b);
        f("glow", m_laser.glow);
        f("alpha", m_laser.alpha);
        f("smoke_amt", m_laser.smoke_amt);
        f("smoke_speed", m_laser.smoke_speed);
        f("dot_dist", m_laser.dot_dist);
        f("dot_size", m_laser.dot_size);
    } catch (...) {
        m_laser = LaserCfg{};
    }
}

void RE4VRCrosshair::save_laser_cfg() {
    nlohmann::json j;
    j["enabled"] = m_laser.enabled;
    j["width"] = m_laser.width;
    j["length"] = m_laser.length;
    j["force_color"] = m_laser.force_color;
    j["r"] = m_laser.r;
    j["g"] = m_laser.g;
    j["b"] = m_laser.b;
    j["tune_glow"] = m_laser.tune_glow;
    j["glow"] = m_laser.glow;
    j["alpha"] = m_laser.alpha;
    j["smoke"] = m_laser.smoke;
    j["smoke_amt"] = m_laser.smoke_amt;
    j["smoke_speed"] = m_laser.smoke_speed;
    j["dot_raycast"] = m_laser.dot_raycast;
    j["dot_dist"] = m_laser.dot_dist;
    j["dot_size"] = m_laser.dot_size;
    re4vr::json_save(LASER_CFG_PATH, j);
}

// ============================================================================
// Aufbau
// ============================================================================

std::optional<std::string> RE4VRCrosshair::on_initialize() {
    m_scene_td = sdk::find_type_definition("via.SceneManager");

    if (auto* phys = sdk::find_type_definition("via.physics.System")) {
        m_cast_ray_async =
            phys->get_method("castRayAsync(via.physics.CastRayQuery, via.physics.CastRayResult)");
    }

    if (auto* joint = sdk::find_type_definition("via.Joint")) {
        m_joint_get_position = joint->get_method("get_Position");
        m_joint_get_rotation = joint->get_method("get_Rotation");   // [TOT], s. Spec 16.2
    }

    if (auto* v3 = sdk::find_type_definition("via.vec3")) {
        m_set_item_vec3 = v3->get_method("set_Item(System.Int32, System.Single)");
    }

    if (auto* q = sdk::find_type_definition("via.Quaternion")) {
        m_set_item_quat = q->get_method("set_Item(System.Int32, System.Single)");
    }

    m_t_player_equipment = re4vr::runtime_type(game_namespace("PlayerEquipment").data());
    m_t_arms = re4vr::runtime_type(game_namespace("Arms").data());
    m_t_gui = re4vr::runtime_type("via.gui.GUI");
    m_t_catalog_register = re4vr::runtime_type(game_namespace("WeaponCatalogRegister").data());
    m_t_custom_catalog_register =
        re4vr::runtime_type(game_namespace("WeaponCustomCatalogRegister").data());

    for (auto* t : {m_t_player_equipment, m_t_arms, m_t_gui, m_t_catalog_register,
                    m_t_custom_catalog_register}) {
        (void)keep(t);   // dauerhaft gehalten
    }

    // [ENUM] generate_statics: alle statischen Felder des Typs.
    // sdk::REField::get_data<T>() DEREFERENZIERT bereits (RETypeDB.hpp:1237) --
    // nur das rohe get_data_raw liefert die Adresse.
    const auto statics = [](const char* type_name, const char* field_name,
                            int32_t& out) -> bool {
        auto* td = sdk::find_type_definition(type_name);

        if (td == nullptr) {
            return false;
        }

        for (auto* f : td->get_fields()) {
            if (f == nullptr || !f->is_static()) {
                continue;
            }

            if (f->get_name() == std::string_view{field_name}) {
                try {
                    out = f->get_data<int32_t>(nullptr);
                    return true;
                } catch (...) {
                    return false;
                }
            }
        }

        return false;
    };

    const auto layer_type = std::string{game_namespace("CollisionUtil.Layer")};
    const auto filter_type = std::string{game_namespace("CollisionUtil.Filter")};

    (void)statics(layer_type.c_str(), "Bullet", m_layer_bullet);
    m_have_filter = statics(filter_type.c_str(), "DamageCheckOtherThanPlayer",
                            m_filter_damage_check_other_than_player);

    load_cfg();
    load_hud_cfg();
    load_laser_cfg();

    // ---- Die sechs Hooks (Lua Z.119, 142, 1006, 1007, 1008, 1338) ----
    // Die _G-Installationsriegel des Originals entfallen: das hier laeuft einmal.
    const auto add_pre = [](sdk::REMethodDefinition* m,
                            void (RE4VRCrosshair::*fn)(std::vector<uintptr_t>&)) {
        if (m == nullptr) {
            return;
        }

        g_hookman.add(
            m,
            [fn](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                ((*RE4VRCrosshair::get()).*fn)(args);
                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
    };

    if (auto* td = sdk::find_type_definition(game_namespace("ReticleGuiBehavior"))) {
        add_pre(td->get_method("set_CurrConcentrateRatio(System.Single)"),
                &RE4VRCrosshair::hook_pre_concentrate_ratio);

        auto* ac = td->get_method("applyConcentrate(System.Boolean)");

        if (ac == nullptr) {
            ac = td->get_method("applyConcentrate");
        }

        add_pre(ac, &RE4VRCrosshair::hook_pre_apply_concentrate);
    }

    if (auto* td = sdk::find_type_definition(game_namespace("BulletShellGenerator"))) {
        add_pre(td->get_method("requestFire"), &RE4VRCrosshair::hook_pre_request_fire);
    }

    if (auto* td = sdk::find_type_definition(game_namespace("ShotgunShellGenerator"))) {
        add_pre(td->get_method("requestFire"), &RE4VRCrosshair::hook_pre_request_fire);
    }

    if (auto* td = sdk::find_type_definition(game_namespace("RocketLauncherShellGenerator"))) {
        add_pre(td->get_method("requestGenerate"), &RE4VRCrosshair::hook_pre_rocket_generate);
    }

    if (auto* td = sdk::find_type_definition(game_namespace("LaserSightController"))) {
        auto* lm = td->get_method("updateLaser()");

        if (lm == nullptr) {
            lm = td->get_method("updateLaser");
        }

        if (lm != nullptr) {
            g_hookman.add(
                lm,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    RE4VRCrosshair::get()->hook_pre_update_laser(args);
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {
                    RE4VRCrosshair::get()->hook_post_update_laser();
                });
        }
    }

    return Mod::on_initialize();
}

void RE4VRCrosshair::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRCrosshair", "on_lua_state_created");
    // [PROMPT-FENSTER] Beide sind ECHTE Lua-Funktionen -- vier bzw. eine Datei
    // rufen sie auf. Frische-Fenster statt Flag: es kann nichts haengenbleiben,
    // wenn das zeichnende GUI verschwindet.
    lua["__re4_finisher_prompt_seen"] = 0.0;
    lua["__re4_dodge_prompt_seen"] = 0.0;

    lua["__re4_is_finisher_prompt"] = []() -> bool {
        auto l = ch_lua_state();

        if (l == nullptr) {
            return false;
        }

        const double seen = (*l)["__re4_finisher_prompt_seen"].get_or(0.0);
        return (now_clock() - seen) < 0.15;
    };

    lua["__re4_is_dodge_prompt"] = []() -> bool {
        auto l = ch_lua_state();

        if (l == nullptr) {
            return false;
        }

        const double seen = (*l)["__re4_dodge_prompt_seen"].get_or(0.0);
        return (now_clock() - seen) < 0.15;
    };

    // [ALT-SYSTEM] Ein aus einer frueheren Session noch installierter
    // updateLaser-Hook wuerde diese beiden rufen -- auf Pass-Through setzen,
    // damit er sofort nichts mehr tut (Lua Z.457-458).
    lua["__re4_vr_laser_pre"] = [](sol::object) {};
    lua["__re4_vr_laser_post"] = [](sol::object retval) { return retval; };

    // Beim Laden mitgesetzt (Lua Z.111-112).
    lua["__re4_force_concentrate"] = m_cfg.force_reticle_concentrate;
    lua["__re4_concentrate_bits"] = static_cast<double>(float_bits(m_cfg.concentrate_ratio));

    // Die vier Public-UI-Zeichner. Angemeldet wird TRAEGE (s.
    // ensure_public_ui_registered) -- hier waeren sie zu frueh.
    lua["__re4_crosshair_draw_public_off"] = []() {
        RE4VRCrosshair::get()->draw_public_crosshair_off();
    };
    lua["__re4_crosshair_draw_public_laser_color"] = []() {
        RE4VRCrosshair::get()->draw_public_laser_color();
    };
    lua["__re4_crosshair_draw_public_reticle_color"] = []() {
        RE4VRCrosshair::get()->draw_public_reticle_color();
    };
    lua["__re4_crosshair_draw_public_reticle_size"] = []() {
        RE4VRCrosshair::get()->draw_public_reticle_size();
    };

    // [S1] Globals ohne Leser, aber 1:1 aus dem Original (Spec 1.3).
    lua["__ch_tree_open"] = false;
    lua["__re4_concentrate_hook_installed"] = true;
    lua["__re4_apply_concentrate_installed"] = true;
    lua["__re4_vr_crosshair_hooks_installed"] = true;
    lua["__re4_laser_track_installed"] = true;
    lua["__re4_laser_last"] = 0.0;
    lua["__re4_vr_reticle_param_err_logged"] = sol::lua_nil;
    lua["__re4_apply_concentrate_cb"] = [](sol::object) {};
    lua["__re4_laser_pre"] = [](sol::object) {};
    lua["__re4_laser_post"] = [](sol::object retval) { return retval; };
    lua["__re4_laser_apply"] = [](sol::object) {};
    lua["__re4_vr_crosshair_pre_fire"] = [](sol::object) {};
    lua["__re4_vr_crosshair_post_fire"] = [](sol::object retval) { return retval; };
    lua["__re4_vr_crosshair_pre_rocket"] = [](sol::object) {};

    m_public_ui_registered = false;
    m_dispatcher_present = false;
}

void RE4VRCrosshair::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRCrosshair", "on_lua_state_destroyed");
    // Lua Z.1145-1156 (on_script_reset) ...
    store(m_attack_ray, nullptr);
    store(m_bullet_ray, nullptr);
    store(m_cached_pl_head, nullptr);
    store(m_cached_gun_obj, nullptr);
    m_cached_weapon_id.reset();
    m_reticle_params_applied = false;
    store(m_scene, nullptr);

    // ... plus das, was in Lua das Neu-Ausfuehren der Datei erledigt.
    store(m_current_muzzle_joint, nullptr);
    store(m_last_muzzle_joint, nullptr);
    store(m_laser_this, nullptr);
    m_current_laser_active = false;
    m_dot_armed = false;
    m_dot_stage.reset();
    m_dot_body_weg = false;
    m_public_ui_registered = false;
    m_dispatcher_present = false;
    store(m_laser_ctrl, nullptr);

    load_cfg();
    load_hud_cfg();
    load_laser_cfg();

    // BEWUSST NICHT zurueckgesetzt (Spec 1.4/13): die neun re4.*-Felder und
    // current_weapon_id -- utility/RE4 ist modul-gecacht, diese Werte
    // ueberleben in Lua einen Script-Reset ebenfalls.
}

// ============================================================================
// Helfer
// ============================================================================

bool RE4VRCrosshair::ks_active() const {
    // Lua Z.19-26: Modul-Ladefehler -> is_active() liefert false.
    return re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false);
}

::REManagedObject* RE4VRCrosshair::get_scene() {
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

// ============================================================================
// Raycast (Lua Z.150-189, 269-322)
// ============================================================================

::REManagedObject* RE4VRCrosshair::cast_ray_async(RefHandle& query_slot,
                                                  ::REManagedObject* ray_result,
                                                  const glm::vec3& start, const glm::vec3& end,
                                                  int32_t layer,
                                                  std::optional<int32_t> filter_value) {
    if (m_cast_ray_async == nullptr) {
        return ray_result;
    }

    const auto phys = sdk::get_native_singleton("via.physics.System");

    if (phys == nullptr) {
        return ray_result;
    }

    // [PORTFIX 2026-09-06] Query gehalten und gepinnt -- s. RE4VRCrosshair.hpp.
    // Das Original erzeugt sie pro Aufruf; in Lua haelt die lokale Variable sie
    // aber am Leben, bis der asynchrone Ray sie abgearbeitet hat.
    // [ZWEI QUERYS 07.09.2026] Je Strahl eine eigene Query -- s. Header.
    if (query_slot.obj == nullptr) {
        if (auto* q = sdk::create_instance<::REManagedObject>("via.physics.CastRayQuery", false)) {
            store_forced(query_slot, q);
        }
    }

    auto* ray_query = query_slot.obj;

    if (ray_query == nullptr) {
        return ray_result;
    }

    if (ray_result == nullptr) {
        ray_result = sdk::create_instance<::REManagedObject>("via.physics.CastRayResult", false);

        if (ray_result == nullptr) {
            return nullptr;
        }
    }

    // setRay(via.vec3, via.vec3) -- zwei 16-Byte-Puffer.
    if (auto* m = find_method(ray_query, "setRay(via.vec3, via.vec3)")) {
        auto context = sdk::get_thread_context();
        // [M8] wie Luas build_args: w = 0.0f
        __declspec(align(16)) glm::vec4 a{start.x, start.y, start.z, 0.0f};
        __declspec(align(16)) glm::vec4 b{end.x, end.y, end.z, 0.0f};

        try {
            m->call_safe<void*>(context, ray_query, &a, &b);
        } catch (...) {
        }

        clear_pending(context, true);
    }

    re4vr::call_safe<void*>(ray_query, "clearOptions");
    re4vr::call_safe<void*>(ray_query, "enableAllHits");
    re4vr::call_safe<void*>(ray_query, "enableNearSort");

    // ========================================================================
    // [FILTER-PORTFIX 07.09.2026 -- Laser ging durch alle Oberflaechen]
    // Zwei Fehler an derselben Stelle, beide schon einmal bezahlt (RE4VRWeapons
    // knife_read_ray, 06.09.):
    //
    // 1. `set_FilterInfo` erwartet einen via.physics.FilterInfo -- KEINE Zahl.
    //    Lua uebergibt `CollisionFilter.DamageCheckOtherThanPlayer`, und
    //    REFramework baut daraus per build_args das passende Argument. Nativ
    //    landete die nackte Zahl im Register, die Query trug Muell. Sichtbar
    //    wurde das hier nicht als Absturz, weil der Cast in try/catch laeuft
    //    und clear_vm_exception die Ausnahme wegraeumt -- der Strahl meldete
    //    einfach nie einen Kontakt.
    // 2. Die Setter roh gerufen uebergeben die Zahl anders als build_args.
    //    Sie muessen ueber call_cmd + arg_int laufen.
    //
    // Folge beider Fehler: get_NumContactPoints blieb 0, update_crosshair_world_pos
    // nahm den "kein Treffer"-Zweig und setzte die Himmelsdistanz 100 -- der
    // Laserpunkt lief durch jede Wand.
    // ========================================================================
    if (!m_dmg_filter_done) {
        if (auto* ftd = sdk::find_type_definition(game_namespace("CollisionUtil.Filter"))) {
            if (auto* f = ftd->get_field("DamageCheckOtherThanPlayer")) {
                try {
                    if (auto* fo = f->get_data<::REManagedObject*>(nullptr)) {
                        store_forced(m_dmg_filter, fo);
                    }
                } catch (...) {
                }

                if (m_dmg_filter.obj == nullptr) {
                    // Der Weg, den Luas field:get_data(nil) intern nimmt.
                    try {
                        if (auto* raw = f->get_data_raw(nullptr, false)) {
                            if (auto* fo = *reinterpret_cast<::REManagedObject**>(raw)) {
                                store_forced(m_dmg_filter, fo);
                            }
                        }
                    } catch (...) {
                    }
                }
            }
        }

        // Erst als erledigt vermerken, wenn der Filter wirklich da ist: die
        // statische Tabelle des Typs existiert erst, wenn die Klasse benutzt
        // wurde (derselbe Fehler wie in RE4VRWeapons am 06.09.).
        if (m_dmg_filter.obj != nullptr) {
            m_dmg_filter_done = true;
        }
    }

    if (filter_value.has_value() && m_dmg_filter.obj != nullptr) {
        // Lua Z.319: Re-Cast des Attack-Strahls MIT
        // CollisionFilter.DamageCheckOtherThanPlayer -- Group/MaskBits/Layer
        // werden dabei bewusst uebersprungen.
        std::array<void*, 1> fa{static_cast<void*>(m_dmg_filter.obj)};
        re4vr::call_cmd(ray_query, "set_FilterInfo", std::span<void*>(fa));
    } else if (auto* fi = re4vr::call_safe<::REManagedObject*>(ray_query, "get_FilterInfo")) {
        // Lua Z.297-302: Group 0, MaskBits alles ausser Bit 0, Layer wie ueber-
        // geben (5 = Damage, 10 = Bullet).
        std::array<void*, 1> g{re4vr::arg_int(0)};
        re4vr::call_cmd(fi, "set_Group", std::span<void*>(g));
        std::array<void*, 1> mb{re4vr::arg_int(0xFFFFFFFFu & ~1u)};
        re4vr::call_cmd(fi, "set_MaskBits", std::span<void*>(mb));
        std::array<void*, 1> ly{re4vr::arg_int(layer)};
        re4vr::call_cmd(fi, "set_Layer", std::span<void*>(ly));
        std::array<void*, 1> fa{static_cast<void*>(fi)};
        re4vr::call_cmd(ray_query, "set_FilterInfo", std::span<void*>(fa));
    }

    // [INVOKE STATT CALL_SAFE 20.09.2026] Dieselbe Falle wie in RE4VRWeapons
    // (PORTFIX 06.09., los_clear): call_safe legt die Argumente roh in die
    // Register, bei den Physik-Casts passt das nicht -> c0000005 im Spielcode
    // (re4.exe+0xEB2AC1). Der erste Wurf starb daran, danach meldete das
    // Result nie "fertig" -- das Fadenkreuz hing seither auf dem festen
    // Ersatzabstand 10 m. `invoke` ist der Weg, den Lua nimmt.
    try {
        m_cast_ray_async->invoke(phys, (void*)ray_query, (void*)ray_result);
    } catch (...) {
    }

    re4vr::clear_vm_exception();
    return ray_result;
}

void RE4VRCrosshair::update_crosshair_world_pos(const glm::vec3& start, const glm::vec3& end) {
    if (m_attack_ray.obj == nullptr || m_bullet_ray.obj == nullptr) {
        auto* a = cast_ray_async(m_ray_query_attack, m_attack_ray.obj, start, end, 5,
                                 std::nullopt);
        auto* b = cast_ray_async(m_ray_query_bullet, m_bullet_ray.obj, start, end, 10,
                                 std::nullopt);

        // Lua ruft hier ausdruecklich add_ref() -- force, also auch bei
        // referenceCount == 0 (Z.273-274).
        store_forced(m_attack_ray, a);
        store_forced(m_bullet_ray, b);
    }

    const auto num_contacts = [](::REManagedObject* r) -> int32_t {
        int32_t n = 0;
        re4vr::try_call<int32_t>(r, "get_NumContactPoints", n);
        return n;
    };

    bool fa = false;
    bool fb = false;
    re4vr::try_call<bool>(m_attack_ray.obj, "get_Finished", fa);
    re4vr::try_call<bool>(m_bullet_ray.obj, "get_Finished", fb);

    const bool finished = fa && fb;
    const bool attack_hit = finished && num_contacts(m_attack_ray.obj) > 0;
    const bool any_hit = finished && (attack_hit || num_contacts(m_bullet_ray.obj) > 0);
    const bool both_hit =
        finished && num_contacts(m_attack_ray.obj) > 0 && num_contacts(m_bullet_ray.obj) > 0;

    // ========================================================================
    // [VALUETYPE-FALLE 07.09.2026] `via.physics.ContactPoint` ist ein
    // VALUETYPE (80 Byte) -- die Engine schreibt ihn in einen sret-Puffer, den
    // der Aufrufer stellt. Hier stand `call_safe<REManagedObject*>`, holte also
    // einen Objektzeiger ab und bekam Muell; das anschliessende Lesen von
    // "Distance" scheiterte JEDES Mal. Damit lief update_crosshair_world_pos
    // immer in den letzten else-Zweig und setzte die Distanz auf den festen
    // Fallback 10.0 -- der Laser-Dot (Killer 7 und jedes Laser-Addon) klebte
    // dadurch in konstanter Entfernung, statt auf der Oberflaeche zu sitzen.
    // In Lua konnte das nicht passieren: REFramework stellt den Puffer bei
    // ValueType-Rueckgaben selbst (crosshair.lua Z.294).
    // Exakt dieselbe Falle und derselbe Fix wie in RE4VRWeapons.cpp
    // (knife_read_ray) -- s. [[reference_re4_cpp_valuetype_rueckgabe_sret]].
    // Die Feld-Offsets stehen zentral in RE4VR.hpp (re4vr::contact_point).
    // ========================================================================
    struct ContactData {
        bool ok{false};
        float distance{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 0.0f};
    };

    const auto read_contact = [](::REManagedObject* r) -> ContactData {
        ContactData out{};

        if (r == nullptr) {
            return out;
        }

        auto* mcp = find_method(r, "getContactPoint(System.UInt32)");

        if (mcp == nullptr) {
            return out;
        }

        struct alignas(16) ContactPointBuf {
            uint8_t b[re4vr::contact_point::SIZE];
        };

        auto context = sdk::get_thread_context();
        ContactPointBuf cpb{};
        bool cp_ok = false;

        try {
            mcp->call_safe<ContactPointBuf*>(&cpb, context, r, static_cast<uint32_t>(0));
            cp_ok = true;
        } catch (...) {
            cp_ok = false;
        }

        cp_ok = clear_pending(context, cp_ok);

        if (!cp_ok) {
            return out;
        }

        const auto* n = reinterpret_cast<const float*>(cpb.b + re4vr::contact_point::NORMAL);

        out.ok = true;
        out.distance = *reinterpret_cast<const float*>(cpb.b + re4vr::contact_point::DISTANCE);
        out.normal = glm::vec3{n[0], n[1], n[2]};
        return out;
    };


    if (finished && any_hit) {
        ::REManagedObject* best = nullptr;

        if (both_hit) {
            // Lua Z.287-289: der mit der KLEINEREN Distanz gewinnt.
            const auto a = read_contact(m_attack_ray.obj);
            const auto b = read_contact(m_bullet_ray.obj);
            best = (a.distance < b.distance) ? m_attack_ray.obj : m_bullet_ray.obj;
        } else {
            best = attack_hit ? m_attack_ray.obj : m_bullet_ray.obj;
        }

        const auto cp = read_contact(best);

        if (cp.ok) {
            float dist = cp.distance;
            const bool have_dist = true;

            // [S4] Lua Z.297-298 setzt die Richtung VOR dem Distanz-Test.
            m_crosshair_dir = glm::normalize(end - start);

            if (have_dist) {
                if (dist > 100.0f) {
                    dist = 100.0f;
                }

                // Normal stammt aus demselben sret-Puffer (Offset 0x20).
                m_crosshair_normal = cp.normal;

                m_crosshair_distance = dist;
                m_crosshair_pos = start + (m_crosshair_dir * dist);
            }
        }
    } else if (finished && !any_hit) {
        constexpr float sky_distance = 100.0f;
        m_crosshair_dir = glm::normalize(end - start);
        m_crosshair_distance = sky_distance;
        m_crosshair_pos = start + (m_crosshair_dir * sky_distance);
    } else {
        m_crosshair_dir = glm::normalize(end - start);

        if (m_crosshair_distance.has_value()) {
            m_crosshair_pos = start + (m_crosshair_dir * *m_crosshair_distance);
        } else {
            m_crosshair_pos = start + (m_crosshair_dir * 10.0f);
            m_crosshair_distance = 10.0f;
        }
    }

    if (finished) {
        // attack MIT dem Zahl-Filter, bullet ohne.
        cast_ray_async(m_ray_query_attack, m_attack_ray.obj, start, end, 5,
                       m_have_filter
                           ? std::optional<int32_t>{m_filter_damage_check_other_than_player}
                           : std::nullopt);
        cast_ray_async(m_ray_query_bullet, m_bullet_ray.obj, start, end, 10, std::nullopt);
    }
}

// ============================================================================
// Muendungsdaten (Lua Z.329-449)
// ============================================================================

::REManagedObject* RE4VRCrosshair::find_weapon_on_player_body(const std::string& weapon_name) {
    // [LUIS-FIX] Fuer NPC-geteilte Waffen die Waffe als DIREKTES Kind des
    // SPIELER-Bodys suchen, nie global.
    if (m_scene.obj == nullptr) {
        return nullptr;
    }

    for (const char* body_name : PLAYER_BODY_NAMES) {
        auto* str = sdk::VM::create_managed_string(utility::widen(body_name));

        if (str == nullptr) {
            continue;
        }

        auto* pb = re4vr::call_safe<::REManagedObject*>(m_scene.obj,
                                                       "findGameObject(System.String)", str);

        if (pb == nullptr) {
            continue;
        }

        auto* btf = re4vr::call_safe<::REManagedObject*>(pb, "get_Transform");

        if (btf == nullptr) {
            continue;
        }

        auto* child = re4vr::call_safe<::REManagedObject*>(btf, "get_Child");
        int guard = 0;

        while (child != nullptr && guard < 60) {
            ++guard;

            auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

            if (cgo != nullptr && obj_name_of(cgo) == weapon_name) {
                return cgo;
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }
    }

    return nullptr;
}

void RE4VRCrosshair::update_muzzle_data() {
    if (m_scene.obj == nullptr) {
        store(m_scene, get_scene());

        if (m_scene.obj == nullptr) {
            return;
        }
    }

    const double current_time = now_clock();
    constexpr double CACHE_REFRESH_INTERVAL = 1.0;

    const auto find_go = [&](const char* name) -> ::REManagedObject* {
        auto* str = sdk::VM::create_managed_string(utility::widen(name));

        if (str == nullptr) {
            return nullptr;
        }

        return re4vr::call_safe<::REManagedObject*>(m_scene.obj, "findGameObject(System.String)",
                                                   str);
    };

    if (m_cached_pl_head.obj == nullptr
        || (current_time - m_cache_refresh_time) > CACHE_REFRESH_INTERVAL) {
        auto* head = find_go("ch0a0z0_head");

        if (head == nullptr) {
            for (const char* cid : CHARACTER_IDS) {
                head = find_go(cid);

                if (head != nullptr) {
                    break;
                }
            }
        }

        store(m_cached_pl_head, head);
        m_cache_refresh_time = current_time;   // Lua Z.384
    }

    if (m_cached_pl_head.obj == nullptr) {
        return;
    }

    {
        bool valid = false;

        if (re4vr::try_call<bool>(m_cached_pl_head.obj, "get_Valid", valid) && !valid) {
            store(m_cached_pl_head, nullptr);
            return;
        }
    }

    auto* player_equip =
        (m_t_player_equipment != nullptr)
            ? re4vr::call_safe<::REManagedObject*>(m_cached_pl_head.obj,
                                                  "getComponent(System.Type)", m_t_player_equipment)
            : nullptr;

    if (player_equip == nullptr) {
        return;
    }

    int32_t equip_weapon = 0;

    if (!re4vr::try_call<int32_t>(player_equip, "get_EquipWeaponID()", equip_weapon)) {
        return;
    }

    m_current_weapon_id = equip_weapon;   // Lua Z.396, wird NIE zurueckgesetzt

    if (m_cached_gun_obj.obj == nullptr || m_cached_weapon_id != equip_weapon
        || (current_time - m_cache_refresh_time) > CACHE_REFRESH_INTERVAL) {
        const std::string base = "wp" + std::to_string(equip_weapon);
        ::REManagedObject* gun = nullptr;

        if (NPC_SHARED_WEAPONS.count(equip_weapon) > 0) {
            gun = find_weapon_on_player_body(base);

            if (gun == nullptr) {
                gun = find_weapon_on_player_body(base + "_AO");
            }

            if (gun == nullptr) {
                gun = find_weapon_on_player_body(base + "_MC");
            }
        } else {
            gun = find_go(base.c_str());

            if (gun == nullptr) {
                gun = find_go((base + "_AO").c_str());
            }

            if (gun == nullptr) {
                gun = find_go((base + "_MC").c_str());
            }
        }

        store(m_cached_gun_obj, gun);
        m_cached_weapon_id = equip_weapon;
        m_cache_refresh_time = current_time;   // Lua Z.411 -- ZWEITER Schreibvorgang
    }

    // Lua Z.413: is_valid_managed.
    if (m_cached_gun_obj.obj == nullptr || !re4vr::obj_ok(m_cached_gun_obj.obj)) {
        return;
    }

    auto* bt_arms = (m_t_arms != nullptr)
                        ? re4vr::call_safe<::REManagedObject*>(
                              m_cached_gun_obj.obj, "getComponent(System.Type)", m_t_arms)
                        : nullptr;

    // [LASER-RETICLE] Jedes Update neu: Laser-Addon ab -> Reticle-Dot ist
    // sofort wieder da.
    m_current_laser_active = false;

    if (bt_arms != nullptr) {
        bool en = false;

        if (re4vr::try_call<bool>(bt_arms, "get_EnableLaserSight", en) && en) {
            m_current_laser_active = true;
        }
    }

    ::REManagedObject* muzzle_joint =
        (bt_arms != nullptr)
            ? re4vr::call_safe<::REManagedObject*>(bt_arms, "getMuzzleJoint")
            : nullptr;

    if (muzzle_joint == nullptr) {
        auto* gtf = re4vr::call_safe<::REManagedObject*>(m_cached_gun_obj.obj, "get_Transform");

        if (gtf != nullptr) {
            auto* s1 = sdk::VM::create_managed_string(L"vfx_muzzle");
            muzzle_joint = (s1 != nullptr) ? re4vr::call_safe<::REManagedObject*>(
                                                 gtf, "getJointByName", s1)
                                           : nullptr;

            if (muzzle_joint == nullptr) {
                auto* s2 = sdk::VM::create_managed_string(L"vfx_muzzle1");
                muzzle_joint = (s2 != nullptr) ? re4vr::call_safe<::REManagedObject*>(
                                                     gtf, "getJointByName", s2)
                                               : nullptr;
            }
        }
    }

    if (muzzle_joint != nullptr) {
        // Waffen-GO haengt via motion an der VR-Hand -> Joint sitzt korrekt,
        // AxisZ = Laufachse.
        store(m_current_muzzle_joint, muzzle_joint);

        // [K5/S5] Lua weist BEIDE Werte unbedingt und unabhaengig zu; schlaegt
        // ein Getter fehl, wird das Feld nil und der Raycast sowie die
        // Muzzle-Guards der Feuer-Hooks fallen aus. m_have_muzzle bildet genau
        // dieses "beide Felder stehen" ab und muss deshalb auch wieder false
        // werden koennen.
        glm::vec3 p{};
        glm::vec3 f{};
        const bool ok_p = get_vec3(muzzle_joint, "get_Position", p);
        const bool ok_f = get_vec3(muzzle_joint, "get_AxisZ", f);

        if (ok_p) {
            m_last_muzzle_pos = p;
        }

        if (ok_f) {
            m_last_muzzle_forward = f;
        }

        m_have_muzzle = ok_p && ok_f;

        store(m_last_muzzle_joint, muzzle_joint);
    } else {
        // Nur diese beiden -- die uebrigen Felder bleiben BEWUSST stehen.
        store(m_current_muzzle_joint, nullptr);
        store(m_last_muzzle_joint, nullptr);
    }
}

// ============================================================================
// Reticle ruhigstellen (Lua Z.513-655)
// ============================================================================

void RE4VRCrosshair::apply_point_range(void* param_ptr, sdk::RETypeDefinition* param_td,
                                      bool container_is_value) {
    // [1:1 -- genau der Weg des Originals]
    // Lua: local pointRange = param:get_field("_PointRange")   -- eine KOPIE
    //      pointRange.s = 100 ; pointRange.r = 100             -- TYPGERECHT
    //      write_valuetype(param, 0x10, pointRange)            -- ABSOLUT nach 0x10
    // Das echte Feld _PointRange wird also nur dann veraendert, wenn es
    // zufaellig auf Offset 0x10 liegt. Genau das bilden wir nach: Kopie
    // anlegen, in der Kopie schreiben, Kopie nach 0x10 blitten.
    if (param_ptr == nullptr || param_td == nullptr) {
        return;
    }

    // [WERTTYP = KOPIE] Ist der Container ein roher Struct-Zeiger, dann hielt
    // Lua an dieser Stelle eine memcpy-KOPIE (Sdk.cpp parse_data) -- sowohl das
    // Lesen von _PointRange als auch der write_valuetype nach 0x10 landeten
    // dort in der Kopie und waren wirkungslos. Der Port muss ebenso nichts tun.
    // Zusaetzlich waere die harte 0x10 hier falsch: sie ueberspringt den
    // Managed-Object-Header, den eine eingebettete Struct nicht hat.
    if (container_is_value) {
        return;
    }

    auto* pr_field = param_td->get_field("_PointRange");

    if (pr_field == nullptr) {
        return;
    }

    auto* prt = pr_field->get_type();

    if (prt == nullptr) {
        return;
    }

    const auto size = prt->get_valuetype_size();

    if (size == 0 || size > 256) {
        return;
    }

    auto* src = reinterpret_cast<uint8_t*>(pr_field->get_data_raw(param_ptr, container_is_value));

    if (src == nullptr) {
        return;
    }

    // 1) Kopie ziehen.
    uint8_t buf[256]{};
    std::memcpy(buf, src, size);

    // 2) In der Kopie typgerecht schreiben -- Lua geht ueber set_native_field
    //    und konvertiert; ein hartes float-Write waere bei einem Int-Feld Muell.
    const auto set_member = [&](const char* name) {
        auto* mf = prt->get_field(name);

        if (mf == nullptr) {
            return;
        }

        // Wir schreiben in eine KOPIE, also einen rohen Struct-Zeiger ->
        // immer der fieldptr-Offset.
        const auto off = mf->get_offset_from_fieldptr();

        auto* ft = mf->get_type();
        const std::string tn = (ft != nullptr) ? ft->get_full_name() : std::string{};

        // Breite des Ziels bestimmen und die Schranke ECHT pruefen -- die
        // frühere Fassung hatte einen leeren Rumpf und prüfte gar nichts.
        // Lua prueft ueber is_valid_offset jedes Byte gegen die Strukturgroesse
        // und ueberspringt den Write still, wenn es nicht passt.
        size_t width = 0;

        if (tn == "System.Single" || tn == "System.Int32" || tn == "System.UInt32") {
            width = 4;
        } else if (tn == "System.Double" || tn == "System.Int64" || tn == "System.UInt64") {
            width = 8;
        } else if (tn == "System.Int16" || tn == "System.UInt16") {
            width = 2;
        } else if (tn == "System.SByte" || tn == "System.Byte") {
            width = 1;
        }

        if (width == 0 || off < 0 || static_cast<size_t>(off) + width > size) {
            return;
        }

        auto* dst = buf + off;

        if (tn == "System.Single") {
            *reinterpret_cast<float*>(dst) = 100.0f;
        } else if (tn == "System.Double") {
            *reinterpret_cast<double*>(dst) = 100.0;
        } else if (tn == "System.Int32" || tn == "System.UInt32") {
            *reinterpret_cast<int32_t*>(dst) = 100;
        } else if (tn == "System.Int16" || tn == "System.UInt16") {
            *reinterpret_cast<int16_t*>(dst) = 100;
        } else if (tn == "System.SByte" || tn == "System.Byte") {
            *dst = 100;
        } else if (tn == "System.Int64" || tn == "System.UInt64") {
            *reinterpret_cast<int64_t*>(dst) = 100;
        }
    };

    set_member("s");
    set_member("r");

    // 3) Kopie nach param + 0x10 blitten -- absolut, wie write_valuetype.
    std::memcpy(reinterpret_cast<uint8_t*>(param_ptr) + 0x10, buf, size);
}

void RE4VRCrosshair::process_weapon_data(::REManagedObject* weapon_data) {
    if (weapon_data == nullptr) {
        return;
    }

    auto* def = utility::re_managed_object::get_type_definition(weapon_data);

    if (def == nullptr) {
        return;
    }

    int32_t weapon_id = 0;

    if (auto* wf = def->get_field("_WeaponID")) {
        try {
            weapon_id = wf->get_data<int32_t>(weapon_data);
        } catch (...) {
            weapon_id = 0;
        }
    }

    // [M3] Lua liest ausschliesslich das FELD (Z.585) -- kein Getter.
    void* tbl = nullptr;
    sdk::RETypeDefinition* tbl_td = nullptr;
    bool tbl_is_value = false;

    if (!field_target(weapon_data, def, "_ReticleFitParamTable", tbl, tbl_td, false,
                      tbl_is_value)) {
        return;
    }

    if (RETICLE_SHAPE_WEAPONS.count(weapon_id) > 0) {
        write_field_number(tbl, tbl_td, "_ReticleShape", RETICLE_VALUE, tbl_is_value);
    }

    {
        void* dp = nullptr;
        sdk::RETypeDefinition* dp_td = nullptr;
        bool dp_is_value = false;

        if (field_target(tbl, tbl_td, "_DefaultParam", dp, dp_td, tbl_is_value, dp_is_value)) {
            apply_point_range(dp, dp_td, dp_is_value);
        }
    }

    void* custom_params = nullptr;
    sdk::RETypeDefinition* cp_td = nullptr;
    bool cp_is_value = false;

    if (!field_target(tbl, tbl_td, "_CustomParams", custom_params, cp_td, tbl_is_value,
                      cp_is_value)) {
        return;
    }

    auto* custom_obj = reinterpret_cast<::REManagedObject*>(custom_params);
    int32_t count = 0;
    re4vr::try_call<int32_t>(custom_obj, "get_Count", count);

    for (int32_t i = 0; i < count; ++i) {
        auto* cp = re4vr::call_safe<::REManagedObject*>(custom_obj, "get_Item", i);

        if (cp == nullptr) {
            continue;
        }

        // [PFLICHT] Die Indirektion ueber _Param -- nicht cp selbst.
        auto* cpdef = utility::re_managed_object::get_type_definition(cp);
        void* p = nullptr;
        sdk::RETypeDefinition* p_td = nullptr;

        bool p_is_value = false;

        if (field_target(cp, cpdef, "_Param", p, p_td, false, p_is_value)) {
            apply_point_range(p, p_td, p_is_value);
        }
    }
}

void RE4VRCrosshair::apply_reticle_params() {
    // Einmalig mit Retry: der WeaponCatalog existiert in den ersten Frames
    // einer Szene noch nicht ("kein catalog" -> naechster Frame).
    if (m_reticle_params_applied) {
        return;
    }

    if (m_scene.obj == nullptr) {
        return;
    }

    const auto find_go = [&](const char* name) -> ::REManagedObject* {
        auto* str = sdk::VM::create_managed_string(utility::widen(name));

        if (str == nullptr) {
            return nullptr;
        }

        return re4vr::call_safe<::REManagedObject*>(m_scene.obj, "findGameObject(System.String)",
                                                   str);
    };

    const auto data_table_of = [&](::REManagedObject* catalog_go) -> ::REManagedObject* {
        if (catalog_go == nullptr || m_t_catalog_register == nullptr) {
            return nullptr;
        }

        auto* reg = re4vr::call_safe<::REManagedObject*>(catalog_go, "getComponent(System.Type)",
                                                        m_t_catalog_register);

        if (reg == nullptr) {
            return nullptr;
        }

        auto* ud = re4vr::call_safe<::REManagedObject*>(
            reg, "get_WeaponEquipParamCatalogUserData");

        if (ud == nullptr) {
            return nullptr;
        }

        auto def = utility::re_managed_object::get_type_definition(ud);

        if (def == nullptr) {
            return nullptr;
        }

        auto* f = def->get_field("_DataTable");

        if (f == nullptr) {
            return nullptr;
        }

        try {
            return f->get_data<::REManagedObject*>(ud);
        } catch (...) {
            return nullptr;
        }
    };

    auto* catalog = find_go("WeaponCatalog");

    if (catalog == nullptr) {
        catalog = find_go("WeaponCatalog_AO");
    }

    ::REManagedObject* tables2 = nullptr;

    if (catalog == nullptr) {
        catalog = find_go("WeaponCatalog_MC");
        tables2 = data_table_of(find_go("WeaponCatalog_MC_2nd"));
    }

    if (catalog == nullptr) {
        return;   // "kein catalog" -> naechster Frame
    }

    auto* tables = data_table_of(catalog);

    if (tables == nullptr) {
        return;   // "kein datatable"
    }

    for (auto* t : {tables, tables2}) {
        if (t == nullptr) {
            continue;
        }

        int32_t count = 0;
        re4vr::try_call<int32_t>(t, "get_Count", count);

        for (int32_t i = 0; i < count; ++i) {
            process_weapon_data(re4vr::call_safe<::REManagedObject*>(t, "get_Item", i));
        }
    }

    // Custom-Katalog: Laser-Sight-Attachment ReticleGuiType + PointRange.
    auto* custom = find_go("WeaponCustomCatalog");

    if (custom == nullptr) {
        custom = find_go("WeaponCustomCatalog_AO");
    }

    if (custom == nullptr) {
        custom = find_go("WeaponCustomCatalog_MC");
    }

    if (custom != nullptr && m_t_custom_catalog_register != nullptr) {
        auto* reg = re4vr::call_safe<::REManagedObject*>(custom, "getComponent(System.Type)",
                                                        m_t_custom_catalog_register);
        auto* ud = (reg != nullptr) ? re4vr::call_safe<::REManagedObject*>(
                                          reg, "get_WeaponDetailCustomUserdata")
                                    : nullptr;

        const auto field_obj = [](::REManagedObject* o,
                                  const char* name) -> ::REManagedObject* {
            if (o == nullptr) {
                return nullptr;
            }

            auto def = utility::re_managed_object::get_type_definition(o);

            if (def == nullptr) {
                return nullptr;
            }

            auto* f = def->get_field(name);

            if (f == nullptr) {
                return nullptr;
            }

            try {
                return f->get_data<::REManagedObject*>(o);
            } catch (...) {
                return nullptr;
            }
        };

        const auto field_i32 = [](::REManagedObject* o, const char* name,
                                  int32_t& out) -> bool {
            if (o == nullptr) {
                return false;
            }

            auto def = utility::re_managed_object::get_type_definition(o);

            if (def == nullptr) {
                return false;
            }

            auto* f = def->get_field(name);

            if (f == nullptr) {
                return false;
            }

            try {
                out = f->get_data<int32_t>(o);
                return true;
            } catch (...) {
                return false;
            }
        };

        auto* stages = field_obj(ud, "_WeaponDetailStages");

        if (stages != nullptr) {
            int32_t scount = 0;
            re4vr::try_call<int32_t>(stages, "get_Count", scount);

            for (int32_t i = 0; i < scount; ++i) {
                auto* wd = re4vr::call_safe<::REManagedObject*>(stages, "get_Item", i);
                auto* detail = field_obj(wd, "_WeaponDetailCustom");
                auto* attachments = field_obj(detail, "_AttachmentCustoms");

                if (attachments == nullptr) {
                    continue;
                }

                int32_t acount = 0;
                re4vr::try_call<int32_t>(attachments, "get_Count", acount);

                for (int32_t j = 0; j < acount; ++j) {
                    auto* item = re4vr::call_safe<::REManagedObject*>(attachments, "get_Item", j);
                    int32_t item_id = 0;

                    if (!field_i32(item, "_ItemID", item_id)) {
                        continue;
                    }

                    auto* params = field_obj(item, "_AttachmentParams");

                    if (params == nullptr) {
                        continue;
                    }

                    int32_t pcount = 0;
                    re4vr::try_call<int32_t>(params, "get_Count", pcount);

                    if (item_id == 116008000) {
                        for (int32_t k = 0; k < pcount; ++k) {
                            auto* ad =
                                re4vr::call_safe<::REManagedObject*>(params, "get_Item", k);
                            int32_t pname = 0;

                            if (field_i32(ad, "_AttachmentParamName", pname) && pname == 501) {
                                write_field_number(
                                    ad, utility::re_managed_object::get_type_definition(ad),
                                    "_ReticleGuiType", RETICLE_VALUE, false);
                            }
                        }
                    } else if (item_id == 116006400 || item_id == 116001600
                               || item_id == 116009600) {
                        for (int32_t k = 0; k < pcount; ++k) {
                            auto* ad =
                                re4vr::call_safe<::REManagedObject*>(params, "get_Item", k);
                            void* fit = nullptr;
                            sdk::RETypeDefinition* fit_td = nullptr;
                            bool fit_is_value = false;

                            if (field_target(ad,
                                             utility::re_managed_object::get_type_definition(ad),
                                             "_ReticleFitParam", fit, fit_td, false,
                                             fit_is_value)) {
                                apply_point_range(fit, fit_td, fit_is_value);
                            }
                        }
                    }
                }
            }
        }
    }

    m_reticle_params_applied = true;
}

// ============================================================================
// Phase: LockScene (Lua Z.464-515) -- der teure Pfad
// ============================================================================

void RE4VRCrosshair::on_pre_lock_scene() {
    auto* cm = re4vr::character_manager();
    auto* ctx = (cm != nullptr)
                    ? re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
                    : nullptr;

    if (ctx != nullptr) {
        bool shoot = false;
        bool reticle = false;
        bool changing = false;

        re4vr::try_call<bool>(ctx, "get_IsShootEnable", shoot);
        re4vr::try_call<bool>(ctx, "get_IsReticleDisp", reticle);
        re4vr::try_call<bool>(ctx, "get_IsWeaponChanging", changing);

        // ====================================================================
        // [RAILCAR: NICHT UEBERSCHREIBEN -- gemessen 07.09.2026]
        // re4_vr_minecart.lua setzt in der Lore is_aim/is_reticle_displayed
        // hart auf true. Alphabetisch lief crosshair VOR minecart (c < mi):
        // in Lua stand am Ende der LockScene-Phase also MINECARTS true, und
        // genau das lasen der Reticle-Draw und RE4VRMovement.
        //
        // Im Port steht RE4VRMinecart vor RE4VRCrosshair (es muss vor
        // RE4VRMotion und RE4VRArmChain stehen), also gewann hier der
        // false-Wert aus get_IsShootEnable -- in der Lore fehlte der
        // Reticle-Dot komplett, und Movement las denselben falschen Zustand.
        // Gemessen in reframework/data/re4_minecart_dot.txt: Waffe wp4005,
        // Arms, MuzzleJoint und der Gui_ui2040-Hook waren alle vorhanden,
        // einzig is_aim stand auf false.
        //
        // Deshalb schweigt der Schreiber hier, solange die Lore laeuft --
        // das Ergebnis ist fuer JEDEN Leser dasselbe wie im Original.
        // _IsWeaponChanging bleibt unberuehrt: minecart schreibt es nicht.
        // ====================================================================
        if (re4vr::lua_get_tribool("__re4_railcar_mode") != 1) {
            re4vr::lua_set_bool("is_aim", shoot);
            re4vr::lua_set_bool("is_reticle_displayed", reticle);
        }

        re4vr::lua_set_bool("_IsWeaponChanging", changing);

        // [DOT_ARM] Levelwechsel -- dieselbe Quelle wie der Killswitch.
        int32_t st = 0;

        if (re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", st)) {
            if (!m_dot_stage.has_value() || *m_dot_stage != st) {
                m_dot_stage = st;
                m_dot_armed = false;
            }
        }

        if (shoot) {
            m_dot_armed = true;
        }
    } else {
        re4vr::lua_set_bool("is_aim", false);
        re4vr::lua_set_bool("is_reticle_displayed", false);
        re4vr::lua_set_bool("_IsWeaponChanging", false);
    }

    // [DOT_ARM] Ladephase -- eigener Block, laeuft IMMER, holt den
    // CharacterManager ein ZWEITES Mal (Lua Z.491-501).
    // StageID allein reicht nicht: laedt man DASSELBE Level neu, bleibt sie
    // gleich. Waehrend der Ladephase ist der Player-BODY kurz nicht lesbar --
    // Szene, Manager und GuiManager bleiben dagegen bitgleich, und ein
    // Adressvergleich geht daneben, sobald die Engine eine Adresse
    // wiederverwendet.
    {
        auto* cm2 = re4vr::character_manager();
        auto* ctx2 = (cm2 != nullptr)
                         ? re4vr::call_safe<::REManagedObject*>(cm2, "getPlayerContextRef")
                         : nullptr;
        auto* bg = (ctx2 != nullptr)
                       ? re4vr::call_safe<::REManagedObject*>(ctx2, "get_BodyGameObject")
                       : nullptr;

        if (bg == nullptr) {
            m_dot_body_weg = true;
        } else if (m_dot_body_weg) {
            m_dot_body_weg = false;
            m_dot_armed = false;
        }
    }

    if (ks_active() && re4vr::lua_get_tribool("__re4_railcar_mode") != 1) {
        re4vr::lua_set_bool("is_aim", false);
        re4vr::lua_set_bool("is_reticle_displayed", false);
        re4vr::lua_set_bool("_IsWeaponChanging", false);
    }

    update_muzzle_data();
    apply_reticle_params();

    if (m_have_muzzle) {
        const glm::vec3 pos = m_last_muzzle_pos + (m_last_muzzle_forward * 0.05f);
        update_crosshair_world_pos(pos, pos + (m_last_muzzle_forward * 1000.0f));
    }
}

// ============================================================================
// Phase: LateUpdateBehavior -- Laser-Keepalive (Lua Z.1323-1331)
// ============================================================================

void RE4VRCrosshair::on_late_update_behavior() {
    // NUR in Mercenaries -- die Kampagne bleibt bitgleich.
    if (!re4vr::lua_get_bool("__re4_in_mercs", false)) {
        return;
    }

    if (m_laser_ctrl.obj == nullptr) {
        return;
    }

    // Leiche? Das GameObject ist der einzige Wert, der den Tod nicht ueberlebt.
    if (re4vr::call_safe<::REManagedObject*>(m_laser_ctrl.obj, "get_GameObject") == nullptr) {
        store(m_laser_ctrl, nullptr);
        re4vr::lua_set_nil("__re4_laser_ctrl");
        return;
    }

    laser_apply(m_laser_ctrl.obj);
}

void RE4VRCrosshair::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRCrosshair", "on_pre_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LockScene"_fnv) {
        on_pre_lock_scene();
    }
}

void RE4VRCrosshair::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRCrosshair", "on_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        on_late_update_behavior();
    }
}

// ============================================================================
// GUI-Draw (Lua Z.749-888)
// ============================================================================

void RE4VRCrosshair::write_vec4(::REManagedObject* obj, const glm::vec4& v, uint32_t offset) {
    if (obj == nullptr) {
        return;
    }

    auto* p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + offset);
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
    p[3] = v.w;
}

glm::quat RE4VRCrosshair::hud_quat_from_euler_deg(float dxg, float dyg, float dzg) {
    const auto axis_q = [](float ax, float ay, float az, float ang) {
        const float h = glm::radians(ang) * 0.5f;
        const float s = std::sin(h);
        return glm::quat{std::cos(h), ax * s, ay * s, az * s};
    };

    // Reihenfolge Y * X * Z -- verbindlich.
    return glm::normalize(axis_q(0, 1, 0, dyg) * axis_q(1, 0, 0, dxg) * axis_q(0, 0, 1, dzg));
}

// ============================================================================
// [HUD LEER 2026-09-07, Ansage des Users] Zeigt das Hand-HUD eine Waffe an, die
// gerade gar nicht in der Hand ist, stimmt das Bild nicht: fuer das MESSER
// (wid 5002) hat das Panel kein Symbol und behaelt das zuletzt gezeichnete;
// bei LEEREN HAENDEN schaltet das Spiel die Gruppe von selbst ab.
//
// Gemessen (re4_hud_mess.txt):
//   wid=-1  -> Gui_ui2030/2032_default/2180/2190 alle draw=false, en=false
//   Messer  -> wid=5002, messer=true, gun=false, alle vier weiter draw=true
//
// Deshalb hier derselbe Zustand von Hand: in beiden Faellen zeichnen wir die
// Gruppe nicht. Der Equip-Zustand selbst bleibt unangetastet -- ihn auf "leer"
// zu zwingen geht nicht, das Messer ist fuer das Spiel eine vollwertige Waffe
// (requestEquipBareHand prallt ab, solange es equippt ist -- gemessen).
// ============================================================================
bool RE4VRCrosshair::hud_state_empty() {
    // Zeitbasis wie in RE4VRHolster::clock_now -- eine eigene kleine Uhr,
    // damit hier keine fremde Datei eingebunden werden muss.
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (now - m_hud_empty_t < 0.1) {
        return m_hud_empty_val;
    }

    m_hud_empty_t = now;
    m_hud_empty_val = false;

    auto* cm = re4vr::character_manager();
    auto* ctx = (cm != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef") : nullptr;
    auto* hu = (ctx != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater") : nullptr;

    if (hu == nullptr) {
        return false;   // kein Zustand lesbar -> nichts ausblenden
    }

    bool gun = false, knife = false, gren = false, melee = false;
    const bool has_gun   = re4vr::try_call<bool>(hu, "get_IsEquipGun", gun);
    const bool has_knife = re4vr::try_call<bool>(hu, "get_IsEquipKnife", knife);
    const bool has_gren  = re4vr::try_call<bool>(hu, "get_IsEquipGrenade", gren);
    const bool has_melee = re4vr::try_call<bool>(hu, "get_IsEquipMelee", melee);

    if (!has_gun && !has_knife && !has_gren && !has_melee) {
        return false;   // Getter stumm -> im Zweifel nichts ausblenden
    }

    // Messer in der Hand ODER gar nichts in der Hand.
    m_hud_empty_val = knife || !(gun || knife || gren || melee);

    return m_hud_empty_val;
}

// ============================================================================
// [HUD INVENTAR 2026-09-25, Ansage des Users] Im Inventar (Koffer) flackerte
// die Hand-HUD-Gruppe bei Messer / leeren Haenden. Loesung: solange der Koffer
// offen ist, fassen wir die Gruppe GAR NICHT an -- kein Pin, kein Ausblenden.
// Nur der Koffer, keine anderen Menues. Erkennung wie RE4VRUi::is_inventory_open
// (AttacheCaseManager.get_IsAttacheCaseBusy), kurz gecacht.
// ============================================================================
bool RE4VRCrosshair::hud_inventory_open() {
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (now - m_hud_inv_t < 0.1) {
        return m_hud_inv_val;
    }

    m_hud_inv_t = now;
    m_hud_inv_val = false;

    auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.AttacheCaseManager");

    if (cm == nullptr) {
        return false;
    }

    bool busy = false;

    if (re4vr::try_call<bool>(cm, "get_IsAttacheCaseBusy", busy)) {
        m_hud_inv_val = busy;
    }

    return m_hud_inv_val;
}

void RE4VRCrosshair::apply_hand_hud(::REManagedObject* game_object) {
    const auto hand = re4vr::lua_get_vec3("__vr_rh_world");
    const auto hrot = re4vr::lua_get_quat("__vr_rh_rot");

    if (!hand.has_value() || !hrot.has_value()) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(game_object, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    // View hart auf Welt-Raum zwingen (wie das Reticle). Sonst rendert ein
    // uebrig gebliebener Screen-Space-Pass dasselbe Element mit unseren
    // Welt-Koordinaten als winziges Duplikat am Bildrand.
    auto* gui = (m_t_gui != nullptr) ? re4vr::get_component(game_object, "via.gui.GUI") : nullptr;
    auto* view = (gui != nullptr) ? re4vr::call_safe<::REManagedObject*>(gui, "get_View")
                                  : nullptr;

    if (view != nullptr) {
        re4vr::call_safe<void*>(view, "set_ViewType", static_cast<int32_t>(1));
        re4vr::call_safe<void*>(view, "set_Overlay", true);
    }

    // Native Basis-Laenge (Panel-Groesse) lesen, BEVOR wir die Basis
    // ueberschreiben.
    float L = 0.0833f;
    {
        auto* p = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(tf) + 0x80);
        const float n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);

        if (n > 1e-5f) {
            L = n;
        }
    }

    float rx = m_hud.rx;
    float ry = m_hud.ry;
    float rz = m_hud.rz;
    float dx = m_hud.dx;
    float dy = m_hud.dy;
    float dz = m_hud.dz;

    // [ADA-SET] Nur wenn ada_rot aktiv IST und der sticky Charakter-Getter
    // wirklich "ada" meldet -> Adas eigenes Set (Rotation UND Offset). Kein
    // eigener Body-Check: der sticky Getter ueberbrueckt die Frames, in denen
    // der Body kurz weg ist (sonst klappte die HUD-Gruppe fuer einen Frame
    // zurueck). Die Groesse bleibt bewusst gemeinsam.
    if (m_hud.ada_rot) {
        if (call_global_string("__re4_char_now") == "ada") {
            rx = m_hud.ada_rx;
            ry = m_hud.ada_ry;
            rz = m_hud.ada_rz;
            dx = m_hud.ada_dx;
            dy = m_hud.ada_dy;
            dz = m_hud.ada_dz;
        }
    }

    const glm::vec3 off = *hrot * glm::vec3{dx, dy, dz};
    const glm::quat q = glm::normalize(*hrot * hud_quat_from_euler_deg(rx, ry, rz));
    const glm::mat4 m = glm::mat4_cast(q);
    const float s = L * m_hud.scale;

    write_vec4(tf, m[0] * s, 0x80);
    write_vec4(tf, m[1] * s, 0x90);
    write_vec4(tf, m[2] * s, 0xA0);
    write_vec4(tf, glm::vec4{hand->x + off.x, hand->y + off.y, hand->z + off.z, 1.0f}, 0xB0);
}

bool RE4VRCrosshair::on_pre_gui_draw_element(REComponent* gui_element, void* primitive_context) {
    re4vr::trace("RE4VRCrosshair", "on_pre_gui_draw_element");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return true;
    }

    auto* element = reinterpret_cast<::REManagedObject*>(gui_element);
    auto* game_object = re4vr::call_safe<::REManagedObject*>(element, "get_GameObject");

    if (game_object == nullptr) {
        return true;
    }

    const std::string name = obj_name_of(game_object);

    if (name.empty()) {
        return true;
    }

    // [FINISHER_PROMPT] Gui_ui2200 = RT-Finisher-Prompt. Zeichnet jeden Frame
    // solange sichtbar -> Timestamp merken, das Frische-Fenster macht daraus
    // ein Boolean (kein Reset noetig, save/load-sicher).
    if (name == "Gui_ui2200") {
        if (auto l = ch_lua_state()) {
            (*l)["__re4_finisher_prompt_seen"] = now_clock();
        }
    }

    // [DODGE_PROMPT] 2191_3 = Xbox360-Symbole (bestaetigt).
    // [NICHT ERGAENZEN] "Gui_ui2150" stand hier als vermeintlicher PS-Satz --
    // widerlegt: es ist die DAMAGE-VIGNETTE. Folge des Fehleintrags war, dass
    // bei wenig Gesundheit der linke Grip "aus dem Nichts" zu B wurde. Der
    // Draw-Hook feuert auch fuer UNSICHTBARE GUIs. "Gui_ui2151" ebenfalls nicht
    // aufnehmen, ohne vorher zu BELEGEN, dass es der Ausweich-Prompt ist.
    if (name == "Gui_ui2191_3") {
        if (auto l = ch_lua_state()) {
            (*l)["__re4_dodge_prompt_seen"] = now_clock();
        }
    }

    // [CHOKE_UI] Solange wir jemanden im Wuergegriff halten: ALLE gerade
    // gezeichneten GUIs aus -- ausser der Hand-HUD-Gruppe, die an der Hand
    // klebt und dort nicht im Bild steht.
    if (re4vr::lua_call_global_bool("__re4_is_choking", false)
        && m_hud.guis.find(name) == m_hud.guis.end()) {
        return false;
    }

    if (name == RETICLE_HIDE) {
        return false;
    }

    // [HAND_HUDS]
    {
        const auto it = m_hud.guis.find(name);

        if (it != m_hud.guis.end() && it->second) {
            // [HUD INVENTAR] Koffer offen -> loslassen: das Spiel zeichnet
            // die Gruppe selbst, wir pinnen und verstecken nichts.
            if (hud_inventory_open()) {
                return true;
            }

            if (m_hud.hide_hud) {
                return false;
            }

            // [SCOPE] Beim Zielen durch ein montiertes Scope liegt die
            // Hand-HUD-Gruppe mitten im gezoomten Bild -> waehrenddessen aus.
            if (re4vr::lua_get_bool("vr_scope_active", false)) {
                return false;
            }

            // [HUD LEER] Messer oder leere Haende -> Gruppe aus, statt ein
            // falsches Symbol stehen zu lassen (s. hud_state_empty oben).
            if (hud_state_empty()) {
                return false;
            }

            if (m_hud.enabled) {
                apply_hand_hud(game_object);
            }

            return true;
        }
    }

    // [DOT_CROSSHAIR 26.09.2026 -- Ansage des Users] "Enable Dot Crosshair": der Mittel-Dot
    // Gui_ui2041 wird 1:1 wie das native Reticle behandelt (Raycast-Lage, Farbe, Groesse je Waffe,
    // Laser/Crosshair-aus/Zielen), das native Gui_ui2040 bleibt dann aus. Scope-Ausblendung
    // fuer 2041 macht RE4VRUi (wie fuer 2040).
    if (m_cfg.dot_crosshair && name == RETICLE_DOT) {
        return false;
    }

    if (name != (m_cfg.dot_crosshair ? RETICLE_CENTER_DOT : RETICLE_DOT)) {
        return true;
    }

    // [CROSSHAIR-TOGGLE] Komplett-Aus per UI-Checkbox.
    if (m_cfg.crosshair_off) {
        return false;
    }

    // [LASER-RETICLE] Laser-Aufsatz aktiv -> KEIN Reticle-Dot (nur der Laser).
    if (m_current_laser_active) {
        return false;
    }

    // [RUECKKANAL] is_aim aus _G lesen -- minecart schreibt es dazwischen.
    if (!re4vr::lua_get_bool("is_aim", false) || m_current_muzzle_joint.obj == nullptr
        || !m_crosshair_distance.has_value()) {
        return false;
    }

    auto* transform = re4vr::call_safe<::REManagedObject*>(game_object, "get_Transform");

    if (transform == nullptr) {
        return true;
    }

    auto* gui_comp = re4vr::get_component(game_object, "via.gui.GUI");
    auto* view = (gui_comp != nullptr)
                     ? re4vr::call_safe<::REManagedObject*>(gui_comp, "get_View")
                     : nullptr;

    if (view == nullptr) {
        return true;
    }

    re4vr::call_safe<void*>(view, "set_ViewType", static_cast<int32_t>(1));   // Welt-Raum
    re4vr::call_safe<void*>(view, "set_Overlay", true);
    re4vr::call_safe<void*>(view, "set_Detonemap", true);
    // [SICHTBARKEIT] DepthTest AUS: mit Depth-Test kaempfte der auf der
    // Trefferflaeche liegende Dot je nach Winkel gegen die Szenen-Tiefe ->
    // Z-Fighting -> mal sichtbar, mal komplett weg.
    re4vr::call_safe<void*>(view, "set_DepthTest", false);

    // [RETICLE-COLOR] ColorScale des Panels 'main'. force aus -> 1/1/1 (weiss).
    {
        auto* root = re4vr::call_safe<::REManagedObject*>(view, "get_Child");

        if (root != nullptr) {
            const float r = m_cfg.reticle_color ? m_cfg.reticle_r : 1.0f;
            const float g = m_cfg.reticle_color ? m_cfg.reticle_g : 1.0f;
            const float b = m_cfg.reticle_color ? m_cfg.reticle_b : 1.0f;

            if (auto* m = find_method(root, "set_ColorScale")) {
                auto context = sdk::get_thread_context();
                __declspec(align(16)) glm::vec4 col{r, g, b, 1.0f};

                try {
                    m->call_safe<void*>(context, root, &col);
                } catch (...) {
                }

                clear_pending(context, true);
            }

            // [DOT_FADE 26.09.2026] Belegt (zzz_re4_dotcrosshair_sonde): beim Zielen spielt das Spiel
            // auf "main" EXIT -> "c_color" blendet in ~70 ms auf Alpha 0 und wird unsichtbar (HIDE),
            // beim Loslassen ENTER. Im Dot-Modus ist der Mittel-Dot aber genau DANN das Fadenkreuz ->
            // jedes Kind von "main" jeden Frame wieder sichtbar und voll deckend.
            if (m_cfg.dot_crosshair) {
                int32_t guard = 0;

                for (auto* ch = re4vr::call_safe<::REManagedObject*>(root, "get_Child");
                     ch != nullptr && guard < 16;
                     ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next"), ++guard) {
                    re4vr::call_safe<void*>(ch, "set_Visible", true);

                    if (auto* mc = find_method(ch, "set_ColorScale")) {
                        auto context = sdk::get_thread_context();
                        __declspec(align(16)) glm::vec4 one{1.0f, 1.0f, 1.0f, 1.0f};

                        try {
                            mc->call_safe<void*>(context, ch, &one);
                        } catch (...) {
                        }

                        clear_pending(context, true);
                    }
                }
            }
        }
    }

    // Der VR-Mod hat eine EIGENE Crosshair-Behandlung -- ohne unhide kaempft
    // sie mit unserem Write.
    VR::get()->unhide_crosshair();

    // Position FRISCH vom Muzzle-Joint (gleiche Quelle/Moment wie die
    // Laser-Line) -- der LockScene-Stand ist 1 Frame alt und zappelt relativ
    // zum Laser. Nur die Distanz kommt vom (async) Raycast.
    const float distance = *m_crosshair_distance;
    glm::vec3 mp{};
    glm::vec3 fwd{};
    glm::vec3 dir{};
    glm::vec3 base{};

    if (get_vec3(m_current_muzzle_joint.obj, "get_Position", mp)
        && get_vec3(m_current_muzzle_joint.obj, "get_AxisZ", fwd)) {
        dir = glm::normalize(fwd);
        base = mp;
    } else {
        dir = m_crosshair_dir;
        base = m_crosshair_pos - (dir * distance);
    }

    float scale_distance = distance * 0.075f;

    if (scale_distance < MIN_SCALE) {
        scale_distance = MIN_SCALE;
    } else if (scale_distance > MAX_SCALE) {
        scale_distance = MAX_SCALE;
    }

    // Per-Waffe Groessen-Multiplikator (1.0 = unveraendert).
    // [DOT_CROSSHAIR] Der Mittel-Dot hat EINE Groesse fuer alle Waffen.
    if (m_cfg.dot_crosshair) {
        scale_distance *= m_cfg.dot_size;
    } else if (m_current_weapon_id.has_value()) {
        const auto it = m_cfg.reticle_scale.find(std::to_string(*m_current_weapon_id));

        if (it != m_cfg.reticle_scale.end()) {
            scale_distance *= it->second;
        }
    }

    const glm::mat4 new_mat = glm::mat4_cast(dir_to_quat(dir));
    const glm::vec3 adjusted_pos = base + (dir * (distance - 0.05f));

    write_vec4(transform, new_mat[0] * scale_distance, 0x80);
    write_vec4(transform, new_mat[1] * scale_distance, 0x90);
    write_vec4(transform, new_mat[2] * scale_distance, 0xA0);
    write_vec4(transform, glm::vec4{adjusted_pos.x, adjusted_pos.y, adjusted_pos.z, 1.0f}, 0xB0);

    return true;
}

// ============================================================================
// Bullet-Hooks (Lua Z.891-1010)
// ============================================================================

void RE4VRCrosshair::hook_pre_request_fire(std::vector<uintptr_t>& args) {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    // [LUIS-FIX] NPC-Schuss (Luis' Red9 -> gleiche BulletShellGenerator-Klasse)
    // NICHT als Spieler-Schuss werten: sonst zaehlt __vr_shot_seq hoch (Haptik/
    // Burst bei SEINEM Schuss) UND seine Kugel wird an DEINEN Muzzle umgelenkt.
    // Discriminator = der besitzende CHARACTER.
    if (args.size() > 2) {
        auto* gen = reinterpret_cast<::REManagedObject*>(args[1]);

        if (gen != nullptr) {
            auto* go = re4vr::call_safe<::REManagedObject*>(gen, "get_GameObject");
            auto* tf = (go != nullptr)
                           ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
                           : nullptr;
            bool is_npc = false;
            int guard = 0;

            while (tf != nullptr && guard < 16) {
                ++guard;

                auto* tgo = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
                const std::string s = (tgo != nullptr) ? obj_name_of(tgo) : std::string{};

                if (s.size() >= 2 && s[0] == 'c' && s[1] == 'h') {
                    is_npc = (s.size() < 5) || (PLAYER_CH_PREFIX.count(s.substr(0, 5)) == 0);
                    break;   // erster Character-Ancestor entscheidet
                }

                tf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
            }

            if (is_npc) {
                return;
            }
        }
    }

    // [BURST] Schuss-Zaehler -- auch wenn der Bullet-Hook aus ist.
    re4vr::lua_set_number("__vr_shot_seq",
                          re4vr::lua_get_number("__vr_shot_seq", 0.0) + 1.0);

    if (!m_cfg.bullet_hook) {
        return;
    }

    // [M5] args_impl = 4 + num_params (HookManager.hpp:118). Gebraucht wird
    // args[3], also muessen mindestens 6 Eintraege dastehen.
    if (args.size() < 6) {
        return;
    }

    const auto set_vec3 = [&](uintptr_t addr, const glm::vec3& v) {
        if (m_set_item_vec3 == nullptr) {
            return;
        }

        auto context = sdk::get_thread_context();
        auto* p = reinterpret_cast<void*>(addr);

        for (int32_t i = 0; i < 3; ++i) {
            try {
                m_set_item_vec3->call_safe<void*>(context, p, i, v[i]);
            } catch (...) {
            }
        }

        clear_pending(context, true);
    };

    const auto set_quat = [&](uintptr_t addr, const glm::quat& q) {
        if (m_set_item_quat == nullptr) {
            return;
        }

        auto context = sdk::get_thread_context();
        auto* p = reinterpret_cast<void*>(addr);
        const float v[4] = {q.x, q.y, q.z, q.w};

        for (int32_t i = 0; i < 4; ++i) {
            try {
                m_set_item_quat->call_safe<void*>(context, p, i, v[i]);
            } catch (...) {
            }
        }

        clear_pending(context, true);
    };

    // Scope-Zweig.
    const auto sp = re4vr::lua_get_vec3("vr_scope_aim_pos");
    const auto sd = re4vr::lua_get_vec3("vr_scope_aim_dir");

    if (re4vr::lua_get_bool("vr_scope_active", false) && sp.has_value() && sd.has_value()) {
        set_vec3(args[2], *sp);
        set_quat(args[3], dir_to_quat(glm::normalize(*sd)));
        return;
    }

    if (ks_active() && re4vr::lua_get_tribool("__re4_railcar_mode") != 1) {
        return;
    }

    glm::vec3 muzzle_pos = m_last_muzzle_pos;
    glm::vec3 muzzle_fwd = m_last_muzzle_forward;
    bool have = m_have_muzzle;

    // [LEAD-FIX] IMMER: den Muzzle-Joint LIVE im Feuer-Moment lesen statt aus
    // dem Frame-Cache. So sind Ursprung + Richtung so frisch wie der Feuer-Tick
    // -> der latenzbedingte Vorhalte-Lead schrumpft.
    if (m_current_muzzle_joint.obj != nullptr) {
        glm::vec3 lp{};
        glm::vec3 lf{};

        if (get_vec3(m_current_muzzle_joint.obj, "get_Position", lp)
            && get_vec3(m_current_muzzle_joint.obj, "get_AxisZ", lf)) {
            muzzle_pos = lp;
            muzzle_fwd = lf;
            have = true;
        }
    }

    if (!have) {
        return;
    }

    set_vec3(args[2], muzzle_pos);
    set_quat(args[3], dir_to_quat(glm::normalize(muzzle_fwd)));
}

void RE4VRCrosshair::hook_pre_rocket_generate(std::vector<uintptr_t>& args) {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    // Reihenfolge ist eine ANDERE als beim Kugel-Hook, und es gibt hier
    // KEINEN Live-Joint-Read (kein LEAD-FIX).
    if (!m_cfg.bullet_hook) {
        return;
    }

    if (ks_active() && re4vr::lua_get_tribool("__re4_railcar_mode") != 1) {
        return;
    }

    if (!m_have_muzzle) {
        return;
    }

    // [M5] Gebraucht wird args[5] -> mindestens 8 Eintraege.
    if (args.size() < 8) {
        return;
    }

    auto* owner = reinterpret_cast<::REManagedObject*>(args[5]);

    if (owner == nullptr) {
        return;
    }

    auto def = utility::re_managed_object::get_type_definition(owner);

    if (def == nullptr || def->get_full_name() != std::string{game_namespace("Gun")}) {
        return;
    }

    if (m_set_item_vec3 != nullptr) {
        auto context = sdk::get_thread_context();
        auto* p = reinterpret_cast<void*>(args[2]);

        for (int32_t i = 0; i < 3; ++i) {
            try {
                m_set_item_vec3->call_safe<void*>(context, p, i, m_last_muzzle_pos[i]);
            } catch (...) {
            }
        }

        clear_pending(context, true);
    }

    if (m_set_item_quat != nullptr) {
        const glm::quat rot = dir_to_quat(glm::normalize(m_last_muzzle_forward));
        const float v[4] = {rot.x, rot.y, rot.z, rot.w};
        auto context = sdk::get_thread_context();
        auto* p = reinterpret_cast<void*>(args[3]);

        for (int32_t i = 0; i < 4; ++i) {
            try {
                m_set_item_quat->call_safe<void*>(context, p, i, v[i]);
            } catch (...) {
            }
        }

        clear_pending(context, true);
    }
}

// ============================================================================
// Concentrate (Lua Z.104-147)
// ============================================================================

void RE4VRCrosshair::hook_pre_concentrate_ratio(std::vector<uintptr_t>& args) {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    if (args.size() < 3) {
        return;
    }

    if (re4vr::lua_get_bool("__re4_force_concentrate", false)) {
        // sdk.to_ptr(bits) -- die Float-BITS als Zeigerwert. Absicht.
        const auto bits = static_cast<uint32_t>(
            re4vr::lua_get_number("__re4_concentrate_bits", 0x3F800000));
        args[2] = static_cast<uintptr_t>(bits);
    }
}

void RE4VRCrosshair::hook_pre_apply_concentrate(std::vector<uintptr_t>& args) {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    if (args.size() < 3) {
        return;
    }

    if (re4vr::lua_get_bool("__re4_force_concentrate", false)) {
        args[2] = 1;   // true erzwingen -> Reticle bleibt klein
    }
}

// ============================================================================
// Laser (Lua Z.1159-1345)
// ============================================================================

uint32_t RE4VRCrosshair::laser_rgba(float r, float g, float b, float a) {
    const auto q = [](float v) {
        return static_cast<uint32_t>(std::floor(v * 255.0f + 0.5f));
    };

    // via.Color = ABGR
    return q(r) + q(g) * 256u + q(b) * 65536u + q(a) * 16777216u;
}

void RE4VRCrosshair::hook_pre_update_laser(std::vector<uintptr_t>& args) {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    // Lua Z.1180-1186: laser_this wird JEDES Mal genullt, __re4_laser_ctrl
    // behaelt dagegen den letzten GUELTIGEN Controller -- der Nachzieh-Tick
    // braucht ihn auch dann, wenn die Engine updateLaser gerade nicht ruft.
    // Deshalb zwei getrennte Handles.
    store(m_laser_this, nullptr);

    if (args.size() < 2) {
        return;
    }

    auto* self = reinterpret_cast<::REManagedObject*>(args[1]);

    if (self != nullptr) {
        store(m_laser_this, self);
        store(m_laser_ctrl, self);
        re4vr::lua_set_managed_object("__re4_laser_ctrl", self);
    }
}

void RE4VRCrosshair::hook_post_update_laser() {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return;
    }
    laser_apply(m_laser_this.obj);
}

void RE4VRCrosshair::laser_apply(::REManagedObject* self) {
    if (self == nullptr || !m_laser.enabled) {
        return;
    }

    re4vr::lua_set_number("__re4_laser_last", now_clock());

    auto def = utility::re_managed_object::get_type_definition(self);

    if (def == nullptr) {
        return;
    }

    {
        auto* f = def->get_field("_IsDraw");

        if (f == nullptr) {
            return;
        }

        bool is_draw = false;

        try {
            is_draw = f->get_data<bool>(self);
        } catch (...) {
            return;
        }

        if (!is_draw) {
            return;
        }
    }

    ::REManagedObject* emit = nullptr;

    if (auto* ef = def->get_field("SightEmitJoint")) {
        try {
            emit = ef->get_data<::REManagedObject*>(self);
        } catch (...) {
            emit = nullptr;
        }
    }

    if (emit == nullptr) {
        return;
    }

    glm::vec3 ep{};
    glm::quat er{};
    glm::vec3 ez{};
    const bool have_ep = get_vec3(emit, "get_Position", ep);
    const bool have_er = get_quat(emit, "get_Rotation", er);
    const bool have_ez = get_vec3(emit, "get_AxisZ", ez);

    // [SMOKE] organische Zeit-Modulation (Dicke + Laenge + Alpha).
    float sm_w = 1.0f;
    float sm_l = 1.0f;
    float sm_alpha = 1.0f;

    if (m_laser.smoke) {
        const double t = now_clock() * m_laser.smoke_speed;
        const float n = static_cast<float>(std::sin(t) * 0.5 + std::sin(t * 2.3 + 1.7) * 0.3
                                           + std::sin(t * 5.1 + 3.0) * 0.2);
        sm_w = 1.0f + m_laser.smoke_amt * n;
        sm_l = 1.0f + m_laser.smoke_amt * 0.5f * static_cast<float>(std::sin(t * 2.3 + 1.7));
        sm_alpha = 1.0f - m_laser.smoke_amt * 0.5f * (0.5f + 0.5f * n);
    }

    // [DOT_ARM] Bis zum ersten Aim im Level: STRAHL UND PUNKT gar nicht
    // zeichnen -- vorher projiziert die Engine beides entlang der Kamera und
    // der Punkt klebt an der Waffe.
    {
        bool want = m_dot_armed;

        // [MERCS: NUR BEIM AIM] Dort fuehrt die Engine den Lasersight
        // ausschliesslich im Zielzustand. Statt dagegen anzuschreiben werden
        // Strahl UND Punkt hier nur beim Aim gezeichnet -- damit gibt es weder
        // einen eingefrorenen Punkt noch das Gelb/Rot-Flackern. Die Kampagne
        // bleibt unberuehrt.
        if (re4vr::lua_get_bool("__re4_in_mercs", false)) {
            // Fallback greift NUR bei nil, nicht bei false.
            const int a = re4vr::lua_get_tribool("__vr_aim_input");
            const bool aim = (a >= 0) ? (a == 1) : re4vr::lua_get_bool("is_aim", false);
            want = want && aim;
        }

        for (const char* getter : {"get_Line", "get_Light"}) {
            auto* part = re4vr::call_safe<::REManagedObject*>(self, getter);

            if (part == nullptr) {
                continue;
            }

            auto* g2 = re4vr::call_safe<::REManagedObject*>(part, "get_GameObject");

            if (g2 != nullptr) {
                re4vr::call_safe<void*>(g2, "set_DrawSelf", want);
            }
        }

        // [KEIN FRUEHAUSSTIEG] Frueher stand hier ein Ausstieg bei !want. Das
        // war der Fehler hinter "Laser und Punkt fliegen ohne Aim in der Gegend
        // rum": wir haben dann NUR versteckt und die Positionierung
        // uebersprungen -- schaltet die Engine das GO im selben Frame wieder
        // sichtbar, steht beides auf seiner nativen Projektion entlang der
        // KAMERA statt an der Muendung.
    }

    // (1) STRAHL an Emit-Joint + Dicke/Laenge (smoke-moduliert).
    {
        auto* line = re4vr::call_safe<::REManagedObject*>(self, "get_Line");
        auto* tf = (line != nullptr)
                       ? re4vr::call_safe<::REManagedObject*>(line, "get_Transform")
                       : nullptr;

        if (tf != nullptr) {
            if (have_ep) {
                call_vec3(tf, "set_Position", ep);
            }

            if (have_er) {
                call_quat(tf, "set_Rotation", er);
            }

            const float w = m_laser.width * sm_w;
            call_vec3(tf, "set_LocalScale", glm::vec3{w, w, m_laser.length * sm_l});
        }
    }

    // (2) FARBE (optional): ApplyColor(0xD0) + IsChangeColor(0xD4).
    if (m_laser.force_color) {
        auto* base = reinterpret_cast<uint8_t*>(self);
        *reinterpret_cast<uint32_t*>(base + 0xD0) =
            laser_rgba(m_laser.r, m_laser.g, m_laser.b, 1.0f);
        *reinterpret_cast<uint8_t*>(base + 0xD4) = 1;
    }

    // (2b) TRANSPARENZ (AlphaRate) + GLOW (EmissiveIntensity) -- FLOAT-Params.
    if (m_laser.tune_glow || m_laser.smoke) {
        const float a = (m_laser.tune_glow ? m_laser.alpha : 1.0f) * sm_alpha;
        auto* mp = re4vr::call_safe<::REManagedObject*>(self,
                                                       "get_PlayerLineAlphaMaterialParam");

        if (mp != nullptr) {
            re4vr::call_safe<void*>(mp, "set_ValueF", a);
        }

        if (m_laser.tune_glow) {
            auto* me = re4vr::call_safe<::REManagedObject*>(
                self, "get_PlayerLineEmissiveMaterialParam");

            if (me != nullptr) {
                re4vr::call_safe<void*>(me, "set_ValueF", m_laser.glow);
            }
        }
    }

    // (3) DOT: 'Light'-GO ans Strahl-Ende entlang der stabilen Emit-Achse.
    if (have_ep && have_ez) {
        float dist = m_laser.dot_dist;

        if (m_laser.dot_raycast && m_crosshair_distance.has_value()) {
            dist = *m_crosshair_distance;
        }

        const glm::vec3 endp = ep + (ez * dist);
        auto* light = re4vr::call_safe<::REManagedObject*>(self, "get_Light");

        if (light != nullptr) {
            auto* ltf = re4vr::call_safe<::REManagedObject*>(light, "get_Transform");
            auto* lgo = re4vr::call_safe<::REManagedObject*>(light, "get_GameObject");

            if (lgo != nullptr) {
                // HART true -- das hebt das `want` der Schleife oben fuer das
                // Light-GO wieder auf. Netto: der Dot ist immer sichtbar,
                // `want` wirkt effektiv nur auf die Line. 1:1 uebernommen.
                re4vr::call_safe<void*>(lgo, "set_DrawSelf", true);
            }

            if (ltf != nullptr) {
                call_vec3(ltf, "set_Position", endp);
                call_vec3(ltf, "set_LocalScale",
                          glm::vec3{m_laser.dot_size, m_laser.dot_size, m_laser.dot_size});
            }
        }
    }
}

// ============================================================================
// UI
// ============================================================================

void RE4VRCrosshair::ensure_public_ui_registered() {
    // [TRAEGE ANMELDUNG -- Pflicht] ScriptRunner::reset_scripts() ruft
    // on_lua_state_created (Z.1425), BEVOR es die Autorun-Scripte laedt
    // (ab Z.1438/1452). ##re4_vr_menu.lua setzt bei seinem Start
    // _G.__re4_ui_entries = {} -- eine dort schon eingetragene Anmeldung waere
    // also sofort wieder weg. Deshalb pro Frame nachsehen.
    if (m_public_ui_registered) {
        return;
    }

    auto lua = ch_lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object add = (*lua)["__re4_ui_add"];

    if (!add.valid() || add.get_type() != sol::type::function) {
        m_dispatcher_present = false;
        return;   // ohne Dispatcher zeichnet unser eigenes on_draw_ui
    }

    m_dispatcher_present = true;

    sol::object entries = (*lua)["__re4_ui_entries"];

    if (!entries.valid() || entries.get_type() != sol::type::table) {
        return;
    }

    if (entries.as<sol::table>()["crosshair_off"].valid()) {
        m_public_ui_registered = true;
        return;
    }

    try {
        auto fn = add.as<sol::protected_function>();
        fn(30, "crosshair_off", (*lua)["__re4_crosshair_draw_public_off"]);
        fn(40, "laser_color", (*lua)["__re4_crosshair_draw_public_laser_color"]);
        fn(45, "reticle_color", (*lua)["__re4_crosshair_draw_public_reticle_color"]);
        fn(46, "reticle_size", (*lua)["__re4_crosshair_draw_public_reticle_size"]);
        m_public_ui_registered = true;
    } catch (...) {
    }
}

void RE4VRCrosshair::on_frame() {
    re4vr::trace("RE4VRCrosshair", "on_frame");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    ensure_public_ui_registered();
}

void RE4VRCrosshair::draw_public_crosshair_off() {
    // [UEBERSCHRIFT 11.09.2026] "Crosshair" (war "Select Crosshair Color" in
    // draw_public_reticle_color) steht jetzt HIER, damit "Disable Crosshair"
    // unter ihr und noch ueber der Farbauswahl liegt.
    g_framework->draw_menu_heading("Crosshair", true);

    bool v = m_cfg.crosshair_off;

    // [ZEILENABSTAND 11.09.2026] Dezent mehr Luft zur Farbauswahl darunter --
    // dieselben 8 px wie zwischen den Schaltern in "Miscellaneous".
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, g_framework->menu_px(8.0f)));

    // [SIZE_OBEN 26.09.2026 -- Ansage des Users] "Crosshair Size" (je Waffe, nur ohne Dot) steht
    // ueber "Enable Dot Crosshair" -- vorher unter der Farbauswahl (RE4VRMenu::draw_public).
    draw_public_reticle_size();

    // [DOT_CROSSHAIR 26.09.2026 -- Ansage des Users] ueber "Disable Crosshair".
    bool dot = m_cfg.dot_crosshair;

    if (g_framework->draw_menu_checkbox("Enable Dot Crosshair", &dot)) {
        m_cfg.dot_crosshair = dot;
        save_cfg();
    }

    // [DOT_CROSSHAIR] Eigene Groesse nur mit Dot; die Groesse je Waffe erscheint nur ohne Dot.
    if (m_cfg.dot_crosshair) {
        if (ImGui::SliderFloat("Dot Crosshair Size", &m_cfg.dot_size, 0.25f, 4.0f, "%.2f")) {
            save_cfg();
        }
    }

    if (g_framework->draw_menu_checkbox("Disable Crosshair", &v)) {
        m_cfg.crosshair_off = v;
        save_cfg();
    }

    ImGui::PopStyleVar();
}

void RE4VRCrosshair::draw_public_laser_color() {
    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt den aktiven Preset, die eingefaerbte Schrift entfaellt.
    const auto preset_radio = [&](const char* label, float r, float g, float b) {
        const bool active = m_laser.force_color && std::fabs(m_laser.r - r) < 0.01f
                            && std::fabs(m_laser.g - g) < 0.01f
                            && std::fabs(m_laser.b - b) < 0.01f;

        // [FARBRAHMEN 11.09.2026] Rahmen des Kaestchens in der Farbe des Presets.
        if (g_framework->draw_menu_radio(label, active, ImVec4{r, g, b, 1.0f})) {
            m_laser.force_color = true;
            m_laser.r = r;
            m_laser.g = g;
            m_laser.b = b;
            save_laser_cfg();
        }
    };

    g_framework->draw_menu_heading("Select Laser Color", true);   // [UEBERSCHRIFT 11.09.2026] war orangerot, linksbuendig
    // [LINKSBUENDIG 11.09.2026] Reihe linksbuendig (war mittig), MENU_BUTTON_GAP
    // zwischen den Kaestchen.
    const auto button_gap = g_framework->menu_px(REFramework::MENU_BUTTON_GAP);

    preset_radio("Red", 1.0f, 0.0f, 0.0f);
    ImGui::SameLine(0.0f, button_gap);
    preset_radio("Green", 0.0f, 1.0f, 0.0f);
    ImGui::SameLine(0.0f, button_gap);
    preset_radio("Blue", 0.0f, 0.0f, 1.0f);
    ImGui::SameLine(0.0f, button_gap);
    preset_radio("Yellow", 1.0f, 1.0f, 0.0f);
}

void RE4VRCrosshair::draw_public_reticle_color() {
    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt den aktiven Preset, die eingefaerbte Schrift entfaellt.
    const auto preset_radio = [&](const char* label, float r, float g, float b) {
        const bool active = m_cfg.reticle_color && std::fabs(m_cfg.reticle_r - r) < 0.01f
                            && std::fabs(m_cfg.reticle_g - g) < 0.01f
                            && std::fabs(m_cfg.reticle_b - b) < 0.01f;

        // [FARBRAHMEN 11.09.2026] Rahmen des Kaestchens in der Farbe des Presets.
        if (g_framework->draw_menu_radio(label, active, ImVec4{r, g, b, 1.0f})) {
            m_cfg.reticle_color = true;
            m_cfg.reticle_r = r;
            m_cfg.reticle_g = g;
            m_cfg.reticle_b = b;
            save_cfg();
        }
    };

    // [UEBERSCHRIFT 11.09.2026] Die Ueberschrift ("Crosshair") zeichnet
    // draw_public_crosshair_off, darunter steht "Disable Crosshair".

    // [CROSSHAIR COLOR 11.09.2026] Kleiner Abstand, dann die Beschriftung
    // linksbuendig und die Farben UNTEREINANDER -- mit denselben 8 px Luft wie
    // die Schalter in "Miscellaneous".
    ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(6.0f)));
    ImGui::TextUnformatted("Crosshair Color");

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, g_framework->menu_px(8.0f)));

    // [WHITE] Default oben: Weiss ist KEINE erzwungene Farbe, sondern
    // reticle_color = false.
    if (g_framework->draw_menu_radio("White##ret", !m_cfg.reticle_color)) {
        m_cfg.reticle_color = false;
        save_cfg();
    }

    preset_radio("Red##ret", 1.0f, 0.0f, 0.0f);
    preset_radio("Green##ret", 0.0f, 1.0f, 0.0f);
    preset_radio("Blue##ret", 0.0f, 0.0f, 1.0f);
    preset_radio("Yellow##ret", 1.0f, 1.0f, 0.0f);

    ImGui::PopStyleVar();
}

void RE4VRCrosshair::draw_public_reticle_size() {
    // [DOT_CROSSHAIR] Mit Dot gilt "Dot Crosshair Size" (unter dem Dot-Schalter).
    if (m_cfg.dot_crosshair || !m_current_weapon_id.has_value()) {
        return;
    }

    const auto key = std::to_string(*m_current_weapon_id);
    const auto it = m_cfg.reticle_scale.find(key);
    float cur = (it != m_cfg.reticle_scale.end()) ? it->second : 1.0f;

    if (ImGui::SliderFloat("Crosshair Size", &cur, 0.25f, 4.0f, "%.2f")) {
        m_cfg.reticle_scale[key] = cur;
        save_cfg();
    }
}

void RE4VRCrosshair::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    // In Lua sind das vier getrennte on_draw_ui, die sich ueber __ch_tree_open
    // verketten. Nativ ist es EIN Callback mit denselben verschachtelten Trees.
    if (!ImGui::TreeNode("RE4VR - Crosshair")) {
        return;
    }

    bool v = false;

    v = m_cfg.crosshair_off;

    if (ImGui::Checkbox("Crosshair komplett aus", &v)) {
        m_cfg.crosshair_off = v;
        save_cfg();
    }

    v = m_cfg.bullet_hook;

    if (ImGui::Checkbox("Bullet-Hook (Kugeln folgen VR-Muzzle)", &v)) {
        m_cfg.bullet_hook = v;
        save_cfg();
    }

    v = m_cfg.force_reticle_concentrate;

    if (ImGui::Checkbox("Reticle dauerhaft zusammengezogen", &v)) {
        m_cfg.force_reticle_concentrate = v;
        re4vr::lua_set_bool("__re4_force_concentrate", v);
        save_cfg();
    }

    if (m_cfg.force_reticle_concentrate) {
        float r = m_cfg.concentrate_ratio;

        if (ImGui::SliderFloat("Concentrate-Wert (1=eng, 0=weit)", &r, 0.0f, 1.0f, "%.2f")) {
            m_cfg.concentrate_ratio = r;
            re4vr::lua_set_number("__re4_concentrate_bits",
                                  static_cast<double>(float_bits(r)));
            re4vr::lua_set_number("__re4_concentrate_val", r);
            save_cfg();
        }
    }

    // Reticle-Groesse pro Waffe (editiert die aktuell equippte Waffe).
    ImGui::Separator();

    if (m_current_weapon_id.has_value()) {
        const auto key = std::to_string(*m_current_weapon_id);
        const auto it = m_cfg.reticle_scale.find(key);
        float cur = (it != m_cfg.reticle_scale.end()) ? it->second : 1.0f;
        const auto label = std::string{"Reticle-Groesse (wp"} + key + ")";

        if (ImGui::SliderFloat(label.c_str(), &cur, 0.25f, 4.0f, "%.2f")) {
            m_cfg.reticle_scale[key] = cur;
            save_cfg();
        }

        if (ImGui::Button("Reset Reticle-Groesse (diese Waffe)")) {
            m_cfg.reticle_scale.erase(key);
            save_cfg();
        }
    } else {
        ImGui::Text("Reticle-Groesse: keine Waffe equippt");
    }

    ImGui::Separator();
    ImGui::Text("Farbe");

    v = m_cfg.reticle_color;

    if (ImGui::Checkbox("Farbe erzwingen (sonst weiss)", &v)) {
        m_cfg.reticle_color = v;
        save_cfg();
    }

    if (m_cfg.reticle_color) {
        float col[3] = {m_cfg.reticle_r, m_cfg.reticle_g, m_cfg.reticle_b};

        if (ImGui::ColorEdit3("Crosshair-Farbe", col)) {
            m_cfg.reticle_r = col[0];
            m_cfg.reticle_g = col[1];
            m_cfg.reticle_b = col[2];
            save_cfg();
        }
    }

    if (ImGui::Button("Gelb##retdev")) {
        m_cfg.reticle_color = true;
        m_cfg.reticle_r = 1.0f;
        m_cfg.reticle_g = 1.0f;
        m_cfg.reticle_b = 0.0f;
        save_cfg();
    }

    // ---- Hand-HUDs ----
    if (ImGui::TreeNode("RE4 VR \xE2\x80\x94 Hand-HUDs")) {
        v = m_hud.enabled;

        if (ImGui::Checkbox("Aktiv (GUI-Gruppe folgt rechter Hand)", &v)) {
            m_hud.enabled = v;
            save_hud_cfg();
        }

        v = m_hud.hide_hud;

        if (ImGui::Checkbox("HUD ausblenden (Ziel-GUIs komplett aus)", &v)) {
            m_hud.hide_hud = v;
            save_hud_cfg();
        }

        ImGui::Separator();
        ImGui::Text("Gemeinsame Position + Groesse (Verhaeltnis bleibt erhalten):");

        const auto slider = [&](const char* label, float& dst, float lo, float hi,
                                const char* fmt) {
            float t = dst;

            if (ImGui::SliderFloat(label, &t, lo, hi, fmt)) {
                dst = t;
                save_hud_cfg();
            }
        };

        slider("Rechts (dx)", m_hud.dx, -0.5f, 0.5f, "%.3f");
        slider("Hoch (dy)", m_hud.dy, -0.5f, 0.5f, "%.3f");
        slider("Vor/Zurueck (dz)", m_hud.dz, -0.5f, 0.5f, "%.3f");
        slider("Groesse (scale, 1=nativ)", m_hud.scale, 0.02f, 1.0f, "%.3f");
        slider("Rot X (Pitch)", m_hud.rx, -180.0f, 180.0f, "%.0f");
        slider("Rot Y (Yaw)", m_hud.ry, -180.0f, 180.0f, "%.0f");
        slider("Rot Z (Roll)", m_hud.rz, -180.0f, 180.0f, "%.0f");

        ImGui::Separator();
        ImGui::Text("Ada: eigene Ausrichtung + Position (Groesse bleibt gemeinsam):");

        v = m_hud.ada_rot;

        if (ImGui::Checkbox("Eigene Rotation + Position fuer Ada verwenden", &v)) {
            m_hud.ada_rot = v;
            save_hud_cfg();
        }

        {
            const auto ch = call_global_string("__re4_char_now");
            const std::string line = std::string{"   aktueller Charakter: "}
                                     + (ch.empty() ? std::string{"unbekannt"} : ch);
            ImGui::TextColored(ImColor{0xFF888888}, "%s", line.c_str());
        }

        slider("Ada Rot X (Pitch)", m_hud.ada_rx, -180.0f, 180.0f, "%.0f");
        slider("Ada Rot Y (Yaw)", m_hud.ada_ry, -180.0f, 180.0f, "%.0f");
        slider("Ada Rot Z (Roll)", m_hud.ada_rz, -180.0f, 180.0f, "%.0f");
        slider("Ada Rechts (dx)", m_hud.ada_dx, -0.5f, 0.5f, "%.3f");
        slider("Ada Hoch (dy)", m_hud.ada_dy, -0.5f, 0.5f, "%.3f");
        slider("Ada Vor/Zurueck (dz)", m_hud.ada_dz, -0.5f, 0.5f, "%.3f");

        ImGui::Separator();
        ImGui::Text("Einzelne Anzeigen an/aus:");

        for (const auto& [gname, label] : HUD_TARGETS) {
            bool g = m_hud.guis[gname];
            const auto text = std::string{label} + " (" + gname + ")";

            if (ImGui::Checkbox(text.c_str(), &g)) {
                m_hud.guis[gname] = g;
                save_hud_cfg();
            }
        }

        ImGui::TreePop();
    }

    // ---- Laser ----
    if (ImGui::TreeNode("RE4 VR \xE2\x80\x94 Laser")) {
        const auto slider = [&](const char* label, float& dst, float lo, float hi,
                                const char* fmt) {
            float t = dst;

            if (ImGui::SliderFloat(label, &t, lo, hi, fmt)) {
                dst = t;
                save_laser_cfg();
            }
        };

        v = m_laser.enabled;

        if (ImGui::Checkbox("VR-Laser-Fix aktiv (an Waffe tackern)", &v)) {
            m_laser.enabled = v;
            save_laser_cfg();
        }

        ImGui::Separator();
        ImGui::Text("Strahl");
        slider("Dicke", m_laser.width, 0.1f, 6.0f, "%.2f");
        slider("Laenge", m_laser.length, 0.1f, 4.0f, "%.2f");

        ImGui::Separator();
        ImGui::Text("Farbe");

        v = m_laser.force_color;

        if (ImGui::Checkbox("Farbe erzwingen (sonst native)", &v)) {
            m_laser.force_color = v;
            save_laser_cfg();
        }

        if (m_laser.force_color) {
            float col[3] = {m_laser.r, m_laser.g, m_laser.b};

            if (ImGui::ColorEdit3("Laser-Farbe", col)) {
                m_laser.r = col[0];
                m_laser.g = col[1];
                m_laser.b = col[2];
                save_laser_cfg();
            }
        }

        if (ImGui::Button("Gelb##laserdev")) {
            m_laser.force_color = true;
            m_laser.r = 1.0f;
            m_laser.g = 1.0f;
            m_laser.b = 0.0f;
            save_laser_cfg();
        }

        ImGui::Separator();
        ImGui::Text("Glow / Deckkraft");

        v = m_laser.tune_glow;

        if (ImGui::Checkbox("Glow/Alpha tunen", &v)) {
            m_laser.tune_glow = v;
            save_laser_cfg();
        }

        if (m_laser.tune_glow) {
            slider("Leuchtkraft (Emissive)", m_laser.glow, 0.0f, 8.0f, "%.2f");
            slider("Deckkraft (Alpha, niedrig=transluzent)", m_laser.alpha, 0.0f, 1.0f, "%.2f");
        }

        ImGui::Separator();
        ImGui::Text("Smokey");

        v = m_laser.smoke;

        if (ImGui::Checkbox("Smokey-Modus", &v)) {
            m_laser.smoke = v;
            save_laser_cfg();
        }

        if (m_laser.smoke) {
            slider("Staerke", m_laser.smoke_amt, 0.0f, 1.0f, "%.2f");
            slider("Tempo", m_laser.smoke_speed, 0.5f, 30.0f, "%.1f");
        }

        ImGui::Separator();
        ImGui::Text("Dot");

        v = m_laser.dot_raycast;

        if (ImGui::Checkbox("Dot auf Surface (Raycast)", &v)) {
            m_laser.dot_raycast = v;
            save_laser_cfg();
        }

        if (!m_laser.dot_raycast) {
            slider("Dot-Distanz (fix)", m_laser.dot_dist, 0.5f, 30.0f, "%.2f");
        }

        slider("Dot-Groesse", m_laser.dot_size, 0.1f, 5.0f, "%.2f");

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

void RE4VRCrosshair::draw_dev_trees() {
    re4vr::trace("RE4VRCrosshair", "on_draw_ui");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    draw_dev_trees();

    // [MENUE-REIHENFOLGE 2026-09-07] Hier stand der Fallback "kein Lua-Dispatcher
    // -> Public-Block selbst zeichnen". Genau der liess die nackten Optionen
    // zwischen den Entwickler-Trees auftauchen, seit ##re4_vr_menu.lua mit dem
    // Port abgeschaltet ist. Gezeichnet wird jetzt zentral in
    // RE4VRMenu::draw_public, in fester Reihenfolge und ganz oben.
}

#endif // RE4
