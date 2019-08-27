// Duplex - Delete duplicate files

#pragma clang diagnostic push
//#pragma ide diagnostic ignored "modernize-pass-by-value"
//#pragma clang diagnostic ignored "-Wshadow"

#include "pch.h"

#include "fnv_1a_64.h"
#include "junction.h"
#include <z3.h>

#include "md5.hpp"

#ifndef WIN32
using namespace __gnu_cxx;
#else
using namespace stdext;
#endif

using namespace std;
using namespace boost;
using namespace boost::filesystem;
using namespace boost::program_options;

// Command line args
vector<path> PATH_VEC_ARG;
vector<path> RECURSIVE_PATH_VEC_ARG;
vector<path> MD5_PATH_VEC_ARG;
vector<string> RULE_VEC_ARG;
bool AUTOMATIC_ARG(false);
bool VERBOSE_ARG(false);
bool QUIET_ARG(false);
bool DEBUG_ARG(false);
bool USE_MD5_ARG(false);
bool DRY_RUN_ARG(false);
size_t IGNORE_SMALLER_ARG((size_t)-1), IGNORE_LARGER_ARG((size_t)-1);

typedef string Hash;

// Compare functions for regular sort operations do not need any context beyond the value pairs that
// the sort function passes in. This class is created before the sort and stores GroupMap context,
// allowing HashToGroupMap by be sorted by FileInfo in the GroupMap.
template <typename GroupMap> class CompareGroupsByHash {
public:
  explicit CompareGroupsByHash(GroupMap groupMap) : groupMap(groupMap)
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
      return groupA[0].itemPath > groupB[0].itemPath;
    }
    return groupA[0].size > groupB[0].size;
  }

private:
  GroupMap groupMap;
};

// Hold one file entry
class FileInfo {
public:
  FileInfo(path itemPath, size_t size, string hash)
    : itemPath(std::move(itemPath)), size(size), hash(hash)
  {
  }

  FileInfo(path itemPath, size_t size) : itemPath(std::move(itemPath)), size(size)
  {
  }

  string str()
  {
    if (hash.empty()) {
      return fmt::format("{:>14n} {}", size, itemPath.native());
    }
    else {
      return fmt::format("{:>14n} {} {}", size, string(hash), itemPath.native());
    }
  }

  path itemPath;
  size_t size;
  string hash{""};
};

typedef vector<FileInfo> FileVec;

// Hold vector of files in one group. It's important that this is a vector and not
// another sequence, as we rely on iterators into the vector remaining valid even
// when items are removed.
typedef vector<FileInfo> Group;

// Map that files are initially put into when they're found. It both orders and
// groups files by file_size.
typedef map<size_t, Group> SizeToGroupMap;

// Hash of vectors of files is the map that files are put into after
// files with unique file lengths are eliminated. It groups files by
// hash.
typedef unordered_map<Hash, Group> HashToGroupMap;

// Vec that keeps the order that the group_maps were created in. Because
// file_map is a map sorted by file_size, group_walker is also sorted. This
// enables us to move back and forth in groups based on file size, and to put
// the group with the largest files first.
typedef vector<Hash> GroupsBySizeVec;

// A file_ref points to a specific file in a specific group
// typedef pair<HashToGroupMap::iterator, Group::iterator> FileRef;
// typedef deque<FileRef> FileRefs;

class Rules {
public:
  void addRegexRule(const string &arg)
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

