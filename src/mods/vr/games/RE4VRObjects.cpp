// ============================================================================
// RE4VRObjects -- 1:1-Portierung von re4_vr_objects.lua
// Spezifikation: I:\LUATRANS\PORT_OBJECTS_SPEC.md
// ============================================================================

#if defined(RE4)

#include <cmath>
#include <cstdio>
#include <ctime>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>

#include "../../../mods/ScriptRunner.hpp"

#include "RE4VR.hpp"
#include "RE4VRObjects.hpp"

namespace {

double now_clock() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// via.vec3-Getter -- Rueckgabe groesser als ein Register, also versteckter
// Out-Zeiger PLUS 16-Byte-Ausrichtung.
bool get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out) {
    if (obj == nullptr) {
        return false;
    }

    auto def = utility::re_managed_object::get_type_definition(obj);

    if (def == nullptr) {
        return false;
    }

    const auto method = def->get_method(name);

    if (method == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{0.0f, 0.0f, 0.0f, 0.0f};
    bool ok = false;

    try {
        method->call_safe<glm::vec4*>(&buf, context, obj);
        ok = true;
    } catch (...) {
        ok = false;
    }

    re4vr::clear_vm_exception();

    if (ok) {
        out = glm::vec3{buf.x, buf.y, buf.z};
    }

    return ok;
}

// Ein Feld lesen, das eine Zahl ODER ein Enum-Objekt sein kann -- Luas
// `if type(x) ~= "number" then x = x:get_field("value__") end`.
//
// [K1] Das zweite Argument von get_data_raw beschreibt den CONTAINER, nicht
// den Feldtyp (RETypeDB.cpp:344-363): true -> get_offset_from_fieldptr(),
// false -> get_offset_from_base() (bei einer Klasse i.d.R. +0x10).
// Wer hier den FELDtyp uebergibt, liest bei jedem Enum um den fieldptr-Offset
// daneben. Lua macht es richtig: get_native_field_from_field uebergibt
// `ty->is_value_type() && !managed_obj_passed`.
std::optional<int64_t> num_or_enum_field(void* obj, sdk::RETypeDefinition* def,
                                         const char* field, bool container_is_value) {
    if (obj == nullptr || def == nullptr) {
        return std::nullopt;
    }

    auto* f = def->get_field(field);

    if (f == nullptr) {
        return std::nullopt;
    }

    auto* ft = f->get_type();

    if (ft == nullptr) {
        return std::nullopt;
    }

    auto* raw = f->get_data_raw(obj, container_is_value);

    if (raw == nullptr) {
        return std::nullopt;
    }

    // Enum -> Grundtyp; sonst der deklarierte Typ.
    std::string tn = ft->get_full_name();

    if (ft->is_enum()) {
        if (auto* ut = ft->get_underlying_type()) {
            tn = ut->get_full_name();
        } else {
            tn = "System.Int32";
        }
    }

    if (tn == "System.Int64" || tn == "System.UInt64") {
        return *reinterpret_cast<int64_t*>(raw);
    }

    if (tn == "System.Int16") {
        return *reinterpret_cast<int16_t*>(raw);
    }

    if (tn == "System.UInt16") {
        return *reinterpret_cast<uint16_t*>(raw);
    }

    if (tn == "System.SByte") {
        return *reinterpret_cast<int8_t*>(raw);
    }

    if (tn == "System.Byte") {
        return *reinterpret_cast<uint8_t*>(raw);
    }

    if (tn == "System.UInt32") {
        return static_cast<int64_t>(*reinterpret_cast<uint32_t*>(raw));
    }

    return *reinterpret_cast<int32_t*>(raw);
}

} // namespace

// ============================================================================
// Zonen und Ausnahmen
// ============================================================================

