#include "affinitygraph/core.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace affinitygraph {
namespace fs = std::filesystem;
namespace {
std::string read_one(const fs::path &path) {
  std::ifstream input(path);
  std::string value;
  if (!(input >> value)) return {};
  return value;
}
} // namespace

HardwareGraph HardwareGraph::discover(const std::vector<int> &envelope,
                                      const std::string &sys_root) {
  HardwareGraph graph;
  for (int id : envelope) {
    fs::path cpu_path = fs::path(sys_root) / "devices/system/cpu" / ("cpu" + std::to_string(id));
    if (!fs::exists(cpu_path)) throw std::runtime_error("CPU " + std::to_string(id) + " is outside host topology");
    bool online = id == 0 || read_one(cpu_path / "online") != "0";
    if (!online) throw std::runtime_error("CPU " + std::to_string(id) + " is offline");
    int node = -1;
    for (const auto &entry : fs::directory_iterator(cpu_path)) {
      auto name = entry.path().filename().string();
      if (name.rfind("node", 0) == 0) {
        node = std::stoi(name.substr(4));
        break;
      }
    }
    if (node < 0) node = 0;
    graph.cpus.push_back({id, node, online});
  }
  for (int from : graph.nodes()) {
    fs::path distance = fs::path(sys_root) / "devices/system/node" / ("node" + std::to_string(from)) / "distance";
    std::ifstream input(distance);
    double value;
    int to = 0;
    while (input >> value) graph.node_distance[{from, to++}] = value;
  }
  return graph;
}

void HardwareGraph::load_calibration(const std::string &path) {
  if (path.empty()) return;
  fs::path file(path);
  if (fs::is_directory(file)) file /= "hardware-node-edges.csv";
  std::ifstream input(file);
  if (!input) throw std::runtime_error("cannot open hardware calibration: " + file.string());
  std::string header;
  if (!std::getline(input, header) ||
      (header != "source_node,destination_node,same_socket,numa_distance,core_handoff_mean_ns,core_handoff_p95_ns,memory_load_mean_ns,memory_load_cv,stream_2t_triad_mbps,stream_32t_triad_mbps" &&
       header != "source_node,destination_node,same_socket,numa_distance,core_handoff_mean_ns,core_handoff_p95_ns,memory_load_mean_ns,memory_load_cv,stream_2t_triad_mbps,stream_32t_triad_mbps,is_estimated"))
    throw std::runtime_error("unsupported hardware calibration schema: " + file.string());
  std::string line;
  size_t loaded = 0;
  while (std::getline(input, line)) {
    std::stringstream row(line);
    std::vector<std::string> cells;
    std::string cell;
    while (std::getline(row, cell, ',')) cells.push_back(cell);
    if (cells.size() != 10 && cells.size() != 11)
      throw std::runtime_error("invalid hardware calibration row");
    int from = std::stoi(cells[0]), to = std::stoi(cells[1]);
    double distance = std::stod(cells[3]), handoff = std::stod(cells[4]);
    node_distance[{from, to}] = distance;
    node_calibration[{from, to}] = {
        handoff, std::stod(cells[5]), std::stod(cells[6]), std::stod(cells[7]),
        std::stod(cells[8]), std::stod(cells[9])};
    for (const auto &a : cpus) for (const auto &b : cpus)
      if (a.node == from && b.node == to) cpu_latency[{a.id, b.id}] = a.id == b.id ? 0.0 : handoff;
    ++loaded;
  }
  for (int from : nodes()) for (int to : nodes())
    if (!node_distance.contains({from, to})) throw std::runtime_error("calibration does not cover resource envelope nodes");
  if (!loaded) throw std::runtime_error("empty hardware calibration");
}

double HardwareGraph::latency(int from_cpu, int to_cpu) const {
  if (from_cpu == to_cpu) return 0.0;
  if (auto it = cpu_latency.find({from_cpu, to_cpu}); it != cpu_latency.end()) return it->second;
  int from_node = -1, to_node = -1;
  for (const auto &cpu : cpus) {
    if (cpu.id == from_cpu) from_node = cpu.node;
    if (cpu.id == to_cpu) to_node = cpu.node;
  }
  if (auto it = node_distance.find({from_node, to_node}); it != node_distance.end()) return it->second;
  return from_cpu == to_cpu ? 0.0 : (from_node == to_node ? 10.0 : 20.0);
}

std::vector<int> HardwareGraph::nodes() const {
  std::set<int> result;
  for (const auto &cpu : cpus) result.insert(cpu.node);
  return {result.begin(), result.end()};
}

std::vector<int> HardwareGraph::cpus_in_node(int node) const {
  std::vector<int> result;
  for (const auto &cpu : cpus) if (cpu.node == node && cpu.online) result.push_back(cpu.id);
  std::sort(result.begin(), result.end());
  return result;
}
} // namespace affinitygraph
