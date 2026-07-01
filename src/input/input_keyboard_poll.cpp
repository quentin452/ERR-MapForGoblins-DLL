#include "input_keyboard_poll.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>

namespace goblin::input
{
namespace
{
struct KeyEntry { int vk; ImGuiKey key; bool isText; };

// isText = attempt ToUnicodeEx translation on press (letters/digits/punctuation/numpad).
// Nav/edit keys (Backspace, Enter, arrows, ...) only ever feed AddKeyEvent, never a character —
// matches how TranslateMessage()/WM_CHAR only fires for character-producing keys.
constexpr KeyEntry kKeys[] = {
    {'A', ImGuiKey_A, true}, {'B', ImGuiKey_B, true}, {'C', ImGuiKey_C, true}, {'D', ImGuiKey_D, true},
    {'E', ImGuiKey_E, true}, {'F', ImGuiKey_F, true}, {'G', ImGuiKey_G, true}, {'H', ImGuiKey_H, true},
    {'I', ImGuiKey_I, true}, {'J', ImGuiKey_J, true}, {'K', ImGuiKey_K, true}, {'L', ImGuiKey_L, true},
    {'M', ImGuiKey_M, true}, {'N', ImGuiKey_N, true}, {'O', ImGuiKey_O, true}, {'P', ImGuiKey_P, true},
    {'Q', ImGuiKey_Q, true}, {'R', ImGuiKey_R, true}, {'S', ImGuiKey_S, true}, {'T', ImGuiKey_T, true},
    {'U', ImGuiKey_U, true}, {'V', ImGuiKey_V, true}, {'W', ImGuiKey_W, true}, {'X', ImGuiKey_X, true},
    {'Y', ImGuiKey_Y, true}, {'Z', ImGuiKey_Z, true},
    {'0', ImGuiKey_0, true}, {'1', ImGuiKey_1, true}, {'2', ImGuiKey_2, true}, {'3', ImGuiKey_3, true},
    {'4', ImGuiKey_4, true}, {'5', ImGuiKey_5, true}, {'6', ImGuiKey_6, true}, {'7', ImGuiKey_7, true},
    {'8', ImGuiKey_8, true}, {'9', ImGuiKey_9, true},
    {VK_SPACE, ImGuiKey_Space, true},
    {VK_OEM_1, ImGuiKey_Semicolon, true}, {VK_OEM_PLUS, ImGuiKey_Equal, true},
    {VK_OEM_COMMA, ImGuiKey_Comma, true}, {VK_OEM_MINUS, ImGuiKey_Minus, true},
    {VK_OEM_PERIOD, ImGuiKey_Period, true}, {VK_OEM_2, ImGuiKey_Slash, true},
    {VK_OEM_3, ImGuiKey_GraveAccent, true}, {VK_OEM_4, ImGuiKey_LeftBracket, true},
    {VK_OEM_5, ImGuiKey_Backslash, true}, {VK_OEM_6, ImGuiKey_RightBracket, true},
    {VK_OEM_7, ImGuiKey_Apostrophe, true},
    {VK_NUMPAD0, ImGuiKey_Keypad0, true}, {VK_NUMPAD1, ImGuiKey_Keypad1, true},
    {VK_NUMPAD2, ImGuiKey_Keypad2, true}, {VK_NUMPAD3, ImGuiKey_Keypad3, true},
    {VK_NUMPAD4, ImGuiKey_Keypad4, true}, {VK_NUMPAD5, ImGuiKey_Keypad5, true},
    {VK_NUMPAD6, ImGuiKey_Keypad6, true}, {VK_NUMPAD7, ImGuiKey_Keypad7, true},
    {VK_NUMPAD8, ImGuiKey_Keypad8, true}, {VK_NUMPAD9, ImGuiKey_Keypad9, true},
    {VK_DECIMAL, ImGuiKey_KeypadDecimal, true}, {VK_DIVIDE, ImGuiKey_KeypadDivide, true},
    {VK_MULTIPLY, ImGuiKey_KeypadMultiply, true}, {VK_SUBTRACT, ImGuiKey_KeypadSubtract, true},
    {VK_ADD, ImGuiKey_KeypadAdd, true},
    {VK_BACK, ImGuiKey_Backspace, false}, {VK_TAB, ImGuiKey_Tab, false},
    {VK_RETURN, ImGuiKey_Enter, false}, {VK_ESCAPE, ImGuiKey_Escape, false},
    {VK_DELETE, ImGuiKey_Delete, false}, {VK_INSERT, ImGuiKey_Insert, false},
    {VK_HOME, ImGuiKey_Home, false}, {VK_END, ImGuiKey_End, false},
    {VK_PRIOR, ImGuiKey_PageUp, false}, {VK_NEXT, ImGuiKey_PageDown, false},
    {VK_LEFT, ImGuiKey_LeftArrow, false}, {VK_RIGHT, ImGuiKey_RightArrow, false},
    {VK_UP, ImGuiKey_UpArrow, false}, {VK_DOWN, ImGuiKey_DownArrow, false},
};
constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

bool s_prevDown[kKeyCount] = {};

inline bool is_vk_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void feed_char_for_key(ImGuiIO &io, int vk)
{
    BYTE keyState[256];
    if (!GetKeyboardState(keyState))
        return;
    HKL layout = GetKeyboardLayout(0);
    UINT scanCode = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, layout);
    wchar_t buf[8] = {};
    int n = ToUnicodeEx(static_cast<UINT>(vk), scanCode, keyState, buf, 8, 0, layout);
    // n > 0: resolved character(s) -- feed them. n == 0: no translation in this layout (e.g. a
    // bare modifier). n < 0: a dead key awaiting its 2nd keystroke -- nothing to feed yet, same
    // as WM_CHAR only firing once the accent is resolved.
    if (n > 0)
        for (int i = 0; i < n; ++i)
            io.AddInputCharacter(static_cast<unsigned int>(buf[i]));
}
} // namespace

void poll_keyboard_text_input()
{
    ImGuiIO &io = ImGui::GetIO();
    // Modifiers: refresh every frame (not edge-triggered) -- cheap, mirrors
    // ImGui_ImplWin32_UpdateKeyModifiers()'s own per-frame refresh.
    io.AddKeyEvent(ImGuiMod_Ctrl, is_vk_down(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, is_vk_down(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt, is_vk_down(VK_MENU));
    io.AddKeyEvent(ImGuiKey_LeftShift, is_vk_down(VK_LSHIFT));
    io.AddKeyEvent(ImGuiKey_RightShift, is_vk_down(VK_RSHIFT));
    io.AddKeyEvent(ImGuiKey_LeftCtrl, is_vk_down(VK_LCONTROL));
    io.AddKeyEvent(ImGuiKey_RightCtrl, is_vk_down(VK_RCONTROL));
    io.AddKeyEvent(ImGuiKey_LeftAlt, is_vk_down(VK_LMENU));
    io.AddKeyEvent(ImGuiKey_RightAlt, is_vk_down(VK_RMENU));

    for (int i = 0; i < kKeyCount; ++i)
    {
        const bool down = is_vk_down(kKeys[i].vk);
        if (down == s_prevDown[i])
            continue;
        s_prevDown[i] = down;
        io.AddKeyEvent(kKeys[i].key, down);
        if (down && kKeys[i].isText)
            feed_char_for_key(io, kKeys[i].vk);
    }
}
} // namespace goblin::input
