// ============================================================================
// RE4VRKillswitch -- Portierung von re4vr/re4_vr_killswitch.lua.
// Kopfkommentar und Bauform: siehe RE4VRKillswitch.hpp
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sdk/SceneManager.hpp"
#include "sdk/RETypeDB.hpp"

#include "../../../HookManager.hpp"
#include "../../../Mods.hpp"
#include "../../../REFramework.hpp"   // g_framework->draw_menu_checkbox
#include "RE4VRKillswitch.hpp"

// windows.h (ueber die Includes oben) definiert min/max als MAKROS und zerlegt
// jedes std::min/std::max. Der Fork setzt kein NOMINMAX.
#undef min
#undef max

namespace {

// ---------------------------------------------------------------------------
// Konstanten (1:1 aus der Lua)
// ---------------------------------------------------------------------------
constexpr const char* ZONES_FILE = "re4_vr/re4_vr_killswitch_zones.json";
constexpr const char* KS_CFG_FILE = "re4_vr/re4_vr_killswitch_cfg.json";

constexpr const char* CAMSTATE_ENUM = "chainsaw.CameraDefine.PlayerCameraState";
constexpr const char* OCCUPIED_ENUM = "chainsaw.OccupiedMediatorPriority";
constexpr const char* GIMMICKTYPE_ENUM = "chainsaw.CameraDefine.GimmickType";

constexpr double JUMPDOWN_HOLD = 0.5;
constexpr double ANIM_BLEND_DURATION = 0.2;
constexpr double DAMAGE_HOLD = 0.35;
constexpr double EVT60874_DELAY = 1.2;   // WERT ERSPIELT 2026-07-22
constexpr double EVT40510_DELAY = 5.5;   // WERT ERSPIELT 2026-07-22
constexpr int32_t LADDER_EXIT_ENDFRAME = 100;

// [GIMMICK_FLAG] Interaktions-Flags am PlayerContext, die ~0.2s VOR dem
// zugehoerigen CamState kippen. Solange eins true ist, wird der Frame NICHT
// als Gameplay/Pin-Release behandelt.
// Die auskommentierten Eintraege der Lua (Tuer/Terrain/MoveGimmickRide/Ladder/
// BoxBreak) sind bewusst NICHT hier -- jeder hat dort seinen eigenen Grund.
constexpr std::array GIMMICK_FLAGS{
    "get_IsTerrainUpWithPartner",   // Klettern / Partner-Boost
    "get_IsHookShot",               // Enterhaken
    "get_IsLiftActing",             // Hebel / Lift
};

// Gameplay-State-Allowlist (INVERSIONS-Ansatz): der VR-Zustand gilt NUR fuer
// reines Gameplay; alles ausserhalb defaultet sicher auf "aus".
constexpr std::array GAMEPLAY_CAM_STATES{
    "Normal", "Jog", "Sprint", "Combat", "BattleNormal",
    "WallAlongJog", "QuickTurm", "CrouchQuickTurm", "Crouch", "ForceCrouch",
    "Hold", "HoldVariation", "HoldIronSight", "HoldGrenade", "HoldExtraScope",
    "HoldOpticalScope", "HoldSpecialOpticalScope", "ViaScope",
    "PumpAction", "RifleChangeWeapon",
};

// [PIN_RELEASE] 2026-07-15 STILLGELEGT, NICHT ENTFERNT -- Liste ist LEER: die
// komplette Sprung-/Absatz-Kette steht jetzt in KS2_CAM_STATES. Die Mechanik
// bleibt vollstaendig erhalten; ein Eintrag hier reicht, um sie zurueckzuholen.
// Nebeneffekt: is_airborne_camstate baut darauf auf -> "luftig" ist nur
// noch Landing.
const std::vector<const char*> PIN_RELEASE_CAM_STATES{};

// [TRAVERSAL KOMPLETT -> KS2 2026-07-15] ALLE Traversal-Aktionen KS2 + Nagel.
constexpr std::array KS2_CAM_STATES{
    "TerrainAction", "TerrainAction_2m", "TerrainAction_Jump", "TerrainAction_Window",
    "Fall", "Fall_Jump", "Fall_Window", "Landing",
    "TerrainUpWithPartner", "TerrainUpWithPartner_1m", "HookShot",
    "Damage", "PartnerRescue", "AutoMove",
};

// [GLOBAL KS3 -- 2026-07-15 STILLGELEGT, NICHT ENTFERNT] Liste ist LEER: alle
// vier Eintraege sind nach KS2_CAM_STATES gewandert, weil der Head-Nagel das
// Mesh-Aus ueberfluessig macht. Die KS3-Mechanik bleibt komplett erhalten.
const std::vector<const char*> KS3_CAM_STATES{};

// [KS4] Fuer Leons Fatal-Kick + Roundhouse-Kick.
constexpr std::array KS4_CAM_STATES{"FatalKick", "FatalRoundKick"};

// KS2_RULES/KS3_RULES/KS4_RULES sind in der Lua alle LEER (bis ein solides
// Merkmal steht). Der Regel-Zweig in match_level ist damit tot -- er wandert
// als toter Zweig mit, damit ein spaeterer Eintrag ihn wiederbelebt.

// [GIMMICK_KS3_SPOT] Durchquetsch-Stages, jede einzeln per Monitordump belegt.
const std::unordered_set<int32_t> GKS3_STAGES{
    53303, 53302, 54400, 50401, 50500, 55850, 55851, 55852, 61302, 61301, 60880,
};
// [ZWEI EVENTS IN EINER STAGE] In 55850 leben Durchquetschen UND Mini-Demo mit
// derselben Kamera-Klasse -> dort entscheidet AUSSCHLIESSLICH die Priority.
const std::unordered_set<int32_t> PRIO_ONLY_STAGES{55850};
const std::unordered_set<int32_t> MINIDEMO_KS4_STAGES{55850};
const std::unordered_set<int32_t> GFIX_KS5_STAGES{55852};
const std::unordered_set<int32_t> GFIX_KS4_STAGES{44400};
// [GONDEL-PARENT] BEWUSST eine Liste mit EINEM Eintrag: das ParentGimmick-Bit
// gilt auch fuer Leitern/Quetschstellen -- ohne Stage-Gate wuerde der Block
// quer durchs Spiel feuern.
const std::unordered_set<int32_t> GONDOLA_PARENT_STAGES{60850};
const std::unordered_set<int32_t> MINECART_KS4_STAGES{55201, 55202};

// [JETSKI] Exakt die Stages, die im alten Mod die JETSKI_CFG-Offsets bekamen.
const std::unordered_set<int32_t> JETSKI_KS4_STAGES{
    59103,
    59201, 59202, 59203, 59204, 59205, 59206, 59207,
    59209, 59210, 59211, 59214, 59215, 59216, 59217,
    59218, 59219, 59220, 59221, 59222,
};

// [ELEVATOR] Reverse-Killswitch: in der Box GAMEPLAY pinnen.
struct ElevBox {
    int32_t stage;
    float x1, y1, z1;
    float x2, y2, z2;
    float mxz;
};
constexpr ElevBox ELEV{53202, 130.45f, 27.33f, 64.28f, 131.32f, 33.20f, 63.60f, 3.0f};
constexpr float ELEV_START_RELEASE = 0.005f;   // [UNTEN] Pin erst ein Fitzel NACH dem Start
constexpr float ELEV_TOP_RELEASE = 0.005f;     // [OBEN] Pin ein Fitzel VOR dem Ziel loesen

constexpr ElevBox ELEV2{53302, 89.77f, 19.03f, 107.50f, 90.14f, 13.22f, 108.50f, 3.0f};
constexpr float ELEV2_Y_PAD = 0.30f;   // Y-Puffer nach AUSSEN (oben+unten sicher drin)

// [ELEVATOR5] Gemessene Endpunkte (Monitor-Dumps 16:07 unten / 16:10 oben).
constexpr float ELEV5_X = 300.85f, ELEV5_Z = -105.44f, ELEV5_R = 4.0f;
constexpr float ELEV5_Y_UNTEN = -3.26f, ELEV5_Y_OBEN = 48.23f, ELEV5_Y_TOL = 1.5f;

// ---------------------------------------------------------------------------
// Helfer
// ---------------------------------------------------------------------------
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

bool obj_alive(::REManagedObject* o) {
    return o != nullptr;
}

// Luas `safe(function() return o:call(name) end) == true`.
bool call_bool(::REManagedObject* o, std::string_view name) {
    if (o == nullptr) {
        return false;
    }

    bool out = false;

    return re4vr::try_call<bool>(o, name, out) && out;
}

sdk::RETypeDefinition* type_def_of(::REManagedObject* o) {
    if (o == nullptr) {
        return nullptr;
    }

    return utility::re_managed_object::get_type_definition(o);
}

// Luas `busy:get_type_definition():is_a(td)`.
bool is_a(::REManagedObject* o, sdk::RETypeDefinition* want) {
    if (o == nullptr || want == nullptr) {
        return false;
    }

    auto* td = type_def_of(o);

    if (td == nullptr) {
        return false;
    }

    try {
        return td->is_a(want);
    } catch (...) {
        return false;
    }
}

// System.String aus einem Managed Object. get_string will einen ::SystemString*,
// ein Managed Object muss dafuer gecastet werden.
std::string mstr(::REManagedObject* o) {
    if (o == nullptr) {
        return {};
    }

    return utility::re_string::get_string(reinterpret_cast<::SystemString*>(o));
}

std::string full_name_of(::REManagedObject* o) {
    auto* td = type_def_of(o);

    if (td == nullptr) {
        return {};
    }

    try {
        return td->get_full_name();
    } catch (...) {
        return {};
    }
}

// Alle statischen Enum-Werte eines Typs, deren Name das Praedikat erfuellt.
// Lua liest sie ueber `f:get_data(nil)` mit userdata-Fallback auf `value__` --
// nativ kommen Enums direkt als Integer, das Auspacken entfaellt.
template <typename Pred>
std::optional<std::unordered_set<int32_t>> enum_values_where(const char* type_name, Pred pred) {
    auto* td = sdk::find_type_definition(type_name);

    if (td == nullptr) {
        return std::nullopt;
    }

    std::unordered_set<int32_t> out{};

    for (auto* f : td->get_fields()) {
        if (f == nullptr) {
            continue;
        }

        try {
            if (!f->is_static()) {
                continue;
            }

            const char* nm = f->get_name();

            if (nm == nullptr || !pred(nm)) {
                continue;
            }

            // [PORTFIX 2026-09-06] typrichtig lesen -- s. re4vr::enum_field_value.
            if (const auto v = re4vr::enum_field_value(f, nullptr); v.has_value()) {
                out.insert(static_cast<int32_t>(*v));
            }
        } catch (...) {
        }
    }

    return out;
}

bool starts_with(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

// Luas tostring(st) fuer den activating_reason -- nil wird "nil".
std::string state_str(std::optional<int32_t> st) {
    return st.has_value() ? std::to_string(*st) : std::string{"nil"};
}

sdk::RETypeDefinition* g_player_cam_td{nullptr};
sdk::RETypeDefinition* g_gimmick_motion_td{nullptr};
sdk::RETypeDefinition* g_gimmick_fix_td{nullptr};
sdk::RETypeDefinition* g_action_camera_td{nullptr};
sdk::RETypeDefinition* g_vehicle_camera_td{nullptr};
sdk::RETypeDefinition* g_mfsm2_td{nullptr};
sdk::RETypeDefinition* g_motion_td{nullptr};
sdk::RETypeDefinition* g_elev_td_59100{nullptr};

}   // namespace

std::shared_ptr<RE4VRKillswitch>& RE4VRKillswitch::get() {
    static auto inst = std::make_shared<RE4VRKillswitch>();

    return inst;
}

// ============================================================================
// Persistenz
// ============================================================================

void RE4VRKillswitch::load_zones() {
    m_zones.clear();

    const auto d = re4vr::json_load(ZONES_FILE);

    if (!d.is_array()) {
        return;
    }

    for (const auto& z : d) {
        if (!z.is_object()) {
            continue;
        }

        Zone out{};

        const auto num = [&](const char* k, float def) -> float {
            const auto it = z.find(k);

            return it != z.end() && it->is_number() ? it->get<float>() : def;
        };

        out.stage = static_cast<int32_t>(num("stage", 0.0f));
        out.space = static_cast<int32_t>(num("space", 0.0f));
        out.x = num("x", 0.0f);
        out.y = num("y", 0.0f);
        out.z = num("z", 0.0f);
        // Luas `z.r or 3.0` -- fehlt der Wert, gilt 3 m.
        out.r = num("r", 3.0f);
        out.level = static_cast<int32_t>(num("level", 2.0f));

        // camstate darf FEHLEN (dann matcht die Zone jeden Event-Typ) --
        // das ist etwas anderes als camstate 0.
        if (const auto it = z.find("camstate"); it != z.end() && it->is_number()) {
            out.camstate = it->get<int32_t>();
        }

        if (const auto it = z.find("name"); it != z.end() && it->is_string()) {
            out.name = it->get<std::string>();
        }

        m_zones.push_back(std::move(out));
    }
}

void RE4VRKillswitch::save_zones() {
    nlohmann::json arr = nlohmann::json::array();

    for (const auto& z : m_zones) {
        nlohmann::json e{};
        e["stage"] = z.stage;
        e["space"] = z.space;
        e["x"] = z.x;
        e["y"] = z.y;
        e["z"] = z.z;
        e["r"] = z.r;
        e["level"] = z.level;

        if (z.camstate.has_value()) {
            e["camstate"] = *z.camstate;
        }

        e["name"] = z.name;
        arr.push_back(std::move(e));
    }

    re4vr::json_save(ZONES_FILE, arr);
}

void RE4VRKillswitch::load_ks_cfg() {
    const auto d = re4vr::json_load(KS_CFG_FILE);

    if (!d.is_object()) {
        return;
    }

    // Lua prueft `type(d.x) == "boolean"` -- ein Nicht-Boolean laesst den
    // Startwert stehen.
    if (const auto it = d.find("fp_enabled"); it != d.end() && it->is_boolean()) {
        m_fp_enabled = it->get<bool>();
    }

    if (const auto it = d.find("ks2_as_ks4"); it != d.end() && it->is_boolean()) {
        m_ks2_as_ks4 = it->get<bool>();
    }
}

void RE4VRKillswitch::save_ks_cfg() {
    nlohmann::json d{};
    d["fp_enabled"] = m_fp_enabled;
    d["ks2_as_ks4"] = m_ks2_as_ks4;
    re4vr::json_save(KS_CFG_FILE, d);
}

void RE4VRKillswitch::set_fp_enabled(bool v) {
    m_fp_enabled = v;
    re4vr::lua_set_bool("__re4_ks_fp_enabled", m_fp_enabled);
    save_ks_cfg();
}

// [ZONES] KS-Level (2|3|4) fuer einen Event-Eintritt.
// Match = gleiche Stage UND (Zone ohne camstate ODER gleicher camstate)
// UND Distanz <= Radius (KUGEL um die Event-Mitte, NICHT die ganze Stage).
// Bei Ueberlappung: KS3 hat Vorrang, sonst die naechstgelegene Zone.
std::optional<int32_t> RE4VRKillswitch::zone_level_for(const std::optional<Entry>& e) const {
    if (!e.has_value() || !e->has_pos) {
        return std::nullopt;
    }

    std::optional<int32_t> best_lvl{};
    float best_d2 = 0.0f;

    for (const auto& z : m_zones) {
        if (z.stage != e->stage) {
            continue;
        }

        // Zone ohne camstate matcht jeden Event-Typ.
        if (z.camstate.has_value() && z.camstate != e->camstate) {
            continue;
        }

        const float dx = e->pos.x - z.x;
        const float dy = e->pos.y - z.y;
        const float dz = e->pos.z - z.z;
        const float d2 = dx * dx + dy * dy + dz * dz;

        if (d2 > z.r * z.r) {
            continue;
        }

        if (z.level == 3) {
            return 3;
        }

        if (!best_lvl.has_value() || d2 < best_d2) {
            best_lvl = z.level;
            best_d2 = d2;
        }
    }

    return best_lvl;
}

std::pair<bool, std::string> RE4VRKillswitch::mark_current_zone(int32_t level,
                                                                std::optional<float> radius) {
    // [KS4-ZONE 2026-07-17, so gewollt] Level 4 zusaetzlich erlaubt.
    if (level != 2 && level != 3 && level != 4) {
        return {false, "level muss 2, 3 oder 4 sein"};
    }

    const auto& e = m_cur_episode_entry.has_value() ? m_cur_episode_entry : m_last_episode_entry;

    if (!e.has_value() || !e->has_pos) {
        return {false, "kein Event-Eintritt erfasst"};
    }

    Zone z{};
    z.stage = e->stage;
    z.space = e->space;
    z.x = e->pos.x;
    z.y = e->pos.y;
    z.z = e->pos.z;
    z.r = radius.value_or(3.0f);
    z.level = level;
    z.camstate = e->camstate;
    z.name = "KS" + std::to_string(level) + " stage=" + std::to_string(e->stage)
        + " cam=" + state_str(e->camstate);

    const std::string name = z.name;
    m_zones.push_back(std::move(z));
    save_zones();

    return {true, name};
}

std::pair<bool, std::string> RE4VRKillswitch::remove_last_zone() {
    if (m_zones.empty()) {
        return {false, "keine Zonen"};
    }

    const std::string name = m_zones.back().name;
    m_zones.pop_back();
    save_zones();

    return {true, name};
}

int32_t RE4VRKillswitch::reload_zones() {
    load_zones();

    return static_cast<int32_t>(m_zones.size());
}

// ============================================================================
// Enum-Aufloesung
// ============================================================================

// CamState-Namen -> Enum-Ints (einmalig, sobald die TDB bereit ist).
void RE4VRKillswitch::resolve_state_vals() {
    if (m_state_vals_ok) {
        return;
    }

    auto* td = sdk::find_type_definition(CAMSTATE_ENUM);

    if (td == nullptr) {
        return;
    }

    std::unordered_map<std::string, int32_t> name_to_int{};

    for (auto* f : td->get_fields()) {
        if (f == nullptr) {
            continue;
        }

        try {
            if (!f->is_static()) {
                continue;
            }

            const char* nm = f->get_name();

            if (nm == nullptr) {
                continue;
            }

            name_to_int[nm] = f->get_data<int32_t>(nullptr);
        } catch (...) {
        }
    }

    // Nichts aufgeloest (TDB noch nicht bereit?) -> NICHT cachen, naechsten
    // Frame neu versuchen. Bis dahin defaulten die Helfer sicher.
    if (name_to_int.empty()) {
        return;
    }

    const auto fill = [&](auto& dst, const auto& names) {
        for (const char* n : names) {
            const auto it = name_to_int.find(n);

            if (it != name_to_int.end()) {
                dst.insert(it->second);
            }
        }
    };

    fill(m_gameplay_vals, GAMEPLAY_CAM_STATES);
    fill(m_pinrelease_vals, PIN_RELEASE_CAM_STATES);
    fill(m_ks2_vals, KS2_CAM_STATES);
    fill(m_ks3_vals, KS3_CAM_STATES);
    fill(m_ks4_vals, KS4_CAM_STATES);

    const auto pick = [&](const char* n) -> std::optional<int32_t> {
        const auto it = name_to_int.find(n);

        return it != name_to_int.end() ? std::optional<int32_t>{it->second} : std::nullopt;
    };

    m_damage_int = pick("Damage");
    m_hookshot_int = pick("HookShot");
    m_gimmick_int = pick("Gimmick");
    m_landing_int = pick("Landing");
    m_forcecrouch_int = pick("ForceCrouch");

    m_state_vals_ok = true;
}

// true = voller VR-Zustand erlaubt. nil/unaufloesbar -> sicher AN.
bool RE4VRKillswitch::is_gameplay_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return true;
    }

    resolve_state_vals();

    if (!m_state_vals_ok) {
        return true;   // nicht abwuergen
    }

    return m_gameplay_vals.count(*st) > 0;
}

