#include <cfloat>
#include <cmath>
#include "mg_frame.hpp"
#include "mg_math.hpp"
#include "mglib.hpp"

extern "C" int port_screen_width(void);

static float clip_corners[8][4];

static void corner_bounds(float corners[8][4], float *high, float *low) {
    for (int lane = 0; lane < 4; lane++) {
        high[lane] = corners[0][lane];
        low[lane] = corners[0][lane];
        for (int vertex = 1; vertex < 8; vertex++) {
            high[lane] = std::fmax(high[lane], corners[vertex][lane]);
            low[lane] = std::fmin(low[lane], corners[vertex][lane]);
        }
    }
}

void test1(float (*corners)[4], float (*screen)[4], float (*matrix)[4], float *out_max, float *out_min) {
    sceVu0FMATRIX transform;
    sceVu0MulMatrix(transform, screen, matrix);
    for (int vertex = 0; vertex < 8; vertex++) {
        sceVu0ApplyMatrix(clip_corners[vertex], transform, corners[vertex]);
    }
    corner_bounds(clip_corners, out_max, out_min);
}

void test2(float *out_max, float *out_min) {
    for (int vertex = 0; vertex < 8; vertex++) {
        float depth = std::fabs(clip_corners[vertex][3]);
        float reciprocal = depth == 0.0f ? FLT_MAX : 1.0f / depth;
        clip_corners[vertex][0] *= reciprocal;
        clip_corners[vertex][1] *= reciprocal;
    }
    corner_bounds(clip_corners, out_max, out_min);
}

int mgInsideScreen(float (*corners)[4], float (*matrix)[4], float *out_max, float *out_min) {
    test1(corners, mgRenderInfo.world_screen_rel, matrix, out_max, out_min);
    test2(out_max, out_min);
    return mgClipBoxW(out_max, out_min, mgRenderInfo.screen_box_max, mgRenderInfo.screen_box_min);
}

void mgCFrame::GetLocalMatrix(float (*matrix)[4]) {
    sceVu0CopyMatrix(matrix, trans_matrix);
    if (!use_srt) {
        return;
    }
    for (int column = 0; column < 3; column++) {
        for (int lane = 0; lane < 4; lane++) {
            matrix[column][lane] *= scale[lane];
        }
    }
    sceVu0FVECTOR origin;
    sceVu0CopyVector(origin, matrix[3]);
    if (rot_type & MG_FRAME_ROT_LOCAL_ORIGIN) {
        for (int lane = 0; lane < 3; lane++) {
            matrix[3][lane] = 0.0f;
        }
        matrix[3][3] = 1.0f;
    }
    if (rot_type & MG_FRAME_ROT_APPLY) {
        if (rotation[0] != 0.0f) {
            sceVu0RotMatrixX(matrix, matrix, rotation[0]);
        }
        if (rotation[1] != 0.0f) {
            sceVu0RotMatrixY(matrix, matrix, rotation[1]);
        }
        if (rotation[2] != 0.0f) {
            sceVu0RotMatrixZ(matrix, matrix, rotation[2]);
        }
    }
    for (int lane = 0; lane < 3; lane++) {
        matrix[3][lane] = position[lane] + ((rot_type & MG_FRAME_ROT_LOCAL_ORIGIN) ? origin[lane] : matrix[3][lane]);
    }
    matrix[3][3] = 1.0f;
}

void mgCFrame::GetLWMatrix(float (*matrix)[4]) {
    bool update = reference || changed;
    for (mgCFrame *ancestor = parent; ancestor && !update; ancestor = ancestor->parent) {
        update = ancestor->changed != 0;
    }
    if (update) {
        sceVu0FMATRIX local;
        GetLocalMatrix(local);
        ClearChildFlag();
        if (parent) {
            sceVu0FMATRIX world;
            parent->GetLWMatrix(world);
            sceVu0MulMatrix(lw_matrix, world, local);
        } else {
            sceVu0CopyMatrix(lw_matrix, local);
        }
        changed = 0;
    }
    sceVu0CopyMatrix(matrix, lw_matrix);
}

