// ============================================================================
// RE4VRJiggle -- Portierung von re4_vr_body_physics.lua (19.09.2026)
// Beschreibung: s. RE4VRJiggle.hpp
// ============================================================================

#if defined(RE4)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>

#include <sdk/RETypeDB.hpp>
#include <sdk/RETypes.hpp>
#include <sdk/REContext.hpp>
#include <utility/String.hpp>

#include "RE4VR.hpp"
#include "RE4VRJiggle.hpp"
#include "RE4VRChoke.hpp"
#include "RE4VRHolster.hpp"

#undef min
#undef max

namespace {

double clock_now() {
    return static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// Methoden-Cache: die Ketten-Schleife ruft pro Frame hunderte Getter -- die
// TDB-Suche nach dem Namen nur einmal je Typ.
sdk::REMethodDefinition* meth(::REManagedObject* obj, std::string_view name) {
    if (!re4vr::obj_ok(obj)) {
        return nullptr;
    }

    auto* td = utility::re_managed_object::get_type_definition(obj);

    if (td == nullptr) {
        return nullptr;
    }

    static std::unordered_map<std::string, sdk::REMethodDefinition*> cache{};
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(td));
    key += '|';
    key += name;

    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }

    auto* m = td->get_method(name);
    cache.emplace(std::move(key), m);

    return m;
}

bool clear_pending(sdk::VMContext* context, bool ok) {
    if (context != nullptr && context->unkPtr != nullptr && context->unkPtr->unkPtr != nullptr) {
        context->unkPtr->unkPtr = nullptr;
        return false;
    }

    return ok;
}

// ValueType-Getter ueber einen 16-Byte-ausgerichteten sret-Puffer.
bool get_vec3(::REManagedObject* obj, std::string_view name, glm::vec3& out) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{};
    bool ok = false;

    try {
        m->call_safe<glm::vec4*>(&buf, context, obj);
        ok = true;
    } catch (...) {
        ok = false;
    }

    if (clear_pending(context, ok)) {
        out = glm::vec3{buf.x, buf.y, buf.z};
        return true;
    }

    return false;
}

// Getter mit einem int-Argument (getNodePosition(i)).
bool get_vec3_i(::REManagedObject* obj, std::string_view name, int32_t i, glm::vec3& out) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{};
    bool ok = false;

    try {
        m->call_safe<glm::vec4*>(&buf, context, obj, i);
        ok = true;
    } catch (...) {
        ok = false;
    }

    if (clear_pending(context, ok)) {
        out = glm::vec3{buf.x, buf.y, buf.z};
        return true;
    }

    return false;
}

bool get_quat(::REManagedObject* obj, std::string_view name, glm::quat& out) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::quat buf{1.0f, 0.0f, 0.0f, 0.0f};
    bool ok = false;

    try {
        m->call_safe<glm::quat*>(&buf, context, obj);
        ok = true;
    } catch (...) {
        ok = false;
    }

    if (clear_pending(context, ok)) {
        out = buf;
        return true;
    }

    return false;
}

