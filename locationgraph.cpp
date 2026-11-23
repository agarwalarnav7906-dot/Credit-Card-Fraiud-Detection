#include <iostream>
#include <iomanip>
#include <queue>
#include <set>
#include <algorithm>
#include <limits>

#include "locationgraph.h"
#include "sortmodule.h"
#include "time_module.h"

using namespace std;

// ------------------------------------------------------------
// Construction
// ------------------------------------------------------------

int LocationGraph::indexOf(const string& name) const
{
    for (size_t i = 0; i < names.size(); i++)
    {
        // case-insensitive compare so "delhi" finds "Delhi"
        if (names[i].size() != name.size()) continue;

        bool same = true;
        for (size_t k = 0; k < name.size(); k++)
        {
            if (tolower(static_cast<unsigned char>(names[i][k])) !=
                tolower(static_cast<unsigned char>(name[k])))
            {
                same = false;
                break;
            }
        }
        if (same) return static_cast<int>(i);
    }
    return -1;
}

bool LocationGraph::hasCity(const string& name) const
{
    return indexOf(name) >= 0;
}

bool LocationGraph::edgeExists(int a, int b) const
{
    for (const GraphEdge& e : adj[static_cast<size_t>(a)])
        if (e.to == b) return true;
    return false;
}

void LocationGraph::addUndirectedEdge(int a, int b, double km)
{
    if (a == b || edgeExists(a, b)) return;

    adj[static_cast<size_t>(a)].push_back({b, km});
    adj[static_cast<size_t>(b)].push_back({a, km});
}

void LocationGraph::build(const vector<Location>& list, int k, double maxLinkKm)
{
    cities = list;
    names.clear();
    adj.clear();

    for (const Location& c : cities) names.push_back(c.name);

    int n = static_cast<int>(cities.size());
    adj.assign(static_cast<size_t>(n), vector<GraphEdge>());

    // Link cities that are close enough, and always link each city to
    // its k nearest cities so none is isolated.
    for (int i = 0; i < n; i++)
    {
        vector<pair<double, int>> dist;

        for (int j = 0; j < n; j++)
        {
            if (i == j) continue;

            double d = calculateDistance(
                cities[static_cast<size_t>(i)].latitude,
                cities[static_cast<size_t>(i)].longitude,
                cities[static_cast<size_t>(j)].latitude,
                cities[static_cast<size_t>(j)].longitude);

            dist.push_back({d, j});
        }

        sort(dist.begin(), dist.end());

        for (size_t x = 0; x < dist.size(); x++)
        {
            bool isNearest = static_cast<int>(x) < k;
            bool isClose   = dist[x].first <= maxLinkKm;

            if (isNearest || isClose)
                addUndirectedEdge(i, dist[x].second, dist[x].first);
        }
    }
}

bool LocationGraph::buildFromFile(int k, double maxLinkKm)
{
    vector<Location> all;

    if (!loadAllLocations(all) || all.empty()) return false;

    build(all, k, maxLinkKm);
    return true;
}

int LocationGraph::edgeCount() const
{
    int total = 0;
    for (const auto& list : adj) total += static_cast<int>(list.size());
    return total / 2;                   // each edge is stored twice
}

bool LocationGraph::directDistance(const string& a, const string& b,
                                   double& km) const
{
    int i = indexOf(a), j = indexOf(b);
    if (i < 0 || j < 0) return false;

    km = calculateDistance(
        cities[static_cast<size_t>(i)].latitude,
        cities[static_cast<size_t>(i)].longitude,
        cities[static_cast<size_t>(j)].latitude,
        cities[static_cast<size_t>(j)].longitude);
    return true;
}

// ------------------------------------------------------------
// Traversals
// ------------------------------------------------------------

vector<string> LocationGraph::bfs(const string& start) const
{
    vector<string> order;
    int s = indexOf(start);
    if (s < 0) return order;

    vector<bool> seen(names.size(), false);
    queue<int> q;

    seen[static_cast<size_t>(s)] = true;
    q.push(s);

    while (!q.empty())
    {
        int v = q.front();
        q.pop();
        order.push_back(names[static_cast<size_t>(v)]);

        for (const GraphEdge& e : adj[static_cast<size_t>(v)])
        {
            if (!seen[static_cast<size_t>(e.to)])
            {
                seen[static_cast<size_t>(e.to)] = true;
                q.push(e.to);
            }
        }
    }
    return order;
}

void LocationGraph::dfsVisit(int v, vector<bool>& seen,
                             vector<string>& order) const
{
    seen[static_cast<size_t>(v)] = true;
    order.push_back(names[static_cast<size_t>(v)]);

    for (const GraphEdge& e : adj[static_cast<size_t>(v)])
        if (!seen[static_cast<size_t>(e.to)])
            dfsVisit(e.to, seen, order);
}

vector<string> LocationGraph::dfs(const string& start) const
{
    vector<string> order;
    int s = indexOf(start);
    if (s < 0) return order;

    vector<bool> seen(names.size(), false);
    dfsVisit(s, seen, order);
    return order;
}

int LocationGraph::connectedComponents() const
{
    vector<bool> seen(names.size(), false);
    int components = 0;

    for (size_t i = 0; i < names.size(); i++)
    {
        if (seen[i]) continue;

        components++;
        vector<string> ignore;
        dfsVisit(static_cast<int>(i), seen, ignore);
    }
    return components;
}

// ------------------------------------------------------------
// Dijkstra (simple O(V^2) version - the graph is small)
// ------------------------------------------------------------

