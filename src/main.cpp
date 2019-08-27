// duplex - Interactively delete duplicate files

#pragma clang diagnostic push
#pragma ide diagnostic ignored "InfiniteRecursion"
//#pragma ide diagnostic ignored "modernize-pass-by-value"
//#pragma clang diagnostic ignored "-Wshadow"

#include "pch.h"

#include "fnv_1a_64.h"
#include "junction.h"

#include "md5.hpp"

#ifndef WIN32
using namespace __gnu_cxx;
#else
using namespace stdext;
#endif

using namespace boost;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

// Command line args
std::vector<fs::path> PATH_VEC_ARG;
std::vector<fs::path> RECURSIVE_PATH_VEC_ARG;
std::vector<fs::path> MD5_PATH_VEC_ARG;
std::vector<std::string> RULE_VEC_ARG;
bool AUTOMATIC_ARG(false);
bool VERBOSE_ARG(false);
bool QUIET_ARG(false);
bool DEBUG_ARG(false);
bool USE_MD5_ARG(false);
bool DRY_RUN_ARG(false);
size_t IGNORE_SMALLER_ARG((size_t)-1), IGNORE_LARGER_ARG((size_t)-1);

typedef std::string Hash;

// Compare functions for regular sort operations do not need any context beyond the value pairs that
// the sort function passes in. This class is created before the sort and stores GroupMap context,
// allowing HashToGroupMap by be sorted by FileInfo in the GroupMap.
template <typename GroupMap> class CompareGroupsByHash {
public:
  explicit CompareGroupsByHash(GroupMap groupMap) : groupMap(std::move(groupMap))
  {
  }

  // All files in a group share the same size. If the files in the two groups have different sizes,
  // use that for sorting. If they're the same, fall back to comparing the first path in each
  // FileVec as a tie breaker. The FileVecs for each group should themselves be sorted first.
  bool operator()(const Hash &a, const Hash &b) const
  {
    auto groupA = groupMap.at(a);
    auto groupB = groupMap.at(b);
    if (groupA[0].size == groupB[0].size) {
      return groupA[0].path > groupB[0].path;
    }
    return groupA[0].size > groupB[0].size;
  }

private:
  GroupMap groupMap;
};

// Hold one file entry.
class FileInfo {
public:
  FileInfo(fs::path path, size_t size, std::string hash)
    : path(std::move(path)), size(size), hash(std::move(hash))
  {
  }

  FileInfo(fs::path path, size_t size) : path(std::move(path)), size(size)
  {
  }

  std::string str()
  {
    return hash.empty() ? fmt::format("{:>14n} {}", size, path.native())
                        : fmt::format("{:>14n} {} {}", size, std::string(hash), path.native());
  }

  fs::path path;
  size_t size;
  std::string hash{""};
};

typedef std::vector<FileInfo> FileVec;

// Vector of files in one group. Can't be a sequence as iterators into the vector remaining valid
// even when items are removed.
typedef std::vector<FileInfo> Group;

// Map that files are initially put into when they're found. It both orders and groups files by
// file_size.
typedef std::map<size_t, Group> SizeToGroupMap;

// Hash of vectors of files is the map that files are put into after files with unique file lengths
// are eliminated. It groups files by hash.
typedef std::unordered_map<Hash, Group> HashToGroupMap;

// Vec that keeps the order that the group_maps were created in. Because file_map is a map sorted by
// file_size, group_walker is also sorted. This enables us to move back and forth in groups based on
// file size, and to put the group with the largest files first.
typedef std::vector<Hash> GroupsBySizeVec;

class Rules {
public:
  void addRegexRule(const std::string &arg)
  {
    assertNotEmpty(arg);
    assertNotExists(regexStrVec, arg);
    try {
      regexVec.emplace_back(arg, regbase::perl | regbase::icase);
    }
    catch (std::exception &e) {
      throw std::runtime_error(fmt::format("Invalid regular expression: {}"));
    }
    regexStrVec.push_back(arg);
    pathVec.emplace_back();
  }

  void addPathRule(const fs::path &arg)
  {
    assertNotEmpty(arg);
    assertNotExists(pathVec, arg);
    regexVec.emplace_back();
    regexStrVec.emplace_back("");
    pathVec.push_back(arg);
  }

  void eraseRule(size_t idx)
  {
    regexVec.erase(regexVec.begin() + idx);
    regexStrVec.erase(regexStrVec.begin() + idx);
    pathVec.erase(pathVec.begin() + idx);
  }

  void clear() {
    regexVec.clear();
    regexStrVec.clear();
    pathVec.clear();
  }

