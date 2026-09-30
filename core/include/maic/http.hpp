#pragma once

// The one way to include cpp-httplib in MAIC. Its OpenSSL support changes the layout of httplib::ClientImpl,
// so a translation unit that saw the header without CPPHTTPLIB_OPENSSL_SUPPORT would link a client object of
// a different size than core's and corrupt the heap (that happened: a 34-byte write past a ClientImpl, found
// by ASan). This header refuses to compile in that case; tests/lint_includes.py refuses a direct include.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#error "CPPHTTPLIB_OPENSSL_SUPPORT is not defined: link this target against httplib::httplib (maic_core carries it PUBLIC) rather than including httplib by path"
#endif

#include <httplib.h>