  void addPathRule(const path &arg)
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
        if (fileInfo.itemPath.native() == rule.native()) {
          return true;
        }
      }
      else {
        if (regex_search(fileInfo.itemPath.native(), regexVec[idx])) {
          return true;
        }
      }
      ++idx;
    }
    return false;
  }

  [[nodiscard]] vector<string> getRulesForDisplay() const
  {
    vector<string> r;
    size_t idx = 0;
    for (auto &p : pathVec) {
      if (!p.empty()) {
        r.push_back(fmt::format("path:  {}", p.native()));
      }
      else {
        r.push_back(fmt::format("regex: {}", regexStrVec[idx]));
      }
      ++idx;
    }
    return r;
  }

  //  bool isPathRule(size_t idx) {
  //    return !pathVec[idx].empty();
  //  }

  //  void eraseRule(const string& arg) {
  //    size_t idx = getIndex(regexStrVec, arg);
  //    if (idx != -1) {
  //      eraseIndex(regexStrVec, idx);
  //      eraseIndex(regexVec, idx);
  //    }
  //  }

  //  template<typename T, typename X>
  //  size_t getIndex(const T& v, const X& arg)
  //  {
  //    size_t idx = 0;
  //    for (auto &a : v) {
  //      if (a == arg) {
  //        return idx;
  //      }
  //      ++idx;
  //    }
  //    return -1;
  //  }

  //  template<typename T>
  //  size_t eraseIndex(const T& v, size_t idx)
  //  {
  //    v.erase(v.begin() + idx);
  //  }

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
  vector<regex> regexVec;
  vector<string> regexStrVec;
  vector<path> pathVec;
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

// Verify command line args
void verifyDirPaths();
bool isInvalidDirPath(const path &p);
// Find all files that will be checked for duplicates.
FileVec findAllFiles();
void addPath(FileVec &fileVec, const path &itemPath, const bool &recursive);
void addMd5File(FileVec &fileVec, const path &md5DeepPath);
void addFile(FileVec &fileVec, const path &filePath);
void displayFindStatus(const FileVec &fileVec, bool forceDisplay = false);
// Group files by size and remove single item groups (files with unique sizes can't have dups)
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
// Rules
void addRulesFromCommandLine(Rules &rules);
// Add rules interactively
void sortAllFileInfoVec(HashToGroupMap &hashToGroupMap);
GroupsBySizeVec sortGroupsBySize(const HashToGroupMap &hashToGroupMap);
void addRulesInteractive(Rules &rules, HashToGroupMap &groupMap);
size_t argToIdx(const string &arg, const size_t &maxIdx);
size_t goToGroup(size_t groupIdx, size_t groupCount, const string &cmd);
void refreshGroups(GroupsBySizeVec &groupsBySize, HashToGroupMap &groupMap);
void commandPrompt(string &cmd, string &arg, size_t groupIdx, size_t groupCount);
void displayRules(const Rules &rules);
void displayGroup(const FileVec &fileVec, const Rules &rules, size_t groupIdx, size_t groupCount);
void displayHelp();
void displayTotalStats(const Stats &stats);
// Delete files marked by the rules.
bool confirmDeletePrompt(const Stats &totalStats);
void deleteMarkedFiles(HashToGroupMap &groupMap, const Rules &rules);
bool deleteFile(const FileInfo &fileInfo);
void displayDeleteStatus(const Stats &totalStats, size_t deleteIdx, size_t deletedCount);
// Misc
Stats getGroupStats(const FileVec &fileVec, const Rules &rules);
Stats getTotalStats(const HashToGroupMap &groupMap, const Rules &rules);
// Locale and command line.
void setupLocale();
void parseCommandLine(int argc, char **argv);
void procCommand(size_t &groupIdx, bool &doDisplayHelp, Rules &rules, const Stats &totalStats,
  const FileVec &groupFileVec, const string &cmd, const string &arg, HashToGroupMap &groupMap);
// Print only when called with --verbose.
template <typename... Args> void vprint(const char *fmt, Args &&... args)
{
  if (VERBOSE_ARG) {
    fmt::print(fmt, std::forward<Args>(args)...);
  }
}

int main(int argc, char *argv[])
{
  setupLocale();
  parseCommandLine(argc, argv);
  verifyDirPaths();
  auto fileVec = findAllFiles();
  vprint("fileVec size: {:n}\n", fileVec.size());
  auto sizeToGroupMap = groupFilesBySize(fileVec);
  vprint("sizeToGroupMap size: {:n}\n", sizeToGroupMap.size());
  removeSingleItemGroups(sizeToGroupMap);
  vprint("sizeToGroupMap size: {:n}\n", sizeToGroupMap.size());
  hashAll(fileVec);
  auto hashToGroupMap = groupFilesByHash(fileVec);
  sortAllFileInfoVec(hashToGroupMap);
  // Vec of marking rules
  Rules rules;
  addRulesFromCommandLine(rules);
  // Set up rules for selecting files to delete
  if (!AUTOMATIC_ARG) {
    addRulesInteractive(rules, hashToGroupMap);
  }
  else {
    deleteMarkedFiles(hashToGroupMap, rules);
  }
  // Show final stats after deletes.
  auto totalStats = getTotalStats(hashToGroupMap, rules);
  displayTotalStats(totalStats);
  // Success
  exit(0);
}

