// headless_raylib.cpp - the three raylib functions the simulation uses, for the
// dedicated server ONLY (don't add it to the game's build).
//
// The game rules use raylib's math (raymath.h, header-only) plus TextFormat
// and TraceLog. Supplying those here means the server doesn't link raylib at
// all, so it runs on a headless Linux VPS with no OpenGL or X11 installed.
#include "raylib.h"
#include <cstdarg>
#include <cstdio>

static int g_logLevel = LOG_WARNING;

void SetTraceLogLevel(int logLevel) { g_logLevel = logLevel; }

void TraceLog(int logLevel, const char *text, ...)
{
    if (logLevel < g_logLevel) return;
    static const char *names[] = { "ALL", "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL", "NONE" };
    fprintf(stderr, "%s: ", logLevel >= 0 && logLevel <= 7 ? names[logLevel] : "LOG");
    va_list args;
    va_start(args, text);
    vfprintf(stderr, text, args);
    va_end(args);
    fprintf(stderr, "\n");
}

// Same contract as raylib's: a small ring of static buffers, so a few results
// can be used at once (e.g. two TextFormat calls in one expression)
const char *TextFormat(const char *text, ...)
{
    static char buffers[4][1024];
    static int  index = 0;
    char *buf = buffers[index];
    index = (index + 1) % 4;
    va_list args;
    va_start(args, text);
    vsnprintf(buf, sizeof buffers[0], text, args);
    va_end(args);
    return buf;
}
