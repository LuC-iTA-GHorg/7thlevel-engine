# Third-party components of the port

| File | Project | Version | License | Source | Verification |
|---|---|---|---|---|---|
| `tsf.h` | TinySoundFont (Bernhard Schelling) | v0.9, commit `853a0a171759f1ddba0de1442133a75912bbeffa` (`main` branch) | MIT (text in the header of `tsf.h`, lines 22–40) | https://raw.githubusercontent.com/schellingb/TinySoundFont/853a0a171759f1ddba0de1442133a75912bbeffa/tsf.h | git blob sha `a81f25d5ca2e210720d646dec2dbfaeb119acb09`, 92,719 bytes |
| `stb_truetype.h` | stb (Sean Barrett) | v1.26, commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (last change to the file, 2024-07-15) | MIT or public domain, at your choice (text at the end of the file) | https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h | git blob sha `90a5c2e2b3fe563c98585294ca7a949876aec94c` (same as GitHub), SHA-256 `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`, 199,192 bytes |

Downloaded on 2026-10-16 with the project owner's consent. Not modified:
the implementation is compiled from `port\src\tsf_impl.c` (`build.bat`, step 2, no warnings).

The SoundFont used by TinySoundFont is described in `tools\soundfont\README.md`.

`stb_truetype.h` (downloaded on 2026-10-16 with the owner's consent) is not modified:
the implementation is compiled from `port\src\text_impl.c`. The fonts are in `tools\fonts\README.md`.
