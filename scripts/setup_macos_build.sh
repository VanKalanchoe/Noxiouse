#!/bin/bash
echo Configuring Noxiouse with Xcode + ClangCL...

cmake -S .. -B ../build -G "Xcode" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_TOOLCHAIN_FILE=../NoxCore/vendors/vcpkg/scripts/buildsystems/vcpkg.cmake

if [ $? -ne 0 ]
then
    echo
    echo "CMake configuration failed."
    read -p "Press enter to exit"
    exit $?
fi

echo.
echo CMake configuration successful.
read -p "Press enter to exit"