bool RE4VRKillswitch::is_pinrelease_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return false;
    }

    resolve_state_vals();

    if (!m_state_vals_ok) {
        return false;
    }

    return m_pinrelease_vals.count(*st) > 0;
}

// [ECHTES RUNTERSPRINGEN] "luftig" = Flug- ODER Landephase.
bool RE4VRKillswitch::is_airborne_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return false;
    }

    if (is_pinrelease_camstate(st)) {
        return true;
    }

    return m_landing_int.has_value() && *st == *m_landing_int;
}

// Generischer Stufen-Matcher. Der Regel-Zweig (rules) der Lua ist tot -- alle
// drei Regel-Listen sind leer -- und entfaellt hier deshalb; die CamState-
// Listen sind das, was tatsaechlich greift.
bool RE4VRKillswitch::is_ks2_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return false;
    }

    resolve_state_vals();

    return m_ks2_vals.count(*st) > 0;
}

bool RE4VRKillswitch::is_ks3_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return false;
    }

    resolve_state_vals();

    return m_ks3_vals.count(*st) > 0;
}

bool RE4VRKillswitch::is_ks4_camstate(std::optional<int32_t> st) {
    if (!st.has_value()) {
        return false;
    }

    resolve_state_vals();

    return m_ks4_vals.count(*st) > 0;
}

// --- Lazy aufgeloeste Prioritaeten / GimmickTypes -------------------------
// Muster durchweg wie in der Lua: TDB im 1. Frame evtl. noch nicht bereit ->
// nicht cachen, naechster Frame neu.

std::optional<int32_t> RE4VRKillswitch::coop_jacked_prio() {
    if (!m_coop_jacked_prio.has_value()) {
        m_coop_jacked_prio = re4vr::enum_value(OCCUPIED_ENUM, "PL_JACKED_COOP_READY");
    }

    return m_coop_jacked_prio;
}

// [GRAPPLED_FATAL] Der TOEDLICHE Griff ist ein EIGENER Enum-Wert -- darum ein
// SET aus mehreren Prios statt eines einzelnen Werts. Loest sich einer der
// Namen nicht auf, zaehlen die anderen weiter (kein Totalausfall).
const std::unordered_set<int32_t>* RE4VRKillswitch::grappled_prio() {
    if (!m_grappled_prio.has_value()) {
        std::unordered_set<int32_t> set{};

        for (const char* nm : {"GRAPPLED", "GRAPPLED_FATAL"}) {
            if (const auto v = re4vr::enum_value(OCCUPIED_ENUM, nm); v.has_value()) {
                set.insert(*v);
            }
        }

        if (set.empty()) {
            return nullptr;   // TDB noch nicht bereit -> nicht cachen
        }

        m_grappled_prio = std::move(set);
    }

    return &*m_grappled_prio;
}

const std::unordered_set<int32_t>* RE4VRKillswitch::gondola_vals() {
    if (!m_gondola_vals.has_value()) {
        m_gondola_vals = enum_values_where(GIMMICKTYPE_ENUM,
                                           [](const char* n) { return starts_with(n, "Gondola"); });
    }

    return m_gondola_vals.has_value() ? &*m_gondola_vals : nullptr;
}

const std::unordered_set<int32_t>* RE4VRKillswitch::legtrap_vals() {
    if (!m_legtrap_vals.has_value()) {
        m_legtrap_vals = enum_values_where(
            GIMMICKTYPE_ENUM, [](const char* n) { return starts_with(n, "LegHoldTrap"); });
    }

    return m_legtrap_vals.has_value() ? &*m_legtrap_vals : nullptr;
}

const std::unordered_set<int32_t>* RE4VRKillswitch::elevtrouble_vals() {
    if (!m_elevtrouble_vals.has_value()) {
        m_elevtrouble_vals = enum_values_where(
            GIMMICKTYPE_ENUM, [](const char* n) { return starts_with(n, "ElevatorTrouble"); });
    }

    return m_elevtrouble_vals.has_value() ? &*m_elevtrouble_vals : nullptr;
}

// [DEMO_EXCLUDE] Prioritaeten, deren NAME "FOR_DEMO" ENTHAELT (Substring, nicht
// Praefix) = gescriptete Demo-Events, die NATIV laufen sollen.
const std::unordered_set<int32_t>* RE4VRKillswitch::demo_prios() {
    if (!m_demo_prios.has_value()) {
        m_demo_prios = enum_values_where(OCCUPIED_ENUM, [](const char* n) {
            return std::strstr(n, "FOR_DEMO") != nullptr;
        });
    }

    return m_demo_prios.has_value() ? &*m_demo_prios : nullptr;
}

std::optional<int32_t> RE4VRKillswitch::squeeze_high_prio() {
    if (!m_squeeze_high_prio.has_value()) {
        m_squeeze_high_prio = re4vr::enum_value(OCCUPIED_ENUM, "CH_JACKED_GMK_HIGH");
    }

    return m_squeeze_high_prio;
}

std::optional<int32_t> RE4VRKillswitch::minidemo_prio() {
    if (!m_minidemo_prio.has_value()) {
        m_minidemo_prio = re4vr::enum_value(OCCUPIED_ENUM, "MINI_DEMO");
    }

    return m_minidemo_prio;
}

std::optional<int32_t> RE4VRKillswitch::gfix_low_prio() {
    if (!m_gfix_low_prio.has_value()) {
        m_gfix_low_prio = re4vr::enum_value(OCCUPIED_ENUM, "CH_JACKED_GMK_LOW");

        if (!m_gfix_low_prio.has_value()) {
            m_gfix_low_prio = 3;   // Fallback: Dump zeigte (3)
        }
    }

    return m_gfix_low_prio;
}

// chainsaw.PlayerDefine.State ist eine BITMASKE; ParentGimmick = Bit 53.
void RE4VRKillswitch::resolve_parentgimmick_bit() {
    if (m_parentgimmick_bit.has_value()) {
        return;
    }

    auto* td = sdk::find_type_definition("chainsaw.PlayerDefine.State");

    if (td == nullptr) {
        return;
    }

    for (auto* f : td->get_fields()) {
        if (f == nullptr) {
            continue;
        }

        try {
            if (!f->is_static()) {
                continue;
            }

            const char* nm = f->get_name();

            if (nm == nullptr || std::strcmp(nm, "ParentGimmick") != 0) {
                continue;
            }

            // 64 Bit: das Bit steht bei 2^53, ein int32 wuerde es verlieren.
            m_parentgimmick_bit = f->get_data<uint64_t>(nullptr);

            return;
        } catch (...) {
            return;
        }
    }
}


// ============================================================================
// Getter (Singletons ueber den Frame-Cache)
// ============================================================================

::REManagedObject* RE4VRKillswitch::get_camera_system() {
    return re4vr::fc::managed_singleton("chainsaw.CameraSystem");
}

::REManagedObject* RE4VRKillswitch::get_gui_manager() {
    return re4vr::fc::managed_singleton("chainsaw.GuiManager");
}

::REManagedObject* RE4VRKillswitch::get_busy_controller() {
    auto* csys = get_camera_system();

    if (csys == nullptr) {
        return nullptr;
    }

    auto* main = re4vr::call_safe<::REManagedObject*>(csys, "get_MainCameraController");

    if (main == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController");
}

// [ZONES] Welt-Position des Spielers (Body-Transform) -- exakt die Quelle, die
// auch der Monitor anzeigt, damit erfasste Zonen-Mitte und Laufzeit-Abgleich
// identisch sind.
std::optional<glm::vec3> RE4VRKillswitch::get_player_pos() {
    auto* tf = re4vr::fc::body_tf();

    if (tf == nullptr) {
        return std::nullopt;
    }

    glm::vec3 p{};

    if (!re4vr::obj_get_vec3(tf, "get_Position", p)) {
        return std::nullopt;
    }

    return p;
}

std::pair<std::optional<int32_t>, std::optional<int32_t>> RE4VRKillswitch::read_stage_space() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return {std::nullopt, std::nullopt};
    }

    std::optional<int32_t> stage{};
    std::optional<int32_t> space{};

    if (int32_t v = 0; re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", v)) {
        stage = v;
    }

    if (int32_t v = 0; re4vr::try_call<int32_t>(ctx, "get_CurrentSpaceID", v)) {
        space = v;
    }

    return {stage, space};
}

// Occupied-Priority des Spielers. Lua packt ein userdata-Ergebnis ueber
// value__ aus -- nativ kommt der Enum direkt als Integer.
std::optional<int32_t> RE4VRKillswitch::occupied_priority() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return std::nullopt;
    }

    auto* occ = re4vr::call_safe<::REManagedObject*>(ctx, "get_OccupiedInfo");

    if (occ == nullptr) {
        return std::nullopt;
    }

    // [PORTFIX 2026-09-06] chainsaw.OccupiedMediatorPriority ist ein
    // System.Byte-Enum -- die Engine gibt AL zurueck, die oberen 24 Bit sind
    // ABI-seitig undefiniert. Als int32 gelesen kam hier Muell an und KEIN
    // Priority-Zweig (Grapple, Raeuberleiter, Squeeze, Minidemo, GimmickFix)
    // hat je gematcht. Lua liest ueber invoke typrichtig.
    uint8_t p = 0;

    if (!re4vr::try_call<uint8_t>(occ, "get_Priority", p)) {
        return std::nullopt;
    }

    return static_cast<int32_t>(p);
}

// ============================================================================
// Cutscene / Kamera
// ============================================================================

std::pair<bool, std::string> RE4VRKillswitch::is_real_cutscene() {
    if (auto* csys = get_camera_system(); csys != nullptr) {
        if (call_bool(csys, "get_IsEventCamera")) {
            return {true, "IsEventCamera"};
        }
    }

    if (auto* gm = get_gui_manager(); gm != nullptr) {
        // [FIX 2026-07-10] IsPlayingEvent haengt nach einem Event noch kurz
        // true, WAEHREND die PlayerCamera schon zurueck ist -> das war der
        // Uebergangs-3rd-Person nach der Cutscene. Nur als Cutscene werten,
        // wenn NICHT die Player-Kamera aktiv ist.
        if (call_bool(gm, "get_IsPlayingEvent")) {
            auto* busy = get_busy_controller();
            const bool is_pc = busy != nullptr && is_a(busy, g_player_cam_td);

            if (!is_pc) {
                return {true, "IsPlayingEvent"};
            }
        }
    }

    return {false, {}};
}

bool RE4VRKillswitch::is_player_camera_active() {
    auto* busy = get_busy_controller();

    return busy != nullptr && is_a(busy, g_player_cam_td);
}

