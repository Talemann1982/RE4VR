// ============================================================================
// RE4VRGuestures -- 1:1-Portierung von re4_vr_guestures.lua. Siehe
// RE4VRGuestures.hpp fuer die Bausteine und die Reihenfolge im Mod-Vektor.
//
// Spezifikation: I:\LUATRANS\PORT_GUESTURES_SPEC.md
// ============================================================================
#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SystemArray.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"

#include "RE4VRChoke.hpp"
#include "RE4VRGuestures.hpp"

#undef min
#undef max

namespace {
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

// pcall-Aequivalent fuer void-Aufrufe: eine fehlende Methode ist in Lua kein
// Fehler, ein Aufruf auf einer Leiche sehr wohl.
template <typename... Args>
bool call_pcall(::REManagedObject* obj, std::string_view name, Args... args) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    const auto m = td != nullptr ? td->get_method(name) : nullptr;

    if (m == nullptr) {
        return true;
    }

    auto context = sdk::get_thread_context();
    bool ok = false;

    try {
        m->call_safe<void*>(context, obj, args...);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
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

template <typename... Args>
std::optional<int32_t> opt_int(::REManagedObject* obj, std::string_view name, Args... args) {
    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(obj, name, v, args...)) {
        return std::nullopt;
    }

    return v;
}

template <typename... Args>
std::optional<bool> opt_bool(::REManagedObject* obj, std::string_view name, Args... args) {
    bool v = false;

    if (!re4vr::try_call<bool>(obj, name, v, args...)) {
        return std::nullopt;
    }

    return v;
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

::REManagedObject* type_of(const char* name) {
    auto* td = sdk::find_type_definition(name);

    return td != nullptr ? (::REManagedObject*)td->get_runtime_type() : nullptr;
}

// [FRAME-CACHE] Ueber re4vr::fc -- der Singleton-Lookup ist der teuerste Teil
// der Spieler-Kette und lief bisher bei JEDEM Aufruf neu (in apply_hold & Co.
// fuenfmal pro Frame). Verhalten unveraendert: der Cache haelt nichts ueber den
// Frame hinaus und laesst sich mit __re4_fc_off abschalten.
::REManagedObject* character_manager() {
    return re4vr::fc::managed_singleton("chainsaw.CharacterManager");
}

// Ist das ein echtes System.Array? Nur dann liefert get_size in Lua einen Wert
// (auch 0); sonst scheitert das Binding und der or-Zweig greift.
bool is_system_array(::REManagedObject* o) {
    if (!re4vr::obj_ok(o)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(o);

    if (td == nullptr) {
        return false;
    }

    const auto name = td->get_full_name();

    return name.size() >= 2 && name.compare(name.size() - 2, 2, "[]") == 0;
}

// Ein Feld typgerecht schreiben -- Luas set_field(name, value).
// [CONTAINER-FLAG] get_data_raw(obj, is_value_type) beschreibt den CONTAINER,
// nicht den Feldtyp: bei einem Managed Object immer false.
// Der FELDtyp kommt aus der TDB; ein int in ein System.Single zu schreiben
// waere ein Bitmuster im falschen Format.
bool set_field_num(::REManagedObject* obj, const char* name, double value) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    if (td == nullptr) {
        return false;
    }

    auto* f = td->get_field(name);

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
        // [UEBERLAUF] _SoundTriggerId ist 4294967295 und _JointNameHash
        // 2180083513 -- beide liegen UEBER INT32_MAX. Ein static_cast<int32_t>
        // aus einem double ist dort undefiniert und liefert auf x64
        // 0x80000000. REFrameworks Lua-Bruecke schreibt sie als uint32.
        *reinterpret_cast<uint32_t*>(p) = static_cast<uint32_t>(
            static_cast<uint64_t>(value));
    } else if (tn == "System.Int32") {
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(value);
    } else if (tn == "System.UInt16") {
        *reinterpret_cast<uint16_t*>(p) = static_cast<uint16_t>(value);
    } else if (tn == "System.Int16") {
        *reinterpret_cast<int16_t*>(p) = static_cast<int16_t>(value);
    } else if (tn == "System.Byte") {
        *reinterpret_cast<uint8_t*>(p) = static_cast<uint8_t>(value);
    } else if (tn == "System.SByte") {
        *reinterpret_cast<int8_t*>(p) = static_cast<int8_t>(value);
    } else {
        // Enums und alles Uebrige liegen als 4-Byte-Wert vor.
        *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(value);
    }

    return true;
}

bool set_field_obj(::REManagedObject* obj, const char* name, ::REManagedObject* value) {
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    if (td == nullptr) {
        return false;
    }

    auto* f = td->get_field(name);

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

constexpr const char* JSON_PATH = "re4_vr/re4_vr_guestures.json";
constexpr const char* LEON_BODY = "ch0a0z0_body";
constexpr const char* ADA_BODY = "ch3a8z0_body";

// [FEST IM FORK 17.09.2026 -- Ansage "alles fest in den fork und die json weg"]
// Der komplette Spruch-Pool, frueher re4_vr/re4_vr_voice.json. Je Zeile:
// Body-GO, Wwise-ID (0 = keine), eigenes WAV 1..TAUNT_WAV_COUNT (0 = keins),
// Geste ("" = beide, "fuck_you" = Mittelfinger, "point" = Zeigefinger).
// Wer keinen Eintrag hat, bleibt still -- nie ein fremder Pool.
struct TauntRow {
    const char* body;
    uint32_t    id;
    int         wav;
    const char* gest;
};

constexpr TauntRow TAUNT_TABLE[] = {
    // --- Leon ---
    {LEON_BODY, 656524313u, 0, ""},
    {LEON_BODY, 667389037u, 0, ""},
    {LEON_BODY, 678132341u, 0, ""},
    {LEON_BODY, 995187347u, 0, ""},
    {LEON_BODY, 1074540596u, 0, ""},
    {LEON_BODY, 1438004164u, 0, ""},
    {LEON_BODY, 1438167516u, 0, ""},
    {LEON_BODY, 1593668011u, 0, ""},
    {LEON_BODY, 1652710903u, 0, ""},
    {LEON_BODY, 1697718995u, 0, ""},
    {LEON_BODY, 1701791565u, 0, ""},
    {LEON_BODY, 1721359382u, 0, ""},
    {LEON_BODY, 1824537186u, 0, ""},
    {LEON_BODY, 1830672715u, 0, ""},
    {LEON_BODY, 2055206710u, 0, ""},
    {LEON_BODY, 2113172150u, 0, ""},
    {LEON_BODY, 2148208096u, 0, ""},
    {LEON_BODY, 2157710615u, 0, ""},
    {LEON_BODY, 2328053651u, 0, ""},
    {LEON_BODY, 2340400026u, 0, ""},
    {LEON_BODY, 2406243188u, 0, ""},
    {LEON_BODY, 2408945033u, 0, ""},
    {LEON_BODY, 2470058253u, 0, ""},
    {LEON_BODY, 2547790784u, 0, ""},
    {LEON_BODY, 2657389788u, 0, ""},
    {LEON_BODY, 2685171658u, 0, ""},
    {LEON_BODY, 2730039539u, 0, ""},
    {LEON_BODY, 2759017705u, 0, ""},
    {LEON_BODY, 2768132486u, 0, ""},
    {LEON_BODY, 2778708114u, 0, ""},
    {LEON_BODY, 2793588356u, 0, ""},
    {LEON_BODY, 2801833761u, 0, ""},
    {LEON_BODY, 2879503442u, 0, ""},
    {LEON_BODY, 2890816445u, 0, ""},
    {LEON_BODY, 2939430305u, 0, ""},
    {LEON_BODY, 2980465111u, 0, ""},
    {LEON_BODY, 3041768941u, 0, ""},
    {LEON_BODY, 3205332075u, 0, ""},
    {LEON_BODY, 3373510315u, 0, ""},
    {LEON_BODY, 3374482223u, 0, ""},
    {LEON_BODY, 3375274666u, 0, ""},
    {LEON_BODY, 3395349193u, 0, ""},
    {LEON_BODY, 3575801630u, 0, ""},
    {LEON_BODY, 3586735098u, 0, ""},
    {LEON_BODY, 3741739367u, 0, ""},
    {LEON_BODY, 3778512958u, 0, ""},
    {LEON_BODY, 3802682741u, 0, ""},
    {LEON_BODY, 3832678906u, 0, ""},
    {LEON_BODY, 3878582777u, 0, ""},
    {LEON_BODY, 3926931870u, 0, ""},
    {LEON_BODY, 3947958744u, 0, ""},
    {LEON_BODY, 0u, 1, "fuck_you"},
    {LEON_BODY, 0u, 2, "fuck_you"},
    {LEON_BODY, 0u, 3, "fuck_you"},
    {LEON_BODY, 0u, 5, "fuck_you"},
    {LEON_BODY, 0u, 6, "fuck_you"},
    {LEON_BODY, 0u, 9, "fuck_you"},
    {LEON_BODY, 0u, 10, "fuck_you"},
    {LEON_BODY, 0u, 11, "fuck_you"},
    {LEON_BODY, 0u, 12, "fuck_you"},
    {LEON_BODY, 0u, 13, "fuck_you"},
    {LEON_BODY, 0u, 15, "fuck_you"},
    {LEON_BODY, 0u, 16, "fuck_you"},
    {LEON_BODY, 0u, 17, "fuck_you"},
    {LEON_BODY, 0u, 18, "fuck_you"},
    {LEON_BODY, 0u, 19, "fuck_you"},
    {LEON_BODY, 0u, 20, "fuck_you"},
    {LEON_BODY, 0u, 21, "point"},
    {LEON_BODY, 0u, 22, "point"},
    {LEON_BODY, 0u, 23, "point"},
    {LEON_BODY, 0u, 24, "point"},
    {LEON_BODY, 0u, 25, "point"},
    {LEON_BODY, 0u, 26, "point"},
    {LEON_BODY, 0u, 27, "point"},
    // --- Ada (DLC) -- TAUNT28..34 Mittelfinger, TAUNT35..41 Zeigefinger ---
    {ADA_BODY, 456664512u, 0, ""},
    {ADA_BODY, 681379490u, 0, ""},
    {ADA_BODY, 716155673u, 0, ""},
    {ADA_BODY, 754706280u, 0, ""},
    {ADA_BODY, 787060136u, 0, ""},
    {ADA_BODY, 928625746u, 0, ""},
    {ADA_BODY, 1172029601u, 0, ""},
    {ADA_BODY, 1257805707u, 0, ""},
    {ADA_BODY, 1306573981u, 0, ""},
    {ADA_BODY, 1327025640u, 0, ""},
    {ADA_BODY, 1355644925u, 0, ""},
    {ADA_BODY, 1931147534u, 0, ""},
    {ADA_BODY, 2003113980u, 0, ""},
    {ADA_BODY, 2049405374u, 0, ""},
    {ADA_BODY, 2101448527u, 0, ""},
    {ADA_BODY, 2323254526u, 0, ""},
    {ADA_BODY, 2396848626u, 0, ""},
    {ADA_BODY, 2588121064u, 0, ""},
    {ADA_BODY, 2965559617u, 0, ""},
    {ADA_BODY, 3077554779u, 0, ""},
    {ADA_BODY, 3140459462u, 0, ""},
    {ADA_BODY, 3141318796u, 0, ""},
    {ADA_BODY, 3235683830u, 0, ""},
    {ADA_BODY, 3902777018u, 0, ""},
    {ADA_BODY, 0u, 28, "fuck_you"},
    {ADA_BODY, 0u, 29, "fuck_you"},
    {ADA_BODY, 0u, 30, "fuck_you"},
    {ADA_BODY, 0u, 31, "fuck_you"},
    {ADA_BODY, 0u, 32, "fuck_you"},
    {ADA_BODY, 0u, 33, "fuck_you"},
    {ADA_BODY, 0u, 34, "fuck_you"},
    {ADA_BODY, 0u, 35, "point"},
    {ADA_BODY, 0u, 36, "point"},
    {ADA_BODY, 0u, 37, "point"},
    {ADA_BODY, 0u, 38, "point"},
    {ADA_BODY, 0u, 39, "point"},
    {ADA_BODY, 0u, 40, "point"},
    {ADA_BODY, 0u, 41, "point"},
};

constexpr float LERP_IN = 0.18f;
constexpr float HOLD = 2.50f;
constexpr float LERP_OUT = 0.30f;

constexpr bool STAGGER_ENABLED = true;
constexpr double STAGGER_HOLD = 0.50;   // s, ab wann aus "gehalten" ein Stagger wird
constexpr float STAGGER_REACH = 30.0f;  // m, Wirkradius (rundum, kein Zielen)
constexpr int STAGGER_MAX = 16;         // Sicherheitsdeckel pro Ausloesung
constexpr int32_t FLASH_KEYHASH = 2056866634;

constexpr bool TAUNT_ENABLED = true;    // fest an; KEINE UI, kein Toggle (04.08.)

struct FingerDef {
    const char* key;
    const char* label;
};

const std::array<FingerDef, 4> FINGERS{{
    {"R_Index", "Zeigefinger"},
    {"R_Middle", "Mittelfinger"},
    {"R_Ring", "Ringfinger"},
    {"R_Pinky", "Kleiner Finger"},
}};

const std::array<const char*, 2> POSE_ORDER{"point", "fuck_you"};

const char* pose_label(const std::string& name) {
    if (name == "point") {
        return "Zeigefinger-Pose (LT + R.A)";
    }

    if (name == "fuck_you") {
        return "Stinkefinger (LT + R.B)";
    }

    return name.c_str();
}

// R_Palm, dann je Finger F1..F3, dann die drei Daumenglieder.
std::vector<std::string> joint_names() {
    std::vector<std::string> t{"R_Palm"};

    for (const auto& f : FINGERS) {
        for (int i = 1; i <= 3; ++i) {
            t.push_back(std::string{f.key} + "F" + std::to_string(i));
        }
    }

    for (int i = 1; i <= 3; ++i) {
        t.push_back("R_Thumb" + std::to_string(i));
    }

    return t;
}

const std::vector<std::string>& JOINTS() {
    static const auto j = joint_names();
    return j;
}

// [DAUMEN-ACHSEN 2026-07-21, "Fingerspitze dreht weg statt einzukruemmen"]
// Die vier Finger beugen um ihre lokale Z-Achse, der DAUMEN nicht: in den
// echten RE4-Captures (knifepose/flamehand/singleaction) liegt die Beugung bei
// R_Thumb1 auf X und bei R_Thumb2/R_Thumb3 auf Y. Positiv = zur Handflaeche
// einrollen. Index im Quaternion {w,x,y,z}; Default 4 = Z.
int axis_of(const std::string& jn) {
    if (jn == "R_Thumb1") {
        return 2;
    }

    if (jn == "R_Thumb2" || jn == "R_Thumb3") {
        return 3;
    }

    return 4;
}

float quat_component(const glm::quat& q, int idx) {
    switch (idx) {
    case 1:
        return q.w;
    case 2:
        return q.x;
    case 3:
        return q.y;
    default:
        return q.z;
    }
}

constexpr float PI_F = 3.14159265358979323846f;

float quat_to_deg(const glm::quat& q, const std::string& jn) {
    return std::atan2(quat_component(q, axis_of(jn)), q.w) * 2.0f * 180.0f / PI_F;
}

glm::quat deg_to_quat(float d, const std::string& jn) {
    const float r = d * PI_F / 180.0f * 0.5f;
    glm::quat q{std::cos(r), 0.0f, 0.0f, 0.0f};

    switch (axis_of(jn)) {
    case 2:
        q.x = std::sin(r);
        break;
    case 3:
        q.y = std::sin(r);
        break;
    default:
        q.z = std::sin(r);
        break;
    }

    return q;
}

// [FINGER-ROTATION 2026-07-21] Twist um die Finger-Laengsachse X. Wirkt auf das
// Grundglied -> Mittelglied und Spitze drehen automatisch mit, weil sie Kinder
// davon sind.
glm::quat twist_quat(float d) {
    const float r = d * PI_F / 180.0f * 0.5f;
    return glm::quat{std::cos(r), std::sin(r), 0.0f, 0.0f};
}

// Luas eigene qmul-Reihenfolge (w,x,y,z) -- glm::quat multipliziert identisch,
// aber der Aufruf bleibt explizit, damit die Reihenfolge sichtbar ist.
glm::quat qmul(const glm::quat& a, const glm::quat& b) {
    return glm::quat{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

// [GEMESSEN 2026-07-23] Die am Bomben-Collider gemessenen Flash-Werte.
// _AttackType = 15 (chainsaw.character.AttackType.Flash), Damage/Wince/Break/
// Stopping alle 0 -- also reine Blend-Reaktion ohne Schaden.
struct FlashField {
    const char* name;
    double value;
};

// [KEINE HANDGEZAEHLTE GROESSE] Eine zu grosse Zahl haengt ein wertinitialisiertes
// Element mit `name = nullptr` an -- set_field_num baut daraus ein
// std::string_view(nullptr), also strlen(0), also eine Access Violation mitten
// im Aufbau. Der Stagger feuerte dann NIE. Deshalb CTAD statt einer Zahl.
constexpr auto FLASH_ATTACK_DATA = std::array{
    FlashField{"_KeyNameHash", static_cast<double>(FLASH_KEYHASH)},
    FlashField{"_Damage", 0.0},
    FlashField{"_IsPartnerDamage", 1.0},
    FlashField{"_AttackType", 15.0},
    FlashField{"_AttackPower", 1.0},
    FlashField{"_DeadType", 1.0},
    FlashField{"_Priority", 0.0},
    FlashField{"_SortType", 1.0},
    FlashField{"_Option", 0.0},
    FlashField{"_IntervalTime", 0.0},
    FlashField{"_Enchant", 0.0},
    FlashField{"_IsThroughRestriction", 0.0},
    FlashField{"_ThroughNum", 1.0},
    FlashField{"_DirectionType", 2.0},
    FlashField{"_IsShieldingDecision", 1.0},
    FlashField{"_EnableBackFacingHits", 0.0},
    FlashField{"_ShieldingDecisioningType", 3.0},
    FlashField{"_JointNameHash", 2180083513.0},
    // [SOUND-CRASH-FIX 2026-07-24] war false -> jeder Flash-Hit an allen
    // Gegnern postete ein Wwise-Event; einer traf async einen freed Voice ->
    // c0000005 im Audio-Thread (AK::Monitor::PostCode). Stummschalten = kein
    // Post = kein Crash. Kostet nur den Stagger-Sound.
    FlashField{"_Mute", 1.0},
    FlashField{"_SoundTriggerId", 4294967295.0},
    FlashField{"_MuteEffect", 0.0},
    FlashField{"_CheckEffectCollision", 0.0},
    FlashField{"_IsEmitEffect", 0.0},
    FlashField{"_AxisType", 0.0},
};

// Die restlichen drei Felder stehen getrennt, weil sie in Lua hinter
// _AxisType kommen und pairs() ohnehin keine Reihenfolge garantiert.
constexpr auto FLASH_ATTACK_DATA2 = std::array{
    FlashField{"_EffectCheckInterval", 0.0},
    FlashField{"_CheckRigidBody", 0.0},
    FlashField{"_DefaultThroughNum", 1.0},
};

// [TAUNT-SKIP 2026-08-04] Diese Lines gehoeren zum Twirl und sollen NICHT als
// Geste-Spruch kommen. Die Sperre steht hier und nicht in der JSON: die
// schreibt der #soundplayer beim naechsten Mark-Klick komplett neu.
// Gilt NUR fuer Leons Pool -- es sind seine Wwise-Hashes.
const std::array<uint32_t, 3> TAUNT_SKIP{
    2778708114u,   // "looking good"
    3778512958u,   // "that was not easy"
    3878582777u,   // "not bad"
};

bool is_taunt_skip(uint32_t id) {
    for (uint32_t v : TAUNT_SKIP) {
        if (v == id) {
            return true;
        }
    }

    return false;
}
} // namespace

std::shared_ptr<RE4VRGuestures>& RE4VRGuestures::get() {
    static auto inst = std::make_shared<RE4VRGuestures>();
    return inst;
}

// ============================================================================
// Handles
// ============================================================================

void RE4VRGuestures::store(Handle& h, ::REManagedObject* o, bool unconditional) {
    if (h.obj == o) {
        return;
    }

    drop(h);

    if (o == nullptr) {
        return;
    }

    h.obj = o;
    h.reffed = false;

    if (!utility::re_managed_object::is_managed_object(o)) {
        return;
    }

    // Lua pinnt bedingungslos. Bei selbst erzeugten Objekten MUSS das auch hier
    // so sein -- sonst bleibt eines mit refcount 0 unverankert (Falle 1).
    if (unconditional || static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        h.reffed = true;
    }
}

void RE4VRGuestures::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

void RE4VRGuestures::ensure_types() {
    if (m_types_ready) {
        return;
    }

    m_types_ready = true;
    m_t_sndc = type_of("soundlib.SoundContainer");
}

// ============================================================================
// Posen
// ============================================================================

void RE4VRGuestures::rebuild(const std::string& name) {
    PoseBones b;

    for (const auto& jn : JOINTS()) {
        const auto it = m_deg[name].find(jn);
        b[jn] = deg_to_quat(it != m_deg[name].end() ? it->second : 0.0f, jn);
    }

    for (const auto& f : FINGERS) {
        const auto it = m_rot[name].find(f.key);
        const float r = it != m_rot[name].end() ? it->second : 0.0f;

        if (r != 0.0f) {
            const std::string base = std::string{f.key} + "F1";
            b[base] = qmul(b[base], twist_quat(r));
        }
    }

    m_poses[name] = b;
}

void RE4VRGuestures::load_poses() {
    const auto d = re4vr::json_load(JSON_PATH);
    const auto p = d.is_object() ? d.find("poses") : d.end();
    const bool have_p = d.is_object() && p != d.end() && p->is_object();

    m_deg.clear();
    m_rot.clear();
    m_poses.clear();

    for (const char* name : POSE_ORDER) {
        m_deg[name] = {};
        m_rot[name] = {};

        const auto entry = have_p ? p->find(name) : nlohmann::json::const_iterator{};
        const bool have_entry = have_p && entry != p->end() && entry->is_object();
        const auto src = have_entry ? entry->find("bones") : nlohmann::json::const_iterator{};
        const bool have_src = have_entry && src != entry->end() && src->is_object();

        for (const auto& jn : JOINTS()) {
            float deg = 0.0f;

            if (have_src) {
                const auto q = src->find(jn);

                // Reihenfolge in der Aufnahme: w, x, y, z.
                if (q != src->end() && q->is_array() && q->size() >= 4) {
                    const glm::quat qq{(*q)[0].get<float>(), (*q)[1].get<float>(),
                                       (*q)[2].get<float>(), (*q)[3].get<float>()};
                    deg = quat_to_deg(qq, jn);
                }
            }

            m_deg[name][jn] = deg;
        }

        // Die Drehung des Grundglieds steckt separat in "rot" -- so bleibt die
        // reine Beugung beim Zurueckladen sauber getrennt.
        for (const auto& f : FINGERS) {
            float r = 0.0f;

            if (have_entry) {
                const auto rt = entry->find("rot");

                if (rt != entry->end() && rt->is_object()) {
                    const auto v = rt->find(f.key);

                    // Luas tonumber nimmt auch numerische Strings.
                    if (v != rt->end()) {
                        if (v->is_number()) {
                            r = v->get<float>();
                        } else if (v->is_string()) {
                            try {
                                r = std::stof(v->get<std::string>());
                            } catch (...) {
                            }
                        }
                    }
                }
            }

            m_rot[name][f.key] = r;
        }

        rebuild(name);
    }

    m_loaded = true;
}

void RE4VRGuestures::save_poses() {
    if (!m_loaded) {
        return;
    }

    nlohmann::json out;
    out["version"] = 1;

    nlohmann::json poses = nlohmann::json::object();

    for (const char* name : POSE_ORDER) {
        nlohmann::json bones = nlohmann::json::object();

        // bones = REINE Beugung (ohne Finger-Drehung), rot = Drehung je Finger
        // -> beim Laden sauber trennbar.
        for (const auto& jn : JOINTS()) {
            const auto it = m_deg[name].find(jn);
            const glm::quat q = deg_to_quat(it != m_deg[name].end() ? it->second : 0.0f, jn);
            bones[jn] = nlohmann::json::array({q.w, q.x, q.y, q.z});
        }

        nlohmann::json rot = nlohmann::json::object();

        for (const auto& f : FINGERS) {
            const auto it = m_rot[name].find(f.key);
            rot[f.key] = it != m_rot[name].end() ? it->second : 0.0f;
        }

        nlohmann::json one;
        one["hand"] = "right";
        one["bones"] = bones;
        one["rot"] = rot;
        poses[name] = one;
    }

    out["poses"] = poses;

    re4vr::json_save(JSON_PATH, out);
}

// ============================================================================
// Ablauf
// ============================================================================

// [RECHTE HAND FREI 2026-07-21] Die Geste laeuft auf der RECHTEN Hand -- also
// zaehlt auch nur die. Frei ist sie, wenn gar keine Waffe equippt ist ODER das
// Messer in der LINKEN Hand steckt. Gleiche Regel wie im binding.
bool RE4VRGuestures::hands_free() {
    if (re4vr::lua_get_string("__re4_knife_hand") == "right") {
        return false;
    }

    if (re4vr::lua_get_tribool("__vr_bare_hands") == 1) {
        return true;
    }

    if (re4vr::lua_get_tribool("__re4_knife_equipped") == 1
        && re4vr::lua_get_string("__re4_knife_hand") == "left") {
        return true;
    }

    return false;
}

void RE4VRGuestures::start(const std::string& name) {
    if (!m_loaded) {
        load_poses();
    }

    const auto it = m_poses.find(name);

    if (it == m_poses.end()) {
        return;
    }

    Active a{};
    a.bones = &it->second;
    a.t0 = clock_now();
    m_active = a;
}

// Blend-Faktor aus der Zeit (rein / halten / raus). Kein Wert = fertig.
std::optional<float> RE4VRGuestures::blend_now() {
    if (!m_active.has_value()) {
        return std::nullopt;
    }

    const float e = static_cast<float>(clock_now() - m_active->t0);

    if (e >= (LERP_IN + HOLD + LERP_OUT)) {
        return std::nullopt;
    }

    if (e < LERP_IN) {
        return e / LERP_IN;
    }

    if (e < LERP_IN + HOLD) {
        return 1.0f;
    }

    const float f = 1.0f - ((e - LERP_IN - HOLD) / LERP_OUT);

    return f > 0.0f ? f : 0.0f;
}

// Der Joint-Writer aus reload.lua nlerpt current->target mit dem Blend -> Ein-
// und Ausblenden ergeben sich allein aus dem Faktor.
void RE4VRGuestures::apply() {
    if (!re4vr::lua_has_function("__re4_reload_apply_pose_bones")) {
        return;
    }

    // [VORSCHAU] Zum Einstellen am Desktop: haelt die Pose, solange die
    // Checkbox an ist. Bewusst OHNE bare-hands-Gate (sonst koennte man sie mit
    // Waffe in der Hand nicht sehen), aber nur im Gameplay.
    if (m_preview.has_value() && m_loaded
        && re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1) {
        const auto it = m_poses.find(*m_preview);

        if (it != m_poses.end()) {
            re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones", it->second, 1.0f);
            return;
        }
    }

    if (!m_active.has_value()) {
        return;
    }

    if (!hands_free() || re4vr::lua_get_tribool("__re4_frame_pure_gameplay") != 1) {
        m_active.reset();
        return;
    }

    const auto b = blend_now();

    if (!b.has_value()) {
        m_active.reset();
        return;
    }

    if (m_active->bones != nullptr) {
        re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones", *m_active->bones, *b);
    }
}

// ============================================================================
// [STAGGER 2026-07-23] Stinkefinger GEHALTEN -> alle Gegner ringsum staggern
// ============================================================================
// MECHANIK: HitController.requestAttack schlaegt den _KeyNameHash der
// AttackUserData in HC._UserData._AttackHitUserData._AttackDataList nach. Der
// Spieler-HitController hat diese Tabelle gar nicht -> Suche laeuft ins Leere
// -> keine Reaktion. Deshalb bauen wir eine eigene Tabelle mit EINEM Eintrag.
//
// ZWEI FALLEN, beide teuer bezahlt:
// 1) Selbst erzeugte Managed Objects sind nicht verankert -- ohne add_ref
//    raeumt der GC sie nach dem ersten Einsatz ab, der zweite greift in
//    freigegebenen Speicher -> Access Violation, die kein pcall faengt. Deshalb
//    wird jedes Objekt genau einmal gebaut und sofort festgenagelt.
// 2) Die Tabelle wird NUR fuer die Dauer des Bursts eingehaengt und danach
//    exakt der Vorzustand zurueckgeschrieben. Haengt dort schon eine fremde
//    Tabelle, fassen wir gar nichts an.

::REManagedObject* RE4VRGuestures::flash_ud() {
    if (m_made_ud.obj != nullptr) {
        return m_made_ud.obj;
    }

    auto* u = sdk::create_instance<::REManagedObject>("chainsaw.collision.AttackUserData", true);

    if (u == nullptr) {
        return nullptr;
    }

    set_field_num(u, "_AttackHitDataID", 2.0);
    set_field_num(u, "_KeyNameHash", static_cast<double>(FLASH_KEYHASH));
    set_field_num(u, "<AttackID>k__BackingField", 26.0);

    store(m_made_ud, u, true);   // Lua: pin(u)

    return m_made_ud.obj;
}

::REManagedObject* RE4VRGuestures::flash_table() {
    if (m_made_tbl.obj != nullptr) {
        return m_made_tbl.obj;
    }

    auto* ahu = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.AttackHitUserData", true);
    auto* ad = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.AttackHitUserData.AttackData", true);
    // create_managed_array will den RUNTIME-TYPE, nicht den Namen.
    auto* ad_rt = type_of("chainsaw.collision.AttackHitUserData.AttackData");
    auto* arr = ad_rt != nullptr ? sdk::VM::create_managed_array(ad_rt, 1) : nullptr;

    if (ahu == nullptr || ad == nullptr || arr == nullptr) {
        return nullptr;
    }

    // Falle 1: sofort festnageln, sonst raeumt der GC sie nach dem ersten
    // Einsatz ab. Lua pinnt ALLE DREI vor dem Befuellen -- auch ahu, damit es
    // im Fehlerpfad nicht unverankert zurueckbleibt. Das Array haengt nur ueber
    // _AttackDataList am ahu, die AttackData an gar nichts (das Array-Element
    // bleibt null, s.u.).
    store(m_made_tbl, ahu, true);
    store(m_made_ad, ad, true);
    store(m_made_arr, (::REManagedObject*)arr, true);

    for (const auto& f : FLASH_ATTACK_DATA) {
        set_field_num(ad, f.name, f.value);
    }

    for (const auto& f : FLASH_ATTACK_DATA2) {
        set_field_num(ad, f.name, f.value);
    }

    // [BELEGT 2026-07-23] `set_element` wirft hier (REFramework fuellt
    // Klassen-Arrays nicht so), das Array-Element bleibt null -- und GENAU SO
    // lief der erfolgreiche Test: entscheidend ist offenbar nur, dass am
    // Spieler-HC ueberhaupt ein _AttackHitUserData haengt; die Angriffswerte
    // zieht die Engine ueber _KeyNameHash/_AttackHitDataID der AttackUserData.
    // Der Fehlschlag darf den Bau deshalb NICHT abbrechen.
    try {
        arr->set_element(0, ad);
    } catch (...) {
    }

    re4vr::clear_vm_exception();

    if (!set_field_obj(ahu, "_AttackDataList", (::REManagedObject*)arr)) {
        // [1:1] Lua gibt hier nil zurueck und laesst alle drei gepinnt liegen;
        // der naechste Versuch baut neue. Nur die Tabellen-Referenz faellt weg,
        // die Anker bleiben.
        m_made_tbl.obj = nullptr;
        m_made_tbl.reffed = false;
        return nullptr;
    }

    return m_made_tbl.obj;
}

::REManagedObject* RE4VRGuestures::dmg_ud() {
    if (m_made_dmg.obj != nullptr) {
        return m_made_dmg.obj;
    }

    auto* d = sdk::create_instance<::REManagedObject>(
        "chainsaw.collision.DamageUserData", true);

    if (d == nullptr) {
        return nullptr;
    }

    store(m_made_dmg, d, true);   // Lua: pin(d)

    return m_made_dmg.obj;
}

// Erkennt die EIGENE Tabelle: genau ein Eintrag. Die Tabellen des Spiels haben
// viele (die Flash-Bombe trug 173). Wichtig nach einem Script-Reset -- dann
// haengt evtl. noch die Tabelle aus dem alten Lua-Zustand drin.
bool RE4VRGuestures::is_own_table(::REManagedObject* t) {
    if (t == nullptr) {
        return false;
    }

    auto* lst = re4vr::get_field_object(
        t, "_AttackDataList");

    if (lst == nullptr) {
        return false;
    }

    // [1:1] In Lua steht dort get_size() ODER get_Length(). Das `or` greift NUR
    // bei nil -- eine gemessene 0 bleibt 0. Wer bei 0 auf get_Length ausweicht,
    // haelt eine fremde Tabelle mit einem Eintrag faelschlich fuer die eigene
    // und ueberschreibt sie: genau das, was Falle 2 verhindern soll.
    // get_size scheitert nur, wenn es gar kein Array ist.
    if (is_system_array(lst)) {
        return re4vr::array_size(lst) == 1;
    }

    return opt_int(lst, "get_Length").value_or(0) == 1;
}

void RE4VRGuestures::fire_stagger() {
    if (!STAGGER_ENABLED) {
        return;
    }

    auto* cm = character_manager();
    auto* ctx = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
        : nullptr;
    auto* go = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
        : nullptr;
    auto* hm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.HitManager");
    auto* hc = (hm != nullptr && go != nullptr)
        ? re4vr::call_safe<::REManagedObject*>(hm, "getHitController", go)
        : nullptr;
    auto* tf = go != nullptr ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
                             : nullptr;

    glm::vec3 ppos{};

    if (hc == nullptr || tf == nullptr || !get_vec3(tf, "get_Position", ppos) || cm == nullptr) {
        return;
    }

    auto* ud = flash_ud();
    auto* tbl = flash_table();
    auto* dmg = dmg_ud();

    if (ud == nullptr || tbl == nullptr || dmg == nullptr) {
        return;
    }

    auto* hud = re4vr::get_field_object(hc, "_UserData");

    if (hud == nullptr) {
        return;
    }

    // Falle 2: fremde Tabellen bleiben unangetastet -- eine eigene (1 Eintrag)
    // darf ersetzt werden.
    auto* old = re4vr::get_field_object(
        hud, "_AttackHitUserData");

    if (old != nullptr && !is_own_table(old)) {
        return;
    }

    if (!set_field_obj(hud, "_AttackHitUserData", tbl)) {
        return;
    }

    auto* list = re4vr::call_safe<::REManagedObject*>(cm, "get_EnemyContextList");
    const int32_t count = list != nullptr ? opt_int(list, "get_Count").value_or(0) : 0;
    int hits = 0;

    for (int32_t i = 0; i < count; ++i) {
        if (hits >= STAGGER_MAX) {
            break;
        }

        auto* ectx = re4vr::call_safe<::REManagedObject*>(list, "get_Item", i);
        auto* hp = ectx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ectx, "get_HitPoint")
            : nullptr;

        if (hp == nullptr) {
            continue;
        }

        // Lua: `dead ~= true and (chp or 0) > 0`
        if (opt_bool(hp, "get_IsDead") == std::optional<bool>{true}) {
            continue;
        }

        // [TYP AUS DER TDB] Lua bekommt hier eine generische Zahl. Ist der
        // Getter System.Single, laese ein reines int32 das Float-Bitmuster --
        // dieselbe Absicherung wie in RE4VRChoke::is_live.
        {
            bool alive = false;

            if (const auto v = opt_int(hp, "get_CurrentHitPoint"); v.has_value()) {
                alive = *v > 0;
            } else {
                float fv = 0.0f;
                alive = re4vr::try_call<float>(hp, "get_CurrentHitPoint", fv) && fv > 0.0f;
            }

            if (!alive) {
                continue;
            }
        }

        glm::vec3 pos{};

        if (!get_vec3(ectx, "get_Position", pos)) {
            continue;
        }

        auto* ego = re4vr::call_safe<::REManagedObject*>(ectx, "get_BodyGameObject");

        if (ego == nullptr) {
            continue;
        }

        const glm::vec3 d = pos - ppos;

        if ((d.x * d.x + d.y * d.y + d.z * d.z) > (STAGGER_REACH * STAGGER_REACH)) {
            continue;
        }

        // [FIX 2026-07-23] Frueher wurde hier pauschal auf false zurueckgesetzt
        // -- der Dump zeigt aber, dass HitController von Haus aus
        // AttackEnable=true haben. Wir haben den Spieler-HitController also
        // dauerhaft entschaerft zurueckgelassen (er zaehlte weder als Angreifer
        // noch als Ziel). Jetzt: vorherigen Wert lesen und exakt den
        // wiederherstellen.
        const auto was = opt_bool(hc, "get_AttackEnable");

        call_pcall(hc, "set_AttackEnable", true);
        call_pcall(hc, "requestAttack", ego, ud, dmg);

        if (was != std::optional<bool>{true}) {
            call_pcall(hc, "set_AttackEnable", false);
        }

        ++hits;
    }

    // Vorzustand zurueck -- es bleibt nichts haengen. Laesst sich das Feld
    // nicht auf nil setzen, bleibt unsere (verankerte) Tabelle stehen; der
    // naechste Burst erkennt sie als eigene wieder.
    set_field_obj(hud, "_AttackHitUserData", nullptr);
}

// ============================================================================
// [TAUNT 2026-08-04] Zu JEDER Geste zusaetzlich ein zufaelliger Spruch
// ============================================================================
// PRO CHARAKTER EIN EIGENER POOL: beim Laden wird nach label gruppiert, zur
// Ausloesung der Name des aktuellen Body-GO gelesen und genau dessen Pool
// gezogen. Wer keinen Pool hat, bleibt still -- es wird NIE ein fremder Pool
// gespielt. Reihenfolge = Shuffle-Bag JE POOL.
//
// [SEPARIERT 13.09.2026] Der Beutel haengt zusaetzlich an der GESTE: Eintraege
// mit "gest": "fuck_you"/"point" landen nur im Beutel dieser Geste, alles ohne
// "gest" (also saemtliche Wwise-Sprueche) in beiden. So hat der Mittelfinger
// die WAVs 1..20 und der Zeigefinger die WAVs 21..27, ohne dass sich die
// Sprueche ueberschneiden. Der Tiergriff hat davon nichts -- der zieht in
// RE4VR.cpp seinen eigenen Beutel aus TAUNT01..15.

void RE4VRGuestures::taunt_load() {
    m_taunt_pools.clear();
    m_taunt_bags.clear();
    m_taunt_loaded = true;

    if (!m_taunt_seeded) {
        m_taunt_seeded = true;
        std::srand(static_cast<unsigned>(std::time(nullptr)
                                         + static_cast<long>(clock_now() * 1000.0)));
    }

    // [FEST IM FORK 17.09.2026] Aus TAUNT_TABLE statt aus der JSON -- die
    // Pruefungen von damals bleiben, damit ein Tippfehler in der Tabelle still
    // verworfen wird statt den falschen Sound zu spielen.
    for (const auto& r : TAUNT_TABLE) {
        int wav_index = -1;

        if (r.wav != 0) {
            if (r.wav < 1 || r.wav > re4vr::TAUNT_WAV_COUNT) {
                continue;
            }

            wav_index = r.wav - 1;
        } else if (r.id == 0) {
            continue;
        }

        const std::string lab = r.body;
        const std::string gest = r.gest;

        // Die Skip-Liste gilt NUR fuer Leons Pool -- und nur fuer Wwise-IDs,
        // eigene WAVs stehen nicht darin.
        if (wav_index < 0 && lab == LEON_BODY && is_taunt_skip(r.id)) {
            continue;
        }

        m_taunt_pools[lab].push_back(TauntEntry{wav_index < 0 ? r.id : 0u, wav_index, gest});
    }
}

std::optional<RE4VRGuestures::TauntEntry> RE4VRGuestures::taunt_next(const std::string& body,
                                                                    const std::string& gest) {
    if (!m_taunt_loaded) {
        taunt_load();
    }

    const auto p = m_taunt_pools.find(body);

    if (p == m_taunt_pools.end() || p->second.empty()) {
        return std::nullopt;
    }

    // [SEPARIERT 13.09.2026] Ein Beutel JE GESTE. Der Strich kann in keinem
    // Body-GO-Namen vorkommen, taugt also als Trenner.
    auto& bag = m_taunt_bags[body + "|" + gest];

    if (bag.empty()) {
        // Nur, was zu dieser Geste gehoert: ohne "gest" gilt ein Eintrag fuer
        // beide (so bleiben die Wwise-Sprueche in beiden Beuteln).
        bag.clear();

        for (const auto& e : p->second) {
            if (e.gest.empty() || e.gest == gest) {
                bag.push_back(e);
            }
        }

        if (bag.empty()) {
            return std::nullopt;
        }

        // Fisher-Yates, von hinten -- exakt wie in Lua.
        for (size_t i = bag.size(); i >= 2; --i) {
            const size_t j = static_cast<size_t>(std::rand() % static_cast<int>(i));
            std::swap(bag[i - 1], bag[j]);
        }
    }

    // Lua zieht mit table.remove(bag), also vom ENDE.
    const TauntEntry e = bag.back();
    bag.pop_back();

    return e;
}

void RE4VRGuestures::play_taunt(const std::string& gest) {
    ensure_types();

    if (!TAUNT_ENABLED || m_t_sndc == nullptr) {
        return;
    }

    auto* cm = character_manager();
    auto* ctx = cm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef")
        : nullptr;
    auto* go = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
        : nullptr;

    if (go == nullptr) {
        return;
    }

    const std::string body = managed_string_of(
        re4vr::call_safe<::REManagedObject*>(go, "get_Name"));

    if (body.empty()) {
        return;
    }

    // [SEPARIERT 13.09.2026] Gezogen wird aus dem Beutel DIESER Geste -- die
    // auf die jeweils andere Geste festgelegten Eintraege liegen gar nicht
    // erst darin. Das ersetzt das fruehere Weiterziehen beim Zeigefinger.
    const auto e = taunt_next(body, gest);

    if (!e.has_value()) {
        return;   // kein Pool -> still
    }

    // [TAUNT-WAV 2026-09-12] Eigenes eingebettetes WAV: laeuft ueber winmm und
    // braucht weder SoundContainer noch Wwise. Der Charakter-Pool gilt trotzdem
    // -- gezogen wurde oben schon aus dem Beutel DIESES Bodys.
    if (e->wav_index >= 0) {
        // [ADA LAUTER 17.09.2026] Nur in Adas Pool: ihr Zuschlag kommt auf den
        // allgemeinen Regler. Leon bleibt exakt wie bisher.
        if (body == ADA_BODY) {
            re4vr::play_taunt_wav_db(e->wav_index,
                                     re4vr::taunt_wav_gain_db() + re4vr::ada_wav_extra_db());
        } else {
            re4vr::play_taunt_wav(e->wav_index);
        }

        return;
    }

    // KEIN Komponenten-Cache: der Container wird pro Ausloesung frisch geholt
    // (haelt Savegame-Loads aus).
    auto* con = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)",
                                                     m_t_sndc);

