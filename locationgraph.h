#ifndef LOCATIONGRAPH_H
#define LOCATIONGRAPH_H

#include <string>
#include <vector>
#include "latlong.h"
#include "transaction.h"

// ---------------------------------------------------------------
// LOCATION GRAPH MODULE
//
// Vertices : every city in locations.csv
// Edges    : two cities are linked (undirected, weight = great-circle
//            distance in km) if they are within GRAPH_MAX_LINK_KM of each
//            other OR one is among the other's K nearest cities. The
//            second rule guarantees no city is left isolated.
//
// Provides: adjacency list, BFS, DFS, Dijkstra shortest route,
//           connected components, nearby-city search and the travel
//           trail of a card (used for fraud investigation).
// ---------------------------------------------------------------

const int    GRAPH_NEAREST_NEIGHBOURS = 3;
const double GRAPH_MAX_LINK_KM        = 1000.0;   // also link cities this close

struct GraphEdge
{
    int    to;
    double km;
};

struct RouteResult
{
    bool                found = false;
    double              totalKm = 0.0;
    std::vector<std::string> path;      // city names, start -> end
};

struct TrailStep
{
    std::string location;
    std::string timestamp;
    double      kmFromPrevious = 0.0;   // straight-line distance
};

struct CardTrail
{
    std::vector<TrailStep> steps;
    double totalKm = 0.0;
    int    distinctLocations = 0;
};

class LocationGraph
{
public:
    // Builds the graph from locations.csv. Returns false if the file
    // is missing or empty.
    bool buildFromFile(int nearestNeighbours = GRAPH_NEAREST_NEIGHBOURS,
                       double maxLinkKm = GRAPH_MAX_LINK_KM);

    // Builds from an explicit list (used by tests).
    void build(const std::vector<Location>& cities,
               int nearestNeighbours = GRAPH_NEAREST_NEIGHBOURS,
               double maxLinkKm = GRAPH_MAX_LINK_KM);

    int  cityCount() const { return static_cast<int>(names.size()); }
    int  edgeCount() const;
    bool hasCity(const std::string& name) const;

    // Direct (straight-line) distance between two cities in the graph.
    bool directDistance(const std::string& a, const std::string& b,
                        double& km) const;

    // Traversals - return city names in visiting order.
    std::vector<std::string> bfs(const std::string& start) const;
    std::vector<std::string> dfs(const std::string& start) const;

    // Shortest route along graph edges (Dijkstra, O(V^2)).
    RouteResult shortestRoute(const std::string& from,
                              const std::string& to) const;

    int  connectedComponents() const;

    // All cities within radiusKm of `name` (straight-line), nearest first.
    std::vector<std::pair<std::string, double>>
    nearby(const std::string& name, double radiusKm) const;

    // Travel trail of one card, built from its ALLOWED transactions in
    // time order (uses the sorting module).
    CardTrail cardTrail(int cardNo,
                        const std::vector<Transaction>& history) const;

    void displayAdjacency() const;
    void displayRoute(const RouteResult& r) const;
    void displayTrail(int cardNo, const CardTrail& trail) const;

private:
    std::vector<std::string> names;
    std::vector<Location>    cities;
    std::vector<std::vector<GraphEdge>> adj;

    int  indexOf(const std::string& name) const;
    void addUndirectedEdge(int a, int b, double km);
    bool edgeExists(int a, int b) const;
    void dfsVisit(int v, std::vector<bool>& seen,
                  std::vector<std::string>& order) const;
};

#endif
