#pragma once
#include <string>

// Συνεργάτης: a person or company who does jobs for the business (a plumber, an electrician, a cleaner...).
// A plain contact list for now -- not connected to payments yet.
struct Partner {
    int id = -1;
    std::string name;
    std::string trade;        // what they do ("υδραυλικός", "ηλεκτρολόγος", ...)
    std::string phone;
    std::string email;
    std::string afm;
    std::string notes;
    bool active = true;
    std::string created_at;
};
