#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "REFramework.hpp"

#include "RE4VRMenu.hpp"

#if defined(RE4)

#include "RE4VR.hpp"
#include "RE4VRArmChain.hpp"
#include "RE4VRBinding.hpp"
#include "RE4VRChoke.hpp"
#include "RE4VRCrosshair.hpp"
#include "RE4VRFirstPerson.hpp"
#include "RE4VRGuestures.hpp"
#include "RE4VRHolster.hpp"
#include "RE4VRJiggle.hpp"
#include "RE4VRKillswitch.hpp"
#include "RE4VRMaterials.hpp"
#include "RE4VRMerc.hpp"
#include "RE4VRMinecart.hpp"
#include "RE4VRMotion.hpp"
#include "RE4VRMovement.hpp"
#include "RE4VRRecoil.hpp"
#include "RE4VRReload.hpp"
#include "RE4VRUi.hpp"
#include "RE4VRWeapons.hpp"
#include "RE4VRWeapons2.hpp"

std::shared_ptr<RE4VRMenu>& RE4VRMenu::get() {
    static auto inst = std::make_shared<RE4VRMenu>();
    return inst;
}

std::optional<std::string> RE4VRMenu::on_initialize() {
    load_menu_editor_cfg();
    load_game_settings_cfg();

    return Mod::on_initialize();
}

// ============================================================================
// [VR-MENUE-GROESSE 11.09.2026] reframework/data/re4_vr/re4_vr_menu.json, bei allen
// anderen JSONs (re4vr::datadir). [PFAD 11.09.2026] Lag bis dahin falsch unter
// <Spielordner>/data/re4vr_menu.json -- get_persistent_dir() ist der Spielordner
// selbst, nicht reframework/. Ohne BOM.
// Fehlt die Datei, bleiben die Defaults aus REFramework.hpp.
// ============================================================================
namespace {
std::filesystem::path menu_editor_cfg_path() {
    return re4vr::datadir() / "re4_vr" / "re4_vr_menu.json";
}
}

void RE4VRMenu::load_menu_editor_cfg() {
    if (g_framework == nullptr) {
        return;
    }

    try {
        std::ifstream f{menu_editor_cfg_path()};

        if (!f) {
            return;
        }

        const auto j = nlohmann::json::parse(f, nullptr, false);

        if (j.is_discarded() || !j.is_object()) {
            return;
        }

        if (j.contains("vr_panel_width") && j["vr_panel_width"].is_number()) {
            g_framework->set_vr_menu_panel_width(j["vr_panel_width"].get<float>());
        }

        if (j.contains("vr_panel_distance") && j["vr_panel_distance"].is_number()) {
            g_framework->set_vr_menu_panel_distance(j["vr_panel_distance"].get<float>());
        }

        if (j.contains("vr_category_text_scale") && j["vr_category_text_scale"].is_number()) {
            g_framework->set_vr_menu_nav_text_scale(j["vr_category_text_scale"].get<float>());
        }

        if (j.contains("vr_content_text_scale") && j["vr_content_text_scale"].is_number()) {
            g_framework->set_vr_menu_detail_text_scale(j["vr_content_text_scale"].get<float>());
        }

        if (j.contains("vr_stick_deadzone") && j["vr_stick_deadzone"].is_number()) {
            g_framework->set_vr_menu_stick_deadzone(j["vr_stick_deadzone"].get<float>());
        }

        if (j.contains("vr_category_rounding") && j["vr_category_rounding"].is_number()) {
            g_framework->set_vr_menu_nav_rounding(j["vr_category_rounding"].get<float>());
        }

        if (j.contains("vr_stick_repeat_delay") && j["vr_stick_repeat_delay"].is_number()) {
            g_framework->set_vr_menu_repeat_delay(j["vr_stick_repeat_delay"].get<float>());
        }

        if (j.contains("vr_stick_repeat_interval") && j["vr_stick_repeat_interval"].is_number()) {
            g_framework->set_vr_menu_repeat_interval(j["vr_stick_repeat_interval"].get<float>());
        }

        // [SCROLLTEMPO 14.09.2026] Pixel pro Sekunde, rechter Stick.
        if (j.contains("vr_scroll_speed") && j["vr_scroll_speed"].is_number()) {
            g_framework->set_vr_menu_scroll_speed(j["vr_scroll_speed"].get<float>());
        }

        if (j.contains("vr_background_opacity") && j["vr_background_opacity"].is_number()) {
            g_framework->set_vr_menu_background_opacity(j["vr_background_opacity"].get<float>());
        }
    } catch (...) {
    }
}

