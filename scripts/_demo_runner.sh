#!/usr/bin/env bash
# Helper for asciinema-recorded demos: pretend to be a user typing at the
# board's g474> prompt by driving commands through tether and printing
# both the input line and the captured response with realistic pacing.
#
# Usage:
#   _demo_runner.sh                 # uses commands from $DEMO_CMDS file
#   _demo_runner.sh "cmd1" "cmd2"   # commands as arguments

set -e

SOCK="${TETHER_SOCK:-/tmp/tetherd.sock}"
PROMPT_GREEN=$'\033[1;32m'
RESET=$'\033[0m'

# Verify daemon is alive
if ! tether -s "$SOCK" status >/dev/null 2>&1; then
    echo "ERROR: tetherd not running at $SOCK" >&2
    echo "  start with: tetherd -D /dev/cu.usbmodem* -b 115200 &" >&2
    exit 1
fi

# Read commands from arguments or from the DEMO_CMDS file
if [[ $# -gt 0 ]]; then
    cmds=("$@")
else
    [[ -z "${DEMO_CMDS:-}" ]] && { echo "set DEMO_CMDS or pass commands as args"; exit 1; }
    mapfile -t cmds < "$DEMO_CMDS"
fi

sleep 0.6
for cmd in "${cmds[@]}"; do
    # Skip blank lines, treat #-prefixed lines as banner notes
    case "$cmd" in
        '' )       sleep 0.5 ;;
        '#'* )     printf '%s\n' "${cmd#\# }" ; sleep 1.0 ;;
        '!sleep '* ) sleep "${cmd#!sleep }" ;;
        * )
            # Type the prompt + command
            printf '%sg474>%s ' "$PROMPT_GREEN" "$RESET"
            for ((i=0; i<${#cmd}; i++)); do
                printf '%s' "${cmd:i:1}"
                sleep 0.025
            done
            printf '\n'
            sleep 0.25

            # Send + capture
            tether -s "$SOCK" run \
                --until 'g474> $' \
                --newline crlf \
                --timeout-ms 30000 \
                "$cmd" 2>&1 | sed '$d'   # drop trailing prompt line

            sleep 1.0
            ;;
    esac
done

printf '%sg474>%s ' "$PROMPT_GREEN" "$RESET"
sleep 0.8
printf '\n'
