// Procyon for Windows.
#include "window.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    return procyon::ui::run_app(instance, command_line ? command_line : L"", show_command);
}
