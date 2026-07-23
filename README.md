# Noxiouse

name: Build

on:
  push:
    branches: [ custom-engine ]
  pull_request:
    branches: [ custom-engine ]

jobs:

  build:

    name: ${{ matrix.env.name }}

    runs-on: ${{ matrix.env.os }}

    strategy:
      matrix:
        env:

          # MSVC + Visual Studio solution build
          - name: MSVC VS2026
            os: windows-2025-vs2026
            c: cl
            cxx: cl
            arch: x64
            gen: Visual Studio 18 2026


          # clang-cl + Ninja build (Khronos style)
          - name: ClangCL Ninja
            os: windows-2025-vs2026
            c: clang-cl
            cxx: clang-cl
            arch: x64
            gen: Ninja


    steps:


    - name: Checkout
      uses: actions/checkout@v7
      with:
        submodules: recursive



    #
    # Cache vcpkg
    #
    - name: Cache vcpkg
      uses: actions/cache@v5
      with:
        path: |
          NoxCore/vendors/vcpkg
        key: windows-vcpkg-${{ hashFiles('NoxCore/**') }}
        restore-keys: |
          windows-vcpkg-



    #
    # Install dependencies
    #
    - name: Install dependencies
      shell: cmd
      run: |

        if not exist NoxCore\vendors\vcpkg (
            git clone https://github.com/microsoft/vcpkg.git NoxCore\vendors\vcpkg
        )


        cd NoxCore\vendors\vcpkg


        if not exist vcpkg.exe (
            call bootstrap-vcpkg.bat
        )


        cd ..\..\..

        
        NoxCore\vendors\vcpkg\vcpkg.exe install ^
          sdl3[vulkan] ^
          glm ^
          entt ^
          spdlog ^
          xxhash ^
          yaml-cpp ^
          box2d ^
          imguizmo ^
          freetype ^
          skia ^
          stb ^
          ktx[vulkan] ^
          tinyobjloader ^
          --triplet=x64-windows


    #
    # Install FileWatch
    #
    - name: Install FileWatch
      shell: cmd
      run: |

        if not exist NoxCore\vendors\filewatch (
          git clone https://github.com/ThomasMonkman/filewatch.git NoxCore\vendors\filewatch
        )

    #
    # Install Ninja only for clang-cl job
    #
    - name: Install Ninja
      if: matrix.env.gen == 'Ninja'
      shell: cmd
      run: |

        winget install Ninja-build.Ninja ^
          --accept-source-agreements ^
          --accept-package-agreements



    #
    # Cache Vulkan SDK
    #
    - name: Cache Vulkan SDK
      id: vulkan-cache
      uses: actions/cache@v5
      with:
        path: C:\VulkanSDK\1.4.350.0
        key: windows-vulkan-sdk-1.4.350.0



    #
    # Install Vulkan SDK
    #
    - name: Install Vulkan SDK
      if: steps.vulkan-cache.outputs.cache-hit != 'true'
      shell: pwsh
      run: |

        winget install `
          --id KhronosGroup.VulkanSDK `
          --exact `
          --version 1.4.350.0 `
          --accept-source-agreements `
          --accept-package-agreements



    #
    # Setup Vulkan environment
    #
    - name: Setup Vulkan environment
      shell: pwsh
      run: |

        $version = "1.4.350.0"

        echo "VULKAN_SDK=C:\VulkanSDK\$version" >> $env:GITHUB_ENV
        echo "Vulkan_INCLUDE_DIR=C:\VulkanSDK\$version\Include" >> $env:GITHUB_ENV
        echo "Vulkan_LIBRARY=C:\VulkanSDK\$version\Lib\vulkan-1.lib" >> $env:GITHUB_ENV


        echo "Vulkan SDK:"
        dir C:\VulkanSDK\$version

     #
    # Install Slang
    #
    - name: Install Slang
      shell: cmd
      run: |

        set SLANG_VERSION=2026.13

        curl -L ^
        https://github.com/shader-slang/slang/releases/download/v%SLANG_VERSION%/slang-%SLANG_VERSION%-windows-x86_64.zip ^
        -o slang.zip


        if exist NoxCore\vendors\slang (
            rmdir /s /q NoxCore\vendors\slang
        )


        mkdir NoxCore\vendors\slang


        tar -xf slang.zip -C NoxCore\vendors\slang


        del slang.zip


        echo ==== Slang contents ====
        dir NoxCore\vendors\slang
    
    #
    # Configure CMake
    #
    - name: Configure CMake
      shell: cmd
      run: |

        cmake -S . -B build ^
        -G "${{ matrix.env.gen }}" ^
        -D CMAKE_C_COMPILER=${{ matrix.env.c }} ^
        -D CMAKE_CXX_COMPILER=${{ matrix.env.cxx }} ^
        -DCMAKE_TOOLCHAIN_FILE=NoxCore\vendors\vcpkg\scripts\buildsystems\vcpkg.cmake ^
        -DVulkan_INCLUDE_DIR=%Vulkan_INCLUDE_DIR% ^
        -DVulkan_LIBRARY=%Vulkan_LIBRARY% ^
        -DCMAKE_BUILD_TYPE=Release



    #
    # Build
    #
    - name: Build
      shell: cmd
      run: |

        cmake --build build --parallel --config Release
