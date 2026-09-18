// ============================================================================
// RE4VRUi -- 1:1-Portierung von re4_vr_ui.lua. Siehe RE4VRUi.hpp fuer die
// Bausteine und die Reihenfolge im Mod-Vektor.
//
// Spezifikation: I:\LUATRANS\PORT_UI_SPEC.md
// ============================================================================
#if defined(RE4)

#include <algorithm>
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

#include "RE4VRUi.hpp"

#undef min
#undef max

namespace {
double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
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

::REManagedObject* type_of(const char* name) {
    auto* td = sdk::find_type_definition(name);

    return td != nullptr ? (::REManagedObject*)td->get_runtime_type() : nullptr;
}

// Control-Baum nach einem Namen absuchen, mit hartem Budget (80 Knoten,
// Tiefe 5) -- dieselbe Suche wie RE4VRBinding::find_ctl.
::REManagedObject* find_ctl(::REManagedObject* c, const char* want, int32_t depth,
                            int32_t& budget) {
    while (c != nullptr && budget < 80) {
        ++budget;

        if (obj_name_of(c) == want) {
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

// [ABGR] imgui.text_colored liest die Farbe als ABGR -- unterstes Byte = Rot.
ImVec4 col_abgr(uint32_t c) {
    return ImVec4{static_cast<float>(c & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 24) & 0xFF) / 255.0f};
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_ui.json";
constexpr int32_t FOREST_STAGE = 48000;

constexpr const char* HIDE_NAME = "Gui_ui3121";
constexpr const char* SCALE_NAME = "Gui_ui3101";
constexpr const char* BG_NAME = "AcBackGround";   // Karte UND Koffer
constexpr const char* BINO_GUI = "Gui_ui2110";
// So lange nach dem letzten Zeichnen gilt das Fernglas als offen.
constexpr double BINO_SEEN_SEC = 0.25;

constexpr int32_t VIEWTYPE_SCREEN = 0;
constexpr int32_t VIEWTYPE_WORLD = 1;

// NUR im Hauptmenue weg.
const std::unordered_set<std::string> MENU_NAMES{"Gui_ui0502", "Gui_ui0501"};

// GUIs, die im GANZEN Spiel gelten. Jeder Eintrag hat seinen eigenen Haken;
// der Schluessel ist der Name des Schalters in der JSON.
struct GlobalGui {
    const char* name;
    const char* key;
    const char* label;
};

const std::array<GlobalGui, 10> GLOBAL_GUIS{{
    {"Gui_ui2041", "hide_dot", "Mittel-Dot ausblenden (ganzes Spiel)"},
    {"Gui_ui2152", "hide_vignette", "Damage-Vignette ausblenden (ganzes Spiel)"},
    {"Gui_ui2151", "hide_vignette2", "Damage-Vignette2 ausblenden (ganzes Spiel)"},
    // [2026-08-25] Gui_ui2150 gehoert zur selben Familie: gemessen 23.08. ist
    // es die Damage-/Low-Health-Vignette, NICHT der Ausweich-Prompt.
    {"Gui_ui2150", "hide_vignette3", "Damage-Vignette3 ausblenden (ganzes Spiel)"},
    // [2026-08-21] 0501/0502 standen bisher nur in MENU_NAMES, waren also NUR
    // im Hauptmenue weg. Hier gelten sie im ganzen Spiel; der MENU_NAMES-Zweig
    // bleibt stehen, damit ohne Haken wieder die alte Regel greift.
    {"Gui_ui0501", "hide_0501", "Gui_ui0501 ausblenden (ganzes Spiel)"},
    {"Gui_ui0502", "hide_0502", "Gui_ui0502 ausblenden (ganzes Spiel)"},
    // [2026-08-24] Adas Variante der Karten-Ebene. Der `_AO`-Suffix ist Absicht.
    {"Gui_ui3121_AO", "hide_3121_ao",
     "Adas Karten-Ebene 3121_AO ausblenden (ganzes Spiel)"},
    {"Gui_ui3141_AO", "hide_3141_ao",
     "Adas Karten-Ebene 3141_AO ausblenden (ganzes Spiel)"},
    // [2026-08-29] "Baby Health" -- Startwert AN, die Anzeige ist damit aus.
    {"Gui_ui2031", "hide_baby_health", "Baby Health (2031) ausblenden (ganzes Spiel)"},
    // [2026-08-30] Karten-UI, das immer weg kann.
    {"Gui_ui3131", "hide_3131", "Karten-Ebene 3131 ausblenden (ganzes Spiel)"},
}};

// [SUB-EBENE 2026-09-08] Hier wird NICHT die ganze GUI unterdrueckt, sondern
// nur EIN Control in ihrem Baum unsichtbar gesetzt -- die GUI selbst zeichnet
// weiter. Beim Movie-Overlay ist genau das noetig: `bg` ist die schwarze
// Vollbild-Flaeche, `MovieTarget` daneben das eigentliche Video. Wuerde man
// FullScreenMovieGui_4K komplett ausblenden (GLOBAL_GUIS), waere auch das
// Video weg.
struct SubGui {
    const char* gui;    // GameObject-Name der GUI
    const char* ctl;    // Control-Name im Baum darunter
    const char* key;    // Schaltername in der JSON
    const char* label;
};

const std::array<SubGui, 1> SUB_GUIS{{
    {"FullScreenMovieGui_4K", "bg", "hide_movie_bg",
     "Movie-Hintergrund (FullScreenMovieGui_4K/bg) ausblenden"},
}};

// Welche Ebenen der Pin erfasst und in welcher Reihenfolge (0 = am weitesten
// hinten). [2026-08-11] 3140/3141 gehoeren zum Kartenkoerper und stehen deshalb
// hinten bei 3120/3121; der Umriss steht ueber dem Koerper, unter Funden und
// Navkreuz.
struct GlueLayer {
    const char* name;
    int order;
    const char* label;
};

const std::array<GlueLayer, 8> GLUE_LAYERS{{
    {"Gui_ui3120", 0, "Karten-Ebene (3120)"},
    {"Gui_ui3121", 1, "Karte + Dekoration (3121)"},
    {"Gui_ui3140", 2, "Karten-Ebene (3140)"},
    {"Gui_ui3141", 3, "Karten-Ebene (3141)"},
    {"Gui_ui3104", 4, "Kartenumriss (3104)"},
    {"Gui_ui3103", 5, "Gui_ui3103"},
    {"Gui_ui3101", 6, "Funde (3101)"},
    {"Gui_ui3100", 7, "Navigationskreuz (3100)"},
}};

// [2026-08-11] Karten-Ebenen, die man ganz weglassen kann.
struct MapHideEntry {
    const char* name;
    const char* label;
};

const std::array<MapHideEntry, 3> MAP_HIDE{{
    {"Gui_ui3140", "Karten-Ebene 3140 ausblenden"},
    {"Gui_ui3141", "Karten-Ebene 3141 ausblenden"},
    {"Gui_ui3131_AO", "Adas Karten-Ebene 3131_AO ausblenden (Separate Ways)"},
}};

// [VIEWTYPE] Nur 3120 hat noch einen Haken: die uebrigen Ebenen SIND schon
// Screen (Messbefund 10.08.), dort kann der Haken nichts bewirken.
struct MapView {
    const char* name;
    const char* key;
    const char* label;
};

const std::array<MapView, 1> MAP_VIEWS{{
    {"Gui_ui3120", "vt_3120", "Karte-Ebene (3120) auf Screen"},
}};
} // namespace

std::shared_ptr<RE4VRUi>& RE4VRUi::get() {
    static auto inst = std::make_shared<RE4VRUi>();
    return inst;
}

// ============================================================================
// (1) Fork-Erkennung
// ============================================================================
// In Lua wurde geprueft, ob `vrmod` die Getter ueberhaupt kennt -- auf einem
// vanilla REFramework fehlen sie. Nativ sind wir IMMER unser Fork, die
// Methoden existieren also. Die Globals werden trotzdem gesetzt: scope und
// weapons lesen sie.
void RE4VRUi::run_probe() {
    if (m_probe_done) {
        return;
    }

    m_probe_done = true;

    re4vr::lua_set_bool("__re4_fork_mono", true);
    re4vr::lua_set_bool("__re4_fork_gui_matrix", true);
    re4vr::lua_set_bool("__re4_fork_canvas", true);
    re4vr::lua_set_bool("__re4_fork_ok", true);
}

// ============================================================================
// (2) Mono-Broker
// ============================================================================

void RE4VRUi::on_fork_check() {
    run_probe();
}

void RE4VRUi::mono_request(const std::string& id, bool on) {
    if (id.empty()) {
        return;
    }

    if (on) {
        m_mono_reqs.insert(id);
    } else {
        m_mono_reqs.erase(id);
    }
}

void RE4VRUi::mono_apply() {
    if (m_mono_fail) {
        return;
    }

    const bool want = !m_mono_reqs.empty();

    if (want == m_mono_state) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    try {
        vr->set_mono_rendering(want);
    } catch (...) {
        // Wie in Lua: einmal gescheitert -> nicht jeden Frame nachbohren.
        m_mono_fail = true;
        return;
    }

    m_mono_state = want;
}

// ============================================================================
// (3) Die Schalter -- alle idempotent
// ============================================================================

bool RE4VRUi::is_supported() {
    if (m_supported.has_value()) {
        return *m_supported;
    }

    m_supported = VR::get().get() != nullptr;

    return *m_supported;
}

void RE4VRUi::apply_override(bool want) {
    if (want == m_state.applied) {
        return;
    }

    if (auto* vr = VR::get().get(); vr != nullptr) {
        vr->set_gui_projection_matrix_override_disabled(want);
    }

    m_state.applied = want;
}

void RE4VRUi::apply_elem(bool want) {
    if (!m_elem_supported.has_value()) {
        m_elem_supported = VR::get().get() != nullptr;
    }

    if (!*m_elem_supported || want == m_state.elem) {
        return;
    }

    if (auto* vr = VR::get().get(); vr != nullptr) {
        vr->set_gui_element_override_disabled(want);
    }

    m_state.elem = want;
}

// Mono wird NICHT selbst geschaltet, sondern beim Broker angemeldet -- sonst
// nimmt ein Nutzer dem anderen den Zustand weg.
void RE4VRUi::apply_mono(bool want) {
    m_state.mono = want;
    mono_request("map", want);
}

void RE4VRUi::apply_canvas(bool want) {
    if (want == m_state.canvas) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    vr->set_flatscreen_overlay(want);
    m_state.canvas = want;

    // Groesse nur beim Einschalten setzen; die Slider schieben sie live nach.
    if (want) {
        vr->set_flatscreen_overlay_width(m_canvas_width);
        vr->set_flatscreen_overlay_distance(m_canvas_distance);
    }
}

void RE4VRUi::apply_suspend(bool want) {
    if (!m_suspend_supported.has_value()) {
        m_suspend_supported = VR::get().get() != nullptr;
    }

    if (!*m_suspend_supported || want == m_state.suspend) {
        return;
    }

    if (auto* vr = VR::get().get(); vr != nullptr) {
        vr->set_vr_suspended(want);
    }

    m_state.suspend = want;
}

void RE4VRUi::apply_mapglue(bool want) {
    if (!m_glue_supported.has_value()) {
        m_glue_supported = VR::get().get() != nullptr;
    }

    if (!*m_glue_supported || want == m_state.mapglue) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    vr->set_map_face_glue(want);
    m_state.mapglue = want;

    if (want) {
        vr->set_map_glue_distance(m_glue_distance);
    }
}

// Liste jeden Frame durchschreiben, solange gepinnt wird: ein Haken wirkt damit
// sofort, ohne Sonderbehandlung fuer "waehrend die Karte offen ist".
void RE4VRUi::push_glue_layers() {
    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    for (const auto& g : GLUE_LAYERS) {
        const auto it = m_glue_on.find(g.name);
        const bool on = it != m_glue_on.end() ? it->second : true;
        const uint32_t h = utility::hash(std::string_view{g.name});

        if (on) {
            vr->set_glue_gui_hash(h, g.order);
        } else {
            vr->remove_glue_gui_hash(h);
        }
    }
}

// [ZEIGESTRAHL-PITCH] Nur bei Aenderung schreiben, damit der ImGui-Slider im
// Fork nicht bei jedem Frame ueberschrieben wird.
void RE4VRUi::apply_ptr_pitch() {
    if (m_ptr_pitch_ok == std::optional<bool>{false}) {
        return;
    }

    auto* vr = VR::get().get();

    if (vr == nullptr) {
        return;
    }

    if (!m_ptr_pitch_ok.has_value()) {
        m_ptr_pitch_ok = true;
    }

    if (m_ptr_pitch_sent == std::optional<float>{m_ptr_pitch}) {
        return;
    }

    vr->set_overlay_pointer_pitch(m_ptr_pitch);
    m_ptr_pitch_sent = m_ptr_pitch;
}

// ============================================================================
// Zustandsabfragen -- Singletons NICHT dauerhaft halten: nach einem
// Savegame-Load kann die gecachte Instanz tot sein.
// ============================================================================

bool RE4VRUi::is_map_gui_open() {
    if (!re4vr::obj_ok(m_map_manager)) {
        m_map_manager = sdk::get_managed_singleton<::REManagedObject>("chainsaw.MapManager");
    }

    if (m_map_manager == nullptr) {
        return false;
    }

    // Methodenname EXAKT so: klein anfangend, KEIN "get_" davor.
    // "get_IsMapGuiOpen" existiert nicht (live geprueft 2026-08-07) -- der Call
    // warf dann jeden Frame und die Karte galt immer als zu.
    const auto open = opt_bool(m_map_manager, "isMapGuiOpen");

    if (!open.has_value()) {
        m_map_manager = nullptr;   // Instanz war eine Leiche
        return false;
    }

    return *open;
}

// [INVENTAR 2026-08-09] AcBackGround stoert auch im Koffer.
bool RE4VRUi::is_inventory_open() {
    if (!re4vr::obj_ok(m_case_manager)) {
        m_case_manager =
            sdk::get_managed_singleton<::REManagedObject>("chainsaw.AttacheCaseManager");
    }

    if (m_case_manager == nullptr) {
        return false;
    }

    const auto busy = opt_bool(m_case_manager, "get_IsAttacheCaseBusy");

    if (!busy.has_value()) {
        m_case_manager = nullptr;
        return false;
    }

    return *busy;
}

// [MAINMENUE 2026-08-09] Der Lock steht auch, wenn Karte oder Koffer offen sind
// -- die sind ja ebenfalls Menues. Deshalb zaehlt als Hauptmenue: Lock steht
// UND weder Karte noch Koffer offen.
bool RE4VRUi::is_main_menu_open() {
    if (m_state.map_open || m_state.inv_open) {
        return false;
    }

    if (!re4vr::obj_ok(m_gui_manager)) {
        m_gui_manager = sdk::get_managed_singleton<::REManagedObject>("chainsaw.GuiManager");
    }

    if (m_gui_manager == nullptr) {
        return false;
    }

    const auto locked = opt_bool(m_gui_manager, "get_hasOccupiedPauseMenuSystemLock");

    if (!locked.has_value()) {
        m_gui_manager = nullptr;
        return false;
    }

    return *locked;
}

// [SCOPE-RETICLE 2026-08-28] Gui_ui2040 ist EIN Objekt mit zwei Rollen: ohne
// Scope das Waffen-Reticle, mit MONTIERTEM Scope der Zoom-Hinweis mitten im
// Blickfeld.
//
// ERKENNUNG: `__re4_scope_id` aus re4_vr_weapons.lua -- die Ground-Truth des
// Projekts. Eine EIGENE Erkennung ueber ein "ScopeCamera"-GameObject ist
// widerlegt: das existiert AUCH OHNE montiertes Scope (die LE 5 hatte es blank
// in der Hand, das Reticle war dadurch dauerhaft weg).
//
// FAIL-SAFE: ausgeblendet wird nur, wenn die ItemID wirklich da ist.
bool RE4VRUi::scopehide_attached() {
    // [TYP 04.09.2026] `__re4_scope_id` ist ein STRING, keine Zahl:
    // re4_vr_weapons.lua:195 mappt die ItemID auf "normal"/"thermal"/"hipower"
    // und setzt sonst nil. Ein lua_get_number_opt liefert darauf IMMER nullopt
    // -- der ganze Reticle-Block waere tot und der Zoom-Hinweis bliebe mit
    // montiertem Scope mitten im Blickfeld stehen.
    // Lua prueft `now ~= nil`; leer entspricht hier nil.
    const bool on = !re4vr::lua_get_string("__re4_scope_id").empty();

    if (on != m_scopehide_seen) {
        m_scopehide_seen = on;
    }

    return on;
}

// ============================================================================
// (4) GUI-Zugriff
// ============================================================================
// Root-Control einer GUI: GameObject -> via.gui.GUI -> View -> erstes Kind.
// Bewusst OHNE Cache: die Karte baut ihre GUIs bei jedem Oeffnen neu auf, ein
// gehaltenes Control waere danach eine Leiche.

::REManagedObject* RE4VRUi::gui_view(::REManagedObject* go) {
    auto* td = type_of("via.gui.GUI");

    if (td == nullptr || go == nullptr) {
        return nullptr;
    }

    auto* comp = re4vr::call_safe<::REManagedObject*>(go, "getComponent(System.Type)", td);

    return comp != nullptr ? re4vr::call_safe<::REManagedObject*>(comp, "get_View") : nullptr;
}

::REManagedObject* RE4VRUi::gui_root_control(::REManagedObject* go) {
    auto* view = gui_view(go);

    return view != nullptr ? re4vr::call_safe<::REManagedObject*>(view, "get_Child") : nullptr;
}

// [REPARATUR 2026-08-11] Schreibt EINEN bestimmten ViewType absolut auf eine
// Ebene und vergisst alles Gemerkte. Getrennt von apply_viewtype, weil es fuer
// JEDEN Namen gilt -- auch fuer die ohne eigenen Haken.
//
// Warum es das braucht: der Notaus schrieb frueher pauschal World. Fuer 3104
// war das FALSCH (er ist original Screen), und weil der Fork beim Zeichnen den
// vorgefundenen Zustand als "Original" merkt, blieb der falsche Wert kleben --
// der Umriss war auch mit ausgeschaltetem Pin weg.
bool RE4VRUi::force_viewtype(::REManagedObject* go, const std::string& name) {
    const auto it = m_vt_force.find(name);

    if (it == m_vt_force.end()) {
        return false;
    }

    const int32_t target = it->second;

    if (auto* view = gui_view(go); view != nullptr) {
        call_pcall(view, "set_ViewType", target);
    }

    m_vt_orig.erase(name);
    m_vt_force.erase(it);

    return true;
}

void RE4VRUi::apply_viewtype(::REManagedObject* go, const std::string& name, bool want) {
    auto* view = gui_view(go);

    if (view == nullptr) {
        m_vt_seen[name] = "keine View";
        return;
    }

    // Ist-Wert JEDEN Frame lesen und merken: nur so sieht man in der
    // Statuszeile, ob ein Schreibversuch ueberhaupt haften bleibt.
    const auto cur = opt_int(view, "get_ViewType");

    m_vt_seen[name] = cur.has_value() ? std::to_string(*cur) : std::string{"kein ViewType"};

    if (!cur.has_value()) {
        return;   // Build/Typ kennt es nicht -> Finger weg
    }

    if (want) {
        if (m_vt_orig.find(name) == m_vt_orig.end()) {
            m_vt_orig[name] = *cur;
        }

        if (*cur != VIEWTYPE_SCREEN) {
            call_pcall(view, "set_ViewType", VIEWTYPE_SCREEN);
        }
    } else if (const auto o = m_vt_orig.find(name); o != m_vt_orig.end()) {
        call_pcall(view, "set_ViewType", o->second);
        m_vt_orig.erase(o);
    }
}

// ============================================================================
// (5) [WALDMENUE 2026-08-28] Das Hauptmenue mit der Waldszene erzwingen
// ============================================================================
// 48000 ist die Dorf-/Waldstage; das Spiel waehlt sonst je nach Fortschritt
// eine andere Kulisse. Drei Unterschiede zum Vorlagen-Script, alle absichtlich:
//   * der Manager existiert beim Laden nicht immer -> getickt weiterversuchen
//   * die ORIGINALWERTE werden beim ersten Treffer gemerkt -> der Haken laesst
//     sich auch wieder ausschalten
//   * gedrosselt auf 1x/Sekunde
// HINWEIS: StartupScene liest das Spiel beim Hochfahren -- greift der Haken
// nicht sofort, wirkt er spaetestens nach dem naechsten Spielstart.
void RE4VRUi::forest_apply(bool force) {
    if ((clock_now() - m_forest_last_t) < 1.0 && !force) {
        return;
    }

    m_forest_last_t = clock_now();

    auto* mgr =
        sdk::get_managed_singleton<::REManagedObject>("chainsaw.MainMenuBackgroundManager");

    if (mgr == nullptr) {
        return;
    }

    if (!m_forest_orig.has_value()) {
        const auto v = re4vr::get_field_int(mgr, "MainMenuStage_Village");
        const auto st = re4vr::get_field_int(mgr, "StartupScene");

        if (!v.has_value() || !st.has_value()) {
            return;
        }

        m_forest_orig = ForestOrig{*v, *st};
    }

    const int32_t want_v = m_forest_menu ? FOREST_STAGE : m_forest_orig->village;
    const int32_t want_st = m_forest_menu ? FOREST_STAGE : m_forest_orig->startup;

    const auto now_v = re4vr::get_field_int(mgr, "MainMenuStage_Village");
    const auto now_st = re4vr::get_field_int(mgr, "StartupScene");

    const auto set_int = [&](const char* field, int32_t value) {
        auto* td = utility::re_managed_object::get_type_definition(mgr);
        auto* f = td != nullptr ? td->get_field(field) : nullptr;
        auto* p = f != nullptr ? f->get_data_raw(mgr, false) : nullptr;

        if (p != nullptr) {
            *reinterpret_cast<int32_t*>(p) = value;
        }
    };

    if (now_v != std::optional<int32_t>{want_v}) {
        set_int("MainMenuStage_Village", want_v);
    }

    if (now_st != std::optional<int32_t>{want_st}) {
        set_int("StartupScene", want_st);
    }

    if (m_forest_applied != std::optional<bool>{m_forest_menu}) {
        m_forest_applied = m_forest_menu;
    }
}

// ============================================================================
// Config -- VERZOEGERT gespeichert (0,5 s nach der letzten Aenderung), damit
// das Ziehen eines Sliders nicht pro Frame eine Datei schreibt.
// ============================================================================

void RE4VRUi::load_cfg() {
    // Startwerte, die auch ohne Datei gelten.
    for (const auto& g : GLOBAL_GUIS) {
        m_global_hide[g.key] = true;
    }

    for (const auto& s : SUB_GUIS) {
        m_global_hide[s.key] = true;
    }

    for (const auto& v : MAP_VIEWS) {
        m_vt_on[v.key] = false;
    }

    // [KAPUTTE LOCALS] Compile-Defaults -- diese drei kommen NIE aus der Datei.
    m_map_hide["Gui_ui3140"] = false;
    m_map_hide["Gui_ui3141"] = false;
    m_map_hide["Gui_ui3131_AO"] = true;

    for (const auto& g : GLUE_LAYERS) {
        m_glue_on[g.name] = true;
    }

    m_bino_distance = 1.0f;

    const auto d = re4vr::json_load(CFG_PATH);

    if (!d.is_object()) {
        return;
    }

    const auto b = [&](const char* key, bool cur) {
        const auto it = d.find(key);

        return (it != d.end() && it->is_boolean()) ? it->get<bool>() : cur;
    };
    const auto n = [&](const char* key, float cur) {
        const auto it = d.find(key);

        return (it != d.end() && it->is_number()) ? it->get<float>() : cur;
    };

    m_ptr_pitch = n("ptr_pitch", m_ptr_pitch);
    m_forest_menu = b("forest_menu", m_forest_menu);
    m_opt.gui_matrix = b("gui_matrix", m_opt.gui_matrix);
    m_opt.gui_elem = b("gui_elem", m_opt.gui_elem);
    m_opt.mono = b("mono", m_opt.mono);
    m_opt.canvas = b("canvas", m_opt.canvas);
    m_opt.suspend = b("suspend", m_opt.suspend);
    m_opt.mapglue = b("mapglue", m_opt.mapglue);
    m_glue_distance = n("glue_distance", m_glue_distance);
    m_glue_gap = n("glue_gap", m_glue_gap);
    m_opt.binoglue = b("binoglue", m_opt.binoglue);

    // [KAPUTTE LOCALS] `map_hide`, `glue_on` und `bino_distance` werden hier
    // BEWUSST NICHT gelesen -- in Lua trifft der Ladeblock ein Global, und die
    // Schluessel entstehen deshalb gar nicht erst in der Datei (belegt: sie
    // fehlen in der Live-JSON). Wer sie hier laedt, aendert getestetes
    // Verhalten.

    // [VIEWTYPE] Erst der alte Sammel-Haken (einmalige Migration: er stand fuer
    // "alle vier"), danach die neuen Einzelhaken -- die gewinnen.
    if (const auto it = d.find("viewtype");
        it != d.end() && it->is_boolean() && it->get<bool>()) {
        for (auto& [k, v] : m_vt_on) {
            v = true;
        }
    }

    if (const auto vt = d.find("vt_on"); vt != d.end() && vt->is_object()) {
        for (const auto& [k, v] : vt->items()) {
            if (v.is_boolean() && m_vt_on.find(k) != m_vt_on.end()) {
                m_vt_on[k] = v.get<bool>();
            }
        }
    }

    m_canvas_width = n("canvas_width", m_canvas_width);
    m_canvas_distance = n("canvas_distance", m_canvas_distance);
    m_hide_3121 = b("hide_3121", m_hide_3121);
    m_hide_bg = b("hide_bg", m_hide_bg);

    if (const auto gh = d.find("global_hide"); gh != d.end() && gh->is_object()) {
        for (const auto& [k, v] : gh->items()) {
            if (v.is_boolean() && m_global_hide.find(k) != m_global_hide.end()) {
                m_global_hide[k] = v.get<bool>();
            }
        }
    }

    m_ui3101_scale = n("ui3101_scale", m_ui3101_scale);
}

void RE4VRUi::save_cfg() {
    nlohmann::json d;

    d["gui_matrix"] = m_opt.gui_matrix;
    d["gui_elem"] = m_opt.gui_elem;
    d["mono"] = m_opt.mono;
    d["canvas"] = m_opt.canvas;
    d["suspend"] = m_opt.suspend;
    d["mapglue"] = m_opt.mapglue;
    d["glue_distance"] = m_glue_distance;
    d["glue_gap"] = m_glue_gap;
    d["binoglue"] = m_opt.binoglue;

    // [KAPUTTE LOCALS] `map_hide`, `glue_on` und `bino_distance` werden NICHT
    // geschrieben -- in Lua liest save_cfg dort ein nil, und json.dump_file
    // laesst nil-Werte weg. Genau deshalb fehlen die drei in der Live-JSON.
    // Tauchen sie nach dem Port auf, ist der Port falsch.

    nlohmann::json vt = nlohmann::json::object();

    for (const auto& [k, v] : m_vt_on) {
        vt[k] = v;
    }

    d["vt_on"] = vt;

    d["canvas_width"] = m_canvas_width;
    d["canvas_distance"] = m_canvas_distance;
    d["hide_3121"] = m_hide_3121;
    d["hide_bg"] = m_hide_bg;

    nlohmann::json gh = nlohmann::json::object();

    for (const auto& [k, v] : m_global_hide) {
        gh[k] = v;
    }

    d["global_hide"] = gh;

    d["ui3101_scale"] = m_ui3101_scale;
    d["ptr_pitch"] = m_ptr_pitch;
    d["forest_menu"] = m_forest_menu;

    re4vr::json_save(CFG_PATH, d);
}

// ============================================================================
// Mod-Anbindung
// ============================================================================

std::optional<std::string> RE4VRUi::on_initialize() {
    return Mod::on_initialize();
}

void RE4VRUi::on_lua_state_created(sol::state& lua) {
    // [FUNKTIONS-GLOBALS] Beide werden von fremdem Code gerufen.
    // __re4_mono_request: RE4VRFirstPerson (nativ) und re4_vr_weapons.lua.
    lua["__re4_mono_request"] = [](const std::string& id, sol::object on) {
        RE4VRUi::get()->mono_request(id, on.is<bool>() && on.as<bool>());
    };

    // Liefert das Ergebnis und stoesst die Erkennung an, falls noch nicht durch.
    lua["__re4_fork_check"] = []() {
        RE4VRUi::get()->on_fork_check();

        return std::make_tuple(re4vr::lua_get_bool("__re4_fork_ok", false),
                               re4vr::lua_get_bool("__re4_fork_mono", false),
                               re4vr::lua_get_bool("__re4_fork_gui_matrix", false));
    };

    // Startwerte bewusst false, nicht nil: wer die Globals abfragt, bevor die
    // Erkennung durch ist, bekommt den SICHEREN Fall = bisheriger Weg.
    re4vr::lua_set_bool("__re4_fork_ok", false);
    re4vr::lua_set_bool("__re4_fork_mono", false);
    re4vr::lua_set_bool("__re4_fork_gui_matrix", false);
    re4vr::lua_set_bool("__re4_fork_canvas", false);

    if (!m_cfg_loaded) {
        m_cfg_loaded = true;
        load_cfg();
    }
}

void RE4VRUi::on_lua_state_destroyed(sol::state& lua) {
    // Beim Reload nicht mit abgeschaltetem Override, im Mono oder auf der
    // Leinwand stehenbleiben.
    apply_override(false);
    apply_elem(false);
    apply_mono(false);
    apply_canvas(false);
    apply_suspend(false);
    apply_mapglue(false);

    // Alle Mono-Anforderungen fallen lassen und Mono sauber ausschalten.
    m_mono_reqs.clear();
    mono_apply();

    m_state.map_open = false;
    m_state.inv_open = false;
    m_state.menu_open = false;

    m_map_manager = nullptr;
    m_case_manager = nullptr;
    m_gui_manager = nullptr;

    m_probe_done = false;
    m_cfg_loaded = false;

    // [1:1] In Lua sind das alles LOCALS, die beim Neuladen der Datei frisch
    // entstehen. Besonders `vt_orig`: der Notaus-Knopf ist genau darauf gebaut
    // ("der naechste Durchlauf merkt sich Screen als Original").
    m_vt_orig.clear();
    m_vt_seen.clear();
    m_vt_force.clear();

    // Nach dem Reset wird der JSON-Wert erneut in den Fork geschrieben --
    // sonst holt ein "Reset Scripts" ihn nicht mehr zurueck, wenn am
    // Fork-Slider gedreht wurde.
    m_ptr_pitch_sent.reset();
    m_ptr_pitch_ok.reset();

    m_supported.reset();
    m_elem_supported.reset();
    m_suspend_supported.reset();
    m_glue_supported.reset();

    m_bino_seen_t.reset();
    m_scopehide_seen = false;
    m_forest_orig.reset();
    m_forest_applied.reset();
    m_forest_last_t = 0.0;
    m_cfg_dirty_t.reset();
}

void RE4VRUi::on_frame() {
    if (re4vr::mods_gated()) {
        return;
    }

    run_probe();
    mono_apply();

    forest_apply(false);
    apply_ptr_pitch();

    // Speichern VOR der Fork-Pruefung: die Haken sollen auch dann erhalten
    // bleiben, wenn dieses REFramework den Projektions-Schalter nicht kennt.
    if (m_cfg_dirty_t.has_value() && (clock_now() - *m_cfg_dirty_t) > 0.5) {
        m_cfg_dirty_t.reset();
        save_cfg();
    }

    if (!is_supported()) {
        return;
    }

    m_state.map_open = is_map_gui_open();
    m_state.inv_open = is_inventory_open();
    m_state.menu_open = is_main_menu_open();

    // Soll-Zustand JEDEN Frame ableiten statt nur auf der Flanke zu schalten:
    // so wirkt ein Haken sofort, auch waehrend die Karte schon offen ist.
    apply_override(m_state.map_open && m_opt.gui_matrix);
    apply_elem(m_state.map_open && m_opt.gui_elem);
    apply_mono(m_state.map_open && m_opt.mono);
    apply_canvas(m_state.map_open && m_opt.canvas);
    apply_suspend(m_state.map_open && m_opt.suspend);

    // [FERNGLAS 2026-08-11] Zweiter Pin-Fall. Beide teilen sich den
    // Fork-Schalter, aber jeder bringt seine eigene Distanz und Liste mit;
    // gleichzeitig offen sind sie nie. Karte hat Vorrang.
    const bool bino_up = m_opt.binoglue && m_bino_seen_t.has_value()
        && (clock_now() - *m_bino_seen_t) < BINO_SEEN_SEC;
    const bool map_pin = m_state.map_open && m_opt.mapglue;

    apply_mapglue(map_pin || bino_up);

    if (m_state.mapglue) {
        auto* vr = VR::get().get();

        if (vr != nullptr) {
            if (map_pin) {
                vr->set_map_glue_distance(m_glue_distance);
                vr->set_map_glue_layer_gap(m_glue_gap);
                push_glue_layers();
                vr->remove_glue_gui_hash(utility::hash(std::string_view{BINO_GUI}));
            } else {
                vr->set_map_glue_distance(m_bino_distance);
                vr->set_glue_gui_hash(utility::hash(std::string_view{BINO_GUI}), 0);
            }
        }
    }

    // Slider live nachziehen, solange die Leinwand laeuft.
    if (m_state.canvas) {
        if (auto* vr = VR::get().get(); vr != nullptr) {
            vr->set_flatscreen_overlay_width(m_canvas_width);
            vr->set_flatscreen_overlay_distance(m_canvas_distance);
        }
    }
}

// Nur die benannten GUIs anfassen -- jedes andere Element geht unveraendert
// durch. Die REIHENFOLGE der Zweige ist tragend, s. PORT_UI_SPEC Abschnitt 6.
bool RE4VRUi::on_pre_gui_draw_element(::REComponent* element, void* context) {
    if (re4vr::mods_gated()) {
        return true;
    }

    auto* go = re4vr::call_safe<::REManagedObject*>((::REManagedObject*)element,
                                                    "get_GameObject");

    if (go == nullptr) {
        return true;
    }

    const std::string name = obj_name_of(go);

    if (name.empty()) {
        return true;
    }

    // [MITTEL-DOT 2026-08-10] Diese Namen gelten im GANZEN Spiel -- deshalb
    // stehen sie VOR dem Zustands-Gate.
    for (const auto& gg : GLOBAL_GUIS) {
        if (name == gg.name) {
            const auto it = m_global_hide.find(gg.key);

            return !(it != m_global_hide.end() && it->second);
        }
    }

    // [SUB-EBENE] Nur EIN Control im Baum stilllegen, die GUI selbst zeichnet
    // weiter (sonst waere beim Movie auch das Video weg). Steht wie die
    // GLOBAL_GUIS vor dem Zustands-Gate -> gilt im ganzen Spiel. Wird jeden
    // Frame neu gesetzt, weil die Engine das Control beim Aufbau wieder
    // sichtbar macht; bei ausgeschaltetem Haken fassen wir es NICHT an.
    for (const auto& sg : SUB_GUIS) {
        if (name != sg.gui) {
            continue;
        }

        const auto it = m_global_hide.find(sg.key);

        if (it != m_global_hide.end() && it->second) {
            if (auto* root = gui_root_control(go); root != nullptr) {
                int32_t budget = 0;

                if (auto* ctl = find_ctl(root, sg.ctl, 0, budget); ctl != nullptr) {
                    re4vr::call_safe<void*>(ctl, "set_Visible", false);
                }
            }
        }

        return true;
    }

    // [FERNGLAS] Vor jedem Zustands-Gate: es hat mit Karte, Koffer und
    // Hauptmenue nichts zu tun, es meldet sich einfach durchs Gezeichnetwerden.
    if (name == BINO_GUI) {
        m_bino_seen_t = clock_now();
    }

    // [SELBSTLERNEND 2026-08-11] Im Original wird hier nur noch GEMELDET, nicht
    // mehr selbst gepinnt: das Selbstlernen hatte 3140/3141 stillschweigend
    // dazugenommen und damit den Umriss gekillt. Was gepinnt wird, entscheidet
    // der Tree -- nicht das Script. Ohne Leser entfaellt der Block.

    // Reparatur zuerst und fuer JEDEN Namen -- sie muss auch Ebenen erreichen,
    // die keinen eigenen Haken haben.
    if (!force_viewtype(go, name)) {
        for (const auto& mv : MAP_VIEWS) {
            if (name == mv.name) {
                const auto it = m_vt_on.find(mv.key);
                apply_viewtype(go, name,
                               m_state.map_open && it != m_vt_on.end() && it->second);
                break;
            }
        }
    }

    // [SCOPE-RETICLE] Mit montiertem Scope das Reticle gar nicht erst zeichnen.
    if (name == "Gui_ui2040" && scopehide_attached()) {
        return false;
    }

    // Karte, Koffer oder Hauptmenue -- sonst gar nicht erst weitersehen.
    if (!(m_state.map_open || m_state.inv_open || m_state.menu_open)) {
        return true;
    }

    // [ACBACKGROUND 2026-08-09] Stoert in der Karte UND im Koffer, im
    // Hauptmenue bleibt er stehen.
    if (name == BG_NAME) {
        if (m_state.map_open || m_state.inv_open) {
            return !m_hide_bg;
        }

        return true;
    }

    if (MENU_NAMES.count(name) > 0) {
        return !m_state.menu_open;   // im Hauptmenue gar nicht zeichnen
    }

    // Alles Weitere betrifft ausschliesslich die Karte.
    if (!m_state.map_open) {
        return true;
    }

    if (const auto it = m_map_hide.find(name); it != m_map_hide.end() && it->second) {
        return false;
    }

    if (name == HIDE_NAME) {
        return !m_hide_3121;   // Haken gesetzt -> gar nicht zeichnen
    }

    if (name == SCALE_NAME) {
        // Gesetzt wird ABSOLUT auf den Reglerwert (Basis 1.0) -- nie gegen den
        // gelesenen Ist-Wert, sonst schaukelt er sich pro Frame auf.
        if (auto* ctrl = gui_root_control(go); ctrl != nullptr) {
            set_vec3(ctrl, "set_Scale",
                     glm::vec3{m_ui3101_scale, m_ui3101_scale, m_ui3101_scale});
        }
    }

    return true;
}

// ============================================================================
// UI -- Tree "RE4VR - UI". Zum Vergleichen der Ebenen: einzeln an/aus,
// waehrend die Karte offen ist.
// ============================================================================

void RE4VRUi::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    if (!ImGui::TreeNode("RE4VR - UI")) {
        return;
    }

    bool dirty = false;

    // [ZEIGESTRAHL-PITCH 2026-08-15] Sitzt eigentlich als Slider im Fork -- im
    // Headset ist ImGui aber nicht lesbar, darum hier: eingestellt am Desktop,
    // gespeichert in der JSON, beim Start in den Fork geschrieben.
    if (m_ptr_pitch_ok != std::optional<bool>{false}) {
        if (ImGui::SliderFloat("Zeigestrahl-Neigung (Grad)##ui_ptr", &m_ptr_pitch, -60.0f,
                               60.0f)) {
            dirty = true;
        }

        if (!m_ptr_pitch_ok.has_value()) {
            ImGui::TextColored(col_abgr(0xFF999999),
                               "   (Build noch nicht geprueft -- wirkt ab dem naechsten Frame)");
        }

        ImGui::Separator();
    }

    // [WALDMENUE] Steht bewusst VOR dem is_supported-Return: das hat mit dem
    // Fork nichts zu tun und funktioniert auch auf vanilla REFramework.
    if (ImGui::Checkbox("Hauptmenue: immer Waldszene##ui_forest", &m_forest_menu)) {
        dirty = true;
        forest_apply(true);
    }

    ImGui::Text("   erzwingt Stage 48000 als Menue-Kulisse (StartupScene wirkt evtl. erst "
                "nach Neustart)");
    ImGui::Separator();

    if (!is_supported()) {
        ImGui::TextColored(col_abgr(0xFFFFCC66),
                           "REFramework ohne GUI-Projection-Schalter -- Map-Fix inaktiv.");

        if (dirty) {
            m_cfg_dirty_t = clock_now();
        }

        ImGui::TreePop();
        return;
    }

    dirty |= ImGui::Checkbox("GUI-Matrix-Override aus (Karten-Umriss)##ui_gm",
                             &m_opt.gui_matrix);
    dirty |= ImGui::Checkbox("GUI-Element-Override aus (Icons/Marker)##ui_ge",
                             &m_opt.gui_elem);
    dirty |= ImGui::Checkbox("Mono-Rendering (Karten-Mesh)##ui_mono", &m_opt.mono);
    dirty |= ImGui::Checkbox("Flatscreen-Leinwand (natives Bild als Quad)##ui_cv",
                             &m_opt.canvas);
    dirty |= ImGui::Checkbox("VR komplett aussetzen (echtes Flatscreen)##ui_sus",
                             &m_opt.suspend);
    dirty |= ImGui::Checkbox("Karte am Kopf festpinnen (alle Ebenen ein Anker)##ui_mg",
                             &m_opt.mapglue);

    if (m_opt.mapglue) {
        dirty |= ImGui::SliderFloat("Karten-Abstand (m)##ui_gd", &m_glue_distance, 0.2f,
                                    10.0f);
        dirty |= ImGui::SliderFloat("Ebenen-Staffelung (m)##ui_gg", &m_glue_gap, 0.0f, 0.05f);

        ImGui::Text("   Welche Karten-Ebenen gepinnt werden:");

        for (size_t i = 0; i < GLUE_LAYERS.size(); ++i) {
            const auto& g = GLUE_LAYERS[i];
            auto it = m_glue_on.find(g.name);

            if (it == m_glue_on.end()) {
                it = m_glue_on.emplace(g.name, true).first;
            }

            char id[128]{};
            std::snprintf(id, sizeof(id), "   %s##glue%d", g.label, static_cast<int>(i + 1));

            // [KAPUTTES LOCAL] Wirkt sofort. Der Schluessel landet zwar nie in
            // der Datei, aber Lua meldet hier trotzdem `dirty` -- der
            // Schreibzeitpunkt der uebrigen Werte bleibt damit derselbe.
            dirty |= ImGui::Checkbox(id, &it->second);
        }
    }

    for (size_t i = 0; i < MAP_HIDE.size(); ++i) {
        const auto& mh = MAP_HIDE[i];
        auto it = m_map_hide.find(mh.name);

        if (it == m_map_hide.end()) {
            it = m_map_hide.emplace(mh.name, false).first;
        }

        char id[128]{};
        std::snprintf(id, sizeof(id), "%s##mh%d", mh.label, static_cast<int>(i + 1));

        // [KAPUTTES LOCAL] dito.
        dirty |= ImGui::Checkbox(id, &it->second);
    }

    dirty |= ImGui::Checkbox("Fernglas (2110) am Kopf festpinnen##ui_bg", &m_opt.binoglue);

    if (m_opt.binoglue) {
        // [KAPUTTES LOCAL] dito.
        dirty |= ImGui::SliderFloat("Fernglas-Abstand (m)##ui_bd", &m_bino_distance, 0.2f,
                                    10.0f);
    }

    // [VIEWTYPE] Ein Haken pro Karten-Ebene.
    ImGui::Text("ViewType Screen (gegen Tiefen-Drift der Karte):");

    for (size_t i = 0; i < MAP_VIEWS.size(); ++i) {
        const auto& mv = MAP_VIEWS[i];
        auto it = m_vt_on.find(mv.key);

        if (it == m_vt_on.end()) {
            it = m_vt_on.emplace(mv.key, false).first;
        }

        char id[128]{};
        std::snprintf(id, sizeof(id), "%s##vt%d", mv.label, static_cast<int>(i + 1));
        dirty |= ImGui::Checkbox(id, &it->second);
    }

    // Zielwerte = Messung vom 2026-08-10, vor jedem Eingriff: alles Screen (0),
    // nur Gui_ui3120 war World (1).
    if (ImGui::Button("Karten-ViewTypes reparieren (Messwerte)##ui_vtfix")) {
        m_vt_force = {
            {"Gui_ui3100", 0}, {"Gui_ui3101", 0}, {"Gui_ui3103", 0},
            {"Gui_ui3104", 0}, {"Gui_ui3120", VIEWTYPE_WORLD}, {"Gui_ui3121", 0},
        };

        for (auto& [k, v] : m_vt_on) {
            v = false;
        }

        m_vt_orig.clear();
        dirty = true;
    }

    if (!m_vt_force.empty()) {
        ImGui::TextColored(col_abgr(0xFFFFCC66),
                           "   ... wirkt beim naechsten Zeichnen -- einmal die Karte "
                           "aufmachen.");
    }

    if (m_opt.canvas) {
        dirty |= ImGui::SliderFloat("Leinwand-Breite (m)##ui_cw", &m_canvas_width, 0.1f,
                                    20.0f);
        dirty |= ImGui::SliderFloat("Leinwand-Abstand (m)##ui_cd", &m_canvas_distance, 0.1f,
                                    20.0f);
    }

    ImGui::Separator();

    for (size_t i = 0; i < GLOBAL_GUIS.size(); ++i) {
        const auto& gg = GLOBAL_GUIS[i];
        auto it = m_global_hide.find(gg.key);

        if (it == m_global_hide.end()) {
            it = m_global_hide.emplace(gg.key, true).first;
        }

        char id[160]{};
        std::snprintf(id, sizeof(id), "%s##g%d", gg.label, static_cast<int>(i + 1));
        dirty |= ImGui::Checkbox(id, &it->second);
    }

    for (size_t i = 0; i < SUB_GUIS.size(); ++i) {
        const auto& sg = SUB_GUIS[i];
        auto it = m_global_hide.find(sg.key);

        if (it == m_global_hide.end()) {
            it = m_global_hide.emplace(sg.key, true).first;
        }

        char id[192]{};
        std::snprintf(id, sizeof(id), "%s##s%d", sg.label, static_cast<int>(i + 1));
        dirty |= ImGui::Checkbox(id, &it->second);
    }

    dirty |= ImGui::Checkbox("Gui_ui3121 ausblenden##ui_h3121", &m_hide_3121);
    dirty |= ImGui::Checkbox("AcBackGround ausblenden (Karte + Koffer)##ui_hbg", &m_hide_bg);
    dirty |= ImGui::SliderFloat("Gui_ui3101 Groesse##ui_sc", &m_ui3101_scale, 0.1f, 3.0f);

    ImGui::Separator();

    if (dirty) {
        m_cfg_dirty_t = clock_now();
    }

    ImGui::Text("Karte offen: %s  |  Koffer: %s  |  Hauptmenue: %s",
                m_state.map_open ? "true" : "false", m_state.inv_open ? "true" : "false",
                m_state.menu_open ? "true" : "false");
    ImGui::Text("   aktiv: gui=%s elem=%s mono=%s leinwand=%s suspend=%s kartenpin=%s",
                m_state.applied ? "true" : "false", m_state.elem ? "true" : "false",
                m_state.mono ? "true" : "false", m_state.canvas ? "true" : "false",
                m_state.suspend ? "true" : "false", m_state.mapglue ? "true" : "false");

    // Pro Ebene sichtbar machen, ob der Schalter die View WIRKLICH erwischt hat.
    std::string vt_line = "ViewType (ist / Original):";

    for (const auto& mv : MAP_VIEWS) {
        const std::string nm{mv.name};
        const auto seen = m_vt_seen.find(nm);
        const auto orig = m_vt_orig.find(nm);

        vt_line += "   " + nm.substr(nm.size() >= 4 ? nm.size() - 4 : 0) + "=";
        vt_line += (seen != m_vt_seen.end() ? seen->second : std::string{"-"}) + "/";
        vt_line += (orig != m_vt_orig.end() ? std::to_string(orig->second)
                                            : std::string{"-"});
    }

    ImGui::Text("%s", vt_line.c_str());
    ImGui::TextColored(col_abgr(0xFF99CCFF),
                       "   0 = Screen, 1 = World, \"-\" = Ebene kam beim Zeichnen nie "
                       "vorbei.");

    ImGui::TreePop();
}

#endif // RE4
