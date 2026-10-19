#!/bin/bash
# Build con toolchain NCS v3.3.1 (percorso senza spazi)
export PATH="/c/ncs/toolchains/936afb6332/opt/bin:/c/ncs/toolchains/936afb6332/opt/bin/Scripts:/c/ncs/toolchains/936afb6332/bin:/c/ncs/toolchains/936afb6332/mingw64/bin:/c/ncs/toolchains/936afb6332/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_BASE=C:/ncs/v3.3.1/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=C:/ncs/toolchains/936afb6332/opt/zephyr-sdk
cd /c/ncs/work
west build -b xiao_ble -d as7341_mesh_node/build --no-sysbuild as7341_mesh_node -p auto
