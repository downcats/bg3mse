#!/bin/sh
# Everything a GitHub release of bg3mse needs, in one folder, from what the
# release target made (Mac/Release/Assemble.cmake), and the commands that
# publish it. It publishes nothing itself (the Mac form of publish.ps1).
#
#   sh Mac/Release/publish.sh --build <build directory> --key <update key file>
#                             (--previous <Manifest.json> | --first)
#   sh Mac/Release/publish.sh --build <build directory>      (a release that does not update itself)
#                             [--min-game <version>] [--max-game <version>] [--notice <text>]
#                             [--repo <owner/name>] [--out <folder>] [--not-notarised]
#
# --key        the private half of the update key (bg3mse-sign keygen), kept outside every repository
# --previous   the channel's manifest as it is published now:
#                  gh release download channel-<channel> --repo <repo> --pattern Manifest.json
#              the new one is the next of it; --first when the channel has none yet
# --min-game, --max-game
#              the game versions this release is for (CFBundleVersion, 4.1.1.7631656);
#              without them, every version: the extender checks the game itself when it starts
# --notice     shown to the player when this release is loaded
# --not-notarised
#              lay out a release whose bg3mse.app Apple has not notarised (an
#              ad hoc build, or one without BG3MSE_NOTARY_PROFILE); without
#              it such a release is refused. Its notes tell the player how
#              to open it.
#
# A release built with BG3MSE_SELF_UPDATE=OFF (its manifest.json says
# "self_update": false) has no channel: no key, no manifest, and only the
# release tagged v<version> to publish.
#
# The folder (<build>/publish by default) is laid out as GitHub serves a
# release's files, so that it can be served as it is to try the release:
#
#   v<version>/          bg3mse-<version>.zip (the app), bg3mse-<version>-updater-payload.zip,
#                        bg3mse-<version>-dSYM.zip, SHA256SUMS: the files of the release tagged v<version>
#   channel-<channel>/   Manifest.json, signed: the one file of the release tagged channel-<channel>,
#                        which the updater reads

set -eu

usage() {
    echo "usage: sh Mac/Release/publish.sh --build <dir> --key <file> (--previous <Manifest.json> | --first)" >&2
    echo "       [--min-game <version>] [--max-game <version>] [--notice <text>] [--repo <owner/name>] [--out <folder>] [--not-notarised]" >&2
    exit 2
}
fail() {
    echo "publish: $*" >&2
    exit 1
}

build= key= previous= first= min= max= notice= out= repo=downcats/bg3mse unnotarised=
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || [ "$1" = --first ] || [ "$1" = --not-notarised ] || usage
    case "$1" in
        --build) build=$2; shift 2 ;;
        --key) key=$2; shift 2 ;;
        --previous) previous=$2; shift 2 ;;
        --first) first=1; shift ;;
        --not-notarised) unnotarised=1; shift ;;
        --min-game) min=$2; shift 2 ;;
        --max-game) max=$2; shift 2 ;;
        --notice) notice=$2; shift 2 ;;
        --repo) repo=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$build" ] || usage
[ -n "$previous" ] && [ -n "$first" ] && usage

sign="$build/bg3mse-sign"
release="$build/release"

