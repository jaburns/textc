// -----------------------------------------------------------------------------
// textc - offline text mesh compilation tool
// Copyright (c) 2025 Jeremy Burns
// -----------------------------------------------------------------------------
// ISSUES:
//  - The bounds coming in from msdfgen are snapped to the nearest pixel when
//    computing the uvs, but there's no fundamental reason they have to be.
//  - The glyph boundaries computed by pango are not an exact match for the
//    the ones msdfden generates. It seems like the pango boundaries map to the
//    rendered bounds of the glyphs after hinting/aa/etc, while the msdfgen ones
//    are read directly from the glyph shapes. This causes a slight mismatch
//    between the results from rendering the generated meshes data vs the
//    example outputs from pango. In practice it basically doesn't matter, but
//    if you really squint at the pixels the results aren't totally correct.
// -----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <sys/mman.h>

#include <glib.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include "vendor/lodepng.h"

// -----------------------------------------------------------------------------
// config

#define ENABLE_DEBUG_OUTPUT 1
#define ENABLE_DEBUG_GLYPH_BOUNDS 0
#define MSDFGEN_PX_RANGE 2
#define GLYPH_PADDING 2
#define CACHE_FILE_NAME ".cache"

// -----------------------------------------------------------------------------

#define Assert g_assert
#define Panic(...)                    \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n");        \
        raise(SIGTRAP);               \
        exit(1);                      \
    } while (0)

#define Log(msg) printf("textc: %s\n", msg)

// -----------------------------------------------------------------------------
// memory

#define ARENA_ADDRESS_SPACE_SIZE (1 << 30)
#define ARENA_BLOCK_SIZE 16384

typedef struct {
    uint8_t* head;
    uint8_t* tail;
    size_t blocks_committed;
} Arena;

static Arena arena_create(void) {
    void* ptr = mmap(NULL, ARENA_ADDRESS_SPACE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) Panic("mmap failed");

    return (Arena){
        .head = ptr,
        .tail = ptr,
        .blocks_committed = 0,
    };
}

static void arena_destroy(Arena* arena) {
    int32_t result = munmap(arena->head, ARENA_ADDRESS_SPACE_SIZE);
    if (result == -1) Panic("munmap failed");
    memset(arena, 0, sizeof(Arena));
}

static void arena_resize(Arena* arena, size_t total_size) {
    if (total_size > ARENA_ADDRESS_SPACE_SIZE) Panic("arena overflow");

    size_t total_blocks_required = 1 + total_size / ARENA_BLOCK_SIZE;

    if (total_blocks_required > arena->blocks_committed) {
        size_t cur_size = ARENA_BLOCK_SIZE * arena->blocks_committed;
        size_t add_size = total_blocks_required - arena->blocks_committed;
        arena->blocks_committed = total_blocks_required;

        int32_t result = mprotect(arena->head + cur_size, add_size * ARENA_BLOCK_SIZE, PROT_READ | PROT_WRITE);
        if (result == -1) {
            Panic("mprotect failed");
        }
    } else if (total_blocks_required < arena->blocks_committed) {
        size_t remove_size = ARENA_BLOCK_SIZE - total_blocks_required;
        arena->blocks_committed = total_blocks_required;
        size_t new_size = ARENA_BLOCK_SIZE * arena->blocks_committed;

        int32_t result = madvise(arena->head + new_size, remove_size * ARENA_BLOCK_SIZE, MADV_DONTNEED);
        if (result == -1) Panic("madvise failed");
    }
}

static void* arena_alloc(Arena* arena, size_t size) {
    uint8_t* ret = arena->tail;
    arena->tail += size;
    arena_resize(arena, arena->tail - arena->head);
    return ret;
}

static void arena_clear(Arena* arena) {
    arena->tail = arena->head;
    arena_resize(arena, 0);
}

#define ArenaOf(_t) Arena
#define ArenaPushT(type, arena_ptr) ((type*)arena_alloc((arena_ptr), sizeof(type)))
#define ArenaPopT(type, arena_ptr) ((arena_ptr)->tail -= sizeof(type), (type*)((arena_ptr)->tail))
#define ArenaGetT(type, arena_ptr, idx) (&((type*)(arena_ptr)->head)[idx])
#define ArenaCountT(type, arena_ptr) (((arena_ptr)->tail - (arena_ptr)->head) / sizeof(type))

// -----------------------------------------------------------------------------
// io utils

#define FRead(ptr, size, nitems, stream) \
    if (!fread(ptr, size, nitems, stream)) Panic("file read failed")

#define FWriteValue(type, value, stream)         \
    do {                                         \
        type val = (value);                      \
        fwrite(&val, sizeof(type), 1, (stream)); \
    } while (0)

static char* read_file(Arena* arena, char* filename, uint32_t* out_length) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        Panic("Failed to open file: %s", filename);
    }

    fseek(file, 0, SEEK_END);
    uint32_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* content = arena_alloc(arena, file_size + 1);
    FRead(content, 1, file_size, file);
    content[file_size] = 0;
    fclose(file);

    if (out_length != NULL) {
        *out_length = file_size;
    }
    return content;
}

static uint32_t file_count_lines(char* contents, uint32_t length, uint32_t* out_max_line_length) {
    uint32_t ret = 0;
    uint32_t cur_line_length = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (contents[i] == '\n') {
            if (out_max_line_length && cur_line_length > *out_max_line_length) {
                *out_max_line_length = cur_line_length;
            }
            cur_line_length = 0;
            ret++;
        }
        cur_line_length++;
    }
    return ret;
}

static void file_write_padded_string(FILE* file, char* string, uint8_t len) {
    static const uint32_t zeroes = 0;
    fwrite(&len, sizeof(len), 1, file);
    fwrite(string, sizeof(char), len, file);
    fwrite(&zeroes, sizeof(char), -(len + 1) & 3, file);
}

