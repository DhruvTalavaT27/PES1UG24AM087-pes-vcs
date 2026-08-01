#!/usr/bin/env bash
#
# test_sequence.sh — End-to-end integration test.
#
# Runs the full lifecycle — init, add, status, commit, rm, log — in a
# throwaway directory and asserts on the observable behavior of each
# command, including object-store deduplication.
#
# Run from the repository root after building:
#   make test-integration

set -euo pipefail

BIN="$(cd -- "$(dirname -- "$0")" && pwd)/strata"
WORK="$(mktemp -d)"

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

cd "$WORK"

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1" >&2; exit 1; }

expect_contains() {
    local haystack="$1" needle="$2" label="$3"
    case "$haystack" in
        *"$needle"*) pass "$label" ;;
        *) fail "$label (missing: $needle)" ;;
    esac
}

echo "=== strata end-to-end tests ==="
echo ""

# ── init ─────────────────────────────────────────────────────────────────────
"$BIN" init
[ -d .strata/objects ]    && pass ".strata/objects exists"    || fail ".strata/objects missing"
[ -d .strata/refs/heads ] && pass ".strata/refs/heads exists" || fail ".strata/refs/heads missing"
[ -f .strata/HEAD ]       && pass ".strata/HEAD exists"       || fail ".strata/HEAD missing"
echo ""

# ── staging and status ───────────────────────────────────────────────────────
echo "version 1" > file.txt
echo "hello world" > hello.txt
"$BIN" add file.txt hello.txt

out="$("$BIN" status)"
expect_contains "$out" "On branch main"            "status names the current branch"
expect_contains "$out" "new file:   file.txt"      "status lists staged file.txt"
expect_contains "$out" "new file:   hello.txt"     "status lists staged hello.txt"
echo ""

# ── first commit ─────────────────────────────────────────────────────────────
"$BIN" commit -m "Initial commit"
out="$("$BIN" status)"
expect_contains "$out" "working tree clean"        "status clean after commit"
expect_contains "$("$BIN" log)" "Initial commit"   "log shows the first commit"
echo ""

# ── modify without staging ───────────────────────────────────────────────────
echo "version 2" >> file.txt
out="$("$BIN" status)"
expect_contains "$out" "modified:   file.txt"      "status flags unstaged modification"
"$BIN" add file.txt
out="$("$BIN" status)"
expect_contains "$out" "Changes to be committed"   "staged modification listed for commit"
"$BIN" commit -m "Update file.txt"
expect_contains "$("$BIN" log)" "Update file.txt"  "log shows the second commit"
echo ""

# ── unstage with rm ──────────────────────────────────────────────────────────
echo "goodbye" > bye.txt
"$BIN" add bye.txt
"$BIN" rm bye.txt
out="$("$BIN" status)"
expect_contains "$out" "Untracked files"           "rm returns a file to untracked"
"$BIN" add bye.txt
"$BIN" commit -m "Add farewell"
echo ""

# ── history and refs ─────────────────────────────────────────────────────────
out="$("$BIN" log)"
expect_contains "$out" "Add farewell"              "log shows the third commit"
expect_contains "$out" "HEAD -> main"              "log marks the current branch"

grep -q "ref: refs/heads/main" .strata/HEAD && pass "HEAD points at refs/heads/main" || fail "HEAD not a symbolic ref"
[ -s .strata/refs/heads/main ] && pass "branch ref holds a commit hash" || fail "branch ref empty"
echo ""

# ── deduplication ────────────────────────────────────────────────────────────
# 3 commits over 4 distinct file versions must yield exactly:
#   4 blobs + 3 trees + 3 commits = 10 objects
objects=$(find .strata/objects -type f | wc -l)
[ "$objects" -eq 10 ] && pass "object store deduplicates (10 objects for 3 commits)" \
                        || fail "expected 10 objects, found $objects"

echo ""
echo "=== all end-to-end tests passed ==="
