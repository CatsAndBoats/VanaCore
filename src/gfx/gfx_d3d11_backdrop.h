/* gfx_d3d11_backdrop.h: gfx_present_backdrops on Direct3D 11. Not drawn yet: 0 says so, and VanaCore's windows stay plain
 * translucent ones there (Direct3D 12 blurs the frame behind them: gfx_d3d12_backdrop.h). */

int gfx_present_backdrops(const float* rects, int n)
{
    (void)rects;
    (void)n;
    return 0;
}

/* (never given one: no backdrops, so VanaCore keeps its interface in the one layer) */
void gfx_present_layer_below(GfxTex* layer) { (void)layer; }
