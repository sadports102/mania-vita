/* =====================================================================
 *  mania vita!  -  main.c
 *  A rhythm game for PS Vita that plays osu!mania beatmaps (1K - 5K).
 *  Fan-made project, not affiliated with osu! or ppy Pty Ltd.
 *
 *  C + SDL2 / SDL2_mixer / SDL2_ttf / SDL2_image / zlib / SceAvPlayer
 *
 *  Data folder:  ux0:data/osumania/      (PC testing: ./data/)
 *     font.ttf             optional (a copy is built into the VPK)
 *     songs/               .osz files (auto-extracted) or song folders
 *     skin/  or  skins/<name>/   optional
 *     hit.wav, miss.wav    optional hitsounds (.ogg works too)
 *     settings.txt, scores.txt   created automatically
 *
 *  Maps: 1K, 2K, 3K, 4K and 5K.  mp4 (H.264) video backgrounds
 *  (.avi / .flv videos need a converted .mp4 with the same name).
 *  Song select:  X play, Square mods, Circle sort/search,
 *                Triangle settings, Select scores, Start local/global
 *  Settings:     L/R Settings / Profile tab, Start rescan songs
 *  Mods: EZ NF HR SD PF HD FI FL MR RD AT CM  (health bar + Failed screen)
 * ===================================================================*/
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#ifdef __vita__
#include <psp2/power.h>
#define DATA_DIR "ux0:data/osumania/"
int _newlib_heap_size_user = 128 * 1024 * 1024;   /* more RAM for images/songs */
#else
#define DATA_DIR "data/"
#endif

#define HIT_ERROR_BAR 1     /* 1 = hit error meter on by default, 0 = off */

#define SCREEN_W 960
#define SCREEN_H 544
#define MAX_KEYS 5
#define KEYMODES 5          /* 1K, 2K, 3K, 4K, 5K */
#define AUDIO_CHUNK 1024    /* raise to 2048 if music crackles */
#define STAR_MAX 10.0f      /* highest star rating shown */

/* ================= Globals ================= */
static SDL_Renderer *ren;
static TTF_Font *font_small, *font_mid, *font_big;   /* font_mid = 24pt (Settings) */
static Uint32 frame_no;

static const SDL_Color C_WHITE  = {255, 255, 255, 255};
static const SDL_Color C_GREY   = {170, 170, 190, 255};
static const SDL_Color C_DARK   = { 20,  20,  30, 255};
static const SDL_Color C_YELLOW = {255, 210,  90, 255};

/* ================= Utilities ================= */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* lane type like osu!: 1 = white, 2 = blue, 3 = special (middle) */
static int col_type(int keys, int c) {
    static const unsigned char t[KEYMODES][MAX_KEYS] = {
        { 3 },                  /* 1K */
        { 1, 1 },               /* 2K */
        { 1, 3, 1 },            /* 3K */
        { 1, 2, 2, 1 },         /* 4K */
        { 1, 2, 3, 2, 1 },      /* 5K */
    };
    if (keys < 1 || keys > KEYMODES || c < 0 || c >= keys) return 1;
    return t[keys - 1][c];
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
}

static const char *val_of(const char *line, const char *key) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) return NULL;
    line += n;
    while (*line == ' ') line++;
    return line;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(ren, &rc);
}

/* ================= Text (with cache) ================= */
#define TEXT_CACHE 192
typedef struct {
    TTF_Font *font; Uint32 rgba; char s[128];
    SDL_Texture *tex; int w, h; Uint32 used;
} TextEntry;
static TextEntry tcache[TEXT_CACHE];

/* the font in ux0:data/osumania/ is used first, then the copy built into the VPK */
static TTF_Font *open_font(int size) {
    TTF_Font *f = TTF_OpenFont(DATA_DIR "font.ttf", size);
#ifdef __vita__
    if (!f) f = TTF_OpenFont("app0:font.ttf", size);
#else
    if (!f) f = TTF_OpenFont("font.ttf", size);
#endif
    return f;
}

static SDL_Texture *make_text(TTF_Font *f, const char *s, SDL_Color c, int *w, int *h) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, surf);
    *w = surf->w; *h = surf->h;
    SDL_FreeSurface(surf);
    return t;
}

static SDL_Texture *text_tex(TTF_Font *f, const char *s, SDL_Color c, int *w, int *h, int *temp) {
    *temp = 0;
    if (!f || !s || !*s) return NULL;
    if (strlen(s) >= sizeof tcache[0].s) { *temp = 1; return make_text(f, s, c, w, h); }

    Uint32 rgba = ((Uint32)c.r << 24) | ((Uint32)c.g << 16) | ((Uint32)c.b << 8) | c.a;
    int slot = 0;
    for (int i = 0; i < TEXT_CACHE; i++) {
        TextEntry *e = &tcache[i];
        if (e->tex && e->font == f && e->rgba == rgba && !strcmp(e->s, s)) {
            e->used = frame_no; *w = e->w; *h = e->h;
            return e->tex;
        }
        if (!e->tex) { if (tcache[slot].tex) slot = i; }
        else if (tcache[slot].tex && e->used < tcache[slot].used) slot = i;
    }
    TextEntry *e = &tcache[slot];
    if (e->tex) SDL_DestroyTexture(e->tex);
    e->tex = make_text(f, s, c, &e->w, &e->h);
    if (!e->tex) return NULL;
    e->font = f; e->rgba = rgba; e->used = frame_no;
    strcpy(e->s, s);
    *w = e->w; *h = e->h;
    return e->tex;
}

/* align: 0 = left, 1 = centre, 2 = right */
static int draw_text(TTF_Font *f, const char *s, int x, int y, SDL_Color c, int align) {
    int w = 0, h = 0, temp = 0;
    SDL_Texture *t = text_tex(f, s, c, &w, &h, &temp);
    if (!t) return 0;
    SDL_Rect dst = { align == 1 ? x - w / 2 : align == 2 ? x - w : x, y, w, h };
    SDL_RenderCopy(ren, t, NULL, &dst);
    if (temp) SDL_DestroyTexture(t);
    return w;
}

static void draw_text_scaled(TTF_Font *f, const char *s, int cx, int cy, SDL_Color c, int scale) {
    int w = 0, h = 0, temp = 0;
    SDL_Texture *t = text_tex(f, s, c, &w, &h, &temp);
    if (!t) return;
    SDL_Rect dst = { cx - w * scale / 2, cy - h * scale / 2, w * scale, h * scale };
    SDL_RenderCopy(ren, t, NULL, &dst);
    if (temp) SDL_DestroyTexture(t);
}

static void fit_text(TTF_Font *f, const char *src, int maxw, char *out, size_t n) {
    snprintf(out, n, "%s", src);
    int w = 0;
    if (!f || TTF_SizeUTF8(f, out, &w, NULL) || w <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        if (len + 4 > n) continue;
        memcpy(out + len, "...", 4);
        if (!TTF_SizeUTF8(f, out, &w, NULL) && w <= maxw) return;
    }
}

/* ================= Fast text for numbers (score, combo, accuracy) ================= */
#define GLYPH_SETS 6
typedef struct {
    TTF_Font *font; Uint32 rgba;
    SDL_Texture *t[96]; int w[96]; char ok[96];
} GlyphSet;
static GlyphSet gsets[GLYPH_SETS];

static GlyphSet *glyph_set(TTF_Font *f, SDL_Color c) {
    Uint32 rgba = ((Uint32)c.r << 24) | ((Uint32)c.g << 16) | ((Uint32)c.b << 8) | c.a;
    for (int i = 0; i < GLYPH_SETS; i++)
        if (gsets[i].font == f && gsets[i].rgba == rgba) return &gsets[i];
    for (int i = 0; i < GLYPH_SETS; i++)
        if (!gsets[i].font) { gsets[i].font = f; gsets[i].rgba = rgba; return &gsets[i]; }
    return NULL;
}

static int glyph_get(GlyphSet *g, int ch, SDL_Color c) {
    int i = ch - 32;
    if (!g->ok[i]) {
        char s[2] = { (char)ch, 0 };
        int w = 0, h = 0;
        g->t[i] = make_text(g->font, s, c, &w, &h);
        if (!g->t[i]) TTF_SizeUTF8(g->font, s, &w, NULL);
        g->w[i] = w;
        g->ok[i] = 1;
    }
    return i;
}

static int draw_fast(TTF_Font *f, const char *s, int x, int y, SDL_Color c, int align) {
    GlyphSet *g = f ? glyph_set(f, c) : NULL;
    if (!g) return draw_text(f, s, x, y, c, align);
    int total = 0;
    for (const char *p = s; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) return draw_text(f, s, x, y, c, align);
        total += g->w[glyph_get(g, ch, c)];
    }
    int h = TTF_FontHeight(f);
    int px = align == 1 ? x - total / 2 : align == 2 ? x - total : x;
    for (const char *p = s; *p; p++) {
        int i = (unsigned char)*p - 32;
        if (g->t[i]) {
            SDL_Rect r = { px, y, g->w[i], h };
            SDL_RenderCopy(ren, g->t[i], NULL, &r);
        }
        px += g->w[i];
    }
    return total;
}

static void glyph_warm(TTF_Font *f, SDL_Color c) {
    GlyphSet *g = f ? glyph_set(f, c) : NULL;
    if (!g) return;
    for (const char *p = "0123456789,.%x+- "; *p; p++) glyph_get(g, (unsigned char)*p, c);
}

static void draw_loading(const char *msg) {
    SDL_SetRenderDrawColor(ren, 12, 12, 20, 255);
    SDL_RenderClear(ren);
    draw_text(font_big, msg, SCREEN_W / 2, SCREEN_H / 2 - 20, C_WHITE, 1);
    if (!font_small) fill(0, 0, 40, 40, 255, 0, 0, 255);   /* red square = no font found */
    SDL_RenderPresent(ren);
}

/* ================= Settings values ================= */
static int opt_speed = 24, opt_offset = 0, opt_volume = 80;
static int opt_skin_layout = 1, opt_lane_w = 80, opt_hit_y = 470, opt_hud = 1;
static int opt_delay = 1500, opt_rear = 1, opt_filter = 0;
static int opt_show_bg = 1, opt_bg_dim = 70, opt_preview = 1;
static int opt_health_bar = 1;      /* show the health bar while playing */
static int opt_hit_error = HIT_ERROR_BAR;   /* hit error meter while playing */
static int opt_touch = 1;           /* front touchscreen: tap lanes / scroll song list */
static int opt_video = 1;           /* mp4 video backgrounds */
static int opt_sfx_volume = 60;     /* hitsound volume 0-100 (separate from music) */
static int opt_judge_left = 0;      /* 1 = combo + judgements left of the playfield */
static int opt_note_color = 0;      /* 0 = skin's own colours */
static int opt_lb_global = 0;       /* song select scores: 0 = local, 1 = global */
static int opt_ingame_lb = 2;       /* in-game leaderboard: 0 = off, 1 = local, 2 = global */
static int opt_sort = 0;            /* song list order */
static char search_text[128] = "";  /* song search (not saved) */
static char opt_skin[128] = "";

/* key filter on song select (0 is also what old "4K + 5K" settings load as) */
#define FILTER_COUNT 8
static const char *filter_names[FILTER_COUNT] = {
    "All keys", "4K only", "5K only", "4K + 5K", "1K only", "2K only", "3K only", "1K - 3K"
};

static int filter_allows(int f, int keys) {
    switch (f) {
    case 1: return keys == 4;
    case 2: return keys == 5;
    case 3: return keys == 4 || keys == 5;
    case 4: return keys == 1;
    case 5: return keys == 2;
    case 6: return keys == 3;
    case 7: return keys >= 1 && keys <= 3;
    default: return 1;
    }
}

/* ================= Mods (Square on song select) =================
 *  Grid order in the Mods menu = enum order (4 per row).
 *  Score multipliers multiply together. RD / AT / CM scores aren't saved.
 *  AT and CM are never saved in settings.txt (off at every start).        */
enum { M_EZ, M_NF, M_HR, M_SD, M_PF, M_HD, M_FI, M_FL, M_MR, M_RD, M_AT, M_CM, MOD_COUNT };
#define MODBIT(m)    (1u << (m))
#define MODS_ALL     ((1u << MOD_COUNT) - 1u)
#define MODS_AUTO    (MODBIT(M_AT) | MODBIT(M_CM))        /* the game plays for you */
#define MODS_NOFAIL  (MODBIT(M_NF) | MODS_AUTO)           /* health can't fail you  */
#define MODS_UNSAVED (MODBIT(M_RD) | MODS_AUTO)           /* score isn't saved      */

static unsigned opt_mods = 0;

static const char *mod_short[MOD_COUNT] = {
    "EZ", "NF", "HR", "SD", "PF", "HD", "FI", "FL", "MR", "RD", "AT", "CM"
};
static const char *mod_long[MOD_COUNT] = {
    "Easy", "No Fail", "Hard Rock", "Sudden Death", "Perfect", "Hidden",
    "Fade In", "Flashlight", "Mirror", "Random", "Autoplay", "Cinema"
};
static const char *mod_desc[MOD_COUNT] = {
    "Wider timing windows and you lose less health.",
    "You can't fail when your health runs out.",
    "Tighter timing windows and you lose more health.",
    "Missing a single note fails the song.",
    "Anything below a 300 fails the song.",
    "Notes fade out sooner as your combo grows.",
    "Notes appear later as your combo grows.",
    "The lit area shrinks as your combo grows.",
    "The lanes are flipped left to right.",
    "The lanes are shuffled. Scores aren't saved.",
    "Watch a perfect play. Scores aren't saved.",
    "Autoplay with no playfield or HUD. Scores aren't saved.",
};
static const float mod_mult[MOD_COUNT] = {
    0.5f, 0.5f, 1.08f, 1.0f, 1.0f, 1.08f, 1.08f, 1.08f, 1.0f, 1.0f, 1.0f, 1.0f
};
/* mods that can't be on at the same time */
static const unsigned char mod_pairs[][2] = {
    {M_EZ, M_HR}, {M_SD, M_PF}, {M_NF, M_SD}, {M_NF, M_PF},
    {M_HD, M_FI}, {M_HD, M_FL}, {M_FI, M_FL}, {M_MR, M_RD},
    {M_AT, M_CM}, {M_AT, M_NF}, {M_AT, M_SD}, {M_AT, M_PF},
    {M_CM, M_NF}, {M_CM, M_SD}, {M_CM, M_PF},
};

static int mod_on(unsigned mods, int m) { return (int)((mods >> m) & 1u); }

static unsigned mod_clash_mask(int m) {
    unsigned mask = 0;
    for (size_t i = 0; i < sizeof mod_pairs / sizeof mod_pairs[0]; i++) {
        if (mod_pairs[i][0] == m) mask |= MODBIT(mod_pairs[i][1]);
        if (mod_pairs[i][1] == m) mask |= MODBIT(mod_pairs[i][0]);
    }
    return mask;
}

/* turns a mod on (switching off the ones it clashes with) or off */
static void mod_toggle(int m) {
    if (mod_on(opt_mods, m)) opt_mods &= ~MODBIT(m);
    else opt_mods = (opt_mods & ~mod_clash_mask(m)) | MODBIT(m);
}

/* removes unknown bits and clashing mods (earlier mods win) */
static unsigned mods_clean(unsigned mods) {
    mods &= MODS_ALL;
    for (int m = 0; m < MOD_COUNT; m++)
        if (mod_on(mods, m)) mods &= ~mod_clash_mask(m);
    return mods;
}

static float mods_multiplier(unsigned mods) {
    float x = 1.0f;
    for (int m = 0; m < MOD_COUNT; m++) if (mod_on(mods, m)) x *= mod_mult[m];
    return x;
}

/* "HR HD FL"  (empty string when no mods) */
static void mods_string(unsigned mods, char *out, size_t n) {
    size_t len = 0;
    if (n) out[0] = 0;
    for (int m = 0; m < MOD_COUNT; m++) {
        if (!mod_on(mods, m) || len + 4 >= n) continue;
        len += (size_t)snprintf(out + len, n - len, "%s%s", len ? " " : "", mod_short[m]);
    }
}

/* ================= Button bindings ([keys - 1][lane], -1 = none) ================= */
static int binds[KEYMODES][MAX_KEYS];
static const int default_binds[KEYMODES][MAX_KEYS] = {
    { SDL_CONTROLLER_BUTTON_A, -1, -1, -1, -1 },                                     /* 1K */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_B, -1, -1, -1 },        /* 2K */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_UP,
      SDL_CONTROLLER_BUTTON_B, -1, -1 },                                             /* 3K */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_UP,
      SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_B, -1 },                        /* 4K */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_UP,
      SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_B },   /* 5K */
};
/* keyboard keys (PC testing) */
static const SDL_Keycode kb_keys[KEYMODES][MAX_KEYS] = {
    { SDLK_SPACE, 0, 0, 0, 0 },
    { SDLK_f, SDLK_j, 0, 0, 0 },
    { SDLK_f, SDLK_SPACE, SDLK_j, 0, 0 },
    { SDLK_d, SDLK_f, SDLK_j, SDLK_k, 0 },
    { SDLK_d, SDLK_f, SDLK_SPACE, SDLK_j, SDLK_k },
};

static const char *button_name(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "Cross";
    case SDL_CONTROLLER_BUTTON_B: return "Circle";
    case SDL_CONTROLLER_BUTTON_X: return "Square";
    case SDL_CONTROLLER_BUTTON_Y: return "Triangle";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Right";
    case SDL_CONTROLLER_BUTTON_BACK: return "Select";
    case SDL_CONTROLLER_BUTTON_START: return "Start";
    default: return "(none)";
    }
}

/* ================= Beatmaps ================= */
typedef struct {
    char folder[256];
    char file[192];
    char title[128], artist[128], version[128], creator[64];
    char audio[160], bg[160];
    char video[160];            /* video background file ("" = none) */
    int  video_ofs;             /* when the video starts (ms, can be negative) */
    int keys, preview_ms, note_count;
    float od, hp, stars;
    int bpm, length_ms;
    long added;
    char tags[256];
} MapInfo;

static MapInfo *maps = NULL;
static int map_count = 0, map_cap = 0, maps_skipped = 0;

static float stars_shown(float s) {
    return s < 0.01f ? 0.01f : s > STAR_MAX ? STAR_MAX : s;
}

/* case-insensitive "does text contain word" */
static int ci_has(const char *text, const char *word, size_t wl) {
    for (; *text; text++)
        if (!strncasecmp(text, word, wl)) return 1;
    return 0;
}

/* every word typed must appear in the title, artist, mapper, difficulty name or tags */
static int map_matches_search(const MapInfo *m) {
    const char *p = search_text;
    while (*p) {
        while (*p == ' ') p++;
        size_t wl = strcspn(p, " ");
        if (!wl) break;
        if (!ci_has(m->title, p, wl) && !ci_has(m->artist, p, wl) && !ci_has(m->creator, p, wl) &&
            !ci_has(m->version, p, wl) && !ci_has(m->tags, p, wl)) return 0;
        p += wl;
    }
    return 1;
}

static int map_passes_filter(const MapInfo *m) {
    if (!filter_allows(opt_filter, m->keys)) return 0;
    return map_matches_search(m);
}

/* ================= Star rating (osu!mania strain formula) ================= */
typedef struct { int t, end, col; } SRNote;
static SRNote *sr_buf = NULL;
static int sr_n = 0, sr_cap = 0;

#define SR_STEP      400.0
#define SR_IND_DECAY 0.125
#define SR_ALL_DECAY 0.30
#define SR_WEIGHT    0.9
#define SR_SCALE     0.018

static void sr_push(int t, int end, int col) {
    if (sr_n == sr_cap) {
        int nc = sr_cap ? sr_cap * 2 : 1024;
        SRNote *p = realloc(sr_buf, nc * sizeof *p);
        if (!p) return;
        sr_buf = p; sr_cap = nc;
    }
    sr_buf[sr_n].t = t; sr_buf[sr_n].end = end; sr_buf[sr_n].col = col;
    sr_n++;
}

static int cmp_sr(const void *a, const void *b) {
    const SRNote *A = a, *B = b;
    if (A->t != B->t) return A->t - B->t;
    return A->col - B->col;
}

static int cmp_desc(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static float calc_stars(int keys) {
    if (sr_n == 0 || keys < 1 || keys > MAX_KEYS) return 0.0f;
    qsort(sr_buf, sr_n, sizeof *sr_buf, cmp_sr);

    int last = sr_buf[sr_n - 1].t;
    int nsec = (last > 0 ? last : 0) / (int)SR_STEP + 4;
    double *peaks = malloc(nsec * sizeof *peaks);
    if (!peaks) return 0.0f;
    int np = 0;

    double ind[MAX_KEYS] = {0}, overall = 1.0;
    int held[MAX_KEYS] = {0};
    double prev_ind = 0.0, prev_all = 0.0, interval_end = SR_STEP, peak = 0.0;
    int prev_t = 0, have_prev = 0;

    for (int i = 0; i < sr_n; i++) {
        const SRNote *n = &sr_buf[i];

        /* close finished 400 ms sections */
        while (n->t > interval_end) {
            if (np < nsec) peaks[np++] = peak;
            if (have_prev) {
                double el = (interval_end - prev_t) / 1000.0;
                peak = prev_ind * pow(SR_IND_DECAY, el) + prev_all * pow(SR_ALL_DECAY, el);
            } else peak = 0.0;
            interval_end += SR_STEP;
        }

        if (i > 0) {
            double el = (n->t - sr_buf[i - 1].t) / 1000.0;
            double di = pow(SR_IND_DECAY, el), da = pow(SR_ALL_DECAY, el);
            double hold_factor = 1.0, hold_add = 0.0;
            for (int c = 0; c < keys; c++) {
                if (n->t < held[c] && n->end > held[c]) hold_add = 1.0;
                if (n->end == held[c]) hold_add = 0.0;
                if (held[c] > n->end) hold_factor = 1.25;
                ind[c] *= di;
            }
            ind[n->col] += 2.0 * hold_factor;
            overall = overall * da + (1.0 + hold_add) * hold_factor;
        }
        held[n->col] = n->end;

        double strain = ind[n->col] + overall;
        if (strain > peak) peak = strain;
        prev_ind = ind[n->col]; prev_all = overall; prev_t = n->t; have_prev = 1;
    }
    if (np < nsec) peaks[np++] = peak;

    qsort(peaks, np, sizeof *peaks, cmp_desc);
    double diff = 0.0, w = 1.0;
    for (int i = 0; i < np; i++) { diff += peaks[i] * w; w *= SR_WEIGHT; }
    free(peaks);
    return (float)(diff * SR_SCALE);
}

static void parse_bg_event(const char *line, char *out, size_t n) {
    if (out[0] || strncmp(line, "0,0,", 4) != 0) return;
    const char *p = line + 4, *end;
    if (*p == '"') { p++; end = strchr(p, '"'); }
    else           end = strchr(p, ',');
    if (!end) end = p + strlen(p);
    size_t len = (size_t)(end - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
}

/* [Events] video line:  Video,<offset>,"file.mp4"   or   1,<offset>,"file.mp4"
   Any video file is kept here; video_open uses a same-name .mp4 for .avi / .flv. */
static void parse_video_event(const char *line, MapInfo *m) {
    if (m->video[0]) return;
    const char *p;
    if      (!strncasecmp(line, "Video,", 6)) p = line + 6;
    else if (!strncmp(line, "1,", 2))         p = line + 2;
    else return;

    int ofs = atoi(p);
    p = strchr(p, ',');
    if (!p) return;
    p++;
    const char *end;
    if (*p == '"') { p++; end = strchr(p, '"'); }
    else           end = strchr(p, ',');
    if (!end) end = p + strlen(p);
    size_t len = (size_t)(end - p);
    if (len < 5 || len >= sizeof m->video) return;

    memcpy(m->video, p, len);
    m->video[len] = 0;
    for (char *c = m->video; *c; c++) if (*c == '\\') *c = '/';
    m->video_ofs = ofs;
}

static int read_osu_info(const char *folder, const char *file, MapInfo *m) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", folder, file);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    memset(m, 0, sizeof *m);
    snprintf(m->folder, sizeof m->folder, "%s", folder);
    snprintf(m->file, sizeof m->file, "%s", file);
    m->preview_ms = -1;
    m->od = 5.0f;
    m->hp = 5.0f;
    { struct stat st; if (stat(path, &st) == 0) m->added = (long)st.st_mtime; }
    sr_n = 0;

    int mode = 0, sec = 0;
    float cs = 0;
    char line[1024];
    const char *v;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || (line[0] == '/' && line[1] == '/')) continue;
        if (line[0] == '[') {
            sec = !strcmp(line, "[General]")    ? 1 :
                  !strcmp(line, "[Metadata]")   ? 2 :
                  !strcmp(line, "[Difficulty]") ? 3 :
                  !strcmp(line, "[Events]")     ? 4 :
                  !strcmp(line, "[HitObjects]") ? 5 :
                  !strcmp(line, "[TimingPoints]") ? 6 : 0;
            continue;
        }
        switch (sec) {
        case 1:
            if      ((v = val_of(line, "AudioFilename:"))) snprintf(m->audio, sizeof m->audio, "%s", v);
            else if ((v = val_of(line, "PreviewTime:")))   m->preview_ms = atoi(v);
            else if ((v = val_of(line, "Mode:")))          mode = atoi(v);
            break;
        case 2:
            if      ((v = val_of(line, "Title:")))   snprintf(m->title,   sizeof m->title,   "%s", v);
            else if ((v = val_of(line, "Artist:")))  snprintf(m->artist,  sizeof m->artist,  "%s", v);
            else if ((v = val_of(line, "Version:"))) snprintf(m->version, sizeof m->version, "%s", v);
            else if ((v = val_of(line, "Creator:"))) snprintf(m->creator, sizeof m->creator, "%s", v);
            else if ((v = val_of(line, "Tags:")))    snprintf(m->tags, sizeof m->tags, "%s", v);
            break;
        case 3:
            if      ((v = val_of(line, "CircleSize:")))        cs = (float)atof(v);
            else if ((v = val_of(line, "OverallDifficulty:"))) m->od = (float)atof(v);
            else if ((v = val_of(line, "HPDrainRate:")))       m->hp = (float)atof(v);
            break;
        case 4:
            parse_bg_event(line, m->bg, sizeof m->bg);
            parse_video_event(line, m);
            break;
        case 5: {
            m->note_count++;
            int x, y, t, type, k = (int)(cs + 0.5f);
            if (mode == 3 && k >= 1 && k <= MAX_KEYS &&
                sscanf(line, "%d,%d,%d,%d", &x, &y, &t, &type) == 4) {
                int end = t;
                if (type & 128) {
                    const char *p = line;
                    for (int q = 0; q < 5 && p; q++) { p = strchr(p, ','); if (p) p++; }
                    if (p) end = atoi(p);
                    if (end < t) end = t;
                }
                sr_push(t, end, clampi(x * k / 512, 0, k - 1));
            }
            break;
        }
        case 6: {
            double t, bl;
            if (!m->bpm && sscanf(line, "%lf,%lf", &t, &bl) == 2 && bl > 0)
                m->bpm = (int)(60000.0 / bl + 0.5);
            break;
        }
        }
    }
    fclose(f);

    if (m->hp < 0.0f)  m->hp = 0.0f;
    if (m->hp > 10.0f) m->hp = 10.0f;
    m->keys = (int)(cs + 0.5f);
    if (mode != 3) return 0;
    if (m->keys < 1 || m->keys > MAX_KEYS) return 0;      /* 1K - 5K */
    if (!m->title[0]) snprintf(m->title, sizeof m->title, "%s", file);
    m->stars = calc_stars(m->keys);
    m->length_ms = sr_n ? sr_buf[sr_n - 1].t : 0;
    return 1;
}

/* ================= Automatic .osz extraction ================= */
#define OSZ_BUF (64 * 1024)
static unsigned char osz_in[OSZ_BUF], osz_out[OSZ_BUF];

static Uint16 osz_rd16(const unsigned char *p) { return (Uint16)(p[0] | (p[1] << 8)); }
static Uint32 osz_rd32(const unsigned char *p) {
    return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
}

static void osz_mkdirs(const char *path) {          /* make every folder above a file */
    char tmp[768];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/' && p[-1] != ':') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
}

static int osz_clean_name(char *s) {                /* fix slashes, block bad names */
    for (char *p = s; *p; p++) {
        if (*p == '\\') *p = '/';
        else if (strchr(":*?\"<>|", *p)) *p = '_';
    }
    if (s[0] == '/' || !strcmp(s, "..") || !strncmp(s, "../", 3) || strstr(s, "/../")) return 0;
    return 1;
}

static int osz_write_entry(FILE *zf, long off, int method, Uint32 csize,
                           Uint32 want_crc, const char *out) {
    if (fseek(zf, off, SEEK_SET) != 0) return 0;
    osz_mkdirs(out);
    FILE *of = fopen(out, "wb");
    if (!of) return 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    Uint32 left = csize;
    int ok = 1;

    if (method == 0) {                                 /* stored */
        while (ok && left > 0) {
            size_t n = left > OSZ_BUF ? OSZ_BUF : left;
            if (fread(osz_in, 1, n, zf) != n || fwrite(osz_in, 1, n, of) != n) ok = 0;
            else { crc = crc32(crc, osz_in, (uInt)n); left -= (Uint32)n; }
        }
    } else if (method == 8) {                          /* deflate */
        z_stream zs;
        memset(&zs, 0, sizeof zs);
        int ret = inflateInit2(&zs, -MAX_WBITS);
        if (ret != Z_OK) ok = 0;
        else {
            while (ok && ret != Z_STREAM_END) {
                if (zs.avail_in == 0 && left > 0) {
                    size_t n = left > OSZ_BUF ? OSZ_BUF : left;
                    if (fread(osz_in, 1, n, zf) != n) { ok = 0; break; }
                    left -= (Uint32)n;
                    zs.next_in = osz_in; zs.avail_in = (uInt)n;
                }
                zs.next_out = osz_out; zs.avail_out = OSZ_BUF;
                ret = inflate(&zs, Z_NO_FLUSH);
                size_t got = OSZ_BUF - zs.avail_out;
                if (got) {
                    crc = crc32(crc, osz_out, (uInt)got);
                    if (fwrite(osz_out, 1, got, of) != got) ok = 0;
                }
                if (ret != Z_OK && ret != Z_STREAM_END) ok = 0;
            }
            inflateEnd(&zs);
        }
    } else ok = 0;                                     /* unknown compression */

    fclose(of);
    if (ok && (Uint32)crc != want_crc) ok = 0;
    if (!ok) remove(out);
    return ok;
}

