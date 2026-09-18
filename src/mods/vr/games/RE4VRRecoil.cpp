// ============================================================================
// RE4VRRecoil -- 1:1-Portierung von re4_vr_recoil.lua
// Spezifikation: I:\LUATRANS\PORT_RECOIL_SPEC.md (Fassung 2)
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <utility/String.hpp>

#include "../../../mods/ScriptRunner.hpp"
#include "../../../REFramework.hpp"   // g_framework->draw_menu_heading
#include "../../../HookManager.hpp"
#include "../../VR.hpp"
#include "RE4VR.hpp"
#include "RE4VRRecoil.hpp"

namespace {

// Lua: os.clock(). Lua ist in dieselbe DLL gelinkt -> identische Epoche.
double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// Lua: math.random() -> [0,1). Pro Aufruf neu gezogen (dreimal je Schuss).
double lua_random() {
    static std::mt19937_64 gen{std::random_device{}()};
    static std::uniform_real_distribution<double> dist{0.0, 1.0};
    return dist(gen);
}

re4vr::LuaRef rc_lua_state() {
    // [ABSTURZ 04.09.2026] Sperre des ScriptRunners halten -- s. re4vr::LuaRef.
    return re4vr::LuaRef{};
}

constexpr const char* CFG_PATH = "re4_vr/re4_vr_recoil.json";
constexpr const char* SEED_PATH = "re4_vr/re4_vr_firstperson.json";
constexpr const char* HAPTIC_CFG_PATH = "re4_vr/re4_vr_haptic.json";

constexpr const char* KS_MODULE = "re4vr/re4_vr_killswitch";

} // namespace

// ============================================================================
// Waffentabellen
// ============================================================================

// 42 Eintraege, 1:1 aus dem alten firstperson uebernommen.
// 6107 und 6108 stehen auf 0.0 -- dort wird der Recoil-Block per
// weapon_multiplier <= 0.0 uebersprungen. NICHT auf 1.0 "korrigieren".
const std::unordered_map<int32_t, double> RE4VRRecoil::WEAPON_RECOIL_MULTIPLIERS = {
    {4000, 1.2}, {4001, 1.2}, {4002, 1.2}, {4003, 1.2}, {4004, 1.2}, {4005, 1.4},
    {4100, 1.8}, {4101, 1.8}, {4102, 1.8},
    {4200, 1.5}, {4201, 1.5}, {4202, 1.5},
    {4400, 1.8}, {4401, 1.8}, {4402, 1.8},
    {4500, 2.0}, {4501, 2.0}, {4502, 2.0},
    {4600, 1.8},
    {4900, 2.5}, {4901, 2.5}, {4902, 2.5},
    {6000, 1.2}, {6001, 1.2},
    {6100, 1.2}, {6101, 1.2}, {6102, 1.2}, {6103, 1.2}, {6104, 1.2}, {6105, 1.8},
    {6106, 1.2}, {6107, 0.0}, {6108, 0.0}, {6111, 1.2}, {6112, 1.2}, {6113, 1.2}, {6114, 1.8},
    {6300, 1.2}, {6301, 1.2}, {6302, 1.2}, {6304, 1.2}, {6305, 1.2},
};

const std::unordered_map<int32_t, bool> RE4VRRecoil::WEAPON_AUTO_FLAGS = {
    {4005, true}, {4200, true}, {4201, true}, {4202, true}, {4402, true},
};

// ============================================================================
// Aufbau
// ============================================================================

std::shared_ptr<RE4VRRecoil>& RE4VRRecoil::get() {
    static auto inst = std::make_shared<RE4VRRecoil>();
    return inst;
}

std::optional<std::string> RE4VRRecoil::on_initialize() {
    load_config();
    load_haptic_config();

    // ---- Die drei Hooks (Lua Z.366, 376, 386) ----
    // Die _G-Indirektion und der Installationsriegel des Originals entfallen:
    // das hier laeuft einmal. Jede Methode wird EINZELN geprueft -- das
    // Fehlschlagen einer darf die anderen beiden nicht verhindern.
    if (auto* pcc = sdk::find_type_definition(game_namespace("PlayerCameraController"))) {
        const auto add = [](sdk::REMethodDefinition* m, bool (RE4VRRecoil::*fn)()) {
            if (m == nullptr) {
                return;
            }

            g_hookman.add(
                m,
                [fn](std::vector<uintptr_t>&, std::vector<sdk::RETypeDefinition*>&, uintptr_t) {
                    // [SKIP_ORIGINAL] Ein Pre-Hook, der SKIP_ORIGINAL liefert, muss
                    // auch den Rueckgabewert setzen -- sonst steht Registermuell.
                    // Alle drei Methoden hier sind void, es gibt nichts zu setzen.
                    const bool skip = ((*RE4VRRecoil::get()).*fn)();

                    return skip ? HookManager::PreHookResult::SKIP_ORIGINAL
                                : HookManager::PreHookResult::CALL_ORIGINAL;
                },
                [](uintptr_t&, sdk::RETypeDefinition*, uintptr_t) {});
        };

        add(pcc->get_method("requestRecoil(chainsaw.CameraRecoilParam)"),
            &RE4VRRecoil::hook_pre_request_recoil);
        add(pcc->get_method("updateRecoil"), &RE4VRRecoil::hook_pre_update_recoil);
        add(pcc->get_method("updateHandShake"), &RE4VRRecoil::hook_pre_update_handshake);
    }

    return Mod::on_initialize();
}

