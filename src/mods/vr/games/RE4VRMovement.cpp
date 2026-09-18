// ============================================================================
// RE4VRMovement -- 1:1-Portierung von re4_vr_movement.lua
// Spezifikation: I:\LUATRANS\PORT_MOVEMENT_SPEC.md (samt NACHTRAG 03.09.2026)
//
// Zeilenverweise beziehen sich auf die Lua-Datei, Stand 03.09.2026 (3001 Z.).
//
// LOGGING: Die Lua-Datei ueberdeckt `log` einmal pro Datei stumm (Z.6-12) --
// alle 76 Logzeilen laufen ins Leere, solange `__re4_logging_on` nicht true ist.
// Der Port laesst sie deshalb weg; die Bedingung waere ohnehin immer falsch.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <sdk/Application.hpp>
#include <sdk/MotionFsm2Layer.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <utility/String.hpp>

#include "../../../HookManager.hpp"
#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"   // g_framework->draw_menu_checkbox
#include "../../VR.hpp"
#include "RE4VR.hpp"
#include "RE4VRMovement.hpp"

#if defined(RE4)

namespace {

// Lua: os.clock(). Luas os.clock IST in dieser DLL clock()/CLOCKS_PER_SEC --
// also bitgenau dieselbe Uhr und dieselbe Epoche. (Der Spec-Nachtrag merkt zu
// Recht an, dass MSVCs clock() Wanduhr seit Prozessstart ist und steady_clock
// fuer reine Differenzen genuegen wuerde; std::clock ist aber IDENTISCH statt
// nur aequivalent, und `__re4_sq_end_t` wird als Lua-Global veroeffentlicht.)
double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

constexpr double PI_D = 3.14159265358979323846;

double deg2rad(double d) {
    return d * PI_D / 180.0;
}

double rad2deg(double r) {
    return r * 180.0 / PI_D;
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

// via.vec3/via.vec4/via.Quaternion sind ValueTypes > 8 Byte -> sret-Pfad mit
// 16-Byte-ausgerichtetem Puffer.
bool get_vec4(::REManagedObject* obj, std::string_view name, glm::vec4& out) {
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

// Luas `Vector3f.new(x,y,z)` an eine via.vec4-Signatur: das vierte Element ist
// dort nicht gesetzt und kommt beim Lesen als 1.0 zurueck (s. sqab_tick,
// `(type(p)=="userdata" and p.w) or 1.0`). Deshalb hier fest 1.0, ausser wo
// das Original ausdruecklich ein gelesenes w durchreicht.
bool set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v, float w = 1.0f) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, w};
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

// getJointByName(System.String)
::REManagedObject* joint_by_name(::REManagedObject* tf, const char* name) {
    if (tf == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str);
}

// Luas `q * v` (Quaternion mal Vector3f). glm macht dasselbe.
glm::vec3 quat_rotate_vec3(const glm::quat& q, const glm::vec3& v) {
    return q * v;
}

glm::quat qnorm(const glm::quat& q) {
    return glm::normalize(q);
}

glm::quat qconj(const glm::quat& q) {
    return glm::conjugate(q);
}

// Lua Z.1223-1227 / Z.347-355 -- KEIN Vector3f:to_quat(), sondern der reine
// Yaw-Winkel aus dem geflatteten Forward.
std::optional<double> flat_yaw_from_fwd(const glm::vec3& f) {
    const double len = std::sqrt(static_cast<double>(f.x) * f.x + static_cast<double>(f.z) * f.z);

    if (len < 0.0001) {
        return std::nullopt;
    }

    // Luas math.atan(a, b) mit zwei Argumenten ist atan2.
    return std::atan2(static_cast<double>(f.x) / len, static_cast<double>(f.z) / len);
}

// Lua Z.1229-1232.
glm::quat yaw_to_quat(double yaw) {
    const double half = yaw * 0.5;
    return glm::quat{static_cast<float>(std::cos(half)), 0.0f, static_cast<float>(std::sin(half)),
                     0.0f};
}

// Lua Z.531-535 (spin_axis_quat).
glm::quat axis_quat(const glm::vec3& a, double rad) {
    const double h = rad * 0.5;
    const double s = std::sin(h);
    return glm::quat{static_cast<float>(std::cos(h)), static_cast<float>(a.x * s),
                     static_cast<float>(a.y * s), static_cast<float>(a.z * s)};
}

double wrap_pi(double a) {
    if (a > PI_D) {
        return a - 2.0 * PI_D;
    }

    if (a < -PI_D) {
        return a + 2.0 * PI_D;
    }

    return a;
}

// Lua Z.437-442.
bool quat_approx_equal(const glm::quat& a, const glm::quat& b) {
    double dot = static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y
        + static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w;

    if (dot < 0.0) {
        dot = -dot;
    }

    return dot > 0.9999995;
}

// Lua Z.408 / Z.497 / Z.505.
const std::array<const char*, 4> SPINE_YAW_JOINTS = {"Spine_1", "Spine_2", "Neck_0", "Neck_1"};

// [DUCKEN PER IK 16.09.2026] via.motion.IkLeg2.EffectorCtrl, live aus der TDB
// gelesen (zzz_re4_ikleg_enum_probe.lua): None=0, LocalOffset=1,
// WorldOffset=2, Local=3, World=4. GroundContactUpDistance stand bei Leon auf
// 0,8 -- nur Rueckfall, falls das Einlesen scheitert.
constexpr int32_t IKLEG_CTRL_NONE = 0;
constexpr int32_t IKLEG_CTRL_WORLD_OFFSET = 2;
constexpr float IKLEG_GROUND_UP_GAME = 0.8f;

// [HALS-DREHPUNKT 16.09.2026] Wo der Kopf beim Nicken dreht, gemessen vom
// Tracking-Punkt des Headsets: so weit darunter und dahinter. Anatomische
// Richtwerte -- duckt Leon beim Runterschauen noch, hier nachstellen.
constexpr float RS_NECK_DOWN = 0.10f;
constexpr float RS_NECK_BACK = 0.08f;

// [FUSS-SNAP 16.09.2026] Das Bein-IK setzt die Fuesse beim Roomscale-Gehen nicht
// um -- es haelt sie am alten Bodenkontakt fest (gemessen: 0,5-0,7 m hinter Leon,
// beim Stehen 0, beim Stick-Laufen 0,14-0,28). Stehen sie weiter weg als
// RS_SNAP_DIST, wird das IK fuer EINEN Frame losgelassen: die Fuesse fallen in
// die Animationspose unter dem Koerper zurueck, danach setzt das IK sie dort
// neu auf. Geprueft wird nur alle RS_SNAP_EVERY Frames (zwei Joint-Abfragen).
constexpr float RS_SNAP_DIST = 0.30f;
constexpr int RS_SNAP_EVERY = 6;
// Nicht in tiefer Hocke: ohne IK waere auch das Ducken einen Frame weg -- Leon
// zuckte hoch.
constexpr float RS_SNAP_MAX_DUCK = -0.10f;
// Laeuft der Spieler selbst mit dem Stick, macht die Laufanimation die Schritte.
constexpr float RS_SNAP_STICK_IDLE = 0.20f;

const std::array<const char*, 7> PIN_JOINTS = {"Hip",    "Spine_0", "Spine_1", "Spine_2",
                                               "Neck_0", "Neck_1",  "Head"};

bool is_ub_joint(const std::string& n) {
    return n == "Spine_0" || n == "Spine_1" || n == "Spine_2" || n == "Neck_0" || n == "Neck_1";
}

// Lua Z.913-922 / 928-934 / 941-943.
const std::vector<const char*> STOP_NODES = {
    "ch0_540_JOG_END",       "ch0_550_JOG_END_LIGHT",  "general_0381_stop_jog",
    "ch0_740_DASH_END",      "ch0_749_DASH_CANCEL",    "ch0_406_WALK_END_FRONT",
    "ch0_446_WALK_END_BACK", "general_0380_stop_walk", "ch0_1406_CROUCH_WALK_END_FRONT",
    "ch0_1446_CROUCH_WALK_END_BACK",
};

const std::vector<const char*> START_NODES = {
    "ch0_400_WALK_START_FRONT",        "ch0_440_WALK_START_BACK",
    "ch0_490_WALK_START_TURN",         "ch0_1400_CROUCH_WALK_START_FRONT",
    "ch0_1440_CROUCH_WALK_START_BACK", "ch0_1490_CROUCH_WALK_START_TURN",
};

const std::vector<const char*> RUN_START_NODES = {
    "ch0_500_JOG_START",
    "ch0_700_DASH_START",
    "ch0_1500_CROUCH_TO_JOG_START",
};

constexpr double BODY_YAW_EPS_DEG = 0.05;
constexpr double SQAB_RETRIG_WINDOW = 1.5;   // s
constexpr double SQAB_SAME_SPOT = 3.5;       // m

const char* KS_MODULE = "re4vr/re4_vr_killswitch";

} // namespace

std::shared_ptr<RE4VRMovement>& RE4VRMovement::get() {
    static std::shared_ptr<RE4VRMovement> inst = std::make_shared<RE4VRMovement>();
    return inst;
}

// ============================================================================
// Referenzzaehlung
// ============================================================================

bool RE4VRMovement::keep(::REManagedObject* o, RefHandle& out) {
    drop(out);

    if (o == nullptr || !re4vr::obj_ok(o)) {
        return false;
    }

    out.obj = o;

    // Dieselbe Heuristik wie sol_lua_push.
    if (static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        out.reffed = true;
    } else {
        out.reffed = false;
    }

    return true;
}

void RE4VRMovement::drop(RefHandle& h) {
    if (h.reffed && h.obj != nullptr && re4vr::obj_ok(h.obj)) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}


// ============================================================================
// Konfiguration (Lua Z.41-259)
// ============================================================================

namespace {

bool jbool(const nlohmann::json& d, const char* key, bool cur) {
    // Luas `if d.x ~= nil then cfg.x = d.x == true end` -- jeder Nicht-true-Wert
    // wird zu false, ein fehlender Schluessel laesst den Default stehen.
    if (!d.is_object() || !d.contains(key) || d[key].is_null()) {
        return cur;
    }

    return d[key].is_boolean() && d[key].get<bool>();
}

double jnum(const nlohmann::json& d, const char* key, double cur) {
    // Luas `if type(d.x) == "number" then ... end`.
    if (!d.is_object() || !d.contains(key) || !d[key].is_number()) {
        return cur;
    }

    return d[key].get<double>();
}

} // namespace

void RE4VRMovement::load_cfg() {
    const auto d = re4vr::json_load("re4_vr/re4_vr_movement.json");

    if (d.is_object() && !d.empty()) {
        m_cfg.enabled = jbool(d, "enabled", m_cfg.enabled);
        m_cfg.hmd_yaw_drive = jbool(d, "hmd_yaw_drive", m_cfg.hmd_yaw_drive);
        m_cfg.yaw_offset_deg = jnum(d, "yaw_offset_deg", m_cfg.yaw_offset_deg);
        m_cfg.roomscale = jbool(d, "roomscale", m_cfg.roomscale);
        m_cfg.rs_lean = jbool(d, "rs_lean", m_cfg.rs_lean);
        m_cfg.rs_lean_radius = jnum(d, "rs_lean_radius", m_cfg.rs_lean_radius);
        m_cfg.rs_lean_migrated = jbool(d, "rs_lean_migrated", m_cfg.rs_lean_migrated);
        m_cfg.rs_recenter = jbool(d, "rs_recenter", m_cfg.rs_recenter);
        m_cfg.rs_crouch = jbool(d, "rs_crouch", m_cfg.rs_crouch);
        m_cfg.rs_crouch_pct = jnum(d, "rs_crouch_pct", m_cfg.rs_crouch_pct);
        m_cfg.rs_stand_height = jnum(d, "rs_stand_height", m_cfg.rs_stand_height);
        m_cfg.hmd_follow = jbool(d, "hmd_follow", m_cfg.hmd_follow);
        m_cfg.hmd_follow_deadzone = jnum(d, "hmd_follow_deadzone", m_cfg.hmd_follow_deadzone);
        m_cfg.hmd_follow_alpha = jnum(d, "hmd_follow_alpha", m_cfg.hmd_follow_alpha);
        m_cfg.hip_follow = jbool(d, "hip_follow", m_cfg.hip_follow);
        m_cfg.spine_yaw_deg = jnum(d, "spine_yaw_deg", m_cfg.spine_yaw_deg);
        m_cfg.ub_lock = jbool(d, "ub_lock", m_cfg.ub_lock);
        m_cfg.ub_trim_deg = jnum(d, "ub_trim_deg", m_cfg.ub_trim_deg);
        m_cfg.ub_lock_world = jbool(d, "ub_lock_world", m_cfg.ub_lock_world);
        m_cfg.spine_pin = jbool(d, "spine_pin", m_cfg.spine_pin);
        m_cfg.grav_fix = jbool(d, "grav_fix", m_cfg.grav_fix);
        m_cfg.grav_value = jnum(d, "grav_value", m_cfg.grav_value);
        m_cfg.grav_in_elevator = jbool(d, "grav_in_elevator", m_cfg.grav_in_elevator);
        m_cfg.lag_boost_on = jbool(d, "lag_boost_on", m_cfg.lag_boost_on);
        m_cfg.lag_boost_target = jnum(d, "lag_boost_target", m_cfg.lag_boost_target);
        m_cfg.lag_boost_max = jnum(d, "lag_boost_max", m_cfg.lag_boost_max);
        m_cfg.lag_boost_window = jnum(d, "lag_boost_window", m_cfg.lag_boost_window);
        m_cfg.lag_brake_on = jbool(d, "lag_brake_on", m_cfg.lag_brake_on);
        m_cfg.lag_brake_gain = jnum(d, "lag_brake_gain", m_cfg.lag_brake_gain);
        m_cfg.lag_brake_window = jnum(d, "lag_brake_window", m_cfg.lag_brake_window);

        if (d.contains("pin_z") && d["pin_z"].is_object()) {
            const auto& p = d["pin_z"];
            m_cfg.pin_z.Hip = jnum(p, "Hip", m_cfg.pin_z.Hip);
            m_cfg.pin_z.Spine_0 = jnum(p, "Spine_0", m_cfg.pin_z.Spine_0);
            m_cfg.pin_z.Spine_1 = jnum(p, "Spine_1", m_cfg.pin_z.Spine_1);
            m_cfg.pin_z.Spine_2 = jnum(p, "Spine_2", m_cfg.pin_z.Spine_2);
            m_cfg.pin_z.Neck_0 = jnum(p, "Neck_0", m_cfg.pin_z.Neck_0);
            m_cfg.pin_z.Neck_1 = jnum(p, "Neck_1", m_cfg.pin_z.Neck_1);
            m_cfg.pin_z.Head = jnum(p, "Head", m_cfg.pin_z.Head);
        }

        m_cfg.pin_z_hip_walk = jnum(d, "pin_z_hip_walk", m_cfg.pin_z_hip_walk);

        // [nil-SENTINEL] nur setzen, wenn wirklich eine Zahl dasteht.
        if (d.contains("pin_z_hip_crouch") && d["pin_z_hip_crouch"].is_number()) {
            m_cfg.pin_z_hip_crouch = d["pin_z_hip_crouch"].get<double>();
        }

        m_cfg.pin_x_hip = jnum(d, "pin_x_hip", m_cfg.pin_x_hip);

        if (d.contains("pin_x") && d["pin_x"].is_object()) {
            const auto& p = d["pin_x"];
            m_cfg.pin_x.Spine_0 = jnum(p, "Spine_0", m_cfg.pin_x.Spine_0);
            m_cfg.pin_x.Spine_1 = jnum(p, "Spine_1", m_cfg.pin_x.Spine_1);
            m_cfg.pin_x.Spine_2 = jnum(p, "Spine_2", m_cfg.pin_x.Spine_2);
            m_cfg.pin_x.Neck_0 = jnum(p, "Neck_0", m_cfg.pin_x.Neck_0);
            m_cfg.pin_x.Neck_1 = jnum(p, "Neck_1", m_cfg.pin_x.Neck_1);
            m_cfg.pin_x.Head = jnum(p, "Head", m_cfg.pin_x.Head);
        }

        m_cfg.pin_ub_z = jnum(d, "pin_ub_z", m_cfg.pin_ub_z);
        m_cfg.pin_ub_z_crouch = jnum(d, "pin_ub_z_crouch", m_cfg.pin_ub_z_crouch);
        m_cfg.pin_ub_x = jnum(d, "pin_ub_x", m_cfg.pin_ub_x);
        m_cfg.pin_ub_x_crouch = jnum(d, "pin_ub_x_crouch", m_cfg.pin_ub_x_crouch);
        m_cfg.pin_ub_yaw = jnum(d, "pin_ub_yaw", m_cfg.pin_ub_yaw);
        m_cfg.pin_hip_yaw = jnum(d, "pin_hip_yaw", m_cfg.pin_hip_yaw);
        m_cfg.pin_spine1_roll = jnum(d, "pin_spine1_roll", m_cfg.pin_spine1_roll);

        if (d.contains("pin_pose") && d["pin_pose"].is_object()) {
            m_cfg.pin_pose = d["pin_pose"];
        }

        if (d.contains("crouch_pose") && d["crouch_pose"].is_object()) {
            m_cfg.crouch_pose = d["crouch_pose"];
        }

        m_cfg.body_yaw_speed = jnum(d, "body_yaw_speed", m_cfg.body_yaw_speed);
        m_cfg.yaw_hmd_target = jbool(d, "yaw_hmd_target", m_cfg.yaw_hmd_target);
        m_cfg.cam_body_rotate = jbool(d, "cam_body_rotate", m_cfg.cam_body_rotate);
        m_cfg.stop_skip = jbool(d, "stop_skip", m_cfg.stop_skip);
        m_cfg.stop_skip_frames = jnum(d, "stop_skip_frames", m_cfg.stop_skip_frames);
        m_cfg.start_skip = jbool(d, "start_skip", m_cfg.start_skip);
        m_cfg.start_skip_frames = jnum(d, "start_skip_frames", m_cfg.start_skip_frames);
        m_cfg.no_pivot = jbool(d, "no_pivot", m_cfg.no_pivot);
    }

    // [LEHNEN DEFAULT AN 2026-09-01] Lua Z.245-249: einmalige Migration.
    // Bewusst NACH dem Laden.
    if (!m_cfg.rs_lean_migrated) {
        m_cfg.rs_lean = true;
        m_cfg.rs_lean_migrated = true;
        save_cfg();
    }

    // Lua Z.254: unbedingt, nach dem Laden, OHNE save_cfg. Der ganze
    // cam_body_rotate-Zweig ist damit beim Start unerreichbar -- per UI
    // reaktivierbar, aber niemals persistent.
    m_cfg.cam_body_rotate = false;
}

