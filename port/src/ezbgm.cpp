#include "ezbgm.hpp"
#include "sound.hpp"

PORT_OVERRIDE int CSound::StreamOpenState() { return 0; }