// PlayerCameraState (Enum-Zahl) vom aktiven PlayerCameraController.
// _CurrentStateParam kann ein WERTtyp sein -- dann liefert get_data_raw mit
// dem Container-Flag true den Struct-Zeiger, sonst ist es ein Managed Object.
// (Genau die Falle aus reference_re4_cpp_get_data_raw_container_flag.)
std::optional<int32_t> RE4VRKillswitch::read_cam_state(::REManagedObject* busy) {
    if (busy == nullptr) {
        return std::nullopt;
    }

    auto* busy_def = type_def_of(busy);

    if (busy_def == nullptr) {
        return std::nullopt;
    }

    auto* sp_field = busy_def->get_field("_CurrentStateParam");

    if (sp_field == nullptr) {
        return std::nullopt;
    }

    void* sp_ptr = nullptr;
    bool sp_is_value = false;

    try {
        auto* sp_type = sp_field->get_type();
        sp_is_value = sp_type != nullptr && sp_type->is_value_type();

        if (sp_is_value) {
            sp_ptr = sp_field->get_data_raw(busy, false);
        } else {
            sp_ptr = sp_field->get_data<::REManagedObject*>(busy);
        }
    } catch (...) {
        return std::nullopt;
    }

    if (sp_ptr == nullptr) {
        return std::nullopt;
    }

    sdk::RETypeDefinition* sp_def = nullptr;

    if (sp_is_value) {
        sp_def = sp_field->get_type();
    } else {
        sp_def = utility::re_managed_object::get_type_definition(
            reinterpret_cast<::REManagedObject*>(sp_ptr));
    }

    if (sp_def == nullptr) {
        return std::nullopt;
    }

    auto* st_field = sp_def->get_field("<State>k__BackingField");

    if (st_field == nullptr) {
        return std::nullopt;
    }

    try {
        auto* raw = st_field->get_data_raw(sp_ptr, sp_is_value);

        if (raw == nullptr) {
            return std::nullopt;
        }

        return *reinterpret_cast<int32_t*>(raw);
    } catch (...) {
        return std::nullopt;
    }
}

// [GONDEL] GimmickType -- selbes StateParam wie read_cam_state, nur ein
// anderes Feld.
std::optional<int32_t> RE4VRKillswitch::read_gimmick_type(::REManagedObject* busy) {
    if (busy == nullptr) {
        return std::nullopt;
    }

    auto* busy_def = type_def_of(busy);

    if (busy_def == nullptr) {
        return std::nullopt;
    }

    auto* sp_field = busy_def->get_field("_CurrentStateParam");

    if (sp_field == nullptr) {
        return std::nullopt;
    }

    void* sp_ptr = nullptr;
    bool sp_is_value = false;

    try {
        auto* sp_type = sp_field->get_type();
        sp_is_value = sp_type != nullptr && sp_type->is_value_type();

        if (sp_is_value) {
            sp_ptr = sp_field->get_data_raw(busy, false);
        } else {
            sp_ptr = sp_field->get_data<::REManagedObject*>(busy);
        }
    } catch (...) {
        return std::nullopt;
    }

    if (sp_ptr == nullptr) {
        return std::nullopt;
    }

    sdk::RETypeDefinition* sp_def = nullptr;

    if (sp_is_value) {
        sp_def = sp_field->get_type();
    } else {
        sp_def = utility::re_managed_object::get_type_definition(
            reinterpret_cast<::REManagedObject*>(sp_ptr));
    }

    if (sp_def == nullptr) {
        return std::nullopt;
    }

    auto* gt_field = sp_def->get_field("<GimmickType>k__BackingField");

    if (gt_field == nullptr) {
        return std::nullopt;
    }

    try {
        auto* raw = gt_field->get_data_raw(sp_ptr, sp_is_value);

        if (raw == nullptr) {
            return std::nullopt;
        }

        return *reinterpret_cast<int32_t*>(raw);
    } catch (...) {
        return std::nullopt;
    }
}

// [ELEV_LIVE_CAM] Frischer PlayerCameraState. WARUM: m_current_cam_state wird
// erst im is_pc-Branch WEIT unten gesetzt -- die Force-Branches returnen lange
// davor und wuerden gegen einen eingefrorenen Wert vergleichen.
std::optional<int32_t> RE4VRKillswitch::read_live_cam_state() {
    auto* busy = get_busy_controller();

    if (busy == nullptr || !is_a(busy, g_player_cam_td)) {
        return std::nullopt;
    }

    return read_cam_state(busy);
}

// ============================================================================
// Spieler-Zustandsflags
// ============================================================================

bool RE4VRKillswitch::player_gimmick_active() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    for (const char* fn : GIMMICK_FLAGS) {
        if (call_bool(ctx, fn)) {
            return true;
        }
    }

    return false;
}

// [COOP-JACKED / RAEUBERLEITER] Solides Merkmal: OccupiedInfo.Priority ==
// PL_JACKED_COOP_READY (im normalen Gameplay ist die Priority UNSET).
bool RE4VRKillswitch::player_is_coop_jacked() {
    const auto want = coop_jacked_prio();

    if (!want.has_value()) {
        return false;
    }

    const auto prio = occupied_priority();

    return prio.has_value() && *prio == *want;
}

// [GRAPPLED] Gegner packt Leon. Gemeinsames Merkmal BEIDER Griff-Phasen:
// OccupiedInfo.Priority in {GRAPPLED, GRAPPLED_FATAL}.
bool RE4VRKillswitch::player_is_grappled() {
    const auto* want = grappled_prio();

    if (want == nullptr) {
        return false;
    }

    const auto prio = occupied_priority();

    return prio.has_value() && want->count(*prio) > 0;
}

// [KICK/BARREL] Leons Tritt gegen Fass/Kiste. Flag ist im Stehen false.
bool RE4VRKillswitch::player_is_boxbreak() {
    return call_bool(re4vr::fc::ctx(), "get_IsBoxBreak");
}

// [LEITER] an/auf der Leiter.
bool RE4VRKillswitch::player_is_ladder() {
    return call_bool(re4vr::fc::ctx(), "get_IsLadder");
}

// ============================================================================
// MotionFsm2 / Motion -- Component-Handles gecacht
// ============================================================================

::REManagedObject* RE4VRKillswitch::find_mfsm2(::REManagedObject* body) {
    if (body == nullptr) {
        return nullptr;
    }

    if (g_mfsm2_td != nullptr) {
        auto* m = re4vr::call_safe<::REManagedObject*>(body, "getComponent(System.Type)",
                                                       g_mfsm2_td->get_runtime_type());

        if (m != nullptr) {
            return m;
        }
    }

    // Fallback der Lua: alle Komponenten durchgehen und den Typnamen pruefen.
    auto* comps = re4vr::call_safe<::REManagedObject*>(body, "get_Components");

    if (comps == nullptr) {
        return nullptr;
    }

    const int32_t n = re4vr::array_size(comps);

    for (int32_t i = 0; i < n; ++i) {
        auto* c = re4vr::array_element(comps, i);

        if (c == nullptr) {
            continue;
        }

        const std::string tn = to_lower(full_name_of(c));

        if (tn.find("motionfsm2") != std::string::npos) {
            return c;
        }
    }

    return nullptr;
}

::REManagedObject* RE4VRKillswitch::mfsm2_for_body() {
    auto* body = re4vr::fc::body_go();

    if (body == nullptr) {
        m_mfsm2_comp = nullptr;
        m_mfsm2_body = nullptr;

        return nullptr;
    }

    if (body != m_mfsm2_body) {
        m_mfsm2_comp = nullptr;
        m_mfsm2_body = body;
    }

    if (m_mfsm2_comp == nullptr) {
        m_mfsm2_comp = find_mfsm2(body);
    }

    return m_mfsm2_comp;
}

::REManagedObject* RE4VRKillswitch::motion_for_body() {
    auto* body = re4vr::fc::body_go();

    if (body == nullptr) {
        m_motion_comp = nullptr;
        m_motion_body = nullptr;

        return nullptr;
    }

    if (body != m_motion_body) {
        m_motion_comp = nullptr;
        m_motion_body = body;
    }

    if (m_motion_comp == nullptr && g_motion_td != nullptr) {
        m_motion_comp = re4vr::call_safe<::REManagedObject*>(
            body, "getComponent(System.Type)", g_motion_td->get_runtime_type());
    }

    return m_motion_comp;
}

// MotionFsm2.getCurrentNodeName(0) enthaelt "CROUCH", sobald die Hock-
// Locomotion laeuft.
bool RE4VRKillswitch::is_crouch_active() {
    auto* comp = mfsm2_for_body();

    if (comp == nullptr) {
        return false;
    }

    auto* node = re4vr::call_safe<::REManagedObject*>(comp, "getCurrentNodeName", 0);

    if (node == nullptr) {
        return false;
    }

    const std::string s = to_lower(mstr(node));

    return s.find("crouch") != std::string::npos;
}

// [FP_ONLY/FEIN] Enthaelt IRGENDEIN MotionFsm2-Layer-Node das Token?
// (Substring, case-insensitiv)
bool RE4VRKillswitch::player_node_has(const char* token) {
    if (token == nullptr || *token == '\0') {
        return false;
    }

    auto* comp = mfsm2_for_body();

    if (comp == nullptr) {
        return false;
    }

    const std::string tok = to_lower(token);

    for (int32_t layer = 0; layer <= 7; ++layer) {
        auto* n = re4vr::call_safe<::REManagedObject*>(comp, "getCurrentNodeName", layer);

        if (n == nullptr) {
            continue;
        }

        if (to_lower(mstr(n)).find(tok) != std::string::npos) {
            return true;
        }
    }

    return false;
}

// [CLIP-EBENE] true wenn IRGENDEINE gejackte Motion-Layer einen JackFrom-Helfer
// hat, dessen Name auf das Token ENDET. Ende-Match trennt ":Open" (Tuer) von
// ":OpenLow" (Schublade) -- "Open" ist Teilstring von "OpenLow", daher KEIN
// Substring-, sondern Suffix-Match.
bool RE4VRKillswitch::player_jack_has(const char* token) {
    if (token == nullptr || *token == '\0') {
        return false;
    }

    auto* m = motion_for_body();

    if (m == nullptr) {
        return false;
    }

    const std::string tok = to_lower(token);

    int32_t lc = 0;

    if (!re4vr::try_call<int32_t>(m, "getLayerCount", lc)) {
        lc = 0;
    }

    for (int32_t i = 0; i < lc; ++i) {
        auto* lay = re4vr::call_safe<::REManagedObject*>(m, "getLayer", i);

        if (lay == nullptr || !call_bool(lay, "get_Jacked")) {
            continue;
        }

        auto* jf = re4vr::call_safe<::REManagedObject*>(lay, "get_JackFrom");

        if (jf == nullptr) {
            continue;
        }

        auto* nm = re4vr::call_safe<::REManagedObject*>(jf, "get_Name");

        if (nm == nullptr) {
            continue;
        }

        const std::string s = to_lower(mstr(nm));

        if (s.size() >= tok.size() && s.compare(s.size() - tok.size(), tok.size(), tok) == 0) {
            return true;
        }
    }

    return false;
}

// [LEITER-AUSSTIEG] Der gejackte Kletter-Layer traegt die Phase in der
// ANIM-LAENGE (get_EndFrame), NICHT im Clip-Namen: Einstieg 38 fr, Loop 19 fr,
// AUSSTIEG 166 fr. Muss VOR player_is_ladder (KS4) geprueft werden.
bool RE4VRKillswitch::player_is_ladder_exit(bool cam_is_gameplay) {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    // [STICKY] Zwei Flicker-Quellen, beide vom Latch abgefangen. Nicht bei
    // IsLadder-false loesen, sondern erst wenn der CamState WIEDER ECHTES
    // GAMEPLAY ist (Rueberklettern ganz durch).
    if (m_ladder_exit_latched) {
        if (cam_is_gameplay) {
            m_ladder_exit_latched = false;

            return false;
        }

        return true;
    }

    if (!call_bool(ctx, "get_IsLadder")) {
        return false;
    }

    auto* m = motion_for_body();

    if (m == nullptr) {
        return false;
    }

    int32_t lc = 0;

    if (!re4vr::try_call<int32_t>(m, "getLayerCount", lc)) {
        lc = 0;
    }

    for (int32_t i = 0; i < lc; ++i) {
        auto* lay = re4vr::call_safe<::REManagedObject*>(m, "getLayer", i);

        if (lay == nullptr || !call_bool(lay, "get_Jacked")) {
            continue;
        }

        float ef = 0.0f;

        if (re4vr::try_call<float>(lay, "get_EndFrame", ef)
            && ef > static_cast<float>(LADDER_EXIT_ENDFRAME)) {
            m_ladder_exit_latched = true;

            return true;
        }
    }

    return false;
}

// ============================================================================
// Spezial-Erkennungen
// ============================================================================

// chainsaw.PlayerDefine.State ist eine BITMASKE; ParentGimmick = Bit 53
// (= 9007199254740992). Der belegte Dump-Wert ist 1161928703861587969 -- weit
// ueber 2^53, deshalb MUSS das uint64 sein. Lua rechnet den Test per Modulo
// (2^54 / 2^53), nativ ist die Maske direkt lesbar.
bool RE4VRKillswitch::has_parent_gimmick() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    const auto v = re4vr::get_state_bits(ctx);

    if (!v.has_value()) {
        return false;
    }

    constexpr uint64_t PARENT_GIMMICK_BIT = 9007199254740992ULL;   // 2^53

    return (*v % (PARENT_GIMMICK_BIT * 2ULL)) >= PARENT_GIMMICK_BIT;
}

// [GONDEL] Anders als die Aufzuege meldet sich die Gondel per BENANNTEM
// GimmickType -> kein Positions-Raten noetig.
bool RE4VRKillswitch::is_on_gondola() {
    const auto* vals = gondola_vals();

    if (vals == nullptr) {
        return false;
    }

    auto* busy = get_busy_controller();

    if (busy == nullptr || !is_a(busy, g_player_cam_td)) {
        return false;
    }

    const auto gt = read_gimmick_type(busy);

    return gt.has_value() && vals->count(*gt) > 0;
}

// [BEARTRAP] BEIDE Bedingungen gekoppelt: CamState == Gimmick UND GimmickType
// == LegHoldTrap*. Warum gekoppelt: der GimmickType steht im StateParam auch
// dann noch, wenn der State schon weiter ist (STALE). Das Gimmick-Gate ist die
// Flanke, die den KS wieder loslaesst.
bool RE4VRKillswitch::is_in_legholdtrap() {
    const auto* vals = legtrap_vals();

    if (vals == nullptr) {
        return false;
    }

    // gimmick_int aufloesen: wird sonst erst im is_pc-Branch WEIT unten
    // gesetzt, also nach diesem Force-Branch. Idempotent/billig.
    resolve_state_vals();

    if (!m_gimmick_int.has_value()) {
        return false;
    }

    auto* busy = get_busy_controller();

    if (busy == nullptr || !is_a(busy, g_player_cam_td)) {
        return false;
    }

    // [KS-FORCE-BRANCH] CamState hier LIVE vom busy lesen, NICHT
    // m_current_cam_state: das ist in Force-Branches eingefroren.
    if (read_cam_state(busy) != m_gimmick_int) {
        return false;
    }

    const auto gt = read_gimmick_type(busy);

    return gt.has_value() && vals->count(*gt) > 0;
}

// [ELEVATOR_TROUBLE] Erkennung wie Beartrap: GEKOPPELT CamState==Gimmick UND
// GimmickType-Name beginnt mit "ElevatorTrouble".
bool RE4VRKillswitch::is_elevator_trouble() {
    const auto* vals = elevtrouble_vals();

    if (vals == nullptr) {
        return false;
    }

    resolve_state_vals();

    if (!m_gimmick_int.has_value()) {
        return false;
    }

    auto* busy = get_busy_controller();

    if (busy == nullptr || !is_a(busy, g_player_cam_td)) {
        return false;
    }

    if (read_cam_state(busy) != m_gimmick_int) {
        return false;
    }

    const auto gt = read_gimmick_type(busy);

    return gt.has_value() && vals->count(*gt) > 0;
}

