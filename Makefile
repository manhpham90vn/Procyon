# Common entry points, the same names on macOS and Windows. `make help` lists them.
#
# macOS needs Xcode 27 (see docs/development.md). Windows needs Visual Studio 2022 with the C++
# workload and GNU Make (`winget install ezwinports.make`); `make tools` fetches CMake, Ninja and
# clang-format into build\tools when Visual Studio's CMake component is not installed.
.PHONY: help build test app core run bench screenshots format lint tokens icon tools clean ci

ifeq ($(OS),Windows_NT)
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

screenshots: ## Retake the README screenshots (macOS only for now)
	@echo screenshots is macOS only for now (scripts/screenshots.swift)

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

help: ## List targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-12s %s\n", $$1, $$2}'

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
