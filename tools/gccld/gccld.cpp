//===- gccld.cpp - LLVM 'ld' compatible linker ----------------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This utility is intended to be compatible with GCC, and follows standard
// system 'ld' conventions.  As such, the default output file is ./a.out.
// Additionally, this program outputs a shell script that is used to invoke LLI
// to execute the program.  In this manner, the generated executable (a.out for
// example), is directly executable, whereas the bytecode file actually lives in
// the a.out.bc file generated by this program.  Also, Force is on by default.
//
// Note that if someone (or a script) deletes the executable program generated,
// the .bc file will be left around.  Considering that this is a temporary hack,
// I'm not too worried about this.
//
//===----------------------------------------------------------------------===//

#include "gccld.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Bytecode/WriteBytecodePass.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Linker.h"
#include "Support/CommandLine.h"
#include "Support/FileUtilities.h"
#include "Support/Signals.h"
#include "Support/SystemUtils.h"
#include <fstream>
#include <memory>

using namespace llvm;

namespace {
  cl::list<std::string> 
  InputFilenames(cl::Positional, cl::desc("<input bytecode files>"),
                 cl::OneOrMore);

  cl::opt<std::string> 
  OutputFilename("o", cl::desc("Override output filename"), cl::init("a.out"),
                 cl::value_desc("filename"));

  cl::opt<bool>    
  Verbose("v", cl::desc("Print information about actions taken"));
  
  cl::list<std::string> 
  LibPaths("L", cl::desc("Specify a library search path"), cl::Prefix,
           cl::value_desc("directory"));

  cl::list<std::string> 
  Libraries("l", cl::desc("Specify libraries to link to"), cl::Prefix,
            cl::value_desc("library prefix"));

  cl::opt<bool>
  Strip("s", cl::desc("Strip symbol info from executable"));

  cl::opt<bool>
  NoInternalize("disable-internalize",
                cl::desc("Do not mark all symbols as internal"));
  cl::alias
  ExportDynamic("export-dynamic", cl::desc("Alias for -disable-internalize"),
                cl::aliasopt(NoInternalize));

  cl::opt<bool>
  LinkAsLibrary("link-as-library", cl::desc("Link the .bc files together as a"
                                            " library, not an executable"));
  cl::alias
  Relink("r", cl::desc("Alias for -link-as-library"),
         cl::aliasopt(LinkAsLibrary));

  cl::opt<bool>    
  Native("native",
         cl::desc("Generate a native binary instead of a shell script"));
  