  // Determine if fileInfo matches any of the current rules.
  [[nodiscard]] bool isMatch(const FileInfo &fileInfo) const
  {
    size_t idx = 0;
    for (auto &rule : pathVec) {
      if (!rule.empty()) {
        if (fileInfo.path.native() == rule.native()) {
          return true;
        }
      }
      else {
        if (regex_search(fileInfo.path.native(), regexVec[idx])) {
          return true;
        }
      }
      ++idx;
    }
    return false;
  }

  [[nodiscard]] std::vector<std::string> getRulesForDisplay() const
  {
    std::vector<std::string> r;
    size_t idx = 0;
    for (auto &p : pathVec) {
      if (!p.empty()) {
        r.push_back(p.native());
      }
      else {
        r.push_back(regexStrVec[idx]);
      }
      ++idx;
    }
    return r;
  }

  size_t getRuleCount()
  {
    return pathVec.size();
  }

  template <typename T, typename X> void assertNotExists(const T &v, const X &arg)
  {
    for (auto &a : v) {
      if (a == arg) {
        throw std::runtime_error(fmt::format("Rule already exists: {}", arg));
      }
    }
  }

  template <typename T> void assertNotEmpty(T arg)
  {
    if (arg.empty()) {
      throw std::runtime_error(fmt::format("Missing rule argument"));
    }
  }

private:
  std::vector<regex> regexVec;
  std::vector<std::string> regexStrVec;
  std::vector<fs::path> pathVec;
};

timer LAST_STATUS_TIME;

class Stats {
public:
  Stats()
    : totalCount(0), dupCount(0), markedCount(0), totalBytes(0), dupBytes(0), markedBytes(0),
      groupCount(0)
  {
  }

  void operator+=(const Stats &other)
  {
    totalCount += other.totalCount;
    dupCount += other.dupCount;
    markedCount += other.markedCount;
    totalBytes += other.totalBytes;
    dupBytes += other.dupBytes;
    markedBytes += other.markedBytes;
    groupCount += other.groupCount;
  }

  size_t totalCount;
  size_t dupCount;
  size_t markedCount;
  size_t totalBytes;
  size_t dupBytes;
  size_t markedBytes;
  size_t groupCount;
};

void verifyDirPaths();
bool isInvalidDirPath(const fs::path &p);
FileVec findAllFiles();
void addPath(FileVec &fileVec, const fs::path &path, const bool &recursive);
void addMd5File(FileVec &fileVec, const fs::path &md5DeepPath);
void addFile(FileVec &fileVec, const fs::path &filePath);
void displayFindStatus(const FileVec &fileVec, bool forceDisplay = false);
// Group files by size and remove single item groups (files with unique sizes can't have dups).
SizeToGroupMap groupFilesBySize(const FileVec &fileVec);
template <typename GroupMap> void removeSingleItemGroups(GroupMap &groupMap);
// Hash all remaining files, as they may have dups.
void hashAll(FileVec &fileVec);
void calculateHash(FileInfo &fileInfo);
void displayHashStatus(const FileInfo &fileInfo, size_t accumulatedSize, size_t totalSize,
  size_t fileCount, size_t fileIdx);
size_t getTotalSizeOfUnhashed(FileVec &fileVec);
// Group files again, this time by hash, and again remove single item groups.
HashToGroupMap groupFilesByHash(const FileVec &fileVec);
// Rules.
void addRulesFromCommandLine(Rules &rules);
// Add rules interactively.
void sortAllFileInfoVec(HashToGroupMap &hashToGroupMap);
GroupsBySizeVec sortGroupsBySize(const HashToGroupMap &hashToGroupMap);
void addRulesInteractive(Rules &rules, HashToGroupMap &groupMap);
bool isInt(const std::string &cmd);
size_t argToIdx(const std::string &arg, const size_t &maxIdx);
void refreshGroups(GroupsBySizeVec &groupsBySize, HashToGroupMap &groupMap);
void commandPrompt(std::string &cmd, std::string &arg, size_t groupIdx, size_t groupCount);
void displayRules(const Rules &rules);
void displayGroup(const FileVec &fileVec, const Rules &rules, size_t groupIdx, size_t groupCount);
void displayHelp();
void displayTotalStats(const Stats &stats);
// Delete files marked by the rules.
bool confirmDeletePrompt(const Stats &totalStats);
void deleteMarkedFiles(HashToGroupMap &groupMap, const Rules &rules);
bool deleteFile(const FileInfo &fileInfo);
void displayDeleteStatus(const Stats &totalStats, size_t deleteIdx, size_t deletedCount);
// Misc.
Stats getGroupStats(const FileVec &fileVec, const Rules &rules);
Stats getTotalStats(const HashToGroupMap &groupMap, const Rules &rules);
// Locale and command line.
void setupLocale();
void parseCommandLine(int argc, char **argv);
void procCommand(size_t &groupIdx, bool &doDisplayHelp, Rules &rules, const Stats &totalStats,
  const FileVec &groupFileVec, const std::string &cmd, const std::string &arg, HashToGroupMap &groupMap);