// [GIMMICK_KS3_SPOT] Ans EVENT gekoppelt (kein Radius/Zeit).
// (a) GimmickMotion-Kamera -- in PRIO_ONLY_STAGES uebersprungen.
// (b) Occupied CH_JACKED_GMK_HIGH -- haelt am Ende laenger als die Cam
//     (Blitz-Schutz: die Cam laesst 1-2 Frames FRUEHER los).
bool RE4VRKillswitch::is_gimmick_ks3_spot() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || GKS3_STAGES.count(*stage) == 0) {
        return false;
    }

    bool raw = false;

    if (g_gimmick_motion_td != nullptr && PRIO_ONLY_STAGES.count(*stage) == 0) {
        if (auto* busy = get_busy_controller(); busy != nullptr) {
            raw = is_a(busy, g_gimmick_motion_td);
        }
    }

    if (!raw) {
        if (const auto want = squeeze_high_prio(); want.has_value()) {
            const auto prio = occupied_priority();
            raw = prio.has_value() && *prio == *want;
        }
    }

    if (raw) {
        m_squeeze_latch_t = clock_now();

        return true;
    }

    // Exit-Grace: KS4 nach dem Event ~0.3s halten -> Cam->Gameplay-Uebergang
    // ohne 3rd-Person-Blitz.
    return (clock_now() - m_squeeze_latch_t) < 0.30;
}

// [MINIDEMO_KS4] Dieselbe Mechanik wie is_gimmick_ks3_spot, aber BEWUSST
// GETRENNT: eigene Stage-Liste (MINI_DEMO ist eine allgemeine Demo-Prio) und
// eigener Reason ("ks3_gimmick" schaltet in firstperson den Squeeze-Head-Nagel
// + Antibeam; beides gehoert hier nicht hin).
bool RE4VRKillswitch::is_minidemo_ks4_spot() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || MINIDEMO_KS4_STAGES.count(*stage) == 0) {
        return false;
    }

    bool raw = false;

    if (g_gimmick_motion_td != nullptr && PRIO_ONLY_STAGES.count(*stage) == 0) {
        if (auto* busy = get_busy_controller(); busy != nullptr) {
            raw = is_a(busy, g_gimmick_motion_td);
        }
    }

    if (!raw) {
        if (const auto want = minidemo_prio(); want.has_value()) {
            const auto prio = occupied_priority();
            raw = prio.has_value() && *prio == *want;
        }
    }

    if (raw) {
        m_minidemo_latch_t = clock_now();

        return true;
    }

    return (clock_now() - m_minidemo_latch_t) < 0.30;
}

// [GFIX_KS4] Streng gegated: NUR diese Stage UND diese Kamera UND diese
// Priority -- die A-Halte-Stellen (Prio 6) fallen NICHT hinein.
bool RE4VRKillswitch::is_gimmickfix_ks4_spot() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || GFIX_KS4_STAGES.count(*stage) == 0) {
        return false;
    }

    if (g_gimmick_fix_td == nullptr) {
        return false;
    }

    auto* busy = get_busy_controller();

    if (busy == nullptr || !is_a(busy, g_gimmick_fix_td)) {
        return false;
    }

    const auto want = gfix_low_prio();
    const auto prio = occupied_priority();

    return prio.has_value() && want.has_value() && *prio == *want;
}

// [GFIX_KS5] Sauberes Trennmerkmal zum Durchquetschen ist die KAMERA:
// Durchquetschen = GimmickMotion | dieses Event = GimmickFix. Beide
// Bedingungen (Cam UND Prio) werden verlangt.
bool RE4VRKillswitch::is_gimmickfix_ks5_spot() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || GFIX_KS5_STAGES.count(*stage) == 0) {
        return false;
    }

    bool raw = false;

    if (g_gimmick_fix_td != nullptr) {
        if (auto* busy = get_busy_controller(); busy != nullptr && is_a(busy, g_gimmick_fix_td)) {
            if (const auto want = squeeze_high_prio(); want.has_value()) {
                const auto prio = occupied_priority();
                raw = prio.has_value() && *prio == *want;
            }
        }
    }

    if (raw) {
        m_gfix_latch_t = clock_now();

        return true;
    }

    return (clock_now() - m_gfix_latch_t) < 0.30;
}

// [DEMO_EXCLUDE] Laeuft gerade ein Gimmick-Motion-Event? Ohne Stage-Grenze.
bool RE4VRKillswitch::is_gimmick_motion_now() {
    if (g_gimmick_motion_td == nullptr) {
        return false;
    }

    auto* busy = get_busy_controller();

    return busy != nullptr && is_a(busy, g_gimmick_motion_td);
}

bool RE4VRKillswitch::is_demo_priority_now() {
    const auto* vals = demo_prios();

    if (vals == nullptr) {
        return false;
    }

    const auto p = occupied_priority();

    return p.has_value() && vals->count(*p) > 0;
}

// [CARRY_SWITCH] <TargetJacked> ist true beim Tragen (auch durch die Tuer),
// nil sobald die Cutscene sie uebernimmt.
bool RE4VRKillswitch::is_carrying() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    try {
        return re4vr::get_field_bool_v(ctx, "<TargetJacked>k__BackingField");
    } catch (...) {
        return false;
    }
}

// [ELEVATOR_59100] Aufzug gm81_511 (GmElevator). Component gecacht, bei
// Stage-Wechsel/Verlust neu gesucht.
//
// [ARRAY-BINDING] _MoveArray wird in der Lua mit get_size()/get_element()
// gelesen -- das sind REFramework-BINDINGS auf ein System.Array, KEINE managed
// Methoden. Als get_Count/get_Item portiert liefern sie stumm 0 Elemente und
// der Aufzug wuerde nie erkannt.
bool RE4VRKillswitch::is_riding_elevator_59100() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || *stage != 59100) {
        m_elev59100_comp = nullptr;

        return false;
    }

    if (m_elev59100_comp == nullptr) {
        auto* gm = re4vr::fc::managed_singleton("chainsaw.GimmickManager");

        if (gm == nullptr) {
            return false;
        }

        ::REManagedObject* arr = nullptr;

        try {
            arr = re4vr::get_field_object(gm, "_MoveArray");
        } catch (...) {
            arr = nullptr;
        }

        if (arr == nullptr) {
            return false;
        }

        const int32_t n = re4vr::array_size(arr);

        for (int32_t i = 0; i < n; ++i) {
            auto* core = re4vr::array_element(arr, i);

            if (core == nullptr) {
                continue;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(core, "get_GameObject");

            if (go == nullptr || g_elev_td_59100 == nullptr) {
                continue;
            }

            auto* comp = re4vr::call_safe<::REManagedObject*>(
                go, "getComponent(System.Type)", g_elev_td_59100->get_runtime_type());

            if (comp != nullptr) {
                // Stage 59100 hat nur einen GmElevator (gm81_511).
                m_elev59100_comp = comp;
                break;
            }
        }
    }

    if (m_elev59100_comp == nullptr) {
        return false;
    }

    return call_bool(m_elev59100_comp, "get_IsPlInElevator");
}

bool RE4VRKillswitch::is_jetski_stage() {
    const auto [stage, space] = read_stage_space();

    return stage.has_value() && JETSKI_KS4_STAGES.count(*stage) > 0;
}

// [JETSKI SITZ-ERKENNUNG] Nur die Anfahr-Stage 59103 startet, waehrend Leon
// noch zu Fuss laeuft -> dort zusaetzlich die Fahrt-Kamera verlangen. Die
// reinen Fahr-Stages 592xx: da sitzt man immer schon.
bool RE4VRKillswitch::is_on_jetski() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || JETSKI_KS4_STAGES.count(*stage) == 0) {
        return false;
    }

    if (*stage != 59103) {
        return true;
    }

    auto* busy = get_busy_controller();

    return busy != nullptr && is_a(busy, g_vehicle_camera_td);
}

// [BOAT] Generische "sitzt auf einem Boot"-Erkennung ueber die native Wahrheit
// (get_IsBoat), stageunabhaengig. Der Einstieg laeuft auf dem
// ActionCameraController -> so gewollt KS1; erst die FAHRT
// (VehicleCameraController) ist KS4.
bool RE4VRKillswitch::is_on_boat() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr || !call_bool(ctx, "get_IsBoat")) {
        return false;
    }

    if (auto* busy = get_busy_controller();
        busy != nullptr && g_action_camera_td != nullptr && is_a(busy, g_action_camera_td)) {
        return false;
    }

    return true;
}

// [RAILCAR_MODE] STAGE-UNABHAENGIG: getPlayerRailCar ~= nil UND ParentGimmick.
bool RE4VRKillswitch::is_on_railcar() {
    if (m_railcar_mgr == nullptr) {
        m_railcar_mgr = re4vr::fc::managed_singleton("chainsaw.RailCarManager");
    }

    if (m_railcar_mgr == nullptr) {
        return false;
    }

    auto* car = re4vr::call_safe<::REManagedObject*>(m_railcar_mgr, "getPlayerRailCar");

    if (car == nullptr) {
        // gar kein Spieler-Schienenwagen -> definitiv nicht auf dem Cart
        return false;
    }

    // (a) [EINSTIEGS-BLITZ-FIX] VehicleCameraController aktiv -> SOFORT
    // railcar_mode, auch bevor ParentGimmick gesetzt ist (sonst 1-2 Frames
    // else=KS1 = kurzer 3rd-Person-Blitz).
    if (auto* busy = get_busy_controller();
        busy != nullptr && g_vehicle_camera_td != nullptr && is_a(busy, g_vehicle_camera_td)) {
        return true;
    }

    // (b) sonst: an das Kart geparentet (ParentGimmick).
    resolve_parentgimmick_bit();

    if (!m_parentgimmick_bit.has_value()) {
        return false;
    }

    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    const auto st = re4vr::get_state_bits(ctx);

    if (!st.has_value()) {
        return false;
    }

    return (*st & *m_parentgimmick_bit) != 0ULL;
}

// [THROWSIGHT] Del-Lago-Bootkampf: Stage 46900_46900 UND Leons Body ist ans
// Boot-Objekt "gm02_500_00_1" reparented. Die reine Bootfahrt
// (gm02_500_00_2) bekommt bewusst KEINE Ausnahme.
bool RE4VRKillswitch::is_throwsight_stage() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || !space.has_value()) {
        return false;
    }

    if (*stage != 46900 || *space != 46900) {
        return false;
    }

    auto* tf = re4vr::fc::body_tf();

    if (tf == nullptr) {
        return false;
    }

    auto* current = tf;

    for (int i = 1; i <= 10; ++i) {
        auto* parent_tf = re4vr::call_safe<::REManagedObject*>(current, "get_Parent");

        if (parent_tf == nullptr) {
            return false;
        }

        if (auto* parent_go = re4vr::call_safe<::REManagedObject*>(parent_tf, "get_GameObject");
            parent_go != nullptr) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(parent_go, "get_Name");

            if (nm != nullptr) {
                const std::string name = mstr(nm);

                if (name.find("gm02_500_00_1") != std::string::npos) {
                    return true;
                }
            }
        }

        current = parent_tf;
    }

    return false;
}

// [MINECART_KS4] "cart" (ActionCam = Einstieg) | "cart2" (VehicleCam = Fahrt)
// | nullptr. Stage-Gate zwingend: ActionCam laeuft auch woanders.
const char* RE4VRKillswitch::minecart_ks4_kind() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || MINECART_KS4_STAGES.count(*stage) == 0) {
        return nullptr;
    }

    auto* busy = get_busy_controller();

    if (busy == nullptr) {
        return nullptr;
    }

    if (is_a(busy, g_action_camera_td)) {
        return "cart";
    }

    if (is_a(busy, g_vehicle_camera_td)) {
        return "cart2";
    }

    return nullptr;
}

// [ELEVATOR] Positions-Box, Aufzug 1.
bool RE4VRKillswitch::is_in_elevator() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || *stage != ELEV.stage) {
        return false;
    }

    const auto p = get_player_pos();

    if (!p.has_value()) {
        return false;
    }

    const float xmin = std::min(ELEV.x1, ELEV.x2) - ELEV.mxz;
    const float xmax = std::max(ELEV.x1, ELEV.x2) + ELEV.mxz;
    const float zmin = std::min(ELEV.z1, ELEV.z2) - ELEV.mxz;
    const float zmax = std::max(ELEV.z1, ELEV.z2) + ELEV.mxz;
    // [UNTEN] Pin erst ein Fitzel NACH dem Start, [OBEN] ein Fitzel VOR dem Ziel
    const float ymin = std::min(ELEV.y1, ELEV.y2) + ELEV_START_RELEASE;
    const float ymax = std::max(ELEV.y1, ELEV.y2) - ELEV_TOP_RELEASE;

    return p->x >= xmin && p->x <= xmax && p->z >= zmin && p->z <= zmax && p->y >= ymin
        && p->y <= ymax;
}

// [ELEVATOR2] Y-Puffer nach AUSSEN -- ein Fitzel nach innen schnitt oben genau
// den gemessenen Endpunkt ab ("bricht beim Hochfahren").
bool RE4VRKillswitch::is_in_elevator2_zone() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || *stage != ELEV2.stage) {
        return false;
    }

    const auto p = get_player_pos();

    if (!p.has_value()) {
        return false;
    }

    const float xmin = std::min(ELEV2.x1, ELEV2.x2) - ELEV2.mxz;
    const float xmax = std::max(ELEV2.x1, ELEV2.x2) + ELEV2.mxz;
    const float zmin = std::min(ELEV2.z1, ELEV2.z2) - ELEV2.mxz;
    const float zmax = std::max(ELEV2.z1, ELEV2.z2) + ELEV2.mxz;
    const float ymin = std::min(ELEV2.y1, ELEV2.y2) - ELEV2_Y_PAD;
    const float ymax = std::max(ELEV2.y1, ELEV2.y2) + ELEV2_Y_PAD;

    return p->x >= xmin && p->x <= xmax && p->z >= zmin && p->z <= zmax && p->y >= ymin
        && p->y <= ymax;
}

// [ELEVATOR3] Lift, Stage 55300. KEIN ymax (bewusst): der Schacht ist eine
// Saeule, und wie hoch die Fahrt endet, wurde nie gemessen -- jedes geratene
// ymax lag zu tief und der Force fiel MITTEN in der Fahrt weg.
bool RE4VRKillswitch::is_in_elevator3_zone() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || *stage != 55300) {
        return false;
    }

    const auto p = get_player_pos();

    if (!p.has_value()) {
        return false;
    }

    return p->x >= 175.5f && p->x <= 181.6f && p->z >= 85.8f && p->z <= 92.0f && p->y >= -75.14f;
}

bool RE4VRKillswitch::is_parented_to_elevator() {
    // Wird von RE4VRMotion gesetzt.
    return re4vr::lua_get_tribool("__re4_on_elevator2") == 1;
}

// [ELEVATOR5] Aufzug Space 56200. BEIDE Stages: die Stage WECHSELT waehrend
// der Fahrt (56300 unten <-> 56201 oben).
bool RE4VRKillswitch::is_in_elevator5_cabin() {
    const auto [stage, space] = read_stage_space();

    if (!stage.has_value() || (*stage != 56300 && *stage != 56201)) {
        return false;
    }

    const auto p = get_player_pos();

    if (!p.has_value()) {
        return false;
    }

    // Sicherheitsnetz gegen ganz andere Orte der Stage.
    if (p->y < -10.0f || p->y > 55.0f) {
        return false;
    }

    const float dx = p->x - ELEV5_X;
    const float dz = p->z - ELEV5_Z;

    return (dx * dx + dz * dz) <= (ELEV5_R * ELEV5_R);
}

