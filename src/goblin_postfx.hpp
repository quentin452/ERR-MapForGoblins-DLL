#pragma once
// goblin_postfx — greybox job #2b: restyle ER's FINAL rendered frame with a full-screen post-process
// pass, in the present hook. NO ER-shader RE, no PSO interception — we copy the backbuffer to a temp SRV
// then draw a full-screen triangle sampling it with our style shader, writing back to the backbuffer.
// Safe + engine-agnostic (works on any frame). Stylised (grayscale / posterize / edge), not true wireframe.
// Applied BEFORE our r3d + ImGui so the overlay/HUD stay crisp on top.

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;

namespace goblin::postfx
{
    // Record the post-process (copy backbuffer -> temp -> full-screen restyle -> backbuffer). Lazy init +
    // resize-aware (temp matches the backbuffer). ONLY call when enabled(): it leaves the backbuffer in
    // RENDER_TARGET state (the caller then skips its own PRESENT->RENDER_TARGET barrier). No-op if init fails.
    void apply(ID3D12Device *dev, ID3D12GraphicsCommandList *cl, ID3D12Resource *backbuffer,
               const D3D12_CPU_DESCRIPTOR_HANDLE &rtv);

    void set_enabled(bool on);
    bool enabled();
    void set_mode(int m);       // 1=grayscale 2=posterize 3=edge 4=edge-on-desat
    void set_strength(float s);
}
