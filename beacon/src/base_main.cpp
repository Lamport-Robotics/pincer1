// Built only for env:base-heltec. The base station firmware lives in base/src/main.cpp; PlatformIO
// compiles and scans dependencies from src/ alone, so this shim names the libraries it needs (the
// dependency finder does not follow the include below) and pulls it in. Nothing else.
#ifdef BOARD_HELTEC_V3
#include <Preferences.h>
#include <RadioLib.h>
#include "../base/src/main.cpp"
#endif
