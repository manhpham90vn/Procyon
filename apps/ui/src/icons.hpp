// Icons as vector paths: Lucide SVGs (icons_data.hpp) parsed once into a Path on the 24x24 grid.
#pragma once

#include <string_view>

#include "ui.hpp"

namespace procyon::ui::icons {

struct Icon {
    Path outline;  // every element, for the stroke
    Path closed;   // the closed figures alone, for a fill
};

// The icon by its Lucide name, or null when it isn't in icons_data.hpp.
const Icon *lucide(std::string_view name);

// Parses the inner elements of a 24x24 Lucide SVG (path, rect, circle, ellipse, line, polyline,
// polygon). Exposed for tests.
Icon parse_svg(std::string_view svg);

}  // namespace procyon::ui::icons
