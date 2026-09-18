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
}
