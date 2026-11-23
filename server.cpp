// ===================================================================
// SMART SHIELD - REST API SERVER  (integration layer, new file)
//
// Connects the web frontend (../frontend) to the existing C++ fraud
// engine. It implements the REST contract the frontend already uses
// (frontend/js/api/client.js, "REST CONTRACT v1.1") on top of the
// unchanged backend modules:
//
//   PendingTransactionManager  start -> OTP -> commit/cancel
//   hashmodule                 cards.csv
//   transaction_history        transactions.csv
//   dequemodule                review_queue.csv (fraud review deque)
//   locationgraph              routes + card travel trail
//
// The fraud scoring, OTP rules, expiry, persistence and retry logic all
// stay in the backend modules. This file only parses requests, checks
// access, and converts the backend structs to JSON.
//
// Things the frontend expects that the backend never had, and that live
// here (REST layer) instead:
//   - login + sessions        users.csv          (demo logins)
//   - card ownership / expiry card_owners.csv    (who sees which card)
//   - server-allocated transaction IDs (id omitted in /transactions/start)
//   - Idempotency-Key replay for /transactions/start
//
// Run it from the backend folder (the modules use relative CSV paths):
//     ./smartshield_server [--port 8080] [--host 127.0.0.1] [--frontend ../frontend]
// then open http://localhost:8080
// ===================================================================

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "third_party/httplib.h"
#include "third_party/json.hpp"

#include "atomic_file.h"
#include "dequemodule.h"
#include "fraudmodule.h"
#include "hashmodule.h"
#include "latlong.h"
#include "locationgraph.h"
#include "transaction.h"
#include "transaction_history.h"
#include "transaction_service.h"

using namespace std;
using json = nlohmann::ordered_json;

// ------------------------------------------------------------------
// Constants of the REST layer
// ------------------------------------------------------------------

static const char*  USERS_FILE        = "users.csv";
static const char*  CARD_OWNERS_FILE  = "card_owners.csv";
static const double MAX_AMOUNT        = 999999999.99;   // largest amount the API accepts
static const int    AMOUNT_DECIMALS   = 2;
static const int    ID_ALLOC_RETRIES  = 5;
static const auto   IDEMPOTENCY_RETENTION = chrono::hours(24);
static const auto   SESSION_LIFETIME      = chrono::hours(12);
static const size_t MAX_TEXT_LENGTH   = 64;              // merchant / device id

// ------------------------------------------------------------------
// Shared state
//
// The backend modules are not thread-safe (global card hash table, the
// deque, read-modify-write of CSV files), while cpp-httplib serves
// requests on a thread pool. Every handler therefore runs under g_store.
// PendingTransactionManager has its own mutex; it is always taken after
// g_store, so the lock order is fixed and cannot deadlock.
// ------------------------------------------------------------------

static mutex                     g_store;
static PendingTransactionManager g_pending;
static TransactionDeque          g_queue;
static LocationGraph             g_graph;

struct User
{
    string username, password, id, name, role;   // role: customer / analyst
    bool isAnalyst() const { return role == "analyst"; }
};

struct CardMeta
{
    string owner;        // user id
    string activeUntil;  // YYYY-MM-DD or ""
};

struct Session
{
    string userId;
    chrono::steady_clock::time_point expiresAt;
};

struct IdemEntry
{
    string fingerprint;
    json   response;                               // the Started answer
    chrono::steady_clock::time_point at;
};

static vector<User>          g_users;
static map<int, CardMeta>    g_cardMeta;           // CardID -> owner / expiry
static map<string, Session>  g_sessions;           // token -> session
static map<string, string>   g_pendingOwner;       // pendingId -> user id
static map<string, IdemEntry> g_idem;              // "userId|key" -> entry
static int                   g_idHighWater = 0;    // highest ID handed out

// ------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------

