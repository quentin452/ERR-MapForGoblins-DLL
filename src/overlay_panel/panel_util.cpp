// Shared helpers of the F1 panel section files — text matching (accent-folded), the
// settings-search Filter, and the small widgets several sections reuse. Moved verbatim
// from goblin_overlay_render.cpp in the draw_panel split (big-files refactor item 1).

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "goblin_overlay_render_api.hpp"
#include "worldmap/category_meta.hpp"                 // category_gpu_* / rep / color / icon_key
#include "generated_shared/goblin_overlay_icons.hpp"  // baked ATLAS cells (transitional fallback)

#include <cctype>
#include <cstdint>
#include <cstring>

namespace goblin::overlay::panel
{
using goblin::i18n::tr;  // overlay UI localization (lang/<code>.txt)

namespace
{
    void append_folded(std::string &out, uint32_t cp)
    {
        if (cp < 0x80) { out += (char)tolower((int)cp); return; }
        switch (cp)
        {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: out += 'a'; break;
        case 0xC6: case 0xE6: out += "ae"; break;
        case 0xC7: case 0xE7: out += 'c'; break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: out += 'e'; break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: out += 'i'; break;
        case 0xD0: case 0xF0: out += 'd'; break;
        case 0xD1: case 0xF1: out += 'n'; break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: out += 'o'; break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: out += 'u'; break;
        case 0xDD: case 0xFD: case 0xFF: out += 'y'; break;
        case 0xDE: case 0xFE: out += "th"; break;
        case 0xDF: out += "ss"; break;
        case 0x152: case 0x153: out += "oe"; break;  // Œ / œ
        default: break;  // no ASCII base — drop
        }
    }

