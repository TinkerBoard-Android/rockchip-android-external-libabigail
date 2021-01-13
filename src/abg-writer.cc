// -*- mode: C++ -*-
// Copyright (C) 2013 Free Software Foundation, Inc.
//
// This file is part of the GNU Application Binary Interface Generic
// Analysis and Instrumentation Library (libabigail).  This library is
// free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any
// later version.

// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License
// and a copy of the GCC Runtime Library Exception along with this
// program; see the files COPYING3 and COPYING.RUNTIME respectively.
// If not, see <http://www.gnu.org/licenses/>.

/// @file

#include <ostream>
#include <sstream>
#include <tr1/memory>
#include <tr1/unordered_map>
#include "abg-writer.h"
#include "abg-config.h"

using std::tr1::shared_ptr;
using std::tr1::dynamic_pointer_cast;
using std::tr1::static_pointer_cast;
using std::ostream;
using std::ostringstream;
using std::list;
using std::tr1::unordered_map;

namespace abigail
{
namespace writer
{

class id_manager
{

  unsigned long long
  get_new_id()
  {
    return ++m_cur_id;
  }

public:
  id_manager()
    : m_cur_id(0)
  {
  }

  /// Return a unique string representing a numerical id.
  string
  get_id()
  {
    ostringstream o;
    o << get_new_id();
    return o.str();
  }

  /// Return a unique string representing a numerical ID, prefixed by
  /// #prefix.
  ///
  /// \param prefix the prefix of the returned unique id.
  string
  get_id_with_prefix(const string& prefix)
  {
  ostringstream o;
    o << prefix << get_new_id();
    return o.str();
  }

private:
  unsigned long long m_cur_id;
};//end class id_manager

typedef unordered_map<shared_ptr<type_base>,
		      string,
		      type_shared_ptr_hash,
		      type_shared_ptr_equal> type_shared_ptr_map;
class write_context
{
  write_context();

public:

  write_context(ostream& os)
    : m_ostream(os)
  {
  }

  const config&
  get_config() const
  {
    return m_config;
  }

  ostream&
  get_ostream()
  {
    return m_ostream;
  }

  id_manager&
  get_id_manager()
  {
    return m_id_manager;
  }

