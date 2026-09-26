// ============================================================================
// RE4VRReloadAdv -- 1:1-Portierung von re4_vr_reload_adv.lua (1.291 Zeilen).
//
// Der ADVANCED Mag-Slide. In Lua ein MODUL `M`, veroeffentlicht als
// _G.__re4_reload_mag_slide -- die fuenf Reload-Dateien greifen an rund 250
// Stellen auf 26 seiner Member zu.
//
// Spezifikation: I:\LUATRANS\PORT_RELOAD_STRATEGIE.md
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
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRReloadAdv.hpp"
#include "RE4VRReloadMain.hpp"   // [WPOSE]

#undef min
#undef max

namespace {

constexpr const char* CFG_PATH = "re4_vr/re4_vr_reload_adv.json";

// Lua: os.clock().
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
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

// smoothstep
float ease(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// Quaternion aus Euler-Grad -- Lua baut qz*qy*qx aus drei Einzelachsen
// (Quaternion.new ist dort W,X,Y,Z).
glm::quat quat_from_euler(float rx, float ry, float rz) {
    const float hx = glm::radians(rx) * 0.5f;
    const float hy = glm::radians(ry) * 0.5f;
    const float hz = glm::radians(rz) * 0.5f;

    const glm::quat qx{std::cos(hx), std::sin(hx), 0.0f, 0.0f};
    const glm::quat qy{std::cos(hy), 0.0f, std::sin(hy), 0.0f};
    const glm::quat qz{std::cos(hz), 0.0f, 0.0f, std::sin(hz)};

    return glm::normalize(qz * qy * qx);
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

// push_quat(deg, axis, sign) -- axis ist der LUA-INDEX in {w,x,y,z}:
// 2 = X, 3 = Y, 4 = Z.
glm::quat push_quat(float deg, int axis, float sign = 1.0f) {
    const float r = glm::radians(deg) * 0.5f * sign;
    glm::quat q{std::cos(r), 0.0f, 0.0f, 0.0f};

    switch (axis) {
    case 2: q.x = std::sin(r); break;
    case 3: q.y = std::sin(r); break;
    case 4: q.z = std::sin(r); break;
    default: break;
    }

    return q;
}

// Interpolierte lokale Pose am Fortschritt tt (0..1) entlang der geordneten
// Keyframes. LINEAR -- die Form steckt in den Keyframes selbst, ein
// zusaetzliches Easing wuerde ihre Abstaende verzerren.
bool pose_at(const std::vector<RE4VRReloadAdv::Key>& k, float tt,
             RE4VRReloadAdv::Key& out) {
    if (k.empty()) {
        return false;
    }

    if (k.size() == 1 || tt <= 0.0f) {
        out = k.front();

        return true;
    }

    if (tt >= 1.0f) {
        out = k.back();

        return true;
    }

    const float seg = tt * static_cast<float>(k.size() - 1);
    size_t i = static_cast<size_t>(std::floor(seg));

    if (i >= k.size() - 1) {
        i = k.size() - 2;
    }

    const float f = seg - static_cast<float>(i);
    const auto& a = k[i];
    const auto& b = k[i + 1];

    out.x = a.x + (b.x - a.x) * f;
    out.y = a.y + (b.y - a.y) * f;
    out.z = a.z + (b.z - a.z) * f;
    out.rx = a.rx + (b.rx - a.rx) * f;
    out.ry = a.ry + (b.ry - a.ry) * f;
    out.rz = a.rz + (b.rz - a.rz) * f;

    return true;
}

// Joint entlang einer Keyframe-Bahn setzen: lokale Pose -> Welt ueber die
// Waffen-Transform (klebt an der Waffe, kein World-Drift).
bool apply_keys(::REManagedObject* weapon_tf, ::REManagedObject* joint,
                const RE4VRReloadAdv::Key& k) {
    glm::vec3 gp{};
    glm::quat gr{};

    if (!get_vec3(weapon_tf, "get_Position", gp) || !get_quat(weapon_tf, "get_Rotation", gr)) {
        return false;
    }

    set_vec3(joint, "set_Position", gp + (gr * glm::vec3{k.x, k.y, k.z}));
    set_quat(joint, "set_Rotation", glm::normalize(gr * quat_from_euler(k.rx, k.ry, k.rz)));

    return true;
}

// [KEYFRAME-PREVIEW] Gemeinsamer Rumpf von shell_preview_apply und
// eject_preview_apply -- in Lua zwei getrennte, aber Zeile fuer Zeile gleiche
// Funktionen.
void preview_apply_pose(const RE4VRReloadAdv::Key& s) {
    auto* joint = re4vr::lua_get_pointer("__re4_reload_shell_joint");
    auto* tf = re4vr::lua_get_pointer("__re4_reload_weapon_tf");

    if (joint == nullptr || tf == nullptr) {
        return;
    }

    glm::vec3 gp{};
    glm::quat gr{};

    if (!get_vec3(tf, "get_Position", gp) || !get_quat(tf, "get_Rotation", gr)) {
        return;
    }

    set_vec3(joint, "set_Position", gp + (gr * glm::vec3{s.x, s.y, s.z}));
    set_quat(joint, "set_Rotation", glm::normalize(gr * quat_from_euler(s.rx, s.ry, s.rz)));

    // Wie reload5 (update_cart_in_hand): die Engine skaliert den Shell-/Mag-
    // Joint bei leerer Kammer auf 0 = unsichtbar. Ohne das bleibt die Shell im
    // Preview unsichtbar, obwohl der Joint korrekt an der Tuning-Lage sitzt.
    set_vec3(joint, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
}

}   // namespace

// ============================================================================
// Allowlisten (Lua: M.DOCK_ALLOWED / M.KEYFRAME_INSERT / M.KEYFRAME_EJECT /
// M.SHELL_CLONE / M.PUSH_WIDS)
// ============================================================================

// [HARTE SCHRANKE 2026-07-23] AUSSCHLIESSLICH Pistolen und SMGs. Rifles,
// Shotguns, Revolver, Bogen, Armbrust und Raketenwerfer duerfen den
// Einleit-Punkt NIE benutzen -- auch dann nicht, wenn ueber die UI oder eine
// alte JSON versehentlich ein Eintrag fuer sie entsteht.
static bool dock_allowed(int32_t wid) {
    switch (wid) {
    case 4000: case 4001: case 4002: case 4003: case 4004:   // Pistolen (Leon)
    case 4501: case 6000:                                     // Killer7, Sentinel Nine
    case 4200:                                                // TMP (SMG)
    case 6112: case 6103: case 6113: case 6104:               // Ada
    case 6300:                                                // [MC_6300] XM96E1
    case 6301:                                                // [MC_6301] Blacktail AC
        return true;
    default:
        return false;
    }
}

// [SHELL-KEYFRAMES] Nur Waffen hier nutzen die Keyframe-Bahn. Striker + W-870 +
// Riot Gun + Butterfly + Handcannon + Red9 (4002 = Stripper-Clip, 40021 =
// Einzelpatrone, virtuelle wid) + Sawed-off W-870 + Skull Shaker + SR M1903 +
// Bolt Thrower + LE 5 + Blast Crossbow.
bool RE4VRReloadAdv::is_keyframe_insert(int32_t wid) const {
    switch (wid) {
    case 4102: case 4100: case 4101: case 4500: case 4502:
    case 4002: case 40021:
    case 6100: case 6001: case 4400: case 4600: case 4202:
    case 6104: case 4401: case 4402: case 4201: case 6105:
    case 6114: case 6102:
        return true;
    default:
        return false;
    }
}

// [MAG-EJECT-KEYFRAMES] Eigene Allowlist -- die Insert-Keyframes bleiben
// unangetastet. 6104 (MP-AF) ist AUSGEBAUT: bei ihr kam die Bahn nie sichtbar
// an; ein Wiedereintrag wurde am 15.08. probiert und am selben Tag
// zurueckgenommen (Mag ging verkantet rein). NICHT wieder eintragen, ohne dass
// die Ursache gefunden ist.
// [MERCS-TMP 2026-09-09] 42001 = VIRTUELLE wid fuer die TMP in Mercenaries
// (Krauser). Dieselbe Waffe wp4200, aber eine EIGENE Bahn -- Leons 4200 wird
// dabei nirgends angefasst. Umgeschaltet wird in RE4VRReloadMain::kf_wid().
bool RE4VRReloadAdv::is_keyframe_eject(int32_t wid) const {
    switch (wid) {
    case 4003: case 4001: case 4000: case 4004: case 4200:
    case 4501: case 6000: case 6103: case 6112: case 6300:
    case 6301:
    case 42001:   // [MERCS-TMP] TMP (Mercs/Krauser) <- eigene Bahn
        return true;
    default:
        return false;
    }
}

// [SHELL_CLONE] Waffen, deren native Shell im Preview unsichtbar ist.
bool RE4VRReloadAdv::is_shell_clone(int32_t wid) const {
    return wid == 6100;   // Sawed-off W-870
}

// [PUSH_WIDS] Nur Waffen mit Magazin-Reload duerfen nachdruecken. Alles andere
// hat kein Magazin, das man mit dem Handballen hochschiebt.
bool RE4VRReloadAdv::is_push_wid(int32_t wid) const {
    switch (wid) {
    case 4000: case 4001: case 4003: case 4004: case 4501:
    case 6000: case 6103: case 6112: case 6300: case 6301:
    case 4200: case 6104:   // TMP (Leon) + MP-AF (Ada)
        return true;
    default:
        return false;
    }
}

// ============================================================================
// Initialisierung
// ============================================================================

void RE4VRReloadAdv::on_initialize() {
    // [KEIN AUTO-ANLEGEN 2026-07-23] Nur die ausdruecklich vermessenen Punkte.
    m_docks = {
        {6000, {"_03", 0.0f, -0.092f, -0.061f}},    // Sentinel Nine
        {4003, {"_03", 0.0f, -0.087f, -0.057f}},    // Blacktail
        {6103, {"_03", 0.0f, -0.087f, -0.057f}},    // Blacktail AC (Ada) -- baugleich
        {4000, {"_03", 0.0f, -0.087f, -0.068f}},    // SG-09 R
        {4001, {"_03", 0.0f, -0.075f, -0.050f}},    // Punisher
        {6112, {"_03", 0.0f, -0.075f, -0.050f}},    // Punisher MC (Ada) -- baugleich
        {6300, {"_03", 0.0f, -0.075f, -0.050f}},    // [MC_6300] XM96E1 -- Punisher-Werte
        {6301, {"_03", 0.0f, -0.087f, -0.057f}},    // [MC_6301] Blacktail AC (Mercs) <- 4003
        {4004, {"_03", 0.0f, -0.077f, -0.0949f}},   // Matilda
        // TMP und Adas MP-AF sind baugleich -> identische Werte.
        {4200, {"_03", 0.0f, -0.097f, -0.052f}},    // TMP (Leon)
        {6104, {"_03", 0.0f, -0.097f, -0.052f}},    // MP-AF (Ada, TMP-Klon)
        {4501, {"_03", 0.0f, -0.097f, -0.070f}},    // Killer7
        // Red9 und Adas Samurai Edge sind TOP-LOADER: das Magazin kommt von
        // OBEN in die Waffe, deshalb sind Y und Z hier positiv -- kein
        // Vorzeichenfehler (ausdruecklich bestaetigt).
        {4002, {"_03", 0.0f, 0.064f, 0.051f}},      // Red9
        {6113, {"_03", 0.0f, 0.064f, 0.051f}},      // Samurai Edge (Ada, Red9-Klon)
        // [LE 5 RAUS 2026-07-23] Sie hat keine Kammer, sondern einen
        // Bananenclip, der schlicht angesteckt wird -- kein Startpunkt fuer
        // sie. Ihre Einlege-DISTANZ laeuft weiter ueber DOCK_PORT[4202].
    };

    m_weapons = {
        {4004, WCfg{}},   // exit {0,-0.10,0}, slide_dur 0.18, gravity 9.8
    };

    // [INSERT = EJECT RUECKWAERTS] Default: ALLE Waffen mit Auswurf-Bahn (Leon
    // + Ada). Das ist die BASIS im Code -- load_cfg legt die JSON nur DRUEBER.
    m_rev_insert = {
        {4000, true}, {4001, true}, {4003, true}, {4004, true}, {4200, true},
        {4501, true}, {6000, true},
        {6103, true}, {6112, true},   // 6104 (MP-AF) ist ausgebaut
        {6300, true},                 // [MC_6300] XM96E1
        {6301, true},                 // [MC_6301] Blacktail AC <- 4003
        {42001, true},                // [MERCS-TMP] TMP (Mercs/Krauser) <- 4200
    };

    load_cfg();
    // [ADA: EIGENE KOPIE] nur befuellen, was leer geblieben ist (getunte
    // Saetze bleiben).
    seed_eject_keys();
}

void RE4VRReloadAdv::on_lua_state_destroyed() {
    // [TUNING] Preview/Tuning nie ueber einen Script-Reset hinaus anlassen
    // (das Mag haenge sonst in der Luft).
    eject_preview = false;
    re4vr::lua_set_nil("__re4_mag_eject_kf_preview");
    push_tune = false;
}

// ============================================================================
// Einleit-Punkt (Lua Z.81-183)
// ============================================================================

const RE4VRReloadAdv::Dock* RE4VRReloadAdv::dock(int32_t wid) const {
    const auto it = m_docks.find(wid);

    return it != m_docks.end() ? &it->second : nullptr;
}

// Nur fuer die UI: Eintrag anlegen, wenn man ihn dort bewusst einstellen will.
RE4VRReloadAdv::Dock& RE4VRReloadAdv::dock_or_create(int32_t wid) {
    auto it = m_docks.find(wid);

    if (it == m_docks.end()) {
        it = m_docks.emplace(wid, Dock{}).first;
    }

    return it->second;
}

// Weltposition des Einleit-Punktes: Joint-Position + (Joint-Rotation * Versatz).
// nullopt, wenn der Joint an dieser Waffe nicht existiert -> der Aufrufer faellt
// auf sein bisheriges Verhalten zurueck.
std::optional<glm::vec3> RE4VRReloadAdv::dock_world(::REManagedObject* weapon_tf,
                                                    int32_t wid) {
    if (weapon_tf == nullptr || !dock_allowed(wid)) {
        return std::nullopt;
    }

    const auto* d = dock(wid);

    if (d == nullptr) {
        return std::nullopt;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(d->joint));

    if (str == nullptr) {
        return std::nullopt;
    }

    auto* j = re4vr::call_safe<::REManagedObject*>(weapon_tf, "getJointByName", str);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 jp{};
    glm::quat jr{};

    if (!get_vec3(j, "get_Position", jp) || !get_quat(j, "get_Rotation", jr)) {
        return std::nullopt;
    }

    return jp + (jr * glm::vec3{d->x, d->y, d->z});
}

// Derselbe Punkt, aber LOKAL im Raum der Waffen-Transform -- das braucht der
// Mag-Joint, der ueber set_LocalPosition gefuehrt wird.
std::optional<glm::vec3> RE4VRReloadAdv::dock_local(::REManagedObject* weapon_tf,
                                                    int32_t wid) {
    const auto w = dock_world(weapon_tf, wid);

    if (!w.has_value()) {
        return std::nullopt;
    }

    glm::vec3 gp{};
    glm::quat gr{};

    if (!get_vec3(weapon_tf, "get_Position", gp) || !get_quat(weapon_tf, "get_Rotation", gr)) {
        return std::nullopt;
    }

    return glm::conjugate(gr) * (*w - gp);
}

RE4VRReloadAdv::WCfg& RE4VRReloadAdv::wcfg(int32_t wid) {
    auto it = m_weapons.find(wid);

    if (it == m_weapons.end()) {
        it = m_weapons.emplace(wid, WCfg{}).first;
    }

    return it->second;
}

// ============================================================================
// Shell-Insert-Keyframes (Lua Z.185-330)
// ============================================================================

std::vector<RE4VRReloadAdv::Key>& RE4VRReloadAdv::shell_keys(int32_t wid) {
    return m_shell_keys[wid];
}

// Nutzt die Waffe die Keyframe-Bahn? Allowlist + mind. 1 Keyframe -> sonst
// faellt reload auf den alten Slide.
bool RE4VRReloadAdv::has_shell_keys(int32_t wid) {
    if (uses_rev_insert(wid)) {
        return true;   // [INSERT = EJECT RUECKWAERTS]
    }

    if (!is_keyframe_insert(wid)) {
        return false;
    }

    const auto it = m_shell_keys.find(wid);

    return it != m_shell_keys.end() && !it->second.empty();
}

bool RE4VRReloadAdv::shell_pose_at(int32_t wid, float tt, Key& out) {
    // [INSERT = EJECT RUECKWAERTS] Zeit spiegeln und aus der Auswurf-Bahn
    // lesen: tt=0 (Einschub-Start) landet auf dem LETZTEN Eject-Keyframe (Mag
    // frei), tt=1 auf dem ersten (Mag steckt).
    if (uses_rev_insert(wid)) {
        return eject_pose_at(wid, 1.0f - tt, out);
    }

    const auto it = m_shell_keys.find(wid);

    return it != m_shell_keys.end() && pose_at(it->second, tt, out);
}

bool RE4VRReloadAdv::apply_shell_keys(::REManagedObject* weapon_tf,
                                      ::REManagedObject* joint, int32_t wid, float tt) {
    if (weapon_tf == nullptr || joint == nullptr) {
        return false;
    }

    Key k{};

    if (!shell_pose_at(wid, tt, k)) {
        return false;
    }

    return apply_keys(weapon_tf, joint, k);
}

// Keyframe an den aktuellen Tuning-Werten ANHAENGEN (Reihenfolge = Bahn:
// 1 = Start/Andockpunkt ... n = Kammer).
void RE4VRReloadAdv::shell_add_key(int32_t wid) {
    shell_keys(wid).push_back(shell_live);
    save_cfg();
}

// [SHELL-KEYFRAMES] Preview: haelt den Shell-Joint an der Tuning-Lage (relativ
// zur Waffe). Joint + Waffen-Transform kommen als Globals aus reload.lua.
void RE4VRReloadAdv::shell_preview_apply() {
    const int32_t wid = static_cast<int32_t>(re4vr::lua_get_number("__re4_reload_ui_wid", 0.0));

    // Signalisiert reload2, den Patronen-Clone zu spawnen, solange der
    // Keyframe-Preview an ist -> ein Toggle blendet den Clone ein UND haelt ihn
    // an der Tuning-Lage.
    if (shell_preview && is_keyframe_insert(wid)) {
        re4vr::lua_set_number("__re4_shell_kf_preview", wid);
    } else {
        re4vr::lua_set_nil("__re4_shell_kf_preview");
    }

    // [SHELL_CLONE] Part-Index + Scale fuer den Mesh-Clone (reload4_dlc) jeden
    // Pass exponieren.
    re4vr::lua_set_number("__re4_shell_clone_part", shell_clone_part);
    re4vr::lua_set_number("__re4_shell_clone_scale", shell_clone_scale);

    if (!shell_preview) {
        return;
    }

    if (!is_keyframe_insert(wid)) {
        return;
    }

    preview_apply_pose(shell_live);
}

// ============================================================================
// Mag-Eject-Keyframes (Lua Z.332-487)
// ============================================================================

// [ADA: EIGENE KOPIE 2026-07-25 "kein Kuddelmuddel"] Adas Waffen haben EIGENE
// Keyframes, kein Nachschlagen bei Leon zur Laufzeit. Die Startwerte sind 1:1
// Leons Stand vom 2026-07-25 (6103<-4003, 6112<-4001), weil die Waffen
// baugleich sind. Ab dem ersten eigenen Keyframe in der JSON gilt nur noch die
// JSON -- das hier ist reine Erstbefuellung.
void RE4VRReloadAdv::seed_eject_keys() {
    static const std::pair<int32_t, std::array<Key, 2>> EJECT_SEED[] = {
        // Blacktail AC <- 4003
        {6103, {Key{0.0f, 0.000f, 0.000f, 0.0f, 0.0f, 0.0f},
                Key{0.0f, -0.102f, -0.029f, 0.0f, 0.0f, 0.0f}}},
        // 6104 (MP-AF) bewusst NICHT hier: ausgebaut, s. is_keyframe_eject.
        // Punisher MC <- 4001
        {6112, {Key{0.0f, 0.000f, 0.000f, 0.0f, 0.0f, 0.0f},
                Key{0.0f, -0.102f, -0.017f, 0.0f, 0.0f, 0.0f}}},
        // [MC_6300] XM96E1 <- 4001, damit die Bahn nicht auf den linearen
        // Slide zurueckfaellt. Eigener Satz -> spaeter unabhaengig nachtunbar.
        {6300, {Key{0.0f, 0.000f, 0.000f, 0.0f, 0.0f, 0.0f},
                Key{0.0f, -0.102f, -0.017f, 0.0f, 0.0f, 0.0f}}},
        // [MC_6301 2026-08-15] Blacktail AC (Mercenaries) <- 4003.
        {6301, {Key{0.0f, 0.000f, 0.000f, 0.0f, 0.0f, 0.0f},
                Key{0.0f, -0.102f, -0.029f, 0.0f, 0.0f, 0.0f}}},
    };

    // Nur befuellen, was noch gar nichts hat -- ein vorhandener (getunter) Satz
    // wird NIE angefasst.
    for (const auto& seed : EJECT_SEED) {
        const auto it = m_eject_keys.find(seed.first);

        if (it != m_eject_keys.end() && !it->second.empty()) {
            continue;
        }

        m_eject_keys[seed.first] = std::vector<Key>{seed.second.begin(), seed.second.end()};
    }

    // [MERCS-TMP 2026-09-09] Die Mercs-TMP (42001) startet als KOPIE von Leons
    // 4200 -- nicht als Verweis. Grund: mit <2 Keyframes faellt sie auf den
    // linearen Slide zurueck, die Bahn waere also erst nach dem Tunen wieder
    // da. So aendert sich im Spiel zunaechst NICHTS, und jede Aenderung an
    // 42001 laesst Leons 4200 unberuehrt. Ein bereits getunter Satz bleibt
    // (die Bedingung ist "leer", nicht "fehlt").
    if (const auto it = m_eject_keys.find(42001);
        it == m_eject_keys.end() || it->second.empty()) {
        const auto src = m_eject_keys.find(4200);

        if (src != m_eject_keys.end() && !src->second.empty()) {
            m_eject_keys[42001] = src->second;
        }
    }
}

std::vector<RE4VRReloadAdv::Key>& RE4VRReloadAdv::eject_keys(int32_t wid) {
    return m_eject_keys[wid];
}

// Nutzt die Waffe die Auswurf-Bahn? Allowlist + mind. 2 Keyframes (Start +
// Ende) -> sonst alter Slide.
bool RE4VRReloadAdv::has_eject_keys(int32_t wid) {
    if (!is_keyframe_eject(wid)) {
        return false;
    }

    const auto it = m_eject_keys.find(wid);

    return it != m_eject_keys.end() && it->second.size() >= 2;
}

bool RE4VRReloadAdv::uses_rev_insert(int32_t wid) {
    const auto it = m_rev_insert.find(wid);

    return it != m_rev_insert.end() && it->second && has_eject_keys(wid);
}

// Insert-Dauer fuer die Rueckwaerts-Bahn (sonst gilt weiter shell_dur).
std::optional<float> RE4VRReloadAdv::kf_insert_dur(int32_t wid) {
    if (uses_rev_insert(wid)) {
        return rev_insert_dur;
    }

    return std::nullopt;
}

bool RE4VRReloadAdv::eject_pose_at(int32_t wid, float tt, Key& out) {
    const auto it = m_eject_keys.find(wid);

    return it != m_eject_keys.end() && pose_at(it->second, tt, out);
}

bool RE4VRReloadAdv::apply_eject_keys(::REManagedObject* weapon_tf,
                                      ::REManagedObject* joint, int32_t wid, float tt) {
    if (weapon_tf == nullptr || joint == nullptr) {
        return false;
    }

    Key k{};

    if (!eject_pose_at(wid, tt, k)) {
        return false;
    }

    return apply_keys(weapon_tf, joint, k);
}

// Keyframe anhaengen (Reihenfolge = Bahn: 1 = Kammer ... n = frei/Fall-Start).
void RE4VRReloadAdv::eject_add_key(int32_t wid) {
    eject_keys(wid).push_back(eject_live);
    save_cfg();
}

// [MAG-EJECT-KEYFRAMES] Aktuelle Ruhelage des Mags (in der Kammer) als
// Tuning-Lage uebernehmen -- damit Keyframe #1 exakt dort sitzt, wo das Mag
// steckt, statt ihn von Hand zu suchen.
bool RE4VRReloadAdv::eject_grab_rest() {
    auto* joint = re4vr::lua_get_pointer("__re4_reload_shell_joint");
    auto* tf = re4vr::lua_get_pointer("__re4_reload_weapon_tf");

    if (joint == nullptr || tf == nullptr) {
        return false;
    }

    glm::vec3 jp{};
    glm::quat jr{};
    glm::vec3 gp{};
    glm::quat gr{};

    // jr wird nicht gerechnet, gehoert aber 1:1 zur Torwaechter-Bedingung.
    if (!get_vec3(joint, "get_Position", jp) || !get_quat(joint, "get_Rotation", jr)
        || !get_vec3(tf, "get_Position", gp) || !get_quat(tf, "get_Rotation", gr)) {
        return false;
    }

    const glm::vec3 rel = glm::conjugate(gr) * (jp - gp);

    eject_live.x = rel.x;
    eject_live.y = rel.y;
    eject_live.z = rel.z;

    // Rotation bewusst NICHT mitgelesen: gemessene Live-Rotationen fangen
    // Engine-State ein (siehe Red9-rest_z-Falle). Rot X/Y/Z bleiben, wie sie
    // stehen, und werden von Hand gesetzt.
    return true;
}

// [MAG-EJECT-KEYFRAMES] Preview: haelt den Mag-Joint an der Tuning-Lage. Fuer
// Pistolen ist __re4_reload_shell_joint == wep.mag_joint, also genau der Joint,
// den wir tunen wollen.
void RE4VRReloadAdv::eject_preview_apply() {
    const int32_t wid = static_cast<int32_t>(re4vr::lua_get_number("__re4_reload_ui_wid", 0.0));

    // Signal fuer reload.lua: das Mag ist verschoben -> NICHT als Ruhepose
    // erfassen (sonst Drift).
    if (eject_preview && is_keyframe_eject(wid)) {
        re4vr::lua_set_number("__re4_mag_eject_kf_preview", wid);
    } else {
        re4vr::lua_set_nil("__re4_mag_eject_kf_preview");
    }

    if (!eject_preview) {
        return;
    }

    if (!is_keyframe_eject(wid)) {
        return;
    }

    preview_apply_pose(eject_live);
}

// ============================================================================
// Persistenz (Lua Z.488-620)
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

// String-Key -> wid-Zahl (Lua: tonumber(kk)).
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

std::vector<RE4VRReloadAdv::Key> read_keys(const nlohmann::json& arr) {
    std::vector<RE4VRReloadAdv::Key> t2;

    for (const auto& kf : arr) {
        if (!kf.is_object()) {
            continue;
        }

        RE4VRReloadAdv::Key k{};
        k.x = jnum(kf, "x", 0.0f);
        k.y = jnum(kf, "y", 0.0f);
        k.z = jnum(kf, "z", 0.0f);
        k.rx = jnum(kf, "rx", 0.0f);
        k.ry = jnum(kf, "ry", 0.0f);
        k.rz = jnum(kf, "rz", 0.0f);
        t2.push_back(k);
    }

    return t2;
}

nlohmann::json write_keys(const std::vector<RE4VRReloadAdv::Key>& arr) {
    auto t2 = nlohmann::json::array();

    for (const auto& k : arr) {
        t2.push_back(nlohmann::json{{"x", k.x}, {"y", k.y}, {"z", k.z},
                                    {"rx", k.rx}, {"ry", k.ry}, {"rz", k.rz}});
    }

    return t2;
}

}   // namespace

void RE4VRReloadAdv::load_cfg() {
    const auto data = re4vr::json_load(CFG_PATH);

    if (!data.is_object()) {
        return;
    }

    // [KFH] Keyframe-Handposen pro Waffe (fehlt der Block: alles aus).
    if (const auto it = data.find("kf_hand"); it != data.end()) {
        kfh_from_json(*it);
    }

    // [SHELL-KEYFRAMES] eigene Bahn-Dauer laden.
    if (jhas_num(data, "shell_dur")) { shell_dur = jnum(data, "shell_dur", shell_dur); }
    if (jhas_num(data, "r9_anlauf")) { r9_anlauf = jnum(data, "r9_anlauf", r9_anlauf); }
    if (jhas_num(data, "shell_clone_part")) {
        shell_clone_part = static_cast<int32_t>(jnum(data, "shell_clone_part", 1.0f));
    }
    if (jhas_num(data, "shell_clone_scale")) {
        shell_clone_scale = jnum(data, "shell_clone_scale", shell_clone_scale);
    }

    // [SHELL-KEYFRAMES] geordnete Bahn-Keyframes pro Waffe laden
    // (String-Keys -> wid-Zahl).
    if (const auto it = data.find("shell_keys"); it != data.end() && it->is_object()) {
        m_shell_keys.clear();

        for (const auto& item : it->items()) {
            int32_t wn = 0;

            if (key_to_wid(item.key(), wn) && item.value().is_array()) {
                m_shell_keys[wn] = read_keys(item.value());
            }
        }
    }

    if (jhas_num(data, "eject_dur")) { eject_dur = jnum(data, "eject_dur", eject_dur); }

    // [INSERT = EJECT RUECKWAERTS] Schalter pro Waffe + eigene Einschub-Dauer.
    if (jhas_num(data, "rev_insert_dur")) {
        rev_insert_dur = jnum(data, "rev_insert_dur", rev_insert_dur);
    }

    if (const auto it = data.find("rev_insert_by_wid"); it != data.end() && it->is_object()) {
        // MERGE statt Ersetzen: der Code-Default bleibt die Basis (sonst raeumt
        // eine alte JSON ihn weg), die JSON entscheidet nur pro Waffe -- auch
        // explizites false, damit Abschalten erhalten bleibt.
        for (const auto& item : it->items()) {
            int32_t wn = 0;

            if (key_to_wid(item.key(), wn)) {
                m_rev_insert[wn] = item.value().is_boolean() && item.value().get<bool>();
            }
        }
    }

    if (const auto it = data.find("mag_eject_keys"); it != data.end() && it->is_object()) {
        m_eject_keys.clear();

        for (const auto& item : it->items()) {
            int32_t wn = 0;

            if (key_to_wid(item.key(), wn) && item.value().is_array()) {
                m_eject_keys[wn] = read_keys(item.value());
            }
        }
    }

    // [PUSH_POSE] universeller Block (nicht pro Waffe).
    if (const auto it = data.find("push"); it != data.end() && it->is_object()) {
        const auto& p = *it;

        if (const auto on = p.find("on"); on != p.end() && on->is_boolean()) {
            push.on = on->get<bool>();
        }

        push.in_dur = jnum(p, "in_dur", push.in_dur);
        push.hold = jnum(p, "hold", push.hold);
        push.out_dur = jnum(p, "out_dur", push.out_dur);
        push.release_dur = jnum(p, "release_dur", push.release_dur);
        push.curl = jnum(p, "curl", push.curl);
        push.thumb = jnum(p, "thumb", push.thumb);
        push.rx = jnum(p, "rx", push.rx);
        push.ry = jnum(p, "ry", push.ry);
        push.rz = jnum(p, "rz", push.rz);
        push.px = jnum(p, "px", push.px);
        push.py = jnum(p, "py", push.py);
        push.pz = jnum(p, "pz", push.pz);
        push.ada_px = jnum(p, "ada_px", push.ada_px);
        push.ada_py = jnum(p, "ada_py", push.ada_py);
        push.ada_pz = jnum(p, "ada_pz", push.ada_pz);
        push.reload_speed = jnum(p, "reload_speed", push.reload_speed);
        push.fall_grav_mult = jnum(p, "fall_grav_mult", push.fall_grav_mult);
        push.drop_momentum = jnum(p, "drop_momentum", push.drop_momentum);

        // [POSE1] gecapturte Bones (kopierte "pose1") uebernehmen, wenn
        // vorhanden. Je Bone ein VIERERARRAY in der Reihenfolge w,x,y,z.
        if (const auto b = p.find("bones"); b != p.end() && b->is_object() && !b->empty()) {
            m_push_bones_json.clear();

            for (const auto& item : b->items()) {
                const auto& q = item.value();

                if (q.is_array() && q.size() >= 4) {
                    m_push_bones_json[item.key()] =
                        glm::quat{q[0].get<float>(), q[1].get<float>(),
                                  q[2].get<float>(), q[3].get<float>()};
                }
            }
        }
    }

    // [PUSH-Y PRO WAFFE] Zusatz-Hoehe pro WeaponID laden.
    if (const auto it = data.find("push_y_by_wid"); it != data.end() && it->is_object()) {
        push_y_by_wid.clear();

        for (const auto& item : it->items()) {
            int32_t wn = 0;

            if (key_to_wid(item.key(), wn) && item.value().is_number()) {
                push_y_by_wid[wn] = item.value().get<float>();
            }
        }
    }

    if (const auto it = data.find("docks"); it != data.end() && it->is_object()) {
        for (const auto& item : it->items()) {
            int32_t wid = 0;
            const auto& v = item.value();

            if (!key_to_wid(item.key(), wid) || !v.is_object()) {
                continue;
            }

            Dock d{};
            const auto jn = v.find("joint");
            d.joint = (jn != v.end() && jn->is_string()) ? jn->get<std::string>() : "_03";
            d.x = jnum(v, "x", 0.0f);
            d.y = jnum(v, "y", -0.092f);
            d.z = jnum(v, "z", -0.061f);
            m_docks[wid] = d;
        }
    }

    const auto wit = data.find("weapons");

    if (wit == data.end() || !wit->is_object()) {
        return;
    }

    for (const auto& item : wit->items()) {
        int32_t wid = 0;
        const auto& v = item.value();

        if (!key_to_wid(item.key(), wid) || !v.is_object()) {
            continue;
        }

        WCfg w{};
        const auto ex = v.find("exit");

        if (ex != v.end() && ex->is_object()) {
            w.exit = glm::vec3{jnum(*ex, "x", 0.0f), jnum(*ex, "y", -0.10f), jnum(*ex, "z", 0.0f)};
        } else {
            w.exit = glm::vec3{0.0f, -0.10f, 0.0f};
        }

        w.slide_dur = jnum(v, "slide_dur", 0.18f);
        w.gravity = jnum(v, "gravity", 9.8f);
        w.fall_dist = jnum(v, "fall_dist", 0.85f);
        w.fall_dur = jnum(v, "fall_dur", 0.55f);

        // ACHTUNG: hier gelten NICHT die land_default-Werte -- eine JSON ohne
        // land-Block ergibt in Lua on=false und rx/ry/rz = 0.
        const auto ld = v.find("land");
        const bool has_land = (ld != v.end() && ld->is_object());
        w.land_on = has_land && ld->contains("on") && (*ld)["on"].is_boolean()
                    && (*ld)["on"].get<bool>();
        w.land_rx = has_land ? jnum(*ld, "rx", 0.0f) : 0.0f;
        w.land_ry = has_land ? jnum(*ld, "ry", 0.0f) : 0.0f;
        w.land_rz = has_land ? jnum(*ld, "rz", 0.0f) : 0.0f;

        m_weapons[wid] = w;
    }
}

void RE4VRReloadAdv::save_cfg() {
    nlohmann::json out = nlohmann::json::object();

    for (const auto& entry : m_weapons) {
        const auto& v = entry.second;
        out[std::to_string(entry.first)] = {
            {"exit", {{"x", v.exit.x}, {"y", v.exit.y}, {"z", v.exit.z}}},
            {"slide_dur", v.slide_dur},
            {"gravity", v.gravity},
            {"fall_dist", v.fall_dist},
            {"fall_dur", v.fall_dur},
            {"land", {{"on", v.land_on}, {"rx", v.land_rx}, {"ry", v.land_ry}, {"rz", v.land_rz}}},
        };
    }

    nlohmann::json dout = nlohmann::json::object();

    for (const auto& entry : m_docks) {
        const auto& d = entry.second;
        dout[std::to_string(entry.first)] = {{"joint", d.joint}, {"x", d.x}, {"y", d.y}, {"z", d.z}};
    }

    // [SHELL-KEYFRAMES] geordnete Bahn-Keyframes pro Waffe persistieren.
    nlohmann::json skout = nlohmann::json::object();

    for (const auto& entry : m_shell_keys) {
        skout[std::to_string(entry.first)] = write_keys(entry.second);
    }

    // [MAG-EJECT-KEYFRAMES] Auswurf-Bahn pro Waffe persistieren.
    nlohmann::json ekout = nlohmann::json::object();

    for (const auto& entry : m_eject_keys) {
        ekout[std::to_string(entry.first)] = write_keys(entry.second);
    }

    // [PUSH-Y PRO WAFFE] nur Eintraege != 0 persistieren.
    nlohmann::json pyout = nlohmann::json::object();

    for (const auto& entry : push_y_by_wid) {
        if (entry.second != 0.0f) {
            pyout[std::to_string(entry.first)] = entry.second;
        }
    }

    // [INSERT = EJECT RUECKWAERTS] true UND false persistieren (s. load_cfg).
    nlohmann::json riout = nlohmann::json::object();

    for (const auto& entry : m_rev_insert) {
        riout[std::to_string(entry.first)] = entry.second;
    }

    nlohmann::json pout = {
        {"on", push.on},
        {"in_dur", push.in_dur}, {"hold", push.hold}, {"out_dur", push.out_dur},
        {"release_dur", push.release_dur},
        {"curl", push.curl}, {"thumb", push.thumb},
        {"rx", push.rx}, {"ry", push.ry}, {"rz", push.rz},
        {"px", push.px}, {"py", push.py}, {"pz", push.pz},
        {"ada_px", push.ada_px}, {"ada_py", push.ada_py}, {"ada_pz", push.ada_pz},
        {"reload_speed", push.reload_speed},
        {"fall_grav_mult", push.fall_grav_mult},
        {"drop_momentum", push.drop_momentum},
    };

    if (!m_push_bones_json.empty()) {
        nlohmann::json bones = nlohmann::json::object();

        for (const auto& entry : m_push_bones_json) {
            const auto& q = entry.second;
            bones[entry.first] = nlohmann::json::array({q.w, q.x, q.y, q.z});
        }

        pout["bones"] = bones;
    }

    nlohmann::json d = {
        {"weapons", out},
        {"push", pout},
        {"docks", dout},
        {"shell_keys", skout},
        {"shell_dur", shell_dur},
        {"r9_anlauf", r9_anlauf},
        {"shell_clone_part", shell_clone_part},
        {"shell_clone_scale", shell_clone_scale},
        {"mag_eject_keys", ekout},
        {"eject_dur", eject_dur},
        {"push_y_by_wid", pyout},
        {"rev_insert_by_wid", riout},
        {"rev_insert_dur", rev_insert_dur},
        {"kf_hand", kfh_to_json()},   // [KFH]
    };

    re4vr::json_save(CFG_PATH, d);
}

// ============================================================================
// [PUSH_POSE] Hand-Pose beim Reinschieben (Lua Z.621-856)
// ============================================================================

// [POSE1] Die Nachdrueck-Pose ist die gecapturte "pose1" (linke Hand). Fehlt der
// Block (geloeschte JSON), faellt es auf die generierte flache Hand zurueck.
// Linke Hand: Finger beugen um -Z, L_Thumb1 um +X, L_Thumb2/3 um +Y.
std::unordered_map<std::string, glm::quat> RE4VRReloadAdv::push_bones() {
    if (!m_push_bones_json.empty()) {
        return m_push_bones_json;
    }

    std::unordered_map<std::string, glm::quat> b;
    b["L_Palm"] = push_quat(0.0f, 4);

    for (const char* pre : {"L_IndexF", "L_MiddleF", "L_RingF", "L_PinkyF"}) {
        for (int i = 1; i <= 3; ++i) {
            // -Z = einrollen (links)
            b[std::string{pre} + std::to_string(i)] = push_quat(push.curl, 4, -1.0f);
        }
    }

    b["L_Thumb1"] = push_quat(push.thumb, 2);          // +X
    b["L_Thumb2"] = push_quat(push.thumb * 0.5f, 3);   // +Y
    b["L_Thumb3"] = push_quat(push.thumb * 0.5f, 3);

    return b;
}

// Von reload / reload4_dlc beim Insert-Start gerufen, mit der WeaponID der
// aktuellen Waffe. Ohne passende ID passiert nichts -> lieber keine Geste als
// eine falsche.
void RE4VRReloadAdv::start_push(int32_t wid) {
    if (!push.on) {
        return;
    }

    if (!is_push_wid(wid)) {
        return;
    }

    m_push_t0 = clock_now();

    // [PUSH-Y PRO WAFFE 2026-07-25] Die Waffe, zu der DIESE Geste gehoert,
    // festhalten. Sie ist die verlaesslichste Quelle fuer den Y-Zusatz:
    // __re4_reload_ui_wid wird von fuenf Scripten jeden Frame beschrieben, und
    // publish_dock laeuft mehrfach pro Frame in fremden Passes.
    push_wid = wid;
}

void RE4VRReloadAdv::stop_push() {
    m_push_t0.reset();
}

// [MANUAL_INSERT 2026-08-15] Beim Einschieben VON HAND dauert der Push so lange,
// wie der Spieler braucht -- die feste Bahn Rein/Halten/Zurueck passt dafuer
// nicht. Alles andere bleibt der bisherige Weg, es wird nur die Uhr angehalten.
void RE4VRReloadAdv::begin_push_hold(int32_t wid) {
    start_push(wid);                       // respektiert push.on UND PUSH_WIDS
    push_hold = m_push_t0.has_value();     // nur halten, wenn die Geste anlief
}

void RE4VRReloadAdv::end_push_hold() {
    if (!push_hold) {
        return;
    }

    push_hold = false;

    // Uhr so stellen, dass genau noch die Zurueck-Phase uebrig ist -- danach
    // faellt push_t0 wie immer von selbst weg und der Links-Ausfade
    // (release_dur) uebernimmt.
    const float tm = time_mult();
    m_push_t0 = clock_now() - (push.in_dur * tm + push.hold * tm);
}

// UI-Test: Pose einmal komplett durchfahren (Rein/Halten/Zurueck).
void RE4VRReloadAdv::start_push_test() {
    m_push_t0 = clock_now();
}

// [POSE_CROSSFADE 2026-08-15] Der aktuelle Push-Blend wird veroeffentlicht,
// damit die Mag-in-Hand-Pose in motion GEGENLAEUFIG dazu ausblenden kann.
void RE4VRReloadAdv::push_apply() {
    float blend = 0.0f;

    if (push_tune) {
        blend = 1.0f;                                 // [TUNING] dauerhaft halten
    } else if (push_hold) {
        // [MANUAL_INSERT] Halten heisst NICHT sofort voll: erst die normale
        // Rein-Rampe (in_dur) fahren, DANN oben stehenbleiben. Vorher sprang der
        // Blend in einem Frame auf 1.0 -- damit war "Rein-Lerp s" wirkungslos.
        if (!m_push_t0.has_value()) {
            blend = 1.0f;
        } else {
            const float e2 = static_cast<float>(clock_now() - *m_push_t0);
            const float i2 = push.in_dur * time_mult();
            blend = (e2 < i2) ? (e2 / std::max(i2, 0.01f)) : 1.0f;
        }
    } else {
        if (!m_push_t0.has_value()) {
            re4vr::lua_set_nil("__re4_push_blend");
            return;
        }

        const float e = static_cast<float>(clock_now() - *m_push_t0);
        const float tm = time_mult();
        const float i_dur = push.in_dur * tm;
        const float h_dur = push.hold * tm;
        const float o_dur = push.out_dur * tm;
        const float total = i_dur + h_dur + o_dur;

        if (e >= total) {
            m_push_t0.reset();
            re4vr::lua_set_nil("__re4_push_blend");
            return;
        }

        if (e < i_dur) {
            blend = e / std::max(i_dur, 0.01f);
        } else if (e < i_dur + h_dur) {
            blend = 1.0f;
        } else {
            blend = 1.0f - ((e - i_dur - h_dur) / std::max(o_dur, 0.01f));
        }
    }

    if (blend <= 0.0f) {
        re4vr::lua_set_nil("__re4_push_blend");
        return;
    }

    re4vr::lua_set_number("__re4_push_blend", blend);

    // [BRUECKE] Der Joint-Writer liegt in reload.lua. Sobald der ganze
    // Reload-Block portiert ist, wird daraus ein direkter Aufruf des
    // Geschwister-Teils.
    re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones",
                               re4vr::wpose::pick("(push)", push_bones()), blend);   // [WPOSE]
}

// ============================================================================
// [KFH 2026-09-24] Keyframe-Handposen -- Nachbau von RE9 "Push pose" / "End pose"
//
// Keyframe 1: ab dem Andocken klemmt die linke Hand an der Shell/Patrone, die
// die Bahn entlanglaeuft (Versatz relativ zum Objekt), Finger "(kf1)".
// Letzter Keyframe: Hand relativ zur WAFFE, Finger "(kfend)"; im Bahn-Fenster
// fade_from..fade_to wird von K1 zur End-Lage uebergeblendet, nach dem
// Einrasten end_hold gehalten und ueber out_dur ausgeblendet.
// Das Hand-Ziel geht ueber dieselben drei Globals wie der Magazin-Push
// (__vr_slide_hand_world_pos/rot + __vr_slide_dock_blend_factor).
// Default AUS pro Waffe -- ohne "on" ist jede Funktion hier wirkungslos.
// ============================================================================

// Umfang: nur Waffen OHNE Magazin (Shells, Patronen, Clips, Bolzen). Erster
// Schritt = die Shotguns der Reload-Hauptmaschine.
bool RE4VRReloadAdv::kfh_in_scope(int32_t wid) {
    switch (wid) {
    case 4100: case 4101: case 4102: case 6001:   // W-870, Riot Gun, Striker, Skull Shaker
    case 4002: case 40021:                        // Red9 Stripper-Clip / Einzelpatrone (Reload2)
    case 61130: case 61131:                       // [KFH ADA] Samurai Edge Clip / Einzelpatrone (Reload5)
    case 4400: case 4500: case 4600:              // [KFH 2026-09-25] SR M1903, Butterfly, Bolt Thrower (Reload2)
    case 4502:                                    // Handcannon (Reload3)
    case 6100:                                    // Sawed-off W-870 (Reload4)
    case 6114:                                    // Hunting Rifle (Reload5)
        return true;
    default:
        return false;
    }
}

// [KFH ADA 2026-09-25] Adas Samurai Edge hat EIGENE Handpose-IDs (61130 Clip,
// 61131 Einzelpatrone), faehrt aber weiter Leons Red9-Bahn (4002 / 40021).
// Leons Handposen bleiben unter 4002 / 40021.
int32_t RE4VRReloadAdv::kfh_path_id(int32_t kfid) {
    if (kfid == 61130) {
        return 4002;
    }

    if (kfid == 61131) {
        return 40021;
    }

    return kfid;
}

const RE4VRReloadAdv::KfHand* RE4VRReloadAdv::kfh_cfg(int32_t wid) const {
    const auto it = m_kfh.find(wid);

    return (it != m_kfh.end()) ? &it->second : nullptr;
}

void RE4VRReloadAdv::kfh_begin(int32_t wid) {
    const auto* c = kfh_cfg(wid);

    // [END ALLEIN 2026-09-25] Auch nur "End" an startet: dann blendet die Hand
    // von der Mag-in-hand-Haltung direkt zur End-Pose, ohne Keyframe-1-Pose.
    if (!kfh_in_scope(wid) || c == nullptr || !(c->on || c->end_on)
        || !has_shell_keys(kfh_path_id(wid))) {
        return;
    }

    m_kfr = KfhRun{};
    m_kfr.active = true;
    m_kfr.wid = wid;
    m_kfr.t0 = clock_now();
    m_kfr.prog_t = m_kfr.t0;
}

void RE4VRReloadAdv::kfh_prog(float t) {
    if (!m_kfr.active) {
        return;
    }

    m_kfr.prog = std::clamp(t, 0.0f, 1.0f);
    m_kfr.prog_t = clock_now();
}

void RE4VRReloadAdv::kfh_end() {
    if (!m_kfr.active) {
        return;
    }

    m_kfr.active = false;

    const auto* c = kfh_cfg(m_kfr.wid);
    auto* wtf = (m_kfh_wtf != nullptr && (clock_now() - m_kfh_wtf_t) < 0.3)
        ? m_kfh_wtf : re4vr::lua_get_pointer("__re4_reload_weapon_tf");
    glm::vec3 gp{};
    glm::quat gr{};

    if (c == nullptr || wtf == nullptr || !get_vec3(wtf, "get_Position", gp)
        || !get_quat(wtf, "get_Rotation", gr)) {
        return;
    }

    m_kfr.end_mode = c->end_on;

    // Ohne End-Pose: die letzte Hand-Lage waffenlokal einfrieren und von dort
    // ausblenden (sonst spraenge die Hand beim Einrasten).
    if (!m_kfr.end_mode) {
        if (!m_kfr.have_last) {
            return;
        }

        m_kfr.frz_p = glm::conjugate(gr) * (m_kfr.last_p - gp);
        m_kfr.frz_r = glm::normalize(glm::conjugate(gr) * m_kfr.last_r);
    }

    m_kfr.end_t0 = clock_now();
}

void RE4VRReloadAdv::kfh_cancel() {
    m_kfr.active = false;
    m_kfr.end_t0.reset();
}

// Hand-Ziel + Blend dieses Moments; setzt nebenbei den Finger-Blend.
bool RE4VRReloadAdv::kfh_target(glm::vec3& p, glm::quat& r, float& b) {
    m_kfh_fblend = 0.0f;
    m_kfh_fend = 0.0f;

    // [KFH R9] Waffe: vom fuehrenden Modul (frisch gemeldet), sonst die der
    // Reload-Hauptmaschine.
    const double now = clock_now();
    auto* wtf = (m_kfh_wtf != nullptr && (now - m_kfh_wtf_t) < 0.3)
        ? m_kfh_wtf : re4vr::lua_get_pointer("__re4_reload_weapon_tf");
    glm::vec3 gp{};
    glm::quat gr{};

    if (wtf == nullptr || !get_vec3(wtf, "get_Position", gp) || !get_quat(wtf, "get_Rotation", gr)) {
        return false;
    }

    const auto obj_hand = [&](int32_t wid, float tt, const KfHand& c, glm::vec3& hp,
                              glm::quat& hr) -> bool {
        Key k{};

        if (!shell_pose_at(kfh_path_id(wid), tt, k)) {   // [KFH ADA]
            return false;
        }

        const glm::vec3 op = gp + (gr * glm::vec3{k.x, k.y, k.z});
        const glm::quat orr = glm::normalize(gr * quat_from_euler(k.rx, k.ry, k.rz));
        hp = op + (orr * glm::vec3{c.px, c.py, c.pz});
        hr = glm::normalize(orr * quat_from_euler(c.rx, c.ry, c.rz));

        return true;
    };

    const auto end_hand = [&](const KfHand& c, glm::vec3& hp, glm::quat& hr) {
        hp = gp + (gr * glm::vec3{c.epx, c.epy, c.epz});
        hr = glm::normalize(gr * quat_from_euler(c.erx, c.ery, c.erz));
    };

    // Einstell-Schalter: Hand dauerhaft an Keyframe 1 bzw. am letzten Keyframe.
    if (m_kfh_force != 0) {
        const int32_t wid = kfh_ui_id();   // [KFH] ui_wid ist hier schon wieder nil (gemessen)
        const auto* c = kfh_cfg(wid);

        if (!kfh_in_scope(wid) || c == nullptr) {
            return false;
        }

        if (m_kfh_force == 1) {
            if (!obj_hand(wid, 0.0f, *c, p, r)) {
                return false;
            }
        } else {
            end_hand(*c, p, r);
        }

        b = 1.0f;
        m_kfh_fblend = 1.0f;
        m_kfh_fend = (m_kfh_force == 2) ? 1.0f : 0.0f;
        m_kfh_fwid = wid;

        return true;
    }

    if (m_kfr.active) {
        const auto* c = kfh_cfg(m_kfr.wid);

        // Kein Fortschritt mehr gemeldet = Einlegen abgebrochen (jeder Abbruch-
        // Weg der Reload-Maschine, ohne dort einzeln einzugreifen).
        if (c == nullptr || !(c->on || c->end_on) || (now - m_kfr.prog_t) > 0.25
            || (now - m_kfr.t0) > 8.0) {
            m_kfr.active = false;

            if (c != nullptr && m_kfr.have_last) {
                m_kfr.end_mode = false;
                m_kfr.frz_p = glm::conjugate(gr) * (m_kfr.last_p - gp);
                m_kfr.frz_r = glm::normalize(glm::conjugate(gr) * m_kfr.last_r);
                m_kfr.end_t0 = now - static_cast<double>(c->end_hold);   // direkt ausblenden
            }

            return false;
        }

        // [END ALLEIN] Ohne Keyframe-1-Pose: Hand + Finger laufen allein ueber
        // das Bahn-Fenster fade_from..fade_to von der Mag-in-hand-Haltung zur
        // End-Pose (Blend 0 -> 1), ohne Fenster ueber die ganze Bahn.
        if (!c->on) {
            float e = m_kfr.prog;

            if (c->fade_to > c->fade_from) {
                e = std::clamp((m_kfr.prog - c->fade_from) / (c->fade_to - c->fade_from), 0.0f, 1.0f);
            }

            e = ease(e);
            end_hand(*c, p, r);
            b = e;
            m_kfr.last_p = p;
            m_kfr.last_r = r;
            m_kfr.have_last = true;
            m_kfh_fblend = e;
            m_kfh_fend = 1.0f;
            m_kfh_fwid = m_kfr.wid;

            return e > 0.0f;
        }

        glm::vec3 p1{};
        glm::quat r1{};

        if (!obj_hand(m_kfr.wid, m_kfr.prog, *c, p1, r1)) {
            return false;
        }

        float bin = 1.0f;

        if (c->in_dur > 0.001f) {
            bin = ease(std::clamp(static_cast<float>(now - m_kfr.t0) / c->in_dur, 0.0f, 1.0f));
        }

        float e = 0.0f;

        if (c->end_on && c->fade_to > c->fade_from) {
            e = ease(std::clamp((m_kfr.prog - c->fade_from) / (c->fade_to - c->fade_from), 0.0f, 1.0f));
        }

        if (e > 0.0f) {
            glm::vec3 pe{};
            glm::quat re{};
            end_hand(*c, pe, re);
            p = p1 + (pe - p1) * e;
            r = qnlerp(r1, re, e);
        } else {
            p = p1;
            r = r1;
        }

        b = bin;
        m_kfr.last_p = p;
        m_kfr.last_r = r;
        m_kfr.have_last = true;
        m_kfh_fblend = bin;
        m_kfh_fend = e;
        m_kfh_fwid = m_kfr.wid;

        return true;
    }

    if (m_kfr.end_t0.has_value()) {
        const auto* c = kfh_cfg(m_kfr.wid);

        if (c == nullptr) {
            m_kfr.end_t0.reset();
            return false;
        }

        const float el = static_cast<float>(now - *m_kfr.end_t0);
        const float hold = m_kfr.end_mode ? c->end_hold : 0.0f;
        const float out = std::max(c->out_dur, 0.001f);

        if (el >= hold + out) {
            m_kfr.end_t0.reset();
            return false;
        }

        b = (el < hold) ? 1.0f : 1.0f - ease(std::clamp((el - hold) / out, 0.0f, 1.0f));

        if (m_kfr.end_mode) {
            end_hand(*c, p, r);
            m_kfh_fend = 1.0f;
        } else {
            p = gp + (gr * m_kfr.frz_p);
            r = glm::normalize(gr * m_kfr.frz_r);
            m_kfh_fend = 0.0f;
        }

        m_kfh_fblend = b;
        m_kfh_fwid = m_kfr.wid;

        return b > 0.0f;
    }

    return false;
}

bool RE4VRReloadAdv::kfh_publish() {
    glm::vec3 p{};
    glm::quat r{};
    float b = 0.0f;

    if (!kfh_target(p, r, b) || b <= 0.001f) {
        return false;
    }

    re4vr::lua_set_vec3("__vr_slide_hand_world_pos", p);
    re4vr::lua_set_quat("__vr_slide_hand_world_rot", r);
    re4vr::lua_set_number("__vr_slide_dock_blend_factor", b);

    return true;
}

// Finger: "(kf1)" bzw. "(kfend)" der Waffe, ohne eigenen Satz die
// Mag-in-hand-Finger. Laeuft in den spaeten Paessen (wie push_apply).
void RE4VRReloadAdv::kfh_fingers() {
    glm::vec3 p{};
    glm::quat r{};
    float b = 0.0f;

    if (!kfh_target(p, r, b) || m_kfh_fblend <= 0.001f) {
        return;
    }

    auto* m = RE4VRReloadMain::instance();

    if (m == nullptr) {
        return;
    }

    const int32_t wid = m_kfh_fwid;
    re4vr::wpose::Bones base{};
    m->kfh_base_bones(wid, base);

    re4vr::wpose::Bones k1{};

    if (!re4vr::wpose::own(wid, "(kf1)", &k1)) {
        k1 = base;
    }

    re4vr::wpose::Bones ke{};

    if (!re4vr::wpose::own(wid, "(kfend)", &ke)) {
        ke = k1;
    }

    re4vr::wpose::Bones out = k1;

    if (m_kfh_fend > 0.0f) {
        for (const auto& e : ke) {
            const auto it = out.find(e.first);
            out[e.first] = (it != out.end()) ? qnlerp(it->second, e.second, m_kfh_fend) : e.second;
        }
    }

    const bool wrote = !out.empty() && m->apply_pose_bones(out, m_kfh_fblend);

    // [KFH-DIAG] Messpunkt fuer die Lua-Sonde: was hier geschrieben wird.
    {
        char buf[200];
        const auto it = out.find("L_IndexF1");
        const glm::quat q = (it != out.end()) ? it->second : glm::quat{0.0f, 0.0f, 0.0f, 0.0f};
        std::snprintf(buf, sizeof(buf), "wid=%d fb=%.2f fe=%.2f n=%d own=%d wrote=%d idx=%.3f,%.3f,%.3f,%.3f",
                      wid, m_kfh_fblend, m_kfh_fend, static_cast<int>(out.size()),
                      re4vr::wpose::own(wid, "(kf1)") ? 1 : 0, wrote ? 1 : 0, q.w, q.x, q.y, q.z);
        re4vr::lua_set_string("__re4_kfh_fdbg", buf);
    }
}

// Force: Shell/Patrone an Keyframe 1 bzw. am letzten Keyframe parken und
// sichtbar halten (derselbe Weg wie die Keyframe-Vorschau).
void RE4VRReloadAdv::kfh_force_park() {
    if (m_kfh_force == 0) {
        return;
    }

    const int32_t wid = kfh_ui_id();   // [KFH] ui_wid ist hier schon wieder nil (gemessen)

    // Nur die Shotguns der Reload-Hauptmaschine parken hier ihren Waffen-Joint;
    // Clone-Waffen (Red9 ...) parkt ihr eigenes Modul ueber kfh_force_key.
    // [KFH 2026-09-25] + Adas Sawed-off (Reload4): sichtbar ist dort ebenfalls
    // der native Shell-Joint, nicht der Klon (gemeldet: Klon unsichtbar).
    if (!(wid == 4100 || wid == 4101 || wid == 4102 || wid == 6001 || wid == 6100)) {
        return;
    }

    Key k{};

    if (shell_pose_at(wid, (m_kfh_force == 1) ? 0.0f : 1.0f, k)) {
        preview_apply_pose(k);
    }
}

void RE4VRReloadAdv::kfh_set_weapon(::REManagedObject* tf) {
    m_kfh_wtf = tf;
    m_kfh_wtf_t = clock_now();
}

bool RE4VRReloadAdv::kfh_force_key(int32_t kfid, Key& out) {
    if (m_kfh_force == 0 || kfh_cfg(kfid) == nullptr) {
        return false;
    }

    return shell_pose_at(kfh_path_id(kfid), (m_kfh_force == 1) ? 0.0f : 1.0f, out);   // [KFH ADA]
}

void RE4VRReloadAdv::kfh_set_base(int32_t kfid, const re4vr::wpose::Bones& b) {
    m_kfh_base[kfid] = b;
}

const re4vr::wpose::Bones* RE4VRReloadAdv::kfh_base(int32_t kfid) const {
    const auto it = m_kfh_base.find(kfid);

    return (it != m_kfh_base.end()) ? &it->second : nullptr;
}

int32_t RE4VRReloadAdv::kfh_ui_id() {
    const int32_t w = get_equip_wid().value_or(0);

    const bool single = re4vr::lua_get_string("__re4_r9_kf_mode") == "single";

    if (w == 4002) {   // Red9
        return single ? 40021 : 4002;
    }

    if (w == 6113) {   // [KFH ADA] Samurai Edge: eigene Handposen, Red9-Bahn
        return single ? 61131 : 61130;
    }

    return w;
}

// In den Adv-Paessen NACH allen Reload-Modulen veroeffentlichen -- Module wie
// Reload2 leeren die Dock-Globals selbst, solange ihr eigener Dock nicht
// laeuft. Fallende Flanke: einmal leeren + Ausfade-Zeitstempel.
void RE4VRReloadAdv::kfh_publish_adv() {
    const bool on = kfh_publish();

    if (on) {
        re4vr::lua_set_nil("__re4_reload_lexit_t");
        re4vr::lua_set_bool("__re4_kfh_active", true);   // Motion: keine Stuetzhand-Pose drueber
    } else if (m_kfh_pub_prev) {
        re4vr::lua_set_nil("__re4_kfh_active");
        re4vr::lua_set_nil("__vr_slide_hand_world_pos");
        re4vr::lua_set_nil("__vr_slide_hand_world_rot");
        re4vr::lua_set_number("__vr_slide_dock_blend_factor", 0.0);
        re4vr::lua_set_number("__re4_reload_lexit_t", clock_now());
    }

    m_kfh_pub_prev = on;
}

nlohmann::json RE4VRReloadAdv::kfh_to_json() const {
    nlohmann::json o = nlohmann::json::object();

    for (const auto& e : m_kfh) {
        const auto& c = e.second;
        o[std::to_string(e.first)] = {
            {"on", c.on}, {"in_dur", c.in_dur},
            {"px", c.px}, {"py", c.py}, {"pz", c.pz},
            {"rx", c.rx}, {"ry", c.ry}, {"rz", c.rz},
            {"end_on", c.end_on},
            {"epx", c.epx}, {"epy", c.epy}, {"epz", c.epz},
            {"erx", c.erx}, {"ery", c.ery}, {"erz", c.erz},
            {"fade_from", c.fade_from}, {"fade_to", c.fade_to},
            {"end_hold", c.end_hold}, {"out_dur", c.out_dur},
        };
    }

    return o;
}

void RE4VRReloadAdv::kfh_from_json(const nlohmann::json& j) {
    if (!j.is_object()) {
        return;
    }

    m_kfh.clear();

    for (const auto& item : j.items()) {
        int32_t wid = 0;
        const auto& v = item.value();

        if (!key_to_wid(item.key(), wid) || !v.is_object()) {
            continue;
        }

        KfHand c{};
        const auto jb = [&](const char* k, bool def) {
            const auto it = v.find(k);
            return (it != v.end() && it->is_boolean()) ? it->get<bool>() : def;
        };

        c.on = jb("on", c.on);
        c.in_dur = jnum(v, "in_dur", c.in_dur);
        c.px = jnum(v, "px", c.px); c.py = jnum(v, "py", c.py); c.pz = jnum(v, "pz", c.pz);
        c.rx = jnum(v, "rx", c.rx); c.ry = jnum(v, "ry", c.ry); c.rz = jnum(v, "rz", c.rz);
        c.end_on = jb("end_on", c.end_on);
        c.epx = jnum(v, "epx", c.epx); c.epy = jnum(v, "epy", c.epy); c.epz = jnum(v, "epz", c.epz);
        c.erx = jnum(v, "erx", c.erx); c.ery = jnum(v, "ery", c.ery); c.erz = jnum(v, "erz", c.erz);
        c.fade_from = jnum(v, "fade_from", c.fade_from);
        c.fade_to = jnum(v, "fade_to", c.fade_to);
        c.end_hold = jnum(v, "end_hold", c.end_hold);
        c.out_dur = jnum(v, "out_dur", c.out_dur);
        m_kfh[wid] = c;
    }
}

// Zeitfaktor -> Multiplikator fuer Dauern (Speed 1.0 = unveraendert;
// 0.5 = doppelt so lang).
float RE4VRReloadAdv::time_mult() {
    float sp = push.reload_speed;

    if (sp < 0.2f) {
        sp = 0.2f;
    }

    return 1.0f / sp;
}

// Aktuell gefuehrte Waffe: dieselbe Quelle wie die UI-Trees hier (reload und
// reload4_dlc setzen __re4_reload_ui_wid jeden Frame).
int32_t RE4VRReloadAdv::push_wid_now() {
    if (const auto v = re4vr::lua_get_number_opt("__re4_reload_ui_wid"); v.has_value()) {
        const int32_t w = static_cast<int32_t>(*v);

        // [MERCS-TMP 2026-09-09] Die virtuelle wid gilt NUR fuer die
        // Keyframe-Bahn. Einleit-Punkt und Push-Y laufen im Spiel weiter ueber
        // die echte wid (RE4VRReloadMain reicht dort m_wep.wid durch) -- also
        // hier zurueckmappen, sonst editiert die UI eine leere Karteileiche.
        return (w == 42001) ? 4200 : w;
    }

    if (const auto v = re4vr::lua_get_number_opt("__vr_equip_wid"); v.has_value()) {
        return static_cast<int32_t>(*v);
    }

    return 0;
}

// [PUSH-Y PRO WAFFE 2026-07-25] Der Push-Block bleibt universell -- nur die
// HOEHE (Y) bekommt pro WeaponID einen ZUSATZ-Wert. Default 0 => exakt das
// bisherige Verhalten. Reihenfolge der Quellen, absteigend verlaesslich:
//   1) explizit uebergebene wid  2) push_wid  3) __re4_reload_ui_wid
float RE4VRReloadAdv::push_y_extra(std::optional<int32_t> wid) {
    int32_t w = 0;

    if (wid.has_value()) {
        w = *wid;
    } else if (push_wid.has_value()) {
        w = *push_wid;
    } else {
        w = push_wid_now();
    }

    const auto it = push_y_by_wid.find(w);

    return it != push_y_by_wid.end() ? it->second : 0.0f;
}

// Positions-Offset fuer den AKTUELLEN Charakter (Leon = px/py/pz,
// Ada = px/py/pz + ada_*). Charakter kommt aus dem zentralen __re4_char_now.
void RE4VRReloadAdv::push_pos(std::optional<int32_t> wid, float& x, float& y, float& z) {
    const bool ada = (re4vr::lua_call_global_string("__re4_char_now") == "ada");
    const float ye = push_y_extra(wid);   // [PUSH-Y PRO WAFFE]

    if (!ada) {
        x = push.px;
        y = push.py + ye;
        z = push.pz;

        return;
    }

    x = push.px + push.ada_px;
    y = push.py + push.ada_py + ye;
    z = push.pz + push.ada_pz;
}

float RE4VRReloadAdv::push_blend() {
    if (push_tune) {
        return 1.0f;
    }

    // [MANUAL_INSERT] Hand bleibt am Magazin, bis es einrastet -- aber erst NACH
    // der Rein-Rampe (in_dur), damit der Weg dorthin geblendet wird und nicht
    // springt. Gegenstueck zu push_apply.
    if (push_hold && m_push_t0.has_value()) {
        const float e2 = static_cast<float>(clock_now() - *m_push_t0);
        const float i2 = push.in_dur * time_mult();

        if (e2 >= i2) {
            return 1.0f;
        }

        const float b2 = e2 / std::max(i2, 0.01f);

        return ease(b2);   // smoothstep, wie unten
    }

    if (!m_push_t0.has_value()) {
        return 0.0f;
    }

    const float e = static_cast<float>(clock_now() - *m_push_t0);
    const float tm = time_mult();
    const float i_dur = push.in_dur * tm;
    const float h_dur = push.hold * tm;
    const float o_dur = push.out_dur * tm;
    const float total = i_dur + h_dur + o_dur;

    if (e >= total) {
        return 0.0f;
    }

    float b = 0.0f;

    if (e < i_dur) {
        b = e / std::max(i_dur, 0.01f);
    } else if (e < i_dur + h_dur) {
        b = 1.0f;
    } else {
        b = 1.0f - ((e - i_dur - h_dur) / std::max(o_dur, 0.01f));
    }

    if (b < 0.0f) {
        b = 0.0f;
    }

    return ease(b);   // smoothstep, wie der Slide-Dock-Blend
}

// ============================================================================
// Drop-Maschine (Lua Z.857-990)
// ============================================================================

// [FLOOR] Boden-Y unter dem Spieler = Player-Body-Root (die Fuesse stehen am
// Boden). Damit das Mag bis zum Boden faellt statt auf einer fixen Distanz zu
// stoppen. Eigener Singleton-Cache -- diese Stelle geht bewusst NICHT ueber den
// Frame-Cache.
std::optional<float> RE4VRReloadAdv::get_floor_y() {
    if (!re4vr::obj_ok(m_floor_cm)) {
        m_floor_cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(m_floor_cm, "getPlayerContextRef");
    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
    auto* tf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    glm::vec3 p{};

    if (tf != nullptr && get_vec3(tf, "get_Position", p)) {
        return p.y;
    }

    return std::nullopt;
}

// joint = via.Joint des Magazins, wid = WeaponID, dur_override = Slide-Dauer
// (z.B. Insert-Dauer), exit_local = absolutes lokales Ziel (Einleit-Punkt).
bool RE4VRReloadAdv::begin_drop(::REManagedObject* joint, int32_t wid,
                                const std::optional<float>& dur_override,
                                const std::optional<glm::vec3>& exit_local) {
    if (joint == nullptr) {
        return false;
    }

    glm::vec3 lp{};

    if (!get_vec3(joint, "get_LocalPosition", lp)) {
        return false;
    }

    const auto& w = wcfg(wid);
    auto& d = m_drop;
    d.joint = joint;
    d.wid = wid;
    d.lx0 = lp.x;
    d.ly0 = lp.y;
    d.lz0 = lp.z;

    // [EINLEIT-PUNKT ALS DROP-ZIEL 2026-07-23] Ist ein absolutes lokales Ziel
    // uebergeben (der Einleit-Punkt, exakt derselbe wie beim Reinsliden),
    // gleitet das Mag GENAU dorthin raus -- also rueckwaerts die Kammerachse
    // entlang, dann faellt es. Ohne Ziel: alter Weg (Ruhe + Exit-Vektor).
    if (exit_local.has_value()) {
        d.ex = exit_local->x;
        d.ey = exit_local->y;
        d.ez = exit_local->z;
    } else {
        // [RELATIV] Exit = Ruhe-Pos + Offset -> Mag gleitet nur um den Offset
        // (z.B. rein runter), NICHT auf eine absolute Lokal-Pos (die das Mag
        // quer in X/Z zerren wuerde).
        d.ex = lp.x + w.exit.x;
        d.ey = lp.y + w.exit.y;
        d.ez = lp.z + w.exit.z;
    }

    // [SLIDE-OUT = INSERT-SPEED] wenn das Hauptscript eine Dauer mitgibt
    // (Insert-Dauer), die nehmen -> Rausgleiten genauso schnell wie das
    // Reingleiten. Sonst per-Waffe slide_dur.
    // [DROP_MOMENTUM] frischer Drop -> alte Messung weg.
    d.vx = 0.0f;
    d.vz = 0.0f;
    d.prev_p.reset();
    d.prev_t = 0.0;
    d.land_t.reset();

    d.slide_dur = (dur_override.has_value() && *dur_override > 0.0f) ? *dur_override : w.slide_dur;
    d.gravity = w.gravity;
    d.fall_dist = w.fall_dist;
    d.fall_dur = w.fall_dur;

    // [MAG-EJECT-KEYFRAMES] Hat die Waffe eine Auswurf-Bahn, faehrt die Phase
    // "slide" diese ab (eigene Dauer, unabhaengig von insert_dur/slide_dur).
    // Sonst bleibt alles wie bisher.
    d.use_keys = has_eject_keys(wid);

    if (d.use_keys) {
        d.slide_dur = eject_dur;
    }

    // (Lua setzt hier zusaetzlich d.vx/d.vz/d.lpt zurueck -- Reste des
    // [INHERIT_VEL]-Trackers, die seit dem GRAVITY-DROP 2026-07-08 nirgends
    // mehr gelesen werden.)
    d.srot.reset();
    d.floor_y.reset();
    d.t0 = clock_now();
    d.phase = "slide";
    d.active = true;

    return true;
}

void RE4VRReloadAdv::cancel() {
    m_drop.active = false;
    m_drop.joint = nullptr;
    m_drop.phase.clear();
}

void RE4VRReloadAdv::tick() {
    auto& d = m_drop;

    if (!d.active || d.joint == nullptr) {
        return;
    }

    // [DROP_MOMENTUM 2026-09-09] Die Weltbewegung mitschreiben, solange das Mag
    // noch gefuehrt wird -- in beiden Slide-Wegen, dem linearen UND der
    // Keyframe-Bahn.
    //
    // GEMESSEN WIRD DIE WAFFE, NICHT DAS MAGAZIN. Das Magazin bewegt sich auf
    // der Auswurfbahn ohnehin, auch im Stand -- wer dessen Bewegung uebernimmt,
    // erbt den Auswurf-Schwung und schiesst das Mag im Stand von sich weg
    // (genau so getestet, 09.09.). Die Waffe traegt dagegen nur die Bewegung,
    // die von DIR kommt: Laufen, Drehen, Handbewegung.
    if (d.phase == "slide") {
        glm::vec3 pw{};
        auto* carrier = re4vr::lua_get_pointer("__re4_reload_weapon_tf");

        if (carrier != nullptr && get_vec3(carrier, "get_Position", pw)) {
            const double now = clock_now();

            if (d.prev_p.has_value()) {
                const double dt = now - d.prev_t;

                if (dt > 0.0005 && dt < 0.2) {
                    d.vx = static_cast<float>((pw.x - d.prev_p->x) / dt);
                    d.vz = static_cast<float>((pw.z - d.prev_p->z) / dt);
                }
            }

            d.prev_p = pw;
            d.prev_t = now;
        }
    }

    if (d.phase == "slide") {
        // [MAG-EJECT-KEYFRAMES 2026-07-24] Auswurf entlang der geordneten Bahn
        // statt linearem Zug. Linear ueber die Keyframes (wie beim Insert): die
        // Form steckt in den Keyframes selbst. Fehlt die Waffen-Transform,
        // faellt der Drop auf den alten Slide zurueck.
        if (d.use_keys) {
            auto* wtf = re4vr::lua_get_pointer("__re4_reload_weapon_tf");

            if (wtf != nullptr) {
                float t = static_cast<float>(clock_now() - d.t0) / std::max(d.slide_dur, 0.01f);

                if (t > 1.0f) {
                    t = 1.0f;
                }

                apply_eject_keys(wtf, d.joint, d.wid, t);

                if (t >= 1.0f) {
                    // Letzter Keyframe erreicht -> ab hier uebernimmt der Fall
                    // (identisch zum alten Pfad): Weltpose einfrieren, Boden-Y
                    // merken, Phase wechseln.
                    glm::vec3 p{};

                    if (get_vec3(d.joint, "get_Position", p)) {
                        d.sx = p.x;
                        d.sy = p.y;
                        d.sz = p.z;
                    }

                    glm::quat r{};

                    if (get_quat(d.joint, "get_Rotation", r)) {
                        d.srot = r;
                    }

                    d.floor_y = get_floor_y();

                    // [DROP_MOMENTUM] wie unten: Anteil aus Lua, Default 0.
                    {
                        const float fc = std::clamp(push.drop_momentum, 0.0f, 1.0f);

                        d.vx = std::clamp(d.vx * fc, -3.0f, 3.0f);
                        d.vz = std::clamp(d.vz * fc, -3.0f, 3.0f);
                    }

                    d.phase = "fall";
                    d.t0 = clock_now();
                }

                return;
            }

            d.use_keys = false;
        }

        // [GRAVITY-DROP 2026-07-08] KEIN Lauf-Impuls-Tracking mehr: das Mag
        // faellt am Ende senkrecht mit Gewicht (freier Fall), folgt NICHT mehr
        // der Spielerbewegung. Slide = reines Rausgleiten aus dem Schacht.
        float t = static_cast<float>(clock_now() - d.t0) / std::max(d.slide_dur, 0.01f);

        if (t > 1.0f) {
            t = 1.0f;
        }

        const float u = ease(t);

        // Rausgleiten entlang des (lokalen) Magazinschachts.
        set_vec3(d.joint, "set_LocalPosition",
                 glm::vec3{d.lx0 + (d.ex - d.lx0) * u,
                           d.ly0 + (d.ey - d.ly0) * u,
                           d.lz0 + (d.ez - d.lz0) * u});

        if (t >= 1.0f) {
            // ausgefahrene Welt-Position als Fall-Start snapshotten
            glm::vec3 p{};

            if (get_vec3(d.joint, "get_Position", p)) {
                d.sx = p.x;
                d.sy = p.y;
                d.sz = p.z;
            }

            // [ENTKOPPEL_ROT] Welt-Rotation im selben Moment einfrieren -> das
            // gefallene Mag folgt danach NICHT mehr der Waffenrotation.
            glm::quat r{};

            if (get_quat(d.joint, "get_Rotation", r)) {
                d.srot = r;
            }

            // [FLOOR] Boden-Ziel beim Fall-Start merken -> Mag faellt bis zum
            // Boden, nicht fixe Distanz.
            d.floor_y = get_floor_y();

            // [DROP_MOMENTUM] Anteil aus Lua, Default 0 = bisheriges Verhalten.
            // Gedeckelt, damit ein Ausrutscher im Regler das Mag nicht quer
            // durch den Raum schiesst.
            {
                const float fc = std::clamp(push.drop_momentum, 0.0f, 1.0f);

                d.vx = std::clamp(d.vx * fc, -3.0f, 3.0f);
                d.vz = std::clamp(d.vz * fc, -3.0f, 3.0f);
            }

            d.phase = "fall";
            d.t0 = clock_now();
        }

        return;
    }

    // Phase "fall": ECHTER Gravity-Fall (Gewicht). Das Mag faellt beschleunigt
    // SENKRECHT bis zum Boden und bleibt dann liegen. KEIN geerbter
    // Lauf-Impuls mehr (das Mitziehen sah kacke aus, 07-08).
    const float t = static_cast<float>(clock_now() - d.t0);
    const float g = d.gravity * push.fall_grav_mult;   // [FALL-GEWICHT]
    float fall = 0.5f * g * t * t;                     // s = 1/2 g t^2

    // [FLOOR] bis zum Boden (Exit-Y -> Boden + 2 cm Bodenfreiheit). Fallback =
    // fixe fall_dist ohne Boden-Y.
    float total = d.fall_dist;

    if (d.floor_y.has_value()) {
        total = d.sy - (*d.floor_y + 0.02f);

        if (total < 0.0f) {
            total = 0.0f;
        }
    }

    if (fall > total) {
        fall = total;   // am Boden anhalten (kein Durchfallen/Wegschiessen)
    }

    // Fall-Fortschritt 0..1 (nur fuer die Lande-Pose-Rotation)
    const float tf = (total > 1e-4f) ? (fall / total) : 1.0f;

    // SENKRECHT runter: Start-XZ halten.
    // [DROP_MOMENTUM] Die uebernommene Bewegung laeuft waehrend des Falls
    // konstant weiter -- das Mag folgt NICHT der Waffe, es behaelt nur den
    // Schwung, den es beim Loslassen hatte. Bei Anteil 0 steht hier eine 0 und
    // es faellt senkrecht wie bisher.
    //
    // [AM BODEN IST SCHLUSS] Der Zeitfaktor wird beim Aufschlag eingefroren --
    // sonst rutscht das Mag endlos ueber den Boden weiter (getestet 09.09.).
    // Aufgeschlagen ist es, sobald der Fall gedeckelt wurde.
    if (!d.land_t.has_value() && fall >= total && total > 0.0f) {
        d.land_t = t;
    }

    const float ht = d.land_t.value_or(t);

    set_vec3(d.joint, "set_Position",
             glm::vec3{d.sx + d.vx * ht, d.sy - fall, d.sz + d.vz * ht});

    // [ENTKOPPEL_ROT / LANDE_POSE] Weltrotation jeden Frame festhalten -> Mag
    // bleibt starr im Raum, dreht sich nicht mit der Waffe. Ist die Lande-Pose
    // aktiv, wird ueber den Fall (tf 0->1) von der Flug-Rotation in die
    // tunebare Liege-Pose (Welt-Euler) hineininterpoliert.
    if (d.srot.has_value()) {
        glm::quat r = *d.srot;
        const auto& lc = wcfg(d.wid);

        if (lc.land_on) {
            r = qnlerp(*d.srot, quat_from_euler(lc.land_rx, lc.land_ry, lc.land_rz), tf);
        }

        set_quat(d.joint, "set_Rotation", r);
    }
}

// ============================================================================
// Live-Preview (Lua Z.992-1047)
// ============================================================================

// [FRAME-CACHE 2026-08-17] Dieselben Objekte wurden pro Frame dutzendfach neu
// bei der Engine erfragt. Semantik unveraendert -- die alten Wege stehen als
// Fallback darunter.
::REManagedObject* RE4VRReloadAdv::get_ctx() {
    if (re4vr::fc::on()) {
        return re4vr::fc::ctx();
    }

    if (!re4vr::obj_ok(m_character_manager)) {
        m_character_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");
    }

    return re4vr::call_safe<::REManagedObject*>(m_character_manager, "getPlayerContextRef");
}

std::optional<int32_t> RE4VRReloadAdv::get_equip_wid() {
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

    // Das value__-Auspacken der Lua-Fassung entfaellt: ueber die TypeDB kommt
    // der Enum in C++ direkt als Integer zurueck.
    int32_t wid = 0;

    if (!re4vr::try_call<int32_t>(hu, "get_EquipWeaponID", wid)) {
        return std::nullopt;
    }

    return wid;
}

void RE4VRReloadAdv::start_preview(float seconds) {
    auto* j = m_current_mag_joint;

    if (j == nullptr) {
        return;
    }

    glm::vec3 lp{};

    if (!get_vec3(j, "get_LocalPosition", lp)) {
        return;
    }

    m_preview.joint = j;
    m_preview.lx0 = lp.x;
    m_preview.ly0 = lp.y;
    m_preview.lz0 = lp.z;
    m_preview.wid = get_equip_wid();
    m_preview.until_t = clock_now() + seconds;
    m_preview.active = true;
}

void RE4VRReloadAdv::tick_preview() {
    if (!m_preview.active) {
        return;
    }

    if (clock_now() >= m_preview.until_t || m_preview.joint == nullptr) {
        m_preview.active = false;
        m_preview.joint = nullptr;

        return;
    }

    // Ohne Waffen-ID kommt Lua an dieser Stelle nicht weiter (M.weapons[nil]
    // ist dort ein Laufzeitfehler) -- die Preview laeuft dann leer, bis until_t
    // abgelaufen ist.
    if (!m_preview.wid.has_value()) {
        return;
    }

    const auto& w = wcfg(*m_preview.wid);

    set_vec3(m_preview.joint, "set_LocalPosition",
             glm::vec3{m_preview.lx0 + w.exit.x, m_preview.ly0 + w.exit.y,
                       m_preview.lz0 + w.exit.z});
}

// ============================================================================
// Pass-Einstiege -- Reihenfolge exakt wie die Registrierungen in der Lua-Datei
// ============================================================================

void RE4VRReloadAdv::on_lock_scene_pre() {
    push_apply();
    kfh_publish_adv();   // [KFH]
}

void RE4VRReloadAdv::on_late_update() {
    shell_preview_apply();   // Lua Z.324
    eject_preview_apply();   // Lua Z.485
    push_apply();            // Lua Z.853
    tick_preview();          // Lua Z.1046
    kfh_force_park();        // [KFH]
    kfh_fingers();           // [KFH]
    kfh_publish_adv();       // [KFH]
}

// [WPOSE] "Erzwingen" aus dem Handposen-Baum
void RE4VRReloadAdv::wpose_force() {
    if (auto* m = RE4VRReloadMain::instance(); m != nullptr) {
        m->wpose_force_apply();
    }
}

void RE4VRReloadAdv::on_update_joint_expression() {
    push_apply();
    // [KFH] Parken im LETZTEN Pass wie insert_repos_late -- gemessen: nur in
    // LateUpdate/BeginRendering geparkt stand die Shell bei 0/0.047/0.163
    // statt an Keyframe 1 (0/0.022/0.152).
    kfh_force_park();
    kfh_fingers();   // [KFH]
    kfh_publish_adv();   // [KFH]
    wpose_force();   // [WPOSE]
}

void RE4VRReloadAdv::on_begin_rendering_pre() {
    push_apply();
    kfh_publish_adv();   // [KFH]
}

// [PASS-FIX 2026-07-21, per Probe-Log] Die Push-Pose WURDE geschrieben, war aber
// unsichtbar: motion wendet die Mag-/Rack-Hand-Pose im BeginRendering-POST-Pass
// an, der Pre-Pass laeuft davor -> motion schrieb jeden Frame drueber. Deshalb
// zusaetzlich im POST-Pass; da motion frueher laeuft, gewinnen wir dort.
void RE4VRReloadAdv::on_begin_rendering() {
    shell_preview_apply();   // Lua Z.325
    eject_preview_apply();   // Lua Z.486
    push_apply();            // Lua Z.856
    tick_preview();          // Lua Z.1047
    kfh_force_park();        // [KFH]
    kfh_fingers();           // [KFH]
    kfh_publish_adv();       // [KFH]
    wpose_force();           // [WPOSE] zuletzt: Erzwingen gewinnt
}

// ============================================================================
// UI (Lua Z.1049-1285)
// ============================================================================

void RE4VRReloadAdv::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Reload Adv")) {
        return;
    }

