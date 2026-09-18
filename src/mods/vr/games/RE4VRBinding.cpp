// ============================================================================
// RE4VRBinding -- Implementierung. Kopfkommentar: RE4VRBinding.hpp
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sdk/SceneManager.hpp"
#include "sdk/RETypeDB.hpp"

#include "../../../HookManager.hpp"
#include "../../../Mods.hpp"
#include "../../../VigemPad.hpp"
#include "../../VR.hpp"

#include "RE4VRBinding.hpp"
#include "RE4VRMovement.hpp"
#include "RE4VRCapacitive.hpp"
#include "RE4VRKillswitch.hpp"

// windows.h definiert min/max als MAKROS und zerlegt jedes std::min/std::max.
// Der Fork setzt kein NOMINMAX.
#undef min
#undef max

namespace {

constexpr const char* PREFS_PATH = "re4_vr/re4_vr_bindings.json";

// [DUAL_TRIGGER_MENU] Beide Trigger halten -> START. [2026-07-20] 3.0 -> 2.0
constexpr double DUAL_TRIGGER_HOLD_SEC = 2.0;
// [COMBO_GRACE 2026-08-11] Beide Halte-Kombos galten als losgelassen, sobald EIN
// EINZIGER Frame lang einer der Knoepfe nicht als gedrueckt gelesen wurde -- an
// der Trigger-Schwelle passiert das staendig, die Uhr fing unbemerkt neu an.
constexpr double COMBO_GRACE_SEC = 0.15;
// [START_HOLD 2026-08-11] START wurde nur EINEN Frame gesendet -- verpasst das
// Spiel den, passiert gar nichts.
constexpr int32_t START_HOLD_FRAMES = 20;

// [EASTER_EGG_FLAMETHROWER] Beide A-Buttons 5 s im Gameplay -> Flamethrower.
constexpr double EE_FLAME_HOLD_SEC = 5.0;
constexpr int32_t EE_FLAME_SOUND = 764414311;
constexpr int32_t EE_FLAME_ITEM_ID = 275957056;

constexpr int32_t EDGE_HOLD_FRAMES = 4;
constexpr int32_t GRENADE_COOLDOWN_FRAMES = 60;

// REFramework-VR-Menue Toggle-Sound. [2026-08-04] EINE Wwise-TriggerId auf dem
// SoundContainer des PLAYER-BODY -- bei JEDEM Charakter dieselbe.
constexpr uint32_t REFUI_SOUND = 801086617;

// [SCOPE_GRIP_DELAY 2026-08-11, gemessen] Bei Scope-Waffen rutschte beim
// Gripdruck GENAU EIN Frame Aim durch, bevor das Holster sein grab_armed setzen
// konnte -- das native Aim ist ein Latch, der eine Frame reicht.
constexpr double SCOPE_GRIP_AIM_DELAY = 0.08;

constexpr const char* ADA_PITCH_GUI = "Gui_ui2022";

// [ADA_RAW_A_ZONE 2026-07-20] Stellen, an denen das SPIEL selbst ein langes A
// verlangt (Hold-Prompt). Dort darf Adas A-Longpress-Remap NICHT greifen,
// sonst kommt beim Halten nie ein A an. Positionen aus den Monitor-Dumps.
//
// Die Lua kannte hier zusaetzlich einen Live-Override
// (_G.__re4_ada_raw_a_zones). Der entfaellt: kein Script und kein Modul hat
// ihn je gesetzt (per Grep ueber den ganzen Baum belegt). Eine neue Zone ist
// hier eine Zeile.
struct RawAZone {
    int32_t stage;
    float x, y, z, r;
};

constexpr std::array ADA_RAW_A_ZONES{
    RawAZone{44210, -0.06f, -0.81f, 181.36f, 3.0f},
    RawAZone{51859, -29.79f, 6.20f, -2.30f, 3.0f},      // Dump 2026-07-20 22:20:57
    RawAZone{51504, -33.42f, 28.89f, -69.48f, 3.0f},    // Dump 2026-07-20 22:52:22
    RawAZone{55852, 116.22f, -29.01f, -69.86f, 3.0f},   // Dump 2026-07-21 21:33:51
    RawAZone{56104, 207.78f, 50.29f, -94.80f, 3.0f},    // Dump 2026-07-21 22:44:19
    RawAZone{60870, 25.75f, 1.44f, 248.27f, 3.0f},      // Dump 2026-07-22 13:49:56
    RawAZone{60880, 40.14f, -3.66f, 260.06f, 3.0f},     // Dump 2026-07-22 14:00:06
};

// [SYMBOL/CHURCH/STONE-RIDDLE] Alle GimmickFix-Raetsel mit identischen
// R-Stick->LB/RB-Bindings. Name historisch beibehalten.
const std::unordered_set<int32_t> RIDDLE_STAGES{44110, 45401, 51503, 51502};

// [DODGE_GIMMICK] Getriggert wird auf die GRUPPE Stage + Occupied + CamType.
const std::unordered_set<int32_t> DODGE_GMK_STAGES{60871, 60874};
constexpr int32_t DODGE_GMK_PRIO = 23;      // CH_JACKED_GMK_HIGH
constexpr int32_t DODGE_GMK_CAMTYPE = 5;    // CameraControlType.GimmickOperate

// [CHAPTER_RESULT] Das Post-Kapitel-Menue setzt KEINES der Standard-Signale.
constexpr std::array CHAPTER_RESULT_GUI_NAMES{
    "ChapterEnd", "ChapterDetailResult", "ChapterDetailResultGuide",
    "ChapterStats", "GameClearResult",
};

// [FILE_READER] Dokument-/Buch-Leser (in-world Fund UND Inventar-Ansicht).
constexpr std::array FILE_READER_GUI_NAMES{"FileDetail", "FileSelect"};

// [utility/RE4] is_in_inventory_menu haengt an diesen beiden GUIs.
constexpr std::array INVENTORY_GUI_NAMES{"Gui_ui3030", "Gui_ui3040"};


// [RT-SPERRE IM STAGGER 15.09.2026] Liegt die Waffe wirklich in der Hand?
//   true    = Gun.State ist Holding
//   false   = etwas anderes (im Stagger: None)
//   nullopt = NICHT lesbar -> daraus wird NIE eine Sperre abgeleitet
//
// Bewusst hier lokal statt aus RE4VRReload4 geholt: dieses Modul hat keinen
// Singleton-Zugriff, und ein Quereinstieg haette zwei Module verkoppelt.
// Der Weg ist derselbe wie dort: PlayerEquipment.WeaponList -> get_Item(wid)
// -> get_CurrentState. pe() und equip_wid() kommen aus dem Frame-Cache,
// kosten also nichts.
// [DAUERFEUER 15.09.2026] Zeitstempel der letzten regulaeren Torabfrage und
// des letzten durchgelassenen Schusses, dazu der Zaehler der Blocks.
double g_checkgunfire_t = -100.0;
double g_last_fire_t = -100.0;
int32_t g_dauerfeuer_blocks = 0;

// Ist die aktuelle Waffe FullAuto? Dann greift der Riegel nicht -- fuer
// Vollautomaten ist nicht gemessen, ob die Engine vor jedem Schuss fragt, und
// die TMP darf auf keinen Fall Schuesse verlieren.
bool gun_is_fullauto_local() {
    static std::optional<int32_t> fullauto_val{};

    if (!fullauto_val.has_value()) {
        if (auto* td = sdk::find_type_definition("chainsaw.WeaponStructureParam.ShootType");
            td != nullptr) {
            if (auto* f = td->get_field("FullAuto"); f != nullptr) {
                fullauto_val = f->get_data<int32_t>(nullptr);
            }
        }
    }

    if (!fullauto_val.has_value()) {
        return false;
    }

    auto* ctx = re4vr::fc::on() ? re4vr::fc::ctx() : nullptr;
    auto* hu = (ctx != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
        : nullptr;
    auto* w = (hu != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon")
        : nullptr;

    if (w == nullptr) {
        return false;
    }

    if (int32_t st = 0; re4vr::try_call<int32_t>(w, "get_ShootType", st)) {
        return st == *fullauto_val;
    }

    return false;
}

std::optional<bool> gun_is_holding_local() {
    static std::optional<int32_t> holding_val{};

    if (!holding_val.has_value()) {
        if (auto* td = sdk::find_type_definition("chainsaw.Gun.State"); td != nullptr) {
            if (auto* f = td->get_field("Holding"); f != nullptr) {
                holding_val = f->get_data<int32_t>(nullptr);
            }
        }
    }

    if (!holding_val.has_value()) {
        return std::nullopt;
    }

    auto* p = re4vr::fc::on() ? re4vr::fc::pe() : nullptr;

    if (p == nullptr) {
        return std::nullopt;
    }

    ::REManagedObject* wl = nullptr;

    if (auto* td = utility::re_managed_object::get_type_definition(p); td != nullptr) {
        if (auto* f = td->get_field("WeaponList"); f != nullptr) {
            wl = f->get_data<::REManagedObject*>(p);
        }
    }

    const auto ewid = re4vr::fc::equip_wid();

    if (wl == nullptr || !ewid.has_value()) {
        return std::nullopt;
    }

    auto* a = re4vr::call_safe<::REManagedObject*>(wl, "get_Item", *ewid);

    if (a == nullptr) {
        return std::nullopt;
    }

    int32_t st = 0;

    if (!re4vr::try_call<int32_t>(a, "get_CurrentState", st)) {
        return std::nullopt;
    }

    return st == *holding_val;
}

double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

float clampf(float v, float a, float b) {
    if (v < a) {
        return a;
    }

    if (v > b) {
        return b;
    }

    return v;
}

bool call_bool(::REManagedObject* o, std::string_view name) {
    if (o == nullptr) {
        return false;
    }

    bool out = false;

    return re4vr::try_call<bool>(o, name, out) && out;
}

std::string mstr(::REManagedObject* o) {
    if (o == nullptr) {
        return {};
    }

    return utility::re_string::get_string(reinterpret_cast<::SystemString*>(o));
}

sdk::RETypeDefinition* g_player_cam_td{nullptr};
sdk::RETypeDefinition* g_gimmick_fix_td{nullptr};
sdk::RETypeDefinition* g_sound_container_td{nullptr};

}   // namespace

std::shared_ptr<RE4VRBinding>& RE4VRBinding::get() {
    static auto inst = std::make_shared<RE4VRBinding>();

    return inst;
}

// ============================================================================
// PREFS
// ============================================================================

void RE4VRBinding::load_prefs() {
    const auto d = re4vr::json_load(PREFS_PATH);

    if (!d.is_object()) {
        return;
    }

    // Lua prueft `~= nil` fuer die Booleans (jeder Wert zaehlt, truthy) und
    // `type(...) == "number"` fuer die Zahlen.
    const auto b = [&](const char* k, bool cur) {
        const auto it = d.find(k);

        return it != d.end() && !it->is_null() ? (it->is_boolean() && it->get<bool>()) : cur;
    };
    const auto f = [&](const char* k, float cur) {
        const auto it = d.find(k);

        return it != d.end() && it->is_number() ? it->get<float>() : cur;
    };

    m_prefs.hide_ref_overlay = b("hide_ref_overlay", m_prefs.hide_ref_overlay);
    m_prefs.long_press_sec = f("long_press_sec", m_prefs.long_press_sec);
    m_prefs.short_min_sec = f("short_min_sec", m_prefs.short_min_sec);
    m_prefs.enable_180_rotation = b("enable_180_rotation", m_prefs.enable_180_rotation);
    m_prefs.turn180_window_sec = f("turn180_window_sec", m_prefs.turn180_window_sec);
    m_prefs.turn180_ly_sec = f("turn180_ly_sec", m_prefs.turn180_ly_sec);
    m_prefs.turn180_sec = f("turn180_sec", m_prefs.turn180_sec);
    m_prefs.trackpad_scroll = f("trackpad_scroll", m_prefs.trackpad_scroll);
    g_framework->set_vr_menu_trackpad_scale(m_prefs.trackpad_scroll);
    m_prefs.turn180_rb_sec = f("turn180_rb_sec", m_prefs.turn180_rb_sec);
    m_prefs.enable_snapturn = b("enable_snapturn", m_prefs.enable_snapturn);
    m_prefs.snapturn_deg = static_cast<int32_t>(f("snapturn_deg",
                                                  static_cast<float>(m_prefs.snapturn_deg)));
    m_prefs.snapturn_thresh = f("snapturn_thresh", m_prefs.snapturn_thresh);
}

void RE4VRBinding::save_prefs() {
    nlohmann::json d{};
    d["hide_ref_overlay"] = m_prefs.hide_ref_overlay;
    d["long_press_sec"] = m_prefs.long_press_sec;
    d["short_min_sec"] = m_prefs.short_min_sec;
    d["enable_180_rotation"] = m_prefs.enable_180_rotation;
    d["turn180_window_sec"] = m_prefs.turn180_window_sec;
    d["turn180_ly_sec"] = m_prefs.turn180_ly_sec;
    d["turn180_sec"] = m_prefs.turn180_sec;
    d["trackpad_scroll"] = m_prefs.trackpad_scroll;
    d["turn180_rb_sec"] = m_prefs.turn180_rb_sec;
    d["enable_snapturn"] = m_prefs.enable_snapturn;
    d["snapturn_deg"] = m_prefs.snapturn_deg;
    d["snapturn_thresh"] = m_prefs.snapturn_thresh;
    re4vr::json_save(PREFS_PATH, d);
}

// ============================================================================
// Kleine Helfer
// ============================================================================

// Luas edge_detect: beim Druck einen Halte-Timer starten und ihn
// EDGE_HOLD_FRAMES lang als "gedrueckt" melden, danach still bis zum Loslassen.
bool RE4VRBinding::edge_detect(Edge& e, bool pressed) {
    if (!pressed) {
        e.prev = false;
        e.timer = 0;

        return false;
    }

    if (!e.prev) {
        e.prev = true;
        e.timer = EDGE_HOLD_FRAMES;
    }

    if (e.timer > 0) {
        --e.timer;

        return true;
    }

    return false;
}

std::optional<int32_t> RE4VRBinding::binding_equip_weapon_id() {
    // [FRAME-CACHE] Die 0-Sonderregel bleibt: der Cache liefert die ROHE ID,
    // hier gilt weiterhin "0 heisst keine Waffe" -> kein Wert.
    const auto w = re4vr::fc::equip_wid();

    if (w.has_value() && *w != 0) {
        return w;
    }

    return std::nullopt;
}

bool RE4VRBinding::binding_is_grenade_equipped_live() {
    const auto w = binding_equip_weapon_id();

    return w.has_value() && *w >= 5400 && *w <= 5410;
}

// [GRAPPLE] Niedergerungen? Live verifiziert: get_IsInGrappleDamage=true,
// stabil (kein Flackern). Genutzt, um beim Messer den RT-Mute auszunehmen.
bool RE4VRBinding::player_in_grapple() {
    return call_bool(re4vr::fc::ctx(), "get_IsInGrappleDamage");
}

// [BATTLE] Battle ist KEIN eigener State-Wert, sondern ein Flag-BIT
// (0x1000000000000000, Bit 60), das oben auf die Lokomotion draufgeODERt wird
// -> per Bit-Test pruefen, nicht per Gleichheit.
// Das Bit liegt weit ueber 2^53: als double gelesen ginge es verloren, deshalb
// get_state_bits (uint64) -- dieselbe Stelle wie im Killswitch.
bool RE4VRBinding::player_in_battle() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    const auto st = re4vr::get_state_bits(ctx);

    if (!st.has_value()) {
        return false;
    }

    constexpr uint64_t BATTLE_FLAG = 0x1000000000000000ULL;

    return (*st & BATTLE_FLAG) != 0ULL;
}

// [GIGANTE-RT 2026-08-30] Sitzt Leon dem El Gigante auf dem Ruecken? Bewusst so
// eng wie moeglich: get_IsFatalKill UND die Busy-Kamera ist eine ActionCamera,
// die der GIGANTE angefordert hat (Requester-GO heisst ch1f*, gemessen
// ch1f0z0_body). Beides zusammen gibt es nur waehrend dieser Sequenz.
// KOSTET NICHTS im Normalbetrieb: wird nur bei gezogenem Trigger gerufen.
bool RE4VRBinding::player_on_gigante() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr || !call_bool(ctx, "get_IsFatalKill")) {
        return false;
    }

    auto* busy = RE4VRKillswitch::get()->get_busy_controller_pub();

    if (busy == nullptr) {
        return false;
    }

    auto* rq = re4vr::call_safe<::REManagedObject*>(busy, "get_Requester");

    if (rq == nullptr) {
        return false;
    }

    auto* nm = re4vr::call_safe<::REManagedObject*>(rq, "get_Name");

    if (nm == nullptr) {
        return false;
    }

    const std::string s = mstr(nm);

    return s.rfind("ch1f", 0) == 0;
}

// Parent-Kette des Body ueber hoechstens 10 Stufen nach einem Namen absuchen.
// Gate ist zusaetzlich Stage_Space == 46900_46900.
bool RE4VRBinding::stage_parent_chain_has(const char* prefix) {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    int32_t stage = 0;
    int32_t space = 0;

    if (!re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", stage)
        || !re4vr::try_call<int32_t>(ctx, "get_CurrentSpaceID", space)) {
        return false;
    }

    if (stage != 46900 || space != 46900) {
        return false;
    }

    auto* current = re4vr::fc::body_tf();

    if (current == nullptr) {
        return false;
    }

    for (int i = 1; i <= 10; ++i) {
        auto* parent_tf = re4vr::call_safe<::REManagedObject*>(current, "get_Parent");

        if (parent_tf == nullptr) {
            return false;
        }

        if (auto* parent_go = re4vr::call_safe<::REManagedObject*>(parent_tf, "get_GameObject");
            parent_go != nullptr) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(parent_go, "get_Name");

            if (nm != nullptr && mstr(nm).find(prefix) != std::string::npos) {
                return true;
            }
        }

        current = parent_tf;
    }

    return false;
}

bool RE4VRBinding::is_ada_raw_a_zone() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    int32_t stage = 0;

    if (!re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", stage)) {
        return false;
    }

    auto* tf = re4vr::fc::body_tf();

    if (tf == nullptr) {
        return false;
    }

    glm::vec3 p{};

    if (!re4vr::obj_get_vec3(tf, "get_Position", p)) {
        return false;
    }

    for (const auto& z : ADA_RAW_A_ZONES) {
        if (stage != z.stage) {
            continue;
        }

        const float dx = p.x - z.x;
        const float dy = p.y - z.y;
        const float dz = p.z - z.z;

        if ((dx * dx + dy * dy + dz * dz) <= (z.r * z.r)) {
            return true;
        }
    }

    return false;
}

// [SYMBOL_RIDDLE] Stage + die Busy-Kamera ist ein GimmickFixCameraController.
// Grund fuer die Cam-Bedingung: oeffnet man in derselben Stage das ECHTE
// Pause-Menue, ist die Busy-Cam KEIN GimmickFix -> nur das Raetsel trifft zu,
// das Menue-DPAD-Blaettern bleibt unangetastet.
bool RE4VRBinding::is_symbol_riddle() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    int32_t st = 0;

    if (!re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", st)
        || RIDDLE_STAGES.count(st) == 0) {
        return false;
    }

    auto* busy = RE4VRKillswitch::get()->get_busy_controller_pub();

    if (busy == nullptr || g_gimmick_fix_td == nullptr) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(busy);

    if (td == nullptr) {
        return false;
    }

    try {
        return td->is_a(g_gimmick_fix_td);
    } catch (...) {
        return false;
    }
}

// [TURRET 2026-07-16] MG-Turret (GimmickType.InstalledMachineGun). EIGENER
// Signalweg, NICHT die grosse Kanone. Aus dem StateParam der Busy-Kamera,
// jeden Frame frisch -> nie stale.
bool RE4VRBinding::is_turret_mounted() {
    if (!m_turret_gimmick.has_value()) {
        // Fallback 6, falls das TDB mal wackelt (live verifiziert).
        m_turret_gimmick =
            re4vr::enum_value("chainsaw.CameraDefine.GimmickType", "InstalledMachineGun")
                .value_or(6);
    }

    auto* busy = RE4VRKillswitch::get()->get_busy_controller_pub();

    if (busy == nullptr) {
        return false;
    }

    // Der Killswitch liest denselben GimmickType -- dort steht die erprobte
    // Behandlung des ValueType-StateParams (get_data_raw-Container-Flag).
    const auto gt = RE4VRKillswitch::get()->read_gimmick_type_pub(busy);

    return gt.has_value() && *gt == *m_turret_gimmick;
}

void RE4VRBinding::resolve_chapter_gui_enums() {
    if (m_chapter_enums_done) {
        return;
    }

    m_chapter_enums_done = true;

    for (const char* nm : CHAPTER_RESULT_GUI_NAMES) {
        if (const auto v = re4vr::enum_value("chainsaw.GuiType", nm); v.has_value()) {
            m_chapter_gui_vals.push_back(*v);
        }
    }

    m_render_default_val = re4vr::enum_value("chainsaw.RenderOutputType", "Default").value_or(0);
}

void RE4VRBinding::resolve_file_reader_enums() {
    if (m_file_enums_done) {
        return;
    }

    m_file_enums_done = true;

    for (const char* nm : FILE_READER_GUI_NAMES) {
        if (const auto v = re4vr::enum_value("chainsaw.GuiType", nm); v.has_value()) {
            m_file_reader_gui_vals.push_back(*v);
        }
    }

    resolve_chapter_gui_enums();   // stellt m_render_default_val sicher
}

bool RE4VRBinding::is_chapter_result_gui_open() {
    auto* gm = re4vr::fc::managed_singleton("chainsaw.GuiManager");

    if (gm == nullptr) {
        return false;
    }

    resolve_chapter_gui_enums();

    for (const int32_t gv : m_chapter_gui_vals) {
        bool open = false;

        if (re4vr::try_call<bool>(gm, "isOpenGui", open, gv, m_render_default_val) && open) {
            return true;
        }
    }

    return false;
}

bool RE4VRBinding::is_file_reader_gui_open() {
    auto* gm = re4vr::fc::managed_singleton("chainsaw.GuiManager");

    if (gm == nullptr) {
        return false;
    }

    resolve_file_reader_enums();

    for (const int32_t gv : m_file_reader_gui_vals) {
        bool open = false;

        if (re4vr::try_call<bool>(gm, "isOpenGui", open, gv, m_render_default_val) && open) {
            return true;
        }
    }

    return false;
}

// [utility/RE4] is_in_inventory_menu: wurde Gui_ui3030/3040 in den letzten
// 0,1 s gezeichnet? Der Stempel kommt aus on_pre_gui_draw_element.
bool RE4VRBinding::is_in_inventory_menu() {
    return (clock_now() - m_inventory_shown_t) <= 0.1;
}

