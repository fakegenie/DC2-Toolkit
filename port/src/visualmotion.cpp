#include "visualmotion.hpp"

void visual_build_face(const mgCVisualMDT *visual, const mgCFace *face, const mgVertexWeight *weights);

PORT_OVERRIDE int mgCVisualMotionMDT::CreateFaceMotionPacket(u_int *packet, mgCFace *face, mgCVMotionData *motion) {
    (void)packet;
    (void)motion;
    if (face != NULL && weight != NULL) visual_build_face(this, face, weight);
    return 0;
}
