/*
 * ache-capture: records the screen, and any fullscreen game (exclusive fullscreen
 * included), with OBS's engine (libobs), and writes the encoded video to stdout.
 *
 * Copyright (C) 2026 ache
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version. See LICENSE.
 *
 * How it works
 *   A scene with two layers, both fitted to the canvas:
 *     1. monitor_capture: the chosen monitor (what you see when no game is up)
 *     2. game_capture, "any fullscreen application": OBS's signed graphics hook
 *        takes frames straight from the game, so exclusive fullscreen works
 *   libobs renders that scene on the GPU at the canvas size, scales it to the
 *   output size, converts to NV12 and hands the texture to a hardware encoder
 *   (NVENC, AMF or QuickSync; x264 as a last resort). Frames never go through
 *   system memory.
 *
 * Output (stdout): IVF. A 32-byte header, then for each frame a u32 size, a u64
 * timestamp in 1/fps units and the frame (Annex-B, with SPS/PPS before every keyframe).
 * ffmpeg reads it with `-f ivf -i pipe:0 -c:v copy`. libobs's output is constant frame
 * rate: when its renderer misses a frame (the game has the GPU), it encodes the last
 * picture again, so timestamps never jump but the picture repeats. Frames are only
 * missing from the stream (a gap in the timestamps) if the reader falls ~100 s behind.
 *
 * Status (stderr), for ache to read:
 *   [ache-capture] origin_ns N        once: the QueryPerformanceCounter time (ns) of
 *                                     timestamp 0, so ache can place the video exactly
 *   [ache-capture] stats frames=.. rendered=.. lagged=.. skipped=.. dropped=.. queued_kb=..
 *                                     every second, counts since start: frames written,
 *                                     frames libobs rendered, rendered late (repeated in
 *                                     the video), skipped by the encoder, dropped here
 *   [ache-capture] source game|window|screen   what's being recorded, on every change
 *   [ache-capture] encoder ID         the encoder that's running
 *
 * Control: the program runs until stdin closes or reads "q", then stops cleanly.
 * Logs go to stderr.
 *
 * Games that block the hook (Roblox's anti-cheat, for one) give game capture black
 * frames. OBS lists them in win-capture's compatibility.json ("Game Capture is
 * blocked"); while one of those is the foreground window, ache-capture stops hooking
 * and records that window with Windows.Graphics.Capture (window_capture) instead,
 * until the window closes. EXTRA_BLOCKED adds games OBS's list doesn't have yet.
 *
 * Following the game (--follow-game): the canvas should be the game's own size, not
 * the monitor's (a 1920x1080 game on a 5120x1440 screen would otherwise sit small in
 * the middle). libobs can't resize the canvas while encoding, so when a hooked game's
 * size differs from the canvas (or the game is gone and the canvas isn't the
 * screen's) for 0.75 s, it prints "[ache-capture] canvas WxH" and exits with code 10.
 * The caller restarts it with --canvas WxH.
 *
 * Usage
 *   ache-capture --monitor \\.\DISPLAY1 --canvas 5120x1440 --size 4096x1152
 *                --fps 60 --encoder nvenc --codec h264 --kbps 20000 [--adapter 0]
 *                [--screen 5120x1440 --follow-game] [--no-game]
 *
 * Exit codes: 0 stopped, 2 bad arguments, 3 libobs/video setup failed,
 * 4 encoder failed, 5 output failed, 10 restart with the canvas it printed.
 */

#include <obs.h>
#include <obs-output.h>
#include <util/base.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <ctype.h>

#include <windows.h>
#include <timeapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE out_handle;
static obs_encoder_t *encoder;
static uint32_t fps = 60;
static volatile LONG running = 1;
static volatile LONG exit_code = 0;
static volatile LONG frames = 0;
static volatile LONG keyframes = 0;
static volatile LONG origin_said = 0;

/* Frames wait here for the writer thread. The encoder's thread must never block on
 * stdout: a pipe holds a few KB, and while the reader is busy (ffmpeg writing a
 * segment to disk) a blocked encoder makes libobs skip frames, which shows up as a
 * clip that freezes in and out. OBS's own outputs write from their own thread too. */
