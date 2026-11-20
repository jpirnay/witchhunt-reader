// The SDK's WolfsslCrypto.cpp, built here alone (see WolfsslCrypto.h beside this file). It compiles to nothing
// without FREEINK_CONTENT_WOLFSSL (platformio.ini).
//
// wolfSSL's settings first, so that PlatformIO's library finder, which does not follow the relative include below,
// links this library against wolfSSL.
#include <wolfssl/wolfcrypt/settings.h>

#include "../../freeink-sdk/libs/book/ContentProtection/src/WolfsslCrypto.cpp"