// [MITTELPUNKT = SITZPLATZ, nicht der Monitor-Dump] Der erste Versuch stand mit
// 89.74/20.90/12.15 NEBEN der Kabine und loeste nie aus. Die Zonen-Spur lieferte
// 87.51/20.95/12.99; im Vergleichslauf OHNE Mod stand der Spieler nach dem
// Aufnehmen bei 87.57/20.95/12.72 -- dasselbe Fleckchen. Radius 2.5 deckt den
// Anlaufpunkt 89.31/20.90/11.75 mit ab, die Abschaltung greift also rund eine
// halbe Sekunde VOR dem Aufnehmen.
//
// Warum Ortszone und nicht der Killswitch-Zuender: Ada hat keinen Knopfdruck,
// man laeuft direkt in den Einstieg hinein -- der ParentGimmick-Zustand steht
// erst, wenn die Gondel den Charakter schon geprueft hat, und das ist zu spaet.
// Bei Leon kam der Killswitch ebenfalls zu spaet (is_on_gondola schlaegt erst
// rund eine Sekunde nach dem Aufnehmen an).
//
// [RADIUS LEON 02.09.2026, Ansage des Users: 1.6 war viel zu gross] Jetzt 0.8.
// Untergrenze ist der zweite gemessene Standplatz 182.85/26.27/24.28 -- der
// liegt 0.39 m vom Mittelpunkt, 0.8 laesst also gut das Doppelte an Reserve und
// nimmt den Vorplatz nicht mehr mit.
const std::array<RE4VRObjects::Zone, 2> RE4VRObjects::ZONEN = {{
    {60850, 87.51f, 20.95f, 12.99f, 2.5f, "Adas Gondel, Einstieg"},
    {56100, 182.46f, 26.27f, 24.30f, 0.8f, "Leons Gondel, Einstieg"},
}};

// WAS AN BLEIBT UND WARUM (jedes einzeln bezahlt):
//   binding     -- haelt das virtuelle Pad. Wird es mitten im Druck entladen,
//                  bleibt die letzte Richtung stehen und es geht KEINE Eingabe
//                  mehr (live 02.09.).
//   materials   -- blendet Adas Koerper waehrend der Fahrt aus. Ohne materials
//                  blieb der Kopf aus dem Gameplay davor ausgeblendet stehen.
//   firstperson -- Ansage des Users.
// ACHTUNG: binding requiret killswitch und frame_cache -- die laufen zwangs-
// laeufig mit; Unterordner stehen ohnehin nicht in der Script-Liste.
const std::array<const char*, 4> RE4VRObjects::BLEIBT = {
    "re4_vr_objects.lua",
    "re4_vr_binding.lua",
    "re4_vr_materials.lua",
    "re4_vr_firstperson.lua",
};

std::shared_ptr<RE4VRObjects>& RE4VRObjects::get() {
    static auto inst = std::make_shared<RE4VRObjects>();
    return inst;
}

std::optional<std::string> RE4VRObjects::on_initialize() {
    m_geladen_um = now_clock();   // [TOT] s. Header

    return Mod::on_initialize();
}

void RE4VRObjects::on_lua_state_created(sol::state& lua) {
    re4vr::trace("RE4VRObjects", "on_lua_state_created");
    // Doppelload-Guard des Originals -- nativ gegenstandslos, 1:1 gesetzt.
    lua["__re4_objects_installed"] = true;
}

void RE4VRObjects::on_lua_state_destroyed(sol::state& lua) {
    re4vr::trace("RE4VRObjects", "on_lua_state_destroyed");
    // [K3/M2] In Lua sind das alles LOCALS: der Reset laedt objects.lua neu und
    // sie stehen wieder auf ihren Anfangswerten. Ohne das
    //  * blieb die Reset-Sperre bis zu 3 s stehen statt ~2 Frames, Riegel und
    //    Lua-Script-Zustand liefen so lange auseinander, und die Zeile
    //    "Reset kam nicht durch" feuerte nach JEDEM Umschalten;
    //  * schrieb der Rueckweg FALSE auf die Unverwundbarkeit, waehrend das
    //    Original nach dem Reset den von uns selbst gesetzten Zustand neu merkt
    //    (true/true/true) und beim Verlassen TRUE zurueckschreibt.
    m_reset_laeuft = false;
    m_reset_t = -99.0;
    m_rueckweg_t = -99.0;

    m_invinc_was.reset();
    m_hp_was_valid = false;
    m_hp_was_invincible = false;
    m_hp_was_immortal = false;
    m_hp_was_nodamage = false;
    m_invinc_logged = false;

    // Die Kamera-Flanke ist in Lua ebenfalls eine Local (Z.235).
    m_gimmick_gesehen = false;
    m_phase_b = false;
}

