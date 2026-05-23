#!/bin/sh
#
# SPDX-FileCopyrightText: 2025 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
# apply-patches.sh -- Re-apply hardware/interfaces patches required for the
# checkers camera HAL after repo sync resets those sub-repos.
#
# Usage (from anywhere in the source tree):
#   sh device/amazon/checkers/patches/apply-patches.sh
#
# The script is idempotent: patches already applied are detected via reverse-
# check and skipped without error. See patches/README.md for what each patch
# addresses.

set -e

SCRIPT_DIR="$( cd "$( dirname "$0" )" && pwd )"
LINEAGE_ROOT="$( cd "${SCRIPT_DIR}/../../../.." && pwd )"

echo "Lineage root: ${LINEAGE_ROOT}"
echo "Patches dir:  ${SCRIPT_DIR}"
echo ""

HW_INTERFACES="${LINEAGE_ROOT}/hardware/interfaces"
if [ ! -d "${HW_INTERFACES}" ]; then
    echo "ERROR: ${HW_INTERFACES} not found. Run repo sync first." >&2
    exit 1
fi

FRAMEWORKS_AV="${LINEAGE_ROOT}/frameworks/av"
if [ ! -d "${FRAMEWORKS_AV}" ]; then
    echo "ERROR: ${FRAMEWORKS_AV} not found. Run repo sync first." >&2
    exit 1
fi

PACKAGES_APPS_CAMERA2="${LINEAGE_ROOT}/packages/apps/Camera2"
if [ ! -d "${PACKAGES_APPS_CAMERA2}" ]; then
    echo "ERROR: ${PACKAGES_APPS_CAMERA2} not found. Run repo sync first." >&2
    exit 1
fi

FRAMEWORKS_BASE="${LINEAGE_ROOT}/frameworks/base"
if [ ! -d "${FRAMEWORKS_BASE}" ]; then
    echo "ERROR: ${FRAMEWORKS_BASE} not found. Run repo sync first." >&2
    exit 1
fi

applied_count=0
skipped_count=0
error_count=0

# Collect all known patch filename patterns sorted lexicographically so
# application order is deterministic. The case dispatch below routes each
# filename to its target sub-repo based on the prefix.
patch_list="${SCRIPT_DIR}/.patch_list.$$"
find "${SCRIPT_DIR}" -maxdepth 1 \
    \( -name '[0-9][0-9][0-9][0-9]-*.patch' \
    -o -name 'hardware-interfaces-*.patch' \
    -o -name 'frameworks-av-*.patch' \
    -o -name 'frameworks-base-*.patch' \
    -o -name 'packages-apps-Camera2-*.patch' \) | sort > "${patch_list}"

while IFS= read -r patch; do
    name="$(basename "${patch}")"

    # Filename convention dispatches by prefix:
    #   hardware-interfaces-*    -> hardware/interfaces
    #   frameworks-av-*          -> frameworks/av
    #   frameworks-base-*        -> frameworks/base
    #   packages-apps-Camera2-*  -> packages/apps/Camera2
    #   NNNN-name.patch (legacy) -> hardware/interfaces
    case "${name}" in
        hardware-interfaces-*|[0-9][0-9][0-9][0-9]-*.patch)
            target_repo="${HW_INTERFACES}"
            ;;
        frameworks-av-*)
            target_repo="${FRAMEWORKS_AV}"
            ;;
        frameworks-base-*)
            target_repo="${FRAMEWORKS_BASE}"
            ;;
        packages-apps-Camera2-*)
            target_repo="${PACKAGES_APPS_CAMERA2}"
            ;;
        *)
            echo "WARN: Unknown prefix for ${name} -- skipping (add handling to this script)" >&2
            error_count=$((error_count + 1))
            continue
            ;;
    esac

    echo "Checking: ${name}"
    if git -C "${target_repo}" apply --check "${patch}" 2>/dev/null; then
        git -C "${target_repo}" apply "${patch}"
        echo "  applied"
        applied_count=$((applied_count + 1))
    elif git -C "${target_repo}" apply --check --reverse "${patch}" 2>/dev/null; then
        echo "  already applied -- skipping"
        skipped_count=$((skipped_count + 1))
    else
        echo "  ERROR: patch does not apply and is not already applied -- investigate" >&2
        echo "  Patch:  ${patch}" >&2
        echo "  Target: ${target_repo}" >&2
        error_count=$((error_count + 1))
    fi
    echo ""
done < "${patch_list}"
rm -f "${patch_list}"

echo "Summary: ${applied_count} applied, ${skipped_count} already-applied, ${error_count} errors"
if [ "${error_count}" -gt 0 ]; then
    echo "ERROR: ${error_count} patch(es) failed -- see above" >&2
    exit 1
fi
echo "Done."
