// A file compiled into the executable by launcher/embed_files.cmake.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aln {

struct EmbeddedFile {
    const char* name;
    const uint8_t* data;
    size_t size;
};

} // namespace aln
