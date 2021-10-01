#!/bin/sh
set -e

export SUNDER_HOME="$(realpath ..)"
export SUNDER_IMPORT_PATH="${SUNDER_HOME}/lib"

for f in *.sunder; do
    CMD="${SUNDER_HOME}/bin/sunder-compile -k -o ${f%.sunder} ${f}"
    echo $CMD
    eval $CMD
done