// [ELEVATOR5 LATCH] Startschuss = der Button-Gimmick, NICHT die Y-Bewegung:
// waehrend der Fahrt ist der CamState 0/Normal und von normalem Stehen
// ununterscheidbar. Scharf: in der Kabine + an einem Endpunkt + der Gimmick
// ist gerade VORBEI (Flanke 15 -> nicht-15).
bool RE4VRKillswitch::elevator5_update() {
    const auto p = get_player_pos();

    if (!p.has_value()) {
        m_elev5.latch = false;
        m_elev5.prev_gim = false;
        m_elev5.y.reset();

        return false;
    }

    // gimmick_int aufloesen: passiert sonst erst im is_pc-Branch WEIT unten.
    resolve_state_vals();

    const double now = clock_now();
    const auto cam = read_live_cam_state();
    const bool gim = m_gimmick_int.has_value() && cam.has_value() && *cam == *m_gimmick_int;

    const auto at_endpoint = [](float y) {
        return std::fabs(y - ELEV5_Y_UNTEN) <= ELEV5_Y_TOL
            || std::fabs(y - ELEV5_Y_OBEN) <= ELEV5_Y_TOL;
    };

    if (m_elev5.prev_gim && !gim && at_endpoint(p->y)) {
        // Gimmick vorbei + wir stehen am Endpunkt -> Fahrt beginnt
        m_elev5.latch = true;
        m_elev5.y = p->y;
        m_elev5.t = now;
        m_elev5.still_since = now;
        m_elev5.moved_once = false;
        // [FIX 2026-07-15] ZIEL = der ANDERE Endpunkt. Vorher wurde gegen BEIDE
        // geprueft -> beim Losfahren steht man ja noch am Start, also war "Ziel
        // erreicht" ab dem ersten Frame wahr -> Latch fiel sofort = Flackern.
        m_elev5.target = (std::fabs(p->y - ELEV5_Y_UNTEN) <= ELEV5_Y_TOL) ? ELEV5_Y_OBEN
                                                                          : ELEV5_Y_UNTEN;
    }

    m_elev5.prev_gim = gim;

    if (m_elev5.latch) {
        if ((now - m_elev5.t) >= 0.15) {
            if (std::fabs(p->y - m_elev5.y.value_or(p->y)) > 0.05f) {
                m_elev5.still_since = now;
                m_elev5.moved_once = true;   // ab jetzt darf das Stillstands-Netz greifen
            }

            m_elev5.y = p->y;
            m_elev5.t = now;
        }

        // Ziel erreicht -> fertig.
        if (m_elev5.target.has_value() && std::fabs(p->y - *m_elev5.target) <= ELEV5_Y_TOL) {
            m_elev5.latch = false;
        }

        // Sicherheitsnetz gegen ein ewig haengendes Latch. ERST scharf, wenn die
        // Kabine sich mindestens einmal bewegt hat: zwischen Knopfdruck und
        // Anfahren steht y still -- ohne diesen Guard feuerte das Netz WAEHREND
        // des Anlaufs und riss den Latch weg.
        if (m_elev5.moved_once && (now - m_elev5.still_since) > 1.5) {
            m_elev5.latch = false;
        }
    }

    return m_elev5.latch;
}

// ============================================================================
// evaluate_core -- EIN zentraler Pass pro Frame auf UpdateScene.
//
// Die REIHENFOLGE der Bloecke ist tragend: fast jeder steht dort, wo er steht,
// weil ein spaeterer ihn sonst mitfinge oder ein frueherer ihn ueberschriebe.
// Die Begruendungen stehen jeweils am Block.
//
// Die Info-Flags werden hier in Member geschrieben und erst am Ende von
// evaluate() gesammelt nach Lua veroeffentlicht (publish_globals). Das ist
// verhaltensgleich -- innerhalb eines Durchlaufs liest sie niemand -- spart
// aber rund 40 Lua-Zugriffe pro Frame.
// ============================================================================

