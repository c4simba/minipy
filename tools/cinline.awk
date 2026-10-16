# Prints a C file with each line `#include "aot/....c"` or `#include
# "targets/....c"` replaced by that file (from src/, recursively): the C
# texts plainpy.lib carries are whole files (Makefile, C_TEXT_SRC).
#
#   awk -f tools/cinline.awk src/targets/macos/x2c_host.c
function emit(path,    line, parts, inc) {
    if ((getline line < path) <= 0) { print "cinline: cannot read " path > "/dev/stderr"; exit 1 }
    do {
        if (line ~ /^#include "(aot|targets)\/[^"]*\.c"/) {
            split(line, parts, "\""); inc = "src/" parts[2]
            emit(inc)
        } else print line
    } while ((getline line < path) > 0)
    close(path)
}
BEGIN { emit(ARGV[1]); exit 0 }
