#pragma once
#include <string>
#include <cctype>

// ISO 11649 Structured Creditor Reference ("RF" code), the reference Greek e-banking uses to match
// a transfer to what it pays: "RF" + 2 check digits + up to 21 letters/digits.
// Check: move "RF"+check to the end, letters -> 10..35, the number mod 97 must be 1.

namespace rf_detail {
inline int Mod97(const std::string& s) {   // s: letters/digits, letters expanded to 2 digits
    int m = 0;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) m = (m * 10 + (c - '0')) % 97;
        else { int v = std::toupper((unsigned char)c) - 'A' + 10; m = (m * 100 + v) % 97; }
    }
    return m;
}
inline std::string Clean(const std::string& in) {
    std::string out;
    for (char c : in) if (std::isalnum((unsigned char)c)) out += (char)std::toupper((unsigned char)c);
    return out;
}
}

// "A000123" -> "RF..A000123". Returns "" if the reference is empty, too long or not alphanumeric.
inline std::string MakeRfCode(const std::string& reference) {
    std::string ref = rf_detail::Clean(reference);
    if (ref.empty() || ref.size() > 21) return "";
    int check = 98 - rf_detail::Mod97(ref + "RF00");
    char buf[4]; buf[0] = (char)('0' + check / 10); buf[1] = (char)('0' + check % 10); buf[2] = 0;
    return "RF" + std::string(buf) + ref;
}

// Accepts spaces/lowercase ("rf18 5390 0754 7034"). Returns false for anything malformed.
inline bool IsValidRfCode(const std::string& code) {
    std::string c = rf_detail::Clean(code);
    if (c.size() < 5 || c.size() > 25 || c[0] != 'R' || c[1] != 'F') return false;
    if (!std::isdigit((unsigned char)c[2]) || !std::isdigit((unsigned char)c[3])) return false;
    for (char ch : c) if (!std::isalnum((unsigned char)ch)) return false;
    return rf_detail::Mod97(c.substr(4) + c.substr(0, 4)) == 1;
}

// Canonical stored form: uppercase, no spaces.
inline std::string NormalizeRfCode(const std::string& code) { return rf_detail::Clean(code); }