// ============================================================================
// Kann der Fork das ueberhaupt?
// ============================================================================

bool RE4VRObjects::faehig() {
    if (!m_kann_geprueft) {
        m_kann_geprueft = true;
        m_kann = (ScriptRunner::get() != nullptr);

        if (!m_kann && !m_gemeldet) {
            m_gemeldet = true;
        }
    }

    return m_kann;
}

// ============================================================================
// Lage des Spielers
// ============================================================================

::REManagedObject* RE4VRObjects::player_ctx() {
    auto* cm = re4vr::character_manager();

    if (cm == nullptr) {
        return nullptr;
    }

    return re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef");
}

bool RE4VRObjects::lage(std::optional<int32_t>& stage, std::optional<glm::vec3>& pos,
                        bool& am_gimmick) {
    stage.reset();
    pos.reset();
    am_gimmick = false;

    auto* ctx = player_ctx();

    if (ctx == nullptr) {
        return false;   // Lua: return nil
    }

    {
        int32_t s = 0;

        if (re4vr::try_call<int32_t>(ctx, "get_CurrentStageID", s)) {
            stage = s;
        }
    }

    // [KOSTEN] Ausserhalb unserer Stages ist hier Schluss: das sind im ganzen
    // restlichen Spiel zwei Aufrufe pro Frame (Singleton + StageID) statt
    // sechs. Body, Transform, Position und das State-Bit werden nur dort
    // geholt, wo es ueberhaupt eine Zone gibt.
    if (!stage.has_value() || !is_our_stage(*stage)) {
        return true;
    }

    // ParentGimmick = Bit 53 in chainsaw.PlayerDefine.State. Das Original
    // rechnet mit Modulo, weil Lua-Zahlen keine 64-Bit-Bitoperationen koennen;
    // nativ ist der Bit-Test dasselbe Ergebnis. Das Bit steht vom Aufnehmen bis
    // unten und traegt die Abschaltung ueber die ganze Fahrt.
    {
        std::optional<int64_t> st{};
        int64_t direct = 0;

        if (re4vr::try_call<int64_t>(ctx, "get_State", direct)) {
            st = direct;
        } else {
            // Container ist ein Managed Object -> false.
            st = num_or_enum_field(ctx, utility::re_managed_object::get_type_definition(ctx),
                                   "State", false);
        }

        if (st.has_value()) {
            const uint64_t v = static_cast<uint64_t>(*st);
            am_gimmick = ((v % 18014398509481984ull) >= 9007199254740992ull);
        }
    }

    auto* body = re4vr::call_safe<::REManagedObject*>(ctx, "get_BodyGameObject");
    auto* tf = (body != nullptr) ? re4vr::call_safe<::REManagedObject*>(body, "get_Transform")
                                 : nullptr;

    if (tf != nullptr) {
        glm::vec3 p{};

        if (get_vec3(tf, "get_Position", p)) {
            pos = p;
        }
    }

    return true;
}

bool RE4VRObjects::in_zone(int32_t stage, const glm::vec3& pos, const char*& name) const {
    for (const auto& z : ZONEN) {
        if (z.stage != stage) {
            continue;
        }

        const float dx = pos.x - z.x;
        const float dy = pos.y - z.y;
        const float dz = pos.z - z.z;

        if ((dx * dx + dy * dy + dz * dz) <= (z.r * z.r)) {
            name = z.name;
            return true;
        }
    }

    return false;
}

