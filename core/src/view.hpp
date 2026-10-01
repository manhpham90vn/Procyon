#pragma once

#include <deque>
#include <string>
#include <vector>

#include "procyon/procyon.h"

namespace procyon {

struct View {
    std::vector<pc_row> rows;
    std::deque<std::string> strings;  // deque keeps c_str() pointers stable while growing
};

// Filters, groups/trees and sorts processes into display-ordered rows.
void build_view(const std::vector<pc_process> &processes, const pc_view_query &query, View &view);

}  // namespace procyon
