#include "goblin_postfx.hpp"

#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

namespace
{
    bool g_enabled = false, g_tried = false, g_ok = false;
    int g_mode = 3;          // default: edge-detect greybox outline
    float g_strength = 1.0f;
    ID3D12RootSignature *g_root = nullptr;
    ID3D12PipelineState *g_pso = nullptr;
    ID3D12DescriptorHeap *g_srvheap = nullptr;   // 1 shader-visible SRV (the temp copy)
    ID3D12Resource *g_temp = nullptr;
    UINT g_w = 0, g_h = 0;
    DXGI_FORMAT g_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    bool compile(const char *src, const char *target, ID3DBlob **out)
    {
        ID3DBlob *err = nullptr;
        HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out, &err);
        if (FAILED(hr))
        {
            spdlog::warn("[POSTFX] compile {} failed hr={:#x}: {}", target, (unsigned)hr,
                         err ? (const char *)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        return true;
    }

    bool init(ID3D12Device *dev)
    {
        const char *VS =
            "struct VOut{ float4 pos:SV_POSITION; float2 uv:TEXCOORD; };\n"
            "VOut main(uint id:SV_VertexID){ VOut o; o.uv=float2((id<<1)&2, id&2);\n"
            " o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }\n";
        const char *PS =
            "Texture2D tex:register(t0); SamplerState smp:register(s0);\n"
            "cbuffer C:register(b0){ int mode; float strength; float2 texel; };\n"
            "struct VOut{ float4 pos:SV_POSITION; float2 uv:TEXCOORD; };\n"
            "float lum(float3 c){ return dot(c,float3(0.299,0.587,0.114)); }\n"
            "float4 main(VOut i):SV_TARGET{\n"
            " float3 c = tex.Sample(smp,i.uv).rgb;\n"
            " if(mode==1){ float g=lum(c); c=lerp(c,g.xxx,strength); }\n"
            " else if(mode==2){ float n=4.0; c=floor(c*n)/n; }\n"
            " else if(mode==3||mode==4){\n"
            "   float l=lum(c);\n"
            "   float lx=lum(tex.Sample(smp,i.uv+float2(texel.x,0)).rgb);\n"
            "   float ly=lum(tex.Sample(smp,i.uv+float2(0,texel.y)).rgb);\n"
            "   float e=saturate((abs(l-lx)+abs(l-ly))*strength*8.0);\n"
            "   float3 base = (mode==4)? lerp(c, lum(c).xxx, 0.7) : c;\n"
            "   c = lerp(base, float3(0.05,0.05,0.06), e);\n"   // dark ink on edges = greybox outline
            " }\n"
            " return float4(c,1);\n"
            "}\n";

        ID3DBlob *vsb = nullptr, *psb = nullptr;
        if (!compile(VS, "vs_5_0", &vsb) || !compile(PS, "ps_5_0", &psb))
        {
            if (vsb) vsb->Release();
            if (psb) psb->Release();
            return false;
        }

        // Root sig: [0] SRV table (t0), [1] 4x 32-bit root constants (b0), static sampler s0.
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[1].Constants.ShaderRegister = 0;
        rp[1].Constants.Num32BitValues = 4;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.ShaderRegister = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &ss;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *rsblob = nullptr, *rserr = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr)) ||
            FAILED(dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                            IID_PPV_ARGS(&g_root))))
        {
            spdlog::warn("[POSTFX] root sig failed: {}", rserr ? (const char *)rserr->GetBufferPointer() : "?");
            if (rsblob) rsblob->Release();
            if (rserr) rserr->Release();
            vsb->Release(); psb->Release();
            return false;
        }
        if (rsblob) rsblob->Release();
        if (rserr) rserr->Release();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
        pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = 1;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        for (auto &rt : pd.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = FALSE;

        HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
        vsb->Release(); psb->Release();
        if (FAILED(hr)) { spdlog::warn("[POSTFX] PSO failed hr={:#x}", (unsigned)hr); return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvheap))))
        { spdlog::warn("[POSTFX] srv heap failed"); return false; }

        spdlog::info("[POSTFX] init OK");
        return true;
    }

    // (Re)create the temp texture to match the backbuffer + write its SRV.
    bool ensure_temp(ID3D12Device *dev, ID3D12Resource *bb)
    {
        D3D12_RESOURCE_DESC bd = bb->GetDesc();
        if (g_temp && g_w == (UINT)bd.Width && g_h == bd.Height && g_fmt == bd.Format) return true;
        if (g_temp) { g_temp->Release(); g_temp = nullptr; }
        g_w = (UINT)bd.Width; g_h = bd.Height; g_fmt = bd.Format;

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = g_w; rd.Height = g_h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = g_fmt; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&g_temp))))
        { spdlog::warn("[POSTFX] temp tex alloc failed {}x{}", g_w, g_h); return false; }

        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = g_fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(g_temp, &sv, g_srvheap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &br);
    }
}

namespace goblin::postfx
{
    void set_enabled(bool on) { g_enabled = on; }
    bool enabled() { return g_enabled; }
    void set_mode(int m) { if (m >= 1 && m <= 4) g_mode = m; }
    void set_strength(float s) { if (s >= 0 && s <= 8) g_strength = s; }

    void apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *bb,
               const D3D12_CPU_DESCRIPTOR_HANDLE &rtv)
    {
        if (!g_enabled || !dev || !cl || !bb) return;
        if (!g_tried) { g_tried = true; g_ok = init(dev); }
        if (!g_ok || !ensure_temp(dev, bb)) return;

        // ER's frame is in the backbuffer (PRESENT). Copy it to temp, then restyle temp -> backbuffer.
        barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(cl, g_temp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(g_temp, bb);
        barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(cl, g_temp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        D3D12_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0.f, 1.f};
        D3D12_RECT scr{0, 0, (LONG)g_w, (LONG)g_h};
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &scr);
        cl->SetDescriptorHeaps(1, &g_srvheap);
        cl->SetGraphicsRootSignature(g_root);
        cl->SetPipelineState(g_pso);
        cl->SetGraphicsRootDescriptorTable(0, g_srvheap->GetGPUDescriptorHandleForHeapStart());
        struct { int mode; float strength; float tx, ty; } cb{g_mode, g_strength, 1.f / g_w, 1.f / g_h};
        cl->SetGraphicsRoot32BitConstants(1, 4, &cb, 0);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        // backbuffer left in RENDER_TARGET — the caller's overlay path draws on top, then bb -> PRESENT.
    }
}
