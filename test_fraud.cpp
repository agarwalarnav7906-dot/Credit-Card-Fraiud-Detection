// Unit tests for the fraud engine. Build:
//   g++ -std=c++17 -o test_fraud test_fraud.cpp fraudmodule.cpp card_history.cpp latlong.cpp time_module.cpp
#include <iostream>
#include "fraudmodule.h"
#include "time_module.h"
#include "card_history.h"
#include "location_history.h"
#include <cstdio>

using namespace std;

static int passed = 0, failed = 0;

static void check(const string& name, bool ok)
{
    cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    ok ? passed++ : failed++;
}

const long long NOW = 1800000000LL;     // fixed "current" time for tests

static Transaction make(int id, double amt, const string& merchant,
                        const string& dev, long long ts,
                        double lat = 28.6139, double lon = 77.2090,
                        bool blocked = false)
{
    Transaction t;
    t.id = id; t.cardNo = 1001; t.amount = amt;
    t.merchant = merchant; t.deviceId = dev;
    t.latitude = lat; t.longitude = lon;
    t.timestamp = formatTimestamp(ts);
    t.blocked = blocked;
    return t;
}

// Baseline history: 4 old transactions avg 15000, Amazon, DEV1, hours ago
static vector<Transaction> baseHistory()
{
    vector<Transaction> h;
    double a[4] = {10000, 15000, 20000, 15000};
    for (int i = 0; i < 4; i++)
        h.push_back(make(i + 1, a[i], "Amazon", "DEV1", NOW - 7200 - i * 600));
    return h;
}


// Builds the aggregated card_history record the same way the program does:
// by applying each finished transaction of the card in order.
static CardHistory cardOf(const vector<Transaction>& h,
                          const PreviousLocation* p = nullptr)
{
    CardHistory c;
    c.cardNo = 1001;
    for (const Transaction& t : h)
        if (t.cardNo == 1001) applyTransactionToCard(c, t);
    if (p != nullptr) {
        c.hasLastLocation = true;
        c.lastLocation = p->location;
        c.lastLatitude = p->latitude;
        c.lastLongitude = p->longitude;
        c.lastTimestamp = p->timestamp;
    }
    return c;
}

