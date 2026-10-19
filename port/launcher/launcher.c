/*
 * <game>_launcher - small Win32 launcher (no SDL) for <game>.exe (GAME_NAME of game.h).
 * Reads and writes <game>.ini in its own folder (the same as <game>.exe), checks that the necessary
 * files are present and starts the game. File format: port\src\config.h.
 *
 * Compilato da build.bat: gcc -mwindows ... -lcomctl32 -lcomdlg32 -lshell32 -lole32
 */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "game.h"
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum {
	ID_DATA = 100, ID_DATA_BROWSE, ID_RES, ID_SCALE, ID_FULL, ID_FILTER, ID_INTEGER, ID_VSYNC,
	ID_VOL, ID_MUSIC, ID_SF2, ID_SF2_BROWSE, ID_TRACE, ID_STATUS, ID_PLAY, ID_SAVE, ID_CLOSE,
	ID_VOLTXT, ID_MUSTXT
};

static HWND hwnd, c_data, c_res, c_scale, c_full, c_filter, c_integer, c_vsync, c_vol, c_music, c_sf2, c_trace, c_status;
static wchar_t dir[MAX_PATH];     /* launcher folder */
static wchar_t ini[MAX_PATH];     /* <game>.ini */

/* ---- INI minimale (UTF-8) ---- */

static void utf8_to_w(const char *s, wchar_t *out, int cap)
{
	if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap))
		out[0] = 0;
}

static void w_to_utf8(const wchar_t *s, char *out, int cap)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL))
		out[0] = 0;
}

static void ini_get(const char *key, const char *def, char *out, size_t cap)
{
	FILE *f = _wfopen(ini, L"r");
	char line[1024];
	snprintf(out, cap, "%s", def);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == ';' || line[0] == '#' || line[0] == '[' || !eq)
			continue;
		*eq = 0;
		char *e = eq + 1 + strlen(eq + 1);
		while (e > eq + 1 && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
			*--e = 0;
		if (!_stricmp(line, key))
			snprintf(out, cap, "%s", eq + 1);
	}
	fclose(f);
}

static int ini_int(const char *key, int def)
{
	char d[16], v[64];
	snprintf(d, sizeof(d), "%d", def);
	ini_get(key, d, v, sizeof(v));
	return atoi(v);
}

/* ---- controls ---- */

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, (HMENU)(INT_PTR)id,
		GetModuleHandleW(NULL), NULL);
	SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
	return c;
}

static void label(const wchar_t *t, int x, int y, int w)
{
	make(L"STATIC", t, 0, x, y + 3, w, 18, 0);
}

static void set_text(HWND c, const char *utf8)
{
	wchar_t w[1024];
	utf8_to_w(utf8, w, 1024);
	SetWindowTextW(c, w);
}

static void get_text(HWND c, char *out, int cap)
{
	wchar_t w[1024];
	GetWindowTextW(c, w, 1024);
	w_to_utf8(w, out, cap);
}

static int file_exists(const wchar_t *dirp, const wchar_t *name)
{
	wchar_t p[MAX_PATH * 2];
	if (dirp && dirp[0])
		swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dirp, name);
	else
		swprintf(p, MAX_PATH * 2, L"%ls", name);
	return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Check of the necessary files, written to the status line. */
static void check_files(void)
{
	char d[512];
	wchar_t wd[MAX_PATH], msg[1024], sf[MAX_PATH];
	get_text(c_data, d, sizeof(d));
	utf8_to_w(d, wd, MAX_PATH);
	const wchar_t *dd = wd[0] ? wd : dir;
	int bin = file_exists(dd, L"" GAME_START) && file_exists(dd, L"" GAME_MINIGAME);
	get_text(c_sf2, d, sizeof(d));
	utf8_to_w(d, sf, MAX_PATH);
	int sf2 = sf[0] ? file_exists(NULL, sf) : file_exists(dir, L"GeneralUser-GS.sf2");
	swprintf(msg, 1024, L"%ls game files (.BIN)   %ls " GAME_NAME ".exe   %ls SDL2.dll   %ls SoundFont (music)",
		bin ? L"[ok]" : L"[MISSING]", file_exists(dir, L"" GAME_NAME ".exe") ? L"[ok]" : L"[MISSING]",
		file_exists(dir, L"SDL2.dll") ? L"[ok]" : L"[MISSING]", sf2 ? L"[ok]" : L"[missing: no music]");
	SetWindowTextW(c_status, msg);
}

