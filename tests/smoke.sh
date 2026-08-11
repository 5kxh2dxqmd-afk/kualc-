#!/bin/sh
set -eu
binary=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/fixtures/extensions" && pwd)
test "$("$binary" --root "$root" --list)" = "Hello	printf hello
Tools
  Nested	printf nested"
test "$("$binary" --root "$root" --run Hello)" = hello
test "$("$binary" --root "$root" --run Tools/Nested)" = nested