// ============================================================================
// [PHASE B] -- wann duerfen Adas Meshes verschwinden?
//
// Die Meshes blendet materials aus, sobald __re4_stillzone_hide_now steht.
// Gesetzt hat das Flag frueher motion -- und motion ist waehrend der Fahrt
// abgeschaltet, also setzt es niemand mehr und der Koerper bliebe sichtbar.
// Deshalb wanderte die Erkennung hierher, 1:1 aus motions STILLZONE-Tick.
//
// Warum ueberhaupt eine Phase B und nicht sofort beim Betreten: beim Einsteigen
// soll man sich noch sehen. Erst wenn die Einstiegs-Kamerafahrt durch ist --
// CamState wechselt von Gimmick auf BattleNormal -- verschwindet der Koerper.
// ============================================================================

bool RE4VRObjects::camints() {
    if (m_cam_ints_ok) {
        return true;
    }

    auto* td = sdk::find_type_definition(game_namespace("CameraDefine.PlayerCameraState"));

    if (td == nullptr) {
        return false;
    }

    bool have_g = false;
    bool have_b = false;

    for (auto* f : td->get_fields()) {
        if (f == nullptr || !f->is_static()) {
            continue;
        }

        const auto nm = f->get_name();

        if (nm != std::string_view{"Gimmick"} && nm != std::string_view{"BattleNormal"}) {
            continue;
        }

        // Statische Enum-Literale: get_data_raw liefert die ADRESSE der
        // Ablage, also dereferenzieren.
        auto* raw = f->get_data_raw(nullptr, false);

        if (raw == nullptr) {
            continue;
        }

        const int32_t v = *reinterpret_cast<int32_t*>(raw);

        if (nm == std::string_view{"Gimmick"}) {
            m_cam_gimmick = v;
            have_g = true;
        } else {
            m_cam_battle_normal = v;
            have_b = true;
        }
    }

    if (!have_g || !have_b) {
        return false;
    }

    m_cam_ints_ok = true;
    return true;
}

std::optional<int32_t> RE4VRObjects::camstate() {
    auto* csys = re4vr::camera_system_singleton();

    if (csys == nullptr) {
        return std::nullopt;
    }

    auto* main = re4vr::call_safe<::REManagedObject*>(csys, "get_MainCameraController");
    auto* busy = (main != nullptr)
                     ? re4vr::call_safe<::REManagedObject*>(main, "get_BusyCameraController")
                     : nullptr;

    if (busy == nullptr) {
        return std::nullopt;
    }

    // _CurrentStateParam ist ein Feld, dann darin <State>k__BackingField.
    // [M1] Das Feld kann ein WERT- oder ein Referenztyp sein --
    // RE4VRFirstPerson behandelt dasselbe Feld beidseitig, hier derselbe Weg.
    // Wer es blind als Zeiger liest, nimmt bei einem Werttyp die ersten acht
    // Bytes der Struct als Adresse.
    auto def = utility::re_managed_object::get_type_definition(busy);

    if (def == nullptr) {
        return std::nullopt;
    }

    auto* f_sp = def->get_field("_CurrentStateParam");

    if (f_sp == nullptr) {
        return std::nullopt;
    }

    void* sp_ptr = nullptr;
    bool sp_is_value = false;

    try {
        auto* sp_type = f_sp->get_type();
        sp_is_value = sp_type != nullptr && sp_type->is_value_type();

        if (sp_is_value) {
            sp_ptr = f_sp->get_data_raw(busy, false);   // Container = Managed Object
        } else {
            sp_ptr = f_sp->get_data<::REManagedObject*>(busy);
        }
    } catch (...) {
        sp_ptr = nullptr;
    }

    if (sp_ptr == nullptr) {
        return std::nullopt;
    }

    sdk::RETypeDefinition* sp_def = nullptr;

    if (sp_is_value) {
        sp_def = f_sp->get_type();
    } else {
        sp_def = utility::re_managed_object::get_type_definition(
            reinterpret_cast<::REManagedObject*>(sp_ptr));
    }

    const auto v = num_or_enum_field(sp_ptr, sp_def, "<State>k__BackingField", sp_is_value);

    if (!v.has_value()) {
        return std::nullopt;
    }

    return static_cast<int32_t>(*v);
}

