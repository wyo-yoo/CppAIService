#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
deps_dir="${CPP_AI_DEPS_DIR:-$(dirname -- "$project_dir")/.cppaiservice-deps}"
cmake -S "$project_dir" -B "$project_dir/build-chat" \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$deps_dir/install" -DCHAT_BUILD_TESTS=ON
cmake --build "$project_dir/build-chat" -j "${BUILD_JOBS:-2}"
if [[ ! -e "$project_dir/compile_commands.json" && ! -L "$project_dir/compile_commands.json" ]]; then
    ln -s build-chat/compile_commands.json "$project_dir/compile_commands.json"
fi
ctest --test-dir "$project_dir/build-chat" --output-on-failure
