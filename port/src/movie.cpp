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