struct qnode {
	struct qnode *next;
	size_t size;
	uint8_t data[];
};
#define QUEUE_MAX ((size_t)256 << 20) /* about 100 s at 20 Mb/s */
static SRWLOCK qlock = SRWLOCK_INIT;
static CONDITION_VARIABLE qcv = CONDITION_VARIABLE_INIT;
static struct qnode *qhead, *qtail;
static size_t qbytes, qpeak;
static bool qdropping, writer_stop;
static volatile LONG dropped = 0;

/* Games whose anti-cheat blocks the hook, beyond OBS's list (lowercase .exe names). */
static const char *EXTRA_BLOCKED[] = {NULL};

/* Logging ----------------------------------------------------------------- */

static void log_handler(int level, const char *fmt, va_list args, void *param)
{
	(void)param;
	if (level > LOG_INFO)
		return;
	const char *tag = level <= LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : "info";
	fprintf(stderr, "[%s] ", tag);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	fflush(stderr);
}

static void say(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fputs("[ache-capture] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	fflush(stderr);
	va_end(args);
}

/* stdout ------------------------------------------------------------------ */

static bool write_all(const void *data, size_t size)
{
	const uint8_t *p = data;
	while (size) {
		DWORD chunk = size > 0x40000000 ? 0x40000000 : (DWORD)size;
		DWORD done = 0;
		if (!WriteFile(out_handle, p, chunk, &done, NULL) || done == 0)
			return false;
		p += done;
		size -= done;
	}
	return true;
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static bool write_ivf_header(const char fourcc[4], uint32_t w, uint32_t h)
{
	uint8_t hd[32] = {'D', 'K', 'I', 'F'};
	put_u16(hd + 4, 0);  /* version */
	put_u16(hd + 6, 32); /* header size */
	memcpy(hd + 8, fourcc, 4);
	put_u16(hd + 12, (uint16_t)w);
	put_u16(hd + 14, (uint16_t)h);
	put_u32(hd + 16, fps); /* time base denominator */
	put_u32(hd + 20, 1);   /* time base numerator */
	put_u32(hd + 24, 0);   /* frame count: unknown, it's a stream */
	put_u32(hd + 28, 0);
	return write_all(hd, sizeof(hd));
}

/* The output: encoded packets to stdout ------------------------------------ */

struct pipe_output {
	obs_output_t *output;
};

static const char *pipe_name(void *type_data)
{
	(void)type_data;
	return "ache pipe";
}

static void *pipe_create(obs_data_t *settings, obs_output_t *output)
{
	(void)settings;
	struct pipe_output *p = bzalloc(sizeof(*p));
	p->output = output;
	return p;
}

static void pipe_destroy(void *data)
{
	bfree(data);
}

static bool pipe_start(void *data)
{
	struct pipe_output *p = data;
	if (!obs_output_can_begin_data_capture(p->output, 0))
		return false;
	if (!obs_output_initialize_encoders(p->output, 0))
		return false;
	return obs_output_begin_data_capture(p->output, 0);
}

static void pipe_stop(void *data, uint64_t ts)
{
	(void)ts;
	struct pipe_output *p = data;
	obs_output_end_data_capture(p->output);
}

static void pipe_packet(void *data, struct encoder_packet *pkt)
{
	(void)data;
	if (!pkt || pkt->type != OBS_ENCODER_VIDEO || !running)
		return;

	/* SPS/PPS (VPS for HEVC) before every keyframe, so each 1 s piece decodes on its own. */
	uint8_t *extra = NULL;
	size_t extra_size = 0;
	if (pkt->keyframe)
		obs_encoder_get_extra_data(encoder, &extra, &extra_size);

	/* Timestamps in 1/fps units: frame numbers, with gaps where frames were skipped. */
	int64_t pts = pkt->pts;
	if (pkt->timebase_den > 0 && !(pkt->timebase_num == 1 && (uint32_t)pkt->timebase_den == fps))
		pts = (int64_t)((double)pkt->pts * pkt->timebase_num * fps / pkt->timebase_den + 0.5);

	/* Where timestamp 0 sits on the system clock: libobs stamps each packet with the
	 * QueryPerformanceCounter time of its frame (dts_usec, from the frame the encoder
	 * started on), the same clock ache's audio uses. */
	if (!origin_said && InterlockedExchange(&origin_said, 1) == 0) {
		int64_t at_ns = pkt->dts_usec * 1000 - (int64_t)((double)pts * 1e9 / fps + 0.5);
		say("origin_ns %lld", (long long)at_ns);
	}

	size_t size = 12 + extra_size + pkt->size;
	struct qnode *n = malloc(sizeof(*n) + size);
	if (!n)
		return;
	n->next = NULL;
	n->size = size;
	put_u32(n->data, (uint32_t)(extra_size + pkt->size));
	put_u64(n->data + 4, (uint64_t)(pts < 0 ? 0 : pts));
	if (extra_size)
		memcpy(n->data + 12, extra, extra_size);
	memcpy(n->data + 12 + extra_size, pkt->data, pkt->size);

	AcquireSRWLockExclusive(&qlock);
	/* Reader far behind: drop frames until the next keyframe rather than grow forever. */
	if (qdropping && pkt->keyframe && qbytes + size <= QUEUE_MAX / 2)
		qdropping = false;
	if (!qdropping && qbytes + size > QUEUE_MAX)
		qdropping = true;
	if (qdropping) {
		ReleaseSRWLockExclusive(&qlock);
		free(n);
		InterlockedIncrement(&dropped);
		return;
	}
	if (qtail)
		qtail->next = n;
	else
		qhead = n;
	qtail = n;
	qbytes += size;
	if (qbytes > qpeak)
		qpeak = qbytes;
	ReleaseSRWLockExclusive(&qlock);
	WakeConditionVariable(&qcv);
	InterlockedIncrement(&frames);
	if (pkt->keyframe)
		InterlockedIncrement(&keyframes);
}

/* Drains the queue into stdout. Ends when told to stop and the queue is empty, or
 * when the reader goes away. */
static DWORD WINAPI writer_thread(LPVOID param)
{
	(void)param;
	for (;;) {
		AcquireSRWLockExclusive(&qlock);
		while (!qhead && !writer_stop)
			SleepConditionVariableSRW(&qcv, &qlock, 100, 0);
		struct qnode *n = qhead;
		if (n) {
			qhead = n->next;
			if (!qhead)
				qtail = NULL;
			qbytes -= n->size;
		}
		ReleaseSRWLockExclusive(&qlock);
		if (!n)
			break; /* stopping, and nothing left */
		bool ok = write_all(n->data, n->size);
		free(n);
		if (!ok) {
			/* The reader went away: stop. */
			InterlockedExchange(&running, 0);
			break;
		}
	}
	return 0;
}

static struct obs_output_info pipe_info = {
	.id = "ache_pipe",
	.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
	.get_name = pipe_name,
	.create = pipe_create,
	.destroy = pipe_destroy,
	.start = pipe_start,
	.stop = pipe_stop,
	.encoded_packet = pipe_packet,
};

static void on_output_stop(void *param, calldata_t *cd)
{
	(void)param;
	long long code = calldata_int(cd, "code");
	if (code != OBS_OUTPUT_SUCCESS) {
		obs_output_t *out = calldata_ptr(cd, "output");
		const char *err = out ? obs_output_get_last_error(out) : NULL;
		say("output stopped with code %lld%s%s", code, err ? ": " : "", err ? err : "");
		InterlockedExchange(&exit_code, 4);
	}
	InterlockedExchange(&running, 0);
}

/* stdin: "q" or EOF stops -------------------------------------------------- */

static DWORD WINAPI stdin_thread(LPVOID param)
{
	(void)param;
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	char buf[64];
	DWORD n = 0;
	while (running) {
		if (!ReadFile(in, buf, sizeof(buf), &n, NULL) || n == 0)
			break;
		if (memchr(buf, 'q', n))
			break;
	}
	InterlockedExchange(&running, 0);
	return 0;
}

/* Setup -------------------------------------------------------------------- */

static bool parse_size(const char *s, uint32_t *w, uint32_t *h)
{
	unsigned a = 0, b = 0;
	if (sscanf(s, "%ux%u", &a, &b) != 2 || !a || !b || a > 16384 || b > 16384)
		return false;
	*w = a;
	*h = b;
	return true;
}

static bool load_module(const char *name)
{
	char bin[MAX_PATH], data[MAX_PATH];
	snprintf(bin, sizeof(bin), "../../core/%s/%s.dll", name, name);
	snprintf(data, sizeof(data), "../../core/%s/data", name);
	obs_module_t *mod = NULL;
	int r = obs_open_module(&mod, bin, data);
	if (r != MODULE_SUCCESS) {
		say("couldn't open module %s (%d)", name, r);
		return false;
	}
	if (!obs_init_module(mod)) {
		say("couldn't start module %s", name);
		return false;
	}
	return true;
}

static void fit(obs_sceneitem_t *item, uint32_t w, uint32_t h)
{
	struct vec2 pos, bounds;
	vec2_set(&pos, 0.0f, 0.0f);
	vec2_set(&bounds, (float)w, (float)h);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
}

/* ache's encoder names ("h264_nvenc", or just "nvenc") to an OBS module and encoder id. */
static bool pick_encoder(const char *enc, const char *codec, const char **module, const char **id,
			 const char **fourcc)
{
	bool hevc = strcmp(codec, "hevc") == 0;
	*fourcc = hevc ? "HEVC" : "H264";
	if (strstr(enc, "nvenc")) {
		*module = "obs-nvenc";
		*id = hevc ? "obs_nvenc_hevc_tex" : "obs_nvenc_h264_tex";
	} else if (strstr(enc, "amf")) {
		*module = "obs-ffmpeg";
		*id = hevc ? "h265_texture_amf" : "h264_texture_amf";
	} else if (strstr(enc, "qsv")) {
		*module = "obs-qsv11";
		*id = hevc ? "obs_qsv11_hevc" : "obs_qsv11_v2";
	} else if (strstr(enc, "x264")) {
		*module = "obs-x264";
		*id = "obs_x264";
		*fourcc = "H264";
	} else {
		return false;
	}
	return true;
}

static obs_data_t *encoder_settings(const char *id, uint32_t kbps)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", kbps);
	obs_data_set_int(s, "max_bitrate", kbps);
	obs_data_set_int(s, "keyint_sec", 1); /* a keyframe every second: ache cuts 1 s pieces */
	obs_data_set_int(s, "bf", 0);         /* no B-frames: low latency, pts == dts */
	obs_data_set_int(s, "bframes", 0);
	if (strstr(id, "nvenc")) {
		obs_data_set_string(s, "rate_control", "cbr");
		obs_data_set_string(s, "preset", "p4");
		obs_data_set_string(s, "tune", "ll");
		obs_data_set_string(s, "multipass", "disabled");
		obs_data_set_bool(s, "lookahead", false);
		obs_data_set_bool(s, "repeat_headers", true);
	} else if (strstr(id, "amf")) {
		obs_data_set_string(s, "preset", "speed");
	} else if (strstr(id, "qsv")) {
		obs_data_set_string(s, "target_usage", "TU4");
		obs_data_set_int(s, "async_depth", 4);
	} else if (strstr(id, "x264")) {
		obs_data_set_string(s, "preset", "veryfast");
		obs_data_set_string(s, "tune", "zerolatency");
	}
	return s;
}

/* Blocked games -------------------------------------------------------------- */

static char **blocked;
static size_t blocked_count;

static void add_blocked(const char *exe)
{
	if (!exe || !*exe)
		return;
	char *lower = bstrdup(exe);
	for (char *c = lower; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	for (size_t i = 0; i < blocked_count; i++)
		if (!strcmp(blocked[i], lower)) {
			bfree(lower);
			return;
		}
	blocked = brealloc(blocked, sizeof(char *) * (blocked_count + 1));
	blocked[blocked_count++] = lower;
}

/* OBS's own list of games that block Game Capture, from win-capture's data. */
static void load_blocked(void)
{
	obs_data_t *d = obs_data_create_from_json_file("../../core/win-capture/data/compatibility.json");
	obs_data_array_t *entries = d ? obs_data_get_array(d, "entries") : NULL;
	size_t n = entries ? obs_data_array_count(entries) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(entries, i);
		const char *key = obs_data_get_string(e, "translation_key");
		if (obs_data_get_bool(e, "game_capture") && strstr(key, "GameCapture.Blocked"))
			add_blocked(obs_data_get_string(e, "executable"));
		obs_data_release(e);
	}
	obs_data_array_release(entries);
	obs_data_release(d);
	for (const char **x = EXTRA_BLOCKED; *x; x++)
		add_blocked(*x);
	say("%zu games use window capture instead of the game hook", blocked_count);
}

/* The .exe name of a window's process, lowercase ("" if unknown). */
static void window_exe(HWND w, char *out, size_t size)
{
	out[0] = 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
	if (!p)
		return;
	wchar_t path[MAX_PATH];
	DWORD len = MAX_PATH;
	if (QueryFullProcessImageNameW(p, 0, path, &len)) {
		const wchar_t *base = wcsrchr(path, L'\\');
		WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1, out, (int)size, NULL, NULL);
		for (char *c = out; *c; c++)
			*c = (char)tolower((unsigned char)*c);
	}
	CloseHandle(p);
}