static int osz_extract(const char *zip, const char *dir) {
    FILE *zf = fopen(zip, "rb");
    if (!zf) return 0;
    int ok = 0;
    unsigned char *tail = NULL, *cd = NULL;

    fseek(zf, 0, SEEK_END);
    long size = ftell(zf);
    long tn = size < 65557 ? size : 65557;
    if (tn < 22 || !(tail = malloc(tn))) goto done;
    fseek(zf, size - tn, SEEK_SET);
    if (fread(tail, 1, tn, zf) != (size_t)tn) goto done;

    long e = -1;
    for (long i = tn - 22; i >= 0; i--)
        if (osz_rd32(tail + i) == 0x06054b50) { e = i; break; }
    if (e < 0) goto done;

    int count = osz_rd16(tail + e + 10);
    Uint32 cd_size = osz_rd32(tail + e + 12), cd_off = osz_rd32(tail + e + 16);
    if (!(cd = malloc(cd_size ? cd_size : 1))) goto done;
    if (fseek(zf, (long)cd_off, SEEK_SET) != 0 || fread(cd, 1, cd_size, zf) != cd_size) goto done;

    ok = 1;
    Uint32 pos = 0;
    for (int i = 0; i < count && ok; i++) {
        if (pos + 46 > cd_size || osz_rd32(cd + pos) != 0x02014b50) { ok = 0; break; }
        int    method = osz_rd16(cd + pos + 10);
        Uint32 crc    = osz_rd32(cd + pos + 16);
        Uint32 csize  = osz_rd32(cd + pos + 20);
        int    nlen   = osz_rd16(cd + pos + 28);
        int    elen   = osz_rd16(cd + pos + 30);
        int    clen   = osz_rd16(cd + pos + 32);
        Uint32 loff   = osz_rd32(cd + pos + 42);
        if (pos + 46 + nlen > cd_size) { ok = 0; break; }

        char name[384];
        int n = nlen < (int)sizeof name - 1 ? nlen : (int)sizeof name - 1;
        memcpy(name, cd + pos + 46, n);
        name[n] = 0;
        pos += 46 + nlen + elen + clen;
        if (!n || name[n - 1] == '/' || name[n - 1] == '\\') continue;   /* folder */
        if (!osz_clean_name(name)) continue;

        unsigned char lh[30];
        if (fseek(zf, (long)loff, SEEK_SET) != 0 || fread(lh, 1, 30, zf) != 30 ||
            osz_rd32(lh) != 0x04034b50) { ok = 0; break; }
        long data = (long)loff + 30 + osz_rd16(lh + 26) + osz_rd16(lh + 28);

        char out[768];
        snprintf(out, sizeof out, "%s/%s", dir, name);
        if (!osz_write_entry(zf, data, method, csize, crc, out)) ok = 0;
    }
done:
    free(tail); free(cd); fclose(zf);
    return ok;
}

/* Extracts every .osz in songs/ into its own folder, then deletes the .osz.
   Broken files are renamed to .osz.bad so they aren't retried every start. */
static void osz_extract_all(void) {
    const char *songs = DATA_DIR "songs";
    DIR *d = opendir(songs);
    if (!d) return;

    char (*list)[256] = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || L >= 256 || strcasecmp(e->d_name + L - 4, ".osz") != 0) continue;
        if (count == cap) {
            int nc = cap ? cap * 2 : 16;
            void *p = realloc(list, nc * sizeof *list);
            if (!p) break;
            list = p; cap = nc;
        }
        snprintf(list[count++], 256, "%s", e->d_name);
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        char msg[64], zip[512], dir[512];
        snprintf(msg, sizeof msg, "Extracting songs %d/%d...", i + 1, count);
        draw_loading(msg);
        snprintf(zip, sizeof zip, "%s/%s", songs, list[i]);
        snprintf(dir, sizeof dir, "%s", zip);
        dir[strlen(dir) - 4] = 0;                    /* remove ".osz" */
        mkdir(dir, 0777);
        if (osz_extract(zip, dir)) remove(zip);
        else {
            char bad[520];
            snprintf(bad, sizeof bad, "%s.bad", zip);
            rename(zip, bad);
        }
    }
    free(list);
    if (count > 0) draw_loading("Loading songs...");
}

static void scan_songs(void) {
    osz_extract_all();
    free(maps);
    maps = NULL; map_count = map_cap = 0; maps_skipped = 0;

    DIR *d = opendir(DATA_DIR "songs");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char folder[256];
        snprintf(folder, sizeof folder, "%ssongs/%s", DATA_DIR, e->d_name);
        if (!is_dir(folder)) continue;

        DIR *d2 = opendir(folder);
        if (!d2) continue;
        struct dirent *e2;
        while ((e2 = readdir(d2))) {
            size_t L = strlen(e2->d_name);
            if (L < 5 || strcasecmp(e2->d_name + L - 4, ".osu") != 0) continue;
            MapInfo m;
            if (!read_osu_info(folder, e2->d_name, &m)) { maps_skipped++; continue; }
            if (map_count == map_cap) {
                int nc = map_cap ? map_cap * 2 : 128;
                MapInfo *p = realloc(maps, nc * sizeof *p);
                if (!p) break;
                maps = p; map_cap = nc;
            }
            maps[map_count++] = m;
        }
        closedir(d2);
    }
    closedir(d);
}

/* ================= Grouped song select ================= */
typedef struct { int first, count, last_diff; } SongGroup;

static int       *g_order = NULL;
static int        g_order_count = 0;
static SongGroup *g_groups = NULL;
static int        g_group_count = 0;
static int        g_sel_group = 0, g_sel_diff = 0;
static float      g_scroll = 0.0f;

static int selected_map_index(void) {
    if (g_group_count == 0) return -1;
    return g_order[g_groups[g_sel_group].first + g_sel_diff];
}

/* moves the song select cursor to a map; returns 0 if the map is hidden by the filter */
static int select_map(int idx) {
    for (int gi = 0; gi < g_group_count; gi++)
        for (int d = 0; d < g_groups[gi].count; d++)
            if (g_order[g_groups[gi].first + d] == idx) {
                g_sel_group = gi; g_sel_diff = d;
                return 1;
            }
    return 0;
}

static int cmp_map_order(const void *a, const void *b) {
    const MapInfo *A = &maps[*(const int *)a];
    const MapInfo *B = &maps[*(const int *)b];
    int c = strcasecmp(A->folder, B->folder);
    if (c) return c;
    if (A->keys != B->keys) return A->keys - B->keys;
    if (A->stars != B->stars) return A->stars < B->stars ? -1 : 1;
    return A->note_count - B->note_count;
}

/* ================= Song list sorting ================= */
#define SORT_COUNT 7
static const char *sort_names[SORT_COUNT] = {
    "Title", "Artist", "Creator", "Stars", "BPM", "Date added", "Length"
};

/* value for a whole song (its highest difficulty) */
static double group_value(const SongGroup *g) {
    double v = 0;
    for (int d = 0; d < g->count; d++) {
        const MapInfo *m = &maps[g_order[g->first + d]];
        double x = opt_sort == 3 ? m->stars : opt_sort == 4 ? m->bpm :
                   opt_sort == 5 ? (double)m->added : (double)m->length_ms;
        if (d == 0 || x > v) v = x;
    }
    return v;
}

static int cmp_groups(const void *a, const void *b) {
    const SongGroup *A = a, *B = b;
    const MapInfo *ma = &maps[g_order[A->first]], *mb = &maps[g_order[B->first]];
    int c = 0;
    switch (opt_sort) {
    case 0: c = strcasecmp(ma->title, mb->title); break;
    case 1: c = strcasecmp(ma->artist, mb->artist); break;
    case 2: c = strcasecmp(ma->creator, mb->creator); break;
    default: {
        double x = group_value(A), y = group_value(B);
        if (x != y) c = x < y ? -1 : 1;
        if (opt_sort == 5) c = -c;            /* newest first */
    }
    }
    if (!c) c = strcasecmp(ma->title, mb->title);
    if (!c) c = A->first - B->first;
    return c;
}

static void sort_groups(void) {
    if (g_group_count > 1) qsort(g_groups, g_group_count, sizeof *g_groups, cmp_groups);
}

static void build_song_groups(void) {
    int keep = selected_map_index();
    int n = map_count > 0 ? map_count : 1;
    int *o = realloc(g_order, n * sizeof *o);
    if (o) g_order = o;
    SongGroup *g = realloc(g_groups, n * sizeof *g);
    if (g) g_groups = g;
    if (!o || !g) { g_order_count = g_group_count = 0; return; }

    g_order_count = 0;
    for (int i = 0; i < map_count; i++)
        if (map_passes_filter(&maps[i])) g_order[g_order_count++] = i;
    qsort(g_order, g_order_count, sizeof(int), cmp_map_order);

    g_group_count = 0;
    for (int i = 0; i < g_order_count; i++) {
        if (g_group_count == 0 ||
            strcasecmp(maps[g_order[i]].folder,
                       maps[g_order[g_groups[g_group_count - 1].first]].folder) != 0) {
            g_groups[g_group_count].first = i;
            g_groups[g_group_count].count = 0;
            g_groups[g_group_count].last_diff = 0;
            g_group_count++;
        }
        g_groups[g_group_count - 1].count++;
    }

    sort_groups();
    g_sel_group = 0; g_sel_diff = 0;
    select_map(keep);
    g_scroll = (float)g_sel_group;
}

static void song_select_move(int dy, int dx) {
    if (g_group_count == 0) return;
    if (dy) {
        g_groups[g_sel_group].last_diff = g_sel_diff;
        int n = g_group_count;
        g_sel_group = ((g_sel_group + dy) % n + n) % n;
        g_sel_diff  = g_groups[g_sel_group].last_diff;
    }
    if (dx) {
        int n = g_groups[g_sel_group].count;
        g_sel_diff = clampi(g_sel_diff + dx, 0, n - 1);
    }
}

/* jumps straight to song number gi (used by touch scrolling / tapping a song) */
static void song_select_set(int gi) {
    if (g_group_count == 0) return;
    gi = clampi(gi, 0, g_group_count - 1);
    if (gi == g_sel_group) return;
    g_groups[g_sel_group].last_diff = g_sel_diff;
    g_sel_group = gi;
    g_sel_diff  = clampi(g_groups[gi].last_diff, 0, g_groups[gi].count - 1);
}

/* ================= Skins ================= */
/* key mode index: 1K = 0 ... 5K = 4 */
static int kidx(int keys) { return clampi(keys, 1, MAX_KEYS) - 1; }

/* skin image name part for a lane: "1" (white), "2" (blue) or "S" (middle) */
static const char *col_suffix(int keys, int c) {
    int t = col_type(keys, c);
    return t == 3 ? "S" : t == 2 ? "2" : "1";
}

typedef struct { SDL_Texture *f[16]; int n, w, h; } Anim;
#define ANIM_POOL 128
typedef struct { char path[400]; Anim a; } AnimSlot;
static AnimSlot anim_pool[ANIM_POOL];
static int anim_pool_n = 0;
static Anim anim_none;

typedef struct {
    Anim *note[KEYMODES][MAX_KEYS], *head[KEYMODES][MAX_KEYS], *body[KEYMODES][MAX_KEYS];
    Anim *key[KEYMODES][MAX_KEYS], *keyd[KEYMODES][MAX_KEYS];
    int col_w[KEYMODES][MAX_KEYS], has_col_w[KEYMODES], hit_pos[KEYMODES], score_pos[KEYMODES];
} Skin;
static Skin skin;
static int skin_fps = 30;

#define MAX_SKINS 32
static char skin_names[MAX_SKINS][128];
static char skin_dirs[MAX_SKINS][300];
static int skin_count = 0;

static void skins_scan(void) {
    skin_count = 1;
    snprintf(skin_names[0], sizeof skin_names[0], "Default");
    skin_dirs[0][0] = 0;
    if (is_dir(DATA_DIR "skin")) {
        snprintf(skin_names[skin_count], 128, "skin");
        snprintf(skin_dirs[skin_count], 300, "%sskin", DATA_DIR);
        skin_count++;
    }
    DIR *d = opendir(DATA_DIR "skins");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && skin_count < MAX_SKINS) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof p, "%sskins/%s", DATA_DIR, e->d_name);
        if (!is_dir(p)) continue;
        snprintf(skin_names[skin_count], 128, "%s", e->d_name);
        snprintf(skin_dirs[skin_count], 300, "%s", p);
        skin_count++;
    }
    closedir(d);
}

static int skin_index(void) {
    if (!opt_skin[0]) return 0;
    for (int i = 1; i < skin_count; i++)
        if (!strcmp(skin_names[i], opt_skin)) return i;
    return 0;
}

static SDL_Texture *load_tex(const char *path, int *w, int *h) {
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    *w = s->w; *h = s->h;
    SDL_FreeSurface(s);
    return t;
}

static Anim *skin_anim(const char *dir, const char *name) {
    if (!dir[0] || !name[0]) return &anim_none;
    char base[400];
    snprintf(base, sizeof base, "%s/%s", dir, name);
    for (char *c = base; *c; c++) if (*c == '\\') *c = '/';
    size_t L = strlen(base);
    if (L > 4 && !strcasecmp(base + L - 4, ".png")) base[L - 4] = 0;

    for (int i = 0; i < anim_pool_n; i++)
        if (!strcmp(anim_pool[i].path, base)) return &anim_pool[i].a;
    if (anim_pool_n >= ANIM_POOL) return &anim_none;

    AnimSlot *s = &anim_pool[anim_pool_n];
    memset(s, 0, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", base);
    char p[420];
    for (int i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "%s-%d.png", base, i);
        SDL_Texture *t = load_tex(p, &s->a.w, &s->a.h);
        if (!t) { snprintf(p, sizeof p, "%s-%d@2x.png", base, i); t = load_tex(p, &s->a.w, &s->a.h); }
        if (!t) break;
        s->a.f[s->a.n++] = t;
    }
    if (!s->a.n) {
        snprintf(p, sizeof p, "%s.png", base);
        SDL_Texture *t = load_tex(p, &s->a.w, &s->a.h);
        if (!t) { snprintf(p, sizeof p, "%s@2x.png", base); t = load_tex(p, &s->a.w, &s->a.h); }
        if (t) s->a.f[s->a.n++] = t;
    }
    anim_pool_n++;
    return &s->a;
}

static SDL_Texture *anim_frame(const Anim *a) {
    if (!a || !a->n) return NULL;
    if (a->n == 1) return a->f[0];
    return a->f[(SDL_GetTicks() * (Uint32)skin_fps / 1000) % (Uint32)a->n];
}

static void skin_free(void) {
    for (int i = 0; i < anim_pool_n; i++)
        for (int j = 0; j < anim_pool[i].a.n; j++)
            SDL_DestroyTexture(anim_pool[i].a.f[j]);
    anim_pool_n = 0;
}

static void skin_load(int idx) {
    static char ov[KEYMODES][MAX_KEYS][5][160];
    skin_free();
    memset(&skin, 0, sizeof skin);
    memset(ov, 0, sizeof ov);
    skin_fps = 30;

    const char *dir = (idx > 0 && idx < skin_count) ? skin_dirs[idx] : "";

    if (dir[0]) {
        char path[400];
        snprintf(path, sizeof path, "%s/skin.ini", dir);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            int sec = 0, mk = -1;
            while (fgets(line, sizeof line, f)) {
                trim(line);
                if (!line[0] || (line[0] == '/' && line[1] == '/')) continue;
                if (line[0] == '[') {
                    sec = !strcasecmp(line, "[General]") ? 1 : !strcasecmp(line, "[Mania]") ? 2 : 0;
                    mk = -1;
                    continue;
                }
                char *colon = strchr(line, ':');
                if (!colon) continue;
                *colon = 0;
                char *key = line, *val = colon + 1;
                trim(key); trim(val);

                if (sec == 1 && !strcasecmp(key, "AnimationFramerate")) {
                    int v = atoi(val);
                    if (v > 0) skin_fps = v;
                }
                if (sec != 2) continue;
                if (!strcasecmp(key, "Keys")) {
                    int v = atoi(val);
                    mk = (v >= 1 && v <= MAX_KEYS) ? v - 1 : -1;   /* 1K - 5K */
                    continue;
                }
                if (mk < 0) continue;
                int keys = mk + 1;

                if (!strcasecmp(key, "ColumnWidth")) {
                    char *p = val;
                    for (int c = 0; c < keys && p && *p; c++) {
                        skin.col_w[mk][c] = atoi(p);
                        p = strchr(p, ',');
                        if (p) p++;
                    }
                    skin.has_col_w[mk] = 1;
                } else if (!strcasecmp(key, "HitPosition")) {
                    skin.hit_pos[mk] = atoi(val);
                } else if (!strcasecmp(key, "ScorePosition")) {
                    skin.score_pos[mk] = atoi(val);
                } else {
                    int n = -1, slot = -1;
                    char suf[8] = "";
                    if (sscanf(key, "NoteImage%d%7s", &n, suf) >= 1)
                        slot = !suf[0] ? 0 : !strcmp(suf, "H") ? 1 : !strcmp(suf, "L") ? 2 : -1;
                    else if ((suf[0] = 0, sscanf(key, "KeyImage%d%7s", &n, suf) >= 1))
                        slot = !suf[0] ? 3 : !strcmp(suf, "D") ? 4 : -1;
                    if (slot >= 0 && n >= 0 && n < keys)
                        snprintf(ov[mk][n][slot], 160, "%s", val);
                }
            }
            fclose(f);
        }
    }

    for (int k = 0; k < KEYMODES; k++) {
        int keys = k + 1;
        for (int c = 0; c < MAX_KEYS; c++) {
            if (c >= keys) {
                skin.note[k][c] = skin.head[k][c] = skin.body[k][c] =
                skin.key[k][c] = skin.keyd[k][c] = &anim_none;
                continue;
            }
            const char *t = col_suffix(keys, c);
            char nm[5][160];
            snprintf(nm[0], 160, "mania-note%s", t);
            snprintf(nm[1], 160, "mania-note%sH", t);
            snprintf(nm[2], 160, "mania-note%sL", t);
            snprintf(nm[3], 160, "mania-key%s", t);
            snprintf(nm[4], 160, "mania-key%sD", t);
            for (int s = 0; s < 5; s++) if (ov[k][c][s][0]) snprintf(nm[s], 160, "%s", ov[k][c][s]);
            skin.note[k][c] = skin_anim(dir, nm[0]);
            skin.head[k][c] = skin_anim(dir, nm[1]);
            skin.body[k][c] = skin_anim(dir, nm[2]);
            skin.key[k][c]  = skin_anim(dir, nm[3]);
            skin.keyd[k][c] = skin_anim(dir, nm[4]);
        }
    }
}

/* ================= Song backgrounds (loaded on a second thread) ================= */
static SDL_Texture *bg_tex = NULL;
static int    bg_loaded_map = -1, bg_want_map = -1;
static Uint32 bg_want_since = 0, bg_loaded_at = 0;
static char   bg_loaded_path[512] = "";

static SDL_Thread  *bg_thread = NULL;
static SDL_atomic_t bg_done;
static SDL_Surface *bg_result = NULL;
static char         bg_job_path[512];

/* opens the picture and scales it to the screen (safe to run on a second thread) */
static SDL_Surface *load_bg_surface(const char *path) {
    SDL_Surface *raw = IMG_Load(path);
    if (!raw) return NULL;
    SDL_Surface *src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(raw);
    if (!src) return NULL;
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst) { SDL_FreeSurface(src); return NULL; }

    float s = fmaxf((float)SCREEN_W / src->w, (float)SCREEN_H / src->h);
    SDL_Rect cut;
    cut.w = (int)(SCREEN_W / s);  cut.h = (int)(SCREEN_H / s);
    cut.x = (src->w - cut.w) / 2; cut.y = (src->h - cut.h) / 2;
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_BlitScaled(src, &cut, dst, NULL);
    SDL_FreeSurface(src);
    return dst;
}

static int SDLCALL bg_worker(void *ud) {
    (void)ud;
    bg_result = load_bg_surface(bg_job_path);
    SDL_AtomicSet(&bg_done, 1);
    return 0;
}

static void bg_set_surface(SDL_Surface *s) {
    if (bg_tex) { SDL_DestroyTexture(bg_tex); bg_tex = NULL; }
    if (s) { bg_tex = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s); }
    bg_loaded_at = SDL_GetTicks();
}

/* picks up a finished picture; returns 1 while one is still loading.
   wait = 1 waits for it instead. */
static int bg_finish(int wait) {
    if (!bg_thread) return 0;
    if (!wait && !SDL_AtomicGet(&bg_done)) return 1;
    SDL_WaitThread(bg_thread, NULL);
    bg_thread = NULL;
    bg_set_surface(bg_result);
    bg_result = NULL;
    return 0;
}

static void bg_start(int idx, int threaded) {
    bg_loaded_map = idx;
    char path[512] = "";
    if (idx >= 0 && idx < map_count && maps[idx].bg[0])
        snprintf(path, sizeof path, "%s/%s", maps[idx].folder, maps[idx].bg);
    if (strcmp(path, bg_loaded_path) == 0) return;      /* same picture */
    snprintf(bg_loaded_path, sizeof bg_loaded_path, "%s", path);
    if (!path[0]) { bg_set_surface(NULL); return; }

    if (threaded) {
        snprintf(bg_job_path, sizeof bg_job_path, "%s", path);
        SDL_AtomicSet(&bg_done, 0);
        bg_thread = SDL_CreateThreadWithStackSize(bg_worker, "bg_load", 512 * 1024, NULL);
        if (bg_thread) return;
    }
    bg_set_surface(load_bg_surface(path));              /* load right now */
}

/* loads straight away (used when a song starts) */
static void bg_load_now(int idx) {
    bg_finish(1);
    if (idx == bg_loaded_map) return;
    bg_start(idx, 0);
}

static void draw_background(int idx, int dim, int wait_ms) {
    if (!opt_show_bg) return;
    int busy = bg_finish(0);
    if (idx != bg_want_map) { bg_want_map = idx; bg_want_since = SDL_GetTicks(); }
    if (!busy && idx != bg_loaded_map && SDL_GetTicks() - bg_want_since >= (Uint32)wait_ms)
        bg_start(idx, 1);
    if (!bg_tex) return;

    Uint32 age = SDL_GetTicks() - bg_loaded_at;
    SDL_SetTextureAlphaMod(bg_tex, age >= 200 ? 255 : (Uint8)(age * 255 / 200));
    SDL_RenderCopy(ren, bg_tex, NULL, NULL);
    if (dim > 0) fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, (Uint8)(dim * 255 / 100));
}

/* ================= Song preview (loaded on a second thread) ================= */
static Mix_Music *pv_music = NULL;
static char   pv_path[512] = "";
static int    pv_map = -1, pv_want = -1, pv_start_ms = 0;
static Uint32 pv_want_since = 0;

static SDL_Thread  *pv_thread = NULL;
static SDL_atomic_t pv_done;
static Mix_Music   *pv_job = NULL;       /* the music the second thread is working on */

/* opens the song (if needed) and skips to the preview point - the slow part */
static int SDLCALL pv_worker(void *ud) {
    (void)ud;
    Mix_Music *m = pv_job;
    if (!m) m = Mix_LoadMUS(pv_path);
    if (m && Mix_FadeInMusicPos(m, 1, 800, pv_start_ms / 1000.0) < 0)
        Mix_PlayMusic(m, 1);
    pv_job = m;
    SDL_AtomicSet(&pv_done, 1);
    return 0;
}

/* existing = NULL loads pv_path, otherwise restarts that music */
static void pv_begin(Mix_Music *existing) {
    pv_job = existing;
    pv_music = NULL;
    SDL_AtomicSet(&pv_done, 0);
    pv_thread = SDL_CreateThreadWithStackSize(pv_worker, "pv_load", 512 * 1024, NULL);
    if (!pv_thread) { pv_worker(NULL); pv_music = pv_job; pv_job = NULL; }
}

/* returns 1 while the preview is still loading; wait = 1 waits for it */
static int pv_finish(int wait) {
    if (!pv_thread) return 0;
    if (!wait && !SDL_AtomicGet(&pv_done)) return 1;
    SDL_WaitThread(pv_thread, NULL);
    pv_thread = NULL;
    pv_music = pv_job;
    pv_job = NULL;
    return 0;
}

static void preview_stop(void) {
    pv_finish(1);
    Mix_HaltMusic();
    if (pv_music) { Mix_FreeMusic(pv_music); pv_music = NULL; }
    pv_path[0] = 0;
    pv_map = -1;
    pv_want = -1;
}

static void preview_update(int idx) {
    if (!opt_preview || idx < 0) {
        if (pv_music || pv_thread) preview_stop();
        return;
    }
    if (pv_finish(0)) return;              /* still loading: leave the mixer alone */
    if (idx != pv_want) { pv_want = idx; pv_want_since = SDL_GetTicks(); }

    if (idx != pv_map && SDL_GetTicks() - pv_want_since >= 300) {
        pv_map = idx;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", maps[idx].folder, maps[idx].audio);
        if (strcmp(path, pv_path) != 0) {
            Mix_HaltMusic();
            if (pv_music) { Mix_FreeMusic(pv_music); pv_music = NULL; }
            snprintf(pv_path, sizeof pv_path, "%s", path);
            pv_start_ms = maps[idx].preview_ms > 0 ? maps[idx].preview_ms : 0;
            pv_begin(NULL);
            return;
        }
    }
    if (pv_music && !Mix_PlayingMusic()) pv_begin(pv_music);   /* loop the preview */
}

/* ================= osu!mania ScoreV1 (max 1,000,000 x mod multiplier) ================= */
enum { J_MAX, J_300, J_200, J_100, J_50, J_MISS };

static double sc_points = 0.0, sc_bonus = 100.0, sc_mult = 1.0;
static int    sc_total  = 1;

static void score_reset(int total_judgements, float mult) {
    sc_points = 0.0;
    sc_bonus  = 100.0;
    sc_total  = total_judgements > 0 ? total_judgements : 1;
    sc_mult   = mult > 0.0f ? (double)mult : 1.0;
}

static void score_add_judgement(int j) {
    static const int hit_value[]   = {320, 300, 200, 100, 50, 0};
    static const int bonus_value[] = { 32,  32,  16,   8,  4, 0};
    static const int bonus_add[]   = {  2,   1,   0,   0,  0, 0};
    static const int bonus_sub[]   = {  0,   0,   8,  24, 44, 0};

    if (j == J_MISS) sc_bonus = 0.0;
    else {
        sc_bonus += bonus_add[j] - bonus_sub[j];
        if (sc_bonus > 100.0) sc_bonus = 100.0;
        if (sc_bonus < 0.0)   sc_bonus = 0.0;
    }
    double unit = 500000.0 / sc_total;
    sc_points += unit * hit_value[j] / 320.0
               + unit * bonus_value[j] * sqrt(sc_bonus) / 320.0;
}

static int score_current(void) {
    long cap = lround(1000000.0 * sc_mult);
    long s   = lround(sc_points * sc_mult);
    return (int)(s > cap ? cap : s < 0 ? 0 : s);
}

/* ================= Local scores (scores.txt) ================= */
#define SCORES_PATH DATA_DIR "scores.txt"
#define LB_SHOW 8

typedef struct {
    char key[300];
    int  score, combo, n[6];
    float acc;
    long when;
    unsigned mods;              /* 0 = no mods (and every score saved before mods existed) */
} ScoreEntry;

static ScoreEntry *all_scores = NULL;
static int all_count = 0, all_cap = 0;
static ScoreEntry lb[LB_SHOW];
static int lb_count = 0, lb_total = 0, lb_for_map = -2;

static void map_key(const MapInfo *m, char *out, size_t n) {
    snprintf(out, n, "%s|%s|%dK", base_name(m->folder), m->version, m->keys);
}

static void scores_push(const ScoreEntry *e) {
    if (all_count == all_cap) {
        int nc = all_cap ? all_cap * 2 : 64;
        ScoreEntry *p = realloc(all_scores, nc * sizeof *p);
        if (!p) return;
        all_scores = p; all_cap = nc;
    }
    all_scores[all_count++] = *e;
}

/* one line of scores.txt (shared by saving a new score and rewriting after a delete) */
static void score_write_line(FILE *f, const ScoreEntry *e) {
    fprintf(f, "%s\t%d\t%.2f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%ld\t%u\n",
            e->key, e->score, e->acc, e->combo,
            e->n[0], e->n[1], e->n[2], e->n[3], e->n[4], e->n[5], e->when, e->mods);
}

static void scores_load(void) {
    FILE *f = fopen(SCORES_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        ScoreEntry e;
        memset(&e, 0, sizeof e);
        int got = sscanf(line, "%299[^\t]\t%d\t%f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%ld\t%u",
                         e.key, &e.score, &e.acc, &e.combo,
                         &e.n[0], &e.n[1], &e.n[2], &e.n[3], &e.n[4], &e.n[5], &e.when, &e.mods);
        if (got == 11 || got == 12) {          /* 11 = old line without mods */
            if (got == 11) e.mods = 0;
            e.mods = mods_clean(e.mods);
            scores_push(&e);
        }
    }
    fclose(f);
}

static void scores_save_all(void) {
    FILE *f = fopen(SCORES_PATH, "w");
    if (!f) return;
    for (int i = 0; i < all_count; i++) score_write_line(f, &all_scores[i]);
    fclose(f);
}

static int scores_add(const MapInfo *m, int score, float acc, int combo, const int n[6], unsigned mods) {
    ScoreEntry e;
    memset(&e, 0, sizeof e);
    map_key(m, e.key, sizeof e.key);
    e.score = score; e.acc = acc; e.combo = combo; e.mods = mods;
    memcpy(e.n, n, sizeof e.n);
    e.when = (long)time(NULL);

    int rank = 1;
    for (int i = 0; i < all_count; i++)
        if (!strcmp(all_scores[i].key, e.key) && all_scores[i].score > score) rank++;

    scores_push(&e);
    FILE *f = fopen(SCORES_PATH, "a");
    if (f) { score_write_line(f, &e); fclose(f); }
    lb_for_map = -2;
    return rank;
}

static void scores_delete(int i) {
    if (i < 0 || i >= all_count) return;
    memmove(&all_scores[i], &all_scores[i + 1], (size_t)(all_count - i - 1) * sizeof *all_scores);
    all_count--;
    scores_save_all();
    lb_for_map = -2;
}

/* rank of a saved score on its difficulty; *total = scores on that difficulty */
static int score_rank(int i, int *total) {
    int rank = 1, n = 0;
    for (int k = 0; k < all_count; k++) {
        if (strcmp(all_scores[k].key, all_scores[i].key)) continue;
        n++;
        if (all_scores[k].score > all_scores[i].score) rank++;
    }
    if (total) *total = n;
    return rank;
}

/* finds the installed map a score belongs to (-1 = not installed) */
static int map_from_key(const char *key) {
    char k[300];
    for (int i = 0; i < map_count; i++) {
        map_key(&maps[i], k, sizeof k);
        if (!strcmp(k, key)) return i;
    }
    return -1;
}

