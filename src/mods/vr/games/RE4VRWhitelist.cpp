// ============================================================================
// RE4VRWhitelist -- 1:1-Portierung von re4_vr_whitelist.lua
// Spezifikation: I:\LUATRANS\PORT_WHITELIST_SPEC.md
// ============================================================================

#if defined(RE4)

#include <array>
#include <cstring>
#include <fstream>

#include <sdk/REContext.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>

#include "../../../mods/ScriptRunner.hpp"

#include "RE4VR.hpp"
#include "RE4VRWhitelist.hpp"

namespace {

// ---------------------------------------------------------------------------
// Die japanischen Sortenwoerter als explizite UTF-8-BYTEFOLGEN.
//
// Gesucht wird im Original mit nm:find(w, 1, true) -- PLAIN, also reine
// Byte-Suche; als Lua-Pattern waeren die 3-Byte-Zeichen Gluecksache.
// std::string::find ist ebenfalls eine Byte-Suche, das passt also.
// Die Bytes sind direkt aus der Lua-Datei gezogen, nicht abgetippt -- die
// Quelldatei-Kodierung des Compilers kann uns damit nicht dazwischenfunken.
// ---------------------------------------------------------------------------
constexpr const char* W_FASS_KANJI = "\xE6\xA8\xBD";                              // Fass, Kanji
constexpr const char* W_FASS_HIRA = "\xE3\x81\x9F\xE3\x82\x8B";                   // Fass, Hiragana
constexpr const char* W_FASS_KATA = "\xE3\x82\xBF\xE3\x83\xAB";                   // Fass, Katakana
constexpr const char* W_HOLZKISTE = "\xE6\x9C\xA8\xE7\xAE\xB1";                   // Holzkiste
constexpr const char* W_FENSTER = "\xE7\xAA\x93";                                 // Fenster
constexpr const char* W_VASE = "\xE5\xA3\xBA";                                    // Vase / Krug
constexpr const char* W_KLEIN = "\xE5\xB0\x8F";                                   // klein
constexpr const char* W_MUENZE = "\xE9\x9D\x92\xE3\x82\xB3\xE3\x82\xA4\xE3\x83\xB3"; // Blaue Muenze

constexpr const char* MUENZE_STAMM = "gm84_508_00_";

// [UND-MATCHING] Diese Typen sind eine ZUSAETZLICHE Bedingung, kein zweiter
// Freifahrtschein: ein Objekt zaehlt nur, wenn es einen dieser Typen traegt UND
// sein Name passt. GmWoodBoxBase ist live die Basis von genau vier Klassen
// (GmWoodBox, GmWoodBoxMotion, GmPhasedWoodBox, GmSmoothWoodBox) -- Vasen UND
// Faesser tragen die letzte, die Klasse allein unterscheidet die Sorten also
// NICHT. Fenster erben von GmOMUnitBase und fielen deshalb durch das UND,
// egal wie gut der Name passte -> eigener Typ.
constexpr std::array<const char*, 2> BREAKABLE_TYPES = {
    "chainsaw.GmWoodBoxBase",
    "chainsaw.GmWindow",
};

// [SONDERFAELLE] Gelten OHNE Typbedingung, reines ODER -- als Praefix.
// Drei Schreibweisen live belegt; der kurze Stamm deckt die Form mit _0_ mit ab.
constexpr std::array<const char*, 1> BREAKABLE_NAME_ONLY = {
    MUENZE_STAMM,
};

// [SORTENWORT] Irgendwo im Namen, gilt NUR zusammen mit dem Typ.
// Alle drei japanischen Schriften kommen vor -- das ist der Beweis, warum ein
// einzelnes Fass-Wort nicht reicht: die Asset-Nummer ist nicht gemeinsam
// (500, 565, 566 sind verschiedene Assets), gemeinsam ist nur das Wort.
constexpr std::array<const char*, 6> BREAKABLE_NAME_CONTAINS = {
    W_FASS_KANJI, W_FASS_HIRA, W_FASS_KATA, W_HOLZKISTE, W_FENSTER, W_VASE,
};

// [SORTENWORT OHNE TYP] Die Muenze haengt am Pendel und traegt nur
// IGimmickDurability -- sie ist keine WoodBox.
constexpr std::array<const char*, 1> BREAKABLE_CONTAINS_ONLY = {
    W_MUENZE,
};

bool contains_bytes(const std::string& hay, const char* needle) {
    return needle != nullptr && *needle != '\0' && hay.find(needle) != std::string::npos;
}

bool starts_with(const std::string& hay, const std::string& pre) {
    return hay.size() >= pre.size() && hay.compare(0, pre.size(), pre) == 0;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n\v\f");   // [KL1] Luas %s

    if (b == std::string::npos) {
        return {};
    }

    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

} // namespace

std::shared_ptr<RE4VRWhitelist>& RE4VRWhitelist::get() {
    static auto inst = std::make_shared<RE4VRWhitelist>();
    return inst;
}

// ============================================================================
// Typen und Capture
// ============================================================================

void RE4VRWhitelist::clear_types() {
    for (size_t i = 0; i < m_tds.size(); ++i) {
        if (m_tds_reffed[i] && m_tds[i] != nullptr && re4vr::obj_ok(m_tds[i])) {
            utility::re_managed_object::release(m_tds[i]);
        }
    }

    m_tds.clear();
    m_tds_reffed.clear();
}

void RE4VRWhitelist::resolve_types() {
    // Typedefs bei JEDEM Load frisch -- Aenderungen greifen per Reset Scripts.
    clear_types();

    for (const char* n : BREAKABLE_TYPES) {
        auto* t = re4vr::runtime_type(n);

        if (t == nullptr) {
            continue;   // Lua: `if t then` -- nil-Typen kommen gar nicht in die Liste
        }

        bool reffed = false;

        if (re4vr::obj_ok(t) && static_cast<int32_t>(t->referenceCount) > 0) {
            utility::re_managed_object::add_ref(t);
            reffed = true;
        }

        m_tds.push_back(t);
        m_tds_reffed.push_back(reffed);
    }
}

void RE4VRWhitelist::load_capture() {
    // [CAPTURE-PERSISTENZ] Vom In-Game-Capture-Tool gesammelte Staemme.
    // Hartkodiert steht in BREAKABLE_NAME_PREFIXES NICHTS -- die Liste kommt
    // ausschliesslich von hier.
    m_name_prefixes.clear();

    std::vector<std::string> seen;

    const auto add = [&](const std::string& raw) {
        const auto v = trim(raw);

        if (v.empty() || v[0] == '#') {
            return;   // leer oder Kommentarzeile
        }

        for (const auto& s : seen) {
            if (s == v) {
                return;   // Duplikat
            }
        }

        seen.push_back(v);
        m_name_prefixes.push_back(v);
    };

    // 1) Die JSON bei den anderen Configs.
    try {
        const auto d = re4vr::json_load("re4_vr/re4_vr_breakables.json");

        if (d.is_object() && d.contains("stems") && d["stems"].is_array()) {
            for (const auto& v : d["stems"]) {
                // [KL2] Lua macht tostring(v) -- eine Zahl in der Liste zaehlt
                // also mit.
                if (v.is_string()) {
                    add(v.get<std::string>());
                } else if (v.is_number_integer()) {
                    add(std::to_string(v.get<long long>()));
                } else if (v.is_number()) {
                    add(std::to_string(v.get<double>()));
                }
            }
        }
    } catch (...) {
    }

    // 2) [ABLAGE] Die beiden ALTEN TXT-Pfade werden weiterhin MITGELESEN, damit
    // nichts haengenbleibt, das dort noch liegt. Frueher lag die Sammlung als
    // lose TXT direkt in data/ -- zwischen den Logs, die aufgeraeumt werden;
    // so ist schon einmal eine komplette Sammlung verlorengegangen.
    for (const char* rel : {"re4_vr/re4_breakable_whitelist.txt", "re4_breakable_whitelist.txt"}) {
        try {
            std::ifstream f{re4vr::datadir() / rel};

            if (!f.is_open()) {
                continue;
            }

            std::string line;

            while (std::getline(f, line)) {
                add(line);
            }
        } catch (...) {
        }
    }
}

std::optional<std::string> RE4VRWhitelist::on_initialize() {
    resolve_types();
    load_capture();
    return Mod::on_initialize();
}

// ============================================================================
// Export nach Lua
// ============================================================================

void RE4VRWhitelist::publish(sol::state& lua) {
    // [RUECKBAU] false -> altes Verhalten (Typ ODER Name). Nur setzen, wenn es
    // noch nicht steht -- Luas `if _G.x == nil then ... end`.
    if (!lua["__re4_wl_require_type"].valid()) {
        lua["__re4_wl_require_type"] = true;
    }

    // Die Datenlisten muessen echte Lua-Tabellen sein: weapons.lua liest sie
    // direkt.
    const auto to_table = [&](auto&& src) {
        sol::table t = lua.create_table();
        int i = 1;

        for (const auto& v : src) {
            t[i++] = std::string{v};
        }

        return t;
    };

    lua["__re4_breakable_name_prefixes"] = to_table(m_name_prefixes);
    lua["__re4_breakable_name_only"] = to_table(BREAKABLE_NAME_ONLY);
    lua["__re4_breakable_name_contains"] = to_table(BREAKABLE_NAME_CONTAINS);
    lua["__re4_breakable_contains_only"] = to_table(BREAKABLE_CONTAINS_ONLY);

    {
        sol::table t = lua.create_table();
        int i = 1;

        for (auto* td : m_tds) {
            t[i++] = td;
        }

        lua["__re4_breakable_tds"] = t;
    }

    // Die Y-Versaetze. Werte 1:1.
    lua["__re4_coin_off_y"] = -1.2;     // Muenze haengt am Pendel, Pivot sitzt OBEN
    lua["__re4_vase_off_y"] = -0.4;     // nur die KLEINE Vase, an ihr gemessen
    lua["__re4_snake_off_y"] = -0.8;    // Schlange liegt flach am Boden
    lua["__re4_animal_off_y"] = -0.3;   // Kraehe/Huhn
    lua["__re4_mouse_off_y"] = -0.9;    // Ratte sitzt noch flacher
    lua["__re4_animal_center_max_d"] = 3.0;

    // Die vier Funktionen. weapons.lua ruft sie INLINE auf (200-Local-Limit).
    // [M4] Im Original liegt der KOMPLETTE Rumpf jeder Funktion in einem
    // pcall -- sie konnten per Konstruktion nie einen Fehler nach oben geben.
    // Die Aufrufstellen in weapons.lua haben KEIN pcall, und go.as<...>()
    // kann eine sol::error werfen. Also jede Lambda absichern.
    lua["__re4_is_real_breakable_prop"] = [](sol::object go) -> bool {
        try {
            if (!go.valid() || go.get_type() != sol::type::userdata) {
                return false;
            }

            return RE4VRWhitelist::get()->is_real_breakable_prop(go.as<::REManagedObject*>());
        } catch (...) {
            return false;
        }
    };

    lua["__re4_breakable_yoff"] = [](sol::object go) -> float {
        try {
            if (!go.valid() || go.get_type() != sol::type::userdata) {
                return 0.5f;
            }

            return RE4VRWhitelist::get()->breakable_yoff(go.as<::REManagedObject*>());
        } catch (...) {
            return 0.5f;
        }
    };

    lua["__re4_enemy_off_y"] = [](sol::object ctx) -> float {
        try {
            if (!ctx.valid() || ctx.get_type() != sol::type::userdata) {
                return 0.0f;
            }

            return RE4VRWhitelist::get()->enemy_off_y(ctx.as<::REManagedObject*>());
        } catch (...) {
            return 0.0f;
        }
    };

    lua["__re4_animal_center"] = [](sol::object anim, float fx, float fy, float fz) {
        try {
            if (!anim.valid() || anim.get_type() != sol::type::userdata) {
                return std::make_tuple(fx, fy, fz);
            }

            return RE4VRWhitelist::get()->animal_center(anim.as<::REManagedObject*>(), fx, fy, fz);
        } catch (...) {
            return std::make_tuple(fx, fy, fz);
        }
    };
}

void RE4VRWhitelist::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRWhitelist", "on_lua_state_created");
    publish(lua);
}

