// ============================================================================
// RE4VRWeapons2 -- 1:1-Portierung von re4_vr_weapons2.lua (3.500 Zeilen).
// Spezifikation: I:\LUATRANS\PORT_WEAPONS2_SPEC.md
// ============================================================================

#if defined(RE4)

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>

#include <utility/String.hpp>

// THIS MUST BE INCLUDED OR THE LOG FILE WILL BALLOON TO GIGANTIC SIZE
// AND THE GAME MAY CRASH. THIS IS REQUIRED FOR THE sol_lua_push DECLARATION.
#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VR.hpp"
#include "RE4VRHolster.hpp"
#include "RE4VRWeapons2.hpp"

// windows.h definiert min/max als MAKROS -- der Fork setzt kein NOMINMAX.
#undef min
#undef max

namespace {

// Lua: os.clock(). Fremde Scripte schreiben Zeitstempel in dieselben Globals,
// deshalb MUSS die Zeitbasis std::clock()/CLOCKS_PER_SEC sein.
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

// ValueType-Rueckgaben brauchen den sret-Puffer PLUS ein Erfolgs-Flag:
// re4vr::call_safe verschluckt den Unterschied zwischen "hat 0 geliefert" und
// "ging nicht", und in Lua ist 0 WAHR.
// Siehe [[reference_re4_cpp_valuetype_rueckgabe_sret]].
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

    // via.Quaternion liegt als x,y,z,w im Speicher; glm::quat ist w,x,y,z.
    out = glm::quat{v.w, v.x, v.y, v.z};

    return true;
}

// [PORTFIX 2026-09-06] getJointByName(System.String) braucht einen MANAGED
// String. call_safe reicht die Argumente ungewandelt an den nativen
// Funktionszeiger durch -- ein roher const char* wird von der Engine als
// SystemString-Header gelesen und liefert Muell bzw. eine Managed-Exception,
// die call_safe still schluckt. Folge war: Joint nie gefunden, Pivot nie
// berechnet, Twirl drehte um den Waffen-Origin statt um den Trigger-Joint.
// Alle uebrigen 15 Module hatten diesen Helfer bereits.
::REManagedObject* joint_by_name(::REManagedObject* tf, const char* name) {
    if (tf == nullptr || name == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    return str != nullptr
        ? re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str)
        : nullptr;
}

bool set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{v.x, v.y, v.z, 0.0f};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
        return false;
    }

    return true;
}

bool set_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 arg{q.x, q.y, q.z, q.w};

    try {
        method->call_safe<void*>(context, obj, &arg);
    } catch (...) {
        return false;
    }

    return true;
}

// Lua: quat_from_euler_deg -- ZYX -> Quaternion (W,X,Y,Z).
glm::quat quat_from_euler_deg(float rx, float ry, float rz) {
    const float hx = glm::radians(rx) * 0.5f;
    const float hy = glm::radians(ry) * 0.5f;
    const float hz = glm::radians(rz) * 0.5f;

    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);

    return glm::quat{cx * cy * cz + sx * sy * sz,
                     sx * cy * cz - cx * sy * sz,
                     cx * sy * cz + sx * cy * sz,
                     cx * cy * sz - sx * sy * cz};
}

std::string obj_name_of(::REManagedObject* obj) {
    if (!re4vr::obj_ok(obj)) {
        return {};
    }

    return re4vr::obj_name(obj);
}

// Enum-GETTER als Zahl lesen. Lua schreibt ueberall dasselbe Muster:
//   local v = o:call("get_X")
//   if type(v) == "number" then return v end
//   return v.value__
// Als OBJEKTZEIGER gelesen ist ein Enum toedlich -- das war der Choke-Absturz.
std::optional<int32_t> enum_as_int(::REManagedObject* obj, std::string_view getter) {
    if (!re4vr::obj_ok(obj)) {
        return std::nullopt;
    }

    int32_t v = 0;

    if (re4vr::try_call<int32_t>(obj, getter, v)) {
        return v;
    }

    auto* w = re4vr::call_safe<::REManagedObject*>(obj, getter);

    return w != nullptr ? re4vr::get_field_int(w, "value__") : std::nullopt;
}

// ---------------------------------------------------------------------------
// Konstanten aus der Lua
// ---------------------------------------------------------------------------

// Block 1+2: KNIFE_IDS. In der Lua ZWEIMAL definiert (Z.400 und Z.1664) --
// beide Male mit demselben Inhalt.
bool is_knife_id(int32_t wid) {
    switch (wid) {
    case 5000:
    case 5001:
    case 5002:
    case 5003:
    case 5006:
    case 6107:
    case 6108:
    case 6305:
        return true;
    default:
        return false;
    }
}

// [ADA ONLY 2026-07-21] Messer, die es NUR bei Ada gibt (Separate Ways).
// Bewusst OHNE 6305 (Hot Dogger, Mercenaries) und ohne Leons 5000..5006.
bool is_ada_knife_id(int32_t wid) {
    return wid == 6107 || wid == 6108;
}

// [WESKER_ARM_PARRY] KindID aus re4_vr_merc.lua.
constexpr int32_t WESKER_KIND = 600005;

// [WAFFE ZURUECK] Wie lange nach dem Loslassen der Armblock-Pose der
// Auto-Redraw die Waffe zurueckholen darf.
constexpr float WPARRY_REDRAW_T = 2.0f;

constexpr const char* PARRY_CFG_PATH = "re4_vr/re4_vr_knife_parry.json";

// [PARRY-SOUND LINKS 16.09.2026 -- Ansage des Users] Beim Parry mit dem
// KLON-Messer links klingt nichts: der Klon hat keinen eigenen Parry-Sound, nur
// das echte Messer rechts. Deshalb wird die Wwise-ID hier selbst gezuendet.
// 0 = aus. Die ID liefert der User -- UInt32, s. knife_lh_play_sound.
constexpr uint32_t LH_PARRY_SOUND_ID = 1081366929;

// Der Hook laeuft in JEDEM Frame des Parry-Fensters -- der Ton soll pro Parry
// nur EINMAL kommen.
constexpr double LH_PARRY_SOUND_GAP = 1.0;

// Hook (Spiel-Update) merkt an, on_frame (parry_keep_gun_tick) spielt ab.
std::atomic<bool> g_lh_parry_sound_pending{false};
double g_lh_parry_sound_t = -100.0;

// [PARRY] Kalibrierte Referenz-Pose (rechter Controller RELATIV zum HMD),
// Snapshot 2026-07-04 (natuerliche Halte-Pose, nicht ausgestreckt).
constexpr float PREF_PX = 0.026f, PREF_PY = 0.015f, PREF_PZ = -0.469f;
constexpr float PREF_QW = 0.057f, PREF_QX = 0.653f, PREF_QY = -0.054f, PREF_QZ = 0.753f;

// [WESKER_ARM_PARRY 2026-08-26, GEMESSEN] Eigene Referenzpose fuer Weskers
// linken Arm -- NICHT die gespiegelte Messer-Referenz. Aus einer Ruhe-Erkennung
// (Arm musste 1,5 s still stehen).
constexpr float WREF_PX = 0.178f, WREF_PY = -0.021f, WREF_PZ = -0.347f;
constexpr float WREF_QW = -0.016f, WREF_QX = -0.734f, WREF_QY = 0.246f, WREF_QZ = 0.633f;

// Eigene Toleranz fuer Wesker -- BEWUSST getrennt von KCFG.tol.
// [ZURUECKGENOMMEN 2026-08-29] Nur die Position blieb enger (0.18), die
// Rotation ging auf ihren gemessenen Wert zurueck.
constexpr float WCFG_POS_TOL = 0.18f;
constexpr float WCFG_ROT_TOL = 0.60f;

}   // namespace

// ============================================================================
// Boilerplate
// ============================================================================

std::shared_ptr<RE4VRWeapons2>& RE4VRWeapons2::get() {
    static auto inst = std::make_shared<RE4VRWeapons2>();

    return inst;
}

void RE4VRWeapons2::store(Handle& h, ::REManagedObject* obj, bool unconditional) {
    drop(h);

    if (obj == nullptr) {
        return;
    }

    h.obj = obj;

    // [SELBST ERZEUGTE OBJEKTE] Die refcount-Heuristik verankert ein frisches
    // Objekt NICHT -- bei create_instance MUSS bedingungslos gepinnt werden.
    if (unconditional) {
        try {
            utility::re_managed_object::add_ref(obj);
            h.reffed = true;
        } catch (...) {
            h.reffed = false;
        }

        return;
    }

    try {
        if (utility::re_managed_object::is_managed_object(obj)) {
            utility::re_managed_object::add_ref(obj);
            h.reffed = true;
        }
    } catch (...) {
        h.reffed = false;
    }
}

void RE4VRWeapons2::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        try {
            utility::re_managed_object::release(h.obj);
        } catch (...) {
        }
    }

    h.obj = nullptr;
    h.reffed = false;
}

// [PORTFIX 2026-09-06] Die vier LCFG-Globals an Lua geben. Wird aus
// on_lua_state_created gerufen (dort lebt der State) und nach jedem
// Slider-Speichern.
void RE4VRWeapons2::publish_lcfg_globals() {
    re4vr::lua_set_bool("__re4_knife_blood_on", m_lcfg.blood_on);
    re4vr::lua_set_number("__re4_knife_lt_flip_tap", m_lcfg.tap_sec);
    re4vr::lua_set_number("__re4_knife_lh_flip_speed", m_lcfg.flip_speed);
    re4vr::lua_set_number("__re4_knife_lh_swing_speed", m_lcfg.swing_speed);
}

std::optional<std::string> RE4VRWeapons2::on_initialize() {
    load_parry_cfg();

    // Lua Z.418: lcfg_load(LCFG_PATH) laeuft beim Laden mit LEONS Datei --
    // noch bevor die Charaktererkennung gelaufen ist.
    lcfg_load("re4_vr/re4_vr_knife_lefthand.json");

    // [PORTFIX 2026-09-06] Die vier Globals, die lcfg_load in Lua direkt
    // danach setzt, stehen NICHT mehr hier: on_initialize laeuft auf dem
    // Init-Thread, der Lua-State entsteht erst spaeter im on_frame des
    // ScriptRunners -- lua_set_* ist hier ein stiller No-Op. Die Werte
    // existierten dadurch nie, und die Leser fielen auf ihre
    // Compile-Defaults zurueck: Links-Flip mit 0.5 statt 0.15 (3,3x zu
    // schnell) und ein Tap-Fenster von 0.18 statt 0.23 s -- wodurch ein
    // normaler Tap als HOLD galt, der Reverse-Grip stehen blieb und damit
    // das Wurf-Windup dauerhaft gesperrt war. Das Original setzt sie im
    // DATEIRUMPF, also zwangslaeufig mit lebendem State; der entsprechende
    // Platz ist on_lua_state_created. Genau diese Falle ist in
    // RE4VRMotion::on_initialize bereits dokumentiert und dort behoben.
    publish_lcfg_globals();

    // [PORTFIX 2026-09-06] ww_load_cfg() stand HIER und war damit fast
    // wirkungslos: 8 seiner Uebernahmen laufen ueber lua_set_*, und
    // on_initialize hat noch keinen Lua-State (stiller No-Op). Damit blieben
    // ALLE Wild-West-Werte aus der JSON aus -- allen voran `dir` (JSON: -1)
    // -> die Waffe drehte in die falsche Richtung -- plus offset/sens/speed/
    // sustain/rt_sens. Der Aufruf gehoert nach on_lua_state_created und muss
    // dort bei JEDEM Reset Scripts erneut laufen, weil ein Reset einen neuen
    // sol::state anlegt und die Globals sonst wieder auf den Defaults stehen.

    // [ERSTER TREFFER 2026-08-14] Beim Laden einmal bedingungslos abwerfen.
    knife_drop_caches("Script-Load");

    save_vals_load();

    return Mod::on_initialize();
}

// ============================================================================
// Block 1 -- Finisher-Shake + Parry (Lua Z.13-378)
// ============================================================================

void RE4VRWeapons2::load_parry_cfg() {
    const auto d = re4vr::json_load(PARRY_CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    if (const auto it = d.find("parry_tol"); it != d.end() && it->is_number()) {
        m_parry_tol = it->get<float>();
    }
}

void RE4VRWeapons2::save_parry_cfg() {
    nlohmann::json d{};
    d["parry_tol"] = m_parry_tol;
    re4vr::json_save(PARRY_CFG_PATH, d);
}

namespace {

// Lua: prompt_visible() -- __re4_is_finisher_prompt ist eine FUNKTION
// (Export von crosshair, heute nativ).
bool prompt_visible() {
    return re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false);
}

bool reverse_grip() {
    return re4vr::lua_get_tribool("__vr_knife_flip") == 1;
}

// [LH_CLONE] auch der Links-Klon zaehlt als "Messer draussen" (nicht
// engine-equippt -> knife_equipped=false).
bool knife_out() {
    return re4vr::lua_get_tribool("__re4_knife_equipped") == 1
        || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
}

}   // namespace

void RE4VRWeapons2::knife_tick() {
    // [KS_GLOBAL 2026-07-15] JEDER Killswitch, nicht nur der Kick.
    if (re4vr::lua_get_tribool("__re4_ks_active") == 1) {
        // [WESKER_ARM_PARRY] Das HALTENDE Flag darf hier nicht stehenbleiben --
        // sonst klemmt LB im Killswitch fest, und Weskers BulletRush ist genau
        // so ein Killswitch (~31 % der Frames, gemessen).
        re4vr::lua_set_bool("__re4_wesker_parry_hold", false);

        // [WAFFE ZURUECK] Die Flanken-Erinnerung MUSS hier mit fallen, sonst
        // meldet der erste Frame nach dem Killswitch ein "Pose losgelassen",
        // das nie stattgefunden hat. Ein Redraw-Fenster wird BEWUSST nicht
        // geoeffnet.
        m_st.wesker_hold_prev = false;

        return;
    }

    const double now = clock_now();

    // Fire-Flag = frisch, binding liest es. Auto-Clear nach CFG.fire.
    re4vr::lua_set_bool("__re4_knife_finisher_shake", now < m_st.fire_until);

    auto& vr = VR::get();
    const bool hmd = vr != nullptr && vr->is_hmd_active();

    // ------------------------------------------------------------------
    // [PARRY_POSE] Messer schuetzend vor dem Gesicht?
    // ------------------------------------------------------------------
    {
        bool pose = false;

        // [WESKER_ARM_PARRY] Zweiter, streng gegateter Weg in dieselbe Messung:
        // Mercenaries UND KindID 600005.
        const bool wesker_arm = re4vr::lua_get_tribool("__re4_in_mercs") == 1
            && static_cast<int32_t>(re4vr::lua_get_number("__re4_merc_kind", -1.0)) == WESKER_KIND;

        // [PARRY] NUR wenn das Messer wirklich in der Hand ist: waehrend eines
        // Wurfs bleibt knife_out true -> __re4_knife_flying ausschliessen.
        if ((knife_out() || wesker_arm) && re4vr::lua_get_tribool("__re4_knife_flying") != 1
            && hmd) {
            // [KNIFE_HAND] links = ctrls[0] + gespiegelte Referenz,
            // rechts = ctrls[1] + PREF.
            // [WESKER_ARM_PARRY] Wesker IMMER links.
            //
            // ACHTUNG PORT: Lua zaehlt Controller 1-basiert (ctrls[1]=links),
            // C++ 0-basiert (controllers[0]=links).
            const bool left = wesker_arm
                || re4vr::lua_get_string("__re4_knife_hand") == "left"
                || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;

            const auto& ctrls = vr->get_controllers();
            const size_t cidx = left ? 0u : 1u;

            if (ctrls.size() > cidx) {
                const glm::vec3 hp{vr->get_position(0)};
                const glm::quat hq{vr->get_rotation(0)};
                const glm::vec3 rp{vr->get_position(ctrls[cidx])};
                const glm::quat rq{vr->get_rotation(ctrls[cidx])};

                float rpx, rpy, rpz, rqw, rqx, rqy, rqz;

                if (wesker_arm) {
                    rpx = WREF_PX;
                    rpy = WREF_PY;
                    rpz = WREF_PZ;
                    rqw = WREF_QW;
                    rqx = WREF_QX;
                    rqy = WREF_QY;
                    rqz = WREF_QZ;
                } else {
                    // Referenz je Hand: rechts = PREF (kalibriert),
                    // links = PREF horizontal gespiegelt (Pos-X negiert,
                    // Quaternion (w,x,-y,-z) = sagittale Spiegelung).
                    rpx = left ? -PREF_PX : PREF_PX;
                    rpy = PREF_PY;
                    rpz = PREF_PZ;
                    rqw = PREF_QW;
                    rqx = PREF_QX;
                    rqy = left ? -PREF_QY : PREF_QY;
                    rqz = left ? -PREF_QZ : PREF_QZ;
                }

                const glm::quat hqc = glm::conjugate(hq);
                const glm::vec3 relp = hqc * (rp - hp);
                const glm::quat relq = glm::normalize(hqc * rq);

                const float pdx = relp.x - rpx;
                const float pdy = relp.y - rpy;
                const float pdz = relp.z - rpz;
                const float pdist = std::sqrt(pdx * pdx + pdy * pdy + pdz * pdz);

                float dot = std::abs(relq.w * rqw + relq.x * rqx + relq.y * rqy + relq.z * rqz);

                if (dot > 1.0f) {
                    dot = 1.0f;
                }

                const float rang = 2.0f * std::acos(dot);

                float ptol, rtol;

                if (wesker_arm) {
                    const float t =
                        static_cast<float>(re4vr::lua_get_number("__re4_wesker_pose_tol", 1.0));
                    ptol = WCFG_POS_TOL * t;
                    rtol = WCFG_ROT_TOL * t;
                } else {
                    ptol = m_pcfg.pos_tol * m_parry_tol;
                    rtol = m_pcfg.rot_tol * m_parry_tol;
                }

                if (pdist < ptol && rang < rtol) {
                    pose = true;
                }
            }
        }

        if (wesker_arm) {
            // [WESKER_ARM_PARRY] Eigener Ausgang, bewusst mit ANDERER Logik:
            // Wesker HAELT den Block, solange die Pose steht -- kein frisches
            // 0,5-s-Fenster und kein __re4_knife_parry_pose.
            re4vr::lua_set_bool("__re4_wesker_parry_hold", pose);
            re4vr::lua_set_bool("__re4_knife_parry_pose", false);

            // [WAFFE ZURUECK 2026-08-29] Beim LOSLASSEN der Pose (Flanke
            // true -> false) ein Fenster aufmachen, das der Auto-Redraw in
            // holster als dritter erlaubter Grund gilt.
            if (m_st.wesker_hold_prev && !pose) {
                re4vr::lua_set_number("__re4_wesker_parry_recent_until",
                                      now + static_cast<double>(WPARRY_REDRAW_T));
            }

            m_st.wesker_hold_prev = pose;
        } else {
            re4vr::lua_set_bool("__re4_wesker_parry_hold", false);
            re4vr::lua_set_bool("__re4_knife_parry_pose", pose);

            // [TIMING] nur die FRISCHE Flanke oeffnet ein kurzes Parry-Fenster.
            if (pose && !m_st.parry_pose_prev) {
                re4vr::lua_set_number("__re4_knife_parry_fresh_until",
                                      now + static_cast<double>(m_pcfg.window));
            }
        }

        m_st.parry_pose_prev = pose;
    }

    // ------------------------------------------------------------------
    // GATE: nur Messer draussen + Reverse-Grip + Prompt sichtbar
    // ------------------------------------------------------------------
    const bool gate = knife_out() && reverse_grip() && prompt_visible();

    if (!gate || now < m_st.cooldown_end) {
        m_st.last_p.reset();
        m_st.reversals = 0;
        m_st.last_dir = 0;

        return;
    }

    if (!hmd) {
        m_st.last_p.reset();

        return;
    }

    const auto& controllers = vr->get_controllers();

    // [LH_CLONE/KNIFE_HAND] Messer links -> linker Controller, sonst rechts.
    const bool shake_left = re4vr::lua_get_string("__re4_knife_hand") == "left"
        || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
    const size_t cidx = shake_left ? 0u : 1u;

    if (controllers.size() <= cidx) {
        m_st.last_p.reset();

        return;
    }

    // Hand-Wechsel -> alte last_p stammt vom ANDEREN Controller ->
    // Velocity-Spike -> Phantom-Reversal.
    if (m_st.last_cidx != static_cast<int32_t>(cidx)) {
        m_st.last_cidx = static_cast<int32_t>(cidx);
        m_st.last_p.reset();
        m_st.reversals = 0;
        m_st.last_dir = 0;
    }

    const glm::vec3 wp{vr->get_position(controllers[cidx])};

    const double dt = now - m_st.last_time;

    // on_frame kann mehrfach pro Present feuern -> Mini-dt verwerfen, State
    // unangetastet.
    if (dt <= 0.001) {
        return;
    }

    m_st.last_time = now;

    // zu grosse Luecke -> sauberer Neustart
    if (dt >= 0.2) {
        m_st.last_p = wp;
        m_st.last_dir = 0;
        m_st.reversals = 0;

        return;
    }

    // Sammelfenster abgelaufen -> Zaehler + Richtung zuruecksetzen
    if (now > m_st.window_end) {
        m_st.reversals = 0;
        m_st.last_dir = 0;
    }

    if (m_st.last_p.has_value()) {
        const float dy = wp.y - m_st.last_p->y;
        const float dx = wp.x - m_st.last_p->x;
        const float dz = wp.z - m_st.last_p->z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        const float vy = dy / static_cast<float>(dt);

        // nur vertikal-dominante, schnelle Halbschwuenge zaehlen
        if (std::abs(dy) > m_shake.vert_dom * horiz && std::abs(vy) >= m_shake.speed) {
            const int32_t dir = vy > 0.0f ? 1 : -1;

            if (m_st.last_dir != 0 && dir != m_st.last_dir) {
                ++m_st.reversals;
            }

            m_st.last_dir = dir;
            m_st.window_end = now + static_cast<double>(m_shake.window);

            if (m_st.reversals >= m_shake.reversals) {
                m_st.fire_until = now + static_cast<double>(m_shake.fire);
                m_st.cooldown_end = now + static_cast<double>(m_shake.cooldown);
                m_st.reversals = 0;
                m_st.last_dir = 0;
            }
        }
    }

    m_st.last_p = wp;
}

// ----------------------------------------------------------------------------
// [PARRY] chenstacks auto_parry-Muster, aber NUR in der Schutz-Pose.
// Hook auf chainsaw.PlayerHeadActionSign.updateParryRequest().
// ----------------------------------------------------------------------------
void RE4VRWeapons2::parry_hook_install() {
    auto* td = sdk::find_type_definition("chainsaw.PlayerHeadActionSign");
    auto* m = td != nullptr ? td->get_method("updateParryRequest()") : nullptr;

    if (m == nullptr) {
        return;
    }

    g_hookman.add(
        m,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            try {
                // [TIMING] nur wenn die Pose FRISCH eingenommen wurde.
                const auto u = re4vr::lua_get_number_opt("__re4_knife_parry_fresh_until");

                if (!u.has_value() || clock_now() >= *u) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                if (args.size() < 2) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                auto* inst = reinterpret_cast<::REManagedObject*>(args[1]);

                if (!re4vr::obj_ok(inst)) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                auto* info = re4vr::call_safe<::REManagedObject*>(inst, "get_ParryInfo");

                if (!re4vr::obj_ok(info)) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                if (re4vr::call_safe<bool>(info, "get_IsEnable") != true) {
                    return HookManager::PreHookResult::CALL_ORIGINAL;
                }

                re4vr::call_safe<void*>(info, "set_RequestAction", true);
                re4vr::call_safe<void*>(info, "set_RequestReserve", true);

                // [PARRY-RESTORE 2026-07-20] Der native Parry equippt das ECHTE
                // Messer -- also RECHTS. War es vorher als Klon LINKS, kill die
                // "beide Haende"-Regel den Klon. Stattdessen denselben Latch
                // setzen wie der Finisher.
                if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
                    const double t = clock_now();

                    re4vr::lua_set_bool("__re4_clone_finisher_restore", true);
                    re4vr::lua_set_number("__re4_clone_finisher_restore_t", t);

                    // [PARRY-SOUND LINKS 16.09.2026] Einmal pro Parry vormerken.
                    if (t - g_lh_parry_sound_t >= LH_PARRY_SOUND_GAP) {
                        g_lh_parry_sound_t = t;
                        g_lh_parry_sound_pending = true;
                    }

                    // [PARRY_KEEP_GUN 2026-07-31] NUR im Links-Klon-Fall.
                    // Zeitbasiert -> laeuft von SELBST ab.
                    re4vr::lua_set_number("__re4_parry_keep_gun_until", t + 2.0);

                    // Fruehester Rueckhol-Zeitpunkt: die Parry-Anim braucht
                    // einen Moment, ein sofortiges Equip wuerde sie abwuergen.
                    re4vr::lua_set_number("__re4_parry_keep_gun_from", t + 0.30);

                    // [PARRY_KEEP_GUN] Die Waffen-ID wird hier NICHT gelesen --
                    // in Lua stand diese Stelle VOR get_ctx/sc/safe, ein
                    // Zugriff starb im pcall LAUTLOS. Die zuletzt gehaltene
                    // Waffe schreibt der on_frame-Block am Dateiende
                    // (__re4_parry_last_gun_wid). Verhalten bleibt so.
                }
            } catch (...) {
            }

            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
}

// ============================================================================
// Block 2 -- Messer LINKE Hand (Lua Z.381-1644)
// ============================================================================

namespace {

constexpr const char* LCFG_PATH = "re4_vr/re4_vr_knife_lefthand.json";
constexpr const char* LCFG_PATH_ADA = "re4_vr/re4_vr_knife_lefthand_ada.json";
constexpr const char* LH_OFF_PATH = "re4_vr/re4_vr_knife_lh_offsets.json";
constexpr const char* LH_OFF_PATH_ADA = "re4_vr/re4_vr_knife_lh_offsets_ada.json";
constexpr const char* LH_FLIP_PATH = "re4_vr/re4_vr_knife_lh_flip_offsets.json";
constexpr const char* LH_FLIP_PATH_ADA = "re4_vr/re4_vr_knife_lh_flip_offsets_ada.json";

// ---------------------------------------------------------------------------
// Hand-Pose (knifepose, gespiegelt R->L)
//
// Rechte Capture (gestures "knifepose", 16 Bones, [W,X,Y,Z], R_Palm=Identity).
// Spiegelung R->L per MIRROR (sagittal, Z-normale Ebene):
//   L = (w, x, -y, -z)  -> erhaelt die Flexion und spiegelt Splay/Daumen.
// ---------------------------------------------------------------------------
struct PoseBone {
    const char* name;   // bereits der L_-Name
    float w, x, y, z;   // bereits GESPIEGELT
};

// MIRROR = { w = 1, x = 1, y = -1, z = -1 }
constexpr std::array<PoseBone, 16> KNIFE_POSE_L{{
    {"L_IndexF1", 0.9353885650634766f, 0.049021635204553604f, 0.018328439444303513f,
     -0.3497273623943329f},
    {"L_IndexF2", 0.7740004062652588f, 0.0f, 0.0f, -0.633185088634491f},
    {"L_IndexF3", 0.9165631532669067f, 0.0f, 0.0f, -0.3998900055885315f},
    {"L_MiddleF1", 0.8989609479904175f, 0.005545048974454403f, -0.013179749250411987f,
     -0.43779537081718445f},
    {"L_MiddleF2", 0.7572856545448303f, 0.0f, 0.0f, -0.6530838012695313f},
    {"L_MiddleF3", 0.9105826020240784f, 0.0f, 0.0f, -0.41332709789276123f},
    {"L_Palm", 1.0f, 0.0f, 0.0f, 0.0f},
    {"L_PinkyF1", 0.7959001064300537f, -0.05373960733413696f, 0.027164660394191742f,
     -0.6024260520935059f},
    {"L_PinkyF2", 0.8307902216911316f, 0.0f, 0.0f, -0.5565857291221619f},
    {"L_PinkyF3", 0.9379373788833618f, 0.0f, 0.0f, -0.346804678440094f},
    {"L_RingF1", 0.8344995379447937f, -0.018377188593149185f, 0.027703266590833664f,
     -0.5500048398971558f},
    {"L_RingF2", 0.813831627368927f, 0.0f, 0.0f, -0.5811007618904114f},
    {"L_RingF3", 0.9315847158432007f, 0.0f, 0.0f, -0.36352434754371643f},
    {"L_Thumb1", 0.9212102890014648f, 0.38289788365364075f, 0.0038296207785606384f,
     -0.06889265775680542f},
    {"L_Thumb2", 0.9909763932228088f, 0.0f, -0.13403624296188354f, 0.0f},
    {"L_Thumb3", 0.9985861778259277f, 0.0f, 0.05315697565674782f, 0.0f},
}};

}   // namespace