// Verify that all provided folder names have legal syntax and exist
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

// Create vector of files to check for duplicates.
FileVec findAllFiles()
{
  FileVec fileVec;
  // Add all files in search folders, non-recursive
  for (auto &p : PATH_VEC_ARG) {
    vprint("Processing non-recursive: {}\n", p.native());
    addPath(fileVec, canonical(p), false);
  }
  // Add all files in search folders, recursive
  for (auto &p : RECURSIVE_PATH_VEC_ARG) {
    vprint("Processing recursive: {}\n", p.native());
    addPath(fileVec, canonical(p), true);
  }
  // Add all files provided as md5 vectors
  for (auto &p : MD5_PATH_VEC_ARG) {
    vprint("Processing MD5 file: {}\n", p.native());
    addMd5File(fileVec, p);
  }
  displayFindStatus(fileVec, true);
  return fileVec;
}

// Add a file or directory path.
void addPath(FileVec &fileVec, const path &itemPath, const bool &recursive)
{
  try {
    // Do not follow symlinks and junctions
    if ((!is_regular_file(itemPath) && !is_directory(itemPath)) || isJunction(itemPath)) {
      vprint("Ignored special file: {}\n", itemPath.native());
      return;
    }
    if (is_regular_file(itemPath)) {
      addFile(fileVec, itemPath);
    }
    else {
      vprint("Entering dir: {}\n", itemPath.native());
      for (const auto &subPath : directory_iterator(itemPath)) {
        addPath(fileVec, subPath, recursive);
      }
    }
  }
  catch (const std::exception &e) {
    fmt::print("Error processing: {}\n", itemPath.native());
    fmt::print("Cause: {}\n", e.what());
  }
}

// Add a vector of files generated with md5deep or similar.
// File format is one size, MD5 and full itemPath per line. Example:
// 43912  ccd6dad4b72d1255cf2e7a9dadd64083  C:\Documents and
// Settings\Administrator\Desktop\test.txt
void addMd5File(FileVec &fileVec, const path &md5DeepPath)
{
  regex md5line(" *([0-9]+) +([0-9a-fA-F]{32}) +(.*)");
  boost::smatch what;
  size_t fileCount(0);
  // Open file for reading
  std::wifstream fi(md5DeepPath.native().c_str());
  if (!fi.good()) {
    fmt::print("Error: Couldn't open file: {}\n", md5DeepPath);
    return;
  }

  while (fi.good()) {
    // Count processed files
    fileCount++;
    // Get line
    string line;
    // getline(fi, line); TODO
    trim(line);
    // Ignore empty lines
    if (line.empty()) {
      continue;
    }
    // Parse line into size, md5 and itemPath
    if (!regex_search(line, what, md5line)) {
      fmt::print("Error: Malformed line in md5 file: {}\n", md5DeepPath);
      fmt::print("Line: {}\n", line);
      continue;
    }
    string sizeStr(what[1].first, what[1].second);
    string md5(what[2].first, what[2].second);
    string filePath(what[3].first, what[3].second);
    addFile(fileVec, filePath);
  }
}

void addFile(FileVec &fileVec, const path &filePath)
{
  auto absFilePath = canonical(filePath);
  auto fileSize = filesystem::file_size(absFilePath);
  // Filter small files if requested
  if (IGNORE_SMALLER_ARG != (size_t)-1 && fileSize <= IGNORE_SMALLER_ARG) {
    vprint(
      "Ignored small file (< {:n}): {:n} {}\n", IGNORE_SMALLER_ARG, fileSize, absFilePath.native());
    return;
  }
  // Filter large files if requested
  if (IGNORE_LARGER_ARG != (size_t)-1 && fileSize >= IGNORE_LARGER_ARG) {
    vprint("Ignored large file (> {:n}): {:n} {}\n", IGNORE_LARGER_ARG, fileSize,
      absFilePath.native(), absFilePath.native());
    return;
  }
  // Add file
  auto fileInfo = FileInfo(absFilePath, fileSize, "");
  fileVec.push_back(fileInfo);
  vprint("Found: {}\n", fileInfo.str());
  displayFindStatus(fileVec);
}

