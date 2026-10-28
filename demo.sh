#!/usr/bin/env bash
# Runs the whole Vigil pipeline with one command:
#   display gateway + web app  ->  vigil-tracker  ->  vigil-sim (in the foreground)
# Ctrl-C stops everything.
#
#   ./demo.sh                          # 8 targets, light clutter
#   ./demo.sh --targets 20 --clutter 6 # any other options go to vigil-sim
#   ./demo.sh --no-open                # don't open a browser
#   ./demo.sh --insecure               # turn authentication off (see tools/attacker.py)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build/release"
LOGS="$ROOT/build/demo"
URL="http://localhost:5173"

open_browser=1
insecure=0
sim_args=()
for arg in "$@"; do
  case "$arg" in
    --no-open) open_browser=0 ;;
    --insecure) insecure=1 ;;
    -h | --help)
      sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) sim_args+=("$arg") ;;
  esac
done
if [[ ${#sim_args[@]} -eq 0 ]]; then
  sim_args=(--targets 8 --clutter 3)
fi

say() { printf '\033[36mdemo:\033[0m %s\n' "$*"; }
die() { printf '\033[31mdemo:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------ preflight

for port in 5173 8081; do
  if ss -ltn "sport = :$port" | grep -q LISTEN; then
    die "TCP port $port is already in use. Is another demo (or npm run dev) still running?"
  fi
done
for port in 50000 50001; do
  if ss -lun "sport = :$port" | grep -q ":$port"; then
    die "UDP port $port is already in use. Is another vigil-tracker or gateway still running?"
  fi
done

if [[ ! -x "$BUILD/tracker/vigil-tracker" || ! -x "$BUILD/sim/vigil-sim" ]]; then
  say "building release binaries (first run only)..."
  cmake --preset release >/dev/null
  cmake --build --preset release
fi

if [[ ! -d "$ROOT/display/node_modules" ]]; then
  say "installing display dependencies (first run only)..."
  (cd "$ROOT/display" && npm install --no-fund --no-audit)
fi

mkdir -p "$LOGS"

# Every link is authenticated with one shared key, generated on first run and readable only
# by you. tools/attacker.py uses the same file to know which key it does NOT have.
KEY="$LOGS/vigil.key"
if ((insecure)); then
  auth=(--insecure)
  export VIGIL_INSECURE=1
  say "WARNING: authentication is OFF (--insecure)"
else
  if [[ ! -s "$KEY" ]]; then
    say "generating a key for this machine: ${KEY#"$ROOT/"}"
    (umask 077 && openssl rand -hex 32 >"$KEY")
  fi
  auth=(--key-file "$KEY")
  export VIGIL_KEY_FILE="$KEY"
fi

# ------------------------------------------------------------ start + cleanup

pids=()
sim_pid=""

cleanup() {
  trap - INT TERM EXIT
  echo
  say "stopping..."
  if [[ -n "$sim_pid" ]]; then
    kill -TERM "$sim_pid" 2>/dev/null || true
    wait "$sim_pid" 2>/dev/null || true
  fi
  # Each background service runs in its own process group (setsid), so signalling
  # the group also stops children such as vite and the gateway that npm spawns.
  for pid in "${pids[@]}"; do
    kill -INT -- "-$pid" 2>/dev/null || true
  done
  for _ in {1..30}; do
    alive=0
    for pid in "${pids[@]}"; do kill -0 "$pid" 2>/dev/null && alive=1; done
    ((alive)) || break
    sleep 0.1
  done
  for pid in "${pids[@]}"; do kill -KILL -- "-$pid" 2>/dev/null || true; done
  if [[ -s "$LOGS/tracker.log" ]]; then
    grep -E "reports processed|^rejected:" "$LOGS/tracker.log" | sed 's/^/  /' || true
  fi
  say "logs are in ${LOGS#"$ROOT/"}/"
}
trap cleanup INT TERM EXIT

start() {
  local name=$1
  shift
  # stdin from /dev/null: services must not touch the terminal. Vite puts its stdin into
  # raw mode for its keyboard shortcuts, which turns Ctrl-C into a plain keypress and
  # stops it from reaching this script.
  setsid "$@" </dev/null >"$LOGS/$name.log" 2>&1 &
  pids+=($!)
}

wait_for_port() {
  local port=$1 name=$2
  for _ in {1..100}; do
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && return 0
    kill -0 "${pids[-1]}" 2>/dev/null || die "$name exited early; see ${LOGS#"$ROOT/"}/$3"
    sleep 0.2
  done
  die "$name did not start within 20 s; see ${LOGS#"$ROOT/"}/$3"
}

say "starting display (gateway + web app)..."
start display bash -c "cd '$ROOT/display' && exec npm run dev"
wait_for_port 8081 "display gateway" display.log
wait_for_port 5173 "web app" display.log

say "starting tracker..."
start tracker "$BUILD/tracker/vigil-tracker" --quiet "${auth[@]}"
sleep 0.3
kill -0 "${pids[-1]}" 2>/dev/null || die "tracker exited early; see ${LOGS#"$ROOT/"}/tracker.log"

say "scope is live at $URL"
if ((open_browser)); then
  if grep -qi microsoft /proc/version 2>/dev/null && command -v explorer.exe >/dev/null; then
    explorer.exe "$URL" >/dev/null 2>&1 || true
  elif command -v xdg-open >/dev/null; then
    xdg-open "$URL" >/dev/null 2>&1 || true
  fi
fi

say "starting simulator: vigil-sim ${sim_args[*]}   (Ctrl-C to stop everything)"
echo
# Run in the background and `wait`, because bash defers traps until a foreground
# command finishes; `wait` returns as soon as Ctrl-C arrives, so cleanup runs at once.
"$BUILD/sim/vigil-sim" "${auth[@]}" "${sim_args[@]}" &
sim_pid=$!
wait "$sim_pid" || true
sim_pid=""
