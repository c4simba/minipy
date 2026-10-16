#!/bin/sh
# Source packages of plainpy for an Ubuntu PPA on Launchpad - one per Ubuntu
# series - and, with --upload, their upload. Launchpad builds the binary
# packages itself from them (debian/: amd64 and arm64; no network there, the
# build needs a C compiler and make and runs `make test`).
#
#   tools/launchpad.sh [--upload] [VERSION]
#
# VERSION   the upstream version (default: `make print-version`); a "-" in it
#           becomes "~" (1.2.0-rc1 -> 1.2.0~rc1, before 1.2.0)
# --upload  sign the packages (debsign) and upload them (dput)
#
# Environment:
#   PPA       where to (default ppa:c4simba/plainpy)
#   SERIES    the Ubuntu series (default "jammy noble resolute")
#   PPA_REV   n in 1.2.3-1~noble<n> (default 1): raise it to upload the same
#             version again (Launchpad refuses a version twice)
#   DEBFULLNAME, DEBEMAIL   the uploader in debian/changelog (default: the
#             signing key's user id, else git config user.name / user.email)
#   GPG_KEY   the key to sign with (default: gpg's first secret key)
#   OUT       where the packages go (default build/launchpad)
#
# The sources are `git archive HEAD` (commit first): the .orig.tar.gz is the
# same for every series, as Launchpad wants it; each series' debian/changelog
# is the one entry of that upload (the repository's is for local builds). Needs git, dpkg-dev; for
# --upload devscripts (debsign) and dput, and the key registered with the
# Launchpad account that owns the PPA (.github/workflows/build.yml runs it for
# a tag v1.2.3 with the secret LAUNCHPAD_GPG_KEY).
set -eu

UPLOAD=0
if [ "${1:-}" = "--upload" ]; then UPLOAD=1; shift; fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

VERSION=${1:-$(make -s print-version)}
V=$(printf '%s' "$VERSION" | tr '-' '~')
PPA=${PPA:-ppa:c4simba/plainpy}
SERIES=${SERIES:-jammy noble resolute}
PPA_REV=${PPA_REV:-1}
OUT=${OUT:-build/launchpad}

if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "launchpad.sh: uncommitted changes are not in the packages (they are made from HEAD)" >&2
fi

KEY=${GPG_KEY:-}
if [ -z "$KEY" ] && command -v gpg >/dev/null 2>&1; then
    KEY=$(gpg --batch --list-secret-keys --with-colons 2>/dev/null | awk -F: '/^fpr/ { print $10; exit }')
fi
UID_=""
if [ -n "$KEY" ]; then
    UID_=$(gpg --batch --list-secret-keys --with-colons "$KEY" 2>/dev/null | awk -F: '/^uid/ { print $10; exit }')
fi
NAME=${DEBFULLNAME:-$(printf '%s' "$UID_" | sed -n 's/^\(.*[^ ]\) *<.*>.*$/\1/p')}
NAME=${NAME:-$(git config user.name || true)}
EMAIL=${DEBEMAIL:-$(printf '%s' "$UID_" | sed -n 's/^.*<\(.*\)>.*$/\1/p')}
EMAIL=${EMAIL:-$(git config user.email || true)}
if [ -z "$NAME" ] || [ -z "$EMAIL" ]; then
    echo "launchpad.sh: who uploads? set DEBFULLNAME and DEBEMAIL" >&2; exit 1
fi
if [ "$UPLOAD" = 1 ] && [ -z "$KEY" ]; then
    echo "launchpad.sh: --upload needs a gpg secret key (GPG_KEY)" >&2; exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"
ORIG="plainpy_$V.orig.tar.gz"
git archive --format=tar --prefix="plainpy-$V/" HEAD -- . ':(exclude)debian' | gzip -n -9 > "$OUT/$ORIG"

DATE=$(LC_ALL=C date '+%a, %d %b %Y %H:%M:%S %z')
for s in $SERIES; do
    d="$OUT/$s"
    mkdir -p "$d"
    cp "$OUT/$ORIG" "$d/"
    tar -xzf "$d/$ORIG" -C "$d"
    git archive --format=tar HEAD debian | tar -xf - -C "$d/plainpy-$V"
    {
        printf 'plainpy (%s-1~%s%s) %s; urgency=medium\n\n' "$V" "$s" "$PPA_REV" "$s"
        printf '  * plainpy %s for Ubuntu %s:\n' "$VERSION" "$s"
        printf '    https://github.com/c4simba/plainpy/releases/tag/v%s\n\n' "$VERSION"
        printf ' -- %s <%s>  %s\n' "$NAME" "$EMAIL" "$DATE"
    } > "$d/plainpy-$V/debian/changelog"
    # -S source only, -nc: the tree is clean already (no debhelper needed
    # here), -d: no build dependencies checked, -sa: with the .orig.tar.gz
    (cd "$d/plainpy-$V" && dpkg-buildpackage -S -nc -d -sa -us -uc >/dev/null)
    echo "built $d/plainpy_$V-1~$s${PPA_REV}_source.changes"
done

if [ "$UPLOAD" = 1 ]; then
    for s in $SERIES; do
        c="$OUT/$s/plainpy_$V-1~$s${PPA_REV}_source.changes"
        debsign -k"$KEY" --no-re-sign "$c"
        dput "$PPA" "$c"
    done
    owner=${PPA#ppa:}; owner=${owner%%/*}
    echo "uploaded to $PPA: Launchpad mails $EMAIL when each is accepted and built"
    echo "  https://launchpad.net/~$owner/+archive/ubuntu/${PPA##*/}"
fi
