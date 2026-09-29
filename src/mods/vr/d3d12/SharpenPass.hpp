#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
// [SCHAERFE NACH DEM UPSCALING 15.09.2026]
//
// Der Sharpness-Regler des Upscalers ist wirkungslos: `enableSharpening` und
// `sharpness` gehen an InitUpscaler/EvaluateUpscaler, also ins PDPerfPlugin --
// NVIDIA hat das Sharpening aus DLSS entfernt (seit 2.5.1 wird der Wert
// ignoriert). Das Plugin wird bewusst NICHT angefasst.
//
// Stattdessen ein eigener Pass auf dem FERTIGEN Bild: Contrast Adaptive
// Sharpening (CAS, AMD FidelityFX, hier als kompakter Compute-Shader
// nachgebaut). Er laeuft NACH dem Upscaler und damit unabhaengig davon,
// welche Methode gerade arbeitet (DLSS, FSR3, XESS).
//
// [IN PLACE 15.09.2026 -- gegen "Regler tut nichts in VR"] Frueher schrieb der
// Pass in eine eigene Textur, die NUR in den Backbuffer kopiert wurde. Das HMD
// bekommt sein Bild aber nicht aus dem Backbuffer, sondern direkt aus der
// upscaled Textur (VR.cpp: m_multipass.eye_textures = get_upscaled_texture()).
// Geschaerft wurde also allein das Desktop-Spiegelbild, und nur Auge 0.
// Deshalb jetzt: CAS -> Zwischentextur -> ZURUECK in dieselbe upscaled Textur,
// fuer BEIDE Augen. Danach sieht das HMD dasselbe Bild wie der Backbuffer.
struct SharpenPass {
    // Einmalig: Shader uebersetzen, Root Signature, PSO und Heap anlegen.
    bool setup(ID3D12Device* device);

    // Schaerft `src` an Ort und Stelle. `src` muss im Zustand UNORDERED_ACCESS
    // sein und wird auch so zurueckgelassen. Rueckgabe: true, wenn tatsaechlich
    // geschaerft wurde (bei sharpness <= 0 oder Fehler bleibt das Bild, wie es
    // war -- im Zweifel lieber unscharf als kaputt).
    // `debug` faerbt die RECHTE Bildhaelfte gruen. Damit ist ohne jede Messung
    // sichtbar, ob das Ergebnis dieses Passes im fertigen Bild ankommt: sieht
    // man Gruen, kommt es an; sieht man keins, wird es unterwegs verworfen.
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* src, float sharpness, bool debug = false);

    void reset();

    bool ok() const { return m_pso != nullptr; }

private:
    // Je Quelltextur (linkes/rechtes Auge) eine eigene Zwischentextur -- sonst
    // teilen sich beide Augen eine, und in derselben Command-List kaeme das
    // zweite Auge dem ersten dazwischen.
    static constexpr uint32_t SLOT_COUNT = 2;

    // Die Deskriptoren werden auf der CPU geschrieben, gelesen wird erst
    // spaeter auf der GPU. Mit nur EINEM Paar wuerde der zweite Aufruf das Paar
    // des ersten ueberschreiben -- beide Dispatches saehen dann die Textur des
    // zuletzt angemeldeten Auges. Deshalb ein Ring.
    static constexpr uint32_t RING_COUNT = 8;

    struct Slot {
        ComPtr<ID3D12Resource> target{};
        ID3D12Resource* src{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };

    // Liefert den Slot-Index fuer diese Quelle (legt die Zwischentextur an bzw.
    // passt sie an eine neue Groesse an), -1 im Fehlerfall.
    int32_t ensure_target(ID3D12Device* device, ID3D12Resource* src);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};

    std::array<Slot, SLOT_COUNT> m_slots{};

    uint32_t m_ring{0};
    uint32_t m_next_slot{0};
    uint32_t m_descriptor_size{0};
};

// ============================================================================
// [POST_PASS 28.09.2026 -- Ansage des Users: "AA / Sharpness / Saturation auch
// OHNE Upscaling, wie ReShade"]
//
// Eigener Nachbearbeitungs-Pass auf den FERTIGEN Augenbildern, direkt bevor
// sie an OpenXR/OpenVR gehen (D3D12Component, Multipass-Zweig). Das Spiel kann
// hier nichts mehr zurueckschreiben (ToneMapping/CustomFilter werden jeden
// Frame vom Spiel neu gesetzt -- per Sonde belegt).
// Schritt 1: Schaerfe (LumaSharpen-Art, deutlich kraeftiger als CAS) +
// Saettigung. SMAA folgt als eigener Schritt.
// Der Upscaler-Weg (SharpenPass oben) bleibt unangetastet -- der Pass laeuft
// nur, wenn der Upscaler NICHT arbeitet.
// ============================================================================
struct PostPass {
    bool setup(ID3D12Device* device);

    // Bearbeitet `src` an Ort und Stelle. `src_state` ist der Zustand, in dem
    // die Textur uebergeben wird, und in dem sie auch zurueckbleibt.
    // sharpness 0..3 (0 = aus), saturation 0..2 (1 = unveraendert),
    // brightness = Gamma-Exponent (1 = unveraendert, <1 heller), contrast 1 = unveraendert.
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* src, D3D12_RESOURCE_STATES src_state,
                          float sharpness, float saturation,
                          float brightness = 1.0f, float contrast = 1.0f);   // [28.09.2026] 1 = unveraendert

    void reset();