void RE4VRRecoil::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRRecoil", "on_lua_state_created");
    // Lua Z.230: _G.vr_recoil = _G.vr_recoil or { ... }
    // Eine bestehende Tabelle UEBERLEBT den Reset -- ihre Felder bleiben stehen
    // und werden erst im naechsten LateUpdateBehavior ueberschrieben.
    sol::object existing = lua["vr_recoil"];

    if (!existing.valid() || existing.get_type() != sol::type::table) {
        sol::table t = lua.create_table();
        t["position"] = glm::vec3{0.0f, 0.0f, 0.0f};
        t["rotation"] = glm::identity<glm::quat>();
        t["active"] = false;
        lua["vr_recoil"] = t;
    }

    // Die _G-Indirektion des Originals: ohne Leser, aber 1:1 erhalten.
    lua["__re4_vr_recoil_hooks_installed"] = true;
    lua["__recoil_tree_open"] = false;
    lua["__re4_vr_recoil_on_request"] = [](sol::object) {};
    lua["__re4_vr_recoil_on_update"] = [](sol::object) {};
    lua["__re4_vr_recoil_on_handshake"] = [](sol::object) {};

    lua["__re4_recoil_draw_public"] = []() {
        RE4VRRecoil::get()->draw_public_recoil_level();
    };

    m_public_ui_registered = false;
    m_dispatcher_present = false;
}

void RE4VRRecoil::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRRecoil", "on_lua_state_destroyed");
    // Die Datei hat KEIN on_script_reset -- in Lua entsteht der Zustand beim
    // Neu-Ausfuehren frisch. Genau das hier nachbilden.
    m_state = State{};
    m_pending.clear();
    m_have_last_seq = false;
    m_last_seq = 0.0;
    m_weapon_id_catalog_sorted.clear();
    m_catalog_built = false;
    m_public_ui_registered = false;
    m_dispatcher_present = false;

    m_weapon_intensity_overrides.clear();
    m_weapon_support_overrides.clear();
    load_config();
    load_haptic_config();

    // vr_recoil wird BEWUSST nicht angefasst, s. Spec 9.
}

// ============================================================================
// Konfiguration
// ============================================================================

bool RE4VRRecoil::apply_config_table(const nlohmann::json& d) {
    if (!d.is_object()) {
        return false;
    }

    bool found = false;

    // CONFIG_KEYS -- uebernommen wird, was NICHT nil ist.
    if (d.contains("enable_recoil") && !d["enable_recoil"].is_null()) {
        if (d["enable_recoil"].is_boolean()) {
            m_cfg.enable_recoil = d["enable_recoil"].get<bool>();
        }

        found = true;
    }

    if (d.contains("recoil_intensity_multiplier") && !d["recoil_intensity_multiplier"].is_null()) {
        if (d["recoil_intensity_multiplier"].is_number()) {
            m_cfg.recoil_intensity_multiplier = d["recoil_intensity_multiplier"].get<double>();
        }

        found = true;
    }

    if (d.contains("weapon_intensity_overrides") && d["weapon_intensity_overrides"].is_object()) {
        for (auto it = d["weapon_intensity_overrides"].begin();
             it != d["weapon_intensity_overrides"].end(); ++it) {
            if (it.value().is_number()) {
                m_weapon_intensity_overrides[it.key()] = it.value().get<double>();
            }
        }

        found = true;
    }

    if (d.contains("weapon_support_overrides") && d["weapon_support_overrides"].is_object()) {
        for (auto it = d["weapon_support_overrides"].begin();
             it != d["weapon_support_overrides"].end(); ++it) {
            if (it.value().is_number()) {
                m_weapon_support_overrides[it.key()] = it.value().get<double>();
            }
        }

        found = true;
    }

    return found;
}

void RE4VRRecoil::load_config() {
    m_cfg = Cfg{};

    // [M1] re4vr::json_load liefert bei FEHLENDER Datei ein leeres
    // json::object() -- `is_object()` allein wuerde also auch dann greifen und
    // den Saat-Pfad unerreichbar machen. Lua bekommt an der Stelle nil
    // (type(d) ~= "table") und geht in die Saat. Deshalb die Datei selbst
    // pruefen.
    bool have_own = false;

    try {
        std::error_code ec{};
        have_own = std::filesystem::exists(re4vr::datadir() / CFG_PATH, ec) && !ec;
    } catch (...) {
        have_own = false;
    }

    if (have_own) {
        try {
            const auto d = re4vr::json_load(CFG_PATH);

            if (d.is_object()) {
                apply_config_table(d);
                return;
            }
        } catch (...) {
        }
    }

    // Erster Lauf: Tuning aus der alten firstperson-JSON uebernehmen -- aber
    // AUSSCHLIESSLICH die vier CONFIG_KEYS-Posten, nicht die alten
    // Tuning-Schluessel.
    try {
        const auto seed = re4vr::json_load(SEED_PATH);

        if (apply_config_table(seed)) {
            save_config();
        }
    } catch (...) {
    }
}

void RE4VRRecoil::save_config() {
    nlohmann::json j;
    j["enable_recoil"] = m_cfg.enable_recoil;
    j["recoil_intensity_multiplier"] = m_cfg.recoil_intensity_multiplier;

    nlohmann::json wi = nlohmann::json::object();

    for (const auto& [k, v] : m_weapon_intensity_overrides) {
        wi[k] = v;
    }

    j["weapon_intensity_overrides"] = wi;

    nlohmann::json ws = nlohmann::json::object();

    for (const auto& [k, v] : m_weapon_support_overrides) {
        ws[k] = v;
    }

    j["weapon_support_overrides"] = ws;
    re4vr::json_save(CFG_PATH, j);
}

void RE4VRRecoil::load_haptic_config() {
    m_haptic = HapticCfg{};

    try {
        const auto d = re4vr::json_load(HAPTIC_CFG_PATH);

        if (!d.is_object()) {
            return;
        }

        const auto b = [&](const char* k, bool& dst) {
            if (d.contains(k) && !d[k].is_null() && d[k].is_boolean()) {
                dst = d[k].get<bool>();
            }
        };
        const auto f = [&](const char* k, double& dst) {
            if (d.contains(k) && !d[k].is_null() && d[k].is_number()) {
                dst = d[k].get<double>();
            }
        };

        b("enabled", m_haptic.enabled);
        f("amp_solo", m_haptic.amp_solo);
        f("amp_right_sup", m_haptic.amp_right_sup);
        f("amp_left_sup", m_haptic.amp_left_sup);
        f("dur", m_haptic.dur);
        f("freq", m_haptic.freq);
        f("delay", m_haptic.delay);
    } catch (...) {
        m_haptic = HapticCfg{};
    }
}