RouteResult LocationGraph::shortestRoute(const string& from,
                                         const string& to) const
{
    RouteResult result;

    int s = indexOf(from), t = indexOf(to);
    if (s < 0 || t < 0) return result;

    size_t n = names.size();
    const double INF = numeric_limits<double>::infinity();

    vector<double> dist(n, INF);
    vector<int>    parent(n, -1);
    vector<bool>   done(n, false);

    dist[static_cast<size_t>(s)] = 0.0;

    for (size_t round = 0; round < n; round++)
    {
        // pick the closest unfinished vertex
        int u = -1;
        for (size_t i = 0; i < n; i++)
            if (!done[i] && (u < 0 || dist[i] < dist[static_cast<size_t>(u)]))
                u = static_cast<int>(i);

        if (u < 0 || dist[static_cast<size_t>(u)] == INF) break;

        done[static_cast<size_t>(u)] = true;

        for (const GraphEdge& e : adj[static_cast<size_t>(u)])
        {
            double nd = dist[static_cast<size_t>(u)] + e.km;
            if (nd < dist[static_cast<size_t>(e.to)])
            {
                dist[static_cast<size_t>(e.to)] = nd;
                parent[static_cast<size_t>(e.to)] = u;
            }
        }
    }

    if (dist[static_cast<size_t>(t)] == INF) return result;   // unreachable

    for (int v = t; v != -1; v = parent[static_cast<size_t>(v)])
        result.path.push_back(names[static_cast<size_t>(v)]);

    reverse(result.path.begin(), result.path.end());

    result.found = true;
    result.totalKm = dist[static_cast<size_t>(t)];
    return result;
}

// ------------------------------------------------------------
// Nearby cities
// ------------------------------------------------------------

vector<pair<string, double>>
LocationGraph::nearby(const string& name, double radiusKm) const
{
    vector<pair<string, double>> out;

    int s = indexOf(name);
    if (s < 0) return out;

    for (size_t j = 0; j < names.size(); j++)
    {
        if (static_cast<int>(j) == s) continue;

        double d = calculateDistance(
            cities[static_cast<size_t>(s)].latitude,
            cities[static_cast<size_t>(s)].longitude,
            cities[j].latitude, cities[j].longitude);

        if (d <= radiusKm) out.push_back({names[j], d});
    }

    sort(out.begin(), out.end(),
         [](const pair<string, double>& a, const pair<string, double>& b)
         { return a.second < b.second; });

    return out;
}

// ------------------------------------------------------------
// Card travel trail
// ------------------------------------------------------------

CardTrail LocationGraph::cardTrail(int cardNo,
                                   const vector<Transaction>& history) const
{
    CardTrail trail;

    vector<Transaction> mine;
    for (const Transaction& t : history)
        if (t.cardNo == cardNo && !t.blocked) mine.push_back(t);

    // oldest -> newest, using the sorting module
    mine = sortTransactions(mine, SORT_BY_TIME, ASCENDING);

    set<string> distinct;

    for (size_t i = 0; i < mine.size(); i++)
    {
        TrailStep step;
        step.location  = mine[i].location;
        step.timestamp = mine[i].timestamp;

        if (i > 0)
        {
            step.kmFromPrevious = calculateDistance(
                mine[i - 1].latitude, mine[i - 1].longitude,
                mine[i].latitude,     mine[i].longitude);
            trail.totalKm += step.kmFromPrevious;
        }

        distinct.insert(step.location);
        trail.steps.push_back(step);
    }

    trail.distinctLocations = static_cast<int>(distinct.size());
    return trail;
}

// ------------------------------------------------------------
// Display
// ------------------------------------------------------------

void LocationGraph::displayAdjacency() const
{
    cout << "\n========== LOCATION GRAPH ==========\n";
    cout << "Cities: " << cityCount()
         << " | Links: " << edgeCount()
         << " | Connected groups: " << connectedComponents() << "\n\n";

    cout << fixed << setprecision(0);

    for (size_t i = 0; i < names.size(); i++)
    {
        cout << left << setw(12) << names[i] << "-> ";

        for (size_t k = 0; k < adj[i].size(); k++)
        {
            if (k > 0) cout << ", ";
            cout << names[static_cast<size_t>(adj[i][k].to)]
                 << " (" << adj[i][k].km << " km)";
        }
        cout << '\n';
    }

    cout.unsetf(ios::floatfield);
    cout << setprecision(6);
}

void LocationGraph::displayRoute(const RouteResult& r) const
{
    if (!r.found)
    {
        cout << "No route found (unknown city or not connected).\n";
        return;
    }

    cout << "Route : ";
    for (size_t i = 0; i < r.path.size(); i++)
    {
        if (i > 0) cout << " -> ";
        cout << r.path[i];
    }
    cout << "\nTotal : " << fixed << setprecision(1) << r.totalKm << " km\n";
    cout.unsetf(ios::floatfield);
    cout << setprecision(6);
}

void LocationGraph::displayTrail(int cardNo, const CardTrail& trail) const
{
    cout << "\n========== TRAVEL TRAIL: CARD " << cardNo << " ==========\n";

    if (trail.steps.empty())
    {
        cout << "No completed transactions for this card.\n";
        return;
    }

    cout << fixed << setprecision(1);

    for (size_t i = 0; i < trail.steps.size(); i++)
    {
        const TrailStep& s = trail.steps[i];

        cout << (i + 1) << ". " << left << setw(12) << s.location
             << setw(21) << s.timestamp;

        if (i > 0) cout << " (+" << s.kmFromPrevious << " km)";
        cout << '\n';
    }

    cout << "Total distance travelled : " << trail.totalKm << " km\n";
    cout << "Distinct locations       : " << trail.distinctLocations << '\n';

    cout.unsetf(ios::floatfield);
    cout << setprecision(6);
}
