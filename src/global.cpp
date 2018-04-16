#include "pch.h"

#include "global.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef WIN32
#include <unistd.h>
#endif

using namespace std;

bool is_regular_file(const fs::path& ph)
{
  try {
    // We say that a zero byte file is not a regular file. This is a hack
    // because I'm not positive that I have handled all
    // read/permission/ownership errors that could cause a file that has data in
    // it to look like a zero byte file. If we were to process those, all the
    // "zero" byte files (except for one) would be marked for deletion.
    if (
      !fs::exists(ph) || ph.empty() || !fs::is_regular(ph)
      || fs::is_directory(ph) || fs::symbolic_link_exists(ph)
      || !fs::file_size(ph))
      return false;
  } catch (const fs::filesystem_error& e) {
    wcout << "Filesystem error while attempting to classify file:" << endl;
    //		wcout << ph.native() << " : " << endl;
    wcout << e.what() << endl;
    return false;
  }
  return true;
}
