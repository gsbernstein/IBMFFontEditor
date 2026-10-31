#pragma once

#include <QString>

// Unicode character name in sentence case (e.g. "Latin small letter y with acute"), or an empty
// string when the code point is outside the generated table.
QString unicodeName(char32_t codePoint);