void RE4VRMenu::save_menu_editor_cfg() {
    if (g_framework == nullptr) {
        return;
    }

    try {
        const auto path = menu_editor_cfg_path();
        std::filesystem::create_directories(path.parent_path());

        nlohmann::json j{};
        j["vr_panel_width"] = g_framework->get_vr_menu_panel_width();
        j["vr_panel_distance"] = g_framework->get_vr_menu_panel_distance();
        j["vr_category_text_scale"] = g_framework->get_vr_menu_nav_text_scale();
        j["vr_content_text_scale"] = g_framework->get_vr_menu_detail_text_scale();
        j["vr_stick_deadzone"] = g_framework->get_vr_menu_stick_deadzone();
        j["vr_category_rounding"] = g_framework->get_vr_menu_nav_rounding();
        j["vr_stick_repeat_delay"] = g_framework->get_vr_menu_repeat_delay();
        j["vr_stick_repeat_interval"] = g_framework->get_vr_menu_repeat_interval();
        j["vr_scroll_speed"] = g_framework->get_vr_menu_scroll_speed();
        j["vr_background_opacity"] = g_framework->get_vr_menu_background_opacity();

        std::ofstream f{path, std::ios::trunc};
        f << j.dump(4);
    } catch (...) {
    }
}

// ============================================================================
// [TOXIC SETTINGS 11.09.2026] "Disable Toxic Gamesettings"
//
// Die Spieloptionen liegen in chainsaw.OptionManager als Liste (OptionID, Wert).
// Per MCP am 11.09.2026 belegt: setCurrentOptionValue(OptionID, Int32) stellt
// die Option sofort um, das Spielmenue zeigt den neuen Wert. Jede ID und jeder
// Sollwert unten wurde mit dem User im Menue gegengeprueft (umgeschaltet ->
// Wert gelesen). Achtung Objektivverzeichnung: dort ist AUS = 2, nicht 0.
// ============================================================================
namespace {
struct ToxicSetting {
    int32_t id;       // chainsaw.option.OptionID
    int32_t value;    // Sollwert
    const char* name;
};

constexpr std::array<ToxicSetting, 13> TOXIC_SETTINGS{{
    {68, 0, "ControllerAimAssist"},          // Zielhilfe: AUS (AN = 2)
    {31, 0, "CameraAimAssistLevel"},         // Fadenkreuz-Verlangsamung: 0 (Regler 0..100)
    {4, 0, "ControllerAutoReload"},          // Auto-Nachladen: AUS (AN = 1)
    {63, 0, "FollowCameraRun"},              // Kameraunterstuetzung: AUS (Horizontal & Vertikal = 1)
    {13, 0, "CameraVibration"},              // Kameraschwanken: AUS (AN = 1)
    {44, 0, "DisplayHDRMode"},               // HDR-Modus: AUS
    {10037, 0, "PCGraphicsFSR1"},            // FidelityFX Super Resolution 1: AUS (Ultra Quality = 1)
    {10038, 0, "PCGraphicsFSR2"},            // FidelityFX Super Resolution 2: AUS (Gute Qualitaet = 1)
    {10022, 0, "PCGraphicsReflection"},      // Screen Space Reflections: AUS (AN = 1)
    {10040, 0, "PCGraphicsStrandHair"},      // Haarstraehnen: AUS (Hoch = 2)
    {10013, 0, "PCGraphicsMotionBlur"},      // Bewegungsunschaerfe: AUS (AN = 1)
    {10025, 2, "PCGraphicsLensDistortion"},  // Objektivverzeichnung: AUS = 2 (AN inkl. chrom. Aberration = 0)
    {10024, 0, "PCGraphicsDepthOfField"},    // Schaerfentiefe: AUS (AN = 1)
}};

constexpr const char* GAME_SETTINGS_CFG = "re4_vr/re4_vr_game_settings.json";
}

void RE4VRMenu::load_game_settings_cfg() {
    const auto j = re4vr::json_load(GAME_SETTINGS_CFG);

    if (j.is_object() && j.contains("disable_toxic_settings") && j["disable_toxic_settings"].is_boolean()) {
        m_disable_toxic_settings = j["disable_toxic_settings"].get<bool>();
    }
}

void RE4VRMenu::save_game_settings_cfg() {
    nlohmann::json j{};
    j["disable_toxic_settings"] = m_disable_toxic_settings;

    re4vr::json_save(GAME_SETTINGS_CFG, j);
}

void RE4VRMenu::draw_public_toxic_settings() {
    if (g_framework->draw_menu_checkbox("Disable Toxic Gamesettings", &m_disable_toxic_settings)) {
        save_game_settings_cfg();
        m_toxic_next_check = {};   // beim Einschalten sofort pruefen
    }
}