const char* RE4VRWeapons2::lcfg_path() const {
    return m_lh_char == "ada" ? LCFG_PATH_ADA : LCFG_PATH;
}

const char* RE4VRWeapons2::lh_path() const {
    return m_lh_char == "ada" ? LH_OFF_PATH_ADA : LH_OFF_PATH;
}

const char* RE4VRWeapons2::lh_flip_path() const {
    return m_lh_char == "ada" ? LH_FLIP_PATH_ADA : LH_FLIP_PATH;
}

// [HARD-GUARD] Zweite, unabhaengige Sicherung: waehrend Ada gesteuert wird,
// kann Leons Datei physisch nicht geschrieben werden.
bool RE4VRWeapons2::lh_write_allowed(const char* path) const {
    const std::string p{path};

    if (m_lh_char == "ada" && (p == LH_OFF_PATH || p == LH_FLIP_PATH)) {
        return false;
    }

    if (m_lh_char == "leon" && (p == LH_OFF_PATH_ADA || p == LH_FLIP_PATH_ADA)) {
        return false;
    }

    // [UNBEKANNT = NICHT SCHREIBEN 2026-07-19] Die Luecke, durch die Adas
    // Greif-Werte in LEONS Datei gelandet sind: setzt die Charakter-Erkennung
    // aus, ist m_lh_char leer, die Pfad-Wahl faellt auf Leons Datei und beide
    // Guards oben greifen NICHT (sie pruefen nur "ada" bzw. "leon").
    if (m_lh_char != "ada" && m_lh_char != "leon") {
        return false;
    }

    return true;
}

bool RE4VRWeapons2::lcfg_load(const char* path) {
    const auto d = re4vr::json_load(path);

    if (!d.is_object()) {
        return false;
    }

    const auto n = [&](const char* key, float& dst) {
        if (const auto it = d.find(key); it != d.end() && it->is_number()) {
            dst = it->get<float>();
        }
    };

    n("grab_trigger", m_lcfg.trigger);
    n("grab_release", m_lcfg.release);
    n("flip_tap", m_lcfg.tap_sec);
    n("flip_speed", m_lcfg.flip_speed);
    n("swing_speed", m_lcfg.swing_speed);

    if (const auto it = d.find("blood_on"); it != d.end() && it->is_boolean()) {
        m_lcfg.blood_on = it->get<bool>();
    }

    return true;
}

void RE4VRWeapons2::lcfg_save() {
    re4vr::lua_set_number("__re4_knife_lt_flip_tap", m_lcfg.tap_sec);
    re4vr::lua_set_number("__re4_knife_lh_flip_speed", m_lcfg.flip_speed);
    re4vr::lua_set_number("__re4_knife_lh_swing_speed", m_lcfg.swing_speed);
    re4vr::lua_set_bool("__re4_knife_blood_on", m_lcfg.blood_on);

    const char* p = lcfg_path();

    // [HARD-GUARD] als Ada NIE in Leons Datei schreiben und umgekehrt
    if (m_lh_char == "ada" && std::string{p} == LCFG_PATH) {
        return;
    }

    if (m_lh_char == "leon" && std::string{p} == LCFG_PATH_ADA) {
        return;
    }

    // [UNBEKANNT = NICHT SCHREIBEN] s. lh_write_allowed.
    if (m_lh_char != "ada" && m_lh_char != "leon") {
        return;
    }

    nlohmann::json d{};
    d["grab_trigger"] = m_lcfg.trigger;
    d["grab_release"] = m_lcfg.release;
    d["flip_tap"] = m_lcfg.tap_sec;
    d["flip_speed"] = m_lcfg.flip_speed;
    d["swing_speed"] = m_lcfg.swing_speed;
    d["blood_on"] = m_lcfg.blood_on;

    re4vr::json_save(p, d);
}

void RE4VRWeapons2::lh_off_save() {
    const char* p = lh_path();

    if (!lh_write_allowed(p)) {
        return;
    }

    nlohmann::json out{};

    for (const auto& [wid, v] : m_lh_off) {
        auto& e = out[std::to_string(wid)];
        e["px"] = v.px;
        e["py"] = v.py;
        e["pz"] = v.pz;
        e["rx"] = v.rx;
        e["ry"] = v.ry;
        e["rz"] = v.rz;
    }

    re4vr::json_save(p, out);
    lh_publish();   // [WAISE 05.09.2026] UI-Aenderung sofort an motion weiter
}

// [WAISE 05.09.2026] Der Header versprach "sie bleiben exportiert, weil die UI
// und motion.lua sie lesen" -- der Export fehlte aber. RE4VRMotion liest beide
// Tabellen weiterhin aus dem Lua-State (lua_get_lh_off in RE4VR.cpp:1535 und
// lua_get_xyz_at auf __re4_knife_flip_lh_map, RE4VRMotion.cpp:2330). Ohne
// Export lag das Messer links in der reinen Spiegelung: jeder per-Messer
// getunte Links-Offset und der Links-Flip-Griffoffset waren wirkungslos
// (links hat bewusst KEINEN globalen Fallback).
void RE4VRWeapons2::lh_publish() {
    re4vr::LuaRef lua{};

    if (lua == nullptr) {
        return;
    }

    sol::table off = lua->create_table();

    for (const auto& [wid, v] : m_lh_off) {
        sol::table e = lua->create_table();
        e["px"] = v.px;
        e["py"] = v.py;
        e["pz"] = v.pz;
        e["rx"] = v.rx;
        e["ry"] = v.ry;
        e["rz"] = v.rz;
        off[wid] = e;
    }

    (*lua)["__re4_knife_lh_off_map"] = off;

    sol::table flip = lua->create_table();

    for (const auto& [wid, v] : m_lh_flip_off) {
        sol::table e = lua->create_table();
        e["x"] = v.x;
        e["y"] = v.y;
        e["z"] = v.z;
        flip[wid] = e;
    }

    (*lua)["__re4_knife_flip_lh_map"] = flip;
}

void RE4VRWeapons2::lh_flip_save() {
    const char* p = lh_flip_path();

    if (!lh_write_allowed(p)) {
        return;
    }

    nlohmann::json out{};

    for (const auto& [wid, v] : m_lh_flip_off) {
        auto& e = out[std::to_string(wid)];
        e["x"] = v.x;
        e["y"] = v.y;
        e["z"] = v.z;
    }

    re4vr::json_save(p, out);
    lh_publish();   // [WAISE 05.09.2026] s.o.
}

