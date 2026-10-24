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

    // What kind of client: "person" (ιδιώτης), "building" (πολυκατοικία: phone/email are the διαχειριστής's) or
    // "business" (επιχείρηση). A building client is tied to its building (client_building_id), whose apartments and
    // residents it owns; it is created with the building and follows its address and ΑΦΜ.
    std::string kind = "person";
    std::string email;
    std::string afm;                 // optional for people, set for buildings
    int client_building_id = -1;
    // The accounting record of a resident (Διαχείριση): his shares, payments and receipts hang on it. It is never a
    // client: not listed, not searched, not rated. The resident himself (stays, contact) is a row of `residents`.
    bool resident = false;

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
