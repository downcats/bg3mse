#pragma once

#include <optional>
#include <string>
#include <string_view>

// The extender's settings file, ~/Library/Application Support/bg3mse/bg3mse.conf:
//
//   # a comment          ; also a comment
//   gfx_thread_idling = 1
//
// One `key = value` per line. Keys match without regard to case, blanks around
// keys and values are dropped, and when a key appears twice the last one wins.
// Lines of 512 characters or more, and anything past the first 64 KB, are
// ignored. Settings come from this file only. The environment is read for
// what is not a setting: HOME, for where the file is, and by the start-up of
// libbg3mse.dylib and of the camera module BG3MSE_DISABLE (set to anything,
// the dylib does nothing in the process) and BG3MSE_LOG_DEBUG (the same as
// log_debug below, for a game started from a shell).
//
//   script_extender = 1
//
// starts Norbyte's Script Extender in the game (kScriptExtenderSetting). It is
// off unless set: without it libbg3mse.dylib does what it does without the
// extender, and nothing of the extender's start-up runs.
//
//   log_debug = 1
//
// makes the logs of libbg3mse.dylib and of the camera module take DEBUG lines
// too (kLogDebugSetting). Without it they hold INFO, WARNING and ERROR lines
// only; a console the game was started from shows every line either way.

namespace bg3se::mac {

inline constexpr char kScriptExtenderSetting[] = "script_extender";
inline constexpr char kLogDebugSetting[] = "log_debug";
// A mod material whose shaders are not the game's own has them translated from
// Vulkan to Metal (Mac/Shaders/Translation.h). On without the key;
// shader_translation = 0, or anything else that does not turn a switch on,
// turns it off; = all also translates the materials whose shaders are the
// game's own, which need none, for seeing translation at work
// (TranslationSettingOf, Mac/Shaders/ShaderProvision.h).
inline constexpr char kShaderTranslationSetting[] = "shader_translation";
// shader_translation_only = <part of a material's path> keeps translation to
// the materials whose path has it: one material at a time can be tried.
inline constexpr char kShaderTranslationOnlySetting[] = "shader_translation_only";

// ~/Library/Application Support/bg3mse. The home directory is HOME's, or,
// in a process that has none, the user's by the system's records; empty when
// there is neither (nothing is then read or written).
std::string DataDirectory();

// The value of `key` in the text of a settings file.
std::optional<std::string> ConfLookup(std::string_view text, std::string_view key);

// The value of `key` in bg3mse.conf, read now; nothing when the file or key is missing.
std::optional<std::string> ConfValue(std::string_view key);

// "1", "true", "yes" and "on", in any case, turn a switch on; anything else leaves it off.
bool FlagValue(std::string_view text);

// A switch in bg3mse.conf: off unless set to one of the values above.
bool FeatureEnabled(std::string_view key);

// Whether the logs take DEBUG lines: BG3MSE_LOG_DEBUG is set (to anything),
// or log_debug is switched on in bg3mse.conf.
bool LogDebugWanted();

}
