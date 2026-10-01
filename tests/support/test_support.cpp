#include "test.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ecgtest {

unsigned long ProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(::_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

#if defined(_WIN32)
namespace {

std::wstring Widen(const std::string& text) {
  // Argument text on this platform is ASCII in practice (paths, flags, and
  // identifiers); anything else is passed through byte for byte.
  std::wstring wide;
  wide.reserve(text.size());
  for (const char c : text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return wide;
}

/// Applies the documented Windows command line quoting rules so an argument
/// containing spaces or quotes survives intact.
std::wstring QuoteArgument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring quoted = L"\"";
  for (std::size_t index = 0; index < argument.size();) {
    std::size_t backslashes = 0;
    while (index < argument.size() && argument[index] == L'\\') {
      ++backslashes;
      ++index;
    }
    if (index == argument.size()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (argument[index] == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(argument[index]);
    ++index;
  }
  quoted.push_back(L'"');
  return quoted;
}

}  // namespace
#endif

ChildOutput RunChildProcessCapture(const std::filesystem::path& executable,
                                   const std::vector<std::string>& arguments) {
  ChildOutput result;
  const std::filesystem::path out_path =
      std::filesystem::temp_directory_path() /
      ("ecg_child_out_" + std::to_string(ProcessId()) + "_" + std::to_string(::rand()) + ".txt");
  const std::filesystem::path err_path =
      std::filesystem::temp_directory_path() /
      ("ecg_child_err_" + std::to_string(ProcessId()) + "_" + std::to_string(::rand()) + ".txt");

#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE out_handle = CreateFileW(out_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE err_handle = CreateFileW(err_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out_handle == INVALID_HANDLE_VALUE || err_handle == INVALID_HANDLE_VALUE) {
    if (out_handle != INVALID_HANDLE_VALUE) CloseHandle(out_handle);
    if (err_handle != INVALID_HANDLE_VALUE) CloseHandle(err_handle);
    return result;
  }

  std::wstring command_line = QuoteArgument(executable.wstring());
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(QuoteArgument(Widen(argument)));
  }
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out_handle;
  startup.hStdError = err_handle;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                     &startup, &process);
  if (started == 0) {
    CloseHandle(out_handle);
    CloseHandle(err_handle);
    std::error_code ignore;
    std::filesystem::remove(out_path, ignore);
    std::filesystem::remove(err_path, ignore);
    return result;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(out_handle);
  CloseHandle(err_handle);
  result.exit_code = static_cast<int>(code);
#else
  ::posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_addopen(&actions, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0644);
  ::posix_spawn_file_actions_addopen(&actions, 2, err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0644);
  std::vector<std::string> storage;
  storage.push_back(executable.string());
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  for (std::string& value : storage) {
    argv.push_back(value.data());
  }
  argv.push_back(nullptr);
  ::pid_t pid = 0;
  const int spawn_status = ::posix_spawn(&pid, executable.string().c_str(), &actions, nullptr,
                                         argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (spawn_status != 0) {
    return result;
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

  (void)ReadTextFile(out_path, &result.out);
  (void)ReadTextFile(err_path, &result.err);
  std::error_code ignore;
  std::filesystem::remove(out_path, ignore);
  std::filesystem::remove(err_path, ignore);
  return result;
}

int RunChildProcess(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments) {
  // The launcher below builds a correctly quoted command line, so an argument
  // containing spaces survives. Child output is discarded here because the
  // multiprocess tests communicate through files by design.
  return RunChildProcessCapture(executable, arguments).exit_code;
}

bool ReadTextFile(const std::filesystem::path& path, std::string* out) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[4096];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      out->append(buffer, read);
    }
    if (read < sizeof(buffer)) {
      break;
    }
  }
  std::fclose(file);
  return true;
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
  const bool ok = written == text.size() && std::fclose(file) == 0;
  return ok;
}

}  // namespace ecgtest

int main(int argc, char** argv) {
  const std::vector<ecgtest::TestCase>& tests = ecgtest::Registry::Instance().tests();
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--list") {
      for (const ecgtest::TestCase& test : tests) {
        std::printf("%s\n", test.name.c_str());
      }
      return 0;
    }
    if (argument.rfind("--filter=", 0) == 0) {
      filter = std::string(argument.substr(9));
    }
  }

  int failed = 0;
  int executed = 0;
  for (const ecgtest::TestCase& test : tests) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    ecgtest::CurrentTest::Instance().Begin(test.name);
    test.function();
    const ecgtest::CurrentTest& current = ecgtest::CurrentTest::Instance();
    if (current.failures() == 0) {
      std::printf("PASS %s (%d checks)\n", test.name.c_str(), current.checks());
      std::fflush(stdout);
    } else {
      ++failed;
      std::printf("FAIL %s (%d failures)\n", test.name.c_str(), current.failures());
      for (const std::string& message : current.messages()) {
        std::printf("       %s\n", message.c_str());
      }
      std::fflush(stdout);
    }
  }
  std::printf("%d test(s) executed, %d failed\n", executed, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}