    if (con == nullptr) {
        return;
    }

    call_pcall(con, "trigger(System.UInt32)", e->id);
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRGuestures::on_initialize() {
    // Posen werden erst geladen, wenn der Lua-State steht (json_load selbst
    // braucht ihn nicht, aber apply/hands_free schon).
    return Mod::on_initialize();
}

void RE4VRGuestures::on_lua_state_destroyed(sol::state& lua) {
    // re.on_script_reset: active, hold, preview, DEG, ROT, POSES auf nil;
    // taunt_pools auf nil und taunt_bags leer -- der Pool wird beim naechsten
    // Mal frisch aus TAUNT_TABLE gebaut.
    m_active.reset();
    m_hold.reset();
    m_preview.reset();
    m_deg.clear();
    m_rot.clear();
    m_poses.clear();
    m_loaded = false;
    m_taunt_pools.clear();
    m_taunt_bags.clear();
    m_taunt_loaded = false;
    // [1:1] In Lua faellt `taunt_seeded` beim Neuladen der Datei auf false und
    // math.randomseed laeuft erneut -- die Spruchreihenfolge faengt neu an.
    m_taunt_seeded = false;

    // [1:1] Die gebauten Objekte bleiben stehen. In Lua ueberlebt `made` den
    // Reset zwar NICHT -- aber die dort verankerten Objekte bleiben im Speicher
    // (add_ref ohne Gegenstueck), und der naechste Burst baut neue. Nativ
    // dieselben weiterzubenutzen ist sparsamer und aendert nichts am Verhalten:
    // is_own_table erkennt die alte Tabelle ohnehin als eigene wieder.
}

void RE4VRGuestures::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    // Zuendung einsammeln (binding setzt den Namen genau einmal pro
    // Tastendruck).
    // [1:1] Lua holt das Signal bei JEDEM Wert ab, der nicht nil ist -- auch bei
    // "" oder 0. binding schreibt zwar ausschliesslich "point"/"fuck_you", aber
    // ein Wert, den wir stehen liessen, wuerde die Geste beim naechsten Frame
    // erneut ausloesen.
    const int fire_set = re4vr::lua_get_tribool("__re4_gesture_fire");
    const std::string fire = re4vr::lua_get_string("__re4_gesture_fire");