// Print only when called with --verbose.
// TODO: Replace with logging.
template <typename... Args> void print_verbose(const char *fmt, Args &&... args)
{
  if (VERBOSE_ARG) {
    fmt::print(fmt, std::forward<Args>(args)...);
  }
}

// Print only when not called with --quiet
// TODO: Replace with logging.
template <typename... Args> void print_quiet(const char *fmt, Args &&... args)
{
  if (!QUIET_ARG) {
    fmt::print(fmt, std::forward<Args>(args)...);
  }
}

int main(int argc, char *argv[])
{
  setupLocale();
  parseCommandLine(argc, argv);
  verifyDirPaths();
  auto fileVec = findAllFiles();
  auto sizeToGroupMap = groupFilesBySize(fileVec);
  removeSingleItemGroups(sizeToGroupMap);
  hashAll(fileVec);
  auto hashToGroupMap = groupFilesByHash(fileVec);
  sortAllFileInfoVec(hashToGroupMap);
  // Vec of marking rules.
  Rules rules;
  addRulesFromCommandLine(rules);
  // Set up rules for selecting files to delete.
  if (!AUTOMATIC_ARG) {
    addRulesInteractive(rules, hashToGroupMap);
  }
  else {
    deleteMarkedFiles(hashToGroupMap, rules);
  }
  // Show final stats after deletes.
  auto totalStats = getTotalStats(hashToGroupMap, rules);
  displayTotalStats(totalStats);
  // Success.
  exit(0);
}

// Verify that all provided folder names have legal syntax and exist.
void verifyDirPaths()
{
  bool isInvalid = false;
  for (auto &p : PATH_VEC_ARG) {
    isInvalid |= isInvalidDirPath(p);
  }
  for (auto &p : RECURSIVE_PATH_VEC_ARG) {
    isInvalid |= isInvalidDirPath(p);
  }
  for (auto &p : MD5_PATH_VEC_ARG) {
    isInvalid |= isInvalidDirPath(p);
  }
  if (isInvalid) {
    exit(1);
  }
}

bool isInvalidDirPath(const fs::path &p)
{
  if (!is_directory(p)) {
    fmt::print("Invalid path: {}\n", p.native());
    return true;
  }
  return false;
}

// Create vector of files to check for duplicates.
FileVec findAllFiles()
{
  FileVec fileVec;
  // Add all files in search folders, non-recursive.
  for (auto &p : PATH_VEC_ARG) {
    print_verbose("\nProcessing non-recursive: {}\n", p.native());
    addPath(fileVec, canonical(p), false);
  }
  // Add all files in search folders, recursive.
  for (auto &p : RECURSIVE_PATH_VEC_ARG) {
    print_verbose("\nProcessing recursive: {}\n", p.native());
    addPath(fileVec, canonical(p), true);
  }
  // Add all files provided as md5 vectors.
  for (auto &p : MD5_PATH_VEC_ARG) {
    print_verbose("\nProcessing MD5 file: {}\n", p.native());
    addMd5File(fileVec, p);
  }
  displayFindStatus(fileVec, true);
  return fileVec;
}

// Add a file or directory path.
void addPath(FileVec &fileVec, const fs::path &path, const bool &recursive)
{
  try {
    // Do not follow symlinks and junctions.
    if ((!is_regular_file(path) && !is_directory(path)) || isJunction(path)) {
      print_verbose("Ignored special file: {}\n", path.native());
      return;
    }
    if (is_regular_file(path)) {
      addFile(fileVec, path);
    }
    else {
      print_verbose("Entering dir: {}\n", path.native());
      for (const auto &subPath : fs::directory_iterator(path)) {
        addPath(fileVec, subPath, recursive);
      }
    }
  }
  catch (const std::exception &e) {
    fmt::print("\nIgnored file: {}\n", path.native());
    print_verbose("Cause: {}\n", e.what());
  }
}