  /// Associate a unique id to a given type.  For that, put the type
  /// in a hash table, hashing the type.  So if the type has no id
  /// associated to it, create a new one and return it.  Otherwise,
  /// return the existing id for that type.
  string
  get_id_for_type(shared_ptr<type_base> t)
  {
    type_shared_ptr_map::const_iterator it = m_type_id_map.find(t);
    string id;
    if ( it == m_type_id_map.end())
      {
	string id = get_id_manager().get_id_with_prefix("type-id-");
	m_type_id_map[t] = id;
	return id;
      }
    else
      return m_type_id_map[t];
  }

private:
  id_manager m_id_manager;
  config m_config;
  ostream& m_ostream;
  type_shared_ptr_map m_type_id_map;
};//end write_context

static bool write_translation_unit(const translation_unit&,
				   write_context&,
				   unsigned);
static void write_location(location, translation_unit&, ostream&);
static void write_location(const shared_ptr<decl_base>&, ostream&);
static bool write_visibility(const shared_ptr<decl_base>&, ostream&);
static bool write_binding(const shared_ptr<decl_base>&, ostream&);
static void write_size_and_alignment(const shared_ptr<type_base>, ostream&);
static void write_access(class_decl::access_specifier, ostream&);
static void write_access(shared_ptr<class_decl::member>, ostream&);
static bool write_decl(const shared_ptr<decl_base>,
		       write_context&, unsigned);
static bool write_type_decl(const shared_ptr<type_decl>,
			    write_context&, unsigned);
static bool write_namespace_decl(const shared_ptr<namespace_decl>,
				 write_context&, unsigned);
static bool write_qualified_type_def(const shared_ptr<qualified_type_def>,
				     write_context&, unsigned);
static bool write_pointer_type_def(const shared_ptr<pointer_type_def>,
				   write_context&, unsigned);
static bool write_reference_type_def(const shared_ptr<reference_type_def>,
				     write_context&, unsigned);
static bool write_enum_type_decl(const shared_ptr<enum_type_decl>,
				 write_context&, unsigned);
static bool write_typedef_decl(const shared_ptr<typedef_decl>,
			       write_context&, unsigned);
static bool write_var_decl(const shared_ptr<var_decl>,
			   write_context&, unsigned);
static bool write_function_decl(const shared_ptr<function_decl>,
				write_context&, unsigned);
static bool write_class_decl(const shared_ptr<class_decl>,
			     write_context&, unsigned);
static void	do_indent(ostream&, unsigned);
static void	do_indent_to_level(write_context&, unsigned, unsigned);
static unsigned get_indent_to_level(write_context&, unsigned, unsigned);

/// Emit #nb_whitespaces white spaces into the output stream #o.
void
do_indent(ostream& o, unsigned nb_whitespaces)
{
  for (unsigned i = 0; i < nb_whitespaces; ++i)
    o << ' ';
}

/// Indent #initial_indent + level number of xml element indentation.
///
/// \param ctxt the context of the parsing.
///
/// \param initial_indent the initial number of white space to indent to.
///
/// \param level the number of indentation level to indent to.
static void
do_indent_to_level(write_context&	ctxt,
		   unsigned		initial_indent,
		   unsigned		level)
{
  do_indent(ctxt.get_ostream(),
	    get_indent_to_level(ctxt, initial_indent, level));
}

/// Return the number of white space of indentation that
/// #do_indent_to_level would have used.
///
/// \param ctxt the context of the parsing.
///
/// \param initial_indent the initial number of white space to indent to.
///
/// \param level the number of indentation level to indent to.
static unsigned
get_indent_to_level(write_context& ctxt,
		    unsigned initial_indent,
		    unsigned level)
{
    int nb_ws = initial_indent +
      level * ctxt.get_config().get_xml_element_indent();
    return nb_ws;
}

/// Serialize a translation_unit into an output stream.
///
/// \param tu the translation unit to serialize.
///
/// \param out the output stream.
///
/// \return true upon successful completion, false otherwise.
bool
write_to_ostream(const translation_unit& tu,
		 ostream &out)
{
  write_context ctxt(out);

  return write_translation_unit(tu, ctxt, /*indent=*/0);
}

/// Write a location to the output stream.
///
/// If the location is empty, nothing is written.
///
/// \param loc the location to consider.
///
/// \param tu the translation unit the location belongs to.
///
/// \param o the output stream to write to.
static void
write_location(location		loc,
	       translation_unit&	tu,
	       ostream&		o)
{
  if (!loc)
    return;

  string filepath;
  unsigned line = 0, column = 0;

  tu.get_loc_mgr().expand_location(loc, filepath, line, column);

  o << " filepath='" << filepath << "'"
    << " line='"     << line     << "'"
    << " column='"   << column   << "'";
}

/// Write the location of a decl to the output stream.
///
/// If the location is empty, nothing is written.
///
/// \param decl the decl to consider.
///
/// \param o the output stream to write to.
static void
write_location(const shared_ptr<decl_base>&	decl,
	       ostream&			o)
{
  if (!decl)
    return;

  location loc = decl->get_location();
  if (!loc)
    return;

  string filepath;
  unsigned line = 0, column = 0;
  translation_unit& tu = *get_translation_unit(decl);

  tu.get_loc_mgr().expand_location(loc, filepath, line, column);

  o << " filepath='" << filepath << "'"
    << " line='"     << line     << "'"
    << " column='"   << column   << "'";
}

/// Serialize the visibility property of the current decl as the
/// 'visibility' attribute for the current xml element.
///
/// \param decl the instance of decl_base to consider.
///
/// \param o the output stream to serialize the property to.
///
/// \return true upon successful completion, false otherwise.
static bool
write_visibility(const shared_ptr<decl_base>&	decl,
		 ostream&			o)
{
  if (!decl)
    return false;

  decl_base::visibility v = decl->get_visibility();
  string str;

  switch (v)
    {
    case decl_base::VISIBILITY_NONE:
      return true;
    case decl_base::VISIBILITY_DEFAULT:
      str = "default";
      break;
    case decl_base::VISIBILITY_PROTECTED:
      str = "protected";
      break;
    case decl_base::VISIBILITY_HIDDEN:
      str = "hidden";
      break;
    case decl_base::VISIBILITY_INTERNAL:
	str = "internal";
	break;
    }

  if (str.empty())
    return false;

  o << " visibility='" << str << "'";

  return true;
}

/// Serialize the 'binding' property of the current decl.
///
/// \param decl the decl to consider.
///
/// \param o the output stream to serialize the property to.
static bool
write_binding(const shared_ptr<decl_base>&	decl,
	      ostream&				o)
{
  if (!decl)
    return false;

  decl_base::binding bind = decl_base::BINDING_NONE;

  shared_ptr<var_decl> var =
    dynamic_pointer_cast<var_decl>(decl);
  if (var)
    bind = var->get_binding();
  else
    {
      shared_ptr<function_decl> fun =
	dynamic_pointer_cast<function_decl>(decl);
      if (fun)
	bind = fun->get_binding();
    }

  string str;
  switch (bind)
    {
    case decl_base::BINDING_NONE:
      break;
    case decl_base::BINDING_LOCAL:
      str = "local";
      break;
    case decl_base::BINDING_GLOBAL:
	str = "global";
      break;
    case decl_base::BINDING_WEAK:
      str = "weak";
      break;
    }

  if (!str.empty())
    o << " binding='" << str << "'";

  return true;
}

/// Serialize the size and alignment attributes of a given type.
///
/// \param decl the type to consider.
///
/// \param o the output stream to serialize to.
static void
write_size_and_alignment(const shared_ptr<type_base> decl,
			 ostream& o)
{
  size_t size_in_bits = decl->get_size_in_bits();
  if (size_in_bits)
    o << " size-in-bits='" << size_in_bits << "'";

  size_t alignment_in_bits = decl->get_alignment_in_bits();
  if (alignment_in_bits)
    o << " alignment-in-bits='" << alignment_in_bits << "'";
}

/// Serialize the access specifier.
///
/// \param a the access specifier to serialize.
///
/// \param o the output stream to serialize it to.
static void
write_access(class_decl::access_specifier a, ostream& o)
{
  string access_str = "private";

  switch (a)
    {
    case class_decl::private_access:
      access_str = "private";
      break;

    case class_decl::protected_access:
      access_str = "protected";
      break;

    case class_decl::public_access:
      access_str = "public";
      break;

    default:
      break;
    }

  o << " access='" << access_str << "'";
}

/// Serialize the access specifier of a class member.
///
/// \param member a pointer to the class member to consider.
///
/// \param o the ostream to serialize the member to.
static void
write_access(shared_ptr<class_decl::member> member,
	     ostream& o)
{
  write_access(member->get_access_specifier(), o);
}

/// Serialize a pointer to an of decl_base into an output stream.
///
/// \param decl, the pointer to decl_base to serialize
///
/// \param ctxt the context of the serialization.  It contains e.g, the
/// output stream to serialize to.
///
/// \param indent how many indentation spaces to use during the
/// serialization.
///
/// \return true upon successful completion, false otherwise.
static bool
write_decl(const shared_ptr<decl_base>	decl,
	   write_context&		ctxt,
	   unsigned			indent)
{
  if (write_type_decl(dynamic_pointer_cast<type_decl> (decl),
		      ctxt, indent)
      || write_namespace_decl(dynamic_pointer_cast<namespace_decl>(decl),
			      ctxt, indent)
      || write_qualified_type_def (dynamic_pointer_cast<qualified_type_def>
				   (decl),
				   ctxt, indent)
      || write_pointer_type_def(dynamic_pointer_cast<pointer_type_def>(decl),
				ctxt, indent)
      || write_reference_type_def(dynamic_pointer_cast
				  <reference_type_def>(decl),
				  ctxt, indent)
      || write_enum_type_decl(dynamic_pointer_cast<enum_type_decl>(decl),
			      ctxt, indent)
      || write_typedef_decl(dynamic_pointer_cast<typedef_decl>(decl),
			    ctxt, indent)
      || write_var_decl(dynamic_pointer_cast<var_decl>(decl),
			ctxt, indent)
      || write_function_decl(dynamic_pointer_cast<function_decl>(decl),
			     ctxt, indent)
      || write_class_decl(dynamic_pointer_cast<class_decl>(decl),
			  ctxt, indent))
    return true;

  return false;
}

/// Serialize a translation unit into an output stream.
///
/// \param tu the translation unit to serialize.
///
/// \param ctxt the context of the serialization.  It contains e.g,
/// the output stream to serialize to.
///
/// \param indent how many indentation spaces to use during the
/// serialization.
///
/// \return true upon successful completion, false otherwise.
static bool
write_translation_unit(const translation_unit&	tu,
		       write_context&		ctxt,
		       unsigned		indent)
{
  ostream &o = ctxt.get_ostream();
  const config &c = ctxt.get_config();

  do_indent(o, indent);

  o << "<abi-instr version='"
    << static_cast<int> (c.get_format_major_version_number())
    << "." << static_cast<int>(c.get_format_minor_version_number())
    << "'";

  if (tu.is_empty())
    {
      o << "/>";
      return true;
    }
  else
    o << ">";

  for (translation_unit::decls_type::const_iterator i =
	 tu.get_global_scope()->get_member_decls().begin();
       i != tu.get_global_scope()->get_member_decls().end();
       ++i)
    {
      o << "\n";
      write_decl(*i, ctxt, indent + c.get_xml_element_indent());
    }

  o << "\n";
  do_indent(o, indent);
  o << "</abi-instr>\n";

  return true;
}

/// Serialize a pointer to an instance of basic type declaration, into
/// an output stream.
///
/// \param d the basic type declaration to serialize.
///
/// \param ctxt the context of the serialization.  It contains e.g, the
/// output stream to serialize to.
///
/// \param indent how many indentation spaces to use during the
/// serialization.
///
/// \return true upon successful completion, false otherwise.
static bool
write_type_decl(const shared_ptr<type_decl>	d,
		write_context&			ctxt,
		unsigned			indent)
{
  if (!d)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<type-decl name='" << d->get_name() << "'";

  write_size_and_alignment(d, o);

  write_location(d, o);

  o << " id='" << ctxt.get_id_for_type(d) << "'" <<  "/>";

  return true;
}

/// Serialize a namespace declaration int an output stream.
///
/// \param decl the namespace declaration to serialize.
///
/// \param ctxt the context of the serialization.  It contains e.g, the
/// output stream to serialize to.
///
/// \param indent how many indentation spaces to use during the
/// serialization.
///
/// \return true upon successful completion, false otherwise.
static bool
write_namespace_decl(const shared_ptr<namespace_decl>	decl,
		     write_context&			ctxt,
		     unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();
  const config &c = ctxt.get_config();

  do_indent(o, indent);

  o << "<namespace-decl name='" << decl->get_name() << "'>";

  for (list<shared_ptr<decl_base> >::const_iterator i =
	 decl->get_member_decls ().begin();
       i != decl->get_member_decls ().end();
       ++i)
    {
      o << "\n";
      write_decl(*i, ctxt, indent + c.get_xml_element_indent());
    }

  o << "\n";
  do_indent(o, indent);
  o << "</namespace-decl>";

  return true;
}

/// Serialize a qualified type declaration to an output stream.
///
/// \param decl the qualfied type declaration to write.
///
/// \param ctxt the write context.
///
/// \param indent the number of space to indent to during the
/// serialization.
///
/// \return true upon successful completion, false otherwise.
static bool
write_qualified_type_def(const shared_ptr<qualified_type_def>	decl,
			 write_context&			ctxt,
			 unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<qualified-type-def type-id='"
    << ctxt.get_id_for_type(decl->get_underlying_type())
    << "'";

  if (decl->get_cv_quals() & qualified_type_def::CV_CONST)
    o << " const='yes'";
  if (decl->get_cv_quals() & qualified_type_def::CV_VOLATILE)
    o << " volatile='yes'";

  write_location(static_pointer_cast<decl_base>(decl), o);

  o<< " id='"
    << ctxt.get_id_for_type(decl)
    << "'";

  o << "/>";

  return true;
}

/// Serialize a pointer to an instance of pointer_type_def.
///
/// \param decl the pointer_type_def to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_pointer_type_def(const shared_ptr<pointer_type_def>	decl,
		       write_context&				ctxt,
		       unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<pointer-type-def type-id='"
    << ctxt.get_id_for_type(decl->get_pointed_to_type())
    << "'";