void RE4VRKillswitch::evaluate_core() {
    m_pin_release_active = false;   // default; nur im is_pc-Branch ggf. gesetzt
    m_ks2_active = false;
    m_ks3_active = false;
    m_ks4_active = false;
    m_ks5_active = false;
    m_boxbreak_active = false;

    m_p_leaning_ladder_active = false;
    m_p_minecart_ks4_active = false;
    m_p_minecart2_ks4_active = false;
    m_p_grappled_active = false;
    m_p_gondola_active = false;
    m_p_gondola_ada_active = false;
    m_p_railcar_mode = false;
    m_p_jetski_active = false;
    m_p_boat_active = false;
    m_p_forcecrouch_ks4_active = false;
    m_p_in_squeeze = false;
    m_p_ks_keep_movement = false;
    // __re4_throwsight_active und __re4_evt60874_fullhide stehen BEWUSST nicht
    // hier: die Lua setzt sie nur in einzelnen Zweigen, sie halten also ueber
    // Frames hinweg ihren Wert.

    // 1. Force (UI/Debug)
    if (m_force_killswitch) {
        m_killswitch_active = true;
        m_activating_reason = "force";

        return;
    }

    // 1b. [SCOPE_KILLSWITCH -- AUSGEBAUT 2026-08-28] Hier stand der Force-Branch,
    // der beim Zielen durch ein montiertes Scope ALLES abschaltete (reason
    // "viascope"). Genau der war der Grund, warum die Waffe im Zoom nicht mehr
    // an der Hand hing. Das Scope laeuft jetzt ueber RE4VRScope.

    // 1c. [BOLT_CYCLE] Externes Force-Flag (von re4_vr_weapons gesetzt): wp4400
    // im Iron-Sight, waehrend die native Bolt-Cycle-Anim laeuft. KS4:
    // First-Person BLEIBT, Head/Hair + alle Scripte aus.
    if (re4vr::lua_get_tribool("__re4_force_killswitch_bolt") == 1) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_activating_reason = "boltcycle";

        return;
    }

    // 1c1. [BULLETRUSH] Mercenaries-Ragemodus (Flag von RE4VRMerc). Waehrend der
    // Rage pruegelt der Body nativ, der CamState bleibt aber Gameplay -> ohne
    // diesen Zweig stuenden die VR-Haende mitten in der Anim herum.
    // KAMPAGNE UNBERUEHRT: das Flag setzt ausschliesslich das Mercs-Modul.
    if (re4vr::lua_get_tribool("__re4_force_ks4_bulletrush") == 1) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_activating_reason = "ks4_bulletrush";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1c2. [ELEVATOR_TROUBLE] MUSS VOR der Aufzug-GAMEPLAY-Gate stehen (die
    // pinnt sonst GAMEPLAY).
    if (is_elevator_trouble()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks4_elevatortrouble";

        return;
    }

    // 1d. [ELEVATOR] Reverse-Killswitch: in der Aufzug-Box GAMEPLAY pinnen
    // (killswitch AUS). VOR der Cutscene-/KS-Erkennung, damit ein Flackern den
    // killswitch nicht kurz aktiviert.
    if (is_in_elevator()) {
        m_killswitch_active = false;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "gameplay_elevator";
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;

        return;
    }

    // 1e0. [ELEVATOR5] Kabine steht -> normales Gameplay. Fahrt (Latch) ->
    // GAMEPLAY forcieren.
    if (is_in_elevator5_cabin()) {
        if (elevator5_update() && !is_real_cutscene().first) {
            m_killswitch_active = false;
            m_pin_release_active = false;
            m_p_throwsight_active = false;
            m_activating_reason = "gameplay_elevator5";
            m_fp_latch = false;
            m_fp_latch_state.reset();
            m_fp_latch_level = 0;

            return;
        }
    } else {
        // Kabine verlassen -> Latch faellt. Ohne Reset misst der naechste
        // Eintritt gegen ein uraltes y bzw. der Latch haengt.
        m_elev5.latch = false;
        m_elev5.prev_gim = false;
        m_elev5.y.reset();
        m_elev5.target.reset();
        m_elev5.moved_once = false;
    }

    // 1e. [ELEVATOR2/3] Solange der Aufzug uns parentet bzw. wir in der Box
    // stehen -> GAMEPLAY forcieren.
    if ((is_parented_to_elevator() || is_in_elevator2_zone() || is_in_elevator3_zone())
        && !is_real_cutscene().first) {
        // [ELEVATOR2_ZONE_VORTRITT] Ist fuer Pos+CamState eine KS-Zone
        // gespeichert (z.B. der Button-Druck IM Aufzug), dieser den Vortritt
        // lassen: NICHT Gameplay forcen, sondern zur normalen Erkennung
        // durchfallen.
        const auto ep = get_player_pos();
        const auto [estg, espc] = read_stage_space();
        // [ELEV_LIVE_CAM] LIVE lesen, nicht m_current_cam_state.
        const auto ecam = read_live_cam_state();

        std::optional<int32_t> zlvl{};

        if (ep.has_value()) {
            Entry e{};
            e.stage = estg.value_or(0);
            e.space = espc.value_or(0);
            e.camstate = ecam;
            e.has_pos = true;
            e.pos = *ep;
            zlvl = zone_level_for(e);
        }

        if (!zlvl.has_value()) {
            // [ELEVATOR2_ZONE_CAPTURE] Event-Eintritt mit-erfassen, damit
            // KS2-Zonen (Button) auch IM Aufzug speicherbar sind -- der Zweig,
            // der cur_episode_entry sonst setzt, liegt hinter diesem return.
            if (ep.has_value()) {
                Entry e{};
                e.stage = estg.value_or(0);
                e.space = espc.value_or(0);
                e.camstate = ecam;
                e.has_pos = true;
                e.pos = *ep;
                m_cur_episode_entry = e;
            }

            m_killswitch_active = false;
            m_pin_release_active = false;
            m_p_throwsight_active = false;
            m_activating_reason = "gameplay_elevator2";
            m_fp_latch = false;
            m_fp_latch_state.reset();
            m_fp_latch_level = 0;

            return;
        }

        // sonst: Zone matcht -> durchfallen, normale Erkennung greift sie
    }

    // 1e2. [GONDEL] Leons Gondel (GimmickType GondolaL, Stage 56100) -> KS4.
    // Ohne Eingriff faellt die Fahrt ueber CamState "Gimmick"(15) auf KS1.
    // Warum KS4 und nicht Gameplay-Force: in der Gondel wird nur gefahren;
    // KS4 hat zudem den Vorteil, dass motion/movement gar nicht laufen -> es
    // arbeitet NICHTS gegen das ParentGimmick.
    // [ZURUECK AUF KS4 2026-08-28, Ansage des Users] Die ks5_active-Zeile ist
    // raus: KS4 + Head-Nagel, Mesh bleibt sichtbar. Das Stilllegen macht ab
    // jetzt die STILLZONE in motion -- sie zuendet ueber __re4_gondola_active.
    if (is_on_gondola() && !is_real_cutscene().first) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_p_gondola_active = true;
        m_activating_reason = "ks5_gondola";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // [TOTAL-KILLSWITCH PER ORT -- STILLGELEGT 2026-08-27] Der Fall ist geloest:
    // es war ausschliesslich re4_vr_motion.lua. Der Block bleibt als
    // dokumentierter Notnagel stehen; __re4_ks_ortzonen_an holt ihn zurueck.
    // Kein Setzer im ganzen Baum -> der Zweig ist tot, wandert aber 1:1 mit.
    if (re4vr::lua_get_tribool("__re4_ks_ortzonen_an") == 1 && !is_real_cutscene().first) {
        const auto [zstage, zspace] = read_stage_space();

        if (zstage.has_value() && *zstage == 60850) {
            if (const auto zpos = get_player_pos(); zpos.has_value()) {
                const float zdx = zpos->x - 87.51f;
                const float zdy = zpos->y - 20.95f;
                const float zdz = zpos->z - 12.99f;

                if ((zdx * zdx + zdy * zdy + zdz * zdz) <= (2.5f * 2.5f)) {
                    // [KS1 STATT KS4] KS4 war NICHT restlos: der User blieb in
                    // First-Person, also lief die Kamera-Schicht weiter. Hier
                    // wird deshalb KEINE Stufe gesetzt: killswitch_active ohne
                    // ks2/3/4/5 ist KS1 = voll aus, native 3rd-Person.
                    m_killswitch_active = true;
                    m_pin_release_active = false;
                    m_p_throwsight_active = false;
                    m_activating_reason = "ks1_ortzone_gondel";

                    return;
                }
            }
        }
    }

    // [GONDEL-PARENT / ADAS GONDEL -- WIEDER STILLGELEGT 2026-08-28] Zurueck auf
    // den Stand, der lief: Adas Gondel laeuft als normales Gameplay bzw. mit dem
    // nativen KS1 des Spiels, das Stilllegen macht die STILLZONE.
    // Zurueckholen: __re4_gondola_parent_ks4 = true. Ebenfalls ohne Setzer.
    if (re4vr::lua_get_tribool("__re4_gondola_parent_ks4") == 1) {
        const auto [pstage, pspace] = read_stage_space();

        if (pstage.has_value() && GONDOLA_PARENT_STAGES.count(*pstage) > 0
            && has_parent_gimmick() && !is_real_cutscene().first) {
            auto* busy = get_busy_controller();
            // ohne Busy-Controller (= normales Gameplay) gilt die Player-Kamera
            const bool is_pc_cam = busy == nullptr ? true : is_a(busy, g_player_cam_td);

            if (is_pc_cam) {
                m_killswitch_active = true;
                m_ks4_active = true;
                m_pin_release_active = false;
                m_p_throwsight_active = false;
                m_activating_reason = "ks4_gondola_parent";
                m_p_gondola_ada_active = true;   // [STILLZONE] Zuender fuer motion
                m_fp_latch = true;
                m_fp_latch_state.reset();
                m_fp_latch_level = 4;

                return;
            }
        }
    }

    // [BEARTRAP] Erkennung ueber den GimmickType-NAMEN -> gilt fuer JEDE
    // Beartrap im Spiel. KS4 (war KS2): alle Scripte aus + resetBasePose beim
    // Eintritt; der Head-Nagel bleibt.
    if (is_in_legholdtrap() && !is_real_cutscene().first) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks4_beartrap";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // [ELEVATOR_59100] Von KS4 auf GAMEPLAY umgestellt: als KS4 wuerde ihn der
    // KS4-Yaw-Lock (binding) erfassen -> rechter Stick tot.
    if (is_riding_elevator_59100() && !is_real_cutscene().first && !is_gimmick_motion_now()
        && !is_demo_priority_now()) {
        m_killswitch_active = false;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "gameplay_elevator_59100";
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;

        return;
    }

    // [JETSKI] Fahr-Stages (592xx) -> KS4 wie Minecart/Gondel. KEIN
    // keep_movement (die VehicleCam gibt Position/Kamera komplett vor).
    if (is_on_jetski() && !is_real_cutscene().first && !is_gimmick_motion_now()
        && !is_demo_priority_now()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_p_jetski_active = true;
        m_activating_reason = "ks4_jetski";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // [BOAT_KS4] Boot-Fahrt -> KS4 wie Jetski. throwsight (Fisch-Boss) ist
    // EXPLIZIT ausgenommen. Steht NACH dem Jetski-Branch -> Jetski behaelt
    // Vorrang (auch ein "Boot").
    if (is_on_boat() && !is_throwsight_stage() && !is_real_cutscene().first
        && !is_gimmick_motion_now() && !is_demo_priority_now()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_p_boat_active = true;
        m_activating_reason = "ks4_boat";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // [FORCECROUCH_KS4 40501 -> 40502] ZEIT-HYSTERESE gegen Flackern: der
    // CamState springt im Kriech-Gang zwischen ForceCrouch und Crouch, und der
    // Stagewechsel hat kurze Nicht-ForceCrouch-Frames. Jeder ForceCrouch-Frame
    // schiebt fc_ks4_until ~1s vor -- das ueberbrueckt die Zwischenframes UND
    // traegt den KS weich in Stage 40502 hinein.
    {
        const double now = clock_now();
        const auto [fc_stg, fc_spc] = read_stage_space();

        if (fc_stg.has_value() && *fc_stg == 40501 && fc_spc.has_value() && *fc_spc == 40500
            && m_forcecrouch_int.has_value() && read_live_cam_state() == m_forcecrouch_int) {
            m_fc_ks4_until = now + 1.0;
        }

        if (now < m_fc_ks4_until && !is_real_cutscene().first) {
            m_killswitch_active = true;
            m_ks4_active = true;
            m_pin_release_active = false;
            m_p_throwsight_active = false;
            // Gameplay-ForceCrouch-Head-Nagel aus: KS4 nagelt selbst
            m_p_forcecrouch_active = false;
            m_p_forcecrouch_ks4_active = true;   // eigener HMD-Offset im firstperson
            m_activating_reason = "ks4_forcecrouch";
            m_fp_latch = true;
            m_fp_latch_state.reset();
            m_fp_latch_level = 4;

            return;
        }
    }

    // [GANG_3RD 60874] Der lange gerade Gang soll KOMPLETT in 3rd-Person laufen.
    // Gegatet wird der Abstand zur VERBINDUNGSSTRECKE (Punkt-Segment), Radius
    // 3 m -- also ein Schlauch entlang des Gangs statt zweier Kugeln.
    // Steht VOR dem EVT60874-Block -> im Schlauch gewinnt 3rd-Person auch
    // waehrend der Quicktime-Events.
    {
        bool in_gang = false;
        const auto [gstage, gspace] = read_stage_space();

        if (gstage.has_value() && *gstage == 60874) {
            if (const auto gp = get_player_pos(); gp.has_value()) {
                constexpr float ax = 30.80f, ay = 1.32f, az = 232.38f;
                const float abx = 0.46f - ax, aby = 2.66f - ay, abz = 232.14f - az;
                const float apx = gp->x - ax, apy = gp->y - ay, apz = gp->z - az;
                const float ab2 = abx * abx + aby * aby + abz * abz;
                float t = 0.0f;

                if (ab2 > 0.0f) {
                    t = (apx * abx + apy * aby + apz * abz) / ab2;
                }

                t = std::clamp(t, 0.0f, 1.0f);

                const float dx = apx - abx * t;
                const float dy = apy - aby * t;
                const float dz = apz - abz * t;
                in_gang = (dx * dx + dy * dy + dz * dz) <= 9.0f;
            }
        }

        if (!in_gang) {
            m_gang3rd_t.reset();
        } else {
            if (!m_gang3rd_t.has_value()) {
                m_gang3rd_t = clock_now();
            }

            // [JOINT_RELEASE] Eine EIGENE Flanke pro Eintritt, 0.5 s lang
            // nachgehalten: kommt man aus dem EVT60874-KS4 in den Schlauch, war
            // full_off schon true -> keine Flanke -> die gepinnten Joints
            // blieben stehen (hier sichtbar, weil 3rd-Person laeuft).
            if ((clock_now() - *m_gang3rd_t) < 0.5) {
                if (auto* gtf = re4vr::fc::body_tf(); gtf != nullptr) {
                    re4vr::call_safe<void*>(gtf, "resetBasePose");
                }
            }

            m_killswitch_active = true;   // KS1: keine Stufe gesetzt = voll aus
            m_pin_release_active = false;
            // [KEIN keep_movement] __re4_ks_keep_movement wuerde in movement die
            // KS-Pause abschalten -> Spine-/Hip-/Neck-Pins liefen weiter und
            // verboegen die in 3rd-Person sichtbare Pose.
            m_p_evt60874_fullhide = false;
            m_evt60874_t0.reset();
            m_activating_reason = "gang3rd_60874";

            return;
        }
    }

    // [EVT60874 MID-EVENT-FP] Startet bewusst in 3rd-Person (KS1) und schaltet
    // erst nach EVT60874_DELAY auf First-Person (KS4) um -- ab da zusaetzlich
    // ALLE Meshes aus.
    // [KLEMM-FIX] Die Stage 60874 bleibt nach dem Event bestehen -> ein Gate
    // allein auf die Stage hielte KS4 fuer immer. Deshalb zusaetzlich an die
    // EVENT-Signatur gekoppelt (Occupied CH_JACKED_GMK_HIGH).
    {
        const auto [ev_stg, ev_spc] = read_stage_space();
        bool ev_on = false;

        if (ev_stg.has_value() && *ev_stg == 60874) {
            if (const auto want = squeeze_high_prio(); want.has_value()) {
                const auto eprio = occupied_priority();
                ev_on = eprio.has_value() && *eprio == *want;
            }
        }

        if (ev_on) {
            if (!m_evt60874_t0.has_value()) {
                m_evt60874_t0 = clock_now();
            }

            if ((clock_now() - *m_evt60874_t0) >= EVT60874_DELAY && !is_real_cutscene().first) {
                m_killswitch_active = true;
                m_ks4_active = true;
                m_pin_release_active = false;
                // [FULLHIDE] Ab hier ganzes Mesh aus (materials liest das Flag).
                m_p_evt60874_fullhide = true;
                m_activating_reason = "ks4_evt60874";
                m_fp_latch = true;
                m_fp_latch_state.reset();
                m_fp_latch_level = 4;

                return;
            }

            m_p_evt60874_fullhide = false;   // Uhr laeuft noch -> 3rd-Person
        } else {
            m_evt60874_t0.reset();
            m_p_evt60874_fullhide = false;
        }
    }

    // [ASHLEY-CARRY] Trage-Abschnitt -> KS4, AUSSER in der Zwischendemo.
    {
        const auto [carry_stg, carry_spc] = read_stage_space();

        if (carry_stg.has_value()
            && (*carry_stg == 68102 || *carry_stg == 68103 || *carry_stg == 68105)
            && is_carrying() && !is_real_cutscene().first && !is_gimmick_motion_now()
            && !is_demo_priority_now()) {
            m_killswitch_active = true;
            m_ks4_active = true;
            m_pin_release_active = false;
            m_p_throwsight_active = false;
            // [TEST] movement trotz KS4 weiterlaufen lassen (Turn-Kopplung)
            m_p_ks_keep_movement = true;
            m_activating_reason = "ks4_ashley_carry";
            m_fp_latch = true;
            m_fp_latch_state.reset();
            m_fp_latch_level = 4;

            return;
        }
    }

    // [GFIX_KS4] Stage 44400 + GimmickFix + CH_JACKED_GMK_LOW -> stabil KS4.
    if (is_gimmickfix_ks4_spot()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks4_gfix";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // [GFIX_KS5] MUSS VOR dem Squeeze-Block stehen: der wuerde dasselbe Event
    // ueber die Priority mitfangen und es waere nur KS4 = Koerper ohne Kopf.
    // ks4_active wird MITgesetzt, sonst faellt der Head-Nagel weg (haengt an
    // is_ks4).
    if (is_gimmickfix_ks5_spot()) {
        m_killswitch_active = true;
        m_ks4_active = true;

        if (m_fp_enabled) {
            m_ks5_active = true;
        }

        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks5_gfix";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1f. [GIMMICK_KS3_SPOT] Durchquetschen -> KS4 (war KS2): in KS2 liefen
    // movement/motion mit und un-parenteten den Spieler jeden Frame vom
    // Quetsch-Gimmick -> am ENDE Desync -> Rueckglitch an den Anfang.
    // Der Reason bleibt "ks3_gimmick": firstperson erkennt das Event genau an
    // diesem String -- umbenennen wuerde den Head-Nagel abschalten.
    if (is_gimmick_ks3_spot()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_p_in_squeeze = true;   // [ANTIBEAM] fuer die Rueckwaerts-Sprung-Korrektur
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks3_gimmick";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1f2. [MINIDEMO_KS5] KS5 = wie KS4, ABER GESAMTES Mesh aus.
    // ks4_active MUSS mitgesetzt werden: der Head-Nagel in firstperson und
    // binding fragen is_ks4; bei reinem ks5_active melden beide false und die
    // Kamera haengt nicht mehr am Head-Joint.
    // [3RD-PERSON-TOGGLE] Toggle aus -> nur ks4_active, sonst waere Leon in
    // 3rd-Person unsichtbar.
    if (is_minidemo_ks4_spot()) {
        m_killswitch_active = true;
        m_ks4_active = true;

        if (m_fp_enabled) {
            m_ks5_active = true;
        }

        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_activating_reason = "ks5_minidemo";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1g. [MINECART_KS4 55201/55202] Getrennt geflaggt (minecart vs minecart2),
    // damit firstperson pro Szene einen eigenen HMD-Offset legen kann.
    if (const char* mc_kind = minecart_ks4_kind(); mc_kind != nullptr) {
        m_killswitch_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;

        if (std::strcmp(mc_kind, "cart2") == 0) {
            m_ks4_active = true;
            m_p_minecart2_ks4_active = true;
            m_activating_reason = "minecart2_ks4";
        } else {
            m_ks4_active = true;
            m_p_minecart_ks4_active = true;   // [MINECART_OFFSET] ActionCam-Einstieg
            m_activating_reason = "minecart_ks4";
        }

        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1h. [RAILCAR_MODE] NACH dem Minecart-Intro-Gate: das Intro hat kein
    // ParentGimmick, greift hier also eh nicht mit.
    if (is_on_railcar()) {
        m_killswitch_active = true;
        m_ks4_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        // [RAILCAR] motion + arm_chain laufen TROTZ killswitch weiter
        m_p_railcar_mode = true;
        m_activating_reason = "railcar_mode";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 4;

        return;
    }

    // 1i. [GRAPPLED] VOR der Controller-/Cutscene-Klassifizierung: Phase 1 des
    // Griffs laeuft auf ActionCameraController und wuerde sonst unten als KS1
    // landen. NACH dem Minecart-Gate (das nutzt ebenfalls ActionCam).
    if (player_is_grappled()) {
        m_killswitch_active = true;
        m_pin_release_active = false;
        m_p_throwsight_active = false;
        m_p_grappled_active = true;

        const auto [stg, spc] = read_stage_space();

        // [GRAPPLE_KS3_65100 -> KS4 2026-07-20] War KS3; jetzt KS4, damit KS3
        // ausschliesslich der Damage-Zustand ist.
        if (stg.has_value() && *stg == 65100) {
            m_ks4_active = true;
            m_activating_reason = "ks4_grappled";
            m_fp_latch = true;
            m_fp_latch_state.reset();
            m_fp_latch_level = 4;
        } else {
            m_ks2_active = true;
            m_activating_reason = "ks2_grappled";
            m_fp_latch = true;
            m_fp_latch_state.reset();
            m_fp_latch_level = 2;
        }

        return;
    }

    // [EVT40510 TIMED-KS4] Die ersten EVT40510_DELAY Sekunden KS4, danach faellt
    // der Block durch -> der Cutscene-Zweig darunter macht daraus wieder KS1.
    // [POSITIONS-GATE] Entschieden wird EINMAL beim Eintritt; waehrend der
    // Cutscene wird die Position NICHT mehr geprueft (der Char bewegt sich
    // darin, sonst risse KS4 mittendrin ab).
    {
        const auto [e_stg, e_spc] = read_stage_space();

        if (e_stg.has_value() && *e_stg == 40510 && is_real_cutscene().first) {
            if (!m_evt40510_t.has_value()) {
                bool hit = false;

                if (const auto ep = get_player_pos(); ep.has_value()) {
                    const float dx = ep->x - (-183.43f);
                    const float dy = ep->y - 6.53f;
                    const float dz = ep->z - 85.46f;
                    hit = (dx * dx + dy * dy + dz * dz) <= 9.0f;
                }

                m_evt40510_t = hit ? clock_now() : -1.0;
            }

            if (*m_evt40510_t >= 0.0 && (clock_now() - *m_evt40510_t) < EVT40510_DELAY) {
                m_killswitch_active = true;
                m_ks4_active = true;
                m_pin_release_active = false;
                m_activating_reason = "ks4_evt40510";
                m_fp_latch = true;
                m_fp_latch_state.reset();
                m_fp_latch_level = 4;

                return;
            }

            // Uhr abgelaufen: NICHT zurueckstellen (sonst flackert es zwischen
            // KS4 und KS1) -- einfach durchfallen lassen.
        } else {
            m_evt40510_t.reset();
        }
    }

    // 2. Echte Cutscene
    if (const auto [cut, why] = is_real_cutscene(); cut) {
        m_killswitch_active = true;
        m_activating_reason = why;
        m_p_throwsight_active = false;

        return;
    }

    // 2b. [THROWSIGHT] Del-Lago-Harpunen-Abschnitt: KS2 -> Head/Hair aus, Body
    // sichtbar, dafuer greift ueber ks2_now der Head-Nagel.
    if (is_throwsight_stage()) {
        m_p_throwsight_active = true;
        m_killswitch_active = true;
        m_ks2_active = true;
        m_pin_release_active = false;
        m_activating_reason = "throwsight_ks2";
        m_fp_latch = true;
        m_fp_latch_state.reset();
        m_fp_latch_level = 2;

        return;
    }

    m_p_throwsight_active = false;

    // 3. BusyCameraController-Typ: nur PlayerCameraController = Gameplay
    auto* busy = get_busy_controller();
    const std::string ctrl_name = busy != nullptr ? full_name_of(busy) : std::string{};

    if (ctrl_name != m_current_controller) {
        m_previous_controller = m_current_controller;
        m_current_controller = ctrl_name;
    }

    const bool is_pc = busy != nullptr && is_a(busy, g_player_cam_td);

    if (is_pc) {
        evaluate_player_camera(busy);
    } else {
        m_current_cam_state.reset();
        m_killswitch_active = true;
        m_activating_reason = ctrl_name.empty() ? std::string{"no_controller"} : ctrl_name;
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;

        // [HOOK-ZWISCHENKAMERA] Direkt nach einem Enterhaken schiebt sich fuer
        // ~1 s ein ActionCameraController dazwischen, und genau da sah man sich
        // in der 3rd-Person. Eng gegatet ueber m_hookshot_seen_t: greift NUR
        // nach einem Haken -- Leon hat keinen, ihn kann der Zweig nie treffen.
        if (m_hookshot_seen_t > 0.0
            && (clock_now() - m_hookshot_seen_t) < hookshot_ks4_sec()) {
            m_ks4_active = true;
            m_fp_latch = true;
            m_fp_latch_level = 4;
            m_activating_reason = "ks4_hook_actioncam";
        }
    }
}

// ============================================================================
// Der is_pc-Zweig: PlayerCameraController aktiv, 3 Stufen je nach CamState.
//   (a) Gameplay-State      -> voller VR-Zustand (alles an)
//   (b) Pin-Release-State   -> NUR Pin loesen, HMD/Yaw bleiben
//   (c) sonst               -> voller Killswitch, Stufe per Latch/Zone/Liste
// ============================================================================

void RE4VRKillswitch::evaluate_player_camera(::REManagedObject* busy) {
    auto st = read_cam_state(busy);
    m_current_cam_state = st;

    // [GIMMICK_FLAG] Interaktions-Flag steht (Auto-Attach an Tuer/Leiter/...) ->
    // diesen Frame NICHT als Gameplay werten, auch wenn der CamState noch ein
    // Gameplay-State ist (kippt ~0.2s spaeter).
    const bool gimmick_ng = player_gimmick_active() || player_is_coop_jacked();

    resolve_state_vals();

    // [DAMAGE_HOLD] Waehrend der Damage-Reaktion oszilliert die CamState pro
    // Frame zwischen Damage und Gameplay -> Killswitch flackert -> Arme zucken.
    // Ein Halte-Fenster behandelt oszillierende Gameplay-Frames als Damage.
    if (m_damage_int.has_value() && st.has_value() && *st == *m_damage_int) {
        m_damage_until = clock_now() + DAMAGE_HOLD;
    }

    if (m_damage_int.has_value() && clock_now() < m_damage_until && is_gameplay_camstate(st)) {
        st = m_damage_int;
        m_current_cam_state = st;
    }

    // [STAGGER_HEAL / DAMAGE-ENDE-STEMPEL] Die Reload-Module koennen die
    // fallende Flanke NICHT selbst erkennen: waehrend der Damage-Reaktion
    // meldet die Engine keine Waffe (wid=-1), ihre Frame-Funktion steigt vorher
    // aus. Deshalb hier -- der Killswitch laeuft immer -- den Zeitpunkt des
    // Damage-ENDES veroeffentlichen.
    {
        const bool dmg_now = m_damage_int.has_value() && clock_now() < m_damage_until;

        if (m_p_damage_active && !dmg_now) {
            m_p_damage_end_t = clock_now();
        }

        m_p_damage_active = dmg_now;
    }

    // [ECHTES RUNTERSPRINGEN] Ein BEWUSSTER Sprung-Einstieg macht das Fenster
    // scharf UND setzt jumpdown_confirmed; danach wird es ueber die GANZE
    // Flugphase gehalten -- auch waehrend "JumpLoop", das keinen Sprung-Token
    // matcht. Faktisch redundant, seit die ganze Kette in KS2_CAM_STATES steht;
    // bleibt drin, weil er eine Stufe frueher greift und oszillierende Frames
    // abfaengt.
    const double now = clock_now();

    if (player_node_has("jumpdown") || player_node_has("jumpoff")
        || player_node_has("jump_large") || player_node_has("_jump_")) {
        m_jumpdown_confirmed = true;
        m_jumpdown_until = now + JUMPDOWN_HOLD;
    }

    if (m_jumpdown_confirmed) {
        if (is_airborne_camstate(st)) {
            m_jumpdown_until = now + JUMPDOWN_HOLD;   // Flug/Landung -> nachfuellen
        } else if (is_gameplay_camstate(st)) {
            m_jumpdown_confirmed = false;             // gelandet -> fertig
        }
    }

    if (player_is_boxbreak()) {
        // [KICK/BARREL] Fass-/Kisten-Tritt -> ueber das FLAG geforct (der
        // CamState bleibt Gameplay, greift also nicht ueber die Listen).
        // KS2 (war KS4): Scripte bleiben AN. Der HMD-Offset haengt an
        // __re4_boxbreak_active, NICHT am KS-Grad.
        // WICHTIG: dieser eine KS2 ist vom KS2_ALS_KS4-Schalter AUSGENOMMEN.
        m_killswitch_active = true;
        m_ks2_active = true;
        m_boxbreak_active = true;
        m_pin_release_active = false;
        m_activating_reason = "ks2_boxbreak:" + state_str(st);
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 2;
    } else if (player_is_ladder_exit(is_gameplay_camstate(st))) {
        // [LEITER-AUSSTIEG] VOR dem Leiter-Branch, sonst faengt player_is_ladder
        // den Ausstieg ab.
        m_killswitch_active = true;
        m_ks2_active = true;
        m_pin_release_active = false;
        m_activating_reason = "ks2_ladder_exit:" + state_str(st);
        m_fp_latch = true;
        m_fp_latch_state = st;
        m_fp_latch_level = 2;
    } else if (player_is_ladder()) {
        // [LEITER] KS2: Scripte bleiben AN. Zweck: der Yaw-Stick soll tot sein
        // (KS2_STICK_LOCK greift bei is_ks2).
        m_killswitch_active = true;
        m_ks2_active = true;
        m_pin_release_active = false;
        m_activating_reason = "ks2_ladder:" + state_str(st);
        m_fp_latch = true;
        m_fp_latch_state = st;
        m_fp_latch_level = 2;
        // [LEANING_LADDER] NUR beim Klettern + wenn eine schraege Leiter
        // bestiegen wurde (tryUse-Latch).
        m_p_leaning_ladder_active = m_leaning_ladder_mounted;
    } else if (now < m_jumpdown_until) {
        m_killswitch_active = true;
        m_ks2_active = true;
        m_pin_release_active = false;
        m_activating_reason = "jumpdown:" + state_str(st);
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;
    } else if (is_gameplay_camstate(st) && !gimmick_ng) {
        m_killswitch_active = false;
        m_activating_reason.clear();
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;
        m_leaning_ladder_mounted = false;   // off ladder -> Latch loeschen

        // Event vorbei -> Eintritts-Fingerprint als "letztes Event" sichern
        // (Monitor-Button laesst sich so auch NACH dem Event noch druecken).
        if (m_cur_episode_entry.has_value()) {
            m_last_episode_entry = m_cur_episode_entry;
            m_cur_episode_entry.reset();
        }
    } else if (is_pinrelease_camstate(st) && !gimmick_ng) {
        m_killswitch_active = false;     // HMD-Yaw + Hard-Yaw bleiben aktiv
        m_pin_release_active = true;     // nur Spine/Crouch-Pin loesen
        m_activating_reason = "pinrelease:" + state_str(st);
        m_fp_latch = false;
        m_fp_latch_state.reset();
        m_fp_latch_level = 0;
    } else {
        // [FP_LATCH] Nicht-Gameplay-State. Neue Episode (State-Wechsel) ->
        // Latch reset + Event-Eintritts-Fingerprint EINFRIEREN.
        if (st != m_fp_latch_state) {
            m_fp_latch = false;
            m_fp_latch_state = st;
            m_fp_latch_level = 0;

            const auto p = get_player_pos();
            const auto [stg, spc] = read_stage_space();

            Entry e{};
            e.stage = stg.value_or(0);
            e.space = spc.value_or(0);
            e.camstate = st;
            e.has_pos = p.has_value();

            if (p.has_value()) {
                e.pos = *p;
            }

            m_cur_episode_entry = e;
        }

        if (!m_fp_latch) {
            // [ZONES] map+position zuerst (Kugel um die Event-Mitte);
            // KS3 vor KS2. Danach als Fallback die CamState-Listen.
            const auto zlvl = zone_level_for(m_cur_episode_entry);

            if (zlvl == 3) {
                m_fp_latch = true;
                m_fp_latch_level = 3;
            } else if (zlvl == 4) {
                // [KS4-ZONE] Ohne diesen Zweig fiele eine per Button gesetzte
                // KS4-Zone STUMM durch -> Zone gespeichert, aber wirkungslos.
                m_fp_latch = true;
                m_fp_latch_level = 4;
            } else if (zlvl == 2) {
                m_fp_latch = true;
                m_fp_latch_level = 2;
            } else if (is_ks3_camstate(st)) {
                m_fp_latch = true;
                m_fp_latch_level = 3;
            } else if (is_ks4_camstate(st)) {
                m_fp_latch = true;
                m_fp_latch_level = 4;
            } else if (is_ks2_camstate(st)) {
                m_fp_latch = true;
                m_fp_latch_level = 2;
            }
        }

        // [HOOKSHOT->GIMMICK KS4] Der "Gimmick"-CamState direkt nach dem
        // Enterhaken laeuft sonst als KS1 = volle 3rd-Person.
        if (m_hookshot_int.has_value() && st.has_value() && *st == *m_hookshot_int) {
            m_hookshot_seen_t = clock_now();
            m_p_hookshot_recent_until = clock_now() + hookshot_grace_sec();
        }

        if (m_gimmick_int.has_value() && st.has_value() && *st == *m_gimmick_int
            && m_hookshot_seen_t > 0.0
            && (clock_now() - m_hookshot_seen_t) < hookshot_ks4_sec()) {
            m_fp_latch = true;
            m_fp_latch_level = 4;
        }

        // [DAMAGE_KS3, so gewollt] JEDE Damage-Reaktion auf KS3 (ganzes Mesh
        // aus) -- egal ob sie vorher auf KS2 ODER KS4 aufgeloest hat. Grund wie
        // der Grapple-Override: der Body schwenkt trotz Head-Nagel ins Bild.
        if (m_fp_latch && m_damage_int.has_value() && st.has_value() && *st == *m_damage_int
            && (m_fp_latch_level == 2 || m_fp_latch_level == 4)) {
            m_fp_latch_level = 3;
        }

        m_killswitch_active = true;

        if (m_fp_latch && m_fp_latch_level == 3) {
            m_ks3_active = true;
            m_activating_reason = "ks3:" + state_str(st);
        } else if (m_fp_latch && m_fp_latch_level == 4) {
            m_ks4_active = true;
            m_activating_reason = "ks4:" + state_str(st);
        } else if (m_fp_latch && m_fp_latch_level == 2) {
            m_ks2_active = true;
            m_activating_reason = "ks2:" + state_str(st);
        } else {
            m_activating_reason = "camstate:" + state_str(st);   // KS1 (voll)
        }
    }
}

// ============================================================================
// evaluate -- Edge-Tracking rund um evaluate_core
// ============================================================================

// [PORTFIX 2026-09-06] Idempotent: die erste Runde, in der die TDB steht,
// gewinnt. Wird aus on_lua_state_created UND aus evaluate() gerufen.
void RE4VRKillswitch::resolve_type_defs() {
    if (m_type_defs_ok) {
        return;
    }

    g_player_cam_td = sdk::find_type_definition("chainsaw.PlayerCameraController");
    g_gimmick_motion_td = sdk::find_type_definition("chainsaw.GimmickMotionCameraController");
    g_gimmick_fix_td = sdk::find_type_definition("chainsaw.GimmickFixCameraController");
    g_action_camera_td = sdk::find_type_definition("chainsaw.ActionCameraController");
    g_vehicle_camera_td = sdk::find_type_definition("chainsaw.VehicleCameraController");
    g_mfsm2_td = sdk::find_type_definition("via.motion.MotionFsm2");
    g_motion_td = sdk::find_type_definition("via.motion.Motion");
    g_elev_td_59100 = sdk::find_type_definition("chainsaw.GmElevator");

    m_type_defs_ok = g_player_cam_td != nullptr;
}

void RE4VRKillswitch::evaluate() {
    resolve_type_defs();

    const bool prev = m_killswitch_active;

    evaluate_core();

    // [KS2_ALS_KS4, so gewollt] Jedes erkannte KS2 wird als KS4 behandelt: KS4
    // schaltet zusaetzlich die ungateten Scripte ab und gibt beim Eintritt das
    // Skelett per resetBasePose zurueck. Bewusst als SCHALTER statt Loeschung.
    // [BOXBREAK AUSGENOMMEN] Der Fass-/Kisten-Tritt soll BEWUSST KS2 bleiben.
    if (m_ks2_as_ks4 && m_ks2_active && !m_boxbreak_active) {
        m_ks2_active = false;
        m_ks4_active = true;
    }

    // [FP_GATE] Der Schalter greift NICHT hier: die Stufen bleiben vollstaendig
    // erhalten, damit KS4 weiterhin ALLE Scripte abschaltet. Umgebogen wird
    // allein die First-Person-DARSTELLUNG (is_ks2/3/4 melden dann false).

    // [KS4] Globales Flag fuer die ungateten Scripte. Toggle AUS -> in JEDEM
    // Killswitch alle ungateten Scripte abschalten, nicht nur in KS4/KS5: in
    // 3rd-Person sieht man den ganzen Koerper, dort muss alles ruhen.
    m_p_ks4_active_global = m_ks4_active || m_ks5_active || (!m_fp_enabled && m_killswitch_active);

    m_p_ks_active = m_killswitch_active;
    m_p_ks_reason = m_activating_reason;
    m_p_ks_level_is_4 = m_ks4_active;

    m_p_boxbreak_active = m_boxbreak_active;

    // [KS4-FATALKICK] NUR der echte Kick-CamState darf das setzen. Die LEITER
    // (und andere Flag-KS4) sind AUCH ks4_active -> ohne diesen CamState-Check
    // klaute Fatalkick dem Ladder-Offset den Offset-Zweig.
    m_p_fatalkick_active = m_ks4_active && !m_boxbreak_active
        && m_current_cam_state.has_value() && is_ks4_camstate(m_current_cam_state);

    // [FORCECROUCH_HEADPIN] Reines Info-Flag, KEIN Killswitch-Grad.
    m_p_forcecrouch_active = m_forcecrouch_int.has_value() && m_current_cam_state.has_value()
        && *m_current_cam_state == *m_forcecrouch_int;

    const auto [stg, spc] = read_stage_space();
    m_current_stage = stg;
    m_current_space = spc;

    // [EXIT_FADE] ALLE drei Flanken (KS4, KS3, KS2) setzen denselben Stempel ->
    // motion blendet die Haende auch nach Leiter/Sprung/Traversal/Damage weich
    // ein statt hart zu springen.
    if ((m_prev_ks4_exit && !m_ks4_active) || (m_prev_ks3_exit && !m_ks3_active)
        || (m_prev_ks2_exit && !m_ks2_active)) {
        m_p_ks4_exit_t = clock_now();
    }

    m_prev_ks4_exit = m_ks4_active;
    m_prev_ks3_exit = m_ks3_active;
    m_prev_ks2_exit = m_ks2_active;

    // Edge: Deaktivierung -> blend_back 1.0 -> 0.0 ueber ANIM_BLEND_DURATION.
    // (Liest niemand -- toter Export, per Grep belegt -- bleibt unangetastet,
    // damit nichts kippt, falls es doch mal jemand verdrahtet.)
    if (prev && !m_killswitch_active) {
        m_anim_blend_back = 1.0;
        m_anim_blend_back_t = clock_now();
    }

    if (m_anim_blend_back > 0.0) {
        const double el = clock_now() - m_anim_blend_back_t;

        if (el >= ANIM_BLEND_DURATION) {
            m_anim_blend_back = 0.0;
        } else {
            m_anim_blend_back = 1.0 - (el / ANIM_BLEND_DURATION);
        }
    }

    // [JOINT_RELEASE] Bei EINTRITT in einen "voll aus"-Killswitch (KS1/KS4/KS5)
    // das Skelett EINMAL an die Engine zurueckgeben: unsere Pins halten die
    // Pose fest, und im statischen Zustand treibt die Engine sie NICHT neu an
    // -> unser letzter Write klebt, auch nach Script-Aus.
    // NUR KS1/4/5: KS2/KS3 lassen die Joint-Setz-Scripte ABSICHTLICH laufen.
    {
        const bool ks1 = m_killswitch_active && !m_ks2_active && !m_ks3_active && !m_ks4_active
            && !m_ks5_active;
        bool full_off = ks1 || m_ks4_active || m_ks5_active;

        // [FP_GATE, "die Arme sind krumm"] Toggle AUS -> auch KS2/KS3 gelten als
        // voll-aus, sonst klebt die zuletzt geschriebene Pose an den Joints --
        // in First-Person unsichtbar, in 3rd-Person sofort zu sehen.
        if (!m_fp_enabled && m_killswitch_active) {
            full_off = true;
        }

        if (full_off && !m_prev_full_off) {
            if (auto* tf = re4vr::fc::body_tf(); tf != nullptr) {
                re4vr::call_safe<void*>(tf, "resetBasePose");
            }
        }

        m_prev_full_off = full_off;
    }

    m_was_active = prev;
}

// Alle Info-Flags gesammelt nach Lua. Solange auch nur eine Lua-Datei laeuft,
// muessen sie dort stehen -- binding, weapons und die reload* lesen sie.
void RE4VRKillswitch::publish_globals() {
    re4vr::lua_set_bool("__re4_ks_active", m_p_ks_active);
    re4vr::lua_set_bool("__re4_ks4_active", m_p_ks4_active_global);

    if (m_p_ks_reason.empty()) {
        re4vr::lua_set_nil("__re4_ks_reason");
    } else {
        re4vr::lua_set_string("__re4_ks_reason", m_p_ks_reason);
    }

    // Lua: `(ks4_active and 4) or nil` -- bei KS1/2/3 steht dort NICHTS.
    if (m_p_ks_level_is_4) {
        re4vr::lua_set_number("__re4_ks_level", 4.0);
    } else {
        re4vr::lua_set_nil("__re4_ks_level");
    }

    re4vr::lua_set_bool("__re4_boxbreak_active", m_p_boxbreak_active);
    re4vr::lua_set_bool("__re4_fatalkick_active", m_p_fatalkick_active);
    re4vr::lua_set_bool("__re4_forcecrouch_active", m_p_forcecrouch_active);
    re4vr::lua_set_bool("__re4_forcecrouch_ks4_active", m_p_forcecrouch_ks4_active);
    re4vr::lua_set_bool("__re4_leaning_ladder_active", m_p_leaning_ladder_active);
    re4vr::lua_set_bool("__re4_leaning_ladder_mounted", m_leaning_ladder_mounted);
    re4vr::lua_set_bool("__re4_minecart_ks4_active", m_p_minecart_ks4_active);
    re4vr::lua_set_bool("__re4_minecart2_ks4_active", m_p_minecart2_ks4_active);
    re4vr::lua_set_bool("__re4_grappled_active", m_p_grappled_active);
    re4vr::lua_set_bool("__re4_gondola_active", m_p_gondola_active);
    re4vr::lua_set_bool("__re4_gondola_ada_active", m_p_gondola_ada_active);
    re4vr::lua_set_bool("__re4_railcar_mode", m_p_railcar_mode);
    re4vr::lua_set_bool("__re4_jetski_active", m_p_jetski_active);
    re4vr::lua_set_bool("__re4_boat_active", m_p_boat_active);
    re4vr::lua_set_bool("__re4_in_squeeze", m_p_in_squeeze);
    re4vr::lua_set_bool("__re4_ks_keep_movement", m_p_ks_keep_movement);
    re4vr::lua_set_bool("__re4_throwsight_active", m_p_throwsight_active);
    re4vr::lua_set_bool("__re4_evt60874_fullhide", m_p_evt60874_fullhide);
    re4vr::lua_set_bool("__re4_damage_active", m_p_damage_active);

    if (m_p_damage_end_t.has_value()) {
        re4vr::lua_set_number("__re4_damage_end_t", *m_p_damage_end_t);
    }

    if (m_p_ks4_exit_t.has_value()) {
        re4vr::lua_set_number("__re4_ks4_exit_t", *m_p_ks4_exit_t);
    }

    if (m_p_hookshot_recent_until.has_value()) {
        re4vr::lua_set_number("__re4_hookshot_recent_until", *m_p_hookshot_recent_until);
    }

    // Die Latch-Stempel: in der Lua Globals, weil die Datei am 200er-Local-
    // Limit stand. Nativ Member; hier nur veroeffentlicht, damit Diagnose-
    // Werkzeuge sie weiterhin sehen.
    re4vr::lua_set_number("__re4_squeeze_latch_t", m_squeeze_latch_t);
    re4vr::lua_set_number("__re4_minidemo_latch_t", m_minidemo_latch_t);
    re4vr::lua_set_number("__re4_gfix_latch_t", m_gfix_latch_t);
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRKillswitch::on_initialize() {
    return Mod::on_initialize();
}

void RE4VRKillswitch::install_ladder_hook() {
    if (m_hook_installed) {
        return;
    }

    m_hook_installed = true;

    // [LEANING_LADDER] Hook auf GmLadderBase.tryUse (feuert beim Besteigen).
    // Ist die Leiter eine GmLeaningLadder (schraeg) -> Latch. Downstream NUR im
    // Leiter-Branch genutzt + im Gameplay-Branch zurueckgesetzt.
    auto* ladder_td = sdk::find_type_definition("chainsaw.GmLadderBase");
    auto* leaning_def = sdk::find_type_definition("chainsaw.GmLeaningLadder");

    if (ladder_td == nullptr || leaning_def == nullptr) {
        return;
    }

    auto* m = ladder_td->get_method("tryUse");

    if (m == nullptr) {
        return;
    }

    g_hookman.add(
        m,
        [](std::vector<uintptr_t>& args, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            // args[0]=vmctx, args[1]=this (Ladder), args[2]=user-GO
            if (args.size() >= 2) {
                auto* ladder = reinterpret_cast<::REManagedObject*>(args[1]);
                auto* leaning = sdk::find_type_definition("chainsaw.GmLeaningLadder");

                if (ladder != nullptr && leaning != nullptr && is_a(ladder, leaning)) {
                    RE4VRKillswitch::get()->m_leaning_ladder_mounted = true;
                }
            }

            return HookManager::PreHookResult::CALL_ORIGINAL;
        },
        [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
}

void RE4VRKillswitch::on_lua_state_created(sol::state& lua) {
    if (!m_cfg_loaded) {
        m_cfg_loaded = true;
        load_zones();
        load_ks_cfg();
    }

    // [PORTFIX 2026-09-06] Typdefinitionen aufloesen. Sie standen frueher NUR
    // hier -- on_lua_state_created laeuft aber erst im on_frame des
    // ScriptRunners, waehrend evaluate() schon ab dem ersten UpdateScene
    // tickt. In diesem Fenster war g_player_cam_td nullptr, is_a() meldete
    // false, und JEDER Zweig fiel auf 3rd Person durch -- im Messprotokoll
    // die allererste Zeile "grund=chainsaw.PlayerCameraController". Ein Retry
    // gab es nicht. Jetzt zusaetzlich lazy aus evaluate() heraus.
    resolve_type_defs();
    install_ladder_hook();

    // Startwerte, die die Lua beim Laden setzt.
    re4vr::lua_set_bool("__re4_ks_fp_enabled", m_fp_enabled);
    re4vr::lua_set_bool("__re4_ks2_as_ks4", m_ks2_as_ks4);
    // Die beiden Stellen-Timer beim Load nullen: sonst gilt eine Uhr als
    // "laengst abgelaufen" und der KS4-Teil wird beim naechsten Mal
    // uebersprungen.
    re4vr::lua_set_nil("__re4_evt40510_t");
    re4vr::lua_set_nil("__re4_gang3rd_t");

    // ------------------------------------------------------------------
    // [REQUIRE-MODUL] Die Tabelle unter package.loaded hinterlegen -- die
    // verbliebenen Lua-Dateien requiren unveraendert weiter.
    // ------------------------------------------------------------------
    auto t = lua.create_table();

    const auto ks = []() { return RE4VRKillswitch::get(); };

    t["is_active"] = [ks]() { return ks()->is_active(); };
    t["is_pin_release"] = [ks]() { return ks()->is_pin_release(); };
    t["is_ks2"] = [ks]() { return ks()->is_ks2(); };
    t["is_ks3"] = [ks]() { return ks()->is_ks3(); };
    t["is_ks4"] = [ks]() { return ks()->is_ks4(); };
    t["is_ks5"] = [ks]() { return ks()->is_ks5(); };
    t["is_fp_only"] = [ks]() { return ks()->is_fp_only(); };
    t["just_activated"] = [ks]() { return ks()->just_activated(); };
    t["just_deactivated"] = [ks]() { return ks()->just_deactivated(); };
    t["is_cutscene_active"] = [ks]() { return ks()->is_cutscene_active(); };
    t["is_real_cutscene"] = [ks]() { return ks()->is_real_cutscene_pub(); };
    t["is_crouch_active"] = [ks]() { return ks()->is_crouch_active(); };
    t["is_player_camera_active"] = [ks]() { return ks()->is_player_camera_active(); };
    t["is_pure_gameplay"] = [ks]() { return ks()->is_pure_gameplay(); };

    t["get_busy_controller"] = [ks]() { return ks()->get_busy_controller_pub(); };
    t["get_player_context"] = [ks]() { return ks()->get_player_context(); };
    t["get_player_body"] = [ks]() { return ks()->get_player_body(); };

    // Luas Getter liefern nil, wenn nichts bekannt ist -- sol::object bildet
    // das ab, ein blanker Rueckgabetyp wuerde 0 bzw. "" liefern.
    t["get_controller"] = [ks, &lua]() -> sol::object {
        const auto s = ks()->get_controller();

        return s.empty() ? sol::nil : sol::make_object(lua, s);
    };
    t["get_previous_controller"] = [ks, &lua]() -> sol::object {
        const auto s = ks()->get_previous_controller();

        return s.empty() ? sol::nil : sol::make_object(lua, s);
    };
    t["get_cam_state"] = [ks, &lua]() -> sol::object {
        const auto v = ks()->get_cam_state();

        return v.has_value() ? sol::make_object(lua, *v) : sol::nil;
    };
    t["get_stage_name"] = [ks, &lua]() -> sol::object {
        const auto v = ks()->get_stage_name();

        return v.has_value() ? sol::make_object(lua, *v) : sol::nil;
    };
    t["get_space_id"] = [ks, &lua]() -> sol::object {
        const auto v = ks()->get_space_id();

        return v.has_value() ? sol::make_object(lua, *v) : sol::nil;
    };
    t["get_activating_controller"] = [ks, &lua]() -> sol::object {
        const auto s = ks()->get_activating_reason();

        return s.empty() ? sol::nil : sol::make_object(lua, s);
    };

    t["get_anim_blend_back"] = [ks]() { return ks()->get_anim_blend_back(); };
    t["get_fp_enabled"] = [ks]() { return ks()->get_fp_enabled(); };
    t["set_fp_enabled"] = [ks](sol::object v) {
        ks()->set_fp_enabled(v.is<bool>() && v.as<bool>());
    };

    // [ZONES] mark/remove liefern ZWEI Werte (ok, zone|grund) -- ohne das Tupel
    // bricht #Monitor.lua.
    t["mark_current_zone"] = [ks](sol::object lvl, sol::object rad) {
        const int32_t level = lvl.is<int32_t>() ? lvl.as<int32_t>() : 0;
        std::optional<float> radius{};

        if (rad.is<float>()) {
            radius = rad.as<float>();
        }

        const auto [ok, msg] = ks()->mark_current_zone(level, radius);

        return std::make_tuple(ok, msg);
    };
    t["remove_last_zone"] = [ks]() {
        const auto [ok, msg] = ks()->remove_last_zone();

        return std::make_tuple(ok, msg);
    };
    t["reload_zones"] = [ks]() { return ks()->reload_zones(); };
    t["get_zone_count"] = [ks]() { return static_cast<int32_t>(ks()->get_zone_count()); };
    t["get_zones"] = [ks, &lua]() {
        auto out = lua.create_table();
        int idx = 1;

        for (const auto& z : ks()->get_zones()) {
            auto e = lua.create_table();
            e["stage"] = z.stage;
            e["space"] = z.space;
            e["x"] = z.x;
            e["y"] = z.y;
            e["z"] = z.z;
            e["r"] = z.r;
            e["level"] = z.level;

            if (z.camstate.has_value()) {
                e["camstate"] = *z.camstate;
            }

            e["name"] = z.name;
            out[idx++] = e;
        }

        return out;
    };
    t["get_current_entry"] = [ks, &lua]() -> sol::object {
        const auto e = ks()->current_entry();

        if (!e.has_value()) {
            return sol::nil;
        }

        auto out = lua.create_table();
        out["stage"] = e->stage;
        out["space"] = e->space;

        if (e->camstate.has_value()) {
            out["camstate"] = *e->camstate;
        }

        if (e->has_pos) {
            auto p = lua.create_table();
            p["x"] = e->pos.x;
            p["y"] = e->pos.y;
            p["z"] = e->pos.z;
            out["pos"] = p;
        }

        return out;
    };

    lua["package"]["loaded"]["re4vr/re4_vr_killswitch"] = t;

    // [FORENSIK] Funktions-Global der Lua. Sie setzt ihn JEDEN Frame im
    // is_pc-Zweig neu; hier reicht die einmalige Registrierung, weil das
    // Lambda den aktuellen Wert selbst holt. Leser hat er heute keinen mehr
    // (per Grep ueber alle Scripte und Module belegt) -- er kostet als Lambda
    // aber nichts und bleibt damit 1:1 erhalten.
    lua["__re4_ks_cam_now"] = [&lua]() -> sol::object {
        const auto v = RE4VRKillswitch::get()->get_cam_state();

        return v.has_value() ? sol::make_object(lua, *v) : sol::nil;
    };
}

void RE4VRKillswitch::on_lua_state_destroyed(sol::state& lua) {
    // 1:1 der re.on_script_reset-Block der Lua.
    m_killswitch_active = false;
    m_pin_release_active = false;
    m_ks2_active = false;
    m_ks3_active = false;
    m_ks5_active = false;
    m_fp_latch = false;
    m_fp_latch_state.reset();
    m_fp_latch_level = 0;
    m_damage_until = 0.0;
    m_jumpdown_until = 0.0;
    m_jumpdown_confirmed = false;
    m_was_active = false;
    m_prev_full_off = false;
    m_prev_ks4_exit = false;   // kein Phantom-Fade nach Reset Scripts
    m_p_ks4_exit_t.reset();    // laufendes Fade-Fenster verwerfen
    m_anim_blend_back = 0.0;
    m_force_killswitch = false;

    m_mfsm2_comp = nullptr;
    m_mfsm2_body = nullptr;
    m_motion_comp = nullptr;
    m_motion_body = nullptr;
    m_elev59100_comp = nullptr;
    m_railcar_mgr = nullptr;

    m_cur_episode_entry.reset();
    m_last_episode_entry.reset();

    load_zones();

    // ks4_active steht NICHT im Reset-Block der Lua -- 1:1 uebernommen.
    m_cfg_loaded = false;
}

void RE4VRKillswitch::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (name == nullptr || std::strcmp(name, "UpdateScene") != 0) {
        return;
    }

    // [FEHLER-DIAG] Ein Fehler in evaluate wuerde den ganzen Killswitch-Zustand
    // auf den Startwerten stehen lassen, ohne dass irgendwo etwas auffaellt.
    // Deshalb EINMAL in einem Global hinterlegen -- keine Datei, kein Log.
    try {
        evaluate();
        publish_globals();
    } catch (const std::exception& e) {
        if (!m_err_published) {
            m_err_published = true;
            re4vr::lua_set_string("__re4_ks_err", e.what());
        }
    } catch (...) {
        if (!m_err_published) {
            m_err_published = true;
            re4vr::lua_set_string("__re4_ks_err", "unbekannter Fehler in evaluate");
        }
    }
}

bool RE4VRKillswitch::is_ks4() const {
    // [JETSKI IMMER FP] Wie das Minecart (KS5): die Jetski-Fahrt bleibt in JEDER
    // Toggle-Stellung First-Person. Erkannt am eigenen Flag des Jetski-Zweigs,
    // nicht an der Stufe.
    if (m_ks4_active && m_p_jetski_active) {
        return true;
    }

    return m_ks4_active && m_fp_enabled;
}

// [AUTO_REDRAW] STRIKT: nur reines Gameplay. Gate fuer den Auto-Redraw im
// Holster -- Leiter/Vault/Finisher/Grapple/Fall laufen ueber vollen KS.
bool RE4VRKillswitch::is_pure_gameplay() {
    if (m_killswitch_active || m_pin_release_active) {
        return false;
    }

    // current_cam_state == nil = Cutscene/Event -> false.
    if (!m_current_cam_state.has_value()) {
        return false;
    }

    return is_gameplay_camstate(m_current_cam_state);
}

// [PUBLIC-UI] Schalter fuer die Public-Version: steht OHNE Tree direkt im
// Hauptmenue. Der Dev-Tree "RE4VR - Killswitch" ist seit 2026-07-23 raus.
// [MENUE-REIHENFOLGE 2026-09-07] Dieser Block ist PUBLIC (order 20) und
// gehoert ins nackte UI -- gezeichnet wird er von RE4VRMenu::draw_public.
void RE4VRKillswitch::draw_public_ui() {
    bool on = m_fp_enabled;

    if (g_framework->draw_menu_checkbox("Enable Firstperson Events", &on)) {
        set_fp_enabled(on);
    }
}

#endif // RE4
