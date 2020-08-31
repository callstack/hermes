/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HBCParser.h"
#include "ProfileAnalyzer.h"

#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/Public/Buffer.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/MemoryBuffer.h"

#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/CommandLine.h"
#include "llvh/Support/FileSystem.h"
#include "llvh/Support/InitLLVM.h"
#include "llvh/Support/MemoryBuffer.h"
#include "llvh/Support/PrettyStackTrace.h"
#include "llvh/Support/Signals.h"
#include "llvh/Support/raw_ostream.h"

#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace hermes;
using namespace hermes::hbc;

using llvh::raw_fd_ostream;

static llvh::cl::opt<std::string> InputFilename(
    llvh::cl::Positional,
    llvh::cl::desc("input file"),
    llvh::cl::Required);

static llvh::cl::opt<std::string> DumpOutputFilename(
    "out",
    llvh::cl::desc("Output file name"));

static llvh::cl::opt<std::string> SourceMapFilename(
    "source-map",
    llvh::cl::desc("Optional source-map file name, used by function-info"));

static llvh::cl::opt<std::string> StartupCommands(
    "c",
    llvh::cl::desc(
        "A list of commands to execute before entering "
        "interactive mode separated by semicolon. "
        "You can use this option to execute a bunch of commands "
        "without entering interactive mode, like -c \"cmd1;cmd2;quit\""));

enum class DisassemblyFormat {
  Raw,
  Pretty,
  Objdump,
};

static llvh::cl::opt<DisassemblyFormat> DisassemblyOutputFormat(
    llvh::cl::desc("Disassembly formatting:"),
    llvh::cl::init(DisassemblyFormat::Pretty),
    llvh::cl::values(
        clEnumValN(DisassemblyFormat::Raw, "raw-disassemble", "Legacy format"),
        clEnumValN(
            DisassemblyFormat::Pretty,
            "pretty-disassemble",
            "Pretty print"),
        clEnumValN(
            DisassemblyFormat::Objdump,
            "objdump-disassemble",
            "Like objdump")));

static llvh::cl::opt<std::string> AnalyzeMode(
    "mode",
    llvh::cl::desc(
        "The analysis mode you want to use(either instruction or function)"));

static llvh::cl::opt<std::string> ProfileFile(
    "profile-file",
    llvh::cl::desc(
        "Log file in json format generated by basic block profiler"));

static llvh::cl::opt<bool> ShowSectionRanges(
    "show-section-ranges",
    llvh::cl::init(false),
    llvh::cl::desc("Show the byte range of each section in bytecode"));

static llvh::cl::opt<bool> HumanizeSectionRanges(
    "human",
    llvh::cl::init(false),
    llvh::cl::desc("Print bytecode section ranges in hex format"));

static bool executeCommand(
    llvh::raw_ostream &os,
    ProfileAnalyzer &analyzer,
    BytecodeDisassembler &disassembler,
    const std::string &commandWithOptions);

/// Wrapper around std::getline().
/// Read a line from cin, storing it into \p line.
/// \return true if we have a line, false if input was exhausted.
static bool getline(std::string &line) {
  for (;;) {
    // On receiving EINTR, getline() in libc++ appears to incorrectly mark
    // cin's EOF bit. This means that sucessive getline() calls will return
    // EOF. Workaround this iostream bug by clearing the cin flags on EINTR.
    errno = 0;
    if (std::getline(std::cin, line)) {
      return true;
    } else if (errno == EINTR) {
      std::cin.clear();
    } else {
      // Input exhausted.
      return false;
    }
  }
}