// Menue offen? Dieselben Quellen wie RE4VRBinding::is_any_menu_open, aber hier
// selbst abgefragt (das Binding bleibt unberuehrt) und OHNE "HUD aus" -- das ist
// kein Menue.
bool RE4VRMenu::toxic_menu_open() {
    // Titelbildschirm / Laden: kein Spieler-Kontext.
    if (re4vr::fc::ctx() == nullptr || re4vr::fc::body_go() == nullptr) {
        return true;
    }

    // Pausenmenue (von dort gehen die Optionen im Spiel auf).
    if (auto* pm = re4vr::fc::managed_singleton("share.PauseManager"); pm != nullptr) {
        bool paused = false;

        if (re4vr::try_call<bool>(pm, "isPaused()", paused) && paused) {
            return true;
        }
    }

    if (auto* gm = re4vr::fc::managed_singleton("chainsaw.GuiManager"); gm != nullptr) {
        bool locked = false;

        if (re4vr::try_call<bool>(gm, "get_hasOccupiedPauseMenuSystemLock", locked) && locked) {
            return true;
        }
    }

    return false;
}

void RE4VRMenu::toxic_settings_tick() {
    if (!m_disable_toxic_settings) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < m_toxic_next_check) {
        return;
    }

    m_toxic_next_check = now + std::chrono::seconds(1);

    if (!toxic_menu_open()) {
        return;
    }

    auto* om = re4vr::fc::managed_singleton("chainsaw.OptionManager");

    if (om == nullptr) {
        return;
    }

    for (const auto& s : TOXIC_SETTINGS) {
        int32_t current = -1;

        // Getter mit System.Int32-Rueckgabe -> roh ueber try_call ist hier sicher.
        if (!re4vr::try_call<int32_t>(om, "getCurrentOptionValue(chainsaw.option.OptionID)", current, s.id)) {
            continue;
        }

        // -1 = Eintrag (noch) nicht geladen -- dann nichts schreiben.
        if (current == -1 || current == s.value) {
            continue;
        }

        // Kommando -> ueber call_cmd (Lua-Weg), nicht roh (s. RE4VR.hpp [LUA-WEG]).
        std::array<void*, 2> args{re4vr::arg_int(s.id), re4vr::arg_int(s.value)};

        re4vr::call_cmd(om, "setCurrentOptionValue(chainsaw.option.OptionID, System.Int32)", std::span<void*>(args));
    }
}

// ============================================================================
// [MENUE-SOUNDS 11.09.2026] Vom User im Soundplayer ("GUI / System Sounds")
// ausgewaehlt. Namen statt Enum-Nummern -- die Nummern koennten sich mit einem
// Spiel-Update verschieben. Aufgeloest wird EINMAL live (re4vr::enum_value,
// derselbe Weg wie der Taschenlampen-Klick in RE4VRMotion, dort mit
// CH_GUI_FILE_DECIDE > 2^31 bereits bewaehrt). Nicht gefunden -> stumm.
// ============================================================================
// ============================================================================
// [SPLASH 13.09.2026] Begruessung beim allerersten Start -- s. RE4VRMenu.hpp
// ============================================================================
namespace {
constexpr const char* SPLASH_CFG_PATH = "re4_vr/re4_vr_splash.json";
constexpr double SPLASH_SECONDS = 20.0;

// Vom User vorgegebener Text, nach Rolle getrennt -- die Nummern stehen einzeln,
// weil sie ROT und in der RE-Schrift gezeichnet werden (Vorlage: then.png).
//
// [KEIN DOPPELTES PROZENT 13.09.2026] Hier steht EIN Prozentzeichen. Frueher
// standen zwei, weil ImGui::Text ein printf ist -- gezeichnet wird aber mit
// TextUnformatted, und das nimmt den Text roh: sichtbar wurden zwei Zeichen.
const char* const SPLASH_TITLE = "Welcome to RE4VR";
const char* const SPLASH_INTRO = "Here are some valuable Tips";
const char* const SPLASH_OUTRO = "Have fun!";

// [OHNE KLAMMERN 13.09.2026 -- Ansage] Die Zeile steht rot und in der
// RE-Schrift; Klammern braucht sie dann nicht mehr.
const char* const SPLASH_ONCE =
    "This window only appears once, on the very first start of the mod.";

struct SplashItem {
    const char* num;    // nullptr = Fortsetzungszeile ohne eigene Nummer
    const char* text;
};

const SplashItem SPLASH_ITEMS[] = {
    {"1.", "You can open the mod menu with LT + Left A (Y)."},
    {"2.", "Game settings that interfere with VR have been automatically disabled."},
    {"3.", "The revolver can fire. You just have to cock it first (right stick down)."},
    {"4.", "Yes AFW is beta and will be improved in the future."},
};
}

