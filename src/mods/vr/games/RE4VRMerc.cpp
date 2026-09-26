// ============================================================================
// RE4VRMerc -- 1:1-Portierung von re4_vr_merc.lua. Siehe RE4VRMerc.hpp fuer die
// Bausteine und die Reihenfolge im Mod-Vektor (zwischen Choke und Motion).
//
// Spezifikation: I:\LUATRANS\PORT_MERC_SPEC.md
// ============================================================================
#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <sdk/RETransform.hpp>
#include <sdk/SceneManager.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../HookManager.hpp"
#include "../../../REFramework.hpp"
#include "../../VR.hpp"

#include "RE4VRMerc.hpp"

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

// [BOW_KEEP] Zustand des Hooks. Datei-statisch, weil der Pre-/Post-Hook ein
// zustandsloses Lambda sein muss.
bool s_bow_keep_void = true;
bool s_bow_keep_skipped = false;

// [ABGR] imgui.text_colored liest die Farbe als ABGR -- das UNTERSTE Byte ist
// Rot. 0xFF66CCFF ist also (1.0, 0.8, 0.4) = orange, nicht hellblau.
ImVec4 col_abgr(uint32_t c) {
    return ImVec4{static_cast<float>(c & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 24) & 0xFF) / 255.0f};
}

// Ist das ein echtes System.Array? Gebraucht, bevor ein Array-Zugriff auf ein
// Feld losgelassen wird, das auch ein gewoehnliches Objekt sein kann.
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

// Ein Feld direkt schreiben -- Luas `obj:set_field("_IsDraw", true)`.
// utility::re_managed_object kennt nur das Lesen; der Offset kommt aus der TDB.
// [CONTAINER-FLAG] get_data_raw(obj, is_value_type) -- das zweite Argument
// beschreibt den CONTAINER, nicht den Feldtyp: bei einem Managed Object immer
// false.
template <typename T>
bool set_field_value(::REManagedObject* obj, const char* name, T value) {
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

    *reinterpret_cast<T*>(p) = value;
    return true;
}

