// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2017 Red Hat, Inc.
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

#include "abg-comparison.h"
#include "abg-reporter.h"

#ifndef __ABG_REPORTER_PRIV_H__
#define __ABG_REPORTER_PRIV_H__

/// This is a subroutine of a *::report() function.
///
/// If the diff about two subjects S1 and S2 was reported earlier or
/// is being reported, emit a diagnostic message about this and return
/// from the current diff reporting function.
///
/// @param S1 the first diff subject to take in account.
///
/// @param S2 the second diff subject to take in account.
#define RETURN_IF_BEING_REPORTED_OR_WAS_REPORTED_EARLIER(S1, S2)	\
  do {									\
    if (diff_context_sptr ctxt = d.context())				\
      if (diff_sptr _diff_ = ctxt->get_canonical_diff_for(S1, S2))	\
	if (_diff_->currently_reporting() || _diff_->reported_once())	\
	  {								\
	    if (_diff_->currently_reporting())				\
	      out << indent << "details are being reported\n";		\
	    else							\
	      out << indent << "details were reported earlier\n";	\
	    return;							\
	  }								\
  } while (false)

/// This is a subroutine of a *::report() function.
///
/// If a given diff was reported earlier or is being reported, emit a
/// diagnostic message about this and return from the current diff
/// reporting function.
///
/// @param S1 the first diff subject to take in account.
///
/// @param S2 the second diff subject to take in account.
///
/// @param INTRO_TEXT the introductory text that precedes the
/// diagnostic.
#define RETURN_IF_BEING_REPORTED_OR_WAS_REPORTED_EARLIER2(D, INTRO_TEXT) \
  do {									\
    if (diff_sptr _diff_ = d.context()->get_canonical_diff_for(D))	\
      if (_diff_->currently_reporting() || _diff_->reported_once())	\
	{								\
	  string _name_ = _diff_->first_subject()->get_pretty_representation(); \
	  if (_diff_->currently_reporting())				\
	    out << indent << INTRO_TEXT << " '" << _name_ << "' changed; " \
	      "details are being reported\n";				\
	  else								\
	    out << indent << INTRO_TEXT << " '" << _name_ << "' changed, " \
	      "as reported earlier\n";					\
	  return ;							\
	}								\
} while (false)

/// This is a subroutine of a *::report() function.
///
/// If the diff about two subjects S1 and S2 was reported earlier or
/// is being reported, emit a diagnostic message about this and return
/// from the current diff reporting function.
///
///
/// @param INTRO_TEXT the introductory text that precedes the
/// diagnostic.
#define RETURN_IF_BEING_REPORTED_OR_WAS_REPORTED_EARLIER3(S1, S2, INTRO_TEXT) \
    do {								\
      if (diff_sptr _diff_ = d.context()->get_canonical_diff_for(S1, S2)) \
	if (_diff_->currently_reporting() || _diff_->reported_once())	\
	  {								\
	    string _name_ = _diff_->first_subject()->get_pretty_representation(); \
	    if (_diff_->currently_reporting())				\
	      out << indent << INTRO_TEXT << " '" << _name_ << "' changed; " \
		"details are being reported\n";				\
	    else							\
	      out << indent << INTRO_TEXT << " '" << _name_ << "' changed, " \
		"as reported earlier\n";				\
	    return ;							\
	  } \
    } while (false)

namespace abigail
{

namespace comparison
{

void
represent(const diff_context& ctxt,
	  method_decl_sptr mem_fn,
	  ostream& out);

void
represent_data_member(var_decl_sptr d,
		      const diff_context_sptr& ctxt,
		      ostream& out);

void
maybe_show_relative_offset_change(const var_diff_sptr &diff,
				  diff_context& ctxt,
				  ostream&	out);

void
maybe_show_relative_size_change(const var_diff_sptr	&diff,
				diff_context&		ctxt,
				ostream&		out);

void
represent(const var_diff_sptr	&diff,
	  diff_context_sptr	ctxt,
	  ostream&		out,
	  const string&	indent = "",
	  bool			local_only = false);

bool
report_size_and_alignment_changes(type_or_decl_base_sptr	first,
				  type_or_decl_base_sptr	second,
				  diff_context_sptr		ctxt,
				  ostream&			out,
				  const string&		indent,
				  bool				nl);

bool
report_loc_info(const type_or_decl_base_sptr& tod,
		const diff_context& ctxt,
		ostream &out);

bool
report_name_size_and_alignment_changes(decl_base_sptr		first,
				       decl_base_sptr		second,
				       diff_context_sptr	ctxt,
				       ostream&		out,
				       const string&		indent,
				       bool			nl);

/// Represent the kind of difference we want report_mem_header() to
/// report.
enum diff_kind
{
  del_kind,
  ins_kind,
  subtype_change_kind,
  change_kind
};

void
report_mem_header(ostream& out,
		  diff_kind k,
		  const string& section_name,
		  const string& indent);

void
report_mem_header(ostream& out,
		  size_t number,
		  size_t num_filtered,
		  diff_kind k,
		  const string& section_name,
		  const string& indent);

bool
maybe_report_diff_for_member(const decl_base_sptr&	decl1,
			     const decl_base_sptr&	decl2,
			     const diff_context_sptr&	ctxt,
			     ostream&			out,
			     const string&		indent);

bool
maybe_report_diff_for_symbol(const elf_symbol_sptr&	symbol1,
			     const elf_symbol_sptr&	symbol2,
			     ostream&			out,
			     const string&		indent);

void
show_linkage_name_and_aliases(ostream& out,
			      const string& indent,
			      const elf_symbol& symbol,
			      const string_elf_symbols_map_type& sym_map);

void
maybe_report_interfaces_impacted_by_diff(const diff		*d,
					 ostream		&out,
					 const string		&indent,
					 bool new_line_prefix = true);

void
maybe_report_interfaces_impacted_by_diff(const diff_sptr	&d,
					 ostream		&out,
					 const string		&indent,
					 bool new_line_prefix = false);

} // end namespace comparison
} // end namespace abigail

#endif // __ABG_REPORTER_PRIV_H__