#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <utility/Patch.hpp>

#include <../../directxtk12-src/Inc/GraphicsMemory.h>
#include "mods/vr/d3d12/CommandContext.hpp"

class Mods;
class REGlobals;
class RETypes;

#include "D3D11Hook.hpp"
#include "D3D12Hook.hpp"
#include "DInputHook.hpp"
#include "WindowsMessageHook.hpp"

// Global facilitator
class REFramework {
private:
    void hook_monitor();
    std::atomic<uint32_t> m_do_not_hook_d3d_count{0};

public:
    struct DoNotHook {
        DoNotHook(std::atomic<uint32_t>& count) : m_count(count) {
            ++m_count;
        }
    
        ~DoNotHook() {
            --m_count;
        }

    private:
        std::atomic<uint32_t>& m_count;
    };

    DoNotHook acquire_do_not_hook_d3d() {
        return DoNotHook{m_do_not_hook_d3d_count};
    }


public:
    REFramework(HMODULE reframework_module);
    virtual ~REFramework();

    static auto get_reframework_module() { return s_reframework_module; }
    static void set_reframework_module(HMODULE module) { s_reframework_module = module; }

    bool is_valid() const { return m_valid; }

    bool is_dx11() const { return m_is_d3d11; }

    bool is_dx12() const { return m_is_d3d12; }

    const auto& get_mods() const { return m_mods; }

    const auto& get_mouse_delta() const { return m_mouse_delta; }
    const auto& get_keyboard_state() const { return m_last_keys; }

    Address get_module() const { return m_game_module; }

    bool is_ready() const { return m_initialized && m_game_data_initialized; }
    bool is_game_data_initialized() const { return m_game_data_initialized; }
    bool is_ui_focused() const { return m_is_ui_focused; }

    void run_imgui_frame(bool from_present);

    void on_frame_d3d11();
    void on_post_present_d3d11();
    void on_frame_d3d12();
    void on_post_present_d3d12();
    void on_reset();

    void patch_set_cursor_pos();
    void remove_set_cursor_pos_patch();

    bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param);
    void on_direct_input_keys(const std::array<uint8_t, 256>& keys);

    static inline bool s_fallback_appdata{false};
    static inline bool s_checked_file_permissions{false};
    static std::filesystem::path get_persistent_dir();
    static std::filesystem::path get_persistent_dir(const std::string& dir) {
        return get_persistent_dir() / dir;
    }

    void request_save_config() {
        m_wants_save_config = true;
    }

    enum class RendererType : uint8_t {
        D3D11,
        D3D12
    };
    
    auto get_renderer_type() const { return m_renderer_type; }
    auto& get_d3d11_hook() const { return m_d3d11_hook; }
    auto& get_d3d12_hook() const { return m_d3d12_hook; }

    auto get_window() const { return m_wnd; }
    auto get_last_window_pos() const { return m_last_window_pos; } // REFramework imgui window
    auto get_last_window_size() const { return m_last_window_size; } // REFramework imgui window

    static const char* get_game_name() {
    #if defined(RE2)
        return "re2";
    #elif defined(RE3)
        return "re3";
    #elif defined(RE4)
        return "re4";
    #elif defined(RE7)
        return "re7";
    #elif defined(RE8)
        return "re8";
    #elif defined(DMC5)
        return "dmc5";
    #elif defined(MHRISE)
        return "mhrise";
    #elif defined(SF6)
        return "sf6";
    #elif defined(DD2)
        return "dd2";
    #elif defined(MHWILDS)
        return "mhwilds";
    #else
        return "unknown";
    #endif
    }

    bool is_drawing_ui() const {
        return m_draw_ui;
    }

    void set_draw_ui(bool state, bool should_save = true);

    auto& get_hook_monitor_mutex() {
        return m_hook_monitor_mutex;
    }

    auto& get_startup_mutex() {
        return m_startup_mutex;
    }

    void set_font_size(int size) { 
        if (m_font_size != size) {
            m_font_size = size;
            m_fonts_need_updating = true;
        }
    }

    auto get_font_size() const { return m_font_size; }

    // [UEBERSCHRIFTEN 11.09.2026] Abschnitts-Ueberschrift im Menue: mittig,
    // erster Buchstabe knallrot, Rest weiss, in re4-title,
    // HEADING_FONT_EXTRA px groesser als der uebrige Text, darunter
    // eine halbe Zeile Abstand. Genutzt von "Upscaling" (TemporalUpscaler),
    // "Rendering Technique" (VR) und den Ueberschriften in "Mod Options" --
    // Aussehen nur hier aendern. gap_before = DAVOR Absatz + roter Trennstrich
    // + Absatz (fuer jede Ueberschrift, die nicht ganz oben steht).
    static constexpr int HEADING_FONT_EXTRA = 10;   // [11.09.2026] erst 2, dann 6 -- re4-title braucht Groesse, sonst gequetscht
    void draw_menu_heading(const char* text, bool gap_before = false);

    // Abstand zwischen den Eintraegen einer Auswahl-Reihe (nebeneinander).
    // [LINKSBUENDIG 11.09.2026] Die Helfer fuer mittige Reihen sind entfallen.
    static constexpr float MENU_BUTTON_GAP = 12.0f;

    // [WEISSER RAHMEN 11.09.2026] Toggle (ImGui::Checkbox) mit weissem Rahmen.
    // [ROTER HAKEN 11.09.2026] Haken knallrot -- dieselbe Farbe fuer alle Toggles
    // und Auswahlen im Menue, auch die, die selbst zeichnen (ModToggle).
    static constexpr ImVec4 MENU_CHECKMARK_COLOR{1.0f, 0.0f, 0.0f, 1.0f};
    bool draw_menu_checkbox(const char* label, bool* v);

    // [MENUE-TOGGLE 11.09.2026] Der Stil dazu, auch fuer Stellen, die selbst zeichnen
    // (ModToggle): Rahmen 1 px dicker als im Theme, eckig, weiss bzw. border_color,
    // Haken knallrot. Immer paarweise mit pop_menu_toggle_style.
    void push_menu_toggle_style();
    void push_menu_toggle_style(const ImVec4& border_color);
    void pop_menu_toggle_style();

    // [MENUE-AUSWAHL 11.09.2026] Auswahl statt Knopf, sieht aus wie ein Toggle
    // (Kaestchen, weisser Rahmen, Haken bei der gewaehlten Option).
    // Gibt true beim Anklicken.
    // Die zweite Form faerbt den Rahmen (Farbauswahl: in der jeweiligen Farbe).
    bool draw_menu_radio(const char* label, bool active);
    bool draw_menu_radio(const char* label, bool active, const ImVec4& border_color);

    // [VR-MENUE-KONTEXT 11.09.2026] Das Menue hat in VR einen EIGENEN ImGui-
    // Kontext mit fester Groesse -- unabhaengig von der Desktop-Aufloesung
    // (Vorbild: re7_vr_settings_overlay.dll). Er zeichnet das Menuefenster
    // bildschirmfuellend in RTV::IMGUI, das genau VR_MENU_WIDTH x VR_MENU_HEIGHT
    // gross ist; OpenVR-Overlay und OpenXR-Quad zeigen diese Textur GANZ.
    // Der Desktop behaelt sein normales Fenster, beide gehen gemeinsam auf/zu.
    static constexpr float VR_MENU_WIDTH = 1920.0f;
    static constexpr float VR_MENU_HEIGHT = 1080.0f;
    // [ABSTAND SPALTEN 11.09.2026] Luft zwischen linker Spalte und rechtem Inhalt, px bei 1080.
    static constexpr float MENU_DETAIL_GAP = 24.0f;

    // [VR-MENUE-GROESSE 11.09.2026] Breite und Abstand der VR-Tafel in Metern.
    // Einstellbar im Dev-Build unter Developer -> "RE4VR Menu Editor" (live),
    // gespeichert von RE4VRMenu in reframework/data/re4_vr/re4_vr_menu.json. Sobald die
    // Werte passen, wandern sie hier als Defaults fest hinein.
    static constexpr float VR_MENU_PANEL_WIDTH_DEFAULT = 1.0f;
    static constexpr float VR_MENU_PANEL_DISTANCE_DEFAULT = 1.0f;

    float get_vr_menu_panel_width() const { return m_vr_menu_panel_width; }
    float get_vr_menu_panel_distance() const { return m_vr_menu_panel_distance; }
    void set_vr_menu_panel_width(float meters) { m_vr_menu_panel_width = std::clamp(meters, 0.25f, 5.0f); }
    void set_vr_menu_panel_distance(float meters) { m_vr_menu_panel_distance = std::clamp(meters, 0.25f, 5.0f); }

    // [VR-SCHRIFT 11.09.2026] Textgroessen im VR-Menue, 1.0 = wie bisher.
    //   nav    = linke Spalte (Kategorien in re4-title), die Spalte waechst mit
    //   detail = rechter Bereich: Text, Ueberschriften, Innenraender und Abstaende
    //            wachsen GEMEINSAM, das Verhaeltnis bleibt
    // Das VR-Menue nutzt dafuer eigene Schriften, VR_FONT_OVERSAMPLE-fach so gross
    // gerastert und herunterskaliert -- bis 3.0 bleibt es damit scharf. Seit dem
    // Vollbild-Umbau gelten die Werte auch am Desktop, dort zusaetzlich mit der
    // Fensterhoehe / 1080 skaliert.
    static constexpr int VR_FONT_OVERSAMPLE = 3;
    float get_vr_menu_nav_text_scale() const { return m_vr_menu_nav_text_scale; }
    float get_vr_menu_detail_text_scale() const { return m_vr_menu_detail_text_scale; }
    // [BIS 5.0 16.09.2026] War 3.0. Die Schrift ist VR_FONT_OVERSAMPLE-fach (3) gerastert --
    // oberhalb von 3.0 wird sie hochskaliert und damit etwas weicher.
    void set_vr_menu_nav_text_scale(float scale) { m_vr_menu_nav_text_scale = std::clamp(scale, 0.5f, 5.0f); }
    void set_vr_menu_detail_text_scale(float scale) { m_vr_menu_detail_text_scale = std::clamp(scale, 0.5f, 3.0f); }

    // [LOGO-GROESSE 25.09.2026] Logo links oben, je Zeile ein Faktor auf die
    // Kategorie-Schrift: "RESIDENT EVIL 4" und "VR".
    float get_vr_menu_logo_top_scale() const { return m_vr_menu_logo_top_scale; }
    float get_vr_menu_logo_vr_scale() const { return m_vr_menu_logo_vr_scale; }
    void set_vr_menu_logo_top_scale(float scale) { m_vr_menu_logo_top_scale = std::clamp(scale, 0.3f, 2.0f); }
    void set_vr_menu_logo_vr_scale(float scale) { m_vr_menu_logo_vr_scale = std::clamp(scale, 0.3f, 2.0f); }

    // [LOGO-ABSTAND 26.09.2026] "VR" relativ zur Zeilenhoehe von "RESIDENT EVIL 4" nach oben/unten
    // (0 = alter Stand), Leerraum zwischen "VR" und MOD OPTIONS in Kategorie-Zeilen (1 = alter Stand).
    float get_vr_menu_logo_vr_gap() const { return m_vr_menu_logo_vr_gap; }
    float get_vr_menu_logo_nav_gap() const { return m_vr_menu_logo_nav_gap; }
    void set_vr_menu_logo_vr_gap(float v) { m_vr_menu_logo_vr_gap = std::clamp(v, -1.0f, 1.0f); }
    void set_vr_menu_logo_nav_gap(float v) { m_vr_menu_logo_nav_gap = std::clamp(v, -1.0f, 2.0f); }

    // [STICK-DEADZONE 11.09.2026] Ab welchem Ausschlag die Sticks im VR-Menue
    // navigieren, scrollen und Slider verstellen (0..1). Frueher fest 0.55.
    // [KATEGORIE-RUNDUNG 11.09.2026] Eckenrundung der roten Markierung in der
    // Kategorien-Spalte, in Pixeln bei Massstab 1 (waechst mit "Category Text Size"
    // und der Aufloesung mit). Frueher fest style.FrameRounding (5).
    float get_vr_menu_nav_rounding() const { return m_vr_menu_nav_rounding; }
    void set_vr_menu_nav_rounding(float px) { m_vr_menu_nav_rounding = std::clamp(px, 0.0f, 40.0f); }

    float get_vr_menu_stick_deadzone() const { return m_vr_menu_stick_deadzone; }
    void set_vr_menu_stick_deadzone(float deadzone) { m_vr_menu_stick_deadzone = std::clamp(deadzone, 0.2f, 0.95f); }

    // [STICK-WIEDERHOLUNG 11.09.2026] Stick gehalten: erst nach "delay" Sekunden
    // wiederholt sich der Schritt, danach alle "interval" Sekunden. Gemeint ist das
    // Weiterspringen des Cursors; das Slider-Verstellen laeuft in ImGui fest
    // 2,67-mal so schnell. ImGuis Vorgabe war 0,2 s / 0,04 s -- viel zu hektisch.
    float get_vr_menu_repeat_delay() const { return m_vr_menu_repeat_delay; }
    // [SCROLLTEMPO 14.09.2026 -- Ansage "Slider fuer Scrollgeschwindigkeit"]
    // Pixel pro Sekunde bei voll ausgelenktem rechten Stick.
    float get_vr_menu_scroll_speed() const { return m_vr_menu_scroll_speed; }
    void set_vr_menu_scroll_speed(float px) { m_vr_menu_scroll_speed = std::clamp(px, 200.0f, 4000.0f); }

    // [TRACKPAD-SCROLL 15.09.2026] Eigener Faktor: das Pad liefert kleinere
    // Auslenkungen als der Stick, mit dem Stick-Tempo fuehlt es sich zaeh an.
    float get_vr_menu_trackpad_scale() const { return m_vr_menu_trackpad_scale; }
    void set_vr_menu_trackpad_scale(float f) { m_vr_menu_trackpad_scale = std::clamp(f, 0.5f, 8.0f); }

    float get_vr_menu_repeat_interval() const { return m_vr_menu_repeat_interval; }
    void set_vr_menu_repeat_delay(float seconds) { m_vr_menu_repeat_delay = std::clamp(seconds, 0.1f, 1.0f); }
    void set_vr_menu_repeat_interval(float seconds) { m_vr_menu_repeat_interval = std::clamp(seconds, 0.03f, 0.6f); }

    // [HINTERGRUND 11.09.2026] Deckkraft von Menu.png hinter dem Menue (0 = unsichtbar,
    // dann scheint das Spiel durch). Menu Editor, gespeichert in re4_vr/re4_vr_menu.json.
    float get_vr_menu_background_opacity() const { return m_vr_menu_background_opacity; }
    void set_vr_menu_background_opacity(float opacity) { m_vr_menu_background_opacity = std::clamp(opacity, 0.0f, 1.0f); }

    // Feste Pixelabstaende im Menue (Knopfabstand, Zeilenluft) -- im skalierten
    // rechten VR-Bereich mitgewachsen, sonst unveraendert.
    float menu_px(float px) const { return px * m_menu_px_scale; }

    // [SPLASH 13.09.2026] Der Begruessungs-Splash (RE4VRMenu) zeichnet mit
    // EXAKT denselben Schriften und demselben Massstab wie das Menue -- sonst
    // saehe er daneben aus. Reine Lese-Getter.
    ImFont* vr_font_base() const { return m_vr_font_base; }
    ImFont* vr_font_heading() const { return m_vr_font_heading; }
    float menu_draw_scale() const { return m_menu_draw_scale; }
    // [MENUE-SOUNDS 11.09.2026] Klick-Sounds des Mod-Menues. REFramework erkennt
    // die Aktion (Desktop- wie VR-Kontext) und reiht sie ein; abgespielt wird im
    // Spiel-Thread von RE4VRMenu::on_frame ueber den GuiSoundManager des Spiels.
    enum class MenuSound : uint8_t {
        Open,       // Menue auf/zu (LT + linkes B, Insert, X)
        Tree,       // Tree / CollapsingHeader auf- oder zuklappen
        Toggle,     // Checkbox, Auswahlpunkt, Knopf
        Slider,     // Slider/Drag verschieben (hoechstens alle 50 ms)
        Category,   // Kategorie links wechseln
        Cursor,     // [12.09.2026] Markierung wandert (Stick hoch/runter, Spaltenwechsel)
    };

    std::vector<MenuSound> take_menu_sounds();

    // Aus ImGuiTestEngineHook_ItemInfo -- flags = ImGuiItemStatusFlags.
    void on_imgui_item_info(ImGuiContext* ctx, ImGuiID id, int flags);

    int add_font(const std::filesystem::path& filepath, int size, const std::vector<ImWchar>& ranges = {});

    ImFont* get_font(int index) const {
        if (index >= 0 && index < m_additional_fonts.size()) {
            return m_additional_fonts[index].font;
        } else {
            return nullptr;
        }
    }

