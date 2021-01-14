// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2016 Red Hat, Inc.
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

/// @file read an XML corpus file (in the native Abigail XML format),
/// save it back and diff the resulting XML file against the input
/// file.  They should be identical.

#include <string>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include "abg-ir.h"
#include "abg-reader.h"
#include "abg-writer.h"
#include "abg-tools-utils.h"
#include "test-utils.h"

using std::string;
using std::ofstream;
using std::cerr;

using abigail::tools_utils::file_type;
using abigail::tools_utils::check_file;
using abigail::tools_utils::guess_file_type;
using abigail::tests::get_build_dir;
using abigail::ir::environment;
using abigail::ir::environment_sptr;
using abigail::translation_unit_sptr;
using abigail::corpus_sptr;
using abigail::xml_reader::read_translation_unit_from_file;
using abigail::xml_reader::read_corpus_from_native_xml_file;
using abigail::xml_writer::write_translation_unit;
using abigail::xml_writer::write_corpus_to_native_xml;

/// This is an aggregate that specifies where a test shall get its
/// input from, and where it shall write its ouput to.
struct InOutSpec
{
  const char* in_path;
  const char* in_suppr_spec_path;
  const char* ref_out_path;
  const char* out_path;
};// end struct InOutSpec


InOutSpec in_out_specs[] =
{
  {
    "data/test-read-write/test0.xml",
    "",
    "data/test-read-write/test0.xml",
    "output/test-read-write/test0.xml"
  },
  {
    "data/test-read-write/test1.xml",
    "",
    "data/test-read-write/test1.xml",
    "output/test-read-write/test1.xml"
  },
  {
    "data/test-read-write/test2.xml",
    "",
    "data/test-read-write/test2.xml",
    "output/test-read-write/test2.xml"
  },
  {
    "data/test-read-write/test3.xml",
    "",
    "data/test-read-write/test3.xml",
    "output/test-read-write/test3.xml"
  },
  {
    "data/test-read-write/test4.xml",
    "",
    "data/test-read-write/test4.xml",
    "output/test-read-write/test4.xml"
  },
  {
    "data/test-read-write/test5.xml",
    "",
    "data/test-read-write/test5.xml",
    "output/test-read-write/test5.xml"
  },
  {
    "data/test-read-write/test6.xml",
    "",
    "data/test-read-write/test6.xml",
    "output/test-read-write/test6.xml"
  },
  {
    "data/test-read-write/test7.xml",
    "",
    "data/test-read-write/test7.xml",
    "output/test-read-write/test7.xml"
  },
  {
    "data/test-read-write/test8.xml",
    "",
    "data/test-read-write/test8.xml",
    "output/test-read-write/test8.xml"
  },
  {
    "data/test-read-write/test9.xml",
    "",
    "data/test-read-write/test9.xml",
    "output/test-read-write/test9.xml"
  },
  {
    "data/test-read-write/test10.xml",
    "",
    "data/test-read-write/test10.xml",
    "output/test-read-write/test10.xml"
  },
  {
    "data/test-read-write/test11.xml",
    "",
    "data/test-read-write/test11.xml",
    "output/test-read-write/test11.xml"
  },
  {
    "data/test-read-write/test12.xml",
    "",
    "data/test-read-write/test12.xml",
    "output/test-read-write/test12.xml"
  },
  {
    "data/test-read-write/test13.xml",
    "",
    "data/test-read-write/test13.xml",
    "output/test-read-write/test13.xml"
  },
  {
    "data/test-read-write/test14.xml",
    "",
    "data/test-read-write/test14.xml",
    "output/test-read-write/test14.xml"
  },
  {
    "data/test-read-write/test15.xml",
    "",
    "data/test-read-write/test15.xml",
    "output/test-read-write/test15.xml"
  },
  {
    "data/test-read-write/test16.xml",
    "",
    "data/test-read-write/test16.xml",
    "output/test-read-write/test16.xml"
  },
  {
    "data/test-read-write/test17.xml",
    "",
    "data/test-read-write/test17.xml",
    "output/test-read-write/test17.xml"
  },
  {
    "data/test-read-write/test18.xml",
    "",
    "data/test-read-write/test18.xml",
    "output/test-read-write/test18.xml"
  },
  {
    "data/test-read-write/test19.xml",
    "",
    "data/test-read-write/test19.xml",
    "output/test-read-write/test19.xml"
  },
  {
    "data/test-read-write/test20.xml",
    "",
    "data/test-read-write/test20.xml",
    "output/test-read-write/test20.xml"
  },
  {
    "data/test-read-write/test21.xml",
    "",
    "data/test-read-write/test21.xml",
    "output/test-read-write/test21.xml"
  },
  {
    "data/test-read-write/test22.xml",
    "",
    "data/test-read-write/test22.xml",
    "output/test-read-write/test22.xml"
  },
  {
    "data/test-read-write/test23.xml",
    "",
    "data/test-read-write/test23.xml",
    "output/test-read-write/test23.xml"
  },
  {
    "data/test-read-write/test24.xml",
    "",
    "data/test-read-write/test24.xml",
    "output/test-read-write/test24.xml"
  },
  {
    "data/test-read-write/test25.xml",
    "",
    "data/test-read-write/test25.xml",
    "output/test-read-write/test25.xml"
  },
  {
    "data/test-read-write/test26.xml",
    "",
    "data/test-read-write/test26.xml",
    "output/test-read-write/test26.xml"
  },
  {
    "data/test-read-write/test27.xml",
    "",
    "data/test-read-write/test27.xml",
    "output/test-read-write/test27.xml"
  },
  {
    "data/test-read-write/test28.xml",
    "data/test-read-write/test28-drop-std-fns.abignore",
    "data/test-read-write/test28-without-std-fns-ref.xml",
    "output/test-read-write/test28-without-std-fns.xml"
  },
  {
    "data/test-read-write/test28.xml",
    "data/test-read-write/test28-drop-std-vars.abignore",
    "data/test-read-write/test28-without-std-vars-ref.xml",
    "output/test-read-write/test28-without-std-vars.xml"
  },
  // This should be the last entry.
  {NULL, NULL, NULL, NULL}
};

