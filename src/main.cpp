// Duplex - Delete duplicate files

#include "pch.h"

#include "md5.hpp"
#include "fnv_1a_64.h"
#include "junction.h"

#ifndef WIN32
using namespace __gnu_cxx;
#endif

#ifdef WIN32
using namespace stdext;
#endif

using namespace std;
using namespace boost;
using namespace boost::filesystem;
using namespace boost::program_options;

// Program arguments

vector<string> folder_args;
vector<string> rfolder_args;
vector<string> md5list_args;
vector<string> rule_args;
string search, file;
bool automatic(false);
bool verbose(false);
bool quiet(false);
bool debug(false);
bool use_md5(false);
bool query(false);
bool dry_run(false);
bool remove_empty(false);
u64 smaller(-1), bigger(-1);

// Hold one rule
struct rule
{
  rule(regex rx, string rx_str) : rx(rx), rx_str(rx_str)
  {
  }
  regex rx;
  string rx_str;
};

struct hash_string
{
  // Parameters for hash table
  enum
  {
    bucket_size = 4,
    min_buckets = 8
  };
  // Hash string s1 to size_t value
  size_t operator()(const string& s1) const
  {
    const unsigned char* p = (const unsigned char*)s1.c_str();
    size_t hashval = 0;
    for (size_t n = s1.size(); 0 < n; --n)
      hashval += *p++;
    return (hashval);
  }
  // Test if s1 ordered before s2
  bool operator()(const string& s1, const string& s2) const
  {
    return (s1 < s2);
  }
};

// Hash function for s64
struct hash_s64
{
  // Parameters for hash table
  enum
  {
    bucket_size = 4,
    min_buckets = 8
  };
  // Hash s64 s1 to size_t value
  size_t operator()(const s64& s1) const
  {
    return ((size_t)s1);
  }
  // Test if s1 ordered before s2
  bool operator()(const s64& s1, const s64& s2) const
  {
    return (s1 < s2);
  }
};

// Hold one file entry
struct dup_file
{
  dup_file(path path_inst, u64 size, string hash)
    : path_inst(path_inst), size(size), hash(hash)
  {
  }
  path path_inst;
  u64 size;
  string hash;
};

// Types

// Hold list of files in one group. It's important that this is a list and not
// another sequence, as we rely on iterators into the list remaining valid even
// when items are removed.
typedef list<dup_file> group_t;

// Map that files are initially put into when they're found. It both orders and
// groups files by filesize.
typedef map<u64, group_t> filemap_t;

// List of pointers to the individual files in the file_map
typedef list<dup_file*> filelist_t;

typedef unordered_map<string, group_t, hash_string> groupmap_t;

typedef list<rule> rulelist_t;

// A file_ref points to a specific file in a specific group
typedef pair<groupmap_t::iterator, group_t::iterator> file_ref_t;
typedef deque<file_ref_t> file_refs_t;

filemap_t file_map;

// Display a file with size and path_inst
format filesize_row("%12d %s");
format filesize_hash_row("%12d %s %s");

// hold total stats
struct total_stats_t
{
  total_stats_t()
    : num_total(0),
      num_dup(0),
      num_marked(0),
      num_manual(0),
      num_full_marked(0),
      size_total(0),
      size_dup(0),
      size_marked(0)
  {
  }
  u32 num_total;
  u32 num_dup;
  u32 num_marked;
  u32 num_manual;
  u32 num_full_marked;
  u64 size_total;
  u64 size_dup;
  u64 size_marked;
};

// Declarations

void parse_command_line(int argc, char* argv[]);
bool compare_dup_file(dup_file* _first, dup_file* _second);
void verify_valid_folders();
void get_marked(
  groupmap_t::iterator& groupmap_iter, group_t& group, rulelist_t& rules,
  file_refs_t& files, total_stats_t& total_stats);
bool add_folder(
  const path& dir_path_inst, const bool recursive, const bool verbose,
  const bool quiet, u64 smaller, u64 bigger, bool debug);
bool add_folder_recursive(
  const path& dir_path_inst, const bool recursive, const bool verbose,
  const bool quiet, u64 smaller, u64 bigger, timer& time, u64& file_cnt);
bool add_md5list(
  const path& md5deep_file, const bool verbose, const bool quiet, u64 smaller,
  u64 bigger, bool debug);

