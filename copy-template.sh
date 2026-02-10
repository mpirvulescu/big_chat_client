#!/usr/bin/env bash
# copy-template.sh
# Portable template copier with controlled symlinks.
# Platforms: macOS, FreeBSD 13+, Linux.

set -euo pipefail

# ---------- configuration -----------------------------------------------

# These must be symlinks in the destination.
LINK_ITEMS=(
  "sanitizers.txt"
  ".flags"
  "supported_c_compilers.txt"
)

# These must be copied in the destination.
COPY_ITEMS=(
  ".clang-format"
  ".gitignore"
  "build.sh"
  "build-all.sh"
  "change-compiler.sh"
  "check-compilers.sh"
  "check-env.sh"
  "create-links.sh"
  "move.sh"
  "CMakeLists.txt"
  "config.cmake"
  "generate-flags.sh"
  "README.md"
  "src"
  "include"
)

# ---------- options ------------------------------------------------------

FORCE=0   # overwrite conflicts
DRYRUN=0  # print actions only
QUIET=0   # reduce chatter

usage() {
  echo "Usage: $0 [-f] [-n] [-q] <destination_directory>"
  echo "  -f  force overwrite of existing paths"
  echo "  -n  dry run, print actions without changes"
  echo "  -q  quiet mode"
  exit 2
}

while getopts ":fnq" opt; do
  case "$opt" in
    f) FORCE=1 ;;
    n) DRYRUN=1 ;;
    q) QUIET=1 ;;
    *) usage ;;
  esac
done
shift $((OPTIND-1))
[ $# -eq 1 ] || usage

dest_dir=$1

say()  { [ "$QUIET" -eq 1 ] || echo "$@"; }
warn() { echo "WARN: $@" >&2; }
die()  { echo "ERROR: $@" >&2; exit 4; }

# ---------- portable path helpers ---------------------------------------

# Absolute path without relying on realpath or readlink -f.
abspath() {
  # final component may not exist, resolve parent only
  local p=$1 dir base
  dir=$(dirname -- "$p")
  base=$(basename -- "$p")
  # shellcheck disable=SC2164
  ( cd "$dir" 2>/dev/null || cd .; pwd -P ) | awk -v b="$base" '{print $0"/"b}'
}

# Compute relative path from $2's directory to $1. Falls back to absolute.
relpath() {
  local target=$1 from=$2
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$target" "$from" <<'PY'
import os,sys
t,f=sys.argv[1],sys.argv[2]
print(os.path.relpath(t, start=os.path.dirname(f)))
PY
  else
    # Fallback, acceptable on systems without Python 3.
    echo "$target"
  fi
}

mkd() {
  local d=$1
  if [ ! -d "$d" ]; then
    if [ "$DRYRUN" -eq 1 ]; then say "mkdir -p $d"; else mkdir -p "$d"; fi
  fi
}

clobber_if_needed() {
  local p=$1
  if [ -e "$p" ] || [ -L "$p" ]; then
    if [ "$FORCE" -eq 1 ]; then
      if [ "$DRYRUN" -eq 1 ]; then say "rm -rf $p"; else rm -rf -- "$p"; fi
    else
      warn "$p exists, use -f to overwrite"
      return 1
    fi
  fi
  return 0
}

copy_item() {
  # Directories use cp -a, files preserve perms and times.
  local src=$1 dst=$2
  if [ -d "$src" ] && [ ! -L "$src" ]; then
    if [ "$DRYRUN" -eq 1 ]; then
      say "cp -a \"$src\" \"$dst\""
    else
      cp -a "$src" "$dst"
    fi
  else
    if [ "$DRYRUN" -eq 1 ]; then
      say "install -p \"$src\" \"$dst\""
    else
      if ! install -p "$src" "$dst" 2>/dev/null; then
        cp -p "$src" "$dst"
      fi
    fi
  fi
}

link_item() {
  local src=$1 dst=$2
  local rel
  rel=$(relpath "$src" "$dst")
  if [ "$DRYRUN" -eq 1 ]; then
    say "ln -s \"$rel\" \"$dst\""
  else
    ln -s "$rel" "$dst"
  fi
}

# ---------- main ---------------------------------------------------------

src_root=$(abspath ".")
dest_dir=$(abspath "$dest_dir")
mkd "$dest_dir"

# Validate sources exist, warn if not.
for i in "${LINK_ITEMS[@]}";  do [ -e "$src_root/$i" ] || [ -L "$src_root/$i" ] || warn "missing link source: $i"; done
for i in "${COPY_ITEMS[@]}";  do [ -e "$src_root/$i" ] || [ -L "$src_root/$i" ] || warn "missing copy source: $i"; done

# Create required links.
for i in "${LINK_ITEMS[@]}"; do
  src="$src_root/$i"; dst="$dest_dir/$i"
  [ -e "$src" ] || [ -L "$src" ] || { warn "skip link, not found: $i"; continue; }
  if ! clobber_if_needed "$dst"; then exit 3; fi
  mkd "$(dirname -- "$dst")"
  link_item "$src" "$dst"
  say "linked: $i"
done

# Copy required items.
for i in "${COPY_ITEMS[@]}"; do
  src="$src_root/$i"; dst="$dest_dir/$i"
  [ -e "$src" ] || [ -L "$src" ] || { warn "skip copy, not found: $i"; continue; }

  if [ -d "$src" ] && [ ! -L "$src" ]; then
    if [ -e "$dst" ] || [ -L "$dst" ]; then
      if [ "$FORCE" -eq 1 ]; then
        [ "$DRYRUN" -eq 1 ] && say "rm -rf \"$dst\"" || rm -rf -- "$dst"
      else
        warn "$dst exists, use -f to overwrite"; exit 3
      fi
    fi
    mkd "$(dirname -- "$dst")"
    copy_item "$src" "$dst"
  else
    mkd "$(dirname -- "$dst")"
    if ! clobber_if_needed "$dst"; then exit 3; fi
    copy_item "$src" "$dst"
  fi
  say "copied: $i"
done

# Initialize per destination if .flags is not present.
# You have .flags in LINK_ITEMS. If you want per-project .flags, move it
# from LINK_ITEMS to COPY_ITEMS and keep this block.
pushd "$dest_dir" >/dev/null
if [ ! -e ".flags" ] && [ ! -L ".flags" ]; then
  if [ "$DRYRUN" -eq 1 ]; then
    say "./check-compilers.sh && ./generate-flags.sh"
  else
    say "initializing compiler flags"
    ./check-compilers.sh
    ./generate-flags.sh
  fi
else
  say ".flags present, skip initialization"
fi
popd >/dev/null

say "done."
