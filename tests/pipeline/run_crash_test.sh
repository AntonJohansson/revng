#!/bin/bash
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

# TODO: errexit and pipefail are intentionally not set. Here I'm setting and
# and unsetting them just to silence check-conventions. Remove hack once
# silencing functionality is added to check-conventions.
set -euo pipefail

set +o pipefail
set +o errexit

CMAKE_BINARY_DIR="$1"
EXPECTED_STATUS="$2"
export REVNG_CRASH_SIGNAL="$3"

"${CMAKE_BINARY_DIR}/libexec/revng/revng-pipeline" --load "${CMAKE_BINARY_DIR}/lib/librevngBadBehaviorLibrary.so" 2>&1 \
    | tee -ip /dev/stderr \
    | grep -q 'doCrash()\|WillCrash::WillCrash()\|librevngBadBehaviorLibrary.so'
STATUS=("${PIPESTATUS[@]}")

echo "Status: ${STATUS[*]}"

if [[ "${STATUS[0]}" != "${EXPECTED_STATUS}" ]]; then
  echo "revng-pipeline exited with incorrect exit status code ${STATUS[0]} (!= ${EXPECTED_STATUS})"
  exit 1
elif [[ "${STATUS[2]}" != "0" ]]; then
  echo 'revng-pipeline did not output a stack trace'
  exit 1
else
  exit 0
fi
