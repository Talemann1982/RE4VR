#include <string>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "SharpenPass.hpp"

#pragma comment(lib, "d3dcompiler")

namespace d3d12 {
namespace {
// Contrast Adaptive Sharpening, kompakt nachgebaut (AMD FidelityFX CAS, MIT).
//
// Kern: aus dem 3x3-Kreuz um jedes Pixel werden Minimum und Maximum je Kanal
// bestimmt. Wie viel geschaerft werden darf, haengt vom lokalen Kontrast ab --
// in flachen Bereichen (Himmel, Nebel) wenig, an Kanten mehr. Dadurch rauscht
// es nicht auf und es entstehen keine hellen Saeume wie bei starrem Unsharp
// Masking.
constexpr char CAS_SHADER[] = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

cbuffer Params : register(b0) {
    float g_sharpness;   // 0 = aus .. 1 = maximal
    uint  g_width;
    uint  g_height;
    uint  g_debug;       // 1 = rechte Bildhaelfte gruen faerben (Sichttest)
};

float3 tap(int2 p) {
    p = clamp(p, int2(0, 0), int2(g_width - 1, g_height - 1));
    return g_src.Load(int3(p, 0)).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_width || id.y >= g_height) {
        return;
    }

    const int2 p = int2(id.xy);

    const float3 c = tap(p);
    const float3 n = tap(p + int2( 0, -1));
    const float3 s = tap(p + int2( 0,  1));
    const float3 w = tap(p + int2(-1,  0));
    const float3 e = tap(p + int2( 1,  0));

    const float3 mn = min(c, min(min(n, s), min(w, e)));
    const float3 mx = max(c, max(max(n, s), max(w, e)));

    // Wie viel Spielraum nach oben/unten bleibt -- das begrenzt die Schaerfe.
    const float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-5));
    const float3 wgt = -sqrt(amp) * lerp(0.125, 0.2, saturate(g_sharpness));

    const float3 sum = (n + s + w + e) * wgt + c;
    const float3 div = 1.0 + 4.0 * wgt;

    float3 outc = sum / max(div, 1e-5);
    outc = clamp(outc, mn, mx);   // nie ueber die Nachbarschaft hinaus

    // Sichttest: kommt dieser Pass im fertigen Bild an?
    if (g_debug != 0 && id.x > (g_width / 2)) {
        outc = lerp(outc, float3(0.0, 1.0, 0.0), 0.5);
    }

    g_dst[id.xy] = float4(outc, g_src.Load(int3(p, 0)).a);
}
)";
}

bool SharpenPass::setup(ID3D12Device* device) {
    if (device == nullptr) {
        return false;
    }

    if (m_pso != nullptr) {
        return true;
    }

    ComPtr<ID3DBlob> shader{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(D3DCompile(CAS_SHADER, sizeof(CAS_SHADER) - 1, nullptr, nullptr, nullptr, "main",
                          "cs_5_0", 0, 0, &shader, &errors))) {
        spdlog::error("[SharpenPass] Shader liess sich nicht uebersetzen: {}",
                      errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    // t0 und u0 als Tabelle, dazu vier Root-Konstanten (Staerke + Groesse).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;

    ComPtr<ID3DBlob> rs_blob{};

    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob,
                                           &errors))) {
        spdlog::error("[SharpenPass] Root Signature fehlgeschlagen");
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                           IID_PPV_ARGS(&m_root_sig)))) {
        spdlog::error("[SharpenPass] CreateRootSignature fehlgeschlagen");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_sig.Get();
    pso_desc.CS.pShaderBytecode = shader->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader->GetBufferSize();

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso)))) {
        spdlog::error("[SharpenPass] CreateComputePipelineState fehlgeschlagen");
        m_root_sig.Reset();
        return false;
    }

    // Ein Deskriptorpaar (SRV + UAV) je Ring-Platz: der Aufruf fuer das zweite
    // Auge darf das Paar des ersten nicht ueberschreiben, solange die GPU die
    // Liste noch gar nicht abgearbeitet hat.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = RING_COUNT * 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        spdlog::error("[SharpenPass] CreateDescriptorHeap fehlgeschlagen");
        m_pso.Reset();
        m_root_sig.Reset();
        return false;
    }

    m_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    spdlog::info("[SharpenPass] bereit (CAS, Compute, in place)");

    return true;
}