// [KNIFE_ADA] Charakter-Flanke: beim Wechsel BEIDE Maps aus den Dateien des
// neuen Charakters neu einlesen. Fehlt Adas Datei, wird sie einmalig aus dem
// aktuellen (Leon-)Stand geschrieben. Nur beim WECHSEL, nicht jeden Frame.
void RE4VRWeapons2::lh_char_tick() {
    // __re4_char_now: "ada" | "leon" | nil = UNBEKANNT. nil ist entscheidend --
    // bei unbekanntem Body darf NICHT auf Leon zurueckgefallen werden.
    const std::string want = re4vr::lua_call_global_string("__re4_char_now");

    if (want.empty()) {
        return;   // unbekannt -> NICHT umschalten
    }

    if (want == m_lh_char) {
        return;
    }

    const bool first_ada = want == "ada";

    m_lh_char = want;

    if (first_ada) {
        // Seed schreiben, BEVOR geladen wird (sonst laedt man ins Leere und
        // verliert Leons Stand).
        if (!re4vr::json_load(LH_OFF_PATH_ADA).is_object()) {
            lh_off_save();
        }

        if (!re4vr::json_load(LH_FLIP_PATH_ADA).is_object()) {
            lh_flip_save();
        }
    }

    // lh_reload_map(lh_path(), __re4_knife_lh_off_map)
    if (const auto d = re4vr::json_load(lh_path()); d.is_object()) {
        m_lh_off.clear();

        for (const auto& [k, v] : d.items()) {
            if (!v.is_object()) {
                continue;
            }

            const int32_t wid = std::atoi(k.c_str());
            LhOff o{};

            const auto g = [&](const char* key) -> float {
                const auto it = v.find(key);

                return (it != v.end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            o.px = g("px");
            o.py = g("py");
            o.pz = g("pz");
            o.rx = g("rx");
            o.ry = g("ry");
            o.rz = g("rz");

            m_lh_off[wid] = o;
        }
    }

    if (const auto d = re4vr::json_load(lh_flip_path()); d.is_object()) {
        m_lh_flip_off.clear();

        for (const auto& [k, v] : d.items()) {
            if (!v.is_object()) {
                continue;
            }

            const int32_t wid = std::atoi(k.c_str());
            glm::vec3 o{};

            const auto g = [&](const char* key) -> float {
                const auto it = v.find(key);

                return (it != v.end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            o.x = g("x");
            o.y = g("y");
            o.z = g("z");

            m_lh_flip_off[wid] = o;
        }
    }

    // [WAISE 05.09.2026] beide Tabellen fuer motion nach Lua spiegeln.
    lh_publish();

    // Greif-/Flip-Werte der linken Hand mitziehen; Adas Datei beim ersten Mal
    // aus Leons Stand seeden.
    if (first_ada && !re4vr::json_load(LCFG_PATH_ADA).is_object()) {
        lcfg_save();
    }

    lcfg_load(lcfg_path());

    // [BLUT_SCHALTER] gilt pro Charakter -> beim Wechsel mitziehen.
    re4vr::lua_set_bool("__re4_knife_blood_on", m_lcfg.blood_on);
}

// ----------------------------------------------------------------------------
// __re4_apply_left_knife_pose -- von motion und merc gerufen.
// ----------------------------------------------------------------------------
void RE4VRWeapons2::apply_left_knife_pose() {
    // [LH_CLONE] Im Klon-Modus ist __re4_knife_hand "none" -> Pose zusaetzlich
    // am Klon-Flag anwenden.
    if (re4vr::lua_get_string("__re4_knife_hand") != "left"
        && re4vr::lua_get_tribool("__re4_knife_left_clone") != 1) {
        return;
    }

    // Geschrieben wird ueber den Joint-Writer aus reload.lua -- der ist NOCH
    // LUA, also fuehrt der Weg zwingend ueber den Lua-State.
    {
        std::unordered_map<std::string, glm::quat> bones{};
        bones.reserve(KNIFE_POSE_L.size());

        for (const auto& b : KNIFE_POSE_L) {
            bones.emplace(b.name, glm::quat{b.w, b.x, b.y, b.z});
        }

        re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones", bones, 1.0f);
    }

    // [FLIP FINGER] waehrend des Flips die Finger kurz oeffnen (Peak bei halbem
    // Flip). motion.knife_flip_finger_open ist auf __re4_knife_equipped gated
    // und greift beim Klon nicht.
    const float lp = static_cast<float>(re4vr::lua_get_number("__re4_knife_lh_flip_lerp", 0.0));
    const float bump = 4.0f * lp * (1.0f - lp);

    if (bump <= 0.001f) {
        return;
    }

    const float deg =
        static_cast<float>(re4vr::lua_get_number("__re4_knife_flip_finger_deg", -35.0));

    // links negiert (Spiegelung der rechten Strecke)
    const float h = -(glm::radians(deg * bump) * 0.5f);
    const glm::quat add{std::cos(h), std::sin(h), 0.0f, 0.0f};

    // Lua nimmt hier get_ctx() -> get_BodyGameObject -> get_Transform und
    // haengt am Frame-Cache (require "re4vr/re4_vr_frame_cache").
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return;
    }

    for (const char* bn : {"L_IndexF1", "L_MiddleF1", "L_RingF1", "L_PinkyF1"}) {
        auto* j = joint_by_name(tf, bn);

        if (!re4vr::obj_ok(j)) {
            continue;
        }

        glm::quat cur{};

        if (!get_quat(j, "get_LocalRotation", cur)) {
            continue;
        }

        set_quat(j, "set_LocalRotation", glm::normalize(cur * add));
    }
}

// ----------------------------------------------------------------------------
// Player / PlayerEquipment (fuer das deferred Equip)
// ----------------------------------------------------------------------------

namespace {

::REManagedObject* get_ctx_w2() {
    return re4vr::fc::on() ? re4vr::fc::ctx() : re4vr::player_context();
}

::REManagedObject* get_pe_w2() {
    if (re4vr::fc::on()) {
        return re4vr::fc::pe();
    }

    auto* head = re4vr::head_game_object();

    return head != nullptr ? re4vr::get_component(head, "chainsaw.PlayerEquipment") : nullptr;
}

std::optional<int32_t> get_equip_wid_w2() {
    if (re4vr::fc::on()) {
        return re4vr::fc::equip_wid();
    }

    auto* ctx = get_ctx_w2();
    auto* hu = ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                              : nullptr;

    if (!re4vr::obj_ok(hu)) {
        return std::nullopt;
    }

    // WeaponID ist ein value-type Enum -> IMMER als Zahl lesen, nie als Zeiger.
    // Lua: `if type(wid)=="number" then ... end` und sonst `wid.value__`.
    return enum_as_int(hu, "get_EquipWeaponID");
}

// Lua: set_ParentJoint(name) -- braucht einen managed String.
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

}   // namespace

// [LH_CLONE RELOAD-KILL] Genereller Reload-Detektor: steht irgendein
// Motion-FSM-Layer der Body-GO auf einer "RELOAD"-Node, laeuft eine (native)
// Nachlade-Anim -- egal welche Waffe. Technik 1:1 aus
// motion.native_reload_active, aber wid-unabhaengig.
bool RE4VRWeapons2::player_is_reloading() {
    auto* ctx = get_ctx_w2();

    if (ctx == nullptr) {
        return false;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (!re4vr::obj_ok(go)) {
        return false;
    }

    if (m_mfsm_go.obj != go) {
        store(m_mfsm_go, go);
        drop(m_mfsm_comp);
    }

    if (m_mfsm_comp.obj == nullptr) {
        auto* c = re4vr::get_component(go, "via.motion.MotionFsm2");

        if (c != nullptr) {
            store(m_mfsm_comp, c);
        }
    }

    auto* m = m_mfsm_comp.obj;

    if (m == nullptr) {
        return false;
    }

    for (int32_t layer = 0; layer <= 6; ++layer) {
        auto* n = re4vr::call_safe<::REManagedObject*>(m, "getCurrentNodeName", layer);

        if (n == nullptr) {
            continue;
        }

        std::string s{};

        try {
            s = utility::re_string::get_string(reinterpret_cast<::SystemString*>(n));
        } catch (...) {
            s.clear();
        }

        if (s.find("RELOAD") != std::string::npos) {
            return true;
        }
    }

    return false;
}

// ---- deferred Equip/Stow (im Holster-updateOnFrameHead-Hook ausgefuehrt) ----
//
// __re4_knife_defer und __re4_knife_set_suppress kommen aus dem Holster, der
// heute NATIV ist -> direkt rufen statt ueber Lua.
//
// [TOTER CODE, 1:1 uebernommen] Beide Funktionen werden in der Lua NUR
// DEFINIERT und NIRGENDS gerufen (per Grep ueber die ganze Datei belegt).
// Sie stammen aus SCHRITT 1 vom 07.07., als der Links-Zug noch ein echtes
// requestEquipKnife war; der LH_CLONE-Umbau vom 08.07. hat das durch den
// Mesh-Klon ersetzt und die Aufrufe entfernt, die Definitionen aber stehen
// lassen. Sie wandern unveraendert mit -- wer sie aktiviert, aendert
// getestetes Verhalten.
bool RE4VRWeapons2::defer_draw_left() {
    auto& hol = RE4VRHolster::get();

    if (hol == nullptr) {
        return false;
    }

    hol->set_suppress(false);

    hol->defer([]() {
        auto* pe = get_pe_w2();

        if (pe == nullptr) {
            return;
        }

        // [KNIFE-GATE 2026-07-20] eigener Links-Zug -> Hook laesst ihn durch.
        re4vr::lua_set_number("__re4_knife_draw_ours_t", clock_now());

        re4vr::call_safe<void*>(pe, "clearRequest");
        re4vr::call_safe<void*>(pe, "requestEquipKnife");
        re4vr::call_safe<void*>(pe, "execChangeWeapon");
    });

    return true;
}

bool RE4VRWeapons2::defer_stow_left() {
    auto& hol = RE4VRHolster::get();

    if (hol == nullptr) {
        return false;
    }

    hol->set_suppress(true);

    // binding: Aim-Auto-Draw kurz sperren (kein Flackern)
    re4vr::lua_set_number("__vr_post_stow_until", clock_now() + 0.6);

    hol->defer([]() {
        auto* pe = get_pe_w2();

        if (pe == nullptr) {
            return;
        }

        re4vr::call_safe<void*>(pe, "clearRequest");
        re4vr::call_safe<void*>(pe, "requestEquipBareHand", false, false);
        re4vr::call_safe<void*>(pe, "execChangeWeapon");
    });

    return true;
}

// ----------------------------------------------------------------------------
// Das AKTUELL AUSGEWAEHLTE Messer
// ----------------------------------------------------------------------------

// [CURRENT KNIFE -- POLL] Der reine Inventar-Wechsel im Menue feuert
// equipWeapon NICHT -> zusaetzlich den Inventory pollen.
std::optional<int32_t> RE4VRWeapons2::poll_inventory_knife_wid() {
    auto* pe = get_pe_w2();

    if (pe == nullptr) {
        return std::nullopt;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (!re4vr::obj_ok(inv)) {
        return std::nullopt;
    }

    for (int32_t et = 1; et <= 3; ++et) {
        auto* wi = re4vr::call_safe<::REManagedObject*>(
            inv, "getEquippedWeapon(chainsaw.EquipType)", et);

        if (!re4vr::obj_ok(wi)) {
            continue;
        }

        const auto v = enum_as_int(wi, "get_WeaponId");

        if (v.has_value() && is_knife_id(*v)) {
            return v;
        }
    }

    return std::nullopt;
}

std::optional<int32_t> RE4VRWeapons2::get_selected_knife_wid() {
    // BEIDE WEGE: 1) Inventory-Poll ist AUTORITATIV und haelt den Hook-Cache
    // aktuell. 2) Hook-Cache. 3) Mount-Liste als letzter Fallback.
    if (const auto pv = poll_inventory_knife_wid(); pv.has_value()) {
        re4vr::lua_set_number("__re4_current_knife_wid", static_cast<double>(*pv));

        return pv;
    }

    if (const auto cur = re4vr::lua_get_number_opt("__re4_current_knife_wid"); cur.has_value()) {
        const auto v = static_cast<int32_t>(*cur);

        if (is_knife_id(v)) {
            return v;
        }
    }

    auto* ctx = get_ctx_w2();
    auto* arr = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_MountWeaponIDs")
        : nullptr;

    if (!re4vr::obj_ok(arr)) {
        return std::nullopt;
    }

    // [ARRAY-BINDING] Lua schreibt `arr:call("get_Length")` und dann `arr[i]`.
    // `arr[i]` ist ein REFramework-BINDING, KEIN managed Call -- als get_Item
    // portiert liefert es stumm nichts.
    // Siehe [[reference_re4_cpp_array_bindings_sind_kein_call]].
    // get_MountWeaponIDs ist ein System.Array von Enum-Werten -> ueber
    // re4vr::array_size + array_element lesen.
    const int32_t n = re4vr::array_size(arr);

    for (int32_t i = 0; i < n; ++i) {
        auto* e = re4vr::array_element(arr, i);

        if (e == nullptr) {
            continue;
        }

        // Lua liest den Wert als Zahl ODER ueber `.value__`.
        const auto v = re4vr::get_field_int(e, "value__");

        if (v.has_value() && is_knife_id(*v)) {
            return v;
        }
    }

    return std::nullopt;
}

// Das AUSGEWAEHLTE Messer -> dessen via.render.Mesh + wid.
::REManagedObject* RE4VRWeapons2::find_knife_mesh(std::optional<int32_t>& wid_out) {
    const auto want = get_selected_knife_wid();

    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        wid_out.reset();

        return nullptr;
    }

    ::REManagedObject* want_mesh = nullptr;
    std::optional<int32_t> want_id{};
    ::REManagedObject* any_mesh = nullptr;
    std::optional<int32_t> any_id{};

    // Lua: rekursiver Walk bis Tiefe 12, Geschwisterkette bis 400.
    const std::function<void(::REManagedObject*, int32_t)> walk =
        [&](::REManagedObject* t, int32_t depth) {
            if (t == nullptr || depth > 12 || want_mesh != nullptr) {
                return;
            }

            auto* child = re4vr::call_safe<::REManagedObject*>(t, "get_Child");
            int32_t guard = 0;

            while (child != nullptr && guard < 400) {
                ++guard;

                auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");
                const std::string nm = obj_name_of(go);

                // Lua: nm:find("^wp5") oder ^wp6107/^wp6108/^wp6305
                if (!nm.empty()
                    && (nm.rfind("wp5", 0) == 0 || nm.rfind("wp6107", 0) == 0
                        || nm.rfind("wp6108", 0) == 0 || nm.rfind("wp6305", 0) == 0)) {
                    // Lua: tonumber(nm:match("^wp(%d+)"))
                    size_t p = 2;
                    std::string digits{};

                    while (p < nm.size() && std::isdigit(static_cast<unsigned char>(nm[p]))) {
                        digits.push_back(nm[p]);
                        ++p;
                    }

                    if (!digits.empty()) {
                        const int32_t id = std::atoi(digits.c_str());

                        if (is_knife_id(id)) {
                            auto* mesh = re4vr::get_component(go, "via.render.Mesh");

                            if (mesh != nullptr) {
                                if (want.has_value() && id == *want) {
                                    want_mesh = mesh;
                                    want_id = id;

                                    return;
                                }

                                if (any_mesh == nullptr) {
                                    any_mesh = mesh;
                                    any_id = id;
                                }
                            }
                        }
                    }
                }

                walk(child, depth + 1);

                if (want_mesh != nullptr) {
                    return;
                }

                child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
            }
        };

    walk(tf, 0);

    if (want_mesh != nullptr) {
        wid_out = want_id;

        return want_mesh;
    }

    wid_out = any_id;

    return any_mesh;
}

// ----------------------------------------------------------------------------
// Klon-Lebenszyklus
// ----------------------------------------------------------------------------

// [STALE-CLEANUP] Alle verwaisten "vr_lh_knife"-Klone am Body entsorgen.
// Erst sammeln, DANN zerstoeren (Baum nicht waehrend der Iteration mutieren).
std::optional<int32_t> RE4VRWeapons2::destroy_orphan_clones(::REManagedObject* keep) {
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return std::nullopt;
    }

    std::vector<::REManagedObject*> victims{};

    const std::function<void(::REManagedObject*, int32_t)> walk =
        [&](::REManagedObject* t, int32_t depth) {
            if (t == nullptr || depth > 8) {
                return;
            }

            auto* child = re4vr::call_safe<::REManagedObject*>(t, "get_Child");
            int32_t guard = 0;

            while (child != nullptr && guard < 500) {
                ++guard;

                auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

                if (obj_name_of(go) == "vr_lh_knife" && go != keep) {
                    victims.push_back(go);
                }

                walk(child, depth + 1);

                child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
            }
        };

    walk(tf, 0);

    for (auto* go : victims) {
        re4vr::destroy_game_object(go);
    }

    return static_cast<int32_t>(victims.size());
}

void RE4VRWeapons2::clone_destroy() {
    if (m_clone.obj.obj != nullptr) {
        re4vr::destroy_game_object(m_clone.obj.obj);
    }

    clone_forget();
}

// [LH_CLONE RESET 20.09.2026] Nur die gemerkten Zeiger loslassen -- KEIN
// destroy_game_object. Nach Tod/Laden/Mercs-Levelstart ist das alte GO
// abgebaut, aber weiter "lesbar" ([[saveload_leichen]]): ein destroy darauf
// waere genau der Deref, den wir vermeiden wollen. Reste am Body raeumt
// destroy_orphan_clones ohnehin sekuendlich weg.
void RE4VRWeapons2::clone_forget() {
    drop(m_clone.obj);
    drop(m_clone.mesh);

    m_clone.wid.reset();
    m_clone.parented = false;
    m_clone.part0 = false;
    m_clone.was_flying = false;

    // [KEIN DOPPELMESSER] Sichtbarkeits-Cache fuer den naechsten Klon
    // zuruecksetzen.
    m_clone.vis.reset();

    re4vr::lua_set_nil("__re4_knife_hc_cache");
    re4vr::lua_set_nil("__re4_knife_lh_clone_go");
}

bool RE4VRWeapons2::clone_spawn() {
    std::optional<int32_t> wid{};
    auto* gmesh = find_knife_mesh(wid);

    if (gmesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(gmesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* gmat = re4vr::call_safe<::REManagedObject*>(gmesh, "get_Material");

    auto* go = reinterpret_cast<::REManagedObject*>(re4vr::create_game_object("vr_lh_knife"));

    if (go == nullptr) {
        return false;
    }

    // Lua Z.1073: go:add_ref() SOFORT. Selbst erzeugtes Objekt ->
    // BEDINGUNGSLOS pinnen, die refcount-Heuristik greift hier nicht.
    store(m_clone.obj, go, true);

    // KERN: Skelett (sonst unsichtbar)
    if (auto* motion_rt = re4vr::runtime_type("via.motion.Motion")) {
        re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", motion_rt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* mesh_rt = re4vr::runtime_type("via.render.Mesh")) {
        mesh = re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", mesh_rt);
    }

    if (mesh == nullptr) {
        // Lua verlaesst hier mit `return false` OHNE das GO zu zerstoeren -- in
        // Lua faengt das der GC auf, mit echtem Refcount waere es ein Leck pro
        // Frame. Deshalb hier aufraeumen; Verhalten identisch.
        re4vr::destroy_game_object(go);
        drop(m_clone.obj);

        return false;
    }

    store(m_clone.mesh, mesh);

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (gmat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", gmat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    re4vr::call_safe<void*>(mesh, "set_DrawShadowCast", false);

    auto* ctf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    auto* ctx = get_ctx_w2();
    auto* bgo = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
        : nullptr;

    bool go_valid = true;
    {
        bool v = false;

        if (re4vr::try_call<bool>(go, "get_Valid", v)) {
            go_valid = v;
        }
    }

    bool bgo_valid = bgo != nullptr;
    {
        bool v = false;

        if (bgo != nullptr && re4vr::try_call<bool>(bgo, "get_Valid", v)) {
            bgo_valid = v;
        }
    }

    auto* btf = (bgo != nullptr && bgo_valid)
        ? re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform")
        : nullptr;

    m_clone.parented = false;

    if (ctf != nullptr && btf != nullptr && go_valid && bgo_valid) {
        re4vr::call_safe<void*>(ctf, "set_Parent", btf);

        if (set_parent_joint(ctf, "L_Hand")) {
            m_clone.parented = true;
        }
    }

    m_clone.wid = wid;
    m_clone.part0 = false;

    // [WURF] weapons.lua knife_throw_launch fliegt DIESES GO im Klon-Modus.
    re4vr::lua_set_managed_object("__re4_knife_lh_clone_go", go);

    return true;
}

// Part 0 = ganzes Messer -> nur den anzeigen. Retry bis Mesh ready.
bool RE4VRWeapons2::clone_isolate_part0() {
    if (m_clone.mesh.obj == nullptr) {
        return false;
    }

    bool ready = false;

    if (!re4vr::try_call<bool>(m_clone.mesh.obj, "get_MeshReady", ready) || !ready) {
        return false;
    }

    const auto m = find_method(m_clone.mesh.obj, "setPartsEnable");

    if (m == nullptr) {
        return false;
    }

    for (int32_t i = 0; i < 64; ++i) {
        auto context = sdk::get_thread_context();

        try {
            m->call_safe<void*>(context, m_clone.mesh.obj, i, i == 0);
        } catch (...) {
        }

        clear_pending(context, true);
    }

    return true;
}

// ----------------------------------------------------------------------------
// [LH SOUND] Der Klon hat keinen eigenen SoundContainer -> Sounds ueber ein
// ECHTES Messer-GO am Body spielen.
// ----------------------------------------------------------------------------
::REManagedObject* RE4VRWeapons2::lh_body_knife_soundcontainer() {
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return nullptr;
    }

    // [SUFFIX 2026-08-26, gemessen] In Mercenaries heisst das Messer-GO live
    // "wp5000_MC". Deshalb ZWEI Durchgaenge: ERST der exakte Name (Verhalten
    // wie bisher), DANACH derselbe Name mit Suffix. Reihenfolge zaehlt: gibt es
    // beide GOs, gewinnt weiter plain.
    for (int32_t pass = 1; pass <= 2; ++pass) {
        auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
        int32_t guard = 0;

        while (child != nullptr && guard < 256) {
            ++guard;

            auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");
            const std::string nm = obj_name_of(go);

            if (!nm.empty() && nm.rfind("wp", 0) == 0) {
                size_t p = 2;
                std::string digits{};

                while (p < nm.size() && std::isdigit(static_cast<unsigned char>(nm[p]))) {
                    digits.push_back(nm[p]);
                    ++p;
                }

                // pass 1: "^wp(%d+)$"   -- nach den Ziffern ist Schluss
                // pass 2: "^wp(%d+)_%w+$" -- danach _ und mindestens ein Zeichen
                bool match = false;

                if (!digits.empty()) {
                    if (pass == 1) {
                        match = p == nm.size();
                    } else {
                        match = p < nm.size() && nm[p] == '_' && p + 1 < nm.size();
                    }
                }

                if (match) {
                    const int32_t wid = std::atoi(digits.c_str());

                    if (is_knife_id(wid)) {
                        auto* scn = re4vr::get_component(go, "soundlib.SoundContainer");

                        if (scn != nullptr) {
                            return scn;
                        }
                    }
                }
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }
    }

    return nullptr;
}

void RE4VRWeapons2::knife_lh_play_sound(int32_t id) {
    // [GROSSE IDs 2026-09-08] Die Wwise-IDs sind UInt32. Alles ab 2^31 (z.B. der
    // Wurf-Sound 3788596668) ist als int32_t NEGATIV -- mit dem alten `id <= 0`
    // fiel genau dieser Sound lautlos raus, waehrend swing/flip/hit (alle unter
    // 2^31) klangen. Unten wird ohnehin nach uint32_t zurueckgecastet, der
    // Bitwert stimmt also; nur die Pruefung war zu eng. 0 bleibt "kein Sound".
    if (id == 0) {
        return;
    }

    auto* scn = lh_body_knife_soundcontainer();

    // [TON-AUSFALL 2026-08-26] Findet der Body-Walk gerade kein Messer-GO (es
    // fliegt, es steckt im Gegner, es wird umgehaengt), entfiel der Ton bisher
    // ersatzlos. Deshalb: zuletzt benutzten Container merken, notfalls den des
    // Klon-GOs nehmen.
    if (scn == nullptr && m_clone.obj.obj != nullptr) {
        bool v = true;

        if (re4vr::try_call<bool>(m_clone.obj.obj, "get_Valid", v) && !v) {
            // ungueltig -> nichts
        } else {
            scn = re4vr::get_component(m_clone.obj.obj, "soundlib.SoundContainer");
        }
    }

    if (scn != nullptr) {
        store(m_lh_snd_last, scn);
    } else if (m_lh_snd_last.obj != nullptr) {
        bool v = true;

        if (!re4vr::try_call<bool>(m_lh_snd_last.obj, "get_Valid", v) || v) {
            scn = m_lh_snd_last.obj;
        }
    }

    if (scn != nullptr) {
        re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", static_cast<uint32_t>(id));
    }
}

void RE4VRWeapons2::clone_apply_pose() {
    if (m_clone.obj.obj == nullptr) {
        return;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(m_clone.obj.obj, "get_Transform");

    if (!re4vr::obj_ok(tf)) {
        return;
    }

    LhOff o{};
    bool have_o = false;

    if (m_clone.wid.has_value()) {
        if (const auto it = m_lh_off.find(*m_clone.wid); it != m_lh_off.end()) {
            o = it->second;
            have_o = true;
        }
    }

    float px = have_o ? o.px : 0.0f;
    float py = have_o ? o.py : 0.0f;
    float pz = have_o ? o.pz : 0.0f;

    // [FLIP] Lerp 0->1 wie motion (180 Grad um lokale X, post-multipliziert).
    const float target = re4vr::lua_get_tribool("__vr_knife_flip") == 1 ? 1.0f : 0.0f;

    if (m_flip_prev_target != target) {
        m_flip_prev_target = target;
        knife_lh_play_sound(1007228839);   // Flip-Sound (hin UND zurueck)
    }

    // clone_apply_pose laeuft nur 1x/Frame (rechts lerpt motion in ~5
    // Render-Paessen) -> hoehere Rate, sonst zu langsam.
    const float spd =
        static_cast<float>(re4vr::lua_get_number("__re4_knife_lh_flip_speed", 0.5));

    if (m_flip_lerp < target) {
        m_flip_lerp = std::min(target, m_flip_lerp + spd);
    } else if (m_flip_lerp > target) {
        m_flip_lerp = std::max(target, m_flip_lerp - spd);
    }

    // [FLIP FINGER] die Handpose-Funktion liest das.
    re4vr::lua_set_number("__re4_knife_lh_flip_lerp", m_flip_lerp);

    glm::quat rot = quat_from_euler_deg(have_o ? o.rx : 0.0f, have_o ? o.ry : 0.0f,
                                        have_o ? o.rz : 0.0f);

    if (m_flip_lerp > 0.0001f) {
        const glm::quat fq = quat_from_euler_deg(180.0f * m_flip_lerp, 0.0f, 0.0f);
        rot = glm::normalize(rot * fq);

        // Pro-Messer Flip-Offset, lerp-skaliert, im lokalen Frame.
        if (m_clone.wid.has_value()) {
            if (const auto it = m_lh_flip_off.find(*m_clone.wid); it != m_lh_flip_off.end()) {
                px += it->second.x * m_flip_lerp;
                py += it->second.y * m_flip_lerp;
                pz += it->second.z * m_flip_lerp;
            }
        }
    }

    set_vec3(tf, "set_LocalPosition", glm::vec3{px, py, pz});
    set_quat(tf, "set_LocalRotation", rot);
    set_vec3(tf, "set_LocalScale", glm::vec3{1.0f, 1.0f, 1.0f});
}

// Lifecycle: nur wenn Klon-Modus aktiv. In KS/KS4 weg. Save-Load-fest.
void RE4VRWeapons2::clone_manage() {
    // [STALE-CLEANUP] Selbstheilend & throttled.
    const double now = clock_now();

    if (now - m_orphan_check_t > 1.0) {
        m_orphan_check_t = now;
        destroy_orphan_clones(m_clone.obj.obj);
    }

    if (re4vr::lua_get_tribool("__re4_knife_left_clone") != 1) {
        if (m_clone.obj.obj != nullptr) {
            clone_destroy();
        }

        return;
    }

    if (re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
        || re4vr::lua_get_tribool("__re4_ks4_active") == 1) {
        if (m_clone.obj.obj != nullptr) {
            clone_destroy();
        }

        return;
    }

    // [GRAPPLE 2026-09-08] Niedergerungen schaltet das Spiel auf 3rd Person und
    // spielt seine eigene Wehr-Animation -- dort gehoert das Messer in die
    // RECHTE Hand des Charakters. Unser Klon haengt aber an L_Hand und blieb
    // sichtbar links stehen, waehrend die Kamera den ganzen Koerper zeigt.
    // Deshalb fuer die Dauer des Grapples denselben Weg wie beim Killswitch:
    // Klon weg, die Engine macht ihr Ding. clone_manage laeuft jeden Frame --
    // ist der Grapple vorbei, baut clone_spawn den Klon von selbst wieder auf
    // und das Messer liegt wieder in der Hand des Spielers.
    {
        bool grappled = false;

        if (re4vr::try_call<bool>(re4vr::fc::ctx(), "get_IsInGrappleDamage", grappled)
            && grappled) {
            if (m_clone.obj.obj != nullptr) {
                clone_destroy();
            }

            return;
        }
    }

    if (m_clone.obj.obj != nullptr) {
        bool v = false;

        if (!re4vr::try_call<bool>(m_clone.obj.obj, "get_Valid", v) || !v) {
            clone_destroy();
        }
    }

    // Messer im Menue gewechselt -> Klon neu bauen.
    if (m_clone.obj.obj != nullptr && m_clone.wid.has_value()) {
        const auto sel = get_selected_knife_wid();

        if (sel.has_value() && *sel != *m_clone.wid) {
            clone_destroy();
        }
    }

    if (m_clone.obj.obj == nullptr) {
        clone_spawn();
    }

    if (m_clone.obj.obj == nullptr) {
        return;
    }

    if (!m_clone.part0) {
        if (clone_isolate_part0()) {
            m_clone.part0 = true;
        }
    }

    // [KEIN DOPPELMESSER 2026-07-20] Waehrend der Restore-Phase existiert der
    // Klon links WEITER -- sichtbar waeren dann zwei Messer. Deshalb: solange
    // engine-seitig ein Messer equippt ist, den Klon nur UNSICHTBAR schalten.
    if (m_clone.mesh.obj != nullptr) {
        const bool want = re4vr::lua_get_tribool("__re4_knife_equipped") != 1;

        if (!m_clone.vis.has_value() || *m_clone.vis != want) {
            m_clone.vis = want;
            re4vr::call_safe<void*>(m_clone.mesh.obj, "set_DrawDefault", want);
        }
    }

    // [WURF] waehrend das Klon-Messer fliegt NICHT pinnen. Nach dem Flug den
    // Klon zurueck an L_Hand parenten (der Wurf hatte ihn geloest).
    if (re4vr::lua_get_tribool("__re4_knife_flying") == 1) {
        m_clone.was_flying = true;

        return;
    }

    if (m_clone.was_flying) {
        m_clone.was_flying = false;

        auto* ctx = get_ctx_w2();
        auto* bgo = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;
        auto* btf = bgo != nullptr
            ? re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform")
            : nullptr;
        auto* ctf = re4vr::call_safe<::REManagedObject*>(m_clone.obj.obj, "get_Transform");

        if (ctf != nullptr && btf != nullptr) {
            re4vr::call_safe<void*>(ctf, "set_Parent", btf);
            set_parent_joint(ctf, "L_Hand");
        }
    }

    clone_apply_pose();
}

// [LH_CLONE MELEE] requestAttack am nicht-equippten Messer macht KEINEN
// Schaden. Pivot: NATIVES execMelee.
void RE4VRWeapons2::exec_native_melee() {
    auto* pe = get_pe_w2();

    if (pe == nullptr) {
        return;
    }

    if (!m_melee_combat_looked_up) {
        m_melee_combat_looked_up = true;

        auto* td = sdk::find_type_definition("chainsaw.PlayerDefine.MeleeAttackType");
        auto* f = td != nullptr ? td->get_field("Combat") : nullptr;

        if (f != nullptr) {
            try {
                // Statisches Enum-Feld -> get_data mit container=false
                // (Managed-Object-Weg).
                // Siehe [[reference_re4_cpp_get_data_raw_container_flag]].
                m_melee_combat = f->get_data<int32_t>(nullptr, false);
            } catch (...) {
                m_melee_combat.reset();
            }
        }
    }

    // [ORIGINAL-FEHLER, 1:1] Lua: `_melee_combat = (f and f:get_data(nil)) or false`
    // -- ist der Enum-Wert 0, ist das in Lua FALSE und die Funktion steigt aus.
    // Der native Melee feuert dann NIE. Wandert unveraendert mit.
    if (!m_melee_combat.has_value() || *m_melee_combat == 0) {
        return;
    }

    re4vr::call_safe<void*>(
        pe, "execMelee(chainsaw.PlayerDefine.MeleeAttackType, System.UInt32)",
        *m_melee_combat, static_cast<uint32_t>(0));
}

// ----------------------------------------------------------------------------
// Haupt-Tick der linken Hand (Lua Z.1292-1551)
// ----------------------------------------------------------------------------

namespace {

constexpr float SWING_CD = 0.30f;

// Lua: left_grip_pressed()
bool left_grip_pressed() {
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

// Grab-Haptik: EXAKT derselbe kurze Puls wie rechts, nur LINKS.
void left_grab_haptic() {
    auto& vr = VR::get();

    if (vr == nullptr) {
        return;
    }

    // [PORTFIX 2026-09-06 OPENXR] Keine Handle-Nullpruefung -- unter OpenXR
    // ist get_left_joystick() genau 0 und der Guard haette die linke
    // Grab-Haptik stumm gestellt. Vorlage: RE4VRMinecart.cpp:1281.
    const auto lj = vr->get_left_joystick();

    try {
        vr->trigger_haptic_vibration(0.0f, 0.06f, 200.0f, 0.9f, lj);
    } catch (...) {
    }
}

// Grab-Sound: derselbe wie rechts (Holster-Export, hand-neutral).
void left_grab_sound() {
    auto& hol = RE4VRHolster::get();

    if (hol != nullptr) {
        hol->play_knife_grab_sound();
    }
}

}   // namespace

void RE4VRWeapons2::lh_tick() {
    const double now = clock_now();

    // [KNIFE_HAND] left_intent PERSISTIERT bewusst. Es wird NUR durch einen
    // expliziten Links-Stow ODER einen Rechts-Draw (holster on_grab) auf false
    // gesetzt.

    // Killswitch/KS4: keine Grabs (Grip-Flanke frisch halten).
    if (re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
        || re4vr::lua_get_tribool("__re4_ks4_active") == 1) {
        if (m_clone.obj.obj != nullptr) {
            clone_destroy();
        }

        m_prev_lgrip = left_grip_pressed();
        m_armed = false;

        return;
    }

    // [EINE ZONE 2026-07-19] Distanz NICHT selbst rechnen: holster berechnet sie
    // mit demselben Anker, derselben Formel und demselben Radius wie rechts.
    std::optional<float> d{};

    if (const auto v = re4vr::lua_get_number_opt("__re4_knife_lh_dist"); v.has_value()) {
        d = static_cast<float>(*v);
    } else {
        // FALLBACK: mit der CONTROLLER-Position (__vr_lh_world), NICHT dem
        // L_Hand-Joint -- der kommt durch Arm-IK/Clamp nicht an den Brustpunkt.
        const auto anchor = re4vr::lua_get_vec3("__vr_knife_chest_pos");
        const auto lh = re4vr::lua_get_vec3_any({"__vr_lh_world", "__vr_lh_joint_pos"});

        if (anchor.has_value() && lh.has_value()) {
            d = glm::length(*lh - *anchor);
        }
    }

    m_last_d = d;

    // [EIN RADIUS 2026-07-19] Greif-/Release-Radius kommt vom MESSER-HOLSTER.
    const float r_rel =
        static_cast<float>(re4vr::lua_get_number("__re4_knife_grab_release", m_lcfg.release));

    // [KNIFE_HAND] Links-Holster-Zone fuer weapons.lua. Flag kommt DIREKT aus
    // holster (gleiche Hysterese wie rechts); Fallback nur, wenn nichts kommt.
    int in_zone_tri = re4vr::lua_get_tribool("__re4_knife_lh_in_zone");
    bool in_zone;

    if (in_zone_tri == -1) {
        in_zone = d.has_value() && *d <= r_rel;
    } else {
        in_zone = in_zone_tri == 1;
    }

    re4vr::lua_set_bool("__vr_knife_lh_holster_zone", in_zone);

    // Modell wie rechts: Presse in Trigger-Zone armt, Loslassen in Release-Zone
    // feuert (Hysterese).
    const bool lgrip = left_grip_pressed();

    if (lgrip && !m_prev_lgrip) {
        m_armed = in_zone;
    } else if (m_prev_lgrip && !lgrip) {
        if (m_armed && in_zone) {
            // [LH_CLONE] Links-Zug holt/staut einen MESH-KLON (kein
            // Engine-Equip). Die Gun bleibt der echte Main-Equip rechts.
            const bool in_clone = re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
            const bool equipped = re4vr::lua_get_tribool("__re4_knife_equipped") == 1;

            if (in_clone) {
                // Klon ist in der linken Hand -> wegstecken
                re4vr::lua_set_bool("__re4_knife_left_clone", false);
                clone_destroy();
                left_grab_haptic();
                left_grab_sound();
            } else if (equipped) {
                // Messer ist engine-equippt (RECHTE Hand) -> linke Hand am
                // Holster ignorieren (kein Klau)
            } else {
                // Messer holstered -> Klon in die LINKE Hand ziehen
                re4vr::lua_set_bool("__re4_knife_left_clone", true);

                // [BARE_RIGHT 2026-07-17] War die rechte beim Uebernehmen KEINE
                // echte Gun? Dann darf der RIGHT->GUN-Block nicht die letzte Gun
                // nachziehen. get_IsEquipGun ist zuverlaessig (get_EquipWeaponID
                // luegt bei bare).
                auto* ctx = get_ctx_w2();
                auto* rhu0 = ctx != nullptr
                    ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                    : nullptr;
                const bool is_gun = rhu0 != nullptr
                    && re4vr::call_safe<bool>(rhu0, "get_IsEquipGun") == true;

                re4vr::lua_set_bool("__re4_clone_no_autogun", !is_gun);

                left_grab_haptic();
                left_grab_sound();
            }
        }

        m_armed = false;
    }

    m_prev_lgrip = lgrip;

    // ------------------------------------------------------------------
    // [LH_CLONE MELEE] Links-Klon-Schwung
    // ------------------------------------------------------------------
    auto& vr = VR::get();

    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
        && re4vr::lua_get_tribool("__re4_knife_flying") != 1 && vr != nullptr
        && vr->is_hmd_active()) {
        // [PHYSISCH 2026-07-08] Schwung aus der PHYSISCHEN Controller-Position
        // (VR-Space), NICHT der Welt-Position: die aendert sich durch
        // Spieler-Translation UND -Rotation -> Dauer-Fehltrigger.
        // controllers[0] = links (Lua: controllers[1]).
        const auto& controllers = vr->get_controllers();

        if (controllers.empty()) {
            m_prev_lh.reset();
        } else {
            const glm::vec3 wp{vr->get_position(controllers[0])};

            const double dt = now - m_lh_t;
            float spd = 0.0f;

            if (m_prev_lh.has_value() && dt > 0.001 && dt < 0.2) {
                const float dx = wp.x - m_prev_lh->x;
                const float dy = wp.y - m_prev_lh->y;
                const float dz = wp.z - m_prev_lh->z;
                const float horiz = std::sqrt(dx * dx + dz * dz);

                if (dy > 0.0f && dy > horiz) {
                    spd = 0.0f;   // reines Anheben zaehlt nicht (wie rechts)
                } else {
                    spd = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(dt);
                }
            }

            m_prev_lh = wp;
            m_lh_t = now;

            // [EIN RADIUS] Holster-Wert
            const bool at_holster = d.has_value() && *d <= r_rel;

            // [KNIFE_FLIP] Flip + sichtbarer Finisher-Prompt -> die
            // Shake-Finisher-Geste hat VORRANG, KEIN Links-Melee.
            const bool finisher_active =
                re4vr::lua_get_tribool("__vr_knife_flip") == 1 && prompt_visible();

            const bool gates_ok = !finisher_active && !at_holster
                && re4vr::lua_get_tribool("__re4_knife_throw_gripping") != 1;

            // [SOUND VS. TREFFER] Der Kommentar der Lua kuendigt eine EIGENE,
            // tiefere Treffer-Schwelle an -- eingebaut wurde sie NIE. Live
            // benutzen Treffer und Sound dieselbe. Original-Fehler, 1:1 mit.
            const float thr = static_cast<float>(
                re4vr::lua_get_number("__re4_knife_lh_swing_speed", 3.0));

            if (gates_ok && spd >= thr && (now - m_last_hit) > SWING_CD) {
                m_last_hit = now;

                // [LH_DIRECT] Schaden garantiert via HitPoint.addDamage.
                knife_direct_damage(1.8f);

                // [BREAKABLE 2026-07-09] Boxen im Links-Schwung mitbrechen.
                // break_nearby ist weapons.lua -> noch Lua.
                // [RADIUS 05.09.2026] Lua ruft `bn(lhw)` mit EINEM Argument --
                // der Radius bleibt nil und __re4_break_nearby nimmt seinen
                // Default (__re4_knife_touch bzw. 0.90). Der Port uebergab hier
                // explizit 0.0f; damit verwarf der Nahtest jede Kiste
                // (`d2 > touch*touch` mit touch = 0) und der Links-Schwung
                // zerschlug nichts mehr.
                if (const auto lhw = re4vr::lua_get_vec3("__vr_lh_world"); lhw.has_value()) {
                    re4vr::lua_call_global_pos_bool("__re4_break_nearby", *lhw);
                }
            }

            // SOUND: unveraenderte, hohe Schwelle.
            if (gates_ok && spd >= thr && (now - m_last_swing) > SWING_CD) {
                m_last_swing = now;

                const double sid =
                    re4vr::lua_get_table_number("__re4_knife_snd", "swing", 1800445513.0);

                knife_lh_play_sound(static_cast<int32_t>(sid));
            }
        }
    } else {
        m_prev_lh.reset();
    }

    // [RIGHT_HAND_GUN ENTFERNT 2026-07-17] Hier stand ein Block, der mitten im
    // Gameplay requestEquipGun rief. NICHT wieder einbauen.
    // __re4_clone_no_autogun wird dadurch nur noch gesetzt, nie gelesen --
    // harmlos, bewusst stehen gelassen.

    // ------------------------------------------------------------------
    // [CLONE-FINISHER RESTORE] Absicht latchen, solange der Finisher-Prompt
    // sichtbar ist + Klon links.
    // ------------------------------------------------------------------
    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1 && prompt_visible()) {
        re4vr::lua_set_bool("__re4_clone_finisher_restore", true);
        re4vr::lua_set_number("__re4_clone_finisher_restore_t", now);
    }

    const bool in_ks = re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
        || re4vr::lua_get_tribool("__re4_ks4_active") == 1;

    if (re4vr::lua_get_tribool("__re4_clone_finisher_restore") == 1 && in_ks) {
        // waehrend des Finisher-KS die Zeitmarke frisch halten (die Anim dauert
        // laenger als der Prompt)
        re4vr::lua_set_number("__re4_clone_finisher_restore_t", now);
    }

    const bool clone_fin_restoring =
        re4vr::lua_get_tribool("__re4_clone_finisher_restore") == 1
        && (now - re4vr::lua_get_number("__re4_clone_finisher_restore_t", 0.0)) < 1.5;

    // [LH_CLONE BOTH-HANDS-KILL] Messer engine-equippt (rechts) waehrend ein
    // Klon links ist -> Konflikt. Wurf-Ausnahme: waehrend __re4_knife_flying ist
    // der Klon unterwegs -> nicht killen.
    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
        && re4vr::lua_get_tribool("__re4_knife_equipped") == 1
        && re4vr::lua_get_tribool("__re4_knife_flying") != 1) {
        if (clone_fin_restoring) {
            // Finisher-Fall: NICHT killen. Das rechts equippte Messer bewusst
            // holstern -- aber ERST nach dem KS. throttle: nicht jeden Frame.
            if (!in_ks
                && (!m_clone.fin_kick.has_value() || (now - *m_clone.fin_kick) > 0.2)) {
                m_clone.fin_kick = now;

                auto& hol = RE4VRHolster::get();

                if (hol != nullptr) {
                    hol->holster_bare();
                }
            }
        } else {
            re4vr::lua_set_bool("__re4_knife_left_clone", false);
            clone_destroy();
            left_grab_sound();
        }
    } else if (re4vr::lua_get_tribool("__re4_clone_finisher_restore") == 1
               && m_clone.fin_kick.has_value()
               && re4vr::lua_get_tribool("__re4_knife_equipped") != 1) {
        // Holster hat gegriffen -> Restore fertig.
        re4vr::lua_set_bool("__re4_clone_finisher_restore", false);
        m_clone.fin_kick.reset();
    }

    // Timeout/Abbruch: Latch abgelaufen und nicht mehr im KS -> aufraeumen,
    // sonst wuerde ein SPAETERER Rechts-Equip faelschlich als Finisher gelten.
    if (re4vr::lua_get_tribool("__re4_clone_finisher_restore") == 1 && !clone_fin_restoring
        && !in_ks) {
        re4vr::lua_set_bool("__re4_clone_finisher_restore", false);
        m_clone.fin_kick.reset();
    }

    // [LH_CLONE SUPPORT-KILL] Support-Hand angedockt -> Klon KILLEN.
    // [KNIFE_LEFT_NO_SUPPORT 2026-08-30] AUSNAHME ueber __vr_support_knife_block.
    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
        && re4vr::lua_get_tribool("__vr_support_hand_docked") == 1
        && re4vr::lua_get_tribool("__vr_support_knife_block") != 1
        && re4vr::lua_get_tribool("__re4_knife_flying") != 1) {
        re4vr::lua_set_bool("__re4_knife_left_clone", false);
        clone_destroy();
        left_grab_sound();
    }

    // [LH_CLONE RELOAD/RACK-KILL] Mag-Ziehen ODER Slide-Rack braucht die LINKE
    // Hand -> Klon killen. Sonst ueberschreibt die Messer-Pose (laeuft in motion
    // ZULETZT) die Mag/Rack-Pose.
    {
        const std::string rackp = re4vr::lua_get_string("__vr_rack_hand_pose");
        const std::string magp = re4vr::lua_get_string("__vr_mag_hand_pose");

        const bool reload_active = !rackp.empty() || !magp.empty()
            // __vr_mag_in_hand = das GEMEINSAME Flag ALLER Reload-Module.
            // Manche setzen NUR dieses, KEINE mag_hand_pose.
            || re4vr::lua_get_tribool("__vr_mag_in_hand") == 1
            || re4vr::lua_get_tribool("__vr_slide_rack_active") == 1
            || re4vr::lua_get_tribool("__vr_rack_block_left_knife") == 1
            || player_is_reloading();

        if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
            && re4vr::lua_get_tribool("__re4_knife_flying") != 1 && reload_active) {
            re4vr::lua_set_bool("__re4_knife_left_clone", false);
            clone_destroy();
            left_grab_sound();
        }
    }

    // [LH_CLONE] Klon-Lifecycle/Pose jeden Frame pflegen.
    clone_manage();
}

// ============================================================================
// Block 3 -- Klon-Schaden (Lua Z.1647-2351)
//
// NATIVE Engine-Kette OHNE Collider: HitManager.calcInfo(DamageInfo, HC, HC)
// + HitManager.hitSetting(DamageInfo, HC, HC). requestAttack braucht dagegen
// einen Collider, den der Mesh-Klon nicht hat.
// ============================================================================

namespace {

constexpr const char* KNIFE_DMG_PATH = "re4_vr/re4_vr_knife_dmg.json";   // [NAME 19.09.2026] war re4_knife_dmg.json

constexpr const char* CALC_SIG =
    "calcInfo(chainsaw.HitController.DamageInfo, chainsaw.HitController, chainsaw.HitController)";
constexpr const char* HIT_SIG =
    "hitSetting(chainsaw.HitController.DamageInfo, chainsaw.HitController, chainsaw.HitController)";

::REManagedObject* get_hitmgr() {
    return re4vr::fc::on() ? re4vr::fc::managed_singleton("chainsaw.HitManager")
                           : sdk::get_managed_singleton<::REManagedObject>("chainsaw.HitManager");
}

::REManagedObject* get_hc(::REManagedObject* go) {
    auto* hm = get_hitmgr();

    if (hm == nullptr || go == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(hm, "getHitController", go);
}

::REManagedObject* player_body_go_w2() {
    return re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
}

std::optional<uintptr_t> go_addr(::REManagedObject* o) {
    if (o == nullptr) {
        return std::nullopt;
    }

    return reinterpret_cast<uintptr_t>(o);
}

// Ein Feld typgerecht schreiben -- Luas set_field(name, value).
// [CONTAINER-FLAG] get_data_raw(obj, is_value_type) beschreibt den CONTAINER,
// nicht den Feldtyp: bei einem Managed Object immer false.
bool set_field_num(::REManagedObject* obj, const char* name, double value) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* f = td != nullptr ? td->get_field(name) : nullptr;

    if (f == nullptr) {
        return false;
    }

    auto* p = f->get_data_raw(obj, false);

    if (p == nullptr) {
        return false;
    }

    auto* ft = f->get_type();
    const std::string tn = ft != nullptr ? ft->get_full_name() : std::string{};

    if (tn == "System.Single") {
        *reinterpret_cast<float*>(p) = static_cast<float>(value);
    } else if (tn == "System.Double") {
        *reinterpret_cast<double*>(p) = value;
    } else if (tn == "System.Boolean") {
        *reinterpret_cast<bool*>(p) = value != 0.0;
    } else if (tn == "System.UInt32") {
        *reinterpret_cast<uint32_t*>(p) =
            static_cast<uint32_t>(static_cast<uint64_t>(value));
    } else if (tn == "System.Int32") {
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(value);
    } else {
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(value);
    }

    return true;
}

bool set_field_obj(::REManagedObject* obj, const char* name, ::REManagedObject* value) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* f = td != nullptr ? td->get_field(name) : nullptr;

    if (f == nullptr) {
        return false;
    }

    auto* p = f->get_data_raw(obj, false);

    if (p == nullptr) {
        return false;
    }

    *reinterpret_cast<::REManagedObject**>(p) = value;

    return true;
}

::REManagedObject* get_field_obj(::REManagedObject* obj, const char* name) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    try {
        return re4vr::get_field_object(obj, name);
    } catch (...) {
        return nullptr;
    }
}

}   // namespace

// ---- Gespeicherte MESSER-Schadenswerte (persistent) ------------------------

bool RE4VRWeapons2::save_vals_load() {
    const auto d = re4vr::json_load(KNIFE_DMG_PATH);

    if (!d.is_object()) {
        return false;
    }

    const auto dmg = d.find("damage");

    if (dmg == d.end() || !dmg->is_number() || dmg->get<double>() <= 0.0) {
        return false;
    }

    SavedVals v{};
    v.damage = dmg->get<float>();

    const auto g = [&](const char* key, float def) -> float {
        const auto it = d.find(key);

        return (it != d.end() && it->is_number()) ? it->get<float>() : def;
    };

    v.wince = g("wince", 1.0f);
    v.brk = g("brk", 1.0f);
    v.stop = g("stop", 1.0f);

    if (const auto it = d.find("wid"); it != d.end() && it->is_number()) {
        v.wid = it->get<int32_t>();
    }

    m_saved_vals = v;

    return true;
}

void RE4VRWeapons2::save_vals_store(const SavedVals& v) {
    m_saved_vals = v;

    nlohmann::json d{};
    d["damage"] = v.damage;
    d["wince"] = v.wince;
    d["brk"] = v.brk;
    d["stop"] = v.stop;

    if (v.wid.has_value()) {
        d["wid"] = *v.wid;
    }

    re4vr::json_save(KNIFE_DMG_PATH, d);
}

// ---- Template: DamageInfo aus einem echten Treffer fangen -------------------
// Eigene Instanz + copy, weil die Engine ihre DamageInfo poolt. Der Capture
// liefert die korrekt initialisierte HitInfo-Basis, auf die hitSetting
// angewiesen ist (frische leere Instanz -> AV-Gefahr in hitSetting).
void RE4VRWeapons2::capture_dmginfo(::REManagedObject* real_info, bool is_knife) {
    if (real_info == nullptr) {
        return;
    }

    if (m_dmginfo.obj == nullptr) {
        auto* inst =
            sdk::create_instance<::REManagedObject>("chainsaw.HitController.DamageInfo", true);

        if (inst == nullptr) {
            inst = sdk::create_instance<::REManagedObject>("chainsaw.HitController.DamageInfo");
        }

        if (inst != nullptr) {
            // Lua: pin -- selbst erzeugt, also BEDINGUNGSLOS.
            store(m_dmginfo, inst, true);
        }
    }

    if (m_dmginfo.obj != nullptr) {
        re4vr::call_safe<void*>(m_dmginfo.obj, "copy", real_info);
    }

    if (!is_knife) {
        return;
    }

    float dmg = 0.0f;

    // [PORTFIX 2026-09-06] typrichtig lesen -- das ist der Schaden selbst.
    const auto dmg_o = re4vr::call_num(real_info, "get_Damage");
    dmg = dmg_o.has_value() ? static_cast<float>(*dmg_o) : 0.0f;

    if (!dmg_o.has_value() || dmg <= 0.0f) {
        return;
    }

    SavedVals v{};
    v.damage = dmg;

    float f = 0.0f;
    v.wince = static_cast<float>(re4vr::call_num(real_info, "get_Wince").value_or(1.0));
    v.brk = static_cast<float>(re4vr::call_num(real_info, "get_Break").value_or(1.0));
    v.stop = static_cast<float>(re4vr::call_num(real_info, "get_Stopping").value_or(1.0));
    v.wid = enum_as_int(real_info, "get_WeaponID");

    save_vals_store(v);
}

// callbackAttackHit(DamageInfo): args[2]=Angreifer-HC, args[3]=DamageInfo.
void RE4VRWeapons2::capture_cb(::REManagedObject* attacker_hc, ::REManagedObject* info) {
    if (info == nullptr) {
        return;
    }

    const auto wid = enum_as_int(attacker_hc, "get_WeaponID");

    // [REKONSTRUKTION 2026-07-23] Das ECHTE, von der Engine vollstaendig
    // gefuellte DamageInfo festhalten. Kein create_instance (liefert in diesem
    // Build keine DamageInfo).
    // [OHNE PRIMEN] JEDES Treffer-Ereignis liefert ein vollstaendig gefuelltes
    // DamageInfo -- die Struktur ist das, was wir brauchen.
    const bool is_knife = wid.has_value() && is_knife_id(*wid);

    if (is_knife || m_dmginfo_raw.obj == nullptr) {
        if (is_knife || !m_dmginfo_raw_knife) {
            store(m_dmginfo_raw, info, true);   // Lua: info:add_ref()

            if (is_knife) {
                m_dmginfo_raw_knife = true;
            }
        }
    }

    if (is_knife) {
        capture_dmginfo(info, true);

        return;
    }

    if (m_dmginfo.obj != nullptr) {
        return;
    }

    auto* owner = re4vr::call_safe<::REManagedObject*>(info, "get_AttackOwnerObject");
    auto* pb = player_body_go_w2();

    if (owner != nullptr && pb != nullptr && go_addr(owner) == go_addr(pb)) {
        capture_dmginfo(info, false);
    }
}

// ---- [DI_SAVELOAD] Vorlagen bei Save-Load/Charakterwechsel verwerfen --------
// Bewusst KEIN release auf das gepoolte Objekt: nach dem Load kann es tot sein,
// ein release darauf waere genau der Deref, den wir vermeiden wollen.
// Loslassen genuegt, die Engine besitzt es ohnehin.
// [BODY-EPOCH 2026-09-22] Nur eigenen Zustand verwerfen: der Sound-Container
// des linken Messers gehoert zur alten Szene. drop() gibt nur die eigene
// add_ref-Referenz frei, keine Engine-Aufrufe auf dem alten Objekt.
void RE4VRWeapons2::drop_body_caches() {
    drop(m_lh_snd_last);
}

void RE4VRWeapons2::knife_drop_caches(const char* grund) {
    // Nur den Zeiger loslassen -- NICHT release() rufen.
    m_dmginfo.obj = nullptr;
    m_dmginfo.reffed = false;
    m_dmginfo_raw.obj = nullptr;
    m_dmginfo_raw.reffed = false;
    m_dmginfo_raw_knife = false;
    m_native_di.obj = nullptr;
    m_native_di.reffed = false;
    m_dud.obj = nullptr;
    m_dud.reffed = false;

    // geliehene AttackUserData (weapons.lua __re4_borrow_knife_atk)
    re4vr::lua_set_nil("__re4_knife_atk_cache");

    // [FORENSIK] wann zuletzt verworfen
    re4vr::lua_set_number("__re4_knife_di_dropped_t", clock_now());
    re4vr::lua_set_string("__re4_knife_di_dropped_why", grund);
}

void RE4VRWeapons2::knife_di_guard() {
    const double now = clock_now();

    if (now < m_di_next_check) {
        return;
    }

    m_di_next_check = now + 0.25;

    auto* pb = player_body_go_w2();
    const auto a = go_addr(pb);

    if (!a.has_value()) {
        // [LH_CLONE RESET 20.09.2026] Body GANZ weg = Tod / Quickload /
        // Mercs-Levelstart (Holster nutzt dasselbe Signal, [MERCS-LEVELSTART]).
        // Ein Levelstart mit demselben Charakter kann dieselbe Adresse
        // zurueckliefern -- der Adress-Sprung allein reicht also nicht.
        m_di_body_weg = true;

        return;
    }

    if (!m_di_body_addr.has_value()) {
        m_di_body_addr = a;
        m_di_body_weg = false;

        return;
    }

    const bool neuer_body = (*a != *m_di_body_addr) || m_di_body_weg;

    m_di_body_addr = a;
    m_di_body_weg = false;

    if (!neuer_body) {
        return;
    }

    knife_drop_caches("Body-Wechsel (Save-Load / Stage / Charakter)");

    // [LH_CLONE RESET 20.09.2026] Der Links-Klon gehoert MIT in diesen Reset:
    // bisher ueberlebte __re4_knife_left_clone jeden Levelstart, und
    // clone_manage baute das Messer im naechsten Frame in der linken Hand des
    // NEUEN Charakters wieder auf (gesehen: Leon mit Messer links -> Wechsel
    // auf Krauser -> Krauser startet mit Messer links).
    re4vr::lua_set_bool("__re4_knife_left_clone", false);
    re4vr::lua_set_bool("__re4_knife_left_intent", false);
    clone_forget();
}

// ---- Ziel: naechster lebender Gegner ---------------------------------------
bool RE4VRWeapons2::pick_nearest_enemy(const glm::vec3& pos, float reach, Target& out) {
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return false;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList");

    if (!re4vr::obj_ok(list)) {
        return false;
    }

    int32_t n = 0;

    if (!re4vr::try_call<int32_t>(list, "get_Count", n)) {
        return false;
    }

    bool found = false;
    float bestd = reach * reach;

    for (int32_t i = 0; i < n; ++i) {
        auto* e = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (!re4vr::obj_ok(e)) {
            continue;
        }

        auto* body = re4vr::call_safe<::REManagedObject*>(e, "get_BodyGameObject");

        if (!re4vr::obj_ok(body)) {
            continue;
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

        if (!re4vr::obj_ok(tf)) {
            continue;
        }

        glm::vec3 p{};

        if (!get_vec3(tf, "get_Position", p)) {
            continue;
        }

        auto* hc = get_hc(body);

        if (hc == nullptr) {
            continue;
        }

        // [PORTFIX 2026-09-06] typrichtig aus der TDB lesen (re4vr::call_num) --
        // blind als float gelesen kam 0 an und jeder Gegner galt als tot.
        const auto hp_o = re4vr::call_num(hc, "get_CurrentHitPoint");

        if (!hp_o.has_value() || *hp_o <= 0.0) {
            continue;
        }

        float d2 = 0.0f;

        // [KNOCHEN 2026-08-14] nur zugeschaltet -- der Scan als ALLEINIGES
        // Kriterium traf gar nichts (weapons.lua nearest_bone_dist).
        if (re4vr::lua_get_tribool("__re4_knife_pick_bone") == 1
            && re4vr::lua_has_function("__re4_nearest_bone_dist")) {
            // Vorfilter wie rechts (reach * 4): sonst laeuft der Baum-Walk fuer
            // jeden Gegner im Level.
            const float cx = p.x - pos.x;
            const float cy = (p.y + 0.9f) - pos.y;
            const float cz = p.z - pos.z;
            const float pre = reach * 4.0f;

            if ((cx * cx + cy * cy + cz * cz) <= pre * pre) {
                const auto bd = re4vr::lua_call_bone_dist("__re4_nearest_bone_dist", tf, pos);
                const float b = bd.has_value() ? static_cast<float>(*bd) : 999.0f;
                d2 = b * b;
            } else {
                d2 = 1e9f;
            }
        } else if (re4vr::lua_get_tribool("__re4_knife_pick_sphere") == 1) {
            // Rueckbau: die alte Kugel um die Koerpermitte.
            const float dx = p.x - pos.x;
            const float dy = (p.y + 0.9f) - pos.y;
            const float dz = p.z - pos.z;
            d2 = dx * dx + dy * dy + dz * dz;
        } else {
            // [KASTEN 2026-08-14] Default. Quadratischer Querschnitt statt
            // Kreis -- der runde liess Treffer an den ABGESPREIZTEN ARMEN
            // durchrutschen. Hoehe ueber die volle Figur.
            const float ylo =
                p.y + static_cast<float>(re4vr::lua_get_number("__re4_knife_pick_ylo", 0.0));
            const float yhi =
                p.y + static_cast<float>(re4vr::lua_get_number("__re4_knife_pick_yhi", 1.9));

            float dy = 0.0f;

            if (pos.y < ylo) {
                dy = ylo - pos.y;
            } else if (pos.y > yhi) {
                dy = pos.y - yhi;
            }

            const float dx = std::abs(p.x - pos.x);
            const float dz = std::abs(p.z - pos.z);
            float box = static_cast<float>(re4vr::lua_get_number("__re4_knife_pick_box", 1.30));

            if (box <= 0.01f) {
                box = 1.0f;
            }

            // Kastentest: die groessere der beiden Achsen entscheidet
            // (Chebyshev). Nur der Reichweitentest ist eckig.
            const float edge = std::max(dx, dz) / box;
            d2 = edge * edge + dy * dy;
        }

        if (d2 < bestd) {
            bestd = d2;
            out.body = body;
            out.hc = hc;
            out.pos = p;
            found = true;
        }
    }

    return found;
}

// ---- DamageInfo fuer die native Kette bauen --------------------------------
//
// DIE WICHTIGSTE FUNKTION DER DATEI. An ihr haengen Schaden, BLUT und
// Crashfreiheit: die Engine erkennt den Klon-Treffer nur dann als echten
// Messertreffer an, wenn die DamageInfo feldgleich zu einem Rechts-Treffer ist.
// Siehe [[reference_re4_klon_damageinfo_1zu1_nativ]].
::REManagedObject* RE4VRWeapons2::build_dmginfo(::REManagedObject* victim_hc,
                                                ::REManagedObject* victim_body,
                                                const std::optional<glm::vec3>& contact_pos,
                                                float& dmg_out) {
    auto* di = m_native_di.obj;

    if (di == nullptr) {
        di = sdk::create_instance<::REManagedObject>("chainsaw.HitController.DamageInfo", true);

        if (di == nullptr) {
            di = sdk::create_instance<::REManagedObject>("chainsaw.HitController.DamageInfo");
        }

        if (di == nullptr) {
            return nullptr;
        }

        store(m_native_di, di, true);   // selbst erzeugt -> bedingungslos pinnen
        di = m_native_di.obj;
    }

    // Valide HitInfo-Basis: gefangener Rechts-Treffer bevorzugt, sonst die
    // DamageCalcInfo des Ziels.
    auto* base = m_dmginfo.obj != nullptr
        ? m_dmginfo.obj
        : re4vr::call_safe<::REManagedObject*>(victim_hc, "get_DamageCalcInfo");

    if (base != nullptr) {
        re4vr::call_safe<void*>(di, "copy", base);
    }

    // [WAISE 05.09.2026] Der Choke-Override hat Vorrang -- er ersetzt den
    // Tabellentausch aus re4_vr_choke.lua (s. set_damage_override).
    const float dmg = m_damage_override.has_value()
        ? *m_damage_override
        : (m_saved_vals.has_value() ? m_saved_vals->damage : 225.0f);
    const float wince = m_saved_vals.has_value() ? m_saved_vals->wince : 64.0f;
    const float brk = m_saved_vals.has_value() ? m_saved_vals->brk : 1.0f;
    const float stop = m_saved_vals.has_value() ? m_saved_vals->stop : 1.0f;

    int32_t wid = 5006;

    if (m_saved_vals.has_value() && m_saved_vals->wid.has_value()) {
        wid = *m_saved_vals->wid;
    } else if (const auto c = re4vr::lua_get_number_opt("__re4_current_knife_wid");
               c.has_value()) {
        wid = static_cast<int32_t>(*c);
    }

    dmg_out = dmg;

    auto* pb = player_body_go_w2();

    re4vr::call_safe<void*>(di, "set_Damage", static_cast<int32_t>(std::floor(dmg)));
    re4vr::call_safe<void*>(di, "set_Wince", wince);
    re4vr::call_safe<void*>(di, "set_Break", brk);
    re4vr::call_safe<void*>(di, "set_Stopping", stop);
    re4vr::call_safe<void*>(di, "set_IsCritical", false);
    re4vr::call_safe<void*>(di, "set_IsKill", false);

    if (pb != nullptr) {
        re4vr::call_safe<void*>(di, "set_AttackOwnerObject", pb);
    }

    re4vr::call_safe<void*>(di, "set_WeaponID", wid);

    // [PER-VICTIM] ContactInfo auf den aktuellen Gegner umbiegen -> Schaden +
    // BLUT am richtigen Gegner. AttackGameObject ist hier nur die
    // RUECKFALLEBENE -- unten wird es auf das MESSER-GO umgesetzt.
    if (pb != nullptr) {
        re4vr::call_safe<void*>(di, "set_AttackGameObject", pb);
    }

    if (victim_body != nullptr) {
        re4vr::call_safe<void*>(di, "set_DamageGameObject", victim_body);
    }

    if (contact_pos.has_value()) {
        set_vec3(di, "set_Position", *contact_pos);

        if (pb != nullptr) {
            auto* ptf = re4vr::call_safe<::REManagedObject*>(pb, "get_Transform");
            glm::vec3 pp{};

            if (ptf != nullptr && get_vec3(ptf, "get_Position", pp)) {
                glm::vec3 nrm = *contact_pos - pp;
                float L = glm::length(nrm);

                if (L < 0.001f) {
                    L = 1.0f;
                }

                set_vec3(di, "set_Normal", nrm / L);
            }
        }
    }

    // [PRIME-FREI] Klingen-AttackUserData direkt vom Messer-Collider.
    // __re4_find_knife_hc und __re4_knife_get_attack_ud kommen aus weapons.lua
    // -- BEIDE noch Lua, der Weg fuehrt also ueber den Lua-State.
    bool atk_set = false;
    bool ad_set = false;
    const char* go_txt = "player";

    if (re4vr::lua_has_function("__re4_find_knife_hc")
        && re4vr::lua_has_function("__re4_knife_get_attack_ud")) {
        auto* khc = re4vr::lua_call_global_obj("__re4_find_knife_hc");

        // [1:1 NATIV 2026-08-14 -- gemessen] Im echten Rechts-Treffer stehen
        // AttackGameObject UND WeaponGameObject auf DERSELBEN Adresse: dem
        // GameObject des MESSERS. Der Player-Body steht nur im
        // AttackOwnerObject. GENAU DARAN HING DAS BLUT.
        // Rueckbau: __re4_knife_atk_go_player = true.
        if (re4vr::lua_get_tribool("__re4_knife_atk_go_player") != 1) {
            auto* kgo = khc != nullptr
                ? re4vr::call_safe<::REManagedObject*>(khc, "get_GameObject")
                : nullptr;

            if (kgo != nullptr && re4vr::obj_ok(kgo)) {
                re4vr::call_safe<void*>(di, "set_AttackGameObject", kgo);
                re4vr::call_safe<void*>(di, "set_WeaponGameObject", kgo);
                go_txt = "messer";
            }
        }

        auto* atk = khc != nullptr
            ? re4vr::lua_call_global_obj_arg("__re4_knife_get_attack_ud", khc)
            : nullptr;

        if (atk != nullptr) {
            re4vr::call_safe<void*>(di, "set_AttackUserData", atk);
            atk_set = true;

            // AttackData aus der AttackUserData ableiten -- ohne sie lehnt
            // hitSetting ab.
            auto* ad = re4vr::call_safe<::REManagedObject*>(
                khc, "getAttackData(chainsaw.collision.AttackUserData)", atk);

            if (ad != nullptr) {
                re4vr::call_safe<void*>(di, "set_AttackData", ad);
                ad_set = true;
            }
        }
    }

    // DamageUserData (frisch) + ChildHitController (Opfer).
    auto* dud = m_dud.obj;

    if (dud == nullptr) {
        dud = sdk::create_instance<::REManagedObject>("chainsaw.collision.DamageUserData", true);

        if (dud == nullptr) {
            dud = sdk::create_instance<::REManagedObject>("chainsaw.collision.DamageUserData");
        }

        if (dud != nullptr) {
            store(m_dud, dud, true);
            dud = m_dud.obj;
        }
    }

    bool dud_set = false;

    if (dud != nullptr) {
        re4vr::call_safe<void*>(di, "set_DamageUserData", dud);
        dud_set = true;
    }

    // [1:1 NATIV 2026-08-14] Die ECHTE DamageInfo eines rechten Messertreffers
    // traegt ChildHitController = nil. Ein hier eingetragener Opfer-HC bleibt
    // ueber den Treffer hinaus stehen (das Objekt haengt per add_ref und wird
    // wiederverwendet) -- auch wenn der Gegner Sekunden spaeter stirbt. Genau
    // das Muster des sporadischen c0000005.
    // Deshalb AKTIV LEEREN statt nur weglassen: das copy oben kann aus der
    // Vorlage durchaus einen Zeiger mitgebracht haben.
    // Rueckbau: __re4_knife_set_child = true.
    const char* child_txt = "?";

    if (re4vr::lua_get_tribool("__re4_knife_set_child") == 1 && victim_hc != nullptr) {
        re4vr::call_safe<void*>(di, "set_ChildHitController", victim_hc);
        child_txt = "gesetzt(Rueckbau)";
    } else {
        re4vr::call_safe<void*>(di, "set_ChildHitController", nullptr);

        auto* c = get_field_obj(di, "<ChildHitController>k__BackingField");
        child_txt = c == nullptr ? "leer-ok" : "LEEREN FEHLGESCHLAGEN";
    }

    // [COLLIDABLE-LEICHEN 2026-08-18 -- bewiesen aus vier Crash-Dumps]
    // Der Absturz sass IMMER an derselben Instruktion
    // (chainsaw.EPVExpertDamageEffect.findTargetJoint, EIN Collidable-Parameter).
    // Die beiden Felder stammen aus der eingefangenen Vorlage, also von einem
    // frueheren Treffer an einem ANDEREN Gegner. Ist der tot, ist sein Collider
    // recycelt -- die Typkennung besteht den Check noch, der Inhalt gehoert
    // fremdem Speicher.
    // Warum Nullen sicher ist: die Disassembly beginnt mit `test r8,r8` +
    // Sprung ans Ende -- null wird sauber abgefangen, nur Muell ist toedlich.
    // Rueckbau: __re4_knife_keep_collidables = true.
    if (re4vr::lua_get_tribool("__re4_knife_keep_collidables") != 1) {
        int32_t geleert = 0;

        for (const char* feld : {"AttackCollidable", "DamageCollidable"}) {
            const std::string bf = std::string{"<"} + feld + ">k__BackingField";
            auto* vorher = get_field_obj(di, bf.c_str());

            re4vr::call_safe<void*>(di, (std::string{"set_"} + feld).c_str(), nullptr);

            auto* nachher = get_field_obj(di, bf.c_str());

            if (nachher == nullptr && vorher != nullptr) {
                ++geleert;
            }
        }

        re4vr::lua_set_number(
            "__re4_knife_collidables_cleared",
            re4vr::lua_get_number("__re4_knife_collidables_cleared", 0.0) + geleert);
    }

    // hitSetting prueft evtl. IsActive
    re4vr::call_safe<void*>(di, "set_IsActive", true);

    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s/%s dud=%s child=%s go=%s",
                      atk_set ? "true" : "false", ad_set ? "true" : "false",
                      dud_set ? "true" : "false", child_txt, go_txt);
        re4vr::lua_set_string("__re4_knife_atk_direct", buf);
    }

    return di;
}