static void printHelp(llvh::Optional<llvh::StringRef> command = llvh::None) {
  // Declare variables for help text.
  static const std::unordered_map<std::string, std::string> commandToHelpText = {
      {"function",
       "'function': Compute the runtime instruction frequency "
       "for each function and display in desceding order."
       "Each function name is displayed together with its source code line number.\n\n"
       "'function <FUNC_ID>': Dump basic block stats for function with id <FUNC_ID>.\n\n"
       "'function -used': List all invoked function IDs, one per line.\n\n"
       "USAGE: function [<FUNC_ID> | -used]\n"
       "       fun [<FUNC_ID> | -used]\n"},
      {"instruction",
       "Computes the runtime instruction frequency for each instruction"
       "and displays it in descending order.\n\n"
       "USAGE: instruction\n"
       "       inst\n"},
      {"disassemble",
       "'disassemble': Display bytecode disassembled output of whole binary.\n"
       "'disassemble <FUNC_ID>': Display bytecode disassembled output of function with id <FUNC_ID>.\n"
       "Add the '-offsets' flag to show virtual offsets for all instructions.\n\n"
       "USAGE: disassemble <FUNC_ID> [-offsets]\n"
       "       dis <FUNC_ID> [-offsets]\n"},
      {"summary",
       "Display overall summary information.\n\n"
       "USAGE: summary\n"},
      {"io",
       "Visualize function page I/O access working set"
       "in basic block profile trace.\n\n"
       "USAGE: io\n"},
      {"block",
       "Display top hot basic blocks in sorted order.\n\n"
       "USAGE: block\n"},
      {"at-virtual",
       "Display information about the function at a given virtual offset.\n\n"
       "USAGE: at-virtual <OFFSET>\n"},
      {"help",
       "Help instructions for hbcdump tool commands.\n\n"
       "USAGE: help <COMMAND>\n"
       "       h <COMMAND>\n"},
      {"function-info",
       "Display info about a specific function, or all functions\n\n"
       "USAGE: function-info [<FUNC_ID>]\n"
       "NOTE: Virtual offset is the offset from the beginning of the segment\n"},
      {"string",
       "Display string for ID\n\n"
       "USAGE: string <STRING_ID>\n"},
      {"filename",
       "Display file name for ID\n\n"
       "USAGE: filename <FILENAME_ID>\n"},
      {"epilogue",
       "Dump the epilogue.\n\n"
       "USAGE: epilogue\n"},
  };

  if (command.hasValue() && !command->empty()) {
    const auto it = commandToHelpText.find(*command);
    if (it == commandToHelpText.end()) {
      llvh::outs() << "Invalid command: " << *command << '\n';
      return;
    }
    llvh::outs() << it->second;
  } else {
    static const std::string topLevelHelpText =
        "These commands are defined internally. Type `help' to see this list.\n"
        "Type `help name' to find out more about the function `name'.\n\n";
    llvh::outs() << topLevelHelpText;
    for (const auto it : commandToHelpText) {
      llvh::outs() << it.first << '\n';
    }
  }
}

/// Enters interactive command loop.
static void enterCommandLoop(
    llvh::raw_ostream &os,
    std::shared_ptr<hbc::BCProvider> bcProvider,
    llvh::Optional<std::unique_ptr<llvh::MemoryBuffer>> profileBufferOpt,
    std::unique_ptr<SourceMap> &&sourceMap,
    const std::vector<std::string> &startupCommands) {
  BytecodeDisassembler disassembler(bcProvider);

  // Include source information and func IDs by default in disassembly output.
  DisassemblyOptions options = DisassemblyOptions::IncludeSource |
      DisassemblyOptions::IncludeFunctionIds;
  switch (DisassemblyOutputFormat) {
    case DisassemblyFormat::Raw:
      break;
    case DisassemblyFormat::Pretty:
      options = options | DisassemblyOptions::Pretty;
      break;
    case DisassemblyFormat::Objdump:
      options = options | DisassemblyOptions::Objdump;
      break;
  }
  disassembler.setOptions(options);
  ProfileAnalyzer analyzer(
      os,
      bcProvider,
      profileBufferOpt.hasValue()
          ? llvh::Optional<std::unique_ptr<llvh::MemoryBuffer>>(
                std::move(profileBufferOpt.getValue()))
          : llvh::None,
      std::move(sourceMap));

  // Process startup commands.
  bool terminateLoop = false;
  for (const auto &command : startupCommands) {
    if (executeCommand(os, analyzer, disassembler, command)) {
      terminateLoop = true;
    }
  }

  while (!terminateLoop) {
    os << "hbcdump> ";
    std::string line;
    if (!getline(line)) {
      break;
    }
    terminateLoop = executeCommand(os, analyzer, disassembler, line);
  }
}

/// Find the first instance of a value in a container and remove it.
/// \return true if the value was found and removed, false otherwise.
template <typename Container, typename Value>
static bool findAndRemoveOne(Container &haystack, const Value &needle) {
  auto it = std::find(haystack.begin(), haystack.end(), needle);
  if (it != haystack.end()) {
    haystack.erase(it);
    return true;
  }
  return false;
}

