#include <Overlay/FontAtlas.h>

#include <algorithm>

namespace bg3se::mac::overlay {

int BoundFontAtlas(ImFontAtlas& atlas, int limit)
{
    if (limit < kLeastAtlasLimit) return 0;

    // ImGui takes the sizes of an atlas to be powers of two.
    int most = kLeastAtlasLimit;
    while (most <= limit / 2) most *= 2;

    atlas.TexMaxWidth = std::min(atlas.TexMaxWidth, most);
    atlas.TexMaxHeight = std::min(atlas.TexMaxHeight, most);
    atlas.TexMinWidth = std::min(atlas.TexMinWidth, atlas.TexMaxWidth);
    atlas.TexMinHeight = std::min(atlas.TexMinHeight, atlas.TexMaxHeight);
    return std::max(atlas.TexMaxWidth, atlas.TexMaxHeight);
}

}