void RE4VRMenu::splash_tick() {
    // Genau EINMAL pro Spielstart nachsehen.
    if (!m_splash_checked) {
        m_splash_checked = true;

        // [FALLE 13.09.2026] NICHT auf is_object() pruefen: re4vr::json_load
        // liefert bei FEHLENDER Datei ein leeres Objekt zurueck (RE4VR.cpp) --
        // damit war die Pruefung immer wahr und der Splash kam nie. Es
        // entscheidet allein der Schluessel.
        if (const auto j = re4vr::json_load(SPLASH_CFG_PATH);
            j.is_object() && j.contains("seen")) {
            return;   // schon einmal gezeigt
        }

        m_splash_active = true;
        m_splash_t0 = std::chrono::steady_clock::now();

        // Die Tafel existiert nur, solange die UI gezeichnet wird. `false` =
        // NICHT in die REFramework-Config schreiben: das hier ist ein einmaliger
        // Sonderfall und darf den gespeicherten Menue-Zustand nicht veraendern.
        g_framework->set_draw_ui(true, false);

        // SOFORT merken, nicht erst nach dem Countdown -- stuerzt das Spiel in
        // diesen 10 Sekunden ab, kaeme der Splash sonst beim naechsten Mal
        // wieder.
        nlohmann::json j{};
        j["seen"] = true;
        re4vr::json_save(SPLASH_CFG_PATH, j);

        return;
    }

    if (!m_splash_active) {
        return;
    }

    // Von Hand geschlossen -> auch fertig.
    if (!g_framework->is_drawing_ui()) {
        m_splash_active = false;
        return;
    }

    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_splash_t0).count();

    if (t >= SPLASH_SECONDS) {
        m_splash_active = false;
        g_framework->set_draw_ui(false, false);
    }
}