/* "folder|version|4K"  ->  "folder  [version] 4K" */
static void key_label(const char *key, char *out, size_t n) {
    const char *p1 = strchr(key, '|'), *p2 = strrchr(key, '|');
    if (!p1 || p1 == p2) { snprintf(out, n, "%s", key); return; }
    snprintf(out, n, "%.*s  [%.*s] %s", (int)(p1 - key), key, (int)(p2 - p1 - 1), p1 + 1, p2 + 1);
}

static void fmt_date(long when, const char *fmt, char *out, size_t n) {
    time_t t = (time_t)when;
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(out, n, fmt, tm)) out[0] = 0;
}

static void lb_refresh(int idx) {
    lb_for_map = idx; lb_count = 0; lb_total = 0;
    if (idx < 0) return;
    char key[300];
    map_key(&maps[idx], key, sizeof key);
    for (int i = 0; i < all_count; i++) {
        if (strcmp(all_scores[i].key, key)) continue;
        lb_total++;
        int pos = lb_count;
        while (pos > 0 && lb[pos - 1].score < all_scores[i].score) pos--;
        if (pos >= LB_SHOW) continue;
        int last = lb_count < LB_SHOW ? lb_count : LB_SHOW - 1;
        for (int k = last; k > pos; k--) lb[k] = lb[k - 1];
        lb[pos] = all_scores[i];
        if (lb_count < LB_SHOW) lb_count++;
    }
}

static const char *lb_grade(float acc, SDL_Color *c) {
    if (acc >= 100.0f) { *c = (SDL_Color){255, 230, 120, 255}; return "SS"; }
    if (acc >= 95.0f)  { *c = (SDL_Color){255, 200,  60, 255}; return "S"; }
    if (acc >= 90.0f)  { *c = (SDL_Color){120, 230, 120, 255}; return "A"; }
    if (acc >= 80.0f)  { *c = (SDL_Color){ 90, 160, 255, 255}; return "B"; }
    if (acc >= 70.0f)  { *c = (SDL_Color){200, 120, 255, 255}; return "C"; }
    *c = (SDL_Color){255, 90, 90, 255};
    return "D";
}

static void fmt_score(int s, char *out, size_t n) {
    if (s >= 1000000)   snprintf(out, n, "%d,%03d,%03d", s / 1000000, s / 1000 % 1000, s % 1000);
    else if (s >= 1000) snprintf(out, n, "%d,%03d", s / 1000, s % 1000);
    else                snprintf(out, n, "%d", s);
}

/* Full combo tag from the judgement counts (works for old scores too):
   "PFC" (gold) = only MAX / 300,  "FC" (blue) = no misses,  NULL = combo broken */
static const char *fc_tag(const int *n, SDL_Color *col) {
    static const SDL_Color c_pfc = {255, 210, 90, 255}, c_fc = {120, 200, 255, 255};
    int total = 0, below300 = 0;
    for (int j = J_MAX; j <= J_MISS; j++) total += n[j];
    for (int j = J_200; j <= J_MISS; j++) below300 += n[j];
    if (total == 0 || n[J_MISS] > 0) return NULL;
    *col = below300 == 0 ? c_pfc : c_fc;
    return below300 == 0 ? "PFC" : "FC";
}

/* (the song select leaderboard - Local / Global - is drawn further down,
   just above "Song select") */

/* ================= Note colours ================= */
#define NOTE_COLORS 6
static const char *note_color_names[NOTE_COLORS] = {
    "Skin", "Classic", "Rainbow", "Pink", "Cyan", "Gold"
};
/* [colour][0 = 4K, 1 = 5K][lane] = {R, G, B}   (0-255, change them if you like)
   1K - 3K use the 5K colours (see note_rgb) */
static const Uint8 note_pal[NOTE_COLORS][2][MAX_KEYS][3] = {
    {{{255,255,255},{255,255,255},{255,255,255},{255,255,255},{255,255,255}},   /* Skin */
     {{255,255,255},{255,255,255},{255,255,255},{255,255,255},{255,255,255}}},
    {{{255,255,255},{ 90,200,255},{ 90,200,255},{255,255,255},{255,255,255}},   /* Classic */
     {{255,255,255},{ 90,200,255},{255,200, 60},{ 90,200,255},{255,255,255}}},
    {{{255, 80, 80},{255,220, 60},{ 80,230,120},{ 90,150,255},{255,255,255}},   /* Rainbow */
     {{255, 80, 80},{255,220, 60},{ 80,230,120},{ 90,150,255},{190,110,255}}},
    {{{255,120,200},{255,120,200},{255,120,200},{255,120,200},{255,120,200}},   /* Pink */
     {{255,120,200},{255,120,200},{255,120,200},{255,120,200},{255,120,200}}},
    {{{ 80,230,255},{ 80,230,255},{ 80,230,255},{ 80,230,255},{ 80,230,255}},   /* Cyan */
     {{ 80,230,255},{ 80,230,255},{ 80,230,255},{ 80,230,255},{ 80,230,255}}},
    {{{255,200, 60},{255,200, 60},{255,200, 60},{255,200, 60},{255,200, 60}},   /* Gold */
     {{255,200, 60},{255,200, 60},{255,200, 60},{255,200, 60},{255,200, 60}}},
};

/* gives the chosen colour for a lane; returns 0 when "Skin" is chosen */
static int note_rgb(int keys, int c, Uint8 *r, Uint8 *g, Uint8 *b) {
    /* 1K - 3K lanes borrow these 5K lanes: 1K = middle, 2K = outer, 3K = outer + middle */
    static const int lane5[3][3] = { {2, 2, 2}, {0, 4, 4}, {0, 2, 4} };
    if (opt_note_color <= 0 || opt_note_color >= NOTE_COLORS) return 0;
    int row = 1;
    c = clampi(c, 0, MAX_KEYS - 1);
    if (keys == 4) row = 0;
    else if (keys < 4) c = lane5[clampi(keys, 1, 3) - 1][clampi(c, 0, 2)];
    const Uint8 *p = note_pal[opt_note_color][row][c];
    *r = p[0]; *g = p[1]; *b = p[2];
    return 1;
}

/* ================= Settings file ================= */
#define SETTINGS_PATH DATA_DIR "settings.txt"

static void settings_save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "speed=%d\noffset=%d\nvolume=%d\nskin=%s\nskin_layout=%d\n"
               "lane_width=%d\nhit_y=%d\nhud=%d\nstart_delay=%d\nrear_touch=%d\n"
               "filter=%d\nshow_bg=%d\nbg_dim=%d\npreview=%d\nnote_color=%d\n",
            opt_speed, opt_offset, opt_volume, opt_skin, opt_skin_layout,
            opt_lane_w, opt_hit_y, opt_hud, opt_delay, opt_rear,
            opt_filter, opt_show_bg, opt_bg_dim, opt_preview, opt_note_color);
    fprintf(f, "sort=%d\nmods=%u\nhealth_bar=%d\nhit_error=%d\ntouch=%d\nvideo=%d\n",
            opt_sort, opt_mods & ~MODS_AUTO, opt_health_bar,     /* AT / CM never saved */
            opt_hit_error, opt_touch, opt_video);
    fprintf(f, "judge_left=%d\n", opt_judge_left);
    fprintf(f, "sfx_volume=%d\n", opt_sfx_volume);
    fprintf(f, "lb_global=%d\n", opt_lb_global);
    fprintf(f, "ingame_lb=%d\n", opt_ingame_lb);
    for (int k = 0; k < KEYMODES; k++)              /* bind1_0 ... bind5_4 */
        for (int c = 0; c <= k; c++)
            fprintf(f, "bind%d_%d=%d\n", k + 1, c, binds[k][c]);
    fclose(f);
}

static void settings_load(void) {
    memcpy(binds, default_binds, sizeof binds);
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        int iv = atoi(v), kk, cc;
        if      (!strcmp(k, "speed"))       opt_speed = iv;
        else if (!strcmp(k, "offset"))      opt_offset = iv;
        else if (!strcmp(k, "volume"))      opt_volume = iv;
        else if (!strcmp(k, "skin"))        snprintf(opt_skin, sizeof opt_skin, "%s", v);
        else if (!strcmp(k, "skin_layout")) opt_skin_layout = iv;
        else if (!strcmp(k, "lane_width"))  opt_lane_w = iv;
        else if (!strcmp(k, "hit_y"))       opt_hit_y = iv;
        else if (!strcmp(k, "hud"))         opt_hud = iv;
        else if (!strcmp(k, "start_delay")) opt_delay = iv;
        else if (!strcmp(k, "rear_touch"))  opt_rear = iv;
        else if (!strcmp(k, "filter"))      opt_filter = iv;
        else if (!strcmp(k, "show_bg"))     opt_show_bg = iv;
        else if (!strcmp(k, "bg_dim"))      opt_bg_dim = iv;
        else if (!strcmp(k, "preview"))     opt_preview = iv;
        else if (!strcmp(k, "note_color"))  opt_note_color = iv;
        else if (!strcmp(k, "sort"))        opt_sort = clampi(iv, 0, SORT_COUNT - 1);
        else if (!strcmp(k, "mods"))        opt_mods = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(k, "health_bar"))  opt_health_bar = iv;
        else if (!strcmp(k, "hit_error"))   opt_hit_error = iv;
        else if (!strcmp(k, "touch"))       opt_touch = iv;
        else if (!strcmp(k, "video"))       opt_video = iv;
        else if (!strcmp(k, "judge_left"))  opt_judge_left = iv;
        else if (!strcmp(k, "sfx_volume"))  opt_sfx_volume = iv;
        else if (!strcmp(k, "lb_global"))   opt_lb_global = iv;
        else if (!strcmp(k, "ingame_lb"))   opt_ingame_lb = iv;
        else if (sscanf(k, "bind%d_%d", &kk, &cc) == 2 &&
                 kk >= 1 && kk <= MAX_KEYS && cc >= 0 && cc < kk)
            binds[kk - 1][cc] = clampi(iv, -1, SDL_CONTROLLER_BUTTON_MAX - 1);
    }
    fclose(f);
    opt_speed  = clampi(opt_speed, 4, 40);
    opt_offset = clampi(opt_offset, -500, 500);
    opt_volume = clampi(opt_volume, 0, 100);
    opt_sfx_volume = clampi(opt_sfx_volume, 0, 100);
    opt_lane_w = clampi(opt_lane_w, 40, 120);
    opt_hit_y  = clampi(opt_hit_y, 300, 530);
    opt_delay  = clampi(opt_delay, 500, 5000);
    opt_filter = clampi(opt_filter, 0, FILTER_COUNT - 1);
    opt_bg_dim = clampi(opt_bg_dim, 0, 100);
    opt_note_color = clampi(opt_note_color, 0, NOTE_COLORS - 1);
    opt_mods   = mods_clean(opt_mods & ~MODS_AUTO);
    /* on/off settings can only be 0 or 1 */
    opt_skin_layout = !!opt_skin_layout;
    opt_hud         = !!opt_hud;
    opt_rear        = !!opt_rear;
    opt_show_bg     = !!opt_show_bg;
    opt_preview     = !!opt_preview;
    opt_health_bar  = !!opt_health_bar;
    opt_hit_error   = !!opt_hit_error;
    opt_touch       = !!opt_touch;
    opt_video       = !!opt_video;
    opt_judge_left  = !!opt_judge_left;
    opt_lb_global   = !!opt_lb_global;
}

/* ================= Input devices ================= */
static SDL_GameController *pad = NULL;
static SDL_Joystick *joy = NULL;
static SDL_TouchID rear_id = 0, front_id = 0;
static int has_rear = 0, has_front = 0, rear_fingers = 0;

static const int vita_joy_map[12] = {
    SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START
};

static int btn_held(int b) {
    if (b < 0) return 0;
    if (pad) return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b);
    if (joy) for (int i = 0; i < 12; i++) if (vita_joy_map[i] == b) return SDL_JoystickGetButton(joy, i);
    return 0;
}

/* ================= Audio clock (keeps notes synced to the music) ================= */
static int au_freq = 44100, au_frame_bytes = 4, au_buf_ms = 23;
static volatile int au_running = 0;
static SDL_atomic_t au_frames, au_cb_tick;
static double clk = 0.0;            /* smoothed song position in ms */
static Uint32 clk_tick = 0;
static void *play_music_mem = NULL;

/* Runs on the audio thread after every mixed buffer */
static void SDLCALL postmix_cb(void *ud, Uint8 *stream, int len) {
    (void)ud; (void)stream;
    if (au_running) SDL_AtomicAdd(&au_frames, len / au_frame_bytes);
    SDL_AtomicSet(&au_cb_tick, (int)SDL_GetTicks());
}

static void audio_clock_init(void) {
    int fq = 0, ch = 0;
    Uint16 fmt = 0;
    if (Mix_QuerySpec(&fq, &fmt, &ch) && fq > 0 && ch > 0) {
        au_freq = fq;
        au_frame_bytes = (SDL_AUDIO_BITSIZE(fmt) / 8) * ch;
    }
    au_buf_ms = AUDIO_CHUNK * 1000 / au_freq;
    Mix_SetPostMix(postmix_cb, NULL);
}

static int audio_ms(void) {
    Uint32 cb = (Uint32)SDL_AtomicGet(&au_cb_tick);
    Uint32 frames = (Uint32)SDL_AtomicGet(&au_frames);
    int ms = (int)((Uint64)frames * 1000 / au_freq) - au_buf_ms;
    if (au_running) {
        Uint32 since = SDL_GetTicks() - cb;
        ms += (int)(since < (Uint32)au_buf_ms ? since : (Uint32)au_buf_ms);
    }
    return ms;
}

/* Loads the whole song file into RAM so the memory card can't stall it */
static Mix_Music *load_music_ram(const char *path) {
    SDL_RWops *f = SDL_RWFromFile(path, "rb");
    if (!f) return NULL;
    Sint64 sz = SDL_RWsize(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { SDL_RWclose(f); return Mix_LoadMUS(path); }
    void *buf = malloc((size_t)sz);
    if (!buf) { SDL_RWclose(f); return Mix_LoadMUS(path); }
    size_t got = SDL_RWread(f, buf, 1, (size_t)sz);
    SDL_RWclose(f);
    if (got != (size_t)sz) { free(buf); return Mix_LoadMUS(path); }

    Mix_Music *m = Mix_LoadMUS_RW(SDL_RWFromConstMem(buf, (int)sz), 1);
    if (!m) { free(buf); return Mix_LoadMUS(path); }
    play_music_mem = buf;
    return m;
}

/* ================= Video backgrounds (mp4 / H.264, Vita hardware decoder) ================= */
#define VIDEO_MAX_W   1280
#define VIDEO_MAX_H   720
#define VIDEO_NV21    0      /* colours look wrong (blue skin, orange sky)? set to 1 */
#define VIDEO_ALIGN   16     /* picture slanted or striped? try 1, 32 or 64 */

static int vid_on = 0, vid_started = 0, vid_paused = 0, vid_ofs = 0;
static int vid_w = 0, vid_hh = 0;
static int vid_err = 0;                  /* set when a video turns out to be too big */
static volatile int vid_ready = 0;       /* 1 once the player says the video is ready */
static int vid_tried = 0, vid_jump = 0;
static SDL_Texture *vid_tex = NULL;
static char vid_path[512];

#ifdef __vita__
#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>

static SceAvPlayerHandle vid_player;
static int vid_have = 0, vid_module = 0;     /* module: 0 = not loaded, 1 = ok, -1 = failed */

static void *vid_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    if (align < 16) align = 16;
    return memalign(align, size);
}
static void vid_free(void *p, void *m) { (void)p; free(m); }

/* the player tells us what it's doing (2 = ready to play) */
static void vid_event(void *p, int32_t id, int32_t src, void *data) {
    (void)p; (void)src; (void)data;
    if (id == 2) vid_ready = 1;
}

/* video frames go in video memory (or plain contiguous memory if that's full) */
static void *vid_frame_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p; (void)align;
    void *base = NULL;
    uint32_t sz = (size + 0x3FFFF) & ~0x3FFFFu;                  /* 256 KB steps */
    SceUID b = sceKernelAllocMemBlock("maniavita_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL);
    if (b < 0) {
        sz = (size + 0xFFFFF) & ~0xFFFFFu;                       /* 1 MB steps */
        b = sceKernelAllocMemBlock("maniavita_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    }
    if (b < 0) return NULL;
    sceKernelGetMemBlockBase(b, &base);
    return base;
}
static void vid_frame_free(void *p, void *m) {
    (void)p;
    SceUID b = sceKernelFindMemBlockByAddr(m, 0);
    if (b >= 0) sceKernelFreeMemBlock(b);
}
#endif

static void video_close(void) {
#ifdef __vita__
    if (vid_have) { sceAvPlayerStop(vid_player); sceAvPlayerClose(vid_player); vid_have = 0; }
#endif
    if (vid_tex) { SDL_DestroyTexture(vid_tex); vid_tex = NULL; }
    vid_on = vid_started = vid_paused = 0;
    vid_w = vid_hh = 0;
}

/* returns 0 if OK (or the map has no video), -1 if the video can't be used.
   .avi / .flv etc. are swapped for a .mp4 with the same name (convert them with ffmpeg).
   The player itself is only made when the video's start time comes (see video_update). */
static int video_open(const MapInfo *m) {
    video_close();
    vid_err = 0;
    vid_tried = 0; vid_jump = 0;
    if (!opt_video || !m->video[0]) return 0;
    snprintf(vid_path, sizeof vid_path, "%s/%s", m->folder, m->video);

    char *dot = strrchr(vid_path, '.'), *slash = strrchr(vid_path, '/');
    if (dot && dot > slash && strcasecmp(dot, ".mp4") && strcasecmp(dot, ".m4v")) {
        if ((size_t)(dot - vid_path) + 5 > sizeof vid_path) return 0;
        strcpy(dot, ".mp4");
    }

    struct stat st;
    if (stat(vid_path, &st) != 0) return 0;             /* video file missing: use the picture */
#ifdef __vita__
    if (vid_module == 0) vid_module = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) < 0 ? -1 : 1;
    if (vid_module < 0) return -1;
    vid_ofs = m->video_ofs;
    vid_on = 1;
#endif
    return 0;
}

#ifdef __vita__
/* makes the player; it starts playing by itself */
static int video_create(void) {
    vid_ready = 0;

    SceAvPlayerInitData d;
    memset(&d, 0, sizeof d);
    d.memoryReplacement.allocate          = vid_alloc;
    d.memoryReplacement.deallocate        = vid_free;
    d.memoryReplacement.allocateTexture   = vid_frame_alloc;
    d.memoryReplacement.deallocateTexture = vid_frame_free;
    d.eventReplacement.eventCallback      = vid_event;
    d.basePriority = 0x60;                  /* higher priority so the game can't starve it */
    d.numOutputVideoFrameBuffers = 2;
    d.autoStart = 1;
    d.debugLevel = 0;
    vid_player = sceAvPlayerInit(&d);
    if (!vid_player) return -1;
    vid_have = 1;
    if (sceAvPlayerAddSource(vid_player, vid_path) < 0) return -1;
    sceAvPlayerSetLooping(vid_player, 0);
    return 0;
}
#endif

/* video_pause(1) pauses, video_pause(0) carries on */
static void video_pause(int on) {
#ifdef __vita__
    if (!vid_on || !vid_started) return;
    if (on && !vid_paused) {
        sceAvPlayerPause(vid_player);
        vid_paused = 1;
    } else if (!on && vid_paused) {
        sceAvPlayerResume(vid_player);
        vid_paused = 0;
    }
#else
    (void)on;
#endif
}

/* starts the video at the right moment; returns -1 if the video was too big and got closed */
static int video_update(int now) {
    if (vid_err) { vid_err = 0; return -1; }
#ifdef __vita__
    if (!vid_on || vid_paused) return 0;
    int vt = now - vid_ofs;                              /* where the video should be (ms) */
    if (!vid_started) {
        if (vt < 0 || vid_tried) return 0;               /* not time yet, or it already failed */
        vid_tried = 1;
        if (video_create() < 0) return 0;                /* failed: the background picture stays */
        vid_started = 1;
        vid_jump = vt > 1000;                            /* far in: jump once it's ready */
        return 0;
    }
    if (vid_jump && vid_ready) { vid_jump = 0; sceAvPlayerJumpToTime(vid_player, (uint64_t)vt); }
#else
    (void)now;
#endif
    return 0;
}

/* draws the newest video frame over the whole screen; returns 0 if there's nothing to show */
static int video_draw(void) {
    if (!vid_on) return 0;
#ifdef __vita__
    SceAvPlayerFrameInfo fr;
    if (vid_started) {
        for (int i = 0; i < 16; i++) {                   /* the video's own sound isn't used */
            memset(&fr, 0, sizeof fr);
            if (!sceAvPlayerGetAudioData(vid_player, &fr)) break;
        }
        memset(&fr, 0, sizeof fr);
        if (sceAvPlayerGetVideoData(vid_player, &fr) && fr.pData) {
            int w = (int)fr.details.video.width, h = (int)fr.details.video.height;
            if (w <= 0 || h <= 0 || w > VIDEO_MAX_W || h > VIDEO_MAX_H) {
                if (!vid_tex) {                          /* the very first frame is too big */
                    video_close();
                    vid_err = 1;                         /* video_update reports it next frame */
                    return 0;
                }
                /* one odd frame: skip it */
            } else {
                if (!vid_tex || w != vid_w || h != vid_hh) {
                    if (vid_tex) SDL_DestroyTexture(vid_tex);
                    vid_tex = SDL_CreateTexture(ren, VIDEO_NV21 ? SDL_PIXELFORMAT_NV21 : SDL_PIXELFORMAT_NV12,
                                                SDL_TEXTUREACCESS_STREAMING, w, h);
                    if (!vid_tex) { video_close(); return 0; }
                    vid_w = w; vid_hh = h;
                }
                int pitch = (w + VIDEO_ALIGN - 1) / VIDEO_ALIGN * VIDEO_ALIGN;
                const Uint8 *yp = fr.pData, *uvp = fr.pData + pitch * h;
                SDL_UpdateNVTexture(vid_tex, NULL, yp, pitch, uvp, pitch);
            }
        }
    }
#endif
    if (!vid_tex || vid_w <= 0 || vid_hh <= 0) return 0;

    /* fill the screen, cutting off the edges if needed (like the background pictures) */
    float s = fmaxf((float)SCREEN_W / vid_w, (float)SCREEN_H / vid_hh);
    SDL_Rect src;
    src.w = (int)(SCREEN_W / s); src.h = (int)(SCREEN_H / s);
    src.x = (vid_w - src.w) / 2; src.y = (vid_hh - src.h) / 2;
    SDL_RenderCopy(ren, vid_tex, &src, NULL);
    return 1;
}

/* ================= Hitsounds ================= */
static Mix_Chunk *sfx_hit = NULL, *sfx_miss = NULL;
static Uint32 sfx_miss_at = 0;

static void sfx_put(Uint8 *p, Uint32 v, int bytes) {
    for (int i = 0; i < bytes; i++) p[i] = (Uint8)(v >> (8 * i));
}

/* built-in sound made in memory: a short click (hit) or a low falling tone (miss) */
static Mix_Chunk *sfx_make(int miss) {
    const int rate = 44100;
    const double TWO_PI = 6.283185307179586;
    int n = rate * (miss ? 240 : 50) / 1000;
    int bytes = 44 + n * 2;
    Uint8 *w = calloc(1, (size_t)bytes);
    if (!w) return NULL;
    memcpy(w, "RIFF", 4);          sfx_put(w + 4, (Uint32)(bytes - 8), 4);
    memcpy(w + 8, "WAVEfmt ", 8);  sfx_put(w + 16, 16, 4);
    sfx_put(w + 20, 1, 2);         sfx_put(w + 22, 1, 2);          /* PCM, mono */
    sfx_put(w + 24, rate, 4);      sfx_put(w + 28, rate * 2, 4);
    sfx_put(w + 32, 2, 2);         sfx_put(w + 34, 16, 2);         /* 16-bit */
    memcpy(w + 36, "data", 4);     sfx_put(w + 40, (Uint32)(n * 2), 4);

    double ph = 0.0;
    int tail = n / 5;
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate, v;
        if (!miss) {
            ph += TWO_PI * (1500.0 - 8000.0 * t) / rate;     /* quick high click */
            v = sin(ph) * exp(-t * 90.0) * 0.7;
        } else {
            ph += TWO_PI * (320.0 - 800.0 * t) / rate;       /* low tone falling */
            v = (sin(ph) + 0.3 * sin(ph * 2.0)) * exp(-t * 9.0) * 0.5;
        }
        if (n - i < tail) v *= (double)(n - i) / tail;       /* soft ending, no pop */
        sfx_put(w + 44 + i * 2, (Uint32)(Uint16)(Sint16)(v * 32000.0), 2);
    }
    Mix_Chunk *c = Mix_LoadWAV_RW(SDL_RWFromConstMem(w, bytes), 1);
    free(w);
    return c;
}

/* tries prefix + name + .wav / .ogg */
static Mix_Chunk *sfx_try(const char *prefix, const char *name) {
    static const char *ext[2] = { ".wav", ".ogg" };
    char p[512];
    if (!prefix[0]) return NULL;
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s%s%s", prefix, name, ext[i]);
        Mix_Chunk *c = Mix_LoadWAV(p);
        if (c) return c;
    }
    return NULL;
}

static void sfx_set_volume(void) {
    int v = opt_sfx_volume * MIX_MAX_VOLUME / 100;
    if (sfx_hit)  Mix_VolumeChunk(sfx_hit, v);
    if (sfx_miss) Mix_VolumeChunk(sfx_miss, v);
}

static void sfx_free(void) {
    Mix_HaltChannel(-1);
    if (sfx_hit)  { Mix_FreeChunk(sfx_hit);  sfx_hit = NULL; }
    if (sfx_miss) { Mix_FreeChunk(sfx_miss); sfx_miss = NULL; }
}

/* ux0:data/osumania/hit + miss first, then the skin's osu! sounds, otherwise built-in */
static void sfx_load(void) {
    char sd[320] = "";
    int si = skin_index();
    if (si > 0) snprintf(sd, sizeof sd, "%s/", skin_dirs[si]);
    sfx_free();

    sfx_hit = sfx_try(DATA_DIR, "hit");
    if (!sfx_hit) sfx_hit = sfx_try(sd, "normal-hitnormal");
    if (!sfx_hit) sfx_hit = sfx_make(0);

    sfx_miss = sfx_try(DATA_DIR, "miss");
    if (!sfx_miss) sfx_miss = sfx_try(sd, "combobreak");
    if (!sfx_miss) sfx_miss = sfx_make(1);
    sfx_set_volume();
}

static void sfx_play_hit(void) {
    if (sfx_hit && opt_sfx_volume > 0) Mix_PlayChannel(-1, sfx_hit, 0);
}

/* one miss sound at a time (a missed long note counts as 2 misses) */
static void sfx_play_miss(void) {
    Uint32 t = SDL_GetTicks();
    if (!sfx_miss || opt_sfx_volume <= 0 || t - sfx_miss_at < 80) return;
    sfx_miss_at = t;
    Mix_PlayChannel(-1, sfx_miss, 0);
}

/* ===== Online leaderboard ===== */
#include <curl/curl.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define LB_URL      "https://maniavita.com"
#define LB_PLAYER   "ux0:data/osumania/player.txt"
#define LB_PENDING  "ux0:data/osumania/pending.txt"
#define LB_MAX_ROWS 50

typedef struct {
    int rank; char name[17]; int score; float acc; int combo;
    char mods[48]; float pp; int miss;
} LbRow;

static char lb_name[17] = "";
static char lb_token[33] = "";

static SDL_mutex  *lb_mx = NULL;
static SDL_sem    *lb_wake = NULL;
static SDL_Thread *lb_thread = NULL;
static volatile int lb_quit_flag = 0;

/* Global top list for one map (song select, Global view) */
static LbRow lb_rows[LB_MAX_ROWS];
static int   lb_row_n = 0;
static volatile int lb_state = 0;    /* 0 nothing, 1 loading, 2 loaded, 3 offline/error */
static char  lb_state_map[512] = ""; /* .osu file the rows belong to */
static char  lb_want_map[512] = "";  /* .osu file whose top list is wanted */

/* Profile (Settings > Profile tab) */
typedef struct { char title[200]; int keys; char mods[48]; float acc, pp; } PfPlay;
static PfPlay pf_plays[10];
static int    pf_play_n = 0, pf_rank = 0, pf_plays_total = 0;
static float  pf_pp = 0;
static char   pf_name[17] = "";
static char   pf_msg[96] = "";
static volatile int pf_state = 0;    /* 0 nothing, 1 loading, 2 loaded, 3 offline/error, 4 not on the server yet */
static int    pf_want_keys = 0;      /* 0 = all keys, 1-5 = that key count */

/* Result of the last score sent (used by the Results screen) */
static volatile int lb_sub_state = 0; /* 0 nothing, 1 sending, 2 done, 3 saved offline, 4 rejected */
static int   lb_sub_rank = 0, lb_sub_prank = 0, lb_sub_improved = 0;
static float lb_sub_pp = 0, lb_sub_total = 0;
static char  lb_sub_msg[96] = "";
static char  lb_last_body[2048] = "";

/* --- MD5 of the .osu file (identifies the map) --- */
static uint32_t lb_md5_k[64];

static void lb_md5_block(uint32_t h[4], const uint8_t *p)
{
    static const uint8_t sh[16] = {7,12,17,22, 5,9,14,20, 4,11,16,23, 6,10,15,21};
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        w[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7*i) & 15; }
        int s = sh[(i / 16) * 4 + (i & 3)];
        uint32_t x = a + f + lb_md5_k[i] + w[g];
        a = d; d = c; c = b;
        b = b + ((x << s) | (x >> (32 - s)));
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static int lb_md5_file(const char *path, char out[33])
{
    if (!lb_md5_k[0])
        for (int i = 0; i < 64; i++)
            lb_md5_k[i] = (uint32_t)(fabs(sin(i + 1.0)) * 4294967296.0);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint8_t buf[64], last[128];
    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, 64, f)) == 64) { lb_md5_block(h, buf); total += 64; }
    fclose(f);
    total += n;
    memset(last, 0, sizeof last);
    memcpy(last, buf, n);
    last[n] = 0x80;
    int blocks = (n < 56) ? 1 : 2;
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++) last[blocks * 64 - 8 + i] = (uint8_t)(bits >> (8 * i));
    lb_md5_block(h, last);
    if (blocks == 2) lb_md5_block(h, last + 64);
    for (int i = 0; i < 16; i++)
        sprintf(out + i * 2, "%02x", (unsigned)((h[i / 4] >> ((i % 4) * 8)) & 0xff));
    out[32] = 0;
    return 1;
}