bool set_quat(::REManagedObject* obj, std::string_view name, const glm::quat& q) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::quat buf = q;
    bool ok = false;

    try {
        m->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// Luas Vector3f.new an eine via.vec3-Signatur.
bool set_vec3(::REManagedObject* obj, std::string_view name, const glm::vec3& v) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, 1.0f};
    bool ok = false;

    try {
        m->call_safe<void*>(context, obj, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

// impulsePushNode(int, via.vec3)
bool call_i_vec3(::REManagedObject* obj, std::string_view name, int32_t i, const glm::vec3& v) {
    auto* m = meth(obj, name);

    if (m == nullptr) {
        return false;
    }

    auto context = sdk::get_thread_context();
    __declspec(align(16)) glm::vec4 buf{v.x, v.y, v.z, 1.0f};
    bool ok = false;

    try {
        m->call_safe<void*>(context, obj, i, &buf);
        ok = true;
    } catch (...) {
        ok = false;
    }

    return clear_pending(context, ok);
}

std::string name_of(::REManagedObject* go) {
    auto* s = re4vr::call_safe<::REManagedObject*>(go, "get_Name");

    if (s == nullptr) {
        return {};
    }

    try {
        return utility::re_string::get_string(reinterpret_cast<::SystemString*>(s));
    } catch (...) {
        return {};
    }
}

// getJointByName ignoriert Gross/Klein -> Namen EXAKT vergleichen.
::REManagedObject* joint_exact(::REManagedObject* tf, const char* name) {
    if (tf == nullptr) {
        return nullptr;
    }

    auto* str = sdk::VM::create_managed_string(utility::widen(std::string{name}));

    if (str == nullptr) {
        return nullptr;
    }

    auto* j = re4vr::call_safe<::REManagedObject*>(tf, "getJointByName", str);

    return (j != nullptr && name_of(j) == name) ? j : nullptr;
}

glm::vec3 clamp_len(const glm::vec3& v, float max_len) {
    const float l = glm::length(v);
    return (l > max_len && l > 1e-8f) ? v * (max_len / l) : v;
}

// Kugel-Hand gegen Punkt: Feder (Eindringtiefe) + Daempfung (Annaeherung) +
// Reibung (Tangentialanteil). Leer = kein Kontakt. Wie im Lua-Original.
struct Contact {
    glm::vec3 impulse{};
    float pen{0.0f};
};

std::optional<Contact> soft_contact(const glm::vec3& hand, const glm::vec3& point,
                                    const glm::vec3& vel, float radius, float spring,
                                    float damping, float friction) {
    const glm::vec3 delta = point - hand;
    const float d = glm::length(delta);

    if (d >= radius || d < 1e-5f) {
        return std::nullopt;
    }

    const glm::vec3 n = delta / d;
    const float pen = radius - d;
    const float approach = glm::dot(vel, n);
    const glm::vec3 tan = vel - n * approach;

    Contact c{};
    c.impulse = n * (spring * pen) + n * (damping * std::max(0.0f, approach)) + tan * friction;
    c.pen = pen;

    return c;
}

bool quat_close(const glm::quat& a, const glm::quat& b) {
    return std::abs(glm::dot(a, b)) > 0.999999f;
}

constexpr const char* SOFT_JOINT_NAMES[] = {
    "L_Bust", "R_Bust", "L_Breast", "R_Breast", "Bust_L", "Bust_R",
    "L_Pectoral", "R_Pectoral", "Pectoral_L", "Pectoral_R",
    "Spine_2", "Spine_1", "Spine", "Chest", "C_Spine2", "C_Spine1",
    "Hip", "stomach",
};

} // namespace

std::shared_ptr<RE4VRJiggle>& RE4VRJiggle::get() {
    static auto inst = std::make_shared<RE4VRJiggle>();
    return inst;
}

// ============================================================================
// Haende: joint_pos -> world, sonst die rohe Controller-Position (wie Lua).
// ============================================================================
void RE4VRJiggle::hands(std::optional<glm::vec3>& lh, std::optional<glm::vec3>& rh) {
    lh = re4vr::lua_get_vec3_any({"__vr_lh_joint_pos", "__vr_lh_world"});
    rh = re4vr::lua_get_vec3_any({"__vr_rh_joint_pos", "__vr_rh_world"});

    if (!lh.has_value() && !rh.has_value()) {
        lh = re4vr::lua_get_vec3("__vr_lh_ctrl_raw");
        rh = re4vr::lua_get_vec3("__vr_rh_ctrl_raw");
    }
}

// ============================================================================
// Ashley finden
// ============================================================================
::REManagedObject* RE4VRJiggle::resolve_body() {
    auto* cm = sdk::get_managed_singleton<::REManagedObject>("chainsaw.CharacterManager");

    if (cm == nullptr) {
        return nullptr;
    }

    // Begleiterin (ch2a1z0_body)
    if (auto* partner = re4vr::call_safe<::REManagedObject*>(cm, "getPartnerContextRef")) {
        bool valid = false;

        if (re4vr::try_call<bool>(partner, "get_Valid", valid) && valid) {
            auto* body = re4vr::call_safe<::REManagedObject*>(partner, "get_BodyGameObject");
            const auto n = name_of(body);

            // Keine beliebigen Partner-Koerper waehrend des Spawns (Lua: Crash-Risiko).
            if (body != nullptr && (n.find("ch2a1") != std::string::npos
                                    || n.find("ch0a1") != std::string::npos)) {
                return body;
            }
        }
    }

    // Ashley selbst gespielt (ch0a1z0_body)
    if (auto* player = re4vr::call_safe<::REManagedObject*>(cm, "getPlayerContextRef")) {
        auto* body = re4vr::call_safe<::REManagedObject*>(player, "get_BodyGameObject");

        if (body != nullptr && name_of(body) == "ch0a1z0_body") {
            return body;
        }
    }

    return nullptr;
}

void RE4VRJiggle::drop_all() {
    m_body = nullptr;
    m_body_addr = 0;
    m_cloth_mgr = nullptr;
    m_gpu_cloths.clear();
    m_chains.clear();
    m_soft.clear();
    m_head_j = nullptr;
    m_neck_j = nullptr;
    m_jw_head = JointWrite{};
    m_jw_neck = JointWrite{};
    m_th = glm::vec3{0.0f};
    m_om = glm::vec3{0.0f};
    m_head_prev[0].reset();
    m_head_prev[1].reset();
    m_head_inzone[0] = m_head_inzone[1] = false;
    m_prev_lh.reset();
    m_prev_rh.reset();
    m_wind_zeroed = true;
}

// Jeden Frame: nur der Adressvergleich. Der Baum wird nur bei einem NEUEN
// Koerper abgelaufen -- nach Save-Load/Tod sind alle alten Zeiger Leichen.
void RE4VRJiggle::refresh_body() {
    auto* body = resolve_body();
    const auto a = reinterpret_cast<uintptr_t>(body);

    if (a == m_body_addr) {
        return;
    }

    drop_all();

    if (body != nullptr) {
        discover(body);
    }
}

void RE4VRJiggle::discover(::REManagedObject* body) {
    m_body = body;
    m_body_addr = reinterpret_cast<uintptr_t>(body);

    // Kinder-GOs breitenweise (wie walk_collect, hoechstens 128).
    std::deque<::REManagedObject*> queue{body};
    std::unordered_set<::REManagedObject*> seen{};
    int visited = 0;

    while (!queue.empty() && visited < 128) {
        auto* go = queue.front();
        queue.pop_front();

        if (go == nullptr || !seen.insert(go).second) {
            continue;
        }

        ++visited;

        if (m_cloth_mgr == nullptr) {
            m_cloth_mgr = re4vr::get_component(go, "chainsaw.GPUClothCharacter");
        }

        if (auto* gc = re4vr::get_component(go, "via.dynamics.GpuCloth")) {
            m_gpu_cloths.push_back(gc);
        }

        if (auto* ch = re4vr::get_component(go, "via.motion.Chain")) {
            m_chains.push_back(ch);
        }

        auto* tf = re4vr::call_safe<::REManagedObject*>(go, "get_Transform");
        auto* child = re4vr::call_safe<::REManagedObject*>(tf, "get_Child");

        for (int guard = 0; child != nullptr && guard < 64; ++guard) {
            if (auto* cgo = re4vr::call_safe<::REManagedObject*>(child, "get_GameObject")) {
                queue.push_back(cgo);
            }

            child = re4vr::call_safe<::REManagedObject*>(child, "get_Next");
        }
    }

    auto* btf = re4vr::call_safe<::REManagedObject*>(body, "get_Transform");

    for (const char* jn : SOFT_JOINT_NAMES) {
        if (auto* j = joint_exact(btf, jn)) {
            m_soft.push_back(SoftJoint{jn, j});
        }
    }

    m_head_j = joint_exact(btf, "Head");
    m_neck_j = joint_exact(btf, "Neck_1");

    if (m_neck_j == nullptr) {
        m_neck_j = joint_exact(btf, "Neck_0");
    }
}

// ============================================================================
// Stoff / Ketten / Wind -- LateUpdateBehavior (nach GPUClothCharacter.update)
// ============================================================================
void RE4VRJiggle::zero_wind() {
    if (m_wind_zeroed) {
        return;
    }

    for (auto* gc : m_gpu_cloths) {
        set_vec3(gc, "set_DirectWindForce", glm::vec3{0.0f});
        set_vec3(gc, "set_DirectWindVelocity", glm::vec3{0.0f});
    }

    for (auto* ch : m_chains) {
        re4vr::call_safe<void*>(ch, "set_ExternalWindPower", 0.0f);
    }

    m_wind_zeroed = true;
}

void RE4VRJiggle::tick_body() {
    if (m_body == nullptr) {
        return;
    }

    std::optional<glm::vec3> lh{}, rh{};
    hands(lh, rh);

    // Naechste Hand zum naechsten weichen Ziel.
    std::vector<glm::vec3> targets{};

    for (const auto& sj : m_soft) {
        glm::vec3 p{};

        if (get_vec3(sj.joint, "get_Position", p)) {
            targets.push_back(p);
        }
    }

    if (targets.empty()) {
        auto* tf = re4vr::call_safe<::REManagedObject*>(m_body, "get_Transform");
        glm::vec3 p{};

        if (get_vec3(tf, "get_Position", p)) {
            targets.push_back(p);
        }
    }

    float best_d = 1e9f;
    std::optional<glm::vec3> best_h{}, best_t{};
    bool best_is_r = false;

    for (int hi = 0; hi < 2; ++hi) {
        const auto& h = hi == 0 ? lh : rh;

        if (!h.has_value()) {
            continue;
        }

        for (const auto& t : targets) {
            const float d = glm::length(*h - t);

            if (d < best_d) {
                best_d = d;
                best_h = *h;
                best_t = t;
                best_is_r = hi == 1;
            }
        }
    }

    m_dist = best_h.has_value() ? best_d : -1.0f;

    // Haende weit weg: nichts ansteuern (die Ketten-Schleife ist das Teure).
    // Nur mit echten weichen Joints -- die Wurzel sitzt an ihren Fuessen.
    if (!m_soft.empty() && (m_dist < 0.0f || m_dist > cfg.active_range)) {
        zero_wind();
        m_contact_cloth = m_contact_chain = false;
        m_prev_lh = lh;
        m_prev_rh = rh;
        return;
    }

    if (best_h.has_value()) {
        if (cfg.drive_cloth) {
            drive_cloth(*best_h);
        }

        if (cfg.drive_chain) {
            drive_chain(lh, rh);
        }

        if (cfg.drive_wind) {
            const auto& prev = best_is_r ? m_prev_rh : m_prev_lh;
            const glm::vec3 vel = prev.has_value() ? *best_h - *prev : glm::vec3{0.0f};
            drive_wind(*best_h, *best_t, vel);
        }
    }

    m_prev_lh = lh;
    m_prev_rh = rh;
}

// _HandCapsule: kurze Kapsel um die naehere Hand. Geschrieben ueber die
// Feld-Offsets aus der TDB (via.Capsule p0/p1/r) -- fehlen sie, bleibt nur
// der Radius-Setter.
void RE4VRJiggle::drive_cloth(const glm::vec3& hand) {
    auto* mgr = m_cloth_mgr;

    if (mgr == nullptr) {
        return;
    }

    auto* td = utility::re_managed_object::get_type_definition(mgr);
    auto* f_cap = td != nullptr ? td->get_field("_HandCapsule") : nullptr;
    auto* ctd = f_cap != nullptr ? f_cap->get_type() : nullptr;

    if (f_cap != nullptr && ctd != nullptr) {
        auto* f0 = ctd->get_field("p0");
        auto* f1 = ctd->get_field("p1");
        auto* fr = ctd->get_field("r");
        // Container ist das Managed Object -> false (s. get_data_raw-Falle).
        auto* cap = f_cap->get_data_raw(mgr, false);

        if (cap != nullptr && f0 != nullptr && f1 != nullptr && fr != nullptr) {
            const glm::vec3 half{0.0f, cfg.hand_capsule_len * 0.5f, 0.0f};
            const glm::vec3 p0 = hand - half;
            const glm::vec3 p1 = hand + half;
            // Container ist jetzt die eingebettete Struct -> true.
            auto* a0 = reinterpret_cast<float*>(f0->get_data_raw(cap, true));
            auto* a1 = reinterpret_cast<float*>(f1->get_data_raw(cap, true));
            auto* ar = reinterpret_cast<float*>(fr->get_data_raw(cap, true));

            if (a0 != nullptr && a1 != nullptr && ar != nullptr) {
                a0[0] = p0.x; a0[1] = p0.y; a0[2] = p0.z;
                a1[0] = p1.x; a1[1] = p1.y; a1[2] = p1.z;
                *ar = cfg.cloth_radius;
            }
        }
    }

    re4vr::call_safe<void*>(mgr, "set_HandCapsuleRad", cfg.cloth_radius);
    m_contact_cloth = m_dist >= 0.0f && m_dist < (cfg.cloth_radius + cfg.poke_range);
}

void RE4VRJiggle::drive_wind(const glm::vec3& hand, const glm::vec3& soft, const glm::vec3& vel) {
    if (m_gpu_cloths.empty()) {
        return;
    }

    const auto c = soft_contact(hand, soft, vel, cfg.poke_range, cfg.contact_spring,
                                cfg.contact_damping, cfg.contact_friction);
    const float dist = glm::length(soft - hand);

    for (auto* gc : m_gpu_cloths) {
        re4vr::call_safe<void*>(gc, "set_LocalWind", true);

        if (c.has_value()) {
            // (applyWorldOffset bewusst NICHT: verschiebt die ganze Simulation.)
            const glm::vec3 force = clamp_len(c->impulse * (cfg.wind_strength * 8.0f), cfg.wind_strength);
            set_vec3(gc, "set_DirectWindForce", force);
            set_vec3(gc, "set_DirectWindVelocity", force * 0.5f);
            m_wind_zeroed = false;
        } else if (dist < cfg.wind_range) {
            const glm::vec3 into = glm::normalize(soft - hand);
            const glm::vec3 air = into * (cfg.wind_strength * 0.15f * (1.0f - dist / cfg.wind_range));
            set_vec3(gc, "set_DirectWindForce", air);
            set_vec3(gc, "set_DirectWindVelocity", air * 0.4f);
            m_wind_zeroed = false;
        } else {
            set_vec3(gc, "set_DirectWindForce", glm::vec3{0.0f});
            set_vec3(gc, "set_DirectWindVelocity", glm::vec3{0.0f});
        }
    }
}

void RE4VRJiggle::drive_chain(const std::optional<glm::vec3>& lh, const std::optional<glm::vec3>& rh) {
    m_contact_chain = false;

    const std::optional<glm::vec3> hs[2] = {lh, rh};
    const std::optional<glm::vec3> ps[2] = {m_prev_lh, m_prev_rh};

    for (auto* ch : m_chains) {
        int32_t gcount = 0;
        re4vr::try_call<int32_t>(ch, "getGroupCount", gcount);
        bool chain_hit = false;

        for (int32_t gi = 0; gi < std::min<int32_t>(gcount, 32); ++gi) {
            auto* grp = re4vr::call_safe<::REManagedObject*>(ch, "getGroup", gi);

            if (grp == nullptr) {
                continue;
            }

            int32_t ncount = 0;
            re4vr::try_call<int32_t>(grp, "get_NodeCount", ncount);
            bool group_hit = false;

            for (int32_t ni = 0; ni < std::min<int32_t>(ncount, 16); ++ni) {
                glm::vec3 npos{};

                if (!get_vec3_i(grp, "getNodePosition", ni, npos)) {
                    continue;
                }

                for (int hi = 0; hi < 2; ++hi) {
                    if (!hs[hi].has_value()) {
                        continue;
                    }

                    const glm::vec3 vel = ps[hi].has_value() ? *hs[hi] - *ps[hi] : glm::vec3{0.0f};
                    const auto c = soft_contact(*hs[hi], npos, vel, cfg.chain_radius, cfg.contact_spring,
                                                cfg.contact_damping, cfg.contact_friction);

                    if (c.has_value()
                        && call_i_vec3(grp, "impulsePushNode", ni, clamp_len(c->impulse, cfg.chain_impulse_max))) {
                        group_hit = chain_hit = true;
                    }
                }
            }

            if (group_hit) {
                re4vr::call_safe<void*>(grp, "set_WindCoef", 1.4f);
            }
        }

        if (chain_hit) {
            m_contact_chain = true;
            m_wind_zeroed = false;
            re4vr::call_safe<void*>(ch, "set_ExternalWindAddMode", true);
            re4vr::call_safe<void*>(ch, "set_ExternalWindPower", std::min(4.0f, cfg.wind_strength * 0.25f));
        } else {
            re4vr::call_safe<void*>(ch, "set_ExternalWindPower", 0.0f);
        }
    }
}

// ============================================================================
// Kopf -- Feder-Daempfer auf einem Neigungsvektor (Welt)
// ============================================================================
void RE4VRJiggle::head_step() {
    const double now = clock_now();
    float dt = m_head_last_t >= 0.0 ? static_cast<float>(now - m_head_last_t) : (1.0f / 90.0f);
    m_head_last_t = now;

    if (dt <= 0.0f || dt > 0.05f) {
        dt = 1.0f / 90.0f;
    }

    glm::vec3 target{0.0f};
    m_contact_head = false;

    // [EIGENE FORTBEWEGUNG RAUS 20.09.2026] Beim Losrennen wandert die Hand mit
    // dem Koerper durch die Welt. Ohne diese Korrektur galt das als Schlagtempo:
    // auf Ashley zulaufen und greifen -> Klatscher, obwohl die Hand stillhielt.
    // Gerechnet wird deshalb mit dem Tempo RELATIV zum Spielerkoerper.
    glm::vec3 v_player{0.0f};

    if (auto* btf = re4vr::fc::body_tf()) {
        glm::vec3 bp{};

        if (get_vec3(btf, "get_Position", bp)) {
            if (m_player_prev.has_value()) {
                v_player = (bp - *m_player_prev) / dt;
            }

            m_player_prev = bp;
        }
    }

    glm::vec3 hp{};

    if (m_head_j != nullptr && get_vec3(m_head_j, "get_Position", hp)) {
        glm::vec3 np{};
        const bool has_neck = m_neck_j != nullptr && get_vec3(m_neck_j, "get_Position", np);
        const glm::vec3 up = has_neck ? glm::normalize(hp - np) : glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::vec3 piv = has_neck ? np : hp;

        std::optional<glm::vec3> hs[2]{};
        hands(hs[0], hs[1]);

        // [GRIFF 20.09.2026] Die LINKE Hand haelt sie -- sie liegt dabei am
        // Hals und haette dauernd Kontakt (Klatscher beim Zupacken UND beim
        // Loslassen). Waehrend des Griffs zaehlt deshalb nur die RECHTE Hand;
        // schlagen kann man damit weiter, halten allein loest nichts aus.
        // [NACHLAUF] Noch 0,4 s nach dem Loslassen gesperrt: die haltende Hand
        // liegt dann noch am Hals und waere sonst ein "frischer" Treffer.
        if (choking()) {
            m_hold_until = clock_now() + 0.4;
        }

        const bool hold = clock_now() < m_hold_until;

        // Einmal pro Frame fragen, nicht je Hand -- die Pruefung geht ans Mesh.
        const bool bare = hands_bare();

        for (int hi = 0; hi < 2; ++hi) {
            bool in_zone = false;

            if (hold && hi == 0) {
                // als "schon dran" fuehren, damit das Loslassen kein Treffer ist
                m_head_inzone[hi] = true;
                m_head_prev[hi].reset();

                continue;
            }

            if (hs[hi].has_value()) {
                const glm::vec3 h = *hs[hi];
                const glm::vec3 delta = hp - h;   // Hand -> Kopf
                const float d = glm::length(delta);

                // [NUR MIT LEERER HAND 20.09.2026 -- Ansage] Mit Pistole oder
                // Messer in der Hand wird nicht geschlagen.
                if (d < cfg.head_radius && d > 1e-5f && bare) {
                    in_zone = true;
                    m_contact_head = true;

                    // Wegdruecken: 'up' um (up x n) drehen kippt den Kopf zu n,
                    // also weg von der Hand.
                    const glm::vec3 n = delta / d;
                    const glm::vec3 ax = glm::normalize(glm::cross(up, n));
                    target += ax * ((cfg.head_radius - d) * cfg.head_gain * cfg.head_sensitivity);

                    // Hand-Tempo + Richtung -> Drall um den Hals.
                    const glm::vec3 v = m_head_prev[hi].has_value()
                        ? ((h - *m_head_prev[hi]) / dt) - v_player
                        : glm::vec3{0.0f};
                    const glm::vec3 r = h - piv;
                    const float r2 = std::max(0.01f, glm::dot(r, r));
                    const glm::vec3 w = glm::cross(r, v) / r2 * cfg.head_sensitivity;

                    if (!m_head_inzone[hi]) {
                        // [PEAK AM SLAP 19.09.2026] Auftreffen: Tempo an der
                        // Slap-Schwelle = voller Ausschlag bis Head max,
                        // darunter anteilig, darueber gedeckelt. Die Richtung
                        // kommt aus dem Drall der Hand (sonst: weg von ihr).
                        const float speed = glm::length(v);
                        const float ratio = cfg.slap_min_speed > 1e-3f
                            ? std::min(1.0f, speed / cfg.slap_min_speed) : 1.0f;
                        const glm::vec3 dir = glm::length(w) > 1e-4f ? glm::normalize(w) : ax;
                        m_om += dir * (ratio * cfg.head_sensitivity * peak_spin());
                        on_slap(speed);
                    } else {
                        // Solange die Hand dran ist: mitgefuehrt.
                        const float f = std::min(1.0f, cfg.head_drag * dt);
                        m_om += (w - m_om) * f;
                    }
                }

                m_head_prev[hi] = h;
            } else {
                m_head_prev[hi].reset();
            }

            m_head_inzone[hi] = in_zone;
        }
    }

    const float maxa = glm::radians(cfg.head_max_deg);
    target = clamp_len(target, maxa);
    // Der Treffer darf so schnell sein, wie es fuer Head max noetig ist.
    m_om = clamp_len(m_om, std::max(cfg.head_max_spin, peak_spin() * std::max(1.0f, cfg.head_sensitivity)));

    const glm::vec3 acc = cfg.head_stiffness * (target - m_th) - cfg.head_damping * m_om;
    m_om += acc * dt;
    m_th += m_om * dt;

    const float l = glm::length(m_th);

    if (l > maxa) {
        m_th *= maxa / l;
        m_om *= 0.5f;   // Anschlag: Schwung geht verloren
    }

    m_head_angle = glm::degrees(std::min(l, maxa));
}

// Anfangs-Drehtempo, mit dem die gedaempfte Feder genau bis Head max
// ausschlaegt: Amplitude = w0/wn * exp(-z/sqrt(1-z^2) * atan2(sqrt(1-z^2), z)).
float RE4VRJiggle::peak_spin() const {
    const float k = std::max(1.0f, cfg.head_stiffness);
    const float wn = std::sqrt(k);
    const float z = cfg.head_damping / (2.0f * wn);
    float f = std::exp(-1.0f);   // kritisch/ueberdaempft: ~e^-1

    if (z < 0.999f) {
        const float s = std::sqrt(1.0f - z * z);
        f = std::exp(-z / s * std::atan2(s, z));
    }

    return glm::radians(cfg.head_max_deg) * wn / std::max(0.05f, f);
}

// ============================================================================
// Config -- re4_vr/re4_vr_jiggle.json
// ============================================================================
namespace {
constexpr const char* JIGGLE_JSON = "re4_vr/re4_vr_jiggle.json";
}

void RE4VRJiggle::cfg_ensure() {
    if (m_cfg_loaded) {
        return;
    }

    m_cfg_loaded = true;

    const auto j = re4vr::json_load(JIGGLE_JSON);

    if (j.is_object()) {
        const auto rb = [&](const char* k, bool& v) {
            if (j.contains(k) && j[k].is_boolean()) { v = j[k].get<bool>(); }
        };
        const auto rf = [&](const char* k, float& v) {
            if (j.contains(k) && j[k].is_number()) { v = j[k].get<float>(); }
        };

        rb("enabled", cfg.enabled);
        rb("drive_cloth", cfg.drive_cloth);
        rb("drive_chain", cfg.drive_chain);
        rb("drive_wind", cfg.drive_wind);
        rb("drive_head", cfg.drive_head);
        rb("sounds", cfg.sounds);
        rf("cloth_radius", cfg.cloth_radius);
        rf("chain_radius", cfg.chain_radius);
        rf("hand_capsule_len", cfg.hand_capsule_len);
        rf("contact_spring", cfg.contact_spring);
        rf("contact_damping", cfg.contact_damping);
        rf("contact_friction", cfg.contact_friction);
        rf("chain_impulse_max", cfg.chain_impulse_max);
        rf("wind_strength", cfg.wind_strength);
        rf("wind_range", cfg.wind_range);
        rf("poke_range", cfg.poke_range);
        rf("active_range", cfg.active_range);
        rf("head_radius", cfg.head_radius);
        rf("head_gain", cfg.head_gain);
        rf("head_max_deg", cfg.head_max_deg);
        rf("head_drag", cfg.head_drag);
        rf("head_max_spin", cfg.head_max_spin);
        rf("head_stiffness", cfg.head_stiffness);
        rf("head_damping", cfg.head_damping);
        rf("head_neck_share", cfg.head_neck_share);
        rf("head_sensitivity", cfg.head_sensitivity);
        rf("slap_min_speed", cfg.slap_min_speed);
        rf("reply_delay", cfg.reply_delay);
        rf("slap_gain_db", cfg.slap_gain_db);
    }

    // Immer komplett zurueckschreiben: so stehen ALLE Werte in der Datei,
    // auch die, die sie noch nicht kannte.
    cfg_save();
}

void RE4VRJiggle::cfg_save() {
    nlohmann::json j = nlohmann::json::object();

    j["enabled"] = cfg.enabled;
    j["drive_cloth"] = cfg.drive_cloth;
    j["drive_chain"] = cfg.drive_chain;
    j["drive_wind"] = cfg.drive_wind;
    j["drive_head"] = cfg.drive_head;
    j["sounds"] = cfg.sounds;
    j["cloth_radius"] = cfg.cloth_radius;
    j["chain_radius"] = cfg.chain_radius;
    j["hand_capsule_len"] = cfg.hand_capsule_len;
    j["contact_spring"] = cfg.contact_spring;
    j["contact_damping"] = cfg.contact_damping;
    j["contact_friction"] = cfg.contact_friction;
    j["chain_impulse_max"] = cfg.chain_impulse_max;
    j["wind_strength"] = cfg.wind_strength;
    j["wind_range"] = cfg.wind_range;
    j["poke_range"] = cfg.poke_range;
    j["active_range"] = cfg.active_range;
    j["head_radius"] = cfg.head_radius;
    j["head_gain"] = cfg.head_gain;
    j["head_max_deg"] = cfg.head_max_deg;
    j["head_drag"] = cfg.head_drag;
    j["head_max_spin"] = cfg.head_max_spin;
    j["head_stiffness"] = cfg.head_stiffness;
    j["head_damping"] = cfg.head_damping;
    j["head_neck_share"] = cfg.head_neck_share;
    j["head_sensitivity"] = cfg.head_sensitivity;
    j["slap_min_speed"] = cfg.slap_min_speed;
    j["reply_delay"] = cfg.reply_delay;
    j["slap_gain_db"] = cfg.slap_gain_db;

    re4vr::json_save(JIGGLE_JSON, j);
    m_cfg_dirty = false;
}

// ============================================================================
// Ton -- Klatscher sofort, Antwort aus dem Mittelfinger-Pool mit Verzoegerung
// ============================================================================
// Sind die Haende wirklich leer? Am Mesh gemessen (RE4VRHolster), nicht an der
// equippten Waffen-ID -- die meldet bei leeren Haenden die zuletzt gewaehlte.
bool RE4VRJiggle::hands_bare() {
    auto& ho = RE4VRHolster::get();

    return ho != nullptr && ho->hands_are_bare();
}

// Haelt der Spieler gerade jemanden im Wuergegriff? Direkt am Choke-Modul
// gefragt -- der Weg ueber das Lua-Global __re4_is_choking griff aus diesem
// Hook heraus nicht (Klatscher kam trotz Sperre).
bool RE4VRJiggle::choking() {
    auto& ch = RE4VRChoke::get();

    return ch != nullptr && ch->is_holding();
}

void RE4VRJiggle::on_slap(float speed) {
    if (!cfg.sounds || speed < cfg.slap_min_speed) {
        return;
    }

    const double now = clock_now();

    // Beide Haende im selben Moment = EIN Klaps.
    if (now - m_slap_last_t < 0.25) {
        return;
    }

    m_slap_last_t = now;

    int idx = std::rand() % re4vr::SLAP_WAV_COUNT;

    if (idx == m_slap_last_idx) {
        idx = (idx + 1) % re4vr::SLAP_WAV_COUNT;
    }

    m_slap_last_idx = idx;
    re4vr::play_slap_wav(idx, re4vr::taunt_wav_gain_db() + cfg.slap_gain_db);

    // [SHUFFLE 20.09.2026 -- Ansage] Nicht nach JEDEM Schlag ein Spruch,
    // sondern nur bei jedem 2. bis 3. -- so liegen Choke- und Slap-Sprueche
    // schoen versetzt.
    ++m_slap_count;

    if (m_slap_count < m_slap_next) {
        return;
    }

    m_slap_count = 0;
    m_slap_next = 2 + (std::rand() % 2);   // 2 oder 3

    // Die Antwort kommt nach dem LETZTEN Klaps einer Serie -- jeder weitere
    // schiebt sie nach hinten (PlaySound schneidet sonst den Klatscher ab).
    m_reply_due = now + cfg.reply_delay;
}

void RE4VRJiggle::tick_reply() {
    if (m_reply_due <= 0.0 || clock_now() < m_reply_due) {
        return;
    }

    m_reply_due = 0.0;

    // [KEINE ZWEI STIMMEN 20.09.2026 -- Ansage] Hat der Choke bei DIESEM Griff
    // schon eine Stimme von ihr gefeuert, sagt sie nach dem Schlag nichts.
    // Hat er geschwiegen (er redet nur bei jedem 3.-4. Griff), kommt unserer.
    if (auto& ch = RE4VRChoke::get();
        ch != nullptr && ch->is_holding() && ch->ashley_spoke_this_hold()) {
        return;
    }

    const int idx = re4vr::ashley_reply_wav_next(true, m_reply_bag, &m_reply_last);

    if (idx < 0) {
        return;
    }

    // Ueber den Choke: dort laeuft ihr Mund per Huellkurve mit.
    if (auto& ch = RE4VRChoke::get(); ch != nullptr) {
        ch->play_reply_with_mouth(true, idx, re4vr::taunt_wav_gain_db());
    } else {
        re4vr::play_ashley_reply_wav(true, idx, re4vr::taunt_wav_gain_db());
    }
}

// Neigung AUF die animierte lokale Drehung. Hat die Engine den Joint seit
// unserem letzten Schreiben nicht neu gesetzt, gilt die gemerkte animierte
// Basis -- so stapelt sich der Versatz nie.
void RE4VRJiggle::head_apply_joint(JointWrite& jw, ::REManagedObject* j, float share) {
    if (j == nullptr) {
        return;
    }

    glm::quat L{}, W{};

    if (!get_quat(j, "get_LocalRotation", L) || !get_quat(j, "get_Rotation", W)) {
        return;
    }

    glm::quat base = L;

    if (jw.written.has_value() && quat_close(L, *jw.written) && jw.base.has_value()) {
        base = *jw.base;
    } else {
        jw.base = L;
    }

    const float ang = glm::length(m_th) * share;

    if (ang < 1e-6f) {
        // Ruhe: den Joint einmal an die Animation zurueckgeben.
        if (jw.written.has_value()) {
            set_quat(j, "set_LocalRotation", base);
            jw.written.reset();
        }

        return;
    }

    const glm::quat q = glm::angleAxis(ang, glm::normalize(m_th));
    // Eltern-Welt haengt nicht an unserer eigenen Lokaldrehung: P = W * conj(L)
    const glm::quat P = W * glm::conjugate(L);
    const glm::quat nl = glm::normalize(glm::conjugate(P) * q * P * base);

    set_quat(j, "set_LocalRotation", nl);
    jw.written = nl;
}

void RE4VRJiggle::head_apply() {
    if (!cfg.enabled || !cfg.drive_head || m_body == nullptr) {
        return;
    }

    head_apply_joint(m_jw_neck, m_neck_j, cfg.head_neck_share);
    head_apply_joint(m_jw_head, m_head_j, m_neck_j != nullptr ? (1.0f - cfg.head_neck_share) : 1.0f);
}

// ============================================================================
// Hooks
// ============================================================================
void RE4VRJiggle::on_application_entry(void* entry, const char* name, size_t hash) {
    cfg_ensure();

    if (re4vr::mods_gated() || !cfg.enabled) {
        return;
    }

    if (hash == "LateUpdateBehavior"_fnv) {
        refresh_body();
        tick_body();
        tick_reply();
    } else if (hash == "UpdateJointExpression"_fnv) {
        // [KEIN KOPF IM GRIFF 20.09.2026] Beim Wuergegriff liegt die Hand am
        // Hals -- der Kopf-Schubs haette dort dauernd Kontakt und der Klatscher
        // kaeme beim Zupacken. Solange jemand gehalten wird: nichts davon.
        if (cfg.drive_head && m_body != nullptr) {
            head_step();
            head_apply();
        }
    }
}

void RE4VRJiggle::on_pre_application_entry(void* entry, const char* name, size_t hash) {
    if (re4vr::mods_gated() || !cfg.enabled) {
        return;
    }

    if (hash == "BeginRendering"_fnv) {
        head_apply();
    }
}

// ============================================================================
// Developer-Tree "RE4VR - Jiggle"
// ============================================================================
void RE4VRJiggle::draw_dev_ui() {
    if (!ImGui::TreeNode("RE4VR - Jiggle")) {
        return;
    }

    cfg_ensure();
    bool ch = false;

    if (ImGui::Checkbox("Enabled##jig", &cfg.enabled)) {
        ch = true;

        if (!cfg.enabled) {
            zero_wind();
        }
    }

    ch |= ImGui::Checkbox("Head##jig", &cfg.drive_head);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Cloth##jig", &cfg.drive_cloth);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Chain##jig", &cfg.drive_chain);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("Wind##jig", &cfg.drive_wind);

    ch |= ImGui::SliderFloat("Head sensitivity (master)##jig", &cfg.head_sensitivity, 0.1f, 3.0f, "%.2f");
    ch |= ImGui::SliderFloat("Head gain##jig", &cfg.head_gain, 1.0f, 40.0f, "%.1f");
    ch |= ImGui::SliderFloat("Head max##jig", &cfg.head_max_deg, 10.0f, 70.0f, "%.0f");
    ch |= ImGui::SliderFloat("Head drag##jig", &cfg.head_drag, 0.0f, 40.0f, "%.1f");
    ch |= ImGui::SliderFloat("Head spring##jig", &cfg.head_stiffness, 10.0f, 400.0f, "%.0f");
    ch |= ImGui::SliderFloat("Head damping##jig", &cfg.head_damping, 0.0f, 40.0f, "%.1f");
    ch |= ImGui::SliderFloat("Head radius##jig", &cfg.head_radius, 0.05f, 0.30f, "%.2f");
    ch |= ImGui::SliderFloat("Head max spin##jig", &cfg.head_max_spin, 2.0f, 40.0f, "%.1f");
    ch |= ImGui::SliderFloat("Head neck share##jig", &cfg.head_neck_share, 0.0f, 1.0f, "%.2f");

    ch |= ImGui::Checkbox("Sounds##jig", &cfg.sounds);
    ch |= ImGui::SliderFloat("Slap min speed##jig", &cfg.slap_min_speed, 0.0f, 3.0f, "%.2f m/s");
    ch |= ImGui::SliderFloat("Reply delay##jig", &cfg.reply_delay, 0.2f, 3.0f, "%.2f s");
    ch |= ImGui::SliderFloat("Slap dB##jig", &cfg.slap_gain_db, -30.0f, 12.0f, "%+.1f dB");

    ch |= ImGui::SliderFloat("Cloth hand radius##jig", &cfg.cloth_radius, 0.02f, 0.20f, "%.3f");
    ch |= ImGui::SliderFloat("Chain contact radius##jig", &cfg.chain_radius, 0.04f, 0.25f, "%.3f");
    ch |= ImGui::SliderFloat("Wind strength##jig", &cfg.wind_strength, 0.0f, 20.0f, "%.2f");
    ch |= ImGui::SliderFloat("Poke range##jig", &cfg.poke_range, 0.10f, 0.50f, "%.2f");

    ImGui::Text("Body: %s  cloth=%d chain=%d soft=%d head=%s neck=%s",
                m_body != nullptr ? name_of(m_body).c_str() : "-",
                (int)m_gpu_cloths.size(), (int)m_chains.size(), (int)m_soft.size(),
                m_head_j != nullptr ? "ja" : "nein", m_neck_j != nullptr ? "ja" : "nein");
    ImGui::Text("Dist %.3f m  cloth=%d chain=%d  head=%d tilt=%.1f deg",
                m_dist, (int)m_contact_cloth, (int)m_contact_chain, (int)m_contact_head, m_head_angle);

    // Jede Aenderung landet in der JSON -- geschrieben, sobald der Regler
    // losgelassen ist (nicht bei jedem Zwischenwert).
    if (ch) {
        m_cfg_dirty = true;
    }

    if (m_cfg_dirty && !ImGui::IsAnyItemActive()) {
        cfg_save();
    }

    ImGui::TreePop();
}

#endif // RE4
