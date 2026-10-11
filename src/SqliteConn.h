#pragma once
#include "DataConn.h"
#include <sqlite3.h>
#include <string>
#include <map>

class SqliteConn : public DataConn {
private:
    sqlite3* db = nullptr;
    std::map<std::string, std::vector<std::string>> greekHeaders;

    bool executeQuery(const std::string& sql);
    std::string lastError_;
    std::string dbPath_;
    int apartmentAt(int userId, int y, int m, int d);
    double priceOf(int serviceId);
    std::string toJson(const std::vector<std::string>& cols, const std::vector<std::string>& vals);
    std::vector<std::string> getColumnNames(const std::string& table);

public:
    SqliteConn(const std::string& dbPath);
    ~SqliteConn();

    // All interface methods...
    std::vector<User> searchUsers(const std::string& query, int offset, int limit) override;
    std::vector<User> advancedSearchUsers(const User& criteria, const std::string& idQuery = "") override;
    bool addUser(const User& user) override;
    bool editUser(int id, const User& user) override;
    bool removeUser(int id) override;
    User getUserById(int id) override;

    std::vector<Building> getBuildings() override;
    Building getBuildingById(int id) override;
    int addBuilding(const Building& building) override;
    bool editBuilding(int id, const Building& building) override;
    bool removeBuilding(int id) override;
    std::vector<Apartment> getApartmentsForBuilding(int buildingId) override;
    std::vector<Residency> getResidencies(int apartmentId) override;
    std::vector<Residency> getUserResidencies(int userId) override;
    bool setResidencyHistory(int userId, const std::vector<Residency>& history) override;
    std::vector<Payment> getPaymentsForApartment(int apartmentId) override;
    int getApartmentOnDate(int userId, int y, int m, int d) override { return apartmentAt(userId, y, m, d); }
    std::string takeLastError() override;
    // Once a day: copy the database to <db folder>/backups/ (keeps the newest `keep`).
    void autoBackup(int keep = 14);
    int addApartment(int buildingId, const std::string& label) override;
    bool editApartment(int id, const std::string& label) override;
    bool removeApartment(int id) override;

    std::vector<Service> searchServices(const std::string& query, int offset, int limit) override;
    std::vector<Service> advancedSearchServices(const std::string& label, const std::string& extra_notes,
                                                 const std::string& idQuery = "") override;
    bool addService(int userId, const Service& service) override;
    int addServiceGetId(int userId, const Service& service) override;
    bool editService(int id, const Service& service) override;
    bool removeService(int id) override;
    Service getServiceById(int id) override;
    std::vector<Service> getServicesForUser(int userId) override;
    std::vector<User> getUsersForService(int serviceId) override;
    bool addServiceUser(int serviceId, int userId) override;

    std::vector<MasterService> getMasterServices() override;
    std::vector<Service> getMasterServiceComponents(int masterServiceId) override;
    int addMasterService(const std::string& label) override;
    bool addMasterServiceComponent(int masterServiceId, int serviceId) override;
    bool removeMasterServiceComponent(int masterServiceId, int serviceId) override;
    bool removeMasterService(int id) override;
    std::vector<DataConn::MasterServiceSummary> getMasterServiceSummaries() override;

    bool addPayment(const Payment& payment) override;
    bool editPayment(int id, const Payment& payment) override;
    bool removePayment(int id) override;
    Payment getPaymentById(int id) override;
    std::vector<Payment> getPaymentsForService(int serviceId) override;
    std::vector<Payment> getPaymentsForUser(int userId) override;
    std::vector<Payment> getFuturePayments() override;
    std::vector<Payment> getRecentPayments(int limit) override;
    std::vector<Payment> getAllPayments() override;
    std::vector<Payment> advancedSearchPayments(const std::string& year, const std::string& month, const std::string& day,
                                                 const std::string& amount_due_query, const std::string& amount_paid_query,
                                                 const std::string& status, const std::string& direction,
                                                 const std::string& notes, const std::string& resolution_date,
                                                 const std::string& userName, const std::string& serviceLabel,
                                                 const std::string& idQuery = "", const std::string& serviceIdQuery = "",
                                                 const std::string& userIdQuery = "") override;

    std::vector<std::string> getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                 const std::string& category = "", bool friendly = false) override;
    void logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category = "db") override;

    double getTotalPaid(int year, int month, int userId = -1, int serviceId = -1) override;
    double getTotalPending(int year, int month, int userId = -1, int serviceId = -1) override;

    std::vector<Note> getAllNotes() override;
    bool addNote(const Note& note) override;
    bool editNote(int id, const Note& note) override;
    bool removeNote(int id) override;

    bool exportAllTablesToCSV(const std::string& folder) override;

    std::vector<std::vector<std::string>> getPaginatedData(const std::string& table, int offset, int limit) override;
    std::vector<std::string> getTableColumns(const std::string& table) override;
    int getRowCount(const std::string& table) override;



    std::vector<DataConn::JointServiceSummary> getJointServices() override;

    bool executeRawQuery(const std::string& sql, std::vector<std::string>& outColumns,
                          std::vector<std::vector<std::string>>& outRows, std::string& outError) override;

    std::vector<User> pageUsers(const std::string& query, int scope,
                                 const std::vector<std::string>& letters,
                                 int offset, int limit, const SortSpec& sort = {}) override;
    int countUsers(const std::string& query, int scope,
                    const std::vector<std::string>& letters) override;
    std::vector<Service> pageServices(const std::string& query, int offset, int limit,
                                       const SortSpec& sort = {}) override;
    int countServices(const std::string& query) override;
    std::vector<Payment> pagePayments(const std::string& startDate, const std::string& endDate,
                                       int userId, int serviceId,
                                       const std::string& status, const std::string& direction,
                                       int offset, int limit, const SortSpec& sort = {}) override;
    int countPayments(const std::string& startDate, const std::string& endDate,
                       int userId, int serviceId,
                       const std::string& status, const std::string& direction) override;

    int issueReceipt(const Receipt& r, const std::vector<int>& paymentIds) override;
    bool voidReceipt(int id, const std::string& reason) override;
    Receipt getReceiptById(int id) override;
    std::vector<Receipt> getReceipts(int offset, int limit) override;
    std::vector<Receipt> getReceiptsForUser(int userId) override;
    std::vector<int> getReceiptPaymentIds(int receiptId) override;
    int getReceiptIdForPayment(int paymentId) override;

    // Paged phone-book pickers (dashboard filters) -- see DataConn.h for contract.
    std::vector<User> pickerUsers(const std::vector<std::string>& letters,
                                   const std::string& nameSearch,
                                   int offset, int limit,
                                   bool reverseSort) override;
    int pickerUsersCount(const std::vector<std::string>& letters,
                          const std::string& nameSearch) override;
    std::vector<Service> pickerServices(const std::string& nameSearch,
                                         int offset, int limit) override;
    int pickerServicesCount(const std::string& nameSearch) override;
};