int main(int argc, char* argv[])
{
  parse_command_line(argc, argv);

  // Switch from C locale to user's locale
  // will be used for all new streams
  std::locale::global(std::locale(""));
  // This stream was already created, so must imbue
  cout.imbue(locale(""));

  verify_valid_folders();

  // Build database of files to process

  // Add all files in search folders, non-recursive
  for (vector<string>::iterator i = folder_args.begin(); i != folder_args.end();
       ++i) {
    cout << "Processing non-recursive: " << *i << endl;
    add_folder(
      path(*i), false /* not recursive */, verbose, quiet, smaller, bigger,
      debug);
  }

  // Add all files in search folders, recursive
  for (vector<string>::iterator i = rfolder_args.begin();
       i != rfolder_args.end(); ++i) {
    cout << "Processing recursive: " << *i << endl;
    add_folder(
      path(*i), true /* recursive */, verbose, quiet, smaller, bigger, debug);
  }

  // Add all files provided as md5 lists
  for (vector<string>::iterator i = md5list_args.begin();
       i != md5list_args.end(); ++i) {
    cout << "Processing md5 list: " << *i << endl;
    add_md5list(path(*i), verbose, quiet, smaller, bigger, debug);
  }

  // Remove files that have unique size (optimization -- they can't have
  // duplicates so no need to MD5 them)
  u32 unique_size_cnt(0);
  for (filemap_t::iterator filemap_iter = file_map.begin();
       filemap_iter != file_map.end();) {
    if (filemap_iter->second.size() <= 1) {
      // Erasing an element from a map also not invalidate any iterators, except
      // for iterators that actually point to the element that is being erased.
      file_map.erase(filemap_iter++);
      ++unique_size_cnt;
    }
    else {
      ++filemap_iter;
    }
  }
  verbose&& cout << "Removed " << unique_size_cnt
                 << " file entries based on unique file size" << endl;

  // Show debug info about file_map
  if (debug) {
    cout << "Debug: file_map after unique size optimization:" << endl;
    cout << "Debug: file_map size: " << file_map.size() << endl;
    cout << "Debug: file_map max size: " << file_map.max_size() << endl;
  }

  // Hash of lists of files is the map that files are put into after
  // files with unique file lenghts are eliminated. It groups files by
  // hash.
  groupmap_t groupmap;

  // List that keeps the order that the groupmaps were created in. Because
  // file_map is a map sorted by filesize, groupwalker is also sorted. This
  // enables us to move back and forth in groups based on file size, and to put
  // the group with the biggest files first.
  typedef list<string> groupwalker_t;
  groupwalker_t groupwalker;

  // Sum up file sizes and number of files (for progress display)
  u64 totsize(0);
  u64 numfiles(0);
  for (filemap_t::iterator filemap_iter = file_map.begin();
       filemap_iter != file_map.end(); ++filemap_iter) {
    for (group_t::iterator group_iter = filemap_iter->second.begin();
         group_iter != filemap_iter->second.end(); ++group_iter) {
      totsize += group_iter->size;
      ++numfiles;
    }
  }

  // We want to hash the files in the same order as we found them in the
  // filesystem because processing them in that order is much faster than
  // jumping ramdomly around in the filesystem, as we would if we were to
  // process them by filesize.

  // Create a list of pointers to the dup_file entries and sort that list
  filelist_t filelist;
  for (filemap_t::iterator filemap_iter = file_map.begin();
       filemap_iter != file_map.end(); ++filemap_iter) {
    for (group_t::iterator group_iter = filemap_iter->second.begin();
         group_iter != filemap_iter->second.end(); ++group_iter) {
      filelist.push_back(&*group_iter);
    }
  }
  filelist.sort(compare_dup_file);

  // Calculate hashes for all files
  u64 so_far_size(0);
  u64 file_cnt(0);
  timer time;
  for (filelist_t::iterator filelist_iter = filelist.begin();
       filelist_iter != filelist.end(); ++filelist_iter) {
    ++file_cnt;
    string hash;
    try {
      if ((*filelist_iter)->hash == "") {
        if (use_md5) {
          boost::filesystem::ifstream file(
            (*filelist_iter)->path_inst, ios::binary);
          string ck(md5(file).digest().hex_str_value());
          string wck(ck.begin(), ck.end());
          (*filelist_iter)->hash = wck;
        }
        else {
          u64 fnv_hash(fnv_1a_64((*filelist_iter)->path_inst));
          (*filelist_iter)->hash = str(format("%x") % fnv_hash);
        }
        verbose&& cout << (use_md5 ? "MD5: " : "FNV64: ")
                       << str(
                            filesize_hash_row % (*filelist_iter)->size
                            % (*filelist_iter)->hash
                            % (*filelist_iter)->path_inst.native())
                       << endl;
      }
      // Display status
      so_far_size += (*filelist_iter)->size;
      if (!quiet && totsize && (time.elapsed() >= 1.0 || so_far_size == totsize)) {
        cout << fixed << setprecision(2) << "Calculating "
             << (use_md5 ? "MD5" : "FNV64") << " hashes:\n"
             << "Data: " << (float)so_far_size / (float)totsize * 100 << "% ("
             << so_far_size << " / " << totsize << " bytes)\n"
             << "Files: " << (float)file_cnt / (float)numfiles * 100 << "% ("
             << file_cnt << " / " << numfiles << " files)\n"
             << endl;
        time.restart();
      }
    } catch (boost::filesystem::ifstream::failure e) {
      cout << "Skipped file: " << (*filelist_iter)->path_inst.native() << endl;
      cout << "Cause: " << e.what() << endl;
    } catch (const filesystem_error& e) {
      cout << "Skipped file: " << (*filelist_iter)->path_inst.native() << endl;
      cout << "Cause: " << e.what() << endl;
    } catch (const string& s) {
      cout << "Skipped file: " << (*filelist_iter)->path_inst.native() << endl;
      cout << "Cause: " << s << endl;
    } catch (...) {
      cout << "Skipped file: " << (*filelist_iter)->path_inst.native() << endl;
      cout << "Cause: Unknown exception" << endl;
    }
  }
  filelist.clear();

  // Build database of duplicates
  for (filemap_t::iterator filemap_iter = file_map.begin();
       filemap_iter != file_map.end();) {
    for (group_t::iterator group_iter = filemap_iter->second.begin();
         group_iter != filemap_iter->second.end(); ++group_iter) {
      string hash_and_size(
        group_iter->hash + str(format("%d") % group_iter->size));
      groupmap[hash_and_size].push_back(
        dup_file(group_iter->path_inst, group_iter->size, ""));
      // If this is the first file added to groupmap, we add the group to the
      // walker.
      if (groupmap[hash_and_size].size() == 1) {
        groupwalker.push_front(hash_and_size);
      }
    }
    // Erasing an element from a map does not invalidate any iterators except
    // for iterators that point to the element that is being erased.
    file_map.erase(filemap_iter++);
  }
  // Free memory used for the initial file map
  file_map.clear();

  // Show debug info about group map
  if (debug) {
    cout << "Debug: groupmap size: " << groupmap.size() << endl;
    cout << "Debug: groupmap max size: " << groupmap.max_size() << endl;
  }

  // Add the rules given on the command line

  // List of marking rules
  rulelist_t rulelist;

  for (vector<string>::iterator i = rule_args.begin(); i != rule_args.end();
       ++i) {
    try {
      rulelist.push_back(rule(regex(*i, regbase::perl | regbase::icase), *i));
    } catch (...) {
      cout << "Error: \"" << *i << "\" is not a valid regular expression"
           << endl;
      exit(1);
    }
  }

  // Interactive

  format file_row("%3d %c %12d %s");
  format rule_row("%3d %s");
  bool display_rules(!quiet);
  bool display_help(false);
  bool displaystatus(!quiet);
  bool cleangroups(true);
  bool did_delete(false);
  size_t walkpos(1);

  // Go to first group (the one with the biggest file sizes)
  groupwalker_t::iterator groupwalker_iter(groupwalker.begin());
  walkpos = 1;

  // Start interactive section
  for (;;) {
    // Remove groups with only one file
    if (cleangroups) {
      cleangroups = false;
      u32 cnt_removed(0);
      for (groupwalker_t::iterator iter(groupwalker.begin());
           iter != groupwalker.end();) {
        if (groupmap[*iter].size() <= 1) {
          groupmap.erase(*iter);
          groupwalker.erase(iter++);
          ++cnt_removed;
        }
        else {
          ++iter;
        }
      }
      // Set walker to first group
      groupwalker_iter = groupwalker.begin();
      walkpos = 1;
      verbose&& cnt_removed&& cout << "Removed " << cnt_removed
                                   << " groups with only one member" << endl;
    }

    // Exit if no more groups
    if (!groupmap.size()) {
      if (!quiet) {
        if (did_delete) {
          cout << "No more duplicates" << endl;
        }
        else {
          cout << "No duplicates found" << endl;
        }
      }
      break;
    }

    // Shortcuts for this group
    string groupkey(*groupwalker_iter);
    group_t& group(groupmap[groupkey]);

    if (displaystatus) {
      displaystatus = false;

      total_stats_t total_stats;
      for (groupmap_t::iterator groupmap_iter = groupmap.begin();
           groupmap_iter != groupmap.end(); ++groupmap_iter) {
        file_refs_t files;
        get_marked(
          groupmap_iter, groupmap_iter->second, rulelist, files, total_stats);
      }

      format status_row("%18d %s");
      cout << endl;
      cout << "Status:" << endl << endl;
      cout << str(status_row % total_stats.num_total % "files") << endl;
      cout << str(status_row % total_stats.num_dup % "duplicates") << endl;
      cout << str(status_row % total_stats.num_marked % "marked files") << endl;
      cout << str(status_row % groupmap.size() % "groups") << endl;
      //			cout << str(status_row % num_manual      % "manual groups") <<
      // endl;
      cout << str(status_row % total_stats.size_total % "bytes in all groups")
           << endl;
      cout << str(status_row % total_stats.size_dup % "bytes in duplicates")
           << endl;
      cout << str(
                status_row % total_stats.size_marked
                % "bytes in all marked files")
           << endl;
      // if (num_full_marked)
      //	cout << str(status_row % num_full_marked % "groups with ALL marked
      //(warning!)") << endl;
      cout << str(
                status_row
                % ((float)total_stats.num_total / (float)groupmap.size())
                % "files per group (average)")
           << endl;
    }

    if (display_help) {
      display_help = false;
      cout << "Interactive mode commands:" << endl << endl;
      cout << "f, first          : go to first group" << endl;
      cout << "l, last           : go to last group" << endl;
      cout << "p, previous       : go to previous group" << endl;
      cout << "n, next, <Enter>  : go to next group" << endl;
      //			cout << "t, toggle <index> : toggle mark on file with given index"
      //<< endl;
      cout << "c, clear          : remove all manual editing" << endl;
      cout << "r, rules          : list rules" << endl;
      cout
        << "a, add <rule>     : add rule (case insensitive regular expression)"
        << endl;
      cout << "b, file <file #>  : add rule (file number)" << endl;
      cout << "d, remove <index> : remove rule" << endl;
      cout << "h, help, ?        : display this message" << endl;
      cout << "s, status         : display status" << endl;
#ifdef WIN32
      cout << "o, open <index>   : open file" << endl;
#endif
      cout << "quit, exit        : exit program without deleting anything"
           << endl;
      cout << "delete            : delete all marked files" << endl;
      cout << endl;
    }

    // Display existing rules if previous command was a rule add or delete
    // command
    if (display_rules) {
      display_rules = false;
      cout << "Rules:" << endl << endl;
      if (!rulelist.size()) {
        cout << "No rules defined" << endl;
      }
      else {
        // Display existing rules
        int c(0);
        for (list<rule>::iterator iter = rulelist.begin();
             iter != rulelist.end(); ++iter) {
          cout << str(rule_row % ++c % iter->rx_str) << endl;
        }
      }
      cout << endl;
    }

    // Get command, or hardwire delete command if automatic mode
    string cmd, cmdarg;
    if (!automatic) {
      // Display list of files in group and mark status
      total_stats_t group_stats;
      groupmap_t::iterator dummy_iter = groupmap.begin();
      file_refs_t files;
      get_marked(dummy_iter, group, rulelist, files, group_stats);

      cout << endl;
      u32 cnt_files(0);
      for (group_t::iterator group_iter = group.begin();
           group_iter != group.end(); ++group_iter) {
        bool marked(
          find(files.begin(), files.end(), file_ref_t(dummy_iter, group_iter))
          != files.end());
        cout << str(
                  file_row % ++cnt_files % (marked ? '*' : ' ')
                  % group_iter->size % group_iter->path_inst.native())
             << endl;
      }

      cout << endl;
      format status_row("%18d %s");
      cout << str(status_row % group_stats.size_total % "bytes in group")
           << endl;
      cout << str(status_row % group_stats.size_dup % "bytes in duplicates")
           << endl;
      cout << str(
                status_row % group_stats.size_marked % "bytes in marked files")
           << endl;

      // Get command
      string cmdline;
      cout << endl;
      cout << walkpos << " / " << groupwalker.size() << " > " << flush;
      getline(cin, cmdline);
      // Split command into command and argument
      size_t space_idx = cmdline.find(" ");
      cmd = cmdline.substr(0, space_idx);
      trim(cmd);
      if (space_idx != -1) {
        cmdarg = cmdline.substr(space_idx);
        trim(cmdarg);
      }
    }
    else {
      // automatic
      cmd = "delete";
    }
    // display rules
    if (cmd == "r" || cmd == "rule") {
      // Flag display rules after all rules have been added
      display_rules = true;
    }
    // Add rule (regex)
    else if (cmd == "a" || cmd == "add") {
      if (cmdarg == "") {
        cout << "Error: Missing rule argument" << endl;
        display_help = true;
        continue;
      }
      try {
        rulelist.push_back(
          rule(regex(cmdarg, regbase::perl | regbase::icase), cmdarg));
      } catch (...) {
        cout << "Error: \"" << cmdarg
             << "\" is not a valid regular expression. Ignored." << endl;
        display_help = true;
        continue;
      }
      // Flag display rules after all rules have been added
      display_rules = true;
    }
    // Add rule (file #)
    else if (cmd == "b" || cmd == "file") {
      if (cmdarg == "") {
        cout << "Error: Missing index argument" << endl;
        display_help = true;
        continue;
      }
      u32 idx;
      try {
        idx = lexical_cast<u32>(cmdarg);
        if (idx < 1 || idx > group.size()) {
          throw(0);
        }
      } catch (...) {
        cout << "Error: \"" << cmdarg << "\" is not a valid index" << endl;
        display_help = true;
        continue;
      }
      // Since a list doesn't have random access, we walk out to the required
      // element
      group_t::iterator group_iter = group.begin();
      for (u32 d(0); d < idx - 1; ++d) {
        ++group_iter;
      }
      // Escape path_inst for use as regex
      regex esc("[\\^\\.\\$\\|\\(\\)\\[\\]\\*\\+\\?\\/\\\\]");
      string rep("\\\\\\1&");
      string escaped_path_inst(
        regex_replace(
          group_iter->path_inst.native(), esc, rep,
          match_default | format_sed));
      try {
        rulelist.push_back(
          rule(
            regex(escaped_path_inst, regbase::perl),
            string("Delete: ") + group_iter->path_inst.native()));
      } catch (...) {
        cout << "Error: \"" << cmdarg
             << "\" is not a valid regular expression. Ignored." << endl;
        display_help = true;
        continue;
      }
    }
    // Erase rule
    else if (cmd == "d" || cmd == "remove") {
      if (cmdarg == "") {
        cout << "Error: Missing index argument" << endl;
        display_help = true;
        continue;
      }
      u32 idx;
      try {
        idx = lexical_cast<u32>(cmdarg);
        if (idx < 1 || idx > rulelist.size()) {
          throw(0);
        }
      } catch (...) {
        cout << "Error: \"" << cmdarg << "\" is not a valid index" << endl;
        display_help = true;
        continue;
      }
      // Since a list doesn't have random access, we walk out to the required
      // element
      rulelist_t::iterator rule_iter = rulelist.begin();
      for (u32 d(0); d < idx - 1; ++d) {
        ++rule_iter;
      }
      rulelist.erase(rule_iter);
      // Flag display rules after all rules have been added
      display_rules = true;
    }
    // Go to next group
    else if (cmd == "n" || cmd == "next" || cmd == "") {
      ++groupwalker_iter;
      ++walkpos;
      if (groupwalker_iter == groupwalker.end()) {
        cout << "No more groups" << endl;
        --groupwalker_iter;
        --walkpos;
      }
    }
    // Go to previous group
    else if (cmd == "p" || cmd == "previous") {
      if (groupwalker_iter == groupwalker.begin()) {
        cout << "Already at first group" << endl;
      }
      else {
        --groupwalker_iter;
        --walkpos;
      }
    }
    // Go to first group
    else if (cmd == "f" || cmd == "first") {
      if (groupwalker_iter == groupwalker.begin()) {
        cout << "Already at first group" << endl;
      }
      else {
        groupwalker_iter = groupwalker.begin();
        walkpos = 1;
      }
    }
    // Go to last group
    else if (cmd == "" || cmd == "last") {
      if (walkpos == groupwalker.size()) {
        cout << "Already at last group" << endl;
      }
      else {
        groupwalker_iter = groupwalker.end();
        --groupwalker_iter;
        walkpos = groupwalker.size();
      }
    }
    // Display status
    else if (cmd == "s" || cmd == "status") {
      displaystatus = true;
    }
#ifdef WIN32
    // Open file
    else if (cmd == "o" || cmd == "open") {
      if (cmdarg == "") {
        cout << "Error: Missing index argument" << endl;
        display_help = true;
        continue;
      }
      try {
        int idx(1);
        int pidx(lexical_cast<int>(cmdarg));
        for (group_t::iterator group_iter = group.begin();
             group_iter != group.end(); ++group_iter) {
          if (idx == pidx) {
            stringstream s;
            s << "start /b \"" << group_iter->path_inst.native() << "\"";
            // system(s.str().c_str());
            break;
          }
          ++idx;
        }
        rulelist.push_back(
          rule(regex(cmdarg, regbase::perl | regbase::icase), cmdarg));
      } catch (...) {
        cout << "Error: \"" << cmdarg << "\" is not a valid index." << endl;
      }
    }
#endif
    // Delete files
    else if (cmd == "delete") {
      displaystatus = false;

      total_stats_t total_stats;
      file_refs_t files;
      for (groupmap_t::iterator groupmap_iter = groupmap.begin();
           groupmap_iter != groupmap.end(); ++groupmap_iter) {
        get_marked(
          groupmap_iter, groupmap_iter->second, rulelist, files, total_stats);
      }

      // Delete confirmation
      string cmdline("Y");
      if (!automatic && !quiet) {
        cout << endl;
        for (;;) {
          cout << "About to delete " << total_stats.num_marked << " files ("
               << total_stats.size_marked << " bytes) Delete? (y/n) > "
               << flush;
          getline(cin, cmdline);
          if (
            cmdline == "Y" || cmdline == "y" || cmdline == "N"
            || cmdline == "n")
            break;
        }
      }

      // Do delete
      if (cmdline == "Y" || cmdline == "y") {
        u64 file_cnt(0);
        u64 files_deleted(0);
        timer time;
        for (file_refs_t::iterator file_items_iter = files.begin();
             file_items_iter != files.end(); ++file_items_iter) {
          did_delete = true;
          path this_path_inst(file_items_iter->second->path_inst);
          try {
            if (!dry_run) {
              remove(file_items_iter->second->path_inst);
              ++files_deleted;
              verbose&& cout << "Deleted by rule: " << this_path_inst.native()
                             << endl;
            }
            else {
              cout << "Dry-run: Skipped delete: " << this_path_inst.native()
                   << endl;
            }
            // Delete from group (unless filesystem delete exception)
            file_items_iter->first->second.erase(file_items_iter->second);
          } catch (const filesystem_error& e) {
            cout << "Couldn't delete: " << this_path_inst.native() << endl;
            cout << "Cause: " << e.what() << endl;
          } catch (...) {
            cout << "Couldn't delete: " << this_path_inst.native() << endl;
            cout << "Cause: Unknown exception" << endl;
          }
          // Count files deleted
          ++file_cnt;
          // Display status if one second has elapsed or this is last iteration
          if (
            !quiet
            && (time.elapsed() >= 1.0 || file_cnt == total_stats.num_marked)) {
            cout << setprecision(2) << "Deleting: "
                 << (float)file_cnt / (float)total_stats.num_marked * 100
                 << "% (" << file_cnt << " / " << total_stats.num_marked
                 << " files)\n"
                 << "Failed: " << (file_cnt - files_deleted) << " files\n"
                 << endl;
            time.restart();
          }
        }
      }

      displaystatus = true;
      cleangroups = true;
      // If in automatic mode, exit here
      if (automatic)
        break;
    }
    // Display help
    else if (cmd == "h" || cmd == "help" || cmd == "?") {
      display_help = true;
    }
    // Quit
    else if (cmd == "quit" || cmd == "exit") {
      break;
    }
    // Unknown command
    else {
      cout << "Error: Unknown command" << endl;
      display_help = true;
    }
  } // for (;;)

  // Success
  exit(0);
}