void RE4VRWhitelist::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRWhitelist", "on_lua_state_destroyed");
    // Das Original wird beim Reset neu ausgefuehrt: Typedefs frisch, Capture
    // frisch. Genau das hier.
    resolve_types();
    load_capture();
}

// ============================================================================
// is_real_breakable_prop
// ============================================================================

// [M3] Die Praefix-Liste zur Laufzeit aus Lua holen. Faellt auf die eigene
// Kopie zurueck, wenn der State nicht erreichbar ist.
const std::vector<std::string>& RE4VRWhitelist::live_prefixes() {
    m_live_prefixes.clear();

    // [ABSTURZ 04.09.2026] LuaRef haelt die Sperre des ScriptRunners -- ohne sie
    // lief dieser Zugriff gegen einen State, den ein anderer Thread benutzt.
    re4vr::LuaRef lua;

    if (lua != nullptr) {
        {
            try {
                sol::object o = (*lua)["__re4_breakable_name_prefixes"];

                if (o.valid() && o.get_type() == sol::type::table) {
                    sol::table t = o.as<sol::table>();

                    for (size_t i = 1; i <= t.size(); ++i) {
                        sol::object v = t[i];

                        if (v.valid() && v.get_type() == sol::type::string) {
                            m_live_prefixes.push_back(v.as<std::string>());
                        }
                    }

                    return m_live_prefixes;
                }
            } catch (...) {
            }
        }
    }

    m_live_prefixes = m_name_prefixes;
    return m_live_prefixes;
}