// Display status if more than one second has elapsed
void displayFindStatus(const FileVec &fileVec, bool forceDisplay)
{
  if (forceDisplay || (!QUIET_ARG && LAST_STATUS_TIME.elapsed() >= 1.0)) {
    fmt::print("Files found: {:n}\n", fileVec.size());
    LAST_STATUS_TIME.restart();
  }
}

SizeToGroupMap groupFilesBySize(const FileVec &fileVec)
{
  SizeToGroupMap sizeToGroupMap;
  for (auto &fileInfo : fileVec) {
    sizeToGroupMap[fileInfo.size].push_back(fileInfo);
  }
  return sizeToGroupMap;
}

// Remove groups with only one item. These file in the group has unique file size or hash so
// cannot have duplicates.
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
    fmt::print("Filtered out {:n} single item or empty groups\n", removedCount);
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
      fmt::print("Ignored file: {}\n", fileInfo.itemPath.native());
      fmt::print("Cause: {}\n", e.what());
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
    std::ifstream f(fileInfo.itemPath.native(), ios::binary);
    string ck(md5(f).digest().hex_str_value());
    string wck(ck.begin(), ck.end());
    fileInfo.hash = wck;
  }
  else {
    fileInfo.hash = fnv1A64(fileInfo.itemPath);
  }
  vprint("{}: {}\n", USE_MD5_ARG ? "MD5 " : "FNV64", fileInfo.str());
}

void displayHashStatus(const FileInfo &fileInfo, size_t accumulatedSize, size_t totalSize,
  size_t fileCount, size_t fileIdx)
{
  if (!QUIET_ARG && totalSize &&
    (LAST_STATUS_TIME.elapsed() >= 1.0 || accumulatedSize == totalSize)) {
    fmt::print("Calculating {} hashes:\n", USE_MD5_ARG ? "MD5" : "FNV64");
    fmt::print("Data: {:.2f}% ({:n} / {:n} bytes)\n",
      (float)accumulatedSize / (float)totalSize * 100, accumulatedSize, totalSize);
    fmt::print("Files: {:.2f}% ({:n} / {:n} files)\n", (float)fileIdx / (float)fileCount * 100,
      fileIdx, fileCount);
    LAST_STATUS_TIME.restart();
  }
}

// Sum up the total size of files to hash.
// The vector may include entries imported from md5 files, which includes hash.
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