// global count of number of files found for processing (in both folders and md5
// lists)
bool add_folder(
  const path& dir_path_inst, const bool recursive, const bool verbose,
  const bool quiet, u64 smaller, u64 bigger, bool debug)
{
  u64 file_cnt(0);
  timer time;
  bool res = add_folder_recursive(
    dir_path_inst, recursive, verbose, quiet, smaller, bigger, time, file_cnt);
  // Show debug info about file_map
  if (debug) {
    cout << "Debug: file_map size: " << file_map.size() << endl;
    cout << "Debug: file_map max size: " << file_map.max_size() << endl;
  }
  // Print final file count (was probably not printed since we only print a
  // count every second)
  cout << "Files: " << file_cnt << endl;
  return res;
}

// Recursively find file sizes and path_insts and store info in containers
bool add_folder_recursive(
  const path& dir_path_inst, const bool recursive, const bool verbose,
  const bool quiet, u64 smaller, u64 bigger, timer& time, u64& file_cnt)
{
  // Check that path_inst is valid and not a symlink
  if (is_symlink(dir_path_inst) || is_junction(dir_path_inst)) {
    verbose&& cout << "Skipped symlink: " << dir_path_inst.native() << endl;
    return false;
  }
  verbose&& cout << "Opening folder: " << dir_path_inst.native() << endl;
  try {
    directory_iterator end_itr;
    for (directory_iterator itr(dir_path_inst); itr != end_itr; ++itr) {
      path abs_path_inst(absolute(*itr));
      try {
        if (is_directory(abs_path_inst) && !is_symlink(abs_path_inst)) {
          if (recursive) {
            add_folder_recursive(
              *itr, recursive, verbose, quiet, smaller, bigger, time, file_cnt);
          }
        }
        else {
          if (is_regular_file(abs_path_inst)) {
            u64 size(file_size(abs_path_inst));
            // Filter small files if requested
            if (smaller != -1 && size <= smaller) {
              // for new boost
              verbose&& cout
                << "Skipped <= " << smaller << ": "
                << str(filesize_row % size % abs_path_inst.native()) << endl;
            }
            // Filter big files if requested
            else if (bigger != -1 && size >= bigger) {
              verbose&& cout
                << "Skipped >= " << bigger << ": "
                << str(filesize_row % size % abs_path_inst.native()) << endl;
            }
            else if (!boost::filesystem::is_regular_file(path(file))) {
              verbose&& cout << "Skipped non-regular file: " << file << endl;
            }
            else {
              // Check if file already is in map (this can happen if user messes
              // up the search folder arguments
              // or if some symlink og hardlink is followed (though I'm trying
              // to avoid that)
              bool found_path_inst(false);
              group_t& group(file_map[size]);
              for (group_t::iterator group_iter(group.begin());
                   group_iter != group.end(); ++group_iter) {
                if (group_iter->path_inst == abs_path_inst) {
                  verbose&& cout
                    << "Skipped redundant: "
                    << str(filesize_row % size % abs_path_inst.native())
                    << endl;
                  found_path_inst = true;
                  break;
                }
              }
              if (!found_path_inst) {
                // Add file to map
                group.push_back(dup_file(abs_path_inst, size, ""));
                verbose&& cout
                  << "Found: "
                  << str(filesize_row % size % abs_path_inst.native()) << endl;
              }
            }
          }
          ++file_cnt;
          // Display status if more than one second has elapsed
          if (!quiet && time.elapsed() >= 1.0) {
            cout << "Files: " << file_cnt << endl;
            time.restart();
          }
        }
      } catch (const filesystem_error& e) {
        cout << "Error processing: " << abs_path_inst.native() << endl;
        cout << "Cause: " << e.what() << endl;
      } catch (...) {
        cout << "Error processing: " << abs_path_inst.native() << endl;
        cout << "Cause: Unknown exception" << endl;
      }
    }
  } catch (const filesystem_error& e) {
    cout << "Error: Couldn't open folder: " << dir_path_inst.native() << endl;
    cout << "Cause: " << e.what() << endl;
  } catch (...) {
    cout << "Error: Couldn't open folder: " << dir_path_inst.native() << endl;
    cout << "Cause: Unknown exception" << endl;
  }
  return true;
}