static bool is_blocked(const char *exe)
{
	for (size_t i = 0; i < blocked_count; i++)
		if (!strcmp(blocked[i], exe))
			return true;
	return false;
}

/* window_capture's "window" setting: title:class:exe, with ':' and '#' escaped. */
static void append_escaped(struct dstr *s, const wchar_t *w)
{
	char *u = NULL;
	os_wcs_to_utf8_ptr(w, 0, &u);
	for (const char *c = u ? u : ""; *c; c++) {
		if (*c == ':')
			dstr_cat(s, "#3A");
		else if (*c == '#')
			dstr_cat(s, "#22");
		else
			dstr_cat_ch(s, *c);
	}
	bfree(u);
}

static void point_window_capture(obs_source_t *src, HWND w, const char *exe)
{
	wchar_t title[256] = L"", cls[256] = L"", wexe[MAX_PATH] = L"";
	GetWindowTextW(w, title, 256);
	GetClassNameW(w, cls, 256);
	MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, MAX_PATH);
	struct dstr s = {0};
	append_escaped(&s, title);
	dstr_cat_ch(&s, ':');
	append_escaped(&s, cls);
	dstr_cat_ch(&s, ':');
	append_escaped(&s, wexe);
	obs_data_t *ws = obs_data_create();
	obs_data_set_string(ws, "window", s.array);
	obs_data_set_int(ws, "priority", 2); /* match by executable first */
	obs_data_set_int(ws, "method", 2);   /* Windows.Graphics.Capture */
	obs_data_set_bool(ws, "cursor", true);
	obs_data_set_bool(ws, "client_area", true);
	obs_source_update(src, ws);
	obs_data_release(ws);
	dstr_free(&s);
}