// Sort the FileVec in each group by the itemPaths.
void sortAllFileInfoVec(HashToGroupMap &hashToGroupMap)
{
  for (auto &groupPair : hashToGroupMap) {
    std::sort(std::begin(groupPair.second), std::end(groupPair.second),
      [](const FileInfo &a, const FileInfo &b) { return a.itemPath < b.itemPath; });
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

// Start interactive section
void addRulesInteractive(Rules &rules, HashToGroupMap &groupMap)
{
  bool doDisplayHelp = true;
  size_t groupIdx = 0;
  GroupsBySizeVec groupsBySize;
  string errorMsg;
  fmt::print("\n");

  for (;;) {
    refreshGroups(groupsBySize, groupMap);
    // Exit if no more groups
    if (groupMap.empty()) {
      if (!QUIET_ARG) {
        fmt::print("No more duplicates found\n");
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
      string cmd, arg;
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
  const FileVec &groupFileVec, const string &cmd, const string &arg, HashToGroupMap &groupMap)
{
  auto newGroupIdx = goToGroup(groupIdx, totalStats.groupCount, cmd);
  if (newGroupIdx != groupIdx) {
    groupIdx = newGroupIdx;
    return;
  }
  if (cmd == "delete") {
    if (!totalStats.markedBytes) {
      throw runtime_error("Nothing to delete yet");
    }
    // Prompt for confirmation then delete the currently marked files.
    if(confirmDeletePrompt(totalStats)){
      deleteMarkedFiles(groupMap, rules);
      removeSingleItemGroups(groupMap);
      rules.clear();
    }
    return;
  }
  // Erase a rule
  if (cmd == "d" || cmd == "remove") {
    rules.eraseRule(argToIdx(arg, rules.getRuleCount()));
    return;
  }
  // Display help
  if (cmd == "h" || cmd == "help" || cmd == "?") {
    doDisplayHelp = true;
    return;
  }
  // Add path rule if cmd is a number
  try {
    lexical_cast<size_t>(cmd);
    rules.addPathRule(groupFileVec[argToIdx(cmd, groupFileVec.size()) - 1].itemPath);
    return;
  }
  catch (std::exception& e) {}
  // Add regex rule if cmd is a regex
  if (cmd.size() >= 2) {
    rules.addRegexRule(cmd);
    return;
  }
  throw runtime_error(fmt::format("Unknown command: {}", cmd));
}

void commandPrompt(string &cmd, string &arg, const size_t groupIdx, const size_t groupCount)
{
  string cmdline;
  fmt::print("\n    {:n} / {:n} > ", groupIdx + 1, groupCount);
  cout << flush;
  getline(cin, cmdline);
  // Split command into command and argument
  size_t spaceIdx = cmdline.find(' ');
  cmd = cmdline.substr(0, spaceIdx);
  trim(cmd);
  if (spaceIdx != (size_t)-1) {
    arg = cmdline.substr(spaceIdx);
    trim(arg);
  }
}

size_t goToGroup(size_t groupIdx, size_t groupCount, const string &cmd)
{
  if (cmd == "f" || cmd == "first") {
    if (groupIdx == 0) {
      throw runtime_error("Already at the first group");
    }
    groupIdx = 0;
  }
  else if (cmd == "p" || cmd == "previous") {
    if (groupIdx == 0) {
      throw runtime_error("Already at the first group");
    }
    --groupIdx;
  }
  else if (cmd == "l" || cmd == "last") {
    if (groupIdx == groupCount - 1) {
      throw runtime_error("Already at the last group");
    }
    groupIdx = groupCount - 1;
  }
  else if (cmd == "n" || cmd == "next" || cmd.empty()) {
    if (groupIdx == groupCount - 1) {
      throw runtime_error("Already at the last group");
    }
    ++groupIdx;
  }
  return groupIdx;
}

size_t argToIdx(const string &arg, const size_t &maxIdx)
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

bool isInvalidDirPath(const path &p)
{
  if (!is_directory(p)) {
    fmt::print("Invalid path: {}\n", p.native());
    return true;
  }
  return false;
}

void displayRules(const Rules &rules)
{
  auto ruleVec = rules.getRulesForDisplay();
  fmt::print("\n    Rules:\n");
  if (ruleVec.empty()) {
    fmt::print("              No rules defined\n");
  }
  else {
    int ruleIdx(0);
    for (const auto &r : ruleVec) {
      fmt::print("           {:>3n}: {}\n", ++ruleIdx, r);
    }
  }
}

void displayGroup(
  const FileVec &fileVec, const Rules &rules, const size_t groupIdx, const size_t groupCount)
{
  fmt::print("\n    Duplicates:\n", groupIdx + 1, groupCount);
  size_t fileIdx = 0;
  size_t matchedCount = 0;
  for (const auto &fileInfo : fileVec) {
    bool isMatch = rules.isMatch(fileInfo);
    if (isMatch) {
      ++matchedCount;
      // Don't mark the last file in the group if it would cause all files in the group to be
      // marked. This is to ensure that the program never deletes all files in a group.
      if (matchedCount == fileVec.size()) {
        isMatch = false;
      }
    }
    fmt::print("{:>9}{} {:>3n} {}\n", "", isMatch ? '*' : ' ', ++fileIdx, fileInfo.itemPath.native());
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
  fmt::print("        {:<15}{}\n", "<Enter>",         "go to next group");
  fmt::print("        {:<15}{}\n", "f (first)",       "go to first group");
  fmt::print("        {:<15}{}\n", "l (last)",        "go to last group");
  fmt::print("        {:<15}{}\n", "p (previous)",    "go to previous group");
  fmt::print("        {:<15}{}\n", "regex string",    "add rule to delete all files in all groups matching the regex");
  fmt::print("        {:<15}{}\n", "index number",    "add rule to delete the single file with given index in the group");
  fmt::print("        {:<15}{}\n", "d (index)",       "remove rule");
  fmt::print("        {:<15}{}\n", "h, help, ?",      "display this message");
  fmt::print("        {:<15}{}\n", "exit",            "exit program without deleting anything");
  fmt::print("        {:<15}{}\n", "delete",          "prompt, then delete all marked files");
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
    cout << flush;
    string cmdline;
    getline(cin, cmdline);
    if (cmdline == "Y" || cmdline == "y") {
      fmt::print("\n");
      return true;
    }
    if (cmdline == "N" || cmdline == "n") {
      fmt::print("\n");
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
      fmt::print("Dry-run: Skipped delete: {}\n", fileInfo.itemPath.native());
    }
    else {
      // remove(file.second->itemPath);
      vprint("Deleted: {}\n", fileInfo.itemPath.native());
    }
    return true;
  }
  catch (const filesystem_error &e) {
    fmt::print("Couldn't delete: {}\n", fileInfo.itemPath.native());
    fmt::print("Cause: {}\n", e.what());
  }
  catch (...) {
    fmt::print("Couldn't delete: {}\n", fileInfo.itemPath.native());
    fmt::print("Cause: Unknown exception\n");
  }
  return false;
}

void displayDeleteStatus(const Stats &totalStats, const size_t deleteIdx, const size_t deletedCount)
{
  // Display status if one second has elapsed or this is last iteration
  if (QUIET_ARG || (LAST_STATUS_TIME.elapsed() < 1.0)) {
    return;
  }
  fmt::print("Deleting files: {:.2f}% ({:n} / {:n})\n",
    (float)deleteIdx / (float)totalStats.markedCount * 100, deleteIdx, totalStats.markedCount);
  fmt::print("Failed: {:n} markedFiles\n", deleteIdx - deletedCount);
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

// Switch from C locale to user's locale. This works together with the fmt "{:n}" for adding
// thousand grouping to all ints for US locale and hopefully most others.
void setupLocale()
{
  locale::global(locale(""));
}

void parseCommandLine(int argc, char **argv)
{
  try {
    options_description desc("Duplex - Delete duplicate files - dahlsys.com");
    desc.add_options()("help,h", "produce help message")("dry-run,d", bool_switch(&DRY_RUN_ARG),
      "don't delete anything, just simulate")("automatic,a", bool_switch(&AUTOMATIC_ARG),
      "don't enter interactive mode (delete without confirmation)")("filter-small,s",
      value<size_t>(&IGNORE_SMALLER_ARG), "ignore files of this size and smaller")(
      "filter-large,b", value<size_t>(&IGNORE_LARGER_ARG), "ignore files of this size and larger")(
      "quiet,q", bool_switch(&QUIET_ARG), "display only error messages")("verbose,v",
      bool_switch(&VERBOSE_ARG), "display verbose messages")("debug,e", bool_switch(&DEBUG_ARG),
      "display debug / optimization info")("md5,5", bool_switch(&USE_MD5_ARG),
      "use md5 cryptographic hash (fnv 64 bit hash is used by default)")("rule,u",
      value<vector<string>>(&RULE_VEC_ARG),
      "add marking rule (case insensitive regex)")("rfolder,r",
      value<vector<path>>(&RECURSIVE_PATH_VEC_ARG), "add recursive search folder")("md5list,m",
      value<vector<path>>(&MD5_PATH_VEC_ARG), "add md5 list file (output from md5deep -zr)")(
      "folder,f", value<vector<path>>(&PATH_VEC_ARG), "add search folder");

    positional_options_description p;
    p.add("rfolder", -1);
    variables_map vm;
    store(command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    notify(vm);
    // Display help and exit if required options (yes, I know) are missing
    if (vm.count("help") ||
      (PATH_VEC_ARG.empty() && RECURSIVE_PATH_VEC_ARG.empty() && MD5_PATH_VEC_ARG.empty())) {
      fmt::print("{}\nArguments are equivalent to rfolder options\n", desc);
      exit(1);
    }
    // Switch to md5 hashes if md5lists are used
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