int main()
{
    // Test 1: normal
    {
        auto h = baseHistory();
        Transaction c = make(100, 12000, "amazon", "dev1", NOW);   // case-insens.
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T1 normal: score 0, NORMAL", r.score == 0 && r.status == "NORMAL");
    }

    // First transaction of a card: nothing flagged
    {
        vector<Transaction> h;
        Transaction c = make(1, 999999, "NewShop", "DEVX", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("First txn: no average/merchant/device flags, score 0", r.score == 0);
    }

    // Other card's history must be ignored
    {
        auto h = baseHistory();
        for (auto& t : h) t.cardNo = 2002;
        Transaction c = make(100, 999999, "NewShop", "DEVX", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("Other card's history ignored", r.score == 0 && !r.hasHistoricalAverage);
    }

    // Test 2: spending anomaly (avg 15000 -> 30000 exactly = 2x)
    {
        auto h = baseHistory();
        Transaction c = make(100, 30000, "Amazon", "DEV1", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T2 amount == 2x average triggers +15",
              r.spendingAnomaly && r.score == 15 && r.historicalAverage == 15000);

        Transaction c2 = make(101, 29999, "Amazon", "DEV1", NOW);
        auto r2 = calculateFraudScore(c2, h, cardOf(h), 0, false);
        check("T2b amount just below 2x: no flag", !r2.spendingAnomaly && r2.score == 0);
    }

    // Test 3: rapid frequency (5 previous within 10 min) and 4 = no
    {
        vector<Transaction> h = baseHistory();
        for (int i = 0; i < 4; i++)
            h.push_back(make(10 + i, 15000, "Amazon", "DEV1", NOW - 60 * (i + 1)));
        Transaction c = make(100, 15000, "Amazon", "DEV1", NOW);
        auto r4 = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T3a 4 recent txns: no rapid penalty", !r4.rapidFrequency && r4.recentTransactionCount == 4);

        h.push_back(make(20, 15000, "Amazon", "DEV1", NOW - 600));  // exactly 10 min
        auto r5 = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T3b 5 recent txns (one exactly 10 min old): +10",
              r5.rapidFrequency && r5.score == 10);

        h.push_back(make(21, 15000, "Amazon", "DEV1", NOW - 601));  // outside
        auto r6 = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T3c txn 601s old not counted", r6.recentTransactionCount == 5);
    }

    // Test 4: new merchant
    {
        auto h = baseHistory();
        Transaction c = make(100, 12000, "Flipkart", "DEV1", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T4 new merchant +5", r.newMerchant && r.score == 5);
    }

    // Test 5: new device
    {
        auto h = baseHistory();
        Transaction c = make(100, 12000, "Amazon", "DEV9", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("T5 new device +10", r.newDevice && r.score == 10);
    }

    // Test 6: impossible travel (Delhi -> Mumbai in 30 min)
    {
        auto h = baseHistory();
        PreviousLocation p; p.found = true; p.location = "Delhi";
        p.latitude = 28.6139; p.longitude = 77.2090; p.timestamp = NOW - 1800;
        Transaction c = make(100, 12000, "Amazon", "DEV1", NOW, 19.076, 72.8777);
        auto r = calculateFraudScore(c, h, cardOf(h, &p), 0, false);
        check("T6 impossible travel +30", r.impossibleTravel && r.score == 30);

        // Same trip over 6 hours = normal (flight/train), no flag
        p.timestamp = NOW - 6 * 3600;
        auto r2 = calculateFraudScore(c, h, cardOf(h, &p), 0, false);
        check("T6b same trip over 6h: not impossible", !r2.impossibleTravel && r2.score == 0);

        // Short hop in 1 minute: ignored (below min distance)
        PreviousLocation q = p; q.latitude = 28.5355; q.longitude = 77.3910; q.timestamp = NOW - 60;
        Transaction c2 = make(101, 12000, "Amazon", "DEV1", NOW);
        auto r3 = calculateFraudScore(c2, h, cardOf(h, &q), 0, false);
        check("T6c 20km hop in 1 min: not flagged", !r3.impossibleTravel);
    }

    // Test 7: combined to >= 50 -> OTP required
    {
        auto h = baseHistory();
        PreviousLocation p; p.found = true; p.latitude = 28.6139; p.longitude = 77.2090;
        p.timestamp = NOW - 1800;
        Transaction c = make(100, 40000, "Flipkart", "DEV9", NOW, 19.076, 72.8777);
        auto r = calculateFraudScore(c, h, cardOf(h, &p), 0, false);
        // 30 travel + 15 spend + 10 device + 5 merchant = 60
        check("T7 combined = 60 -> HIGH RISK, OTP required",
              r.score == 60 && r.status == "HIGH RISK" && otpRequiredFor(r.score));
    }

    // Test 8: wrong OTP: prelim 32 + 50 = 82 (spec example, via direct engine call)
    {
        FraudAnalysis a;  // thresholds/cap/classify checks
        check("T8 classify(82) = CRITICAL FRAUD and blocks",
              classifyRisk(82) == "CRITICAL FRAUD" && shouldBlock(82));

        auto h = baseHistory();
        PreviousLocation p; p.found = true; p.latitude = 28.6139; p.longitude = 77.2090;
        p.timestamp = NOW - 1800;
        Transaction c = make(100, 40000, "Flipkart", "DEV9", NOW, 19.076, 72.8777);
        auto pre = calculateFraudScore(c, h, cardOf(h, &p), 0, false);          // 60
        auto fin = calculateFraudScore(c, h, cardOf(h, &p), 1, true);           // +50 -> capped
        check("T8b wrong OTP adds exactly +50 (raw 110), capped at 100",
              fin.rawScore - pre.rawScore == 50 && fin.score == 100 && fin.status == "CRITICAL FRAUD");

        // 32-point style example: device(10)+merchant(5)+... use rapid(10)+spend(15)+merchant(5)=30 +2? use 30+... 
        vector<Transaction> h2 = baseHistory();
        for (int i = 0; i < 5; i++) h2.push_back(make(30 + i, 15000, "Amazon", "DEV1", NOW - 60 * (i + 1)));
        Transaction c2 = make(101, 12000, "Flipkart", "DEV9", NOW);   // rapid10 + dev10 + merch5 = 25
        auto pre2 = calculateFraudScore(c2, h2, cardOf(h2), 0, false);
        auto fin2 = calculateFraudScore(c2, h2, cardOf(h2), 1, true);
        check("T8c prelim 25 + 50 = 75 -> CRITICAL, blocked",
              pre2.score == 25 && fin2.score == 75 && shouldBlock(fin2.score));
    }

    // Test 9: correct OTP -> no penalty
    {
        auto h = baseHistory();
        PreviousLocation p; p.found = true; p.latitude = 28.6139; p.longitude = 77.2090;
        p.timestamp = NOW - 1800;
        Transaction c = make(100, 40000, "Flipkart", "DEV9", NOW, 19.076, 72.8777);
        auto pre = calculateFraudScore(c, h, cardOf(h, &p), 0, false);
        auto fin = calculateFraudScore(c, h, cardOf(h, &p), 0, false);
        check("T9 correct OTP: no +50, score unchanged (60), not blocked",
              fin.score == pre.score && fin.score == 60 && !shouldBlock(fin.score));
    }

    // Repeated auth failure
    {
        auto h = baseHistory();
        Transaction c = make(100, 12000, "Amazon", "DEV1", NOW);
        auto r1 = calculateFraudScore(c, h, cardOf(h), 1, false);
        auto r2 = calculateFraudScore(c, h, cardOf(h), 2, false);
        auto r3 = calculateFraudScore(c, h, cardOf(h), 3, true);
        check("Auth fail: 1 failure no +10", !r1.repeatedAuthFailure && r1.score == 0);
        check("Auth fail: 2 failures +10", r2.repeatedAuthFailure && r2.score == 10);
        check("Auth fail: 3 failures + wrong OTP = 10 + 50 (separate, counted once each)", r3.score == 60);
    }

    // Blocked history is not trusted
    {
        vector<Transaction> h = baseHistory();
        h.push_back(make(50, 90000, "ShadyShop", "EVIL", NOW - 3600, 28.6, 77.2, true));
        Transaction c = make(100, 12000, "ShadyShop", "EVIL", NOW);
        auto r = calculateFraudScore(c, h, cardOf(h), 0, false);
        check("Blocked txn doesn't make device/merchant trusted or skew average",
              r.newDevice && r.newMerchant && r.historicalAverage == 15000);
    }

    // Boundaries of classification + cap
    check("Boundary 29 NORMAL",     classifyRisk(29) == "NORMAL");
    check("Boundary 30 SUSPICIOUS", classifyRisk(30) == "SUSPICIOUS");
    check("Boundary 49 SUSPICIOUS", classifyRisk(49) == "SUSPICIOUS");
    check("Boundary 50 HIGH RISK + OTP", classifyRisk(50) == "HIGH RISK" && otpRequiredFor(50));
    check("Boundary 69 HIGH RISK, not blocked", classifyRisk(69) == "HIGH RISK" && !shouldBlock(69));
    check("Boundary 70 CRITICAL + block", classifyRisk(70) == "CRITICAL FRAUD" && shouldBlock(70));
    check("Cap: 150 -> 100", capScore(150) == 100);

    // ---------- card_history.csv: aggregation + persistence ----------
    {
        const string F = "/tmp/test_card_history.csv";
        remove(F.c_str());

        CardHistory c;
        c.cardNo = 1001;
        Transaction t1 = make(1, 10000, "Amazon", "DEV1", NOW - 5000, 28.6139, 77.2090);
        Transaction t2 = make(2, 20000, "Flipkart", "DEV2", NOW - 3000, 19.076, 72.8777);
        Transaction t3 = make(3, 90000, "Shady", "EVIL", NOW - 1000, 12.97, 77.59, true);
        t3.failedAuthAttempts = 3; t3.otpRequired = true; t3.finalScore = 100;

        updateCardHistory(c, t1, F);
        updateCardHistory(c, t2, F);
        updateCardHistory(c, t3, F);

        check("CardHistory aggregation: 3 total, 2 completed, 1 blocked",
              c.totalTransactions == 3 && c.completedTransactions == 2 &&
              c.blockedTransactions == 1);
        check("CardHistory average ignores blocked txn (15000)",
              averageAmount(c) == 15000.0);
        check("CardHistory blocked device/merchant not trusted",
              !containsIgnoreCase(c.devices, "EVIL") && !containsIgnoreCase(c.merchants, "Shady"));
        check("CardHistory last location = last COMPLETED txn (Mumbai)",
              c.lastLatitude == 19.076 && c.lastTimestamp == NOW - 3000);

        // Simulated restart: reload from disk only
        vector<Transaction> noTxns;
        CardHistory r = getCardHistory(1001, noTxns, F);
        check("Restart: card_history.csv reload matches",
              r.found && r.totalTransactions == 3 && r.completedTransactions == 2 &&
              r.blockedTransactions == 1 && averageAmount(r) == 15000.0 &&
              r.failedOtpAttempts == 3 && r.otpRequiredCount == 1 &&
              containsIgnoreCase(r.merchants, "flipkart") &&
              containsIgnoreCase(r.devices, "dev2") && r.lastTimestamp == NOW - 3000);

        // Another card does not share data
        CardHistory other = getCardHistory(2002, noTxns, F);
        check("Unknown card: found=false, empty history", !other.found && other.totalTransactions == 0);

        // Rebuild from transactions.csv when no aggregate row exists
        remove(F.c_str());
        vector<Transaction> txns = {t1, t2, t3};
        CardHistory rb = getCardHistory(1001, txns, F);
        check("Rebuild from transactions when card_history row missing",
              rb.found && rb.completedTransactions == 2 && averageAmount(rb) == 15000.0);

        remove(F.c_str());
    }

    // ---------- end-to-end: engine reads card record + transactions ----------
    {
        vector<Transaction> txns;
        for (int i = 0; i < 4; i++)
            txns.push_back(make(i + 1, 15000, "Amazon", "DEV1", NOW - 4000 + i * 100));
        CardHistory card = cardOf(txns);
        Transaction c = make(50, 30000, "Amazon", "DEV1", NOW);
        auto r = calculateFraudScore(c, txns, card, 0, false);
        check("Engine uses card_history average (15000) for 2x rule", r.spendingAnomaly && r.score == 15);
    }

    cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
