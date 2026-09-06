#pragma once
#include <string>

struct Note {
    int id = -1;
    std::string title;
    std::string content;
    std::string created_at;
};
