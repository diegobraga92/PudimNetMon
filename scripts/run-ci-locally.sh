#!/usr/bin/env bash
# Mirrors the GitHub Actions workflow (.github/workflows/ci.yml) locally so you
# can run the same checks on your machine before pushing.
#
# Jobs (in the same order as CI):
#   1. C++ Agent (build)          cmake configure + build + ctest  (Debug)
#   2. C++ Collector (build)      cmake configure + build + ctest  (Debug)
#   3. C++ Agent (Windows build)  MSVC + vcpkg + ctest + Inno Setup installer
#                                 (only on a Windows host under Git Bash/MSYS)
#   4. Dashboard (lint + build)   npm ci + lint + test + build
#
# Usage: scripts/run-ci-locally.sh [options]
#
# Options:
#   -h, --help           Show this help and exit.
#   -j, --jobs N         Parallel build jobs (default: number of CPUs).
#   -k, --keep-going     Run every job even if an earlier one fails.
#       --clean          Delete the CI build dirs (build/agent, build/collector,
#                        build/agent-win) before starting.
#       --skip-agent     Skip the C++ Agent (build) job.
#       --skip-collector Skip the C++ Collector (build) job.
#       --skip-windows   Skip the C++ Agent (Windows build) job.
#       --skip-dashboard Skip the Dashboard (lint + build) job.
#       --no-deps        Do not run the apt-get dependency install step.
#
# Exit status is 0 when every enabled job passes, 1 otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# vcpkg pins copied verbatim from ci.yml.
VCPKG_PIN="e90cc0982b7cfae62447f1f3bed1fbca0bc8f6be"
VCPKG_TOOL_RELEASE="2026-07-27"

DO_AGENT=1
DO_COLLECTOR=1
DO_WINDOWS=1
DO_DASHBOARD=1
INSTALL_DEPS=1
KEEP_GOING=0
CLEAN=0

usage() {
    sed -n '2,32p' "$0" | sed -n 's/^# \{0,1\}//p'
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) usage; exit 0 ;;
            -j|--jobs) JOBS="$2"; shift 2 ;;
            -k|--keep-going) KEEP_GOING=1; shift ;;
            --clean) CLEAN=1; shift ;;
            --skip-agent) DO_AGENT=0; shift ;;
            --skip-collector) DO_COLLECTOR=0; shift ;;
            --skip-windows) DO_WINDOWS=0; shift ;;
            --skip-dashboard) DO_DASHBOARD=0; shift ;;
            --no-deps) INSTALL_DEPS=0; shift ;;
            *) printf 'Unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
        esac
    done
}

# ---- output helpers ---------------------------------------------------------
if [ -t 1 ]; then
    C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'
    C_BOLD=$'\033[1m'; C_RESET=$'\033[0m'
else
    C_GREEN=""; C_RED=""; C_YELLOW=""; C_BOLD=""; C_RESET=""
fi

log()     { printf '%s\n' "$*"; }
ok()      { printf '%s%s%s\n' "${C_GREEN}ok${C_RESET}"  "  $*"; }
warn()    { printf '%s%s%s\n' "${C_YELLOW}warn${C_RESET}" "  $*"; }
fail()    { printf '%s%s%s\n' "${C_RED}FAIL${C_RESET}" "  $*"; }
section() { printf '\n%s━━━━ %s ━━━━%s\n' "${C_BOLD}" "$*" "${C_RESET}"; }

run() {
    printf '$ %s\n' "$*"
    "$@"
}

# ---- host detection --------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    *) IS_WINDOWS=0 ;;
esac

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

# ---- job: install dependencies (apt) --------------------------------------
# Same package set as the two Linux jobs in ci.yml.
install_deps() {
    section "Install dependencies (apt)"
    if ! command -v apt-get >/dev/null 2>&1; then
        warn "apt-get not found - install the CI packages manually and rerun with --no-deps."
        return 0
    fi
    local pkgs="build-essential cmake pkg-config \
        protobuf-compiler libprotobuf-dev protobuf-compiler-grpc \
        libgrpc++-dev libgrpc-dev libssl-dev libcurl4-openssl-dev \
        libpq-dev librdkafka-dev libpcap-dev libsystemd-dev libsqlite3-dev"
    run $SUDO apt-get update
    # shellcheck disable=SC2086
    run $SUDO apt-get install -y $pkgs
}

# ---- job: C++ Agent (build) ------------------------------------------------
job_cpp_agent() {
    section "C++ Agent (build) - Configure"
    run cmake -S "$ROOT/agent" -B "$ROOT/build/agent" -DCMAKE_BUILD_TYPE=Debug

    section "C++ Agent (build) - Build"
    run cmake --build "$ROOT/build/agent" -j "$JOBS"

    section "C++ Agent (build) - Test (CTest)"
    ( cd "$ROOT/build/agent" && ctest --output-on-failure )
}

# ---- job: C++ Collector (build) --------------------------------------------
job_cpp_collector() {
    section "C++ Collector (build) - Configure"
    run cmake -S "$ROOT/collector" -B "$ROOT/build/collector" -DCMAKE_BUILD_TYPE=Debug

    section "C++ Collector (build) - Build"
    run cmake --build "$ROOT/build/collector" -j "$JOBS"

    section "C++ Collector (build) - Test (CTest)"
    ( cd "$ROOT/build/collector" && ctest --output-on-failure )
}

