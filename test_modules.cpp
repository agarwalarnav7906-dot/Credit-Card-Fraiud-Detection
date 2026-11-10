// Unit tests for the sorting, deque and location-graph modules. Build:
//   g++ -std=c++17 -o test_modules test_modules.cpp sortmodule.cpp dequemodule.cpp locationgraph.cpp latlong.cpp time_module.cpp fraudmodule.cpp card_history.cpp transaction_history.cpp
#include <iostream>
#include <cstdio>
#include "sortmodule.h"
#include "dequemodule.h"
#include "locationgraph.h"
#include "time_module.h"
#include "fraudmodule.h"

using namespace std;

static int passed = 0, failed = 0;

static void check(const string& name, bool ok)
{
    cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    ok ? passed++ : failed++;
}

const long long BASE = 1800000000LL;

static Transaction mk(int id, int card, double amt, int score, long long ts,
                      const string& loc = "Delhi",
                      double lat = 28.6139, double lon = 77.2090)
{
    Transaction t;
    t.id = id; t.cardNo = card; t.amount = amt;
    t.finalScore = score; t.score = score;
    t.timestamp = formatTimestamp(ts);
    t.location = loc; t.latitude = lat; t.longitude = lon;
    t.fraudStatus = classifyRisk(score);
    return t;
}

static vector<int> ids(const vector<Transaction>& v)
{
    vector<int> r;
    for (const auto& t : v) r.push_back(t.id);
    return r;
}