static char* read_cmd(Arena* arena, char* cmd, uint32_t* out_length) {
    char buffer[1024];
    char* ret = arena_alloc(arena, 0);
    *out_length = 0;

    FILE* pipe = popen(cmd, "r");
    if (pipe == NULL) Panic("!");
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        size_t read_len = strnlen(buffer, 1024);
        *out_length += read_len;
        char* write = arena_alloc(arena, read_len);
        memcpy(write, buffer, read_len);
    }
    if (pclose(pipe) == -1) Panic("!");

    *(char*)arena_alloc(arena, 1) = '\0';

    return ret;
}

#define HASH_DJB2_INIT 5381

static void hash_djb2_acc(uint32_t* hash, void* data, size_t count, size_t stride) {
    uint8_t* str = data;
    while (count-- > 0) {
        *hash = (*hash << 5) + (*hash ^ *str);
        str += stride;
    }
}

static uint32_t hash_djb2(void* data, size_t count, size_t stride) {
    uint32_t hash = HASH_DJB2_INIT;
    hash_djb2_acc(&hash, data, count, stride);
    return hash;
}

// -----------------------------------------------------------------------------
// csv formats

typedef struct {
    char* face;
    uint32_t size;
    float lineheight;
} TextStyle;

typedef struct {
    char* name;
    TextStyle style;
} StylesCsvEntry;

typedef struct {
    char* key;
    uint32_t width;
    uint32_t height;
    char** languages;
} StringsCsvEntry;

typedef struct {
    char* name;
} StringsCsvLanguage;

typedef struct {
    StylesCsvEntry* styles;
    uint32_t styles_count;

    StringsCsvEntry* strings;
    uint32_t strings_count;
    uint32_t max_string_length;

    char** languages;
    uint32_t language_count;

    uint32_t hash;
    bool cached_hash_matched;
} InputCsv;

static void parse_csv(
    Arena* arena,
    char* file_contents,
    uint32_t file_length,
    InputCsv* ctx,
    void (*on_header)(Arena* arena, InputCsv* ctx, char** items, uint32_t item_count),
    void (*on_row)(Arena* arena, InputCsv* ctx, char** items, uint32_t item_count)
) {
    ArenaOf(char*) items = arena_create();
    bool inside_quotes = false;
    bool done_header = false;
    char* cursor = file_contents;
    char* head = arena_alloc(arena, file_length);
    char* tail = head;

    for (;;) {
        char c = *cursor;
        if (inside_quotes) {
            if (c == '"') {
                if (*(cursor + 1) == '"') {
                    *tail++ = '"';
                    cursor++;
                } else {
                    inside_quotes = false;
                }
            } else {
                *tail++ = *cursor;
            }
        } else if (c == '"') {
            inside_quotes = true;
        } else if (c == ',' || c == '\n' || c == '\0') {
            *tail++ = '\0';
            *ArenaPushT(char*, &items) = head;
            head = tail;
            if (c != ',') {
                uint32_t item_count = ArenaCountT(char*, &items);
                if (**ArenaGetT(char*, &items, 0) != '\0' && item_count > 1) {
                    if (done_header) {
                        on_row(arena, ctx, (char**)items.head, item_count);
                    } else if (on_header) {
                        on_header(arena, ctx, (char**)items.head, item_count);
                    }
                    done_header = true;
                }
                arena_clear(&items);
            }
            if (c == '\0') break;
        } else {
            *tail++ = *cursor;
        }
        cursor++;
    }

    arena_destroy(&items);
}

static void parse_styles_csv_row(Arena* arena, InputCsv* input, char** items, uint32_t item_count) {
    Assert(item_count == 4);
    StylesCsvEntry* entry = &input->styles[input->styles_count++];
    entry->name = items[0];
    entry->style.face = items[1];
    entry->style.size = atoi(items[2]);
    entry->style.lineheight = atof(items[3]);
}

#define STRINGS_CSV_PARAM_ENTRIES 3

static void parse_strings_csv_header(Arena* arena, InputCsv* input, char** items, uint32_t item_count) {
    Assert(item_count > STRINGS_CSV_PARAM_ENTRIES);
    input->language_count = item_count - STRINGS_CSV_PARAM_ENTRIES;
    input->languages = arena_alloc(arena, input->language_count * sizeof(char*));
    for (uint32_t i = 0; i < input->language_count; ++i) {
        input->languages[i] = items[i + STRINGS_CSV_PARAM_ENTRIES];
    }
}

static void parse_strings_csv_row(Arena* arena, InputCsv* input, char** items, uint32_t item_count) {
    Assert(item_count == STRINGS_CSV_PARAM_ENTRIES + input->language_count);
    StringsCsvEntry* entry = &input->strings[input->strings_count++];
    entry->key = items[0];
    entry->width = atoi(items[1]);
    entry->height = atoi(items[2]);
    entry->languages = arena_alloc(arena, input->language_count * sizeof(char*));
    for (uint32_t i = 0; i < input->language_count; ++i) {
        entry->languages[i] = items[STRINGS_CSV_PARAM_ENTRIES + i];
    }
}

