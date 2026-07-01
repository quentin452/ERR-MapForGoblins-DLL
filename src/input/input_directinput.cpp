#include "input_directinput.hpp"
#include "input_shared.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>
#include <spdlog/spdlog.h>

namespace goblin::input
{
namespace
{
// DirectInput8 — ER's primary input path (imports DINPUT8.dll); the world-map
// cursor/pan follows the mouse through here even after raw input + window
// messages are blocked. Zero the device state while the menu is open.
using DIGetDeviceStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD, LPVOID);
using DIGetDeviceDataFn = HRESULT(STDMETHODCALLTYPE *)(IDirectInputDevice8 *, DWORD,
                                                       LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
DIGetDeviceStateFn o_di_get_device_state = nullptr;
DIGetDeviceDataFn o_di_get_device_data = nullptr;

// DirectInput8 device hooks. The vtable is shared by all devices (mouse +
// keyboard), so zeroing on menu_open() blocks both — which is exactly what we
// want while the menu owns input.
HRESULT STDMETHODCALLTYPE hk_di_get_device_state(IDirectInputDevice8 *dev, DWORD cb,
                                                 LPVOID data)
{
    HRESULT hr = o_di_get_device_state(dev, cb, data);
    if (menu_open() && data && SUCCEEDED(hr))
        memset(data, 0, cb);   // no axes / no buttons / no keys
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_di_get_device_data(IDirectInputDevice8 *dev, DWORD cb,
                                                LPDIDEVICEOBJECTDATA rg, LPDWORD inout,
                                                DWORD flags)
{
    HRESULT hr = o_di_get_device_data(dev, cb, rg, inout, flags);
    if (menu_open() && inout)
        *inout = 0;            // report zero buffered events
    return hr;
}
} // namespace

void install_directinput_hooks()
{
    // Resolve the IDirectInputDevice8 vtable via a throwaway mouse device, then hook
    // GetDeviceState (vtable[9]) + GetDeviceData (vtable[10]).
    IDirectInput8 *di8 = nullptr;
    IDirectInputDevice8 *dev = nullptr;
    if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                     IID_IDirectInput8, reinterpret_cast<void **>(&di8),
                                     nullptr)) &&
        di8 && SUCCEEDED(di8->CreateDevice(GUID_SysMouse, &dev, nullptr)) && dev)
    {
        void **vt = *reinterpret_cast<void ***>(dev);
        void *gds = vt[9], *gdd = vt[10];
        if (MH_CreateHook(gds, reinterpret_cast<void *>(&hk_di_get_device_state),
                          reinterpret_cast<void **>(&o_di_get_device_state)) == MH_OK)
            MH_EnableHook(gds);
        if (MH_CreateHook(gdd, reinterpret_cast<void *>(&hk_di_get_device_data),
                          reinterpret_cast<void **>(&o_di_get_device_data)) == MH_OK)
            MH_EnableHook(gdd);
        spdlog::info("[OVERLAY] DirectInput8 device hooks installed");
    }
    else
    {
        spdlog::warn("[OVERLAY] DirectInput8 resolve failed — map may still follow mouse");
    }
    if (dev) dev->Release();
    if (di8) di8->Release();
}
} // namespace goblin::input