// Add a list of files generated with md5deep or similar
//
// File format is one size, MD5 and full path_inst per line. Example:
//
// 43912  ccd6dad4b72d1255cf2e7a9dadd64083  C:\Documents and
// Settings\Administrator\Desktop\test.txt
bool add_md5list(
  const path& md5deep_file, const bool verbose, const bool quiet, u64 smaller,
  u64 bigger, bool debug)
{
  regex md5line(" *([0-9]+) +([0-9a-fA-F]{32}) +(.*)");
  boost::smatch what;
  timer time;
  u64 file_cnt(0);

  // Open file for reading
  std::wifstream fi(md5deep_file.native().c_str());
  if (!fi.good()) {
    cout << "Error: Couldn't open file" << endl;
    return false;
  }

  while (fi.good()) {
    // Count processed files
    file_cnt++;

    // Get line
    string line;
    // getline(fi, line); TODO
    trim(line);

    // Ignore empty lines
    if (line == "")
      continue;

    // Parse line into size, md5 and path_inst
    if (!regex_search(line, what, md5line)) {
      cout << "Error: Malformed line in md5 file:" << endl;
      cout << "\"" << line << "\"" << endl;
      continue;
    }
    string size_str(what[1].first, what[1].second);
    string md5(what[2].first, what[2].second);
    string file(what[3].first, what[3].second);

    u64 size = lexical_cast<u64>(size_str);

    // Filter small files if requested
    if (smaller != -1 && size <= smaller) {
      verbose&& cout << "Skipped <= " << smaller << ": "
                     << str(filesize_row % size % file) << endl;
    }
    // Filter big files if requested
    else if (bigger != -1 && size >= bigger) {
      verbose&& cout << "Skipped >= " << bigger << ": "
                     << str(filesize_row % size % file) << endl;
    }
    else if (!boost::filesystem::is_regular_file(path(file))) {
      verbose&& cout << "Skipped non-regular file: " << file << endl;
    }
    else {
      // Process file
      file_map[size].push_back(dup_file(path(file), size, md5));
      verbose&& cout << "Found: " << str(filesize_row % size % file) << endl;
    }
    // Display progress once every second
    if (!quiet && time.elapsed() >= 1.0) {
      cout << "Files: " << file_cnt << endl;
      time.restart();
    }
  }
  // Display final count (probably wasn't displayed above since we display only
  // once per second)
  if (!quiet) {
    cout << "Files: " << file_cnt++ << endl;
  }
  // Show debug info about file_map
  if (debug) {
    cout << "Debug: file_map size: " << file_map.size() << endl;
    cout << "Debug: file_map max size: " << file_map.max_size() << endl;
  }
  return true;
}