/* --- Player name + secret token (player.txt) --- */
static void lb_save_player(void)
{
    FILE *f = fopen(LB_PLAYER, "w");
    if (!f) return;
    fprintf(f, "name=%s\ntoken=%s\n", lb_name, lb_token);
    fclose(f);
}

static void lb_load_player(void)
{
    FILE *f = fopen(LB_PLAYER, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "name=", 5))  snprintf(lb_name, sizeof lb_name, "%s", line + 5);
            if (!strncmp(line, "token=", 6)) snprintf(lb_token, sizeof lb_token, "%s", line + 6);
        }
        fclose(f);
    }
    if (strlen(lb_token) < 32) {   /* first run: make a secret token */
        srand((unsigned)time(NULL) ^ (unsigned)SDL_GetPerformanceCounter());
        for (int i = 0; i < 32; i++) lb_token[i] = "0123456789abcdef"[rand() & 15];
        lb_token[32] = 0;
        lb_save_player();
    }
}

static int lb_name_ok(void)
{
    size_t n = strlen(lb_name);
    return n >= 3 && n <= 16;
}

/* Keeps only letters, numbers, _ and - (spaces become _) */
static void lb_set_name(const char *s)
{
    int j = 0;
    for (int i = 0; s[i] && j < 16; i++) {
        char ch = s[i];
        if (ch == ' ') ch = '_';
        if (isalnum((unsigned char)ch) || ch == '_' || ch == '-') lb_name[j++] = ch;
    }
    lb_name[j] = 0;
    lb_save_player();
}

/* --- HTTP --- */
typedef struct { char *p; size_t n, cap; } LbBuf;

static size_t lb_write_cb(void *data, size_t sz, size_t nm, void *user)
{
    LbBuf *b = (LbBuf *)user;
    size_t k = sz * nm, room = b->cap - 1 - b->n;
    size_t c = k < room ? k : room;
    memcpy(b->p + b->n, data, c);
    b->n += c;
    b->p[b->n] = 0;
    return k;
}

static int lb_online(void)
{
    int st = 0;
    if (sceNetCtlInetGetState(&st) < 0) return 0;
    return st == SCE_NETCTL_STATE_CONNECTED;
}

/* why the last request failed (also written to online_log.txt) */
#define LB_LOG "ux0:data/osumania/online_log.txt"
static char lb_err[96] = "";

/* largest block that can still be allocated, in KB (for the log) */
static unsigned lb_free_kb(void)
{
    for (size_t s = (size_t)64 << 20; s >= 16384; s /= 2) {
        void *p = malloc(s);
        if (p) { free(p); return (unsigned)(s / 1024); }
    }
    return 0;
}

static void lb_fail(const char *why)
{
    char when[32] = "";
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", tm);
    SDL_LockMutex(lb_mx);
    snprintf(lb_err, sizeof lb_err, "%s", why);
    FILE *f = fopen(LB_LOG, "a");
    if (f) { fprintf(f, "[%s] %s (free %u KB)\n", when, why, lb_free_kb()); fclose(f); }
    SDL_UnlockMutex(lb_mx);
}

/* certificates: read cacert.pem once and keep only the ones this OpenSSL
   can load (one bad certificate makes curl reject the whole file) */
static char  *lb_ca_data = NULL;
static size_t lb_ca_len = 0;
static int lb_ca_ok = 0, lb_ca_bad = 0;

/* only the root certificates Cloudflare uses for maniavita.com.
   (each certificate curl keeps uses a lock, and the Vita runs out of those
   if all 118 from cacert.pem are loaded) */
static int lb_ca_wanted(X509 *x)
{
    static const char *keep[] = {
        "ISRG Root", "GTS Root", "GlobalSign", "SSL.com",
        "USERTrust", "Sectigo", "AAA Certificate Services", "DigiCert Global Root"
    };
    char name[256];
    name[0] = 0;
    X509_NAME_oneline(X509_get_subject_name(x), name, sizeof name);
    for (int i = 0; i < (int)(sizeof keep / sizeof keep[0]); i++)
        if (strstr(name, keep[i])) return 1;
    return 0;
}

static void lb_ca_load(void)
{
    if (lb_ca_data) return;
    static const char *paths[] = { "app0:cacert.pem", "ux0:data/osumania/cacert.pem" };
    char *raw = NULL;
    long sz = 0;
    for (int i = 0; i < 2 && !raw; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && (raw = malloc(sz + 1)) != NULL) {
            sz = (long)fread(raw, 1, sz, f);
            raw[sz] = 0;
        }
        fclose(f);
    }
    if (!raw) return;

    char *good = malloc(sz + 1);
    if (!good) { free(raw); return; }
    FILE *lg = fopen("ux0:data/osumania/ca_log.txt", "w");
    size_t len = 0;
    int ok = 0, bad = 0, n = 0;
    const char *p = raw;
    while ((p = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL) {
        const char *e = strstr(p, "-----END CERTIFICATE-----");
        if (!e) break;
        e += 25;
        n++;
        ERR_clear_error();
        X509 *x = NULL;
        int keep = 0;
        BIO *bio = BIO_new_mem_buf((void *)p, (int)(e - p));
        if (bio) {
            x = PEM_read_bio_X509(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }
        if (!x) {
            bad++;
            if (lg) {
                char eb[256];
                unsigned long ec = ERR_get_error();
                ERR_error_string_n(ec, eb, sizeof eb);
                fprintf(lg, "cert %d failed: %s\n", n, ec ? eb : "no OpenSSL error");
            }
        } else {
            keep = lb_ca_wanted(x);
            if (keep && lg) {
                char name[256];
                name[0] = 0;
                X509_NAME_oneline(X509_get_subject_name(x), name, sizeof name);
                fprintf(lg, "keep %d: %s\n", n, name);
            }
            X509_free(x);   /* free right away so its lock is given back */
        }
        if (keep) {
            memcpy(good + len, p, (size_t)(e - p));
            len += (size_t)(e - p);
            good[len++] = '\n';
            ok++;
        }
        p = e;
    }
    good[len] = 0;
    ERR_clear_error();
    free(raw);
    if (lg) { fprintf(lg, "=== %d kept, %d failed, %d total ===\n", ok, bad, n); fclose(lg); }
    lb_ca_ok = ok;
    lb_ca_bad = bad;
    if (ok == 0) { free(good); return; }
    lb_ca_data = good;
    lb_ca_len = len;
}

/* 1 = the server answered (reply in out), 0 = no Wi-Fi / no connection */
static int lb_http(const char *path, const char *post, char *out, size_t cap)
{
    out[0] = 0;
    if (!lb_online()) { lb_fail("no Wi-Fi connection"); return 0; }
    CURL *c = curl_easy_init();
    if (!c) { lb_fail("curl init failed"); return 0; }
    char url[600];
    snprintf(url, sizeof url, "%s%s", LB_URL, path);
    LbBuf b = { out, 0, cap };
    char cerr[CURL_ERROR_SIZE];
    cerr[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    lb_ca_load();
    if (!lb_ca_data) {
        curl_easy_cleanup(c);
        lb_fail("cacert.pem could not be read");
        return 0;
    }
    struct curl_blob ca_blob = { lb_ca_data, lb_ca_len, CURL_BLOB_COPY };
    curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca_blob);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "maniavita/1.0");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, lb_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    char why[96];
    if (r != CURLE_OK) {                         /* never reached the server */
        snprintf(why, sizeof why, "%s (certs %d ok, %d bad)",
                 cerr[0] ? cerr : curl_easy_strerror(r), lb_ca_ok, lb_ca_bad);
        lb_fail(why);
        return 0;
    }
    if (strncmp(out, "OK", 2) && strncmp(out, "ERR", 3)) {   /* got a web page, not our reply */
        char start[48];
        snprintf(start, sizeof start, "%s", out);
        for (char *p = start; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        snprintf(why, sizeof why, "HTTP %ld: %s", code, start);
        lb_fail(why);
        return 0;
    }
    return 1;
}

/* --- Download a map's top 50 (osu = path of the .osu file) --- */
static void lb_do_top(const char *osu, char *buf, size_t cap)
{
    LbRow rows[LB_MAX_ROWS];
    char md5[33], path[64];
    int n = 0, ok = 0;
    if (!lb_md5_file(osu, md5)) lb_fail("couldn't read the .osu file");
    else {
        snprintf(path, sizeof path, "/top?map=%s", md5);
        if (lb_http(path, NULL, buf, cap)) {
            ok = !strncmp(buf, "OK", 2);
            if (!ok) {
                char why[96];
                snprintf(why, sizeof why, "top list: %.60s", buf);
                why[strcspn(why, "\r\n")] = 0;
                lb_fail(why);
            }
        }
    }
    if (ok) {
        char *line = strchr(buf, '\n');
        while (line && n < LB_MAX_ROWS) {
            line++;
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            LbRow r;
            memset(&r, 0, sizeof r);
            if (sscanf(line, "%d\t%16[^\t]\t%d\t%f\t%d\t%47[^\t]\t%f\t%d",
                       &r.rank, r.name, &r.score, &r.acc, &r.combo,
                       r.mods, &r.pp, &r.miss) == 8) {
                if (!strcmp(r.mods, "-")) r.mods[0] = 0;
                rows[n++] = r;
            }
            line = end;
        }
    }
    SDL_LockMutex(lb_mx);
    if (!strcmp(lb_want_map, osu)) {   /* ignore if the player moved to another map */
        memcpy(lb_rows, rows, sizeof(LbRow) * n);
        lb_row_n = n;
        lb_state = ok ? 2 : 3;
        snprintf(lb_state_map, sizeof lb_state_map, "%s", osu);
    }
    SDL_UnlockMutex(lb_mx);
}

/* --- Download the player's profile (keys 0 = all key counts) --- */
static void lb_do_profile(int keys, char *buf, size_t cap)
{
    PfPlay plays[10];
    char path[128], name[17] = "", msg[96] = "";
    int n = 0, rank = 0, total = 0, st = 3;
    float pp = 0;
    char *en = curl_easy_escape(NULL, lb_name, 0);
    if (keys) snprintf(path, sizeof path, "/player?name=%s&keys=%d", en ? en : "", keys);
    else      snprintf(path, sizeof path, "/player?name=%s", en ? en : "");
    curl_free(en);

    if (!lb_http(path, NULL, buf, cap)) {
        SDL_LockMutex(lb_mx);
        snprintf(msg, sizeof msg, "%s", lb_err);
        SDL_UnlockMutex(lb_mx);
    } else if (!strncmp(buf, "ERR no player", 13)) {
        st = 4;
    } else if (!strncmp(buf, "OK ", 3) &&
               sscanf(buf + 3, "%16[^\t]\t%f\t%d\t%d", name, &pp, &rank, &total) == 4) {
        st = 2;
        char *line = strchr(buf, '\n');
        while (line && n < 10) {
            line++;
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            PfPlay p;
            memset(&p, 0, sizeof p);
            if (sscanf(line, "%199[^\t]\t%d\t%47[^\t]\t%f\t%f",
                       p.title, &p.keys, p.mods, &p.acc, &p.pp) == 5) {
                if (!strcmp(p.mods, "-")) p.mods[0] = 0;
                plays[n++] = p;
            }
            line = end;
        }
    } else {
        snprintf(msg, sizeof msg, "%.60s", buf);
        msg[strcspn(msg, "\r\n")] = 0;
        lb_fail(msg);
    }
    SDL_LockMutex(lb_mx);
    if (pf_want_keys == keys) {        /* ignore if the player switched key mode */
        memcpy(pf_plays, plays, sizeof(PfPlay) * n);
        pf_play_n = n;
        pf_rank = rank;
        pf_plays_total = total;
        pf_pp = pp;
        snprintf(pf_name, sizeof pf_name, "%s", name);
        snprintf(pf_msg, sizeof pf_msg, "%s", msg);
        pf_state = st;
    }
    SDL_UnlockMutex(lb_mx);
}

/* --- Offline queue (pending.txt, one score per line) --- */
static void lb_queue_add(const char *body)
{
    SDL_LockMutex(lb_mx);
    FILE *f = fopen(LB_PENDING, "a");
    if (f) { fprintf(f, "%s\n", body); fclose(f); }
    SDL_UnlockMutex(lb_mx);
    SDL_SemPost(lb_wake);
}

static int lb_queue_first(char *out, size_t cap)
{
    int got = 0;
    SDL_LockMutex(lb_mx);
    FILE *f = fopen(LB_PENDING, "r");
    if (f) {
        if (fgets(out, (int)cap, f)) { out[strcspn(out, "\r\n")] = 0; got = 1; }
        fclose(f);
    }
    SDL_UnlockMutex(lb_mx);
    return got;
}

static void lb_queue_pop(void)
{
    SDL_LockMutex(lb_mx);
    FILE *f = fopen(LB_PENDING, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *all = malloc(sz + 1);
        size_t n = all ? fread(all, 1, sz, f) : 0;
        fclose(f);
        if (all) {
            all[n] = 0;
            char *rest = strchr(all, '\n');
            rest = rest ? rest + 1 : all + n;
            if (*rest) {
                FILE *w = fopen(LB_PENDING, "wb");
                if (w) { fputs(rest, w); fclose(w); }
            } else {
                remove(LB_PENDING);
            }
            free(all);
        }
    }
    SDL_UnlockMutex(lb_mx);
}

/* --- Background thread --- */
static int lb_worker(void *arg)
{
    (void)arg;
    char *buf  = malloc(16384);
    char *body = malloc(2048);
    char *want = malloc(512);
    if (!buf || !body || !want) { free(buf); free(body); free(want); return 0; }
    Uint32 last_send = 0, pause_until = 0;

    while (!lb_quit_flag) {
        SDL_SemWaitTimeout(lb_wake, 1000);
        if (lb_quit_flag) break;

        /* 1. Song select (Global view) wants a map's top list */
        SDL_LockMutex(lb_mx);
        snprintf(want, 512, "%s", lb_want_map);
        int need = want[0] && lb_state == 1;
        SDL_UnlockMutex(lb_mx);
        if (need) lb_do_top(want, buf, 16384);

        /* 2. Profile tab wants the player's stats */
        SDL_LockMutex(lb_mx);
        int pkeys = pf_want_keys, pneed = pf_state == 1;
        SDL_UnlockMutex(lb_mx);
        if (pneed) lb_do_profile(pkeys, buf, 16384);

        /* 3. Send one waiting score (3 s apart) */
        Uint32 now = SDL_GetTicks();
        if (now < pause_until || now - last_send < 3200) continue;
        if (!lb_queue_first(body, 2048)) continue;
        if (!body[0]) { lb_queue_pop(); continue; }

        SDL_LockMutex(lb_mx);
        int is_last = !strcmp(body, lb_last_body);
        SDL_UnlockMutex(lb_mx);

        last_send = SDL_GetTicks();
        if (!lb_http("/submit", body, buf, 16384)) {
            if (lb_sub_state == 1) {                /* the newest score waits behind this one too */
                SDL_LockMutex(lb_mx);
                snprintf(lb_sub_msg, sizeof lb_sub_msg, "%s", lb_err);
                SDL_UnlockMutex(lb_mx);
                lb_sub_state = 3;                   /* saved, sent later */
            }
            pause_until = SDL_GetTicks() + 30000;
            continue;
        }
        if (!strncmp(buf, "ERR too fast", 12) || !strncmp(buf, "ERR server", 10)) {
            pause_until = SDL_GetTicks() + 10000;   /* try again soon */
            continue;
        }
        lb_queue_pop();
        SDL_LockMutex(lb_mx);
        if (is_last) {
            if (!strncmp(buf, "OK", 2)) {
                sscanf(buf + 2, "%d %d %f %f %d", &lb_sub_improved, &lb_sub_rank,
                       &lb_sub_pp, &lb_sub_total, &lb_sub_prank);
                lb_sub_state = 2;
            } else {
                snprintf(lb_sub_msg, sizeof lb_sub_msg, "%s", buf + 4);
                lb_sub_msg[strcspn(lb_sub_msg, "\r\n")] = 0;
                lb_sub_state = 4;
            }
        }
        if (lb_want_map[0]) lb_state = 1;           /* refresh the open top list */
        SDL_UnlockMutex(lb_mx);
    }
    free(buf);
    free(body);
    free(want);
    return 0;
}

/* --- Used by the rest of the game --- */
/* asks for a map's global top list (osu = path of the .osu file) */
static void lb_request_top(const char *osu)
{
    SDL_LockMutex(lb_mx);
    snprintf(lb_want_map, sizeof lb_want_map, "%s", osu);
    lb_state = 1;
    SDL_UnlockMutex(lb_mx);
    SDL_SemPost(lb_wake);
}

/* asks for the player's profile (keys 0 = all key counts) */
static void pf_request(int keys)
{
    SDL_LockMutex(lb_mx);
    pf_want_keys = keys;
    pf_state = 1;
    SDL_UnlockMutex(lb_mx);
    SDL_SemPost(lb_wake);
}

/* c[] = 320, 300, 200, 100, 50, miss.  mods = "HR,HD" style ("" for none) */
static void lb_submit(const char *osu_path, const char *title, int keys, float stars,
                      int score, int combo, const int c[6], const char *mods)
{
    if (strstr(mods, "AT")) return;                 /* never send autoplay */
    if (!lb_name_ok()) {
        snprintf(lb_sub_msg, sizeof lb_sub_msg, "set a player name in Settings");
        lb_sub_state = 4;
        return;
    }
    char md5[33];
    if (!lb_md5_file(osu_path, md5)) return;
    int notes = c[0] + c[1] + c[2] + c[3] + c[4] + c[5];
    char t[151];
    snprintf(t, sizeof t, "%s", title);
    char *et = curl_easy_escape(NULL, t, 0);
    char *en = curl_easy_escape(NULL, lb_name, 0);
    char *em = curl_easy_escape(NULL, mods, 0);
    char body[2048];
    snprintf(body, sizeof body,
        "name=%s&token=%s&map=%s&title=%s&keys=%d&notes=%d&stars=%.2f"
        "&score=%d&combo=%d&c320=%d&c300=%d&c200=%d&c100=%d&c50=%d&c0=%d&mods=%s",
        en ? en : "", lb_token, md5, et ? et : "", keys, notes, stars,
        score, combo, c[0], c[1], c[2], c[3], c[4], c[5], em ? em : "");
    curl_free(et); curl_free(en); curl_free(em);
    SDL_LockMutex(lb_mx);
    snprintf(lb_last_body, sizeof lb_last_body, "%s", body);
    lb_sub_state = 1;
    SDL_UnlockMutex(lb_mx);
    lb_queue_add(body);
}

static void lb_init(void)
{
    lb_load_player();
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static void *net_mem = NULL;
    net_mem = malloc(1 << 20);
    SceNetInitParam np;
    np.memory = net_mem;
    np.size = 1 << 20;
    np.flags = 0;
    if (sceNetInit(&np) < 0) { free(net_mem); net_mem = NULL; }  /* already on: fine */
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_ALL);
    lb_mx = SDL_CreateMutex();
    lb_wake = SDL_CreateSemaphore(0);
    lb_thread = SDL_CreateThreadWithStackSize(lb_worker, "lb", 256 * 1024, NULL);
}

static void lb_quit(void)
{
    lb_quit_flag = 1;
    if (lb_wake) SDL_SemPost(lb_wake);
    if (lb_thread) SDL_DetachThread(lb_thread);
}
/* ===== End of online leaderboard ===== */

/* ================= Gameplay ================= */
typedef struct { int t, end, col, hold, state; } Note;
enum { N_PENDING, N_HOLDING, N_DONE };

static Note *notes = NULL;
static int note_n = 0, first_live = 0, last_note_time = 0;
static int play_map = -1, play_keys = 4;
static unsigned play_mods = 0;           /* mods used by this play */
static int play_auto = 0;                /* AT or CM: the game presses the keys */
static int auto_up_at[MAX_KEYS];         /* song time when autoplay lets go of a key */
static float hitwin[6];
static int cnt[6], combo, max_combo;
static int col_down[MAX_KEYS];
static int last_j = -1;
static Uint32 last_j_time;
static Uint32 play_t0, pause_tick;
static int music_started, paused, pause_sel, pause_confirm;
static Uint32 resume_at;                 /* != 0 while the 3-2-1 countdown runs */
#define RESUME_COUNTDOWN_MS 1500
enum { P_RESUME, P_RETRY, P_SPEED, P_OFFSET, P_QUIT, P_COUNT };
static Mix_Music *play_music = NULL;

/* health: 0.0 (failed) to 1.0 (full) */
static float health = 1.0f, hp_gain = 0.01f, hp_loss = 0.06f;
static int fail_pending = 0;
static const char *fail_reason = "";

static int res_score, res_combo, res_n[6], res_rank, res_progress;
static unsigned res_mods;
static float res_acc, res_health;

static int lay_x[MAX_KEYS], lay_w[MAX_KEYS], lay_x0, lay_total, lay_hit_y, lay_judge_y;

/* ---- HD / FI / FL: how much of the lanes is covered (grows with combo) ---- */
#define COVER_MIN   0.20f      /* at 0 combo */
#define COVER_MAX   0.62f      /* at COVER_COMBO combo or more */
#define COVER_COMBO 400
static float  cover_frac = COVER_MIN;
static Uint32 cover_tick = 0;

static void cover_update(void) {
    int c = combo < COVER_COMBO ? combo : COVER_COMBO;
    float target = COVER_MIN + (COVER_MAX - COVER_MIN) * (float)c / COVER_COMBO;
    Uint32 t = SDL_GetTicks();
    float dt = cover_tick ? (t - cover_tick) / 1000.0f : 0.0f;
    cover_tick = t;
    float k = (dt > 0.1f ? 0.1f : dt) * 4.0f;        /* smooth slide (about 1/4 second) */
    cover_frac += (target - cover_frac) * (k > 1.0f ? 1.0f : k);
}

/* ---- Hit error meter data (drawn by draw_hit_error) ---- */
#define HE_HITS    32
#define HE_FADE_MS 3000
static int    he_ofs[HE_HITS];          /* ms: negative = early, positive = late */
static Uint32 he_time[HE_HITS];
static int    he_next = 0, he_n = 0, he_last = 0;
static float  he_avg = 0.0f;

static void he_reset(void) { he_next = he_n = he_last = 0; he_avg = 0.0f; }

static void he_add(int d) {
    he_ofs[he_next] = d;
    he_time[he_next] = SDL_GetTicks();
    he_next = (he_next + 1) % HE_HITS;
    if (he_n < HE_HITS) he_n++;
    he_last = d;
    he_avg = he_n == 1 ? (float)d : he_avg * 0.8f + d * 0.2f;
}

static int cmp_note(const void *a, const void *b) {
    const Note *A = a, *B = b;
    if (A->t != B->t) return A->t - B->t;
    return A->col - B->col;
}

static int load_notes(const MapInfo *m) {
    free(notes);
    notes = NULL; note_n = 0;
    int cap = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", m->folder, m->file);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int in_ho = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '[') { in_ho = !strcmp(line, "[HitObjects]"); continue; }
        if (!in_ho || !line[0]) continue;
        int x, y, t, type;
        if (sscanf(line, "%d,%d,%d,%d", &x, &y, &t, &type) != 4) continue;
        Note n;
        memset(&n, 0, sizeof n);
        n.col = clampi(x * m->keys / 512, 0, m->keys - 1);
        n.t = n.end = t;
        n.hold = (type & 128) != 0;
        if (n.hold) {
            const char *p = line;
            for (int k = 0; k < 5 && p; k++) { p = strchr(p, ','); if (p) p++; }
            if (p) n.end = atoi(p);
            if (n.end <= n.t) { n.hold = 0; n.end = n.t; }
        }
        if (note_n == cap) {
            int nc = cap ? cap * 2 : 512;
            Note *p = realloc(notes, nc * sizeof *p);
            if (!p) break;
            notes = p; cap = nc;
        }
        notes[note_n++] = n;
    }
    fclose(f);
    qsort(notes, note_n, sizeof(Note), cmp_note);
    return note_n > 0;
}

/* Mirror flips the lanes, Random shuffles them (same shuffle for the whole song) */
static void apply_lane_mods(int keys) {
    int mr = mod_on(play_mods, M_MR), rd = mod_on(play_mods, M_RD);
    if (!mr && !rd) return;
    int perm[MAX_KEYS];
    for (int c = 0; c < keys; c++) perm[c] = mr ? keys - 1 - c : c;
    if (rd && keys > 1) {
        srand((unsigned)time(NULL) ^ (unsigned)SDL_GetTicks());
        int same = 1;
        for (int tries = 0; tries < 8 && same; tries++) {     /* avoid "no change" */
            for (int c = keys - 1; c > 0; c--) {
                int r = rand() % (c + 1), t = perm[c];
                perm[c] = perm[r]; perm[r] = t;
            }
            same = 1;
            for (int c = 0; c < keys; c++) if (perm[c] != c) same = 0;
        }
    }
    for (int i = 0; i < note_n; i++) notes[i].col = perm[clampi(notes[i].col, 0, keys - 1)];
    qsort(notes, note_n, sizeof(Note), cmp_note);
}

static void compute_layout(int keys) {
    int k = kidx(keys);
    float s = SCREEN_H / 480.0f;
    lay_total = 0;
    for (int c = 0; c < keys; c++) {
        int w = opt_lane_w;
        if (opt_skin_layout && skin.has_col_w[k] && skin.col_w[k][c] > 0)
            w = (int)(skin.col_w[k][c] * s + 0.5f);
        lay_w[c] = w;
        lay_total += w;
    }
    lay_x0 = (SCREEN_W - lay_total) / 2;
    int x = lay_x0;
    for (int c = 0; c < keys; c++) { lay_x[c] = x; x += lay_w[c]; }

    lay_hit_y = opt_hit_y;
    if (opt_skin_layout && skin.hit_pos[k] > 0)
        lay_hit_y = clampi((int)(skin.hit_pos[k] * s), 200, SCREEN_H - 10);
    lay_judge_y = lay_hit_y - 170;
    if (opt_skin_layout && skin.score_pos[k] > 0) lay_judge_y = (int)(skin.score_pos[k] * s);
    lay_judge_y = clampi(lay_judge_y, 60, SCREEN_H - 60);
}

static int song_time(void) {
    /* before the music starts (or no music): use the system timer */
    if (!music_started || !play_music) {
        Uint32 tk = paused ? pause_tick : SDL_GetTicks();
        return (int)((Sint64)tk - (Sint64)play_t0) - opt_offset;
    }
    if (paused) return (int)clk - opt_offset;

    /* smooth timer that is constantly pulled toward the real audio position */
    Uint32 now = SDL_GetTicks();
    if (now != clk_tick) {
        clk += (double)(now - clk_tick);
        clk_tick = now;
        double diff = audio_ms() - clk;
        if (fabs(diff) > 60.0) clk += diff;          /* big jump: snap */
        else                   clk += diff * 0.1;    /* small drift: ease */
    }
    return (int)clk - opt_offset;
}

static float calc_acc(void) {
    int total = cnt[0] + cnt[1] + cnt[2] + cnt[3] + cnt[4] + cnt[5];
    if (!total) return 100.0f;
    double pts = 300.0 * (cnt[J_MAX] + cnt[J_300]) + 200.0 * cnt[J_200]
               + 100.0 * cnt[J_100] + 50.0 * cnt[J_50];
    return (float)(pts / (300.0 * total) * 100.0);
}

static int judge_from(int diff) {
    for (int j = J_MAX; j <= J_50; j++) if (diff <= hitwin[j]) return j;
    return J_MISS;
}

/* ends the play on the next update (safe to call from anywhere) */
static void fail_now(const char *why) {
    if (fail_pending) return;
    fail_pending = 1;
    fail_reason = why;
}

static void hp_add(float d) {
    health += d;
    if (health > 1.0f) health = 1.0f;
    if (health < 0.0f) health = 0.0f;
    if (health <= 0.0f && !(play_mods & MODS_NOFAIL)) fail_now("Your health ran out");
}

static void apply_judgement(int j) {
    /* MAX / 300 / 200 refill health, 100 does nothing, 50 / MISS drain it */
    static const float gain[6] = { 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f };
    static const float loss[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.4f, 1.0f };

    cnt[j]++;
    score_add_judgement(j);
    if (j == J_MISS) { combo = 0; sfx_play_miss(); }
    else if (++combo > max_combo) max_combo = combo;
    last_j = j;
    last_j_time = SDL_GetTicks();

    hp_add(hp_gain * gain[j] - hp_loss * loss[j]);
    if (mod_on(play_mods, M_SD) && j == J_MISS) fail_now("Sudden Death: you missed a note");
    if (mod_on(play_mods, M_PF) && j > J_300)   fail_now("Perfect: you hit less than a 300");
}

static void press_col(int c) {
    col_down[c] = 1;
    sfx_play_hit();
    int now = song_time();
    for (int i = first_live; i < note_n; i++) {
        Note *n = &notes[i];
        if (n->col != c || n->state != N_PENDING) continue;
        int d = now - n->t;
        if (d < -hitwin[J_MISS]) break;
        int j = judge_from(abs(d));
        if (j != J_MISS) he_add(d);
        apply_judgement(j);
        if (n->hold && j != J_MISS) n->state = N_HOLDING;
        else {
            if (n->hold) apply_judgement(J_MISS);
            n->state = N_DONE;
        }
        break;
    }
}

static void release_col(int c) {
    col_down[c] = 0;
    int now = song_time();
    for (int i = first_live; i < note_n; i++) {
        Note *n = &notes[i];
        if (n->col != c || n->state != N_HOLDING) continue;
        int early = n->end - now;
        apply_judgement(early <= 0 ? J_MAX : judge_from(early));
        n->state = N_DONE;
        break;
    }
}

static void update_notes(int now) {
    for (int i = first_live; i < note_n; i++) {
        Note *n = &notes[i];
        if (n->t > now) break;
        if (n->state == N_PENDING && now - n->t > hitwin[J_50]) {
            apply_judgement(J_MISS);
            if (n->hold) apply_judgement(J_MISS);
            n->state = N_DONE;
        } else if (n->state == N_HOLDING && now >= n->end) {
            apply_judgement(J_MAX);
            n->state = N_DONE;
        }
    }
    while (first_live < note_n && notes[first_live].state == N_DONE) first_live++;
}

/* Autoplay: hits every note perfectly and holds long notes to the end */
static void autoplay_update(int now) {
    for (int c = 0; c < play_keys; c++)
        if (col_down[c] && now >= auto_up_at[c]) col_down[c] = 0;

    for (int i = first_live; i < note_n; i++) {
        Note *n = &notes[i];
        if (n->t > now) break;
        if (n->state != N_PENDING) continue;
        int c = n->col;
        apply_judgement(J_MAX);
        col_down[c] = 1;
        sfx_play_hit();
        if (n->hold) { n->state = N_HOLDING; auto_up_at[c] = n->end; }
        else         { n->state = N_DONE;    auto_up_at[c] = n->t + 80; }
    }
}