// Add a vector of files generated with md5deep or similar. File format is one size, MD5 and full
// path per line. Example: 43912  ccd6dad4b72d1255cf2e7a9dadd64083  C:\Documents and
// Settings\Administrator\Desktop\test.txt
void addMd5File(FileVec &fileVec, const fs::path &md5DeepPath)
{
  regex md5Line(" *([0-9]+) +([0-9a-fA-F]{32}) +(.*)");
  boost::smatch what;
  size_t fileCount(0);
  // Open file for reading.
  std::wifstream fi(md5DeepPath.native().c_str());
  if (!fi.good()) {
    fmt::print("\nError: Couldn't open file: {}\n", md5DeepPath);
    return;
  }

  while (fi.good()) {
    fileCount++;
    std::string line;
    // getline(fi, line); TODO
    trim(line);
    if (line.empty()) {
      continue;
    }
    // Parse line into size, md5 and path.
    if (!regex_search(line, what, md5Line)) {
      fmt::print("\nError: Malformed line in md5 file: {}\n", md5DeepPath);
      fmt::print("Line: {}\n", line);
      continue;
    }
    std::string sizeStr(what[1].first, what[1].second);
    std::string md5(what[2].first, what[2].second);
    std::string filePath(what[3].first, what[3].second);
    addFile(fileVec, filePath);
  }
}

void addFile(FileVec &fileVec, const fs::path &filePath)
{
  auto absFilePath = canonical(filePath);
  auto fileSize = filesystem::file_size(absFilePath);
  // Filter small files if requested.
  if (IGNORE_SMALLER_ARG != (size_t)-1 && fileSize <= IGNORE_SMALLER_ARG) {
    print_verbose(
      "Ignored small file (< {:n}): {:n} {}\n", IGNORE_SMALLER_ARG, fileSize, absFilePath.native());
    return;
  }
  // Filter large files if requested.
  if (IGNORE_LARGER_ARG != (size_t)-1 && fileSize >= IGNORE_LARGER_ARG) {
    print_verbose("Ignored large file (> {:n}): {:n} {}\n", IGNORE_LARGER_ARG, fileSize,
      absFilePath.native(), absFilePath.native());
    return;
  }
  // Add file.
  auto fileInfo = FileInfo(absFilePath, fileSize, "");
  fileVec.push_back(fileInfo);
  print_verbose("Found: {}\n", fileInfo.str());
  displayFindStatus(fileVec);
}

// Display status if more than one second has elapsed.
void displayFindStatus(const FileVec &fileVec, bool forceDisplay)
{
  if (forceDisplay || (!QUIET_ARG && LAST_STATUS_TIME.elapsed() >= 1.0)) {
    print_quiet("\nFiles found: {:n}\n", fileVec.size());
    LAST_STATUS_TIME.restart();
  }
  fmt::print("\n");
}

SizeToGroupMap groupFilesBySize(const FileVec &fileVec)
{
  SizeToGroupMap sizeToGroupMap;
  for (auto &fileInfo : fileVec) {
    sizeToGroupMap[fileInfo.size].push_back(fileInfo);
  }
  return sizeToGroupMap;
}

// Remove groups with only one item. These file in the group has unique file size or hash so cannot
// have duplicates.
template <typename GroupMap> void removeSingleItemGroups(GroupMap &groupMap)
{
  size_t removedCount = 0;
  auto iter = groupMap.begin();
  while (iter != groupMap.end()) {
    if (iter->second.size() <= 1) {
      iter = groupMap.erase(iter);
      ++removedCount;
    }
    else {
      ++iter;
    }
  }
  if (removedCount) {
    print_quiet("\nFiltered out {:n} single item or empty groups\n", removedCount);
  }
}

// Calculate hashes for all files.
void hashAll(FileVec &fileVec)
{
  auto totalSizeOfUnhashed = getTotalSizeOfUnhashed(fileVec);
  size_t accumulatedSize(0);
  for (auto &fileInfo : fileVec) {
    accumulatedSize += fileInfo.size;
    try {
      calculateHash(fileInfo);
    }
    catch (std::exception &e) {
      fmt::print("\nIgnored file: {}\n", fileInfo.path.native());
      print_verbose("Cause: {}\n", e.what());
    }
    displayHashStatus(
      fileInfo, accumulatedSize, totalSizeOfUnhashed, fileVec.size(), fileVec.size());
  }
}

