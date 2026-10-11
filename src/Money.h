#pragma once
#include <cmath>
#include <cstdio>
#include <string>

// Money is euros with exactly two decimals (0..99 cents). The DB columns stay REAL euros so the
// raw view / CSV / SQL console keep reading naturally, but every value is snapped to whole cents
// on the way in, and anything that must be exact (balances, SQL totals) is computed in integer
// cents. A cent-exact double converts back to the same cents losslessly, so nothing drifts.

inline long long ToCents(double eur) { return std::llround(eur * 100.0); }
inline double FromCents(long long cents) { return (double)cents / 100.0; }
inline double RoundMoney(double eur) { return FromCents(ToCents(eur)); }

// due - paid, done in cents so 0.10 + 0.20 - 0.30 really is 0.
inline double MoneySub(double a, double b) { return FromCents(ToCents(a) - ToCents(b)); }

// "35.00" -- the canonical text used to prefill inputs and in diffs.
inline std::string FormatMoney(double eur) {
    long long c = ToCents(eur);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%lld.%02lld", c < 0 ? "-" : "", std::llabs(c) / 100, std::llabs(c) % 100);
    return buf;
}

// Parses what a person actually types: "12", "12.5", "12,50", "1.234,56", "1,234.56", "€ 40".
// The LAST '.' or ',' is the decimal separator when followed by 1-2 digits; other separators are
// thousands grouping. More than 2 decimals, letters, or an empty field -> false (out untouched),
// so a typo can never be silently saved as a different amount.
inline bool ParseMoney(const std::string& text, double& out) {
    std::string s;
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == ' ' || ch == '\t') continue;
        if (ch == 0xE2 && i + 2 < text.size() && (unsigned char)text[i + 1] == 0x82 && (unsigned char)text[i + 2] == 0xAC) { i += 2; continue; } // €
        s += (char)ch;
    }
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; s.erase(0, 1); }
    if (s.empty()) return false;

    size_t sep = s.find_last_of(".,");
    std::string intPart = s, frac;
    if (sep != std::string::npos) {
        size_t fracLen = s.size() - sep - 1;
        // "1.234" (Greek grouping, no comma anywhere) -> 1234. "1,234" is NOT guessed: for a Greek
        // user that is a 3-decimal typo, so it is rejected like any other >2-decimal input.
        bool onlyGrouping = fracLen == 3 && s[sep] == '.' && s.find(',') == std::string::npos;
        if (fracLen > 2 && !onlyGrouping) return false;
        if (!onlyGrouping) { intPart = s.substr(0, sep); frac = s.substr(sep + 1); }
    }
    long long whole = 0;
    bool anyDigit = false;
    for (char c : intPart) {
        if (c == '.' || c == ',') continue;
        if (c < '0' || c > '9') return false;
        whole = whole * 10 + (c - '0');
        anyDigit = true;
        if (whole > 100000000000LL) return false;   // > 1e11 € is a typo, not an amount
    }
    long long cents = 0;
    for (size_t i = 0; i < frac.size(); i++) {
        if (frac[i] < '0' || frac[i] > '9') return false;
        cents = cents * 10 + (frac[i] - '0');
        anyDigit = true;
    }
    if (frac.size() == 1) cents *= 10;
    if (!anyDigit) return false;
    long long total = whole * 100 + cents;
    out = FromCents(neg ? -total : total);
    return true;
}
