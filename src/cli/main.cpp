// Energy-Cost-Governor command line entry point.

#include <string>
#include <vector>

#include "commands.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  return ecg::cli::RunCommand(arguments);
}