// Get all files that are marked in a group
void get_marked(
  groupmap_t::iterator& groupmap_iter, group_t& group, rulelist_t& rules,
  file_refs_t& files, total_stats_t& total_stats)
{
  u64 file_size(0);
  u32 num_marked(0);
  for (group_t::iterator group_iter = group.begin(); group_iter != group.end();
       ++group_iter) {
    // Update total stats
    if (!file_size) {
      file_size = group_iter->size;
    }
    // If we have marked all but one file in group, exit (no rule can mark all
    // files in one group)
    if (num_marked + 1 == group.size()) {
      break;
    }
    // Run all rules on group
    for (list<rule>::iterator rules_iter = rules.begin();
         rules_iter != rules.end(); ++rules_iter) {
      if (regex_search(group_iter->path_inst.native(), rules_iter->rx)) {
        ++num_marked;
        files.push_back(file_ref_t(groupmap_iter, group_iter));
        // Don't try any other rules when we get a match (would create duplicate
        // delete entries)
        break;
      }
    }
  }
  // Update total stats
  total_stats.num_total += (u32)group.size();
  total_stats.num_dup += (u32)group.size() - 1;
  total_stats.num_marked += num_marked;
  total_stats.size_total += group.size() * file_size;
  total_stats.size_dup += (group.size() - 1) * file_size;
  total_stats.size_marked += num_marked * file_size;
}

