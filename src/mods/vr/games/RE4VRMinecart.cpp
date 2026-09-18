// ============================================================================
// RE4VRMinecart -- 1:1-Portierung von re4_vr_minecart.lua. Siehe RE4VRMinecart.hpp
// fuer die Bausteine und die zwingende Reihenfolge im Mod-Vektor.
//
// Spezifikation: I:\LUATRANS\PORT_MINECART_SPEC.md -- **es gilt der NACHTRAG**
// ab Zeile 1279, der den Rumpf davor an mehreren Stellen korrigiert.
// ============================================================================
#if defined(RE4)

#include <cmath>
#include <cstdio>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VRHolster.hpp"
#include "RE4VRMinecart.hpp"

// windows.h (ueber die Includes oben) definiert min/max als MAKROS und zerlegt
// damit jedes std::min/std::max. Der Fork setzt kein NOMINMAX -- gleiche
// Gegenmassnahme wie in RE4VRArmChain/RE4VRHolster.
#undef min
#undef max

namespace {
// Lua Z.307/416/492/508 -- os.clock(). Der EINZIGE Zeitgeber der Datei; der
// Reload-Fade (Z.155/158) ist bewusst frame- und nicht zeitgetaktet.
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// Geschuetzte Managed-Zugriffe -- Bauform wie in RE4VRArmChain/RE4VRHolster:
// 16-Byte-Puffer (die Engine behandelt via.vec3/via.quat als 16 Byte) und ein
// Erfolgs-Flag, weil re4vr::call_safe den Unterschied zwischen "hat 0
// geliefert" und "ging nicht" verschluckt.
sdk::REMethodDefinition* find_method(::REManagedObject* obj, std::string_view name) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    return td != nullptr ? td->get_method(name) : nullptr;
}

