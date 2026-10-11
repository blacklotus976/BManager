#pragma once
#include <string>

// A physical building. Buildings exist on their own, independent of who happens
// to live in them -- clients merely point at an apartment inside one.
struct Building {
    int id = -1;
    std::string address;
    std::string area;
    std::string postal_code;   // ΤΚ
    std::string afm;           // building's own ΑΦΜ
    std::string created_at;
};

// One apartment (unit) inside a building. Rows exist whether or not anyone
// currently lives there, so vacant apartments are visible too.
struct Apartment {
    int id = -1;
    int building_id = -1;
    std::string label;         // e.g. "Α1", "Ισόγειο"
};

// Who lived in an apartment, and when. A client's CURRENT apartment is users.apartment_id;
// this table keeps the whole history so an apartment's past can be told even after the
// client moved. to_date is empty while the stay is still open.
struct Residency {
    int id = -1;
    int user_id = -1;
    int apartment_id = -1;
    std::string from_date;     // YYYY-MM-DD
    std::string to_date;       // YYYY-MM-DD, empty = still living there
};