// pcall-Aequivalent fuer void-Aufrufe.
//
// [FEIN, ABER WICHTIG] Luas `pcall(function() o:call("x", ...) end)` schlaegt
// NUR bei einer echten Engine-Exception fehl. Eine FEHLENDE Methode liefert
// dort nil und laesst pcall erfolgreich sein -- das darf die Selbstheilung
// ("Cache verwerfen") also nicht ausloesen. Ein Aufruf auf einem
// FREIGEGEBENEN Objekt wirft dagegen sehr wohl, und genau den soll sie fangen.
template <typename... Args>
bool call_pcall(::REManagedObject* obj, std::string_view name, Args... args) {
    // Leiche -> wie ein Wurf in Lua.
    if (!re4vr::obj_ok(obj)) {
        return false;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);
    const auto m = td != nullptr ? td->get_method(name) : nullptr;

    // Methode fehlt -> in Lua kein Fehler.
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

// [LUA-WAHRHEIT] `== false` -- ein fehlgeschlagener Aufruf liefert nil und
// gilt als GUELTIG.
bool valid_not_false(::REManagedObject* obj) {
    if (obj == nullptr) {
        return false;
    }

    bool v = false;

    if (!re4vr::try_call<bool>(obj, "get_Valid", v)) {
        return true;
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

template <typename... Args>
std::optional<int32_t> opt_int(::REManagedObject* obj, std::string_view name, Args... args) {
    int32_t v = 0;

    if (!re4vr::try_call<int32_t>(obj, name, v, args...)) {
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

// [NIL-TYPE-GUARD] getComponent NIE mit nil-Type -- das flutet das Log mit
// Engine-Exceptions (via.render.SkinnedMesh existiert in RE4 gar nicht).
::REManagedObject* get_component(::REManagedObject* go, ::REManagedObject* t) {
    if (go == nullptr || t == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", t);
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

// [FRAME-CACHE] Ueber re4vr::fc -- der Singleton-Lookup ist der teuerste Teil
// der Spieler-Kette und lief bisher bei JEDEM Aufruf neu (in apply_hold & Co.
// fuenfmal pro Frame). Verhalten unveraendert: der Cache haelt nichts ueber den
// Frame hinaus und laesst sich mit __re4_fc_off abschalten.
::REManagedObject* character_manager() {
    return re4vr::fc::managed_singleton("chainsaw.CharacterManager");
}

constexpr float PI_F = 3.14159265358979323846f;

float deg2rad(float d) {
    return d * PI_F / 180.0f;
}

glm::quat q_axis(float deg, float x, float y, float z) {
    const float h = deg2rad(deg) * 0.5f;
    const float s = std::sin(h);

    return glm::quat{std::cos(h), x * s, y * s, z * s};
}

// Achsenreihenfolge identisch zu re4_vr_motion.lua (Y * X * Z), damit sich die
// Slider genauso anfuehlen wie die uebrigen Rot-Slider im Projekt.
glm::quat quat_from_euler_deg(float px, float py, float pz) {
    const glm::quat q = q_axis(py, 0.0f, 1.0f, 0.0f) * q_axis(px, 1.0f, 0.0f, 0.0f)
        * q_axis(pz, 0.0f, 0.0f, 1.0f);

    return glm::normalize(q);
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_merc.json";
constexpr int CHECK_EVERY = 30;
constexpr int32_t BOW_WID = 6304;
constexpr const char* BOW_PIN_JOINT = "R_Hand";
constexpr double BR_MAX_SEC = 60.0;

// ---------------------------------------------------------------------------
// Material-Listen. Alle LIVE gedumpt (02.08.2026 und spaeter), Zeichen fuer
// Zeichen wie im Original.
// ---------------------------------------------------------------------------

// Leon -- identisch mit HIDE_MATERIALS_LEON aus re4_vr_materials.lua. Es
// scheiterte also NUR am Body-Namen, nie an der Logik.
const std::unordered_set<std::string> HIDE_LEON{
    // GO 'head'
    "EyeAO_mat", "EyeOut_mat", "Face_mat", "EyeWet_mat", "BrowsEyeLashes_mat",
    "Eye_inside_mat", "Mouth_mat",
    // GO 'hair'
    "Hair00_Mat", "Hair01_Mat",
    // [PINSTRIPE-HAAR 2026-08-05] eigene Haar-Materialien des Kostuems
    "pl0074_Hair_Mat", "pl0074_Hair2_Mat",
};

// Luis: eigenes 'head' (MIT Beard_Mat) und 'hair' (nur Hair_A_Mat). Beide
// werden schon ueber den GO-Namen erfasst -- die Liste ist Rueckfallebene.
const std::unordered_set<std::string> HIDE_LUIS{
    "Beard_Mat", "BrowsEyelashes_mat", "EyeAO_mat", "EyeInside_mat", "EyeOut_mat",
    "EyeWet_mat", "Face_mat", "Mouth_mat", "Hair_A_Mat",
};

// Krauser: DER ERSTE, bei dem die GO-Namen NICHT 'head'/'hair' heissen --
// Kopf 'chi200_10', Haare 'chb700_21'. Fuer ihn traegt die Materialliste die
// Erkennung.
const std::unordered_set<std::string> HIDE_KRAUSER{
    // Kopf (GO 'chi200_10')
    "EyeAO_mat", "Eye_out_mat", "Face_mat", "Mouth_mat", "Blow_mat", "Eye_in_mat",
    "Eyewet_mat", "Eye_Lashes_mat",
    // Haare (GO 'chb700_21')
    "Hair1_Mat", "Hair2_Mat", "Hair3_Mat", "Hair4_Mat",
};

// HUNK: hat weder 'head' noch 'hair' -- sein Kopf ist die GASMASKE im GO
// 'chi300_10'. Face_Mat/Body_Mat sind generisch, deshalb greift die Liste nur
// ueber Hunks KindID.
const std::unordered_set<std::string> HIDE_HUNK{
    "Face_Mat", "Body_Mat", "Glass_out_Mat", "Glass_in_Mat",
};

// Ada: Kopf 'cha200_10' (inkl. Lens_Inside_mat), Haare 'cha200_20'.
// Achtung: kleines "mat" am Ende der Haar-Materialien, anders als bei Leon.
const std::unordered_set<std::string> HIDE_ADA{
    "Ao_mat", "Blow_mat", "EyeLash_mat", "Eye_in_mat", "Eye_out_mat", "Eyewet_mat",
    "Face_mat", "Mouth_mat", "Lens_Inside_mat",
    "Hair00_mat", "Hair01_mat", "Hair02_mat",
};

// Wesker: Kopf 'cha600_10' -- die Sonnenbrille steckt MIT drin, geht also ohne
// Extrawurst weg. Haare 'cha600_20'.
const std::unordered_set<std::string> HIDE_WESKER{
    "Ao_mat", "Blow_mat", "Eye_in_mat", "Eye_out_mat", "Eyewet_mat", "Face_mat",
    "Mouth_mat", "GlassLens_mat", "Glass_mat", "NosePad_Mat", "Emi_Glass_mat",
    "Hair_A_Mat", "Hair_B_Mat", "Hair_C_Mat", "Hair_D_Mat", "Hair_E_Mat",
};

// Kopfbedeckungen u.ae., die NICHT in einem eigenen GO liegen, sondern als
// Submaterial im grossen 'body'-Mesh haengen. Die gehen nur per-Material weg --
// mesh-weit wuerde der ganze Koerper verschwinden.
const std::unordered_set<std::string> HIDE_MATS_EXTRA{
    "Hat00_Mat",   // Leon, Pinstripe-Kostuem
    "Hat01_Mat",   // Leon, Pinstripe-Kostuem
    "Beret_Mat",   // Krauser, Baskenmuetze (im 47er Body-Mesh chi200_00)
    "pl0074_Hair_Mat", "pl0074_Hair2_Mat",
};

// [BARETT 2026-08-21, in EMV live nachgemessen] Krausers rotes Barett heisst
// NICHT 'Beret_Mat' -- das sichtbare Barett ist 'Boa_Mat' (bisher faelschlich
// als "Fellkragen" gefuehrt), das Band daran 'Ribbon_Mat'.
// BEWUSST NUR BEI KRAUSER: beides sind generische Namen.
const std::unordered_set<std::string> HIDE_MATS_EXTRA_KRAUSER{
    "Boa_Mat", "Ribbon_Mat",
    // [2026-09-09, in EMV gefunden] Auf chi200_00, hinter den Handschuhen.
    // Stand in keiner Liste.
    "Crest_Mat",
};

// [SZENENWEG 2026-09-09] -----------------------------------------------------
// Der Transform-Baum ist die FALSCHE Quelle. collect_hh laeuft ihn ab, findet
// dort z.B. Krausers chi200_10 und setzt es auf DrawDefault=false -- eine Sonde
// liest danach sauber "aus". Sichtbar bleiben Augen und Haare trotzdem, und in
// EMV stehen die Haken gesetzt. EMV sucht ueber die SZENE und schaltet PRO
// MATERIAL. Genau dieser Unterschied steht im Code schon bei scan_extra_mats:
// "EMV findet seine Meshes NICHT ueber den Transform-Baum, sondern ueber die
// SZENE". Beim Barett war es derselbe Fehler.
//
// Live durchgetestet am 09.09.2026 ueber ein Lua-Testscript: Luis 9/9,
// Krauser inkl. des vorher unbekannten Crest_Mat, die uebrigen ebenfalls.
//
// DREI SPERREN, jede fuer sich ausreichend -- Krauser und Luis kommen auch in
// Leons Kampagne vor, dort darf davon NICHTS greifen:
//   1. __re4_in_mercs  (verlangt zusaetzlich einen der sechs Mercs-Bodys;
//      Separate Ways faellt darueber raus, Mercs-Ada heisst ch3a8z0_MC_body)
//   2. __re4_merc_kind (der GESPIELTE Charakter, nie die Gegnerliste --
//      Kampagnen-Krauser ist KindID 200011)
//   3. Abstammung: das GO muss unter dem Koerper des Spielers haengen. Daran
//      scheitert jedes NPC-Modell, auch bei gleichem Namen.
struct SceneHide {
    int32_t kind;
    const std::unordered_set<std::string>* gos;
    const std::unordered_set<std::string>* mats;
};

const std::unordered_set<std::string> SCENE_GOS_LEON{"head", "hair"};
const std::unordered_set<std::string> SCENE_MATS_LEON{
    "EyeAO_mat", "EyeOut_mat", "Face_mat", "EyeWet_mat", "BrowsEyeLashes_mat",
    "Eye_inside_mat", "Mouth_mat", "Hair00_Mat", "Hair01_Mat",
    "pl0074_Hair_Mat", "pl0074_Hair2_Mat", "Hat00_Mat", "Hat01_Mat",
};

const std::unordered_set<std::string> SCENE_GOS_LUIS{"head", "hair"};
const std::unordered_set<std::string> SCENE_MATS_LUIS{
    "Beard_Mat", "BrowsEyelashes_mat", "EyeAO_mat", "EyeInside_mat", "EyeOut_mat",
    "EyeWet_mat", "Face_mat", "Mouth_mat", "Hair_A_Mat",
};

// EyeAO_mat und Mouth_mat stehen hier BEWUSST nicht drin: auf dem EMV-Bild
// waren sie nicht als gehakt zu erkennen, und der Testlauf war ohne sie sauber.
const std::unordered_set<std::string> SCENE_GOS_KRAUSER{
    "chi200_10", "chb700_21", "chi200_00",
};
const std::unordered_set<std::string> SCENE_MATS_KRAUSER{
    "Eye_out_mat", "Face_mat", "Blow_mat", "Eye_in_mat", "Eyewet_mat",
    "Eye_Lashes_mat",
    "Hair1_Mat", "Hair2_Mat", "Hair3_Mat", "Hair4_Mat",
    "Crest_Mat",
};

const std::unordered_set<std::string> SCENE_GOS_HUNK{"chi300_10"};
const std::unordered_set<std::string> SCENE_MATS_HUNK{
    "Face_Mat", "Body_Mat", "Glass_out_Mat", "Glass_in_Mat",
};

const std::unordered_set<std::string> SCENE_GOS_ADA{"cha200_10", "cha200_20"};
const std::unordered_set<std::string> SCENE_MATS_ADA{
    "Ao_mat", "Blow_mat", "EyeLash_mat", "Eye_in_mat", "Eye_out_mat", "Eyewet_mat",
    "Face_mat", "Mouth_mat", "Lens_Inside_mat",
    "Hair00_mat", "Hair01_mat", "Hair02_mat",
};

const std::unordered_set<std::string> SCENE_GOS_WESKER{"cha600_10", "cha600_20"};
const std::unordered_set<std::string> SCENE_MATS_WESKER{
    "Ao_mat", "Blow_mat", "Eye_in_mat", "Eye_out_mat", "Eyewet_mat", "Face_mat",
    "Mouth_mat", "GlassLens_mat", "Glass_mat", "NosePad_Mat", "Emi_Glass_mat",
    "Hair_A_Mat", "Hair_B_Mat", "Hair_C_Mat", "Hair_D_Mat", "Hair_E_Mat",
};

const std::array<SceneHide, 6> SCENE_HIDE{{
    {600000, &SCENE_GOS_LEON,    &SCENE_MATS_LEON},
    {600001, &SCENE_GOS_LUIS,    &SCENE_MATS_LUIS},
    {600002, &SCENE_GOS_KRAUSER, &SCENE_MATS_KRAUSER},
    {600003, &SCENE_GOS_HUNK,    &SCENE_MATS_HUNK},
    {380000, &SCENE_GOS_ADA,     &SCENE_MATS_ADA},
    {600005, &SCENE_GOS_WESKER,  &SCENE_MATS_WESKER},
}};

const SceneHide* scene_hide_for(int32_t kind) {
    for (const auto& e : SCENE_HIDE) {
        if (e.kind == kind) {
            return &e;
        }
    }

    return nullptr;
}

// [EXTRA_MATS 2026-08-05] Materialien, die im Voll-Aus NICHT auf DrawDefault
// hoeren -- gesucht wird SZENENWEIT (s. Kampagne-Sperre in apply_full_hide).
const std::unordered_set<std::string> FULLHIDE_EXTRA_MATS{
    "JacketFur_Mat",   // Leon, Jacken-Kostuem (Fell am Kragen)
    "Jacket_Mat",      // Leon, Jacke
    "Boa_Mat",         // Krauser, sichtbares Barett (chi200_00)
    // [BAND 2026-09-09] Gehoert zum Barett und wurde im Szenen-Scan bisher
    // nicht mitgesucht -- ohne das bleibt beim Ausblenden das Band stehen.
    "Ribbon_Mat",
};

// [RAGE-BARETT 2026-09-09] Im Ragemodus wird NUR dieser Ausschnitt der Liste
// oben geschaltet. 'Jacket_Mat'/'JacketFur_Mat' sind generische Namen und
// wuerden szenenweit 21 Sekunden lang auch fremde Meshes treffen -- im
// Voll-Aus (ks3/ks5) ist das hinnehmbar, im laufenden Gameplay nicht.
const std::unordered_set<std::string> RAGE_SCENE_MATS_KRAUSER{
    "Boa_Mat", "Ribbon_Mat",
};

// GO-Namen, die als Kopf/Haar gelten (Hauptweg, kostuemfest).
const std::unordered_set<std::string> HH_GO_NAMES{"head", "hair"};

// [NUR_WESKER 2026-08-05] Leons Mercs-Superpower ist eine andere.
// [KRAUSER DAZU 2026-09-09 -- gemessen, nicht vermutet] Krausers HeadUpdater ist
// ein EIGENER Typ (chainsaw.Ch6i2z0HeadUpdater), er fuehrt aber dieselben
// Felder. Sonde re4_krauser_power.txt, Rage von 369.94 s bis 390.97 s (21,0 s):
//   get_PlayingBulletRush  false -> true
//   <_BulletRushState>     0 -> 1 -> 3 -> 4   (Ende: 4 -> 5 -> 0)
//   _PlayingBulletRush     false -> true
//   _NowRushLv             0 -> 1
// Damit reicht der vorhandene Weg: KindID eintragen, den KS4 macht wie bei
// Wesker der Killswitch. 600002 = ch6i2z0_body = Krauser.
// [NUR DIESE ZWEI] Leon, Luis, HUNK und Ada bekommen ausdruecklich KEINEN
// KS4 -- ihre Mercs-Faehigkeit ist eine andere und bleibt normales Gameplay.
const std::array<int32_t, 2> BR_CHARS{600005, 600002};

// [WAFFENLISTE 2026-08-18 -- vom Tester vorgegeben] In Mercs haben GENAU DIESE
// DREI Waffen einen Laser. Liste schlaegt Heuristik.
const std::array<int32_t, 3> MERC_LASER_WIDS{6304, 4000, 4501};

// [DER TRENNER IST DER HALTER 2026-08-25] Drittperson-Griff: Halter L0 id=502
// bank=10. Weiterer Drittperson-Griff = weiterer Eintrag, mehr nicht.
struct GrabMotion {
    int layer;
    int32_t id;
    int32_t bank;
};

const std::array<GrabMotion, 1> GRAB_MOTIONS{{{0, 502, 10}}};
} // namespace

std::shared_ptr<RE4VRMerc>& RE4VRMerc::get() {
    static auto inst = std::make_shared<RE4VRMerc>();
    return inst;
}

// ---------------------------------------------------------------------------
// Die sechs Mercs-Charaktere. Mercs gibt JEDEM waehlbaren Charakter einen
// eigenen Body/KindID-Slot -- Leon heisst hier NICHT ch0a0z0_body, sondern
// ch6i0z0_body (KindID 600000). Genau deshalb greift das Head/Hair-Verstecken
// aus re4_vr_materials.lua im Mercs nicht: dessen Whitelist kennt nur ch0a0z0,
// ch0a1z0 und ch3a8z0.
// Ada faellt aus der Reihe: keine 6000xx-ID, sondern ihre Separate-Ways-KindID
// 380000 mit _MC-Suffix am Body. 600004 ist unbelegt -- Ada belegt den Platz.
//
// Die Tabelle steht in einer Member-Funktion, weil MercChar privat ist.
// ---------------------------------------------------------------------------
const std::array<RE4VRMerc::MercChar, 6>& RE4VRMerc::merc_chars() {
    static const std::array<MercChar, 6> tbl{{
        {600000, "ch6i0z0_body", "Leon (Mercs)", &HIDE_LEON, nullptr, "merc_leon"},
        {600001, "ch6i1z0_body", "Luis (Mercs)", &HIDE_LUIS, nullptr, "merc_luis"},
        {600002, "ch6i2z0_body", "Krauser (Mercs)", &HIDE_KRAUSER,
         &HIDE_MATS_EXTRA_KRAUSER, "merc_krauser"},
        {600003, "ch6i3z0_body", "HUNK (Mercs)", &HIDE_HUNK, nullptr, "merc_hunk"},
        {380000, "ch3a8z0_MC_body", "Ada (Mercs)", &HIDE_ADA, nullptr, "merc_ada"},
        {600005, "ch6i5z0_body", "Wesker (Mercs)", &HIDE_WESKER, nullptr, "merc_wesker"},
    }};

    return tbl;
}

const RE4VRMerc::MercChar* RE4VRMerc::merc_char(int32_t kind) {
    for (const auto& c : merc_chars()) {
        if (c.kind == kind) {
            return &c;
        }
    }

    return nullptr;
}

// [SW_VS_MERCS 2026-08-04] get_GuiManager allein erkennt Mercenaries NICHT
// zuverlaessig -- live gemessen, waehrend Ada im Separate-Ways-DLC lief:
// get_GuiManager lieferte einen Cp1021GuiManager und CampaignID stand auf
// Invalid (-1), also exakt das Mercs-Muster. Der Body-Name trennt beide sauber.
bool RE4VRMerc::is_merc_body(const std::string& name) {
    for (const auto& c : merc_chars()) {
        if (name == c.body) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Handles
// ============================================================================

void RE4VRMerc::store(Handle& h, ::REManagedObject* o) {
    if (h.obj == o) {
        return;
    }

    drop(h);

    if (o == nullptr) {
        return;
    }

    h.obj = o;
    h.reffed = false;

    if (utility::re_managed_object::is_managed_object(o)
        && static_cast<int32_t>(o->referenceCount) > 0) {
        utility::re_managed_object::add_ref(o);
        h.reffed = true;
    }
}

void RE4VRMerc::drop(Handle& h) {
    if (h.obj != nullptr && h.reffed) {
        utility::re_managed_object::release(h.obj);
    }

    h.obj = nullptr;
    h.reffed = false;
}

void RE4VRMerc::ensure_types() {
    if (m_types_ready) {
        return;
    }

    m_types_ready = true;

    // via.render.SkinnedMesh existiert in RE4 NICHT -> sdk.typeof waere nil und
    // jeder getComponent-Aufruf damit wirft eine Engine-Exception, die
    // REFramework SYNCHRON auf Platte loggt. via.render.Mesh deckt geskinnte
    // Meshes ohnehin ab.
    m_t_mesh = type_of("via.render.Mesh");
    // [FUR 2026-08-15] Das Fell am Kragen ist KEIN Material auf einem Mesh: es
    // haengt an via.render.Fur bzw. via.render.ShellFurMesh -- beide erben von
    // via.render.RenderEntity, NICHT von via.render.Mesh.
    m_t_fur = type_of("via.render.Fur");
    m_t_shellfur = type_of("via.render.ShellFurMesh");
    m_t_motion = type_of("via.motion.Motion");
    m_t_lsc = type_of("chainsaw.LaserSightController");
    m_t_gui = type_of("via.gui.GUI");
}

// ============================================================================
// (1) Erkennung
// ============================================================================
// Singleton-Getter MIT Cache-Verwurf: ein Cache, der einen Szenen-/Savewechsel
// ueberlebt, liefert still nil und friert den Zustand ein (genau der Bug, der
// den Anim-Export 2026-08-02 getoetet hat).

::REManagedObject* RE4VRMerc::get_merc_mgr() {
    if (m_merc_mgr.obj == nullptr) {
        store(m_merc_mgr,
              sdk::get_managed_singleton<::REManagedObject>("chainsaw.MercenariesManager"));
    }

    return m_merc_mgr.obj;
}

::REManagedObject* RE4VRMerc::get_campaign_mgr() {
    if (m_campaign_mgr.obj == nullptr) {
        store(m_campaign_mgr,
              sdk::get_managed_singleton<::REManagedObject>("chainsaw.CampaignManager"));
    }

    return m_campaign_mgr.obj;
}

::REManagedObject* RE4VRMerc::get_char_mgr() {
    if (m_char_mgr.obj == nullptr) {
        store(m_char_mgr, character_manager());
    }

    return m_char_mgr.obj;
}

// POSITIV: chainsaw.MercenariesManager.get_GuiManager liefert einen lebenden
// chainsaw.Cp1021GuiManager. In der Kampagne registriert die Mercs-Scene diesen
// GUI-Manager nie -> dort nil.
// STUETZE: chainsaw.CampaignManager.get_CurrentCampaign ist im Mercs 'Invalid'
// (-1); in Haupt- UND Ada-Kampagne 'Main' (0).
bool RE4VRMerc::detect(std::optional<int32_t>& cid_out) {
    auto* mm = get_merc_mgr();
    auto* gui = mm != nullptr
        ? re4vr::call_safe<::REManagedObject*>(mm, "get_GuiManager")
        : nullptr;

    if (mm != nullptr && gui == nullptr) {
        // Manager da, aber kein GUI: entweder Kampagne (richtig) oder der
        // Singleton ist tot -> einmal verwerfen, naechster Tick holt frisch.
        drop(m_merc_mgr);
    }

    auto* cm = get_campaign_mgr();
    cid_out.reset();

    if (cm != nullptr) {
        cid_out = opt_int(cm, "get_CurrentCampaign");

        if (!cid_out.has_value()) {
            drop(m_campaign_mgr);
        }
    }

    return gui != nullptr;
}

// Liefert KindID, Body-GO-Name und den Body-Transform.
// [KEIN-WERT-FALLE 2026-08-06] IMMER drei Werte -- ein blankes `return nil`
// lieferte in Lua nur EINEN, und `select(2, ...)` gar keinen.
RE4VRMerc::PlayerBody RE4VRMerc::get_player_body() {
    PlayerBody out{};

    auto* cmgr = get_char_mgr();
    auto* ctx = cmgr != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cmgr, "getPlayerContextRef")
        : nullptr;

    if (ctx == nullptr) {
        drop(m_char_mgr);
        return out;
    }

    out.kind = opt_int(ctx, "get_KindID");

    auto* go = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (go == nullptr) {
        return out;
    }

    out.name = obj_name_of(go);
    out.name_ok = !out.name.empty();
    out.tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    return out;
}

// ============================================================================
// (2) Kopf/Haare verstecken
// ============================================================================
// Vorgehen wie in re4_vr_materials.lua im [SHADOW]-Zweig: ein Mesh, das
// AUSSCHLIESSLICH aus Kopf/Haar-Materialien besteht, wird mesh-weit aus dem
// Farb-Pass genommen und wirft trotzdem Schatten -- besser als
// setMaterialsEnable(false), das den Schatten mitnimmt.
//
// ZWEI Wege, ein Mesh als Kopf/Haar zu erkennen:
// (1) GO-NAME ist 'head' oder 'hair' -> Hauptweg, kostuemfest. Noetig geworden
//     durch Leons Pinstripe-Kostuem: Body-Name und KindID bleiben gleich, aber
//     das Haar-Mesh traegt dann pl0074_Hair_Mat statt Hair00_Mat.
// (2) ALLE Materialien stehen in der Hide-Liste -> Zusatzsicherung fuer
//     Charaktere, deren Kopf-GO anders heisst (Krauser, HUNK, Ada, Wesker).
//
// In beiden Faellen nur REINE Kopf/Haar-Meshes. Ein gemischtes Mesh (z.B.
// 'body', das bei Pinstripe den Hut traegt) wird nie mesh-weit versteckt.

void RE4VRMerc::clear_hh() {
    for (auto& e : m_hh_meshes) {
        drop(e.mesh);
    }

    m_hh_meshes.clear();
    m_hh_set = false;
    m_hh_body.clear();
}

void RE4VRMerc::clear_all_meshes() {
    for (auto& h : m_all_meshes) {
        drop(h);
    }

    m_all_meshes.clear();
    m_all_set = false;
    m_full_hidden = false;
}

bool RE4VRMerc::collect_hh(::REManagedObject* tf,
                           const std::unordered_set<std::string>* hide,
                           const std::unordered_set<std::string>* extra) {
    ensure_types();
    clear_hh();

    if (tf == nullptr || m_t_mesh == nullptr) {
        return false;
    }

    const std::function<void(::REManagedObject*, int)> walk =
        [&](::REManagedObject* t, int depth) {
            if (t == nullptr || depth > 14) {
                return;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(t, "get_GameObject");
            auto* mesh = get_component(go, m_t_mesh);

            if (mesh != nullptr) {
                int32_t n = 0;

                if (re4vr::try_call<int32_t>(mesh, "get_MaterialNum", n) && n > 0) {
                    const std::string gname = obj_name_of(go);
                    bool hit = !gname.empty()
                        && HH_GO_NAMES.count(to_lower(gname)) > 0;

                    if (!hit && hide != nullptr) {
                        hit = true;

                        for (int32_t i = 0; i < n; ++i) {
                            auto* mn = re4vr::call_safe<::REManagedObject*>(
                                mesh, "getMaterialName", i);
                            const std::string s = managed_string_of(mn);

                            if (s.empty() || hide->count(s) == 0) {
                                hit = false;
                                break;
                            }
                        }
                    }

                    if (hit) {
                        HhEntry e{};
                        store(e.mesh, mesh);
                        e.name = gname;
                        m_hh_meshes.push_back(e);
                    } else {
                        // Gemischtes Mesh: einzelne Materialien (Hut) einsammeln
                        for (int32_t i = 0; i < n; ++i) {
                            auto* mn = re4vr::call_safe<::REManagedObject*>(
                                mesh, "getMaterialName", i);
                            const std::string s = managed_string_of(mn);

                            if (s.empty()) {
                                continue;
                            }

                            if (HIDE_MATS_EXTRA.count(s) > 0
                                || (extra != nullptr && extra->count(s) > 0)) {
                                HhEntry e{};
                                store(e.mesh, mesh);
                                e.idx = i;
                                e.name = gname + "/" + s;
                                m_hh_meshes.push_back(e);
                            }
                        }
                    }
                }
            }

            auto* c = re4vr::call_safe<::REManagedObject*>(t, "get_Child");

            while (c != nullptr) {
                walk(c, depth + 1);
                c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
            }
        };

    walk(tf, 0);

    m_hh_set = !m_hh_meshes.empty();

    if (!m_hh_set) {
        clear_hh();
    }

    return m_hh_set;
}

bool RE4VRMerc::collect_all_meshes(::REManagedObject* tf) {
    ensure_types();
    clear_all_meshes();

    if (tf == nullptr || m_t_mesh == nullptr) {
        return false;
    }

    const std::function<void(::REManagedObject*, int)> walk =
        [&](::REManagedObject* t, int depth) {
            if (t == nullptr || depth > 14) {
                return;
            }

            auto* go = re4vr::call_safe<::REManagedObject*>(t, "get_GameObject");
            auto* mesh = get_component(go, m_t_mesh);

            if (mesh != nullptr) {
                Handle h{};
                store(h, mesh);
                m_all_meshes.push_back(h);
            }

            auto* c = re4vr::call_safe<::REManagedObject*>(t, "get_Child");

            while (c != nullptr) {
                walk(c, depth + 1);
                c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
            }
        };

    walk(tf, 0);

    m_all_set = !m_all_meshes.empty();

    if (!m_all_set) {
        clear_all_meshes();
    }

    return m_all_set;
}

// [GRAPPLE-AUSNAHME 2026-08-25] Es gibt einen DRITTEN Mercs-Griff, bei dem die
// Kamera draussen bleibt, der Killswitch aber KS4 meldet -- der Kopf faellt
// damit unter die Erstpersonen-Regel und verschwindet.
//
// [AUS ALS DEFAULT 2026-08-26, Entscheidung des Users] Der Trenner
// "Halter-Anim" ist widerlegt: am 08:12:53 lief halterL0 id=502 bank=10 (also
// die Ausnahme, Kopf AN) in einem Griff, in dem die Kamera IM Kopf sass.
// Bis der echte Unterschied gemessen ist, gilt die normale Regel.
// Die MESSUNG laeuft unveraendert weiter -- der Schalter wird bewusst erst GANZ
// UNTEN abgefragt, sonst faellt mit ihm auch die [MERC-GRAB]-Zeile weg.
bool RE4VRMerc::grapple_head_exception() {
    ensure_types();

    if (re4vr::lua_get_tribool("__re4_grappled_active") != 1) {
        m_grab_hit_logged = false;
        // naechster Griff sucht seinen Halter neu
        drop(m_grab_holder_mo);
        m_grab_holder_searched = false;
        m_grab_holder_name = "-";
        return false;
    }

    if (m_t_motion == nullptr) {
        return false;
    }

    auto* cmgr = get_char_mgr();
    auto* ctx = cmgr != nullptr
        ? re4vr::call_safe<::REManagedObject*>(cmgr, "getPlayerContextRef")
        : nullptr;
    auto* go = ctx != nullptr
        ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
        : nullptr;

    if (go == nullptr) {
        return false;
    }

    auto* mo = get_component(go, m_t_motion);

    if (mo == nullptr) {
        return false;
    }

    // Den Haltenden EINMAL pro Griff suchen (er wechselt waehrenddessen nicht).
    // Seine ANIMATION wird dagegen jeden Frame gelesen: sie wechselt mit den
    // Phasen des Griffs, und genau daran haengt die Entscheidung.
    if (!m_grab_holder_searched) {
        m_grab_holder_searched = true;

        auto* btf0 = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        glm::vec3 pp{};
        const bool have_pp = btf0 != nullptr && get_vec3(btf0, "get_Position", pp);

        auto* lst = cmgr != nullptr
            ? re4vr::call_safe<::REManagedObject*>(cmgr, "get_EnemyContextList")
            : nullptr;
        const int32_t n = lst != nullptr ? opt_int(lst, "get_Count").value_or(0) : 0;

        for (int32_t i = 0; i < n; ++i) {
            auto* e = re4vr::call_safe<::REManagedObject*>(lst, "get_Item", i);
            auto* ego = e != nullptr
                ? re4vr::call_safe<::REManagedObject*>(e, "get_BodyGameObject")
                : nullptr;
            auto* etf = ego != nullptr
                ? re4vr::call_safe<::REManagedObject*>(ego, "get_Transform")
                : nullptr;

            glm::vec3 ep{};

            if (etf == nullptr || !get_vec3(etf, "get_Position", ep) || !have_pp) {
                continue;
            }

            const glm::vec3 d = ep - pp;

            if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) < 2.0f) {
                m_grab_holder_name = obj_name_of(ego);
                store(m_grab_holder_mo, get_component(ego, m_t_motion));
                break;
            }
        }
    }

    bool hit = false;

    if (m_grab_holder_mo.obj != nullptr) {
        for (int32_t li = 0; li <= 2; ++li) {
            auto* lay = re4vr::call_safe<::REManagedObject*>(m_grab_holder_mo.obj,
                                                             "getLayer", li);

            if (lay == nullptr) {
                continue;
            }

            const auto id = opt_int(lay, "get_MotionID");

            if (!id.has_value() || static_cast<uint32_t>(*id) == 4294967295u) {
                continue;
            }

            auto bank = opt_int(lay, "get_MotionBankID");

            if (!bank.has_value()) {
                bank = opt_int(lay, "get_BankID");
            }


            for (const auto& m : GRAB_MOTIONS) {
                if (li == m.layer && *id == m.id
                    && (bank.has_value() && *bank == m.bank)) {
                    hit = true;
                }
            }
        }
    }

    // Default: im Griff bleibt der Kopf aus (s.o.).
    if (!m_grab_head_on) {
        return false;
    }

    return hit;
}

// Kopf/Haare gehoeren NUR in KS1 ins Bild (volle Drittperson). Gameplay und
// KS2/KS3/KS4/KS5 sind alle Erstperson -- exakt die Regel aus
// re4_vr_materials.lua:575.
bool RE4VRMerc::show_head_now() {
    // [GRAPPLE-AUSNAHME] Dieser eine Griff laeuft in Drittperson.
    if (grapple_head_exception()) {
        return true;
    }

    // Faellt das Killswitch-Modul aus, ist der Kopf schlicht immer aus --
    // derselbe Rueckfall wie in Lua (`is_active = function() return false end`).
    if (!re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", "is_active", false)) {
        return false;
    }

    for (const char* fn : {"is_ks2", "is_ks3", "is_ks4", "is_ks5"}) {
        if (re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", fn, false)) {
            return false;
        }
    }

    return true;
}

bool RE4VRMerc::full_hide_now() {
    for (const char* fn : {"is_ks3", "is_ks5"}) {
        if (re4vr::lua_module_call_bool("re4vr/re4_vr_killswitch", fn, false)) {
            return true;
        }
    }

    return false;
}

// [KRAUSER-SZENENWEG 2026-09-09] Leben die gemerkten Meshes noch?
bool RE4VRMerc::scene_cache_alive() {
    if (!m_kr_set) {
        return false;
    }

    for (const auto& e : m_kr_mats) {
        if (re4vr::call_safe<::REManagedObject*>(e.mesh.obj, "get_GameObject") == nullptr) {
            return false;
        }

        auto* mn = re4vr::call_safe<::REManagedObject*>(e.mesh.obj, "getMaterialName", e.idx);

        if (managed_string_of(mn) != e.name) {
            return false;
        }
    }

    return true;
}

// Ueber die SZENE suchen (wie EMV), aber nur Krausers drei GameObjects
// anfassen. Aufbau bewusst wie scan_extra_mats -- inklusive der 5-Sekunden-
// Bremse, ein findComponents ueber die ganze Szene ist teuer.
// Haengt das GameObject unter dem Koerper des Spielers? DAS ist die eigentliche
// Sperre gegen Leons Kampagne: 'head', 'hair', 'Face_mat' tragen dort auch die
// Gegner -- aber niemals unter UNSEREM Koerper.
bool RE4VRMerc::under_player_body(::REManagedObject* go, const std::string& body) {
    auto* tf = go != nullptr
        ? re4vr::call_safe<::REManagedObject*>(go, "get_Transform")
        : nullptr;

    for (int i = 0; i < 12 && tf != nullptr; ++i) {
        auto* g = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

        if (g != nullptr && obj_name_of(g) == body) {
            return true;
        }

        tf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
    }

    return false;
}

void RE4VRMerc::scan_scene_mats(int32_t kind) {
    ensure_types();

    const auto* def = scene_hide_for(kind);
    const std::string body = re4vr::lua_get_string("__re4_merc_body");

    if (def == nullptr || body.empty()) {
        return;
    }

    // Charakterwechsel -> alter Satz ist wertlos
    if (m_kr_set && m_kr_kind != kind) {
        for (auto& e : m_kr_mats) {
            drop(e.mesh);
        }

        m_kr_mats.clear();
        m_kr_set = false;
        m_kr_off = false;
        m_kr_scan_t = 0.0;
    }

    if (m_kr_set && !scene_cache_alive()) {
        for (auto& e : m_kr_mats) {
            drop(e.mesh);
        }

        m_kr_mats.clear();
        m_kr_set = false;
        m_kr_off = false;
        m_kr_scan_t = 0.0;
    }

    if (m_kr_set) {
        return;
    }

    if ((clock_now() - m_kr_scan_t) < 5.0) {
        return;
    }

    m_kr_scan_t = clock_now();

    if (m_t_mesh == nullptr) {
        return;
    }

    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return;
    }

    auto* arr = re4vr::call_safe<::REManagedObject*>(
        (::REManagedObject*)scene, "findComponents(System.Type)", m_t_mesh);

    if (arr == nullptr) {
        return;
    }

    const int32_t n = re4vr::array_size(arr);

    for (int32_t i = 0; i < n; ++i) {
        auto* mesh = re4vr::array_element(arr, i);

        if (mesh == nullptr) {
            continue;
        }

        auto* go = re4vr::call_safe<::REManagedObject*>(mesh, "get_GameObject");

        if (go == nullptr || def->gos->count(obj_name_of(go)) == 0) {
            continue;
        }

        if (!under_player_body(go, body)) {
            continue;
        }

        int32_t mn = 0;

        if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", mn)) {
            continue;
        }

        for (int32_t mi = 0; mi < mn; ++mi) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialName", mi);
            const std::string s = managed_string_of(nm);

            if (s.empty() || def->mats->count(s) == 0) {
                continue;
            }

            ExtraMat e{};
            store(e.mesh, mesh);
            e.idx = mi;
            e.name = s;
            m_kr_mats.push_back(e);
        }
    }

    m_kr_set = !m_kr_mats.empty();
    m_kr_kind = m_kr_set ? kind : -1;
}

// Im AUS-Zustand jeden Tick nachdruecken (die Engine setzt beim Nachladen
// zurueck), beim Wiedereinschalten genuegt die Flanke -- Muster wie
// apply_extra_mats.
void RE4VRMerc::apply_scene_mats(bool off) {
    if (!m_kr_set) {
        return;
    }

    if (!off && off == m_kr_off) {
        return;
    }

    for (const auto& e : m_kr_mats) {
        if (!call_pcall(e.mesh.obj, "setMaterialsEnable", e.idx, !off)) {
            for (auto& x : m_kr_mats) {
                drop(x.mesh);
            }

            m_kr_mats.clear();
            m_kr_set = false;
            m_kr_off = false;
            return;
        }
    }

    m_kr_off = off;
}

// Jeden Frame erzwingen: die Engine setzt DrawDefault beim Nachladen/
// Reparenten zurueck. Schlaegt ein Call fehl, ist der gecachte Renderer tot
// -> Cache wegwerfen statt still nichts mehr zu tun.
void RE4VRMerc::apply_hide() {
    if (!m_hh_set) {
        return;
    }

    // Kopf/Haare NUR in KS1 sichtbar -- alles andere ist Erstperson.
    // [KS1-ONLY 2026-08-02] Vorher hing das nur an is_active, und die ist in
    // BEIDEN Mercs-Grapples an: dem von hinten (Drittperson, KS1) und dem von
    // vorne (bleibt Erstperson). Die Stufe trennt sie sauber.
    const bool want_hidden = m_hide_enabled && !show_head_now();

    for (auto& e : m_hh_meshes) {
        bool ok = false;

        if (e.idx.has_value()) {
            // Einzelmaterial (Hut im gemischten body-Mesh): nur dieser Slot.
            // BEIDE Aufrufformen wie in re4_vr_materials.lua -- die kurze
            // Variante trifft je nach Overload-Aufloesung nicht immer. Nur die
            // erste entscheidet ueber den Cache, genau wie in Lua.
            ok = call_pcall(e.mesh.obj, "setMaterialsEnable", *e.idx, !want_hidden);
            call_pcall(e.mesh.obj, "setMaterialsEnable(System.Int32,System.Boolean)",
                       *e.idx, !want_hidden);
        } else {
            ok = call_pcall(e.mesh.obj, "set_DrawDefault", !want_hidden)
                && call_pcall(e.mesh.obj, "set_DrawShadowCast", true);
        }

        if (!ok) {
            clear_hh();
            return;
        }
    }
}

// ============================================================================
// (3) [MERC_FULLHIDE 2026-08-05] "bei den Merc-Charakteren muss das Mesh bei
// genau demselben Trigger komplett aus"
// ============================================================================
// Trigger ist derselbe wie in re4_vr_materials.lua (fp_only_now = ks3 or ks5).
// Bei Leon/Ada blendet materials.lua dort den ganzen Koerper aus -- dessen
// Body-Whitelist kennt aber KEINEN Mercs-Body, darum sah man in Mercs bei jedem
// Treffer den eigenen Koerper.
//
// Verfahren wie im [SHADOW BODY]-Zweig dort: Materialien ANLASSEN (Geometrie
// fuer den Schatten) und nur den Farb-Pass abschalten.

// [LEICHENTEST 2026-08-18] In Mercs gab es bisher GAR KEINEN Gueltigkeitstest:
// einmal gefunden, hielt der Cache bis ans Ende -- `setMaterialsEnable` an
// einer Leiche WIRFT nicht, es verpufft. Belegt im Log: das GameObject war weg,
// `get_DrawDefault()` antwortete aber weiter. Das GameObject ist der einzige
// Wert, der den Tod nicht ueberlebt -> danach wird geprueft.
bool RE4VRMerc::extra_cache_alive() {
    if (!m_extra_set) {
        return false;
    }

    for (const auto& e : m_extra_mats) {
        auto* go = re4vr::call_safe<::REManagedObject*>(e.mesh.obj, "get_GameObject");

        if (go == nullptr) {
            return false;
        }

        auto* mn = re4vr::call_safe<::REManagedObject*>(e.mesh.obj, "getMaterialName",
                                                        e.idx);

        if (managed_string_of(mn) != e.name) {
            return false;
        }
    }

    if (m_extra_furs_set) {
        for (const auto& f : m_extra_furs) {
            if (re4vr::call_safe<::REManagedObject*>(f.obj, "get_GameObject") == nullptr) {
                return false;
            }
        }
    }

    return true;
}

void RE4VRMerc::scan_extra_mats() {
    ensure_types();

    // Toten Cache verwerfen und SOFORT neu suchen duerfen (Drosselung mit
    // zuruecksetzen, sonst stuende das Fell bis zu 5 s sichtbar da).
    if (m_extra_set && !extra_cache_alive()) {
        for (auto& e : m_extra_mats) {
            drop(e.mesh);
        }

        m_extra_mats.clear();
        m_extra_set = false;
        m_extra_mats_off = false;

        for (auto& f : m_extra_furs) {
            drop(f);
        }

        m_extra_furs.clear();
        m_extra_furs_set = false;
        m_extra_scan_t = 0.0;
    }

    if (m_extra_set) {
        return;
    }

    // Teuer -> hoechstens alle 5 s.
    if ((clock_now() - m_extra_scan_t) < 5.0) {
        return;
    }

    m_extra_scan_t = clock_now();

    if (m_t_mesh == nullptr) {
        return;
    }

    // [EXTRA_MATS 2026-08-05] Die Jacke blieb im KS3 stehen, obwohl DrawDefault
    // am GO 'jacket' nachweislich false war. EMV findet seine Meshes NICHT ueber
    // den Transform-Baum, sondern ueber die SZENE -- genau dieser Weg hier.
    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return;
    }

    auto* arr = re4vr::call_safe<::REManagedObject*>(
        (::REManagedObject*)scene, "findComponents(System.Type)", m_t_mesh);

    if (arr == nullptr) {
        return;
    }

    // [ARRAY, KEINE LISTE] findComponents liefert ein System.Array -- Luas
    // `arr:get_elements()` ist ein REFramework-Binding, get_Count/get_Item
    // gibt es darauf nicht.
    const int32_t n = re4vr::array_size(arr);

    for (int32_t i = 0; i < n; ++i) {
        auto* mesh = re4vr::array_element(arr, i);

        if (mesh == nullptr) {
            continue;
        }

        int32_t mn = 0;

        if (!re4vr::try_call<int32_t>(mesh, "get_MaterialNum", mn)) {
            continue;
        }

        for (int32_t mi = 0; mi < mn; ++mi) {
            auto* nm = re4vr::call_safe<::REManagedObject*>(mesh, "getMaterialName", mi);
            const std::string s = managed_string_of(nm);

            if (s.empty() || FULLHIDE_EXTRA_MATS.count(s) == 0) {
                continue;
            }

            ExtraMat e{};
            store(e.mesh, mesh);
            e.idx = mi;
            e.name = s;
            m_extra_mats.push_back(e);
        }
    }

    m_extra_set = !m_extra_mats.empty();

    // [FUR 2026-08-15] Fur-Komponenten am gefundenen Mesh einsammeln (GO,
    // Eltern-GO, direkte Kinder). Doppelte Eintraege sind unkritisch.
    // BEWUSST NICHT szenenweit: Gegner (und Krauser als Gegner-Modell) haben
    // eigene Fur-Komponenten.
    if (!m_extra_set || m_extra_furs_set) {
        return;
    }

    const auto take = [&](::REManagedObject* go) {
        if (go == nullptr) {
            return;
        }

        for (auto* t : {m_t_fur, m_t_shellfur}) {
            // [NIL-TYPE-GUARD] Fehlender Typ bleibt nullptr und wird
            // uebersprungen -- getComponent(nil) flutet das Log.
            auto* c = get_component(go, t);

            if (c != nullptr) {
                Handle h{};
                store(h, c);
                m_extra_furs.push_back(h);
            }
        }
    };

    for (const auto& e : m_extra_mats) {
        auto* go = re4vr::call_safe<::REManagedObject*>(e.mesh.obj, "get_GameObject");

        if (go == nullptr) {
            continue;
        }

        take(go);

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

        if (tf == nullptr) {
            continue;
        }

        auto* par = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");

        if (par != nullptr) {
            take(re4vr::call_safe<::REManagedObject*>(par, "get_GameObject"));
        }

        auto* ch = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

        while (ch != nullptr) {
            take(re4vr::call_safe<::REManagedObject*>(ch, "get_GameObject"));
            ch = re4vr::call_safe<::REManagedObject*>(ch, "get_Next");
        }
    }

    m_extra_furs_set = !m_extra_furs.empty();
}

void RE4VRMerc::apply_extra_mats(bool off, const std::unordered_set<std::string>* only) {
    if (!m_extra_set && !m_extra_furs_set) {
        return;
    }

    // [GLEICHZUG 2026-08-18] Vorher lief das nur auf der Flanke. Baut die
    // Engine Mesh/Fur waehrend eines laufenden Voll-Aus neu auf (neue Runde,
    // Respawn, Kostuem), wurde nie wieder geschrieben und das Fell stand
    // sichtbar da. Im AUS-Zustand jeden Tick nachdruecken, beim Einschalten
    // genuegt die Flanke -- identisch zu materials.lua seit dem 09.08.
    // Ein FILTERWECHSEL zaehlt wie ein Zustandswechsel: sonst bliebe beim
    // Uebergang Rage -> Voll-Aus (und zurueck) der jeweils andere Satz stehen.
    if (!off && off == m_extra_mats_off && only == m_extra_only) {
        return;
    }

    // [FUR 2026-08-15] Zuerst das Fell: die Material-Schleife darunter steigt
    // bei einem toten Mesh mit return aus -- stuende das Fell danach, bliebe es
    // in genau dem Fall sichtbar.
    if (m_extra_furs_set) {
        for (const auto& f : m_extra_furs) {
            call_pcall(f.obj, "set_DrawDefault", !off);
            call_pcall(f.obj, "set_DrawShadowCast", true);
        }
    }

    if (!m_extra_set) {
        m_extra_mats_off = off;
        m_extra_only = only;
        return;
    }

    for (const auto& e : m_extra_mats) {
        // [RAGE] Mit Filter nur die genannten Materialien anfassen -- die
        // uebrigen gehoeren dem Voll-Aus und bleiben unberuehrt.
        if (only != nullptr && only->count(e.name) == 0) {
            continue;
        }

        // Aufrufform wie im EMV (init.lua ~7335).
        if (!call_pcall(e.mesh.obj, "setMaterialsEnable", e.idx, !off)) {
            // Mesh tot -> neu suchen
            for (auto& x : m_extra_mats) {
                drop(x.mesh);
            }

            m_extra_mats.clear();
            m_extra_set = false;
            m_extra_mats_off = false;

            for (auto& x : m_extra_furs) {
                drop(x);
            }

            m_extra_furs.clear();
            m_extra_furs_set = false;
            return;
        }
    }

    m_extra_mats_off = off;
    m_extra_only = only;

    // [FUR-DIAG 2026-08-18] NUR Export, kein Verhalten -- derselbe Satz wie in
    // materials, damit ein Wegwerf-Logger in Mercs GENAU die Objekte misst, die
    // dieser Zweig auch schaltet. Nativ koennen die Objektlisten nicht nach Lua
    // durchgereicht werden; die drei skalaren Felder gehen mit.
    re4vr::lua_ensure_table("__re4_fur_dbg");
    re4vr::lua_set_table_number("__re4_fur_dbg", "mats",
                                static_cast<double>(m_extra_mats.size()));
    re4vr::lua_set_table_number("__re4_fur_dbg", "furs",
                                static_cast<double>(m_extra_furs.size()));
    re4vr::lua_set_table_bool("__re4_fur_dbg", "off", m_extra_mats_off);
    // `quelle` ist im Original ein String ("mercs"). Es gibt dafuer keinen
    // Tabellen-Setter; der Eintrag ist reine Diagnose ohne Leser und entfaellt.
    // Die drei Zahlen darueber beantworten dieselbe Frage.
}

// Waehrend KS3/KS5 jeden Frame erzwingen (die Engine setzt DrawDefault selbst
// zurueck), beim Austritt EINMAL zurueckschalten.
void RE4VRMerc::apply_full_hide() {
    // [KAMPAGNE-SPERRE 2026-09-01] NUR IN MERCENARIES. FULLHIDE_EXTRA_MATS
    // enthaelt 'Boa_Mat' = Krausers Barett, und gesucht wird SZENENWEIT -- also
    // ueber jedes Mesh im Level, nicht nur ueber den Spielerkoerper. In der
    // Kampagne ist Krauser der GEGNER: trifft er dich, geht KS3/KS5 an und sein
    // Barett verschwindet. `hide_enabled` ist der Desktop-Haken und steht per
    // Default auf true -- der hat nie gegatet.
    const bool in_mercs = re4vr::lua_get_tribool("__re4_in_mercs") == 1;
    const bool want_extra = m_hide_enabled && in_mercs && full_hide_now();

    // [RAGE-BARETT 2026-09-09] Krausers SICHTBARES Barett ist 'Boa_Mat' (+ Band
    // 'Ribbon_Mat'). Es haengt nicht im Transform-Baum des Spielerkoerpers --
    // EMV findet solche Meshes nur ueber die SZENE, genau dafuer gibt es
    // scan_extra_mats. Dieser Weg lief bisher aber AUSSCHLIESSLICH im Voll-Aus
    // (full_hide_now = ks3/ks5). Der Ragemodus setzt KS4: Kopf und Haare gingen
    // ueber show_head_now weg, das Barett blieb sichtbar stehen.
    // Also denselben Scan auch im Rage fahren -- aber nur die zwei
    // Barett-Materialien schalten (s. RAGE_SCENE_MATS_KRAUSER) und nur bei
    // Krauser, sonst traefe es in Mercs fremde Meshes.
    const bool rage_beret = m_hide_enabled && in_mercs
        && re4vr::lua_get_tribool("__re4_force_ks4_bulletrush") == 1
        && static_cast<int32_t>(re4vr::lua_get_number("__re4_merc_kind", -1.0)) == 600002;

    if (want_extra || rage_beret) {
        scan_extra_mats();
    }

    if (want_extra) {
        apply_extra_mats(true, nullptr);
    } else if (rage_beret) {
        apply_extra_mats(true, &RAGE_SCENE_MATS_KRAUSER);
    } else {
        apply_extra_mats(false, nullptr);
    }

    if (!m_all_set) {
        return;
    }

    const bool want = m_hide_enabled && full_hide_now();

    // Normalfall: nichts anfassen.
    if (!want && !m_full_hidden) {
        return;
    }

    for (const auto& m : m_all_meshes) {
        if (!call_pcall(m.obj, "set_DrawDefault", !want)
            || !call_pcall(m.obj, "set_DrawShadowCast", true)) {
            // toter Renderer (Save-Load/Rundenende) -> Cache verwerfen statt
            // still nichts mehr zu tun
            clear_all_meshes();
            return;
        }
    }

    m_full_hidden = want;
}

// ============================================================================
// (4) [MERC_WEP_OFFSET 2026-08-03] WAFFE-only Versatz fuer Mercs-Waffen
// ============================================================================
// Die Waffe haengt (wie in der Kampagne) an der RECHTEN Hand. Hier wird NUR die
// Waffe gegen die Hand verschoben/gedreht -- die Hand selbst bleibt 1:1, weil
// dieser Hook erst ganz am Ende von attach_weapon greift.

RE4VRMerc::WepOff& RE4VRMerc::wep_off_for(int32_t wid) {
    auto it = m_wep_off.find(wid);

    if (it == m_wep_off.end()) {
        it = m_wep_off.emplace(wid, WepOff{}).first;
    }

    return it->second;
}

bool RE4VRMerc::wep_apply(glm::vec3& pos, glm::quat& rot, int32_t wid,
                          const glm::quat& hand_rot) {
    if (!m_wep_off_on) {
        return false;
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
        return false;
    }

    const auto it = m_wep_off.find(wid);

    // Lua: `local o = wep_off[tostring(wid)]; if not o then return nil, nil end`
    // -- der Eintrag wird hier ABSICHTLICH nicht angelegt.
    if (it == m_wep_off.end()) {
        return false;
    }

    const WepOff& o = it->second;

    if (o.px == 0.0f && o.py == 0.0f && o.pz == 0.0f && o.rx == 0.0f && o.ry == 0.0f
        && o.rz == 0.0f) {
        return false;
    }

    // Offset im Hand-Frame. Die Rotation aus DIESEM Pass; __vr_rh_rot ist nur
    // der Fallback -- es wird erst am Tick-Ende publiziert und ist beim Aufruf
    // einen Pass alt.
    const glm::vec3 ov = hand_rot * glm::vec3{o.px, o.py, o.pz};
    pos = pos + ov;

    if (o.rx != 0.0f || o.ry != 0.0f || o.rz != 0.0f) {
        rot = glm::normalize(rot * quat_from_euler_deg(o.rx, o.ry, o.rz));
    }

    return true;
}

// ============================================================================
// (5) [BOW_POSE 2026-08-03] Handposen fuer den Compound Bow
// ============================================================================
// compoundBOW     -- LINKS gecaptured  -> gespiegelt auf die RECHTE Hand
// compoundBOWLEFT -- RECHTS gecaptured -> gespiegelt auf die LINKE Hand
// Gespiegelt wird zur Laufzeit (Bone L_x <-> R_x + Quaternion), damit die
// Rohdaten in der JSON unveraendert bleiben.

RE4VRMerc::PoseBones RE4VRMerc::bow_mirror_bones(const PoseBones& src) const {
    PoseBones out;

    for (const auto& [name, q] : src) {
        std::string n2 = name;

        if (name.size() >= 2) {
            const std::string side = name.substr(0, 2);

            if (side == "L_") {
                n2 = "R_" + name.substr(2);
            } else if (side == "R_") {
                n2 = "L_" + name.substr(2);
            }
        }

        // Welche der drei Varianten die richtige ist, haengt an der
        // Achsenkonvention der Fingerjoints -- deshalb umschaltbar statt
        // geraten (Default 1).
        if (m_bow_mirror_mode == 2) {
            out[n2] = glm::quat{q.w, -q.x, q.y, -q.z};
        } else if (m_bow_mirror_mode == 3) {
            out[n2] = glm::quat{q.w, -q.x, -q.y, q.z};
        } else {
            out[n2] = glm::quat{q.w, q.x, -q.y, -q.z};
        }
    }

    return out;
}

const RE4VRMerc::PoseBones* RE4VRMerc::bow_mirrored(const std::string& name) {
    const auto c = m_bow_mirror_cache.find(name);

    if (c != m_bow_mirror_cache.end()) {
        return &c->second;
    }

    const auto p = m_bow_poses.find(name);

    if (p == m_bow_poses.end()) {
        return nullptr;
    }

    const auto ins = m_bow_mirror_cache.emplace(name, bow_mirror_bones(p->second));

    return &ins.first->second;
}

// Aufgerufen von RE4VRMotion im BeginRendering-POST-Pass (im Pre-Pass wuerde
// die Engine-Anim die Pose jeden Frame ueberschreiben).
void RE4VRMerc::apply_bow_pose() {
    if (!m_bow_pose_on) {
        return;
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
        return;
    }

    if (static_cast<int32_t>(re4vr::lua_get_number("__vr_dbg_wep_id", -1.0)) != BOW_WID) {
        return;
    }

    // Geschrieben wird ueber __re4_reload_apply_pose_bones (reload.lua). Fehlt
    // die Funktion, passiert nichts.
    if (!re4vr::lua_has_function("__re4_reload_apply_pose_bones")) {
        return;
    }

    const auto* rp = bow_mirrored("compoundBOW");       // -> rechte Hand
    const auto* lp = bow_mirrored("compoundBOWLEFT");   // -> linke Hand (Lazypose)

    // [BOW_KNIFE_LEFT 2026-08-05] Liegt das Messer in der LINKEN Hand, gehoert
    // die linke Pose dem Messer. Wir laufen im POST-Pass NACH
    // __re4_apply_left_knife_pose -- ohne diesen Ausstieg wuerde die
    // Bogen-Lazypose die Messerpose jeden Frame ueberschreiben.
    if (re4vr::lua_get_string("__re4_knife_hand") == "left") {
        lp = nullptr;
    }

    bool okr = false;
    bool okl = false;

    if (rp != nullptr) {
        okr = re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones", *rp,
                                         m_bow_pose_blend);
    }

    if (lp != nullptr) {
        okl = re4vr::lua_call_pose_bones("__re4_reload_apply_pose_bones", *lp,
                                         m_bow_pose_blend);
    }

    // Sichtbar machen, ob wirklich geschrieben wurde -- "Pose sieht falsch aus"
    // und "Pose kommt gar nicht an" fuehlen sich im Headset identisch an.
    char buf[160]{};
    std::snprintf(buf, sizeof(buf), "rechts=%s (%d Bones)  links=%s (%d Bones)",
                  okr ? "geschrieben" : "NEIN",
                  rp != nullptr ? static_cast<int>(rp->size()) : 0,
                  okl ? "geschrieben" : "NEIN",
                  lp != nullptr ? static_cast<int>(lp->size()) : 0);
    m_bow_dbg = buf;
}

// ============================================================================
// (6) [BOW_PIN 2026-08-06] Compound Bow NATIV an die Hand haengen
// ============================================================================
// Bisher wurde der Bogen wie jede Waffe pro Frame in WELT-Koordinaten
// nachgezogen -> er lief der Hand sichtbar hinterher. Dieselbe Lektion gab es
// bei den Magazin-/Shell-Klonen: erst das NATIVE Parenten hat sie festgeklebt.
// Die Engine propagiert die Transform dann VOR dem Skinning.
//
// CRASH-FALLE: set_Parent auf einen STALEN Transform loest eine native Access
// Violation aus, die pcall NICHT faengt -> vor jedem Parenten get_Valid.

::REManagedObject* RE4VRMerc::bow_find_go(::REManagedObject* btf,
                                          ::REManagedObject** tf_out) {
    if (tf_out != nullptr) {
        *tf_out = nullptr;
    }

    if (btf == nullptr) {
        return nullptr;
    }

    // [1:1] Die Schleife ueber die drei Suffixe laeuft jedesmal von vorn --
    // und nur ueber die DIREKTEN Kinder, nicht rekursiv.
    for (const char* suffix : {"", "_MC", "_AO"}) {
        char nm[32]{};
        std::snprintf(nm, sizeof(nm), "wp%d%s", BOW_WID, suffix);

        auto* c = re4vr::call_safe<::REManagedObject*>(btf, "get_Child");

        while (c != nullptr) {
            auto* g = re4vr::call_safe<::REManagedObject*>(c, "get_GameObject");

            if (obj_name_of(g) == nm) {
                if (tf_out != nullptr) {
                    *tf_out = c;
                }

                return g;
            }

            c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
        }
    }

    return nullptr;
}

void RE4VRMerc::bow_unpin() {
    // Zurueck an den Body-Root: die Engine setzt die Waffe beim naechsten
    // Equip/Holster ohnehin selbst, wichtig ist nur, dass sie nicht am
    // Handgelenk kleben bleibt.
    if (m_bowp_tf.obj != nullptr) {
        auto* cmgr = get_char_mgr();
        auto* ctx = cmgr != nullptr
            ? re4vr::call_safe<::REManagedObject*>(cmgr, "getPlayerContextRef")
            : nullptr;
        auto* bgo = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;
        const bool ok = bgo != nullptr && valid_not_false(bgo);
        auto* btf = ok ? re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform")
                       : nullptr;

        if (btf != nullptr) {
            auto* str = sdk::VM::create_managed_string(L"");

            if (str != nullptr) {
                call_pcall(m_bowp_tf.obj, "set_ParentJoint", str);
            }
        }
    }

    drop(m_bowp_go);
    drop(m_bowp_tf);
    m_bowp_pinned = false;
}

// [BODY-EPOCH 2026-09-22] Nach Save-Load/Tod bleiben die alten Objekte
// ansprechbar -- Schreiben auf die Leiche wirft nicht, die Selbstheilungen
// (pcall-Fehler, opt_bool leer, Namensluecke) greifen dann nicht sicher.
// Nur die eigenen Handles loslassen (release wie on_lua_state_destroyed),
// KEIN set_ParentJoint/Engine-Aufruf auf dem alten Bogen: wie beim Reset
// bleibt er, wo er ist, der Pin-Zweig setzt den neuen Bogen neu.
void RE4VRMerc::drop_body_caches() {
    drop(m_bowp_go);
    drop(m_bowp_tf);
    m_bowp_pinned = false;

    drop(m_br_hu);

    // Kopf/Haar- und Voll-Aus-Meshes: der CHECK_EVERY-Zweig sammelt neu
    // (!m_hh_set). m_full_hidden faellt mit -- der neue Body ist frisch.
    clear_hh();
    clear_all_meshes();
}

void RE4VRMerc::update_bow_pin() {
    const bool want = m_bow_pin_on
        && re4vr::lua_get_tribool("__re4_in_mercs") == 1
        && static_cast<int32_t>(re4vr::lua_get_number("__vr_dbg_wep_id", -1.0)) == BOW_WID
        && re4vr::lua_get_tribool("__re4_ks4_active") != 1
        && re4vr::lua_get_tribool("__re4_ks_active") != 1;

    if (!want) {
        if (m_bowp_pinned) {
            bow_unpin();
        }

        re4vr::lua_set_bool("__re4_merc_bow_pinned", false);
        return;
    }

    if (!m_bowp_pinned) {
        auto* cmgr = get_char_mgr();
        auto* ctx = cmgr != nullptr
            ? re4vr::call_safe<::REManagedObject*>(cmgr, "getPlayerContextRef")
            : nullptr;
        auto* bgo = ctx != nullptr
            ? re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject")
            : nullptr;

        // [CRASH-FALLE] beide Seiten auf Gueltigkeit pruefen
        if (bgo == nullptr || !valid_not_false(bgo)) {
            return;
        }

        auto* btf = re4vr::call_safe<::REManagedObject*>(bgo, "get_Transform");

        if (btf == nullptr) {
            return;
        }

        ::REManagedObject* tf = nullptr;
        auto* go = bow_find_go(btf, &tf);

        if (go == nullptr || tf == nullptr) {
            return;
        }

        if (!valid_not_false(go)) {
            return;
        }

        call_pcall(tf, "set_Parent", btf);

        auto* jstr = sdk::VM::create_managed_string(utility::widen(std::string{BOW_PIN_JOINT}));

        if (jstr == nullptr || !call_pcall(tf, "set_ParentJoint", jstr)) {
            return;
        }

        store(m_bowp_go, go);
        store(m_bowp_tf, tf);
        m_bowp_pinned = true;
    }

    // nur noch LOKALE Pose -- die Welt macht die Engine.
    // [1:1] Lua hat beide Setter in EINEM pcall: wirft die Position, wird die
    // Rotation gar nicht mehr versucht.
    const WepOff& o = wep_off_for(BOW_WID);

    if (set_vec3(m_bowp_tf.obj, "set_LocalPosition", glm::vec3{o.px, o.py, o.pz})) {
        set_quat(m_bowp_tf.obj, "set_LocalRotation",
                 quat_from_euler_deg(o.rx, o.ry, o.rz));
    }

    re4vr::lua_set_bool("__re4_merc_bow_pinned", true);
}

// ============================================================================
// (8) [BULLETRUSH_KS4 2026-08-05] Mercenaries-Ragemodus
// ============================================================================
// Waehrend der Rage-Phase steht am HeadUpdater (chainsaw.Ch6CommonHeadUpdater --
// gilt fuer ALLE Mercs-Charaktere) get_PlayingBulletRush auf true. Der
// Killswitch ist in dieser Zeit komplett AUS -- fuer die Engine ist das
// normales Gameplay, deshalb bleiben die VR-Haende stehen, waehrend der Body
// nativ pruegelt.
//
// Wir publizieren das Flag; den KS4 macht daraus der Killswitch. Jeden Frame
// aktualisiert (NICHT im CHECK_EVERY-Takt): das Flag muss sofort wieder fallen.
void RE4VRMerc::update_bulletrush() {
    const int32_t kind = static_cast<int32_t>(re4vr::lua_get_number("__re4_merc_kind", -1.0));
    bool is_br_char = false;

    for (int32_t k : BR_CHARS) {
        if (k == kind) {
            is_br_char = true;
            break;
        }
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1 || !is_br_char) {
        drop(m_br_hu);
        m_br_since = 0.0;
        re4vr::lua_set_bool("__re4_force_ks4_bulletrush", false);
        return;
    }

    if (m_br_hu.obj == nullptr) {
        auto* cmgr = get_char_mgr();
        auto* ctx = cmgr != nullptr
            ? re4vr::call_safe<::REManagedObject*>(cmgr, "getPlayerContextRef")
            : nullptr;
        store(m_br_hu, ctx != nullptr
                  ? re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater")
                  : nullptr);
    }

    bool on = false;

    if (m_br_hu.obj != nullptr) {
        const auto v = opt_bool(m_br_hu.obj, "get_PlayingBulletRush");

        // Kein Wert = toter/gewechselter HeadUpdater -> Cache verwerfen.
        if (!v.has_value()) {
            drop(m_br_hu);
        } else {
            on = *v;
        }
    }

    if (on) {
        if (m_br_since == 0.0) {
            m_br_since = clock_now();
        } else if ((clock_now() - m_br_since) > BR_MAX_SEC) {
            // Notbremse: laenger als eine Rage-Phase dauert das nie.
            on = false;
        }
    } else {
        m_br_since = 0.0;
    }

    re4vr::lua_set_bool("__re4_force_ks4_bulletrush", on);

    // [RAGE: KOPF/HAAR/BARETT BLEIBEN AUS 2026-09-09] Ob Kopf und Haare
    // versteckt werden, entscheidet show_head_now() -- und das meldet bei KS4
    // richtig "aus". Der Haken sitzt im CACHE: beim Zuenden und beim Auslaufen
    // tauscht das Spiel am Koerper Meshes bzw. Material-Slots (Krauser bekommt
    // die Powerarme). Ein toter Renderer heilt sich selbst (der Schreibfehler
    // wirft clear_hh aus), ein VERSCHOBENER Material-Index aber NICHT:
    // setMaterialsEnable(idx, ...) auf einem lebenden Mesh gelingt weiterhin,
    // nur trifft es dann den falschen Slot -- genau der Fall bei Krausers
    // Barett, das als Einzelmaterial im Body-Mesh chi200_00 steckt.
    //
    // Deshalb an BEIDEN Flanken einmal neu einsammeln, statt auf den
    // CHECK_EVERY-Takt (30 Frames) zu warten.
    if (on != m_br_on_prev) {
        m_br_on_prev = on;

        const auto pb = get_player_body();

        if (pb.name_ok && pb.tf != nullptr) {
            const auto* mc = pb.kind.has_value() ? merc_char(*pb.kind) : nullptr;

            collect_hh(pb.tf, mc != nullptr ? mc->hide : nullptr,
                       mc != nullptr ? mc->extra : nullptr);
            m_hh_body = m_hh_set ? pb.name : std::string{};
        }
    }
}

// ============================================================================
// (7) [MERCS_HUD 2026-08-05] Groesse + Position der Mercs-Anzeigen
// ============================================================================
// Das sind KEINE eigenen GUI-GameObjects, sondern GuiBehaviors -- deshalb ueber
// deren Panels. Angefasst wird jeweils EIN Control.
//
// [MERCS-HUD 2026-08-09, live gefunden] Mercenaries hat EIGENE HUD-Behaviors
// mit dem Praefix `Cp1021` -- `chainsaw.TimerGuiBehavior` (ohne Praefix) ist der
// Timer der KAMPAGNE und existiert hier nur nutzlos in der Szene. Deshalb
// bewegten die Slider nichts, egal ueber welches Panel.
//
// [BASIS FEST 2026-08-09, gemessen] bx/by = Ausgangsposition des jeweiligen
// `_Main`. NICHT live nachmessen: nach einem Rundenwechsel liefert die Szene
// ein anderes Panel, und was man dann abliest, ist nicht zwingend die Nulllage.
// Ueber 12 Messungen mit ausgeschalteten Haken waren diese Werte bitgenau
// konstant, Scale 1.0.

void RE4VRMerc::init_hud() {
    if (!m_hud.empty()) {
        return;
    }

    const auto add = [&](const char* key, float bx, float by, const char* type,
                         const char* go) {
        HudCfg c{};
        c.bx = type != nullptr ? std::optional<float>{bx} : std::nullopt;
        c.by = type != nullptr ? std::optional<float>{by} : std::nullopt;
        c.type = type;
        c.field = type != nullptr ? "_Main" : nullptr;
        c.to_parent = false;
        c.go = go;
        m_hud.emplace_back(key, c);
    };

    add("timer", 960.0f, 0.0f, "chainsaw.Cp1021TimerGuiBehavior", nullptr);
    add("score", 1444.0f, 0.0f, "chainsaw.Cp1021HudScoreDispGuiBehavior", nullptr);
    add("combo", 1920.0f, 0.0f, "chainsaw.Cp1021HudComboGuiBehavior", nullptr);
    add("total", 1520.0f, 66.0f, "chainsaw.Cp1021HudTotalScoreGuiBehavior", nullptr);
    add("gauge", 0.0f, -20.0f, "chainsaw.BulletRushGaugeGuiBehavior", nullptr);
    // [2026-08-09] Fuer diese beiden gibt es kein Behavior, ueber das der
    // Szene-Scan sie finden koennte -- sie werden ueber den GameObject-NAMEN im
    // Draw-Hook gegriffen. bx/by bleiben leer und werden beim ERSTEN Zeichnen
    // EINMAL gelesen.
    add("ui2710", 0.0f, 0.0f, nullptr, "Gui_ui2710");
    add("ui2764", 0.0f, 0.0f, nullptr, "Gui_ui2764");
}

RE4VRMerc::HudCfg* RE4VRMerc::hud_get(const char* key) {
    for (auto& [k, c] : m_hud) {
        if (k == key) {
            return &c;
        }
    }

    return nullptr;
}

// Ziel-Control eines Behaviors suchen (Szene-Scan, gedrosselt).
::REManagedObject* RE4VRMerc::find_ctrl(const HudCfg& cfg) {
    if (cfg.type == nullptr) {
        return nullptr;
    }

    auto* t = type_of(cfg.type);

    if (t == nullptr) {
        return nullptr;
    }

    auto* scene = sdk::get_current_scene();

    if (scene == nullptr) {
        return nullptr;
    }

    auto* comps = re4vr::call_safe<::REManagedObject*>(scene, "findComponents(System.Type)", t);

    if (comps == nullptr) {
        return nullptr;
    }

    const int32_t n = re4vr::array_size(comps);


    for (int32_t i = 0; i < n; ++i) {
        auto* beh = re4vr::array_element(comps, i);

        if (beh == nullptr) {
            continue;
        }

        // [FELDZUGRIFF 05.09.2026 -- GEMESSEN] Hier stand
        // utility::re_managed_object::get_field<::REManagedObject*>. Dieser Weg
        // laeuft ueber die NATIVE via.*-Reflexion und findet ein managed
        // C#-Feld wie `_Main` nicht -- er lieferte still nullptr. Beleg:
        // reframework/data/re4_merc_hud.txt zeigte "comps=ok n=1 ... feld=NULL"
        // fuer alle fuenf Behaviors. Luas beh:get_field("_Main") nimmt die TDB.
        auto* ctrl = re4vr::get_field_object(beh, cfg.field);

        if (ctrl == nullptr) {
            continue;
        }

        // Array-Feld (_NumberPanel) -> erstes Element.
        //
        // [NUR WENN ES WIRKLICH EINES IST] Lua schreibt hier
        // `if f:get_size() then ctrl = f:get_element(0) end` -- beides sind
        // REFramework-BINDINGS, die es auf einem via.gui.Panel gar nicht gibt.
        // Der Aufruf scheitert dort still in `s()`, und `ctrl` bleibt das Panel.
        // Nativ waere ein array_size() auf dem Panel dagegen ein echter
        // managed Call auf einem Nicht-Array: er wirft, und safe_wrap schreibt
        // die Exception SYNCHRON ins Log -- einmal pro Sekunde, solange Mercs
        // laeuft. Deshalb erst den Typ pruefen.
        if (is_system_array(ctrl)) {
            // Lua: get_size() liefert auch 0 -- das ist WAHR, also wird
            // get_element(0) gerufen und liefert nil; `if ctrl then` scheitert
            // dann und das Behavior wird uebersprungen.
            ctrl = re4vr::array_element(ctrl, 0);

            if (ctrl == nullptr) {
                continue;
            }
        }

        if (cfg.to_parent) {
            auto* p = re4vr::call_safe<::REManagedObject*>(ctrl, "get_Parent");

            if (p != nullptr) {
                ctrl = p;
            }
        }

        // [SCOPE_HIDE] Namen des tragenden GameObjects merken -- damit blendet
        // der Draw-Hook genau diese Anzeigen im Scope aus, ohne sie irgendwo
        // hartkodieren zu muessen.
        const std::string nm = obj_name_of(
            re4vr::call_safe<::REManagedObject*>(beh, "get_GameObject"));

        if (!nm.empty()) {
            m_hud_names.insert(nm);
        }

        return ctrl;
    }

    return nullptr;
}

// Root-Control einer GUI ueber ihr GameObject. Bewusst OHNE Cache: zwischen
// zwei Runden baut die Engine die HUD-GUIs neu auf, ein gehaltenes Control
// waere danach eine Leiche.
::REManagedObject* RE4VRMerc::root_ctrl(::REManagedObject* go) {
    ensure_types();

    if (m_t_gui == nullptr) {
        return nullptr;
    }

    auto* comp = get_component(go, m_t_gui);
    auto* view = comp != nullptr
        ? re4vr::call_safe<::REManagedObject*>(comp, "get_View")
        : nullptr;

    return view != nullptr ? re4vr::call_safe<::REManagedObject*>(view, "get_Child")
                           : nullptr;
}

// Kind mit diesem Namen (Geschwisterkette). Ueber NAMEN, nicht ueber Index --
// auf die Reihenfolge der Kinder sollte man sich nicht verlassen.
::REManagedObject* RE4VRMerc::child_by_name(::REManagedObject* ctrl, const char* want) {
    auto* c = re4vr::call_safe<::REManagedObject*>(ctrl, "get_Child");

    while (c != nullptr) {
        if (managed_string_of(re4vr::call_safe<::REManagedObject*>(c, "get_Name")) == want) {
            return c;
        }

        c = re4vr::call_safe<::REManagedObject*>(c, "get_Next");
    }

    return nullptr;
}

// [NEUE RUNDE 2026-08-09] Zwischen zwei Mercs-Runden baut die Engine die
// HUD-GUIs neu auf. Das alte Control bleibt als LEICHE im Speicher liegen und
// liefert weiterhin brav eine Position -- ein Check "kommt noch ein Wert?"
// schlaegt also NICHT an. Deshalb wird im Scan-Takt immer neu gesucht.
// Die Ausgangsposition wird BEWUSST NICHT mitgelesen, sie steht fest in HUD.
void RE4VRMerc::hud_apply() {
    const int gate = re4vr::lua_get_tribool("__re4_in_mercs");

    if (gate != 1) {
        return;
    }

    const double now = clock_now();
    const bool may_scan = (now - m_hud_scan_t) > 1.0;

    for (auto& [key, cfg] : m_hud) {
        // Eintraege mit `go` haben kein Behavior -- die holt sich der
        // Draw-Hook selbst.
        if (cfg.go != nullptr) {
            continue;
        }

        const auto cached = m_hud_cache.find(key);
        const bool have = cached != m_hud_cache.end() && cached->second.obj != nullptr;

        if (!have && !may_scan) {
            continue;
        }

        // Toten Cache erkennen: liefert das Control keine Position mehr, ist es
        // weg.
        if (have) {
            glm::vec3 dummy{};

            if (!get_vec3(cached->second.obj, "get_Position", dummy)) {
                drop(m_hud_cache[key]);
                m_hud_cache.erase(key);
            }
        }

        if (!may_scan) {
            if (m_hud_cache.find(key) == m_hud_cache.end()) {
                continue;
            }
        } else {
            // [SCOPE_HIDE] Gesucht wird IMMER (auch ohne Haken) -- nur so kennt
            // der Draw-Hook unten die GameObject-Namen. Veraendert wird nach wie
            // vor nur mit Haken.
            auto* fresh = find_ctrl(cfg);

            if (fresh == nullptr) {
                if (const auto it = m_hud_cache.find(key); it != m_hud_cache.end()) {
                    drop(it->second);
                    m_hud_cache.erase(it);
                }

                continue;
            }

            Handle h{};
            store(h, fresh);

            if (const auto it = m_hud_cache.find(key); it != m_hud_cache.end()) {
                drop(it->second);
            }

            m_hud_cache[key] = h;
        }

        auto* ctrl = m_hud_cache[key].obj;

        if (ctrl == nullptr || !cfg.on) {
            continue;
        }

        // absolut gegen die feste Basis, nie gegen einen gelesenen Ist-Wert
        const glm::vec3 want{cfg.bx.value_or(0.0f) + cfg.x, cfg.by.value_or(0.0f) + cfg.y,
                             0.0f};

        set_vec3(ctrl, "set_Position", want);
        set_vec3(ctrl, "set_Scale", glm::vec3{cfg.scale, cfg.scale, cfg.scale});
    }

    if (may_scan) {
        m_hud_scan_t = now;
    }
}

// [SCOPE_HIDE 2026-08-09] Beim Zielen durch ein montiertes Scope liegen die
// Mercs-Anzeigen mitten im gezoomten Bild -> gar nicht erst zeichnen.
bool RE4VRMerc::on_pre_gui_draw_element(::REComponent* element, void* context) {
    if (re4vr::mods_gated()) {
        return true;
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
        return true;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>((::REManagedObject*)element,
                                                    "get_GameObject");
    const std::string nm = obj_name_of(go);

    if (nm.empty()) {
        return true;
    }

    // [2026-08-09] Namens-Eintraege (Gui_ui2710/2764): hier gesetzt, weil es
    // fuer sie kein Behavior gibt. Sonst genau wie die anderen: Position
    // ABSOLUT als Basis + Offset, Groesse absolut auf den Regler.
    for (auto& [key, cfg] : m_hud) {
        if (cfg.go == nullptr || nm != cfg.go || !cfg.on) {
            continue;
        }

        auto* ctrl = root_ctrl(go);

        if (ctrl == nullptr) {
            continue;
        }

        // Nulllage genau EINMAL lesen, danach nie wieder (sonst Aufschaukeln:
        // man laese den eigenen Wert und der Offset kaeme ein zweites Mal drauf).
        if (!cfg.bx.has_value() || !cfg.by.has_value()) {
            glm::vec3 p{};

            if (get_vec3(ctrl, "get_Position", p)) {
                cfg.bx = p.x;
                cfg.by = p.y;
                save_cfg();
            }
        }

        if (cfg.bx.has_value() && cfg.by.has_value()) {
            set_vec3(ctrl, "set_Position",
                     glm::vec3{*cfg.bx + cfg.x, *cfg.by + cfg.y, 0.0f});
        }

        set_vec3(ctrl, "set_Scale", glm::vec3{cfg.scale, cfg.scale, cfg.scale});
    }

    // [2026-08-09] Gui_ui2770: mit Haken gar nicht erst zeichnen.
    if (nm == "Gui_ui2770" && m_hide_2770) {
        return false;
    }

    // [2026-08-09] Timer-Hintergrund. Jeden Frame durchgedrueckt, weil die
    // Engine die Sichtbarkeit sonst zurueckstellt. Die Knoten werden ueber
    // NAMEN gesucht, nicht ueber Kind-Indizes.
    if (nm == "Gui_ui2700" && m_hide_timer_bg) {
        auto* root = root_ctrl(go);

        if (root != nullptr) {
            if (auto* a = child_by_name(root, "c_bg_new"); a != nullptr) {
                call_pcall(a, "set_Visible", false);
            }

            auto* t = child_by_name(root, "c_timer");
            auto* b = t != nullptr ? child_by_name(t, "c_bg") : nullptr;

            if (b != nullptr) {
                call_pcall(b, "set_Visible", false);
            }
        }
    }

    // [SCOPE 2026-08-28] Quelle ist `vr_scope_active` (RE4VRScope); die alten
    // Scope-Flags sind ausgebaut.
    if (re4vr::lua_get_tribool("vr_scope_active") != 1) {
        return true;
    }

    if (m_hud_names.count(nm) > 0) {
        return false;
    }

    return true;
}

// ============================================================================
// (9) [LASER-DOT MERCS 2026-08-18] Der rote Punkt am Ende des Lasers
// ============================================================================
// In der Kampagne haben alle Pistolen mit Laser-Aufsatz einen Punkt am
// Strahlende. In Mercs wird der STRAHL gezeichnet, der PUNKT fehlt: die
// Mercs-Waffe kennt keine angebauten Teile, `IsEnableLaserSight` ist false, und
// der PointerEffect des Controllers steht auf isFinished/isDisposing.
//
// FIX (live bestaetigt "dot ist da"): Tip-Position rechnen lassen, den
// Controller zeichnen lassen, und bei totem Effekt `start()`.
// RUECKBAU: _G.__re4_merc_dot = false

::REManagedObject* RE4VRMerc::dot_gun() {
    auto* cm = character_manager();

    if (cm == nullptr) {
        return nullptr;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return nullptr;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return nullptr;
    }

    auto* g = re4vr::call_safe<::REManagedObject*>(hu, "get_EquipWeapon");

    if (g == nullptr) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(g);

    return (td != nullptr && td->is_a("chainsaw.Gun")) ? g : nullptr;
}

// Der Controller haengt am Aufsatz, also evtl. ein paar Ebenen unter dem
// Waffen-GO.
::REManagedObject* RE4VRMerc::dot_find(::REManagedObject* go, int depth) {
    ensure_types();

    if (go == nullptr || m_t_lsc == nullptr || depth > 6) {
        return nullptr;
    }

    if (auto* c = get_component(go, m_t_lsc); c != nullptr) {
        return c;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    if (tf == nullptr) {
        return nullptr;
    }

    auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");
    int guard = 0;

    while (child != nullptr && guard < 128) {
        ++guard;

        auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject");

        if (auto* r = dot_find(cgo, depth + 1); r != nullptr) {
            return r;
        }

        child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
    }

    return nullptr;
}

// [LEVELSTART 2026-08-18 -- gemessen] Zwei Mercs-Neustarts: Szene,
// MercenariesManager und GuiManager blieben bitgleich, NUR die
// Player-Body-Adresse wechselte jedes Mal. Waehrend der Ladephase (Body nicht
// lesbar) wird NICHTS zurueckgesetzt, sonst zaehlte jedes kurze Aussetzen als
// Neustart.
std::optional<uintptr_t> RE4VRMerc::player_body_addr() {
    auto* cm = character_manager();

    if (cm == nullptr) {
        return std::nullopt;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return std::nullopt;
    }

    auto* bg = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");

    if (bg == nullptr) {
        return std::nullopt;
    }

    return reinterpret_cast<uintptr_t>(bg);
}

void RE4VRMerc::dot_tick() {
    if (re4vr::lua_get_tribool("__re4_merc_dot") == 0) {
        return;
    }

    // Eine neue Runde erkennt man daran, dass der Spieler-Body kurz NICHT
    // lesbar ist.
    if (!player_body_addr().has_value()) {
        m_dot.body_weg = true;   // Ladephase -- hier NICHTS zuruecksetzen
    } else if (m_dot.body_weg) {
        m_dot.body_weg = false;  // Body wieder da -> neue Runde
        m_dot.aimed = false;
        drop(m_dot.lsc);
        m_dot.key.reset();
        m_dot.tries = 0;
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
        drop(m_dot.lsc);
        m_dot.key.reset();
        m_dot.aimed = false;
        return;
    }

    auto* g = dot_gun();

    if (g == nullptr) {
        return;
    }

    // Nur die drei Mercs-Laserwaffen.
    const auto wid = opt_int(g, "get_WeaponID");
    bool is_laser = false;

    if (wid.has_value()) {
        for (int32_t w : MERC_LASER_WIDS) {
            if (w == *wid) {
                is_laser = true;
                break;
            }
        }
    }

    if (!is_laser) {
        return;
    }

    // Meldet die Engine selbst einen Lasersight, fassen wir nichts an.
    if (opt_bool(g, "get_EnableLaserSight") == std::optional<bool>{true}) {
        return;
    }

    // [WAFFENWECHSEL ZUERST 2026-08-18] Der Reihenfolge-Fehler: der Aim-Check
    // stand VOR dem Waffenvergleich, und `aimed` wurde beim Wechsel gar nicht
    // zurueckgesetzt. Wer am Rundenanfang mit irgendetwas anderem gezielt
    // hatte, brachte den Punkt beim spaeteren Ziehen der Laserwaffe sofort mit.
    auto* wgo = re4vr::call_safe<::REManagedObject*>(g, "get_GameObject");

    if (wgo == nullptr) {
        return;
    }

    const uintptr_t key = reinterpret_cast<uintptr_t>(wgo);

    if (m_dot.key != std::optional<uintptr_t>{key}) {
        m_dot.key = key;
        drop(m_dot.lsc);
        m_dot.tries = 0;
        m_dot.draw_frames = 0;
        m_dot.aimed = false;
    }

    // [ERST BEIM AIMEN 2026-08-18] Bis zum ersten Zielen wird NICHTS gemacht:
    // direkt nach dem Levelstart gibt es noch keine gueltige Tip-Position, der
    // Punkt entstuende am Anker-GO und klebte an der Muendung.
    if (!m_dot.aimed) {
        if (re4vr::lua_get_tribool("__vr_aim_input") != 1) {
            return;
        }

        m_dot.aimed = true;
    }

    // [NUR BEIM AIM 2026-08-22] Ausserhalb des Zielens NICHTS erzwingen -- und
    // aktiv abschalten. Vorher lief der Block dauerhaft und ueberschrieb das
    // Ausblenden aus crosshair (Laser sichtbar ohne Aim), und jedes start()
    // baute den Punkt-Effekt neu auf (Farbwechsel).
    if (re4vr::lua_get_tribool("__vr_aim_input") != 1) {
        if (m_dot.lsc.obj != nullptr) {
            call_pcall(m_dot.lsc.obj, "setDraw", false);
            set_field_value<bool>(m_dot.lsc.obj, "_IsDraw", false);
        }

        return;
    }

    if (m_dot.lsc.obj == nullptr) {
        const double t = clock_now();

        if (t < m_dot.next_scan) {
            return;
        }

        m_dot.next_scan = t + 0.5;
        store(m_dot.lsc, dot_find(wgo, 0));

        if (m_dot.lsc.obj == nullptr) {
            return;
        }
    }

    // 1. Die TIP-POSITION rechnen lassen -- ohne sie hat der Punkt kein Ziel
    // und klebt an der Waffe. Die Engine ruft das in Mercs nie, weil sie keinen
    // Lasersight kennt; die Rechnung selbst funktioniert einwandfrei.
    if (auto* pe_eq = re4vr::call_safe<::REManagedObject*>(g, "get_OwnerEquipment");
        pe_eq != nullptr) {
        call_pcall(pe_eq, "set_IsEnableLaserSight", true);
    }

    call_pcall(g, "castLaserSightTip");
    call_pcall(g, "updateLaserSightTip");

    // 2. Der Controller muss zeichnen duerfen.
    // [FELDZUGRIFF 05.09.2026] TDB-Weg -- der alte Weg meldete IMMER false.
    if (re4vr::get_field_bool(m_dot.lsc.obj, "_IsDraw") != true) {
        call_pcall(m_dot.lsc.obj, "setDraw", true);
        set_field_value<bool>(m_dot.lsc.obj, "_IsDraw", true);
    }

    // 3. Toten Punkt-Effekt neu aufbauen lassen -- sofort, sobald ein toter
    // Container gesehen wird, hoechstens dreimal pro Waffe.
    // (Zwei Nachbesserungen sind gescheitert und bleiben draussen: den Anker
    // selbst setzen -- das macht der Laser-Hook in crosshair, zwei Schreiber =
    // Punkt schwebt; und start() verzoegern -- dann kam gar kein Punkt mehr.)
    if (m_dot.tries < 3) {
        // [FELDZUGRIFF 05.09.2026] TDB-Weg -- der alte Weg lieferte immer
        // nullptr, `dead` war dadurch IMMER wahr und start() feuerte jedes Mal.
        auto* eff = re4vr::get_field_object(
            m_dot.lsc.obj, "<PointerEffect>k__BackingField");
        const bool dead = eff == nullptr
            || opt_bool(eff, "get_isFinished") == std::optional<bool>{true}
            || opt_bool(eff, "get_isDisposing") == std::optional<bool>{true};

        if (dead) {
            ++m_dot.tries;
            call_pcall(m_dot.lsc.obj, "start");
        }
    }
}

// Im LateUpdateBehavior, also NACH dem Engine-Update: frueher gesetzt, dreht
// die Engine _IsDraw im selben Frame wieder zurueck.
void RE4VRMerc::on_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        dot_tick();
    }
}

// ============================================================================
// [BOW_KEEP 2026-08-05] Die Engine steckt den Compound Bow im Lauf selbst weg
// ============================================================================
// Im Volltrace steht die Kette OHNE jeden Tastendruck und ohne Lua im Stack:
//   requestEquipBareHand(0,0) -> requestChangeWeaponAction -> execChangeWeapon
//   -> equipWeapon(0,-1,..) -> castoffWeapon(-255) -> storageWeapon(6304,0)
// Also ein NATIVER Zug, den wir nicht wollen. Geblockt wird der AUSLOESER,
// damit die Kette gar nicht erst anlaeuft -- mit denselben Gates wie beim
// Messer in weapons2:
//   * nur in Mercenaries und nur mit dem Bogen (6304) in der Hand,
//   * nur NATIVE Aufrufe -- steht eins UNSERER Module im Stack, geht es durch,
//   * nie im Killswitch/KS4.

bool RE4VRMerc::bow_keep_should_skip() {
    // [SCRIPTGATE] Riegel zu = dieses Script existiert in Lua gar nicht, der
    // Block ist also nicht scharf. Ohne diese Zeile bliebe der Hook aktiv,
    // waehrend __re4_in_mercs auf seinem letzten true festhaengt -- der Bogen
    // liesse sich in Mercs nicht mehr weglegen, obwohl "Scripte aus" steht.
    if (re4vr::mods_gated()) {
        return false;
    }

    if (re4vr::lua_get_tribool("__re4_in_mercs") != 1) {
        return false;
    }

    if (static_cast<int32_t>(re4vr::lua_get_number("__vr_dbg_wep_id", -1.0)) != BOW_WID) {
        return false;
    }

    if (re4vr::lua_get_tribool("__re4_holster_killswitch") == 1) {
        return false;
    }

    if (re4vr::lua_get_tribool("__re4_ks4_active") == 1) {
        return false;
    }

    // [DER STACK-TEST, ZWEITEILIG -- s. PORT_MERC_SPEC Abschnitt 1]
    //
    // Lua prueft `debug.traceback` auf einen `autorun`-Eintrag, der nicht merc
    // selbst ist. Das deckte zwei Gruppen ab, und beide muessen weiter durch:
    //
    // (a) Die Lua-Scripte, die es noch gibt -- re4_vr_weapons.lua und
    //     re4_vr_weapons2.lua rufen requestEquipBareHand. Sie erkennt man
    //     daran, dass die Lua-VM gerade AKTIV ist: ein Aufruf aus einem
    //     Lua-Callback hat einen Stackframe, ein rein nativer Zug hat keinen.
    //
    // (b) Die inzwischen NATIVEN Aufrufer -- RE4VRHolster ruft die Methode an
    //     drei Stellen. In Lua stand dort `autorun/re4_vr_holster.lua` im
    //     Stack und der Aufruf ging durch; nativ waere der Stack leer und wir
    //     wuerden IHN blocken -- der Bogen liesse sich in Mercs nicht mehr
    //     weglegen. Dagegen die Marke __re4_barehand_ours_t, die holster
    //     unmittelbar vor jedem Aufruf setzt.
    if ((clock_now() - re4vr::lua_get_number("__re4_barehand_ours_t", -999.0)) < 0.05) {
        return false;
    }

    // Steht die Lua-VM gerade in einem Aufruf, hat eines unserer Scripte
    // gerufen -- dann geht es durch, genau wie in Lua.
    if (re4vr::lua_is_executing()) {
        return false;
    }

    return true;
}

void RE4VRMerc::install_bow_keep_hook() {
    if (m_hook_installed) {
        return;
    }

    m_hook_installed = true;

    auto* td = sdk::find_type_definition("chainsaw.PlayerEquipment");
    auto* m = td != nullptr
        ? td->get_method("requestEquipBareHand(System.Boolean, System.Boolean)")
        : nullptr;

    if (m == nullptr) {
        return;
    }

    // [SIGNATUR-FALLE] SKIP_ORIGINAL braucht bei nicht-void einen sauberen
    // Rueckgabewert -- sonst gibt HookManager weiter, was zufaellig in RAX
    // stand. Das Original ermittelt den Typ zur LAUFZEIT
    // (`bk_m:get_return_type():get_full_name()`) und schreibt bei nicht-void im
    // Post-Hook `sdk.to_ptr(0)`. Genau das hier, statt "ist ja void" zu
    // behaupten.
    {
        auto* rt = m->get_return_type();
        const auto rn = rt != nullptr ? rt->get_full_name() : std::string{};
        s_bow_keep_void = rn.empty() || rn == "System.Void";
    }

    g_hookman.add(
        m,
        [](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
            bool skip = false;

            try {
                skip = RE4VRMerc::get()->bow_keep_should_skip();
            } catch (...) {
                skip = false;
            }

            if (!skip) {
                return HookManager::PreHookResult::CALL_ORIGINAL;
            }

            re4vr::lua_set_number(
                "__re4_merc_bow_keep_blocked",
                re4vr::lua_get_number("__re4_merc_bow_keep_blocked", 0.0) + 1.0);

            s_bow_keep_skipped = true;

            return HookManager::PreHookResult::SKIP_ORIGINAL;
        },
        [](uintptr_t& ret, sdk::RETypeDefinition*, uintptr_t) {
            // Nur nach einem SKIP und nur bei nicht-void nullen -- sonst
            // bleibt der echte Rueckgabewert des Originals stehen, genau wie
            // Luas Durchreiche (`return ret`).
            if (s_bow_keep_skipped) {
                s_bow_keep_skipped = false;

                if (!s_bow_keep_void) {
                    ret = 0;
                }
            }
        });
}

// ============================================================================
// Config
// ============================================================================

void RE4VRMerc::load_cfg() {
    init_hud();

    const auto d = re4vr::json_load(CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    // Lua prueft je Schluessel `~= nil` und vergleicht dann `== true` --
    // ein Nicht-Boolean wird also zu false, nicht ignoriert.
    const auto b = [&](const char* key, bool cur) {
        const auto it = d.find(key);

        return it != d.end() && !it->is_null() ? (it->is_boolean() && it->get<bool>()) : cur;
    };
    // Luas tonumber() nimmt auch numerische Strings.
    const auto n = [&](const char* key, float cur) {
        const auto it = d.find(key);

        if (it == d.end()) {
            return cur;
        }

        if (it->is_number()) {
            return it->get<float>();
        }

        if (it->is_string()) {
            try {
                return std::stof(it->get<std::string>());
            } catch (...) {
                return cur;
            }
        }

        return cur;
    };

    m_wep_off_on = b("wep_off_on", m_wep_off_on);
    m_bow_pose_on = b("bow_pose_on", m_bow_pose_on);
    m_hide_2770 = b("hide_2770", m_hide_2770);
    m_grab_head_on = b("grab_head_on", m_grab_head_on);
    m_hide_timer_bg = b("hide_timer_bg", m_hide_timer_bg);
    m_bow_mirror_mode = static_cast<int>(n("bow_mirror_mode",
                                           static_cast<float>(m_bow_mirror_mode)));
    m_bow_pose_blend = n("bow_pose_blend", m_bow_pose_blend);

    if (const auto p = d.find("poses"); p != d.end() && p->is_object()) {
        m_bow_poses.clear();
        m_bow_mirror_cache.clear();
        // Rohstruktur unveraendert merken -- sie geht beim Speichern
        // wortgleich zurueck (s. m_bow_poses_raw im Header).
        m_bow_poses_raw = *p;

        for (const auto& [pname, pv] : p->items()) {
            if (!pv.is_object()) {
                continue;
            }

            const auto bones = pv.find("bones");

            if (bones == pv.end() || !bones->is_object()) {
                continue;
            }

            PoseBones pb;

            for (const auto& [bone, q] : bones->items()) {
                // Reihenfolge in der Aufnahme: w, x, y, z.
                if (q.is_array() && q.size() >= 4) {
                    pb[bone] = glm::quat{q[0].get<float>(), q[1].get<float>(),
                                         q[2].get<float>(), q[3].get<float>()};
                }
            }

            if (!pb.empty()) {
                m_bow_poses[pname] = pb;
            }
        }
    }

    // [MERCS_HUD] Groesse/Offset aller HUD-Teile.
    if (const auto h = d.find("hud"); h != d.end() && h->is_object()) {
        for (auto& [key, cfg] : m_hud) {
            const auto src = h->find(key);

            if (src == h->end() || !src->is_object()) {
                continue;
            }

            if (const auto it = src->find("on"); it != src->end() && !it->is_null()) {
                cfg.on = it->is_boolean() && it->get<bool>();
            }

            if (const auto it = src->find("scale"); it != src->end() && it->is_number()) {
                cfg.scale = it->get<float>();
            }

            if (const auto it = src->find("x"); it != src->end() && it->is_number()) {
                cfg.x = it->get<float>();
            }

            if (const auto it = src->find("y"); it != src->end() && it->is_number()) {
                cfg.y = it->get<float>();
            }

            // Nulllage nur fuer Namens-Eintraege (die anderen haben sie fest im
            // Code).
            if (cfg.go != nullptr) {
                const auto bx = src->find("bx");
                const auto by = src->find("by");

                if (bx != src->end() && bx->is_number() && by != src->end()
                    && by->is_number()) {
                    cfg.bx = bx->get<float>();
                    cfg.by = by->get<float>();
                }
            }
        }
    }

    if (const auto w = d.find("wep_off"); w != d.end() && w->is_object()) {
        for (const auto& [k, v] : w->items()) {
            if (!v.is_object()) {
                continue;
            }

            int32_t wid = 0;

            try {
                wid = std::stoi(k);
            } catch (...) {
                continue;
            }

            WepOff& o = wep_off_for(wid);
            const auto num = [&](const char* f) {
                const auto it = v.find(f);

                return (it != v.end() && it->is_number()) ? it->get<float>() : 0.0f;
            };

            o.px = num("px");
            o.py = num("py");
            o.pz = num("pz");
            o.rx = num("rx");
            o.ry = num("ry");
            o.rz = num("rz");
        }
    }
}

void RE4VRMerc::save_cfg() {
    nlohmann::json d;

    d["wep_off_on"] = m_wep_off_on;
    d["bow_pose_on"] = m_bow_pose_on;
    d["hide_2770"] = m_hide_2770;
    d["grab_head_on"] = m_grab_head_on;
    d["hide_timer_bg"] = m_hide_timer_bg;
    d["bow_mirror_mode"] = m_bow_mirror_mode;
    d["bow_pose_blend"] = m_bow_pose_blend;

    nlohmann::json wo = nlohmann::json::object();

    for (const auto& [wid, o] : m_wep_off) {
        nlohmann::json e;
        e["px"] = o.px;
        e["py"] = o.py;
        e["pz"] = o.pz;
        e["rx"] = o.rx;
        e["ry"] = o.ry;
        e["rz"] = o.rz;
        wo[std::to_string(wid)] = e;
    }

    d["wep_off"] = wo;

    // poses MUSS mitgeschrieben werden, sonst radiert der erste Slider-Klick
    // die kopierten Capture-Werte aus der JSON. Und zwar UNVERAENDERT: merc
    // liest die Posen nur, es aendert sie nie -- Felder wie `src_hand` und
    // `hand` muessen genau so wieder herauskommen, wie sie hineinkamen.
    d["poses"] = m_bow_poses_raw.is_object() ? m_bow_poses_raw
                                             : nlohmann::json::object();

    // [MERCS_HUD] generisch ueber die Tabelle, damit ein neuer Eintrag nicht
    // auch noch hier nachgetragen werden muss.
    nlohmann::json hud = nlohmann::json::object();

    for (const auto& [key, cfg] : m_hud) {
        nlohmann::json e;
        e["on"] = cfg.on;
        e["scale"] = cfg.scale;
        e["x"] = cfg.x;
        e["y"] = cfg.y;

        // [1:1] Lua schreibt `bx`/`by` fuer ALLE Eintraege -- auch fuer die
        // fuenf Behavior-Eintraege, deren Nulllage fest im Code steht. Beim
        // Laden werden sie fuer die dann wieder ignoriert (nur Namens-Eintraege
        // duerfen ihre gemessene Nulllage aus der Datei ziehen).
        if (cfg.bx.has_value() && cfg.by.has_value()) {
            e["bx"] = *cfg.bx;
            e["by"] = *cfg.by;
        }

        hud[key] = e;
    }

    d["hud"] = hud;

    re4vr::json_save(CFG_PATH, d);
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRMerc::on_initialize() {
    // Wie bei choke und motion: load_cfg schreibt und liest Lua-Globals, der
    // State entsteht aber erst spaeter.
    return Mod::on_initialize();
}

void RE4VRMerc::on_lua_state_created(sol::state& lua) {
    // [FUNKTIONS-GLOBALS] motion ruft beide direkt -- ohne sie passiert dort
    // exakt nichts (die Aufrufe sind nil-geprueft).
    lua["__re4_merc_apply_bow_pose"] = []() { RE4VRMerc::get()->apply_bow_pose(); };

    // Rueckgabe wie im Original: np, nr -- oder nil, nil fuer "nicht zustaendig".
    lua["__re4_merc_wep_apply"] = [](sol::this_state ts, sol::object wpos, sol::object wrot,
                                     sol::object wid, sol::object hand_rot)
        -> std::tuple<sol::object, sol::object> {
        sol::state_view sv{ts};
        const auto none = std::make_tuple(sol::object{sv, sol::lua_nil},
                                          sol::object{sv, sol::lua_nil});

        if (!wpos.is<glm::vec3>() || !wrot.is<glm::quat>()) {
            return none;
        }

        // Lua: `if not wid then return nil, nil end`
        if (!wid.is<double>()) {
            return none;
        }

        // Die Rotation aus DIESEM Pass; __vr_rh_rot ist nur der Fallback -- es
        // wird erst am Tick-Ende publiziert und ist beim Aufruf einen Pass alt.
        glm::quat hr{1.0f, 0.0f, 0.0f, 0.0f};

        if (hand_rot.is<glm::quat>()) {
            hr = hand_rot.as<glm::quat>();
        } else if (const auto g = re4vr::lua_get_quat("__vr_rh_rot"); g.has_value()) {
            hr = *g;
        } else {
            return none;
        }

        glm::vec3 p = wpos.as<glm::vec3>();
        glm::quat r = wrot.as<glm::quat>();

        if (!RE4VRMerc::get()->wep_apply(p, r, static_cast<int32_t>(wid.as<double>()), hr)) {
            return none;
        }

        return std::make_tuple(sol::make_object(sv, p), sol::make_object(sv, r));
    };

    // Der HUD-Block sichert die einmal gelesene Nulllage sofort.
    lua["__re4_merc_save_cfg"] = []() { RE4VRMerc::get()->save_cfg(); };

    lua["__re4_merc_bow_keep_hooked"] = true;
    lua["__re4_merc_bow_keep_blocked"] = 0.0;

    if (!m_cfg_loaded) {
        m_cfg_loaded = true;
        load_cfg();
    }

    install_bow_keep_hook();
}

void RE4VRMerc::on_lua_state_destroyed(sol::state& lua) {
    // In Lua laedt REFramework die Datei neu -- alle Locals starten leer.
    // Die Caches muessen mit, sonst schreiben wir nach dem Reset in Leichen.
    clear_hh();
    clear_all_meshes();

    for (auto& e : m_extra_mats) {
        drop(e.mesh);
    }

    m_extra_mats.clear();
    m_extra_set = false;
    m_extra_mats_off = false;

    for (auto& f : m_extra_furs) {
        drop(f);
    }

    m_extra_furs.clear();
    m_extra_furs_set = false;
    m_extra_scan_t = 0.0;

    for (auto& [k, h] : m_hud_cache) {
        drop(h);
    }

    m_hud_cache.clear();
    m_hud_names.clear();
    m_hud_scan_t = 0.0;

    // [1:1] Der Bow-Pin wird NICHT geloest. In Lua verschwindet `bowp` beim
    // Reload einfach -- der Bogen bleibt am R_Hand geparentet, bis die Engine
    // ihn beim naechsten Equip/Holster selbst umsetzt (genau das sagt der
    // Kommentar bei bow_unpin). Anders als das dauerhaft abgedunkelte Messer
    // in RE4VRChoke richtet das keinen bleibenden Schaden an, also bleibt es.
    drop(m_bowp_go);
    drop(m_bowp_tf);
    m_bowp_pinned = false;

    drop(m_merc_mgr);
    drop(m_campaign_mgr);
    drop(m_char_mgr);
    drop(m_br_hu);
    drop(m_grab_holder_mo);
    drop(m_dot.lsc);

    m_grab_holder_searched = false;
    m_grab_holder_name = "-";
    m_grab_hit_logged = false;
    m_br_since = 0.0;
    m_dot.key.reset();
    m_dot.tries = 0;
    m_dot.next_scan = 0.0;
    m_dot.body_weg = false;
    m_dot.aimed = false;
    m_dot.draw_frames = 0;

    // [1:1] In Lua legt der Reload die Datei neu an -- alle Config-Locals
    // starten wieder auf ihrem Compile-Default, bevor lh_load_cfg laeuft.
    m_wep_off.clear();
    m_bow_poses.clear();
    m_bow_mirror_cache.clear();
    m_bow_poses_raw = nlohmann::json{};
    m_wep_off_on = true;
    m_bow_pose_on = true;
    m_hide_2770 = true;
    m_grab_head_on = false;
    m_hide_timer_bg = true;
    m_bow_mirror_mode = 1;
    m_bow_pose_blend = 1.0f;
    m_bow_pin_on = true;
    m_hud.clear();   // wird von init_hud() mit den festen Nulllagen neu gebaut

    m_frames = 0;
    m_last_state.reset();
    m_last_kind.reset();
    m_merc_body_ok = false;
    m_round_gap = false;
    m_hide_enabled = true;
    m_bow_dbg = "noch nichts geschrieben";

    m_cfg_loaded = false;
}

void RE4VRMerc::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    // [BODY-EPOCH 2026-09-22] vor allen Cache-Nutzern dieses Ticks
    if (re4vr::body_epoch() != m_body_epoch) {
        m_body_epoch = re4vr::body_epoch();
        drop_body_caches();
    }

    ensure_types();

    update_bulletrush();
    // [BOW_PIN] Compound Bow haengt nativ am Handgelenk
    update_bow_pin();

    // [REIHENFOLGE 2026-08-05] Voll-Aus ZUERST, Kopf/Haar danach: beim
    // Verlassen von KS3/KS5 setzt das Voll-Aus alle Materialien wieder an --
    // apply_hide muss im SELBEN Frame das letzte Wort ueber Kopf/Haare haben,
    // sonst blitzen sie einen Frame lang auf.
    apply_full_hide();
    apply_hide();

    // [SZENENWEG 2026-09-09] Steht NACH apply_hide: dort laeuft der
    // Transform-Weg, der nachweislich am falschen Objekt schreibt. Dieser hier
    // trifft das, was EMV auch trifft. Sperren s. Kommentar bei SCENE_HIDE.
    {
        const int32_t kind =
            static_cast<int32_t>(re4vr::lua_get_number("__re4_merc_kind", -1.0));

        if (re4vr::lua_get_tribool("__re4_in_mercs") == 1
            && scene_hide_for(kind) != nullptr) {
            scan_scene_mats(kind);
            apply_scene_mats(m_hide_enabled && !show_head_now());
        } else if (m_kr_set) {
            // Mercs verlassen -> alles zurueckgeben, sonst blieben die
            // Materialien im naechsten Modus aus.
            apply_scene_mats(false);
        }
    }

    // sucht + setzt; der zweite, spaete Pass haengt an BeginRendering
    hud_apply();

    ++m_frames;

    if (m_frames % CHECK_EVERY != 0) {
        return;
    }

    std::optional<int32_t> cid{};
    bool in_mercs = detect(cid);

    // [SW_VS_MERCS 2026-08-04] ZWEITES Kriterium: der Body-GO des Spielers muss
    // einer der Mercs-Bodys sein. Ohne das meldet detect auch im
    // Separate-Ways-DLC "Mercenaries". Ist der Body gerade nicht lesbar
    // (Ladephase), bleibt der letzte Befund stehen.
    if (in_mercs) {
        const auto pb = get_player_body();

        if (pb.name_ok) {
            m_merc_body_ok = is_merc_body(pb.name);
        }

        in_mercs = m_merc_body_ok;
    } else {
        m_merc_body_ok = false;
    }

    re4vr::lua_set_bool("__re4_in_mercs", in_mercs);

    if (cid.has_value()) {
        re4vr::lua_set_number("__re4_merc_cid", static_cast<double>(*cid));
    } else {
        re4vr::lua_set_nil("__re4_merc_cid");
    }

    if (m_last_state != std::optional<bool>{in_mercs}) {
        m_last_state = in_mercs;
    }

    if (!in_mercs) {
        re4vr::lua_set_nil("__re4_merc_kind");
        re4vr::lua_set_nil("__re4_merc_body");
        // ausserhalb Mercs fassen wir NICHTS an
        clear_hh();
        clear_all_meshes();
        // [ARM_KEY] ausserhalb Mercs wieder Leons Preset
        re4vr::lua_set_nil("__vr_active_char");
        return;
    }

    const auto pb = get_player_body();

    if (pb.kind.has_value()) {
        re4vr::lua_set_number("__re4_merc_kind", static_cast<double>(*pb.kind));
    } else {
        re4vr::lua_set_nil("__re4_merc_kind");
    }

    if (pb.name_ok) {
        re4vr::lua_set_string("__re4_merc_body", pb.name);
    } else {
        re4vr::lua_set_nil("__re4_merc_body");
    }

    // [CACHE-LEICHEN 2026-08-09, Log-belegt] Beim Rundenwechsel meldet der Body
    // kurz NICHTS. Der Neuaufbau unten haengt aber an `hh_body ~= body` -- und
    // weil in der Luecke nichts passiert, ist der Name danach WIEDER derselbe:
    // die Bedingung ist falsch, die gecachten Renderer der alten Runde bleiben
    // stehen. Auf so eine Leiche zu schreiben wirft KEINEN Fehler, also greift
    // auch die pcall-Selbstheilung nicht. Deshalb die Luecke selbst als
    // Trigger nehmen.
    if (!pb.name_ok) {
        clear_hh();
        clear_all_meshes();

        // [RUNDEN-TOKEN 2026-08-09] Dieselbe Luecke ist der einzige
        // verlaessliche Hinweis auf "neue Runde". Nur EINMAL pro Luecke
        // hochzaehlen, nicht pro Frame.
        if (!m_round_gap) {
            m_round_gap = true;
            re4vr::lua_set_number("__re4_merc_round",
                                  re4vr::lua_get_number("__re4_merc_round", 0.0) + 1.0);
        }

        return;
    }

    m_round_gap = false;

    // [ARM_KEY 2026-08-06] Arm-IK pro Mercs-Charakter. arm_chain kann das
    // laengst -- nur hat den Wert NIE jemand gesetzt, also lief alles (auch
    // Krauser) mit Leons Werten. Fehlt ein Block in der JSON, faellt arm_chain
    // weiter auf "leon" zurueck.
    const auto* mc = pb.kind.has_value() ? merc_char(*pb.kind) : nullptr;
    re4vr::lua_set_string("__vr_active_char", mc != nullptr ? mc->arm_key : "leon");

    if (m_last_kind != pb.kind) {
        m_last_kind = pb.kind;
    }

    // Kopf/Haar-Meshes suchen, sobald sich der Body geaendert hat oder der
    // Cache verworfen wurde. Laeuft fuer JEDEN Mercs-Charakter -- der GO-Name
    // reicht; eine hinterlegte Hide-Liste ist nur die Zusatzsicherung.
    if (!m_hh_set || m_hh_body != pb.name) {
        collect_hh(pb.tf, mc != nullptr ? mc->hide : nullptr,
                   mc != nullptr ? mc->extra : nullptr);
        m_hh_body = m_hh_set ? pb.name : std::string{};

        // [MERC_FULLHIDE] am selben Punkt einsammeln: gleicher Body, gleiche
        // Lebensdauer
        collect_all_meshes(pb.tf);
    }
}

// ============================================================================
// Statusanzeige am DESKTOP (im Headset nicht lesbar).
// ============================================================================

void RE4VRMerc::hud_ui() {
    ImGui::Separator();
    ImGui::TextColored(col_abgr(0xFF66CCFF),
                       "HUD (nur Mercenaries): Groesse + Position");

    if (ImGui::Checkbox("Gui_ui2770 ausblenden##merc_2770", &m_hide_2770)) {
        save_cfg();
    }

    // [GRAPPLE-AUSNAHME 2026-08-25] Aus = wieder wie vorher (Kopf weg, auch in
    // diesem Griff). Default ist AUS, s. grapple_head_exception.
    if (ImGui::Checkbox("Kopf/Haare im Drittperson-Griff anlassen##merc_grab",
                        &m_grab_head_on)) {
        save_cfg();
    }

    if (ImGui::Checkbox("Hintergrund hinter dem Countdown aus##merc_tbg", &m_hide_timer_bg)) {
        save_cfg();
    }

    const std::array<std::pair<const char*, const char*>, 7> order{{
        {"timer", "Countdown oben"},
        {"score", "Punkte-Anzeige"},
        {"combo", "Combo-Anzeige"},
        {"total", "Gesamtpunktzahl"},
        {"gauge", "Balken unten"},
        {"ui2710", "Gui_ui2710"},
        {"ui2764", "Gui_ui2764"},
    }};

    const bool in_mercs = re4vr::lua_get_tribool("__re4_in_mercs") == 1;

    for (const auto& [key, name] : order) {
        auto* cfg = hud_get(key);

        if (cfg == nullptr) {
            continue;
        }

        char id[96]{};

        std::snprintf(id, sizeof(id), "%s anpassen##hud%s", name, key);

        if (ImGui::Checkbox(id, &cfg->on)) {
            save_cfg();
        }

        // [2026-08-09] Ohne Haken wird gar nicht gesucht -- dann steht hier
        // zwangslaeufig "nein" und die Slider koennen nichts bewirken. Das war
        // die eigentliche Ursache fuer "die Slider machen nichts".
        if (cfg->go != nullptr) {
            char buf[160]{};

            if (cfg->bx.has_value() && cfg->by.has_value()) {
                std::snprintf(buf, sizeof(buf),
                              "   ueber den GameObject-Namen (%s), Nulllage: %.0f / %.0f",
                              cfg->go, *cfg->bx, *cfg->by);
                ImGui::TextColored(col_abgr(0xFF00FF00), "%s", buf);
            } else {
                std::snprintf(buf, sizeof(buf),
                              "   ueber den GameObject-Namen (%s), Nulllage: noch nicht gelesen",
                              cfg->go);
                ImGui::TextColored(col_abgr(0xFF66CCFF), "%s", buf);
            }
        } else if (!cfg->on) {
            ImGui::TextColored(col_abgr(0xFF66CCFF),
                               "   gefunden: nein -- Haken ist aus, es wird nicht gesucht");
        } else if (!in_mercs) {
            ImGui::TextColored(col_abgr(0xFF66CCFF),
                               "   gefunden: nein -- wir sind nicht in Mercenaries");
        } else {
            const bool found = m_hud_cache.find(key) != m_hud_cache.end();
            ImGui::TextColored(found ? col_abgr(0xFF00FF00)
                                     : col_abgr(0xFF5555FF),
                               found ? "   gefunden: ja"
                                     : "   gefunden: nein (noch nicht in der Szene)");
        }

        std::snprintf(id, sizeof(id), "Groesse##hs%s", key);

        if (ImGui::DragFloat(id, &cfg->scale, 0.01f, 0.10f, 3.00f, "%.2f")) {
            save_cfg();
        }

        std::snprintf(id, sizeof(id), "X (rechts +)##hx%s", key);

        if (ImGui::DragFloat(id, &cfg->x, 1.0f, -1500.0f, 1500.0f, "%.0f")) {
            save_cfg();
        }

        std::snprintf(id, sizeof(id), "Y (runter +)##hy%s", key);

        if (ImGui::DragFloat(id, &cfg->y, 1.0f, -1500.0f, 1500.0f, "%.0f")) {
            save_cfg();
        }
    }
}

void RE4VRMerc::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - Mercenaries (DLC)")) {
        return;
    }

    init_hud();

    const bool on = re4vr::lua_get_tribool("__re4_in_mercs") == 1;
    ImGui::TextColored(on ? col_abgr(0xFF00FF00)
                          : col_abgr(0xFF888888),
                       on ? "Mercenaries: AKTIV" : "Mercenaries: nicht aktiv");

    {
        const auto cid = re4vr::lua_get_number_opt("__re4_merc_cid");
        char buf[128]{};
        std::snprintf(buf, sizeof(buf),
                      "CampaignID: %s   (-1 = Invalid/Mercs, 0 = Main/Kampagne)",
                      cid.has_value() ? std::to_string(static_cast<int>(*cid)).c_str()
                                      : "nil");
        ImGui::Text("%s", buf);
    }

    {
        const auto kind = re4vr::lua_get_number_opt("__re4_merc_kind");
        const std::string body = re4vr::lua_get_string("__re4_merc_body");
        char buf[192]{};
        std::snprintf(buf, sizeof(buf), "Charakter: KindID %s   Body '%s'",
                      kind.has_value() ? std::to_string(static_cast<int>(*kind)).c_str()
                                       : "nil",
                      body.empty() ? "nil" : body.c_str());
        ImGui::Text("%s", buf);
    }

    // [1:1] Dieser Haken wird NICHT gespeichert -- er ist in Lua eine reine
    // Laufzeit-Local ohne Config-Anbindung.
    ImGui::Checkbox("Kopf/Haare ausblenden (VR)##merc_hide", &m_hide_enabled);

    ImGui::Text("   erkannte Kopf/Haar-Meshes: %d", static_cast<int>(m_hh_meshes.size()));

    // [MERCS_HUD] Groesse + Position von Countdown und Balken
    hud_ui();

    // [BOW_POSE] Handposen des Compound Bow (Werte aus unserer eigenen merc.json)
    ImGui::Separator();

    if (ImGui::Checkbox("Compound-Bow-Handposen (rechts + linke Lazypose)##merc_bp",
                        &m_bow_pose_on)) {
        save_cfg();
    }

    {
        const bool have_r = m_bow_poses.count("compoundBOW") > 0;
        const bool have_l = m_bow_poses.count("compoundBOWLEFT") > 0;
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "   Posen in merc.json:  compoundBOW=%s   compoundBOWLEFT=%s",
                      have_r ? "ja" : "FEHLT", have_l ? "ja" : "FEHLT");
        ImGui::Text("%s", buf);
    }

    ImGui::Text("   zuletzt: %s", m_bow_dbg.c_str());

    if (ImGui::SliderInt("Spiegel-Variante (1-3)##merc_mm", &m_bow_mirror_mode, 1, 3)) {
        m_bow_mirror_cache.clear();
        save_cfg();
    }

    ImGui::Text("   1 = (w, x,-y,-z)   2 = (w,-x, y,-z)   3 = (w,-x,-y, z)");

    if (ImGui::DragFloat("Pose-Blend##merc_pb", &m_bow_pose_blend, 0.01f, 0.0f, 1.0f,
                         "%.2f")) {
        save_cfg();
    }

    // [MERC_WEP_OFFSET] Bewegt NUR die Waffe gegen die rechte Hand.
    ImGui::Separator();

    if (ImGui::Checkbox("Waffe-only Offset (Mercs-Waffen)##merc_wo", &m_wep_off_on)) {
        save_cfg();
    }

    const auto wid_now = re4vr::lua_get_number_opt("__vr_dbg_wep_id");

    if (wid_now.has_value()) {
        ImGui::Text("   aktuelle Waffe: %d", static_cast<int>(*wid_now));
    } else {
        ImGui::Text("   aktuelle Waffe: nil");
    }

    if (wid_now.has_value()) {
        WepOff& o = wep_off_for(static_cast<int32_t>(*wid_now));

        ImGui::TextColored(col_abgr(0xFF66CCFF),
                           "Waffe gegen die RECHTE Hand versetzen (Pos in m, Rot in Grad).\n"
                           "Die Hand bleibt stehen. Alles 0 = Hook inaktiv, dann gilt allein "
                           "motion.");

        bool dirty = false;
        dirty |= ImGui::DragFloat("Wpn Pos X##merc_px", &o.px, 0.002f, -1.0f, 1.0f, "%.3f");
        dirty |= ImGui::DragFloat("Wpn Pos Y##merc_py", &o.py, 0.002f, -1.0f, 1.0f, "%.3f");
        dirty |= ImGui::DragFloat("Wpn Pos Z##merc_pz", &o.pz, 0.002f, -1.0f, 1.0f, "%.3f");
        dirty |= ImGui::DragFloat("Wpn Rot X##merc_rx", &o.rx, 0.2f, -180.0f, 180.0f, "%.1f");
        dirty |= ImGui::DragFloat("Wpn Rot Y##merc_ry", &o.ry, 0.2f, -180.0f, 180.0f, "%.1f");
        dirty |= ImGui::DragFloat("Wpn Rot Z##merc_rz", &o.rz, 0.2f, -180.0f, 180.0f, "%.1f");

        if (ImGui::Button("Reset Waffen-Offset to 0##merc_reset")) {
            o = WepOff{};
            dirty = true;
        }

        if (dirty) {
            save_cfg();
        }
    }

    ImGui::TreePop();
}

#endif // RE4