void mgCFrame::GetLWMatrixTopBottom(float (*matrix)[4]) {
    if (reference) {
        GetLWMatrix(matrix);
        return;
    }
    if (changed || (parent && parent->changed)) {
        sceVu0FMATRIX local;
        GetLocalMatrix(local);
        ClearChildFlag();
        if (parent) {
            sceVu0MulMatrix(lw_matrix, parent->lw_matrix, local);
        } else {
            sceVu0CopyMatrix(lw_matrix, local);
        }
        changed = 0;
    }
    sceVu0CopyMatrix(matrix, lw_matrix);
}

void mgCFrame::GetBBoardMatrix(int mode, float (*matrix)[4], mgRENDER_INFO *info) {
    sceVu0FMATRIX world;
    sceVu0FMATRIX result;
    GetLWMatrix(world);
    sceVu0UnitMatrix(result);
    float scale = std::sqrt(world[0][0] * world[0][0] + world[0][1] * world[0][1] + world[0][2] * world[0][2]);
    if (mode & (MG_FRAME_BILLBOARD_FULL | MG_FRAME_BILLBOARD_Y)) {
        sceVu0FVECTOR direction = {info->camera_pos[0] - world[3][0], 0.0f, info->camera_pos[2] - world[3][2], 0.0f};
        sceVu0Normalize(result[2], direction);
        result[0][0] = result[2][2];
        result[0][2] = -result[2][0];
        if (mode & MG_FRAME_BILLBOARD_FULL) {
            sceVu0FMATRIX pitch;
            sceVu0UnitMatrix(pitch);
            direction[1] = info->camera_pos[1] - world[3][1];
            sceVu0Normalize(direction, direction);
            float horizontal = std::sqrt(direction[0] * direction[0] + direction[2] * direction[2]);
            pitch[1][1] = horizontal;
            pitch[2][2] = horizontal;
            pitch[1][2] = -direction[1];
            pitch[2][1] = direction[1];
            sceVu0MulMatrix(result, result, pitch);
        }
    }
    for (int column = 0; column < 3; column++) {
        for (int row = 0; row < 3; row++) {
            result[column][row] *= scale;
        }
    }
    sceVu0CopyVector(result[3], world[3]);
    result[3][3] = 1.0f;
    sceVu0CopyMatrix(lw_matrix, result);
    sceVu0CopyMatrix(matrix, result);
    changed = 0;
    ClearChildFlag();
}

int mgCFrame::GetDrawRect(mgVu0FBOX *rect, mgCDrawManager *manager) {
    mgRENDER_INFO *info = (manager ? manager : &mgDrawManager)->render_info;
    sceVu0FMATRIX world;
    if (attr && attr->billboard) {
        GetBBoardMatrix(attr->billboard, world, info);
    } else {
        GetLWMatrixTopBottom(world);
    }
    float high[4] = {};
    float low[4] = {};
    int flags = attr ? attr->draw : MG_FRAME_DRAW_VISIBLE;
    int visible = 0;
    if ((flags & MG_FRAME_DRAW_VISIBLE) && visual && bound) {
        test1(bound->corner, info->world_screen_rel, world, high, low);
        test2(high, low);
        float half_width = mgScreenWidth * 0.5f;
        float half_height = mgScreenHeight * 0.5f;
        float half_frame = port_screen_width() * 0.5f;
        visible = low[0] <= half_frame && high[0] >= -half_frame && low[1] <= half_height && high[1] >= -half_height && high[3] >= info->clip_min[2];
        if (visible) {
            high[0] += half_width;
            low[0] += half_width;
            high[1] += half_height;
            low[1] += half_height;
        }
    }
    if (!(flags & MG_FRAME_DRAW_SKIP_CHILDREN)) {
        for (mgCFrame *node = child; node; node = node->brother) {
            if (node->attr && (node->attr->draw & MG_FRAME_DRAW_SKIP_BY_PARENT)) {
                continue;
            }
            mgVu0FBOX child_rect;
            if (!node->GetDrawRect(&child_rect, nullptr)) {
                continue;
            }
            for (int lane = 0; lane < 4; lane++) {
                high[lane] = visible ? std::fmax(high[lane], child_rect.max[lane]) : child_rect.max[lane];
                low[lane] = visible ? std::fmin(low[lane], child_rect.min[lane]) : child_rect.min[lane];
            }
            visible = 1;
        }
    }
    if ((flags & MG_FRAME_DRAW_SKIP_CHILDREN) && !visible) {
        return 0;
    }
    sceVu0CopyVector(rect->max, high);
    sceVu0CopyVector(rect->min, low);
    return visible;
}
