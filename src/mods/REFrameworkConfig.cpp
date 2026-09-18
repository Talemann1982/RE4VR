#include "../REFramework.hpp"

#include "REFrameworkConfig.hpp"

#ifdef RE4
#include <vector>
#include <Windows.h>
// GetFileVersionInfo* leben in version.dll -- der Fork linkt sie sonst nirgends.
#pragma comment(lib, "version.lib")

namespace {
// [RE4 1.5.9.0 TEXTURLAYOUT 2026-09-09] Welche re4.exe laeuft gerade?
//
// Die Exe weist sich selbst sauber aus -- nachgemessen an beiden Fassungen:
//   vor dem Update : FileVersion 1.1.1.0
//   ab dem Update  : FileVersion 1.5.9.0
// Unsere DLL steckt als dinput8.dll IM Spielprozess, also reicht
// GetModuleFileName(nullptr) fuer den Pfad der laufenden Exe. Kein
// Steam-Manifest, keine Pfadsuche, funktioniert auch ohne Steam.
//
// Rueckgabe false bedeutet ausdruecklich "nicht feststellbar ODER aelter" --
// beides fuehrt zum bisherigen Verhalten, nie zu einem Experiment.
bool re4_exe_is_1590_or_newer() {
    wchar_t path[MAX_PATH]{};

    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return false;
    }

    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path, &ignored);

    if (size == 0) {
        return false;
    }

    std::vector<uint8_t> buf(size);

    if (GetFileVersionInfoW(path, 0, size, buf.data()) == 0) {
        return false;
    }

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;

    if (VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) == 0 || ffi == nullptr) {
        return false;
    }

    const uint32_t major = HIWORD(ffi->dwFileVersionMS);
    const uint32_t minor = LOWORD(ffi->dwFileVersionMS);
    const uint32_t build = HIWORD(ffi->dwFileVersionLS);

    spdlog::info("[RE4] Exe-Version: {}.{}.{}", major, minor, build);

    // Vergleich als eine Zahl, damit 1.10.x nicht faelschlich kleiner als 1.5.x wirkt.
    const uint64_t have = ((uint64_t)major << 32) | ((uint64_t)minor << 16) | build;
    const uint64_t need = ((uint64_t)1 << 32) | ((uint64_t)5 << 16) | 9;

    return have >= need;
}
} // namespace
#endif

std::shared_ptr<REFrameworkConfig>& REFrameworkConfig::get() {
     static std::shared_ptr<REFrameworkConfig> instance{std::make_shared<REFrameworkConfig>()};
     return instance;
}

std::optional<std::string> REFrameworkConfig::on_initialize() {
    return Mod::on_initialize();
}

void REFrameworkConfig::on_draw_ui() {
    if (!ImGui::CollapsingHeader("Configuration")) {
        return;
    }

    ImGui::TreePush("Configuration");

    m_menu_key->draw("Menu Key");
    m_show_cursor_key->draw("Show Cursor Key");
    m_remember_menu_state->draw("Remember Menu Open/Closed State");
    m_always_show_cursor->draw("Draw Cursor With Menu Open");

    if (m_font_size->draw("Font Size")) {
        g_framework->set_font_size(m_font_size->value());
    }

    ImGui::TreePop();
}

void REFrameworkConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }
}

void REFrameworkConfig::on_config_load(const utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_load(cfg);
    }

    if (m_remember_menu_state->value()) {
        g_framework->set_draw_ui(m_menu_open->value(), false);
    }
    
    g_framework->set_font_size(m_font_size->value());

    // [RE4 1.5.9.0 TEXTURLAYOUT] Das Flag MUSS stehen, bevor die erste
    // Engine-Textur gelesen wird -- das passiert erst beim Aufbau der
    // VR-Augentexturen, also lange nach dem Laden der Konfiguration.
    //
    // Standard ist -1 = selbst erkennen. Das ist kein Komfort, sondern noetig:
    // ein Neuinstallierer hat noch GAR KEINE Konfigurationsdatei, und mit dem
    // falschen Layout kommt das Spiel nie so weit, eine anzulegen -- er kaeme
    // also an keinen Schalter heran. 0/1 uebersteuern die Erkennung fuer Tests.
#ifdef RE4
    const auto layout_mode = m_re4_texture_layout->value();
    const bool use_new_layout = (layout_mode < 0) ? re4_exe_is_1590_or_newer()
                                                  : (layout_mode != 0);

    sdk::renderer::g_re4_new_texture_layout = use_new_layout;

    spdlog::info("[RE4] Texturlayout: {} (Schalter {})",
                 use_new_layout ? "ab 1.5.9.0" : "bis 1.1.1.0",
                 layout_mode < 0 ? "automatisch" : (layout_mode != 0 ? "1 erzwungen" : "0 erzwungen"));
#endif
}

void REFrameworkConfig::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