private:
    static constexpr uint32_t SLOT_COUNT = 2;
    static constexpr uint32_t RING_COUNT = 8;

    struct Slot {
        ComPtr<ID3D12Resource> target{};
        ID3D12Resource* src{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};      // Format der Quelle
        DXGI_FORMAT uav_format{DXGI_FORMAT_UNKNOWN};  // Format der Zwischentextur (SRGB -> UNORM)
    };

    int32_t ensure_target(ID3D12Device* device, ID3D12Resource* src);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};

    std::array<Slot, SLOT_COUNT> m_slots{};

    uint32_t m_ring{0};
    uint32_t m_next_slot{0};
    uint32_t m_descriptor_size{0};
    bool m_failed{false};   // Shader/PSO einmal gescheitert -> nie wieder versuchen
};

// ============================================================================
// [POST_PASS SMAA 28.09.2026] SMAA 1x (Jimenez et al., MIT -- Daten und Shader
// in SmaaData.inl, aus ReShade uebernommen). Drei Grafik-Durchgaenge:
// Kanten (Luma) -> Mischgewichte (AreaTex/SearchTex) -> Nachbarn mischen.
// Laeuft wie PostPass an Ort und Stelle auf einem Augenbild; die Quelle bleibt
// in src_state zurueck.
// ============================================================================
struct SmaaPass {
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* src, D3D12_RESOURCE_STATES src_state);

    void reset();

private:
    static constexpr uint32_t SLOT_COUNT = 2;
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t SRV_PER_DRAW = 5;   // t0 Farbe, t1 Kanten, t2 Gewichte, t3 Area, t4 Search

    struct Slot {
        ComPtr<ID3D12Resource> edges{};    // R8G8_UNORM
        ComPtr<ID3D12Resource> blend{};    // R8G8B8A8_UNORM
        ComPtr<ID3D12Resource> out{};      // Format der Quelle
        ID3D12Resource* src{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint32_t rtv_base{0};              // 3 RTVs ab hier im RTV-Heap
    };

    bool setup(ID3D12Device* device);
    bool ensure_pso_out(ID3D12Device* device, DXGI_FORMAT format);
    bool upload_luts(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);
    int32_t ensure_slot(ID3D12Device* device, ID3D12Resource* src);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso_edges{};
    ComPtr<ID3D12PipelineState> m_pso_weights{};
    ComPtr<ID3D12PipelineState> m_pso_out{};
    DXGI_FORMAT m_pso_out_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3DBlob> m_vs_edges{}, m_ps_edges{}, m_vs_weights{}, m_ps_weights{}, m_vs_blend{}, m_ps_blend{};

    ComPtr<ID3D12DescriptorHeap> m_srv_heap{};
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    uint32_t m_srv_size{0};
    uint32_t m_rtv_size{0};

    ComPtr<ID3D12Resource> m_area{};
    ComPtr<ID3D12Resource> m_search{};
    ComPtr<ID3D12Resource> m_upload{};
    bool m_luts_ready{false};

    std::array<Slot, SLOT_COUNT> m_slots{};
    uint32_t m_ring{0};
    uint32_t m_next_slot{0};
    bool m_failed{false};
};

// ============================================================================
// [UI_TRENNUNG 28.09.2026] UI aus Nachbearbeitung/Upscaler heraushalten.
// Maske = Unterschied zwischen den zwei Engine-Kopien pro Auge (ohne UI / mit UI,
// VR::ui_capture). Wo UI ist, kommt das UI-Bild zurueck:
//   ohne Upscaler: die unbearbeitete Kopie des Augenbilds (save_original)
//   mit Upscaler:  die Engine-Kopie "mit UI" in Renderaufloesung (nicht
//                  durch DLSS/FSR verschmiert)
// ============================================================================
struct UiRestorePass {
    // Unbearbeitetes Augenbild sichern (vor SMAA/PostPass). src bleibt in src_state.
    bool save_original(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
                       D3D12_RESOURCE_STATES src_state, int eye);
    ID3D12Resource* original(int eye) const { return (eye == 0 || eye == 1) ? m_orig[eye].Get() : nullptr; }

    // UI zurueck ins Augenbild mischen. ui_src = original(eye) oder Engine "final".
    // hudless/final/(ui_src bei ui_is_final) sind Engine-Texturen in COMMON.
    bool dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src,
                          D3D12_RESOURCE_STATES src_state, ID3D12Resource* ui_src, bool ui_is_final,
                          ID3D12Resource* hudless, ID3D12Resource* final_tex, int eye,
                          bool debug_mask = false);   // [UI_MASKE_ROT] erkannte UI-Pixel rot

    void reset();

private:
    static constexpr uint32_t RING_COUNT = 8;
    static constexpr uint32_t DESC_PER_DRAW = 5;   // t0..t3 + u0

    bool setup(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_root_sig{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_heap{};
    uint32_t m_descriptor_size{0};
    uint32_t m_ring{0};
    bool m_failed{false};

    std::array<ComPtr<ID3D12Resource>, 2> m_orig{};     // unbearbeitetes Augenbild, Ruhezustand NON_PIXEL_SHADER_RESOURCE
    std::array<ComPtr<ID3D12Resource>, 2> m_target{};   // UAV-Ziel, Ruhezustand UNORDERED_ACCESS
    std::array<DXGI_FORMAT, 2> m_target_fmt{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
};
}
