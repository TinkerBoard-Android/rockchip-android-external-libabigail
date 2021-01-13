// -*- Mode: C++ -*-

// Copyright (C) 2013 Red Hat, Inc.
//
// This file is part of the GNU Application Binary Interface Generic
// Analysis and Instrumentation Library (libabigail).  This library is
// free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the
// Free Software Foundation; either version 3, or (at your option) any
// later version.

// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Lesser Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this program; see the file COPYING-LGPLV3.  If
// not, see <http://www.gnu.org/licenses/>.
//
// Author: Dodji Seketeli

#include <string>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include "abg-tools-utils.h"
#include "abg-diff-utils.h"
#include "test-utils.h"

using std::string;
using std::ofstream;
using std::cerr;

/// This structure describes the set of strings we want to diff
/// against each other as well as the reports we expect from these
/// diffs.
struct SESInOutSpec
{
  // This is the path to the report we expect from the diff algorithm
  // applied to second_string (below) diffed against first_string.
  const char* in_path;
  // This is the path where to store the report of the diff algorithm
  // applied to second_string (below) diffed against first_string.
  const char* out_path;
  // This is the first string to feed the diff algorithm with.
  const char* first_string;
  // This is the second string to feed the diff algorithm with.
  const char* second_string;
};// end struct SESInOutSpec

SESInOutSpec in_out_specs[] =
{
  {
    "data/test-core-diff/report0.txt",
    "output/test-core-diff/report0.txt",
    "abcabba",
    "cbabac"
  },
  {
    "data/test-core-diff/report1.txt",
    "output/test-core-diff/report1.txt",
    "xxabxx",
    "xbx"
  },
  {
    "data/test-core-diff/report2.txt",
    "output/test-core-diff/report2.txt",
    "xxabxx",
    "xbcx"
  },
  {
    "data/test-core-diff/report3.txt",
    "output/test-core-diff/report3.txt",
    "abc",
    "abdecfgc"
  },
  {
    "data/test-core-diff/report4.txt",
    "output/test-core-diff/report4.txt",
    "xxx",
    "xxx"
  },
  {
    "data/test-core-diff/report5.txt",
    "output/test-core-diff/report5.txt",
    "xabx",
    "xbx"
  },
  {
    "data/test-core-diff/report6.txt",
    "output/test-core-diff/report6.txt",
    "fou",
    "fubar"
  },
  // This should be the last entry.
  {NULL, NULL, NULL, NULL}
};

using abigail::diff_utils::edit_script;
using abigail::diff_utils::compute_ses;
using abigail::diff_utils::display_edit_script;

int
main()
{
  unsigned result = 1;

  bool problem = false;
  string in_path, out_path;

  for (SESInOutSpec *s = in_out_specs; s->in_path; ++s)
    {
      string input_suffix(s->in_path);
      in_path = abigail::tests::get_src_dir() + "/tests/" + input_suffix;

      string output_suffix(s->out_path);
      out_path = abigail::tests::get_build_dir() + "/tests/" + output_suffix;

      if (!abigail::tools::ensure_parent_dir_created(out_path))
	{
	  cerr << "Could not create parent director for " << out_path;
	  problem = true;
	  return result;
	}

      ofstream of(out_path.c_str(), std::ios_base::trunc);
      if (!of.is_open())
	{
	  cerr << "failed to read " << out_path << "\n";
	  problem = true;
	  continue;
	}

      edit_script ses;
      // Compute the Shortest Edit Script (aka diff) that changes
      // s->first_string into s->second_string.
      compute_ses(s->first_string, s->second_string, ses);
      // Emit a report about that edit script
      display_edit_script(ses, s->first_string, s->second_string, of);
      of.close();
      // Diff that report against what we expect.
      string cmd = "diff -u " + in_path + " " + out_path;
      if (system(cmd.c_str()))
	problem= true;
    }

  return problem;
}