# The release the target made: one, whole.
set -- "$release"/bg3mse-*-updater-payload.zip
[ $# = 1 ] && [ -f "$1" ] || fail "$release holds no release, or more than one: build the release target again"
name=$(basename "$1" -updater-payload.zip)
version=${name#bg3mse-}
for file in "$name.zip" "$name-updater-payload.zip" "$name-dSYM.zip" SHA256SUMS; do
    [ -f "$release/$file" ] || fail "$release/$file is not there: build the release target with BG3MSE_UPDATER_PUBLIC_KEY set"
done
(cd "$release" && /usr/bin/shasum -a 256 -c SHA256SUMS > /dev/null) || fail "$release/SHA256SUMS does not hold for the files beside it"
channel=$(/usr/bin/plutil -extract channel raw "$release/$name/manifest.json") || fail "cannot read $release/$name/manifest.json"
commit=$(/usr/bin/plutil -extract commit raw "$release/$name/manifest.json")
game=$(/usr/bin/plutil -extract game_build_checked raw "$release/$name/manifest.json")
updates=$(/usr/bin/plutil -extract self_update raw "$release/$name/manifest.json" 2>/dev/null || echo true)

if [ "$updates" = false ]; then
    [ -z "$key$previous$first$min$max$notice" ] ||
        fail "this release does not update itself (BG3MSE_SELF_UPDATE=OFF): it has no channel, so --key, --first, --previous, --min-game, --max-game and --notice are not for it"
else
    [ -n "$key" ] || usage
    [ -n "$previous" ] || [ -n "$first" ] || usage
    [ -x "$sign" ] || fail "$sign is not there: build the release first (cmake --build $build -j 3 --target release)"
    [ -f "$key" ] || fail "$key is not there"
fi

# Notarised: the app as a player unzips it carries Apple's ticket.
check=$(/usr/bin/mktemp -d)
notarised=
if /usr/bin/ditto -x -k "$release/$name.zip" "$check" && /usr/bin/xcrun stapler validate "$check/bg3mse.app" > /dev/null 2>&1; then notarised=1; fi
rm -rf "$check"
if [ -z "$notarised" ]; then
    [ -n "$unnotarised" ] || fail "bg3mse.app in $name.zip is not notarised: macOS will block it until each player allows it in System Settings. Build it with BG3MSE_SIGN_IDENTITY and BG3MSE_NOTARY_PROFILE, or pass --not-notarised to lay it out all the same"
    echo "publish: WARNING: bg3mse.app in $name.zip is NOT notarised; its notes say how to open it" >&2
fi

out=${out:-$build/publish}
assets="$out/v$version"
manifest_dir="$out/channel-$channel"
q() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
notes() {  # the release's notes: what it is, and how to open it when it is not notarised
    printf 'bg3mse %s for Baldur'"'"'s Gate 3, checked against game version %s.%s\n' "$version" "$game" "$1" > "$out/notes-v$version.md"
    if [ -z "$notarised" ]; then
        printf '\nThis release isn'"'"'t notarised by Apple, so macOS blocks bg3mse the first time.\n' >> "$out/notes-v$version.md"
        printf 'Go to System Settings > Privacy & Security, click Open Anyway next to "bg3mse", then Open.\n' >> "$out/notes-v$version.md"
    fi
}
print_release() {  # the command that publishes the release tagged v<version>
    echo "gh release create v$version --repo $repo --target $commit --title $(q "bg3mse $version") --notes-file $(q "$out/notes-v$version.md") --latest \\"
    for file in "$name.zip" "$name-updater-payload.zip" "$name-dSYM.zip" SHA256SUMS; do
        echo "    $(q "$assets/$file") \\"
    done | sed '$ s/ \\$//'
}

if [ "$updates" = false ]; then
    rm -rf "$assets"
    mkdir -p "$assets"
    for file in "$name.zip" "$name-updater-payload.zip" "$name-dSYM.zip" SHA256SUMS; do
        cp "$release/$file" "$assets/"
    done
    notes " Doesn't update itself."
    echo "Ready in $out (a release that does not update itself: no channel manifest):"
    (cd "$out" && find "v$version" -type f | sort | sed 's/^/  /')
    echo
    echo "To publish, with commit $commit pushed to $repo:"
    echo
    print_release
    exit 0
fi

# The key signs what the release's loader checks with.
public=$("$sign" public "$key") || fail "cannot read the update key $key"
public=$(printf '%s' "$public" | tr 'A-F' 'a-f')
LC_ALL=C grep -q -a -F "$public" "$release/$name/libbg3mse-loader.dylib" || fail "$key is not the update key this release was built with (BG3MSE_UPDATER_PUBLIC_KEY)"

rm -rf "$assets" "$manifest_dir"
mkdir -p "$assets" "$manifest_dir"
for file in "$name.zip" "$name-updater-payload.zip" "$name-dSYM.zip" SHA256SUMS; do
    cp "$release/$file" "$assets/"
done

set -- manifest --key "$key" --channel "$channel" --out "$manifest_dir/Manifest.json"
[ -n "$previous" ] && set -- "$@" --previous "$previous"
set -- "$@" --add "$assets/$name-updater-payload.zip"
[ -n "$min" ] && set -- "$@" --min-game "$min"
[ -n "$max" ] && set -- "$@" --max-game "$max"
[ -n "$notice" ] && set -- "$@" --release-notice "$notice"
"$sign" "$@" > /dev/null || fail "bg3mse-sign could not write the channel's manifest"
"$sign" verify "$public" "$manifest_dir/Manifest.json" > /dev/null || fail "the channel's manifest does not verify"
notes ""

echo "Ready in $out:"
(cd "$out" && find "v$version" "channel-$channel" -type f | sort | sed 's/^/  /')
echo
echo "To publish, with commit $commit pushed to $repo:"
echo
print_release
echo
if [ -n "$first" ]; then
    echo "gh release create channel-$channel --repo $repo --target $commit --title $(q "$channel channel") --notes '' --latest=false $(q "$manifest_dir/Manifest.json")"
else
    echo "gh release upload channel-$channel --repo $repo --clobber $(q "$manifest_dir/Manifest.json")"
fi
