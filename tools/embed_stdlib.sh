#!/bin/sh
# embed_stdlib.sh DIR > OUT: the standard library modules written in Python
# (src/stdlib/a.py, src/stdlib/pkg/__init__.py, src/stdlib/pkg/b.py) as C
# initializers {"name", is_package, "source"} for src/stdlib.c.
dir=$1
find "$dir" -name '*.py' | LC_ALL=C sort | while read -r f; do
    n=${f#"$dir"/}; n=${n%.py}; pkg=0
    case $n in */__init__) n=${n%/__init__}; pkg=1;; esac
    n=$(printf '%s' "$n" | tr / .)
    printf '{"%s",%d,\n' "$n" "$pkg"
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/?/\\?/g' -e 's/^/"/' -e 's/$/\\n"/' "$f"   # (\?: no trigraphs)
    printf '},\n'
done
