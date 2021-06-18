#include <algorithm>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/properties.hpp>

#include "midgard/util.h"
#include "sif/costconstants.h"
#include "sif/recost.h"
#include "thor/chinese_postman_graph.h"
#include "thor/worker.h"
#include "tyr/serializers.h"

using namespace valhalla::baldr;
using namespace valhalla::midgard;
using namespace valhalla::sif;

namespace valhalla {
namespace thor {

midgard::PointLL to_ll(const valhalla::Location& l) {
  return midgard::PointLL{l.ll().lng(), l.ll().lat()};
}
midgard::PointLL thor_worker_t::getPointLL(baldr::GraphId node) {
  const NodeInfo* ni_start = reader->nodeinfo(node);
  graph_tile_ptr tile = reader->GetGraphTile(node);
  return ni_start->latlng(tile->header()->base_ll());
}
std::string pointLLToJson(const midgard::PointLL l) {
  // To be something like this {"lat":40.739735,"lon":-73.979713}
  std::string json = "{";
  json += "\"lat\":";
  json += std::to_string(l.lat());

  json += ",\"lon\":";
  json += std::to_string(l.lng());

  json += "}";

  return json;
}

std::string locationsToJson(std::vector<midgard::PointLL> locations) {
  // To be something like this [{"lat":40.744014,"lon":-73.990508},
  // {"lat":40.739735,"lon":-73.979713}]
  std::string json = "[";
  bool extraCharacter = false;
  for (const auto& location : locations) {
    // std::cout << kv.first << " has value " << kv.second << std::endl;
    json += pointLLToJson(location) + ", ";
    extraCharacter = true;
  }
  // remove last two character ", "
  if (extraCharacter) {
    json.pop_back();
    json.pop_back();
  }

  json += "]";
  return json;
}

inline float find_percent_along(const valhalla::Location& location, const GraphId& edge_id) {
  for (const auto& e : location.path_edges()) {
    if (e.graph_id() == edge_id)
      return e.percent_along();
  }
  throw std::logic_error("Could not find candidate edge for the location");
}

void buildPath(GraphReader& graphreader,
               const Options& /*options*/,
               const valhalla::Location& origin,
               const valhalla::Location& dest,
               const baldr::TimeInfo& time_info,
               const bool invariant,
               std::vector<GraphId> path_edges,
               const std::shared_ptr<sif::DynamicCost>& costing_) {
  // Build a vector of path info
  for (auto edge_id : path_edges) {
    std::cout << edge_id << ", ";
  }
  std::cout << std::endl;

  // once we recovered the whole path we should construct list of PathInfo objects
  // set of edges recovered from shortcuts (excluding shortcut's start edges)
  std::unordered_set<GraphId> recovered_inner_edges;

  std::vector<PathInfo> path;
  path.reserve(path_edges.size());

  auto edge_itr = path_edges.begin();
  const auto edge_cb = [&edge_itr, &path_edges]() {
    return (edge_itr == path_edges.end()) ? GraphId{} : (*edge_itr++);
  };

  const auto label_cb = [&path, &recovered_inner_edges](const EdgeLabel& label) {
    path.emplace_back(label.mode(), label.cost(), label.edgeid(), 0, label.restriction_idx(),
                      label.transition_cost(), recovered_inner_edges.count(label.edgeid()));
  };

  float source_pct;
  try {
    source_pct = find_percent_along(origin, path_edges.front());
  } catch (...) { throw std::logic_error("Could not find candidate edge used for origin label"); }

  float target_pct;
  try {
    target_pct = find_percent_along(dest, path_edges.back());
  } catch (...) {
    throw std::logic_error("Could not find candidate edge used for destination label");
  }

  // recost edges in final path; ignore access restrictions
  try {
    sif::recost_forward(graphreader, *costing_, edge_cb, label_cb, source_pct, target_pct, time_info,
                        invariant, true);
  } catch (const std::exception& e) {
    LOG_ERROR(std::string("Bi-directional astar failed to recost final path: ") + e.what());
  }

  for (auto p : path) {
    std::cout << p.edgeid << ", " << p.elapsed_cost.cost << ", " << p.transition_cost.cost
              << std::endl;
  }
}

std::string thor_worker_t::computeFloydWarshall(std::vector<midgard::PointLL> sources,
                                                std::vector<midgard::PointLL> targets,
                                                std::string costing) {
  Api request;
  // Update request with source and target, also costing
  std::string jsonMatrixRequest = "{\"sources\":" + locationsToJson(sources) +
                                  ", \"targets\":" + locationsToJson(targets) + ",\"costing\":\"" +
                                  costing + "\"}";
  ParseApi(jsonMatrixRequest, Options::sources_to_targets, request);
  std::cout << "matrix result:\n" << matrix(request);
  return matrix(request);
}

void thor_worker_t::chinese_postman(Api& request) {

  baldr::DateTime::tz_sys_info_cache_t tz_cache_;

  auto correlated = request.options().locations();
  auto it = correlated.begin();
  auto origin = &it;
  it++;
  auto destination = &it;

  valhalla::Location originLocation = **origin;
  valhalla::Location destinationLocation = **destination;

  midgard::PointLL originPoint = to_ll(originLocation);
  midgard::PointLL destinationPoint = to_ll(destinationLocation);

  ChinesePostmanGraph G;
  // Only for auto for now
  const auto& costing_ = mode_costing[Costing::auto_];

  std::cout << "thor_worker_t::chinese_postman" << std::endl;
  // time this whole method and save that statistic
  auto _ = measure_scope_time(request, "thor_worker_t::isochrones");

  parse_locations(request);
  auto costing = parse_costing(request);
  auto& options = *request.mutable_options();

  auto* co = options.mutable_costing_options(options.costing());
  std::list<std::string> avoid_edge_ids;

  for (auto& avoid_edge : co->avoid_edges()) {
    avoid_edge_ids.push_back(std::to_string(GraphId(avoid_edge.id())));
  }

  bool originNodeFound = false;
  CPVertex originVertex;

  // Add chinese edges to internal set
  for (auto& edge : co->chinese_edges()) {
    // skip the edge if it's not allowed (reverse one way)
    const DirectedEdge* directed_edge = reader->directededge(baldr::GraphId(edge.id()));
    if (!directed_edge->forward()) {
      continue;
    }
    bool found = (std::find(avoid_edge_ids.begin(), avoid_edge_ids.end(),
                            std::to_string(GraphId(edge.id()))) != avoid_edge_ids.end());
    if (found)
      continue;
    GraphId start_node = reader->edge_startnode(GraphId(edge.id()));
    GraphId end_node = reader->edge_endnode(GraphId(edge.id()));
    CPVertex start_vertex = CPVertex(start_node);
    if (!originNodeFound) {
      originVertex = start_vertex;
      originNodeFound = true;
    }
    G.addVertex(start_vertex);
    CPVertex end_vertex = CPVertex(end_node);
    G.addVertex(end_vertex);
    // The cost of an edge is not relevant for the graph since we need to visit all the edges.
    // For a simplicity, I put Cost(1, 1) for it.
    // The cost is only considered when matching the unbalanced nodes.
    Cost cost(1, 1);
    CPEdge cpEdge(cost, baldr::GraphId(edge.id()));
    G.addEdge(start_vertex, end_vertex, cpEdge);
  }

  std::cout << "Num of vertices: " << G.numVertices() << std::endl;
  std::cout << "Num of edges: " << G.numEdges() << std::endl;

  if (G.getUnbalancedVertices().size() == 0) {
    std::vector<GraphId> edgeGraphIds = G.computeIdealEulerCycle(originVertex);
    std::cout << "Ideal graph" << std::endl;

    bool invariant = options.has_date_time_type() && options.date_time_type() == Options::invariant;
    auto time_info = TimeInfo::make(originLocation, *reader, &tz_cache_);
    buildPath(*reader, options, originLocation, destinationLocation, time_info, invariant,
              edgeGraphIds, costing_);
  } else {
    std::cout << "Non Ideal graph" << std::endl;
    std::vector<midgard::PointLL> overPoints; // Node that has too many incoming
    std::vector<midgard::PointLL> underPoints;
    std::vector<midgard::PointLL> locations;
    for (auto const& v : G.getUnbalancedVertices()) {
      auto l = getPointLL(GraphId(v.first));
      std::cout << "location (" << v.first << "): " << l.lng() << ", " << l.lat() << std::endl;
      locations.push_back(l);
      if (v.second > 0) {
        overPoints.push_back(l);
      } else if (v.second < 0) {
        underPoints.push_back(l);
      }
    }
    std::string matrixOutput = computeFloydWarshall(overPoints, underPoints, costing);
    std::cout << "matrix output:\n" << matrixOutput;
  }
}

} // namespace thor
} // namespace valhalla