void calculateHash(FileInfo &fileInfo)
{
  if (!fileInfo.hash.empty()) {
    return;
  }
  if (USE_MD5_ARG) {
    std::ifstream f(fileInfo.path.native(), std::ios::binary);
    std::string ck(md5(f).digest().hex_str_value());
    std::string wck(ck.begin(), ck.end());
    fileInfo.hash = wck;
  }
  else {
    fileInfo.hash = fnv1A64(fileInfo.path);
  }
  print_verbose("{}: {}\n", USE_MD5_ARG ? "MD5 " : "FNV64", fileInfo.str());
}

void displayHashStatus(const FileInfo &fileInfo, size_t accumulatedSize, size_t totalSize,
  size_t fileCount, size_t fileIdx)
{
  if (!QUIET_ARG && totalSize &&
    (LAST_STATUS_TIME.elapsed() >= 1.0 || accumulatedSize == totalSize)) {
    print_quiet("\nCalculating {} hashes:\n", USE_MD5_ARG ? "MD5" : "FNV64");
    print_quiet("Data: {:.2f}% ({:n} / {:n} bytes)\n",
      (float)accumulatedSize / (float)totalSize * 100, accumulatedSize, totalSize);
    print_quiet("Files: {:.2f}% ({:n} / {:n} files)\n", (float)fileIdx / (float)fileCount * 100,
      fileIdx, fileCount);
    LAST_STATUS_TIME.restart();
    print_quiet("\n");
  }
}

// Sum up the total size of files to hash. The vector may include entries imported from md5 files,
// which includes hash.
size_t getTotalSizeOfUnhashed(FileVec &fileVec)
{
  size_t totalSize(0);
  for (const auto &fileInfo : fileVec) {
    if (fileInfo.hash.empty()) {
      totalSize += fileInfo.size;
    }
  }
  return totalSize;
}

HashToGroupMap groupFilesByHash(const FileVec &fileVec)
{
  HashToGroupMap hashToGroupMap;
  for (const auto &fileInfo : fileVec) {
    hashToGroupMap[fileInfo.hash].push_back(fileInfo);
  }
  return hashToGroupMap;
}

void addRulesFromCommandLine(Rules &rules)
{
  for (auto &ruleArg : RULE_VEC_ARG) {
    rules.addRegexRule(ruleArg);
  }
}

// Sort the FileVec in each group by the paths.
void sortAllFileInfoVec(HashToGroupMap &hashToGroupMap)
{
  for (auto &groupPair : hashToGroupMap) {
    std::sort(std::begin(groupPair.second), std::end(groupPair.second),
      [](const FileInfo &a, const FileInfo &b) { return a.path < b.path; });
  }
}

GroupsBySizeVec sortGroupsBySize(const HashToGroupMap &hashToGroupMap)
{
  GroupsBySizeVec groupsBySize;
  for (auto &groupPair : hashToGroupMap) {
    groupsBySize.push_back(groupPair.first);
  }

  std::sort(std::begin(groupsBySize), std::end(groupsBySize), CompareGroupsByHash(hashToGroupMap));
  return groupsBySize;
}

// Start interactive section.
void addRulesInteractive(Rules &rules, HashToGroupMap &groupMap)
{
  bool doDisplayHelp = true;
  size_t groupIdx = 0;
  GroupsBySizeVec groupsBySize;
  std::string errorMsg;
  fmt::print("\n");

  for (;;) {
    refreshGroups(groupsBySize, groupMap);
    // Exit if no more groups.
    if (groupMap.empty()) {
      if (!QUIET_ARG) {
        print_quiet("\nNo more duplicates found\n");
      }
      return;
    }
    const auto &groupFileVec = groupMap[groupsBySize[groupIdx]];
    auto totalStats = getTotalStats(groupMap, rules);
    displayRules(rules);
    displayGroup(groupFileVec, rules, groupIdx, totalStats.groupCount);
    displayTotalStats(totalStats);
    if (doDisplayHelp) {
      doDisplayHelp = false;
      displayHelp();
    }
    if (!errorMsg.empty()) {
      fmt::print("\n{:>4}Error:\n{:>15}{}\n", "", "", errorMsg);
      errorMsg.clear();
    }
    try {
      std::string cmd, arg;
      commandPrompt(cmd, arg, groupIdx, totalStats.groupCount);
      // Exit program without deleting the marked files (if any).
      if (cmd == "quit" || cmd == "exit") {
        return;
      }
      procCommand(
        groupIdx, doDisplayHelp, rules, totalStats, groupFileVec, cmd, arg, groupMap);
    }
    catch (const std::runtime_error &e) {
      errorMsg = e.what();
    }
  }
}

