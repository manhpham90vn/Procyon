#include "view.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace procyon {
namespace {

struct Node {
    pc_row row{};
    const char *name = "";
    const char *user = "";
    int32_t pid = 0;
    std::vector<size_t> children;
};

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(std::string_view haystack, const std::string &needle_lower) {
    if (needle_lower.empty()) return true;
    return lowercase(haystack).find(needle_lower) != std::string::npos;
}

bool matches(const pc_process &p, const std::string &needle) {
    if (needle.empty()) return true;
    return contains(p.name, needle) || contains(p.app_name, needle) || contains(p.user, needle) ||
           std::to_string(p.pid).find(needle) != std::string::npos;
}

Node process_node(const std::vector<pc_process> &processes, size_t index) {
    const pc_process &p = processes[index];
    Node node;
    node.name = p.name;
    node.user = p.user;
    node.pid = p.pid;
    node.row.process_index = static_cast<int32_t>(index);
    node.row.process_count = 1;
    node.row.cpu_percent = p.cpu_percent;
    node.row.memory_bytes = p.memory_bytes;
    node.row.disk_read_bps = p.disk_read_bps;
    node.row.disk_write_bps = p.disk_write_bps;
    node.row.net_rx_bps = p.net_rx_bps;
    node.row.net_tx_bps = p.net_tx_bps;
    node.row.threads = p.threads;
    node.row.group_pid = -1;
    return node;
}

// Sums known values; stays -1 when no member reports the metric.
template <typename T>
void accumulate(T &total, T value) {
    if (value < 0) return;
    total = total < 0 ? value : total + value;
}

double sort_number(const pc_row &row, int32_t column) {
    switch (column) {
        case PC_COLUMN_CPU: return row.cpu_percent;
        case PC_COLUMN_MEMORY: return static_cast<double>(row.memory_bytes);
        case PC_COLUMN_DISK_READ: return row.disk_read_bps;
        case PC_COLUMN_DISK_WRITE: return row.disk_write_bps;
        case PC_COLUMN_NET_RX: return row.net_rx_bps;
        case PC_COLUMN_NET_TX: return row.net_tx_bps;
        case PC_COLUMN_THREADS: return row.threads;
        default: return 0;
    }
}

struct Comparator {
    const std::vector<Node> &nodes;
    int32_t column;
    bool descending;