// [IGNORE_HUD_OFF 2026-07-17] ignore_hud_off laesst NUR die IsHudOff-Quelle weg.
// WOFUER: "HUD aus" ist KEIN Menue -- das Spiel blendet es auch bei Events aus
// ("force look" vor dem Ausweich-Prompt, live belegt: hudOff=true, paused/
// pauseLock/case alle false). Dadurch landete das Binding im MENU-Branch und
// der Ausweich-Grip war tot.
// NICHT hudOff generell rausnehmen: dann liefe bei jedem HUD-aus-Event das
// volle Gameplay-Binding.
bool RE4VRBinding::is_any_menu_open(bool ignore_hud_off) {
    // Ohne Spieler-Kontext gilt "Menue" -- 1:1 der Lua-Ausstieg.
    if (re4vr::fc::ctx() == nullptr || re4vr::fc::body_go() == nullptr) {
        return true;
    }

    // [PAUSE] Greift auch bei "Pause WAEHREND Cutscene", wo KEINE GUI-Flags
    // gesetzt sind.
    if (auto* pm = re4vr::fc::managed_singleton("share.PauseManager"); pm != nullptr) {
        if (call_bool(pm, "isPaused()")) {
            return true;
        }
    }

    auto* gm = re4vr::fc::managed_singleton("chainsaw.GuiManager");

    if (!ignore_hud_off && gm != nullptr && call_bool(gm, "get_IsHudOff")) {
        return true;
    }

    if (gm != nullptr && call_bool(gm, "get_hasOccupiedPauseMenuSystemLock")) {
        return true;
    }

    if (auto* acm = re4vr::fc::managed_singleton("chainsaw.AttacheCaseManager"); acm != nullptr) {
        if (call_bool(acm, "get_IsAttacheCaseBusy")) {
            return true;
        }
    }

    // [TYPEWRITER/ARMOURY] get_IsAttacheCaseBusy deckt NUR den Koffer ab -> die
    // Archivbox fiel durch und landete im KS-Branch (X/Move tot).
    // get_IsTypewriterWindow ist true, solange das GANZE Fenster offen ist.
    if (auto* am = re4vr::fc::managed_singleton("chainsaw.ArmouryManager"); am != nullptr) {
        if (call_bool(am, "get_IsTypewriterWindow")) {
            return true;
        }
    }

    // [2026-08-09] Methodenname EXAKT klein anfangend, KEIN "get_" davor:
    // "get_IsMapGuiOpen" existiert NICHT (live belegt) -- der Call warf jeden
    // Frame, die Karte galt damit NIE als Menue.
    if (auto* mm = re4vr::fc::managed_singleton("chainsaw.MapManager"); mm != nullptr) {
        if (call_bool(mm, "isMapGuiOpen")) {
            return true;
        }
    }

    if (is_chapter_result_gui_open()) {
        return true;
    }

    return is_file_reader_gui_open();
}

// [MAP_TRIGGERS 2026-08-15] Ist GERADE die Kartenansicht offen? Eigene, kleine
// Abfrage statt eines Flags: nur die laufende Antwort des Spiels zaehlt, es
// gibt nichts zu setzen und nichts zurueckzusetzen.
bool RE4VRBinding::is_map_open_now() {
    auto* mm = re4vr::fc::managed_singleton("chainsaw.MapManager");

    return mm != nullptr && call_bool(mm, "isMapGuiOpen");
}

// [MERC_GAMEPLAY_BRANCH] Bewusst NUR das Global lesen: keine eigenen
// Singleton-Lookups pro Frame, und ohne Mercs-Modul ist das Ergebnis false.
bool RE4VRBinding::is_mercs_active() {
    return re4vr::lua_get_tribool("__re4_in_mercs") == 1;
}

// [CHAR_STICKY 2026-07-19] "Ada ist fuer Ada, Leon fuer Leon -- immer."
// Ueber das zentrale __re4_char_now: das haelt den letzten BEKANNTEN Charakter
// fest und ueberbrueckt Body-Aussetzer (Waffenwechsel/Holster/Laden). Eine
// eigene Body-Pruefung lieferte in solchen Frames false = "Leon", und Ada
// rutschte in Leons Zweig -- beim L.A-Longpress genau der Moment, in dem der
// Grapple nicht kommt.
bool RE4VRBinding::is_ada_active() {
    const auto ch = re4vr::lua_call_global_string("__re4_char_now");

    if (!ch.empty()) {
        return ch == "ada";
    }

    // Fallback auf die alte Direktpruefung, falls char_now nicht geladen ist.
    auto* body = re4vr::fc::body_go();

    if (body == nullptr) {
        return false;
    }

    auto* nm = re4vr::call_safe<::REManagedObject*>(body, "get_Name");

    return nm != nullptr && mstr(nm) == "ch3a8z0_body";
}

// [ADA_PITCH_GUI 2026-07-22] Solange bei ADA Gui_ui2022 gezeichnet wird, ist
// der Pitch frei -- sonst greift der RY-Lock. NUR ADA.
bool RE4VRBinding::ada_pitch_free() {
    if ((clock_now() - m_ada_pitch_gui_last) >= 0.25) {
        return false;
    }

    return is_ada_active();
}


// ============================================================================
// Init
// ============================================================================

// Luas ensure_init: VR-Handles holen und das Pad anlegen. Ohne beides passiert
// gar nichts -- die Lua stieg an derselben Stelle aus (`if not ok3 then
// return false end`).
bool RE4VRBinding::ensure_init() {
    if (m_inited) {
        return true;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return false;
    }

    if (vr->get_controllers().size() < 2) {
        return false;
    }

    // [PORTFIX 2026-09-06 OPENXR] KEINE Handle-Nullpruefung: Luas `if js then`
    // ist KEIN Gueltigkeitstest -- 0 ist in Lua WAHR. Unter OpenXR ist
    // get_left_joystick() aber genau 0 (VRRuntime::Hand::LEFT) und damit
    // identisch mit k_ulInvalidInputValueHandle. Der Vergleich stellte dort
    // die linke Hand komplett tot. Vorlage: RE4VRMinecart.cpp:1281.
    // Die Action-Handles holt der Port bei jedem Zugriff frisch von VR (sie
    // sind dort Member) -- der ACT-Cache der Lua entfaellt ersatzlos.

    if (!VigemPad::get().init()) {
        return false;
    }

    m_inited = true;

    return true;
}

namespace {

// Luas safe_digital(action, hand).
bool digital(vr::VRActionHandle_t action, vr::VRInputValueHandle_t hand) {
    auto* vr = VR::get().get();

    if (vr == nullptr || action == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active(action, hand);
    } catch (...) {
        return false;
    }
}

// [MENUE-STEUERUNG 11.09.2026] Ungesperrt -- nur fuer die Kombi LT + linkes B, die
// das Menue auch dann schliessen muss, wenn alle normalen Eingaben gesperrt sind.
bool digital_raw(vr::VRActionHandle_t action, vr::VRInputValueHandle_t hand) {
    auto* vr = VR::get().get();

    if (vr == nullptr || action == vr::k_ulInvalidActionHandle) {
        return false;
    }

    try {
        return vr->is_action_active_raw(action, hand);
    } catch (...) {
        return false;
    }
}

}   // namespace

// ============================================================================
// Fernglas
// ============================================================================

// [BINO 2026-07-22] Die vier Werte sind Slider im First-Person-Tree,
// persistiert in dessen JSON und hier ueber __re4_bino_cfg gelesen. Die
// Zahlen im Header bleiben als Fallback, falls firstperson noch nicht geladen
// hat -> Verhalten dann exakt wie vorher.
float RE4VRBinding::bino_val(const char* key, float fallback) {
    return static_cast<float>(re4vr::lua_get_table_number("__re4_bino_cfg", key, fallback));
}

