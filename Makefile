# Common entry points. `make help` lists them.
.PHONY: help build test app core run bench format lint tokens icon clean ci

help: ## List targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-8s %s\n", $$1, $$2}'

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

format: ## Format Swift and C/C++ sources in place
	scripts/format.sh

lint: ## All read-only checks (tokens, swift-format, clang-format, clang-tidy, shellcheck)
	scripts/lint.sh

tokens: ## Regenerate design tokens and the process catalog
	python3 scripts/gen-tokens.py
	python3 scripts/gen-catalog.py

icon: ## Re-render the app icon
	swift scripts/make-icon.swift apps/macos/Resources/AppIcon.icns

clean: ## Remove build outputs
	rm -rf .build build dist

ci: lint test core app ## What CI runs