// ---- NATIVER Treffer: calcInfo + hitSetting (kein Collider) -----------------
namespace {

// a,b = (Angreifer,Opfer) in EINER Reihenfolge.
std::optional<bool> native_hit(::REManagedObject* hm, ::REManagedObject* di,
                               ::REManagedObject* a, ::REManagedObject* b) {
    re4vr::call_safe<void*>(hm, CALC_SIG, di, a, b);

    bool ok = false;

    if (re4vr::try_call<bool>(hm, HIT_SIG, ok, di, a, b)) {
        return ok;
    }

    return std::nullopt;
}

// Ein Feld ROH sichern und zurueckschreiben. Lua nutzt get_field/set_field mit
// gemischten Typen (Zahlen UND Objekte); roh ist das typunabhaengig und
// verhaelt sich identisch.
struct RawField {
    void* p{nullptr};
    size_t size{0};
    uint64_t old{0};
    bool valid{false};
};

RawField raw_field_open(::REManagedObject* obj, const char* name) {
    RawField r{};

    if (!re4vr::obj_ok(obj)) {
        return r;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    auto* f = td != nullptr ? td->get_field(name) : nullptr;

    if (f == nullptr) {
        return r;
    }

    auto* p = f->get_data_raw(obj, false);

    if (p == nullptr) {
        return r;
    }

    auto* ft = f->get_type();
    const std::string tn = ft != nullptr ? ft->get_full_name() : std::string{};

    // Objektverweise sind 8 Byte, die hier benutzten Zahlen/Bools 4 bzw. 1.
    if (tn == "System.Single" || tn == "System.Int32" || tn == "System.UInt32") {
        r.size = 4;
    } else if (tn == "System.Boolean") {
        r.size = 1;
    } else {
        r.size = 8;
    }

    r.p = p;
    r.old = 0;
    std::memcpy(&r.old, p, r.size);
    r.valid = true;

    return r;
}

void raw_field_restore(const RawField& r) {
    if (!r.valid || r.p == nullptr) {
        return;
    }

    std::memcpy(r.p, &r.old, r.size);
}

}   // namespace

bool RE4VRWeapons2::knife_direct_damage_at(const glm::vec3& pos, float reach) {
    Target tgt{};

    if (!pick_nearest_enemy(pos, reach, tgt)) {
        re4vr::lua_set_string("__re4_knife_clone_dbg", "noenemy");

        return false;
    }

    auto* victim = tgt.hc;

    // Angreifer-HC = Player-Body-HitController.
    auto* pb = player_body_go_w2();
    auto* atkhc = pb != nullptr ? get_hc(pb) : nullptr;

    if (atkhc == nullptr) {
        re4vr::lua_set_string("__re4_knife_clone_dbg", "noatkhc");

        return false;
    }

    auto* hm = get_hitmgr();

    if (hm == nullptr) {
        re4vr::lua_set_string("__re4_knife_clone_dbg", "nohm");

        return false;
    }

    // [KONTAKTPUNKT 2026-08-14] Vorher lag der Punkt FEST auf Gegner-Wurzel +
    // 1.0 -- egal wo die Klinge war. Die Engine sucht ueber findTargetJoint den
    // Joint ZUR POSITION, deshalb sah jeder Treffer gleich aus.
    // Jetzt wird `pos` benutzt und auf den Gegner gezogen (Hoehe geklemmt,
    // horizontal auf einen Koerperradius um die Achse).
    // Rueckbau: __re4_knife_contact_center = true.
    glm::vec3 cpos{};

    if (re4vr::lua_get_tribool("__re4_knife_contact_center") != 1) {
        // [RADIUS] 0.35 war zu weit: der Einschlag lag rund einen halben Meter
        // VOR dem Gegner. Das ist der Abstand von der KOERPERACHSE -- ein
        // Ganado misst dort nur gut 0.15 m.
        const float R = static_cast<float>(re4vr::lua_get_number("__re4_knife_hit_radius", 0.15));
        const float ymin =
            static_cast<float>(re4vr::lua_get_number("__re4_knife_hit_ymin", 0.20));
        const float ymax =
            static_cast<float>(re4vr::lua_get_number("__re4_knife_hit_ymax", 1.80));

        float y = pos.y;

        if (y < tgt.pos.y + ymin) {
            y = tgt.pos.y + ymin;
        }

        if (y > tgt.pos.y + ymax) {
            y = tgt.pos.y + ymax;
        }

        float dx = pos.x - tgt.pos.x;
        float dz = pos.z - tgt.pos.z;
        const float d = std::sqrt(dx * dx + dz * dz);

        if (d > R && d > 0.0001f) {
            dx *= R / d;
            dz *= R / d;
        }

        cpos = glm::vec3{tgt.pos.x + dx, y, tgt.pos.z + dz};
    } else {
        cpos = glm::vec3{tgt.pos.x, tgt.pos.y + 1.0f, tgt.pos.z};
    }

    float dmg = 0.0f;
    auto* di = build_dmginfo(victim, tgt.body, cpos, dmg);

    if (di == nullptr) {
        re4vr::lua_set_string("__re4_knife_clone_dbg", "nodi");

        return false;
    }

    // [PORTFIX 2026-09-06] typrichtig lesen -- s. oben.
    const auto hp0_o = re4vr::call_num(victim, "get_CurrentHitPoint");
    const float hp0 = hp0_o.has_value() ? static_cast<float>(*hp0_o) : 0.0f;
    const bool have_hp0 = hp0_o.has_value();

    // [CRASH-HARDEN 2026-07-20 -- Crashdump belegt] Stirbt oder despawnt der
    // Gegner waehrend das Messer fliegt, existiert sein HitController zwar noch
    // als Objekt, ist intern aber tot. native_hit deref't dann null, und ein
    // pcall faengt eine Access Violation NICHT.
    {
        bool alive = have_hp0 && hp0 > 0.0f;

        if (alive) {
            bool v = true;

            if (re4vr::try_call<bool>(victim, "get_Valid", v) && !v) {
                alive = false;
            }
        }

        if (alive) {
            bool v = true;

            if (re4vr::try_call<bool>(victim, "get_IsLive", v) && !v) {
                alive = false;
            }
        }

        // [ORIGINAL-FEHLER, 1:1] Lua prueft hier zusaetzlich
        // `tgt.ctx:get_IsEliminated()` -- aber pick_nearest_enemy setzt das Feld
        // `ctx` NIE (es liefert nur body, hc, pos). Die Pruefung lief also immer
        // ins Leere und wandert als Leerlauf mit.

        if (!alive) {
            re4vr::lua_set_string("__re4_knife_clone_dbg", "victim-tot/ungueltig");

            return false;
        }
    }

    // [WURF STECKEN 2026-08-26] WER getroffen wurde und WO -- die Flugmaschine
    // in weapons.lua leitet daraus ab, in wen sie das Messer stecken soll.
    re4vr::lua_set_hit_record("__re4_knife_last_hit", tgt.body, cpos, clock_now());

    // Reihenfolge A fest (Angreifer, Opfer). B ist raus -- sie hat im Test einen
    // Gegner GEHEILT (hp 85->1296).
    const auto ok = native_hit(hm, di, atkhc, victim);

    const auto hp1_o = re4vr::call_num(victim, "get_CurrentHitPoint");
    const float hp1 = hp1_o.has_value() ? static_cast<float>(*hp1_o) : 0.0f;
    const bool have_hp1 = hp1_o.has_value();

    // [HIT SOUND 2026-07-09] Messer-Treffer-Sound explizit ueber den
    // Messer-Container -- hitSetting spielt ihn nicht.
    const bool hit_landed =
        (ok.has_value() && *ok) || (have_hp0 && have_hp1 && hp1 < hp0);

    if (hit_landed) {
        // [FLEISCH 2026-08-26] Fuer JEDEN Gegnertreffer mit JEDEM Messer
        // dieselbe Stich-ID -- Wurf wie Melee. Die alte Wurf-ID bleibt nur ueber
        // __re4_knife_lh_use_throw_snd erreichbar.
        const bool melee = re4vr::lua_get_string("__re4_knife_hit_src") == "melee";
        double sid = re4vr::lua_get_number("__re4_knife_lh_hit_snd", 238304172.0);

        if (!melee && re4vr::lua_get_tribool("__re4_knife_lh_use_throw_snd") == 1) {
            sid = re4vr::lua_get_number("__re4_knife_lh_throw_hit_snd", 797300665.0);
        }

        // [STUMM 2026-08-26] Der Choke spielt beim STECKENBLEIBEN seinen eigenen
        // Ton. Setzt jemand __re4_knife_hit_mute, entfaellt hier der Laut -- und
        // NUR der.
        if (re4vr::lua_get_tribool("__re4_knife_hit_mute") != 1) {
            knife_lh_play_sound(static_cast<int32_t>(std::floor(sid)));
        }
    }

    // [BLUT] -- RUINE, siehe Spec §2.2.
    //
    // Die Vorlage __re4_blood_cap kam aus dem laengst geloeschten
    // re4_zz_blood_trace; das Global hat heute KEINEN Produzenten mehr
    // (ueber den ganzen autorun-Baum geprueft). `cap` ist damit immer nil,
    // bstat bleibt "nocap" und saveStamp wird NIE gerufen.
    //
    // Das heisst NICHT, dass Blut fehlt: seit dem 14.08. baut build_dmginfo die
    // DamageInfo feldgleich zu einem echten Rechts-Treffer, und die Engine
    // macht das Blut aus hitSetting heraus selbst. Der Kommentar der Lua
    // ("hitSetting rendert selbst kein Blut") ist veraltet und falsch.
    //
    // NICHT reparieren -- das war der Crash-Weg, und die Engine ruft eine
    // ANDERE saveStamp-Overload (StampEmitData) als dieser Replay.
    const char* bstat = "off";

    if (re4vr::lua_get_tribool("__re4_knife_blood_on") != 0) {
        const bool have_cap = re4vr::lua_has_value("__re4_blood_cap");

        // Die Lebend-Pruefung bleibt trotzdem stehen: sie ist billig und der
        // Block ist der dokumentierte Rueckweg.
        bool lebt = have_hp1 && hp1 > 0.0f;

        if (lebt) {
            bool v = true;

            if (re4vr::try_call<bool>(victim, "get_Valid", v) && !v) {
                lebt = false;
            }
        }

        if (lebt) {
            bool v = true;

            if (re4vr::try_call<bool>(victim, "get_IsLive", v) && !v) {
                lebt = false;
            }
        }

        if (lebt && !(tgt.body != nullptr && re4vr::obj_ok(tgt.body))) {
            lebt = false;
        }

        if (!have_cap) {
            bstat = "nocap";
        } else if (!lebt) {
            bstat = "opfer-tot/ungueltig";
        } else {
            // Unerreichbar, solange __re4_blood_cap keinen Produzenten hat.
            bstat = "node";
        }
    }

    const std::string src = re4vr::lua_get_string("__re4_knife_hit_src");
    re4vr::lua_set_nil("__re4_knife_hit_src");

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s set=%s dmg=%.0f hp %.0f->%.0f blut=%s atk=%s",
                      src.empty() ? "wurf" : src.c_str(),
                      ok.has_value() ? (*ok ? "true" : "false") : "nil", dmg,
                      have_hp0 ? hp0 : -1.0f, have_hp1 ? hp1 : -1.0f, bstat,
                      re4vr::lua_get_string("__re4_knife_atk_direct").c_str());
        re4vr::lua_set_string("__re4_knife_clone_dbg", buf);
    }

    return (have_hp0 && have_hp1 && hp1 < hp0) || (ok.has_value() && *ok);
}

