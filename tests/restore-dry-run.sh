#!/bin/sh
# Restore checks, run as dry runs: they need neither X nor i3, and a dry run
# that tried to reach i3 fails here because DISPLAY and I3SOCK are unset.
# Usage: restore-dry-run.sh path/to/i3-snapshot
set -u

snapshot=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
unset DISPLAY I3SOCK WAYLAND_DISPLAY SWAYSOCK
failures=0

b64() {
	printf '%s' "$1" | base64 | tr -d '\n'
}

# check NAME STATUS ARGS... : runs i3-snapshot ARGS with $tmp/in on stdin and
# compares its exit status, stdout and stderr with STATUS, $tmp/out.expected and
# $tmp/err.expected.
check() {
	name=$1 status=$2
	shift 2
	"$snapshot" "$@" <"$tmp/in" >"$tmp/out" 2>"$tmp/err"
	actual=$?
	if [ "$actual" -ne "$status" ] ||
		! cmp -s "$tmp/out" "$tmp/out.expected" ||
		! cmp -s "$tmp/err" "$tmp/err.expected"; then
		echo "FAIL: $name (exit $actual, expected $status)"
		echo "--- stdout"; cat "$tmp/out"
		echo "--- expected stdout"; cat "$tmp/out.expected"
		echo "--- stderr"; cat "$tmp/err"
		echo "--- expected stderr"; cat "$tmp/err.expected"
		failures=$((failures + 1))
	else
		echo "ok: $name"
	fi
}

: >"$tmp/err.expected"

# Values with quotes, semicolons and backslashes stay one argument each.
printf '%s %s 11 12 %s\n' "$(b64 'HDMI-1"; exec touch /tmp/x; "')" "$(b64 '2: "web" \ ;')" \
	"$(b64 'a\Eb') " >"$tmp/in"
cat >"$tmp/out.expected" <<'EOF'
i3-msg [con_id=11] move workspace to output "HDMI-1\"; exec touch /tmp/x; \""
i3-msg [con_id=12] move container to workspace "2: \"web\" \\ ;"
EOF
check "values are quoted" 0 -y
cat >"$tmp/out.expected" <<'EOF'
i3-msg [workspace="\\A\\Q2: \"web\" \\ ;\\E\\z"] move workspace to output "HDMI-1\"; exec touch /tmp/x; \""
i3-msg [title="\\A\\Qa\\E\\\\E\\Qb\\E\\z"] move container to workspace "2: \"web\" \\ ;"
EOF
check "titles and workspaces match literally" 0 -y -t

# Each line once, in order. Blank lines, a CRLF line, spaces and tabs around
# the fields, an untitled window with and without a trailing separator, and a
# final line without a newline.
o=$(b64 eDP-1) w1=$(b64 1) w2=$(b64 2) t=$(b64 term)
printf '\n%s %s 1 2 %s\r\n \t\n\t%s\t%s  3 4 \n%s %s 5 6' "$o" "$w1" "$t" "$o" "$w2" "$o" "$w1" >"$tmp/in"
cat >"$tmp/out.expected" <<'EOF'
i3-msg [con_id=1] move workspace to output "eDP-1"
i3-msg [con_id=2] move container to workspace "1"
i3-msg [con_id=3] move workspace to output "eDP-1"
i3-msg [con_id=4] move container to workspace "2"
i3-msg [con_id=5] move workspace to output "eDP-1"
i3-msg [con_id=6] move container to workspace "1"
EOF
check "every line once" 0 -y

# Nothing to restore.
: >"$tmp/in"
: >"$tmp/out.expected"
check "empty snapshot" 0 -y
printf '\n \t\n\r\n' >"$tmp/in"
check "blank snapshot" 0 -y