void RE4VRMenu::draw_splash_window() {
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_splash_t0).count();
    const int rest = static_cast<int>(SPLASH_SECONDS - t) + 1;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(g_framework->menu_px(48.0f), g_framework->menu_px(40.0f)));

    if (ImGui::Begin("##re4vr_splash", nullptr, FLAGS)) {
        // [GLEICHE GROESSE WIE DAS MENUE] Exakt die Rechnung aus
        // REFramework::draw_menu_window fuer den Detail-Bereich.
        const float sc = g_framework->get_vr_menu_detail_text_scale()
                         * g_framework->menu_draw_scale();
        const float fs = (sc > 0.0f)
            ? (sc / static_cast<float>(REFramework::VR_FONT_OVERSAMPLE)) : 1.0f;

        auto* const base_font = g_framework->vr_font_base();
        auto* const head_font = g_framework->vr_font_heading();

        // [GROESSE 13.09.2026] Die RE-Schrift der Nummern traegt optisch dicker
        // auf als die Menue-Schrift. Damit Zahl und Satz gleich gross WIRKEN,
        // laeuft der weisse Text mit diesem Faktor.
        constexpr float TEXT_MULT = 1.55f;

        const ImVec4 ROT{0.87f, 0.00f, 0.00f, 1.00f};
        const ImVec4 WEISS{0.92f, 0.92f, 0.92f, 1.00f};
        const ImVec4 GRAU{0.55f, 0.55f, 0.55f, 1.00f};

        // Eine Zeile mittig setzen. Gemessen wird mit dem Font, der gerade
        // aktiv ist -- deshalb erst pushen, dann messen, dann zentrieren.
        const auto zeile_mittig = [&](const char* txt, ImFont* f, const ImVec4& col,
                                      float mult = 1.0f) {
            if (f != nullptr) {
                ImGui::PushFont(f);
                ImGui::SetWindowFontScale(fs * mult);
            }

            const float w = ImGui::CalcTextSize(txt).x;
            const float avail = ImGui::GetContentRegionAvail().x;

            if (avail > w) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(txt);
            ImGui::PopStyleColor();

            if (f != nullptr) {
                ImGui::PopFont();
            }
        };

        zeile_mittig(SPLASH_TITLE, head_font, ROT);
        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(52.0f)));

        zeile_mittig(SPLASH_INTRO, base_font, WEISS, TEXT_MULT);
        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(26.0f)));

        // ---- Der FAQ-Block ----
        // Die Punkte stehen UNTEREINANDER buendig, der Block als Ganzes ist
        // mittig (so wie in der Vorlage). Dafuer zuerst die breiteste Zeile.
        float num_w = 0.0f;

        if (head_font != nullptr) {
            ImGui::PushFont(head_font);
            ImGui::SetWindowFontScale(fs);
            num_w = ImGui::CalcTextSize("5.  ").x;
            ImGui::PopFont();
        }

        float block_w = 0.0f;

        if (base_font != nullptr) {
            ImGui::PushFont(base_font);
            ImGui::SetWindowFontScale(fs * TEXT_MULT);
        }

        for (const auto& it : SPLASH_ITEMS) {
            block_w = std::max(block_w, num_w + ImGui::CalcTextSize(it.text).x);
        }

        if (base_font != nullptr) {
            ImGui::PopFont();
        }

        const float avail_block = ImGui::GetContentRegionAvail().x;
        const float block_x = ImGui::GetCursorPosX()
            + ((avail_block > block_w) ? (avail_block - block_w) * 0.5f : 0.0f);

        for (const auto& it : SPLASH_ITEMS) {
            // Die NUMMER: rot und in der RE-Schrift.
            if (it.num != nullptr && head_font != nullptr) {
                ImGui::SetCursorPosX(block_x);
                ImGui::PushFont(head_font);
                ImGui::SetWindowFontScale(fs);
                ImGui::PushStyleColor(ImGuiCol_Text, ROT);
                ImGui::TextUnformatted(it.num);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine(0.0f, 0.0f);
            }

            // Der Text startet IMMER an derselben Spalte -- so stehen
            // Fortsetzungszeilen buendig unter dem Satz, nicht unter der Nummer.
            ImGui::SetCursorPosX(block_x + num_w);

            if (base_font != nullptr) {
                ImGui::PushFont(base_font);
                ImGui::SetWindowFontScale(fs * TEXT_MULT);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, WEISS);
            ImGui::TextUnformatted(it.text);
            ImGui::PopStyleColor();

            if (base_font != nullptr) {
                ImGui::PopFont();
            }

            // Luft zwischen den Punkten; Fortsetzungszeilen bleiben eng dran.
            ImGui::Dummy(ImVec2(0.0f,
                                g_framework->menu_px(it.num != nullptr ? 12.0f : 0.0f)));
        }

        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(18.0f)));

        zeile_mittig(SPLASH_OUTRO, base_font, WEISS, TEXT_MULT);

        // [SIGNATUR 13.09.2026] Auf DERSELBEN Zeile wie "Have fun!", aber ganz
        // rechts -- SameLine holt den Cursor zurueck in die Zeile, danach wird
        // er an den rechten Rand gesetzt.
        {
            ImGui::SameLine();

            // RE-Schrift, weiss. ImGui kann NICHT kursiv stellen -- dafuer
            // braeuchte es einen eigenen Kursiv-Font, den das Menue nicht hat.
            // Dieselbe Groesse wie die rote Schlusszeile darunter (0.5).
            if (head_font != nullptr) {
                ImGui::PushFont(head_font);
                ImGui::SetWindowFontScale(fs * 0.5f);
            }

            const char* sig = "-Talemann";
            const float w = ImGui::CalcTextSize(sig).x;

            // Endet buendig mit der rechten Kante des FAQ-Blocks -- also unter
            // dem Ende des laengsten Satzes, nicht am Fensterrand.
            ImGui::SetCursorPosX(block_x + block_w - w);
            ImGui::PushStyleColor(ImGuiCol_Text, WEISS);
            ImGui::TextUnformatted(sig);
            ImGui::PopStyleColor();

            if (head_font != nullptr) {
                ImGui::PopFont();
            }
        }

        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(14.0f)));

        // [KLEINER 13.09.2026] Die RE-Schrift traegt hier viel zu dick auf --
        // in voller Menuegroesse schob diese eine Zeile den Countdown aus dem
        // Bild. Halbe Groesse: bleibt rot und in der Hausschrift, ohne alles
        // zu erschlagen.
        zeile_mittig(SPLASH_ONCE, head_font, ROT, 0.5f);
        ImGui::Dummy(ImVec2(0.0f, g_framework->menu_px(18.0f)));

        char cd[64]{};
        std::snprintf(cd, sizeof(cd), "closing in %d ...", rest > 0 ? rest : 0);
        zeile_mittig(cd, base_font, GRAU);

        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void RE4VRMenu::on_frame() {
    // [SPLASH] Vor allem anderen: er oeffnet die UI selbst und schliesst sie wieder.
    splash_tick();

    // [TOXIC SETTINGS 11.09.2026] Vor den Sounds -- die steigen bei leerer Liste aus.
    toxic_settings_tick();

    auto sounds = g_framework->take_menu_sounds();

    if (sounds.empty()) {
        return;
    }

    static const std::array<const char*, 6> SOUND_NAMES{
        "CH_GUI_FILE_UNIQUE_01",      // Open     -- Menue auf/zu
        "CH_GUI_SAVELOAD_CANCEL",     // Tree     -- Tree auf-/zuklappen
        "CH_GUI_FILE_DECIDE",         // Toggle   -- Checkbox, Auswahlpunkt, Knopf
        "CH_GUI_INGAMESHOP_CANCEL",   // Slider   -- Slider verschieben
        "CH_GUI_DIALOG_OPEN",         // Category -- Kategorie wechseln
        "CH_GUI_ARTWORK_CURSOR",      // Cursor   -- Markierung wandert (12.09.2026)
    };

    static std::array<std::optional<int32_t>, 6> values{};
    static bool resolved = false;

    if (!resolved) {
        resolved = true;

        for (size_t i = 0; i < SOUND_NAMES.size(); ++i) {
            values[i] = re4vr::enum_value("chainsaw.gui.GuiSoundType", SOUND_NAMES[i]);
        }
    }

    auto* gsm = re4vr::fc::managed_singleton("chainsaw.GuiSoundManager");

    if (gsm == nullptr) {
        return;
    }

    for (const auto sound : sounds) {
        const auto idx = (size_t)sound;

        if (idx < values.size() && values[idx].has_value()) {
            re4vr::call_safe<void*>(gsm, "wwiseTriggerTarget(chainsaw.gui.GuiSoundType)", *values[idx]);
        }
    }
}