  write_size_and_alignment(decl, o);

  o << " id='" << ctxt.get_id_for_type(decl) << "'";

  write_location(static_pointer_cast<decl_base>(decl), o);
  o << "/>";

  return true;
}

/// Serialize a pointer to an instance of reference_type_def.
///
/// \param decl the reference_type_def to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_reference_type_def(const shared_ptr<reference_type_def>	decl,
			 write_context&			ctxt,
			 unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<reference-type-def kind='";
  if (decl->is_lvalue())
    o << "lvalue";
  else
    o << "rvalue";
  o << "'";

  o << " type-id='" << ctxt.get_id_for_type(decl->get_pointed_to_type()) << "'";

  write_size_and_alignment(decl, o);

  o << " id='" << ctxt.get_id_for_type(decl) << "'";

  write_location(static_pointer_cast<decl_base>(decl), o);

  o << "/>";
  return true;
}

/// Serialize a pointer to an instance of enum_type_decl.
///
/// \param decl the enum_type_decl to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_enum_type_decl(const shared_ptr<enum_type_decl>	decl,
		     write_context&			ctxt,
		     unsigned				indent)
{
    if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);
  o << "<enum-decl name='" << decl->get_name() << "'";

  write_location(static_pointer_cast<decl_base>(decl), o);

  o << " id='" << ctxt.get_id_for_type(decl) << "'>\n";

  do_indent(o, indent + ctxt.get_config().get_xml_element_indent());
  o << "<underlying-type type-id='"
    << ctxt.get_id_for_type(decl->get_underlying_type())
    << "'/>\n";

  for (list<enum_type_decl::enumerator>::const_iterator i =
	 decl->get_enumerators().begin();
       i != decl->get_enumerators().end();
       ++i)
    {
      do_indent(o, indent + ctxt.get_config().get_xml_element_indent());
      o << "<enumerator name='"
	<< i->get_name()
	<< "' value='"
	<< i->get_value()
	<< "'/>\n";
    }

  do_indent(o, indent);
  o << "</enum-decl>";

  return true;
}

