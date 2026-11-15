#pragma once

#define RAPIDJSON_PARSE_DEFAULT_FLAGS kParseCommentsFlag | kParseTrailingCommasFlag | kParseNanAndInfFlag | kParseIterativeFlag
#define RAPIDJSON_WRITE_DEFAULT_FLAGS kWriteNanAndInfFlag | kWriteNanAndInfNullFlag
#define RAPIDJSON_SIMD
#if defined(__aarch64__)
#define RAPIDJSON_NEON
#else
#define RAPIDJSON_SSE42
#endif
#define RAPIDJSON_HAS_STDSTRING 1

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