void RE4VRRecoil::save_haptic_config() {
    nlohmann::json j;
    j["enabled"] = m_haptic.enabled;
    j["amp_solo"] = m_haptic.amp_solo;
    j["amp_right_sup"] = m_haptic.amp_right_sup;
    j["amp_left_sup"] = m_haptic.amp_left_sup;
    j["dur"] = m_haptic.dur;
    j["freq"] = m_haptic.freq;
    j["delay"] = m_haptic.delay;
    re4vr::json_save(HAPTIC_CFG_PATH, j);
}

// ============================================================================
// Waffen-Ermittlung
// ============================================================================

void RE4VRRecoil::build_weapon_id_catalog_sorted() {
    if (m_catalog_built) {
        return;
    }

    m_weapon_id_catalog_sorted.clear();
    m_weapon_id_catalog_sorted.reserve(WEAPON_RECOIL_MULTIPLIERS.size());

    for (const auto& [wid, mult] : WEAPON_RECOIL_MULTIPLIERS) {
        m_weapon_id_catalog_sorted.push_back(wid);
    }

    std::sort(m_weapon_id_catalog_sorted.begin(), m_weapon_id_catalog_sorted.end());
    m_catalog_built = true;
}

std::optional<int32_t> RE4VRRecoil::get_current_weapon_id() {
    // [FRAME-CACHE] Einmal pro Frame aufloesen statt bei jedem Aufruf. Der
    // Cache selbst wertet __re4_fc_off aus, nicht diese Datei -- liefert on()
    // false, geht es hier den langen Weg.
    if (auto lua = rc_lua_state()) {
        sol::object fc = (*lua)["__re4_frame_cache"];

        if (fc.valid() && fc.get_type() == sol::type::table) {
            try {
                sol::table t = fc.as<sol::table>();
                sol::protected_function on = t["on"];
                sol::protected_function wid_fn = t["equip_wid"];

                if (on.valid() && wid_fn.valid()) {
                    auto r = on();

                    if (r.valid() && static_cast<sol::object>(r).is<bool>()
                        && static_cast<sol::object>(r).as<bool>()) {
                        auto w = wid_fn();

                        if (w.valid()) {
                            sol::object o = w;

                            if (o.get_type() == sol::type::number) {
                                return static_cast<int32_t>(o.as<double>());
                            }
                        }

                        return std::nullopt;   // Cache sagt: keine Waffe
                    }
                }
            } catch (...) {
            }
        }
    }

    // Langer Weg: CharacterManager -> PlayerContext -> HeadUpdater.
    // Jeder Schritt einzeln geschuetzt; ein Fehler fuehrt zu nil und damit zu
    // Multiplikator 1.0, NICHT 0.0.
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return std::nullopt;
    }

    auto* ctx = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");

    if (ctx == nullptr) {
        return std::nullopt;
    }

    auto* hu = re4vr::call_safe<::REManagedObject*>(ctx, "get_HeadUpdater");

    if (hu == nullptr) {
        return std::nullopt;
    }

    int32_t wid = 0;

    if (!re4vr::try_call<int32_t>(hu, "get_EquipWeaponID", wid)) {
        return std::nullopt;
    }

    return wid;
}

std::string RE4VRRecoil::weapon_key_from_id(int32_t wid) {
    // string.format("wp%04d", wid) -- vierstellig mit fuehrenden Nullen.
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "wp%04d", wid);
    return std::string{buf};
}

double RE4VRRecoil::get_effective_weapon_recoil_multiplier(double& base, double& per,
                                                           double& global, std::string& key) {
    const auto wid = get_current_weapon_id();

    base = 1.0;
    key.clear();

    if (wid.has_value()) {
        const auto it = WEAPON_RECOIL_MULTIPLIERS.find(*wid);
        base = (it != WEAPON_RECOIL_MULTIPLIERS.end()) ? it->second : 1.0;
        key = weapon_key_from_id(*wid);
    }

    per = 1.0;

    if (!key.empty()) {
        const auto it = m_weapon_intensity_overrides.find(key);

        if (it != m_weapon_intensity_overrides.end()) {
            per = it->second;
        }
    }

    global = m_cfg.recoil_intensity_multiplier;

    return base * global * per;
}

// ============================================================================
// Die beiden Tore
// ============================================================================

bool RE4VRRecoil::fp_active() const {
    // Lua Z.184-202. Port von should_fp_be_active, OHNE Stage-Overrides/Scope.
    //
    // [WICHTIG] Punkt 3 unten ist HEUTE TOT: re4vr/re4_vr_killswitch.lua
    // exportiert in seiner Rueckgabetabelle nur is_active; die drei
    // FP-Funktionen existieren im gesamten Live-Set nicht. Ist-Verhalten
    // damit: fp_active() == not killswitch_active. Die Kaskade wird trotzdem
    // 1:1 nachgebaut -- ein Killswitch, der sie eines Tages haette, soll
    // dasselbe tun wie das Original.
    if (!re4vr::lua_module_call_bool(KS_MODULE, "is_active", false)) {
        return true;   // kein Modul, Fehler, oder Killswitch inaktiv
    }

    if (re4vr::lua_module_call_bool(KS_MODULE, "is_reload_active", false)) {
        return true;
    }

    if (re4vr::lua_module_call_bool(KS_MODULE, "is_first_person_action_active", false)) {
        return true;
    }

    if (re4vr::lua_module_call_bool(KS_MODULE, "is_first_person_animation_active", false)) {
        return true;
    }

    return false;
}

bool RE4VRRecoil::hmd_active() const {
    return VR::get()->is_hmd_active();
}

// ============================================================================
// requestRecoil (Lua Z.239-337) -- der Kern
// ============================================================================