bool RE4VRWhitelist::is_real_breakable_prop(::REManagedObject* go) {
    if (go == nullptr) {
        return false;
    }

    // [UND-MATCHING] Typ und Name werden ueber die GANZE Kette (self + 4
    // Parents) GESAMMELT und erst am Ende verknuepft -- sie sitzen naemlich oft
    // auf VERSCHIEDENEN Ebenen (HitController auf dem Kind "Before"/"After",
    // WoodBox-Komponente + gm-Name am Parent).
    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
    bool has_type = false;
    bool has_name = false;

    for (int i = 0; i <= 4; ++i) {
        if (tf == nullptr) {
            break;
        }

        auto* g = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");

        if (g != nullptr) {
            if (!has_type) {
                for (auto* td : m_tds) {
                    if (re4vr::call_safe<::REManagedObject*>(g, "getComponent(System.Type)", td)
                        != nullptr) {
                        has_type = true;
                        break;
                    }
                }
            }

            const std::string nm = re4vr::obj_name(g);

            if (!nm.empty()) {
                // Sonderfaelle gelten SOFORT, ohne Typbedingung -- als Praefix ...
                for (const char* pre : BREAKABLE_NAME_ONLY) {
                    if (starts_with(nm, pre)) {
                        return true;
                    }
                }

                // ... und als Wort irgendwo im Namen: das Muenz-Wort steht mal
                // vorne, mal mittendrin.
                for (const char* w : BREAKABLE_CONTAINS_ONLY) {
                    if (contains_bytes(nm, w)) {
                        return true;
                    }
                }

                if (!has_name) {
                    // [M3] BEWUSST aus der Lua-Tabelle, nicht aus m_name_prefixes:
                    // #Breakable_Capture.lua haengt neue Staemme LIVE an genau
                    // diese Tabelle an ("Homing greift sofort"). Eine C++-Kopie
                    // saehe sie erst nach einem Reset Scripts.
                    for (const auto& pre : live_prefixes()) {
                        if (starts_with(nm, pre)) {
                            has_name = true;
                            break;
                        }
                    }
                }

                // Sortenwort IRGENDWO im Namen -- plain, kein Pattern.
                if (!has_name) {
                    for (const char* w : BREAKABLE_NAME_CONTAINS) {
                        if (contains_bytes(nm, w)) {
                            has_name = true;
                            break;
                        }
                    }
                }
            }
        }

        tf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
    }

    // [SICHERUNG] Liefert die Typaufloesung nichts (leere Typliste), waere sonst
    // ALLES tot -> die Typbedingung gilt dann als erfuellt (Verhalten wie vor
    // dem UND).
    // [M5] Lua prueft `~= false`: NUR ein echtes false schaltet ab, jeder
    // andere Wert (Zahl, String, Tabelle) gilt als an. lua_get_tribool liefert
    // fuer Nicht-Booleans 0 und haette sie faelschlich als false gelesen.
    bool require_type = true;

    {
        re4vr::LuaRef lua;

        if (lua != nullptr) {
            {
                try {
                    sol::object o = (*lua)["__re4_wl_require_type"];

                    if (o.valid() && o.get_type() == sol::type::boolean && !o.as<bool>()) {
                        require_type = false;
                    }
                } catch (...) {
                }
            }
        }
    }

    const bool need_type = require_type && !m_tds.empty();

    if (!has_name) {
        return false;
    }

    return !need_type || has_type;
}

