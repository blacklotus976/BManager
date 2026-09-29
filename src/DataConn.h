#pragma once
#include "models/User.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Payment.h"
#include "models/Note.h"
#include <vector>
#include <string>

class DataConn {
public:
    virtual ~DataConn() = default;

    // --- Users ---
    virtual std::vector<User> searchUsers(const std::string& query, int offset, int limit) = 0;
    // Advanced multi-field search (Αναζήτηση tab): every non-empty field in `criteria`
    // becomes an AND-ed LIKE clause; empty fields are not filtered on.
    // idQuery is a free-text substring match against CAST(id AS TEXT), so "half-written ids"
    // (e.g. "5" matching id=5, 15, 52) work the same way as every other text field.
    virtual std::vector<User> advancedSearchUsers(const User& criteria, const std::string& idQuery = "") = 0;
    virtual bool addUser(const User& user) = 0;
    virtual bool editUser(int id, const User& user) = 0;
    virtual bool removeUser(int id) = 0;
    virtual User getUserById(int id) = 0;

    // --- Services ---
    // A `services` row is a generic, reusable catalog entry (e.g. "Πετρέλαιο",
    // "Καθαριότητα") created ONCE, never duplicated per-user. Which user(s) it
    // applies to lives in service_users -- one row = single owner, several rows
    // = joint/shared. Periodicity/recurrence concepts were removed per the
    // client: the app logs/registers payments (including scheduling ahead as
    // future payments); it is not the app's job to automate anything periodical.
    virtual std::vector<Service> searchServices(const std::string& query, int offset, int limit) = 0;
    virtual std::vector<Service> advancedSearchServices(const std::string& label, const std::string& extra_notes,
                                                          const std::string& idQuery = "") = 0;
    virtual bool addService(int userId, const Service& service) = 0;
    // Same insert as addService, but returns the new service id (-1 on failure) so callers
    // can chain more service_users rows onto it (multi-owner creation).
    virtual int addServiceGetId(int userId, const Service& service) = 0;
    virtual bool editService(int id, const Service& service) = 0;
    virtual bool removeService(int id) = 0;
    virtual Service getServiceById(int id) = 0;
    virtual std::vector<Service> getServicesForUser(int userId) = 0;
    virtual std::vector<User> getUsersForService(int serviceId) = 0;
    virtual bool addServiceUser(int serviceId, int userId) = 0;

    // --- Master Services (named packages grouping several real, independent
    // services) ---
    // A package is purely an organizational/viewing grouping on top of services
    // that already work exactly as they do now -- payments never change, they
    // still attach to one real (component) service + one user via the existing
    // service_id/user_id/service_users mechanism. No new payment dimension.
    virtual std::vector<MasterService> getMasterServices() = 0;
    virtual std::vector<Service> getMasterServiceComponents(int masterServiceId) = 0;
    virtual int addMasterService(const std::string& label) = 0;
    virtual bool addMasterServiceComponent(int masterServiceId, int serviceId) = 0;
    virtual bool removeMasterServiceComponent(int masterServiceId, int serviceId) = 0;
    virtual bool removeMasterService(int id) = 0;
    struct MasterServiceSummary {
        MasterService masterService;
        std::vector<Service> components;
        std::vector<std::vector<User>> componentRosters; // roster per component, via getUsersForService
    };
    virtual std::vector<MasterServiceSummary> getMasterServiceSummaries() = 0;

    // --- Payments ---
    virtual bool addPayment(const Payment& payment) = 0;
    virtual bool editPayment(int id, const Payment& payment) = 0;
    virtual bool removePayment(int id) = 0;
    virtual Payment getPaymentById(int id) = 0;
    virtual std::vector<Payment> getPaymentsForService(int serviceId) = 0;
    virtual std::vector<Payment> getPaymentsForUser(int userId) = 0;
    virtual std::vector<Payment> getFuturePayments() = 0;
    virtual std::vector<Payment> getRecentPayments(int limit) = 0;
    // All payments (past/current/future), unfiltered - backs the dashboard timeline, which
    // applies date/user/service filters in memory since they combine (AND) and change live.
    virtual std::vector<Payment> getAllPayments() = 0;
    // Advanced payment search (Αναζήτηση tab): payment's own fields plus owner-name /
    // service-label substring match via JOIN against users/services. Any empty string
    // param is not filtered on. year/month/day, idQuery, serviceIdQuery and userIdQuery are
    // all free-text substring matches against the stringified column (LIKE, not exact
    // equality) so half-written numbers behave like half-written text everywhere.
        // Advanced payment search (Αναζήτηση tab): payment's own fields plus owner-name /
    // service-label substring match via JOIN against users/services. Any empty string
    // param is not filtered on. year/month/day, idQuery, serviceIdQuery and userIdQuery are
    // all free-text substring matches against the stringified column (LIKE, not exact
    // equality) so half-written numbers behave like half-written text everywhere.
    virtual std::vector<Payment> advancedSearchPayments(const std::string& year, const std::string& month, const std::string& day,
                                                          const std::string& amount_due_query, const std::string& amount_paid_query,
                                                          const std::string& status, const std::string& direction,
                                                          const std::string& notes, const std::string& resolution_date,
                                                          const std::string& userName, const std::string& serviceLabel,
                                                          const std::string& idQuery = "", const std::string& serviceIdQuery = "",
                                                          const std::string& userIdQuery = "") = 0;

