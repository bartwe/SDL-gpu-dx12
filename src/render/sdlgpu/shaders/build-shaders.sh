#!/usr/bin/env bash

set -e

spirv_bundle="spir-v.h"

rm -f "$spirv_bundle"

for i in *.vert *.frag; do
    spv="$i.spv"
    metal="$i.metal"
    hlsl="$i.hlsl"
    glslangValidator "$i" -V -o "$spv" --quiet
    spirv-cross "$spv" --msl --output "$metal"
    spirv-cross "$spv" --hlsl --shader-model 50 --hlsl-enable-compat --output "$hlsl"
    xxd -i "$spv" | perl -w -p -e 's/\Aunsigned /const unsigned /;' > "$spv.h"
    echo "#include \"$spv.h\"" >> "$spirv_bundle"

    # TODO compile shaders for non-Vulkan backends
done

