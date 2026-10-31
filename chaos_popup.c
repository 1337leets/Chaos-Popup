/*
 * chaos_popup.c
 *
 * A harmless popup prank for Windows. Error-style popups appear slowly at first,
 * then faster, each with its own sound. They all stay open until the sequence
 * ends, then close together as the program exits. Press Esc at any time to stop it.
 *
 * It only shows windows and plays the sound files that come with Windows. All
 * limits are constants below, and there are no command line options.
 *
 * Build: gcc chaos_popup.c -o chaos_popup.exe -mwindows -luser32 -lwinmm
 */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef POPUP_COUNT
#define POPUP_COUNT 200       /* total number of popups */
#endif
#define POPUP_LIFE_MS 60000   /* backup timeout, the program normally exits first */
#define FIRST_DELAY_MS 500    /* gap between the first two popups */
#define LAST_DELAY_MS 40      /* gap between the last two popups */
#define FINAL_HOLD_MS 3000    /* all popups stay up this long after the last one */
#define MAX_RUN_MS 90000      /* the program always exits by this time */
#define MAX_SOUNDS 24         /* sounds allowed to overlap */
#define SOUND_LIMIT_MS 5000   /* a single sound never runs longer than this */
#define MONITOR_LIMIT 16
#define POLL_MS 10

static const char *TITLES[] = {
    "System Error", "Warning", "Critical Failure", "Attention Required",
    "Unexpected Problem", "Security Alert", "Process Failure",
    "System Notice", "Important", "Alert"
};
#define TITLE_COUNT (sizeof(TITLES) / sizeof(TITLES[0]))

static const char *MESSAGES[] = {
    "Operation failed unexpectedly.",
    "Please close all applications immediately.",
    "Unknown exception occurred in kernel32.dll.",
    "Resource not available. Retry?",
    "Memory integrity compromised.",
    "Do you want to attempt recovery?",
    "Attempting to isolate process.",
    "Press OK to continue.",
    "System will restart in 30 seconds.",
    "Corrupt configuration detected.",
    "Unhandled exception in process.",
    "Access violation at 0xFFFFFFFF.",
    "Stack overflow detected.",
    "Critical service has stopped.",
    "Hardware failure imminent."
};
#define MESSAGE_COUNT (sizeof(MESSAGES) / sizeof(MESSAGES[0]))

static const char *WAV_NAMES[] = {
    "Windows Critical Stop.wav",
    "Windows Error.wav",
    "Windows Exclamation.wav",
    "Windows Foreground.wav",
    "Windows Hardware Fail.wav",
};
#define WAV_NAME_COUNT (sizeof(WAV_NAMES) / sizeof(WAV_NAMES[0]))

typedef struct {
    WAVEFORMATEX *format;
    BYTE *samples;
    DWORD size;
} Sound;

typedef struct {
    char title[64];
    char message[128];
    const Sound *sound;
    RECT monitor;         /* the display this popup appears on */
    double x, y;          /* position on that display, from 0 to 1 */
} Popup;

typedef int (WINAPI *MessageBoxTimeoutFn)(HWND, LPCSTR, LPCSTR, UINT, WORD, DWORD);

static Sound sounds[WAV_NAME_COUNT];
static int sound_count;
static HANDLE sound_slots;
static MessageBoxTimeoutFn show_popup;

static RECT monitors[MONITOR_LIMIT];
static int monitor_count;

static __thread RECT target_monitor;
static __thread double target_x;
static __thread double target_y;

/* Loads a WAV file once and keeps it in memory. The file is only read. */
static void load_sound(const char *path, Sound *out) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(file, NULL), got = 0;
    BYTE *data = (size != INVALID_FILE_SIZE && size > 12) ? malloc(size) : NULL;
    BOOL read_ok = data && ReadFile(file, data, size, &got, NULL) && got == size;
    CloseHandle(file);
    if (!read_ok || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        free(data);
        return;
    }

    BYTE *p = data + 12, *end = data + size;
    while (end - p >= 8) {
        DWORD chunk = *(DWORD *)(p + 4);
        BYTE *body = p + 8;
        if (chunk > (DWORD)(end - body)) break;
        if (memcmp(p, "fmt ", 4) == 0 && chunk >= 16) out->format = (WAVEFORMATEX *)body;
        if (memcmp(p, "data", 4) == 0) { out->samples = body; out->size = chunk; }
        if (chunk + (chunk & 1) > (DWORD)(end - body)) break;
        p = body + chunk + (chunk & 1);
    }
    if (!out->format || !out->samples || !out->size) {
        free(data);
        memset(out, 0, sizeof(*out));
    }
}

