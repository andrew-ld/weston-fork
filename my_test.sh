#!/bin/bash

export LD_LIBRARY_PATH="$(pwd)/install/lib/":${LD_LIBRARY_PATH}
export PATH="$(pwd)/install/bin/":${PATH}

weston --xwayland --shell=kiosk