bool RE4VRObjects::phase_b_tick(bool drin, int32_t stage) {
    if (!drin) {
        m_gimmick_gesehen = false;
        m_phase_b = false;
        return false;
    }

    if (m_phase_b) {
        return true;
    }

    // ADA (60850) behaelt die Kamerafahrt: dort laeuft man in den Einstieg
    // hinein und soll sich dabei noch sehen. ALLE ANDEREN ZONEN (Leon): sofort
    // -- bei ihm gibt es diese CamState-Flanke ohnehin nicht, deshalb blieb
    // sein Koerper die ganze Fahrt sichtbar.
    if (!is_flanke_stage(stage)) {
        return true;
    }

    if (re4vr::lua_get_bool("__re4_gondel_camflanke_aus", false)) {
        m_phase_b = true;
        return true;
    }

    if (camints()) {
        const auto cam = camstate();

        if (cam.has_value()) {
            if (*cam == m_cam_gimmick) {
                m_gimmick_gesehen = true;
            }

            if (m_gimmick_gesehen && *cam == m_cam_battle_normal) {
                m_phase_b = true;
            }
        }
    }

    return m_phase_b;
}

// ============================================================================
// [UNVERWUNDBAR] -- 1:1 uebernommen aus re4_vr_motion.lua (dort ausgebaut).
//
// set_IsInvincible(true) allein REICHT NICHT: live gemessen 28.08., man
// verliert damit weiter Energie. Der Schaden wird am HitPoint verrechnet, und
// dort sitzen die eigentlichen Schalter -- get_Invincible/get_Immortal/
// get_NoDamage werden in reload3.lua genau so abgefragt, bevor Schaden gebucht
// wird. Der HitPoint wird JEDEN Frame frisch geholt: nach Save-Load/Body-
// Wechsel zeigt ein gemerkter Handle auf eine Leiche. Alle drei Flags werden
// gesetzt, weil ungemessen ist, welches die Engine hier auswertet. Das
// verhindert nur den SCHADEN, nicht den Treffer und nicht den Rueckstoss.
// ============================================================================

void RE4VRObjects::invinc(bool an) {
    if (re4vr::lua_get_bool("__re4_gondel_invinc_aus", false)) {
        an = false;
    }

    // [KOSTEN] Aus + nichts gesetzt = NICHTS TUN. Ohne diesen Riegel lief der
    // else-Zweig jeden Frame im ganzen Spiel und schrieb drei Setter auf den
    // HitPoint -- das kostet nicht nur, es wuerde auch die echten i-Frames des
    // Spiels ueberschreiben.
    if (!an && !m_invinc_was.has_value() && !m_hp_was_valid && !m_invinc_logged) {
        return;
    }

    auto* ctx = player_ctx();

    if (ctx == nullptr) {
        return;
    }

    auto* hp = re4vr::call_safe<::REManagedObject*>(ctx, "get_HitPoint");

    if (an) {
        if (!m_invinc_was.has_value()) {
            bool v = false;
            re4vr::try_call<bool>(ctx, "get_IsInvincible", v);
            m_invinc_was = v;
        }

        if (hp != nullptr && !m_hp_was_valid) {
            bool a = false;
            bool b = false;
            bool c = false;
            re4vr::try_call<bool>(hp, "get_Invincible", a);
            re4vr::try_call<bool>(hp, "get_Immortal", b);
            re4vr::try_call<bool>(hp, "get_NoDamage", c);
            m_hp_was_invincible = a;
            m_hp_was_immortal = b;
            m_hp_was_nodamage = c;
            m_hp_was_valid = true;
        }

        // Jeden Frame nachdruecken: das Spiel setzt diese Flags selbst
        // (i-Frames, Reaktionen).
        re4vr::call_safe<void*>(ctx, "set_IsInvincible", true);

        if (hp != nullptr) {
            re4vr::call_safe<void*>(hp, "set_Invincible", true);
            re4vr::call_safe<void*>(hp, "set_Immortal", true);
            re4vr::call_safe<void*>(hp, "set_NoDamage", true);
        }

        // [KEIN LOG IM FORK 16.09.2026] Der Merker bleibt: er steht auch in einer
        // echten Bedingung weiter oben. Nur Getter und Logzeile sind raus.
        if (!m_invinc_logged) {
            m_invinc_logged = true;
        }
    } else {
        if (m_invinc_was.has_value()) {
            re4vr::call_safe<void*>(ctx, "set_IsInvincible", *m_invinc_was);
            m_invinc_was.reset();
        }

        if (hp != nullptr) {
            // Ohne gemerkten Vorzustand: Normalzustand, alles false.
            const bool a = m_hp_was_valid ? m_hp_was_invincible : false;
            const bool b = m_hp_was_valid ? m_hp_was_immortal : false;
            const bool c = m_hp_was_valid ? m_hp_was_nodamage : false;

            re4vr::call_safe<void*>(hp, "set_Invincible", a);
            re4vr::call_safe<void*>(hp, "set_Immortal", b);
            re4vr::call_safe<void*>(hp, "set_NoDamage", c);

        }

        m_hp_was_valid = false;
        m_invinc_logged = false;
    }
}