// ============================================================================
// breakable_yoff
// ============================================================================

float RE4VRWhitelist::breakable_yoff(::REManagedObject* go) {
    if (go == nullptr) {
        return 0.5f;
    }

    auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");

    // Nur DREI Parents hier, nicht vier -- 1:1 wie im Original.
    for (int i = 0; i <= 3; ++i) {
        if (tf == nullptr) {
            break;
        }

        auto* g = re4vr::call_safe<::REManagedObject*>(tf, "get_GameObject");
        const std::string nm = (g != nullptr) ? re4vr::obj_name(g) : std::string{};

        if (!nm.empty()) {
            // Beide Wege fragen denselben Namen: ohne diesen Zweig bekaeme die
            // Muenze den Default +0.5, also 0,5 m UEBER dem Pendel-Pivot, der
            // ohnehin schon oben sitzt -- der Wurf ginge drueber.
            if (contains_bytes(nm, W_MUENZE) || starts_with(nm, MUENZE_STAMM)) {
                const double v = re4vr::lua_get_number("__re4_coin_off_y", -0.8);
                return static_cast<float>(v);
            }

            // Beide Zeichen EINZELN gesucht, damit alle Klammer-Schreibweisen
            // mitgehen. Nur die KLEINE Vase bekommt den Wert -- er wurde an ihr
            // gemessen; die grosse steht am Boden und ist ~1 m hoch, fuer sie
            // waere er unter dem Objekt.
            if (contains_bytes(nm, W_VASE) && contains_bytes(nm, W_KLEIN)) {
                const double v = re4vr::lua_get_number("__re4_vase_off_y", 0.15);
                return static_cast<float>(v);
            }

            // Das Explosionsfass bleibt bewusst beim Default: sein Ursprung
            // sitzt am Fassboden, +0.5 ist die Mitte.
        }

        tf = re4vr::call_safe<::REManagedObject*>(tf, "get_Parent");
    }

    return 0.5f;
}

