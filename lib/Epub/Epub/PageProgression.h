#pragma once

#include <cstdint>

// The OPF spine's page-progression-direction: which way the book's pages turn. Default when the spine says nothing or
// says `default`, which leaves it to the reader (EPUB 3.3, the spine element).
enum class PageProgression : uint8_t { Default = 0, Ltr, Rtl };