    if (fire_set >= 0 || !fire.empty()) {
        re4vr::lua_set_nil("__re4_gesture_fire");

        if (hands_free() && re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1) {
            start(fire);

            // [TAUNT 2026-08-04] beide Gesten -- jede mit ihrem eigenen Beutel
            // und ihren eigenen WAVs (Ansage 13.09.2026). `fire` ist
            // "fuck_you" oder "point".
            play_taunt(fire);

            // [ASHLEY ANTWORTET 16.09.2026] Galt die Geste IHR, sagt sie etwas
            // dazu -- Zeigefinger und Stinkefinger haben getrennte Poels. Ob sie
            // gemeint war, entscheidet der Choke-Mod (dort sitzt die Zielsuche
            // nach Ashley und ihre Mundbewegung). Hier steht bewusst KEINE
            // weitere Bedingung: Haende frei und reines Gameplay sind oben
            // schon geprueft.
            if (auto choke = RE4VRChoke::get(); choke != nullptr) {
                choke->ashley_reply(fire == "fuck_you");
            }
        }
    }

    // [STAGGER-HALTEN 2026-07-23] Reines Gameplay hat NICHT Vorrang, sondern
    // ist Vorbedingung: sobald wir im Menue/Killswitch/Boot/Fernglas sind (oder
    // die Haende nicht mehr frei), wird der Haltezustand VERWORFEN statt
    // weiterzulaufen -- ein danach erst reifender Halter zuendet also nie.
    const bool gate = (re4vr::lua_get_tribool("__re4_frame_pure_gameplay") == 1)
        && hands_free();
    const std::string g = gate ? re4vr::lua_get_string("__re4_gest_prev") : std::string{};

