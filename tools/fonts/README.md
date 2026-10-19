# Fonts for the game text

The game asks for **Times New Roman** (also as "Times Roman") and **Arial**, height 20 and 24
(type 18 resources, `src\docs\SPRITE_ANIM.md`). The port uses, in order:

1. the Windows fonts if present (`%WINDIR%\Fonts\times.ttf`, `arial.ttf`): the same ones
   as the original, read from the system and not copied into the project;
2. otherwise these free fonts, with identical metrics:

| File | Replaces | Version | License |
|---|---|---|---|
| `LiberationSerif-Regular.ttf` | Times New Roman | Liberation Fonts 2.1.5 | SIL Open Font License 1.1 (`LICENSE`) |
| `LiberationSans-Regular.ttf` | Arial | Liberation Fonts 2.1.5 | SIL Open Font License 1.1 (`LICENSE`) |

Source: https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
(release 2.1.5 of 2021-09-30), original archive in `tools\_downloads\` (2,385,008 bytes,
SHA-256 `7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0`; the source does not
publish a hash, this is the one computed at download time). SHA-256 of the extracted files:
`LiberationSerif-Regular.ttf` `058ea80864aef09a23f45cbec2bb5400bc3dfbdea01c3f10538a21fcb497fb74`,
`LiberationSans-Regular.ttf` `76d04c18ea243f426b7de1f3ad208e927008f961dc5945e5aad352d0dfde8ee8`.
Downloaded on 2026-10-16 with the project owner's consent.