bool RE4VRRecoil::hook_pre_request_recoil() {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return false;
    }
    // Der EINZIGE Pfad, der das native requestRecoil durchlaufen laesst:
    // kein HMD UND fp_active() == false. Alles andere endet auf SKIP_ORIGINAL.
    if (hmd_active()) {
        if (!fp_active()) {
            return true;   // HMD an, Gate zu -> nur skippen
        }
        // sonst weiter in den Rumpf (Luas goto)
    } else if (!fp_active()) {
        return false;      // ORIGINAL laufen lassen
    }

    double weapon_base_mult = 1.0;
    double per = 1.0;
    double global = 1.0;
    std::string key;
    const double weapon_multiplier =
        get_effective_weapon_recoil_multiplier(weapon_base_mult, per, global, key);

    if (m_cfg.enable_recoil) {
        if (weapon_multiplier <= 0.0) {
            return true;   // die 0.0-Waffen (6107/6108)
        }

        // [M2] Lua legt genau diesen Rumpf in ein pcall (Z.261-333). Der
        // Pre-Hook laeuft aus dem asmjit-Trampolin -- eine durchgereichte
        // C++-Exception haette dort keinen definierten Unwind-Weg.
        // Fehlschlag = kein Kick, aber trotzdem SKIP_ORIGINAL.
        try {

        const double random_factor = 1.0 + (lua_random() - 0.5) * RECOIL_RANDOMNESS;
        const double mult_exp = (std::max)(0.1, (std::min)(1.0, RECOIL_MULT_EXPONENT));
        double total_mult = std::pow(weapon_multiplier, mult_exp) * random_factor;

        // ---- Stuetzhand ----
        bool support_hand_active = false;
        {
            const int docked = re4vr::lua_get_tribool("__vr_support_hand_docked");

            if (docked >= 0) {
                support_hand_active = (docked == 1);
            } else {
                // elseif-Fallback: nur wenn __vr_support_hand_docked nil ist.
                auto lua = rc_lua_state();

                if (lua != nullptr) {
                    sol::object bf = (*lua)["__vr_support_blend_factor"];

                    if (bf.valid() && bf.get_type() == sol::type::number) {
                        support_hand_active = bf.as<double>() > 0.5;
                    }
                }
            }
        }

        if (support_hand_active) {
            // [SUPPORT_PER_WEAPON] Der Schluessel wird HIER NEU geholt, nicht
            // wiederverwendet -- 1:1 wie im Original.
            const auto swid = get_current_weapon_id();
            double sm = RECOIL_SUPPORTED_IMPULSE_MULT;

            if (swid.has_value()) {
                const auto skey = weapon_key_from_id(*swid);
                const auto it = m_weapon_support_overrides.find(skey);

                if (it != m_weapon_support_overrides.end()) {
                    sm = it->second;
                }
            }

            if (sm > 0.0) {
                total_mult *= sm;
            }
        } else {
            const bool heavy = weapon_base_mult >= 2.0;
            const double um = heavy ? RECOIL_UNSUPPORTED_HEAVY_MULT : RECOIL_UNSUPPORTED_LIGHT_MULT;

            if (um > 0.0) {
                total_mult *= um;
            }
        }

        // ---- Vollautomat: die Waffen-ID wird ERNEUT geholt ----
        {
            const auto wid_auto = get_current_weapon_id();

            if (wid_auto.has_value()
                && WEAPON_AUTO_FLAGS.find(*wid_auto) != WEAPON_AUTO_FLAGS.end()) {
                total_mult *= (std::max)(0.05, RECOIL_AUTO_SCALE);
            }
        }

        // ---- Peaks ----
        const double pos_peak = RECOIL_POSITION_INTENSITY * total_mult;
        const double new_pos_y = pos_peak * 0.6;
        const double new_pos_z = -pos_peak;

        const double pitch_peak = RECOIL_ROTATION_INTENSITY * total_mult
                                  + (lua_random() - 0.5) * RECOIL_VERTICAL_SPREAD * total_mult;
        const double yaw_peak =
            (lua_random() - 0.5) * 2.0 * RECOIL_HORIZONTAL_SPREAD * total_mult;

        m_state.recoil_attack_pos_y += new_pos_y;
        m_state.recoil_attack_pos_z += new_pos_z;
        m_state.recoil_attack_pitch += pitch_peak;
        m_state.recoil_attack_yaw += yaw_peak;

        // ---- Deckel ----
        // [CAP-FIX] Der Deckel wurde frueher aus den BASIS-Intensitaeten
        // gerechnet, also OHNE total_mult -- ein absoluter Anschlag. Sobald
        // total_mult >= stack_cap war, landete JEDE Einstellung auf demselben
        // gekappten Wert und der Per-Waffe-Slider tat nichts. Gemeint ist ein
        // DAUERFEUER-Deckel: aufaddierte Kicks max. stack_cap x dem Peak
        // DIESES Schusses. Die Multiplikation mit total_mult ist Pflicht.
        if (RECOIL_STACK_CAP > 0.0) {
            const double cap_pos = RECOIL_POSITION_INTENSITY * total_mult * RECOIL_STACK_CAP;
            const double cap_rot = RECOIL_ROTATION_INTENSITY * total_mult * RECOIL_STACK_CAP;
            const double cap_yaw = RECOIL_HORIZONTAL_SPREAD * total_mult * RECOIL_STACK_CAP;

            m_state.recoil_attack_pos_y = (std::min)(m_state.recoil_attack_pos_y, cap_pos * 0.6);
            m_state.recoil_attack_pos_z = (std::max)(m_state.recoil_attack_pos_z, -cap_pos);
            m_state.recoil_attack_pitch = (std::min)(m_state.recoil_attack_pitch, cap_rot);

            if (m_state.recoil_attack_yaw > cap_yaw) {
                m_state.recoil_attack_yaw = cap_yaw;
            }

            if (m_state.recoil_attack_yaw < -cap_yaw) {
                m_state.recoil_attack_yaw = -cap_yaw;
            }
        }

        m_state.recoil_attack_t = 0.0;
        m_state.recoil_attack_active = true;
        m_state.recoil_active = true;

        // os.clock() wird hier nur EINMAL gelesen und fuer beide Zeitmarken
        // benutzt.
        const double now_shot = now_clock();

        if (!m_state.has_last_t) {
            m_state.recoil_last_t = now_shot;
            m_state.has_last_t = true;
        }

        m_state.recoil_last_shot_t = now_shot;
        m_state.has_last_shot_t = true;

        } catch (...) {
            // wie Luas pcall: schlucken, der Kick faellt aus
        }
    }

    return true;   // SKIP_ORIGINAL
}

