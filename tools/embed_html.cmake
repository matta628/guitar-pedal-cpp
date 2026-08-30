# Turns web/index.html into a C++ header holding it as a raw string literal, so
# the pedal ships as one binary with no runtime file lookup and no --ui-dir flag
# to get wrong on the Pi.
#
# Invoked by CMake as:
#   cmake -DINPUT=<html> -DOUTPUT=<header> -P tools/embed_html.cmake
#
# The delimiter must not appear in the page. PEDALPAGE is unlikely enough that
# the check below is a belt-and-braces assert rather than a real constraint.

file(READ "${INPUT}" PAGE_CONTENT)

string(FIND "${PAGE_CONTENT}" ")PEDALPAGE\"" BAD_DELIM)
if(NOT BAD_DELIM EQUAL -1)
  message(FATAL_ERROR "${INPUT} contains the raw-string delimiter )PEDALPAGE\"")
endif()

file(WRITE "${OUTPUT}"
"#pragma once

// Generated from web/index.html by tools/embed_html.cmake. Do not edit by hand.

namespace webui {
inline constexpr char kIndexHtml[] = R\"PEDALPAGE(${PAGE_CONTENT})PEDALPAGE\";
}  // namespace webui
")