/// Serialize a pointer to an instance of typedef_decl.
///
/// \param decl the typedef_decl to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_typedef_decl(const shared_ptr<typedef_decl>	decl,
		   write_context&			ctxt,
		   unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<typedef-decl name='" << decl->get_name() << "'";

  o << " type-id='" << ctxt.get_id_for_type(decl->get_underlying_type()) << "'";

  write_location(decl, o);

  o << " id='"
    << ctxt.get_id_for_type(decl)
    << "'/>";

  return true;
}

/// Serialize a pointer to an instances of var_decl.
///
/// \param decl the var_decl to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_var_decl(const shared_ptr<var_decl>	decl,
	       write_context&			ctxt,
	       unsigned			indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<var-decl name='" << decl->get_name() << "'";
  o << " type-id='" << ctxt.get_id_for_type(decl->get_type()) << "'";

  const string& mangled_name = decl->get_mangled_name();

  if (!mangled_name.empty())
    o << " mangled-name='" << mangled_name << "'";

  write_visibility(decl, o);

  write_binding(decl, o);

  write_location(decl, o);

  o << "/>";

  return true;
}

/// Serialize a pointer to a function_decl.
///
/// \param decl the pointer to function_decl to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the number of indentation white spaces to use.
///
/// \return true upon succesful completion, false otherwise.
static bool
write_function_decl(const shared_ptr<function_decl>	decl,
		    write_context&			ctxt,
		    unsigned				indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent(o, indent);

  o << "<function-decl name='" << decl->get_name() << "'";

  if (!decl->get_mangled_name().empty())
    o << " mangled-name='" << decl->get_mangled_name() << "'";

  write_location(decl, o);

  if (decl->is_declared_inline())
    o << " declared-inline='yes'";

  write_visibility(decl, o);

  write_binding(decl, o);

  o << ">\n";

  std::list<shared_ptr<function_decl::parameter> >::const_iterator pi;
  for (pi = decl->get_parameters().begin();
       pi != decl->get_parameters().end();
       ++pi)
    {
      do_indent(o, indent + ctxt.get_config().get_xml_element_indent());
      o << "<parameter type-id='"
	<< ctxt.get_id_for_type((*pi)->get_type())
	<< "'";

      if (!(*pi)->get_name().empty())
	o << " name='" << (*pi)->get_name() << "'";

      write_location((*pi)->get_location(), *get_translation_unit(decl), o);

      o << "/>\n";
    }

  if (shared_ptr<type_base> return_type = decl->get_return_type())
    {
      do_indent(o, indent + ctxt.get_config().get_xml_element_indent());
      o << "<return type-id='" << ctxt.get_id_for_type(return_type) << "'/>\n";
    }

  do_indent(o, indent);
  o << "</function-decl>";

  return true;
}

