#pragma once

#include <vector>

namespace maic::protocol {

// One file of protocol/, as cmake/embed_protocol.cmake wrote it into the generated protocol_files.cpp: its path
// under protocol/ and its text in chunks, to be joined.
struct EmbeddedFile {
    const char* path;
    std::vector<const char*> chunks;
};
const std::vector<EmbeddedFile>& embedded_files();

}  // namespace maic::protocol
