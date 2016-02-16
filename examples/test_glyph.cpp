extern "C" {
#include "test.h"
}

#include <fontconfig/fontconfig.h>
#include <dlfcn.h>
#include <ft2build.h>
#include <string>
#include <unistd.h>
#include <stdio.h>
#include <memory>

#include FT_ADVANCES_H
#include FT_BITMAP_H
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_SIZES_H
#include FT_SYSTEM_H
static FT_Library g_ftlibrary;
static FT_Face g_face; 

static const int hanzi_start = 0x4E00;
static const int hanzi_end = 0x9FA5;
static int curr_zi = hanzi_start;

template <typename R, typename T, R (*P)(T*)> struct SkFunctionWrapper {
    R operator()(T* t) { return P(t); }
};

template <typename T, void (*P)(T*)> class SkAutoTCallVProc
    : public std::unique_ptr<T, SkFunctionWrapper<void, T, P>> {
public:
    SkAutoTCallVProc(T* obj): std::unique_ptr<T, SkFunctionWrapper<void, T, P>>(obj) {}

    operator T*() const { return this->get(); }
    T* detach() { return this->release(); }
};

template<typename T, void (*D)(T*)> void FcTDestroy(T* t) {
    D(t);
}

template <typename T, T* (*C)(), void (*D)(T*)> class SkAutoFc
    : public SkAutoTCallVProc<T, FcTDestroy<T, D> > {
public:
    SkAutoFc() : SkAutoTCallVProc<T, FcTDestroy<T, D> >(C()) {
        T* obj = this->operator T*();
    }
    explicit SkAutoFc(T* obj) : SkAutoTCallVProc<T, FcTDestroy<T, D> >(obj) {}
};

typedef SkAutoFc<FcCharSet, FcCharSetCreate, FcCharSetDestroy> SkAutoFcCharSet;
typedef SkAutoFc<FcConfig, FcConfigCreate, FcConfigDestroy> SkAutoFcConfig;
typedef SkAutoFc<FcFontSet, FcFontSetCreate, FcFontSetDestroy> SkAutoFcFontSet;
typedef SkAutoFc<FcLangSet, FcLangSetCreate, FcLangSetDestroy> SkAutoFcLangSet;
typedef SkAutoFc<FcObjectSet, FcObjectSetCreate, FcObjectSetDestroy> SkAutoFcObjectSet;
typedef SkAutoFc<FcPattern, FcPatternCreate, FcPatternDestroy> SkAutoFcPattern;

static const char* get_string(FcPattern* pattern, const char object[], const char* missing = "") {
    FcChar8* value;
    if (FcPatternGetString(pattern, object, 0, &value) != FcResultMatch) {
        return missing;
    }
    return (const char*)value;
}

enum SkFILE_Flags {
    kRead_SkFILE_Flag   = 0x01,
    kWrite_SkFILE_Flag  = 0x02
};

static bool sk_exists(const char *path, SkFILE_Flags flags) {
    int mode = F_OK;
    if (flags & kRead_SkFILE_Flag) {
        mode |= R_OK;
    }
    if (flags & kWrite_SkFILE_Flag) {
        mode |= W_OK;
    }
    return (0 == access(path, mode));
}

static bool FontAccessible(FcPattern* font, const char** path) {
    // FontConfig can return fonts which are unreadable.
    const char* filename = get_string(font, FC_FILE, nullptr);
    if (nullptr == filename) {
        return false;
    }
    bool exists = sk_exists(filename, kRead_SkFILE_Flag);
    if (exists) {
        *path = filename;
    }
    return exists;
}

class init_exception : public std::exception
{
public:
    init_exception(const char* what) :
        what_(what) {}
    virtual const char* what() const noexcept override { return what_; }
        
private:
    const char* what_;
};


static std::string lookup_font_path()
{
    SkAutoFcPattern pattern;
    SkAutoFcConfig fFC(FcInitLoadConfigAndFonts());
    int weight = FC_WEIGHT_REGULAR;
    int width = FC_WIDTH_NORMAL;
    int slant = FC_SLANT_ROMAN;
    const char* path;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8*)"sans-serif");
    FcPatternAddInteger(pattern, FC_WEIGHT, weight);
    FcPatternAddInteger(pattern, FC_WIDTH, width);
    FcPatternAddInteger(pattern, FC_SLANT, slant);


    FcConfigSubstitute(fFC, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    SkAutoFcPattern font(FcFontMatch(fFC, pattern, &result));
    if (nullptr == font || !FontAccessible(font, &path)) {
        throw init_exception("no font is found.");
    }
    return path;
}

