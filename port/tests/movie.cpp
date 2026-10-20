#include <cassert>
#include "movie.hpp"

static int event(sceMpeg *, sceMpegCbData *, void *) { return 1; }

int main() {
    sceMpeg mpeg;
    assert(sceMpegCreate(&mpeg, nullptr, 0) == 0);
    assert(sceMpegAddCallback(&mpeg, sceMpegCbNodata, event, nullptr) == nullptr);
    assert(sceMpegAddCallback(&mpeg, sceMpegCbNodata, nullptr, nullptr) == event);
    assert(sceMpegAddStrCallback(&mpeg, sceMpegStrM2V, 0, event, nullptr) == nullptr);
    assert(sceMpegAddStrCallback(&mpeg, sceMpegStrM2V, 0, nullptr, nullptr) == event);
    assert(sceMpegIsEnd(&mpeg) == 1 && sceMpegGetPicture(&mpeg, nullptr, 0) < 0);
    assert(sceMpegDemuxPssRing(&mpeg, nullptr, 64, nullptr, 64) == 64);
    assert(sceMpegReset(&mpeg) == 0 && sceMpegIsEnd(&mpeg) == 1);
    assert(sceMpegDelete(&mpeg) == 0);
    CMovie movie;
    movie.Load(nullptr, (mgCMemory *)nullptr, 640, 512, true, false);
    movie.Play(nullptr);
    assert(movie.IsStarted() == 1 && movie.EndCheck() == 1 && !movie.is_playing);
    movie.SwitchThread();
    movie.Term();
    movie.Load(nullptr, (mgCMemory *)nullptr, 640, 512, true, false, true);
    assert(movie.EndCheck() == 1);
    movie.Load(nullptr, (mgCMemory **)nullptr, 640, 512, true, false, true);
    assert(movie.EndCheck() == 1);
    movie.Term();
    return 0;
}
