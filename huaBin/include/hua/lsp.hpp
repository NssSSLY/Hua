#pragma once
#include <iostream>

namespace hua {
// Run the Language Server Protocol (JSON-RPC 2.0 over stdio) loop.
int run_lsp(std::istream& in = std::cin, std::ostream& out = std::cout);
}