void procCommand(size_t &groupIdx, bool &doDisplayHelp, Rules &rules, const Stats &totalStats,
  const FileVec &groupFileVec, const std::string &cmd, const std::string &arg, HashToGroupMap &groupMap)
{
  if (cmd == "delete") {
    if (!totalStats.markedBytes) {
      throw std::runtime_error("Nothing to delete yet");
    }
    // Prompt for confirmation then delete the currently marked files.
    if(confirmDeletePrompt(totalStats)){
      deleteMarkedFiles(groupMap, rules);
      removeSingleItemGroups(groupMap);
      rules.clear();
    }
  }
  else if (cmd == "f" || cmd == "first") {
    if (groupIdx == 0) {
      throw std::runtime_error("Already at the first group");
    }
    groupIdx = 0;
  }
  else if (cmd == "p" || cmd == "previous") {
    if (groupIdx == 0) {
      throw std::runtime_error("Already at the first group");
    }
    --groupIdx;
  }
  else if (cmd == "l" || cmd == "last") {
    if (groupIdx == totalStats.groupCount - 1) {
      throw std::runtime_error("Already at the last group");
    }
    groupIdx = totalStats.groupCount - 1;
  }
  else if (cmd == "n" || cmd == "next" || cmd.empty()) {
    if (groupIdx == totalStats.groupCount - 1) {
      throw std::runtime_error("Already at the last group");
    }
    ++groupIdx;
  }
  // Add path rule if cmd is a number.
  else if (isInt(cmd)) {
    rules.addPathRule(groupFileVec[argToIdx(cmd, groupFileVec.size()) - 1].path);
  }
  // Add regex rule if cmd is a regex.
  else if (cmd.size() >= 2) {
    rules.addRegexRule(cmd);
  }
  // Erase a rule.
  else if (cmd == "d" || cmd == "remove") {
    rules.eraseRule(argToIdx(arg, rules.getRuleCount()));
  }
  // Display help.
  else if (cmd == "h" || cmd == "help" || cmd == "?") {
    doDisplayHelp = true;
  }
  else {
    throw std::runtime_error(fmt::format("Unknown command: {}", cmd));
  }
}

// Prompt user for command and optional argument.
void commandPrompt(std::string &cmd, std::string &arg, const size_t groupIdx, const size_t groupCount)
{
  std::string cmdline;
  fmt::print("\n    {:n} / {:n} > ", groupIdx + 1, groupCount);
  std::cout << std::flush;
  getline(std::cin, cmdline);
  // Split command into command and argument.
  size_t spaceIdx = cmdline.find(' ');
  cmd = cmdline.substr(0, spaceIdx);
  trim(cmd);
  if (spaceIdx != (size_t)-1) {
    arg = cmdline.substr(spaceIdx);
    trim(arg);
  }
}

bool isInt(const std::string &cmd)
{
  try {
    lexical_cast<size_t>(cmd);
    return true;
  }
  catch (std::exception& e) {
    return false;
  }
}

size_t argToIdx(const std::string &arg, const size_t &maxIdx)
{
  size_t idx;
  if (arg.empty()) {
    throw std::runtime_error("Missing index argument");
  }
  try {
    idx = lexical_cast<size_t>(arg);
  }
  catch (std::exception &e) {
    throw std::runtime_error(fmt::format("Invalid index: {}", arg));
  }
  if (idx < 1 || idx > maxIdx) {
    throw std::runtime_error(fmt::format("Index must be between 1 and {}", maxIdx));
  }
  return idx;
}

void refreshGroups(GroupsBySizeVec &groupsBySize, HashToGroupMap &groupMap)
{
  removeSingleItemGroups(groupMap);
  groupsBySize = sortGroupsBySize(groupMap);
}

void displayRules(const Rules &rules)
{
  auto ruleVec = rules.getRulesForDisplay();
  fmt::print("\n    Rules:\n");
  if (ruleVec.empty()) {
    fmt::print("{:<15}No rules defined\n", "");
  }
  else {
    int ruleIdx(0);
    for (const auto &r : ruleVec) {
      fmt::print("{:>14n} {}\n", ++ruleIdx, r);
    }
  }
}