static void set_game_mode(obs_source_t *game_src, const char *mode)
{
	obs_data_t *gs = obs_data_create();
	obs_data_set_string(gs, "capture_mode", mode);
	obs_source_update(game_src, gs);
	obs_data_release(gs);
}

static void usage(void)
{
	fputs("usage: ache-capture --monitor \\\\.\\DISPLAY1 --canvas WxH --size WxH --fps N\n"
	      "                    --encoder nvenc|amf|qsv|x264 [--codec h264|hevc] [--kbps N]\n"
	      "                    [--adapter N] [--screen WxH --follow-game] [--no-game]\n",
	      stderr);
}

int main(int argc, char **argv)
{
	const char *monitor = "";
	const char *enc_name = "nvenc";
	const char *codec = "h264";
	uint32_t canvas_w = 0, canvas_h = 0, out_w = 0, out_h = 0, kbps = 20000, adapter = 0;
	uint32_t screen_w = 0, screen_h = 0;
	bool game = true, follow = false;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--no-game")) {
			game = false;
			continue;
		}
		if (!strcmp(a, "--follow-game")) {
			follow = true;
			continue;
		}
		if (!v) {
			usage();
			return 2;
		}
		i++;
		if (!strcmp(a, "--monitor"))
			monitor = v;
		else if (!strcmp(a, "--canvas") && parse_size(v, &canvas_w, &canvas_h))
			;
		else if (!strcmp(a, "--size") && parse_size(v, &out_w, &out_h))
			;
		else if (!strcmp(a, "--screen") && parse_size(v, &screen_w, &screen_h))
			;
		else if (!strcmp(a, "--fps"))
			fps = (uint32_t)atoi(v);
		else if (!strcmp(a, "--encoder"))
			enc_name = v;
		else if (!strcmp(a, "--codec"))
			codec = v;
		else if (!strcmp(a, "--kbps"))
			kbps = (uint32_t)atoi(v);
		else if (!strcmp(a, "--adapter"))
			adapter = (uint32_t)atoi(v);
		else {
			usage();
			return 2;
		}
	}
	if (!canvas_w || fps < 1 || fps > 360 || kbps < 500) {
		usage();
		return 2;
	}
	if (!out_w) {
		out_w = canvas_w;
		out_h = canvas_h;
	}
	if (!screen_w) {
		screen_w = canvas_w;
		screen_h = canvas_h;
	}
	out_w &= ~1u;
	out_h &= ~1u;

	const char *enc_module, *enc_id, *fourcc;
	if (!pick_encoder(enc_name, codec, &enc_module, &enc_id, &fourcc)) {
		say("unknown encoder %s", enc_name);
		return 2;
	}

	/* libobs finds its data and plugins relative to the working directory, as OBS does. */
	wchar_t self[MAX_PATH];
	if (GetModuleFileNameW(NULL, self, MAX_PATH)) {
		wchar_t *slash = wcsrchr(self, L'\\');
		if (slash) {
			*slash = 0;
			SetCurrentDirectoryW(self);
		}
	}
	out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	/* As OBS does: a busy game shouldn't starve capture of CPU time. (libobs-d3d11
	 * also raises this process's GPU scheduling priority.) */
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	/* Windows 11 puts windowless background processes on EcoQoS (efficiency cores, lower
	 * clocks) and coarse timers; libobs's renderer then misses frame deadlines. Opt out,
	 * as a foreground app would be. */
	PROCESS_POWER_THROTTLING_STATE pt = {.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
					     .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
							    PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
					     .StateMask = 0};
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
	timeBeginPeriod(1);
	base_set_log_handler(log_handler, NULL);

	if (!obs_startup("en-US", NULL, NULL)) {
		say("libobs didn't start");
		return 3;
	}
	struct obs_audio_info oai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	obs_reset_audio(&oai);
	struct obs_video_info ovi = {
		.graphics_module = "libobs-d3d11",
		.fps_num = fps,
		.fps_den = 1,
		.base_width = canvas_w,
		.base_height = canvas_h,
		.output_width = out_w,
		.output_height = out_h,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = adapter,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		say("video setup failed (%d)", vr);
		obs_shutdown();
		return 3;
	}

	if (!load_module("win-capture") || !load_module(enc_module)) {
		obs_shutdown();
		return 3;
	}
	obs_register_output(&pipe_info);
	obs_post_load_modules();

	/* The scene: the monitor, with any fullscreen game on top. */
	obs_scene_t *scene = obs_scene_create("ache");
	obs_data_t *ms = obs_data_create();
	obs_data_set_string(ms, "monitor_id", monitor);
	obs_data_set_int(ms, "method", 0); /* auto: Desktop Duplication or Windows.Graphics.Capture */
	obs_data_set_bool(ms, "capture_cursor", true);
	obs_source_t *screen = obs_source_create("monitor_capture", "screen", ms, NULL);
	obs_data_release(ms);
	obs_sceneitem_t *screen_item = NULL;
	if (screen) {
		screen_item = obs_scene_add(scene, screen);
		fit(screen_item, canvas_w, canvas_h);
	}

	obs_source_t *game_src = NULL, *window_src = NULL;
	obs_sceneitem_t *window_item = NULL;
	HWND blocked_window = NULL;
	uint64_t last_fg_check = 0;
	if (game) {
		obs_data_t *gs = obs_data_create();
		obs_data_set_string(gs, "capture_mode", "any_fullscreen");
		obs_data_set_bool(gs, "capture_cursor", true);
		obs_data_set_bool(gs, "allow_transparency", false);
		obs_data_set_bool(gs, "anti_cheat_hook", true);
		/* The hook copies the game's frame on every Present it allows. Unlimited, that's
		 * twice our frame rate (game-capture.c halves the interval); a game drawing 1000
		 * frames a second then pays for 120 copies a second it doesn't need, inside its
		 * own frame time. Limited, it copies at our frame rate. */
		obs_data_set_bool(gs, "limit_framerate", true);
		obs_data_set_bool(gs, "capture_overlays", false);
		game_src = obs_source_create("game_capture", "game", gs, NULL);
		obs_data_release(gs);
		if (game_src)
			fit(obs_scene_add(scene, game_src), canvas_w, canvas_h);

		/* For games that block the hook: their window, off until one is in front. */
		load_blocked();
		obs_data_t *ws = obs_data_create();
		obs_data_set_int(ws, "method", 2);
		window_src = obs_source_create("window_capture", "window", ws, NULL);
		obs_data_release(ws);
		if (window_src) {
			window_item = obs_scene_add(scene, window_src);
			fit(window_item, canvas_w, canvas_h);
			obs_sceneitem_set_visible(window_item, false);
			obs_source_set_enabled(window_src, false);
		}
	}
	obs_set_output_source(0, obs_scene_get_source(scene));

	obs_data_t *es = encoder_settings(enc_id, kbps);
	encoder = obs_video_encoder_create(enc_id, "video", es, NULL);
	obs_data_release(es);
	if (!encoder) {
		say("encoder %s isn't available", enc_id);
		obs_shutdown();
		return 4;
	}
	obs_encoder_set_video(encoder, obs_get_video());

	obs_output_t *output = obs_output_create("ache_pipe", "pipe", NULL, NULL);
	obs_output_set_video_encoder(output, encoder);
	signal_handler_connect(obs_output_get_signal_handler(output), "stop", on_output_stop, NULL);

	if (!write_ivf_header(fourcc, out_w, out_h)) {
		say("stdout is closed");
		obs_shutdown();
		return 5;
	}
	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		say("couldn't start: %s", err ? err : "the encoder didn't open");
		obs_shutdown();
		return 4;
	}
	say("recording %ux%u -> %ux%u at %u fps with %s, %u kbps%s", canvas_w, canvas_h, out_w, out_h, fps, enc_id,
	    kbps, game ? ", game capture on" : "");

	say("encoder %s", enc_id);
	say("source screen");

	HANDLE writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
	CreateThread(NULL, 0, stdin_thread, NULL, 0, NULL);
	uint64_t last_report = os_gettime_ns(), last_stats = last_report;
	uint64_t mismatch_since = 0;
	const char *source = "screen";
	while (running) {
		Sleep(50);
		uint64_t now = os_gettime_ns();
		/* A game that blocks the hook in front: record its window instead, until it closes. */
		if (window_src && now - last_fg_check > 500000000ULL) {
			last_fg_check = now;
			if (blocked_window && !IsWindow(blocked_window)) {
				say("blocked game closed: back to the game hook");
				blocked_window = NULL;
				obs_sceneitem_set_visible(window_item, false);
				obs_source_set_enabled(window_src, false);
				set_game_mode(game_src, "any_fullscreen");
			}
			HWND fg = GetForegroundWindow();
			char exe[MAX_PATH];
			if (fg && fg != blocked_window) {
				window_exe(fg, exe, sizeof(exe));
				if (*exe && is_blocked(exe)) {
					say("%s blocks the game hook: recording its window instead", exe);
					blocked_window = fg;
					set_game_mode(game_src, "hotkey"); /* stop hooking it */
					point_window_capture(window_src, fg, exe);
					obs_source_set_enabled(window_src, true);
					obs_sceneitem_set_visible(window_item, true);
				}
			}
		}
		if (follow && game_src) {
			/* The size the canvas should be: the game's (hooked, or its window), else the screen's. */
			obs_source_t *g = blocked_window ? window_src : game_src;
			uint32_t gw = obs_source_get_width(g), gh = obs_source_get_height(g);
			/* While a game is hooked it covers the canvas: stop duplicating the desktop
			 * underneath (a 5120x1440 screen every frame is real GPU work). */
			bool hooked = gw && gh;
			if (screen_item && obs_sceneitem_visible(screen_item) == hooked) {
				obs_sceneitem_set_visible(screen_item, !hooked);
				obs_source_set_enabled(screen, !hooked);
			}
			uint32_t want_w = gw && gh ? gw : screen_w, want_h = gw && gh ? gh : screen_h;
			if (want_w != canvas_w || want_h != canvas_h) {
				if (!mismatch_since)
					mismatch_since = now;
				else if (now - mismatch_since > 750000000ULL) {
					say("canvas %ux%u", want_w, want_h);
					InterlockedExchange(&exit_code, 10);
					break;
				}
			} else {
				mismatch_since = 0;
			}
		}
		/* What's on the canvas: a hooked game, a blocked game's window, or the screen. */
		const char *now_source = blocked_window ? "window"
					 : (game_src && obs_source_get_width(game_src) && obs_source_get_height(game_src)) ? "game"
					 : "screen";
		if (strcmp(now_source, source)) {
			source = now_source;
			say("source %s", source);
		}
		if (now - last_stats >= 1000000000ULL) {
			last_stats = now;
			video_t *v = obs_get_video();
			AcquireSRWLockShared(&qlock);
			size_t queued = qbytes;
			ReleaseSRWLockShared(&qlock);
			say("stats frames=%ld rendered=%u lagged=%u skipped=%u dropped=%ld queued_kb=%zu", frames, obs_get_total_frames(),
			    obs_get_lagged_frames(), video_output_get_skipped_frames(v), dropped, queued >> 10);
		}
		if (now - last_report > 10000000000ULL) {
			last_report = now;
			video_t *v = obs_get_video();
			AcquireSRWLockShared(&qlock);
			size_t queued = qbytes, peak = qpeak;
			ReleaseSRWLockShared(&qlock);
			say("frames %ld, keyframes %ld, skipped %u of %u, lagged %u, queued %zu KB (peak %zu KB), dropped %ld",
			    frames, keyframes, video_output_get_skipped_frames(v), video_output_get_total_frames(v),
			    obs_get_lagged_frames(), queued >> 10, peak >> 10, dropped);
		}
	}

	obs_output_force_stop(output);
	/* Let what's queued reach the reader (briefly), then stop the writer. */
	AcquireSRWLockExclusive(&qlock);
	writer_stop = true;
	ReleaseSRWLockExclusive(&qlock);
	WakeConditionVariable(&qcv);
	if (writer)
		WaitForSingleObject(writer, 2000);
	obs_output_release(output);
	obs_encoder_release(encoder);
	obs_set_output_source(0, NULL);
	if (window_src)
		obs_source_release(window_src);
	if (game_src)
		obs_source_release(game_src);
	if (screen)
		obs_source_release(screen);
	obs_scene_release(scene);
	obs_shutdown();
	return (int)exit_code;
}