/* ================= Lanes held by buttons / touch ================= */
#define TOUCH_MAX 10
static SDL_FingerID tf_id[TOUCH_MAX];
static int tf_lane[TOUCH_MAX] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int touch_n[MAX_KEYS];             /* front-touch fingers on each lane */

/* the lane the rear touchpad hits: the middle (special) lane in 1K, 3K and 5K */
static int rear_lane(void) {
    if (!opt_rear) return -1;
    for (int c = 0; c < play_keys; c++) if (col_type(play_keys, c) == 3) return c;
    return -1;
}

static int input_held(int c) {
    int k = kidx(play_keys);
    if (btn_held(binds[k][c])) return 1;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (kb_keys[k][c] && ks[SDL_GetScancodeFromKey(kb_keys[k][c])]) return 1;
    if (c == rear_lane() && rear_fingers > 0) return 1;
    if (touch_n[c] > 0) return 1;
    return 0;
}

/* a button / finger went down or up on lane c (handles several inputs on one lane) */
static void lane_down(int c) {
    if (c < 0 || c >= play_keys || play_auto || paused) return;
    if (!col_down[c]) press_col(c);
}
static void lane_up(int c) {
    if (c < 0 || c >= play_keys || play_auto || paused) return;
    if (col_down[c] && !input_held(c)) release_col(c);
}

static void touch_reset(void) {
    for (int i = 0; i < TOUCH_MAX; i++) tf_lane[i] = -1;
    memset(touch_n, 0, sizeof touch_n);
}

/* screen x (0.0 - 1.0) -> lane; outside the playfield = the nearest lane */
static int touch_lane_at(float x) {
    int px = (int)(x * SCREEN_W);
    for (int c = 0; c < play_keys - 1; c++)
        if (px < lay_x[c] + lay_w[c]) return c;
    return play_keys - 1;
}

static int touch_slot(SDL_FingerID id) {
    for (int i = 0; i < TOUCH_MAX; i++) if (tf_lane[i] >= 0 && tf_id[i] == id) return i;
    return -1;
}

static void play_touch_down(SDL_FingerID id, float x) {
    if (!opt_touch || play_auto || paused || touch_slot(id) >= 0) return;
    for (int i = 0; i < TOUCH_MAX; i++) {
        if (tf_lane[i] >= 0) continue;
        int c = touch_lane_at(x);
        tf_id[i] = id; tf_lane[i] = c; touch_n[c]++;
        lane_down(c);
        return;
    }
}

static void play_touch_move(SDL_FingerID id, float x) {
    int s = touch_slot(id);
    if (s < 0) return;
    int c = touch_lane_at(x), old = tf_lane[s];
    if (c == old) return;
    touch_n[old]--; tf_lane[s] = c; touch_n[c]++;
    lane_up(old);
    lane_down(c);
}

static void play_touch_up(SDL_FingerID id) {
    int s = touch_slot(id);
    if (s < 0) return;
    int c = tf_lane[s];
    tf_lane[s] = -1;
    if (touch_n[c] > 0) touch_n[c]--;
    lane_up(c);
}

static void play_rear_down(void) { rear_fingers++; lane_down(rear_lane()); }
static void play_rear_up(void)   { if (rear_fingers > 0) rear_fingers--; lane_up(rear_lane()); }

static void play_cleanup(void) {
    au_running = 0;
    Mix_HaltMusic();
    if (play_music) { Mix_FreeMusic(play_music); play_music = NULL; }
    free(play_music_mem);
    play_music_mem = NULL;
    free(notes);
    notes = NULL; note_n = 0;
    video_close();
    touch_reset();
    paused = 0;
    resume_at = 0;
}

static char status_msg[160];
static Uint32 status_until;
static int screen;
static int rep_btn = -1, rep_action;
static Uint32 rep_next;

enum { SCR_SELECT, SCR_SETTINGS, SCR_BINDS, SCR_PLAY, SCR_RESULTS, SCR_SCORES, SCR_FAILED };

static void set_status(const char *msg) {
    snprintf(status_msg, sizeof status_msg, "%s", msg);
    status_until = SDL_GetTicks() + 3000;
}

static void play_warmup(void);
static void load_beats(const MapInfo *m);   /* written further down */

static void ig_reset(void);
static void start_map(int idx) {
    if (idx < 0) return;
    play_cleanup();
    preview_stop();
    draw_loading("Loading...");

    const MapInfo *m = &maps[idx];
    play_mods = mods_clean(opt_mods);
    if (!load_notes(m)) { set_status("Couldn't load this map"); screen = SCR_SELECT; return; }
    apply_lane_mods(m->keys);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", m->folder, m->audio);
    play_music = load_music_ram(path);
    if (!play_music) set_status("Audio file not found - playing without music");

    bg_load_now(idx);
    if (video_open(m) < 0) set_status("Couldn't play this map's video - showing the background");
    play_map = idx;
    play_keys = m->keys;
    play_auto = (play_mods & MODS_AUTO) != 0;
    compute_layout(play_keys);

    int total = 0;
    last_note_time = 0;
    for (int i = 0; i < note_n; i++) {
        total += notes[i].hold ? 2 : 1;
        if (notes[i].end > last_note_time) last_note_time = notes[i].end;
    }
    score_reset(total, mods_multiplier(play_mods));
    memset(cnt, 0, sizeof cnt);
    memset(col_down, 0, sizeof col_down);
    memset(auto_up_at, 0, sizeof auto_up_at);
    combo = max_combo = 0;
    load_beats(&maps[idx]);
    he_reset();
    ig_reset();
    touch_reset();
    rear_fingers = 0;
    cover_frac = COVER_MIN;
    cover_tick = 0;

    first_live = 0;
    last_j = -1;

    /* timing windows (EZ = wider, HR = tighter) */
    float od = m->od;
    hitwin[J_MAX]  = 16.0f;
    hitwin[J_300]  = 64.0f - 3.0f * od;
    hitwin[J_200]  = 97.0f - 3.0f * od;
    hitwin[J_100]  = 127.0f - 3.0f * od;
    hitwin[J_50]   = 151.0f - 3.0f * od;
    hitwin[J_MISS] = 188.0f - 3.0f * od;
    float ws = mod_on(play_mods, M_EZ) ? 1.4f : mod_on(play_mods, M_HR) ? 1.0f / 1.4f : 1.0f;
    for (int j = 0; j < 6; j++) hitwin[j] *= ws;

    /* health: the map's HP Drain (0-10) decides how much you gain and lose */
    float hp = m->hp;
    hp_loss = 0.03f + 0.007f * hp;      /* one miss: 3% (HP 0) up to 10% (HP 10) */
    hp_gain = 0.012f - 0.0006f * hp;    /* one MAX/300: 1.2% down to 0.6% */
    if (mod_on(play_mods, M_EZ)) hp_loss *= 0.5f;
    if (mod_on(play_mods, M_HR)) hp_loss *= 1.4f;
    health = 1.0f;
    fail_pending = 0;
    fail_reason = "";

    play_warmup();

    play_t0 = SDL_GetTicks() + opt_delay;
    music_started = 0;
    paused = 0;
    resume_at = 0;
    clk = 0.0;
    rep_btn = -1;
    screen = SCR_PLAY;
}

static void finish_map(void) {
    res_score    = score_current();
    res_acc      = calc_acc();
    res_combo    = max_combo;
    res_mods     = play_mods;
    res_health   = health;
    res_progress = 100;
    memcpy(res_n, cnt, sizeof res_n);
    res_rank = (play_mods & MODS_UNSAVED) ? 0
             : scores_add(&maps[play_map], res_score, res_acc, res_combo, cnt, play_mods);
    lb_sub_state = 0;
    if (!(play_mods & MODS_UNSAVED)) {                 /* never RD / AT / CM */
        const MapInfo *om = &maps[play_map];
        char osu[512], otitle[400], omods[64];
        snprintf(osu, sizeof osu, "%s/%s", om->folder, om->file);
        snprintf(otitle, sizeof otitle, "%s - %s [%s]", om->artist, om->title, om->version);
        mods_string(play_mods, omods, sizeof omods);
        for (char *p = omods; *p; p++) if (*p == ' ') *p = ',';   /* "HR HD" -> "HR,HD" */
        lb_submit(osu, otitle, om->keys, om->stars, res_score, res_combo, cnt, omods);
    }

    play_cleanup();
    screen = SCR_RESULTS;
}

static void fail_map(void) {
    int now = song_time();
    res_score    = score_current();
    res_acc      = calc_acc();
    res_combo    = max_combo;
    res_mods     = play_mods;
    res_health   = 0.0f;
    res_rank     = 0;
    res_progress = last_note_time > 0 ? clampi(now * 100 / last_note_time, 0, 100) : 0;
    memcpy(res_n, cnt, sizeof res_n);
    play_cleanup();
    fail_pending = 0;
    rep_btn = -1;
    screen = SCR_FAILED;
}

static void pause_game(void) {
    if (paused) { resume_at = 0; return; }   /* paused during countdown: back to menu */
    (void)song_time();                        /* freeze the clock at the current position */
    paused = 1;
    au_running = 0;
    resume_at = 0;
    pause_confirm = 0;
    pause_sel = P_RESUME;
    pause_tick = SDL_GetTicks();
    Mix_PauseMusic();
    video_pause(1);
}

/* actually continues the song */
static void resume_now(void) {
    play_t0 += SDL_GetTicks() - pause_tick;
    paused = 0;
    resume_at = 0;
    clk_tick = SDL_GetTicks();
    cover_tick = 0;
    if (music_started && play_music) { Mix_ResumeMusic(); au_running = 1; }
    video_pause(0);
    if (!play_auto)
        for (int c = 0; c < play_keys; c++)
            if (col_down[c] && !input_held(c)) release_col(c);
    rep_btn = -1;
}

/* starts the 3-2-1 countdown */
static void resume_game(void) {
    if (!paused || resume_at) return;
    if (song_time() < 0) { resume_now(); return; }   /* still in the start delay */
    resume_at = SDL_GetTicks() + RESUME_COUNTDOWN_MS;
}

/* ================= Hold note combo ticks (like osu!stable) ================= */
#define HOLD_TICKS_PER_BEAT 4      /* combo +1 every 1/4 beat while holding */

typedef struct { int t; double beat; } BeatPoint;
static BeatPoint *beats = NULL;
static int beat_n = 0;
static int ht_note[MAX_KEYS], ht_next[MAX_KEYS];

/* reads the song's BPM changes from the .osu file */
static void load_beats(const MapInfo *m) {
    free(beats);
    beats = NULL; beat_n = 0;
    for (int c = 0; c < MAX_KEYS; c++) ht_note[c] = -1;

    char path[512], line[1024];
    snprintf(path, sizeof path, "%s/%s", m->folder, m->file);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int cap = 0, in_tp = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '[') { in_tp = !strcmp(line, "[TimingPoints]"); continue; }
        if (!in_tp || !line[0]) continue;
        double t, bl;
        if (sscanf(line, "%lf,%lf", &t, &bl) != 2 || bl <= 0) continue;   /* skip green lines */
        if (beat_n == cap) {
            int nc = cap ? cap * 2 : 16;
            BeatPoint *p = realloc(beats, nc * sizeof *p);
            if (!p) break;
            beats = p; cap = nc;
        }
        beats[beat_n].t = (int)t;
        beats[beat_n].beat = bl;
        beat_n++;
    }
    fclose(f);
}

/* time between combo ticks at song time t */
static int hold_tick_len(int t) {
    double bl = beat_n ? beats[0].beat : 500.0;     /* no timing info: 120 BPM */
    for (int i = 0; i < beat_n && beats[i].t <= t; i++) bl = beats[i].beat;
    int step = (int)(bl / HOLD_TICKS_PER_BEAT);
    return step < 10 ? 10 : step;
}

/* adds combo while long notes are being held */
static void hold_ticks_update(int now) {
    for (int i = first_live; i < note_n; i++) {
        Note *n = &notes[i];
        if (n->t > now) break;
        if (!n->hold || n->state != N_HOLDING) continue;
        int c = n->col;
        int step = hold_tick_len(n->t);
        if (ht_note[c] != i) { ht_note[c] = i; ht_next[c] = n->t + step; }
        while (ht_next[c] <= now && ht_next[c] < n->end) {
            if (++combo > max_combo) max_combo = combo;
            ht_next[c] += step;
        }
    }
}

static void update_play(void) {
    if (fail_pending) { fail_map(); return; }
    if (paused) {
        if (!resume_at || !SDL_TICKS_PASSED(SDL_GetTicks(), resume_at)) return;
        resume_now();                     /* countdown finished */
    }
    if (!music_started && SDL_TICKS_PASSED(SDL_GetTicks(), play_t0)) {
        au_running = 0;
        SDL_AtomicSet(&au_frames, 0);
        if (play_music && Mix_PlayMusic(play_music, 0) == 0) {
            au_running = 1;
        } else if (play_music) {                 /* failed: fall back to timer */
            Mix_FreeMusic(play_music); play_music = NULL;
            free(play_music_mem); play_music_mem = NULL;
        }
        play_t0 = SDL_GetTicks();
        clk = 0.0;
        clk_tick = play_t0;
        music_started = 1;
    }
    int now = song_time();
    if (play_auto) autoplay_update(now);
    hold_ticks_update(now);
    update_notes(now);
    cover_update();
    if (video_update(now) < 0) set_status("Video is bigger than 720p - showing the background");
    if (fail_pending) { fail_map(); return; }
    if (first_live >= note_n && now > last_note_time + 1500) finish_map();
}

/* ================= Drawing: gameplay ================= */
/* draws a skin image tinted with the chosen note colour */
static void copy_tinted(SDL_Texture *t, int c, const SDL_Rect *r) {
    Uint8 cr, cg, cb;
    int tint = note_rgb(play_keys, c, &cr, &cg, &cb);
    if (tint) SDL_SetTextureColorMod(t, cr, cg, cb);
    SDL_RenderCopy(ren, t, NULL, r);
    if (tint) SDL_SetTextureColorMod(t, 255, 255, 255);
}

/* lane colour when the skin has no image */
static void col_color(int keys, int c, Uint8 *r, Uint8 *g, Uint8 *b) {
    switch (col_type(keys, c)) {
    case 1:  *r = 230; *g = 230; *b = 240; break;
    case 2:  *r =  90; *g = 170; *b = 255; break;
    default: *r = 255; *g = 200; *b =  80; break;
    }
    note_rgb(keys, c, r, g, b);   /* use the chosen note colour (unless "Skin") */
}

static void draw_note_img(Anim *a, Anim *fallback, int c, int y) {
    int x = lay_x[c], w = lay_w[c];
    SDL_Texture *t = anim_frame(a);
    if (!t) { a = fallback; t = anim_frame(a); }
    if (t && a->w > 0) {
        int h = a->h * w / a->w;
        SDL_Rect r = { x, y - h, w, h };
        copy_tinted(t, c, &r);
    } else {
        Uint8 cr, cg, cb;
        col_color(play_keys, c, &cr, &cg, &cb);
        fill(x + 2, y - 18, w - 4, 18, cr, cg, cb, 255);
    }
}

static const char *jname[6] = { "MAX", "300", "200", "100", "50", "MISS" };
static const SDL_Color jcol[6] = {
    {255, 255, 210, 255}, {255, 220, 80, 255}, {120, 230, 120, 255},
    { 90, 160, 255, 255}, {180, 180, 180, 255}, {255, 80, 80, 255}
};
static const SDL_Color C_EARLY = {120, 200, 255, 255}, C_LATE = {255, 150, 90, 255};

/* creates the text images before the song starts, so there's no hitch on the first hits */
static void play_warmup(void) {
    int w, h, tmp;
    glyph_warm(font_big, C_WHITE);
    glyph_warm(font_small, C_GREY);
    for (int j = 0; j < 6; j++) text_tex(font_big, jname[j], jcol[j], &w, &h, &tmp);
    text_tex(font_small, "EARLY", C_EARLY, &w, &h, &tmp);
    text_tex(font_small, "LATE",  C_LATE,  &w, &h, &tmp);
}

/* ---- Hidden / Fade In / Flashlight lane covers ---- */
static unsigned field_mods = 0;     /* only set while drawing gameplay (not the preview) */

/* black cover over the lanes from y0 to y1; the soft edge is at y0 (soft_top) or y1 */
static void lane_cover(int y0, int y1, int fade, int soft_top) {
    if (y1 <= y0) return;
    if (fade > y1 - y0) fade = y1 - y0;
    if (fade < 1) fade = 1;
    int s0 = soft_top ? y0 + fade : y0;
    int s1 = soft_top ? y1 : y1 - fade;
    if (s1 > s0) fill(lay_x0, s0, lay_total, s1 - s0, 0, 0, 0, 255);
    for (int i = 0; i < fade; i += 4) {
        int h = fade - i < 4 ? fade - i : 4;
        int a = 255 * (i + 2) / fade;
        int y = soft_top ? y0 + i : y1 - i - h;
        fill(lay_x0, y, lay_total, h, 0, 0, 0, (Uint8)(a > 255 ? 255 : a));
    }
}

/* covers grow with combo (cover_frac: 0.20 at 0 combo up to 0.62 at 400 combo) */
static void draw_covers(void) {
    int H = lay_hit_y;
    int fade = H / 8;
    int cov = (int)(H * cover_frac);
    if (mod_on(field_mods, M_HD)) lane_cover(H - cov, H, fade, 1);       /* from the hit line up */
    if (mod_on(field_mods, M_FI)) lane_cover(0, cov, fade, 0);           /* from the top down */
    if (mod_on(field_mods, M_FL)) {                                      /* both sides, middle visible */
        lane_cover(0, cov / 2, fade / 2, 0);
        lane_cover(H - cov / 2, H, fade / 2, 1);
    }
}

/* vertical health bar on the right of the playfield */
static void draw_health_bar(float h) {
    int top = 40, bot = lay_hit_y, H = bot - top;
    int x = lay_x0 + lay_total + 6;
    if (H <= 0) return;
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    fill(x, top, 10, H, 0, 0, 0, 170);
    int fh = (int)(H * h);
    if (h > 0.5f)       fill(x + 2, bot - fh, 6, fh,  90, 220, 120, 230);
    else if (h > 0.25f) fill(x + 2, bot - fh, 6, fh, 255, 210,  90, 230);
    else                fill(x + 2, bot - fh, 6, fh, 255,  90,  90, 230);
}

/* Draws lanes, notes and keys using the current layout (lay_*) and play_keys.
   Shared by gameplay and the Settings preview. */
static void draw_field(const Note *ns, int count, int first, int now, const int *down) {
    int k = kidx(play_keys);

    fill(lay_x0, 0, lay_total, SCREEN_H, 0, 0, 0, 180);
    for (int c = 0; c < play_keys; c++) {
        if (down[c]) fill(lay_x[c], 0, lay_w[c], lay_hit_y, 255, 255, 255, 22);
        if (c > 0) fill(lay_x[c], 0, 1, SCREEN_H, 255, 255, 255, 40);
    }
    fill(lay_x0, lay_hit_y, lay_total, 3, 255, 255, 255, 200);

    /* notes */
    float pxms = (float)lay_hit_y / (11485.0f / opt_speed);
    for (int i = first; i < count; i++) {
        const Note *n = &ns[i];
        if (n->state == N_DONE) continue;
        int yh = lay_hit_y - (int)((n->t - now) * pxms);
        if (yh < -100) break;
        int c = n->col;
        if (c < 0 || c >= play_keys) continue;
        if (n->hold) {
            int ye = lay_hit_y - (int)((n->end - now) * pxms);
            if (n->state == N_HOLDING) yh = lay_hit_y;
            if (ye < -100) ye = -100;
            if (yh > ye) {
                SDL_Texture *bt = anim_frame(skin.body[k][c]);
                if (bt) {
                    SDL_Rect r = { lay_x[c], ye, lay_w[c], yh - ye };
                    copy_tinted(bt, c, &r);
                } else {
                    Uint8 cr, cg, cb;
                    col_color(play_keys, c, &cr, &cg, &cb);
                    fill(lay_x[c] + lay_w[c] / 6, ye, lay_w[c] - lay_w[c] / 3, yh - ye,
                         cr, cg, cb, n->state == N_HOLDING ? 220 : 150);
                }
            }
            draw_note_img(skin.head[k][c], skin.note[k][c], c, yh);
        } else {
            draw_note_img(skin.note[k][c], &anim_none, c, yh);
        }
    }

    /* HD / FI / FL covers (over the notes, under the keys) */
    if (field_mods) draw_covers();

    /* keys */
    for (int c = 0; c < play_keys; c++) {
        Anim *a = down[c] && skin.keyd[k][c]->n ? skin.keyd[k][c] : skin.key[k][c];
        SDL_Texture *t = anim_frame(a);
        if (t && a->w > 0) {
            int h = a->h * lay_w[c] / a->w;
            SDL_Rect r = { lay_x[c], SCREEN_H - h, lay_w[c], h };
            SDL_RenderCopy(ren, t, NULL, &r);
        } else {
            if (down[c]) fill(lay_x[c] + 2, lay_hit_y + 6, lay_w[c] - 4, SCREEN_H - lay_hit_y - 10, 200, 200, 255, 200);
            else         fill(lay_x[c] + 2, lay_hit_y + 6, lay_w[c] - 4, SCREEN_H - lay_hit_y - 10, 60, 60, 80, 200);
        }
    }
}

/* ---- Hit error meter ---- */
#define HE_Y      -1       /* -1 = bottom of the screen, or a fixed height like 500 */
#define HE_PX_MS  1.0f     /* pixels per ms (wider with EZ, narrower with HR) */
#define HE_H      14

static void draw_hit_error(int cx) {
    if (!opt_hit_error) return;
    int y = HE_Y >= 0 ? HE_Y : SCREEN_H - HE_H - 8;   /* bottom of the screen */
    float half = hitwin[J_50] * HE_PX_MS;
    if (half > SCREEN_W / 2 - 10) half = SCREEN_W / 2 - 10;
    if (half < 20.0f) half = 20.0f;
    float s = half / hitwin[J_50];

    /* timing zones, widest (50) first */
    fill(cx - (int)half - 2, y - 2, (int)half * 2 + 4, HE_H + 4, 0, 0, 0, 120);
    for (int j = J_50; j >= J_MAX; j--) {
        int w = (int)(hitwin[j] * s);
        fill(cx - w, y + 4, w * 2, HE_H - 8, jcol[j].r, jcol[j].g, jcol[j].b, 130);
    }
    fill(cx - 1, y - 2, 2, HE_H + 4, 255, 255, 255, 230);           /* centre line */

    /* one tick per hit, fading out */
    Uint32 t = SDL_GetTicks();
    for (int i = 0; i < he_n; i++) {
        Uint32 age = t - he_time[i];
        if (age >= HE_FADE_MS) continue;
        int d = he_ofs[i];
        int x = cx + (int)(d * s);
        if (x < cx - (int)half) x = cx - (int)half;
        if (x > cx + (int)half) x = cx + (int)half;
        int j = judge_from(abs(d));
        Uint8 a = (Uint8)(230 * (HE_FADE_MS - age) / HE_FADE_MS);
        fill(x - 1, y - 3, 3, HE_H + 6, jcol[j].r, jcol[j].g, jcol[j].b, a);
    }
    if (he_n <= 0) return;

    /* average marker above the bar */
    int ax = cx + (int)(he_avg * s);
    fill(ax - 4, y - 9, 9, 4, 255, 255, 255, 220);
}

/* where the combo + judgement go: centre of the lanes, or left of the playfield */
static int judge_x(int cx, int *align) {
    if (opt_judge_left && lay_x0 >= 80) { *align = 2; return lay_x0 - 16; }
    *align = 1;
    return cx;
}

/* EARLY / LATE for the newest hit, just above the combo (not shown for MAX) */
static void draw_early_late(int x, int align) {
    if (!opt_hit_error || he_n <= 0) return;
    int li = (he_next + HE_HITS - 1) % HE_HITS;
    if (SDL_GetTicks() - he_time[li] < 700 && abs(he_last) > hitwin[J_MAX])
        draw_text(font_small, he_last < 0 ? "EARLY" : "LATE", x, lay_judge_y - 84,
                  he_last < 0 ? C_EARLY : C_LATE, align);
}

/* ---- In-game leaderboard (far left of the playfield) ----
   Select during a song: Global -> Local -> Off. Shows 5 rows around you,
   starting from the lowest score; you move up as you pass people. */
#define IG_SHOW 5        /* rows shown at once */
#define IG_MAX  100      /* most scores kept per map */
#define IG_Y    70       /* top of the board (make bigger to move it down) */
#define IG_W    210      /* width (gets narrower if the lanes are wide) */
#define IG_ROW  46       /* row height */

typedef struct { char name[24], mods[40]; int score, combo; float acc; } IgRow;
static IgRow  ig_loc[IG_MAX], ig_glb[IG_MAX];
static int    ig_loc_n = 0, ig_glb_n = 0, ig_built = 0;
static int    ig_gl = 0;                  /* global: 0 not asked, 1 waiting, 2 ready, 3 offline */
static int    ig_last_above = -1;
static Uint32 ig_gl_since = 0, ig_flash = 0, ig_msg_until = 0;

static void ig_reset(void) {
    ig_loc_n = ig_glb_n = 0;
    ig_built = 0;
    ig_gl = 0;
    ig_last_above = -1;
    ig_flash = 0;
}

/* Select: Global -> Local -> Off -> Global */
static void ig_cycle(void) {
    opt_ingame_lb = (opt_ingame_lb + 2) % 3;
    ig_last_above = -1;
    ig_msg_until = SDL_GetTicks() + 1200;
    settings_save();
}

static int cmp_ig(const void *a, const void *b) {
    return ((const IgRow *)b)->score - ((const IgRow *)a)->score;
}

/* your saved scores for this map, best first */
static void ig_build_local(void) {
    char key[300];
    ig_loc_n = 0;
    ig_built = 1;
    map_key(&maps[play_map], key, sizeof key);
    for (int i = 0; i < all_count && ig_loc_n < IG_MAX; i++) {
        const ScoreEntry *e = &all_scores[i];
        if (strcmp(e->key, key)) continue;
        IgRow *r = &ig_loc[ig_loc_n++];
        fmt_date(e->when, "%y-%m-%d", r->name, sizeof r->name);
        mods_string(e->mods, r->mods, sizeof r->mods);
        r->score = e->score;
        r->combo = e->combo;
        r->acc   = e->acc;
    }
    qsort(ig_loc, ig_loc_n, sizeof *ig_loc, cmp_ig);
}

/* copies the online top list for this map (asks the server if it isn't loaded) */
static void ig_poll_global(void) {
    char osu[512];
    int same, st, ask = 0;
    if (ig_gl >= 2) return;
    snprintf(osu, sizeof osu, "%s/%s", maps[play_map].folder, maps[play_map].file);
    SDL_LockMutex(lb_mx);
    same = !strcmp(lb_state_map, osu);
    st = lb_state;
    if (same && st == 2) {
        ig_glb_n = lb_row_n < IG_MAX ? lb_row_n : IG_MAX;
        for (int i = 0; i < ig_glb_n; i++) {
            IgRow *r = &ig_glb[i];
            const LbRow *e = &lb_rows[i];
            snprintf(r->name, sizeof r->name, "%s", e->name);
            snprintf(r->mods, sizeof r->mods, "%s", e->mods);
            for (char *p = r->mods; *p; p++) if (*p == ',') *p = ' ';
            r->score = e->score;
            r->combo = e->combo;
            r->acc   = e->acc;
        }
        ig_gl = 2;
    } else if (ig_gl == 0) {
        ask = 1;
    } else if (same && st == 3 && SDL_GetTicks() - ig_gl_since > 1500) {
        ig_gl = 3;
    }
    SDL_UnlockMutex(lb_mx);
    if (ask) { lb_request_top(osu); ig_gl = 1; ig_gl_since = SDL_GetTicks(); }
}

static void draw_ingame_board(void) {
    char buf[96], fitb[96], sc[24];
    Uint32 now = SDL_GetTicks();
    int mode = opt_ingame_lb;
    int toast = !SDL_TICKS_PASSED(now, ig_msg_until);
    int w = IG_W < lay_x0 - 24 ? IG_W : lay_x0 - 24;
    const int x = 12;

    if (mode == 0 || play_map < 0 || w < 130) {
        if (toast) draw_text(font_small, "Leaderboard off  (Select)", x, IG_Y - 26, C_YELLOW, 0);
        return;
    }
    if (!ig_built) ig_build_local();
    if (mode == 2) ig_poll_global();

    const IgRow *rows = mode == 2 ? ig_glb : ig_loc;
    int n = mode == 2 ? (ig_gl == 2 ? ig_glb_n : 0) : ig_loc_n;

    /* header */
    const char *head = mode == 1 ? "Local"
                     : ig_gl == 3 ? "Global (offline)"
                     : ig_gl < 2  ? "Global (loading...)" : "Global";
    draw_text(font_small, head, x, IG_Y - 26, toast ? C_YELLOW : C_GREY, 0);

    /* where you are: everyone with a score >= yours is above you */
    int cur = score_current(), above = 0;
    while (above < n && rows[above].score >= cur) above++;
    if (ig_last_above >= 0 && above < ig_last_above) ig_flash = now;   /* passed someone */
    ig_last_above = above;

    int total = n + 1;                                  /* + you */
    int show = total < IG_SHOW ? total : IG_SHOW;
    int start = clampi(above - (IG_SHOW - 1), 0, total - show);

    for (int i = 0; i < show; i++) {
        int p = start + i, y = IG_Y + i * IG_ROW;
        if (p == above) {
            /* your row (live) */
            if (now - ig_flash < 400) fill(x, y, w, IG_ROW - 4, 170, 140, 60, 235);
            else                      fill(x, y, w, IG_ROW - 4, 90, 75, 40, 220);
            fill(x, y, 4, IG_ROW - 4, 255, 210, 90, 255);
            if (mode == 2 && above >= n && ig_glb_n >= LB_MAX_ROWS) snprintf(buf, sizeof buf, "--");
            else snprintf(buf, sizeof buf, "#%d", p + 1);
            draw_fast(font_small, buf, x + 10, y + 2, C_YELLOW, 0);
            fit_text(font_small, lb_name[0] ? lb_name : "You", w - 62, fitb, sizeof fitb);
            draw_text(font_small, fitb, x + 54, y + 2, C_YELLOW, 0);
            fmt_score(cur, sc, sizeof sc);
            draw_fast(font_small, sc, x + 10, y + 21, C_WHITE, 0);
            snprintf(buf, sizeof buf, "%.2f%%", calc_acc());
            draw_fast(font_small, buf, x + w - 8, y + 21, C_GREY, 2);
        } else {
            const IgRow *e = &rows[p < above ? p : p - 1];
            int mw = 0;
            fill(x, y, w, IG_ROW - 4, 30, 30, 45, 170);
            snprintf(buf, sizeof buf, "#%d", p + 1);
            draw_text(font_small, buf, x + 10, y + 2, C_GREY, 0);
            if (e->mods[0]) {
                fit_text(font_small, e->mods, 70, fitb, sizeof fitb);
                mw = draw_text(font_small, fitb, x + w - 8, y + 2, C_YELLOW, 2) + 8;
            }
            fit_text(font_small, e->name, w - 62 - mw, fitb, sizeof fitb);
            draw_text(font_small, fitb, x + 54, y + 2, C_WHITE, 0);
            fmt_score(e->score, sc, sizeof sc);
            draw_text(font_small, sc, x + 10, y + 21, C_WHITE, 0);
            snprintf(buf, sizeof buf, "%.2f%%", e->acc);
            draw_text(font_small, buf, x + w - 8, y + 21, C_GREY, 2);
        }
    }
}