void displayGroup(
  const FileVec &fileVec, const Rules &rules, const size_t groupIdx, const size_t groupCount)
{
  fmt::print("\n    Duplicates:\n", groupIdx + 1, groupCount);
  size_t fileIdx = 0;
  size_t matchedCount = 0;
  auto allAreMarked = false;
  for (const auto &fileInfo : fileVec) {
    auto markerStr = " ";
    if (rules.isMatch(fileInfo)) {
      // Don't mark the last file in the group if it would cause all files in the group to be
      // marked. This is to ensure that the program never deletes all files in a group.
      allAreMarked = ++matchedCount == fileVec.size();
      markerStr = allAreMarked ? "P" : "*";
    }
    fmt::print("{:>9}{} {:>3n} {}\n", "", markerStr, ++fileIdx, fileInfo.path.native());
  }
  if (allAreMarked) {
    fmt::print("\n{:>14} To preserve one copy, the matching file marked with P will NOT be deleted\n", "");
  }
  auto groupStats = getGroupStats(fileVec, rules);
  fmt::print("\n");
  fmt::print("{:>14n} bytes per file, all with hash {}\n", fileVec[0].size, fileVec[0].hash);
  fmt::print("{:>14n} bytes in group\n", groupStats.totalBytes);
  fmt::print("{:>14n} bytes in duplicates\n", groupStats.dupBytes);
  fmt::print("{:>14n} bytes in marked files\n", groupStats.markedBytes);
}

void displayHelp()
{
  fmt::print("\n    Commands:\n");
  fmt::print("{:<8} n, next, <Enter>  go to next group\n", "");
  fmt::print("{:<8} f, first          go to first group\n", "");
  fmt::print("{:<8} l, last           go to last group\n", "");
  fmt::print("{:<8} p, previous       go to previous group\n", "");
  fmt::print("{:<8} regex (string)    add rule to delete all files in all groups matching the regex\n", "");
  fmt::print("{:<8} index (number)    add rule to delete the single file with given index in the group\n", "");
  fmt::print("{:<8} d, delete         remove rule\n", "");
  fmt::print("{:<8} h, help, ?        display this message\n", "");
  fmt::print("{:<8} exit              exit program without deleting anything\n", "");
  fmt::print("{:<8} delete            prompt, then delete all marked files\n", "");
}

void displayTotalStats(const Stats &stats)
{
  fmt::print("\n    Total:\n");
  fmt::print("{:>14n} files\n", stats.totalCount);
  fmt::print("{:>14n} groups\n", stats.groupCount);
  fmt::print("{:>14n} duplicates\n", stats.dupCount);
  fmt::print("{:>14n} marked files\n", stats.markedCount);
  fmt::print("{:>14n} bytes in all groups\n", stats.totalBytes);
  fmt::print("{:>14n} bytes in duplicates\n", stats.dupBytes);
  fmt::print("{:>14n} bytes in all marked files\n", stats.markedBytes);
  fmt::print(
    "{:>14.2f} files per group (average)\n", (float)stats.totalCount / (float)stats.groupCount);
}

bool confirmDeletePrompt(const Stats &totalStats)
{
  fmt::print("\n");
  while (true) {
    fmt::print("About to delete {:n} files ({:n} bytes) Delete? (y/n) > ", totalStats.markedCount,
      totalStats.markedBytes);
    std::cout << std::flush;
    std::string cmdline;
    getline(std::cin, cmdline);
    if (cmdline == "Y" || cmdline == "y") {
      return true;
    }
    if (cmdline == "N" || cmdline == "n") {
      return false;
    }
  }
}

void deleteMarkedFiles(HashToGroupMap &groupMap, const Rules &rules)
{
  size_t deletedCount = 0;
  size_t deleteIdx = 0;
  auto totalStats = getTotalStats(groupMap, rules);

  for (auto &groupPair : groupMap) {
    auto &fileVec = groupPair.second;
    auto fileIter = fileVec.begin();
    while (fileIter != fileVec.end()) {
      if (rules.isMatch(*fileIter)) {
        if (deleteFile(*fileIter)) {
          ++deletedCount;
        }
        fileIter = fileVec.erase(fileIter);
      }
      else {
        ++fileIter;
      }
      ++deleteIdx;
      displayDeleteStatus(totalStats, deleteIdx, deletedCount);
    }
  }
}

bool deleteFile(const FileInfo &fileInfo)
{
  try {
    if (DRY_RUN_ARG) {
      fmt::print("Dry-run: Skipped delete: {}\n", fileInfo.path.native());
    }
    else {
      remove(fileInfo.path);
      print_verbose("Deleted: {}\n", fileInfo.path.native());
    }
    return true;
  }
  catch (const fs::filesystem_error &e) {
    fmt::print("Couldn't delete: {}\n", fileInfo.path.native());
    print_verbose("Cause: {}\n", e.what());
  }
  catch (...) {
    fmt::print("Couldn't delete: {}\n", fileInfo.path.native());
    print_verbose("Cause: Unknown exception\n");
  }
  return false;
}

