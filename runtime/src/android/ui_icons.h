#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Official artwork for the touch controls from the player's game files (ui_icons.cpp)
namespace ui_icons {
struct Request {
    std::string layout;   // layout archive in the 2D pack, without ".szs" (e.g. "BtnItemIcon_00")
    std::string texture;  // texture in its timg/ folder, without ".bflim" (e.g. "Icon128_21^l")
};
// writes outDir/<texture>.rgba for each texture found; returns how many
int extract(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests);

// The game's icon font (CKingPic.bffnt: button glyphs at U+E000 and up, one RGBA8 sheet), for the
// controller button style mod (ButtonStyle.java). font_cells: the cells of `codes` as RGBA8, cell
// after cell (cw x ch each, top row first); font_patch: writes such cells back into the font, in
// place (the file keeps its size; a text font's redrawn characters get the whole cell as their
// width). Both false if the font isn't the expected kind.
bool font_cells(const uint8_t* font, size_t size, const std::vector<uint16_t>& codes, std::vector<uint8_t>& rgba,
                uint32_t& cw, uint32_t& ch);
bool font_patch(uint8_t* font, size_t size, const std::vector<uint16_t>& codes, const uint8_t* rgba, size_t rgbaSize);
}  // namespace ui_icons
