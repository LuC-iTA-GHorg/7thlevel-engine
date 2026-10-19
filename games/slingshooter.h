/*
 * Game profile: Slingshooter (Jungle). The only game-specific input of the engine: build.bat
 * copies this file to build\game.h and every source includes it from there.
 *
 * Startup works like the original (TOPSY FUN_1000_0756): the engine takes the name of its own
 * EXE, changes the extension to .BIN and opens that library. GAME_EXE is the EXE the CD starts.
 * The 7THLEVEL.INI section is string 0xD2 of GAME_EXE_RES (LoadString in FUN_1000_05E4);
 * GAME_INI_SECTION is only used when that EXE is missing.
 *
 * Only #define lines: the file is also read by windres (port\res\*.rc), by build.bat (findstr)
 * and by the PowerShell scripts (regex on '#define NAME "value"').
 */
#ifndef GAME_H
#define GAME_H

#define GAME_TITLE "Slingshooter"                        /* window and resource title */
#define GAME_NAME "slingshooter"                         /* port files: slingshooter.exe, slingshooter.ini, slingshooter_launcher.exe */
#define GAME_EXE "T&PSHTPC.EXE"                          /* original EXE started by the CD */
#define GAME_EXE_RES "T&PSHTPC.EXE"                      /* EXE with the cursors, the icon and string 0xD2 */
#define GAME_START "T&PSHTPC.BIN"                        /* GAME_EXE with the .BIN extension: first library */
#define GAME_MINIGAME "JUNGSHOT.BIN"                     /* checked by the launcher together with GAME_START */
#define GAME_VIEWER "JUNGSHOT.BIN"                       /* archive opened by the resource viewer */
#define GAME_INI_SECTION "Pumbaa And Timon's Jungle Arcade" /* fallback when GAME_EXE_RES is missing */
#define GAME_INI_NO_UNESCAPE 1                           /* Jungle EXE: opcode 3B keeps escapes (1008:AC00) */

#endif
