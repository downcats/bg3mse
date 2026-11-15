#pragma once

// The names of the story entries (Engine/Entries/Osiris.cpp) and of the
// operands the binding reads back from them (Mac/Binding/OsirisBinding.cpp):
// an operand is asked for by what it is, never by its position in a list.

namespace bg3se::mac::engine::osiris {

// The place where the game writes the story header, and the one where it
// closes the generated story.
inline constexpr char kWriteHeader[] = "osi::OsirisManager::WriteHeader";
inline constexpr char kCloseStory[] = "esv::StoryImplementation::ComposeAndBuildStory: call of ls::FileWriter::Close";
inline constexpr char kWriterClose[] = "ls::FileWriter::Close";

// The game's function that packs a story function's handle.
inline constexpr char kGetHandle[] = "osi::OsirisInterface::GetHandle";

// kWriteHeader: the offset of the byte of an ls::Path that says where its
// text is (in the path itself unless the byte is negative, else behind the
// pointer at its start).
inline constexpr char kPathTextFlag[] = "path text flag";

// kCloseStory: the generated story's writer and the path it writes to are
// locals of the function, each at an offset from its stack pointer. The
// writer is constructed there from the path, closed there, and the path is
// then made the wide string the story is compiled from.
inline constexpr char kWriterConstructed[] = "writer constructed";
inline constexpr char kWriterConstructedFrom[] = "from the path";
inline constexpr char kWriterClosed[] = "writer closed";
inline constexpr char kPathCompiled[] = "path compiled";

}
