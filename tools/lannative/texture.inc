// texture.inc - runtime textures for the script extender (API 2.5, included by secore.inc). Lets a mod use a PNG next to its mod.json as an item icon without cooking.
//   create_texture(bgra, w, h, path): UTexture2D::CreateTransient(w, h, PF_B8G8R8A8) -> copy the pixels into mip 0 -> UpdateResource -> moved into /Game/Mods/<Mod>/<Name> and rooted,
//   so a soft path ("m_InventoryIcon") resolves to it like a cooked asset. texture_from_file(file, path): decodes PNG / JPEG / BMP / GIF / TIFF with Windows Imaging Component (part of
//   Windows, no third-party decoder) and calls create_texture. FTexturePlatformData is not a UPROPERTY: its pointer is found by scanning the new texture for a structure that holds the
//   size just asked for (self-checking; refused when not found).
#define COBJMACROS
#include <wincodec.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#define SE_PF_B8G8R8A8 2

static uint8_t *se_tex_platform_data(void *tex, int w, int h) {          // FTexturePlatformData { int SizeX, SizeY, NumSlices; uint8 PixelFormat; TIndirectArray<FTexture2DMipMap> Mips (+0x10) }
    for (int off = 0x80; off < 0x200; off += 8) {
        uint8_t *p = *(uint8_t **)((uint8_t *)tex + off); if (!se_valid_ptr(p)) continue;
        if (*(int *)p != w || *(int *)(p + 4) != h || p[12] != SE_PF_B8G8R8A8 || *(int *)(p + 0x18) != 1) continue;
        uint8_t *mip = **(uint8_t ***)(p + 0x10); if (se_valid_ptr(mip) && *(int *)mip == w && *(int *)(mip + 4) == h) return p;
    }
    return NULL;
}
static void *api_p_create_texture(const AbsApi *self, const void *bgra, int w, int h, const char *path) {
    const char *who = plug_name_of(plug_of(self)); void *tex = NULL;
    if (!bgra || w < 1 || h < 1 || w > 4096 || h > 4096 || !path || strncmp(path, "/Game/Mods/", 11) || strlen(path) > 220) { logf_("plugin %s: create_texture refused: %dx%d, path '%s' (must be under /Game/Mods/, at most 4096x4096)", who, w, h, path ? path : "(null)"); return NULL; }
    if (!api_p_is_game_thread(self)) { logf_("plugin %s: create_texture refused - not on the game thread", who); return NULL; }
    char pkg[240], nm[120]; const char *dot = strchr(path, '.');
    if (!dot) { logf_("plugin %s: create_texture refused: '%s' needs /Game/Mods/<Mod>/<Name>.<Name>", who, path); return NULL; }
    memcpy(pkg, path, (size_t)(dot - path)); pkg[dot - path] = 0; snprintf(nm, sizeof nm, "%s", dot + 1);
    void *ex = api_p_find_object(self, path); if (ex) { logf_("plugin %s: create_texture %s already exists (%p), reused", who, path, ex); return ex; }
    __try {
        uint32_t rct = se_rva_sym("UTexture2D::CreateTransient", 0x1789D60u), rlock = se_rva_sym("FUntypedBulkData::Lock", 0x7ACD10u), runl = se_rva_sym("FUntypedBulkData::Unlock", 0x857400u);
        uint32_t rupd = se_rva_sym("UTexture2D::UpdateResource", 0x1795590u), rcp = se_rva_sym("CreatePackage", 0x8BFAC0u), rren = se_rva_sym("UObject::Rename", 0x898190u);
        tex = ((void *(*)(int, int, int))(g_base + rct))(w, h, SE_PF_B8G8R8A8);
        if (!tex) { logf_("plugin %s: create_texture %s: CreateTransient returned NULL", who, path); return NULL; }
        uint8_t *pd = se_tex_platform_data(tex, w, h);
        if (!pd) { logf_("plugin %s: create_texture %s: platform data not found on %p - refused", who, path, tex); return NULL; }
        uint8_t *bulk = **(uint8_t ***)(pd + 0x10) + 8;                                      // FTexture2DMipMap { int SizeX, SizeY; FByteBulkData BulkData (+8) }
        void *dst = ((void *(*)(void *, uint32_t))(g_base + rlock))(bulk, 2);                // LOCK_READ_WRITE; CreateTransient already sized it to w*h*4
        if (!dst) { logf_("plugin %s: create_texture %s: bulk data lock failed", who, path); return NULL; }
        memcpy(dst, bgra, (size_t)w * h * 4);
        ((void (*)(void *))(g_base + runl))(bulk);
        ((void (*)(void *))(g_base + rupd))(tex);
        wchar_t wpkg[240], wnm[120]; u8_to_w(pkg, wpkg, 240); u8_to_w(nm, wnm, 120);
        void *pk = ((void *(*)(void *, const wchar_t *))(g_base + rcp))(NULL, wpkg);
        int ren = pk && ((unsigned char (*)(void *, const wchar_t *, void *, unsigned))(g_base + rren))(tex, wnm, pk, 1 | 4 | 16 | 32) != 0;
        if (pk) se_root_flag(pk, 1); se_root_flag(tex, 1);
        logf_("plugin %s: create_texture %s: %dx%d texture %p (platform data +%p), package %p, rename %d, find_object -> %p (rooted)", who, path, w, h, tex, (void *)pd, pk, ren, api_p_find_object(self, path));
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("plugin %s: create_texture %s: EXCEPTION 0x%08lX", who, path, GetExceptionCode()); tex = NULL; }
    return tex;
}
// decode an image file to 32-bit BGRA (straight alpha) with WIC; returns malloc'd pixels or NULL with a reason
static uint8_t *se_decode_image(const wchar_t *file, int *w, int *h, char *why, int cap) {
    HRESULT hc = CoInitializeEx(NULL, COINIT_MULTITHREADED); int uninit = SUCCEEDED(hc);       // RPC_E_CHANGED_MODE: COM already up in another mode, fine
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *d = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL; uint8_t *px = NULL; UINT uw = 0, uh = 0; HRESULT hr;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (FAILED(hr)) { snprintf(why, cap, "WIC factory 0x%08lX", (unsigned long)hr); goto done; }
    hr = IWICImagingFactory_CreateDecoderFromFilename(f, file, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d); if (FAILED(hr)) { snprintf(why, cap, "cannot open / decode the file (0x%08lX)", (unsigned long)hr); goto done; }
    hr = IWICBitmapDecoder_GetFrame(d, 0, &fr); if (FAILED(hr)) { snprintf(why, cap, "no frame (0x%08lX)", (unsigned long)hr); goto done; }
    hr = IWICImagingFactory_CreateFormatConverter(f, &cv); if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { snprintf(why, cap, "format conversion (0x%08lX)", (unsigned long)hr); goto done; }
    IWICFormatConverter_GetSize(cv, &uw, &uh);
    if (uw < 1 || uh < 1 || uw > 4096 || uh > 4096) { snprintf(why, cap, "size %ux%u (1..4096)", uw, uh); goto done; }
    px = malloc((size_t)uw * uh * 4); if (!px) { snprintf(why, cap, "out of memory"); goto done; }
    hr = IWICFormatConverter_CopyPixels(cv, NULL, uw * 4, uw * uh * 4, px); if (FAILED(hr)) { snprintf(why, cap, "CopyPixels 0x%08lX", (unsigned long)hr); free(px); px = NULL; goto done; }
    *w = (int)uw; *h = (int)uh;
done:
    if (cv) IWICFormatConverter_Release(cv); if (fr) IWICBitmapFrameDecode_Release(fr); if (d) IWICBitmapDecoder_Release(d); if (f) IWICImagingFactory_Release(f);
    if (uninit) CoUninitialize();
    return px;
}
// texture_from_file(file, path): file = an absolute path (a content mod's icon_file is resolved against its mod folder by the content handler before this is called)
static void *api_p_texture_from_file(const AbsApi *self, const char *file, const char *path) {
    const char *who = plug_name_of(plug_of(self)); if (!file || !path) return NULL;
    wchar_t wf[MAX_PATH]; u8_to_w(file, wf, MAX_PATH); int w = 0, h = 0; char why[200] = "";
    uint8_t *px = se_decode_image(wf, &w, &h, why, sizeof why);
    if (!px) { logf_("plugin %s: texture_from_file %s: %s", who, file, why); return NULL; }
    logf_("plugin %s: texture_from_file %s: decoded %dx%d", who, file, w, h);
    void *t = api_p_create_texture(self, px, w, h, path); free(px); return t;
}