int32_t SharpenPass::ensure_target(ID3D12Device* device, ID3D12Resource* src) {
    if (device == nullptr || src == nullptr) {
        return -1;
    }

    const auto desc = src->GetDesc();

    // Vorhandenen Slot dieser Quelle wiederverwenden, solange die Groesse passt.
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        auto& s = m_slots[i];

        if (s.src == src && s.target != nullptr && s.width == (uint32_t)desc.Width
            && s.height == desc.Height && s.format == desc.Format) {
            return (int32_t)i;
        }
    }

    int32_t index = -1;

    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        if (m_slots[i].src == src || m_slots[i].target == nullptr) {
            index = (int32_t)i;

            break;
        }
    }

    if (index < 0) {
        index = (int32_t)(m_next_slot % SLOT_COUNT);
        m_next_slot = (m_next_slot + 1) % SLOT_COUNT;
    }

    auto& slot = m_slots[index];
    slot.target.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto tex = desc;
    tex.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&slot.target)))) {
        spdlog::error("[SharpenPass] Zwischentextur konnte nicht angelegt werden");
        slot.src = nullptr;

        return -1;
    }

    slot.target->SetName(L"RE4VR SharpenPass target");
    slot.src = src;
    slot.width = (uint32_t)desc.Width;
    slot.height = desc.Height;
    slot.format = desc.Format;

    return index;
}

bool SharpenPass::dispatch_inplace(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                   ID3D12Resource* src, float sharpness, bool debug) {
    if (device == nullptr || cmd == nullptr || src == nullptr) {
        return false;
    }

    if (sharpness <= 0.0f && !debug) {
        return false;   // aus -- Bild unveraendert lassen
    }

    if (!setup(device)) {
        return false;   // im Zweifel lieber unscharf als kaputt
    }

    const auto slot_index = ensure_target(device, src);

    if (slot_index < 0) {
        return false;
    }

    auto& slot = m_slots[slot_index];

    const uint32_t ring = m_ring;
    m_ring = (m_ring + 1) % RING_COUNT;

    auto cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = m_heap->GetGPUDescriptorHandleForHeapStart();

    cpu.ptr += (SIZE_T)(ring * 2) * m_descriptor_size;
    gpu.ptr += (UINT64)(ring * 2) * m_descriptor_size;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = slot.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = slot.format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_uav{cpu.ptr + m_descriptor_size};
    device->CreateUnorderedAccessView(slot.target.Get(), nullptr, &uav, cpu_uav);

    // Quelle zum Lesen umschalten -- am Ende steht sie wieder auf
    // UNORDERED_ACCESS, so wie der Aufrufer sie uebergeben hat.
    D3D12_RESOURCE_BARRIER to_srv{};
    to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_srv.Transition.pResource = src;
    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &to_srv);

    ID3D12DescriptorHeap* heaps[]{m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_root_sig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu);

    // Obergrenze 1.2 -- darueber entstehen Farbartefakte (vom User gemessen).
    const float s = sharpness > 1.2f ? 1.2f : sharpness;
    cmd->SetComputeRoot32BitConstants(1, 1, &s, 0);
    cmd->SetComputeRoot32BitConstants(1, 1, &slot.width, 1);
    cmd->SetComputeRoot32BitConstants(1, 1, &slot.height, 2);

    const uint32_t dbg = debug ? 1u : 0u;
    cmd->SetComputeRoot32BitConstants(1, 1, &dbg, 3);

    cmd->Dispatch((slot.width + 7) / 8, (slot.height + 7) / 8, 1);

    // [IN PLACE 15.09.2026] Ergebnis zurueck in die upscaled Textur -- nur die
    // sieht das HMD (VR.cpp: m_multipass.eye_textures = get_upscaled_texture()).
    // Die Transitionen sorgen zugleich dafuer, dass der Compute-Pass fertig
    // ist, bevor kopiert wird.
    D3D12_RESOURCE_BARRIER to_copy[2]{};
    to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy[0].Transition.pResource = slot.target.Get();
    to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy[1].Transition.pResource = src;
    to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    cmd->ResourceBarrier(2, to_copy);

    cmd->CopyResource(src, slot.target.Get());

    D3D12_RESOURCE_BARRIER back[2]{to_copy[0], to_copy[1]};
    back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    cmd->ResourceBarrier(2, back);

    return true;
}

void SharpenPass::reset() {
    for (auto& s : m_slots) {
        s.target.Reset();
        s.src = nullptr;
        s.width = 0;
        s.height = 0;
        s.format = DXGI_FORMAT_UNKNOWN;
    }

    m_heap.Reset();
    m_pso.Reset();
    m_root_sig.Reset();
    m_ring = 0;
    m_next_slot = 0;
}
}
