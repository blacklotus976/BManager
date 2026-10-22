#pragma once
#include <string>

// Money that actually moved (table `payments`): a client paid us (direction "in") or we paid a third party on a
// client's behalf ("out"). What it pays for is recorded separately as allocations to charges (bills), so one
// transfer can settle several bills and one bill can be paid in instalments. A receipt is issued for money.
struct MoneyPayment {
    int id = -1;
    int user_id = -1;
    std::string pay_date;        // YYYY-MM-DD
    double amount = 0.0;         // euros, whole cents, > 0
    std::string direction = "in";
    std::string method = "unspecified";   // unspecified | cash | collector | bank | ebanking | card | iris | other
    std::string reference;       // bank transaction id etc.
    std::string notes;
    int receipt_id = -1;
    int batch_id = -1;
    std::string created_at;      // local time
    double allocated = 0.0;      // derived: how much of it settles charges; amount - allocated = client credit
    std::string who, what;       // read-only (Ταμείο): client or partner, and what it paid for
};

// How much of a payment goes to one charge.
struct Allocation {
    int payment_id = -1;
    int charge_id = -1;
    double amount = 0.0;
};