// ============================================================================
// Das nackte UI -- feste Reihenfolge.
//
// Die Zahlen in den Kommentaren sind die `order`-Werte aus dem alten
// Lua-Dispatcher (##re4_vr_menu.lua); die Reihenfolge hier bildet sie exakt ab.
// Jeder Aufruf zeichnet den Block, den der jeweilige Mod schon vorher als
// eigene draw_public_*-Methode hatte -- inhaltlich aendert sich nichts.
// ============================================================================
void RE4VRMenu::draw_public() {
    // [NEUE REIHENFOLGE 11.09.2026] Vom User festgelegt -- die alten
    // `order`-Werte des Lua-Dispatchers gelten fuer diesen Block nicht mehr.
    // Ueberschriften: Status, Select Headset, Recoil, Crosshair, Select Laser
    // Color, Miscellaneous, Shotgun Shell Ratio, Red9 Shell Ratio.
    RE4VRMotion::get()->draw_public_headset();          // Status + Select Headset
    RE4VRRecoil::get()->draw_public_recoil_level();     // Recoil

    RE4VRCrosshair::get()->draw_public_crosshair_off(); // Crosshair: Ueberschrift + Disable Crosshair
    RE4VRCrosshair::get()->draw_public_reticle_color(); //            Farbauswahl
    RE4VRCrosshair::get()->draw_public_reticle_size();  //            Crosshair Size (nur mit Waffe)
    RE4VRCrosshair::get()->draw_public_laser_color();   // Select Laser Color

    // [MISCELLANEOUS 11.09.2026] Die freien Schalter bekommen eine eigene
    // Ueberschrift und stehen unter "Select Laser Color".
    g_framework->draw_menu_heading("Miscellaneous", true);

    // [ZEILENABSTAND 11.09.2026] Dezent mehr Luft zwischen den Schaltern:
    // 8 px statt der 4 px aus dem Theme, nur fuer diesen Block.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, g_framework->menu_px(8.0f)));
    draw_public_toxic_settings();                       // Disable Toxic Gamesettings (erster Schalter)
    RE4VRKillswitch::get()->draw_public_ui();           // Enable Firstperson Events
    RE4VRWeapons::get()->draw_public_assist_light();    // Disable Assist Light
    RE4VRBinding::get()->draw_public_ui();              // 180 Grad, Snapturn, Roomscale
    ImGui::PopStyleVar();

    RE4VRReload::get()->draw_public_shotgun_ratio();    // Shotgun Shell Ratio
    RE4VRReload::get()->draw_public_red9_ratio();       // Red9 Shell Ratio

    // [MENUE-KATEGORIEN 11.09.2026] Hier stand als letzter Punkt der
    // Sammelbaum "REFramework Options". Er ist jetzt eine eigene Kategorie
    // ganz unten in der linken Spalte (REFramework::draw_menu_nav) und
    // bleibt dort auch im Public-Release.
}