/* Each sound opens its own output, so overlapping sounds all get heard. */
static DWORD WINAPI sound_thread(LPVOID param) {
    const Sound *sound = param;
    HANDLE done = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT out = NULL;

    if (done && waveOutOpen(&out, WAVE_MAPPER, sound->format, (DWORD_PTR)done, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        WAVEHDR header = {0};
        header.lpData = (LPSTR)sound->samples;
        header.dwBufferLength = sound->size;
        if (waveOutPrepareHeader(out, &header, sizeof(header)) == MMSYSERR_NOERROR) {
            if (waveOutWrite(out, &header, sizeof(header)) == MMSYSERR_NOERROR) {
                DWORD start = GetTickCount();
                while (!(((volatile WAVEHDR *)&header)->dwFlags & WHDR_DONE) && GetTickCount() - start < SOUND_LIMIT_MS)
                    WaitForSingleObject(done, 100);
            }
            waveOutReset(out);
            waveOutUnprepareHeader(out, &header, sizeof(header));
        }
        waveOutClose(out);
    }
    if (done) CloseHandle(done);
    ReleaseSemaphore(sound_slots, 1, NULL);
    return 0;
}

static void start_sound(const Sound *sound) {
    if (!sound || WaitForSingleObject(sound_slots, 0) != WAIT_OBJECT_0) return;
    HANDLE thread = CreateThread(NULL, 0, sound_thread, (LPVOID)sound, 0, NULL);
    if (thread) CloseHandle(thread);
    else ReleaseSemaphore(sound_slots, 1, NULL);
}

/* Moves a popup to its random position as it opens, using its real size so it can reach every screen edge. */
static LRESULT CALLBACK move_hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_ACTIVATE) {
        HWND window = (HWND)wparam;
        RECT box;
        if (GetWindowRect(window, &box)) {
            int free_w = (target_monitor.right - target_monitor.left) - (box.right - box.left);
            int free_h = (target_monitor.bottom - target_monitor.top) - (box.bottom - box.top);
            int x = target_monitor.left + (free_w > 0 ? (int)(target_x * free_w) : 0);
            int y = target_monitor.top + (free_h > 0 ? (int)(target_y * free_h) : 0);
            SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

static DWORD WINAPI popup_thread(LPVOID param) {
    Popup *popup = param;
    target_monitor = popup->monitor;
    target_x = popup->x;
    target_y = popup->y;

    start_sound(popup->sound);
    HHOOK hook = SetWindowsHookExA(WH_CBT, move_hook, NULL, GetCurrentThreadId());
    show_popup(NULL, popup->message, popup->title, MB_OK | MB_ICONERROR | MB_TOPMOST, 0, POPUP_LIFE_MS);
    if (hook) UnhookWindowsHookEx(hook);

    free(popup);
    return 0;
}

static BOOL CALLBACK add_monitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data) {
    (void)monitor; (void)dc; (void)data;
    if (monitor_count < MONITOR_LIMIT) monitors[monitor_count++] = *rect;
    return TRUE;
}

/* Picks a display at random. Larger displays get more popups, so the spread stays even. */
static RECT pick_monitor(void) {
    double total = 0.;
    for (int i = 0; i < monitor_count; ++i)
        total += (double)(monitors[i].right - monitors[i].left) * (monitors[i].bottom - monitors[i].top);
    double point = total * rand() / (RAND_MAX + 1.);
    for (int i = 0; i < monitor_count; ++i) {
        point -= (double)(monitors[i].right - monitors[i].left) * (monitors[i].bottom - monitors[i].top);
        if (point < 0.) return monitors[i];
    }
    return monitors[0];
}

/* Waits while watching for Esc and the time limit. Returns 0 when it is time to exit. */
static int wait_ms(int ms, DWORD started) {
    DWORD begin = GetTickCount();
    do {
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || GetTickCount() - started >= MAX_RUN_MS) return 0;
        Sleep(POLL_MS);
    } while ((int)(GetTickCount() - begin) < ms);
    return 1;
}

int main(void) {
    /* The timeout is the backup way to close popups, so do not run without it. */
    HMODULE user32 = GetModuleHandleA("user32.dll");
    show_popup = user32 ? (MessageBoxTimeoutFn)(void *)GetProcAddress(user32, "MessageBoxTimeoutA") : NULL;
    if (!show_popup) return 1;

    char media[MAX_PATH], path[MAX_PATH + 64];
    UINT length = GetWindowsDirectoryA(media, sizeof(media));
    for (int i = 0; length && length < sizeof(media) && i < (int)WAV_NAME_COUNT; ++i) {
        Sound sound = {0};
        snprintf(path, sizeof(path), "%s\\Media\\%s", media, WAV_NAMES[i]);
        load_sound(path, &sound);
        if (sound.samples) sounds[sound_count++] = sound;
    }
    sound_slots = CreateSemaphoreA(NULL, MAX_SOUNDS, MAX_SOUNDS, NULL);

    EnumDisplayMonitors(NULL, NULL, add_monitor, 0);
    if (!monitor_count) {
        RECT primary = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        monitors[monitor_count++] = primary;
    }

    DWORD started = GetTickCount();
    srand(started);

    for (int i = 0; i < POPUP_COUNT; ++i) {
        Popup *popup = calloc(1, sizeof(Popup));
        if (popup) {
            snprintf(popup->title, sizeof(popup->title), "%s - %04X", TITLES[rand() % TITLE_COUNT], rand() & 0xFFFF);
            snprintf(popup->message, sizeof(popup->message), "%s\n\nError code: 0x%04X", MESSAGES[rand() % MESSAGE_COUNT], rand() & 0xFFFF);
            popup->sound = (sound_count && sound_slots) ? &sounds[rand() % sound_count] : NULL;
            popup->monitor = pick_monitor();
            popup->x = (double)rand() / RAND_MAX;
            popup->y = (double)rand() / RAND_MAX;

            HANDLE thread = CreateThread(NULL, 0, popup_thread, popup, 0, NULL);
            if (thread) CloseHandle(thread);
            else free(popup);
        }

        /* The gap shrinks smoothly from the first delay to the last one. */
        double progress = POPUP_COUNT > 1 ? (double)i / (POPUP_COUNT - 1) : 1.0;
        int delay = (int)(FIRST_DELAY_MS * pow((double)LAST_DELAY_MS / FIRST_DELAY_MS, progress));
        if (!wait_ms(delay, started)) ExitProcess(0);
    }

    /* Exiting closes every popup that is still open. */
    wait_ms(FINAL_HOLD_MS, started);
    ExitProcess(0);
}