void RE4VRMovement::save_cfg() {
    // [K1] Niemals speichern, bevor geladen wurde -- sonst ueberschreibt der
    // erste Schreibvorgang die Datei mit Compile-Defaults.
    if (!m_cfg_loaded) {
        return;
    }

    nlohmann::json d = nlohmann::json::object();
    d["enabled"] = m_cfg.enabled;
    d["hmd_yaw_drive"] = m_cfg.hmd_yaw_drive;
    d["yaw_offset_deg"] = m_cfg.yaw_offset_deg;
    d["roomscale"] = m_cfg.roomscale;
    d["rs_lean"] = m_cfg.rs_lean;
    d["rs_lean_radius"] = m_cfg.rs_lean_radius;
    d["rs_lean_migrated"] = m_cfg.rs_lean_migrated;
    d["rs_recenter"] = m_cfg.rs_recenter;
    d["rs_crouch"] = m_cfg.rs_crouch;
    d["rs_crouch_pct"] = m_cfg.rs_crouch_pct;
    d["rs_stand_height"] = m_cfg.rs_stand_height;
    d["hmd_follow"] = m_cfg.hmd_follow;
    d["hmd_follow_deadzone"] = m_cfg.hmd_follow_deadzone;
    d["hmd_follow_alpha"] = m_cfg.hmd_follow_alpha;
    d["hip_follow"] = m_cfg.hip_follow;
    d["spine_yaw_deg"] = m_cfg.spine_yaw_deg;
    d["ub_lock"] = m_cfg.ub_lock;
    d["ub_trim_deg"] = m_cfg.ub_trim_deg;
    d["ub_lock_world"] = m_cfg.ub_lock_world;
    d["spine_pin"] = m_cfg.spine_pin;

    nlohmann::json pz = nlohmann::json::object();
    pz["Hip"] = m_cfg.pin_z.Hip;
    pz["Spine_0"] = m_cfg.pin_z.Spine_0;
    pz["Spine_1"] = m_cfg.pin_z.Spine_1;
    pz["Spine_2"] = m_cfg.pin_z.Spine_2;
    pz["Neck_0"] = m_cfg.pin_z.Neck_0;
    pz["Neck_1"] = m_cfg.pin_z.Neck_1;
    pz["Head"] = m_cfg.pin_z.Head;
    d["pin_z"] = pz;

    d["pin_z_hip_walk"] = m_cfg.pin_z_hip_walk;

    // [nil-SENTINEL] In Lua verschwindet ein nil-Feld beim json.dump_file
    // KOMPLETT -- weder null noch 0.0 schreiben.
    if (m_cfg.pin_z_hip_crouch.has_value()) {
        d["pin_z_hip_crouch"] = *m_cfg.pin_z_hip_crouch;
    }

    d["pin_x_hip"] = m_cfg.pin_x_hip;

    nlohmann::json px = nlohmann::json::object();
    px["Spine_0"] = m_cfg.pin_x.Spine_0;
    px["Spine_1"] = m_cfg.pin_x.Spine_1;
    px["Spine_2"] = m_cfg.pin_x.Spine_2;
    px["Neck_0"] = m_cfg.pin_x.Neck_0;
    px["Neck_1"] = m_cfg.pin_x.Neck_1;
    px["Head"] = m_cfg.pin_x.Head;
    d["pin_x"] = px;

    d["pin_ub_z"] = m_cfg.pin_ub_z;
    d["pin_ub_x"] = m_cfg.pin_ub_x;
    d["pin_ub_x_crouch"] = m_cfg.pin_ub_x_crouch;
    d["pin_ub_yaw"] = m_cfg.pin_ub_yaw;
    d["pin_ub_z_crouch"] = m_cfg.pin_ub_z_crouch;
    d["pin_hip_yaw"] = m_cfg.pin_hip_yaw;
    d["pin_spine1_roll"] = m_cfg.pin_spine1_roll;
    d["body_yaw_speed"] = m_cfg.body_yaw_speed;
    d["yaw_hmd_target"] = m_cfg.yaw_hmd_target;
    d["cam_body_rotate"] = m_cfg.cam_body_rotate;
    d["stop_skip"] = m_cfg.stop_skip;
    d["stop_skip_frames"] = m_cfg.stop_skip_frames;
    d["start_skip"] = m_cfg.start_skip;
    d["start_skip_frames"] = m_cfg.start_skip_frames;
    d["no_pivot"] = m_cfg.no_pivot;
    d["grav_fix"] = m_cfg.grav_fix;
    d["grav_value"] = m_cfg.grav_value;
    d["grav_in_elevator"] = m_cfg.grav_in_elevator;
    d["lag_boost_on"] = m_cfg.lag_boost_on;
    d["lag_boost_target"] = m_cfg.lag_boost_target;
    d["lag_boost_max"] = m_cfg.lag_boost_max;
    d["lag_boost_window"] = m_cfg.lag_boost_window;
    d["lag_brake_on"] = m_cfg.lag_brake_on;
    d["lag_brake_gain"] = m_cfg.lag_brake_gain;
    d["lag_brake_window"] = m_cfg.lag_brake_window;

    // Auch hier: ein nil-Feld faellt in Lua ganz weg.
    if (m_cfg.pin_pose.is_object() && !m_cfg.pin_pose.empty()) {
        d["pin_pose"] = m_cfg.pin_pose;
    }

    if (m_cfg.crouch_pose.is_object() && !m_cfg.crouch_pose.empty()) {
        d["crouch_pose"] = m_cfg.crouch_pose;
    }

    re4vr::json_save("re4_vr/re4_vr_movement.json", d);
}

// ============================================================================
// Getter (Lua Z.271-329)
// ============================================================================

::REManagedObject* RE4VRMovement::get_body_transform() {
    // [FRAME-CACHE] Lua fragt zuerst `__re4_frame_cache`. Das Modul liegt in
    // Lua und liefert dort `_fc.body_tf()` ZURUECK, auch wenn es nil ist --
    // ohne Fallback danach. re4vr::body_transform() hat diesen Zweig nicht,
    // deshalb wird der Weg hier von Hand nachgebaut.
    // [ABWEICHUNG, bewusst] Lua Z.272-273 gibt `_fc.body_tf()` zurueck, wenn der
    // Frame-Cache an ist -- AUCH wenn das nil ist, ohne Rueckfall danach.
    // Nachbauen liesse sich das nur mit einem Getter fuer Managed Objects aus
    // einer Lua-Tabelle, den RE4VR.hpp nicht hat. Der native Weg unten baut
    // dieselbe Kette und liefert denselben Handle; einziger Unterschied ist,
    // dass sie oefter als einmal pro Frame aufgeloest wird.
    if (m_character_manager.obj == nullptr || !re4vr::obj_ok(m_character_manager.obj)) {
        keep(re4vr::character_manager(), m_character_manager);
    }

    if (m_character_manager.obj == nullptr) {
        return nullptr;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(m_character_manager.obj, "getPlayerContextRef");

    if (ctx == nullptr) {
        return nullptr;
    }

    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(body, "get_Transform");
}

::REManagedObject* RE4VRMovement::get_player_cam_controller() {
    if (m_camera_system.obj == nullptr || !re4vr::obj_ok(m_camera_system.obj)) {
        keep(re4vr::camera_system_singleton(), m_camera_system);
    }

    if (m_camera_system.obj == nullptr) {
        return nullptr;
    }

    auto* main =
        re4vr::call_safe<::REManagedObject*>(m_camera_system.obj, "get_MainCameraController");

    if (main == nullptr) {
        return nullptr;
    }

    auto* busy = re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController");

    if (busy == nullptr || m_player_cam_td == nullptr) {
        return nullptr;
    }

    auto def = utility::re_managed_object::get_type_definition(busy);

    if (def == nullptr || !def->is_a(m_player_cam_td)) {
        return nullptr;
    }

    return busy;
}

// Lua Z.300-313.
bool RE4VRMovement::is_ks_active() {
    // [THROWSIGHT] Del-Lago-Harpunen-Stage: wie "KS aktiv" behandeln.
    if (re4vr::lua_get_bool("__re4_throwsight_active", false)) {
        return true;
    }

    // [KS_KEEP_MOVEMENT] Voll-aus-KS, der movement absichtlich weiterlaufen laesst.
    if (re4vr::lua_get_bool("__re4_ks_keep_movement", false)) {
        return false;
    }

    return re4vr::lua_module_call_bool(KS_MODULE, "is_active", false);
}

// Lua Z.318-322 / Z.331-334: fehlende Funktion -> false.
bool RE4VRMovement::is_pin_release() {
    return re4vr::lua_module_call_bool(KS_MODULE, "is_pin_release", false);
}

bool RE4VRMovement::is_crouch_active() {
    return re4vr::lua_module_call_bool(KS_MODULE, "is_crouch_active", false);
}

// Lua Z.2341-2362.
bool RE4VRMovement::pure_gameplay_only() {
    if (is_ks_active()) {
        return false;
    }

    if (re4vr::lua_get_bool("__re4_ks_active", false)) {
        return false;
    }

    // Luas `if type(killswitch.is_pure_gameplay) == "function" then ... end`:
    // FEHLT die Funktion, wird die Pruefung UEBERSPRUNGEN. Nur ein Aufruf, der
    // wirft oder nicht-true liefert, sperrt. Genau das ist die tribool-Form
    // (0 = gesperrt, 1 = durch, -1 = Modul/State fehlt -> wie "Funktion fehlt").
    if (re4vr::lua_module_call_tribool(KS_MODULE, "is_pure_gameplay") == 0) {
        return false;
    }

    if (m_pg_pause.obj == nullptr || !re4vr::obj_ok(m_pg_pause.obj)) {
        keep(sdk::get_managed_singleton<::REManagedObject>("share.PauseManager"), m_pg_pause);
    }

    if (m_pg_pause.obj != nullptr) {
        bool paused = false;

        if (re4vr::try_call<bool>(m_pg_pause.obj, "isPaused()", paused) && paused) {
            return false;
        }
    }

    if (m_pg_gui.obj == nullptr || !re4vr::obj_ok(m_pg_gui.obj)) {
        keep(sdk::get_managed_singleton<::REManagedObject>(game_namespace("GuiManager")), m_pg_gui);
    }

    if (m_pg_gui.obj != nullptr) {
        bool lock = false;

        if (re4vr::try_call<bool>(m_pg_gui.obj, "get_hasOccupiedPauseMenuSystemLock", lock)
            && lock) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Yaw-Helfer
// ============================================================================

std::optional<double> RE4VRMovement::flat_yaw_of(const glm::quat& rot) const {
    return flat_yaw_from_fwd(rot * glm::vec3{0.0f, 0.0f, 1.0f});
}

// Lua Z.347-355. ACHTUNG: das Original benutzt hier `fwd:to_quat()`, und das
// ist in REFramework `glm::quat(rowMajor4(lookAtLH({0,0,0}, v, {0,1,0})))` --
// ROLLFREI, NICHT der minimale Bogen und NICHT yaw_to_quat(atan2(x,z)).
// Ein Ersatz durch glm::quatLookAt / glm::rotation / atan2 liefert ANDERE
// Vorzeichen. NaN droht hier nicht, weil fwd.y auf 0 gezwungen ist.
std::optional<glm::quat> RE4VRMovement::yaw_quat_of(const glm::quat& rot) const {
    glm::vec3 fwd = rot * glm::vec3{0.0f, 0.0f, 1.0f};
    fwd.y = 0.0f;

    const double len = std::sqrt(static_cast<double>(fwd.x) * fwd.x
                                 + static_cast<double>(fwd.z) * fwd.z);

    if (len < 0.0001) {
        return std::nullopt;
    }

    const glm::vec3 n{static_cast<float>(fwd.x / len), 0.0f, static_cast<float>(fwd.z / len)};
    return glm::quat(glm::rowMajor4(glm::lookAtLH(glm::vec3{0.0f, 0.0f, 0.0f}, n,
                                                  glm::vec3{0.0f, 1.0f, 0.0f})));
}

// ============================================================================
// Hip-Follow (Lua Z.365-420)
// ============================================================================

::REManagedObject* RE4VRMovement::get_hip_joint(::REManagedObject* tf) {
    if (m_hip_joint.obj != nullptr) {
        if ((now_clock() - m_hipjv_bad) < 0.5) {
            return nullptr;
        }

        glm::vec3 probe{};

        if (get_vec3(m_hip_joint.obj, "get_Position", probe)) {
            return m_hip_joint.obj;
        }

        m_hipjv_bad = now_clock();
        drop(m_hip_joint);
    }

    if (tf == nullptr) {
        return nullptr;
    }

    auto* j = joint_by_name(tf, "Hip");

    if (j != nullptr) {
        keep(j, m_hip_joint);
    }

    return m_hip_joint.obj;
}

bool RE4VRMovement::is_user_turning() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    const auto ax = vr->get_right_stick_axis();
    return std::abs(ax.x) > 0.1f;
}

void RE4VRMovement::apply_hip_follow(::REManagedObject* tf, const glm::quat& root_yaw_quat) {
    if (!m_cfg.hip_follow) {
        return;
    }

    if (mv_in_zone()) {
        return;   // [STILLZONE]
    }

    if (tf == nullptr) {
        return;
    }

    auto* hip = get_hip_joint(tf);

    if (hip == nullptr) {
        return;
    }

    glm::quat hr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(hip, "get_Rotation", hr)) {
        return;
    }

    const auto hip_yaw = yaw_quat_of(hr);

    if (!hip_yaw.has_value()) {
        return;
    }

    if (!is_user_turning()) {
        // Referenz im Stand mitlernen: offset = root_yaw^-1 * hip_yaw
        const glm::quat off = qnorm(qconj(root_yaw_quat) * *hip_yaw);

        if (m_hip_ref_offset.has_value()) {
            m_hip_ref_offset = glm::slerp(*m_hip_ref_offset, off, 0.1f);
        } else {
            m_hip_ref_offset = off;
        }

        return;
    }

    // Beim Drehen: Hip-Yaw hart auf Root-Yaw * Referenz zwingen
    if (!m_hip_ref_offset.has_value()) {
        return;
    }

    const glm::quat desired_yaw = qnorm(root_yaw_quat * *m_hip_ref_offset);
    const glm::quat correction = qnorm(desired_yaw * qconj(*hip_yaw));
    const glm::quat new_rot = qnorm(correction * hr);
    set_quat(hip, "set_Rotation", new_rot);
}

::REManagedObject* RE4VRMovement::get_spine_joint(::REManagedObject* tf, const char* name) {
    const std::string key{name};
    auto it = m_spine_joint_cache.find(key);

    if (it != m_spine_joint_cache.end() && it->second.obj != nullptr) {
        glm::vec3 probe{};

        if (get_vec3(it->second.obj, "get_Position", probe)) {
            return it->second.obj;
        }

        drop(it->second);
        m_spine_joint_cache.erase(it);
    }

    if (tf == nullptr) {
        return nullptr;
    }

    auto* j = joint_by_name(tf, name);

    if (j != nullptr) {
        RefHandle h{};

        if (keep(j, h)) {
            m_spine_joint_cache[key] = h;
        }
    }

    return j;
}

// ============================================================================
// Spine-Pin (Lua Z.497-907)
// ============================================================================

namespace {

// Lua Z.520-526: spin_yaw_of -- Yaw des geflatteten Forward, nil bei zu kurz.
std::optional<double> spin_yaw_of(const glm::quat& rot) {
    const glm::vec3 f = rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const double len = std::sqrt(static_cast<double>(f.x) * f.x + static_cast<double>(f.z) * f.z);

    if (len < 0.0001) {
        return std::nullopt;
    }

    return std::atan2(static_cast<double>(f.x) / len, static_cast<double>(f.z) / len);
}

} // namespace

// Lua Z.501-518.
bool RE4VRMovement::spinepin_resolve(::REManagedObject* tf) {
    if (m_spinepin_tf == tf && !m_spinepin_joints.empty()) {
        return true;
    }

    m_spinepin_tf = tf;

    for (auto& h : m_spinepin_joints) {
        drop(h);
    }

    m_spinepin_joints.clear();
    m_spinepin_names.clear();
    // m_spinepin_pose bleibt absichtlich stehen: die gecapturte Pose ist
    // Skelett-generisch -- nach Save-Load greift der Pin sofort wieder.

    // Anker = Root-Joint (joints[0]).
    auto* joints = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints");

    if (joints != nullptr) {
        // [ARRAY, KEINE LISTE -- Fund 04.09.2026] Lua Z.511 schreibt `js[0]`,
        // das REFramework-Array-Binding. get_Item gibt es auf einem
        // System.Array nicht -- der Anker-Joint des Spine-Pins kam nie an.
        auto* root = re4vr::array_element(joints, 0);

        if (root != nullptr) {
            RefHandle h{};

            if (keep(root, h)) {
                m_spinepin_joints.push_back(h);
                m_spinepin_names.emplace_back("root");
            }
        }
    }

    for (const char* name : PIN_JOINTS) {
        auto* j = joint_by_name(tf, name);

        if (j != nullptr) {
            RefHandle h{};

            if (keep(j, h)) {
                m_spinepin_joints.push_back(h);
                m_spinepin_names.emplace_back(name);
            }
        }
    }

    if (m_spinepin_joints.empty()) {
        return false;
    }

    // Null_Offset separat (Parallel-Joint unter root, kein Kettenglied).
    keep(joint_by_name(tf, "Null_Offset"), m_spinepin_null_off);
    return true;
}

// Lua Z.562-613. ENTDRILLEN: pro Kettenglied den Welt-Yaw (relativ zum
// Transform) neutralisieren, Locals daraus neu ableiten.
bool RE4VRMovement::spinepin_capture(::REManagedObject* tf, Pose& out) {
    glm::quat tr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", tr)) {
        return false;
    }

    const glm::quat tri = qconj(tr);
    const size_t n = m_spinepin_joints.size();

    // 1) Welt-Rotationen einsammeln und pro Joint im Yaw geradedrehen.
    std::vector<glm::quat> fixed_rel(n);

    for (size_t i = 0; i < n; ++i) {
        glm::quat wr{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat(m_spinepin_joints[i].obj, "get_Rotation", wr)) {
            return false;
        }

        glm::quat rel_r = qnorm(tri * wr);
        const auto y = spin_yaw_of(rel_r);

        if (y.has_value() && *y != 0.0) {
            rel_r = qnorm(yaw_to_quat(-*y) * rel_r);
        }

        fixed_rel[i] = rel_r;
    }

    // 2) Locals aus den begradigten Welt-Posen ableiten; nebenbei je Joint das
    // Konjugat der PARENT-Rotation merken (fuer die Offset-Slider).
    out.rel.assign(n, PoseEntry{});
    out.par_inv.assign(n, std::nullopt);

    for (size_t i = 0; i < n; ++i) {
        glm::vec3 lp{};

        if (!get_vec3(m_spinepin_joints[i].obj, "get_LocalPosition", lp)) {
            return false;
        }

        glm::quat lr{1.0f, 0.0f, 0.0f, 0.0f};

        if (i == 0) {
            lr = fixed_rel[0];
            out.par_inv[i] = std::nullopt;   // Parent = Transform -> Identitaet
        } else {
            lr = qnorm(qconj(fixed_rel[i - 1]) * fixed_rel[i]);
            out.par_inv[i] = qconj(fixed_rel[i - 1]);
        }

        out.rel[i].p = lp;
        out.rel[i].r = lr;
    }

    // Null_Offset roh-lokal capturen.
    out.null_rel.reset();

    if (m_spinepin_null_off.obj != nullptr) {
        glm::vec3 lp{};
        glm::quat lr{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_vec3(m_spinepin_null_off.obj, "get_LocalPosition", lp)
            && get_quat(m_spinepin_null_off.obj, "get_LocalRotation", lr)) {
            PoseEntry e{};
            e.p = lp;
            e.r = lr;
            out.null_rel = e;
        }
    }

    out.valid = true;
    return true;
}

// Lua Z.617-641.
void RE4VRMovement::spinepin_store(const Pose& pose, const char* cfg_key) {
    if (!pose.valid || m_spinepin_names.empty()) {
        return;
    }

    nlohmann::json store = nlohmann::json::object();
    nlohmann::json joints = nlohmann::json::object();

    for (size_t i = 0; i < m_spinepin_names.size() && i < pose.rel.size(); ++i) {
        const auto& r = pose.rel[i];
        nlohmann::json e = nlohmann::json::object();
        e["px"] = r.p.x;
        e["py"] = r.p.y;
        e["pz"] = r.p.z;
        e["rw"] = r.r.w;
        e["rx"] = r.r.x;
        e["ry"] = r.r.y;
        e["rz"] = r.r.z;

        // In Lua sind iw/ix/iy/iz bei fehlendem par_inv `nil` -- und ein
        // nil-Feld verschwindet beim json.dump_file KOMPLETT.
        if (i < pose.par_inv.size() && pose.par_inv[i].has_value()) {
            const auto& pi = *pose.par_inv[i];
            e["iw"] = pi.w;
            e["ix"] = pi.x;
            e["iy"] = pi.y;
            e["iz"] = pi.z;
        }

        joints[m_spinepin_names[i]] = e;
    }

    store["joints"] = joints;

    if (pose.null_rel.has_value()) {
        nlohmann::json nl = nlohmann::json::object();
        nl["px"] = pose.null_rel->p.x;
        nl["py"] = pose.null_rel->p.y;
        nl["pz"] = pose.null_rel->p.z;
        nl["rw"] = pose.null_rel->r.w;
        nl["rx"] = pose.null_rel->r.x;
        nl["ry"] = pose.null_rel->r.y;
        nl["rz"] = pose.null_rel->r.z;
        store["null"] = nl;
    }

    if (std::string{cfg_key} == "pin_pose") {
        m_cfg.pin_pose = store;
    } else {
        m_cfg.crouch_pose = store;
    }

    save_cfg();
}

// Lua Z.644-671. Rueckgabe false = "nil" (fehlende oder kaputte Pose).
bool RE4VRMovement::spinepin_restore(const char* cfg_key, Pose& out) {
    const nlohmann::json& store =
        (std::string{cfg_key} == "pin_pose") ? m_cfg.pin_pose : m_cfg.crouch_pose;

    if (!store.is_object() || !store.contains("joints") || !store["joints"].is_object()) {
        return false;
    }

    if (m_spinepin_names.empty()) {
        return false;
    }

    const auto& js = store["joints"];
    const size_t n = m_spinepin_names.size();
    out.rel.assign(n, PoseEntry{});
    out.par_inv.assign(n, std::nullopt);

    for (size_t i = 0; i < n; ++i) {
        const auto& nm = m_spinepin_names[i];

        if (!js.contains(nm) || !js[nm].is_object()) {
            return false;
        }

        const auto& s = js[nm];

        if (!s.contains("px") || !s["px"].is_number() || !s.contains("rw")
            || !s["rw"].is_number()) {
            return false;
        }

        out.rel[i].p = glm::vec3{static_cast<float>(jnum(s, "px", 0.0)),
                                 static_cast<float>(jnum(s, "py", 0.0)),
                                 static_cast<float>(jnum(s, "pz", 0.0))};
        out.rel[i].r = glm::quat{static_cast<float>(jnum(s, "rw", 1.0)),
                                 static_cast<float>(jnum(s, "rx", 0.0)),
                                 static_cast<float>(jnum(s, "ry", 0.0)),
                                 static_cast<float>(jnum(s, "rz", 0.0))};

        if (s.contains("iw") && s["iw"].is_number()) {
            out.par_inv[i] = glm::quat{static_cast<float>(jnum(s, "iw", 1.0)),
                                       static_cast<float>(jnum(s, "ix", 0.0)),
                                       static_cast<float>(jnum(s, "iy", 0.0)),
                                       static_cast<float>(jnum(s, "iz", 0.0))};
        }
    }

    out.null_rel.reset();

    if (store.contains("null") && store["null"].is_object() && store["null"].contains("px")
        && store["null"]["px"].is_number()) {
        const auto& nl = store["null"];
        PoseEntry e{};
        e.p = glm::vec3{static_cast<float>(jnum(nl, "px", 0.0)),
                        static_cast<float>(jnum(nl, "py", 0.0)),
                        static_cast<float>(jnum(nl, "pz", 0.0))};
        e.r = glm::quat{static_cast<float>(jnum(nl, "rw", 1.0)),
                        static_cast<float>(jnum(nl, "rx", 0.0)),
                        static_cast<float>(jnum(nl, "ry", 0.0)),
                        static_cast<float>(jnum(nl, "rz", 0.0))};
        out.null_rel = e;
    }

    out.valid = true;
    return true;
}