    // --- Paged "phone book" pickers (dashboard filters) -------------------
    // These fetch only one page of rows at a time -- the dashboard pickers
    // never load the full users/services table just to render a filter row.
    //
    // pickerUsers:
    //   letterFilter  "" = all rows; "Α" = only names whose first char is Α
    //                 (uses LIKE '<letter>%'). Matches how the rest of the app
    //                 treats Greek text -- caller passes the exact letter form
    //                 that appears in stored data).
    //   nameSearch    substring match anywhere in full_name (LOWER-matched).
    //   reverseSort   true = Z→A, false = A→Z.
    // letters: set of starting-letter prefixes to include (OR-ed together).
    //          Empty = no letter filtering (all users).
    //          e.g. {"Α","Π"} matches names starting with Α or Π.
    virtual std::vector<User> pickerUsers(const std::vector<std::string>& letters,
                                           const std::string& nameSearch,
                                           int offset, int limit,
                                           bool reverseSort) = 0;
    virtual int pickerUsersCount(const std::vector<std::string>& letters,
                                  const std::string& nameSearch) = 0;

    // pickerServices: name substring match, always alphabetical by label.
    virtual std::vector<Service> pickerServices(const std::string& nameSearch,
                                                 int offset, int limit) = 0;
    virtual int pickerServicesCount(const std::string& nameSearch) = 0;

    // --- Timeline / Audit ---
    // category: "business" for normal-app-flow CRUD on users/services/payments/service_users,
    // "db" for the raw SQL console (and anything else that isn't a business action).
    virtual std::vector<std::string> getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                          const std::string& category = "", bool friendly = false) = 0;
    virtual void logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category = "db") = 0;

    // --- Metrics ---
    virtual double getTotalPaid(int year, int month, int userId = -1, int serviceId = -1) = 0;
    virtual double getTotalPending(int year, int month, int userId = -1, int serviceId = -1) = 0;

    // --- Notes (standalone scratchpad, not tied to users/services/payments) ---
    virtual std::vector<Note> getAllNotes() = 0;
    virtual bool addNote(const Note& note) = 0;
    virtual bool editNote(int id, const Note& note) = 0;
    virtual bool removeNote(int id) = 0;

    // --- Backup ---
    virtual bool exportAllTablesToCSV(const std::string& folder) = 0;

    // --- Raw View (paginated) ---
    virtual std::vector<std::vector<std::string>> getPaginatedData(const std::string& table, int offset, int limit) = 0;
    virtual std::vector<std::string> getTableColumns(const std::string& table) = 0;
    virtual int getRowCount(const std::string& table) = 0;

    struct JointServiceSummary {
        Service service;
        std::vector<User> users;
        std::vector<double> userTotalPaid;   // per user, total paid for this service (any direction)
        std::vector<double> userTotalDue;    // per user, total due for this service (any direction)
        double totalDue, totalPaid;
        // Direction-aware net, e.g. a bill paid on the clients' behalf (direction='out')
        // then collected back from them (direction='in') -- what should trend to zero.
        std::vector<double> userNet;         // per user: paidOut - collected
        double totalNet = 0.0;
    };
    virtual std::vector<JointServiceSummary> getJointServices() = 0;

    // --- Raw SQL console (Settings screen power-user feature) ---
    virtual bool executeRawQuery(const std::string& sql, std::vector<std::string>& outColumns,
                                  std::vector<std::vector<std::string>>& outRows, std::string& outError) = 0;

};
