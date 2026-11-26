# cmake -DIN=<shader.wgsl> -DOUT=<header.h> -DNAME=<symbol> -P WgslHeader.cmake
# WGSL text as a byte-array header shaped like DXC's -Fh output (const unsigned char NAME[] = {...}), ending in a 0 byte so the
# WebGPU backend can pass it as a string. sizeof(NAME) counts that 0.
# Storage textures become write-only: HLSL's RWTexture2D is read_write in SPIR-V, which WebGPU allows for few formats without
# extra features, and shaders only store to them (shaders/Compute.hlsli mkRwStore2D). The edited WGSL is written next to IN.
file(READ ${IN} wgsl)
string(REGEX REPLACE "texture_storage_2d<([a-z0-9]+), read_write>" "texture_storage_2d<\\1, write>" wgsl "${wgsl}")
# Immediates (push constants) are a WGSL language feature in browsers (Chrome 150+): declared so a browser without it fails clearly
if(wgsl MATCHES "var<immediate>")
    string(PREPEND wgsl "requires immediate_address_space;\n")
endif()
file(WRITE ${IN}.final "${wgsl}")
file(READ ${IN}.final hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n    " bytes "${bytes}")
file(WRITE ${OUT} "// Generated from ${IN} (WGSL, Tint). Do not edit.\nconst unsigned char ${NAME}[] = {\n    ${bytes}0x00};\n")