// Lua Z.673-907.
void RE4VRMovement::apply_spine_pin(bool can_capture) {
    if (!m_cfg.spine_pin) {
        m_spinepin_pose = Pose{};
        re4vr::lua_set_bool("__vr_surge_bridged", false);
        return;
    }

    if (is_ks_active()) {
        return;
    }

    // [STILLZONE] `rel` bleibt stehen.
    if (mv_in_zone()) {
        return;
    }

    if (is_pin_release()) {
        re4vr::lua_set_bool("__vr_surge_bridged", false);
        return;
    }

    // [CROUCH_RELEASE] Pin waehrend der Hock-Anim aussetzen, Pose behalten.
    if (is_crouch_active()) {
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    // [SPAWN_GUARD] Save-Load/Tod baut den Player neu: alte Joint-Handles sind
    // Leichen. Anker validieren, bei Leiche nur die Joints neu aufloesen.
    if (!m_spinepin_joints.empty()) {
        glm::vec3 probe{};

        if (!get_vec3(m_spinepin_joints[0].obj, "get_Position", probe)) {
            m_spinepin_tf = nullptr;

            for (auto& h : m_spinepin_joints) {
                drop(h);
            }

            m_spinepin_joints.clear();
            m_spinepin_names.clear();
            drop(m_spinepin_null_off);
        }
    }

    if (!spinepin_resolve(tf)) {
        return;
    }

    if (!m_spinepin_pose.valid) {
        re4vr::lua_set_bool("__vr_surge_bridged", false);

        // 1) Persistierte Steh-Pose von Platte (einmal pro Script-Lauf).
        if (!m_spinepin_cfg_tried) {
            m_spinepin_cfg_tried = true;
            Pose p{};

            if (spinepin_restore("pin_pose", p)) {
                m_spinepin_pose = p;
                return;
            }
        }

        // 2)/3) im ruhigen Stand sauber capturen + sichern, sonst SOFORT
        // provisorisch aus der laufenden Anim.
        if (can_capture) {
            const auto a = re4vr::lua_get_string("__vr_anim_l0");
            Pose p{};

            if (spinepin_capture(tf, p)) {
                m_spinepin_pose = p;
                std::string low = a;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (!a.empty() && low.find("stand") != std::string::npos) {
                    m_spinepin_provisional = false;
                    spinepin_store(p, "pin_pose");
                } else {
                    m_spinepin_provisional = true;
                }
            }
        }

        return;
    }

    // Provisorische Pose beim ersten ruhigen Stand still ersetzen + sichern.
    if (m_spinepin_provisional && can_capture) {
        const auto a = re4vr::lua_get_string("__vr_anim_l0");
        std::string low = a;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (!a.empty() && low.find("stand") != std::string::npos) {
            Pose p{};

            if (spinepin_capture(tf, p)) {
                m_spinepin_pose = p;
                m_spinepin_provisional = false;
                spinepin_store(p, "pin_pose");
            }
        }
    }

    const Pose& cur = m_spinepin_pose;

    // Bridge aktiv: firstperson darf den Surge-Shift NICHT nochmal auf die
    // Kamera legen (Head traegt ihn schon).
    re4vr::lua_set_bool("__vr_surge_bridged", true);

    // [HIP_RUN_Z] Blendfaktor: nur im Rennen, weich rein/raus -- 1x/Frame im
    // Capture-Slot ticken.
    if (can_capture) {
        const auto a = re4vr::lua_get_string("__vr_anim_l0");
        bool running = false;
        bool walking = false;

        if (!a.empty()) {
            std::string low = a;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            running = low.find("jog") != std::string::npos
                || low.find("dash") != std::string::npos
                || low.find("run") != std::string::npos;
            walking = !running && (low.find("walk") != std::string::npos);
        }

        m_run_blend = m_run_blend + ((running ? 1.0 : 0.0) - m_run_blend) * 0.15;
        m_walk_blend = m_walk_blend + ((walking ? 1.0 : 0.0) - m_walk_blend) * 0.15;
    }

    // [SURGE_BRIDGE] Der Anker uebernimmt den Welt-Versatz der Kamera.
    glm::vec3 anchor_p = cur.rel.empty() ? glm::vec3{} : cur.rel[0].p;
    const bool has_anchor = !cur.rel.empty();
    const double sdx = re4vr::lua_get_number("__vr_surge_dx", 0.0);
    const double sdz = re4vr::lua_get_number("__vr_surge_dz", 0.0);

    if (has_anchor && (sdx != 0.0 || sdz != 0.0)) {
        glm::quat tr{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(tf, "get_Rotation", tr)) {
            const glm::vec3 off = qconj(tr)
                * glm::vec3{static_cast<float>(sdx), 0.0f, static_cast<float>(sdz)};
            anchor_p = glm::vec3{anchor_p.x + off.x, anchor_p.y + off.y, anchor_p.z + off.z};
        }
    }

    // [UB_Z_DELTA] Referenz fuer die Holster-Optik ist das normale Gehen.
    re4vr::lua_set_number("__re4_ub_z_delta", 0.0);

    for (size_t i = 0; i < m_spinepin_joints.size() && i < cur.rel.size(); ++i) {
        const auto& rel = cur.rel[i];
        glm::vec3 p = (i == 0 && has_anchor) ? anchor_p : rel.p;
        const std::string name = i < m_spinepin_names.size() ? m_spinepin_names[i] : std::string{};

        // [PIN_Z] Slider-Offset "nach hinten"; nil, wenn der Name keinen
        // Eintrag hat (das ist bei "root" so).
        std::optional<double> zo{};

        if (name == "Hip") {
            zo = m_cfg.pin_z.Hip;
        } else if (name == "Spine_0") {
            zo = m_cfg.pin_z.Spine_0;
        } else if (name == "Spine_1") {
            zo = m_cfg.pin_z.Spine_1;
        } else if (name == "Spine_2") {
            zo = m_cfg.pin_z.Spine_2;
        } else if (name == "Neck_0") {
            zo = m_cfg.pin_z.Neck_0;
        } else if (name == "Neck_1") {
            zo = m_cfg.pin_z.Neck_1;
        } else if (name == "Head") {
            zo = m_cfg.pin_z.Head;
        }

        // [PIN_X] per-Wirbel Seitwaerts-Offset. pin_x hat KEINEN Hip-Eintrag.
        double xo = 0.0;

        if (name == "Spine_0") {
            xo = m_cfg.pin_x.Spine_0;
        } else if (name == "Spine_1") {
            xo = m_cfg.pin_x.Spine_1;
        } else if (name == "Spine_2") {
            xo = m_cfg.pin_x.Spine_2;
        } else if (name == "Neck_0") {
            xo = m_cfg.pin_x.Neck_0;
        } else if (name == "Neck_1") {
            xo = m_cfg.pin_x.Neck_1;
        } else if (name == "Head") {
            xo = m_cfg.pin_x.Head;
        }

        const bool is_ub = is_ub_joint(name);

        if (is_ub) {
            zo = zo.value_or(0.0) + m_cfg.pin_ub_z;
            xo = xo + m_cfg.pin_ub_x;
        }

        if (name == "Hip") {
            zo = zo.value_or(0.0) * m_run_blend + m_cfg.pin_z_hip_walk * m_walk_blend;
            xo = xo + m_cfg.pin_x_hip;
        } else if (name == "Spine_0") {
            zo = zo.value_or(0.0) - m_cfg.pin_z.Hip * m_run_blend
                - m_cfg.pin_z_hip_walk * m_walk_blend;
            xo = xo - m_cfg.pin_x_hip;
        }

        if ((zo.has_value() && *zo != 0.0) || xo != 0.0) {
            glm::vec3 v{static_cast<float>(xo), 0.0f, static_cast<float>(-zo.value_or(0.0))};

            if (i < cur.par_inv.size() && cur.par_inv[i].has_value()) {
                v = *cur.par_inv[i] * v;
            }

            p = glm::vec3{p.x + v.x, p.y + v.y, p.z + v.z};
        }

        glm::quat r = rel.r;

        // [HIP_YAW] Huefte+Beine um die Hochachse; Spine_0 dreht gegen.
        const double hy = m_cfg.pin_hip_yaw;

        if (hy != 0.0 && (name == "Hip" || name == "Spine_0")) {
            glm::vec3 axis{0.0f, 1.0f, 0.0f};

            if (i < cur.par_inv.size() && cur.par_inv[i].has_value()) {
                axis = *cur.par_inv[i] * axis;
            }

            const double ang = (name == "Hip") ? hy : -hy;
            r = qnorm(axis_quat(axis, deg2rad(ang)) * r);
        }

        // [PIN_UB_YAW] verteilter Twist auf der gepinnten Pose.
        if (is_ub && m_cfg.pin_ub_yaw != 0.0) {
            glm::vec3 axis{0.0f, 1.0f, 0.0f};

            if (i < cur.par_inv.size() && cur.par_inv[i].has_value()) {
                axis = *cur.par_inv[i] * axis;
            }

            r = qnorm(axis_quat(axis, deg2rad(m_cfg.pin_ub_yaw)) * r);
        }

        // [PIN_ROLL] Spine_1 um die Transform-Vorwaertsachse kippen.
        if (name == "Spine_1" && m_cfg.pin_spine1_roll != 0.0) {
            glm::vec3 axis{0.0f, 0.0f, 1.0f};

            if (i < cur.par_inv.size() && cur.par_inv[i].has_value()) {
                axis = *cur.par_inv[i] * axis;
            }

            r = qnorm(axis_quat(axis, deg2rad(m_cfg.pin_spine1_roll)) * r);
        }

        // [ROOMSCALE-HOEHE 16.09.2026] Mit Roomscale traegt das Bein-IK die Hoehe.
        // Gemessen (zzz_re4_schweben_probe.lua, 269 Zeilen): Hip stand IMMER exakt
        // 0,929 ueber dem Transform, egal wie tief der Kopf war -- und die Fuesse
        // gingen dafuer bis 1,64 m HOCH. Das IK senkt die Huefte und beugt die
        // Beine, dieser Pin setzte die Huefte danach zurueck, und die gebeugten
        // Beine (Kinder der Huefte) hingen mit oben: verknotete Beine, Schweben.
        // Darum hier bei root und Hip nur X/Z pinnen und die Hoehe so lassen, wie
        // Animation und IK sie gerade gesetzt haben. Rotationen und alle anderen
        // Joints bleiben unveraendert gepinnt. Ohne Roomscale: wie bisher.
        if (m_cfg.roomscale && (name == "root" || name == "Hip")) {
            glm::vec3 cur_lp{};

            if (get_vec3(m_spinepin_joints[i].obj, "get_LocalPosition", cur_lp)) {
                p.y = cur_lp.y;
            }
        }

        set_vec3(m_spinepin_joints[i].obj, "set_LocalPosition", p);
        set_quat(m_spinepin_joints[i].obj, "set_LocalRotation", r);
    }

    // Null_Offset separat festnageln.
    if (m_spinepin_null_off.obj != nullptr && cur.null_rel.has_value()) {
        set_vec3(m_spinepin_null_off.obj, "set_LocalPosition", cur.null_rel->p);
        set_quat(m_spinepin_null_off.obj, "set_LocalRotation", cur.null_rel->r);
    }
}

// ============================================================================
// FSM-Eingriffe (Lua Z.909-1055)
//
// Die Lua-Fassung benutzt REFrameworks Baum-Bindings; dahinter stehen dieselben
// nativen Klassen (sdk::behaviortree::TreeObject/TreeNode, sdk::MotionFsm2Layer),
// die hier direkt verwendet werden.
//
// INDEX-FALLE: Die sol-Bindung von NativeArrayNoCapacity ist NULL-basiert
// (`sol::meta_function::index` reicht den Index unveraendert an `arr[i]`
// durch, Sdk.cpp). Luas `states[1]` ist also NATIV Index 1, nicht 0.
// `get_children()`/`get_actions()` liefern dagegen std::vector, das sol als
// EINS-basierte Lua-Tabelle zeigt -- dort ist Luas `[2]` der native Index 1.
// ============================================================================

::REManagedObject* RE4VRMovement::get_motion_fsm(::REManagedObject** out_ctx) {
    if (out_ctx != nullptr) {
        *out_ctx = nullptr;
    }

    if (m_character_manager.obj == nullptr || !re4vr::obj_ok(m_character_manager.obj)) {
        keep(re4vr::character_manager(), m_character_manager);
    }

    if (m_character_manager.obj == nullptr) {
        return nullptr;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(m_character_manager.obj, "getPlayerContextRef");

    if (ctx == nullptr) {
        return nullptr;
    }

    if (out_ctx != nullptr) {
        *out_ctx = ctx;
    }

    auto def = utility::re_managed_object::get_type_definition(ctx);

    if (def == nullptr) {
        return nullptr;
    }

    auto* f_bu = def->get_field("_BodyUpdater");

    if (f_bu == nullptr) {
        return nullptr;
    }

    ::REManagedObject* updater = nullptr;

    try {
        auto* raw = f_bu->get_data_raw(ctx, false);
        updater = raw != nullptr ? *reinterpret_cast<::REManagedObject**>(raw) : nullptr;
    } catch (...) {
        updater = nullptr;
    }

    if (updater == nullptr) {
        return nullptr;
    }

    auto udef = utility::re_managed_object::get_type_definition(updater);

    if (udef == nullptr) {
        return nullptr;
    }

    auto* f_fsm = udef->get_field("<MotionFsm>k__BackingField");

    if (f_fsm == nullptr) {
        return nullptr;
    }

    try {
        auto* raw = f_fsm->get_data_raw(updater, false);
        return raw != nullptr ? *reinterpret_cast<::REManagedObject**>(raw) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

namespace {

// mfsm:call("getLayer", 0) -> via.motion.MotionFsm2Layer -> get_tree_object()
sdk::behaviortree::TreeObject* fsm_tree(::REManagedObject* mfsm) {
    if (mfsm == nullptr) {
        return nullptr;
    }

    auto* layer = re4vr::call_safe<sdk::MotionFsm2Layer*>(mfsm, "getLayer", 0);

    if (layer == nullptr) {
        return nullptr;
    }

    return layer->get_tree_object();
}

// Ein Zahlenfeld eines Managed Objects lesen (float). Luas `act._StartFrame`.
std::optional<double> act_get_num(::REManagedObject* act, const char* name) {
    if (act == nullptr) {
        return std::nullopt;
    }

    auto def = utility::re_managed_object::get_type_definition(act);

    if (def == nullptr) {
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

bool act_set_num(::REManagedObject* act, const char* name, double v) {
    if (act == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(act);

    if (def == nullptr) {
        return false;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return false;
    }

    auto* ft = f->get_type();

    if (ft == nullptr) {
        return false;
    }

    try {
        auto* raw = f->get_data_raw(act, false);

        if (raw == nullptr) {
            return false;
        }

        const auto sz = ft->get_valuetype_size();

        if (sz == 4) {
            *reinterpret_cast<float*>(raw) = static_cast<float>(v);
            return true;
        }

        if (sz == 8) {
            *reinterpret_cast<double*>(raw) = v;
            return true;
        }
    } catch (...) {
    }

    return false;
}

void act_set_bool(::REManagedObject* act, const char* name, bool v) {
    if (act == nullptr) {
        return;
    }

    auto def = utility::re_managed_object::get_type_definition(act);

    if (def == nullptr) {
        return;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return;
    }

    try {
        auto* raw = f->get_data_raw(act, false);

        if (raw != nullptr) {
            *reinterpret_cast<uint8_t*>(raw) = v ? 1 : 0;
        }
    } catch (...) {
    }
}

} // namespace

// Lua Z.966-1002.
bool RE4VRMovement::apply_skip_nodes(const std::vector<const char*>& nodes, double frames,
                                     bool enable, bool overwrite_interp,
                                     ::REManagedObject** out_ctx) {
    ::REManagedObject* ctx = nullptr;
    auto* mfsm = get_motion_fsm(&ctx);

    if (out_ctx != nullptr) {
        *out_ctx = ctx;
    }

    if (mfsm == nullptr) {
        if (out_ctx != nullptr) {
            *out_ctx = nullptr;   // Lua liefert hier `false, nil`
        }

        return false;
    }

    auto* tree = fsm_tree(mfsm);

    if (tree == nullptr) {
        if (out_ctx != nullptr) {
            *out_ctx = nullptr;
        }

        return false;
    }

    bool touched = false;

    for (const char* name : nodes) {
        auto* node = tree->get_node_by_name(std::string_view{name});

        if (node == nullptr) {
            continue;
        }

        auto acts = node->get_actions();

        // Luas `if not (acts and acts[1]) then acts = get_unloaded_actions() end`
        if (acts.empty()) {
            acts = node->get_unloaded_actions();
        }

        for (auto* act : acts) {
            const auto sf = act_get_num(act, "_StartFrame");

            if (!sf.has_value()) {
                continue;
            }

            if (enable) {
                // [COLLAPSE] StartFrame ans Ende der Anim; Rueckfall = fester
                // Frameskip, falls _EndFrame fehlt.
                const auto ef = act_get_num(act, "_EndFrame");
                const double val = (ef.has_value() && *ef > 0.0) ? *ef : frames;
                act_set_num(act, "_StartFrame", val);
            } else {
                act_set_num(act, "_StartFrame", 0.0);
            }

            act_set_bool(act, "_OverwriteInterpolation", enable && overwrite_interp);
            touched = true;
        }
    }

    return touched;
}

bool RE4VRMovement::apply_stop_skip(bool enable) {
    ::REManagedObject* ctx = nullptr;
    const bool touched = apply_skip_nodes(STOP_NODES, m_cfg.stop_skip_frames, enable, true, &ctx);

    if (touched) {
        m_stop_skip.applied = enable;
        m_stop_skip.last_ctx = ctx;
    }

    return touched;
}

bool RE4VRMovement::apply_start_skip(bool enable) {
    // Walk-Starts: Skip + Re-Interpolation.
    ::REManagedObject* dummy = nullptr;
    const bool t_walk =
        apply_skip_nodes(START_NODES, m_cfg.start_skip_frames, enable, true, &dummy);
    // Run-Starts: Skip OHNE _OverwriteInterpolation -> kein Halt beim Walk->Run.
    ::REManagedObject* ctx = nullptr;
    const bool t_run =
        apply_skip_nodes(RUN_START_NODES, m_cfg.start_skip_frames, enable, false, &ctx);
    const bool touched = t_walk || t_run;

    if (touched) {
        m_start_skip.applied = enable;
        m_start_skip.last_ctx = ctx;
    }

    return touched;
}

// Lua Z.1012-1055.
bool RE4VRMovement::apply_no_pivot(bool disable) {
    ::REManagedObject* ctx = nullptr;
    auto* mfsm = get_motion_fsm(&ctx);

    if (mfsm == nullptr) {
        return false;
    }

    auto* tree = fsm_tree(mfsm);

    if (tree == nullptr) {
        return false;
    }

    bool ok = false;

    try {
        auto* jog = tree->get_node_by_name(std::string_view{"JogLoop"});

        if (jog != nullptr) {
            auto* data = jog->get_data();

            if (data != nullptr) {
                auto& states = data->get_states();

                // [INDEX] Luas `states[1]` ist NATIV Index 1 (die sol-Bindung
                // reicht den Index unveraendert durch). `states[1]` ist genau
                // dann nicht-nil, wenn size() > 1 ist.
                while (disable && states.size() > 1) {
                    states.erase(1);
                }

                while (!disable && states.size() <= 1) {
                    states.emplace();   // Luas states:emplace(0) -> fix_pointers=false
                }

                if (states.size() > 1) {
                    states[1] = 109;    // JogTurn-Node (alphaZomega-Wert)
                }

                ok = true;
            }
        }
    } catch (...) {
        ok = false;
    }

    // Jog-STEERING toeten: DampingAngle-Action am JogLoop-Child.
    try {
        auto* jog = tree->get_node_by_name(std::string_view{"JogLoop"});

        if (jog != nullptr) {
            const auto children = jog->get_children();

            // Luas `get_children()[2]` -- std::vector kommt als EINS-basierte
            // Lua-Tabelle, das zweite Kind ist nativ Index 1.
            if (children.size() > 1 && children[1] != nullptr) {
                auto* jloop = children[1];
                auto acts = jloop->get_actions();

                if (acts.empty()) {
                    acts = jloop->get_unloaded_actions();
                }

                if (!acts.empty() && acts[0] != nullptr) {
                    auto* turner = acts[0];
                    auto tdef = utility::re_managed_object::get_type_definition(turner);

                    if (tdef != nullptr) {
                        if (auto* f = tdef->get_field("<DampingAngle>k__BackingField")) {
                            auto* ft = f->get_type();
                            auto* raw = f->get_data_raw(turner, false);

                            // Waere das Feld ein WERTtyp, bekaeme Lua eine Kopie
                            // und der Write darauf waere wirkungslos -- dann hier
                            // ebenfalls nichts tun.
                            if (raw != nullptr && ft != nullptr && !ft->is_value_type()) {
                                auto* dmp = *reinterpret_cast<::REManagedObject**>(raw);

                                if (dmp != nullptr) {
                                    act_set_num(dmp, "_DampingTime", disable ? 99.0 : 0.5);
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
    }

    if (ok) {
        m_no_pivot.applied = disable;
        m_no_pivot.last_ctx = ctx;
    }

    return ok;
}

// Lua Z.1174-1191.
void RE4VRMovement::update_stop_skip() {
    // [STILLZONE] In der Zone NICHT nachtragen.
    if (mv_in_zone()) {
        return;
    }

    ::REManagedObject* ctx = nullptr;
    get_motion_fsm(&ctx);

    if (ctx == nullptr) {
        return;
    }

    if (m_cfg.stop_skip && ctx != m_stop_skip.last_ctx) {
        apply_stop_skip(true);
    }

    if (m_cfg.start_skip && ctx != m_start_skip.last_ctx) {
        apply_start_skip(true);
    }

    if (m_cfg.no_pivot && ctx != m_no_pivot.last_ctx) {
        apply_no_pivot(true);
    }
}

// ============================================================================
// Stillzone (Lua Z.1122-1172)
// ============================================================================

bool RE4VRMovement::mv_in_zone() const {
    // Das Signal kommt aus re4_vr_motion.lua. Fehlt es, laeuft alles wie immer.
    return re4vr::lua_get_bool("__re4_stillzone_movement", false);
}

void RE4VRMovement::mv_zone_tick() {
    const bool drin = mv_in_zone();

    if (drin && !m_zone_prev) {
        // BETRETEN: die drei FSM-Eingriffe aktiv zurueckschreiben.
        m_zone_t = now_clock();

        if (m_cfg.stop_skip) {
            apply_stop_skip(false);
        }

        if (m_cfg.start_skip) {
            apply_start_skip(false);
        }

        if (m_cfg.no_pivot) {
            apply_no_pivot(false);
        }
    } else if (!drin && m_zone_prev) {
        // VERLASSEN: nur wiederherstellen, was eingeschaltet geblieben ist.
        m_zone_t.reset();

        if (m_cfg.stop_skip) {
            apply_stop_skip(true);
        }

        if (m_cfg.start_skip) {
            apply_start_skip(true);
        }

        if (m_cfg.no_pivot) {
            apply_no_pivot(true);
        }
    }

    m_zone_prev = drin;

    // [M5] Lua schreibt diese drei in JEDEM Tick fort (Z.1096/1105/1111), nicht
    // nur einmal beim Veroeffentlichen.
    re4vr::lua_set_bool("__re4_mv_zone_prev", drin);

    if (m_zone_t.has_value()) {
        re4vr::lua_set_number("__re4_mv_zone_t", *m_zone_t);
    } else {
        re4vr::lua_set_nil("__re4_mv_zone_t");
    }

    // FRISCHE POSE waehrend des Fensters.
    if (drin && m_zone_t.has_value() && (now_clock() - *m_zone_t) < m_zone_win) {
        auto* tf = get_body_transform();

        if (tf != nullptr) {
            re4vr::call_safe<void*>(tf, "resetBasePose");
        }
    }
}

// ============================================================================
// Anim-Export (Lua Z.1195-1221)
// ============================================================================

void RE4VRMovement::update_anim_export() {
    auto* tf = get_body_transform();

    if (tf == nullptr) {
        re4vr::lua_set_nil("__vr_anim_l0");
        drop(m_bw_motion);
        return;
    }

    if (m_bw_motion.obj == nullptr || !re4vr::obj_ok(m_bw_motion.obj)) {
        auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

        if (go != nullptr) {
            keep(re4vr::get_component(go, "via.motion.Motion"), m_bw_motion);
        }
    }

    if (m_bw_motion.obj == nullptr) {
        re4vr::lua_set_nil("__vr_anim_l0");
        return;
    }

    std::string name{};
    auto* layer = re4vr::call_safe<::REManagedObject*>(m_bw_motion.obj, "getLayer", 0);

    if (layer != nullptr) {
        auto* node = re4vr::call_safe<::REManagedObject*>(layer, "get_HighestWeightMotionNode");

        if (node != nullptr) {
            // get_MotionName liefert ein managed System.String.
            auto* n = re4vr::call_safe<::REManagedObject*>(node, "get_MotionName");

            if (n != nullptr) {
                try {
                    name = utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
                } catch (...) {
                    name.clear();
                }
            }
        }
    }

    if (name.empty()) {
        re4vr::lua_set_nil("__vr_anim_l0");
        // Zweite Sicherung: Transform gueltig, gecachte Motion tot.
        drop(m_bw_motion);
    } else {
        re4vr::lua_set_string("__vr_anim_l0", name);
    }
}

// ============================================================================
// UB-Lock / Spine-Yaw (Lua Z.1240-1332)
// ============================================================================

void RE4VRMovement::apply_ub_lock(bool can_capture) {
    if (!m_cfg.ub_lock) {
        m_ub_lock_pose.reset();
        m_ub_lock_rel.reset();
        m_ub_lock_base.reset();
        return;
    }

    if (is_ks_active()) {
        return;
    }

    // [STILLZONE] Pose-Slots bewusst NICHT nilen.
    if (mv_in_zone()) {
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    if (m_cfg.ub_lock_world) {
        // [WELT-MODUS] Welt-Rotation absolut erzwingen.
        glm::quat brot{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat(tf, "get_Rotation", brot)) {
            return;
        }

        const auto byaw = flat_yaw_of(brot);

        if (!byaw.has_value()) {
            return;
        }

        const glm::quat bq = yaw_to_quat(*byaw);

        if (!m_ub_lock_rel.has_value()) {
            if (!can_capture) {
                return;
            }

            const glm::quat inv = qconj(bq);
            std::map<std::string, glm::quat> rel{};

            for (const char* name : SPINE_YAW_JOINTS) {
                auto* j = get_spine_joint(tf, name);

                if (j == nullptr) {
                    return;
                }

                glm::quat w{1.0f, 0.0f, 0.0f, 0.0f};

                if (!get_quat(j, "get_Rotation", w)) {
                    return;
                }

                rel[name] = qnorm(inv * w);
            }

            m_ub_lock_rel = rel;
        }

        const double trim = m_cfg.ub_trim_deg;
        std::optional<glm::quat> tq{};

        if (trim != 0.0) {
            const double half = deg2rad(trim) * 0.5;
            tq = glm::quat{static_cast<float>(std::cos(half)), 0.0f,
                           static_cast<float>(std::sin(half)), 0.0f};
        }

        for (const char* name : SPINE_YAW_JOINTS) {
            auto* j = get_spine_joint(tf, name);
            const auto it = m_ub_lock_rel->find(name);

            if (j != nullptr && it != m_ub_lock_rel->end()) {
                const glm::quat tgt =
                    tq.has_value() ? qnorm(bq * *tq * it->second) : qnorm(bq * it->second);
                set_quat(j, "set_Rotation", tgt);
            }
        }

        return;
    }

    // Lazy-Capture beim (Re-)Aktivieren: aktuelle LOKALE Pose einfrieren.
    if (!m_ub_lock_pose.has_value()) {
        if (!can_capture) {
            return;
        }

        std::map<std::string, glm::quat> pose{};
        bool complete = true;

        for (const char* name : SPINE_YAW_JOINTS) {
            auto* j = get_spine_joint(tf, name);
            glm::quat lr{1.0f, 0.0f, 0.0f, 0.0f};

            if (j != nullptr && get_quat(j, "get_LocalRotation", lr)) {
                pose[name] = lr;
            } else {
                complete = false;
            }
        }

        if (!complete) {
            return;
        }

        // Trim-Basis am ERSTEN Joint: ganzer Torso dreht als Block.
        const char* first = SPINE_YAW_JOINTS[0];
        auto* j1 = get_spine_joint(tf, first);
        glm::quat w1{1.0f, 0.0f, 0.0f, 0.0f};

        if (j1 == nullptr || !get_quat(j1, "get_Rotation", w1)) {
            return;
        }

        auto* p1j = re4vr::call_safe<::REManagedObject*>(j1, "get_Parent");
        glm::quat p1{1.0f, 0.0f, 0.0f, 0.0f};

        if (p1j == nullptr || !get_quat(p1j, "get_Rotation", p1)) {
            return;
        }

        m_ub_lock_pose = pose;
        UbBase b{};
        b.raw = pose[first];
        b.parent = p1;
        b.world = w1;
        m_ub_lock_base = b;
        m_ub_trim_applied.reset();
    }

    // [UB_TRIM] Re-Bake nur bei Slider-Aenderung.
    const double trim = m_cfg.ub_trim_deg;

    if ((!m_ub_trim_applied.has_value() || *m_ub_trim_applied != trim)
        && m_ub_lock_base.has_value()) {
        const char* first = SPINE_YAW_JOINTS[0];

        if (trim == 0.0) {
            (*m_ub_lock_pose)[first] = m_ub_lock_base->raw;
            m_ub_trim_applied = trim;
        } else {
            const double half = deg2rad(trim) * 0.5;
            const glm::quat y{static_cast<float>(std::cos(half)), 0.0f,
                              static_cast<float>(std::sin(half)), 0.0f};
            const glm::quat nl = qnorm(qconj(m_ub_lock_base->parent) * y * m_ub_lock_base->world);
            (*m_ub_lock_pose)[first] = nl;
            m_ub_trim_applied = trim;
        }
    }

    for (const char* name : SPINE_YAW_JOINTS) {
        auto* j = get_spine_joint(tf, name);
        const auto it = m_ub_lock_pose->find(name);

        if (j != nullptr && it != m_ub_lock_pose->end()) {
            set_quat(j, "set_LocalRotation", it->second);
        }
    }
}

// Lua Z.1311-1332.
void RE4VRMovement::apply_spine_yaw(::REManagedObject* tf) {
    const double deg = m_cfg.spine_yaw_deg;

    if (deg == 0.0 || tf == nullptr) {
        return;
    }

    const double half = deg2rad(deg) * 0.5;
    const glm::quat yaw_off{static_cast<float>(std::cos(half)), 0.0f,
                            static_cast<float>(std::sin(half)), 0.0f};

    for (const char* name : SPINE_YAW_JOINTS) {
        auto* j = get_spine_joint(tf, name);

        if (j == nullptr) {
            continue;
        }

        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat(j, "get_Rotation", r)) {
            continue;
        }

        // Anti-Akkumulation: wenn die Rotation noch unsere eigene letzte
        // Schreibung ist, NICHT erneut draufrechnen.
        const auto it = m_spine_last_written.find(name);
        const bool same = it != m_spine_last_written.end() && quat_approx_equal(r, it->second);

        if (!same) {
            const glm::quat nr = qnorm(yaw_off * r);
            set_quat(j, "set_Rotation", nr);
            m_spine_last_written[name] = nr;
        }
    }
}

// ============================================================================
// Crouch-UB-Z (Lua Z.1347-1400)
// ============================================================================

void RE4VRMovement::apply_crouch_ub_z(bool capture_base) {
    if (!m_cfg.spine_pin) {
        return;
    }

    // Rigide Crouch-Pose gepinnt? Dann hat apply_crouch_pin das Sagen.
    if (m_crouchpin_pose.valid) {
        return;
    }

    const double z = m_cfg.pin_ub_z_crouch;

    if (z == 0.0) {
        m_crouch_ubz_base.clear();
        m_crouch_ubz_base_valid = false;
        return;
    }

    if (is_ks_active()) {
        return;
    }

    if (!is_crouch_active()) {
        m_crouch_ubz_base.clear();
        m_crouch_ubz_base_valid = false;
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    if (!spinepin_resolve(tf)) {
        return;
    }

    if (m_spinepin_joints.empty() || m_spinepin_names.empty()) {
        return;
    }

    glm::quat tr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", tr)) {
        return;
    }

    const glm::quat tri = qconj(tr);

    // Native UB-Basis 1x/Frame einsammeln (post-anim -> echte Hock-Pose).
    if (capture_base) {
        m_crouch_ubz_base.clear();

        for (size_t i = 0; i < m_spinepin_names.size() && i < m_spinepin_joints.size(); ++i) {
            if (is_ub_joint(m_spinepin_names[i])) {
                glm::vec3 lp{};

                if (get_vec3(m_spinepin_joints[i].obj, "get_LocalPosition", lp)) {
                    m_crouch_ubz_base[static_cast<int>(i)] = lp;
                }
            }
        }

        m_crouch_ubz_base_valid = true;
    }

    if (!m_crouch_ubz_base_valid) {
        return;
    }

    for (size_t i = 0; i < m_spinepin_names.size() && i < m_spinepin_joints.size(); ++i) {
        if (!is_ub_joint(m_spinepin_names[i])) {
            continue;
        }

        const auto it = m_crouch_ubz_base.find(static_cast<int>(i));

        if (it == m_crouch_ubz_base.end()) {
            continue;
        }

        // transform-Z "nach hinten" in den Parent-Raum des Joints drehen.
        glm::vec3 v{0.0f, 0.0f, static_cast<float>(-z)};

        if (i >= 1) {
            auto* parent = m_spinepin_joints[i - 1].obj;
            glm::quat pw{1.0f, 0.0f, 0.0f, 0.0f};

            if (parent != nullptr && get_quat(parent, "get_Rotation", pw)) {
                v = qconj(qnorm(tri * pw)) * v;
            }
        }

        const glm::vec3 p = it->second;
        set_vec3(m_spinepin_joints[i].obj, "set_LocalPosition",
                 glm::vec3{p.x + v.x, p.y + v.y, p.z + v.z});
    }
}

// ============================================================================
// Crouch-Pin (Lua Z.1414-1553)
// ============================================================================

void RE4VRMovement::apply_crouch_pin(bool can_capture) {
    if (!m_cfg.spine_pin) {
        return;   // Master aus: Pose-Slot behalten
    }

    if (is_ks_active()) {
        return;
    }

    if (is_pin_release()) {
        return;
    }

    if (!is_crouch_active()) {
        // Crouch verlassen: Pose-Slot BEHALTEN, nur den Capture-Wunsch verwerfen.
        m_crouchpin_capture_req = false;
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    // Spawn-Guard wie beim Steh-Pin.
    if (!m_spinepin_joints.empty()) {
        glm::vec3 probe{};

        if (!get_vec3(m_spinepin_joints[0].obj, "get_Position", probe)) {
            m_spinepin_tf = nullptr;

            for (auto& h : m_spinepin_joints) {
                drop(h);
            }

            m_spinepin_joints.clear();
            m_spinepin_names.clear();
            drop(m_spinepin_null_off);
        }
    }

    if (!spinepin_resolve(tf)) {
        return;
    }

    if (m_spinepin_joints.empty() || m_spinepin_names.empty()) {
        return;
    }

    // Capture-Wunsch (Button): Enforce diesen Frame AUSSETZEN.
    if (m_crouchpin_capture_req) {
        if (can_capture) {
            Pose p{};

            if (spinepin_capture(tf, p)) {
                m_crouchpin_pose = p;
                m_crouchpin_capture_req = false;
                spinepin_store(p, "crouch_pose");
            }
        }

        return;
    }

    // Persistierte Hock-Pose 1x pro Script-Lauf von Platte ziehen.
    if (!m_crouchpin_pose.valid) {
        if (!m_crouchpin_cfg_tried) {
            m_crouchpin_cfg_tried = true;
            Pose p{};

            if (spinepin_restore("crouch_pose", p)) {
                m_crouchpin_pose = p;
            }
        }

        if (!m_crouchpin_pose.valid) {
            // Nichts gepinnt -> native Crouch, Surge gehoert auf die Kamera.
            re4vr::lua_set_bool("__vr_surge_bridged", false);
            return;
        }
    }

    const Pose& cur = m_crouchpin_pose;
    const double ubz = m_cfg.pin_ub_z_crouch;

    // [UB_Z_DELTA] Referenz = Steh-Pin. Reines Info-Global fuer holster.
    re4vr::lua_set_number("__re4_ub_z_delta", ubz - m_cfg.pin_ub_z);

    // [HIP_CROUCH_Z] eigener Wert; solange nie gesetzt (nil), gilt pin_z.Hip.
    const double hipz = m_cfg.pin_z_hip_crouch.value_or(m_cfg.pin_z.Hip);

    // [SURGE_BRIDGE] wie im Steh-Pin.
    re4vr::lua_set_bool("__vr_surge_bridged", true);

    glm::vec3 anchor_p = cur.rel.empty() ? glm::vec3{} : cur.rel[0].p;
    const bool has_anchor = !cur.rel.empty();
    const double sdx = re4vr::lua_get_number("__vr_surge_dx", 0.0);
    const double sdz = re4vr::lua_get_number("__vr_surge_dz", 0.0);

    if (has_anchor && (sdx != 0.0 || sdz != 0.0)) {
        glm::quat tr{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(tf, "get_Rotation", tr)) {
            const glm::vec3 off = qconj(tr)
                * glm::vec3{static_cast<float>(sdx), 0.0f, static_cast<float>(sdz)};
            anchor_p = glm::vec3{anchor_p.x + off.x, anchor_p.y + off.y, anchor_p.z + off.z};
        }
    }

    for (size_t i = 0; i < m_spinepin_joints.size() && i < cur.rel.size(); ++i) {
        const auto& rel = cur.rel[i];
        glm::vec3 p = (i == 0 && has_anchor) ? anchor_p : rel.p;
        const std::string name = i < m_spinepin_names.size() ? m_spinepin_names[i] : std::string{};

        double zo = 0.0;
        double xo = 0.0;

        if (is_ub_joint(name)) {
            zo += ubz;
            xo += m_cfg.pin_ub_x_crouch;
        }

        if (name == "Hip") {
            zo += hipz;
        } else if (name == "Spine_0") {
            zo -= hipz;
        }

        if (zo != 0.0 || xo != 0.0) {
            glm::vec3 v{static_cast<float>(xo), 0.0f, static_cast<float>(-zo)};

            if (i < cur.par_inv.size() && cur.par_inv[i].has_value()) {
                v = *cur.par_inv[i] * v;
            }

            p = glm::vec3{p.x + v.x, p.y + v.y, p.z + v.z};
        }

        // [ROOMSCALE-HOEHE 16.09.2026] Mit Roomscale traegt das Bein-IK die Hoehe.
        // Gemessen (zzz_re4_schweben_probe.lua, 269 Zeilen): Hip stand IMMER exakt
        // 0,929 ueber dem Transform, egal wie tief der Kopf war -- und die Fuesse
        // gingen dafuer bis 1,64 m HOCH. Das IK senkt die Huefte und beugt die
        // Beine, dieser Pin setzte die Huefte danach zurueck, und die gebeugten
        // Beine (Kinder der Huefte) hingen mit oben: verknotete Beine, Schweben.
        // Darum hier bei root und Hip nur X/Z pinnen und die Hoehe so lassen, wie
        // Animation und IK sie gerade gesetzt haben. Rotationen und alle anderen
        // Joints bleiben unveraendert gepinnt. Ohne Roomscale: wie bisher.
        if (m_cfg.roomscale && (name == "root" || name == "Hip")) {
            glm::vec3 cur_lp{};

            if (get_vec3(m_spinepin_joints[i].obj, "get_LocalPosition", cur_lp)) {
                p.y = cur_lp.y;
            }
        }

        set_vec3(m_spinepin_joints[i].obj, "set_LocalPosition", p);
        set_quat(m_spinepin_joints[i].obj, "set_LocalRotation", rel.r);
    }

    if (m_spinepin_null_off.obj != nullptr && cur.null_rel.has_value()) {
        set_vec3(m_spinepin_null_off.obj, "set_LocalPosition", cur.null_rel->p);
        set_quat(m_spinepin_null_off.obj, "set_LocalRotation", cur.null_rel->r);
    }
}

// ============================================================================
// Harter Yaw-Follow (Lua Z.1556-1827)
//
// [CBR_DIAG] Die Diagnosefunktion (Lua Z.1567) beginnt mit `do return end` --
// ihr ganzer Rumpf ist toter Code und wird nicht mitportiert. Ihr einziger
// Aufrufer (Z.1613) bleibt damit wirkungslos, genau wie im Original.
// ============================================================================

namespace {

// via.mat4 ist 64 Byte -> sret mit 16-Byte-ausgerichtetem Puffer.
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

// Ein Quaternion-Feld eines Managed Objects lesen (Luas get_field).
bool get_quat_field(::REManagedObject* obj, const char* name, glm::quat& out) {
    if (obj == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return false;
    }

    try {
        auto* raw = f->get_data_raw(obj, false);

        if (raw == nullptr) {
            return false;
        }

        out = *reinterpret_cast<glm::quat*>(raw);
        return true;
    } catch (...) {
        return false;
    }
}

bool set_quat_field(::REManagedObject* obj, const char* name, const glm::quat& v) {
    if (obj == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    auto* f = def->get_field(name);

    if (f == nullptr) {
        return false;
    }

    try {
        auto* raw = f->get_data_raw(obj, false);

        if (raw == nullptr) {
            return false;
        }

        *reinterpret_cast<glm::quat*>(raw) = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool hmd_active() {
    auto* vr = VR::get().get();
    return vr != nullptr && vr->is_hmd_active();
}

} // namespace

// Lua Z.1612-1806.
void RE4VRMovement::apply_hard_yaw(bool sample) {
    // Lua Z.1613 ruft hier cbr_diag() -- toter Rumpf, entfaellt.

    if (!m_cfg.enabled) {
        m_owned_yaw.reset();
        return;
    }

    if (is_ks_active()) {
        m_last_yaw_quat.reset();
        m_body_yaw_last_t.reset();
        m_owned_yaw.reset();
        return;
    }

    if (!hmd_active()) {
        m_owned_yaw.reset();
        return;
    }

    // Avancieren nur 1x/Frame (LockScene-pre); Re-Applies laufen separat.
    if (!sample) {
        return;
    }

    // [AIM_NATIVE] Beim Zielen richtet die ENGINE den Body selbst aus.
    if (re4vr::lua_get_bool("is_aim", false)) {
        m_body_yaw_last_t.reset();
        m_owned_yaw.reset();
        return;
    }

    auto* pcc = get_player_cam_controller();

    if (pcc == nullptr) {
        m_last_yaw_quat.reset();
        m_body_yaw_last_t.reset();
        m_owned_yaw.reset();
        return;
    }

    // [CAM_BODY_ROTATE] Beim Start unerreichbar (Lua Z.254 setzt den Schalter
    // hart auf false), per UI aber reaktivierbar -- deshalb vollstaendig
    // portiert.
    if (m_cfg.cam_body_rotate) {
        auto* tf = get_body_transform();

        if (tf == nullptr) {
            return;
        }

        glm::quat body_rot{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat(tf, "get_Rotation", body_rot)) {
            return;
        }

        const auto body_yaw = flat_yaw_of(body_rot);

        if (!body_yaw.has_value()) {
            return;
        }

        if (!m_owned_yaw.has_value()) {
            m_owned_yaw = *body_yaw;
        }

        auto* cam = sdk::get_primary_camera();
        glm::mat4 wm{1.0f};

        if (cam == nullptr || !get_mat4(cam, "get_WorldMatrix", wm)) {
            return;
        }

        // wm[2] = Kamera-+Z (Backward); Body-Forward ist 180 Grad dazu.
        double tfx = -static_cast<double>(wm[2].x);
        double tfz = -static_cast<double>(wm[2].z);
        const double tlen = std::sqrt(tfx * tfx + tfz * tfz);

        if (tlen < 0.0001) {
            return;
        }

        tfx /= tlen;
        tfz /= tlen;

        double target_yaw = std::atan2(tfx, tfz);
        const auto hy = hmd_raw_yaw();

        if (hy.has_value()) {
            target_yaw = target_yaw + *hy;   // [WATCH] Vorzeichen
        }

        if (m_cfg.yaw_offset_deg != 0.0) {
            target_yaw = target_yaw + deg2rad(m_cfg.yaw_offset_deg);
        }

        tfx = std::sin(target_yaw);
        tfz = std::cos(target_yaw);

        const double now = now_clock();
        double dt = 0.011;

        if (m_body_yaw_last_t.has_value()) {
            dt = now - *m_body_yaw_last_t;

            if (dt <= 0.0) {
                dt = 0.011;
            } else if (dt > 0.1) {
                dt = 0.1;
            }
        }

        m_body_yaw_last_t = now;

        const double pfx = std::sin(*m_owned_yaw);
        const double pfz = std::cos(*m_owned_yaw);

        const double dot = pfx * tfx + pfz * tfz;
        const double crossY = pfz * tfx - pfx * tfz;
        const double angle = std::atan2(crossY, dot);
        const double deg = rad2deg(angle);

        if (std::abs(deg) >= 0.001) {
            const double perpx = pfz;
            const double perpz = -pfx;
            const double s = (angle > 0.0) ? 1.0 : -1.0;
            const double prx = -s * perpx;
            const double prz = -s * perpz;
            const double k = (std::abs(deg) / 720.0) * dt * m_cfg.body_yaw_speed;
            const double nfx = pfx + (pfx - prx) * k;
            const double nfz = pfz + (pfz - prz) * k;
            const double nlen = std::sqrt(nfx * nfx + nfz * nfz);

            if (nlen >= 0.00001) {
                m_owned_yaw = wrap_pi(std::atan2(nfx / nlen, nfz / nlen));
            }
        }

        m_last_yaw_quat = yaw_to_quat(*m_owned_yaw);

        // Erzwingen nur bei Engine-Abweichung (Tippel-Fix).
        const double diff = wrap_pi(*m_owned_yaw - *body_yaw);
        const glm::quat q = yaw_to_quat(*m_owned_yaw);

        if (std::abs(diff) >= deg2rad(BODY_YAW_EPS_DEG)) {
            set_quat(tf, "set_Rotation", q);
        }

        apply_hip_follow(tf, q);
        return;
    }

    // ---- Legacy-Pfad: NUR LESEN -------------------------------------------
    // [PIN_1TO1] Solange der Spine-Pin steht: Ziel IMMER inkl. HMD und Snap
    // statt Lerp, unabhaengig von den Reglern.
    const bool pin_hard = m_cfg.spine_pin && m_spinepin_pose.valid;
    glm::vec3 fwd{};

    if (m_cfg.yaw_hmd_target || pin_hard) {
        auto* cam = sdk::get_primary_camera();
        glm::mat4 wm{1.0f};

        if (cam == nullptr || !get_mat4(cam, "get_WorldMatrix", wm)) {
            return;
        }

        // wm[2] = Kamera-+Z (Backward) -- gleiche Konvention wie cam_rot*(0,0,1).
        fwd = glm::vec3{wm[2].x, 0.0f, wm[2].z};
    } else {
        glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat_field(pcc, "_CameraRotation", cam_rot)) {
            return;
        }

        fwd = cam_rot * glm::vec3{0.0f, 0.0f, 1.0f};
        fwd.y = 0.0f;
    }

    // Flatten auf Yaw; Body-Forward ist 180 Grad zur Kamera -> invertieren.
    const double len = std::sqrt(static_cast<double>(fwd.x) * fwd.x
                                 + static_cast<double>(fwd.z) * fwd.z);

    if (len < 0.0001) {
        return;
    }

    const glm::vec3 n{static_cast<float>(-fwd.x / len), 0.0f,
                      static_cast<float>(-fwd.z / len)};

    // [to_quat] REFrameworks Vector3f:to_quat() ist
    // glm::quat(rowMajor4(lookAtLH({0,0,0}, v, {0,1,0}))) -- ROLLFREI, NICHT
    // der minimale Bogen und NICHT yaw_to_quat(atan2(x,z)). Ein Ersatz durch
    // quatLookAt/rotation/atan2 liefert ANDERE Vorzeichen.
    glm::quat yaw_quat = glm::quat(
        glm::rowMajor4(glm::lookAtLH(glm::vec3{0.0f, 0.0f, 0.0f}, n, glm::vec3{0.0f, 1.0f, 0.0f})));

    if (m_cfg.yaw_offset_deg != 0.0) {
        const double half = deg2rad(m_cfg.yaw_offset_deg) * 0.5;
        const glm::quat off{static_cast<float>(std::cos(half)), 0.0f,
                            static_cast<float>(std::sin(half)), 0.0f};
        yaw_quat = qnorm(off * yaw_quat);
    }

    m_last_yaw_quat = yaw_quat;

    const double now = now_clock();
    double dt = 0.011;

    if (m_body_yaw_last_t.has_value()) {
        dt = now - *m_body_yaw_last_t;

        if (dt <= 0.0) {
            dt = 0.011;
        } else if (dt > 0.1) {
            dt = 0.1;
        }
    }

    m_body_yaw_last_t = now;

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    glm::quat body_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", body_rot)) {
        return;
    }

    const auto body_yaw = flat_yaw_of(body_rot);

    if (!body_yaw.has_value()) {
        return;
    }

    // Ownership-Uebernahme ohne Sprung: vom Ist-Yaw starten.
    if (!m_owned_yaw.has_value()) {
        m_owned_yaw = *body_yaw;
    }

    const glm::vec3 tfwd = yaw_quat * glm::vec3{0.0f, 0.0f, 1.0f};
    const double target_yaw = std::atan2(static_cast<double>(tfwd.x), static_cast<double>(tfwd.z));

    double d = wrap_pi(target_yaw - *m_owned_yaw);

    if (std::abs(d) >= deg2rad(BODY_YAW_EPS_DEG)) {
        double k = dt * m_cfg.body_yaw_speed;

        if (k > 1.0) {
            k = 1.0;
        }

        if (pin_hard) {
            k = 1.0;   // [PIN_1TO1] Snap, kein Nachlaufen
        }

        m_owned_yaw = wrap_pi(*m_owned_yaw + d * k);
    }

    const double diff = wrap_pi(*m_owned_yaw - *body_yaw);
    const glm::quat q = yaw_to_quat(*m_owned_yaw);

    if (std::abs(diff) >= deg2rad(BODY_YAW_EPS_DEG)) {
        set_quat(tf, "set_Rotation", q);
    }

    // Hip hart mitnehmen (kein Anim-Nachlerpen beim Drehen).
    apply_hip_follow(tf, q);
}

// Lua Z.1810-1827.
void RE4VRMovement::enforce_hard_yaw() {
    if (!m_owned_yaw.has_value()) {
        return;
    }

    if (!m_cfg.enabled) {
        return;
    }

    if (is_ks_active()) {
        return;
    }

    if (re4vr::lua_get_bool("is_aim", false)) {
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    glm::quat body_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", body_rot)) {
        return;
    }

    const auto body_yaw = flat_yaw_of(body_rot);

    if (!body_yaw.has_value()) {
        return;
    }

    const double diff = wrap_pi(*m_owned_yaw - *body_yaw);

    if (std::abs(diff) < deg2rad(BODY_YAW_EPS_DEG)) {
        return;
    }

    set_quat(tf, "set_Rotation", yaw_to_quat(*m_owned_yaw));
}

// ============================================================================
// Roomscale (Lua Z.1837-2098, 2226-2241)
// ============================================================================

// Lua Z.1846-1861.
void RE4VRMovement::apply_auto_center() {
    if (!hmd_active()) {
        return;
    }

    if (m_character_manager.obj == nullptr || !re4vr::obj_ok(m_character_manager.obj)) {
        keep(re4vr::character_manager(), m_character_manager);
    }

    if (m_character_manager.obj == nullptr) {
        return;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(m_character_manager.obj, "getPlayerContextRef");

    if (ctx == nullptr || ctx == m_auto_center_ctx) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    const auto hmd = vr->get_position(0);
    auto so = vr->get_standing_origin();

    m_auto_center_ctx = ctx;
    so.x = hmd.x;
    so.z = hmd.z;
    vr->set_standing_origin(so);
}

// Lua Z.1865-1872. Y bleibt.
void RE4VRMovement::roomscale_recenter() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    const auto hmd = vr->get_position(0);
    auto so = vr->get_standing_origin();
    so.x = hmd.x;
    so.z = hmd.z;
    vr->set_standing_origin(so);
}

// Lua Z.1880-1917.
void RE4VRMovement::roomscale_recenter_events() {
    if (!(m_cfg.roomscale && m_cfg.rs_recenter)) {
        m_rs_prev_ks = false;
        m_rs_prev_pos.reset();
        return;
    }

    const bool ks = is_ks_active();

    if (m_rs_prev_ks && !ks) {
        roomscale_recenter();
    }

    m_rs_prev_ks = ks;

    if (ks) {
        m_rs_prev_pos.reset();
        return;
    }

    auto* tf = get_body_transform();
    glm::vec3 pos{};

    if (tf == nullptr || !get_vec3(tf, "get_Position", pos)) {
        m_rs_prev_pos.reset();
        return;
    }

    if (m_rs_prev_pos.has_value()) {
        const double dx = static_cast<double>(pos.x) - m_rs_prev_pos->x;
        const double dz = static_cast<double>(pos.z) - m_rs_prev_pos->y;

        // Teleport: mehr als 3 m in EINEM Frame war kein Gehen.
        if ((dx * dx + dz * dz) > (3.0 * 3.0)) {
            roomscale_recenter();
        }
    }

    m_rs_prev_pos = glm::vec2{pos.x, pos.z};
}

// Lua Z.1938-1972.
void RE4VRMovement::roomscale_crouch() {
    if (!(m_cfg.roomscale && m_cfg.rs_crouch)) {
        return;
    }

    if (is_ks_active()) {
        return;
    }

    if (!pure_gameplay_only()) {
        return;
    }

    const double stand = m_cfg.rs_stand_height;

    if (stand < 0.5) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    const auto hmd = vr->get_position(0);

    const double pct = m_cfg.rs_crouch_pct;
    const double down_at = stand * (1.0 - pct);
    const double up_at = stand * (1.0 - pct * 0.6);   // Hysterese

    bool want = false;

    if (static_cast<double>(hmd.y) < down_at) {
        want = true;
    } else if (static_cast<double>(hmd.y) > up_at) {
        want = false;
    } else {
        return;   // dazwischen: gar nichts tun
    }

    const bool ist = is_crouch_active();

    if (want == ist) {
        return;
    }

    const double now = now_clock();

    if (now < m_rs_crouch_next_t) {
        return;
    }

    m_rs_crouch_next_t = now + 0.6;
    re4vr::lua_set_bool("__re4_want_crouch_press", true);   // binding drueckt B
}

// ============================================================================
// [ROOMSCALE 1:1 PRAYDOG 16.09.2026 -- Ansage des Users]
// Wortgleich uebernommen aus FirstPerson::update_player_roomscale
// (src/mods/FirstPerson.cpp:1389), dem Roomscale von RE2/RE3/RE7 -- dort seit
// Jahren erprobt. Unser eigener Weg davor hatte vier Abweichungen, jede davon
// mit eigenen Nachbesserungen (Deadzone, Soft-Knee, Alpha, 20-cm-Gatter,
// Sammeln + spaeter Schreiben):
//
//   1. Bezug: Kopf gegen den STANDING-ORIGIN (Absolutabstand), nicht das
//      Frame-zu-Frame-Delta. Kann nicht driften.
//   2. Der Origin wird GELERPT, abhaengig von Abstand und delta_time -- nie
//      hart gesetzt. Deshalb braucht es keine Glaettung obendrauf.
//   3. Richtung: auf die zuletzt GERENDERTE Kameraposition zu
//      (last_render_matrix[3]), nicht das selbst gedrehte Rohdelta.
//   4. Geschrieben mit sdk::set_transform_position(..., no_dirty = true) im
//      PRE-Hook von UnlockScene. Praydogs Kommentar dazu: "BAD idea to call
//      this without no_dirty while scene is locked. causes parent objects
//      to get stuck." -- genau das war unser alter Befund "in LockScene-pre
//      ist der Write verloren".
//
// Unterschiede zu praydog, alle bewusst und klein:
//   * Gates: seine RE2-Abfragen (CameraControlType::PLAYER, is_jacked) sind
//     durch unsere Entsprechungen ersetzt (PlayerCameraController aktiv,
//     Killswitch aus). Seine Logik -- jedes verfehlte Gate setzt die
//     Fehlschlag-Uhr, danach 1 s Pause -- bleibt identisch.
//   * Hinter unserem Haken m_cfg.roomscale. Ohne Haken laeuft dieser Weg NIE,
//     das normale Gameplay bleibt Byte fuer Byte beim alten HMD-Follow.
//   * glm::normalize auf einen Nullvektor ergaebe NaN und schriebe den
//     Spieler ins Nichts -- dafuer ein Laengen-Check, sonst nichts.
// ============================================================================
// [DUCKEN PER IK 16.09.2026] Leons Bein-IK. Bei jedem Aufruf frisch geholt:
// nach Kostuem- oder Szenenwechsel ist es eine neue Komponente.
::REManagedObject* RE4VRMovement::rs_ikleg() {
    auto* tf = get_body_transform();
    auto* go = tf != nullptr
        ? re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject")
        : nullptr;

    return go != nullptr ? re4vr::get_component(go, "via.motion.IkLeg2") : nullptr;
}

// Stellt das Bein-IK auf die Werte des Spiels zurueck -- genau einmal pro
// Wechsel, nicht jeden Frame. Laeuft bei JEDEM verfehlten Gate von
// roomscale(): Cutscene, Menue, Killswitch, Haken aus. Sonst bliebe Leon dort
// in der Hocke stehen, in der der Spieler gerade war.
void RE4VRMovement::rs_ik_restore() {
    if (!m_rs_ik_active) {
        return;
    }

    m_rs_ik_active = false;

    // [ROOMSCALE-KAMERA 16.09.2026] Kamera-Ausgleich SOFORT auf 0 -- vor jedem
    // weiteren Ausstieg. Bliebe er haengen, saesse die Kamera auch ohne
    // Roomscale zu hoch.
    if (auto* vr = VR::get().get(); vr != nullptr) {
        vr->set_roomscale_camera_y_comp(0.0f);
        vr->clear_roomscale_body();
    }

    auto* ik = rs_ikleg();

    if (ik == nullptr) {
        m_rs_snap_pending = false;
        return;
    }

    // [FUSS-SNAP 16.09.2026] Lief gerade ein Snap, das IK unbedingt wieder
    // einschalten -- sonst bliebe es nach Cutscene/Menue/Haken-aus dauerhaft aus.
    if (m_rs_snap_pending) {
        re4vr::call_safe<void*>(ik, "set_Enabled", true);
        m_rs_snap_pending = false;
    }

    set_vec3(ik, "set_CenterOffset", glm::vec3{0.0f, 0.0f, 0.0f});
    re4vr::call_safe<void*>(ik, "set_CenterPositionCtrl", m_rs_ik_ctrl);
    re4vr::call_safe<void*>(ik, "set_GroundContactUpDistance", m_rs_ik_ground_up);
}

void RE4VRMovement::roomscale() {
    const double now = now_clock();

    if (!m_cfg.enabled || !m_cfg.roomscale) {
        m_rs_last_failure = now;
        rs_ik_restore();
        return;
    }

    // Entspricht praydogs m_last_camera_type != PLAYER und is_jacked().
    if (is_ks_active() || !hmd_active() || get_player_cam_controller() == nullptr) {
        m_rs_last_failure = now;
        rs_ik_restore();
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        m_rs_last_failure = now;
        rs_ik_restore();
        return;
    }

    auto* transform = reinterpret_cast<::RETransform*>(get_body_transform());

    if (transform == nullptr) {
        m_rs_last_failure = now;
        rs_ik_restore();
        return;
    }

    // try to fix some weirdness after exiting cutscenes and stuff (praydog)
    if ((now - m_rs_last_failure) < 1.0) {
        return;
    }

    auto* app = sdk::Application::get();

    if (app == nullptr) {
        return;
    }

    // Roomscale movement.
    const auto old_standing_origin = vr->get_standing_origin();
    const auto old_hmd_pos = vr->get_position(0);
    const auto hmd_pos = Vector4f{old_hmd_pos.x, old_standing_origin.y, old_hmd_pos.z, old_hmd_pos.w};

    // [DUCKEN PER IK 16.09.2026 -- Ansage des Users: NUR das Ducken tauschen]
    // Frueher drueckte roomscale_crouch() beim tiefen Kopf die Ducken-Taste.
    // Jetzt wie bei praydog (FirstPerson.cpp:1263, RE8VR.cpp:410): das
    // Bein-IK verschiebt den Schwerpunkt um die Kopfhoehe gegen den Origin,
    // die Beine knicken ein -- stufenlos, ohne Taste und ohne Kalibrierung.
    //
    // Gemessen: Leon hat via.motion.IkLeg2 (nicht IkLeg), die Center-Setter
    // sind dieselben, WorldOffset ist auch dort 2. Das Spiel selbst laesst den
    // Center-Offset auf 0/None.
    //
    // NUR Y: X/Z schiebt oben der Transform-Teil. Der Origin steht in Y auf
    // der Kopfhoehe beim VR-Start (VR.cpp:1727) -- im Stehen ist dy also ~0.
    if (auto* ik = rs_ikleg(); ik != nullptr) {
        if (!m_rs_ik_active) {
            // Die Werte des Spiels einmal merken, bevor sie ueberschrieben werden.
            float up = IKLEG_GROUND_UP_GAME;
            int32_t ctrl = IKLEG_CTRL_NONE;
            re4vr::try_call<float>(ik, "get_GroundContactUpDistance", up);
            re4vr::try_call<int32_t>(ik, "get_CenterPositionCtrl", ctrl);
            m_rs_ik_ground_up = up;
            m_rs_ik_ctrl = ctrl;
            m_rs_ik_active = true;
        }

        // [NICHT NACH OBEN 16.09.2026] Nur absinken, nie ueber die Standhoehe
        // heben: steht der Kopf hoeher als der Origin, bliebe dy positiv und das IK
        // hoebe Leon vom Boden ab. Praydog nimmt das in RE2 bewusst in Kauf
        // ("the player can slightly float") -- hier nicht.
        // [HALS-DREHPUNKT 16.09.2026 -- Befund des Users] Leon duckte sich schon,
        // wenn man im Stehen nur NACH UNTEN SCHAUTE. Der Tracking-Punkt des
        // Headsets sitzt vorne vor den Augen; beim Nicken dreht er um den Hals und
        // sinkt dabei, obwohl der Kopf gleich hoch bleibt. Gemessen wird die Hoehe
        // deshalb am Drehpunkt im Hals: RS_NECK_DOWN unter und RS_NECK_BACK hinter
        // dem Headset, jeweils in dessen eigenen Achsen. Der Punkt steht beim
        // Nicken still.
        //
        // "+ RS_NECK_DOWN" gleicht den Abstand wieder aus: beim Blick geradeaus ist
        // head_y exakt die Headset-Hoehe -- dort aendert sich gegenueber vorher also
        // nichts, und der Origin (beim Recenter auf die Headset-Hoehe gesetzt)
        // bleibt die richtige Referenz.
        //
        // Spalte 1 der Kopfmatrix ist "oben", Spalte 2 "hinten" (s.
        // OverlayComponent::update_panel_anchor).
        const auto hmd_mat = vr->get_transform(0);
        const float neck_y = hmd_mat[3].y
                             + hmd_mat[1].y * -RS_NECK_DOWN
                             + hmd_mat[2].y * RS_NECK_BACK;
        const float head_y = neck_y + RS_NECK_DOWN;

        const float dy = (std::min)(head_y - old_standing_origin.y, 0.0f);

        set_vec3(ik, "set_CenterOffset", glm::vec3{0.0f, dy, 0.0f});
        re4vr::call_safe<void*>(ik, "set_CenterPositionCtrl", IKLEG_CTRL_WORLD_OFFSET);

        // [ROOMSCALE-KAMERA 16.09.2026] Dieselbe Hoehe der Kamera melden, damit der
        // Headset-Versatz sie nicht ein zweites Mal abzieht (VR.cpp).
        vr->set_roomscale_camera_y_comp(dy);

        // [KAMERA-IST-HOEHE 16.09.2026] Leons Transform-Hoehe dazu -- VR.cpp misst
        // daran, wie weit die Spielkamera tatsaechlich schon gesunken ist.
        vr->set_roomscale_body_y(sdk::get_transform_position(transform).y);

        // [SCHWEBEN 16.09.2026] GroundContactUpDistance wird NICHT mehr auf 0
        // gesetzt. Praydog tut das in RE8 gegen Hochdruecken -- bei Leons
        // IkLeg2 schwebte er damit einen halben Meter, auch mit kalibrierter
        // Standhoehe (dy = 0). Der Spielwert (0,8) bleibt stehen. Einzige
        // Aenderung dieses Tests -- steht Leon danach am Boden, ist es belegt.

        // [FUSS-SNAP 16.09.2026] Erst den Snap vom letzten Frame beenden -- IK
        // wieder an, die Fuesse stehen jetzt unter dem Koerper.
        if (m_rs_snap_pending) {
            re4vr::call_safe<void*>(ik, "set_Enabled", true);
            m_rs_snap_pending = false;
        } else if (++m_rs_snap_frame >= RS_SNAP_EVERY) {
            m_rs_snap_frame = 0;

            const auto stick = vr->get_left_stick_axis();
            const bool stick_idle = glm::length(stick) < RS_SNAP_STICK_IDLE;

            if (stick_idle && dy > RS_SNAP_MAX_DUCK) {
                auto* tfo = reinterpret_cast<::REManagedObject*>(transform);
                auto* lf = joint_by_name(tfo, "L_Foot");
                auto* rf = joint_by_name(tfo, "R_Foot");
                glm::vec3 lp{};
                glm::vec3 rp{};

                if (lf != nullptr && rf != nullptr && get_vec3(lf, "get_Position", lp)
                    && get_vec3(rf, "get_Position", rp)) {
                    const auto tp = sdk::get_transform_position(transform);
                    const float fx = (lp.x + rp.x) * 0.5f - tp.x;
                    const float fz = (lp.z + rp.z) * 0.5f - tp.z;

                    if ((fx * fx + fz * fz) > RS_SNAP_DIST * RS_SNAP_DIST) {
                        re4vr::call_safe<void*>(ik, "set_Enabled", false);
                        m_rs_snap_pending = true;
                    }
                }
            }
        }
    } else {
        // [ROOMSCALE-KAMERA 16.09.2026] Kein Bein-IK (Body-Wechsel, Laden): dann
        // traegt auch niemand Hoehe -- Kamera-Ausgleich aus, sonst bliebe der
        // letzte Wert stehen.
        vr->set_roomscale_camera_y_comp(0.0f);
        vr->clear_roomscale_body();
    }

    if (glm::length(hmd_pos - old_standing_origin) > 0.01f) {
        const auto t = app->get_delta_time() * 0.1f;
        const auto standing_origin = glm::lerp(old_standing_origin, hmd_pos, glm::length(hmd_pos - old_standing_origin) * t);
        vr->set_standing_origin(standing_origin);

        const auto standing_diff = standing_origin - old_standing_origin;

        const auto player_pos = sdk::get_transform_position(transform);
        const auto& last_render_matrix = vr->get_last_render_matrix();
        const auto lerp_to = Vector4f{last_render_matrix[3].x, player_pos.y, last_render_matrix[3].z, player_pos.w};

        // Einzige Ergaenzung: kein normalize auf einen Nullvektor.
        if (glm::length(lerp_to - player_pos) < 1e-5f) {
            return;
        }

        const auto new_pos = player_pos + (glm::normalize(lerp_to - player_pos) * glm::length(standing_diff));

        // BAD idea to call this without no_dirty while scene is locked. causes parent objects to get stuck. (praydog)
        sdk::set_transform_position(transform, new_pos, true);
    }
}

// Lua Z.1974-2098.
void RE4VRMovement::apply_hmd_body_follow() {
    if (!m_cfg.enabled || !m_cfg.hmd_follow) {
        return;
    }

    // [ROOMSCALE 1:1 PRAYDOG 16.09.2026] Mit Haken uebernimmt roomscale()
    // im PRE-Hook von UnlockScene. Hier laeuft dann NICHTS mehr -- die
    // Roomscale-Zweige weiter unten sind seither tot, bleiben aber bewusst
    // stehen, damit der Weg OHNE Haken unangetastet ist.
    if (m_cfg.roomscale) {
        return;
    }

    if (is_ks_active()) {
        return;
    }

    if (!hmd_active()) {
        return;
    }

    if (get_player_cam_controller() == nullptr) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    const auto hmd = vr->get_position(0);
    auto so = vr->get_standing_origin();

    double dx = static_cast<double>(hmd.x) - static_cast<double>(so.x);
    double dz = static_cast<double>(hmd.z) - static_cast<double>(so.z);
    double dist2 = dx * dx + dz * dz;

    // [ROOMSCALE] "Bewegung uebertragen" statt "Versatz jagen": die Strecke
    // seit dem letzten Aufruf nehmen. rs_prev_hmd wird erst NACH dem Anwenden
    // fortgeschrieben -- sonst ginge jede Bewegung verloren, die unter der
    // Schwelle abbricht.
    bool rs_delta = false;

    if (m_cfg.roomscale && !m_cfg.rs_lean && m_rs_prev_hmd.has_value()) {
        dx = static_cast<double>(hmd.x) - m_rs_prev_hmd->x;
        dz = static_cast<double>(hmd.z) - m_rs_prev_hmd->y;
        dist2 = dx * dx + dz * dz;
        rs_delta = true;

        // 20 cm in EINEM Aufruf kann kein Schritt sein -> nur neu einnorden.
        if (dist2 > (0.20 * 0.20)) {
            m_rs_prev_hmd = glm::vec2{hmd.x, hmd.z};
            return;
        }
    }

    // [ROOMSCALE-LEAN] Mit Lean-Zone darf der Kopf den Body bis zum Radius
    // verlassen, ohne dass der Body nachrutscht.
    double dead = m_cfg.hmd_follow_deadzone;

    if (m_cfg.roomscale && m_cfg.rs_lean) {
        dead = (std::max)(dead, m_cfg.rs_lean_radius);
    }

    if (rs_delta) {
        dead = 0.0002;   // nur noch Rausch-Gatter
    }

    if (dist2 < (dead * dead)) {
        return;
    }

    // [SOFT_FOLLOW] Soft-Knee: nur den Anteil JENSEITS der Deadzone.
    const double dist = std::sqrt(dist2);
    double knee = (dist - dead) / dist;

    if (m_cfg.roomscale && !m_cfg.rs_lean) {
        knee = 1.0;
    }

    if (rs_delta) {
        knee = 1.0;   // eine Strecke wird nicht beschnitten
    }

    dx *= knee;
    dz *= knee;

    // Mit Haken 1:1 nachziehen, sonst der eingestellte Wert. Bewusst als
    // Effektivwert und NICHT durch Schreiben in cfg.
    double alpha = m_cfg.roomscale ? 1.0 : m_cfg.hmd_follow_alpha;

    if (alpha > 0.99) {
        alpha = 0.99;
    } else if (alpha < 0.01) {
        alpha = 0.01;
    }

    const double now = now_clock();
    const double dt =
        m_hmd_follow_last_t.has_value() ? (std::min)(now - *m_hmd_follow_last_t, 0.1) : 0.016;
    m_hmd_follow_last_t = now;
    double a = 1.0 - std::exp(std::log(1.0 - alpha) * dt * 60.0);

    if (rs_delta) {
        a = 1.0;   // die Strecke ist schon "pro Frame"
    }

    dx *= a;
    dz *= a;

    // Tracking -> Welt.
    glm::vec3 delta{static_cast<float>(dx), 0.0f, static_cast<float>(dz)};
    const auto rot_off = vr->get_rotation_offset();
    delta = quat_rotate_vec3(rot_off, delta);

    auto* cam = sdk::get_primary_camera();
    auto* cam_go = cam != nullptr ? re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject")
                                  : nullptr;
    auto* cam_tf = cam_go != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cam_go, "get_Transform")
        : nullptr;
    glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};

    if (cam_tf == nullptr || !get_quat(cam_tf, "get_Rotation", cam_rot)) {
        return;
    }

    // [ROOMSCALE] Die Kamerarotation enthaelt PITCH und ROLL. Eine waagerechte
    // Trackingstrecke damit zu drehen macht sie kuerzer/schief, sobald der Kopf
    // nickt. Fuer Roomscale deshalb NUR das Yaw.
    if (m_cfg.roomscale) {
        const auto cyaw = flat_yaw_of(cam_rot);

        if (cyaw.has_value()) {
            cam_rot = yaw_to_quat(*cyaw);
        }
    }

    delta = quat_rotate_vec3(cam_rot, delta);
    delta.y = 0.0f;

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        return;
    }

    glm::vec3 pos{};

    if (!get_vec3(tf, "get_Position", pos)) {
        return;
    }

    // [MESSBEFUND] In LockScene-pre ist der Write verloren (0,001 m von 0,5 m).
    // Deshalb hier nur SAMMELN und spaeter in der Enforce-Phase schreiben.
    if (m_cfg.roomscale) {
        m_rs_pending.x += delta.x;
        m_rs_pending.y += delta.z;
    } else {
        set_vec3(tf, "set_Position", glm::vec3{pos.x + delta.x, pos.y, pos.z + delta.z});
    }

    // Standing-Origin um den GLEICHEN Anteil Richtung HMD ziehen (Y bleibt!).
    so.x = static_cast<float>(static_cast<double>(so.x) + dx);
    so.z = static_cast<float>(static_cast<double>(so.z) + dz);
    vr->set_standing_origin(so);

    // Erst JETZT fortschreiben.
    m_rs_prev_hmd = glm::vec2{hmd.x, hmd.z};
}

// Lua Z.2226-2241.
void RE4VRMovement::roomscale_flush_body() {
    if (!m_cfg.roomscale) {
        m_rs_pending = glm::vec2{0.0f, 0.0f};
        return;
    }

    if (m_rs_pending.x == 0.0f && m_rs_pending.y == 0.0f) {
        return;
    }

    auto* tf = get_body_transform();

    if (tf == nullptr) {
        m_rs_pending = glm::vec2{0.0f, 0.0f};
        return;
    }

    glm::vec3 pos{};

    if (!get_vec3(tf, "get_Position", pos)) {
        m_rs_pending = glm::vec2{0.0f, 0.0f};
        return;
    }

    set_vec3(tf, "set_Position",
             glm::vec3{pos.x + m_rs_pending.x, pos.y, pos.z + m_rs_pending.y});
    m_rs_pending = glm::vec2{0.0f, 0.0f};
}

// ============================================================================
// [HMD_YAW_DRIVE] (Lua Z.2100-2199)
// Treibt das Engine-Kamera-Yaw-Feld _Yaw vom HMD -> die Engine dreht Body und
// Kamera NATIV (wie der Stick). Doppel-HMD-Vermeidung ueber
// rotation_offset = -hmd_yaw.
// ============================================================================

std::optional<double> RE4VRMovement::hmd_raw_yaw() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return std::nullopt;
    }

    // vrmod:get_transform(0):to_quat() -- Matrix4x4f:to_quat() ist schlicht
    // glm::quat(m). Die lookAtLH-Falle greift hier NICHT.
    const auto m = vr->get_transform(0);
    return flat_yaw_of(glm::quat(m));
}

bool RE4VRMovement::hmd_yaw_active() {
    if (!m_cfg.hmd_yaw_drive) {
        return false;
    }

    if (!hmd_active()) {
        return false;
    }

    if (is_ks_active()) {
        return false;
    }

    return true;
}

// Lua Z.2136-2190.
void RE4VRMovement::drive_hmd_yaw() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    if (!hmd_yaw_active()) {
        // Inaktiv: Offset neutral, Baseline neu nehmen. AUSNAHME: haelt der
        // Recenter gerade einen Event-Offset, NICHT auf 0 zwingen.
        m_hmd_yaw_prev.reset();

        if (!re4vr::lua_get_bool("__vr_recenter_hold", false)) {
            vr->set_rotation_offset(yaw_to_quat(0.0));
        }

        return;
    }

    if (m_hmd_yaw_pcc.obj == nullptr) {
        return;
    }

    const auto hmd = hmd_raw_yaw();

    if (!hmd.has_value()) {
        return;
    }

    if (!m_hmd_yaw_prev.has_value()) {
        m_hmd_yaw_prev = *hmd;
    }

    const double d = wrap_pi(*hmd - *m_hmd_yaw_prev);
    m_hmd_yaw_prev = *hmd;

    // HMD-Yaw-Delta in das Engine-Kamera-Yaw.
    std::optional<double> yaw_new{};
    const auto yaw = act_get_num(m_hmd_yaw_pcc.obj, "_Yaw");

    if (yaw.has_value()) {
        yaw_new = wrap_pi(*yaw + d);
        act_set_num(m_hmd_yaw_pcc.obj, "_Yaw", *yaw_new);
    }

    // [ANTI-JITTER] Engine daempft das Kamera-Yaw -> Saegezahn. Output-Yaw
    // direkt auf den ungedaempften Zielwert (_Yaw + pi), NUR Yaw.
    if (yaw_new.has_value()) {
        glm::quat cam_rot{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat_field(m_hmd_yaw_pcc.obj, "_CameraRotation", cam_rot)) {
            const auto cur = flat_yaw_of(cam_rot);

            if (cur.has_value()) {
                const double dyaw = wrap_pi((*yaw_new + PI_D) - *cur);
                const glm::quat new_cr = yaw_to_quat(dyaw) * cam_rot;
                set_quat_field(m_hmd_yaw_pcc.obj, "_CameraRotation", new_cr);

                auto def = utility::re_managed_object::get_type_definition(m_hmd_yaw_pcc.obj);

                if (def != nullptr) {
                    if (auto* f = def->get_field("_MainCameraController")) {
                        try {
                            auto* raw = f->get_data_raw(m_hmd_yaw_pcc.obj, false);
                            auto* main =
                                raw != nullptr ? *reinterpret_cast<::REManagedObject**>(raw)
                                               : nullptr;

                            if (main != nullptr) {
                                set_quat_field(main, "_CameraRotation", new_cr);
                            }
                        } catch (...) {
                        }
                    }
                }
            }
        }
    }

    // Headset im Bild ausgleichen (sonst Doppel-HMD).
    vr->set_rotation_offset(yaw_to_quat(-*hmd));
}

void RE4VRMovement::hook_capture_pcc(::REManagedObject* pcc) {
    keep(pcc, m_hmd_yaw_pcc);
}

void RE4VRMovement::hook_drive_hmd_yaw() {
    // [SCRIPTGATE] Auch der Hook-Rumpf steigt bei geschlossenem Riegel aus.
    if (re4vr::mods_gated()) {
        return;
    }

    drive_hmd_yaw();
}

// ============================================================================
// Gravitation (Lua Z.2364-2414)
// ============================================================================

::REManagedObject* RE4VRMovement::ground_adsorber(::REManagedObject** out_ctx) {
    if (out_ctx != nullptr) {
        *out_ctx = nullptr;
    }

    if (m_character_manager.obj == nullptr || !re4vr::obj_ok(m_character_manager.obj)) {
        keep(re4vr::character_manager(), m_character_manager);
    }

    if (m_character_manager.obj == nullptr) {
        return nullptr;
    }

    auto* c = re4vr::call_safe<::REManagedObject*>(m_character_manager.obj, "getPlayerContextRef");

    if (c == nullptr) {
        return nullptr;
    }

    if (out_ctx != nullptr) {
        *out_ctx = c;
    }

    auto* bu = re4vr::call_safe<::REManagedObject*>(c, "get_BodyUpdater");

    if (bu == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(bu, "get_GroundAdsorber");
}

// Lua Z.2374-2412.
void RE4VRMovement::apply_grav_fix() {
    ::REManagedObject* c = nullptr;
    auto* ga = ground_adsorber(&c);

    if (ga == nullptr || c == nullptr) {
        return;
    }

    const auto cur = act_get_num(ga, "_GravitationalAcceleration");

    if (!m_grav_orig.has_value() && cur.has_value() && *cur > 0.0 && *cur < 100.0) {
        m_grav_orig = *cur;
    }

    // [SELBSTHEILUNG] Ein "Reset Scripts" mitten im Fix fand hier cur = 150 vor:
    // die Plausibilitaetsgrenze (< 100) verhinderte das Merken, grav_orig blieb
    // leer, und die Funktion stieg ab da JEDES Mal sofort aus -- der erhoehte
    // Wert klebte im ganzen Spiel fest.
    if (!m_grav_orig.has_value() && cur.has_value() && *cur >= 100.0) {
        m_grav_orig = 24.0;   // Engine-Default
    }

    if (!m_grav_orig.has_value()) {
        return;
    }

    m_grav_active = false;

    // NUR im reinen Gameplay.
    if (m_cfg.grav_fix && pure_gameplay_only() && !mv_in_zone()) {
        bool run = false;
        bool walk = false;
        double intensity = 0.0;
        re4vr::try_call<bool>(c, "get_IsRun", run);
        re4vr::try_call<bool>(c, "get_IsWalk", walk);
        float mi = 0.0f;

        if (re4vr::try_call<float>(c, "get_MoveIntensity", mi)) {
            intensity = static_cast<double>(mi);
        }

        const bool moving = run || walk || intensity > 0.1;

        // Leiter und bewusster Sprung ausgenommen. Luas `~= true` heisst: ein
        // GESCHEITERTER Call zaehlt als "nicht auf der Leiter".
        bool ladder = false;
        bool jumping = false;
        const bool ladder_true = re4vr::try_call<bool>(c, "get_IsLadder", ladder) && ladder;
        const bool jumping_true = re4vr::try_call<bool>(c, "get_IsJumping", jumping) && jumping;

        if (moving && !ladder_true && !jumping_true) {
            m_grav_active = true;
            act_set_num(ga, "_GravitationalAcceleration", m_cfg.grav_value);
            return;
        }
    }

    if (cur.has_value() && std::abs(*cur - *m_grav_orig) > 0.01) {
        act_set_num(ga, "_GravitationalAcceleration", *m_grav_orig);
    }
}

// ============================================================================
// [LAGMOVE] Anlauf-Boost + Nachlauf-Bremse (Lua Z.2416-2500)
// ============================================================================

// Lua Z.2422-2429.
double RE4VRMovement::lag_pad_axis() {
    auto* gp = sdk::get_native_singleton("via.hid.GamePad");

    if (gp == nullptr || m_pad_td == nullptr) {
        return 0.0;
    }

    const auto method = m_pad_td->get_method("get_LastInputDevice");

    if (method == nullptr) {
        return 0.0;
    }

    ::REManagedObject* pad = nullptr;

    try {
        pad = method->call_safe<::REManagedObject*>(sdk::get_thread_context(), gp);
    } catch (...) {
        pad = nullptr;
    }

    re4vr::clear_vm_exception();

    if (pad == nullptr) {
        return 0.0;
    }

    // [AUFRUFKONVENTION 05.09.2026] Hier stand try_call<glm::vec2> auf
    // get_AxisL, begruendet mit "8 Byte, kommt in XMM0". Der Fork selbst sagt an
    // zwei Stellen wortwoertlich das Gegenteil -- VR.cpp:3812 und
    // FreeCam.cpp:224: "It's not a Vector2f because via.vec2 is not actually
    // 8 bytes, we don't want stack corruption to occur" -- und liest deshalb das
    // FELD `AxisL` als Vector3f*. Genau so ist derselbe Lua-Aufruf in
    // RE4VRFirstPerson.cpp (get_vec2_field) portiert. Nimmt die Engine einen
    // versteckten sret-Zeiger, landet der Puffer in RCX, wo der VMContext
    // erwartet wird: lag_pad_axis lieferte Muell (kaputte Stick-Flanke in
    // apply_lag_fix) und riskierte einen Absturz in UpdateMotion.
    glm::vec2 a{0.0f, 0.0f};

    // `AxisL` ist ein NATIVES via.*-Feld -- dafuer ist
    // utility::re_managed_object::get_field der richtige Weg (er liefert einen
    // Zeiger IN das Objekt). Nur managed C#-Felder brauchen die TDB-Helfer.
    {
        auto* raw = utility::re_managed_object::get_field<Vector3f*>(pad, "AxisL");

        if (raw == nullptr) {
            return 0.0;
        }

        a = glm::vec2{raw->x, raw->y};
    }

    return std::sqrt(static_cast<double>(a.x) * a.x + static_cast<double>(a.y) * a.y);
}

// Lua Z.2431-2500.
void RE4VRMovement::apply_lag_fix() {
    if (!(m_cfg.lag_boost_on || m_cfg.lag_brake_on)) {
        return;
    }

    // Ausserhalb des reinen Gameplays KOMPLETT still -- inklusive Zustand
    // zuruecksetzen, damit keine alte Stick-Flanke aus dem Menue nachwirkt.
    if (!pure_gameplay_only()) {
        m_lagm.last_pos.reset();
        m_lagm.last_t.reset();
        m_lagm.edge_t.reset();
        m_lagm.edge_kind = 0;
        m_lagm.pad_was = 0.0;
        m_lagm.boost_now = 0.0;
        m_lagm.brake_now = false;
        return;
    }

    ::REManagedObject* c = nullptr;
    auto* ga = ground_adsorber(&c);

    if (c == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(c, "get_BodyGameObject");
    auto* btf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform") : nullptr;

    if (btf == nullptr) {
        return;
    }

    glm::vec3 cur{};

    if (!get_vec3(btf, "get_Position", cur)) {
        return;
    }

    const double now = now_clock();
    const double dt = m_lagm.last_t.has_value()
        ? (std::min)((std::max)(now - *m_lagm.last_t, 0.001), 0.1)
        : 0.016;
    m_lagm.last_t = now;

    // native Geschwindigkeit (XZ) aus dem Frame-Delta
    double dx = 0.0;
    double dz = 0.0;

    if (m_lagm.last_pos.has_value()) {
        dx = static_cast<double>(cur.x) - m_lagm.last_pos->x;
        dz = static_cast<double>(cur.z) - m_lagm.last_pos->z;
    }

    const double step = std::sqrt(dx * dx + dz * dz);
    m_lagm.spd = step / dt;

    // Stick-Flanken (der Wille des Spielers, nicht der Zustand der Figur)
    const double pad = lag_pad_axis();

    if (pad >= 0.5 && m_lagm.pad_was < 0.5) {
        m_lagm.edge_t = now;
        m_lagm.edge_kind = 1;   // START
    } else if (pad < 0.15 && m_lagm.pad_was >= 0.15) {
        m_lagm.edge_t = now;
        m_lagm.edge_kind = 2;   // STOP
    }

    m_lagm.pad_was = pad;

    bool ladder = false;
    bool jumping = false;
    bool blocked = (re4vr::try_call<bool>(c, "get_IsLadder", ladder) && ladder)
        || (re4vr::try_call<bool>(c, "get_IsJumping", jumping) && jumping);

    if (ga != nullptr) {
        // Luas `~= true`: ein gescheiterter Call zaehlt als "nicht am Boden".
        bool ground = false;

        if (!(re4vr::try_call<bool>(ga, "get_Ground", ground) && ground)) {
            blocked = true;
        }
    }

    m_lagm.boost_now = 0.0;
    m_lagm.brake_now = false;

    if (!blocked && m_lagm.edge_t.has_value()) {
        const double since = now - *m_lagm.edge_t;

        if (m_cfg.lag_boost_on && m_lagm.edge_kind == 1 && pad >= 0.5
            && since <= m_cfg.lag_boost_window) {
            const double add =
                (std::min)((std::max)(m_cfg.lag_boost_target - m_lagm.spd, 0.0),
                           m_cfg.lag_boost_max);

            if (add > 0.001) {
                glm::vec3 md{};

                if (get_vec3(c, "get_MoveDirection", md)) {
                    const double ml = std::sqrt(static_cast<double>(md.x) * md.x
                                                + static_cast<double>(md.z) * md.z);

                    if (ml > 1e-4) {
                        const double k = (add * dt) / ml;
                        set_vec3(btf, "set_Position",
                                 glm::vec3{static_cast<float>(cur.x + md.x * k), cur.y,
                                           static_cast<float>(cur.z + md.z * k)});
                        m_lagm.boost_now = add;
                    }
                }
            }
        } else if (m_cfg.lag_brake_on && m_lagm.edge_kind == 2 && pad < 0.15
                   && since <= m_cfg.lag_brake_window && m_lagm.last_pos.has_value()
                   && step > 0.0001) {
            const double g = (std::min)((std::max)(m_cfg.lag_brake_gain, 0.0), 1.0);
            set_vec3(btf, "set_Position",
                     glm::vec3{static_cast<float>(m_lagm.last_pos->x + dx * g), cur.y,
                               static_cast<float>(m_lagm.last_pos->z + dz * g)});
            m_lagm.brake_now = true;
        }
    }

    glm::vec3 after{};
    m_lagm.last_pos = get_vec3(btf, "get_Position", after) ? after : cur;
}

// ============================================================================
// [SQUEEZE_ANTIBEAM] (Lua Z.2825-2975)
// ============================================================================

::REManagedObject* RE4VRMovement::sqab_body_tf() {
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return nullptr;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return nullptr;
    }

    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(body, "get_Transform");
}

bool RE4VRMovement::sqab_in_gimmick() {
    auto* cs = re4vr::camera_system_singleton();

    if (cs == nullptr || m_sqab_gm_td == nullptr) {
        return false;
    }

    auto* main = re4vr::call_safe<::REManagedObject*>(cs, "get_MainCameraController");

    if (main == nullptr) {
        return false;
    }

    auto* busy = re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController");

    if (busy == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(busy);
    return def != nullptr && def->is_a(m_sqab_gm_td);
}

namespace {

double sqab_dist(const glm::vec4& a, const glm::vec4& b) {
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

// Lua Z.2862-2895.
void RE4VRMovement::sqab_tick() {
    const double now = now_clock();
    const bool a = sqab_in_gimmick();
    auto* tf = sqab_body_tf();

    if (tf == nullptr) {
        return;
    }

    glm::vec4 p{};

    if (!get_vec4(tf, "get_Position", p)) {
        return;
    }

    // Luas `(type(p) == "userdata" and p.w) or 1.0`.
    const glm::vec4 pp{p.x, p.y, p.z, p.w != 0.0f ? p.w : 1.0f};

    if (a && !m_sqab_active) {
        m_sqab_active = true;
        m_sqab_pin = false;

        const double end_t = re4vr::lua_get_number("__re4_sq_end_t", -999.0);
        const auto exit = re4vr::lua_get_vec3("__re4_sq_exit");
        const double dt = now - end_t;

        if (exit.has_value()) {
            const glm::vec4 ex{exit->x, exit->y, exit->z, 1.0f};

            if (dt < SQAB_RETRIG_WINDOW && sqab_dist(pp, ex) < SQAB_SAME_SPOT) {
                m_sqab_pin = true;
                m_sqab_pin_pos = ex;
            }
        }

        m_sqab_last = pp;
    }

    if (a) {
        if (m_sqab_pin && m_sqab_pin_pos.has_value()) {
            set_vec3(tf, "set_Position",
                     glm::vec3{m_sqab_pin_pos->x, m_sqab_pin_pos->y, m_sqab_pin_pos->z}, pp.w);
        } else {
            m_sqab_last = pp;
        }

        return;
    }

    if (m_sqab_active && !a) {
        m_sqab_active = false;
        const auto ex = (m_sqab_pin && m_sqab_pin_pos.has_value()) ? m_sqab_pin_pos : m_sqab_last;

        if (ex.has_value()) {
            re4vr::lua_set_vec3("__re4_sq_exit", glm::vec3{ex->x, ex->y, ex->z});
        }

        re4vr::lua_set_number("__re4_sq_end_t", now);
        m_sqab_pin = false;
    }
}

// Lua Z.2932-2944.
bool RE4VRMovement::sqab_is_retrigger() {
    const double dt = now_clock() - re4vr::lua_get_number("__re4_sq_end_t", -999.0);

    if (dt >= SQAB_RETRIG_WINDOW) {
        return false;
    }

    const auto ex = re4vr::lua_get_vec3("__re4_sq_exit");

    if (!ex.has_value()) {
        return false;
    }

    auto* tf = sqab_body_tf();
    glm::vec4 p{};

    if (tf == nullptr || !get_vec4(tf, "get_Position", p)) {
        return false;
    }

    const glm::vec4 exv{ex->x, ex->y, ex->z, 1.0f};
    return sqab_dist(glm::vec4{p.x, p.y, p.z, 1.0f}, exv) < SQAB_SAME_SPOT;
}

// Lua Z.2946-2975. Rueckgabe true = SKIP_ORIGINAL mit `out_ret` als Ersatz.
//
// [NIE BLIND SKIPPEN] setupJackLayer gibt System.Int32 zurueck (den
// JackLayerIndex). Ein SKIP_ORIGINAL ohne definierten Rueckgabewert liefert dem
// Aufrufer einen UNDEFINIERTEN Index; startJackPl holt sich darueber den Layer,
// bekommt null und schreibt hinein -> c0000005 mit rcx=0.
// Laesst sich der Index nicht lesen, wird NICHT geblockt.
bool RE4VRMovement::sqab_setup_jack_pre(::REManagedObject* holder, int32_t& out_ret) {
    if (re4vr::mods_gated()) {
        return false;   // Riegel zu -> CALL_ORIGINAL
    }

    if (!sqab_is_retrigger()) {
        return false;
    }

    if (holder == nullptr) {
        return false;
    }

    int32_t idx = 0;

    if (!re4vr::try_call<int32_t>(holder, "get_JackLayerIndex", idx)) {
        // Ohne gueltigen Index NICHT skippen.
        return false;
    }

    out_ret = idx;
    return true;
}

// ============================================================================
// Phasen (Lua Z.2202-2266, 2502-2534, 2897)
// ============================================================================

void RE4VRMovement::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRMovement", "on_pre_application_entry");
    if (re4vr::mods_gated()) {
        return;
    }

    if (!m_types_resolved) {
        m_types_resolved = true;
        m_player_cam_td = sdk::find_type_definition(game_namespace("PlayerCameraController"));
        m_pad_td = sdk::find_type_definition("via.hid.GamePad");
        m_sqab_gm_td = sdk::find_type_definition(game_namespace("GimmickMotionCameraController"));
    }

    if (hash == "LockScene"_fnv) {
        apply_auto_center();
        roomscale_recenter_events();
        // [DUCKEN PER IK 16.09.2026] roomscale_crouch() (Ducken-Taste) laeuft nicht
        // mehr -- das Ducken macht jetzt das Bein-IK in roomscale(). Sie lief
        // ausschliesslich mit Roomscale-Haken; ohne Haken aendert sich nichts.
        apply_hmd_body_follow();
        apply_hard_yaw(true);
        apply_ub_lock(true);   // einziger Capture-Slot (stabil, nach Yaw-Write)
        mv_zone_tick();        // [STILLZONE] Flanke auswerten
        update_stop_skip();
        update_anim_export();
        apply_spine_pin(true); // Capture-Slot + Enforce
        apply_crouch_pin(false);
        return;
    }

    // [ROOMSCALE 1:1 PRAYDOG 16.09.2026] Dieselbe Phase wie bei ihm:
    // FirstPerson::on_pre_unlock_scene.
    if (hash == "UnlockScene"_fnv) {
        roomscale();
        return;
    }

    if (hash == "BeginRendering"_fnv) {
        enforce_hard_yaw();
        apply_ub_lock(false);
        apply_spine_pin(false);
        apply_crouch_pin(false);
        apply_crouch_ub_z(false);
    }
}

void RE4VRMovement::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRMovement", "on_application_entry");
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        roomscale_flush_body();
        enforce_hard_yaw();

        // Spine/Neck-Yaw NUR hier: einmal pro Frame, direkt nach dem
        // Anim-Update. UNABHAENGIG von cfg.enabled -- eigener Feature-Schalter
        // ist der Slider selbst (0 = aus).
        //
        // [1:1] Das `if not is_ks_active()` umschliesst apply_ub_lock UND
        // apply_spine_yaw. Fuer apply_ub_lock ist es redundant (die Funktion
        // prueft selbst) -- es existiert fuer apply_spine_yaw. Wer es
        // "aufraeumt", zerlegt spine_yaw.
        if (!is_ks_active()) {
            apply_ub_lock(false);
            auto* tf = get_body_transform();

            if (tf != nullptr) {
                apply_spine_yaw(tf);
            }
        }

        // [SPINE_PIN] LateUpdateBehavior ist PFLICHT-Slot.
        apply_spine_pin(false);
        apply_crouch_pin(true);    // post-anim: native Hock-Pose capturen
        apply_crouch_ub_z(true);   // post-anim: native UB-Basis capturen
        return;
    }

    if (hash == "BeginRendering"_fnv) {
        enforce_hard_yaw();
        apply_ub_lock(false);
        apply_spine_pin(false);
        apply_crouch_pin(false);
        apply_crouch_ub_z(false);
        sqab_tick();   // Lua Z.2897: zweiter BeginRendering-Callback
        return;
    }

    if (hash == "UpdateMotion"_fnv) {
        enforce_hard_yaw();
        apply_ub_lock(false);
        apply_spine_pin(false);
        apply_crouch_pin(false);
        apply_lag_fix();   // [LAGMOVE] nach firstperson.apply_movement_stabilization
        return;
    }

    if (hash == "UpdateJointExpression"_fnv) {
        apply_ub_lock(false);
        apply_spine_pin(false);
        apply_crouch_pin(false);
        apply_crouch_ub_z(false);
    }
}

void RE4VRMovement::on_frame() {
    re4vr::trace("RE4VRMovement", "on_frame");
    // [SCRIPTGATE] ACHTUNG: hier ist ein blankes `return` gefaehrlich. Bei
    // geschlossenem Riegel muss der Gravitations-Fix erst den ORIGINALWERT
    // zurueckstellen und danach still sein -- sonst klebt 150 im ganzen Spiel.
    if (re4vr::mods_gated()) {
        m_grav_active = false;

        if (m_grav_orig.has_value()) {
            ::REManagedObject* c = nullptr;
            auto* ga = ground_adsorber(&c);

            if (ga != nullptr) {
                const auto cur = act_get_num(ga, "_GravitationalAcceleration");

                if (cur.has_value() && std::abs(*cur - *m_grav_orig) > 0.01) {
                    act_set_num(ga, "_GravitationalAcceleration", *m_grav_orig);
                }
            }
        }

        return;
    }

    apply_grav_fix();
}

// ============================================================================
// Hooks (Lua Z.2192-2199 und Z.2946-2975)
// ============================================================================

std::optional<std::string> RE4VRMovement::on_initialize() {
    // [HMD_YAW_DRIVE] updateCameraPosition: Pre merkt sich die richtige
    // PlayerCameraController-Instanz, Post treibt das Yaw. Genau dieses Timing
    // hat das Original (Lua Z.2192-2199).
    if (auto* pcc = sdk::find_type_definition(game_namespace("PlayerCameraController"))) {
        if (auto* m = pcc->get_method("updateCameraPosition")) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    // Luas args[2] = this  ->  C++ args[1].
                    if (args.size() > 1) {
                        RE4VRMovement::get()->hook_capture_pcc(
                            reinterpret_cast<::REManagedObject*>(args[1]));
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& retval, sdk::RETypeDefinition*, uintptr_t) {
                    RE4VRMovement::get()->hook_drive_hmd_yaw();
                    // Luas `return retval` -- unveraendert durchreichen.
                });
        }
    }

    // [SQUEEZE_ANTIBEAM] setupJackLayer blocken, aber MIT definiertem
    // Rueckgabewert. Der Lua-Guard `__re4_jackblock_hooked` entfaellt: nativ
    // laeuft on_initialize genau einmal.
    if (auto* jh = sdk::find_type_definition(game_namespace("MotionJackSuppliedHolder"))) {
        if (auto* m = jh->get_method("setupJackLayer")) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    auto self = RE4VRMovement::get();
                    self->m_sqab_skip_valid = false;

                    ::REManagedObject* holder =
                        args.size() > 1 ? reinterpret_cast<::REManagedObject*>(args[1]) : nullptr;
                    int32_t ret = 0;

                    if (!self->sqab_setup_jack_pre(holder, ret)) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    self->m_sqab_skip_ret = ret;
                    self->m_sqab_skip_valid = true;
                    return HookManager::PreHookResult::SKIP_ORIGINAL;
                },
                [](uintptr_t& retval, sdk::RETypeDefinition*, uintptr_t) {
                    auto self = RE4VRMovement::get();

                    if (self->m_sqab_skip_valid) {
                        self->m_sqab_skip_valid = false;
                        retval = static_cast<uintptr_t>(
                            static_cast<uint32_t>(self->m_sqab_skip_ret));
                    }
                });
        }
    }

    // [K1] Die Konfiguration MUSS hier geladen werden, nicht im ersten
    // Phasen-Einstieg: der liegt hinter dem SCRIPTGATE, on_draw_ui aber nicht.
    // Startet das Spiel in einer gegateten Stage (oder zeichnet die UI vor dem
    // ersten LockScene), arbeiteten die Regler sonst auf Compile-Defaults --
    // und der erste Klick schriebe ueber save_cfg() die komplette JSON des
    // Users platt, inklusive pin_pose und crouch_pose.
    // Im Original laeuft das Laden beim Datei-Load, also vor allem anderen.
    load_cfg();
    m_cfg_loaded = true;

    return Mod::on_initialize();
}

// ============================================================================
// Lua-Anbindung (Lua Z.1141-1172, 2300-2337)
// ============================================================================

void RE4VRMovement::publish_globals(sol::state& lua) {
    // [WICHTIG] _G ueberlebt "Reset Scripts" NICHT -- reset_scripts legt einen
    // neuen sol::state an. Deshalb MUSS das hier bei JEDEM on_lua_state_created
    // erneut laufen, sonst verschwindet das Public-Menue in
    // re4_vr_binding.lua (es prueft `type(fn) == "function"`) nach dem ersten
    // Reset dauerhaft.
    auto self = RE4VRMovement::get();

    // [STILLZONE] Die beiden Funktionen liest movement selbst und motion.
    lua["__re4_mv_in_zone"] = []() -> bool { return RE4VRMovement::get()->mv_in_zone(); };
    lua["__re4_mv_zone_tick"] = []() { RE4VRMovement::get()->mv_zone_tick(); };
    lua["__re4_mv_zone_prev"] = false;
    lua["__re4_mv_zone_t"] = sol::lua_nil;
    lua["__re4_mv_zone_win"] = 0.5;

    // ZBOB ausgebaut (Lua Z.251).
    lua["__vr_zbob_tau"] = sol::lua_nil;

    // Der Lua-Guard des Jack-Hooks: gesetzt lassen, damit eine versehentlich
    // mitlaufende alte Datei NICHT doppelt hookt (Lua Z.2928).
    lua["__re4_jackblock_hooked"] = true;

    // ---- Public-UI-Bruecken (Lua Z.2300-2337) --------------------------
    lua["__re4_roomscale_get"] = []() -> bool { return RE4VRMovement::get()->m_cfg.roomscale; };
    lua["__re4_roomscale_set"] = [](bool v) {
        auto s = RE4VRMovement::get();
        s->m_cfg.roomscale = v;
        s->save_cfg();
    };

    lua["__re4_rs_crouch_get"] = []() -> bool { return RE4VRMovement::get()->m_cfg.rs_crouch; };
    lua["__re4_rs_crouch_set"] = [](bool v) {
        auto s = RE4VRMovement::get();
        s->m_cfg.rs_crouch = v;
        s->save_cfg();
    };

    lua["__re4_rs_crouch_pct_get"] = []() -> double {
        return RE4VRMovement::get()->m_cfg.rs_crouch_pct;
    };
    lua["__re4_rs_crouch_pct_set"] = [](sol::object v) {
        auto s = RE4VRMovement::get();
        // Luas `tonumber(v) or 0.20`.
        s->m_cfg.rs_crouch_pct = v.is<double>() ? v.as<double>() : 0.20;
        s->save_cfg();
    };

    lua["__re4_rs_stand_get"] = []() -> double {
        return RE4VRMovement::get()->m_cfg.rs_stand_height;
    };

    // Kalibrieren: aufrecht stehen, druecken. Rueckgabe = uebernommene Hoehe
    // oder nil, wenn keine brauchbare Pose anlag.
    lua["__re4_rs_stand_calibrate"] = [](sol::this_state ts) -> sol::object {
        auto s = RE4VRMovement::get();
        auto* vr = VR::get().get();

        if (vr != nullptr) {
            const auto h = vr->get_position(0);

            if (h.y > 0.5f) {
                s->m_cfg.rs_stand_height = static_cast<double>(h.y);
                s->save_cfg();
                return sol::make_object(ts, s->m_cfg.rs_stand_height);
            }
        }

        return sol::make_object(ts, sol::lua_nil);
    };

    lua["__re4_rs_lean_get"] = []() -> bool { return RE4VRMovement::get()->m_cfg.rs_lean; };
    lua["__re4_rs_lean_set"] = [](bool v) {
        auto s = RE4VRMovement::get();
        s->m_cfg.rs_lean = v;
        s->save_cfg();
    };

    lua["__re4_rs_lean_rad_get"] = []() -> double {
        return RE4VRMovement::get()->m_cfg.rs_lean_radius;
    };
    lua["__re4_rs_lean_rad_set"] = [](sol::object v) {
        auto s = RE4VRMovement::get();
        s->m_cfg.rs_lean_radius = v.is<double>() ? v.as<double>() : 0.10;
        s->save_cfg();
    };

    lua["__re4_rs_recenter_get"] = []() -> bool { return RE4VRMovement::get()->m_cfg.rs_recenter; };
    lua["__re4_rs_recenter_set"] = [](bool v) {
        auto s = RE4VRMovement::get();
        s->m_cfg.rs_recenter = v;
        s->save_cfg();
    };

    (void)self;
}

void RE4VRMovement::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRMovement", "on_lua_state_created");
    publish_globals(lua);
}

// Lua Z.2767-2810 (on_script_reset) PLUS das, was das Neuladen der Datei
// mitbringt: alle `local`s stehen danach wieder auf ihren Anfangswerten.
void RE4VRMovement::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRMovement", "on_lua_state_destroyed");
    (void)lua;
    reset_state();
}

void RE4VRMovement::reset_state() {
    // [STEPFALL_GRAV] Gravitation zurueck auf den Spielwert.
    if (m_grav_orig.has_value()) {
        ::REManagedObject* c = nullptr;
        auto* ga = ground_adsorber(&c);

        if (ga != nullptr) {
            act_set_num(ga, "_GravitationalAcceleration", *m_grav_orig);
        }
    }

    // FSM-Patches ueberleben "Reset Scripts" -> sauber zuruecksetzen.
    if (m_stop_skip.applied) {
        apply_stop_skip(false);
    }

    if (m_start_skip.applied) {
        apply_start_skip(false);
    }

    if (m_no_pivot.applied) {
        apply_no_pivot(false);
    }

    drop(m_character_manager);
    drop(m_camera_system);
    m_last_yaw_quat.reset();
    m_body_yaw_last_t.reset();
    m_owned_yaw.reset();
    drop(m_hmd_yaw_pcc);
    m_hmd_yaw_prev.reset();
    drop(m_hip_joint);
    m_hip_ref_offset.reset();
    m_hipjv_bad = -999.0;   // Lua-Local (Z.369)

    for (auto& [k, h] : m_spine_joint_cache) {
        drop(h);
    }

    m_spine_joint_cache.clear();
    m_spine_last_written.clear();
    m_ub_lock_pose.reset();
    m_ub_lock_rel.reset();
    m_ub_lock_base.reset();
    m_ub_trim_applied.reset();

    m_spinepin_tf = nullptr;

    for (auto& h : m_spinepin_joints) {
        drop(h);
    }

    m_spinepin_joints.clear();
    m_spinepin_names.clear();
    m_spinepin_pose = Pose{};
    drop(m_spinepin_null_off);
    m_crouch_ubz_base.clear();
    m_crouch_ubz_base_valid = false;

    // [M3] Der Lua-Kommentar an dieser Stelle behauptet, crouchpin.rel bleibe
    // stehen. Das stimmt NICHT: "Reset Scripts" fuehrt die Datei neu aus, und
    // `local crouchpin = { ... }` (Lua Z.1344) legt eine frische Tabelle an --
    // rel UND cfg_tried sind danach weg, die Hock-Pose wird wieder von Platte
    // geholt. Genau das hier, sonst haengt eine Pose im Speicher, die der
    // zurueckgesetzte cfg_tried nie ersetzen kann.
    m_crouchpin_pose = Pose{};
    m_crouchpin_capture_req = false;

    re4vr::lua_set_bool("__vr_surge_bridged", false);
    m_auto_center_ctx = nullptr;
    drop(m_bw_motion);

    // ---- was das Neuladen der Datei zusaetzlich zuruecksetzt ------------
    // [KRITISCH] grav_orig ist im Original ein Datei-`local` und nach jedem
    // Reset WEG -- genau deshalb existiert die Selbstheilung (cur >= 100 ->
    // 24.0) in apply_grav_fix. Behielte der Port den Member, waere dieser Zweig
    // NIE erreichbar und die Gravitation 150 kleble nach einem Reset im ganzen
    // Spiel fest. Deshalb hier NACH dem Restore oben zuruecksetzen.
    m_grav_orig.reset();
    m_grav_active = false;

    // Ebenfalls Locals: die Pin-Versuchsmarken. Ohne das griffe der Weg
    // "Pose von Platte" nach einem Reset nicht mehr.
    m_spinepin_cfg_tried = false;
    m_spinepin_provisional = false;
    m_crouchpin_cfg_tried = false;
    m_run_blend = 0.0;
    m_walk_blend = 0.0;

    m_stop_skip = SkipState{};
    m_start_skip = SkipState{};
    m_no_pivot = SkipState{};

    m_zone_prev = false;
    m_zone_t.reset();
    m_zone_win = 0.5;

    m_hmd_follow_last_t.reset();
    m_rs_prev_ks = false;
    m_rs_prev_pos.reset();
    m_rs_prev_hmd.reset();
    m_rs_pending = glm::vec2{0.0f, 0.0f};
    m_rs_crouch_next_t = 0.0;

    drop(m_pg_pause);
    drop(m_pg_gui);
    m_lagm = Lagm{};

    m_sqab_active = false;
    m_sqab_last.reset();
    m_sqab_pin = false;
    m_sqab_pin_pos.reset();
    m_sqab_skip_valid = false;

    m_cfg = Cfg{};
    load_cfg();
}

// ============================================================================
// UI (Lua Z.2537-2764)
// ============================================================================

void RE4VRMovement::draw_public_roomscale() {
    // ------------------------------------------------------------------
    // [PUBLIC-UI ROOMSCALE] 1:1 aus re4_vr_binding.lua Z.3158-3245: Kopie des
    // Roomscale-Hakens aus dem Movement-Tree, darunter -- und nur bei
    // angehaktem Roomscale -- die Lehn-, Recenter- und Duck-Regler.
    // Die Werte liegen samt Persistenz in RE4VRMovement; hier steht nur die
    // Anzeige, deshalb zeigen Dev-Tree und nacktes Menue immer dasselbe.
    // Beim Port ging der Block verloren: er hing am Lua-Dispatcher, den es seit
    // der Abschaltung von ##re4_vr_menu.lua nicht mehr gibt.
    // ------------------------------------------------------------------
    if (g_framework->draw_menu_checkbox("Enable Roomscale", &m_cfg.roomscale)) {
        save_cfg();
    }

    if (!m_cfg.roomscale) {
        return;
    }

    // [LEHNEN IMMER AN] Wie in der Lua: der Toggle wird im oeffentlichen Menue
    // nicht gezeigt, der Wert stattdessen aktiv auf true gehalten. Der Dev-Tree
    // in RE4VRMovement kann ihn weiterhin umschalten.
    if (!m_cfg.rs_lean) {
        m_cfg.rs_lean = true;
        save_cfg();
    }

    {
        auto rad = static_cast<float>(m_cfg.rs_lean_radius);

        if (ImGui::DragFloat("   Lean radius (m)", &rad, 0.01f, 0.0f, 1.0f, "%.2f")) {
            m_cfg.rs_lean_radius = static_cast<double>(rad);
            save_cfg();
        }
    }

    if (g_framework->draw_menu_checkbox("   Automatic Recenter after Cutscene", &m_cfg.rs_recenter)) {
        save_cfg();
    }
}

void RE4VRMovement::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRMovement", "on_draw_ui");
    if (!ImGui::TreeNode("RE4VR - Movement")) {
        return;
    }

    const auto cb = [&](const char* label, bool& v) -> bool {
        if (ImGui::Checkbox(label, &v)) {
            save_cfg();
            return true;
        }

        return false;
    };

    const auto df = [&](const char* label, double& v, float speed, float lo, float hi,
                        const char* fmt) -> bool {
        float f = static_cast<float>(v);

        if (ImGui::DragFloat(label, &f, speed, lo, hi, fmt)) {
            v = static_cast<double>(f);
            save_cfg();
            return true;
        }

        return false;
    };

    const auto sf = [&](const char* label, double& v, float lo, float hi,
                        const char* fmt) -> bool {
        float f = static_cast<float>(v);

        if (ImGui::SliderFloat(label, &f, lo, hi, fmt)) {
            v = static_cast<double>(f);
            save_cfg();
            return true;
        }

        return false;
    };

    cb("Koerper folgt Blickrichtung", m_cfg.enabled);

    if (ImGui::Checkbox("HMD treibt _Yaw (HMD Movement)", &m_cfg.hmd_yaw_drive)) {
        m_hmd_yaw_prev.reset();
        save_cfg();
    }

    df("Koerper-Drehung Offset (Grad)", m_cfg.yaw_offset_deg, 0.1f, -180.0f, 180.0f, "%.1f");
    df("Koerper-Drehgeschwindigkeit (120 = 1:1)", m_cfg.body_yaw_speed, 0.1f, 1.0f, 120.0f,
       "%.1f");
    cb("Koerper folgt auch Kopfdrehung", m_cfg.yaw_hmd_target);
    cb("Cam-Body-Rotate (Ziel inkl. HMD)", m_cfg.cam_body_rotate);

    ImGui::Separator();
    cb("Roomscale (1:1 statt Nachlaufen)", m_cfg.roomscale);

    if (m_cfg.roomscale) {
        ImGui::TextColored(ImVec4{0.40f, 0.80f, 1.00f, 1.0f},
                           "   Achtung: KEINE Kollision -- physisch in eine Wand laufen"
                           " schiebt dich hindurch. Staerke-Regler ist waehrenddessen"
                           " wirkungslos.");

        cb("   Lehnen erlauben (Body bleibt stehen)", m_cfg.rs_lean);

        if (m_cfg.rs_lean) {
            df("   Lehn-Radius (m)", m_cfg.rs_lean_radius, 0.01f, 0.0f, 1.0f, "%.2f");
        }

        cb("   Recentering nach Cutscene/Teleport", m_cfg.rs_recenter);
    }

    cb("Koerper folgt HMD-Position", m_cfg.hmd_follow);
    df("HMD-Position Deadzone (m)", m_cfg.hmd_follow_deadzone, 0.001f, 0.0f, 0.2f, "%.3f");
    sf("HMD-Position Staerke", m_cfg.hmd_follow_alpha, 0.02f, 1.0f, "%.2f");

    ImGui::Separator();
    cb("Huefte hart mitdrehen", m_cfg.hip_follow);

    if (ImGui::Checkbox("Oberkoerper steif", &m_cfg.ub_lock)) {
        m_ub_lock_pose.reset();   // bei (Re-)Aktivierung frisch capturen
        m_ub_lock_rel.reset();
        m_ub_lock_base.reset();
        save_cfg();
    }

    if (m_cfg.ub_lock) {
        if (ImGui::Checkbox("Oberkoerper steif: Welt-Modus", &m_cfg.ub_lock_world)) {
            m_ub_lock_pose.reset();
            m_ub_lock_rel.reset();
            m_ub_lock_base.reset();
            save_cfg();
        }

        df("Oberkoerper Yaw-Trim (Grad)", m_cfg.ub_trim_deg, 0.1f, -90.0f, 90.0f, "%.1f");
    }

    if (ImGui::Checkbox("Spine2+Necks festnageln (Pos+Rot, hart)", &m_cfg.spine_pin)) {
        m_spinepin_pose = Pose{};   // bei (Re-)Aktivierung frisch im Stand capturen
        save_cfg();
    }

    if (m_cfg.spine_pin) {
        if (is_ks_active()) {
            ImGui::TextColored(ImVec4{1.0f, 0.84f, 0.0f, 1.0f}, "Pin: BLOCKIERT (Killswitch aktiv!)");
        } else if (m_spinepin_pose.valid) {
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Pin: AKTIV");
        } else {
            ImGui::TextColored(ImVec4{1.0f, 0.84f, 0.0f, 1.0f}, "Pin: warte auf ruhigen Stand...");
        }

        if (ImGui::Button("Pin-Pose neu aufnehmen (ruhig stehen!)")) {
            m_spinepin_pose = Pose{};
            m_spinepin_cfg_tried = true;   // nicht die alte Pose von Platte ziehen
            m_cfg.pin_pose = nlohmann::json{};
            save_cfg();
        }

        df("Beine nach hinten (Rennen) (m)", m_cfg.pin_z.Hip, 0.001f, -0.1f, 0.4f, "%.3f");
        df("Beine nach hinten (normales Gehen) (m)", m_cfg.pin_z_hip_walk, 0.001f, -0.1f, 0.4f,
           "%.3f");

        // [nil-SENTINEL] Solange nichts eingestellt wurde, zeigt der Regler den
        // geerbten Renn-Wert; die erste Bewegung entkoppelt ihn.
        {
            float f = static_cast<float>(m_cfg.pin_z_hip_crouch.value_or(m_cfg.pin_z.Hip));

            if (ImGui::DragFloat("Beine nach hinten (Crouch) (m)", &f, 0.001f, -0.1f, 0.4f,
                                 "%.3f")) {
                m_cfg.pin_z_hip_crouch = static_cast<double>(f);
                save_cfg();
            }

            if (!m_cfg.pin_z_hip_crouch.has_value()) {
                ImGui::TextColored(ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
                                   "   (noch nicht entkoppelt - erbt gerade den Rennen-Wert)");
            }
        }

        df("Oberkoerper nach hinten (m)", m_cfg.pin_ub_z, 0.001f, -0.1f, 0.15f, "%.3f");
        df("Oberkoerper nach hinten IM CROUCH (m)", m_cfg.pin_ub_z_crouch, 0.001f, -0.2f, 0.2f,
           "%.3f");

        if (m_crouchpin_capture_req) {
            ImGui::TextColored(ImVec4{1.0f, 0.84f, 0.0f, 1.0f},
                               "Crouch-Pin: ducke dich + still halten...");
        } else if (m_crouchpin_pose.valid) {
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "Crouch-Pin: AKTIV");
        } else {
            ImGui::TextColored(ImVec4{0.67f, 0.67f, 0.67f, 1.0f},
                               "Crouch-Pin: AUS (native Hocke)");
        }

        if (ImGui::Button("Crouch-Pose pinnen (IM DUCKEN druecken!)")) {
            m_crouchpin_capture_req = true;
        }

        if (ImGui::Button("Crouch-Pin entfernen")) {
            m_crouchpin_pose = Pose{};
            m_crouchpin_capture_req = false;
            m_crouchpin_cfg_tried = true;   // nicht die alte Pose von Platte ziehen
            m_cfg.crouch_pose = nlohmann::json{};
            save_cfg();
        }

        df("Oberkoerper seitlich (m) [nicht im Crouch]", m_cfg.pin_ub_x, 0.001f, -0.15f, 0.15f,
           "%.3f");
        df("Oberkoerper seitlich IM CROUCH (m)", m_cfg.pin_ub_x_crouch, 0.001f, -0.15f, 0.15f,
           "%.3f");
        df("Oberkoerper Yaw (Grad)", m_cfg.pin_ub_yaw, 0.1f, -45.0f, 45.0f, "%.1f");
    }

    if (ImGui::Checkbox("Stop-Animationen ueberspringen", &m_cfg.stop_skip)) {
        apply_stop_skip(m_cfg.stop_skip);
        save_cfg();
    }

    if (m_cfg.stop_skip) {
        float f = static_cast<float>(m_cfg.stop_skip_frames);

        if (ImGui::DragFloat("Stop-Animationen Frameskip", &f, 0.5f, 0.0f, 120.0f, "%.0f")) {
            m_cfg.stop_skip_frames = static_cast<double>(f);
            apply_stop_skip(true);
            save_cfg();
        }
    }

    ImGui::Text("Stop-Skip: %s", m_stop_skip.applied ? "AN" : "-");

    if (ImGui::Checkbox("Start-Animationen ueberspringen", &m_cfg.start_skip)) {
        apply_start_skip(m_cfg.start_skip);
        save_cfg();
    }

    if (m_cfg.start_skip) {
        float f = static_cast<float>(m_cfg.start_skip_frames);

        if (ImGui::DragFloat("Start-Animationen Frameskip", &f, 0.5f, 0.0f, 120.0f, "%.0f")) {
            m_cfg.start_skip_frames = static_cast<double>(f);
            apply_start_skip(true);
            save_cfg();
        }
    }

    ImGui::Text("Start-Skip: %s", m_start_skip.applied ? "AN" : "-");

    if (ImGui::Checkbox("Dreh-Animationen deaktivieren", &m_cfg.no_pivot)) {
        apply_no_pivot(m_cfg.no_pivot);
        save_cfg();
    }

    ImGui::Text("Dreh-Anims: %s", m_no_pivot.applied ? "AUS" : "an");

    df("Oberkoerper-Drehung manuell (Grad)", m_cfg.spine_yaw_deg, 0.1f, -90.0f, 90.0f, "%.1f");

    if (m_owned_yaw.has_value()) {
        ImGui::Text("Yaw-Quat: %s   Owned: %.0f Grad   KS: %s",
                    m_last_yaw_quat.has_value() ? "ok" : "-", rad2deg(*m_owned_yaw),
                    is_ks_active() ? "true" : "false");
    } else {
        ImGui::Text("Yaw-Quat: %s   Owned: -   KS: %s", m_last_yaw_quat.has_value() ? "ok" : "-",
                    is_ks_active() ? "true" : "false");
    }

    ImGui::Text("--- Stepfall-Fix (Treppab rueckwaerts) ---");
    cb("Gravitation waehrend Bewegung anheben", m_cfg.grav_fix);
    df("Gravitation (Original 24)", m_cfg.grav_value, 1.0f, 0.0f, 400.0f, "%.0f");

    if (m_grav_orig.has_value()) {
        ImGui::Text("orig=%.1f   greift_jetzt=%s   (bei JEDEM Killswitch aus)", *m_grav_orig,
                    m_grav_active ? "true" : "false");
    } else {
        ImGui::Text("orig=nil   greift_jetzt=%s   (bei JEDEM Killswitch aus)",
                    m_grav_active ? "true" : "false");
    }

    ImGui::Text("--- Anlaufen / Stehenbleiben (Lag) ---");
    cb("Anlauf-Boost", m_cfg.lag_boost_on);
    df("Boost: Zielgeschwindigkeit (m/s)", m_cfg.lag_boost_target, 0.01f, 0.0f, 6.0f, "%.2f");
    df("Boost: max. Zuschuss (m/s)", m_cfg.lag_boost_max, 0.01f, 0.0f, 6.0f, "%.2f");
    df("Boost: Fenster (s)", m_cfg.lag_boost_window, 0.01f, 0.0f, 1.0f, "%.2f");
    cb("Nachlauf-Bremse", m_cfg.lag_brake_on);
    df("Bremse: Nachlauf-Anteil (0=sofort stehen)", m_cfg.lag_brake_gain, 0.01f, 0.0f, 1.0f,
       "%.2f");
    df("Bremse: Fenster (s, > Nachlauf ~0.5)", m_cfg.lag_brake_window, 0.01f, 0.0f, 2.0f, "%.2f");
    ImGui::Text("speed=%.2f m/s   boost=%.2f   bremst=%s", m_lagm.spd, m_lagm.boost_now,
                m_lagm.brake_now ? "true" : "false");

    ImGui::TreePop();
}

#endif // RE4