static InputCsv parse_input_files(Arena* arena) {
    Arena scratch = arena_create();

    InputCsv ret = {0};

    ret.hash = HASH_DJB2_INIT;

    uint32_t styles_length;
    char* styles_contents = read_file(&scratch, "styles.csv", &styles_length);
    uint32_t strings_length;
    char* strings_contents = read_file(&scratch, "strings.csv", &strings_length);

    hash_djb2_acc(&ret.hash, styles_contents, styles_length, 1);
    hash_djb2_acc(&ret.hash, strings_contents, strings_length, 1);

    FILE* file = fopen(CACHE_FILE_NAME, "rb+");

    if (file) {
        uint32_t old_hash;
        FRead(&old_hash, sizeof(uint32_t), 1, file);
        fseek(file, 0, SEEK_SET);
        FWriteValue(uint32_t, ret.hash, file);
        fclose(file);

        if (old_hash == ret.hash) {
            ret.cached_hash_matched = true;
            goto end;
        }
    }

    uint32_t max_styles = file_count_lines(styles_contents, styles_length, NULL);
    ret.styles = arena_alloc(arena, max_styles * sizeof(StylesCsvEntry));
    parse_csv(arena, styles_contents, styles_length, &ret, NULL, parse_styles_csv_row);

    uint32_t max_strings = file_count_lines(strings_contents, strings_length, &ret.max_string_length);
    ret.strings = arena_alloc(arena, max_strings * sizeof(StringsCsvEntry));
    parse_csv(arena, strings_contents, strings_length, &ret, parse_strings_csv_header, parse_strings_csv_row);

end:
    arena_destroy(&scratch);
    return ret;
}

// -----------------------------------------------------------------------------
// font loading

typedef struct {
    char* face;
    char* family_name;
    PangoFontDescription* pango_font_desc;
} LoadedFont;

typedef struct {
    LoadedFont* elems;
    uint32_t count;
} LoadedFonts;

static LoadedFont* find_font_by_family_name(LoadedFonts* loaded_fonts, char* family_name) {
    for (int32_t i = 0; i < loaded_fonts->count; ++i) {
        if (!strcmp(loaded_fonts->elems[i].family_name, family_name)) {
            return &loaded_fonts->elems[i];
        }
    }
    return NULL;
}

static LoadedFont* find_font_by_face(LoadedFonts* loaded_fonts, char* face) {
    for (int32_t i = 0; i < loaded_fonts->count; ++i) {
        if (!strcmp(loaded_fonts->elems[i].face, face)) {
            return &loaded_fonts->elems[i];
        }
    }
    return NULL;
}