static void draw_play(void) {
    int now = song_time();
    char buf[64], sc[24];
    int cinema = mod_on(play_mods, M_CM);
    int cx = SCREEN_W / 2;
    int dim = cinema ? 0 : opt_bg_dim;

    /* video if this map has one (and Video is on), otherwise the picture */
    if (video_draw()) {
        if (dim > 0) fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, (Uint8)(dim * 255 / 100));
    } else {
        draw_background(play_map, dim, 0);
    }

    if (!cinema) {
        field_mods = play_mods;
        draw_field(notes, note_n, first_live, now, col_down);
        field_mods = 0;

        cx = lay_x0 + lay_total / 2;
        int ja, jx = judge_x(cx, &ja);
        if (last_j >= 0 && SDL_GetTicks() - last_j_time < 400)
            draw_text(font_big, jname[last_j], jx, lay_judge_y, jcol[last_j], ja);
        if (combo >= 2) {
            snprintf(buf, sizeof buf, "%d", combo);
            draw_fast(font_big, buf, jx, lay_judge_y - 60, C_WHITE, ja);
        }
        draw_hit_error(cx);
        draw_early_late(jx, ja);
        if (opt_health_bar) draw_health_bar(health);
        draw_ingame_board();
        if (opt_hud) {
            fmt_score(score_current(), sc, sizeof sc);
            draw_fast(font_big, sc, SCREEN_W - 12, 4, C_WHITE, 2);
            snprintf(buf, sizeof buf, "%.2f%%", calc_acc());
            draw_fast(font_small, buf, SCREEN_W - 12, 50, C_GREY, 2);
        }
        if (play_mods) {
            mods_string(play_mods, buf, sizeof buf);
            draw_text(font_small, buf, 12, 8, C_YELLOW, 0);
        }
        if (last_note_time > 0 && now > 0) {
            float p = (float)now / last_note_time;
            if (p > 1) p = 1;
            fill(0, 0, (int)(SCREEN_W * p), 3, 255, 255, 255, 120);
        }
    } else if (!paused && now < 2500) {
        draw_text(font_small, "Cinema - press Start to pause", SCREEN_W / 2, SCREEN_H - 40, C_GREY, 1);
    }

    if (paused && resume_at) {
        /* 3-2-1 countdown */
        Uint32 t = SDL_GetTicks();
        int left = SDL_TICKS_PASSED(t, resume_at) ? 0 : (int)(resume_at - t);
        int n = clampi((left + 499) / 500, 1, 3);
        snprintf(buf, sizeof buf, "%d", n);
        fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 90);
        draw_text_scaled(font_big, buf, cx, SCREEN_H / 2 - 40, C_YELLOW, 3);
        draw_text(font_small, "Get ready!   (Start = back to menu)", cx, SCREEN_H / 2 + 40, C_WHITE, 1);
    } else if (paused) {
        static const char *items[P_COUNT] = {
            "Resume", "Retry", "Scroll speed", "Audio offset", "Quit to song select"
        };
        const MapInfo *m = &maps[play_map];
        char info[300], fitb[300], mb[64];

        fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 180);
        mods_string(play_mods, mb, sizeof mb);
        if (mb[0]) snprintf(info, sizeof info, "Paused  (%s)", mb);
        else       snprintf(info, sizeof info, "Paused");
        draw_text(font_big, info, SCREEN_W / 2, 46, C_WHITE, 1);

        snprintf(info, sizeof info, "%s - %s  [%dK] %s  %.2f*", m->artist, m->title, m->keys,
                 m->version, stars_shown(m->stars));
        fit_text(font_small, info, 800, fitb, sizeof fitb);
        draw_text(font_small, fitb, SCREEN_W / 2, 98, C_GREY, 1);

        int prog = last_note_time > 0 ? clampi(now * 100 / last_note_time, 0, 100) : 0;
        fmt_score(score_current(), sc, sizeof sc);
        snprintf(info, sizeof info, "%d%% done    Score %s    %.2f%%    Combo %dx    Health %d%%",
                 prog, sc, calc_acc(), combo, (int)(health * 100.0f + 0.5f));
        draw_text(font_small, info, SCREEN_W / 2, 126, C_WHITE, 1);
        fill(SCREEN_W / 2 - 200, 156, 400, 6, 255, 255, 255, 50);
        fill(SCREEN_W / 2 - 200, 156, prog * 4, 6, 255, 210, 90, 220);

        for (int i = 0; i < P_COUNT; i++) {
            int y = 190 + i * 50, sel = i == pause_sel;
            if (i == P_SPEED)       snprintf(info, sizeof info, "Scroll speed:  < %d >", opt_speed);
            else if (i == P_OFFSET) snprintf(info, sizeof info, "Audio offset:  < %+d ms >", opt_offset);
            else if (i == P_QUIT && pause_confirm) snprintf(info, sizeof info, "Press X again to quit");
            else                    snprintf(info, sizeof info, "%s", items[i]);
            if (sel) fill(SCREEN_W / 2 - 200, y - 6, 400, 40, 255, 210, 90, 60);
            draw_text(font_small, info, SCREEN_W / 2, y + 2, sel ? C_YELLOW : C_WHITE, 1);
        }
        draw_text(font_small, "Up/Down: select   Left/Right: change   X: confirm   O / Start: resume",
                  SCREEN_W / 2, SCREEN_H - 34, C_GREY, 1);
    }
}

/* ================= Menu helpers ================= */
static void settings_open(void);
static void scores_open(int from_results);

static const SDL_Color C_RED = {255, 90, 90, 255};

static SDL_Color star_color(float s) {
    if (s < 2.0f) return (SDL_Color){120, 200, 255, 255};
    if (s < 3.0f) return (SDL_Color){120, 230, 120, 255};
    if (s < 4.0f) return (SDL_Color){255, 220,  90, 255};
    if (s < 5.0f) return (SDL_Color){255, 160,  70, 255};
    if (s < 6.5f) return (SDL_Color){255,  90, 110, 255};
    return (SDL_Color){200, 120, 255, 255};
}

static void draw_status(void) {
    if (!status_msg[0] || SDL_TICKS_PASSED(SDL_GetTicks(), status_until)) return;
    fill(0, SCREEN_H - 76, SCREEN_W, 36, 0, 0, 0, 210);
    draw_text(font_small, status_msg, SCREEN_W / 2, SCREEN_H - 70, C_YELLOW, 1);
}

static void draw_help(const char *s) {
    fill(0, SCREEN_H - 34, SCREEN_W, 34, 0, 0, 0, 180);
    draw_text(font_small, s, SCREEN_W / 2, SCREEN_H - 29, C_GREY, 1);
}

/* loads the background for a map without drawing it (used by the Settings preview) */
static void bg_tick(int idx, int wait_ms) {
    int busy = bg_finish(0);
    if (idx != bg_want_map) { bg_want_map = idx; bg_want_since = SDL_GetTicks(); }
    if (!busy && idx != bg_loaded_map && SDL_GetTicks() - bg_want_since >= (Uint32)wait_ms)
        bg_start(idx, 1);
}

static void fmt_ratio(int mx, int n300, char *out, size_t n) {
    if (n300 > 0)    snprintf(out, n, "%.2f : 1", (double)mx / n300);
    else if (mx > 0) snprintf(out, n, "%d : 0", mx);
    else             snprintf(out, n, "-");
}

static void rescan_songs(void) {
    char buf[64];
    preview_stop();
    bg_finish(1);
    bg_loaded_map = bg_want_map = -1;
    draw_loading("Loading songs...");
    scan_songs();
    build_song_groups();
    lb_for_map = -2;
    snprintf(buf, sizeof buf, "Found %d maps", map_count);
    set_status(buf);
}

/* ================= Sort / search menu (Circle on song select) ================= */
#ifdef __vita__
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/apputil.h>
#endif

enum { SM_SORT, SM_SEARCH, SM_CLEAR, SM_CLOSE, SM_COUNT };
static int sm_open = 0, sm_sel = 0, ime_open = 0;
static Uint32 ime_block_until = 0;

static void apply_song_list(void) {
    build_song_groups();
    lb_for_map = -2;
}

#ifdef __vita__
static SceWChar16 ime_title[32], ime_init[130], ime_buf[130];

static void utf8_to_16(const char *s, SceWChar16 *out, int max) {
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max - 1) {
        unsigned c = *p++;
        if (c >= 0xF0) { while ((*p & 0xC0) == 0x80) p++; c = '?'; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        else if (c >= 0x80) c = '?';
        out[n++] = (SceWChar16)c;
    }
    out[n] = 0;
}

static void utf16_to_8(const SceWChar16 *s, char *out, size_t max) {
    size_t n = 0;
    for (; *s; s++) {
        unsigned c = *s;
        if (c >= 0xD800 && c <= 0xDFFF) c = '?';
        if (c < 0x80) { if (n + 1 >= max) break; out[n++] = (char)c; }
        else if (c < 0x800) {
            if (n + 2 >= max) break;
            out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 63));
        } else {
            if (n + 3 >= max) break;
            out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 63));
            out[n++] = (char)(0x80 | (c & 63));
        }
    }
    out[n] = 0;
}
#endif

static int ime_name = 0;    /* 1 = the keyboard is for the player name, 0 = song search */

static void search_open(void) {
#ifdef __vita__
    static int dlg_ready = 0;
    if (!dlg_ready) {
        SceAppUtilInitParam ip; SceAppUtilBootParam bp; SceCommonDialogConfigParam cp;
        memset(&ip, 0, sizeof ip); memset(&bp, 0, sizeof bp); memset(&cp, 0, sizeof cp);
        sceAppUtilInit(&ip, &bp);
        sceCommonDialogSetConfigParam(&cp);
        dlg_ready = 1;
    }
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    utf8_to_16(ime_name ? "Player name (3-16)" : "Search songs", ime_title, 32);
    utf8_to_16(ime_name ? lb_name : search_text, ime_init, 130);
    memset(ime_buf, 0, sizeof ime_buf);
    p.supportedLanguages = 0x0001FFFF;
    p.languagesForced = SCE_TRUE;
    p.type = ime_name ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = ime_title;
    p.maxTextLength = ime_name ? 16 : 127;
    p.initialText = ime_init;
    p.inputTextBuffer = ime_buf;
    if (sceImeDialogInit(&p) >= 0) { ime_open = 1; sm_open = 0; }
    else { ime_name = 0; set_status("Couldn't open the keyboard"); }
#else
    ime_name = 0;
    set_status("Search uses the Vita keyboard");
#endif
}

/* opens the keyboard to type the online player name */
static void name_open(void) {
    ime_name = 1;
    search_open();
}

/* called every frame: picks up the text when the keyboard closes */
static void search_update(void) {
#ifdef __vita__
    if (!ime_open || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return;
    SceImeDialogResult r;
    memset(&r, 0, sizeof r);
    sceImeDialogGetResult(&r);
    if (r.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        char msg[80];
        if (ime_name) {
            char typed[130], old[17];
            snprintf(old, sizeof old, "%s", lb_name);
            utf16_to_8(ime_buf, typed, sizeof typed);
            trim(typed);
            lb_set_name(typed);
            if (lb_name_ok()) {
                snprintf(msg, sizeof msg, "Player name set to %s", lb_name);
                set_status(msg);
            } else {                                   /* too short: keep the old name */
                snprintf(lb_name, sizeof lb_name, "%s", old);
                lb_save_player();
                set_status("Names need 3-16 letters, numbers, _ or -");
            }
        } else {
            utf16_to_8(ime_buf, search_text, sizeof search_text);
            trim(search_text);
            apply_song_list();
            if (search_text[0]) {
                snprintf(msg, sizeof msg, "%d song%s found", g_group_count, g_group_count == 1 ? "" : "s");
                set_status(msg);
            }
        }
    }
    sceImeDialogTerm();
    ime_open = 0;
    ime_name = 0;
    ime_block_until = SDL_GetTicks() + 300;   /* the keyboard's X press won't start a song */
#endif
}

static void sort_menu_change(int d) {
    opt_sort = (opt_sort + d + SORT_COUNT) % SORT_COUNT;
    apply_song_list();
    settings_save();
}

static void sort_menu_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    sm_sel = (sm_sel + SM_COUNT - 1) % SM_COUNT; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sm_sel = (sm_sel + 1) % SM_COUNT; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  if (sm_sel == SM_SORT) sort_menu_change(-1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (sm_sel == SM_SORT) sort_menu_change(1);  break;
    case SDL_CONTROLLER_BUTTON_A:
        if (sm_sel == SM_SORT) sort_menu_change(1);
        else if (sm_sel == SM_SEARCH) search_open();
        else if (sm_sel == SM_CLEAR) {
            if (search_text[0]) { search_text[0] = 0; apply_song_list(); set_status("Search cleared"); }
            sm_open = 0;
        } else sm_open = 0;
        break;
    case SDL_CONTROLLER_BUTTON_B: sm_open = 0; break;
    }
}

/* returns 1 if the menu or keyboard used the button */
static int sort_menu_takes(int b) {
    if (ime_open || !SDL_TICKS_PASSED(SDL_GetTicks(), ime_block_until)) return 1;
    if (sm_open) { sort_menu_button(b); return 1; }
    if (b == SDL_CONTROLLER_BUTTON_B) { sm_open = 1; sm_sel = 0; return 1; }
    return 0;
}

static void draw_sort_menu(void) {
    static const char *labels[SM_COUNT] = { "Sort by", "Search...", "Clear search", "Close" };
    char buf[200], fitb[200];
    if (!sm_open) return;
    const int W = 440, H = 60 + SM_COUNT * 40, X = (SCREEN_W - W) / 2, Y = (SCREEN_H - H) / 2;
    fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 120);
    fill(X, Y, W, H, 20, 20, 32, 245);
    fill(X, Y, W, 4, 255, 210, 90, 255);
    draw_text(font_big, "Sort / Search", X + W / 2, Y + 10, C_WHITE, 1);
    for (int i = 0; i < SM_COUNT; i++) {
        int y = Y + 60 + i * 40, sel = i == sm_sel;
        if (sel) fill(X + 10, y - 4, W - 20, 34, 255, 210, 90, 60);
        draw_text(font_small, labels[i], X + 24, y + 2, sel ? C_YELLOW : C_WHITE, 0);
        buf[0] = 0;
        if (i == SM_SORT)
            snprintf(buf, sizeof buf, "%s%s%s", sel ? "< " : "", sort_names[opt_sort], sel ? " >" : "");
        else if (i == SM_SEARCH && search_text[0])
            snprintf(buf, sizeof buf, "\"%s\"", search_text);
        if (buf[0]) {
            fit_text(font_small, buf, 220, fitb, sizeof fitb);
            draw_text(font_small, fitb, X + W - 24, y + 2, sel ? C_YELLOW : C_GREY, 2);
        }
    }
    fill(0, SCREEN_H - 34, SCREEN_W, 34, 0, 0, 0, 255);
    draw_help("Up/Down: select   Left/Right: change sort   X: choose   O: close");
}

/* ================= Mods menu (Square on song select) ================= */
#define MM_COLS 4
#define MM_ROWS ((MOD_COUNT + MM_COLS - 1) / MM_COLS)
static int mm_open = 0, mm_sel = 0;

static void mods_menu_close(void) {
    mm_open = 0;
    settings_save();                     /* remembers the mods (except AT / CM) */
}

static void mods_menu_button(int b) {
    int r = mm_sel / MM_COLS, c = mm_sel % MM_COLS;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    r = (r + MM_ROWS - 1) % MM_ROWS; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  r = (r + 1) % MM_ROWS; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  c = (c + MM_COLS - 1) % MM_COLS; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: c = (c + 1) % MM_COLS; break;
    case SDL_CONTROLLER_BUTTON_A:          mod_toggle(mm_sel); return;
    case SDL_CONTROLLER_BUTTON_Y:
        if (opt_mods) { opt_mods = 0; set_status("All mods off"); }
        return;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_X:
    case SDL_CONTROLLER_BUTTON_START:      mods_menu_close(); return;
    default: return;
    }
    int n = r * MM_COLS + c;
    if (n < MOD_COUNT) mm_sel = n;
}

static void draw_mods_menu(void) {
    if (!mm_open) return;
    const int CW = 140, CH = 56, GAP = 8;
    const int W = MM_COLS * CW + (MM_COLS - 1) * GAP + 40;
    const int H = 60 + MM_ROWS * (CH + GAP) + 100;
    const int X = (SCREEN_W - W) / 2, Y = (SCREEN_H - 34 - H) / 2;
    char buf[160], fitb[160], mb[64];

    fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 140);
    fill(X, Y, W, H, 20, 20, 32, 245);
    fill(X, Y, W, 4, 255, 210, 90, 255);
    draw_text(font_big, "Mods", X + W / 2, Y + 10, C_WHITE, 1);

    for (int m = 0; m < MOD_COUNT; m++) {
        int x = X + 20 + (m % MM_COLS) * (CW + GAP);
        int y = Y + 60 + (m / MM_COLS) * (CH + GAP);
        int on = mod_on(opt_mods, m), sel = m == mm_sel;
        if (on) fill(x, y, CW, CH, 220, 170, 60, 235);
        else    fill(x, y, CW, CH, 45, 45, 65, 230);
        if (sel) {                                   /* yellow frame */
            fill(x - 3, y - 3, CW + 6, 3, 255, 230, 120, 255);
            fill(x - 3, y + CH, CW + 6, 3, 255, 230, 120, 255);
            fill(x - 3, y, 3, CH, 255, 230, 120, 255);
            fill(x + CW, y, 3, CH, 255, 230, 120, 255);
        }
        draw_text(font_small, mod_short[m], x + CW / 2, y + 5, on ? C_DARK : C_WHITE, 1);
        fit_text(font_small, mod_long[m], CW - 10, fitb, sizeof fitb);
        draw_text(font_small, fitb, x + CW / 2, y + 29, on ? C_DARK : C_GREY, 1);
    }

    int by = Y + 60 + MM_ROWS * (CH + GAP) + 4;
    fit_text(font_small, mod_desc[mm_sel], W - 40, fitb, sizeof fitb);
    draw_text(font_small, fitb, X + W / 2, by, C_WHITE, 1);

    mods_string(opt_mods, mb, sizeof mb);
    if (mb[0]) snprintf(buf, sizeof buf, "Active: %s     Score x%.2f", mb, mods_multiplier(opt_mods));
    else       snprintf(buf, sizeof buf, "No mods active     Score x1.00");
    fit_text(font_small, buf, W - 40, fitb, sizeof fitb);
    draw_text(font_small, fitb, X + W / 2, by + 30, C_YELLOW, 1);

    if (opt_mods & MODS_UNSAVED)
        draw_text(font_small, "Scores won't be saved with these mods", X + W / 2, by + 58, C_RED, 1);

    fill(0, SCREEN_H - 34, SCREEN_W, 34, 0, 0, 0, 255);
    draw_help("Arrows: select   X: on/off   Triangle: all off   O / Square: close");
}

/* ================= Results ================= */
static void draw_results(void) {
    char buf[300], fitb[300], sc[24], mb[64];
    const MapInfo *m = &maps[play_map];
    draw_background(play_map, opt_bg_dim > 80 ? opt_bg_dim : 80, 0);

    draw_text(font_big, "Results", SCREEN_W / 2, 10, C_WHITE, 1);
    snprintf(buf, sizeof buf, "%s - %s  [%dK] %s  %.2f*", m->artist, m->title, m->keys,
             m->version, stars_shown(m->stars));
    fit_text(font_small, buf, 880, fitb, sizeof fitb);
    draw_text(font_small, fitb, SCREEN_W / 2, 58, C_GREY, 1);

    /* grade, score, accuracy */
    SDL_Color gc;
    const char *g = lb_grade(res_acc, &gc);
    fill(60, 96, 380, 370, 0, 0, 0, 170);
    draw_text_scaled(font_big, g, 250, 170, gc, 4);

    SDL_Color fcc;                                       /* FC / PFC, top right of the box */
    const char *fc = fc_tag(res_n, &fcc);
    if (fc) draw_text(font_big, fc, 426, 104, fcc, 2);

    fmt_score(res_score, sc, sizeof sc);
    draw_text(font_big, sc, 250, 262, C_WHITE, 1);
    snprintf(buf, sizeof buf, "%.2f%%     %dx", res_acc, res_combo);
    draw_text(font_small, buf, 250, 312, C_WHITE, 1);

    mods_string(res_mods, mb, sizeof mb);
    if (mb[0]) snprintf(buf, sizeof buf, "Mods: %s  (x%.2f)", mb, mods_multiplier(res_mods));
    else       snprintf(buf, sizeof buf, "No mods");
    fit_text(font_small, buf, 360, fitb, sizeof fitb);
    draw_text(font_small, fitb, 250, 342, mb[0] ? C_YELLOW : C_GREY, 1);

    if (!(res_mods & MODS_NOFAIL)) {
        snprintf(buf, sizeof buf, "Health left  %d%%", (int)(res_health * 100.0f + 0.5f));
        draw_text(font_small, buf, 250, 372, C_GREY, 1);
    }

    if (res_mods & MODS_UNSAVED) snprintf(buf, sizeof buf, "Score not saved (unranked mods)");
    else if (res_rank == 1)      snprintf(buf, sizeof buf, "New best score!");
    else                         snprintf(buf, sizeof buf, "Rank #%d on this map", res_rank);
    draw_text(font_small, buf, 250, 410, (res_mods & MODS_UNSAVED) ? C_GREY : C_YELLOW, 1);
    /* online leaderboard */
    {
        SDL_Color oc = C_GREY;
        buf[0] = 0;
        switch (lb_sub_state) {
        case 1: snprintf(buf, sizeof buf, "Sending score online..."); break;
        case 2:
            if (lb_sub_improved)
                snprintf(buf, sizeof buf, "Global #%d   +%.0fpp   (%.0fpp, player #%d)",
                         lb_sub_rank, lb_sub_pp, lb_sub_total, lb_sub_prank);
            else
                snprintf(buf, sizeof buf, "Sent - your online best is higher");
            oc = C_YELLOW;
            break;
        case 3: snprintf(buf, sizeof buf, "Offline: %s", lb_sub_msg); break;
        case 4: snprintf(buf, sizeof buf, "Not sent: %s", lb_sub_msg); oc = C_RED; break;
        }
        if (buf[0]) {
            fit_text(font_small, buf, 360, fitb, sizeof fitb);
            draw_text(font_small, fitb, 250, 438, oc, 1);
        }
    }

    /* judgements */
    fill(500, 96, 400, 370, 0, 0, 0, 170);
    for (int j = 0; j < 6; j++) {
        int y = 110 + j * 44;
        draw_text(font_big, jname[j], 530, y, jcol[j], 0);
        snprintf(buf, sizeof buf, "%d", res_n[j]);
        draw_text(font_big, buf, 870, y, C_WHITE, 2);
    }
    fmt_ratio(res_n[J_MAX], res_n[J_300], fitb, sizeof fitb);
    snprintf(buf, sizeof buf, "MAX:300 ratio   %s", fitb);
    draw_text(font_small, buf, 700, 386, C_GREY, 1);

    draw_help("X / O: Song select   Square: Retry   Select: Scores");
}

static void results_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_B:
        select_map(play_map);
        lb_for_map = -2;
        screen = SCR_SELECT;
        break;
    case SDL_CONTROLLER_BUTTON_X:
        start_map(play_map);
        break;
    case SDL_CONTROLLER_BUTTON_BACK:
        select_map(play_map);
        scores_open(1);
        break;
    }
}

/* ================= Failed screen ================= */
static void draw_failed(void) {
    char buf[300], fitb[300], sc[24], mb[64];
    const MapInfo *m = &maps[play_map];
    draw_background(play_map, 85, 0);
    fill(0, 0, SCREEN_W, SCREEN_H, 60, 0, 0, 90);            /* red tint */

    draw_text_scaled(font_big, "Failed", SCREEN_W / 2, 30, C_RED, 2);
    snprintf(buf, sizeof buf, "%s - %s  [%dK] %s  %.2f*", m->artist, m->title, m->keys,
             m->version, stars_shown(m->stars));
    fit_text(font_small, buf, 880, fitb, sizeof fitb);
    draw_text(font_small, fitb, SCREEN_W / 2, 118, C_GREY, 1);
    draw_text(font_small, fail_reason[0] ? fail_reason : "You failed", SCREEN_W / 2, 150, C_WHITE, 1);

    /* how far you got */
    const int BW = 500, BX = (SCREEN_W - BW) / 2, BY = 196;
    fill(BX, BY, BW, 12, 255, 255, 255, 50);
    fill(BX, BY, BW * res_progress / 100, 12, 255, 90, 90, 230);
    snprintf(buf, sizeof buf, "You got %d%% of the way through", res_progress);
    draw_text(font_small, buf, SCREEN_W / 2, BY + 22, C_WHITE, 1);

    /* stats */
    fill(BX, 262, BW, 160, 0, 0, 0, 170);
    fmt_score(res_score, sc, sizeof sc);
    snprintf(buf, sizeof buf, "Score %s     %.2f%%     %dx", sc, res_acc, res_combo);
    draw_text(font_small, buf, SCREEN_W / 2, 274, C_WHITE, 1);
    for (int j = 0; j < 6; j++) {
        int x = BX + 30 + (j % 3) * 160, y = 314 + (j / 3) * 32;
        draw_text(font_small, jname[j], x, y, jcol[j], 0);
        snprintf(buf, sizeof buf, "%d", res_n[j]);
        draw_text(font_small, buf, x + 120, y, C_WHITE, 2);
    }
    mods_string(res_mods, mb, sizeof mb);
    if (mb[0]) snprintf(buf, sizeof buf, "Mods: %s", mb);
    else       snprintf(buf, sizeof buf, "No mods   (Square on song select: No Fail / Easy)");
    fit_text(font_small, buf, BW - 20, fitb, sizeof fitb);
    draw_text(font_small, fitb, SCREEN_W / 2, 388, mb[0] ? C_YELLOW : C_GREY, 1);

    draw_help("X / Square: Retry   O: Song select   (failed plays aren't saved)");
}

static void failed_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_X:
        start_map(play_map);
        break;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_START:
        select_map(play_map);
        lb_for_map = -2;
        screen = SCR_SELECT;
        break;
    }
}

/* ================= Song select leaderboard (Local / Global) ================= */
#define LB_X   620
#define LB_Y   44
#define LB_W   324
#define LB_ROW 52

static int    gl_pending = 0;      /* 1 = waiting before asking the server */
static Uint32 gl_since = 0;        /* when the selected map last changed */
static char   gl_for[512] = "";    /* .osu file the Global board is showing */

/* Start on song select: switches between Local and Global */
static void lb_toggle_view(void) {
    opt_lb_global = !opt_lb_global;
    gl_for[0] = 0;                 /* ask the server again */
    settings_save();
}

/* panel + "Local | Global" tabs (the active one is yellow and underlined) */
static void draw_board_head(int global, const char *right) {
    fill(LB_X, LB_Y, LB_W, 40 + LB_SHOW * LB_ROW + 6, 0, 0, 0, 160);
    int w1 = draw_text(font_small, "Local", LB_X + 12, LB_Y + 8, global ? C_GREY : C_YELLOW, 0);
    draw_text(font_small, "|", LB_X + 12 + w1 + 8, LB_Y + 8, C_GREY, 0);
    int gx = LB_X + 12 + w1 + 22;
    int w2 = draw_text(font_small, "Global", gx, LB_Y + 8, global ? C_YELLOW : C_GREY, 0);
    fill(global ? gx : LB_X + 12, LB_Y + 31, global ? w2 : w1, 2, 255, 210, 90, 255);
    if (right && right[0]) draw_text(font_small, right, LB_X + LB_W - 12, LB_Y + 8, C_GREY, 2);
}

/* your own scores from scores.txt */
static void draw_local_board(void) {
    char buf[128], sc[24], date[16], mb[64], fb[64];
    snprintf(buf, sizeof buf, "%d score%s", lb_total, lb_total == 1 ? "" : "s");
    draw_board_head(0, buf);
    if (lb_count == 0) {
        draw_text(font_small, "No scores yet", LB_X + 12, LB_Y + 50, C_GREY, 0);
        return;
    }
    for (int i = 0; i < lb_count; i++) {
        int y = LB_Y + 40 + i * LB_ROW;
        if (i == 0) fill(LB_X + 6, y, LB_W - 12, LB_ROW - 6, 90, 75, 40, 210);
        else        fill(LB_X + 6, y, LB_W - 12, LB_ROW - 6, 45, 45, 65, 210);

        SDL_Color gc;
        const char *g = lb_grade(lb[i].acc, &gc);
        draw_text(font_big, g, LB_X + 14, y - 2, gc, 0);

        fmt_score(lb[i].score, sc, sizeof sc);
        snprintf(buf, sizeof buf, "#%d  %s", i + 1, sc);
        draw_text(font_small, buf, LB_X + 76, y + 1, C_WHITE, 0);
        if (lb[i].mods) {                                   /* mods on the right */
            mods_string(lb[i].mods, mb, sizeof mb);
            fit_text(font_small, mb, 100, fb, sizeof fb);
            draw_text(font_small, fb, LB_X + LB_W - 14, y + 1, C_YELLOW, 2);
        }

        fmt_date(lb[i].when, "%y-%m-%d", date, sizeof date);
        snprintf(buf, sizeof buf, "%.2f%%  %dx  %s", lb[i].acc, lb[i].combo, date);
        int tw = draw_text(font_small, buf, LB_X + 76, y + 23, C_GREY, 0);
        SDL_Color fcc;
        const char *fc = fc_tag(lb[i].n, &fcc);             /* FC / PFC after the date */
        if (fc) draw_text(font_small, fc, LB_X + 76 + tw + 10, y + 23, fcc, 0);
    }
}

