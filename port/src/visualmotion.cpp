#include "visualmotion.hpp"
#include "mg_math.hpp"

void visual_build_face(const mgCVisualMDT *visual, const mgCFace *face, const mgVertexWeight *weights);

PORT_OVERRIDE int mgCVisualMotionMDT::CreateFaceMotionPacket(u_int *packet, mgCFace *face, mgCVMotionData *motion) {
    (void)packet;
    (void)motion;
    if (face != NULL && weight != NULL) visual_build_face(this, face, weight);
    return 0;
}

int motion_bones(mgCVisualMotionMDT *visual, float (*matrix)[4], float (*bones)[4][4]) {
    if (visual->frame == NULL || visual->base_matrix == NULL) return 0;
    int slots = 0;
    while (slots < (int)(sizeof visual->bone / sizeof visual->bone[0]) && visual->bone[slots] >= 0) slots++;
    if (slots == 0) return 0;
    float inverse_matrix[4][4], root_matrix[4][4], inverse_root[4][4], root_local[4][4];
    mgInversMatrix(inverse_matrix, matrix);
    visual->frame[0]->GetLWMatrix(root_matrix);
    mgMulMatrix(inverse_root, root_matrix, visual->base_matrix[visual->frame_id]);
    mgInversMatrix(root_local, inverse_root);
    for (int i = 0; i < slots; i++) {
        int slot = visual->bone[i];
        if (visual->frame[slot] == NULL) continue;
        float frame_matrix[4][4], slot_matrix[4][4], slot_inverse[4][4];
        visual->frame[slot]->GetLWMatrix(frame_matrix);
        mgMulMatrix(slot_matrix, root_matrix, visual->base_matrix[slot]);
        mgMulMatrix(slot_matrix, root_local, slot_matrix);
        mgInversMatrix(slot_inverse, slot_matrix);
        mgMulMatrix(frame_matrix, inverse_matrix, frame_matrix);
        mgMulMatrix(bones[i], frame_matrix, slot_inverse);
    }
    return slots;
}

PORT_OVERRIDE int mgCVisualMotionMDT::CreateExtRenderInfoPacket(u_int *packet, float (*matrix)[4], mgRENDER_INFO *render_info) {
    (void)packet;
    (void)matrix;
    (void)render_info;
    return 0;
}
