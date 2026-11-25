#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Install the tap's formulae from local bottles, before they are uploaded:
#   install-local.sh DIR [FORMULA...]      (default FORMULA: seahorn)
# DIR is a `docker/release.py --homebrew` output (artifacts/homebrew): the
# formulae with their bottle blocks in DIR/Formula (release source) or
# DIR/Formula-local (a local source, not for publishing), the bottles in
# DIR/bottles. The script taps a copy of those formulae whose root_url is
# file://DIR/bottles as seahorn/seahorn, trusts it, installs, and fails
# unless every installed formula was poured from a bottle (no source build).
set -euo pipefail
[[ $# -ge 1 ]] || { echo 'usage: install-local.sh DIR [FORMULA...]' >&2; exit 2; }
dir=$(CDPATH='' cd -- "$1" && pwd)
shift
formulae=("${@:-seahorn}")
formula_dir=$dir/Formula
[[ -d "$formula_dir" ]] || formula_dir=$dir/Formula-local
[[ -d "$formula_dir" && -d "$dir/bottles" ]] ||
  { echo "$dir: no Formula/ (or Formula-local/) or bottles/" >&2; exit 2; }
command -v brew > /dev/null || { echo 'brew is not on PATH' >&2; exit 2; }
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_ENV_HINTS=1

if brew tap | grep -qx seahorn/seahorn; then
  echo 'seahorn/seahorn is already tapped: brew uninstall its formulae and' \
       'brew untap seahorn/seahorn first' >&2
  exit 2
fi
tap=$(mktemp -d)/homebrew-seahorn
mkdir -p "$tap/Formula"
cp "$formula_dir"/*.rb "$tap/Formula/"
# shellcheck disable=SC2016 # Ruby code, not shell
brew ruby -e '
  root = ARGV.shift
  ARGV.each do |path|
    text = File.read(path)
    File.write(path, text.gsub(/^(\s*root_url )"[^"]*"/) { "#{Regexp.last_match(1)}\"#{root}\"" })
  end
' "file://$dir/bottles" "$tap"/Formula/*.rb
grep -h 'root_url' "$tap"/Formula/*.rb
git -C "$tap" init -q
git -C "$tap" add Formula
git -C "$tap" -c user.name=local -c user.email=local@localhost commit -q -m 'local bottles'
brew tap seahorn/seahorn "$tap"
# Homebrew loads formulae of other taps only once they are trusted
brew trust seahorn/seahorn
for formula in "${formulae[@]}"; do
  brew install "seahorn/seahorn/$formula"
done
# every installed keg, the dependencies from homebrew-core included
brew ruby -e '
  built = Formula.installed.reject { |f| f.any_installed_keg&.tab&.poured_from_bottle }
  abort "built from source, not poured: #{built.map(&:full_name).join(" ")}" unless built.empty?
  puts "poured from bottles: #{Formula.installed.map(&:full_name).sort.join(" ")}"
'