private:
        void save_config();
    void consume_input();
    void update_fonts();
    void invalidate_device_objects();

    void draw_ui();
    void draw_about();

    // [MENUE-KATEGORIEN 11.09.2026] Hauptfenster: links die Kategorien, rechts
    // der Inhalt der gewaehlten. Die Auswahl lebt nur fuer die laufende
    // Sitzung, beim Spielstart steht "Mod Options" (der oberste Eintrag).
    enum class MenuCategory : int {
        ModOptions,
        Upscaler,
        Bindings,     // Bild der Controller-Belegung
        Developer,    // nur bei RE4VR_DEV_UI
        RefOptions,
    };

    // Anzahl der Eintraege oben -- bei einer neuen Kategorie mitziehen
    // (m_vr_menu.detail_last_id/-scroll haengen daran).
    static constexpr size_t MENU_CATEGORY_COUNT = 5;

    void draw_menu_nav();
    void draw_menu_detail();
    void draw_ref_options();   // Inhalt der Kategorie "REFramework Options"

    // Das Menuefenster selbst (Titel, Kategorien, Inhalt) -- fuer den Desktop
    // (vr = false, 600x500, schliessbar) und im VR-Kontext (vr = true,
    // bildschirmfuellend). Rueckgabe: Fenster noch offen.
    bool draw_menu_window(bool vr);

    // [BINDINGS 11.09.2026] Kategorie "Bindings": das Bild in voller Breite des
    // rechten Bereichs, Hoehe aus dem Seitenverhaeltnis -- waechst also mit dem
    // Fenster (Desktop 600x500 wie VR 1920x1080). Nur D3D12.
    void draw_bindings_image();
    // [HINTERGRUND 11.09.2026] Menu.png bildfuellend hinter das Menue; false = nicht verfuegbar.
    bool draw_menu_background();
    // Laedt die eingebetteten Menue-Bilder (Bindings + Hintergrund).
    void create_menu_images_d3d12(ID3D12Device* device);

    // Ein Frame des VR-Menue-Kontexts; laeuft in run_imgui_frame direkt nach
    // dem Desktop-Frame (m_imgui_mtx gehalten), gerendert in on_frame_d3d12.
    void run_vr_menu_frame();

    // [TASTATUR-NAVIGATION 16.09.2026 -- Ansage des Users] Das Desktop-Menue mit
    // der Tastatur bedienen, nach denselben Regeln wie der Controller im VR-Menue:
    // Pfeiltasten = Richtung, Leertaste = auswaehlen, Backspace = zurueck (in der
    // Kategorien-Spalte: Menue zu). Laeuft in run_imgui_frame VOR ImGui::NewFrame.
    void run_desk_menu_keyboard();
    // Liefert nach dem Desktop-Frame true, wenn Backspace das Menue schliessen soll.
    bool m_desk_menu_close{false};

    // Die Tasten, die die Tastatur-Navigation selbst an ImGui gibt. Ihre echten
    // Nachrichten gehen dann NICHT an ImGui (on_message) -- ausser ein Textfeld ist aktiv.
    static bool is_desk_menu_nav_key(WPARAM vk) {
        return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT
               || vk == VK_SPACE || vk == VK_BACK;
    }

    struct MenuNavState {
        ImGuiContext* ctx{nullptr};
        ImFontAtlas* atlas{nullptr};   // geteilt mit dem Desktop-Kontext
        bool has_draw_data{false};

        // [MENUE-STEUERUNG 11.09.2026] Controller-Navigation (kein Laser mehr).
        bool drawing{false};            // gerade im VR-Kontext am Zeichnen
        bool was_open{false};           // Menuezustand im letzten Frame
        void* nav_window{nullptr};      // ImGuiWindow* der Kategorien-Spalte
        int nav_item_flags{0};          // ItemStatusFlags unter dem Nav-Cursor (letzter Frame)
        int frame_nav_item_flags{0};    // ... im laufenden Frame gesammelt
        bool jump_to_nav{false};        // Cursor auf die gewaehlte Kategorie setzen
        float scroll_delta{0.0f};       // rechter Stick -> rechter Bereich
        bool tweaking{false};           // markierter Slider wird gerade verstellt
        bool face_down_pulse_prev{false};
        bool l_a_prev{false};
        int dir_prev{0};

        // [SPALTENWECHSEL 11.09.2026] ImGui findet links/rechts nur Elemente auf
        // gleicher Hoehe -- den Wechsel zwischen den Spalten machen wir selbst.
        bool jump_to_detail{false};     // Cursor auf den ersten bedienbaren Eintrag rechts
        ImGuiID left_probe_id{0};       // NavId beim Druck nach links ...
        int left_probe_frames{0};       // ... hat sie sich nach 3 Frames nicht bewegt -> zur Kategorie
        bool rs_left_prev{false};

        // [SPALTENWECHSEL MERKEN 11.09.2026] Letzter Cursor-Eintrag und Scroll im
        // rechten Bereich, je Kategorie -- "rechts" aus der Kategorien-Spalte kehrt
        // dorthin zurueck statt nach ganz oben. Ist der Eintrag nicht mehr da (Tree
        // zu), gilt der erste bedienbare Eintrag im Frame (fallback).
        ImGuiID detail_last_id[MENU_CATEGORY_COUNT]{};
        float detail_last_scroll[MENU_CATEGORY_COUNT]{};
        ImGuiID detail_fallback_id{0};
        void* detail_fallback_window{nullptr};   // ImGuiWindow* des Fallback-Eintrags

        // [KATEGORIE-RAND 11.09.2026] Oberster/unterster Eintrag links. "hoch" am
        // obersten bzw. "runter" am untersten tut nichts -- vorher sprang der
        // Cursor aus der Spalte und die Markierung war weg. Kein Rundlauf links.
        ImGuiID nav_first_id{0};
        ImGuiID nav_last_id{0};

        // [CURSOR-SOUND 12.09.2026] NavId des letzten Frames -- wandert die
        // Markierung, gibt es einen Ton.
        ImGuiID nav_prev_id{0};

        // [TASTATUR-NAVIGATION 16.09.2026] Nur Desktop: Flanken von Leertaste/Backspace.
        bool key_select_prev{false};
        bool key_back_prev{false};
        bool cancel_pulse_prev{false};   // Escape-Impuls im letzten Frame gesendet
    };

    MenuNavState m_vr_menu{};
    // [TASTATUR-NAVIGATION 16.09.2026] Derselbe Zustand fuer das Desktop-Menue.
    MenuNavState m_desk_menu{};

    // Zustand des Kontexts, der GERADE das Menue zeichnet -- sonst nullptr.
    MenuNavState* active_menu_nav() {
        if (m_vr_menu.drawing) {
            return &m_vr_menu;
        }

        return m_desk_menu.drawing ? &m_desk_menu : nullptr;
    }

    // [VR-MENUE-GROESSE 11.09.2026]
    float m_vr_menu_panel_width{VR_MENU_PANEL_WIDTH_DEFAULT};
    float m_vr_menu_panel_distance{VR_MENU_PANEL_DISTANCE_DEFAULT};

    // [VR-SCHRIFT 11.09.2026]
    float m_vr_menu_nav_text_scale{1.0f};
    float m_vr_menu_detail_text_scale{1.0f};
    float m_vr_menu_logo_top_scale{0.72f};
    float m_vr_menu_logo_vr_scale{1.0f};
    float m_vr_menu_logo_vr_gap{-0.35f};   // [LOGO-ABSTAND]
    float m_vr_menu_logo_nav_gap{0.25f};   // [LOGO-ABSTAND]
    float m_vr_menu_stick_deadzone{0.7f};   // [STICK-DEADZONE] war fest 0.55 -- zu empfindlich
    float m_vr_menu_nav_rounding{5.0f};     // [KATEGORIE-RUNDUNG] = bisheriges FrameRounding
    float m_vr_menu_repeat_delay{0.45f};    // [STICK-WIEDERHOLUNG] Sekunden bis zur ersten Wiederholung
    float m_vr_menu_repeat_interval{0.2f};  // [STICK-WIEDERHOLUNG] Sekunden zwischen Wiederholungen
    float m_vr_menu_scroll_speed{900.0f};   // [SCROLLTEMPO 14.09.2026] Pixel/Sekunde, rechter Stick
    float m_vr_menu_trackpad_scale{2.5f};   // [TRACKPAD-SCROLL 15.09.2026] Faktor auf das Stick-Tempo
    float m_vr_menu_background_opacity{1.0f};  // [HINTERGRUND] Deckkraft von Menu.png
    float m_menu_px_scale{1.0f};
    float m_menu_draw_scale{0.0f};   // [VOLLBILD] Massstab waehrend draw_menu_window, sonst 0

    // [MENUE-SOUNDS 11.09.2026]
    void queue_menu_sound(MenuSound sound);

    std::mutex m_menu_sound_mtx{};
    std::vector<MenuSound> m_menu_sounds{};
    // Letzter Checked-/Opened-Zustand je Widget, getrennt je ImGui-Kontext.
    std::unordered_map<ImGuiContext*, std::unordered_map<ImGuiID, uint8_t>> m_menu_item_states{};
    std::chrono::steady_clock::time_point m_last_slider_sound{};
    std::optional<bool> m_menu_sound_prev_draw_ui{};

    MenuCategory m_menu_category{MenuCategory::ModOptions};