static LoadedFonts load_fonts(Arena* arena) {
    const char* file;

    LoadedFonts ret = {0};

    GDir* dir = g_dir_open(".", 0, NULL);
    if (dir == NULL) Panic("Could not open directory: ./\n");
    while ((file = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(file, ".ttf")) continue;
        ++ret.count;
    }
    g_dir_close(dir);

    FcConfig* config = FcConfigCreate();
    FcConfigSetCurrent(config);

    ret.elems = arena_alloc(arena, ret.count * sizeof(LoadedFont));
    uint32_t i = 0;

    dir = g_dir_open(".", 0, NULL);
    while ((file = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(file, ".ttf")) continue;

        FcBool result = FcConfigAppFontAddFile(config, (FcChar8*)file);
        if (result == FcFalse) Panic("Failed to load font: %s\n", file);

        FcPattern* pat = FcPatternCreate();
        FcPatternAddString(pat, FC_FILE, (FcChar8*)file);
        FcConfigSubstitute(config, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult fc_result;
        FcPattern* font_match = FcFontMatch(config, pat, &fc_result);
        if (!font_match) Panic("Failed to match font.\n");

        FcChar8* family_name = NULL;
        if (FcPatternGetString(font_match, FC_FAMILY, 0, &family_name) != FcResultMatch) {
            Panic("Failed to get font family name.\n");
        }

        char* file_dot = strrchr(file, '.');
        uint32_t face_len = file_dot - file;

        char* face = arena_alloc(arena, face_len + 1);
        memcpy(face, file, face_len);
        face[face_len] = 0;

        ret.elems[i].face = face;
        ret.elems[i].family_name = (char*)family_name;
        ret.elems[i].pango_font_desc = pango_font_description_from_string((const char*)family_name);
        i++;
    }

    g_dir_close(dir);

    return ret;
}

// -----------------------------------------------------------------------------
// pango text shaping callback

typedef struct {
    char* face;
    uint32_t uid;
    uint32_t id;
} GlyphId;

typedef struct {
    float x0, y0, x1, y1;
    uint32_t source_idx;
    uint32_t glyph_uid;
} TypesetGlyph;

typedef struct _ShimRenderer {
    PangoRenderer parent_instance;
    LoadedFonts* loaded_fonts;
    char* cur_face;
    uint32_t cur_source_offset;
    ArenaOf(GlyphId) used_glyphs;
    ArenaOf(TypesetGlyph) typeset_glyphs;
} ShimRenderer;

typedef struct _ShimRendererClass {
    PangoRendererClass parent_class;
} ShimRendererClass;

G_DEFINE_TYPE(ShimRenderer, shim_renderer, PANGO_TYPE_RENDERER)

static uint64_t get_glyph_uid(char* face, uint32_t id) {
    uint32_t face_hash = hash_djb2(face, strnlen(face, 255), 1);
    return ((uint64_t)face_hash << 32) | ((uint64_t)id);
}

static void shim_renderer_prepare_run(PangoRenderer* renderer0, PangoLayoutRun* run) {
    ShimRenderer* renderer = (ShimRenderer*)renderer0;
    renderer->cur_source_offset = run->item->offset;

    PangoFontDescription* font_desc = pango_font_describe(run->item->analysis.font);
    renderer->cur_face = find_font_by_family_name(renderer->loaded_fonts, (char*)pango_font_description_get_family(font_desc))->face;
    pango_font_description_free(font_desc);
}

static void shim_renderer_draw_glyphs(PangoRenderer* renderer0, PangoFont* font, PangoGlyphString* glyphs, int x, int y) {
    ShimRenderer* renderer = (ShimRenderer*)renderer0;

    double base_x = (double)x / (double)PANGO_SCALE;
    double base_y = (double)y / (double)PANGO_SCALE;

    int32_t x_position = 0;

    for (int32_t i = 0; i < glyphs->num_glyphs; i++) {
        PangoGlyphInfo* gi = &glyphs->glyphs[i];

        PangoRectangle ink_extents;
        PangoRectangle logical_extents;
        pango_font_get_glyph_extents(font, gi->glyph, &ink_extents, &logical_extents);

        if (gi->glyph != PANGO_GLYPH_EMPTY && ink_extents.width > 1 && ink_extents.height > 1) {
            double cx = base_x + (double)(x_position + gi->geometry.x_offset) / PANGO_SCALE;
            double cy = base_y + (double)(gi->geometry.y_offset) / PANGO_SCALE;

            uint32_t used_glyph_count = ArenaCountT(GlyphId, &renderer->used_glyphs);
            uint32_t used_glyph_uid = -1;

            if (gi->glyph & PANGO_GLYPH_UNKNOWN_FLAG) continue;

            for (uint32_t i = 0; i < used_glyph_count; ++i) {
                GlyphId* used = ArenaGetT(GlyphId, &renderer->used_glyphs, i);
                if (used->id == gi->glyph && !strcmp(used->face, renderer->cur_face)) {
                    used_glyph_uid = used->uid;
                    goto already_used;
                }
            }

            used_glyph_uid = get_glyph_uid(renderer->cur_face, gi->glyph);
            *ArenaPushT(GlyphId, &renderer->used_glyphs) = (GlyphId){
                .face = renderer->cur_face,
                .uid = used_glyph_uid,
                .id = gi->glyph,
            };

        already_used:

            *ArenaPushT(TypesetGlyph, &renderer->typeset_glyphs) = (TypesetGlyph){
                .source_idx = glyphs->log_clusters[i] + renderer->cur_source_offset,
                .glyph_uid = used_glyph_uid,
                .x0 = (float)(cx + (double)ink_extents.x / (double)PANGO_SCALE),
                .y0 = (float)(cy + (double)ink_extents.y / (double)PANGO_SCALE),
                .x1 = (float)(cx + (double)ink_extents.x / (double)PANGO_SCALE + (double)ink_extents.width / (double)PANGO_SCALE),
                .y1 = (float)(cy + (double)ink_extents.y / (double)PANGO_SCALE + (double)ink_extents.height / (double)PANGO_SCALE),
            };
        }

        x_position += gi->geometry.width;
    }
}

static void shim_renderer_init(ShimRenderer* self) {}
static void shim_renderer_class_init(ShimRendererClass* klass) {
    PangoRendererClass* renderer_class = PANGO_RENDERER_CLASS(klass);
    renderer_class->draw_glyphs = shim_renderer_draw_glyphs;
    renderer_class->prepare_run = shim_renderer_prepare_run;
}

static ShimRenderer* shim_renderer_new(LoadedFonts* loaded_fonts) {
    ShimRenderer* ret = g_object_new(shim_renderer_get_type(), NULL);
    ret->loaded_fonts = loaded_fonts;
    ret->typeset_glyphs = arena_create();
    ret->used_glyphs = arena_create();
    return ret;
}

// -----------------------------------------------------------------------------
// atlas generation

#define ATLAS_GLYPH_BITMAP_SIZE 128

typedef struct {
    uint8_t* bytes;
    int32_t xmin, xmax;  // min inclusive, max exclusive
    int32_t ymin, ymax;
} AtlasGlyphBitmap;

typedef struct {
    float u0, v0;
    float u1, v1;
} AtlasGlyphUv;

typedef struct {
    int32_t x, y;
} AtlasGlyphPosition;

typedef struct {
    uint32_t index;
    int32_t height;
} AtlasGlyphHeight;

static int32_t sort_cmp_atlas_glyph_height(const void* va, const void* vb) {
    const AtlasGlyphHeight *a = (AtlasGlyphHeight*)va, *b = (AtlasGlyphHeight*)vb;
    return b->height - a->height;
}

static uint32_t pack_atlas_glyphs(AtlasGlyphPosition* out_positions, AtlasGlyphBitmap* glyphs, size_t glyph_count) {
    Arena scratch = arena_create();

    AtlasGlyphHeight* order = arena_alloc(&scratch, glyph_count * sizeof(AtlasGlyphHeight));
    AtlasGlyphPosition* sorted_pos = arena_alloc(&scratch, glyph_count * sizeof(AtlasGlyphPosition));

    int32_t max_dim = 0;
    for (int32_t i = 0; i < glyph_count; i++) {
        int32_t width = glyphs[i].xmax - glyphs[i].xmin;
        int32_t height = glyphs[i].ymax - glyphs[i].ymin;
        order[i].index = i;
        order[i].height = height;
        if (width > max_dim) max_dim = width;
        if (height > max_dim) max_dim = height;
    }

    qsort(order, glyph_count, sizeof(AtlasGlyphHeight), sort_cmp_atlas_glyph_height);

    int32_t size = 1;
    while (size < max_dim) size *= 2;

    {
    retry_pack: {}
        int32_t cur_x = 0, cur_y = 0;
        int32_t row_height = 0;
        for (uint32_t i = 0; i < glyph_count; i++) {
            uint32_t idx = order[i].index;
            int32_t width = glyphs[idx].xmax - glyphs[idx].xmin;
            int32_t height = glyphs[idx].ymax - glyphs[idx].ymin;

            if (cur_x + width > size) {
                cur_x = 0;
                cur_y += row_height;
                row_height = 0;
            }
            if (cur_y + height > size) {
                size *= 2;
                goto retry_pack;
            }
            sorted_pos[i].x = cur_x;
            sorted_pos[i].y = cur_y;
            cur_x += width;
            if (height > row_height) {
                row_height = height;
            }
        }
    }

    for (uint32_t i = 0; i < glyph_count; i++) {
        out_positions[order[i].index] = sorted_pos[i];
    }

    arena_destroy(&scratch);

    return size;
}

static AtlasGlyphBitmap* render_glyph_msdf_bitmaps(Arena* arena, ShimRenderer* renderer) {
    static const size_t CMD_BUF_SIZE = 1024;
    static char command[CMD_BUF_SIZE];

    uint32_t used_glyph_count = ArenaCountT(GlyphId, &renderer->used_glyphs);
    GlyphId* used_glyphs = (GlyphId*)renderer->used_glyphs.head;

    AtlasGlyphBitmap* ret = arena_alloc(arena, used_glyph_count * sizeof(AtlasGlyphBitmap));

    for (int32_t i = 0; i < used_glyph_count; ++i) {
        uint32_t glyph = used_glyphs[i].id;

        uint32_t size;
        snprintf(
            command,
            CMD_BUF_SIZE,
            "tool/msdfgen metrics -font %s.ttf g%u -emnormalize",
            used_glyphs[i].face,
            glyph
        );

        char* msdfgen_metrics = read_cmd(arena, command, &size);
        float msdf_x0 = 0.f, msdf_y0 = 0.f, msdf_x1 = 0.f, msdf_y1 = 0.f;
        Assert(4 == sscanf(msdfgen_metrics, "bounds = %f , %f , %f , %f", &msdf_x0, &msdf_y0, &msdf_x1, &msdf_y1));
        int32_t x0 = (int32_t)floorf(64.f * msdf_x0);
        int32_t x1 = (int32_t)ceilf(64.f * msdf_x1);
        int32_t y0 = (int32_t)floorf(64.f * msdf_y0);
        int32_t y1 = (int32_t)ceilf(64.f * msdf_y1);

        snprintf(
            command,
            CMD_BUF_SIZE,
            "tool/msdfgen mtsdf -font %s.ttf g%u -pxrange %u -emnormalize -translate 0.5 0.5 -scale 64 -dimensions %u %u -format bin",
            used_glyphs[i].face,
            glyph,
            MSDFGEN_PX_RANGE,
            ATLAS_GLYPH_BITMAP_SIZE,
            ATLAS_GLYPH_BITMAP_SIZE
        );

        system(command);
        ret[i].bytes = (uint8_t*)read_file(arena, "output.bin", &size);
        remove("output.bin");
        Assert(size == ATLAS_GLYPH_BITMAP_SIZE * ATLAS_GLYPH_BITMAP_SIZE * 4);

        ret[i].xmin = 32 + x0 - GLYPH_PADDING;
        ret[i].xmax = 32 + x1 + GLYPH_PADDING;
        ret[i].ymin = 32 + y0 - GLYPH_PADDING;
        ret[i].ymax = 32 + y1 + GLYPH_PADDING;
    }

    return ret;
}

static AtlasGlyphUv* bake_used_glyphs_to_atlas(Arena* arena, ShimRenderer* renderer) {
    uint32_t used_glyph_count = ArenaCountT(GlyphId, &renderer->used_glyphs);

    AtlasGlyphUv* ret = arena_alloc(arena, used_glyph_count * sizeof(AtlasGlyphUv));
    Arena scratch = arena_create();

    AtlasGlyphBitmap* bitmaps = render_glyph_msdf_bitmaps(&scratch, renderer);

    AtlasGlyphPosition* packed_pos = arena_alloc(&scratch, used_glyph_count * sizeof(AtlasGlyphPosition));
    uint32_t atlas_dim = pack_atlas_glyphs(packed_pos, bitmaps, used_glyph_count);

    uint8_t* atlas = arena_alloc(&scratch, atlas_dim * atlas_dim * 4);

    for (int32_t i = 0; i < used_glyph_count; ++i) {
        AtlasGlyphBitmap bmp = bitmaps[i];

        int32_t basex = packed_pos[i].x;
        int32_t basey = packed_pos[i].y;

        int32_t ow = bmp.xmax - bmp.xmin;
        int32_t oh = bmp.ymax - bmp.ymin;

        int32_t oy = basey;
        for (int32_t y = bmp.ymax - 1; y >= bmp.ymin; y--, oy++) {
            unsigned char* src_pixels = bmp.bytes + (y * ATLAS_GLYPH_BITMAP_SIZE + bmp.xmin) * 4;
            unsigned char* dst_pixels = atlas + (oy * atlas_dim + basex) * 4;
            memcpy(dst_pixels, src_pixels, ow * 4);
        }

        ret[i] = (AtlasGlyphUv){
            .u0 = (float)(basex + GLYPH_PADDING) / (float)atlas_dim,
            .v0 = (float)(basey + GLYPH_PADDING) / (float)atlas_dim,
            .u1 = (float)(basex + GLYPH_PADDING + ow - 4) / (float)atlas_dim,
            .v1 = (float)(basey + GLYPH_PADDING + oh - 4) / (float)atlas_dim,
        };
    }

    unsigned error = lodepng_encode32_file("bin/atlas.png", atlas, atlas_dim, atlas_dim);
    if (error) Panic("Error saving PNG: %s\n", lodepng_error_text(error));

    arena_destroy(&scratch);

    return ret;
}

static int32_t sort_cmp_glyph_id(const void* va, const void* vb) {
    const GlyphId *a = va, *b = vb;

    int32_t face_delta = strcmp(a->face, b->face);
    if (face_delta) return face_delta;

    return a->id < b->id   ? -1
           : a->id > b->id ? 1
                           : 0;
}

static AtlasGlyphUv* bake_used_glyphs_to_atlas_cached(Arena* arena, ShimRenderer* renderer, uint32_t csv_hash) {
    AtlasGlyphUv* ret = NULL;

    uint32_t used_glyph_count = ArenaCountT(GlyphId, &renderer->used_glyphs);
    qsort(renderer->used_glyphs.head, used_glyph_count, sizeof(GlyphId), sort_cmp_glyph_id);
    uint32_t new_hash = hash_djb2(renderer->used_glyphs.head + offsetof(GlyphId, uid), used_glyph_count, sizeof(GlyphId));

    FILE* file = fopen(CACHE_FILE_NAME, "rb+");
    if (file) {
        fseek(file, sizeof(uint32_t), SEEK_SET);  // skip over the csv hash
        uint32_t stored_hash;
        FRead(&stored_hash, sizeof(uint32_t), 1, file);
        if (stored_hash == new_hash) {
            Log("using cached atlas...");
            FRead(&used_glyph_count, sizeof(uint32_t), 1, file);
            ret = arena_alloc(arena, sizeof(AtlasGlyphUv) * used_glyph_count);
            FRead(ret, sizeof(AtlasGlyphUv), used_glyph_count, file);
            fclose(file);
            return ret;
        }
        fclose(file);
    }

    Log("baking atlas...");
    ret = bake_used_glyphs_to_atlas(arena, renderer);

    file = fopen(CACHE_FILE_NAME, "wb+");
    fwrite(&csv_hash, sizeof(uint32_t), 1, file);
    fwrite(&new_hash, sizeof(uint32_t), 1, file);
    fwrite(&used_glyph_count, sizeof(uint32_t), 1, file);
    fwrite(ret, sizeof(AtlasGlyphUv), used_glyph_count, file);
    fclose(file);
    return ret;
}

// -----------------------------------------------------------------------------
// parsing and rendering strings

typedef struct {
    char* value;
    uint32_t value_len;
    uint32_t start_idx;
    uint32_t end_idx;
} UserTag;

typedef struct {
    uint32_t user_tag_count;
    UserTag* user_tags;
    uint32_t typeset_glyph_count;
    TypesetGlyph* typeset_glyphs;
} RenderedPage;

typedef struct {
    uint32_t page_count;
    RenderedPage* pages;
} RenderedString;

static int32_t sort_cmp_glyph_source_idx(const void* va, const void* vb) {
    const TypesetGlyph *a = va, *b = vb;
    return a->source_idx < b->source_idx   ? -1
           : a->source_idx > b->source_idx ? 1
                                           : 0;
}

static RenderedPage render_page(
    Arena* arena,
    PangoContext* pango_context,
    ShimRenderer* renderer,
    PangoAttrList* attr_list,
    char* strings_table_key,
    uint32_t page_number,
    uint32_t width,
    uint32_t height,
    char* contents,
    uint32_t contents_len,
    UserTag* user_tags,
    uint32_t user_tag_count
) {
    char filename_buffer[256];
    arena_clear(&renderer->typeset_glyphs);

    Arena scratch = arena_create();

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(surface);

    PangoLayout* layout = pango_layout_new(pango_context);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_height(layout, height * PANGO_SCALE);
    pango_layout_set_text(layout, contents, -1);
    pango_layout_set_attributes(layout, attr_list);
    pango_renderer_draw_layout((PangoRenderer*)renderer, layout, 0, 0);

    TypesetGlyph* glyphs = (TypesetGlyph*)renderer->typeset_glyphs.head;
    uint32_t glyph_count = ArenaCountT(TypesetGlyph, &renderer->typeset_glyphs);

    // sort glyphs into logical order instead of being always left-to-right
    qsort(glyphs, glyph_count, sizeof(TypesetGlyph), sort_cmp_glyph_source_idx);

    // convert source string indices in user tags to glyph array indices
    {
        uint32_t* index_map = arena_alloc(&scratch, sizeof(uint32_t) * contents_len);
        memset(index_map, 0xFF, sizeof(uint32_t) * contents_len);

        for (uint32_t i = 0; i < glyph_count; ++i) {
            index_map[glyphs[i].source_idx] = i;
        }
        uint32_t prev = 0;
        for (uint32_t i = 0; i < contents_len; ++i) {
            if (index_map[i] == UINT32_MAX) {
                index_map[i] = prev;
            } else {
                prev = index_map[i];
            }
        }
        for (uint32_t i = 0; i < user_tag_count; ++i) {
            user_tags[i].start_idx = index_map[user_tags[i].start_idx];
            user_tags[i].end_idx = index_map[user_tags[i].end_idx];
        }
    }

#if ENABLE_DEBUG_OUTPUT
    if (width > 0) {
        cairo_set_source_rgb(cr, 1, 1, 1);
        pango_cairo_show_layout(cr, layout);

        unsigned char* data = cairo_image_surface_get_data(surface);
        int32_t stride = cairo_image_surface_get_stride(surface);
        int32_t width = cairo_image_surface_get_width(surface);
        int32_t height = cairo_image_surface_get_height(surface);
        unsigned char* png_data = arena_alloc(&scratch, width * height * 4);
        for (int32_t y = 0; y < height; y++) {
            for (int32_t x = 0; x < width; x++) {
                unsigned char* src_pixel = data + y * stride + x * 4;
                unsigned char* dst_pixel = png_data + (y * width + x) * 4;
                dst_pixel[0] = src_pixel[2];
                dst_pixel[1] = src_pixel[1];
                dst_pixel[2] = src_pixel[0];
                dst_pixel[3] = src_pixel[3];
            }
        }

#if ENABLE_DEBUG_GLYPH_BOUNDS
        for (int32_t i = 0; i < ArenaCountT(TypesetGlyph, &renderer->typeset_glyphs); ++i) {
            TypesetGlyph* glyph = ArenaGetT(TypesetGlyph, &renderer->typeset_glyphs, i);

            float fx0 = floorf(glyph->x0);
            float fx1 = floorf(glyph->x1);
            float fy0 = floorf(glyph->y0);
            float fy1 = floorf(glyph->y1);

            int32_t minx = MAX(0.f, fx0);
            int32_t maxx = MIN((float)(width - 1), fx1);
            int32_t miny = MAX(0.f, fy0);
            int32_t maxy = MIN((float)(height - 1), fy1);

            for (int32_t y = miny; y <= maxy; ++y) {
                for (int32_t x = minx; x <= maxx; ++x) {
                    uint8_t* dst_pixel = png_data + (y * width + x) * 4;
                    dst_pixel[0] = MAX(dst_pixel[0], 0x7f);
                    dst_pixel[1] = MAX(dst_pixel[1], 0x7f);
                    dst_pixel[2] = MAX(dst_pixel[2], 0x7f);
                    dst_pixel[3] = MAX(dst_pixel[3], 0x7f);
                }
            }
        }
#endif  // ENABLE_DEBUG_GLYPH_BOUNDS

        snprintf(filename_buffer, 256, "bin/%s.%u.png", strings_table_key, page_number);
        unsigned error = lodepng_encode32_file(filename_buffer, png_data, width, height);
        if (error) Panic("error saving PNG: %s\n", lodepng_error_text(error));
    }
#endif  // ENABLE_DEBUG_OUTPUT

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    RenderedPage ret = {
        .typeset_glyph_count = ArenaCountT(TypesetGlyph, &renderer->typeset_glyphs),
        .typeset_glyphs = arena_alloc(arena, ret.typeset_glyph_count * sizeof(TypesetGlyph)),
        .user_tag_count = user_tag_count,
        .user_tags = arena_alloc(arena, ret.user_tag_count * sizeof(UserTag)),
    };
    memcpy(ret.typeset_glyphs, renderer->typeset_glyphs.head, ret.typeset_glyph_count * sizeof(TypesetGlyph));
    memcpy(ret.user_tags, user_tags, user_tag_count * sizeof(UserTag));

    arena_destroy(&scratch);
    return ret;
}

static void write_style_attr_range(LoadedFonts* loaded_fonts, PangoAttrList* attr_list, TextStyle* style, uint32_t start, uint32_t end) {
    if (end <= start) return;

    PangoAttribute* attr;

    attr = pango_attr_line_height_new(style->lineheight);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attr_list, attr);

    attr = pango_attr_size_new(style->size * PANGO_SCALE);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attr_list, attr);

    attr = pango_attr_font_desc_new(find_font_by_face(loaded_fonts, style->face)->pango_font_desc);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attr_list, attr);
}

