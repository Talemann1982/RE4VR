// ============================================================================
// RE4VRReload4 -- 1:1-Portierung von re4_vr_reload.lua (7.007 Zeilen).
// Spezifikation: I:\LUATRANS\PORT_RELOAD1_SPEC.md
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <array>
#include <span>
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
#include <sdk/SceneManager.hpp>

#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRWeapons2.hpp"
#include "RE4VRReloadAdv.hpp"
#include "RE4VRReloadMain.hpp"
#include "RE4VRReload4.hpp"

#undef min
#undef max

// ============================================================================
// Lokale Helfer
// ============================================================================
namespace {

constexpr const char* CFG_PATH = "re4_vr/re4_vr_reload4_dlc.json";
constexpr float DOCK_BLEND_SPEED = 0.10f;
constexpr float CHAMBER_BLEND_SPEED = 0.10f;
constexpr float DROP_FALL_DUR = 1.0f;          // [DROP-AUTOCLEAR]
constexpr uint32_t BREAK_PUMP_SND_ID = 3370013164u;
constexpr float SKULLSHAKER_PUMP_FORCE_DUR = 0.55f;

// Lua: os.clock().
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

float ease(float t) {   // smoothstep
    return t * t * (3.0f - 2.0f * t);
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

// via.Transform/via.Joint liefern Position/Rotation als ValueType -- die
// brauchen den sret-Puffer, sonst schreibt die Engine in den VMContext.
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

// Quaternion aus Euler-Grad -- Lua baut qz*qy*qx (Quaternion.new ist W,X,Y,Z).
// ACHTUNG: reload.lua normalisiert hier NICHT (anders als reload_adv).
glm::quat quat_from_euler(float rx, float ry, float rz) {
    const float hx = glm::radians(rx) * 0.5f;
    const float hy = glm::radians(ry) * 0.5f;
    const float hz = glm::radians(rz) * 0.5f;

    const glm::quat qx{std::cos(hx), std::sin(hx), 0.0f, 0.0f};
    const glm::quat qy{std::cos(hy), 0.0f, std::sin(hy), 0.0f};
    const glm::quat qz{std::cos(hz), 0.0f, 0.0f, std::sin(hz)};

    return qz * qy * qx;
}

// Normalisierte Quaternion-Interpolation (kuerzester Weg via Vorzeichen-Check).
glm::quat qnlerp(const glm::quat& a, const glm::quat& b, float t) {
    glm::quat bb = b;

    if ((a.w * bb.w + a.x * bb.x + a.y * bb.y + a.z * bb.z) < 0.0f) {
        bb = glm::quat{-bb.w, -bb.x, -bb.y, -bb.z};
    }

    const glm::quat r{a.w + (bb.w - a.w) * t, a.x + (bb.x - a.x) * t,
                      a.y + (bb.y - a.y) * t, a.z + (bb.z - a.z) * t};

    const float l = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);

    if (l < 1e-6f) {
        return a;
    }

    return glm::quat{r.w / l, r.x / l, r.y / l, r.z / l};
}

glm::vec3 vec_sub(const glm::vec3& a, const glm::vec3& b) {
    return glm::vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

float vec_len(const glm::vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Einen Joint der Transform holen (getJointByName(System.String)).
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

// Enum-Getter, dessen Ergebnis eine ZAHL ist. Ueber die TypeDB kommt der Enum
// direkt als Integer -- das value__-Auspacken der Lua-Fassung entfaellt.
std::optional<int32_t> call_enum(::REManagedObject* obj, std::string_view name) {
    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(obj, name, v)) {
        return std::nullopt;
    }

    return v;
}

}   // namespace

// ============================================================================
// Allowlisten / Waffendaten (Lua: Datei-Locals mit Code-Konstanten)
// ============================================================================

bool RE4VRReload4::is_shotgun(int32_t wid) {
    // [DLC] nur die Sawed-off W-870 (Basis: 4100).
    return wid == 6100;
}

bool RE4VRReload4::is_pistol(int32_t wid) {
    // [DLC] Blacktail AC (Basis 4003) und Punisher MC (Basis 4001).
    // [6113 RAUS] Die Samurai Edge ist EXAKT eine Red9 -> Top-Loader, kein Mag;
    // sie lebt komplett in reload5_dlc. NIE hier wieder eintragen: zwei Teile
    // duerfen nie dieselbe WeaponID verwalten.
    return wid == 6103 || wid == 6112;
}

bool RE4VRReload4::is_smg(int32_t wid) {
    // [DLC] MP-AF (TMP-Klon). Die Chicago w/ Drum (6101) lebt in reload5_dlc.
    return wid == 6104;
}

const char* RE4VRReload4::category_of(int32_t wid) {
    if (is_pistol(wid)) {
        return "pistols";
    }

    if (is_smg(wid)) {
        return "smgs";
    }

    if (is_shotgun(wid)) {
        return "shotguns";
    }

    // magnum / rifles sind leer
    return nullptr;
}

bool RE4VRReload4::category_enabled(const char* cat) const {
    if (cat == nullptr) {
        return false;
    }

    if (std::strcmp(cat, "pistols") == 0)  { return cfg.pistols_enabled; }
    if (std::strcmp(cat, "smgs") == 0)     { return cfg.smgs_enabled; }
    if (std::strcmp(cat, "shotguns") == 0) { return cfg.shotguns_enabled; }
    if (std::strcmp(cat, "magnum") == 0)   { return cfg.magnum_enabled; }
    if (std::strcmp(cat, "rifles") == 0)   { return cfg.rifles_enabled; }

    return false;
}

// Waffen, die NACH dem Schuss KEIN Cyceln brauchen (frei ballern bis 0).
// [DLC] leer: die Sawed-off ist wie die W-870 eine echte Pump-Gun -> der Pump
// nach dem Schuss bleibt.
bool RE4VRReload4::no_cycle_after_shot(int32_t wid) {
    (void)wid;

    return false;
}

// Nach einem TAKTISCHEN Shell-Insert kein Cycle/Pump.
// [DLC] leer (war nur die Riot Gun 4101).
bool RE4VRReload4::no_reload_cycle(int32_t wid) {
    (void)wid;

    return false;
}

// Feuer-Block/Dry-Fire NUR bei wirklich leerer Waffe.
// [DLC] leer (war nur der Striker 4102).
bool RE4VRReload4::dryfire_only_when_empty(int32_t wid) {
    (void)wid;

    return false;
}

// "slide"-Joint ist ein DREHSCHALTER, kein Z-Slide.
// [DLC] leer: kein Drehschalter im reload4-Set.
bool RE4VRReload4::rotary_cycle(int32_t wid) {
    (void)wid;

    return false;
}

// Lever/Klappe-Shotgun.
// [DLC] leer: keine Break-Action im reload4-Set.
bool RE4VRReload4::break_action(int32_t wid) {
    (void)wid;

    return false;
}

// [CHAMBER_HOLD_PERSIST] Nach-Rack-Halt NICHT beim 1. Schuss loesen, sondern
// bis die Waffe wirklich leer ist. Gilt fuer ALLE Standard-Pistolen.
bool RE4VRReload4::chamber_hold_persist(int32_t wid) {
    // [DLC] die beiden Standardpistolen (6113 -> reload5_dlc).
    return wid == 6103 || wid == 6112;
}

// [ENGINE_CLOSES_SLIDE] Das Schliessen macht die Engine selbst -- NUR die LE 5.
// (W-870 RAUS: wir halten das Pump-Joint _01 selbst auf rest_z, um den nativen
// AfterShoot-Pump zu unterdruecken. TMP RAUS seit dem echten Slide-Rack auf _09.)
bool RE4VRReload4::engine_closes_slide(int32_t wid) {
    // [DLC] leer. Die MP-AF (6104) bekommt ein echtes Slide-Rack ueber Joint
    // _09 -- mit "Engine schliesst selbst" gaebe es gar keine Rack-Bedingung.
    (void)wid;

    return false;
}

// [PARENT-RAUM-DOCK] Mag-Joint _04 an einem anderen Parent als die Pistolen.
bool RE4VRReload4::parent_space_dock(int32_t wid) {
    return wid == 6104;   // MP-AF (Adas TMP)
}

// [EMPTY-RELOAD SLIDE] eigener Lade-Slide-Joint fuers Chambern nach Leerschuss.
const char* RE4VRReload4::empty_reload_joint(int32_t wid) {
    // [DLC] leer: die Basis 4100 (W-870) hat keinen _08-Lade-Slide.
    (void)wid;

    return nullptr;
}

const char* RE4VRReload4::rack_pose_empty(int32_t wid) {
    (void)wid;   // [DLC] leer (war nur die Riot Gun 4101)

    return nullptr;
}

// [SHOTGUN] Z-Offset von der _04-Shell-Ruhepose zum CHAMBER-Port.
std::optional<float> RE4VRReload4::shotgun_chamber_z(int32_t wid) {
    // [DLC] Sawed-off W-870 = 1:1 die W-870 (4100).
    return (wid == 6100) ? std::optional<float>{-0.093f} : std::nullopt;
}

// [SG PUMP MUTE] Engine-Auto-Pump-Sound-ID pro Waffe.
std::optional<uint32_t> RE4VRReload4::auto_pump_mute(int32_t wid) {
    // [DLC] Sawed-off: die W-870-Auto-Pump-ID als Start.
    return (wid == 6100) ? std::optional<uint32_t>{1964290782u} : std::nullopt;
}

const char* RE4VRReload4::weapon_name(int32_t wid) {
    switch (wid) {
    case 4000: return "SG-09 R";
    case 4001: return "Punisher";
    case 4002: return "Red9";
    case 4003: return "Blacktail";
    case 4004: return "Matilda";
    case 4005: return "Don Quixote";
    case 4100: return "W-870";
    case 4101: return "Riot Gun";
    case 4102: return "Striker";
    case 4200: return "TMP";
    case 4201: return "Chicago Sweeper";
    case 4202: return "LE 5";
    case 4400: return "SR M1903";
    case 4401: return "Stingray";
    case 4402: return "CQBR Assault Rifle";
    case 4500: return "Broken Butterfly";
    case 4501: return "Killer7";
    case 4502: return "Handcannon";
    case 4600: return "Bolt Thrower";
    case 4701: return "Flamethrower";
    case 4702: return "P.R.L. 9412";
    case 4800: return "Silent Crossbow";
    case 4801: return "XJF-350 Compound Bow";
    case 4900: return "Rocket Launcher";
    case 4901: return "Rocket Launcher (Special)";
    case 4902: return "Infinite Rocket Launcher";
    case 5000: return "Combat Knife";
    case 5001: return "Fighting Knife";
    case 5002: return "Kitchen Knife";
    case 5003: return "Boot Knife";
    case 5004: return "Water Pipe";
    case 5005: return "Giant Rock";
    case 5006: return "Primal Knife";
    case 5400: return "Hand Grenade";
    case 5401: return "Heavy Grenade";
    case 5402: return "Flash Grenade";
    case 5403: return "Chicken Egg";
    case 5404: return "Brown Chicken Egg";
    case 5405: return "Gold Chicken Egg";
    case 5500: return "Unlimited Bottomless Ammo";
    case 6000: return "Sentinel Nine";
    case 6001: return "Skull Shaker";
    case 6100: return "Sawed-off W-870";
    case 6101: return "Chicago Sweeper w/ Drum";
    case 6102: return "Blast Crossbow";
    case 6103: return "Blacktail AC";
    case 6104: return "MP-AF";
    case 6105: return "Anti-Materiel Rifle";
    case 6106: return "Rocket Launcher (SW)";
    case 6107: return "Tactical Knife";
    case 6108: return "Elite Knife";
    case 6109: return "Scorcher XL";
    case 6110: return "XM96E1";
    case 6111: return "Infinite Rocket Launcher (SW)";
    case 6112: return "Punisher MC";
    case 6113: return "Samurai Edge";
    case 6114: return "Hunting Rifle";
    case 6300: return "XM96E1 (MC)";
    case 6301: return "Blacktail AC (MC)";
    case 6302: return "Quickdraw Army";
    case 6304: return "EJF-338 Compound Bow";
    case 6305: return "Hot Dogger";
    default:   return nullptr;
    }
}

std::string RE4VRReload4::wp_label(int32_t wid) {
    const char* n = weapon_name(wid);
    char buf[96]{};

    if (n != nullptr) {
        std::snprintf(buf, sizeof(buf), "wp%d (%s)", wid, n);
    } else {
        std::snprintf(buf, sizeof(buf), "wp%d", wid);
    }

    return buf;
}

// [SND] Trigger-IDs auf dem SoundContainer der equippten Waffe, PER WAFFE.
std::optional<uint32_t> RE4VRReload4::snd_id(int32_t wid, const char* key) {
    // Die Punisher-IDs sind der Satz, den fast alle Waffen erben.
    struct Set {
        uint32_t dry_fire, mag_eject, mag_insert, mag_floor, slide_back, mag_holster;
        uint32_t extra;         // chamber / cycle / break_open (0 = keiner)
        const char* extra_key;
    };

    static const Set PUNISHER{812850326u, 1466005368u, 1757452382u,
                              3140689763u, 943565871u, 1839787494u, 0u, nullptr};

    Set s{};

    // [DLC] Die IDs sind 1:1 von der jeweiligen Basiswaffe uebernommen. Falls im
    // DLC etwas stumm oder falsch klingt: die echten IDs mit dem Sound-Player
    // ziehen und hier eintragen.
    switch (wid) {
    case 6112:   // Punisher MC <- Punisher (4001)
    case 6103:   // Blacktail AC <- Blacktail (4003, = Punisher-IDs)
    case 6104:   // MP-AF <- TMP (4200)
        s = PUNISHER;
        break;
    case 6100:   // Sawed-off <- W-870 (4100)
        s = Set{812850326u, 1466005368u, 942865223u,
                1351699582u, 741230436u, 1839787494u, 0u, nullptr};
        break;
    default:
        return std::nullopt;
    }

    if (std::strcmp(key, "dry_fire") == 0)    { return s.dry_fire; }
    if (std::strcmp(key, "mag_eject") == 0)   { return s.mag_eject; }
    if (std::strcmp(key, "mag_insert") == 0)  { return s.mag_insert; }
    if (std::strcmp(key, "mag_floor") == 0)   { return s.mag_floor; }
    if (std::strcmp(key, "slide_back") == 0)  { return s.slide_back; }
    if (std::strcmp(key, "mag_holster") == 0) { return s.mag_holster; }

    if (s.extra_key != nullptr && std::strcmp(key, s.extra_key) == 0) {
        return s.extra;
    }

    return std::nullopt;
}

// ============================================================================
// Tabellen-Zugriff mit Auto-Anlegen (Lua: maghand/shell_eject_cfg/slide_pose/...)
// ============================================================================

RE4VRReload4::MagHand& RE4VRReload4::maghand(int32_t wid) {
    return m_maghand[wid];
}

RE4VRReload4::ShellEject& RE4VRReload4::shell_eject_cfg(int32_t wid) {
    return m_shell_eject[wid];
}

RE4VRReload4::SlidePose& RE4VRReload4::slide_pose(int32_t wid) {
    return m_slide_pose[wid];
}

RE4VRReload4::SlidePose& RE4VRReload4::slide_pose2(int32_t wid) {
    return m_slide_pose2[wid];
}

RE4VRReload4::Rotary& RE4VRReload4::rotary_cfg(int32_t wid) {
    auto it = m_rotary_cfg.find(wid);

    if (it == m_rotary_cfg.end()) {
        it = m_rotary_cfg.emplace(wid, Rotary{}).first;
    }

    return it->second;
}

// ============================================================================
// Initialisierung -- die Code-Tabellen aus re4_vr_reload.lua
// ============================================================================

RE4VRReload4* RE4VRReload4::s_instance = nullptr;

void RE4VRReload4::on_initialize() {
    s_instance = this;

    // ---- Per-Waffe Joints (Lua: JOINTS, Z.1470) ----------------------
    // [JOINTS CODE-ONLY] Sie werden BEWUSST NICHT aus der JSON geladen: ein
    // verklickter Cycler-Wert hat frueher still den Code-Default ueberstimmt.
    const auto J = [&](int32_t wid, const char* mag, const char* slide) {
        m_joint_mag[wid] = mag;
        m_joint_slide[wid] = slide;
    };

    // [DLC] 1:1 von der jeweiligen Basiswaffe.
    J(6112, "_14", "_01");   // Punisher MC   <- 4001
    J(6103, "_14", "_01");   // Blacktail AC  <- 4003
    J(6104, "_04", "_09");   // MP-AF         <- TMP 4200
    J(6100, "_04", "_01");   // Sawed-off     <- W-870 4100 (Shell=_04, Pump=_01)

    // ---- Slide-Rack-Posen (Lua: SLIDE_POSE, Z.326) -------------------
    // Pose 2 (sdock_*/srack_*) kommt aus den Struct-Defaults = die
    // Blacktail-Startwerte aus SLIDE_POSE_DEFAULT.
    const auto SP = [&](int32_t wid, float rest, float park, float back,
                        float dx, float dy, float dz,
                        float rx, float ry, float rz) {
        SlidePose p{};
        p.rest_z = rest; p.park_z = park; p.back_z = back;
        p.dock_x = dx; p.dock_y = dy; p.dock_z = dz;
        p.rack_rx = rx; p.rack_ry = ry; p.rack_rz = rz;
        m_slide_pose[wid] = p;
    };

    // [DLC] Alle Werte 1:1 aus reload von der jeweiligen Basiswaffe.
    SP(6112, 0.10042f, 0.08542f, 0.06042f, 0.045f, 0.032f, -0.192f, 53.7f, 285.7f, -86.1f);
    SP(6103, 0.10042f, 0.08542f, 0.06042f, 0.045f, 0.032f, -0.192f, 53.7f, 285.7f, -86.1f);
    // [TMP _09 -- gemessen] Ruhe (Slide ZU) = -0.07458. Die TMP hat KEIN
    // Hold-Open: der Slide steht auch bei leerer Waffe vorne -> park_z = rest_z.
    // Der Rack ist ein kurzer Zug nach hinten (back_z) und wieder vor. rest_z war
    // frueher 0.10042 -- falsch von der Punisher geerbt.
    SP(6104, -0.07458f, -0.07458f, -0.10500f, 0.045f, 0.032f, -0.192f, 53.7f, 285.7f, -86.1f);
    // [SHOTGUN] "slide" = PUMP-Joint _01, rest_z = vorn/gechambert.
    SP(6100, 0.42500f, 0.42500f, 0.30000f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    // ---- Andock-Punkte (Lua: DOCK_PORT / LEVER_PORT) -----------------
    // [DLC] leer: 4001/4003/4200/4100 haben keinen eigenen Andock-Port (das
    // waren LE5, Striker und Skull Shaker).
    //
    // ---- Drehschalter / Klapphebel (Lua: ROTARY) ---------------------
    // [DLC] leer: kein Drehschalter und keine Break-Action im reload4-Set.

    // ---- Hand-Posen pro Waffe (Lua: MAG_POSE / RACK_POSE) ------------
    // [DLC] Die MP-AF erbt die TMP-Pose, die Sawed-off die Shotgun-Shell-Pose.
    // [PUNISHER/BLACKTAIL] 6112/6103 stehen EXPLIZIT auf "MAG": Leons Pistolen
    // holen sich diese Pose ueber den GLOBALEN Wert (cfg.mag_hold_pose = "MAG").
    // Bei Ada stand der global auf "" (leer = keine Pose, Finger frei) -> das
    // Punisher-Magazin wurde ohne Handpose gehalten. Explizit pro Waffe ist
    // stabiler: es ueberlebt jeden Klick auf "Globale Pose entfernen".
    m_mag_pose[6104] = "TMPMAG";
    m_mag_pose[6100] = "Shotgunshell";
    m_mag_pose[6112] = "MAG";
    m_mag_pose[6103] = "MAG";
    // [MP-AF] Der Slide nutzt dieselbe Pose wie der Samurai-Edge-Slide
    // ("Red9Slide" ist laut Code eine 1:1-Kopie von "rack-slide").
    m_rack_pose[6100] = "SGPUMP";     // Sawed-off = W-870-Pumpgriff
    m_rack_pose[6103] = "MAGRack";    // Blacktail AC (wie 4003)
    m_rack_pose[6104] = "rack-slide";

    // ---- Shell-Sub-Mesh-Parts (Lua: SHELL_PART_FIXED) ----------------
    // [DLC] Sawed-off W-870 (6100): Part 1 = die HUELSE. Beleg steht im
    // Reload-Code selbst -- beim Striker (4102) ist Part 1 A/B live verifiziert,
    // der Skull-Shaker-Clone isoliert Part 1, und `shell_parts` in
    // re4_vr_reload.json steht auf 6001:[1]. Die Sawed-off hat weder
    // empty_reload_joint noch einen Lern-Diff-Pfad, lernt den Part also nie
    // selbst -> ohne diesen Eintrag bliebe die Shell leer und der Mesh-Clone
    // zeigte Part 0 = das GANZE Waffen-Mesh (man lud eine komplette Shotgun in
    // die Shotgun).
    m_shell_parts[6100] = {1};

    // ---- Insert-Distanz pro GATTUNG ----------------------------------
    m_insert_dist["pistols"]  = 0.15f;
    m_insert_dist["smgs"]     = 0.15f;
    m_insert_dist["shotguns"] = 0.15f;
    m_insert_dist["magnum"]   = 0.15f;
    m_insert_dist["rifles"]   = 0.15f;

    // ---- chainsaw.Gun.State: Enum-Werte + Namen ----------------------
    if (auto* td = sdk::find_type_definition("chainsaw.Gun.State"); td != nullptr) {
        if (auto* f = td->get_field("AmmoEmpty"); f != nullptr) {
            m_ammoempty_num = f->get_data<int32_t>(nullptr);
        }

        if (auto* f = td->get_field("Holding"); f != nullptr) {
            m_gun_state_holding = f->get_data<int32_t>(nullptr);
        }

        for (auto* f : td->get_fields()) {
            if (f != nullptr && f->is_static()) {
                m_state_names[f->get_data<int32_t>(nullptr)] = f->get_name();
            }
        }
    }

    load_cfg();
    // [POSE-STORE] importierte Posen (aus gestures.json) sofort zurueckschreiben
    // -> die Daten liegen danach in reload.json.
    save_cfg();
}

// ============================================================================
// Persistenz (Lua Z.1584-1818)
// ============================================================================
namespace {

float jnum(const nlohmann::json& j, const char* key, float def) {
    if (!j.is_object()) {
        return def;
    }

    const auto it = j.find(key);

    return (it != j.end() && it->is_number()) ? it->get<float>() : def;
}

bool jhas_num(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) {
        return false;
    }

    const auto it = j.find(key);

    return it != j.end() && it->is_number();
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

void RE4VRReload4::load_cfg() {
    const auto data = re4vr::json_load(CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    // In der Live-JSON liegt die Konfiguration unter "cfg"; die alte Fassung
    // hatte sie flach. Lua: c = data.cfg or data.
    const nlohmann::json& c = (data.contains("cfg") && data["cfg"].is_object())
                              ? data["cfg"] : data;

    // "enabled" (Master) wird NICHT geladen -> bleibt immer true.
    cfg.pistols_enabled  = jbool(c, "pistols_enabled",  cfg.pistols_enabled);
    cfg.smgs_enabled     = jbool(c, "smgs_enabled",     cfg.smgs_enabled);
    cfg.shotguns_enabled = jbool(c, "shotguns_enabled", cfg.shotguns_enabled);
    cfg.magnum_enabled   = jbool(c, "magnum_enabled",   cfg.magnum_enabled);
    cfg.rifles_enabled   = jbool(c, "rifles_enabled",   cfg.rifles_enabled);
    cfg.reload_ammo      = jbool(c, "reload_ammo",      cfg.reload_ammo);
    cfg.rack_enabled     = jbool(c, "rack_enabled",     cfg.rack_enabled);
    cfg.rack_haptic      = jbool(c, "rack_haptic",      cfg.rack_haptic);
    cfg.sound_enabled    = jbool(c, "sound_enabled",    cfg.sound_enabled);
    cfg.enabled = true;

    cfg.gravity            = jnum(c, "gravity",            cfg.gravity);
    cfg.insert_dur         = jnum(c, "insert_dur",         cfg.insert_dur);
    cfg.insert_distance    = jnum(c, "insert_distance",    cfg.insert_distance);
    cfg.rack_grab_dist     = jnum(c, "rack_grab_dist",     cfg.rack_grab_dist);
    cfg.mag_floor_delay    = jnum(c, "mag_floor_delay",    cfg.mag_floor_delay);
    cfg.pump_haptic_delay  = jnum(c, "pump_haptic_delay",  cfg.pump_haptic_delay);
    cfg.insert_overshoot   = jnum(c, "insert_overshoot",   cfg.insert_overshoot);
    cfg.insert_settle      = jnum(c, "insert_settle",      cfg.insert_settle);
    cfg.insert_haptic      = jnum(c, "insert_haptic",      cfg.insert_haptic);
    cfg.insert_snd_at      = jnum(c, "insert_snd_at",      cfg.insert_snd_at);
    cfg.insert_travel      = jnum(c, "insert_travel",      cfg.insert_travel);
    cfg.insert_back_out    = jnum(c, "insert_back_out",    cfg.insert_back_out);
    cfg.insert_redock      = jnum(c, "insert_redock",      cfg.insert_redock);
    cfg.insert_snap_at     = jnum(c, "insert_snap_at",     cfg.insert_snap_at);
    cfg.rack_pose_side_deg = jnum(c, "rack_pose_side_deg", cfg.rack_pose_side_deg);
    cfg.pump_start_pull    = jnum(c, "pump_start_pull",    cfg.pump_start_pull);
    cfg.pump_push_frac     = jnum(c, "pump_push_frac",     cfg.pump_push_frac);
    cfg.skull_idx1         = jnum(c, "skull_idx1",         cfg.skull_idx1);
    cfg.skull_idx2         = jnum(c, "skull_idx2",         cfg.skull_idx2);
    cfg.skull_idx3         = jnum(c, "skull_idx3",         cfg.skull_idx3);
    cfg.skull_open_deg     = jnum(c, "skull_open_deg",     cfg.skull_open_deg);
    cfg.skull_spin_dur     = jnum(c, "skull_spin_dur",     cfg.skull_spin_dur);

    if (jhas_num(c, "shotgun_ratio")) {
        cfg.shotgun_ratio = static_cast<int32_t>(jnum(c, "shotgun_ratio", 2.0f));
    }

    cfg.insert_punch  = jbool(c, "insert_punch",  cfg.insert_punch);
    cfg.insert_manual = jbool(c, "insert_manual", cfg.insert_manual);

    {
        const auto s = jstr(c, "mag_hold_pose", cfg.mag_hold_pose);

        if (!s.empty()) {
            cfg.mag_hold_pose = s;
        }
    }

    cfg.rack_pose = jstr(c, "rack_pose", cfg.rack_pose);

    {
        const auto s = jstr(c, "rack_pose_side", cfg.rack_pose_side);

        if (!s.empty()) {
            cfg.rack_pose_side = s;
        }
    }

    // Legacy: altes GLOBALES Daumen-Offset (vor der Per-Waffe-Umstellung).
    std::optional<glm::vec3> legacy_thumb{};

    if (jhas_num(c, "thumb_rx") || jhas_num(c, "thumb_ry") || jhas_num(c, "thumb_rz")) {
        legacy_thumb = glm::vec3{jnum(c, "thumb_rx", 0.0f), jnum(c, "thumb_ry", 0.0f),
                                 jnum(c, "thumb_rz", 0.0f)};
    }

    std::unordered_map<int32_t, bool> thumb_present{};

    if (const auto it = data.find("maghand"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            MagHand m{};
            m.x = jnum(v, "x", 0.0f);
            m.y = jnum(v, "y", 0.0f);
            m.z = jnum(v, "z", 0.0f);
            m.rx = jnum(v, "rx", 0.0f);
            m.ry = jnum(v, "ry", 0.0f);
            m.rz = jnum(v, "rz", 0.0f);
            m.t_rx = jnum(v, "t_rx", 0.0f);
            m.t_ry = jnum(v, "t_ry", 0.0f);
            m.t_rz = jnum(v, "t_rz", 0.0f);
            m_maghand[wid] = m;

            if (v.contains("t_rx") || v.contains("t_ry") || v.contains("t_rz")) {
                thumb_present[wid] = true;
            }
        }
    }

    // [SHELL_CLONE] Skull-Shaker-Clone-Offsets
    if (const auto it = data.find("shell_clone"); it != data.end() && it->is_object()) {
        const auto& v = *it;

        if (jhas_num(v, "part")) {
            m_ss_clone.part = static_cast<int32_t>(jnum(v, "part", 1.0f));
        }

        m_ss_clone.x = jnum(v, "x", m_ss_clone.x);
        m_ss_clone.y = jnum(v, "y", m_ss_clone.y);
        m_ss_clone.z = jnum(v, "z", m_ss_clone.z);
        m_ss_clone.rx = jnum(v, "rx", m_ss_clone.rx);
        m_ss_clone.ry = jnum(v, "ry", m_ss_clone.ry);
        m_ss_clone.rz = jnum(v, "rz", m_ss_clone.rz);
        m_ss_clone.scale = jnum(v, "scale", m_ss_clone.scale);
    }

    // [SHELL_EJECT] Chamber-Startposition pro Waffe
    if (const auto it = data.find("shell_eject"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& e = shell_eject_cfg(wid);
            e.sx = jnum(v, "sx", e.sx);
            e.sy = jnum(v, "sy", e.sy);
            e.sz = jnum(v, "sz", e.sz);
            e.srx = jnum(v, "srx", e.srx);
            e.sry = jnum(v, "sry", e.sry);
            e.srz = jnum(v, "srz", e.srz);
            e.vx = jnum(v, "vx", e.vx);
            e.vy = jnum(v, "vy", e.vy);
            e.vz = jnum(v, "vz", e.vz);
            e.grav = jnum(v, "grav", e.grav);
            e.dur = jnum(v, "dur", e.dur);
            e.spin = jnum(v, "spin", e.spin);
        }
    }

    // Migration: globaler Daumen -> Matilda, falls dort nichts gespeichert ist.
    if (legacy_thumb.has_value() && thumb_present.find(4004) == thumb_present.end()) {
        auto& m = maghand(4004);
        m.t_rx = legacy_thumb->x;
        m.t_ry = legacy_thumb->y;
        m.t_rz = legacy_thumb->z;
    }

    // [PORT] Punisher erbt Matildas Mag-Hand-Pose, falls noch nichts Eigenes.
    if (m_maghand.find(4001) == m_maghand.end() && m_maghand.find(4004) != m_maghand.end()) {
        m_maghand[4001] = m_maghand[4004];
    }

    // SG-09 R / Blacktail / Sentinel Nine / SMGs erben 1:1 die Punisher-Pose.
    if (m_maghand.find(4001) != m_maghand.end()) {
        const MagHand s = m_maghand[4001];

        for (int32_t w : {4000, 4002, 4003, 6000, 4200, 4201, 4202, 6300}) {
            if (m_maghand.find(w) == m_maghand.end()) {
                m_maghand[w] = s;
            }
        }
    }

    // Slide-Hand-Dock (Position + Rotations-Offset) pro Waffe.
    const auto load_sp = [&](const nlohmann::json& v, SlidePose& sp, bool with_pose2) {
        sp.dock_x = jnum(v, "dock_x", sp.dock_x);
        sp.dock_y = jnum(v, "dock_y", sp.dock_y);
        sp.dock_z = jnum(v, "dock_z", sp.dock_z);
        sp.rack_rx = jnum(v, "rack_rx", sp.rack_rx);
        sp.rack_ry = jnum(v, "rack_ry", sp.rack_ry);
        sp.rack_rz = jnum(v, "rack_rz", sp.rack_rz);
        sp.rest_z = jnum(v, "rest_z", sp.rest_z);
        sp.park_z = jnum(v, "park_z", sp.park_z);
        sp.back_z = jnum(v, "back_z", sp.back_z);

        if (with_pose2) {
            // [POSE2_OFFSETS] Fehlen sie in einer alten JSON, bleibt der
            // Blacktail-Startwert aus dem Struct-Default stehen.
            sp.sdock_x = jnum(v, "sdock_x", sp.sdock_x);
            sp.sdock_y = jnum(v, "sdock_y", sp.sdock_y);
            sp.sdock_z = jnum(v, "sdock_z", sp.sdock_z);
            sp.srack_rx = jnum(v, "srack_rx", sp.srack_rx);
            sp.srack_ry = jnum(v, "srack_ry", sp.srack_ry);
            sp.srack_rz = jnum(v, "srack_rz", sp.srack_rz);
        }
    };

    if (const auto it = data.find("slide_dock"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_object()) {
                load_sp(item.value(), slide_pose(wid), true);
            }
        }
    }

    if (const auto it = data.find("slide_dock2"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_object()) {
                load_sp(item.value(), slide_pose2(wid), false);
            }
        }
    }

    if (const auto it = data.find("mag_pose"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_string()) {
                m_mag_pose[wid] = item.value().get<std::string>();
            }
        }
    }

    if (const auto it = data.find("rack_pose_w"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_string()) {
                m_rack_pose[wid] = item.value().get<std::string>();
            }
        }
    }

    // [INSERT-DIST PRO GATTUNG] Pistolen erben den alten CFG.insert_distance,
    // andere Gattungen ihren gespeicherten Wert ODER eine Kopie davon.
    m_insert_dist["pistols"] = cfg.insert_distance;

    {
        const auto sd = data.find("insert_dist");

        for (const char* cat : {"pistols", "smgs", "shotguns", "magnum", "rifles"}) {
            const bool have = (sd != data.end() && sd->is_object() && jhas_num(*sd, cat));

            if (have) {
                m_insert_dist[cat] = jnum(*sd, cat, m_insert_dist[cat]);
            } else if (std::strcmp(cat, "pistols") != 0) {
                m_insert_dist[cat] = m_insert_dist["pistols"];
            }
        }
    }

    if (const auto it = data.find("insert_dist_wid"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_number()) {
                m_insert_dist_wid[wid] = item.value().get<float>();
            }
        }
    }

    if (const auto it = data.find("shotgun_ratio_wid"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (key_to_wid(item.key(), wid) && item.value().is_number()) {
                m_shotgun_ratio_wid[wid] = item.value().get<int32_t>();
            }
        }
    }

    // [SHELL-DOCK PERSIST] NUR die Striker laden -- andere Waffen bleiben auf
    // ihren Code-Werten, egal was in der JSON steht.
    if (const auto it = data.find("dock_port"); it != data.end() && it->is_object()) {
        const auto s = it->find("4102");

        if (s != it->end() && s->is_object() && m_dock_port.count(4102) > 0) {
            auto& d = m_dock_port[4102];
            d.x = jnum(*s, "x", d.x);
            d.y = jnum(*s, "y", d.y);
            d.z = jnum(*s, "z", d.z);
        }
    }

    // [SHELL_PART_PERSIST] gelernte Sub-Mesh-Part-Indizes pro Waffe
    if (const auto it = data.find("shell_parts"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;

            if (!key_to_wid(item.key(), wid) || !item.value().is_array() || item.value().empty()) {
                continue;
            }

            std::vector<int32_t> parts;

            for (const auto& p : item.value()) {
                if (p.is_number()) {
                    parts.push_back(p.get<int32_t>());
                }
            }

            if (!parts.empty()) {
                m_shell_parts[wid] = parts;
            }
        }
    }

    // [ROTARY_CYCLE] Drehschalter-Werte pro Waffe
    if (const auto it = data.find("rotary"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            auto& rr = rotary_cfg(wid);
            rr.rx = jnum(v, "rx", rr.rx);
            rr.ry = jnum(v, "ry", rr.ry);
            rr.rz = jnum(v, "rz", rr.rz);
            rr.grab_dist = jnum(v, "grab_dist", rr.grab_dist);
            rr.lerp = jnum(v, "lerp", rr.lerp);
            rr.pitch_range = jnum(v, "pitch_range", rr.pitch_range);
        }
    }

    // [POSE-STORE] 1) Pose-DATEN aus reload.json
    const auto read_poses = [&](const nlohmann::json& src) {
        for (const auto& item : src.items()) {
            const auto& v = item.value();

            if (!v.is_object()) {
                continue;
            }

            const auto b = v.find("bones");

            if (b == v.end() || !b->is_object()) {
                continue;
            }

            Pose p{};
            p.hand = jstr(v, "hand", "");

            for (const auto& bone : b->items()) {
                const auto& q = bone.value();

                if (q.is_array() && q.size() >= 4) {
                    p.bones[bone.key()] = glm::quat{q[0].get<float>(), q[1].get<float>(),
                                                    q[2].get<float>(), q[3].get<float>()};
                }
            }

            m_poses[item.key()] = p;
        }
    };

    if (const auto it = data.find("poses"); it != data.end() && it->is_object()) {
        read_poses(*it);
    }

    // 2) neue Captures aus gestures.json importieren (gestures = Capture-Tool).
    // Die gestures-Version gewinnt (frischester Capture).
    {
        const auto gj = re4vr::json_load("re4_vr/re4_vr_gestures_capture.json");

        if (gj.is_object()) {
            const auto gp = gj.find("poses");
            read_poses((gp != gj.end() && gp->is_object()) ? *gp : gj);
        }
    }
}

void RE4VRReload4::save_cfg() {
    nlohmann::json jt = nlohmann::json::object();

    for (const auto& e : m_joint_mag) {
        jt[std::to_string(e.first)] = {
            {"mag", e.second},
            {"slide", m_joint_slide.count(e.first) ? m_joint_slide.at(e.first) : std::string{}},
        };
    }

    nlohmann::json c = {
        {"enabled", cfg.enabled},
        {"skull_idx1", cfg.skull_idx1}, {"skull_idx2", cfg.skull_idx2},
        {"skull_idx3", cfg.skull_idx3},
        {"skull_open_deg", cfg.skull_open_deg}, {"skull_spin_dur", cfg.skull_spin_dur},
        {"pistols_enabled", cfg.pistols_enabled}, {"smgs_enabled", cfg.smgs_enabled},
        {"shotguns_enabled", cfg.shotguns_enabled}, {"magnum_enabled", cfg.magnum_enabled},
        {"rifles_enabled", cfg.rifles_enabled},
        {"gravity", cfg.gravity},
        {"mag_hold_pose", cfg.mag_hold_pose},
        {"insert_dur", cfg.insert_dur}, {"insert_distance", cfg.insert_distance},
        {"insert_punch", cfg.insert_punch}, {"insert_overshoot", cfg.insert_overshoot},
        {"insert_settle", cfg.insert_settle}, {"insert_haptic", cfg.insert_haptic},
        {"insert_snd_at", cfg.insert_snd_at},
        {"insert_manual", cfg.insert_manual}, {"insert_travel", cfg.insert_travel},
        {"insert_snap_at", cfg.insert_snap_at}, {"insert_back_out", cfg.insert_back_out},
        {"insert_redock", cfg.insert_redock},
        {"reload_ammo", cfg.reload_ammo},
        {"rack_enabled", cfg.rack_enabled}, {"rack_grab_dist", cfg.rack_grab_dist},
        {"rack_pull_dist", cfg.rack_pull_dist},
        {"pump_start_pull", cfg.pump_start_pull}, {"pump_push_frac", cfg.pump_push_frac},
        {"rack_haptic", cfg.rack_haptic}, {"rack_pose", cfg.rack_pose},
        {"rack_pose_side", cfg.rack_pose_side}, {"rack_pose_side_deg", cfg.rack_pose_side_deg},
        {"sound_enabled", cfg.sound_enabled}, {"mag_floor_delay", cfg.mag_floor_delay},
        {"shotgun_ratio", cfg.shotgun_ratio}, {"pump_haptic_delay", cfg.pump_haptic_delay},
    };

    nlohmann::json mh = nlohmann::json::object();

    for (const auto& e : m_maghand) {
        const auto& v = e.second;
        mh[std::to_string(e.first)] = {
            {"x", v.x}, {"y", v.y}, {"z", v.z},
            {"rx", v.rx}, {"ry", v.ry}, {"rz", v.rz},
            {"t_rx", v.t_rx}, {"t_ry", v.t_ry}, {"t_rz", v.t_rz},
        };
    }

    nlohmann::json se = nlohmann::json::object();

    for (const auto& e : m_shell_eject) {
        const auto& v = e.second;
        se[std::to_string(e.first)] = {
            {"sx", v.sx}, {"sy", v.sy}, {"sz", v.sz},
            {"srx", v.srx}, {"sry", v.sry}, {"srz", v.srz},
            {"vx", v.vx}, {"vy", v.vy}, {"vz", v.vz},
            {"grav", v.grav}, {"dur", v.dur}, {"spin", v.spin},
        };
    }

    nlohmann::json sdk_dock = nlohmann::json::object();

    for (const auto& e : m_slide_pose) {
        const auto& v = e.second;
        sdk_dock[std::to_string(e.first)] = {
            {"dock_x", v.dock_x}, {"dock_y", v.dock_y}, {"dock_z", v.dock_z},
            {"rack_rx", v.rack_rx}, {"rack_ry", v.rack_ry}, {"rack_rz", v.rack_rz},
            {"sdock_x", v.sdock_x}, {"sdock_y", v.sdock_y}, {"sdock_z", v.sdock_z},
            {"srack_rx", v.srack_rx}, {"srack_ry", v.srack_ry}, {"srack_rz", v.srack_rz},
            {"rest_z", v.rest_z}, {"park_z", v.park_z}, {"back_z", v.back_z},
        };
    }

    nlohmann::json sdk_dock2 = nlohmann::json::object();

    for (const auto& e : m_slide_pose2) {
        const auto& v = e.second;
        sdk_dock2[std::to_string(e.first)] = {
            {"dock_x", v.dock_x}, {"dock_y", v.dock_y}, {"dock_z", v.dock_z},
            {"rack_rx", v.rack_rx}, {"rack_ry", v.rack_ry}, {"rack_rz", v.rack_rz},
            {"rest_z", v.rest_z}, {"park_z", v.park_z}, {"back_z", v.back_z},
        };
    }

    nlohmann::json mpose = nlohmann::json::object();
    nlohmann::json rpose = nlohmann::json::object();

    for (const auto& e : m_mag_pose)  { mpose[std::to_string(e.first)] = e.second; }
    for (const auto& e : m_rack_pose) { rpose[std::to_string(e.first)] = e.second; }

    nlohmann::json idist = nlohmann::json::object();

    for (const auto& e : m_insert_dist) { idist[e.first] = e.second; }

    nlohmann::json idist_w = nlohmann::json::object();

    for (const auto& e : m_insert_dist_wid) { idist_w[std::to_string(e.first)] = e.second; }

    nlohmann::json sratio_w = nlohmann::json::object();

    for (const auto& e : m_shotgun_ratio_wid) { sratio_w[std::to_string(e.first)] = e.second; }

    nlohmann::json rotcfg = nlohmann::json::object();

    for (const auto& e : m_rotary_cfg) {
        const auto& v = e.second;
        rotcfg[std::to_string(e.first)] = {
            {"rx", v.rx}, {"ry", v.ry}, {"rz", v.rz},
            {"grab_dist", v.grab_dist}, {"lerp", v.lerp}, {"pitch_range", v.pitch_range},
        };
    }

    nlohmann::json sparts = nlohmann::json::object();

    for (const auto& e : m_shell_parts) { sparts[std::to_string(e.first)] = e.second; }

    nlohmann::json sclone = {
        {"part", m_ss_clone.part},
        {"x", m_ss_clone.x}, {"y", m_ss_clone.y}, {"z", m_ss_clone.z},
        {"rx", m_ss_clone.rx}, {"ry", m_ss_clone.ry}, {"rz", m_ss_clone.rz},
        {"scale", m_ss_clone.scale},
    };

    // Nur die Striker persistieren -- andere DOCK_PORT-Waffen bleiben rein
    // code-getrieben.
    nlohmann::json dpout = nlohmann::json::object();

    if (const auto it = m_dock_port.find(4102); it != m_dock_port.end()) {
        dpout["4102"] = {{"x", it->second.x}, {"y", it->second.y}, {"z", it->second.z}};
    }

    nlohmann::json poses = nlohmann::json::object();

    for (const auto& e : m_poses) {
        nlohmann::json bones = nlohmann::json::object();

        for (const auto& b : e.second.bones) {
            bones[b.first] = nlohmann::json::array({b.second.w, b.second.x,
                                                    b.second.y, b.second.z});
        }

        poses[e.first] = {{"hand", e.second.hand}, {"bones", bones}};
    }

    nlohmann::json d = {
        {"cfg", c}, {"joints", jt}, {"maghand", mh}, {"shell_eject", se},
        {"slide_dock", sdk_dock}, {"slide_dock2", sdk_dock2},
        {"mag_pose", mpose}, {"rack_pose_w", rpose},
        {"insert_dist", idist}, {"insert_dist_wid", idist_w},
        {"shotgun_ratio_wid", sratio_w}, {"poses", poses},
        {"rotary", rotcfg}, {"shell_parts", sparts},
        {"shell_clone", sclone}, {"dock_port", dpout},
    };

    re4vr::json_save(CFG_PATH, d);
}

// [RATIO GETEILT] In Lua fragt dieser Teil zuerst `__re4_shotgun_ratio_get` --
// also den Schalter des MAIN-Teils -- und faellt nur ohne ihn auf die eigenen
// Werte zurueck. Da der Main-Teil immer eine Zahl liefert, greifen die beiden
// Zeilen darunter praktisch nie; sie stehen trotzdem hier, damit der Fallback
// derselbe bleibt.
int32_t RE4VRReload4::shotgun_ratio_get(int32_t wid) {
    if (m_main != nullptr) {
        return m_main->shotgun_ratio_get(wid);
    }

    const auto it = m_shotgun_ratio_wid.find(wid);

    return (it != m_shotgun_ratio_wid.end()) ? it->second : cfg.shotgun_ratio;
}

// ============================================================================
// Player / Waffe (Lua Z.1820-1855)
// ============================================================================
// [FRAME-CACHE] Dieselben Objekte wurden pro Frame dutzendfach neu bei der
// Engine erfragt. Semantik unveraendert -- die alten Wege stehen als Fallback.

::REManagedObject* RE4VRReload4::get_ctx() {
    if (re4vr::fc::on()) {
        return re4vr::fc::ctx();
    }

    if (!re4vr::obj_ok(m_character_manager)) {
        m_character_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

std::optional<int32_t> RE4VRReload4::get_equip_wid() {
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

::REManagedObject* RE4VRReload4::get_body() {
    if (re4vr::fc::on()) {
        return re4vr::fc::body_go();
    }

    auto* ctx = get_ctx();

    return re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
}

::REManagedObject* RE4VRReload4::body_tf() {
    if (re4vr::fc::on()) {
        return re4vr::fc::body_tf();
    }

    auto* b = get_body();

    return re4vr::call_safe<::REManagedObject*>(b, "get_Transform");
}

// PlayerEquipment
::REManagedObject* RE4VRReload4::pe() {
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

std::optional<int32_t> RE4VRReload4::get_equip_type_main() {
    if (m_equip_type_main.has_value()) {
        return m_equip_type_main;
    }

    auto* td = sdk::find_type_definition("chainsaw.EquipType");
    auto* f = (td != nullptr) ? td->get_field("Main") : nullptr;

    if (f != nullptr) {
        m_equip_type_main = f->get_data<int32_t>(nullptr);
    }

    return m_equip_type_main;
}

// [GO_SUFFIX] Waffen-GOs heissen nicht immer exakt "wp####": bei Ada haengen
// Punisher MC / Rocket Launcher als "wp6112_AO" im Baum. Reihenfolge
// "" -> "_AO" -> "_MC" ist WICHTIG: wo es ein plain-GO gibt, ist "_AO" ein
// Schatten-Proxy und darf NICHT gewinnen.
::REManagedObject* RE4VRReload4::find_weapon(int32_t wid) {
    auto* bt = body_tf();

    if (bt == nullptr) {
        return nullptr;
    }

    char base[16]{};
    std::snprintf(base, sizeof(base), "wp%04d", wid);

    // Rekursive Kindersuche wie Luas search_tree (max. Tiefe 5, 128 Geschwister).
    const std::function<::REManagedObject*(::REManagedObject*, const std::string&, int)> search =
        [&](::REManagedObject* tf, const std::string& target, int depth) -> ::REManagedObject* {
        if (tf == nullptr || depth < 0) {
            return nullptr;
        }

        auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
        int n = 0;

        while (child != nullptr && n < 128) {
            ++n;
            auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

            if (go != nullptr) {
                auto* nm = re4vr::call_safe<::REManagedObject*>(go, "get_Name");
                const auto name = (nm != nullptr)
                    ? utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm))
                    : std::string{};

                if (name == target && re4vr::call_bool_not_false(go, "get_DrawSelf")) {
                    return child;
                }
            }

            if (auto* f = search(child, target, depth - 1); f != nullptr) {
                return f;
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }

        return nullptr;
    };

    for (const char* suffix : {"", "_AO", "_MC"}) {
        if (auto* tf = search(bt, std::string{base} + suffix, 5); tf != nullptr) {
            return tf;
        }
    }

    return nullptr;
}

// ============================================================================
// [POSE-STORE] Hand-Pose anwenden (eigene Engine, KEINE gestures-Abhaengigkeit)
// ============================================================================

const std::unordered_map<std::string, ::REManagedObject*>& RE4VRReload4::pose_build_map() {
    auto* tf = body_tf();

    if (tf == nullptr) {
        m_pmap.clear();

        return m_pmap;
    }

    if (tf == m_pmap_tf && !m_pmap.empty()) {
        return m_pmap;
    }

    m_pmap.clear();

    // [ARRAY-BINDING] Lua liest hier gemischt: get_Count als Call, die
    // Elemente aber ueber joints[i] -- das ist das Index-Binding, nicht
    // get_Item. Nativ ist beides array_size/array_element.
    auto* joints = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints");

    if (joints != nullptr) {
        const int32_t count = re4vr::array_size(joints);

        for (int32_t i = 0; i < count; ++i) {
            auto* j = re4vr::array_element(joints, i);

            if (j == nullptr) {
                continue;
            }

            auto* nm = re4vr::call_safe<::REManagedObject*>(j, "get_Name");

            if (nm == nullptr) {
                continue;
            }

            const auto name =
                utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm));

            if (!name.empty()) {
                m_pmap[name] = j;
            }
        }
    }

    m_pmap_tf = tf;

    return m_pmap;
}

namespace {

// Der gemeinsame Joint-Writer von pose_apply und apply_pose_bones.
void write_pose(const std::unordered_map<std::string, ::REManagedObject*>& map,
                const std::unordered_map<std::string, glm::quat>& bones, float blend) {
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
}

}   // namespace

bool RE4VRReload4::apply_pose(const std::string& name, float blend) {
    const auto it = m_poses.find(name);

    if (it == m_poses.end() || it->second.bones.empty()) {
        return false;
    }

    const auto& map = pose_build_map();

    if (map.empty()) {
        return false;
    }

    if (blend <= 0.0f) {
        return true;
    }

    write_pose(map, it->second.bones, blend);

    return true;
}

// [KNIFE_HAND] Externe Pose (Bone-Dict DIREKT, ohne Namen).
bool RE4VRReload4::apply_pose_bones(const std::unordered_map<std::string, glm::quat>& bones,
                                       float blend) {
    const auto& map = pose_build_map();

    if (map.empty()) {
        return false;
    }

    if (blend <= 0.0f) {
        return true;
    }

    write_pose(map, bones, blend);

    return true;
}

std::vector<std::string> RE4VRReload4::pose_names() const {
    std::vector<std::string> t;

    for (const auto& e : m_poses) {
        t.push_back(e.first);
    }

    std::sort(t.begin(), t.end());

    return t;
}

// ============================================================================
// [ACCESSOR] Die einzige persistente WeaponItem-Instanz (Lua Z.75-118)
// ============================================================================
// Live gemessen: getEquipWeaponItem, getEquippedWeapon und die Zeilen aus
// getInventoryItemList liefern bei JEDEM Aufruf eine frische KOPIE -- Schreiben
// darauf wirkt im selben Tick und ist danach WEG. Einzig
// pe:getEquipWeaponAccessor() ist stabil und haelt in <Item>k__BackingField
// das ECHTE chainsaw.WeaponItem.

::REManagedObject* RE4VRReload4::real_wi(std::optional<int32_t> want_wid) {
    auto* p = pe();

    if (p == nullptr) {
        return nullptr;
    }

    auto* acc = re4vr::call_safe<::REManagedObject*>(p, "getEquipWeaponAccessor");

    if (acc == nullptr) {
        return nullptr;
    }

    auto* real = re4vr::call_safe<::REManagedObject*>(acc, "get_Item");

    if (real == nullptr) {
        return nullptr;
    }

    // Der Ammo-Getter ist zugleich der Lebend-Test.
    if (!call_enum(real, "get_CurrentAmmoCount").has_value()) {
        return nullptr;
    }

    // Beim Waffenwechsel kann der Accessor kurz noch die ALTE Waffe halten.
    if (want_wid.has_value()) {
        const auto w = call_enum(real, "get_WeaponId");

        if (w.has_value() && *w != *want_wid) {
            return nullptr;
        }
    }

    return real;
}

::REManagedObject* RE4VRReload4::get_weapon_item() {
    // [ACCESSOR] zuerst die ECHTE, persistente Instanz -- alles darunter sind
    // KOPIEN, Schreiben verpufft.
    if (auto* rw = real_wi(); rw != nullptr) {
        return rw;
    }

    auto* p = pe();

    return re4vr::call_safe<::REManagedObject*>(p, "getEquipWeaponItem");
}

namespace {

// [LOG-FLUT] Luas guid_to_string: NICHT System.Guid.ToString aufrufen -- der
// Call schlaegt fehl und REFramework schreibt jede Warnung SYNCHRON auf die
// Platte (in einer Schleife ueber die Inventar-Zeilen sind das ~18 Zeilen pro
// Millisekunde, das hat schon einmal den Script-Thread abgewuergt). Verglichen
// wird ueber die ROHFELDER; das Ergebnis muss kein Guid-Format haben.
// ============================================================================
// [GUID ALS WERT 07.09.2026] System.Guid ist 16 Byte. call_safe<T> nimmt wegen
// sizeof(T)==8 NICHT den Out-Parameter-Zweig -- die Guid landet dann im als
// Rueckgabepuffer missverstandenen VMContext, und der Aufrufer haelt Muell.
// Genau deshalb liefen die beiden Guid-Vergleiche unten leer: `want` und `key`
// wurden aus einem ungueltigen Zeiger gebildet, die Zeile wurde nie ueber ihre
// Guid gefunden und es blieb der Fallback ueber die Waffen-ID -- der bei zwei
// gleichen Waffen im Koffer die falsche Zeile trifft.
// Derselbe Weg wie in RE4VRHolster ([FIX 2] dort): eigener 16-Byte-Puffer.
// ============================================================================
struct alignas(16) GuidVal {
    uint32_t a{}, b{}, c{}, d{};
};

template <typename... Args>
std::string guid_key_val(::REManagedObject* obj, std::string_view name, Args... args) {
    if (obj == nullptr) {
        return {};
    }

    auto* method = find_method(obj, name);

    if (method == nullptr) {
        return {};
    }

    auto context = sdk::get_thread_context();
    GuidVal g{};
    bool ok = false;

    try {
        method->call_safe<GuidVal*>(&g, context, obj, args...);
        ok = true;
    } catch (...) {
        ok = false;
    }

    ok = clear_pending(context, ok);

    if (!ok || (g.a == 0 && g.b == 0 && g.c == 0 && g.d == 0)) {
        return {};
    }

    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%u-%u-%u-%u", g.a, g.b, g.c, g.d);
    return buf;
}

}   // namespace

// Inventory-Row-Item (autoritativ, mit Reserve-Anbindung) -- KEIN Cache.
::REManagedObject* RE4VRReload4::get_inv_row_weapon_item() {
    auto* p = pe();

    if (p == nullptr) {
        return nullptr;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController");

    if (inv == nullptr) {
        return nullptr;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(inv, "getInventoryItemList");

    if (list == nullptr) {
        return nullptr;
    }

    int32_t cnt = 0;
    re4vr::try_call<int32_t>(list, "get_Count", cnt);

    const auto et = get_equip_type_main();
    const auto wid = get_equip_wid();
    ::REManagedObject* by_wid = nullptr;

    // Guid-Match bevorzugt, sonst WeaponId-Match. Die Guid wird ueber ihre
    // ROHFELDER verglichen -- System.Guid.ToString wirft eine Warnung pro
    // Aufruf, und REFramework schreibt jede davon SYNCHRON auf die Platte
    // (das hat schon einmal den Script-Thread abgewuergt).
    std::string want{};

    if (et.has_value()) {
        want = guid_key_val(inv, "getEquippedID", *et);
    }

    for (int32_t i = 0; i < cnt; ++i) {
        auto* row = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (row == nullptr) {
            continue;
        }

        if (!want.empty()) {
            const auto key = guid_key_val(row, "get_ID");

            if (!key.empty() && key == want) {
                return row;
            }
        }

        if (wid.has_value() && by_wid == nullptr) {
            const auto rw = call_enum(row, "get_WeaponId");

            if (rw.has_value() && *rw == *wid) {
                by_wid = row;
            }
        }
    }

    return by_wid;
}

::REManagedObject* RE4VRReload4::get_live_weapon_item() {
    const auto ewid = get_equip_wid();

    // 0) [ACCESSOR] DIE EINZIGE PERSISTENTE INSTANZ.
    if (auto* real = real_wi(ewid); real != nullptr) {
        return real;
    }

    // 1) Gecachtes Gun-Item -- nur wenn es EINDEUTIG die aktuell equippte Waffe
    // traegt. `get_IsValid` ist hier bewusst RAUS: auf einem toten Item ist
    // genau dieser Aufruf schon die Exception, die er verhindern soll.
    if (re4vr::obj_ok(m_live_wi)
        && call_enum(m_live_wi, "get_CurrentAmmoCount").has_value()) {
        const auto cwid = call_enum(m_live_wi, "get_WeaponId");

        if (ewid.has_value() && cwid.has_value() && *cwid == *ewid) {
            return m_live_wi;
        }
    }

    // 2) Das ECHTE equippte Item -- immer die aktuelle Waffe.
    auto* p = pe();
    auto* eqwi = re4vr::call_safe<::REManagedObject*>(p, "getEquipWeaponItem");

    if (eqwi != nullptr && call_enum(eqwi, "get_CurrentAmmoCount").has_value()) {
        return eqwi;
    }

    // 3) Fallback: Inventory-Row
    return get_inv_row_weapon_item();
}

::REManagedObject* RE4VRReload4::live_gun() {
    return get_live_weapon_item();
}

// Der HUD-/Runtime-Ladestand (kein Cache) = die WAHRHEIT.
std::optional<int32_t> RE4VRReload4::gun_ammo() {
    auto* p = pe();

    if (p == nullptr) {
        return std::nullopt;
    }

    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(p, "getCurrentGunAmmo", v)) {
        return std::nullopt;
    }

    return v;
}

// [GUN_AMMO_SYNC] Ein Schreiben auf das WeaponItem landet NUR im Item -- die
// Runtime-Gun (HUD, Feuern, Dry-Fire-Erkennung) wird davon nicht angefasst.
// updateGunAmmo ist genau die Sync-Funktion, die die Engine selbst benutzt.
void RE4VRReload4::sync_gun_ammo() {
    auto* p = pe();

    if (p == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(p, "updateGunAmmo");
}

::REManagedObject* RE4VRReload4::equip_gun() {
    auto* p = pe();

    return re4vr::call_safe<::REManagedObject*>(p, "getEquipWeaponItem");
}

// Laedt die Waffe Schuss fuer Schuss? Dann fuehrt die Engine den Ladestand
// selbst und verwirft unsere addAmmoCount-Schreibvorgaenge.
bool RE4VRReload4::is_loop_reload() {
    auto* p = pe();

    if (p == nullptr) {
        return false;
    }

    bool v = false;

    return re4vr::try_call<bool>(p, "isLoopReload", v) && v;
}

// [WER NULLT DAS MAGAZIN?] Jede Stelle, die auf 0 schreibt, meldet sich hier an
// und legt dabei den Rest als Merker ab.
void RE4VRReload4::carry_capture(::REManagedObject* wi, const char* where,
                                    std::optional<int32_t> wid) {
    (void)where;

    if (wi == nullptr) {
        return;
    }

    const auto cur = call_enum(wi, "get_CurrentAmmoCount");

    if (!cur.has_value() || *cur <= 0) {
        return;
    }

    // [SPERRE WAFFENBEWUSST] Die Sperre soll nur Doppelmeldungen DESSELBEN
    // Auswurfs verhindern. Ein fremder Merker ist verwaist und wird ersetzt.
    if (m_mag_carry.has_value() && *m_mag_carry > 0) {
        const auto hw = m_mag_carry_wid;

        if (!hw.has_value() || !wid.has_value() || *hw == *wid) {
            return;
        }
    }

    m_mag_carry = *cur;
    m_mag_carry_wid = wid;
}

// ============================================================================
// Der Ladeweg (Lua Z.398-1331)
// ============================================================================
namespace {

// Rohzugriff auf ein Int32-Feld. Deckt Luas `set_field` UND `write_dword`
// zugleich ab: gefunden wird ueber die TypeDB, notfalls ueber das gemessene
// Offset (in Lua steht dort die harte Zahl, weil `set_field` an diesen beiden
// Feldern mit "Attempted to set invalid field" scheiterte).
int32_t* field_i32(::REManagedObject* obj, const char* name, uint32_t fallback) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    uint32_t off = fallback;
    auto* td = utility::re_managed_object::get_type_definition(obj);

    if (td != nullptr) {
        if (auto* f = td->get_field(name); f != nullptr) {
            off = f->get_offset_from_base();
        }
    }

    if (off == 0) {
        return nullptr;
    }

    return reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(obj) + off);
}

// Katzenohren: unendliche Reserve? Und fuer WELCHE Sorte?
bool infinite_reserve_for(int32_t id) {
    auto& w2 = RE4VRWeapons2::get();

    if (w2 == nullptr || !w2->infinite_reserve()) {
        return false;
    }

    std::string s{};
    std::optional<double> n{};

    if (!w2->infinite_ammo_id(s, n)) {
        return false;
    }

    // [ZAHL STATT TEXT] Verglichen wird ueber die ZAHL: der tostring eines
    // Enum-Objekts ist eine ADRESSE, die sich bei jedem Aufruf aendert.
    if (n.has_value()) {
        return static_cast<int32_t>(*n) == id;
    }

    return !s.empty() && s == std::to_string(id);
}

}   // namespace

// [KATZENOHREN] Unendlich gilt NUR fuer die Sorte, die die equippte Waffe
// gerade benutzt -- pauschal fuer alle Sorten war ein Fehler.
int32_t RE4VRReload4::item_count_sum(::REManagedObject* inv, int32_t id) {
    if (inv == nullptr) {
        return 0;
    }

    if (infinite_reserve_for(id)) {
        return 999;
    }

    auto* items = re4vr::call_safe<::REManagedObject*>(inv, "getItems");

    if (items == nullptr) {
        return 0;
    }

    int32_t n = 0;
    re4vr::try_call<int32_t>(items, "get_Count", n);

    int32_t sum = 0;

    for (int32_t i = 0; i < n; ++i) {
        auto* it = re4vr::call_safe<::REManagedObject*>(items, "get_Item", i);

        if (it == nullptr) {
            continue;
        }

        const auto iid = call_enum(it, "get_ItemId");

        if (iid.has_value() && *iid == id) {
            int32_t c = 0;
            re4vr::try_call<int32_t>(it, "get_CurrentItemCount", c);
            sum += c;
        }
    }

    return sum;
}

// [CRASH-HARDEN v2] Reserve-Abzug OHNE den nativen reduce: der laeuft intern
// ueber die Item-Rows und deref't eine "stale/Spiegel"-Row mit null-Innenobjekt
// -> c0000005 -> FULL CRASH. Rufen wir ihn nie, kann er nie crashen.
bool RE4VRReload4::safe_reduce(::REManagedObject* inv, int32_t ammo_id, int32_t n) {
    if (inv == nullptr || n <= 0) {
        return false;
    }

    // [KATZENOHREN] Nichts abbuchen, aber "gebucht" melden, damit die Aufrufer
    // ihren normalen Weg weiterlaufen.
    if (infinite_reserve_for(ammo_id)) {
        return true;
    }

    const int32_t before_sum = item_count_sum(inv, ammo_id);

    // (1) ueber getInventoryItemList -> row:get_Item() -> reduceCount
    {
        auto* list = re4vr::call_safe<::REManagedObject*>(inv, "getInventoryItemList");
        int32_t lcnt = 0;

        if (list != nullptr) {
            re4vr::try_call<int32_t>(list, "get_Count", lcnt);
        }

        int32_t rem2 = n;

        for (int32_t i = 0; i < lcnt && rem2 > 0; ++i) {
            auto* row = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

            if (row == nullptr) {
                continue;
            }

            const auto rid = call_enum(row, "get_ItemId");

            if (!rid.has_value() || *rid != ammo_id) {
                continue;
            }

            auto* real = re4vr::call_safe<::REManagedObject*>(row, "get_Item");

            if (real == nullptr) {
                continue;
            }

            int32_t have = 0;
            re4vr::try_call<int32_t>(real, "get_CurrentItemCount", have);

            if (have <= 0) {
                continue;
            }

            const int32_t take = std::min(have, rem2);
            re4vr::call_safe<void*>(real, "reduceCount", take);

            int32_t now = have;
            re4vr::try_call<int32_t>(real, "get_CurrentItemCount", now);

            // [ABZUG_NACHMESSEN] reduceCount lief mehrfach durch, ohne etwas zu
            // bewegen -> an der Row nachmessen und notfalls den direkten Setter.
            if (now >= have) {
                re4vr::call_safe<void*>(real, "setItemCount", have - take);
                re4vr::try_call<int32_t>(real, "get_CurrentItemCount", now);
            }

            rem2 -= take;
        }

        if (item_count_sum(inv, ammo_id) < before_sum) {
            return true;
        }
    }

    // (G) DIE ECHTE LISTE -- ueber FELDER statt Getter. Jeder Getter liefert
    // bei jedem Aufruf eine andere Adresse, also Klone; deshalb verpufft dort
    // jeder Schreibvorgang. Die Originale liegen bei:
    //   CsInventoryController.<_CsInventory>k__BackingField (0xa0)
    //   CsInventory._InventoryItems (0x38)
    //   CsInventoryItem.<Item>k__BackingField (0x10)
    //   Item._CurrentItemCount (0x34)
    {
        ::REManagedObject* csinv = nullptr;

        if (auto* td = utility::re_managed_object::get_type_definition(inv); td != nullptr) {
            if (auto* f = td->get_field("<_CsInventory>k__BackingField"); f != nullptr) {
                csinv = f->get_data<::REManagedObject*>(inv);
            }
        }

        ::REManagedObject* lst = nullptr;

        if (re4vr::obj_ok(csinv)) {
            if (auto* td = utility::re_managed_object::get_type_definition(csinv); td != nullptr) {
                if (auto* f = td->get_field("_InventoryItems"); f != nullptr) {
                    lst = f->get_data<::REManagedObject*>(csinv);
                }
            }
        }

        int32_t lcnt2 = 0;

        if (lst != nullptr) {
            re4vr::try_call<int32_t>(lst, "get_Count", lcnt2);
        }

        int32_t rem3 = n;

        const auto row_item = [](::REManagedObject* row) -> ::REManagedObject* {
            if (!re4vr::obj_ok(row)) {
                return nullptr;
            }

            auto* td = utility::re_managed_object::get_type_definition(row);
            auto* f = (td != nullptr) ? td->get_field("<Item>k__BackingField") : nullptr;

            return (f != nullptr) ? f->get_data<::REManagedObject*>(row) : nullptr;
        };

        const auto item_id_field = [](::REManagedObject* it) -> std::optional<int32_t> {
            if (!re4vr::obj_ok(it)) {
                return std::nullopt;
            }

            auto* td = utility::re_managed_object::get_type_definition(it);
            auto* f = (td != nullptr) ? td->get_field("_ItemId") : nullptr;

            if (f == nullptr) {
                return std::nullopt;
            }

            return f->get_data<int32_t>(it);
        };

        for (int32_t i = 0; i < lcnt2 && rem3 > 0; ++i) {
            auto* row2 = re4vr::call_safe<::REManagedObject*>(lst, "get_Item", i);
            auto* it2 = row_item(row2);
            const auto iid2 = item_id_field(it2);

            if (it2 == nullptr || !iid2.has_value() || *iid2 != ammo_id) {
                continue;
            }

            auto* cnt_ptr = field_i32(it2, "_CurrentItemCount", 0x34);

            if (cnt_ptr == nullptr) {
                continue;
            }

            const int32_t have2 = *cnt_ptr;
            const int32_t take2 = std::min(have2, rem3);

            if (take2 > 0) {
                *cnt_ptr = have2 - take2;
                rem3 -= take2;
            }
        }

        // [LEERZEILE RAUS] Munition mit 0 gibt es im Inventar nicht -- ist eine
        // Zeile leergebucht, muss sie verschwinden. CsInventory.remove(Guid) ist
        // NICHT die reduce/reload-Familie und laeuft nur auf Zeilen, die schon
        // auf 0 stehen.
        if (m_inv_remove_empty && csinv != nullptr && lst != nullptr) {
            for (int32_t i = lcnt2 - 1; i >= 0; --i) {
                auto* row3 = re4vr::call_safe<::REManagedObject*>(lst, "get_Item", i);
                auto* it3 = row_item(row3);
                const auto iid3 = item_id_field(it3);

                if (it3 == nullptr || !iid3.has_value() || *iid3 != ammo_id) {
                    continue;
                }

                auto* cnt_ptr = field_i32(it3, "_CurrentItemCount", 0x34);

                if (cnt_ptr == nullptr || *cnt_ptr > 0) {
                    continue;
                }

                auto* td = utility::re_managed_object::get_type_definition(it3);
                auto* f = (td != nullptr) ? td->get_field("_ID") : nullptr;

                if (f == nullptr) {
                    continue;
                }

                // [GUID ALS WERT 07.09.2026 -- gemessen: 0er-Zeilen blieben im
                // Koffer stehen] `_ID` ist eine System.Guid, also ein
                // 16-Byte-VALUETYPE und KEIN Objektzeiger. Mit
                // get_data<REManagedObject*> kam Muell heraus, remove() traf
                // damit nichts -- die leergebuchte Zeile blieb im Inventar
                // sichtbar. In Lua konnte das nicht passieren: dort liefert
                // `it3:get_field("_ID")` den Wert, und REFramework reicht ihn
                // beim Aufruf passend weiter.
                // Richtig ist der Zeiger AUF das Feld im Objekt (get_data_raw
                // mit false = managed object, s.
                // [[reference_re4_cpp_get_data_raw_container_flag]]) -- derselbe
                // Guid-Weg wie in RE4VRHolster ([FIX 2] dort).
                void* gid = nullptr;

                try {
                    gid = f->get_data_raw(it3, false);
                } catch (...) {
                    gid = nullptr;
                }

                if (gid != nullptr) {
                    std::array<void*, 1> ga{gid};
                    re4vr::call_cmd(csinv, "remove(System.Guid)", std::span<void*>(ga));
                }
            }
        }

        if (item_count_sum(inv, ammo_id) < before_sum) {
            return true;
        }
    }

    // Fallback: der bisherige Weg ueber die getItems-Kopien (wirkungslos, aber
    // harmlos -- er steht 1:1 so in der Lua).
    auto* items = re4vr::call_safe<::REManagedObject*>(inv, "getItems");

    if (items == nullptr) {
        return false;
    }

    int32_t cnt = 0;
    re4vr::try_call<int32_t>(items, "get_Count", cnt);

    int32_t rem = n;

    for (int32_t i = 0; i < cnt && rem > 0; ++i) {
        auto* it = re4vr::call_safe<::REManagedObject*>(items, "get_Item", i);

        if (it == nullptr) {
            continue;
        }

        const auto iid = call_enum(it, "get_ItemId");

        if (!iid.has_value() || *iid != ammo_id) {
            continue;
        }

        int32_t have = 0;
        re4vr::try_call<int32_t>(it, "get_CurrentItemCount", have);

        if (have <= 0) {
            continue;
        }

        const int32_t take = std::min(have, rem);
        re4vr::call_safe<void*>(it, "reduceCount", take);

        int32_t now = have;
        re4vr::try_call<int32_t>(it, "get_CurrentItemCount", now);

        if (now >= have) {
            re4vr::call_safe<void*>(it, "setItemCount", have - take);
            re4vr::try_call<int32_t>(it, "get_CurrentItemCount", now);
        }

        const int32_t drop = std::max(0, have - now);

        if (drop <= 0) {
            break;   // weitere Rows bringen nichts, wenn der Mutator nicht greift
        }

        rem -= drop;
    }

    return rem < n;
}

// [LADEN + BUCHEN] Stand vorher merken, laden lassen, und wenn die Waffe voller
// wurde, ohne dass die Reserve gefallen ist, genau diese Menge buchen.
bool RE4VRReload4::load_and_book(::REManagedObject* inv, int32_t et, int32_t n, bool refill) {
    auto* w = (inv != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(inv, "getEquippedWeapon", et) : nullptr;
    const auto aid = (w != nullptr) ? call_enum(w, "get_CurrentAmmo") : std::nullopt;

    // [UNENDLICH = REFILL] Katzenohren heisst: es wird einfach geladen. Im
    // Inventar liegt dann aber KEINE Munition -- der Reserve-Check in
    // safe_inv_reload lehnt sonst JEDE Menge ab und es kommt nicht einmal ein
    // Einlegesound. `refill = true` ist der dafuer vorgesehene Weg.
    if (!refill && aid.has_value() && infinite_reserve_for(*aid)) {
        refill = true;
    }

    const auto rsv = [&]() -> int32_t {
        return (inv != nullptr && aid.has_value()) ? item_count_sum(inv, *aid) : 0;
    };

    const int32_t r0 = rsv();
    const auto g0 = gun_ammo();

    const bool ok = safe_inv_reload(inv, et, n, refill);

    const auto g1 = gun_ammo();

    if (g0.has_value() && g1.has_value() && *g1 > *g0 && rsv() >= r0 && aid.has_value()) {
        safe_reduce(inv, *aid, *g1 - *g0);
    }

    return ok;
}

// [NO_NATIVE_RELOAD] Der EIGENTLICHE FIX: die AV liegt IMMER im nativen
// CsInventoryController.reload. Kein Vorab-Check kann sie verhindern, weil
// enableReloadItem/reload DIESELBE null-Row deref'en. Der einzige Weg, der
// garantiert nicht crashen kann, ist: DEN CALL NICHT MACHEN.
bool RE4VRReload4::safe_inv_reload(::REManagedObject* inv, int32_t et, int32_t n, bool refill) {
    if (inv == nullptr || n <= 0) {
        return false;
    }

    // (1) Gun-Item da? (Uebergangsframe: getEquippedWeapon == null -> der native
    // reload deref't null.)
    auto* w = re4vr::call_safe<::REManagedObject*>(inv, "getEquippedWeapon", et);

    if (w == nullptr) {
        return false;
    }

    // (2) [AMMO-GUARD] Reserve des zur Waffe passenden Ammo-Items lesen und n
    // NICHT ueberschreiten. refill == true = "gibt Munition" -> kein Check.
    if (!refill) {
        const auto ammo_id = call_enum(w, "get_CurrentAmmo");

        if (!ammo_id.has_value()) {
            return false;
        }

        int32_t cnt = 0;

        if (!re4vr::try_call<int32_t>(inv, "getItemCountSum", cnt, *ammo_id) || cnt < n) {
            return false;
        }

        // (Luas `drain`-Merker wird hier gesetzt, aber nirgends mehr gelesen --
        // der Split-Weg ist mit dem nativen Reload ausgebaut worden.)
    }

    auto* wi_live = live_gun();
    const auto before = gun_ammo();

    // [ACCESSOR] ZUERST die echte, persistente Instanz -- __re4_live_wi ist der
    // alte Hook-Cache (kann nach Save-Load eine Leiche sein), wi_live/w sind
    // KOPIEN (Schreiben verpufft).
    ::REManagedObject* add_wi = real_wi();

    if (add_wi == nullptr) {
        add_wi = re4vr::obj_ok(m_live_wi) ? m_live_wi : wi_live;
    }

    const auto item_ammo = [&]() -> std::optional<int32_t> {
        return (add_wi != nullptr) ? call_enum(add_wi, "get_CurrentAmmoCount") : std::nullopt;
    };

    if (wi_live == nullptr || !before.has_value()) {
        ++m_reload_direct_fail;

        return false;
    }

    // [CAP-KANDIDATEN] Eine EINZELNE Instanz zu fragen ist zu wenig: der Reihe
    // nach alle vier, die erste mit cap > 0 gewinnt.
    int32_t cap = 0;

    {
        const std::array<::REManagedObject*, 4> cands{add_wi, wi_live, equip_gun(), w};

        for (size_t i = 0; i < cands.size(); ++i) {
            const auto v = (cands[i] != nullptr)
                ? call_enum(cands[i], "get_CurrentAmmoMax") : std::nullopt;

            if (cap <= 0 && v.has_value() && *v > 0) {
                cap = *v;
            }

            // [ADD_WI_TOT] Diese Messung MERKEN: liefert add_wi hier nichts, ist
            // die Instanz tot -- und genau darauf schreibt der Ladeweg unten.
            // Ein Call auf eine tote Instanz ist ein virtueller Call ins Leere.
            if (i == 0) {
                m_addwi_alive = v.has_value();
            }
        }
    }

    const int32_t it_b = item_ammo().value_or(*before);
    const int32_t target = (cap > 0) ? std::min(cap, it_b + n) : (it_b + n);
    const int32_t got = target - it_b;

    if (got <= 0) {
        return false;   // schon voll
    }

    // [PHANTOM-RESERVE / FREMDE MUNITION] Reserve-Kandidaten in dieser
    // Reihenfolge: die Waffe im SLOT und die equippte Instanz gehoeren per
    // Definition zur Waffe in der Hand, `add_wi` (der Hook-Cache) erst zuletzt.
    // Jeder Kandidat wird zusaetzlich per WeaponId gegen die gefuehrte Waffe
    // geprueft -- sonst laedt die Killer7 Gewehrpatronen.
    int32_t rsv = 0;
    std::optional<int32_t> rsv_aid{};
    std::optional<int32_t> rsv_wid{};
    const auto soll = m_reload_ui_wid;

    {
        const std::array<::REManagedObject*, 4> cands{w, equip_gun(), wi_live, add_wi};

        for (auto* c : cands) {
            if (rsv > 0 || c == nullptr) {
                continue;
            }

            const auto cwid = call_enum(c, "get_WeaponId");

            if (soll.has_value() && cwid.has_value() && *cwid != *soll) {
                continue;
            }

            // [USABLE-LISTE] get_CurrentAmmo ist ein beschreibbares Feld -- steht
            // dort eine fremde Sorte, meldet die Waffe 0 Reserve, obwohl der
            // Koffer voll ist. Die Waffe weiss aber selbst, welche Sorten sie
            // nimmt: get_UsableAmmoList().
            std::vector<int32_t> cand_ids;

            if (const auto a = call_enum(c, "get_CurrentAmmo"); a.has_value()) {
                cand_ids.push_back(*a);
            }

            // [ARRAY-BINDING] get_size / get_element, NICHT get_Count/get_Item.
            if (auto* arr = re4vr::call_safe<::REManagedObject*>(c, "get_UsableAmmoList");
                arr != nullptr) {
                const int32_t asz = re4vr::array_size(arr);

                for (int32_t k = 0; k < asz; ++k) {
                    // Die Eintraege sind eingepackte Enum-Werte -- als Objekt
                    // gelesen ist ihr tostring die Adresse. Nativ kommt die Zahl.
                    if (auto* e = re4vr::array_element(arr, k); e != nullptr) {
                        if (auto* td = utility::re_managed_object::get_type_definition(e);
                            td != nullptr) {
                            if (auto* f = td->get_field("value__"); f != nullptr) {
                                cand_ids.push_back(f->get_data<int32_t>(e));
                            }
                        }
                    }
                }
            }

            for (int32_t aid : cand_ids) {
                const int32_t s = item_count_sum(inv, aid);

                if (s > 0 && rsv <= 0) {
                    rsv = s;
                    rsv_aid = aid;
                    rsv_wid = cwid;
                    // Die Sorte stammt aus der Kandidatenliste DIESER Waffe --
                    // also belegt zulaessig und damit auch schreibbar.
                    m_reserve_aid_ok = true;
                }
            }
        }
    }

    if (rsv <= 0) {
        rsv_wid.reset();
        m_reserve_aid_ok = false;
    }

    m_reserve_aid = rsv_aid;         // exakt DIESE ID zieht der Abzug spaeter ab
    m_reserve_aid_wid = rsv_wid;

    // [HUD-RESERVE] Zeigt die Waffe "0 Reserve", obwohl der Koffer voll ist,
    // steht in ihrem Feld _CurrentAmmo die falsche Sorte -- das Spiel zeichnet
    // die Anzeige aus genau diesem Feld.
    if (rsv_aid.has_value() && m_reserve_aid_ok) {
        const auto wi_now = call_enum(w, "get_CurrentAmmo");

        if (!wi_now.has_value() || *wi_now != *rsv_aid) {
            re4vr::call_safe<void*>(w, "setAmmoId", *rsv_aid);
        }
    }

    // (Luas `is_full` wird hier berechnet -- der zugehoerige Zweig (Weg A,
    // execReload) ist seit dem Crashdump vom 06.08. LEER.)

    // (P) Zeilen-Check. Reine MESSUNG: die leere Zeile entsteht WAEHREND des
    // nativen Calls, eine Pruefung davor kann sie prinzipiell nicht sehen.
    // Deshalb haengt hier keine Ladeentscheidung mehr dran.
    {
        auto* list = re4vr::call_safe<::REManagedObject*>(inv, "getInventoryItemList");
        int32_t rows_n = 0;

        if (list != nullptr) {
            re4vr::try_call<int32_t>(list, "get_Count", rows_n);

            for (int32_t i = 0; i < rows_n; ++i) {
                auto* row = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

                if (row != nullptr) {
                    (void)re4vr::call_safe<::REManagedObject*>(row, "get_Item");
                }
            }
        }
    }

    if (add_wi == nullptr) {
        ++m_reload_direct_fail;

        return false;
    }

    const int32_t hud_b = gun_ammo().value_or(*before);

    // [AUSWEICHEN STATT AUFGEBEN] `add_wi` tot, aber die Slot-Waffe lebt ->
    // Leiche aus dem Cache werfen und auf `w` ausweichen.
    if (!m_addwi_alive && w != nullptr && call_enum(w, "get_CurrentAmmoMax").has_value()) {
        m_live_wi = nullptr;

        if (auto* rw = real_wi(); rw != nullptr) {
            add_wi = rw;
        } else {
            add_wi = w;
        }

        m_addwi_alive = true;
    }

    if (!m_addwi_alive) {
        // [SELBSTHEILUNG] Nicht nur abbrechen: die Leiche AUS DEM CACHE werfen.
        // Die Leiche selbst wird NICHT angefasst (ein Getter darauf ist eine AV).
        m_live_wi = nullptr;
        m_live_wi_t = -1.0;
        ++m_reload_direct_fail;

        return false;
    }

    auto* gun_obj = re4vr::call_safe<::REManagedObject*>(pe(), "getEquipWeapon");

    // [LEERES MAG] Bei leerem Magazin hat die Waffe moeglicherweise gar keine
    // gueltige Munitionssorte gesetzt -- dann weiss addAmmoCount nicht, WAS es
    // addieren soll. [KEIN FREMDSCHREIBEN] Geschrieben wird nur, wenn die ID
    // nachweislich von einer Instanz DIESER Waffe stammt.
    if (hud_b <= 0 && m_reserve_aid.has_value()) {
        if (soll.has_value() && m_reserve_aid_wid.has_value() && *m_reserve_aid_wid == *soll) {
            re4vr::call_safe<void*>(add_wi, "setAmmoId", *m_reserve_aid);
        }
    }

    // [RELOAD-STATE ZUERST] DER fehlende Schritt: die Engine bucht Munition nur
    // im Reload-State. State None + laden -> nichts passiert.
    if (gun_obj != nullptr) {
        re4vr::call_safe<void*>(gun_obj, "onReloadStart");
    }

    // [MAG-CARRY] Der beim Auswurf gemerkte Magazinrest kommt hier wieder drauf
    // -- er kostet KEINE Reserve (die wurde damals schon bezahlt), erhoeht also
    // nur die Menge fuer die Waffe, nicht `n`.
    int32_t carry_add = 0;

    if (m_mag_carry.has_value() && *m_mag_carry > 0) {
        const auto nowid = call_enum(add_wi, "get_WeaponId");

        if (!m_mag_carry_wid.has_value() || !nowid.has_value()
            || *m_mag_carry_wid == *nowid) {
            int32_t room = cap - got;

            if (room < 0) {
                room = 0;
            }

            carry_add = std::min(*m_mag_carry, room);
        }
    }
    // [NICHT HIER LOESCHEN] Der Merker wird erst unten bei `loaded > 0`
    // verbraucht -- wirkt keiner der vier Schreibwege, waere er sonst
    // ersatzlos weg (Magazin drin, Waffe leer, Dry-Fire).

    // Vier Schreibversuche, jeder mit Sync danach, jeder nur wenn die HUD noch
    // nicht gestiegen ist -> nie doppelt geladen.
    re4vr::call_safe<void*>(add_wi, "addAmmoCount", got + carry_add, true);
    sync_gun_ammo();
    int32_t hud_a = gun_ammo().value_or(hud_b);

    if (hud_a <= hud_b) {
        re4vr::call_safe<void*>(add_wi, "addAmmoCount", got + carry_add, false);
        sync_gun_ammo();
        hud_a = gun_ammo().value_or(hud_b);
    }

    // [FORCE_SET] addAmmoCount ist additiv und haengt am Reload-Zustand;
    // forceSetAmmoCount setzt den Ladestand ABSOLUT.
    if (hud_a <= hud_b) {
        const int32_t sum = hud_b + got + carry_add;
        const int32_t want_abs = (cap > 0) ? std::min(sum, cap) : sum;

        re4vr::call_safe<void*>(add_wi, "forceSetAmmoCount", want_abs);
        sync_gun_ammo();
        hud_a = gun_ammo().value_or(hud_b);

        if (hud_a <= hud_b) {
            re4vr::call_safe<void*>(add_wi, "setAmmoCount", want_abs);
            sync_gun_ammo();
            hud_a = gun_ammo().value_or(hud_b);
        }
    }

    const int32_t loaded = std::max(0, hud_a - hud_b);

    if (loaded > 0) {
        // [NICHT DOPPELT ZAHLEN] `loaded` enthaelt auch die gutgeschriebenen
        // Patronen aus dem gedroppten Magazin -- die sind laengst bezahlt.
        const int32_t pay = std::max(0, loaded - carry_add);

        if (m_reserve_aid.has_value() && pay > 0) {
            safe_reduce(inv, *m_reserve_aid, pay);
        }

        m_mag_carry.reset();
        m_mag_carry_wid.reset();
        ++m_reload_direct_ok;

        return true;
    }

    ++m_reload_direct_fail;

    return false;
}

// ============================================================================
// Waffen-Cache (Lua Z.2405-2472)
// ============================================================================

void RE4VRReload4::refresh_weapon() {
    const auto wid = get_equip_wid();

    if (!wid.has_value() || *wid == 0) {
        m_wep = Wep{};

        return;
    }

    if (m_wep.wid.has_value() && *m_wep.wid == *wid && m_wep.mag_joint != nullptr
        && m_wep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_wep.tf, "get_Position", p)) {
            return;
        }
    }

    // [SAVE_LOAD] Gleiche WeaponId, aber Re-Resolve noetig = neue Instanz durch
    // Save-Load/Respawn. Der Waffenwechsel-Reset greift hier NICHT.
    const bool same_wid = (m_wep.wid.has_value() && *m_wep.wid == *wid);

    m_wep = Wep{};

    const auto mit = m_joint_mag.find(*wid);
    const std::string mag_name = (mit != m_joint_mag.end()) ? mit->second : std::string{};
    const auto sit = m_joint_slide.find(*wid);
    const std::string slide_name = (sit != m_joint_slide.end()) ? sit->second : std::string{};

    // (Luas TOP_LOADER-Zweig ist unerreichbar: die Tabelle ist leer, seit der
    // Red9 nach reload2 ausgezogen ist.)
    if (mag_name.empty()) {
        return;
    }

    auto* tf = find_weapon(*wid);

    if (tf == nullptr) {
        return;
    }

    auto* j = joint_by_name(tf, mag_name);

    if (j == nullptr) {
        return;
    }

    m_wep.wid = *wid;
    m_wep.tf = tf;
    m_wep.mag_joint = j;
    m_wep.slide_joint = slide_name.empty() ? nullptr : joint_by_name(tf, slide_name);

    // [PUMP_GRIP] Den NAMEN nach aussen geben, damit motion den Vordergriff an
    // EXAKT dasselbe Joint haengt, das hier gepumpt wird. Nur der Name, kein
    // Objekt -- das ueberlebt keinen Savegame-Load.
    if (!slide_name.empty()) {
        re4vr::lua_set_string("__re4_rack_joint_name", slide_name);
    } else {
        re4vr::lua_set_nil("__re4_rack_joint_name");
    }

    re4vr::lua_set_number("__re4_rack_joint_wid", *wid);

    // [ROTARY/BREAK] Ruhe-Rotation merken -- BIND-Pose (get_BaseLocalRotation),
    // NICHT live: nach einem Script-Reset ist die Klappe evtl. noch offen, und
    // live wuerde diese OFFENE Lage als "Ruhe" merken.
    if ((rotary_cycle(*wid) || break_action(*wid)) && m_wep.slide_joint != nullptr) {
        glm::quat q{};

        if (get_quat(m_wep.slide_joint, "get_BaseLocalRotation", q)
            || get_quat(m_wep.slide_joint, "get_LocalRotation", q)) {
            m_wep.cycle_rest_rot = q;
        }

        // [BREAK_ACTION] Klappe beim Erwerb einmalig auf die Zu-Ruhe setzen ->
        // nach einem Reset schnappt sie sofort zu, passend zum frischen State.
        if (break_action(*wid) && m_wep.cycle_rest_rot.has_value()) {
            set_quat(m_wep.slide_joint, "set_LocalRotation", *m_wep.cycle_rest_rot);
        }
    }

    // [EMPTY-RELOAD SLIDE] separater Lade-Slide-Joint (_08)
    if (const char* erj = empty_reload_joint(*wid); erj != nullptr) {
        m_wep.slide_joint2 = joint_by_name(tf, erj);
    }

    if (same_wid) {
        m_weapon_reacquired = true;
    }
}

// [EMPTY-RELOAD SLIDE] aktiver Rack-Joint + Pose: im Empty-Reload-Modus der
// Lade-Slide (_08) mit eigener Travel-Pose, sonst der normale Slide/Pump (_01).
::REManagedObject* RE4VRReload4::rack_joint() {
    if (m_rack.empty_reload && m_wep.slide_joint2 != nullptr) {
        return m_wep.slide_joint2;
    }

    return m_wep.slide_joint;
}

RE4VRReload4::SlidePose& RE4VRReload4::rack_slide_pose() {
    const int32_t wid = m_wep.wid.value_or(0);

    return m_rack.empty_reload ? slide_pose2(wid) : slide_pose(wid);
}

// ============================================================================
// [SND] Waffen-Sounds (Lua Z.2542-2555)
// ============================================================================
// 1:1 wie der Sound-Player: simple trigger(uint32) ist hoerbar (die Full-Sig
// lief lautlos -> verworfen).
void RE4VRReload4::play_weapon_sound(std::optional<uint32_t> id) {
    if (!cfg.sound_enabled || !id.has_value() || *id == 0) {
        return;
    }

    if (m_wep.tf == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_wep.tf, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    static auto* td_snd = sdk::find_type_definition("soundlib.SoundContainer");

    auto* scn = re4vr::get_component(go, td_snd);

    if (scn == nullptr) {
        return;
    }

    m_sg_our_sound = true;
    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", *id);
    m_sg_our_sound = false;
}

// ============================================================================
// Ist die equippte Waffe von uns verwaltet? (Lua Z.2595)
// ============================================================================
std::optional<int32_t> RE4VRReload4::handled() {
    // KEIN Master mehr -> jede Gattung haengt allein an ihrem eigenen Toggle.
    const auto wid = get_equip_wid();

    if (!wid.has_value()) {
        return std::nullopt;
    }

    const char* cat = category_of(*wid);

    if (!category_enabled(cat)) {
        return std::nullopt;
    }

    const auto it = m_joint_mag.find(*wid);

    if (it == m_joint_mag.end() || it->second.empty()) {
        return std::nullopt;
    }

    return wid;
}

// "Mag drin?" wird ABGELEITET (kein getracktes Flag -> kann nicht desyncen).
bool RE4VRReload4::mag_is_present() const {
    return !(m_drop.active || m_mag_hand.active || m_mag_insert.active);
}

// ============================================================================
// Ammo setzen / leeren (Lua Z.2365-2403)
// ============================================================================
// WICHTIG: forceSetAmmoCount/setAmmoCount sind nur der SAVE-Pfad (greifen zur
// Laufzeit NICHT). Der LAUFZEIT-Pfad ist die rohe Speicherschreibung auf
// _CurrentAmmoCount (0x44) -- das Feld, das get_CurrentAmmoCount + HUD lesen.
bool RE4VRReload4::drain_to_zero(::REManagedObject* wi) {
    if (wi == nullptr) {
        return false;
    }

    const auto cur = [&]() -> int32_t {
        return call_enum(wi, "get_CurrentAmmoCount").value_or(0);
    };

    if (cur() <= 0) {
        return true;
    }

    carry_capture(wi, "re4_vr_reload4_dlc.lua:1122", std::nullopt);   // [MAG-REST]

    if (auto* p = field_i32(wi, "_CurrentAmmoCount", 0x44); p != nullptr) {
        *p = 0;
    }

    if (cur() > 0) { re4vr::call_safe<void*>(wi, "reduceAmmoCount", cur()); }
    if (cur() > 0) { re4vr::call_safe<void*>(wi, "addAmmoCount", -cur(), false); }
    if (cur() > 0) { re4vr::call_safe<void*>(wi, "forceSetAmmoCount", 0); }
    if (cur() > 0) { re4vr::call_safe<void*>(wi, "setAmmoCount", 0); }

    return true;
}

bool RE4VRReload4::set_gun_loaded(int32_t n) {
    auto* wi = get_live_weapon_item();
    auto* row = get_inv_row_weapon_item();

    if (n == 0) {
        // [KEIN VERWERFEN] Was noch im Magazin steckt, ist NICHT weg -- der
        // Auswurf setzt die Waffe sichtbar auf 0 (VR-Fiktion), der Rest wird
        // gemerkt und beim Einsetzen wieder draufgerechnet.
        drain_to_zero(wi);

        if (row != nullptr && row != wi) {
            drain_to_zero(row);
        }

        return true;
    }

    // andere Werte: Save-Pfad (selten genutzt)
    if (wi == nullptr) {
        return false;
    }

    re4vr::call_safe<void*>(wi, "forceSetAmmoCount", n);
    re4vr::call_safe<void*>(wi, "setAmmoCount", n);

    return true;
}

// ============================================================================
// Mag-Drop (Lua Z.2624-2735)
// ============================================================================

bool RE4VRReload4::start_mag_drop() {
    if (m_wep.mag_joint == nullptr) {
        return false;
    }

    m_mag_hand.active = false;      // neuer Drop -> Mag nicht mehr in der Hand
    m_mag_insert.active = false;    // evtl. laufenden Insert abbrechen
    m_mag_insert.settle = false;    // [INSERT-PUNCH] Nachfedern mit abbrechen
    m_mag_tune.active = false;      // Einstell-Modus weicht dem echten Reload

    // [MAG_RETAIN] Geladenen Stand MERKEN, bevor die UI auf 0 geht.
    if (cfg.reload_ammo) {
        auto cur = gun_ammo();

        if (!cur.has_value()) {
            auto* wi0 = get_live_weapon_item();
            cur = (wi0 != nullptr) ? call_enum(wi0, "get_CurrentAmmoCount") : std::nullopt;
        }

        m_mag_retained = cur.value_or(0);

        // [EINE QUELLE] Dieselbe Zahl zusaetzlich als Merker fuer den Ladeweg
        // spiegeln -- kein zweiter Merker, nur sichtbar gemacht.
        m_mag_carry = m_mag_retained;
        m_mag_carry_wid = get_equip_wid();

        set_gun_loaded(0);   // UI auf 0

        // [ACCESSOR-FOLGE] Die 0 ist UNSER Werk. Die Kammer-Pruefung beim
        // Einsetzen darf diesen selbstgesetzten 0-Stand NICHT als "Kammer leer"
        // werten, sonst verlangt jede Magazinwaffe nach JEDEM Reload ein Rack.
        m_rack._zeroed_by_us = true;
    }

    // bevorzugt das Advanced-Modul (Slide-aus-der-Kammer + Fall)
    if (m_adv != nullptr) {
        m_adv->set_current_mag_joint(m_wep.mag_joint);

        // [EINLEIT-PUNKT ALS DROP-ZIEL] Das Mag gleitet zum SELBEN Punkt raus,
        // an dem es beim Reinsliden ansetzt -- rueckwaerts die Kammerachse
        // entlang. Fehlt der Punkt -> alter Exit-Vektor.
        std::optional<glm::vec3> drop_target{};
        const int32_t wid = m_wep.wid.value_or(0);

        if (parent_space_dock(wid) && m_wep.mag_joint != nullptr && m_wep.tf != nullptr) {
            const auto W = m_adv->dock_world(m_wep.tf, wid);

            if (W.has_value()) {
                glm::vec3 jw{}, jlp{};
                glm::quat jr{}, jlr{};

                if (get_vec3(m_wep.mag_joint, "get_Position", jw)
                    && get_quat(m_wep.mag_joint, "get_Rotation", jr)
                    && get_vec3(m_wep.mag_joint, "get_LocalPosition", jlp)
                    && get_quat(m_wep.mag_joint, "get_LocalRotation", jlr)) {
                    const glm::vec3 dd = jlr * (glm::conjugate(jr) * (*W - jw));
                    drop_target = jlp + dd;
                }
            }
        } else if (m_wep.tf != nullptr) {
            drop_target = m_adv->dock_local(m_wep.tf, wid);
        }

        // Slide-Out = Insert-Speed
        if (m_adv->begin_drop(m_wep.mag_joint, wid, cfg.insert_dur, drop_target)) {
            m_drop.active = true;
            m_drop.use_module = true;
            m_drop.joint = m_wep.mag_joint;

            return true;
        }
    }

    // Fallback: einfacher Gravity-Drop im Welt-Raum
    glm::vec3 p{};

    if (!get_vec3(m_wep.mag_joint, "get_Position", p)) {
        return false;
    }

    m_drop.joint = m_wep.mag_joint;
    m_drop.use_module = false;
    m_drop.sx = p.x;
    m_drop.sy = p.y;
    m_drop.sz = p.z;

    // [ENTKOPPEL_ROT] Weltrotation beim Drop-Start einfrieren.
    glm::quat r{};
    m_drop.srot = get_quat(m_wep.mag_joint, "get_Rotation", r)
        ? std::optional<glm::quat>{r} : std::nullopt;

    m_drop.t0 = clock_now();
    m_drop.active = true;

    return true;
}

void RE4VRReload4::stop_mag_drop() {
    if (m_drop.use_module && m_adv != nullptr) {
        m_adv->cancel();
    }

    m_drop.active = false;
    m_drop.use_module = false;
    m_drop.joint = nullptr;
}

void RE4VRReload4::update_mag_drop() {
    if (!m_drop.active) {
        return;
    }

    if (m_drop.use_module) {
        if (m_adv != nullptr) {
            m_adv->tick();
        }

        return;
    }

    if (m_drop.joint == nullptr) {
        return;
    }

    const float t = static_cast<float>(clock_now() - m_drop.t0);

    // [DROP-AUTOCLEAR] Der freie Fall ist rein kosmetisch. Nach DROP_FALL_DUR
    // die Drop-Phase BEENDEN, sonst haengt drop.active -> Feuer FUER IMMER
    // gesperrt (Soft-Lock). Darf nie passieren.
    if (t > DROP_FALL_DUR) {
        // Shotgun: die losgelassene Shell ist nur Deko -> _04 zurueck in die
        // Ruhepose. Pistole: Mag ist echt ausgeworfen -> liegen lassen.
        if (is_shotgun(m_wep.wid.value_or(0)) && m_wep.mag_joint != nullptr
            && m_wep.rest_lp.has_value()) {
            set_vec3(m_wep.mag_joint, "set_LocalPosition", *m_wep.rest_lp);

            if (m_wep.rest_lr.has_value()) {
                set_quat(m_wep.mag_joint, "set_LocalRotation", *m_wep.rest_lr);
            }
        }

        m_drop.active = false;
        m_drop.joint = nullptr;

        return;
    }

    const float fall = 0.5f * cfg.gravity * t * t;   // s = 1/2 g t^2
    set_vec3(m_drop.joint, "set_Position", glm::vec3{m_drop.sx, m_drop.sy - fall, m_drop.sz});

    // [ENTKOPPEL_ROT] Weltrotation jeden Frame festhalten.
    if (m_drop.srot.has_value()) {
        set_quat(m_drop.joint, "set_Rotation", *m_drop.srot);
    }
}

// ============================================================================
// Mag in der linken Hand (Lua Z.2739-2949)
// ============================================================================

::REManagedObject* RE4VRReload4::get_left_hand() {
    auto* bt = body_tf();

    if (bt == nullptr) {
        return nullptr;
    }

    if (!re4vr::obj_ok(m_lhand_joint)) {
        m_lhand_joint = joint_by_name(bt, "L_Hand");

        if (m_lhand_joint == nullptr) {
            m_lhand_joint = joint_by_name(bt, "L_Arm_Hand");
        }
    }

    return m_lhand_joint;
}

::REManagedObject* RE4VRReload4::get_thumb_joint() {
    auto* bt = body_tf();

    if (bt == nullptr) {
        return nullptr;
    }

    if (!re4vr::obj_ok(m_thumb_joint)) {
        m_thumb_joint = joint_by_name(bt, "L_Thumb1");
    }

    return m_thumb_joint;
}

// [MANUAL_INSERT] Der Messwert des Handschubs.
// [ZURUECK AUF LEONS WEG 2026-08-15] Gemessen wird ausschliesslich die HOEHE
// des linken Controllers -- fuer BEIDE Charaktere, ohne Gate, ohne Sonderfall.
// Dazwischen lagen Waffen-Y-Achse, Handweg-Integration, Ausreisser-Bremse und
// ein Ada-Gate: alle wieder raus, keiner war durch eine Messung gedeckt.
// Vorzeichen: hoehere Hand = kleinerer Wert -> (d0 - d) waechst.
std::optional<float> RE4VRReload4::mag_push_dist() {
    const auto lp = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");

    if (!lp.has_value()) {
        return std::nullopt;
    }

    return -lp->y;
}

void RE4VRReload4::update_mag_in_hand() {
    // aktuell gefuehrte Waffe (Einleit-Punkt-UI in reload_adv)
    m_reload_ui_wid = m_wep.wid;

    if (m_wep.wid.has_value()) {
        re4vr::lua_set_number("__re4_reload_ui_wid", *m_wep.wid);
    } else {
        re4vr::lua_set_nil("__re4_reload_ui_wid");
    }

    // aktiv durch echten Reload-Grab ODER reinen Einstell-Modus
    ::REManagedObject* joint = nullptr;

    if (m_mag_hand.active) {
        joint = m_mag_hand.joint;
    } else if (m_mag_tune.active) {
        joint = m_wep.mag_joint;
    }

    // [DLC LET-GO] Diese Render-Passes laufen AUCH, waehrend Leon eine seiner
    // Waffen haelt -- und da dieses Teil spaeter laeuft, wuerde es die Pose im
    // selben Frame wieder nullen. Die Pose-Globals gehoeren dem Teil, das die
    // Waffe verwaltet: ist das nicht dieses, fassen wir sie NICHT an. Das
    // Aufraeumen beim Loslassen macht r4_release an der Flanke.
    if (!m_managed) {
        return;
    }

    if (joint == nullptr) {
        // Mag nicht (mehr) in der Hand -> Hand-Pose dem Spiel zurueckgeben
        re4vr::lua_set_nil("__vr_mag_hand_pose");
        re4vr::lua_set_nil("__vr_mag_hand_trx");
        re4vr::lua_set_nil("__vr_mag_hand_try");
        re4vr::lua_set_nil("__vr_mag_hand_trz");

        return;
    }

    const int32_t wid_for = m_mag_hand.active ? m_mag_hand.wid.value_or(m_wep.wid.value_or(0))
                                              : m_wep.wid.value_or(0);
    const auto& m = maghand(wid_for);

    // Greif-Pose NICHT hier anwenden (Pre-Anim -> Engine ueberschreibt).
    // Stattdessen Pose-NAME + Daumen-Offset publizieren; motion wendet es im
    // POST-ANIM-Pass an.
    std::string mp{};

    if (m_wep.wid.has_value()) {
        const auto it = m_mag_pose.find(*m_wep.wid);

        if (it != m_mag_pose.end()) {
            mp = it->second;
        }
    }

    if (mp.empty()) {
        mp = cfg.mag_hold_pose;
    }

    if (!mp.empty()) {
        re4vr::lua_set_string("__vr_mag_hand_pose", mp);
    } else {
        re4vr::lua_set_nil("__vr_mag_hand_pose");
    }

    re4vr::lua_set_number("__vr_mag_hand_trx", m.t_rx);
    re4vr::lua_set_number("__vr_mag_hand_try", m.t_ry);
    re4vr::lua_set_number("__vr_mag_hand_trz", m.t_rz);

    // [SHELL_CLONE] Skull Shaker: den Mag-Joint NICHT in die Hand ziehen ->
    // sonst haengt Part 1 doppelt zum Mesh-Clone in der Hand.
    if (m_wep.wid.has_value() && *m_wep.wid == 6001) {
        return;
    }

    auto* lh = get_left_hand();

    if (lh == nullptr) {
        return;
    }

    glm::vec3 hp{};

    if (!get_vec3(lh, "get_Position", hp)) {
        return;
    }

    glm::quat hr{};
    const bool has_rot = get_quat(lh, "get_Rotation", hr);
    glm::vec3 w = hp;

    if (has_rot) {
        w = hp + (hr * glm::vec3{m.x, m.y, m.z});
    }

    set_vec3(joint, "set_Position", w);

    if (has_rot) {
        set_quat(joint, "set_Rotation",
                 glm::normalize(hr * quat_from_euler(m.rx, m.ry, m.rz)));
    }
}

// Chamber-Ruhepose (lokal) des Mag-Joints erfassen, solange unmanipuliert.
void RE4VRReload4::capture_mag_rest() {
    if (m_wep.mag_joint == nullptr) {
        return;
    }

    if (m_drop.active || m_mag_hand.active || m_mag_insert.active) {
        return;
    }

    // [RUHELAGE HAERTEN] Diese live gemessene Nulllage ist der Endpunkt der
    // Einschubachse -- faengt sie einen Engine-/Eigen-Zustand ein, zeigt die
    // Achse danach falsch und der Handschub bringt keinen Fortschritt mehr.
    if (m_mag_insert.settle) {
        return;
    }

    // Kein reines Gameplay (Killswitch/Cutscene/Menue/Boot): dort posiert die
    // Engine Waffe und Mag selbst. BEWUSST nur bei ausdruecklichem `false`
    // blocken -- ohne Ruhelage gibt es keinen Insert.
    if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") == 0) {
        return;
    }

    if (m_shell_eject_st.preview || m_shell_eject_st.flying) {
        return;   // [SHELL_EJECT] _04 verschoben -> nicht als Ruhepose erfassen
    }

    if (re4vr::lua_has_value("__re4_mag_eject_kf_preview")) {
        return;   // [MAG-EJECT-KEYFRAMES] Mag haengt am Tuning-Punkt
    }

    if (m_mag_out) {
        return;   // [MAG_OUT] Mag-Mesh ist versteckt
    }

    glm::vec3 lp{};
    glm::quat lr{};

    if (get_vec3(m_wep.mag_joint, "get_LocalPosition", lp)) {
        m_wep.rest_lp = lp;
    }

    if (get_quat(m_wep.mag_joint, "get_LocalRotation", lr)) {
        m_wep.rest_lr = lr;
    }

    // [SHOTGUN CHAMBER] Chamber-Port (waffen-lokaler Offset) jetzt erfassen,
    // solange _04 in Ruhe ist: echte _04-Weltpos + Z-Offset in _04-lokalem Z
    // = Port-Welt; dann relativ zur Waffen-Transform speichern.
    const auto zoff = is_shotgun(m_wep.wid.value_or(0))
        ? shotgun_chamber_z(*m_wep.wid) : std::nullopt;

    if (zoff.has_value() && m_wep.tf != nullptr) {
        glm::vec3 jw{}, gp{};
        glm::quat jr{}, gr{};

        if (get_vec3(m_wep.mag_joint, "get_Position", jw)
            && get_quat(m_wep.mag_joint, "get_Rotation", jr)
            && get_vec3(m_wep.tf, "get_Position", gp)
            && get_quat(m_wep.tf, "get_Rotation", gr)) {
            const glm::vec3 cw = jw + (jr * glm::vec3{0.0f, 0.0f, *zoff});
            m_wep.chamber_off = glm::conjugate(gr) * (cw - gp);
        }
    }
}

// ============================================================================
// Mag-Insert (Lua Z.2952-3522)
// ============================================================================

bool RE4VRReload4::start_mag_insert() {
    if (m_wep.mag_joint == nullptr || !m_wep.rest_lp.has_value()) {
        return false;
    }

    glm::vec3 lp{};

    if (!get_vec3(m_wep.mag_joint, "get_LocalPosition", lp)) {
        return false;
    }

    glm::quat lr{};
    const bool has_lr = get_quat(m_wep.mag_joint, "get_LocalRotation", lr);

    const int32_t wid = m_wep.wid.value_or(0);

    m_mag_insert.joint = m_wep.mag_joint;
    m_mag_insert.slp = lp;
    m_mag_insert.slr = has_lr ? std::optional<glm::quat>{lr} : std::nullopt;
    m_mag_insert.rlp = *m_wep.rest_lp;
    m_mag_insert.rlr = m_wep.rest_lr;

    // [SHOTGUN] Insert-ZIEL = Chamber-Port (rest + Z-Offset), nicht die vordere
    // _04-Ruhe -> die Shell gleitet IN den Ladeport statt nach vorne zu fliegen.
    if (const auto zoff = shotgun_chamber_z(wid); zoff.has_value()) {
        m_mag_insert.rlp = glm::vec3{m_wep.rest_lp->x, m_wep.rest_lp->y,
                                     m_wep.rest_lp->z + *zoff};
    }

    // [SHELL-KEYFRAMES] Nutzt diese Waffe die Keyframe-Bahn? Dann faehrt
    // update_mag_insert die geordnete Bahn ab und der lineare Slide gilt NICHT.
    m_mag_insert.keyframe = (m_adv != nullptr) && m_adv->has_shell_keys(wid);

    m_mag_insert.t0 = clock_now();
    m_mag_insert.dur = cfg.insert_dur;

    // [MASTER-TEMPO] Insert-Dauer mit demselben Faktor strecken wie die
    // Push-Geste -> das gesamte Reinladen wird gleichmaessig langsamer.
    if (m_adv != nullptr) {
        if (m_mag_insert.keyframe) {
            // [INSERT = EJECT RUECKWAERTS] Waffen mit rueckwaerts gefahrener
            // Auswurf-Bahn haben eine EIGENE Dauer -- sonst zoegen sie
            // shell_dur, den gemeinsamen Wert der Schrotflinten-Shells.
            const auto kf = m_adv->kf_insert_dur(wid);
            m_mag_insert.dur = kf.value_or(m_adv->shell_dur);
        } else {
            m_mag_insert.dur = cfg.insert_dur * m_adv->time_mult();
        }
    }

    m_mag_insert.active = true;
    m_mag_insert.snd_played = false;

    // [MANUAL_INSERT] Handschub scharf machen: Nullpunkt ist der Messwert GENAU
    // JETZT, also im Moment des Andockens.
    m_mag_insert.manual = false;
    m_mag_insert.prog = 0.0f;

    // [NOTBREMSE] Startwerte fuer den Handschub-Watchdog. Bei JEDEM Andocken
    // frisch -- auch wenn der Handschub gar nicht scharf wird.
    m_mag_insert.wd_t0 = clock_now();
    m_mag_insert.wd_max = 0.0f;
    m_mag_insert.wd_lp0.reset();

    {
        // [WELTWEG WAR DER FEHLER] Der Watchdog merkte sich frueher die
        // WELTPOSITION der linken Hand -- beim Laufen standen so 12 METER
        // "Handweg" im Log. Jetzt dieselbe Groesse wie die Kopplung: linke Hand
        // GEGEN rechte Hand.
        const auto l0 = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");
        const auto r0 = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

        if (l0.has_value() && r0.has_value()) {
            m_mag_insert.wd_lp0 = vec_sub(*l0, *r0);
        }
    }

    {
        // Waffe mit Druecken-Geste? -> Handschub. Ein Ausnahme-Eintrag `false`
        // schlaegt das ab.
        const bool ok_wid = (m_adv != nullptr) && m_adv->is_push_wid(wid)
            && !(m_insert_manual_wid.count(wid) > 0 && !m_insert_manual_wid.at(wid));

        if (cfg.insert_manual && ok_wid) {
            m_mag_insert.d0 = mag_push_dist();
            m_mag_insert.manual = m_mag_insert.d0.has_value();
            m_mag_insert.p0.reset();   // [1:1] Nullpunkt der Kopplung neu setzen
        }
    }

    // [PUSH_POSE] Das Mag verlaesst die linke Hand -> kurz in die universelle
    // Druecken-Pose lerpen. [MANUAL_INSERT] Im Handschub als HALTEN: die Hand
    // dockt per IK ans Magazin und bleibt unten am Mag kleben.
    if (m_adv != nullptr) {
        if (m_mag_insert.manual) {
            m_adv->begin_push_hold(wid);
        } else {
            m_adv->start_push(wid);
        }
    }

    // [INSERT-PUNCH] Ziel der Einschub-Phase liegt um insert_overshoot WEITER
    // in Einschubrichtung. Danach federt es in insert_settle zurueck.
    m_mag_insert.olp.reset();
    m_mag_insert.settle = false;

    const float ov = cfg.insert_overshoot;

    // [MANUAL_INSERT] Kein Overshoot beim Handschub: dort IST die Hand die
    // Position -- ein Ziel hinter der Ruhelage hiesse, am Anschlag noch
    // weiterschieben zu muessen.
    if (ov > 0.0f && !m_mag_insert.keyframe && !m_mag_insert.manual) {
        const glm::vec3 d = vec_sub(m_mag_insert.rlp, m_mag_insert.slp);
        const float len = vec_len(d);

        if (len > 1e-5f) {
            m_mag_insert.olp = m_mag_insert.rlp + (d / len) * ov;
        }
    }

    return true;
}

void RE4VRReload4::update_mag_insert() {
    // [INSERT-PUNCH] Nachfedern aus dem Overshoot zurueck in die Ruhelage.
    // Laeuft NACH dem eigentlichen Insert und schreibt NUR die Position.
    if (m_mag_insert.settle && m_mag_insert.joint != nullptr) {
        float st = static_cast<float>(clock_now() - m_mag_insert.settle_t0)
            / std::max(cfg.insert_settle, 0.01f);

        if (st >= 1.0f) {
            st = 1.0f;

            if (!m_mag_insert.visual) {   // [SS-FLASH] Sicht-Pass beendet nichts
                m_mag_insert.settle = false;
            }
        }

        if (m_mag_insert.olp.has_value()) {
            const float u2 = ease(st);
            const glm::vec3 o = *m_mag_insert.olp;
            const glm::vec3 r = m_mag_insert.rlp;
            set_vec3(m_mag_insert.joint, "set_LocalPosition",
                     glm::vec3{o.x + (r.x - o.x) * u2, o.y + (r.y - o.y) * u2,
                               o.z + (r.z - o.z) * u2});
        }
    }

    if (!m_mag_insert.active || m_mag_insert.joint == nullptr) {
        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);
    float t = static_cast<float>(clock_now() - m_mag_insert.t0)
        / std::max(m_mag_insert.dur, 0.01f);

    // [MANUAL_INSERT] Handschub: t kommt aus dem zurueckgelegten Handweg statt
    // aus der Uhr. 0 = Andockpunkt, 1 = eingerastet. Dazwischen kann man
    // beliebig vor und zurueck -- die Bahn wertet t jedes Mal frisch aus.
    if (m_mag_insert.manual) {
        const auto d = mag_push_dist();

        if (d.has_value() && m_mag_insert.d0.has_value()) {
            // [1:1-KOPPLUNG] Das Magazin klebt am CONTROLLER. Die
            // Controller-Position wird auf die ECHTE Einschubachse projiziert
            // (Andockpunkt -> Ruhelage, mit der Waffe gedreht).
            // [KEIN WELTANKER] Gemessen wird NICHT gegen einen Punkt an der
            // Waffe: beim LAUFEN wandert der mit, und die Fortbewegung zaehlt
            // als Einschub. Also LINKE gegen RECHTE Hand -- aber
            // RICHTUNGSBEHAFTET, entlang der Einschubachse.
            // ZWINGEND die ROHEN Controller-Positionen: die Handjoints haengen
            // im Einschub per IK am Magazin -> Rueckkopplung.
            std::optional<float> p11{};

            if (m_mag_11 && m_adv != nullptr && m_wep.tf != nullptr) {
                const auto lp = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");
                const auto rp = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");
                const glm::vec3 b = m_mag_insert.olp.value_or(m_mag_insert.rlp);
                glm::quat wr{};

                if (lp.has_value() && rp.has_value()
                    && get_quat(m_wep.tf, "get_Rotation", wr)) {
                    const glm::vec3 a = m_mag_insert.slp;
                    const glm::vec3 ax = wr * vec_sub(b, a);
                    const float L2 = ax.x * ax.x + ax.y * ax.y + ax.z * ax.z;

                    if (L2 > 1e-8f) {
                        float v = ((lp->x - rp->x) * ax.x + (lp->y - rp->y) * ax.y
                                   + (lp->z - rp->z) * ax.z) / L2;

                        if (!m_mag_insert.p0.has_value()) {
                            m_mag_insert.p0 = v;
                        }

                        p11 = v - *m_mag_insert.p0;
                    }
                }
            }

            if (p11.has_value()) {
                m_mag_insert.prog = *p11;
            } else {
                m_mag_insert.prog = (*m_mag_insert.d0 - *d)
                    / std::max(cfg.insert_travel, 0.01f);
            }
        }

        t = m_mag_insert.prog;

        // Hinter den Andockpunkt zurueckgezogen -> das Mag liegt wieder in der
        // linken Hand. KEIN Abbruch der Reload-Logik: gebucht wird erst am
        // Anschlag. [SS-FLASH] Nur im echten Pass.
        if (!m_mag_insert.visual
            && t <= -(cfg.insert_back_out / std::max(cfg.insert_travel, 0.01f))) {
            m_mag_insert.active = false;
            m_mag_insert.settle = false;
            m_mag_insert.manual = false;

            // Druecken-Pose sauber ausfahren lassen -- sonst bliebe die Hand am
            // Magazin-Dock haengen, obwohl das Mag wieder der Hand gehoert.
            if (m_adv != nullptr) {
                m_adv->end_push_hold();
            }

            m_mag_hand.joint = m_mag_insert.joint;
            m_mag_hand.wid = m_wep.wid;
            // Sentinel: erst wieder andocken, wenn ein Stueck VORgeschoben
            // wurde -- sonst klebte es im naechsten Frame sofort wieder an.
            m_mag_hand.redock_pending = true;
            m_mag_hand.redock_d.reset();
            // In der Hand bleibt es NUR bei gehaltenem Grip -- sonst haelt es
            // niemand und es faellt zu Boden.
            m_mag_hand.active = m_mag_hand.grip_held;
            m_mag_hand.want_drop = !m_mag_hand.active;

            return;
        }

        // [NOTBREMSE] "Ich schiebe, und das Magazin geht nicht rein": das Mag
        // klebt am Andockpunkt, prog bleibt bei ~0. Greift nur, wenn der
        // Controller sich nachweislich bewegt hat (> 6 cm) und nach 1.5 s der
        // Schub unter 5 Prozent liegt. Haelt man die Hand still, zuendet sie
        // NICHT -> kein Automatik-Einschub.
        if (!m_mag_insert.visual && m_mag_insert.wd_t0.has_value()
            && (clock_now() - *m_mag_insert.wd_t0) > 1.5 && m_mag_insert.prog < 0.05f) {
            const auto lr = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");
            const auto rr = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

            if (m_mag_insert.wd_lp0.has_value() && lr.has_value() && rr.has_value()) {
                const glm::vec3 ln = vec_sub(*lr, *rr);
                const float md = vec_len(vec_sub(ln, *m_mag_insert.wd_lp0));

                if (md > m_mag_insert.wd_max) {
                    m_mag_insert.wd_max = md;
                }
            }

            if (m_mag_insert.wd_max > 0.06f) {
                m_mag_insert.manual = false;
                m_mag_insert.t0 = clock_now();   // Zeitbahn beginnt JETZT
                m_mag_insert.wd_t0.reset();

                // `end_push_hold` muss hier selbst gerufen werden: der Abschluss
                // unten faehrt die Pose nur im manual-Zweig zurueck.
                if (m_adv != nullptr) {
                    m_adv->end_push_hold();
                }

                t = 0.0f;
            }
        }

        if (t < 0.0f) {
            t = 0.0f;
        }
    }

    if (t > 1.0f) {
        t = 1.0f;
    }

    // [SND] Einrast-Sound kurz VOR dem Anschlag, einmalig. Mit Punch sitzt er
    // spaeter (insert_snd_at), ohne Punch bleibt es beim alten Timing (0.75).
    // [MANUAL_INSERT] Im Handschub spielt hier NICHTS: jede Wegschwelle liegt
    // vor dem Anschlag und laesst sich zurueckziehen.
    if (!m_mag_insert.visual && !m_mag_insert.manual && !m_mag_insert.snd_played
        && t >= (cfg.insert_punch ? cfg.insert_snd_at : 0.75f)) {
        m_mag_insert.snd_played = true;
        play_weapon_sound(snd_id(wid, "mag_insert"));
    }

    if (m_mag_insert.keyframe) {
        // [SHELL-KEYFRAMES] Shell-Joint entlang der geordneten Keyframes
        // (Position + Rotation, relativ zur Waffe).
        if (m_adv != nullptr && m_wep.tf != nullptr) {
            m_adv->apply_shell_keys(m_wep.tf, m_mag_insert.joint, wid, t);
        }
    } else {
        // [INSERT-PUNCH] Ease-IN statt Smoothstep. [MANUAL_INSERT] Im Handschub
        // LINEAR: die Hand ist die Position.
        const float u = m_mag_insert.manual ? t
            : (cfg.insert_punch ? (t * t * t) : ease(t));

        glm::vec3 a = m_mag_insert.slp;
        const glm::vec3 b = m_mag_insert.olp.value_or(m_mag_insert.rlp);

        // [EINLEIT-PUNKT] Startpunkt ist NICHT die Hand, sondern ein fester
        // Punkt am Waffen-Skelett. Fehlt der Joint, liefert dock_local nichts
        // und es bleibt beim bisherigen Verhalten.
        if (m_adv != nullptr && m_wep.tf != nullptr) {
            if (const auto dl = m_adv->dock_local(m_wep.tf, wid); dl.has_value()) {
                a = *dl;
            }

            // [NUR TMP/MP-AF] Diese beiden haben den Mag-Joint _04 an einem
            // ANDEREN Parent als die Pistolen -> der Waffenraum-Punkt sass
            // seitlich. NUR fuer sie den Welt-Punkt ueber die aktuelle
            // Joint-Lage in den Parent-Raum bringen:
            //   localTarget = localPos + (localRot * worldRot^-1) * (W - worldPos)
            if (parent_space_dock(wid) && m_mag_insert.joint != nullptr) {
                const auto W = m_adv->dock_world(m_wep.tf, wid);

                if (W.has_value()) {
                    glm::vec3 jw{}, jlp{};
                    glm::quat jr{}, jlr{};

                    if (get_vec3(m_mag_insert.joint, "get_Position", jw)
                        && get_quat(m_mag_insert.joint, "get_Rotation", jr)
                        && get_vec3(m_mag_insert.joint, "get_LocalPosition", jlp)
                        && get_quat(m_mag_insert.joint, "get_LocalRotation", jlr)) {
                        a = jlp + (jlr * (glm::conjugate(jr) * (*W - jw)));
                    }
                }
            }
        }

        set_vec3(m_mag_insert.joint, "set_LocalPosition",
                 glm::vec3{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u,
                           a.z + (b.z - a.z) * u});

        if (m_mag_insert.slr.has_value() && m_mag_insert.rlr.has_value()) {
            set_quat(m_mag_insert.joint, "set_LocalRotation",
                     qnlerp(*m_mag_insert.slr, *m_mag_insert.rlr, u));
        }
    }

    // [SS-FLASH] Ab hier haengt die RELOAD-LOGIK (Ammo-Buchung, Rack, Haptik,
    // mag_out). Der Sicht-Pass steigt hier IMMER aus -- er hat oben nur die
    // Position geschrieben. Aus dieser Phase darf NIE ein Ammo-Write entstehen.
    if (m_mag_insert.visual) {
        return;
    }

    // [SELBST-EINRASTEN] Im Handschub genuegt insert_snap_at des Weges.
    float snap = 1.0f;

    if (m_mag_insert.manual) {
        snap = std::clamp(cfg.insert_snap_at, 0.5f, 1.0f);
    }

    if (t < snap) {
        return;
    }

    m_mag_insert.active = false;

    // [MANUAL_INSERT] Der Einrast-Klack gehoert an DIESEN Moment: hier ist das
    // Magazin tatsaechlich eingerastet.
    if (m_mag_insert.manual && !m_mag_insert.snd_played) {
        m_mag_insert.snd_played = true;
        play_weapon_sound(snd_id(wid, "mag_insert"));
    }

    if (m_mag_insert.manual) {
        m_mag_insert.manual = false;

        if (m_adv != nullptr) {
            m_adv->end_push_hold();
        }
    }

    // [INSERT-PUNCH] Anschlag erreicht: Nachfedern + kurzer Haptik-Puls auf der
    // RECHTEN Hand (die haelt die Waffe). Beides rein sensorisch.
    if (m_mag_insert.olp.has_value()) {
        m_mag_insert.settle = true;
        m_mag_insert.settle_t0 = clock_now();
    }

    if (cfg.insert_haptic > 0.0f) {
        auto& vr = VR::get();

        if (vr != nullptr && vr->is_hmd_active()) {
            vr->trigger_haptic_vibration(0.0f, 0.06f, 90.0f, cfg.insert_haptic,
                                         vr->get_right_joystick());
        }
    }

    // [RACK-LOCK] Ammo gerade eingelegt -> Shaft/Slide-Grab kurz sperren.
    m_rack._ammo_input_t = clock_now();

    // [MAG_OUT] PHYSISCH ein frisches Mag in der Kammer -> Anti-Doppeldrop-Flag
    // loesen. An den INSERT gekoppelt, NICHT an loaded>0 (Ammo kann 0 bleiben).
    m_mag_out = false;

    // Rack NUR noetig, wenn die Waffe beim Drop LEER war. Taktischer Reload
    // (noch Patronen drin) -> KEIN Rack, Slide direkt vor (gechambert).
    // Ladestand UNMITTELBAR vor dem Einsetzen -- muss HIER schon feststehen.
    auto loaded0 = gun_ammo();

    if (!loaded0.has_value()) {
        auto* wi0 = get_live_weapon_item();
        loaded0 = (wi0 != nullptr) ? call_enum(wi0, "get_CurrentAmmoCount") : std::nullopt;
    }

    // [ACCESSOR-FOLGE] `_empty_chamber` zaehlt NICHT, wenn die 0 von unserem
    // eigenen Mag-Drop-Leeren stammt -- sonst rackt jede Magazinwaffe nach
    // JEDEM Reload.
    const bool empty_chamber = loaded0.has_value() && *loaded0 == 0
        && !is_shotgun(wid) && !m_rack._zeroed_by_us;

    if (m_rack.empty_when_dropped || empty_chamber) {
        m_rack.needs = true;   // sticky, nur durch Rack-Geste/Wechsel/Reset weg
    } else {
        m_rack.needs = false;

        // [SLIDE_STUCK_BACK FIX] Taktischer Reload nach einer VORHER leeren
        // Kammer: die Engine hatte den Slide bei 0 Ammo zurueckgelockt und
        // spielt nach manuellem Reload keine Schliess-Anim. Ein EINMALIGER
        // Snap reicht NICHT -> _chambered_hold haelt ihn jeden Frame.
        if (!engine_closes_slide(wid)) {
            m_rack._chambered_hold = true;
            m_rack._prev_gun_ammo.reset();
        }

        if (m_wep.slide_joint != nullptr && !engine_closes_slide(wid)) {
            glm::vec3 cur{};

            if (get_vec3(m_wep.slide_joint, "get_LocalPosition", cur)) {
                set_vec3(m_wep.slide_joint, "set_LocalPosition",
                         glm::vec3{cur.x, cur.y, slide_pose(wid).rest_z});
            }
        }
    }

    // [EMPTY-RELOAD SLIDE] War die Waffe VOR diesem Insert komplett leer?
    // -> dieser Reload chambert per _08-Lade-Slide. VOR dem Ammo-Add lesen.
    if (empty_reload_joint(wid) != nullptr) {
        auto ld = gun_ammo();

        if (!ld.has_value()) {
            auto* wi = get_live_weapon_item();
            ld = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;
        }

        // STICKY: einmal leer-nachgeladen -> Slide-Modus bleibt bis zum Rack.
        // NUR bei ECHTEM 0 (kein Wert -> NICHT setzen).
        if (ld.has_value() && *ld == 0) {
            m_rack.empty_reload = true;
        }
    }

    // [SHOTGUN] Shell eingelegt -> Chamber-Fenster oeffnen. NUR die Riot Gun
    // braucht bei taktischem Reload KEINEN Pump.
    if (is_shotgun(wid)) {
        if (!no_reload_cycle(wid) || m_rack.empty_reload) {
            m_rack.needs = true;
        }
    }

    m_rack.empty_when_dropped = false;
    m_rack._zeroed_by_us = false;   // Merker verbraucht, Zyklus zu Ende

    if (!cfg.reload_ammo) {
        return;
    }

    // echter Ammo-Reload (wie das Game): Mag fuellen + Reserve abziehen
    auto* wi = get_live_weapon_item();
    auto* p = pe();
    auto* inv = (p != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController") : nullptr;

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    int32_t reserve = 0;

    if (inv != nullptr && ammo_id.has_value()) {
        reserve = item_count_sum(inv, *ammo_id);
    }

    if (is_shotgun(wid)) {
        // [ROTARY_DEFER] Striker: Ammo NICHT beim Insert zaehlen -> erst am
        // Drehschalter. Alle anderen Shotguns: sofort laden wie bisher.
        if (rotary_cycle(wid)) {
            m_rotary.pending = true;
        } else {
            rotary_do_load();
        }

        return;
    }

    // [RUNTIME-FIX 0/0] Ziel = min(cap, retained + reserve). ZWEI Quellen
    // getrennt: (1) der Reserve-Anteil ueber den Ladeweg (zieht die Reserve),
    // (2) der Retained-Anteil (schon bezahlt) frei auffuellen OHNE Abzug.
    const int32_t cap = (wi != nullptr)
        ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
    const int32_t target = std::min(cap, m_mag_retained + reserve);
    const int32_t used = std::max(0, target - m_mag_retained);

    const auto read_rsv = [&]() -> int32_t {
        return (inv != nullptr && ammo_id.has_value()) ? item_count_sum(inv, *ammo_id) : reserve;
    };

    const int32_t r_b4 = reserve;
    const int32_t b4 = gun_ammo().value_or(0);
    const auto et = get_equip_type_main();

    // (1) Reserve-Anteil laden. NUR wenn wirklich eine Waffe live equippt ist
    // UND die live equippte Waffe == der Reload-Waffe: beim Um-Equippen mitten
    // im Reload zeigt get_equip_wid schon die NEUE Waffe, das Gun-Item ist im
    // Uebergangsframe aber noch null.
    const auto ewid_now = get_equip_wid();

    if (et.has_value() && inv != nullptr && used > 0 && ewid_now.value_or(-1) >= 0
        && ewid_now.has_value() && m_wep.wid.has_value() && *ewid_now == *m_wep.wid) {
        load_and_book(inv, *et, used, false);
    }

    const int32_t af = gun_ammo().value_or(b4);
    const int32_t got = std::max(0, af - b4);

    // Reserve nur ziehen, wenn die Engine es nicht selbst tat.
    if (got > 0 && inv != nullptr && ammo_id.has_value() && read_rsv() >= r_b4) {
        safe_reduce(inv, *ammo_id, got);
    }

    // (2) Retained-Anteil: auf target auffuellen (frei, KEIN Reserve-Abzug)
    if (gun_ammo().value_or(af) < target && wi != nullptr) {
        if (auto* ptr = field_i32(wi, "_CurrentAmmoCount", 0x44); ptr != nullptr) {
            *ptr = target;
        }

        const int32_t now = gun_ammo().value_or(b4 + got);

        if (now < target) {
            re4vr::call_safe<void*>(wi, "addAmmoCount", target - now, false);
        }
    }

    m_mag_retained = 0;
}

// ============================================================================
// Andockpunkte + Proximity (Lua Z.3524-3636)
// ============================================================================

// Chamber-Weltpunkt = Waffen-Transform * gecachter Chamber-Offset.
std::optional<glm::vec3> RE4VRReload4::shotgun_chamber_world() {
    if (m_wep.tf == nullptr || !m_wep.chamber_off.has_value()) {
        return std::nullopt;
    }

    glm::vec3 gp{};
    glm::quat gr{};

    if (!get_vec3(m_wep.tf, "get_Position", gp) || !get_quat(m_wep.tf, "get_Rotation", gr)) {
        return std::nullopt;
    }

    return gp + (gr * *m_wep.chamber_off);
}

// [DOCK-PORT] Andock-Spot = LIVE-Weltpos eines FESTEN Gun-Joints + Offset in
// dessen lokalen Achsen. Der Joint bewegt sich (anders als der Mag-Joint) NICHT
// mit der Hand.
std::optional<glm::vec3> RE4VRReload4::dock_port_world() {
    if (!m_wep.wid.has_value() || m_wep.tf == nullptr) {
        return std::nullopt;
    }

    const auto it = m_dock_port.find(*m_wep.wid);

    if (it == m_dock_port.end()) {
        return std::nullopt;
    }

    auto* j = joint_by_name(m_wep.tf, it->second.joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{it->second.x, it->second.y, it->second.z});
}

// [LEVER_PORT] Greif-Punkt des Break-Hebels (wie dock_port_world).
std::optional<glm::vec3> RE4VRReload4::lever_port_world() {
    if (!m_wep.wid.has_value() || m_wep.tf == nullptr) {
        return std::nullopt;
    }

    const auto it = m_lever_port.find(*m_wep.wid);

    if (it == m_lever_port.end()) {
        return std::nullopt;
    }

    auto* j = joint_by_name(m_wep.tf, it->second.joint);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{0.0f, 0.0f, it->second.z});
}

// Proximity: Mag in Hand nah genug an die Waffe -> automatisch in die Kammer.
void RE4VRReload4::check_mag_insert_proximity() {
    if (!m_mag_hand.active) {
        return;
    }

    auto* lh = get_left_hand();

    if (lh == nullptr) {
        return;
    }

    glm::vec3 hp{};

    if (!get_vec3(lh, "get_Position", hp)) {
        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);

    // Reihenfolge: alter DOCK_PORT -> Keyframe #1 -> Adv-Punkt -> Ersatz.
    // [KEYFRAME-ANKER] Die Schrotflinten mit Shell-Bahn und die Magnums stehen
    // WEDER in DOCK_PORT NOCH in DOCK_ALLOWED -- die Kette fiel fuer sie bis auf
    // `__vr_rh_world` durch, also auf die RECHTE HAND. Gemessen wurde damit
    // Abstand linke Hand -> GRIFFHAND statt -> Ladeport. Richtig ist der
    // ANKERPUNKT DER BAHN: Keyframe #1, dieselbe waffenrelative Rechnung wie
    // apply_shell_keys. rev_insert-Waffen ausgenommen -- dort ist tt=0 der
    // Auswurf-Endpunkt, also der falsche Punkt.
    std::optional<glm::vec3> kfp{};

    if (m_adv != nullptr && m_wep.tf != nullptr && m_wep.wid.has_value()
        && m_insert_kf_anchor && m_adv->has_shell_keys(wid)
        && !m_adv->uses_rev_insert(wid)) {
        RE4VRReloadAdv::Key k{};

        if (m_adv->shell_pose_at(wid, 0.0f, k)) {
            glm::vec3 wp0{};
            glm::quat wr0{};

            if (get_vec3(m_wep.tf, "get_Position", wp0)
                && get_quat(m_wep.tf, "get_Rotation", wr0)) {
                kfp = wp0 + (wr0 * glm::vec3{k.x, k.y, k.z});
            }
        }
    }

    std::optional<glm::vec3> advp{};

    if (m_adv != nullptr && m_wep.tf != nullptr) {
        advp = m_adv->dock_world(m_wep.tf, wid);
    }

    std::optional<glm::vec3> gp = dock_port_world();

    if (!gp.has_value()) { gp = kfp; }
    if (!gp.has_value()) { gp = advp; }
    if (!gp.has_value()) { gp = re4vr::lua_get_vec3("__vr_rh_world"); }

    if (!gp.has_value() && m_wep.tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_wep.tf, "get_Position", p)) {
            gp = p;
        }
    }

    if (!gp.has_value()) {
        return;
    }

    const float d = vec_len(vec_sub(hp, *gp));
    m_mag_hand.dist = d;   // fuer die UI-Anzeige

    // [INSERT-DIST PRO WAFFE] Per-Waffe-Override vor dem Gattungs-Wert.
    float idist = m_insert_dist["pistols"];

    if (const char* cat = category_of(wid); cat != nullptr) {
        const auto ci = m_insert_dist.find(cat);

        if (ci != m_insert_dist.end()) {
            idist = ci->second;
        }
    }

    if (const auto wit = m_insert_dist_wid.find(wid); wit != m_insert_dist_wid.end()) {
        idist = wit->second;
    }

    // [MANUAL_INSERT] Nach einem Rueckzieher steht die Hand noch mitten in der
    // Andock-Distanz -- ohne Sperre klebte das Mag sofort wieder an. Der
    // Sentinel heisst "Abstand beim naechsten Durchlauf einmalig merken".
    if (m_mag_hand.redock_pending) {
        m_mag_hand.redock_d = d;
        m_mag_hand.redock_pending = false;

        return;
    }

    if (m_mag_hand.redock_d.has_value()) {
        if (d > idist) {
            m_mag_hand.redock_d.reset();       // normal weit weg
        } else if (d <= (*m_mag_hand.redock_d - cfg.insert_redock)) {
            m_mag_hand.redock_d.reset();       // wieder Richtung Waffe geschoben
        } else {
            return;                            // im Totband: NICHT andocken
        }
    }

    if (d <= idist) {
        m_mag_hand.active = false;
        start_mag_insert();
    }
}

// ============================================================================
// Mag in die Hand / fallen lassen (Lua Z.3639-3664)
// ============================================================================

bool RE4VRReload4::mag_to_hand() {
    if (m_wep.mag_joint == nullptr) {
        return false;
    }

    stop_mag_drop();
    m_mag_insert.active = false;
    m_mag_insert.settle = false;
    m_mag_tune.active = false;
    m_mag_hand.active = true;
    m_mag_hand.joint = m_wep.mag_joint;
    m_mag_hand.wid = m_wep.wid;
    m_mag_hand.redock_d.reset();       // frischer Grab -> keine Andock-Sperre
    m_mag_hand.redock_pending = false;

    return true;
}

bool RE4VRReload4::drop_mag_simple() {
    if (m_wep.mag_joint == nullptr) {
        return false;
    }

    glm::vec3 p{};

    if (!get_vec3(m_wep.mag_joint, "get_Position", p)) {
        return false;
    }

    m_drop.joint = m_wep.mag_joint;
    m_drop.use_module = false;
    m_drop.sx = p.x;
    m_drop.sy = p.y;
    m_drop.sz = p.z;

    glm::quat r{};
    m_drop.srot = get_quat(m_wep.mag_joint, "get_Rotation", r)
        ? std::optional<glm::quat>{r} : std::nullopt;

    m_drop.t0 = clock_now();
    m_drop.active = true;

    return true;
}

// [TOP_LOADER] aktuelle Reserve-Ammo der equippten Waffe.
int32_t RE4VRReload4::current_reserve() {
    auto* wi = get_live_weapon_item();

    if (wi == nullptr) {
        wi = equip_gun();
    }

    if (wi == nullptr) {
        return 0;
    }

    const auto ammo_id = call_enum(wi, "get_CurrentAmmo");

    if (!ammo_id.has_value()) {
        return 0;
    }

    auto* p = pe();
    auto* inv = (p != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController") : nullptr;

    if (inv == nullptr) {
        return 0;
    }

    return item_count_sum(inv, *ammo_id);
}

// [SHOTGUN GIMMICK] Rein kosmetisch: die Shell fliegt beim unnoetigen Pump,
// solange Munition geladen ist. KEINE Ammo-Aenderung.
bool RE4VRReload4::pump_one_out() {
    auto* wi = get_live_weapon_item();

    if (wi == nullptr) {
        return false;
    }

    return call_enum(wi, "get_CurrentAmmoCount").value_or(0) > 0;
}

// ============================================================================
// Mag-Griff vom Holster (Lua Z.3774-3835)
// ============================================================================
// [DLC] Nicht unsere Waffe? -> an die bestehende Kette weiterreichen
// (reload3/reload2/reload). Der Merker grip_held wird dabei bewusst NICHT
// gesetzt: fuer Leons Waffen fuehrt der Main-Teil seinen eigenen.
std::optional<bool> RE4VRReload4::set_mag_chain_probe(bool active) {
    const auto wid = get_equip_wid();

    if (!wid.has_value() || category_of(*wid) == nullptr) {
        return std::nullopt;
    }

    return set_mag_in_hand(active);
}

bool RE4VRReload4::set_mag_in_hand(bool active) {
    const int32_t wid = m_wep.wid.value_or(0);

    // [MANUAL_INSERT] Haelt die linke Hand den Grip GERADE? Diese Funktion ist
    // die einzige verlaessliche Quelle: das Holster ruft sie flankenweise beim
    // Druecken UND beim Loslassen. Waehrend eines laufenden Inserts ist
    // mag_hand.active false -- ohne diesen Merker wuesste der Rueckzieher
    // nicht, dass der Finger laengst offen ist.
    m_mag_hand.grip_held = active;

    if (active) {
        if (!m_managed) {
            return false;   // [LET-GO] Toggle aus -> kein Grab, alles nativ
        }

        if (is_shotgun(wid)) {
            // [SHOTGUN] Shell aus dem Holster in die linke Hand. KEIN mag_out
            // noetig -> nur greifbar, wenn Reserve da ist und die Roehre nicht
            // schon voll ist.
            // [STRIKER] Nach einer eingelegten Shell MUSS erst der Drehschalter
            // gedreht werden.
            if (rotary_cycle(wid) && m_rack.needs) {
                return false;
            }

            // [SKULL SHAKER] Shell nur bei OFFENEM Hebel holbar.
            if (break_action(wid) && re4vr::lua_get_tribool("__vr_break_open") != 1) {
                return false;
            }

            if (m_mag_hand.active || m_mag_insert.active) {
                return false;
            }

            auto* wi0 = get_live_weapon_item();
            const int32_t loaded = (wi0 != nullptr)
                ? call_enum(wi0, "get_CurrentAmmoCount").value_or(0) : 0;
            const int32_t cap0 = (wi0 != nullptr)
                ? call_enum(wi0, "get_CurrentAmmoMax").value_or(0) : 0;

            if (current_reserve() <= 0 || (cap0 > 0 && loaded >= cap0)) {
                return false;
            }

            const bool ok = mag_to_hand();

            if (ok) {
                play_weapon_sound(snd_id(wid, "mag_holster"));
            }

            return ok;
        }

        // Nur erlauben, wenn die Kammer LEER ist (Mag schon draussen). Es gibt
        // nur EINEN Mag-Joint: bei vollem Mag wuerde der Holster-Grab das
        // Kammer-Mag herausziehen.
        if (m_mag_hand.active) {
            return false;   // kein zweites Mag in die Hand
        }

        if (!m_mag_out) {
            return false;
        }

        const bool ok = mag_to_hand();

        if (ok) {
            play_weapon_sound(snd_id(wid, "mag_holster"));
        }

        return ok;
    }

    // Grip losgelassen
    if (is_shotgun(wid)) {
        // [SHOTGUN] Shell nicht eingefuehrt -> faellt auf den Boden (wie ein
        // Pistolen-Mag), NICHT zurueck in die Ruhepose.
        if (m_mag_hand.active) {
            m_mag_hand.active = false;
            drop_mag_simple();
        }

        return true;
    }

    // war das Mag noch in der Hand -> faellt auf den Boden.
    if (m_mag_hand.active) {
        m_mag_hand.active = false;
        drop_mag_simple();
    }

    return true;
}

// ============================================================================
// Mag-Auswurf (Lua Z.3846-3938)
// ============================================================================
// ROBUSTER Auswurf: raeumt JEDEN Zwischenzustand ab und startet immer einen
// frischen Drop. Nie blockiert.
bool RE4VRReload4::force_eject() {
    // [UNLIMITED] Waffe mit Unlimited-Ammo muss NIE nachladen -> B-Drop
    // stilllegen (sonst haengt der Reload-Flow).
    if (auto& w2 = RE4VRWeapons2::get(); w2 != nullptr && w2->is_unlimited()) {
        return false;
    }

    refresh_weapon();   // Joint nach Save-Load/Wechsel neu aufloesen

    if (m_wep.mag_joint == nullptr) {
        return false;
    }

    // [MAG_OUT] Mag ist schon draussen -> NICHT noch eins droppen.
    if (m_mag_out) {
        return false;
    }

    // [KEIN DROP OHNE RESERVE] Ohne Nachschub wird das Magazin gar nicht erst
    // ausgeworfen -- gilt fuer JEDE Waffe.
    if (current_reserve() <= 0) {
        return false;
    }

    // [0-RESERVE-KEIN-EJECT] FAIL-OPEN: nur sperren, wenn die Reserve-Kette
    // SAUBER liest UND 0 ergibt; jeder unlesbare Zwischenzustand laesst B durch.
    {
        auto* ewi = get_live_weapon_item();

        if (ewi == nullptr) {
            ewi = equip_gun();
        }

        const auto eam = (ewi != nullptr) ? call_enum(ewi, "get_CurrentAmmo") : std::nullopt;
        auto* p3 = pe();
        auto* inv3 = (p3 != nullptr)
            ? re4vr::call_safe<::REManagedObject*>(p3, "get_InventoryController") : nullptr;

        if (ewi != nullptr && eam.has_value() && inv3 != nullptr) {
            if (item_count_sum(inv3, *eam) <= 0) {
                return false;
            }
        }
    }

    // RE9-Latch: war die Waffe JETZT (vor dem Drop) leer? -> nach dem Reload
    // Rack noetig. FRISCHER Engine-Read statt des Caches.
    {
        const auto fresh_ga = gun_ammo();

        if (fresh_ga.has_value()) {
            m_rack.empty_when_dropped = (*fresh_ga == 0);
        } else {
            m_rack.empty_when_dropped = (live_loaded_count().value_or(-1) == 0);
        }

        // [KAMMER] Der Read oben kann bereits UNSERE 0 sehen. Der beim Nullen
        // gemerkte Magazinrest ist die verlaessliche Auskunft: war etwas drin,
        // war die Kammer nicht leer.
        // [BESITZER PRUEFEN] Der Rest zaehlt nur, wenn er zu DIESER Waffe
        // gehoert -- sonst konnte ein liegengebliebener Wert einer FREMDEN
        // Waffe die Rack-Pflicht abschalten. BEWUSST GROSSZUEGIG: verworfen
        // wird nur der echte Fremdfall.
        if (m_mag_carry.has_value() && *m_mag_carry > 0) {
            const auto cw = m_mag_carry_wid;
            const auto ew = get_equip_wid();

            if (!cw.has_value() || (ew.has_value() && *cw == *ew)) {
                m_rack.empty_when_dropped = false;
            }
        }
    }

    m_rack._chambered_hold = false;   // neuer Reload-Zyklus -> Halt aus

    // NIE blockiert: jeden laufenden Zwischenzustand abraeumen, Mag in
    // Ruhepose, frisch droppen.
    stop_mag_drop();
    m_mag_hand.active = false;
    m_mag_insert.active = false;
    m_mag_insert.settle = false;
    m_mag_tune.active = false;

    if (m_wep.rest_lp.has_value()) {
        set_vec3(m_wep.mag_joint, "set_LocalPosition", *m_wep.rest_lp);
    }

    if (m_wep.rest_lr.has_value()) {
        set_quat(m_wep.mag_joint, "set_LocalRotation", *m_wep.rest_lr);
    }

    const bool started = start_mag_drop();

    if (started) {
        m_mag_out = true;   // erst bei erfolgreichem Drop setzen
        play_weapon_sound(snd_id(m_wep.wid.value_or(0), "mag_eject"));
    }

    return started;
}

// B kommt aus dem Binding: es liest den rechten B-Knopf ohnehin jeden Frame und
// publiziert ihn VOR dem Consume. Das ist die EINE autoritative Quelle -- ein
// eigener vrmod-Read haette ein stale Action-Handle und legte B dauerhaft tot.
bool RE4VRReload4::right_b_down() {
    return re4vr::lua_get_tribool("__vr_raw_r_bbutton") == 1;
}

// ============================================================================
// Ladestand + Gun-State (Lua Z.3977-4075)
// ============================================================================

std::optional<int32_t> RE4VRReload4::read_loaded_of(::REManagedObject* wi,
                                                       std::optional<int32_t> want_wid) {
    if (wi == nullptr) {
        return std::nullopt;
    }

    if (want_wid.has_value()) {
        const auto cwid = call_enum(wi, "get_WeaponId");

        if (cwid.has_value() && *cwid != *want_wid) {
            return std::nullopt;   // falsches Item -> ignorieren
        }
    }

    return call_enum(wi, "get_CurrentAmmoCount");
}

// KONSISTENZ ist alles: lesen vom GLEICHEN Item, auf das set_gun_loaded /
// addAmmoCount schreiben. getCurrentGunAmmo war eine andere Quelle -> raus.
std::optional<int32_t> RE4VRReload4::live_loaded_count() {
    const auto ewid = get_equip_wid();

    if (const auto n = read_loaded_of(get_live_weapon_item(), ewid); n.has_value()) {
        return n;
    }

    return read_loaded_of(get_weapon_item(), std::nullopt);
}

// [GUN_STATE] Live-Gun der EQUIPPTEN Waffe DIREKT aus PlayerEquipment.WeaponList
// (Dictionary<WeaponID, Arms>). Kein Hook, keine Baum-Suche, kein Neustart.
::REManagedObject* RE4VRReload4::get_gun() {
    auto* p = pe();

    if (p == nullptr) {
        return nullptr;
    }

    ::REManagedObject* wl = nullptr;

    if (auto* td = utility::re_managed_object::get_type_definition(p); td != nullptr) {
        if (auto* f = td->get_field("WeaponList"); f != nullptr) {
            wl = f->get_data<::REManagedObject*>(p);
        }
    }

    if (wl == nullptr) {
        return nullptr;
    }

    const auto ewid = get_equip_wid();

    if (!ewid.has_value()) {
        return nullptr;
    }

    // 1) get_Item mit dem Enum-Key
    if (auto* a = re4vr::call_safe<::REManagedObject*>(wl, "get_Item", *ewid); a != nullptr) {
        if (call_enum(a, "get_CurrentState").has_value()) {
            return a;
        }
    }

    // 2) Fallback: ueber die internen _entries iterieren
    ::REManagedObject* entries = nullptr;
    int32_t cnt = 0;

    if (auto* td = utility::re_managed_object::get_type_definition(wl); td != nullptr) {
        if (auto* f = td->get_field("_entries"); f != nullptr) {
            entries = f->get_data<::REManagedObject*>(wl);
        }

        if (auto* f = td->get_field("_count"); f != nullptr) {
            cnt = f->get_data<int32_t>(wl);
        }
    }

    if (entries == nullptr) {
        return nullptr;
    }

    for (int32_t i = 0; i < cnt; ++i) {
        // [ARRAY-BINDING] entries[i] ist das Index-Binding, nicht get_Item.
        auto* e = re4vr::array_element(entries, i);

        if (e == nullptr) {
            continue;
        }

        ::REManagedObject* v = nullptr;

        if (auto* td = utility::re_managed_object::get_type_definition(e); td != nullptr) {
            if (auto* f = td->get_field("value"); f != nullptr) {
                v = f->get_data<::REManagedObject*>(e);
            }
        }

        if (v == nullptr) {
            continue;
        }

        const auto vwid = call_enum(v, "get_WeaponID");

        if (vwid.has_value() && *vwid == *ewid && call_enum(v, "get_CurrentState").has_value()) {
            return v;
        }
    }

    return nullptr;
}

std::optional<int32_t> RE4VRReload4::gun_state_val() {
    auto* g = get_gun();

    if (g == nullptr) {
        return std::nullopt;
    }

    return call_enum(g, "get_CurrentState");
}

// [EMPTY] Sieht die ENGINE die Waffe als leer? Zuverlaessig, unabhaengig vom
// kaputten Ammo-Zaehler.
bool RE4VRReload4::gun_is_empty() {
    const auto v = gun_state_val();

    return v.has_value() && m_ammoempty_num.has_value() && *v == *m_ammoempty_num;
}

// Gun aus AmmoEmpty holen (= "Patrone gechambert").
void RE4VRReload4::gun_chamber() {
    auto* g = get_gun();

    if (g == nullptr || !m_gun_state_holding.has_value()) {
        return;
    }

    re4vr::call_safe<void*>(g, "set_CurrentState", *m_gun_state_holding);
}

// ============================================================================
// Haptik (Lua Z.4078-4102)
// ============================================================================
void RE4VRReload4::rack_haptic(float amp, float dur) {
    if (!cfg.rack_haptic) {
        return;
    }

    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return;
    }

    // linke Hand rackt
    vr->trigger_haptic_vibration(0.0f, dur, 169.385f, amp, vr->get_left_joystick());
}

// [SHOTGUN PUMP] Haptik mit optionalem Delay -> aufs Audio legbar.
void RE4VRReload4::queue_haptic(float amp, float dur) {
    const float d = cfg.pump_haptic_delay;

    if (d <= 0.0f) {
        rack_haptic(amp, dur);

        return;
    }

    m_haptic_queue.push_back(Haptic{clock_now() + d, amp, dur});
}

void RE4VRReload4::service_haptics() {
    if (m_haptic_queue.empty()) {
        return;
    }

    const double now = clock_now();

    for (size_t i = 0; i < m_haptic_queue.size();) {
        if (now >= m_haptic_queue[i].at) {
            rack_haptic(m_haptic_queue[i].amp, m_haptic_queue[i].dur);
            m_haptic_queue.erase(m_haptic_queue.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// ============================================================================
// [ROTARY_DEFER] Shotgun-Ammo-Add (Lua Z.4108-4179)
// ============================================================================
void RE4VRReload4::rotary_do_load() {
    if (!cfg.reload_ammo) {
        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);
    auto* wi = get_live_weapon_item();
    auto* p = pe();
    auto* inv = (p != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController") : nullptr;

    const auto ammo_id = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
    int32_t reserve = 0;

    if (inv != nullptr && ammo_id.has_value()) {
        reserve = item_count_sum(inv, *ammo_id);
    }

    const int32_t loaded = (wi != nullptr)
        ? call_enum(wi, "get_CurrentAmmoCount").value_or(0) : 0;
    const int32_t cap = (wi != nullptr)
        ? call_enum(wi, "get_CurrentAmmoMax").value_or(0) : 0;
    const int32_t ratio = std::max(1, shotgun_ratio_get(wid));
    const int32_t add = std::min({ratio, std::max(0, cap - loaded), reserve});

    if (add <= 0) {
        return;
    }

    // [ACCESSOR] echte Instanz zuerst; der Hook-Cache kann eine Leiche sein,
    // `wi` eine Kopie.
    ::REManagedObject* awi = real_wi();

    if (awi == nullptr) {
        awi = re4vr::obj_ok(m_live_wi) ? m_live_wi : wi;
    }

    const int32_t b4 = (awi != nullptr)
        ? call_enum(awi, "get_CurrentAmmoCount").value_or(loaded) : loaded;

    // [KEIN ABZUG OHNE LADUNG] Der Ladestand des Item-Objekts ist KEIN Beweis,
    // dass die Waffe geladen hat -- beim Skull Shaker steigt er, waehrend die
    // HUD auf 0 stehenbleibt. Erfolg wird an der HUD gemessen.
    const int32_t hud_b4 = gun_ammo().value_or(0);
    const int32_t r_b4 = reserve;
    const auto et = get_equip_type_main();

    bool did = false;
    const auto ewid_now = get_equip_wid();

    if (et.has_value() && inv != nullptr && ewid_now.value_or(-1) >= 0
        && ewid_now.has_value() && m_wep.wid.has_value() && *ewid_now == *m_wep.wid) {
        // [NUR WER EINEN EIGENEN PFAD HAT] Dem Helfer sagen, dass HIER noch ein
        // eigener Nachschlag kommt, wenn er false meldet.
        re4vr::lua_set_bool("__re4_caller_has_fallback", true);
        did = load_and_book(inv, *et, add, false);
        re4vr::lua_set_nil("__re4_caller_has_fallback");
    }

    int32_t af = (awi != nullptr)
        ? call_enum(awi, "get_CurrentAmmoCount").value_or(b4) : b4;

    // Die beiden addAmmoCount-Nachschlaege sind das Sicherheitsnetz: sie laufen
    // NUR, wenn der Helfer nicht geladen hat UND das Item sich nicht bewegt hat.
    if (!did && af <= b4 && awi != nullptr) {
        re4vr::call_safe<void*>(awi, "addAmmoCount", add, true);
        sync_gun_ammo();
        af = call_enum(awi, "get_CurrentAmmoCount").value_or(b4);
    }

    if (!did && af <= b4 && awi != nullptr) {
        re4vr::call_safe<void*>(awi, "addAmmoCount", add, false);
        sync_gun_ammo();
        af = call_enum(awi, "get_CurrentAmmoCount").value_or(b4);
    }

    // Erfolg = was in der WAFFE angekommen ist (HUD), nicht was am Item steht.
    const int32_t hud_af = gun_ammo().value_or(hud_b4);
    const int32_t gained = std::max(0, hud_af - hud_b4);

    if (gained <= 0) {
        return;   // nichts in der Waffe angekommen -> nichts abbuchen
    }

    if (inv != nullptr && ammo_id.has_value()) {
        // Reserve nur manuell ziehen, wenn der genutzte Pfad sie NICHT selbst zog.
        if (item_count_sum(inv, *ammo_id) >= r_b4) {
            safe_reduce(inv, *ammo_id, gained);
        }
    }
}

// ============================================================================
// Rack-Zustand (Lua Z.4181-4374)
// ============================================================================

void RE4VRReload4::clear_rack() {
    const int32_t wid = m_wep.wid.value_or(0);

    // [EMPTY-RELOAD SLIDE] War es der _08-Lade-Slide? -> _08 nach vorn schnappen
    // + Modus beenden. Vor dem _01-Handling, damit es unabhaengig von
    // ENGINE_CLOSES_SLIDE laeuft.
    const bool was_er = m_rack.empty_reload;
    m_rack.empty_reload = false;

    if (was_er && m_wep.slide_joint2 != nullptr) {
        glm::vec3 cur{};

        if (get_vec3(m_wep.slide_joint2, "get_LocalPosition", cur)) {
            set_vec3(m_wep.slide_joint2, "set_LocalPosition",
                     glm::vec3{cur.x, cur.y, slide_pose2(wid).rest_z});
        }
    }

    m_rack.needs = false;
    m_rack.grab_active = false;
    m_rack.pulled = false;
    m_rack.pushed = false;
    m_rack.frac = 0.0f;

    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    re4vr::lua_set_string("__re4_bf_who", "reload4/clear_rack");
    re4vr::lua_set_bool("__vr_rack_block_left_knife", false);

    // [GUN_STATE] Engine aus AmmoEmpty holen -> rack.empty wird false.
    gun_chamber();

    // [ENGINE_CLOSES_SLIDE] Bei diesen Waffen schliesst die Engine den Slide
    // selbst -> wir forcen NICHTS.
    if (engine_closes_slide(wid)) {
        m_rack._chambered_hold = false;
        m_rack._prev_gun_ammo.reset();

        return;
    }

    if (rotary_cycle(wid)) {   // joint_01 dreht, kein Z -> kein Position-Snap
        m_rack._chambered_hold = false;
        m_rack._prev_gun_ammo.reset();

        return;
    }

    // [CHAMBER_HOLD] Slide ab jetzt auf ZU halten (die Engine spielt die
    // Schliess-Anim nach unserem manuellen Reload nicht).
    m_rack._chambered_hold = true;
    m_rack._prev_gun_ammo.reset();

    if (m_wep.slide_joint != nullptr) {
        glm::vec3 cur{};

        if (get_vec3(m_wep.slide_joint, "get_LocalPosition", cur)) {
            set_vec3(m_wep.slide_joint, "set_LocalPosition",
                     glm::vec3{cur.x, cur.y, slide_pose(wid).rest_z});
        }
    }
}

// [CUTSCENE] Verlassen wir das Gameplay, gilt jede Klappe als zu; beim ERSTEN
// Gameplay-Frame danach wird der Zustand einmal sauber geraeumt (Flanke, nicht
// nur waehrend der Sequenz -- in ihr laeuft diese Funktion womoeglich gar nicht).
bool RE4VRReload4::reset_open_after_cutscene() {
    const int gp = re4vr::lua_get_tribool("__re4_frame_is_gameplay");

    if (gp == 0) {
        m_gp_was_out = true;

        return true;
    }

    if (m_gp_was_out) {
        m_gp_was_out = false;
        m_break.open = false;
        m_break.prog = 0.0f;
        m_break._was_open = false;
        m_break._prev_b = false;
        m_chamber.open = false;
        m_chamber.blend = 0.0f;
        re4vr::lua_set_bool("__vr_break_open", false);
        re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    }

    return false;
}

void RE4VRReload4::update_rack_state() {
    if (reset_open_after_cutscene()) {
        re4vr::lua_set_bool("__vr_break_open", false);
        re4vr::lua_set_bool("__vr_block_fire_when_empty", false);

        return;
    }

    if (m_rack.tuning) {
        return;   // UI-Vorschau: Live-Logik aussetzen
    }

    const int32_t wid = m_wep.wid.value_or(0);

    // (Luas TOP_LOADER-Zweig ist unerreichbar -- die Tabelle ist leer.)

    if (!handled().has_value()) {
        // nur an "Manual Pistol Reload" gekoppelt, kein Extra-Toggle
        m_rack.empty = false;

        if (m_rack.needs || re4vr::lua_get_tribool("__vr_block_fire_when_empty") == 1) {
            clear_rack();
        }

        return;
    }

    const auto loaded = live_loaded_count();
    const bool flow_active = !mag_is_present();
    m_rack.has_mag = loaded.has_value() && *loaded > 0;

    // [EMPTY_SLIDE] Slide auf MITTEL SOFORT bei 0 Ammo -> ueber das game-eigene
    // isGunAmmoEmpty (greift auch im IgnoreClerMask-Zwischenstate).
    auto* p = pe();
    bool empty = false;

    if (p != nullptr) {
        re4vr::try_call<bool>(p, "isGunAmmoEmpty", empty);
    }

    m_rack.empty = empty;

    // [CHAMBER_HOLD] Schuss erkennen (echte Gun-Ammo SINKT) -> Nach-Rack-Halt
    // loesen, ab dann spielt die Engine die Slide-Anim selbst.
    if (m_rack._chambered_hold) {
        const auto ga = gun_ammo();

        if (chamber_hold_persist(wid)) {
            // [CHAMBER_HOLD_PERSIST] NICHT beim Schuss loesen -> Slide bleibt zu,
            // bis die Waffe leer ist.
            if (m_rack.empty) {
                m_rack._chambered_hold = false;
            }
        } else {
            if (ga.has_value() && m_rack._prev_gun_ammo.has_value()
                && *ga < *m_rack._prev_gun_ammo) {
                m_rack._chambered_hold = false;
            }
        }

        m_rack._prev_gun_ammo = ga;
    }

    // needs_rack wird hier NICHT abgeleitet (das war der Fehler). Es wird beim
    // Insert gelatcht und NUR durch die Rack-Geste geclear't -> sticky.
    // [SHOTGUN] Nach JEDEM Schuss ist ein Pump noetig -- aber nur, wenn danach
    // noch Munition in der Kammer ist. Hat der Schuss auf 0 geleert, waere der
    // Pump unmoeglich und wirkte wie ein Soft-Lock.
    if (is_shotgun(wid)) {
        const int32_t seq =
            static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

        if (m_rack._prev_shot_seq.has_value() && seq > *m_rack._prev_shot_seq
            && m_rack.has_mag && !no_cycle_after_shot(wid)) {
            m_rack.needs = true;
        }

        m_rack._prev_shot_seq = seq;
    }

    // [SHOTGUN] needs_rack pre-empted in motion die Support-Hand. Bei der
    // Shotgun soll die Hand im Pump-Fenster AUF dem Vordergriff bleiben ->
    // daher needs_rack hier NICHT melden.
    re4vr::lua_set_bool("__vr_needs_rack", m_rack.needs && !is_shotgun(wid));

    // [IK-GATE] Slide-Rack-Grab aktiv -> motion sperrt die Two-Hand-IK.
    re4vr::lua_set_bool("__vr_slide_rack_active", m_rack.grab_active);
    re4vr::lua_set_bool("__vr_shotgun_pump_active", m_rack.grab_active && is_shotgun(wid));

    if (break_action(wid)) {
        // [SKULL SHAKER] Block/Dry-Fire wenn leer ODER Cock aussteht ODER Hebel
        // OFFEN. rack.empty ist jeden Frame frisch, KEIN Cache.
        re4vr::lua_set_bool("__vr_block_fire_when_empty",
                            m_rack.empty || m_rack.needs
                            || re4vr::lua_get_tribool("__vr_break_open") == 1);
        re4vr::lua_set_string("__re4_bf_who", "reload4/dryfire+break-open");
    } else if (dryfire_only_when_empty(wid)) {
        // [STRIKER] Block wenn leer ODER ein Cycle aussteht. KEIN
        // flow_active/mag_out-Block (Drum = Fehl-Block).
        re4vr::lua_set_bool("__vr_block_fire_when_empty", m_rack.empty || m_rack.needs);
        re4vr::lua_set_string("__re4_bf_who", "reload4/dryfire nur wenn leer");
    } else {
        // [LIVE-EMPTY] Grundregel: die Engine entscheidet ueber "kann nicht
        // feuern" via rack.empty. [RACK-ZWANG] rack.needs FEHLTE hier frueher --
        // der native Reload chambert die Waffe, der Block fiel weg und man
        // konnte OHNE Slide-Rack weiterfeuern.
        re4vr::lua_set_bool("__vr_block_fire_when_empty",
                            m_rack.empty || m_rack.needs || m_mag_out || flow_active
                            || (no_reload_cycle(wid) && m_rack.empty_reload));
        re4vr::lua_set_string("__re4_bf_who", "reload4/standard (rack+mag+flow)");
    }

    // linker Grip frei fuer den Slide-Grab (Messer aus)
    // [SKULL SHAKER] Die Ausnahme fuer 6001 steht in RE4VRReloadMain --
    // dort gehoert die Waffe hin (break_action(6001)). Hier liefert
    // break_action() immer false, die Zeile haette nie gegriffen.
    re4vr::lua_set_bool("__vr_rack_block_left_knife", m_rack.needs);
}

// ============================================================================
// Eingaben (Lua Z.4379-4431)
// ============================================================================
// [STALE_HANDLE_FIX] Action-Handle UND Joystick JEDEN Frame frisch holen: nach
// Save-Load/Szenenwechsel geht ein gecachtes Handle stale -> is_action_active
// liefert lautlos false -> der Grab ist tot bis zum Reset.

bool RE4VRReload4::left_grip_down() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto act = vr->get_action_grip();
    const auto lj = vr->get_left_joystick();

    if (act == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active(act, lj);
    } catch (...) {
        return false;
    }
}

// [ROTARY_CYCLE] Linker Trigger (weapon_dial-Action, wie motion).
bool RE4VRReload4::left_trigger_down() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto act = vr->get_action_weapon_dial();
    const auto lj = vr->get_left_joystick();

    if (act == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active(act, lj);
    } catch (...) {
        return false;
    }
}

// [BREAK_ACTION] Pitch des LINKEN Controllers in Grad (forward.y-Winkel).
std::optional<float> RE4VRReload4::left_ctrl_pitch_deg() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return std::nullopt;
    }

    // Lua: vrmod:get_controllers()[1] -- das erste Element ist der LINKE.
    const auto& cs = vr->get_controllers();

    if (cs.empty()) {
        return std::nullopt;
    }

    try {
        const glm::quat q{vr->get_rotation(cs[0])};
        const glm::vec3 f = q * glm::vec3{0.0f, 0.0f, 1.0f};
        const float y = std::clamp(f.y, -1.0f, 1.0f);

        return glm::degrees(std::asin(y));
    } catch (...) {
        return std::nullopt;
    }
}

// Linke CONTROLLER-Welt-Position. WICHTIG: fuer Grab-Distanz UND Zug den
// Controller nehmen, NICHT den L_Hand-Bone -- der wird beim Dock an den Slide
// gepinnt, sonst waere die Zug-Bewegung immer null.
std::optional<glm::vec3> RE4VRReload4::get_left_controller_pos() {
    auto& vr = VR::get();

    if (vr == nullptr || !vr->is_hmd_active()) {
        return std::nullopt;
    }

    const auto& cs = vr->get_controllers();

    if (cs.empty()) {
        return std::nullopt;
    }

    try {
        const auto p = vr->get_position(cs[0]);

        return glm::vec3{p.x, p.y, p.z};
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Rack-Geste (Lua Z.4436-4751)
// ============================================================================
// Linke Hand am Slide + LINKER GRIP greift -> Grip HALTEN und ganz nach hinten
// ziehen (Slide folgt via frac) -> Grip LOSLASSEN = fertig. "armed": der Grip
// muss einmal losgelassen worden sein, bevor neu gegriffen werden kann.
void RE4VRReload4::update_rack_gesture() {
    if (m_rack.tuning) {
        return;   // UI-Vorschau: Geste aussetzen
    }

    const int32_t wid = m_wep.wid.value_or(0);

    // [STAGGER-FIX] Riot Gun: nach einem Stagger mit ausstehendem Rack ging
    // weder Rack noch Schuss. Der Stagger raeumt `empty_reload` weg (`needs`
    // bleibt sticky), und damit greift der Shotgun-Pull-to-Pump-Zweig am _01
    // statt des Release-arm-Pfads am _08. Der Modus laesst sich rekonstruieren:
    // bei der Riot Gun wird `needs` AUSSCHLIESSLICH ueber `empty_reload` gesetzt.
    if (no_reload_cycle(wid) && m_rack.needs && !m_rack.empty_reload) {
        m_rack.empty_reload = true;
        re4vr::lua_set_bool("__vr_empty_reload_active", true);
    }

    if (rotary_cycle(wid) || break_action(wid)) {
        return;   // eigener Pfad, kein Pull-Rack
    }

    // [SHOTGUN PUMP-FENSTER] Pump NUR wenn rack.needs. AUSSERHALB des Fensters
    // rackt die Shotgun NICHT -> der linke Grip am Schaft ist frei fuer die
    // Two-Hand-IK. Gilt fuer ALLE Waffen gleich.
    if (!m_rack.needs) {
        m_rack.grab_active = false;
        m_rack.pulled = false;
        m_rack.pushed = false;
        m_rack.frac = 0.0f;
        m_rack.armed = false;
        m_rack._pump_ref_z.reset();
        m_rack._last_grip = false;
        m_rack._last_dist = -1.0f;

        return;
    }

    if (m_wep.slide_joint == nullptr
        || (m_rack.empty_reload && m_wep.slide_joint2 == nullptr)) {
        refresh_weapon();
    }

    auto* sj = rack_joint();

    // Linke Hand-WELT-Ziel (motion, Controller-getrieben) fuer Grab-Distanz +
    // Zug. NICHT der L_Hand-Bone (wird beim Dock an den Slide gepinnt -> Zug=0)
    // und NICHT der vrmod-Controller (anderer Koordinatenraum -> dist=129).
    // [SHOTGUN] rohe un-gedockte Hand: noetig fuer den nahtlosen Pull-to-Pump
    // aus der Two-Hand-Haltung.
    std::optional<glm::vec3> hp{};

    if (is_shotgun(wid)) {
        hp = re4vr::lua_get_vec3("__vr_lh_ctrl_world");
    }

    if (!hp.has_value()) { hp = re4vr::lua_get_vec3("__vr_lh_world"); }
    if (!hp.has_value()) { hp = re4vr::lua_get_vec3("__vr_unified_lh_pos"); }
    if (!hp.has_value()) { hp = re4vr::lua_get_vec3("__vr_lh_joint_pos"); }

    if (!hp.has_value()) {
        auto* lh = get_left_hand();
        glm::vec3 p{};

        if (lh != nullptr && get_vec3(lh, "get_Position", p)) {
            hp = p;
        }
    }

    std::optional<glm::vec3> sp{};

    if (sj != nullptr) {
        glm::vec3 p{};

        if (get_vec3(sj, "get_Position", p)) {
            sp = p;
        }
    }

    const bool grip = left_grip_down();
    m_rack._last_grip = grip;
    m_rack._last_dist = (hp.has_value() && sp.has_value())
        ? vec_len(vec_sub(*hp, *sp)) : -1.0f;

    if (sj == nullptr || !hp.has_value() || !sp.has_value()) {
        return;
    }

    if (!m_rack.grab_active) {
        // [RACK-LOCK] Kein Grab, solange eine Patrone in der Hand ist ODER
        // gerade in die Kammer gleitet. Waehrend eine Patrone einfaehrt, darf
        // nie gerackt werden -- sonst startet die Geste MITTEN im Insert und
        // zieht den Slide auf back_z.
        if (m_mag_hand.active || m_mag_insert.active) {
            m_rack.armed = false;
            m_rack._pump_ref_z.reset();

            return;
        }

        // Shotgun NICHT nach-sperren -> direkt nach dem Shell-Einlegen soll man
        // pumpen koennen.
        if (!is_shotgun(wid) && m_rack._ammo_input_t.has_value()
            && (clock_now() - *m_rack._ammo_input_t) < 0.2) {
            m_rack.armed = false;

            return;
        }

        // [RIOT GUN KEIN PUMP] Die Riot Gun ist semi-auto: ihr Rack sitzt
        // SEITLICH (_08), gepumpt wird bei ihr gar nicht mehr. Der
        // Pull-to-Pump am Vorderschaft war die Ursache des Stagger-Deadlocks --
        // er greift jetzt nur noch bei der W-870.
        if (is_shotgun(wid) && !m_rack.empty_reload && !no_reload_cycle(wid)) {
            // [SHOTGUN PULL-TO-PUMP] Nahtlos aus der Two-Hand-Haltung: KEIN
            // Grip-Release noetig. Erst der ZUG startet den Pump.
            if (grip && m_rack._last_dist >= 0.0f
                && m_rack._last_dist <= cfg.rack_grab_dist) {
                glm::quat srot{};
                const bool has_srot = get_quat(sj, "get_Rotation", srot);
                float cz = 0.0f;

                if (has_srot) {
                    cz = (glm::conjugate(srot) * vec_sub(*hp, *sp)).z;
                }

                // [RE9_PUMP] Referenz = ABSTAND DER BEIDEN HAENDE (1:1 aus dem
                // RE9-Mod). Kein Waffenbezug -> keine Rueckkopplung mit
                // PUMP_NO_Z in motion; kein Weltanker -> Laufen zaehlt nicht
                // mit; richtungslos -> die Controller-Richtung ist egal.
                const auto rhp = re4vr::lua_get_vec3("__vr_rh_world").has_value()
                    ? re4vr::lua_get_vec3("__vr_rh_world")
                    : re4vr::lua_get_vec3("__vr_unified_rh_pos");

                std::optional<float> dist_now{};

                if (rhp.has_value()) {
                    dist_now = vec_len(vec_sub(*hp, *rhp));
                }

                if (dist_now.has_value() && !m_rack._pump_init_dist.has_value()) {
                    m_rack._pump_init_dist = *dist_now;
                }

                float pull = 0.0f;

                if (dist_now.has_value() && m_rack._pump_init_dist.has_value()) {
                    pull = *m_rack._pump_init_dist - *dist_now;

                    // Selbstnachziehend: solange nicht gezogen wurde und der
                    // Abstand WAECHST, wird die Referenz nachgefuehrt.
                    if (pull < 0.0f) {
                        m_rack._pump_init_dist = *dist_now;
                        pull = 0.0f;
                    }
                }

                if (pull > cfg.pump_start_pull) {
                    m_rack.grab_active = true;
                    m_rack.armed = false;
                    m_rack.pulled = false;
                    m_rack.pushed = false;
                    m_rack.frac = 0.0f;
                    m_rack.gx = hp->x;
                    m_rack.gy = hp->y;
                    m_rack.gz = hp->z;
                    m_rack.g_relz = cz;

                    // [RE9_PUMP] Handabstand JETZT als Nullpunkt einfrieren.
                    m_rack._pump_init_dist = dist_now;
                    m_rack._pump_max = 0.0f;
                    m_rack._pump_ref_z.reset();
                    rack_haptic(0.25f, 0.03f);
                    m_rack.pump_off.reset();

                    if (const auto shp = re4vr::lua_get_vec3("__vr_support_hand_world_pos");
                        shp.has_value()) {
                        const glm::vec3 d = vec_sub(*shp, *sp);
                        m_rack.pump_off = has_srot ? (glm::conjugate(srot) * d) : d;
                    }
                }
            } else {
                m_rack._pump_ref_z.reset();   // Hand weg / Grip los -> Reset
            }

            return;
        }

        // Nicht-Shotgun ODER Shotgun-Empty-Reload (_08): Release-arm +
        // Proximity-Grab, pistolen-artig. Grip einmal loslassen = scharf.
        if (!grip) {
            m_rack.armed = true;

            return;
        }

        if (!m_rack.armed) {
            return;
        }

        if (m_rack._last_dist >= 0.0f && m_rack._last_dist <= cfg.rack_grab_dist) {
            m_rack.grab_active = true;
            m_rack.armed = false;
            m_rack.pulled = false;
            m_rack.pushed = false;
            m_rack.frac = 0.0f;

            // [ROHER CONTROLLER] Anker aus der UNGEDOCKTEN Hand. `hp` ist die
            // GEDOCKTE Hand -- sobald der Rack-Dock greift, ist sie unsere
            // eigene Vorgabe und der Zug wuerde sich selbst messen.
            const auto hpr = re4vr::lua_get_vec3("__vr_lh_ctrl_world").value_or(*hp);
            m_rack.gx = hpr.x;
            m_rack.gy = hpr.y;
            m_rack.gz = hpr.z;

            // [LAUFEN] Zweiter Anker: die WAFFENHAND. Der Zug wurde bisher gegen
            // einen festen WELTpunkt gemessen -- beim Gehen wandern beide Haende
            // mit dem Koerper, und diese Fortbewegung landete komplett im Zug.
            auto rhr = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

            if (!rhr.has_value()) { rhr = re4vr::lua_get_vec3("__vr_rh_world"); }
            if (!rhr.has_value()) { rhr = re4vr::lua_get_vec3("__vr_unified_rh_pos"); }

            if (rhr.has_value()) {
                m_rack.rgx = rhr->x;
                m_rack.rgy = rhr->y;
                m_rack.rgz = rhr->z;
            } else {
                m_rack.rgx.reset();
                m_rack.rgy.reset();
                m_rack.rgz.reset();
            }

            glm::quat srot{};
            std::optional<glm::vec3> rel{};

            if (get_quat(sj, "get_Rotation", srot)) {
                rel = glm::conjugate(srot) * vec_sub(*hp, *sp);
            }

            m_rack.g_relz = rel.has_value() ? std::optional<float>{rel->z} : std::nullopt;

            // [RACK_POSE_ANGLE] Aus welcher Richtung ist die Hand gekommen?
            // 0 Grad = genau von HINTEN, 90 Grad = genau von der SEITE.
            // Seitenneutral ueber |x|, Hoehe bewusst ignoriert. EINMAL beim
            // Greifen bestimmt -- sonst springt die Pose waehrend des Zugs.
            // [POSE2_OFFSETS] Im Einstellmodus NICHT neu bestimmen.
            if (!m_rack.dock_tune) {
                m_rack.pose_side = false;
            }

            // [RACK_BACK_ONLY] Die Killer7 (4501) hat nur EINE Art zu racken ->
            // Winkel gar nicht erst auswerten.
            if (rel.has_value() && !m_rack.dock_tune && wid != 4501) {
                const float ang = glm::degrees(std::atan2(std::abs(rel->x), -rel->z));
                m_rack.pose_side = ang >= cfg.rack_pose_side_deg;
                m_rack._pose_ang = ang;
            }

            rack_haptic(0.25f, 0.03f);
            m_rack.pump_off.reset();
        }

        return;
    }

    // gegriffen + Grip gehalten -> Slide folgt dem RUECKWAERTS-Zug.
    if (grip) {
        const auto& sp_pose = rack_slide_pose();
        const float travel = std::max(std::abs(sp_pose.back_z - sp_pose.park_z), 0.005f);
        float pull = 0.0f;

        if (is_shotgun(wid) && m_rack._pump_init_dist.has_value()) {
            // [RE9_PUMP] Zug = Annaeherung der beiden HAENDE. Immun gegen
            // PUMP_NO_Z/X, gegen Laufen und gegen die Controller-Richtung.
            auto rhp = re4vr::lua_get_vec3("__vr_rh_world");

            if (!rhp.has_value()) { rhp = re4vr::lua_get_vec3("__vr_unified_rh_pos"); }

            const auto hpr = re4vr::lua_get_vec3("__vr_lh_ctrl_world").value_or(*hp);

            if (rhp.has_value()) {
                const float dist_now = vec_len(vec_sub(hpr, *rhp));
                pull = *m_rack._pump_init_dist - dist_now;

                // Solange nicht durchgezogen: waechst der Abstand, zieht die
                // Referenz nach (ein Nachgreifen blockiert den Zyklus nicht).
                if (!m_rack.pulled && pull < 0.0f) {
                    m_rack._pump_init_dist = dist_now;
                    pull = 0.0f;
                    m_rack._pump_max = 0.0f;
                }

                if (pull < 0.0f) {
                    pull = 0.0f;
                }

                if (m_rack._pump_max < pull) {
                    m_rack._pump_max = pull;
                }
            }
        } else if (is_shotgun(wid)) {
            // FALLBACK (kein Handabstand messbar): gun-relativ gegen den
            // Slide-Frame.
            glm::quat srot{};
            glm::vec3 spos{};
            std::optional<float> cz{};

            if (get_quat(sj, "get_Rotation", srot) && get_vec3(sj, "get_Position", spos)) {
                cz = (glm::conjugate(srot) * vec_sub(*hp, spos)).z;
            }

            if (cz.has_value() && m_rack.g_relz.has_value()) {
                pull = *m_rack.g_relz - *cz;   // nach hinten -> cz sinkt

                if (pull < 0.0f) {
                    pull = 0.0f;
                }
            }
        } else {
            // Nicht-Shotgun: Welt-Projektion der Controller-Verschiebung auf die
            // Slide-Rueckwaerts-Achse.
            const auto hpr = re4vr::lua_get_vec3("__vr_lh_ctrl_world").value_or(*hp);
            float px = hpr.x - m_rack.gx;
            float py = hpr.y - m_rack.gy;
            float pz = hpr.z - m_rack.gz;

            // [LAUFEN] Die Bewegung der WAFFENHAND abziehen; uebrig bleibt die
            // Bewegung der Ziehhand GEGEN die Waffe -- mit erhaltener Richtung,
            // damit die eingestellten travel-Werte weiter gelten.
            if (m_rack.rgx.has_value() && m_rack_relative) {
                auto rhn = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");

                if (!rhn.has_value()) { rhn = re4vr::lua_get_vec3("__vr_rh_world"); }
                if (!rhn.has_value()) { rhn = re4vr::lua_get_vec3("__vr_unified_rh_pos"); }

                if (rhn.has_value()) {
                    px -= (rhn->x - *m_rack.rgx);
                    py -= (rhn->y - *m_rack.rgy);
                    pz -= (rhn->z - *m_rack.rgz);
                }
            }

            glm::quat srot{};

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
        }

        const float f = pull / travel;
        m_rack.frac = (f > 1.0f) ? 1.0f : f;

        if (m_rack.frac >= 1.0f && !m_rack.pulled) {
            m_rack.pulled = true;
            // [SND] Slide-Sound beim Zurueckziehen. Pistolen: der 2. Sound kommt
            // beim Loslassen/Snap; Shotgun beim Nach-vorn-Schieben.
            play_weapon_sound(snd_id(wid, "slide_back"));

            if (is_shotgun(wid)) {
                queue_haptic(0.95f, 0.07f);
            }
        }

        // [SHOTGUN PUMP] RE9-Modell: nach vollem Zug wieder nach VORN geschoben
        // -> HER-Sound + Zyklus fertig, OHNE Loslassen. Das Vorschieben wird
        // RELATIV zum weitesten Punkt gemessen (frac <= 0.15 waere bei 8,7 cm
        // Travel nur ~1,3 cm Toleranz -- praktisch nicht treffbar).
        const float back = m_rack._pump_max - pull;
        const float back_need = std::max(0.03f, travel * cfg.pump_push_frac);

        if (is_shotgun(wid) && !m_rack.empty_reload && m_rack.pulled && !m_rack.pushed
            && (back >= back_need || m_rack.frac <= 0.15f)) {
            m_rack.pushed = true;
            play_weapon_sound(snd_id(wid, "slide_back"));   // HER
            queue_haptic(0.95f, 0.07f);

            if (m_rack.needs) {
                clear_rack();
            }

            // Grab BEHALTEN (clear_rack setzt es false) + Zyklus zuruecksetzen
            // -> ohne Loslassen direkt weiterpumpen.
            m_rack.grab_active = true;
            m_rack.pulled = false;
            m_rack.pushed = false;
            m_rack._pump_max = 0.0f;
            m_rack._pump_init_dist.reset();
        }

        return;
    }

    // Grip losgelassen
    if (m_rack.pulled && !m_rack.pushed) {
        // voll gezogen, aber nicht nach vorn gepumpt (Shotgun) bzw. Pistole
        // losgelassen -> Snap nach vorn = HER-Sound.
        clear_rack();
        rack_haptic(0.95f, 0.07f);
        play_weapon_sound(snd_id(wid, "slide_back"));
    } else {
        m_rack.grab_active = false;
        m_rack.frac = 0.0f;
        m_rack.pulled = false;
        m_rack.pushed = false;
        m_rack.armed = true;
    }
}

// ============================================================================
// [ROTARY_CYCLE] Striker-Drehschalter (Lua Z.4756-4803)
// ============================================================================
void RE4VRReload4::update_rotary_cycle() {
    const int32_t wid = m_wep.wid.value_or(0);

    if (!rotary_cycle(wid)) {
        re4vr::lua_set_bool("__vr_block_two_hand", false);
        m_rotary._grip_latched = false;
        m_rotary._prev_grip = false;

        return;
    }

    if (m_rotary.preview) {
        // UI-Tuning: Hand am Schalter, Two-Hand blocken
        m_rack.grab_active = true;
        re4vr::lua_set_bool("__vr_block_two_hand", true);

        return;
    }

    const bool grip = left_grip_down();
    const bool trig = left_trigger_down();

    // [SWITCH-GRIP-LATCH] Eine Grip-FLANKE waehrend ein Cycle noetig ist gehoert
    // dem Schalter -> diese Grip-Haltung blockt die Two-Hand-IK (Schalter und
    // Vordergriff liegen zu nah). Erst Loslassen + erneut Greifen erlaubt sie.
    if (grip && !m_rotary._prev_grip && m_rack.needs) {
        m_rotary._grip_latched = true;
    }

    if (!grip) {
        m_rotary._grip_latched = false;
    }

    m_rotary._prev_grip = grip;
    re4vr::lua_set_bool("__vr_block_two_hand", m_rotary._grip_latched);

    // Distanz Hand -> Drehschalter (rohe un-gedockte Hand)
    const auto& r = rotary_cfg(wid);
    auto hp = re4vr::lua_get_vec3("__vr_lh_world");

    if (!hp.has_value()) { hp = re4vr::lua_get_vec3("__vr_unified_lh_pos"); }
    if (!hp.has_value()) { hp = re4vr::lua_get_vec3("__vr_lh_joint_pos"); }

    std::optional<glm::vec3> sp{};

    if (m_wep.slide_joint != nullptr) {
        glm::vec3 p{};

        if (get_vec3(m_wep.slide_joint, "get_Position", p)) {
            sp = p;
        }
    }

    m_rotary._last_dist = (hp.has_value() && sp.has_value())
        ? vec_len(vec_sub(*hp, *sp)) : -1.0f;

    // "near" ist ein windows.h-Makro -> eigener Name.
    const bool is_near = (m_rotary._last_dist >= 0.0f) && (m_rotary._last_dist <= r.grab_dist);
    const bool at_switch = m_rack.needs && grip && is_near;

    if (m_rotary.dir == 0) {
        m_rotary.prog = 0.0f;   // [IDLE] sonst haengt es vom UI-Preview fest
    }

    // Trigger-FLANKE am Schalter -> eine Drehung starten
    if (at_switch && trig && !m_rotary._prev_trig && m_rotary.dir == 0
        && m_rotary.prog <= 0.0001f) {
        m_rotary.dir = 1;
        rack_haptic(0.30f, 0.04f);
        play_weapon_sound(snd_id(wid, "cycle"));
    }

    m_rotary._prev_trig = trig;

    const float spd = r.lerp;

    if (m_rotary.dir == 1) {
        m_rotary.prog = std::min(1.0f, m_rotary.prog + spd);

        if (m_rotary.prog >= 1.0f) {
            // [ROTARY_DEFER] Ammo kommt ERST hier, am Dreh-Peak.
            if (m_rotary.pending) {
                rotary_do_load();
                m_rotary.pending = false;
            }

            clear_rack();          // chambert + rack.needs weg
            m_rotary.dir = -1;     // zurueckdrehen
        }
    } else if (m_rotary.dir == -1) {
        m_rotary.prog = std::max(0.0f, m_rotary.prog - spd);

        if (m_rotary.prog <= 0.0f) {
            m_rotary.dir = 0;
        }
    }

    // Hand am Schalter halten, solange am Schalter ODER waehrend der Drehung
    m_rack.grab_active = at_switch || m_rotary.dir != 0;

    // [ROTARY_LEXIT] Fallende Flanke des Schalter-Docks -> denselben weichen
    // Links-Ausfade triggern wie beim Mag-Push.
    if (m_rotary._was_grab && !m_rack.grab_active) {
        re4vr::lua_set_number("__re4_reload_lexit_t", clock_now());
    }

    m_rotary._was_grab = m_rack.grab_active;
}

// ============================================================================
// [BREAK_ACTION] Skull-Shaker-Klapphebel (Lua Z.4808-4944)
// ============================================================================
void RE4VRReload4::update_break_action() {
    if (reset_open_after_cutscene()) {
        re4vr::lua_set_bool("__vr_break_open", false);
        re4vr::lua_set_bool("__vr_block_fire_when_empty", false);

        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);

    if (!break_action(wid)) {
        re4vr::lua_set_bool("__vr_break_open", false);
        m_break.open = false;
        m_break._was_open = false;

        return;
    }

    if (m_break.preview) {
        re4vr::lua_set_bool("__vr_break_open", m_break.prog > 0.15f);

        return;   // UI-Tuning: der Slider treibt prog
    }

    const auto& r = rotary_cfg(wid);

    // RIGHT-B-FLANKE togglet auf/zu
    const bool b = right_b_down();

    if (b && !m_break._prev_b) {
        m_break.open = !m_break.open;
        play_weapon_sound(snd_id(wid, "break_open"));
    }

    m_break._prev_b = b;

    const float target = m_break.open ? 1.0f : 0.0f;
    const float spd = r.lerp;

    if (m_break.prog < target) {
        m_break.prog = std::min(target, m_break.prog + spd);
    } else if (m_break.prog > target) {
        m_break.prog = std::max(target, m_break.prog - spd);

        if (m_break.prog <= 0.0f && m_break._was_open) {
            m_break._was_open = false;

            if (m_rack.needs) {
                clear_rack();   // voll zu = cocken/chambern
            }
        }
    }

    if (m_break.open) {
        m_break._was_open = true;
    }

    re4vr::lua_set_bool("__vr_break_open", m_break.open || m_break.prog > 0.15f);
}

// [BREAK_PUMP_SND] Cock-Fenster: __vr_pump_anim_active + _progress treiben den
// Dreh-Loop und die Cock-Pose in motion, dazu zwei Rack-Sounds.
void RE4VRReload4::update_break_pump_sound() {
    const int32_t wid = m_wep.wid.value_or(0);

    if (!break_action(wid)) {
        m_pumpsnd = PumpSnd{};
        re4vr::lua_set_bool("__vr_pump_anim_active", false);
        re4vr::lua_set_nil("__vr_pump_anim_progress");

        return;
    }

    const double now = clock_now();

    // Schuss-Flanke. Init OHNE Trigger (kein Geister-Cock beim Equip, wenn der
    // globale seq von anderen Waffen schon hochgezaehlt war).
    const int32_t seq = static_cast<int32_t>(re4vr::lua_get_number("__vr_shot_seq", 0.0));

    if (!m_pumpsnd.prev_seq.has_value()) {
        m_pumpsnd.prev_seq = seq;
    } else if (seq > *m_pumpsnd.prev_seq) {
        // [SKULL_FLICK] Der Schuss startet das Cock-Fenster NICHT mehr -- nur
        // noch die Flanke merken. Ausgeloest wird ausschliesslich per Flick.
        m_pumpsnd.prev_seq = seq;
    }

    float spin_dur = cfg.skull_spin_dur;

    if (spin_dur < 0.10f) {
        spin_dur = 0.10f;
    }

    if (m_pumpsnd.t0.has_value() && (now - *m_pumpsnd.t0) < spin_dur) {
        re4vr::lua_set_bool("__vr_pump_anim_active", true);
        re4vr::lua_set_number("__vr_pump_anim_progress", (now - *m_pumpsnd.t0) / spin_dur);
    } else {
        m_pumpsnd.t0.reset();
        re4vr::lua_set_bool("__vr_pump_anim_active", false);
        re4vr::lua_set_nil("__vr_pump_anim_progress");

        // [SKULL_FLICK] Drehung durch -> jetzt erst chambern.
        if (m_pumpsnd.cock_pending) {
            m_pumpsnd.cock_pending = false;

            if (m_rack.needs) {
                clear_rack();
            }
        }
    }

    // 2x Rack-Sound: der erste 0.1 s nach dem Flick, der zweite 0.42 s danach.
    if (m_pumpsnd.first_at.has_value() && now >= *m_pumpsnd.first_at) {
        play_weapon_sound(BREAK_PUMP_SND_ID);
        m_pumpsnd.first_at.reset();
        m_pumpsnd.second_at = now + 0.42;
    }

    if (m_pumpsnd.second_at.has_value() && now >= *m_pumpsnd.second_at) {
        play_weapon_sound(BREAK_PUMP_SND_ID);
        m_pumpsnd.second_at.reset();
    }
}

// [SKULL_FLICK] Wrist-Flick am RECHTEN Controller cockt den Skull Shaker.
// Messung 1:1 wie der Trommel-Flick in reload2: Pitch-Geschwindigkeit ueber
// forward.y, gezaehlt wird nur eine RICHTUNGSUMKEHR (Hin- + Rueckschlag =
// echter Ruck), damit langsames Kippen nicht ausloest.
void RE4VRReload4::break_cock_flick() {
    if (!break_action(m_wep.wid.value_or(0))) {
        return;
    }

    if (!m_rack.needs) {
        m_pumpsnd.pk_dir = 0;

        return;   // kein Cock offen
    }

    if (re4vr::lua_get_tribool("__vr_break_open") == 1) {
        m_pumpsnd.pk_dir = 0;

        return;   // Klappe auf = Nachladen
    }

    if (m_pumpsnd.t0.has_value()) {
        return;   // Drehung laeuft schon
    }

    const auto rot = re4vr::lua_get_quat("__vr_rh_rot");

    if (!rot.has_value()) {
        m_pumpsnd.fy.reset();

        return;
    }

    const glm::vec3 fwd = *rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const double now = clock_now();
    const float fy = fwd.y;

    // [FLICK-EMPFINDLICHKEIT] 8.0 verlangte ein hartes Zucken -> 5.0.
    constexpr float vmin = 5.0f;
    constexpr double rev = 0.30;

    if (m_pumpsnd.pk_dir != 0 && (now - m_pumpsnd.pk_t) > rev) {
        m_pumpsnd.pk_dir = 0;
    }

    if (m_pumpsnd.fy.has_value() && m_pumpsnd.fy_t.has_value()) {
        const double dt = now - *m_pumpsnd.fy_t;

        if (dt > 0.001 && dt < 0.2) {
            const float vel = static_cast<float>((fy - *m_pumpsnd.fy) / dt);

            if (std::abs(vel) > vmin) {
                const int dir = (vel > 0.0f) ? 1 : -1;

                if (m_pumpsnd.pk_dir != 0 && dir != m_pumpsnd.pk_dir
                    && (now - m_pumpsnd.last_flick) > 0.5) {
                    m_pumpsnd.t0 = now;              // Dreh-Fenster starten
                    m_pumpsnd.first_at = now + 0.1;  // 2x Rack-Sound
                    m_pumpsnd.second_at.reset();
                    m_pumpsnd.cock_pending = true;   // am Fensterende chambern
                    m_pumpsnd.last_flick = now;
                    m_pumpsnd.pk_dir = 0;
                } else {
                    m_pumpsnd.pk_dir = dir;          // Hinschlag merken
                    m_pumpsnd.pk_t = now;
                }
            }
        }
    }

    m_pumpsnd.fy = fy;
    m_pumpsnd.fy_t = now;
}

// ============================================================================
// IK-Dock der linken Hand (Lua Z.4946-5076)
// ============================================================================
// [DOCK_LERP] Blend EINMAL pro Frame rampen (NICHT in publish_dock -- das
// laeuft 5x/Frame und wuerde 5x hochzaehlen).
void RE4VRReload4::update_dock_blend() {
    const int32_t wid = m_wep.wid.value_or(0);
    const float want = ((m_rack.grab_active || m_rack.dock_tune)
                        && m_wep.slide_joint != nullptr) ? 1.0f : 0.0f;
    float b = m_rack.dock_blend;

    if (b < want) {
        b = std::min(b + DOCK_BLEND_SPEED, want);
    } else if (b > want) {
        // [PUMP-REDOCK] Shotguns: Slide-Dock beim LOESEN schneller runterrampen
        // -> die Support-Hand re-dockt frueher an den Vordergriff.
        float down = is_shotgun(wid) ? 0.30f : DOCK_BLEND_SPEED;

        // [ROTARY_LEXIT] Drehschalter-Waffen hart loesen; den weichen Weg zum
        // Controller macht der Lexit-Fade in motion.
        if (rotary_cycle(wid)) {
            down = 1.0f;
        }

        b = std::max(b - down, want);
    }

    m_rack.dock_blend = b;
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", ease(b));
}

void RE4VRReload4::publish_dock() {
    // [DOCK_LERP] Gate auf dock_blend (nicht grab_active): solange der Blend > 0
    // ist, bleibt das Slide-Ziel gesetzt, damit die Konsumenten sauber
    // zurueck-interpolieren.
    auto* rj = rack_joint();
    const int32_t wid = m_wep.wid.value_or(0);

    if (m_rack.dock_blend > 0.001f && rj != nullptr) {
        glm::vec3 p{};
        glm::quat r{};
        const bool has_p = get_vec3(rj, "get_Position", p);
        const bool has_r = get_quat(rj, "get_Rotation", r);
        const auto& sd = rack_slide_pose();

        if (is_shotgun(wid) && m_rack.pump_off.has_value() && !rotary_cycle(wid)) {
            // [SHOTGUN PUMP] Hand = Slide-Pos + (Slide-Rot * gemerkter lokaler
            // Offset) -> wandert mit dem Pump. Rotation = live Support-Hand.
            if (has_p && has_r) {
                p = p + (r * *m_rack.pump_off);
            }

            if (const auto srot = re4vr::lua_get_quat("__vr_support_hand_world_rot");
                srot.has_value()) {
                r = *srot;
            }
        } else {
            // [POSE2_OFFSETS] Welcher Offset-Satz gilt? Bei Pistolen entscheidet
            // dieselbe Weiche wie bei der Finger-Pose (rack.pose_side, EINMAL
            // beim Greifen bestimmt). Alle anderen Gattungen und der
            // _08-Empty-Reload-Slide nehmen nur den ersten Satz.
            float dx = sd.dock_x, dy = sd.dock_y, dz = sd.dock_z;
            float rx = sd.rack_rx, ry = sd.rack_ry, rz = sd.rack_rz;

            const char* cat = category_of(wid);

            if (m_rack.pose_side && !m_rack.empty_reload && cat != nullptr
                && std::strcmp(cat, "pistols") == 0) {
                dx = sd.sdock_x; dy = sd.sdock_y; dz = sd.sdock_z;
                rx = sd.srack_rx; ry = sd.srack_ry; rz = sd.srack_rz;
            }

            if (has_p && has_r && (dx != 0.0f || dy != 0.0f || dz != 0.0f)) {
                p = p + (r * glm::vec3{dx, dy, dz});
            }

            // Rotations-Offset auf die Slide-Rotation -> feste, tunebare Pose.
            if (has_r && (rx != 0.0f || ry != 0.0f || rz != 0.0f)) {
                r = glm::normalize(r * quat_from_euler(rx, ry, rz));
            }
        }

        re4vr::lua_set_vec3("__vr_slide_hand_world_pos", p);
        re4vr::lua_set_quat("__vr_slide_hand_world_rot", r);

        return;
    }

    // [PUSH_DOCK] BEVOR geleert wird: laeuft gerade das Mag-Nachdruecken,
    // gehoert das Dock dem Magazin. Wichtig, weil publish_dock 5x pro Frame
    // laeuft -- ein blindes Leeren raeumte das Push-Dock jedes Mal wieder ab.
    if (publish_push_dock()) {
        return;
    }

    // [DOCK-RELEASE] Fallende Flanke des Push-Docks -> EINMAL einen Zeitstempel
    // setzen; die linke Hand fadet dann in motion weich zum Controller. Die
    // Globals hier weiter HART leeren -- das Ausfaden macht allein motion.
    if (m_rack.pushdock_was_active) {
        re4vr::lua_set_number("__re4_reload_lexit_t", clock_now());
        m_rack.pushdock_was_active = false;
    }

    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
}

// [PUSH_DOCK] Waehrend die Hand das Mag nachdrueckt, exakt derselbe Weg wie
// beim Slide-Grab: ein IK-Dock-Ziel veroeffentlichen. Bezug ist der
// MAGAZIN-Joint, also klebt die Hand am Mag und folgt der Waffe -- unabhaengig
// vom linken Controller. Laeuft NUR, wenn das Slide-Dock nicht selbst aktiv ist.
bool RE4VRReload4::publish_push_dock() {
    if (m_adv == nullptr || m_wep.mag_joint == nullptr) {
        return false;
    }

    if (m_rack.dock_blend > 0.001f) {
        return false;   // Slide-Dock hat Vorrang
    }

    const float b = m_adv->push_blend();

    if (b <= 0.001f) {
        return false;
    }

    glm::vec3 p{};
    glm::quat r{};

    if (!get_vec3(m_wep.mag_joint, "get_Position", p)
        || !get_quat(m_wep.mag_joint, "get_Rotation", r)) {
        return false;
    }

    // [ADA] Positions-Offset ueber den Getter holen: bei Ada steckt ihr
    // Zusatz-Offset schon drin. [PUSH-Y PRO WAFFE] wep.wid MITGEBEN -- der
    // Y-Zusatz darf nicht am globalen __re4_reload_ui_wid haengen, weil
    // publish_dock mehrfach pro Frame laeuft, auch in fremden Passes.
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    m_adv->push_pos(m_wep.wid, ox, oy, oz);

    if (ox != 0.0f || oy != 0.0f || oz != 0.0f) {
        p = p + (r * glm::vec3{ox, oy, oz});
    }

    const auto& o = m_adv->push;

    if (o.rx != 0.0f || o.ry != 0.0f || o.rz != 0.0f) {
        r = glm::normalize(r * quat_from_euler(o.rx, o.ry, o.rz));
    }

    re4vr::lua_set_vec3("__vr_slide_hand_world_pos", p);
    re4vr::lua_set_quat("__vr_slide_hand_world_rot", r);
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", b);

    // [DOCK-RELEASE] Push-Dock AKTIV -> fuer die Exit-Flanke markieren und einen
    // evtl. laufenden Links-Ausfade abbrechen (Dock hat Vorrang).
    m_rack.pushdock_was_active = true;
    re4vr::lua_set_nil("__re4_reload_lexit_t");

    return true;
}

// ============================================================================
// Slide-Pose treiben (Lua Z.5080-5153)
// ============================================================================
void RE4VRReload4::apply_slide_park() {
    if (!cfg.enabled) {
        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);

    if (rotary_cycle(wid) || break_action(wid)) {
        return;   // slide-Joint ist Rotation/Hebel, kein Z-Slide
    }

    // [SHOTGUN_PUMP_SUPPRESS] Die Shotgun-Engine rackt das Pump-Joint _01 nach
    // JEDEM Schuss selbst (AfterShoot-Node). Das wollen wir MANUELL machen ->
    // den nativen Pump unterdruecken, indem wir _01 jeden Post-Anim-Pass hart
    // auf rest_z halten. AUSNAHME: waehrend unseres eigenen Racks oder der
    // UI-Vorschau -> dann faellt es zur normalen Logik durch.
    if (is_shotgun(wid)) {
        if (m_rack.empty_reload) {
            // [EMPTY-RELOAD SLIDE] Pump-Joint _01 IMMER vorn halten (wird in
            // diesem Modus nicht benutzt); der _08-Lade-Slide wird unten
            // generisch getrieben.
            if (m_wep.slide_joint != nullptr) {
                glm::vec3 cur{};

                if (get_vec3(m_wep.slide_joint, "get_LocalPosition", cur)) {
                    set_vec3(m_wep.slide_joint, "set_LocalPosition",
                             glm::vec3{cur.x, cur.y, slide_pose(wid).rest_z});
                }
            }
            // fall through -> generischer Block treibt _08
        } else if (!(m_rack.grab_active || m_rack.tuning)) {
            if (m_wep.slide_joint != nullptr) {
                glm::vec3 cur{};

                if (get_vec3(m_wep.slide_joint, "get_LocalPosition", cur)) {
                    set_vec3(m_wep.slide_joint, "set_LocalPosition",
                             glm::vec3{cur.x, cur.y, slide_pose(wid).rest_z});
                }
            }

            return;
        }
    }

    // [SLIDE-IDLE-HOLD] GENERELLER FIX: wird ein Rack durch Stagger/Event
    // unterbrochen, bleibt _chambered_hold AUS -> die ENGINE haelt den Slide in
    // Hold-Open HINTER back_z, obwohl die Waffe geladen und feuerbereit ist.
    // Den rest_z-Halt deshalb am ECHTEN Ammo-Stand festmachen statt am fragilen
    // Flag. NICHT bei ENGINE_CLOSES_SLIDE.
    bool chambered_idle = false;

    if (!engine_closes_slide(wid)
        && !(m_rack.needs || m_rack.grab_active || m_rack.tuning || m_mag_out
             || m_rack.empty || m_rack.empty_when_dropped || m_rack.empty_reload)) {
        const auto ld = gun_ammo();

        if (ld.has_value() && *ld > 0) {
            chambered_idle = true;
        }
    }

    if (!(m_rack.empty || m_mag_out || m_rack.needs || m_rack.grab_active
          || m_rack.tuning || m_rack._chambered_hold || chambered_idle)) {
        return;
    }

    auto* sj = rack_joint();

    if (sj == nullptr) {
        return;
    }

    glm::vec3 cur{};

    if (!get_vec3(sj, "get_LocalPosition", cur)) {
        return;   // x/y bleiben, nur Z wird gesetzt
    }

    const auto& sp = rack_slide_pose();
    float z = 0.0f;

    if (m_rack.tuning) {
        z = sp.park_z + (sp.back_z - sp.park_z) * m_rack.tune_frac;
    } else if (m_rack.grab_active) {
        z = sp.park_z + (sp.back_z - sp.park_z) * m_rack.frac;   // 1:1 gezogen
    } else if (m_rack._chambered_hold) {
        if (engine_closes_slide(wid)) {
            return;   // das Schliessen macht die Engine
        }

        z = sp.rest_z;   // NACH dem Rack zu halten
    } else if (chambered_idle) {
        z = sp.rest_z;   // [SLIDE-IDLE-HOLD] geladen + feuerbereit
    } else if (m_rack.needs || m_rack.empty_when_dropped || m_rack.empty) {
        z = sp.park_z;   // 0 Ammo ODER leer nach Drop -> MITTEL bis gerackt
    } else {
        if (engine_closes_slide(wid)) {
            return;   // taktischer Mag-Out: die Engine schliesst
        }

        z = sp.rest_z;   // taktischer Mag-Out (noch Patronen) -> gechambert
    }

    set_vec3(sj, "set_LocalPosition", glm::vec3{cur.x, cur.y, z});
}

// Rack-Handpose der LINKEN Hand. Wir wenden sie NICHT selbst an (unsere Passes
// sind PRE-Anim -> die Engine ueberschreibt), sondern publizieren den NAMEN;
// motion wendet ihn im POST-ANIM-Pass an.
void RE4VRReload4::apply_rack_hand_pose() {
    const int32_t wid = m_wep.wid.value_or(0);
    std::string rp{};

    // [EMPTY-RELOAD SLIDE] Im _08-Modus eigene Hand-Pose statt der Pump-Pose.
    if (m_rack.empty_reload) {
        if (const char* e = rack_pose_empty(wid); e != nullptr) {
            rp = e;
        }
    }

    if (rp.empty()) {
        const char* cat = category_of(wid);

        if (cat != nullptr && std::strcmp(cat, "pistols") == 0) {
            // [RACK_POSE_ANGLE] Pistolen haben ZWEI Posen. RACK_POSE[wid] wird
            // fuer sie deshalb NICHT mehr gelesen -- MAGRack kommt jetzt ueber
            // den Seiten-Zweig und gilt fuer ALLE Pistolen.
            rp = m_rack.pose_side ? cfg.rack_pose_side : cfg.rack_pose;
        } else {
            const auto it = m_rack_pose.find(wid);
            rp = (it != m_rack_pose.end()) ? it->second : cfg.rack_pose;
        }
    }

    if (!cfg.enabled || rp.empty() || m_mag_hand.active || m_mag_insert.active) {
        re4vr::lua_set_nil("__vr_rack_hand_pose");

        return;
    }

    bool want = false;

    if (m_rack.dock_tune) {
        want = true;
    } else if (m_rack.needs) {
        // [POSE ERST AM RACK] Frueher genuegte NAEHE -> die Hand nahm die Pose
        // schon ein, sobald sie in die Naehe kam. Jetzt nur bei echtem Grab.
        want = m_rack.grab_active;
    }

    if (want) {
        re4vr::lua_set_string("__vr_rack_hand_pose", rp);
    } else {
        re4vr::lua_set_nil("__vr_rack_hand_pose");
    }
}

// ============================================================================
// Joint-Finder (UI-Werkzeug, Lua Z.5194-5246)
// ============================================================================

// [FINDER] Waffen-Transform UNABHAENGIG von wep.tf: bei einer noch nicht
// konfigurierten Waffe bleibt wep.tf nil -> sonst koennte man die Joints gar
// nicht durchsuchen.
::REManagedObject* RE4VRReload4::finder_tf() {
    if (m_wep.tf != nullptr) {
        return m_wep.tf;
    }

    const auto wid = get_equip_wid();

    if (!wid.has_value() || *wid == 0) {
        return nullptr;
    }

    return find_weapon(*wid);
}

// [FINDER] alle Joint-Namen der equippten Waffe. 1) echte Joint-Liste,
// 2) Fallback: generische Namen abklopfen. Per wid gecacht.
const std::vector<std::string>& RE4VRReload4::weapon_joint_names() {
    const auto wid = m_wep.wid.has_value() ? m_wep.wid : get_equip_wid();

    if (m_jn_cache_wid.has_value() && wid.has_value() && *m_jn_cache_wid == *wid
        && !m_jn_cache.empty()) {
        return m_jn_cache;
    }

    auto* tf = finder_tf();

    if (tf == nullptr) {
        m_jn_cache.clear();

        return m_jn_cache;
    }

    std::vector<std::string> names;
    std::unordered_map<std::string, bool> seen;

    // [ARRAY-BINDING] get_size / get_element, NICHT get_Count/get_Item.
    if (auto* arr = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints"); arr != nullptr) {
        const int32_t n = re4vr::array_size(arr);

        for (int32_t i = 0; i < n; ++i) {
            auto* jt = re4vr::array_element(arr, i);

            if (jt == nullptr) {
                continue;
            }

            auto* nm = re4vr::call_safe<::REManagedObject*>(jt, "get_Name");

            if (nm == nullptr) {
                continue;
            }

            const auto name =
                utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm));

            if (!name.empty() && !seen[name]) {
                seen[name] = true;
                names.push_back(name);
            }
        }
    }

    if (names.empty()) {
        std::vector<std::string> cand{"root"};

        for (int i = 0; i <= 30; ++i) {
            char b[8]{};
            std::snprintf(b, sizeof(b), "_%02d", i);
            cand.emplace_back(b);
        }

        for (const char* nm : {"_100", "_101", "_102", "_103", "_104", "_105"}) {
            cand.emplace_back(nm);
        }

        for (const auto& nm : cand) {
            if (!seen[nm] && joint_by_name(tf, nm) != nullptr) {
                seen[nm] = true;
                names.push_back(nm);
            }
        }
    }

    if (!names.empty()) {
        m_jn_cache_wid = wid;
        m_jn_cache = names;
    }

    return m_jn_cache;
}

void RE4VRReload4::apply_finder() {
    if (!m_finder.active) {
        return;
    }

    auto* tf = finder_tf();

    if (tf == nullptr) {
        return;
    }

    auto* j = joint_by_name(tf, m_finder.name);

    if (j == nullptr) {
        return;
    }

    if (m_finder.base_name != m_finder.name || !m_finder.base.has_value()) {
        glm::vec3 lp{};

        if (!get_vec3(j, "get_LocalPosition", lp)) {
            return;
        }

        m_finder.base = lp;
        m_finder.base_name = m_finder.name;
    }

    set_vec3(j, "set_LocalPosition",
             glm::vec3{m_finder.base->x + m_finder.x, m_finder.base->y + m_finder.y,
                       m_finder.base->z + m_finder.z});
}

// ============================================================================
// Chamber / Shell-Eject (Lua Z.5249-5324)
// ============================================================================

// [CHAMBER] Top-Loader: der rechte B togglet den Chamber auf/zu.
// (Die CHAMBER-Tabelle ist leer, seit der Red9 nach reload2 ausgezogen ist --
// dieser Weg ist heute unerreichbar, bleibt aber 1:1 stehen.)
void RE4VRReload4::toggle_chamber() {
    m_chamber.open = !m_chamber.open;
    play_weapon_sound(snd_id(m_wep.wid.value_or(0), "chamber"));
}

// [SHELL_EJECT] Shell-Joint _04 in die Chamber-Startposition setzen.
void RE4VRReload4::apply_shell_eject() {
    const int32_t wid = m_wep.wid.value_or(0);

    if (!is_shotgun(wid)) {
        return;
    }

    if (m_wep.mag_joint == nullptr || !m_wep.rest_lp.has_value()) {
        return;
    }

    if (!(m_shell_eject_st.preview || m_shell_eject_st.flying)) {
        return;   // sonst kontrolliert die Engine _04
    }

    const auto& s = shell_eject_cfg(wid);
    const glm::vec3 rl = *m_wep.rest_lp;
    float px = rl.x + s.sx;
    float py = rl.y + s.sy;
    float pz = rl.z + s.sz;
    float spin_deg = 0.0f;

    if (m_shell_eject_st.flying) {
        const float t = m_shell_eject_st.t;
        px += s.vx * t;
        py += s.vy * t - 0.5f * s.grav * t * t;   // Bogen: hoch, dann fallen
        pz += s.vz * t;
        spin_deg = s.spin * t;                    // Taumeln
    }

    set_vec3(m_wep.mag_joint, "set_LocalPosition", glm::vec3{px, py, pz});

    if (m_wep.rest_lr.has_value()) {
        set_quat(m_wep.mag_joint, "set_LocalRotation",
                 glm::normalize(*m_wep.rest_lr
                                * quat_from_euler(s.srx + spin_deg, s.sry, s.srz)));
    }
}

// [SHELL_EJECT] Flug-State 1x/Frame fortschreiben (NICHT in apply_shell_eject --
// das laeuft 4x/Frame).
void RE4VRReload4::update_shell_eject() {
    const int32_t wid = m_wep.wid.value_or(0);

    if (!is_shotgun(wid)) {
        m_shell_eject_st.flying = false;
        m_shell_eject_st._prev_pulled = false;

        return;
    }

    const double now = clock_now();
    double dt = now - (m_shell_eject_st.last_clock != 0.0 ? m_shell_eject_st.last_clock : now);
    m_shell_eject_st.last_clock = now;

    if (dt < 0.0 || dt > 0.1) {
        dt = 0.016;   // Sprung-Schutz (Pause/Ladebildschirm)
    }

    // [GIMMICK] Auf der rack.pulled-Flanke (Chamber offen) fliegt die Shell
    // kosmetisch raus, solange geladen.
    if (m_rack.pulled && !m_shell_eject_st._prev_pulled && !m_shell_eject_st.preview) {
        if (pump_one_out()) {
            m_shell_eject_st.flying = true;
            m_shell_eject_st.t = 0.0f;
        }
    }

    m_shell_eject_st._prev_pulled = m_rack.pulled;

    if (m_shell_eject_st.flying) {
        m_shell_eject_st.t += static_cast<float>(dt);

        if (m_shell_eject_st.t >= shell_eject_cfg(wid).dur) {
            m_shell_eject_st.flying = false;   // Flug vorbei
        }
    }
}

// [CHAMBER] Chamber-Joint in den Override-Paessen halten.
void RE4VRReload4::apply_chamber() {
    // (CHAMBER ist leer -> diese Funktion tut heute nichts. 1:1 uebernommen.)
    (void)m_chamber;
}

// ============================================================================
// Zentraler Reset (Lua Z.5333-5379)
// ============================================================================
// Bei JEDEM Waffenwechsel / Unequip / Enemy-Grab -> kein haengender
// Zwischenstand (halber Drop/Insert, Mag-in-Hand, stale Block/needs_rack).
void RE4VRReload4::reset_reload_state() {
    stop_mag_drop();
    m_drop.active = false;
    m_mag_hand.active = false;
    m_mag_insert.active = false;
    m_mag_insert.settle = false;
    m_mag_tune.active = false;

    m_rack.grab_active = false;
    m_rack.armed = false;
    m_rack.frac = 0.0f;
    m_rack.pulled = false;
    m_rack.needs = false;
    m_rack.has_mag = false;
    m_rack.empty_when_dropped = false;
    m_rack._zeroed_by_us = false;
    m_rack.empty_reload = false;

    m_rotary.prog = 0.0f;
    m_rotary.dir = 0;
    m_rotary._prev_trig = false;
    m_rotary.pending = false;
    m_rotary._grip_latched = false;
    m_rotary._prev_grip = false;
    re4vr::lua_set_bool("__vr_block_two_hand", false);

    m_break.prog = 0.0f;
    m_break.open = false;
    m_break._was_open = false;
    m_break._prev_b = false;
    re4vr::lua_set_bool("__vr_break_open", false);

    m_rack._pump_ref_z.reset();
    // [PUMP_OFF STALE] sonst behaelt eine vorher gepumpte Shotgun ihren lokalen
    // Pump-Offset; bei der naechsten Shotgun naehme publish_dock faelschlich den
    // Pump-Zweig und ignorierte deren Dock-/Rot-Offsets.
    m_rack.pump_off.reset();

    // [GOLDEN RULE] Schuss-Sequenz-Baseline beim Wechsel NEU syncen, sonst
    // triggert die Shotgun-Schuss-Erkennung faelschlich rack.needs auf der
    // frisch gezogenen Waffe.
    m_rack._prev_shot_seq.reset();
    m_rack._chambered_hold = false;

    m_chamber.open = false;
    m_chamber.blend = 0.0f;
    m_chamber.base.reset();
    m_chamber.base_joint.clear();

    // mag_out NICHT hier nullen -> per-Waffe persistent.
    re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    re4vr::lua_set_string("__re4_bf_who", "reload4/reset_reload_state (Waffenwechsel)");
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_shotgun_pump_active", false);
    re4vr::lua_set_bool("__vr_rack_block_left_knife", false);
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", false);
    re4vr::lua_set_nil("__vr_slide_hand_world_pos");
    re4vr::lua_set_nil("__vr_slide_hand_world_rot");
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
    m_rack.dock_blend = 0.0f;
    re4vr::lua_set_nil("__vr_rack_hand_pose");

    // [WI_CACHE] gecachtes Live-Item ist nach dem Wechsel stale.
    m_live_wi = nullptr;
}

// ============================================================================
// [MAG_OUT] Mag-Mesh aus der Kammer halten (Lua Z.5761-5774)
// ============================================================================
// Die Engine setzt das Mag beim Equip neu in die Kammer -> wir skalieren den
// Mag-Joint jeden Frame auf 0. NUR wenn kein Flow laeuft.
void RE4VRReload4::apply_mag_out_hidden() {
    const bool should_hide = m_mag_out && m_wep.mag_joint != nullptr
        && !m_drop.active && !m_mag_hand.active && !m_mag_insert.active;

    if (should_hide) {
        set_vec3(m_wep.mag_joint, "set_LocalScale", glm::vec3{0.0f, 0.0f, 0.0f});
        m_mag_hidden_applied = true;
    } else if (m_mag_hidden_applied) {
        if (m_wep.mag_joint != nullptr) {
            set_vec3(m_wep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
        }

        m_mag_hidden_applied = false;
    }
}

// ============================================================================
// [SHELL_PART] Sub-Mesh-Parts der Shotgun-Huelse (Lua Z.5776-5835)
// ============================================================================
// Die gechamberte Shell ist ein Sub-Mesh-PART des Gun-Meshes. Die Engine
// disabled ihn bei leerer Kammer -> bei 0 ist nichts zum Anzeigen da
// (Bone-Scale reicht nicht). RE9-Mechanismus: setPartsEnable am via.render.Mesh.
// Der Part-Index kommt AUTOMATISCH per Diff, kein Hardcode.
::REManagedObject* RE4VRReload4::sg_mesh() {
    if (m_wep.tf != m_sg.tf || m_sg.mesh == nullptr) {
        m_sg.tf = m_wep.tf;
        m_sg.mesh = nullptr;
        m_sg.e1.reset();
        m_sg.empty = false;

        // [PERSIST] Part-Index aus dem Per-Waffe-Cache wiederherstellen: nach
        // Wechsel-zurueck-bei-leer wuerde der Diff sonst nie laufen.
        m_sg.shell.clear();

        if (m_wep.wid.has_value()) {
            if (const auto it = m_shell_parts.find(*m_wep.wid); it != m_shell_parts.end()) {
                m_sg.shell = it->second;
            }
        }

        auto* go = (m_wep.tf != nullptr)
            ? re4vr::call_safe<::REManagedObject*>(m_wep.tf, "get_GameObject") : nullptr;

        if (go != nullptr) {
            static auto* td_mesh = sdk::find_type_definition("via.render.Mesh");
            m_sg.mesh = re4vr::get_component(go, td_mesh);
        }
    }

    return m_sg.mesh;
}

bool RE4VRReload4::sg_part_on(::REManagedObject* m, int32_t i) {
    bool v = false;

    return re4vr::try_call<bool>(m, "getPartsEnable", v, i) && v;
}

// e1 (enabled-Set bei loaded>0) EINMAL merken; den Shell-Part EINMAL per Diff
// ermitteln und dann STABIL behalten (der Part-Index ist fix).
void RE4VRReload4::sg_update_parts(std::optional<int32_t> loaded) {
    const int32_t wid = m_wep.wid.value_or(0);

    // [SHELL_PART_FIX] Auch Break-Action lernt den Shell-Part. (Die
    // SHELL_PART_LEARN-Tabelle ist leer -> heute nur die Riot Gun.)
    if (empty_reload_joint(wid) == nullptr) {
        return;
    }

    auto* m = sg_mesh();

    if (m == nullptr) {
        return;
    }

    // Gate auf KAMMER leer (isGunAmmoEmpty), NICHT loaded == 0: nach der 1.
    // Shell ist loaded=1 (Roehre), aber die Kammer ist bis zum _08-Rack leer.
    m_sg.empty = m_rack.empty;

    if (loaded.has_value() && *loaded > 0 && !m_sg.empty) {
        if (!m_sg.e1.has_value()) {
            std::vector<int32_t> set;

            for (int32_t i = 0; i <= 255; ++i) {
                if (sg_part_on(m, i)) {
                    set.push_back(i);
                }
            }

            m_sg.e1 = set;
        }
    } else if (m_sg.empty && m_sg.e1.has_value() && m_sg.shell.empty()) {
        // Shell-Part EINMAL bestimmen (nachlaufen, bis nicht leer -- die Engine
        // disabled ihn evtl. 1-2 Frames spaeter).
        std::vector<int32_t> sh;

        for (int32_t p : *m_sg.e1) {
            if (!sg_part_on(m, p)) {
                sh.push_back(p);
            }
        }

        if (!sh.empty()) {
            m_sg.shell = sh;
            m_shell_parts[wid] = sh;   // [PERSIST] ueberlebt den Waffenwechsel
            save_cfg();                // ... und (via Disk) den Script-Reset
        }
    }
}

// on=true (Shell in der Hand): IMMER sichtbar erzwingen, egal ob die Kammer leer
// oder voll ist. on=false: nur bei LEERER Kammer ausschalten (Phantom-Huelse);
// bei voller Kammer NICHTS anfassen -> die Engine zeigt die Huelse selbst.
void RE4VRReload4::sg_force_shell_parts(bool on) {
    auto* m = sg_mesh();   // ZUERST: kann m_sg (inkl. shell) bei Wechsel neu setzen

    if (m == nullptr || m_sg.shell.empty()) {
        return;
    }

    if (!on && !m_sg.empty) {
        return;   // volle Kammer + nicht getragen -> Engine machen lassen
    }

    for (int32_t p : m_sg.shell) {
        re4vr::call_safe<void*>(m, "setPartsEnable", static_cast<uint64_t>(p), on);
    }
}

// ============================================================================
// [SHELL_SPAWN] Getragene Huelse als eigene Instanz (Lua Z.5837-5914)
// ============================================================================
// Waffen ohne statischen Shell-Part: die Engine generiert die Huelse via
// ShotgunShellGenerator prozedural -> bei 0 Ammo ist nichts da. Wir
// instanziieren das Shell-Prefab der Waffe einmal und docken es an die
// _04-Joint-Weltpose.
// (Die SHELL_SPAWN-Tabelle ist LEER, seit der Skull Shaker den Mesh-Clone
// nutzt -- dieser Weg ist heute unerreichbar, bleibt aber 1:1 stehen.)

void RE4VRReload4::destroy_spawn_go() {
    if (m_spawn.go != nullptr) {
        re4vr::destroy_game_object(m_spawn.go);
    }

    m_spawn.go = nullptr;
    m_spawn.tf = nullptr;
}

// Generator-Komponente der equippten Waffe. ROBUST per Typ-NAME: die Komponente
// kann chainsaw.ShotgunShellGenerator ODER die GmActor-Subklasse sein ->
// getComponent(exakter Typ) wuerde die andere verfehlen.
::REManagedObject* RE4VRReload4::find_shell_generator() {
    if (m_wep.tf == nullptr) {
        return nullptr;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(m_wep.tf, "get_Child");
    int n = 0;

    while (child != nullptr && n < 64) {
        ++n;
        auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

        if (go != nullptr) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(go, "get_Name");
            const auto name = (nm != nullptr)
                ? utility::re_string::get_string(reinterpret_cast<::SystemString*>(nm))
                : std::string{};

            if (name.find("ShellGenerator") != std::string::npos) {
                auto* comps = re4vr::call_safe<::REManagedObject*>(go, "get_Components");

                if (comps != nullptr) {
                    const int32_t cn = re4vr::array_size(comps);

                    for (int32_t i = 0; i < cn; ++i) {
                        auto* c = re4vr::array_element(comps, i);

                        if (c == nullptr) {
                            continue;
                        }

                        auto* td = utility::re_managed_object::get_type_definition(c);
                        const std::string tn = (td != nullptr) ? td->get_full_name() : "";

                        if (tn.find("ShellGenerator") != std::string::npos) {
                            return c;
                        }
                    }
                }
            }
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return nullptr;
}

::REManagedObject* RE4VRReload4::ensure_shell_spawn() {
    if (m_spawn.go != nullptr && m_spawn.wid == m_wep.wid
        && re4vr::call_bool_not_false(m_spawn.go, "get_Valid")) {
        return m_spawn.go;
    }

    if (m_spawn.go != nullptr && m_spawn.wid != m_wep.wid) {
        destroy_spawn_go();
    }

    auto* gen = find_shell_generator();

    if (gen == nullptr) {
        return nullptr;
    }

    auto* ud = re4vr::call_safe<::REManagedObject*>(gen, "get_UserData");

    if (ud == nullptr) {
        return nullptr;
    }

    ::REManagedObject* prefab = nullptr;

    if (auto* td = utility::re_managed_object::get_type_definition(ud); td != nullptr) {
        if (auto* f = td->get_field("_Prefab"); f != nullptr) {
            prefab = f->get_data<::REManagedObject*>(ud);
        }
    }

    if (prefab == nullptr) {
        return nullptr;
    }

    bool ready = true;

    if (re4vr::try_call<bool>(prefab, "get_Ready", ready) && !ready) {
        return nullptr;
    }

    std::optional<glm::vec3> pos{};
    glm::vec3 p{};

    if (m_wep.mag_joint != nullptr && get_vec3(m_wep.mag_joint, "get_Position", p)) {
        pos = p;
    }

    if (!pos.has_value()) {
        pos = re4vr::lua_get_vec3("__vr_lh_world");
    }

    if (!pos.has_value() && m_wep.tf != nullptr && get_vec3(m_wep.tf, "get_Position", p)) {
        pos = p;
    }

    if (!pos.has_value()) {
        return nullptr;
    }

    const glm::quat rot = re4vr::lua_get_quat("__vr_lh_rot").value_or(
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f});

    __declspec(align(16)) glm::vec4 vpos{pos->x, pos->y, pos->z, 0.0f};
    __declspec(align(16)) glm::vec4 vrot{rot.x, rot.y, rot.z, rot.w};

    auto* go = re4vr::call_safe<::REManagedObject*>(prefab, "instantiate", &vpos, &vrot);

    if (go == nullptr) {
        return nullptr;
    }

    // Shell-Script/Physik aus -> reine Optik, die Transform steuern WIR.
    re4vr::call_safe<void*>(go, "set_UpdateSelf", false);

    m_spawn.go = go;
    m_spawn.tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
    m_spawn.wid = m_wep.wid;

    return go;
}

void RE4VRReload4::update_shell_spawn() {
    // (SHELL_SPAWN ist leer -> der Zweig raeumt heute nur auf.)
    if (m_spawn.go != nullptr) {
        destroy_spawn_go();
    }
}

// ============================================================================
// [SHELL_CLONE] Skull Shaker: Patrone in der Hand als Mesh-Klon (Lua Z.5916-6043)
// ============================================================================
// Technik wie der reload2-Revolver: via.motion.Motion baut das Skelett,
// via.render.Mesh + setMesh(lebender Gun-Holder) + Gun-Material, dann Part
// isolieren.

::REManagedObject* RE4VRReload4::ss_gun_mesh() {
    if (m_wep.tf == nullptr) {
        return nullptr;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(m_wep.tf, "get_GameObject");

    if (go == nullptr) {
        return nullptr;
    }

    static auto* td_mesh = sdk::find_type_definition("via.render.Mesh");

    return re4vr::get_component(go, td_mesh);
}

void RE4VRReload4::ss_destroy() {
    if (m_ssc.obj != nullptr) {
        re4vr::destroy_game_object(m_ssc.obj);
    }

    m_ssc.obj = nullptr;
    m_ssc.mesh = nullptr;
    m_ssc.parts_sig.clear();
    m_ssc.parented = false;
}

bool RE4VRReload4::ss_spawn() {
    if (m_ssc.obj != nullptr) {
        return true;
    }

    auto* gmesh = ss_gun_mesh();

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");
    auto* go = re4vr::create_game_object("vr_so_shell");

    if (go == nullptr) {
        return false;
    }

    auto* obj = reinterpret_cast<::REManagedObject*>(go);

    // Lua: go:add_ref() SOFORT. Selbst erzeugtes Objekt -> BEDINGUNGSLOS
    // pinnen, die refcount-Heuristik greift hier nicht.
    utility::re_managed_object::add_ref(obj);

    // KERN: die Motion-Komponente baut das Skelett (sonst unsichtbar).
    if (auto* motion_rt = re4vr::runtime_type("via.motion.Motion"); motion_rt != nullptr) {
        re4vr::call_safe<::REManagedObject*>(obj, "createComponent(System.Type)", motion_rt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* mesh_rt = re4vr::runtime_type("via.render.Mesh"); mesh_rt != nullptr) {
        mesh = re4vr::call_safe<::REManagedObject*>(obj, "createComponent(System.Type)", mesh_rt);
    }

    if (mesh == nullptr) {
        // Lua verlaesst hier mit `return false` OHNE das GO zu zerstoeren -- mit
        // echtem Refcount waere das ein Leck pro Frame.
        re4vr::destroy_game_object(obj);

        return false;
    }

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (gmat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", gmat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);

    m_ssc.obj = obj;
    m_ssc.mesh = mesh;

    // [SCENE-PARENT] Der Clone MUSS in den Szenen-Graph, sonst rendert das
    // geskinnte Mesh nicht (ein blosses create legt das Objekt an, es zeichnet
    // aber nichts). Hier an die WAFFEN-Transform: der lokale Frame ist damit der
    // Waffen-Root -- die Keyframes sind ohnehin waffenrelativ, ss_place setzt
    // danach nur noch die LOKALE Pose (klebt an der Waffe, kein Welt-Drift).
    m_ssc.parented = false;

    auto* ctf = re4vr::call_safe<::REManagedObject*>(m_ssc.obj, "get_Transform");

    if (ctf != nullptr && m_wep.tf != nullptr) {
        re4vr::call_safe<void*>(ctf, "set_Parent", m_wep.tf);
        m_ssc.parented = true;
    }

    return true;
}

// [CLONE-PART AUS DEM RELOAD-WEG] Der Clone isoliert GENAU die Parts, die auch
// der echte Reload als Huelse sichtbar schaltet (m_shell_parts, gespeist aus dem
// Lern-Diff oder der festen Zuordnung). Damit koennen Werkzeug und Spiel nicht
// auseinanderlaufen. Der Slider-Wert aus reload_adv ist nur noch Notnagel, wenn
// fuer die Waffe gar nichts hinterlegt ist -- und nie Part 0 (= das ganze
// Waffen-Mesh).
void RE4VRReload4::ss_isolate() {
    if (m_ssc.mesh == nullptr) {
        return;
    }

    std::vector<int32_t> want{};
    std::string sig{};
    const auto it = m_shell_parts.find(m_wep.wid.value_or(0));

    if (it != m_shell_parts.end() && !it->second.empty()) {
        want = it->second;
        sig = "parts:";

        for (size_t i = 0; i < want.size(); ++i) {
            sig += (i ? "," : "") + std::to_string(want[i]);
        }
    } else {
        int32_t part = (m_adv != nullptr) ? m_adv->shell_clone_part : 1;

        if (part < 1) {
            part = 1;
        }

        want.push_back(part);
        sig = "part:" + std::to_string(part);
    }

    if (m_ssc.parts_sig == sig) {
        return;
    }

    for (int32_t i = 0; i <= 48; ++i) {
        const bool on = std::find(want.begin(), want.end(), i) != want.end();
        re4vr::call_safe<void*>(m_ssc.mesh, "setPartsEnable",
                                static_cast<uint64_t>(i), on);
    }

    m_ssc.parts_sig = sig;
}

// Position an der Keyframe-Bahn (relativ zur WAFFE, wie die Preview in
// reload_adv): Welt = Waffenposition + Waffenrotation * Offset.
void RE4VRReload4::ss_place(float px, float py, float pz,
                            float rx, float ry, float rz) {
    if (m_ssc.obj == nullptr) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(m_ssc.obj, "get_Transform");

    if (tf == nullptr) {
        return;
    }

    const float scl = (m_adv != nullptr) ? m_adv->shell_clone_scale : 1.0f;

    // [SCENE-PARENT] An der Waffen-Transform geparentet -> der lokale Frame IST
    // der Waffen-Root -> die Keyframe-Pose direkt LOKAL setzen (identische
    // Mathematik wie der alte Welt-Weg, nur ohne Nachziehen/Drift).
    if (m_ssc.parented) {
        set_vec3(tf, "set_LocalPosition", glm::vec3{px, py, pz});
        set_quat(tf, "set_LocalRotation", quat_from_euler(rx, ry, rz));
        set_vec3(tf, "set_LocalScale", glm::vec3{scl, scl, scl});

        return;
    }

    // Fallback (nicht geparentet): der Welt-Weg relativ zur Waffe.
    if (m_wep.tf == nullptr) {
        return;
    }

    glm::vec3 gp{};
    glm::quat gr{};

    if (!get_vec3(m_wep.tf, "get_Position", gp)
        || !get_quat(m_wep.tf, "get_Rotation", gr)) {
        return;
    }

    set_vec3(tf, "set_Position", gp + (gr * glm::vec3{px, py, pz}));
    set_quat(tf, "set_Rotation", glm::normalize(gr * quat_from_euler(rx, ry, rz)));
    set_vec3(tf, "set_LocalScale", glm::vec3{scl, scl, scl});
}

// [SHELL_CLONE SAWED-OFF] Die native _04-Shell der Sawed-off ist beim
// Keyframe-Preview nicht sichtbar (der Shell-Part wird nie gelernt, die Engine
// skaliert die leere Kammer auf 0). Deshalb spawnen wir die getragene Huelse als
// EIGENE Mesh-Instanz (Gun-Mesh geklont, ein Part isoliert) und setzen sie auf
// die Keyframe-Bahn: Preview = Tuning-Lage, Insert = die Bahn abfahren.
//
// In Lua steht daneben noch ein zweiter, gleich gebauter Block fuer den Skull
// Shaker (6001) an der L_Hand -- der ist hier TOT: er prueft auf wid == 6001,
// und diese Datei verwaltet ausschliesslich 6100/6103/6104/6112. Deshalb ist er
// nicht mitportiert.
void RE4VRReload4::ss_apply() {
    if (!cfg.enabled || m_wep.wid.value_or(0) != 6100 || m_adv == nullptr) {
        if (m_ssc.obj != nullptr) {
            ss_destroy();
        }

        return;
    }

    const bool preview =
        static_cast<int32_t>(re4vr::lua_get_number("__re4_shell_kf_preview", 0.0)) == 6100;
    const bool inserting = m_mag_insert.active && m_mag_insert.keyframe;

    if (!(preview || inserting)) {
        if (m_ssc.obj != nullptr) {
            ss_destroy();
        }

        return;
    }

    if (m_ssc.obj == nullptr && !ss_spawn()) {
        return;
    }

    ss_isolate();

    RE4VRReloadAdv::Key k{};
    bool have = false;

    if (inserting) {
        float t = static_cast<float>(clock_now() - m_mag_insert.t0)
                  / std::max(m_mag_insert.dur, 0.01f);

        if (t > 1.0f) {
            t = 1.0f;
        }

        have = m_adv->shell_pose_at(6100, t, k);
    }

    if (!have) {
        k = m_adv->shell_live;
    }

    ss_place(k.x, k.y, k.z, k.rx, k.ry, k.rz);
}

// In Lua haengt der Sawed-off-Klon mit DERSELBEN Funktion auch am
// BeginRendering-POST (keine schlanke Sonderfassung wie beim Hand-Klon).
void RE4VRReload4::ss_repos_late() {
    ss_apply();
}

// ============================================================================
// Pass-Rumpfe (Lua Z.6045-6205)
// ============================================================================

void RE4VRReload4::apply_drop_pass() {
    if (!cfg.enabled) {
        return;
    }

    // [X-LINKS-BLITZ] Der Proximity-Check lief in einem anderen Hook als diese
    // Anwendung -- lief die Anwendung im Frame VORHER, blieb das Mag noch einen
    // Frame an der Handposition (seitlich), bevor der Insert es auf die Achse
    // zog. Deshalb den Check hier im SELBEN Pass zuerst laufen lassen; er ist
    // idempotent (mag_hand.active schon false -> sofortiger Return).
    check_mag_insert_proximity();
    update_mag_drop();
    update_mag_in_hand();
    update_mag_insert();
    apply_mag_out_hidden();
}

// [ENTKOPPEL_ROT/HMD] Der gedroppte Mag ist ein ANIMIERTER Waffen-Joint ->
// braucht den VOLLEN Joint-Override-Stack. In UpdateJointExpression schreibt
// die Engine den Joint aus der HMD-gefuehrten Waffenpose zurueck -> ohne
// unseren Write in diesem Pass wackelte das gefallene Mag mit dem Kopf.
void RE4VRReload4::apply_drop_joint_pass() {
    if (!cfg.enabled) {
        return;
    }

    if (m_drop.active) {
        update_mag_drop();
    }
}

// [SS-FLASH] Letzter Schreibpunkt VOR dem Rendern. BEWUSST eng: NUR wp6001,
// NUR update_mag_insert (kein Drop, kein Proximity-Check) und das im
// Sicht-Modus -- aus dieser Phase kann KEIN Reload-Call, kein Ammo-Write und
// kein Sound entstehen.
void RE4VRReload4::ss_flash_pass() {
    if (!cfg.enabled || m_wep.wid.value_or(0) != 6001 || !m_mag_insert.active) {
        return;
    }

    m_mag_insert.visual = true;
    update_mag_insert();
    m_mag_insert.visual = false;
}

// Slide-Park + Joint-Finder schreiben ANIMIERTE Joints -> brauchen den VOLLEN
// Joint-Override-Stack, sonst ueberschreibt die Engine den Write.
void RE4VRReload4::apply_slide_pass() {
    if (!cfg.enabled) {
        return;
    }

    const int32_t wid = m_wep.wid.value_or(0);

    // [LET-GO] Joint-Writes NUR wenn die Waffe verwaltet ist.
    if (m_managed) {
        apply_chamber();
        apply_slide_park();

        // [ROTARY/BREAK] Der slide-Joint ist eine Rotation. Ruhe-Rotation um
        // (rx,ry,rz)*prog drehen. prog 0 = Ruhe.
        // [SKULL_LADE] Ohne cycle_rest_rot schreibt der Block NICHTS -- dann
        // bleibt die Lade stumm. Fehlt sie, hier einmal nachziehen.
        if (break_action(wid) && m_wep.slide_joint != nullptr
            && !m_wep.cycle_rest_rot.has_value()) {
            glm::quat q{};

            if (get_quat(m_wep.slide_joint, "get_BaseLocalRotation", q)
                || get_quat(m_wep.slide_joint, "get_LocalRotation", q)) {
                m_wep.cycle_rest_rot = q;
            }
        }

        if ((rotary_cycle(wid) || break_action(wid)) && m_wep.slide_joint != nullptr
            && m_wep.cycle_rest_rot.has_value()) {
            const float p = rotary_cycle(wid) ? m_rotary.prog : m_break.prog;

            if (p > 0.0001f) {
                const auto& r = rotary_cfg(wid);
                const glm::quat q = quat_from_euler(r.rx * p, r.ry * p, r.rz * p);
                set_quat(m_wep.slide_joint, "set_LocalRotation",
                         glm::normalize(*m_wep.cycle_rest_rot * q));
            } else if (break_action(wid)) {
                // [SKULL_FLICK] Bei der Break-Action heisst prog 0 NICHT "Engine
                // ueberlassen": GEMESSEN zieht die native AfterShoot-Anim _02
                // nach JEDEM Schuss von selbst auf -72.3 Grad auf. Weil wir bei
                // prog 0 gar nichts geschrieben haben, kam sie ungehindert
                // durch. Jetzt: _02 im vollen Override-Stack IMMER selbst setzen
                // -- zu, ausser im Flick-Fenster, wo wir dieselbe Bahn
                // nachfahren. [SKULL_LADE] Waehrend der Drehung steht die Lade
                // auf dem eingestellten Winkel, danach wieder auf der Ruhelage.
                const bool open = re4vr::lua_get_tribool("__vr_pump_anim_active") == 1;
                const float soll = open ? cfg.skull_open_deg : 0.0f;
                const glm::quat q = quat_from_euler(soll, 0.0f, 0.0f);
                set_quat(m_wep.slide_joint, "set_LocalRotation",
                         glm::normalize(*m_wep.cycle_rest_rot * q));
            }
        }

        // [EMPTY-RELOAD SLIDE] _08 ZU halten, WANN IMMER kein aktiver
        // Empty-Reload-Zug laeuft: die Engine schliesst den _08 nach dem
        // manuellen Zug NICHT, clear_rack snappt ihn nur 1x.
        if (empty_reload_joint(wid) != nullptr && m_wep.slide_joint2 != nullptr
            && !m_rack.empty_reload) {
            glm::vec3 cur{};

            if (get_vec3(m_wep.slide_joint2, "get_LocalPosition", cur)) {
                set_vec3(m_wep.slide_joint2, "set_LocalPosition",
                         glm::vec3{cur.x, cur.y, slide_pose2(wid).rest_z});
            }
        }

        // [SHELL_VISIBLE] Shell beim Tragen sichtbar erzwingen:
        // (1) _04-Bone-Scale auf 1 (die Engine skaliert ihn bei leerer Kammer
        // auf 0), (2) Shell-Sub-Mesh-PART einschalten.
        // [SS-HANDOVER] Skull Shaker: beim TRAGEN ist die Shell der Hand-Clone
        // und _04 bleibt versteckt. Ab dem INSERT ist es umgekehrt.
        const bool ss_insert = (wid == 6001) && m_mag_insert.active;

        if (m_wep.mag_joint != nullptr
            && (ss_insert
                || (is_shotgun(wid) && wid != 6001
                    && (m_mag_hand.active || m_mag_tune.active || m_mag_insert.active)))) {
            set_vec3(m_wep.mag_joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
            sg_force_shell_parts(true);
        } else {
            sg_force_shell_parts(false);
        }

        apply_shell_eject();

        // Dock-Ziel der Hand JEDEN Pass NACH dem Slide-Setzen frisch
        // publizieren -> arm_chain liest in jedem Pass die aktuelle
        // Slide-Position = kein Nachhinken.
        publish_dock();
        apply_rack_hand_pose();
    }

    // Der Finder ist ein UI-Werkzeug -> unabhaengig von _managed.
    if (!(m_rack.needs || m_rack.tuning)) {
        apply_finder();
    }
}

// ============================================================================
// Pass-Einstiege -- Reihenfolge exakt wie die Registrierungen in der Lua-Datei
// ============================================================================

void RE4VRReload4::on_lock_scene_pre() {
    ss_apply();              // Lua Z.6038
    apply_drop_pass();       // Lua Z.6059
    apply_slide_pass();      // Lua Z.6202
}

void RE4VRReload4::on_late_update() {
    ss_apply();              // Lua Z.6039
    apply_drop_pass();       // Lua Z.6060
    apply_slide_pass();      // Lua Z.6203
}

void RE4VRReload4::on_update_joint_expression() {
    ss_apply();              // Lua Z.6040
    apply_drop_joint_pass(); // Lua Z.6087
    apply_slide_pass();      // Lua Z.6204

    // [VERSATZ BEIM LAUFEN 2026-09-07] Dieselbe Sache wie in RE4VRReload2:
    // die *_late-Funktionen fahren gespawnte Mesh-Part-Klone entlang ihrer
    // Bahn und standen bisher NUR im BeginRendering(POST). Danach bewegt die
    // Engine Hand und Waffe noch zweimal (LateUpdateBehavior,
    // UpdateJointExpression) -- der Klon bleibt stehen und sitzt beim Laufen
    // um eine konstante Frame-Strecke versetzt. Hier im LETZTEN Pass
    // nachziehen; Waffen-Joints brauchen das nicht, die propagiert die Engine.
    ss_repos_late();
}

void RE4VRReload4::on_begin_rendering_pre() {
    ss_apply();              // Lua Z.6041
    ss_flash_pass();         // Lua Z.6070
    apply_drop_joint_pass(); // Lua Z.6088
    apply_slide_pass();      // Lua Z.6205
}

void RE4VRReload4::on_begin_rendering() {
    ss_repos_late();         // Lua Z.6042
    apply_drop_pass();       // Lua Z.6061
}

// ============================================================================
// Getaktete Teil-Ticks (Lua Z.2079-2138, 6939-7007)
// ============================================================================

// [SORTE HEILEN] Zeigt eine Waffe "0 Reserve", obwohl die passende Munition im
// Koffer liegt, steht in ihrem Feld _CurrentAmmo eine Sorte ohne Bestand -- das
// Spiel zeichnet die Anzeige aus genau diesem Feld, und mit 0 Reserve laesst
// sich gar nicht erst nachladen. Der Ladeweg kann das nicht reparieren, er wird
// nie erreicht. Deshalb hier getickt, unabhaengig vom Nachladen.
void RE4VRReload4::tick_sortenfix() {
    if (clock_now() < m_sortefix_next) {
        return;
    }

    m_sortefix_next = clock_now() + 1.0;

    auto* wi = real_wi();

    if (wi == nullptr) {
        return;
    }

    auto* p = pe();

    if (p == nullptr) {
        return;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(p, "get_InventoryController");

    if (inv == nullptr) {
        return;
    }

    const auto cur = call_enum(wi, "get_CurrentAmmo");

    if (!cur.has_value()) {
        return;
    }

    auto* arr = re4vr::call_safe<::REManagedObject*>(wi, "get_UsableAmmoList");

    if (item_count_sum(inv, *cur) > 0) {
        return;   // Bestand da -> nichts zu heilen
    }

    if (arr == nullptr) {
        return;
    }

    // [ARRAY-BINDING] get_size / get_element.
    const int32_t n2 = re4vr::array_size(arr);

    for (int32_t i = 0; i < n2; ++i) {
        auto* a = re4vr::array_element(arr, i);

        if (a == nullptr) {
            continue;
        }

        std::optional<int32_t> anum{};

        if (auto* td = utility::re_managed_object::get_type_definition(a); td != nullptr) {
            if (auto* f = td->get_field("value__"); f != nullptr) {
                anum = f->get_data<int32_t>(a);
            }
        }

        if (!anum.has_value() || *anum == *cur) {
            continue;
        }

        if (item_count_sum(inv, *anum) > 0) {
            re4vr::call_safe<void*>(wi, "setAmmoId", *anum);

            return;
        }
    }
}

// [LIVE_WI_SAVELOAD] Der Cache wird bei jedem Waffenwechsel verworfen, aber
// NICHT beim Save-Load mit derselben Waffe -- genau dort wird das Item neu
// instanziiert und der Cache zur Leiche. Deshalb Save-Load an der ADRESSE des
// Player-Body-GO erkennen und den Cache VORHER wegwerfen, statt ihn hinterher
// zu befragen (ein Getter auf einer toten Instanz ist die Exception selbst).
void RE4VRReload4::tick_saveload_guard() {
    if (clock_now() < m_lw_next) {
        return;
    }

    m_lw_next = clock_now() + 0.25;

    auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");

    if (cm == nullptr) {
        return;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return;
    }

    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (body == nullptr) {
        return;
    }

    const auto a = reinterpret_cast<uintptr_t>(body);

    if (!m_lw_body_addr.has_value()) {
        m_lw_body_addr = a;

        return;
    }

    if (a == *m_lw_body_addr) {
        return;
    }

    m_lw_body_addr = a;
    m_live_wi = nullptr;
}

// [RUNDEN-RESET] Neue Mercenaries-Runde -> Waffenzustand wegwerfen. Das Spiel
// laedt die Map neu, die alten Objekte bleiben ansprechbar (Schreiben verpufft
// lautlos). `__re4_merc_round` ist der einzige verlaessliche Hinweis.
void RE4VRReload4::tick_merc_round() {
    const auto t = re4vr::lua_get_number_opt("__re4_merc_round");
    const auto tv = t.has_value() ? std::optional<int32_t>{static_cast<int32_t>(*t)}
                                  : std::nullopt;

    if (tv == m_round_seen) {
        return;
    }

    m_round_seen = tv;
    m_pe_cache = nullptr;

    // Ladezustand der letzten Runde: in der neuen ist die Waffe frisch.
    m_rack.needs = false;
    m_rack.empty = false;
    m_rack.empty_when_dropped = false;
    m_rack._zeroed_by_us = false;
    m_rack.empty_reload = false;
    m_rack.armed = false;
    m_rack.grab_active = false;
    m_rack.pulled = false;
    m_rack.pushed = false;

    // Die eine Referenz, an der der ganze Ladeweg haengt -- nach dem Neuladen
    // der Map ist sie eine Leiche.
    m_live_wi = nullptr;
}

// [DRYFIRE_WATCH] Die Munition liegt an ZWEI Stellen: im Inventar-Item und in
// der Runtime-Gun. Geschossen wird aus der Runtime-Gun. Schreibt ein Ladeweg
// nur das Item, zeigt alles "geladen", die Waffe feuert aber nicht.
// WARUM NICHT JEDEN FRAME: die Uebertragung geht nur Item -> Waffe. Beim Schuss
// zieht die Engine erst die Waffe runter und danach das Item; in diesem Fenster
// wuerde ein Sync den alten, hoeheren Item-Wert zurueckschreiben.
// DESHALB ALS WAECHTER: nur alle 0.5 s messen, und nur eingreifen, wenn beide
// Werte seit der letzten Messung UNVERAENDERT sind.
void RE4VRReload4::tick_dryfire_watch() {
    const double now = clock_now();

    if ((now - m_dfw_t) < 0.5) {
        return;
    }

    m_dfw_t = now;

    auto* p = pe();

    if (p == nullptr) {
        m_dfw_run.reset();
        m_dfw_item.reset();

        return;
    }

    const auto run = gun_ammo();
    auto* wi = get_live_weapon_item();
    const auto item = (wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount") : std::nullopt;

    if (!run.has_value() || !item.has_value()) {
        m_dfw_run = run;
        m_dfw_item = item;

        return;
    }

    if (*item > *run && m_dfw_run.has_value() && m_dfw_item.has_value()
        && *run == *m_dfw_run && *item == *m_dfw_item) {
        sync_gun_ammo();
    }

    m_dfw_run = run;
    m_dfw_item = item;
}

// ============================================================================
// HAUPT-TICK (Lua Z.5388-5753)
// ============================================================================
// [DLC LET-GO] Der Main-Teil darf die geteilten __vr_*-Globals jeden Frame auf
// false/nil schreiben, solange er die Waffe nicht verwaltet -- er ist das ERSTE
// Reload-Teil, und die spaeteren (reload2..5) ueberschreiben danach. DIESES Teil
// laeuft SPAETER, also gewinnen seine Writes. Wuerde es genauso blind nullen,
// killt es bei JEDER Maincampaign-Waffe die Ausgaben von reload/reload2/reload3
// (Revolver, Bolt, Armbrust, Chicago, RL, Red9 ...).
// Deshalb das Muster von reload3: freigeben NUR an der FLANKE
// verwaltet -> nicht-verwaltet, danach die Finger von den Globals lassen.
void RE4VRReload4::r4_release() {
    if (!m_r4_had) {
        return;   // wir hielten sie nie -> nichts anfassen
    }

    m_r4_had = false;
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", false);
    re4vr::lua_set_bool("__vr_block_fire_when_empty", false);
    re4vr::lua_set_string("__re4_bf_who", "reload4/CFG aus");
    re4vr::lua_set_bool("__vr_needs_rack", false);
    re4vr::lua_set_bool("__vr_rack_block_left_knife", false);
    re4vr::lua_set_nil("__vr_rack_hand_pose");
    re4vr::lua_set_bool("__re4_reload_grab_empty", false);
    re4vr::lua_set_bool("__vr_motion_paused", false);

    // [FIX] Die Mag-Hand-Pose gehoert hierher (Flanke), NICHT in
    // update_mag_in_hand: dort lief sie bei jeder fremden Waffe und hat Leons
    // Pose jeden Frame genullt.
    re4vr::lua_set_nil("__vr_mag_hand_pose");
    re4vr::lua_set_nil("__vr_mag_hand_trx");
    re4vr::lua_set_nil("__vr_mag_hand_try");
    re4vr::lua_set_nil("__vr_mag_hand_trz");

    // [STAGGER-RESTE] Diese vier wurden frueher NUR am Ende des Ticks gesetzt --
    // also nach dem Ausstieg, der bei jedem Verschwinden der Waffe greift, vor
    // allem im STAGGER (die Engine meldet ueber die ganze Damage-Phase keine
    // Waffe). Wer mitten im Durchladen oder mit Magazin in der Hand getroffen
    // wurde, liess sie auf `true` stehen: motion friert die Waffe weiter ein,
    // sperrt die Laengsachse und haelt die Stuetzhand vom Schaft fern.
    re4vr::lua_set_bool("__vr_slide_rack_active", false);
    re4vr::lua_set_bool("__vr_shotgun_pump_active", false);
    re4vr::lua_set_bool("__vr_mag_in_hand", false);
    re4vr::lua_set_bool("__vr_empty_reload_active", false);
}

void RE4VRReload4::on_frame() {
    // Reihenfolge wie in der Lua-Datei: die getakteten Teile stehen dort als
    // eigene on_frame-Callbacks VOR dem Haupt-Tick.
    tick_sortenfix();
    tick_saveload_guard();

    if (!cfg.enabled) {
        m_managed = false;
        r4_release();   // [DLC LET-GO] nur an der Flanke freigeben
        tick_merc_round();
        tick_dryfire_watch();
        update_fire_gate();

        return;
    }

    refresh_weapon();

    const auto hwid = handled();

    // Waffenwechsel / Unequip / Enemy-Grab -> kompletten Reload-State resetten
    if (hwid != m_last_handled_wid) {
        // [MAG_OUT / RACK-ZWANG / MAG_RETAIN PRO WAFFE] Zustand pro Waffe
        // sichern und wiederherstellen -- reset_reload_state loescht sonst bei
        // JEDEM kurzen Wechsel (Granate, Messer, Stagger) den Rack-Zwang, und
        // man koennte ohne Slide-Rack weiterfeuern.
        if (m_last_handled_wid.has_value()) {
            m_mag_out_store[*m_last_handled_wid] = m_mag_out;
            m_rack._needs_store[*m_last_handled_wid] = m_rack.needs;
            m_rack._retained_store[*m_last_handled_wid] = m_mag_retained;

            if (!hwid.has_value()) {
                m_rack._gone_wid = *m_last_handled_wid;   // Waffe nur WEG
            }
        }

        reset_reload_state();

        if (hwid.has_value() && m_rack._gone_wid.has_value() && *m_rack._gone_wid == *hwid) {
            // [STAGGER_MAGOUT] A -> nil -> A: die Engine hat in der Abwesenheit
            // selbst gechambert, also gilt das Mag als drin.
            m_mag_out = false;
            m_mag_out_store[*hwid] = false;
            m_mag_retained = 0;
            m_rack._retained_store.erase(*hwid);

            // [SPIEGEL MITLOESCHEN] Ein stehengebliebener Merker wuerde beim
            // naechsten Nachladen ein ZWEITES Mal gutgeschrieben -- und liesse
            // die Merk-Funktion beim naechsten Auswurf sofort aussteigen.
            if (!m_mag_carry_wid.has_value() || *m_mag_carry_wid == *hwid) {
                m_mag_carry.reset();
                m_mag_carry_wid.reset();
            }

            m_rack._gone_wid.reset();
        } else {
            m_mag_out = hwid.has_value() && m_mag_out_store.count(*hwid) > 0
                && m_mag_out_store.at(*hwid);
            m_mag_retained = 0;

            if (hwid.has_value()) {
                if (const auto it = m_rack._retained_store.find(*hwid);
                    it != m_rack._retained_store.end()) {
                    m_mag_retained = it->second;
                }

                m_rack._gone_wid.reset();
            }
        }

        m_rack.needs = false;

        if (hwid.has_value()) {
            if (const auto it = m_rack._needs_store.find(*hwid);
                it != m_rack._needs_store.end()) {
                m_rack.needs = it->second;
            }
        }

        m_last_handled_wid = hwid;
    }

    // [SAVE_LOAD] Gleiche Waffe, aber neu instanziiert -> stale Lua-State neu
    // initialisieren. Die Engine haelt nach dem Laden das Mag in der Kammer.
    if (m_weapon_reacquired) {
        m_weapon_reacquired = false;

        if (hwid.has_value()) {
            reset_reload_state();
            m_mag_out = false;

            // [REGAL LEEREN] Nach Save-Load gilt NICHTS von vorher: BEIDE
            // Regale komplett leeren -- sonst holt ein spaeterer Waffenwechsel
            // einen alten mag_out oder rack.needs zurueck.
            m_mag_out_store.clear();
            m_rack._needs_store.clear();
            m_rack._gone_wid.reset();
            m_rack._retained_store.clear();
            m_mag_retained = 0;
        }
    }

    // [DLC LET-GO] Waffe NICHT verwaltet: loslassen, aber NUR an der Flanke --
    // sonst wuerden wir die Ausgaben der frueheren Reload-Teile ueberschreiben.
    if (!hwid.has_value()) {
        m_managed = false;
        r4_release();
        tick_merc_round();
        tick_dryfire_watch();

        return;
    }

    m_managed = true;
    // [DLC LET-GO] wir halten die Globals gerade -> beim Loslassen einmal frei
    m_r4_had = true;

    // (Luas RED9_NATIVE_AMMO-Block steht hier -- er greift nur fuer wid 4002,
    // und die ist nach reload2 ausgezogen: `handled()` liefert sie nie.)

    capture_mag_rest();              // Chamber-Ruhepose laufend erfassen
    check_mag_insert_proximity();    // Mag nah an der Waffe -> auto-insert
    update_shell_spawn();

    // Mag-Joint ans Advanced-Modul reichen (fuer die Live-Preview im UI)
    if (m_adv != nullptr) {
        m_adv->set_current_mag_joint(m_wep.mag_joint);
    }

    // Self-Heal: haengender/alter Drop bei Equip-Wechsel ODER ungueltigem Joint
    if (m_drop.active && !m_drop.use_module) {
        if (m_drop.joint != m_wep.mag_joint || !re4vr::obj_ok(m_drop.joint)) {
            stop_mag_drop();
        }
    }

    // verwaltete Waffe -> das Binding soll den rechten B abfangen (kein nativer
    // Reload). (Luas TOP_LOADER-Ausnahme ist heute wirkungslos.)
    const bool on = true;
    re4vr::lua_set_bool("__vr_manual_reload_consume_b", on);

    // [MAG_OUT] KEIN Heilen ueber loaded>0! "Mag physisch draussen" darf NICHT
    // am Ammo-Zaehler haengen: mit Infinite Ammo ist loaded nach dem Drop sofort
    // wieder >0 -> das hat mag_out faelschlich geloest.
    // UI auf 0 HALTEN, solange das Mag draussen ist. NICHT waehrend des
    // Inserts, sonst wuerde der Fuell-Wert sofort genullt.
    if (m_mag_out && !m_mag_insert.active) {
        auto* wi = get_live_weapon_item();

        if (wi != nullptr && call_enum(wi, "get_CurrentAmmoCount").value_or(0) > 0) {
            carry_capture(wi, "re4_vr_reload4_dlc.lua:3584", std::nullopt);

            if (auto* ptr = field_i32(wi, "_CurrentAmmoCount", 0x44); ptr != nullptr) {
                *ptr = 0;
            }
        }
    }

    // [MAG_AVAIL] Fuer den Holster: gibt es ueberhaupt etwas zu greifen?
    {
        const int32_t wid = m_wep.wid.value_or(0);

        if (is_shotgun(wid)) {
            // Sperr-Puls wenn Reserve leer ODER Roehre voll.
            auto* wifull = get_live_weapon_item();
            const int32_t lo = (wifull != nullptr)
                ? call_enum(wifull, "get_CurrentAmmoCount").value_or(0) : 0;
            const int32_t cp = (wifull != nullptr)
                ? call_enum(wifull, "get_CurrentAmmoMax").value_or(0) : 0;
            const bool full = (cp > 0 && lo >= cp);

            // [STRIKER] Cycle ausstehend -> Holster gesperrt (erst drehen).
            const bool rotary_block = rotary_cycle(wid) && m_rack.needs;
            // [SKULL SHAKER] gesperrt, solange der Hebel ZU ist.
            const bool break_block = break_action(wid)
                && re4vr::lua_get_tribool("__vr_break_open") != 1;

            re4vr::lua_set_bool("__re4_reload_grab_empty",
                                !m_mag_hand.active && !m_mag_insert.active
                                && (current_reserve() <= 0 || full || rotary_block
                                    || break_block));
        } else {
            bool avail = false;

            if (m_mag_out && !m_mag_hand.active && !m_mag_insert.active) {
                int32_t reserve = 0;
                auto* wi = get_live_weapon_item();
                const auto ammo_id = (wi != nullptr)
                    ? call_enum(wi, "get_CurrentAmmo") : std::nullopt;
                auto* p2 = pe();
                auto* inv2 = (p2 != nullptr)
                    ? re4vr::call_safe<::REManagedObject*>(p2, "get_InventoryController")
                    : nullptr;

                if (inv2 != nullptr && ammo_id.has_value()) {
                    reserve = item_count_sum(inv2, *ammo_id);
                }

                avail = (m_mag_retained + reserve) > 0;
            }

            re4vr::lua_set_bool("__re4_reload_grab_empty",
                                m_mag_out && !m_mag_hand.active && !m_mag_insert.active
                                && !avail);
        }
    }

    // Slide-Rack: Empty-Block pflegen + Rack-Geste auswerten
    update_rack_state();

    // [SLIDE-STAGGER-GUARD] Wird man mitten im Slide-Rack getroffen, bleibt der
    // Slide irgendwo hinten stehen. Der Killswitch stempelt das Damage-ENDE --
    // diese Funktion laeuft waehrend des Staggers gar nicht (die Engine meldet
    // keine Waffe), kann die Flanke also nicht selbst sehen.
    // WICHTIG: NICHT clear_rack rufen! Das chambert intern und nimmt den
    // Rack-Zwang mit. Hier NUR die Slide-Position korrigieren, und auch das nur,
    // wenn KEIN Rack aussteht.
    {
        const auto det = re4vr::lua_get_number_opt("__re4_damage_end_t");

        if (det.has_value()
            && (!m_rack._heal_done_t.has_value() || *m_rack._heal_done_t != *det)
            && (clock_now() - *det) < 3.0 && !m_rack.needs && !m_rack.empty
            && !m_mag_out && m_wep.slide_joint != nullptr) {
            m_rack._heal_done_t = *det;

            const auto& sp = slide_pose(m_wep.wid.value_or(0));
            glm::vec3 cur{};

            if (get_vec3(m_wep.slide_joint, "get_LocalPosition", cur)) {
                set_vec3(m_wep.slide_joint, "set_LocalPosition",
                         glm::vec3{cur.x, cur.y, sp.rest_z});
            }

            m_rack.grab_active = false;
            m_rack.pulled = false;
            m_rack.pushed = false;
            m_rack.frac = 0.0f;

            // [HALTEN] Ein einmaliger Snap reicht NICHT: apply_slide_park laesst
            // den Slide danach wieder los und die Engine zieht ihn erneut nach
            // hinten. _chambered_hold haelt ihn bis zum ersten Schuss.
            m_rack._chambered_hold = true;
        }
    }

    service_haptics();
    update_rack_gesture();
    update_rotary_cycle();
    update_break_action();
    break_cock_flick();          // [SKULL_FLICK] VOR dem Fenster-Update
    update_break_pump_sound();

    // [IK-GATE] AUTORITATIV jeden Frame publizieren -- update_rack_state hat
    // fruehe returns, dort konnte das Global auf true haengen bleiben.
    const int32_t wid_now = m_wep.wid.value_or(0);
    re4vr::lua_set_bool("__vr_shotgun_pump_active",
                        m_rack.grab_active && is_shotgun(wid_now));

    // [SKULL_FLICK] Break-Action-Gate fuer die Ruhepose in motion. AUTORITATIV
    // hier und NUR hier. LIVE-wid statt wep.wid (die ueberlebt einen Save-Load
    // stale), und ein ZEITSTEMPEL statt true/false: laeuft dieser Tick nicht
    // mehr (Script tot, Killswitch, bare hands), ist das Gate nach 0.3 s von
    // selbst zu und motion faellt auf die native Hand zurueck.
    if (break_action(get_equip_wid().value_or(0))) {
        re4vr::lua_set_number("__re4_break_wid_active", clock_now());
    } else {
        re4vr::lua_set_nil("__re4_break_wid_active");
    }

    // [SKULL_INDEX] Zeigefinger-Beugung der Ruhepose an motion.
    re4vr::lua_set_number("__re4_skull_idx1", cfg.skull_idx1);
    re4vr::lua_set_number("__re4_skull_idx2", cfg.skull_idx2);
    re4vr::lua_set_number("__re4_skull_idx3", cfg.skull_idx3);

    re4vr::lua_set_bool("__vr_slide_rack_active", m_rack.grab_active);
    // [EMPTY-RELOAD RACK] Riot-Gun-_08-Zug: die Gun NICHT einfrieren -> sie
    // folgt der Hand, statt starr zu stehen.
    re4vr::lua_set_bool("__vr_empty_reload_active", m_rack.empty_reload);

    // [MAG-DOCK-LOCK] AUTORITATIV: die linke Hand haelt gerade Mag/Shell (oder
    // ein Insert laeuft, oder < 0.2 s nach dem letzten Ammo-Input) -> die
    // Support-Hand darf NICHT an den Schaft docken.
    re4vr::lua_set_bool("__vr_mag_in_hand",
                        m_mag_hand.active || m_mag_insert.active
                        || (m_rack._ammo_input_t.has_value()
                            && (clock_now() - *m_rack._ammo_input_t) < 0.2));

    // [SHELL-KEYFRAMES] Shell-Joint + Waffen-Transform + wid fuer die
    // Keyframe-UI in reload_adv exponieren (jeden Frame, damit die Vorschau auch
    // ohne laufenden Insert greift).
    re4vr::lua_set_managed_object("__re4_reload_shell_joint", m_wep.mag_joint);
    re4vr::lua_set_managed_object("__re4_reload_weapon_tf", m_wep.tf);
    m_reload_ui_wid = m_wep.wid;

    if (m_wep.wid.has_value()) {
        re4vr::lua_set_number("__re4_reload_ui_wid", *m_wep.wid);
    } else {
        re4vr::lua_set_nil("__re4_reload_ui_wid");
    }

    update_shell_eject();   // nach rack.pulled

    // [MANUAL_INSERT] SICHERUNG: das Halten der Druecken-Pose darf NIE einen
    // laufenden Handschub ueberleben. Den Insert brechen mehrere fremde Wege ab
    // (Mag fallen lassen, neuer Grab, Waffenwechsel, Save-Load) -- die kennen
    // den Push nicht.
    if (m_adv != nullptr && m_adv->push_hold
        && !(m_mag_insert.active && m_mag_insert.manual)) {
        m_adv->end_push_hold();
    }

    // [MANUAL_INSERT] Rueckzieher OHNE gehaltenen Grip -> das Mag faellt.
    if (m_mag_hand.want_drop) {
        m_mag_hand.want_drop = false;
        m_mag_hand.redock_d.reset();
        m_mag_hand.redock_pending = false;

        if (!m_mag_hand.active && !m_drop.active) {
            drop_mag_simple();
        }
    }

    update_dock_blend();   // [DOCK_LERP] 1x/Frame rampen (vor publish_dock)
    publish_dock();

    // [SHELL_PART] enabled-Part-Set bei loaded>0 merken + bei 0 den Diff bilden.
    if (empty_reload_joint(wid_now) != nullptr) {
        auto* wi = get_live_weapon_item();
        sg_update_parts((wi != nullptr) ? call_enum(wi, "get_CurrentAmmoCount")
                                        : std::nullopt);
    }

    // [SND] Mag-Boden-Sound verzoegert: bei Drop-START Timer setzen.
    if (m_drop.active && !m_drop_prev) {
        m_mag_floor_at = clock_now() + cfg.mag_floor_delay;
    }

    m_drop_prev = m_drop.active;

    if (m_mag_floor_at > 0.0 && clock_now() >= m_mag_floor_at) {
        m_mag_floor_at = 0.0;
        play_weapon_sound(snd_id(wid_now, "mag_floor"));
    }

    // [SND] Dry-Fire: leere/gesperrte Waffe + Schuss-Trigger gezogen.
    const bool et = re4vr::lua_get_tribool("__re4_empty_trigger_held") == 1;

    if (et && !m_empty_trig_prev) {
        play_weapon_sound(snd_id(wid_now, "dry_fire"));
    }

    m_empty_trig_prev = et;

    // B-Flanke -> ROBUSTER Mag-Auswurf.
    const bool bd = right_b_down();
    // [MAG_B_GUARD 2026-09-10] Nur Karte, Typewriter und Inventar: dort ist der
    // rechte B Zentrieren bzw. Zurueck und darf die Waffe nicht anfassen.
    // Geblockt wird nur die AKTION: die Flanke wird weiter gepflegt, sonst
    // feuert ein beim Schliessen noch gehaltener B sofort den Auswurf.
    const bool b_menu = re4vr::lua_get_tribool("__re4_mag_block") == 1;

    if (on && bd && !m_b_prev && !b_menu) {
        // [SHOTGUN] Kein B-Eject: die Shotgun wirft kein Mag aus, das Pumpen
        // macht die linke Hand. B bleibt fuer Shotguns wirkungslos.
        if (is_shotgun(wid_now)) {
            // nichts
        } else {
            // (Luas CHAMBER-Zweig ist unerreichbar -- die Tabelle ist leer.)
            force_eject();
        }
    }

    m_b_prev = bd;

    tick_merc_round();
    tick_dryfire_watch();
    update_fire_gate();
}

// ============================================================================
// Hooks (Lua Z.2295-2359, 2563-2588)
// ============================================================================
void RE4VRReload4::install_hooks() {
    // Das ECHTE Gun-WeaponItem cachen: reduceAmmoCount (Schuss) und
    // addAmmoCount (Reload) laufen beide auf dem Live-Item.
    if (auto* td = sdk::find_type_definition("chainsaw.WeaponItem"); td != nullptr) {
        for (const char* name : {"reduceAmmoCount", "addAmmoCount"}) {
            auto* m = td->get_method(name);

            if (m == nullptr) {
                continue;
            }

            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload4::instance();

                    if (s != nullptr && args.size() >= 2) {
                        // [CACHE-TTL] Zeitstempel mitschreiben: das gecachte
                        // WeaponItem darf nur kurz nach dem Hook benutzt werden.
                        // Danach kann es eine tote Instanz sein, und schon ein
                        // Getter darauf ist ein Call ins Leere.
                        s->m_live_wi = reinterpret_cast<::REManagedObject*>(args[1]);
                        s->m_live_wi_t = clock_now();
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }

        // RE-ACQUIRE OHNE SCHUSS: die HUD liest get_CurrentAmmoCount jeden Frame
        // auf dem ECHTEN Laufzeit-Item.
        if (auto* gca = td->get_method("get_CurrentAmmoCount"); gca != nullptr) {
            g_hookman.add(
                gca,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload4::instance();

                    if (s == nullptr || args.size() < 2) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    // [CACHE WAR TOT, NICHT LEER] Hier stand ein Early-Return
                    // auf `~= nil`. Eine tote Instanz ist nicht nil -> der
                    // Return griff, die Auffrischung lief nie, und der Ladeweg
                    // schrieb dauerhaft in ein totes Objekt. Die Leiche selbst
                    // wird NICHT angefasst -- der Cache wird zeitgesteuert
                    // erneuert: hoechstens 10x pro Sekunde.
                    const double now = clock_now();

                    if (s->m_live_wi_t > now - 0.1) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    auto* wi = reinterpret_cast<::REManagedObject*>(args[1]);

                    if (!re4vr::obj_ok(wi)) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const auto cwid = call_enum(wi, "get_WeaponId");
                    const auto ewid = s->get_equip_wid();

                    if (cwid.has_value() && ewid.has_value() && *cwid == *ewid) {
                        s->m_live_wi = wi;
                        s->m_live_wi_t = now;
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // [SG PUMP MUTE] Die Shotgun-Engine spielt nach JEDEM Schuss automatisch den
    // Pump-Cock-Sound, obwohl wir die Pump-Bewegung unterdruecken.
    // soundlib.SoundManager.postRequestInfo ist der globale Sound-Funnel: ALLE
    // Sounds laufen hier durch (auch Motion-SE, die am Weapon-SoundContainer
    // vorbeigehen -- deshalb sah der Container-Hook den Pump nie).
    // RequestInfo erbt von SoundTriggerInfo -> _TriggerId@0x10, _EventId@0x14.
    if (auto* td = sdk::find_type_definition("soundlib.SoundManager"); td != nullptr) {
        auto* m = td->get_method("postRequestInfo(soundlib.SoundManager.RequestInfo)");

        if (m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) {
                    auto* s = RE4VRReload4::instance();

                    if (s == nullptr || args.size() < 3) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const int32_t wid = s->m_wep.wid.value_or(0);

                    if (!is_shotgun(wid)) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const auto mute_id = auto_pump_mute(wid);

                    if (!mute_id.has_value() || s->m_sg_our_sound) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    auto* info = reinterpret_cast<::REManagedObject*>(args[2]);

                    if (!re4vr::obj_ok(info)) {
                        return HookManager::PreHookResult::CALL_ORIGINAL;
                    }

                    const auto base = reinterpret_cast<uintptr_t>(info);
                    const uint32_t tid = *reinterpret_cast<uint32_t*>(base + 0x10);
                    const uint32_t eid = *reinterpret_cast<uint32_t*>(base + 0x14);

                    if (tid == *mute_id || eid == *mute_id) {
                        return HookManager::PreHookResult::SKIP_ORIGINAL;
                    }

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                // postRequestInfo gibt void zurueck -- nichts zu setzen.
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }
    // ========================================================================
    // [FIRE-GATE 2026-07-20 -- Ursache per Log+Live-Abfrage belegt]
    // PROBLEM: Unser Feuer-Block (f.RT weglassen) erreicht die Engine NICHT. Das
    // VR-Framework reicht den echten Controller-Trigger zusaetzlich direkt
    // weiter -- belegt im Log: "SHOT von_uns=false block_fire=true", also Schuss
    // OHNE unser Gamepad. Bei den meisten Waffen faellt das nicht auf, weil die
    // Engine sie nach dem 0-Reload als leer fuehrt und dann von sich aus nicht
    // feuert. Bei Adas Blacktail AC chambert sie dagegen selbst (gemessen:
    // getCurrentGunAmmo=0 UND isGunAmmoEmpty=false) -> die echte Bremse faellt
    // weg und nur unsere wirkungslose bleibt.
    //
    // LOESUNG: Nicht den Trigger abfangen, sondern der Engine IHRE EIGENE Frage
    // beantworten. isEnableFire ist die Pruefung "darf jetzt gefeuert werden?".
    // Solange ein Rack aussteht, antworten wir false -- damit ist der
    // Eingabeweg egal.
    //
    // ABSICHERUNG siehe update_fire_gate(); dazu der ZEITSTEMPEL: tickt on_frame
    // nicht mehr, loest sich die Sperre nach 0.3 s von selbst auf (eine
    // haengende Sperre waere eine tote Waffe).
    // ========================================================================
    if (auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment"); td != nullptr) {
        if (auto* m = td->get_method("isEnableFire"); m != nullptr) {
            g_hookman.add(
                m,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&,
                   uintptr_t) { return HookManager::PreHookResult::CALL_ORIGINAL; },
                [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
                    auto* s = RE4VRReload4::instance();

                    if (s == nullptr || !s->m_fg_block) {
                        return;
                    }

                    // alt -> die Sperre loest sich
                    if ((clock_now() - s->m_fg_t) >= 0.3) {
                        return;
                    }

                    // false -> Engine feuert nicht, spielt ihren eigenen Dry-Fire
                    ret = 0;
                });
        }
    }
}

// Die Bedingungen des Fire-Gates -- EINMAL pro Frame, weil der Hook nicht in den
// Lua-State greifen darf (isEnableFire feuert sehr oft). Mehrfach abgesichert,
// weil eine haengende Sperre = tote Waffe = gamebreaking:
// * nur wenn dieses Modul die Waffe verwaltet (m_managed)
// * nur wenn wirklich ein Rack aussteht (m_rack.needs)
// * nicht im Killswitch/KS4, nicht waehrend Mag-Flow (Drop/Hand/Insert)
// * Not-Aus jederzeit: _G.__re4_fire_gate = false
void RE4VRReload4::update_fire_gate() {
    m_fg_block = false;

    if (re4vr::lua_get_tribool("__re4_fire_gate") != 1) {
        return;
    }

    if (!m_managed || !m_rack.needs) {
        return;
    }

    if (re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
        || re4vr::lua_get_tribool("__re4_ks4_active") == 1) {
        return;
    }

    if (m_drop.active || m_mag_hand.active || m_mag_insert.active) {
        return;
    }

    m_fg_block = true;
    m_fg_t = clock_now();
}

// ============================================================================
// Lua-Zustand
// ============================================================================
void RE4VRReload4::on_lua_state_created() {
    // Diese Funktions-Globals lesen FREMDE Scripte (motion, merc, holster,
    // knife_lefthand) -- sie bleiben als Lua-Export bestehen.
    re4vr::LuaRef lua{};

    if (lua == nullptr) {
        return;
    }

    // reload4 setzt die geteilten Funktions-Globals NICHT: in Lua stehen sie
    // dort hinter einem `or`, der erste Setzer (reload) gewinnt also -- und der
    // ist der Main-Teil. Hier bleibt nur die EIGENE Pose-Liste, die reload4
    // zusaetzlich exportiert.
    (*lua)["__re4_reload4_pose_names"] = []() -> std::vector<std::string> {
        auto* s = RE4VRReload4::instance();

        return (s != nullptr) ? s->pose_names() : std::vector<std::string>{};
    };

    // [FIRE-GATE] Not-Aus-Schalter anlegen: _G.__re4_fire_gate = false schaltet
    // den isEnableFire-Riegel jederzeit ab.
    (*lua)["__re4_fire_gate"] = true;

    // Beim Script-Reload keinen stale Feuer-Block hinterlassen.
    (*lua)["__vr_block_fire_when_empty"] = false;
    (*lua)["__re4_bf_who"] = "reload4/Script-Start";
    (*lua)["__vr_needs_rack"] = false;
    (*lua)["__vr_shotgun_pump_active"] = false;
    (*lua)["__vr_rack_block_left_knife"] = false;
}

void RE4VRReload4::on_lua_state_destroyed() {
    // Die Engine-Objekte in unseren Caches ueberleben den Lua-Reset nicht
    // zwingend -- alles verwerfen, was neu aufgeloest werden kann.
    m_pmap_tf = nullptr;
    m_pmap.clear();
    m_live_wi = nullptr;
    m_live_wi_t = -1.0;
    m_lhand_joint = nullptr;
    m_thumb_joint = nullptr;
    m_pe_cache = nullptr;
    m_character_manager = nullptr;
    m_wep = Wep{};
    m_sg = SgParts{};
    m_ssc = SsClone{};
    m_spawn = Spawn{};
}

// ============================================================================
// UI (Lua Z.6210-6905)
// ============================================================================
namespace {

// Luas category_row: Checkbox + gruenes "Enable" + Label.
bool ui_category_row(const char* label, const char* key, bool& value) {
    const std::string id = std::string{"##mr_"} + key;
    const bool changed = ImGui::Checkbox(id.c_str(), &value);
    ImGui::SameLine();
    ImGui::TextColored(ImColor{0xFF00FF00}, "Enable");
    ImGui::SameLine();
    ImGui::Text("%s", label);

    return changed;
}

// Der Joint-Finder-Block taucht in drei Trees identisch auf.
struct FinderIds {
    const char* active;
    const char* idx;
    const char* prev;
    const char* next;
    const char* ox;
    const char* oy;
    const char* oz;
};

}   // namespace

void RE4VRReload4::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Reload4 (Separate Ways)")) {
        return;
    }

    const int32_t cur_wid = m_wep.wid.value_or(0);

    ImGui::Text("Equippt: %s   %s%s", wp_label(cur_wid).c_str(),
                m_drop.active ? "[MAG GEDROPPT] " : "",
                m_rack.needs ? "[SLIDE RACK NOETIG - RT GESPERRT]" : "");
    ImGui::Separator();

    // [NO_NATIVE_RELOAD] Die Zaehler zeigen, ob der Ersatzweg greift: "geladen"
    // muss hochzaehlen, "fehlt" muss 0 bleiben. Zaehlt "fehlt" hoch, laedt eine
    // Waffe nicht mehr -- dann melden, NICHT den nativen Call zurueckholen.
    ImGui::TextColored(ImColor{0xFF66CCFF},
                       "Reload-Ladeweg:  geladen=%d   fehlt=%d   (nativer Reload ausgebaut)",
                       m_reload_direct_ok, m_reload_direct_fail);
    ImGui::Separator();

    ImGui::TextColored(ImColor{0xFF00FFFF}, "Weapon Options:");

    if (ImGui::TreeNode("Sounds (Mag / Slide / Dry-Fire)")) {
        if (ImGui::Checkbox("Waffen-Sounds an", &cfg.sound_enabled)) { save_cfg(); }
        if (ImGui::SliderFloat("Mag-Boden Delay (s)", &cfg.mag_floor_delay, 0.0f, 2.0f)) {
            save_cfg();
        }

        ImGui::TreePop();
    }

    if (ui_category_row("Manual Pistol Reload", "pistols_enabled", cfg.pistols_enabled)) {
        save_cfg();
    }

    if (ImGui::TreeNode("Pistolen - Einstellungen")) {
        const int32_t sw = (cur_wid != 0) ? cur_wid : 4004;
        auto& sp = slide_pose(sw);
        auto& m = maghand(sw);

        if (ImGui::TreeNode("Status (live)")) {
            ImGui::Text("Waffe: %s", wp_label(sw).c_str());

            std::string st;

            if (!m_rack.needs) {
                st = "bereit";
            } else if (m_rack.grab_active) {
                char b[64]{};
                std::snprintf(b, sizeof(b), "ziehen... %.0f%%", m_rack.frac * 100.0f);
                st = b;
            } else if (m_rack.has_mag) {
                st = "Mag drin -> linken Grip an den Slide + zurueckziehen";
            } else {
                st = "leer -> Mag laden";
            }

            ImGui::TextColored(ImColor{0xFFFFCC00}, "Slide-Rack: %s", st.c_str());
            ImGui::Text("Hand->Slide: %.3f m  (Grab <= %.3f)",
                        (m_rack._last_dist >= 0.0f) ? m_rack._last_dist : 0.0f,
                        cfg.rack_grab_dist);
            ImGui::Text("Hand->Waffe: %.3f m %s", m_mag_hand.dist,
                        m_mag_hand.active ? "[Mag in Hand]" : "");
            ImGui::TreePop();
        }

        ImGui::Separator();
        ImGui::TextColored(ImColor{0xFF00FFFF}, "======  EINSTELLUNG MAGAZIN  ======");

        if (ImGui::TreeNode("Mag-Drop & Insert")) {
            if (ImGui::SliderFloat("Gravitation (Fall)", &cfg.gravity, 1.0f, 30.0f)) {
                save_cfg();
            }

            // [MASTER-TEMPO] EIN Regler fuers gesamte Reinladen (Mag-Slide +
            // Push-Geste). Liegt in reload_adv, damit beide Phasen ihn teilen.
            if (m_adv != nullptr) {
                if (ImGui::SliderFloat("Reinlade-Tempo (klein = langsamer)",
                                       &m_adv->push.reload_speed, 0.3f, 2.0f, "%.2f")) {
                    m_adv->save_cfg();
                }
            }

            if (ImGui::SliderFloat("Insert-Dauer s (Slide-in)", &cfg.insert_dur, 0.05f, 1.0f)) {
                save_cfg();
            }

            if (ImGui::Checkbox("Mag von Hand hochschieben (alle Waffen mit Druecken-Geste)",
                                &cfg.insert_manual)) {
                save_cfg();
            }

            if (cfg.insert_manual) {
                if (ImGui::SliderFloat("  Handweg m (Andocken -> eingerastet)",
                                       &cfg.insert_travel, 0.02f, 0.30f, "%.3f")) {
                    save_cfg();
                }

                if (ImGui::SliderFloat("  Selbst-Einrasten ab Anteil (1.00 = aus)",
                                       &cfg.insert_snap_at, 0.50f, 1.00f, "%.2f")) {
                    save_cfg();
                }

                if (ImGui::SliderFloat("  Rueckweg m (dann wieder in die Hand)",
                                       &cfg.insert_back_out, 0.005f, 0.10f, "%.3f")) {
                    save_cfg();
                }

                if (ImGui::SliderFloat("  Wiederandocken erst nach m Vorschub",
                                       &cfg.insert_redock, 0.002f, 0.05f, "%.3f")) {
                    save_cfg();
                }

                ImGui::TextColored(ImColor{0xFF888888},
                                   "   Einrast-Sound: kommt beim echten Einrasten (keine Schwelle)");
                // KEIN Prozentzeichen im Text: der Formatstring wuerde sonst ein
                // Argument erwarten -- bekannte Falle.
                ImGui::TextColored(ImColor{0xFFFFCC00}, "   Schub: %.0f Prozent%s",
                                   (m_mag_insert.manual ? m_mag_insert.prog : 0.0f) * 100.0f,
                                   m_mag_insert.manual ? " [Handschub laeuft]" : "");
            }

            if (ImGui::Checkbox("Reinhaemmern statt reingleiten (Ease-in)", &cfg.insert_punch)) {
                save_cfg();
            }

            if (cfg.insert_punch) {
                if (ImGui::SliderFloat("  Overshoot m (wie tief drueber hinaus)",
                                       &cfg.insert_overshoot, 0.0f, 0.02f)) {
                    save_cfg();
                }

                if (ImGui::SliderFloat("  Nachfedern s (zurueck in die Ruhelage)",
                                       &cfg.insert_settle, 0.02f, 0.30f)) {
                    save_cfg();
                }
            }

            if (ImGui::SliderFloat("Einrast-Haptik (0 = aus)", &cfg.insert_haptic, 0.0f, 1.0f)) {
                save_cfg();
            }

            if (ImGui::SliderFloat("Einrast-Sound frueher/spaeter (Anteil der Einschub-Phase)",
                                   &cfg.insert_snd_at, 0.50f, 1.00f)) {
                save_cfg();
            }

            // [MATILDA-EINLEIT-PUNKT] Y/Z NUR fuer die Matilda. Schreibt direkt
            // in ihren Eintrag in reload_adv und speichert dort.
            if (m_adv != nullptr) {
                auto& d = m_adv->dock_or_create(4004);
                ImGui::Text("-- Matilda Einleit-Punkt (Joint %s) --", d.joint.c_str());

                if (ImGui::SliderFloat("  Matilda Y (Hoehe)##maty", &d.y, -0.30f, 0.30f, "%.4f")) {
                    m_adv->save_cfg();
                }

                if (ImGui::SliderFloat("  Matilda Z (Tiefe)##matz", &d.z, -0.30f, 0.30f, "%.4f")) {
                    m_adv->save_cfg();
                }
            }

            // [INSERT-DIST PRO WAFFE] eigener Wert pro Waffe.
            {
                float pcur = m_insert_dist["pistols"];

                if (const auto it = m_insert_dist_wid.find(sw); it != m_insert_dist_wid.end()) {
                    pcur = it->second;
                }

                char lbl[96]{};
                std::snprintf(lbl, sizeof(lbl), "Insert-Distanz m (Snap, %s)",
                              wp_label(sw).c_str());

                if (ImGui::SliderFloat(lbl, &pcur, 0.03f, 0.50f)) {
                    m_insert_dist_wid[sw] = pcur;
                    save_cfg();
                }
            }

            if (ImGui::Checkbox("Ammo beim Insert nachladen (wie Game)", &cfg.reload_ammo)) {
                save_cfg();
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Mag-in-Hand Pose (Holster -> Waffe)")) {
            ImGui::Text("Waffe: %s", wp_label(sw).c_str());
            ImGui::TextColored(ImColor{0xFF00FF00}, "Aktive Pose: %s",
                               cfg.mag_hold_pose.empty() ? "(keine)"
                                                         : cfg.mag_hold_pose.c_str());
            ImGui::Spacing();
            ImGui::Text("Pose waehlen:");

            for (const auto& n : pose_names()) {
                const std::string b = "Setze Pose: " + n;

                if (ImGui::Button(b.c_str())) {
                    cfg.mag_hold_pose = n;
                    save_cfg();
                }
            }

            if (!cfg.mag_hold_pose.empty()
                && ImGui::Button("Pose entfernen (Finger frei zum Aufnehmen)")) {
                cfg.mag_hold_pose.clear();
                save_cfg();
            }

            {
                char buf[64]{};
                std::snprintf(buf, sizeof(buf), "%s", cfg.mag_hold_pose.c_str());

                if (ImGui::InputText("Pose-Name (manuell)", buf, sizeof(buf))) {
                    cfg.mag_hold_pose = buf;
                    save_cfg();
                }
            }

            ImGui::Spacing();

            if (ImGui::TreeNode("Mag-Position in der Hand (Offset, pro Waffe)")) {
                ImGui::Checkbox("Mag in Hand halten (Tuning)", &m_mag_tune.active);

                bool ch = false;
                ch |= ImGui::SliderFloat("Mag X", &m.x, -0.2f, 0.2f);
                ch |= ImGui::SliderFloat("Mag Y", &m.y, -0.2f, 0.2f);
                ch |= ImGui::SliderFloat("Mag Z", &m.z, -0.2f, 0.2f);
                ch |= ImGui::SliderFloat("Mag RotX", &m.rx, -180.0f, 180.0f);
                ch |= ImGui::SliderFloat("Mag RotY", &m.ry, -180.0f, 180.0f);
                ch |= ImGui::SliderFloat("Mag RotZ", &m.rz, -180.0f, 180.0f);

                if (ch) {
                    save_cfg();
                }

                ImGui::Text("Mag-Daumen tunen (additiv auf die Pose, pro Waffe, Grad):");

                bool tc = false;
                tc |= ImGui::SliderFloat("Mag-Daumen RotX", &m.t_rx, -90.0f, 90.0f);
                tc |= ImGui::SliderFloat("Mag-Daumen RotY", &m.t_ry, -90.0f, 90.0f);
                tc |= ImGui::SliderFloat("Mag-Daumen RotZ", &m.t_rz, -90.0f, 90.0f);

                if (tc) {
                    save_cfg();
                }

                ImGui::Spacing();
                ImGui::Text("Offset von anderer Waffe kopieren:");

                for (int32_t src : {4000, 4001, 4002, 4003, 4004, 6000, 4200, 4201, 4202, 6300}) {
                    if (src == sw || m_maghand.count(src) == 0) {
                        continue;
                    }

                    char b[96]{};
                    std::snprintf(b, sizeof(b), "Kopiere von %s", wp_label(src).c_str());

                    if (ImGui::Button(b)) {
                        m_maghand[sw] = m_maghand[src];
                        save_cfg();
                    }
                }

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        ImGui::Separator();
        ImGui::TextColored(ImColor{0xFFFFCC00}, "======  EINSTELLUNG SLIDE  ======");

        if (ImGui::TreeNode("Slide-Rack (Greif-Distanz + Positionen)")) {
            ImGui::Text("Waffe: %s", wp_label(sw).c_str());

            if (ImGui::SliderFloat("Slide-Grab-Distanz m (left grip)",
                                   &cfg.rack_grab_dist, 0.05f, 0.80f)) {
                save_cfg();
            }

            // [RACK_POSE_ANGLE] NUR Pistolen: Umschaltwinkel zwischen den beiden
            // Rack-Handposen.
            if (ImGui::SliderFloat("Pose-Umschaltwinkel Grad (Pistolen: hinten<->seitlich)",
                                   &cfg.rack_pose_side_deg, 5.0f, 85.0f, "%.0f")) {
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888},
                               "   hinten = \"%s\"   seitlich = \"%s\"   |   letzter Grab: %.0f Grad -> %s",
                               cfg.rack_pose.c_str(), cfg.rack_pose_side.c_str(),
                               m_rack._pose_ang.value_or(0.0f),
                               m_rack.pose_side ? "seitlich" : "hinten");
            ImGui::Spacing();
            ImGui::Text("Slide-Positionen (lokal Z):");

            bool sz = false;
            sz |= ImGui::DragFloat("ZU (rest_z / vorne, gechambert)##slz", &sp.rest_z,
                                   0.001f, -0.5f, 0.5f, "%.5f");
            sz |= ImGui::DragFloat("MITTEL (park_z / leer)##slz", &sp.park_z,
                                   0.001f, -0.5f, 0.5f, "%.5f");
            sz |= ImGui::DragFloat("GANZ AUF (back_z / Rack-Ende)##slz", &sp.back_z,
                                   0.001f, -0.5f, 0.5f, "%.5f");
            ImGui::Checkbox("Vorschau: Slide-Pos per Regler##slz", &m_rack.tuning);

            if (m_rack.tuning) {
                ImGui::SliderFloat("Vorschau park->back##slz", &m_rack.tune_frac, 0.0f, 1.0f);
            }

            if (sz) {
                save_cfg();
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Slide-Grab Hand-Pose (steif am Slide)")) {
            ImGui::Text("Waffe: %s", wp_label(sw).c_str());
            ImGui::Checkbox("Hand-Pose einstellen (Slide-Dock erzwingen)", &m_rack.dock_tune);

            if (m_rack.dock_tune) {
                ImGui::TextColored(ImColor{0xFF00FF00},
                                   "  EINSTELLMODUS AKTIV - Hand sitzt am Slide");
            }

            const char* cat = category_of(cur_wid);
            const bool is_pist = (cat != nullptr && std::strcmp(cat, "pistols") == 0);
            ImGui::TextColored(ImColor{0xFF888888}, "   aktive Pose: %s%s",
                               m_rack.pose_side ? "2 - SEITLICH (MAG-Rack)"
                                                : "1 - VON HINTEN (Default)",
                               is_pist ? "" : "   (keine Pistole -> immer Satz 1)");

            if (m_rack._pose_ang.has_value()) {
                ImGui::TextColored(ImColor{0xFF888888}, "   Annaeherungswinkel zuletzt: %.0f Grad",
                                   *m_rack._pose_ang);
            }

            bool sch = false;
            ImGui::Text("Pose 1 - Annaeherung VON HINTEN (Default-Slide-Rack)");
            sch |= ImGui::DragFloat("Slide-Dock X##sd", &sp.dock_x, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Dock Y##sd", &sp.dock_y, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Dock Z##sd", &sp.dock_z, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Hand RotX##sd", &sp.rack_rx, 0.5f, -360.0f, 360.0f, "%.1f");
            sch |= ImGui::DragFloat("Slide-Hand RotY##sd", &sp.rack_ry, 0.5f, -360.0f, 360.0f, "%.1f");
            sch |= ImGui::DragFloat("Slide-Hand RotZ##sd", &sp.rack_rz, 0.5f, -360.0f, 360.0f, "%.1f");

            if (ImGui::Button("Pose 1 auf Punisher-Startwerte##sd1")) {
                sp.dock_x = 0.045f; sp.dock_y = 0.032f; sp.dock_z = -0.192f;
                sp.rack_rx = 53.7f; sp.rack_ry = 285.7f; sp.rack_rz = -86.1f;
                sch = true;
            }

            ImGui::Separator();
            ImGui::Text("Pose 2 - Annaeherung SEITLICH (MAG-Rack) -- nur Pistolen");
            sch |= ImGui::DragFloat("Slide-Dock X (Pose2)##sd2", &sp.sdock_x, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Dock Y (Pose2)##sd2", &sp.sdock_y, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Dock Z (Pose2)##sd2", &sp.sdock_z, 0.002f, -5.0f, 5.0f, "%.4f");
            sch |= ImGui::DragFloat("Slide-Hand RotX (Pose2)##sd2", &sp.srack_rx, 0.5f, -360.0f, 360.0f, "%.1f");
            sch |= ImGui::DragFloat("Slide-Hand RotY (Pose2)##sd2", &sp.srack_ry, 0.5f, -360.0f, 360.0f, "%.1f");
            sch |= ImGui::DragFloat("Slide-Hand RotZ (Pose2)##sd2", &sp.srack_rz, 0.5f, -360.0f, 360.0f, "%.1f");

            if (ImGui::Button("Pose 2 auf Blacktail-Startwerte##sd2")) {
                sp.sdock_x = 0.077f; sp.sdock_y = -0.008f; sp.sdock_z = -0.056f;
                sp.srack_rx = 4.7f; sp.srack_ry = 153.7f; sp.srack_rz = -37.6f;
                sch = true;
            }

            // Zum Einstellen von Pose 2, ohne sie im Spiel treffen zu muessen.
            if (ImGui::Checkbox("Pose 2 zur Vorschau erzwingen (nur im Einstellmodus)",
                                &m_rack.pose_side_preview)) {
                m_rack.pose_side = m_rack.pose_side_preview;
            }

            if (sch) {
                save_cfg();
            }

            ImGui::TreePop();
        }

        ImGui::Separator();

        if (ImGui::TreeNode("Joints (Mag/Slide pro Waffe, Cycler-Probe)")) {
            std::vector<int32_t> jlist;
            std::unordered_map<int32_t, bool> seen;

            if (cur_wid != 0) {
                jlist.push_back(cur_wid);
                seen[cur_wid] = true;
            }

            for (int32_t w : {4000, 4001, 4002, 4003, 4004, 6000, 4200, 4201, 4202, 6300}) {
                if (!seen[w]) {
                    jlist.push_back(w);
                    seen[w] = true;
                }
            }

            for (int32_t w : jlist) {
                char mbuf[32]{};
                char sbuf[32]{};
                std::snprintf(mbuf, sizeof(mbuf), "%s",
                              m_joint_mag.count(w) ? m_joint_mag[w].c_str() : "");
                std::snprintf(sbuf, sizeof(sbuf), "%s",
                              m_joint_slide.count(w) ? m_joint_slide[w].c_str() : "");

                char l1[128]{};
                char l2[128]{};
                std::snprintf(l1, sizeof(l1), "%s Mag##j%d", wp_label(w).c_str(), w);
                std::snprintf(l2, sizeof(l2), "%s Slide##j%d", wp_label(w).c_str(), w);

                const bool cm = ImGui::InputText(l1, mbuf, sizeof(mbuf));
                const bool cs = ImGui::InputText(l2, sbuf, sizeof(sbuf));

                if (cm || cs) {
                    m_joint_mag[w] = mbuf;
                    m_joint_slide[w] = sbuf;
                    save_cfg();
                }
            }

            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "VISUELLER FINDER (Joint per Slider durchschalten):");

            const bool fa = ImGui::Checkbox("Finder aktiv##finder", &m_finder.active);

            // Beim Einschalten einen sichtbaren Offset vorgeben.
            if (fa && m_finder.active && m_finder.x == 0.0f && m_finder.z == 0.0f) {
                m_finder.z = 0.06f;
            }

            const auto& names = weapon_joint_names();
            const int total = static_cast<int>(names.size());

            if (total > 0) {
                m_finder.idx = std::clamp(m_finder.idx, 1, total);

                char lbl[64]{};
                std::snprintf(lbl, sizeof(lbl), "Joint %d/%d##finderidx", m_finder.idx, total);
                ImGui::SliderInt(lbl, &m_finder.idx, 1, total);

                if (ImGui::Button("< vorheriger##finderprev")) {
                    m_finder.idx = (m_finder.idx <= 1) ? total : (m_finder.idx - 1);
                }

                ImGui::SameLine();

                if (ImGui::Button("naechster >##findernext")) {
                    m_finder.idx = (m_finder.idx >= total) ? 1 : (m_finder.idx + 1);
                }

                m_finder.name = names[static_cast<size_t>(m_finder.idx - 1)];
                ImGui::TextColored(ImColor{0xFF00FF00}, "  Aktueller Joint: %s",
                                   m_finder.name.c_str());
            } else {
                ImGui::TextColored(ImColor{0xFFFFAA00},
                                   "  (keine Joints gelesen - Waffe equippt?) Name manuell:");
                char nb[32]{};
                std::snprintf(nb, sizeof(nb), "%s", m_finder.name.c_str());

                if (ImGui::InputText("Joint-Name##finder", nb, sizeof(nb))) {
                    m_finder.name = nb;
                }
            }

            ImGui::SliderFloat("Finder X-Offset##finder", &m_finder.x, -0.20f, 0.20f);
            ImGui::SliderFloat("Finder Y-Offset##finder", &m_finder.y, -0.20f, 0.20f);
            ImGui::SliderFloat("Finder Z-Offset##finder", &m_finder.z, -0.20f, 0.20f);
            ImGui::TextColored(ImColor{0xFF888888},
                               "  Slider/Buttons schalten den Joint; Offset zeigt sichtbar welcher es ist.");

            const int32_t awid = (cur_wid != 0) ? cur_wid : get_equip_wid().value_or(0);

            if (awid != 0 && !m_finder.name.empty()) {
                char b1[128]{};
                char b2[128]{};
                std::snprintf(b1, sizeof(b1), "-> als MAG-Joint fuer %s setzen##setmag",
                              wp_label(awid).c_str());
                std::snprintf(b2, sizeof(b2), "-> als SLIDE-Joint fuer %s setzen##setslide",
                              wp_label(awid).c_str());

                if (ImGui::Button(b1)) {
                    m_joint_mag[awid] = m_finder.name;
                    save_cfg();
                    refresh_weapon();
                }

                ImGui::SameLine();

                if (ImGui::Button(b2)) {
                    m_joint_slide[awid] = m_finder.name;
                    save_cfg();
                    refresh_weapon();
                }

                ImGui::TextColored(ImColor{0xFF888888}, "  aktuell gesetzt: Mag=%s  Slide=%s",
                                   m_joint_mag.count(awid) ? m_joint_mag[awid].c_str() : "",
                                   m_joint_slide.count(awid) ? m_joint_slide[awid].c_str() : "");
            }

            ImGui::TreePop();
        }

        ImGui::TreePop();
    }

    // ---- WEITERE GATTUNGEN ----
    if (ui_category_row("Manual SMG Reload", "smgs_enabled", cfg.smgs_enabled)) {
        save_cfg();
    }

    if (ImGui::TreeNode("SMG - Einstellungen")) {
        const int32_t awid = (cur_wid != 0) ? cur_wid : get_equip_wid().value_or(0);
        ImGui::Text("Equippt: %s", wp_label(awid).c_str());
        ImGui::TextColored(ImColor{0xFF888888}, "  aktuell: Mag=%s  Slide=%s",
                           m_joint_mag.count(awid) ? m_joint_mag[awid].c_str() : "",
                           m_joint_slide.count(awid) ? m_joint_slide[awid].c_str() : "");

        if (awid != 0) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF888888},
                               "Hand-Posen (im Code zugewiesen): Mag=%s  Rack=%s",
                               m_mag_pose.count(awid) ? m_mag_pose[awid].c_str() : "(global)",
                               m_rack_pose.count(awid) ? m_rack_pose[awid].c_str() : "(global)");

            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Mag-Position in der Hand (Offset, pro Waffe):");
            auto& m = maghand(awid);
            ImGui::Checkbox("Mag in Hand halten (Tuning)##smgmagtune", &m_mag_tune.active);

            bool ch = false;
            ch |= ImGui::SliderFloat("Mag X##smg", &m.x, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Mag Y##smg", &m.y, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Mag Z##smg", &m.z, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Mag RotX##smg", &m.rx, -180.0f, 180.0f);
            ch |= ImGui::SliderFloat("Mag RotY##smg", &m.ry, -180.0f, 180.0f);
            ch |= ImGui::SliderFloat("Mag RotZ##smg", &m.rz, -180.0f, 180.0f);
            ImGui::Text("Mag-Daumen (additiv auf die Pose, Grad):");
            ch |= ImGui::SliderFloat("Mag-Daumen RotX##smg", &m.t_rx, -90.0f, 90.0f);
            ch |= ImGui::SliderFloat("Mag-Daumen RotY##smg", &m.t_ry, -90.0f, 90.0f);
            ch |= ImGui::SliderFloat("Mag-Daumen RotZ##smg", &m.t_rz, -90.0f, 90.0f);

            if (ch) {
                save_cfg();
            }

            ImGui::Separator();
            float curd = m_insert_dist["smgs"];

            if (const auto it = m_insert_dist_wid.find(awid); it != m_insert_dist_wid.end()) {
                curd = it->second;
            }

            ImGui::TextColored(ImColor{0xFF66CCFF}, "Mag-Reingleit-Distanz fuer %s:",
                               wp_label(awid).c_str());

            if (ImGui::SliderFloat("Reingleit-Distanz m (diese Waffe)##widid",
                                   &curd, 0.03f, 0.50f)) {
                m_insert_dist_wid[awid] = curd;
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888}, "  live Hand->Waffe: %.3f m",
                               m_mag_hand.dist);
        }

        ImGui::TreePop();
    }

    if (ui_category_row("Manual Shotgun Reload", "shotguns_enabled", cfg.shotguns_enabled)) {
        save_cfg();
    }

    if (ImGui::TreeNode("Shotgun - Shell laden / Pump (W-870)")) {
        const int32_t awid = (cur_wid != 0) ? cur_wid : get_equip_wid().value_or(0);
        ImGui::Text("Equippt: %s", wp_label(awid).c_str());

        if (!is_shotgun(awid)) {
            ImGui::TextColored(ImColor{0xFFFFAA00},
                               "  (aktuell ist keine eingerichtete Shotgun equippt - W-870 nehmen)");
        }

        ImGui::TextColored(ImColor{0xFF888888}, "  Joints: Shell=%s  Pump=%s",
                           m_joint_mag.count(awid) ? m_joint_mag[awid].c_str() : "",
                           m_joint_slide.count(awid) ? m_joint_slide[awid].c_str() : "");

        // [SHOTGUN] Lade-Verhaeltnis PRO WAFFE (Override; sonst globaler Default)
        ImGui::Separator();
        ImGui::TextColored(ImColor{0xFF66CCFF},
                           "Lade-Verhaeltnis fuer %s (Shells pro Einfuehrbewegung):",
                           wp_label(awid).c_str());
        {
            const int32_t eff = shotgun_ratio_get(awid);

            for (int32_t r = 1; r <= 3; ++r) {
                char b[32]{};
                std::snprintf(b, sizeof(b), "%s1:%d %s##shratio%d",
                              (eff == r) ? "[" : " ", r, (eff == r) ? "]" : " ", r);

                if (ImGui::Button(b) && awid != 0) {
                    m_shotgun_ratio_wid[awid] = r;
                    save_cfg();
                }

                if (r < 3) {
                    ImGui::SameLine();
                }
            }
        }

        ImGui::TextColored(ImColor{0xFF888888},
                           "  (pro Shotgun; noch nicht gesetzte nutzen den globalen Default 1:2)");

        // [SHELL-DOCK] Andock-Spot der STRIKER justieren.
        if (awid == 4102 && m_dock_port.count(4102) > 0) {
            auto& dp = m_dock_port[4102];
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFFFF00FF}, "Andock-Slider Striker");
            ImGui::TextColored(ImColor{0xFF888888}, "  (Joint %s + Offset, persistiert)",
                               dp.joint.c_str());

            bool dpc = false;
            dpc |= ImGui::DragFloat("Shell-Dock X##dp", &dp.x, 0.002f, -0.5f, 0.5f, "%.4f");
            dpc |= ImGui::DragFloat("Shell-Dock Y##dp", &dp.y, 0.002f, -0.5f, 0.5f, "%.4f");
            dpc |= ImGui::DragFloat("Shell-Dock Z##dp", &dp.z, 0.002f, -0.5f, 0.5f, "%.4f");

            if (dpc) {
                save_cfg();
            }
        }

        if (awid != 0) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Shell-Position in der Hand (Offset, pro Waffe):");
            auto& m = maghand(awid);
            ImGui::Checkbox("Shell in Hand halten (Tuning)##shmagtune", &m_mag_tune.active);

            bool ch = false;
            ch |= ImGui::SliderFloat("Shell X##sh", &m.x, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Shell Y##sh", &m.y, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Shell Z##sh", &m.z, -0.2f, 0.2f);
            ch |= ImGui::SliderFloat("Shell RotX##sh", &m.rx, -180.0f, 180.0f);
            ch |= ImGui::SliderFloat("Shell RotY##sh", &m.ry, -180.0f, 180.0f);
            ch |= ImGui::SliderFloat("Shell RotZ##sh", &m.rz, -180.0f, 180.0f);
            ImGui::Text("Shell-Daumen (additiv auf die Pose, Grad):");
            ch |= ImGui::SliderFloat("Shell-Daumen RotX##sh", &m.t_rx, -90.0f, 90.0f);
            ch |= ImGui::SliderFloat("Shell-Daumen RotY##sh", &m.t_ry, -90.0f, 90.0f);
            ch |= ImGui::SliderFloat("Shell-Daumen RotZ##sh", &m.t_rz, -90.0f, 90.0f);

            if (ch) {
                save_cfg();
            }
        }

        // [SHELL_CLONE] Skull Shaker: eigene Hand-Offsets fuer den Mesh-Clone.
        if (awid == 6001) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Skull Shaker: Patrone-in-Hand = Mesh-Clone von Part 1 (eigene Offsets):");
            ImGui::Checkbox("Vorschau: Patrone-Clone in der Hand (zum Tunen)##ssclprev",
                            &m_ss_clone.preview);
            ImGui::TextColored(ImColor{0xFF888888},
                               "  (Vorschau erzwingt den Clone dauerhaft in der Hand)");

            bool cc = false;
            cc |= ImGui::SliderFloat("Clone X##sscl", &m_ss_clone.x, -0.2f, 0.2f);
            cc |= ImGui::SliderFloat("Clone Y##sscl", &m_ss_clone.y, -0.2f, 0.2f);
            cc |= ImGui::SliderFloat("Clone Z##sscl", &m_ss_clone.z, -0.2f, 0.2f);
            cc |= ImGui::SliderFloat("Clone RotX##sscl", &m_ss_clone.rx, -180.0f, 180.0f);
            cc |= ImGui::SliderFloat("Clone RotY##sscl", &m_ss_clone.ry, -180.0f, 180.0f);
            cc |= ImGui::SliderFloat("Clone RotZ##sscl", &m_ss_clone.rz, -180.0f, 180.0f);
            cc |= ImGui::SliderFloat("Clone Scale##sscl", &m_ss_clone.scale, 0.1f, 3.0f);
            cc |= ImGui::SliderInt("Mesh-Part-Index (Patrone = 1)##ssclpart",
                                   &m_ss_clone.part, 0, 48);

            if (cc) {
                save_cfg();
            }
        }

        // [EMPTY-RELOAD SLIDE] _08-Lade-Slide (Riot Gun)
        if (awid != 0 && empty_reload_joint(awid) != nullptr) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Empty-Reload Lade-Slide (_08) - Chambern nach Leerschuss:");
            auto& s2 = slide_pose2(awid);

            if (ImGui::Button("Capture _08 rest = Live-Z jetzt##er")) {
                glm::vec3 lp{};

                if (m_wep.slide_joint2 != nullptr
                    && get_vec3(m_wep.slide_joint2, "get_LocalPosition", lp)) {
                    s2.rest_z = lp.z;

                    if (s2.park_z == 0.0f) {
                        s2.park_z = lp.z;
                    }

                    save_cfg();
                }
            }

            bool d = false;
            d |= ImGui::SliderFloat("_08 rest_z (vorn/chambered)##er", &s2.rest_z, -0.6f, 0.6f, "%.4f");
            d |= ImGui::SliderFloat("_08 park_z (wartet)##er", &s2.park_z, -0.6f, 0.6f, "%.4f");
            d |= ImGui::SliderFloat("_08 back_z (voll gezogen)##er", &s2.back_z, -0.6f, 0.6f, "%.4f");
            d |= ImGui::SliderFloat("_08 Hand Dock X##er", &s2.dock_x, -0.3f, 0.3f);
            d |= ImGui::SliderFloat("_08 Hand Dock Y##er", &s2.dock_y, -0.3f, 0.3f);
            d |= ImGui::SliderFloat("_08 Hand Dock Z##er", &s2.dock_z, -0.3f, 0.3f);
            d |= ImGui::SliderFloat("_08 Hand Rot X##er", &s2.rack_rx, -180.0f, 180.0f);
            d |= ImGui::SliderFloat("_08 Hand Rot Y##er", &s2.rack_ry, -180.0f, 180.0f);
            d |= ImGui::SliderFloat("_08 Hand Rot Z##er", &s2.rack_rz, -180.0f, 180.0f);

            if (d) {
                save_cfg();
            }
        }

        // [SHELL_EJECT] Chamber-Startposition + Flug
        if (awid != 0) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Shell-Ejection: Chamber-Startposition (_04 Offset):");
            ImGui::Checkbox("Preview Shell in Chamber (Tuning)##shejprev",
                            &m_shell_eject_st.preview);
            auto& s = shell_eject_cfg(awid);

            bool ec = false;
            ec |= ImGui::SliderFloat("Chamber X##shej", &s.sx, -0.3f, 0.3f);
            ec |= ImGui::SliderFloat("Chamber Y##shej", &s.sy, -0.3f, 0.3f);
            ec |= ImGui::SliderFloat("Chamber Z##shej", &s.sz, -0.3f, 0.3f);
            ec |= ImGui::SliderFloat("Chamber RotX##shej", &s.srx, -180.0f, 180.0f);
            ec |= ImGui::SliderFloat("Chamber RotY##shej", &s.sry, -180.0f, 180.0f);
            ec |= ImGui::SliderFloat("Chamber RotZ##shej", &s.srz, -180.0f, 180.0f);
            ImGui::TextColored(ImColor{0xFF66CCFF}, "-- Flug (lokal zur Waffe; vx = rechts) --");
            ec |= ImGui::SliderFloat("Flug vx rechts##shej", &s.vx, -3.0f, 3.0f);
            ec |= ImGui::SliderFloat("Flug vy hoch##shej", &s.vy, -3.0f, 3.0f);
            ec |= ImGui::SliderFloat("Flug vz vor/zur##shej", &s.vz, -3.0f, 3.0f);
            ec |= ImGui::SliderFloat("Gravity##shej", &s.grav, 0.0f, 20.0f);
            ec |= ImGui::SliderFloat("Flugdauer s##shej", &s.dur, 0.1f, 2.0f);
            ec |= ImGui::SliderFloat("Spin Grad/s##shej", &s.spin, -1440.0f, 1440.0f);

            if (ec) {
                save_cfg();
            }

            if (ImGui::Button("Test-Flug##shejtest")) {
                m_shell_eject_st.preview = false;
                m_shell_eject_st.flying = true;
                m_shell_eject_st.t = 0.0f;
            }

            ImGui::TextColored(ImColor{0xFF888888},
                               "(Trigger im Spiel: Pump voll zurueckziehen. vx links/rechts ueber Vorzeichen.)");
        }

        // [SHOTGUN] Reingleit-Distanz + Pump-Positionen
        if (awid != 0) {
            ImGui::Separator();
            float curd = m_insert_dist["shotguns"];

            if (const auto it = m_insert_dist_wid.find(awid); it != m_insert_dist_wid.end()) {
                curd = it->second;
            }

            ImGui::TextColored(ImColor{0xFF66CCFF}, "Shell-Reingleit-Distanz fuer %s:",
                               wp_label(awid).c_str());

            if (ImGui::SliderFloat("Reingleit-Distanz m (diese Waffe)##shwidid",
                                   &curd, 0.03f, 0.50f)) {
                m_insert_dist_wid[awid] = curd;
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888}, "  live Hand->Waffe: %.3f m",
                               m_mag_hand.dist);

            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Pump (lokale Z): rest=vorn(gechambert), back=ganz gezogen, park=Mitte");
            auto& sp2 = slide_pose(awid);

            bool pc = false;
            // Die Range muss die W-870-Skala abdecken (rest/park ~0.425).
            pc |= ImGui::SliderFloat("Pump rest_z (vorn)##shpump", &sp2.rest_z, -0.20f, 0.60f);
            pc |= ImGui::SliderFloat("Pump back_z (gezogen)##shpump", &sp2.back_z, -0.20f, 0.60f);
            pc |= ImGui::SliderFloat("Pump park_z (Mitte)##shpump", &sp2.park_z, -0.20f, 0.60f);
            pc |= ImGui::SliderFloat("Pump-Greif-Distanz m (linker Grip)##shpumpgrab",
                                     &cfg.rack_grab_dist, 0.05f, 0.80f);
            // [PUMP_START_PULL] Zu klein = die Shotgun wird schon beim blossen
            // Halten steif.
            pc |= ImGui::SliderFloat("Pump-Startzug m (ab wann Zug zaehlt)##shpumppull",
                                     &cfg.pump_start_pull, 0.01f, 0.30f);
            pc |= ImGui::SliderFloat("Pump-Rueckweg (Anteil des Zugs)##shpumppush",
                                     &cfg.pump_push_frac, 0.20f, 1.00f);
            pc |= ImGui::SliderFloat("Pump-Haptik-Delay s (aufs Audio legen)##shhapd",
                                     &cfg.pump_haptic_delay, 0.0f, 0.30f);

            if (pc) {
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888}, "  Hand->Pump: %.3f m  needs_rack=%s",
                               (m_rack._last_dist >= 0.0f) ? m_rack._last_dist : 0.0f,
                               m_rack.needs ? "true" : "false");
        }

        // [ROTARY_CYCLE] Drehschalter-Tuning (Striker)
        if (awid != 0 && rotary_cycle(awid)) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Drehschalter (joint_01) - Rotation pro Cycle (Grad):");

            if (ImGui::Button("Capture Ruhe-Rotation jetzt##rotrest")) {
                glm::quat q{};

                if (m_wep.slide_joint != nullptr
                    && get_quat(m_wep.slide_joint, "get_LocalRotation", q)) {
                    m_wep.cycle_rest_rot = q;
                }
            }

            ImGui::Checkbox("Vorschau: Drehung per Regler##rotprev", &m_rotary.preview);

            if (m_rotary.preview) {
                ImGui::SliderFloat("Vorschau-Drehung 0..1##rotprog", &m_rotary.prog, 0.0f, 1.0f);
                ImGui::TextColored(ImColor{0xFF00FF00},
                                   "  VORSCHAU AKTIV - Schalter folgt dem Regler");
            }

            auto& r = rotary_cfg(awid);
            bool rch = false;
            rch |= ImGui::SliderFloat("Dreh RotX (Grad)##rotrx", &r.rx, -90.0f, 90.0f);
            rch |= ImGui::SliderFloat("Dreh RotY (Grad)##rotry", &r.ry, -90.0f, 90.0f);
            rch |= ImGui::SliderFloat("Dreh RotZ (Grad)##rotrz", &r.rz, -90.0f, 90.0f);
            ImGui::TextColored(ImColor{0xFF888888}, "  rest_rot=%s  needs=%s  dir=%d prog=%.2f",
                               m_wep.cycle_rest_rot.has_value() ? "ok" : "nil",
                               m_rack.needs ? "true" : "false", m_rotary.dir, m_rotary.prog);
            rch |= ImGui::SliderFloat("Dreh-Geschwindigkeit (lerp, klein=sanft)##rotlerp",
                                      &r.lerp, 0.01f, 0.30f);
            rch |= ImGui::SliderFloat("Schalter-Greif-Distanz m (left grip)##rotgrab",
                                      &r.grab_dist, 0.03f, 0.50f);

            if (rch) {
                save_cfg();   // [PERSIST] ueber Reset Scripts retten
            }

            ImGui::TextColored(ImColor{0xFF888888}, "  live Hand->Schalter: %.3f m",
                               m_rotary._last_dist);

            ImGui::Spacing();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Hand-Pose am Drehschalter (Dock-Offset, persistiert):");
            auto& dsp = slide_pose(awid);
            ImGui::Checkbox("Hand an den Schalter zwingen (Einstellmodus)##rotdock",
                            &m_rack.dock_tune);

            if (m_rack.dock_tune) {
                ImGui::TextColored(ImColor{0xFF00FF00},
                                   "  EINSTELLMODUS AKTIV - Hand sitzt am Schalter");
            }

            bool hch = false;
            hch |= ImGui::DragFloat("Hand Dock X##rotsd", &dsp.dock_x, 0.002f, -5.0f, 5.0f, "%.4f");
            hch |= ImGui::DragFloat("Hand Dock Y##rotsd", &dsp.dock_y, 0.002f, -5.0f, 5.0f, "%.4f");
            hch |= ImGui::DragFloat("Hand Dock Z##rotsd", &dsp.dock_z, 0.002f, -5.0f, 5.0f, "%.4f");
            hch |= ImGui::DragFloat("Hand RotX##rotsd", &dsp.rack_rx, 0.5f, -360.0f, 360.0f, "%.1f");
            hch |= ImGui::DragFloat("Hand RotY##rotsd", &dsp.rack_ry, 0.5f, -360.0f, 360.0f, "%.1f");
            hch |= ImGui::DragFloat("Hand RotZ##rotsd", &dsp.rack_rz, 0.5f, -360.0f, 360.0f, "%.1f");

            if (hch) {
                save_cfg();
            }
        }

        // [BREAK_ACTION] Klapphebel-Tuning (Skull Shaker)
        if (awid != 0 && break_action(awid)) {
            ImGui::Separator();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Klapphebel (joint_02) - Break-Action (RIGHT-B auf/zu):");

            if (ImGui::Button("Capture Ruhe-Rotation (zu) jetzt##brkrest")) {
                glm::quat q{};

                if (m_wep.slide_joint != nullptr
                    && get_quat(m_wep.slide_joint, "get_LocalRotation", q)) {
                    m_wep.cycle_rest_rot = q;
                }
            }

            ImGui::Checkbox("Vorschau: Klappe per Regler##brkprev", &m_break.preview);

            if (m_break.preview) {
                ImGui::SliderFloat("Vorschau auf 0..1##brkprog", &m_break.prog, 0.0f, 1.0f);
                ImGui::TextColored(ImColor{0xFF00FF00},
                                   "  VORSCHAU AKTIV - Klappe folgt dem Regler");
            }

            auto& br = rotary_cfg(awid);
            bool bc = false;
            bc |= ImGui::SliderFloat("Klappe RotX (auf, Grad)##brkrx", &br.rx, -120.0f, 120.0f);
            bc |= ImGui::SliderFloat("Klappe RotY (auf, Grad)##brkry", &br.ry, -120.0f, 120.0f);
            bc |= ImGui::SliderFloat("Klappe RotZ (auf, Grad)##brkrz", &br.rz, -120.0f, 120.0f);
            bc |= ImGui::SliderFloat("Klapp-Geschwindigkeit (lerp)##brklerp", &br.lerp,
                                     0.01f, 0.30f);

            if (bc) {
                save_cfg();
            }

            ImGui::Spacing();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Lade (joint_02) waehrend der Drehung - nur beim Wrist-Flick:");
            bool oc = false;
            oc |= ImGui::SliderFloat("Lade Oeffnungswinkel RotX (Grad)##skopdeg",
                                     &cfg.skull_open_deg, -120.0f, 120.0f);
            oc |= ImGui::SliderFloat("Dreh-Dauer beim Flick (s, klein = schneller)##skspindur",
                                     &cfg.skull_spin_dur, 0.10f, 1.50f);

            if (oc) {
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888},
                               "  -30 = live eingestellt (klappt nach unten). Gilt fuer die Dauer der Drehung.");

            ImGui::Spacing();
            ImGui::TextColored(ImColor{0xFF66CCFF},
                               "Ruhepose skull-default - Zeigefinger beugen (Grad, + = krummer):");
            // Die Regler wirken SOFORT (motion legt die Grad additiv auf die
            // Pose), SET macht sie dauerhaft.
            ImGui::SliderFloat("Zeigefinger Glied 1##skidx1", &cfg.skull_idx1, -30.0f, 100.0f);
            ImGui::SliderFloat("Zeigefinger Glied 2##skidx2", &cfg.skull_idx2, -30.0f, 100.0f);
            ImGui::SliderFloat("Zeigefinger Glied 3##skidx3", &cfg.skull_idx3, -30.0f, 100.0f);

            if (ImGui::Button("SET (Zeigefinger speichern)##skidxset")) {
                save_cfg();
            }

            ImGui::TextColored(ImColor{0xFF888888},
                               "  Regler wirken sofort; SET macht sie dauerhaft.");
            ImGui::TextColored(ImColor{0xFF888888}, "  rest=%s prog=%.2f open=%s  (RIGHT-B togglet)",
                               m_wep.cycle_rest_rot.has_value() ? "ok" : "nil", m_break.prog,
                               (re4vr::lua_get_tribool("__vr_break_open") == 1) ? "true" : "false");
        }

        ImGui::TreePop();
    }

    if (ui_category_row("Manual Magnum Reload", "magnum_enabled", cfg.magnum_enabled)) {
        save_cfg();
    }

    if (ui_category_row("Manual Rifle Reload", "rifles_enabled", cfg.rifles_enabled)) {
        save_cfg();
    }

    ImGui::TextColored(ImColor{0xFF888888}, "  (Magnum/Rifle: Logik folgt als Stufen)");
    ImGui::TreePop();
}

#endif
