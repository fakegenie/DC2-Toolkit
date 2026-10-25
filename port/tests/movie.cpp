#include <cassert>
#include "movie.hpp"

int main() {
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