public:
    bool hook_d3d11();
    bool hook_d3d12();

private:
    bool initialize();
    bool initialize_game_data();
    bool initialize_windows_message_hook();

    bool first_frame_initialize();

    void call_on_frame();

    static inline HMODULE s_reframework_module{};

    bool m_first_frame{true};
    bool m_first_frame_d3d_initialize{true};
    bool m_is_d3d12{false};
    bool m_is_d3d11{false};
    bool m_valid{false};
    bool m_initialized{false};
    bool m_created_default_cfg{false};
    bool m_started_game_data_thread{false};
    std::atomic<bool> m_terminating{false}; // Destructor is called
    std::atomic<bool> m_game_data_initialized{false};
    std::atomic<bool> m_mods_fully_initialized{false};
    
    // UI
    bool m_has_frame{false};
    bool m_wants_device_object_cleanup{false};
    bool m_wants_save_config{false};
    // [MENUE ZU BEIM START 11.09.2026] War true: REFramework oeffnete das Menue beim
    // Spielstart von selbst. Jetzt zu -- geoeffnet wird nur ueber LT + linkes B oder
    // Insert. (RememberMenuState steht in der Config auf false, laedt also nichts.)
    bool m_draw_ui{false};
    bool m_last_draw_ui{m_draw_ui};
    bool m_is_ui_focused{false};
    bool m_cursor_state{false};
    bool m_cursor_state_changed{true};
    bool m_ui_option_transparent{true};
    bool m_ui_passthrough{false};
    
    ImVec2 m_last_window_pos{};
    ImVec2 m_last_window_size{};

    struct AdditionalFont {
        std::filesystem::path filepath{};
        int size{16};
        std::vector<ImWchar> ranges{};
        ImFont* font{};
    };

    bool m_fonts_need_updating{true};
    int m_font_size{16};
    std::vector<AdditionalFont> m_additional_fonts{};
    ImFont* m_heading_font{nullptr};   // Ueberschriften, wird in update_fonts neu gesetzt
    ImFont* m_vr_font_base{nullptr};      // [VR-SCHRIFT] Grundschrift, VR_FONT_OVERSAMPLE-fach gerastert
    ImFont* m_vr_font_heading{nullptr};   // [VR-SCHRIFT] re4-title, VR_FONT_OVERSAMPLE-fach gerastert
    ImFont* m_vr_font_nav{nullptr};       // [KATEGORIE-SCHRIFT] = m_vr_font_heading (re4-title)

    std::mutex m_input_mutex{};
    std::recursive_mutex m_config_mtx{};
    std::recursive_mutex m_imgui_mtx{};
    std::recursive_mutex m_patch_mtx{};

    HWND m_wnd{0};
    HMODULE m_game_module{0};

    float m_accumulated_mouse_delta[2]{};
    float m_mouse_delta[2]{};
    std::array<uint8_t, 256> m_last_keys{0};
    std::unique_ptr<D3D11Hook> m_d3d11_hook{};
    std::unique_ptr<D3D12Hook> m_d3d12_hook{};
    std::unique_ptr<WindowsMessageHook> m_windows_message_hook;
    std::unique_ptr<DInputHook> m_dinput_hook;
    std::shared_ptr<spdlog::logger> m_logger;
    Patch::Ptr m_set_cursor_pos_patch{};

    std::string m_error{""};

    // Game-specific stuff
    std::unique_ptr<Mods> m_mods;

    std::recursive_mutex m_hook_monitor_mutex{};
    std::recursive_mutex m_startup_mutex{};
    std::unique_ptr<std::jthread> m_d3d_monitor_thread{};
    std::chrono::steady_clock::time_point m_last_present_time{};
    std::chrono::steady_clock::time_point m_last_message_time{};
    std::chrono::steady_clock::time_point m_last_sendmessage_time{};
    std::chrono::steady_clock::time_point m_last_chance_time{};
    uint32_t m_frames_since_init{0};
    bool m_has_last_chance{true};
    bool m_first_initialize{true};

    bool m_sent_message{false};
    bool m_message_hook_requested{false};

    RendererType m_renderer_type{RendererType::D3D11};

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

