# LZMA SDK 26.06 (C 7z decoder only)

The files here are copied unchanged from `C/` of the LZMA SDK 26.06 (`lzma2606.7z`, linked from
<https://www.7-zip.org/sdk.html>): only what decoding a `.7z` archive with LZMA / LZMA2 needs. The
runtime builds them with `Z7_NO_METHODS_FILTERS` (no branch filters but BCJ2, which `7zDec.c` always
links; no PPMd), so `Bra.h` and `Delta.h` are here for `7zDec.c`'s includes only. Used by
`runtime/src/hd_texture_pack.cpp`.

## License (from the SDK's `DOC/lzma-sdk.txt`)

LZMA SDK is written and placed in the public domain by Igor Pavlov.

Some code in LZMA SDK is based on public domain code from another developers:
  1) PPMd var.H (2001): Dmitry Shkarin
  2) SHA-256: Wei Dai (Crypto++ library)

Anyone is free to copy, modify, publish, use, compile, sell, or distribute the
original LZMA SDK code, either in source code form or as a compiled binary, for
any purpose, commercial or non-commercial, and by any means.

LZMA SDK code is compatible with open source licenses, for example, you can
include it to GNU GPL or GNU LGPL code.