bool RE4VRWeapons2::knife_direct_damage(float reach_unused) {
    const auto lh = re4vr::lua_get_vec3("__vr_lh_world");

    if (!lh.has_value()) {
        return false;
    }

    // Tag fuers Log (Wurf ruft direct_damage_at direkt -> default "wurf").
    re4vr::lua_set_string("__re4_knife_hit_src", "melee");

    // [REACH] Links-Melee-Reichweite = wie das RECHTE Messer
    // (__re4_knife_reach, ~0.9). Vorher fest 1.8 -> traf zu weit entfernte
    // Gegner. Der uebergebene Wert (Lua: 1.8) wird BEWUSST verworfen, genau wie
    // im Original.
    (void)reach_unused;

    const float r = static_cast<float>(re4vr::lua_get_number("__re4_knife_reach", 0.9));

    return knife_direct_damage_at(*lh, r);
}

// ----------------------------------------------------------------------------
// [KLON-TREFFER 2026-07-23] Den EMPFANG nachbauen statt die Anmeldung.
// Die Engine ruft am Ziel onHitDamage(DamageInfo) -- beim Huhn GmChicken, bei
// der haengenden Muenze GmBlueCoin. requestAttack ist nur die Anmeldung und
// wird ohne Beruehrung des Klingen-Colliders nie aufgeloest.
// ----------------------------------------------------------------------------
namespace {

::REManagedObject* find_onhit_receiver(::REManagedObject* go) {
    for (int32_t depth = 0; depth <= 3; ++depth) {
        if (go == nullptr) {
            return nullptr;
        }

        auto* comps = re4vr::call_safe<::REManagedObject*>(go, "get_Components");
        int32_t n = 0;

        if (comps != nullptr) {
            re4vr::try_call<int32_t>(comps, "get_Count", n);
        }

        const int32_t lim = std::min(n, 24);

        for (int32_t i = 0; i < lim; ++i) {
            auto* c =
                re4vr::call_safe<::REManagedObject*>(comps, "get_Item(System.Int32)", i);

            if (!re4vr::obj_ok(c)) {
                continue;
            }

            auto* td = utility::re_managed_object::get_type_definition(c);

            if (td != nullptr && td->get_method("onHitDamage") != nullptr) {
                return c;
            }
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        auto* ptf = tf != nullptr ? re4vr::call_safe<::REManagedObject*>(tf, "get_Parent")
                                  : nullptr;

        go = ptf != nullptr ? re4vr::call_safe<::REManagedObject*>(ptf, "get_GameObject")
                            : nullptr;
    }

    return nullptr;
}

}   // namespace

bool RE4VRWeapons2::knife_onhit_replay(::REManagedObject* target_go,
                                       ::REManagedObject* atk_ud) {
    if (target_go == nullptr) {
        return false;
    }

    auto* recv = find_onhit_receiver(target_go);

    if (recv == nullptr) {
        return false;
    }

    // Das echte Treffer-Objekt der Engine; ohne das wird NICHT gefeuert.
    auto* di = m_dmginfo_raw.obj;

    // [SAVE-LOAD 2026-07-23] Das eingefangene Objekt gehoert der Engine (Pool).
    // Nach einem Save-Load kann der Zeiger tot sein. Deshalb vor jeder Benutzung
    // kurz anfassen -- aber NUR pruefen, ob es ansprechbar ist, NICHT ob ein
    // bestimmtes Feld einen Wert hat (ein nil kam auch bei gueltigen Objekten
    // vor und hat die Vorlage bei jedem Schlag verworfen).
    if (di != nullptr) {
        bool alive = false;

        try {
            alive = utility::re_managed_object::get_type_definition(di) != nullptr;
        } catch (...) {
            alive = false;
        }

        if (!alive) {
            m_dmginfo_raw.obj = nullptr;
            m_dmginfo_raw.reffed = false;
            m_dmginfo_raw_knife = false;
            di = nullptr;
        }
    }

    // [KEIN HOOK NOETIG 2026-07-23] Faellt die eingefangene Vorlage aus, nehmen
    // wir die DamageInfo, die der Ziel-HitController ohnehin besitzt -- erlaubt,
    // seit wir alle veraenderten Felder direkt danach zurueckschreiben.
    if (di == nullptr) {
        di = re4vr::call_safe<::REManagedObject*>(get_hc(target_go), "get_DamageCalcInfo");
    }

    if (di == nullptr) {
        return false;
    }

    // [POOL-SCHUTZ 2026-07-23] `di` ist das GEPOOLTE DamageInfo der Engine.
    // Werte merken, setzen, aufrufen, danach exakt den Vorzustand
    // zurueckschreiben -- sonst arbeiten alle folgenden echten Treffer mit
    // unseren Werten weiter (Symptom: Gegner und Huehner bluten nicht mehr).
    auto* kgo = re4vr::call_safe<::REManagedObject*>(
        re4vr::lua_get_pointer("__re4_knife_atk_hc"), "get_GameObject");

    const float v_damage =
        m_saved_vals.has_value() ? m_saved_vals->damage : 225.0f;
    const float v_wince = m_saved_vals.has_value() ? m_saved_vals->wince : 86.4f;
    const float v_brk = m_saved_vals.has_value() ? m_saved_vals->brk : 1080.0f;
    const float v_stop = m_saved_vals.has_value() ? m_saved_vals->stop : 1008.0f;
    const int32_t v_wid =
        (m_saved_vals.has_value() && m_saved_vals->wid.has_value()) ? *m_saved_vals->wid : 5001;

    std::vector<RawField> saved{};

    const auto put_num = [&](const char* f, double val) {
        auto r = raw_field_open(di, f);

        if (r.valid) {
            saved.push_back(r);
            set_field_num(di, f, val);
        }
    };

    const auto put_obj = [&](const char* f, ::REManagedObject* val) {
        // Lua ueberspringt Eintraege mit nil-Wert (`if f[2] ~= nil`).
        if (val == nullptr) {
            return;
        }

        auto r = raw_field_open(di, f);

        if (r.valid) {
            saved.push_back(r);
            set_field_obj(di, f, val);
        }
    };

    put_num("<Damage>k__BackingField", v_damage);
    put_num("<Wince>k__BackingField", v_wince);
    put_num("<Break>k__BackingField", v_brk);
    put_num("<Stopping>k__BackingField", v_stop);
    put_num("<WeaponID>k__BackingField", static_cast<double>(v_wid));
    put_num("<IsActive>k__BackingField", 1.0);
    put_obj("<DamageGameObject>k__BackingField", target_go);
    put_obj("<WeaponGameObject>k__BackingField", kgo);
    put_obj("<AttackGameObject>k__BackingField", kgo);
    put_obj("<AttackUserData>k__BackingField", atk_ud);

    bool ok = false;

    try {
        re4vr::call_safe<void*>(recv, "onHitDamage", di);
        ok = true;
    } catch (...) {
        ok = false;
    }

    // [SOUND WIE BEIM GEGNER 2026-07-23] Die Engine spielt bei unserem Nachbau
    // nichts -- also denselben Messer-Trefferton feuern.
    if (ok) {
        const bool melee = re4vr::lua_get_string("__re4_knife_hit_src") == "melee";
        double sid = re4vr::lua_get_number("__re4_knife_lh_hit_snd", 238304172.0);

        if (!melee && re4vr::lua_get_tribool("__re4_knife_lh_use_throw_snd") == 1) {
            sid = re4vr::lua_get_number("__re4_knife_lh_throw_hit_snd", 797300665.0);
        }

        if (re4vr::lua_get_tribool("__re4_knife_hit_mute") != 1) {
            knife_lh_play_sound(static_cast<int32_t>(std::floor(sid)));
        }
    }

    // Vorzustand des gepoolten Objekts sofort wiederherstellen.
    //
    // [ORIGINAL-DETAIL] Lua laeuft hier ueber `pairs(saved)`, NICHT `ipairs` --
    // Absicht, weil `saved` Luecken hat (nil-Werte werden uebersprungen). Ein
    // ipairs braeche nach der ersten Luecke ab und liesse Felder des gepoolten
    // Objekts DAUERHAFT verbogen. Hier gibt es keine Luecken, weil nur
    // tatsaechlich geschriebene Felder in den Vektor kommen.
    for (const auto& r : saved) {
        raw_field_restore(r);
    }

    return ok;
}

// [BREAKABLE 2026-07-09] Box-Break-Completion fuer den Klon. break_nearby macht
// set_Routine(Break) = nur Optik; die Engine-Completion kommt sonst per
// requestAttack, das beim Klon mangels Collider NICHT landet.
bool RE4VRWeapons2::knife_hitset_box(::REManagedObject* box_go,
                                     const std::optional<glm::vec3>& pos) {
    if (box_go == nullptr) {
        return false;
    }

    auto* boxhc = get_hc(box_go);

    if (boxhc == nullptr) {
        return false;
    }

    auto* pb = player_body_go_w2();
    auto* atkhc = pb != nullptr ? get_hc(pb) : nullptr;

    if (atkhc == nullptr) {
        return false;
    }

    auto* hm = get_hitmgr();

    if (hm == nullptr) {
        return false;
    }

    std::optional<glm::vec3> cpos{};

    if (pos.has_value()) {
        cpos = glm::vec3{pos->x, pos->y + 0.3f, pos->z};
    }

    float dmg = 0.0f;
    auto* di = build_dmginfo(boxhc, box_go, cpos, dmg);

    if (di == nullptr) {
        return false;
    }

    native_hit(hm, di, atkhc, boxhc);

    // [BREAK SOUND] Der alte hardcodierte trigger auf box_go war stumm (das ist
    // ein Collider-Kind OHNE eigenen SoundContainer). Ersetzt durch
    // clone_break_sound in weapons.lua break_nearby.
    return true;
}

// ---- STEP 1: nativen RECHTS-Messer-Schaden aufnehmen (persistieren) ---------
void RE4VRWeapons2::saved_vals_tick() {
    if (m_saved_vals.has_value()) {
        return;
    }

    auto* cap = m_dmginfo.obj;

    if (cap == nullptr) {
        return;
    }

    float dmg = 0.0f;

    // [PORTFIX 2026-09-06] typrichtig lesen -- das ist der Schaden selbst.
    const auto dmg_o = re4vr::call_num(cap, "get_Damage");
    dmg = dmg_o.has_value() ? static_cast<float>(*dmg_o) : 0.0f;

    if (!dmg_o.has_value() || dmg <= 0.0f) {
        return;
    }

    SavedVals v{};
    v.damage = dmg;

    float f = 0.0f;
    v.wince = static_cast<float>(re4vr::call_num(cap, "get_Wince").value_or(1.0));
    v.brk = static_cast<float>(re4vr::call_num(cap, "get_Break").value_or(1.0));
    v.stop = static_cast<float>(re4vr::call_num(cap, "get_Stopping").value_or(1.0));
    v.wid = enum_as_int(cap, "get_WeaponID");

    save_vals_store(v);
}

// ============================================================================
// Block 5 -- Kanone (Lua Z.3182-3227)
//
// GmCannonV2 auf gm84_572.
//  1) Kanonen-Jack erkennen -> __re4_at_cannon (binding gibt dann den rechten
//     Stick-Y frei).
//  2) [YAW ERWEITERN] GmCannonV2._YawRotateRangeRad (via.Range {s@0xE0, r@0xE4},
//     RAD) begrenzt den Schwenk. Wir schreiben jeden Frame einen weiteren
//     Bereich rein (direkter Write; haelt gegen Engine-Resets).
// ============================================================================
void RE4VRWeapons2::cannon_tick() {
    // Kanone in der Body-Parent-Kette suchen (8 Ebenen).
    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    ::REManagedObject* go = nullptr;
    auto* x = tf;

    for (int32_t i = 0; i < 8; ++i) {
        if (x == nullptr) {
            break;
        }

        auto* g = re4vr::call_safe<::REManagedObject*>(x, "get_GameObject");
        const std::string nm = obj_name_of(g);

        if (nm.find("gm84_572") != std::string::npos) {
            go = g;

            break;
        }

        x = re4vr::call_safe<::REManagedObject*>(x, "get_Parent");
    }

    re4vr::lua_set_bool("__re4_at_cannon", go != nullptr);

    if (go == nullptr) {
        drop(m_c_go);
        drop(m_c_cc);

        return;
    }

    if (m_c_go.obj != go) {
        store(m_c_go, go);

        auto* cc = re4vr::get_component(go, "chainsaw.GmCannonV2");

        drop(m_c_cc);

        if (cc != nullptr) {
            store(m_c_cc, cc);
        }
    }

    // [KS_GLOBAL 2026-07-15] Killswitch -> NICHTS in die Engine schreiben.
    // Bewusst ERST HIER: die Kanonen-Suche + __re4_at_cannon oben sind
    // STATE-PFLEGE, die andere Scripte lesen -- die muss weiterlaufen, sonst
    // haengt das Flag im KS auf einem alten Wert.
    // (Regel: nur die AUSGABE gaten, nicht die Zustandsermittlung.)
    if (re4vr::lua_get_tribool("__re4_ks_active") == 1) {
        return;
    }

    if (m_c_cc.obj == nullptr) {
        return;
    }

    // [MK.II AUSNEHMEN 2026-07-11] Die zweite Kanone (gm84_572_00_1) NICHT
    // anfassen -> ihre native Sektor-Restriktion bleibt unberuehrt (sonst kein
    // Links-Schwenk / Aussteig-Problem).
    if (re4vr::lua_get_tribool("__re4_cannon_yaw_widen") == 0) {
        return;
    }

    const std::string cnm = obj_name_of(go);

    if (cnm.find("gm84_572_00_1") != std::string::npos) {
        return;
    }

    const float mn = static_cast<float>(re4vr::lua_get_number("__re4_cannon_yaw_min", -3.05));
    const float mx = static_cast<float>(re4vr::lua_get_number("__re4_cannon_yaw_max", 3.05));

    // via.Range in GmCannonV2 @ 0xE0 (s) / 0xE4 (r) direkt ueberschreiben.
    try {
        const auto base = reinterpret_cast<uintptr_t>(m_c_cc.obj);
        *reinterpret_cast<float*>(base + 0xE0) = mn;
        *reinterpret_cast<float*>(base + 0xE4) = mx;
    } catch (...) {
    }
}

// ============================================================================
// Block 6 -- Unlimited + Katzenohren (Lua Z.3260-3437)
// ============================================================================

// Live equippte WeaponItem (gleicher Pfad wie reload.lua).
::REManagedObject* RE4VRWeapons2::get_equipped_wi() {
    auto* pe = get_pe_w2();

    if (pe == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeaponItem");
}

// ERKENNUNG (waffen-agnostisch, per Live-Log verifiziert 2026-07-02):
// WeaponItem.get_IsBulletFull == true <=> unlimited.
bool RE4VRWeapons2::is_unlimited() {
    const double now = clock_now();

    if ((now - m_unlim_c.t) < 0.10) {
        return m_unlim_c.val;
    }

    m_unlim_c.t = now;

    auto* wi = get_equipped_wi();
    bool v = false;

    m_unlim_c.val = wi != nullptr && re4vr::try_call<bool>(wi, "get_IsBulletFull", v) && v;

    return m_unlim_c.val;
}

// [KATZENOHREN / INFINITE RESERVE 2026-08-20]
// Ein getragenes Accessoire haengt als eigenes GO DIREKT am Body und heisst
// "ac####_##". Beim Wechsel wird das GO NEU erzeugt -> niemals den Zeiger
// merken, immer frisch ueber den Namen.
std::string RE4VRWeapons2::accessory_now() {
    const double now = clock_now();

    if ((now - m_acc_c.t) < 0.25) {
        return m_acc_c.id;
    }

    m_acc_c.t = now;
    m_acc_c.id.clear();

    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return m_acc_c.id;
    }

    auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

    // [KEINE WILDCARDS 2026-08-27] Frueher stand hier ein `break` beim ERSTEN
    // Knoten, der auf ac####_## passte -- also bei irgendeinem Accessoire. Am
    // Body haengt aber mehr als eines, und die Kind-Reihenfolge wechselt
    // staendig. Gemessen am 27.08.: um 09:42 kam ac3900_10 heraus, um 09:52
    // ac0000_00 -- dieselben Ohren, dasselbe Spiel.
    // Jetzt zaehlen NUR die Katzenohren; nur wenn gar keine dabei sind, wird
    // ersatzweise das erste gefundene gemeldet (rein informativ).
    std::string erstes{};

    while (ch != nullptr) {
        auto* go = re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject");
        const std::string nm = obj_name_of(go);

        // Lua: nm:match("^ac%d%d%d%d_%d%d$")
        bool shape = nm.size() == 9 && nm[0] == 'a' && nm[1] == 'c' && nm[6] == '_';

        if (shape) {
            for (const int i : {2, 3, 4, 5, 7, 8}) {
                if (!std::isdigit(static_cast<unsigned char>(nm[i]))) {
                    shape = false;

                    break;
                }
            }
        }

        if (shape) {
            // [ADA 2026-08-21] Leon = ac1300_10, Ada = ac3900_10. Mit nur Leons
            // ID hat die Regel bei Ada nie angeschlagen.
            if (nm == "ac1300_10" || nm == "ac3900_10") {
                m_acc_c.id = nm;

                return m_acc_c.id;
            }

            if (erstes.empty()) {
                erstes = nm;
            }
        }

        ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
    }

    m_acc_c.id = erstes;

    return m_acc_c.id;
}

bool RE4VRWeapons2::infinite_reserve() {
    const std::string a = accessory_now();

    return a == "ac1300_10" || a == "ac3900_10";
}

// [NUR DIE EIGENE SORTE 2026-08-20] Liefert die Munitionssorte, die die
// equippte Waffe GERADE benutzt -- nur fuer die gilt "unendlich".
// Rueckgabe: (string, zahl) -- beide Formen, weil reload.lua an einer Stelle
// ueber den String und an einer ueber die Zahl vergleicht.
// Zwischengespeichert werden NUR String und Zahl, NIE das Enum-Objekt selbst
// (ValueType-Rueckgaben sind kurzlebige Puffer).
bool RE4VRWeapons2::infinite_ammo_id(std::string& s_out, std::optional<double>& n_out) {
    const double now = clock_now();

    if ((now - m_amid.t) < 0.25) {
        s_out = m_amid.s;
        n_out = m_amid.n;

        return m_amid.valid;
    }

    m_amid.t = now;
    m_amid.s.clear();
    m_amid.n.reset();
    m_amid.valid = false;

    // [KEIN UNENDLICH TROTZ OHREN 2026-09-02 -- GEMESSEN] Waffen, denen das
    // Spiel AUCH mit Katzenohren keine unendliche Munition gibt.
    //   4600 Bolt Thrower / Armbrust (Leon)
    //   4701 Flamethrower (Easter-Egg-Waffe)
    {
        auto* ctx = get_ctx_w2();
        auto* hu = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
            : nullptr;
        const auto ew = enum_as_int(hu, "get_EquipWeaponID");

        if (ew.has_value() && (*ew == 4600 || *ew == 4701)) {
            // nil, und zwar MITGECACHT
            s_out.clear();
            n_out.reset();

            return false;
        }
    }

    auto* wi = get_equipped_wi();
    // [ENUM 05.09.2026] get_CurrentAmmo liefert einen ENUM. REFramework gibt
    // den nach Lua als ZAHL zurueck -- deshalb versuchte __re4_id_num zuerst
    // tonumber(tostring(x)). Als Objektzeiger gelesen ist der Wert Muell (und
    // ein Deref darauf gefaehrlich). Alle anderen Fundstellen im Fork
    // (Reload2/3/4/5/Main) nehmen dafuer enum_as_int -- diese hier war die
    // einzige Ausnahme.
    const auto aid_num = wi != nullptr ? enum_as_int(wi, "get_CurrentAmmo")
                                       : std::optional<int32_t>{};

    if (aid_num.has_value()) {
        m_amid.s = std::to_string(*aid_num);
        m_amid.n = static_cast<double>(*aid_num);
        m_amid.valid = true;

        // [WAISE 05.09.2026] Hier stand zusaetzlich ein Aufruf von __re4_id_num
        // (aus reload.lua, mit dessen Port verschwunden -- er lief ins Leere und
        // m_amid.n blieb IMMER leer). Die Lua-Funktion machte nichts anderes,
        // als aus dem Enum eine Zahl zu holen; enum_as_int oben liefert die
        // direkt, der Umweg entfaellt damit ganz.
    }

    s_out = m_amid.s;
    n_out = m_amid.n;

    return m_amid.valid;
}

// (2)+(3) Pro Frame die Lock-Flags setzen. Laeuft als LETZTER on_frame ->
// gewinnt ueber die grab_empty-Zuweisungen der reload-Files.
//
// KEIN KILLSWITCH-GATE -- ABSICHT, nicht vergessen: dieser Tick IST nur
// State-Pflege (er schreibt nichts in die Engine, nur Lua-Globals). Ein
// KS-Gate wuerde die Flags EINFRIEREN statt sie zu pflegen -> Reload haengt
// nach dem Killswitch. Das unterscheidet ihn von der Kanone oben.
void RE4VRWeapons2::unlimited_tick() {
    const bool u = is_unlimited();

    re4vr::lua_set_bool("__re4_block_b_drop", u);

    if (u) {
        re4vr::lua_set_bool("__re4_reload_grab_empty", true);
    }
}

// ============================================================================
// Block 7 -- PARRY_KEEP_GUN (Lua Z.3463-3500)
//
// Nach einem Parry mit dem KLON-MESSER LINKS soll die vorher rechts gehaltene
// Schusswaffe zurueck in die Hand.
// LOG-BEFUND: die Schusswaffe wird NIE weggenommen -- sie ist nach dem Parry
// blos nicht mehr AKTIV in der Hand (get_EquipWeaponID = -1). Deshalb ist der
// richtige Aufruf `requestEquipGun` und NICHT equipWeapon.
// ============================================================================
void RE4VRWeapons2::parry_keep_gun_tick() {
    // [PARRY-SOUND LINKS 16.09.2026] Vom Parry-Hook vorgemerkt -> hier einmal
    // abspielen, ueber denselben Weg wie Wurf/Flip/Treffer des linken Messers.
    if (g_lh_parry_sound_pending.exchange(false)) {
        knife_lh_play_sound(static_cast<int32_t>(LH_PARRY_SOUND_ID));
    }

    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return;
    }