bool RE4VRRecoil::hook_pre_update_recoil() {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return false;
    }
    if (hmd_active()) {
        return true;
    }

    if (!fp_active()) {
        return false;
    }

    return true;
}

bool RE4VRRecoil::hook_pre_update_handshake() {
    // [SCRIPTGATE] Riegel zu -> Original unveraendert laufen lassen,
    // als waere dieser Hook nie registriert worden.
    if (re4vr::mods_gated()) {
        return false;
    }
    if (hmd_active()) {
        return true;
    }

    if (!fp_active()) {
        return false;
    }

    return true;
}

// ============================================================================
// Feder-Simulation und Export (Lua Z.402-511)
// ============================================================================

void RE4VRRecoil::update_spring_and_export() {
    if (m_state.recoil_active || m_state.recoil_attack_active) {
        const double now = now_clock();
        double dt = 0.0;

        if (m_state.has_last_t) {
            dt = (std::min)(now - m_state.recoil_last_t, 0.05);
        }

        m_state.recoil_last_t = now;
        m_state.has_last_t = true;

        // ---- Attack-Phase ----
        if (m_state.recoil_attack_active && dt > 0.0) {
            const double T = (std::max)(RECOIL_ATTACK_DURATION, 0.001);
            m_state.recoil_attack_t += dt;

            if (m_state.recoil_attack_t >= T) {
                m_state.spring_pos_y = m_state.recoil_attack_pos_y;
                m_state.spring_pos_z = m_state.recoil_attack_pos_z;
                m_state.spring_pitch = m_state.recoil_attack_pitch;
                m_state.spring_yaw = m_state.recoil_attack_yaw;
                m_state.spring_vel_y = 0.0;
                m_state.spring_vel_z = 0.0;
                m_state.spring_vel_pitch = 0.0;
                m_state.spring_vel_yaw = 0.0;
                m_state.recoil_attack_pos_y = 0.0;
                m_state.recoil_attack_pos_z = 0.0;
                m_state.recoil_attack_pitch = 0.0;
                m_state.recoil_attack_yaw = 0.0;
                m_state.recoil_attack_t = 0.0;
                m_state.recoil_attack_active = false;
            } else {
                const double s =
                    std::sin((m_state.recoil_attack_t / T) * (3.14159265358979323846 * 0.5));
                m_state.spring_pos_y = m_state.recoil_attack_pos_y * s;
                m_state.spring_pos_z = m_state.recoil_attack_pos_z * s;
                m_state.spring_pitch = m_state.recoil_attack_pitch * s;
                m_state.spring_yaw = m_state.recoil_attack_yaw * s;
                m_state.spring_vel_y = 0.0;
                m_state.spring_vel_z = 0.0;
                m_state.spring_vel_pitch = 0.0;
                m_state.spring_vel_yaw = 0.0;
            }
        }

        // ---- Feder-Phase ----
        if (!m_state.recoil_attack_active && dt > 0.0) {
            const double k = RECOIL_SPRING_STIFFNESS;
            double c = RECOIL_SPRING_DAMPING;

            if (m_state.has_last_shot_t) {
                const double since_last = now - m_state.recoil_last_shot_t;
                const double win = (std::max)(0.01, RECOIL_SUSTAINED_WINDOW);

                if (since_last < win) {
                    const double t_blend = 1.0 - (since_last / win);
                    c = c + (RECOIL_SUSTAINED_DAMPING - c) * t_blend;
                }
            }

            const int steps = (std::max)(1, static_cast<int>(std::floor(dt / 0.008)));
            const double sub = dt / steps;

            for (int i = 0; i < steps; ++i) {
                const double ay = -k * m_state.spring_pos_y - c * m_state.spring_vel_y;
                m_state.spring_vel_y += ay * sub;
                m_state.spring_pos_y += m_state.spring_vel_y * sub;

                const double az = -k * m_state.spring_pos_z - c * m_state.spring_vel_z;
                m_state.spring_vel_z += az * sub;
                m_state.spring_pos_z += m_state.spring_vel_z * sub;

                const double ap = -k * m_state.spring_pitch - c * m_state.spring_vel_pitch;
                m_state.spring_vel_pitch += ap * sub;
                m_state.spring_pitch += m_state.spring_vel_pitch * sub;

                const double aw = -k * m_state.spring_yaw - c * m_state.spring_vel_yaw;
                m_state.spring_vel_yaw += aw * sub;
                m_state.spring_yaw += m_state.spring_vel_yaw * sub;
            }

            const double pos_mag =
                std::fabs(m_state.spring_pos_y) + std::fabs(m_state.spring_pos_z);
            const double rot_mag =
                std::fabs(m_state.spring_pitch) + std::fabs(m_state.spring_yaw);
            const double vel_mag = std::fabs(m_state.spring_vel_y)
                                   + std::fabs(m_state.spring_vel_z)
                                   + std::fabs(m_state.spring_vel_pitch)
                                   + std::fabs(m_state.spring_vel_yaw);

            if (pos_mag < 0.00005 && rot_mag < 0.0002 && vel_mag < 0.001) {
                m_state.spring_pos_y = 0.0;
                m_state.spring_pos_z = 0.0;
                m_state.spring_vel_y = 0.0;
                m_state.spring_vel_z = 0.0;
                m_state.spring_pitch = 0.0;
                m_state.spring_yaw = 0.0;
                m_state.spring_vel_pitch = 0.0;
                m_state.spring_vel_yaw = 0.0;
                m_state.recoil_active = false;
                // recoil_last_shot_t bleibt BEWUSST stehen.
                m_state.has_last_t = false;
            }
        }
    }

    // ---- Export ----
    auto lua = rc_lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object o = (*lua)["vr_recoil"];

    if (!o.valid() || o.get_type() != sol::type::table) {
        return;
    }

    sol::table t = o.as<sol::table>();

    if (m_cfg.enable_recoil && (m_state.recoil_active || m_state.recoil_attack_active)) {
        t["position"] = glm::vec3{0.0f, static_cast<float>(m_state.spring_pos_y),
                                  static_cast<float>(m_state.spring_pos_z)};

        // [RECOIL Y-DOMINANT] Pitch (Muendung kippt hoch) ist der Haupt-Recoil
        // -> verstaerkt. Das * 0.5 ist die Quaternion-Half-Angle-Konvention
        // (NICHT antasten); PITCH_KICK_GAIN multipliziert den Winkel VOR der
        // Half-Angle-Bildung.
        constexpr double PITCH_KICK_GAIN = 2.0;
        const double ph = -(m_state.spring_pitch * PITCH_KICK_GAIN) * 0.5;
        const double yw = m_state.spring_yaw * 0.5;

        // Quaternion.new(w, x, y, z) == glm::quat{w, x, y, z}
        const glm::quat pitch_q{static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)),
                                0.0f, 0.0f};
        const glm::quat yaw_q{static_cast<float>(std::cos(yw)), 0.0f,
                              static_cast<float>(std::sin(yw)), 0.0f};

        t["rotation"] = glm::normalize(pitch_q * yaw_q);
        t["active"] = true;
    } else {
        t["position"] = glm::vec3{0.0f, 0.0f, 0.0f};
        t["rotation"] = glm::identity<glm::quat>();
        t["active"] = false;
    }
}