/// Serialize a class_decl type.
///
/// \param decl the pointer to class_decl to serialize.
///
/// \param ctxt the context of the serialization.
///
/// \param indent the initial indentation to use.
static bool
write_class_decl(const shared_ptr<class_decl> decl,
		 write_context& ctxt, unsigned indent)
{
  if (!decl)
    return false;

  ostream &o = ctxt.get_ostream();

  do_indent_to_level(ctxt, indent, 0);

  o << "<class-decl name='" << decl->get_name() << "'";

  write_size_and_alignment(decl, o);

  write_visibility(decl, o);

  write_location(decl, o);

  o << " id='" << ctxt.get_id_for_type(decl) << "'";
  o << ">\n";

  for (list<shared_ptr<class_decl::base_spec> >::const_iterator base =
	 decl->get_base_specifiers().begin();
       base != decl->get_base_specifiers().end();
       ++base)
    {
      do_indent_to_level(ctxt, indent, 1);
      o << "<base-class";
      write_access(*base, o);
      o << " type-id='"
	<< ctxt.get_id_for_type((*base)->get_base_class())
	<< "'/>\n";
    }

  for (list<shared_ptr<class_decl::member_type> >::const_iterator ti =
	 decl->get_member_types().begin();
       ti != decl->get_member_types().end();
       ++ti)
    {
      do_indent_to_level(ctxt, indent, 1);
      o << "<member-type";
      write_access(*ti, o);
      o << ">\n";

      write_decl(dynamic_pointer_cast<decl_base>((*ti)->as_type()), ctxt,
		 get_indent_to_level(ctxt, indent, 2));
      o << "\n";

      do_indent_to_level(ctxt, indent, 1);
      o << "</member-type>\n";
    }

  for (list<shared_ptr<class_decl::data_member> >::const_iterator data =
	 decl->get_data_members().begin();
       data != decl->get_data_members().end();
       ++data)
    {
      do_indent_to_level(ctxt, indent, 1);
      o << "<data-member";
      write_access(*data, o);
      o << ">\n";

      write_var_decl(*data, ctxt, get_indent_to_level(ctxt, indent, 2));
      o << "\n";

      do_indent_to_level(ctxt, indent, 1);
      o << "</data-member>\n";
    }

  for (list<shared_ptr<class_decl::member_function> >::const_iterator fn =
	 decl->get_member_functions().begin();
       fn != decl->get_member_functions().end();
       ++fn)
    {
      do_indent_to_level(ctxt, indent, 1);
      o << "<member-function";
      write_access(*fn, o);
      o << ">\n";

      write_function_decl(*fn, ctxt, get_indent_to_level(ctxt, indent, 2));
      o << "\n";

      do_indent_to_level(ctxt, indent, 1);
      o << "</member-function>\n";
    }

  do_indent_to_level(ctxt, indent, 0);

  o << "</class-decl>";

  return true;
}

}//end namespace writer
}//end namespace abigail
