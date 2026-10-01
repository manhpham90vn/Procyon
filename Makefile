# Common entry points. `make help` lists them.
.PHONY: help build test app core run format lint tokens icon clean ci

help: ## List targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-8s %s\n", $$1, $$2}'

build: ## Debug build of the Swift package (app + helper)
	swift build

test: ## Run unit tests
	swift test

app: ## Release dist/Procyon.app
	scripts/build-macos-app.sh release

core: ## Build the C++ core, CLI and helper with CMake
	cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release
	cmake --build build/core -j

run: app ## Build and open the app
	open dist/Procyon.app

format: ## Format Swift and C/C++ sources in place
	scripts/format.sh

lint: ## All read-only checks (tokens, swift-format, clang-format, clang-tidy, shellcheck)
	scripts/lint.sh

tokens: ## Regenerate design tokens
	python3 scripts/gen-tokens.py

icon: ## Re-render the app icon
	swift scripts/make-icon.swift apps/macos/Resources/AppIcon.icns

clean: ## Remove build outputs
	rm -rf .build build dist

ci: lint test core app ## What CI runs
