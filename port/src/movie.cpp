#include <cstdlib>
#include <cstring>
#include "movie.hpp"

extern "C" {
#include "movie.h"
}

PORT_OVERRIDE void CMovie::Load(char *name, mgCMemory **memory, int width, int height,
                                bool with_audio, bool loop, bool init_sound) {
    is_playing = false;
    port_movie_open(name, with_audio, loop);
}

PORT_OVERRIDE void CMovie::Load(char *name, mgCMemory *memory, int width, int height,
                                bool with_audio, bool loop) {
    is_playing = false;
    port_movie_open(name, with_audio, loop);
}

PORT_OVERRIDE void CMovie::Load(char *name, mgCMemory *memory, int width, int height,
                                bool with_audio, bool loop, bool init_sound) {
    is_playing = false;
    port_movie_open(name, with_audio, loop);
}

PORT_OVERRIDE void CMovie::Play(char *path) {
    port_movie_start();
    is_playing = !port_movie_end();
}
PORT_OVERRIDE void CMovie::SwitchThread() {
    port_movie_step();
    is_playing = !port_movie_end();
}
PORT_OVERRIDE void CMovie::Term() {
    port_movie_close();
    is_playing = false;
}
PORT_OVERRIDE int CMovie::EndCheck() { return port_movie_end(); }
PORT_OVERRIDE int CMovie::IsStarted() { return port_movie_started(); }

struct MpegCallbacks {
    sceMpegCallback events[7];
    sceMpegCallback streams[5][32];
};

int sceMpegInit(void) { return 0; }

int sceMpegCreate(sceMpeg *mp, u_char *work_area, int work_area_size) {
    if (!mp) return -1;
    std::memset(mp, 0, sizeof(*mp));
    mp->sys = std::calloc(1, sizeof(MpegCallbacks));
    return mp->sys ? 0 : -1;
}

int sceMpegDelete(sceMpeg *mp) {
    if (!mp) return -1;
    std::free(mp->sys);
    mp->sys = nullptr;
    return 0;
}

int sceMpegReset(sceMpeg *mp) {
    if (!mp) return -1;
    mp->frameCount = 0;
    return 0;
}

int sceMpegGetPicture(sceMpeg *mp, sceIpuRGB32 *rgb32, int mbcount) { return -1; }
int sceMpegIsEnd(sceMpeg *mp) { return 1; }

sceMpegCallback sceMpegAddCallback(sceMpeg *mp, sceMpegCbType type, sceMpegCallback callback,
                                   void *anyData) {
    if (!mp || !mp->sys || type < 0 || type >= 7) return nullptr;
    auto *state = (MpegCallbacks *)mp->sys;
    sceMpegCallback previous = state->events[type];
    state->events[type] = callback;
    return previous;
}

sceMpegCallback sceMpegAddStrCallback(sceMpeg *mp, sceMpegStrType strType, int ch,
                                      sceMpegCallback callback, void *anyData) {
    if (!mp || !mp->sys || strType < 0 || strType >= 5 || ch < 0 || ch >= 32) return nullptr;
    auto *state = (MpegCallbacks *)mp->sys;
    sceMpegCallback previous = state->streams[strType][ch];
    state->streams[strType][ch] = callback;
    return previous;
}

int sceMpegDemuxPssRing(sceMpeg *mp, u_char *start, int size, u_char *buf_start, int buf_size) {
    return size > 0 ? size : 0;
}