# Every invalid line is reported, by line number, and nothing is printed.
{
	printf '%s %s 1 2 %s\n' "$o" "$w1" "$t"
	printf '%s %s 12abc 2 %s\n' "$o" "$w1" "$t"
	printf '\n'
	printf '%s %s 1 -1 %s\n' "$o" "$w1" "$t"
	printf '%s %s 0 2 %s\n' "$o" "$w1" "$t"
	printf '%s %s 18446744073709551616 2 %s\n' "$o" "$w1" "$t"
	printf '%s %s 18446744073709551615 2 %s\n' "$o" "$w1" "$t"
	printf '%s %s 1 2\n' "$o" "$w1"
	printf '%s 1 2\n' "$o"
	printf '%s %s 1 2 %s x\n' "$o" "$w1" "$t"
	printf 'eDP-1 %s 1 2 %s\n' "$w1" "$t"
	printf '%s MQ= 1 2 %s\n' "$o" "$t"
	printf '%s YQA= 1 2 %s\n' "$o" "$t"
	printf '%s %s 1 2 %s\r\r\n' "$o" "$w1" "$t"
} >"$tmp/in"
: >"$tmp/out.expected"
cat >"$tmp/err.expected" <<'EOF'
line 2: workspace id is not a nonzero decimal number of at most 64 bits
line 4: window id is not a nonzero decimal number of at most 64 bits
line 5: workspace id is not a nonzero decimal number of at most 64 bits
line 6: workspace id is not a nonzero decimal number of at most 64 bits
line 9: expected 4 or 5 fields, found 3
line 10: expected 4 or 5 fields, found 6
line 11: output name is not valid base64
line 12: workspace name is not valid base64
line 13: workspace name contains a NUL character
line 14: more than one carriage return at the end of the line
EOF
check "invalid lines" 1 -y
check "invalid lines with -c" 1 -y -c
: >"$tmp/err.expected"

# Raw snapshots: four fields each followed by one space, then the title verbatim.
printf 'HDMI-1 2:web 7 8  a  title with spaces \nHDMI-1 3 9 10\n' >"$tmp/in"
cat >"$tmp/out.expected" <<'EOF'
i3-msg [con_id=7] move workspace to output "HDMI-1"
i3-msg [con_id=8] move container to workspace "2:web"
i3-msg [con_id=9] move workspace to output "HDMI-1"
i3-msg [con_id=10] move container to workspace "3"
EOF
check "raw snapshot" 0 -y -r
head -n 1 "$tmp/in" >"$tmp/in1" && mv "$tmp/in1" "$tmp/in"
cat >"$tmp/out.expected" <<'EOF'
i3-msg [workspace="\\A\\Q2:web\\E\\z"] move workspace to output "HDMI-1"
i3-msg [title="\\A\\Q a  title with spaces \\E\\z"] move container to workspace "2:web"
EOF
check "raw snapshot by title" 0 -y -r -t
printf 'HDMI-1  2 7 8 t\nHDMI-1 2 7\n' >"$tmp/in"
: >"$tmp/out.expected"
cat >"$tmp/err.expected" <<'EOF'
line 1: expected output, workspace, workspace id and window id, each followed by one space
line 2: expected output, workspace, workspace id and window id, each followed by one space
EOF
check "invalid raw lines" 1 -y -r
: >"$tmp/err.expected"

# A window without a title cannot be found by title.
printf '%s %s 1 2\n%s %s 3 4 %s\n' "$o" "$w1" "$o" "$w2" "$t" >"$tmp/in"
: >"$tmp/out.expected"
echo 'Failed to move 2 ().' >"$tmp/err.expected"
check "untitled window by title" 1 -y -t
cat >"$tmp/out.expected" <<'EOF'
i3-msg [workspace="\\A\\Q2\\E\\z"] move workspace to output "eDP-1"
i3-msg [title="\\A\\Qterm\\E\\z"] move container to workspace "2"
EOF
check "untitled window by title with -c" 0 -y -t -c

if [ "$failures" -ne 0 ]; then
	echo "$failures check(s) failed"
	exit 1
fi