    if (g != "fuck_you") {
        m_hold.reset();
        return;
    }

    if (!m_hold.has_value()) {
        Hold h{};
        h.t0 = clock_now();
        h.fired = false;
        m_hold = h;
    }

    if (!m_hold->fired && (clock_now() - m_hold->t0) >= STAGGER_HOLD) {
        // genau EINMAL pro Halten; erst Loslassen macht wieder scharf
        m_hold->fired = true;

        try {
            fire_stagger();
        } catch (...) {
        }
    }
}

// Gleiche 4 Stufen wie arm_chain/motion -> die Pose ueberlebt die nativen
// Anim-Passes.
void RE4VRGuestures::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LockScene"_fnv || hash == "BeginRendering"_fnv) {
        apply();
    }
}

void RE4VRGuestures::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv || hash == "UpdateJointExpression"_fnv) {
        apply();
    }
}

// ============================================================================
// UI (Desktop): Fingerkruemmung je Pose einstellen. Grad = Beugung um die
// lokale Achse, 0 = gestreckt, positiv = eingerollt. Jede Aenderung wird sofort
// gespeichert.
// ============================================================================

void RE4VRGuestures::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Guestures")) {
        return;
    }

    if (!m_loaded) {
        load_poses();
    }

    ImGui::Text("Nur mit LEEREN Haenden im Gameplay: L.Trigger halten + R.A / R.B.");
    ImGui::Text("0 Grad = Finger gestreckt, groesser = staerker eingerollt.");
    ImGui::Spacing();

    // [BEREICH 2026-07-21] Der Daumen braucht mehr Gegenrichtung (Spitze bis
    // -90) als die Finger.
    const auto dslider = [&](const std::string& name, const std::string& jn,
                             const std::string& label) {
        float cur = 0.0f;
        const auto it = m_deg[name].find(jn);

        if (it != m_deg[name].end()) {
            cur = it->second;
        }

        const float lo = jn.find("Thumb") != std::string::npos ? -90.0f : -30.0f;

        if (ImGui::SliderFloat(label.c_str(), &cur, lo, 140.0f, "%.0f Grad")) {
            m_deg[name][jn] = cur;
            rebuild(name);
            save_poses();
        }
    };

    for (const char* pname : POSE_ORDER) {
        const std::string name{pname};

        if (!ImGui::TreeNode(pose_label(name))) {
            continue;
        }

        bool on = m_preview.has_value() && *m_preview == name;

        if (ImGui::Checkbox("Vorschau: Pose halten (zum Einstellen)", &on)) {
            if (on) {
                m_preview = name;
            } else {
                m_preview.reset();
            }
        }

        ImGui::Spacing();

        for (const auto& f : FINGERS) {
            ImGui::Text("-- %s --", f.label);

            // [FINGER-ROTATION] EIN Wert fuer den ganzen Finger.
            float rc = 0.0f;
            const auto rit = m_rot[name].find(f.key);

            if (rit != m_rot[name].end()) {
                rc = rit->second;
            }

            const std::string rlabel = std::string{"  Drehung (ganzer Finger)##rot"} + name
                + f.key;

            if (ImGui::SliderFloat(rlabel.c_str(), &rc, -90.0f, 90.0f, "%.0f Grad")) {
                m_rot[name][f.key] = rc;
                rebuild(name);
                save_poses();
            }

            dslider(name, std::string{f.key} + "F1",
                    std::string{"  Grundglied##"} + name + f.key);
            dslider(name, std::string{f.key} + "F2",
                    std::string{"  Mittelglied##"} + name + f.key);
            dslider(name, std::string{f.key} + "F3",
                    std::string{"  Fingerspitze##"} + name + f.key);
        }

        ImGui::Text("-- Daumen --");
        dslider(name, "R_Thumb1", std::string{"  Grundglied##"} + name + "th");
        dslider(name, "R_Thumb2", std::string{"  Mittelglied##"} + name + "th");
        dslider(name, "R_Thumb3", std::string{"  Fingerspitze##"} + name + "th");

        ImGui::Text("-- Handflaeche --");
        dslider(name, "R_Palm", std::string{"  Palm##"} + name);

        ImGui::TreePop();
    }

    ImGui::TreePop();
}

#endif // RE4
