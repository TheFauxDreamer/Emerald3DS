// The GPU compositor: a second way to draw the top screen.
//
// The PICA200 draws the GBA layers from a tile atlas. The CPU only decodes the
// tiles that changed and sends quads. rp2350/ppu.c stays the reference: a frame
// goes to the GPU only when GpuComposeCheck() finds nothing in it that this
// file cannot draw as ppu.c does. See the top of gpu_compose.c.
//
// The design comes from the compositor of pokeemerald-3Ds-dualscreen by
// ZallaxDev (MIT), release v0.1.1: the tile atlas with palette versions, and
// text BGs kept in textures that change cell by cell.

#ifndef CTR_GPU_COMPOSE_H
#define CTR_GPU_COMPOSE_H

#include <3ds.h>
#include <citro2d.h>
#include <stdint.h>

// Why a frame goes to ppu.c. GpuComposeCheck() and GpuComposeDraw() return a
// mask of these. 0 means that the GPU draws the frame.
enum {
    GPU_WHY_BITMAP    = 1u << 0,   // DISPCNT mode 3 to 7
    GPU_WHY_OBJWIN    = 1u << 1,   // the OBJ window is on
    GPU_WHY_WINDOW    = 1u << 2,   // a window region hides the backdrop
    GPU_WHY_BLENDBACK = 1u << 3,   // alpha blend with the backdrop as source
    GPU_WHY_SINGULAR  = 1u << 4,   // an affine matrix with no inverse
    GPU_WHY_FADE      = 1u << 5,   // most BG colours changed this frame
    GPU_WHY_BUDGET    = 1u << 6,   // too many quads or tiles for one frame
    GPU_WHY_VRAM      = 1u << 7,   // no VRAM for a texture
    GPU_WHY_COUNT     = 8
};

// A short name for each bit, for the log.
extern const char *const kGpuWhyName[GPU_WHY_COUNT];

// The four GBA regions and the wide field of one frame.
typedef struct {
    const uint8_t *reg, *pal, *vram, *oam;
    int margin;                     // 0, or CTR_WIDE_MARGIN on a wide frame
    const uint16_t *side[4];        // the side map of each BG, or NULL
    unsigned sideDelta;
} GpuComposeInput;

// After C2D_Init. Returns 0 if there is no VRAM for the surface. The GPU path
// is then off for the session.
int  GpuComposeInit(void);
void GpuComposeExit(void);

// Outside a frame, before C3D_FrameBegin. Deletes the textures that a size
// change released. citro3d cannot delete a render target inside a frame.
void GpuComposeMaintain(void);

// The reasons that the registers, palette and OAM give. Call it once for each
// presented frame, because it also keeps the palette of the last frame.
unsigned GpuComposeCheck(const GpuComposeInput *in);

// Inside a frame, after C3D_FrameBegin. Draws the frame into the surface.
// Returns 0, or the reasons that stopped it. After a failure the surface holds
// nothing useful, and the caller must draw the frame with ppu.c.
unsigned GpuComposeDraw(const GpuComposeInput *in);

// The surface: 512x256 RGBA8. GBA x = 0 is column CTR_WIDE_MARGIN and GBA
// y = 0 is row 0, the same layout as the ppu.c stage.
C3D_Tex *GpuComposeTexture(void);

// The quads and tile decodes of the last GpuComposeDraw(), for the log.
void GpuComposeStats(unsigned *quads, unsigned *decodes);

// Outside a frame. Waits for the GPU, then copies rows 0 to 159 of the surface
// to 'dst', 'stride' pixels for each row, from surface column 0. RGB565 if
// rgba8 is 0, else RGBA8 as the GPU stores it. 'bounce' is linear memory of
// 512x256 pixels in that format.
void GpuComposeReadback(void *dst, int stride, int rgba8, void *bounce);

#endif // CTR_GPU_COMPOSE_H