    // [WPOSE 2026-09-24] Handposen pro Waffe
    if (auto* m = RE4VRReloadMain::instance(); m != nullptr) {
        m->draw_wpose_ui();
    }

    const int32_t wid = get_equip_wid().value_or(4004);
    ImGui::Text("Bearbeite: wp%d", wid);

    auto& w = wcfg(wid);
    bool ch = false;

    ch |= ImGui::DragFloat("Exit X (lokal)", &w.exit.x, 0.001f, -0.5f, 0.5f, "%.4f");
    ch |= ImGui::DragFloat("Exit Y (runter)", &w.exit.y, 0.001f, -0.5f, 0.5f, "%.4f");
    ch |= ImGui::DragFloat("Exit Z (zurueck)", &w.exit.z, 0.001f, -0.5f, 0.5f, "%.4f");
    ch |= ImGui::SliderFloat("Slide-Dauer s (0=Insert-Speed)", &w.slide_dur, 0.05f, 1.0f);
    ch |= ImGui::SliderFloat("Fall-Distanz m (kontrolliert)", &w.fall_dist, 0.1f, 2.0f);
    ch |= ImGui::SliderFloat("Fall-Dauer s", &w.fall_dur, 0.1f, 1.5f);

    // [FALL-GEWICHT 2026-07-23] GLOBAL fuer alle Mags: MULTIPLIKATOR auf die
    // Fallbeschleunigung (d.gravity, Standard 9.8 = echte Schwerkraft).
    // 1.0 = echte Schwerkraft, hoeher = schneller unten, niedriger = langsamer.
    //
    // [BEREICH GEOEFFNET 2026-09-09] Das Minimum stand auf 1.0 -- langsamer als
    // echte Schwerkraft ging also gar nicht. Genau das wurde aber gebraucht:
    // ueber die kurze Fallstrecke liest das Auge einen schnellen, schnurgeraden
    // Fall als gewichtslos, nicht als schwer. Ab jetzt 0.2 bis 8.
    ch |= ImGui::SliderFloat("Fall-Tempo (global, 1=echte Schwerkraft)",
                             &push.fall_grav_mult, 0.2f, 8.0f, "%.2f");

