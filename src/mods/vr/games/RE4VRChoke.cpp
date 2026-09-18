// ============================================================================
// RE4VRChoke -- 1:1-Portierung von re4_vr_choke.lua. Siehe RE4VRChoke.hpp fuer
// die Bausteine und die zwingende Reihenfolge im Mod-Vektor (VOR RE4VRMotion).
//
// Spezifikation: I:\LUATRANS\PORT_CHOKE_SPEC.md
// ============================================================================
#if defined(RE4)

#include <algorithm>
#include <sdk/SceneManager.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <tuple>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VRWeapons2.hpp"
#include "RE4VRMotion.hpp"
#include "RE4VRArmChain.hpp"
#include "RE4VRAshleyMouth.hpp"
#include "RE4VRChoke.hpp"
#include "RE4VRWeapons.hpp"

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

// getMaterialFloat4(mi, vi) -- zwei Indizes vor dem sret-Puffer.
bool get_mat_vec4(::REManagedObject* obj, std::string_view name, int32_t mi, int32_t vi,
                  glm::vec4& out) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{};
    bool ok = false;

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj, mi, vi);
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

bool set_mat_vec4(::REManagedObject* obj, std::string_view name, int32_t mi, int32_t vi,
                  const glm::vec4& v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf = v;
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, mi, vi, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

bool set_mat_float(::REManagedObject* obj, std::string_view name, int32_t mi, int32_t vi,
                   float v) {
    const auto method = find_method(obj, name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    bool ok = false;

    try {
        method->call_safe<void*>(context, obj, mi, vi, v);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// Luas `Vector3f.new(x,y,z)` an eine via.vec4-Signatur.
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

// addDamage nimmt je nach Build System.Int32 oder System.Single. Der Typ wird
// aus der TDB gelesen statt geraten.
void add_damage(::REManagedObject* hp, float dmg) {
    const auto method = find_method(hp, "addDamage");

    if (method == nullptr) {
        return;
    }

    bool as_float = false;

    if (const auto params = method->get_param_types(); !params.empty()) {
        const auto name = params[0] != nullptr ? params[0]->get_full_name() : std::string{};
        as_float = (name == "System.Single") || (name == "System.Double");
    }

    auto context = sdk::get_thread_context();

    try {
        if (as_float) {
            method->call_safe<void*>(context, hp, dmg);
        } else {
            method->call_safe<void*>(context, hp, static_cast<int32_t>(dmg));
        }
    } catch (...) {
    }

    clear_pending(context, true);
}

// [ABSTURZ 04.09.2026 -- aus dem Dump belegt] Ein Getter, der ein ENUM oder
// einen anderen ValueType liefert, gibt eine ZAHL zurueck. Holt man sein
// Ergebnis als `REManagedObject*`, steht dort die Zahl selbst als "Zeiger" --
// bei chainsaw.ActionState.get_Category ist das die Kategorie 3. Jede
// Dereferenzierung darauf ist eine Access Violation:
//     mov rax,[rcx]   ds:0000000000000003=????
// In Lua ist derselbe Fall harmlos: dort kommt eine Zahl an, und der folgende
// `cat_obj:get_type_definition()` scheitert still im safe().
//
// Deshalb NIE blind als Objekt behandeln: erst den Rueckgabetyp aus der TDB
// lesen. `is_value_type` heisst hier "das Ergebnis ist ein Wert, kein Objekt".
bool getter_returns_object(::REManagedObject* obj, std::string_view name) {
    const auto m = find_method(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto* rt = m->get_return_type();

    if (rt == nullptr) {
        return false;
    }

    return !rt->is_value_type();
}

// [LUA-WAHRHEIT] `safe(function() return o:call("get_Valid") end) == false`.
// Ein FEHLGESCHLAGENER Aufruf liefert in Lua nil und gilt damit als GUELTIG --
// nur ein echtes false sperrt. Deshalb try_call + strikter Vergleich.
bool valid_not_false(::REManagedObject* obj) {
    if (obj == nullptr) {
        return false;
    }

    bool v = false;

    if (!re4vr::try_call<bool>(obj, "get_Valid", v)) {
        return true;   // nil -> in Lua wahr
    }

    return v;
}

// Dasselbe Muster fuer beliebige bool-Getter: liefert nullopt bei nil.
template <typename... Args>
std::optional<bool> opt_bool(::REManagedObject* obj, std::string_view name, Args... args) {
    bool v = false;

    if (!re4vr::try_call<bool>(obj, name, v, args...)) {
        return std::nullopt;
    }

    return v;
}

template <typename... Args>
std::optional<int32_t> opt_int(::REManagedObject* obj, std::string_view name, Args... args) {
    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(obj, name, v, args...)) {
        return std::nullopt;
    }

    return v;
}

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

bool set_parent_joint(::REManagedObject* tf, const char* name) {
    if (tf == nullptr) {
        return false;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return false;
    }

    const auto method = find_method(tf, "set_ParentJoint");

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    bool ok = false;

    try {
        method->call_safe<void*>(context, tf, str);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

std::string managed_string_of(::REManagedObject* s) {
    if (s == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(s));
    } catch (...) {
        return {};
    }
}

std::string obj_name_of(::REManagedObject* go) {
    if (go == nullptr) {
        return {};
    }

    return managed_string_of(re4vr::call_safe<::REManagedObject*>(go, "get_Name"));
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

::REManagedObject* type_of(const char* name) {
    auto* td = sdk::find_type_definition(name);

    return td != nullptr ? (::REManagedObject*)td->get_runtime_type() : nullptr;
}

::REManagedObject* get_component(::REManagedObject* go, ::REManagedObject* t) {
    // [NIL-TYPE-GUARD] getComponent NIE mit nil-Type -- das wirft eine
    // Game-Exception, die den naechsten fremden Aufruf reisst.
    if (go == nullptr || t == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", t);
}

// [FRAME-CACHE] Ueber re4vr::fc -- der Singleton-Lookup ist der teuerste Teil
// der Spieler-Kette und lief bisher bei JEDEM Aufruf neu (in apply_hold & Co.
// fuenfmal pro Frame). Verhalten unveraendert: der Cache haelt nichts ueber den
// Frame hinaus und laesst sich mit __re4_fc_off abschalten.
::REManagedObject* character_manager() {
    return re4vr::fc::managed_singleton("chainsaw.CharacterManager");
}

// [FRAME-CACHE] s. character_manager().
::REManagedObject* player_ctx() {
    return re4vr::fc::ctx();
}

float horiz(const glm::vec3& a, const glm::vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;

    return std::sqrt(dx * dx + dz * dz);
}

constexpr float PI_F = 3.14159265358979323846f;

float deg2rad(float d) {
    return d * PI_F / 180.0f;
}

glm::quat q_axis(float deg, float ax, float ay, float az) {
    const float h = deg2rad(deg) * 0.5f;
    const float s = std::sin(h);

    return glm::quat{std::cos(h), ax * s, ay * s, az * s};
}

glm::quat yaw_quat(float yaw) {
    const float h = yaw * 0.5f;

    return glm::quat{std::cos(h), 0.0f, std::sin(h), 0.0f};
}

// [STECK-MESSUNG] Begrenzt mitschreiben -- Diagnose darf nicht endlos wachsen.
// Reicht das nicht, steht die Ursache ohnehin woanders.
int g_stick_logged = 0;
constexpr int STICK_LOG_MAX = 40;

constexpr const char* CFG_PATH = "re4_vr/re4_vr_choke.json";

// ---- [ACHIEVEMENT 13.09.2026] "WHAT A BAT JOKE" -------------------------
// Eigene Datei: re4_vr_splash.json wird beim Schreiben ganz ersetzt, ein
// zweiter Schluessel darin waere beim naechsten Start weg.
constexpr const char* ACHIEVEMENT_CFG_PATH = "re4_vr/re4_vr_achievement.json";

// [ACHIEVEMENT 2 -- 15.09.2026] Eigene Datei, wie angesagt: der Stich-Zaehler
// und die Freischaltung des Schlangen-Chokes stehen fuer sich.
constexpr const char* ACHIEVEMENT2_CFG_PATH = "re4_vr/re4_vr_achievement2.json";
// Wartezeit zwischen dem Griff und der Tafel (Ansage 13.09.2026: rund 1 s).
constexpr double ACHIEVEMENT_DELAY = 1.0;

// [ASHLEY-MESSER 15.09.2026] Das WIEVIELTE Messer die Tafel ausloest.
constexpr int32_t ASHLEY_STAB_GOAL = 3;
// Standzeit der Tafel; der Countdown darin zaehlt von dieser Zahl herunter.
constexpr double ACHIEVEMENT_SECONDS = 6.0;

// [STAGGER-FENSTER 13.09.2026] So lange darf der Rueckwechsel hoechstens auf
// einen Gameplay-Frame warten. Eine Trefferreaktion ist nach ein bis zwei
// Sekunden durch; Tod und Ladevorgang dauern laenger und fallen damit heraus.
constexpr double REEQUIP_MAX_WAIT = 6.0;

// [TEST 13.09.2026 -- Ansage "zum Testen am Anfang zeigen"] true = die Tafel
// kommt bei JEDEM Spielstart einmal von selbst (ACHIEVEMENT_TEST_AFTER
// Sekunden nach dem ersten Choke-Tick), damit man sie nicht erst in der
// Kanalisation suchen muss. Fuer die Veroeffentlichung wieder auf false --
// die Fledermaus-Erkennung selbst haengt NICHT daran.
constexpr bool ACHIEVEMENT_TEST_AT_START = false;

// ---- [ASHLEY 13.09.2026] Die Begleitung greifen -------------------------
// GEMESSEN am 13.09. (zzz_re4_ashley_probe, Lauf 1+2): die PartnerContextList
// haelt genau EINEN Eintrag, chainsaw.PartnerBaseContext mit dem Body
// "ch2a1z0_body" -- das ist Ashley als BEGLEITUNG. Der Name ch0a1z0_body aus
// RE4VRMaterials gehoert dagegen der SPIELBAREN Ashley (KS3); beide stehen
// hier, damit der Griff in keinem Abschnitt ins Leere sucht.
// [FLEDERMAUS-FAHRER 13.09.2026] Alles, was ihre Bewegung schreibt, wird fuer
// die Griffdauer abgeschaltet und beim Loslassen zurueckgegeben.
//
// GEMESSEN (zzz_re4_fledermaus_check.lua, Komponenten-Dump): SequenceTrackUpdater
// und MotionFsm2 allein genuegten NICHT -- sie flog trotzdem weiter (0,35 s nach
// dem Griff 6,6 m hinter der Hand, Griff-Log 23:54:09). Also kommen ihre eigene
// Logik (GmBat) und die Tier-Hilfe (AnimalUtility) dazu; beide stehen laut Dump
// auf enabled=true. via.motion.Motion bleibt AN -- sonst friert das Mesh ein
// (ohne Skelett wird es unsichtbar, s. Klon-Bau in RE4VRWeapons2).
// [ALLES STILL 13.09.2026 -- Ansage "leg doch alles still wenn ich zupacke"]
// Einzelne Komponenten abzuschalten hat nicht gereicht (SequenceTrackUpdater +
// MotionFsm2 waren aus, sie flog trotzdem 6,6 m weit weiter). Also wird beim
// Griff JEDE Komponente ihres GameObjects abgeschaltet -- ausser den dreien,
// die sie ueberhaupt sichtbar in der Hand halten:
//
//   via.Transform      -- daran haengen wir sie; ohne ihn gibt es keine Lage
//   via.render.Mesh    -- das sichtbare Modell
//   via.motion.Motion  -- ohne Skelett wird das Mesh unsichtbar (dieselbe
//                         Erfahrung wie beim Klon-Bau in RE4VRWeapons2)
//
// Beim Loslassen bekommt jede ihren alten Zustand zurueck, sie fliegt also
// normal weiter. Gilt NUR fuer die Fledermaus.
constexpr const char* BAT_KEEP[] = {
    "via.Transform",
    "via.render.Mesh",
    "via.motion.Motion",
};

// [ASHLEY ANTWORTET 16.09.2026] Wann gilt eine Geste IHR? Gemessen wird der
// BLICK, nicht der Finger.
//
// Der erste Anlauf nahm die Fingerrichtung (erstes zu letztem Glied). Live
// gemessen (zzz_re4_ashley_reply_probe.lua, acht Gesten direkt vor ihrem
// Gesicht) lag sie dabei NIE unter 85 Grad, meist bei 95-165: beim
// Stinkefinger haelt man die Hand hoch, die Fingerspitze zeigt nach oben und
// nicht auf die Person. Der Blick lag in denselben Messungen bei 8 bis 22
// Grad -- auch noch auf sechs Meter.
constexpr float REPLY_ANGLE_DEG = 30.0f;   // Kegel um die Blickrichtung
constexpr float REPLY_RANGE_M = 15.0f;     // weiter weg reagiert sie nicht

// Lautstaerke nach Abstand. REPLY_FULL_M ist der Radius, in dem sie so laut
// klingt wie beim Griff -- also genau so, wie der dB-Regler eingestellt ist.
// Weiter weg kommt ein ABZUG obendrauf, hoechstens REPLY_MIN_DB.
constexpr float REPLY_FULL_M = 2.0f;
constexpr float REPLY_MIN_DB = -30.0f;

constexpr const char* ASHLEY_BODY_NPC = "ch2a1z0_body";
constexpr const char* ASHLEY_BODY_PLAYABLE = "ch0a1z0_body";
constexpr double ACHIEVEMENT_TEST_AFTER = 8.0;

// ---- Konstanten aus dem Lua-Kopf (keine UI, keine Config) ----------------
constexpr const char* PARENT_JOINT = "L_Palm";
constexpr const char* NECK_JOINT = "Neck_1";
constexpr const char* NECK_ALT = "Neck_0";

// [STICK-KLON] Obergrenze fuer die Standzeit der Anzeige-Kopie im Gegner.
// Sie soll bis zum Tod stecken bleiben duerfen -- aber nie fuer immer,
// sonst bleibt bei einem Gegner, der nie despawnt, ein Objekt zurueck.
constexpr double STICK_CLONE_MAX_S = 60.0;
constexpr const char* LEON_BODY = "ch0a0z0_body";
// [ADA-MAUS 17.09.2026] Adas Spielerkoerper in Separate Ways (s. RE4VRMotion).
constexpr const char* ADA_BODY = "ch3a8z0_body";

constexpr uint32_t KNIFE_HIT_SND = 238304172u;
constexpr uint32_t KNIFE_STUCK_SND = 1489546289u;
constexpr uint32_t GRAB_SOUND = 198619862u;
const std::array<uint32_t, 3> ENEMY_GRAB_SOUNDS{4280093339u, 4246125810u, 3929631921u};
constexpr double ENEMY_SND_DELAY = 0.25;

// [ASHLEY-LINE 16.09.2026 -- Ansage "bei jedem choke kommt was"] Ihre eigenen
// Sprachzeilen aus dem Spiel, gefunden ueber #soundplayer.lua ->
// "Player Voice" -> Ashley (Ch2a1z0). Ablauf bei JEDEM Griff an ihr:
//   1. ASHLEY_CHOKE_LINE -- kommt IMMER, kurz nach dem Griff.
//   2. danach entweder einer unserer eigenen WAVs (nur alle 4-6 Griffe)
//      oder eine aus ASHLEY_CHOKE_LINES2 im Shuffle. Eines von beiden
//      IMMER -- es bleibt nie bei der ersten Zeile allein.
constexpr uint32_t ASHLEY_CHOKE_LINE = 1745997412u;

const std::array<uint32_t, 7> ASHLEY_CHOKE_LINES2{
    1735904227u, 1773445295u, 2134929579u, 2457096359u,
    2208803161u, 4013546536u, 4140913716u,
};

// "ein ganz wenig delay" -- der Griff sitzt, dann sagt sie es.
constexpr double ASHLEY_LINE_DELAY = 0.20;

// Abstand zur zweiten Zeile ("direkt danach"). Gerechnet ab dem GRIFF, nicht
// ab dem Ende ihrer Zeile -- deren Laenge kennt niemand, Wwise meldet sie
// nicht zurueck.
//
// [16.09.2026 -- Ansage "die 174 ist nur ein kurzes stoehnen"] Unser eigener
// WAV-Spruch kommt darum dichter dran als die Fuellzeile.
constexpr double ASHLEY_WAV_GAP = 0.60;
constexpr double ASHLEY_LINE2_GAP = 1.00;

// Shuffle-Beutel ueber ASHLEY_CHOKE_LINES2 -- dieselbe Bauform wie
// re4vr::ashley_wav_next (Fisher-Yates, gezogen wird von hinten, an der
// Beutelgrenze nie zweimal dieselbe). Lokal, weil der Pool aus fertigen
// Wwise-IDs besteht und nicht aus WAV-Indizes.
uint32_t ashley_line_next(std::vector<int>& bag, int* last) {
    if (bag.empty()) {
        for (int i = 0; i < static_cast<int>(ASHLEY_CHOKE_LINES2.size()); ++i) {
            bag.push_back(i);
        }

        for (size_t i = bag.size(); i >= 2; --i) {
            const size_t j = static_cast<size_t>(std::rand() % static_cast<int>(i));
            std::swap(bag[i - 1], bag[j]);
        }

        if (last != nullptr && *last >= 0 && bag.size() > 1 && bag.back() == *last) {
            std::swap(bag.back(), bag.front());
        }
    }

    const int idx = bag.back();
    bag.pop_back();

    if (last != nullptr) {
        *last = idx;
    }

    return ASHLEY_CHOKE_LINES2[static_cast<size_t>(idx)];
}

// Gemessen am Gegner: nach einem Parry steht er in diesem ActionState.
const std::array<uint32_t, 2> PARRY_ACTIONS{4291272998u, 2472229444u};
// Am Gegner gemessen, waehrend Gui_ui2141 ueber ihm stand.
const std::array<uint32_t, 2> GRABBABLE_ACTIONS{541602114u, 3600067305u};

const std::array<const char*, 6> TORSO_JOINTS{
    "Hip", "Spine_0", "Spine_1", "Spine_2", "Neck_0", "Neck_1",
};

// [MESSER-PFLICHT] Dieselben IDs wie in re4_vr_holster.lua.
const std::array<int32_t, 8> KNIFE_IDS{5000, 5001, 5002, 5003, 5006, 6107, 6108, 6305};
const std::array<const char*, 8> KNIFE_WP{
    "wp5000", "wp5001", "wp5002", "wp5003", "wp5006", "wp6107", "wp6108", "wp6305",
};

bool is_knife_wp(const std::string& nm) {
    for (const char* k : KNIFE_WP) {
        if (nm == k) {
            return true;
        }
    }

    // Der Name kann ein _AO/_MC-Suffix tragen -> Praefix-Vergleich ueber die
    // ersten sechs Zeichen, exakt wie `nm:sub(1, 6)` in Lua.
    if (nm.size() >= 6) {
        const std::string pre = nm.substr(0, 6);

        for (const char* k : KNIFE_WP) {
            if (pre == k) {
                return true;
            }
        }
    }

    return false;
}

bool is_knife_id(int32_t id) {
    for (int32_t k : KNIFE_IDS) {
        if (k == id) {
            return true;
        }
    }

    return false;
}

bool in_list(const std::array<uint32_t, 2>& list, uint32_t v) {
    return list[0] == v || list[1] == v;
}

// Handpose der linken Hand im Griff -- mit #Guestures aufgenommen und BEWUSST
// als Zahlen hinterlegt (die "#"-Scripte sind im Release nicht dabei).
struct PoseEntry {
    const char* joint;
    float w, x, y, z;
};

const std::array<PoseEntry, 16> CHOKE_POSE{{
    {"L_Thumb1", 0.986413f, 0.114720f, 0.050566f, -0.106168f},
    {"L_Thumb2", 0.983978f, 0.007059f, -0.178136f, 0.002266f},
    {"L_Thumb3", 0.990069f, 0.000000f, 0.140585f, 0.000000f},
    {"L_IndexF1", 0.999466f, 0.001501f, 0.001655f, -0.032605f},
    {"L_IndexF2", 0.983340f, 0.000000f, 0.000000f, -0.181774f},
    {"L_IndexF3", 0.995108f, 0.000000f, 0.000000f, -0.098795f},
    {"L_MiddleF1", 0.998105f, 0.002412f, 0.013136f, -0.060075f},
    {"L_MiddleF2", 0.926335f, 0.000000f, 0.000000f, -0.376702f},
    {"L_MiddleF3", 0.968103f, 0.000000f, 0.000000f, -0.250552f},
    {"L_RingF1", 0.998131f, 0.000919f, 0.011039f, -0.060093f},
    {"L_RingF2", 0.903925f, 0.000000f, 0.000000f, -0.427691f},
    {"L_RingF3", 0.960472f, 0.000000f, 0.000000f, -0.278378f},
    {"L_PinkyF1", 0.996054f, 0.003005f, 0.007690f, -0.088365f},
    {"L_PinkyF2", 0.889663f, 0.000000f, 0.000000f, -0.456618f},
    {"L_PinkyF3", 0.966529f, 0.000000f, 0.000000f, -0.256557f},
    {"L_Palm", 1.000000f, -0.000000f, -0.000000f, -0.000000f},
}};

// [DAMAGE-KATEGORIE] Der Enum-Wert wird NICHT geraten, sondern beim ersten
// echten Kategorie-Objekt aus dessen Typ gelesen.
const std::array<const char*, 4> CAT_DAMAGE_NAMES{"Damage", "Damaged", "Hit", "Hitting"};
} // namespace

std::shared_ptr<RE4VRChoke>& RE4VRChoke::get() {
    static auto inst = std::make_shared<RE4VRChoke>();
    return inst;
}

// ============================================================================
// Handles
// ============================================================================

void RE4VRChoke::store(Handle& h, ::REManagedObject* o, bool force_ref) {
    if (h.obj == o) {
        return;
    }

    drop(h);

    if (o == nullptr) {
        return;
    }

    h.obj = o;
    h.reffed = false;

    // [SELBST ERZEUGT 2026-09-10] Die Heuristik unten haelt ein frisch
    // erzeugtes GameObject fuer nicht haltenswert -- sein referenceCount ist
    // 0. Genau dafuer gibt es force_ref: dort wird bedingungslos gepinnt.
    if (force_ref) {
        if (utility::re_managed_object::is_managed_object(o)) {
            utility::re_managed_object::add_ref(o);
            h.reffed = true;
        }

        return;
    }

    if (utility::re_managed_object::is_managed_object(o)
        && static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        h.reffed = true;
    }
}

void RE4VRChoke::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

// ============================================================================
// Typen -- sdk.typeof laeuft in Lua beim LADEN, hier beim ersten Gebrauch.
// (In Lua steht T_MOTION/T_SNDC ganz oben; ein Fehlschlag dort haette das
// Script getoetet, hier faellt nur die jeweilige Abfrage aus.)
// ============================================================================

void RE4VRChoke::ensure_types() {
    if (m_types_ready) {
        return;
    }

    m_types_ready = true;
    m_t_motion = type_of("via.motion.Motion");
    m_t_sndc = type_of("soundlib.SoundContainer");
    m_t_mesh = type_of("via.render.Mesh");
    // Reihenfolge 1:1: IkLeg2, IkLeg, GroundAdsorber.
    m_t_ik[0] = type_of("via.motion.IkLeg2");
    m_t_ik[1] = type_of("via.motion.IkLeg");
    m_t_ik[2] = type_of("chainsaw.GroundAdsorber");
}

// ============================================================================
// [ACHIEVEMENT 13.09.2026] Erste gegriffene Fledermaus
// ============================================================================
// Merkt den Griff SOFORT auf der Platte und stellt die Tafel eine Sekunde
// spaeter an. Zweimal soll sie nie kommen -- weder im selben Spielstart
// (m_achievement_seen) noch im naechsten (die JSON).
void RE4VRChoke::achievement_arm() {
    if (!m_achievement_checked) {
        m_achievement_checked = true;

        // [FALLE, s. splash_tick] json_load liefert bei FEHLENDER Datei ein
        // leeres Objekt -- es entscheidet allein der Schluessel.
        const auto j = re4vr::json_load(ACHIEVEMENT_CFG_PATH);
        m_achievement_seen = j.is_object() && j.contains("bat_choke");
    }

    if (m_achievement_seen || m_achievement_due > 0.0) {
        return;
    }

    m_achievement_seen = true;
    m_achievement_due = clock_now() + ACHIEVEMENT_DELAY;

    // [ASHLEY-MESSER 15.09.2026] Erst LADEN, dann ergaenzen: hier stand ein
    // frisches Objekt, das die ganze Datei ueberschrieben hat -- der
    // Stich-Zaehler daneben waere bei jedem Schreiben verloren gegangen.
    auto j = re4vr::json_load(ACHIEVEMENT_CFG_PATH);

    if (!j.is_object()) {
        j = nlohmann::json::object();
    }

    j["bat_choke"] = true;
    re4vr::json_save(ACHIEVEMENT_CFG_PATH, j);
}

// ============================================================================
// [ASHLEY-MESSER 15.09.2026] Drei Messer in Ashley -- das dritte gibt die Tafel
// ============================================================================
// Eigene Datei (re4_vr_achievement2.json), damit Zaehler und Freischaltung fuer
// sich stehen. Das dritte Messer setzt "snake_choke" und stellt die zweite Tafel
// an -- genau einmal, weil der Schluessel danach in der Datei steht.
//
// [16.09.2026] Der Schluessel heisst weiter "snake_choke", GIBT den Griff aber
// nicht mehr frei: der Schlangen-Choke ist seit 16.09. ganz ausgebaut. Die Tafel
// bleibt, womit sie kuenftig belohnt wird, ist offen.
void RE4VRChoke::ashley_stab() {
    auto j = re4vr::json_load(ACHIEVEMENT2_CFG_PATH);

    if (!j.is_object()) {
        j = nlohmann::json::object();
    }

    if (m_ashley_stabs < 0) {
        m_ashley_stabs = (j.contains("ashley_stabs") && j["ashley_stabs"].is_number_integer())
            ? j["ashley_stabs"].get<int32_t>() : 0;
    }

    ++m_ashley_stabs;
    j["ashley_stabs"] = m_ashley_stabs;

    re4vr::lua_set_number("__re4_ashley_stabs", static_cast<double>(m_ashley_stabs));

    // [HAERTUNG 16.09.2026] Ohne die Datei des ersten Achievements gibt es das
    // zweite nicht -- der Zaehler laeuft weiter, die Freischaltung wartet.
    if (m_ashley_stabs >= ASHLEY_STAB_GOAL && !j.contains("snake_choke") && achievement_unlocked()) {
        j["snake_choke"] = true;

        m_snake_checked = true;
        m_snake_unlocked = true;

        // Dieselbe Verzoegerung wie beim ersten: Tafel und Ton kommen eine
        // Sekunde nach dem Stich, nicht mitten in der Bewegung.
        if (m_achievement_due <= 0.0) {
            m_achievement_due = clock_now() + ACHIEVEMENT_DELAY;
            m_achievement_which = 2;
        }
    }

    re4vr::json_save(ACHIEVEMENT2_CFG_PATH, j);
}

// [DREI MESSER 15.09.2026, umgewidmet 16.09.2026] Sagt, ob das zweite
// Achievement steht. Es gab frueher den Schlangen-Griff frei (der ist seit
// 16.09. ausgebaut) und schaltet stattdessen Ashleys ANTWORTEN frei: nur wenn das
// hier true ist, gibt es den Regler im Menue und reagiert sie auf eine Geste.
// Der JSON-Schluessel heisst weiter "snake_choke" -- wer es schon hat, soll es
// nicht durch eine Umbenennung verlieren.
bool RE4VRChoke::reply_unlocked() {
    if (!m_snake_checked) {
        m_snake_checked = true;

        const auto j = re4vr::json_load(ACHIEVEMENT2_CFG_PATH);
        m_snake_unlocked = j.is_object() && j.contains("snake_choke");
    }

    // [HAERTUNG 16.09.2026] Der zweite Schluessel allein reicht nicht: fehlt
    // "bat_choke" in re4_vr_achievement.json, gilt auch das zweite als nicht da.
    return m_snake_unlocked && achievement_unlocked();
}

bool RE4VRChoke::achievement_unlocked() {
    if (!m_achievement_checked) {
        m_achievement_checked = true;

        const auto j = re4vr::json_load(ACHIEVEMENT_CFG_PATH);
        m_achievement_seen = j.is_object() && j.contains("bat_choke");
    }

    return m_achievement_seen;
}

// [ASHLEY 13.09.2026] Die Begleitung als Griffziel. Bewusst KEINE Freigaben
// (Trefferreaktion, Parade): ihr Kontext hat gar kein get_ActionState -- die
// Abfrage aus pick_target liefe dort ins Leere. Entschieden wird wie beim
// Tier allein ueber Reichweite, dazu dasselbe Hoehenfenster wie beim Gegner.
::REManagedObject* RE4VRChoke::pick_ashley(const glm::vec3& hp) {
    // OHNE das Achievement gibt es sie als Ziel nicht (Ansage 13.09.2026).
    if (!achievement_unlocked()) {
        return nullptr;
    }

    auto* cm = character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_PartnerContextList")
        : nullptr;

    if (list == nullptr) {
        return nullptr;
    }

    const int32_t n = opt_int(list, "get_Count").value_or(0);

    ::REManagedObject* best = nullptr;
    float bestd = m_cfg.grab_dist;

    for (int32_t i = 0; i < n; ++i) {
        auto* pctx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        // is_live passt unveraendert: get_IsEliminated und get_HitPoint hat sie
        // (gemessen), und is_animal ist hier false.
        if (!is_live(pctx)) {
            continue;
        }

        // Nur Ashley -- eine andere Begleitung (Luis, Ada) bleibt aussen vor.
        auto* go = re4vr::call_safe<::REManagedObject*>(pctx, "get_BodyGameObject");
        const auto bn = obj_name_of(go);

        if (bn != ASHLEY_BODY_NPC && bn != ASHLEY_BODY_PLAYABLE) {
            continue;
        }

        glm::vec3 rp{};

        if (!get_vec3(pctx, "get_Position", rp)) {
            continue;
        }

        const float dy = hp.y - rp.y;
        const float d = horiz(rp, hp);

        if (dy < m_cfg.hand_min_y || dy > m_cfg.hand_max_y || d >= bestd) {
            continue;
        }

        bestd = d;
        best = pctx;
    }

    return best;
}

// ============================================================================
// Config
// ============================================================================

void RE4VRChoke::load_cfg() {
    const auto d = re4vr::json_load(CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    // b() nimmt nur echte Booleans, n() nur Zahlen -- alles andere behaelt den
    // bisherigen Wert.
    const auto b = [&](const char* key, bool cur) {
        const auto it = d.find(key);

        return (it != d.end() && it->is_boolean()) ? it->get<bool>() : cur;
    };
    const auto n = [&](const char* key, float cur) {
        const auto it = d.find(key);

        if (it == d.end()) {
            return cur;
        }

        if (it->is_number()) {
            return it->get<float>();
        }

        // Luas tonumber() nimmt auch Zahl-Strings.
        if (it->is_string()) {
            try {
                return std::stof(it->get<std::string>());
            } catch (...) {
                return cur;
            }
        }

        return cur;
    };

    m_cfg.choke_on = b("on", m_cfg.choke_on);
    m_cfg.chicken_grab = b("chicken_grab", m_cfg.chicken_grab);
    m_cfg.chicken_hold = n("chicken_hold", m_cfg.chicken_hold);
    m_cfg.taunt_vol_db = n("taunt_vol_db", m_cfg.taunt_vol_db);
    m_cfg.taunt_delay_s = n("taunt_delay_s", m_cfg.taunt_delay_s);
    m_cfg.hand_yaw_deg = n("hand_yaw_deg", m_cfg.hand_yaw_deg);
    m_cfg.hand_pitch_deg = n("hand_pitch_deg", m_cfg.hand_pitch_deg);
    m_cfg.hand_roll_deg = n("hand_roll_deg", m_cfg.hand_roll_deg);

    // [MAUS 14.09.2026] Fehlt der Eintrag in der JSON (alle bisherigen
    // Konfigurationen), erbt die Maus den allgemeinen Satz -- ihr Verhalten ist
    // nach dem Update also unveraendert, und es gibt trotzdem ab sofort eigene
    // Regler. Steht muss NACH den drei hand_*_deg, sonst erbt sie Nullen.
    m_cfg.mouse_hand_yaw = n("mouse_hand_yaw", m_cfg.hand_yaw_deg);
    m_cfg.mouse_hand_pitch = n("mouse_hand_pitch", m_cfg.hand_pitch_deg);
    m_cfg.mouse_hand_roll = n("mouse_hand_roll", m_cfg.hand_roll_deg);
    m_cfg.animal_stick_in = n("animal_stick_in", m_cfg.animal_stick_in);
    // Der Abspieler haelt den Pegel selbst -- gleich nach dem Laden dorthin
    // durchstellen, sonst klingt der erste Sound nach Vorgabe statt nach JSON.
    re4vr::set_taunt_wav_gain_db(m_cfg.taunt_vol_db);
    m_cfg.ada_gain_db = n("ada_gain_db", m_cfg.ada_gain_db);
    re4vr::set_ada_wav_extra_db(m_cfg.ada_gain_db);
    m_cfg.chicken_break_snd = static_cast<uint32_t>(
        n("chicken_break_snd", static_cast<float>(m_cfg.chicken_break_snd)));
    m_cfg.leon_campaign_only = b("leon_only", m_cfg.leon_campaign_only);
    m_cfg.require_parry = b("require_parry", m_cfg.require_parry);
    m_cfg.debug = b("debug", m_cfg.debug);

    // [KAPUTTES LOCAL] `FLICKER_DEBUG = b(d.flicker, FLICKER_DEBUG)` schreibt
    // in Lua ins GLOBAL, nicht in das erst bei Z.1729 deklarierte local. Das
    // Laufzeit-Flag bleibt also unberuehrt -- 1:1 nachgebaut, indem hier
    // NICHTS gesetzt wird. Das Global wird trotzdem gefuehrt.
    {
        const auto it = d.find("flicker");

        if (it != d.end() && it->is_boolean()) {
            re4vr::lua_set_bool("FLICKER_DEBUG", it->get<bool>());
        }
    }

    m_cfg.y_lerp = n("y_lerp", m_cfg.y_lerp);
    m_cfg.fix_height = b("fix_height", m_cfg.fix_height);
    m_cfg.neck_up = n("neck_up", m_cfg.neck_up);
    m_cfg.reequip_after = b("reequip_after", m_cfg.reequip_after);
    m_cfg.y_clamp_on = b("y_clamp_on", m_cfg.y_clamp_on);
    m_cfg.y_clamp = n("y_clamp", m_cfg.y_clamp);
    m_cfg.hand_lat = n("hand_lat", m_cfg.hand_lat);
    m_cfg.grip_away = n("away", m_cfg.grip_away);
    m_cfg.grip_side = n("side", m_cfg.grip_side);
    m_cfg.grip_up = n("up", m_cfg.grip_up);
    m_cfg.grab_dist = n("grab_dist", m_cfg.grab_dist);
    m_cfg.hand_min_y = n("hand_min", m_cfg.hand_min_y);
    m_cfg.hand_max_y = n("hand_max", m_cfg.hand_max_y);
    m_cfg.hold_max = n("hold_max", m_cfg.hold_max);
    m_cfg.cooldown = n("cooldown", m_cfg.cooldown);
    m_cfg.parry_window = n("parry_window", m_cfg.parry_window);
    m_cfg.parry_target_only = b("parry_target_only", m_cfg.parry_target_only);
    m_cfg.knife_to_right = b("knife_right", m_cfg.knife_to_right);
    m_cfg.knife_stick = b("knife_stick", m_cfg.knife_stick);
    m_cfg.knife_stick_t = n("knife_stick_t", m_cfg.knife_stick_t);
    m_cfg.knife_stick_in = n("knife_stick_in", m_cfg.knife_stick_in);
    m_cfg.stick_snap = n("stick_snap_max", m_cfg.stick_snap);
    m_cfg.knife_stick_v = n("knife_stick_v", m_cfg.knife_stick_v);
    m_cfg.knife_stick_w = n("knife_stick_w", m_cfg.knife_stick_w);
    m_cfg.choke_swing_t = n("knife_swing_t_choke", m_cfg.choke_swing_t);
    m_cfg.fwd_only = b("fwd_only", m_cfg.fwd_only);
    m_cfg.tip_on = b("tip_on", m_cfg.tip_on);
    m_cfg.tip_r = n("tip_r", m_cfg.tip_r);
    m_cfg.tip_speed = n("tip_speed", m_cfg.tip_speed);
    m_cfg.tip_arm = n("tip_arm", m_cfg.tip_arm);
    m_cfg.tip_len = n("tip_len", m_cfg.tip_len);
    m_cfg.knife_dim = b("knife_dim", m_cfg.knife_dim);
    m_cfg.knife_dim_f = n("knife_dim_f", m_cfg.knife_dim_f);

    // [KAPUTTES LOCAL] `STOP_ENEMY_SOUNDS = b(d.stop_enemy_snd, ...)` trifft
    // ebenfalls nur das Global -- das local ab Z.635 bleibt auf true. Also
    // auch hier: das Laufzeit-Flag NICHT anfassen.
    {
        const auto it = d.find("stop_enemy_snd");

        if (it != d.end() && it->is_boolean()) {
            re4vr::lua_set_bool("STOP_ENEMY_SOUNDS", it->get<bool>());
        }
    }

    m_cfg.use_native_hit = b("native_hit", m_cfg.use_native_hit);
    m_cfg.knife_dmg = n("knife_dmg", m_cfg.knife_dmg);
    m_cfg.knife_dmg_stick = n("knife_dmg_stick", m_cfg.knife_dmg_stick);

    // [MIGRATION 2026-08-27] Fehlt das neue Feld, stammt die Datei aus der
    // Zeit davor -- dann gelten einmalig beide neuen Vorgaben. Sobald einmal
    // gespeichert wurde, greift das nie wieder.
    if (d.find("knife_dmg_stick") == d.end() || d["knife_dmg_stick"].is_null()) {
        m_cfg.knife_dmg = 150.0f;
        m_cfg.knife_dmg_stick = 250.0f;
    }

    m_cfg.min_neck_y = n("neck_min", m_cfg.min_neck_y);
    m_cfg.max_neck_y = n("neck_max", m_cfg.max_neck_y);
    m_cfg.freeze_yaw = b("freeze_yaw", m_cfg.freeze_yaw);
    m_cfg.lock_on_grab = b("lock_on_grab", m_cfg.lock_on_grab);
    m_cfg.torso_pin = b("torso_pin", m_cfg.torso_pin);
    m_cfg.torso_upright = b("torso_upright", m_cfg.torso_upright);

    // FREEZE_MOTION ist in Lua NIE als local deklariert -- also ein echtes
    // Global, das hier konsistent geladen wird. Ausgewertet wird es nirgends.
    {
        const auto it = d.find("freeze_motion");

        if (it != d.end() && it->is_boolean()) {
            re4vr::lua_set_bool("FREEZE_MOTION", it->get<bool>());
        }
    }

    m_cfg.grapple_flags = b("grapple_flags", m_cfg.grapple_flags);
    m_cfg.leg_ik_off = b("leg_ik_off", m_cfg.leg_ik_off);
    m_cfg.parent_to_body = b("parent_body", m_cfg.parent_to_body);
    m_cfg.face_yaw_deg = n("face_yaw", m_cfg.face_yaw_deg);
    m_cfg.animal_yaw_deg = n("animal_yaw", m_cfg.animal_yaw_deg);
    m_cfg.animal_off_x = n("animal_off_x", m_cfg.animal_off_x);
    m_cfg.animal_off_y = n("animal_off_y", m_cfg.animal_off_y);
    m_cfg.animal_off_z = n("animal_off_z", m_cfg.animal_off_z);
    // [STARTWERTE 13.09.2026 -- Ansage "du kannst der Kraehe gerne die
    // Startwerte der Maus geben"] Fehlt ein crow_*-Eintrag in der JSON, gilt
    // der eingestellte MAUS-Wert als Vorgabe -- die Kraehe faengt also nicht bei
    // 0 an, sondern dort, wo die Maus schon gut sitzt. Sobald an ihren Reglern
    // gedreht wird, stehen ihre eigenen Werte in der Datei und die Maus ist
    // unberuehrt. (Die animal_*-Werte werden weiter oben geladen, stehen hier
    // also schon.)
    m_cfg.crow_yaw_deg = n("crow_yaw", m_cfg.animal_yaw_deg);
    m_cfg.crow_pitch_deg = n("crow_pitch", m_cfg.animal_pitch_deg);
    m_cfg.crow_roll_deg = n("crow_roll", m_cfg.animal_roll_deg);
    m_cfg.crow_off_x = n("crow_off_x", m_cfg.animal_off_x);
    m_cfg.crow_off_y = n("crow_off_y", m_cfg.animal_off_y);
    m_cfg.crow_off_z = n("crow_off_z", m_cfg.animal_off_z);
    m_cfg.bat_yaw_deg = n("bat_yaw", m_cfg.bat_yaw_deg);
    m_cfg.bat_pitch_deg = n("bat_pitch", m_cfg.bat_pitch_deg);
    m_cfg.bat_roll_deg = n("bat_roll", m_cfg.bat_roll_deg);
    m_cfg.bat_grab_dist = n("bat_grab_dist", m_cfg.bat_grab_dist);
    m_cfg.bat_hold_s = n("bat_hold_s", m_cfg.bat_hold_s);

    m_cfg.animal_pin = b("animal_pin", m_cfg.animal_pin);
    m_cfg.crow_pin = b("crow_pin", m_cfg.crow_pin);
    m_cfg.bat_pin = b("bat_pin", m_cfg.bat_pin);
    m_cfg.chick_pin = b("chick_pin", m_cfg.chick_pin);
    m_cfg.ene_pin = b("ene_pin", m_cfg.ene_pin);
    m_cfg.ashley_pin = b("ashley_pin", m_cfg.ashley_pin);

    m_cfg.reply_gain_db = n("reply_gain_db", m_cfg.reply_gain_db);
    m_cfg.reply_delay_s = n("reply_delay_s", m_cfg.reply_delay_s);
    m_cfg.mouth_on = b("mouth_on", m_cfg.mouth_on);
    m_cfg.mouth_scale = n("mouth_scale", m_cfg.mouth_scale);
    m_cfg.mouth_speed = n("mouth_speed", m_cfg.mouth_speed);

    m_cfg.crow_hand_yaw = n("crow_hand_yaw", m_cfg.crow_hand_yaw);
    m_cfg.crow_hand_pitch = n("crow_hand_pitch", m_cfg.crow_hand_pitch);
    m_cfg.crow_hand_roll = n("crow_hand_roll", m_cfg.crow_hand_roll);
    m_cfg.bat_hand_yaw = n("bat_hand_yaw", m_cfg.bat_hand_yaw);
    m_cfg.bat_hand_pitch = n("bat_hand_pitch", m_cfg.bat_hand_pitch);
    m_cfg.bat_hand_roll = n("bat_hand_roll", m_cfg.bat_hand_roll);
    m_cfg.chick_hand_yaw = n("chick_hand_yaw", m_cfg.chick_hand_yaw);
    m_cfg.chick_hand_pitch = n("chick_hand_pitch", m_cfg.chick_hand_pitch);
    m_cfg.chick_hand_roll = n("chick_hand_roll", m_cfg.chick_hand_roll);
    m_cfg.ene_hand_yaw = n("ene_hand_yaw", m_cfg.ene_hand_yaw);
    m_cfg.ene_hand_pitch = n("ene_hand_pitch", m_cfg.ene_hand_pitch);
    m_cfg.ene_hand_roll = n("ene_hand_roll", m_cfg.ene_hand_roll);
    m_cfg.ashley_hand_yaw = n("ashley_hand_yaw", m_cfg.ashley_hand_yaw);
    m_cfg.ashley_hand_pitch = n("ashley_hand_pitch", m_cfg.ashley_hand_pitch);
    m_cfg.ashley_hand_roll = n("ashley_hand_roll", m_cfg.ashley_hand_roll);
    m_cfg.chick_yaw_deg = n("chick_yaw", m_cfg.chick_yaw_deg);
    m_cfg.chick_pitch_deg = n("chick_pitch", m_cfg.chick_pitch_deg);
    m_cfg.chick_roll_deg = n("chick_roll", m_cfg.chick_roll_deg);
    m_cfg.chick_off_x = n("chick_off_x", m_cfg.chick_off_x);
    m_cfg.chick_off_y = n("chick_off_y", m_cfg.chick_off_y);
    m_cfg.chick_off_z = n("chick_off_z", m_cfg.chick_off_z);
    m_cfg.ene_yaw_deg = n("ene_yaw", m_cfg.ene_yaw_deg);
    m_cfg.ene_pitch_deg = n("ene_pitch", m_cfg.ene_pitch_deg);
    m_cfg.ene_roll_deg = n("ene_roll", m_cfg.ene_roll_deg);
    m_cfg.ene_off_x = n("ene_off_x", m_cfg.ene_off_x);
    m_cfg.ene_off_y = n("ene_off_y", m_cfg.ene_off_y);
    m_cfg.ene_off_z = n("ene_off_z", m_cfg.ene_off_z);
    m_cfg.ashley_yaw_deg = n("ashley_yaw", m_cfg.ashley_yaw_deg);
    m_cfg.ashley_pitch_deg = n("ashley_pitch", m_cfg.ashley_pitch_deg);
    m_cfg.ashley_roll_deg = n("ashley_roll", m_cfg.ashley_roll_deg);
    m_cfg.ashley_off_x = n("ashley_off_x", m_cfg.ashley_off_x);
    m_cfg.ashley_off_y = n("ashley_off_y", m_cfg.ashley_off_y);
    m_cfg.ashley_off_z = n("ashley_off_z", m_cfg.ashley_off_z);
    m_cfg.bat_off_x = n("bat_off_x", m_cfg.bat_off_x);
    m_cfg.bat_off_y = n("bat_off_y", m_cfg.bat_off_y);
    m_cfg.bat_off_z = n("bat_off_z", m_cfg.bat_off_z);
    m_cfg.animal_pitch_deg = n("animal_pitch", m_cfg.animal_pitch_deg);
    m_cfg.animal_roll_deg = n("animal_roll", m_cfg.animal_roll_deg);

    // [ADA-MAUS 17.09.2026] Fehlt ein Eintrag, gilt Leons Maus-Wert als Start.
    // Steht bewusst HIER: erst ab dieser Zeile sind alle animal_*- und
    // mouse_hand_*-Werte geladen.
    m_cfg.ada_mouse_yaw_deg = n("ada_mouse_yaw", m_cfg.animal_yaw_deg);
    m_cfg.ada_mouse_pitch_deg = n("ada_mouse_pitch", m_cfg.animal_pitch_deg);
    m_cfg.ada_mouse_roll_deg = n("ada_mouse_roll", m_cfg.animal_roll_deg);
    m_cfg.ada_mouse_off_x = n("ada_mouse_off_x", m_cfg.animal_off_x);
    m_cfg.ada_mouse_off_y = n("ada_mouse_off_y", m_cfg.animal_off_y);
    m_cfg.ada_mouse_off_z = n("ada_mouse_off_z", m_cfg.animal_off_z);
    m_cfg.ada_mouse_hand_yaw = n("ada_mouse_hand_yaw", m_cfg.mouse_hand_yaw);
    m_cfg.ada_mouse_hand_pitch = n("ada_mouse_hand_pitch", m_cfg.mouse_hand_pitch);
    m_cfg.ada_mouse_hand_roll = n("ada_mouse_hand_roll", m_cfg.mouse_hand_roll);
    m_cfg.ada_mouse_pin = b("ada_mouse_pin", m_cfg.ada_mouse_pin);
    m_cfg.pose_on = b("pose_on", m_cfg.pose_on);
    m_cfg.pose_force = b("pose_force", m_cfg.pose_force);

    const auto th = d.find("thumb");

    if (th != d.end() && th->is_object()) {
        const auto one = [&](const char* key, glm::vec3& dst) {
            const auto s = th->find(key);

            if (s == th->end() || !s->is_object()) {
                return;
            }

            const auto pick = [&](const char* f, float cur) {
                const auto it = s->find(f);

                return (it != s->end() && it->is_number()) ? it->get<float>() : cur;
            };

            dst.x = pick("x", dst.x);
            dst.y = pick("y", dst.y);
            dst.z = pick("z", dst.z);
        };

        one("t1", m_cfg.thumb_t1);
        one("t2", m_cfg.thumb_t2);
        one("t3", m_cfg.thumb_t3);
    }
}

void RE4VRChoke::save_cfg() {
    nlohmann::json d;

    d["on"] = m_cfg.choke_on;
    d["chicken_grab"] = m_cfg.chicken_grab;
    d["chicken_hold"] = m_cfg.chicken_hold;
    d["taunt_vol_db"] = m_cfg.taunt_vol_db;
    d["ada_gain_db"] = m_cfg.ada_gain_db;
    d["taunt_delay_s"] = m_cfg.taunt_delay_s;
    d["hand_yaw_deg"] = m_cfg.hand_yaw_deg;
    d["hand_pitch_deg"] = m_cfg.hand_pitch_deg;
    d["hand_roll_deg"] = m_cfg.hand_roll_deg;
    d["mouse_hand_yaw"] = m_cfg.mouse_hand_yaw;
    d["mouse_hand_pitch"] = m_cfg.mouse_hand_pitch;
    d["mouse_hand_roll"] = m_cfg.mouse_hand_roll;
    d["animal_stick_in"] = m_cfg.animal_stick_in;
    d["chicken_break_snd"] = static_cast<double>(m_cfg.chicken_break_snd);
    d["leon_only"] = m_cfg.leon_campaign_only;
    d["require_parry"] = m_cfg.require_parry;
    d["debug"] = m_cfg.debug;
    d["away"] = m_cfg.grip_away;
    d["side"] = m_cfg.grip_side;
    d["up"] = m_cfg.grip_up;
    d["grab_dist"] = m_cfg.grab_dist;
    d["hand_min"] = m_cfg.hand_min_y;
    d["hand_max"] = m_cfg.hand_max_y;
    d["hold_max"] = m_cfg.hold_max;
    d["cooldown"] = m_cfg.cooldown;
    d["parry_window"] = m_cfg.parry_window;
    d["parry_target_only"] = m_cfg.parry_target_only;
    d["knife_right"] = m_cfg.knife_to_right;
    d["knife_stick"] = m_cfg.knife_stick;
    d["knife_stick_t"] = m_cfg.knife_stick_t;
    d["knife_stick_in"] = m_cfg.knife_stick_in;
    d["stick_snap_max"] = m_cfg.stick_snap;
    d["knife_stick_v"] = m_cfg.knife_stick_v;
    d["knife_stick_w"] = m_cfg.knife_stick_w;
    d["knife_swing_t_choke"] = m_cfg.choke_swing_t;
    d["fwd_only"] = m_cfg.fwd_only;
    d["knife_dim"] = m_cfg.knife_dim;
    d["knife_dim_f"] = m_cfg.knife_dim_f;

    // [KAPUTTES LOCAL] `stop_enemy_snd = STOP_ENEMY_SOUNDS` liest in Lua das
    // GLOBAL. Ist es nie gesetzt worden, ist es nil -- und json.dump_file
    // laesst nil-Werte weg. Genau das hier: der Schluessel entsteht nur, wenn
    // das Global tatsaechlich existiert.
    if (const auto g = re4vr::lua_get_tribool("STOP_ENEMY_SOUNDS"); g >= 0) {
        d["stop_enemy_snd"] = (g == 1);
    }

    d["native_hit"] = m_cfg.use_native_hit;
    d["knife_dmg"] = m_cfg.knife_dmg;
    d["knife_dmg_stick"] = m_cfg.knife_dmg_stick;
    d["neck_min"] = m_cfg.min_neck_y;
    d["neck_max"] = m_cfg.max_neck_y;
    d["freeze_yaw"] = m_cfg.freeze_yaw;
    d["face_yaw"] = m_cfg.face_yaw_deg;
    d["animal_yaw"] = m_cfg.animal_yaw_deg;
    d["animal_off_x"] = m_cfg.animal_off_x;
    d["animal_off_y"] = m_cfg.animal_off_y;
    d["animal_off_z"] = m_cfg.animal_off_z;
    d["crow_yaw"] = m_cfg.crow_yaw_deg;
    d["crow_pitch"] = m_cfg.crow_pitch_deg;
    d["crow_roll"] = m_cfg.crow_roll_deg;
    d["crow_off_x"] = m_cfg.crow_off_x;
    d["crow_off_y"] = m_cfg.crow_off_y;
    d["crow_off_z"] = m_cfg.crow_off_z;
    d["bat_yaw"] = m_cfg.bat_yaw_deg;
    d["bat_pitch"] = m_cfg.bat_pitch_deg;
    d["bat_roll"] = m_cfg.bat_roll_deg;
    d["bat_grab_dist"] = m_cfg.bat_grab_dist;
    d["bat_hold_s"] = m_cfg.bat_hold_s;

    d["animal_pin"] = m_cfg.animal_pin;
    d["crow_pin"] = m_cfg.crow_pin;
    d["bat_pin"] = m_cfg.bat_pin;
    d["chick_pin"] = m_cfg.chick_pin;
    d["ene_pin"] = m_cfg.ene_pin;
    d["ashley_pin"] = m_cfg.ashley_pin;

    d["reply_gain_db"] = m_cfg.reply_gain_db;
    d["reply_delay_s"] = m_cfg.reply_delay_s;
    d["mouth_on"] = m_cfg.mouth_on;
    d["mouth_scale"] = m_cfg.mouth_scale;
    d["mouth_speed"] = m_cfg.mouth_speed;

    d["crow_hand_yaw"] = m_cfg.crow_hand_yaw;
    d["crow_hand_pitch"] = m_cfg.crow_hand_pitch;
    d["crow_hand_roll"] = m_cfg.crow_hand_roll;
    d["bat_hand_yaw"] = m_cfg.bat_hand_yaw;
    d["bat_hand_pitch"] = m_cfg.bat_hand_pitch;
    d["bat_hand_roll"] = m_cfg.bat_hand_roll;
    d["chick_hand_yaw"] = m_cfg.chick_hand_yaw;
    d["chick_hand_pitch"] = m_cfg.chick_hand_pitch;
    d["chick_hand_roll"] = m_cfg.chick_hand_roll;
    d["ene_hand_yaw"] = m_cfg.ene_hand_yaw;
    d["ene_hand_pitch"] = m_cfg.ene_hand_pitch;
    d["ene_hand_roll"] = m_cfg.ene_hand_roll;
    d["ashley_hand_yaw"] = m_cfg.ashley_hand_yaw;
    d["ashley_hand_pitch"] = m_cfg.ashley_hand_pitch;
    d["ashley_hand_roll"] = m_cfg.ashley_hand_roll;
    d["chick_yaw"] = m_cfg.chick_yaw_deg;
    d["chick_pitch"] = m_cfg.chick_pitch_deg;
    d["chick_roll"] = m_cfg.chick_roll_deg;
    d["chick_off_x"] = m_cfg.chick_off_x;
    d["chick_off_y"] = m_cfg.chick_off_y;
    d["chick_off_z"] = m_cfg.chick_off_z;
    d["ene_yaw"] = m_cfg.ene_yaw_deg;
    d["ene_pitch"] = m_cfg.ene_pitch_deg;
    d["ene_roll"] = m_cfg.ene_roll_deg;
    d["ene_off_x"] = m_cfg.ene_off_x;
    d["ene_off_y"] = m_cfg.ene_off_y;
    d["ene_off_z"] = m_cfg.ene_off_z;
    d["ashley_yaw"] = m_cfg.ashley_yaw_deg;
    d["ashley_pitch"] = m_cfg.ashley_pitch_deg;
    d["ashley_roll"] = m_cfg.ashley_roll_deg;
    d["ashley_off_x"] = m_cfg.ashley_off_x;
    d["ashley_off_y"] = m_cfg.ashley_off_y;
    d["ashley_off_z"] = m_cfg.ashley_off_z;
    d["bat_off_x"] = m_cfg.bat_off_x;
    d["bat_off_y"] = m_cfg.bat_off_y;
    d["bat_off_z"] = m_cfg.bat_off_z;
    d["animal_pitch"] = m_cfg.animal_pitch_deg;
    d["animal_roll"] = m_cfg.animal_roll_deg;
    d["ada_mouse_yaw"] = m_cfg.ada_mouse_yaw_deg;
    d["ada_mouse_pitch"] = m_cfg.ada_mouse_pitch_deg;
    d["ada_mouse_roll"] = m_cfg.ada_mouse_roll_deg;
    d["ada_mouse_off_x"] = m_cfg.ada_mouse_off_x;
    d["ada_mouse_off_y"] = m_cfg.ada_mouse_off_y;
    d["ada_mouse_off_z"] = m_cfg.ada_mouse_off_z;
    d["ada_mouse_hand_yaw"] = m_cfg.ada_mouse_hand_yaw;
    d["ada_mouse_hand_pitch"] = m_cfg.ada_mouse_hand_pitch;
    d["ada_mouse_hand_roll"] = m_cfg.ada_mouse_hand_roll;
    d["ada_mouse_pin"] = m_cfg.ada_mouse_pin;
    d["lock_on_grab"] = m_cfg.lock_on_grab;
    d["torso_pin"] = m_cfg.torso_pin;
    d["torso_upright"] = m_cfg.torso_upright;

    // Dito FREEZE_MOTION -- echtes Global, kein local.
    if (const auto g = re4vr::lua_get_tribool("FREEZE_MOTION"); g >= 0) {
        d["freeze_motion"] = (g == 1);
    }

    d["grapple_flags"] = m_cfg.grapple_flags;
    d["leg_ik_off"] = m_cfg.leg_ik_off;
    d["parent_body"] = m_cfg.parent_to_body;
    d["pose_on"] = m_cfg.pose_on;
    d["pose_force"] = m_cfg.pose_force;
    d["y_clamp_on"] = m_cfg.y_clamp_on;
    d["y_clamp"] = m_cfg.y_clamp;
    d["hand_lat"] = m_cfg.hand_lat;
    d["reequip_after"] = m_cfg.reequip_after;
    d["fix_height"] = m_cfg.fix_height;
    d["neck_up"] = m_cfg.neck_up;
    d["y_lerp"] = m_cfg.y_lerp;
    d["tip_on"] = m_cfg.tip_on;
    d["tip_r"] = m_cfg.tip_r;
    d["tip_speed"] = m_cfg.tip_speed;
    d["tip_arm"] = m_cfg.tip_arm;
    d["tip_len"] = m_cfg.tip_len;

    // [1:1] `flicker` steht NICHT in der gespeicherten Tabelle -- der
    // Kommentar im Original ("wird jetzt mitgespeichert") ist falsch.
    nlohmann::json th;
    const auto put = [&](const char* key, const glm::vec3& v) {
        nlohmann::json e;
        e["x"] = v.x;
        e["y"] = v.y;
        e["z"] = v.z;
        th[key] = e;
    };

    put("t1", m_cfg.thumb_t1);
    put("t2", m_cfg.thumb_t2);
    put("t3", m_cfg.thumb_t3);
    d["thumb"] = th;

    re4vr::json_save(CFG_PATH, d);
}

// ============================================================================
// Helfer
// ============================================================================

// [FRAME-CACHE] Beide ueber re4vr::fc. player_body_tf() steckt in apply_hold
// und apply_choke_pose, die zusammen SIEBENMAL pro Frame laufen (fuenf Phasen
// plus joints_tick in zweien davon) -- ohne Cache also siebenmal die volle
// Kette aus vier Engine-Aufrufen.
::REManagedObject* RE4VRChoke::player_body_go() {
    return re4vr::fc::body_go();
}

::REManagedObject* RE4VRChoke::player_body_tf() {
    return re4vr::fc::body_tf();
}

// PlayerEquipment haengt am HEAD-GameObject, nicht am Body.
// [FRAME-CACHE] ueber re4vr::fc.
::REManagedObject* RE4VRChoke::player_equipment() {
    return re4vr::fc::pe();
}

std::optional<int32_t> RE4VRChoke::equip_type_main() {
    if (m_et_main.has_value()) {
        return m_et_main;
    }

    m_et_main = re4vr::enum_value("chainsaw.EquipType", "Main");

    return m_et_main;
}

int32_t RE4VRChoke::root_none_value() {
    if (m_root_none.has_value()) {
        return *m_root_none;
    }

    // Lua: `ROOT_NONE = n or 0` -- schlaegt die TDB-Suche fehl, gilt 0.
    m_root_none = re4vr::enum_value("via.motion.RootPlayMode", "None").value_or(0);

    return *m_root_none;
}

// [MESSER-PFLICHT] Ohne Messer im INVENTAR laeuft requestEquipKnife ins Leere.
// KEIN Cache: das Messer kann jederzeit weg sein, und ein Komponenten-Cache
// ueberlebt keinen Save-Load.
bool RE4VRChoke::have_knife() {
    auto* pe = player_equipment();

    if (pe == nullptr) {
        return false;
    }

    auto* inv = re4vr::call_safe<::REManagedObject*>(pe, "get_InventoryController");

    if (inv == nullptr) {
        return false;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(inv, "getInventoryItemList");

    if (list == nullptr) {
        return false;
    }

    const int32_t n = opt_int(list, "get_Count").value_or(0);

    for (int32_t i = 0; i < n; ++i) {
        auto* row = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (row == nullptr) {
            continue;
        }

        // get_WeaponId kann als Zahl ODER als Enum-Objekt kommen -- in Lua
        // faengt das `tonumber(w)` bzw. der value__-Rueckfall ab.
        if (const auto wid = opt_int(row, "get_WeaponId"); wid.has_value()) {
            if (is_knife_id(*wid)) {
                return true;
            }

            continue;
        }

        // [ENUM-FALLE] Nur anfassen, wenn get_WeaponId wirklich ein OBJEKT
        // liefert -- ein Enum kommt als Zahl, und die als Zeiger zu
        // dereferenzieren ist eine Access Violation (s. get_Category).
        if (!getter_returns_object(row, "get_WeaponId")) {
            continue;
        }

        auto* w = re4vr::call_safe<::REManagedObject*>(row, "get_WeaponId");

        if (w != nullptr) {
            if (const auto v = re4vr::get_field_int(w, "value__");
                v.has_value() && is_knife_id(*v)) {
                return true;
            }
        }
    }

    return false;
}

// [RUECKWECHSEL] Zurueck auf die zuletzt aktive Hauptwaffe. Reihenfolge und
// execChangeWeapon 1:1 wie in re4_vr_holster.lua:571.
void RE4VRChoke::reequip_last_weapon() {
    auto* pe = player_equipment();

    if (pe == nullptr) {
        return;
    }

    const auto et = equip_type_main();

    if (!et.has_value()) {
        return;
    }

    re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
    re4vr::call_safe<void*>(pe, "clearRequest");
    re4vr::call_safe<void*>(
        pe, "requestChangeActiveWeapon(chainsaw.EquipType, System.Boolean, System.Boolean)",
        *et, false, false);
    re4vr::call_safe<void*>(pe, "execChangeWeapon");
}

// [BAREHANDS 2026-09-12 -- Ansage "mit bare hands gechoked, danach liegt die
// letzte Waffe in der Hand"] requestChangeActiveWeapon(EquipType.Main) holt
// IMMER die Hauptwaffe, egal was vorher in der Hand lag. Wer mit leeren
// Haenden zugepackt hat, bekommt deshalb hier den Gegenweg.
//
// Der Weg ist 1:1 der von RE4VRHolster: clearRequest, die Marke
// __re4_barehand_ours_t (daran erkennt merc UNSERE Aufrufe und blockt sie
// nicht), dann requestEquipBareHand + execChangeWeapon.
void RE4VRChoke::restore_bare_hands() {
    auto* pe = player_equipment();

    if (pe == nullptr) {
        return;
    }

    re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
    re4vr::call_safe<void*>(pe, "clearRequest");
    re4vr::lua_set_number("__re4_barehand_ours_t", clock_now());
    re4vr::call_safe<void*>(pe, "requestEquipBareHand", false, false);
    re4vr::call_safe<void*>(pe, "execChangeWeapon");
}

// [BELEGT 2026-08-24] Im Koerperbaum haengt mehr als ein wpXXXX (geholstert,
// Reste nach Save-Load), und "nimm das erste" erwischt nach einem Ladevorgang
// das falsche. Darum ALLE Kandidaten sammeln und den nehmen, der der RECHTEN
// HAND am naechsten ist.
RE4VRChoke::KnifePick RE4VRChoke::knife_transform() {
    KnifePick out{};

    auto* btf = player_body_tf();

    if (btf == nullptr) {
        return out;
    }

    struct Cand {
        ::REManagedObject* tf{nullptr};
        std::string nm{};
        std::optional<glm::vec3> p{};
    };

    std::vector<Cand> cands;

    // Rekursiver Walk, Tiefe 8, je Ebene hoechstens 128 Geschwister -- 1:1.
    const std::function<void(::REManagedObject*, int)> walk =
        [&](::REManagedObject* tf, int d) {
            if (tf == nullptr || d < 0) {
                return;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
            const std::string nm = obj_name_of(go);

            if (!nm.empty() && is_knife_wp(nm)) {
                Cand c{};
                c.tf = tf;
                c.nm = nm;

                glm::vec3 p{};

                if (get_vec3(tf, "get_Position", p)) {
                    c.p = p;
                }

                cands.push_back(c);
            }

            auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
            int n = 0;

            while (ch != nullptr && n < 128) {
                ++n;
                walk(ch, d - 1);
                ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
            }
        };

    walk(btf, 8);

    if (cands.empty()) {
        return out;
    }

    // [GEWORFENES AUSSORTIEREN] Das fliegende Messer haengt WEITER im
    // Koerperbaum, nur an `root` statt an `R_Wep`. Haengt also irgendein
    // Kandidat an einem Waffen-Joint, zaehlen nur noch die.
    std::vector<Cand> wep;

    for (const auto& c : cands) {
        auto* pj = re4vr::call_safe<::REManagedObject*>(c.tf, "get_ParentJoint");
        std::string pjn;

        if (pj != nullptr) {
            // In Lua kommt hier entweder ein String oder ein Joint-Objekt.
            pjn = managed_string_of(re4vr::call_safe<::REManagedObject*>(pj, "get_Name"));

            if (pjn.empty()) {
                pjn = managed_string_of(pj);
            }
        }

        if (!pjn.empty() && pjn.find("Wep") != std::string::npos) {
            wep.push_back(c);
        }
    }

    if (!wep.empty()) {
        cands = wep;
    }

    out.count = static_cast<int>(cands.size());

    const auto rh = re4vr::lua_get_vec3("__vr_rh_world");

    // Ohne Handposition bleibt das erste -- dann ist die Auswahl so gut wie
    // vorher, nicht schlechter.
    if (!rh.has_value()) {
        out.tf = cands[0].tf;
        out.nm = cands[0].nm;
        return out;
    }

    const Cand* best = nullptr;
    float bestd = 0.0f;

    for (const auto& c : cands) {
        if (!c.p.has_value()) {
            continue;
        }

        const glm::vec3 d = *c.p - *rh;
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);

        if (best == nullptr || dist < bestd) {
            best = &c;
            bestd = dist;
        }
    }

    if (best == nullptr) {
        out.tf = cands[0].tf;
        out.nm = cands[0].nm;
        return out;
    }

    out.tf = best->tf;
    out.nm = best->nm;
    out.dist = bestd;

    return out;
}

// SoundContainer des ECHTEN Messers -- weapons2 spielt den Hit-Sound ueber den
// linken Klon, den es hier nicht gibt.
::REManagedObject* RE4VRChoke::knife_sound_container() {
    ensure_types();

    auto* btf = player_body_tf();

    if (btf == nullptr) {
        return nullptr;
    }

    ::REManagedObject* found = nullptr;

    const std::function<void(::REManagedObject*, int)> walk =
        [&](::REManagedObject* tf, int d) {
            if (tf == nullptr || d < 0 || found != nullptr) {
                return;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
            const std::string nm = obj_name_of(go);

            if (!nm.empty() && is_knife_wp(nm)) {
                auto* sc = get_component(go, m_t_sndc);

                if (sc != nullptr) {
                    found = sc;
                    return;
                }
            }

            auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
            int n = 0;

            while (ch != nullptr && n < 128 && found == nullptr) {
                ++n;
                walk(ch, d - 1);
                ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
            }
        };

    walk(btf, 8);

    return found;
}

// ============================================================================
// Sounds
// ============================================================================

// [STOPP-RUNDUMSCHLAG] Vor unserem Laut werden ALLE laufenden Events des
// Gegners abgewuergt -- drei Wege, exakt wie der STOP-Knopf im Soundplayer.
int RE4VRChoke::stop_enemy_sounds(::REManagedObject* ego, ::REManagedObject* con) {
    if (ego == nullptr || con == nullptr) {
        return 0;
    }

    auto* snd_td = sdk::find_type_definition("soundlib.SoundManager");

    if (snd_td == nullptr) {
        return 0;
    }

    auto* m_stop_byreq = snd_td->get_method(
        "stopEventByRequestId(via.GameObject, System.UInt32, System.UInt32)");
    auto* m_stop_event = snd_td->get_method(
        "stopEvent(via.GameObject, System.UInt32, System.UInt32)");

    int n = 0;

    // (1) laufende Wiedergaben ueber ihre Request-Id
    if (m_stop_byreq != nullptr) {
        // [1:1] Lua probiert das FELD zuerst und faellt erst dann auf den
        // Getter zurueck (Z.660-661).
        auto* dict = re4vr::get_field_object(
            con, "_PlayingRequestIdDict");

        if (dict == nullptr) {
            dict = re4vr::call_safe<::REManagedObject*>(con, "get_PlayingRequestIdDict");
        }

        auto* keys = dict != nullptr
            ? re4vr::call_safe<::REManagedObject*>(dict, "get_Keys")
            : nullptr;
        const int32_t cnt = keys != nullptr ? opt_int(keys, "get_Count").value_or(0) : 0;

        for (int32_t i = 0; i < cnt; ++i) {
            const auto k = opt_int(keys, "get_Item", i);

            if (!k.has_value()) {
                continue;
            }

            auto context = sdk::get_thread_context();

            try {
                m_stop_byreq->call_safe<void*>(context, nullptr, ego,
                                               static_cast<uint32_t>(*k), 0u);
                ++n;
            } catch (...) {
            }

            clear_pending(context, true);
        }
    }

    // (2)+(3) Events der AKTIVEN Trigger-Liste stoppen und den Trigger am
    // Container selbst. BEWUSST nur `_TriggerInfoList`, nicht die ganze Bank.
    auto* list = re4vr::get_field_object(
        con, "_TriggerInfoList");

    if (list == nullptr) {
        return n;
    }

    int32_t size = opt_int(list, "get_Count").value_or(-1);

    if (size < 0) {
        size = re4vr::get_field_int_v(list, "_size");
    }

    for (int32_t i = 0; i < size; ++i) {
        auto* e = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (e == nullptr) {
            continue;
        }

        const auto tid = re4vr::get_field_int(e, "_TriggerId");
        const auto eid = re4vr::get_field_int(e, "_EventId");

        if (m_stop_event != nullptr) {
            if (eid.has_value() && *eid > 0) {
                auto context = sdk::get_thread_context();

                try {
                    m_stop_event->call_safe<void*>(context, nullptr, ego,
                                                   static_cast<uint32_t>(*eid), 0u);
                } catch (...) {
                }

                clear_pending(context, true);
                ++n;
            }

            if (tid.has_value() && *tid > 0) {
                auto context = sdk::get_thread_context();

                try {
                    m_stop_event->call_safe<void*>(context, nullptr, ego,
                                                   static_cast<uint32_t>(*tid), 0u);
                } catch (...) {
                }

                clear_pending(context, true);
            }
        }

        if (tid.has_value() && *tid > 0) {
            re4vr::call_safe<void*>(
                con, "stopTriggered(System.UInt32, via.GameObject, System.UInt32)",
                static_cast<uint32_t>(*tid), ego, 0u);
        }
    }

    return n;
}

// [ZEITPUNKT 2026-08-26] Erst die laufenden Laute abwuergen, den eigenen Laut
// danach starten -- im Zugriffs-Frame geht ein Voice-Event sonst unter.
void RE4VRChoke::play_enemy_grab_sound(::REManagedObject* ego) {
    ensure_types();

    if (m_t_sndc == nullptr || ego == nullptr) {
        return;
    }

    auto* con = get_component(ego, m_t_sndc);

    if (con == nullptr) {
        return;
    }

    if (m_stop_enemy_sounds) {
        stop_enemy_sounds(ego, con);
    }

    // [1:1] Luas math.random ist in 5.4 beim Start automatisch geseedet -- ein
    // ungeseedetes std::rand() lieferte bei jedem Spielstart DIESELBE Lautfolge.
    static bool seeded = false;

    if (!seeded) {
        seeded = true;
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }

    const uint32_t id = ENEMY_GRAB_SOUNDS[static_cast<size_t>(
        std::rand() % static_cast<int>(ENEMY_GRAB_SOUNDS.size()))];

    const double delay = re4vr::lua_get_number("__re4_choke_enemy_snd_delay", ENEMY_SND_DELAY);

    if (delay <= 0.0) {
        re4vr::call_safe<void*>(con, "trigger(System.UInt32)", id);
        return;
    }

    store(m_snd_retry.ego, ego);
    m_snd_retry.id = id;
    m_snd_retry.at.clear();
    m_snd_retry.at.push_back(clock_now() + delay);
}

// [HUHN 2026-09-11] Beliebige Wwise-ID ueber Leons SoundContainer -- derselbe
// Weg wie play_grab_sound, nur mit freier ID.
void RE4VRChoke::play_body_sound(uint32_t id) {
    if (id == 0u) {
        return;
    }

    ensure_types();

    if (m_t_sndc == nullptr) {
        return;
    }

    auto* go = player_body_go();

    if (go == nullptr) {
        return;
    }

    auto* con = get_component(go, m_t_sndc);

    if (con == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(con, "trigger(System.UInt32)", id);
}

void RE4VRChoke::play_grab_sound() {
    ensure_types();

    if (m_t_sndc == nullptr) {
        return;
    }

    auto* go = player_body_go();

    if (go == nullptr) {
        return;
    }

    auto* con = get_component(go, m_t_sndc);

    if (con == nullptr) {
        return;
    }

    re4vr::call_safe<void*>(con, "trigger(System.UInt32)", GRAB_SOUND);
}

// ============================================================================
// Parry / Zustand
// ============================================================================

// Parry-Flag jeden Frame lesen und den Zeitpunkt merken (das Flag steht nur
// wenige Frames).
void RE4VRChoke::poll_parry() {
    auto* ctx = player_ctx();

    if (ctx == nullptr) {
        return;
    }

    const bool parry = opt_bool(ctx, "get_IsParry").value_or(false)
        || opt_bool(ctx, "get_IsBigParry").value_or(false);

    if (parry) {
        m_last_parry = clock_now();

        // Wen haben wir pariert? Der Angreifer steht als AttackSignOwner drin.
        auto* owner = re4vr::call_safe<::REManagedObject*>(ctx, "get_AttackSignOwner");

        if (owner != nullptr) {
            m_parry_victim = reinterpret_cast<uintptr_t>(owner);
        } else if (!m_parry_victim.has_value()) {
            // Rueckfall: der naechste Gegner, der selbst in einem
            // Parry-Zustand steht.
            auto* cm2 = character_manager();
            auto* list = cm2 != nullptr
                ? re4vr::call_safe<::REManagedObject*>(cm2, "get_EnemyContextList")
                : nullptr;
            const int32_t n = list != nullptr ? opt_int(list, "get_Count").value_or(0) : 0;

            for (int32_t i = 0; i < n; ++i) {
                auto* e = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);
                auto* st = e != nullptr
                    ? re4vr::call_safe<::REManagedObject*>(e, "get_ActionState")
                    : nullptr;
                const auto act = st != nullptr ? opt_int(st, "get_Action") : std::nullopt;

                if (!act.has_value() || !in_list(PARRY_ACTIONS, static_cast<uint32_t>(*act))) {
                    continue;
                }

                auto* go = re4vr::call_safe<::REManagedObject*>(e, "get_BodyGameObject");

                if (go != nullptr) {
                    m_parry_victim = reinterpret_cast<uintptr_t>(go);
                    break;
                }
            }
        }
    }

    if ((clock_now() - m_last_parry) >= static_cast<double>(m_cfg.parry_window)) {
        m_parry_victim.reset();
    }
}

bool RE4VRChoke::parry_open() const {
    return (clock_now() - m_last_parry) < static_cast<double>(m_cfg.parry_window);
}

bool RE4VRChoke::is_leon_campaign() {
    if (!m_cfg.leon_campaign_only) {
        return true;
    }

    return obj_name_of(player_body_go()) == LEON_BODY;
}

std::optional<glm::vec3> RE4VRChoke::hand_pos() {
    if (const auto p = re4vr::lua_get_vec3("__vr_lh_world"); p.has_value()) {
        return p;
    }

    auto* btf = player_body_tf();
    auto* j = joint_by_name(btf, PARENT_JOINT);

    if (j == nullptr) {
        return std::nullopt;
    }

    glm::vec3 p{};

    return get_vec3(j, "get_Position", p) ? std::optional<glm::vec3>{p} : std::nullopt;
}

bool RE4VRChoke::is_live(::REManagedObject* ectx) {
    if (ectx == nullptr) {
        return false;
    }

    if (!valid_not_false(ectx)) {
        return false;
    }

    // [HUHN] Kein IsEliminated, kein HitPoint -- gemessen liefern beide
    // "NICHTS". Lebt es noch, sagt GmAnimal.get_IsDead.
    if (m_held.is_animal) {
        // [FLEDERMAUS 13.09.2026] Sie meldet im Flug get_IsDead = true
        // (gemessen). Ohne diese Ausnahme faellt der Griff im selben Frame
        // wieder auseinander, in dem er zustande kam.
        if (m_held.species == SPECIES_BAT) {
            return true;   // valid_not_false steht schon darueber
        }

        return !opt_bool(ectx, "get_IsDead").value_or(false);
    }

    if (opt_bool(ectx, "get_IsEliminated").value_or(false)) {
        return false;
    }

    auto* hp = re4vr::call_safe<::REManagedObject*>(ectx, "get_HitPoint");

    if (hp == nullptr) {
        return false;
    }

    // [LUA-WAHRHEIT] `~= true`: nil zaehlt hier als NICHT lebendig.
    if (opt_bool(hp, "get_IsLive") != std::optional<bool>{true}) {
        return false;
    }

    // [TYP AUS DER TDB] Lua bekommt hier eine generische Zahl. Ist der Getter
    // System.Single, laese ein reines int32 das Float-Bitmuster -- deshalb
    // beide Wege, int zuerst.
    if (const auto v = opt_int(hp, "get_CurrentHitPoint"); v.has_value()) {
        return *v > 0;
    }

    float fv = 0.0f;

    if (re4vr::try_call<float>(hp, "get_CurrentHitPoint", fv)) {
        return fv > 0.0f;
    }

    return false;
}

// [DAMAGE-KATEGORIE] Nur EIN Versuch, nicht jeden Frame durch die TDB.
std::optional<int32_t> RE4VRChoke::cat_damage_value(::REManagedObject* cat_obj,
                                                    bool had_value) {
    if (m_cat_damage.has_value()) {
        return m_cat_damage;
    }

    // [1:1] Drei Faelle, genau wie in Lua:
    //   get_Category lieferte NICHTS      -> nil, _cat_tried bleibt false
    //   es lieferte eine ZAHL (kein Objekt) -> _cat_tried = true, dann nil:
    //                                        Freigabe 1 ist ab jetzt tot
    //   es lieferte ein Objekt            -> normale Suche in der TDB
    if (m_cat_tried || !had_value) {
        return std::nullopt;
    }

    m_cat_tried = true;

    if (cat_obj == nullptr) {
        // Zahl statt Objekt -- in Lua scheitert hier get_type_definition.
        return std::nullopt;
    }

    auto* td = utility::re_managed_object::get_type_definition(cat_obj);

    if (td == nullptr) {
        return std::nullopt;
    }

    for (const char* nm : CAT_DAMAGE_NAMES) {
        if (const auto v = re4vr::enum_value(td->get_full_name().c_str(), nm); v.has_value()) {
            m_cat_damage = *v;
            return m_cat_damage;
        }
    }

    // [GEMESSEN 2026-08-24] Findet die TDB kein passendes Feld, gilt der LIVE
    // gemessene Wert: Idle/Wait 0, Bewegung 1, Angriff 2, Damage 3.
    m_cat_damage = 3;

    return m_cat_damage;
}

// ============================================================================
// [SPITZEN-TREFFER] Klingenlaenge, Spitze, Ziele, Eintritt
// ============================================================================

// Klingenlaenge vom Transform-Ursprung (Griff) bis zur Spitze. Die WorldAABB
// ist achsparallel und ueberschaetzt bei schraegem Messer -- darum wird ueber
// die Frames das MINIMUM je GO-Name behalten.
float RE4VRChoke::blade_len(::REManagedObject* ktf, const std::string& kname,
                            const glm::vec3& u) {
    ensure_types();

    const std::string key = kname.empty() ? std::string{"?"} : kname;
    const auto it = m_blade_len_seen.find(key);
    const bool has_best = it != m_blade_len_seen.end();
    const float best = has_best ? it->second : m_cfg.tip_len;

    if (m_t_mesh == nullptr || ktf == nullptr) {
        return best;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(ktf, "get_GameObject");
    auto* mesh = get_component(go, m_t_mesh);

    // Manche Waffen tragen das Mesh im Kind-GO -- eine Ebene tiefer nachsehen,
    // mehr nicht.
    if (mesh == nullptr) {
        auto* ch = re4vr::call_safe<::REManagedObject*>(ktf, "get_Child");
        int n = 0;

        while (ch != nullptr && n < 8 && mesh == nullptr) {
            ++n;
            auto* cgo = re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject");
            mesh = get_component(cgo, m_t_mesh);
            ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
        }
    }

    if (mesh == nullptr) {
        return best;
    }

    // via.AABB ist ein VALUETYPE (32 Byte: minpos + maxpos), KEIN Objekt --
    // also der sret-Puffer. via.AABB:getCenter liefert MUELL, deshalb min/max
    // selbst lesen (derselbe Weg wie in RE4VRWhitelist).
    auto* mesh_td = utility::re_managed_object::get_type_definition(mesh);

    if (mesh_td == nullptr) {
        return best;
    }

    auto* method = mesh_td->get_method("get_WorldAABB");

    if (method == nullptr) {
        return best;
    }

    __declspec(align(16)) uint8_t aabb_buf[64]{};
    auto context = sdk::get_thread_context();
    bool ok_call = false;

    try {
        method->call_safe<uint8_t*>(aabb_buf, context, mesh);
        ok_call = true;
    } catch (...) {
        ok_call = false;
    }

    re4vr::clear_vm_exception();

    if (!ok_call) {
        return best;
    }

    auto* aabb_td = sdk::find_type_definition("via.AABB");

    if (aabb_td == nullptr) {
        return best;
    }

    auto* f_min = aabb_td->get_field("minpos");
    auto* f_max = aabb_td->get_field("maxpos");

    if (f_min == nullptr || f_max == nullptr) {
        return best;
    }

    const auto off_min = f_min->get_offset_from_fieldptr();
    const auto off_max = f_max->get_offset_from_fieldptr();

    if (off_min + sizeof(glm::vec3) > sizeof(aabb_buf)
        || off_max + sizeof(glm::vec3) > sizeof(aabb_buf)) {
        return best;
    }

    glm::vec3 mn{};
    glm::vec3 mx{};
    std::memcpy(&mn, aabb_buf + off_min, sizeof(mn));
    std::memcpy(&mx, aabb_buf + off_max, sizeof(mx));

    // Leere AABB (min = +FLT_MAX, max = -FLT_MAX) -> unbrauchbar.
    if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) {
        return best;
    }

    glm::vec3 o{};

    if (!get_vec3(ktf, "get_Position", o)) {
        return best;
    }

    // `far` waere hier ein windows.h-MAKRO (altes Segmentierungs-Schluesselwort)
    // und zerlegt die Deklaration -- daher der ausgeschriebene Name.
    float farthest = 0.0f;

    for (int i = 0; i < 8; ++i) {
        const float cx = ((i % 2) == 0) ? mn.x : mx.x;
        const float cy = (((i / 2) % 2) == 0) ? mn.y : mx.y;
        const float cz = (((i / 4) % 2) == 0) ? mn.z : mx.z;
        const float d = (cx - o.x) * u.x + (cy - o.y) * u.y + (cz - o.z) * u.z;

        if (d > farthest) {
            farthest = d;
        }
    }

    // Sanity: ein Messer ist zwischen 10 und 60 cm lang. Alles andere ist eine
    // geplatzte Messung und wird verworfen statt uebernommen.
    if (farthest < 0.10f || farthest > 0.60f) {
        return best;
    }

    if (!has_best || farthest < best) {
        m_blade_len_seen[key] = farthest;
        return farthest;
    }

    return best;
}

// Spitze, Klingenrichtung (Einheitsvektor) und Laenge. +AxisZ ist die
// Klingenrichtung -- gemessen, nicht angenommen (echte Stiche Z=0.92/0.96,
// Ausholen 0.29-0.59).
bool RE4VRChoke::blade_tip(::REManagedObject* ktf, const std::string& kname,
                           glm::vec3& tip, glm::vec3& u, float& len) {
    if (ktf == nullptr) {
        return false;
    }

    glm::vec3 o{};
    glm::vec3 ax{};

    if (!get_vec3(ktf, "get_Position", o) || !get_vec3(ktf, "get_AxisZ", ax)) {
        return false;
    }

    const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);

    if (al < 0.001f) {
        return false;
    }

    u = ax / al;
    len = blade_len(ktf, kname, u);
    tip = o + u * len;

    return true;
}

bool RE4VRChoke::blade_tip_public(::REManagedObject* ktf, const std::string& kname,
                                  glm::vec3& tip, glm::vec3& axis, float& len) {
    return blade_tip(ktf, kname, tip, axis, len);
}

// Die Trefferkoerper des Gehaltenen: Kopf, Hals, Brust -- als drei Kugeln.
// BEWUSST ohne Joint-Namen ausser dem Hals: die heissen je Gegnertyp anders.
bool RE4VRChoke::victim_spheres(std::vector<Sphere>& out) {
    out.clear();

    if (m_held.tf.obj == nullptr || m_held.ctx.obj == nullptr) {
        return false;
    }

    auto* nj = neck_joint_of(m_held.tf.obj);

    if (nj == nullptr) {
        nj = joint_by_name(m_held.tf.obj, NECK_ALT);
    }

    glm::vec3 np{};

    if (nj == nullptr || !get_vec3(nj, "get_Position", np)) {
        return false;
    }

    glm::vec3 rp{};

    // Ohne Wurzel bleibt der Hals allein -- besser als gar kein Treffer.
    if (!get_vec3(m_held.ctx.obj, "get_Position", rp)) {
        out.push_back(Sphere{np, m_cfg.tip_r});
        return true;
    }

    const glm::vec3 d = np - rp;   // Wurzel (Fuesse) -> Hals

    out.push_back(Sphere{np + d * 0.13f, m_cfg.tip_r});          // Kopf
    out.push_back(Sphere{np, m_cfg.tip_r});                      // Hals
    out.push_back(Sphere{np - d * 0.25f, m_cfg.tip_r * 1.4f});   // Brust

    return true;
}

// Geht die Strecke p0->p1 von AUSSEN in die Kugel hinein?
//   * War p0 schon INNERHALB (cc <= 0), gibt es keinen Eintritt -> Herausziehen
//     zaehlt nie.
//   * Der vordere Schnittpunkt muss auf der Strecke liegen (0 <= t <= 1) ->
//     Durchsausen bei hohem Tempo wird erwischt, Vorbeiziehen nicht.
bool RE4VRChoke::sphere_entry(const glm::vec3& p0, const glm::vec3& p1, const Sphere& s,
                              glm::vec3& contact, float& t_out) {
    const glm::vec3 d = p1 - p0;
    const float a = d.x * d.x + d.y * d.y + d.z * d.z;

    if (a < 1e-9f) {
        return false;
    }

    const glm::vec3 f = p0 - s.c;
    const float cc = f.x * f.x + f.y * f.y + f.z * f.z - s.r * s.r;

    if (cc <= 0.0f) {
        return false;   // Spitze war schon drin: kein Eintritt
    }

    const float b = 2.0f * (f.x * d.x + f.y * d.y + f.z * d.z);
    const float disc = b * b - 4.0f * a * cc;

    if (disc < 0.0f) {
        return false;   // geht an der Kugel vorbei
    }

    const float t = (-b - std::sqrt(disc)) / (2.0f * a);

    if (t < 0.0f || t > 1.0f) {
        return false;
    }

    contact = p0 + d * t;
    t_out = t;

    return true;
}

// Der ganze Stich-Erkenner, ein Frame lang. Laeuft NUR waehrend eines Griffs.
void RE4VRChoke::tip_tick(double now) {
    // [ZUPACK-SPERRE 14.09.2026 -- Ansage "es sticht die Gegner in Mercs sofort"]
    //
    // sphere_entry vergleicht die Strecke der KLINGE gegen die Kugeln an ihrem
    // JETZIGEN Ort. Beim Zupacken wird das Opfer aber in die Hand GESETZT --
    // seine Kugeln (Hals/Kopf 0,22 m, Brust x1,4 = 0,31 m) fahren dabei quer
    // ueber die praktisch stehende Klinge. Fuer den Test sieht das aus wie ein
    // sauberer Eintritt von aussen, und die Zupack-Bewegung der Hand liefert
    // das noetige Tempo gleich mit. Der "Stich" kam also vom OPFER, nicht vom
    // Messer -- und wo genau die Kugeln beim Setzen entlangfahren, haengt an
    // den Versatz-Reglern. Deshalb trat es bei manchen Einstellungen auf und
    // bei anderen nie.
    //
    // Die ersten tip_arm Sekunden nach dem Griff wird deshalb gemessen, aber
    // nicht ausgeloest. Danach steht das Opfer fest in der Hand, und ein echter
    // Stich wird unveraendert erkannt.
    const bool arming = (now - m_held.t0) < m_cfg.tip_arm;

    const auto k = knife_transform();

    if (k.tf == nullptr) {
        m_tip_last.p.reset();
        return;
    }

    glm::vec3 tp{};
    glm::vec3 u{};
    float L = 0.0f;

    if (!blade_tip(k.tf, k.nm, tp, u, L)) {
        m_tip_last.p.reset();
        return;
    }

    const auto prev = m_tip_last.p;
    const double pt = m_tip_last.t;
    const std::string pnm = m_tip_last.nm;
    const bool had_nm = m_tip_last.has_nm;

    m_tip_last.p = tp;
    m_tip_last.t = now;
    m_tip_last.nm = k.nm;
    m_tip_last.has_nm = true;

    // Erster Frame im Griff, oder das Messer hat gewechselt: die "Strecke"
    // waere ein Sprung quer durch den Raum und wuerde alles aufspiessen.
    if (!prev.has_value() || !had_nm || pnm != k.nm) {
        return;
    }

    const double dt = now - pt;

    if (dt <= 0.0 || dt > 0.25) {
        return;   // Aussetzer (Menue, Ladebalken)
    }

    const glm::vec3 s = tp - *prev;
    const float seg = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);

    if ((seg / static_cast<float>(dt)) < m_cfg.tip_speed) {
        return;   // reinlehnen/abstellen ist kein Stich
    }

    // [NUR ABWAERTS 2026-08-29, Ansage des Users] Zieht man die Klinge aus dem
    // Hals nach oben heraus, ist das fuer die KOPF-Kugel ein sauberer Eintritt
    // von aussen -- der Rueckweg zaehlte als zweiter Stich. Geprueft wird die
    // Spitzenstrecke GENAU DIESES Frames.
    if (s.y > 0.0f) {
        return;
    }

    std::vector<Sphere> sp;

    if (!victim_spheres(sp) || sp.empty()) {
        return;
    }

    glm::vec3 best{};
    float bestt = 9.9f;
    bool found = false;

    for (const auto& one : sp) {
        glm::vec3 c{};
        float t = 0.0f;

        if (sphere_entry(*prev, tp, one, c, t) && t < bestt) {
            best = c;
            bestt = t;
            found = true;   // der frueheste Eintritt auf der Strecke
        }
    }

    if (!found) {
        return;
    }

    // Waehrend der Zupack-Sperre ist alles gerechnet, der Eintritt wird aber
    // verworfen -- das Einschnappen des Opfers zaehlt nicht als Stich.
    if (arming) {
        return;
    }

    // [EIN TREFFER PRO STICH 2026-08-27] Solange die Klinge in der Kugel
    // steckte, feuerte JEDER Frame die native Kette erneut -- der dritte
    // Aufruf hat das Spiel gerissen (c0000005 in chainsaw.HitManager.hitSetting).
    const double rehit = re4vr::lua_get_number("__re4_choke_tip_rehit", 0.35);

    if ((now - m_tip_hit_t) < rehit) {
        return;
    }

    m_tip_hit_t = now;

    StickInfo st{};
    st.has_c = true;
    st.c = best;
    st.ktf = k.tf;
    st.nm = k.nm;
    st.d = k.dist;
    st.n = k.count;
    st.len = L;
    st.v = seg / static_cast<float>(dt);

    knife_hit(&st);
}

// ============================================================================
// Stich
// ============================================================================

void RE4VRChoke::vlog_add(float v0, float pk, bool stuck) {
    m_vlog.push_back(VLog{v0, pk, stuck});

    while (m_vlog.size() > 6) {
        m_vlog.erase(m_vlog.begin());
    }
}

void RE4VRChoke::knife_hit(const StickInfo* st) {
    if (m_held.ctx.obj == nullptr) {
        return;
    }

    // [ASHLEY-MESSER 15.09.2026] Hier und nur hier wird gezaehlt: beide
    // Trefferwege (Spitzen-Erkenner und die alte Swing-Flanke) laufen genau
    // einmal pro Stich hier durch, und der Spitzen-Weg haelt selbst die
    // Rehit-Sperre von 0,35 s -- ein Stich kann also nicht doppelt zaehlen.
    if (m_held.is_ashley) {
        ashley_stab();
    }

    bool done = false;

    // [EINE ENTSCHEIDUNG 2026-08-27] Die Formel faellt GANZ OBEN und genau
    // einmal -- Schaden, Stummschaltung und Ton lesen dieselbe Zahl.
    const bool has_c = st != nullptr && st->has_c;
    const bool will_stick = m_cfg.knife_stick
        && ((has_c && (m_cfg.knife_stick_v <= 0.0f || st->v >= m_cfg.knife_stick_v))
            || (!has_c && m_cfg.knife_stick_v <= 0.0f));
    const float dmg_want = will_stick ? m_cfg.knife_dmg_stick : m_cfg.knife_dmg;

    if (m_cfg.use_native_hit) {
        // 1:1 wie der LINKE Melee: erst das Tag setzen, sonst laeuft die Kette
        // in ihrem WURF-Zweig.
        re4vr::lua_set_string("__re4_knife_hit_src", "melee");

        // [STECK-TON] Bleibt das Messer stecken, soll NUR KNIFE_STUCK_SND zu
        // hoeren sein -- die Trefferkette wird fuer diesen einen Aufruf
        // stummgeschaltet (Schaden und Blut bleiben unberuehrt).
        if (will_stick) {
            re4vr::lua_set_bool("__re4_knife_hit_mute", true);
        }

        const double r = re4vr::lua_get_number("__re4_knife_reach", 0.9);

        // [SPITZE] Liegt der Durchstosspunkt vor, ist ER der Treffpunkt --
        // nicht die Hand.
        std::optional<glm::vec3> cp{};

        if (has_c) {
            cp = st->c;
        } else if (const auto rh = re4vr::lua_get_vec3("__vr_rh_world"); rh.has_value()) {
            cp = rh;
        } else {
            glm::vec3 p{};

            if (get_vec3(m_held.ctx.obj, "get_Position", p)) {
                cp = p;
            }
        }

        if (m_held.ctx.obj != m_nat_last_ctx) {
            m_nat_last_ctx = m_held.ctx.obj;
            m_nat_n = 0;
        }

        ++m_nat_n;

        // [SCHADEN FUER DIESEN EINEN STICH 2026-08-27] Die native Kette holt
        // ihre Zahl aus __re4_knife_saved_vals.damage. Statt die gemeinsame
        // Datei anzufassen -- sie gehoert auch dem linken Messer -- wird die
        // Tabelle fuer genau diesen Aufruf gegen eine KOPIE mit unserer Zahl
        // getauscht und danach die urspruengliche REFERENZ zurueckgelegt.
        // Alle uebrigen Felder (Wince, Break, Stopping, Waffen-ID) wandern mit.
        // Das laeuft absichtlich in EINEM Helfer: eine Feldaenderung an der
        // bestehenden Tabelle waere nicht dasselbe (bei einer Tabelle ohne
        // `damage` bliebe unser Wert dauerhaft stehen).
        // [WAISE 05.09.2026] Der Helfer tauschte das Feld in der Lua-Tabelle
        // __re4_knife_saved_vals -- die es seit dem Port von weapons2 nicht
        // mehr gibt (die Werte liegen dort nativ in m_saved_vals). Der Tausch
        // ging also ins Leere und BEIDE Choke-Schadensregler waren wirkungslos.
        // Gleiche Semantik nativ: setzen, rufen, in jedem Fall zuruecksetzen.
        if (cp.has_value()) {
            auto& w2 = RE4VRWeapons2::get();

            if (w2 != nullptr) {
                w2->set_damage_override(static_cast<float>(dmg_want));
            }

            done = re4vr::lua_call_global_pos_radius_bool(
                "__re4_knife_direct_damage_at", *cp, static_cast<float>(r));

            if (w2 != nullptr) {
                w2->set_damage_override(std::nullopt);
            }
        }
    }

    // [HUHN] Weder die native Kette (sucht in der EnemyContextList) noch
    // get_HitPoint greifen bei einem GmChicken -- eigener Weg, s. oben.
    if (!done && m_held.is_animal) {
        done = chicken_damage();
    }

    if (!done) {
        auto* hp = re4vr::call_safe<::REManagedObject*>(m_held.ctx.obj, "get_HitPoint");

        if (hp != nullptr) {
            // [TYP AUS DER TDB] Lua reicht eine generische Zahl durch und
            // REFramework konvertiert auf den Parametertyp. Nativ muss der Typ
            // stimmen -- ein int32 an einen System.Single-Parameter landet als
            // Bitmuster im falschen Register.
            add_damage(hp, dmg_want);
        }
    }

    // [STECK-TON] Welcher Ton gehoert zu DIESEM Stich -- dieselbe Zahl, die
    // auch den Schaden ausgesucht hat.
    if (auto* sc = knife_sound_container(); sc != nullptr) {
        re4vr::call_safe<void*>(sc, "trigger(System.UInt32)",
                                will_stick ? KNIFE_STUCK_SND : KNIFE_HIT_SND);
    }

    // Stummschaltung endet mit diesem Stich, egal wie er ausging.
    re4vr::lua_set_nil("__re4_knife_hit_mute");

    if (has_c && m_cfg.knife_stick) {
        // [SPITZEN-WEG] Kein WARTEN mehr: dass die Klinge eingedrungen ist,
        // steht bereits fest. Die WUCHT-SCHWELLE entscheidet weiterhin, ob es
        // STECKENBLEIBT.
        const bool hart = (m_cfg.knife_stick_v <= 0.0f) || (st->v >= m_cfg.knife_stick_v);

        if (hart) {
            stick_into_victim(m_held.ctx.obj, m_held.tf.obj, nullptr, nullptr, st->ktf,
                              st->nm, st->d, st->n, &st->c, st->len);
            release();
        }

        vlog_add(st->v, st->v, hart);
    } else if (m_cfg.knife_stick && m_cfg.knife_stick_v <= 0.0f) {
        // [SOFORT 2026-08-25] Ohne Wucht-Schwelle gibt es nichts zu
        // entscheiden -> stecken im SELBEN Frame. Jede Wartezeit laesst die
        // Hand weiterziehen und das Messer landet neben dem Hals.
        stick_into_victim(nullptr, nullptr, nullptr, nullptr, nullptr, {}, std::nullopt, 0,
                          nullptr, std::nullopt);
        release();
    } else if (m_cfg.knife_stick) {
        m_pend.at = clock_now() + std::max(0.0f, m_cfg.knife_stick_w);
        m_pend.v0 = static_cast<float>(re4vr::lua_get_number("vr_knife_velocity", 0.0));
        // Gegner JETZT festhalten: bis zur Entscheidung ist `held` womoeglich
        // schon leer.
        store(m_pend.ctx, m_held.ctx.obj);
        store(m_pend.tf, m_held.tf.obj);
        m_pend.wp.reset();
        m_pend.wr.reset();

        const auto k0 = knife_transform();
        store(m_pend.ktf, k0.tf);
        m_pend.kname = k0.nm;
        m_pend.kdist = k0.dist;
        m_pend.kcount = k0.count;

        if (k0.tf != nullptr) {
            // [KOPIE 2026-08-25] Die Werte SOFORT in Zahlen ausschreiben --
            // haelt man das ValueType-Objekt fest, zeigt es im naechsten Frame
            // den neuen Wert.
            glm::vec3 p0{};
            glm::quat r0{1.0f, 0.0f, 0.0f, 0.0f};

            if (get_vec3(k0.tf, "get_Position", p0)) {
                m_pend.wp = p0;
            }

            if (get_quat(k0.tf, "get_Rotation", r0)) {
                m_pend.wr = r0;
            }
        }
    }
}

// ============================================================================
// Abdunkeln des steckenden Messers
// ============================================================================
// Gemerkt werden NUR die Variablen, die wir auch anfassen -- und zwar mit
// ihrem Originalwert, damit das Zuruecksetzen exakt ist (nicht "wieder hell
// rechnen", das driftet).

bool RE4VRChoke::dim_build(::REManagedObject* mesh) {
    int32_t mnum = 0;

    if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", mnum) || mnum == 0) {
        return false;
    }

    m_dim.cols.clear();
    m_dim.zeros.clear();

    for (int32_t mi = 0; mi < mnum; ++mi) {
        int32_t vnum = 0;

        if (!re4vr::try_call<int32_t>(mesh, "getMaterialVariableNum", vnum, mi)) {
            continue;
        }

        for (int32_t vi = 0; vi < vnum; ++vi) {
            auto* vn = re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialVariableName",
                                                            mi, vi);

            if (vn == nullptr) {
                continue;
            }

            const std::string low = to_lower(managed_string_of(vn));

            if (low.empty()) {
                continue;
            }

            const bool is_color = low.find("color") != std::string::npos
                || low.find("albedo") != std::string::npos
                || low.find("diffuse") != std::string::npos
                || low.find("basecol") != std::string::npos;

            if (is_color) {
                glm::vec4 f4{};

                if (get_mat_vec4(mesh, "getMaterialFloat4", mi, vi, f4)) {
                    m_dim.cols.push_back(DimVar4{mi, vi, f4});
                }
            } else if (low == "metallic" || low == "cavity") {
                float v = 0.0f;

                if (re4vr::try_call<float>(mesh, "getMaterialFloat", v, mi, vi)) {
                    m_dim.zeros.push_back(DimVar1{mi, vi, v});
                }
            }
        }
    }

    return !m_dim.cols.empty() || !m_dim.zeros.empty();
}

// [KLON-DIM 2026-09-12] Dieselbe Auswahl wie dim_build -- Farbvariablen und
// metallic/cavity -- aber ohne Gedaechtnis: fuer Klone, die im Gegner stecken
// bleiben und am Ende zerstoert werden. Statisch, damit RE4VRWeapons seine
// Wurf-Klone ueber denselben Weg abdunkeln kann.
void RE4VRChoke::dim_mesh_once(::REManagedObject* mesh, float f) {
    if (mesh == nullptr || f >= 1.0f) {
        return;
    }

    int32_t mnum = 0;

    if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", mnum) || mnum == 0) {
        return;
    }

    for (int32_t mi = 0; mi < mnum; ++mi) {
        int32_t vnum = 0;

        if (!re4vr::try_call<int32_t>(mesh, "getMaterialVariableNum", vnum, mi)) {
            continue;
        }

        for (int32_t vi = 0; vi < vnum; ++vi) {
            auto* vn = re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialVariableName",
                                                            mi, vi);

            if (vn == nullptr) {
                continue;
            }

            const std::string low = to_lower(managed_string_of(vn));

            if (low.empty()) {
                continue;
            }

            const bool is_color = low.find("color") != std::string::npos
                || low.find("albedo") != std::string::npos
                || low.find("diffuse") != std::string::npos
                || low.find("basecol") != std::string::npos;

            if (is_color) {
                glm::vec4 v{};

                if (get_mat_vec4(mesh, "getMaterialFloat4", mi, vi, v)) {
                    set_mat_vec4(mesh, "setMaterialFloat4", mi, vi,
                                 glm::vec4{v.x * f, v.y * f, v.z * f, v.w});
                }
            } else if (low == "metallic" || low == "cavity") {
                float v = 0.0f;

                if (re4vr::try_call<float>(mesh, "getMaterialFloat", v, mi, vi)) {
                    set_mat_float(mesh, "setMaterialFloat", mi, vi, v * f);
                }
            }
        }
    }
}

// dark=true dunkel, dark=false zurueck auf die gemerkten Originalwerte.
void RE4VRChoke::dim_apply(bool dark) {
    if (m_dim.mesh.obj == nullptr) {
        return;
    }

    if (!valid_not_false(m_dim.mesh.obj)) {
        drop(m_dim.mesh);
        m_dim.cols.clear();
        m_dim.zeros.clear();
        m_dim.built = false;
        m_dim.on = false;
        return;
    }

    const float f = dark ? m_cfg.knife_dim_f : 1.0f;

    for (const auto& e : m_dim.cols) {
        glm::vec4 v = e.v;

        if (dark) {
            v.x = e.v.x * f;
            v.y = e.v.y * f;
            v.z = e.v.z * f;
        }

        set_mat_vec4(m_dim.mesh.obj, "setMaterialFloat4", e.mi, e.vi, v);
    }

    for (const auto& e : m_dim.zeros) {
        set_mat_float(m_dim.mesh.obj, "setMaterialFloat", e.mi, e.vi,
                      dark ? (e.orig * f) : e.orig);
    }

    m_dim.on = dark;

    if (!dark) {
        drop(m_dim.mesh);
        m_dim.cols.clear();
        m_dim.zeros.clear();
        m_dim.built = false;
    }
}

void RE4VRChoke::dim_start(::REManagedObject* ktf) {
    ensure_types();

    if (!m_cfg.knife_dim || ktf == nullptr || m_t_mesh == nullptr) {
        return;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(ktf, "get_GameObject");

    if (go == nullptr) {
        return;
    }

    auto* mesh = get_component(go, m_t_mesh);

    if (mesh == nullptr) {
        return;
    }

    store(m_dim.mesh, mesh);

    if (!dim_build(mesh)) {
        drop(m_dim.mesh);
        return;
    }

    m_dim.built = true;
    dim_apply(true);
}

// ============================================================================
// Messer im Gegner stecken lassen
// ============================================================================

// Zurueck in die Hand. Wird von JEDEM Ausgang gerufen.
// [CRASH 21.08.] set_Parent auf einem STALEN Transform ist eine native AV, die
// kein pcall faengt -- darum vor jedem Schritt get_Valid. Schlaegt das
// Zurueckhaengen fehl, wird das Messer wenigstens FREI gemacht.
// [HUHN 2026-09-11 -- Testerbefund "Wurf toetet es, Stich nicht"]
// 1:1 der Tier-Zweig aus RE4VRWeapons::break_nearby (dort Z.3170 ff): Tiere
// haben weder WoodBox noch Durability noch einen EnemyContext, also bekommen
// sie ihren eigenen requestAttack mit selbst gebauter DamageUserData. Der
// Angreifer ist derselbe wie beim Wurf -- der HitController des Messers.
bool RE4VRChoke::chicken_damage() {
    auto* ego = m_held.tf.obj != nullptr
        ? re4vr::call_safe<::REManagedObject*>(m_held.tf.obj, "get_GameObject")
        : nullptr;

    if (ego == nullptr) {
        return false;
    }

    auto& w = RE4VRWeapons::get();

    if (w == nullptr) {
        return false;
    }

    auto* hc = w->find_knife_hc();
    auto* atk = hc != nullptr ? w->knife_get_attack_ud(hc) : nullptr;

    // [ADA-TIER 17.09.2026] Messer ohne eigene Angriffsdaten (Adas wp6108):
    // Koerper-HitController + dessen AttackUserData, wie im Tier-Zweig von
    // RE4VRWeapons::break_nearby.
    bool body_path = false;

    if (atk == nullptr) {
        body_path = w->body_attack_fallback(hc, atk);
    }

    if (hc == nullptr || atk == nullptr) {
        return false;
    }

    // [SELBST ERZEUGT] create_instance -> bedingungslos pinnen, sonst ist das
    // Objekt beim naechsten GC weg (dieselbe Falle wie beim Klon).
    auto* dmg = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.DamageUserData", true);

    if (dmg == nullptr) {
        dmg = sdk::create_instance<::REManagedObject>("chainsaw.collision.DamageUserData");
    }

    if (dmg == nullptr) {
        return false;
    }

    // [ADA-TIER 17.09.2026] Den Koerper-HitController hinterher wieder so lassen,
    // wie er war -- das Messer bleibt beim bisherigen Verhalten.
    bool body_was = true;

    if (body_path) {
        re4vr::try_call<bool>(hc, "get_AttackEnable", body_was);
    }

    re4vr::call_safe<void*>(hc, "set_AttackEnable", true);
    re4vr::lua_set_number("__re4_knife_our_until", clock_now() + 0.25);
    re4vr::call_safe<void*>(hc, "requestAttack", ego, atk, dmg);

    if (body_path && !body_was) {
        re4vr::call_safe<void*>(hc, "set_AttackEnable", false);
    }

    return true;
}

void RE4VRChoke::stick_return() {
    // [PIN AUS] Hand-Pin sofort wieder freigeben
    re4vr::lua_set_nil("__re4_choke_knife_stuck");

    // [WEGGESCHLEUDERT] Die in release() aufgeschobene Root Motion jetzt
    // wieder scharf machen.
    if (m_stick.mo.obj != nullptr) {
        auto* mo = m_stick.mo.obj;
        const auto rm = m_stick.rm_was;

        if (rm.has_value() && valid_not_false(mo)) {
            re4vr::call_safe<void*>(mo, "set_RootMotion", *rm);
        }

        drop(m_stick.mo);
        m_stick.rm_was.reset();
    }

    // [ABDUNKELN] Originalfarben zurueck -- IMMER, bevor irgendetwas anderes
    // schiefgehen kann. Das hier ist das ECHTE Messer.
    if (m_dim.on) {
        dim_apply(false);
    }

    // [EINBLENDEN GENAUSO FRUEH 13.09.2026 -- "nach einem Stich ins Huhn kommt
    // das Messer nicht wieder"] Stand hier frueher WEITER UNTEN, hinter dem
    // Ausstieg `if (m_stick.tf.obj == nullptr) return;`. Beim Klon-Weg haelt
    // aber real_tf das echte Messer auf LocalScale 0, waehrend stick.tf der
    // Traeger des alten Weges ist: faellt der weg (Save-Load, Script-Reset,
    // Opfer verschwindet), stieg stick_return() aus, BEVOR das Messer wieder
    // eingeblendet wurde -- und es blieb unsichtbar in der Hand liegen.
    //
    // Dasselbe Prinzip wie beim Abdunkeln direkt darueber: zuerst alles
    // zuruecknehmen, was den Spieler etwas kostet, danach erst aufraeumen.
    if (m_stick.real_hidden) {
        stick_hide_real(m_stick.real_tf.obj, false);
    }

    if (m_stick.tf.obj == nullptr) {
        return;
    }

    auto* ktf = m_stick.tf.obj;
    auto* par = m_stick.parent.obj;
    const std::string jnt = m_stick.joint;
    const auto rest_lp = m_stick.rest_lp;
    const auto rest_lr = m_stick.rest_lr;

    // [GRAPPLE-FLAGS] Vor dem Leeren sichern -- unten wird der Zustand
    // zurueckgesetzt, der Traeger wird danach nicht mehr erreichbar.
    auto* grap_ctx = m_stick.grapple_pending ? m_stick.vic_ctx.obj : nullptr;
    m_stick.grapple_pending = false;

    // Zuerst den Zustand leeren -- danach darf nichts mehr daran haengen.
    Handle keep_tf = m_stick.tf;
    Handle keep_par = m_stick.parent;
    m_stick.tf = Handle{};
    m_stick.parent = Handle{};
    m_stick.joint.clear();
    m_stick.until_t = 0.0;
    re4vr::lua_set_nil("__re4_choke_stick_until");
    drop(m_stick.vic_tf);
    // [1:1] `stick.vic_ctx` wird im Original NICHT genullt -- nur der
    // Diagnoseblock liest es, und der haengt ohnehin an stick.tf.
    m_stick.root0.reset();
    m_stick.check_at.reset();
    m_stick.rest_lp.reset();
    m_stick.rest_lr.reset();
    m_stick.clone_way = false;

    // [GELAENDE-KORREKTUR] Jetzt erst freigeben -- der Nachlauf ist durch, der
    // Koerper steht wieder da, wo die Engine ihn erwartet.
    if (grap_ctx != nullptr && valid_not_false(grap_ctx)) {
        re4vr::call_safe<void*>(grap_ctx, "set_IsConstOnGrapple", false);
        re4vr::call_safe<void*>(grap_ctx, "set_IgnoreTerrainCorrectOnGrapple", false);
    }

    // [STICK-KLON] Die Kopie bleibt bewusst STEHEN -- sie soll im Gegner
    // stecken, auch wenn das Original laengst zurueck in der Hand ist. Um ihr
    // Ende kuemmert sich stick_clone_tick(). Hier wird nur das echte Messer
    // wieder sichtbar gemacht.
    if (m_stick.real_hidden) {
        stick_hide_real(m_stick.real_tf.obj, false);
    }

    if (valid_not_false(ktf)) {
        if (par != nullptr && valid_not_false(par)) {
            re4vr::call_safe<void*>(ktf, "set_Parent(via.Transform)", par);
            set_parent_joint(ktf, jnt.c_str());

            // [POSE ZURUECK statt NULL 2026-09-10 -- Testerbefund] Hier stand
            // pauschal LocalPosition(0,0,0) und gar keine Rotation. Das ist
            // genau das Bild aus der Messung (lokal=0/0/0, lokrot=Identitaet):
            // war `par` der Gegner, blieb das Messer im Ursprung des Joints
            // haengen -- neben dem Gesicht. Jetzt kommt die Lage zurueck, die
            // das Messer VOR dem Stich hatte; nur wenn die fehlt, bleibt es
            // beim alten Verhalten.
            if (rest_lp.has_value()) {
                set_vec3(ktf, "set_LocalPosition", *rest_lp);
            } else {
                set_vec3(ktf, "set_LocalPosition", glm::vec3{0.0f, 0.0f, 0.0f});
            }

            if (rest_lr.has_value()) {
                set_quat(ktf, "set_LocalRotation", *rest_lr);
            }
        } else {
            // [MESSER HEIMATLOS 13.09.2026 -- "ins Messer gesteckt, nun kommt
            // keins mehr aus dem Holster"] Hier stand `set_Parent(nullptr)` --
            // und genau das hat das Messer aus dem Koerperbaum GEWORFEN.
            //
            // Zwei Wege kommen hier an:
            //  * KLON-Weg (der normale): stick_into_victim laesst
            //    m_stick.parent bewusst leer, weil das echte Messer den Baum
            //    nie verlassen hat -- `par` ist also nullptr, und das Nullen
            //    riss es aus seiner Heimat. Im Sondenlog steht danach
            //    "FALL 3: GO existiert, haengt aber NICHT am Spieler".
            //  * ALTER Weg mit verlorener Heimat (Opfer/Body weg): auch dort
            //    ist heimatlos schlechter als "haengt noch irgendwo".
            //
            // Also: gar nichts tun. Das ist die Lehre aus dem stillgelegten
            // Wachhund -- Verlustwege dort reparieren, wo sie entstehen, und
            // NIE ein set_Parent ins Blaue schreiben.
        }
    }

    drop(keep_tf);
    drop(keep_par);
}

// ============================================================================
// [STICK-KLON 2026-09-10] Eine reine Anzeige-Kopie des Messers
// ============================================================================
// WARUM ueberhaupt: bisher wurde die ECHTE Waffe in den Gegner umgehaengt. Sie
// ist aber nur EINMAL da -- also musste sie nach knife_stick_t wieder zurueck,
// und laenger stecken ging nicht. Eine Kopie loest beides: sie darf im Gegner
// bleiben, waehrend das Original ganz normal in der Hand weiterlebt.
//
// BEWUSST EIN EIGENER KLON: RE4VRWeapons2 baut mit derselben Technik das LINKE
// Messer ("vr_lh_knife"). Den mitzubenutzen wuerde dem Spieler das linke Messer
// wegnehmen, sobald er wuergt. Diese Kopie kann deshalb NICHTS ausser stecken:
// kein Schaden, kein Sound, keine Kollision, keine Logik -- nur Mesh.
//
// Bauplan 1:1 aus RE4VRWeapons2 (dort erprobt):
//   create_game_object -> SOFORT pinnen -> via.motion.Motion (ohne Skelett
//   bleibt das Mesh unsichtbar) -> via.render.Mesh -> setMesh + set_Material
//   vom Original.
bool RE4VRChoke::stick_clone_make(::REManagedObject* ktf, ::REManagedObject* htf,
                                  const std::string& joint, const glm::vec3& lp,
                                  const std::optional<glm::quat>& lr) {
    ensure_types();

    if (ktf == nullptr || htf == nullptr || m_t_mesh == nullptr) {
        return false;
    }

    // Vorgaenger weg -- es gibt immer nur EINE Kopie. Sonst haetten wir bei
    // jedem Stich ein Objekt mehr in der Szene.
    stick_clone_drop();

    auto* kgo = re4vr::call_safe<::REManagedObject*>(ktf, "get_GameObject");

    if (kgo == nullptr) {
        return false;
    }

    auto* src_mesh = get_component(kgo, m_t_mesh);

    if (src_mesh == nullptr) {
        return false;
    }

    auto* holder = re4vr::call_safe<::REManagedObject*>(src_mesh, "getMesh");

    if (holder == nullptr) {
        return false;
    }

    auto* src_mat = re4vr::call_safe<::REManagedObject*>(src_mesh, "get_Material");

    auto* go = reinterpret_cast<::REManagedObject*>(
        re4vr::create_game_object("vr_choke_stick_knife"));

    if (go == nullptr) {
        return false;
    }

    // Selbst erzeugt -> BEDINGUNGSLOS pinnen. Die refcount-Heuristik greift
    // hier nicht (dieselbe Falle wie beim linken Messer).
    store(m_stick.clone, go, true);

    if (auto* motion_rt = re4vr::runtime_type("via.motion.Motion")) {
        re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", motion_rt);
    }

    ::REManagedObject* mesh = nullptr;

    if (auto* mesh_rt = re4vr::runtime_type("via.render.Mesh")) {
        mesh = re4vr::call_safe<::REManagedObject*>(go, "createComponent(System.Type)", mesh_rt);
    }

    if (mesh == nullptr) {
        re4vr::destroy_game_object(go);
        drop(m_stick.clone);

        return false;
    }

    store(m_stick.clone_mesh, mesh);

    re4vr::call_safe<void*>(mesh, "setMesh", holder);

    if (src_mat != nullptr) {
        re4vr::call_safe<void*>(mesh, "set_Material", src_mat);
    }

    re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
    re4vr::call_safe<void*>(mesh, "set_Enabled", true);
    re4vr::call_safe<void*>(mesh, "set_FrustumCulling", false);
    re4vr::call_safe<void*>(mesh, "set_DrawShadowCast", false);

    auto* ctf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (ctf == nullptr) {
        re4vr::destroy_game_object(go);
        drop(m_stick.clone_mesh);
        drop(m_stick.clone);

        return false;
    }

    store(m_stick.clone_tf, ctf);

    // Ab hier exakt der Weg, den vorher das echte Messer ging: an den Gegner,
    // an den gewaehlten Joint, und die LOKALE Pose setzen. Weltkoordinaten
    // wirken bei gesetztem ParentJoint nicht (teuer belegt am 24.08.).
    re4vr::call_safe<void*>(ctf, "set_Parent(via.Transform)", htf);
    set_parent_joint(ctf, joint.c_str());
    set_vec3(ctf, "set_LocalPosition", lp);

    if (lr.has_value()) {
        set_quat(ctf, "set_LocalRotation", *lr);
    }

    // Obergrenze, damit nie etwas stehenbleibt, wenn der Gegner nie despawnt.
    m_stick.clone_until = clock_now() + STICK_CLONE_MAX_S;

    return true;
}

void RE4VRChoke::stick_clone_drop() {
    if (m_stick.clone.obj != nullptr) {
        re4vr::destroy_game_object(m_stick.clone.obj);
    }

    drop(m_stick.clone_tf);
    drop(m_stick.clone_mesh);
    drop(m_stick.clone);
    drop(m_stick.clone_vic);
    m_stick.clone_until = 0.0;
}

// Die Kopie ueberlebt den Rueckbau des echten Messers -- sie soll ja stecken
// bleiben. Weg kommt sie erst, wenn ihre Zeit um ist oder ihr Traeger nicht
// mehr taugt (Gegner despawnt, Levelwechsel).
void RE4VRChoke::stick_clone_tick() {
    if (m_stick.clone.obj == nullptr) {
        return;
    }

    bool weg = clock_now() >= m_stick.clone_until;

    // [TOD 2026-09-10 -- Ansage] Die Kopie gehoert IN den Koerper. Sobald der
    // Gegner tot ist, geht er in Ragdoll oder wird abgeraeumt -- ein Messer,
    // das dann noch an einem Joint klebt, steht irgendwann in der Luft.
    // Deshalb: Tod = weg. Gelesen wird der HitController des Traegers.
    if (!weg && m_stick.clone_vic.obj != nullptr) {
        auto* vic = m_stick.clone_vic.obj;

        if (!valid_not_false(vic)) {
            weg = true;
        } else {
            // [TYPRICHTIG] Zahl-Getter nie blind typisieren -- der Wert kommt
            // ueber call_num aus der TDB, nicht als geratenes int32.
            if (const auto hp = re4vr::call_num(vic, "get_CurrentHitPoint");
                hp.has_value() && *hp <= 0.0) {
                weg = true;
            }

            bool live = true;

            if (!weg && re4vr::try_call<bool>(vic, "get_IsLive", live) && !live) {
                weg = true;
            }
        }
    }

    if (!weg) {
        auto* ctf = m_stick.clone_tf.obj;

        if (ctf == nullptr || !valid_not_false(ctf)) {
            weg = true;
        } else {
            auto* par = re4vr::call_safe<::REManagedObject*>(ctf, "get_Parent");

            if (par == nullptr || !valid_not_false(par)) {
                weg = true;
            }
        }
    }

    if (weg) {
        stick_clone_drop();
    }
}

// [HUHN-DREHUNG 12.09.2026 -- Ansage "stimmt nur beim Huhn nicht"] Bei Gegnern
// sitzt die berechnete Pose richtig und bleibt deshalb voellig unangetastet.
// Nur das GmChicken bekommt die drei Korrekturwinkel aufgesetzt -- alle drei
// auf 0 liefert wieder exakt die Quaternion, die hier hereinkommt.
// [ART 13.09.2026] Welcher Reglersatz gilt fuer das Tier in der Hand.
// Die Maus behaelt bewusst die alten animal_*-Felder: ihre eingestellten Werte
// sollen unveraendert weitergelten (Ansage "mach die bitte nicht kaputt").
RE4VRChoke::AnimalTune RE4VRChoke::tune_of_held() const {
    AnimalTune t{};

    // [ASHLEY 13.09.2026] Exklusiv ihr Satz. Seit dem HAND-Bezug (s. held_rot)
    // wirken bei ihr auch die drei Winkel -- sie laeuft denselben Weg wie die
    // necklosen Tiere, nur ueber ihren Neck_1 statt ueber die Huefte.
    if (m_held.is_ashley) {
        t = {m_cfg.ashley_yaw_deg, m_cfg.ashley_pitch_deg, m_cfg.ashley_roll_deg,
             m_cfg.ashley_off_x, m_cfg.ashley_off_y, m_cfg.ashley_off_z};

        return t;
    }

    // [HUHN + GEGNER 13.09.2026] Beide haben ein echtes Neck_1 und liefen
    // deshalb bis heute ueber den alten Weg (eingefrorener Anmarsch-Yaw). Jetzt
    // gilt fuer sie derselbe HAND-Bezug wie fuer Ashley und die necklosen
    // Tiere -- jeder mit eigenem Reglersatz.
    if (!m_held.no_neck) {
        if (m_held.ctx.obj == nullptr) {
            return t;   // nichts in der Hand -> alles 0
        }

        if (m_held.is_animal) {   // Huhn (das einzige Tier MIT Neck_1)
            t = {m_cfg.chick_yaw_deg, m_cfg.chick_pitch_deg, m_cfg.chick_roll_deg,
                 m_cfg.chick_off_x, m_cfg.chick_off_y, m_cfg.chick_off_z};
        } else {                  // Gegner
            t = {m_cfg.ene_yaw_deg, m_cfg.ene_pitch_deg, m_cfg.ene_roll_deg,
                 m_cfg.ene_off_x, m_cfg.ene_off_y, m_cfg.ene_off_z};
        }

        return t;
    }

    switch (m_held.species) {
    case SPECIES_CROW:
        t = {m_cfg.crow_yaw_deg, m_cfg.crow_pitch_deg, m_cfg.crow_roll_deg,
             m_cfg.crow_off_x, m_cfg.crow_off_y, m_cfg.crow_off_z};
        break;

    case SPECIES_BAT:
        t = {m_cfg.bat_yaw_deg, m_cfg.bat_pitch_deg, m_cfg.bat_roll_deg,
             m_cfg.bat_off_x, m_cfg.bat_off_y, m_cfg.bat_off_z};
        break;

    default:   // Maus -- und jedes kuenftige Rig ohne Hals, bis es eigene bekommt
        // [ADA-MAUS 17.09.2026] Adas Maus hat ihren eigenen Satz.
        if (m_held.species == SPECIES_MOUSE && m_held.ada_player) {
            t = {m_cfg.ada_mouse_yaw_deg, m_cfg.ada_mouse_pitch_deg, m_cfg.ada_mouse_roll_deg,
                 m_cfg.ada_mouse_off_x, m_cfg.ada_mouse_off_y, m_cfg.ada_mouse_off_z};
            break;
        }

        t = {m_cfg.animal_yaw_deg, m_cfg.animal_pitch_deg, m_cfg.animal_roll_deg,
             m_cfg.animal_off_x, m_cfg.animal_off_y, m_cfg.animal_off_z};
        break;
    }

    return t;
}

// [HANDDREHUNG JE ART 13.09.2026] Dieselbe Auswahl wie tune_of_held, nur fuer
// die linke Hand. hand_*_deg bleibt der Satz der MAUS bzw. jedes Tieres ohne
// eigene Werte -- so bleiben die eingestellten Winkel von gestern gueltig.
RE4VRChoke::HandTune RE4VRChoke::hand_tune_of_held() const {
    HandTune h{};

    if (m_held.ctx.obj == nullptr) {
        return h;   // nichts in der Hand -> nichts drehen
    }

    if (m_held.is_ashley) {
        return {m_cfg.ashley_hand_yaw, m_cfg.ashley_hand_pitch, m_cfg.ashley_hand_roll};
    }

    if (!m_held.is_animal) {
        return {m_cfg.ene_hand_yaw, m_cfg.ene_hand_pitch, m_cfg.ene_hand_roll};
    }

    if (m_held.is_chicken) {
        return {m_cfg.chick_hand_yaw, m_cfg.chick_hand_pitch, m_cfg.chick_hand_roll};
    }

    switch (m_held.species) {
    case SPECIES_CROW:
        return {m_cfg.crow_hand_yaw, m_cfg.crow_hand_pitch, m_cfg.crow_hand_roll};

    case SPECIES_BAT:
        return {m_cfg.bat_hand_yaw, m_cfg.bat_hand_pitch, m_cfg.bat_hand_roll};

    case SPECIES_MOUSE:
        // [ADA-MAUS 17.09.2026]
        if (m_held.ada_player) {
            return {m_cfg.ada_mouse_hand_yaw, m_cfg.ada_mouse_hand_pitch, m_cfg.ada_mouse_hand_roll};
        }

        return {m_cfg.mouse_hand_yaw, m_cfg.mouse_hand_pitch, m_cfg.mouse_hand_roll};

    default:   // jedes kuenftige Rig ohne eigene Werte
        return {m_cfg.hand_yaw_deg, m_cfg.hand_pitch_deg, m_cfg.hand_roll_deg};
    }
}

// [TUNE-PIN 13.09.2026] Welcher Haken gilt fuer das, was gerade haengt.
bool RE4VRChoke::tune_pin_of_held() const {
    if (m_held.ctx.obj == nullptr) {
        return false;
    }

    if (m_held.is_ashley) {
        return m_cfg.ashley_pin;
    }

    if (!m_held.is_animal) {
        return m_cfg.ene_pin;
    }

    if (m_held.is_chicken) {
        return m_cfg.chick_pin;
    }

    switch (m_held.species) {
    case SPECIES_CROW: return m_cfg.crow_pin;
    case SPECIES_BAT:  return m_cfg.bat_pin;
    // [ADA-MAUS 17.09.2026]
    case SPECIES_MOUSE: return m_held.ada_player ? m_cfg.ada_mouse_pin : m_cfg.animal_pin;
    default:           return m_cfg.animal_pin;
    }
}

// [EIN BEZUG FUER ALLES 14.09.2026 -- Ansage "die Hand und ihr Neck-Joint
// sitzen immer woanders zueinander"] Drehung UND Versatz muessen dieselbe
// Richtung benutzen, sonst wandert das Opfer ueber die Handflaeche, sobald die
// Hand sich bewegt. Diese Funktion liefert den Yaw der HAND -- den Bezug, an
// dem seit heute beides haengt.
//
// Genommen wird die waagerechteste Achse des L_Palm-Joints; welche das ist,
// wird EINMAL pro Spielstart bestimmt und danach nie wieder angefasst.
std::optional<float> RE4VRChoke::hand_basis_yaw() const {
    // ========================================================================
    // [SWING-TWIST 14.09.2026 -- der fuenfte Anlauf, und der erste OHNE
    //  Laufzeit-Entscheidung]
    //
    // Hier stand bisher: von den drei Achsen des L_Palm die WAAGERECHTESTE
    // waehlen, ihr Vorzeichen bestimmen, beides merken. Genau daran ist es
    // gescheitert -- jede dieser Entscheidungen kann kippen, und eine
    // gekippte Entscheidung ist kein kleiner Fehler, sondern 90 oder 180 Grad:
    //
    //   * Log 20:59/21:06 (dev-DLL mit Wahl pro GRIFF): neun Griffe bei
    //     REL_PALM ~ +137, dann einer bei -44,1 -- 181 Grad Sprung mitten in
    //     der Sitzung.
    //   * Davor (Wahl einmal pro SPIELSTART): dieselbe Spiegelung, nur
    //     sitzungsweit -- ashley_yaw musste von +56 auf -134 nachgezogen
    //     werden.
    //
    // Jetzt wird nichts mehr gewaehlt. Die Handdrehung wird in "Drehung um die
    // Welt-Hochachse" (TWIST) und "Rest" (SWING) zerlegt; genommen wird der
    // Twist:
    //
    //     q = (w, x, y, z)  ->  twist um Y  =  2 * atan2(y, w)
    //
    // Das ist eine STETIGE Funktion der Handdrehung: keine Achsenwahl, kein
    // Vorzeichen, kein Kippen bei steil stehendem Joint -- die ganze
    // Fehlerklasse, die uns seit dem 13.09. beschaeftigt, kann hier nicht mehr
    // entstehen. Die Neigung der Hand bleibt draussen, das Opfer steht also
    // weiter aufrecht.
    //
    // Der feste Versatz der Aufnahme (CHOKE_POSE stellt L_Palm absolut) steckt
    // unveraendert mit drin -- er ist eine Konstante und gehoert damit in den
    // Regler der jeweiligen Art, nicht in diese Funktion. Einmal nachziehen
    // ist also faellig.
    //
    // Entartung: nur wenn w UND y ~ 0 sind (Hand exakt 180 Grad um eine
    // waagerechte Achse gedreht). Dann gibt es keinen Twist, und der Aufrufer
    // faellt auf seinen Ersatzbezug zurueck -- so wie bisher auch.
    // ========================================================================
    auto* btf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();
    auto* palm = btf != nullptr ? joint_by_name(btf, PARENT_JOINT) : nullptr;

    if (palm == nullptr) {
        return std::nullopt;
    }

    glm::quat pr{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(palm, "get_Rotation", pr)) {
        return std::nullopt;
    }

    pr = glm::normalize(pr);

    if ((pr.w * pr.w + pr.y * pr.y) < 1.0e-8f) {
        return std::nullopt;
    }

    return 2.0f * std::atan2(pr.y, pr.w);
}

glm::quat RE4VRChoke::held_rot(const glm::quat& base) const {
    // [TIER-DREHUNG 12.09.2026] Frueher haing das an is_chicken. Jetzt an
    // no_neck: Maus, Kraehe und Fledermaus haengen an der Huefte und liegen
    // dadurch anders in der Hand. Huhn und Gegner gehen unveraendert durch.
    //
    // [ASHLEY IMMER GLEICH 13.09.2026 -- Ansage "koennen wir das auch bei
    // ihr hinbekommen"] Sie laeuft denselben Weg: HAND-Bezug statt
    // eingefrorenem Anmarsch-Yaw, und die Ausrichtung sitzt an ihrem GRIFF-
    // JOINT (bei ihr Neck_1, nicht die Huefte). Der Rechenweg darunter kennt
    // den Joint ohnehin nur ueber m_held.grab_joint -- ob das Rig einen Hals
    // hat, spielt fuer ihn keine Rolle.
    //
    // [HUHN + GEGNER 13.09.2026 -- Ansage "kannst du direkt machen"] Damit
    // gilt er jetzt fuer ALLE: die Ausnahme hier ist ersatzlos weg. Wer nichts
    // in der Hand hat oder keinen Griff-Joint kennt, faellt weiter unten auf
    // die alte Zielrotation zurueck.
    if (m_held.ctx.obj == nullptr) {
        return base;
    }

    // ------------------------------------------------------------------
    // [BEZUG = HAND, 13.09.2026 -- gemessen mit zzz_re4_maus_grab_probe.lua]
    //
    // `base` ist der beim Zupacken EINMAL bestimmte Yaw: die Linie vom Tier zum
    // Spielerkoerper, danach eingefroren. Damit haengt die Lage in der Hand
    // daran, aus welcher Richtung man sich dem Tier genaehert hat -- und genau
    // das zeigt der Mitschnitt vom 00:26:
    //
    //   YAW_HUEFTE im Halten, sieben Griffe: -163, -157, -128, -144, -147,
    //   +115, -93 Grad -- INNERHALB eines Griffs jeweils stabil auf 0,3 Grad.
    //   Die Kompensation arbeitet also sauber, sie haelt nur jedes Mal einen
    //   anderen Wert fest. Die Position war im selben Mitschnitt ueber alle
    //   Griffe konstant (VOR +0.225..+0.238, SEIT -0.031..-0.053,
    //   HOCH -0.130..-0.137), und die HAND ebenfalls in Ordnung
    //   (HAND_ZU_LH_WORLD = 0.000).
    //
    // Fuer die necklosen Tiere ist der Bezug deshalb die LINKE HAND: das Tier
    // sitzt in jedem Griff gleich und dreht sich mit dem Handgelenk mit. Die
    // drei Tier-Regler sind der feste Versatz dazu.
    //
    // Faellt der Hand-Joint aus, bleibt es beim alten Verhalten (eingefrorener
    // Anmarsch-Yaw) -- lieber schief als gar keine Ausrichtung.
    // ------------------------------------------------------------------
    glm::quat basis = base;

    {
        // [STABILE BASIS 13.09.2026 -- gemessen] Vorher kam der Yaw aus dem
        // VORWAERTSVEKTOR der Handrotation. Bei der Choke-Handhaltung zeigt
        // diese Achse fast senkrecht, und die Projektion auf die Waagerechte
        // kippt dann bei kleinsten Bewegungen: im Log [CHOKEROT] sprang BASIS
        // INNERHALB eines Griffs zwischen -170.9, -44.8 und -172.5 Grad. Auf so
        // einer Basis kann nichts stabil stehen.
        //
        // Genommen wird deshalb die Linie KOERPER -> HAND, flach. Sie ist ueber
        // die Armlaenge sauber definiert, kippt nicht, und es ist exakt die
        // Bezugsrichtung, mit der auch die Positions-Regler rechnen -- Lage und
        // Drehung haengen damit an derselben Groesse.
        auto* btf = re4vr::fc::on() ? re4vr::fc::body_tf() : re4vr::body_transform();

        glm::vec3 bp{};
        const auto hp = re4vr::lua_get_vec3("__vr_lh_world");

        // [IMMER GLEICH, JETZT WIRKLICH 14.09.2026 -- gemessen an Ashley, gilt
        // genauso fuer Gegner und Huhn] Die Linie Koerper->Hand dreht sich mit,
        // je nachdem WO die Hand steht: im Griff-Log liegen 180 Grad und 130
        // Grad nebeneinander, und die Ausreisser sind genau die Griffe mit der
        // Hand nah an der Koerpermitte (6-7 cm statt 12 cm).
        //
        // Fuer Rigs MIT Hals (Gegner, Ashley, Huhn) ist die Blickrichtung des
        // Koerpers der bessere Bezug: sie ist in jedem Griff dieselbe Groesse,
        // egal wohin die Hand zeigt -- gewuergt wird ohnehin vor dem Koerper.
        //
        // Die necklosen Tiere (Maus, Kraehe, Fledermaus) behalten den
        // HAND-Bezug: sie sollen sich mit dem Handgelenk mitdrehen (Ansage vom
        // 13.09., dort so gemessen und eingestellt).
        // [BEZUG FUER RIGS MIT HALS 14.09.2026]
        //
        // Drei Anlaeufe, alle gemessen:
        //  1. Linie Koerper->Hand -- kippt mit der Handposition: im Griff-Log
        //     standen 180 und 130 Grad nebeneinander, die Ausreisser waren die
        //     Griffe mit der Hand nah an der Koerpermitte.
        //  2. Volle Drehung des L_Palm-Joints -- FALSCH: der Joint ist nicht
        //     aufrecht, seine Neigung kippt das Opfer mit (Messung 00:50:
        //     REL_ROLL -94, REL_PITCH +31, Wurzel 1,4 m seitlich).
        //  3. Blickrichtung des Koerpers, nur der YAW -- das bleibt: sie ist in
        //     jedem Griff dieselbe Groesse, haelt das Opfer aufrecht, und die
        //     Position kommt ohnehin von der Hand (apply_hold), das Opfer sitzt
        //     also in der Handflaeche und schaut immer gleich.
        //
        // Die necklosen Tiere behalten den Linien-Bezug (dort gemessen und
        // eingestellt, Ansage vom 13.09.).
        // [BEZUG = HAND, ABER AUFRECHT 14.09.2026]
        //
        // Vier Anlaeufe, alle gemessen:
        //  1. Linie Koerper->Hand -- kippt mit der Handposition (180 und 130
        //     Grad nebeneinander im Griff-Log).
        //  2. Blickrichtung des Koerpers -- stabil zum KOERPER, aber nicht zur
        //     HAND: verschiebst du die Hand, steht das Opfer anders zu ihr.
        //  3. VOLLE Drehung des L_Palm -- hand-fest und konstant (Messung
        //     00:50: Roll -94,4 zweimal gleich), aber um die Schieflage des
        //     Joints verdreht: das Opfer hing quer.
        //  4. DIESER Weg: die Hand gibt nur den YAW, Neigung und Kippen des
        //     Joints bleiben draussen. Damit dreht sich das Opfer mit dem
        //     Handgelenk, steht aber immer aufrecht -- und die drei Regler je
        //     Art sind der feste Versatz dazu.
        //
        // Welche Joint-Achse dabei "nach vorn" zeigt, ist nicht zu raten (sie
        // liegt bei der Choke-Handhaltung fast senkrecht, und genau daran ist
        // die fruehere Yaw-Projektion gescheitert). Deshalb wird beim GRIFF
        // einmal die WAAGERECHTESTE Achse bestimmt und fuer den ganzen Griff
        // behalten -- die Handpose im Choke ist fest, also faellt bei jedem
        // Griff dieselbe Wahl.
        const bool bezug_hand = !m_held.no_neck;
        const auto hy = bezug_hand ? hand_basis_yaw() : std::nullopt;

        if (hy.has_value()) {
            basis = yaw_quat(*hy);
        } else if (btf != nullptr && hp.has_value() && get_vec3(btf, "get_Position", bp)) {
            const float dx = hp->x - bp.x;
            const float dz = hp->z - bp.z;

            if ((dx * dx + dz * dz) > 0.0025f) {   // ab 5 cm Abstand eindeutig
                basis = yaw_quat(std::atan2(dx, dz));
            } else {
                // [NAH AM KOERPER 14.09.2026 -- gemessen an Ashley] Steht die
                // Hand fast in der Koerpermitte, ist die Linie Koerper->Hand
                // nicht mehr eindeutig. Hier stand bisher gar nichts, es blieb
                // also beim eingefrorenen ANMARSCH-Yaw -- und der ist bei jedem
                // Griff ein anderer. Im Griff-Log sind das die Ausreisser mit
                // REL_YAW +130 statt 180, alle mit nur 6-7 cm Handabstand.
                //
                // Ersatz ist die BLICKRICHTUNG des Koerpers: die ist immer
                // definiert und in jedem Griff dieselbe Groesse.
                glm::quat br{1.0f, 0.0f, 0.0f, 0.0f};

                if (get_quat(btf, "get_Rotation", br)) {
                    const float byaw = std::atan2(
                        2.0f * (br.w * br.y + br.x * br.z),
                        1.0f - 2.0f * (br.y * br.y + br.x * br.x));
                    basis = yaw_quat(byaw);
                }
            }
        }
    }

    const AnimalTune tune = tune_of_held();

    const glm::quat ziel = glm::normalize(
        basis * q_axis(tune.yaw, 0.0f, 1.0f, 0.0f)
              * q_axis(tune.pitch, 1.0f, 0.0f, 0.0f)
              * q_axis(tune.roll, 0.0f, 0.0f, 1.0f));

    // ------------------------------------------------------------------
    // [LAGE AM JOINT 13.09.2026 -- Ansage "bau das mit dem immer gleich
    // sitzen", nachdem "die Animation anhalten ist bloed"]
    //
    // Bisher bekam der TRANSFORM die Zielrotation. Wie das Tier dann in der
    // Hand liegt, haengt aber daran, wo die laufende ANIMATION seine Knochen
    // gerade hinstellt -- deshalb entschied die Pose im Moment des Zupackens
    // ueber Yaw, Pitch und Roll, und zwei Maeuse sassen nie gleich.
    //
    // Ausgerichtet wird darum nicht mehr der Transform, sondern der GRIFF-JOINT
    // (bei diesen Tieren die Huefte):
    //
    //   R_joint = R_transform * R_lokal     (R_lokal ist das, was die Anim tut)
    //   gesucht: R_joint == ziel
    //   also      R_transform = ziel * inverse(R_lokal)
    //
    // R_lokal wird JEDEN Frame frisch gemessen, die Anim laeuft also voll
    // weiter -- sie verschiebt nur nicht mehr die Lage in der Hand. Beide
    // Rotationen stammen aus demselben Frame; dass die Engine direkt nach
    // unseren Writes noch alte Weltmatrizen liefert, faellt damit heraus,
    // weil nur ihr VERHAELTNIS zaehlt.
    //
    // Schlaegt eine der Messungen fehl, bleibt es beim alten Verhalten.
    // ------------------------------------------------------------------
    if (m_held.grab_joint.empty() || m_held.tf.obj == nullptr) {
        return ziel;
    }

    auto* j = joint_by_name(m_held.tf.obj, m_held.grab_joint.c_str());

    if (j == nullptr) {
        return ziel;
    }

    glm::quat rj{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat rt{1.0f, 0.0f, 0.0f, 0.0f};

    if (!get_quat(j, "get_Rotation", rj) || !get_quat(m_held.tf.obj, "get_Rotation", rt)) {
        return ziel;
    }

    const glm::quat lokal = glm::normalize(glm::inverse(rt) * rj);
    const glm::quat out = glm::normalize(ziel * glm::inverse(lokal));

    return out;
}

// Das echte Messer fuer die Steckdauer unsichtbar: LocalScale 0 am Transform,
// derselbe Weg, den der Reload fuer das ausgeworfene Magazin nimmt. Der
// Bone-Scale ueberlebt die Handpose, die motion/holster jeden Frame schreiben.
void RE4VRChoke::stick_hide_real(::REManagedObject* ktf, bool hide) {
    if (ktf == nullptr || !valid_not_false(ktf)) {
        m_stick.real_hidden = false;
        drop(m_stick.real_tf);

        return;
    }

    set_vec3(ktf, "set_LocalScale",
             hide ? glm::vec3{0.0f, 0.0f, 0.0f} : glm::vec3{1.0f, 1.0f, 1.0f});

    m_stick.real_hidden = hide;

    if (!hide) {
        drop(m_stick.real_tf);
    }
}

// [JOINT AM EINSTICH 2026-08-26] Joint-Namen NICHT raten -- sie heissen je
// Gegnertyp anders -- sondern aus get_Joints den naechstgelegenen nehmen.
// [TIER-MITTE 2026-09-12 -- Ansage "immer in der Mitte, wie ein Hip-Joint"]
// Koerpermitte eines Tieres als Mittelpunkt der Welt-AABB seines Meshes.
//
// Warum nicht nach einem Joint namens "Hip" suchen: Huhn, Maus, Kraehe und
// Fledermaus sind eigene Rigs, ein gleich benannter Hueft-Joint ist nirgends
// zugesichert. Der Mesh-Mittelpunkt gilt dagegen fuer jedes Tier, auch fuer
// kuenftige Arten. Denselben Weg nimmt das Messer-Homing bei Tieren schon
// (RE4VRWeapons.cpp, [ANIMAL-CENTER 2026-07-16]).
//
// via.AABB ist ein VALUETYPE (minpos + maxpos), KEIN Objekt -- also der
// sret-Puffer, und getCenter liefert MUELL. Darum min/max selbst lesen,
// derselbe Weg wie in blade_len() weiter oben.
std::optional<glm::vec3> RE4VRChoke::animal_center(::REManagedObject* htf) {
    ensure_types();

    if (htf == nullptr || m_t_mesh == nullptr) {
        return std::nullopt;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(htf, "get_GameObject");
    auto* mesh = get_component(go, m_t_mesh);

    if (mesh == nullptr) {
        return std::nullopt;
    }

    auto* mesh_td = utility::re_managed_object::get_type_definition(mesh);
    auto* method = mesh_td != nullptr ? mesh_td->get_method("get_WorldAABB") : nullptr;

    if (method == nullptr) {
        return std::nullopt;
    }

    __declspec(align(16)) uint8_t aabb_buf[64]{};
    bool ok_call = false;

    try {
        method->call_safe<uint8_t*>(aabb_buf, sdk::get_thread_context(), mesh);
        ok_call = true;
    } catch (...) {
        ok_call = false;
    }

    re4vr::clear_vm_exception();

    if (!ok_call) {
        return std::nullopt;
    }

    auto* aabb_td = sdk::find_type_definition("via.AABB");
    auto* f_min = aabb_td != nullptr ? aabb_td->get_field("minpos") : nullptr;
    auto* f_max = aabb_td != nullptr ? aabb_td->get_field("maxpos") : nullptr;

    if (f_min == nullptr || f_max == nullptr) {
        return std::nullopt;
    }

    const auto off_min = f_min->get_offset_from_fieldptr();
    const auto off_max = f_max->get_offset_from_fieldptr();

    if (off_min + sizeof(glm::vec3) > sizeof(aabb_buf)
        || off_max + sizeof(glm::vec3) > sizeof(aabb_buf)) {
        return std::nullopt;
    }

    glm::vec3 mn{};
    glm::vec3 mx{};
    std::memcpy(&mn, aabb_buf + off_min, sizeof(mn));
    std::memcpy(&mx, aabb_buf + off_max, sizeof(mx));

    // Leere AABB (min = +FLT_MAX, max = -FLT_MAX) -> unbrauchbar.
    if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) {
        return std::nullopt;
    }

    return (mn + mx) * 0.5f;
}

// [TIER-OBERFLAECHE 14.09.2026 -- Ansage "die Spitze soll nur knapp unter das
// Mesh"] Derselbe Weg wie animal_center, nur werden min und max
// durchgereicht. via.AABB ist ein WERTTYP -> sret-Puffer, und getCenter
// liefert Muell; gelesen wird deshalb minpos/maxpos selbst.
bool RE4VRChoke::animal_aabb(::REManagedObject* htf, glm::vec3& mn, glm::vec3& mx) {
    ensure_types();

    if (htf == nullptr || m_t_mesh == nullptr) {
        return false;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>(htf, "get_GameObject");
    auto* mesh = get_component(go, m_t_mesh);

    if (mesh == nullptr) {
        return false;
    }

    auto* mesh_td = utility::re_managed_object::get_type_definition(mesh);
    auto* method = mesh_td != nullptr ? mesh_td->get_method("get_WorldAABB") : nullptr;

    if (method == nullptr) {
        return false;
    }

    __declspec(align(16)) uint8_t aabb_buf[64]{};
    bool ok_call = false;

    try {
        method->call_safe<uint8_t*>(aabb_buf, sdk::get_thread_context(), mesh);
        ok_call = true;
    } catch (...) {
        ok_call = false;
    }

    re4vr::clear_vm_exception();

    if (!ok_call) {
        return false;
    }

    auto* aabb_td = sdk::find_type_definition("via.AABB");
    auto* f_min = aabb_td != nullptr ? aabb_td->get_field("minpos") : nullptr;
    auto* f_max = aabb_td != nullptr ? aabb_td->get_field("maxpos") : nullptr;

    if (f_min == nullptr || f_max == nullptr) {
        return false;
    }

    const auto off_min = f_min->get_offset_from_fieldptr();
    const auto off_max = f_max->get_offset_from_fieldptr();

    if (off_min + sizeof(glm::vec3) > sizeof(aabb_buf)
        || off_max + sizeof(glm::vec3) > sizeof(aabb_buf)) {
        return false;
    }

    std::memcpy(&mn, aabb_buf + off_min, sizeof(mn));
    std::memcpy(&mx, aabb_buf + off_max, sizeof(mx));

    return !(mn.x > mx.x || mn.y > mx.y || mn.z > mx.z);
}

// [TIER-HALS 12.09.2026] s. Header. Erst der exakte Name (Mensch, Huhn), dann
// die gemessene Kette ueber die echten Joint-Namen des Rigs -- klein
// geschrieben verglichen, weil die Maus ihr ganzes Skelett klein fuehrt.
// [ANIM AUF 0 -- 13.09.2026] s. Header.
//
// Die Namen der Layer-Zugriffe stehen nicht fest -- deshalb der Reihe nach
// probieren, genau wie es der Granatenflug bei updateMove_* macht. Was nicht
// existiert, liefert nullptr und wird still uebersprungen; gefunden wird beim
// ersten Treffer.
void RE4VRChoke::animal_anim_rewind(::REManagedObject* mo) {
    if (mo == nullptr) {
        return;
    }

    // [GEMESSEN 13.09.2026, zzz_re4_maus_anim_probe.lua] Die Maus hat GENAU
    // EINEN Layer, erreichbar ueber "getLayer", und darauf wirkt set_Frame:
    //   Griff 1..4: MotionID=20 BankID=0, Frame 11.27 / 86.98 / 55.53 / 7.53
    //   bei End=87 -- immer dieselbe Anim, aber jedes Mal eine andere PHASE.
    //   Nach set_Frame(0) stand dort jedes Mal 0.00.
    // Der Index wird als uint32 UND als int32 versucht: call_safe wandelt
    // Argumente nicht, und welchen der beiden die Signatur verlangt, ist aus
    // der Lua-Seite nicht ablesbar.
    ::REManagedObject* layer =
        re4vr::call_safe<::REManagedObject*>(mo, "getLayer", static_cast<uint32_t>(0));

    if (layer == nullptr) {
        layer = re4vr::call_safe<::REManagedObject*>(mo, "getLayer", static_cast<int32_t>(0));
    }

    if (layer == nullptr) {
        return;
    }

    // [KEIN RUECKGABEWERT] set_Frame liefert void -- ein call_safe<void*> gibt
    // darauf IMMER nullptr, ein Erfolgstest daran waere also immer "fehlgeschlagen"
    // und wuerde jedes Mal zusaetzlich set_Time feuern. Gefragt wird deshalb der
    // Wert selbst, genau wie es die Sonde getan hat.
    re4vr::call_safe<void*>(layer, "set_Frame", 0.0f);

    float f = re4vr::call_safe<float>(layer, "get_Frame");

    if (f > 0.01f) {
        re4vr::call_safe<void*>(layer, "set_Time", 0.0f);
    }
}

std::string RE4VRChoke::neck_joint_name_of(::REManagedObject* tf) {
    if (tf == nullptr) {
        return {};
    }

    // [CASE-FALLE 13.09.2026 -- gemessen mit zzz_re4_maus_phasen_probe.lua]
    // Hier stand `if (joint_by_name(tf, NECK_JOINT) != nullptr) return NECK_JOINT;`
    // -- und getJointByName matcht offenbar OHNE Ruecksicht auf Gross- und
    // Kleinschreibung: bei der Maus lieferte "Neck_1" ihr "neck_1" zurueck.
    // Damit galt sie als Rig MIT Hals, m_held.no_neck blieb false, und weder
    // der Hand-Bezug noch die Anim-Kompensation noch die drei Tier-Regler
    // liefen ueberhaupt an. Im Phasen-Log stand deshalb ueber alle Griffe
    // TRANSFORM pitch=+0.0 roll=+0.0: ein reines Yaw-Quaternion, naemlich der
    // alte eingefrorene Anmarsch-Yaw -- und der ist bei jedem Griff ein anderer.
    //
    // Entschieden wird deshalb an den ECHTEN Joint-Namen des Rigs, exakt
    // verglichen. Mensch und Huhn haben ihr "Neck_1" wirklich so geschrieben
    // und gehen unveraendert durch.

    auto* js = re4vr::call_safe<::REManagedObject*>(tf, "get_Joints");

    if (js == nullptr) {
        return {};
    }

    // [ARRAY, KEINE LISTE] wie in nearest_joint_of -- get_Count/get_Item
    // liefern hier stumm nichts.
    const int32_t cnt = re4vr::array_size(js);

    std::vector<std::pair<std::string, std::string>> namen;   // klein, original
    namen.reserve(static_cast<size_t>(cnt > 0 ? cnt : 0));

    for (int32_t i = 0; i < cnt; ++i) {
        auto* j = re4vr::array_element(js, i);

        if (j == nullptr) {
            continue;
        }

        const std::string nm = obj_name_of(j);

        if (!nm.empty()) {
            namen.emplace_back(to_lower(nm), nm);
        }
    }

    // [ANSAGE 12.09.2026: "nimm doch Hip"] Rangfolge fuer die drei gemessenen
    // Tierrigs: HUEFTE zuerst. Maus fuehrt sie als "hips", Kraehe und
    // Fledermaus als "Hip" -- alle drei sind damit bedient, und das Tier haengt
    // mittig an der Hand statt am Hals. Der Rest der Kette ist nur Rueckfall
    // fuer Rigs, die wir noch nicht gesehen haben.
    static const char* RANG[] = {
        "hips", "hip", "spine_0", "spine_1", "spine_2", "chest",
        "neck_1", "neck", "neck_0", "neck_2", "head",
    };

    // Exakt "Neck_1" hat Vorrang -- das ist der Mensch/das Huhn.
    for (const auto& e : namen) {
        if (e.second == NECK_JOINT) {
            return e.second;
        }
    }

    // [GEGNER UNBERUEHRT 13.09.2026] Ab hier beginnt die TIER-Rangfolge, die
    // auf die Huefte ausweicht. Fuer einen Gegner waere das eine stille
    // Verhaltensaenderung -- und bei Gegnern soll sich nichts aendern (Ansage).
    // Findet sich bei ihnen kein exakt geschriebenes "Neck_1", gilt deshalb
    // weiter der alte Weg: der Aufrufer nimmt NECK_JOINT und laesst
    // getJointByName entscheiden (das vergleicht ohnehin ohne Ruecksicht auf
    // Gross- und Kleinschreibung).
    if (!m_held.is_animal) {
        return {};
    }

    for (const char* want : RANG) {
        for (const auto& e : namen) {
            if (e.first == want) {
                return e.second;
            }
        }
    }

    return {};
}

::REManagedObject* RE4VRChoke::neck_joint_of(::REManagedObject* tf) {
    const std::string nm = neck_joint_name_of(tf);

    // Leer heisst "kein Tier-Sonderweg" -- dann exakt wie frueher ueber
    // NECK_JOINT gehen. So bleibt der Gegner-Choke Bit fuer Bit der alte.
    if (nm.empty()) {
        return joint_by_name(tf, NECK_JOINT);
    }

    return joint_by_name(tf, nm.c_str());
}

::REManagedObject* RE4VRChoke::nearest_joint_of(::REManagedObject* htf, const glm::vec3& p,
                                                std::string& name_out, float& dist_out,
                                                int& n_out) {
    name_out.clear();
    dist_out = -1.0f;
    n_out = 0;

    if (htf == nullptr) {
        return nullptr;
    }

    auto* js = re4vr::call_safe<::REManagedObject*>(htf, "get_Joints");

    if (js == nullptr) {
        return nullptr;
    }

    // [ARRAY, KEINE LISTE] get_Joints liefert ein System.Array. Luas
    // `js:get_elements()` ist ein REFramework-BINDING, keine managed Methode --
    // ein get_Count/get_Item darauf liefert stumm nichts und die Schleife
    // laeuft nie (belegt in re4_vr_weapons.lua:3018, "joints=0 bei JEDEM
    // Gegner"). Dann waere use_joint immer Neck_1 und der Kopfstich haenge
    // wieder am Hals -- genau der Fehler, gegen den der Fix vom 26.08.
    // geschrieben wurde.
    const int32_t cnt = re4vr::array_size(js);

    ::REManagedObject* best = nullptr;
    float bestd = 1e9f;

    for (int32_t i = 0; i < cnt; ++i) {
        // Lua zaehlt JEDES Element mit, auch ein leeres -- 1:1.
        ++n_out;

        auto* j = re4vr::array_element(js, i);

        if (j == nullptr) {
            continue;
        }

        glm::vec3 jp{};

        if (!get_vec3(j, "get_Position", jp)) {
            continue;
        }

        const glm::vec3 d = jp - p;
        const float dd = d.x * d.x + d.y * d.y + d.z * d.z;

        if (dd < bestd) {
            bestd = dd;
            best = j;
        }
    }

    if (best == nullptr) {
        return nullptr;
    }

    name_out = managed_string_of(re4vr::call_safe<::REManagedObject*>(best, "get_Name"));
    dist_out = std::sqrt(bestd);

    return best;
}

void RE4VRChoke::stick_into_victim(::REManagedObject* vctx, ::REManagedObject* vtf,
                                   const glm::vec3* pwp, const glm::quat* pwr,
                                   ::REManagedObject* pktf, const std::string& pkname,
                                   std::optional<float> pkdist, int pkcount,
                                   const glm::vec3* pcontact, std::optional<float> plen) {
    if (!m_cfg.knife_stick || m_stick.tf.obj != nullptr) {
        return;
    }

    auto* hctx = vctx != nullptr ? vctx : m_held.ctx.obj;
    auto* htf = vtf != nullptr ? vtf : m_held.tf.obj;

    if (hctx == nullptr || htf == nullptr) {
        return;
    }

    if (!valid_not_false(htf)) {
        return;
    }

    // [MESSER VOM STICHMOMENT 2026-08-26] Der Kandidat wird im Stichmoment
    // mitgegeben; ohne ihn wie bisher live gesucht.
    ::REManagedObject* ktf = pktf;
    std::string kname = pkname;
    std::optional<float> kdist = pkdist;
    int kcount = pkcount;

    if (ktf == nullptr) {
        const auto k = knife_transform();
        ktf = k.tf;
        kname = k.nm;
        kdist = k.dist;
        kcount = k.count;
    }

    if (ktf == nullptr || !valid_not_false(ktf)) {
        return;
    }

    // [PLAUSIBILITAET] Beim Stich liegt das Messer in der Hand -- also
    // hoechstens eine Armlaenge weg. Sonst lieber gar nicht stecken.
    if (kdist.has_value() && *kdist > 0.60f) {
        return;
    }

    store(m_stick.parent, re4vr::call_safe<::REManagedObject*>(ktf, "get_Parent"));

    {
        auto* pj = re4vr::call_safe<::REManagedObject*>(ktf, "get_ParentJoint");
        m_stick.joint.clear();

        if (pj != nullptr) {
            m_stick.joint = managed_string_of(
                re4vr::call_safe<::REManagedObject*>(pj, "get_Name"));

            if (m_stick.joint.empty()) {
                m_stick.joint = managed_string_of(pj);
            }
        }
    }

    store(m_stick.tf, ktf);
    store(m_stick.vic_tf, htf);
    store(m_stick.vic_ctx, hctx);
    m_stick.until_t = clock_now() + static_cast<double>(m_cfg.knife_stick_t);

    // [STICK-ENDE 2026-09-12] Genau dann kommt das echte Messer zurueck und
    // gehoert wieder auf Scale 1. Der Wachhund in RE4VRWeapons braucht diesen
    // Zeitpunkt, sonst muesste er mit einer erfundenen Frist arbeiten -- der
    // Nachlauf ist ein REGLER (0,1 bis 5 s), keine Konstante.
    re4vr::lua_set_number("__re4_choke_stick_until", m_stick.until_t);

    // [SICHTBAR BLEIBEN] NICHT LocalPosition(0,0,0) -- das ist der URSPRUNG
    // des Joints, also mitten im Koerper. Stattdessen die WELT-Pose vom Moment
    // des Stichs.
    std::optional<glm::vec3> wp{};
    std::optional<glm::quat> wr{};

    if (pwp != nullptr) {
        wp = *pwp;
    } else {
        glm::vec3 p{};

        if (get_vec3(ktf, "get_Position", p)) {
            wp = p;
        }
    }

    if (pwr != nullptr) {
        wr = *pwr;
    } else {
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(ktf, "get_Rotation", r)) {
            wr = r;
        }
    }

    // [SPITZE AN DEN DURCHSTOSSPUNKT] Der Ursprung muss um die Klingenlaenge
    // ZURUECK, dann sitzt die Spitze im Punkt. KNIFE_STICK_IN schiebt darueber
    // hinaus tiefer hinein -- entlang derselben Achse. Die Rotation bleibt
    // unangetastet: wie du zugestochen hast, so steckt es.
    // [REIHENFOLGE GEAENDERT 14.09.2026 -- Ansage "der Einschub ist wirkungslos"]
    // Hier stand frueher `back = Klingenlaenge - knife_stick_in`, der Einschub
    // steckte also schon im Ursprung. Der stick_snap-Block weiter unten zieht
    // den Ursprung danach aber auf hoechstens stick_snap an den Knochen heran
    // -- und hat damit genau die Einschubtiefe wieder weggerechnet.
    //
    // Gemessen an zwei Reglerstellungen des Users: -0,300 ergab back = 0,55 m,
    // geklemmt auf 8,0 cm; +0,185 ergab back = 0,065 m, blieb bei 6,5 cm. Ueber
    // den GANZEN Regelweg also 1,5 cm Unterschied -- im Spiel nicht zu sehen.
    //
    // Jetzt sitzt die Spitze hier exakt im Einstichpunkt, der Snap macht seine
    // Arbeit auf dieser sauberen Lage, und der Einschub kommt DANACH (s. unten).
    if (pcontact != nullptr) {
        glm::vec3 axc{};

        if (get_vec3(ktf, "get_AxisZ", axc)) {
            const float alc = std::sqrt(axc.x * axc.x + axc.y * axc.y + axc.z * axc.z);

            if (alc > 0.001f) {
                wp = *pcontact - (axc / alc) * plen.value_or(m_cfg.tip_len);
            }
        }
    }

    // Hals-Joint EINMAL holen: Bezug fuer den Einschub UND fuer die lokale
    // Pose unten.
    auto* nj_ref = neck_joint_of(htf);

    if (nj_ref == nullptr) {
        nj_ref = joint_by_name(htf, NECK_ALT);
    }

    // [EINSCHUB] Nur noch der alte Weg (TIP.on = false): die Klinge bleibt an
    // ihrer Lage aus dem Stichmoment, KNIFE_STICK_IN schiebt sie ENTLANG IHRER
    // EIGENEN ACHSE tiefer hinein. +AxisZ ist die Klingenrichtung.
    if (pcontact == nullptr && wp.has_value() && m_cfg.knife_stick_in > 0.0f) {
        glm::vec3 ax1{};

        if (get_vec3(ktf, "get_AxisZ", ax1)) {
            const float al1 = std::sqrt(ax1.x * ax1.x + ax1.y * ax1.y + ax1.z * ax1.z);

            if (al1 > 0.001f) {
                const float f1 = m_cfg.knife_stick_in / al1;
                wp = *wp + ax1 * f1;
            }
        }
    }

    // [AUS 2026-08-26] Das Nachdrehen zum Hals gehoert zum selben Fehler wie
    // die Hals-Projektion -- es laeuft nur noch auf ausdrueckliches
    // _G.__re4_choke_aim_blade = true.
    if (wr.has_value() && wp.has_value() && nj_ref != nullptr
        && re4vr::lua_get_tribool("__re4_choke_aim_blade") == 1) {
        glm::vec3 jp0{};
        glm::vec3 ax0{};

        if (get_vec3(nj_ref, "get_Position", jp0) && get_vec3(ktf, "get_AxisZ", ax0)) {
            glm::vec3 b = jp0 - *wp;
            const float bl = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
            const float al = std::sqrt(ax0.x * ax0.x + ax0.y * ax0.y + ax0.z * ax0.z);

            if (bl > 0.001f && al > 0.001f) {
                b /= bl;
                // [VORZEICHEN 2026-08-26] -AxisZ ist hier die Spitze.
                const glm::vec3 an = -ax0 / al;
                float dot = an.x * b.x + an.y * b.y + an.z * b.z;

                if (dot < 0.9995f) {
                    glm::vec3 c{an.y * b.z - an.z * b.y, an.z * b.x - an.x * b.z,
                                an.x * b.y - an.y * b.x};
                    float cl = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);

                    if (cl < 1e-6f) {
                        // Genau entgegengesetzt: irgendeine Achse senkrecht
                        // zur Klinge nehmen.
                        c = glm::vec3{-an.y, an.x, 0.0f};
                        cl = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);

                        if (cl < 1e-6f) {
                            c = glm::vec3{0.0f, -an.z, an.y};
                            cl = 1.0f;
                        }
                    }

                    dot = std::clamp(dot, -1.0f, 1.0f);

                    const float ang = std::acos(dot);
                    const float sh = std::sin(ang * 0.5f);
                    const glm::quat dq{std::cos(ang * 0.5f), c.x / cl * sh, c.y / cl * sh,
                                       c.z / cl * sh};
                    wr = dq * *wr;
                }
            }
        }
    }

    // Der Knochen, an dem es haengt: der dem Einstich naechste.
    ::REManagedObject* jref2 = nullptr;
    std::string jname2;
    float jdist2 = -1.0f;
    int jn2 = 0;

    // [TIER-MITTE 2026-09-12 -- Ansage] Bei TIEREN haengt es nicht am
    // Einstichpunkt, sondern immer an der Koerpermitte: der Joint, der dem
    // Mittelpunkt der Mesh-AABB am naechsten liegt -- faktisch die Hueft-/
    // Rumpfgegend, ohne dass wir die Joint-Namen der Arten kennen muessen.
    // Gilt fuer alle vier Arten (Huhn, Maus, Kraehe, Fledermaus).
    //
    // Bei GEGNERN bleibt alles unveraendert -- dort entscheidet weiter der
    // Einstichpunkt (Ansage: "bei Gegnern ist alles top so wie es ist").
    bool animal_mid = false;

    if (m_held.is_animal) {
        if (const auto ctr = animal_center(htf)) {
            jref2 = nearest_joint_of(htf, *ctr, jname2, jdist2, jn2);
            animal_mid = jref2 != nullptr;
        }
    }

    // Kein Tier, oder die AABB war unbrauchbar -> der bisherige Weg.
    if (jref2 == nullptr && re4vr::lua_get_tribool("__re4_choke_stick_joint_auto") != 0) {
        const glm::vec3 ref = pcontact != nullptr ? *pcontact
                                                  : wp.value_or(glm::vec3{0.0f, 0.0f, 0.0f});

        if (pcontact != nullptr || wp.has_value()) {
            jref2 = nearest_joint_of(htf, ref, jname2, jdist2, jn2);
        }
    }

    // [TIER-HALS 12.09.2026] Fallback nicht mehr blind "Neck_1": an einem
    // Tierrig gibt es den nicht, und ein leerer Joint laesst das Messer am
    // Objektursprung haengen.
    std::string use_joint = jname2;

    if (use_joint.empty()) {
        use_joint = neck_joint_name_of(htf);
    }

    if (use_joint.empty()) {
        use_joint = NECK_JOINT;
    }

    m_stick.joint_used = use_joint;

    // [LOKALE POSE 2026-08-24 -- belegt] `set_Position` (WELT) wirkt hier
    // NICHT: bei gesetztem ParentJoint leitet die Engine die Weltlage jeden
    // Frame aus Joint-Matrix x LocalPosition ab. Also die gewuenschte Weltlage
    // in Joint-Koordinaten umrechnen. Bezug ist der Joint, an dem es JETZT
    // haengt -- sonst sitzt es um genau diesen Versatz daneben.
    // [2026-09-10] Die Pose wird jetzt ZUERST nur BERECHNET -- geschrieben wird
    // sie gleich entweder auf die Kopie (Regelfall) oder, wenn die nicht
    // zustande kommt, wie frueher auf das echte Messer.
    auto* ref_j = jref2 != nullptr ? jref2 : nj_ref;

    // [SNAP 2026-09-10] Den Steckpunkt an den Koerper ziehen, wenn er zu weit
    // ausserhalb liegt. NUR der Ursprung wandert -- Richtung und Winkel der
    // Klinge bleiben unangetastet, es steckt also weiter so, wie zugestochen
    // wurde, nur eben IM Gegner statt daneben. Gezogen wird auf den Joint zu,
    // an dem es gleich haengt; damit stimmt der Bezug der lokalen Pose unten.
    // [TIER-MITTE] Beim Tier wandert der Ursprung GANZ auf den Mitte-Joint --
    // das Messer steckt also mittig im Koerper und nicht dort, wo zugestochen
    // wurde. Der Klingenwinkel bleibt unangetastet, es zeigt weiter in die
    // Stichrichtung. Damit laeuft der stick_snap-Block darunter leer (Abstand
    // ist dann 0), was gewollt ist.
    if (animal_mid && ref_j != nullptr) {
        glm::vec3 jmid{};

        if (get_vec3(ref_j, "get_Position", jmid)) {
            wp = jmid;

            // [SPITZE AUF DIE HAUT 14.09.2026 -- Ansage "die Spitze soll nur
            // knapp unter das Mesh"] Vorher wanderte der Ursprung auf die
            // Koerpermitte und animal_stick_in schob VON DORT entlang der
            // Klinge -- bei einem 12-cm-Huhn steckt damit die halbe Klinge
            // durch das ganze Tier.
            //
            // Jetzt wird die OBERFLAECHE gesucht: von der Mitte GEGEN die
            // Klingenrichtung bis zum Rand der Welt-AABB -- das ist die Stelle,
            // an der die Klinge eintritt. Die Spitze kommt genau dorthin, plus
            // animal_stick_in als TIEFE unter der Haut; der Ursprung ergibt
            // sich daraus um die Klingenlaenge zurueck. Richtung und Winkel
            // bleiben unangetastet.
            //
            // Ist die AABB unbrauchbar, bleibt es beim alten Weg (Mitte plus
            // Versatz) -- lieber zu tief als gar nicht steckend.
            {
                glm::vec3 axa{};

                if (get_vec3(ktf, "get_AxisZ", axa)) {
                    const float ala = std::sqrt(axa.x * axa.x + axa.y * axa.y
                                               + axa.z * axa.z);

                    if (ala > 0.001f) {
                        const glm::vec3 a = axa / ala;

                        glm::vec3 mn{};
                        glm::vec3 mx{};
                        bool haut = false;
                        glm::vec3 surf{};

                        if (animal_aabb(htf, mn, mx)) {
                            const glm::vec3 d = -a;
                            float t_exit = 1.0e9f;

                            for (int i = 0; i < 3; ++i) {
                                const float di = (i == 0) ? d.x : (i == 1) ? d.y : d.z;
                                const float ci = (i == 0) ? jmid.x : (i == 1) ? jmid.y : jmid.z;
                                const float lo = (i == 0) ? mn.x : (i == 1) ? mn.y : mn.z;
                                const float hi = (i == 0) ? mx.x : (i == 1) ? mx.y : mx.z;

                                if (std::fabs(di) < 1.0e-6f) {
                                    continue;   // parallel zu dieser Ebene
                                }

                                const float t1 = (lo - ci) / di;
                                const float t2 = (hi - ci) / di;
                                const float tm = std::max(t1, t2);

                                if (tm > 0.0f && tm < t_exit) {
                                    t_exit = tm;
                                }
                            }

                            if (t_exit < 1.0e8f) {
                                surf = jmid + d * t_exit;
                                haut = true;
                            }
                        }

                        if (haut) {
                            const glm::vec3 spitze = surf + a * m_cfg.animal_stick_in;
                            wp = spitze - a * plen.value_or(m_cfg.tip_len);
                        } else if (m_cfg.animal_stick_in != 0.0f) {
                            wp = *wp + a * m_cfg.animal_stick_in;
                        }
                    }
                }
            }
        }
    }

    if (m_cfg.stick_snap > 0.0f && wp.has_value() && ref_j != nullptr) {
        glm::vec3 jsnap{};

        if (get_vec3(ref_j, "get_Position", jsnap)) {
            const glm::vec3 dv = *wp - jsnap;
            const float dl = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);

            if (dl > m_cfg.stick_snap && dl > 0.0001f) {
                wp = jsnap + dv * (m_cfg.stick_snap / dl);
            }
        }
    }

    // [EINSCHUB ZULETZT 14.09.2026] Erst jetzt, nach dem Snap, die Tiefe
    // entlang der Klinge aufschlagen -- so wirkt der Regler in voller Laenge,
    // und der Snap sieht die Einschubtiefe nicht mehr als "Abstand zum
    // Koerper". Positiv = tiefer hinein, negativ = wieder heraus. Gilt nur fuer
    // GEGNER: beim Tier steckt die Tiefe schon in der Oberflaechen-Rechnung
    // weiter oben, und ein zweites Mal aufschlagen wuerde sie verdoppeln.
    if (pcontact != nullptr && wp.has_value() && !animal_mid
        && m_cfg.knife_stick_in != 0.0f) {
        glm::vec3 axi{};

        if (get_vec3(ktf, "get_AxisZ", axi)) {
            const float ali = std::sqrt(axi.x * axi.x + axi.y * axi.y + axi.z * axi.z);

            if (ali > 0.001f) {
                wp = *wp + (axi / ali) * m_cfg.knife_stick_in;
            }
        }
    }

    glm::vec3 jp{};
    glm::quat jr{1.0f, 0.0f, 0.0f, 0.0f};
    const bool have_j = ref_j != nullptr && get_vec3(ref_j, "get_Position", jp)
        && get_quat(ref_j, "get_Rotation", jr);

    std::optional<glm::vec3> lp_out{};
    std::optional<glm::quat> lr_out{};

    if (wp.has_value() && have_j) {
        lp_out = glm::conjugate(jr) * (*wp - jp);

        if (wr.has_value()) {
            lr_out = glm::conjugate(jr) * *wr;
        }
    }

    // [STICK-KLON 2026-09-10] Der Regelfall: eine Anzeige-Kopie steckt im
    // Gegner, das ECHTE Messer wird nicht angefasst -- es wird nur unsichtbar
    // gemacht und kommt nach knife_stick_t unveraendert zurueck in die Hand.
    // Damit kann die Kopie im Gegner bleiben, ohne dem Spieler das Messer zu
    // nehmen. Klappt das nicht (kein Mesh, kein GameObject, keine Joint-Pose),
    // laeuft unveraendert der alte Weg -- inklusive des reparierten Rueckbaus.
    const bool cloned = lp_out.has_value()
        && stick_clone_make(ktf, htf, use_joint, *lp_out, lr_out);

    if (cloned) {
        store(m_stick.real_tf, ktf);
        stick_hide_real(ktf, true);

        // [KLON-DIM 2026-09-12] Der Klon steckt ab diesem Moment im Opfer --
        // genau ab hier gilt der Regler, nicht frueher.
        if (m_cfg.knife_dim) {
            dim_mesh_once(m_stick.clone_mesh.obj, m_cfg.knife_dim_f);
        }

        // Traeger fuer die Todeserkennung -- eigener Griff, s. Header.
        store(m_stick.clone_vic, hctx);
        m_stick.clone_way = true;

        // Parent bewusst LEER lassen: stick_return() haengt dann nichts um und
        // blendet nur wieder ein. Das echte Messer hat den Baum nie verlassen.
        drop(m_stick.parent);
        m_stick.joint.clear();
        m_stick.rest_lp.reset();
        m_stick.rest_lr.reset();
        m_stick.parented = false;

        // [PIN AUS] gilt nur fuer den alten Weg, in dem die echte Waffe im
        // Gegner hing. Hier bleibt sie in der Hand -- motion darf sie ganz
        // normal fuehren.

        m_stick.check_at = clock_now() + 0.20;

        glm::vec3 rp0c{};

        if (get_vec3(hctx, "get_Position", rp0c)) {
            m_stick.root0 = rp0c;
        } else {
            m_stick.root0.reset();
        }

        return;
    }

    // ------------------------------------------------------------------
    // Alter Weg: die echte Waffe wandert in den Gegner.
    // ------------------------------------------------------------------
    m_stick.clone_way = false;

    // [RUECKBAU-POSE 2026-09-10] VOR dem Umhaengen merken, wie das Messer lag.
    // Ohne das setzt stick_return() beim Zurueckholen pauschal Null -- genau
    // der Fehler, der es neben dem Gesicht haengen liess.
    {
        glm::vec3 lp0{};
        glm::quat lr0{1.0f, 0.0f, 0.0f, 0.0f};

        m_stick.rest_lp = get_vec3(ktf, "get_LocalPosition", lp0)
            ? std::optional<glm::vec3>{lp0} : std::nullopt;
        m_stick.rest_lr = get_quat(ktf, "get_LocalRotation", lr0)
            ? std::optional<glm::quat>{lr0} : std::nullopt;
    }

    re4vr::call_safe<void*>(ktf, "set_Parent(via.Transform)", htf);
    set_parent_joint(ktf, use_joint.c_str());

    if (lp_out.has_value()) {
        set_vec3(ktf, "set_LocalPosition", *lp_out);

        if (lr_out.has_value()) {
            set_quat(ktf, "set_LocalRotation", *lr_out);
        }
    } else {
        // Ohne Joint bleibt nur der Weltweg.
        if (wp.has_value()) {
            set_vec3(ktf, "set_Position", *wp);
        }

        if (wr.has_value()) {
            set_quat(ktf, "set_Rotation", *wr);
        }
    }

    // [NACHGEPRUEFT 2026-08-24] Ob set_Parent wirklich gegriffen hat, sagt nur
    // das Gegenlesen: der Parent MUSS jetzt der Gegner sein.
    auto* par_now = re4vr::call_safe<::REManagedObject*>(ktf, "get_Parent");
    m_stick.parented = par_now != nullptr && par_now == htf;

    // [PIN AUS] re4_vr_motion.lua zieht die Waffe sonst jeden Frame an die
    // Hand zurueck. Zeitstempel, nicht Flag.
    re4vr::lua_set_number("__re4_choke_knife_stuck", clock_now());

    dim_start(ktf);

    // [MESSZEITPUNKT 2026-08-24] set_LocalPosition wirkt erst, wenn die Engine
    // die Weltmatrix neu berechnet -- ein get_Position im selben Frame liefert
    // noch den ALTEN Wert.
    m_stick.check_at = clock_now() + 0.20;

    // [STECK-MESSUNG] Fest eingebaut, ohne Haken: welcher Joint wurde gewaehlt,
    // wie weit war er vom Einstich, wie viele standen zur Wahl. Genau die
    // Zahlen, die in Lua unter DEBUG standen -- ohne sie ist "steckt nicht
    // anstaendig" nicht zu trennen von "falscher Joint" oder "falsche Pose".
    // [STECK-MESSUNG AUSGEBAUT 2026-09-08] Der Mitschnitt nach
    // re4_choke_stick.txt ist raus.

    glm::vec3 rp0{};

    if (get_vec3(hctx, "get_Position", rp0)) {
        m_stick.root0 = rp0;
    } else {
        m_stick.root0.reset();
    }
}

// ============================================================================
// Handpose im Griff
// ============================================================================

// [ASHLEY MUND 16.09.2026] Transform ihres Gesichts-Rigs: es haengt als
// Kind-GameObject "head" unter ihrem Koerper. Ihr Koerper-Rig hat nur 61
// Joints und keinen Kiefer -- die 231 Gesichtsjoints stehen dort.
// [MUND OHNE GRIFF 16.09.2026 -- Ansage des Users] Ashleys Gesichts-Transform
// UNABHAENGIG vom Choke. Bis dahin lief der Mund ueber face_transform_of_held()
// und damit ueber m_held.tf: war der Griff vor dem Ende des Spruchs zu Ende,
// stand ihr Mund mitten im Satz still. Gesucht wird sie hier genauso wie in
// pick_ashley -- ueber die PartnerContextList, nicht ueber den Griff.
//
// Jeden Frame frisch statt gecacht: der Weg laeuft nur, WAEHREND ein Spruch
// spielt (m_mouth_wav >= 0), also ein paar Sekunden am Stueck. Ein Handle
// wuerde dafuer einen Bodywechsel (Szene, Kostuem) ueberleben muessen.
// Ihr PartnerContext -- die eine Stelle, an der sie gesucht wird. Alles
// Weitere (Gesicht, Position) haengt daran.
::REManagedObject* RE4VRChoke::ashley_partner_ctx() {
    auto* cm = character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_PartnerContextList")
        : nullptr;

    if (list == nullptr) {
        return nullptr;
    }

    const int32_t n = opt_int(list, "get_Count").value_or(0);

    for (int32_t i = 0; i < n; ++i) {
        auto* pctx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);
        auto* go = pctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(pctx, "get_BodyGameObject")
            : nullptr;
        const auto bn = obj_name_of(go);

        if (bn == ASHLEY_BODY_NPC || bn == ASHLEY_BODY_PLAYABLE) {
            return pctx;
        }
    }

    return nullptr;
}

::REManagedObject* RE4VRChoke::ashley_face_transform() {
    auto* pctx = ashley_partner_ctx();
    auto* go = pctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(pctx, "get_BodyGameObject")
        : nullptr;
    auto* tf = go != nullptr
        ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
        : nullptr;
    auto* ch = tf != nullptr
        ? re4vr::call_safe<::REManagedObject*>(tf, "get_Child")
        : nullptr;
    int32_t hop = 0;

    while (ch != nullptr && hop < 64) {
        ++hop;

        if (obj_name_of(re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject")) == "head") {
            return ch;
        }

        ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
    }

    return nullptr;
}

// ============================================================================
// [ASHLEY ANTWORTET 16.09.2026 -- Ansage des Users] Zeigt der Spieler IHR den
// Zeige- oder den Stinkefinger, sagt sie etwas dazu. Gerufen von
// RE4VRGuestures::on_frame(), sobald eine Geste zuendet -- dort ist bereits
// geprueft, dass die Haende frei sind und reines Gameplay laeuft, und die Geste
// zuendet genau EINMAL pro Tastendruck.
//
// "Galt sie ihr?" wird am FINGER gemessen, nicht an der Controller-Achse: von
// seinem ersten zu seinem letzten Glied. Diese Richtung ist das, was der
// Spieler sieht, und braucht keine Annahme darueber, wohin ein Controller
// zeigt. Liegt Ashley in einem Kegel von REPLY_ANGLE_DEG darum und naeher als
// REPLY_RANGE_M, war sie gemeint.
// ============================================================================
void RE4VRChoke::ashley_reply(bool fuck) {
    // Ohne das zweite Achievement gibt es die Antworten nicht.
    if (!reply_unlocked()) {
        return;
    }


    if (re4vr::ashley_reply_count(fuck) <= 0) {
        return;   // fuer diese Geste liegen noch keine WAVs in der DLL
    }

    // Ist sie ueberhaupt da?
    auto* pctx = ashley_partner_ctx();

    if (pctx == nullptr) {
        return;
    }

    glm::vec3 ap{};

    if (!get_vec3(pctx, "get_Position", ap)) {
        return;
    }

    // Auf KOPFHOEHE zielen, nicht auf ihre Fusspunkt-Position: aus zwei Metern
    // Abstand liegen dazwischen sonst schon ueber 30 Grad.
    if (auto* face = ashley_face_transform(); face != nullptr) {
        glm::vec3 hp{};

        if (get_vec3(face, "get_Position", hp)) {
            ap = hp;
        }
    }

    // Blickrichtung des Spielers.
    auto* cam = reinterpret_cast<::REManagedObject*>(sdk::get_primary_camera());
    auto* cgo = cam != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cam, "get_GameObject")
        : nullptr;
    auto* ctf = cgo != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cgo, "get_Transform")
        : nullptr;

    if (ctf == nullptr) {
        return;
    }

    glm::vec3 cp{};
    glm::quat crot{};

    if (!get_vec3(ctf, "get_Position", cp) || !get_quat(ctf, "get_Rotation", crot)) {
        return;
    }

    // [ACHSE 16.09.2026, live belegt] Vorwaerts ist MINUS Z. Mit +Z kamen in
    // der Sonde durchweg 157-172 Grad heraus, waehrend der Spieler sie direkt
    // ansah -- also genau die Gegenrichtung.
    const glm::vec3 blick = crot * glm::vec3{0.0f, 0.0f, -1.0f};
    const glm::vec3 zu_ihr = ap - cp;
    const float blick_len = glm::length(blick);
    const float dist = glm::length(zu_ihr);

    if (blick_len < 1e-4f || dist < 1e-3f || dist > REPLY_RANGE_M) {
        return;
    }

    const float cosang = glm::dot(blick / blick_len, zu_ihr / dist);

    if (cosang < std::cos(glm::radians(REPLY_ANGLE_DEG))) {
        return;   // er hat woanders hingesehen, die Geste galt nicht ihr
    }


    // [NICHT JEDES MAL 16.09.2026] Erst ab HIER gezaehlt: eine Geste, die ihr
    // gar nicht galt, soll den Takt nicht verbrauchen. Je Geste ein eigener
    // Zaehler -- wer nur den Mittelfinger zeigt, hoert trotzdem jede fuenfte
    // Antwort und nicht seltener.
    int& zaehler = fuck ? m_reply_grabs_fuck : m_reply_grabs_point;
    int& naechster = fuck ? m_reply_next_fuck : m_reply_next_point;

    ++zaehler;

    if (zaehler < naechster) {
        return;
    }

    zaehler = 0;
    naechster = 4 + (std::rand() % 3);   // 4, 5 oder 6

    const int idx = fuck
        ? re4vr::ashley_reply_wav_next(true, m_reply_bag_fuck, &m_reply_last_fuck)
        : re4vr::ashley_reply_wav_next(false, m_reply_bag_point, &m_reply_last_point);

    if (idx < 0) {
        return;
    }

    // Lautstaerke nach Abstand -- als ZUSCHLAG auf den eingestellten dB-Regler,
    // nicht an seiner Stelle (Ansage 16.09.2026): "0 dB" ist viel zu laut, der
    // Regler steht bewusst darunter. Direkt vor dem Spieler klingt sie also
    // genau so laut wie ihre Choke-Sprueche, und mit dem Abstand faellt sie von
    // DORT aus ab.
    //
    // Abfall mit 1/r, gedeckelt bei REPLY_MIN_DB. PlaySound kennt keine
    // Richtung -- die Entfernung koennen wir abbilden, die Seite nicht.
    const float daempfung = dist <= REPLY_FULL_M
        ? 0.0f
        : std::max(REPLY_MIN_DB, -20.0f * std::log10(dist / REPLY_FULL_M));

    const float db = re4vr::taunt_wav_gain_db() + m_cfg.reply_gain_db + daempfung;

    // [WARTEZEIT 16.09.2026] Nicht sofort: der eigene Taunt laeuft noch, und
    // eine Antwort, die ins Wort faellt, klingt nicht nach Antwort. Gespielt
    // wird in reply_tick(), der Pegel steht hier schon fest.
    if (m_cfg.reply_delay_s > 0.0f) {
        m_reply_pending = idx;
        m_reply_pending_fuck = fuck;
        m_reply_pending_db = db;
        m_reply_due = clock_now() + static_cast<double>(m_cfg.reply_delay_s);

        return;
    }

    m_reply_due = 0.0;
    m_reply_pending = -1;

    re4vr::play_ashley_reply_wav(fuck, idx, db);

    // Der Mund laeuft mit -- seit 16.09. haengt er nur an der WAV, nicht mehr
    // am Griff.
    m_mouth_kind = fuck ? 2 : 1;
    m_mouth_wav = idx;
    m_mouth_t0 = clock_now();
}

// Die wartende Antwort wird hier faellig -- im on_frame, nicht an den Griff
// gebunden: sie kommt auch dann, wenn der Spieler sich inzwischen wegdreht.
void RE4VRChoke::reply_tick() {
    if (m_reply_due <= 0.0 || clock_now() < m_reply_due) {
        return;
    }

    const int idx = m_reply_pending;
    const bool fuck = m_reply_pending_fuck;
    const float db = m_reply_pending_db;

    m_reply_due = 0.0;
    m_reply_pending = -1;

    if (idx < 0) {
        return;
    }

    re4vr::play_ashley_reply_wav(fuck, idx, db);

    m_mouth_kind = fuck ? 2 : 1;
    m_mouth_wav = idx;
    m_mouth_t0 = clock_now();
}

::REManagedObject* RE4VRChoke::face_transform_of_held() {
    auto* tf = m_held.tf.obj;

    if (tf == nullptr) {
        return nullptr;
    }

    auto* c = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int32_t n = 0;

    while (c != nullptr && n < 64) {
        ++n;

        if (obj_name_of(re4vr::call_safe<::REManagedObject*>(c, "get_GameObject")) == "head") {
            return c;
        }

        c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
    }

    return nullptr;
}

// Der Mund folgt dem TON: die Huellkurve der laufenden WAV (ein Wert je 25 ms,
// offline aus der Datei gerechnet) sagt, wie weit er offen ist, und geblendet
// wird zwischen ihren zwei gemessenen Posen. Laeuft kein Spruch, bleibt alles
// unberuehrt -- ihre eigene Mimik gehoert weiter dem Spiel.
void RE4VRChoke::mouth_tick() {
    // [MUND OHNE GRIFF 16.09.2026] Frueher stand hier zusaetzlich
    // !m_held.is_ashley. Der Spruch laeuft jetzt zu Ende, auch wenn der
    // Griff vorher los ist -- unsere WAVs starten ohnehin nur beim GRIFF,
    // eine zweite Sperre ueber das Halten brauchte es dafuer nie.
    if (!m_cfg.mouth_on || m_mouth_wav < 0) {
        return;
    }

    // Huellkurve dieser WAV: beim ersten Mal aus der eingebetteten Datei
    // gerechnet, danach behalten. Neue WAVs sind damit ohne Zutun dabei.
    // [ASHLEY ANTWORTET 16.09.2026] Der Schluessel traegt den Pool mit, sonst
    // bekaeme Antwort Nr. 0 die Huellkurve von Choke-Spruch Nr. 0.
    const int env_key = m_mouth_kind * 1000 + m_mouth_wav;

    if (m_mouth_env.find(env_key) == m_mouth_env.end()) {
        char name[24]{};

        if (m_mouth_kind == 0) {
            std::snprintf(name, sizeof(name), "ASHLEY%02d", m_mouth_wav + 1);
        } else {
            re4vr::ashley_reply_res_name(m_mouth_kind == 2, m_mouth_wav, name, sizeof(name));
        }

        m_mouth_env[env_key] = re4vr::wav_envelope(name);
    }

    const auto& env = m_mouth_env[env_key];

    if (env.empty()) {
        m_mouth_wav = -1;

        return;
    }

    const double t = (clock_now() - m_mouth_t0) * static_cast<double>(m_cfg.mouth_speed);
    const int idx = static_cast<int>(t / 0.025);

    if (idx < 0 || idx >= (int)env.size()) {
        m_mouth_wav = -1;   // Spruch vorbei, Mund gehoert wieder dem Spiel

        return;
    }

    auto* face = ashley_face_transform();

    if (face == nullptr) {
        return;
    }

    const float u = std::clamp((float)env[idx] / 255.0f * m_cfg.mouth_scale, 0.0f, 2.0f);

    for (int i = 0; i < re4vr::ASHLEY_MOUTH_COUNT; ++i) {
        const auto& mp = re4vr::ASHLEY_MOUTH[i];

        if (auto* j = joint_by_name(face, mp.joint)) {
            set_quat(j, "set_LocalRotation", glm::normalize(glm::slerp(mp.zu, mp.offen, u)));
        }
    }

    // [LIPPEN 16.09.2026] Und derselbe Wert auf die POSITIONEN. Ohne das
    // ging der Kiefer auf, die Lippen blieben stehen -- die Engine dreht sie
    // nicht, sie verschiebt sie (C_LowerLip 9,75 mm gegen 2,1 Grad, C_D_Jaw
    // 9,01 mm bei 0,00 Grad). Gemessen, siehe RE4VRAshleyMouth.hpp.
    for (int i = 0; i < re4vr::ASHLEY_MOUTH_SHIFT_COUNT; ++i) {
        const auto& ms = re4vr::ASHLEY_MOUTH_SHIFT[i];

        if (auto* j = joint_by_name(face, ms.joint)) {
            set_vec3(j, "set_LocalPosition", glm::mix(ms.zu, ms.offen, u));
        }
    }
}

void RE4VRChoke::apply_choke_pose() {
    if (!m_cfg.pose_on) {
        return;
    }

    auto* btf = player_body_tf();

    if (btf == nullptr) {
        return;
    }

    for (const auto& e : CHOKE_POSE) {
        auto* j = joint_by_name(btf, e.joint);

        if (j == nullptr) {
            continue;
        }

        // Absolut setzen: die Aufnahme IST die fertige Haltung. Additiv waere
        // hier falsch, dann wuerde die laufende Anim die Pose verschieben.
        set_quat(j, "set_LocalRotation", glm::quat{e.w, e.x, e.y, e.z});
    }

    // [HAND-DREHUNG 12.09.2026] Die HALTENDE linke Hand zusaetzlich drehen.
    // ADDITIV auf die vorhandene LocalRotation: die Hand soll dem Controller
    // weiter folgen und nur um diese Winkel weitergedreht werden. Absolut
    // setzen hiesse gegen die ArmChain schreiben, die L_Hand ebenfalls fuehrt.
    //
    // L_Hand steckt bewusst NICHT in CHOKE_POSE -- die Aufnahme deckt nur die
    // Finger und L_Palm ab, die Handstellung selbst kommt aus dem Controller.
    // NUR beim Halten eines TIERES (Ansage 12.09.): bei Gegnern bleibt die
    // Handstellung unangetastet.
    // [JE ART 13.09.2026] Der Satz kommt jetzt aus hand_tune_of_held und gilt
    // fuer JEDEN Gegriffenen -- Gegner und Ashley eingeschlossen.
    if (const auto ht = hand_tune_of_held(); ht.any()) {
        if (auto* jh = joint_by_name(btf, "L_Hand")) {
            glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

            const glm::quat add =
                q_axis(ht.yaw, 0.0f, 1.0f, 0.0f)
                * q_axis(ht.pitch, 1.0f, 0.0f, 0.0f)
                * q_axis(ht.roll, 0.0f, 0.0f, 1.0f);

            // [NUR DIE MAUS 14.09.2026 -- Ansage "meine Hand steht bei jedem
            // Maus-Grab anders"] Additiv heisst: die drei Winkel liegen auf
            // dem, was die Arm-Chain in diesem Frame geschrieben hat, also auf
            // deiner Controller-Lage -- die absolute Handstellung ist damit bei
            // jedem Griff eine andere.
            //
            // Fuer die MAUS wird der Satz deshalb ABSOLUT gesetzt: die drei
            // Winkel SIND die Handstellung, jeder Maus-Griff sieht gleich aus.
            // Gegner, Ashley, Huhn, Kraehe und Fledermaus bleiben AUSDRUECKLICH
            // auf dem additiven Weg -- dort ist nichts zu reparieren.
            const bool maus = m_held.is_animal && m_held.species == SPECIES_MOUSE;

            if (maus) {
                set_quat(jh, "set_LocalRotation", add);
            } else if (get_quat(jh, "get_LocalRotation", cur)) {
                set_quat(jh, "set_LocalRotation", cur * add);
            }
        }
    }

    // Daumen-Feinjustage obendrauf: je Gelenk X, dann Y, dann Z (additiv).
    const auto thumb = [&](const char* nm, const glm::vec3& t) {
        if (t.x == 0.0f && t.y == 0.0f && t.z == 0.0f) {
            return;
        }

        auto* j = joint_by_name(btf, nm);

        if (j == nullptr) {
            return;
        }

        glm::quat cur{1.0f, 0.0f, 0.0f, 0.0f};

        if (!get_quat(j, "get_LocalRotation", cur)) {
            return;
        }

        glm::quat q = cur;

        if (t.x != 0.0f) {
            q = glm::normalize(q * q_axis(t.x, 1.0f, 0.0f, 0.0f));
        }

        if (t.y != 0.0f) {
            q = glm::normalize(q * q_axis(t.y, 0.0f, 1.0f, 0.0f));
        }

        if (t.z != 0.0f) {
            q = glm::normalize(q * q_axis(t.z, 0.0f, 0.0f, 1.0f));
        }

        set_quat(j, "set_LocalRotation", q);
    };

    thumb("L_Thumb1", m_cfg.thumb_t1);
    thumb("L_Thumb2", m_cfg.thumb_t2);
    thumb("L_Thumb3", m_cfg.thumb_t3);
}

// ============================================================================
// Zielsuche
// ============================================================================

// [TIER 2026-09-10/12 -- Ansage] Tiere sollen sich greifen lassen wie Gegner.
// Sie stehen aber NICHT in der EnemyContextList (das sind Gimmick-Objekte),
// also eigene Quelle: die Szene nach den Tier-Typen absuchen.
//
// Gegriffen werden (seit 12.09., Ansage) ALLE vier Tierarten: GmChicken,
// GmMouse, GmCrow und GmBat. In der Szene stehen laut Messung vom 10.09.
// 25 Tiere. Kraehe und Fledermaus fliegen -- ob der Griff an einem fliegenden
// Tier ueberhaupt sinnvoll aussieht, muss der Test zeigen.
//
// Huhn gemessen am lebenden Objekt (10.09., 23:47): 59 Joints inklusive
// Neck_0..Neck_3, Head und Jaw -- der Griff findet sein Neck_1 also vor, ohne
// Umbau. Dazu via.motion.Motion (Root Motion), via.physics.CharacterController
// und chainsaw.HitController. Was fehlt, ist get_HitPoint am GmChicken selbst
// und jede Art von ActionState -- beides faengt der Tier-Zweig ab.
//
// ACHTUNG: Das Skelett der MAUS ist NICHT gemessen. Hat sie kein Neck_1,
// findet der Griff seinen Joint nicht und sie bleibt liegen, wo sie ist.
// Das ist dann eine Messung wert, kein Blindumbau.
//
// chicken_out meldet, ob es ein HUHN wurde: nur das Huhn bekommt in held_rot
// die drei gemessenen Korrekturwinkel, bei jedem anderen Tier waere die Pose
// damit verdreht.
::REManagedObject* RE4VRChoke::pick_animal(const glm::vec3& hp, float max_d,
                                           int* species_out, bool* chicken_out) {
    if (chicken_out != nullptr) {
        *chicken_out = false;
    }

    if (species_out != nullptr) {
        *species_out = SPECIES_NONE;
    }

    // Szene holen -- 1:1 der Weg aus RE4VRCrosshair::get_scene(): via.SceneManager
    // ist ein NATIVES Singleton, get_managed_singleton liefert dafuer nichts.
    ::REManagedObject* scene = nullptr;

    {
        const auto sm = sdk::get_native_singleton("via.SceneManager");
        auto* smtd = sdk::find_type_definition("via.SceneManager");

        if (sm == nullptr || smtd == nullptr) {
            return nullptr;
        }

        const auto method = smtd->get_method("get_CurrentScene");

        if (method == nullptr) {
            return nullptr;
        }

        try {
            scene = method->call_safe<::REManagedObject*>(sdk::get_thread_context(), sm);
        } catch (...) {
            scene = nullptr;
        }
    }

    if (scene == nullptr) {
        return nullptr;
    }

    // Reihenfolge egal -- entschieden wird allein ueber den Abstand zur Hand.
    struct AnimalType {
        const char* name;
        bool        is_chicken;
        int         species;   // [ART 13.09.2026] entscheidet den Reglersatz
        bool        ignore_dead;   // [FLEDERMAUS 13.09.2026] s. unten
    };
    // ACHTUNG Herkunft: GmChicken und GmMouse sind im Code BELEGT (GmChicken
    // ueber die eigene Suche hier, GmMouse ueber den Typvergleich im
    // Messer-Homing, RE4VRWeapons.cpp:3813). GmCrow und GmBat stehen dagegen
    // nur in unseren eigenen KOMMENTAREN von der Messung am 10.09. ("25 Tiere
    // in der Szene") -- nirgends in ausfuehrbarem Code. Schreibfehler faellt
    // nicht auf: type_of() liefert dann nullptr und der Typ wird still
    // uebersprungen. Also beim ersten Test gegenpruefen, ob Kraehe und
    // Fledermaus wirklich greifbar sind.
    // [FLEDERMAUS LEBT ANGEBLICH NICHT 13.09.2026 -- gemessen mit
    // zzz_re4_fledermaus_check.lua beim Vorbeiflug in der Kanalisation]
    // Die zwei Fledermaeuse, die ueber den Spieler hinwegfliegen, melden
    // get_IsDead = TRUE, die zwei weit entfernt geparkten melden false. Sie war
    // 0,47 m von der Hand entfernt (Greifweite 1,30 m) und wurde trotzdem
    // uebersprungen -- der Griff konnte also nie klappen.
    // Deshalb gilt die Totenpruefung fuer die Fledermaus NICHT. Huhn, Maus und
    // Kraehe behalten sie unveraendert: dort ist sie belegt sinnvoll (tote
    // Huehner liegen herum).
    static constexpr AnimalType ANIMALS[] = {
        {"chainsaw.GmChicken", true,  SPECIES_NONE,  false},
        {"chainsaw.GmMouse",   false, SPECIES_MOUSE, false},
        {"chainsaw.GmCrow",    false, SPECIES_CROW,  false},
        {"chainsaw.GmBat",     false, SPECIES_BAT,   true},
    };

    ::REManagedObject* best = nullptr;
    // Obergrenze fuer den Vergleich: die groesste vorkommende Reichweite. Die
    // ART-eigene Grenze steht unten in kind_max und entscheidet wirklich.
    float bestd = std::max(max_d, m_cfg.bat_grab_dist);
    bool  best_chicken = false;
    int   best_species = SPECIES_NONE;

    for (const auto& kind : ANIMALS) {
        // [FLEDERMAUS-RADIUS 13.09.2026 -- Ansage "erweitere von mir aus NUR
        // bei der Fledermaus den Radius"] Sie fliegt vorbei, statt zu stehen:
        // gemessen war sie im dichtesten Frame 0,22 m entfernt, zwei Frames
        // spaeter schon ueber 1 m. Mit der normalen Greifweite muesste man den
        // einen Frame treffen. Ihr eigener Radius gilt AUSSCHLIESSLICH fuer
        // chainsaw.GmBat -- Huhn, Maus und Kraehe behalten grab_dist.
        const float kind_max = kind.species == SPECIES_BAT
            ? std::max(max_d, m_cfg.bat_grab_dist) : max_d;
        auto* t = type_of(kind.name);

        if (t == nullptr) {
            continue;   // Typ in dieser Stage unbekannt -> ueberspringen
        }

        auto* arr = re4vr::call_safe<::REManagedObject*>(
            scene, "findComponents(System.Type)", t);

        if (arr == nullptr) {
            continue;
        }

        const int32_t cnt = re4vr::array_size(arr);

        for (int32_t i = 0; i < cnt; ++i) {
            auto* c = re4vr::array_element(arr, i);

            if (c == nullptr || !valid_not_false(c)) {
                continue;
            }

            // Totes Tier nicht greifen -- GmAnimal.get_IsDead, derselbe Weg, den
            // das Messer-Homing bei Tieren nimmt.
            // [FLEDERMAUS 13.09.2026] Ausnahme s. Tabelle oben: die fliegenden
            // melden sich selbst als tot.
            if (!kind.ignore_dead && opt_bool(c, "get_IsDead").value_or(false)) {
                continue;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(c, "get_GameObject");
            auto* tf = go != nullptr
                ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform") : nullptr;

            if (tf == nullptr) {
                continue;
            }

            glm::vec3 rp{};

            if (!get_vec3(tf, "get_Position", rp)) {
                continue;
            }

            // Ein Tier ist flach -- die Hoehenfenster der Gegner (hand_min_y/max_y
            // messen gegen die Fuesse eines stehenden Ganado) passen hier nicht.
            // Gegriffen wird schlicht nach Abstand zur Hand.
            const float d = glm::length(rp - hp);

            if (d < kind_max && d < bestd) {
                bestd = d;
                best = c;
                best_chicken = kind.is_chicken;
                best_species = kind.species;
            }
        }
    }

    if (chicken_out != nullptr) {
        *chicken_out = best_chicken;
    }

    if (species_out != nullptr) {
        *species_out = best_species;
    }

    return best;
}

::REManagedObject* RE4VRChoke::pick_target(const glm::vec3& hp) {
    // [KRAUSER-SPERRE AUFGEHOBEN 2026-09-07, Ansage des Users] Hier stand ein
    // Block auf __re4_holster_knife_only -- damit war der Choke in Stage 55302
    // aus, solange Krauser in der Szene ist. Choke und Messerwurf sollen dort
    // wieder gehen; das Holster bleibt unveraendert gesperrt (die fuenf
    // Abfragen in RE4VRHolster sind NICHT angefasst), also weiter versteckte
    // Holster und nur das Messer in der Hand. Wirkt ausschliesslich in dieser
    // Stage -- ausserhalb ist das Global ohnehin false.

    auto* cm = character_manager();
    auto* list = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList")
        : nullptr;

    if (list == nullptr) {
        return nullptr;
    }

    const int32_t n = opt_int(list, "get_Count").value_or(0);

    ::REManagedObject* best = nullptr;
    float bestd = m_cfg.grab_dist;

    for (int32_t i = 0; i < n; ++i) {
        auto* ectx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);

        if (!is_live(ectx)) {
            continue;
        }

        glm::vec3 rp{};

        if (!get_vec3(ectx, "get_Position", rp)) {
            continue;
        }

        const float dy = hp.y - rp.y;
        const float d = horiz(rp, hp);

        // Erst die billigen Bedingungen: Hoehe der Hand ueber seinen Fuessen
        // und Abstand.
        if (dy < m_cfg.hand_min_y || dy > m_cfg.hand_max_y || d >= bestd) {
            continue;
        }

        // [SCHLANGE RAUS 16.09.2026 -- Ansage des Users] Die Schlange ist nicht
        // greifbar, auch nicht als gewoehnlicher Gegner im Hoehenfenster.
        if (auto* sdef = utility::re_managed_object::get_type_definition(ectx);
            sdef != nullptr) {
            try {
                if (sdef->get_full_name().find("Ch8g2z0") != std::string::npos) {
                    continue;
                }
            } catch (...) {
            }
        }

        // FREIGABE 1: er spielt gerade eine Trefferreaktion. Statt einzelne
        // Hashes zu sammeln wird die KATEGORIE geprueft.
        auto* st0 = re4vr::call_safe<::REManagedObject*>(ectx, "get_ActionState");
        const auto act0 = st0 != nullptr ? opt_int(st0, "get_Action") : std::nullopt;

        // [1:1] Lua reicht an cat_damage_value IMMER das Ergebnis von
        // get_Category durch -- auch dann, wenn das eine blanke Zahl ist. In
        // dem Fall laeuft dort get_type_definition in den safe()-Fehlerfall,
        // die Funktion liefert nil, und weil _cat_tried VORHER gesetzt wird,
        // bleibt Freigabe 1 fuer den Rest der Sitzung TOT.
        //
        // [ABSTURZ 04.09.2026] Nativ darf das Ergebnis NICHT blind als Objekt
        // genommen werden: get_Category liefert ein Enum, also die blanke Zahl.
        // Als Zeiger gelesen ist das die Adresse 3, und die erste
        // Dereferenzierung reisst das Spiel (Dump: mov rax,[rcx], rcx=3).
        // Deshalb erst den Rueckgabetyp fragen.
        ::REManagedObject* cat_obj = nullptr;
        std::optional<int32_t> catn{};
        bool cat_had_value = false;

        if (st0 != nullptr) {
            if (getter_returns_object(st0, "get_Category")) {
                cat_obj = re4vr::call_safe<::REManagedObject*>(st0, "get_Category");
                cat_had_value = cat_obj != nullptr;

                if (cat_obj != nullptr) {
                    catn = re4vr::get_field_int(cat_obj, "value__");
                }
            } else {
                // Wertetyp -> die Zahl IST das Ergebnis. In Lua kommt hier
                // genau dieselbe Zahl an.
                catn = opt_int(st0, "get_Category");
                cat_had_value = catn.has_value();
            }
        }

        bool ok_state = false;

        // USE_DAMAGE_CAT ist im Original eine feste local = true.
        // [KURZSCHLUSS 1:1] `catn ~= nil and catn == cat_damage_value(cat0)` --
        // bei fehlender Kategorie wird die Funktion GAR NICHT gerufen, und
        // _cat_tried bleibt false. Sonst waere der eine TDB-Versuch schon am
        // ersten Gegner mit unlesbarer Kategorie verbraucht.
        const bool cat_hit = catn.has_value()
            && cat_damage_value(cat_obj, cat_had_value) == catn;

        if (cat_hit
            || (act0.has_value() && in_list(GRABBABLE_ACTIONS, static_cast<uint32_t>(*act0)))) {
            ok_state = true;
        } else if (m_cfg.require_parry && m_cfg.parry_target_only && m_parry_victim.has_value()) {
            // FREIGABE 2: genau der, den wir gerade pariert haben
            auto* go = re4vr::call_safe<::REManagedObject*>(ectx, "get_BodyGameObject");
            ok_state = go != nullptr && reinterpret_cast<uintptr_t>(go) == *m_parry_victim;
        } else if (m_cfg.require_parry) {
            ok_state = parry_open()
                || (act0.has_value() && in_list(PARRY_ACTIONS, static_cast<uint32_t>(*act0)));
        } else {
            ok_state = true;   // Gate aus: jeder in Reichweite
        }

        // GROESSE zuletzt: der Hals-Joint wird nur noch am echten Kandidaten
        // angefasst. Vorher lief das ueber alle Gegner und warf bei jedem,
        // dessen Skelett gerade nicht bereit ist, eine Engine-Exception.
        if (ok_state) {
            auto* go2 = re4vr::call_safe<::REManagedObject*>(ectx, "get_BodyGameObject");
            auto* tf2 = go2 != nullptr
                ? re4vr::call_safe<::REManagedObject*>(go2, "get_Transform")
                : nullptr;
            auto* nj2 = tf2 != nullptr ? neck_joint_of(tf2) : nullptr;

            if (nj2 == nullptr && tf2 != nullptr) {
                nj2 = joint_by_name(tf2, NECK_ALT);
            }

            glm::vec3 nw2{};

            if (nj2 != nullptr && get_vec3(nj2, "get_Position", nw2)) {
                const float h2 = nw2.y - rp.y;

                if (h2 < m_cfg.min_neck_y || h2 > m_cfg.max_neck_y) {
                    ok_state = false;
                }
            }
        }

        if (ok_state) {
            best = ectx;
            bestd = d;
        }
    }

    return best;
}

// ============================================================================
// Halten
// ============================================================================

// [CRASH-GUARD 21.08.] Stirbt der Gegner im Griff, raeumt die Engine Body,
// Joints, Motion und IK weg, waehrend unsere Phasen-Callbacks weiter in die
// gemerkten Referenzen schreiben. Ein pcall faengt so eine AV NICHT.
bool RE4VRChoke::held_usable() {
    if (m_held.ctx.obj == nullptr || m_held.tf.obj == nullptr) {
        return false;
    }

    if (!valid_not_false(m_held.tf.obj)) {
        return false;
    }

    // [HUHN 2026-09-11] Am GmChicken haengt der Zustand NICHT wie am
    // EnemyContext: valid_not_false prueft get_Valid, das dort zwar existiert,
    // aber nichts ueber den Griff aussagt. Entscheidend ist allein, ob der
    // KOERPER noch steht -- den haben wir oben schon geprueft -- und ob es
    // lebt. Faellt held_usable() faelschlich auf false, laeuft apply_hold()
    // nicht mehr: dann wird das Huhn nicht mehr an die Hand geschrieben (es
    // bleibt starr stehen) UND __re4_choke_hand_y fehlt, womit die
    // Hand-Klemme in RE4VRMotion ihren Bezug verliert.
    if (m_held.is_animal) {
        // [FLEDERMAUS 14.09.2026 -- DIE Ursache, warum sie nie in der Hand lag]
        // Sie meldet im Flug get_IsDead = true. Damit stand held_usable() auf
        // false, und genau daran haengen apply_choke_pose() UND apply_hold():
        // es gab also weder die Wuerge-Handpose noch das Schreiben an die Hand.
        // Dieselbe Ausnahme wie in is_live, aus demselben Grund.
        if (m_held.species == SPECIES_BAT) {
            return true;
        }

        return !opt_bool(m_held.ctx.obj, "get_IsDead").value_or(false);
    }

    if (!valid_not_false(m_held.ctx.obj)) {
        return false;
    }

    return is_live(m_held.ctx.obj);
}

void RE4VRChoke::torso_capture(::REManagedObject* etf) {
    for (auto& e : m_held.torso) {
        drop(e.j);
    }

    m_held.torso.clear();
    m_held.torso_set = false;

    if (!m_cfg.torso_pin) {
        return;
    }

    for (const char* nm : TORSO_JOINTS) {
        auto* j = joint_by_name(etf, nm);

        if (j == nullptr) {
            continue;
        }

        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
        bool have_r = false;

        if (m_cfg.torso_upright) {
            // Basis-/Ruhelage des Joints = aufrecht. Faellt sie aus, notfalls
            // die aktuelle nehmen.
            have_r = get_quat(j, "get_BaseLocalRotation", r);
        }

        if (!have_r) {
            have_r = get_quat(j, "get_LocalRotation", r);
        }

        if (!have_r) {
            continue;
        }

        Held::TorsoEntry e{};
        store(e.j, j);
        e.r = r;

        glm::vec3 p{};

        if (get_vec3(j, "get_BaseLocalPosition", p) || get_vec3(j, "get_LocalPosition", p)) {
            e.p = p;
        }

        m_held.torso.push_back(e);
    }

    m_held.torso_set = !m_held.torso.empty();
}

// ...und in jeder Phase zurueckschreiben, sonst gewinnt die Anim dazwischen.
void RE4VRChoke::torso_enforce() {
    if (!m_cfg.torso_pin || !m_held.torso_set) {
        return;
    }

    if (!held_usable()) {
        return;
    }

    for (const auto& e : m_held.torso) {
        if (e.j.obj == nullptr) {
            continue;
        }

        set_quat(e.j.obj, "set_LocalRotation", e.r);

        if (e.p.has_value()) {
            set_vec3(e.j.obj, "set_LocalPosition", *e.p);
        }
    }
}

// [FRUEHE PHASEN] Bleibt stehen, wird aber seit dem 22.08. nicht mehr gerufen
// (RUECKBAU: hier statt apply_hold eintragen). Der Zielpunkt wird in allen
// Phasen frisch gerechnet, weil der gemerkte in Bewegung um die seit der
// letzten BR-Phase zurueckgelegte Strecke veraltet ist.
void RE4VRChoke::hold_last() {
    if (!m_held.last_target.has_value() || !held_usable()) {
        return;
    }

    torso_enforce();

    set_vec3(m_held.tf.obj, "set_Position", *m_held.last_target);

    if (m_held.yaw.has_value()) {
        // [HUHN-DREHUNG 12.09.2026] Gegner bleiben unveraendert, nur das Huhn
        // bekommt die drei Korrekturwinkel aufgesetzt.
        set_quat(m_held.tf.obj, "set_Rotation", held_rot(yaw_quat(*m_held.yaw)));
    }
}

// [Y-KLEMME] Zielhoehe auf das erlaubte Fenster um die Griffhoehe stutzen.
float RE4VRChoke::clamp_hold_y(float y) {
    // [WUERGEHOEHE] Feste Hoehe aktiv -> die Hand regelt allein, hier NICHT
    // zusaetzlich klemmen.
    if (m_cfg.fix_height) {
        return y;
    }

    if (!m_cfg.y_clamp_on || !m_held.hold_base.has_value()) {
        return y;
    }

    auto* btf = player_body_tf();
    glm::vec3 pp{};

    if (btf == nullptr || !get_vec3(btf, "get_Position", pp)) {
        return y;
    }

    const float base = pp.y + *m_held.hold_base;

    if (y > base + m_cfg.y_clamp) {
        return base + m_cfg.y_clamp;
    }

    if (y < base - m_cfg.y_clamp) {
        return base - m_cfg.y_clamp;
    }

    return y;
}

void RE4VRChoke::apply_hold() {
    if (!held_usable()) {
        return;
    }

    // [FLEDERMAUS SICHTBAR 14.09.2026 -- im Lua-Versuch belegt] Sie hing mit
    // 0,00 m Abstand an der Hand und war trotzdem nicht zu sehen: an ihrem
    // GameObject stand DrawSelf = false. Wer es abschaltet, schreibt nach uns
    // -- also JEDEN Frame erzwingen, solange sie gehalten wird. Mesh dazu, weil
    // beide Wege das Zeichnen unterbinden koennen.
    if (m_held.species == SPECIES_BAT) {
        if (auto* bgo = re4vr::call_safe<::REManagedObject*>(m_held.ctx.obj, "get_GameObject")) {
            re4vr::call_safe<void*>(bgo, "set_DrawSelf", true);

            if (auto* mesh = m_t_mesh != nullptr ? get_component(bgo, m_t_mesh) : nullptr) {
                re4vr::call_safe<void*>(mesh, "set_Enabled", true);
                re4vr::call_safe<void*>(mesh, "set_DrawDefault", true);
            }
        }
    }

    auto* btf = player_body_tf();
    auto* pj = btf != nullptr ? joint_by_name(btf, PARENT_JOINT) : nullptr;

    glm::vec3 palm{};

    if (pj == nullptr || !get_vec3(pj, "get_Position", palm)) {
        return;
    }

    // [WUERGEHOEHE] Ziel: MEIN Halsjoint + NECK_UP. Gesetzt wird aber die
    // QUELLE der linken Hand (__vr_lh_world) -- die traegt gegenueber dem
    // Palm-Joint einen festen Rig-Versatz, der hier live herausgerechnet wird.
    if (m_cfg.fix_height) {
        auto* pn = joint_by_name(btf, "Neck_1");

        if (pn == nullptr) {
            pn = joint_by_name(btf, "Neck_0");
        }

        glm::vec3 pnp{};
        const auto lhw = re4vr::lua_get_vec3("__vr_lh_world");

        if (pn != nullptr && get_vec3(pn, "get_Position", pnp) && lhw.has_value()) {
            re4vr::lua_set_number("__re4_choke_hand_y",
                                  (pnp.y + m_cfg.neck_up) - (palm.y - lhw->y));
            re4vr::lua_set_number("__re4_choke_hand_play",
                                  m_cfg.y_clamp_on ? m_cfg.y_clamp : 0.0f);
            // [HAND IMMER GLEICH 13.09.2026] Die feste seitliche Lage. Motion
            // nimmt sie statt des beim Zupacken eingefrorenen Messwerts.
            re4vr::lua_set_number("__re4_choke_hand_lat", m_cfg.hand_lat);
        }
    }

    // Aufrecht halten (nur Yaw; sonst erbt er die Rotation des Palm-Joints).
    glm::vec3 ppos{};
    const bool have_ppos = btf != nullptr && get_vec3(btf, "get_Position", ppos);

    glm::vec3 rootp{};
    const bool have_root = get_vec3(m_held.tf.obj, "get_Position", rootp);

    auto yaw = m_held.yaw;

    if (!m_cfg.freeze_yaw && have_ppos && have_root) {
        const float dx = ppos.x - rootp.x;
        const float dz = ppos.z - rootp.z;

        if ((dx * dx + dz * dz) > 0.0001f) {
            yaw = std::atan2(dx, dz) + deg2rad(m_cfg.face_yaw_deg);
        }
    }

    if (yaw.has_value()) {
        // [HUHN-DREHUNG 12.09.2026] s. oben -- zweite Schreibstelle der Pose.
        set_quat(m_held.tf.obj, "set_Rotation", held_rot(yaw_quat(*yaw)));
    }

    // Joint-Writes laufen NICHT hier, sondern in joints_tick -- sonst
    // schreiben wir sie in fuenf Phasen und die Engine rechnet dazwischen ihre
    // Anim.
    auto* nj = neck_joint_of(m_held.tf.obj);

    if (nj == nullptr) {
        nj = joint_by_name(m_held.tf.obj, NECK_ALT);
    }

    glm::vec3 neck{};
    const bool have_neck = nj != nullptr && get_vec3(nj, "get_Position", neck);

    const bool have_root2 = get_vec3(m_held.tf.obj, "get_Position", rootp);

    if (!((m_cfg.lock_on_grab && m_held.neck_off.has_value() && have_root2)
          || (have_neck && have_root2))) {
        return;
    }

    float tx = palm.x;
    float ty = palm.y;
    float tz = palm.z;

    // [TIER-VERSATZ 12.09.2026] Tiere ohne Neck_1 haengen an der Huefte und
    // sitzen dadurch anders in der Hand -- sie bekommen ihren eigenen Versatz
    // OBENDRAUF. Huhn und Gegner rechnen mit den reinen Griff-Reglern weiter.
    const AnimalTune tune = tune_of_held();
    const float o_side = m_cfg.grip_side + tune.ox;
    const float o_up = m_cfg.grip_up + tune.oy;
    const float o_away = m_cfg.grip_away + tune.oz;

    if (o_away != 0.0f || o_up != 0.0f || o_side != 0.0f) {
        // Richtung: von deinem Koerper zur Hand, flach (ohne Hoehe), normiert.
        float dx = 0.0f;
        float dz = 0.0f;

        if (have_ppos) {
            dx = palm.x - ppos.x;
            dz = palm.z - ppos.z;
        }

        const float len = std::sqrt(dx * dx + dz * dz);

        // [EIN BEZUG FUER ALLES 14.09.2026] Fuer Rigs MIT Hals kommt die
        // Richtung jetzt aus derselben Quelle wie die Drehung -- dem Yaw der
        // HAND. Vorher zeigte sie entlang der Linie Koerper->Hand: die dreht
        // sich, sobald du die Hand bewegst, und genau deshalb sass der
        // Halsjoint jedes Mal woanders auf der Handflaeche.
        //
        // Die necklosen Tiere behalten die alte Richtung (dort eingestellt).
        const auto hy_off = !m_held.no_neck ? hand_basis_yaw() : std::nullopt;

        if (hy_off.has_value()) {
            const float fx = std::sin(*hy_off);
            const float fz = std::cos(*hy_off);   // vorwaerts (von dir weg)
            const float sx = fz;
            const float sz = -fx;                 // rechtwinklig = seitlich

            tx += fx * o_away + sx * o_side;
            tz += fz * o_away + sz * o_side;
        } else if (len > 0.0001f) {
            const float fx = dx / len;
            const float fz = dz / len;   // vorwaerts (von dir weg)
            const float sx = fz;
            const float sz = -fx;        // rechtwinklig dazu = seitlich

            tx += fx * o_away + sx * o_side;
            tz += fz * o_away + sz * o_side;
        }

        ty += o_up;
    }

    glm::vec3 no = m_held.neck_off.value_or(glm::vec3{0.0f, 0.0f, 0.0f});

    // ------------------------------------------------------------------
    // [LAGE AM JOINT -- TEIL 2, 13.09.2026 -- Ansage "ihre x/y/z Position ist
    // aber bei jedem Griff woanders"]
    //
    // Der Zielpunkt ist "Handflaeche minus Versatz Wurzel->Griff-Joint". Dieser
    // Versatz kommt aus neck_measure() und ist ein WELT-Abstand aus dem
    // vorigen Frame -- er enthaelt also die Transform-Rotation von DAMALS. Seit
    // die Drehung jeden Frame gegen die Animation nachgezogen wird, passt das
    // nicht mehr zusammen: der Versatz zeigt in eine andere Richtung als die
    // Lage, die wir gerade setzen. Uebrig bleibt ein Restfehler, der davon
    // abhaengt, wie das Tier beim Zupacken stand -- genau das Symptom.
    //
    // Fuer die necklosen Tiere wird der Versatz deshalb umgerechnet:
    //   lokal = inverse(R_transform_jetzt) * (Joint_welt - Wurzel_welt)
    //   no    = R_ziel * lokal
    // Beide Messwerte stammen aus demselben Frame, die Zielrotation ist exakt
    // die, die gleich geschrieben wird. Damit sitzt der Griff-Joint in JEDEM
    // Griff auf demselben Punkt der Handflaeche.
    //
    // Huhn und Gegner bleiben beim gemessenen Weltversatz.
    // ------------------------------------------------------------------
    // [ASHLEY IMMER GLEICH 13.09.2026] Auch der Versatz muss aus derselben
    // Quelle kommen wie die Drehung -- sonst sitzt sie zwar gleich gedreht,
    // aber an wechselnder Stelle der Handflaeche.
    // [HUHN + GEGNER 13.09.2026] Gilt seither fuer jeden Gegriffenen.
    if (have_neck && have_root2 && yaw.has_value()) {
        glm::quat rt_jetzt{1.0f, 0.0f, 0.0f, 0.0f};

        if (get_quat(m_held.tf.obj, "get_Rotation", rt_jetzt)) {
            const glm::vec3 lokal = glm::inverse(rt_jetzt) * (neck - rootp);

            // held_rot() liefert fuer diese Tiere die HAND-bezogene Ziellage
            // (s. dort) -- Position und Drehung stammen damit aus derselben
            // Quelle und koennen nicht auseinanderlaufen.
            no = held_rot(yaw_quat(*yaw)) * lokal;
        }
    }

    m_held.last_target = glm::vec3{tx - no.x, ty - no.y, tz - no.z};

    // [VERSATZ 21.08.] Der Abstand Wurzel->Hals wird NICHT frisch gelesen:
    // direkt nach unseren Joint-Writes liefert die Engine noch die alten
    // Weltmatrizen. Es gilt der am ENDE des letzten Frames gemessene Versatz.
    std::optional<glm::vec3> off = m_held.neck_off;

    if (!off.has_value() && have_neck) {
        off = neck - rootp;
    }

    if (!off.has_value()) {
        return;
    }

    // Die Hoehe, bei der Neck1 auf der Handflaeche sitzt -- das ist die
    // Wahrheit, gegen die geklemmt werden darf.
    float fy = ty - off->y;

    // [Y-KLEMME] Bezug beim ERSTEN gueltigen Frame einfrieren, erst danach
    // klemmen. So faengt die Klemme nie die Ausrichtung selbst ab.
    if (!m_held.hold_base.has_value() && have_ppos) {
        m_held.hold_base = fy - ppos.y;
    }

    fy = clamp_hold_y(fy);

    // hold_last schreibt last_target in den spaeteren Phasen nach -> denselben,
    // geklemmten Wert hinterlegen.
    m_held.last_target = glm::vec3{tx - off->x, fy, tz - off->z};

    set_vec3(m_held.tf.obj, "set_Position", glm::vec3{tx - off->x, fy, tz - off->z});
}

// Nach ALLEN Writes des Frames den Versatz Wurzel->Hals nachmessen -- dann
// stimmen die Weltmatrizen und der Wert gilt fuer den naechsten Frame.
void RE4VRChoke::neck_measure() {
    if (!held_usable()) {
        return;
    }

    auto* nj = neck_joint_of(m_held.tf.obj);

    if (nj == nullptr) {
        nj = joint_by_name(m_held.tf.obj, NECK_ALT);
    }

    glm::vec3 nw{};
    glm::vec3 rw{};

    if (nj != nullptr && get_vec3(nj, "get_Position", nw)
        && get_vec3(m_held.tf.obj, "get_Position", rw)) {
        m_held.neck_off = nw - rw;
    }
}

// Joint-Overrides (Rumpf-Pin + Handpose) gehoeren in die Joint-Phase:
// UpdateJointExpression laeuft NACH der Anim und vor dem Skinning.
void RE4VRChoke::joints_tick() {

    // [HAND-DREHUNG 12.09.2026 -- "hat keinen Effekt"] Die drei Regler wirkten
    // nicht, weil arm_chain in JEDER Phase der LETZTE Callback ist und ihr
    // HAND_REPIN die WELT-Rotation der linken Hand neu setzt (RE4VRArmChain.cpp,
    // Block [HAND_REPIN]). Alles, was wir hier auf L_Hand schreiben, ist danach
    // weg. Darum wird die Drehung ZUSAETZLICH dort hineingereicht.
    //
    // Das direkte Schreiben in apply_choke_pose bleibt bestehen: es traegt den
    // Fall, dass arm_chain aus ist. Doppelt gedreht wird nie -- laeuft
    // arm_chain, ueberschreibt sie unsere Schreibung ohnehin komplett.
    //
    // JEDEN Frame setzen (auch das Abschalten), damit kein vergessenes
    // Zuruecksetzen die Hand dauerhaft verdreht laesst.
    {
        const auto ht = hand_tune_of_held();
        const bool want = ht.any();   // haelt schon "nichts in der Hand" ab

        std::optional<glm::quat> extra{};

        if (want) {
            extra = glm::normalize(q_axis(ht.yaw, 0.0f, 1.0f, 0.0f)
                                   * q_axis(ht.pitch, 1.0f, 0.0f, 0.0f)
                                   * q_axis(ht.roll, 0.0f, 0.0f, 1.0f));
        }

        if (auto ac = RE4VRArmChain::get(); ac != nullptr) {
            ac->set_left_hand_extra_rot(extra);
        }
    }

    if (m_held.ctx.obj != nullptr && held_usable()) {
        // REIHENFOLGE (21.08. teuer gelernt): ERST den Rumpf aufrichten, DANN
        // den Hals messen und positionieren.
        torso_enforce();
        apply_choke_pose();
        apply_hold();
    } else if (m_cfg.pose_force) {
        apply_choke_pose();
    }

    // [MUND OHNE GRIFF 16.09.2026] AUSSERHALB des Griff-Zweigs: ein einmal
    // gestarteter Spruch laeuft zu Ende, egal ob sie noch in der Hand haengt.
    // Laeuft keiner, steigt mouth_tick() sofort aus und ihre Mimik bleibt
    // unberuehrt.
    mouth_tick();
}

// ============================================================================
// Loslassen
// ============================================================================

void RE4VRChoke::release() {
    if (m_held.ctx.obj == nullptr) {
        return;
    }

    // [MESSER STECKEN v2] Hier wird das Messer BEWUSST NICHT zurueckgeholt:
    // der Stich beendet den Griff, und genau dann SOLL es steckenbleiben.
    if (m_held.ik_set) {
        for (auto& e : m_held.ik) {
            if (e.c.obj != nullptr && e.was.has_value()) {
                re4vr::call_safe<void*>(e.c.obj, "set_Enabled", *e.was);
            }

            drop(e.c);
        }

        m_held.ik.clear();
        m_held.ik_set = false;
    }

    if (m_cfg.grapple_flags) {
        // [GELAENDE-KORREKTUR 2026-09-10 -- gemessen] Steckt unser Messer noch
        // in ihm, bleiben die Flags AN. Sofort zurueckgestellt gibt die Engine
        // die Gelaende-Korrektur wieder frei, waehrend der Koerper noch dort
        // steht, wohin WIR ihn gezogen haben -- und die holt den Versatz dann
        // in EINEM Frame nach (Messung 23:35: 2.53 m, rein horizontal, y
        // unveraendert, 0.45 s nach dem Loslassen). Dasselbe Muster wie bei der
        // Root Motion direkt darunter: erst am Ende des Nachlaufs.
        if (m_held.is_animal) {
            // [HUHN] nichts freizugeben -- es wurde auch nichts gesetzt.
        } else if (m_stick.tf.obj != nullptr) {
            m_stick.grapple_pending = true;
        } else {
            re4vr::call_safe<void*>(m_held.ctx.obj, "set_IsConstOnGrapple", false);
            re4vr::call_safe<void*>(m_held.ctx.obj,
                                    "set_IgnoreTerrainCorrectOnGrapple", false);
        }
    }

    if (m_held.mo.obj != nullptr) {
        if (m_held.mo_was.has_value()) {
            re4vr::call_safe<void*>(m_held.mo.obj, "set_Enabled", *m_held.mo_was);
        }

        if (m_held.rm_was.has_value()) {
            // [WEGGESCHLEUDERT 2026-08-24] Der Stich beendet den Griff in
            // genau dem Frame, in dem die Trefferreaktion startet. Schalten
            // wir die ROOT MOTION hier sofort wieder scharf, schleudert die
            // Anim ihn 3-4 m davon. Steckt unser Messer in ihm, bleibt sie
            // deshalb bis zum Ende des Nachlaufs AUS.
            if (m_stick.tf.obj != nullptr) {
                store(m_stick.mo, m_held.mo.obj);
                m_stick.rm_was = m_held.rm_was;
            } else {
                re4vr::call_safe<void*>(m_held.mo.obj, "set_RootMotion", *m_held.rm_was);
            }
        }
    }

    m_held.mo_was.reset();

    // get_Valid PFLICHT: set_Parent auf einem stalen Transform ist eine AV.
    const bool valid = m_held.tf.obj != nullptr && valid_not_false(m_held.tf.obj);

    if (valid) {
        glm::vec3 rp{};
        const bool have_rp = get_vec3(m_held.tf.obj, "get_Position", rp);

        glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        const bool have_rot = get_quat(m_held.tf.obj, "get_Rotation", rot);

        auto* btf = player_body_tf();
        glm::vec3 pp{};
        const bool have_pp = btf != nullptr && get_vec3(btf, "get_Position", pp);

        set_parent_joint(m_held.tf.obj, "");
        re4vr::call_safe<void*>(m_held.tf.obj, "set_Parent", (::REManagedObject*)nullptr);

        // Absetzen: sonst behaelt er die Lage aus der Hand und haengt schraeg
        // in der Luft. Erst loesen, dann Weltlage setzen.
        if (have_rp) {
            const float y = have_pp ? pp.y : rp.y;
            set_vec3(m_held.tf.obj, "set_Position", glm::vec3{rp.x, y, rp.z});
        }

        if (have_rot) {
            const float yaw = std::atan2(2.0f * (rot.w * rot.y + rot.x * rot.z),
                                         1.0f - 2.0f * (rot.y * rot.y + rot.x * rot.x));
            set_quat(m_held.tf.obj, "set_Rotation", yaw_quat(yaw));
        }
    }

    drop(m_held.ctx);
    drop(m_held.tf);
    drop(m_held.mo);
    // [FLEDERMAUS-FAHRER 13.09.2026] Bahn und Motion-FSM zurueckgeben, sonst
    // bleibt sie fuer immer in der Luft stehen.
    // [ZURUECK AUF DIE BAHN 14.09.2026] ERST an den Ausgangspunkt setzen, DANN
    // die Komponenten wieder anschalten -- andersherum sieht ihre Bahn fuer
    // einen Frame die Handposition und sie faengt dort unten an zu krabbeln.
    if (m_held.bat_home_pos.has_value() && m_held.tf.obj != nullptr
        && valid_not_false(m_held.tf.obj)) {
        set_vec3(m_held.tf.obj, "set_Position", *m_held.bat_home_pos);

        if (m_held.bat_home_rot.has_value()) {
            set_quat(m_held.tf.obj, "set_Rotation", *m_held.bat_home_rot);
        }
    }

    m_held.bat_home_pos.reset();
    m_held.bat_home_rot.reset();

    for (auto& e : m_held.drivers) {
        if (e.c.obj != nullptr && e.was.has_value() && valid_not_false(e.c.obj)) {
            re4vr::call_safe<void*>(e.c.obj, "set_Enabled", *e.was);
        }

        drop(e.c);
    }

    m_held.drivers.clear();

    m_held.is_animal = false;
    m_held.is_chicken = false;
    m_held.ada_player = false;
    m_held.is_ashley = false;
    m_held.no_neck = false;
    m_held.species = SPECIES_NONE;
    m_held.grab_joint.clear();
    m_held.rm_was.reset();
    m_held.sw_prev = false;
    m_held.yaw.reset();

    // [RUECKWECHSEL] Nur wenn WIR das Messer gezogen haben -- STRIKT gegen
    // false, so wie in Lua (`held.knife_was == false`). Deferred, damit der
    // Wechsel nicht im selben Frame wie das Loesen laeuft (dort verpufft er).
    if (m_cfg.reequip_after && m_cfg.knife_to_right
        && m_held.knife_was == std::optional<bool>{false}) {
        // [BAREHANDS 2026-09-12] Ziel des Wechsels JETZT festhalten -- m_held
        // wird gleich geleert, faellig wird der Wechsel aber erst spaeter.
        m_reequip_bare = m_held.bare_was == std::optional<bool>{true};

        // [MESSER STECKEN v2] Steckt das Messer gerade im Gegner, darf der
        // Rueckwechsel NICHT sofort laufen -- requestChangeActiveWeapon nimmt
        // uns genau das Messer weg, das da noch stecken soll.
        const double wait = m_stick.tf.obj != nullptr
            ? (static_cast<double>(m_cfg.knife_stick_t) + 0.15)
            : 0.10;
        m_reequip_at = clock_now() + wait;
        m_reequip_until = clock_now() + wait + 3.00;
        // [STAGGER-FENSTER 13.09.2026] Absolute Obergrenze: bis hierhin darf das
        // Fenster beim Warten auf einen Gameplay-Frame nachgezogen werden.
        m_reequip_dead = clock_now() + wait + REEQUIP_MAX_WAIT;
        m_reequip_wait_seit = 0.0;
    }

    // [HAND-Y] Klemme aufheben und die Hand WEICH zum Controller zurueckholen:
    // exakt der KS4-Austritts-Lerp aus re4_vr_motion.lua.
    re4vr::lua_set_nil("__re4_choke_hand_play");
    re4vr::lua_set_nil("__re4_choke_dy");
    re4vr::lua_set_nil("__re4_choke_hand_y");
    re4vr::lua_set_nil("__re4_choke_hand_lat");
    re4vr::lua_set_nil("__re4_choke_y_lerp");
    // [UNSICHTBAR-FIX] er haengt wieder frei -> materials darf ihn wieder sehen
    re4vr::lua_set_nil("__re4_choke_victim");
    re4vr::lua_set_number("__re4_ks4_exit_t", clock_now());

    m_held.py0.reset();
    m_held.knife_was.reset();
    m_held.bare_was.reset();
    m_held.neck_off.reset();
    m_held.dy0.reset();
    m_held.hold_base.reset();

    // [1:1] `held.last_target` und `held.torso` ueberleben das Loslassen im
    // Original -- sie werden hier BEWUSST nicht geleert. Jeder Leser steht
    // hinter held_usable(), und torso_capture gibt die Handles beim naechsten
    // Griff frei; es bleibt also nichts liegen.
    m_last_release = clock_now();
}

// ============================================================================
// Zugriff
// ============================================================================

void RE4VRChoke::grab(::REManagedObject* ectx) {
    ensure_types();

    // [HUHN] GmChicken kennt kein get_BodyGameObject -- dort IST das
    // GameObject der Koerper (gemessen 23:47: Transform, 59 Joints, Motion,
    // HitController haengen alle direkt daran).
    auto* ego = m_held.is_animal
        ? re4vr::call_safe<::REManagedObject*>(ectx, "get_GameObject")
        : re4vr::call_safe<::REManagedObject*>(ectx, "get_BodyGameObject");
    auto* etf = ego != nullptr ? re4vr::call_safe<::REManagedObject*>(ego, "get_Transform")
                               : nullptr;
    auto* btf = player_body_tf();
    auto* bgo = player_body_go();

    if (etf == nullptr || btf == nullptr || bgo == nullptr) {
        return;
    }

    if (!valid_not_false(etf) || !valid_not_false(bgo)) {
        return;
    }

    // [UNSICHTBAR-FIX 2026-08-24] Ab hier haengt der Gegner im Transform-Baum
    // UNSERES Bodys. re4_vr_materials laeuft diesen Baum jeden Frame ab und
    // blendet im Voll-Ausblenden JEDES Mesh darunter aus -- also auch ihn.
    // Gelesen wird das NUR zusammen mit dem Herzschlag __re4_choke_seen.
    re4vr::lua_set_number("__re4_choke_victim",
                          static_cast<double>(reinterpret_cast<uintptr_t>(ego)));
    re4vr::lua_set_number("__re4_choke_seen", clock_now());

    re4vr::call_safe<void*>(etf, "set_Parent", btf);

    if (!m_cfg.parent_to_body) {
        set_parent_joint(etf, PARENT_JOINT);
        set_vec3(etf, "set_LocalPosition", glm::vec3{0.0f, 0.0f, 0.0f});
    } else {
        set_parent_joint(etf, "");
    }

    // Root Motion stilllegen: sie fuehrt Position UND Rotation des
    // Wurzeltransforms. Die uebrige Anim laeuft weiter -> Treffer/Tod bleiben
    // sichtbar.
    auto* mo = m_t_motion != nullptr ? get_component(ego, m_t_motion) : nullptr;

    if (mo != nullptr) {
        store(m_held.mo, mo);
        m_held.rm_was = opt_int(mo, "get_RootMotion");
        re4vr::call_safe<void*>(mo, "set_RootMotion", root_none_value());
    }

    // Blickrichtung EINMAL festlegen: zu uns gedreht, ab dann fest.
    glm::vec3 ppos{};
    glm::vec3 rp{};
    m_held.yaw.reset();

    if (get_vec3(btf, "get_Position", ppos) && get_vec3(etf, "get_Position", rp)) {
        const float dx = ppos.x - rp.x;
        const float dz = ppos.z - rp.z;

        if ((dx * dx + dz * dz) > 0.0001f) {
            m_held.yaw = std::atan2(dx, dz) + deg2rad(m_cfg.face_yaw_deg);
        }
    }

    if (m_cfg.leg_ik_off) {
        for (auto& e : m_held.ik) {
            drop(e.c);
        }

        m_held.ik.clear();

        for (auto* t : m_t_ik) {
            // [1:1] Luas `ipairs(T_IK)` bricht beim ERSTEN nil ab -- faellt
            // also sdk.typeof("via.motion.IkLeg2") aus, wird gar keine
            // Komponente mehr angefasst. Ein Weiterlaufen waere neues Verhalten.
            if (t == nullptr) {
                break;
            }

            auto* comp = get_component(ego, t);

            if (comp == nullptr) {
                continue;
            }

            Held::IkEntry e{};
            store(e.c, comp);
            e.was = opt_bool(comp, "get_Enabled");
            re4vr::call_safe<void*>(comp, "set_Enabled", false);
            m_held.ik.push_back(e);
        }

        m_held.ik_set = true;
    }

    // [HUHN] Die drei Grapple-Setter gibt es nur am EnemyContext -- am
    // GmChicken laufen sie ins Leere (gemessen: alle drei "NICHTS").
    // [FLEDERMAUS-FAHRER 13.09.2026] Ihre Bahn abschalten, sonst schreibt sie
    // unsere Position jeden Frame wieder weg. NUR bei der Fledermaus: Huhn,
    // Maus und Kraehe haengen bereits sauber in der Hand.
    if (m_held.species == SPECIES_BAT) {
        // Ausgangslage merken -- dorthin kommt sie beim Loslassen zurueck.
        {
            glm::vec3 hp0{};
            glm::quat hr0{1.0f, 0.0f, 0.0f, 0.0f};

            m_held.bat_home_pos = get_vec3(etf, "get_Position", hp0)
                ? std::optional<glm::vec3>{hp0} : std::nullopt;
            m_held.bat_home_rot = get_quat(etf, "get_Rotation", hr0)
                ? std::optional<glm::quat>{hr0} : std::nullopt;
        }

        auto* comps = re4vr::call_safe<::REManagedObject*>(ego, "get_Components");
        const int32_t cn = comps != nullptr ? re4vr::array_size(comps) : 0;

        for (int32_t i = 0; i < cn; ++i) {
            auto* comp = re4vr::array_element(comps, i);

            if (comp == nullptr || !valid_not_false(comp)) {
                continue;
            }

            // Die drei Lebenserhalter bleiben an.
            // [FALLE, s. RE4VR.cpp:562] get_full_name() liefert den String PER
            // WERT -- erst in eine eigene Variable, dann vergleichen.
            auto* def = utility::re_managed_object::get_type_definition(comp);
            const std::string tn = def != nullptr ? def->get_full_name() : std::string{};
            bool behalten = false;

            for (const char* k : BAT_KEEP) {
                if (tn == k) {
                    behalten = true;
                    break;
                }
            }

            if (behalten) {
                continue;
            }

            const auto war = opt_bool(comp, "get_Enabled");

            // Was ohnehin aus ist, muss weder abgeschaltet noch gemerkt werden.
            if (war == std::optional<bool>{false}) {
                continue;
            }

            Held::IkEntry e{};
            store(e.c, comp);
            e.was = war;
            re4vr::call_safe<void*>(comp, "set_Enabled", false);
            m_held.drivers.push_back(e);
        }
    }

    // [ASHLEY 13.09.2026] Sie hat die drei Setter genauso wenig wie ein Tier
    // (gemessen: alle drei "fehlt") -- also gar nicht erst rufen.
    if (m_cfg.grapple_flags && !m_held.is_animal && !m_held.is_ashley) {
        re4vr::call_safe<void*>(ectx, "set_IsConstOnGrapple", true);
        re4vr::call_safe<void*>(ectx, "requestIgnoreTerrainCorrectOnGrapple");
        re4vr::call_safe<void*>(ectx, "set_IgnoreTerrainCorrectOnGrapple", true);
    }

    torso_capture(etf);

    // [Y-KLEMME] Hoehenversatz Gegner-Wurzel <-> unsere Wurzel EINMAL merken.
    m_held.dy0.reset();
    m_held.py0.reset();

    {
        glm::vec3 pp0{};
        glm::vec3 rp0{};
        const bool have_pp0 = get_vec3(btf, "get_Position", pp0);
        const bool have_rp0 = get_vec3(etf, "get_Position", rp0);

        if (have_pp0 && have_rp0) {
            m_held.dy0 = rp0.y - pp0.y;
        }

        // [HAND-Y] Fuer die Handklemme reicht unsere EIGENE Wurzelhoehe. Die
        // Handhoehe selbst wird bewusst NICHT hier gemessen (Offset-Falle).
        if (have_pp0) {
            m_held.py0 = pp0.y;
        }
    }

    // wird am Ende des ersten Frames gemessen (s. neck_measure)
    m_held.neck_off.reset();

    // Messer in die rechte Hand ziehen (und den Links-Klon weichen lassen)
    if (m_cfg.knife_to_right) {
        auto* ctx = player_ctx();
        auto* hgo = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadGameObject")
            : nullptr;
        auto* pe = get_component(hgo, type_of("chainsaw.PlayerEquipment"));

        // [RUECKWECHSEL] War schon vor uns ein Messer gezogen? Ehrlich
        // beantwortet das nur der HeadUpdater. `or false` in Lua macht aus nil
        // UND aus false gleichermassen false.
        auto* hu = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
            : nullptr;
        m_held.knife_was = hu != nullptr
            ? opt_bool(hu, "get_IsEquipKnife").value_or(false)
            : false;

        // [BAREHANDS 2026-09-12] Zweite Frage, die der HeadUpdater NICHT
        // beantwortet: lag ueberhaupt etwas in der Hand? __vr_bare_hands
        // kommt aus holster und ist am MESH gemessen (weapon_actually_in_hand),
        // nicht an get_EquipWeaponID -- das luegt bei leeren Haenden und meldet
        // die zuletzt gewaehlte Waffe.
        //
        // Gelesen VOR unserem requestEquipKnife: danach beschreibt der Wert
        // unseren eigenen Griff, nicht mehr den Zustand davor.
        m_held.bare_was = re4vr::lua_get_tribool("__vr_bare_hands") == 1;

        if (pe != nullptr) {
            re4vr::lua_set_number("__re4_our_equip_until", clock_now() + 0.5);
            re4vr::lua_set_number("__re4_knife_draw_ours_t", clock_now());
            re4vr::lua_set_bool("__re4_knife_left_intent", false);
            re4vr::lua_set_bool("__re4_knife_left_clone", false);
            re4vr::call_safe<void*>(pe, "requestEquipKnife");
            re4vr::call_safe<void*>(pe, "execChangeWeapon");
        }
    }

    // Reverse-Grip; zurueckgedreht wird per Trigger wie immer.
    // Das Global bleibt (weapons2 und binding lesen es), aber verlassen darf
    // man sich darauf nicht mehr -- binding laeuft seit dem Port NACH uns.
    // Den Zustand setzt deshalb flip_carry() direkt an RE4VRMotion, sobald das
    // Messer da ist.
    re4vr::lua_set_bool("__vr_knife_flip", true);
    RE4VRMotion::get()->force_knife_flip();

    store(m_held.ctx, ectx);
    store(m_held.tf, etf);

    // [TIER-DREHUNG 12.09.2026] Hier faellt die Entscheidung, welche Rigs die
    // drei Tier-Regler sehen: die OHNE Neck_1 -- gemessen GmMouse, GmCrow und
    // GmBat. Ein Gegner kann nie hineinlaufen, das Flag haengt an is_animal.
    m_held.grab_joint = neck_joint_name_of(etf);
    m_held.no_neck = m_held.is_animal
        && m_held.grab_joint != std::string{NECK_JOINT};

    // [ANIM AUF 0 -- 13.09.2026] Nur fuer die necklosen Tiere und nur EINMAL,
    // hier an der Griff-Flanke: die Animation laeuft danach normal weiter.
    // m_held.mo haelt die Motion-Komponente, die weiter oben ohnehin fuer die
    // Root Motion geholt wurde.
    if (m_held.no_neck && m_held.is_animal) {
        animal_anim_rewind(m_held.mo.obj);
    }
    m_held.t0 = clock_now();
    m_held.sw_prev = false;

    // [SPITZEN-TREFFER] Mit einem frischen Griff faengt auch die Spitzen-Spur
    // neu an. Sonst waere die erste "Strecke" der Sprung von dort, wo das
    // Messer beim LETZTEN Griff war.
    m_tip_last.p.reset();
    m_tip_last.nm.clear();
    m_tip_last.has_nm = false;

    // [GRIFF-SOUND] erst hier: der Griff steht, kein Fehlausstieg mehr.
    play_grab_sound();
    play_enemy_grab_sound(ego);
}

// ============================================================================
// Stichrichtung (alter Weg, nur bei TIP.on = false)
// ============================================================================

// [STICHRICHTUNG 2026-08-25] Kurze Historie der rechten Hand. Ein einzelner
// Frame (~11 ms) ist zu verrauscht -- 12 Plaetze decken bei 90 fps ~130 ms ab.
void RE4VRChoke::rh_track() {
    const auto p = re4vr::lua_get_vec3("__vr_rh_world");

    if (!p.has_value()) {
        return;
    }

    ++m_rh_ri;

    if (m_rh_ri > 12) {
        m_rh_ri = 1;
    }

    auto& e = m_rh_ring[static_cast<size_t>(m_rh_ri - 1)];
    e.p = *p;
    e.t = clock_now();
    e.set = true;
}

// true = die Hand bewegt sich auf den Gehaltenen ZU (oder wir koennen es nicht
// beurteilen). Im Zweifel IMMER true: lieber ein Stich zu viel als ein
// verschluckter.
bool RE4VRChoke::swing_towards_victim() {
    if (!m_cfg.fwd_only) {
        return true;
    }

    const auto hp = re4vr::lua_get_vec3("__vr_rh_world");

    if (!hp.has_value()) {
        return true;
    }

    const double now = clock_now();
    const RhEntry* ref = nullptr;
    double oldest = -1.0;

    for (const auto& e : m_rh_ring) {
        if (!e.set) {
            continue;
        }

        const double age = now - e.t;

        // [FENSTER GEKUERZT 2026-08-26] Vorher 0.04-0.30 s -- bei einem
        // schnellen Vor-und-Zurueck lag die Referenz damit noch VOR der
        // Hinbewegung, und das Zurueckziehen zaehlte als Stich.
        if (age >= 0.03 && age <= 0.10 && (oldest < 0.0 || age > oldest)) {
            oldest = age;
            ref = &e;
        }
    }

    if (ref == nullptr) {
        return true;   // noch keine Historie
    }

    const glm::vec3 m = *hp - ref->p;
    const float ml = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);

    if (ml < 0.02f) {
        return true;   // kaum bewegt -> nicht blockieren
    }

    // Ziel ist der Hals des Gehaltenen, notfalls seine Wurzel.
    std::optional<glm::vec3> tp{};

    if (m_held.tf.obj != nullptr) {
        auto* nj = neck_joint_of(m_held.tf.obj);

        if (nj == nullptr) {
            nj = joint_by_name(m_held.tf.obj, NECK_ALT);
        }

        glm::vec3 p{};

        if (nj != nullptr && get_vec3(nj, "get_Position", p)) {
            tp = p;
        } else if (get_vec3(m_held.tf.obj, "get_Position", p)) {
            tp = p;
        }
    }

    if (!tp.has_value()) {
        return true;
    }

    const glm::vec3 t = *tp - *hp;
    const float tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);

    if (tl < 0.001f) {
        return true;
    }

    // [BEIDE BEDINGUNGEN 2026-08-26 -- gemessen] Die Klingenachse ALLEIN
    // reicht nicht (beim Zurueckziehen zeigt die Spitze weiter auf den Hals),
    // die Handbewegung allein ebensowenig (Ausholer kamen durch). Erst UND
    // trennt alle gemessenen Faelle sauber.
    const bool hand_ok = ((m.x * t.x + m.y * t.y + m.z * t.z) / (ml * tl)) > 0.0f;

    if (!hand_ok) {
        return false;
    }

    const auto k = knife_transform();
    glm::vec3 ax{};

    if (k.tf == nullptr || !get_vec3(k.tf, "get_AxisZ", ax)) {
        return true;   // ohne Messer nicht blockieren
    }

    const float al = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);

    if (al < 0.001f) {
        return true;
    }

    // +AxisZ zeigt beim Zustechen zum Ziel (echte Stiche 0.90/0.73/0.65,
    // Ausholen 0.29-0.59).
    const float blade = (ax.x * t.x + ax.y * t.y + ax.z * t.z) / (al * tl);

    return blade > 0.6f;
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRChoke::on_initialize() {
    // [KEIN CONFIG-LADEN HIER] Mods::on_initialize laeuft auf dem Init-Thread;
    // der Lua-State entsteht erst spaeter. load_cfg schreibt aber die drei
    // Globals (FLICKER_DEBUG, STOP_ENEMY_SOUNDS, FREEZE_MOTION) -- also erst
    // in on_lua_state_created. Derselbe Fund wie bei RE4VRMotion.
    return Mod::on_initialize();
}

void RE4VRChoke::on_lua_state_created(sol::state& lua) {
    // [FUNKTIONS-GLOBALS] Beide werden von fremdem Code gerufen und muessen
    // den Port ueberleben -- s. PORT_CHOKE_SPEC Abschnitt 6.
    lua["__re4_is_choking"] = []() { return RE4VRChoke::get()->is_choking(); };

    // Rueckgabe wie im Original: spitze, ux, uy, uz, laenge. re4_vr_weapons.lua
    // nimmt sie fuer seinen Messerwurf; ohne sie faellt der Wurf auf eine feste
    // Klingenlaenge zurueck.
    lua["__re4_blade_tip"] = [](sol::this_state ts, sol::object ktf, sol::object kname)
        -> std::tuple<sol::object, float, float, float, float> {
        sol::state_view sv{ts};

        ::REManagedObject* tf = nullptr;

        // Der Aufrufer reicht einen via.Transform durch -- der Fork bindet
        // REManagedObject* als Userdata.
        if (ktf.is<::REManagedObject*>()) {
            tf = ktf.as<::REManagedObject*>();
        }

        std::string nm;

        if (kname.is<std::string>()) {
            nm = kname.as<std::string>();
        }

        glm::vec3 tip{};
        glm::vec3 axis{};
        float len = 0.0f;

        // Lua gibt bei Fehlschlag NUR nil zurueck -- der Aufrufer prueft
        // `if not tp then`. Die uebrigen Rueckgaben sind dann egal.
        if (tf == nullptr || !RE4VRChoke::get()->blade_tip_public(tf, nm, tip, axis, len)) {
            return {sol::object{sv, sol::lua_nil}, 0.0f, 0.0f, 0.0f, 0.0f};
        }

        return {sol::make_object(sv, tip), axis.x, axis.y, axis.z, len};
    };

    // [CHOKE-RUHE] Der Block am Dateiende setzt beide Globals beim Laden.
    // __re4_choke_ruhe = false: der HitController des Spielers bleibt AN
    // (Absturz 27.08. in chainsaw.HitManager.hitSetting).
    lua["__re4_choke_ruhe_hook"] = true;
    lua["__re4_choke_ruhe"] = false;

    if (!m_cfg_loaded) {
        m_cfg_loaded = true;
        load_cfg();
    }
}

void RE4VRChoke::on_lua_state_destroyed(sol::state& lua) {
    // Lua: `re.on_script_reset(function() release() end)`. Danach laedt
    // REFramework die Datei NEU -- saemtliche Locals starten leer.
    release();

    // [BEWUSSTE ABWEICHUNG -- die einzige in diesem Modul]
    // In Lua sind `stick` und `dim` nach dem Neuladen einfach WEG. Das Messer
    // bleibt dann am Gegner geparentet und, schlimmer, DAUERHAFT abgedunkelt:
    // die gemerkten Originalfarben verschwinden mit der Tabelle, und das hier
    // ist das ECHTE Messer, kein Klon. Genau davor warnt der Autor an zwei
    // Stellen selbst -- stick_return() "wird von JEDEM Ausgang gerufen", und
    // der Dim-Wachhund sagt woertlich: "es darf unter KEINEN Umstaenden dunkel
    // in der Hand landen, egal ueber welchen Weg der Griff geendet hat".
    // Das Original haette hier aufgeraeumt, wenn es gekonnt haette; wir
    // koennen es, also tun wir es.
    stick_return();

    // [STICK-KLON] Selbst erzeugtes GameObject -- es ueberlebt den Reset sonst
    // sichtbar in der Szene, ohne dass noch jemand einen Griff daran hat.
    stick_clone_drop();

    if (m_dim.on) {
        dim_apply(false);
    }

    // Ab hier 1:1: der neue sol::state kennt unsere Globals nicht mehr, und in
    // Lua faengt jede dieser Groessen nach dem Reset bei ihrem Startwert an.
    drop(m_snd_retry.ego);
    m_snd_retry.at.clear();
    m_snd_retry.id = 0;

    drop(m_pend.ctx);
    drop(m_pend.tf);
    drop(m_pend.ktf);
    m_pend.at = 0.0;
    m_pend.v0 = 0.0f;
    m_pend.wp.reset();
    m_pend.wr.reset();
    m_pend.kname.clear();
    m_pend.kdist.reset();
    m_pend.kcount = 0;

    m_reequip_at = 0.0;
    m_reequip_until = 0.0;
    m_reequip_bare = false;
    m_tip_hit_t = -999.0;
    m_tip_last.p.reset();
    m_tip_last.nm.clear();
    m_tip_last.has_nm = false;
    m_last_parry = -99.0;
    m_parry_victim.reset();
    m_last_release = 0.0;
    m_prev_grip = false;

    // [TAUNT-DELAY 2026-09-12] Ein wartender Spruch gehoert zum abgeraeumten
    // Griff -- sonst faellt er nach Reset/Levelwechsel aus dem Nichts.
    m_taunt_due = 0.0;
    m_taunt_pending_index = -1;
    m_nat_last_ctx = nullptr;
    m_nat_n = 0;
    m_blade_len_seen.clear();
    m_vlog.clear();

    for (auto& e : m_rh_ring) {
        e.set = false;
    }

    m_rh_ri = 0;

    // Die einmalige TDB-Suche faengt ebenfalls neu an (in Lua sind
    // _cat_damage/_cat_tried Locals der Datei).
    m_cat_damage.reset();
    m_cat_tried = false;

    // Und die Laufzeit-Flags, die in Lua an ihrem Compile-Default starten.
    m_stop_enemy_sounds = true;

    m_cfg_loaded = false;
}

bool RE4VRChoke::is_choking() const {
    return (clock_now() - re4vr::lua_get_number("__re4_choke_seen", 0.0)) < 0.15;
}

// [CHOKE-FLIP 04.09.2026 -- dritter und richtiger Anlauf]
//
// Lua setzte `_G.__vr_knife_flip = true` und verliess sich darauf, dass
// re4_vr_binding.lua VOR choke.lua laeuft (b < c) und damit verliert. Seit dem
// Port steht Choke vor dem ScriptRunner -- binding laeuft also NACH uns und
// loescht den Wert in re4_vr_binding.lua:1895 im Zugriffsframe wieder.
//
// Zwei Anlaeufe ueber das Global waren deshalb wirkungslos bzw. ein Kampf
// gegen binding. Das Global GEHOERT aber dem RT-Toggle des Spielers -- wir
// haben da nichts zu erzwingen.
//
// Richtig ist der native Weg: RE4VRMotion ist selbst ein Modul und steht
// direkt hinter uns. Es bekommt den Zustand DIREKT gesetzt (Global + Lerp auf
// die Endstellung), genau wie es das nach einem Killswitch fuer
// __re4_knife_flip_pre_ks tut. Die Flip-OFFSETS je Messer (knife_flip_pos)
// zieht attach_weapon dabei unveraendert aus der Config -- das gerade
// equippte Messer landet also mit SEINEN Werten in der rechten Hand.
//
// Gerufen wird das im Zugriffsmoment und im Zeitfenster danach: erst wenn
// weapons.lua das Messer als equippt meldet, greift der Flip ueberhaupt
// (knife_flip_spin steigt vorher aus). Danach gehoert er wieder dem Spieler.
void RE4VRChoke::flip_carry() {
    if (m_held.ctx.obj == nullptr) {
        return;
    }

    if ((clock_now() - m_held.t0) > 1.0) {
        return;
    }

    RE4VRMotion::get()->force_knife_flip();
}

void RE4VRChoke::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    flip_carry();

    if (hash == "LockScene"_fnv) {
        apply_hold();
    } else if (hash == "UnlockScene"_fnv) {
        apply_hold();
    } else if (hash == "BeginRendering"_fnv) {
        // [ZWEI REGISTRIERUNGEN] Das Original haengt an pre-BeginRendering
        // ZWEI Callbacks: erst joints_tick, dann noch einmal
        // apply_hold. Das ist der getestete Zustand -- nicht zusammenfassen.
        joints_tick();
        apply_hold();
    }
}

void RE4VRChoke::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    flip_carry();

    if (hash == "UpdateJointExpression"_fnv) {
        joints_tick();
    } else if (hash == "LateUpdateBehavior"_fnv) {
        apply_hold();
    } else if (hash == "BeginRendering"_fnv) {
        apply_hold();
        // Nach ALLEN Writes des Frames den Versatz Wurzel->Hals nachmessen.
        neck_measure();
    }
}

// ============================================================================
// [CHOKE-RUHE] Steht drin, zuendet aber nicht -- __re4_choke_ruhe ist false.
// Wieder an: _G.__re4_choke_ruhe = true. Log-Praefix: [CHRUHE]
// ============================================================================

void RE4VRChoke::choke_ruhe_tick() {
    const auto zurueck = [&](const char* grund) {
        if (!m_ruhe_aktiv) {
            return;
        }

        if (m_ruhe_hc.obj != nullptr && m_ruhe_hc_was.has_value()) {
            re4vr::call_safe<void*>(m_ruhe_hc.obj, "set_Enabled", *m_ruhe_hc_was);
        }


        m_ruhe_aktiv = false;
        drop(m_ruhe_hc);
        m_ruhe_hc_was.reset();
    };

    if (re4vr::lua_get_tribool("__re4_choke_ruhe") != 1) {
        zurueck("abgeschaltet");
        return;
    }

    const double now = clock_now();
    const double seen = re4vr::lua_get_number("__re4_choke_seen", -999.0);
    const bool haelt = (now - seen) < 0.15;

    if (haelt && !m_ruhe_aktiv) {
        auto* ctx = player_ctx();
        auto* go = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;
        auto* komp = get_component(go, type_of("chainsaw.HitController"));

        if (komp != nullptr) {
            store(m_ruhe_hc, komp);
            m_ruhe_hc_was = opt_bool(komp, "get_Enabled");
            m_ruhe_aktiv = true;
            m_ruhe_seit = now;
        }
    } else if (m_ruhe_aktiv && !haelt) {
        zurueck("Griff zu Ende");
        return;
    }

    if (!m_ruhe_aktiv) {
        return;
    }

    // Zeitgrenze = Haltedauer-Slider + 1 s Luft. Nur als Notaus.
    const double grenze = re4vr::lua_get_number("__re4_choke_hold_max", 2.0) + 1.0;

    if ((now - m_ruhe_seit) > grenze) {
        zurueck("Zeitgrenze");
        return;
    }

    // Jeden Frame nachschreiben: die Engine schaltet Komponenten teils selbst
    // wieder scharf.
    re4vr::call_safe<void*>(m_ruhe_hc.obj, "set_Enabled", false);
}

// ============================================================================
// on_frame
// ============================================================================

void RE4VRChoke::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    // [TEST 13.09.2026] Die Tafel einmal pro Spielstart von selbst zeigen --
    // nur zum Ansehen, ohne Fledermaus. Haengt allein an
    // ACHIEVEMENT_TEST_AT_START und laesst die JSON unberuehrt, der echte
    // Fledermaus-Weg bleibt davon unabhaengig.
    if constexpr (ACHIEVEMENT_TEST_AT_START) {
        if (!m_achievement_test_armed) {
            m_achievement_test_armed = true;
            m_achievement_due = clock_now() + ACHIEVEMENT_TEST_AFTER;
        }
    }

    ensure_types();

    // [STICK-KLON] Die Kopie lebt laenger als der Griff -- ihr Ende haengt
    // deshalb am Frame, nicht an stick_return().
    stick_clone_tick();

    // [ZEITPUNKT 2026-08-26] Der eingeplante Gegnerlaut, sobald die Stop-Welle
    // durch ist.
    if (m_snd_retry.ego.obj != nullptr && !m_snd_retry.at.empty()) {
        const double now = clock_now();
        size_t i = 0;

        while (i < m_snd_retry.at.size()) {
            if (now >= m_snd_retry.at[i]) {
                m_snd_retry.at.erase(m_snd_retry.at.begin() + static_cast<long>(i));

                auto* ego2 = m_snd_retry.ego.obj;

                if (valid_not_false(ego2) && m_t_sndc != nullptr) {
                    auto* con2 = get_component(ego2, m_t_sndc);

                    if (con2 != nullptr) {
                        re4vr::call_safe<void*>(con2, "trigger(System.UInt32)",
                                                m_snd_retry.id);
                    }
                }
            } else {
                ++i;
            }
        }

        if (m_snd_retry.at.empty()) {
            drop(m_snd_retry.ego);
            m_snd_retry.id = 0;
        }
    }

    // [WUCHT-ENTSCHEIDUNG 2026-08-25] Der Stich ist ein paar Frames her, die
    // Spitze steht jetzt fest.
    if (m_pend.at > 0.0 && clock_now() >= m_pend.at) {
        m_pend.at = 0.0;

        float pk = static_cast<float>(re4vr::lua_get_number("vr_knife_velocity_peak", 0.0));

        if (m_pend.v0 > pk) {
            pk = m_pend.v0;
        }

        // Es zaehlt allein, ob der Gegner vom Stich noch GUELTIG in der Szene
        // steht -- ob er lebt oder ob wir ihn noch halten, ist egal.
        auto* vtf = m_pend.tf.obj;
        const bool can = vtf != nullptr && valid_not_false(vtf);
        const bool do_stick = can && (m_cfg.knife_stick_v <= 0.0f || pk >= m_cfg.knife_stick_v);

        if (do_stick) {
            // [LIVE-POSE AUCH HIER 2026-08-25] Die vorgemerkte Ausloese-Pose
            // wird NICHT mehr uebergeben -- sie stammt vom Anfang der
            // Stichbewegung, mit der Klinge noch quer.
            stick_into_victim(m_pend.ctx.obj, m_pend.tf.obj, nullptr, nullptr, m_pend.ktf.obj,
                              m_pend.kname, m_pend.kdist, m_pend.kcount, nullptr,
                              std::nullopt);

            // [STECK-TON 2026-08-26] Auf diesem Weg faellt die Entscheidung
            // erst jetzt, der normale Stich-Laut ist also schon gefallen.
            if (auto* sc2 = knife_sound_container(); sc2 != nullptr) {
                re4vr::call_safe<void*>(sc2, "trigger(System.UInt32)", KNIFE_STUCK_SND);
            }

            release();   // laeuft der Griff noch, endet er jetzt
        }

        drop(m_pend.ctx);
        drop(m_pend.tf);
        drop(m_pend.ktf);
        m_pend.wp.reset();
        m_pend.wr.reset();
        m_pend.kname.clear();
        m_pend.kdist.reset();
        m_pend.kcount = 0;

        vlog_add(m_pend.v0, pk, do_stick);
    }

    // [MESSER-SEITE GEPRUEFT 2026-08-28] Waehrend das Messer steckt, laeuft
    // der Waffen-RUECKWECHSEL -- und der TAUSCHT das Messer-GO aus. stick.tf
    // wird damit stale, und set_Parent/set_DrawSelf darauf ist eine native AV.
    if (m_stick.tf.obj != nullptr && !valid_not_false(m_stick.tf.obj)) {
        stick_return();   // setzt stick.tf = nullptr
    }

    if (m_stick.tf.obj != nullptr) {
        // Nach dem Stich haengt die Gueltigkeit am Gegner-Transform, nicht
        // mehr an `held`. Raeumt die Engine ihn ab, kommt das Messer sofort
        // zurueck.
        const bool vic_ok = m_stick.vic_tf.obj == nullptr
            || valid_not_false(m_stick.vic_tf.obj);

        if (clock_now() >= m_stick.until_t || !vic_ok) {
            stick_return();
        } else if (m_stick.clone_way) {
            // [KLON-WEG] Nichts zu tun: die echte Waffe haengt unveraendert in
            // der Hand (nur unsichtbar), im Gegner steckt die Kopie. Weder der
            // motion-Herzschlag noch das Sichtbar-Zwingen duerfen hier laufen
            // -- beide gehoeren zum alten Weg, in dem die ECHTE Waffe im
            // Gegner hing.
        } else {
            // [PIN AUS] Herzschlag fuer motion
            re4vr::lua_set_number("__re4_choke_knife_stuck", clock_now());

            // [SICHTBAR HALTEN 2026-08-26] Stirbt der Gegner am Stich,
            // versteckt das Spiel das Messer-GO -- es hing immer richtig, es
            // wurde nur nicht gezeichnet.
            if (re4vr::lua_get_tribool("__re4_choke_force_draw") != 0) {
                auto* kgo2 = re4vr::call_safe<::REManagedObject*>(m_stick.tf.obj,
                                                                  "get_GameObject");

                if (kgo2 != nullptr
                    && opt_bool(kgo2, "get_DrawSelf") == std::optional<bool>{false}) {
                    re4vr::call_safe<void*>(kgo2, "set_DrawSelf", true);
                }
            }
        }
    }

    // [NACHMESSUNG] Erst jetzt stimmen die Weltmatrizen -- ein get_Position im
    // Setz-Frame liefert noch den alten Wert (Lehre vom 24.08.).
    if (m_stick.tf.obj != nullptr && m_stick.check_at.has_value()
        && clock_now() >= *m_stick.check_at) {
        m_stick.check_at = clock_now() + 0.30;

        // [NACHMESSUNG-LOG AUSGEBAUT 2026-09-08] Der Mitschnitt nach
        // re4_choke_stick.txt ist raus.
    }

    // [ABDUNKELN -- WACHHUND] Es steckt nichts mehr, das Messer ist aber noch
    // gedimmt? Sofort zurueck. Das ist das ECHTE Messer.
    if (m_dim.on && m_stick.tf.obj == nullptr) {
        dim_apply(false);
    }

    // [EINBLENDEN -- WACHHUND 13.09.2026] Dasselbe fuer die Skalierung: steckt
    // nichts mehr und laeuft auch kein Klon mehr, darf das echte Messer nicht
    // auf LocalScale 0 stehenbleiben.
    //
    // BEWUSST hier und nicht in einem fremden Modul: der Choke haelt real_tf
    // selbst, stick_hide_real() prueft das Handle vorher (valid_not_false) und
    // raeumt es sonst auf, und geschrieben wird ausschliesslich die Skalierung
    // DIESES einen Transforms -- kein set_Parent, kein fremder Zustand, kein
    // Dauerfeuer. Genau das unterscheidet es vom ausgebauten knife_home_guard.
    if (m_stick.real_hidden && m_stick.tf.obj == nullptr
        && m_stick.clone.obj == nullptr) {
        stick_hide_real(m_stick.real_tf.obj, false);
    }

    // [RUECKWECHSEL] Vor allem anderen: steht ein Wechsel an, ist er faellig.
    if (m_reequip_at > 0.0 && clock_now() >= m_reequip_at) {
        if (clock_now() > m_reequip_until) {
            m_reequip_at = 0.0;   // Fenster abgelaufen -> aufgeben
        } else if (re4vr::lua_get_tribool("__re4_frame_is_gameplay") != 1) {
            // [STAGGER 13.09.2026 -- Ansage "da ist der Choke auch zuende, egal
            // ob durch unseren Ablauf oder Stagger"] Hier wurde der anstehende
            // Wechsel WEGGEWORFEN, sobald ein einziger Frame nicht als Gameplay
            // zaehlte -- eine Trefferreaktion genuegt, und das Messer blieb.
            //
            // Jetzt wird nur GEWARTET: ausgefuehrt wird weiterhin
            // ausschliesslich auf einem Gameplay-Frame (die Deadlock-Sperre vom
            // 12.09. bleibt damit unangetastet), aber der Wunsch ueberlebt die
            // paar Frames der Reaktion. Nach Tod oder Ladevorgang laeuft das
            // 3-s-Fenster darueber ohnehin ab und der Wechsel faellt weg.
            // [HAENGER 12.09.2026] Die Deadlock-Lehre gilt unveraendert: in
            // einem Nicht-Gameplay-Frame wird NICHTS ausgefuehrt -- dort baut
            // die Engine die Waffen selbst um, und ein zweiter Wechsel mitten
            // im laufenden ist der belegte Selbst-Deadlock (Spinlock im
            // Spielcode). Gewartet werden darf aber.
            //
            // [STAGGER-FENSTER 13.09.2026 -- "wir staggern durch die Gegend und
            // danach sollte wieder die letzte Gun da sein"] Eine Trefferreaktion
            // dauert laenger als die urspruenglichen 3 s Restfenster, wenn sie
            // spaet kommt. Solange kein Gameplay laeuft, wird das Fenster
            // deshalb nachgezogen -- aber hoechstens bis REEQUIP_MAX_WAIT nach
            // dem Loslassen. Tod und Ladevorgang dauern laenger, dort faellt
            // der Wechsel damit weiter sauber weg.
            // [ABSTURZ BEIM LADEN 14.09.2026 -- 0xC0000374 in ntdll, Heap]
            // Das Warten war zu grosszuegig: ueber sechs Sekunden getragen,
            // landet ein anstehender Waffenwechsel mitten im LADEVORGANG, wo
            // die Engine die Waffen selbst umbaut -- genau das Muster aus der
            // Haenger-Notiz vom 12.09.
            //
            // Eine Trefferreaktion ist nach Bruchteilen einer Sekunde durch.
            // Also wird nur noch eine HALBE Sekunde am Stueck gewartet; dauert
            // die Nicht-Gameplay-Phase laenger (Tod, Laden, Demo), faellt der
            // Wechsel weg wie vor dem Umbau.
            if (m_reequip_wait_seit <= 0.0) {
                m_reequip_wait_seit = clock_now();
            }

            if ((clock_now() - m_reequip_wait_seit) > 0.5) {
                m_reequip_at = 0.0;
                m_reequip_until = 0.0;
                m_reequip_bare = false;
                m_reequip_wait_seit = 0.0;
            } else {
                const double neu = clock_now() + 0.1;

                if (neu > m_reequip_until) {
                    m_reequip_until = neu;
                }
            }
        } else {
            auto* pe2 = player_equipment();
            auto* cx2 = player_ctx();
            auto* hu2 = cx2 != nullptr
                ? re4vr::call_safe<::REManagedObject*>(cx2, "get_HeadUpdater")
                : nullptr;
            const auto knife_now = hu2 != nullptr ? opt_bool(hu2, "get_IsEquipKnife")
                                                  : std::nullopt;

            // Lua: `knife_now ~= false` -- nil zaehlt hier als "noch Messer".
            if (pe2 != nullptr && knife_now != std::optional<bool>{false}) {
                // [BAREHANDS 2026-09-12] Wer mit leeren Haenden zugepackt hat,
                // bekommt leere Haende zurueck -- sonst zieht EquipType.Main
                // die Hauptwaffe, die er nie gezogen hatte.
                if (m_reequip_bare) {
                    // [EINMAL 12.09.2026 -- Symptom "Messer aus dem Holster
                    // geholt, es haengt sich sofort wieder weg"] Der Main-Weg
                    // fasst alle 0,25 s nach, solange das Messer noch in der
                    // Hand ist -- fuer LEERE HAENDE ist genau das falsch: zieht
                    // der Spieler in diesem Fenster selbst ein Messer, steckt
                    // ihm der naechste Durchlauf es wieder weg. Also ein
                    // einziger Versuch, danach ist Schluss.
                    restore_bare_hands();
                    m_reequip_at = 0.0;
                    m_reequip_until = 0.0;
                    m_reequip_bare = false;
                } else {
                    // [EINMAL 13.09.2026 -- gemessen mit zzz_re4_pistole_probe.lua]
                    // Hier wurde alle 0,25 s nachgefasst, solange noch ein
                    // Messer in der Hand ist -- also bis zu 3 s lang. Wer in
                    // diesem Fenster selbst zur Pistole greift, verliert sie:
                    // reequip_last_weapon() beginnt mit clearRequest, und das
                    // loescht die Anforderung, die der Holster gerade gestellt
                    // hat. Im Log stand genau das, sechsmal hintereinander:
                    //   clearRequest [WIR] -> execChangeWeapon [WIR]
                    // waehrend __vr_pistol_holster_zone die ganze Zeit true war
                    // und wid auf 5001 (Messer) stehen blieb.
                    //
                    // Gegen das Verpuffen im Loesungs-Frame genuegt der
                    // verzoegerte START (0,10 s bzw. knife_stick_t + 0,15);
                    // dafuer braucht es kein Dauerfeuer.
                    reequip_last_weapon();
                    m_reequip_at = 0.0;
                    m_reequip_until = 0.0;
                    m_reequip_wait_seit = 0.0;
                }
            } else {
                m_reequip_at = 0.0;
            }
        }
    }

    // [ASHLEY-LINE 16.09.2026] Ihre faelligen Spielzeilen. Bauform 1:1 wie der
    // verzoegerte Gegnerlaut (m_snd_retry): Handle auf ihr GameObject,
    // get_component(soundlib.SoundContainer) -> trigger(System.UInt32). Ihr
    // chainsaw.SoundCh2a1z0Container haengt unter chainsaw.SoundPlayerContainer,
    // genau wie Leons Ch0a0z0Container, den play_body_sound() ueber denselben
    // Weg schon ausloest.
    if (!m_ashley_line_q.empty()) {
        ensure_types();

        const double now = clock_now();
        auto* ago = m_ashley_line_go.obj;
        size_t i = 0;

        while (i < m_ashley_line_q.size()) {
            if (now >= m_ashley_line_q[i].at) {
                const uint32_t id = m_ashley_line_q[i].id;
                m_ashley_line_q.erase(m_ashley_line_q.begin() + static_cast<long>(i));

                if (valid_not_false(ago) && m_t_sndc != nullptr) {
                    if (auto* con = get_component(ago, m_t_sndc); con != nullptr) {
                        re4vr::call_safe<void*>(con, "trigger(System.UInt32)", id);
                    }
                }
            } else {
                ++i;
            }
        }

        if (m_ashley_line_q.empty()) {
            drop(m_ashley_line_go);
        }
    }

    // [WARTEZEIT 16.09.2026] Ashleys Antwort auf eine Geste.
    reply_tick();

    // [TAUNT-DELAY 2026-09-12] Der wartende Spruch wird hier faellig -- VOR
    // jedem Ausstieg dieser Funktion, sonst verschluckt ihn ein Frame ohne VR
    // oder ein Release. Er spielt auch dann, wenn das Tier inzwischen wieder
    // los ist: die Wartezeit haengt am GRIFF, nicht am Halten.
    if (m_taunt_due > 0.0 && clock_now() >= m_taunt_due) {
        const int widx = m_taunt_pending_index;
        const bool ash = m_taunt_pending_ashley;

        m_taunt_due = 0.0;
        m_taunt_pending_index = -1;
        m_taunt_pending_ashley = false;

        if (widx >= 0) {
            // [ASHLEY 13.09.2026] Gleicher Weg, gleicher Regler -- nur ein
            // anderer Namensraum in der DLL.
            if (ash) {
                re4vr::play_ashley_wav(widx);

                // [ASHLEY MUND 16.09.2026] Der Mund folgt genau diesem Spruch.
                m_mouth_kind = 0;
                m_mouth_wav = widx;
                m_mouth_t0 = clock_now();
            } else {
                re4vr::play_taunt_wav(widx);
            }
        }
    }

    // [ACHIEVEMENT 13.09.2026] Genau wie der Spruch: die Tafel steht hier an,
    // eine Sekunde nach dem Griff an der ersten Fledermaus. Ton und Bild
    // starten im SELBEN Frame.
    if (m_achievement_due > 0.0 && clock_now() >= m_achievement_due) {
        m_achievement_due = 0.0;

        re4vr::play_achievement_wav();

        if (g_framework != nullptr) {
            // [ACHIEVEMENT 2 -- 15.09.2026] Dieselbe Ecke, derselbe Rahmen,
            // derselbe Jingle -- nur die Tafel ist eine andere.
            g_framework->start_achievement_overlay(ACHIEVEMENT_SECONDS, m_achievement_which);
        }

        m_achievement_which = 1;
    }

    auto* vr = VR::get().get();

    // [1:1] Lua fragt NUR `if not vr then return end` -- ob das HMD aktiv ist,
    // wird nie geprueft. Ein zusaetzliches is_hmd_active() waere strenger und
    // wuerde einen laufenden Griff nie mehr freigeben (der Gegner bliebe an den
    // Body geparentet haengen, mit stillgelegter Root Motion und toter Bein-IK).
    if (vr == nullptr) {
        choke_ruhe_tick();
        return;
    }

    // [HANDLE NICHT CACHEN 2026-08-28] Frueher lagen act_grip/lh_joy nach dem
    // ERSTEN erfolgreichen Holen fuer immer in den Locals -- wird das Action-Set
    // danach neu gebunden, ist das gemerkte Handle stale und es kommt NIE eine
    // Grip-Flanke ("erst nach Reset Scripts ging es"). Nativ holen wir ohnehin
    // jeden Frame frisch; der Rueckbau-Schalter __re4_choke_cache_grip haette
    // hier keine Entsprechung und entfaellt.
    const auto act_grip = vr->get_action_grip();
    const auto lh_joy = vr->get_left_joystick();

    // [LUA-WAHRHEIT] `if not (act_grip and lh_joy) then return end` steigt NUR
    // bei nil aus -- ein Handle 0 ist in Lua WAHR und das Script laeuft weiter.
    // Ein `== 0`-Ausstieg wuerde den ganzen Griff-Zweig ueberspringen: kein
    // Herzschlag __re4_choke_seen, kein tip_tick und vor allem KEIN release().
    // Genau derselbe Fehler wie beim linken Rumble im Minecart-Port.
    // Nativ gibt es kein nil -- die Handles kommen aus dem VR-Modul und sind
    // hier immer vorhanden, also entfaellt der Ausstieg ersatzlos.

    poll_parry();

    bool grip = false;

    try {
        grip = vr->is_action_active(act_grip, lh_joy);
    } catch (...) {
        grip = false;
    }

    if (m_held.ctx.obj != nullptr) {
        const double now = clock_now();

        // [WUCHT 2026-08-25] Steht die Steck-Entscheidung noch aus, darf der
        // Griff hier NICHT abgeraeumt werden. Der TOD des Gegners bricht
        // dagegen weiter SOFORT ab.
        const bool wait_stick = (m_pend.at > 0.0 && now < m_pend.at);

        if (!is_live(m_held.ctx.obj)) {
            re4vr::lua_set_string("__re4_choke_release_why", "tot");
            release();
            m_prev_grip = grip;
            choke_ruhe_tick();
            return;
        }

        // [HUHN] Eigene, laengere Haltedauer.
        const double hmax = static_cast<double>(
            m_held.is_animal ? m_cfg.chicken_hold : m_cfg.hold_max);

        // [TUNE-PIN 13.09.2026 -- Ansage "kann ich die ewig pinnen, auch wenn
        // ich left grab loslasse?"] Mit gesetztem Haken haelt der Griff, bis
        // ERNEUT zugepackt wird (Grip-Flanke) oder der Haken faellt. Der Tod
        // des Opfers loest weiterhin sofort -- der Ausstieg dafuer steht oben.
        // [LOSGERISSEN 13.09.2026 -- Ansage "sie ist weitergeflogen und mein
        // linker Arm bleibt steif"] Die Fledermaus fliegt trotz Parenting
        // weiter (gemessen: 8 m in zwei Sekunden, ihr eigener Fahrer schreibt
        // nach uns). Mit gesetztem DAUERGRIFF endet der Griff dann NIE -- und
        // die Handklemme bleibt stehen, obwohl laengst nichts mehr in der Hand
        // ist.
        //
        // Deshalb: was weiter als die doppelte Greifweite von der Hand weg ist,
        // ist losgerissen. Gilt fuer JEDE Art -- ein Gegner, der so weit weg
        // ist, haengt auch nicht mehr an uns.
        // [FLEDERMAUS-AUSBRUCH 14.09.2026 -- Ansage "2 Sekunden spaeter haut
        // sie ab"] Sie laesst sich nicht lange halten: nach bat_hold_s reisst
        // sie sich los, mit demselben Laut wie jedes andere Choke-Ende.
        // Der DAUERGRIFF-Haken sticht das -- sonst waere sie zum Einstellen
        // wieder nicht zu halten.
        if (m_held.species == SPECIES_BAT && !tune_pin_of_held()
            && (now - m_held.t0) > static_cast<double>(m_cfg.bat_hold_s)) {
            re4vr::lua_set_string("__re4_choke_release_why", "fledermaus_ausbruch");
            play_body_sound(m_cfg.chicken_break_snd);
            release();
            m_prev_grip = grip;
            choke_ruhe_tick();

            return;
        }

        // [KARENZ 13.09.2026] Erst nach einer halben Sekunde pruefen. Im ersten
        // Frame nach dem Griff steht die Fledermaus noch dort, wohin ihre
        // gerade abgeschaltete Flugbahn sie gesetzt hat -- ohne Karenz riss der
        // Notaus den Griff sofort wieder auf, noch bevor apply_hold sie
        // ueberhaupt an die Hand schreiben konnte.
        if (const auto hp_now = hand_pos();
            (now - m_held.t0) > 0.5 && hp_now.has_value() && m_held.tf.obj != nullptr) {
            glm::vec3 vp{};

            if (get_vec3(m_held.tf.obj, "get_Position", vp)) {
                const float weg = glm::length(vp - *hp_now);
                const float grenze = std::max(m_cfg.grab_dist, m_cfg.bat_grab_dist) * 2.0f;

                if (weg > grenze) {
                    re4vr::lua_set_string("__re4_choke_release_why", "losgerissen");
                    release();
                    m_prev_grip = grip;
                    choke_ruhe_tick();

                    return;
                }
            }
        }

        if (tune_pin_of_held()) {
            // Erneutes Zupacken beendet den Dauergriff -- und zwar auf genau
            // demselben Weg wie ein normales Ende (Laut, release, Ruhe-Tick,
            // raus), damit hier kein zweiter Ausstieg gepflegt werden muss.
            if (grip && !m_prev_grip) {
                re4vr::lua_set_string("__re4_choke_release_why", "erneut_zugepackt");
                play_body_sound(m_cfg.chicken_break_snd);
                release();
                m_prev_grip = grip;
                choke_ruhe_tick();

                return;
            }

            // Sonst: weiter halten. Pose, Stich und Herzschlag laufen unten
            // unveraendert -- nur die zwei Abbruchgruende (Grip los, Zeit um)
            // gelten nicht.
        } else if ((!grip || (now - m_held.t0 > hmax)) && !wait_stick) {
            // [CHOKE-ENDE 13.09.2026 -- Ansage "bei jedem chokeende, bei
            // ashley und bei enemies"] Frueher stand hier nur das Losreissen
            // am TIER (is_animal + Zeit abgelaufen bei gedruecktem Griff).
            // Jetzt klingt es an JEDEM Ende dieses Griffs -- egal ob die Zeit
            // abgelaufen ist oder du selbst loslaesst, und fuer Gegner, Ashley
            // und Tiere gleichermassen.
            //
            // BEWUSST NICHT beim Tod des Opfers: der Ausstieg dafuer steht
            // weiter oben (is_live) und hat seine eigenen Spiel-Laute.
            re4vr::lua_set_string("__re4_choke_release_why",
                                  !grip ? "grip_losgelassen" : "haltedauer_um");
            play_body_sound(m_cfg.chicken_break_snd);

            release();
            m_prev_grip = grip;
            choke_ruhe_tick();
            return;
        }

        // [CHOKE_UI] Herzschlag fuer andere Scripte. BEWUSST ein Zeitstempel
        // statt eines Flags: stirbt dieses Modul mitten im Griff, ist das
        // Fenster nach 0.15 s von selbst zu.
        re4vr::lua_set_number("__re4_choke_seen", now);

        // [GRIFF-SCHWELLE] Nur solange wir halten. nil statt 0, damit motion
        // gar nicht erst umschaltet.
        if (m_cfg.choke_swing_t > 0.0f) {
            re4vr::lua_set_number("__re4_choke_swing_threshold", m_cfg.choke_swing_t);
        } else {
            re4vr::lua_set_nil("__re4_choke_swing_threshold");
        }

        rh_track();

        // [FUER DIE NOTBREMSE] re4_vr_binding.lua haengt seine Zwangsloesung
        // der Drehsperre daran, statt eine eigene Zahl zu pflegen.
        // [HUHN 2026-09-11] Die ECHTE Haltedauer melden. binding haengt daran
        // seine Notbremse (hold_max + 0.5 s), die eine gesperrte Achse wieder
        // freigibt, falls ein Griff haengt. Mit dem Gegnerwert (2 s) gab sie
        // beim Huhn (5 s) nach 2.5 s Strafen und Drehen wieder frei -- mitten
        // im laufenden Griff.
        re4vr::lua_set_number("__re4_choke_hold_max",
                              m_held.is_animal ? m_cfg.chicken_hold : m_cfg.hold_max);

        // [HAND-Y] Von hier kommt nur noch, wie weit WIR uns seit dem Zugriff
        // selbst in der Hoehe bewegt haben (Treppe) -- und der Spielraum.
        if (m_cfg.y_clamp_on) {
            re4vr::lua_set_number("__re4_choke_hand_play", m_cfg.y_clamp);
        } else {
            re4vr::lua_set_nil("__re4_choke_hand_play");
        }

        // [HAND IMMER GLEICH 13.09.2026] Jeden Frame mitfuehren, damit der
        // Regler LIVE wirkt -- sonst gaelte der Wert erst beim naechsten Griff.
        re4vr::lua_set_number("__re4_choke_hand_lat", m_cfg.hand_lat);

        re4vr::lua_set_number("__re4_choke_y_lerp", m_cfg.y_lerp);

        if (!m_cfg.fix_height) {
            re4vr::lua_set_nil("__re4_choke_hand_y");
        }

        if (m_held.py0.has_value()) {
            auto* bt2 = player_body_tf();
            glm::vec3 pp2{};

            if (bt2 != nullptr && get_vec3(bt2, "get_Position", pp2)) {
                re4vr::lua_set_number("__re4_choke_dy", pp2.y - *m_held.py0);
            } else {
                re4vr::lua_set_number("__re4_choke_dy", 0.0);
            }
        }

        // [SPITZEN-TREFFER 2026-08-26] Der neue Weg misst selbst und braucht
        // nichts von motion.
        if (m_cfg.tip_on) {
            tip_tick(now);
            m_prev_grip = grip;
            choke_ruhe_tick();
            return;
        }

        const bool sw = re4vr::lua_get_tribool("vr_knife_swing") == 1;

        // [STICHRICHTUNG 2026-08-25] Nur die FLANKE entscheidet.
        if (sw && !m_held.sw_prev && swing_towards_victim()) {
            knife_hit(nullptr);
        }

        m_held.sw_prev = sw;
        m_prev_grip = grip;
        choke_ruhe_tick();
        return;
    }

    // [WARUM KEIN GRIFF 13.09.2026 -- Ansage "wieder kein grab"] Bei JEDEM
    // Zupacken den Grund veroeffentlichen, so wie __re4_pin_why es beim Messer
    // tut. Eine Lua-Sonde kann die Gates hier sonst nicht sehen, und geraten
    // wird nicht mehr. Kostet nur an der Grip-Flanke eine Zuweisung.
    if (grip && !m_prev_grip) {
        const char* warum = nullptr;

        if (!m_cfg.choke_on) {
            warum = "choke_aus";
        } else if ((clock_now() - m_last_release) < static_cast<double>(m_cfg.cooldown)) {
            warum = "cooldown";
        } else if (!is_leon_campaign()) {
            warum = "nicht_leon";
        } else if (!have_knife()) {
            warum = "kein_messer_im_inventar";
        } else if (!hand_pos().has_value()) {
            warum = "keine_handposition";
        }

        re4vr::lua_set_string("__re4_choke_why", warum != nullptr ? warum : "gates_ok");
    }

    // [MESSER-PFLICHT 2026-08-24] Kein Messer im Inventar -> gar nicht greifen.
    // [CHOKE GANZ UNTEN IN DER KETTE 15.09.2026 -- Ansage des Users] Jede
    // andere Aufgabe der linken Hand geht vor: Messer-Holster, Mag-Holster und
    // das Racken der Waffe. Steckt die Hand in einer dieser Zonen, gehoert die
    // Grip-Flanke DORT hin -- der Choke nimmt sie nicht.
    //
    // Es sind dieselben Marken, an denen RE4VRBinding schon heute entscheidet,
    // ob der linke Grip ueberhaupt als Messer-Grip zaehlt; gemessen werden sie
    // an der rohen Controller-Position, gelten also auch, wenn die sichtbare
    // Hand gerade geklemmt wird.
    //
    // Nur die AUSLOESUNG ist gesperrt; ein laufender Griff wird nicht angefasst.
    const char* busy_grund = nullptr;

    if (re4vr::lua_get_tribool("__re4_knife_lh_in_zone") == 1) {
        busy_grund = "messerholster_zone";
    } else if (re4vr::lua_get_tribool("__vr_in_mag_holster_zone") == 1) {
        busy_grund = "magholster_zone";
    } else if (re4vr::lua_get_tribool("__vr_rack_block_left_knife") == 1) {
        busy_grund = "rack_noetig";
    } else if ((clock_now() - re4vr::lua_get_number("__re4_lh_reload_grab_t", -999.0)) < 0.15) {
        // [NACHLADEN VOR CHOKE 16.09.2026] Klappe (Red9 / Samurai Edge), Bogen-
        // Hebel oder Bolt in Griffweite -- gesetzt von RE4VRReload2/5.
        busy_grund = "nachladegriff";
    } else if ((clock_now() - re4vr::lua_get_number("__re4_lh_support_near_t", -999.0)) < 0.15) {
        // [SUPPORT VOR CHOKE 17.09.2026] Hand am Vordergriff (Zwei-Hand-IK) --
        // gesetzt von RE4VRMotion::update_support_dock.
        busy_grund = "supportgriff";
    } else if ((clock_now() - re4vr::lua_get_number("__re4_lh_switch_near_t", -999.0)) < 0.15) {
        // [SCHALTER VOR CHOKE 17.09.2026] Hand am Feuerwahlschalter des
        // Gewehrs -- gesetzt von RE4VRReload2::rf_update_switch.
        busy_grund = "waffenschalter";
    }

    const bool hand_busy = busy_grund != nullptr;

    if (m_cfg.choke_on && grip && !m_prev_grip && hand_busy) {
        re4vr::lua_set_string("__re4_choke_why", busy_grund);
    }

    if (m_cfg.choke_on && grip && !m_prev_grip && !hand_busy
        && (clock_now() - m_last_release) >= static_cast<double>(m_cfg.cooldown)
        && is_leon_campaign() && have_knife()) {
        const auto hp = hand_pos();
        auto* tgt = hp.has_value() ? pick_target(*hp) : nullptr;

        // [TIER 2026-09-10/12] Gegner haben Vorrang -- steht keiner in Reichweite,
        // darf auch ein Tier gegriffen werden (Huhn, Maus, Kraehe, Fledermaus).
        //
        // ACHTUNG: animal NUR aus dem Tier-Zweig setzen. Waere es pauschal true,
        // liefe auch jeder GEGNER ueber die Tier-Wege (get_IsDead statt
        // IsEliminated, keine Grapple-Setter, Tier-Haltezeit) -- und der
        // Gegner-Choke soll unveraendert bleiben.
        bool animal = false;
        bool chick  = false;
        int  species = SPECIES_NONE;

        // [ASHLEY 13.09.2026] Zwischen Gegner und Tier: steht kein Gegner in
        // Reichweite, darf die BEGLEITUNG gegriffen werden -- aber erst, wenn
        // "WHAT A BAT JOKE" freigeschaltet ist. pick_ashley haelt die Sperre
        // selbst, hier steht bewusst keine zweite Abfrage.
        bool ashley = false;

        if (tgt == nullptr && hp.has_value()) {
            tgt = pick_ashley(*hp);
            ashley = tgt != nullptr;
        }

        if (tgt == nullptr && hp.has_value() && m_cfg.chicken_grab) {
            tgt = pick_animal(*hp, m_cfg.grab_dist, &species, &chick);
            animal = tgt != nullptr;
        }

        // [WARUM KEIN GRIFF 13.09.2026] Was die Zielsuche ergeben hat.
        re4vr::lua_set_string("__re4_choke_why",
            tgt == nullptr ? "nichts_in_reichweite"
                           : (ashley ? "ashley"
                                     : (animal ? "tier" : "gegner")));

        if (tgt != nullptr) {
            // Tier-Wege gelten fuer JEDE Art; is_chicken nur fuers Huhn, weil
            // allein held_rot die am Huhn gemessenen Korrekturwinkel braucht.
            m_held.is_animal = animal;
            m_held.is_chicken = animal && chick;
            m_held.is_ashley = ashley;
            m_held.species = animal ? species : SPECIES_NONE;

            // [ADA-MAUS 17.09.2026] Wer greift -- einmal beim Zupacken, nicht
            // jeden Frame. Waehlt bei der Maus Adas eigenen Reglersatz.
            const std::string player_body = obj_name_of(player_body_go());
            m_held.ada_player = player_body == ADA_BODY;

            grab(tgt);

            // [ACHIEVEMENT 13.09.2026] Erste gegriffene FLEDERMAUS -> die Tafel
            // steht an. Gemerkt wird sofort (nicht erst nach der Wartezeit),
            // damit ein Absturz in dieser Sekunde sie nicht erneut ausloest.
            if (animal && species == SPECIES_BAT) {
                achievement_arm();
            }

            // [TAUNT-WAV 2026-09-12] Griff an einem TIER -> ein zufaelliges der
            // eingebetteten eigenen WAVs (Shuffle-Beutel: jedes einmal, dann neu
            // gemischt). Gilt fuer alle vier Tierarten.
            //
            // Bei GEGNERN passiert hier bewusst NICHTS (Ansage 12.09.2026).
            //
            // Genau eine Ausloesung pro Griff: der Zweig haengt an der Flanke
            // `grip && !m_prev_grip`, nicht am gehaltenen Zustand.
            //
            // [TAUNT-DELAY 2026-09-12] Nur hier, also nur am TIER, darf der
            // Spruch nachlaufen. Gewuerfelt wird trotzdem JETZT -- so bleibt
            // der Beutel an die Zahl der Griffe gebunden.
            // [NUR LEONS KAMPAGNE 17.09.2026 -- Ansage des Users] Die Sprueche
            // sind Leons Stimme: sie kommen NUR, wenn Leon der Spieler ist UND
            // nicht Mercenaries laeuft. Ada (Separate Ways) greift Tiere weiter,
            // aber stumm. Geprueft beim Zupacken -- ein verzoegerter Spruch wird
            // dann gar nicht erst vorgemerkt.
            const bool leon_story = player_body == LEON_BODY
                && !re4vr::lua_get_bool("__re4_in_mercs", false);

            if (animal && leon_story) {
                const int widx = re4vr::taunt_wav_next(m_taunt_wav_bag, &m_taunt_wav_last);

                if (m_cfg.taunt_delay_s > 0.0f) {
                    m_taunt_pending_index = widx;
                    m_taunt_pending_ashley = false;
                    m_taunt_due = clock_now() + static_cast<double>(m_cfg.taunt_delay_s);
                } else {
                    m_taunt_due = 0.0;
                    m_taunt_pending_index = -1;
                    m_taunt_pending_ashley = false;
                    re4vr::play_taunt_wav(widx);
                }
            }

            // [ASHLEY-SPRUECHE 13.09.2026] Genau derselbe Weg wie beim Tier,
            // inklusive derselben Wartezeit (Regler taunt_delay_s) -- aber
            // NICHT bei jedem Griff: sie sagt erst beim 3. bis 4. wieder etwas,
            // sonst sind die acht Dateien in einer Minute durchgehoert.
            // Gezaehlt wird der Griff, nicht die Zeit.
            if (ashley) {
                // [ASHLEY-LINE 16.09.2026] Bei JEDEM Griff sagt sie etwas:
                // zuerst immer ASHLEY_CHOKE_LINE, danach entweder unser
                // eigener WAV-Spruch (nur alle 4-6 Griffe -- sonst sind die
                // acht Dateien in einer Minute durch) oder eine ihrer
                // uebrigen Spielzeilen im Shuffle.
                //
                // tgt ist bei ihr der PartnerContext; der SoundContainer
                // haengt an ihrem BodyGameObject.
                auto* ago = re4vr::call_safe<::REManagedObject*>(
                    tgt, "get_BodyGameObject");

                ++m_ashley_grabs;

                const bool own_wav = m_ashley_grabs >= m_ashley_next_at;

                if (own_wav) {
                    m_ashley_grabs = 0;
                    m_ashley_next_at = 4 + (std::rand() % 3);   // 4, 5 oder 6
                }

                const double now = clock_now();
                const double wav_at = now + ASHLEY_LINE_DELAY + ASHLEY_WAV_GAP;
                const double line2_at = now + ASHLEY_LINE_DELAY + ASHLEY_LINE2_GAP;

                if (ago != nullptr) {
                    store(m_ashley_line_go, ago);
                    m_ashley_line_q.clear();
                    m_ashley_line_q.push_back({now + ASHLEY_LINE_DELAY, ASHLEY_CHOKE_LINE});

                    // Kein eigener Spruch faellig -> die Luecke fuellt eine
                    // ihrer anderen Zeilen, damit nie nur die erste kommt.
                    if (!own_wav) {
                        m_ashley_line_q.push_back(
                            {line2_at, ashley_line_next(m_ashley_line_bag, &m_ashley_line_last)});
                    }
                }

                // Unser Spruch laeuft weiter ueber m_taunt_due, jetzt aber
                // IMMER verzoegert: ihre Spielzeile liegt davor. Der Regler
                // taunt_delay_s bleibt damit der Regler des TIERgriffs.
                if (own_wav) {
                    const int widx = re4vr::ashley_wav_next(m_ashley_wav_bag, &m_ashley_wav_last);

                    m_taunt_pending_index = widx;
                    m_taunt_pending_ashley = true;
                    m_taunt_due = wav_at;
                }
            }
        }
    }

    m_prev_grip = grip;
    choke_ruhe_tick();
}

// ============================================================================
// DEV-UI  "RE4VR - Choke"
// Werte sind LIVE -- die Pose selbst kommt aus der Aufnahme oben und wird hier
// nicht angetastet. Gespeichert wird bei jeder Aenderung.
// ============================================================================

void RE4VRChoke::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Choke")) {
        return;
    }

    bool dirty = false;

    // [ASHLEY ANTWORTET 16.09.2026 -- Ansage des Users] Ganz oben, weil gerade
    // daran gestellt wird. Beides wirkt sofort und liegt in der Choke-JSON.
    ImGui::Text("-- Ashleys Antwort auf eine Geste --");

    if (ImGui::SliderFloat("Antwort lauter (dB)##ck_rgain", &m_cfg.reply_gain_db,
                           -20.0f, 20.0f, "%.1f")) {
        dirty = true;
    }

    if (ImGui::SliderFloat("Antwort Wartezeit (s)##ck_rdelay", &m_cfg.reply_delay_s,
                           0.0f, 3.0f, "%.2f")) {
        dirty = true;
    }

    ImGui::Text("dB kommt AUF den allgemeinen Regler; der Abstand zieht danach ab.");
    ImGui::Separator();

    if (ImGui::Checkbox("Tiere auch greifen##ck_chick", &m_cfg.chicken_grab)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Haltedauer Tier (s)##ck_chhold", &m_cfg.chicken_hold,
                         0.1f, 0.5f, 30.0f, "%.1f")) {
        dirty = true;
    }

    {
        // Wwise-ID als Text: ein int-Feld kippt bei IDs ab 2^31 ins Negative.
        char sbuf[32];
        std::snprintf(sbuf, sizeof(sbuf), "%u", m_cfg.chicken_break_snd);

        if (ImGui::InputText("Sound am Choke-Ende (Wwise-ID, 0=aus)##ck_chsnd",
                             sbuf, sizeof(sbuf),
                             ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsDecimal)) {
            m_cfg.chicken_break_snd = static_cast<uint32_t>(std::strtoul(sbuf, nullptr, 10));
            dirty = true;
        }
    }

    ImGui::Text("   Huhn, Maus, Kraehe und Fledermaus (Kraehe/Fledermaus ungetestet)");

    // [TAUNT-WAV 2026-09-12] Lautstaerke der EIGENEN Sprach-WAVs. Wirkt sofort
    // auf den naechsten Sound und landet mit dirty in der Choke-JSON.
    // Der Probe-Knopf ist hier Pflicht, nicht Luxus: eingestellt wird am
    // DESKTOP (im Headset ist ImGui nicht lesbar), und dort steht kein Huhn zum
    // Greifen. Ohne ihn liesse sich der Pegel gar nicht beurteilen.
    if (ImGui::DragFloat("Lautstaerke eigene Sprueche (dB)##ck_tvol",
                         &m_cfg.taunt_vol_db, 0.25f, -40.0f, 12.0f, "%+.2f dB")) {
        re4vr::set_taunt_wav_gain_db(m_cfg.taunt_vol_db);
        dirty = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Probe##ck_tvol_test")) {
        re4vr::play_taunt_wav(re4vr::taunt_wav_next(m_taunt_wav_bag, &m_taunt_wav_last));
    }

    ImGui::Text("   0 = Datei unveraendert; gilt auch fuer die Mittelfinger-Geste");

    // [ADA LAUTER 17.09.2026] Nur Adas eigene Sprueche, AUF den Regler oben.
    if (ImGui::SliderFloat("Ada lauter (dB)##ck_adagain", &m_cfg.ada_gain_db,
                           -20.0f, 20.0f, "%+.1f dB")) {
        re4vr::set_ada_wav_extra_db(m_cfg.ada_gain_db);
        dirty = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Probe##ck_adagain_test")) {
        // TAUNT28..41 sind Adas (Index 27..40).
        re4vr::play_taunt_wav_db(27 + std::rand() % 14,
                                 re4vr::taunt_wav_gain_db() + m_cfg.ada_gain_db);
    }

    // [TAUNT-DELAY 2026-09-12] Direkt unter der Lautstaerke (Ansage). Wirkt NUR
    // auf den Tiergriff -- die Mittelfinger-Geste bleibt unangetastet.
    if (ImGui::DragFloat("Verzoegerung Tiergriff (s)##ck_tdelay",
                         &m_cfg.taunt_delay_s, 0.05f, 0.0f, 5.0f, "%.2f s")) {
        if (m_cfg.taunt_delay_s < 0.0f) {
            m_cfg.taunt_delay_s = 0.0f;
        }

        dirty = true;
    }

    ImGui::Text("   0 = sofort; nur beim Griff an Tieren, nicht bei der Geste");
    ImGui::Text("   Der Probe-Knopf spielt immer sofort (Pegel beurteilen)");

    if (ImGui::Checkbox("Aktiv##ck_on", &m_cfg.choke_on)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nur Leon (Kampagne)##ck_leon", &m_cfg.leon_campaign_only)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nur nach Parry##ck_rp", &m_cfg.require_parry)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Parry-Fenster (s)##ck_pw", &m_cfg.parry_window, 0.1f, 0.2f, 10.0f,
                         "%.1f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nur den Parierten greifen##ck_pto", &m_cfg.parry_target_only)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Messer beim Zugriff nach rechts (flipped)##ck_kr",
                        &m_cfg.knife_to_right)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nativer Treffer (Blut, riskant)##ck_nh", &m_cfg.use_native_hit)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Messerschaden -- normaler Stich##ck_dmg", &m_cfg.knife_dmg, 5.0f,
                         10.0f, 2000.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Messerschaden -- Messer bleibt stecken##ck_dmgs",
                         &m_cfg.knife_dmg_stick, 5.0f, 10.0f, 2000.0f, "%.0f")) {
        dirty = true;
    }

    // [SPITZEN-TREFFER 2026-08-26] AN = die Spitze muss durch den Gegner
    // gehen; AUS = der alte Weg mit allen Reglern darunter.
    ImGui::Separator();
    ImGui::Text("Stich-Erkennung");

    if (ImGui::Checkbox("Treffer ueber die Klingenspitze (empfohlen)##ck_tip", &m_cfg.tip_on)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Trefferradius Kopf/Hals (m)##ck_tipr", &m_cfg.tip_r, 0.01f, 0.05f,
                         0.60f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Sperre nach dem Zupacken (s)##ck_tiparm", &m_cfg.tip_arm, 0.05f,
                         0.0f, 2.0f, "%.2f")) {
        dirty = true;
    }

    ImGui::Text("   solange zaehlt das Einschnappen des Opfers nicht als Stich");

    if (ImGui::DragFloat("Mindesttempo der Spitze (m/s)##ck_tipv", &m_cfg.tip_speed, 0.05f,
                         0.0f, 6.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Klingenlaenge, falls ungemessen (m)##ck_tipl", &m_cfg.tip_len,
                         0.005f, 0.05f, 0.60f, "%.3f")) {
        dirty = true;
    }

    // Die gemessenen Laengen zum Ablesen -- damit man sieht, dass sich jedes
    // Messer selbst vermisst, statt es glauben zu muessen.
    if (m_blade_len_seen.empty()) {
        ImGui::Text("   noch keine Klinge vermessen (misst sich beim ersten Stich)");
    } else {
        for (const auto& [nm, L] : m_blade_len_seen) {
            ImGui::Text("   %s: %.3f m", nm.c_str(), L);
        }
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Stich beendet den Griff, Messer bleibt stecken##ck_ks",
                        &m_cfg.knife_stick)) {
        if (!m_cfg.knife_stick) {
            stick_return();
        }

        dirty = true;
    }

    if (ImGui::DragFloat("Nachlauf: wie lange es steckt (s)##ck_stick", &m_cfg.knife_stick_t,
                         0.05f, 0.1f, 5.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Max. Abstand zum Koerper (m, 0 = aus)##ck_sticksnap",
                         &m_cfg.stick_snap, 0.005f, 0.0f, 0.50f, "%.3f")) {
        dirty = true;
    }

    ImGui::Text("   zieht einen danebengegangenen Stich an den naechsten Knochen");

    // [NEGATIV ERLAUBT 14.09.2026 -- Ansage "stecken gut, aber etwas zu tief"]
    // Untergrenze war 0,000: wer bei 0,025 noch zu tief sitzt, hatte keinen Weg
    // mehr. Der Tier-Regler kann das laengst (-0,300..0,300), hier galt es nur
    // nie. Negativ = die Klinge wird entlang derselben Achse wieder herausgezogen.
    if (ImGui::DragFloat("Einschub in den Gegner (m, negativ = heraus)##ck_stickin",
                         &m_cfg.knife_stick_in,
                         0.005f, -0.30f, 0.30f, "%.3f")) {
        dirty = true;
    }

    ImGui::Text("   nur GEGNER -- bei Tieren ueberschreibt die Koerpermitte diesen Wert");

    // [TIER-EINSCHUB 2026-09-12] Eigener Wert fuer Tiere, weil dort die Mitte der
    // Bezugspunkt ist und das Messer von dort aus zu weit drin sitzt (Ansage).
    // Negativ erlaubt: das ist die Richtung, die hier gebraucht wird.
    // [BEDEUTUNG GEAENDERT 14.09.2026] Frueher: Versatz ab der Koerpermitte.
    // Jetzt: wie tief die SPITZE unter der Haut sitzt (0 = genau auf dem Mesh).
    if (ImGui::DragFloat("Spitze unter der Haut, Tiere (m)##ck_astickin",
                         &m_cfg.animal_stick_in, 0.002f, -0.10f, 0.30f, "%.3f")) {
        dirty = true;
    }

    ImGui::Text("   0 = Spitze genau auf dem Mesh, 0,010 = einen Zentimeter drin");

    ImGui::Text("   ab der Koerpermitte entlang der Klinge; 0 = genau die Mitte");

    if (ImGui::DragFloat(m_cfg.tip_on ? "Stecken erst ab Spitzentempo (m/s, 0 = immer)##ck_stickv"
                                      : "Stecken erst ab Wucht (m/s, 0 = immer)##ck_stickv",
                         &m_cfg.knife_stick_v, 0.1f, 0.0f, 20.0f, "%.2f")) {
        dirty = true;
    }

    if (m_cfg.tip_on) {
        ImGui::Text("   darunter: normaler Messertreffer, Messer bleibt in der Hand, Griff "
                    "laeuft weiter");
    }

    // Das Wartefenster gehoert zum alten Weg -- der Spitzen-Weg misst im
    // Trefferframe und wartet nie.
    if (!m_cfg.tip_on) {
        if (ImGui::DragFloat("Wartefenster fuer die Spitze (s)##ck_stickw",
                             &m_cfg.knife_stick_w, 0.01f, 0.0f, 0.50f, "%.2f")) {
            dirty = true;
        }
    }

    // [GRIFF-SCHWELLE] Gilt NUR im Griff. Das normale Melee behaelt seine
    // eigene Schwelle im Tree "RE4VR - Motion" -> "Messer".
    if (ImGui::DragFloat("Stich erkennen ab (m/s, nur im Griff, 0 = wie normal)##ck_swingt",
                         &m_cfg.choke_swing_t, 0.1f, 0.0f, 8.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nur Stiche nach vorn zaehlen (Ausholen ignorieren)##ck_fwd",
                        &m_cfg.fwd_only)) {
        dirty = true;
    }

    if (m_vlog.empty()) {
        ImGui::Text("Letzte Stiche: noch keiner gemessen");
    } else {
        ImGui::Text("Letzte Stiche (neueste unten):");

        for (const auto& r : m_vlog) {
            if (m_cfg.tip_on) {
                ImGui::Text("   Spitzentempo %5.2f m/s   ->  %s", r.v0,
                            r.st ? "STECKT" : "nur Schaden");
            } else {
                ImGui::Text("   Ausloesung %5.2f   Spitze %5.2f   ->  %s", r.v0, r.pk,
                            r.st ? "STECKT" : "nur Schaden");
            }
        }
    }

    // [KAPUTTES LOCAL] Reines Laufzeit-Flag: die UI schaltet es sofort, ein
    // save_cfg speichert es aber nicht (s. PORT_CHOKE_SPEC Abschnitt 2).
    if (ImGui::Checkbox("Laufende Gegnerlaute vorher stoppen##ck_snd", &m_stop_enemy_sounds)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Steckendes Messer abdunkeln##ck_dim", &m_cfg.knife_dim)) {
        if (!m_cfg.knife_dim && m_dim.on) {
            dim_apply(false);
        }

        dirty = true;
    }

    // [LABEL 2026-08-24] Der Wert ist ein FAKTOR auf die Farbe: kleiner =
    // dunkler, 1.00 = unveraendert.
    if (ImGui::DragFloat("Helligkeit im Gegner (1.00 = unveraendert)##ck_dimf",
                         &m_cfg.knife_dim_f, 0.01f, 0.05f, 1.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Groesse min (Hals-Hoehe m)##ck_hmin", &m_cfg.min_neck_y, 0.01f, 0.3f,
                         2.5f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Groesse max (Hals-Hoehe m)##ck_hmax", &m_cfg.max_neck_y, 0.01f, 0.3f,
                         3.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("Diagnose ins Log##ck_dbg", &m_cfg.debug)) {
        dirty = true;
    }


    ImGui::Separator();
    ImGui::Text("Griff");

    // Verschiebt den GEGNER gegenueber deiner Handflaeche. Bezug: die Linie
    // von deinem Koerper zur linken Hand.
    if (ImGui::DragFloat("X seitlich (m, + = rechts)##ck_side", &m_cfg.grip_side, 0.005f,
                         -1.0f, 1.0f, "%.3f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Y hoch/runter (m)##ck_up", &m_cfg.grip_up, 0.005f, -1.0f, 1.0f,
                         "%.3f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Z vor/zurueck (m, + = von mir weg)##ck_away", &m_cfg.grip_away,
                         0.005f, -1.0f, 1.0f, "%.3f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Reichweite horizontal (m)##ck_dist", &m_cfg.grab_dist, 0.01f, 0.2f,
                         3.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Hand min ueber seinen Fuessen (m)##ck_hmn", &m_cfg.hand_min_y, 0.01f,
                         0.0f, 2.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Hand max ueber seinen Fuessen (m)##ck_hmx", &m_cfg.hand_max_y, 0.01f,
                         0.2f, 3.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Haltedauer (s)##ck_hold", &m_cfg.hold_max, 0.1f, 0.5f, 10.0f,
                         "%.1f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Pause danach (s)##ck_cd", &m_cfg.cooldown, 0.1f, 0.0f, 5.0f,
                         "%.1f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("Blickrichtung einfrieren##ck_fy", &m_cfg.freeze_yaw)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Ruhig halten (Versatz beim Zugriff messen)##ck_log",
                        &m_cfg.lock_on_grab)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Rumpf einfrieren (wie SPINE_PIN bei Leon)##ck_tp", &m_cfg.torso_pin)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Rumpf aufrecht (Basis-Pose statt Haltung)##ck_tu",
                        &m_cfg.torso_upright)) {
        dirty = true;
    }

    // [1:1] FREEZE_MOTION ist in Lua NIE als local deklariert -- ein echtes
    // Global, das gespeichert und geladen wird, aber NIRGENDS ausgewertet:
    // held.mo_was wird nur gelesen und genullt, nie gesetzt. Die Checkbox ist
    // damit wirkungslos, bleibt aber stehen.
    {
        bool fm = re4vr::lua_get_tribool("FREEZE_MOTION") == 1;

        if (ImGui::Checkbox("Animation komplett einfrieren##ck_fm", &fm)) {
            re4vr::lua_set_bool("FREEZE_MOTION", fm);
            dirty = true;
        }
    }

    if (ImGui::Checkbox("Engine-Griffschalter (Const/Terrain)##ck_gf", &m_cfg.grapple_flags)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Bein-IK im Griff aus##ck_ik", &m_cfg.leg_ik_off)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Nach dem Griff zurueck zur letzten Waffe##ck_re",
                        &m_cfg.reequip_after)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Feste Wuergehoehe (an meinem Hals)##ck_fh", &m_cfg.fix_height)) {
        dirty = true;
    }

    if (ImGui::DragFloat("   ueber meinem Hals (m)##ck_neckup", &m_cfg.neck_up, 0.01f, -0.5f,
                         0.5f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("   Nachziehen X/Y (klein = weich, 1 = hart)##ck_ylerp", &m_cfg.y_lerp,
                         0.01f, 0.0f, 1.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("Klemme X/Y (nicht anheben/verreissen)##ck_yc", &m_cfg.y_clamp_on)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Spielraum X/Y (m, +/- um den Griffpunkt)##ck_yclamp", &m_cfg.y_clamp,
                         0.01f, 0.0f, 1.0f, "%.2f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("An Body haengen statt an L_Palm##ck_pb", &m_cfg.parent_to_body)) {
        dirty = true;
    }

    if (ImGui::DragFloat("Blickrichtung drehen (Grad)##ck_yaw", &m_cfg.face_yaw_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    // [HAND IMMER GLEICH 13.09.2026] Feste seitliche Lage der Wuergehand.
    if (ImGui::DragFloat("Wuergehand seitlich (+ = rechts)##ck_hlat", &m_cfg.hand_lat,
                         0.005f, -0.6f, 0.6f, "%.3f")) {
        dirty = true;
    }

    ImGui::Text("   0 = mittig vor dem Koerper; vor/zurueck bleibt frei");

    // [TIER-DREHUNG 12.09.2026 -- Ansage "stell diese Slider einfach auf die
    // Maus um ... also mouse/bat/crow"] Sie sind zurueck, wirken aber NUR auf
    // Rigs ohne Neck_1 (Maus, Kraehe, Fledermaus), die seit heute an der Huefte
    // haengen. Huhn und Gegner sehen sie nicht.
    ImGui::Text("MAUS drehen (Grad) -- Kraehe und Fledermaus haben eigene Werte");

    if (ImGui::DragFloat("Tier Yaw (Y)##ck_ayaw", &m_cfg.animal_yaw_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Tier Pitch (X)##ck_apitch", &m_cfg.animal_pitch_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Tier Roll (Z)##ck_aroll", &m_cfg.animal_roll_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::Checkbox("DAUERGRIFF zum Einstellen (Maus)##ck_apin", &m_cfg.animal_pin)) {
        dirty = true;
    }

    ImGui::Text("MAUS verschieben (m) -- zusaetzlich zum Griff-Versatz");

    if (ImGui::DragFloat("Tier X seitlich (+ = rechts)##ck_aox", &m_cfg.animal_off_x,
                         0.005f, -1.0f, 1.0f, "%.3f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Tier Y hoch/runter##ck_aoy", &m_cfg.animal_off_y,
                         0.005f, -1.0f, 1.0f, "%.3f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Tier Z vor/zurueck (+ = von mir weg)##ck_aoz", &m_cfg.animal_off_z,
                         0.005f, -1.0f, 1.0f, "%.3f")) {
        dirty = true;
    }

    if (ImGui::Button("Tierdrehung auf 0##ck_arst")) {
        m_cfg.animal_yaw_deg = 0.0f;
        m_cfg.animal_pitch_deg = 0.0f;
        m_cfg.animal_roll_deg = 0.0f;
        dirty = true;
    }

    ImGui::SameLine();

    if (ImGui::Button("Tierversatz auf 0##ck_aorst")) {
        m_cfg.animal_off_x = 0.0f;
        m_cfg.animal_off_y = 0.0f;
        m_cfg.animal_off_z = 0.0f;
        dirty = true;
    }

    // [EIGENE SAETZE JE ART 13.09.2026] Die Regler oben sind die MAUS und
    // behalten ihre Werte. Kraehe und Fledermaus haengen als eigene Baeume
    // darunter -- sie sind anders gebaut (die Kraehe hat Fluegel und einen
    // langen Hals, die Fledermaus gar keinen) und brauchen eigene Werte.
    // [HANDDREHUNG JE ART 13.09.2026 -- Ansage "das brauche ich pro
    // Choke-Partner"] Jeder Satz traegt jetzt drei Bloecke: das OPFER drehen,
    // das Opfer verschieben, und die eigene linke HAND drehen.
    const auto satz = [&](const char* titel, const char* kuerzel,
                          float& yaw, float& pitch, float& roll,
                          float& ox, float& oy, float& oz,
                          float& hy, float& hp, float& hr,
                          bool& pin) {
        if (!ImGui::TreeNode(titel)) {
            return;
        }

        char id[64];

        // [TUNE-PIN 13.09.2026] Zum Einstellen: haelt das Gegriffene, bis man
        // erneut zupackt -- sonst kommt man z. B. an eine Fledermaus nie lange
        // genug heran.
        std::snprintf(id, sizeof(id), "DAUERGRIFF zum Einstellen##ck_%s_pin", kuerzel);

        if (ImGui::Checkbox(id, &pin)) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Yaw (Y)##ck_%s_y", kuerzel);

        if (ImGui::DragFloat(id, &yaw, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Pitch (X)##ck_%s_p", kuerzel);

        if (ImGui::DragFloat(id, &pitch, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Roll (Z)##ck_%s_r", kuerzel);

        if (ImGui::DragFloat(id, &roll, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "X seitlich (+ = rechts)##ck_%s_ox", kuerzel);

        if (ImGui::DragFloat(id, &ox, 0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Y hoch/runter##ck_%s_oy", kuerzel);

        if (ImGui::DragFloat(id, &oy, 0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Z vor/zurueck (+ = von mir weg)##ck_%s_oz", kuerzel);

        if (ImGui::DragFloat(id, &oz, 0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "HAND Yaw (Y)##ck_%s_hy", kuerzel);

        if (ImGui::DragFloat(id, &hy, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "HAND Pitch (X)##ck_%s_hp", kuerzel);

        if (ImGui::DragFloat(id, &hp, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "HAND Roll (Z)##ck_%s_hr", kuerzel);

        if (ImGui::DragFloat(id, &hr, 1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        std::snprintf(id, sizeof(id), "Alles auf 0##ck_%s_rst", kuerzel);

        if (ImGui::Button(id)) {
            yaw = 0.0f;
            pitch = 0.0f;
            roll = 0.0f;
            ox = 0.0f;
            oy = 0.0f;
            oz = 0.0f;
            hy = 0.0f;
            hp = 0.0f;
            hr = 0.0f;
            dirty = true;
        }

        ImGui::TreePop();
    };

    satz("Kraehe (eigene Werte)##ck_crow", "crow",
         m_cfg.crow_yaw_deg, m_cfg.crow_pitch_deg, m_cfg.crow_roll_deg,
         m_cfg.crow_off_x, m_cfg.crow_off_y, m_cfg.crow_off_z,
         m_cfg.crow_hand_yaw, m_cfg.crow_hand_pitch, m_cfg.crow_hand_roll,
         m_cfg.crow_pin);

    // [MAUS 14.09.2026] Nur die HANDdrehung -- ihre Koerperwerte kommen weiter
    // aus dem allgemeinen Tiersatz oben, damit Kraehe, Fledermaus und Huhn
    // davon unberuehrt bleiben.
    if (ImGui::TreeNode("Maus -- eigene Handdrehung##ck_mouse")) {
        if (ImGui::DragFloat("Hand Yaw (Y)##ck_mhy", &m_cfg.mouse_hand_yaw,
                             1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Hand Pitch (X)##ck_mhp", &m_cfg.mouse_hand_pitch,
                             1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Hand Roll (Z)##ck_mhr", &m_cfg.mouse_hand_roll,
                             1.0f, -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        ImGui::Text("   gilt NUR fuer die Maus; Koerperlage kommt aus dem Tiersatz");

        if (ImGui::Button("Maus-Handdrehung auf 0##ck_mh0")) {
            m_cfg.mouse_hand_yaw = 0.0f;
            m_cfg.mouse_hand_pitch = 0.0f;
            m_cfg.mouse_hand_roll = 0.0f;
            dirty = true;
        }

        ImGui::TreePop();
    }

    // [ADA-MAUS 17.09.2026 -- Ansage des Users] Adas Maus in Separate Ways.
    satz("Maus ADA (eigene Werte)##ck_ada_mouse", "ada_mouse",
         m_cfg.ada_mouse_yaw_deg, m_cfg.ada_mouse_pitch_deg, m_cfg.ada_mouse_roll_deg,
         m_cfg.ada_mouse_off_x, m_cfg.ada_mouse_off_y, m_cfg.ada_mouse_off_z,
         m_cfg.ada_mouse_hand_yaw, m_cfg.ada_mouse_hand_pitch, m_cfg.ada_mouse_hand_roll,
         m_cfg.ada_mouse_pin);

    satz("Fledermaus (eigene Werte)##ck_bat", "bat",
         m_cfg.bat_yaw_deg, m_cfg.bat_pitch_deg, m_cfg.bat_roll_deg,
         m_cfg.bat_off_x, m_cfg.bat_off_y, m_cfg.bat_off_z,
         m_cfg.bat_hand_yaw, m_cfg.bat_hand_pitch, m_cfg.bat_hand_roll,
         m_cfg.bat_pin);

    // [FLEDERMAUS-RADIUS 13.09.2026] Steht bewusst AUSSERHALB des Baums: er ist
    // die einzige Art mit eigener Greifweite, und man soll sie finden, ohne den
    // Baum aufzuklappen.
    if (ImGui::DragFloat("Greifweite NUR Fledermaus (m)##ck_batdist", &m_cfg.bat_grab_dist,
                         0.05f, 0.5f, 6.0f, "%.2f")) {
        dirty = true;
    }

    ImGui::Text("   Sie fliegt vorbei -- alle anderen Tiere behalten die normale Greifweite");

    // [AUSBRUCH 14.09.2026] Wie lange sie sich halten laesst.
    if (ImGui::DragFloat("Fledermaus reisst sich los nach (s)##ck_bathold", &m_cfg.bat_hold_s,
                         0.1f, 0.3f, 15.0f, "%.1f")) {
        dirty = true;
    }

    ImGui::Text("   Mit Leons Loslass-Laut; der DAUERGRIFF-Haken sticht diese Zeit");

    // [HUHN + GEGNER 13.09.2026 -- Ansage "auch jeweils einen eigenen Tree"]
    // Seit heute laufen beide ueber denselben HAND-Bezug wie Ashley und die
    // necklosen Tiere -- also brauchen sie auch je einen eigenen Satz. Beim
    // Gegner steht Yaw ab Werk auf 180: die Hand-Basis zeigt von dir weg, ohne
    // die halbe Drehung schaute er in dieselbe Richtung wie du.
    satz("Huhn (eigene Werte)##ck_chick2", "chick2",
         m_cfg.chick_yaw_deg, m_cfg.chick_pitch_deg, m_cfg.chick_roll_deg,
         m_cfg.chick_off_x, m_cfg.chick_off_y, m_cfg.chick_off_z,
         m_cfg.chick_hand_yaw, m_cfg.chick_hand_pitch, m_cfg.chick_hand_roll,
         m_cfg.chick_pin);

    satz("Gegner (eigene Werte)##ck_ene", "ene",
         m_cfg.ene_yaw_deg, m_cfg.ene_pitch_deg, m_cfg.ene_roll_deg,
         m_cfg.ene_off_x, m_cfg.ene_off_y, m_cfg.ene_off_z,
         m_cfg.ene_hand_yaw, m_cfg.ene_hand_pitch, m_cfg.ene_hand_roll,
         m_cfg.ene_pin);

    // [ASHLEY 13.09.2026 -- Ansage "eigene Slider, exklusiv fuer sie"] Ihr
    // eigener Baum mit Drehung UND Versatz. Die Werte gelten ausschliesslich,
    // solange SIE in der Hand haengt -- Gegner und Tiere sehen davon nichts.
    if (ImGui::TreeNode("Ashley verschieben (eigene Werte)##ck_ash")) {
        ImGui::Text("Nur waehrend Ashley gehalten wird, zusaetzlich zum Griff-Versatz");

        if (ImGui::Checkbox("DAUERGRIFF zum Einstellen##ck_ash_pin", &m_cfg.ashley_pin)) {
            dirty = true;
        }

        // [ASHLEY MUND 16.09.2026] Mund auf/zu zu unseren Spruechen: ein Haken,
        // zwei Regler, ein Speichern -- mehr nicht.
        ImGui::Checkbox("Mund bewegt sich zu ihren Spruechen##ck_ash_mouth",
                        &m_cfg.mouth_on);

        ImGui::DragFloat("Mund Ausschlag (Aufreissen)##ck_ash_msc", &m_cfg.mouth_scale,
                         0.1f, 0.0f, 8.0f, "%.2f");

        ImGui::DragFloat("Mund Tempo##ck_ash_msp", &m_cfg.mouth_speed,
                         0.05f, 0.25f, 3.0f, "%.2f");

        if (ImGui::Button("Mund-Werte speichern##ck_ash_msave")) {
            dirty = true;
        }

        ImGui::Text("Sie sitzt seit 13.09. in JEDEM Griff gleich (Hand-Bezug am Neck_1)");

        if (ImGui::DragFloat("Yaw (Y)##ck_ash_y", &m_cfg.ashley_yaw_deg, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Pitch (X)##ck_ash_p", &m_cfg.ashley_pitch_deg, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Roll (Z)##ck_ash_r", &m_cfg.ashley_roll_deg, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("X seitlich (+ = rechts)##ck_ash_ox", &m_cfg.ashley_off_x,
                             0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Y hoch/runter##ck_ash_oy", &m_cfg.ashley_off_y,
                             0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("Z vor/zurueck (+ = von mir weg)##ck_ash_oz", &m_cfg.ashley_off_z,
                             0.005f, -1.0f, 1.0f, "%.3f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("HAND Yaw (Y)##ck_ash_hy", &m_cfg.ashley_hand_yaw, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("HAND Pitch (X)##ck_ash_hp", &m_cfg.ashley_hand_pitch, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::DragFloat("HAND Roll (Z)##ck_ash_hr", &m_cfg.ashley_hand_roll, 1.0f,
                             -180.0f, 180.0f, "%.0f")) {
            dirty = true;
        }

        if (ImGui::Button("Ashley alles auf 0##ck_ash_rst")) {
            m_cfg.ashley_hand_yaw = 0.0f;
            m_cfg.ashley_hand_pitch = 0.0f;
            m_cfg.ashley_hand_roll = 0.0f;
            m_cfg.ashley_yaw_deg = 0.0f;
            m_cfg.ashley_pitch_deg = 0.0f;
            m_cfg.ashley_roll_deg = 0.0f;
            m_cfg.ashley_off_x = 0.0f;
            m_cfg.ashley_off_y = 0.0f;
            m_cfg.ashley_off_z = 0.0f;
            dirty = true;
        }

        ImGui::TreePop();
    }

    // [HAND-DREHUNG 12.09.2026 -- Ansage] Hier standen "Huhn Yaw/Pitch/Roll",
    // die das gehaltene TIER gedreht haben. Gewollt war aber die eigene linke
    // HAND: "ich wollte nicht das HUHN drehen, sondern meine linke hand waehrend
    // ich das huhn halte". Die Huhn-Regler lagen ohnehin auf 0 und standen nicht
    // in der gespeicherten JSON -- sie waren also nie in Benutzung. chick_*_deg
    // und held_rot bleiben im Code (auf 0 exakt die Identitaet) und koennen
    // jederzeit raus, falls sie nicht mehr gebraucht werden.
    ImGui::Text("Linke Hand drehen (Grad) -- MAUS bzw. Tier ohne eigene Werte");
    ImGui::Text("   Kraehe, Fledermaus, Huhn, Gegner und Ashley: in ihrem eigenen Baum");

    if (ImGui::DragFloat("Hand Yaw (Y)##ck_hyaw", &m_cfg.hand_yaw_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Hand Pitch (X)##ck_hpitch", &m_cfg.hand_pitch_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::DragFloat("Hand Roll (Z)##ck_hroll", &m_cfg.hand_roll_deg, 1.0f,
                         -180.0f, 180.0f, "%.0f")) {
        dirty = true;
    }

    if (ImGui::Button("Hand-Drehung zuruecksetzen##ck_hreset")) {
        m_cfg.hand_yaw_deg = 0.0f;
        m_cfg.hand_pitch_deg = 0.0f;
        m_cfg.hand_roll_deg = 0.0f;
        dirty = true;
    }

    ImGui::Text("   additiv auf die Controller-Haltung; 0/0/0 = unveraendert");

    ImGui::Separator();
    ImGui::Text("Handpose (Aufnahme 'choke')");

    if (ImGui::Checkbox("Pose anwenden##ck_po", &m_cfg.pose_on)) {
        dirty = true;
    }

    if (ImGui::Checkbox("Pose dauernd forcen (Einstellen)##ck_pf", &m_cfg.pose_force)) {
        dirty = true;
    }

    if (ImGui::TreeNode("Daumen (additiv auf die Pose)")) {
        const std::array<std::pair<glm::vec3*, const char*>, 3> thumbs{{
            {&m_cfg.thumb_t1, "Thumb1 (Wurzel)"},
            {&m_cfg.thumb_t2, "Thumb2 (Mitte)"},
            {&m_cfg.thumb_t3, "Thumb3 (Spitze)"},
        }};
        const std::array<const char*, 3> keys{"t1", "t2", "t3"};

        for (size_t i = 0; i < thumbs.size(); ++i) {
            auto* t = thumbs[i].first;
            ImGui::Text("%s", thumbs[i].second);

            char id[64]{};

            std::snprintf(id, sizeof(id), "X (beugen)##ck_%sx", keys[i]);

            if (ImGui::DragFloat(id, &t->x, 0.5f, -180.0f, 180.0f, "%.1f")) {
                dirty = true;
            }

            std::snprintf(id, sizeof(id), "Y (drehen)##ck_%sy", keys[i]);

            if (ImGui::DragFloat(id, &t->y, 0.5f, -180.0f, 180.0f, "%.1f")) {
                dirty = true;
            }

            std::snprintf(id, sizeof(id), "Z (spreizen)##ck_%sz", keys[i]);

            if (ImGui::DragFloat(id, &t->z, 0.5f, -180.0f, 180.0f, "%.1f")) {
                dirty = true;
            }

            ImGui::Separator();
        }

        if (ImGui::Button("Daumen zuruecksetzen##ck_treset")) {
            m_cfg.thumb_t1 = glm::vec3{0.0f, 0.0f, 0.0f};
            m_cfg.thumb_t2 = glm::vec3{0.0f, 0.0f, 0.0f};
            m_cfg.thumb_t3 = glm::vec3{0.0f, 0.0f, 0.0f};
            dirty = true;
        }

        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text(m_held.ctx.obj != nullptr ? "Im Griff: ja" : "Im Griff: nein");
    ImGui::Text(parry_open() ? "Parry-Fenster: OFFEN" : "Parry-Fenster: zu");

    if (dirty) {
        save_cfg();
    }

    ImGui::TreePop();
}

#endif // RE4
