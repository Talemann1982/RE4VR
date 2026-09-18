#pragma once

#include "Mod.hpp"

class Mods {
public:
    Mods();
    virtual ~Mods() {}

    std::optional<std::string> on_initialize() const;
    std::optional<std::string> on_initialize_d3d_thread() const;

    void on_pre_imgui_frame() const;
    void on_frame() const;
    void on_present() const;
    void on_post_frame() const;
    // [MENUE-KATEGORIEN 11.09.2026] Das Hauptfenster hat links Kategorien und
    // rechts deren Inhalt. Jede Kategorie mit Mod-Inhalt hat hier ihre eigene
    // Zeichenfunktion; REFramework::draw_menu_detail ruft nur die gewaehlte.
    void draw_upscaler() const;      // "Upscaler"
    void draw_mod_options() const;   // "Mod Options"
    void draw_developer() const;     // "Developer" -- nur bei RE4VR_DEV_UI
    bool has_developer() const;      // false im Public-Release -> Kategorie fehlt

    // [REF OPTIONS 10.09.2026] Zeichnet die REFramework-EIGENEN Trees (VR,
    // Camera, Graphics, FreeCam, SceneMods, Hooks, LooseFileLoader, APIProxy,
    // PluginLoader ...). Wird vom Hauptfenster in der Kategorie
    // "REFramework Options" aufgerufen.
    void draw_ref_trees() const;
    void on_device_reset() const;

    const auto& get_mods() const {
        return m_mods;
    }

private:
    std::vector<std::shared_ptr<Mod>> m_mods;
};