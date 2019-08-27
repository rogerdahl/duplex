#pragma once
//#pragma message("Compiling PCH - Should only happen once per project")

// Local libraries
#define _CRT_SECURE_NO_DEPRECATE
#define _SCL_SECURE_NO_DEPRECATE
#include "targetver.h"
#include <fmt/format.h>
#include <fmt/ostream.h>
//#include <spdlog/spdlog.h>

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
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// App
#include "int_types.h"