void displayDeleteStatus(const Stats &totalStats, const size_t deleteIdx, const size_t deletedCount)
{
  // Display status if one second has elapsed or this is last iteration
  if (QUIET_ARG || (LAST_STATUS_TIME.elapsed() < 1.0)) {
    return;
  }
  print_quiet("Deleting files: {:.2f}% ({:n} / {:n})\n",
    (float)deleteIdx / (float)totalStats.markedCount * 100, deleteIdx, totalStats.markedCount);
  print_quiet("Failed: {:n} markedFiles\n", deleteIdx - deletedCount);
  LAST_STATUS_TIME.restart();
}

Stats getGroupStats(const FileVec &groupFileVec, const Rules &rules)
{
  Stats stats;
  size_t fileIdx = 0;
  for (const auto &fileInfo : groupFileVec) {
    stats.totalCount += 1;
    stats.totalBytes += fileInfo.size;
    stats.groupCount = 1;
    if (fileIdx) {
      stats.dupCount += 1;
      stats.dupBytes += fileInfo.size;
    }
    if (rules.isMatch(fileInfo)) {
      // Don't mark the last file in the group if it would cause all files in the group to be
      // marked. This is to ensure that the program never deletes all files in a group.
      if (stats.markedCount != groupFileVec.size() - 1) {
        stats.markedCount += 1;
        stats.markedBytes += fileInfo.size;
      }
    }
    ++fileIdx;
  }
  return stats;
}

Stats getTotalStats(const HashToGroupMap &groupMap, const Rules &rules)
{
  Stats stats;
  for (const auto &group : groupMap) {
    auto groupStats = getGroupStats(group.second, rules);
    stats += groupStats;
  }
  return stats;
}

// Switch from C locale to user's locale. This works together with fmt "{:n}" for adding thousand
// grouping to all ints for US locale and hopefully most others.
void setupLocale()
{
  std::locale::global(std::locale(""));
}

void parseCommandLine(int argc, char **argv)
{
  try {
    po::options_description desc("duplex - Delete duplicate files - dahlsys.com");
    desc.add_options()("help,h", "produce help message")("dry-run,d", po::bool_switch(&DRY_RUN_ARG),
      "don't delete anything, just simulate")("automatic,a", po::bool_switch(&AUTOMATIC_ARG),
      "don't enter interactive mode (delete without confirmation)")("filter-small,s",
      po::value<size_t>(&IGNORE_SMALLER_ARG), "ignore files of this size and smaller")(
      "filter-large,b", po::value<size_t>(&IGNORE_LARGER_ARG), "ignore files of this size and larger")(
      "quiet,q", po::bool_switch(&QUIET_ARG), "display only error messages")("verbose,v",
      po::bool_switch(&VERBOSE_ARG), "display verbose messages")("debug,e", po::bool_switch(&DEBUG_ARG),
      "display debug / optimization info")("md5,5", po::bool_switch(&USE_MD5_ARG),
      "use md5 cryptographic hash (fnv 64 bit hash is used by default)")("rule,u",
      po::value<std::vector<std::string>>(&RULE_VEC_ARG),
      "add marking rule (case insensitive regex)")("rfolder,r",
      po::value<std::vector<fs::path>>(&RECURSIVE_PATH_VEC_ARG), "add recursive search folder")("md5list,m",
      po::value<std::vector<fs::path>>(&MD5_PATH_VEC_ARG), "add md5 list file (output from md5deep -zr)")(
      "folder,f", po::value<std::vector<fs::path>>(&PATH_VEC_ARG), "add search folder");

    po::positional_options_description p;
    p.add("rfolder", -1);
    po::variables_map vm;
    store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    notify(vm);
    // Display help and exit if required options (yes, I know) are missing.
    if (vm.count("help") ||
      (PATH_VEC_ARG.empty() && RECURSIVE_PATH_VEC_ARG.empty() && MD5_PATH_VEC_ARG.empty())) {
      fmt::print("{}\nArguments are equivalent to rfolder options\n", desc);
      exit(1);
    }
    // Switch to md5 hashes if md5lists are used.
    if (!MD5_PATH_VEC_ARG.empty()) {
      fmt::print("Enabled md5 hashes due to md5list being used\n");
      USE_MD5_ARG = true;
    }
  }
  catch (std::exception &e) {
    fmt::print("Error: {}\n", e.what());
    exit(1);
  }
  catch (...) {
    fmt::print("Error: Unknown exception\n");
    exit(1);
  }
}

#pragma clang diagnostic pop