static string trim(const string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Same rule as the console's readText(): characters that are structural
// in the CSV files (',' and '|') or break a row are turned into spaces.
static string cleanText(string s)
{
    for (char& c : s)
        if (c == ',' || c == '|' || c == '\r' || c == '\n' || c == '\t') c = ' ';
    return trim(s);
}

static string lowerCase(string s)
{
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// "active" -> "Active". cards.csv is hand-editable, so case is not reliable.
static string titleCase(const string& s)
{
    string out = lowerCase(trim(s));
    if (!out.empty()) out[0] = static_cast<char>(toupper(static_cast<unsigned char>(out[0])));
    return out;
}

static string randomHex(int bytes)
{
    static const char* hex = "0123456789abcdef";
    random_device rd;
    string out;
    for (int i = 0; i < bytes; i++)
    {
        unsigned v = rd() & 0xFF;
        out += hex[v >> 4];
        out += hex[v & 0xF];
    }
    return out;
}

static vector<string> splitCsvLine(const string& line, size_t fields)
{
    vector<string> out;
    stringstream ss(line);
    string f;
    for (size_t i = 0; i < fields; i++)
    {
        if (!getline(ss, f, ',')) f.clear();
        out.push_back(trim(f));
    }
    return out;
}

static bool validDate(const string& s)
{
    int y, m, d;
    char extra;
    if (s.size() != 10 || sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &extra) != 3) return false;
    if (y < 1900 || m < 1 || m > 12 || d < 1) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = days[m - 1] + ((m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) ? 1 : 0);
    return d <= maxDay;
}

// ------------------------------------------------------------------
// users.csv / card_owners.csv
// ------------------------------------------------------------------

static bool loadUsers()
{
    g_users.clear();
    ifstream in(USERS_FILE);
    if (!in.is_open()) return false;

    string line;
    getline(in, line);                                   // header
    while (getline(in, line))
    {
        if (trim(line).empty()) continue;
        vector<string> f = splitCsvLine(line, 5);
        User u{lowerCase(f[0]), f[1], f[2], f[3], lowerCase(f[4])};
        if (u.username.empty() || u.id.empty()) continue;
        if (u.role != "analyst") u.role = "customer";
        g_users.push_back(u);
    }
    return !g_users.empty();
}

static void loadCardOwners()
{
    g_cardMeta.clear();
    ifstream in(CARD_OWNERS_FILE);
    if (!in.is_open()) return;                           // no owners yet

    string line;
    getline(in, line);                                   // header
    while (getline(in, line))
    {
        if (trim(line).empty()) continue;
        vector<string> f = splitCsvLine(line, 3);
        try { g_cardMeta[stoi(f[0])] = CardMeta{f[1], f[2]}; }
        catch (...) { cout << "Warning: invalid card_owners record skipped.\n"; }
    }
}

static bool saveCardOwners(const map<int, CardMeta>& meta)
{
    ostringstream out;
    out << "CardID,OwnerUserID,ActiveUntil\n";
    for (const auto& kv : meta)
        out << kv.first << ',' << kv.second.owner << ',' << kv.second.activeUntil << '\n';
    return writeFileAtomically(CARD_OWNERS_FILE, out.str());
}

// ------------------------------------------------------------------
// Errors + JSON responses
// ------------------------------------------------------------------

struct ApiError
{
    int    status;
    string code, message, field;
};

[[noreturn]] static void fail(int status, const string& code, const string& message,
                              const string& field = "")
{
    throw ApiError{status, code, message, field};
}

static void sendJson(httplib::Response& res, const json& body, int status = 200)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static void sendError(httplib::Response& res, const ApiError& e)
{
    json err = {{"code", e.code}, {"message", e.message}};
    if (!e.field.empty()) err["field"] = e.field;
    sendJson(res, json{{"error", err}}, e.status);
}

static json parseBody(const httplib::Request& req)
{
    if (trim(req.body).empty()) return json::object();
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        fail(400, "INVALID_REQUEST", "Request body must be a JSON object");
    return body;
}

// ------------------------------------------------------------------
// Authentication + visibility
// ------------------------------------------------------------------

static const User* findUserById(const string& id)
{
    for (const User& u : g_users) if (u.id == id) return &u;
    return nullptr;
}

static const User& requireUser(const httplib::Request& req)
{
    const auto now = chrono::steady_clock::now();
    for (auto it = g_sessions.begin(); it != g_sessions.end(); )
        it = (now >= it->second.expiresAt) ? g_sessions.erase(it) : next(it);

    string auth = req.get_header_value("Authorization");
    const string prefix = "Bearer ";
    if (auth.compare(0, prefix.size(), prefix) == 0)
    {
        auto it = g_sessions.find(auth.substr(prefix.size()));
        if (it != g_sessions.end())
            if (const User* u = findUserById(it->second.userId)) return *u;
    }
    fail(401, "UNAUTHORIZED", "Please sign in again");
}

static void requireAnalyst(const User& u)
{
    if (!u.isAnalyst()) fail(403, "FORBIDDEN", "Analysts only");
}

// Analysts see every card; customers only the cards they own.
static vector<Card> cardsFor(const User& u)
{
    vector<Card> out;
    for (const Card& c : getAllCards())
    {
        auto m = g_cardMeta.find(c.cardNo);
        if (u.isAnalyst() || (m != g_cardMeta.end() && m->second.owner == u.id))
            out.push_back(c);
    }
    sort(out.begin(), out.end(), [](const Card& a, const Card& b) { return a.cardNo < b.cardNo; });
    return out;
}

static bool canSeeCard(const User& u, int cardNo, Card* card = nullptr)
{
    for (const Card& c : cardsFor(u))
        if (c.cardNo == cardNo) { if (card) *card = c; return true; }
    return false;
}

// ------------------------------------------------------------------
// Backend struct -> JSON (field names = C++ member names, as the
// frontend contract specifies)
// ------------------------------------------------------------------

static json cardJson(const Card& c)
{
    json j = {{"id", c.cardNo}, {"holder", c.name}, {"type", titleCase(c.accountType)},
              {"status", titleCase(c.status)}, {"points", c.points}};
    auto m = g_cardMeta.find(c.cardNo);
    if (m != g_cardMeta.end() && !m->second.activeUntil.empty()) j["exp"] = m->second.activeUntil;
    return j;
}

static json txnJson(const Transaction& t, bool withDistance)
{
    json j = {{"id", t.id}, {"cardNo", t.cardNo}, {"amount", t.amount},
              {"merchant", t.merchant}, {"location", t.location},
              {"timestamp", t.timestamp}, {"deviceId", t.deviceId}};
    if (withDistance) j["distanceFromPrevious"] = t.distanceFromPrevious;
    j["prelimScore"] = t.prelimScore;
    j["finalScore"] = t.finalScore;
    j["otpRequired"] = t.otpRequired;
    j["otpStatus"] = t.otpStatus;
    j["authStatus"] = t.authStatus;
    j["failedAuthAttempts"] = t.failedAuthAttempts;
    j["fraudStatus"] = t.fraudStatus;
    j["decision"] = t.decision;
    j["blocked"] = t.blocked;
    j["reasons"] = t.reasons;                       // ';'-joined, split by client.js
    return j;
}

static json analysisJson(const FraudAnalysis& a)
{
    return json{
        {"hasHistoricalAverage", a.hasHistoricalAverage},
        {"previousCompletedCount", a.previousCompletedCount},
        {"historicalAverage", a.historicalAverage},
        {"spendingAnomaly", a.spendingAnomaly},
        {"recentTransactionCount", a.recentTransactionCount},
        {"rapidFrequency", a.rapidFrequency},
        {"hasPreviousLocation", a.hasPreviousLocation},
        {"distanceKm", a.distanceKm},
        {"hoursElapsed", a.hoursElapsed},
        {"speedKmh", a.speedKmh},
        {"impossibleTravel", a.impossibleTravel},
        {"hasDeviceHistory", a.hasDeviceHistory},
        {"newDevice", a.newDevice},
        {"hasMerchantHistory", a.hasMerchantHistory},
        {"newMerchant", a.newMerchant},
        {"failedAuthAttempts", a.failedAuthAttempts},
        {"repeatedAuthFailure", a.repeatedAuthFailure},
        {"wrongOtp", a.wrongOtp},
        {"score", a.score},
        {"rawScore", a.rawScore},
        {"status", a.status},
        {"reasons", a.reasons}};
}

static json queueItemJson(const QueueItem& q)
{
    return json{{"id", q.id}, {"cardNo", q.cardNo}, {"amount", q.amount},
                {"score", q.score}, {"status", q.status}, {"timestamp", q.timestamp}};
}

static json outcomeJson(const TransactionOutcome& o)
{
    json j = {{"txn", txnJson(o.txn, true)}, {"finalAnalysis", analysisJson(o.finalAnalysis)},
              {"oldPoints", o.oldPoints}, {"newPoints", o.newPoints},
              {"pointsSaved", o.pointsSaved}, {"locationSaved", o.locationSaved},
              {"transactionSaved", o.transactionSaved}, {"cardHistorySaved", o.cardHistorySaved},
              {"queueSaved", o.queueSaved}, {"fullySaved", o.fullySaved},
              {"enqueued", o.enqueued}, {"queuedAtFront", o.queuedAtFront},
              {"evicted", o.evicted}};
    if (o.evicted) j["dropped"] = queueItemJson(o.dropped);
    return j;
}

static const char* statusName(PendingStatus s)
{
    switch (s)
    {
        case PendingStatus::Started:                return "Started";
        case PendingStatus::DuplicateTransactionId: return "DuplicateTransactionId";
        case PendingStatus::CardLocked:             return "CardLocked";
        case PendingStatus::NotFound:               return "NotFound";
        case PendingStatus::Expired:                return "Expired";
        case PendingStatus::OtpNotRequired:         return "OtpNotRequired";
        case PendingStatus::OtpRequired:            return "OtpRequired";
        case PendingStatus::WrongOtp:               return "WrongOtp";
        case PendingStatus::Committed:              return "Committed";
        case PendingStatus::AttemptsExhausted:      return "AttemptsExhausted";
        case PendingStatus::Cancelled:              return "Cancelled";
        case PendingStatus::SaveFailed:             return "SaveFailed";
    }
    return "NotFound";
}

// PendingResult -> the contract's PendingResult. Fields the backend does
// not set for a status are sent as null (as client.js expects).
static json pendingJson(const PendingResult& r, bool refusedCancel = false)
{
    const PendingStatus s = r.status;
    const bool hasOutcome = s == PendingStatus::Committed || s == PendingStatus::AttemptsExhausted ||
                            (s == PendingStatus::SaveFailed && !refusedCancel);
    const bool hasEntryInfo = hasOutcome || s == PendingStatus::Started || s == PendingStatus::WrongOtp ||
                              s == PendingStatus::OtpNotRequired || s == PendingStatus::OtpRequired;

    json j;
    j["status"] = statusName(s);
    j["pendingId"] = r.pendingId;
    j["otpRequired"] = hasEntryInfo ? json(r.otpRequired) : json(nullptr);
    j["failedAttempts"] = hasEntryInfo ? json(r.failedAttempts) : json(nullptr);
    j["attemptsRemaining"] = (s == PendingStatus::Started || s == PendingStatus::WrongOtp)
                                 ? json(r.attemptsRemaining) : json(nullptr);
    j["expiresInSeconds"] = r.expiresInSeconds >= 0 ? json(r.expiresInSeconds) : json(nullptr);
    j["txn"] = (s == PendingStatus::Started || hasOutcome) ? txnJson(r.txn, true) : json(nullptr);
    j["prelim"] = s == PendingStatus::Started ? analysisJson(r.prelim) : json(nullptr);
    j["outcome"] = hasOutcome ? outcomeJson(r.outcome) : json(nullptr);
    return j;
}

// Pending entries are private to the user who started them: another user
// gets NotFound, exactly as for an unknown ID.
static bool ownsPending(const User& u, const string& pendingId)
{
    auto it = g_pendingOwner.find(pendingId);
    return it != g_pendingOwner.end() && it->second == u.id;
}

// Drops the owner record once the backend says the entry is gone. Only
// called after the OWNER's own request: a stranger's NotFound must not
// unlink the real owner from a live transaction.
static void forgetPendingIfFinished(const PendingResult& r)
{
    switch (r.status)
    {
        case PendingStatus::Committed: case PendingStatus::AttemptsExhausted:
        case PendingStatus::Cancelled: case PendingStatus::Expired:
        case PendingStatus::NotFound:
            g_pendingOwner.erase(r.pendingId);
            break;
        default: break;
    }
}

// ------------------------------------------------------------------
// POST /transactions/start - validation as in the contract
// ------------------------------------------------------------------

static bool hasAtMostTwoDecimals(double a)
{
    double cents = a * 100.0;
    double tol = max(1e-6, fabs(cents) * 1e-14);
    return fabs(cents - round(cents)) <= tol;
}

static json startTransaction(const User& u, const json& b, const httplib::Request& req)
{
    // ---- Idempotency-Key (optional) ----
    const bool hasKey = req.has_header("Idempotency-Key");
    const string key = req.get_header_value("Idempotency-Key");
    static const regex keyRe("^[A-Za-z0-9._-]{1,64}$");
    if (hasKey && !regex_match(key, keyRe))
        fail(400, "INVALID_REQUEST", "Invalid Idempotency-Key", "Idempotency-Key");

    // ---- card ----
    if (!b.contains("cardNo") || !b["cardNo"].is_number_integer())
        fail(400, "INVALID_REQUEST", "Card is required", "cardNo");
    const long long cardNoLL = b["cardNo"].get<long long>();
    Card card;
    if (cardNoLL <= 0 || cardNoLL > INT_MAX || !canSeeCard(u, static_cast<int>(cardNoLL), &card))
        fail(404, "CARD_NOT_FOUND", "Card not found");

    // ---- id (optional) ----
    int requestedId = 0;
    if (b.contains("id") && !b["id"].is_null())
    {
        if (!b["id"].is_number_integer() || b["id"].get<long long>() <= 0 || b["id"].get<long long>() > INT_MAX)
            fail(400, "INVALID_REQUEST", "Transaction ID must be a whole number above 0", "id");
        requestedId = static_cast<int>(b["id"].get<long long>());
    }

    // ---- amount ----
    if (!b.contains("amount") || !b["amount"].is_number())
        fail(400, "INVALID_REQUEST", "Amount must be 0.01 to 999999999.99 with at most 2 decimals", "amount");
    const double amount = b["amount"].get<double>();
    if (!isfinite(amount) || amount < 0.01 || amount > MAX_AMOUNT || !hasAtMostTwoDecimals(amount))
        fail(400, "INVALID_REQUEST", "Amount must be 0.01 to 999999999.99 with at most 2 decimals", "amount");

    // ---- merchant / device ----
    auto text = [&](const char* name) {
        return (b.contains(name) && b[name].is_string()) ? cleanText(b[name].get<string>()) : string();
    };
    const string merchant = toUpperCase(text("merchant"));
    const string deviceId = text("deviceId");
    if (merchant.empty()) fail(400, "INVALID_REQUEST", "Merchant is required", "merchant");
    if (deviceId.empty()) fail(400, "INVALID_REQUEST", "Device ID is required", "deviceId");
    if (merchant.size() > MAX_TEXT_LENGTH)
        fail(400, "INVALID_REQUEST", "Merchant is too long (max 64 characters)", "merchant");
    if (deviceId.size() > MAX_TEXT_LENGTH)
        fail(400, "INVALID_REQUEST", "Device ID is too long (max 64 characters)", "deviceId");

    // ---- location ----
    Location location;
    const string locName = toUpperCase(text("location"));
    if (locName.empty() || !getLocation(locName, location))
        fail(400, "UNKNOWN_LOCATION", "Location not found - choose a city from the list", "location");

    // ---- idempotent replay (only a completed Started answer is replayed) ----
    ostringstream fp;
    fp << requestedId << '|' << card.cardNo << '|' << llround(amount * 100.0) << '|'
       << merchant << '|' << location.name << '|' << deviceId;
    const string idemKey = u.id + "|" + key;
    const auto now = chrono::steady_clock::now();

    for (auto it = g_idem.begin(); it != g_idem.end(); )
        it = (now - it->second.at > IDEMPOTENCY_RETENTION) ? g_idem.erase(it) : next(it);

    if (hasKey)
    {
        auto it = g_idem.find(idemKey);
        if (it != g_idem.end())
        {
            if (it->second.fingerprint != fp.str())
                fail(422, "IDEMPOTENCY_KEY_REUSED", "This Idempotency-Key was used with a different request");
            json replay = it->second.response;
            long long elapsed = chrono::duration_cast<chrono::seconds>(now - it->second.at).count();
            if (replay["expiresInSeconds"].is_number())
                replay["expiresInSeconds"] = max(0LL, replay["expiresInSeconds"].get<long long>() - elapsed);
            return replay;
        }
    }

    // ---- start (backend PendingTransactionManager) ----
    vector<Transaction> history;
    loadTransactions(history);

    TransactionRequest request;
    request.cardNo = card.cardNo;
    request.amount = amount;
    request.merchant = merchant;
    request.location = location;
    request.deviceId = deviceId;

    PendingResult r;

    if (requestedId > 0)
    {
        request.id = requestedId;
        r = g_pending.start(request, card, history, g_graph);
    }
    else
    {
        // Server-allocated ID: highest known + 1. The manager rejects IDs that
        // are still pending, so step forward a few times before giving up.
        long long candidate = g_idHighWater;
        for (const Transaction& t : history) candidate = max<long long>(candidate, t.id);
        candidate++;

        for (int attempt = 0;; attempt++, candidate++)
        {
            if (attempt >= ID_ALLOC_RETRIES || candidate > INT_MAX)
                fail(503, "ID_ALLOCATION_FAILED", "Could not allocate a transaction ID, try again");

            request.id = static_cast<int>(candidate);
            r = g_pending.start(request, card, history, g_graph);
            if (r.status != PendingStatus::DuplicateTransactionId) break;
        }
    }

    json response = pendingJson(r);

    if (r.status == PendingStatus::Started)
    {
        g_idHighWater = max(g_idHighWater, request.id);
        g_pendingOwner[r.pendingId] = u.id;
        if (hasKey) g_idem[idemKey] = IdemEntry{fp.str(), response, now};
    }
    return response;
}

// ------------------------------------------------------------------
// Route table
// ------------------------------------------------------------------

using Handler = function<json(const httplib::Request&, const json&)>;

// Wraps a handler: takes the store lock, parses JSON, maps errors.
static httplib::Server::Handler wrap(Handler h)
{
    return [h](const httplib::Request& req, httplib::Response& res) {
        lock_guard<mutex> lock(g_store);
        try
        {
            json body = (req.method == "POST") ? parseBody(req) : json::object();
            sendJson(res, h(req, body));
        }
        catch (const ApiError& e) { sendError(res, e); }
        catch (const exception& e)
        {
            cerr << "Internal error on " << req.method << ' ' << req.path << ": " << e.what() << '\n';
            sendError(res, ApiError{500, "INTERNAL", "Internal server error", ""});
        }
    };
}

static void registerRoutes(httplib::Server& svr)
{
    // ---- auth ----
    svr.Post("/api/auth/login", wrap([](const httplib::Request&, const json& b) {
        string username = (b.contains("username") && b["username"].is_string()) ? lowerCase(trim(b["username"].get<string>())) : "";
        string password = (b.contains("password") && b["password"].is_string()) ? b["password"].get<string>() : "";
        for (const User& u : g_users)
        {
            if (u.username == username && u.password == password)
            {
                string token = randomHex(32);
                g_sessions[token] = Session{u.id, chrono::steady_clock::now() + SESSION_LIFETIME};
                return json{{"token", token}, {"user", {{"id", u.id}, {"name", u.name}, {"role", u.role}}}};
            }
        }
        fail(401, "INVALID_CREDENTIALS", "Wrong username or password");
    }));

    // ---- config (backend constants) ----
    svr.Get("/api/config", wrap([](const httplib::Request& req, const json&) {
        requireUser(req);
        return json{{"maxScore", MAX_SCORE}, {"maxOtpAttempts", MAX_OTP_ATTEMPTS},
                    {"pendingTtlSeconds", PENDING_TTL_SECONDS}, {"queueCapacity", DEQUE_CAPACITY},
                    {"maxAmount", MAX_AMOUNT}, {"amountDecimals", AMOUNT_DECIMALS}};
    }));

    // ---- transactions (transactions.csv) ----
    svr.Get("/api/transactions", wrap([](const httplib::Request& req, const json&) {
        const User& u = requireUser(req);
        vector<int> ids;
        for (const Card& c : cardsFor(u)) ids.push_back(c.cardNo);

        vector<Transaction> history;
        loadTransactions(history);

        json out = json::array();
        for (const Transaction& t : history)
            if (find(ids.begin(), ids.end(), t.cardNo) != ids.end()) out.push_back(txnJson(t, false));
        return out;
    }));

    // ---- cards (cards.csv + card_owners.csv) ----
    svr.Get("/api/cards", wrap([](const httplib::Request& req, const json&) {
        json out = json::array();
        for (const Card& c : cardsFor(requireUser(req))) out.push_back(cardJson(c));
        return out;
    }));

    svr.Post("/api/cards", wrap([](const httplib::Request& req, const json& b) {
        const User& u = requireUser(req);

        if (!b.contains("id") || !b["id"].is_number_integer() ||
            b["id"].get<long long>() <= 0 || b["id"].get<long long>() > INT_MAX)
            fail(400, "INVALID_REQUEST", "Card ID must be a whole number above 0", "id");
        const int id = static_cast<int>(b["id"].get<long long>());

        string type = (b.contains("type") && b["type"].is_string()) ? titleCase(b["type"].get<string>()) : "";
        if (type != "Credit" && type != "Debit")
            fail(400, "INVALID_REQUEST", "Type must be Credit or Debit", "type");

        string exp = (b.contains("exp") && b["exp"].is_string()) ? trim(b["exp"].get<string>()) : "";
        if (!validDate(exp)) fail(400, "INVALID_REQUEST", "Active-until date must be YYYY-MM-DD", "exp");

        Card existing;
        if (searchCard(id, existing)) fail(409, "CARD_EXISTS", "Card ID already in use");

        // Owner row first: if cards.csv then fails, the owner row is undone,
        // so no card is ever saved without an owner.
        map<int, CardMeta> meta = g_cardMeta;
        meta[id] = CardMeta{u.id, exp};
        if (!saveCardOwners(meta)) fail(500, "SAVE_FAILED", "Could not save the card, try again");

        if (!addCard(id, cleanText(u.name), type, "Active", 0))
        {
            saveCardOwners(g_cardMeta);                       // undo owner row
            fail(500, "SAVE_FAILED", "Could not save the card, try again");
        }
        g_cardMeta = meta;
        return json{{"ok", true}};
    }));

    // ---- card travel trail (locationgraph) ----
    svr.Get(R"(/api/cards/(\d{1,10})/trail)", wrap([](const httplib::Request& req, const json&) {
        const User& u = requireUser(req);
        long long id = stoll(req.matches[1].str());
        if (id > INT_MAX || !canSeeCard(u, static_cast<int>(id))) fail(404, "CARD_NOT_FOUND", "Card not found");

        vector<Transaction> history;
        loadTransactions(history);
        CardTrail trail = g_graph.cardTrail(static_cast<int>(id), history);

        json steps = json::array();
        for (const TrailStep& s : trail.steps)
            steps.push_back({{"location", s.location}, {"timestamp", s.timestamp}, {"kmFromPrevious", s.kmFromPrevious}});
        return json{{"steps", steps}, {"totalKm", trail.totalKm}, {"distinctLocations", trail.distinctLocations}};
    }));

    // ---- locations (locations.csv + graph) ----
    svr.Get("/api/locations", wrap([](const httplib::Request& req, const json&) {
        requireUser(req);
        vector<Location> all;
        loadAllLocations(all);
        json out = json::array();
        for (const Location& l : all) out.push_back(toUpperCase(l.name));
        return out;
    }));

    svr.Get("/api/locations/route", wrap([](const httplib::Request& req, const json&) {
        requireUser(req);
        string from = toUpperCase(req.get_param_value("from")), to = toUpperCase(req.get_param_value("to"));
        if (!g_graph.hasCity(from) || !g_graph.hasCity(to)) fail(404, "UNKNOWN_CITY", "Unknown city");
        RouteResult r = g_graph.shortestRoute(from, to);
        return json{{"found", r.found}, {"totalKm", r.totalKm}, {"path", r.path}};
    }));

    // ---- pending transaction flow (PendingTransactionManager) ----
    svr.Post("/api/transactions/start", wrap([](const httplib::Request& req, const json& b) {
        return startTransaction(requireUser(req), b, req);
    }));

    svr.Post(R"(/api/transactions/([0-9a-f]{1,64})/otp)", wrap([](const httplib::Request& req, const json& b) {
        const User& u = requireUser(req);
        const string pid = req.matches[1].str();
        PendingResult r;
        r.pendingId = pid;                                    // NotFound by default
        if (ownsPending(u, pid))
        {
            string code;
            if (b.contains("code"))
                code = b["code"].is_string() ? trim(b["code"].get<string>()) : b["code"].dump();
            r = g_pending.submitOtp(pid, code, g_queue);
            forgetPendingIfFinished(r);
        }
        return pendingJson(r);
    }));

    svr.Post(R"(/api/transactions/([0-9a-f]{1,64})/commit)", wrap([](const httplib::Request& req, const json&) {
        const User& u = requireUser(req);
        const string pid = req.matches[1].str();
        PendingResult r;
        r.pendingId = pid;
        if (ownsPending(u, pid))
        {
            r = g_pending.commit(pid, g_queue);
            forgetPendingIfFinished(r);
        }
        return pendingJson(r);
    }));

    svr.Post(R"(/api/transactions/([0-9a-f]{1,64})/cancel)", wrap([](const httplib::Request& req, const json&) {
        const User& u = requireUser(req);
        const string pid = req.matches[1].str();
        PendingResult r;
        r.pendingId = pid;
        if (ownsPending(u, pid))
        {
            r = g_pending.cancel(pid);
            forgetPendingIfFinished(r);
        }
        // A refused cancel (SaveFailed) carries no outcome: part is already saved.
        return pendingJson(r, /*refusedCancel=*/true);
    }));

    // ---- review queue (dequemodule) ----
    svr.Get("/api/review-queue", wrap([](const httplib::Request& req, const json&) {
        requireAnalyst(requireUser(req));
        json items = json::array();
        QueueItem q;
        for (int p = 0; g_queue.at(p, q); p++) items.push_back(queueItemJson(q));
        return json{{"capacity", DEQUE_CAPACITY}, {"size", g_queue.size()}, {"items", items}};
    }));

    auto pop = [](bool front) {
        return wrap([front](const httplib::Request& req, const json&) {
            requireAnalyst(requireUser(req));
            QueueItem q;
            bool removed = front ? g_queue.deleteFront(q) : g_queue.deleteRear(q);
            if (!removed) return json{{"removed", nullptr}};

            // The console ignored a failed save here, so the case came back
            // after a restart. Put it back and report the failure instead.
            if (!g_queue.save())
            {
                if (front) g_queue.insertFront(q); else g_queue.insertRear(q);
                fail(500, "SAVE_FAILED", "Could not save the review queue, try again");
            }
            return json{{"removed", queueItemJson(q)}};
        });
    };
    svr.Post("/api/review-queue/front/pop", pop(true));
    svr.Post("/api/review-queue/rear/pop", pop(false));

    // ---- unknown API paths: JSON 404 (not the static-file handler) ----
    svr.Get(R"(/api/.*)", [](const httplib::Request& req, httplib::Response& res) {
        sendError(res, ApiError{404, "NOT_FOUND", "Unknown endpoint " + req.path, ""});
    });
    svr.Post(R"(/api/.*)", [](const httplib::Request& req, httplib::Response& res) {
        sendError(res, ApiError{404, "NOT_FOUND", "Unknown endpoint " + req.path, ""});
    });
}

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------

int main(int argc, char** argv)
{
    string host = "127.0.0.1", frontend = "../frontend";
    int port = 8080;

    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        auto value = [&]() -> string {
            if (i + 1 >= argc) { cerr << "Missing value for " << a << '\n'; exit(2); }
            return argv[++i];
        };
        if (a == "--port") port = atoi(value().c_str());
        else if (a == "--host") host = value();
        else if (a == "--frontend") frontend = value();
        else
        {
            cout << "Usage: smartshield_server [--port 8080] [--host 127.0.0.1] [--frontend ../frontend]\n"
                    "Run it from the backend folder (it reads the CSV files there).\n";
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (port <= 0 || port > 65535) { cerr << "Invalid port\n"; return 2; }

    // ---- load data (same files as the console program) ----
    if (!ifstream("cards.csv").is_open() || !ifstream("locations.csv").is_open())
    {
        cerr << "cards.csv / locations.csv not found. Run the server from the backend folder.\n";
        return 1;
    }
    loadCards();
    g_queue.load();
    if (!g_graph.buildFromFile())
    {
        cerr << "locations.csv is empty - the location graph is unavailable.\n";
        return 1;
    }
    if (!loadUsers())
    {
        cerr << USERS_FILE << " missing or empty - nobody could sign in.\n";
        return 1;
    }
    loadCardOwners();

    httplib::Server svr;
    registerRoutes(svr);

    if (!svr.set_mount_point("/", frontend))
        cerr << "Warning: frontend folder '" << frontend << "' not found; only the API is served.\n";

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0)
            cout << req.method << ' ' << req.path << " -> " << res.status << endl;
    });

    cout << "Smart Shield server: http://" << (host == "0.0.0.0" ? "localhost" : host) << ':' << port << '\n'
         << "  cards " << getAllCards().size() << ", cities " << g_graph.cityCount()
         << ", review queue " << g_queue.size() << '/' << DEQUE_CAPACITY
         << ", users " << g_users.size() << endl;

    if (!svr.listen(host, port))
    {
        cerr << "Could not listen on " << host << ':' << port << " (port in use?)\n";
        return 1;
    }
    return 0;
}
