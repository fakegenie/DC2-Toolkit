#include "outline.hpp"
#include "draw.hpp"
#include "mg_frame.hpp"
#include "mglib.hpp"
#include <cmath>

PORT_OVERRIDE int COutLineDraw::Draw(float scale, float alpha) {
    if (frame == NULL || texture == NULL || enable == 0) return 0;
    if (!(scale <= 1.0f)) scale = 1.0f;
    if (width <= 0.0f) {
        frame->SetAttrParamObjAlpha(alpha, 1);
        return mgDrawDirect(frame);
    }
    float scaled_width = width * scale * scale;
    float opacity = (int)scaled_width <= 0 ? std::fmax(scaled_width, 0.01f) : 1.0f;
    int edge_offset = (int)(16.0f * scaled_width);
    mgVu0FBOX draw_box;
    if (mgGetDrawRect(frame, &draw_box) == 0) return 0;
    int edged = hide_edge == 0 && !(alpha < 1.0f) && edge_offset > 0 && !(opacity < 0.1f);
    u_int edge_tint = (u_int)color[0] | (u_int)color[1] << 8 | (u_int)color[2] << 16 | (u_int)(128.0f * opacity * alpha) << 24;
    u_int body_tint = 0x808080u | (u_int)(128.0f * alpha) << 24;
    visual_outline_begin();
    int result = mgDrawDirect(frame);
    visual_outline_end(edged ? edge_offset : 0, edge_tint, body_tint);
    return result;
}
