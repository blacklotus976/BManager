// Standalone checks for Money.h: g++ -std=c++17 -I../src money_test.cpp && ./a.out
#include "Money.h"
#include <cstdio>

static int fails = 0;
static void parses(const char* in, long long cents) {
    double v = -1;
    if (!ParseMoney(in, v) || ToCents(v) != cents) { std::printf("FAIL parse \"%s\" -> %s %.4f (want %lld)\n", in, ParseMoney(in, v) ? "ok" : "rejected", v, cents); fails++; }
}
static void rejects(const char* in) {
    double v = 0;
    if (ParseMoney(in, v)) { std::printf("FAIL accepted \"%s\" -> %.4f\n", in, v); fails++; }
}
static void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); fails++; } }

int main() {
    parses("12", 1200);        parses("12.5", 1250);      parses("12,5", 1250);
    parses("12,50", 1250);     parses("0,99", 99);        parses("0.01", 1);
    parses("1.234,56", 123456); parses("1,234.56", 123456); parses("1.234", 123400);
    parses("€ 40", 4000);      parses("40€", 4000);       parses(" 7 ", 700);
    parses("-15,30", -1530);   parses(",5", 50);
    rejects("");  rejects("abc");  rejects("12.345,678");  rejects("1,999");  rejects("12.3.4x"); rejects("€");
    rejects("12,555");

    check(RoundMoney(182.52499999999998) == 182.52, "round 182.524999");
    check(ToCents(0.1 + 0.2) == 30, "0.1+0.2 -> 30 cents");
    check(MoneySub(0.30, 0.1 + 0.2) == 0.0, "0.30 - (0.1+0.2) == 0");
    check(FormatMoney(35) == "35.00", "format 35");
    check(FormatMoney(0.5) == "0.50", "format 0.5");
    check(FormatMoney(-3.05) == "-3.05", "format -3.05");
    double sum = 0; for (int i = 0; i < 1000; i++) sum += RoundMoney(0.10);
    check(ToCents(sum) == 10000, "1000 x 0.10 == 100.00");

    std::printf(fails ? "%d FAILED\n" : "all money tests passed\n", fails);
    return fails ? 1 : 0;
}