// Kind-Kette des Body nach einem GameObject "Binoculars" absuchen.
bool RE4VRBinding::is_binoculars_active() {
    auto* tf = re4vr::fc::body_tf();

    if (tf == nullptr) {
        return false;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int32_t count = 0;

    while (child != nullptr && count < 200) {
        ++count;

        if (auto* go = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");
            go != nullptr) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(go, "get_Name");

            if (nm != nullptr && mstr(nm) == "Binoculars") {
                return true;
            }
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return false;
}

// Beide Frame-Callbacks brauchen denselben Szene-Lauf. Das Ergebnis gilt nur
// fuer diesen Engine-Frame.
bool RE4VRBinding::is_binoculars_active_this_frame() {
    auto* vr = VR::get().get();
    const int32_t frame = vr != nullptr ? vr->get_frame_count() : -1;

    if (frame < 0) {
        return is_binoculars_active();
    }

    if (m_bino_scan_frame == frame) {
        return m_bino_scan_active;
    }

    m_bino_scan_frame = frame;
    m_bino_scan_active = is_binoculars_active();

    return m_bino_scan_active;
}

// Fernglas-Zoom: Erkennung + Stick-Eingabe + Kamera-Offset, jeden Frame.
// Das ist Luas erster on_frame (Z.671).
void RE4VRBinding::bino_tick() {
    if (is_binoculars_active_this_frame()) {
        if (!m_bino.active) {
            m_bino.active = true;
            m_bino.offset = bino_val("start", m_bino.start_offset);
        }

        auto* vr = VR::get().get();

        if (vr != nullptr && vr->is_hmd_active() && vr->is_using_controllers()) {
            const float stick_y = vr->get_left_stick_axis().y;

            if (std::fabs(stick_y) > 0.1f) {
                m_bino.offset -= stick_y * bino_val("speed", m_bino.stick_speed) * 0.016f;
                m_bino.offset = clampf(m_bino.offset, bino_val("min", m_bino.min_offset),
                                       bino_val("max", m_bino.max_offset));
            }
        }
    } else if (m_bino.active) {
        m_bino.active = false;
        m_bino.offset = 0.0f;
    }

    bino_apply_offset();
}

// [BINO_PHASEN 11.09.2026] s. RE4VRBinding.hpp. Gerechnet wie bisher: Kamera
// entlang Rotation * (0,0,1) um m_bino.offset verschieben.
void RE4VRBinding::bino_apply_offset() {
    if (!m_bino.active || m_bino.offset == 0.0f) {
        m_bino_last_written.reset();
        return;
    }

    auto* camera = sdk::get_primary_camera();

    if (camera == nullptr) {
        return;
    }

    auto* cam_go = re4vr::call_safe<::REManagedObject*>(
        reinterpret_cast<::REManagedObject*>(camera), "get_GameObject");

    if (cam_go == nullptr) {
        return;
    }

    auto* cam_tf = re4vr::call_safe<::REManagedObject*>(cam_go, "get_Transform");

    if (cam_tf == nullptr) {
        return;
    }

    glm::vec3 cam_pos{};
    glm::quat cam_rot{};

    if (!re4vr::obj_get_vec3(cam_tf, "get_Position", cam_pos)
        || !re4vr::obj_get_quat(cam_tf, "get_Rotation", cam_rot)) {
        return;
    }

    // Kamera steht noch auf unserem Wert -> das Spiel hat nicht zurueckgesetzt.
    if (m_bino_last_written.has_value()
        && glm::dot(cam_pos - *m_bino_last_written, cam_pos - *m_bino_last_written) < 0.0001f) {
        return;
    }

    const glm::vec3 forward = cam_rot * glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::vec3 target = cam_pos + forward * m_bino.offset;

    re4vr::call_safe<void*>(cam_tf, "set_Position", glm::vec4{target.x, target.y, target.z, 1.0f}, true);
    m_bino_last_written = target;
}

// [BINO_PHASEN 11.09.2026] Ausschliesslich der Fernglas-Offset -- dieselben Phasen
// wie die HMD-Offsets der First Person (UnlockScene, LateUpdateBehavior,
// BeginRendering). Ohne aktives Fernglas kehrt bino_apply_offset sofort zurueck.
void RE4VRBinding::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (name != nullptr && (std::strcmp(name, "UnlockScene") == 0 || std::strcmp(name, "BeginRendering") == 0)) {
        bino_apply_offset();
    }
}

void RE4VRBinding::on_application_entry(void* entry, const char* name, size_t hash) {
    if (name != nullptr && (std::strcmp(name, "LateUpdateBehavior") == 0 || std::strcmp(name, "BeginRendering") == 0)) {
        bino_apply_offset();
    }
}

// ============================================================================
// Prompt-Stempel aus dem GUI-Draw
// ============================================================================

namespace {

// Control-Baum nach einem Namen absuchen, mit hartem Budget (80 Knoten, Tiefe
// 5) -- 1:1 Luas find_ctl.
::REManagedObject* find_ctl(::REManagedObject* c, const char* want, int32_t depth,
                            int32_t& budget) {
    while (c != nullptr && budget < 80) {
        ++budget;

        auto* nm = re4vr::call_safe<::REManagedObject*>(c, "get_Name");

        if (nm != nullptr && mstr(nm) == want) {
            return c;
        }

        if (depth < 5) {
            if (auto* ch = re4vr::call_safe<::REManagedObject*>(c, "get_Child"); ch != nullptr) {
                if (auto* hit = find_ctl(ch, want, depth + 1, budget); hit != nullptr) {
                    return hit;
                }
            }
        }

        c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
    }

    return nullptr;
}

bool ctl_visible(::REManagedObject* ch, const char* name) {
    if (ch == nullptr) {
        return false;
    }

    int32_t budget = 0;
    auto* hit = find_ctl(ch, name, 0, budget);

    return hit != nullptr && call_bool(hit, "get_Visible");
}

}   // namespace

bool RE4VRBinding::on_pre_gui_draw_element(::REComponent* element, void* context) {
    auto* go = re4vr::call_safe<::REManagedObject*>(
        reinterpret_cast<::REManagedObject*>(element), "get_GameObject");

    if (go == nullptr) {
        return true;
    }

    auto* nm_obj = re4vr::call_safe<::REManagedObject*>(go, "get_Name");

    if (nm_obj == nullptr) {
        return true;
    }

    const std::string n = mstr(nm_obj);

    if (n == ADA_PITCH_GUI) {
        m_ada_pitch_gui_last = clock_now();
    }

    // [utility/RE4] is_in_inventory_menu haengt an diesen beiden GUIs. Das
    // Original prueft zusaetzlich das Update-Byte an Offset 0x10.
    for (const char* inv : INVENTORY_GUI_NAMES) {
        if (n == inv) {
            const auto* raw = reinterpret_cast<const uint8_t*>(go);

            if (raw[0x10] == 1) {
                m_inventory_shown_t = clock_now();
            }

            break;
        }
    }

    // [RECT_PROMPT] Nur dieses eine Element -> der Baum-Lauf laeuft nicht im
    // Normalbetrieb, sondern nur waehrend des Prompts.
    if (n == "Gui_ui2191_3") {
        auto* view = re4vr::call_safe<::REManagedObject*>(
            reinterpret_cast<::REManagedObject*>(element), "get_View");
        auto* ch = view != nullptr
            ? re4vr::call_safe<::REManagedObject*>(view, "get_Child")
            : nullptr;

        // Adas Rect-Prompt.
        if (ctl_visible(ch, "c_btn_rect_anime")) {
            m_rect_prompt_last = clock_now();
        }

        // [LEON_LB_PROMPT] Leon nutzt im selben Container "c_btn_rect" (OHNE
        // _anime) -- exakter Name trennt es sauber von Adas Control.
        if (ctl_visible(ch, "c_btn_rect")) {
            m_leon_lb_last = clock_now();
        }

        // [DODGE_CIRCLE] Das Ausweichen selbst.
        if (ctl_visible(ch, "c_btn_circle_anime")) {
            m_dodge_circle_last = clock_now();
        }
    }

    return true;
}

// [RECT_PROMPT] Frisch gezeichnet (0.15 s) UND in der Stage, in der die
// Aufforderung vorkommt.
bool RE4VRBinding::is_rect_prompt_now() {
    if ((clock_now() - m_rect_prompt_last) >= 0.15) {
        return false;
    }

    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    int32_t st = 0;

    return re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", st) && st == 60874;
}

// [LEON_LB_PROMPT] Es ist eine GEGNER-ATTACKE -> kann in JEDER Stage kommen,
// deshalb KEIN Stage-Gate. Stattdessen hart auf Leon begrenzt, damit es Adas
// stage-gegateten RB-Prompt nie kapert.
bool RE4VRBinding::is_leon_lb_prompt_now() {
    if ((clock_now() - m_leon_lb_last) >= 0.15) {
        return false;
    }

    return !is_ada_active();
}

// [DODGE_CIRCLE 2026-07-31] Zwei Wege:
// 1. POSITIV: c_btn_circle_anime frisch gezeichnet (Xbox-Symbolsatz).
// 2. FALLBACK: der Container-Stempel aus crosshair (deckt den PS-Symbolsatz
//    ab) -- aber NUR, wenn nicht gleichzeitig der Rect- oder LB-Prompt lebt.
//    Genau diese beiden teilen sich den Container und haben das falsche B
//    (= Crouch aus dem Nichts) erzeugt.
bool RE4VRBinding::is_dodge_prompt_now() {
    if ((clock_now() - m_dodge_circle_last) < 0.15) {
        return true;
    }

    if (!re4vr::lua_call_global_bool("__re4_is_dodge_prompt", false)) {
        return false;
    }

    return !is_rect_prompt_now() && !is_leon_lb_prompt_now();
}

// [DODGE_GIMMICK 2026-07-22] Das bildschirmfuellende "Ausweichen!"-Overlay
// laesst sich NICHT ueber GUI-Namen fassen (Toggle-Probe: selbst mit ALLEN
// ausgeblendeten Elementen erscheint es). Per Sweep-Probe wurde der ZUSTAND
// gemessen -- getriggert wird auf die GRUPPE Stage + Occupied 23 + CamType 5.
// Steht bewusst hinter der Grip-Abfrage: die Lookups laufen nur bei
// gedruecktem Grip.
bool RE4VRBinding::is_dodge_gimmick_now() {
    auto* ctx = re4vr::fc::ctx();

    if (ctx == nullptr) {
        return false;
    }

    int32_t stage = 0;

    if (!re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", stage)
        || DODGE_GMK_STAGES.count(stage) == 0) {
        return false;
    }

    auto* occ = re4vr::call_safe<::REManagedObject*>(ctx, "get_OccupiedInfo");

    if (occ == nullptr) {
        return false;
    }

    // [PORTFIX 07.09.2026] chainsaw.OccupiedMediatorPriority ist ein
    // System.Byte-Enum: die Engine gibt AL zurueck, die oberen 24 Bit sind
    // ABI-seitig undefiniert. Als int32 gelesen kam hier Muell an und der
    // Vergleich gegen DODGE_GMK_PRIO schlug praktisch immer fehl.
    // RE4VRKillswitch hat denselben Fehler schon am 06.09. behoben (dort
    // matchte KEIN Priority-Zweig mehr) -- diese Stelle wurde dabei uebersehen.
    // Gefunden ueber den Typ-Abgleich aller Zahl-Getter: get_Priority wurde an
    // zwei Stellen mit UNTERSCHIEDLICHEN Typen gelesen.
    uint8_t prio_raw = 0;

    if (!re4vr::try_call<uint8_t>(occ, "get_Priority", prio_raw)
        || static_cast<int32_t>(prio_raw) != DODGE_GMK_PRIO) {
        return false;
    }

    auto* csys = re4vr::fc::managed_singleton("chainsaw.CameraSystem");

    if (csys == nullptr) {
        return false;
    }

    auto* main = re4vr::call_safe<::REManagedObject*>(csys, "get_MainCameraController");

    if (main == nullptr) {
        return false;
    }

    int32_t cam_type = 0;

    return re4vr::try_call<int32_t>(main, "get_BusyCameraType", cam_type)
        && cam_type == DODGE_GMK_CAMTYPE;
}

// ============================================================================
// [PROMPT_GRIP] Prompt-gebundene Grip-Bindings zentral, mit FLANKE +
// EINMAL-IMPULS statt "solange der Grip gehalten wird".
//
// Warum: ein Level-Trigger schickte den Knopf jeden Frame des Fensters raus --
// ein B, das nach dem Ausweichen noch anliegt, ist im Normalzustand CROUCH.
// Und zwei getrennte Bloecke konnten gleichzeitig feuern (bei Leons LB-Prompt
// gingen B UND LB raus).
//
// Gezuendet wird NICHT auf der Grip-Flanke, sondern sobald Fenster UND Grip
// zusammen anliegen -- wer den Grip schon vor dem Prompt haelt (in der Hektik
// der Normalfall), weicht trotzdem aus.
//
// REIHENFOLGE nach dem Fix vom 2026-08-02:
//  1. Adas Rect-Prompt (RB) -- stage-gegatet, bleibt UNANGETASTET vorn.
//  2. Ausweichen (B) ueber den POSITIVEN Beleg c_btn_circle_anime. Muss VOR
//     den LB-Zweig: beim Ausweichen war gleichzeitig "c_btn_rect" gezeichnet,
//     der LB-Zweig griff zuerst und schickte LB -- das Ausweichen fiel
//     komplett aus, obwohl es korrekt erkannt wurde.
//  3. Leons LB-Prompt, danach der Container-Fallback (der das falsche Crouch
//     erzeugt hatte und deshalb hinter Rect UND LB steht).
// ============================================================================

void RE4VRBinding::apply_prompt_grip(Frame& f, bool grip_down) {
    const double now = clock_now();

    if (grip_down) {
        const char* btn = nullptr;

        if (is_rect_prompt_now()) {
            btn = "RB";
        } else if ((now - m_dodge_circle_last) < 0.15) {
            btn = "B";
        } else if (is_leon_lb_prompt_now()) {
            btn = "LB";
        } else if (is_dodge_prompt_now()) {
            btn = "B";
        } else if (is_dodge_gimmick_now()) {
            btn = "B";
        }

        if (btn != nullptr) {
            m_pgrip.win_t = now;

            if (!m_pgrip.fired) {
                m_pgrip.fired = true;
                m_pgrip.btn = btn;
                m_pgrip.frames = 4;
                m_pgrip.until_t = now + 0.12;
            }
        } else if ((now - m_pgrip.win_t) > 0.20) {
            m_pgrip.fired = false;   // Fenster zu -> fuer das naechste scharf
        }
    } else {
        m_pgrip.fired = false;       // Grip losgelassen -> wieder scharf
    }

    // Frames allein frieren ein, wenn der Zweig zwischendurch nicht laeuft --
    // deshalb zusaetzlich die Zeit-Deadline.
    if (m_pgrip.frames > 0 && now > m_pgrip.until_t) {
        m_pgrip.frames = 0;
    }

    if (m_pgrip.frames > 0 && m_pgrip.btn != nullptr) {
        const std::string_view b{m_pgrip.btn};

        if (b == "RB") {
            f.rb = true;
        } else if (b == "LB") {
            f.lb = true;
        } else if (b == "B") {
            f.b = true;
        }

        --m_pgrip.frames;
    }
}

// [PROMPT_RT] Dasselbe Muster fuer den Finisher-RT: bisher wurde RT jeden
// Frame des Fensters durchgereicht -- ein RT, der nach dem Finisher noch
// anliegt, ist im Normalzustand wieder Messer-Flip/Angriff.
// 6 Frames / 0.15 s: etwas laenger als beim Grip, der Finisher darf auf keinen
// Fall verschluckt werden.
void RE4VRBinding::apply_finisher_rt(Frame& f, bool window_on, bool trigger_down) {
    const double now = clock_now();

    if (window_on && trigger_down) {
        m_rtp.win_t = now;

        if (!m_rtp.fired) {
            m_rtp.fired = true;
            m_rtp.frames = 6;
            m_rtp.until_t = now + 0.15;
        }
    } else if (!trigger_down || (now - m_rtp.win_t) > 0.20) {
        m_rtp.fired = false;
    }

    if (m_rtp.frames > 0 && now > m_rtp.until_t) {
        m_rtp.frames = 0;
    }

    if (m_rtp.frames > 0) {
        f.rt = 1.0f;
        --m_rtp.frames;
    }
}

// ============================================================================
// Snapturn / 180-Grad-Kehrtwende
// ============================================================================

// WARUM AM ENGINE-YAW UND NICHT AN DER VR-ORIGIN: movement setzt
// set_rotation_offset JEDEN Frame, um das Headset im Bild auszugleichen. Ein
// Snap ueber die Origin waere im naechsten Frame ueberschrieben -- zwei Regler
// auf derselben Groesse, die bekannte Falle.
// Movement dreht die Welt ueber _Yaw am PlayerCameraController und arbeitet
// dort rein ADDITIV -> unser einmaliger Sprung wird im naechsten Frame als
// neuer Ausgangswert uebernommen, nicht bekaempft.
void RE4VRBinding::add_camera_yaw(float delta) {
    auto* busy = RE4VRKillswitch::get()->get_busy_controller_pub();

    if (busy == nullptr || g_player_cam_td == nullptr) {
        return;
    }

    auto* td = utility::re_managed_object::get_type_definition(busy);

    if (td == nullptr) {
        return;
    }

    try {
        if (!td->is_a(g_player_cam_td)) {
            return;   // nur anfassen, wenn der Controller der Gameplay-Kamera gehoert
        }

        auto* fld = td->get_field("_Yaw");

        if (fld == nullptr) {
            return;
        }

        // _Yaw liegt direkt am Controller-Objekt (kein ValueType-Container)
        // -> get_data_raw mit false, einmal lesen, einmal zurueckschreiben.
        auto* raw = reinterpret_cast<float*>(fld->get_data_raw(busy, false));

        if (raw != nullptr) {
            *raw = *raw + delta;
        }
    } catch (...) {
    }
}

void RE4VRBinding::qt_reset() {
    m_qt.phase = 0;
    m_qt.window_time = 0.0f;
    m_qt.seq = 0;
    m_qt.cooldown = false;
    m_qt.post = 0;
}

// [2026-08-10 ZEITBASIERT] Die Sequenz lief frueher ueber FRAMES (4 + 6 = 10).
// Gemessen: die Erkennung feuert sauber, das Spiel macht aber nichts daraus.
// Bei 60 fps waren die 6 RB-Frames 100 ms -- in VR laeuft das Spiel mit
// 90-120 fps, damit blieben nur 50-66 ms, zu wenig fuer die gehaltene Kombi.
void RE4VRBinding::qt_update(float ly, float dt) {
    if (m_qt.seq > 0) {
        ++m_qt.seq;

        const double ly_s = m_prefs.turn180_ly_sec;
        const double rb_s = m_prefs.turn180_rb_sec;

        if ((clock_now() - m_qt.t0) > (ly_s + rb_s)) {
            m_qt.seq = 0;
            m_qt.cooldown = true;
            m_qt.phase = 0;
            m_qt.post = QuickTurn::POST_FRAMES;
        }

        return;
    }

    if (m_qt.post > 0) {
        --m_qt.post;
    }

    if (m_qt.cooldown) {
        // erst wieder scharf, wenn der Stick tatsaechlich losgelassen wurde
        if (ly > QuickTurn::RELEASE) {
            m_qt.cooldown = false;
        }

        return;
    }

    if (m_qt.phase == 0) {
        if (ly <= QuickTurn::DOWN) {
            m_qt.phase = 1;
        }
    } else if (m_qt.phase == 1) {
        if (ly >= QuickTurn::RELEASE) {
            m_qt.phase = 2;
            m_qt.window_time = 0.0f;
        }
    } else if (m_qt.phase == 2) {
        m_qt.window_time += dt;

        if (ly <= QuickTurn::DOWN) {
            // zweiter Tipp im Fenster -> Sequenz laeuft
            m_qt.seq = 1;
            m_qt.phase = 0;
            m_qt.t0 = clock_now();
        } else if (m_qt.window_time > m_prefs.turn180_window_sec) {
            m_qt.phase = 0;   // Fenster verstrichen -> war ein normaler Rueckwaertsgang
        }
    }
}

// [SCOPE_GRIP_DELAY] Aim ab der DRUCKFLANKE kurz zurueckhalten, damit das
// Holster-Script sich melden kann. Wird in dieser Zeit kein Grab angemeldet,
// aimt es ganz normal weiter -- nur ein paar Millisekunden spaeter.
// Gilt NUR fuer Scope-Waffen.
bool RE4VRBinding::scope_grip_aim_blocked(bool grip_now) {
    if (re4vr::lua_get_number("__re4_scope_wid", -1.0) < 0.0) {
        m_scope_grip_edge_t.reset();
        m_scope_grip_prev = grip_now;

        return false;
    }

    if (grip_now && !m_scope_grip_prev) {
        m_scope_grip_edge_t = clock_now();
    } else if (!grip_now) {
        m_scope_grip_edge_t.reset();
    }

    m_scope_grip_prev = grip_now;

    return m_scope_grip_edge_t.has_value()
        && (clock_now() - *m_scope_grip_edge_t) < SCOPE_GRIP_AIM_DELAY;
}

// ============================================================================
// Sounds / Easteregg
// ============================================================================

void RE4VRBinding::play_gui_sound(int32_t enum_val) {
    if (enum_val == 0) {
        return;
    }

    auto* gsm = re4vr::fc::managed_singleton("chainsaw.GuiSoundManager");

    if (gsm == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(gsm, "wwiseTriggerTarget(chainsaw.gui.GuiSoundType)", enum_val);
}

// [BODY_SOUND 2026-08-04] Wwise-TriggerId auf dem SoundContainer des aktuellen
// PLAYER-BODY -- derselbe Container wie die Gesten-Sprueche. Kein
// Charakter-Gate noetig: die ID sitzt auf jedem dieser Container unter
// derselben Nummer. KEIN Komponenten-Cache (ueberlebt sonst keinen
// Savegame-Load) -- laeuft nur bei Tastendruck.
void RE4VRBinding::play_body_sound(uint32_t id) {
    if (id == 0 || g_sound_container_td == nullptr) {
        return;
    }

    auto* go = re4vr::fc::body_go();

    if (go == nullptr) {
        return;
    }

    auto* con = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)",
                                                     g_sound_container_td->get_runtime_type());

    if (con == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(con, "trigger(System.UInt32)", id);
}

// [EASTER_EGG_FLAMETHROWER] Pro SPIELSTAND statt globalem Flag: das alte
// Config-Flag ueberlebte "Neues Spiel" und blockierte den Grant auch dort, wo
// der FT gar nicht im Archiv liegt. Jetzt haengt alles an existsItem gegen die
// AKTUELLE Aufbewahrung.
void RE4VRBinding::grant_flamethrower_to_armoury() {
    auto* am = re4vr::fc::managed_singleton("chainsaw.ArmouryManager");

    if (am == nullptr) {
        return;   // ohne Aufbewahrung kein Erfolg -> spaeter erneut moeglich
    }

    bool has = false;

    if (re4vr::try_call<bool>(am, "existsItem", has, EE_FLAME_ITEM_ID) && has) {
        return;   // schon drin -> nichts tun, kein Doppel, kein Spam-Sound
    }

    auto* gu = sdk::find_type_definition("chainsaw.ChainsawGuiUtil");
    auto* gen = gu != nullptr ? gu->get_method("generateItem") : nullptr;

    if (gen == nullptr) {
        return;
    }

    ::REManagedObject* item = nullptr;

    try {
        auto ctx = sdk::get_thread_context();
        // generateItem(itemId, itemCount, durability, ammoId, ammoCount, changeId)
        item = gen->call<::REManagedObject*>(ctx, nullptr, EE_FLAME_ITEM_ID, 1, -1, -1, 100,
                                             false);
    } catch (...) {
        item = nullptr;
    }

    if (item == nullptr) {
        return;
    }

    // Belohnungs-Sound nur bei echtem Neuzugang -- deshalb erst nach dem
    // erfolgreichen Einlegen.
    bool added = false;

    try {
        re4vr::call_safe<void*>(am, "addArmouryItem(chainsaw.Item)", item);
        added = true;
    } catch (...) {
        added = false;
    }

    if (added) {
        play_gui_sound(EE_FLAME_SOUND);
    }
}

// [REF_OVERLAY] Das Palm-Overlay lebt im Plugin re_vr.dll und ist nur ueber
// Lua erreichbar. Das ist hier unkritisch: die drei Aufrufstellen sind Sync,
// ein Tick alle 30 Frames und der Toggle -- kein heisser Pfad.
void RE4VRBinding::overlay_set_enabled(bool on) {
    re4vr::lua_call_table_fn_bool("overlay", "set_enabled", on);
}

void RE4VRBinding::overlay_tick() {
    re4vr::lua_call_table_fn_void("overlay", "tick");
}

// ============================================================================
// Main Frame -- ersetzt den share.hid.Device-update-Hook.
//
// Die REIHENFOLGE der Branch-Kette ist tragend und steht so seit dem
// HID-Script. Begruendungen jeweils am Zweig.
// ============================================================================

void RE4VRBinding::on_frame() {
    // Luas erster on_frame (Fernglas-Zoom) laeuft VOR dem Haupt-Handler --
    // dieselbe Reihenfolge wie in der Datei.
    bino_tick();

    // Luas dritter on_frame (PlayWork-Pause-Notaus) steht am Dateiende, also
    // NACH dem Haupt-Handler. Er ist selbst gedrosselt (2x/s).
    if (!ensure_init()) {
        playwork_pause_tick();
        merchant_pause_tick();

        return;
    }

    auto* vr = VR::get().get();
    const bool vr_active = vr != nullptr && vr->is_hmd_active() && vr->is_using_controllers();

    if (!vr_active) {
        // Beim Deaktivieren einmal einen Neutral-Frame senden (kein stale Input)
        if (m_was_active) {
            Frame neutral{};
            apply_frame(neutral);
            m_was_active = false;
        }

        playwork_pause_tick();
        merchant_pause_tick();

        return;
    }

    m_was_active = true;

    // [REF_OVERLAY] DLL-Zustand einmal syncen, dann gedrosselt re-asserten
    // (faengt Overlay-Respawns nach SteamVR-Szenenwechsel / REF-Reset).
    if (!m_ref_overlay_synced) {
        m_ref_overlay_synced = true;
        overlay_set_enabled(!m_prefs.hide_ref_overlay);
    }

    if (m_prefs.hide_ref_overlay) {
        ++m_ref_overlay_tick;

        if (m_ref_overlay_tick >= 30) {
            m_ref_overlay_tick = 0;
            overlay_tick();
        }
    }

    const auto left_stick = vr->get_left_stick_axis();
    auto right_stick = vr->get_right_stick_axis();

    // Physischer rechter Stick (Comfort-/Body-Turn) -- movement schiebt die
    // gespeicherte Blickrichtung nach, solange er aktiv ist.
    re4vr::lua_set_bool("__vr_user_stick_active",
                        std::fabs(right_stick.x) > 0.12f || std::fabs(right_stick.y) > 0.12f);

    // [REVOLVER COCK] Rohe rechte-Stick-Y veroeffentlichen -> reload2 nutzt
    // Stick-DOWN zum Hahn-Spannen (wp4500). Y ist fuer die Kamera ohnehin
    // gesperrt (decoupled pitch) -> konfliktfrei.
    re4vr::lua_set_number("__vr_right_stick_y", right_stick.y);

    // HMD-Movement: Sim-Yaw auf dem Stick dreht nur den Koerper; die Sicht
    // bleibt ueber die gespeicherte apply_hmd-Basis entkoppelt.
    if (re4vr::lua_get_tribool("__vr_hmd_movement_enabled") == 1
        && re4vr::lua_get_tribool("__vr_camera_decoupled") == 1
        && re4vr::lua_get_tribool("__vr_save_restore_active") != 1) {
        if (!is_in_inventory_menu()) {
            const float sim_x = static_cast<float>(re4vr::lua_get_number("__vr_yaw_sim_x", 0.0));

            if (std::fabs(sim_x) > 0.001f) {
                right_stick.x = clampf(right_stick.x + sim_x, -1.0f, 1.0f);
            }
        }
    }

    // Frame-Zustand: Sticks 1:1 durchreichen.
    Frame f{};
    f.lx = left_stick.x;
    f.ly = left_stick.y;
    f.rx = right_stick.x;
    f.ry = right_stick.y;

    // [BACKWARD] KEIN Magnitude-Cap: normales Rueckwaertsgehen kann das Spiel
    // nativ. Eingegriffen wird nur bei Dash rueckwaerts (NO_BACK_SPRINT unten).
    const bool moving_backward = f.ly < 0.0f;

    const auto lh = vr->get_left_joystick();
    const auto rh = vr->get_right_joystick();

    const auto act_trigger = vr->get_action_trigger();
    const auto act_grip = vr->get_action_grip();
    const auto act_a = vr->get_action_a_button();
    const auto act_b = vr->get_action_b_button();
    // [TRACKPAD 15.09.2026] Nur Index/Vive haben eines; auf Quest bleibt das
    // Handle ungueltig und digital() liefert schlicht false -- dort aendert
    // sich dadurch NICHTS.
    const auto act_tpclick = vr->get_action_touchpad_click();
    const auto act_joy = vr->get_action_joystick_click();
    const auto act_dial = vr->get_action_weapon_dial();

    // --- Grip-Zuordnungen ------------------------------------------------

    // holster setzt das, wenn bare-handed + geholstert -> der rechte Grip darf
    // das virtuelle Pad nicht erreichen (sonst zieht das Spiel automatisch die
    // letzte Waffe). Das Holster liest den rohen Grip weiter selbst.
    const auto right_grip_maps_to_gamepad = [&]() -> bool {
        if (!digital(act_grip, rh)) {
            return false;
        }

        if (re4vr::lua_get_tribool("__vr_in_holster_zone") == 1) {
            return false;
        }

        // [BARE_HANDS] Leere Haende -> Right-Grip NICHT ans Gamepad: sonst
        // mappt er auf f.LB und das Spiel zieht die letzte Waffe hervor.
        if (re4vr::lua_get_tribool("__vr_bare_hands") == 1) {
            // [STAGGER_DRAW] Ausnahme: kurz nach einem Damage-Stagger DARF der
            // Right-Grip die letzte Waffe wieder ziehen (die versteckt die
            // Engine im Stagger).
            const double u = re4vr::lua_get_number("__vr_stagger_recent_until", -1.0);

            if (!(u > 0.0 && clock_now() < u)) {
                return false;
            }
        }

        if (re4vr::lua_get_tribool("__vr_block_shoot_ready") == 1) {
            return false;
        }

        if (re4vr::lua_get_tribool("__vr_holster_block_right_grip_gamepad") == 1) {
            return false;
        }

        // z.B. Bolt-Action offen -> kein Aimen
        return re4vr::lua_get_tribool("__vr_block_aim") != 1;
    };

    const auto holster_right_grip_counts_as_knife_ready = [&]() -> bool {
        return re4vr::lua_get_tribool("__vr_holster_rgrip_as_knife_ready") == 1
            && digital(act_grip, rh);
    };

    const bool l_grip_from_left = digital(act_grip, lh);
    // holster: rechte Hand ueber dem left_chest-Slot + R-Grip -> zaehlt wie der
    // linke Grip fuer Messer-LB / Swing / Holster-Flanke.
    const bool holster_left_chest_rgrip_as_lgrip =
        re4vr::lua_get_tribool("__vr_holster_left_chest_rgrip_as_left_grip") == 1
        && digital(act_grip, rh);

    const bool head_flash_blocks = re4vr::lua_get_tribool("__vr_in_head_flashlight_zone") == 1;
    const bool mag_holster_blocks = re4vr::lua_get_tribool("__vr_in_mag_holster_zone") == 1;

    // [TWO_HAND_IK] Der linke Grip ist vom Messer ENTKOPPELT -> frei fuer die
    // 2-Hand-IK. Die Messer-Funktion bleibt im Script: der
    // Holster-Chest-R-Grip-Pfad loest weiter Knife-Ready/Swing aus, nur der
    // LINKE Grip nicht mehr.
    constexpr bool TWO_HAND_FREE_LEFT_GRIP = true;
    bool l_grip_melee_from_left =
        l_grip_from_left && !head_flash_blocks && !mag_holster_blocks;

    if (TWO_HAND_FREE_LEFT_GRIP) {
        l_grip_melee_from_left = false;
    }

    bool l_grip_effective_for_melee =
        l_grip_melee_from_left || holster_left_chest_rgrip_as_lgrip;

    // [SLIDE_RACK] Waehrend ein Rack noetig ist: linker Grip = Slide-Grab,
    // NICHT Messer.
    if (re4vr::lua_get_tribool("__vr_rack_block_left_knife") == 1) {
        l_grip_effective_for_melee = false;
    }

    // --- Granate ---------------------------------------------------------

    // [GRENADE_IN_HAND] Die MAIN-WeaponID erkennt eine ueber das Holster
    // gezogene Granate NICHT (Main bleibt z.B. LE5) -> f.LB feuerte beim
    // Wurf-Grip. __vr_grenade_in_hand ist das ehrliche "Granate in der Hand".
    const bool gren_live_binding = binding_is_grenade_equipped_live()
        || re4vr::lua_get_tribool("__vr_grenade_in_hand") == 1;

    const bool vr_is_grenade_equipped = re4vr::lua_get_tribool("vr_is_grenade_equipped") == 1;
    const bool vr_grenade_throw = re4vr::lua_get_tribool("vr_grenade_throw") == 1;

    bool grenade_motion_rt = false;
    {
        const bool signal = (vr_is_grenade_equipped || gren_live_binding) && vr_grenade_throw;

        if (signal && !m_prev_vr_grenade_throw) {
            m_grenade_rt_pulse_frames = 6;
        }

        m_prev_vr_grenade_throw = signal;

        if (m_grenade_rt_pulse_frames > 0) {
            grenade_motion_rt = true;
            --m_grenade_rt_pulse_frames;
        }
    }

    const bool grenade_motion_rt_armed = grenade_motion_rt;
    // Wurfobjekt draussen: Messer-Motion->RT und Phantom-LB-Grips blocken.
    const bool grenade_blocks_right_hand_knife_motion =
        vr_is_grenade_equipped || gren_live_binding;
    const bool disable_motion_grenade_rt =
        re4vr::lua_get_tribool("__vr_disable_motion_grenade_rt") == 1;

    // --- Eingaben --------------------------------------------------------

    const bool l_trigger = digital(act_dial, lh);
    const bool r_trigger = digital(act_trigger, rh);

    // Roher physischer Trigger, IMMER (unabhaengig von Waffe/Branch/Aim) ->
    // Grapple-RT.
    re4vr::lua_set_bool("__vr_raw_r_trigger", r_trigger);

    // [RT-DIAG 2026-09-08 -- WEGWERF] Der RT-Blocker nach Cutscenes zeigt sich im
    // Lua-Log als __vr_raw_r_trigger=0. Das hat ZWEI moegliche Ursachen, die von
    // aussen nicht zu trennen sind: die Action liefert wirklich false, ODER diese
    // Zeile wird gar nicht mehr erreicht (dann steht der Global nur still).
    // Der Zaehler trennt genau das: steigt er im Bug weiter, laeuft die Zeile.
    // Zusaetzlich der rechte B-Button als Gegenprobe DERSELBEN Hand -- ist er 1
    // waehrend rt 0 bleibt, ist nur die Trigger-Action betroffen, nicht die Hand.
    {
        static double s_input_n = 0.0;
        s_input_n += 1.0;
        re4vr::lua_set_number("__re4_dbg_input_n", s_input_n);
        re4vr::lua_set_bool("__re4_dbg_rb_raw", digital(act_b, rh));
    }

    const bool knife_equipped = re4vr::lua_get_tribool("__re4_knife_equipped") == 1;
    const std::string knife_hand = re4vr::lua_get_string("__re4_knife_hand");
    const bool knife_left_clone = re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
    const bool finisher_prompt = re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false);

    // [KNIFE_FLIP 2026-07-03] Messer in der Hand: RT-Flanke togglet den
    // 180-Grad-Reverse-Grip. Der RT-Ausgang ist beim Messer ohnehin stillgelegt
    // -> der physische Trigger ist frei dafuer.
    // [KNIFE_HAND] Nur wenn das Messer RECHTS liegt.
    if (knife_equipped && knife_hand != "left") {
        // Im Grapple feuert der Trigger statt zu flippen -> Toggle aussetzen,
        // prev_rt aber weiter pflegen (kein Phantom-Toggle beim Verlassen).
        // [FINISHER] Bei sichtbarem Prompt togglet RT NICHT -> RT ist dann
        // echter RT (Finisher).
        if (r_trigger && re4vr::lua_get_tribool("__vr_knife_flip_prev_rt") != 1
            && !player_in_grapple() && !finisher_prompt) {
            re4vr::lua_set_bool("__vr_knife_flip",
                                re4vr::lua_get_tribool("__vr_knife_flip") != 1);
        }

        re4vr::lua_set_bool("__vr_knife_flip_prev_rt", r_trigger);
    } else {
        re4vr::lua_set_bool("__vr_knife_flip_prev_rt", false);

        // Flip NICHT zwangsweise auf false, wenn das Messer LINKS liegt -> dann
        // gehoert der Toggle dem LINKEN Trigger.
        if (knife_hand != "left" && !knife_left_clone) {
            re4vr::lua_set_bool("__vr_knife_flip", false);
        }
    }

    // [KNIFE_FLIP LINKS / FL_FLIP] Kurzer Tap = Flip, laenger Halten = DPAD.
    // Der Flip feuert beim LOSLASSEN -> kein Zucken beim Halten.
    // Vorrang hat IMMER das Messer; die Lampe bekommt den Tap nur, wenn sie an
    // ist und die linke Hand nichts Besseres tut.
    bool lt_flip_shift = false;
    const double LT_FLIP_TAP = re4vr::lua_get_number("__re4_knife_lt_flip_tap", 0.18);
    const bool lt_knife_left = (knife_equipped && knife_hand == "left") || knife_left_clone;
    const bool lt_flip_fl = !lt_knife_left
        && re4vr::lua_get_tribool("__re4_fl_active") == 1
        && re4vr::lua_get_tribool("__re4_fl_hand_busy") != 1;

    if (lt_knife_left || lt_flip_fl) {
        if (l_trigger) {
            if (!m_lt_flip_prev) {
                m_lt_flip_press_t = clock_now();   // Rising Edge: Tap-Timer starten
                m_lt_flip_hold = false;
            }

            if (!m_lt_flip_hold && (clock_now() - m_lt_flip_press_t) >= LT_FLIP_TAP) {
                m_lt_flip_hold = true;   // Halten erkannt -> DPAD, Flip unterdruecken
            }

            lt_flip_shift = m_lt_flip_hold;
        } else {
            if (m_lt_flip_prev && !m_lt_flip_hold && !player_in_grapple()) {
                if (lt_knife_left) {
                    re4vr::lua_set_bool("__vr_knife_flip",
                                        re4vr::lua_get_tribool("__vr_knife_flip") != 1);
                } else {
                    re4vr::lua_set_bool("__vr_fl_flip",
                                        re4vr::lua_get_tribool("__vr_fl_flip") != 1);
                }
            }

            m_lt_flip_hold = false;
        }

        m_lt_flip_prev = l_trigger;
    } else {
        m_lt_flip_prev = false;
        m_lt_flip_hold = false;
    }

    const bool l_abutton = digital(act_a, lh);
    const bool r_abutton = digital(act_a, rh);
    bool l_bbutton = digital(act_b, lh);
    bool r_bbutton = digital(act_b, rh);

    // [MANUAL_RELOAD] Autoritative B-Quelle fuer die Reload-Module. VOR dem
    // Consume veroeffentlicht, damit der echte Tastendruck ankommt.
    re4vr::lua_set_bool("__vr_raw_r_bbutton", r_bbutton);

    // Der rechte B wird von der manuellen Reload-Logik konsumiert -> dann NICHT
    // als Gamepad-X weiterreichen (kein nativer Reload).
    // [MENU_B_FIX 2026-07-17] ABER im Menue/Typewriter NICHT konsumieren: dort
    // ist R.B = Xbox-X (Move Item). Sonst schluckt eine verwaltete Waffe den B
    // auch im Inventar -> "X geht nicht".
    if (re4vr::lua_get_tribool("__vr_manual_reload_consume_b") == 1 && !is_any_menu_open()) {
        r_bbutton = false;
    }

    const bool l_joyclick = digital(act_joy, lh);
    const bool r_joyclick = digital(act_joy, rh);
    const bool l_dpad_up = digital(vr->get_action_dpad_up(), lh);
    const bool l_dpad_down = digital(vr->get_action_dpad_down(), lh);
    const bool l_dpad_left = digital(vr->get_action_dpad_left(), lh);
    const bool l_dpad_right = digital(vr->get_action_dpad_right(), lh);

    bool handle_pause = false;

    if (auto* rt = vr->get_runtime(); rt != nullptr && rt->handle_pause) {
        // [MENUE-STEUERUNG 11.09.2026] Die System-/Menue-Taste kommt als Runtime-
        // Ereignis, nicht ueber is_action_active -- deshalb hier eigens sperren.
        // Das Ereignis wird trotzdem verbraucht, sonst kaeme es nach dem
        // Schliessen des Menues nachtraeglich an.
        handle_pause = !vr->is_menu_input_blocked();
        rt->handle_pause = false;
    }

    // --- Wiederverwendete Teilstuecke ------------------------------------

    // DPAD-Shift: L.Trigger gehalten + R-Stick -> DPAD; sonst die echten
    // DPad-Actions.
    const auto apply_dpad_shift = [&]() {
        // [KNIFE_FLIP LINKS / FL_FLIP] Besitzt Messer oder Lampe den LT-Tap,
        // wird LT erst ab der Halteschwelle DPAD-Shift -- sonst loest jeder
        // Flip nebenbei ein DPAD aus.
        const bool shift_on =
            l_trigger && ((!lt_knife_left && !lt_flip_fl) || lt_flip_shift);

        if (shift_on) {
            if (right_stick.y >= 0.9f) {
                f.dpad_up = true;
            } else if (right_stick.y <= -0.9f) {
                f.dpad_down = true;
            }

            if (right_stick.x >= 0.9f) {
                f.dpad_right = true;
            } else if (right_stick.x <= -0.9f) {
                f.dpad_left = true;
            }

            // R-Stick geht waehrend des Shifts NUR in die Waffenwahl -- nicht
            // ans Pad (sonst rotiert die Kamera im Hintergrund mit).
            f.rx = 0.0f;
        } else {
            if (l_dpad_up) {
                f.dpad_up = true;
            }

            if (l_dpad_down) {
                f.dpad_down = true;
            }

            if (l_dpad_left) {
                f.dpad_left = true;
            }

            if (l_dpad_right) {
                f.dpad_right = true;
            }
        }
    };

    // L.Grip -> Knife Ready: GECLEART 2026-07-07 (toter Pfad). Der linke Grip
    // gehoert dem Links-Messer-Script. Feuert bewusst NICHTS mehr.
    const auto apply_l_grip_knife = [&]() {
        (void)l_grip_effective_for_melee;
    };

    // R.Trigger/Motion -> RT inkl. Granaten-Cooldown.
    const auto apply_rt_attack = [&]() {
        if (m_grenade_throw_cooldown > 0) {
            --m_grenade_throw_cooldown;
            re4vr::lua_set_bool("vr_knife_swing", false);   // Rest-Swing vom Wurf unterdruecken
        }

        if (vr_grenade_throw || vr_is_grenade_equipped || gren_live_binding) {
            m_grenade_throw_cooldown = GRENADE_COOLDOWN_FRAMES;
        }

        // [KNIFE_MELEE_REWORK] Der Messer-Swing zuendet NICHT mehr RT -- das
        // echte, selbst detektierte Melee liegt in weapons und konsumiert
        // vr_knife_swing dort. Nur noch der Granaten-Wurf triggert Motion->RT.
        const bool motion_attack = grenade_motion_rt_armed && !disable_motion_grenade_rt;

        // Slide-Rack: leeres Mag -> Feuern gesperrt bis nachgeladen UND
        // gerackt. Gilt nur fuers Gun-Feuern; Granate bleibt erlaubt.
        const bool block_empty =
            re4vr::lua_get_tribool("__vr_block_fire_when_empty") == 1 && !motion_attack;

        // [SND] Dry-Fire: reload liest die Flanke und spielt den Klick.
        re4vr::lua_set_bool("__re4_empty_trigger_held",
                            r_trigger && block_empty && !motion_attack);

        // [QUICK-KNIFE-BLOCK] RT nur an die Engine geben, wenn wirklich gezielt
        // wird ODER ein Granaten-Wurf laeuft. Sonst loest RT OHNE Aim bei einer
        // Schusswaffe den nativen Quick-Knife aus.
        const bool aiming = re4vr::lua_get_tribool("__vr_aim_input") == 1;

        if (((r_trigger && aiming) || motion_attack) && !block_empty) {
            f.rt = 1.0f;
            re4vr::lua_set_number("__re4_rt_given_t", clock_now());
        }

        // [GRENADE-AIM-FORCE 2026-07-23, Log-belegt] Der native Wurf braucht RT
        // UND Aim GLEICHZEITIG. Der Wurf loest beim Grip-LOSLASSEN aus -- in dem
        // Moment ist der Grip weg und apply_r_grip_aim setzt f.LT nicht mehr.
        // Deshalb waehrend des Wurf-Pulses Aim kuenstlich halten.
        if (motion_attack) {
            f.lt = 1.0f;
        }
    };

    // R.Grip -> Holster-Melee: Knife Ready (LB) | sonst Aim (LT)
    const auto apply_r_grip_aim = [&]() {
        if (holster_right_grip_counts_as_knife_ready()
            && !grenade_blocks_right_hand_knife_motion) {
            f.lb = true;
        } else if (knife_equipped) {
            // [KNIFE_THROW] Messer equippt: rechter Grip ist der WURF (weapons
            // liest ihn roh) -> hier KEIN f.LT, damit das Messer beim Grip
            // nicht aimt/zoomt.
        } else if (scope_grip_aim_blocked(digital(act_grip, rh))) {
            // [SCOPE_GRIP_DELAY] Erste Millisekunden nach der Druckflanke noch
            // kein Aim, damit das Holster sich melden kann.
        } else if (re4vr::lua_get_tribool("__vr_holster_grab_armed") == 1) {
            // [HOLSTER_GRAB] Aim NUR aus, wenn die Grip-Presse IN einer Zone
            // BEGANN (echter Greifvorgang). Mit gedruecktem Aim ueber ein
            // Holster HOVERN sperrt das Aim nicht mehr.
        } else if (const double u = re4vr::lua_get_number("__vr_post_stow_until", -1.0);
                   u > 0.0 && clock_now() < u) {
            // [POST_STOW] NUR kurz NACH dem Wegstecken kein Aim -> verhindert,
            // dass der Aim-Auto-Draw die Waffe sofort zurueckzieht.
        } else if (re4vr::lua_get_tribool("__vr_bare_hands") == 1) {
            // [BARE_HANDS] Leere Haende: KEIN Aim -> das Spiel wuerde sonst
            // automatisch die letzte Waffe ziehen.
        } else if (re4vr::lua_get_tribool("__vr_knife_in_hand") == 1) {
            // [MELEE_NO_AIM] NUR Messer: echter Eigenwurf -> Grip = Wurf, kein
            // Aim. GRANATE gehoert hier NICHT rein: ihr Wurf laeuft ueber den
            // NATIVEN Pfad, der das native Aim BRAUCHT.
        } else if (right_grip_maps_to_gamepad()) {
            f.lt = 1.0f;
        }
    };

    // --- Vor-Branch-Kombos ------------------------------------------------

    // [REF_OVERLAY] L.Trigger + L.B -> REFramework-VR-Menue togglen. Steht VOR
    // dem Branch-Split, gilt damit in ALLEN Branches.
    {
        // [MENUE-STEUERUNG 11.09.2026] ROH gelesen: bei offenem Menue liefern
        // l_trigger/l_bbutton nichts mehr (VR::is_menu_input_blocked), die Kombi
        // muss das Menue aber auch wieder schliessen. Die Flanke unten bleibt
        // dadurch auch beim Halten ueber das Schliessen hinweg richtig.
        const bool combo = digital_raw(act_dial, lh) && digital_raw(act_b, lh);

        if (combo && !m_lt_b_overlay_was) {
            // [REF_UI 2026-08-14] Dieselbe Kombi macht ZWEI Dinge: das Menue
            // selbst auf/zu, und das Palm-Overlay des Plugins folgt dem
            // Menuezustand (sonst versteckte es genau das Overlay, auf dem
            // unser Menue liegt).
            const bool want_ui = !g_framework->is_drawing_ui();
            g_framework->set_draw_ui(want_ui);

            m_prefs.hide_ref_overlay = !want_ui;
            save_prefs();
            overlay_set_enabled(!m_prefs.hide_ref_overlay);

            // [MENUE-SOUNDS 11.09.2026] Hier stand play_body_sound(REFUI_SOUND).
            // Den Auf-/Zu-Sound spielt jetzt REFramework selbst (MenuSound::Open,
            // CH_GUI_FILE_UNIQUE_01) -- fuer JEDEN Weg, auch Insert. Hier noch
            // einmal abgespielt, klaenge es doppelt.
        }

        m_lt_b_overlay_was = combo;

        if (combo) {
            l_bbutton = false;   // L.B nicht zusaetzlich als Y senden
        }
    }

    // [DUAL_TRIGGER_MENU] Beide Trigger halten -> START. Pre-Branch, feuert
    // einmal pro Hold (Flanke), Anwendung weiter unten.
    bool dual_trigger_start = false;
    {
        if (l_trigger && r_trigger) {
            m_dual_trigger_gap_clock.reset();   // wieder beide da -> Karenz verfaellt

            if (!m_dual_trigger_start_clock.has_value()) {
                m_dual_trigger_start_clock = clock_now();
            }

            if (!m_dual_trigger_fired
                && (clock_now() - *m_dual_trigger_start_clock) >= DUAL_TRIGGER_HOLD_SEC) {
                dual_trigger_start = true;
                m_dual_trigger_fired = true;
            }
        } else if (m_dual_trigger_start_clock.has_value()) {
            // [COMBO_GRACE] Kurzes Flackern an der Trigger-Schwelle ist KEIN
            // Loslassen. `fired` wird bewusst erst zusammen mit der Uhr
            // geloescht, sonst feuert es waehrend der Karenz jeden Frame neu.
            if (!m_dual_trigger_gap_clock.has_value()) {
                m_dual_trigger_gap_clock = clock_now();
            }

            if ((clock_now() - *m_dual_trigger_gap_clock) >= COMBO_GRACE_SEC) {
                m_dual_trigger_start_clock.reset();
                m_dual_trigger_fired = false;
                m_dual_trigger_gap_clock.reset();
            }
        } else {
            m_dual_trigger_fired = false;
            m_dual_trigger_gap_clock.reset();
        }
    }

    // --- Branch-Zustaende -------------------------------------------------

    auto* ks = RE4VRKillswitch::get().get();
    const bool ks_active = ks->is_active();
    const bool in_throwsight = is_throwsight_stage();
    const bool in_boat = is_boat_stage();
    const bool in_menu = is_any_menu_open();

    // [ADA_A MENUE-UEBERNAHME 2026-07-20, per Log bewiesen] Adas rechtes A
    // reicht das A erst beim LOSLASSEN nach. Bestaetigt man im Menue mit A,
    // laeuft beim Schliessen sofort der Gameplay-Zweig -- der sah den noch
    // gedrueckten Knopf als FRISCHE Presse und schickte ein A hinterher: der
    // Haendler ging sofort wieder auf.
    if (in_menu) {
        m_ada_ra.prev = r_abutton;
        m_ada_ra.down_t.reset();
        m_ada_ra.fired_rb = false;
        m_ada_ra.a_timer = 0;
    }

    const bool in_binoculars = is_binoculars_active_this_frame();
    const bool in_turret = is_turret_mounted();
    // [JETSKI] KS4-Flag aus dem Killswitch (Stages 592xx)
    const bool in_jetski = re4vr::lua_get_tribool("__re4_jetski_active") == 1;
    // [BOAT] KS4-Flag: die FAHRT (VehicleCam), NICHT der Einstieg (ActionCam)
    const bool boat_active = re4vr::lua_get_tribool("__re4_boat_active") == 1;

    // [BACK_GUARD] Verlaesst man ein Menue mit noch gehaltenem Links-A, darf das
    // im Gameplay/KS NICHT sofort als BACK (= Map) durchschlagen.
    // [TURRET] GENAU DIESELBE Falle beim Absteigen von der MG-Turret.
    if ((in_menu || in_turret) && l_abutton) {
        m_la.back_guard = true;
    }

    // [KS_X_GUARD 2026-08-18] Dasselbe Muster fuer den rechten B (= Gamepad-X =
    // nativer Reload), aber fuers KS-ENDE: waehrend der Uebergabe ist die
    // Engine typischerweise frueher "Gameplay" als unser ks_active, und ein
    // noch gehaltenes X feuert dort den nativen Reload.
    // Gemerkt wird am ROHEN Knopf: r_bbutton ist bei verwalteten Waffen schon
    // konsumiert, der Latch kaeme sonst nie zustande.
    if (ks_active && re4vr::lua_get_tribool("__vr_raw_r_bbutton") == 1) {
        m_la.ks_x_guard = true;
    }

    // [X_MUTE_GATE] Nur in reinem Gameplay darf der bare-hands/Messer-X-Mute
    // greifen. In Menu/Boot/Throwsight/KS/Binoculars ist X eine legitime
    // Funktion (Inventar-Objekte bewegen) und darf NICHT stillgelegt werden.
    // [PWPAUSE-STAGE 15.09.2026] In 47400/47410 -- den Stages mit dem
    // PlayWorkGimmick-Pausehaenger -- meldet GuiManager.get_IsHudOff DAUERHAFT
    // true, auch nachdem der Notaus die Pause geloest hat. is_any_menu_open()
    // wertet das als Menue, damit steht frame_is_gameplay dort permanent auf
    // false -- und daran haengt der halbe Mod: capture_mag_rest misst nie eine
    // Ruhelage (Magazin laesst sich nicht mehr einsetzen und verschwindet beim
    // Andocken), der native Reload-Knopf wird nicht mehr stillgelegt, und der
    // EquipLock haelt das Schiessen fest. Gemessen in re4_gameplaygate.txt:
    //   Stage=47400 | paused=false hudOff=true ... | MENUE wegen: IsHudOff
    //
    // Zurueckschalten laesst sich das HUD nicht: GuiManager hat nur
    // get_IsHudOff, keinen Setter und kein Feld dazu (re4_hudoff.txt). Ein
    // Force aus Lua kommt zu spaet, weil dieses Global hier jeden Frame neu
    // geschrieben wird.
    //
    // Deshalb genau hier und NUR fuer diese zwei Stages: hudOff nicht als
    // Menue werten. Die fuenf anderen Riegel (Pause, PauseMenuLock, Koffer,
    // Schreibmaschine, Karte) bleiben auch dort scharf -- greift einer davon,
    // liefert is_any_menu_open(true) weiterhin true und es bleibt beim Menue.
    // [NACHTRAG 15.09.2026] Es ist nicht nur HudOff. Nach Typewriter oder
    // Haendler bleibt in denselben Stages auch
    // GuiManager.hasOccupiedPauseMenuSystemLock stehen (gemessen
    // re4_gameplaygate.txt 11:54-11:56: ueber eine Minute true, obwohl HUD
    // zurueck, Fenster zu, nicht pausiert). Freigeben geht nicht: der Setter
    // set_hasOccupiedPauseMenuSystemLock(false) existiert und laeuft durch,
    // das Spiel setzt den Lock aber im naechsten Frame sofort wieder
    // (re4_guilock.txt 12:03). Also auch dieser Riegel hier ignoriert.
    //
    // Was in diesen Stages WEITER zaehlt, wird darum einzeln geprueft statt
    // ueber is_any_menu_open(true): echte Pause, Koffer, Schreibmaschine,
    // Karte, Kapitelabschluss und Leseansicht. Nur wenn nichts davon offen
    // ist, gilt das Menue als reiner Haenger.
    bool menu_for_gp = in_menu;

    // [PERF 15.09.2026] Dieser Block lief JEDEN Frame, sobald irgendein
    // Menue-Flag stand -- und in der kaputten Stage haengt hudOff dauerhaft,
    // also Stage-Abfrage plus sechs Singleton-Calls pro Frame. Das hat die
    // CPU-Frametime sichtbar gekostet ("war seit dem C++-Port erledigt, jetzt
    // wieder schlecht"). Die Antwort aendert sich nicht im Millisekundentakt:
    // sie wird jetzt 5x pro Sekunde neu ermittelt und dazwischen gemerkt.
    static double s_menu_next = 0.0;
    static bool s_menu_haenger = false;

    // Die Stage wechselt alle paar Minuten -- sie deshalb nur EINMAL PRO
    // SEKUNDE lesen und merken. Ausserhalb der beiden betroffenen Stages
    // bleibt damit pro Frame ein einziger Zahlenvergleich uebrig; das war die
    // Beschwerde ("unverhaeltnismaessig fuer 2 Stages im ganzen Spiel").
    static double s_stage_next = 0.0;
    static int32_t s_stage = -1;

    if (clock_now() >= s_stage_next) {
        s_stage_next = clock_now() + 1.0;
        s_stage = -1;

        if (auto* ctx = re4vr::fc::ctx(); ctx != nullptr) {
            int32_t st = 0;

            if (re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", st)) {
                s_stage = st;
            }
        }
    }

    const bool pw_stage = (s_stage == 47400 || s_stage == 47410);

    if (in_menu && pw_stage && clock_now() < s_menu_next) {
        if (s_menu_haenger) {
            menu_for_gp = false;
        }
    } else if (in_menu && pw_stage) {
        s_menu_next = clock_now() + 0.20;
        s_menu_haenger = false;

        {
            {
                bool echtes_menue = false;

                if (auto* pm = re4vr::fc::managed_singleton("share.PauseManager");
                    pm != nullptr) {
                    echtes_menue = echtes_menue || call_bool(pm, "isPaused()");
                }

                if (auto* acm = re4vr::fc::managed_singleton("chainsaw.AttacheCaseManager");
                    acm != nullptr) {
                    echtes_menue = echtes_menue || call_bool(acm, "get_IsAttacheCaseBusy");
                }

                if (auto* am = re4vr::fc::managed_singleton("chainsaw.ArmouryManager");
                    am != nullptr) {
                    echtes_menue = echtes_menue || call_bool(am, "get_IsTypewriterWindow");
                }

                if (auto* mm = re4vr::fc::managed_singleton("chainsaw.MapManager");
                    mm != nullptr) {
                    echtes_menue = echtes_menue || call_bool(mm, "isMapGuiOpen");
                }

                echtes_menue = echtes_menue || is_chapter_result_gui_open()
                    || is_file_reader_gui_open();

                if (!echtes_menue) {
                    menu_for_gp = false;
                    s_menu_haenger = true;
                }
            }
        }
    }

    const bool frame_is_gameplay =
        !menu_for_gp && !in_boat && !in_throwsight && !in_binoculars && !ks_active;
    re4vr::lua_set_bool("__re4_frame_is_gameplay", frame_is_gameplay);

    // [MAG_B_GUARD 2026-09-10 -- Testerbefund] Der Mag-Auswurf per rechtem B
    // gehoert AUSSCHLIESSLICH ins reine Gameplay. Der rohe B (Z.1733) wird
    // bedingungslos publiziert und die Reload-Module lesen KEINEN Killswitch --
    // ohne dieses Flag fiele das Magazin in Karte, Inventar und Cutscenes.
    //
    // Bewusst EINZELN abgefragt statt is_any_menu_open()/frame_is_gameplay:
    // beide ziehen "HUD aus" mit herein, und HUD-aus ist KEIN Menue -- das
    // Spiel blendet es auch mitten im Gameplay aus. Killswitch-Zustaende
    // (Gondel, Boot, Jetski, Minecart, Aufzug) sind bewusst NICHT dabei: dort
    // wird gekaempft, das Nachladen muss gehen.
    //
    // Reihenfolge = billigste zuerst, jede weitere nur bei Bedarf.
    {
        bool mag_block = false;

        if (auto* mm = re4vr::fc::managed_singleton("chainsaw.MapManager"); mm != nullptr) {
            mag_block = call_bool(mm, "isMapGuiOpen");
        }

        if (!mag_block) {
            if (auto* am = re4vr::fc::managed_singleton("chainsaw.ArmouryManager"); am != nullptr) {
                mag_block = call_bool(am, "get_IsTypewriterWindow");
            }
        }

        if (!mag_block) {
            if (auto* acm = re4vr::fc::managed_singleton("chainsaw.AttacheCaseManager");
                acm != nullptr) {
                mag_block = call_bool(acm, "get_IsAttacheCaseBusy");
            }
        }

        if (!mag_block) {
            if (auto* pm = re4vr::fc::managed_singleton("share.PauseManager"); pm != nullptr) {
                mag_block = call_bool(pm, "isPaused()");
            }
        }

        // [ECHTE CUTSCENE] Die Event-Kamera hat uebernommen.
        if (!mag_block) {
            if (auto* csys = re4vr::fc::managed_singleton("chainsaw.CameraSystem");
                csys != nullptr) {
                mag_block = call_bool(csys, "get_IsEventCamera");
            }
        }

        // [INGAME-CUTSCENE] get_IsPlayingEvent ROH, also OHNE den
        // PlayerCamera-Filter aus RE4VRKillswitch::is_real_cutscene(). Genau
        // dieser Filter laesst die Ingame-Cutscenes durch: dort bleibt die
        // Spielerkamera aktiv und Leon hat die Waffe in der Hand -- der Fall,
        // in dem das Magazin bisher unbemerkt fiel.
        if (!mag_block) {
            if (auto* gm = re4vr::fc::managed_singleton("chainsaw.GuiManager"); gm != nullptr) {
                mag_block = call_bool(gm, "get_IsPlayingEvent");
            }
        }

        // [KAPITEL-ERGEBNIS] ChapterEnd/ChapterDetailResult/ChapterStats/
        // GameClearResult -- setzt KEINES der Standard-Menue-Signale, deshalb
        // die eigene GuiType-Abfrage (steht oben schon fertig da).
        if (!mag_block) {
            mag_block = is_chapter_result_gui_open();
        }

        // [DOKUMENT-LESER] FileDetail/FileSelect: Zettel in der Welt UND die
        // Datei-Ansicht aus dem Inventar. Auch kein Gameplay.
        if (!mag_block) {
            mag_block = is_file_reader_gui_open();
        }

        re4vr::lua_set_bool("__re4_mag_block", mag_block);
    }

    // [WARUM-EXPORT] NUR Diagnose, kein Verhalten: welcher der fuenf Zustaende
    // das Flag gerade auf false zieht.
    re4vr::lua_set_string("__re4_gameplay_why",
                          frame_is_gameplay  ? "gameplay"
                              : in_menu      ? "menu"
                              : in_boat      ? "boat"
                              : in_throwsight ? "throwsight"
                              : in_binoculars ? "bino"
                              : ks_active    ? "killswitch"
                                             : "?");

    // [PURE_GAMEPLAY_EXPORT] Wie oben, aber zusaetzlich ohne Turret und Jetski
    // (die haben eigene Branches, fehlen oben aber, weil der X-Mute sie bewusst
    // mitnimmt). Liest der Wild-West-Twirl fuer sein RT-Halte-Gate.
    const bool frame_pure_gameplay = frame_is_gameplay && !in_turret && !in_jetski;
    re4vr::lua_set_bool("__re4_frame_pure_gameplay", frame_pure_gameplay);

    // [GESTURE_SHIFT 2026-07-21] LT + R.A -> "point", LT + R.B -> "fuck_you",
    // AUSSCHLIESSLICH mit leeren Haenden und in reinem Gameplay.
    {
        // Kriterium ist die RECHTE Hand, nicht "gar nichts in der Hand": gar
        // keine Waffe zaehlt, UND das Messer in der LINKEN Hand zaehlt auch.
        bool right_free = re4vr::lua_get_tribool("__vr_bare_hands") == 1;

        if (!right_free && knife_equipped && knife_hand == "left") {
            right_free = true;
        }

        if (right_free && knife_hand == "right") {
            right_free = false;
        }

        // ACHTUNG, teuer bezahlt am 04.08.: hier stand zusaetzlich ein
        // Body-GO-Namens-Lookup pro Frame. Schlaegt der in EINEM Frame fehl,
        // faellt der Gesten-Mute weg -> das A geht als Xbox-A durch (Waffe wird
        // gezogen) und das R.B schlaegt als X durch (Magazin fliegt raus).
        // In einem Eingabepfad hat ein Live-Lookup nichts verloren.
        const char* g = nullptr;

        // Dieselben Umstaende fuer beide Ausloeser: reines Gameplay, freie
        // rechte Hand, kein Mercs, keine Stillzone.
        const bool gest_erlaubt = frame_pure_gameplay && right_free
            && !is_mercs_active()
            && re4vr::lua_get_tribool("__re4_stillzone_hide_now") != 1;

        if (l_trigger && gest_erlaubt) {
            if (r_abutton) {
                g = "point";
            } else if (r_bbutton) {
                g = "fuck_you";
            }
        }

        // [TRACKPAD-GESTEN 15.09.2026 -- Ansage des Users] Auf den Valve
        // Knuckles zusaetzlich OHNE LT: Trackpad-Klick LINKS -> Zeigefinger,
        // RECHTS -> Stinkefinger. Auf Controllern ohne Trackpad (Quest) ist das
        // Action-Handle ungueltig, digital() liefert dort immer false -- fuer
        // sie aendert sich also NICHTS.
        if (g == nullptr && gest_erlaubt) {
            // [TRACKPAD-PRESS 15.09.2026] Nicht mehr die boolesche Aktion
            // direkt: unter OpenXR hat der Index keinen trackpad/click, dort
            // zaehlt die KRAFT. is_touchpad_pressed deckt beide Runtimes ab.
            if (vr->is_touchpad_pressed(VRRuntime::Hand::LEFT)) {
                g = "point";
            } else if (vr->is_touchpad_pressed(VRRuntime::Hand::RIGHT)) {
                g = "fuck_you";
            }
        }

        re4vr::lua_set_bool("__re4_gest_mute", g != nullptr);

        const std::string prev = re4vr::lua_get_string("__re4_gest_prev");

        if (g != nullptr && prev != g) {
            re4vr::lua_set_string("__re4_gesture_fire", g);
        }

        if (g != nullptr) {
            re4vr::lua_set_string("__re4_gest_prev", g);
        } else {
            re4vr::lua_set_nil("__re4_gest_prev");
        }
    }

    // [EASTER_EGG_FLAMETHROWER] Beide A-Buttons 5 s im Gameplay.
    const bool both_a = l_abutton && r_abutton;
    {
        // [NUR LEON 2026-08-11] Das Easteregg legt den Flamethrower in die
        // Aufbewahrung der Kampagne -- es gehoert also zu Leon.
        const bool in_gameplay = frame_is_gameplay && !is_ada_active() && !is_mercs_active();

        if (both_a && in_gameplay) {
            m_ee_flame_gap_clock.reset();

            if (!m_ee_flame_clock.has_value()) {
                m_ee_flame_clock = clock_now();
            }

            if (!m_ee_flame_fired && (clock_now() - *m_ee_flame_clock) >= EE_FLAME_HOLD_SEC) {
                m_ee_flame_fired = true;
                grant_flamethrower_to_armoury();
            }
        } else if (m_ee_flame_clock.has_value()) {
            // [COMBO_GRACE] Ein einzelner Frame ohne einen der beiden A-Buttons
            // hat die vollen 5 s zurueckgeworfen. Kurze Aussetzer zaehlen nicht.
            if (!m_ee_flame_gap_clock.has_value()) {
                m_ee_flame_gap_clock = clock_now();
            }

            if ((clock_now() - *m_ee_flame_gap_clock) >= COMBO_GRACE_SEC) {
                m_ee_flame_clock.reset();
                m_ee_flame_fired = false;
                m_ee_flame_gap_clock.reset();
            }
        } else {
            m_ee_flame_fired = false;
            m_ee_flame_gap_clock.reset();
        }
    }

    // ------------------------------------------------------------------
    // DIE BRANCH-KETTE
    // ------------------------------------------------------------------
    bool ada_mode = false;

    if (in_boat && !in_menu) {
        if (l_joyclick) {
            f.ls = true;
        }

        if (l_trigger) {
            f.lt = 1.0f;
        }

        // L.Grip -> LB DIREKT wie im alten Mod (roher Grip, kein Melee-Gate)
        // + Holster beim Loslassen.
        const bool l_grip_boot = digital(act_grip, lh);

        if (l_grip_boot) {
            f.lb = true;
        }

        if (m_prev_l_grip && !l_grip_boot) {
            re4vr::lua_set_bool("vr_holster_knife", true);
        }

        m_prev_l_grip = l_grip_boot;

        if (l_abutton) {
            f.back = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (edge_detect(m_edge_r_jc, r_joyclick)) {
            f.b = true;
        }

        // R.Trigger -> RT DIREKT: die native Boot-Steuerung braucht das
        // Quick-Knife-/Aim-Gating aus apply_rt_attack nicht.
        if (r_trigger) {
            f.rt = 1.0f;
        }

        if (digital(act_grip, rh)) {
            f.lb = true;
        }

        if (edge_detect(m_edge_r_a, r_abutton)) {
            f.a = true;
        }

        if (edge_detect(m_edge_r_b, r_bbutton)) {
            f.x = true;
        }
    } else if (in_throwsight && !in_menu) {
        // [THROWSIGHT] `&& !in_menu`: bei offenem Menue greift der Menue-Zweig.
        // Linker Stick bleibt AN (nativ) -> Ausweichen im Boot moeglich.
        if (l_trigger) {
            f.lb = true;
        }

        // [ERST LOSLASSEN] Den ROHEN Grip veroeffentlichen: das Latch darf sich
        // nur loesen, wenn der Finger wirklich offen ist. An f.LT gemessen
        // verfiel es zu frueh (in der Holster-Zone wird der Grip zeitweise gar
        // nicht auf LT gemappt).
        const bool lgrip = digital(act_grip, lh);
        re4vr::lua_set_bool("__vr_raw_l_grip", lgrip);

        if (lgrip) {
            f.lt = 1.0f;
        }

        if (edge_detect(m_edge_l_a_b, l_abutton)) {
            f.b = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_trigger) {
            f.lb = true;
        }

        // R.Grip -> LB (wie R.Trigger -- beide werfen die Harpune). ROHER Grip:
        // bare_hands/Holster/block_shoot_ready sind hier sinnlos und blockten
        // den Wurf.
        if (digital(act_grip, rh)) {
            f.lb = true;
        }

        if (edge_detect(m_edge_r_a, r_abutton)) {
            f.a = true;
        }

        if (edge_detect(m_edge_r_b, r_bbutton)) {
            f.x = true;
        }
    } else if (in_menu) {
        // [DODGE_PROMPT bei HUD-AUS 2026-07-17] Der Ausweich-Prompt landet hier
        // statt im Gameplay-Zweig: das Spiel blendet fuer die Sequenz das HUD
        // aus ("force look"), und get_IsHudOff zaehlt als Menue. Kein echtes
        // Menue also -- is_any_menu_open(true) fragt "ist ein ECHTES Menue
        // offen?" -> nur wenn NEIN, gehoert der Grip dem Ausweichen.
        // Pause/Koffer/Map/Typewriter bleiben unberuehrt.
        bool dodge_hudoff = false;

        if (is_rect_prompt_now() && l_grip_from_left && !is_any_menu_open(true)) {
            dodge_hudoff = true;
            f.rb = true;
        } else if (is_leon_lb_prompt_now() && l_grip_from_left && !is_any_menu_open(true)) {
            dodge_hudoff = true;
            f.lb = true;
        } else if (re4vr::lua_call_global_bool("__re4_is_dodge_prompt", false)
                   && l_grip_from_left && !is_any_menu_open(true)) {
            dodge_hudoff = true;
            f.b = true;
        }

        // [SYMBOL_RIDDLE] Rechter Stick links -> LB, rechts -> RB. DAUERDRUCK,
        // solange gekippt (kein Edge; so gewollt: Symbole wechseln).
        const bool symbol_riddle = is_symbol_riddle();

        if (symbol_riddle) {
            if (right_stick.x <= -0.5f) {
                f.lb = true;
            } else if (right_stick.x >= 0.5f) {
                f.rb = true;
            }
        }

        if (l_joyclick) {
            f.ls = true;
        }

        if (l_trigger) {
            f.lt = 1.0f;

            // [SYMBOL_RIDDLE] Im Raetsel gehoert der R-Stick den Bumpern ->
            // hier KEIN DPAD (sonst doppelt).
            if (!symbol_riddle) {
                if (right_stick.y >= 0.9f) {
                    f.dpad_up = true;
                } else if (right_stick.y <= -0.9f) {
                    f.dpad_down = true;
                }

                if (right_stick.x >= 0.9f) {
                    f.dpad_right = true;
                } else if (right_stick.x <= -0.9f) {
                    f.dpad_left = true;
                }
            }

            // R-Stick waehrend des Shifts nicht ans Pad (sonst Doppel-Nav)
            f.rx = 0.0f;
            f.ry = 0.0f;
        }

        // [DODGE] Im Ausweich-Fenster gehoert der Grip dem B oben -> kein LB.
        if (digital(act_grip, lh) && !dodge_hudoff) {
            f.lb = true;
        }

        // [LONG_LATCH] edge_detect immer aufrufen (Zustand frisch halten), aber
        // B unterdruecken, solange der Longpress-Latch haelt (A wurde beim
        // Map-Oeffnen noch gehalten) -> kein Sofort-Cancel.
        if (edge_detect(m_edge_l_a_b, l_abutton) && !m_la.long_consumed) {
            f.b = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.rs = true;
        }

        if (r_trigger) {
            f.rt = 1.0f;
        }

        // R.Grip -> RB. ROHER Grip wie L.Grip->LB: die Gameplay-Gates
        // (bare_hands/holster/block_shoot_ready) blockten RB faelschlich im
        // Inventar.
        if (digital(act_grip, rh)) {
            f.rb = true;
        }

        if (r_abutton) {
            f.a = true;
        }

        if (edge_detect(m_edge_r_b, r_bbutton)) {
            f.x = true;
        }
    } else if (in_turret) {
        // [TURRET] Kopie der Gameplay-Bindings mit DREI Ausnahmen:
        // (1) rechter Stick FREI in X UND Y, (2) rechter Trigger RAW,
        // (3) Links-A = Xbox-B (Feuer) statt Map/Longpress.
        // Steht VOR ks_active -> uebersteuert die KS1, in der die Turret sonst
        // haengt. Steht NACH in_menu.
        f.rx = right_stick.x;
        f.ry = right_stick.y;

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        if (l_abutton) {
            f.b = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        if (r_trigger) {
            f.rt = 1.0f;
        }

        apply_r_grip_aim();

        if (r_abutton) {
            f.a = true;
        }

        if (r_bbutton) {
            f.x = true;
        }
    } else if (in_jetski) {
        // [JETSKI] Steht VOR ks_active -> Jetski IST KS4 und wuerde sonst vom
        // KS-Zweig geschluckt. BEIDE Sticks wirken als normaler LINKER Stick;
        // pro Achse gewinnt der staerker ausgelenkte. Die rechten Kamera-Achsen
        // sind tot (Blick kommt vom HMD).
        const float jx_l = left_stick.x;
        const float jy_l = left_stick.y;
        const float jx_r = right_stick.x;
        const float jy_r = right_stick.y;

        f.lx = (std::fabs(jx_r) > std::fabs(jx_l)) ? jx_r : jx_l;
        f.ly = (std::fabs(jy_r) > std::fabs(jy_l)) ? jy_r : jy_l;
        f.rx = 0.0f;
        f.ry = 0.0f;

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        if (l_abutton) {
            f.back = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        if (r_trigger) {
            f.rt = 1.0f;
        }

        apply_r_grip_aim();

        if (r_abutton) {
            f.a = true;
        }

        if (r_bbutton) {
            f.x = true;
        }
    } else if (ks_active) {
        // KILLSWITCH: wie Gameplay, aber ohne Longpress/Flanken.
        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        // [DODGE_GIMMICK / RECT_PROMPT] Rein ADDITIV. Der Zustands-Check laeuft
        // nur bei gedruecktem Grip (Kurzschluss). Rect hat Vorrang, damit nie
        // beide Knoepfe gleichzeitig feuern.
        if (l_grip_from_left) {
            if (is_rect_prompt_now()) {
                f.rb = true;
            } else if (is_leon_lb_prompt_now()) {
                f.lb = true;
            } else if (is_dodge_gimmick_now()) {
                f.b = true;
            }
        }

        if (l_abutton) {
            f.back = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        apply_rt_attack();
        apply_r_grip_aim();

        if (r_abutton) {
            f.a = true;
        }

        // [KS_X_FLANKE 2026-08-18] Frueher PEGEL -- damit ging X in JEDEM
        // KS-Frame neu raus. Beim Uebergang Cutscene -> Gameplay ist die Engine
        // im Zweifel frueher im Gameplay als unser ks_active, und ein gehaltenes
        // X wird dort zum NATIVEN RELOAD. Jetzt ueber edge_detect.
        if (edge_detect(m_edge_r_b, r_bbutton)) {
            f.x = true;
        }
    } else if (in_binoculars) {
        // BINOCULARS: Gameplay-Bindings, aber L.Stick = Zoom (genullt),
        // R.Stick x2.0 Empfindlichkeit.
        f.lx = 0.0f;
        f.ly = 0.0f;
        f.rx = right_stick.x * 2.0f;
        f.ry = right_stick.y * 2.0f;

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        if (l_abutton) {
            f.back = true;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        apply_rt_attack();
        apply_r_grip_aim();

        if (r_abutton) {
            f.a = true;
        }

        if (r_bbutton) {
            f.x = true;
        }
    } else if (is_mercs_active()) {
        // GAMEPLAY (MERCENARIES). Steht bewusst VOR dem Ada-Zweig: in Mercs ist
        // Ada ebenfalls spielbar -- stuende Ada zuerst, landete sie im
        // Kampagnen-Zweig von Separate Ways.
        if (m_bino.active) {
            m_bino.active = false;
            m_bino.offset = 0.0f;
        }

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        // [MERC_LA_STICKS] In Mercenaries hat Links-A WEDER Short- NOCH
        // Longpress. Stattdessen loest die Taste BEIDE Stick-Klicks aus.
        // la_hold wird leergehalten: die Timer-Zweige hinter dem Branch-Split
        // lesen es weiter, ein stehengebliebener Zaehler wuerde spaeter ein
        // Phantom-BACK oder -RS ausloesen.
        if (l_abutton && !both_a) {
            f.rs = true;
            f.ls = true;
        }

        m_la.pressed = false;
        m_la.frames = 0;
        m_la.fired_long = false;

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        apply_prompt_grip(f, l_grip_from_left);
        apply_rt_attack();
        apply_r_grip_aim();

        if (r_abutton && !both_a) {
            f.a = true;
        }

        // [X_TOT_IM_GAMEPLAY] R.B -> X bleibt AUS wie bei Leon und Ada.
        // [BOW_NATIVE_X 2026-08-05] EINE Ausnahme: der Compound Bow (6304) hat
        // keinen manuellen Ladeweg und braucht das Xbox-X.
        // edge_detect wird IMMER gerufen (auch wenn die Waffe nicht passt),
        // sonst bleibt prev auf true haengen und die naechste Flanke faellt aus.
        const bool r_b_edge = edge_detect(m_edge_r_b, r_bbutton);

        if (r_b_edge && !both_a && re4vr::lua_get_number("__vr_dbg_wep_id", -1.0) == 6304.0
            && frame_pure_gameplay && !knife_equipped) {
            f.x = true;
        }
    } else if (is_ada_active()) {
        // GAMEPLAY (ADA / SEPARATE WAYS)
        if (m_bino.active) {
            m_bino.active = false;
            m_bino.offset = 0.0f;
        }

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        // [ADA_LA 2026-07-21] Ada braucht keinen Ashley-Befehl -> ihr linkes A
        // ist DIREKT Xbox-BACK, ohne Longpress und ohne Shortpress-Timer.
        if (l_abutton && !both_a) {
            f.back = true;
        }

        m_la.pressed = false;
        m_la.frames = 0;
        m_la.fired_long = false;

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        apply_prompt_grip(f, l_grip_from_left);
        apply_rt_attack();
        apply_r_grip_aim();

        // [ADA_A 2026-07-20] Rechtes A: KURZ = Xbox A, LANG = RB (Greifhaken).
        // Da beides auf DEMSELBEN Knopf liegt, kann A erst beim Loslassen
        // entschieden werden -- es wird als kurzer Impuls nachgereicht.
        //  1) IMPULS KURZ (4 Frames): ein langer Impuls wirkt wie "gehalten",
        //     und das Spiel macht daraus zwei Interaktionen.
        //  2) COOLDOWN: nach einem A fuer 0.25 s kein weiteres.
        //  3) ZUSTAND IMMER PFLEGEN -- auch im Menue-Block oben.
        {
            const double now = clock_now();
            const double hold = re4vr::lua_get_number("__re4_ada_a_long_sec", 0.35);

            // [ADA_A_RAW_ZONE] In der Zone A 1:1 durchreichen (das Spiel
            // verlangt dort langes A). Zustand trotzdem pflegen.
            if (is_ada_raw_a_zone()) {
                if (r_abutton && !both_a) {
                    f.a = true;
                }

                m_ada_ra.prev = r_abutton;
                m_ada_ra.down_t.reset();
                m_ada_ra.fired_rb = false;
                m_ada_ra.a_timer = 0;
                m_ada_ra.rb_timer = 0;
                m_ada_ra.a_until = 0.0;
                m_ada_ra.rb_until = 0.0;
            } else if (r_abutton && !m_ada_ra.prev) {
                m_ada_ra.down_t = now;
                m_ada_ra.fired_rb = false;
            } else if (r_abutton && m_ada_ra.down_t.has_value() && !m_ada_ra.fired_rb
                       && (now - *m_ada_ra.down_t) >= hold) {
                m_ada_ra.fired_rb = true;
                m_ada_ra.rb_timer = 20;      // Longpress -> RB, KEIN A
                m_ada_ra.rb_until = now + 0.22;
            } else if (!r_abutton && m_ada_ra.prev) {
                // Loslassen: kurz genug UND der letzte Impuls lange genug her?
                if (!m_ada_ra.fired_rb && (now - m_ada_ra.last_a) > 0.25) {
                    m_ada_ra.a_timer = 4;
                    m_ada_ra.a_until = now + 0.10;
                    m_ada_ra.last_a = now;
                }

                m_ada_ra.down_t.reset();
                m_ada_ra.fired_rb = false;
            }

            m_ada_ra.prev = r_abutton;

            // [ADA_A_IMPULS_DEADLINE, per Dump BEWIESEN] Die Impuls-Timer sind
            // FRAME-Zaehler und ticken nur, wenn dieser Zweig laeuft. Waehrend
            // des Killswitchs fror der RB-Impuls ein und wurde SEKUNDEN spaeter
            // weitergereicht -- der Haken feuerte ein zweites Mal.
            if (m_ada_ra.a_timer > 0 && now > m_ada_ra.a_until) {
                m_ada_ra.a_timer = 0;
            }

            if (m_ada_ra.rb_timer > 0 && now > m_ada_ra.rb_until) {
                m_ada_ra.rb_timer = 0;
            }

            if (m_ada_ra.a_timer > 0 && !both_a) {
                f.a = true;
                --m_ada_ra.a_timer;
            }

            if (m_ada_ra.rb_timer > 0) {
                f.rb = true;
                --m_ada_ra.rb_timer;
            }
        }

        // [X_TOT_IM_GAMEPLAY] R.B -> X hier ENTFERNT: im Gameplay hat X
        // ausschliesslich von UNS vergebene Aufgaben.
    } else {
        // GAMEPLAY (LEON)
        if (m_bino.active) {
            m_bino.active = false;
            m_bino.offset = 0.0f;
        }

        if (l_joyclick) {
            f.ls = true;
        }

        apply_dpad_shift();
        apply_l_grip_knife();

        // [1:1 BASIS 2026-07-20] Ada laeuft BEWUSST exakt wie Leon:
        // Short = CMD Ashley (RS), Long = Map (BACK). Der frueher hier aktive
        // Ada-Sonderfall ist AUS -- deshalb steht ada_mode fest auf false.
        ada_mode = false;

        // [LA_TIMING] Schwellen aus dem UI (Sekunden -> Frames @ ~90 fps).
        int32_t long_frames = static_cast<int32_t>(m_prefs.long_press_sec * 90.0f + 0.5f);

        if (long_frames < 3) {
            long_frames = 3;
        }

        const int32_t short_min_frames =
            static_cast<int32_t>(m_prefs.short_min_sec * 90.0f + 0.5f);

        if (both_a) {
            // Easter-Egg-Geste laeuft: L.A-Longpress/Short unterdruecken
            m_la.pressed = false;
            m_la.frames = 0;
            m_la.fired_long = false;
        } else if (l_abutton) {
            if (!m_la.pressed) {
                m_la.pressed = true;
                m_la.frames = 0;
                m_la.fired_long = false;
            }

            ++m_la.frames;

            if (m_la.frames >= long_frames && !m_la.fired_long) {
                // 20-Frame-Halte-Timer, damit BACK sicher registriert wird
                m_la.long_timer = 20;
                m_la.long_is_ada = ada_mode;
                m_la.fired_long = true;
                // [LONG_LATCH] A ist beim Ausloesen NOCH gehalten. Sobald BACK
                // die Map oeffnet, wird in_menu true -> der Menue-Zweig mappt
                // das gehaltene Links-A auf B (= Cancel) und schliesst die Map
                // sofort.
                m_la.long_consumed = true;
            }
        } else {
            // Short nur, wenn der Tap lang genug war und kein Long feuerte.
            if (m_la.pressed && !m_la.fired_long && m_la.frames >= short_min_frames) {
                m_la.short_timer = 20;
                m_la.short_is_ada = ada_mode;
            }

            m_la.pressed = false;
            m_la.frames = 0;
            m_la.fired_long = false;
        }

        if (l_bbutton) {
            f.y = true;
        }

        if (handle_pause) {
            f.start = true;
        }

        if (r_joyclick) {
            f.b = true;
        }

        apply_prompt_grip(f, l_grip_from_left);
        apply_rt_attack();
        apply_r_grip_aim();

        if (r_abutton && !both_a) {
            f.a = true;
        }

        // [X_TOT_IM_GAMEPLAY] R.B -> X hier ENTFERNT (siehe Ada-Zweig).
    }

    // ------------------------------------------------------------------
    // POST-BRANCH -- diese Bloecke haben das letzte Wort
    // ------------------------------------------------------------------

    // [DUAL_TRIGGER_MENU] In ALLEN Branches AUSSER Menue (dort ist man schon
    // drin -> START wuerde nur wieder schliessen).
    if (dual_trigger_start) {
        m_start_hold_timer = START_HOLD_FRAMES;
    }

    if (m_start_hold_timer > 0) {
        if (!in_menu) {
            f.start = true;
        }

        --m_start_hold_timer;
    }

    // L.AButton Short-Press-Timer (laeuft ausserhalb der Branches)
    if (m_la.short_timer > 0) {
        if (m_la.short_is_ada) {
            f.back = true;   // Ada short = Map
        } else {
            f.rs = true;     // Leon short = CMD Ashley
        }

        --m_la.short_timer;
    }

    // L.AButton Long-Press-Timer (haelt die Taste 20 Frames)
    if (m_la.long_timer > 0) {
        if (m_la.long_is_ada) {
            f.rb = true;     // Ada long = RB/Grapple
        } else {
            f.back = true;   // Leon long = Map
        }

        --m_la.long_timer;
    }

    // [BACK_GUARD] Solange das aus dem Menue gehaltene Links-A nicht
    // losgelassen wurde: JEDES BACK schlucken.
    if (m_la.back_guard) {
        f.back = false;
    }

    // [KS_X_GUARD] Ein X-Druck, der im Killswitch begonnen hat, wird ab dem
    // Moment geschluckt, in dem der KS vorbei ist -- im KS selbst soll X
    // weiter wirken.
    if (m_la.ks_x_guard && !ks_active) {
        f.x = false;
    }

    // [LONG_LATCH] Latch loesen, sobald A physisch los ist -- auch wenn wir
    // gerade im Menue sind (dort laeuft der Gameplay-Release-Zweig nicht).
    if (!l_abutton) {
        m_la.long_consumed = false;
        m_la.back_guard = false;
    }

    if (re4vr::lua_get_tribool("__vr_raw_r_bbutton") != 1) {
        m_la.ks_x_guard = false;
    }

    // RY-Lock: Pitch kommt vom HMD, rechter Stick-Y wird gesperrt. Der
    // DPAD-Shift liest die ROHE Achse und bleibt unberuehrt.
    // Ausnahmen: __vr_unlock_ry, Menue (dort ist der Stick Navigation),
    // Throwsight (Harpune zielt hoch/runter), Fernglas, Kanone, Turret, Jetski
    // und Adas Pitch-GUI.
    if (re4vr::lua_get_tribool("__vr_unlock_ry") != 1 && !in_menu && !in_throwsight
        && !in_binoculars && re4vr::lua_get_tribool("__re4_at_cannon") != 1 && !in_turret
        && !in_jetski && !ada_pitch_free()) {
        f.ry = 0.0f;
    }

    // [KS2_STICK_LOCK / KS4_STICK_LOCK] Bei KS2 und KS4 den rechten Stick
    // KOMPLETT sperren -- also auch den YAW.
    // DIESELBEN AUSNAHMEN WIE DER RY-LOCK, nicht kuerzen:
    //  in_menu       -> sonst waere die Menue-Navigation tot, sobald man AUS
    //                   einer KS2-Zone das Menue oeffnet.
    //  in_throwsight -> die Del-Lago-Harpune LAEUFT SELBST ALS KS2; ohne diese
    //                   Ausnahme waere der Bosskampf unspielbar.
    //  in_binoculars -> braucht den Stick fuer Pitch/Zoom.
    // [KS4_STICK_EXEMPT] throwsight/Boot/Railcar sind zwar KS4-Events, dort
    // soll der rechte Stick aber wie im normalen Gameplay laufen. Das Loren-
    // INTRO bleibt ABSICHTLICH gesperrt.
    // [BULLETRUSH_STICK] Der Mercs-Ragemodus laeuft als KS4, ist aber AKTIVES
    // Gameplay -- der Stick muss dort weiter drehen.
    const bool ks_stick_exempt = in_boat
        || re4vr::lua_get_tribool("__re4_railcar_mode") == 1
        || re4vr::lua_get_tribool("__re4_force_ks4_bulletrush") == 1;

    if (!in_menu && !in_throwsight && !in_binoculars && !in_turret && !in_jetski
        && !ks_stick_exempt && (ks->is_ks2() || ks->is_ks4())) {
        f.rx = 0.0f;
        f.ry = 0.0f;
    }

    // [BOAT_STICK] Im Boot den GESAMTEN rechten Stick ROH durchreichen -> nativ
    // frei umsehen/zielen. Steht NACH den Stick-Locks und ueberschreibt sie
    // bewusst. NUR wenn NICHT im Menue (dort hat die Navigation Vorrang).
    if (boat_active && !in_menu) {
        f.rx = right_stick.x;
        f.ry = right_stick.y;
    }

    // [NO_BACK_SPRINT] Rueckwaerts kann man nicht sprinten: Dash (L3) bei
    // Rueckwaerts-Stick schlucken -- sonst dreht die Engine den Char 180 Grad
    // und rennt zur Kamera.
    if (moving_backward) {
        f.ls = false;
    }

    apply_frame(f);

    playwork_pause_tick();
    merchant_pause_tick();
}

// ============================================================================
// apply_frame -- die 15 Post-Branch-Eingriffe und der Versand ans Pad.
//
// Die REIHENFOLGE ist hier genauso tragend wie in der Branch-Kette:
// Grapple/Battle/Gigante setzen RT branchunabhaengig auf 1.0, deshalb steht der
// Map-/Gondel-Block als LETZTE Zeile vor set_trigger("RT"). Wer das umstellt,
// macht den Trigger in der Karte wieder auf.
// ============================================================================

void RE4VRBinding::apply_frame(Frame& f) {
    auto& pad = VigemPad::get();

    // [MENUE-STEUERUNG 11.09.2026] Mod-Menue offen (oder Loslass-Waechter nach dem
    // Schliessen): das Spiel bekommt vom Pad NICHTS. Die Controller-Eingaben sind
    // zwar schon zentral gesperrt (VR::is_menu_input_blocked), aber in dieser
    // Funktion setzen Timer und Automatiken Teile von f selbst (Roomscale-Hocken,
    // Red9-Aim, Kart-Neigung, Wesker-Block, erzwungene Trigger ...). Deshalb wird f
    // unmittelbar vor JEDEM Versand-Block neutral gesetzt.
    const bool menu_blocked = VR::get()->is_menu_input_blocked();

    const auto neutral_if_menu = [&]() {
        if (menu_blocked) {
            f = Frame{};
        }
    };

    // [SLOT-FIX 08.09.2026] Einmalig unser Pad identifizieren und XInput
    // umlenken, falls es nicht auf Slot 0 sitzt (s. VigemPad.cpp).
    pad.tick_slot_fix();

    // [MESSPUNKT] Beweis, dass der Pad-Export in diesem Frame ueberhaupt laeuft.
    re4vr::lua_set_number("__re4_apply_frame_t", clock_now());

    const bool frame_is_gameplay = re4vr::lua_get_tribool("__re4_frame_is_gameplay") == 1;

    // [RED9_RELOAD_AIM] Waehrend der nativen Red9-Reload-Anim Aim forcen -- die
    // linke-Hand-Anim ist an den Aim-/Hold-Zustand gekoppelt. motion setzt das
    // Flag nur fuer wp4002 + Reload-Node.
    // [GATE] nur im Gameplay -> im Menue/Boot/KS nie LT forcen.
    if (re4vr::lua_get_tribool("__vr_red9_reloading") == 1 && frame_is_gameplay) {
        f.lt = 1.0f;
    }

    // [BOLT_REAIM 2026-08-09] Bolt Rifle: nach dem Schuss wird der GEHALTENE
    // Griff/Aim echt abgebrochen und bleibt abgebrochen. Es wird ausdruecklich
    // KEIN neues Zielen erzwungen -- wer weiterzoomen will, muss loslassen und
    // neu druecken, damit das Spiel eine echte Flanke sieht.
    // BEWUSST ENG: der Stempel kommt nur von weapons bei einem Repetierer mit
    // echter Shoot-Node; die Sperre gilt nur, solange die Waffe ein Repetierer
    // ist UND Gameplay laeuft. 6114 (Ada) ist derselbe Repetierer wie 4400 --
    // ein harter 4400-Vergleich loeschte bei ihr den Stempel sofort.
    // Steht NACH allen LT-Settern und VOR dem Export.
    if (re4vr::lua_has_value("__re4_bolt_aim_cut_t")) {
        const bool held = f.lt >= 0.5f;
        const double bw = re4vr::lua_get_number("__re4_scope_wid", -1.0);

        if (!held || (bw != 4400.0 && bw != 6114.0) || !frame_is_gameplay) {
            re4vr::lua_set_nil("__re4_bolt_aim_cut_t");
        } else {
            f.lt = 0.0f;
        }
    }

    // [AIM_INPUT] Export fuer motions Aim-Transition: der INPUT laeuft dem
    // Engine-Kamera-Sprung voraus (is_aim flippt erst NACH dem Sprung).
    re4vr::lua_set_bool("__vr_aim_input", f.lt >= 0.5f);

    // [KART-KIPPAUSGLEICH PER HMD] minecart setzt das Global AUSSCHLIESSLICH
    // bei echter Kart-Fahrt in der Kippphase. Sonst passiert hier gar nichts --
    // der LINKE Stick bleibt sonst voellig unberuehrt (er ist die Bewegung).
    {
        const double lean = re4vr::lua_get_number("__re4_cart_lean_lx", 0.0);

        if (lean != 0.0) {
            f.lx = static_cast<float>(lean);
        }
    }

    const bool pure_gameplay = re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1;

    // [SNAPTURN 2026-08-10] Rechter Stick dreht in Stufen statt stufenlos.
    // Am ENGINE-YAW, nicht an der VR-Origin -- Begruendung bei add_camera_yaw.
    {
        if (m_prefs.enable_snapturn && pure_gameplay) {
            const float rx = f.rx;
            const float thr = m_prefs.snapturn_thresh;

            // Erst wieder scharf, wenn der Stick zurueck Richtung Mitte war
            // (halbe Schwelle als Hysterese) -- sonst springt Dauerhalten
            // endlos weiter.
            if (std::fabs(rx) < (thr * 0.5f)) {
                m_qt.st_armed = true;
            }

            if (m_qt.st_armed && std::fabs(rx) >= thr) {
                m_qt.st_armed = false;
                // Vorzeichen: das Engine-Yaw laeuft der Stickrichtung entgegen
                // -- Stick nach rechts gibt einen NEGATIVEN Yaw-Schritt.
                const float d = glm::radians(static_cast<float>(m_prefs.snapturn_deg))
                    * (rx > 0.0f ? -1.0f : 1.0f);
                add_camera_yaw(d);
            }

            // Der rohe Stick darf jetzt NICHT zusaetzlich ans Pad, sonst dreht
            // die Engine stufenlos weiter und der Sprung geht darin unter.
            // NUR die X-Achse; RY bleibt unangetastet.
            f.rx = 0.0f;
        } else {
            m_qt.st_armed = true;
        }
    }

    // [QUICKTURN_180] Doppel-Tipp linker Stick runter -> Kehrtwende.
    {
        if (m_prefs.enable_180_rotation && pure_gameplay) {
            qt_update(f.ly, 1.0f / 60.0f);

            if (m_qt.seq > 0) {
                // [TURN180 2026-08-11 NEU GEBAUT] Kein Tastenspiel mehr (frueher
                // LY auf -1.0 halten und RB dazu). Das schickte die native Kombi
                // ins Spiel, und die kam gegen unseren eigenen Yaw-Antrieb nicht
                // an: gemessen drehte der Koerper 130-190 Grad, die Kamera
                // folgte nicht, danach zog es alles zurueck.
                // Jetzt drehen wir selbst -- derselbe Weg wie beim Snapturn.
                m_qt.turn_left = glm::pi<float>();
                m_qt.turn_last = clock_now();

                // Einmal ausloesen reicht: Sequenz sofort beenden, Cooldown wie
                // gehabt, damit ein gehaltener Stick nicht endlos weiterdreht.
                m_qt.seq = 0;
                m_qt.cooldown = true;
                m_qt.phase = 0;
                m_qt.post = QuickTurn::POST_FRAMES;
            }

            // [TURN180_LERP] Die Drehung ueber die eingestellte Dauer verteilen.
            if (m_qt.turn_left > 0.0f) {
                const float dur = m_prefs.turn180_sec;
                const double now = clock_now();
                const float dt = static_cast<float>(now - m_qt.turn_last);
                m_qt.turn_last = now;

                float step = 0.0f;

                if (dur <= 0.001f) {
                    step = m_qt.turn_left;
                } else {
                    step = glm::pi<float>() * (dt / dur);

                    if (step > m_qt.turn_left) {
                        step = m_qt.turn_left;
                    }
                }

                m_qt.turn_left -= step;
                add_camera_yaw(step);
            }
        } else if (m_qt.phase != 0 || m_qt.seq > 0 || m_qt.cooldown || m_qt.post > 0) {
            qt_reset();
        }
    }

    // [ROOMSCALE_CROUCH 2026-08-11] Der Kopfhoehen-Vergleich in movement setzt
    // die Marke, hier wird B gedrueckt. Ein einzelner Frame war zu kurz (das
    // Log zeigte 32x "B angefordert", waehrend der Hock-Zustand nie umsprang)
    // -> die Marke wird in einen Zaehler umgesetzt und B ueber MEHRERE Frames
    // gehalten, wie ein echter Tastendruck.
    if (re4vr::lua_get_tribool("__re4_want_crouch_press") == 1) {
        re4vr::lua_set_bool("__re4_want_crouch_press", false);
        re4vr::lua_set_number("__re4_crouch_press_frames", 4.0);
    }

    if (const double cpf = re4vr::lua_get_number("__re4_crouch_press_frames", 0.0); cpf > 0.0) {
        re4vr::lua_set_number("__re4_crouch_press_frames", cpf - 1.0);
        f.b = true;
    }

    // [CHOKE: KEIN DREHEN, KEIN STRAFEN 2026-08-22] Waehrend eines Wuergegriffs
    // stehen BEIDE Seitwaerts-Achsen still: die Ausrichtung des Gehaltenen ist
    // eingefroren -- drehst du dich weg oder laeufst seitlich um ihn herum,
    // rutscht die Hand vom Hals. Vor/zurueck bleibt frei.
    //
    // [NIE STALE] Eine gesperrte Achse ist das Schlimmste, was haengenbleiben
    // kann -- deshalb DREI unabhaengige Riegel, jeder loest die Sperre allein:
    //  1. Frischefenster 0.15 s -- faengt ein TOTES Choke-Modul ab.
    //  2. Nur waehrend echtem Gameplay -- faengt Levelwechsel, Ladephase,
    //     Menue und Killswitch ab ("mitten im Griff ein neues Level").
    //  3. Harte Obergrenze = Haltedauer aus choke + 0,5 s Luft -- faengt einen
    //     HAENGENDEN Griff ab. Fehlt der Wert, greift 5 s blind.
    {
        const bool on = re4vr::lua_call_global_bool("__re4_is_choking", false)
            && frame_is_gameplay;

        if (on) {
            if (!m_choke_axis_block_t.has_value()) {
                m_choke_axis_block_t = clock_now();
            }

            const double lim = re4vr::lua_get_number("__re4_choke_hold_max", 4.5) + 0.5;

            if ((clock_now() - *m_choke_axis_block_t) < lim) {
                f.rx = 0.0f;   // Drehen
                f.lx = 0.0f;   // Strafen
            } else if (!m_choke_axis_block_warned) {
                m_choke_axis_block_warned = true;
            }
        } else {
            m_choke_axis_block_t.reset();
            m_choke_axis_block_warned = false;
        }
    }

    // [GONDEL: NUR LINKER STICK 2026-08-30] In Adas Gondel friert die Kabine bei
    // JEDER Eingabe ausser Laufen ein. Also geht waehrend der Fahrt nur noch
    // LX/LY ans Pad, alles andere wird neutral gesetzt.
    // KEIN LATCH, KEIN TIMER: die Abfrage gilt exakt fuer diesen Frame.
    // FENSTER: erst AB DEM EINSTIEG (Phase B der Stillzone), nicht schon an der
    // Ortszone davor. NUR ADA -- bei Leon laeuft ohnehin KS4.
    // NUR IM GAMEPLAY: waehrend der Fahrt kann man ins Menue, dort muss alles
    // normal arbeiten, sonst kommt man nicht mehr heraus.
    const bool gondel_lstick = re4vr::lua_get_tribool("__re4_stillzone_hide_now") == 1
        && re4vr::lua_get_tribool("__re4_gondola_active") != 1
        && frame_is_gameplay
        && re4vr::lua_get_tribool("__re4_gondel_lstick_aus") != 1;

    if (gondel_lstick) {
        f.rx = 0.0f;
        f.ry = 0.0f;   // rechter Stick aus (Umsehen macht das HMD)
        f.lt = 0.0f;
        f.rt = 0.0f;
        f.a = false;
        f.b = false;
        f.x = false;
        f.y = false;
        f.lb = false;
        f.rb = false;
        f.ls = false;
        f.rs = false;
        f.back = false;
        f.start = false;
        f.dpad_up = false;
        f.dpad_down = false;
        f.dpad_left = false;
        f.dpad_right = false;
    }

    neutral_if_menu();   // [MENUE-STEUERUNG]

    pad.set_axis("LX", clampf(f.lx, -1.0f, 1.0f));
    pad.set_axis("LY", clampf(f.ly, -1.0f, 1.0f));
    pad.set_axis("RX", clampf(f.rx, -1.0f, 1.0f));
    pad.set_axis("RY", clampf(f.ry, -1.0f, 1.0f));

    // [ERST LOSLASSEN 2026-08-12] Nach einem Holster-Grab zaehlt der Grip erst
    // wieder als Zielen, wenn er einmal losgelassen wurde. Die Durchsetzung
    // steht unmittelbar vor dem Versand, weil f.LT an acht Stellen gesetzt wird.
    // Geloest wird ausschliesslich am ROHEN Grip.
    if (re4vr::lua_get_tribool("__re4_aim_relatch") == 1) {
        f.lt = 0.0f;

        if (re4vr::lua_get_tribool("__vr_raw_l_grip") != 1) {
            re4vr::lua_set_bool("__re4_aim_relatch", false);
        }
    }

    // [MAP_TRIGGERS 2026-08-15] In der Kartenansicht zoomen LT und RT die Karte
    // -- gezoomt wird dort ausschliesslich mit dem rechten Stick. Geblockt wird
    // nur die AUSGABE: der Trigger wird weiter gelesen, deshalb bleibt
    // "L.Trigger + R-Stick -> DPAD" vollstaendig erhalten.
    // NUR EINE Abfrage pro Frame.
    const bool map_block_triggers = is_map_open_now();

    if (map_block_triggers) {
        f.lt = 0.0f;
    }

    neutral_if_menu();   // [MENUE-STEUERUNG]

    pad.set_trigger("LT", f.lt);

    // [BURST] Nur noch die Trigger-Flanke: sie sagt dem nativen Gate, wann ein
    // neuer Druck beginnt und der Zaehler wieder bei 0 anfaengt. Die Begrenzung
    // selbst sitzt am nativen Schuss (isEnableFire), weil sie pro Schuss statt
    // pro Frame greifen muss.
    {
        const bool rt_down = f.rt >= 0.5f;

        if (rt_down && !m_burst_prev_rt) {
            re4vr::lua_set_number("__vr_burst_press_id",
                                  re4vr::lua_get_number("__vr_burst_press_id", 0.0) + 1.0);
        }

        re4vr::lua_set_bool("__vr_burst_rt_down", rt_down);
        m_burst_prev_rt = rt_down;
    }

    const bool knife_equipped = re4vr::lua_get_tribool("__re4_knife_equipped") == 1;
    const bool finisher_prompt = re4vr::lua_call_global_bool("__re4_is_finisher_prompt", false);
    const bool knife_flip = re4vr::lua_get_tribool("__vr_knife_flip") == 1;

    // [KNIFE_RT_MUTE 2026-07-03] Messer in der Hand -> rechten Trigger
    // stilllegen. [GATE] NUR im reinen Gameplay: sonst schluckt der Mute RT in
    // JEDEM Menu/Boot/KS -- die Branches setzen RT dort bewusst.
    // [FINISHER_LINKS 2026-09-09 -- Testerbefund] Liegt das Messer LINKS
    // (equippt mit knife_hand == "left" ODER als Klon), besitzt RT den Flip
    // NICHT -- der haengt dort am LINKEN Trigger ([KNIFE_FLIP LINKS], oben).
    // Also darf der physische RT beim Prompt IMMER durch. Vorher hing der
    // Durchlass am Fenster `!knife_flip` und am Gate `knife_equipped`: der
    // Backstab mit Messer links ging deshalb NUR ueber die Stich-Geste, ein
    // blosser RT-Druck tat gar nichts. Rechts bleibt alles wie bisher, dort
    // gehoert RT im Flip-Zustand der Geste.
    const bool knife_left_clone = re4vr::lua_get_tribool("__re4_knife_left_clone") == 1;
    const bool knife_is_left = knife_left_clone
        || re4vr::lua_get_string("__re4_knife_hand") == "left";
    const bool finisher_rt_window = finisher_prompt && (knife_is_left || !knife_flip);
    const bool raw_rt_now = re4vr::lua_get_tribool("__vr_raw_r_trigger") == 1;

    if (knife_equipped && frame_is_gameplay) {
        f.rt = 0.0f;

        // [KNIFE_FINISHER] Bei sichtbarem Prompt haengt es am Flip-Zustand:
        //  * FLIPPED: Stich-Geste noetig -> RT NICHT durchlassen.
        //  * NORMAL: der physische RT wird durchgelassen.
        // Der Aufruf steht bewusst AUSSERHALB der Prompt-Abfrage: die Funktion
        // muss ihren Zustand auch pflegen, wenn das Fenster zu ist -- sonst
        // bliebe sie mit gehaltenem Trigger fuers naechste Prompt stumpf.
        if (finisher_prompt && knife_flip
            && re4vr::lua_get_tribool("__re4_knife_finisher_shake") == 1) {
            f.rt = 1.0f;
        }

        apply_finisher_rt(f, finisher_rt_window, raw_rt_now);
    }

    // [LH_CLONE FINISHER] Der Klon ist NICHT engine-equippt -> das Gate oben
    // greift nicht. Gleiche Zwei-Fall-Logik bei sichtbarem Prompt.
    if (knife_left_clone && frame_is_gameplay && finisher_prompt) {
        if (knife_flip) {
            f.rt = re4vr::lua_get_tribool("__re4_knife_finisher_shake") == 1 ? 1.0f : 0.0f;
        }

        // Der physische RT: nur hier aufrufen, wenn der Block oben NICHT lief
        // -- apply_finisher_rt fuehrt einen Frame-Zaehler, ein zweiter Aufruf
        // im selben Frame wuerde ihn doppelt herunterzaehlen.
        if (!knife_equipped) {
            apply_finisher_rt(f, finisher_rt_window, raw_rt_now);
        }
    }

    // [KNIFE_X_MUTE / BARE_HANDS_X_MUTE] Messer equippt ODER geworfen ODER
    // leere Haende -> X (nativer Reload) stilllegen. Sonst loest der rechte B
    // einen nativen Pistol-Reload aus bzw. zieht bare-handed die zuletzt
    // geholsterte Waffe hervor.
    // NUR im reinen Gameplay: in Menu/Boot/KS ist X legitim (Inventar).
    const auto bind_wid = binding_equip_weapon_id();
    const bool is_bare_hands = !bind_wid.has_value() || *bind_wid < 0;

    if ((knife_equipped || re4vr::lua_get_tribool("__re4_knife_flying") == 1 || is_bare_hands)
        && frame_is_gameplay) {
        f.x = false;   // Button = bool! 0.0f waere truthy und wuerde X DRUECKEN
    }

    // [GESTURE_SHIFT] Laeuft eine Gesten-Kombi, darf das A nicht zusaetzlich
    // als Xbox-A rausgehen (sonst greift/interagiert Leon dabei).
    if (re4vr::lua_get_tribool("__re4_gest_mute") == 1) {
        f.a = false;
    }

    const bool raw_rt = re4vr::lua_get_tribool("__vr_raw_r_trigger") == 1;

    // [GRAPPLE-WEHREN] Niedergerungen -> den rohen Trigger feuern lassen, egal
    // welcher Branch/Aim/Messer. Der Grab ist ein Killswitch-State, und dort
    // reicht apply_rt_attack RT nur bei Aim durch -- im Grab zielt man nicht.
    if (raw_rt && player_in_grapple()) {
        f.rt = 1.0f;
    }

    // [BATTLE-RT] Im Kampf-State den rohen RT durchfeuern lassen.
    // [MESSER-FLIP AUSNAHME] ABER NICHT, wenn das rechte Messer RT gerade fuer
    // den FLIP besitzt: sonst macht ein RT-Druck BEIDES -- Flip UND nativen
    // Attack. Der Messer-Angriff laeuft in VR ueber den physischen Swing, NIE
    // ueber RT. Beim Finisher-Prompt besitzt der Flip RT nicht -> dann darf es
    // feuern.
    const bool knife_owns_rt = knife_equipped
        && re4vr::lua_get_string("__re4_knife_hand") != "left" && !finisher_prompt;

    if (raw_rt && player_in_battle() && !knife_owns_rt) {
        f.rt = 1.0f;
    }

    // [GIGANTE-RT 2026-08-30] Zweiter, unabhaengiger Durchlass fuer den Ritt.
    // Steht BEWUSST hinter der Battle-Zeile: dort faellt raus, wer MIT
    // gezogenem Messer aufsitzt -- genau die Spieler, bei denen der Stich nie
    // kam. Hier gilt der Messer-Zustand absichtlich NICHT.
    if (raw_rt && player_on_gigante()) {
        f.rt = 1.0f;
    }

    // [MAP_TRIGGERS / GONDEL] LETZTE Zeile vor dem Versand: Grapple-, Battle-
    // und Gigante-RT direkt darueber setzen RT branchunabhaengig auf 1.0 --
    // stuende der Block frueher, drehten sie ihn zurueck.
    if (map_block_triggers || gondel_lstick) {
        f.rt = 0.0f;
    }

    // [RT-SPERRE IM STAGGER 15.09.2026] Siehe Header. Steht bewusst GANZ
    // zuletzt -- Battle-, Grapple- und Gigante-RT setzen darueber auf 1.0.
    {
        const double now = clock_now();
        const std::optional<bool> holding = gun_is_holding_local();

        bool sperren = false;

        if (holding.has_value() && !*holding) {
            if (!m_rt_wait_since.has_value()) {
                m_rt_wait_since = now;
            }

            if (!m_rt_wait_gaveup) {
                if ((now - *m_rt_wait_since) > RT_WAIT_MAX) {
                    m_rt_wait_gaveup = true;   // Notbremse: nie dauerhaft sperren
                } else {
                    sperren = true;
                }
            }
        } else {
            // Waffe da ODER Zustand unbekannt -> alles zurueck, nichts sperren.
            m_rt_wait_since.reset();
            m_rt_wait_gaveup = false;
        }

        if (sperren) {
            f.rt = 0.0f;
        }

        re4vr::lua_set_number("__re4_rt_stagger_block", sperren ? now : 0.0);

    }

    neutral_if_menu();   // [MENUE-STEUERUNG]

    pad.set_trigger("RT", f.rt);

    neutral_if_menu();   // [MENUE-STEUERUNG]

    pad.set_button("A", f.a);
    pad.set_button("B", f.b);
    pad.set_button("X", f.x);
    pad.set_button("Y", f.y);

    // [WESKER_ARM_PARRY 2026-08-26] Wesker hat in Mercenaries nie ein Messer und
    // blockt mit dem ARM. weapons2 meldet die gehaltene Schutz-Pose -> hier wird
    // daraus ein GEHALTENER LB. Rein ADDITIV -- LB wird nur gesetzt, nie
    // geloescht. Eine Charakter-Abfrage ist nicht noetig: das Flag ist
    // ausserhalb von Mercs immer false.
    {
        const bool wp_hold = re4vr::lua_get_tribool("__re4_wesker_parry_hold") == 1;

        // [GONDEL] steht nach dem Nur-linker-Stick-Block und wuerde LB sonst
        // wieder setzen.
        if (wp_hold && frame_is_gameplay && !gondel_lstick) {
            f.lb = true;
        }

        // Reine Veroeffentlichung, kein Verhalten.
        re4vr::lua_set_bool("__re4_wesker_lb_sent", wp_hold && frame_is_gameplay);
    }

    neutral_if_menu();   // [MENUE-STEUERUNG]

    pad.set_button("LB", f.lb);
    pad.set_button("RB", f.rb);
    pad.set_button("LS", f.ls);
    pad.set_button("RS", f.rs);
    pad.set_button("BACK", f.back);
    pad.set_button("START", f.start);
    pad.set_button("DPAD_UP", f.dpad_up);
    pad.set_button("DPAD_DOWN", f.dpad_down);
    pad.set_button("DPAD_LEFT", f.dpad_left);
    pad.set_button("DPAD_RIGHT", f.dpad_right);

    // [DPAD_EQUIP 2026-07-17] Das DPad ist der native Waffen-Shortcut = ein
    // GEWOLLTER Wechsel -> Freifahrt fuer den KNIFE_KEEP_OUT-Block in weapons.
    // Ohne das waere mit Messer in der Hand KEIN DPad-Waffenwechsel moeglich.
    // Der Engine-Klau nach dem Killswitch kommt OHNE Input -> faellt nicht in
    // dieses Fenster und bleibt geblockt.
    if (f.dpad_up || f.dpad_down || f.dpad_left || f.dpad_right) {
        re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
    }

    // [PAD-SPIEGEL 2026-07-22] Rein lesende Diagnose: was WIR in diesem Frame
    // ans virtuelle Pad geschickt haben. Ein von uns gesendeter Knopf erscheint
    // der Engine als normaler Spieler-Input -- ein daraus folgender
    // Waffenwechsel stand im Trace bisher als "NATIV" und war nicht von echtem
    // Engine-Verhalten zu unterscheiden.
    {
        char line[256]{};
        std::snprintf(line, sizeof(line),
                      "RT=%.2f LT=%.2f A=%d B=%d X=%d Y=%d LB=%d RB=%d LS=%d RS=%d "
                      "DP=%c%c%c%c LX=%.2f LY=%.2f RX=%.2f RY=%.2f",
                      f.rt, f.lt, f.a ? 1 : 0, f.b ? 1 : 0, f.x ? 1 : 0, f.y ? 1 : 0,
                      f.lb ? 1 : 0, f.rb ? 1 : 0, f.ls ? 1 : 0, f.rs ? 1 : 0,
                      f.dpad_up ? 'U' : '-', f.dpad_down ? 'D' : '-',
                      f.dpad_left ? 'L' : '-', f.dpad_right ? 'R' : '-',
                      f.lx, f.ly, f.rx, f.ry);
        re4vr::lua_set_string("__re4_last_pad", line);
    }
}

// ============================================================================
// [PLAYWORK-PAUSE NOTAUS 2026-08-26]
//
// Beim Laden EINES bestimmten Saves (Stage 47400) startet ein PlayWorkGimmick
// eine Custom-Pause und gibt sie nie wieder frei -- das Spiel bleibt dauerhaft
// pausiert, obwohl Rig, Meshes, VR-Sitzung und Waffen vollstaendig da sind.
//
// WAS DAS TUT: nichts blocken, nichts vorbeugen, keinen Eingabepfad gaten --
// erst NACHDEM der Haenger nachweislich steht, wird genau diese eine
// Custom-Pause freigegeben. Das ist exakt die Gegenoperation, die das Spiel
// beim Stage-Wechsel selbst aufruft.
//
// Fuenf Riegel muessen ALLE zutreffen. Riegel 5 ist der wichtige: der Haenger
// beginnt gemessen bei +2,2 s. Ein ECHTER PlayWork mitten im Spiel (Kurbel,
// Schublade) startet immer viel spaeter und kann damit nie in den Notaus
// laufen -- erst dadurch ist die kurze Wartezeit von 4 s gefahrlos.
// ============================================================================

// [MENDEZ IN DER SZENE 15.09.2026 -- Ansage des Users] Regel, genau so:
// Laedt man ein Savegame in diesen Stages, laeuft der Pause-Loeser. Ist
// MENDEZ in der Szene, bleibt alles normal -- kein Eingriff. Killt er uns und
// wir wachen vor dem Kampf wieder auf, ist er nach dem Laden NICHT mehr in der
// Szene, und der Notaus ist wieder aktiv.
//
// Er steht als Gegner-Context in der Liste, die auch pick_nearest_enemy
// benutzt (CharacterManager.get_EnemyContextList). Gemessen in
// re4_szene_scan.txt: chainsaw.Ch1f4z1Context (KindID 200020) und
// chainsaw.Ch1f5z1Context (KindID 200021) -- seine zwei Formen.
bool RE4VRBinding::mendez_in_scene() {
    auto* cm = re4vr::fc::managed_singleton("chainsaw.CharacterManager");

    if (cm == nullptr) {
        return false;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList");

    if (list == nullptr) {
        return false;
    }

    int32_t n = 0;

    if (!re4vr::try_call<int32_t>(list, "get_Count", n)) {
        try {
            n = re4vr::get_field_int_v(list, "_size");
        } catch (...) {
            return false;
        }
    }

    for (int32_t i = 0; i < n && i < 64; ++i) {
        auto* e = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (e == nullptr) {
            continue;
        }

        auto* td = utility::re_managed_object::get_type_definition(e);

        if (td == nullptr) {
            continue;
        }

        const std::string tn = td->get_full_name();

        if (tn != "chainsaw.Ch1f4z1Context" && tn != "chainsaw.Ch1f5z1Context") {
            continue;
        }

        // [KORREKTUR 15.09.2026] Die reine Existenz reicht NICHT: beide
        // Contexts stehen von Anfang an in der Liste, auch lange vor dem
        // Kampf -- damit war der Notaus sofort aus und der Fix wirkungslos.
        // Erst wenn er wirklich in der Szene steht, hat er ein
        // BodyGameObject; genau danach filtert auch pick_nearest_enemy.
        if (auto* body = re4vr::call_safe<::REManagedObject*>(e, "get_BodyGameObject");
            re4vr::obj_ok(body)) {
            return true;
        }
    }

    return false;
}

void RE4VRBinding::playwork_pause_tick() {
    constexpr const char* OWNER = "PlayWorkGimmick";
    constexpr int32_t PAUSE_ID = 2;
    // [STAGE-RIEGEL RAUS 15.09.2026 -- Ansage des Users] Der Haenger trat an
    // derselben Stelle wieder auf, die Sonde las aber Stage 47410 statt 47400:
    //   Stage=47410  _CustomPause=1 [Pause=2 Owner=PlayWorkGimmick]
    // Dieselbe Signatur, nur der Nachbar-Abschnitt -- und der harte Vergleich
    // auf 47400 liess den Notaus in der ersten Zeile aussteigen. Weil der
    // naechste Nachbar dasselbe Problem haette, faellt die Stage-Bedingung
    // ganz weg. Die drei uebrigen Riegel sind eindeutig genug: keine normale
    // Pause offen, GENAU EIN CustomPause-Eintrag mit Pause 2 und Owner
    // PlayWorkGimmick, und das seit PW_WAIT Sekunden ununterbrochen.
    constexpr double PW_WAIT = 4.0;
    constexpr double PW_WINDOW = 6.0;

    const double now = clock_now();

    if ((now - m_pw.last_check) < 0.5) {
        return;   // 2x pro Sekunde reicht, kostet nichts
    }

    m_pw.last_check = now;

    // [MENDEZ] Riegel vorerst AUS: sein Context hat auch vor dem Kampf ein
    // BodyGameObject, damit galt er sofort als anwesend -- der Pause-Loeser war
    // dauerhaft abgeschaltet und man konnte sich nicht mehr bewegen. Erst wenn
    // das unterscheidende Merkmal gemessen ist (zzz_re4_szene_scan.lua schreibt
    // body/enabled/draw/elim/dead/alive/hp mit), wird hier wieder gegatet.
    if (re4vr::lua_get_bool("__re4_mendez_gate", false) && mendez_in_scene()) {
        re4vr::lua_set_string("__re4_playwork_pause_last",
                              "Mendez in der Szene -- Notaus aus");

        return;
    }

    auto* pm = re4vr::fc::managed_singleton("share.PauseManager");

    // Anzahl einer Liste (get_Count oder _size).
    const auto cnt = [](::REManagedObject* lst) -> std::optional<int32_t> {
        if (lst == nullptr) {
            return std::nullopt;
        }

        if (int32_t c = 0; re4vr::try_call<int32_t>(lst, "get_Count", c)) {
            return c;
        }

        try {
            return re4vr::get_field_int_v(lst, "_size");
        } catch (...) {
            return std::nullopt;
        }
    };

    // Nachmessen NIE im selben Frame: der PauseManager arbeitet seine Requests
    // verzoegert ab.
    if (m_pw.check_at.has_value() && now >= *m_pw.check_at) {
        m_pw.check_at.reset();

        if (pm != nullptr) {
            ::REManagedObject* cp = nullptr;

            try {
                cp = re4vr::get_field_object(pm,
                                                                              "_CustomPause");
            } catch (...) {
            }

            const auto noch = cnt(cp).value_or(-1);
            const bool paused = call_bool(pm, "isPaused()");
            re4vr::lua_set_string("__re4_playwork_pause_last",
                                  !paused ? std::string{"befreit"}
                                          : "haengt weiter (_CustomPause="
                                                + std::to_string(noch) + ")");
        }
    }

    // Ladeflanke: Kontext war weg und ist wieder da -> pro Ladevorgang genau
    // ein Versuch.
    auto* ctx = re4vr::fc::ctx();
    const bool has_ctx = ctx != nullptr;

    if (has_ctx && !m_pw.had_ctx) {
        m_pw.paused_since.reset();
        m_pw.already_freed = false;
        m_pw.load_t = now;
    }

    m_pw.had_ctx = has_ctx;

    // [STAGE-RIEGEL 15.09.2026 -- Ansage des Users] Nur in den beiden Stages
    // mit dem PlayWorkGimmick-Haenger arbeiten, sonst nirgends.
    //
    // ACHTUNG, STELLE IST WICHTIG: der Riegel stand zuerst GANZ OBEN und hat
    // damit die Ladeflanke direkt darueber verschluckt -- beim Laden ist die
    // Stage kurz nicht lesbar, der Tick stieg vorher aus, `already_freed`
    // wurde nie zurueckgesetzt, und nach Tod + Neuladen feuerte der Notaus
    // kein zweites Mal ("dein pause fix geht nicht", 13:33).
    {
        int32_t sid = 0;

        if (!has_ctx || !re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", sid)
            || (sid != 47400 && sid != 47410)) {
            return;
        }
    }

    if (!has_ctx || m_pw.already_freed || pm == nullptr) {
        return;
    }

    if (!call_bool(pm, "isPaused()")) {
        m_pw.paused_since.reset();

        return;
    }

    if (!m_pw.paused_since.has_value()) {
        m_pw.paused_since = now;
    }

    if ((now - *m_pw.paused_since) < PW_WAIT) {
        return;
    }

    // Riegel 5: hat die Pause frueh genug nach dem Laden BEGONNEN?
    if (!m_pw.load_t.has_value() || (*m_pw.paused_since - *m_pw.load_t) > PW_WINDOW) {
        return;
    }

    // Riegel 1: Stage
    int32_t st = 0;

    // Stage wird nur noch GELESEN (fuer die Log-Zeile), nicht mehr geprueft.
    re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", st);

    // Riegel 2: keine normale Pause offen (Menue, Cutscene, ...)
    ::REManagedObject* plist = nullptr;

    try {
        plist = re4vr::get_field_object(pm, "_Pause");
    } catch (...) {
    }

    if (cnt(plist).value_or(0) != 0) {
        return;
    }

    // Riegel 3: genau die eine bekannte Custom-Pause. Ueber das Traegerfeld
    // _items, weil CustomPauseInfo ein ValueType ist und get_Item dort
    // plausiblen Muell liefern kann.
    ::REManagedObject* clist = nullptr;

    try {
        clist = re4vr::get_field_object(pm, "_CustomPause");
    } catch (...) {
    }

    if (cnt(clist).value_or(-1) != 1) {
        return;
    }

    ::REManagedObject* it = nullptr;

    try {
        auto* items = re4vr::get_field_object(clist, "_items");

        if (items != nullptr) {
            it = re4vr::array_element(items, 0);
        }
    } catch (...) {
    }

    if (it == nullptr) {
        it = re4vr::call_safe<::REManagedObject*>(clist, "get_Item", 0);
    }

    if (it == nullptr) {
        return;
    }

    int32_t p_id = -1;
    std::string p_owner{};

    try {
        p_id = re4vr::get_field_int_v(it, "Pause");
        auto* on = re4vr::get_field_object(it, "OwnerName");
        p_owner = mstr(on);
    } catch (...) {
        return;
    }

    if (p_id != PAUSE_ID || p_owner != OWNER) {
        return;
    }

    // Alle Riegel offen: EINMAL freigeben. Die Ueberladung wird NICHT geraten,
    // sondern aus der Typdatenbank geholt -- bevorzugt die mit einem String
    // (dem OwnerName, mit dem das Spiel die Pause selbst freigibt).
    m_pw.already_freed = true;

    auto* pm_td = sdk::find_type_definition("share.PauseManager");

    if (pm_td == nullptr) {
        return;
    }

    sdk::REMethodDefinition* end_m = nullptr;
    std::vector<std::string> end_types{};

    for (auto& m : pm_td->get_methods()) {
        if (m.get_name() == nullptr || std::strcmp(m.get_name(), "requestEndCustomPause") != 0) {
            continue;
        }

        std::vector<std::string> ts{};

        for (auto* pt : m.get_param_types()) {
            ts.emplace_back(pt != nullptr ? pt->get_full_name() : "?");
        }

        const auto has_string = [](const std::vector<std::string>& v) {
            return std::find(v.begin(), v.end(), "System.String") != v.end();
        };

        if (end_m == nullptr || (has_string(ts) && !has_string(end_types))) {
            end_m = &m;
            end_types = ts;
        }
    }

    if (end_m == nullptr) {
        return;
    }

    // Parameter nach ihrem TYP befuellen (Pause-ID / OwnerName / Rest 0 bzw.
    // false) -- genau wie die Lua-Fassung.
    try {
        auto tctx = sdk::get_thread_context();

        if (end_types.size() == 2 && end_types[1] == "System.String") {
            end_m->call<void*>(tctx, pm, PAUSE_ID, sdk::VM::create_managed_string(
                                                       utility::widen(OWNER)));
        } else if (end_types.size() == 3 && end_types[1] == "System.String") {
            // [DRITTER PARAMETER 15.09.2026 -- gemessen] Genau diese
            // Ueberladung hat das Spiel:
            //   requestEndCustomPause(System.UInt32, System.String, System.Action)
            // Der dritte ist ein Callback und bleibt LEER -- so steht es schon
            // in der Messung vom 26.08. Der Port kannte nur die ein- und
            // zweiteilige Form und ist deshalb still in den else-Zweig
            // gelaufen ("Signatur nicht behandelt"), nachdem alle vier Riegel
            // offen waren.
            end_m->call<void*>(tctx, pm, PAUSE_ID,
                               sdk::VM::create_managed_string(utility::widen(OWNER)),
                               nullptr);
        } else if (end_types.size() == 1) {
            end_m->call<void*>(tctx, pm, PAUSE_ID);
        } else {
            re4vr::lua_set_string("__re4_playwork_pause_last",
                                  "Signatur nicht behandelt -- nichts veraendert");

            return;
        }
    } catch (...) {
        re4vr::lua_set_string("__re4_playwork_pause_last", "Aufruf fehlgeschlagen");

        return;
    }

    re4vr::lua_set_number("__re4_playwork_pause_freed",
                          re4vr::lua_get_number("__re4_playwork_pause_freed", 0.0) + 1.0);
    re4vr::lua_set_string("__re4_playwork_pause_last", "aufgerufen");
    m_pw.check_at = now + 1.0;
}

// [MERCHANT-PAUSE 15.09.2026] Der zweite Haenger derselben Stelle: nach dem
// Verlassen des HAENDLERS bleibt in der NORMALEN Pause-Liste ein Eintrag
// stehen -- gemessen (re4_pwpause.txt 12:46:50):
//   _Pause=1 [Pause=7 OwnerName=InGameMenuOccupiedModule]
// Damit liefert isPaused() dauerhaft true, der Fork meldet gameplay=0
// [menu:paused] und nichts geht mehr. Beim Typewriter passiert das nicht,
// deshalb "geht nach dem Typewriter wieder, nach dem Merchant nicht".
//
// playwork_pause_tick raeumt nur die CustomPause-Liste; diese hier ist eine
// andere. Die Gegenoperation heisst requestEndPause(PauseType, String, Action)
// -- aus der Typdatenbank, nicht geraten (Inventur 12:48).
//
// ENG: nur in 47400/47410, nur dieser eine Owner, nur wenn wirklich kein
// Fenster mehr offen ist (Typewriter/Karte zu, keine CustomPause), erst nach
// MP_WAIT Sekunden und hoechstens alle MP_COOL Sekunden.
void RE4VRBinding::merchant_pause_tick() {
    constexpr const char* MP_OWNER = "InGameMenuOccupiedModule";
    constexpr double MP_WAIT = 3.0;
    constexpr double MP_COOL = 5.0;

    const double now = clock_now();

    if ((now - m_mp_last_check) < 0.5) {
        return;
    }

    m_mp_last_check = now;

    if (now < m_mp_next) {
        return;
    }

    // Stage-Riegel
    {
        auto* c = re4vr::fc::ctx();
        int32_t sid = 0;

        if (c == nullptr || !re4vr::try_call<int32_t>(c, "get_CurrentStageID", sid)
            || (sid != 47400 && sid != 47410)) {
            m_mp_since.reset();

            return;
        }
    }

    // Ist wirklich nichts mehr offen?
    if (auto* am = re4vr::fc::managed_singleton("chainsaw.ArmouryManager");
        am != nullptr && call_bool(am, "get_IsTypewriterWindow")) {
        m_mp_since.reset();

        return;
    }

    if (auto* mm = re4vr::fc::managed_singleton("chainsaw.MapManager");
        mm != nullptr && call_bool(mm, "isMapGuiOpen")) {
        m_mp_since.reset();

        return;
    }

    auto* pm = re4vr::fc::managed_singleton("share.PauseManager");

    if (pm == nullptr) {
        m_mp_since.reset();

        return;
    }

    const auto cnt = [](::REManagedObject* lst) -> std::optional<int32_t> {
        if (lst == nullptr) {
            return std::nullopt;
        }

        if (int32_t c = 0; re4vr::try_call<int32_t>(lst, "get_Count", c)) {
            return c;
        }

        try {
            return re4vr::get_field_int_v(lst, "_size");
        } catch (...) {
            return std::nullopt;
        }
    };

    // Genau EIN Eintrag, und der muss unserer sein.
    ::REManagedObject* plist = nullptr;

    try {
        plist = re4vr::get_field_object(pm, "_Pause");
    } catch (...) {
    }

    if (cnt(plist).value_or(-1) != 1) {
        m_mp_since.reset();

        return;
    }

    ::REManagedObject* it = nullptr;

    try {
        if (auto* items = re4vr::get_field_object(plist, "_items"); items != nullptr) {
            it = re4vr::array_element(items, 0);
        }
    } catch (...) {
    }

    if (it == nullptr) {
        m_mp_since.reset();

        return;
    }

    int32_t p_id = -1;
    std::string p_owner{};

    try {
        p_id = re4vr::get_field_int_v(it, "Pause");
        p_owner = mstr(re4vr::get_field_object(it, "OwnerName"));
    } catch (...) {
        m_mp_since.reset();

        return;
    }

    if (p_owner != MP_OWNER) {
        m_mp_since.reset();

        return;
    }

    if (!m_mp_since.has_value()) {
        m_mp_since = now;
    }

    if ((now - *m_mp_since) < MP_WAIT) {
        return;
    }

    // Freigeben -- Ueberladung aus der TDB, String managed erzeugen.
    auto* pm_td = sdk::find_type_definition("share.PauseManager");

    if (pm_td == nullptr) {
        return;
    }

    for (auto& m : pm_td->get_methods()) {
        if (m.get_name() == nullptr || std::strcmp(m.get_name(), "requestEndPause") != 0) {
            continue;
        }

        std::vector<std::string> ts{};

        for (auto* pt : m.get_param_types()) {
            ts.emplace_back(pt != nullptr ? pt->get_full_name() : "?");
        }

        if (ts.size() != 3 || ts[1] != "System.String" || ts[2] != "System.Action") {
            continue;   // die Option-Ueberladung braucht ein Objekt, das wir nicht haben
        }

        try {
            auto tctx = sdk::get_thread_context();
            m.call<void*>(tctx, pm, p_id,
                          sdk::VM::create_managed_string(utility::widen(MP_OWNER)),
                          nullptr);

            m_mp_freed++;
            m_mp_next = now + MP_COOL;
            m_mp_since.reset();

            re4vr::lua_set_number("__re4_merchant_pause_freed",
                                  static_cast<double>(m_mp_freed));
            re4vr::lua_set_string("__re4_merchant_pause_last", "aufgerufen");
        } catch (...) {
            re4vr::lua_set_string("__re4_merchant_pause_last", "Aufruf fehlgeschlagen");
        }

        return;
    }

    re4vr::lua_set_string("__re4_merchant_pause_last", "requestEndPause nicht gefunden");
}

// ============================================================================
// [FEUER-BEGRENZUNG 2026-07-23] Einzelschuss/Burst am NATIVEN Schuss statt am
// Trigger.
//
// GEMESSEN -- und der erste Versuch war falsch: ein PRE-Hook auf `execFire` mit
// SKIP_ORIGINAL bleibt WIRKUNGSLOS. Im Log steht auch warum:
// `BulletShellGenerator.requestFire` laeuft JEDES Mal rund eine Millisekunde
// VOR `execFire` -- das Projektil ist da also schon erzeugt. Ebenso wenig taugt
// ein Block am ShellGenerator selbst: dort waeren Munition und Sound bereits
// durch.
//
// RICHTIGER HEBEL ist die Frage, die das Spiel VOR dem Schuss stellt:
// `PlayerEquipment.isEnableFire`. Antwortet sie false, feuert die Engine nicht.
// Der Weg ist ausserdem frameunabhaengig, weil das Spiel selbst fragt, wann
// immer es feuern will.
//
// Diesen Befund NICHT wieder verwerfen.
// ============================================================================

void RE4VRBinding::install_hooks() {
    if (m_hooks_installed) {
        return;
    }

    m_hooks_installed = true;

    auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment");

    if (td == nullptr) {
        return;
    }

    // --- PlayerCondition_CheckGunFire.evaluate ---------------------------
    // [DAUERFEUER 15.09.2026] Die Signatur des Bugs steht fest (gemessen im
    // selben Lauf, re4_mgbug_dump.txt 12:xx):
    //   gesunder Schuss: CheckGunFire = 2 gefragt bei 2 Schuessen (1:1)
    //   Salve:           CheckGunFire = 0 gefragt bei 22 Schuessen
    // Die Schuesse der Salve entstehen also an der Torbedingung VORBEI -- der
    // Befehl kommt aus einem Motion-Event (SequenceTrackUpdater ->
    // CsInventoryController.useWeapon), ausgeloest durch einen Stagger.
    // ShootType/RapidBaseFrame/RapidSpeed sind dabei UNVERAENDERT (SemiAuto,
    // 20/2.0 = 0.167 s erlaubt, gefeuert wird alle 0.03 s).
    //
    // Deshalb hier nur ein Zeitstempel: wann wurde zuletzt regulaer gefragt?
    if (auto* ctd = sdk::find_type_definition("chainsaw.PlayerCondition_CheckGunFire");
        ctd != nullptr) {
        if (auto* cm = ctd->get_method("evaluate"); cm != nullptr) {
            g_hookman.add(
                cm,
                [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    g_checkgunfire_t = clock_now();

                    return HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
        }
    }

    // --- isEnableFire ----------------------------------------------------
    if (auto* m = td->get_method("isEnableFire"); m != nullptr) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t& retval, sdk::RETypeDefinition*, uintptr_t) {
                // [PUMP/EMPTY FRAMEUNABHAENGIG] Der Feuer-Block bei leerer oder
                // ungepumpter Waffe lief bisher NUR ueber RT (1x pro Frame).
                // Bei niedriger Bildrate rutscht ein Schuss durch, bevor RT
                // genullt wird -- man konnte die W-870 ohne Pump weiterfeuern.
                // isEnableFire wird PRO Schuss gefragt, ist also
                // frameunabhaengig.
                if (re4vr::lua_get_tribool("__vr_block_fire_when_empty") == 1
                    && re4vr::lua_get_tribool("__re4_frame_is_gameplay") == 1) {
                    retval = 0;

                    return;
                }

                // [DAUERFEUER-RIEGEL 15.09.2026] Ein Schuss, vor dem KEINE
                // CheckGunFire-Abfrage stand, ist ein Bug-Schuss: im gesunden
                // Fall fragt die Engine 1:1 vor jedem Schuss, in der Salve kein
                // einziges Mal. Gemessen ist der Takt dort 0.03-0.05 s, erlaubt
                // waeren 0.167 s -- ein legitimer Schuss kann also nie so kurz
                // hinter dem letzten liegen UND gleichzeitig ohne Abfrage
                // kommen.
                //
                // Vollautomaten bleiben unangetastet (fuer sie ist nicht
                // gemessen, ob die Engine dort ueberhaupt fragt): der Riegel
                // gilt nur, wenn die Waffe NICHT FullAuto ist.
                if (re4vr::lua_get_tribool("__re4_dauerfeuer_riegel") != 0) {
                    const double now = clock_now();

                    if ((now - g_checkgunfire_t) > 0.20
                        && (now - g_last_fire_t) < 0.12
                        && !gun_is_fullauto_local()) {
                        g_dauerfeuer_blocks++;
                        re4vr::lua_set_number("__re4_dauerfeuer_blocks",
                                              static_cast<double>(g_dauerfeuer_blocks));
                        retval = 0;   // false -> Engine feuert nicht

                        return;
                    }

                    g_last_fire_t = now;
                }

                if (re4vr::lua_get_tribool("__re4_burst_gate") != 1) {
                    return;
                }

                if (re4vr::lua_get_tribool("__vr_burst_active") != 1) {
                    return;
                }

                if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") != 1) {
                    return;
                }

                // losgelassen = frei
                if (re4vr::lua_get_tribool("__vr_burst_rt_down") != 1) {
                    return;
                }

                const double n = re4vr::lua_get_number("__vr_burst_count", 0.0);

                if (n <= 0.0) {
                    return;
                }

                // Neuer Trigger-Druck -> Startpunkt des Schuss-Zaehlers merken
                const double press = re4vr::lua_get_number("__vr_burst_press_id", 0.0);

                if (press != re4vr::lua_get_number("__vr_burst_seen_press", -1.0)) {
                    re4vr::lua_set_number("__vr_burst_seen_press", press);
                    re4vr::lua_set_number("__vr_burst_start_seq",
                                          re4vr::lua_get_number("__vr_shot_seq", 0.0));
                }

                const double fired = re4vr::lua_get_number("__vr_shot_seq", 0.0)
                    - re4vr::lua_get_number("__vr_burst_start_seq", 0.0);

                if (fired >= n) {
                    retval = 0;   // false -> Engine feuert nicht
                }
            });
    }

    // --- isEnableAutoReload ----------------------------------------------
    // [AUTO-RELOAD-BLOCK 2026-08-17] Playtester, Riot Gun: waehrend des
    // Shell-fuer-Shell-Ladens startet die ENGINE ihre eigene Ladeanimation und
    // wiederholt sie endlos. Der Weg dorthin ist der Feuerbefehl: geschossen
    // wird nichts (das Feuer-Gate oben faengt es ab), aber die Engine macht aus
    // einem Feuerbefehl auf eine nicht feuerbereite Waffe ihren AUTO-RELOAD.
    //
    // SCHARF NUR, solange eine von unseren Modulen verwaltete Waffe in der Hand
    // ist. Damit sind die Waffen, die den nativen Reload BRAUCHEN, automatisch
    // draussen (Red9/Top-Loader, Compound Bow).
    // KEIN Gameplay-Gate: genau in den HUD-aus- und Killswitch-Frames rutscht
    // der Trigger durch, dort muss der Block gelten.
    if (auto* m = td->get_method("isEnableAutoReload"); m != nullptr) {
        g_hookman.add(
            m,
            [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            },
            [](uintptr_t& retval, sdk::RETypeDefinition*, uintptr_t) {
                if (re4vr::lua_get_tribool("__re4_autoreload_gate") != 1) {
                    return;
                }

                if (re4vr::lua_get_tribool("__vr_manual_reload_consume_b") != 1) {
                    return;
                }

                retval = 0;   // false -> Engine laedt NICHT von selbst nach
                re4vr::lua_set_number(
                    "__re4_autoreload_blocked",
                    re4vr::lua_get_number("__re4_autoreload_blocked", 0.0) + 1.0);
            });
    }
}

// ============================================================================
// UI
// ============================================================================

// [SNAPTURN-UI] EINE Funktion, zwei Orte: der nackte Public-Bereich zeigt immer
// nur eine KOPIE dessen, was auch im Dev-Tree liegt. `sfx` haengt an jede
// ImGui-ID -- ohne eigene IDs behandelte ImGui die doppelten Labels als
// DASSELBE Element und der zweite Haken waere tot.
void RE4VRBinding::draw_snapturn_ui(const char* sfx) {
    const std::string id_st = std::string{"Enable Snapturn##st"} + sfx;

    if (g_framework->draw_menu_checkbox(id_st.c_str(), &m_prefs.enable_snapturn)) {
        // [2026-08-11] Hier stand eine Scharfschaltung von st_armed. Sie ist
        // ueberfluessig: der Laufzeit-Block schaltet selbst wieder scharf,
        // sobald der Stick Richtung Mitte geht.
        save_prefs();
    }

    if (!m_prefs.enable_snapturn) {
        return;
    }

    ImGui::Text("   Degree:");

    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt die Wahl, das gruene "[45]" fuer die aktive Gradzahl entfaellt.
    for (const int32_t d : {30, 45, 90}) {
        ImGui::SameLine();

        const std::string bid = std::to_string(d) + "##snapdeg" + sfx;

        if (g_framework->draw_menu_radio(bid.c_str(), m_prefs.snapturn_deg == d)) {
            m_prefs.snapturn_deg = d;
            save_prefs();
        }
    }

    // Die Schwelle ist eine Feinjustage und gehoert NUR in den Dev-Tree. Der
    // Wert dahinter ist derselbe -- was hier eingestellt wird, gilt also
    // automatisch auch fuer den Haken im nackten Public-Bereich.
    if (std::strcmp(sfx, "dev") == 0) {
        const std::string tid = std::string{"Ausloese-Schwelle##snapthr"} + sfx;

        if (ImGui::DragFloat(tid.c_str(), &m_prefs.snapturn_thresh, 0.01f, 0.30f, 0.99f,
                             "%.2f")) {
            save_prefs();
        }
    }
}

// [TURN SPEED 15.09.2026 -- Ansage des Users] Ein Slider, der dasselbe tut wie
// der Kamera-Regler im Spielmenue, aber ueber dessen Grenze hinaus. Gemessen
// (re4_optionid.txt 14:22, Regler von Hand verstellt): die Option heisst
// CameraNormalSpeed und hat die ID 9; das Menue laesst hoechstens 90 zu.
// Geschrieben wird ueber denselben Weg wie die Toxic Settings:
// OptionManager.setCurrentOptionValue(OptionID, Int32).
//
// Der Wert gehoert dem SPIEL -- es speichert ihn selbst. Deshalb wird hier
// nichts zwischengelagert: beim Zeichnen der aktuelle Stand gelesen, beim
// Schieben sofort gesetzt.
namespace {
constexpr int32_t CAMERA_NORMAL_SPEED_ID = 9;
constexpr int32_t TURN_SPEED_MAX = 130;

void draw_turn_speed_slider(const char* sfx) {
    auto* om = re4vr::fc::managed_singleton("chainsaw.OptionManager");

    if (om == nullptr) {
        return;
    }

    int32_t cur = -1;

    if (!re4vr::try_call<int32_t>(om, "getCurrentOptionValue(chainsaw.option.OptionID)", cur,
                                  CAMERA_NORMAL_SPEED_ID)
        || cur < 0) {
        return;   // Optionen noch nicht geladen -- dann keinen Regler zeigen
    }

    int32_t v = cur;

    if (ImGui::SliderInt((std::string{"Turn Speed##ts_"} + sfx).c_str(), &v, 0,
                         TURN_SPEED_MAX)) {
        re4vr::call_safe<void*>(
            om, "setCurrentOptionValue(chainsaw.option.OptionID, System.Int32)",
            CAMERA_NORMAL_SPEED_ID, v);
    }
}
}

// [PUBLIC-UI] Nacktes Haupt-UI (ohne Tree) -- dieselben PREFS-Werte wie im
// Dev-Tree, beide Haken zeigen also immer dasselbe. Steht seit dem 05.09. in
// einer eigenen Funktion, weil der nackte Teil OBEN bleibt und der Tree nach
// UNTEN zu den anderen gewandert ist.
void RE4VRBinding::draw_public_ui() {
    // Ganz oben, danach Luft und ein Trennstrich -- erst dann beginnt das
    // bisherige Bild (Ansage des Users).
    draw_turn_speed_slider("pub");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (g_framework->draw_menu_checkbox("Enable 180 degree turn", &m_prefs.enable_180_rotation)) {
        if (!m_prefs.enable_180_rotation) {
            qt_reset();
        }

        save_prefs();
    }

    draw_snapturn_ui("pub");


    // [PUBLIC-UI ROOMSCALE] Der Block liegt in RE4VRMovement, wo auch die
    // Werte und ihre Persistenz zu Hause sind -- gezeichnet wird er hier,
    // direkt unter Snapturn, genau wie in re4_vr_binding.lua Z.3158.
    if (auto mv = RE4VRMovement::get(); mv != nullptr) {
        mv->draw_public_roomscale();
    }
}

void RE4VRBinding::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Binding")) {
        return;
    }

    draw_turn_speed_slider("dev");

    // [TRACKPAD-SCROLL 15.09.2026 -- Ansage des Users] Das Pad liefert kleinere
    // Auslenkungen als der Stick; mit dem Stick-Tempo scrollt es zu zaeh.
    {
        float f = g_framework->get_vr_menu_trackpad_scale();

        if (ImGui::SliderFloat("RE4VR Trackpad Scroll##tpscroll", &f, 0.5f, 8.0f, "%.1fx")) {
            g_framework->set_vr_menu_trackpad_scale(f);
            m_prefs.trackpad_scroll = f;
            save_prefs();
        }
    }

    // [TRACKPAD-DIAGNOSE 15.09.2026] Das Scrollen im eigenen Menue kam unter
    // OpenVR nicht an. Hier die ROHWERTE, am Desktop ablesbar: kommt beim
    // Wischen nichts an, liegt es an der Bindung/dem Action-Handle; kommen
    // Werte an, liegt es an der Auswertung im Menue.
    {
        auto& vr = VR::get();

        if (vr != nullptr) {
            const auto pad = vr->get_right_touchpad_axis();
            const bool klick_l = vr->is_action_active(vr->get_action_touchpad_click(),
                                                      vr->get_left_joystick());
            const bool klick_r = vr->is_action_active(vr->get_action_touchpad_click(),
                                                      vr->get_right_joystick());

            ImGui::Text("Trackpad rechts: x=%.2f y=%.2f | Klick L=%d R=%d | Handle=%s",
                        pad.x, pad.y, klick_l ? 1 : 0, klick_r ? 1 : 0,
                        (vr->get_action_touchpad() == vr::k_ulInvalidActionHandle)
                            ? "UNGUELTIG" : "ok");

            // [DIAGNOSE 15.09.2026] Der Press kommt unter OpenXR nicht an.
            // -1 = keine OpenXR-Runtime, -2 = Aktion nicht gebunden, sonst die
            // rohe Kraft. "press" ist das, was die Gesten tatsaechlich sehen.
            ImGui::Text("Trackpad Kraft: L=%.2f R=%.2f | press L=%d R=%d | ForceHandle=%s",
                        vr->get_touchpad_force(VRRuntime::Hand::LEFT),
                        vr->get_touchpad_force(VRRuntime::Hand::RIGHT),
                        vr->is_touchpad_pressed(VRRuntime::Hand::LEFT) ? 1 : 0,
                        vr->is_touchpad_pressed(VRRuntime::Hand::RIGHT) ? 1 : 0,
                        (vr->get_action_touchpad_force() == vr::k_ulInvalidActionHandle)
                            ? "UNGUELTIG" : "ok");
        }
    }

    // [LA_TIMING] Links-A Tap/Hold-Zeiten.
    if (ImGui::SliderFloat("Links-A: Longpress ab (s)##la_long", &m_prefs.long_press_sec, 0.2f,
                           3.0f)) {
        save_prefs();
    }

    if (ImGui::SliderFloat("Links-A: Tap mindestens (s)##la_short", &m_prefs.short_min_sec, 0.0f,
                           0.5f)) {
        save_prefs();
    }

    ImGui::Separator();
    draw_snapturn_ui("dev");
    ImGui::Separator();

    if (m_prefs.enable_180_rotation) {
        if (ImGui::SliderFloat("180: Doppel-Tipp-Fenster (s)##t180w",
                               &m_prefs.turn180_window_sec, 0.05f, 0.50f)) {
            save_prefs();
        }

        if (ImGui::SliderFloat("180: Drehdauer (s)##t180d", &m_prefs.turn180_sec, 0.0f, 0.60f)) {
            save_prefs();
        }
    }

    // ------------------------------------------------------------------
    // [CAPACITIVE 2026-09-09] Grip-Empfindlichkeit. Gilt NUR unter OpenXR:
    // unter OpenVR liest das Index-Profil den Kraftsensor schon richtig, dort
    // fasst RE4VRCapacitive bewusst nichts an.
    // Die Werte gehoeren RE4VRCapacitive, das sie alle zwei Sekunden
    // nachschiebt (REFramework stellt beim Config-Neuladen sonst seine eigenen
    // Defaults wieder her). Deshalb hier ueber set_cfg schreiben -- von Hand im
    // VR-Tree des Frameworks verstellte Werte waeren nach dem naechsten Takt
    // wieder weg.
    ImGui::Separator();

    if (ImGui::TreeNode("Capacitive (Grip-Empfindlichkeit, nur OpenXR)")) {
        auto& cap = RE4VRCapacitive::get();

        if (cap == nullptr) {
            ImGui::Text("Modul nicht geladen.");
        } else {
            bool use_analog = cap->get_use_analog();
            bool prefer_force = cap->get_prefer_force();
            auto press = static_cast<float>(cap->get_press());
            auto release = static_cast<float>(cap->get_release());

            bool dirty = false;

            dirty |= ImGui::Checkbox("Eigene Schwelle statt Runtime##cap_ana", &use_analog);
            dirty |= ImGui::Checkbox("Kraftsensor bevorzugen (Index)##cap_force", &prefer_force);
            dirty |= ImGui::SliderFloat("Greifen ab##cap_press", &press, 0.05f, 0.95f, "%.2f");
            dirty |= ImGui::SliderFloat("Loslassen unter##cap_rel", &release, 0.05f, 0.95f, "%.2f");

            ImGui::Text("Standard: greifen 0.30 / loslassen 0.25");
            ImGui::Text("Loslassen wird nie ueber Greifen gelassen (sonst klemmt der Griff).");

            if (dirty) {
                cap->set_cfg(use_analog, prefer_force, static_cast<double>(press),
                             static_cast<double>(release));
            }
        }

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRBinding::on_initialize() {
    return Mod::on_initialize();
}

void RE4VRBinding::on_lua_state_created(sol::state& lua) {
    if (!m_cfg_loaded) {
        m_cfg_loaded = true;
        load_prefs();
    }

    g_player_cam_td = sdk::find_type_definition("chainsaw.PlayerCameraController");
    g_gimmick_fix_td = sdk::find_type_definition("chainsaw.GimmickFixCameraController");
    g_sound_container_td = sdk::find_type_definition("soundlib.SoundContainer");

    install_hooks();

    // Startwerte, die die Lua beim Laden setzt.
    lua["vr_controller_type"] = "index";

    // Knife/Grenade-Globals, die andere Module lesen. Luas Muster ist
    // `if x == nil then x = false end` -- also nur setzen, wenn es fehlt.
    for (const char* g : {"vr_knife_swing", "vr_grenade_throw", "vr_is_grenade_equipped",
                          "vr_holster_knife", "vr_knife_equip", "vr_knife_active",
                          "__vr_melee_physical_active"}) {
        if (!lua[g].valid()) {
            lua[g] = false;
        }
    }

    // [GIGANTE-RT] NOT-AUS: auf false -> Verhalten exakt wie vor dem 30.08.
    re4vr::lua_set_bool("__re4_gigante_rt", true);

    // Die drei Gate-Globals der nativen Hooks (Not-Aus + Diagnose).
    re4vr::lua_set_bool("__re4_burst_gate", true);
    re4vr::lua_set_bool("__re4_autoreload_gate", true);
    re4vr::lua_set_number("__re4_autoreload_blocked", 0.0);

    // [PLAYWORK-PAUSE] Aus: __re4_playwork_pause_notaus = false
    re4vr::lua_set_bool("__re4_playwork_pause_notaus", true);
    re4vr::lua_set_number("__re4_playwork_pause_freed", 0.0);
    re4vr::lua_set_string("__re4_playwork_pause_last", "-");
}

void RE4VRBinding::on_lua_state_destroyed(sol::state& lua) {
    // 1:1 der re.on_script_reset-Block der Lua.
    m_inited = false;
    m_prev_l_grip = false;
    m_grenade_throw_cooldown = 0;
    m_grenade_rt_pulse_frames = 0;
    m_prev_vr_grenade_throw = false;

    m_la.pressed = false;
    m_la.frames = 0;
    m_la.fired_long = false;
    m_la.short_timer = 0;
    m_la.back_guard = false;
    // [KS_X_GUARD] halb gesetzten Latch nicht ueber Reset Scripts schleppen
    m_la.ks_x_guard = false;

    m_was_active = false;
    m_lt_b_overlay_was = false;
    m_ref_overlay_synced = false;
    m_ref_overlay_tick = 0;

    // [QUICKTURN_180] halb gelaufene Doppel-Tipp-Phase nicht mitschleppen
    qt_reset();

    m_dual_trigger_start_clock.reset();
    m_dual_trigger_fired = false;
    m_dual_trigger_gap_clock.reset();
    m_start_hold_timer = 0;

    m_ee_flame_clock.reset();
    m_ee_flame_fired = false;
    m_ee_flame_gap_clock.reset();

    m_edge_r_a = Edge{};
    m_edge_r_b = Edge{};
    m_edge_r_jc = Edge{};
    m_edge_l_a_b = Edge{};

    m_cfg_loaded = false;
}

#endif // RE4
