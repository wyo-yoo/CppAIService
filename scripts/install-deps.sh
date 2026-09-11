#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
deps_dir="${CPP_AI_DEPS_DIR:-$(dirname -- "$project_dir")/.cppaiservice-deps}"
jobs="${BUILD_JOBS:-2}"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake git python3 ca-certificates pkg-config \
    libssl-dev libcurl4-openssl-dev nlohmann-json3-dev \
    libboost-dev libboost-chrono-dev libboost-system-dev \
    libmysqlcppconn-dev default-libmysqlclient-dev librabbitmq-dev \
    mysql-server rabbitmq-server
mkdir -p "$deps_dir/src" "$deps_dir/install"

# Pin the revisions used by this project's verified build. Keep existing source checkouts intact.
fetch_source() {
    local name="$1" revision="$2" url="$3"
    source_dir="$deps_dir/src/$name-$revision"
    if [[ ! -d "$source_dir/.git" ]]; then
        git init "$source_dir"
        git -C "$source_dir" remote add origin "$url"
    fi
    if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$source_dir" fetch --depth=1 origin "$revision"
    fi
    if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
        printf 'Source directory contains local changes: %s\n' "$source_dir" >&2
        return 1
    fi
    git -C "$source_dir" checkout --detach "$revision"
}

fetch_source muduo f1fc77e0c13b80e5086ff457362c8a86d1b609d4 https://github.com/chenshuo/muduo.git
build_dir="$deps_dir/build/$(basename -- "$source_dir")"
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps_dir/install" -DMUDUO_BUILD_EXAMPLES=OFF
cmake --build "$build_dir" -j "$jobs"
cmake --install "$build_dir"

fetch_source SimpleAmqpClient cf44cc810a07ef68657881c9aedaf5c5a699bbfd https://github.com/alanxz/SimpleAmqpClient.git
build_dir="$deps_dir/build/$(basename -- "$source_dir")"
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$deps_dir/install" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTING=OFF -DBUILD_API_DOCS=OFF
cmake --build "$build_dir" -j "$jobs"
cmake --install "$build_dir"
sudo systemctl start mysql rabbitmq-server
printf 'Dependencies are installed in %s/install\n' "$deps_dir"
