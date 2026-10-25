"""Pin C sources to C11 for the native simulator build.

Host GCCs (GCC 15+) default C sources to C23, where `bool` became a keyword.
Some C dependencies (QRCode's qrcode.h: `typedef unsigned char bool;` plus its
own true/false constants) predate that and no longer compile. Pinning C to
`-std=gnu11` only touches C compiles (CFLAGS); C++ keeps the env's
`-std=gnu++2a`. Every C dep in this tree (miniz, pngle, tjpgd, unibreak,
uzlib) is fine on C11.

Appended from a pre-script so it runs before PlatformIO derives the library
builder envs; the pin therefore reaches third-party library compiles too.
"""

Import("env")

env.AppendUnique(CFLAGS=["-std=gnu11"])
