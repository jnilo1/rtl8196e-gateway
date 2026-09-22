#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
adapter_dir=$(cd "${script_dir}/.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/serial-adapter-test.XXXXXX")
generated_dir=$(mktemp -d "${TMPDIR:-/tmp}/serial-adapter-generated.XXXXXX")
trap 'rm -f "${binary}"; rm -rf "${generated_dir}"' EXIT

cc -std=c11 -Wall -Wextra -Wpedantic -Werror -D_GNU_SOURCE \
  -I"${script_dir}/stubs" -I"${adapter_dir}" \
  "${adapter_dir}/serial_adapter_posix.c" "${script_dir}/test_serial_adapter_posix.c" \
  -lutil -o "${binary}"
"${binary}"

printf 'generated vendor adapter\n' > "${generated_dir}/serial_adapter.c"
printf 'sources = serial_adapter.c\n' > "${generated_dir}/zigbeed.Makefile"
printf 'source = $(SDK_PATH)/protocol/zigbee/app/projects/zigbeed/serial_adapter.c\n' > "${generated_dir}/zigbeed.project.mak"
"${adapter_dir}/install_serial_adapter.sh" "${generated_dir}"
cmp "${adapter_dir}/serial_adapter_posix.c" "${generated_dir}/serial_adapter.c"
cmp "${adapter_dir}/serial_adapter_posix.h" "${generated_dir}/serial_adapter_posix.h"
echo "adapter substitution test passed"