void RE4VRRecoil::on_application_entry(void* entry, const char* name, size_t hash) {
    re4vr::trace("RE4VRRecoil", "on_application_entry");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        // Laeuft UNKONDITIONIERT jeden Frame -- ohne HMD-, FP- oder
        // Killswitch-Pruefung, genau wie im Original.
        update_spring_and_export();
    }
}

// ============================================================================
// Schuss-Haptik (Lua Z.677-778)
// ============================================================================

void RE4VRRecoil::haptic_pulse(uint64_t handle, double amp) {
    // [K1] KEINE handle-Nullpruefung. Unter OpenXR ist der linke Joystick
    // buchstaeblich 0 (VR.cpp:1429 -> VRRuntime::Hand::LEFT == 0), und in Lua
    // ist 0 wahrheitswertig -- dort feuert der Puls. Eine `handle == 0`-Sperre
    // haette die Stuetzhand-Haptik unter OpenXR lautlos komplett abgeschaltet.
    // Lua prueft nur auf nil, und nil gibt es hier nicht.
    if (amp <= 0.0) {
        return;
    }

    try {
        VR::get()->trigger_haptic_vibration(0.0f, static_cast<float>(m_haptic.dur),
                                            static_cast<float>(m_haptic.freq),
                                            static_cast<float>(amp), handle);
    } catch (...) {
    }
}

void RE4VRRecoil::haptic_fire(bool support) {
    // Rechts IMMER (solo 100% / mit Support 70%), links nur bei angedockter
    // Support-Hand (30%).
    haptic_pulse(VR::get()->get_right_joystick(),
                 support ? m_haptic.amp_right_sup : m_haptic.amp_solo);

    if (support) {
        haptic_pulse(VR::get()->get_left_joystick(), m_haptic.amp_left_sup);
    }
}

void RE4VRRecoil::haptic_tick() {
    // [KS_GLOBAL] JEDER Killswitch: Recoil und Haptik haben in KS1..KS5 nichts
    // verloren. Gelesen wird die GLOBAL __re4_ks_active -- bewusst ein anderer
    // Weg als fp_active(), das die Modultabelle direkt fragt.
    if (re4vr::lua_get_bool("__re4_ks_active", false)) {
        return;
    }

    if (!m_haptic.enabled) {
        return;
    }

    if (!VR::get()->is_hmd_active()) {
        return;
    }

    const double now = now_clock();

    // [DELAY] faellige geplante Pulse abfeuern
    for (size_t i = 0; i < m_pending.size();) {
        if (now >= m_pending[i].at) {
            const bool sup = m_pending[i].support;
            m_pending.erase(m_pending.begin() + static_cast<long>(i));
            haptic_fire(sup);
        } else {
            ++i;
        }
    }

    // Schuss-Flanke erkennen
    const double seq = re4vr::lua_get_number("__vr_shot_seq", 0.0);

    if (!m_have_last_seq) {
        m_have_last_seq = true;
        m_last_seq = seq;
        return;   // erster Frame: nur Basislinie, NICHT feuern
    }

    if (seq <= m_last_seq) {
        return;
    }

    m_last_seq = seq;

    const bool support = re4vr::lua_get_bool("__vr_support_hand_docked", false);

    if (m_haptic.delay <= 0.0) {
        haptic_fire(support);
    } else {
        m_pending.push_back(Pending{now + m_haptic.delay, support});
    }
}

void RE4VRRecoil::on_frame() {
    re4vr::trace("RE4VRRecoil", "on_frame");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    ensure_public_ui_registered();
    haptic_tick();
}

// ============================================================================
// UI
// ============================================================================

