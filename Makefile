# Common entry points, the same names on macOS, Windows and Linux. `make help` lists them.
#
# macOS needs Xcode 27 (see docs/development.md). Windows needs Visual Studio 2022 with the C++
# workload and GNU Make (`winget install ezwinports.make`); `make tools` fetches CMake, Ninja and
# clang-format into build\tools when Visual Studio's CMake component is not installed. Linux needs a
# C++20 compiler, CMake, Ninja and GTK 4 (`make tools` prints the packages to install).
.PHONY: help build test app core run bench screenshots format lint tokens icon tools clean ci install package

ifeq ($(OS),Windows_NT)
# ---- Windows ---- (each `make help` lists the targets of its own block)
# Recipes run through cmd.exe whatever shell `make` was started from (Git Bash, PowerShell, cmd).
SHELL := cmd.exe
.SHELLFLAGS := /c
PS := powershell -NoProfile -ExecutionPolicy Bypass -File

help: ## List targets
	@$(PS) scripts\make-help.ps1

build: ## Debug build of the core, CLI, tests and app into build\windows-debug
	scripts\build-windows.cmd debug --no-tests

test: ## Release build and the core tests
	scripts\build-windows.cmd release

core: test ## Build the C++ core and CLI with CMake, run the core tests

app: ## Release dist\windows\Procyon.exe
	scripts\build-windows.cmd release --no-tests

run: app ## Build and open the app
	start "" dist\windows\Procyon.exe

bench: ## Measure the app against the spec's performance targets (python: make tools fetches one)
	powershell -NoProfile -ExecutionPolicy Bypass -Command ". scripts\dev-env-windows.ps1; & (Find-Python) scripts\bench-windows.py; exit $$LASTEXITCODE"

screenshots: app ## Render the main screens, light and dark, into dist\windows\screenshots
	$(PS) scripts\screenshots-windows.ps1

format: ## Format C/C++ sources in place (and regenerate tokens when python is available)
	$(PS) scripts\format-windows.ps1

lint: ## Read-only checks (tokens, clang-format, python)
	$(PS) scripts\lint-windows.ps1

tokens: ## Regenerate design tokens and the process catalog (needs python)
	python scripts\gen-tokens.py
	python scripts\gen-catalog.py

icon: ## Re-render the Windows app icon from the shared PNG
	$(PS) scripts\make-icon-windows.ps1

tools: ## Fetch CMake, Ninja and clang-format into build\tools
	$(PS) scripts\tools-windows.ps1

clean: ## Remove build outputs (keeps build\tools)
	if exist build\windows rmdir /s /q build\windows
	if exist build\windows-debug rmdir /s /q build\windows-debug
	if exist dist rmdir /s /q dist

ci: lint test app ## What CI runs

else
UNAME_S := $(shell uname -s)
endif

ifeq ($(UNAME_S),Linux)
# ---- Linux ----
PREFIX ?= $(HOME)/.local
SCREENSHOT_PAGES ?= overview processes cpu history inspect startup services

help: ## List targets
	@awk -F':.*## ' '/^# ---- /{f = /Linux/; next} f && /^[a-z-]+:.*## / {printf "  %-12s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

build: ## Debug build of the core, CLI, tests and app into build/linux-debug
	scripts/build-linux.sh debug --no-tests

test: ## Release build and the core tests
	scripts/build-linux.sh release

core: test ## Build the C++ core and CLI with CMake, run the core tests

app: ## Release dist/linux/procyon (GTK 4, fonts embedded)
	scripts/build-linux.sh release --no-tests

run: app ## Build and open the app
	dist/linux/procyon

bench: ## Measure the app against the spec's performance targets (needs a display; xvfb-run on CI)
	scripts/bench-linux.py

screenshots: app ## Render the main screens, light and dark, into dist/linux/screenshots
	@mkdir -p dist/linux/screenshots
	@for page in $(SCREENSHOT_PAGES); do for theme in light dark; do \
		dist/linux/procyon --screenshot dist/linux/screenshots/$$page-$$theme.png --$$theme --page $$page || exit 1; \
	done; done; echo dist/linux/screenshots

format: ## Format C/C++ sources in place (and regenerate tokens)
	scripts/format.sh

lint: ## Read-only checks (tokens, clang-format, clang-tidy, shellcheck). SKIP_TIDY=1 skips clang-tidy
	scripts/lint.sh

tokens: ## Regenerate design tokens and the process catalog
	python3 scripts/gen-tokens.py
	python3 scripts/gen-catalog.py

icon: ## (macOS and Windows) re-render the app icon; Linux installs the shared PNG
	@echo "Linux uses apps/macos/Resources/AppIcon.png as is (make icon on macOS re-renders it)"

tools: ## Print the packages the Linux build needs
	@echo "Debian/Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev clang-tidy shellcheck"
	@echo "Fedora:        sudo dnf install gcc-c++ cmake ninja-build pkgconf gtk4-devel clang-tools-extra ShellCheck"
	@echo "Arch:          sudo pacman -S base-devel cmake ninja pkgconf gtk4 clang shellcheck"
	@echo "clang-format:  19 or later (CI pins 19.1.0: pipx install clang-format==19.1.0); older ones format differently"

install: app ## Install into PREFIX (default ~/.local): binary, desktop entry, icon, licenses
	cmake --install build/linux --prefix "$(PREFIX)"

package: app ## dist/Procyon-<version>-linux-<arch>.tar.gz and its SHA-256
	scripts/package-linux.sh

clean: ## Remove build outputs
	rm -rf build/linux build/linux-debug build/core dist

ci: lint test app ## What CI runs

else ifneq ($(OS),Windows_NT)
# ---- macOS ----

help: ## List targets
	@awk -F':.*## ' '/^# ---- /{f = /macOS/; next} f && /^[a-z-]+:.*## / {printf "  %-12s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

build: ## Debug build of the Swift package (app + helper)
	swift build

test: ## Run unit tests
	swift test

app: ## Release dist/Procyon.app
	scripts/build-macos-app.sh release

core: ## Build the C++ core, CLI and helper with CMake, run the core tests
	cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release
	cmake --build build/core -j
	ctest --test-dir build/core --output-on-failure

run: app ## Build and open the app
	open dist/Procyon.app

bench: ## Measure the app against the spec's performance targets
	scripts/bench-macos.py

screenshots: ## Retake the README screenshots (light and dark) in docs/screenshots
	@test -d dist/Procyon.app || scripts/build-macos-app.sh release
	swift scripts/screenshots.swift --pages overview processes cpu energy history inspect startup

format: ## Format Swift and C/C++ sources in place
	scripts/format.sh

lint: ## All read-only checks (tokens, swift-format, clang-format, clang-tidy, shellcheck)
	scripts/lint.sh

tokens: ## Regenerate design tokens and the process catalog
	python3 scripts/gen-tokens.py
	python3 scripts/gen-catalog.py

icon: ## Re-render the app icon
	swift scripts/make-icon.swift apps/macos/Resources/AppIcon.icns

tools: ## (Windows) fetch build tools; nothing to do on macOS
	@echo "Nothing to fetch on macOS: Xcode provides the toolchain, brew install clang-format llvm shellcheck"

clean: ## Remove build outputs
	rm -rf .build build dist

ci: lint test core app ## What CI runs

endif
