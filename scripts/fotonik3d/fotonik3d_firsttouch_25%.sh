#!/bin/bash

export SICM_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null && cd ../.. && pwd )"
source $SICM_DIR/scripts/all/firsttouch.sh
cd $SICM_DIR/examples/high/fotonik3d/run

firsttouch ".25" "./fotonik3d"