    auto* ctxr = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef()");

    if (ctxr == nullptr) {
        return;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctxr, "get_HeadUpdater");

    if (hu == nullptr) {
        return;
    }

    const auto wv = enum_as_int(hu, "get_EquipWeaponID");
    const int32_t wn = wv.value_or(-1);

    // Fortlaufend die zuletzt gehaltene NICHT-Messer-Waffe mitschreiben.
    // -1 = "gar nichts" wird bewusst NICHT gemerkt.
    if (wn > 0 && !is_knife_id(wn)) {
        re4vr::lua_set_number("__re4_parry_last_gun_wid", static_cast<double>(wn));
    }

    const auto ku = re4vr::lua_get_number_opt("__re4_parry_keep_gun_until");

    if (!ku.has_value()) {
        return;
    }

    const double now = clock_now();

    // Sobald die Lage stabil ist, sofort zurueckholen -- nicht bis zum Timeout
    // warten. Stabil = fruehester Zeitpunkt erreicht UND Messer nicht mehr
    // equippt UND nichts in der Hand. Das Timeout bleibt als Notbremse.
    const double kf = re4vr::lua_get_number("__re4_parry_keep_gun_from", 0.0);

    // [PARRY_HEAL 2026-09-24 -- Tester-Sonde re4_ammo_tester_sonde (1).txt]
    // Seit dem Equip-Riegel (RE4VRWeapons::cb_equip_weapon, 13.09.) bleibt die
    // Schusswaffe beim Parry IN der Hand (get_EquipWeaponID = 4000), das
    // Inventar fuehrt aber das Messer (Accessor-Item 5000). Folge: HUD 0,
    // Nachladen bucht falsch, bis weggesteckt wird. Diese Lage heisst hier
    // "mismatch" und wird wie "nichts in der Hand" behandelt.
    const auto acc_wid = [&]() -> int32_t {
        auto* pe = re4vr::fc::pe();
        auto* acc = (pe != nullptr)
            ? re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeaponAccessor") : nullptr;
        auto* it = (acc != nullptr) ? re4vr::call_safe<::REManagedObject*>(acc, "get_Item") : nullptr;
        return (it != nullptr) ? enum_as_int(it, "get_WeaponId").value_or(-1) : -1;
    };
    const bool mismatch = wn > 0 && !is_knife_id(wn) && is_knife_id(acc_wid());

    const bool stable = now >= kf
        && re4vr::lua_get_tribool("__re4_knife_equipped") != 1 && (wn <= 0 || mismatch);

    if (!stable && now < *ku) {
        return;
    }

    // ab hier genau EIN Versuch
    re4vr::lua_set_nil("__re4_parry_keep_gun_until");
    re4vr::lua_set_nil("__re4_parry_keep_gun_from");

    if (re4vr::lua_get_tribool("__re4_knife_equipped") == 1) {
        return;
    }

    if (wn > 0 && !mismatch) {
        return;   // es haelt schon wieder etwas (und das Inventar stimmt) -> Finger weg
    }

    if (!mismatch && !re4vr::lua_get_number_opt("__re4_parry_last_gun_wid").has_value()) {
        return;
    }

    auto* eq = re4vr::call_safe<::REManagedObject*>(hu, "get_Equipment");

    if (eq == nullptr) {
        return;
    }

    // [PARRY_HEAL] Eigener Wechsel -> am Equip-Riegel vorbei (sonst blockt er
    // genau diesen Rueckholversuch, wie jeden Wechsel des Spiels).
    re4vr::lua_set_number("__re4_our_equip_until", now + 0.5);
    re4vr::call_safe<void*>(eq, "requestEquipGun");
}

// ============================================================================
// Block 4 -- Wild West Twirl (Lua Z.2354-3163)
//
// Anhaltende AUF-AB-Wippe der Waffenhand (rechts) -> die PISTOLE dreht sich um
// die Pitch-Achse um den TRIGGER-JOINT, plus eine HAND-POSE, die mit dem Spin
// reinlerpt. Nur Pistolen, nur wenn NICHT gezielt.
// ============================================================================

namespace {

constexpr const char* WW_JSON_PATH = "re4_vr/re4_vr_wildwest.json";

// Trigger-Joint im WAFFEN-Skelett heisst "_03" -- NICHT "joint_03".
// getJointByName("joint_03") gab null -> Pivot wurde nie berechnet.
constexpr const char* TRIGGER_JOINT = "_03";

bool is_pistol(int32_t wid) {
    switch (wid) {
    case 4000:
    case 4001:
    case 4002:
    case 4003:
    case 4004:
    case 4500:
    case 4501:
    case 4502:
    case 6000:
    case 6103:
    case 6112:
    case 6113:
    case 6300:   // [SW/MC] 6112 Punisher MC, 6300 Mercenaries-DLC
        return true;
    default:
        return false;
    }
}

constexpr std::array<const char*, 15> ALL_JOINTS{
    "R_Thumb1",  "R_Thumb2",  "R_Thumb3",  "R_IndexF1", "R_IndexF2",
    "R_IndexF3", "R_MiddleF1", "R_MiddleF2", "R_MiddleF3", "R_RingF1",
    "R_RingF2",  "R_RingF3",  "R_PinkyF1", "R_PinkyF2", "R_PinkyF3"};

// Alte per-Finger-Keys -> die 3 Joints (fuer Migration alter JSON).
const std::array<std::pair<const char*, std::array<const char*, 3>>, 5> FINGER_OLD_MAP{{
    {"thumb", {"R_Thumb1", "R_Thumb2", "R_Thumb3"}},
    {"index", {"R_IndexF1", "R_IndexF2", "R_IndexF3"}},
    {"middle", {"R_MiddleF1", "R_MiddleF2", "R_MiddleF3"}},
    {"ring", {"R_RingF1", "R_RingF2", "R_RingF3"}},
    {"pinky", {"R_PinkyF1", "R_PinkyF2", "R_PinkyF3"}},
}};

constexpr float BOB_GAP = 0.5f;
constexpr float BLEND_SPD = 8.0f;   // Finger-Pose Ein/Ausblend-Tempo (~0.15s)

constexpr int32_t TWIRL_SND = 288425943;

// [STOCK-SPERRE 2026-08-12] Nur DIESE ItemIDs sind Schulterstuetzen (live
// gemessen). Alles andere am Lauf (Laser usw.) laesst den Twirl in Ruhe.
bool is_stock_item(int32_t id) {
    return id == 116001600   // Red9   (4002)
        || id == 116009600;  // Matilda(4004)
}

// [TWIRL-SPRUCH 2026-08-04] Leon kommentiert seine Show -- fest verdrahtet.
constexpr std::array<int32_t, 3> TWIRL_VOICE{
    1517954045,   // "not bad, right?"
    2778708114,   // "looking good"
    3878582777,   // "not bad"
};

constexpr float TWIRL_VOICE_MIN = 1.5f;   // s am Stueck aktiv gedreht
constexpr int32_t TWIRL_VOICE_EVERY = 7;  // und dann nur jeder 7.
constexpr int32_t TWIRL_VOICE_END = 3778512958;   // "that was not easy"
constexpr int32_t TWIRL_VOICE_END_EVERY = 11;
constexpr const char* TWIRL_VOICE_BODY = "ch0a0z0_body";

// Quaternion aus additivem Euler (Grad). Reihenfolge X*Y*Z.
glm::quat axis_q(float deg, float ax, float ay, float az) {
    const float h = glm::radians(deg) * 0.5f;
    const float s = std::sin(h);

    return glm::quat{std::cos(h), s * ax, s * ay, s * az};
}

bool euler_add(float dx, float dy, float dz, glm::quat& out) {
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) {
        return false;
    }

    out = axis_q(dx, 1, 0, 0) * axis_q(dy, 0, 1, 0) * axis_q(dz, 0, 0, 1);

    return true;
}

::REManagedObject* head_updater_w2() {
    auto* ctx = get_ctx_w2();

    return ctx != nullptr ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                          : nullptr;
}

::REManagedObject* weap_transform(::REManagedObject* hu) {
    auto* weap = hu != nullptr ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
                               : nullptr;
    auto* go = weap != nullptr ? re4vr::call_safe<::REManagedObject*>(weap, "get_GameObject")
                               : nullptr;

    return go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform") : nullptr;
}

}   // namespace


namespace {

// Lua: `_G.x = (_G.x ~= false)` -- nil ODER true ergibt true, nur explizites
// false bleibt false. Setzt IMMER, wie das Original.
void seed_bool_not_false(const char* name) {
    re4vr::lua_set_bool(name, re4vr::lua_get_tribool(name) != 0);
}

// Lua: `_G.x = (_G.x == true)` -- nur explizites true ergibt true.
void seed_bool_is_true(const char* name) {
    re4vr::lua_set_bool(name, re4vr::lua_get_tribool(name) == 1);
}

// Lua: `_G.x = tonumber(_G.x) or <def>`
void seed_number(const char* name, double def) {
    re4vr::lua_set_number(name, re4vr::lua_get_number(name, def));
}

}   // namespace

void RE4VRWeapons2::ww_save_cfg() {
    nlohmann::json t{};

    t["enabled"] = re4vr::lua_get_bool("__re4_ww_enabled", true);
    t["sustain"] = re4vr::lua_get_number("__re4_ww_sustain", 0.60);
    t["sens"] = re4vr::lua_get_number("__re4_ww_sens", 1.20);
    t["speed"] = re4vr::lua_get_number("__re4_ww_speed", 720.0);
    t["dir"] = re4vr::lua_get_number("__re4_ww_dir", 1.0);

    t["pivot"]["x"] = re4vr::lua_get_number("__re4_ww_pivot_x", 0.0);
    t["pivot"]["y"] = re4vr::lua_get_number("__re4_ww_pivot_y", 0.0);
    t["pivot"]["z"] = re4vr::lua_get_number("__re4_ww_pivot_z", 0.0);

    t["offset"]["x"] = re4vr::lua_get_number("__re4_ww_off_x", 0.0);
    t["offset"]["y"] = re4vr::lua_get_number("__re4_ww_off_y", 0.0);
    t["offset"]["z"] = re4vr::lua_get_number("__re4_ww_off_z", 0.0);

    t["sound"] = re4vr::lua_get_bool("__re4_ww_sound", true);
    t["snd_interval"] = re4vr::lua_get_number("__re4_ww_snd_interval", 0.12);

    // [RT_GATE]
    t["rt_gate"] = re4vr::lua_get_bool("__re4_ww_rt_gate", true);
    t["rt_sens"] = re4vr::lua_get_number("__re4_ww_rt_sens", 0.10);
    t["rt_lockout"] = re4vr::lua_get_number("__re4_ww_rt_lockout", 1.0);

    for (const auto& [jn, g] : m_fing) {
        t["joints"][jn]["x"] = g.x;
        t["joints"][jn]["y"] = g.y;
        t["joints"][jn]["z"] = g.z;
    }

    // [PER-WAFFE KEYFRAMES] String-Keys fuer JSON (numerische wid-Keys wuerden
    // als Sparse-Array dumpen).
    nlohmann::json okw = nlohmann::json::object();

    for (const auto& [wid, arr] : m_okeys) {
        nlohmann::json a = nlohmann::json::array();

        for (const auto& kf : arr) {
            nlohmann::json e{};
            e["a"] = kf.a;
            e["x"] = kf.x;
            e["y"] = kf.y;
            e["z"] = kf.z;
            a.push_back(e);
        }

        okw[std::to_string(wid)] = a;
    }

    t["okeys_by_wid"] = okw;

    re4vr::json_save(WW_JSON_PATH, t);
}