  // Compatibility options that are ignored but supported by LD
  cl::opt<std::string>
  CO3("soname", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<std::string>
  CO4("version-script", cl::Hidden, cl::desc("Compatibility option: ignored"));
  cl::opt<bool>
  CO5("eh-frame-hdr", cl::Hidden, cl::desc("Compatibility option: ignored"));
}

namespace llvm {

/// PrintAndReturn - Prints a message to standard error and returns a value
/// usable for an exit status.
///
/// Inputs:
///  progname - The name of the program (i.e. argv[0]).
///  Message  - The message to print to standard error.
///  Extra    - Extra information to print between the program name and thei
///             message.  It is optional.
///
/// Return value:
///  Returns a value that can be used as the exit status (i.e. for exit()).
///
int
PrintAndReturn(const char *progname,
               const std::string &Message,
               const std::string &Extra)
{
  std::cerr << progname << Extra << ": " << Message << "\n";
  return 1;
}

/// CopyEnv - This function takes an array of environment variables and makes a
/// copy of it.  This copy can then be manipulated any way the caller likes
/// without affecting the process's real environment.
///
/// Inputs:
///  envp - An array of C strings containing an environment.
///
/// Return value:
///  NULL - An error occurred.
///
///  Otherwise, a pointer to a new array of C strings is returned.  Every string
///  in the array is a duplicate of the one in the original array (i.e. we do
///  not copy the char *'s from one array to another).
///
char ** CopyEnv(char ** const envp) {
  // Count the number of entries in the old list;
  unsigned entries;   // The number of entries in the old environment list
  for (entries = 0; envp[entries] != NULL; entries++)
  {
    ;
  }

  // Add one more entry for the NULL pointer that ends the list.
  ++entries;

  // If there are no entries at all, just return NULL.
  if (entries == 0)
    return NULL;

  // Allocate a new environment list.
  char **newenv;
  if ((newenv = new (char *) [entries]) == NULL)
    return NULL;

  // Make a copy of the list.  Don't forget the NULL that ends the list.
  entries = 0;
  while (envp[entries] != NULL) {
    newenv[entries] = new char[strlen (envp[entries]) + 1];
    strcpy (newenv[entries], envp[entries]);
    ++entries;
  }
  newenv[entries] = NULL;

  return newenv;
}


/// RemoveEnv - Remove the specified environment variable from the environment
/// array.
///
/// Inputs:
///  name - The name of the variable to remove.  It cannot be NULL.
///  envp - The array of environment variables.  It cannot be NULL.
///
/// Notes:
///  This is mainly done because functions to remove items from the environment
///  are not available across all platforms.  In particular, Solaris does not
///  seem to have an unsetenv() function or a setenv() function (or they are
///  undocumented if they do exist).
///
void RemoveEnv(const char * name, char ** const envp) {
  for (unsigned index=0; envp[index] != NULL; index++) {
    // Find the first equals sign in the array and make it an EOS character.
    char *p = strchr (envp[index], '=');
    if (p == NULL)
      continue;
    else
      *p = '\0';

    // Compare the two strings.  If they are equal, zap this string.
    // Otherwise, restore it.
    if (!strcmp(name, envp[index]))
      *envp[index] = '\0';
    else
      *p = '=';
  }

  return;
}

} // End llvm namespace

int main(int argc, char **argv, char **envp) {
  cl::ParseCommandLineOptions(argc, argv, " llvm linker for GCC\n");

  std::string ModuleID("gccld-output");
  std::auto_ptr<Module> Composite(new Module(ModuleID));

  // We always look first in the current directory when searching for libraries.
  LibPaths.insert(LibPaths.begin(), ".");

  // If the user specified an extra search path in their environment, respect
  // it.
  if (char *SearchPath = getenv("LLVM_LIB_SEARCH_PATH"))
    LibPaths.push_back(SearchPath);

  // Remove any consecutive duplicates of the same library...
  Libraries.erase(std::unique(Libraries.begin(), Libraries.end()),
                  Libraries.end());

  // Link in all of the files
  if (LinkFiles(argv[0], Composite.get(), InputFilenames, Verbose))
    return 1; // Error already printed

  if (!LinkAsLibrary)
    LinkLibraries(argv[0], Composite.get(), Libraries, LibPaths,
                  Verbose, Native);

  // Link in all of the libraries next...

  // Create the output file.
  std::string RealBytecodeOutput = OutputFilename;
  if (!LinkAsLibrary) RealBytecodeOutput += ".bc";
  std::ofstream Out(RealBytecodeOutput.c_str());
  if (!Out.good())
    return PrintAndReturn(argv[0], "error opening '" + RealBytecodeOutput +
                                   "' for writing!");

  // Ensure that the bytecode file gets removed from the disk if we get a
  // SIGINT signal.
  RemoveFileOnSignal(RealBytecodeOutput);

  // Generate the bytecode file.
  if (GenerateBytecode(Composite.get(), Strip, !NoInternalize, &Out)) {
    Out.close();
    return PrintAndReturn(argv[0], "error generating bytecode");
  }

  // Close the bytecode file.
  Out.close();

  // If we are not linking a library, generate either a native executable
  // or a JIT shell script, depending upon what the user wants.
  if (!LinkAsLibrary) {
    // If the user wants to generate a native executable, compile it from the
    // bytecode file.
    //
    // Otherwise, create a script that will run the bytecode through the JIT.
    if (Native) {
      // Name of the Assembly Language output file
      std::string AssemblyFile = OutputFilename + ".s";

      // Mark the output files for removal if we get an interrupt.
      RemoveFileOnSignal(AssemblyFile);
      RemoveFileOnSignal(OutputFilename);

      // Determine the locations of the llc and gcc programs.
      std::string llc = FindExecutable("llc", argv[0]);
      std::string gcc = FindExecutable("gcc", argv[0]);
      if (llc.empty())
        return PrintAndReturn(argv[0], "Failed to find llc");

      if (gcc.empty())
        return PrintAndReturn(argv[0], "Failed to find gcc");

      // Generate an assembly language file for the bytecode.
      if (Verbose) std::cout << "Generating Assembly Code\n";
      GenerateAssembly(AssemblyFile, RealBytecodeOutput, llc, envp);
      if (Verbose) std::cout << "Generating Native Code\n";
      GenerateNative(OutputFilename, AssemblyFile, Libraries, LibPaths,
                     gcc, envp);

      // Remove the assembly language file.
      removeFile (AssemblyFile);
    } else {
      // Output the script to start the program...
      std::ofstream Out2(OutputFilename.c_str());
      if (!Out2.good())
        return PrintAndReturn(argv[0], "error opening '" + OutputFilename +
                                       "' for writing!");
      Out2 << "#!/bin/sh\nlli \\\n";
      // gcc accepts -l<lib> and implicitly searches /lib and /usr/lib.
      LibPaths.push_back("/lib");
      LibPaths.push_back("/usr/lib");
      // We don't need to link in libc! In fact, /usr/lib/libc.so may not be a
      // shared object at all! See RH 8: plain text.
      std::vector<std::string>::iterator libc = 
        std::find(Libraries.begin(), Libraries.end(), "c");
      if (libc != Libraries.end()) Libraries.erase(libc);
      // List all the shared object (native) libraries this executable will need
      // on the command line, so that we don't have to do this manually!
      for (std::vector<std::string>::iterator i = Libraries.begin(), 
             e = Libraries.end(); i != e; ++i) {
        std::string FullLibraryPath = FindLib(*i, LibPaths, true);
        if (!FullLibraryPath.empty())
          Out2 << "    -load=" << FullLibraryPath << " \\\n";
      }
      Out2 << "    $0.bc $*\n";
      Out2.close();
    }
  
    // Make the script executable...
    MakeFileExecutable(OutputFilename);

    // Make the bytecode file readable and directly executable in LLEE as well
    MakeFileExecutable(RealBytecodeOutput);
    MakeFileReadable(RealBytecodeOutput);
  }

  return 0;
}