static RenderedString render_string_entry(
    Arena* arena,
    PangoContext* pango_context,
    ShimRenderer* renderer,
    InputCsv* input,
    uint32_t language_idx,
    uint32_t string_idx
) {
    RenderedString ret = {0};
    Arena scratch = arena_create();

    char* page_buffer = arena_alloc(&scratch, input->max_string_length);
    ArenaOf(TextStyle*) style_history = arena_create();
    ArenaOf(UserTag) user_tag_stack = arena_create();
    ArenaOf(UserTag) user_tags = arena_create();
    ArenaOf(RenderedPage) pages_acc = arena_create();

    StringsCsvEntry* string = &input->strings[string_idx];
    bool in_style_tag = false;
    char* content_base = string->languages[language_idx];
    char* tag_start = NULL;
    char* page_read = content_base;
    char* page_write = page_buffer;
    TextStyle* cur_style = &input->styles[0].style;
    uint32_t attr_range_start = 0;
    PangoAttrList* attr_list = pango_attr_list_new();

    while (*page_read) {
        if (*page_read == '[' && *(page_read + 1) == '#') {
            // start of tag
            if (!(page_read > content_base && *(page_read - 1) == '[')) {
                // handle [[# as literal [#
                page_read++;

                if (*(page_read + 1) == '-') {
                    page_read++;
                    in_style_tag = true;
                }

                tag_start = page_read + 1;
            }
        } else if (!tag_start) {
            // not inside tag
            *page_write++ = *page_read;
        } else if (*page_read == ']') {
            // end of tag
            uint32_t tag_len = page_read - tag_start;

            if (in_style_tag) {  // was style-changing tag [#- ... ]
                uint32_t attr_range_end = (uint32_t)(page_write - page_buffer);
                write_style_attr_range(renderer->loaded_fonts, attr_list, cur_style, attr_range_start, attr_range_end);
                attr_range_start = attr_range_end;

                if (tag_len == 0) {
                    // empty, pop style history stack
                    if (ArenaCountT(TextStyle*, &style_history) > 0) {
                        cur_style = *ArenaPopT(TextStyle*, &style_history);
                    }
                } else {
                    // set new style by name
                    for (uint32_t i = 0; i < input->styles_count; ++i) {
                        if (!strncmp(input->styles[i].name, tag_start, tag_len)) {
                            *ArenaPushT(TextStyle*, &style_history) = cur_style;
                            cur_style = &input->styles[i].style;
                            break;
                        }
                    }
                }
            } else if (!strncmp(tag_start, ".", tag_len)) {
                // page break
                uint32_t attr_range_end = (uint32_t)(page_write - page_buffer);
                write_style_attr_range(renderer->loaded_fonts, attr_list, cur_style, attr_range_start, attr_range_end);

                *page_write = 0;
                *ArenaPushT(RenderedPage, &pages_acc) = render_page(
                    arena, pango_context, renderer, attr_list, string->key, ret.page_count++, string->width, string->height, page_buffer, page_write - page_buffer,
                    (UserTag*)user_tags.head, ArenaCountT(UserTag, &user_tags)
                );
                page_write = page_buffer;
                pango_attr_list_unref(attr_list);
                attr_list = pango_attr_list_new();
                attr_range_start = 0;
                arena_clear(&user_tags);
            } else if (!strncmp(tag_start, "/", tag_len)) {
                // end user tag
                if (ArenaCountT(UserTag, &user_tag_stack) > 0) {
                    UserTag* tag = ArenaPopT(UserTag, &user_tag_stack);
                    tag->end_idx = page_write - page_buffer;
                    *ArenaPushT(UserTag, &user_tags) = *tag;
                }
            } else {
                // start user tag
                *ArenaPushT(UserTag, &user_tag_stack) = (UserTag){
                    .start_idx = page_write - page_buffer,
                    .value = tag_start,
                    .value_len = tag_len,
                };
            }

            tag_start = NULL;
            in_style_tag = false;
        }
        page_read++;
    }

    uint32_t attr_range_end = (uint32_t)(page_write - page_buffer);
    write_style_attr_range(renderer->loaded_fonts, attr_list, cur_style, attr_range_start, attr_range_end);

    *page_write = 0;
    *ArenaPushT(RenderedPage, &pages_acc) = render_page(
        arena, pango_context, renderer, attr_list, string->key, ret.page_count++, string->width, string->height, page_buffer, page_write - page_buffer,
        (UserTag*)user_tags.head, ArenaCountT(UserTag, &user_tags)
    );
    pango_attr_list_unref(attr_list);

    ret.pages = arena_alloc(arena, ret.page_count * sizeof(RenderedPage));
    memcpy(ret.pages, pages_acc.head, ret.page_count * sizeof(RenderedPage));

    arena_destroy(&scratch);
    arena_destroy(&style_history);
    arena_destroy(&user_tag_stack);
    arena_destroy(&user_tags);
    arena_destroy(&pages_acc);

    return ret;
}

// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: textc [language]\n");
        return 1;
    }

    Arena base_arena = arena_create();

    InputCsv input = parse_input_files(&base_arena);
    if (input.cached_hash_matched) {
        return 0;
    }

    uint32_t lang_idx = 0;
    for (; lang_idx < input.language_count; ++lang_idx) {
        if (!strcmp(input.languages[lang_idx], argv[1])) break;
    }
    if (lang_idx == input.language_count) {
        fprintf(stderr, "language key not present strings.csv: '%s'", argv[1]);
        return 1;
    }

    PangoContext* context = pango_font_map_create_context(pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT));
    LoadedFonts loaded_fonts = load_fonts(&base_arena);
    ShimRenderer* renderer = shim_renderer_new(&loaded_fonts);
    ArenaOf(RenderedString) results = arena_create();

    Log("shaping text...");
    for (int32_t i = 0; i < input.strings_count; ++i) {
        RenderedString rendered = render_string_entry(&base_arena, context, renderer, &input, lang_idx, i);
        if (input.strings[i].width > 0) {
            *ArenaPushT(RenderedString, &results) = rendered;
        }
    }

    AtlasGlyphUv* glyph_uvs = bake_used_glyphs_to_atlas_cached(&base_arena, renderer, input.hash);

    FILE* file = fopen("bin/strings.txtc", "wb+");

    FWriteValue(uint32_t, 0x00545854, file);  // filetype bytes: TXTv (high byte is version)

    fwrite(&input.strings_count, sizeof(uint32_t), 1, file);
    for (uint32_t i = 0; i < ArenaCountT(RenderedString, &results); ++i) {
        RenderedString* str = ArenaGetT(RenderedString, &results, i);

        file_write_padded_string(file, input.strings[i].key, strnlen(input.strings[i].key, 255));
        fwrite(&input.strings[i].width, sizeof(uint32_t), 1, file);
        fwrite(&input.strings[i].height, sizeof(uint32_t), 1, file);

        fwrite(&str->page_count, sizeof(uint32_t), 1, file);
        for (uint32_t j = 0; j < str->page_count; ++j) {
            RenderedPage* page = &str->pages[j];

            fwrite(&page->user_tag_count, sizeof(uint32_t), 1, file);
            for (uint32_t k = 0; k < page->user_tag_count; ++k) {
                UserTag* tag = &page->user_tags[k];

                file_write_padded_string(file, tag->value, tag->value_len);
                fwrite(&tag->start_idx, sizeof(uint32_t), 1, file);
                fwrite(&tag->end_idx, sizeof(uint32_t), 1, file);
            }

            uint32_t vertex_count = 4 * page->typeset_glyph_count;
            fwrite(&vertex_count, sizeof(uint32_t), 1, file);
            for (uint32_t k = 0; k < page->typeset_glyph_count; ++k) {
                TypesetGlyph* glyph = &page->typeset_glyphs[k];

                uint32_t idx = UINT32_MAX;
                for (uint32_t j = 0; j < ArenaCountT(GlyphId, &renderer->used_glyphs); ++j) {
                    GlyphId* gid = ArenaGetT(GlyphId, &renderer->used_glyphs, j);
                    if (glyph->glyph_uid == gid->uid) {
                        idx = j;
                        break;
                    }
                }
                Assert(idx < UINT32_MAX);

#define X(a, b)                                           \
    fwrite(&glyph->x##a, sizeof(float), 1, file);         \
    fwrite(&glyph->y##b, sizeof(float), 1, file);         \
    fwrite(&glyph_uvs[idx].u##a, sizeof(float), 1, file); \
    fwrite(&glyph_uvs[idx].v##b, sizeof(float), 1, file);

                X(0, 0);
                X(0, 1);
                X(1, 1);
                X(1, 0);

#undef X
            }
        }
    }

    fclose(file);
    Log("done");
    return 0;
}