static void init()
{
    std::string font_path(lookup_font_path());
    printf("font path: %s.\n", font_path.c_str());
    FT_Error err = FT_Init_FreeType(&g_ftlibrary);
    if (err) {
        printf("fails to init ft2: %d.\n", err);
        throw init_exception("ft2");
    }
    err = FT_New_Face(g_ftlibrary, font_path.c_str(), 0, &g_face);
    if (err) {
        printf("fails to load face: %d.\n", err);
        throw init_exception("load face");
    }
}

static int get_next_hanzi()
{
    int now = curr_zi++;
    if (curr_zi == hanzi_end) {
        curr_zi = hanzi_start;
    }
    return now;
}

typedef float SkScalar;
#define SkFDot6ToScalar(x)  ((SkScalar)(x) * 0.015625f)

static int move_proc(const FT_Vector* pt, void* ctx) {
    VGPath path = (VGPath)ctx;
    VGubyte command = VG_MOVE_TO_ABS;
    SkScalar dataArray[2] = { SkFDot6ToScalar(pt->x), -SkFDot6ToScalar(pt->y) };
    vgAppendPathData(path, 1,
                     &command, dataArray);
    return 0;
}

static int line_proc(const FT_Vector* pt, void* ctx) {
    VGPath path = (VGPath)ctx;
    VGubyte command = VG_LINE_TO_ABS;
    SkScalar dataArray[2] = { SkFDot6ToScalar(pt->x), -SkFDot6ToScalar(pt->y) };
    vgAppendPathData(path, 1,
                     &command, dataArray);
    return 0;
}

static int quad_proc(const FT_Vector* pt0, const FT_Vector* pt1,
                     void* ctx) {
    VGPath path = (VGPath)ctx;
    VGubyte command = VG_QUAD_TO_ABS;
    SkScalar dataArray[4] = { SkFDot6ToScalar(pt0->x), -SkFDot6ToScalar(pt0->y),
                 SkFDot6ToScalar(pt1->x), -SkFDot6ToScalar(pt1->y) };
    vgAppendPathData(path, 1,
                     &command, dataArray);
    return 0;
}

static int cubic_proc(const FT_Vector* pt0, const FT_Vector* pt1,
                      const FT_Vector* pt2, void* ctx) {
    VGPath path = (VGPath)ctx;
    VGubyte command = VG_CUBIC_TO_ABS;
    SkScalar dataArray[6] = { SkFDot6ToScalar(pt0->x), -SkFDot6ToScalar(pt0->y),
                  SkFDot6ToScalar(pt1->x), -SkFDot6ToScalar(pt1->y),
                  SkFDot6ToScalar(pt2->x), -SkFDot6ToScalar(pt2->y) };
    vgAppendPathData(path, 1,
                     &command, dataArray);
    return 0;
}

static void buildPath(VGPath path)
{
    FT_Outline_Funcs    funcs;

    funcs.move_to   = move_proc;
    funcs.line_to   = line_proc;
    funcs.conic_to  = quad_proc;
    funcs.cubic_to  = cubic_proc;
    funcs.shift     = 0;
    funcs.delta     = 0;
    const FT_UInt glyph_id = FT_Get_Char_Index(g_face, get_next_hanzi());
    if (!glyph_id) {
        throw init_exception("get glyph_id.");
    }

    if (FT_Load_Glyph(g_face, glyph_id, FT_LOAD_NO_SCALE) != 0) {
        throw init_exception("load glyph_id.");
    }

    FT_Error err = FT_Outline_Decompose(&g_face->glyph->outline, &funcs, path);
    if (err) {
        throw init_exception("decompose outline.");
    }
}

static void display(float)
{
  int x,y,p;
  VGfloat white[] = {1,1,1,1};
  
  vgSetfv(VG_CLEAR_COLOR, 4, white);
  vgClear(0, 0, testWidth(), testHeight());
  
  vgSeti(VG_MATRIX_MODE, VG_MATRIX_PATH_USER_TO_SURFACE);
  
  vgLoadIdentity();
  vgTranslate(0,100);
  vgScale(100,-100);
  VGPath path = testCreatePath();
  buildPath(path);
  vgDrawPath(path, VG_FILL_PATH);
  vgDestroyPath(path);
  static bool bFirst = true;
  if (bFirst) {
      bFirst = false;
      typedef int (*pfnglXSwapIntervalSGI) (int);
      pfnglXSwapIntervalSGI swapInterval = (pfnglXSwapIntervalSGI)dlsym(RTLD_DEFAULT, "glXSwapIntervalSGI");
      swapInterval(0);
  }
}

int main(int argc, char **argv)
{
  testInit(argc, argv, 500,500, "ShivaVG: Font Test");
  testCallback(TEST_CALLBACK_DISPLAY, (CallbackFunc)display);
  init(); 
  testRun();
  
  return EXIT_SUCCESS;
}
