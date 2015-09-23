#!/bin/sh

# **********************makefile_linux_ppc_5020_2637_r
make -f makefile_linux_ppc_5020_r clean

make -f makefile_linux_ppc_5020_r clean all 1> ../../../../10-common/version/compileinfo/zkclient_makefile_linux_ppc_5020_r.txt
cp ./libzkclient.a ../../../../10-common/lib/release/linux_ppc_qoriq_2637/
cp ./libzkclient.a ../../../../10-common/lib/debug/linux_ppc_qoriq_2637/
echo makefile_linux_ppc_5020_r completed



echo makefile_linux_x86_r

make -f makefile_linux_x86_r clean
make -f makefile_linux_x86_r 1> ../../../../10-common/version/compileinfo/zkclient_kdvp_linux_x86_release.txt 2>&1
cp ./libzkclient.a ../../../../10-common/lib/release/linux
cp ./libzkclient.a ../../../../10-common/lib/debug/linux

echo makefile_zkclient completed

cd ..

