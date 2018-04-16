#pragma once
//#pragma message("Compiling PCH - Should only happen once per project")

// misc
#define _CRT_SECURE_NO_DEPRECATE
#define _SCL_SECURE_NO_DEPRECATE
#include "targetver.h"

// Boost
#define BOOST_FILESYSTEM_VERSION 3
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/regex.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/timer.hpp>
#include <boost/scoped_ptr.hpp>

// Std
#include <iostream>
#include <iterator>
#include <fstream>
#include <list>
#include <map>

#ifdef WIN32
#include <hash_map>
#include <hash_set>
#else
#include <unordered_map>
//#include <ext/hash_map>
//#include <ext/hash_set>
#endif

#include <string>
#include <algorithm>
#include <vector>
#include <sstream>
#include <iomanip>
#include <locale>

// App
#include "int_types.h"