    // [DROP_MOMENTUM 2026-09-09] Wieviel von der Bewegung beim Loslassen das
    // Mag mitnimmt. 0 = senkrecht fallen (bisheriges Verhalten), 1 = voller
    // Schwung. Gemeint ist ein kleiner Wert -- das Mag soll NICHT der Waffe
    // folgen, nur nicht sofort zurueckbleiben, wenn du laeufst.
    ch |= ImGui::SliderFloat("Drop-Schwung (global, 0=senkrecht)",
                             &push.drop_momentum, 0.0f, 1.0f, "%.2f");
    ImGui::Separator();

    // [LANDE_POSE] Ziel-Weltrotation, in die das Mag beim Fallen hineindreht.
    ch |= ImGui::Checkbox("Lande-Pose (dreht beim Fall in Liege-Pose)", &w.land_on);

    if (w.land_on) {
        ch |= ImGui::DragFloat("  Land Rot X (Grad)", &w.land_rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("  Land Rot Y (Grad)", &w.land_ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("  Land Rot Z (Grad)", &w.land_rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ImGui::Text("Testen: Mag droppen (B) -> es dreht sich beim Fall in diese Pose.");
    }

    ImGui::Separator();

    // [PUSH_POSE] universell (nicht pro Waffe) -> eigener Block, kein wcfg.
    ImGui::Text("-- Hand-Pose beim Reinschieben (universell) --");
    ch |= ImGui::Checkbox("Hand geht kurz in Druecken-Pose", &push.on);

    if (push.on) {
        ch |= ImGui::SliderFloat("  Rein-Lerp s", &push.in_dur, 0.02f, 0.30f);
        ch |= ImGui::SliderFloat("  Halten s", &push.hold, 0.0f, 0.40f);
        // [MINIMUM 0 2026-07-31] Hier wird NICHT auf eine Zielpose animiert,
        // sondern nur das Ueberschreiben ausgeblendet -- darunter liegt die
        // normale, controllergesteuerte Hand.
        ch |= ImGui::SliderFloat("  Zurueck-Lerp s (0 = sofort aus)", &push.out_dur, 0.0f, 1.50f);
        // [DOCK-RELEASE 2026-07-24] Lerpt die Hand nach dem Push weich vom
        // Magazin-Dock zum linken Controller (statt hartem Snap). 0 = aus.
        ch |= ImGui::SliderFloat("  Snap-Ausfaden s (Hand vom Mag zum Controller)",
                                 &push.release_dur, 0.0f, 1.50f);

        if (!m_push_bones_json.empty()) {
            ImGui::TextColored(ImColor{0xFF00FF00},
                               "  Pose: gecapturte \"pose1\" (linke Hand, 16 Joints)");
        } else {
            ImGui::TextColored(ImColor{0xFF5555FF}, "  Pose: generiert (pose1 fehlt in der JSON)");
            ch |= ImGui::SliderFloat("  Fingerkruemmung Grad (0 = flach, ~85 = Faust)",
                                     &push.curl, 0.0f, 120.0f);
            ch |= ImGui::SliderFloat("  Daumen Grad", &push.thumb, -30.0f, 60.0f);
        }

        ImGui::Text("  -- Hand ans Magazin legen (ganze Hand, nicht nur Finger) --");
        ch |= ImGui::DragFloat("  Hand Rot X (Grad)", &push.rx, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("  Hand Rot Y (Grad)", &push.ry, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("  Hand Rot Z (Grad)", &push.rz, 0.5f, -180.0f, 180.0f, "%.1f");
        ch |= ImGui::DragFloat("  Hand Pos X (m)", &push.px, 0.001f, -0.3f, 0.3f, "%.4f");
        ch |= ImGui::DragFloat("  Hand Pos Y (m)", &push.py, 0.001f, -0.3f, 0.3f, "%.4f");
        ch |= ImGui::DragFloat("  Hand Pos Z (m)", &push.pz, 0.001f, -0.3f, 0.3f, "%.4f");
        ImGui::Text("  -- Ada: Zusatz-Offset (kleinere Haende), kommt NUR bei ihr oben drauf --");
        ch |= ImGui::DragFloat("  Ada Pos X (m)", &push.ada_px, 0.001f, -0.1f, 0.1f, "%.4f");
        ch |= ImGui::DragFloat("  Ada Pos Y (m)", &push.ada_py, 0.001f, -0.1f, 0.1f, "%.4f");
        ch |= ImGui::DragFloat("  Ada Pos Z (m)", &push.ada_pz, 0.001f, -0.1f, 0.1f, "%.4f");

        // [PUSH-Y PRO WAFFE 2026-07-25] Nur die Hoehe pro Waffe nachjustierbar;
        // alles andere bleibt universell. 0 = Default (genau der Wert oben).
        ImGui::Text("  -- Nur Hoehe: Zusatz PRO WAFFE (0 = Default oben) --");
        const int32_t pyw = push_wid_now();
        const auto pyit = push_y_by_wid.find(pyw);
        float pyc = pyit != push_y_by_wid.end() ? pyit->second : 0.0f;

        char pylabel[96]{};
        std::snprintf(pylabel, sizeof(pylabel), "  Hand Pos Y Zusatz -- wid %d (m)##pushywid", pyw);

        if (ImGui::DragFloat(pylabel, &pyc, 0.001f, -0.15f, 0.15f, "%.4f")) {
            if (pyc == 0.0f) {
                push_y_by_wid.erase(pyw);
            } else {
                push_y_by_wid[pyw] = pyc;
            }

            ch = true;
        }

        ImGui::TextColored(ImColor{0xFF888888},
                           "  effektives Y fuer wid %d: %.4f  (universell %.4f %+0.4f)",
                           pyw, push.py + pyc, push.py, pyc);

        if (!push_y_by_wid.empty()) {
            std::vector<std::string> lst;

            for (const auto& entry : push_y_by_wid) {
                char buf[48]{};
                std::snprintf(buf, sizeof(buf), "%d:%+0.4f", entry.first, entry.second);
                lst.emplace_back(buf);
            }

            std::sort(lst.begin(), lst.end());
            std::string joined = "  eigene Werte: ";

            for (size_t i = 0; i < lst.size(); ++i) {
                joined += lst[i];

                if (i + 1 < lst.size()) {
                    joined += "  ";
                }
            }

            ImGui::TextColored(ImColor{0xFF00FF00}, "%s", joined.c_str());

            if (ImGui::Button("Zusatz dieser Waffe zuruecksetzen##pushyclr")) {
                push_y_by_wid.erase(pyw);
                ch = true;
            }
        }

        ImGui::Checkbox("TUNING: Pose dauerhaft halten (in VR ausrichten)", &push_tune);

        if (push_tune) {
            ImGui::TextColored(ImColor{0xFF00FF00},
                               "  Tuning laeuft -- Waffe mit steckendem Mag halten und Slider schieben.");
        }
    }

    ImGui::Separator();

    // [EINLEIT-PUNKT 2026-07-23] Kammereingang der aktuell gefuehrten Waffe.
    if (ImGui::TreeNode("Einleit-Punkt (Kammereingang: Joint + Versatz)")) {
        const int32_t dwid = push_wid_now();
        auto& d = dock_or_create(dwid);
        ImGui::Text("Waffe: %d", dwid);

        char jbuf[64]{};
        std::snprintf(jbuf, sizeof(jbuf), "%s", d.joint.c_str());
        bool dch = false;

        if (ImGui::InputText("Joint (z.B. _03)", jbuf, sizeof(jbuf))) {
            d.joint = jbuf;
            dch = true;
        }

        dch |= ImGui::DragFloat("Versatz X (m)  [mittig = 0]", &d.x, 0.001f, -0.20f, 0.20f, "%.4f");
        dch |= ImGui::DragFloat("Versatz Y (m)  [Hoehe]", &d.y, 0.001f, -0.30f, 0.30f, "%.4f");
        dch |= ImGui::DragFloat("Versatz Z (m)  [Tiefe]", &d.z, 0.001f, -0.30f, 0.30f, "%.4f");

        if (dch) {
            save_cfg();
        }

        ImGui::TextColored(ImColor{0xFF888888},
                           "Sentinel Nine (6000) = _03 mit Y -0.092 / Z -0.061 (Referenz).");
        ImGui::TreePop();
    }

    ImGui::Separator();

    // [SHELL-KEYFRAMES 2026-07-24] Keyframe-Bahn fuer den Shell-Einschub.
    if (ImGui::TreeNode("Shell-Insert Keyframes (Bahn statt Slide)")) {
        const int32_t skw = static_cast<int32_t>(re4vr::lua_get_number("__re4_reload_ui_wid", 0.0));

        if (!is_keyframe_insert(skw)) {
            ImGui::TextColored(ImColor{0xFF888888}, "wid %d nutzt die Keyframe-Bahn NICHT.", skw);

            if (uses_rev_insert(skw)) {
                ImGui::TextColored(ImColor{0xFF00AAFF},
                                   "  ...sondern die AUSWURF-Bahn rueckwaerts -- einzustellen im Baum darunter.");
            }
        } else {
            ImGui::TextColored(ImColor{0xFF00FF00}, "Keyframe-Bahn AKTIV fuer wid %d", skw);

            if (skw == 4002 || skw == 40021) {
                ImGui::TextColored(ImColor{0xFF888888},
                                   "  Red9: 4002 = Stripper-Clip, 40021 = Einzelpatrone -- eigene Bahn je Modus.");
                ImGui::Checkbox("Modus: Einzelpatrone (AUS = Stripper-Clip)##r9mode", &r9_single);
                re4vr::lua_set_string("__re4_r9_kf_mode", r9_single ? "single" : "strip");

                if (skw == 40021) {
                    if (ImGui::SliderFloat(
                            "Anlauf Hand -> 1. Keyframe (Anteil der Bahn, 0 = harter Start)##r9anl",
                            &r9_anlauf, 0.0f, 0.8f)) {
                        save_cfg();
                    }
                }
            }

            auto& s = shell_live;

            if (ImGui::SliderFloat("Bahn-Dauer s (1. -> letzter Keyframe)##shkdur",
                                   &shell_dur, 0.05f, 2.0f)) {
                save_cfg();
            }

            ImGui::Checkbox("Joint/Shell einblenden + an Tuning-Lage halten", &shell_preview);

            if (is_shell_clone(skw)) {
                ImGui::TextColored(ImColor{0xFFFFAA00},
                                   "Shell wird als Mesh-Clone gespawnt (nativer Joint bleibt unsichtbar).");
                float part = static_cast<float>(shell_clone_part);
                const bool pp = ImGui::DragFloat(
                    "Clone Mesh-Part (durchdrehen bis nur die Huelse steht)##shclp",
                    &part, 1.0f, 0.0f, 48.0f, "%.0f");

                if (pp) {
                    shell_clone_part = static_cast<int32_t>(std::floor(part + 0.5f));
                }

                const bool ps = ImGui::DragFloat("Clone Scale##shcls", &shell_clone_scale,
                                                 0.01f, 0.1f, 3.0f, "%.3f");

                if (pp || ps) {
                    save_cfg();
                }
            }

            ImGui::Text("-- Tuning-Lage (relativ zur Waffe) --");
            ImGui::DragFloat("X##shk", &s.x, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Y##shk", &s.y, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Z##shk", &s.z, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Rot X##shk", &s.rx, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::DragFloat("Rot Y##shk", &s.ry, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::DragFloat("Rot Z##shk", &s.rz, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::Spacing();

            if (ImGui::Button("   K E Y F R A M E   S P E I C H E R N   (anhaengen)   ##shkadd")) {
                shell_add_key(skw);
            }

            auto& kk = shell_keys(skw);
            ImGui::TextColored(ImColor{0xFFFF00FF},
                               "%d Keyframe(s)  (#1 = Andockpunkt/Start ... letzter = Kammer):",
                               static_cast<int>(kk.size()));

            for (size_t i = 0; i < kk.size(); ++i) {
                const auto k = kk[i];
                ImGui::Text("  #%d  p(%.3f,%.3f,%.3f) r(%.0f,%.0f,%.0f)",
                            static_cast<int>(i + 1), k.x, k.y, k.z, k.rx, k.ry, k.rz);
                ImGui::SameLine();

                char lb[32]{};
                std::snprintf(lb, sizeof(lb), "Laden##shkl%d", static_cast<int>(i + 1));

                if (ImGui::Button(lb)) {
                    s = k;
                }

                ImGui::SameLine();
                std::snprintf(lb, sizeof(lb), "X##shkx%d", static_cast<int>(i + 1));

                if (ImGui::Button(lb)) {
                    kk.erase(kk.begin() + static_cast<ptrdiff_t>(i));
                    save_cfg();
                    break;
                }
            }

            if (!kk.empty() && ImGui::Button("Alle Keyframes loeschen##shkclr")) {
                kk.clear();
                save_cfg();
            }

            ImGui::Spacing();
            ImGui::Text("-- Vorschau: Bahn abfahren (setzt die Tuning-Lage) --");

            if (ImGui::SliderFloat("Bahn-Fortschritt 0..1##shkprev", &shell_prev_t, 0.0f, 1.0f)
                && !kk.empty()) {
                Key p{};

                if (shell_pose_at(skw, shell_prev_t, p)) {
                    s = p;
                }
            }
        }

        ImGui::TreePop();
    }

    ImGui::Separator();

    // [MAG-EJECT-KEYFRAMES 2026-07-24] Keyframe-Bahn fuer den Mag-AUSWURF.
    if (ImGui::TreeNode("Mag-Eject Keyframes (Auswurf-Bahn statt Slide)")) {
        const int32_t ekw = static_cast<int32_t>(re4vr::lua_get_number("__re4_reload_ui_wid", 0.0));

        if (!is_keyframe_eject(ekw)) {
            ImGui::TextColored(ImColor{0xFF888888},
                               "wid %d nutzt die Auswurf-Bahn NICHT (nicht in KEYFRAME_EJECT).", ekw);
        } else {
            if (ekw == 42001) {
                ImGui::TextColored(ImColor{0xFFFFAA00},
                                   "  MERCENARIES-TMP (Krauser) -- eigene Bahn, Leons TMP 4200 bleibt unberuehrt.");
            }

            auto& ek = eject_keys(ekw);

            if (ek.size() >= 2) {
                ImGui::TextColored(ImColor{0xFF00FF00}, "Auswurf-Bahn AKTIV fuer wid %d", ekw);
            } else {
                ImGui::TextColored(ImColor{0xFF00AAFF},
                                   "wid %d: noch <2 Keyframes -> es laeuft weiter der alte Slide.", ekw);
            }

            if (ImGui::SliderFloat("Bahn-Dauer s (1. -> letzter Keyframe)##ekdur",
                                   &eject_dur, 0.05f, 2.0f)) {
                save_cfg();
            }

            // [INSERT = EJECT RUECKWAERTS 2026-07-25] Einschub = dieselbe Bahn
            // rueckwaerts.
            ImGui::Spacing();
            ImGui::Text("-- Einschub aus dieser Bahn (rueckwaerts) statt bisheriger Methode --");

            char rl[64]{};
            std::snprintf(rl, sizeof(rl), "Mag-IN = Auswurf rueckwaerts##revins%d", ekw);
            bool rv = m_rev_insert[ekw];

            if (ImGui::Checkbox(rl, &rv)) {
                m_rev_insert[ekw] = rv;
                save_cfg();
            }

            if (m_rev_insert[ekw]) {
                if (ek.size() >= 2) {
                    ImGui::TextColored(ImColor{0xFF00FF00},
                                       "  Einschub laeuft rueckwaerts ueber dieselben Keyframes (letzter -> #1).");
                } else {
                    ImGui::TextColored(ImColor{0xFF00AAFF},
                                       "  Noch <2 Keyframes -> Einschub bleibt bei der alten Methode.");
                }

                if (ImGui::SliderFloat("Einschub-Dauer s (eigener Wert)##revinsdur",
                                       &rev_insert_dur, 0.05f, 2.0f)) {
                    save_cfg();
                }

                ImGui::TextColored(ImColor{0xFF888888},
                                   "  Achtung: Start ist wie bei jeder Keyframe-Bahn ein fester Punkt an der");
                ImGui::TextColored(ImColor{0xFF888888},
                                   "  Waffe -- die Handposition wird beim Einschub ignoriert.");
            }

            ImGui::Checkbox("Mag einblenden + an Tuning-Lage halten (PREVIEW)", &eject_preview);

            if (eject_preview) {
                ImGui::TextColored(ImColor{0xFF00FF00},
                                   "Preview laeuft -- Waffe in VR halten und Slider schieben, das Mag folgt.");
            }

            ImGui::Spacing();

            if (ImGui::Button("Ruhelage uebernehmen (Mag in der Kammer -> Keyframe #1)##ekrest")) {
                if (!eject_grab_rest()) {
                    ImGui::TextColored(ImColor{0xFF5555FF}, "  Keine Waffe/Joint gefunden.");
                }
            }

            ImGui::Text("-- Tuning-Lage (relativ zur Waffe) --");
            auto& e = eject_live;
            ImGui::DragFloat("X##ekk", &e.x, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Y##ekk", &e.y, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Z##ekk", &e.z, 0.001f, -0.5f, 0.5f, "%.4f");
            ImGui::DragFloat("Rot X##ekk", &e.rx, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::DragFloat("Rot Y##ekk", &e.ry, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::DragFloat("Rot Z##ekk", &e.rz, 0.5f, -180.0f, 180.0f, "%.1f");
            ImGui::Spacing();

            if (ImGui::Button("   K E Y F R A M E   S P E I C H E R N   (anhaengen)   ##ekadd")) {
                eject_add_key(ekw);
            }

            ImGui::TextColored(ImColor{0xFFFF00FF},
                               "%d Keyframe(s)  (#1 = Mag in der Kammer ... letzter = frei, dann faellt es):",
                               static_cast<int>(ek.size()));

            for (size_t i = 0; i < ek.size(); ++i) {
                const auto k = ek[i];
                ImGui::Text("  #%d  p(%.3f,%.3f,%.3f) r(%.0f,%.0f,%.0f)",
                            static_cast<int>(i + 1), k.x, k.y, k.z, k.rx, k.ry, k.rz);
                ImGui::SameLine();

                char lb[32]{};
                std::snprintf(lb, sizeof(lb), "Laden##ekl%d", static_cast<int>(i + 1));

                if (ImGui::Button(lb)) {
                    e = k;
                }

                ImGui::SameLine();
                std::snprintf(lb, sizeof(lb), "X##ekx%d", static_cast<int>(i + 1));

                if (ImGui::Button(lb)) {
                    ek.erase(ek.begin() + static_cast<ptrdiff_t>(i));
                    save_cfg();
                    break;
                }
            }

            if (!ek.empty() && ImGui::Button("Alle Keyframes loeschen##ekclr")) {
                ek.clear();
                save_cfg();
            }

            ImGui::Spacing();
            ImGui::Text("-- Vorschau: Bahn abfahren (setzt die Tuning-Lage) --");

            if (ImGui::SliderFloat("Bahn-Fortschritt 0..1##ekprev", &eject_prev_t, 0.0f, 1.0f)
                && !ek.empty()) {
                Key p{};

                if (eject_pose_at(ekw, eject_prev_t, p)) {
                    e = p;
                }
            }
        }

        ImGui::TreePop();
    }

    ImGui::Separator();

    if (ImGui::Button("Preview Slide-Pose 5s")) {
        start_preview(5.0f);
    }

    ImGui::SameLine();

    if (ImGui::Button("Speichern")) {
        save_cfg();
    }

    if (ch) {
        save_cfg();
    }

    ImGui::TreePop();
}

// ============================================================================
// [KFH 2026-09-24] Dev-Baum "RE4VR - Keyframes"
// ============================================================================
void RE4VRReloadAdv::draw_kf_ui() {
    if (!ImGui::TreeNode("RE4VR - Keyframes")) {
        return;
    }

    // Red9: derselbe Modus-Schalter wie in "Shell-Insert Keyframes" (Reload Adv)
    if (get_equip_wid().value_or(0) == 4002 || get_equip_wid().value_or(0) == 6113) {
        if (ImGui::Checkbox("Modus: Einzelpatrone (AUS = Stripper-Clip)##kfhr9", &r9_single)) {
            re4vr::lua_set_string("__re4_r9_kf_mode", r9_single ? "single" : "strip");
        }
    }

    const int32_t wid = kfh_ui_id();

    ImGui::Text("Waffe: %d", wid);

    if (!kfh_in_scope(wid)) {
        ImGui::Text("Diese Waffe ist nicht dabei");
        m_kfh_force = 0;
        ImGui::TreePop();
        return;
    }

    if (!has_shell_keys(kfh_path_id(wid))) {   // [KFH ADA]
        ImGui::Text("Keine Keyframe-Bahn fuer diese Waffe");
    }

    auto* m = RE4VRReloadMain::instance();
    KfHand c = (kfh_cfg(wid) != nullptr) ? *kfh_cfg(wid) : KfHand{};
    bool ch = false;
    bool save = false;

    // Copy ALL: beide Posen samt Einstellungen einer anderen Waffe (keine Bahn)
    {
        static int32_t src = -1;
        const std::string cur = (src >= 0) ? std::to_string(src) : "-";

        if (ImGui::BeginCombo("Copy ALL from##kfhca", cur.c_str())) {
            for (const auto& e : m_kfh) {
                if (e.first != wid && ImGui::Selectable(std::to_string(e.first).c_str(), e.first == src)) {
                    src = e.first;
                }
            }

            ImGui::EndCombo();
        }

        ImGui::SameLine();

        if (ImGui::Button("Copy ALL##kfhcab") && src >= 0 && src != wid && kfh_cfg(src) != nullptr) {
            c = *kfh_cfg(src);
            ch = true;
            save = true;

            for (const char* n : {"(kf1)", "(kfend)"}) {
                re4vr::wpose::Bones b{};

                if (re4vr::wpose::own(src, n, &b)) {
                    re4vr::wpose::set_own(wid, n, b);
                } else {
                    re4vr::wpose::drop_own(wid, n);
                }
            }
        }
    }

    const auto drag = [&](const char* label, float* v, float speed, float lo, float hi, const char* fmt) {
        if (ImGui::DragFloat(label, v, speed, lo, hi, fmt)) {
            ch = true;
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save = true;
        }
    };

    if (ImGui::TreeNode("Keyframe 1 (Andocken)##kfh1")) {
        if (ImGui::Checkbox("An##kfh1on", &c.on)) {
            ch = true;
            save = true;
        }

        bool f1 = (m_kfh_force == 1);

        if (ImGui::Checkbox("Force Keyframe 1##kfh1f", &f1)) {
            m_kfh_force = f1 ? 1 : 0;
        }

        drag("Einblenden s##kfh1in", &c.in_dur, 0.005f, 0.0f, 1.0f, "%.3f");
        static bool s_g1 = false;   // Grob: 10x groessere Schritte (nicht gespeichert)
        ImGui::Text("Hand relativ zur Shell");
        ImGui::SameLine();
        ImGui::Checkbox("Grob##kfh1g", &s_g1);
        const float gm1 = s_g1 ? 10.0f : 1.0f;
        drag("X##kfh1", &c.px, 0.001f * gm1, -0.5f, 0.5f, "%.4f");
        drag("Y##kfh1", &c.py, 0.001f * gm1, -0.5f, 0.5f, "%.4f");
        drag("Z##kfh1", &c.pz, 0.001f * gm1, -0.5f, 0.5f, "%.4f");
        drag("Rot X##kfh1", &c.rx, 0.5f * gm1, -180.0f, 180.0f, "%.1f");
        drag("Rot Y##kfh1", &c.ry, 0.5f * gm1, -180.0f, 180.0f, "%.1f");
        drag("Rot Z##kfh1", &c.rz, 0.5f * gm1, -180.0f, 180.0f, "%.1f");

        if (m != nullptr) {
            m->kfh_finger_ui(wid, "(kf1)", "Kopieren von Mag-in-hand-Pose", false);
        }

        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Letzter Keyframe (End)##kfhe")) {
        if (ImGui::Checkbox("An##kfheon", &c.end_on)) {
            ch = true;
            save = true;
        }

        bool f2 = (m_kfh_force == 2);

        if (ImGui::Checkbox("Force End##kfhef", &f2)) {
            m_kfh_force = f2 ? 2 : 0;
        }

        static bool s_ge = false;   // Grob: 10x groessere Schritte (nicht gespeichert)
        ImGui::Text("Hand relativ zur Waffe");
        ImGui::SameLine();
        ImGui::Checkbox("Grob##kfheg", &s_ge);
        const float gme = s_ge ? 10.0f : 1.0f;
        drag("X##kfhe", &c.epx, 0.001f * gme, -0.5f, 0.5f, "%.4f");
        drag("Y##kfhe", &c.epy, 0.001f * gme, -0.5f, 0.5f, "%.4f");
        drag("Z##kfhe", &c.epz, 0.001f * gme, -0.5f, 0.5f, "%.4f");
        drag("Rot X##kfhe", &c.erx, 0.5f * gme, -180.0f, 180.0f, "%.1f");
        drag("Rot Y##kfhe", &c.ery, 0.5f * gme, -180.0f, 180.0f, "%.1f");
        drag("Rot Z##kfhe", &c.erz, 0.5f * gme, -180.0f, 180.0f, "%.1f");
        drag("Ueberblenden ab (Bahn 0-1)##kfhff", &c.fade_from, 0.005f, 0.0f, 1.0f, "%.3f");
        drag("Ueberblenden bis (Bahn 0-1)##kfhft", &c.fade_to, 0.005f, 0.0f, 1.0f, "%.3f");
        drag("Halten s##kfheh", &c.end_hold, 0.005f, 0.0f, 2.0f, "%.3f");
        drag("Ausblenden s##kfheo", &c.out_dur, 0.005f, 0.0f, 2.0f, "%.3f");

        if (m != nullptr) {
            m->kfh_finger_ui(wid, "(kfend)", "Kopieren von Keyframe-1-Pose", true);
        }

        ImGui::TreePop();
    }

    if (ch) {
        m_kfh[wid] = c;
    }

    if (save) {
        save_cfg();

        // Finger-Kopien aus Copy ALL liegen im [WPOSE]-Speicher der Reload-JSON.
        if (m != nullptr) {
            m->kfh_save();
        }
    }

    ImGui::TreePop();
}

#endif