bool clear_pending(sdk::VMContext* context, bool ok) {
    // Eine haengende Exception wuerde sonst beim naechsten Engine-Aufruf im
    // fremden Kontext hochkommen. Bauform 1:1 aus RE4VRMovement.
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
// dort nicht gesetzt und kommt beim Lesen als 1.0 zurueck.
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

// [C(b) DES NACHTRAGS] get_Tilt braucht ein Erfolgs-Flag, KEINEN float-Sentinel:
// Lua prueft `if not tilt`, und 0.0 ist in Lua WAHR. Ein 0.0 == "ungueltig"
// wuerde bei echtem Tilt 0 faelschlich abbrechen.
bool get_float(::REManagedObject* obj, std::string_view name, float& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    float buf = 0.0f;
    bool ok = false;

    try {
        buf = method->call_safe<float>(context, obj);
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

// get_MotionName liefert ein managed System.String.
std::string motion_name_of(::REManagedObject* node) {
    if (node == nullptr) {
        return {};
    }

    auto* n = re4vr::call_safe<::REManagedObject*>(node, "get_MotionName");

    if (n == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
    } catch (...) {
        return {};
    }
}

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    return s;
}

// Lua Z.268-273. Yaw aus der Vorwaertsrichtung eines Quaternions.
// [D(d) DES NACHTRAGS] Die Aufrufer speisen `Matrix4x4f:to_quat()` ein, also
// glm::quat(m) -- die lookAtLH-Falle von Luas Vector3f:to_quat greift hier
// NICHT. Wer sie versehentlich anwendet, killt den Kipp-Ausgleich.
std::optional<float> yaw_of_quat(const glm::quat& q) {
    const glm::vec3 f = q * glm::vec3{0.0f, 0.0f, 1.0f};
    const float l = std::sqrt(f.x * f.x + f.z * f.z);

    if (l < 1e-4f) {
        return std::nullopt;
    }

    // Luas zweiargumentiges math.atan == atan2.
    return std::atan2(f.x / l, f.z / l);
}

glm::quat yaw_to_quat(float y) {
    const float h = y * 0.5f;
    return glm::quat{std::cos(h), 0.0f, std::sin(h), 0.0f};
}

float yaw_wrap(float a) {
    constexpr float PI = 3.14159265358979323846f;

    while (a > PI) {
        a -= 2.0f * PI;
    }

    while (a < -PI) {
        a += 2.0f * PI;
    }

    return a;
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_minecart.json";
constexpr const char* MOVE_CFG_PATH = "re4_vr/re4_vr_movement.json";

// Lua Z.170. root wird BEWUSST nicht gepinnt: auf dem Cart treibt der Wagen den
// root; gepinnt wird nur die Torso-Kette relativ zum jeweiligen Parent.
const std::array<const char*, 7> PIN_JOINTS{
    "Hip", "Spine_0", "Spine_1", "Spine_2", "Neck_0", "Neck_1", "Head",
};
} // namespace

std::shared_ptr<RE4VRMinecart>& RE4VRMinecart::get() {
    static auto inst = std::make_shared<RE4VRMinecart>();
    return inst;
}

// ============================================================================
// Handles
// ============================================================================

void RE4VRMinecart::store(Handle& h, ::REManagedObject* o) {
    if (h.obj == o) {
        return;
    }

    drop(h);

    if (o == nullptr) {
        return;
    }

    h.obj = o;
    h.reffed = false;

    // In Lua macht sol_lua_push das add_ref unsichtbar mit. Nur zaehlbare
    // Objekte anfassen -- dieselbe Heuristik wie in den anderen Modulen.
    if (utility::re_managed_object::is_managed_object(o)
        && static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        h.reffed = true;
    }
}

void RE4VRMinecart::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

// ============================================================================
// Config
// ============================================================================

void RE4VRMinecart::load_cfg() {
    const auto d = re4vr::json_load(CFG_PATH);

    if (d.is_object()) {
        // [1:1] Der Ladeblock (Lua Z.74-90) uebernimmt NUR diese elf Schluessel.
        // yaw_follow bleibt bewusst aussen vor (deprecated, verursachte den
        // Stick-Yaw-Anschlag), die zwoelf rumble_* schlicht, weil das Original
        // sie nie nachgezogen hat. save_cfg schreibt trotzdem alle 24.
        m_cfg.body_down = re4vr::j_num(d, "body_down", m_cfg.body_down);
        m_cfg.body_back = re4vr::j_num(d, "body_back", m_cfg.body_back);
        m_cfg.yaw_sign = re4vr::j_num(d, "yaw_sign", m_cfg.yaw_sign);
        m_cfg.lean_enabled = re4vr::j_bool(d, "lean_enabled", m_cfg.lean_enabled);
        m_cfg.lean_threshold = re4vr::j_num(d, "lean_threshold", m_cfg.lean_threshold);
        m_cfg.lean_full = re4vr::j_num(d, "lean_full", m_cfg.lean_full);
        m_cfg.lean_sign = re4vr::j_num(d, "lean_sign", m_cfg.lean_sign);
        m_cfg.lean_tilt_min = re4vr::j_num(d, "lean_tilt_min", m_cfg.lean_tilt_min);
        m_cfg.recenter_enabled = re4vr::j_bool(d, "recenter_enabled", m_cfg.recenter_enabled);
        m_cfg.recenter_dead = re4vr::j_num(d, "recenter_dead", m_cfg.recenter_dead);
        m_cfg.recenter_rate = re4vr::j_num(d, "recenter_rate", m_cfg.recenter_rate);
    }

    m_cfg_loaded = true;
}

void RE4VRMinecart::save_cfg() {
    // [DATENVERLUST-RIEGEL] Vor dem Laden NIE schreiben -- sonst ueberbuegelt
    // ein Reglerklick die JSON mit Compile-Defaults. Dieselbe Falle wurde beim
    // movement-Port gefunden.
    if (!m_cfg_loaded) {
        return;
    }

    nlohmann::json j;
    j["body_down"] = m_cfg.body_down;
    j["body_back"] = m_cfg.body_back;
    j["yaw_follow"] = m_cfg.yaw_follow;
    j["yaw_sign"] = m_cfg.yaw_sign;
    j["lean_enabled"] = m_cfg.lean_enabled;
    j["lean_threshold"] = m_cfg.lean_threshold;
    j["lean_full"] = m_cfg.lean_full;
    j["lean_sign"] = m_cfg.lean_sign;
    j["lean_tilt_min"] = m_cfg.lean_tilt_min;
    j["recenter_enabled"] = m_cfg.recenter_enabled;
    j["recenter_dead"] = m_cfg.recenter_dead;
    j["recenter_rate"] = m_cfg.recenter_rate;
    j["rumble_enabled"] = m_cfg.rumble_enabled;
    j["rumble_amp"] = m_cfg.rumble_amp;
    j["rumble_rate"] = m_cfg.rumble_rate;
    j["rumble_dur"] = m_cfg.rumble_dur;
    j["rumble_freq"] = m_cfg.rumble_freq;
    j["rumble_clack"] = m_cfg.rumble_clack;
    j["rumble_clack_every"] = m_cfg.rumble_clack_every;
    j["rumble_clack_amp"] = m_cfg.rumble_clack_amp;
    j["rumble_intro"] = m_cfg.rumble_intro;
    j["rumble_speed_min"] = m_cfg.rumble_speed_min;
    j["rumble_speed_full"] = m_cfg.rumble_speed_full;
    j["rumble_speed_scale"] = m_cfg.rumble_speed_scale;

    re4vr::json_save(CFG_PATH, j);
}

std::optional<std::string> RE4VRMinecart::on_initialize() {
    load_cfg();
    return Mod::on_initialize();
}

void RE4VRMinecart::on_lua_state_destroyed(sol::state& lua) {
    // "Reset Scripts" nimmt die Lua-Seite mit -- dieses Modul ueberlebt es.
    //
    // ACHTUNG, hier steckt mehr als re.on_script_reset (Z.575-578): jener
    // Handler raeumt nur pin.tf/joints/names, bw_motion und bw_go. Beim Reset
    // wird die Lua-Datei aber KOMPLETT NEU AUSGEFUEHRT -- damit sind ALLE
    // File-Locals wieder auf ihrem Anfangswert und die Config frisch aus der
    // JSON gelesen. Ein Member, der das hier nicht nachbildet, ueberlebt den
    // Reset faelschlich: eine neu gespeicherte pin_pose oder eine von Hand
    // editierte JSON wuerden sonst erst nach einem Spielneustart greifen.
    drop(m_pin_tf);

    for (auto& h : m_pin_joints) {
        drop(h);
    }

    m_pin_joints.clear();
    m_pin_names.clear();

    drop(m_bw_motion);
    drop(m_bw_go);

    // --- was das Neu-Ausfuehren der Datei sonst noch zuruecksetzt ---
    drop(m_wep_go);
    drop(m_wep_motion);
    drop(m_rail_manager);

    m_pin_map.clear();
    m_pin_map_loaded = false;
    m_pin_map_valid = false;

    m_t_motion = nullptr;

    m_yf_entry_body.reset();
    m_yf_entry_hmd.reset();

    m_ck_last = 0.0;   // Lua: `local _ck_last = 0` -> erster Aufruf feuert sofort

    m_rc_last_t.reset();
    m_rc_dbg = RcDbg{};

    m_rum_t0.reset();
    m_rum_next_t = 0.0;
    m_rum_next_clack = 0.0;
    m_rum_side = 0;
    m_rum_dbg = RumDbg{};

    m_spd_t.reset();
    m_spd_p = glm::vec3{};
    m_spd_v = 0.0f;
    m_spd_src = "-";

    m_lean_dbg = LeanDbg{};

    // Lua liest den Config-Block (Z.74-90) beim Neu-Ausfuehren erneut.
    load_cfg();
}

bool RE4VRMinecart::railcar_active() const {
    // [A4 DES NACHTRAGS] Luas `rawget(_G,"__re4_railcar_mode") == true` ist
    // strikt: ein Nicht-Boolean zaehlt NICHT als true. Genau das leistet
    // lua_get_tribool(...) == 1, nicht lua_get_bool(..., false).
    return re4vr::lua_get_tribool("__re4_railcar_mode") == 1;
}

// ============================================================================
// (1) Layer0-Anim-Name exportieren (Lua Z.113-126)
// ============================================================================

void RE4VRMinecart::ensure_t_motion() {
    // Lua loest sdk.typeof("via.motion.Motion") zur LADEZEIT auf (Z.112/136).
    // Wird das erst im ersten on_frame nachgeholt, kann eine fruehere
    // LockScene-Phase ohne Typ dastehen: __vr_anim_l0 waere dann faelschlich
    // nil und ein laufendes Reload unerkannt.
    if (m_t_motion == nullptr) {
        m_t_motion = re4vr::runtime_type("via.motion.Motion");
    }
}

void RE4VRMinecart::update_anim_export() {
    ensure_t_motion();

    auto* tf = re4vr::body_transform();

    if (tf == nullptr) {
        re4vr::lua_set_nil("__vr_anim_l0");
        drop(m_bw_motion);
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

    if (go != m_bw_go.obj) {
        store(m_bw_go, go);
        drop(m_bw_motion);
    }

    if (m_bw_motion.obj == nullptr && go != nullptr && m_t_motion != nullptr) {
        // getComponent(System.Type) mit dem gecachten via.motion.Motion-Typ.
        store(m_bw_motion,
              re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", m_t_motion));
    }

    if (m_bw_motion.obj == nullptr) {
        re4vr::lua_set_nil("__vr_anim_l0");
        return;
    }

    auto* layer = re4vr::call_safe<::REManagedObject*>(m_bw_motion.obj, "getLayer", 0);
    auto* node = layer != nullptr
                     ? re4vr::call_safe<::REManagedObject*>(layer, "get_HighestWeightMotionNode")
                     : nullptr;
    const std::string name = motion_name_of(node);

    // Lua Z.125 prueft STRENG: type(name)=="string" and name ~= "".
    if (name.empty()) {
        re4vr::lua_set_nil("__vr_anim_l0");
    } else {
        re4vr::lua_set_string("__vr_anim_l0", name);
    }
}

// ============================================================================
// [RAILCAR_RELOAD] Lua Z.138-166
//
// Die native Reload-Anim laeuft an der WAFFE (via.motion.Motion), nicht am
// Body -- die Body-Layer bleiben in der Mounted-Halte-Pose. Gemessen: Reload =
// "wp4002_tram_0810_Reload"; Idle/Aim/Fire heissen general_0500_Idle_Loop /
// _0510_Aim_Loop / _0512_Aim_Fire -> case-insensitives "reload" ist ein
// sauberer Diskriminator.
// ============================================================================

::REManagedObject* RE4VRMinecart::weapon_game_object() {
    // [A6 DES NACHTRAGS -- BLOCKER, hier aufgeloest]
    // Das Original liest `re4.weapon_gameobject` aus der Lua-Modultabelle
    // utility/RE4. Global ist dort nur `_RE4Lib`; das Feld selbst ist von C++
    // nicht erreichbar (es gibt keinen lua_get_table_pointer). Deshalb dieselbe
    // Kette nativ, wie utility/RE4.lua sie baut:
    //   HeadGameObject -> chainsaw.PlayerEquipment -> getEquipWeapon -> GameObject
    //
    // ABWEICHUNG, bewusst und dokumentiert: utility/RE4.lua fuellt sein Feld im
    // UpdateBehavior-Entry, wir werten die Kette in LockScene aus. Der Wert ist
    // damit einen Tick FRISCHER, nicht aelter. Fuer die reine Reload-Erkennung
    // ist das unkritisch, bitgleich ist es nicht.
    auto* head = re4vr::head_game_object();

    if (head == nullptr) {
        return nullptr;
    }

    auto* pe = re4vr::get_component(head, "chainsaw.PlayerEquipment");

    if (pe == nullptr) {
        return nullptr;
    }

    auto* weapon = re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeapon");

    if (weapon == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(weapon, "get_GameObject");
}

void RE4VRMinecart::update_reload_flag() {
    ensure_t_motion();

    bool reloading = false;

    // Lua benutzt hier ein `repeat ... until true` mit break als Sprungziel.
    do {
        auto* wgo = weapon_game_object();

        if (wgo == nullptr) {
            break;
        }

        if (m_wep_go.obj != wgo) {
            store(m_wep_go, wgo);
            drop(m_wep_motion);
        }

        if (m_wep_motion.obj == nullptr && m_t_motion != nullptr) {
            store(m_wep_motion,
                  re4vr::call_safe<::REManagedObject*>(wgo, "getComponent(System.Type)", m_t_motion));
        }

        if (m_wep_motion.obj == nullptr) {
            break;
        }

        auto* lay = re4vr::call_safe<::REManagedObject*>(m_wep_motion.obj, "getLayer", 0);
        auto* node = lay != nullptr
                         ? re4vr::call_safe<::REManagedObject*>(lay, "get_HighestWeightMotionNode")
                         : nullptr;
        // Lua Z.148 ist hier bewusst LOCKERER als Z.125: tostring(nm):lower()
        // akzeptiert jeden Wert. Ein leerer Name findet "reload" nicht.
        const std::string nm = to_lower(motion_name_of(node));

        if (!nm.empty() && nm.find("reload") != std::string::npos) {
            reloading = true;
        }
    } while (false);

    re4vr::lua_set_bool("__re4_railcar_reloading", reloading);

    // [RELOAD-FADE] Pin-Einfluss: 1 = voll gepinnt, 0 = voll nativ/Reload.
    // Reload-Start rampt REIN nach 0 (Kopf gleitet in die Reload-Pose),
    // Reload-Ende rampt RAUS nach 1. Gleicher Schritt, bewusst frame-getaktet.
    const double f = re4vr::lua_get_number("__re4_railcar_reload_fade", 1.0);

    if (reloading) {
        if (f > 0.0) {
            re4vr::lua_set_number("__re4_railcar_reload_fade", std::max(0.0, f - 0.09));
        }

        // Hand-Ease-out-Capture fuers naechste Reload-Ende ruecksetzen.
        // Ausgewertet wird das ausschliesslich in re4_vr_motion.lua.
        re4vr::lua_set_nil("__re4_reload_hand_fp");
        re4vr::lua_set_nil("__re4_reload_hand_fr");
    } else {
        if (f < 1.0) {
            re4vr::lua_set_number("__re4_railcar_reload_fade", std::min(1.0, f + 0.09));
        }
    }
}

// ============================================================================
// (3) Spine-/Torso-Pin (Lua Z.172-248)
// ============================================================================

bool RE4VRMinecart::load_pin_pose() {
    // Lua Z.173: einmal geladen, danach nie wieder angefasst -- auch nicht beim
    // Script-Reset. 1:1 uebernommen, inklusive der Folge, dass eine im Spiel
    // neu gespeicherte pin_pose erst nach einem Neustart greift.
    if (m_pin_map_loaded) {
        return m_pin_map_valid;
    }

    // [NUR DEN ERFOLG CACHEN] Lua setzt `pin_map` erst ganz am Ende, wenn die
    // Tabelle wirklich gefuellt ist -- ein Fehlschlag laesst pin_map nil und
    // die naechste Frame liest die JSON ERNEUT. Wer hier schon oben ein
    // "geladen"-Flag setzt, schaltet den Spine-Pin nach einem einzigen
    // Fehlversuch fuer die ganze Sitzung ab (z.B. wenn movement die pin_pose
    // beim ersten Kart-Frame noch nicht geschrieben hat) -> Arm am Anschlag
    // und Hand-Flackern, genau der Zustand, gegen den das Script existiert.
    const auto d = re4vr::json_load(MOVE_CFG_PATH);

    if (!d.is_object() || !d.contains("pin_pose")) {
        return false;
    }

    const auto& pp = d["pin_pose"];

    if (!pp.is_object() || !pp.contains("joints") || !pp["joints"].is_object()) {
        return false;
    }

    for (const auto& [name, s] : pp["joints"].items()) {
        if (!s.is_object() || !s.contains("px") || !s["px"].is_number()
            || !s.contains("rw") || !s["rw"].is_number()) {
            continue;
        }

        PinRel rel{};
        rel.p = glm::vec3{re4vr::j_num(s, "px", 0.0f), re4vr::j_num(s, "py", 0.0f),
                          re4vr::j_num(s, "pz", 0.0f)};
        rel.r = glm::quat{re4vr::j_num(s, "rw", 1.0f), re4vr::j_num(s, "rx", 0.0f),
                          re4vr::j_num(s, "ry", 0.0f), re4vr::j_num(s, "rz", 0.0f)};
        m_pin_map[name] = rel;
    }

    if (m_pin_map.empty()) {
        return false; // Lua: `if next(m) == nil then return nil end` -- erneut versuchen
    }

    m_pin_map_loaded = true;
    m_pin_map_valid = true;
    return true;
}

bool RE4VRMinecart::resolve_spine(::REManagedObject* tf) {
    // Cache; bei Save/Load (tote Handles) neu aufloesen. Der Vergleich
    // `pin.tf == tf` ist in Lua ein roher Zeigervergleich (sol equal_to).
    if (m_pin_tf.obj == tf && !m_pin_joints.empty()) {
        auto* probe = m_pin_joints[0].obj;
        glm::vec3 dummy{};

        if (probe != nullptr && get_vec3(probe, "get_Position", dummy)) {
            return true;
        }
    }

    for (auto& h : m_pin_joints) {
        drop(h);
    }

    m_pin_joints.clear();
    m_pin_names.clear();
    store(m_pin_tf, tf);

    for (const auto* name : PIN_JOINTS) {
        auto* j = joint_by_name(tf, name);

        if (j != nullptr) {
            Handle h{};
            store(h, j);
            m_pin_joints.push_back(h);
            m_pin_names.emplace_back(name);
        }
    }

    if (m_pin_joints.empty()) {
        return false;
    }

    return true;
}

void RE4VRMinecart::apply_spine_pin() {
    // [KEIN eigener SCRIPTGATE-Riegel] Beide Aufrufer sind bereits gegatet:
    // RE4VRArmChain::railcar_pin_spine() und unsere eigenen Phasen. Wer den Pin
    // je aus einer ungegateten Stelle ruft, umgeht die Stillzone (60850/56100).
    if (!railcar_active()) {
        return;
    }

    // [RELOAD-FADE] 1 = voller Pin, 0 = voll nativ. Beachte den Default 1.0 --
    // an den beiden Phasen-Aufrufstellen prueft das Original mit Default 0.0.
    const float f = static_cast<float>(re4vr::lua_get_number("__re4_railcar_reload_fade", 1.0));

    if (f <= 0.0f) {
        return; // mitten im Reload -> Pin ganz aus, native Anim durchlassen
    }

    if (!load_pin_pose()) {
        return;
    }

    auto* tf = re4vr::body_transform();

    if (tf == nullptr) {
        return;
    }

    if (!resolve_spine(tf)) {
        return;
    }

    const float down = m_cfg.body_down;
    const float back = m_cfg.body_back;

    for (size_t i = 0; i < m_pin_joints.size(); ++i) {
        const auto it = m_pin_map.find(m_pin_names[i]);

        if (it == m_pin_map.end()) {
            continue;
        }

        const PinRel& rel = it->second;
        glm::vec3 p = rel.p;

        // [SITZEN] NUR Hip absenken -> die ganze Oberkoerper-Kette (Kinder von
        // Hip) faellt mit. Lokal-Y gegen den root; auf dem ebenen Cart
        // entspricht das ~Welt-unten.
        if (m_pin_names[i] == "Hip" && (down != 0.0f || back != 0.0f)) {
            p = glm::vec3{p.x, p.y - down, p.z - back};
        }

        auto* j = m_pin_joints[i].obj;

        if (j == nullptr) {
            continue;
        }

        if (f < 1.0f) {
            // [RELOAD-FADE] von der aktuellen (nativen Reload-End-)Local zur
            // Pin-Pose blenden -> kein Snap.
            glm::vec3 cp{};
            glm::quat cr{1.0f, 0.0f, 0.0f, 0.0f};

            // Lua legt EINEN pcall um Lesen UND Schreiben: scheitert ein
            // Getter, wird fuer diesen Joint GAR NICHTS geschrieben. Wer hier
            // trotzdem schreibt, laesst den Joint hart in die Pin-Pose
            // schnappen -- genau der Snap, den der Fade verhindern soll.
            if (!get_vec3(j, "get_LocalPosition", cp) || !get_quat(j, "get_LocalRotation", cr)) {
                continue;
            }

            p = glm::vec3{cp.x + (p.x - cp.x) * f, cp.y + (p.y - cp.y) * f,
                          cp.z + (p.z - cp.z) * f};
            const glm::quat rr = glm::slerp(cr, rel.r, f);

            set_vec3(j, "set_LocalPosition", p);
            set_quat(j, "set_LocalRotation", rr);
        } else {
            set_vec3(j, "set_LocalPosition", p);
            set_quat(j, "set_LocalRotation", rel.r);
        }
    }
}

// ============================================================================
// (2) Crosshair-Force (Lua Z.255-259)
// ============================================================================

void RE4VRMinecart::force_crosshair() {
    if (!railcar_active()) {
        return;
    }

    // is_reticle_displayed hat im gesamten Live-Set KEINEN Leser (nachgesucht,
    // auch im C++-Baum). Wird trotzdem geschrieben -- 1:1.
    re4vr::lua_set_bool("is_reticle_displayed", true);
    re4vr::lua_set_bool("is_aim", true);
}

// ============================================================================
// [YAW-FOLLOW] Lua Z.276-291
//
// Im Auslieferungszustand ein TOTER PFAD: cfg.yaw_follow steht auf false und
// wird bewusst nicht aus der JSON geladen. Trotzdem 1:1 portiert -- wer den
// Haken in der UI setzt, bekommt exakt das alte Verhalten.
// ============================================================================

void RE4VRMinecart::update_yaw_follow() {
    auto* vr = VR::get().get();

    if (!(m_cfg.yaw_follow && railcar_active() && vr != nullptr && vr->is_hmd_active())) {
        m_yf_entry_body.reset();
        re4vr::lua_set_nil("__re4_railcar_yaw_delta");
        return;
    }

    auto* tf = re4vr::body_transform();

    if (tf == nullptr) {
        return;
    }

    glm::quat br{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(tf, "get_Rotation", br)) {
        return;
    }

    const auto by = yaw_of_quat(br);

    if (!by.has_value()) {
        return;
    }

    const auto hy = yaw_of_quat(glm::quat{vr->get_transform(0)});

    if (!hy.has_value()) {
        return;
    }

    if (!m_yf_entry_body.has_value()) {
        m_yf_entry_body = *by;
        m_yf_entry_hmd = *hy;
    }

    const float delta = yaw_wrap(*by - *m_yf_entry_body) * m_cfg.yaw_sign;

    // firstperson koennte die Hand-Basis um -delta korrigieren (Anti-
    // Doppeldreh). Dieser Konsument wurde nie gebaut -- das Global hat heute
    // keinen Leser ausser der eigenen UI.
    re4vr::lua_set_number("__re4_railcar_yaw_delta", delta);
    re4vr::lua_set_bool("__vr_recenter_hold", true);

    vr->set_rotation_offset(yaw_to_quat(delta - m_yf_entry_hmd.value_or(0.0f)));
}

// ============================================================================
// [MINECART KNIFE] Lua Z.302-317
//
// Kam der Spieler mit Messer in den Kart, gibt das Spiel die Mounted-Gun NICHT.
// Der KnifeCloseTimer wird im Kart nicht getickt -> das Timer-Holster aus
// weapons.lua bleibt wirkungslos. Fix: aktiv auf die Main-Waffe wechseln,
// DEFERRED im updateOnFrameHead-Hook, ~alle 0.4 s solange etwas anderes als die
// Cart-Gun equippt ist.
// ============================================================================

void RE4VRMinecart::cart_knife_swap() {
    const bool in_cart = railcar_active()
                         || re4vr::lua_get_tribool("__re4_minecart_ks4_active") == 1
                         || re4vr::lua_get_tribool("__re4_minecart2_ks4_active") == 1;

    if (!in_cart) {
        return;
    }

    const double now = clock_now();

    if (now - m_ck_last < 0.4) {
        return;
    }

    m_ck_last = now;

    // Universell: die deferred Funktion forced wp4005 (Cart-Gun) und
    // ueberspringt sich selbst, wenn schon 4005 anliegt -- egal, mit welcher
    // Waffe man hereinkam. __re4_knife_equipped diente nur noch dem Log und
    // wird hier deshalb gar nicht mehr gelesen (Lua Z.311-312: totes Local).
    //
    // Beide Funktionen sind seit dem Holster-Port nativ; der Umweg ueber die
    // Lua-Exports __re4_knife_defer / __re4_force_change_to_main entfaellt.
    // ACHTUNG: RE4VRHolster::defer nimmt std::function<void()>, der Lua-Export
    // dagegen eine sol::protected_function -- direkt aufrufen, nicht ueber den
    // sol-Wrapper.
    auto holster = RE4VRHolster::get();

    if (holster != nullptr) {
        holster->defer([]() {
            if (auto h = RE4VRHolster::get(); h != nullptr) {
                h->force_change_to_main();
            }
        });
    }
}

// ============================================================================
// [KIPP-AUSGLEICH PER HMD] Lua Z.348-386
//
// Live gemessen: chainsaw.GmRailCar Tilt = -0.394, OutForceTiltMax = 0.5.
// Tilt NEGATIV = kippt nach links -> Gegensteuern nach rechts (positives LX).
// Drei Gates, alle muessen zutreffen, sonst wird das Global gar nicht erst
// gesetzt und das Binding bleibt unberuehrt.
// ============================================================================

::REManagedObject* RE4VRMinecart::get_player_railcar() {
    if (m_rail_manager.obj == nullptr) {
        store(m_rail_manager,
              sdk::get_managed_singleton<::REManagedObject>("chainsaw.RailCarManager"));
    }

    if (m_rail_manager.obj == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(m_rail_manager.obj, "getPlayerRailCar");
}

void RE4VRMinecart::update_cart_lean() {
    // Standardfall: kein Eingriff. Global IMMER zuerst loeschen, damit ein
    // einzelner ausgefallener Frame nicht zu einem haengenden Stick fuehrt.
    re4vr::lua_set_nil("__re4_cart_lean_lx");
    m_lean_dbg.active = false;

    if (m_cfg.lean_enabled != true) {
        return;
    }

    if (!railcar_active()) {
        return;
    }

    auto* car = get_player_railcar();

    if (car == nullptr) {
        return;
    }

    // [C(b)] Erfolgs-Flag statt Sentinel -- 0.0 ist in Lua WAHR.
    float tilt = 0.0f;

    if (!get_float(car, "get_Tilt", tilt)) {
        return;
    }

    m_lean_dbg.tilt = tilt;

    // Kippphase? Sonst raus -- im normalen Fahren darf das HMD den Stick NIE
    // anfassen.
    if (std::fabs(tilt) < m_cfg.lean_tilt_min) {
        return;
    }

    // get_ReturnTilt ist die einzige Stelle, an der ein Getter legitim false
    // liefern darf; Luas `== true` entschaerft die safe()-Verschmelzung.
    if (re4vr::call_safe<bool>(car, "get_ReturnTilt") == true) {
        return;
    }

    // [KEIN is_hmd_active()-GATE] Als einzige vrmod-Stelle der Datei prueft das
    // Original hier NICHT auf ein aktives HMD -- es verlaesst sich darauf, dass
    // safe() einen Fehler schluckt. In C++ existiert VR::get() immer und
    // get_transform(0) wirft nicht; bei inaktivem HMD liefert die
    // Identitaetsmatrix right=(1,0,0) -> roll=0 -> Frühausstieg unten. Das Gate
    // wird bewusst NICHT nachgeruestet: es waere neues Verhalten.
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    // Echte Kopfneigung im Raum, NICHT die Spielkamera -- die kippt mit dem
    // Kart mit und waere als Messgroesse unbrauchbar. Der Rechts-Vektor des HMD
    // liefert mit seiner Y-Komponente direkt den Sinus des Roll.
    const glm::quat q{vr->get_transform(0)};
    const glm::vec3 right = q * glm::vec3{1.0f, 0.0f, 0.0f};
    const float roll = -right.y; // Kopf nach rechts geneigt -> positiv
    m_lean_dbg.roll = roll;

    const float th = m_cfg.lean_threshold;
    const float full = std::max(th + 0.01f, m_cfg.lean_full);
    const float mag = std::fabs(roll);

    if (mag < th) {
        return; // unter der Schwelle: Kopf-Wackeln ignorieren
    }

    float amt = (mag - th) / (full - th);

    if (amt > 1.0f) {
        amt = 1.0f;
    }

    const float out = amt * ((roll >= 0.0f) ? 1.0f : -1.0f) * m_cfg.lean_sign;

    re4vr::lua_set_number("__re4_cart_lean_lx", out);
    m_lean_dbg.active = true;
    m_lean_dbg.out = out;
}

// ============================================================================
// [AUTO-RECENTER IM KART] Lua Z.411-438
//
// Belegt per Diagnose-Log: nicht der Kopf-Joint wandert, sondern der SPIELER
// steht physisch woanders; RoomScale uebertraegt den Versatz zwischen HMD und
// Origin bewusst. Beim staendigen Gegenlehnen summiert sich das. Hier wird die
// Origin TRAEGE nachgezogen -- nur ausserhalb einer Totzone, damit kurzes
// Vorbeugen frei bleibt. NUR X und Z: die Hoehe bleibt unangetastet.
// ============================================================================

void RE4VRMinecart::update_cart_recenter() {
    auto* vr = VR::get().get();

    if (m_cfg.recenter_enabled != true) {
        m_rc_last_t.reset();
        return;
    }

    if (!railcar_active()) {
        m_rc_last_t.reset();
        return;
    }

    if (vr == nullptr || !vr->is_hmd_active()) {
        m_rc_last_t.reset();
        return;
    }

    const double now = clock_now();
    const double dt = m_rc_last_t.has_value() ? (now - *m_rc_last_t) : 0.0;
    m_rc_last_t = now;

    if (dt <= 0.0 || dt > 0.25) {
        return; // erster Frame / Haenger: nur die Zeitbasis setzen
    }

    const auto hmd = vr->get_position(0);
    auto so = vr->get_standing_origin();

    const float dx = hmd.x - so.x;
    const float dz = hmd.z - so.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    m_rc_dbg.off = dist;
    m_rc_dbg.moved = 0.0f;

    const float dead = m_cfg.recenter_dead;

    if (dist <= dead) {
        return; // innerhalb der Totzone: nichts tun
    }

    // Nur den Anteil JENSEITS der Totzone abbauen, hoechstens rate*dt.
    const float step = std::min(m_cfg.recenter_rate * static_cast<float>(dt), dist - dead);

    if (step <= 0.0f) {
        return;
    }

    const float ux = dx / dist;
    const float uz = dz / dist;
    so.x += ux * step;
    so.z += uz * step;
    vr->set_standing_origin(so);
    m_rc_dbg.moved = step;
}

// ============================================================================
// [ZUG-RUMBLE] Lua Z.463-543
//
// Leichtes Dauer-Rumpeln in beiden Controllern, solange man im Kart FAEHRT.
// Alle rumble_rate Sekunden ein kurzer Puls mit tiefer Frequenz; die Amplitude
// wird mit zwei ueberlagerten Sinussen moduliert statt mit Zufall -- so wird das
// Rollen ungleichmaessig, wiederholt sich aber nicht hoerbar. Dazu alle
// rumble_clack_every Sekunden ein Schienenstoss, abwechselnd links/rechts.
// ============================================================================

bool RE4VRMinecart::cart_ride_now() const {
    if (railcar_active()) {
        return true;
    }

    // Der Intro-EINSTIEG (ActionCam, __re4_minecart_ks4_active) bleibt bewusst
    // still -- da sitzt man noch nicht.
    if (m_cfg.rumble_intro && re4vr::lua_get_tribool("__re4_minecart2_ks4_active") == 1) {
        return true;
    }

    return false;
}

float RE4VRMinecart::cart_speed_now() {
    // [TEMPO-GATE] "Im Kart" allein taugt nicht als Schalter: der Zustand steht
    // schon beim ANSCHIEBEN und noch, wenn das Kart laengst haelt. Gemessen
    // wird deshalb der Positionswechsel -- am KART selbst, damit das Laufen des
    // Spielers nichts vortaeuscht; ist das Kart nicht greifbar (Intro-Fahrt),
    // faellt es auf den Body zurueck (der ist dann ans Kart geparentet).
    ::REManagedObject* tf = nullptr;
    const char* src = "-";

    auto* car = get_player_railcar();

    if (car != nullptr) {
        auto* go = re4vr::call_safe<::REManagedObject*>(car, "get_GameObject");

        if (go != nullptr) {
            tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        }

        if (tf != nullptr) {
            src = "Kart";
        }
    }

    if (tf == nullptr) {
        tf = re4vr::body_transform();

        if (tf != nullptr) {
            src = "Body";
        }
    }

    m_spd_src = src;

    if (tf == nullptr) {
        m_spd_t.reset();
        m_spd_v = 0.0f;
        return 0.0f;
    }

    glm::vec3 p{};

    if (!get_vec3(tf, "get_Position", p)) {
        return m_spd_v;
    }

    const double now = clock_now();

    if (!m_spd_t.has_value()) {
        m_spd_t = now;
        m_spd_p = p;
        return m_spd_v;
    }

    const double dt = now - *m_spd_t;

    // Deltas erst ab 0.03 s (bei 90 fps waeren Frame-Deltas fast nur Rauschen),
    // danach EMA-geglaettet, damit ein einzelner Ruckler weder zuendet noch
    // aussetzt.
    if (dt >= 0.03) {
        const glm::vec3 d = p - m_spd_p;
        float v = static_cast<float>(
            std::sqrt(static_cast<double>(d.x * d.x + d.y * d.y + d.z * d.z)) / dt);

        if (v > 60.0f) {
            v = m_spd_v; // Teleport/Stage-Wechsel nicht als Tempo werten
        }

        m_spd_v += (v - m_spd_v) * 0.35f;
        m_spd_t = now;
        m_spd_p = p;
    }

    return m_spd_v;
}

void RE4VRMinecart::update_cart_rumble() {
    auto* vr = VR::get().get();

    if (!m_cfg.rumble_enabled) {
        m_rum_t0.reset();
        m_rum_dbg.on = false;
        return;
    }

    if (vr == nullptr || !vr->is_hmd_active()) {
        m_rum_t0.reset();
        m_rum_dbg.on = false;
        return;
    }

    if (!cart_ride_now()) {
        m_rum_t0.reset();
        m_rum_dbg.on = false;
        m_rum_dbg.amp = 0.0f;
        m_spd_t.reset();
        return;
    }

    const double now = clock_now();

    if (!m_rum_t0.has_value()) {
        m_rum_t0 = now;
        m_rum_next_t = 0.0;
        m_rum_next_clack = now + m_cfg.rumble_clack_every;
    }

    // [TEMPO-GATE] JEDEN Frame messen (nicht nur im Puls-Takt), damit die
    // LIVE-Anzeige beim Einstellen stimmt und das Auslaufen sofort greift.
    const float v = cart_speed_now();
    const float vmin = m_cfg.rumble_speed_min;
    const float vfull = std::max(vmin + 0.1f, m_cfg.rumble_speed_full);

    if (v < vmin) {
        m_rum_dbg.on = false;
        m_rum_dbg.amp = 0.0f;
        return; // Schieben/Stillstand: still
    }

    float vk = 1.0f;

    if (m_cfg.rumble_speed_scale) {
        vk = (v - vmin) / (vfull - vmin);

        if (vk < 0.0f) {
            vk = 0.0f;
        } else if (vk > 1.0f) {
            vk = 1.0f;
        }
    }

    m_rum_dbg.on = true;

    if (now < m_rum_next_t) {
        return;
    }

    m_rum_next_t = now + std::max(0.03f, m_cfg.rumble_rate);

    const float t = static_cast<float>(now - *m_rum_t0);
    const float wob = 0.75f + 0.15f * std::sin(t * 7.3f) + 0.10f * std::sin(t * 2.1f + 1.3f);
    float amp = m_cfg.rumble_amp * wob * vk;
    float dur = std::max(0.02f, m_cfg.rumble_dur);
    const float frq = std::max(10.0f, m_cfg.rumble_freq);

    float la = amp;
    float ra = amp;

    if (m_cfg.rumble_clack && now >= m_rum_next_clack) {
        m_rum_next_clack = now + std::max(0.3f, m_cfg.rumble_clack_every);
        m_rum_side = (m_rum_side == 0) ? 1 : 0;
        const float ca = m_cfg.rumble_clack_amp * vk;

        if (m_rum_side == 0) {
            la = ca;
            ra = ca * 0.55f;
        } else {
            la = ca * 0.55f;
            ra = ca;
        }

        dur = std::min(0.10f, dur + 0.03f);
    }

    // [KEINE HANDLE-NULLPRUEFUNG] Luas `if lj then` ist KEIN Test auf
    // Gueltigkeit: 0 ist in Lua WAHR, der Puls feuert dort immer. Unter OpenXR
    // ist get_left_joystick() aber genau 0 (VRRuntime::Hand::LEFT), also
    // identisch mit k_ulInvalidInputValueHandle -- ein Vergleich dagegen haette
    // den linken Controller im Kart dauerhaft stumm gestellt und dem
    // Schienenstoss die Seitenabwechslung genommen.
    // RE4VRRecoil und RE4VRHolster verzichten aus demselben Grund darauf.
    const auto lj = vr->get_left_joystick();
    const auto rj = vr->get_right_joystick();

    vr->trigger_haptic_vibration(0.0f, dur, frq, la, lj);
    vr->trigger_haptic_vibration(0.0f, dur, frq, ra, rj);

    m_rum_dbg.amp = (la + ra) * 0.5f;
}

// ============================================================================
// Phasen (Lua Z.550-578)
// ============================================================================

void RE4VRMinecart::on_frame() {
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen. re4_vr_minecart.lua steht NICHT in der
    // BLEIBT-Liste von objects.
    if (re4vr::mods_gated()) {
        return;
    }

    ensure_t_motion();

    // Eigener on_frame: Haptik gehoert in keinen Render-Pass (die Entries unten
    // laufen mehrfach pro Frame).
    update_cart_rumble();
}

void RE4VRMinecart::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr || std::string_view{name} != "LockScene") {
        return;
    }

    update_cart_recenter(); // self-gated: zieht die Origin nur im Kart nach
    update_cart_lean();     // self-gated: setzt das Global nur im Kart + Kippphase
    update_yaw_follow();    // self-gated
    cart_knife_swap();      // VOR dem railcar-Gate: greift auch in den Intros

    if (!railcar_active()) {
        // [1:1 -- NICHT "aufraeumen"] Nur das Reload-Flag wird geraeumt.
        // __re4_railcar_reload_fade bleibt bewusst stehen: verlaesst man den
        // Kart mitten im Reload, startet die naechste Fahrt mit angerampter
        // Pose und braucht ~8 Frames zum Hochlaufen. Das ist ein Fehler des
        // Originals, aber ein GETESTETER -- wer ihn hier behebt, aendert
        // Verhalten.
        re4vr::lua_set_bool("__re4_railcar_reloading", false);
        return;
    }

    update_anim_export();
    update_reload_flag();

    // [RELOAD-FADE] Reload-Entry: arm_chain pausiert dann (ruft den Pin NICHT)
    // -> hier selbst den eased Pin schreiben, solange fade > 0. Beachte den
    // Default 0.0 (nicht 1.0 wie in apply_spine_pin).
    if (re4vr::lua_get_tribool("__re4_railcar_reloading") == 1
        && re4vr::lua_get_number("__re4_railcar_reload_fade", 0.0) > 0.0) {
        apply_spine_pin();
    }

    force_crosshair();
}

void RE4VRMinecart::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (name == nullptr || std::string_view{name} != "LateUpdateBehavior") {
        return;
    }

    // [RELOAD-FADE] Entry-Ease auch post-anim re-asserten, sonst ueberschreibt
    // die Engine-Anim den eased Pin.
    if (re4vr::lua_get_tribool("__re4_railcar_reloading") == 1
        && re4vr::lua_get_number("__re4_railcar_reload_fade", 0.0) > 0.0) {
        apply_spine_pin();
    }

    force_crosshair();
}

// ============================================================================
// UI (Lua Z.580-661)
// ============================================================================

void RE4VRMinecart::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (re4vr::mods_gated()) {
        return;
    }

    if (!ImGui::TreeNode("RE4VR - Minecart")) {
        return;
    }

    ImGui::Text("Greift in allen drei Loren-Zustaenden (Intro-Einstieg, Intro-Fahrt, echte Fahrt).");
    ImGui::Text("Kamera ankert am gepinnten Head -> kein Tilt-Offset mehr noetig.");

    if (ImGui::DragFloat("Oberkoerper runter / Sitzen (m)", &m_cfg.body_down, 0.005f, 0.0f, 1.0f, "%.3f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Oberkoerper zurueck / weiter hinten (m)", &m_cfg.body_back, 0.005f, -0.5f, 0.5f, "%.3f")) {
        save_cfg();
    }

    ImGui::Text("(zurueck zieht Hip+Kette inkl. Head -> Kamera geht mit. Vorzeichen umkehren falls falsche Richtung.)");

    ImGui::Separator();
    ImGui::Text("-- Kipp-Ausgleich per Kopfneigung (nur im Kart, nur waehrend einer Kippphase) --");

    if (ImGui::Checkbox("Mit dem Kopf gegenlehnen statt rechtem Stick", &m_cfg.lean_enabled)) {
        save_cfg();
    }

    if (ImGui::DragFloat("Ansprechschwelle (sin Roll)", &m_cfg.lean_threshold, 0.01f, 0.05f, 0.60f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Voller Ausschlag ab (sin Roll)", &m_cfg.lean_full, 0.01f, 0.10f, 0.95f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Kippphase ab Kart-Neigung", &m_cfg.lean_tilt_min, 0.01f, 0.02f, 0.50f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::Button("Gleicht falschherum aus? Richtung umkehren")) {
        m_cfg.lean_sign = -m_cfg.lean_sign;
        save_cfg();
    }

    // Lua formatiert die Stick-Ausgabe mit "%.2f" -- std::to_string haette
    // sechs Nachkommastellen gezeigt.
    char lean_out[32]{};

    if (m_lean_dbg.active) {
        std::snprintf(lean_out, sizeof(lean_out), "%.2f", m_lean_dbg.out);
    } else {
        std::snprintf(lean_out, sizeof(lean_out), "aus");
    }

    ImGui::Text("Richtung = %.0f   |   LIVE  Kart-Tilt = %.3f   Kopf-Roll = %.3f   Stick-Ausgabe = %s",
                m_cfg.lean_sign, m_lean_dbg.tilt, m_lean_dbg.roll, lean_out);
    ImGui::Text("Tilt negativ = Kart kippt nach links -> Kopf nach rechts neigen.");

    ImGui::Separator();
    ImGui::Text("-- Auto-Recenter im Kart (gegen das Wegdriften vom Kopf beim staendigen Lehnen) --");

    if (ImGui::Checkbox("Sitzposition langsam nachfuehren", &m_cfg.recenter_enabled)) {
        save_cfg();
    }

    if (ImGui::DragFloat("Totzone (m) - darunter passiert nichts", &m_cfg.recenter_dead, 0.01f, 0.0f, 0.50f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Nachzieh-Tempo (m/s)", &m_cfg.recenter_rate, 0.05f, 0.05f, 2.0f, "%.2f")) {
        save_cfg();
    }

    ImGui::Text("LIVE  Versatz zum Ankerpunkt = %.3f m   |   zieht gerade %.4f m/Frame nach",
                m_rc_dbg.off, m_rc_dbg.moved);

    ImGui::Separator();
    ImGui::Text("-- Yaw-Follow: Blick-Basis dreht mit der Kart-Fahrtrichtung --");

    if (ImGui::Checkbox("Yaw-Follow (geradeaus = nach vorn aus dem Kart)", &m_cfg.yaw_follow)) {
        save_cfg();
    }

    if (ImGui::Button("Dreht falschherum? Richtung umkehren")) {
        m_cfg.yaw_sign = -m_cfg.yaw_sign;
        save_cfg();
    }

    ImGui::Text("Richtung = %.0f   |   LIVE Yaw-Delta = %.1f Grad", m_cfg.yaw_sign,
                re4vr::lua_get_number("__re4_railcar_yaw_delta", 0.0) * 57.2957795130823);

    ImGui::Separator();
    ImGui::Text("-- Zug-Rumble: leichtes Rumpeln in beiden Controllern waehrend der Fahrt --");

    if (ImGui::Checkbox("Rumpeln an", &m_cfg.rumble_enabled)) {
        save_cfg();
    }

    if (ImGui::DragFloat("Staerke (0 = aus, 1 = voll)", &m_cfg.rumble_amp, 0.01f, 0.0f, 1.0f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Takt (s zwischen zwei Pulsen)", &m_cfg.rumble_rate, 0.01f, 0.03f, 0.50f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Pulslaenge (s)", &m_cfg.rumble_dur, 0.01f, 0.02f, 0.30f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Frequenz (Hz) - tief = dumpfes Rollen", &m_cfg.rumble_freq, 1.0f, 10.0f, 250.0f, "%.0f")) {
        save_cfg();
    }

    if (ImGui::Checkbox("Schienenstoesse (ta-tak, wechselt links/rechts)", &m_cfg.rumble_clack)) {
        save_cfg();
    }

    if (ImGui::DragFloat("Stoss-Abstand (s)", &m_cfg.rumble_clack_every, 0.05f, 0.30f, 6.0f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Stoss-Staerke", &m_cfg.rumble_clack_amp, 0.01f, 0.0f, 1.0f, "%.2f")) {
        save_cfg();
    }

    if (ImGui::Checkbox("auch in der Intro-Fahrt (VehicleCam)", &m_cfg.rumble_intro)) {
        save_cfg();
    }

    ImGui::Text("Nur ab Fahrt-Tempo (gegen Rumpeln beim Anschieben und beim Auslaufen):");

    if (ImGui::Checkbox("Staerke mit dem Tempo hochblenden", &m_cfg.rumble_speed_scale)) {
        save_cfg();
    }

    if (ImGui::DragFloat("Mindest-Tempo (m/s) - darunter still", &m_cfg.rumble_speed_min, 0.1f, 0.0f, 20.0f, "%.1f")) {
        save_cfg();
    }

    if (ImGui::DragFloat("Volles Tempo (m/s) - ab hier volle Staerke", &m_cfg.rumble_speed_full, 0.1f, 0.5f, 40.0f, "%.1f")) {
        save_cfg();
    }

    ImGui::Text("LIVE  laeuft = %s   |   Staerke = %.2f   |   Tempo = %.1f m/s (Quelle: %s)",
                m_rum_dbg.on ? "JA" : "nein", m_rum_dbg.amp, m_spd_v, m_spd_src.c_str());
    ImGui::Text("Einstellen: beim Anschieben und beim Auslaufen das LIVE-Tempo ablesen -> Mindest-Tempo dazwischen legen.");

    ImGui::TreePop();
}

#endif // RE4
