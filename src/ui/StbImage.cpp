// The single translation unit that compiles stb_image (public domain / MIT, third_party/stb).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8   // file names are UTF-8 (Greek folder names on Windows)
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