/* top scores from maniavita.com for the selected map */
static void draw_global_board(int idx) {
    static LbRow rows[LB_MAX_ROWS];
    static const SDL_Color c_fc = {120, 200, 255, 255};
    char osu[512], buf[160], fitb[160], sc[24], err[96] = "";
    const MapInfo *m = &maps[idx];
    Uint32 now = SDL_GetTicks();

    /* only ask the server once the player stops scrolling for 400 ms */
    snprintf(osu, sizeof osu, "%s/%s", m->folder, m->file);
    if (strcmp(osu, gl_for)) {
        snprintf(gl_for, sizeof gl_for, "%s", osu);
        gl_pending = 1;
        gl_since = now;
    }
    if (gl_pending && now - gl_since >= 400) {
        gl_pending = 0;
        lb_request_top(osu);
    }

    /* copy the rows if they belong to this map (old rows stay while refreshing) */
    int n = 0, st = 1;
    SDL_LockMutex(lb_mx);
    if (!strcmp(lb_state_map, osu)) {
        n = lb_row_n;
        memcpy(rows, lb_rows, sizeof(LbRow) * n);
        st = lb_state;
        if (st == 1 && n > 0) st = 2;
        if (st == 3) snprintf(err, sizeof err, "%s", lb_err);
    }
    SDL_UnlockMutex(lb_mx);

    if (st == 2) snprintf(buf, sizeof buf, "%d online", n);
    else buf[0] = 0;
    draw_board_head(1, buf);

    if (st == 1) {
        draw_text(font_small, "Loading...", LB_X + 12, LB_Y + 50, C_GREY, 0);
        return;
    }
    if (st == 3) {
        draw_text(font_small, "Offline", LB_X + 12, LB_Y + 50, C_RED, 0);
        fit_text(font_small, err[0] ? err : "couldn't reach maniavita.com", LB_W - 24, fitb, sizeof fitb);
        draw_text(font_small, fitb, LB_X + 12, LB_Y + 76, C_GREY, 0);
        draw_text(font_small, "Start: back to Local scores", LB_X + 12, LB_Y + 110, C_GREY, 0);
        return;
    }
    if (n == 0) {
        draw_text(font_small, "No online scores yet", LB_X + 12, LB_Y + 50, C_GREY, 0);
        draw_text(font_small, "Play it to be the first!", LB_X + 12, LB_Y + 76, C_GREY, 0);
        return;
    }

    /* your own row: highlighted, and shown in 8th place if you're further down */
    int mine = -1;
    if (lb_name[0])
        for (int i = 0; i < n; i++)
            if (!strcmp(rows[i].name, lb_name)) { mine = i; break; }

    int show = n < LB_SHOW ? n : LB_SHOW;
    for (int i = 0; i < show; i++) {
        int r = (i == LB_SHOW - 1 && mine >= LB_SHOW) ? mine : i;
        const LbRow *e = &rows[r];
        int y = LB_Y + 40 + i * LB_ROW, me = r == mine;
        if (me) fill(LB_X + 6, y, LB_W - 12, LB_ROW - 6, 90, 75, 40, 220);
        else    fill(LB_X + 6, y, LB_W - 12, LB_ROW - 6, 45, 45, 65, 210);
        if (me) fill(LB_X + 6, y, 4, LB_ROW - 6, 255, 210, 90, 255);

        fmt_score(e->score, sc, sizeof sc);
        int sw = draw_text(font_small, sc, LB_X + LB_W - 14, y + 1, C_WHITE, 2);
        snprintf(buf, sizeof buf, "#%d  %s", e->rank, e->name);
        fit_text(font_small, buf, LB_W - 28 - sw - 10, fitb, sizeof fitb);
        draw_text(font_small, fitb, LB_X + 14, y + 1, me ? C_YELLOW : C_WHITE, 0);

        snprintf(buf, sizeof buf, "%.2f%%  %dx", e->acc, e->combo);
        int tw = draw_text(font_small, buf, LB_X + 14, y + 23, C_GREY, 0);
        if (e->miss == 0) draw_text(font_small, "FC", LB_X + 14 + tw + 10, y + 23, c_fc, 0);

        if (e->mods[0]) snprintf(buf, sizeof buf, "%s  %.0fpp", e->mods, e->pp);
        else            snprintf(buf, sizeof buf, "%.0fpp", e->pp);
        for (char *p = buf; *p; p++) if (*p == ',') *p = ' ';      /* "HR,HD" -> "HR HD" */
        fit_text(font_small, buf, 140, fitb, sizeof fitb);
        draw_text(font_small, fitb, LB_X + LB_W - 14, y + 23, e->mods[0] ? C_YELLOW : C_GREY, 2);
    }
}

static void draw_leaderboard(void) {
    int idx = selected_map_index();
    if (idx != lb_for_map) { lb_refresh(idx); gl_for[0] = 0; }
    if (idx < 0) return;
    if (opt_lb_global) draw_global_board(idx);
    else               draw_local_board();
}

/* ================= Song select ================= */
#define SEL_LX 16          /* song list position and size */
#define SEL_LY 116
#define SEL_LW 590
#define SEL_LH 388
#define SEL_RH 44          /* height of one song row */

/* front touchscreen on song select */
static int   st_active = 0, st_moved = 0;
static float st_x0, st_y0, st_scroll0;

/* top-right line: current sort, or the search (yellow) */
static void draw_list_status(void) {
    char buf[300], fitb[300];
    if (search_text[0]) {
        snprintf(buf, sizeof buf, "Search \"%s\": %d song%s   Sort: %s", search_text,
                 g_group_count, g_group_count == 1 ? "" : "s", sort_names[opt_sort]);
        fit_text(font_small, buf, 324, fitb, sizeof fitb);
        draw_text(font_small, fitb, 944, 14, C_YELLOW, 2);
    } else {
        snprintf(buf, sizeof buf, "Sort: %s   %d songs, %d maps (%s)", sort_names[opt_sort],
                 g_group_count, g_order_count, filter_names[opt_filter]);
        fit_text(font_small, buf, 324, fitb, sizeof fitb);
        draw_text(font_small, fitb, 944, 14, C_GREY, 2);
    }
}

static void draw_select(void) {
    char buf[320], fitb[320], mb[64];
    int idx = selected_map_index();
    draw_background(idx, opt_bg_dim, 250);

    if (g_group_count == 0) {
        fill(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 120);
        if (search_text[0] && map_count) {
            /* the search found nothing */
            snprintf(buf, sizeof buf, "Nothing matches \"%s\"", search_text);
            fit_text(font_big, buf, 880, fitb, sizeof fitb);
            draw_text(font_big, fitb, SCREEN_W / 2, 180, C_WHITE, 1);
            draw_text(font_small, "Press O and choose \"Clear search\" to see all songs again.",
                      SCREEN_W / 2, 240, C_GREY, 1);
        } else {
            draw_text(font_big, map_count ? "No maps match the key filter" : "No songs found",
                      SCREEN_W / 2, 180, C_WHITE, 1);
            draw_text(font_small, map_count ? "Change \"Key modes\" in Settings (Triangle)."
                                            : "Copy .osz files or song folders to ux0:data/osumania/songs/",
                      SCREEN_W / 2, 240, C_GREY, 1);
        }
        if (maps_skipped) {
            snprintf(buf, sizeof buf, "%d map(s) skipped (not osu!mania 1K - 5K)", maps_skipped);
            draw_text(font_small, buf, SCREEN_W / 2, 270, C_GREY, 1);
        }
        draw_list_status();
        draw_help("Sq: Mods   O: Sort/Search   Triangle: Settings   Start: Rescan songs");
        draw_mods_menu();
        return;
    }

    /* smooth scrolling (not while a finger is dragging the list) */
    if (!st_active || !st_moved) {
        float d = (float)g_sel_group - g_scroll;
        if (d > 8.0f)  g_scroll = (float)g_sel_group - 8.0f;
        if (d < -8.0f) g_scroll = (float)g_sel_group + 8.0f;
        g_scroll += ((float)g_sel_group - g_scroll) * 0.3f;
        if (fabsf((float)g_sel_group - g_scroll) < 0.01f) g_scroll = (float)g_sel_group;
    }

    /* selected map info */
    const MapInfo *m = &maps[idx];
    const SongGroup *sg = &g_groups[g_sel_group];
    fill(16, 10, 590, 98, 0, 0, 0, 170);

    int modw = 0;                                     /* active mods, top right of the box */
    mods_string(opt_mods, mb, sizeof mb);
    if (mb[0]) {
        snprintf(buf, sizeof buf, "%s  %.2fx", mb, mods_multiplier(opt_mods));
        fit_text(font_small, buf, 230, fitb, sizeof fitb);
        modw = draw_text(font_small, fitb, 594, 18, C_YELLOW, 2) + 14;
    }
    fit_text(font_big, m->title, 566 - modw, fitb, sizeof fitb);
    draw_text(font_big, fitb, 28, 10, C_WHITE, 0);

    snprintf(buf, sizeof buf, "%s   //   mapped by %s", m->artist, m->creator[0] ? m->creator : "?");
    fit_text(font_small, buf, 566, fitb, sizeof fitb);
    draw_text(font_small, fitb, 28, 56, C_GREY, 0);

    snprintf(buf, sizeof buf, "%.2f*", stars_shown(m->stars));
    int w1 = draw_text(font_small, buf, 594, 82, star_color(m->stars), 2);
    snprintf(buf, sizeof buf, "< %d/%d >", g_sel_diff + 1, sg->count);
    int w2 = draw_text(font_small, buf, 594 - w1 - 14, 82, C_GREY, 2);
    snprintf(buf, sizeof buf, "[%dK] %s   HP %.1f  OD %.1f", m->keys, m->version, m->hp, m->od);
    fit_text(font_small, buf, 566 - w1 - w2 - 30, fitb, sizeof fitb);
    draw_text(font_small, fitb, 28, 82, C_YELLOW, 0);

    /* song list */
    SDL_Rect clip = { SEL_LX, SEL_LY, SEL_LW, SEL_LH };
    SDL_RenderSetClipRect(ren, &clip);
    int cy = SEL_LY + SEL_LH / 2 - SEL_RH / 2;
    int g0 = (int)floorf(g_scroll) - SEL_LH / SEL_RH / 2 - 2;
    for (int gi = g0; gi <= g0 + SEL_LH / SEL_RH + 4; gi++) {
        if (gi < 0 || gi >= g_group_count) continue;
        int y = cy + (int)lroundf(((float)gi - g_scroll) * SEL_RH);
        if (y + SEL_RH < SEL_LY || y > SEL_LY + SEL_LH) continue;
        int sel = gi == g_sel_group;
        const MapInfo *gm = sel ? m : &maps[g_order[g_groups[gi].first]];
        if (sel) {
            fill(SEL_LX, y, SEL_LW, SEL_RH - 4, 90, 75, 40, 220);
            fill(SEL_LX, y, 4, SEL_RH - 4, 255, 210, 90, 255);
        } else fill(SEL_LX, y, SEL_LW, SEL_RH - 4, 20, 20, 32, 180);
        fit_text(font_small, gm->title, 460, fitb, sizeof fitb);
        draw_text(font_small, fitb, 30, y + 1, sel ? C_YELLOW : C_WHITE, 0);
        fit_text(font_small, gm->artist, 460, fitb, sizeof fitb);
        draw_text(font_small, fitb, 30, y + 19, C_GREY, 0);
        int n = g_groups[gi].count;
        snprintf(buf, sizeof buf, "%d diff%s", n, n == 1 ? "" : "s");
        draw_text(font_small, buf, 594, y + 10, C_GREY, 2);
    }
    SDL_RenderSetClipRect(ren, NULL);

    draw_list_status();
    draw_leaderboard();
    draw_help("X: Play   Sq: Mods   O: Sort/Search   Tri: Settings   Select: Scores   Start: Local/Global");
    draw_mods_menu();
}

static void select_button(int b) {
    if (mm_open) { mods_menu_button(b); return; }
    if (sort_menu_takes(b)) return;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       song_select_move(-1, 0); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     song_select_move(1, 0);  break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     song_select_move(0, -1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    song_select_move(0, 1);  break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  song_select_move(-5, 0); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: song_select_move(5, 0);  break;
    case SDL_CONTROLLER_BUTTON_A:             start_map(selected_map_index()); break;
    case SDL_CONTROLLER_BUTTON_X:             mm_open = 1; break;     /* Square: mods */
    case SDL_CONTROLLER_BUTTON_Y:             settings_open(); break;
    case SDL_CONTROLLER_BUTTON_BACK:          scores_open(0); break;
    case SDL_CONTROLLER_BUTTON_START:
        if (g_group_count == 0) rescan_songs();    /* no songs: Start still rescans */
        else lb_toggle_view();                      /* Local <-> Global */
        break;
    }
}

/* ---- front touchscreen: drag the list, tap a song, tap it again to play ---- */
static int sel_touch_ok(void) {
    return opt_touch && screen == SCR_SELECT && !mm_open && !sm_open && !ime_open && g_group_count > 0;
}

/* fx, fy = finger position from 0.0 to 1.0 */
static void select_touch_down(float fx, float fy) {
    float x = fx * SCREEN_W, y = fy * SCREEN_H;
    st_active = st_moved = 0;
    if (!sel_touch_ok()) return;
    if (x < SEL_LX || x >= SEL_LX + SEL_LW) return;
    if (y < 10 || y >= SEL_LY + SEL_LH) return;          /* info box or song list only */
    st_active = 1;
    st_x0 = x; st_y0 = y;
    st_scroll0 = g_scroll;
}

static void select_touch_move(float fx, float fy) {
    if (!st_active || !sel_touch_ok()) return;
    if (st_y0 < SEL_LY) return;                          /* started on the info box: no drag */
    float dy = fy * SCREEN_H - st_y0;
    if (!st_moved && fabsf(dy) < 12.0f) return;          /* small wobble = still a tap */
    st_moved = 1;
    float s = st_scroll0 - dy / SEL_RH;
    if (s < 0.0f) s = 0.0f;
    if (s > (float)(g_group_count - 1)) s = (float)(g_group_count - 1);
    g_scroll = s;
    int gi = (int)lroundf(s);
    if (gi != g_sel_group) song_select_set(gi);
}

static void select_touch_up(void) {
    if (!st_active) return;
    int moved = st_moved;
    st_active = st_moved = 0;
    if (moved || !sel_touch_ok()) return;

    if (st_y0 < SEL_LY) {                                /* info box: left / right = difficulty */
        if (st_x0 < SEL_LX + SEL_LW / 3)          song_select_move(0, -1);
        else if (st_x0 > SEL_LX + SEL_LW * 2 / 3) song_select_move(0, 1);
        return;
    }
    int cy = SEL_LY + SEL_LH / 2 - SEL_RH / 2;
    int gi = (int)floorf((st_y0 - cy) / SEL_RH + g_scroll);
    if (gi < 0 || gi >= g_group_count) return;
    if (gi == g_sel_group) start_map(selected_map_index());
    else song_select_set(gi);
}

/* ================= Settings (with live preview) ================= */
enum { S_SKIN, S_SKIN_LAYOUT, S_NOTE_COLOR, S_SPEED, S_OFFSET, S_VOLUME, S_SFX_VOLUME, S_LANE_W, S_HIT_Y,
       S_HUD, S_HEALTH_BAR, S_HIT_ERROR, S_JUDGE_POS, S_SHOW_BG, S_VIDEO, S_BG_DIM, S_PREVIEW, S_DELAY,
       S_TOUCH, S_REAR, S_FILTER, S_PLAYER,
       S_BINDS1, S_BINDS2, S_BINDS3, S_BINDS4, S_BINDS5,
       S_BACK, S_COUNT };
static const char *set_labels[S_COUNT] = {
    "Skin", "Skin layout", "Note colour", "Scroll speed", "Audio offset", "Music volume", "Hitsound volume",
    "Lane width", "Hit position", "Score display", "Health bar", "Hit error bar", "Combo position",
    "Song background", "Video background", "Background dim", "Song preview", "Start delay",
    "Touch screen", "Rear touch (middle)", "Key modes", "Player name",
    "1K buttons...", "2K buttons...", "3K buttons...", "4K buttons...", "5K buttons...",
    "Save & back"
};
#define SET_PANEL_W 472     /* left panel; the preview is on the right */
#define SET_ROW_Y0  58
#define SET_ROW_H   34
#define SET_ROWS    13      /* rows visible at once (the list scrolls) */
static int set_sel = 0, set_top = 0, set_prev_keys = 4, set_old_filter = 0;
static int set_tab = 0;                                 /* 0 = Settings, 1 = Profile */
static int bind_k = 3, bind_sel = 0, bind_wait = 0;     /* bind_k: 0 = 1K ... 4 = 5K */

/* Profile tab: which key mode the stats are for (0 = all, 1-5 = 1K-5K) */
#define PF_MODES 6
static const char *pf_mode_names[PF_MODES] = { "All", "1K", "2K", "3K", "4K", "5K" };
static int pf_mode = 0;

static void settings_open(void) {
    set_sel = 0;
    set_top = 0;
    set_tab = 0;
    set_old_filter = opt_filter;
    screen = SCR_SETTINGS;
}

static void settings_close(void) {
    settings_save();
    if (opt_filter != set_old_filter) build_song_groups();
    lb_for_map = -2;
    screen = SCR_SELECT;
}

/* asks the server for the profile in the chosen key mode */
static void profile_open(void) {
    if (!lb_name_ok()) return;
    pf_request(pf_mode);
}

static void set_tab_switch(int tab) {
    if (tab == set_tab) return;
    set_tab = tab;
    if (set_tab == 1 && pf_state != 1) profile_open();
}

static const char *on_off(int v) { return v ? "On" : "Off"; }

/* does this key mode have a middle lane? (1K, 3K, 5K) */
static int has_middle(int keys) {
    for (int c = 0; c < keys; c++) if (col_type(keys, c) == 3) return 1;
    return 0;
}

/* keeps the selected row on screen (with 2 rows of space when possible) */
static void set_follow(void) {
    if (set_sel < set_top + 2) set_top = set_sel - 2;
    if (set_sel > set_top + SET_ROWS - 3) set_top = set_sel - SET_ROWS + 3;
    set_top = clampi(set_top, 0, S_COUNT - SET_ROWS);
}

static void set_select(int n) {
    set_sel = (n + S_COUNT) % S_COUNT;
    if (set_sel >= S_BINDS1 && set_sel <= S_BINDS5) set_prev_keys = set_sel - S_BINDS1 + 1;
    if (set_sel == S_REAR && !has_middle(set_prev_keys)) set_prev_keys = 5;
    set_follow();
}

static void set_value(int i, char *out, size_t n) {
    int k = kidx(set_prev_keys);
    out[0] = 0;
    switch (i) {
    case S_SKIN:        snprintf(out, n, "%s", skin_names[skin_index()]); break;
    case S_SKIN_LAYOUT: snprintf(out, n, "%s", opt_skin_layout ? "From skin.ini" : "Custom"); break;
    case S_NOTE_COLOR:  snprintf(out, n, "%s", note_color_names[opt_note_color]); break;
    case S_SPEED:       snprintf(out, n, "%d", opt_speed); break;
    case S_OFFSET:      snprintf(out, n, "%+d ms", opt_offset); break;
    case S_VOLUME:      snprintf(out, n, "%d%%", opt_volume); break;
    case S_SFX_VOLUME:  snprintf(out, n, "%d%%", opt_sfx_volume); break;
    case S_LANE_W:
        if (opt_skin_layout && skin.has_col_w[k]) snprintf(out, n, "%d px (skin)", opt_lane_w);
        else snprintf(out, n, "%d px", opt_lane_w);
        break;
    case S_HIT_Y:
        if (opt_skin_layout && skin.hit_pos[k] > 0) snprintf(out, n, "%d (skin)", opt_hit_y);
        else snprintf(out, n, "%d", opt_hit_y);
        break;
    case S_HUD:        snprintf(out, n, "%s", on_off(opt_hud)); break;
    case S_HEALTH_BAR: snprintf(out, n, "%s", on_off(opt_health_bar)); break;
    case S_HIT_ERROR:  snprintf(out, n, "%s", on_off(opt_hit_error)); break;
    case S_JUDGE_POS:  snprintf(out, n, "%s", opt_judge_left ? "Left" : "Centre"); break;
    case S_SHOW_BG:    snprintf(out, n, "%s", on_off(opt_show_bg)); break;
    case S_VIDEO:      snprintf(out, n, "%s", on_off(opt_video)); break;
    case S_BG_DIM:     snprintf(out, n, "%d%%", opt_bg_dim); break;
    case S_PREVIEW:    snprintf(out, n, "%s", on_off(opt_preview)); break;
    case S_DELAY:      snprintf(out, n, "%.2f s", opt_delay / 1000.0); break;
    case S_TOUCH:      snprintf(out, n, "%s", on_off(opt_touch)); break;
    case S_REAR:       snprintf(out, n, "%s", on_off(opt_rear)); break;
    case S_FILTER:     snprintf(out, n, "%s", filter_names[opt_filter]); break;
    case S_PLAYER:     snprintf(out, n, "%s", lb_name[0] ? lb_name : "(not set)"); break;
    case S_BINDS1: case S_BINDS2: case S_BINDS3: case S_BINDS4: case S_BINDS5:
        snprintf(out, n, ">"); break;
    }
}

static void set_change(int i, int d) {
    switch (i) {
    case S_SKIN: {
        int s = (skin_index() + d + skin_count) % skin_count;
        snprintf(opt_skin, sizeof opt_skin, "%s", s ? skin_names[s] : "");
        draw_loading("Loading skin...");
        skin_load(s);
        sfx_load();
        break;
    }
    case S_SKIN_LAYOUT: opt_skin_layout = !opt_skin_layout; break;
    case S_NOTE_COLOR:  opt_note_color = (opt_note_color + d + NOTE_COLORS) % NOTE_COLORS; break;
    case S_SPEED:  opt_speed  = clampi(opt_speed + d, 4, 40); break;
    case S_OFFSET: opt_offset = clampi(opt_offset + d * 5, -500, 500); break;
    case S_VOLUME:
        opt_volume = clampi(opt_volume + d * 5, 0, 100);
        Mix_VolumeMusic(opt_volume * MIX_MAX_VOLUME / 100);
        break;
    case S_SFX_VOLUME:
        opt_sfx_volume = clampi(opt_sfx_volume + d * 5, 0, 100);
        sfx_set_volume();
        sfx_play_hit();                  /* so you can hear the new volume */
        break;
    case S_LANE_W:     opt_lane_w = clampi(opt_lane_w + d * 5, 40, 120); break;
    case S_HIT_Y:      opt_hit_y  = clampi(opt_hit_y + d * 5, 300, 530); break;
    case S_HUD:        opt_hud = !opt_hud; break;
    case S_HEALTH_BAR: opt_health_bar = !opt_health_bar; break;
    case S_HIT_ERROR:  opt_hit_error = !opt_hit_error; break;
    case S_JUDGE_POS:  opt_judge_left = !opt_judge_left; break;
    case S_SHOW_BG:    opt_show_bg = !opt_show_bg; break;
    case S_VIDEO:      opt_video = !opt_video; break;
    case S_BG_DIM:     opt_bg_dim = clampi(opt_bg_dim + d * 5, 0, 100); break;
    case S_PREVIEW:    opt_preview = !opt_preview; break;
    case S_DELAY:      opt_delay = clampi(opt_delay + d * 250, 500, 5000); break;
    case S_TOUCH:      opt_touch = !opt_touch; break;
    case S_REAR:       opt_rear = !opt_rear; break;
    case S_FILTER:     opt_filter = (opt_filter + d + FILTER_COUNT) % FILTER_COUNT; break;
    }
}

/* demo notes that loop forever in the preview (played perfectly, like autoplay) */
#define DEMO_STEP  200
#define DEMO_SLOTS 12
#define DEMO_CYCLE (DEMO_STEP * DEMO_SLOTS)
#define DEMO_NOTES (DEMO_SLOTS * 4)
static Note demo_notes[DEMO_NOTES];

static void draw_settings_preview(void) {
    static const int pat[8] = {0, 1, 2, 3, 4, 3, 2, 1};
    static int demo_last_hit = -1;
    const int PX = SET_PANEL_W, PW = SCREEN_W - SET_PANEL_W;
    int now = DEMO_CYCLE + (int)(SDL_GetTicks() % DEMO_CYCLE);
    int down[MAX_KEYS] = {0};
    int last_hit = -100000;
    char buf[64];

    play_keys = clampi(set_prev_keys, 1, MAX_KEYS);
    for (int i = 0; i < DEMO_NOTES; i++) {
        Note *n = &demo_notes[i];
        int s = i % DEMO_SLOTS;
        int c = pat[s % 8] % play_keys;
        /* keep lane 1 free while its long note is held (matters in 2K / 3K) */
        if (s > 0 && s <= 3 && c == 0 && play_keys > 1) c = 1 + (s % (play_keys - 1));
        memset(n, 0, sizeof *n);
        n->t    = (i / DEMO_SLOTS) * DEMO_CYCLE + s * DEMO_STEP;
        n->col  = s == 0 ? 0 : c;
        n->hold = s == 0 && play_keys > 1;
        n->end  = n->hold ? n->t + 3 * DEMO_STEP : n->t;
        if (n->t <= now) {
            last_hit = n->t;
            if (n->hold && now < n->end) { n->state = N_HOLDING; down[n->col] = 1; }
            else {
                n->state = N_DONE;
                if (now - n->t < 80) down[n->col] = 1;
            }
        }
    }

    SDL_Rect vp = { PX, 0, PW, SCREEN_H };
    SDL_RenderSetViewport(ren, &vp);
    fill(0, 0, PW, SCREEN_H, 12, 12, 20, 255);
    if (opt_show_bg) {
        bg_tick(selected_map_index(), 250);
        if (bg_tex) {
            SDL_Rect src = { (SCREEN_W - PW) / 2, 0, PW, SCREEN_H };
            SDL_SetTextureAlphaMod(bg_tex, 255);
            SDL_RenderCopy(ren, bg_tex, &src, NULL);
            if (opt_bg_dim > 0) fill(0, 0, PW, SCREEN_H, 0, 0, 0, (Uint8)(opt_bg_dim * 255 / 100));
        }
    }

    compute_layout(play_keys);
    int shift = (PW - lay_total) / 2 - lay_x0;
    lay_x0 += shift;
    for (int c = 0; c < play_keys; c++) lay_x[c] += shift;
    draw_field(demo_notes, DEMO_NOTES, 0, now, down);
    if (opt_health_bar) {               /* gently moving demo health */
        float t = (float)(SDL_GetTicks() % 4000) / 4000.0f;
        draw_health_bar(0.65f + 0.3f * sinf(t * 6.2832f));
    }

    int cx = lay_x0 + lay_total / 2;
    int ja, jx = judge_x(cx, &ja);
    if (now - last_hit < 300) draw_text(font_big, jname[J_MAX], jx, lay_judge_y, jcol[J_MAX], ja);
    snprintf(buf, sizeof buf, "%d", (int)(SDL_GetTicks() / DEMO_STEP % 1000) + 1);
    draw_fast(font_big, buf, jx, lay_judge_y - 60, C_WHITE, ja);

    if (opt_hit_error) {                /* demo hit error bar */
        if (hitwin[J_50] <= 0.0f) {     /* no song played yet: use OD 8 windows */
            hitwin[J_MAX] = 16.0f;  hitwin[J_300] = 40.0f;  hitwin[J_200] = 73.0f;
            hitwin[J_100] = 103.0f; hitwin[J_50]  = 127.0f; hitwin[J_MISS] = 164.0f;
        }
        if (last_hit != demo_last_hit) {
            demo_last_hit = last_hit;
            he_add((int)(SDL_GetTicks() % 37) - 18);
        }
        draw_hit_error(cx);
    }
    if (opt_hud) {
        draw_fast(font_big, "727,727", PW - 12, 4, C_WHITE, 2);
        draw_fast(font_small, "99.27%", PW - 12, 50, C_GREY, 2);
    }
    draw_early_late(jx, ja);
    snprintf(buf, sizeof buf, "%dK preview", play_keys);
    draw_text(font_small, buf, 10, 8, C_YELLOW, 0);
    SDL_RenderSetViewport(ren, NULL);
}

/* "Settings | Profile" tabs at the top left (the active one is underlined) */
static void draw_set_tabs(void) {
    int w1 = draw_text(font_big, "Settings", 24, 6, set_tab == 0 ? C_WHITE : C_GREY, 0);
    int px = 24 + w1 + 28;
    int w2 = draw_text(font_big, "Profile", px, 6, set_tab == 1 ? C_WHITE : C_GREY, 0);
    fill(set_tab == 0 ? 24 : px, 48, set_tab == 0 ? w1 : w2, 3, 255, 210, 90, 255);
}

