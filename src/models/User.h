#pragma once
#include <string>

struct User {
    int id = -1;
    std::string full_name;
    std::string phone;
    // Client's own legacy numbering/coding scheme -- a free-text second key that
    // doesn't align with our DB `id`, plays no other role beyond being searchable.
    std::string special_code;
    std::string created_at;

    // The ONLY stored link to where this client lives: an apartment (-1 = none).
    // A client carries no address of their own any more -- location comes from
    // the apartment, and the apartment belongs to a building.
    int apartment_id = -1;

    // ---- DERIVED, read-only ------------------------------------------------
    // Filled in by the SELECT's JOIN (users -> apartments -> buildings) so every
    // screen can keep showing "where they live" without a second query. Never
    // stored on the users table and ignored by addUser/editUser.
    int building_id = -1;
    std::string address;
    std::string area;
    std::string postal_code;
    std::string apartment;     // the apartment's label, e.g. "Α1"
};
