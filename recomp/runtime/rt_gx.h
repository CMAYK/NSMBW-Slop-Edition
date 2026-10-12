/* rt_gx.h - GX state shared between the command processor and the renderer */
#pragma once
#include <stdint.h>

typedef struct {
    /* CP */
    uint32_t cp_matidx_a, cp_matidx_b, cp_vcd_lo, cp_vcd_hi;
    uint32_t cp_vat_a[8], cp_vat_b[8], cp_vat_c[8];
    uint32_t cp_array_base[16], cp_array_stride[16];
    /* XF: 0x000-0x7FF matrix/light memory, 0x1000+ registers */
    uint32_t xf_mem[0x800];
    uint32_t xf_regs[0x100];
    /* BP */
    uint32_t bp[256];
    uint32_t bp_mask;
} gx_state;

extern gx_state gx;
int gx_vertex_size(int vat);

/* renderer (rt_gxrender.c) */
void gx_render_init(void);
void gx_render_draw(int prim, int vat, uint32_t count, const uint8_t *data);
void gx_render_efb_copy(uint32_t bp52);
void gx_render_flush(void);
void gx_render_tlut_load(int reg, uint32_t val);
void gx_render_tex_invalidate(void);
void gx_render_tev_reg(int reg, uint32_t v);