void RE4VRWeapons2::ww_load_cfg() {
    // ---- Defaults (Lua setzt sie als Globals, bevor load_cfg laeuft) ----
    for (const char* jn : ALL_JOINTS) {
        m_fing[jn] = glm::vec3{0.0f};
    }

    seed_bool_not_false("__re4_ww_enabled");
    seed_number("__re4_ww_sustain", 0.60);
    seed_number("__re4_ww_sens", 1.20);
    seed_bool_not_false("__re4_ww_rt_gate");
    seed_number("__re4_ww_rt_sens", 0.10);
    seed_number("__re4_ww_rt_lockout", 1.0);
    seed_number("__re4_ww_speed", 720.0);
    seed_number("__re4_ww_dir", 1.0);
    seed_number("__re4_ww_pivot_x", 0.0);
    seed_number("__re4_ww_pivot_y", 0.0);
    seed_number("__re4_ww_pivot_z", 0.0);
    seed_number("__re4_ww_off_x", 0.0);
    seed_number("__re4_ww_off_y", 0.0);
    seed_number("__re4_ww_off_z", 0.0);
    seed_number("__re4_ww_prev_angle", 90.0);
    seed_bool_is_true("__re4_ww_prev_interp");
    seed_bool_not_false("__re4_ww_sound");
    seed_number("__re4_ww_snd_interval", 0.12);
    seed_bool_not_false("__re4_ww_block_with_stock");

    const auto d = re4vr::json_load(WW_JSON_PATH);

    if (!d.is_object()) {
        return;
    }

    const auto b = [&](const char* key, const char* global) {
        if (const auto it = d.find(key); it != d.end() && !it->is_null()) {
            re4vr::lua_set_bool(global, it->is_boolean() && it->get<bool>());
        }
    };
    const auto n = [&](const char* key, const char* global) {
        if (const auto it = d.find(key); it != d.end() && it->is_number()) {
            re4vr::lua_set_number(global, it->get<double>());
        }
    };

    b("enabled", "__re4_ww_enabled");
    b("sound", "__re4_ww_sound");
    n("snd_interval", "__re4_ww_snd_interval");
    n("sustain", "__re4_ww_sustain");
    n("sens", "__re4_ww_sens");
    b("rt_gate", "__re4_ww_rt_gate");
    n("rt_sens", "__re4_ww_rt_sens");
    n("rt_lockout", "__re4_ww_rt_lockout");
    n("speed", "__re4_ww_speed");
    n("dir", "__re4_ww_dir");

    if (const auto p = d.find("pivot"); p != d.end() && p->is_object()) {
        const auto g = [&](const char* k) -> double {
            const auto it = p->find(k);

            return (it != p->end() && it->is_number()) ? it->get<double>() : 0.0;
        };

        re4vr::lua_set_number("__re4_ww_pivot_x", g("x"));
        re4vr::lua_set_number("__re4_ww_pivot_y", g("y"));
        re4vr::lua_set_number("__re4_ww_pivot_z", g("z"));
    }

    if (const auto p = d.find("offset"); p != d.end() && p->is_object()) {
        const auto g = [&](const char* k) -> double {
            const auto it = p->find(k);

            return (it != p->end() && it->is_number()) ? it->get<double>() : 0.0;
        };

        re4vr::lua_set_number("__re4_ww_off_x", g("x"));
        re4vr::lua_set_number("__re4_ww_off_y", g("y"));
        re4vr::lua_set_number("__re4_ww_off_z", g("z"));
    }

    const auto read_keys = [](const nlohmann::json& arr) {
        std::vector<OKey> t2{};

        for (const auto& kf : arr) {
            if (!kf.is_object()) {
                continue;
            }

            const auto a = kf.find("a");

            if (a == kf.end() || !a->is_number()) {
                continue;
            }

            const auto g = [&](const char* k) -> float {
                const auto it = kf.find(k);

                return (it != kf.end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            t2.push_back(OKey{a->get<float>(), g("x"), g("y"), g("z")});
        }

        std::sort(t2.begin(), t2.end(),
                  [](const OKey& p, const OKey& q) { return p.a < q.a; });

        return t2;
    };

    if (const auto ok = d.find("okeys_by_wid"); ok != d.end() && ok->is_object()) {
        m_okeys.clear();

        for (const auto& [k, arr] : ok->items()) {
            if (!arr.is_array()) {
                continue;
            }

            m_okeys[std::atoi(k.c_str())] = read_keys(arr);
        }
    } else if (const auto o = d.find("okeys"); o != d.end() && o->is_array()) {
        // [MIGRATION] alte flache Keyframes -> Blacktail (4003)
        m_okeys[4003] = read_keys(*o);
    }

    if (const auto j = d.find("joints"); j != d.end() && j->is_object()) {
        // neues Pro-Joint-Format
        for (auto& [jn, g] : m_fing) {
            const auto s = j->find(jn);

            if (s == j->end() || !s->is_object()) {
                continue;
            }

            const auto rd = [&](const char* k) -> float {
                const auto it = s->find(k);

                return (it != s->end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            g = glm::vec3{rd("x"), rd("y"), rd("z")};
        }
    } else if (const auto fj = d.find("fingers"); fj != d.end() && fj->is_object()) {
        // Migration: altes per-Finger -> alle 3 Joints
        for (const auto& [fn, joints] : FINGER_OLD_MAP) {
            const auto s = fj->find(fn);

            if (s == fj->end() || !s->is_object()) {
                continue;
            }

            const auto rd = [&](const char* k) -> float {
                const auto it = s->find(k);

                return (it != s->end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            const glm::vec3 v{rd("x"), rd("y"), rd("z")};

            for (const char* jn : joints) {
                if (m_fing.find(jn) != m_fing.end()) {
                    m_fing[jn] = v;
                }
            }
        }
    }
}

// [STOCK-SPERRE 2026-08-12] Mit montiertem Stock ergibt der Twirl keinen Sinn.
// Gesperrt wird AUSSCHLIESSLICH ueber die beiden gemessenen STOCK-ItemIDs --
// `isPartsEquipped()` allein war FALSCH: ein montierter LASER ist ebenfalls ein
// Part und hat den Twirl komplett gesperrt.
// Gelesen wird auf der ECHTEN Instanz ueber den Accessor.
bool RE4VRWeapons2::stock_mounted(std::optional<int32_t> wid) {
    if (re4vr::lua_get_tribool("__re4_ww_block_with_stock") == 0) {
        return false;
    }

    const double now = clock_now();

    if (m_stock_c.wid == wid && (now - m_stock_c.t) < 0.25) {
        return m_stock_c.on;
    }

    m_stock_c.wid = wid;
    m_stock_c.t = now;
    m_stock_c.on = false;

    auto* pe = get_pe_w2();
    auto* acc = pe != nullptr
        ? re4vr::call_safe<::REManagedObject*>(pe, "getEquipWeaponAccessor")
        : nullptr;
    auto* wi = acc != nullptr ? re4vr::call_safe<::REManagedObject*>(acc, "get_Item") : nullptr;

    if (wi == nullptr) {
        return m_stock_c.on;
    }

    // Erst wenn ueberhaupt ein Part dran ist, die ID holen (ohne Part -1).
    bool eqp = false;

    if (!re4vr::try_call<bool>(wi, "isPartsEquipped", eqp) || !eqp) {
        return m_stock_c.on;
    }

    const auto pid = enum_as_int(wi, "getEquippedPartsItemId");

    // Gesperrt wird NUR bei einer bekannten Stock-ID. Laser/sonstige Parts (und
    // alles Unlesbare) gelten als "kein Stock".
    m_stock_c.on = pid.has_value() && is_stock_item(*pid);

    return m_stock_c.on;
}

std::optional<glm::vec3> RE4VRWeapons2::compute_local_pivot(::REManagedObject* T) {
    glm::vec3 O{};
    glm::quat Q{};

    if (!get_vec3(T, "get_Position", O) || !get_quat(T, "get_Rotation", Q)) {
        return std::nullopt;
    }

    auto* jt = joint_by_name(T, TRIGGER_JOINT);

    if (jt == nullptr) {
        return std::nullopt;
    }

    glm::vec3 J{};

    if (!get_vec3(jt, "get_Position", J)) {
        return std::nullopt;
    }

    return glm::conjugate(Q) * (J - O);
}

// Interpolierter Waffen-Offset am Salto-Fortschritt (Grad 0..360, wrap-around).
bool RE4VRWeapons2::offset_at(float phase, glm::vec3& out) {
    if (m_cur_okeys == nullptr) {
        return false;
    }

    const auto& K = *m_cur_okeys;
    const size_t n = K.size();

    if (n == 0) {
        return false;
    }

    phase = std::fmod(phase, 360.0f);

    if (phase < 0.0f) {
        phase += 360.0f;
    }

    if (n == 1) {
        out = glm::vec3{K[0].x, K[0].y, K[0].z};

        return true;
    }

    const OKey* lo = nullptr;
    const OKey* hi = nullptr;

    for (size_t i = 0; i < n; ++i) {
        if (K[i].a <= phase) {
            lo = &K[i];
        } else {
            break;
        }
    }

    for (size_t i = n; i-- > 0;) {
        if (K[i].a >= phase) {
            hi = &K[i];
        } else {
            break;
        }
    }

    if (lo != nullptr && hi != nullptr && lo == hi) {
        out = glm::vec3{lo->x, lo->y, lo->z};

        return true;
    }

    float t = 0.0f;

    if (lo == nullptr || hi == nullptr) {
        // phase liegt vor dem ersten / nach dem letzten -> Wrap 360->0
        lo = &K[n - 1];
        hi = &K[0];

        const float span = K[0].a + 360.0f - K[n - 1].a;
        const float dd = phase >= K[n - 1].a ? phase - K[n - 1].a
                                             : phase + 360.0f - K[n - 1].a;
        t = span > 0.0001f ? dd / span : 0.0f;
    } else {
        const float span = hi->a - lo->a;
        t = span > 0.0001f ? (phase - lo->a) / span : 0.0f;
    }

    t = std::clamp(t, 0.0f, 1.0f);

    out = glm::vec3{lo->x + (hi->x - lo->x) * t, lo->y + (hi->y - lo->y) * t,
                    lo->z + (hi->z - lo->z) * t};

    return true;
}

// Keyframe an Fortschritt `phase` anlegen/ersetzen (Toleranz 4 Grad).
void RE4VRWeapons2::save_keyframe(float phase, const glm::vec3& off) {
    if (m_cur_okeys == nullptr) {
        return;
    }

    phase = std::fmod(phase, 360.0f);

    if (phase < 0.0f) {
        phase += 360.0f;
    }

    for (auto& k : *m_cur_okeys) {
        if (std::abs(k.a - phase) < 4.0f) {
            k.x = off.x;
            k.y = off.y;
            k.z = off.z;
            ww_save_cfg();

            return;
        }
    }

    m_cur_okeys->push_back(OKey{phase, off.x, off.y, off.z});

    std::sort(m_cur_okeys->begin(), m_cur_okeys->end(),
              [](const OKey& p, const OKey& q) { return p.a < q.a; });

    ww_save_cfg();
}

// Twirl-SOUND: One-Shot, waehrend der Drehung im Intervall neu getriggert.
void RE4VRWeapons2::ww_sound_tick(::REManagedObject* hu, double now) {
    if (re4vr::lua_get_tribool("__re4_ww_sound") == 0) {
        return;
    }

    if (now < m_twirl_snd.next_t) {
        return;
    }

    auto* T = weap_transform(hu);
    auto* go = T != nullptr ? re4vr::call_safe<::REManagedObject*>(T, "get_GameObject")
                            : nullptr;
    auto* scn = go != nullptr ? re4vr::get_component(go, "soundlib.SoundContainer") : nullptr;

    if (scn == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", static_cast<uint32_t>(TWIRL_SND));

    m_twirl_snd.next_t =
        now + std::max(0.03, re4vr::lua_get_number("__re4_ww_snd_interval", 0.12));
}

// Die Lines liegen auf LEONS BODY-Container -- der Waffen-Container kennt sie
// nicht. Heisst der Body anders (Ada, Mercs), passiert gar nichts.
// Kein Komponenten-Cache (haelt Savegame-Loads aus).
::REManagedObject* RE4VRWeapons2::twirl_voice_container() {
    auto* go = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();

    if (go == nullptr || obj_name_of(go) != TWIRL_VOICE_BODY) {
        return nullptr;   // nur Leon
    }

    return re4vr::get_component(go, "soundlib.SoundContainer");
}

// Laeuft WAEHREND der Drehung mit: schlaegt genau in dem Frame zu, in dem die
// Mindestdauer voll ist.
void RE4VRWeapons2::twirl_voice_tick() {
    if (m_tvoice.fired || !m_tvoice.t0.has_value()) {
        return;
    }

    if (m_tvoice.dur < TWIRL_VOICE_MIN) {
        return;
    }

    // pro Twirl nur EINE Pruefung, egal wie lange weitergedreht wird
    m_tvoice.fired = true;
    ++m_tvoice.count;

    if ((m_tvoice.count % TWIRL_VOICE_EVERY) != 0) {
        return;
    }

    auto* scn = twirl_voice_container();

    if (scn == nullptr) {
        return;
    }

    // Zufaellig, aber nie zweimal derselbe hintereinander: aus n-1 ziehen und
    // den letzten ueberspringen.
    const int32_t n = static_cast<int32_t>(TWIRL_VOICE.size());
    int32_t li = 0;

    for (int32_t i = 0; i < n; ++i) {
        if (TWIRL_VOICE[i] == m_tvoice.last_id) {
            li = i + 1;   // Lua ist 1-basiert
        }
    }

    int32_t pick;

    if (n > 1 && li > 0) {
        pick = 1 + (std::rand() % (n - 1));

        if (pick >= li) {
            ++pick;
        }
    } else {
        pick = 1 + (std::rand() % n);
    }

    const int32_t id = TWIRL_VOICE[static_cast<size_t>(pick - 1)];

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)", static_cast<uint32_t>(id));
    m_tvoice.last_id = id;
}

// Laeuft beim PARKEN: der Abschluss-Kommentar. tvoice.fired ist genau dann
// true, wenn dieser Twirl die Mindestdauer geschafft hat.
void RE4VRWeapons2::twirl_voice_end() {
    if (!m_tvoice.fired) {
        return;
    }

    ++m_tvoice.end_count;

    if ((m_tvoice.end_count % TWIRL_VOICE_END_EVERY) != 0) {
        return;
    }

    auto* scn = twirl_voice_container();

    if (scn == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(scn, "trigger(System.UInt32)",
                            static_cast<uint32_t>(TWIRL_VOICE_END));
}

void RE4VRWeapons2::ww_reset_bob() {
    m_prev_vsign = 0;
    m_prev_vsign_t = 0.0;
    m_bob_started.reset();
    m_last_flip_t = 0.0;
}

void RE4VRWeapons2::ww_reset_all() {
    m_spin.active = false;
    m_spin.finishing = false;
    m_spin.deg = 0.0f;
    m_spin.by_rt = false;

    re4vr::lua_set_number("__re4_wildwest_angle", 0.0);
    re4vr::lua_set_number("__re4_wildwest_progress", 0.0);

    m_finger_blend = 0.0f;
    re4vr::lua_set_number("__re4_wildwest_finger_blend", 0.0);

    m_prev_y.reset();
    m_active_lp.reset();
    ww_reset_bob();

    // Re-Triggern stoppen; naechster Spin spielt sofort.
    m_twirl_snd.next_t = 0.0;
}

// [RT_GATE 2026-07-21] Ist der Direkt-Twirl gerade freigeschaltet? Reine
// LESE-Abfrage, greift nirgends in RT ein.
bool RE4VRWeapons2::ww_rt_gate() {
    if (re4vr::lua_get_tribool("__re4_ww_rt_gate") == 0) {
        return false;
    }

    // Schuss-Zaehler beobachten (laeuft IMMER, auch wenn das Gate zu ist)
    if (const auto seq = re4vr::lua_get_number_opt("__vr_shot_seq"); seq.has_value()) {
        if (!m_last_shot_seq.has_value() || *seq != *m_last_shot_seq) {
            if (m_last_shot_seq.has_value()) {
                m_last_shot_t = clock_now();
            }

            m_last_shot_seq = *seq;
        }
    }

    const bool raw = re4vr::lua_get_tribool("__vr_raw_r_trigger") == 1;

    if (raw && !m_rt_prev_raw) {
        // Flanke: nur ein Druck, der OHNE Aim beginnt, darf spaeter twirlen
        m_rt_press_armed = re4vr::lua_get_tribool("__vr_aim_input") != 1
            && re4vr::lua_get_tribool("is_aim") != 1;
    } else if (!raw) {
        m_rt_press_armed = false;
    }

    m_rt_prev_raw = raw;

    if (!raw || !m_rt_press_armed) {
        return false;
    }

    const double lock = re4vr::lua_get_number("__re4_ww_rt_lockout", 1.0);

    if ((clock_now() - m_last_shot_t) < lock) {
        return false;
    }

    // inkl. Turret/Jetski raus
    if (re4vr::lua_get_tribool("__re4_frame_pure_gameplay") != 1) {
        return false;
    }

    // [MESSER LINKS ERLAUBT 2026-07-21] Gesperrt wird nur, wenn das Messer in
    // der WAFFENHAND (rechts) steckt. Der LINKS-Klon laesst die Gun rechts
    // equippt und darf weiter twirlen.
    if (re4vr::lua_get_tribool("__re4_knife_equipped") == 1
        && re4vr::lua_get_string("__re4_knife_hand") != "left") {
        return false;
    }

    return true;
}

void RE4VRWeapons2::ww_tick() {
    // When disabled, avoid all native weapon/head lookups. reset_all keeps the
    // externally visible state exactly as it was before this early return.
    if (re4vr::lua_get_tribool("__re4_ww_enabled") == 0) {
        ww_reset_all();

        return;
    }

    const double now = clock_now();

    auto* hu = head_updater_w2();
    const auto wid = enum_as_int(hu, "get_EquipWeaponID");
    const bool pistol = wid.has_value() && is_pistol(*wid);

    // [STOCK-SPERRE] Mit Schulterstuetze kein Twirl.
    if (!pistol || stock_mounted(wid)) {
        ww_reset_all();

        return;
    }

    // [PER-WAFFE KEYFRAMES] OKEYS auf das Array DIESER Waffe ZEIGEN --
    // offset_at, save_keyframe und die UI teilen sich den Zeiger. Ein Kopieren
    // wuerde Keyframe-Edits verlieren.
    m_cur_okeys = &m_okeys[*wid];

    // [GESAMT-OFFSET] der apply-Pass braucht die ID (laeuft ohne hu-Zugriff)
    re4vr::lua_set_number("__re4_ww_cur_wid", static_cast<double>(*wid));

    // [PIVOT FRISCH 2026-07-17] Jeden RUHE-Frame neu berechnen -- aber nur
    // uebernehmen, wenn es gelingt. Waehrend spin.active NICHT neu rechnen: die
    // Waffe rotiert dann, der Pivot bliebe dem Dreh hinterher.
    if (!m_spin.active) {
        if (auto* T = weap_transform(hu); T != nullptr) {
            if (const auto lp = compute_local_pivot(T); lp.has_value()) {
                m_pivot_cache[*wid] = *lp;
            }
        }
    }

    if (const auto it = m_pivot_cache.find(*wid); it != m_pivot_cache.end()) {
        m_active_lp = it->second
            + glm::vec3{static_cast<float>(re4vr::lua_get_number("__re4_ww_pivot_x", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_ww_pivot_y", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_ww_pivot_z", 0.0))};
    } else {
        m_active_lp.reset();
    }

    double dt = m_prev_t.has_value() ? (now - *m_prev_t) : 0.016;

    if (dt <= 0.0) {
        dt = 0.016;
    } else if (dt > 0.1) {
        dt = 0.1;
    }

    const bool aiming = re4vr::lua_get_tribool("is_aim") == 1;

    // [RT_GATE] Gehaltener RT = Direkt-Twirl: feinere Empfindlichkeit + KEIN
    // Sustain-Anlauf.
    const bool gate = ww_rt_gate();
    const float sens_now = gate
        ? static_cast<float>(re4vr::lua_get_number("__re4_ww_rt_sens", 0.25))
        : static_cast<float>(re4vr::lua_get_number("__re4_ww_sens", 1.2));

    // Faellt das Gate weg, waehrend ein Gate-Spin laeuft, faehrt der Spin sauber
    // auf die volle Umdrehung aus.
    if (m_rt_gate_prev && !gate && m_spin.active && m_spin.by_rt && !m_spin.finishing) {
        m_spin.finishing = true;
        m_spin.finish_target = std::ceil(m_spin.deg / 360.0f) * 360.0f;
    }

    m_rt_gate_prev = gate;

    // ---- Wippe ----
    std::optional<float> y{};

    {
        auto& vr = VR::get();

        if (vr != nullptr && vr->is_hmd_active()) {
            const auto& cs = vr->get_controllers();

            if (cs.size() >= 2) {
                // rechter Controller (Lua: cs[2])
                y = glm::vec3{vr->get_position(cs[1])}.y;
            }
        }
    }

    if (y.has_value() && m_prev_y.has_value() && m_prev_t.has_value()) {
        const double dd = now - *m_prev_t;

        if (dd > 0.0005) {
            const float vy = (*y - *m_prev_y) / static_cast<float>(dd);

            if (std::abs(vy) >= sens_now) {
                const int32_t vsign = vy > 0.0f ? 1 : -1;

                // [SENS_FIX 2026-07-17] Die letzte Richtung VERFAELLT nach
                // BOB_GAP. Vorher blieb prev_vsign ewig stehen -> zwei EINZELNE
                // kraeftige Bewegungen mit beliebiger Pause galten als
                // Richtungswechsel, und die Empfindlichkeit filterte praktisch
                // nichts.
                if (m_prev_vsign != 0 && (now - m_prev_vsign_t) > BOB_GAP) {
                    m_prev_vsign = 0;
                }

                if (m_prev_vsign != 0 && vsign != m_prev_vsign) {
                    if (!m_bob_started.has_value() || (now - m_last_flip_t) > BOB_GAP) {
                        m_bob_started = now;
                    }

                    m_last_flip_t = now;
                }

                m_prev_vsign = vsign;
                m_prev_vsign_t = now;
            }
        }
    }

    if (m_bob_started.has_value() && (now - m_last_flip_t) > BOB_GAP) {
        ww_reset_bob();
    }

    const bool bobbing =
        m_bob_started.has_value() && (now - m_last_flip_t) <= BOB_GAP;

    // ---- Spin ----
    if (m_spin.active) {
        m_spin.deg += static_cast<float>(re4vr::lua_get_number("__re4_ww_speed", 720.0) * dt);

        // [TWIRL-SPRUCH] Dauer NUR der aktiven Drehung mitschreiben; das
        // Ausfahren zaehlt nicht mit.
        if (!m_spin.finishing && m_tvoice.t0.has_value()) {
            m_tvoice.dur = static_cast<float>(now - *m_tvoice.t0);
            twirl_voice_tick();
        }

        if (m_spin.finishing) {
            if (m_spin.deg >= m_spin.finish_target) {
                m_spin.active = false;
                m_spin.finishing = false;
                m_spin.deg = 0.0f;
                re4vr::lua_set_number("__re4_wildwest_angle", 0.0);

                m_tvoice.t0.reset();   // Stoppuhr aus
                twirl_voice_end();
            }
        } else {
            if (!bobbing || aiming) {
                m_spin.finishing = true;
                m_spin.finish_target = std::ceil(m_spin.deg / 360.0f) * 360.0f;
            }
        }

        if (m_spin.active) {
            const float dir = re4vr::lua_get_number("__re4_ww_dir", 1.0) >= 0.0 ? 1.0f : -1.0f;

            re4vr::lua_set_number("__re4_wildwest_angle",
                                  glm::radians(m_spin.deg) * dir);
            re4vr::lua_set_number("__re4_wildwest_progress",
                                  std::fmod(m_spin.deg, 360.0f));
        }
    } else {
        // [RT_GATE] Bei gehaltenem RT faellt der Sustain-Anlauf weg.
        const double sustain_now =
            gate ? 0.0 : re4vr::lua_get_number("__re4_ww_sustain", 0.6);

        // Toggle an -> das freie Twirlen (nur Wippen, ohne RT) ist GESPERRT.
        const bool start_allowed =
            re4vr::lua_get_tribool("__re4_ww_rt_gate") == 0 || gate;

        if (start_allowed && !aiming && m_bob_started.has_value()
            && (now - *m_bob_started) >= sustain_now) {
            m_spin.active = true;
            m_spin.finishing = false;
            m_spin.deg = 0.0f;

            // merkt, ob DIESER Spin aus dem Gate kam (nur der endet beim
            // Loslassen)
            m_spin.by_rt = gate;

            m_tvoice.t0 = now;
            m_tvoice.dur = 0.0f;
            m_tvoice.fired = false;

            // [PIVOT-ERZWINGEN 2026-07-18] Pivot friert waehrend spin.active
            // ein. Direkt nach einem Waffenwechsel sitzt die neue Waffe im 1.
            // Ruhe-Frame noch nicht sauber in der VR-Hand -> der GANZE erste
            // Salto waere verfaelscht. Deshalb zum Spin-Start frisch erzwingen.
            if (auto* Tsp = weap_transform(hu); Tsp != nullptr) {
                if (const auto lp = compute_local_pivot(Tsp); lp.has_value()) {
                    m_pivot_cache[*wid] = *lp;
                }
            }
        }
    }

    // Twirl-Sound: waehrend der Drehung (inkl. Ausfahren) im Intervall neu
    // anspielen, beim Parken aufhoeren. Preview macht KEINEN Sound.
    if (m_spin.active && re4vr::lua_get_tribool("__re4_ww_sound") != 0) {
        ww_sound_tick(hu, now);
    } else {
        m_twirl_snd.next_t = 0.0;
    }

    // Finger-Blend folgt spin.active (lerp). Vorschau haelt die Pose zum Tunen.
    const float tgt = (m_spin.active || re4vr::lua_get_tribool("__re4_ww_pose_preview") == 1)
        ? 1.0f
        : 0.0f;

    m_finger_blend += (tgt - m_finger_blend)
        * std::min(1.0f, static_cast<float>(dt) * BLEND_SPD);

    if (m_finger_blend < 0.0005f) {
        m_finger_blend = 0.0f;
    }

    re4vr::lua_set_number("__re4_wildwest_finger_blend", m_finger_blend);

    // [VORSCHAU] Gun auf festen Winkel einfrieren -> Pivot + Waffen-Offset live
    // tunen ohne zu wippen.
    if (!m_spin.active && re4vr::lua_get_tribool("__re4_ww_pose_preview") == 1) {
        const float dir = re4vr::lua_get_number("__re4_ww_dir", 1.0) >= 0.0 ? 1.0f : -1.0f;
        const float pa = static_cast<float>(re4vr::lua_get_number("__re4_ww_prev_angle", 90.0));

        re4vr::lua_set_number("__re4_wildwest_angle", glm::radians(pa) * dir);
        re4vr::lua_set_number("__re4_wildwest_progress", std::fmod(pa, 360.0f));
    }

    m_prev_y = y;
    m_prev_t = now;
}

// ---- Twirl-Anwendung (motion attach_weapon): Pos+Rot um joint_03 ----
bool RE4VRWeapons2::wildwest_apply(glm::vec3& wpos, glm::quat& wrot) {
    const float a = static_cast<float>(re4vr::lua_get_number("__re4_wildwest_angle", 0.0));

    // NUR eingreifen, wenn tatsaechlich gedreht wird. __re4_wildwest_angle ist
    // ausschliesslich bei Pistolen waehrend der Dreh-/Ausfahr-Phase != 0 ->
    // reset_all nullt ihn fuer alles andere.
    if (a == 0.0f) {
        return false;
    }

    glm::quat new_wrot = wrot;
    glm::vec3 new_wpos = wpos;

    // Drehung um den Pivot (joint_03 + Feinjustage).
    const float h = a * 0.5f;
    const glm::quat R{std::cos(h), std::sin(h), 0.0f, 0.0f};

    new_wrot = glm::normalize(wrot * R);

    if (m_active_lp.has_value()) {
        new_wpos = (wpos + (wrot * *m_active_lp)) - (new_wrot * *m_active_lp);
    }

    // Waffen-Positions-Offset. Quelle: im EDIT-Vorschau-Modus die Live-Slider,
    // sonst der per-Phase interpolierte Keyframe-Offset. Ohne Keyframes -> Slider.
    const bool editing = re4vr::lua_get_tribool("__re4_ww_pose_preview") == 1
        && !m_spin.active && re4vr::lua_get_tribool("__re4_ww_prev_interp") != 1;

    glm::vec3 off{};
    bool have_off = false;

    if (!editing && m_cur_okeys != nullptr && !m_cur_okeys->empty()) {
        const float phase =
            static_cast<float>(re4vr::lua_get_number("__re4_wildwest_progress", 0.0));
        have_off = offset_at(phase, off);
    }

    if (!have_off) {
        off = glm::vec3{static_cast<float>(re4vr::lua_get_number("__re4_ww_off_x", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_ww_off_y", 0.0)),
                        static_cast<float>(re4vr::lua_get_number("__re4_ww_off_z", 0.0))};
    }

    if (off.x != 0.0f || off.y != 0.0f || off.z != 0.0f) {
        new_wpos += wrot * off;
    }

    wpos = new_wpos;
    wrot = new_wrot;

    return true;
}

// ---- Finger-Pose (motion Finger-Pass): additive lokale Rotation, geblendet --
void RE4VRWeapons2::wildwest_fingers() {
    const float blend =
        static_cast<float>(re4vr::lua_get_number("__re4_wildwest_finger_blend", 0.0));

    if (blend <= 0.001f) {
        return;
    }

    auto* tf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

    if (!re4vr::obj_ok(tf)) {
        return;
    }

    for (const char* bn : ALL_JOINTS) {
        const auto it = m_fing.find(bn);

        if (it == m_fing.end()) {
            continue;
        }

        glm::quat add{};

        if (!euler_add(it->second.x * blend, it->second.y * blend, it->second.z * blend,
                       add)) {
            continue;
        }

        auto* j = joint_by_name(tf, bn);

        if (!re4vr::obj_ok(j)) {
            continue;
        }

        glm::quat cur{};

        if (!get_quat(j, "get_LocalRotation", cur)) {
            continue;
        }

        set_quat(j, "set_LocalRotation", glm::normalize(cur * add));
    }
}

// ============================================================================
// Rahmen: Ticks, Lua-Exporte, Late-Init
// ============================================================================

// [LADEZEIT-KOPPLUNG] Der Wrap auf __re4_reload_set_mag_in_hand faengt ein, was
// reload/2/3/4_dlc/5_dlc gesetzt haben. Er darf deshalb NICHT in
// on_lua_state_created installiert werden -- dort haben die Reload-Luas noch
// nichts geschrieben. Also einmalig im ersten on_frame.
bool RE4VRWeapons2::ensure_init() {
    if (m_inited) {
        return true;
    }

    re4vr::LuaRef lua;

    if (lua == nullptr) {
        return false;
    }

    m_inited = true;

    // Den bisherigen Wert festhalten und unter demselben Namen eine Funktion
    // hinterlegen, die IHN weiterreicht. Solange auch nur eine Reload-Lua
    // laeuft, muss das ein echtes Lua-Callable sein.
    try {
        sol::object prev = (*lua)["__re4_reload_set_mag_in_hand"];

        if (prev.valid() && prev.get_type() == sol::type::function) {
            m_orig_smih = prev.as<sol::protected_function>();
        }

        (*lua)["__re4_reload_set_mag_in_hand"] = [](sol::object active) -> bool {
            auto& self = RE4VRWeapons2::get();

            return self != nullptr && self->smih_call(active);
        };

        m_smih_wrapped = true;
    } catch (...) {
    }

    parry_hook_install();
    install_equip_hook();
    install_capture_hook();

    return true;
}

// (1) Mag-Holster zentral sperren: unser Wrap sitzt aussen. Bei Unlimited
// liefert der Grab false -> kein Mag in die Hand.
bool RE4VRWeapons2::smih_call(sol::object active) {
    if (is_unlimited()) {
        return false;
    }

    if (!m_orig_smih.valid()) {
        return false;
    }

    try {
        auto r = m_orig_smih(active);

        if (!r.valid()) {
            return false;
        }

        sol::object o = r;

        return o.valid() && o.is<bool>() && o.as<bool>();
    } catch (...) {
        return false;
    }
}

// ----------------------------------------------------------------------------
// Hook auf chainsaw.PlayerEquipment.equipWeapon(...)
//
// [SPEC §2.1 -- DER GEFAEHRLICHSTE PUNKT]
// Die Lua entscheidet in BEIDEN Zweigen ueber `debug.traceback`, ob der Aufruf
// NATIV war ("kein autorun-Script im Stack"). Nativ ist der Lua-Stack immer
// leer -- jeder Aufruf gaelte damit als nativ, auch unsere eigenen aus
// RE4VRHolster/RE4VRBinding/RE4VRMinecart. Der Gun-Restore-Guard wuerde ab dem
// Port genau die Equips wegblocken, die durchgehen sollen.
//
// Loesung wie bei merc: die Lua-Stack-Pruefung BLEIBT (weapons.lua und der
// Reload-Block laufen noch als Lua) und bekommt eine Zeitmarke daneben, die
// jeder native Aufrufer unmittelbar vor seinem Equip setzt.
// ----------------------------------------------------------------------------
namespace {

bool s_equip_skipped = false;
bool s_equip_void = true;

// "Der Aufruf kam von uns" -- Zeitmarke ODER laufende Lua-VM.
//
// [FIX 05.09.2026 -- gemeldet: "mit Messer in der Hand keine Pistole mehr aus
// dem Holster" und "nach dem Choke bleibt das Messer statt der letzten Waffe"]
// Hier stand NUR `__re4_equip_ours_t` -- eine Marke, die im ganzen Projekt
// KEIN Aufrufer setzt (nachgeprueft: null Setzer). Damit war jeder eigene
// Equip aus dem nativen Holster/Choke "nativ", und der Gun-Restore-Guard
// unten blockte genau die Zuege, die durchgehen sollen.
// Die Marke, die es WIRKLICH gibt, ist `__re4_our_equip_until` (0,5-s-Fenster):
// holster_exec setzt sie zentral fuer JEDE deferred Holster-Aktion (Lua
// Z.640), holster_bare, der Auto-Redraw, RE4VRChoke und RE4VRBinding ebenso.
// Genau dieselbe Marke lesen RE4VRWeapons (KNIFE_KEEP_OUT) und RE4VREquipLock
// schon immer -- weapons2 war die einzige Stelle, die sie nicht kannte.
bool equip_call_is_ours() {
    if (clock_now() < re4vr::lua_get_number("__re4_our_equip_until", 0.0)) {
        return true;
    }

    if ((clock_now() - re4vr::lua_get_number("__re4_equip_ours_t", -999.0)) < 0.05) {
        return true;
    }

    return re4vr::lua_is_executing();
}

}   // namespace

void RE4VRWeapons2::install_equip_hook() {
    if (m_hooks_installed) {
        return;
    }

    m_hooks_installed = true;

    auto* etd = sdk::find_type_definition("chainsaw.PlayerEquipment");
    auto* em = etd != nullptr
        ? etd->get_method(
              "equipWeapon(chainsaw.EquipType, chainsaw.WeaponID, System.Boolean, System.Boolean)")
        : nullptr;

    if (em == nullptr) {
        return;
    }

    {
        auto* rt = em->get_return_type();
        const auto rn = rt != nullptr ? rt->get_full_name() : std::string{};
        s_equip_void = rn.empty() || rn == "System.Void";
    }

    g_hookman.add(
        em,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            bool skip = false;

            try {
                auto& self = RE4VRWeapons2::get();

                if (self != nullptr && args.size() >= 4) {
                    skip = self->equipweapon_cb(args[3]);
                }
            } catch (...) {
                skip = false;
            }

            if (!skip) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            s_equip_skipped = true;

            return HookManager::PreHookResult::SKIP_ORIGINAL;
        },
        [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
            // [SKIP_ORIGINAL OHNE RUECKGABEWERT = REGISTERMUELL] Nur nach einem
            // SKIP und nur bei nicht-void nullen.
            if (s_equip_skipped) {
                s_equip_skipped = false;

                if (!s_equip_void) {
                    ret = 0;
                }
            }
        });
}

// Rueckgabe: true = SKIP_ORIGINAL.
bool RE4VRWeapons2::equipweapon_cb(uintptr_t raw_arg) {
    // Weg 2: DRAW/Equip -> WeaponID sofort cachen. WeaponID = value-type Enum.
    // Lua nimmt sdk.to_int64(args[4]) UND to_managed_object(...).value__ und
    // akzeptiert den Wert nur, wenn er ein MESSER ist.
    const auto raw = static_cast<int32_t>(raw_arg);
    std::optional<int32_t> v{};

    if (is_knife_id(raw)) {
        v = raw;
    } else {
        auto* o = reinterpret_cast<::REManagedObject*>(raw_arg);

        if (re4vr::obj_ok(o)) {
            if (const auto m = re4vr::get_field_int(o, "value__");
                m.has_value() && is_knife_id(*m)) {
                v = *m;
            }
        }
    }

    if (v.has_value()) {
        re4vr::lua_set_number("__re4_current_knife_wid", static_cast<double>(*v));
    }

    // ------------------------------------------------------------------
    // [QUICK-KNIFE-BLOCK (ADA ONLY) 2026-07-21]
    //
    // [ADA GLEICH WIE ALLE 2026-08-28 -- Ansage des Users] Der Block ist seit
    // dem 28.08. AUS: __re4_knife_ada_block wird nirgends mehr auf true
    // gesetzt. Er bleibt als dokumentierter Rueckweg stehen.
    // ------------------------------------------------------------------
    if (re4vr::lua_get_tribool("__re4_knife_ada_block") == 1 && v.has_value()
        && is_ada_knife_id(*v)) {
        auto* go = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();

        // Gesteuerter Body = Ada? Unbekannt/nil -> false, im Zweifel NICHT
        // blocken.
        if (obj_name_of(go) == "ch3a8z0_body") {
            const double now = clock_now();

            const bool want_knife =
                (now - re4vr::lua_get_number("__re4_knife_draw_ours_t", -999.0)) < 1.0
                || re4vr::lua_get_tribool("__re4_knife_left_intent") == 1
                || re4vr::lua_get_tribool("__re4_knife_left_clone") == 1
                || re4vr::lua_get_tribool("__re4_knife_flying") == 1
                || re4vr::lua_get_tribool("__re4_clone_finisher_restore") == 1
                || re4vr::lua_get_tribool("__re4_holster_killswitch") == 1
                || re4vr::lua_get_tribool("__re4_ks4_active") == 1
                || re4vr::lua_get_tribool("__re4_holster_knife_only") == 1;

            if (!want_knife && !equip_call_is_ours()) {
                return true;   // SKIP_ORIGINAL
            }
        }
    }

    // ------------------------------------------------------------------
    // [KNIFE-KEEP vs NATIVE GUN-RESTORE 2026-07-11]
    // Die Engine equippt NACH einem Killswitch nativ die Main-Gun zurueck,
    // obwohl ein Messer in der Hand ist. Solchen Equip BLOCKEN.
    //
    // [MINECART 2026-07-14] wp4005 = Cart-Gun, existiert AUSSCHLIESSLICH im
    // Minecart und wird beim Kart-Eintritt NATIV angereicht -- IMMER durchlassen.
    // ------------------------------------------------------------------
    const int32_t wid = v.has_value() ? *v : raw;

    if (wid >= 4000 && wid < 5000 && wid != 4005) {
        auto* ctx = get_ctx_w2();
        auto* hu = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
            : nullptr;

        bool knife_in_hand = false;

        if (hu != nullptr) {
            re4vr::try_call<bool>(hu, "get_IsEquipKnife", knife_in_hand);
        }

        // [TOTER ZWEIG, 1:1 -- korrigiert 05.09.2026]
        // Der Lua-Filter lautet (Z.963-966):
        //     if line:find("autorun") and not line:find("knife_lefthand")
        //         then native = false; break end
        // Also: JEDE Stack-Zeile aus einem autorun-Script -- die eigene
        // eingeschlossen -- setzt `native` auf false. Der Callback steht selbst
        // in autorun/re4_vr_weapons2.lua, seine Frames stehen im traceback:
        // `native` war damit IMMER false und dieser Zweig hat in Lua NIE
        // geblockt. Die Ausnahme sollte "weapons2" lauten, nicht
        // "knife_lefthand" -- genau so steht sie im Ada-Zweig direkt darueber
        // (Z.935), was den Tippfehler belegt.
        //
        // Der Port hatte den Filter umgekehrt gelesen und den Zweig scharf
        // gemacht. Ergebnis war das gemeldete "mit Messer in der Hand keine
        // Pistole mehr aus dem Holster" und "nach dem Choke bleibt das Messer":
        // jeder Gun-Equip bei Messer in der Hand wurde verworfen.
        //
        // 1:1 heisst hier: nichts blocken. Die Bedingung bleibt stehen, damit
        // die Absicht dokumentiert ist, aber sie fuehrt zu keinem SKIP mehr.
        if (knife_in_hand && !equip_call_is_ours()) {
            // bewusst KEIN SKIP_ORIGINAL -- s.o.
        }
    }

    return false;
}

// ----------------------------------------------------------------------------
// Hook auf chainsaw.HitController.callbackAttackHit
// args[2] = Angreifer-HC, args[3] = DamageInfo.
// ----------------------------------------------------------------------------
void RE4VRWeapons2::install_capture_hook() {
    auto* td = sdk::find_type_definition("chainsaw.HitController");
    auto* m = td != nullptr ? td->get_method("callbackAttackHit") : nullptr;

    if (m == nullptr) {
        return;
    }

    g_hookman.add(
        m,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            try {
                auto& self = RE4VRWeapons2::get();

                if (self != nullptr && args.size() >= 3) {
                    self->capture_cb(reinterpret_cast<::REManagedObject*>(args[1]),
                                     reinterpret_cast<::REManagedObject*>(args[2]));
                }
            } catch (...) {
            }

            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
}

void RE4VRWeapons2::on_lua_state_created(sol::state& lua) {
    // [PORTFIX 2026-09-06] Erst hier lebt der Lua-State -- s. on_initialize.
    publish_lcfg_globals();

    // Wild-West-Config: JSON lesen UND die Globals setzen. Ohne Guard, damit
    // ein "Reset Scripts" die Werte genauso wiederherstellt, wie das Original
    // sie beim erneuten Laden der .lua gesetzt haette.
    ww_load_cfg();
    // Funktions-Globals, die weapons.lua und der Reload-Block noch als Lua
    // rufen. Sie fallen weg, sobald diese Dateien portiert sind.
    lua["__re4_apply_left_knife_pose"] = []() {
        if (auto& s = RE4VRWeapons2::get(); s != nullptr) {
            s->apply_left_knife_pose();
        }
    };

    lua["__re4_knife_lh_play_sound"] = [](sol::object id) {
        if (auto& s = RE4VRWeapons2::get(); s != nullptr && id.is<double>()) {
            // [GROSSE IDs 2026-09-08] Erst nach uint32_t -- direkt nach int32_t
            // ist ein Wert ab 2^31 (Wurf-Sound) ein Ueberlauf und damit
            // undefiniert; ueber uint32_t stimmt der Bitwert.
            s->knife_lh_play_sound(
                static_cast<int32_t>(static_cast<uint32_t>(id.as<double>())));
        }
    };

    lua["__re4_knife_direct_damage"] = [](sol::object reach) -> bool {
        auto& s = RE4VRWeapons2::get();

        return s != nullptr
            && s->knife_direct_damage(reach.is<double>()
                                          ? static_cast<float>(reach.as<double>())
                                          : 1.8f);
    };

    lua["__re4_knife_direct_damage_at"] = [](sol::object pos, sol::object reach) -> bool {
        auto& s = RE4VRWeapons2::get();

        if (s == nullptr || !pos.is<glm::vec3>()) {
            return false;
        }

        return s->knife_direct_damage_at(
            pos.as<glm::vec3>(),
            reach.is<double>() ? static_cast<float>(reach.as<double>()) : 1.0f);
    };

    lua["__re4_knife_onhit_replay"] = [](sol::object go, sol::object ud) -> bool {
        auto& s = RE4VRWeapons2::get();

        if (s == nullptr || !go.valid() || go.get_type() != sol::type::userdata) {
            return false;
        }

        auto* ud_p = (ud.valid() && ud.get_type() == sol::type::userdata)
            ? ud.as<::REManagedObject*>()
            : nullptr;

        return s->knife_onhit_replay(go.as<::REManagedObject*>(), ud_p);
    };

    lua["__re4_knife_hitset_box"] = [](sol::object go, sol::object pos) -> bool {
        auto& s = RE4VRWeapons2::get();

        if (s == nullptr || !go.valid() || go.get_type() != sol::type::userdata) {
            return false;
        }

        std::optional<glm::vec3> p{};

        if (pos.is<glm::vec3>()) {
            p = pos.as<glm::vec3>();
        }

        return s->knife_hitset_box(go.as<::REManagedObject*>(), p);
    };

    lua["__re4_wildwest_apply"] = [&lua](sol::object wpos, sol::object wrot) {
        auto& s = RE4VRWeapons2::get();

        // Lua gibt bei fehlenden Argumenten die Eingaben unveraendert zurueck.
        if (s == nullptr || !wpos.is<glm::vec3>() || !wrot.is<glm::quat>()) {
            return std::make_tuple(wpos, wrot);
        }

        glm::vec3 p = wpos.as<glm::vec3>();
        glm::quat r = wrot.as<glm::quat>();

        if (!s->wildwest_apply(p, r)) {
            return std::make_tuple(wpos, wrot);
        }

        return std::make_tuple(sol::make_object(lua, p), sol::make_object(lua, r));
    };

    lua["__re4_wildwest_fingers"] = []() {
        if (auto& s = RE4VRWeapons2::get(); s != nullptr) {
            s->wildwest_fingers();
        }
    };

    lua["__re4_is_unlimited"] = []() -> bool {
        auto& s = RE4VRWeapons2::get();

        return s != nullptr && s->is_unlimited();
    };

    lua["__re4_infinite_reserve"] = []() -> bool {
        auto& s = RE4VRWeapons2::get();

        return s != nullptr && s->infinite_reserve();
    };

    lua["__re4_accessory_now"] = [&lua]() -> sol::object {
        auto& s = RE4VRWeapons2::get();

        if (s == nullptr) {
            return sol::nil;
        }

        const auto a = s->accessory_now();

        return a.empty() ? sol::nil : sol::make_object(lua, a);
    };

    // Rueckgabe: (string, zahl) -- reload.lua vergleicht an einer Stelle ueber
    // den String und an einer ueber die Zahl.
    lua["__re4_infinite_ammo_id"] = [&lua]() {
        auto& s = RE4VRWeapons2::get();

        std::string str{};
        std::optional<double> num{};

        if (s == nullptr || !s->infinite_ammo_id(str, num)) {
            return std::make_tuple(sol::object{sol::nil}, sol::object{sol::nil});
        }

        return std::make_tuple(
            str.empty() ? sol::object{sol::nil} : sol::make_object(lua, str),
            num.has_value() ? sol::make_object(lua, *num) : sol::object{sol::nil});
    };

    lua["__re4_ww_stock_now"] = []() -> bool {
        auto& s = RE4VRWeapons2::get();

        return s != nullptr && s->stock_now();
    };

    // [CHAR_STICKY] guarded definiert -- motion und holster bringen dieselbe
    // Funktion mit, die ZUERST GELADENE gewinnt. Deshalb hier NUR setzen, wenn
    // noch nichts da ist.
    if (!re4vr::lua_has_function("__re4_char_now")) {
        lua["__re4_char_now"] = [&lua]() -> sol::object {
            auto* go = re4vr::fc::on() ? re4vr::fc::body_go() : re4vr::body_game_object();
            const std::string n = obj_name_of(go);

            // [ADA_MERCS_BODY] Adas Mercs-Body zaehlt ebenfalls als "ada".
            if (n == "ch3a8z0_body" || n == "ch3a8z0_MC_body") {
                re4vr::lua_set_string("__re4_char_last", "ada");

                return sol::make_object(lua, "ada");
            }

            if (n == "ch0a0z0_body" || n == "ch0a1z0_body") {
                re4vr::lua_set_string("__re4_char_last", "leon");

                return sol::make_object(lua, "leon");
            }

            // [STICKY ZURUECKGENOMMEN] bei unbekanntem Body nil -- NICHT auf
            // Leon zurueckfallen.
            return sol::nil;
        };
    }
}

void RE4VRWeapons2::on_lua_state_destroyed(sol::state&) {
    // [_G ueberlebt Reset Scripts NICHT] Der State ist weg -- alles, was auf ihn
    // zeigt, muss zurueckgesetzt werden, sonst ruft der Wrap in einen toten
    // State.
    m_orig_smih = sol::protected_function{};
    m_smih_wrapped = false;
    m_inited = false;

    // [STALE-CLEANUP] Vorm Script-Reload den getrackten Klon zerstoeren -> kein
    // Waise fuers naechste Script-Objekt (Lua: re.on_script_reset).
    clone_destroy();
    re4vr::lua_set_bool("__re4_knife_left_clone", false);
    destroy_orphan_clones(nullptr);
}

bool RE4VRWeapons2::stock_now() const {
    return m_stock_c.on;
}

void RE4VRWeapons2::on_frame() {
    // [SCRIPTGATE -- ergaenzt 07.09.2026] Riegel zu = dieses Modul ist so still,
    // als waere seine Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    // Fehlte hier komplett: waehrend der Gondelfahrt schaltet RE4VRObjects alle
    // Scripte ab AUSSER objects/binding/materials/firstperson (BLEIBT-Liste) --
    // re4_vr_weapons.lua und re4_vr_weapons2.lua gehoerten nie dazu, liefen im
    // Port aber weiter.
    if (re4vr::mods_gated()) {
        return;
    }

    if (!ensure_init()) {
        return;
    }

    // [BODY-EPOCH 2026-09-22] Body gewechselt (Save-Load/Tod): gemerkte Zeiger
    // an der Leiche verwerfen. knife_di_guard (DamageInfo, Links-Klon) bleibt
    // unveraendert und macht seinen Teil weiter selbst.
    if (const auto ep = re4vr::body_epoch(); ep != m_body_epoch) {
        m_body_epoch = ep;
        drop_body_caches();
    }

    // Reihenfolge = Registrierungsreihenfolge der on_frame in der Lua.
    // Sie ist NICHT beliebig: der Unlimited-Tick muss zuletzt laufen (er
    // ueberschreibt grab_empty), und lh_char_tick steht bewusst vor lh_tick.
    knife_tick();          // Z. 131  Block 1
    lh_char_tick();        // Z. 799  Block 2 (eigener Tick, laeuft immer)
    lh_tick();             // Z.1292  Block 2
    knife_di_guard();      // Z.1813  Block 3
    saved_vals_tick();     // Z.2333  Block 3
    ww_tick();             // Z.2857  Block 4
    cannon_tick();         // Z.3189  Block 5
    unlimited_tick();      // Z.3431  Block 6
    parry_keep_gun_tick(); // Z.3463  Block 7
}

// ============================================================================
// UI
//
// In Lua oeffnet Z.10 den Parent-Tree "RE4VR - Weapons2" und Z.3166 schliesst
// ihn wieder; die vier Feature-Trees nesten sich dazwischen. Hier ist das EIN
// zusammenhaengender Block, das Ergebnis ist dasselbe.
// ============================================================================

void RE4VRWeapons2::draw_parry_ui() {
    if (!ImGui::TreeNode("RE4 VR - Messer Parry")) {
        return;
    }

    float v = m_parry_tol;

    if (ImGui::SliderFloat("Parry-Toleranz (groesser = leichter)", &v, 0.3f, 2.5f, "%.2f")) {
        m_parry_tol = v;
        save_parry_cfg();
    }

    ImGui::TreePop();
}

void RE4VRWeapons2::draw_lh_ui() {
    const std::string title =
        "RE4 VR - Messer LINKE Hand [" + (m_lh_char.empty() ? std::string{"leon"} : m_lh_char)
        + "]";

    if (!ImGui::TreeNode(title.c_str())) {
        return;
    }

    // [BLUT_SCHALTER] Der Schalter ist heute WIRKUNGSLOS (der saveStamp-Replay
    // hat keine Vorlage mehr, s. Spec §2.2). Bleibt 1:1 stehen.
    {
        bool b = m_lcfg.blood_on;

        if (ImGui::Checkbox("Blut-Effekt am Links-Messer-Treffer (aus = Crash-Test)", &b)) {
            m_lcfg.blood_on = b;
            lcfg_save();
        }

        if (!m_lcfg.blood_on) {
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 1.0f, 1.0f},
                               "  Blut-Replay AUS -- Schaden und Trefferton laufen normal weiter");
        }
    }

    ImGui::Separator();
    ImGui::Text("-- Greifen --");
    ImGui::Text("Abstand linke Hand -> Holster: %s  (armed=%s)",
                m_last_d.has_value() ? std::to_string(*m_last_d).c_str() : "-",
                m_armed ? "true" : "false");

    // [EIN RADIUS 2026-07-19] Die beiden Links-Slider sind ENTFERNT -- der
    // Radius kommt vom Messer-Holster (ein Regler fuer beide Haende).
    ImGui::TextColored(
        ImVec4{0.53f, 0.53f, 0.53f, 1.0f},
        "Greif-Radius: %.3f / Release: %.3f  (aus dem Messer-Holster)",
        re4vr::lua_get_number("__re4_knife_grab_trigger", m_lcfg.trigger),
        re4vr::lua_get_number("__re4_knife_grab_release", m_lcfg.release));

    {
        float v = m_lcfg.tap_sec;

        if (ImGui::SliderFloat("Tap/Hold-Grenze (s) [< = Flip, > = DPAD]##lhfliptap", &v, 0.08f,
                               0.40f, "%.2f")) {
            m_lcfg.tap_sec = v;
            lcfg_save();
        }
    }

    {
        float v = m_lcfg.flip_speed;

        if (ImGui::SliderFloat("Flip-Tempo (klein=weich, gross=instant)##lhflipspd", &v, 0.05f,
                               1.0f, "%.2f")) {
            m_lcfg.flip_speed = v;
            lcfg_save();
        }
    }

    {
        float v = m_lcfg.swing_speed;

        if (ImGui::SliderFloat("Melee-Schwung-Schwelle (m/s)  [hoch=unempfindlicher]##lhswing",
                               &v, 1.0f, 10.0f, "%.2f")) {
            m_lcfg.swing_speed = v;
            lcfg_save();
        }
    }

    ImGui::Text("Schaden = game-nativ (Template vom 1. Rechts-Treffer). Kein kuenstlicher Wert.");
    ImGui::Separator();
    ImGui::Text("-- Mesh-Offset pro Messer (bewegt den Klon in der linken Hand) --");

    // [LH_CLONE] Im Klon-Modus ist das Messer NICHT equippt -> die ID kommt vom
    // Klon, nicht von get_equip_wid.
    std::optional<int32_t> wid{};

    if (re4vr::lua_get_tribool("__re4_knife_left_clone") == 1) {
        wid = m_clone.wid;
    } else if (re4vr::lua_get_string("__re4_knife_hand") == "left") {
        wid = get_equip_wid_w2();
    }

    if (!wid.has_value()) {
        ImGui::Text("Messer erst in die LINKE Hand ziehen -> dann pro Messer tunen.");
        ImGui::TreePop();

        return;
    }

    auto& o = m_lh_off[*wid];

    ImGui::Text("Messer-ID: %d", *wid);

    bool ch = false;
    ch |= ImGui::SliderFloat("Pos X##lhoff", &o.px, -0.30f, 0.30f, "%.4f");
    ch |= ImGui::SliderFloat("Pos Y##lhoff", &o.py, -0.30f, 0.30f, "%.4f");
    ch |= ImGui::SliderFloat("Pos Z##lhoff", &o.pz, -0.30f, 0.30f, "%.4f");
    ch |= ImGui::SliderFloat("Rot X (Grad)##lhoff", &o.rx, -180.0f, 180.0f, "%.1f");
    ch |= ImGui::SliderFloat("Rot Y (Grad)##lhoff", &o.ry, -180.0f, 180.0f, "%.1f");
    ch |= ImGui::SliderFloat("Rot Z (Grad)##lhoff", &o.rz, -180.0f, 180.0f, "%.1f");

    if (ch) {
        lh_off_save();
    }

    ImGui::Separator();
    ImGui::Text("-- Flip-Offset pro Messer (Reverse-Grip) --");

    auto& fo = m_lh_flip_off[*wid];

    bool fch = false;
    fch |= ImGui::SliderFloat("Flip Pos X##lhflipoff", &fo.x, -0.30f, 0.30f, "%.4f");
    fch |= ImGui::SliderFloat("Flip Pos Y##lhflipoff", &fo.y, -0.30f, 0.30f, "%.4f");
    fch |= ImGui::SliderFloat("Flip Pos Z##lhflipoff", &fo.z, -0.30f, 0.30f, "%.4f");

    if (fch) {
        lh_flip_save();
    }

    ImGui::TreePop();
}

void RE4VRWeapons2::draw_ww_ui() {
    if (!ImGui::TreeNode("RE4 VR - Wild West (Pistol Twirl)")) {
        return;
    }

    // Luas gslider: Wert aus dem Global, bei Aenderung zurueck + speichern.
    const auto gslider = [&](const char* label, const char* gkey, float lo, float hi) {
        float v = static_cast<float>(re4vr::lua_get_number(gkey, 0.0));

        if (ImGui::SliderFloat(label, &v, lo, hi)) {
            re4vr::lua_set_number(gkey, v);
            ww_save_cfg();
        }
    };

    {
        bool on = re4vr::lua_get_tribool("__re4_ww_enabled") != 0;

        if (ImGui::Checkbox("Aktiv", &on)) {
            re4vr::lua_set_bool("__re4_ww_enabled", on);
            ww_save_cfg();
        }
    }

    ImGui::Text("Wippen (Waffenhand, nicht gezielt) -> dreht solange du wippst; aufhoeren -> parkt.");
    ImGui::Spacing();

    gslider("Sustain (s) bis Start", "__re4_ww_sustain", 0.1f, 2.0f);
    gslider("Wippe-Empfindlichkeit (m/s)", "__re4_ww_sens", 0.3f, 4.0f);
    gslider("Dreh-Tempo (Grad/s)", "__re4_ww_speed", 180.0f, 2160.0f);

    {
        bool on = re4vr::lua_get_tribool("__re4_ww_rt_gate") != 0;

        if (ImGui::Checkbox("R-Trigger halten = Direkt-Twirl (kein Sustain)", &on)) {
            re4vr::lua_set_bool("__re4_ww_rt_gate", on);
            ww_save_cfg();
        }

        if (on) {
            gslider("  Empfindlichkeit bei gehaltenem RT (m/s)", "__re4_ww_rt_sens", 0.02f, 1.50f);
            gslider("  Sperre nach dem letzten Schuss (s)", "__re4_ww_rt_lockout", 0.0f, 3.0f);
        }
    }

    {
        bool rev = re4vr::lua_get_number("__re4_ww_dir", 1.0) < 0.0;

        if (ImGui::Checkbox("Andersherum drehen", &rev)) {
            re4vr::lua_set_number("__re4_ww_dir", rev ? -1.0 : 1.0);
            ww_save_cfg();
        }
    }

    {
        bool on = re4vr::lua_get_tribool("__re4_ww_sound") != 0;

        if (ImGui::Checkbox("Twirl-Sound (waehrend der Drehung)", &on)) {
            re4vr::lua_set_bool("__re4_ww_sound", on);
            ww_save_cfg();
        }

        if (on) {
            gslider("  Sound-Wiederhol-Intervall (s)", "__re4_ww_snd_interval", 0.03f, 0.60f);
        }
    }

    if (ImGui::TreeNode("Pivot + Waffen-Offset (Waffen-Local, m)")) {
        {
            bool pv = re4vr::lua_get_tribool("__re4_ww_pose_preview") == 1;
            ImGui::Checkbox("Vorschau: Gun eingefroren gedreht (zum Tunen ohne Wippen)", &pv);
            re4vr::lua_set_bool("__re4_ww_pose_preview", pv);
        }

        gslider("Vorschau-Winkel (Grad)", "__re4_ww_prev_angle", 10.0f, 350.0f);

        ImGui::Text("-- Pivot (Drehpunkt: joint_03 + Versatz) --");
        gslider("Pivot X", "__re4_ww_pivot_x", -0.30f, 0.30f);
        gslider("Pivot Y", "__re4_ww_pivot_y", -0.30f, 0.30f);
        gslider("Pivot Z", "__re4_ww_pivot_z", -0.30f, 0.30f);

        ImGui::Text("-- Waffen-Offset (ganze Gun verschieben) --");
        gslider("Offset X", "__re4_ww_off_x", -0.30f, 0.30f);
        gslider("Offset Y", "__re4_ww_off_y", -0.30f, 0.30f);
        gslider("Offset Z", "__re4_ww_off_z", -0.30f, 0.30f);

        ImGui::Spacing();
        ImGui::Text("-- Offset-Keyframes ueber den 360-Salto --");
        ImGui::Text("Ablauf: Vorschau AN -> Vorschau-Winkel auf eine Salto-Phase -> Offset X/Y/Z stellen");
        ImGui::Text("bis der Finger am Trigger sitzt -> 'Keyframe speichern'. Fuer die naechste Phase wiederholen.");

        {
            bool ip = re4vr::lua_get_tribool("__re4_ww_prev_interp") == 1;

            if (ImGui::Checkbox("Vorschau interpoliert (Vorschau-Winkel sweepen)", &ip)) {
                re4vr::lua_set_bool("__re4_ww_prev_interp", ip);
                ww_save_cfg();
            }
        }

        // [PER-WAFFE KEYFRAMES] Keyframes gelten fuer die AKTUELL equippte
        // Waffe -> den ZEIGER darauf setzen (kein Kopieren).
        {
            auto* hu = head_updater_w2();
            const auto w = enum_as_int(hu, "get_EquipWeaponID");

            if (w.has_value() && is_pistol(*w)) {
                m_cur_okeys = &m_okeys[*w];
                ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f},
                                   "Keyframes fuer AKTUELLE Waffe: wid %d", *w);
            } else {
                ImGui::TextColored(
                    ImVec4{1.0f, 0.8f, 0.4f, 1.0f},
                    "Keine Pistole in der Hand -> equippe die Waffe, deren Salto du tunen willst.");
            }
        }

        if (ImGui::Button("Keyframe @ Vorschau-Winkel speichern")) {
            save_keyframe(
                static_cast<float>(re4vr::lua_get_number("__re4_ww_prev_angle", 90.0)),
                glm::vec3{static_cast<float>(re4vr::lua_get_number("__re4_ww_off_x", 0.0)),
                          static_cast<float>(re4vr::lua_get_number("__re4_ww_off_y", 0.0)),
                          static_cast<float>(re4vr::lua_get_number("__re4_ww_off_z", 0.0))});
        }

        ImGui::SameLine();

        if (ImGui::Button("Alle Keyframes loeschen") && m_cur_okeys != nullptr) {
            m_cur_okeys->clear();
            ww_save_cfg();
        }

        if (m_cur_okeys != nullptr) {
            ImGui::Text("%d Keyframe(s):", static_cast<int>(m_cur_okeys->size()));

            for (size_t i = 0; i < m_cur_okeys->size(); ++i) {
                const auto& k = (*m_cur_okeys)[i];

                ImGui::Text("  %3.0f deg: (%.3f, %.3f, %.3f)", k.a, k.x, k.y, k.z);
                ImGui::SameLine();

                if (ImGui::Button(("Laden##ok" + std::to_string(i)).c_str())) {
                    re4vr::lua_set_number("__re4_ww_prev_angle",
                                          std::clamp(k.a, 10.0f, 350.0f));
                    re4vr::lua_set_number("__re4_ww_off_x", k.x);
                    re4vr::lua_set_number("__re4_ww_off_y", k.y);
                    re4vr::lua_set_number("__re4_ww_off_z", k.z);
                    ww_save_cfg();
                }

                ImGui::SameLine();

                if (ImGui::Button(("X##ok" + std::to_string(i)).c_str())) {
                    m_cur_okeys->erase(m_cur_okeys->begin() + static_cast<long>(i));
                    ww_save_cfg();

                    break;
                }
            }
        }

        ImGui::Text("%s", m_active_lp.has_value() ? "Trigger-Joint _03 gefunden"
                                                  : "Trigger-Joint _03 (noch) nicht gefunden");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Hand-Pose (additive Grad, PRO SEGMENT X/Y/Z)")) {
        {
            bool pv = re4vr::lua_get_tribool("__re4_ww_pose_preview") == 1;
            ImGui::Checkbox("Pose-Vorschau (zum Tunen ohne Drehen, Pistole halten)", &pv);
            re4vr::lua_set_bool("__re4_ww_pose_preview", pv);
        }

        ImGui::Text("Jeder Finger = 3 Segmente (F1=Basis .. F3=Kuppe) einzeln kippbar.");

        const std::array<std::pair<const char*, std::array<const char*, 3>>, 5> fingers{{
            {"Daumen", {"R_Thumb1", "R_Thumb2", "R_Thumb3"}},
            {"Zeigefinger", {"R_IndexF1", "R_IndexF2", "R_IndexF3"}},
            {"Mittelfinger", {"R_MiddleF1", "R_MiddleF2", "R_MiddleF3"}},
            {"Ringfinger", {"R_RingF1", "R_RingF2", "R_RingF3"}},
            {"Kleiner", {"R_PinkyF1", "R_PinkyF2", "R_PinkyF3"}},
        }};

        for (const auto& [label, joints] : fingers) {
            if (!ImGui::TreeNode(label)) {
                continue;
            }

            for (size_t gi = 0; gi < joints.size(); ++gi) {
                const char* jn = joints[gi];
                auto& g = m_fing[jn];

                ImGui::Text("Segment %d  (%s)", static_cast<int>(gi + 1), jn);

                bool ch = false;
                ch |= ImGui::SliderFloat((std::string{jn} + " X").c_str(), &g.x, -180.0f, 180.0f);
                ch |= ImGui::SliderFloat((std::string{jn} + " Y").c_str(), &g.y, -180.0f, 180.0f);
                ch |= ImGui::SliderFloat((std::string{jn} + " Z").c_str(), &g.z, -180.0f, 180.0f);

                if (ch) {
                    ww_save_cfg();
                }
            }

            ImGui::TreePop();
        }

        ImGui::TreePop();
    }

    if (m_spin.active) {
        ImGui::Text("Status: %s (%.0f Grad, pose %.2f)",
                    m_spin.finishing ? "faehrt aus" : "dreht", m_spin.deg, m_finger_blend);
    } else {
        ImGui::Text("Status: bereit");
    }

    ImGui::TreePop();
}

void RE4VRWeapons2::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Weapons2")) {
        return;
    }

    draw_parry_ui();
    draw_lh_ui();
    draw_ww_ui();

    ImGui::TreePop();
}

#endif