    // Decode UTF-8 `s` into a lowercase, accent-folded ASCII string (see append_folded).
    std::string fold_ci(const char *s)
    {
        std::string out;
        const unsigned char *p = (const unsigned char *)s;
        while (p && *p)
        {
            unsigned char c = *p;
            uint32_t cp;
            int len;
            if (c < 0x80)            { cp = c;        len = 1; }
            else if ((c >> 5) == 0x6)  { cp = c & 0x1F; len = 2; }
            else if ((c >> 4) == 0xE)  { cp = c & 0x0F; len = 3; }
            else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
            else { ++p; continue; }  // stray continuation / invalid lead — skip
            int i = 1;
            for (; i < len; ++i)
            {
                if ((p[i] & 0xC0) != 0x80) break;  // truncated sequence
                cp = (cp << 6) | (p[i] & 0x3F);
            }
            p += i;  // advance by bytes actually consumed
            append_folded(out, cp);
        }
        return out;
    }
} // namespace

// Case-insensitive, accent-insensitive substring match (empty needle = match all).
// Used by the Sections & categories search box (Quest Browser has its own copy).
bool contains_ci(const char *hay, const char *need)
{
    if (!need || !need[0]) return true;
    return fold_ci(hay).find(fold_ci(need)) != std::string::npos;
}

// Word-order-independent match for the item search: every whitespace-separated
// token of `query` must appear (case/accent-insensitive substring) somewhere in
// `hay`. So "Claw Talisman", "Talisman Claw" and "griffe claw" all match the same
// marker when `hay` is its combined game-language + English text. Empty query (no
// tokens) returns false — callers guard the empty box separately.
bool matches_all_tokens(const std::string &hay, const char *query)
{
    const std::string fh = fold_ci(hay.c_str());
    bool any = false;
    for (const char *p = query; *p;)
    {
        while (*p == ' ' || *p == '\t') ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (p == start) break;
        any = true;
        if (fh.find(fold_ci(std::string(start, p - start).c_str())) == std::string::npos)
            return false;
    }
    return any;
}

bool Filter::match(const char *keywords)
{
    bool m = true;
    if (filtering)
    {
        // Match the ENGLISH keywords AND their translation (when the language table has
        // one), so a localized user can search in either language.
        const char *tk = tr(keywords);
        m = (tk == keywords) ? matches_all_tokens(keywords, q)
                             : matches_all_tokens(std::string(keywords) + " " + tk, q);
        if (m) hits++;
    }
    return m;
}

// Resolve category `c`'s marker icon to a drawable {texture, uv} — the SAME tier order as
// draw_category_icon (native map-point sprite → representative item icon → baked atlas cell), but returns
// the handle instead of drawing, so a draw-list can AddImage it at an arbitrary position (the virtual map
// canvas). Returns false if nothing resolved yet (caller draws the group-colour dot). Native tiers only
// resolve once the sprite is resident (native map opened once); the atlas tier is always available.
bool resolve_category_icon(const OverlayFrameCtx &ctx, int c, void *&tex, ImVec2 &uv0, ImVec2 &uv1)
{
    namespace wm = goblin::worldmap;
    float u0, v0, u1, v1; void *t = nullptr; bool ok = false;
    if (const char *sym = wm::category_gpu_icon_name(c))
        ok = goblin::overlay_api::native_map_point_icon_by_name(sym, t, u0, v0, u1, v1);
    if (!ok) if (int iid = wm::category_gpu_iconId(c)) ok = goblin::overlay_api::native_map_point_icon(iid, t, u0, v0, u1, v1);
    if (!ok) if (int rep = wm::category_rep_icon(c)) ok = goblin::overlay_api::native_item_icon(rep, t, u0, v0, u1, v1);
    if (ok) { tex = t; uv0 = ImVec2(u0, v0); uv1 = ImVec2(u1, v1); return true; }
    if (ctx.atlas_srv)
    {
        using namespace goblin::overlay_icons;
        if (const char *key = wm::category_icon_key(c))
            for (int i = 0; i < ICON_CELL_COUNT; ++i)
                if (std::strcmp(ICON_CELLS[i].key, key) == 0)
                {
                    const IconCell &cell = ICON_CELLS[i];
                    tex = ctx.atlas_srv;
                    uv0 = ImVec2((cell.col * CELL) / (float)ATLAS_W, (cell.row * CELL) / (float)ATLAS_H);
                    uv1 = ImVec2(((cell.col + 1) * CELL) / (float)ATLAS_W, ((cell.row + 1) * CELL) / (float)ATLAS_H);
                    return true;
                }
    }
    return false;
}

void draw_category_icon(const OverlayFrameCtx &ctx, int c, float size)
{
    namespace wm = goblin::worldmap;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    // Tiers 1-3: native runtime sprite (name symbol → numeric map-point id → representative item
    // icon), the SAME backends the renderer draws with. Resolve only once the sprite is resident.
    void *t = nullptr;
    float u0, v0, u1, v1;
    bool ok = false;
    if (const char *sym = wm::category_gpu_icon_name(c))
        ok = goblin::overlay_api::native_map_point_icon_by_name(sym, t, u0, v0, u1, v1);
    if (!ok)
        if (int iid = wm::category_gpu_iconId(c))
            ok = goblin::overlay_api::native_map_point_icon(iid, t, u0, v0, u1, v1);
    if (!ok)
        if (int rep = wm::category_rep_icon(c))
            ok = goblin::overlay_api::native_item_icon(rep, t, u0, v0, u1, v1);
    if (ok)
    {
        ImGui::Image(reinterpret_cast<ImTextureID>(t), ImVec2(size, size), ImVec2(u0, v0), ImVec2(u1, v1));
        return;
    }

    // Tier 4: baked atlas cell (transitional fallback; always resident when atlas_srv is bound).
    if (ctx.atlas_srv)
    {
        using namespace goblin::overlay_icons;
        if (const char *key = wm::category_icon_key(c))
            for (int i = 0; i < ICON_CELL_COUNT; ++i)
                if (std::strcmp(ICON_CELLS[i].key, key) == 0)
                {
                    const IconCell &cell = ICON_CELLS[i];
                    ImVec2 a((cell.col * CELL) / (float)ATLAS_W, (cell.row * CELL) / (float)ATLAS_H);
                    ImVec2 b(((cell.col + 1) * CELL) / (float)ATLAS_W, ((cell.row + 1) * CELL) / (float)ATLAS_H);
                    ImGui::Image(reinterpret_cast<ImTextureID>(ctx.atlas_srv), ImVec2(size, size), a, b);
                    return;
                }
    }

    // Tier 5: universal fallback — a filled dot in the category's group color (needs no art).
    ImGui::Dummy(ImVec2(size, size));
    dl->AddCircleFilled(ImVec2(p.x + size / 2, p.y + size / 2), size * 0.34f, wm::category_color(c));
}

void grace_candidate_gate_warning()
{
    if ((*goblin::overlay_api::cfg_graceOverlay_ptr()))
        return;
    ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.15f, 1.0f),
        "(!) Few/no candidates listed?\n"
        "    The forced MENU_MAP_* grace sprites are only created while\n"
        "    'grace_overlay' is ON. Enable it via the checkbox below (Overlay\n"
        "    graces), or set it = true in MapForGoblins.ini. Otherwise only the\n"
        "    live SB_ERR_Grace_* frame the game happens to draw will appear.");
}