static void load_settings(void)
{
	char v[512];
	ini_get("DataDir", "", v, sizeof(v));
	set_text(c_data, v);
	SendMessageW(c_res, CB_SETCURSEL, ini_int("Resolution", 800) == 640 ? 1 : 0, 0);
	SendMessageW(c_scale, CB_SETCURSEL, ini_int("Scale", 1), 0);
	SendMessageW(c_full, BM_SETCHECK, ini_int("Fullscreen", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
	ini_get("Filter", "nearest", v, sizeof(v));
	SendMessageW(c_filter, CB_SETCURSEL, !_stricmp(v, "linear") ? 1 : 0, 0);
	SendMessageW(c_integer, BM_SETCHECK, ini_int("IntegerScale", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(c_vsync, BM_SETCHECK, ini_int("Vsync", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(c_vol, TBM_SETPOS, TRUE, ini_int("Volume", 100));
	SendMessageW(c_music, TBM_SETPOS, TRUE, ini_int("MusicVolume", 100));
	ini_get("SoundFont", "", v, sizeof(v));
	set_text(c_sf2, v);
	SendMessageW(c_trace, BM_SETCHECK, ini_int("Trace", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
}

static int checked(HWND c)
{
	return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int save_settings(void)
{
	char data[512], sf2[512];
	get_text(c_data, data, sizeof(data));
	get_text(c_sf2, sf2, sizeof(sf2));
	FILE *f = _wfopen(ini, L"w");
	if (!f) {
		MessageBoxW(hwnd, L"Cannot write " GAME_NAME ".ini (protected folder?).", L"" GAME_TITLE, MB_ICONERROR);
		return 0;
	}
	fprintf(f, "; " GAME_NAME ".exe settings (written by " GAME_NAME "_launcher.exe, can be edited by hand).\n");
	fprintf(f, "; The command line (--800, --scale N, --fullscreen, --data, --sf2) takes precedence.\n[Port]\n");
	fprintf(f, "; Resolution: 800 (800x600) or 640 (640x480). The game is drawn at this resolution.\n");
	fprintf(f, "Resolution=%d\n", SendMessageW(c_res, CB_GETCURSEL, 0, 0) == 1 ? 640 : 800);
	fprintf(f, "; Scale: 0 = the largest that fits on the screen, 1..8 = window N times the resolution\n");
	fprintf(f, "Scale=%d\n", (int)SendMessageW(c_scale, CB_GETCURSEL, 0, 0));
	fprintf(f, "Fullscreen=%d\n", checked(c_full));
	fprintf(f, "; Filter: nearest (sharp pixels) or linear (smooth)\n");
	fprintf(f, "Filter=%s\n", SendMessageW(c_filter, CB_GETCURSEL, 0, 0) == 1 ? "linear" : "nearest");
	fprintf(f, "; IntegerScale: 1 = integer multiples only (black bars), 0 = fills the window\n");
	fprintf(f, "IntegerScale=%d\n", checked(c_integer));
	fprintf(f, "Vsync=%d\n", checked(c_vsync));
	fprintf(f, "Volume=%d\n", (int)SendMessageW(c_vol, TBM_GETPOS, 0, 0));
	fprintf(f, "MusicVolume=%d\n", (int)SendMessageW(c_music, TBM_GETPOS, 0, 0));
	fprintf(f, "; Trace=1 writes " GAME_NAME "_trace.txt (diagnostics: useful for reporting a problem)\n");
	fprintf(f, "Trace=%d\n", checked(c_trace));
	fprintf(f, "; DataDir: folder of the .BIN files (empty = same folder as " GAME_NAME ".exe)\n");
	fprintf(f, "DataDir=%s\n", data);
	fprintf(f, "; SoundFont: .sf2 file (empty = GeneralUser-GS.sf2 next to " GAME_NAME ".exe)\n");
	fprintf(f, "SoundFont=%s\n", sf2);
	fclose(f);
	return 1;
}

static void play(void)
{
	wchar_t cmd[MAX_PATH * 2];
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	if (!save_settings())
		return;
	if (!file_exists(dir, L"" GAME_NAME ".exe")) {
		MessageBoxW(hwnd, L"" GAME_NAME ".exe is not in the launcher folder.", L"" GAME_TITLE, MB_ICONERROR);
		return;
	}
	swprintf(cmd, MAX_PATH * 2, L"\"%ls\\" GAME_NAME ".exe\"", dir);
	if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	else
		MessageBoxW(hwnd, L"Cannot start " GAME_NAME ".exe.", L"" GAME_TITLE, MB_ICONERROR);
}

static void browse_dir(void)
{
	BROWSEINFOW bi = { 0 };
	wchar_t path[MAX_PATH];
	bi.hwndOwner = hwnd;
	bi.lpszTitle = L"Folder with the game files (.BIN)";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	LPITEMIDLIST id = SHBrowseForFolderW(&bi);
	if (id && SHGetPathFromIDListW(id, path)) {
		char u[1024];
		w_to_utf8(path, u, sizeof(u));
		set_text(c_data, u);
		check_files();
	}
	CoTaskMemFree(id);
}

static void browse_sf2(void)
{
	OPENFILENAMEW o = { sizeof(o) };
	wchar_t path[MAX_PATH] = L"";
	o.hwndOwner = hwnd;
	o.lpstrFilter = L"SoundFont (*.sf2)\0*.sf2\0All files\0*.*\0";
	o.lpstrFile = path;
	o.nMaxFile = MAX_PATH;
	o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (GetOpenFileNameW(&o)) {
		char u[1024];
		w_to_utf8(path, u, sizeof(u));
		set_text(c_sf2, u);
		check_files();
	}
}

static void update_labels(void)
{
	wchar_t t[32];
	swprintf(t, 32, L"%d%%", (int)SendMessageW(c_vol, TBM_GETPOS, 0, 0));
	SetDlgItemTextW(hwnd, ID_VOLTXT, t);
	swprintf(t, 32, L"%d%%", (int)SendMessageW(c_music, TBM_GETPOS, 0, 0));
	SetDlgItemTextW(hwnd, ID_MUSTXT, t);
}

static void add(HWND combo, const wchar_t *s)
{
	SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
}

static void create_controls(void)
{
	int y = 12;
	label(L"Game files (.BIN):", 12, y, 150);
	c_data = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 170, y, 350, 22, ID_DATA);
	make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 530, y - 1, 80, 24, ID_DATA_BROWSE);
	y += 28;
	label(L"(empty = same folder as " GAME_NAME ".exe)", 170, y - 4, 400);
	y += 20;
	label(L"Game resolution:", 12, y, 150);
	c_res = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 170, y, 160, 120, ID_RES);
	add(c_res, L"800 x 600"); add(c_res, L"640 x 480");
	y += 30;
	label(L"Window size:", 12, y, 150);
	c_scale = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 170, y, 160, 200, ID_SCALE);
	add(c_scale, L"Largest possible"); add(c_scale, L"1x"); add(c_scale, L"2x"); add(c_scale, L"3x");
	add(c_scale, L"4x"); add(c_scale, L"5x"); add(c_scale, L"6x"); add(c_scale, L"7x"); add(c_scale, L"8x");
	c_full = make(L"BUTTON", L"Fullscreen", BS_AUTOCHECKBOX, 350, y, 160, 22, ID_FULL);
	y += 30;
	label(L"Scaling:", 12, y, 150);
	c_filter = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 170, y, 160, 120, ID_FILTER);
	add(c_filter, L"Sharp (pixels)"); add(c_filter, L"Smooth");
	c_integer = make(L"BUTTON", L"Integer multiples only (black bars)", BS_AUTOCHECKBOX, 350, y, 260, 22, ID_INTEGER);
	y += 30;
	c_vsync = make(L"BUTTON", L"Sync with the monitor (vsync)", BS_AUTOCHECKBOX, 170, y, 300, 22, ID_VSYNC);
	y += 32;
	label(L"Master volume:", 12, y, 150);
	c_vol = make(L"msctls_trackbar32", L"", TBS_AUTOTICKS, 170, y - 4, 350, 30, ID_VOL);
	SendMessageW(c_vol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	make(L"STATIC", L"", 0, 530, y + 3, 60, 18, ID_VOLTXT);
	y += 32;
	label(L"Music volume:", 12, y, 150);
	c_music = make(L"msctls_trackbar32", L"", TBS_AUTOTICKS, 170, y - 4, 350, 30, ID_MUSIC);
	SendMessageW(c_music, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
	make(L"STATIC", L"", 0, 530, y + 3, 60, 18, ID_MUSTXT);
	y += 34;
	label(L"SoundFont (music):", 12, y, 150);
	c_sf2 = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 170, y, 350, 22, ID_SF2);
	make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 530, y - 1, 80, 24, ID_SF2_BROWSE);
	y += 32;
	c_trace = make(L"BUTTON", L"Write " GAME_NAME "_trace.txt (diagnostics, slows down a little)", BS_AUTOCHECKBOX, 170, y, 440, 22, ID_TRACE);
	y += 32;
	c_status = make(L"STATIC", L"", 0, 12, y, 600, 36, ID_STATUS);
	y += 44;
	make(L"BUTTON", L"Start the game", BS_DEFPUSHBUTTON, 170, y, 160, 30, ID_PLAY);
	make(L"BUTTON", L"Save", BS_PUSHBUTTON, 340, y, 120, 30, ID_SAVE);
	make(L"BUTTON", L"Close", BS_PUSHBUTTON, 470, y, 120, 30, ID_CLOSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
	case WM_CREATE:
		hwnd = h;
		create_controls();
		load_settings();
		update_labels();
		check_files();
		return 0;
	case WM_HSCROLL:
		update_labels();
		return 0;
	case WM_COMMAND:
		switch (LOWORD(w)) {
		case ID_DATA_BROWSE: browse_dir(); break;
		case ID_SF2_BROWSE: browse_sf2(); break;
		case ID_DATA:
		case ID_SF2:
			if (HIWORD(w) == EN_CHANGE && c_status)
				check_files();
			break;
		case ID_SAVE:
			if (save_settings())
				SetWindowTextW(c_status, L"Settings saved to " GAME_NAME ".ini.");
			break;
		case ID_PLAY: play(); break;
		case ID_CLOSE: DestroyWindow(h); break;
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	(void)prev; (void)cmd;
	INITCOMMONCONTROLSEX cc = { sizeof(cc), ICC_BAR_CLASSES };
	InitCommonControlsEx(&cc);
	CoInitialize(NULL);
	GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *s = wcsrchr(dir, L'\\');
	if (s)
		*s = 0;
	swprintf(ini, MAX_PATH, L"%ls\\" GAME_NAME ".ini", dir);
	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); /* port\res\game.rc (NULL without an icon) */
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"PortLauncher";
	RegisterClassW(&wc);
	RECT r = { 0, 0, 630, 470 };
	AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	HWND w = CreateWindowW(L"PortLauncher", L"" GAME_TITLE " - Launcher", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
	ShowWindow(w, show);
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0)) {
		if (!IsDialogMessageW(w, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	return 0;
}