private: // D3D misc
    void set_imgui_style() noexcept;

private: // D3D11 Init
    bool init_d3d11();
    void deinit_d3d11();

private: // D3D12 Init
    bool init_d3d12();
    void deinit_d3d12();

private: // D3D11 members
    struct D3D11 {
        ComPtr<ID3D11Texture2D> blank_rt{};
		ComPtr<ID3D11Texture2D> rt{};
        ComPtr<ID3D11RenderTargetView> blank_rt_rtv{};
		ComPtr<ID3D11RenderTargetView> rt_rtv{};
		ComPtr<ID3D11ShaderResourceView> rt_srv{};
        uint32_t rt_width{};
        uint32_t rt_height{};
		ComPtr<ID3D11RenderTargetView> bb_rtv{};
    } m_d3d11{};

public:
    auto& get_blank_rendertarget_d3d11() { return m_d3d11.blank_rt; }
    auto& get_rendertarget_d3d11() { return m_d3d11.rt; }
    auto get_rendertarget_width_d3d11() const { return m_d3d11.rt_width; }
    auto get_rendertarget_height_d3d11() const { return m_d3d11.rt_height; }

private: // D3D12 members
    struct D3D12 {
        std::vector<std::unique_ptr<d3d12::CommandContext>> cmd_ctxs{};
        uint32_t cmd_ctx_index{0};

        enum class RTV : int{
            BACKBUFFER_0,
            BACKBUFFER_1,
            BACKBUFFER_2,
            BACKBUFFER_3,
            BACKBUFFER_4,
            BACKBUFFER_5,
            BACKBUFFER_6,
            BACKBUFFER_7,
            BACKBUFFER_8,
            BACKBUFFER_LAST = BACKBUFFER_8,
            IMGUI,
            BLANK,
            COUNT,
        };

        enum class SRV : int {
            IMGUI_FONT_BACKBUFFER,
            IMGUI_FONT_VR,
            IMGUI_VR,
            BLANK,
            RE4VR_BINDINGS_IMAGE,   // [BINDINGS 11.09.2026] Bild der Menue-Kategorie "Bindings"
            RE4VR_MENU_BACKGROUND,  // [HINTERGRUND 11.09.2026] Menu.png hinter dem ganzen Menue
            COUNT
        };

        ComPtr<ID3D12DescriptorHeap> rtv_desc_heap{};
        ComPtr<ID3D12DescriptorHeap> srv_desc_heap{};
        ComPtr<ID3D12Resource> rts[(int)RTV::COUNT]{};

        auto& get_rt(RTV rtv) { return rts[(int)rtv]; }

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_rtv(ID3D12Device* device, RTV rtv) {
            return {rtv_desc_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)rtv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
        }

        D3D12_CPU_DESCRIPTOR_HANDLE get_cpu_srv(ID3D12Device* device, SRV srv) {
            return {srv_desc_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)srv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        }

        D3D12_GPU_DESCRIPTOR_HANDLE get_gpu_srv(ID3D12Device* device, SRV srv) {
            return {srv_desc_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                    (SIZE_T)srv * (SIZE_T)device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)};
        }

        uint32_t rt_width{};
        uint32_t rt_height{};

        // [BINDINGS 11.09.2026] Textur zum eingebetteten re4vr_bindings.jpg, SRV
        // liegt im Slot SRV::RE4VR_BINDINGS_IMAGE. Leer, wenn das Laden scheiterte.
        ComPtr<ID3D12Resource> bindings_image{};
        // [HINTERGRUND 11.09.2026] Textur zum eingebetteten Menu.png, SRV im Slot
        // SRV::RE4VR_MENU_BACKGROUND. Leer, wenn das Laden scheiterte.
        ComPtr<ID3D12Resource> menu_background{};
        std::array<void*, 2> imgui_backend_datas{};
        std::unique_ptr<DirectX::DX12::GraphicsMemory> graphics_memory{}; // for use in several places around REF
    } m_d3d12{};

public:
    auto& get_blank_rendertarget_d3d12() { return m_d3d12.get_rt(D3D12::RTV::BLANK); }
    auto& get_rendertarget_d3d12() { return m_d3d12.get_rt(D3D12::RTV::IMGUI); }
    auto get_rendertarget_width_d3d12() { return m_d3d12.rt_width; }
    auto get_rendertarget_height_d3d12() { return m_d3d12.rt_height; }

private:
    // [MENUE-BILDER 11.09.2026] Eingebettetes JPG/PNG per WIC in eine Textur, SRV in
    // `slot`. Steht hier unten, weil D3D12::SRV erst oben im struct bekannt wird.
    ComPtr<ID3D12Resource> load_embedded_image_d3d12(ID3D12Device* device, const unsigned char* data, size_t size,
                                                     D3D12::SRV slot, const wchar_t* name);
};

extern std::unique_ptr<REFramework> g_framework;
