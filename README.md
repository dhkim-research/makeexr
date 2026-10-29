# makeexr

Converts a Radiance HDR picture into an OpenEXR file with three channels **X, Y, Z**:
CIE 1931 tristimulus values in **cd/m²**, 32-bit float. Y is the luminance.

mergehdr and Radiance store RGB pictures (and mergehdr also XYZ pictures) in Radiance units,
cd/m² ÷ 179. makeexr multiplies them by 179 and undoes the header `EXPOSURE`, so the EXR holds
absolute values that can be used directly, for example in MATLAB.

## Use

```
makeexr input.hdr > output.exr
makeexr input.hdr output.exr
```

Both forms give the same file. In **Windows PowerShell 5** (the PowerShell that comes with
Windows) use the second form: its `>` re-encodes program output as text and breaks binary files.
In the Command Prompt (cmd) and in PowerShell 7 both forms work.

makeexr.exe is a single file: OpenEXR is built in, and nothing else (Radiance, OpenEXR,
Visual C++ runtime) has to be installed.

## Reading the result in MATLAB

MATLAB R2022b or newer with the Image Processing Toolbox:

```matlab
xyz = exrread("output.exr", Channels=["X","Y","Z"]);   % H x W x 3, single, cd/m^2
L = xyz(:,:,2);                                          % luminance
```

The channel names are X, Y and Z, so `exrread` needs the `Channels` argument
(without it, it looks for R, G and B).

## What is converted

| Header of the .hdr | Treated as |
|---|---|
| `PRIMARIES=` line | RGB with these primaries (Radiance RGB, linear sRGB, XYZ, or any other) |
| `FORMAT=32-bit_rle_xyze` (e.g. from `ra_xyze`) | XYZ already in cd/m² (Radiance's XYZE convention), not multiplied by 179 |
| no `PRIMARIES=` | Radiance RGB (Radiance's default primaries); a note is printed |
| `EXPOSURE=` lines | divided out (their product) |
| `HDRSPACE_COLOR= xyz cd/m2` (set in hdrspace) | XYZ already in cd/m², not multiplied by 179 |

RGB is converted to XYZ with the matrix of the primaries, normalised so that RGB = (1, 1, 1)
is the white point with Y = 1; the Y row is the luminance weights of the colour space
(0.265, 0.670, 0.065 for Radiance RGB). So

```
X, Y, Z = 179 × M · (R, G, B) / EXPOSURE
```

RGBE pixels are decoded as Radiance does: (mantissa + 0.5) · 2^(exponent − 136).
All Radiance picture orientations are read; the EXR is written top row first. `COLORCORR=`
lines are ignored. Pictures that hdrspace marked as raw camera values or as luminance are refused.

The EXR uses ZIP (lossless) compression and 32-bit float, because half floats stop at 65504
and would clip the sun and other bright sources. The header carries a `comments` line describing
the conversion, `units` = `cd/m^2`, and the original Radiance header as `radianceHeader`.

## Building

CMake 3.20 or newer and a C++17 compiler. OpenEXR 3.3.2 (with Imath and libdeflate) is
downloaded and linked statically during the build:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

GitHub Actions builds `makeexr.exe` for 64-bit Windows on every push (`.github/workflows/windows.yml`)
and checks the converted values against an independent calculation.

## Licence

MIT (see `LICENSE`). OpenEXR, Imath and libdeflate are under their own BSD/MIT-style licences.
