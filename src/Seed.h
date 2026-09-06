#pragma once
#include "DataConn.h"

// Seeds realistic Greek property-management demo data if the users table is empty.
void seedDemoDataIfEmpty(DataConn* db);
