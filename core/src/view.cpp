#include "view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace procyon {
namespace {

#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

struct Node {
    pc_row row{};
    const char *name = "";
    const char *user = "";
    int32_t pid = 0;
    std::vector<size_t> children;
};

// ---- case folding: the filter is UTF-8 and case-insensitive beyond ASCII ----

// Decodes one UTF-8 sequence at `at`; returns its length (1 for a stray byte, reported as-is so it
// still compares byte for byte).
size_t decode_utf8(std::string_view text, size_t at, char32_t &out) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    size_t length = 1;  // ASCII, or a stray byte passed through as-is
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        out = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        out = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        out = lead & 0x07;
    }
    if (length == 1 || at + length > text.size()) {
        out = lead;
        return 1;
    }
    for (size_t i = 1; i < length; ++i) {
        const unsigned char next = byte(at + i);
        if ((next & 0xC0) != 0x80) {
            out = lead;
            return 1;
        }
        out = (out << 6) | (next & 0x3F);
    }
    return length;
}

void append_utf8(std::string &out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Lowercases one code point. towlower covers whatever the C locale knows; the fallback folds the
// scripts process and user names are realistically written in (Latin-1, Latin Extended-A, Greek,
// Cyrillic) when the locale is plain "C", which only knows ASCII.
char32_t fold(char32_t cp) {
    if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
    const auto lowered = static_cast<char32_t>(std::towlower(static_cast<wint_t>(cp)));
    if (lowered != cp) return lowered;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;  // À-Þ
    if (cp == 0x178) return 0xFF;                                  // Ÿ -> ÿ
    if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
        // Latin Extended-A alternates upper/lower, even/odd, except in Ĺ-Ň (0x139-0x148) and Ź-Ž
        // (0x179-0x17E) where the pairs are odd/even.
        const bool odd_upper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        return (cp % 2 == 0) != odd_upper ? cp + 1 : cp;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;  // Greek Α-Ω
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;                 // Cyrillic А-Я
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;                 // Cyrillic Ѐ-Џ
    return cp;
}

void lowercase_into(std::string_view text, std::string &out) {
    out.clear();
    for (size_t at = 0; at < text.size();) {
        char32_t cp = 0;
        const size_t length = decode_utf8(text, at, cp);
        if (length == 1 && cp >= 0x80)
            out += text[at];  // not UTF-8: keep the byte
        else
            append_utf8(out, fold(cp));
        at += length;
    }
}

// The lowercased filter plus a reusable buffer, so matching a field allocates nothing in steady state.
class Matcher {
public:
    explicit Matcher(const char *filter) {
        if (filter) lowercase_into(filter, needle_);
    }
    bool empty() const { return needle_.empty(); }

    bool contains(std::string_view haystack) {
        if (needle_.empty()) return true;
        lowercase_into(haystack, buffer_);
        return buffer_.find(needle_) != std::string::npos;
    }

    bool matches(const pc_process &p) {
        if (needle_.empty()) return true;
        char pid[16];
        (void)std::snprintf(pid, sizeof(pid), "%d", p.pid);
        return contains(p.name) || contains(p.app_name) || contains(p.user) || std::strstr(pid, needle_.c_str());
    }

private:
    std::string needle_;
    std::string buffer_;
};

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
    node.row.gpu_percent = p.gpu_percent;
    node.row.power_watts = p.power_watts;
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
        case PC_COLUMN_GPU: return row.gpu_percent;
        case PC_COLUMN_POWER: return row.power_watts;
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

void build_grouped(const std::vector<pc_process> &processes, Matcher &filter, View &view, std::vector<Node> &nodes,
                   std::vector<size_t> &roots) {
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
        const bool group_matches = filter.contains(first.app_name);
        std::vector<size_t> members;
        for (size_t i : all)
            if (group_matches || filter.matches(processes[i])) members.push_back(i);
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
        group.row.net_rx_bps = group.row.net_tx_bps = group.row.gpu_percent = group.row.power_watts = -1;
        group.row.memory_bytes = -1;
        group.row.threads = -1;
        // Totals cover every member, not only the ones the filter lets through below: the group
        // row is the app's real footprint, and a filter narrows what is listed under it, not what
        // the app uses. process_count follows the same rule.
        for (size_t i : all) {
            const pc_process &p = processes[i];
            accumulate(group.row.cpu_percent, p.cpu_percent);
            accumulate(group.row.memory_bytes, p.memory_bytes);
            accumulate(group.row.disk_read_bps, p.disk_read_bps);
            accumulate(group.row.disk_write_bps, p.disk_write_bps);
            accumulate(group.row.net_rx_bps, p.net_rx_bps);
            accumulate(group.row.net_tx_bps, p.net_tx_bps);
            accumulate(group.row.threads, p.threads);
            accumulate(group.row.gpu_percent, p.gpu_percent);
            accumulate(group.row.power_watts, p.power_watts);
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

void build_tree(const std::vector<pc_process> &processes, Matcher &filter, std::vector<Node> &nodes,
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
    std::vector<char> included(count, filter.empty());
    if (!filter.empty()) {
        for (size_t i = 0; i < count; ++i) {
            if (!filter.matches(processes[i])) continue;
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
    Matcher filter(query.filter);

    std::vector<Node> nodes;
    std::vector<size_t> roots;
    nodes.reserve(processes.size() + 64);

    switch (query.mode) {
        case PC_VIEW_GROUPED: build_grouped(processes, filter, view, nodes, roots); break;
        case PC_VIEW_TREE: build_tree(processes, filter, nodes, roots); break;
        default:
            for (size_t i = 0; i < processes.size(); ++i) {
                if (!filter.matches(processes[i])) continue;
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