void parse_command_line(int argc, char* argv[])
{
  try {
    options_description desc("Duplex - Delete duplicate files - dahlsys.com");
    desc.add_options()("help,h", "produce help message")(
      "dry-run,d", bool_switch(&dry_run),
      "don't delete anything, just simulate")(
      "automatic,a", bool_switch(&automatic),
      "don't enter interactive mode (delete without confirmation)")(
      "filter-small,s", value<u64>(&smaller),
      "ignore files of this size and smaller")(
      "filter-big,b", value<u64>(&bigger),
      "ignore files of this size and bigger")(
      "quiet,q", bool_switch(&quiet), "display only error messages")(
      "verbose,v", bool_switch(&verbose), "display verbose messages")(
      "debug,e", bool_switch(&debug), "display debug / optimization info")(
      "md5,5", bool_switch(&use_md5),
      "use md5 cryptographic hash (fnv 64 bit hash is used by default)")(
      "rule,u", value<vector<string> >(&rule_args),
      "add marking rule (case insensitive regex)")(
      "rfolder,r", value<vector<string> >(&rfolder_args),
      "add recursive search folder")(
      "md5list,m", value<vector<string> >(&md5list_args),
      "add md5 list file (output from md5deep -zr)")(
      "folder,f", value<vector<string> >(&folder_args), "add search folder");

    positional_options_description p;
    p.add("rfolder", -1);

    variables_map vm;
    store(command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    notify(vm);

    // Display help and exit if required options (yes, I know) are missing
    if (
      vm.count("help") || (!folder_args.size() && !rfolder_args.size()
                           && !md5list_args.size())) {
      cout << desc << endl
           << "Arguments are equivalent to rfolder options" << endl;
      exit(1);
    }

    // Switch to md5 hashes if md5lists are used
    if (md5list_args.size()) {
      verbose&& cout << "Enabled md5 hashes due to md5list being used" << endl;
      use_md5 = true;
    }
  } catch (std::exception& e) {
    cerr << "Error: " << e.what() << endl;
    exit(1);
  } catch (...) {
    cerr << "Error: Unknown exception" << endl;
    ;
    exit(1);
  }
}

// Verify that all provided folder names have legal syntax and exist
void verify_valid_folders()
{
  vector<string>::iterator i;
  try {
    for (i = folder_args.begin(); i != folder_args.end(); ++i) {
      if (!exists(*i) || !is_directory(*i)) {
        throw 0;
      }
    }
    for (i = rfolder_args.begin(); i != rfolder_args.end(); ++i) {
      if (!exists(*i) || !is_directory(*i)) {
        throw 0;
      }
    }
    for (i = md5list_args.begin(); i != md5list_args.end(); ++i) {
      if (!exists(*i) || is_directory(*i)) {
        throw 0;
      }
    }
  } catch (...) {
    cout << "Error: Illegal path_inst: " << endl;
    cout << *i << endl;
    exit(1);
  }
}

// Comparison for dup_file pointers
bool compare_dup_file(dup_file* _first, dup_file* _second)
{
  return _first->path_inst.native() < _second->path_inst.native();
  // string first(_first->path_inst.native());
  // string second(_second->path_inst.native());
  // unsigned int i(0);
  // while ((i < first.length()) && (i < second.length())) {
  //  if (tolower(first[i]) < tolower(second[i])) {
  //    return true;
  //  }
  //  else if (tolower(first[i]) > tolower(second[i])) {
  //    return false;
  //  }
  //  ++i;
  //}
  // if (first.length()<second.length()) {
  //  return true;
  //}
  // return false;
}
