// Single translation unit that compiles the stb_image implementation. The overlay
// decodes the embedded PNG icon atlas (goblin_overlay_icons) from memory at load.
// PNG-only, no stdio (we never touch the filesystem) to keep the build lean.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