void RE4VRMenu::draw_menu_editor() {
    // [STYLE EDITOR 10.09.2026] ImGuis eingebauter Live-Editor, frueher GANZ
    // unten und bewusst hier drin: draw_dev() laeuft nur bei RE4VR_DEV_UI, der
    // Editor ist im Public-Release also automatisch weg -- wie alle Trees
    // darueber.
    // [ALPHABETISCH 16.09.2026 -- Ansage des Users] Stand bis heute ausserhalb
    // der Liste ganz unten; jetzt ein Eintrag "Menu Editor" in draw_dev().
    //
    // Was er kann: unter "Colors" jeden der ~55 Farbslots einzeln (WindowBg,
    // Border, Text, Header, Button ... je mit eigenem Alpha), unter "Sizes" die
    // Rundungen, Rahmenstaerken und Abstaende.
    //
    // ACHTUNG: die Aenderungen sind FLUECHTIG -- sie leben nur im ImGui-Kontext,
    // nicht in der Config, und jede Kontext-Erstellung setzt sie ueber
    // REFramework::set_imgui_style() auf den Code-Stand zurueck. Der Weg zum
    // Behalten: Tab "Colors" -> Haken "Only Modified Colors" aus -> "Export"
    // (To Clipboard) -> der Block gehoert in set_imgui_style(). Die "Sizes"
    // erfasst der Export NICHT, die muessen abgelesen werden.
    ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_Once);

    // [MENU EDITOR 11.09.2026] War "RE4VR - Style Editor". Ganz oben jetzt die
    // Groesse der VR-Tafel (live, gespeichert beim Loslassen des Reglers in
    // reframework/data/re4_vr/re4_vr_menu.json), darunter wie bisher der Style Editor.
    // [11.09.2026] TreeNode statt CollapsingHeader: der Header zeichnete seinen Pfeil
    // in voller Groesse (framed), die anderen Trees mit 70 % -- jetzt gleich gross.
    if (ImGui::TreeNode("RE4VR - Menu Editor")) {
        // [VR-SCHRIFT 11.09.2026] Textgroessen im VR-Menue, 1.0 = wie bisher.
        // Gelten fuer VR-Menue und Desktop (dort zusaetzlich mit der Fensterhoehe
        // skaliert, s. REFramework::draw_menu_window).
        float nav_scale = g_framework->get_vr_menu_nav_text_scale();

        // [BIS 5.0 16.09.2026] Ueber 3.0 wird die Schrift weicher (Oversample 3).
        if (ImGui::SliderFloat("Category Text Size", &nav_scale, 0.5f, 5.0f, "%.2f")) {
            g_framework->set_vr_menu_nav_text_scale(nav_scale);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        // [KATEGORIE-RUNDUNG 11.09.2026] Nur die rote Markierung links.
        float nav_rounding = g_framework->get_vr_menu_nav_rounding();

        if (ImGui::SliderFloat("Category Rounding", &nav_rounding, 0.0f, 40.0f, "%.1f")) {
            g_framework->set_vr_menu_nav_rounding(nav_rounding);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        float detail_scale = g_framework->get_vr_menu_detail_text_scale();

        if (ImGui::SliderFloat("Content Text Size", &detail_scale, 0.5f, 3.0f, "%.2f")) {
            g_framework->set_vr_menu_detail_text_scale(detail_scale);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        // [HINTERGRUND 11.09.2026] Deckkraft von Menu.png hinter dem Menue, live;
        // 0 = Bild aus, das Spiel scheint durch.
        float bg_opacity = g_framework->get_vr_menu_background_opacity();

        if (ImGui::SliderFloat("Background Opacity", &bg_opacity, 0.0f, 1.0f, "%.2f")) {
            g_framework->set_vr_menu_background_opacity(bg_opacity);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        // [STICK-DEADZONE 11.09.2026] Ab welchem Stick-Ausschlag das VR-Menue reagiert.
        float deadzone = g_framework->get_vr_menu_stick_deadzone();

        if (ImGui::SliderFloat("Stick Deadzone", &deadzone, 0.2f, 0.95f, "%.2f")) {
            g_framework->set_vr_menu_stick_deadzone(deadzone);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        // [STICK-WIEDERHOLUNG 11.09.2026] Halten: Wartezeit bis zum ersten
        // Wiederholen und Abstand zwischen den Wiederholungen, in Sekunden.
        float repeat_delay = g_framework->get_vr_menu_repeat_delay();

        if (ImGui::SliderFloat("Stick Repeat Delay (s)", &repeat_delay, 0.1f, 1.0f, "%.2f")) {
            g_framework->set_vr_menu_repeat_delay(repeat_delay);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        // [SCROLLTEMPO 14.09.2026 -- Ansage] Wie schnell der rechte Stick den
        // rechten Bereich scrollt. 900 war der bisherige feste Wert.
        float scroll_speed = g_framework->get_vr_menu_scroll_speed();

        if (ImGui::SliderFloat("Scroll Speed (px/s)", &scroll_speed, 200.0f, 4000.0f, "%.0f")) {
            g_framework->set_vr_menu_scroll_speed(scroll_speed);
        }

        // Gespeichert wird wie bei den Nachbarn: erst beim Loslassen.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        float repeat_interval = g_framework->get_vr_menu_repeat_interval();

        if (ImGui::SliderFloat("Stick Repeat Interval (s)", &repeat_interval, 0.03f, 0.6f, "%.2f")) {
            g_framework->set_vr_menu_repeat_interval(repeat_interval);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        float width = g_framework->get_vr_menu_panel_width();

        if (ImGui::SliderFloat("VR Menu Width (m)", &width, 0.5f, 4.0f, "%.2f")) {
            g_framework->set_vr_menu_panel_width(width);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        float distance = g_framework->get_vr_menu_panel_distance();

        if (ImGui::SliderFloat("VR Menu Distance (m)", &distance, 0.5f, 3.0f, "%.2f")) {
            g_framework->set_vr_menu_panel_distance(distance);
        }

        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_menu_editor_cfg();
        }

        ImGui::Separator();

        // Style Editor: die Aenderungen sind weiterhin FLUECHTIG (s. oben).
        ImGui::ShowStyleEditor();

        ImGui::TreePop();
    }
}

// ============================================================================
// Die Entwickler-Trees -- ALPHABETISCH nach Anzeigename.
//
// Der Name steht hier ausgeschrieben daneben, damit die Sortierung an EINER
// Stelle nachvollziehbar ist und beim Hinzufuegen eines Mods nicht geraten
// werden muss. Sortiert wird zur Laufzeit, nicht von Hand -- ein neuer Eintrag
// darf also irgendwo in die Liste.
// ============================================================================
void RE4VRMenu::draw_dev() {
    // [DEV-UEBERSCHRIFTEN 11.09.2026] Oben "C++" (unsere nativen Module), unten vor
    // den Lua-Oberflaechen "LUA" -- beide wie die anderen Menue-Ueberschriften
    // (re4-title, erster Buchstabe rot, Absatz darunter).
    g_framework->draw_menu_heading("C++");

    struct Entry {
        const char* name;
        std::function<void()> draw;
    };

    std::array<Entry, 24> entries{{
        {"Arm Chain",            [] { RE4VRArmChain::get()->draw_dev_ui(); }},
        {"Binding",              [] { RE4VRBinding::get()->draw_dev_ui(); }},
        {"Choke",                [] { RE4VRChoke::get()->draw_dev_ui(); }},
        {"Crosshair",            [] { RE4VRCrosshair::get()->draw_dev_ui(); }},
        {"FirstPerson",          [] { RE4VRFirstPerson::get()->draw_dev_ui(); }},
        {"Frametimes",           [] { RE4VR::get()->draw_dev_ui(); }},
        {"Guestures",            [] { RE4VRGuestures::get()->draw_dev_ui(); }},
        {"Holster",              [] { RE4VRHolster::get()->draw_dev_ui(); }},
        {"Jiggle",               [] { RE4VRJiggle::get()->draw_dev_ui(); }},
        {"Materials",            [] { RE4VRMaterials::get()->draw_dev_ui(); }},
        {"Menu Editor",          [this] { draw_menu_editor(); }},
        {"Mercenaries (DLC)",    [] { RE4VRMerc::get()->draw_dev_ui(); }},
        {"Minecart",             [] { RE4VRMinecart::get()->draw_dev_ui(); }},
        {"Motion",               [] { RE4VRMotion::get()->draw_dev_ui(); }},
        {"Movement",             [] { RE4VRMovement::get()->draw_dev_ui(); }},
        {"Recoil & Haptic",      [] { RE4VRRecoil::get()->draw_dev_ui(); }},
        {"Reload",               [] { RE4VRReload::get()->draw_dev_main(); }},
        {"Reload Adv",           [] { RE4VRReload::get()->draw_dev_adv(); }},
        {"Reload2",              [] { RE4VRReload::get()->draw_dev_r2(); }},
        {"Reload3",              [] { RE4VRReload::get()->draw_dev_r3(); }},
        {"Reload4 (Separate Ways)",  [] { RE4VRReload::get()->draw_dev_r4(); }},
        {"Reload 5 (Separate Ways)", [] { RE4VRReload::get()->draw_dev_r5(); }},
        {"UI",                   [] { RE4VRUi::get()->draw_dev_ui(); }},
        {"Weapons",              [] { RE4VRWeapons::get()->draw_dev_ui(); }},
    }};

    std::array<const Entry*, entries.size()> order{};

    for (size_t i = 0; i < entries.size(); ++i) {
        order[i] = &entries[i];
    }

    std::sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) {
        return std::string_view{a->name} < std::string_view{b->name};
    });

    for (const auto* e : order) {
        if (e->draw) {
            e->draw();
        }
    }

    // Weapons2 haengt bewusst hinter Weapons (in Lua war es die alphabetisch
    // letzte Datei, und der Baum liest sich als Fortsetzung).
    RE4VRWeapons2::get()->draw_dev_ui();

    // [DEV-UEBERSCHRIFTEN 11.09.2026] Trennstrich + "LUA" -- darunter haengt
    // Mods::draw_developer die Oberflaechen der geladenen Lua-Scripte an.
    g_framework->draw_menu_heading("LUA", true);
}

#endif // RE4
