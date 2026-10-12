#pragma once
#include <string>

// A discount offer (προσφορά). It never changes catalog prices: when a matching charge is created the person can
// claim it (the charge is saved at the discounted amount and an offer_uses row keeps list price + discount) or ignore it.
struct Offer {
    int id = -1;
    std::string name;
    std::string kind = "percent";   // percent | fixed (euros off)
    double value = 0.0;             // 10 = 10% or 10.00 €
    int user_id = -1;               // -1 = every client
    std::string target_type = "all";// all | service | package
    int target_id = -1;
    std::string valid_from, valid_to;   // YYYY-MM-DD, inclusive
    std::string status = "active";  // active | cancelled
    std::string note, cancel_reason, created_at;
    int uses = 0;                   // read-only: how many charges used it
    double given = 0.0;             // read-only: total discount given
};

// Discount for a list price (cents exact, never more than the price).
inline double OfferDiscount(const Offer& o, double listPrice) {
    long long list = (long long)(listPrice * 100.0 + (listPrice >= 0 ? 0.5 : -0.5));
    long long d = o.kind == "percent" ? (long long)(list * o.value / 100.0 + 0.5) : (long long)(o.value * 100.0 + 0.5);
    if (d > list) d = list;
    if (d < 0) d = 0;
    return d / 100.0;
}
