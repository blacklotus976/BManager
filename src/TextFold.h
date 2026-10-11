#pragma once
#include <cctype>
#include <string>

// Case- and accent-insensitive form of a string for searching: ASCII lowercased, Greek uppercased
// with tonos/dialytika removed (ά/Ά -> Α, final ς -> Σ). Byte lengths are preserved, so match
// positions in the folded text map back to the original (used for highlighting).
// Shared by the UI and the database (registered as the SQL function fold()).
inline std::string FoldGreek(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp; int len;
        if (c < 0x80) { out += (char)std::tolower(c); i++; continue; }
        else if ((c >> 5) == 0x6 && i + 1 < in.size()) { cp = ((c & 0x1F) << 6) | ((unsigned char)in[i+1] & 0x3F); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < in.size()) { cp = ((c & 0x0F) << 12) | (((unsigned char)in[i+1] & 0x3F) << 6) | ((unsigned char)in[i+2] & 0x3F); len = 3; }
        else { out += (char)c; i++; continue; }
        unsigned m = cp;
        if (cp >= 0x3B1 && cp <= 0x3C9) m = (cp == 0x3C2) ? 0x3A3 : cp - 0x20;          // lower -> upper, final sigma
        else switch (cp) {
            case 0x386: case 0x3AC: m = 0x391; break;                                      // Ά ά
            case 0x388: case 0x3AD: m = 0x395; break;                                      // Έ έ
            case 0x389: case 0x3AE: m = 0x397; break;                                      // Ή ή
            case 0x38A: case 0x3AF: case 0x3AA: case 0x3CA: case 0x390: m = 0x399; break; // Ί ί Ϊ ϊ ΐ
            case 0x38C: case 0x3CC: m = 0x39F; break;                                      // Ό ό
            case 0x38E: case 0x3CD: case 0x3AB: case 0x3CB: case 0x3B0: m = 0x3A5; break; // Ύ ύ Ϋ ϋ ΰ
            case 0x38F: case 0x3CE: m = 0x3A9; break;                                      // Ώ ώ
            default: break;
        }
        if (m < 0x80) out += (char)m;
        else if (m < 0x800) { out += (char)(0xC0 | (m >> 6)); out += (char)(0x80 | (m & 0x3F)); }
        else { out += (char)(0xE0 | (m >> 12)); out += (char)(0x80 | ((m >> 6) & 0x3F)); out += (char)(0x80 | (m & 0x3F)); }
        i += len;
    }
    return out;
}
