#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# One formula of docker/Homebrew.Dockerfile: add it to the tap, build it from
# source for a bottle, `brew test`, `brew bottle --json`. The bottle gets the
# remote file name (brew bottle writes `<name>--<version>...`; the release
# asset and the formula's URL are `<name>-<version>...`), so the directory
# can be uploaded or served as is. Timings go to timings.tsv.
set -euo pipefail
[[ $# -eq 2 ]] || { echo 'usage: homebrew-bottle.sh FORMULA ROOT_URL' >&2; exit 2; }
name=$1
root_url=$2
[[ "$root_url" == https://* ]] || { echo "ROOT_URL is not an https URL: '$root_url'" >&2; exit 2; }
out=$HOME/artifacts/homebrew
bottles=$out/bottles
cp "$HOME/tap/$name.rb" "$TAP/Formula/$name.rb"
git -C "$TAP" add "Formula/$name.rb"
git -C "$TAP" commit -q -m "$name"
version=$(brew ruby -e "puts Formula['seahorn/seahorn/$name'].version")
timed() { # timed STEP COMMAND...
  local start=$SECONDS
  "${@:2}"
  printf '%s\t%s\t%s\n' "$name" "$1" "$((SECONDS - start))" >> "$out/timings.tsv"
}
timed install brew install --build-bottle "seahorn/seahorn/$name"
# every linked library broken-free and from a declared dependency
timed linkage brew linkage --test --strict "seahorn/seahorn/$name"
timed test brew test --verbose "seahorn/seahorn/$name"
cd "$bottles"
timed bottle brew bottle --json --no-rebuild \
  --root-url="$root_url/$name-$version" "seahorn/seahorn/$name"
# the image has no Python: Homebrew's Ruby reads the JSON
brew ruby -e '
  name = ARGV.fetch(0)
  Dir["#{name}--*.bottle.json"].each do |path|
    JSON.parse(File.read(path)).fetch("seahorn/seahorn/#{name}").fetch("bottle").fetch("tags").each_value do |tag|
      File.rename(tag.fetch("local_filename"), tag.fetch("filename"))
      puts "bottle: #{tag.fetch("filename")} sha256=#{tag.fetch("sha256")}"
    end
  end
' "$name"
mkdir -p "$out/logs/$name"
cp -R "$HOME/.cache/Homebrew/Logs/$name/." "$out/logs/$name/" 2>/dev/null || :