int main()
{
    // ---------------- SORTING ----------------
    vector<Transaction> list = {
        mk(1, 1001, 500,   10, BASE + 300),
        mk(2, 1001, 9000,  80, BASE + 100),
        mk(3, 1001, 250,   40, BASE + 200),
        mk(4, 1001, 9000,  20, BASE + 400),   // same amount as #2
    };

    check("Sort by time ascending",
          ids(sortTransactions(list, SORT_BY_TIME, ASCENDING)) == vector<int>({2, 3, 1, 4}));

    check("Sort by time descending",
          ids(sortTransactions(list, SORT_BY_TIME, DESCENDING)) == vector<int>({4, 1, 3, 2}));

    check("Sort by amount ascending",
          ids(sortTransactions(list, SORT_BY_AMOUNT, ASCENDING)) == vector<int>({3, 1, 2, 4}));

    check("Sort by amount descending is stable (ties keep input order)",
          ids(sortTransactions(list, SORT_BY_AMOUNT, DESCENDING)) == vector<int>({2, 4, 1, 3}));

    check("Sort by risk descending",
          ids(sortTransactions(list, SORT_BY_RISK, DESCENDING)) == vector<int>({2, 3, 4, 1}));

    check("Original vector unchanged by sorting", ids(list) == vector<int>({1, 2, 3, 4}));

    check("Empty list sorts to empty",
          sortTransactions(vector<Transaction>(), SORT_BY_TIME).empty());

    check("Top 2 riskiest",
          ids(topRiskTransactions(list, 2)) == vector<int>({2, 3}));

    check("Top N larger than list returns all",
          topRiskTransactions(list, 99).size() == 4);

    {
        vector<Transaction> big;
        for (int i = 0; i < 200; i++)
            big.push_back(mk(i, 1001, (i * 37) % 101, (i * 53) % 100, BASE + (i * 71) % 997));
        auto s = sortTransactions(big, SORT_BY_AMOUNT, ASCENDING);
        bool ordered = true;
        for (size_t i = 1; i < s.size(); i++)
            if (s[i - 1].amount > s[i].amount) ordered = false;
        check("200 random transactions sort correctly", ordered && s.size() == 200);
    }

    // ---------------- DEQUE ----------------
    {
        TransactionDeque d;
        QueueItem a, b, c, out;
        a.id = 1; b.id = 2; c.id = 3;

        check("New deque is empty", d.isEmpty() && d.size() == 0);
        check("deleteFront on empty fails", !d.deleteFront(out));
        check("deleteRear on empty fails", !d.deleteRear(out));

        d.insertRear(a);              // [1]
        d.insertRear(b);              // [1 2]
        d.insertFront(c);             // [3 1 2]

        d.peekFront(out); bool f = (out.id == 3);
        d.peekRear(out);  bool r = (out.id == 2);
        check("insertFront / insertRear order", f && r && d.size() == 3);

        d.deleteFront(out); bool o1 = (out.id == 3);
        d.deleteRear(out);  bool o2 = (out.id == 2);
        d.deleteFront(out); bool o3 = (out.id == 1);
        check("deleteFront / deleteRear order", o1 && o2 && o3 && d.isEmpty());
    }

    {
        // wrap-around: fill, drain half, refill, front insert past index 0
        TransactionDeque d;
        QueueItem q, out;

        for (int i = 0; i < DEQUE_CAPACITY; i++) { q.id = i; d.insertRear(q); }
        check("Deque reports full", d.isFull());
        q.id = 999;
        check("insertRear on full fails", !d.insertRear(q));
        check("insertFront on full fails", !d.insertFront(q));

        for (int i = 0; i < 10; i++) d.deleteFront(out);
        for (int i = 0; i < 10; i++) { q.id = 100 + i; d.insertRear(q); }

        bool order = true;
        for (int i = 0; i < 10; i++) { d.deleteFront(out); if (out.id != 10 + i) order = false; }
        for (int i = 0; i < 10; i++) { d.deleteFront(out); if (out.id != 100 + i) order = false; }
        check("Circular wrap-around keeps FIFO order", order && d.isEmpty());

        q.id = 1; d.insertFront(q);      // front index must wrap backwards
        q.id = 2; d.insertFront(q);
        d.deleteFront(out); bool w1 = (out.id == 2);
        d.deleteRear(out);  bool w2 = (out.id == 1);
        check("insertFront wraps below index 0", w1 && w2);
    }

    {
        TransactionDeque d;
        check("NORMAL transaction not queued", !d.enqueueForReview(mk(1, 1001, 10, 5, BASE)) && d.isEmpty());

        d.enqueueForReview(mk(2, 1001, 10, 35, BASE));   // suspicious -> rear
        d.enqueueForReview(mk(3, 1001, 10, 60, BASE));   // high risk  -> rear
        d.enqueueForReview(mk(4, 1001, 10, 90, BASE));   // critical   -> FRONT

        QueueItem out;
        d.peekFront(out); bool f = (out.id == 4);
        d.peekRear(out);  bool r = (out.id == 3);
        check("Critical goes to front, others to rear", f && r && d.size() == 3);
    }

    {
        TransactionDeque d;
        for (int i = 0; i < DEQUE_CAPACITY; i++) d.enqueueForReview(mk(i, 1001, 10, 40, BASE));

        check("Full deque rejects non-critical", !d.enqueueForReview(mk(500, 1001, 10, 40, BASE)));

        bool ev = false; QueueItem dropped;
        bool placed = d.enqueueForReview(mk(501, 1001, 10, 95, BASE), &ev, &dropped);

        QueueItem front, rear;
        d.peekFront(front); d.peekRear(rear);
        check("Critical evicts rear when full",
              placed && ev && dropped.id == DEQUE_CAPACITY - 1 &&
              front.id == 501 && rear.id == DEQUE_CAPACITY - 2 && d.isFull());
    }

    {
        const string file = "test_review_queue.tmp";
        TransactionDeque d;
        d.enqueueForReview(mk(7, 1001, 1234.5, 80, BASE));
        d.enqueueForReview(mk(8, 1002, 99, 55, BASE + 5));
        d.save(file);

        TransactionDeque e;
        e.load(file);
        QueueItem a, b;
        e.at(0, a); e.at(1, b);
        check("Save / load round-trip",
              e.size() == 2 && a.id == 7 && b.id == 8 && a.amount == 1234.5 &&
              a.timestamp == formatTimestamp(BASE));
        remove(file.c_str());
    }

    // ---------------- LOCATION GRAPH ----------------
    {
        vector<Location> cities = {
            {"Delhi", 28.6139, 77.2090}, {"Noida", 28.5355, 77.3910},
            {"Gurugram", 28.4595, 77.0266}, {"Mumbai", 19.0760, 72.8777},
            {"Pune", 18.5204, 73.8567}, {"Chennai", 13.0827, 80.2707}
        };

        LocationGraph g;
        g.build(cities, 1, 150.0);           // short links only, plus 1 nearest

        check("City count", g.cityCount() == 6);
        check("hasCity is case-insensitive", g.hasCity("delhi") && !g.hasCity("Paris"));
        check("Two groups: Delhi cluster, Mumbai cluster, Chennai joined by nearest",
              g.connectedComponents() >= 2);

        RouteResult r = g.shortestRoute("Delhi", "Noida");
        check("Direct route Delhi->Noida", r.found && r.path.size() == 2 && r.totalKm > 15 && r.totalKm < 30);

        RouteResult same = g.shortestRoute("Delhi", "Delhi");
        check("Route to itself is zero km", same.found && same.totalKm == 0.0 && same.path.size() == 1);

        check("Unknown city -> no route", !g.shortestRoute("Delhi", "Paris").found);

        check("BFS starts at source, stays in its group",
              g.bfs("Delhi").front() == "Delhi" && g.bfs("Delhi").size() == 3);
        check("DFS visits same set as BFS",
              g.dfs("Delhi").size() == g.bfs("Delhi").size());

        auto near = g.nearby("Delhi", 50);
        check("Nearby within 50 km: Noida then Gurugram (nearest first)",
              near.size() == 2 && near[0].first == "Noida" && near[1].first == "Gurugram");

        double km = 0;
        check("directDistance Mumbai-Pune ~120 km",
              g.directDistance("Mumbai", "Pune", km) && km > 110 && km < 130);
    }

    {
        // Full graph from the real CSV (run from the project folder)
        LocationGraph g;
        if (g.buildFromFile())
        {
            check("locations.csv graph is fully connected", g.connectedComponents() == 1);

            RouteResult r = g.shortestRoute("Dehradun", "Chennai");
            double direct = 0;
            g.directDistance("Dehradun", "Chennai", direct);
            check("Graph route is never shorter than straight line",
                  r.found && r.totalKm >= direct - 0.001);
        }
        else
            cout << "[SKIP] locations.csv not found - run from the project folder\n";
    }

    {
        vector<Location> cities = {
            {"Delhi", 28.6139, 77.2090}, {"Mumbai", 19.0760, 72.8777}
        };
        LocationGraph g;
        g.build(cities, 1, 100.0);

        vector<Transaction> h = {
            mk(3, 1001, 10, 0, BASE + 7200, "Delhi",  28.6139, 77.2090),
            mk(1, 1001, 10, 0, BASE,        "Mumbai", 19.0760, 72.8777),   // oldest, saved out of order
            mk(2, 1002, 10, 0, BASE + 50,   "Delhi",  28.6139, 77.2090),   // other card
        };
        h.push_back(mk(4, 1001, 10, 90, BASE + 9000, "Mumbai", 19.0760, 72.8777));
        h.back().blocked = true;                                           // blocked: ignored

        CardTrail tr = g.cardTrail(1001, h);
        check("Trail ignores other cards and blocked txns, sorted by time",
              tr.steps.size() == 2 && tr.steps[0].location == "Mumbai" &&
              tr.steps[1].location == "Delhi" && tr.distinctLocations == 2 &&
              tr.totalKm > 1100 && tr.totalKm < 1200);

        check("Trail of unknown card is empty", g.cardTrail(9999, h).steps.empty());
    }

    cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
