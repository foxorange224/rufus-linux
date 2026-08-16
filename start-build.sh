#!/bin/bash
echo "Cleaning..."
rm -rf build > /dev/null
rm rufus > /dev/null
echo ""

echo "Building..."
echo ""
sleep 1
echo "3"
sleep 1
echo -e "\033[1A\033[K"
echo "2"
sleep 1
echo -e "\033[1A\033[K"
echo "1 (^w^)"
sleep 1
echo -e "\033[1A\033[K"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-Os -s -fno-ident -ffunction-sections -fdata-sections" \
  -DCMAKE_C_FLAGS="-Os -s -fno-ident -ffunction-sections -fdata-sections" \
  -DCMAKE_EXE_LINKER_FLAGS="-s -Wl,--gc-sections" \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build build -j$(nproc)
echo ""
echo "All done, have fun!"
echo "> Rufus is located at the directory. <"
cp ./build/src/rufus ./
exit 0