/// Walk the array of InOutSpecs above, read the input files it points
/// to, write it into the output it points to and diff them.
int
main()
{
  unsigned result = 1;

  bool is_ok = true;
  string in_path, out_path, in_suppr_spec_path, ref_out_path;
  for (InOutSpec* s = in_out_specs; s->in_path; ++s)
    {
      string input_suffix(s->in_path);
      in_path =
	string(abigail::tests::get_src_dir()) + "/tests/" + input_suffix;

      if (!check_file(in_path, cerr))
	return true;

      string ref_out_path_suffix(s->ref_out_path);
      ref_out_path =
	string(abigail::tests::get_src_dir())
	+ "/tests/" + ref_out_path_suffix;

      if (!check_file(ref_out_path, cerr))
	return true;

      if (s->in_suppr_spec_path && strcmp(s->in_suppr_spec_path, ""))
	{
	  in_suppr_spec_path = string(s->in_suppr_spec_path);
	  in_suppr_spec_path =
	    string(abigail::tests::get_src_dir())
	    + "/tests/"
	    + in_suppr_spec_path;
	}
      else
	in_suppr_spec_path.clear();

      environment_sptr env(new environment);
      translation_unit_sptr tu;
      corpus_sptr corpus;

      file_type t = guess_file_type(in_path);
      if (t == abigail::tools_utils::FILE_TYPE_UNKNOWN)
	{
	  cerr << in_path << "is an unknown file type\n";
	  is_ok = false;
	  continue;
	}

      string output_suffix(s->out_path);
      out_path =
	string(abigail::tests::get_build_dir()) + "/tests/" + output_suffix;
      if (!abigail::tools_utils::ensure_parent_dir_created(out_path))
	{
	  cerr << "Could not create parent director for " << out_path;
	  is_ok = false;
	  return result;
	}

      string abilint = string(get_build_dir()) + "/tools/abilint";
      if (!in_suppr_spec_path.empty())
	abilint +=string(" --suppr ") + in_suppr_spec_path;
      string cmd = abilint + " " + in_path + " > " + out_path;

      if (system(cmd.c_str()))
	{
	  cerr << "ABI XML file doesn't pass abilint: " << out_path << "\n";
	  is_ok &= false;
	}

      cmd = "diff -u " + ref_out_path + " " + out_path;
      if (system(cmd.c_str()))
	is_ok &= false;
    }

  return !is_ok;
}