    bool operator()(size_t lhs, size_t rhs) const {
        const Node &a = nodes[lhs];
        const Node &b = nodes[rhs];
        int order = 0;
        switch (column) {
            case PC_COLUMN_NAME: order = strcasecmp(a.name, b.name); break;
            case PC_COLUMN_USER: order = strcasecmp(a.user, b.user); break;
            case PC_COLUMN_PID: order = (a.pid > b.pid) - (a.pid < b.pid); break;
            default: {
                const double x = sort_number(a.row, column), y = sort_number(b.row, column);
                // Unknown values always sink to the bottom, whatever the direction.
                if ((x < 0) != (y < 0)) return y < 0;
                order = (x > y) - (x < y);
            }
        }
        if (order != 0) return descending ? order > 0 : order < 0;
        return a.pid < b.pid;
    }
};

void emit(std::vector<Node> &nodes, size_t index, int32_t parent_row, int32_t depth, const Comparator &cmp,
          std::vector<pc_row> &rows) {
    Node &node = nodes[index];
    std::sort(node.children.begin(), node.children.end(), cmp);
    const auto row_index = static_cast<int32_t>(rows.size());
    pc_row row = node.row;
    row.parent_row = parent_row;
    row.depth = depth;
    row.child_count = static_cast<int32_t>(node.children.size());
    rows.push_back(row);
    for (size_t child : node.children) emit(nodes, child, row_index, depth + 1, cmp, rows);
}

void build_grouped(const std::vector<pc_process> &processes, const std::string &needle, View &view,
                   std::vector<Node> &nodes, std::vector<size_t> &roots) {
    std::unordered_map<std::string_view, std::vector<size_t>> groups;
    std::vector<std::string_view> order;
    for (size_t i = 0; i < processes.size(); ++i) {
        std::string_view id = processes[i].app_id;
        auto [it, inserted] = groups.try_emplace(id);
        if (inserted) order.push_back(id);
        it->second.push_back(i);
    }

    for (std::string_view id : order) {
        const auto &all = groups[id];
        const pc_process &first = processes[all.front()];
        const bool group_matches = contains(first.app_name, needle);
        std::vector<size_t> members;
        for (size_t i : all)
            if (group_matches || matches(processes[i], needle)) members.push_back(i);
        if (members.empty()) continue;

        if (all.size() == 1) {
            roots.push_back(nodes.size());
            nodes.push_back(process_node(processes, members.front()));
            continue;
        }

        // Representative process: the member whose parent is outside the group, lowest pid first.
        std::unordered_map<int32_t, bool> in_group;
        for (size_t i : all) in_group[processes[i].pid] = true;
        const pc_process *main = &processes[all.front()];
        for (size_t i : all) {
            const pc_process &p = processes[i];
            const bool is_root = !in_group.count(p.ppid);
            const bool main_is_root = !in_group.count(main->ppid);
            if ((is_root && !main_is_root) || (is_root == main_is_root && p.pid < main->pid)) main = &p;
        }

        view.strings.emplace_back(id);
        const char *group_id = view.strings.back().c_str();
        view.strings.emplace_back(first.app_name);
        const char *group_name = view.strings.back().c_str();

        Node group;
        group.name = group_name;
        group.user = main->user;
        group.pid = main->pid;
        group.row.process_index = -1;
        group.row.group_id = group_id;
        group.row.group_name = group_name;
        group.row.group_pid = main->pid;
        group.row.cpu_percent = group.row.disk_read_bps = group.row.disk_write_bps = -1;
        group.row.net_rx_bps = group.row.net_tx_bps = -1;
        group.row.memory_bytes = -1;
        group.row.threads = -1;
        for (size_t i : all) {
            const pc_process &p = processes[i];
            accumulate(group.row.cpu_percent, p.cpu_percent);
            accumulate(group.row.memory_bytes, p.memory_bytes);
            accumulate(group.row.disk_read_bps, p.disk_read_bps);
            accumulate(group.row.disk_write_bps, p.disk_write_bps);
            accumulate(group.row.net_rx_bps, p.net_rx_bps);
            accumulate(group.row.net_tx_bps, p.net_tx_bps);
            accumulate(group.row.threads, p.threads);
        }
        group.row.process_count = static_cast<int32_t>(all.size());

        const size_t group_index = nodes.size();
        roots.push_back(group_index);
        nodes.push_back(group);
        for (size_t i : members) {
            nodes[group_index].children.push_back(nodes.size());
            nodes.push_back(process_node(processes, i));
        }
    }
}

void build_tree(const std::vector<pc_process> &processes, const std::string &needle, std::vector<Node> &nodes,
                std::vector<size_t> &roots) {
    const size_t count = processes.size();
    std::unordered_map<int32_t, size_t> by_pid;
    for (size_t i = 0; i < count; ++i) by_pid[processes[i].pid] = i;

    std::vector<long> parent(count, -1);
    for (size_t i = 0; i < count; ++i) {
        auto it = by_pid.find(processes[i].ppid);
        if (it != by_pid.end() && it->second != i) parent[i] = static_cast<long>(it->second);
    }

    // A node is shown if it matches or has a matching descendant.
    std::vector<char> included(count, needle.empty());
    if (!needle.empty()) {
        for (size_t i = 0; i < count; ++i) {
            if (!matches(processes[i], needle)) continue;
            long current = static_cast<long>(i);
            for (size_t steps = 0; current >= 0 && !included[current] && steps <= count; ++steps) {
                included[current] = 1;
                current = parent[current];
            }
        }
    }

    std::vector<long> node_of(count, -1);
    for (size_t i = 0; i < count; ++i) {
        if (!included[i]) continue;
        node_of[i] = static_cast<long>(nodes.size());
        nodes.push_back(process_node(processes, i));
    }
    for (size_t i = 0; i < count; ++i) {
        if (node_of[i] < 0) continue;
        const long p = parent[i];
        if (p >= 0 && node_of[p] >= 0)
            nodes[node_of[p]].children.push_back(node_of[i]);
        else
            roots.push_back(node_of[i]);
    }
}

}  // namespace

void build_view(const std::vector<pc_process> &processes, const pc_view_query &query, View &view) {
    view.rows.clear();
    view.strings.clear();
    const std::string needle = query.filter ? lowercase(query.filter) : std::string();

    std::vector<Node> nodes;
    std::vector<size_t> roots;
    nodes.reserve(processes.size() + 64);

    switch (query.mode) {
        case PC_VIEW_GROUPED: build_grouped(processes, needle, view, nodes, roots); break;
        case PC_VIEW_TREE: build_tree(processes, needle, nodes, roots); break;
        default:
            for (size_t i = 0; i < processes.size(); ++i) {
                if (!matches(processes[i], needle)) continue;
                roots.push_back(nodes.size());
                nodes.push_back(process_node(processes, i));
            }
    }

    const Comparator cmp{nodes, query.sort_column, query.descending};
    std::sort(roots.begin(), roots.end(), cmp);
    if (query.limit > 0 && roots.size() > static_cast<size_t>(query.limit)) roots.resize(query.limit);
    view.rows.reserve(nodes.size());
    for (size_t root : roots) emit(nodes, root, -1, 0, cmp, view.rows);
}

}  // namespace procyon