bool scale_control(const char *label, float *v, float lo, float hi,
                   float step, float step_fast, const char *fmt)
{
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(150.0f);
    changed |= ImGui::SliderFloat("##slider", v, lo, hi, fmt);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::InputFloat("##input", v, step, step_fast, fmt);
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

// On-screen keyboard for gamepad text entry (dx-bugs-backlog PR C-2 part 2). ImGui's gamepad
// nav (enabled in PR C-2 part 1) has no answer for InputText, so this draws a popup made of
// ordinary buttons — D-pad/stick already moves focus between them and A already activates
// them, for free, via the SAME nav we already enabled. No custom input polling here at all.
// Writes directly into the caller's InputText buffer; next frame InputTextWithHint just
// re-renders whatever's in it, same as if the user had typed it on a real keyboard.
void draw_gamepad_keyboard_button(const char *popup_id, char *buf, size_t buf_size)
{
    static const char *const ALPHA_ROWS[] = {"ABCDEFGHIJK", "LMNOPQRSTUV", "WXYZ"};
    static const char *const QWERTY_ROWS[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    const char *const *rows = ((*goblin::overlay_api::cfg_virtualKeyboardLayout_ptr()) == 1) ? QWERTY_ROWS : ALPHA_ROWS;

    ImGui::PushID(popup_id);
    // NOT SameLine(): the InputText above is sized to GetContentRegionAvail().x (100% width),
    // so a same-line button would land past the panel's right edge — invisible without
    // scrolling. Own line instead, always visible regardless of the field's width.
    if (ImGui::SmallButton("Kbd")) ImGui::OpenPopup(popup_id);   // ASCII-only: U+2328 isn't in the merged font ranges
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("On-screen keyboard for gamepad text entry."));

    if (ImGui::BeginPopup(popup_id))
    {
        const size_t len = strlen(buf);
        for (int r = 0; r < 3; ++r)
        {
            for (const char *c = rows[r]; *c; ++c)
            {
                ImGui::PushID(static_cast<int>(c - rows[r]) + r * 100);
                char label[2] = {*c, '\0'};
                if (ImGui::Button(label) && len + 1 < buf_size)
                {
                    buf[len] = *c;
                    buf[len + 1] = '\0';
                }
                ImGui::PopID();
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        if (ImGui::Button(tr("Space")) && len + 1 < buf_size) { buf[len] = ' '; buf[len + 1] = '\0'; }
        ImGui::SameLine();
        if (ImGui::Button(tr("Backspace")) && len > 0) buf[len - 1] = '\0';
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear"))) buf[0] = '\0';
        ImGui::SameLine();
        if (ImGui::Button(tr("Done"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
} // namespace goblin::overlay::panel