void RE4VRRecoil::ensure_public_ui_registered() {
    // [TRAEGE ANMELDUNG] on_lua_state_created laeuft VOR dem Laden der
    // Autorun-Scripte; ##re4_vr_menu.lua leert __re4_ui_entries bei seinem
    // Start. Deshalb pro Frame nachsehen.
    // Bewusste Abweichung: das Original meldet einmalig beim Laden an und
    // faellt sonst auf ein eigenes on_draw_ui zurueck.
    if (m_public_ui_registered) {
        return;
    }

    auto lua = rc_lua_state();

    if (lua == nullptr) {
        return;
    }

    sol::object add = (*lua)["__re4_ui_add"];

    if (!add.valid() || add.get_type() != sol::type::function) {
        m_dispatcher_present = false;
        return;
    }

    sol::object entries = (*lua)["__re4_ui_entries"];

    if (!entries.valid() || entries.get_type() != sol::type::table) {
        // [M3b] Ohne Eintragsliste ist der Dispatcher fuer uns unbrauchbar --
        // dann muss der eigene Fallback zeichnen duerfen.
        m_dispatcher_present = false;
        return;
    }

    m_dispatcher_present = true;

    if (entries.as<sol::table>()["recoil_level"].valid()) {
        m_public_ui_registered = true;
        return;
    }

    try {
        auto fn = add.as<sol::protected_function>();
        auto r = fn(50, "recoil_level", (*lua)["__re4_recoil_draw_public"]);

        // [M3a] protected_function WIRFT nicht -- das Ergebnis muss geprueft
        // werden, sonst gilt die Anmeldung als geglueckt und der Block ist
        // bis zum naechsten Reset nirgends zu sehen.
        if (r.valid()) {
            m_public_ui_registered = true;
        }
    } catch (...) {
    }
}

void RE4VRRecoil::draw_public_recoil_level() {
    // [STUFEN] OFF / Mid / Max = 0.00 / 0.75 / 1.50. Nach dem Cap-Fix schlagen
    // die Multiplikatoren voll durch -- die alten 1.00/3.00 waren zu hoch.
    // OFF schaltet den Rueckstoss komplett ab.
    struct Step {
        const char* label;
        double value;
    };

    static const Step steps[3] = {{"OFF", 0.0}, {"Mid", 0.75}, {"Max", 1.50}};
    const double cur = m_cfg.recoil_intensity_multiplier;

    g_framework->draw_menu_heading("Recoil", true);   // [UEBERSCHRIFT 11.09.2026] war orangerot, linksbuendig

    // [LINKSBUENDIG 11.09.2026] Die drei nebeneinander linksbuendig (war mittig),
    // mit MENU_BUTTON_GAP dazwischen.
    // [MENUE-AUSWAHL 11.09.2026] Auswahl-Kaestchen statt Knoepfen -- der Haken
    // zeigt die Wahl, die blaue Schrift der aktiven Stufe entfaellt.
    const auto button_gap = g_framework->menu_px(REFramework::MENU_BUTTON_GAP);

    for (int i = 0; i < 3; ++i) {
        const auto& st = steps[i];

        // [UEBERSCHRIFT 11.09.2026] Die Knoepfe stehen jetzt in eigener Zeile
        // unter der Ueberschrift -- SameLine deshalb nur noch ZWISCHEN ihnen.
        // Frueher stand es vor jedem Knopf, damit der erste neben "Recoil:" lag.
        if (i > 0) {
            ImGui::SameLine(0.0f, button_gap);
        }

        const auto id = std::string{st.label} + "##recoilpublic";

        if (g_framework->draw_menu_radio(id.c_str(), std::fabs(cur - st.value) < 0.001)) {
            m_cfg.recoil_intensity_multiplier = st.value;
            save_config();
        }
    }
}

