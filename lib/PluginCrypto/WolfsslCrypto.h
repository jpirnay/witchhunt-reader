#pragma once

// The Free-Ink SDK's WolfsslCrypto (RSA, PKCS#12, AES, SHA over our wolfSSL), for POST /api/crypto. Only this class
// of the SDK's libs/book/ContentProtection is built (PluginCrypto.cpp): its zip reader and vendored miniz stay out, as
// protected books are decrypted on our own ZipFile and uzlib. This header forwards to the SDK's, so the SDK source
// built here finds it too.
#include "../../freeink-sdk/libs/book/ContentProtection/include/WolfsslCrypto.h"
