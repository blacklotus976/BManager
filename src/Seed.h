#pragma once
#include "DataConn.h"

// Seeds the realistic Greek property-management demo data set (4 buildings with
// apartments, 16 clients living in them, services, packages, payments) if the
// users table is empty. main.cpp only calls this for a brand-new database file.
// mediaDir: folder with the demo photos/videos/PDF to attach ("" = no attachments).
void seedDemoDataIfEmpty(DataConn* db, const std::string& mediaDir = "");