void RE4VRRecoil::draw_dev_ui() {
    // [MENUE-REIHENFOLGE 2026-09-07] Frueher on_draw_ui -- REFramework rief das
    // in der Reihenfolge des Mod-Vektors auf, wodurch Public-Optionen und
    // Entwickler-Trees durcheinander standen. Gezeichnet wird jetzt zentral von
    // RE4VRMenu (alphabetisch, und nur wenn RE4VR_DEV_UI an ist).

    re4vr::trace("RE4VRRecoil", "on_draw_ui");
    // [SCRIPTGATE] Riegel zu = dieses Modul ist so still, als waere seine
    // Lua-Datei nicht geladen (s. re4vr::set_mods_gated).
    if (re4vr::mods_gated()) {
        return;
    }

    if (!ImGui::TreeNode("RE4VR - Recoil & Haptic")) {
        return;
    }

    bool changed = false;
    bool v = false;

    ImGui::Text("VR Recoil Settings");

    v = m_cfg.enable_recoil;

    if (ImGui::Checkbox("Enable Recoil", &v)) {
        m_cfg.enable_recoil = v;
        changed = true;
    }

    if (m_cfg.enable_recoil) {
        const auto current_wid = get_current_weapon_id();
        double base_mult = 1.0;
        double per_mult = 1.0;
        double global_mult = 1.0;
        std::string key;
        const double eff_mult =
            get_effective_weapon_recoil_multiplier(base_mult, per_mult, global_mult, key);

        if (current_wid.has_value()) {
            const auto weapon_name = weapon_key_from_id(*current_wid);
            ImGui::Text("Current Weapon: %s (Recoil: %.2fx  | base %.1fx x global %.2f x weapon %.2f)",
                        weapon_name.c_str(), eff_mult, base_mult, global_mult, per_mult);

            if (eff_mult >= 3.0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "(VERY HIGH)");
            } else if (eff_mult >= 2.0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{1.0f, 0.6f, 0.3f, 1.0f}, "(HIGH)");
            } else if (eff_mult >= 1.5) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{1.0f, 0.8f, 0.3f, 1.0f}, "(MEDIUM)");
            }
        }

        ImGui::Indent(20.0f);

        {
            float g = static_cast<float>(m_cfg.recoil_intensity_multiplier);

            if (ImGui::SliderFloat("General Weapon Intensity", &g, 0.0f, 3.0f, "%.2f")) {
                m_cfg.recoil_intensity_multiplier = g;
                changed = true;
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Scales all recoil, on top of weapon class scaling and per-weapon overrides.");
            }
        }

        if (current_wid.has_value()) {
            const auto wkey = weapon_key_from_id(*current_wid);

            {
                const auto it = m_weapon_intensity_overrides.find(wkey);
                float cur = (it != m_weapon_intensity_overrides.end())
                                ? static_cast<float>(it->second)
                                : 1.0f;

                const bool ch = ImGui::SliderFloat("Weapon Intensity", &cur, 0.0f, 10.0f, "%.2f");

                // [1:1] changed wird HIER vor der Schluesselpruefung gesetzt --
                // anders als beim Support-Slider unten. Nicht vereinheitlichen.
                changed = changed || ch;

                if (ch) {
                    if (std::fabs(cur - 1.0f) < 0.0001f) {
                        m_weapon_intensity_overrides.erase(wkey);
                    } else {
                        m_weapon_intensity_overrides[wkey] = cur;
                    }
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Clear##wp_int_clear")) {
                m_weapon_intensity_overrides.erase(wkey);
                changed = true;
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Clears only this weapon's override (back to 1.00).");
            }

            // [SUPPORT_PER_WEAPON] Wie weit die linke (Stuetz-)Hand DIESE Waffe
            // beruhigt. 100% = Stuetzhand aendert nichts.
            const double sdef = RECOIL_SUPPORTED_IMPULSE_MULT * 100.0;
            {
                const auto it = m_weapon_support_overrides.find(wkey);
                float scur = (it != m_weapon_support_overrides.end())
                                 ? static_cast<float>(it->second * 100.0)
                                 : static_cast<float>(sdef);

                // [1:1] hier steht changed NUR innerhalb des Zweigs.
                if (ImGui::SliderFloat("Support-Hand: Recoil auf %", &scur, 10.0f, 100.0f,
                                       "%.0f %%")) {
                    m_weapon_support_overrides[wkey] =
                        (std::max)(0.10, (std::min)(1.0, static_cast<double>(scur) / 100.0));
                    changed = true;
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Clear##wp_sup_clear")) {
                m_weapon_support_overrides.erase(wkey);
                changed = true;
            }

            if (ImGui::IsItemHovered()) {
                char buf[128]{};
                std::snprintf(buf, sizeof(buf), "Zurueck auf den Default (%.0f %%).", sdef);
                ImGui::SetTooltip("%s", buf);
            }
        }

        if (ImGui::TreeNode("All Weapon Intensities")) {
            build_weapon_id_catalog_sorted();
            ImGui::Text("Tip: set to 1.00 to clear an override.");
            ImGui::Separator();

            const double sdef_all = RECOIL_SUPPORTED_IMPULSE_MULT * 100.0;

            for (const int32_t wid : m_weapon_id_catalog_sorted) {
                const auto key2 = weapon_key_from_id(wid);

                {
                    const auto it = m_weapon_intensity_overrides.find(key2);
                    float cur = (it != m_weapon_intensity_overrides.end())
                                    ? static_cast<float>(it->second)
                                    : 1.0f;

                    char label[64]{};
                    std::snprintf(label, sizeof(label), "%s##all_weapon_int_%d", key2.c_str(), wid);

                    if (ImGui::SliderFloat(label, &cur, 0.0f, 10.0f, "%.2f")) {
                        if (std::fabs(cur - 1.0f) < 0.0001f) {
                            m_weapon_intensity_overrides.erase(key2);
                        } else {
                            m_weapon_intensity_overrides[key2] = cur;
                        }

                        changed = true;
                    }
                }

                {
                    const auto it = m_weapon_support_overrides.find(key2);
                    float scur = (it != m_weapon_support_overrides.end())
                                     ? static_cast<float>(it->second * 100.0)
                                     : static_cast<float>(sdef_all);

                    char slabel[64]{};
                    std::snprintf(slabel, sizeof(slabel), "  ^ Support %%##all_weapon_sup_%d", wid);

                    if (ImGui::SliderFloat(slabel, &scur, 10.0f, 100.0f, "%.0f %%")) {
                        m_weapon_support_overrides[key2] =
                            (std::max)(0.10, (std::min)(1.0, static_cast<double>(scur) / 100.0));
                        changed = true;
                    }
                }
            }

            ImGui::TreePop();
        }

        ImGui::Unindent(20.0f);
    }

    if (changed) {
        save_config();
    }

    // ---- Haptik-Sub-Tree ----
    if (ImGui::TreeNode("Schuss-Haptik")) {
        bool hchanged = false;
        bool hv = m_haptic.enabled;

        if (ImGui::Checkbox("Aktiv", &hv)) {
            m_haptic.enabled = hv;
            hchanged = true;
        }

        const auto slider = [&](const char* label, double& dst, float lo, float hi,
                                const char* fmt) {
            float t = static_cast<float>(dst);

            if (ImGui::SliderFloat(label, &t, lo, hi, fmt)) {
                dst = t;
                hchanged = true;
            }
        };

        slider("Rechts allein (100%)", m_haptic.amp_solo, 0.0f, 1.0f, "%.2f");
        slider("Rechts mit Support (70%)", m_haptic.amp_right_sup, 0.0f, 1.0f, "%.2f");
        slider("Links Support (30%)", m_haptic.amp_left_sup, 0.0f, 1.0f, "%.2f");
        slider("Delay (s) - Haptik kommt spaeter", m_haptic.delay, 0.0f, 0.30f, "%.3f");
        slider("Dauer (s)", m_haptic.dur, 0.01f, 0.20f, "%.3f");
        slider("Frequenz (Hz)", m_haptic.freq, 40.0f, 300.0f, "%.0f");

        if (hchanged) {
            save_haptic_config();
        }

        ImGui::TreePop();
    }

    // [MENUE-REIHENFOLGE 2026-09-07] Hier stand der Fallback "kein Lua-Dispatcher
    // -> Public-Block selbst zeichnen". Genau der liess die nackten Optionen
    // zwischen den Entwickler-Trees auftauchen, seit ##re4_vr_menu.lua mit dem
    // Port abgeschaltet ist. Gezeichnet wird jetzt zentral in
    // RE4VRMenu::draw_public, in fester Reihenfolge und ganz oben.

    ImGui::TreePop();
}

#endif // RE4
