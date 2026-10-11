#pragma once
#include <string>

// A file (photo, video, PDF, ...) attached to a record. The bytes live in <db folder>/attachments/, named by their
// SHA-256 (content-addressed: identical files are stored once and any change to a file is detectable); the
// database keeps this row. Attachments are never deleted: removing one hides it, with a reason, and keeps it in
// the history. Attachments of receipts can't be removed at all (they are the evidence).
struct Attachment {
    int id = -1;
    std::string entity;          // receipt | payment | charge | client | building | apartment
    int entity_id = -1;
    std::string sha256;
    std::string file_name;       // original name, for display / export
    std::string kind;            // image | video | pdf | other
    long long size_bytes = 0;
    std::string stored_path;     // relative to the database folder
    std::string note;
    std::string created_at;      // local time
    std::string removed_at;      // "" = visible
    std::string removed_reason;
};
