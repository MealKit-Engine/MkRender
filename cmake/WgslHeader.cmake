# cmake -DIN=<shader.wgsl> -DOUT=<header.h> -DNAME=<symbol> -P WgslHeader.cmake
# WGSL text as a byte-array header shaped like DXC's -Fh output (const unsigned char NAME[] = {...}), ending in a 0 byte so the
# WebGPU backend can pass it as a string. sizeof(NAME) counts that 0.
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n    " bytes "${bytes}")
file(WRITE ${OUT} "// Generated from ${IN} (WGSL, Tint). Do not edit.\nconst unsigned char ${NAME}[] = {\n    ${bytes}0x00};\n")