// ============================================================================
// Haupt-Tick. Jeden Frame, weil die Zone rund eine halbe Sekunde vor dem
// Aufnehmen greifen muss -- ein Takt von 0,5 s waere schon zu spaet.
// ============================================================================

void RE4VRObjects::on_frame() {
    re4vr::trace("RE4VRObjects", "on_frame");
    if (re4vr::lua_get_bool("__re4_objects_aus", false)) {
        // Not-Aus: auch der native Riegel muss dann alles freigeben.
        re4vr::set_mods_gated(false);
        return;
    }

    // [KEINE SACKGASSE] Normalerweise raeumt das Neuladen die Sperre weg.
    // Bleibt der Reset aus (falsche DLL, Reset unterdrueckt), haenge die Datei
    // sonst fuer immer -- deshalb ein Verfall.
    if (m_reset_laeuft && (now_clock() - m_reset_t) > RESET_TIMEOUT) {
        m_reset_laeuft = false;
    }

    std::optional<int32_t> stage{};
    std::optional<glm::vec3> pos{};
    bool am_gimmick = false;
    lage(stage, pos, am_gimmick);

    // Ausserhalb unserer Stages: Unverwundbarkeit und Mesh-Flags zurueckbauen
    // -- UND, ganz wichtig, den Rueckweg offenhalten.
    //
    // [RUECKWEG 03.09.2026] Hier stand vorher ein blankes return. Folge: laedt
    // man aus einer Zone heraus ein Savegame woanders hin, sieht der Tick eine
    // fremde Stage, steigt sofort aus -- und niemand schaltet die Scripte je
    // wieder ein. Die Ein/Aus-Liste lebt im ScriptRunner und ueberlebt jeden
    // Savegame-Load, das Spiel raeumt sie NICHT auf.
    // Gedrosselt auf 1x/s: is_script_enabled hat im restlichen Spiel nichts
    // jeden Frame zu suchen.
    if (stage.has_value() && !is_our_stage(*stage)) {
        re4vr::set_mods_gated(false);
        invinc(false);

        if (re4vr::lua_get_bool("__re4_stillzone_hide_now", false)) {
            re4vr::lua_set_bool("__re4_stillzone_hide_now", false);
        }

        if (re4vr::lua_get_bool("__re4_gondel_hide_now", false)) {
            re4vr::lua_set_bool("__re4_gondel_hide_now", false);
        }

        const double now = now_clock();

        if ((now - m_rueckweg_t) >= 1.0 && faehig() && !m_reset_laeuft) {
            m_rueckweg_t = now;

            auto runner = ScriptRunner::get();

            if (runner != nullptr && runner->any_known_script_disabled()) {
                runner->set_all_scripts_enabled(true);
                m_reset_laeuft = true;
                m_reset_t = now;
                runner->request_reset_scripts();
            }
        }

        return;
    }

    // Kein lesbarer Zustand (Ladephase, Szenenwechsel) heisst NICHT "Zone
    // verlassen". Ohne diesen Riegel wuerde ein einziger blinder Frame mitten
    // in der Fahrt alle Scripte wieder einschalten -- und genau das
    // Wiedereinschalten friert die Kabine. Der Riegel gilt auch fuer den
    // nativen Gate: hier wird er BEWUSST nicht angefasst.
    if (!stage.has_value() || !pos.has_value()) {
        return;
    }

    const char* name = nullptr;
    const bool drin = in_zone(*stage, *pos, name);

    // Zwei Zuender, beide nur innerhalb von STAGES: die Ortszone, und das
    // ParentGimmick-Bit haelt ueber die ganze Fahrt.
    // Der fruehere Killswitch-Nachzuender ist raus -- er kam ohnehin zu spaet,
    // und seine Hilfsfunktion wurde beim Ausbau versehentlich mit entfernt,
    // wodurch dieser Tick jeden Frame an einem Nil-Call starb.
    const bool soll_aus = is_our_stage(*stage) && (drin || am_gimmick);

    // [SCRIPTGATE] Der native Gegenpart zum Script-Umschalten. Anders als das
    // braucht er keinen Reset und ist billig -- deshalb jeden Frame gesetzt und
    // nicht nur auf der Flanke.
    re4vr::set_mods_gated(soll_aus);

    // Unverwundbarkeit: NUR fuer Adas Gondelfahrt (Stage 60850), sonst
    // nirgends. Sie stammt aus deren Sonderfall -- man wird waehrend der Fahrt
    // angegriffen und kann sich mit abgeschalteten Scripten nicht wehren. An
    // Leons Gondel hat sie nichts verloren. Haengt am Zustand, nicht am
    // Umschalten, und wird jeden Frame nachgedrueckt.
    invinc(soll_aus && *stage == 60850);

    // Meshes ausblenden -- ZWEI GETRENNTE FLAGS, und das mit Absicht:
    //   Ada  -> __re4_stillzone_hide_now : an diesem Flag haengt in binding.lua
    //           (~1495) zusaetzlich die Eingabesperre ihrer Fahrt (rechter
    //           Stick + Buttons aus). Das gehoert zu Ada und bleibt so.
    //   Leon -> __re4_gondel_hide_now    : eigenes Flag, das NUR materials
    //           liest. Seine Fahrt bleibt damit voll bedienbar.
    const bool hide = phase_b_tick(soll_aus, *stage);
    re4vr::lua_set_bool("__re4_stillzone_hide_now", hide && (*stage == 60850));
    re4vr::lua_set_bool("__re4_gondel_hide_now",
                        hide && (*stage != 60850) && is_hide_stage(*stage));

    // Ist-Zustand aus dem Fork lesen, nicht aus einem Member: der ueberlebt den
    // Reset zwar, aber die Ein/Aus-Liste ist die Wahrheit.
    const bool ok_f = faehig();
    auto runner = ScriptRunner::get();

    if (!ok_f || runner == nullptr || m_reset_laeuft) {
        return;
    }

    // [ZUSTANDSPROBE 04.09.2026] Frueher: !is_script_enabled(PROBE_DATEI) mit
    // PROBE_DATEI = "re4_vr_motion.lua". Diese Stellvertreter-Datei wandert mit
    // dem motion-Port nach autorun\c++\ und existiert dann nicht mehr --
    // is_script_enabled haette fuer Unbekanntes dauerhaft true geliefert, der
    // Riegel haette sich in der Zone alle 3 s selbst neu ausgeloest
    // (Endlos-Reset) und der Rueckweg oben waere nie wieder gefeuert.
    // Die Frage gilt jetzt der Ein/Aus-Liste selbst.
    const bool ist_aus = runner->any_known_script_disabled();

    if (soll_aus == ist_aus) {
        return;
    }

    if (soll_aus) {
        runner->set_all_scripts_enabled(false);

        for (const char* n : BLEIBT) {
            runner->set_script_enabled(n, true);
        }
    } else {
        runner->set_all_scripts_enabled(true);
    }

    m_reset_laeuft = true;
    m_reset_t = now_clock();
    runner->request_reset_scripts();
}

#endif // RE4