/// Simple RAII helper for setting and reverting disassembler options.
class DisassemblerOptionsHolder {
 public:
  DisassemblerOptionsHolder(
      BytecodeDisassembler &disassembler,
      DisassemblyOptions newOptions)
      : disassembler_(disassembler), savedOptions_(disassembler.getOptions()) {
    disassembler_.setOptions(newOptions);
  }

  ~DisassemblerOptionsHolder() {
    disassembler_.setOptions(savedOptions_);
  }

 private:
  BytecodeDisassembler &disassembler_;
  DisassemblyOptions savedOptions_;
};

/// Execute a single command from \p commandTokens.
/// \return true telling caller to terminate the interactive command loop.
static bool executeCommand(
    llvh::raw_ostream &os,
    ProfileAnalyzer &analyzer,
    BytecodeDisassembler &disassembler,
    const std::string &commandWithOptions) {
  // Parse command tokens.
  llvh::SmallVector<llvh::StringRef, 8> commandTokens;
  llvh::StringRef(commandWithOptions).split(commandTokens, ' ');
  if (commandTokens.empty()) {
    // Ignore empty input.
    return false;
  }

  const llvh::StringRef command = commandTokens[0];
  if (command == "function" || command == "fun") {
    if (findAndRemoveOne(commandTokens, "-used")) {
      analyzer.dumpUsedFunctionIDs();
    } else if (commandTokens.size() == 1) {
      analyzer.dumpFunctionStats();
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      analyzer.dumpFunctionBasicBlockStat(funcId);
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "instruction" || command == "inst") {
    if (commandTokens.size() == 1) {
      analyzer.dumpInstructionStats();
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "disassemble" || command == "dis") {
    auto localOptions = findAndRemoveOne(commandTokens, "-offsets")
        ? DisassemblyOptions::IncludeVirtualOffsets
        : DisassemblyOptions::None;
    DisassemblerOptionsHolder optionsHolder(
        disassembler, disassembler.getOptions() | localOptions);
    if (commandTokens.size() == 1) {
      disassembler.disassemble(os);
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      if (funcId >= disassembler.getFunctionCount()) {
        os << "Error: no function with id: " << funcId << " exists.\n";
        return false;
      }
      disassembler.disassembleFunction(funcId, os);
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "string" || command == "str") {
    if (commandTokens.size() != 2) {
      printHelp(command);
      return false;
    }
    uint32_t stringId;
    if (commandTokens[1].getAsInteger(0, stringId)) {
      os << "Error: cannot parse string_id as integer.\n";
      return false;
    }
    analyzer.dumpString(stringId);
  } else if (command == "filename") {
    if (commandTokens.size() != 2) {
      printHelp(command);
      return false;
    }
    uint32_t filenameId;
    if (commandTokens[1].getAsInteger(0, filenameId)) {
      os << "Error: cannot parse filename_id as integer.\n";
      return false;
    }
    analyzer.dumpFileName(filenameId);
  } else if (command == "function-info") {
    JSONEmitter json(os, /* pretty */ true);
    if (commandTokens.size() == 1) {
      analyzer.dumpAllFunctionInfo(json);
    } else if (commandTokens.size() == 2) {
      uint32_t funcId;
      if (commandTokens[1].getAsInteger(0, funcId)) {
        os << "Error: cannot parse func_id as integer.\n";
        return false;
      }
      analyzer.dumpFunctionInfo(funcId, json);
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "io") {
    analyzer.dumpIO();
  } else if (command == "summary" || command == "sum") {
    analyzer.dumpSummary();
  } else if (command == "block") {
    analyzer.dumpBasicBlockStats();
  } else if (command == "at_virtual" || command == "at-virtual") {
    JSONEmitter json(os, /* pretty */ true);
    if (commandTokens.size() == 2) {
      uint32_t virtualOffset;
      if (commandTokens[1].getAsInteger(0, virtualOffset)) {
        os << "Error: cannot parse virtualOffset as integer.\n";
        return false;
      }
      auto funcId = analyzer.getFunctionFromVirtualOffset(virtualOffset);
      if (funcId.hasValue()) {
        analyzer.dumpFunctionInfo(*funcId, json);
      } else {
        os << "Virtual offset " << virtualOffset << " is invalid.\n";
      }
    } else {
      printHelp(command);
      return false;
    }
  } else if (command == "epilogue" || command == "epi") {
    analyzer.dumpEpilogue();
  } else if (command == "help" || command == "h") {
    // Interactive help command.
    if (commandTokens.size() == 2) {
      printHelp(commandTokens[1]);
    } else {
      printHelp();
    }
    return false;
  } else if (command == "quit") {
    // Quit command loop.
    return true;
  } else {
    printHelp(command);
    return false;
  }
  os << "\n";
  return false;
}

int main(int argc, char **argv) {
#ifndef HERMES_FBCODE_BUILD
  // Normalize the arg vector.
  llvh::InitLLVM initLLVM(argc, argv);
#else
  // When both HERMES_FBCODE_BUILD and sanitizers are enabled, InitLLVM may have
  // been already created and destroyed before main() is invoked. This presents
  // a problem because InitLLVM can't be instantiated more than once in the same
  // process. The most important functionality InitLLVM provides is shutting
  // down LLVM in its destructor. We can use "llvm_shutdown_obj" to do the same.
  llvh::llvm_shutdown_obj Y;
#endif
  llvh::cl::ParseCommandLineOptions(argc, argv, "Hermes bytecode dump tool\n");

  llvh::ErrorOr<std::unique_ptr<llvh::MemoryBuffer>> fileBufOrErr =
      llvh::MemoryBuffer::getFile(InputFilename);

  if (!fileBufOrErr) {
    llvh::errs() << "Error: fail to open file: " << InputFilename << ": "
                 << fileBufOrErr.getError().message() << "\n";
    return -1;
  }

  auto buffer =
      llvh::make_unique<hermes::MemoryBuffer>(fileBufOrErr.get().get());
  const uint8_t *bytecodeStart = buffer->data();
  auto ret =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer));
  if (!ret.first) {
    llvh::errs() << "Error: fail to deserializing bytecode: " << ret.second;
    return 1;
  }

  // Parse startup commands list(separated by semicolon).
  std::vector<std::string> startupCommands;
  if (!StartupCommands.empty()) {
    std::istringstream iss(StartupCommands.data());
    std::string command;
    while (getline(iss, command, ';')) {
      startupCommands.emplace_back(command);
    }
  }

  llvh::Optional<raw_fd_ostream> fileOS;
  if (!DumpOutputFilename.empty()) {
    std::error_code EC;
    fileOS.emplace(DumpOutputFilename.data(), EC, llvh::sys::fs::F_Text);
    if (EC) {
      llvh::errs() << "Error: fail to open file " << DumpOutputFilename << ": "
                   << EC.message() << '\n';
      return -1;
    }
  }
  auto &output = fileOS ? *fileOS : llvh::outs();

  std::unique_ptr<SourceMap> sourceMap;
  if (!SourceMapFilename.empty()) {
    llvh::ErrorOr<std::unique_ptr<llvh::MemoryBuffer>> sourceMapBufOrErr =
        llvh::MemoryBuffer::getFile(SourceMapFilename);
    if (!sourceMapBufOrErr) {
      llvh::errs() << "Error: fail to open file: " << SourceMapFilename << ": "
                   << sourceMapBufOrErr.getError().message() << "\n";
      return -1;
    }
    sourceMap = SourceMapParser::parse(*sourceMapBufOrErr.get().get());
    if (!sourceMap) {
      llvh::errs() << "Error loading source map: " << SourceMapFilename << "\n";
      return -1;
    }
  }

  if (ProfileFile.empty()) {
    if (ShowSectionRanges) {
      BytecodeSectionWalker walker(bytecodeStart, std::move(ret.first), output);
      walker.printSectionRanges(HumanizeSectionRanges);
    } else {
      enterCommandLoop(
          output,
          std::move(ret.first),
          llvh::None,
          std::move(sourceMap),
          startupCommands);
    }
  } else {
    llvh::ErrorOr<std::unique_ptr<llvh::MemoryBuffer>> profileBuffer =
        llvh::MemoryBuffer::getFile(ProfileFile);
    if (!profileBuffer) {
      llvh::errs() << "Error: fail to open file: " << ProfileFile
                   << profileBuffer.getError().message() << "\n";
      return -1;
    }
    enterCommandLoop(
        output,
        std::move(ret.first),
        std::move(profileBuffer.get()),
        std::move(sourceMap),
        startupCommands);
  }

  return 0;
}
