#include "goblin_r3d.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <spdlog/spdlog.h>

namespace
{
    // ── state (created once, on the present thread) ─────────────────────────────
    bool g_enabled = false;
    bool g_tried = false;   // init attempted (success or hard-fail — don't retry a hard fail)
    bool g_ok = false;      // init succeeded
    ID3D12RootSignature *g_root = nullptr;
    ID3D12PipelineState *g_pso = nullptr;
    ID3D12Resource *g_vb = nullptr, *g_ib = nullptr;
    D3D12_VERTEX_BUFFER_VIEW g_vbv{};
    D3D12_INDEX_BUFFER_VIEW g_ibv{};
    uint32_t g_frame = 0;   // spin counter (Date/time-free)

    struct Vtx { float px, py, pz, cr, cg, cb; };

    // ── 4x4 row-major helpers, row-vector convention (v' = v * M) ───────────────
    struct M4 { float m[16]; };
    M4 mul(const M4 &A, const M4 &B)
    {
        M4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += A.m[i * 4 + k] * B.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }
    M4 rotY(float a)
    {
        float c = std::cos(a), s = std::sin(a);
        return M4{{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}};
    }
    M4 rotX(float a)
    {
        float c = std::cos(a), s = std::sin(a);
        return M4{{1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1}};
    }
    M4 translate(float x, float y, float z) { return M4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}}; }
    // LH perspective, row-vector, D3D clip z in [0,1]
    M4 perspLH(float fovY, float aspect, float zn, float zf)
    {
        float f = 1.0f / std::tan(fovY * 0.5f);
        return M4{{f / aspect, 0, 0, 0,
                   0, f, 0, 0,
                   0, 0, zf / (zf - zn), 1,
                   0, 0, -zn * zf / (zf - zn), 0}};
    }

    bool compile(const char *src, const char *target, ID3DBlob **out)
    {
        ID3DBlob *err = nullptr;
        HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, out, &err);
        if (FAILED(hr))
        {
            spdlog::warn("[R3D] shader compile {} failed hr={:#x}: {}", target, (unsigned)hr,
                         err ? (const char *)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        return true;
    }

    ID3D12Resource *make_upload_buf(ID3D12Device *dev, const void *data, size_t bytes)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *res = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))))
            return nullptr;
        void *p = nullptr;
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(res->Map(0, &none, &p)))
        {
            std::memcpy(p, data, bytes);
            res->Unmap(0, nullptr);
        }
        return res;
    }

    bool init(ID3D12Device *dev)
    {
        const char *VS =
            "cbuffer C : register(b0){ row_major float4x4 mvp; };\n"
            "struct VIn{ float3 pos:POSITION; float3 col:COLOR; };\n"
            "struct VOut{ float4 pos:SV_POSITION; float3 col:COLOR; };\n"
            "VOut main(VIn i){ VOut o; o.pos = mul(float4(i.pos,1.0), mvp); o.col = i.col; return o; }\n";
        const char *PS =
            "struct VOut{ float4 pos:SV_POSITION; float3 col:COLOR; };\n"
            "float4 main(VOut i):SV_TARGET{ return float4(i.col,1.0); }\n";

        ID3DBlob *vsb = nullptr, *psb = nullptr;
        if (!compile(VS, "vs_5_0", &vsb) || !compile(PS, "ps_5_0", &psb))
        {
            if (vsb) vsb->Release();
            if (psb) psb->Release();
            return false;
        }

        // Root signature: 16 root 32-bit constants at b0 (the MVP), IA input layout allowed.
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants.ShaderRegister = 0;
        rp.Constants.Num32BitValues = 16;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *rsblob = nullptr, *rserr = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr)) ||
            FAILED(dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                            IID_PPV_ARGS(&g_root))))
        {
            spdlog::warn("[R3D] root sig failed: {}", rserr ? (const char *)rserr->GetBufferPointer() : "?");
            if (rsblob) rsblob->Release();
            if (rserr) rserr->Release();
            vsb->Release(); psb->Release();
            return false;
        }
        if (rsblob) rsblob->Release();
        if (rserr) rserr->Release();

        D3D12_INPUT_ELEMENT_DESC il[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
        pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
        pd.InputLayout = {il, 2};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // matches the overlay's swapchain (ImGui init)
        pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = 1;
        pd.SampleMask = UINT_MAX;
        // Rasterizer: WIREFRAME + no cull => no depth/winding worries for the first proof (greybox look).
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        // Blend: opaque.
        for (auto &rt : pd.BlendState.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        // DepthStencil: disabled.
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;

        HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
        vsb->Release(); psb->Release();
        if (FAILED(hr)) { spdlog::warn("[R3D] PSO failed hr={:#x}", (unsigned)hr); return false; }

        // Unit cube [-0.5,0.5], per-corner color (a colourful cube reads as clearly 3D).
        const Vtx v[8] = {
            {-.5f, -.5f, -.5f, 0, 1, 1}, {.5f, -.5f, -.5f, 1, 0, 1}, {.5f, .5f, -.5f, 1, 1, 0},
            {-.5f, .5f, -.5f, 0, 1, 0}, {-.5f, -.5f, .5f, 0, .4f, 1}, {.5f, -.5f, .5f, 1, .4f, 0},
            {.5f, .5f, .5f, 1, 1, 1},    {-.5f, .5f, .5f, .4f, 1, .4f}};
        const uint16_t idx[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 4, 5, 1, 4, 1, 0,
                                  3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 4, 0, 3, 4, 3, 7};
        g_vb = make_upload_buf(dev, v, sizeof(v));
        g_ib = make_upload_buf(dev, idx, sizeof(idx));
        if (!g_vb || !g_ib) { spdlog::warn("[R3D] VB/IB alloc failed"); return false; }
        g_vbv = {g_vb->GetGPUVirtualAddress(), (UINT)sizeof(v), (UINT)sizeof(Vtx)};
        g_ibv = {g_ib->GetGPUVirtualAddress(), (UINT)sizeof(idx), DXGI_FORMAT_R16_UINT};

        spdlog::info("[R3D] init OK (root+PSO+cube)");
        return true;
    }
}

namespace goblin::r3d
{
    void set_enabled(bool on) { g_enabled = on; }
    bool enabled() { return g_enabled; }

    void draw_test_cube(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, float vpW, float vpH)
    {
        if (!g_enabled || !dev || !cl) return;
        if (!g_tried) { g_tried = true; g_ok = init(dev); }
        if (!g_ok) return;
        if (!(vpW > 0 && vpH > 0)) return;

        const float a = (float)(g_frame++) * 0.02f;
        M4 model = mul(rotX(a * 0.6f), rotY(a));                 // spin
        M4 view = translate(0.f, 0.f, 3.5f);                    // push the box in front of the camera at origin
        M4 proj = perspLH(1.0472f /*60deg*/, vpW / vpH, 0.1f, 100.f);
        M4 mvp = mul(mul(model, view), proj);                   // row-vector: v * Model * View * Proj

        D3D12_VIEWPORT vp{0, 0, vpW, vpH, 0.f, 1.f};
        D3D12_RECT sc{0, 0, (LONG)vpW, (LONG)vpH};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->SetGraphicsRootSignature(g_root);
        cl->SetPipelineState(g_pso);
        cl->SetGraphicsRoot32BitConstants(0, 16, mvp.m, 0);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &g_vbv);
        cl->IASetIndexBuffer(&g_ibv);
        cl->DrawIndexedInstanced(36, 1, 0, 0, 0);
    }
}