// ============================================================================
// enemy_off_y
// ============================================================================

float RE4VRWhitelist::enemy_off_y(::REManagedObject* ctx) {
    if (ctx == nullptr) {
        return 0.0f;
    }

    auto* def = utility::re_managed_object::get_type_definition(ctx);

    if (def == nullptr) {
        return 0.0f;
    }

    std::string tn;

    try {
        tn = def->get_full_name();
    } catch (...) {
        return 0.0f;
    }

    // Die Schlange ist ein GEGNER, liegt aber flach am Boden -> der
    // Gegner-Zielpunkt (Fallback pos.y + 0.9) zielt DRUEBER. Alle anderen
    // Gegner: 0, unveraendert.
    if (tn.find("Ch8g2z0") != std::string::npos) {
        return static_cast<float>(re4vr::lua_get_number("__re4_snake_off_y", -0.8));
    }

    return 0.0f;
}

// ============================================================================
// animal_center
// ============================================================================

std::tuple<float, float, float> RE4VRWhitelist::animal_center(::REManagedObject* anim, float fx,
                                                              float fy, float fz) {
    // ECHTER Mesh-Mittelpunkt statt Konstante. Eine feste Abwaerts-Konstante
    // faehrt den Zielpunkt in DAS hinein, worauf das Tier sitzt: Rabe am Boden
    // + (-0.3) = Luft drunter, egal -- Rabe auf dem Gelaender + (-0.3) =
    // Zielpunkt IM Gelaender -> los_clear scheitert -> das Tier wird gar nicht
    // erst gepickt. Nachtunen hilft da grundsaetzlich nicht.
    const auto fallback = std::make_tuple(fx, fy, fz);

    if (anim == nullptr) {
        return fallback;
    }

    auto* mesh = re4vr::call_safe<::REManagedObject*>(anim, "get_Mesh");

    if (mesh == nullptr) {
        return fallback;
    }

    // [K1] via.AABB ist ein VALUETYPE (32 Byte: minpos + maxpos), KEIN Objekt.
    // Ein call_safe<REManagedObject*> waehlt ueber sizeof(T) den NICHT-sret-
    // Zweig und ruft f(context, mesh) statt f(&out, context, mesh) -- RCX ist
    // dann der VMContext-Zeiger, und die Engine schreibt 32 Byte HINEIN.
    // Also der versteckte Out-Zeiger mit eigenem, 16-Byte-ausgerichtetem
    // Puffer, genau wie bei via.vec3/via.quat.
    // get_WorldAABB ist BEREITS Weltraum -- kein Transformieren noetig.
    const auto m_mesh = utility::re_managed_object::get_type_definition(mesh);

    if (m_mesh == nullptr) {
        return fallback;
    }

    auto* method = m_mesh->get_method("get_WorldAABB");

    if (method == nullptr) {
        return fallback;
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
        return fallback;
    }

    // [K2] Die Feld-Offsets muessen ueber get_offset_from_fieldptr() kommen --
    // sdk::REField::get_data reicht is_value_type gar nicht durch und rechnet
    // immer mit get_offset_from_base(). Bei einem ValueType waeren das die
    // falschen Offsets.
    //
    // !! via.AABB:getCenter NICHT BENUTZEN !! Die Methode existiert im TDB,
    // liefert ueber die ValueType-Bruecke aber MUELL -- gemessen
    // 944337.50/5.83/-51.44, waehrend minpos/maxpos derselben AABB sauber
    // waren. Fies daran: getCenter gibt PLAUSIBLE Zahlen zurueck, nur falsche,
    // ein "ist es eine Zahl?"-Test faellt darauf rein. Also min/max selbst
    // mitteln.
    auto* aabb_td = sdk::find_type_definition("via.AABB");

    if (aabb_td == nullptr) {
        return fallback;
    }

    auto* f_min = aabb_td->get_field("minpos");
    auto* f_max = aabb_td->get_field("maxpos");

    if (f_min == nullptr || f_max == nullptr) {
        return fallback;
    }

    const auto off_min = f_min->get_offset_from_fieldptr();
    const auto off_max = f_max->get_offset_from_fieldptr();

    if (off_min + sizeof(glm::vec3) > sizeof(aabb_buf)
        || off_max + sizeof(glm::vec3) > sizeof(aabb_buf)) {
        return fallback;
    }

    glm::vec3 mn{};
    glm::vec3 mx{};
    std::memcpy(&mn, aabb_buf + off_min, sizeof(mn));
    std::memcpy(&mx, aabb_buf + off_max, sizeof(mx));

    // Leere AABB: die Engine initialisiert sie mit min = +FLT_MAX,
    // max = -FLT_MAX. Mitteln ergaebe 0/0/0, also mitten in der Welt.
    if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) {
        return fallback;
    }

    const float cx = (mn.x + mx.x) * 0.5f;
    const float cy = (mn.y + mx.y) * 0.5f;
    const float cz = (mn.z + mx.z) * 0.5f;

    // Sanity gegen den Fallback-Punkt. Gleiche Lektion wie der
    // [KOPF-SANITY]-Test in __re4_enemy_aim_point, wo ein FERNER, fremder Kopf
    // das Ziel als 33 m weg gelten liess. Ein Kraehen-Mesh-Center liegt real
    // unter 0,5 m vom Ursprung.
    const double m = re4vr::lua_get_number("__re4_animal_center_max_d", 3.0);
    const float dx = cx - fx;
    const float dy = cy - fy;
    const float dz = cz - fz;

    if ((dx * dx + dy * dy + dz * dz) > static_cast<float>(m * m)) {
        return fallback;
    }

    return std::make_tuple(cx, cy, cz);
}

#endif // RE4
