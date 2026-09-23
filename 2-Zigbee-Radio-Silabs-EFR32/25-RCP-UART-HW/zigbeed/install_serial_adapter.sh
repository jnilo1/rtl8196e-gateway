#!/usr/bin/env bash
# Substitute the generated Silicon Labs adapter in a generated Zigbeed tree.
# The SDK installation is never modified; only the disposable build tree is.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
generated_dir=${1:?usage: install_serial_adapter.sh GENERATED_ZIGBEED_DIR}
vendor_adapter="${generated_dir}/serial_adapter.c"
project_adapter="${script_dir}/serial_adapter_posix.c"
project_header="${script_dir}/serial_adapter_posix.h"

[[ -d "${generated_dir}" ]] || { echo "ERROR: generated Zigbeed directory not found: ${generated_dir}" >&2; exit 1; }
[[ -f "${vendor_adapter}" ]] || { echo "ERROR: expected generated vendor serial_adapter.c not found: ${vendor_adapter}" >&2; exit 1; }
[[ -f "${project_adapter}" && -f "${project_header}" ]] || { echo "ERROR: project serial adapter source is incomplete" >&2; exit 1; }
grep -Rqs -- "serial_adapter.c" "${generated_dir}"/*.Makefile "${generated_dir}"/*.mak || { echo "ERROR: generated makefiles do not reference serial_adapter.c" >&2; exit 1; }

# SLC generated makefiles compile this project-local name.  Replace the working
# copy atomically with the project source so no vendor adapter object is linked.
install -m 0644 "${project_adapter}" "${vendor_adapter}"
install -m 0644 "${project_header}" "${generated_dir}/serial_adapter_posix.h"
sed -i 's|\$(SDK_PATH)/protocol/zigbee/app/projects/zigbeed/serial_adapter\.c|serial_adapter.c|g' "${generated_dir}/zigbeed.project.mak"
grep -Fq 'serial_adapter.c' "${generated_dir}/zigbeed.project.mak" || { echo "ERROR: generated makefile adapter reference was not replaced" >&2; exit 1; }
grep -Fq '\$(SDK_PATH)/protocol/zigbee/app/projects/zigbeed/serial_adapter.c' "${generated_dir}/zigbeed.project.mak" && { echo "ERROR: vendor adapter remains in generated makefile" >&2; exit 1; }
grep -Fq "POSIX transport adapter for the generated Zigbeed application" "${vendor_adapter}" || { echo "ERROR: adapter substitution did not take effect" >&2; exit 1; }
echo "Using jnilo1 Zigbeed serial adapter: PTY + native TCP"