/* ---- Profile tab: your online stats from maniavita.com ---- */
static void draw_profile(void) {
    PfPlay plays[10];
    char buf[300], fitb[300], name[17], msg[96];
    int n, rank, total, st;
    float pp;

    draw_background(selected_map_index(), opt_bg_dim > 80 ? opt_bg_dim : 80, 250);
    fill(0, 0, SCREEN_W, SCREEN_H, 10, 10, 18, 200);
    draw_set_tabs();

    SDL_LockMutex(lb_mx);
    st = pf_state;
    n = pf_play_n;
    memcpy(plays, pf_plays, sizeof(PfPlay) * n);
    rank = pf_rank;
    total = pf_plays_total;
    pp = pf_pp;
    snprintf(name, sizeof name, "%s", pf_name);
    snprintf(msg, sizeof msg, "%s", pf_msg);
    SDL_UnlockMutex(lb_mx);

    if (!lb_name_ok()) {
        draw_text(font_big, "No player name set", 24, 90, C_WHITE, 0);
        draw_text(font_small, "Press L to go back to Settings, then choose \"Player name\".",
                  24, 146, C_GREY, 0);
        draw_text(font_small, "Your profile is created on maniavita.com with your first online score.",
                  24, 174, C_GREY, 0);
        draw_help("L/R: tabs   Start: rescan songs   O: back");
        return;
    }

    /* key mode chips: All 1K 2K 3K 4K 5K */
    for (int i = 0; i < PF_MODES; i++) {
        int x = 24 + i * 64, sel = i == pf_mode;
        if (sel) fill(x, 62, 56, 30, 220, 170, 60, 235);
        else     fill(x, 62, 56, 30, 45, 45, 65, 230);
        draw_text(font_small, pf_mode_names[i], x + 28, 66, sel ? C_DARK : C_WHITE, 1);
    }
    snprintf(buf, sizeof buf, "maniavita.com/u/%s", lb_name);
    draw_text(font_small, buf, SCREEN_W - 24, 66, C_GREY, 2);

    if (st == 1 || st == 0) {
        draw_text(font_small, "Loading...", 24, 112, C_GREY, 0);
    } else if (st == 3) {
        draw_text(font_big, "Offline", 24, 106, C_RED, 0);
        fit_text(font_small, msg[0] ? msg : "couldn't reach maniavita.com", SCREEN_W - 48, fitb, sizeof fitb);
        draw_text(font_small, fitb, 24, 156, C_GREY, 0);
        draw_text(font_small, "Press X to try again.", 24, 184, C_GREY, 0);
    } else if (st == 4) {
        snprintf(buf, sizeof buf, "%s isn't on the server yet", lb_name);
        draw_text(font_big, buf, 24, 106, C_WHITE, 0);
        draw_text(font_small, "Finish a song (without RD / AT / CM) while online to create your profile.",
                  24, 156, C_GREY, 0);
    } else {
        /* name, rank, pp, plays */
        int rw;
        if (rank > 0) {
            snprintf(buf, sizeof buf, "#%d", rank);
            rw = draw_text(font_big, buf, SCREEN_W - 24, 104, C_YELLOW, 2);
        } else {
            rw = draw_text(font_big, "Unranked", SCREEN_W - 24, 104, C_GREY, 2);
        }
        fit_text(font_big, name[0] ? name : lb_name, SCREEN_W - 48 - rw - 20, fitb, sizeof fitb);
        draw_text(font_big, fitb, 24, 104, C_WHITE, 0);

        snprintf(buf, sizeof buf, "%.2f pp     %d play%s     %s", pp, total, total == 1 ? "" : "s",
                 pf_mode ? pf_mode_names[pf_mode] : "All key modes");
        draw_text(font_small, buf, 24, 152, C_WHITE, 0);
        fill(24, 182, SCREEN_W - 48, 2, 255, 255, 255, 40);

        /* top 10 plays */
        draw_text(font_small, "Best plays", 24, 190, C_YELLOW, 0);
        if (n == 0) draw_text(font_small, "No plays in this key mode yet", 24, 222, C_GREY, 0);
        for (int i = 0; i < n; i++) {
            int y = 218 + i * 28;
            if (i % 2 == 0) fill(20, y - 2, SCREEN_W - 40, 28, 255, 255, 255, 14);
            snprintf(buf, sizeof buf, "%d", i + 1);
            draw_text(font_small, buf, 34, y, C_GREY, 0);
            fit_text(font_small, plays[i].title, 560, fitb, sizeof fitb);
            draw_text(font_small, fitb, 64, y, C_WHITE, 0);
            snprintf(buf, sizeof buf, "%dK", plays[i].keys);
            draw_text(font_small, buf, 640, y, C_GREY, 0);
            if (plays[i].mods[0]) {
                snprintf(buf, sizeof buf, "%s", plays[i].mods);
                for (char *p = buf; *p; p++) if (*p == ',') *p = ' ';
                fit_text(font_small, buf, 100, fitb, sizeof fitb);
                draw_text(font_small, fitb, 684, y, C_YELLOW, 0);
            }
            snprintf(buf, sizeof buf, "%.2f%%", plays[i].acc);
            draw_text(font_small, buf, 860, y, C_GREY, 2);
            snprintf(buf, sizeof buf, "%.0fpp", plays[i].pp);
            draw_text(font_small, buf, SCREEN_W - 30, y, C_WHITE, 2);
        }
    }
    draw_help("L/R: tabs   Left/Right: key mode   X: refresh   Start: rescan songs   O: back");
}

static void profile_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        pf_mode = (pf_mode + PF_MODES - 1) % PF_MODES;
        profile_open();
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        pf_mode = (pf_mode + 1) % PF_MODES;
        profile_open();
        break;
    case SDL_CONTROLLER_BUTTON_A:
        profile_open();
        break;
    }
}

static void draw_settings(void) {
    char val[160], buf[200], fitb[200];
    if (set_tab == 1) { draw_profile(); return; }

    draw_settings_preview();
    fill(0, 0, SET_PANEL_W, SCREEN_H, 10, 10, 18, 235);
    draw_set_tabs();
    snprintf(buf, sizeof buf, "%d / %d", set_sel + 1, S_COUNT);
    draw_text(font_small, buf, SET_PANEL_W - 20, 20, C_GREY, 2);

    for (int r = 0; r < SET_ROWS && set_top + r < S_COUNT; r++) {
        int i = set_top + r, y = SET_ROW_Y0 + r * SET_ROW_H, sel = i == set_sel;
        if (sel) {
            fill(10, y - 2, SET_PANEL_W - 30, SET_ROW_H - 2, 255, 210, 90, 60);
            fill(10, y - 2, 4, SET_ROW_H - 2, 255, 210, 90, 255);
        }
        int vw = 0;
        set_value(i, val, sizeof val);
        if (val[0]) {
            if (sel && i < S_BINDS1 && i != S_PLAYER) snprintf(buf, sizeof buf, "< %s >", val);
            else                     snprintf(buf, sizeof buf, "%s", val);
            fit_text(font_mid, buf, 210, fitb, sizeof fitb);
            vw = draw_text(font_mid, fitb, SET_PANEL_W - 28, y, sel ? C_YELLOW : C_GREY, 2);
        }
        fit_text(font_mid, set_labels[i], SET_PANEL_W - 28 - 24 - vw - 14, fitb, sizeof fitb);
        draw_text(font_mid, fitb, 24, y, sel ? C_YELLOW : C_WHITE, 0);
    }

    /* scroll bar */
    int track = SET_ROWS * SET_ROW_H - 2;
    int th = track * SET_ROWS / S_COUNT;
    int ty = SET_ROW_Y0 - 2 + (track - th) * set_top / (S_COUNT - SET_ROWS);
    fill(SET_PANEL_W - 14, SET_ROW_Y0 - 2, 4, track, 255, 255, 255, 40);
    fill(SET_PANEL_W - 14, ty, 4, th, 255, 210, 90, 200);

    draw_help("Up/Down: select  Left/Right: change  X: open  Sq: 1K-5K  L/R: Profile  Start: rescan  O: back");
}

static void settings_button(int b) {
    if (ime_open || !SDL_TICKS_PASSED(SDL_GetTicks(), ime_block_until)) return;

    /* the same on both tabs */
    switch (b) {
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  set_tab_switch(0); return;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_tab_switch(1); return;
    case SDL_CONTROLLER_BUTTON_START:         rescan_songs(); return;
    case SDL_CONTROLLER_BUTTON_B:             settings_close(); return;
    }
    if (set_tab == 1) { profile_button(b); return; }

    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    set_select(set_sel - 1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  set_select(set_sel + 1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  set_change(set_sel, -1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: set_change(set_sel, 1);  break;
    case SDL_CONTROLLER_BUTTON_A:
        if (set_sel >= S_BINDS1 && set_sel <= S_BINDS5) {
            bind_k = set_sel - S_BINDS1;
            bind_sel = 0;
            bind_wait = 0;
            set_prev_keys = bind_k + 1;
            screen = SCR_BINDS;
        } else if (set_sel == S_PLAYER) name_open();
        else if (set_sel == S_BACK) settings_close();
        else set_change(set_sel, 1);
        break;
    case SDL_CONTROLLER_BUTTON_X:                       /* Square: next key mode in the preview */
        set_prev_keys = set_prev_keys % MAX_KEYS + 1;
        break;
    }
}

/* ================= Button bindings ================= */
static void draw_binds(void) {
    char buf[160];
    int keys = bind_k + 1;
    draw_settings_preview();
    fill(0, 0, SET_PANEL_W, SCREEN_H, 10, 10, 18, 235);
    snprintf(buf, sizeof buf, "%dK buttons", keys);
    draw_text(font_big, buf, 24, 6, C_WHITE, 0);
    for (int c = 0; c < keys; c++) {
        int y = 70 + c * 48, sel = c == bind_sel;
        Uint8 r, g, b;
        col_color(keys, c, &r, &g, &b);
        if (sel) {
            fill(10, y - 4, SET_PANEL_W - 20, 42, 255, 210, 90, 60);
            fill(10, y - 4, 4, 42, 255, 210, 90, 255);
        }
        fill(24, y + 3, 20, 26, r, g, b, 255);
        snprintf(buf, sizeof buf, "Lane %d", c + 1);
        draw_text(font_mid, buf, 56, y, sel ? C_YELLOW : C_WHITE, 0);
        const char *v = (sel && bind_wait) ? "Press a button..." : button_name(binds[bind_k][c]);
        draw_text(font_mid, v, SET_PANEL_W - 24, y, sel ? C_YELLOW : C_GREY, 2);
    }
    if (opt_rear)
        for (int c = 0; c < keys; c++)
            if (col_type(keys, c) == 3) {
                snprintf(buf, sizeof buf, "Rear touch also hits lane %d", c + 1);
                draw_text(font_small, buf, 24, 70 + keys * 48 + 10, C_GREY, 0);
            }
    draw_help(bind_wait ? "Press the button you want for this lane"
                        : "Up/Down: lane   X: change   Triangle: defaults   O: back");
}

static void binds_button(int b) {
    int keys = bind_k + 1;
    if (bind_wait) {
        binds[bind_k][bind_sel] = b;
        bind_wait = 0;
        settings_save();
        return;
    }
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:   bind_sel = (bind_sel + keys - 1) % keys; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: bind_sel = (bind_sel + 1) % keys; break;
    case SDL_CONTROLLER_BUTTON_A:         bind_wait = 1; break;
    case SDL_CONTROLLER_BUTTON_Y:
        memcpy(binds[bind_k], default_binds[bind_k], sizeof binds[bind_k]);
        settings_save();
        set_status("Buttons reset to default");
        break;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_START:
        screen = SCR_SETTINGS;
        break;
    }
}

/* ================= Pause menu ================= */
static void pause_button(int b) {
    if (resume_at) {                      /* countdown running */
        if (b == SDL_CONTROLLER_BUTTON_START || b == SDL_CONTROLLER_BUTTON_B) pause_game();
        return;
    }
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        pause_sel = (pause_sel + P_COUNT - 1) % P_COUNT; pause_confirm = 0; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        pause_sel = (pause_sel + 1) % P_COUNT; pause_confirm = 0; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
        int d = b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 1 : -1;
        if (pause_sel == P_SPEED)       opt_speed  = clampi(opt_speed + d, 4, 40);
        else if (pause_sel == P_OFFSET) opt_offset = clampi(opt_offset + d * 5, -500, 500);
        break;
    }
    case SDL_CONTROLLER_BUTTON_A:
        if (pause_sel == P_RESUME) resume_game();
        else if (pause_sel == P_RETRY) { settings_save(); start_map(play_map); }
        else if (pause_sel == P_QUIT) {
            if (!pause_confirm) { pause_confirm = 1; break; }
            settings_save();
            play_cleanup();
            lb_for_map = -2;
            screen = SCR_SELECT;
        }
        break;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_BACK:
        ig_cycle();
        break;
        resume_game();
        break;

    }
}

/* ================= Scores screen ================= */
#define SS_ROWS  8
#define SS_ROW_H 50
static int *ss_list = NULL;
static int ss_n = 0, ss_sel = 0, ss_top = 0, ss_mode = 0, ss_map = -1, ss_del = 0;

static int cmp_ss(const void *a, const void *b) {
    const ScoreEntry *A = &all_scores[*(const int *)a];
    const ScoreEntry *B = &all_scores[*(const int *)b];
    if (ss_mode == 0 && A->score != B->score) return B->score - A->score;   /* best first */
    return A->when < B->when ? 1 : A->when > B->when ? -1 : 0;              /* newest first */
}

static void ss_rebuild(void) {
    int *p = realloc(ss_list, (size_t)(all_count + 1) * sizeof *p);
    if (!p) { ss_n = 0; return; }
    ss_list = p;
    ss_n = 0;
    char key[300] = "";
    if (ss_mode == 0 && ss_map >= 0) map_key(&maps[ss_map], key, sizeof key);
    for (int i = 0; i < all_count; i++)
        if (ss_mode == 1 || (key[0] && !strcmp(all_scores[i].key, key))) ss_list[ss_n++] = i;
    qsort(ss_list, ss_n, sizeof *ss_list, cmp_ss);
    ss_sel = clampi(ss_sel, 0, ss_n > 0 ? ss_n - 1 : 0);
    ss_del = 0;
}

static void ss_follow(void) {
    if (ss_sel < ss_top) ss_top = ss_sel;
    if (ss_sel >= ss_top + SS_ROWS) ss_top = ss_sel - SS_ROWS + 1;
    if (ss_top < 0) ss_top = 0;
}

static void scores_open(int from_results) {
    ss_map = from_results ? play_map : selected_map_index();
    ss_mode = ss_map < 0 ? 1 : 0;
    ss_sel = ss_top = 0;
    ss_rebuild();
    if (from_results && res_rank > 0)          /* highlight the score you just set */
        for (int i = 0; i < ss_n; i++)
            if (ss_list[i] == all_count - 1) ss_sel = i;
    ss_follow();
    screen = SCR_SCORES;
}

static void ss_change_map(int dy, int dx) {
    if (ss_mode != 0 || g_group_count == 0) return;
    song_select_move(dy, dx);
    ss_map = selected_map_index();
    ss_sel = ss_top = 0;
    ss_rebuild();
}

static void draw_scores(void) {
    char buf[320], fitb[320], sc[24], date[40], mb[64];
    int idx = (ss_mode == 0 && ss_map >= 0) ? ss_map : selected_map_index();
    draw_background(idx, opt_bg_dim > 80 ? opt_bg_dim : 80, 250);

    draw_text(font_big, ss_mode == 0 ? "Scores - this difficulty" : "Scores - all maps", 16, 6, C_WHITE, 0);
    if (ss_n) {
        snprintf(buf, sizeof buf, "%d / %d", ss_sel + 1, ss_n);
        draw_text(font_small, buf, 944, 16, C_GREY, 2);
    }
    if (ss_mode == 0 && ss_map >= 0) {
        const MapInfo *m = &maps[ss_map];
        snprintf(buf, sizeof buf, "%s - %s  [%dK] %s  %.2f*", m->artist, m->title, m->keys,
                 m->version, stars_shown(m->stars));
    } else snprintf(buf, sizeof buf, "%d score%s saved, newest first", all_count, all_count == 1 ? "" : "s");
    fit_text(font_small, buf, 928, fitb, sizeof fitb);
    draw_text(font_small, fitb, 16, 50, C_GREY, 0);

    /* list */
    const int LX = 16, LY = 80, LW = 456, PH = SS_ROWS * SS_ROW_H + 12;
    fill(LX, LY, LW, PH, 0, 0, 0, 170);
    if (!ss_n) draw_text(font_small, "No scores yet", LX + 14, LY + 14, C_GREY, 0);
    for (int r = 0; r < SS_ROWS && ss_top + r < ss_n; r++) {
        int li = ss_top + r, sel = li == ss_sel;
        const ScoreEntry *e = &all_scores[ss_list[li]];
        int y = LY + 6 + r * SS_ROW_H;
        if (sel) {
            fill(LX + 6, y, LW - 12, SS_ROW_H - 6, 90, 75, 40, 220);
            fill(LX + 6, y, 4, SS_ROW_H - 6, 255, 210, 90, 255);
        } else fill(LX + 6, y, LW - 12, SS_ROW_H - 6, 45, 45, 65, 210);

        SDL_Color gc;
        const char *g = lb_grade(e->acc, &gc);
        draw_text(font_big, g, LX + 16, y - 2, gc, 0);
        fmt_score(e->score, sc, sizeof sc);
        fmt_date(e->when, "%Y-%m-%d", date, sizeof date);

        int modw = 0;                                    /* mods on the right */
        if (e->mods) {
            mods_string(e->mods, mb, sizeof mb);
            fit_text(font_small, mb, 110, fitb, sizeof fitb);
            modw = draw_text(font_small, fitb, LX + LW - 16, y + 1, C_YELLOW, 2) + 10;
        }
        if (ss_mode == 0) {
            snprintf(buf, sizeof buf, "#%d   %s", li + 1, sc);
            draw_text(font_small, buf, LX + 76, y + 1, sel ? C_YELLOW : C_WHITE, 0);
            snprintf(buf, sizeof buf, "%.2f%%   %dx   %s", e->acc, e->combo, date);
        } else {
            key_label(e->key, buf, sizeof buf);
            fit_text(font_small, buf, LW - 100 - modw, fitb, sizeof fitb);
            draw_text(font_small, fitb, LX + 76, y + 1, sel ? C_YELLOW : C_WHITE, 0);
            snprintf(buf, sizeof buf, "%s   %.2f%%   %s", sc, e->acc, date);
        }
        int tw = draw_text(font_small, buf, LX + 76, y + 22, C_GREY, 0);
        SDL_Color fcc;
        const char *fc = fc_tag(e->n, &fcc);
        if (fc) draw_text(font_small, fc, LX + 76 + tw + 10, y + 22, fcc, 0);
    }

    /* details of the selected score */
    const int DX = 488, DW = 456;
    fill(DX, LY, DW, PH, 0, 0, 0, 170);
    if (ss_n) {
        int si = ss_list[ss_sel];
        const ScoreEntry *e = &all_scores[si];
        int total = 0, rank = score_rank(si, &total);
        SDL_Color gc;
        const char *g = lb_grade(e->acc, &gc);

        draw_text_scaled(font_big, g, DX + 70, LY + 64, gc, 2);
        fmt_score(e->score, sc, sizeof sc);
        draw_text(font_big, sc, DX + 140, LY + 14, C_WHITE, 0);
        SDL_Color fcc;
        const char *fc = fc_tag(e->n, &fcc);             /* FC / PFC, top right */
        if (fc) draw_text(font_mid, fc, DX + DW - 16, LY + 20, fcc, 2);
        snprintf(buf, sizeof buf, "Rank #%d of %d on this map", rank, total);
        draw_text(font_small, buf, DX + 140, LY + 64, C_GREY, 0);

        mods_string(e->mods, mb, sizeof mb);
        if (mb[0]) snprintf(buf, sizeof buf, "Mods  %s  (x%.2f)", mb, mods_multiplier(e->mods));
        else       snprintf(buf, sizeof buf, "No mods");
        fit_text(font_small, buf, DW - 160, fitb, sizeof fitb);
        draw_text(font_small, fitb, DX + 140, LY + 90, mb[0] ? C_YELLOW : C_GREY, 0);

        snprintf(buf, sizeof buf, "Accuracy  %.2f%%      Max combo  %dx", e->acc, e->combo);
        draw_text(font_small, buf, DX + 20, LY + 130, C_WHITE, 0);
        for (int j = 0; j < 6; j++) {
            int x = DX + 20 + (j / 3) * 220, y = LY + 170 + (j % 3) * 30;
            draw_text(font_small, jname[j], x, y, jcol[j], 0);
            snprintf(buf, sizeof buf, "%d", e->n[j]);
            draw_text(font_small, buf, x + 190, y, C_WHITE, 2);
        }
        fmt_ratio(e->n[J_MAX], e->n[J_300], fitb, sizeof fitb);
        snprintf(buf, sizeof buf, "MAX:300 ratio   %s", fitb);
        draw_text(font_small, buf, DX + 20, LY + 270, C_WHITE, 0);
        fmt_date(e->when, "%Y-%m-%d  %H:%M", date, sizeof date);
        snprintf(buf, sizeof buf, "Played   %s", date);
        draw_text(font_small, buf, DX + 20, LY + 300, C_GREY, 0);
        if (ss_mode == 1) {
            key_label(e->key, buf, sizeof buf);
            fit_text(font_small, buf, DW - 40, fitb, sizeof fitb);
            draw_text(font_small, fitb, DX + 20, LY + 330, C_GREY, 0);
        }
        if (ss_del)
            draw_text(font_small, "Press Triangle again to delete this score", DX + DW / 2, LY + 372, C_RED, 1);
    }

    draw_help(ss_mode == 0
        ? "Up/Down: score  Left/Right: diff  L/R: song  Sq: all scores  X: play  Tri x2: delete  O: back"
        : "Up/Down: score   Sq: this difficulty   X: play   Tri x2: delete   O: back");
}

static void scores_button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        if (ss_n) ss_sel = (ss_sel + ss_n - 1) % ss_n;
        ss_del = 0;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        if (ss_n) ss_sel = (ss_sel + 1) % ss_n;
        ss_del = 0;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     ss_change_map(0, -1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    ss_change_map(0, 1);  break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  ss_change_map(-1, 0); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: ss_change_map(1, 0);  break;
    case SDL_CONTROLLER_BUTTON_X:
        ss_mode = !ss_mode;
        if (ss_mode == 0 && ss_map < 0) ss_mode = 1;
        ss_sel = ss_top = 0;
        ss_rebuild();
        break;
    case SDL_CONTROLLER_BUTTON_A: {
        int idx = ss_mode == 0 ? ss_map : (ss_n ? map_from_key(all_scores[ss_list[ss_sel]].key) : -1);
        if (idx < 0) { set_status(ss_n ? "That map isn't installed any more" : "No map selected"); break; }
        select_map(idx);
        start_map(idx);
        return;
    }
    case SDL_CONTROLLER_BUTTON_Y:
        if (!ss_n) break;
        if (!ss_del) { ss_del = 1; break; }
        scores_delete(ss_list[ss_sel]);
        ss_rebuild();
        set_status("Score deleted");
        break;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_BACK:
        screen = SCR_SELECT;
        break;
    }
    ss_follow();
}

/* ================= Input dispatch ================= */
static void on_button(int b) {
    switch (screen) {
    case SCR_SELECT:   select_button(b);   break;
    case SCR_SETTINGS: settings_button(b); break;
    case SCR_BINDS:    binds_button(b);    break;
    case SCR_PLAY:     if (paused) pause_button(b); break;
    case SCR_RESULTS:  results_button(b);  break;
    case SCR_SCORES:   scores_button(b);   break;
    case SCR_FAILED:   failed_button(b);   break;
    }
}

static int is_repeat_button(int b) {
    return b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
           b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ||
           b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
}

static void button_down(int b) {
    if (b < 0) return;
    if (screen == SCR_PLAY && !paused) {
        if (b == SDL_CONTROLLER_BUTTON_START) { pause_game(); return; }
        if (b == SDL_CONTROLLER_BUTTON_BACK) {          /* Select: leaderboard Global/Local/Off */
            int bound = 0, kk = kidx(play_keys);
            for (int c = 0; c < play_keys; c++) if (binds[kk][c] == b) bound = 1;
            if (!bound) { ig_cycle(); return; }
        }
        int k = kidx(play_keys);
        for (int c = 0; c < play_keys; c++)
            if (binds[k][c] == b) lane_down(c);
        return;
    }
    int shoulder = b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    int leftright = b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    int can_repeat = is_repeat_button(b) && !(screen == SCR_BINDS && bind_wait)
                  && !(screen == SCR_SETTINGS && shoulder)                    /* L/R = tabs */
                  && !(screen == SCR_SETTINGS && set_tab == 1 && leftright);  /* key mode: 1 request per press */
    on_button(b);
    if (can_repeat) { rep_btn = b; rep_action = screen; rep_next = SDL_GetTicks() + 350; }
}

static void button_up(int b) {
    if (b < 0) return;
    if (b == rep_btn) rep_btn = -1;
    if (screen == SCR_PLAY && !paused) {
        int k = kidx(play_keys);
        for (int c = 0; c < play_keys; c++)
            if (binds[k][c] == b) lane_up(c);
    }
}

/* touchscreens: front = lanes during a song / song list on song select, rear = middle lane */
static void handle_finger(const SDL_TouchFingerEvent *f, Uint32 type) {
    if (has_rear && f->touchId == rear_id) {
        if (screen != SCR_PLAY) return;
        if (type == SDL_FINGERDOWN)    play_rear_down();
        else if (type == SDL_FINGERUP) play_rear_up();
        return;
    }
    if (!has_front || f->touchId != front_id) return;
    if (screen == SCR_PLAY) {
        if (type == SDL_FINGERDOWN)        play_touch_down(f->fingerId, f->x);
        else if (type == SDL_FINGERMOTION) play_touch_move(f->fingerId, f->x);
        else                               play_touch_up(f->fingerId);
    } else if (screen == SCR_SELECT) {
        if (type == SDL_FINGERDOWN)        select_touch_down(f->x, f->y);
        else if (type == SDL_FINGERMOTION) select_touch_move(f->x, f->y);
        else                               select_touch_up();
    }
}

/* keyboard (for PC testing) */
static int key_to_button(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP:        return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case SDLK_DOWN:      return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDLK_LEFT:      return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDLK_RIGHT:     return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    case SDLK_RETURN:    return SDL_CONTROLLER_BUTTON_A;
    case SDLK_BACKSPACE:
    case SDLK_ESCAPE:    return SDL_CONTROLLER_BUTTON_B;
    case SDLK_q:         return SDL_CONTROLLER_BUTTON_X;
    case SDLK_w:         return SDL_CONTROLLER_BUTTON_Y;
    case SDLK_TAB:       return SDL_CONTROLLER_BUTTON_BACK;
    case SDLK_F5:        return SDL_CONTROLLER_BUTTON_START;
    case SDLK_PAGEUP:    return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case SDLK_PAGEDOWN:  return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    }
    return -1;
}

static void make_dir(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n > 1 && tmp[n - 1] == '/') tmp[n - 1] = 0;
    mkdir(tmp, 0777);
}

/* ================= Main ================= */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
#ifdef __vita__
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
        return 1;
    SDL_Window *win = SDL_CreateWindow("mania vita!", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCREEN_W, SCREEN_H, 0);
    if (!win) return 1;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) return 1;

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    TTF_Init();
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, AUDIO_CHUNK) == 0) {
        audio_clock_init();
        Mix_AllocateChannels(16);         /* room for fast hitsounds that overlap */
    }
    lb_init();
    lb_ca_load();
    make_dir(DATA_DIR);
    make_dir(DATA_DIR "songs");

    /* ux0:data/osumania/font.ttf first, otherwise the font inside the .vpk */
    font_small = open_font(18);
    font_mid   = open_font(24);
    font_big   = open_font(36);

    settings_load();
    Mix_VolumeMusic(opt_volume * MIX_MAX_VOLUME / 100);
    draw_loading("Loading skin...");
    skins_scan();
    skin_load(skin_index());
    sfx_load();
    draw_loading("Loading songs...");
    scan_songs();
    scores_load();
    build_song_groups();

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { if (!pad) pad = SDL_GameControllerOpen(i); }
        else if (!joy) joy = SDL_JoystickOpen(i);
    }
    /* Vita: touch device 0 = front screen, 1 = rear touchpad */
    int touch_devs = SDL_GetNumTouchDevices();
    if (touch_devs >= 1) { front_id = SDL_GetTouchDevice(0); has_front = 1; }
    if (touch_devs >= 2) { rear_id  = SDL_GetTouchDevice(1); has_rear  = 1; }

    screen = SCR_SELECT;
    Uint32 last_frame = SDL_GetTicks();
    int running = 1;

    while (running) {
        frame_no++;

        /* the game was stopped (PS button / sleep): pause the song */
        Uint32 now = SDL_GetTicks();
        if (screen == SCR_PLAY && !paused && music_started && now - last_frame > 400) pause_game();
        last_frame = now;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;

            case SDL_CONTROLLERDEVICEADDED:
                if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN: button_down(e.cbutton.button); break;
            case SDL_CONTROLLERBUTTONUP:   button_up(e.cbutton.button);   break;
            case SDL_JOYBUTTONDOWN:
                if (!pad && e.jbutton.button < 12) button_down(vita_joy_map[e.jbutton.button]);
                break;
            case SDL_JOYBUTTONUP:
                if (!pad && e.jbutton.button < 12) button_up(vita_joy_map[e.jbutton.button]);
                break;

            case SDL_KEYDOWN: {
                SDL_Keycode sym = e.key.keysym.sym;
                if (screen == SCR_PLAY && !paused) {
                    if (e.key.repeat) break;
                    if (sym == SDLK_ESCAPE) { pause_game(); break; }
                    int k = kidx(play_keys);
                    for (int c = 0; c < play_keys; c++)
                        if (kb_keys[k][c] && kb_keys[k][c] == sym) lane_down(c);
                } else {
                    int b = key_to_button(sym);
                    if (screen == SCR_PLAY && sym == SDLK_ESCAPE) b = SDL_CONTROLLER_BUTTON_START;
                    if (b >= 0) on_button(b);
                }
                break;
            }
            case SDL_KEYUP:
                if (screen == SCR_PLAY && !paused) {
                    int k = kidx(play_keys);
                    for (int c = 0; c < play_keys; c++)
                        if (kb_keys[k][c] && kb_keys[k][c] == e.key.keysym.sym) lane_up(c);
                }
                break;

            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                handle_finger(&e.tfinger, e.type);
                break;

            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                    e.window.event == SDL_WINDOWEVENT_MINIMIZED)
                    if (screen == SCR_PLAY) pause_game();
                break;
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_DIDENTERBACKGROUND:
                if (screen == SCR_PLAY) pause_game();
                break;
            }
        }

        /* holding a direction button repeats it in menus */
        if (rep_btn >= 0) {
            if (rep_action != screen || (screen == SCR_PLAY && !paused) || !btn_held(rep_btn))
                rep_btn = -1;
            else if (SDL_TICKS_PASSED(SDL_GetTicks(), rep_next)) {
                rep_next = SDL_GetTicks() + (screen == SCR_SELECT ? 70 : 90);
                on_button(rep_btn);
            }
        }

        search_update();
        if (screen == SCR_PLAY) update_play();
        else if (screen != SCR_RESULTS && screen != SCR_FAILED)
            preview_update((screen == SCR_SCORES && ss_mode == 0 && ss_map >= 0) ? ss_map
                                                                                 : selected_map_index());

        SDL_SetRenderDrawColor(ren, 12, 12, 20, 255);
        SDL_RenderClear(ren);
        switch (screen) {
        case SCR_SELECT:   draw_select();   break;
        case SCR_SETTINGS: draw_settings(); break;
        case SCR_BINDS:    draw_binds();    break;
        case SCR_PLAY:     draw_play();     break;
        case SCR_RESULTS:  draw_results();  break;
        case SCR_SCORES:   draw_scores();   break;
        case SCR_FAILED:   draw_failed();   break;
        }
        if (screen == SCR_SELECT) draw_sort_menu();   /* menu first... */
        draw_status();                                /* ...so messages show on top */
        SDL_RenderPresent(ren);
    }

    preview_stop();
    bg_finish(1);
    play_cleanup();
    settings_save();
    sfx_free();
    lb_quit();
    Mix_CloseAudio();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 0;
}

