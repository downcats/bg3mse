#pragma once

#include <string>

// Norbyte's List, TMap and the tables built on them (GameDefinitions/Osiris.h),
// compiled as his sources are.

namespace bg3se::mac::test {

// Run on containers of the standard library the story library is built with
// (libc++): a std::list read as a List, a List built as his code builds one
// read as a std::list, a std::map searched and walked as a TMap, and a
// function table with one such map in a bucket. Everything that did not
// hold, one to a line; empty when all did. (StoryContainersNorbyte.cpp)
std::string CheckStoryContainers();

// Lists built and walked through his types alone, in one function, with the
// optimisation and the alias analysis his sources are compiled with: the end
// node of a list is reached as members of the list and as a node, and a write
// through one must be seen by a read through the other. (StoryListsNorbyte.cpp)
std::string CheckStoryListsBuilt();

}
