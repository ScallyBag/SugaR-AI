/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef _WIN32
#if _WIN32_WINNT < 0x0601
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Force to include needed API prototypes
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include "VersionHelpers.h"
// The needed Windows API for processor groups could be missed from old Windows
// versions, so instead of calling them directly (forcing the linker to resolve
// the calls at compile time), try to load them at runtime. To do this we need
// first to define the corresponding function pointers.
extern "C" {
typedef bool(*fun1_t)(LOGICAL_PROCESSOR_RELATIONSHIP,
                      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
typedef bool(*fun2_t)(USHORT, PGROUP_AFFINITY);
typedef bool(*fun3_t)(HANDLE, CONST GROUP_AFFINITY*, PGROUP_AFFINITY);
}
#endif

#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <bitset>
#include <cstdlib>
#include <regex>

#ifdef __GNUC__
#include <sys/stat.h> //for stat()
#endif

#ifdef _MSC_VER
#else
#include <sys/types.h>
#include <dirent.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include <stdlib.h>
#include <sys/mman.h>
#endif

#if defined(__APPLE__) || defined(__ANDROID__) || defined(__OpenBSD__) || (defined(__GLIBCXX__) && !defined(_GLIBCXX_HAVE_ALIGNED_ALLOC) && !defined(_WIN32)) || defined(__e2k__)
#define POSIXALIGNEDALLOC
#include <stdlib.h>
#endif

#include "misc.h"
#include "thread.h"

using namespace std;

namespace Stockfish {

namespace {

/// Version number. If Version is left empty, then compile date in the format
/// DD-MM-YY and show in engine_info.
const string Version = "";

/// Our fancy logging facility. The trick here is to replace cin.rdbuf() and
/// cout.rdbuf() with two Tie objects that tie cin and cout to a file stream. We
/// can toggle the logging of std::cout and std:cin at runtime whilst preserving
/// usual I/O functionality, all without changing a single line of code!
/// Idea from http://groups.google.com/group/comp.lang.c++/msg/1d941c0f26ea0d81

struct Tie: public streambuf { // MSVC requires split streambuf for cin and cout

  Tie(streambuf* b, streambuf* l) : buf(b), logBuf(l) {}

  int sync() override { return logBuf->pubsync(), buf->pubsync(); }
  int overflow(int c) override { return log(buf->sputc((char)c), "<< "); }
  int underflow() override { return buf->sgetc(); }
  int uflow() override { return log(buf->sbumpc(), ">> "); }

  streambuf *buf, *logBuf;

  int log(int c, const char* prefix) {

    static int last = '\n'; // Single log file

    if (last == '\n')
        logBuf->sputn(prefix, 3);

    return last = logBuf->sputc((char)c);
  }
};

class Logger {

  Logger() : in(cin.rdbuf(), file.rdbuf()), out(cout.rdbuf(), file.rdbuf()) {}
 ~Logger() { start(""); }

  ofstream file;
  Tie in, out;

public:
  static void start(const std::string& fname) {

    static Logger l;

    if (!fname.empty() && !l.file.is_open())
    {
        l.file.open(fname, ifstream::out);

        if (!l.file.is_open())
        {
            cerr << "Unable to open debug log file " << fname << endl;
            exit(EXIT_FAILURE);
        }

        cin.rdbuf(&l.in);
        cout.rdbuf(&l.out);
    }
    else if (fname.empty() && l.file.is_open())
    {
        cout.rdbuf(l.out.buf);
        cin.rdbuf(l.in.buf);
        l.file.close();
    }
  }
};

} // namespace

namespace Utility
{
    string myFolder;

    namespace
    {
#if defined(_WIN32) || defined (_WIN64)
        constexpr char DirectorySeparator = '\\';
#else
        constexpr char DirectorySeparator = '/';
#endif
    }

    void init(const char* arg0)
    {
        string s = arg0;
        size_t i = s.find_last_of(DirectorySeparator);
        if (i != string::npos)
            myFolder = s.substr(0, i);
    }

    //Remove double or single quotes from a string
    string unquote(const string& s)
    {
        string s1 = s;

        if (s1.size() > 2)
        {
            if ((s1.front() == '\"' && s1.back() == '\"') || (s1.front() == '\'' && s1.back() == '\''))
            {
                s1 = s1.substr(1, s1.size() - 2);
            }
        }

        return s1;
    }

    //Map relative folder or filename to local directory of the engine executable
    string map_path(const string& path)
    {
        string newPath = path;

        //Make sure we have something to work on
        if (!path.size() || !myFolder.size())
            return path;

        //Make sure we can map this path
        if (newPath.find(DirectorySeparator) == string::npos)
            newPath = myFolder + DirectorySeparator + newPath;

        return newPath;
    }

    bool file_exists(const string& filename)
    {
        struct stat info;
        if (stat(filename.c_str(), &info) == 0)
            return (info.st_mode & S_IFREG) == S_IFREG;

        return false;
    }

    bool is_game_decided(const Position& pos, Value lastScore)
    {
        //Assume game is decided if game ply is above 200
        if (pos.game_ply() > 200)
            return true;

        //Assume game is decided if |last score| is above 2.5 Pawn
        if (lastScore != VALUE_NONE && std::abs(lastScore) > PawnValueEg * 5 / 2)
            return true;

        //Assume game is decided if |last score| is below 0.25 Pawn and game ply is above 120
        if (pos.game_ply() > 120 && lastScore < PawnValueEg / 4)
            return true;

        //Assume game is decided if remaining pieces is less than 9
        if (pos.count<ALL_PIECES>() < 9)
            return true;

        //Assume game is not decided!
        return false;
    }
}

/// engine_info() returns the full name of the current SugaR version. This
/// will be either "SugaR <Tag> DD-MM-YY" (where DD-MM-YY is the date when
/// the program was compiled) or "SugaR <Version>", depending on whether
/// Version is empty.

string engine_info(bool to_uci) {

  const string months("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec");
  string month, day, year;
  stringstream ss, date(__DATE__); // From compiler, format is "Sep 21 2008"

  ss << "SugaR AI 2.10 " << Version << setfill('0');

  ss << (to_uci  ? "\nid author ": " by ")
     << "Stockfish Team, Marco Zerbinati, Khalid Omar";

  if (!to_uci)
  {
      date >> month >> day >> year;

      ss << "\n"
         << compiler_info()
         << "\nBuild date/time    : " << year << '-' << setw(2) << setfill('0') << month << '-' << setw(2) << setfill('0') << day << ' ' << __TIME__
         << "\n";
  }

  return ss.str();
}


/// compiler_info() returns a string trying to describe the compiler we use

std::string compiler_info() {

  #define stringify2(x) #x
  #define stringify(x) stringify2(x)
  #define make_version_string(major, minor, patch) stringify(major) "." stringify(minor) "." stringify(patch)

/// Predefined macros hell:
///
/// __GNUC__           Compiler is gcc, Clang or Intel on Linux
/// __INTEL_COMPILER   Compiler is Intel
/// _MSC_VER           Compiler is MSVC or Intel on Windows
/// _WIN32             Building on Windows (any)
/// _WIN64             Building on Windows 64 bit

  std::string compiler = "\nCompiled by ";

  #ifdef __clang__
     compiler += "clang++ ";
     compiler += make_version_string(__clang_major__, __clang_minor__, __clang_patchlevel__);
  #elif __INTEL_COMPILER
     compiler += "Intel compiler ";
     compiler += "(version ";
     compiler += stringify(__INTEL_COMPILER) " update " stringify(__INTEL_COMPILER_UPDATE);
     compiler += ")";
  #elif _MSC_VER
     compiler += "MSVC ";
     compiler += "(version ";
     compiler += stringify(_MSC_FULL_VER) "." stringify(_MSC_BUILD);
     compiler += ")";
  #elif defined(__e2k__) && defined(__LCC__)
    #define dot_ver2(n) \
      compiler += (char)'.'; \
      compiler += (char)('0' + (n) / 10); \
      compiler += (char)('0' + (n) % 10);

     compiler += "MCST LCC ";
     compiler += "(version ";
     compiler += std::to_string(__LCC__ / 100);
     dot_ver2(__LCC__ % 100)
     dot_ver2(__LCC_MINOR__)
     compiler += ")";
  #elif __GNUC__
     compiler += "g++ (GNUC) ";
     compiler += make_version_string(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
  #else
     compiler += "Unknown compiler ";
     compiler += "(unknown version)";
  #endif

  #if defined(__APPLE__)
     compiler += " on Apple";
  #elif defined(__CYGWIN__)
     compiler += " on Cygwin";
  #elif defined(__MINGW64__)
     compiler += " on MinGW64";
  #elif defined(__MINGW32__)
     compiler += " on MinGW32";
  #elif defined(__ANDROID__)
     compiler += " on Android";
  #elif defined(__linux__)
     compiler += " on Linux";
  #elif defined(_WIN64)
     compiler += " on Microsoft Windows 64-bit";
  #elif defined(_WIN32)
     compiler += " on Microsoft Windows 32-bit";
  #else
     compiler += " on unknown system";
  #endif

  compiler += "\nCompilation settings include: ";
  compiler += (Is64Bit ? " 64bit" : " 32bit");
  #if defined(USE_VNNI)
    compiler += " VNNI";
  #endif
  #if defined(USE_AVX512)
    compiler += " AVX512";
  #endif
  compiler += (HasPext ? " BMI2" : "");
  #if defined(USE_AVX2)
    compiler += " AVX2";
  #endif
  #if defined(USE_SSE41)
    compiler += " SSE41";
  #endif
  #if defined(USE_SSSE3)
    compiler += " SSSE3";
  #endif
  #if defined(USE_SSE2)
    compiler += " SSE2";
  #endif
  compiler += (HasPopCnt ? " POPCNT" : "");
  #if defined(USE_MMX)
    compiler += " MMX";
  #endif
  #if defined(USE_NEON)
    compiler += " NEON";
  #endif

  #if !defined(NDEBUG)
    compiler += " DEBUG";
  #endif

  compiler += "\n__VERSION__ macro expands to: ";
  #ifdef __VERSION__
     compiler += __VERSION__;
  #else
     compiler += "(undefined macro)";
  #endif
  compiler += "\n";

  return compiler;
}

string format_bytes(uint64_t bytes, int decimals)
{
    static const uint64_t _1KB = 1024;
    static const uint64_t _1MB = _1KB * 1024;
    static const uint64_t _1GB = _1MB * 1024;

    std::stringstream ss;

    if (bytes < _1KB)
        ss << bytes << " B";
    else if (bytes < _1MB)
        ss << std::fixed << std::setprecision(decimals) << ((double)bytes / _1KB) << "KB";
    else if (bytes < _1GB)
        ss << std::fixed << std::setprecision(decimals) << ((double)bytes / _1MB) << "MB";
    else
        ss << std::fixed << std::setprecision(decimals) << ((double)bytes / _1GB) << "GB";

    return ss.str();
}

void show_logo()
{
#if defined(_WIN32)
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbiInfo;
    if (hConsole && !GetConsoleScreenBufferInfo(hConsole, &csbiInfo))
        hConsole = nullptr;

    if (hConsole)
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
#elif defined(__linux)
    cout << "\033[1;31m";
#endif

    cout <<
        R"(
  _           _  
 /_`   _  _  /_/ 
 _//_//_//_|/ \_
      __/
	  
)" << endl;

#if defined(_WIN32)
    if (hConsole)
        SetConsoleTextAttribute(hConsole, csbiInfo.wAttributes);
#elif defined(__linux)
    cout << "\033[0m";
#endif
}

namespace SysInfo
{
    namespace
    {
        uint32_t numaNodeCount = 0;
        uint32_t processorCoreCount = 0;
        uint32_t logicalProcessorCount = 0;
        uint32_t processorCacheSize[3] = { 0, 0, 0 };

        uint64_t totalMemory = 0;

        std::string osInfo;
        std::string cpuBrand;

        void init_hw_info()
        {
#if defined(_WIN32)
            typedef BOOL(WINAPI* GLPIEX)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
            static GLPIEX impGetLogicalProcessorInformationEx = (GLPIEX)(void (*)(void))GetProcAddress(GetModuleHandle("kernel32.dll"), "GetLogicalProcessorInformationEx");

            void* oldBuffer = nullptr;
            DWORD len = 0;
            DWORD offset = 0;

            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* bufferEx = nullptr;
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buffer = nullptr;
            GROUP_AFFINITY* nodeGroupMask = nullptr;
            ULONGLONG* nodeMask = nullptr;

            auto release_memory = [&]()
            {
                if (bufferEx) { free(bufferEx);      bufferEx = nullptr; }
                if (buffer) { free(buffer);        buffer = nullptr; }
                if (oldBuffer) { free(oldBuffer);     oldBuffer = nullptr; }
                if (nodeGroupMask) { free(nodeGroupMask); nodeGroupMask = nullptr; }
                if (nodeMask) { free(nodeMask);      nodeMask = nullptr; }
            };

            // Use windows processor groups?
            if (impGetLogicalProcessorInformationEx)
            {
                // Get array of node and core data
                while (true)
                {
                    if (impGetLogicalProcessorInformationEx(RelationAll, bufferEx, &len))
                        break;

                    //Save old buffer in case realloc fails
                    oldBuffer = bufferEx;

                    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
                        bufferEx = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)realloc(bufferEx, len);
                    else
                        bufferEx = nullptr; //Old value already stored in 'oldBuffer'

                    if (!bufferEx)
                    {
                        release_memory();
                        return;
                    }
                }

                //Prepare
                size_t maxNodes = 16;

                //Allocate memory
                nodeGroupMask = (GROUP_AFFINITY*)malloc(maxNodes * sizeof(GROUP_AFFINITY));
                if (!nodeGroupMask)
                {
                    release_memory();
                    return;
                }

                //Numa nodes loop
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* ptr = bufferEx;
                while (offset < len && offset + ptr->Size <= len)
                {
                    if (ptr->Relationship == RelationNumaNode)
                    {
                        if (numaNodeCount == maxNodes)
                        {
                            maxNodes += 16;

                            oldBuffer = nodeGroupMask;
                            nodeGroupMask = (GROUP_AFFINITY*)realloc(nodeGroupMask, maxNodes * sizeof(GROUP_AFFINITY));
                            if (!nodeGroupMask)
                            {
                                release_memory();
                                return;
                            }
                        }

                        nodeGroupMask[numaNodeCount] = ptr->NumaNode.GroupMask;
                        numaNodeCount++;
                    }

                    offset += ptr->Size;
                    ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((char*)ptr) + ptr->Size);
                }

                //Physical/Logical cores loop and cache
                ptr = bufferEx;
                offset = 0;
                while (offset < len && offset + ptr->Size <= len)
                {
                    if (ptr->Relationship == RelationProcessorCore)
                    {
                        // Loop through nodes to find one with matching group number and intersecting mask
                        for (size_t i = 0; i < numaNodeCount; i++)
                        {
                            if (nodeGroupMask[i].Group == ptr->Processor.GroupMask[0].Group && (nodeGroupMask[i].Mask & ptr->Processor.GroupMask[0].Mask))
                            {
                                ++processorCoreCount;
                                logicalProcessorCount += ptr->Processor.Flags == LTP_PC_SMT ? 2 : 1;
                            }
                        }
                    }
                    else if (ptr->Relationship == RelationCache)
                    {
                        switch (ptr->Cache.Level)
                        {
                        case 1:
                        case 2:
                        case 3:
                            processorCacheSize[ptr->Cache.Level - 1] += ptr->Cache.CacheSize;
                            break;

                        default:
                            assert(false);
                            break;
                        }
                    }

                    offset += ptr->Size;
                    ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((char*)ptr) + ptr->Size);
                }
            }
            else // Use windows but not its processor groups
            {
                while (true)
                {
                    if (GetLogicalProcessorInformation(buffer, &len))
                        break;

                    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
                    {
                        //Save old buffer in case realloc fails
                        oldBuffer = buffer;

                        buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)realloc(buffer, len);
                    }
                    else
                    {
                        buffer = nullptr; //Old value already stored in 'oldBuffer'
                    }

                    if (!buffer)
                    {
                        release_memory();
                        return;
                    }
                }

                //Prepare
                size_t maxNodes = 16;

                //Allocate memory
                nodeMask = (ULONGLONG*)malloc(maxNodes * sizeof(ULONGLONG));
                if (!nodeMask)
                {
                    release_memory();
                    return;
                }

                //Numa nodes loop
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION* ptr = buffer;
                while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= len)
                {
                    if (ptr->Relationship == RelationNumaNode)
                    {
                        if (numaNodeCount == maxNodes)
                        {
                            maxNodes += 16;

                            oldBuffer = nodeMask;
                            nodeMask = (ULONGLONG*)realloc(nodeMask, maxNodes * sizeof(ULONGLONG));
                            if (!nodeMask)
                            {
                                release_memory();
                                return;
                            }
                        }

                        nodeMask[numaNodeCount] = ptr->ProcessorMask;
                        numaNodeCount++;
                    }

                    offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                    ptr++;
                }

                //Physical/Logical cores and cache loop
                ptr = buffer;
                offset = 0;
                while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) <= len)
                {
                    if (ptr->Relationship == RelationProcessorCore)
                    {
                        // Loop through nodes to find one with intersecting mask
                        for (size_t i = 0; i < numaNodeCount; i++)
                        {
                            if (nodeMask[i] & ptr->ProcessorMask)
                            {
                                ++processorCoreCount;
                                logicalProcessorCount += ptr->ProcessorCore.Flags == 1 ? 2 : 1;
                            }
                        }
                    }
                    else if (ptr->Relationship == RelationCache)
                    {
                        switch (ptr->Cache.Level)
                        {
                        case 1:
                        case 2:
                        case 3:
                            processorCacheSize[ptr->Cache.Level - 1] += ptr->Cache.Size;
                            break;

                        default:
                            assert(false);
                            break;
                        }
                    }

                    offset += sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                    ptr++;
                }
            }

            release_memory();
#elif defined(__linux__)
            const int max_buffer = 1024;
            char buffer[max_buffer];

            //Run 'lscpu' to get CPU information
            FILE* stream = popen("lscpu 2>&1", "r");
            if (!stream)
                return;

            string cpuData;
            while (!feof(stream))
            {
                if (fgets(buffer, max_buffer, stream) != NULL)
                    cpuData.append(buffer);
            }

            pclose(stream);

            if (cpuData.empty())
                return;

            auto parse_unit = [](const char *s)
            {
                if (strcasecmp(s, "KB") == 0 || strcasecmp(s, "KiB") == 0)
                    return 1024;

                if (strcasecmp(s, "MB") == 0 || strcasecmp(s, "MiB") == 0)
                    return 1024 * 1024;

                if (strcasecmp(s, "GB") == 0 || strcasecmp(s, "GiB") == 0)
                    return 1024 * 1024 * 1024;

                return 0;
            };

            //Find required data
            static regex rgxNumberOfCpus("^CPU\\(s\\):\\s*(\\d*)$");
            static regex rgxThreadsPerCode("^Thread\\(s\\) per core:\\s*(\\d*)$");
            static regex rgxNumaNodes("NUMA node\\(s\\):\\s*(\\d*)$");
            static regex rgxL1dCache("^L1d cache:\\s*(\\d*) (.*)$");
            static regex rgxL1iCache("^L1i cache:\\s*(\\d*) (.*)$");
            static regex rgxL2Cache("^L2 cache:\\s*(\\d*) (.*)$");
            static regex rgxL3Cache("^L3 cache:\\s*(\\d*) (.*)$");
            static regex rgxCpuBrand("^Model name:\\s*(.*)$");

            int tempThreadsPerCode = 0;

            std::stringstream ss(cpuData);
            std::string line;
            while (std::getline(ss, line))
            {
                smatch match;
                if (regex_search(line, match, rgxNumberOfCpus))
                {
                    processorCoreCount = (uint32_t)atoi(match[1].str().c_str());
                }
                else if(regex_search(line, match, rgxThreadsPerCode))
                {
                    tempThreadsPerCode = atoi(match[1].str().c_str());
                }
                else if (regex_search(line, match, rgxL1dCache) || regex_search(line, match, rgxL1iCache))
                {
                    processorCacheSize[0] += (uint32_t)atoi(match[1].str().c_str()) * parse_unit(match[2].str().c_str());
                }
                else if (regex_search(line, match, rgxL2Cache))
                {
                    processorCacheSize[1] += (uint32_t)atoi(match[1].str().c_str()) * parse_unit(match[2].str().c_str());
                }
                else if (regex_search(line, match, rgxL3Cache))
                {
                    processorCacheSize[2] += (uint32_t)atoi(match[1].str().c_str()) * parse_unit(match[2].str().c_str());
                }
                else if (regex_search(line, match, rgxNumaNodes))
                {
                    numaNodeCount = (uint32_t)atoi(match[1].str().c_str());
                }
                else if (regex_search(line, match, rgxCpuBrand))
                {
                    cpuBrand = match[1].str();
                }
            }

            if (processorCoreCount)
            {
                if (tempThreadsPerCode)
                    logicalProcessorCount = processorCoreCount * tempThreadsPerCode;
                else
                    logicalProcessorCount = processorCoreCount;
            }
#endif
        }

        void init_processor_brand()
        {
#if defined(_WIN32)
            HKEY hKey = HKEY_LOCAL_MACHINE;
            TCHAR Data[1024];

            //Clear buffer
            ZeroMemory(Data, sizeof(Data));

            //Open target registry key
            DWORD buffersize = sizeof(Data) / sizeof(Data[0]);
            LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, TEXT("Hardware\\Description\\System\\CentralProcessor\\0\\"), 0, KEY_READ, &hKey);
            if (result == ERROR_SUCCESS)
            {
                //Get value of 'Key = ProcessorNameString' which is the processor name
                result = RegQueryValueEx(hKey, TEXT("ProcessorNameString"), NULL, NULL, (LPBYTE)&Data, &buffersize);

                //If we failed to retrieve the processor name, then we retrun "N/A"
                if (result == ERROR_SUCCESS)
                {
                    cpuBrand = Data;
                }

                // Close the Registry Key
                RegCloseKey(hKey);
            }
#elif defined(__linux__)
            //Nothing to do here since CPU brand is read when init_hw_info() is called
#endif
        }

        void init_os_info()
        {
#if defined(_WIN32)
            {
                InitVersion();

                if (IsWindowsXPOrGreater())
                {
                    if (IsWindowsXPSP1OrGreater())
                    {
                        if (IsWindowsXPSP2OrGreater())
                        {
                            if (IsWindowsXPSP3OrGreater())
                            {
                                if (IsWindowsVistaOrGreater())
                                {
                                    if (IsWindowsVistaSP1OrGreater())
                                    {
                                        if (IsWindowsVistaSP2OrGreater())
                                        {
                                            if (IsWindows7OrGreater())
                                            {
                                                if (IsWindows7SP1OrGreater())
                                                {
                                                    if (IsWindows8OrGreater())
                                                    {
                                                        if (IsWindows8Point1OrGreater())
                                                        {
                                                            if (IsWindows10OrGreater())
                                                            {
                                                                osInfo = "Windows 10";
                                                            }
                                                            else
                                                            {
                                                                osInfo = "Windows 8.1";
                                                            }
                                                        }
                                                        else
                                                        {
                                                            osInfo = "Windows 8";
                                                        }
                                                    }
                                                    else
                                                    {
                                                        osInfo = "Windows 7 SP1";
                                                    }
                                                }
                                                else
                                                {
                                                    osInfo = "Windows 7";
                                                }
                                            }
                                            else
                                            {
                                                osInfo = "Vista SP2";
                                            }
                                        }
                                        else
                                        {
                                            osInfo = "Vista SP1";
                                        }
                                    }
                                    else
                                    {
                                        osInfo = "Vista";
                                    }
                                }
                                else
                                {
                                    osInfo = "XP SP3";
                                }
                            }
                            else
                            {
                                osInfo = "XP SP2";
                            }
                        }
                        else
                        {
                            osInfo = "XP SP1";
                        }
                    }
                    else
                    {
                        osInfo = "XP";
                    }
                }

                if (IsWindowsServer())
                {
                    osInfo += " Server";
                }
                else
                {
                    osInfo += " Client";
                }

                osInfo += " Or Greater";
            }
#elif defined(__linux__)
            ifstream distribInfp("/etc/lsb-release");
            if (!distribInfp.is_open())
                return;

            static regex rgxDistributionID("^DISTRIB_ID=(.*)$");
            static regex rgxDistribRelease("^DISTRIB_RELEASE=(.*)$");
            static regex rgxDistribDescription("^DISTRIB_DESCRIPTION=\"(.*)\"$");

            std::string distribID;
            std::string distribRelease;
            std::string distribDescription;

            std::string line;
            while (std::getline(distribInfp, line))
            {
                smatch match;
                if (regex_search(line, match, rgxDistributionID))
                {
                    distribID = match[1].str();
                }
                else if (regex_search(line, match, rgxDistribRelease))
                {
                    distribRelease = match[1].str();
                }
                else if (regex_search(line, match, rgxDistribDescription))
                {
                    distribDescription = match[1].str();

                    //If we have the distrib description then we are good to go
                    break;
                }
            }

            if (!distribDescription.empty())
                osInfo = distribDescription;
            else if (!distribID.empty() && !distribRelease.empty())
                osInfo = distribID + " " + distribRelease;
#endif
        }

        void init_mem_info()
        {
#if defined(_WIN32)
            ULONGLONG totMem;
            if (GetPhysicallyInstalledSystemMemory(&totMem))
            {
                //Returned value is in KB
                totalMemory = totMem * 1024;
            }
            else
            {
                MEMORYSTATUSEX statex;
                statex.dwLength = sizeof(statex);

                if (GlobalMemoryStatusEx(&statex))
                    totalMemory = statex.ullTotalPhys;
                else
                    totalMemory = 0;
            }
#elif defined(__linux__)
            ifstream memInfo("/proc/meminfo");
            if (!memInfo.is_open())
                return;

            static regex rgxMemTotal("^MemTotal:\\s*(\\d*) (.*)$$");

            std::string line;
            while (std::getline(memInfo, line))
            {
                smatch match;
                if (regex_search(line, match, rgxMemTotal))
                {
                    totalMemory = strtoull(match[1].str().c_str(), nullptr, 10);
                    if (strcasecmp(match[2].str().c_str(), "KB") == 0 || strcasecmp(match[2].str().c_str(), "KiB") == 0)
                        totalMemory *= 1024;
                    else if (strcasecmp(match[2].str().c_str(), "MB") == 0 || strcasecmp(match[2].str().c_str(), "MiB") == 0)
                        totalMemory *= 1024 * 1024;
                    else if (strcasecmp(match[2].str().c_str(), "GB") == 0 || strcasecmp(match[2].str().c_str(), "GiB") == 0)
                        totalMemory *= 1024 * 1024 * 1024;

                    //We found what we are looking for
                    break;
                }
            }
#endif
        }
    }

    void init()
    {
        init_hw_info();
        init_processor_brand();
        init_os_info();
        init_mem_info();
    }

    const string numa_nodes()
    {
        if (!numaNodeCount)
            return "N/A";

        return to_string(numaNodeCount);
    }

    const string physical_cores()
    {
        if (!processorCoreCount)
            return "N/A";

        return to_string(processorCoreCount);
    }

    const string logical_cores()
    {
        if (!logicalProcessorCount)
            return "N/A";

        return to_string(logicalProcessorCount);
    }

    const string is_hyper_threading()
    {
        if (!logicalProcessorCount || !processorCoreCount)
            return "N/A";

        return processorCoreCount == logicalProcessorCount ? "No" : "Yes";
    }

    const string cache_info(int idx)
    {
        if (!processorCacheSize[idx])
            return "N/A";

        return format_bytes(processorCacheSize[idx], 0);
    }

    const string os_info()
    {
        if (osInfo.empty())
            return "N/A";

        return osInfo;
    }

    const string processor_brand()
    {
        if (cpuBrand.empty())
            return "N/A";

        return cpuBrand;
    }

    const string total_memory()
    {
        if (totalMemory == 0)
            return "N/A";

        return format_bytes(totalMemory, 0);
    }
}

/// Debug functions used mainly to collect run-time statistics
static std::atomic<int64_t> hits[2], means[2];

void dbg_hit_on(bool b) { ++hits[0]; if (b) ++hits[1]; }
void dbg_hit_on(bool c, bool b) { if (c) dbg_hit_on(b); }
void dbg_mean_of(int v) { ++means[0]; means[1] += v; }

void dbg_print() {

  if (hits[0])
      cerr << "Total " << hits[0] << " Hits " << hits[1]
           << " hit rate (%) " << 100 * hits[1] / hits[0] << endl;

  if (means[0])
      cerr << "Total " << means[0] << " Mean "
           << (double)means[1] / means[0] << endl;
}


/// Used to serialize access to std::cout to avoid multiple threads writing at
/// the same time.

std::ostream& operator<<(std::ostream& os, SyncCout sc) {

  static std::mutex m;

  if (sc == IO_LOCK)
      m.lock();

  if (sc == IO_UNLOCK)
      m.unlock();

  return os;
}


/// Trampoline helper to avoid moving Logger to misc.h
void start_logger(const std::string& fname) { Logger::start(fname); }


/// prefetch() preloads the given address in L1/L2 cache. This is a non-blocking
/// function that doesn't stall the CPU waiting for data to be loaded from memory,
/// which can be quite slow.
#ifdef NO_PREFETCH

void prefetch(void*) {}

#else

void prefetch(void* addr) {

#  if defined(__INTEL_COMPILER)
   // This hack prevents prefetches from being optimized away by
   // Intel compiler. Both MSVC and gcc seem not be affected by this.
   __asm__ ("");
#  endif

#  if defined(__INTEL_COMPILER) || defined(_MSC_VER)
  _mm_prefetch((char*)addr, _MM_HINT_T0);
#  else
  __builtin_prefetch(addr);
#  endif
}

#endif


/// std_aligned_alloc() is our wrapper for systems where the c++17 implementation
/// does not guarantee the availability of aligned_alloc(). Memory allocated with
/// std_aligned_alloc() must be freed with std_aligned_free().

void* std_aligned_alloc(size_t alignment, size_t size) {

#if defined(POSIXALIGNEDALLOC)
  void *mem;
  return posix_memalign(&mem, alignment, size) ? nullptr : mem;
#elif defined(_WIN32)
  return _mm_malloc(size, alignment);
#else
  return std::aligned_alloc(alignment, size);
#endif
}

void std_aligned_free(void* ptr) {

#if defined(POSIXALIGNEDALLOC)
  free(ptr);
#elif defined(_WIN32)
  _mm_free(ptr);
#else
  free(ptr);
#endif
}

/// aligned_large_pages_alloc() will return suitably aligned memory, if possible using large pages.

#if defined(_WIN32)

static void* aligned_large_pages_alloc_windows(size_t allocSize) {

  #if !defined(_WIN64)
    return nullptr;
  #else

  HANDLE hProcessToken { };
  LUID luid { };
  void* mem = nullptr;

  const size_t largePageSize = GetLargePageMinimum();
  if (!largePageSize)
      return nullptr;

  // We need SeLockMemoryPrivilege, so try to enable it for the process
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hProcessToken))
      return nullptr;

  if (LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &luid))
  {
      TOKEN_PRIVILEGES tp { };
      TOKEN_PRIVILEGES prevTp { };
      DWORD prevTpLen = 0;

      tp.PrivilegeCount = 1;
      tp.Privileges[0].Luid = luid;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

      // Try to enable SeLockMemoryPrivilege. Note that even if AdjustTokenPrivileges() succeeds,
      // we still need to query GetLastError() to ensure that the privileges were actually obtained.
      if (AdjustTokenPrivileges(
              hProcessToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), &prevTp, &prevTpLen) &&
          GetLastError() == ERROR_SUCCESS)
      {
          // Round up size to full pages and allocate
          allocSize = (allocSize + largePageSize - 1) & ~size_t(largePageSize - 1);
          mem = VirtualAlloc(
              NULL, allocSize, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);

          // Privilege no longer needed, restore previous state
          AdjustTokenPrivileges(hProcessToken, FALSE, &prevTp, 0, NULL, NULL);
      }
  }

  CloseHandle(hProcessToken);

  return mem;

  #endif
}

void* aligned_large_pages_alloc(size_t allocSize) {

  // Try to allocate large pages
  void* mem = aligned_large_pages_alloc_windows(allocSize);

  // Fall back to regular, page aligned, allocation if necessary
  if (!mem)
      mem = VirtualAlloc(NULL, allocSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

  return mem;
}

#else

void* aligned_large_pages_alloc(size_t allocSize) {

#if defined(__linux__)
  constexpr size_t alignment = 2 * 1024 * 1024; // assumed 2MB page size
#else
  constexpr size_t alignment = 4096; // assumed small page size
#endif

  // round up to multiples of alignment
  size_t size = ((allocSize + alignment - 1) / alignment) * alignment;
  void *mem = std_aligned_alloc(alignment, size);
#if defined(MADV_HUGEPAGE)
  madvise(mem, size, MADV_HUGEPAGE);
#endif
  return mem;
}

#endif


/// aligned_large_pages_free() will free the previously allocated ttmem

#if defined(_WIN32)

void aligned_large_pages_free(void* mem) {

  if (mem && !VirtualFree(mem, 0, MEM_RELEASE))
  {
      DWORD err = GetLastError();
      std::cerr << "Failed to free large page memory. Error code: 0x"
                << std::hex << err
                << std::dec << std::endl;
      exit(EXIT_FAILURE);
  }
}

#else

void aligned_large_pages_free(void *mem) {
  std_aligned_free(mem);
}

#endif


namespace WinProcGroup {

#ifndef _WIN32

void bindThisThread(size_t) {}

#else

/// best_group() retrieves logical processor information using Windows specific
/// API and returns the best group id for the thread with index idx. Original
/// code from Texel by Peter Österlund.

int best_group(size_t idx) {

  int threads = 0;
  int nodes = 0;
  int cores = 0;
  DWORD returnLength = 0;
  DWORD byteOffset = 0;

  // Early exit if the needed API is not available at runtime
  HMODULE k32 = GetModuleHandle("Kernel32.dll");
  auto fun1 = (fun1_t)(void(*)())GetProcAddress(k32, "GetLogicalProcessorInformationEx");
  if (!fun1)
      return -1;

  // First call to get returnLength. We expect it to fail due to null buffer
  if (fun1(RelationAll, nullptr, &returnLength))
      return -1;

  // Once we know returnLength, allocate the buffer
  SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buffer, *ptr;
  ptr = buffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(returnLength);

  // Second call, now we expect to succeed
  if (!fun1(RelationAll, buffer, &returnLength))
  {
      free(buffer);
      return -1;
  }

  while (byteOffset < returnLength)
  {
      if (ptr->Relationship == RelationNumaNode)
          nodes++;

      else if (ptr->Relationship == RelationProcessorCore)
      {
          cores++;
          threads += (ptr->Processor.Flags == LTP_PC_SMT) ? 2 : 1;
      }

      assert(ptr->Size);
      byteOffset += ptr->Size;
      ptr = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(((char*)ptr) + ptr->Size);
  }

  free(buffer);

  std::vector<int> groups;

  // Run as many threads as possible on the same node until core limit is
  // reached, then move on filling the next node.
  for (int n = 0; n < nodes; n++)
      for (int i = 0; i < cores / nodes; i++)
          groups.push_back(n);

  // In case a core has more than one logical processor (we assume 2) and we
  // have still threads to allocate, then spread them evenly across available
  // nodes.
  for (int t = 0; t < threads - cores; t++)
      groups.push_back(t % nodes);

  // If we still have more threads than the total number of logical processors
  // then return -1 and let the OS to decide what to do.
  return idx < groups.size() ? groups[idx] : -1;
}


/// bindThisThread() set the group affinity of the current thread

void bindThisThread(size_t idx) {

  // Use only local variables to be thread-safe
  int group = best_group(idx);

  if (group == -1)
      return;

  // Early exit if the needed API are not available at runtime
  HMODULE k32 = GetModuleHandle("Kernel32.dll");
  auto fun2 = (fun2_t)(void(*)())GetProcAddress(k32, "GetNumaNodeProcessorMaskEx");
  auto fun3 = (fun3_t)(void(*)())GetProcAddress(k32, "SetThreadGroupAffinity");

  if (!fun2 || !fun3)
      return;

  GROUP_AFFINITY affinity;
  if (fun2(group, &affinity))
  {
	  sync_cout << "info string Binding thread " << idx << " to group " << group << sync_endl;
      fun3(GetCurrentThread(), &affinity, nullptr);
  }
}

#endif

} // namespace WinProcGroup

#ifdef _WIN32
#include <direct.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#define GETCWD getcwd
#endif

namespace CommandLine {

string argv0;            // path+name of the executable binary, as given by argv[0]
string binaryDirectory;  // path of the executable directory
string workingDirectory; // path of the working directory

void init(int argc, char* argv[]) {
    (void)argc;
    string pathSeparator;

    // extract the path+name of the executable binary
    argv0 = argv[0];

#ifdef _WIN32
    pathSeparator = "\\";
  #ifdef _MSC_VER
    // Under windows argv[0] may not have the extension. Also _get_pgmptr() had
    // issues in some windows 10 versions, so check returned values carefully.
    char* pgmptr = nullptr;
    if (!_get_pgmptr(&pgmptr) && pgmptr != nullptr && *pgmptr)
        argv0 = pgmptr;
  #endif
#else
    pathSeparator = "/";
#endif

    // extract the working directory
    workingDirectory = "";
    char buff[40000];
    char* cwd = GETCWD(buff, 40000);
    if (cwd)
        workingDirectory = cwd;

    // extract the binary directory path from argv0
    binaryDirectory = argv0;
    size_t pos = binaryDirectory.find_last_of("\\/");
    if (pos == std::string::npos)
        binaryDirectory = "." + pathSeparator;
    else
        binaryDirectory.resize(pos + 1);

    // pattern replacement: "./" at the start of path is replaced by the working directory
    if (binaryDirectory.find("." + pathSeparator) == 0)
        binaryDirectory.replace(0, 1, workingDirectory);
}


} // namespace CommandLine

} // namespace Stockfish
