#pragma once

#if defined(RE4)

#include <chrono>

#include "RE4VR.hpp"

// ============================================================================
// RE4VRMenu -- die ZENTRALE Reihenfolge des REFramework-Hauptmenues.
//
// Ersetzt das fruehere ##re4_vr_menu.lua. Dort sammelte ein Lua-Dispatcher
// (__re4_ui_add) die schlanken Public-Optionen ein, sortierte sie nach `order`
// und zeichnete sie in EINEM on_draw_ui -- deshalb standen sie oben und die
// Entwickler-Trees darunter. Mit dem Port ist das Script abgeschaltet, der
// Dispatcher fehlt, und jeder Mod zeichnet seither selbst: Optionen und Trees
// stehen durcheinander, in der Reihenfolge des Mod-Vektors.
//
// Dieser Mod stellt den alten Zustand nativ wieder her:
//   1. Er steht GANZ VORNE im Mod-Vektor -> er zeichnet als Erster.
//   2. Zuerst die Public-Optionen in fester Reihenfolge (die frueheren
//      `order`-Werte stehen als Kommentar an den Aufrufen).
//   3. Danach die Entwickler-Trees, ALPHABETISCH nach ihrem Anzeigenamen.
//
// Die anderen Mods zeichnen nicht mehr selbst: ihr `on_draw_ui` heisst jetzt
// `draw_dev_ui` und wird ausschliesslich von hier gerufen.
// ============================================================================

// ----------------------------------------------------------------------------
// [DER SCHALTER] Entwickler-Trees ein- oder ausblenden.
//
//   true  = alle "RE4VR - ..."-Trees sichtbar (Arbeitsmodus)
//   false = NUR das nackte Public-UI (Release) -- die Menue-Kategorie
//           "Developer" fehlt dann ganz
//
// Das ist die einzige Stelle, die dafuer geaendert werden muss. Danach neu
// bauen -- am Verhalten des Mods aendert sich nichts, es geht ausschliesslich
// um die Anzeige.
// ----------------------------------------------------------------------------
constexpr bool RE4VR_DEV_UI = false;

class RE4VRMenu : public Mod {
public:
    static std::shared_ptr<RE4VRMenu>& get();

    std::string_view get_name() const override { return "RE4VRMenu"; }

    std::optional<std::string> on_initialize() override;

    // [MENUE-SOUNDS 11.09.2026] Spielt die von REFramework eingereihten
    // Menue-Sounds im Spiel-Thread ueber chainsaw.GuiSoundManager ab.
    void on_frame() override;

    // [MENUE-KATEGORIEN 11.09.2026] Kein on_draw_ui mehr: das Hauptfenster ruft
    // die beiden Bloecke getrennt, je in ihrer Kategorie (Mods.cpp) --
    // draw_public in "Mod Options", draw_dev in "Developer".
    void draw_public();   // das nackte UI, feste Reihenfolge
    void draw_dev();      // die Entwickler-Trees, alphabetisch

private:
    // [VR-MENUE-GROESSE 11.09.2026] Werte des "RE4VR Menu Editor" (Breite und
    // Abstand der VR-Tafel) in reframework/data/re4_vr/re4_vr_menu.json.
    void load_menu_editor_cfg();
    void save_menu_editor_cfg();

    // Der Tree "RE4VR - Menu Editor" -- ein Eintrag in der alphabetischen Liste von draw_dev().
    void draw_menu_editor();

    // [TOXIC SETTINGS 11.09.2026] "Disable Toxic Gamesettings" -- erster Schalter
    // unter Miscellaneous, Default AN. Haelt 13 fuer VR schaedliche Spieloptionen
    // auf ihrem Sollwert (Tabelle in RE4VRMenu.cpp). Geprueft wird nur, solange
    // ein Menue offen ist (nur dort kann man sie umstellen), einmal pro Sekunde.
    // Gespeichert in reframework/data/re4_vr/re4_vr_game_settings.json.
    // Bewusst NICHT in RE4VRBinding: der Eingabepfad bleibt unberuehrt.
    void draw_public_toxic_settings();
    void toxic_settings_tick();
    bool toxic_menu_open();
    void load_game_settings_cfg();
    void save_game_settings_cfg();

    bool m_disable_toxic_settings{true};
    std::chrono::steady_clock::time_point m_toxic_next_check{};

    // ======================================================================
    // [SPLASH 13.09.2026] Begruessung beim ALLERERSTEN Start des Mods.
    //
    // Erkennung ueber eine eigene Marker-Datei
    // (reframework/data/re4_vr/re4_vr_splash.json): fehlt sie, war der Mod hier
    // noch nie an. BEWUSST eine eigene Datei und nicht eine bestehende Config --
    // die werden auch aus anderen Gruenden neu angelegt, und diese hier kann man
    // zum erneuten Anzeigen einfach loeschen. Beim Ausliefern darf sie NICHT
    // mitgepackt werden.
    //
    // Gezeigt wird KEIN eigenes Fenster: waehrend der Splash laeuft, zeichnet
    // draw_public() nur ihn und sonst nichts. Damit hat er automatisch exakt
    // Groesse, Abstand und Weltlage der Menue-Tafel (OverlayComponent stellt sie
    // beim Oeffnen einmal vor den Kopf).
    //
    // Geschlossen wird er von SELBST nach SPLASH_SECONDS -- keine Tastenkombi.
    // ======================================================================
    void splash_tick();

public:
    // [EIGENES PANEL 13.09.2026] Der Splash ist KEIN Menue-Inhalt mehr: er wird
    // von REFramework::draw_ui() VOR dem Menuefenster gezeichnet, und das Menue
    // wird dann gar nicht erst aufgebaut. Sonst saehe man Titelzeile,
    // Kategorienleiste und Rahmen mit -- gewollt ist nur der Text.
    bool splash_active() const { return m_splash_active; }
    void draw_splash_window();

private:

    bool m_splash_checked{false};
    bool m_splash_active{false};
    std::chrono::steady_clock::time_point m_splash_t0{};
};

#endif // RE4