# ---- job: C++ Agent (Windows build) ----------------------------------------
# Only meaningful on Windows. On any other host it reports as skipped.
job_cpp_agent_windows() {
    if [ "$IS_WINDOWS" -ne 1 ]; then
        warn "Skipped: needs a Windows host (MSVC + vcpkg + Inno Setup)."
        return 0
    fi

    section "C++ Agent (Windows build) - Prepare vcpkg (pinned baseline)"
    VCPKG_ROOT="${VCPKG_ROOT:-$ROOT/.vcpkg}"
    export VCPKG_ROOT
    export VCPKG_DEFAULT_TRIPLET="x64-windows-static-md-release"
    if [ ! -f "$VCPKG_ROOT/vcpkg.exe" ]; then
        git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
        git -C "$VCPKG_ROOT" checkout "$VCPKG_PIN"
        curl.exe -L --fail --retry 5 --retry-delay 5 --retry-connrefused \
            -o "$VCPKG_ROOT/vcpkg.exe" \
            "https://github.com/microsoft/vcpkg-tool/releases/download/$VCPKG_TOOL_RELEASE/vcpkg.exe"
    fi
    run "$VCPKG_ROOT/vcpkg.exe" version --disable-metrics

    section "C++ Agent (Windows build) - Configure (MSVC)"
    run cmake -S "$ROOT/agent" -B "$ROOT/build/agent-win" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release

    section "C++ Agent (Windows build) - Build (MSVC)"
    # Bounded parallelism, same as CI (unbounded MSBuild /m can OOM while
    # linking gRPC and obscures linker errors).
    run cmake --build "$ROOT/build/agent-win" --config Release -j 2

    section "C++ Agent (Windows build) - Test (CTest)"
    ( cd "$ROOT/build/agent-win" && ctest -C Release --output-on-failure )

    section "C++ Agent (Windows build) - Installer (Inno Setup)"
    local iscc=""
    if command -v ISCC.exe >/dev/null 2>&1; then
        iscc="$(command -v ISCC.exe)"
    else
        for p in \
            "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
            "/c/Program Files/Inno Setup 6/ISCC.exe"; do
            if [ -f "$p" ]; then iscc="$p"; break; fi
        done
    fi
    if [ -z "$iscc" ]; then
        warn "ISCC.exe not found - installer step skipped (install Inno Setup: choco install innosetup)."
    else
        local version
        version="$(sed -n 's/.*project(pudim-agent VERSION \([0-9][0-9.]*\)).*/\1/p' \
            "$ROOT/agent/CMakeLists.txt")"
        version="${version:-0.1.0}"
        ( cd "$ROOT/installer" && run "$iscc" "/DMyAppVersion=$version" installer-agent.iss )
        log "  Installer output: $ROOT/dist/installer/PudimNetMon-Agent-Setup-$version.exe"
    fi
    # The CI smoke test (silent install -> service -> uninstall) registers a
    # Windows service and needs an elevated shell; left to CI on purpose.
    warn "Smoke test skipped: service registration requires an elevated shell."
}



# ---- job: Dashboard (lint + build) -----------------------------------------
job_dashboard() {
    if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
        warn "Skipped: needs Node.js + npm (CI uses Node 26 via actions/setup-node)."
        return 0
    fi

    section "Dashboard - Install dependencies (npm ci)"
    ( cd "$ROOT/dashboard" && run npm ci )

    section "Dashboard - Lint"
    ( cd "$ROOT/dashboard" && run npm run lint )

    section "Dashboard - Test"
    ( cd "$ROOT/dashboard" && run npm test )

    section "Dashboard - Build"
    ( cd "$ROOT/dashboard" && run npm run build )
}

# ---- job runner -------------------------------------------------------------
RESULTS=()

run_job() {
    local name="$1"; shift
    section "Job: $name"
    if ( set -e; "$@" ); then
        ok "$name"
        RESULTS+=("PASS|$name")
    else
        fail "$name"
        RESULTS+=("FAIL|$name")
        if [ "$KEEP_GOING" -eq 0 ]; then
            printf '\n%sAborting on first failure (use -k/--keep-going to run the rest).%s\n' \
                "${C_YELLOW}" "${C_RESET}"
            print_summary
            exit 1
        fi
    fi
}

print_summary() {
    section "Summary"
    local failed=0
    for r in "${RESULTS[@]}"; do
        if [[ "$r" == "PASS|"* ]]; then
            printf '  %s  %s\n' "${C_GREEN}PASS${C_RESET}" "${r#PASS|}"
        else
            printf '  %s  %s\n' "${C_RED}FAIL${C_RESET}" "${r#FAIL|}"
            failed=1
        fi
    done
    [ "$failed" -eq 0 ]
}

main() {
    parse_args "$@"

    log "${C_BOLD}PudimNetMon local CI mirror${C_RESET} (workflow: .github/workflows/ci.yml)"
    log "  repo:    $ROOT"
    log "  host:    $(uname -s) $(uname -m)"
    log "  jobs:    $JOBS"
    log "  cmake:   $(cmake --version | head -1 | awk '{print $3}')"
    log "  git:     $(git --version | awk '{print $3}')"

    if [ "$CLEAN" -eq 1 ]; then
        section "Clean CI build dirs"
        rm -rf "$ROOT/build/agent" "$ROOT/build/collector" "$ROOT/build/agent-win"
        ok "removed build/agent, build/collector, build/agent-win"
    fi

    if [ "$INSTALL_DEPS" -eq 1 ]; then
        run_job "Install dependencies (apt)" install_deps
    fi
    [ "$DO_AGENT" -eq 1 ]     && run_job "C++ Agent (build)"           job_cpp_agent
    [ "$DO_COLLECTOR" -eq 1 ] && run_job "C++ Collector (build)"       job_cpp_collector
    [ "$DO_WINDOWS" -eq 1 ]   && run_job "C++ Agent (Windows build)"   job_cpp_agent_windows
    [ "$DO_DASHBOARD" -eq 1 ] && run_job "Dashboard (lint + build)"    job_dashboard

    print_summary
}

main "$@"
