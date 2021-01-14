// -*- Mode: C++ -*-
//
// Copyright (C) 2013-2019 Red Hat, Inc.
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

/// @file
///
/// This file contains the definitions of the entry points to
/// de-serialize an instance of @ref abigail::corpus from a file in
/// elf format, containing dwarf information.

#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <assert.h>
#include <limits.h>
#include <cstring>
#include <cmath>
#include <elfutils/libdwfl.h>
#include <dwarf.h>
#include <algorithm>
#include <tr1/unordered_map>
#include <tr1/unordered_set>
#include <stack>
#include <deque>
#include <list>
#include <ostream>
#include <sstream>

#include "abg-ir-priv.h"
#include "abg-suppression-priv.h"
#include "abg-corpus-priv.h"

#include "abg-internal.h"
// <headers defining libabigail's API go under here>
ABG_BEGIN_EXPORT_DECLARATIONS

#include "abg-dwarf-reader.h"
#include "abg-sptr-utils.h"
#include "abg-tools-utils.h"

ABG_END_EXPORT_DECLARATIONS
// </headers defining libabigail's API>

#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffff
#endif

using std::string;

namespace abigail
{

using std::cerr;

/// The namespace for the DWARF reader.
namespace dwarf_reader
{

using std::tr1::dynamic_pointer_cast;
using std::tr1::static_pointer_cast;
using std::tr1::unordered_map;
using std::tr1::unordered_set;
using std::stack;
using std::deque;
using std::list;

/// Where a DIE comes from. For instance, a DIE can come from the main
/// debug info section, the alternate debug info section or from the
/// type unit section.
enum die_source
{
  NO_DEBUG_INFO_DIE_SOURCE,
  PRIMARY_DEBUG_INFO_DIE_SOURCE,
  ALT_DEBUG_INFO_DIE_SOURCE,
  TYPE_UNIT_DIE_SOURCE,
  NUMBER_OF_DIE_SOURCES,	// This one must always be the latest
				// enumerator
};

/// Prefix increment operator for @ref die_source.
///
/// @param source the die_source to increment.
/// @return the incremented source.
static die_source&
operator++(die_source& source)
{
  source = static_cast<die_source>(source + 1);
  return source;
}

/// A functor used by @ref dwfl_sptr.
struct dwfl_deleter
{
  void
  operator()(Dwfl* dwfl)
  {dwfl_end(dwfl);}
};//end struct dwfl_deleter

/// A convenience typedef for a shared pointer to a Dwfl.
typedef shared_ptr<Dwfl> dwfl_sptr;

/// A convenience typedef for a vector of Dwarf_Off.
typedef vector<Dwarf_Off> dwarf_offsets_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die and which value is the corresponding artefact.
typedef unordered_map<Dwarf_Off, type_or_decl_base_sptr> die_artefact_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die, (given by dwarf_dieoffset()) and which value is the
/// corresponding class_decl.
typedef unordered_map<Dwarf_Off, class_decl_sptr> die_class_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die, (given by dwarf_dieoffset()) and which value is the
/// corresponding class_or_union_sptr.
typedef unordered_map<Dwarf_Off, class_or_union_sptr> die_class_or_union_map_type;

/// Convenience typedef for a map which key the offset of a dwarf die
/// and which value is the corresponding function_decl.
typedef unordered_map<Dwarf_Off, function_decl_sptr> die_function_decl_map_type;

/// Convenience typedef for a map which key is the offset of a dwarf
/// die and which value is the corresponding function_type.
typedef unordered_map<Dwarf_Off, function_type_sptr> die_function_type_map_type;

/// Convenience typedef for a map which key is the offset of a
/// DW_TAG_compile_unit and the value is the corresponding @ref
/// translation_unit_sptr.
typedef unordered_map<Dwarf_Off, translation_unit_sptr> die_tu_map_type;

/// Convenience typedef for a map which key is the offset of a DIE and
/// the value is the corresponding qualified name of the DIE.
typedef unordered_map<Dwarf_Off, interned_string> die_istring_map_type;

/// Convenience typedef for a map which is an interned_string and
/// which value is a vector of offsets.
typedef unordered_map<interned_string,
		      dwarf_offsets_type,
		      hash_interned_string>
istring_dwarf_offsets_map_type;

/// Convenience typedef for a map which key is an elf address and
/// which value is an elf_symbol_sptr.
typedef unordered_map<GElf_Addr, elf_symbol_sptr> addr_elf_symbol_sptr_map_type;

/// Convenience typedef for a set of ELF addresses.
typedef unordered_set<GElf_Addr> address_set_type;

typedef unordered_set<interned_string, hash_interned_string> istring_set_type;

/// Convenience typedef for a shared pointer to an @ref address_set_type.
typedef shared_ptr<address_set_type> address_set_sptr;

/// Convenience typedef for a shared pointer to an
/// addr_elf_symbol_sptr_map_type.
typedef shared_ptr<addr_elf_symbol_sptr_map_type> addr_elf_symbol_sptr_map_sptr;

/// Convenience typedef for a stack containing the scopes up to the
/// current point in the abigail Internal Representation (aka IR) tree
/// that is being built.
typedef stack<scope_decl*> scope_stack_type;

/// Convenience typedef for a map which key is a dwarf offset.  The
/// value is also a dwarf offset.
typedef unordered_map<Dwarf_Off, Dwarf_Off> offset_offset_map_type;

/// Convenience typedef for a map which key is a string and which
/// value is a vector of smart pointer to a class.
typedef unordered_map<string, classes_type> string_classes_map;

/// The abstraction of the place where a partial unit has been
/// imported.  This is what the DW_TAG_imported_unit DIE expresses.
///
/// This type thus contains:
///	- the offset to which the partial unit is imported
///	- the offset of the imported partial unit.
///	- the offset of the imported partial unit.
struct imported_unit_point
{
  Dwarf_Off	offset_of_import;
  // The boolean below is true iff the imported unit comes from the
  // alternate debug info file.
  die_source	imported_unit_die_source;
  Dwarf_Off	imported_unit_die_off;
  Dwarf_Off	imported_unit_cu_off;
  Dwarf_Off	imported_unit_child_off;

  /// Default constructor for @ref the type imported_unit_point.
  imported_unit_point ()
    : offset_of_import(),
      imported_unit_die_source(PRIMARY_DEBUG_INFO_DIE_SOURCE),
      imported_unit_die_off(),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {}

  /// Constructor of @ref the type imported_unit_point.
  ///
  /// @param import_off the offset of the point at which the unit has
  /// been imported.
  imported_unit_point (Dwarf_Off import_off)
    : offset_of_import(import_off),
      imported_unit_die_source(PRIMARY_DEBUG_INFO_DIE_SOURCE),
      imported_unit_die_off(),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {}

  /// Constructor of @ref the type imported_unit_point.
  ///
  /// @param import_off the offset of the point at which the unit has
  /// been imported.
  ///
  /// @param from where the imported DIE comes from.
  ///
  /// @param imported_die the die of the unit that has been imported.
  imported_unit_point (Dwarf_Off	import_off,
		       const Dwarf_Die& imported_die,
		       die_source from)
    : offset_of_import(import_off),
      imported_unit_die_source(from),
      imported_unit_die_off(dwarf_dieoffset
			    (const_cast<Dwarf_Die*>(&imported_die))),
      imported_unit_cu_off(),
      imported_unit_child_off()
  {
    Dwarf_Die imported_unit_child;

    dwarf_child(const_cast<Dwarf_Die*>(&imported_die),
		&imported_unit_child);
    imported_unit_child_off =
      dwarf_dieoffset(const_cast<Dwarf_Die*>(&imported_unit_child));

    Dwarf_Die cu_die_memory;
    Dwarf_Die *cu_die;

    cu_die = dwarf_diecu(const_cast<Dwarf_Die*>(&imported_unit_child),
			 &cu_die_memory, 0, 0);
    imported_unit_cu_off = dwarf_dieoffset(cu_die);
  }
}; // struct imported_unit_point

/// Convenience typedef for a vector of @ref imported_unit_point.
typedef vector<imported_unit_point> imported_unit_points_type;

/// Convenience typedef for a vector of @ref imported_unit_point.
typedef unordered_map<Dwarf_Off, imported_unit_points_type>
tu_die_imported_unit_points_map_type;

/// "Less than" operator for instances of @ref imported_unit_point
/// type.
///
/// @param the left hand side operand of the "Less than" operator.
///
/// @param the right hand side operand of the "Less than" operator.
///
/// @return true iff @p l is less than @p r.
static bool
operator<(const imported_unit_point& l, const imported_unit_point& r)
{return l.offset_of_import < r.offset_of_import;}

static void
add_symbol_to_map(const elf_symbol_sptr& sym,
		  string_elf_symbols_map_type& map);

static bool
find_symbol_table_section(Elf* elf_handle, Elf_Scn*& section);

static bool
get_symbol_versionning_sections(Elf*		elf_handle,
				Elf_Scn*&	versym_section,
				Elf_Scn*&	verdef_section,
				Elf_Scn*&	verneed_section);

static bool
get_parent_die(const read_context&	ctxt,
	       Dwarf_Die*		die,
	       Dwarf_Die&		parent_die,
	       size_t			where_offset);

static bool
get_scope_die(const read_context&	ctxt,
	      Dwarf_Die*		die,
	      size_t			where_offset,
	      Dwarf_Die&		scope_die);

static bool
die_is_anonymous(Dwarf_Die* die);

static bool
die_is_type(Dwarf_Die* die);

static bool
die_is_decl(Dwarf_Die* die);

static bool
die_is_namespace(Dwarf_Die* die);

static bool
die_is_unspecified(Dwarf_Die* die);

static bool
die_is_void_type(Dwarf_Die* die);

static bool
die_is_pointer_type(Dwarf_Die* die);

static bool
pointer_or_qual_die_of_anonymous_class_type(Dwarf_Die* die);

static bool
die_is_reference_type(Dwarf_Die* die);

static bool
die_is_pointer_or_reference_type(Dwarf_Die* die);

static bool
die_is_pointer_reference_or_typedef_type(Dwarf_Die* die);

static bool
die_is_class_type(Dwarf_Die* die);

static bool
die_is_qualified_type(Dwarf_Die* die);

static bool
die_has_object_pointer(Dwarf_Die* die,
		       Dwarf_Die& object_pointer);

static bool
die_this_pointer_from_object_pointer(Dwarf_Die* die,
				     Dwarf_Die& this_pointer);

static bool
die_this_pointer_is_const(Dwarf_Die* die);

static bool
die_object_pointer_is_for_const_method(Dwarf_Die* die);

static bool
die_is_at_class_scope(const read_context& ctxt,
		      Dwarf_Die* die,
		      size_t where_offset,
		      Dwarf_Die& class_scope_die);
static bool
eval_last_constant_dwarf_sub_expr(Dwarf_Op*	expr,
				  uint64_t	expr_len,
				  int64_t&	value,
				  bool&	is_tls_address);

static translation_unit::language
dwarf_language_to_tu_language(size_t l);

static bool
die_unsigned_constant_attribute(Dwarf_Die*	die,
				unsigned	attr_name,
				uint64_t&	cst);

static bool
die_signed_constant_attribute(Dwarf_Die*die,
			      unsigned	attr_name,
			      int64_t&	cst);

static bool
die_constant_attribute(Dwarf_Die *die,
		       unsigned attr_name,
		       array_type_def::subrange_type::bound_value &value);

static bool
die_attribute_has_form(Dwarf_Die*					 die,
		       unsigned						 attr_name,
		       unsigned int					 form);

static bool
die_attribute_is_signed(Dwarf_Die* die, unsigned attr_name);

static bool
die_attribute_is_unsigned(Dwarf_Die* die, unsigned attr_name);

static bool
die_attribute_has_no_signedness(Dwarf_Die* die, unsigned attr_name);

static bool
die_address_attribute(Dwarf_Die* die, unsigned attr_name, Dwarf_Addr& result);

static string
die_name(Dwarf_Die* die);

static location
die_location(const read_context& ctxt, Dwarf_Die* die);

static bool
die_location_address(Dwarf_Die*	die,
		     Dwarf_Addr&	address,
		     bool&		is_tls_address);

static bool
die_die_attribute(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Die& result,
		  bool look_thru_abstract_origin = true);

static string
get_internal_anonynous_die_base_name(Dwarf_Die *die);

static string
build_internal_anonymous_die_name(const string &base_name,
				  size_t anonymous_type_index);


static string
get_internal_anonymous_die_name(Dwarf_Die *die,
				size_t anonymous_type_index);

static string
die_qualified_type_name(const read_context& ctxt, Dwarf_Die* die, size_t where);

static string
die_qualified_decl_name(const read_context& ctxt, Dwarf_Die* die, size_t where);

static string
die_qualified_name(const read_context& ctxt, Dwarf_Die* die, size_t where);

static bool
die_qualified_type_name_empty(const read_context& ctxt,
			      Dwarf_Die* die, size_t where,
			      string &qualified_name);

static void
die_return_and_parm_names_from_fn_type_die(const read_context& ctxt,
					   Dwarf_Die* die,
					   size_t where_offset,
					   bool pretty_print,
					   string &return_type_name,
					   string &class_name,
					   vector<string>& parm_names,
					   bool& is_const,
					   bool& is_static);

static string
die_function_signature(const read_context& ctxt,
		       Dwarf_Die *die,
		       size_t where_offset);

static bool
die_peel_qual_ptr(Dwarf_Die *die, Dwarf_Die& peeled_die);

static bool
die_function_type_is_method_type(const read_context& ctxt,
				 Dwarf_Die *die,
				 size_t where_offset,
				 Dwarf_Die& object_pointer_die,
				 Dwarf_Die& class_die,
				 bool& is_static);

static string
die_pretty_print_type(read_context& ctxt, Dwarf_Die* die, size_t where_offset);

static string
die_pretty_print_decl(read_context& ctxt, Dwarf_Die* die, size_t where_offset);

static string
die_pretty_print(read_context& ctxt, Dwarf_Die* die, size_t where_offset);

static void
maybe_canonicalize_type(Dwarf_Die* die, read_context& ctxt);

static uint64_t
get_default_array_lower_bound(translation_unit::language l);

static bool
find_lower_bound_in_imported_unit_points(const imported_unit_points_type&,
					 Dwarf_Off,
					 imported_unit_points_type::const_iterator&);

static array_type_def::subrange_sptr
build_subrange_type(read_context& ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    bool		associate_type_to_die = true);

static void
build_subranges_from_array_type_die(read_context&	ctxt,
				    Dwarf_Die*		die,
				    array_type_def::subranges_type& subranges,
				    size_t		where_offset,
				    bool		associate_type_to_die = true);

static bool
compare_dies(const read_context& ctxt,
	     Dwarf_Die *l, Dwarf_Die *r,
	     bool update_canonical_dies_on_the_fly);

/// Convert an elf symbol type (given by the ELF{32,64}_ST_TYPE
/// macros) into an elf_symbol::type value.
///
/// Note that this function aborts when given an unexpected value.
///
/// @param the symbol type value to convert.
///
/// @return the converted value.
static elf_symbol::type
stt_to_elf_symbol_type(unsigned char stt)
{
  elf_symbol::type t = elf_symbol::NOTYPE_TYPE;

  switch (stt)
    {
    case STT_NOTYPE:
      t = elf_symbol::NOTYPE_TYPE;
      break;
    case STT_OBJECT:
      t = elf_symbol::OBJECT_TYPE;
      break;
    case STT_FUNC:
      t = elf_symbol::FUNC_TYPE;
      break;
    case STT_SECTION:
      t = elf_symbol::SECTION_TYPE;
      break;
    case STT_FILE:
      t = elf_symbol::FILE_TYPE;
      break;
    case STT_COMMON:
      t = elf_symbol::COMMON_TYPE;
      break;
    case STT_TLS:
      t = elf_symbol::TLS_TYPE;
      break;
    case STT_GNU_IFUNC:
      t = elf_symbol::GNU_IFUNC_TYPE;
      break;
    default:
      // An unknown value that probably ought to be supported?  Let's
      // abort right here rather than yielding garbage.
      ABG_ASSERT_NOT_REACHED;
    }

  return t;
}

/// Convert an elf symbol binding (given by the ELF{32,64}_ST_BIND
/// macros) into an elf_symbol::binding value.
///
/// Note that this function aborts when given an unexpected value.
///
/// @param the symbol binding value to convert.
///
/// @return the converted value.
static elf_symbol::binding
stb_to_elf_symbol_binding(unsigned char stb)
{
  elf_symbol::binding b = elf_symbol::GLOBAL_BINDING;

  switch (stb)
    {
    case STB_LOCAL:
      b = elf_symbol::LOCAL_BINDING;
      break;
    case STB_GLOBAL:
      b = elf_symbol::GLOBAL_BINDING;
      break;
    case STB_WEAK:
      b = elf_symbol::WEAK_BINDING;
      break;
    case STB_GNU_UNIQUE:
      b = elf_symbol::GNU_UNIQUE_BINDING;
      break;
    default:
      ABG_ASSERT_NOT_REACHED;
    }

  return b;

}

/// Convert an ELF symbol visiblity given by the symbols ->st_other
/// data member as returned by the GELF_ST_VISIBILITY macro into a
/// elf_symbol::visiblity value.
///
/// @param stv the value of the ->st_other data member of the ELF
/// symbol.
///
/// @return the converted elf_symbol::visiblity value.
static elf_symbol::visibility
stv_to_elf_symbol_visibility(unsigned char stv)
{

  elf_symbol::visibility v = elf_symbol::DEFAULT_VISIBILITY;

  switch (stv)
    {
    case STV_DEFAULT:
      v = elf_symbol::DEFAULT_VISIBILITY;
      break;
    case STV_INTERNAL:
      v = elf_symbol::INTERNAL_VISIBILITY;
      break;
    case STV_HIDDEN:
      v = elf_symbol::HIDDEN_VISIBILITY;
      break;
    case STV_PROTECTED:
      v = elf_symbol::PROTECTED_VISIBILITY;
      break;
    default:
      ABG_ASSERT_NOT_REACHED;
    }

  return v;
}

/// Convert the value of the e_machine field of GElf_Ehdr into a
/// string.  This is to get a string representing the architecture of
/// the elf file at hand.
///
/// @param e_machine the value of GElf_Ehdr::e_machine.
///
/// @return the string representation of GElf_Ehdr::e_machine.
static string
e_machine_to_string(GElf_Half e_machine)
{
  string result;
  switch (e_machine)
    {
    case EM_NONE:
      result = "elf-no-arch";
      break;
    case EM_M32:
      result = "elf-att-we-32100";
      break;
    case EM_SPARC:
      result = "elf-sun-sparc";
      break;
    case EM_386:
      result = "elf-intel-80386";
      break;
    case EM_68K:
      result = "elf-motorola-68k";
      break;
    case EM_88K:
      result = "elf-motorola-88k";
      break;
    case EM_860:
      result = "elf-intel-80860";
      break;
    case EM_MIPS:
      result = "elf-mips-r3000-be";
      break;
    case EM_S370:
      result = "elf-ibm-s370";
      break;
    case EM_MIPS_RS3_LE:
      result = "elf-mips-r3000-le";
      break;
    case EM_PARISC:
      result = "elf-hp-parisc";
      break;
    case EM_VPP500:
      result = "elf-fujitsu-vpp500";
      break;
    case EM_SPARC32PLUS:
      result = "elf-sun-sparc-v8plus";
      break;
    case EM_960:
      result = "elf-intel-80960";
      break;
    case EM_PPC:
      result = "elf-powerpc";
      break;
    case EM_PPC64:
      result = "elf-powerpc-64";
      break;
    case EM_S390:
      result = "elf-ibm-s390";
      break;
    case EM_V800:
      result = "elf-nec-v800";
      break;
    case EM_FR20:
      result = "elf-fujitsu-fr20";
      break;
    case EM_RH32:
      result = "elf-trw-rh32";
      break;
    case EM_RCE:
      result = "elf-motorola-rce";
      break;
    case EM_ARM:
      result = "elf-arm";
      break;
    case EM_FAKE_ALPHA:
      result = "elf-digital-alpha";
      break;
    case EM_SH:
      result = "elf-hitachi-sh";
      break;
    case EM_SPARCV9:
      result = "elf-sun-sparc-v9-64";
      break;
    case EM_TRICORE:
      result = "elf-siemens-tricore";
      break;
    case EM_ARC:
      result = "elf-argonaut-risc-core";
      break;
    case EM_H8_300:
      result = "elf-hitachi-h8-300";
      break;
    case EM_H8_300H:
      result = "elf-hitachi-h8-300h";
      break;
    case EM_H8S:
      result = "elf-hitachi-h8s";
      break;
    case EM_H8_500:
      result = "elf-hitachi-h8-500";
      break;
    case EM_IA_64:
      result = "elf-intel-ia-64";
      break;
    case EM_MIPS_X:
      result = "elf-stanford-mips-x";
      break;
    case EM_COLDFIRE:
      result = "elf-motorola-coldfire";
      break;
    case EM_68HC12:
      result = "elf-motorola-68hc12";
      break;
    case EM_MMA:
      result = "elf-fujitsu-mma";
      break;
    case EM_PCP:
      result = "elf-siemens-pcp";
      break;
    case EM_NCPU:
      result = "elf-sony-ncpu";
      break;
    case EM_NDR1:
      result = "elf-denso-ndr1";
      break;
    case EM_STARCORE:
      result = "elf-motorola-starcore";
      break;
    case EM_ME16:
      result = "elf-toyota-me16";
      break;
    case EM_ST100:
      result = "elf-stm-st100";
      break;
    case EM_TINYJ:
      result = "elf-alc-tinyj";
      break;
    case EM_X86_64:
      result = "elf-amd-x86_64";
      break;
    case EM_PDSP:
      result = "elf-sony-pdsp";
      break;
    case EM_FX66:
      result = "elf-siemens-fx66";
      break;
    case EM_ST9PLUS:
      result = "elf-stm-st9+";
      break;
    case EM_ST7:
      result = "elf-stm-st7";
      break;
    case EM_68HC16:
      result = "elf-motorola-68hc16";
      break;
    case EM_68HC11:
      result = "elf-motorola-68hc11";
      break;
    case EM_68HC08:
      result = "elf-motorola-68hc08";
      break;
    case EM_68HC05:
      result = "elf-motorola-68hc05";
      break;
    case EM_SVX:
      result = "elf-sg-svx";
      break;
    case EM_ST19:
      result = "elf-stm-st19";
      break;
    case EM_VAX:
      result = "elf-digital-vax";
      break;
    case EM_CRIS:
      result = "elf-axis-cris";
      break;
    case EM_JAVELIN:
      result = "elf-infineon-javelin";
      break;
    case EM_FIREPATH:
      result = "elf-firepath";
      break;
    case EM_ZSP:
      result = "elf-lsi-zsp";
      break;
    case EM_MMIX:
      result = "elf-don-knuth-mmix";
      break;
    case EM_HUANY:
      result = "elf-harvard-huany";
      break;
    case EM_PRISM:
      result = "elf-sitera-prism";
      break;
    case EM_AVR:
      result = "elf-atmel-avr";
      break;
    case EM_FR30:
      result = "elf-fujistu-fr30";
      break;
    case EM_D10V:
      result = "elf-mitsubishi-d10v";
      break;
    case EM_D30V:
      result = "elf-mitsubishi-d30v";
      break;
    case EM_V850:
      result = "elf-nec-v850";
      break;
    case EM_M32R:
      result = "elf-mitsubishi-m32r";
      break;
    case EM_MN10300:
      result = "elf-matsushita-mn10300";
      break;
    case EM_MN10200:
      result = "elf-matsushita-mn10200";
      break;
    case EM_PJ:
      result = "elf-picojava";
      break;
    case EM_OPENRISC:
      result = "elf-openrisc-32";
      break;
    case EM_ARC_A5:
      result = "elf-arc-a5";
      break;
    case EM_XTENSA:
      result = "elf-tensilica-xtensa";
      break;

#ifdef HAVE_EM_AARCH64_MACRO
    case EM_AARCH64:
      result = "elf-arm-aarch64";
      break;
#endif

#ifdef HAVE_EM_TILEPRO_MACRO
    case EM_TILEPRO:
      result = "elf-tilera-tilepro";
      break;
#endif

#ifdef HAVE_EM_TILEGX_MACRO
    case EM_TILEGX:
      result = "elf-tilera-tilegx";
      break;
#endif

    case EM_NUM:
      result = "elf-last-arch-number";
      break;
    case EM_ALPHA:
      result = "elf-non-official-alpha";
      break;
    default:
      {
	std::ostringstream o;
	o << "elf-unknown-arch-value-" << e_machine;
	result = o.str();
      }
      break;
    }
  return result;
}

/// The kind of ELF hash table found by the function
/// find_hash_table_section_index.
enum hash_table_kind
{
  NO_HASH_TABLE_KIND = 0,
  SYSV_HASH_TABLE_KIND,
  GNU_HASH_TABLE_KIND
};

/// Get the offset offset of the hash table section.
///
/// @param elf_handle the elf handle to use.
///
/// @param ht_section_offset this is set to the resulting offset
/// of the hash table section.  This is set iff the function returns true.
///
/// @param symtab_section_offset the offset of the section of the
/// symbol table the hash table refers to.
static hash_table_kind
find_hash_table_section_index(Elf*	elf_handle,
			      size_t&	ht_section_index,
			      size_t&	symtab_section_index)
{
  if (!elf_handle)
    return NO_HASH_TABLE_KIND;

  GElf_Shdr header_mem, *section_header;
  bool found_sysv_ht = false, found_gnu_ht = false;
  for (Elf_Scn* section = elf_nextscn(elf_handle, 0);
       section != 0;
       section = elf_nextscn(elf_handle, section))
    {
      section_header= gelf_getshdr(section, &header_mem);
      if (section_header->sh_type != SHT_HASH
	  && section_header->sh_type != SHT_GNU_HASH)
	continue;

      ht_section_index = elf_ndxscn(section);
      symtab_section_index = section_header->sh_link;

      if (section_header->sh_type == SHT_HASH)
	found_sysv_ht = true;
      else if (section_header->sh_type == SHT_GNU_HASH)
	found_gnu_ht = true;
    }

  if (found_gnu_ht)
    return GNU_HASH_TABLE_KIND;
  else if (found_sysv_ht)
    return SYSV_HASH_TABLE_KIND;
  else
    return NO_HASH_TABLE_KIND;
}

/// Find the symbol table.
///
/// If we are looking at a relocatable or executable file, this
/// function will return the .symtab symbol table (of type
/// SHT_SYMTAB).  But if we are looking at a DSO it returns the
/// .dynsym symbol table (of type SHT_DYNSYM).
///
/// @param elf_handle the elf handle to consider.
///
/// @param symtab the symbol table found.
///
/// @return true iff the symbol table is found.
static bool
find_symbol_table_section(Elf* elf_handle, Elf_Scn*& symtab)
{
  Elf_Scn* section = 0, *dynsym = 0, *sym_tab = 0;
  while ((section = elf_nextscn(elf_handle, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type == SHT_DYNSYM)
	dynsym = section;
      else if (header->sh_type == SHT_SYMTAB)
	sym_tab = section;
    }

  if (dynsym || sym_tab)
    {
      GElf_Ehdr eh_mem;
      GElf_Ehdr* elf_header = gelf_getehdr(elf_handle, &eh_mem);
      if (elf_header->e_type == ET_REL
	  || elf_header->e_type == ET_EXEC)
	symtab = sym_tab ? sym_tab : dynsym;
      else
	symtab = dynsym ? dynsym : sym_tab;
      return true;
    }
  return false;
}

/// Find the index (in the section headers table) of the symbol table
/// section.
///
/// If we are looking at a relocatable or executable file, this
/// function will return the index for the .symtab symbol table (of
/// type SHT_SYMTAB).  But if we are looking at a DSO it returns the
/// index for the .dynsym symbol table (of type SHT_DYNSYM).
///
/// @param elf_handle the elf handle to use.
///
/// @param symtab_index the index of the symbol_table, that was found.
///
/// @return true iff the symbol table section index was found.
static bool
find_symbol_table_section_index(Elf* elf_handle,
				size_t& symtab_index)
{
  Elf_Scn* section = 0;
  if (!find_symbol_table_section(elf_handle, section))
    return false;

  symtab_index = elf_ndxscn(section);
  return true;
}

/// Find and return a section by its name and its type.
///
/// @param elf_handle the elf handle to use.
///
/// @param name the name of the section.
///
/// @param section_type the type of the section.  This is the
/// Elf32_Shdr::sh_type (or Elf64_Shdr::sh_type) data member.
/// Examples of values of this parameter are SHT_PROGBITS or SHT_NOBITS.
///
/// @return the section found, nor nil if none was found.
static Elf_Scn*
find_section(Elf* elf_handle, const string& name, Elf64_Word section_type)
{
  GElf_Ehdr ehmem, *elf_header;
  elf_header = gelf_getehdr(elf_handle, &ehmem);

  Elf_Scn* section = 0;
  while ((section = elf_nextscn(elf_handle, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type != section_type)
	continue;

      const char* section_name =
	elf_strptr(elf_handle, elf_header->e_shstrndx, header->sh_name);
      if (section_name && name == section_name)
	return section;
    }

  return 0;
}

/// Find and return the .text section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .text section found.
static Elf_Scn*
find_text_section(Elf* elf_handle)
{return find_section(elf_handle, ".text", SHT_PROGBITS);}

/// Find and return the .bss section.
///
/// @param elf_handle.
///
/// @return the .bss section found.
static Elf_Scn*
find_bss_section(Elf* elf_handle)
{return find_section(elf_handle, ".bss", SHT_NOBITS);}

/// Find and return the .rodata section.
///
/// @param elf_handle.
///
/// @return the .rodata section found.
static Elf_Scn*
find_rodata_section(Elf* elf_handle)
{return find_section(elf_handle, ".rodata", SHT_PROGBITS);}

/// Find and return the .data section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .data section found.
static Elf_Scn*
find_data_section(Elf* elf_handle)
{return find_section(elf_handle, ".data", SHT_PROGBITS);}

/// Find and return the .data1 section.
///
/// @param elf_handle the elf handle to use.
///
/// @return the .data1 section found.
static Elf_Scn*
find_data1_section(Elf* elf_handle)
{return find_section(elf_handle, ".data1", SHT_PROGBITS);}

/// Get the address at which a given binary is loaded in memory⋅
///
/// @param elf_handle the elf handle for the binary to consider.
///
/// @param load_address the address where the binary is loaded.  This
/// is set by the function iff it returns true.
///
/// @return true if the function could get the binary load address
/// and assign @p load_address to it.
static bool
get_binary_load_address(Elf *elf_handle,
			GElf_Addr &load_address)
{
  GElf_Ehdr eh_mem;
  GElf_Ehdr *elf_header = gelf_getehdr(elf_handle, &eh_mem);
  size_t num_segments = elf_header->e_phnum;
  GElf_Phdr *lowest_program_header = 0, *program_header = 0;
  GElf_Phdr ph_mem;

  for (unsigned i = 0; i < num_segments; ++i)
    {
      program_header = gelf_getphdr(elf_handle, i, &ph_mem);
      if (program_header->p_type == PT_LOAD
	  && (!lowest_program_header
	      || program_header->p_vaddr < lowest_program_header->p_vaddr))
	lowest_program_header = program_header;
    }

  if (lowest_program_header)
    {
      // This program header represent the segment containing the
      // first byte of this binary.  We want to return the address
      // at which the segment is loaded in memory.
      load_address = lowest_program_header->p_vaddr;
      return true;
    }
  return false;
}

/// Find the file name of the alternate debug info file.
///
/// @param elf_module the elf module to consider.
///
/// @param out parameter.  Is set to the file name of the alternate
/// debug info file, iff this function returns true.
///
/// @return true iff the location of the alternate debug info file was
/// found.
static bool
find_alt_debug_info_link(Dwfl_Module *elf_module,
			 string &alt_file_name)
{
  GElf_Addr bias = 0;
  Dwarf *dwarf = dwfl_module_getdwarf(elf_module, &bias);
  Elf *elf = dwarf_getelf(dwarf);
  GElf_Ehdr ehmem, *elf_header;
  elf_header = gelf_getehdr(elf, &ehmem);

  Elf_Scn* section = 0;
  while ((section = elf_nextscn(elf, section)) != 0)
    {
      GElf_Shdr header_mem, *header;
      header = gelf_getshdr(section, &header_mem);
      if (header->sh_type != SHT_PROGBITS)
	continue;

      const char *section_name = elf_strptr(elf,
					    elf_header->e_shstrndx,
					    header->sh_name);

      char *alt_name = 0;
      char *buildid = 0;
      size_t buildid_len = 0;
      if (section_name != 0
	  && strcmp(section_name, ".gnu_debugaltlink") == 0)
	{
	  Elf_Data *data = elf_getdata(section, 0);
	  if (data != 0 && data->d_size != 0)
	    {
	      alt_name = (char*) data->d_buf;
	      char *end_of_alt_name =
		(char *) memchr(alt_name, '\0', data->d_size);
	      buildid_len = data->d_size - (end_of_alt_name - alt_name + 1);
	      if (buildid_len == 0)
		return false;
	      buildid = end_of_alt_name + 1;
	    }
	}
      else
	continue;

      if (buildid == 0 || alt_name == 0)
	return false;

      alt_file_name = alt_name;
      return true;
    }

  return false;
}

/// Find alternate debuginfo file of a given "link" under a set of
/// root directories.
///
/// The link is a string that is read by the function
/// find_alt_debug_info_link().  That link is a path that is relative
/// to a given debug info file, e.g, "../../../.dwz/something.debug".
/// It designates the alternate debug info file associated to a given
/// debug info file.
///
/// This function will thus try to find the .dwz/something.debug file
/// under some given root directories.
///
/// @param root_dirs the set of root directories to look from.
///
/// @param alt_file_name a relative path to the alternate debug info
/// file to look for.
///
/// @param alt_file_path the resulting absolute path to the alternate
/// debuginfo path denoted by @p alt_file_name and found under one of
/// the directories in @p root_dirs.  This is set iff the function
/// returns true.
///
/// @return true iff the function found the alternate debuginfo file.
static bool
find_alt_debug_info_path(const vector<char**> root_dirs,
			 const string &alt_file_name,
			 string &alt_file_path)
{
  if (alt_file_name.empty())
    return false;

  string altfile_name = tools_utils::trim_leading_string(alt_file_name, "../");

  for (vector<char**>::const_iterator i = root_dirs.begin();
       i != root_dirs.end();
       ++i)
    if (tools_utils::find_file_under_dir(**i, altfile_name, alt_file_path))
      return true;

  return false;
}

/// Return the alternate debug info associated to a given main debug
/// info file.
///
/// @param elf_module the elf module to consider.
///
/// @param debug_root_dirs a set of root debuginfo directories under
/// which too look for the alternate debuginfo file.
///
/// @param alt_file_name output parameter.  This is set to the file
/// path of the alternate debug info file associated to @p elf_module.
/// This is set iff the function returns a non-null result.
///
/// @param alt_fd the file descriptor used to access the alternate
/// debug info.  If this parameter is set by the function, then the
/// caller needs to fclose it, otherwise the file descriptor is going
/// to be leaked.  Note however that on recent versions of elfutils
/// where libdw.h contains the function dwarf_getalt(), this parameter
/// is set to 0, so it doesn't need to be fclosed.
///
/// Note that the alternate debug info file is a DWARF extension as of
/// DWARF 4 ans is decribed at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @return the alternate debuginfo, or null.  If @p alt_fd is
/// non-zero, then the caller of this function needs to call
/// dwarf_end() on the returned alternate debuginfo pointer,
/// otherwise, it's going to be leaked.
static Dwarf*
find_alt_debug_info(Dwfl_Module *elf_module,
		    const vector<char**> debug_root_dirs,
		    string& alt_file_name,
		    int& alt_fd)
{
  if (elf_module == 0)
    return 0;

  Dwarf* result = 0;
  find_alt_debug_info_link(elf_module, alt_file_name);

#ifdef LIBDW_HAS_DWARF_GETALT
  // We are on recent versions of elfutils where the function
  // dwarf_getalt exists, so let's use it.
  Dwarf_Addr bias = 0;
  Dwarf* dwarf = dwfl_module_getdwarf(elf_module, &bias);
  result = dwarf_getalt(dwarf);
  alt_fd = 0;
#else
  // We are on an old version of elfutils where the function
  // dwarf_getalt doesn't exist yet, so let's open code its
  // functionality
  char *alt_name = 0;
  const char *file_name = 0;
  void **user_data = 0;
  Dwarf_Addr low_addr = 0;
  char *alt_file = 0;

  file_name = dwfl_module_info(elf_module, &user_data,
			       &low_addr, 0, 0, 0, 0, 0);

  alt_fd = dwfl_standard_find_debuginfo(elf_module, user_data,
					file_name, low_addr,
					alt_name, file_name,
					0, &alt_file);

  result = dwarf_begin(alt_fd, DWARF_C_READ);
#endif

  if (result == 0)
    {
      // So we didn't find the alternate debuginfo file from the
      // information that is in the debuginfo file associated to
      // elf_module.  Maybe the alternate debuginfo file is located
      // under one of the directories in debug_root_dirs.  So let's
      // look in there.
      string alt_file_path;
      if (!find_alt_debug_info_path(debug_root_dirs,
				    alt_file_name,
				    alt_file_path))
	return result;

      // If we reach this point it means we have found the path to the
      // alternate debuginfo file and it's in alt_file_path.  So let's
      // open it and read it.
      int fd = open(alt_file_path.c_str(), O_RDONLY);
      if (fd == -1)
	return result;
      result = dwarf_begin(fd, DWARF_C_READ);

#ifdef LIBDW_HAS_DWARF_GETALT
      Dwarf_Addr bias = 0;
      Dwarf* dwarf = dwfl_module_getdwarf(elf_module, &bias);
      dwarf_setalt(dwarf, result);
#endif
    }

  return result;
}

/// Compare a symbol name against another name, possibly demangling
/// the symbol_name before performing the comparison.
///
/// @param symbol_name the symbol_name to take in account.
///
/// @param name the second name to take in account.
///
/// @param demangle if true, demangle @p symbol_name and compare the
/// result of the demangling with @p name.
///
/// @return true iff symbol_name equals name.
static bool
compare_symbol_name(const string& symbol_name,
		    const string& name,
		    bool demangle)
{
  if (demangle)
    {
      string m = demangle_cplus_mangled_name(symbol_name);
      return m == name;
    }
  return symbol_name == name;
}

/// Return the SHT_GNU_versym, SHT_GNU_verdef and SHT_GNU_verneed
/// sections that are involved in symbol versionning.
///
/// @param elf_handle the elf handle to use.
///
/// @param versym_section the SHT_GNU_versym section found.  If the
/// section wasn't found, this is set to nil.
///
/// @param verdef_section the SHT_GNU_verdef section found.  If the
/// section wasn't found, this is set to nil.
///
/// @param verneed_section the SHT_GNU_verneed section found.  If the
/// section wasn't found, this is set to nil.
///
/// @return true iff at least one of the sections where found.
static bool
get_symbol_versionning_sections(Elf*		elf_handle,
				Elf_Scn*&	versym_section,
				Elf_Scn*&	verdef_section,
				Elf_Scn*&	verneed_section)
{
  Elf_Scn* section = NULL;
  GElf_Shdr mem;
  Elf_Scn* versym = NULL, *verdef = NULL, *verneed = NULL;

  while ((section = elf_nextscn(elf_handle, section)) != NULL)
    {
      GElf_Shdr* h = gelf_getshdr(section, &mem);
      if (h->sh_type == SHT_GNU_versym)
	versym = section;
      else if (h->sh_type == SHT_GNU_verdef)
	verdef = section;
      else if (h->sh_type == SHT_GNU_verneed)
	verneed = section;
    }

  if (versym || verdef || verneed)
    {
      // At least one the versionning sections was found.  Return it.
      versym_section = versym;
      verdef_section = verdef;
      verneed_section = verneed;
      return true;
    }

  return false;
}

/// Get the version definition (from the SHT_GNU_verdef section) of a
/// given symbol represented by a pointer to GElf_Versym.
///
/// @param elf_hande the elf handle to use.
///
/// @param versym the symbol to get the version definition for.
///
/// @param verdef_section the SHT_GNU_verdef section.
///
/// @param version the resulting version definition.  This is set iff
/// the function returns true.
///
/// @return true upon successful completion, false otherwise.
static bool
get_version_definition_for_versym(Elf*			 elf_handle,
				  GElf_Versym*		 versym,
				  Elf_Scn*		 verdef_section,
				  elf_symbol::version&	 version)
{
  Elf_Data* verdef_data = elf_getdata(verdef_section, NULL);
  GElf_Verdef verdef_mem;
  GElf_Verdef* verdef = gelf_getverdef(verdef_data, 0, &verdef_mem);
  size_t vd_offset = 0;

  for (;; vd_offset += verdef->vd_next)
    {
      for (;verdef != 0;)
	{
	  if (verdef->vd_ndx == (*versym & 0x7fff))
	    // Found the version of the symbol.
	    break;
	  vd_offset += verdef->vd_next;
	  verdef = (verdef->vd_next == 0
		    ? 0
		    : gelf_getverdef(verdef_data, vd_offset, &verdef_mem));
	}

      if (verdef != 0)
	{
	  GElf_Verdaux verdaux_mem;
	  GElf_Verdaux *verdaux = gelf_getverdaux(verdef_data,
						  vd_offset + verdef->vd_aux,
						  &verdaux_mem);
	  GElf_Shdr header_mem;
	  GElf_Shdr* verdef_section_header = gelf_getshdr(verdef_section,
							  &header_mem);
	  size_t verdef_stridx = verdef_section_header->sh_link;
	  version.str(elf_strptr(elf_handle, verdef_stridx, verdaux->vda_name));
	  if (*versym & 0x8000)
	    version.is_default(false);
	  else
	    version.is_default(true);
	  return true;
	}
      if (!verdef || verdef->vd_next == 0)
	break;
    }
  return false;
}

/// Get the version needed (from the SHT_GNU_verneed section) to
/// resolve an undefined symbol represented by a pointer to
/// GElf_Versym.
///
/// @param elf_hande the elf handle to use.
///
/// @param versym the symbol to get the version definition for.
///
/// @param verneed_section the SHT_GNU_verneed section.
///
/// @param version the resulting version definition.  This is set iff
/// the function returns true.
///
/// @return true upon successful completion, false otherwise.
static bool
get_version_needed_for_versym(Elf*			elf_handle,
			      GElf_Versym*		versym,
			      Elf_Scn*			verneed_section,
			      elf_symbol::version&	version)
{
  if (versym == 0 || elf_handle == 0 || verneed_section == 0)
    return false;

  size_t vn_offset = 0;
  Elf_Data* verneed_data = elf_getdata(verneed_section, NULL);
  GElf_Verneed verneed_mem;
  GElf_Verneed* verneed = gelf_getverneed(verneed_data, 0, &verneed_mem);

  for (;verneed; vn_offset += verneed->vn_next)
    {
      size_t vna_offset = vn_offset;
      GElf_Vernaux vernaux_mem;
      GElf_Vernaux *vernaux = gelf_getvernaux(verneed_data,
					      vn_offset + verneed->vn_aux,
					      &vernaux_mem);
      for (;vernaux != 0 && verneed;)
	{
	  if (vernaux->vna_other == *versym)
	    // Found the version of the symbol.
	    break;
	  vna_offset += verneed->vn_next;
	  verneed = (verneed->vn_next == 0
		     ? 0
		     : gelf_getverneed(verneed_data, vna_offset, &verneed_mem));
	}

      if (verneed != 0 && vernaux != 0 && vernaux->vna_other == *versym)
	{
	  GElf_Shdr header_mem;
	  GElf_Shdr* verneed_section_header = gelf_getshdr(verneed_section,
							   &header_mem);
	  size_t verneed_stridx = verneed_section_header->sh_link;
	  version.str(elf_strptr(elf_handle,
				 verneed_stridx,
				 vernaux->vna_name));
	  if (*versym & 0x8000)
	    version.is_default(false);
	  else
	    version.is_default(true);
	  return true;
	}

      if (!verneed || verneed->vn_next == 0)
	break;
    }
  return false;
}

/// Return the version for a symbol that is at a given index in its
/// SHT_SYMTAB section.
///
/// @param elf_handle the elf handle to use.
///
/// @param symbol_index the index of the symbol to consider.
///
/// @param get_def_version if this is true, it means that that we want
/// the version for a defined symbol; in that case, the version is
/// looked for in a section of type SHT_GNU_verdef.  Otherwise, if
/// this parameter is false, this means that we want the version for
/// an undefined symbol; in that case, the version is the needed one
/// for the symbol to be resolved; so the version is looked fo in a
/// section of type SHT_GNU_verneed.
///
/// @param version the version found for symbol at @p symbol_index.
///
/// @return true iff a version was found for symbol at index @p
/// symbol_index.
static bool
get_version_for_symbol(Elf*			elf_handle,
		       size_t			symbol_index,
		       bool			get_def_version,
		       elf_symbol::version&	version)
{
  Elf_Scn *versym_section = NULL,
    *verdef_section = NULL,
    *verneed_section = NULL;

  if (!get_symbol_versionning_sections(elf_handle,
				       versym_section,
				       verdef_section,
				       verneed_section))
    return false;

  GElf_Versym versym_mem;
  Elf_Data* versym_data = (versym_section)
    ? elf_getdata(versym_section, NULL)
    : NULL;
  GElf_Versym* versym = (versym_data)
    ? gelf_getversym(versym_data, symbol_index, &versym_mem)
    : NULL;

  if (versym == 0 || *versym <= 1)
    // I got these value from the code of readelf.c in elfutils.
    // Apparently, if the symbol version entry has these values, the
    // symbol must be discarded. This is not documented in the
    // official specification.
    return false;

  if (get_def_version)
    {
      if (*versym == 0x8001)
	// I got this value from the code of readelf.c in elfutils
	// too.  It's not really documented in the official
	// specification.
	return false;

      if (verdef_section
	  && get_version_definition_for_versym(elf_handle, versym,
					       verdef_section, version))
	return true;
    }
  else
    {
      if (verneed_section
	  && get_version_needed_for_versym(elf_handle, versym,
					   verneed_section, version))
	return true;
    }

  return false;
}

/// Lookup a symbol using the SysV ELF hash table.
///
/// Note that this function hasn't been tested.  So it hasn't been
/// debugged yet.  IOW, it is not known to work.  Or rather, it's
/// almost like it's surely doesn't work ;-)
///
/// Use it at your own risks.  :-)
///
///@parm env the environment we are operating from.
///
/// @param elf_handle the elf_handle to use.
///
/// @param sym_name the symbol name to look for.
///
/// @param ht_index the index (in the section headers table) of the
/// hash table section to use.
///
/// @param sym_tab_index the index (in the section headers table) of
/// the symbol table to use.
///
/// @param demangle if true, demangle @p sym_name before comparing it
/// to names from the symbol table.
///
/// @param syms_found a vector of symbols found with the name @p
/// sym_name.  table.
static bool
lookup_symbol_from_sysv_hash_tab(const environment*		env,
				 Elf*				elf_handle,
				 const string&			sym_name,
				 size_t			ht_index,
				 size_t			sym_tab_index,
				 bool				demangle,
				 vector<elf_symbol_sptr>&	syms_found)
{
  Elf_Scn* sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  ABG_ASSERT(sym_tab_section);

  Elf_Data* sym_tab_data = elf_getdata(sym_tab_section, 0);
  ABG_ASSERT(sym_tab_data);

  GElf_Shdr sheader_mem;
  GElf_Shdr* sym_tab_section_header = gelf_getshdr(sym_tab_section,
						   &sheader_mem);
  Elf_Scn* hash_section = elf_getscn(elf_handle, ht_index);
  ABG_ASSERT(hash_section);

  // Poke at the different parts of the hash table and get them ready
  // to be used.
  unsigned long hash = elf_hash(sym_name.c_str());
  Elf_Data* ht_section_data = elf_getdata(hash_section, 0);
  Elf32_Word* ht_data = reinterpret_cast<Elf32_Word*>(ht_section_data->d_buf);
  size_t nb_buckets = ht_data[0];
  size_t nb_chains = ht_data[1];

  if (nb_buckets == 0)
    // An empty hash table.  Not sure if that is possible, but it
    // would mean an empty table of exported symbols.
    return false;

  //size_t nb_chains = ht_data[1];
  Elf32_Word* ht_buckets = &ht_data[2];
  Elf32_Word* ht_chains = &ht_buckets[nb_buckets];

  // Now do the real work.
  size_t bucket = hash % nb_buckets;
  size_t symbol_index = ht_buckets[bucket];

  GElf_Sym symbol;
  const char* sym_name_str;
  size_t sym_size;
  elf_symbol::type sym_type;
  elf_symbol::binding sym_binding;
  elf_symbol::visibility sym_visibility;
  bool found = false;

  do
    {
      ABG_ASSERT(gelf_getsym(sym_tab_data, symbol_index, &symbol));
      sym_name_str = elf_strptr(elf_handle,
				sym_tab_section_header->sh_link,
				symbol.st_name);
      if (sym_name_str
	  && compare_symbol_name(sym_name_str, sym_name, demangle))
	{
	  sym_type = stt_to_elf_symbol_type(GELF_ST_TYPE(symbol.st_info));
	  sym_binding = stb_to_elf_symbol_binding(GELF_ST_BIND(symbol.st_info));
	  sym_visibility =
	    stv_to_elf_symbol_visibility(GELF_ST_VISIBILITY(symbol.st_other));
	  sym_size = symbol.st_size;
	  elf_symbol::version ver;
	  if (get_version_for_symbol(elf_handle, symbol_index,
				     /*get_def_version=*/true, ver))
	    ABG_ASSERT(!ver.str().empty());
	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env,
			       symbol_index,
			       sym_size,
			       sym_name_str,
			       sym_type,
			       sym_binding,
			       symbol.st_shndx != SHN_UNDEF,
			       symbol.st_shndx == SHN_COMMON,
			       ver, sym_visibility);
	  syms_found.push_back(symbol_found);
	  found = true;
	}
      symbol_index = ht_chains[symbol_index];
    } while (symbol_index != STN_UNDEF || symbol_index >= nb_chains);

  return found;
}

/// Get the size of the elf class, in bytes.
///
/// @param elf_handle the elf handle to use.
///
/// @return the size computed.
static char
get_elf_class_size_in_bytes(Elf* elf_handle)
{
  char result = 0;
  GElf_Ehdr hdr;

  ABG_ASSERT(gelf_getehdr(elf_handle, &hdr));
  int c = hdr.e_ident[EI_CLASS];

  switch (c)
    {
    case ELFCLASS32:
      result = 4;
      break;
    case ELFCLASS64:
      result = 8;
      break;
    default:
      ABG_ASSERT_NOT_REACHED;
    }

  return result;
}

/// Get a given word of a bloom filter, referred to by the index of
/// the word.  The word size depends on the current elf class and this
/// function abstracts that nicely.
///
/// @param elf_handle the elf handle to use.
///
/// @param bloom_filter the bloom filter to consider.
///
/// @param index the index of the bloom filter to return.
static GElf_Word
bloom_word_at(Elf*		elf_handle,
	      Elf32_Word*	bloom_filter,
	      size_t		index)
{
  GElf_Word result = 0;
  GElf_Ehdr h;
  ABG_ASSERT(gelf_getehdr(elf_handle, &h));
  int c;
  c = h.e_ident[EI_CLASS];

  switch(c)
    {
    case ELFCLASS32:
      result = bloom_filter[index];
      break ;
    case ELFCLASS64:
      {
	GElf_Word* f= reinterpret_cast<GElf_Word*>(bloom_filter);
	result = f[index];
      }
      break;
    default:
      abort();
    }

  return result;
}

/// The abstraction of the gnu elf hash table.
///
/// The members of this struct are explained at
///   - https://sourceware.org/ml/binutils/2006-10/msg00377.html
///   - https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections.
struct gnu_ht
{
  size_t nb_buckets;
  Elf32_Word* buckets;
  Elf32_Word* chain;
  size_t first_sym_index;
  size_t bf_nwords;
  size_t bf_size;
  Elf32_Word* bloom_filter;
  size_t shift;
  size_t sym_count;
  Elf_Scn* sym_tab_section;
  GElf_Shdr sym_tab_section_header;

  gnu_ht()
    : nb_buckets(0),
      buckets(0),
      chain(0),
      first_sym_index(0),
      bf_nwords(0),
      bf_size(0),
      bloom_filter(0),
      shift(0),
      sym_count(0),
      sym_tab_section(0)
  {}
}; // end struct gnu_ht

/// Setup the members of the gnu hash table.
///
/// @param elf_handle a handle on the elf file to use.
///
/// @param ht_index the index  (into the elf section headers table) of
/// the hash table section to use.
///
/// @param sym_tab_index the index (into the elf section headers
/// table) of the symbol table the gnu hash table is about.
///
/// @param ht the resulting hash table.
///
/// @return true iff the hash table @ ht could be setup.
static bool
setup_gnu_ht(Elf* elf_handle,
	     size_t ht_index,
	     size_t sym_tab_index,
	     gnu_ht& ht)
{
  ht.sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  ABG_ASSERT(ht.sym_tab_section);
  ABG_ASSERT(gelf_getshdr(ht.sym_tab_section, &ht.sym_tab_section_header));
  ht.sym_count =
    ht.sym_tab_section_header.sh_size / ht.sym_tab_section_header.sh_entsize;
  Elf_Scn* hash_section = elf_getscn(elf_handle, ht_index);
  ABG_ASSERT(hash_section);

  // Poke at the different parts of the hash table and get them ready
  // to be used.
  Elf_Data* ht_section_data = elf_getdata(hash_section, 0);
  Elf32_Word* ht_data = reinterpret_cast<Elf32_Word*>(ht_section_data->d_buf);

  ht.nb_buckets = ht_data[0];
  if (ht.nb_buckets == 0)
    // An empty hash table.  Not sure if that is possible, but it
    // would mean an empty table of exported symbols.
    return false;
  ht.first_sym_index = ht_data[1];
  // The number of words used by the bloom filter.  A size of a word
  // is ELFCLASS.
  ht.bf_nwords = ht_data[2];
  // The shift used by the bloom filter code.
  ht.shift = ht_data[3];
  // The data of the bloom filter proper.
  ht.bloom_filter = &ht_data[4];
  // The size of the bloom filter in 4 bytes word.  This is going to
  // be used to index the 'bloom_filter' above, which is of type
  // Elf32_Word*; thus we need that bf_size be expressed in 4 bytes
  // words.
  ht.bf_size = (get_elf_class_size_in_bytes(elf_handle) / 4) * ht.bf_nwords;
  // The buckets of the hash table.
  ht.buckets = ht.bloom_filter + ht.bf_size;
  // The chain of the hash table.
  ht.chain = ht.buckets + ht.nb_buckets;

  return true;
}

/// Look into the symbol tables of the underlying elf file and find
/// the symbol we are being asked.
///
/// This function uses the GNU hash table for the symbol lookup.
///
/// The reference of for the implementation of this function can be
/// found at:
///   - https://sourceware.org/ml/binutils/2006-10/msg00377.html
///   - https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections.
///
/// @param elf_handle the elf handle to use.
///
/// @param sym_name the name of the symbol to look for.
///
/// @param ht_index the index of the hash table header to use.
///
/// @param sym_tab_index the index of the symbol table header to use
/// with this hash table.
///
/// @param demangle if true, demangle @p sym_name.
///
/// @param syms_found the vector of symbols found with the name @p
/// sym_name.
///
/// @return true if a symbol was actually found.
static bool
lookup_symbol_from_gnu_hash_tab(const environment*		env,
				Elf*				elf_handle,
				const string&			sym_name,
				size_t				ht_index,
				size_t				sym_tab_index,
				bool				demangle,
				vector<elf_symbol_sptr>&	syms_found)
{
  gnu_ht ht;
  if (!setup_gnu_ht(elf_handle, ht_index, sym_tab_index, ht))
    return false;

  // Now do the real work.

  // Compute bloom hashes (GNU hash and second bloom specific hashes).
  size_t h1 = elf_gnu_hash(sym_name.c_str());
  size_t h2 = h1 >> ht.shift;
  // The size of one of the words used in the bloom
  // filter, in bits.
  int c = get_elf_class_size_in_bytes(elf_handle) * 8;
  int n =  (h1 / c) % ht.bf_nwords;
  unsigned char bitmask = (1 << (h1 % c)) | (1 << (h2 % c));

  // Test if the symbol is *NOT* present in this ELF file.
  if ((bloom_word_at(elf_handle, ht.bloom_filter, n) & bitmask) != bitmask)
    return false;

  size_t i = ht.buckets[h1 % ht.nb_buckets];
  if (i == STN_UNDEF)
    return false;

  Elf32_Word stop_word, *stop_wordp;
  elf_symbol::version ver;
  GElf_Sym symbol;
  const char* sym_name_str;
  bool found = false;

  elf_symbol::type sym_type;
  elf_symbol::binding sym_binding;
  elf_symbol::visibility sym_visibility;

  // Let's walk the hash table and record the versions of all the
  // symbols which name equal sym_name.
  for (i = ht.buckets[h1 % ht.nb_buckets],
	 stop_wordp = &ht.chain[i - ht.first_sym_index];
       i != STN_UNDEF
	 && (stop_wordp
	     < ht.chain + (ht.sym_count - ht.first_sym_index));
       ++i, ++stop_wordp)
    {
      stop_word = *stop_wordp;
      if ((stop_word & ~ 1)!= (h1 & ~1))
	// A given bucket can reference several hashes.  Here we
	// stumbled across a hash value different from the one we are
	// looking for.  Let's keep walking.
	continue;

      ABG_ASSERT(gelf_getsym(elf_getdata(ht.sym_tab_section, 0),
			 i, &symbol));
      sym_name_str = elf_strptr(elf_handle,
				ht.sym_tab_section_header.sh_link,
				symbol.st_name);
      if (sym_name_str
	  && compare_symbol_name(sym_name_str, sym_name, demangle))
	{
	  // So we found a symbol (in the symbol table) that equals
	  // sym_name.  Now lets try to get its version and record it.
	  sym_type = stt_to_elf_symbol_type(GELF_ST_TYPE(symbol.st_info));
	  sym_binding = stb_to_elf_symbol_binding(GELF_ST_BIND(symbol.st_info));
	 sym_visibility =
	   stv_to_elf_symbol_visibility(GELF_ST_VISIBILITY(symbol.st_other));

	  if (get_version_for_symbol(elf_handle, i,
				     /*get_def_version=*/true,
				     ver))
	    ABG_ASSERT(!ver.str().empty());

	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env, i, symbol.st_size, sym_name_str,
			       sym_type, sym_binding,
			       symbol.st_shndx != SHN_UNDEF,
			       symbol.st_shndx == SHN_COMMON,
			       ver, sym_visibility);
	  syms_found.push_back(symbol_found);
	  found = true;
	}

      if (stop_word & 1)
	// The last bit of the stop_word is 1.  That means we need to
	// stop here.  We reached the end of the chain of values
	// referenced by the hask bucket.
	break;
    }
  return found;
}

/// Look into the symbol tables of the underlying elf file and find
/// the symbol we are being asked.
///
/// This function uses the elf hash table (be it the GNU hash table or
/// the sysv hash table) for the symbol lookup.
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use.
///
/// @param ht_kind the kind of hash table to use.  This is returned by
/// the function function find_hash_table_section_index.
///
/// @param ht_index the index (in the section headers table) of the
/// hash table section to use.
///
/// @param sym_tab_index the index (in section headers table) of the
/// symbol table index to use with this hash table.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, demangle @p sym_name.
///
/// @param syms_found the symbols that were actually found with the
/// name @p symbol_name.
///
/// @return true iff the function found the symbol from the elf hash
/// table.
static bool
lookup_symbol_from_elf_hash_tab(const environment*		env,
				Elf*				elf_handle,
				hash_table_kind		ht_kind,
				size_t				ht_index,
				size_t				symtab_index,
				const string&			symbol_name,
				bool				demangle,
				vector<elf_symbol_sptr>&	syms_found)
{
  if (elf_handle == 0 || symbol_name.empty())
    return false;

  if (ht_kind == NO_HASH_TABLE_KIND)
    return false;

  if (ht_kind == SYSV_HASH_TABLE_KIND)
    return lookup_symbol_from_sysv_hash_tab(env,
					    elf_handle, symbol_name,
					    ht_index,
					    symtab_index,
					    demangle,
					    syms_found);
  else if (ht_kind == GNU_HASH_TABLE_KIND)
    return lookup_symbol_from_gnu_hash_tab(env,
					   elf_handle, symbol_name,
					   ht_index,
					   symtab_index,
					   demangle,
					   syms_found);
  return false;
}

/// Lookup a symbol from the symbol table directly.
///
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use.
///
/// @param sym_name the name of the symbol to look up.
///
/// @param sym_tab_index the index (in the section headers table) of
/// the symbol table section.
///
/// @param demangle if true, demangle the names found in the symbol
/// table before comparing them with @p sym_name.
///
/// @param sym_name_found the actual name of the symbol found.
///
/// @param sym_type the type of the symbol found.
///
/// @param sym_binding the binding of the symbol found.
///
/// @param sym_versions the versions of the symbol found.
///
/// @return true iff the symbol was found.
static bool
lookup_symbol_from_symtab(const environment*		env,
			  Elf*				elf_handle,
			  const string&		sym_name,
			  size_t			sym_tab_index,
			  bool				demangle,
			  vector<elf_symbol_sptr>&	syms_found)
{
  // TODO: read all of the symbol table, store it in memory in a data
  // structure that associates each symbol with its versions and in
  // which lookups of a given symbol is fast.
  Elf_Scn* sym_tab_section = elf_getscn(elf_handle, sym_tab_index);
  ABG_ASSERT(sym_tab_section);

  GElf_Shdr header_mem;
  GElf_Shdr * sym_tab_header = gelf_getshdr(sym_tab_section,
					    &header_mem);

  size_t symcount = sym_tab_header->sh_size / sym_tab_header->sh_entsize;
  Elf_Data* symtab = elf_getdata(sym_tab_section, NULL);
  GElf_Sym* sym;
  char* name_str = 0;
  elf_symbol::version ver;
  bool found = false;

  for (size_t i = 0; i < symcount; ++i)
    {
      GElf_Sym sym_mem;
      sym = gelf_getsym(symtab, i, &sym_mem);
      name_str = elf_strptr(elf_handle,
			    sym_tab_header->sh_link,
			    sym->st_name);

      if (name_str && compare_symbol_name(name_str, sym_name, demangle))
	{
	  elf_symbol::type sym_type =
	    stt_to_elf_symbol_type(GELF_ST_TYPE(sym->st_info));
	  elf_symbol::binding sym_binding =
	    stb_to_elf_symbol_binding(GELF_ST_BIND(sym->st_info));
	  elf_symbol::visibility sym_visibility =
	    stv_to_elf_symbol_visibility(GELF_ST_VISIBILITY(sym->st_other));
	  bool sym_is_defined = sym->st_shndx != SHN_UNDEF;
	  bool sym_is_common = sym->st_shndx == SHN_COMMON;

	  if (get_version_for_symbol(elf_handle, i,
				     /*get_def_version=*/sym_is_defined,
				     ver))
	    ABG_ASSERT(!ver.str().empty());
	  elf_symbol_sptr symbol_found =
	    elf_symbol::create(env, i, sym->st_size,
			       name_str, sym_type,
			       sym_binding, sym_is_defined,
			       sym_is_common, ver, sym_visibility);
	  syms_found.push_back(symbol_found);
	  found = true;
	}
    }

  if (found)
    return true;

  return false;
}

/// Look into the symbol tables of the underlying elf file and see
/// if we find a given symbol.
///
/// @param env the environment we are operating from.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, try to demangle the symbol name found in
/// the symbol table before comparing it to @p symbol_name.
///
/// @param syms_found the list of symbols found, with the name @p
/// symbol_name.
///
/// @param sym_type this is set to the type of the symbol found.  This
/// shall b a standard elf.h value for symbol types, that is SHT_OBJECT,
/// STT_FUNC, STT_IFUNC, etc ...
///
/// Note that this parameter is set iff the function returns true.
///
/// @param sym_binding this is set to the binding of the symbol found.
/// This is a standard elf.h value of the symbol binding kind, that
/// is, STB_LOCAL, STB_GLOBAL, or STB_WEAK.
///
/// @param symbol_versions the versions of the symbol @p symbol_name,
/// if it was found.
///
/// @return true iff a symbol with the name @p symbol_name was found.
static bool
lookup_symbol_from_elf(const environment*		env,
		       Elf*				elf_handle,
		       const string&			symbol_name,
		       bool				demangle,
		       vector<elf_symbol_sptr>&	syms_found)
{
  size_t hash_table_index = 0, symbol_table_index = 0;
  hash_table_kind ht_kind = NO_HASH_TABLE_KIND;

  if (!demangle)
    ht_kind = find_hash_table_section_index(elf_handle,
					    hash_table_index,
					    symbol_table_index);

  if (ht_kind == NO_HASH_TABLE_KIND)
    {
      if (!find_symbol_table_section_index(elf_handle, symbol_table_index))
	return false;

      return lookup_symbol_from_symtab(env,
				       elf_handle,
				       symbol_name,
				       symbol_table_index,
				       demangle,
				       syms_found);
    }

  return lookup_symbol_from_elf_hash_tab(env,
					 elf_handle,
					 ht_kind,
					 hash_table_index,
					 symbol_table_index,
					 symbol_name,
					 demangle,
					 syms_found);
}

/// Look into the symbol tables of the underlying elf file and see if
/// we find a given public (global or weak) symbol of function type.
///
/// @param env the environment we are operating from.
///
/// @param elf_handle the elf handle to use for the query.
///
/// @param symbol_name the function symbol to look for.
///
/// @param func_syms the vector of public functions symbols found, if
/// any.
///
/// @return true iff the symbol was found.
static bool
lookup_public_function_symbol_from_elf(const environment*		env,
				       Elf*				elf_handle,
				       const string&			symbol_name,
				       vector<elf_symbol_sptr>&	func_syms)
{
  vector<elf_symbol_sptr> syms_found;
  bool found = false;

  if (lookup_symbol_from_elf(env, elf_handle, symbol_name,
			     /*demangle=*/false, syms_found))
    {
      for (vector<elf_symbol_sptr>::const_iterator i = syms_found.begin();
	   i != syms_found.end();
	   ++i)
	{
	  elf_symbol::type type = (*i)->get_type();
	  elf_symbol::binding binding = (*i)->get_binding();

	  if ((type == elf_symbol::FUNC_TYPE
	       || type == elf_symbol::GNU_IFUNC_TYPE
	       || type == elf_symbol::COMMON_TYPE)
	      && (binding == elf_symbol::GLOBAL_BINDING
		  || binding == elf_symbol::WEAK_BINDING))
	    {
	      func_syms.push_back(*i);
	      found = true;
	    }
	}
    }

  return found;
}

/// Look into the symbol tables of the underlying elf file and see if
/// we find a given public (global or weak) symbol of variable type.
///
/// @param env the environment we are operating from.
///
/// @param elf the elf handle to use for the query.
///
/// @param symname the variable symbol to look for.
///
/// @param var_syms the vector of public variable symbols found, if any.
///
/// @return true iff symbol @p symname was found.
static bool
lookup_public_variable_symbol_from_elf(const environment*		env,
				       Elf*				elf,
				       const string&			symname,
				       vector<elf_symbol_sptr>&	var_syms)
{
  vector<elf_symbol_sptr> syms_found;
  bool found = false;

  if (lookup_symbol_from_elf(env, elf, symname, /*demangle=*/false, syms_found))
    {
      for (vector<elf_symbol_sptr>::const_iterator i = syms_found.begin();
	   i != syms_found.end();
	   ++i)
	if ((*i)->is_variable()
	    && ((*i)->get_binding() == elf_symbol::GLOBAL_BINDING
		|| (*i)->get_binding() == elf_symbol::WEAK_BINDING))
	  {
	    var_syms.push_back(*i);
	    found = true;
	  }
    }

  return found;
}

/// Get data tag information of an ELF file by looking up into its
/// dynamic segment
///
/// @param elf the elf handle to use for the query.
///
/// @param dt_tag data tag to look for in dynamic segment
/// @param dt_tag_data vector of found information for a given @p data_tag
///
/// @return true iff data tag @p data_tag was found

bool
lookup_data_tag_from_dynamic_segment(Elf*                       elf,
                                     Elf64_Sxword               data_tag,
                                     vector<string>&            dt_tag_data)
{
  size_t num_prog_headers = 0;
  bool found = false;
  if (elf_getphdrnum(elf, &num_prog_headers) < 0)
    return found;

  // Cycle through each program header.
  for (size_t i = 0; i < num_prog_headers; ++i)
    {
      GElf_Phdr phdr_mem;
      GElf_Phdr *phdr = gelf_getphdr(elf, i, &phdr_mem);
      if (phdr == NULL || phdr->p_type != PT_DYNAMIC)
        continue;

      // Poke at the dynamic segment like a section, so that we can
      // get its section header information; also we'd like to read
      // the data of the segment by using elf_getdata() but that
      // function needs a Elf_Scn data structure to act on.
      // Elfutils doesn't really have any particular function to
      // access segment data, other than the functions used to
      // access section data.
      Elf_Scn *dynamic_section = gelf_offscn(elf, phdr->p_offset);
      GElf_Shdr  shdr_mem;
      GElf_Shdr *dynamic_section_header = gelf_getshdr(dynamic_section,
						       &shdr_mem);
      if (dynamic_section_header == NULL
          || dynamic_section_header->sh_type != SHT_DYNAMIC)
        continue;

      // Get data of the dynamic segment (seen as a section).
      Elf_Data *data = elf_getdata(dynamic_section, NULL);
      if (data == NULL)
        continue;

      // Get the index of the section headers string table.
      size_t string_table_index = 0;
      ABG_ASSERT (elf_getshdrstrndx(elf, &string_table_index) >= 0);

      size_t dynamic_section_header_entry_size = gelf_fsize(elf,
                                                            ELF_T_DYN, 1,
                                                            EV_CURRENT);

      GElf_Shdr link_mem;
      GElf_Shdr *link =
        gelf_getshdr(elf_getscn(elf,
                                dynamic_section_header->sh_link),
		     &link_mem);
      ABG_ASSERT(link != NULL);

      size_t num_dynamic_section_entries =
        dynamic_section_header->sh_size / dynamic_section_header_entry_size;

      // Now walk through all the DT_* data tags that are in the
      // segment/section
      for (size_t j = 0; j < num_dynamic_section_entries; ++j)
        {
          GElf_Dyn dynamic_section_mem;
          GElf_Dyn *dynamic_section = gelf_getdyn(data,
                                                  j,
                                                  &dynamic_section_mem);
          if (dynamic_section->d_tag == data_tag)
            {
              dt_tag_data.push_back(elf_strptr(elf,
                                               dynamic_section_header->sh_link,
					       dynamic_section->d_un.d_val));
              found = true;
            }
        }
    }
  return found;
}

/// Convert the type of ELF file into @ref elf_type.
///
/// @param elf the elf handle to use for the query.
///
/// @return the @ref elf_type for a given elf type.
static elf_type
elf_file_type(Elf* elf)
{
  GElf_Ehdr ehdr_mem;
  GElf_Ehdr *header = gelf_getehdr (elf, &ehdr_mem);
  vector<string> dt_debug_data;

  switch (header->e_type)
    {
    case ET_DYN:
      if (lookup_data_tag_from_dynamic_segment(elf, DT_DEBUG, dt_debug_data))
	return ELF_TYPE_PI_EXEC;
      else
	return ELF_TYPE_DSO;
    case ET_EXEC:
      return ELF_TYPE_EXEC;
    case ET_REL:
      return ELF_TYPE_RELOCATABLE;
    default:
      return ELF_TYPE_UNKNOWN;
    }
}

// ---------------------------------------
// <location expression evaluation types>
// ---------------------------------------

/// An abstraction of a value representing the result of the
/// evaluation of a dwarf expression.  This is abstraction represents
/// a partial view on the possible values because we are only
/// interested in extracting the latest and longuest constant
/// sub-expression of a given dwarf expression.
class expr_result
{
  bool is_const_;
  int64_t const_value_;

public:
  expr_result()
    : is_const_(true),
      const_value_(0)
  {}

  expr_result(bool is_const)
    : is_const_(is_const),
      const_value_(0)
  {}

  explicit expr_result(int64_t v)
    :is_const_(true),
     const_value_(v)
  {}

  /// @return true if the value is a constant.  Otherwise, return
  /// false, meaning the value represents a quantity for which we need
  /// inferior (a running program) state to determine the value.
  bool
  is_const() const
  {return is_const_;}


  /// @param f a flag saying if the value is set to a constant or not.
  void
  is_const(bool f)
  {is_const_ = f;}

  /// Get the current constant value iff this represents a
  /// constant.
  ///
  /// @param value the out parameter.  Is set to the constant value of
  /// the @ref expr_result.  This is set iff the function return true.
  ///
  ///@return true if this has a constant value, false otherwise.
  bool
  const_value(int64_t& value)
  {
    if (is_const())
      {
	value = const_value_;
	return true;
      }
    return false;
  }

  /// Getter of the constant value of the current @ref expr_result.
  ///
  /// Note that the current @ref expr_result must be constant,
  /// otherwise the current process is aborted.
  ///
  /// @return the constant value of the current @ref expr_result.
  int64_t
  const_value() const
  {
    ABG_ASSERT(is_const());
    return const_value_;
  }

  operator int64_t() const
  {return const_value();}

  expr_result&
  operator=(const int64_t v)
  {
    const_value_ = v;
    return *this;
  }

  bool
  operator==(const expr_result& o) const
  {return const_value_ == o.const_value_ && is_const_ == o.is_const_;}

  bool
  operator>=(const expr_result& o) const
  {return const_value_ >= o.const_value_;}

  bool
  operator<=(const expr_result& o) const
  {return const_value_ <= o.const_value_;}

  bool
  operator>(const expr_result& o) const
  {return const_value_ > o.const_value_;}

  bool
  operator<(const expr_result& o) const
  {return const_value_ < o.const_value_;}

  expr_result
  operator+(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ += v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result&
  operator+=(int64_t v)
  {
    const_value_ += v;
    return *this;
  }

  expr_result
  operator-(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ -= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator%(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ %= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const();
    return r;
  }

  expr_result
  operator*(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ *= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const();
    return r;
  }

  expr_result
  operator|(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ |= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator^(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ ^= v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator>>(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ = r.const_value_ >> v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator<<(const expr_result& v) const
  {
    expr_result r(*this);
    r.const_value_ = r.const_value_ << v.const_value_;
    r.is_const_ = r.is_const_ && v.is_const_;
    return r;
  }

  expr_result
  operator~() const
  {
    expr_result r(*this);
    r.const_value_ = ~r.const_value_;
    return r;
  }

  expr_result
  neg() const
  {
    expr_result r(*this);
    r.const_value_ = -r.const_value_;
    return r;
  }

  expr_result
  abs() const
  {
    expr_result r = *this;
    r.const_value_ = std::abs(static_cast<long double>(r.const_value()));
    return r;
  }

  expr_result
  operator&(const expr_result& o)
  {
    expr_result r(*this);
    r.const_value_ &= o.const_value_;
    r.is_const_ = r.is_const_ && o.is_const_;
    return r;
  }

  expr_result
  operator/(const expr_result& o)
  {
    expr_result r(*this);
    r.is_const_ = r.is_const_ && o.is_const_;
    return r.const_value() / o.const_value();
  }
};// class end expr_result;

/// A class that implements a stack of @ref expr_result, to be used in
/// the engine evaluating DWARF expressions.
class expr_result_stack_type
{
  vector<expr_result> elems_;

public:

  expr_result_stack_type()
  {elems_.reserve(4);}

  expr_result&
  operator[](unsigned i)
  {
    unsigned s = elems_.size();
    ABG_ASSERT(s > i);
    return elems_[s - 1 -i];
  }

  const expr_result&
  operator[](unsigned i) const
  {return const_cast<expr_result_stack_type*>(this)->operator[](i);}

  unsigned
  size() const
  {return elems_.size();}

  vector<expr_result>::reverse_iterator
  begin()
  {return elems_.rbegin();}

  const vector<expr_result>::reverse_iterator
  begin() const
  {return const_cast<expr_result_stack_type*>(this)->begin();}

  vector<expr_result>::reverse_iterator
  end()
  {return elems_.rend();}

  const vector<expr_result>::reverse_iterator
  end() const
  {return const_cast<expr_result_stack_type*>(this)->end();}

  expr_result&
  front()
  {return elems_.back();}

  const expr_result&
  front() const
  {return const_cast<expr_result_stack_type*>(this)->front();}

  void
  push_front(expr_result e)
  {elems_.push_back(e);}

  expr_result
  pop_front()
  {
    expr_result r = front();
    elems_.pop_back();
    return r;
  }

  void
  erase(vector<expr_result>::reverse_iterator i)
  {elems_.erase(--i.base());}

  void
  clear()
  {elems_.clear();}
}; // end class expr_result_stack_type

/// Abstraction of the evaluation context of a dwarf expression.
struct dwarf_expr_eval_context
{
  expr_result accum;
  expr_result_stack_type stack;
  // Is set to true if the result of the expression that got evaluated
  // is a TLS address.
  bool set_tls_addr;

  dwarf_expr_eval_context()
    : accum(/*is_const=*/false),
      set_tls_addr(false)
  {
    stack.push_front(expr_result(true));
  }

  void
  reset()
  {
    stack.clear();
    stack.push_front(expr_result(true));
    accum = expr_result(false);
    set_tls_addr = false;
  }

  /// Set a flag to to tell that the result of the expression that got
  /// evaluated is a TLS address.
  ///
  /// @param f true iff the result of the expression that got
  /// evaluated is a TLS address, false otherwise.
  void
  set_tls_address(bool f)
  {set_tls_addr = f;}

  /// Getter for the flag that tells if the result of the expression
  /// that got evaluated is a TLS address.
  ///
  /// @return true iff the result of the expression that got evaluated
  /// is a TLS address.
  bool
  set_tls_address() const
  {return set_tls_addr;}

  expr_result
  pop()
  {
    expr_result r = stack.front();
    stack.pop_front();
    return r;
  }

  void
  push(const expr_result& v)
  {stack.push_front(v);}
};//end class dwarf_expr_eval_context

// ---------------------------------------
// </location expression evaluation types>
// ---------------------------------------

/// An enum for the diffent kinds of linux kernel specific symbol
/// tables.
enum kernel_symbol_table_kind
{
  /// This is for an undefined kind of kernel symbol table.
  KERNEL_SYMBOL_TABLE_KIND_UNDEFINED,

  /// The __ksymtab symbol table.
  KERNEL_SYMBOL_TABLE_KIND_KSYMTAB,

  /// The __ksymtab_gpl symbol table.
  KERNEL_SYMBOL_TABLE_KIND_KSYMTAB_GPL
};

/// An enum which specifies the format of the kernel symbol table
/// (__ksymtab or __ksymtab_gpl).
enum ksymtab_format
{
  /// This enumerator means that no __ksymtab format has been
  /// determined yet.
  UNDEFINED_KSYMTAB_FORMAT,

  /// Before Linux v4.19, the format of the __ksymtab (and the
  /// __ksymtab_gpl) section was the following.
  ///
  /// It's an array of entries.  Each entry describes a symbol.  Each
  /// entry is made of two words.  each is of the word size of the
  /// architecture. (8-bytes on a 64 bits arch and 4-bytes on a 32
  /// bits arch) The first word is the address of a symbol.  The
  /// second one is the address of a static global variable symbol
  /// which value is the string representing the symbol name.  That
  /// string is in the __ksymtab_strings section.
  ///
  /// So we are mostly interested in the symbol address part of each
  /// entry.
  ///
  /// Thus this enumerator means that we have the pre v4.19 __ksymtab
  /// section format.
  PRE_V4_19_KSYMTAB_FORMAT,

  /// Since, Linux v4.19, the format of the __ksymtab section has
  /// changed.  The commit that changed is
  /// https://github.com/torvalds/linux/commit/7290d58095712a89f845e1bca05334796dd49ed2.
  ///
  /// The __ksymtab and __ksymtab_gpl sections each are an array of
  /// entries.  Each entry describes a symbol.  Each entry is made of
  /// two words.  Each word is 4-bytes length.  The first word is the
  /// 'place-relative' address of a symbol.  The second one is the
  /// 'place-relative' address of a static global variable symbol
  /// which value is the string representing the symbol name.  That
  /// string is in the __ksymtab_strings section.
  ///
  /// Below is the description of what a "place-relative address"
  /// means.  For that, we are going to define the meaning of four
  /// values: 'N', 'S', 'O', and 'A'.
  ///
  /// *** 'N' and '0' ***
  /// Suppose 'N' is the value of the number stored at offset 'O' (big
  /// oh, not zero) in the __ksymtab section.
  ///
  /// *** 'S'***
  /// That N designates a symbol in the symtab section which value is
  /// S.  So S is the symbol value (in the .symtab symbol table)
  /// referred to by the number N found at offset 'O'.
  ///
  /// *** 'A' ***
  /// Also, suppose the __ksymtab section will be loaded at memory
  /// address A, as indicated by the 'address' field of the section
  /// header describing the __ksymtab section.
  ///
  /// So here is the formula that gives us S, from N:
  ///
  ///     S = N + O + A.
  ///
  /// Storing addresses this way does away with the need to have
  /// relocations for the __ksymtab section.  So in effect, vmlinux
  /// binaries implementing this new format of __ksymtab won't have
  /// any .rela__ksymtab relocation section for the __ksymtab section
  /// in particular (nor any relocation section at all).
  ///
  ///
  /// Note that we are mostly interested in the symbol address part of
  /// each entry.
  V4_19_KSYMTAB_FORMAT
}; // end enum ksymtab_format

/// The context used to build ABI corpus from debug info in DWARF
/// format.
///
/// This context is to be created by create_read_context().  It's then
/// passed to all the routines that read specific dwarf bits as they
/// get some important data from it.
///
/// When a new data member is added to this context, it must be
/// initiliazed by the read_context::initiliaze() function.  So please
/// do not forget.
class read_context
{
public:
  struct options_type
  {
    environment*	env;
    bool		load_in_linux_kernel_mode;
    bool		load_all_types;
    bool		ignore_symbol_table;
    bool		show_stats;
    bool		do_log;

    options_type()
      : env(),
	load_in_linux_kernel_mode(),
	load_all_types(),
	ignore_symbol_table(),
	show_stats(),
	do_log()
    {}
  };// read_context::options_type

  /// A set of containers that contains one container per kind of @ref
  /// die_source.  This allows to associate DIEs to things, depending
  /// on the source of the DIE.
  template <typename ContainerType>
  class die_source_dependant_container_set
  {
    ContainerType primary_debug_info_container_;
    ContainerType alt_debug_info_container_;
    ContainerType type_unit_container_;

  public:

    /// Getter for the container associated to DIEs coming from a
    /// given @ref die_source.
    ///
    /// @param source the die_source for which we want the container.
    ///
    /// @return the container that associates DIEs coming from @p
    /// source to something.
    ContainerType&
    get_container(die_source source)
    {
      ContainerType *result = 0;
      switch (source)
	{
	case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	  result = &primary_debug_info_container_;
	  break;
	case ALT_DEBUG_INFO_DIE_SOURCE:
	  result = &alt_debug_info_container_;
	  break;
	case TYPE_UNIT_DIE_SOURCE:
	  result = &type_unit_container_;
	  break;
	case NO_DEBUG_INFO_DIE_SOURCE:
	case NUMBER_OF_DIE_SOURCES:
	  ABG_ASSERT_NOT_REACHED;
	}
      return *result;
    }

    /// Getter for the container associated to DIEs coming from a
    /// given @ref die_source.
    ///
    /// @param source the die_source for which we want the container.
    ///
    /// @return the container that associates DIEs coming from @p
    /// source to something.
    const ContainerType&
    get_container(die_source source) const
    {
      return const_cast<die_source_dependant_container_set*>(this)->
	get_container(source);
    }

    /// Getter for the container associated to DIEs coming from the
    /// same source as a given DIE.
    ///
    /// @param ctxt the read context to consider.
    ///
    /// @param die the DIE which should have the same source as the
    /// source of the container we want.
    ///
    /// @return the container that associates DIEs coming from the
    /// same source as @p die.
    ContainerType&
    get_container(const read_context& ctxt, Dwarf_Die *die)
    {
      die_source source = NO_DEBUG_INFO_DIE_SOURCE;
      ABG_ASSERT(ctxt.get_die_source(die, source));
      return get_container(source);
    }

    /// Getter for the container associated to DIEs coming from the
    /// same source as a given DIE.
    ///
    /// @param ctxt the read context to consider.
    ///
    /// @param die the DIE which should have the same source as the
    /// source of the container we want.
    ///
    /// @return the container that associates DIEs coming from the
    /// same source as @p die.
    const ContainerType&
    get_container(const read_context& ctxt, Dwarf_Die *die) const
    {
      return const_cast<die_source_dependant_container_set*>(this)->
	get_container(ctxt, die);
    }

    /// Clear the container set.
    void
    clear()
    {
      primary_debug_info_container_.clear();
      alt_debug_info_container_.clear();
      type_unit_container_.clear();
    }
  }; // end die_dependant_container_set

  suppr::suppressions_type	supprs_;
  unsigned short		dwarf_version_;
  Dwfl_Callbacks		offline_callbacks_;
  // The set of directories under which to look for debug info.
  vector<char**>		debug_info_root_paths_;
  dwfl_sptr			handle_;
  Dwarf*			dwarf_;
  // The alternate debug info.  Alternate debug info sections are a
  // DWARF extension as of DWARF4 and are described at
  // http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.  Below are
  // the file desctor used to access the alternate debug info
  // sections, and the representation of the DWARF debug info.  Both
  // need to be freed after we are done using them, with fclose and
  // dwarf_end.
  int				alt_fd_;
  Dwarf*			alt_dwarf_;
  string			alt_debug_info_path_;
  // The address range of the offline elf file we are looking at.
  Dwfl_Module*			elf_module_;
  mutable Elf*			elf_handle_;
  string			elf_path_;
  mutable Elf_Scn*		bss_section_;
  mutable Elf_Scn*		text_section_;
  mutable Elf_Scn*		rodata_section_;
  mutable Elf_Scn*		data_section_;
  mutable Elf_Scn*		data1_section_;
  mutable Elf_Scn*		symtab_section_;
  // The "Official procedure descriptor section, aka .opd", used in
  // ppc64 elf v1 binaries.  This section contains the procedure
  // descriptors on that platform.
  Elf_Scn*			opd_section_;
  /// The format of the special __ksymtab section from the linux
  /// kernel binary.
  mutable ksymtab_format	ksymtab_format_;
  /// The size of one entry of the __ksymtab section.
  mutable size_t		ksymtab_entry_size_;
  /// The number of entries in the __ksymtab section.
  mutable size_t		nb_ksymtab_entries_;
  /// The number of entries in the __ksymtab_gpl section.
  mutable size_t		nb_ksymtab_gpl_entries_;
  /// The special __ksymtab and __ksymtab_gpl sections from linux
  /// kernel or module binaries.  The former is used to store
  /// references to symbols exported using the EXPORT_SYMBOL macro
  /// from the linux kernel.  The latter is used to store references
  /// to symbols exported using the EXPORT_SYMBOL_GPL macro from the
  /// linux kernel.
  Elf_Scn*			ksymtab_section_;
  Elf_Scn*			ksymtab_gpl_section_;
  Elf_Scn*			versym_section_;
  Elf_Scn*			verdef_section_;
  Elf_Scn*			verneed_section_;
  bool				symbol_versionning_sections_loaded_;
  bool				symbol_versionning_sections_found_;
  Dwarf_Die*			cur_tu_die_;
  mutable dwarf_expr_eval_context	dwarf_expr_eval_context_;
  // A set of maps (one per kind of die source) that associates a decl
  // string representation with the DIEs (offsets) representing that
  // decl.
  mutable die_source_dependant_container_set<istring_dwarf_offsets_map_type>
  decl_die_repr_die_offsets_maps_;
  // A set of maps (one per kind of die source) that associates a type
  // string representation with the DIEs (offsets) representing that
  // type.
  mutable die_source_dependant_container_set<istring_dwarf_offsets_map_type>
  type_die_repr_die_offsets_maps_;
  mutable die_source_dependant_container_set<die_istring_map_type>
  die_qualified_name_maps_;
  mutable die_source_dependant_container_set<die_istring_map_type>
  die_pretty_repr_maps_;
  mutable die_source_dependant_container_set<die_istring_map_type>
  die_pretty_type_repr_maps_;
  // A set of maps (one per kind of die source) that associates the
  // offset of a decl die to its corresponding decl artifact.
  mutable die_source_dependant_container_set<die_artefact_map_type>
  decl_die_artefact_maps_;
  // A set of maps (one per kind of die source) that associates the
  // offset of a type die to its corresponding type artifact.
  mutable die_source_dependant_container_set<die_artefact_map_type>
  type_die_artefact_maps_;
  /// A set of vectors (one per kind of die source) that associates
  /// the offset of a type DIE to the offset of its canonical DIE.
  mutable die_source_dependant_container_set<offset_offset_map_type>
  canonical_type_die_offsets_;
  /// A set of vectors (one per kind of die source) that associates
  /// the offset of a decl DIE to the offset of its canonical DIE.
  mutable die_source_dependant_container_set<offset_offset_map_type>
  canonical_decl_die_offsets_;
  die_class_or_union_map_type	die_wip_classes_map_;
  die_class_or_union_map_type	alternate_die_wip_classes_map_;
  die_class_or_union_map_type	type_unit_die_wip_classes_map_;
  die_function_type_map_type	die_wip_function_types_map_;
  die_function_type_map_type	alternate_die_wip_function_types_map_;
  die_function_type_map_type	type_unit_die_wip_function_types_map_;
  die_function_decl_map_type	die_function_with_no_symbol_map_;
  vector<Dwarf_Off>		types_to_canonicalize_;
  vector<Dwarf_Off>		alt_types_to_canonicalize_;
  vector<Dwarf_Off>		type_unit_types_to_canonicalize_;
  vector<type_base_sptr>	extra_types_to_canonicalize_;
  string_classes_map		decl_only_classes_map_;
  die_tu_map_type		die_tu_map_;
  corpus_group_sptr		cur_corpus_group_;
  corpus_sptr			cur_corpus_;
  translation_unit_sptr	cur_tu_;
  scope_decl_sptr		nil_scope_;
  scope_stack_type		scope_stack_;
  offset_offset_map_type	primary_die_parent_map_;
  // A map that associates each tu die to a vector of unit import
  // points, in the main debug info
  tu_die_imported_unit_points_map_type tu_die_imported_unit_points_map_;
  // A map that associates each tu die to a vector of unit import
  // points, in the alternate debug info
  tu_die_imported_unit_points_map_type alt_tu_die_imported_unit_points_map_;
  tu_die_imported_unit_points_map_type type_units_tu_die_imported_unit_points_map_;
  // A DIE -> parent map for DIEs coming from the alternate debug info
  // file.
  offset_offset_map_type	alternate_die_parent_map_;
  offset_offset_map_type	type_section_die_parent_map_;
  list<var_decl_sptr>		var_decls_to_add_;
  addr_elf_symbol_sptr_map_sptr fun_addr_sym_map_;
  // On PPC64, the function entry point address is different from the
  // GElf_Sym::st_value value, which is the address of the descriptor
  // of the function.  The map below thus associates the address of
  // the entry point to the function symbol.  If we are not on ppc64,
  // then this map ought to be empty.  Only the fun_addr_sym_map_ is
  // used in that case.  On ppc64, though, both maps are used.
  addr_elf_symbol_sptr_map_sptr fun_entry_addr_sym_map_;
  string_elf_symbols_map_sptr	fun_syms_;
  addr_elf_symbol_sptr_map_sptr var_addr_sym_map_;
  string_elf_symbols_map_sptr	var_syms_;
  string_elf_symbols_map_sptr	undefined_fun_syms_;
  string_elf_symbols_map_sptr	undefined_var_syms_;
  address_set_sptr		linux_exported_fn_syms_;
  address_set_sptr		linux_exported_var_syms_;
  address_set_sptr		linux_exported_gpl_fn_syms_;
  address_set_sptr		linux_exported_gpl_var_syms_;
  vector<string>		dt_needed_;
  string			dt_soname_;
  string			elf_architecture_;
  corpus::exported_decls_builder* exported_decls_builder_;
  options_type			options_;
  read_context();

public:

  /// Constructor of read_context.
  ///
  /// @param elf_path the path to the elf file the context is to be
  /// used for.
  ///
  /// @param debug_info_root_paths a vector of pointers to the path to
  /// the root directory under which the debug info is to be found for
  /// @p elf_path.  Leave this empty if the debug info is not in a
  /// split file.
  ///
  /// @param environment the environment used by the current context.
  /// This environment contains resources needed by the reader and by
  /// the types and declarations that are to be created later.  Note
  /// that ABI artifacts that are to be compared all need to be
  /// created within the same environment.
  ///
  /// Please also note that the life time of this environment object
  /// must be greater than the life time of the resulting @ref
  /// read_context the context uses resources that are allocated in
  /// the environment.
  ///
  /// @param load_all_types if set to false only the types that are
  /// reachable from publicly exported declarations (of functions and
  /// variables) are read.  If set to true then all types found in the
  /// debug information are loaded.
  ///
  /// @param linux_kernel_mode if set to true, then consider the special
  /// linux kernel symbol tables when determining if a symbol is
  /// exported or not.
  read_context(const string&	elf_path,
	       const vector<char**>& debug_info_root_paths,
	       ir::environment* environment,
	       bool		load_all_types,
	       bool		linux_kernel_mode)
  {
    initialize(elf_path, debug_info_root_paths, environment,
	       load_all_types, linux_kernel_mode);
  }

  /// Initializer of read_context.
  ///
  /// @param elf_path the path to the elf file the context is to be
  /// used for.
  ///
  /// @param debug_info_root_paths a vector of pointers to the path to
  /// the root directory under which the debug info is to be found for
  /// @p elf_path.  Leave this empty if the debug info is not in a
  /// split file.
  ///
  /// @param environment the environment used by the current context.
  /// This environment contains resources needed by the reader and by
  /// the types and declarations that are to be created later.  Note
  /// that ABI artifacts that are to be compared all need to be
  /// created within the same environment.
  ///
  /// Please also note that the life time of this environment object
  /// must be greater than the life time of the resulting @ref
  /// read_context the context uses resources that are allocated in
  /// the environment.
  ///
  /// @param load_all_types if set to false only the types that are
  /// reachable from publicly exported declarations (of functions and
  /// variables) are read.  If set to true then all types found in the
  /// debug information are loaded.
  ///
  /// @param linux_kernel_mode if set to true, then consider the
  /// special linux kernel symbol tables when determining if a symbol
  /// is exported or not.
  void
  initialize(const string&	elf_path,
	     const vector<char**>& debug_info_root_paths,
	     ir::environment* environment,
	     bool		load_all_types,
	     bool		linux_kernel_mode)
  {
    dwarf_version_ = 0;
    dwarf_ = 0;
    handle_.reset();
    alt_fd_ = 0;
    alt_dwarf_ = 0;
    elf_module_ = 0;
    elf_handle_ = 0;
    elf_path_ = elf_path;
    bss_section_ = 0;
    text_section_ = 0;
    rodata_section_ = 0;
    data_section_ = 0;
    data1_section_ = 0;
    symtab_section_ = 0;
    opd_section_ = 0;
    ksymtab_format_ = UNDEFINED_KSYMTAB_FORMAT;
    ksymtab_entry_size_ = 0;
    nb_ksymtab_entries_ = 0;
    nb_ksymtab_gpl_entries_ = 0;
    ksymtab_section_ = 0;
    ksymtab_gpl_section_ = 0;
    versym_section_ = 0;
    verdef_section_ = 0;
    verneed_section_ = 0;
    symbol_versionning_sections_loaded_ = 0;
    symbol_versionning_sections_found_ = 0;
    cur_tu_die_ =  0;
    exported_decls_builder_ = 0;

    clear_alt_debug_info_data();

    supprs_.clear();
    decl_die_repr_die_offsets_maps_.clear();
    type_die_repr_die_offsets_maps_.clear();
    die_qualified_name_maps_.clear();
    die_pretty_repr_maps_.clear();
    die_pretty_type_repr_maps_.clear();
    decl_die_artefact_maps_.clear();
    type_die_artefact_maps_.clear();
    canonical_type_die_offsets_.clear();
    canonical_decl_die_offsets_.clear();
    die_wip_classes_map_.clear();
    alternate_die_wip_classes_map_.clear();
    type_unit_die_wip_classes_map_.clear();
    die_wip_function_types_map_.clear();
    alternate_die_wip_function_types_map_.clear();
    type_unit_die_wip_function_types_map_.clear();
    die_function_with_no_symbol_map_.clear();
    types_to_canonicalize_.clear();
    alt_types_to_canonicalize_.clear();
    type_unit_types_to_canonicalize_.clear();
    extra_types_to_canonicalize_.clear();
    decl_only_classes_map_.clear();
    die_tu_map_.clear();
    cur_corpus_group_.reset();
    cur_corpus_.reset();
    cur_tu_.reset();
    primary_die_parent_map_.clear();
    tu_die_imported_unit_points_map_.clear();
    alt_tu_die_imported_unit_points_map_.clear();
    type_units_tu_die_imported_unit_points_map_.clear();
    alternate_die_parent_map_.clear();
    type_section_die_parent_map_.clear();
    var_decls_to_add_.clear();
    fun_addr_sym_map_.reset();
    fun_entry_addr_sym_map_.reset();
    fun_syms_.reset();
    var_addr_sym_map_.reset();
    var_syms_.reset();
    undefined_fun_syms_.reset();
    undefined_var_syms_.reset();
    linux_exported_fn_syms_.reset();
    linux_exported_var_syms_.reset();
    linux_exported_gpl_fn_syms_.reset();
    linux_exported_gpl_var_syms_.reset();
    dt_needed_.clear();
    dt_soname_.clear();
    elf_architecture_.clear();

    clear_per_translation_unit_data();

    memset(&offline_callbacks_, 0, sizeof(offline_callbacks_));
    create_default_dwfl(debug_info_root_paths);
    options_.env = environment;
    options_.load_in_linux_kernel_mode = linux_kernel_mode;
    options_.load_all_types = load_all_types;
    load_in_linux_kernel_mode(linux_kernel_mode);
  }

  /// Clear the resources related to the alternate DWARF data.
  void
  clear_alt_debug_info_data()
  {
    if (alt_fd_)
      {
	close(alt_fd_);
	alt_fd_ = 0;
	if (alt_dwarf_)
	  {
	    dwarf_end(alt_dwarf_);
	    alt_dwarf_ = 0;
	  }
	alt_debug_info_path_.clear();
      }
  }

  /// Detructor of the @ref read_context type.
  ~read_context()
  {
    clear_alt_debug_info_data();
  }

  /// Clear the data that is relevant only for the current translation
  /// unit being read.  The rest of the data is relevant for the
  /// entire ABI corpus.
  void
  clear_per_translation_unit_data()
  {
    while (!scope_stack().empty())
      scope_stack().pop();
    var_decls_to_re_add_to_tree().clear();
  }

  /// Clear the data that is relevant for the current corpus being
  /// read.
  void
  clear_per_corpus_data()
  {
    die_qualified_name_maps_.clear();
    die_pretty_repr_maps_.clear();
    die_pretty_type_repr_maps_.clear();
    clear_types_to_canonicalize();
  }

  /// Getter of the options of the read context.
  ///
  /// @return the options of the read context.
  options_type&
  options()
  {return options_;}

  /// Getter of the options of the read context.
  ///
  /// @return the options of the read context.
  const options_type&
  options() const
  {return options_;}

  /// Getter of the options of the read context.
  ///
  /// @return the options of the read context.
  void
  options(const options_type& o)
  {options_ = o;}

  /// Getter for the current environment.
  ///
  /// @return the current environment.
  const ir::environment*
  env() const
  {return options_.env;}

  /// Getter for the current environment.
  ///
  /// @return the current environment.
  ir::environment*
  env()
  {return options_.env;}

  /// Setter for the current environment.
  ///
  /// @param env the new current environment.
  void
  env(ir::environment* env)
  {options_.env = env;}

  /// Getter of the suppression specifications to be used during
  /// ELF/DWARF parsing.
  ///
  /// @return the suppression specifications.
  const suppr::suppressions_type&
  get_suppressions() const
  {return supprs_;}

  /// Getter of the suppression specifications to be used during
  /// ELF/DWARF parsing.
  ///
  /// @return the suppression specifications.
  suppr::suppressions_type&
  get_suppressions()
  {return supprs_;}

  /// Getter for the callbacks of the Dwarf Front End library of
  /// elfutils that is used by this reader to read dwarf.
  ///
  /// @return the callbacks.
  const Dwfl_Callbacks*
  offline_callbacks() const
  {return &offline_callbacks_;}

  /// Getter for the callbacks of the Dwarf Front End library of
  /// elfutils that is used by this reader to read dwarf.
  /// @returnthe callbacks
  Dwfl_Callbacks*
  offline_callbacks()
  {return &offline_callbacks_;}

  /// Constructor for a default Dwfl handle that knows how to load debug
  /// info from a library or executable elf file.
  ///
  /// @param debug_info_root_paths a vector of pointers to the root
  /// path under which to look for the debug info of the elf files
  /// that are later handled by the Dwfl.  This is for cases where the
  /// debug info is split into a different file from the binary we
  /// want to inspect.  On Red Hat compatible systems, this root path
  /// is usually /usr/lib/debug by default.  If this argument is set
  /// to the empty set, then "./debug" and /usr/lib/debug will be
  /// searched for sub-directories containing the debug info file.
  /// Note that for now, elfutils wants this path to be absolute
  /// otherwise things just don't work and the debug info is not
  /// found.
  ///
  /// @return the constructed Dwfl handle.
  void
  create_default_dwfl(const vector<char**>& debug_info_root_paths)
  {
    offline_callbacks()->find_debuginfo = dwfl_standard_find_debuginfo;
    offline_callbacks()->section_address = dwfl_offline_section_address;
    offline_callbacks()->debuginfo_path =
      debug_info_root_paths.empty() ? 0 : debug_info_root_paths.front();
    handle_.reset(dwfl_begin(offline_callbacks()),
		  dwfl_deleter());
    debug_info_root_paths_ = debug_info_root_paths;
  }

  unsigned short
  dwarf_version() const
  {return dwarf_version_;}

  void
  dwarf_version(unsigned short v)
  {dwarf_version_ = v;}

  /// Getter for a smart pointer to a handle on the dwarf front end
  /// library that we use to read dwarf.
  ///
  /// @return the dwfl handle.
  dwfl_sptr
  dwfl_handle() const
  {return handle_;}

  /// Setter for a smart pointer to a handle on the dwarf front end
  /// library that we use to read dwarf.
  ///
  /// @param h the new dwfl handle.
  void
  dwfl_handle(dwfl_sptr& h)
  {handle_ = h;}

  Dwfl_Module*
  elf_module() const
  {return elf_module_;}

  /// Return the ELF descriptor for the binary we are analizing.
  ///
  /// @return a pointer to the Elf descriptor representing the binary
  /// we are analizing.
  Elf*
  elf_handle() const
  {
    if (elf_handle_ == 0)
      {
	if (elf_module())
	  {
	    GElf_Addr bias = 0;
	    elf_handle_ = dwfl_module_getelf(elf_module(), &bias);
	  }
      }
    return elf_handle_;
  }

  /// Return the ELF descriptor used for DWARF access.
  ///
  /// This can be the same as read_context::elf_handle() above, if the
  /// DWARF info is in the same ELF file as the one of the binary we
  /// are analizing.  It is different if e.g, the debug info is split
  /// from the ELF file we are analizing.
  ///
  /// @return a pointer to the ELF descriptor used to access debug
  /// info.
  Elf*
  dwarf_elf_handle() const
  {return dwarf_getelf(dwarf());}

  /// Test if the debug information is in a separate ELF file wrt the
  /// main ELF file of the program (application or shared library) we
  /// are analizing.
  ///
  /// @return true if the debug information is in a separate ELF file
  /// compared to the main ELF file of the program (application or
  /// shared library) that we are looking at.
  bool
  dwarf_is_splitted() const
  {return dwarf_elf_handle() != elf_handle();}

  /// Add paths to the set of paths under which to look for split
  /// debuginfo files.
  ///
  /// @param debug_info_root_paths the paths to add.
  void
  add_debug_info_root_paths(const vector<char **>& debug_info_root_paths)
  {
    debug_info_root_paths_.insert(debug_info_root_paths_.end(),
				  debug_info_root_paths.begin(),
				  debug_info_root_paths.end());
  }

  /// Add a path to the set of paths under which to look for split
  /// debuginfo files.
  ///
  /// @param debug_info_root_path the path to add.
  void
  add_debug_info_root_path(char** debug_info_root_path)
  {debug_info_root_paths_.push_back(debug_info_root_path);}

  /// Find the alternate debuginfo file associated to a given elf file.
  ///
  /// @param elf_module represents the elf file to consider.
  ///
  /// @param alt_file_name the resulting path to the alternate
  /// debuginfo file found.  This is set iff the function returns a
  /// non-nil value.
  Dwarf*
  find_alt_debug_info(Dwfl_Module *elf_module,
		      string& alt_file_name,
		      int& alt_fd)
  {
    Dwarf *result = 0;
    result = dwarf_reader::find_alt_debug_info(elf_module,
					       debug_info_root_paths_,
					       alt_file_name, alt_fd);
    return result;
  }

  /// Load the debug info associated with an elf file that is at a
  /// given path.
  ///
  /// @return a pointer to the DWARF debug info pointer upon
  /// successful debug info loading, NULL otherwise.
  Dwarf*
  load_debug_info()
  {
    if (!dwfl_handle())
      return 0;

    if (dwarf_)
      return dwarf_;

    elf_module_ =
      dwfl_report_offline(dwfl_handle().get(),
			  basename(const_cast<char*>(elf_path().c_str())),
			  elf_path().c_str(),
			  -1);
    dwfl_report_end(dwfl_handle().get(), 0, 0);

    Dwarf_Addr bias = 0;
    dwarf_ = dwfl_module_getdwarf(elf_module_, &bias);
    // Look for split debuginfo files under multiple possible
    // debuginfo roots.
    for (vector<char**>::const_iterator i = debug_info_root_paths_.begin();
	 dwarf_ == 0 && i != debug_info_root_paths_.end();
	 ++i)
      {
	offline_callbacks()->debuginfo_path = *i;
	dwarf_ = dwfl_module_getdwarf(elf_module_, &bias);
      }

    if (!alt_dwarf_)
      alt_dwarf_ = find_alt_debug_info(elf_module_,
				       alt_debug_info_path_,
				       alt_fd_);

    return dwarf_;
  }

  /// Return the main debug info we are looking at.
  ///
  /// @return the main debug info.
  Dwarf*
  dwarf() const
  {return dwarf_;}

  /// Return the alternate debug info we are looking at.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the alternate debug info.
  Dwarf*
  alt_dwarf() const
  {return alt_dwarf_;}

  /// Return the correct debug info, depending on the DIE source we
  /// are looking at.
  ///
  /// @param source the DIE source to consider.
  ///
  /// @return the right debug info, depending on @p source.
  Dwarf*
  dwarf_per_die_source(die_source source) const
  {
    Dwarf *result = 0;
    switch(source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
      case TYPE_UNIT_DIE_SOURCE:
	result = dwarf();
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	result = alt_dwarf();
	break;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return result;
  }

  /// Return the path to the alternate debug info as contained in the
  /// .gnu_debugaltlink section of the main elf file.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the path to the alternate debug info file, or an empty
  /// path if no alternate debug info file is associated.
  const string&
  alt_debug_info_path() const
  {return alt_debug_info_path_;}

  /// Return the path to the ELF path we are reading.
  ///
  /// @return the elf path.
  const string&
  elf_path() const
  {return elf_path_;}

  /// Return the bss section of the ELF file we are reading.
  ///
  /// The first time this function is called, the ELF file is scanned
  /// to look for the section we are looking for.  Once the section is
  /// found, it's cached.
  ///
  /// Subsequent calls to this function just return the cached
  /// version.
  ///
  /// @return the bss section.
  Elf_Scn*
  bss_section() const
  {
    if (!bss_section_)
      bss_section_ = find_bss_section(elf_handle());
    return bss_section_;
  }

  /// Return the text section of the ELF file we are reading.
  ///
  /// The first time this function is called, the ELF file is scanned
  /// to look for the section we are looking for.  Once the section is
  /// found, it's cached.
  ///
  /// Subsequent calls to this function just return the cached
  /// version.
  ///
  /// return the text section.
  Elf_Scn*
  text_section() const
  {
    if (!text_section_)
      text_section_ = find_text_section(elf_handle());
    return text_section_;
  }

  /// Return the rodata section of the ELF file we are reading.
  ///
  /// The first time this function is called, the ELF file is scanned
  /// to look for the section we are looking for.  Once the section is
  /// found, it's cached.
  ///
  /// Subsequent calls to this function just return the cached
  /// version.
  ///
  /// return the rodata section.
  Elf_Scn*
  rodata_section() const
  {
    if (!rodata_section_)
      rodata_section_ =find_rodata_section(elf_handle());
    return rodata_section_;
  }

  /// Return the data section of the ELF file we are reading.
  ///
  /// The first time this function is called, the ELF file is scanned
  /// to look for the section we are looking for.  Once the section is
  /// found, it's cached.
  ///
  /// Subsequent calls to this function just return the cached
  /// version.
  ///
  /// return the data section.
  Elf_Scn*
  data_section() const
  {
    if (!data_section_)
      data_section_ = find_data_section(elf_handle());
    return data_section_;
  }

  /// Return the data1 section of the ELF file we are reading.
  ///
  /// The first time this function is called, the ELF file is scanned
  /// to look for the section we are looking for.  Once the section is
  /// found, it's cached.
  ///
  /// Subsequent calls to this function just return the cached
  /// version.
  ///
  /// return the data1 section.
  Elf_Scn*
  data1_section() const
  {
    if (!data1_section_)
      data1_section_ = find_data1_section(elf_handle());
    return data1_section_;
  }

  const Dwarf_Die*
  cur_tu_die() const
  {return cur_tu_die_;}

  void
  cur_tu_die(Dwarf_Die* cur_tu_die)
  {cur_tu_die_ = cur_tu_die;}

  dwarf_expr_eval_context&
  dwarf_expr_eval_ctxt() const
  {return dwarf_expr_eval_context_;}

  /// Getter of the maps set that associates a representation of a
  /// decl DIE to a vector of offsets of DIEs having that representation.
  ///
  /// @return the maps set that associates a representation of a decl
  /// DIE to a vector of offsets of DIEs having that representation.
  const die_source_dependant_container_set<istring_dwarf_offsets_map_type>&
  decl_die_repr_die_offsets_maps() const
  {return decl_die_repr_die_offsets_maps_;}

  /// Getter of the maps set that associates a representation of a
  /// decl DIE to a vector of offsets of DIEs having that representation.
  ///
  /// @return the maps set that associates a representation of a decl
  /// DIE to a vector of offsets of DIEs having that representation.
  die_source_dependant_container_set<istring_dwarf_offsets_map_type>&
  decl_die_repr_die_offsets_maps()
  {return decl_die_repr_die_offsets_maps_;}

  /// Getter of the maps set that associate a representation of a type
  /// DIE to a vector of offsets of DIEs having that representation.
  ///
  /// @return the maps set that associate a representation of a type
  /// DIE to a vector of offsets of DIEs having that representation.
  const die_source_dependant_container_set<istring_dwarf_offsets_map_type>&
  type_die_repr_die_offsets_maps() const
  {return type_die_repr_die_offsets_maps_;}

  /// Getter of the maps set that associate a representation of a type
  /// DIE to a vector of offsets of DIEs having that representation.
  ///
  /// @return the maps set that associate a representation of a type
  /// DIE to a vector of offsets of DIEs having that representation.
  die_source_dependant_container_set<istring_dwarf_offsets_map_type>&
  type_die_repr_die_offsets_maps()
  {return type_die_repr_die_offsets_maps_;}


  /// Compute the offset of the canonical DIE of a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param canonical_die_offset out parameter.  This is set to the
  /// resulting canonical DIE that was computed.
  ///
  /// @param die_as_type if yes, it means @p die has to be considered
  /// as a type.
  void
  compute_canonical_die_offset(Dwarf_Die *die,
			       Dwarf_Off &canonical_die_offset,
			       bool die_as_type) const
  {
    offset_offset_map_type &canonical_dies =
      die_as_type
      ? const_cast<read_context*>(this)->canonical_type_die_offsets_.
      get_container(*this, die)
      : const_cast<read_context*>(this)->canonical_decl_die_offsets_.
      get_container(*this, die);

    Dwarf_Die canonical_die;
    compute_canonical_die(die, canonical_dies, canonical_die, die_as_type);

    canonical_die_offset = dwarf_dieoffset(&canonical_die);
  }

  /// Compute (find) the canonical DIE of a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param canonical_dies the vector in which the canonical dies ar
  /// stored.  The index of each element is the offset of the DIE we
  /// want the canonical DIE for.  And the value of the element at
  /// that index is the canonical DIE offset we are looking for.
  ///
  /// @param canonical_die_offset out parameter.  This is set to the
  /// resulting canonical DIE that was computed.
  ///
  /// @param die_as_type if yes, it means @p die has to be considered
  /// as a type.
  void
  compute_canonical_die(Dwarf_Die *die,
			offset_offset_map_type& canonical_dies,
			Dwarf_Die &canonical_die,
			bool die_as_type) const
  {
    die_source source;
    ABG_ASSERT(get_die_source(die, source));

    Dwarf_Off die_offset = dwarf_dieoffset(die);

    compute_canonical_die(die_offset, source,
			  canonical_dies,
			  canonical_die, die_as_type);
  }

  /// Compute (find) the canonical DIE of a given DIE.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source the source of the DIE to consider.
  ///
  /// @param canonical_dies the vector in which the canonical dies ar
  /// stored.  The index of each element is the offset of the DIE we
  /// want the canonical DIE for.  And the value of the element at
  /// that index is the canonical DIE offset we are looking for.
  ///
  /// @param canonical_die_offset out parameter.  This is set to the
  /// resulting canonical DIE that was computed.
  ///
  /// @param die_as_type if yes, it means @p die has to be considered
  /// as a type.
  void
  compute_canonical_die(Dwarf_Off die_offset,
			die_source source,
			offset_offset_map_type& canonical_dies,
			Dwarf_Die &canonical_die,
			bool die_as_type) const
  {
    // The map that associates the string representation of 'die'
    // with a vector of offsets of potentially equivalent DIEs.
    istring_dwarf_offsets_map_type& map =
      die_as_type
      ? (const_cast<read_context*>(this)->
	 type_die_repr_die_offsets_maps().get_container(source))
      : (const_cast<read_context*>(this)->
	 decl_die_repr_die_offsets_maps().get_container(source));

    Dwarf_Die die;
    ABG_ASSERT(dwarf_offdie(dwarf_per_die_source(source), die_offset, &die));

    // The variable repr is the the string representation of 'die'.
    //
    // Even if die_as_type is true -- which means that 'die' is said
    // to be considered as a type -- we always consider a
    // DW_TAG_subprogram DIE as a decl here, as far as its string
    // representation is concerned.
    interned_string name =
      (die_as_type)
      ? get_die_pretty_type_representation(&die, /*where=*/0)
      : get_die_pretty_representation(&die, /*where=*/0);

    Dwarf_Off canonical_die_offset = 0;
    istring_dwarf_offsets_map_type::iterator i = map.find(name);
    if (i == map.end())
      {
	dwarf_offsets_type offsets;
	offsets.push_back(die_offset);
	map[name] = offsets;
	set_canonical_die_offset(canonical_dies, die_offset, die_offset);
	get_die_from_offset(source, die_offset, &canonical_die);
	return;
      }

    if (odr_is_relevant(&die))
      {
	// ODR is relevant for this DIE.  In this case, all types with
	// the same name are considered equivalent.  So the array
	// i->second shoud only have on element.  If not, then
	// the DIEs referenced in the array should all compare equal.
	// Otherwise, this is an ODR violation.  In any case, return
	// the first element of the array.
	// ABG_ASSERT(i->second.size() == 1);
	canonical_die_offset = i->second.front();
	get_die_from_offset(source, canonical_die_offset, &canonical_die);
	set_canonical_die_offset(canonical_dies, die_offset, die_offset);
	return;
      }

    Dwarf_Off cur_die_offset;
    Dwarf_Die potential_canonical_die;
    for (dwarf_offsets_type::const_iterator o = i->second.begin();
	 o != i->second.end();
	 ++o)
      {
	cur_die_offset = *o;
	get_die_from_offset(source, cur_die_offset, &potential_canonical_die);
	if (compare_dies(*this, &die, &potential_canonical_die,
			 /*update_canonical_dies_on_the_fly=*/false))
	  {
	    canonical_die_offset = cur_die_offset;
	    set_canonical_die_offset(canonical_dies, die_offset,
				     canonical_die_offset);
	    get_die_from_offset(source, canonical_die_offset, &canonical_die);
	    return;
	  }
      }

    canonical_die_offset = die_offset;
    i->second.push_back(die_offset);
    set_canonical_die_offset(canonical_dies, die_offset, die_offset);
    get_die_from_offset(source, canonical_die_offset, &canonical_die);
  }

  /// Getter of the canonical DIE of a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param canonical_die output parameter.  Is set to the resuling
  /// canonical die, if this function returns true.
  ///
  /// @param where the offset of the logical DIE we are supposed to be
  /// calling this function from.  If set to zero this means this is
  /// to be ignored.
  ///
  /// @param die_as_type if set to yes, it means @p die is to be
  /// considered as a type DIE.
  ///
  /// @return true iff a canonical DIE was found for @p die.
  bool
  get_canonical_die(Dwarf_Die *die,
		    Dwarf_Die &canonical_die,
		    size_t where,
		    bool die_as_type) const
  {
    die_source source;
    ABG_ASSERT(get_die_source(die, source));

    offset_offset_map_type &canonical_dies =
      die_as_type
      ? const_cast<read_context*>(this)->canonical_type_die_offsets_.
      get_container(source)
      : const_cast<read_context*>(this)->canonical_decl_die_offsets_.
      get_container(source);

    Dwarf_Off die_offset = dwarf_dieoffset(die);
    if (Dwarf_Off canonical_die_offset =
	get_canonical_die_offset(canonical_dies, die_offset))
      {
	get_die_from_offset(source, canonical_die_offset, &canonical_die);
	return true;
      }

    // The map that associates the string representation of 'die'
    // with a vector of offsets of potentially equivalent DIEs.
    istring_dwarf_offsets_map_type& map =
      die_as_type
      ? (const_cast<read_context*>(this)->
	 type_die_repr_die_offsets_maps().get_container(*this, die))
      : (const_cast<read_context*>(this)->
	 decl_die_repr_die_offsets_maps().get_container(*this, die));

    // The variable repr is the the string representation of 'die'.
    //
    // Even if die_as_type is true -- which means that 'die' is said
    // to be considered as a type -- we always consider a
    // DW_TAG_subprogram DIE as a decl here, as far as its string
    // representation is concerned.
    interned_string name =
      (die_as_type /*&& dwarf_tag(die) != DW_TAG_subprogram*/)
      ? get_die_pretty_type_representation(die, where)
      : get_die_pretty_representation(die, where);

    istring_dwarf_offsets_map_type::iterator i = map.find(name);
    if (i == map.end())
      return false;

    if (odr_is_relevant(die))
      {
	// ODR is relevant for this DIE.  In this case, all types with
	// the same name are considered equivalent.  So the array
	// i->second shoud only have on element.  If not, then
	// the DIEs referenced in the array should all compare equal.
	// Otherwise, this is an ODR violation.  In any case, return
	// the first element of the array.
	// ABG_ASSERT(i->second.size() == 1);
	Dwarf_Off canonical_die_offset = i->second.front();
	get_die_from_offset(source, canonical_die_offset, &canonical_die);
	set_canonical_die_offset(canonical_dies,
				 die_offset,
				 canonical_die_offset);
	return true;
      }

    Dwarf_Off cur_die_offset;
    for (dwarf_offsets_type::const_iterator o = i->second.begin();
	 o != i->second.end();
	 ++o)
      {
	cur_die_offset = *o;
	get_die_from_offset(source, cur_die_offset, &canonical_die);
	// compare die and canonical_die.
	if (compare_dies(*this, die, &canonical_die,
			 /*update_canonical_dies_on_the_fly=*/true))
	  {
	    set_canonical_die_offset(canonical_dies,
				     die_offset,
				     cur_die_offset);
	    return true;
	  }
      }

    return false;
  }

  /// Retrieve the canonical DIE of a given DIE.
  ///
  /// The canonical DIE is a DIE that is structurally equivalent to
  /// this one.
  ///
  /// Note that this function caches the canonical DIE that was
  /// computed.  Subsequent invocations of this function on the same
  /// DIE return the same cached DIE.
  ///
  /// @param die the DIE to get a canonical type for.
  ///
  /// @param canonical_die the resulting canonical DIE.
  ///
  /// @param where the offset of the logical DIE we are supposed to be
  /// calling this function from.  If set to zero this means this is
  /// to be ignored.
  ///
  /// @param die_as_type if true, consider DIE is a type.
  ///
  /// @return true if an *existing* canonical DIE was found.
  /// Otherwise, @p die is considered as being a canonical DIE for
  /// itself. @p canonical_die is thus set to the canonical die in
  /// either cases.
  bool
  get_or_compute_canonical_die(Dwarf_Die* die,
			       Dwarf_Die& canonical_die,
			       size_t where,
			       bool die_as_type) const
  {
    die_source source;
    ABG_ASSERT(get_die_source(die, source));

    offset_offset_map_type &canonical_dies =
      die_as_type
      ? const_cast<read_context*>(this)->canonical_type_die_offsets_.
      get_container(source)
      : const_cast<read_context*>(this)->canonical_decl_die_offsets_.
      get_container(source);

    Dwarf_Off initial_die_offset = dwarf_dieoffset(die);

    if (Dwarf_Off canonical_die_offset =
	get_canonical_die_offset(canonical_dies,
				 initial_die_offset))
      {
	get_die_from_offset(source, canonical_die_offset, &canonical_die);
	return true;
      }

    // The map that associates the string representation of 'die'
    // with a vector of offsets of potentially equivalent DIEs.
    istring_dwarf_offsets_map_type& map =
      die_as_type
      ? (const_cast<read_context*>(this)->
	 type_die_repr_die_offsets_maps().get_container(*this, die))
      : (const_cast<read_context*>(this)->
	 decl_die_repr_die_offsets_maps().get_container(*this, die));

    // The variable repr is the the string representation of 'die'.
    //
    // Even if die_as_type is true -- which means that 'die' is said
    // to be considered as a type -- we always consider a
    // DW_TAG_subprogram DIE as a decl here, as far as its string
    // representation is concerned.
    interned_string name =
      (die_as_type)
      ? get_die_pretty_type_representation(die, where)
      : get_die_pretty_representation(die, where);

    istring_dwarf_offsets_map_type::iterator i = map.find(name);
    if (i == map.end())
      {
	dwarf_offsets_type offsets;
	offsets.push_back(initial_die_offset);
	map[name] = offsets;
	get_die_from_offset(source, initial_die_offset, &canonical_die);
	set_canonical_die_offset(canonical_dies,
				 initial_die_offset,
				 initial_die_offset);
	return false;
      }

    if (odr_is_relevant(die))
      {
	// ODR is relevant for this DIE.  In this case, all types with
	// the same name are considered equivalent.  So the array
	// i->second shoud only have on element.  If not, then
	// the DIEs referenced in the array should all compare equal.
	// Otherwise, this is an ODR violation.  In any case, return
	// the first element of the array.
	// ABG_ASSERT(i->second.size() == 1);
	Dwarf_Off die_offset = i->second.front();
	get_die_from_offset(source, die_offset, &canonical_die);
	set_canonical_die_offset(canonical_dies,
				 initial_die_offset,
				 die_offset);
	return true;
      }

    // walk i->second without any iterator (using a while loop rather
    // than a for loop) because compare_dies might add new content to
    // the end of the i->second vector during the walking.
    dwarf_offsets_type::size_type n = 0, s = i->second.size();
    while (n < s)
      {
	Dwarf_Off die_offset = i->second[n];
	get_die_from_offset(source, die_offset, &canonical_die);
	// compare die and canonical_die.
	if (compare_dies(*this, die, &canonical_die,
			 /*update_canonical_dies_on_the_fly=*/true))
	  {
	    set_canonical_die_offset(canonical_dies,
				     initial_die_offset,
				     die_offset);
	    return true;
	  }
	++n;
      }

    // We didn't find a canonical DIE for 'die'.  So let's consider
    // that it is its own canonical DIE.
    get_die_from_offset(source, initial_die_offset, &canonical_die);
    i->second.push_back(initial_die_offset);
    set_canonical_die_offset(canonical_dies,
			     initial_die_offset,
			     initial_die_offset);

    return false;
  }

  /// Get the source of the DIE.
  ///
  /// The function returns an enumerator value saying if the DIE comes
  /// from the .debug_info section of the primary debug info file, the
  /// .debug_info section of the alternate debug info file, or the
  /// .debug_types section.
  ///
  /// @param die the DIE to get the source of.
  ///
  /// @param source out parameter.  The function sets this parameter
  /// to the source of the DIE @p iff it returns true.
  ///
  /// @return true iff the source of the DIE could be determined and
  /// returned.
  bool
  get_die_source(Dwarf_Die *die, die_source &source) const
  {
    ABG_ASSERT(die);
    return get_die_source(*die, source);
  }

  /// Get the source of the DIE.
  ///
  /// The function returns an enumerator value saying if the DIE comes
  /// from the .debug_info section of the primary debug info file, the
  /// .debug_info section of the alternate debug info file, or the
  /// .debug_types section.
  ///
  /// @param die the DIE to get the source of.
  ///
  /// @param source out parameter.  The function sets this parameter
  /// to the source of the DIE @p iff it returns true.
  ///
  /// @return true iff the source of the DIE could be determined and
  /// returned.
  bool
  get_die_source(Dwarf_Die &die, die_source &source) const
  {
    Dwarf_Die cu_die;
    Dwarf_Die cu_kind;
    uint8_t address_size = 0, offset_size = 0;
    if (!dwarf_diecu(&die, &cu_die, &address_size, &offset_size))
      return false;

    Dwarf_Half version = 0;
    Dwarf_Off abbrev_offset = 0;
    uint64_t type_signature = 0;
    Dwarf_Off type_offset = 0;
    if (!dwarf_cu_die(cu_die.cu, &cu_kind,
		      &version, &abbrev_offset,
		      &address_size, &offset_size,
		      &type_signature, &type_offset))
      return false;

    int tag = dwarf_tag(&cu_kind);

    if (tag == DW_TAG_compile_unit
	|| tag == DW_TAG_partial_unit)
      {
	Dwarf *die_dwarf = dwarf_cu_getdwarf(cu_die.cu);
	if (dwarf() == die_dwarf)
	  source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	else if (alt_dwarf() == die_dwarf)
	  source = ALT_DEBUG_INFO_DIE_SOURCE;
	else
	  ABG_ASSERT_NOT_REACHED;
      }
    else if (tag == DW_TAG_type_unit)
      source = TYPE_UNIT_DIE_SOURCE;
    else
      return false;

    return true;
  }

  /// Getter for the DIE designated by an offset.
  ///
  /// @param source the source of the DIE to get.
  ///
  /// @param offset the offset of the DIE to get.
  ///
  /// @param die the resulting DIE.  The pointer has to point to an
  /// allocated memory region.
  void
  get_die_from_offset(die_source source, Dwarf_Off offset, Dwarf_Die *die) const
  {
    if (source == TYPE_UNIT_DIE_SOURCE)
      ABG_ASSERT(dwarf_offdie_types(dwarf_per_die_source(source), offset, die));
    else
      ABG_ASSERT(dwarf_offdie(dwarf_per_die_source(source), offset, die));
  }

public:

  /// Add an entry to the relevant die->decl map.
  ///
  /// @param die the DIE to add the the map.
  ///
  /// @param decl the decl to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @param do_associate_by_repr if true then this function
  /// associates the representation string of @p die with the
  /// declaration @p decl, in a corpus-wide manner.  That is, in the
  /// entire current corpus, there is going to be just one declaration
  /// associated with a DIE of the string representation of @p die.
  ///
  /// @param do_associate_by_repr_per_tu if true, then this function
  /// associates the representation string of @p die with the
  /// declaration @p decl in a translation unit wide manner.  That is,
  /// in the entire current translation unit, there is going to be
  /// just one declaration associated with a DIE of the string
  /// representation of @p die.
  void
  associate_die_to_decl(Dwarf_Die* die,
			decl_base_sptr decl,
			size_t where_offset,
			bool do_associate_by_repr = false)
  {
    die_source source;
    ABG_ASSERT(get_die_source(die, source));

    die_artefact_map_type& m =
      decl_die_artefact_maps().get_container(source);

    size_t die_offset;
    if (do_associate_by_repr)
      {
	Dwarf_Die equiv_die;
	get_or_compute_canonical_die(die, equiv_die, where_offset,
				     /*die_as_type=*/false);
	die_offset = dwarf_dieoffset(&equiv_die);
      }
    else
      die_offset = dwarf_dieoffset(die);

    m[die_offset] = decl;
  }

public:

  /// Lookup the decl for a given DIE.
  ///
  /// The returned decl is either the decl of the DIE that as the
  /// exact offset @p die_offset
  /// die_offset, or
  /// give
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source where the DIE represented by @p die_offset comes
  /// from.
  ///
  /// Note that "alternate debug info sections" is a GNU extension as
  /// of DWARF4 and is described at
  /// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1
  ///
  /// @return the resulting decl, or null if no decl is associated to
  /// the DIE represented by @p die_offset.
  decl_base_sptr
  lookup_decl_from_die_offset(Dwarf_Off die_offset, die_source source)
  {
    decl_base_sptr result =
      is_decl(lookup_artifact_from_die_offset(die_offset, source,
					      /*die_as_type=*/false));

    return result;
  }

  /// Get the qualified name of a given DIE.
  ///
  /// If the name of the DIE was already computed before just return
  /// that name from a cache.  Otherwise, build the name, cache it and
  /// return it.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the interned string representing the qualified name of
  /// @p die.
  interned_string
  get_die_qualified_name(Dwarf_Die *die, size_t where_offset)
  {
    ABG_ASSERT(die);
    die_istring_map_type& map =
      die_qualified_name_maps_.get_container(*this, die);

    size_t die_offset = dwarf_dieoffset(die);
    die_istring_map_type::const_iterator i = map.find(die_offset);

    if (i == map.end())
      {
	read_context& ctxt  = *const_cast<read_context*>(this);
	string qualified_name = die_qualified_name(ctxt, die, where_offset);
	interned_string istr = env()->intern(qualified_name);
	map[die_offset] = istr;
	return istr;
      }

    return i->second;
  }

  /// Get the qualified name of a given DIE.
  ///
  /// If the name of the DIE was already computed before just return
  /// that name from a cache.  Otherwise, build the name, cache it and
  /// return it.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the interned string representing the qualified name of
  /// @p die.
  interned_string
  get_die_qualified_name(Dwarf_Die *die, size_t where_offset) const
  {
    return const_cast<read_context*>(this)->
      get_die_qualified_name(die, where_offset);
  }

  /// Get the qualified name of a given DIE which is considered to be
  /// the DIE for a type.
  ///
  /// For instance, for a DW_TAG_subprogram DIE, this function
  /// computes the name of the function *type* that corresponds to the
  /// function.
  ///
  /// If the name of the DIE was already computed before just return
  /// that name from a cache.  Otherwise, build the name, cache it and
  /// return it.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the interned string representing the qualified name of
  /// @p die.
  interned_string
  get_die_qualified_type_name(Dwarf_Die *die, size_t where_offset) const
  {
    ABG_ASSERT(die);

    // The name of the translation unit die is "".
    if (die == cur_tu_die())
      return env()->intern("");

    die_istring_map_type& map =
      die_qualified_name_maps_.get_container(*const_cast<read_context*>(this),
					     die);

    size_t die_offset = dwarf_dieoffset(die);
    die_istring_map_type::const_iterator i =
      map.find(die_offset);

    if (i == map.end())
      {
	read_context& ctxt  = *const_cast<read_context*>(this);
	string qualified_name;
	int tag = dwarf_tag(die);
	if ((tag == DW_TAG_structure_type
	     || tag == DW_TAG_class_type
	     || tag == DW_TAG_union_type)
	    && die_is_anonymous(die))
	  {
	    location l = die_location(*this, die);
	    qualified_name = l ? l.expand() : "noloc";
	    qualified_name = "unnamed-at-" + qualified_name;
	  }
	else
	  qualified_name =
	    die_qualified_type_name(ctxt, die, where_offset);

	interned_string istr = env()->intern(qualified_name);
	map[die_offset] = istr;
	return istr;
      }

    return i->second;
  }

  /// Get the pretty representation of a DIE that represents a type.
  ///
  /// For instance, for the DW_TAG_subprogram, this function computes
  /// the pretty representation of the type of the function, not the
  /// pretty representation of the function declaration.
  ///
  /// Once the pretty representation is computed, it's stored in a
  /// cache.  Subsequent invocations of this function on the same DIE
  /// will yield the cached name.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the interned_string that represents the pretty
  /// representation.
  interned_string
  get_die_pretty_type_representation(Dwarf_Die *die, size_t where_offset) const
  {
    ABG_ASSERT(die);
    die_istring_map_type& map =
      die_pretty_type_repr_maps_.get_container(*const_cast<read_context*>(this),
					       die);

    size_t die_offset = dwarf_dieoffset(die);
    die_istring_map_type::const_iterator i = map.find(die_offset);

    if (i == map.end())
      {
	read_context& ctxt = *const_cast<read_context*>(this);
	string pretty_representation =
	  die_pretty_print_type(ctxt, die, where_offset);
	interned_string istr = env()->intern(pretty_representation);
	map[die_offset] = istr;
	return istr;
      }

    return i->second;
  }

  /// Get the pretty representation of a DIE.
  ///
  /// Once the pretty representation is computed, it's stored in a
  /// cache.  Subsequent invocations of this function on the same DIE
  /// will yield the cached name.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the interned_string that represents the pretty
  /// representation.
  interned_string
  get_die_pretty_representation(Dwarf_Die *die, size_t where_offset) const
  {
    ABG_ASSERT(die);

    die_istring_map_type& map =
      die_pretty_repr_maps_.get_container(*const_cast<read_context*>(this),
					  die);

    size_t die_offset = dwarf_dieoffset(die);
    die_istring_map_type::const_iterator i = map.find(die_offset);

    if (i == map.end())
      {
	read_context& ctxt = *const_cast<read_context*>(this);
	string pretty_representation =
	  die_pretty_print(ctxt, die, where_offset);
	interned_string istr = env()->intern(pretty_representation);
	map[die_offset] = istr;
	return istr;
      }

    return i->second;
  }

  /// Lookup the artifact that was built to represent a type that has
  /// the same pretty representation as the type denoted by a given
  /// DIE.
  ///
  /// Note that the DIE must have previously been associated with the
  /// artifact using the functions associate_die_to_decl or
  /// associate_die_to_type.
  ///
  /// Also, note that the scope of the lookup is the current ABI
  /// corpus.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @return the type artifact found.
  type_or_decl_base_sptr
  lookup_type_artifact_from_die(Dwarf_Die *die) const
  {
    type_or_decl_base_sptr artifact =
      lookup_artifact_from_die(die, /*type_as_die=*/true);
    if (function_decl_sptr fn = is_function_decl(artifact))
      return fn->get_type();
    return artifact;
  }

  /// Lookup the artifact that was built to represent a type or a
  /// declaration that has the same pretty representation as the type
  /// denoted by a given DIE.
  ///
  /// Note that the DIE must have previously been associated with the
  /// artifact using the functions associate_die_to_decl or
  /// associate_die_to_type.
  ///
  /// Also, note that the scope of the lookup is the current ABI
  /// corpus.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @param die_as_type if true, it means the DIE is to be considered
  /// as a type.
  ///
  /// @return the artifact found.
  type_or_decl_base_sptr
  lookup_artifact_from_die(Dwarf_Die *die, bool die_as_type = false) const
  {
    Dwarf_Die equiv_die;
    if (!get_or_compute_canonical_die(die, equiv_die, /*where=*/0, die_as_type))
      return type_or_decl_base_sptr();

    const die_artefact_map_type& m =
      die_as_type
      ? type_die_artefact_maps().get_container(*this, &equiv_die)
      : decl_die_artefact_maps().get_container(*this, &equiv_die);

    size_t die_offset = dwarf_dieoffset(&equiv_die);
    die_artefact_map_type::const_iterator i = m.find(die_offset);

    if (i == m.end())
      return type_or_decl_base_sptr();
    return i->second;
  }

  /// Lookup the artifact that was built to represent a type or a
  /// declaration that has the same pretty representation as the type
  /// denoted by the offset of a given DIE.
  ///
  /// Note that the DIE must have previously been associated with the
  /// artifact using either associate_die_to_decl or
  /// associate_die_to_type.
  ///
  /// Also, note that the scope of the lookup is the current ABI
  /// corpus.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  ///
  /// @param die_as_type if true, it means the DIE is to be considered
  /// as a type.
  ///
  /// @return the artifact found.
  type_or_decl_base_sptr
  lookup_artifact_from_die_offset(Dwarf_Off die_offset,
				  die_source source,
				  bool die_as_type = false) const
  {
    const die_artefact_map_type& m =
      die_as_type
      ? type_die_artefact_maps().get_container(source)
      : decl_die_artefact_maps().get_container(source);

    die_artefact_map_type::const_iterator i = m.find(die_offset);
    if (i == m.end())
      return type_or_decl_base_sptr();
    return i->second;
  }

  /// Get the language used to generate a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param lang the resulting language.
  ///
  /// @return true iff the language of the DIE was found.
  bool
  get_die_language(Dwarf_Die *die, translation_unit::language &lang) const
  {
    Dwarf_Die cu_die;
    ABG_ASSERT(dwarf_diecu(die, &cu_die, 0, 0));

    uint64_t l = 0;
    if (!die_unsigned_constant_attribute(&cu_die, DW_AT_language, l))
      return false;

    lang = dwarf_language_to_tu_language(l);
    return true;
  }

  /// Check if we can assume the One Definition Rule[1] to be relevant
  /// for the current translation unit.
  ///
  /// [1]: https://en.wikipedia.org/wiki/One_Definition_Rule
  ///
  /// At the moment this returns true if the current translation unit
  /// is in C++ language.  In that case, it's relevant to assume that
  /// we use optimizations based on the ODR.
  bool
  odr_is_relevant() const
  {return odr_is_relevant(cur_transl_unit()->get_language());}

  /// Check if we can assume the One Definition Rule[1] to be relevant
  /// for a given language.
  ///
  /// [1]: https://en.wikipedia.org/wiki/One_Definition_Rule
  ///
  /// At the moment this returns true if the language considered
  /// is C++, Java or Ada.
  bool
  odr_is_relevant(translation_unit::language l) const
  {
    return (is_cplus_plus_language(l)
	    || is_java_language(l)
	    || is_ada_language(l));
  }

  /// Check if we can assume the One Definition Rule to be relevant
  /// for a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @return true if the ODR is relevant for @p die.
  bool
  odr_is_relevant(Dwarf_Off die_offset, die_source source) const
  {
    Dwarf_Die die;
    ABG_ASSERT(dwarf_offdie(dwarf_per_die_source(source), die_offset, &die));
    return odr_is_relevant(&die);
  }

  /// Check if we can assume the One Definition Rule to be relevant
  /// for a given DIE.
  ///
  /// @param die the DIE to consider.
  ///
  /// @return true if the ODR is relevant for @p die.
  bool
  odr_is_relevant(Dwarf_Die *die) const
  {
    translation_unit::language lang;
    if (!get_die_language(die, lang))
      return odr_is_relevant();

    return odr_is_relevant(lang);
  }

  /// Getter for the maps set that associates a decl DIE offset to an
  /// artifact.
  ///
  /// @return the maps set that associates a decl DIE offset to an
  /// artifact.
  die_source_dependant_container_set<die_artefact_map_type>&
  decl_die_artefact_maps()
  {return decl_die_artefact_maps_;}

  /// Getter for the maps set that associates a decl DIE offset to an
  /// artifact.
  ///
  /// @return the maps set that associates a decl DIE offset to an
  /// artifact.
  const die_source_dependant_container_set<die_artefact_map_type>&
  decl_die_artefact_maps() const
  {return decl_die_artefact_maps_;}

  /// Getter for the maps set that associates a type DIE offset to an
  /// artifact.
  ///
  /// @return the maps set that associates a type DIE offset to an
  /// artifact.
  die_source_dependant_container_set<die_artefact_map_type>&
  type_die_artefact_maps()
  {return type_die_artefact_maps_;}

  /// Getter for the maps set that associates a type DIE offset to an
  /// artifact.
  ///
  /// @return the maps set that associates a type DIE offset to an
  /// artifact.
  const die_source_dependant_container_set<die_artefact_map_type>&
  type_die_artefact_maps() const
  {return type_die_artefact_maps_;}

  /// Set the canonical DIE offset of a given DIE.
  ///
  /// @param canonical_dies the vector that holds canonical DIEs.
  ///
  /// @param die_offset the offset of the DIE to set the canonical DIE
  /// for.
  ///
  /// @param canonical_die_offset the canonical DIE offset to
  /// associate to @p die_offset.
  void
  set_canonical_die_offset(offset_offset_map_type &canonical_dies,
			   Dwarf_Off die_offset,
			   Dwarf_Off canonical_die_offset) const
  {
    canonical_dies[die_offset] = canonical_die_offset;}

  /// Set the canonical DIE offset of a given DIE.
  ///
  ///
  /// @param die_offset the offset of the DIE to set the canonical DIE
  /// for.
  ///
  /// @param source the source of the DIE denoted by @p die_offset.
  ///
  /// @param canonical_die_offset the canonical DIE offset to
  /// associate to @p die_offset.
  ///
  /// @param die_as_type if true, it means that @p die_offset has to
  /// be considered as a type.
  void
  set_canonical_die_offset(Dwarf_Off die_offset,
			   die_source source,
			   Dwarf_Off canonical_die_offset,
			   bool die_as_type) const
  {
    offset_offset_map_type &canonical_dies =
      die_as_type
      ? const_cast<read_context*>(this)->canonical_type_die_offsets_.
      get_container(source)
      : const_cast<read_context*>(this)->canonical_decl_die_offsets_.
      get_container(source);

    set_canonical_die_offset(canonical_dies,
			     die_offset,
			     canonical_die_offset);
  }

  /// Set the canonical DIE offset of a given DIE.
  ///
  ///
  /// @param die the DIE to set the canonical DIE for.
  ///
  /// @param canonical_die_offset the canonical DIE offset to
  /// associate to @p die_offset.
  ///
  /// @param die_as_type if true, it means that @p die has to be
  /// considered as a type.
  void
  set_canonical_die_offset(Dwarf_Die *die,
			   Dwarf_Off canonical_die_offset,
			   bool die_as_type) const
  {
    die_source source;
    ABG_ASSERT(get_die_source(die, source));

    Dwarf_Off die_offset = dwarf_dieoffset(die);

    set_canonical_die_offset(die_offset, source,
			     canonical_die_offset,
			     die_as_type);
  }

  /// Get the canonical DIE offset of a given DIE.
  ///
  /// @param canonical_dies the vector that contains canonical DIES.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @return the canonical of the DIE denoted by @p die_offset, or
  /// zero if no canonical DIE was found.
  Dwarf_Off
  get_canonical_die_offset(offset_offset_map_type &canonical_dies,
			   Dwarf_Off die_offset) const
  {
    offset_offset_map_type::const_iterator it = canonical_dies.find(die_offset);
    if (it == canonical_dies.end())
      return 0;
    return it->second;
  }

  /// Get the canonical DIE offset of a given DIE.
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source the source of the DIE denoted by @p die_offset.
  ///
  /// @param die_as_type if true, it means that @p is to be considered
  /// as a type DIE.
  ///
  /// @return the canonical of the DIE denoted by @p die_offset, or
  /// zero if no canonical DIE was found.
  Dwarf_Off
  get_canonical_die_offset(Dwarf_Off die_offset,
			   die_source source,
			   bool die_as_type) const
  {
    offset_offset_map_type &canonical_dies =
      die_as_type
      ? const_cast<read_context*>(this)->canonical_type_die_offsets_.
      get_container(source)
      : const_cast<read_context*>(this)->canonical_decl_die_offsets_.
      get_container(source);

    return get_canonical_die_offset(canonical_dies, die_offset);
  }

  /// Associate a DIE (representing a type) to the type that it
  /// represents.
  ///
  /// @param die the DIE to consider.
  ///
  /// @param type the type to associate the DIE to.
  ///
  /// @param where_offset where in the DIE stream we logically are.
  void
  associate_die_to_type(Dwarf_Die	*die,
			type_base_sptr	type,
			size_t		where)
  {
    if (!type)
      return;

    Dwarf_Die equiv_die;
    get_or_compute_canonical_die(die, equiv_die, where, /*die_as_type=*/true);

    die_artefact_map_type& m =
      type_die_artefact_maps().get_container(*this, &equiv_die);

    size_t die_offset = dwarf_dieoffset(&equiv_die);
    m[die_offset] = type;
  }

  /// Lookup the type associated to a given DIE.
  ///
  /// Note that the DIE must have been associated to type by a
  /// previous invocation of the function
  /// read_context::associate_die_to_type().
  ///
  /// @param die the DIE to consider.
  ///
  /// @return the type associated to the DIE or NULL if no type is
  /// associated to the DIE.
  type_base_sptr
  lookup_type_from_die(Dwarf_Die* die) const
  {
    type_or_decl_base_sptr artifact =
      lookup_artifact_from_die(die, /*die_as_type=*/true);
    if (function_decl_sptr fn = is_function_decl(artifact))
      return fn->get_type();
    return is_type(artifact);
  }

  /// Lookup the type associated to a DIE at a given offset, from a
  /// given source.
  ///
  /// Note that the DIE must have been associated to type by a
  /// previous invocation of the function
  /// read_context::associate_die_to_type().
  ///
  /// @param die_offset the offset of the DIE to consider.
  ///
  /// @param source the source of the DIE to consider.
  ///
  /// @return the type associated to the DIE or NULL if no type is
  /// associated to the DIE.
  type_base_sptr
  lookup_type_from_die_offset(size_t die_offset, die_source source) const
  {
    type_base_sptr result;
    const die_artefact_map_type& m =
      type_die_artefact_maps().get_container(source);
    die_artefact_map_type::const_iterator i = m.find(die_offset);
    if (i != m.end())
      {
	if (function_decl_sptr fn = is_function_decl(i->second))
	  return fn->get_type();
	result = is_type(i->second);
      }

    if (!result)
      {
	// Maybe we are looking for a class type being constructed?
	const die_class_or_union_map_type& m = die_wip_classes_map(source);
	die_class_or_union_map_type::const_iterator i = m.find(die_offset);

	if (i != m.end())
	  result = i->second;
      }

    if (!result)
      {
	// Maybe we are looking for a function type being constructed?
	const die_function_type_map_type& m =
	  die_wip_function_types_map(source);
	die_function_type_map_type::const_iterator i = m.find(die_offset);

	if (i != m.end())
	  result = i->second;
      }

    return result;
  }

  /// Getter of a map that associates a die that represents a
  /// class/struct with the declaration of the class, while the class
  /// is being constructed.
  ///
  /// @param source where the DIE is from.
  ///
  /// @return the map that associates a DIE to the class that is being
  /// built.
  const die_class_or_union_map_type&
  die_wip_classes_map(die_source source) const
  {return const_cast<read_context*>(this)->die_wip_classes_map(source);}

  /// Getter of a map that associates a die that represents a
  /// class/struct with the declaration of the class, while the class
  /// is being constructed.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return the map that associates a DIE to the class that is being
  /// built.
  die_class_or_union_map_type&
  die_wip_classes_map(die_source source)
  {
    switch (source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_wip_classes_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_die_wip_classes_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return die_wip_classes_map_;
  }

  /// Getter for a map that associates a die (that represents a
  /// function type) whith a function type, while the function type is
  /// being constructed (WIP == work in progress).
  ///
  /// @param source where the DIE comes from.n
  ///
  /// @return the map of wip function types.
  const die_function_type_map_type&
  die_wip_function_types_map(die_source source) const
  {return const_cast<read_context*>(this)->die_wip_function_types_map(source);}

  /// Getter for a map that associates a die (that represents a
  /// function type) whith a function type, while the function type is
  /// being constructed (WIP == work in progress).
  ///
  /// @param source where DIEs of the map come from.
  ///
  /// @return the map of wip function types.
  die_function_type_map_type&
  die_wip_function_types_map(die_source source)
  {
    switch (source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_wip_function_types_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_die_wip_function_types_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return die_wip_function_types_map_;
  }

  /// Getter for a map that associates a die with a function decl
  /// which has a linkage name but no elf symbol yet.
  ///
  /// This is to fixup function decls with linkage names, but with no
  /// link to their underlying elf symbol.  There are some DIEs like
  /// that in DWARF sometimes, especially when the compiler optimizes
  /// stuff aggressively.
  die_function_decl_map_type&
  die_function_decl_with_no_symbol_map()
  {return die_function_with_no_symbol_map_;}

  /// Return true iff a given offset is for the DIE of a class that is
  /// being built, but that is not fully built yet.  WIP == "work in
  /// progress".
  ///
  /// @param offset the DIE offset to consider.
  ///
  /// @param source where the DIE of the map come from.
  ///
  /// @return true iff @p offset is the offset of the DIE of a class
  /// that is being currently built.
  bool
  is_wip_class_die_offset(Dwarf_Off offset, die_source source) const
  {
    die_class_or_union_map_type::const_iterator i =
      die_wip_classes_map(source).find(offset);
    return (i != die_wip_classes_map(source).end());
  }

  /// Return true iff a given offset is for the DIE of a function type
  /// that is being built at the moment, but is not fully built yet.
  /// WIP == work in progress.
  ///
  /// @param offset DIE offset to consider.
  ///
  /// @param source where the DIE comes from.
  ///
  /// @return true iff @p offset is the offset of the DIE of a
  /// function type that is being currently built.
  bool
  is_wip_function_type_die_offset(Dwarf_Off offset, die_source source) const
  {
    die_function_type_map_type::const_iterator i =
      die_wip_function_types_map(source).find(offset);
    return (i != die_wip_function_types_map(source).end());
  }

  /// Getter for the map of declaration-only classes that are to be
  /// resolved to their definition classes by the end of the corpus
  /// loading.
  ///
  /// @return a map of string -> vector of classes where the key is
  /// the fully qualified name of the class and the value is the
  /// vector of declaration-only class.
  const string_classes_map&
  declaration_only_classes() const
  {return decl_only_classes_map_;}

  /// Getter for the map of declaration-only classes that are to be
  /// resolved to their definition classes by the end of the corpus
  /// loading.
  ///
  /// @return a map of string -> vector of classes where the key is
  /// the fully qualified name of the class and the value is the
  /// vector of declaration-only class.
  string_classes_map&
  declaration_only_classes()
  {return decl_only_classes_map_;}

  /// If a given class is a declaration-only class then stash it on
  /// the side so that at the end of the corpus reading we can resolve
  /// it to its definition.
  ///
  /// @param klass the class to consider.
  void
  maybe_schedule_declaration_only_class_for_resolution(class_decl_sptr& klass)
  {
    if (klass->get_is_declaration_only()
	&& klass->get_definition_of_declaration() == 0)
      {
	string qn = klass->get_qualified_name();
	string_classes_map::iterator record =
	  declaration_only_classes().find(qn);
	if (record == declaration_only_classes().end())
	  declaration_only_classes()[qn].push_back(klass);
	else
	  record->second.push_back(klass);
      }
  }

  /// Test if a given declaration-only class has been scheduled for
  /// resolution to a defined class.
  ///
  /// @param klass the class to consider for the test.
  ///
  /// @return true iff @p klass is a declaration-only class and if
  /// it's been scheduled for resolution to a defined class.
  bool
  is_decl_only_class_scheduled_for_resolution(class_decl_sptr& klass)
  {
    if (klass->get_is_declaration_only())
      return (declaration_only_classes().find(klass->get_qualified_name())
	      != declaration_only_classes().end());

    return false;
  }

  /// Walk the declaration-only classes that have been found during
  /// the building of the corpus and resolve them to their definitions.
  void
  resolve_declaration_only_classes()
  {
    vector<string> resolved_classes;

    for (string_classes_map::iterator i =
	   declaration_only_classes().begin();
	 i != declaration_only_classes().end();
	 ++i)
      {
	bool to_resolve = false;
	for (classes_type::iterator j = i->second.begin();
	     j != i->second.end();
	     ++j)
	  if ((*j)->get_is_declaration_only()
	      && ((*j)->get_definition_of_declaration() == 0))
	    to_resolve = true;

	if (!to_resolve)
	  {
	    resolved_classes.push_back(i->first);
	    continue;
	  }

	// Now, for each decl-only class that have the current name
	// 'i->first', let's try to poke at the fully defined class
	// that is defined in the same translation unit as the
	// declaration.
	//
	// If we find one class (defined in the TU of the declaration)
	// that defines the declaration, then the declaration can be
	// resolved to that class.
	//
	// If no defining class is found in the TU of the declaration,
	// then there are possibly three cases to consider:
	//
	//   1/ There is exactly one class that defines the
	//   declaration and that class is defined in another TU.  In
	//   this case, the declaration is resolved to that
	//   definition.
	//
	//   2/ There are more than one class that define that
	//   declaration and none of them is defined in the TU of the
	//   declaration.  In this case, the declaration is left
	//   unresolved.
	//
	//   3/ No class defines the declaration.  In this case, the
	//   declaration is left unresoved.

	// So get the classes that might define the current
	// declarations which name is i->first.
	const type_base_wptrs_type *classes =
	  lookup_class_types(i->first, *current_corpus());
	if (!classes)
	  continue;

	unordered_map<string, class_decl_sptr> per_tu_class_map;
	for (type_base_wptrs_type::const_iterator c = classes->begin();
	     c != classes->end();
	     ++c)
	  {
	    class_decl_sptr klass = is_class_type(type_base_sptr(*c));
	    ABG_ASSERT(klass);

	    klass = is_class_type(look_through_decl_only_class(klass));
	    if (klass->get_is_declaration_only())
	      continue;

	    string tu_path = klass->get_translation_unit()->get_absolute_path();
	    if (tu_path.empty())
	      continue;

	    // Build a map that associates the translation unit path
	    // to the class (that potentially defines the declarations
	    // that we consider) that are defined in that translation unit.
	    per_tu_class_map[tu_path] = klass;
	  }

	if (!per_tu_class_map.empty())
	  {
	    // Walk the declarations to resolve and resolve them
	    // either to the definitions that are in the same TU as
	    // the declaration, or to the definition found elsewhere,
	    // if there is only one such definition.
	    for (classes_type::iterator j = i->second.begin();
		 j != i->second.end();
		 ++j)
	      {
		if ((*j)->get_is_declaration_only()
		    && ((*j)->get_definition_of_declaration() == 0))
		  {
		    string tu_path =
		      (*j)->get_translation_unit()->get_absolute_path();
		    unordered_map<string, class_decl_sptr>::const_iterator e =
		      per_tu_class_map.find(tu_path);
		    if (e != per_tu_class_map.end())
		      (*j)->set_definition_of_declaration(e->second);
		    else if (per_tu_class_map.size() == 1)
		      (*j)->set_definition_of_declaration
			(per_tu_class_map.begin()->second);
		  }
	      }
	    resolved_classes.push_back(i->first);
	  }
      }

    size_t num_decl_only_classes = declaration_only_classes().size(),
      num_resolved = resolved_classes.size();
    if (show_stats())
      cerr << "resolved " << num_resolved
	   << " class declarations out of "
	   << num_decl_only_classes
	   << "\n";

    for (vector<string>::const_iterator i = resolved_classes.begin();
	 i != resolved_classes.end();
	 ++i)
      declaration_only_classes().erase(*i);

    for (string_classes_map::iterator i = declaration_only_classes().begin();
	 i != declaration_only_classes().end();
	 ++i)
      {
	if (show_stats())
	  {
	    if (i == declaration_only_classes().begin())
	      cerr << "Here are the "
		   << num_decl_only_classes - num_resolved
		   << " unresolved class declarations:\n";
	    else
	      cerr << "    " << i->first << "\n";
	  }
      }
  }

  /// Some functions described by DWARF may have their linkage name
  /// set, but no link to their actual underlying elf symbol.  When
  /// these are virtual member functions, comparing the enclosing type
  /// against another one which has its underlying symbol properly set
  /// might lead to spurious type changes.
  ///
  /// If the corpus contains a symbol with the same name as the
  /// linkage name of the function, then set up the link between the
  /// function and its underlying symbol.
  ///
  /// Note that for the moment, only virtual member functions are
  /// fixed up like this.  This is because they really are the only
  /// fuctions of functions that can affect types (in spurious ways).
  void
  fixup_functions_with_no_symbols()
  {
    corpus_sptr corp = current_corpus();
    if (!corp)
      return;

    die_function_decl_map_type &fns_with_no_symbol =
      die_function_decl_with_no_symbol_map();

    if (do_log())
      cerr << fns_with_no_symbol.size()
	   << " functions to fixup, potentially\n";

    for (die_function_decl_map_type::iterator i = fns_with_no_symbol.begin();
	 i != fns_with_no_symbol.end();
	 ++i)
      if (elf_symbol_sptr sym =
	  corp->lookup_function_symbol(i->second->get_linkage_name()))
	{
	  ABG_ASSERT(is_member_function(i->second));
	  ABG_ASSERT(get_member_function_is_virtual(i->second));
	  i->second->set_symbol(sym);
	  if (do_log())
	    cerr << "fixed up '"
		 << i->second->get_pretty_representation()
		 << "' with symbol '"
		 << sym->get_id_string()
		 << "'\n";
	}

    fns_with_no_symbol.clear();
  }

  /// Return a reference to the vector containing the offsets of the
  /// types that need late canonicalizing.
  ///
  /// @param source whe DIEs referred to by the offsets contained in
  /// the vector to return are from.
  vector<Dwarf_Off>&
  types_to_canonicalize(die_source source)
  {
    switch (source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alt_types_to_canonicalize_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_unit_types_to_canonicalize_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return types_to_canonicalize_;
  }

  /// Return a reference to the vector containing the offsets of the
  /// types that need late canonicalizing.
  ///
  /// @param source where the DIEs referred to by the offset in the
  /// returned vector are from.
  const vector<Dwarf_Off>&
  types_to_canonicalize(die_source source) const
  {return const_cast<read_context*>(this)->types_to_canonicalize(source);}

  /// Return a reference to the vector containing the types created
  /// during the binary analysis but that are not tied to a given
  /// DWARF DIE.
  ///
  /// @return reference to the vector containing the types created
  /// during the binary analysis but that are not tied to a given
  /// DWARF DIE.
  const vector<type_base_sptr>&
  extra_types_to_canonicalize() const
  {return extra_types_to_canonicalize_;}

  /// Clear the containers holding types to canonicalize.
  void
  clear_types_to_canonicalize()
  {
    types_to_canonicalize_.clear();
    alt_types_to_canonicalize_.clear();
    type_unit_types_to_canonicalize_.clear();
    extra_types_to_canonicalize_.clear();
  }

  /// Put the offset of a DIE representing a type on a side vector so
  /// that when the reading of the debug info of the current
  /// translation unit is done, we can get back to the type DIE and
  /// from there, to the type it's associated to, and then
  /// canonicalize it.  This what we call late canonicalization.
  ///
  /// @param die the type DIE to schedule for late type
  /// canonicalization.
  void
  schedule_type_for_late_canonicalization(Dwarf_Die *die)
  {
    Dwarf_Off o;
    die_source source;

    Dwarf_Die equiv_die;
    ABG_ASSERT(get_canonical_die(die, equiv_die,
			     /*where=*/0,
			     /*die_as_type=*/true));

    ABG_ASSERT(get_die_source(&equiv_die, source));
    o = dwarf_dieoffset(&equiv_die);

    const die_artefact_map_type& m =
      type_die_artefact_maps().get_container(*this, die);

    die_artefact_map_type::const_iterator i = m.find(o);
    ABG_ASSERT(i != m.end());

    // Then really do the scheduling.
    types_to_canonicalize(source).push_back(o);
  }

  /// Types that were created but not tied to a particular DIE, must
  /// be scheduled for late canonicalization using this method.
  ///
  /// @param t the type to schedule for late canonicalization.
  void
  schedule_type_for_late_canonicalization(const type_base_sptr &t)
  {
    extra_types_to_canonicalize_.push_back(t);
  }

  /// Canonicalize types which DIE offsets are stored in vectors on
  /// the side.  This is a sub-routine of
  /// read_context::perform_late_type_canonicalizing().
  ///
  /// @param source where the DIE of the types to canonicalize are
  /// from.
  void
  canonicalize_types_scheduled(die_source source)
  {
    if (do_log())
      {
	cerr << "going to canonicalize types";
	corpus_sptr c = current_corpus();
	if (c)
	  cerr << " of corpus " << current_corpus()->get_path();
	cerr << " (DIEs source: " << source << ")\n";
      }

    if (!types_to_canonicalize(source).empty())
      {
	size_t total = types_to_canonicalize(source).size();
	if (do_log())
	  cerr << total << " types to canonicalize\n";
	for (size_t i = 0; i < total; ++i)
	  {
	    Dwarf_Off element = types_to_canonicalize(source)[i];
	    type_base_sptr t =
	      lookup_type_from_die_offset(element, source);
	    ABG_ASSERT(t);
	    if (do_log())
	      {
		cerr << "canonicalizing type "
		     << get_pretty_representation(t, false)
		     << " [" << i + 1 << "/" << total << "]";
		if (corpus_sptr c = current_corpus())
		  cerr << "@" << c->get_path();
		cerr << " ...";
	      }
	    canonicalize(t);
	    if (do_log())
	      cerr << " DONE\n";
	  }

	// Now canonicalize types that were created but not tied to
	// any DIE.
	if (!extra_types_to_canonicalize().empty())
	  {
	    size_t total = extra_types_to_canonicalize().size();
	    if (do_log())
	      cerr << total << " extra types to canonicalize\n";
	    size_t i = 1;
	    for (vector<type_base_sptr>::const_iterator it =
		   extra_types_to_canonicalize().begin();
		 it != extra_types_to_canonicalize().end();
		 ++it, ++i)
	      {
		if (do_log())
		  {
		    cerr << "canonicalizing extra type "
			 << get_pretty_representation(*it, false)
			 << " [" << i << "/" << total << "]";
		    if (corpus_sptr c = current_corpus())
		      cerr << "@" << c->get_path();
		    cerr << " ...";
		  }
		canonicalize(*it);
	      }
	  }
      }
    if (do_log())
      cerr << "finished canonicalizing types.  (source: "
	   << source << ")\n";
  }

  /// Compute the number of canonicalized and missed types in the late
  /// canonicalization phase.
  ///
  /// @param source where the DIEs of the canonicalized types are
  /// from.
  ///
  /// @param canonicalized the number of types that got canonicalized
  /// is added to the value already present in this parameter.
  ///
  /// @param missed the number of types scheduled for late
  /// canonicalization and which couldn't be canonicalized (for a
  /// reason) is added to the value already present in this parameter.
  void
  add_late_canonicalized_types_stats(die_source	source,
				     size_t&		canonicalized,
				     size_t&		missed) const
  {
    for (vector<Dwarf_Off>::const_iterator i =
	   types_to_canonicalize(source).begin();
	 i != types_to_canonicalize(source).end();
	 ++i)
      {
        type_base_sptr t = lookup_type_from_die_offset(*i, source);
	if (t->get_canonical_type())
	  ++canonicalized;
	else
	  ++missed;
      }
  }

  /// Compute the number of canonicalized and missed types in the late
  /// canonicalization phase.
  ///
  /// @param canonicalized the number of types that got canonicalized
  /// is added to the value already present in this parameter.
  ///
  /// @param missed the number of types scheduled for late
  /// canonicalization and which couldn't be canonicalized (for a
  /// reason) is added to the value already present in this parameter.
  void
  add_late_canonicalized_types_stats(size_t& canonicalized,
				     size_t& missed) const
  {
    for (die_source source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	 source < NUMBER_OF_DIE_SOURCES;
	 ++source)
      add_late_canonicalized_types_stats(source, canonicalized, missed);
  }

  // Look at the types that need to be canonicalized after the
  // translation unit has been constructed and canonicalize them.
  void
  perform_late_type_canonicalizing()
  {
    for (die_source source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
	 source < NUMBER_OF_DIE_SOURCES;
	 ++source)
      canonicalize_types_scheduled(source);

    if (show_stats())
      {
	size_t num_canonicalized = 0, num_missed = 0, total = 0;
	add_late_canonicalized_types_stats(num_canonicalized,
					   num_missed);
	total = num_canonicalized + num_missed;
	cerr << "binary: "
	     << elf_path()
	     << "\n";
	cerr << "    # late canonicalized types: "
	     << num_canonicalized
	     << " (" << num_canonicalized * 100 / total << "%)\n"
	     << "    # missed canonicalization opportunities: "
	     << num_missed
	     << " (" << num_missed * 100 / total << "%)\n";
      }

  }

  const die_tu_map_type&
  die_tu_map() const
  {return die_tu_map_;}

  die_tu_map_type&
  die_tu_map()
  {return die_tu_map_;}

  /// Getter for the map that associates a translation unit DIE to the
  /// vector of imported unit points that it contains.
  ///
  /// @param source where the DIEs are from.
  ///
  /// @return the map.
  const tu_die_imported_unit_points_map_type&
  tu_die_imported_unit_points_map(die_source source) const
  {return const_cast<read_context*>(this)->tu_die_imported_unit_points_map(source);}

  /// Getter for the map that associates a translation unit DIE to the
  /// vector of imported unit points that it contains.
  ///
  /// @param source where the DIEs are from.
  ///
  /// @return the map.
  tu_die_imported_unit_points_map_type&
  tu_die_imported_unit_points_map(die_source source)
  {
    switch (source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alt_tu_die_imported_unit_points_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_units_tu_die_imported_unit_points_map_;
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	// We cannot reach this point.
	ABG_ASSERT_NOT_REACHED;
      }
    return tu_die_imported_unit_points_map_;
  }

  /// Getter of the current corpus being constructed.
  ///
  /// @return the current corpus.
  const corpus_sptr
  current_corpus() const
  {return cur_corpus_;}

  /// Getter of the current corpus being constructed.
  ///
  /// @return the current corpus.
  corpus_sptr
  current_corpus()
  {return cur_corpus_;}

  /// Setter of the current corpus being constructed.
  ///
  /// @param c the new corpus.
  void
  current_corpus(const corpus_sptr& c)
  {
    if (c)
      cur_corpus_ = c;
  }

  /// Reset the current corpus being constructed.
  ///
  /// This actually deletes the current corpus being constructed.
  void
  reset_current_corpus()
  {cur_corpus_.reset();}

  /// Getter of the current corpus group being constructed.
  ///
  /// @return current the current corpus being constructed, if any, or
  /// nil.
  const corpus_group_sptr
  current_corpus_group() const
  {return cur_corpus_group_;}

  /// Getter of the current corpus group being constructed.
  ///
  /// @return current the current corpus being constructed, if any, or
  /// nil.
  corpus_group_sptr
  current_corpus_group()
  {return cur_corpus_group_;}

  /// Setter of the current corpus group being constructed.
  ///
  /// @param g the new corpus group.
  void
  current_corpus_group(const corpus_group_sptr& g)
  {
    if (g)
      cur_corpus_group_ = g;
  }

  /// Test if there is a corpus group being built.
  ///
  /// @return if there is a corpus group being built, false otherwise.
  bool
  has_corpus_group() const
  {return cur_corpus_group_;}

  /// Return the main corpus from the current corpus group, if any.
  ///
  /// @return the main corpus of the current corpus group, if any, nil
  /// if no corpus group is being constructed.
  corpus_sptr
  main_corpus_from_current_group()
  {
    if (cur_corpus_group_ && !cur_corpus_group_->get_corpora().empty())
      return cur_corpus_group_->get_corpora().front();

    return corpus_sptr();
  }

  /// Return the main corpus from the current corpus group, if any.
  ///
  /// @return the main corpus of the current corpus group, if any, nil
  /// if no corpus group is being constructed.
  const corpus_sptr
  main_corpus_from_current_group() const
  {return const_cast<read_context*>(this)->main_corpus_from_current_group();}

  /// Test if the current corpus being built is the main corpus of the
  /// current corpus group.
  ///
  /// @return return true iff the current corpus being built is the
  /// main corpus of the current corpus group.
  bool
  current_corpus_is_main_corpus_from_current_group() const
  {
    corpus_sptr main_corpus = main_corpus_from_current_group();

    if (main_corpus && main_corpus.get() == cur_corpus_.get())
      return true;

    return false;
  }

  /// Return true if the current corpus is part of a corpus group
  /// being built and if it's not the main corpus of the group.
  ///
  /// For instance, this would return true if we are loading a linux
  /// kernel *module* that is part of the current corpus group that is
  /// being built.  In this case, it means we should re-use types
  /// coming from the "vmlinux" binary that is the main corpus of the
  /// group.
  ///
  /// @return true if the current corpus is part of a corpus group
  /// being built and if it's not the main corpus of the group.
  corpus_sptr
  should_reuse_type_from_corpus_group() const
  {
    if (has_corpus_group() && is_c_language(cur_transl_unit()->get_language()))
      if (corpus_sptr main_corpus = main_corpus_from_current_group())
	if (!current_corpus_is_main_corpus_from_current_group())
	  return main_corpus;

    return corpus_sptr();
  }

  /// Get the map that associates each DIE to its parent DIE.  This is
  /// for DIEs coming from the main debug info sections.
  ///
  /// @param source where the DIEs in the map come from.
  ///
  /// @return the DIE -> parent map.
  const offset_offset_map_type&
  die_parent_map(die_source source) const
  {return const_cast<read_context*>(this)->die_parent_map(source);}

  /// Get the map that associates each DIE to its parent DIE.  This is
  /// for DIEs coming from the main debug info sections.
  ///
  /// @param source where the DIEs in the map come from.
  ///
  /// @return the DIE -> parent map.
  offset_offset_map_type&
  die_parent_map(die_source source)
  {
    switch (source)
      {
      case PRIMARY_DEBUG_INFO_DIE_SOURCE:
	break;
      case ALT_DEBUG_INFO_DIE_SOURCE:
	return alternate_die_parent_map_;
      case TYPE_UNIT_DIE_SOURCE:
	return type_section_die_parent_map();
      case NO_DEBUG_INFO_DIE_SOURCE:
      case NUMBER_OF_DIE_SOURCES:
	ABG_ASSERT_NOT_REACHED;
      }
    return primary_die_parent_map_;
  }

  const offset_offset_map_type&
  type_section_die_parent_map() const
  {return type_section_die_parent_map_;}

  offset_offset_map_type&
  type_section_die_parent_map()
  {return type_section_die_parent_map_;}

  /// Getter of the current translation unit.
  ///
  /// @return the current translation unit being constructed.
  const translation_unit_sptr&
  cur_transl_unit() const
  {return cur_tu_;}

  /// Getter of the current translation unit.
  ///
  /// @return the current translation unit being constructed.
  translation_unit_sptr&
  cur_transl_unit()
  {return cur_tu_;}

  /// Setter of the current translation unit.
  ///
  /// @param tu the current translation unit being constructed.
  void
  cur_transl_unit(translation_unit_sptr tu)
  {
    if (tu)
      cur_tu_ = tu;
  }

  /// Return the global scope of the current translation unit.
  ///
  /// @return the global scope of the current translation unit.
  const scope_decl_sptr&
  global_scope() const
  {return cur_transl_unit()->get_global_scope();}

  /// Return a scope that is nil.
  ///
  /// @return a scope that is nil.
  const scope_decl_sptr&
  nil_scope() const
  {return nil_scope_;}

  const scope_stack_type&
  scope_stack() const
  {return scope_stack_;}

  scope_stack_type&
  scope_stack()
  {return scope_stack_;}

  scope_decl*
  current_scope()
  {
    if (scope_stack().empty())
      {
	if (cur_transl_unit())
	  scope_stack().push(cur_transl_unit()->get_global_scope().get());
      }
    return scope_stack().top();
  }

  list<var_decl_sptr>&
  var_decls_to_re_add_to_tree()
  {return var_decls_to_add_;}

  /// Return the type of the current elf file.
  ///
  /// @return the type of the current elf file.
  elf_type
  get_elf_file_type()
  {
    return elf_file_type(elf_handle());
  }

  /// The section containing the symbol table from the current ELF
  /// file.
  ///
  /// Note that after it's first invocation, this function caches the
  /// symbol table that it found.  Subsequent invocations just return
  /// the cached symbol table section.
  ///
  /// @return the symbol table section if found
  Elf_Scn*
  find_symbol_table_section() const
  {
    if (!symtab_section_)
      dwarf_reader::find_symbol_table_section(elf_handle(),
					      const_cast<read_context*>(this)->symtab_section_);
    return symtab_section_;
  }

  /// Return the "Official Procedure descriptors section."  This
  /// section is named .opd, and is usually present only on PPC64
  /// ELFv1 binaries.
  ///
  /// @return the .opd section, if found.  Return nil otherwise.
  Elf_Scn*
  find_opd_section() const
  {
    if (!opd_section_)
      const_cast<read_context*>(this)->opd_section_=
	find_section(elf_handle(), ".opd", SHT_PROGBITS);
    return opd_section_;
  }

  /// Return the __ksymtab section of a linux kernel ELF file (either
  /// a vmlinux binary or a kernel module).
  ///
  /// @return the __ksymtab section if found, nil otherwise.
  Elf_Scn*
  find_ksymtab_section() const
  {
    if (!ksymtab_section_)
      const_cast<read_context*>(this)->ksymtab_section_ =
	find_section(elf_handle(), "__ksymtab", SHT_PROGBITS);
    return ksymtab_section_;
  }

  /// Return the __ksymtab_gpl section of a linux kernel ELF file
  /// (either a vmlinux binary or a kernel module).
  ///
  /// @return the __ksymtab_gpl section if found, nil otherwise.
  Elf_Scn*
  find_ksymtab_gpl_section() const
  {
    if (!ksymtab_gpl_section_)
      const_cast<read_context*>(this)->ksymtab_gpl_section_ =
	find_section(elf_handle(), "__ksymtab_gpl", SHT_PROGBITS);
    return ksymtab_gpl_section_;
  }

  /// Return the SHT_GNU_versym, SHT_GNU_verdef and SHT_GNU_verneed
  /// sections that are involved in symbol versionning.
  ///
  /// @param versym_section the SHT_GNU_versym section found.
  ///
  /// @param verdef_section the SHT_GNU_verdef section found.
  ///
  /// @param verneed_section the SHT_GNU_verneed section found.
  ///
  /// @return true iff the sections where found.
  bool
  get_symbol_versionning_sections(Elf_Scn*&	versym_section,
				  Elf_Scn*&	verdef_section,
				  Elf_Scn*&	verneed_section)
  {
    if (!symbol_versionning_sections_loaded_)
      {
	symbol_versionning_sections_found_ =
	  dwarf_reader::get_symbol_versionning_sections(elf_handle(),
							versym_section_,
							verdef_section_,
							verneed_section_);
	symbol_versionning_sections_loaded_ = true;
      }

    versym_section = versym_section_;
    verdef_section = verdef_section_;
    verneed_section = verneed_section_;
    return symbol_versionning_sections_found_;
  }

  /// Return the version for a symbol that is at a given index in its
  /// SHT_SYMTAB section.
  ///
  /// The first invocation of this function caches the results and
  /// subsequent invocations just return the cached results.
  ///
  /// @param symbol_index the index of the symbol to consider.
  ///
  /// @param get_def_version if this is true, it means that that we want
  /// the version for a defined symbol; in that case, the version is
  /// looked for in a section of type SHT_GNU_verdef.  Otherwise, if
  /// this parameter is false, this means that we want the version for
  /// an undefined symbol; in that case, the version is the needed one
  /// for the symbol to be resolved; so the version is looked fo in a
  /// section of type SHT_GNU_verneed.
  ///
  /// @param version the version found for symbol at @p symbol_index.
  ///
  /// @return true iff a version was found for symbol at index @p
  /// symbol_index.
  bool
  get_version_for_symbol(size_t		symbol_index,
			 bool			get_def_version,
			 elf_symbol::version&	version)
  {
    Elf_Scn *versym_section = NULL,
      *verdef_section = NULL,
      *verneed_section = NULL;

    if (!get_symbol_versionning_sections(versym_section,
					 verdef_section,
					 verneed_section))
      return false;

    GElf_Versym versym_mem;
    Elf_Data* versym_data = (versym_section)
      ? elf_getdata(versym_section, NULL)
      : NULL;
    GElf_Versym* versym = (versym_data)
      ? gelf_getversym(versym_data, symbol_index, &versym_mem)
      : NULL;

    if (versym == 0 || *versym <= 1)
      // I got these value from the code of readelf.c in elfutils.
      // Apparently, if the symbol version entry has these values, the
      // symbol must be discarded. This is not documented in the
      // official specification.
      return false;

    if (get_def_version)
      {
	if (*versym == 0x8001)
	  // I got this value from the code of readelf.c in elfutils
	  // too.  It's not really documented in the official
	  // specification.
	  return false;

	if (verdef_section
	    && get_version_definition_for_versym(elf_handle(), versym,
						 verdef_section, version))
	  return true;
      }
    else
      {
	if (verneed_section
	    && get_version_needed_for_versym(elf_handle(), versym,
					     verneed_section, version))
	  return true;
      }

    return false;
  }

  /// Look into the symbol tables of the underlying elf file and see
  /// if we find a given symbol.
  ///
  /// @param symbol_name the name of the symbol to look for.
  ///
  /// @param demangle if true, demangle the symbols found in the symbol
  /// tables.
  ///
  /// @param syms the vector of symbols with the name @p symbol_name
  /// that were found.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_symbol_from_elf(const string&			symbol_name,
			 bool				demangle,
			 vector<elf_symbol_sptr>&	syms) const
  {
    return dwarf_reader::lookup_symbol_from_elf(env(),
						elf_handle(),
						symbol_name,
						demangle,
						syms);
  }

  /// Given the index of a symbol into the symbol table of an ELF
  /// file, look the symbol up, build an instace of @ref elf_symbol
  /// and return it.
  ///
  /// @param symbol_index the index of the symbol into the symbol
  /// table of the current elf file.
  ///
  /// @param symbol the resulting instance of @ref elf_symbol, iff the
  /// function returns true.
  ///
  /// @return the elf symbol found or nil if none was found.
  elf_symbol_sptr
  lookup_elf_symbol_from_index(size_t symbol_index)
  {
    Elf_Scn* symtab_section = find_symbol_table_section();
    if (!symtab_section)
      return elf_symbol_sptr();

    GElf_Shdr header_mem;
    GElf_Shdr* symtab_sheader = gelf_getshdr(symtab_section,
					     &header_mem);

    Elf_Data* symtab = elf_getdata(symtab_section, 0);
    ABG_ASSERT(symtab);

    GElf_Sym* s, smem;
    s = gelf_getsym(symtab, symbol_index, &smem);
    if (!s)
      return elf_symbol_sptr();

    bool sym_is_defined = s->st_shndx != SHN_UNDEF;
    bool sym_is_common = s->st_shndx == SHN_COMMON; // this occurs in
						    // relocatable
						    // files.
    const char* name_str = elf_strptr(elf_handle(),
				      symtab_sheader->sh_link,
				      s->st_name);
    if (name_str == 0)
      name_str = "";

    elf_symbol::version ver;
    get_version_for_symbol(symbol_index,
			   sym_is_defined,
			   ver);

    elf_symbol::visibility vis =
      stv_to_elf_symbol_visibility(GELF_ST_VISIBILITY(s->st_other));

    elf_symbol_sptr sym =
      elf_symbol::create(env(), symbol_index, s->st_size, name_str,
			 stt_to_elf_symbol_type(GELF_ST_TYPE(s->st_info)),
			 stb_to_elf_symbol_binding(GELF_ST_BIND(s->st_info)),
			 sym_is_defined, sym_is_common, ver, vis);
    return sym;
  }

  /// Read 8 bytes and convert their value into an uint64_t.
  ///
  /// @param bytes the array of bytes to read the next 8 bytes from.
  /// Note that this array must be at least 8 bytes long.
  ///
  /// @param result where to store the resuting uint64_t that was read.
  ///
  /// @param is_big_endian if true, read the 8 bytes in Big Endian
  /// mode, otherwise, read them in Little Endian.
  ///
  /// @param true if the 8 bytes could be read, false otherwise.
  bool
  read_uint64_from_array_of_bytes(const uint8_t	*bytes,
				  bool			is_big_endian,
				  uint64_t		&result) const
  {
    return read_int_from_array_of_bytes(bytes, 8, is_big_endian, result);
  }

  /// Read N bytes and convert their value into an integer type T.
  ///
  /// Note that N cannot be bigger than 8 for now. The type passed needs to be
  /// at least of the size of number_of_bytes.
  ///
  /// @param bytes the array of bytes to read the next 8 bytes from.
  /// Note that this array must be at least 8 bytes long.
  ///
  /// @param number_of_bytes the number of bytes to read.  This number
  /// cannot be bigger than 8.
  ///
  /// @param is_big_endian if true, read the 8 bytes in Big Endian
  /// mode, otherwise, read them in Little Endian.
  ///
  /// @param result where to store the resuting integer that was read.
  ///
  ///
  /// @param true if the 8 bytes could be read, false otherwise.
  template<typename T>
  bool
  read_int_from_array_of_bytes(const uint8_t	*bytes,
			       unsigned char	number_of_bytes,
			       bool		is_big_endian,
			       T		&result) const
  {
    if (!bytes)
      return false;

    ABG_ASSERT(number_of_bytes <= 8);
    ABG_ASSERT(number_of_bytes <= sizeof(T));

    T res = 0;

    const uint8_t *cur = bytes;
    if (is_big_endian)
      {
	// In Big Endian, the most significant byte is at the lowest
	// address.
	const uint8_t* msb = cur;
	res = *msb;

	// Now read the remaining least significant bytes.
	for (uint i = 1; i < number_of_bytes; ++i)
	  res = (res << 8) | ((T)msb[i]);
      }
    else
      {
	// In Little Endian, the least significant byte is at the
	// lowest address.
	const uint8_t* lsb = cur;
	res = *lsb;
	// Now read the remaining most significant bytes.
	for (uint i = 1; i < number_of_bytes; ++i)
	  res = res | (((T)lsb[i]) << i * 8);
      }

    result = res;
    return true;
  }

  /// Lookup the address of the function entry point that corresponds
  /// to the address of a given function descriptor.
  ///
  /// On PPC64, a function pointer is the address of a function
  /// descriptor.  Function descriptors are located in the .opd
  /// section.  Each function descriptor is a triplet of three
  /// addresses, each one on 64 bits.  Among those three address only
  /// the first one is of any interest to us: the address of the entry
  /// point of the function.
  ///
  /// This function returns the address of the entry point of the
  /// function whose descriptor's address is given.
  ///
  /// http://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi.html#FUNC-DES
  ///
  /// https://www.ibm.com/developerworks/community/blogs/5894415f-be62-4bc0-81c5-3956e82276f3/entry/deeply_understand_64_bit_powerpc_elf_abi_function_descriptors?lang=en
  ///
  /// @param fn_desc_address the address of the function descriptor to
  /// consider.
  ///
  /// @return the address of the entry point of the function whose
  /// descriptor has the address @p fn_desc_address.  If there is no
  /// .opd section (e.g because we are not on ppc64) or more generally
  /// if the function descriptor could not be found then this function
  /// just returns the address of the fuction descriptor.
  GElf_Addr
  lookup_ppc64_elf_fn_entry_point_address(GElf_Addr fn_desc_address) const
  {
    if (!elf_handle())
      return fn_desc_address;

    if (!elf_architecture_is_ppc64())
      return fn_desc_address;

    bool is_big_endian = elf_architecture_is_big_endian();

    Elf_Scn *opd_section = find_opd_section();
    if (!opd_section)
      return fn_desc_address;

    GElf_Shdr header_mem;
    // The section header of the .opd section.
    GElf_Shdr *opd_sheader = gelf_getshdr(opd_section, &header_mem);

    // The offset of the function descriptor entry, in the .opd
    // section.
    size_t fn_desc_offset = fn_desc_address - opd_sheader->sh_addr;
    Elf_Data *elf_data = elf_rawdata(opd_section, 0);

    // Ensure that the opd_section has at least 8 bytes, starting from
    // the offset we want read the data from.
    if (elf_data->d_size <= fn_desc_offset + 8)
      return fn_desc_address;

    // A pointer to the data of the .opd section, that we can actually
    // do something with.
    uint8_t * bytes = (uint8_t*) elf_data->d_buf;

    // The resulting address we are looking for is going to be formed
    // in this variable.
    GElf_Addr result = 0;
    ABG_ASSERT(read_uint64_from_array_of_bytes(bytes + fn_desc_offset,
					   is_big_endian, result));

    return result;
  }

  /// Given the address of the beginning of a function, lookup the
  /// symbol of the function, build an instance of @ref elf_symbol out
  /// of it and return it.
  ///
  /// @param symbol_start_addr the address of the beginning of the
  /// function to consider.
  ///
  /// @param sym the resulting symbol.  This is set iff the function
  /// returns true.
  ///
  /// @return the elf symbol found at address @p symbol_start_addr, or
  /// nil if none was found.
  elf_symbol_sptr
  lookup_elf_fn_symbol_from_address(GElf_Addr symbol_start_addr) const
  {
    addr_elf_symbol_sptr_map_type::const_iterator i,
      nil = fun_entry_addr_sym_map().end();

    if ((i = fun_entry_addr_sym_map().find(symbol_start_addr)) == nil)
      return elf_symbol_sptr();

    return i->second;
  }

  /// Given the address of a global variable, lookup the symbol of the
  /// variable, build an instance of @ref elf_symbol out of it and
  /// return it.
  ///
  /// @param symbol_start_addr the address of the beginning of the
  /// variable to consider.
  ///
  /// @param the symbol found, iff the function returns true.
  ///
  /// @return the elf symbol found or nil if none was found.
  elf_symbol_sptr
  lookup_elf_var_symbol_from_address(GElf_Addr symbol_start_addr) const
  {
    addr_elf_symbol_sptr_map_type::const_iterator i,
      nil = var_addr_sym_map().end();

    if ((i = var_addr_sym_map().find(symbol_start_addr)) == nil)
      return elf_symbol_sptr();

    return i->second;
  }

  /// Lookup an elf symbol, knowing its address.
  ///
  /// This function first looks for a function symbol having this
  /// address; if it doesn't find any, then it looks for a variable
  /// symbol.
  ///
  /// @param symbol_addr the address of the symbol of the symbol we
  /// are looking for.  Note that the address is a relative offset
  /// starting from the beginning of the .text section.  Addresses
  /// that are presen in the symbol table (the one named .symtab).
  ///
  /// @return the elf symbol if found, or nil otherwise.
  elf_symbol_sptr
  lookup_elf_symbol_from_address(GElf_Addr symbol_addr) const
  {
    elf_symbol_sptr result = lookup_elf_fn_symbol_from_address(symbol_addr);
    if (!result)
      result = lookup_elf_var_symbol_from_address(symbol_addr);
    return result;
  }

  /// Look in the symbol tables of the underying elf file and see if
  /// we find a symbol of a given name of function type.
  ///
  /// @param sym_name the name of the symbol to look for.
  ///
  /// @param syms the public function symbols that were found, with
  /// the name @p sym_name.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_public_function_symbol_from_elf(const string&			sym_name,
					 vector<elf_symbol_sptr>&	syms)
  {
    return dwarf_reader::lookup_public_function_symbol_from_elf(env(),
								elf_handle(),
								sym_name,
								syms);
  }

  /// Look in the symbol tables of the underying elf file and see if
  /// we find a symbol of a given name of variable type.
  ///
  /// @param sym_name the name of the symbol to look for.
  ///
  /// @param syms the variable symbols that were found, with the name
  /// @p sym_name.
  ///
  /// @return true iff the symbol was found.
  bool
  lookup_public_variable_symbol_from_elf(const string&		  sym_name,
					 vector<elf_symbol_sptr>& syms)
  {
    return dwarf_reader::lookup_public_variable_symbol_from_elf(env(),
								elf_handle(),
								sym_name,
								syms);
  }

  /// Test if a given function symbol has been exported.
  ///
  /// @param symbol_address the address of the symbol we are looking
  /// for.  Note that this address must be a relative offset from the
  /// beginning of the .text section, just like the kind of addresses
  /// that are present in the .symtab section.
  ///
  /// @returnthe elf symbol if found, or nil otherwise.
  elf_symbol_sptr
  function_symbol_is_exported(GElf_Addr symbol_address) const
  {
    elf_symbol_sptr symbol = lookup_elf_fn_symbol_from_address(symbol_address);
    if (!symbol)
      return symbol;

    if (!symbol->is_public())
      return elf_symbol_sptr();

    address_set_sptr set;
    bool looking_at_linux_kernel_binary =
      load_in_linux_kernel_mode() && is_linux_kernel_binary();

    if (looking_at_linux_kernel_binary)
      {
	if ((set = linux_exported_fn_syms()))
	  {
	    if (set->find(symbol_address) != set->end())
	      return symbol;
	  }
	if ((set = linux_exported_gpl_fn_syms()))
	  {
	    if (set->find(symbol_address) != set->end())
	      return symbol;
	  }
	return elf_symbol_sptr();
      }

    return symbol;
  }

  /// Test if a given variable symbol has been exported.
  ///
  /// @param symbol_address the address of the symbol we are looking
  /// for.  Note that this address must be a relative offset from the
  /// beginning of the .text section, just like the kind of addresses
  /// that are present in the .symtab section.
  ///
  /// @returnthe elf symbol if found, or nil otherwise.
  elf_symbol_sptr
  variable_symbol_is_exported(GElf_Addr symbol_address) const
  {
    elf_symbol_sptr symbol = lookup_elf_var_symbol_from_address(symbol_address);
    if (!symbol)
      return symbol;

    if (!symbol->is_public())
      return elf_symbol_sptr();

    address_set_sptr set;
    bool looking_at_linux_kernel_binary =
      load_in_linux_kernel_mode() && is_linux_kernel_binary();

    if (looking_at_linux_kernel_binary)
      {
	if ((set = linux_exported_var_syms()))
	  {
	    if (set->find(symbol_address) != set->end())
	      return symbol;
	  }
	if ((set = linux_exported_gpl_var_syms()))
	  {
	    if (set->find(symbol_address) != set->end())
	      return symbol;
	  }
	return elf_symbol_sptr();
      }

    return symbol;
  }

  /// Getter for the map of function address -> symbol.
  ///
  /// @return the function address -> symbol map.
  const addr_elf_symbol_sptr_map_sptr
  fun_addr_sym_map_sptr() const
  {
    maybe_load_symbol_maps();
    return fun_addr_sym_map_;
  }

  /// Getter for the map of function address -> symbol.
  ///
  /// @return the function address -> symbol map.
  addr_elf_symbol_sptr_map_sptr
  fun_addr_sym_map_sptr()
  {
    maybe_load_symbol_maps();
    return fun_addr_sym_map_;
  }

  /// Getter for the map of function symbol address -> function symbol
  /// index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  const addr_elf_symbol_sptr_map_type&
  fun_addr_sym_map() const
  {
    maybe_load_symbol_maps();
    return *fun_addr_sym_map_;
  }

  /// Getter for the map of function symbol address -> function symbol
  /// index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  addr_elf_symbol_sptr_map_type&
  fun_addr_sym_map()
  {
    maybe_load_symbol_maps();
    return *fun_addr_sym_map_;
  }

  /// Getter for a pointer to the map that associates the address of
  /// an entry point of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return a pointer to the map that associates the address of an
  /// entry point of a function with the symbol of that function.
  addr_elf_symbol_sptr_map_sptr&
  fun_entry_addr_sym_map_sptr()
  {
    if (!fun_entry_addr_sym_map_ && !fun_addr_sym_map_)
      maybe_load_symbol_maps();
    if (elf_architecture_is_ppc64())
      return fun_entry_addr_sym_map_;
    return fun_addr_sym_map_;
  }

  /// Getter for a pointer to the map that associates the address of
  /// an entry point of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return a pointer to the map that associates the address of an
  /// entry point of a function with the symbol of that function.
  const addr_elf_symbol_sptr_map_sptr&
  fun_entry_addr_sym_map_sptr() const
  {return const_cast<read_context*>(this)->fun_entry_addr_sym_map_sptr();}


  /// Getter for the map that associates the address of an entry point
  /// of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return the map that associates the address of an entry point of
  /// a function with the symbol of that function.
  addr_elf_symbol_sptr_map_type&
  fun_entry_addr_sym_map()
  {return *fun_entry_addr_sym_map_sptr();}

  /// Getter for the map that associates the address of an entry point
  /// of a function with the symbol of that function.
  ///
  /// Note that on non-"PPC64 ELFv1" binaries, this map is the same as
  /// the one that assciates the address of a function with the symbol
  /// of that function.
  ///
  /// @return the map that associates the address of an entry point of
  /// a function with the symbol of that function.
  const addr_elf_symbol_sptr_map_type&
  fun_entry_addr_sym_map() const
  { return *fun_entry_addr_sym_map_sptr();}

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a shared pointer to the map of function symbols.
  const string_elf_symbols_map_sptr&
  fun_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a shared pointer to the map of function symbols.
  string_elf_symbols_map_sptr&
  fun_syms_sptr()
  {
    maybe_load_symbol_maps();
    return fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a reference to the map of function symbols.
  const string_elf_symbols_map_type&
  fun_syms() const
  {
    maybe_load_symbol_maps();
    return *fun_syms_;
  }

  /// Getter for the map of function symbols (name -> sym).
  ///
  /// @return a reference to the map of function symbols.
  string_elf_symbols_map_type&
  fun_syms()
  {
    maybe_load_symbol_maps();
    return *fun_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a shared pointer to the map of variable symbols.
  const string_elf_symbols_map_sptr
  var_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a shared pointer to the map of variable symbols.
  string_elf_symbols_map_sptr
  var_syms_sptr()
  {
    maybe_load_symbol_maps();
    return var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a reference to the map of variable symbols.
  const string_elf_symbols_map_type&
  var_syms() const
  {
    maybe_load_symbol_maps();
    return *var_syms_;
  }

  /// Getter for the map of variable symbols (name -> sym)
  ///
  /// @return a reference to the map of variable symbols.
  string_elf_symbols_map_type&
  var_syms()
  {
    maybe_load_symbol_maps();
    return *var_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined function
  /// symbols.
  const string_elf_symbols_map_sptr&
  undefined_fun_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined function
  /// symbols.
  string_elf_symbols_map_sptr&
  undefined_fun_syms_sptr()
  {
    maybe_load_symbol_maps();
    return undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined function symbols.
  const string_elf_symbols_map_type&
  undefined_fun_syms() const
  {
    maybe_load_symbol_maps();
    return *undefined_fun_syms_;
  }

  /// Getter for the map of undefined function symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined function symbols.
  string_elf_symbols_map_type&
  undefined_fun_syms()
  {
    maybe_load_symbol_maps();
    return *undefined_fun_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined variable
  /// symbols.
  const string_elf_symbols_map_sptr&
  undefined_var_syms_sptr() const
  {
    maybe_load_symbol_maps();
    return undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a (smart) pointer to the map of undefined variable
  /// symbols.
  string_elf_symbols_map_sptr&
  undefined_var_syms_sptr()
  {
    maybe_load_symbol_maps();
    return undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined variable symbols.
  const string_elf_symbols_map_type&
  undefined_var_syms() const
  {
    maybe_load_symbol_maps();
    return *undefined_var_syms_;
  }

  /// Getter for the map of undefined variable symbols (name -> vector
  /// of symbols).
  ///
  /// @return a reference to the map of undefined variable symbols.
  string_elf_symbols_map_type&
  undefined_var_syms()
  {
    maybe_load_symbol_maps();
    return *undefined_var_syms_;
  }

  /// Getter for the set of addresses of function symbols that are
  /// explicitely exported, for a linux kernel (module) binary.  These
  /// are the addresses of function symbols present in the __ksymtab
  /// section
  address_set_sptr&
  linux_exported_fn_syms()
  {return linux_exported_fn_syms_;}

  /// Getter for the set of addresses of functions that are
  /// explicitely exported, for a linux kernel (module) binary.  These
  /// are the addresses of function symbols present in the __ksymtab
  /// section.
  ///
  /// @return the set of addresses of exported function symbols.
  const address_set_sptr&
  linux_exported_fn_syms() const
  {return const_cast<read_context*>(this)->linux_exported_fn_syms();}

  /// Create an empty set of addresses of functions exported from a
  /// linux kernel (module) binary, or return the one that already
  /// exists.
  ///
  /// @return the set of addresses of exported function symbols.
  address_set_sptr&
  create_or_get_linux_exported_fn_syms()
  {
    if (!linux_exported_fn_syms_)
      linux_exported_fn_syms_.reset(new address_set_type);
    return linux_exported_fn_syms_;
  }

  /// Getter for the set of addresses of v ariables that are
  /// explicitely exported, for a linux kernel (module) binary.  These
  /// are the addresses of variable symbols present in the __ksymtab
  /// section.
  ///
  /// @return the set of addresses of exported variable symbols.
  address_set_sptr&
  linux_exported_var_syms()
  {return linux_exported_var_syms_;}

  /// Getter for the set of addresses of variables that are
  /// explicitely exported, for a linux kernel (module) binary.  These
  /// are the addresses of variable symbols present in the __ksymtab
  /// section.
  ///
  /// @return the set of addresses of exported variable symbols.
  const address_set_sptr&
  linux_exported_var_syms() const
  {return const_cast<read_context*>(this)->linux_exported_var_syms();}


  /// Create an empty set of addresses of variables exported from a
  /// linux kernel (module) binary, or return the one that already
  /// exists.
  ///
  /// @return the set of addresses of exported variable symbols.
  address_set_sptr&
  create_or_get_linux_exported_var_syms()
  {
    if (!linux_exported_var_syms_)
      linux_exported_var_syms_.reset(new address_set_type);
    return linux_exported_var_syms_;
  }


  /// Getter for the set of addresses of function symbols that are
  /// explicitely exported as GPL, for a linux kernel (module) binary.
  /// These are the addresses of function symbols present in the
  /// __ksymtab_gpl section.
  address_set_sptr&
  linux_exported_gpl_fn_syms()
  {return linux_exported_gpl_fn_syms_;}

  /// Getter for the set of addresses of function symbols that are
  /// explicitely exported as GPL, for a linux kernel (module) binary.
  /// These are the addresses of function symbols present in the
  /// __ksymtab_gpl section.
  const address_set_sptr&
  linux_exported_gpl_fn_syms() const
  {return const_cast<read_context*>(this)->linux_exported_gpl_fn_syms();}

  /// Create an empty set of addresses of GPL functions exported from
  /// a linux kernel (module) binary, or return the one that already
  /// exists.
  ///
  /// @return the set of addresses of exported function symbols.
  address_set_sptr&
  create_or_get_linux_exported_gpl_fn_syms()
  {
    if (!linux_exported_gpl_fn_syms_)
      linux_exported_gpl_fn_syms_.reset(new address_set_type);
    return linux_exported_gpl_fn_syms_;
  }

  /// Getter for the set of addresses of variable symbols that are
  /// explicitely exported as GPL, for a linux kernel (module) binary.
  /// These are the addresses of variable symbols present in the
  /// __ksymtab_gpl section.
  address_set_sptr&
  linux_exported_gpl_var_syms()
  {return linux_exported_gpl_var_syms_;}

  /// Getter for the set of addresses of variable symbols that are
  /// explicitely exported as GPL, for a linux kernel (module) binary.
  /// These are the addresses of variable symbols present in the
  /// __ksymtab_gpl section.
  const address_set_sptr&
  linux_exported_gpl_var_syms() const
  {return const_cast<read_context*>(this)->linux_exported_gpl_var_syms();}

  /// Create an empty set of addresses of GPL variables exported from
  /// a linux kernel (module) binary, or return the one that already
  /// exists.
  ///
  /// @return the set of addresses of exported variable symbols.
  address_set_sptr&
  create_or_get_linux_exported_gpl_var_syms()
  {
    if (!linux_exported_gpl_var_syms_)
      linux_exported_gpl_var_syms_.reset(new address_set_type);
    return linux_exported_gpl_var_syms_;
  }

  /// Getter for the ELF dt_needed tag.
  const vector<string>&
  dt_needed() const
  {return dt_needed_;}

  /// Getter for the ELF dt_soname tag.
  const string&
  dt_soname() const
  {return dt_soname_;}

  /// Getter for the ELF architecture of the current file.
  const string&
  elf_architecture() const
  {return elf_architecture_;}

  /// Return the size of a word for the current architecture.
  /// @return the size of a word.
  unsigned char
  architecture_word_size() const
  {
    unsigned char word_size = 0;
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);
    if (elf_header->e_ident[EI_CLASS] == ELFCLASS32)
      word_size = 4;
    else if (elf_header->e_ident[EI_CLASS] == ELFCLASS64)
      word_size = 8;
    else
      ABG_ASSERT_NOT_REACHED;
    return word_size;
  }

  /// Test if the architecture of the current binary is ppc64.
  ///
  /// @return true iff the architecture of the current binary is ppc64.
  bool
  elf_architecture_is_ppc64() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    return (elf_header && elf_header->e_machine == EM_PPC64);
  }

  /// Test if the endianness of the current binary is Big Endian.
  ///
  /// https://en.wikipedia.org/wiki/Endianness.
  ///
  /// @return true iff the current binary is Big Endian.
  bool
  elf_architecture_is_big_endian() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    bool is_big_endian = (elf_header->e_ident[EI_DATA] == ELFDATA2MSB);

    if (!is_big_endian)
      ABG_ASSERT(elf_header->e_ident[EI_DATA] == ELFDATA2LSB);

    return is_big_endian;
  }

  /// Test if the current elf file being read is an executable.
  ///
  /// @return true iff the current elf file being read is an
  /// executable.
  bool
  current_elf_file_is_executable() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);
    return elf_header->e_type == ET_EXEC;
  }

  /// Test if the current elf file being read is a dynamic shared
  /// object.
  ///
  /// @return true iff the current elf file being read is a
  /// dynamic shared object.
  bool
  current_elf_file_is_dso() const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);
    return elf_header->e_type == ET_DYN;
  }

  /// Getter for the map of global variables symbol address -> global
  /// variable symbol index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  const addr_elf_symbol_sptr_map_type&
  var_addr_sym_map() const
  {return const_cast<read_context*>(this)->var_addr_sym_map();}

  /// Getter for the map of global variables symbol address -> global
  /// variable symbol index.
  ///
  /// @return the map.  Note that this initializes the map once when
  /// its nedded.
  addr_elf_symbol_sptr_map_type&
  var_addr_sym_map()
  {
    if (!var_addr_sym_map_)
      maybe_load_symbol_maps();
    return *var_addr_sym_map_;
  }

  /// Load the maps address -> function symbol, address -> variable
  /// symbol and the maps of function and variable undefined symbols.
  ///
  /// @param load_fun_map whether to load the address to function map.
  ///
  /// @param load_var_map whether to laod the address to variable map.
  ///
  /// @param load_undefined_fun_map whether to load the undefined
  /// function map.
  ///
  /// @param load_undefined_var_map whether to laod the undefined
  /// variable map.
  ///
  /// @return return true iff the maps have be loaded.
  bool
  load_symbol_maps_from_symtab_section(bool load_fun_map,
				       bool load_var_map,
				       bool load_undefined_fun_map,
				       bool load_undefined_var_map)
  {
    Elf_Scn* symtab_section = find_symbol_table_section();
    if (!symtab_section)
      return false;

    GElf_Shdr header_mem;
    GElf_Shdr* symtab_sheader = gelf_getshdr(symtab_section,
					     &header_mem);
    size_t nb_syms = symtab_sheader->sh_size / symtab_sheader->sh_entsize;

    Elf_Data* symtab = elf_getdata(symtab_section, 0);
    ABG_ASSERT(symtab);

    bool is_ppc64 = elf_architecture_is_ppc64();

    for (size_t i = 0; i < nb_syms; ++i)
      {
	GElf_Sym* sym, sym_mem;
	sym = gelf_getsym(symtab, i, &sym_mem);
	ABG_ASSERT(sym);

	if ((load_fun_map || load_undefined_fun_map)
	    && (GELF_ST_TYPE(sym->st_info) == STT_FUNC
		|| GELF_ST_TYPE(sym->st_info) == STT_GNU_IFUNC))
	  {
	    elf_symbol_sptr symbol = lookup_elf_symbol_from_index(i);
	    ABG_ASSERT(symbol);
	    ABG_ASSERT(symbol->is_function());


	    if (load_fun_map && symbol->is_public())
	      {
		{
		  string_elf_symbols_map_type::iterator it =
		    fun_syms_->find(symbol->get_name());
		  if (it == fun_syms_->end())
		    {
		      (*fun_syms_)[symbol->get_name()] = elf_symbols();
		      it = fun_syms_->find(symbol->get_name());
		    }
		  string name = symbol->get_name();
		  it->second.push_back(symbol);
		}

		{
		  addr_elf_symbol_sptr_map_type::const_iterator it =
		    fun_addr_sym_map_->find(sym->st_value);
		  if (it == fun_addr_sym_map_->end())
		    (*fun_addr_sym_map_)[sym->st_value] = symbol;
		  else  if (sym->st_value != 0)
		    it->second->get_main_symbol()->add_alias(symbol);

		  if (is_ppc64)
		    {
		      // For ppc64 ELFv1 binaries, we need to build a
		      // function entry point address -> function
		      // symbol map.  This is in addition to the
		      // function pointer -> symbol map.  This is
		      // because on ppc64 ELFv1, a function pointer is
		      // different from a function entry point
		      // address.
		      //
		      // On ppc64 ELFv1, the DWARF DIE of a function
		      // references the address of the entry point of
		      // the function symbol; whereas the value of the
		      // function symbol is the function pointer.  As
		      // these addresses are different, if I we want
		      // to get to the symbol of a function from its
		      // entry point address (as referenced by DWARF
		      // function DIEs) we must have the two maps I
		      // mentionned right above.
		      //
		      // In other words, we need a map that associates
		      // a function enty point address with the symbol
		      // of that function, to be able to get the
		      // function symbol that corresponds to a given
		      // function DIE, on ppc64.
		      //
		      // The value of the function pointer (the value
		      // of the symbol) usually refers to the offset
		      // of a table in the .opd section.  But
		      // sometimes, for a symbol named "foo", the
		      // corresponding symbol named ".foo" (note the
		      // dot before foo) which value is the entry
		      // point address of the function; that entry
		      // point address refers to a region in the .text
		      // section.
		      //
		      // So we are only interested in values of the
		      // symbol that are in the .opd section.
		      GElf_Addr fn_desc_addr = sym->st_value;
		      GElf_Addr fn_entry_point_addr =
			lookup_ppc64_elf_fn_entry_point_address(fn_desc_addr);
		      addr_elf_symbol_sptr_map_type::const_iterator it2 =
			fun_entry_addr_sym_map().find(fn_entry_point_addr);

		      if (it2 == fun_entry_addr_sym_map().end())
			fun_entry_addr_sym_map()[fn_entry_point_addr] = symbol;
		      else if (address_is_in_opd_section(fn_desc_addr))
			{
			  // Either
			  //
			  // 'symbol' must have been registered as an
			  // alias for it2->second->get_main_symbol(),
			  // right before the "if (ppc64)" statement.
			  //
			  // Or
			  //
			  // if the name of 'symbol' is foo, then the
			  // name of it2->second is ".foo".  That is,
			  // foo is the name of the symbol when it
			  // refers to the function descriptor in the
			  // .opd section and ".foo" is an internal
			  // name for the address of the entry point
			  // of foo.
			  //
			  // In the latter case, we just want to keep
			  // a refernce to "foo" as .foo is an
			  // internal name.

			  bool two_symbols_alias =
			    it2->second->get_main_symbol()->does_alias(*symbol);
			  bool symbol_is_foo_and_prev_symbol_is_dot_foo =
			    (it2->second->get_name()
			     == string(".") + symbol->get_name());

			  ABG_ASSERT(two_symbols_alias
				 || symbol_is_foo_and_prev_symbol_is_dot_foo);

			  if (symbol_is_foo_and_prev_symbol_is_dot_foo)
			    // Let's just keep a reference of the
			    // symbol that the user sees in the source
			    // code (the one named foo).  The symbol
			    // which name is prefixed with a "dot" is
			    // an artificial one.
			    fun_entry_addr_sym_map()[fn_entry_point_addr] = symbol;
			}
		    }
		}
	      }
	    else if (load_undefined_fun_map && !symbol->is_defined())
	      {
		string_elf_symbols_map_type::iterator it =
		  undefined_fun_syms_->find(symbol->get_name());
		if (it == undefined_fun_syms_->end())
		  {
		    (*undefined_fun_syms_)[symbol->get_name()] = elf_symbols();
		    it = undefined_fun_syms_->find(symbol->get_name());
		  }
		it->second.push_back(symbol);
	      }
	  }
	else if ((load_var_map || load_undefined_var_map)
		 && (GELF_ST_TYPE(sym->st_info) == STT_OBJECT
		     || GELF_ST_TYPE(sym->st_info) == STT_TLS)
		 // If the symbol is for an OBJECT, the index of the
		 // section it refers to cannot be absolute.
		 // Otherwise that OBJECT is not a variable.
		 && (sym->st_shndx != SHN_ABS
		     || GELF_ST_TYPE(sym->st_info) != STT_OBJECT ))
	  {
	    elf_symbol_sptr symbol = lookup_elf_symbol_from_index(i);
	    ABG_ASSERT(symbol);
	    ABG_ASSERT(symbol->is_variable());

	    if (load_var_map && symbol->is_public())
	      {
		{
		  string_elf_symbols_map_type::iterator it =
		    var_syms_->find(symbol->get_name());
		  if (it == var_syms_->end())
		    {
		      (*var_syms_)[symbol->get_name()] = elf_symbols();
		      it = var_syms_->find(symbol->get_name());
		    }
		  string name = symbol->get_name();
		  it->second.push_back(symbol);
		}

		if (symbol->is_common_symbol())
		  {
		    string_elf_symbols_map_type::iterator it =
		      var_syms_->find(symbol->get_name());
		    ABG_ASSERT(it != var_syms_->end());
		    const elf_symbols& common_sym_instances = it->second;
		    ABG_ASSERT(!common_sym_instances.empty());
		    if (common_sym_instances.size() > 1)
		      {
			elf_symbol_sptr main_common_sym =
			  common_sym_instances[0];
			ABG_ASSERT(main_common_sym->get_name()
			       == symbol->get_name());
			ABG_ASSERT(main_common_sym->is_common_symbol());
			ABG_ASSERT(symbol.get() != main_common_sym.get());
			main_common_sym->add_common_instance(symbol);
		      }
		  }
		else
		  {
		    addr_elf_symbol_sptr_map_type::const_iterator it =
		      var_addr_sym_map_->find(sym->st_value);
		    if (it == var_addr_sym_map_->end())
		      (*var_addr_sym_map_)[sym->st_value] = symbol;
		    else
		      it->second->get_main_symbol()->add_alias(symbol);
		  }
	      }
	    else if (load_undefined_var_map && !symbol->is_defined())
	      {
		string_elf_symbols_map_type::iterator it =
		  undefined_var_syms_->find(symbol->get_name());
		if (it == undefined_var_syms_->end())
		  {
		    (*undefined_var_syms_)[symbol->get_name()] = elf_symbols();
		    it = undefined_var_syms_->find(symbol->get_name());
		  }
		it->second.push_back(symbol);
	      }
	  }
      }
    return true;
  }

  /// Try reading the first __ksymtab section entry as if it is in the
  /// pre-v4_19 format and lookup a symbol from the .symbol section to
  /// see if that succeeds.  If it does, then we can assume the
  /// __ksymtab section is in the pre-v4_19 format.
  ///
  /// @return the symbol resulting from the lookup of the symbol
  /// address we got from reading the first entry of the ksymtab
  /// section assuming the pre-v4.19 format.  If nil, it means the
  /// __ksymtab section is not in the pre-v4.19 format.
  elf_symbol_sptr
  try_reading_first_ksymtab_entry_using_pre_v4_19_format() const
  {
    Elf_Scn *section = find_ksymtab_section();
    Elf_Data *elf_data = elf_rawdata(section, 0);
    uint8_t *bytes = reinterpret_cast<uint8_t*>(elf_data->d_buf);
    bool is_big_endian = elf_architecture_is_big_endian();
    elf_symbol_sptr symbol;
    unsigned char symbol_value_size = architecture_word_size();

    GElf_Addr symbol_address = 0, adjusted_symbol_address = 0;
    ABG_ASSERT(read_int_from_array_of_bytes(bytes,
					    symbol_value_size,
					    is_big_endian,
					    symbol_address));
    adjusted_symbol_address = maybe_adjust_fn_sym_address(symbol_address);
    symbol = lookup_elf_symbol_from_address(adjusted_symbol_address);
    return symbol;
  }

  /// Try reading the first __ksymtab section entry as if it is in the
  /// v4_19 format and lookup a symbol from the .symbol section to see
  /// if that succeeds.  If it does, then we can assume the __ksymtab
  /// section is in the v4_19 format.
  ///
  /// @return the symbol resulting from the lookup of the symbol
  /// address we got from reading the first entry of the ksymtab
  /// section assuming the v4.19 format.  If nil, it means the
  /// __ksymtab section is not in the v4.19 format.
  elf_symbol_sptr
  try_reading_first_ksymtab_entry_using_v4_19_format() const
  {
    Elf_Scn *section = find_ksymtab_section();
    Elf_Data *elf_data = elf_rawdata(section, 0);
    uint8_t *bytes = reinterpret_cast<uint8_t*>(elf_data->d_buf);
    bool is_big_endian = elf_architecture_is_big_endian();
    elf_symbol_sptr symbol;

    int32_t offset = 0;
    const unsigned char symbol_value_size = sizeof(offset);
    GElf_Addr symbol_address = 0, adjusted_symbol_address = 0;
    ABG_ASSERT(read_int_from_array_of_bytes(bytes,
					    symbol_value_size,
					    is_big_endian,
					    offset));
    GElf_Shdr mem;
    GElf_Shdr *section_header = gelf_getshdr(section, &mem);
    symbol_address = offset + section_header->sh_addr;

    adjusted_symbol_address = maybe_adjust_fn_sym_address(symbol_address);
    symbol = lookup_elf_symbol_from_address(adjusted_symbol_address);
    return symbol;
  }

  /// Determine the format of the __ksymtab and __ksymtab_gpl
  /// sections.
  ///
  /// This is important because we need the know the format of these
  /// sections to be able to read from them.
  ///
  /// @return the format the __ksymtab[_gpl] sections.
  enum ksymtab_format
  get_ksymtab_format() const
  {
    if (!find_ksymtab_section())
      ksymtab_format_ = UNDEFINED_KSYMTAB_FORMAT;
    else
      {
	if (ksymtab_format_ == UNDEFINED_KSYMTAB_FORMAT)
	  {
	    // OK this is a dirty little heuristic to determine the
	    // format of the ksymtab section.
	    //
	    // We try to read the first ksymtab entry assuming a
	    // pre-v4.19 format.  If that succeeds then we are in the
	    // pr-v4.19 format.  Otherwise, try reading it assuming a
	    // v4.19 format.  For now, we just support
	    // PRE_V4_19_KSYMTAB_FORMAT and V4_19_KSYMTAB_FORMAT.
	    if (try_reading_first_ksymtab_entry_using_pre_v4_19_format())
	      ksymtab_format_ = PRE_V4_19_KSYMTAB_FORMAT;
	    else if (try_reading_first_ksymtab_entry_using_v4_19_format())
	      ksymtab_format_ = V4_19_KSYMTAB_FORMAT;
	    else
	      // If a new format emerges, then we need to add its
	      // support above.
	      ABG_ASSERT_NOT_REACHED;
	  }
      }
    return ksymtab_format_;
  }

  /// Getter of the size of the symbol value part of an entry of the
  /// ksymtab section.
  ///
  /// @return the size of the symbol value part of the entry of the
  /// ksymtab section.
  unsigned char
  get_ksymtab_symbol_value_size() const
  {
    unsigned char result = 0;
    ksymtab_format format = get_ksymtab_format();
    if (format == UNDEFINED_KSYMTAB_FORMAT)
      ;
    else if (format == PRE_V4_19_KSYMTAB_FORMAT)
      result = architecture_word_size();
    else if (format == V4_19_KSYMTAB_FORMAT)
      result = 4;
    else
      ABG_ASSERT_NOT_REACHED;

    return result;
  }

  /// Getter of the size of one entry of the ksymtab section.
  ///
  /// @return the size of one entry of the ksymtab section.
  unsigned char
  get_ksymtab_entry_size() const
  {
    if (ksymtab_entry_size_ == 0)
      // The entry size if 2 *  symbol_value_size.
      ksymtab_entry_size_ = 2 * get_ksymtab_symbol_value_size();

    return ksymtab_entry_size_;
  }

  /// Getter of the number of entries that are present in the ksymtab
  /// section.
  ///
  /// @return the number of entries that are present in the ksymtab
  /// section.
  size_t
  get_nb_ksymtab_entries() const
  {
    if (nb_ksymtab_entries_ == 0)
      {
	Elf_Scn *section = find_ksymtab_section();
	GElf_Shdr header_mem;
	GElf_Shdr *section_header = gelf_getshdr(section, &header_mem);
	size_t entry_size = get_ksymtab_entry_size();
	ABG_ASSERT(entry_size);
	nb_ksymtab_entries_ = section_header->sh_size / entry_size;
      }
    return nb_ksymtab_entries_;
  }

  /// Getter of the number of entries that are present in the
  /// ksymtab_gpl section.
  ///
  /// @return the number of entries that are present in the
  /// ksymtab_gpl section.
  size_t
  get_nb_ksymtab_gpl_entries()
  {
    if (nb_ksymtab_gpl_entries_ == 0)
      {
	Elf_Scn *section = find_ksymtab_gpl_section();
	GElf_Shdr header_mem;
	GElf_Shdr *section_header = gelf_getshdr(section, &header_mem);
	size_t entry_size = get_ksymtab_entry_size();
	ABG_ASSERT(entry_size);
	nb_ksymtab_gpl_entries_ = section_header->sh_size / entry_size;
      }
    return nb_ksymtab_gpl_entries_;
  }

  /// Load a given kernel symbol table.
  ///
  /// One can thus retrieve the resulting symbols by using the
  /// accessors read_context::linux_exported_fn_syms(),
  /// read_context::linux_exported_var_syms(),
  /// read_context::linux_exported_gpl_fn_syms(), or
  /// read_context::linux_exported_gpl_var_syms().
  ///
  /// @param kind the kind of kernel symbol table to load.
  ///
  /// @return true upon successful completion, false otherwise.
  bool
  load_kernel_symbol_table(kernel_symbol_table_kind kind)
  {
    size_t nb_entries = 0;
    Elf_Scn *section = 0;
    address_set_sptr linux_exported_fns_set, linux_exported_vars_set;

    switch (kind)
      {
      case KERNEL_SYMBOL_TABLE_KIND_UNDEFINED:
	break;
      case KERNEL_SYMBOL_TABLE_KIND_KSYMTAB:
	section = find_ksymtab_section();
	nb_entries = get_nb_ksymtab_entries();
	linux_exported_fns_set = create_or_get_linux_exported_fn_syms();
	linux_exported_vars_set = create_or_get_linux_exported_var_syms();
	break;
      case KERNEL_SYMBOL_TABLE_KIND_KSYMTAB_GPL:
	section = find_ksymtab_gpl_section();
	nb_entries = get_nb_ksymtab_gpl_entries();
	linux_exported_fns_set = create_or_get_linux_exported_gpl_fn_syms();
	linux_exported_vars_set = create_or_get_linux_exported_gpl_var_syms();
	break;
      }

    if (!linux_exported_vars_set || !linux_exported_fns_set)
      return false;

    // The data of the section.
    Elf_Data *elf_data = elf_rawdata(section, 0);

    // An array-of-bytes view of the elf data above.  Something we can
    // actually program with.  Phew.
    uint8_t *bytes = reinterpret_cast<uint8_t*>(elf_data->d_buf);

    // This is where to store an address of a symbol that we read from
    // the section.
    GElf_Addr symbol_address = 0, adjusted_symbol_address = 0;

    // So the section is an array of entries.  Each entry describes a
    // symbol.  Each entry is made of two words.
    //
    // The first word is the address of a symbol.  The second one is
    // the address of a static global variable symbol which value is
    // the string representing the symbol name.  That string is in the
    // __ksymtab_strings section.  Here, we are only interested in the
    // first entry.
    //
    // Lets thus walk the array of entries, and let's read just the
    // symbol address part of each entry.
    bool is_big_endian = elf_architecture_is_big_endian();
    elf_symbol_sptr symbol;
    unsigned char symbol_value_size = get_ksymtab_symbol_value_size();

    for (size_t i = 0, entry_offset = 0;
	 i < nb_entries;
	 ++i, entry_offset = get_ksymtab_entry_size() * i)
      {
	symbol_address = 0;
	ABG_ASSERT(read_int_from_array_of_bytes(&bytes[entry_offset],
						symbol_value_size,
						is_big_endian,
						symbol_address));

	// Starting from linux kernel v4.19, it can happen that the
	// address value read from the ksymtab[_gpl] section might
	// need some decoding to get the real symbol address that has
	// a meaning in the .symbol section.
	symbol_address =
	  maybe_adjust_sym_address_from_v4_19_ksymtab(symbol_address,
						      entry_offset, section);

	// We might also want to adjust the symbol address, depending
	// on if we are looking at an ET_REL, an executable or a
	// shared object binary.
	adjusted_symbol_address = maybe_adjust_fn_sym_address(symbol_address);

	// OK now the symbol address should be in a suitable form to
	// be used to look the symbol up in the usual .symbol section
	// (aka ELF symbol table).
	symbol = lookup_elf_symbol_from_address(adjusted_symbol_address);
	if (!symbol)
	  {
	    adjusted_symbol_address =
	      maybe_adjust_var_sym_address(symbol_address);
	    symbol = lookup_elf_symbol_from_address(adjusted_symbol_address);
	    if (!symbol)
	      // This must be a symbol that is of type neither FUNC
	      // (function) nor OBJECT (variable).  There are for intance,
	      // symbols of type 'NOTYPE' in the ksymtab symbol table.  I
	      // am not sure what those are.
	      continue;
	  }

	address_set_sptr set;
	if (symbol->is_function())
	  {
	    ABG_ASSERT(lookup_elf_fn_symbol_from_address(adjusted_symbol_address));
	    set = linux_exported_fns_set;
	  }
	else if (symbol->is_variable())
	  {
	    ABG_ASSERT(lookup_elf_var_symbol_from_address(adjusted_symbol_address));
	    set = linux_exported_vars_set;
	  }
	else
	  ABG_ASSERT_NOT_REACHED;
	set->insert(adjusted_symbol_address);
      }
    return true;
  }

  /// Load the special __ksymtab section. This is for linux kernel
  /// (module) files.
  ///
  /// @return true upon successful completion, false otherwise.
  bool
  load_ksymtab_symbols()
  {
    return load_kernel_symbol_table(KERNEL_SYMBOL_TABLE_KIND_KSYMTAB);
  }

  /// Load the special __ksymtab_gpl section. This is for linux kernel
  /// (module) files.
  ///
  /// @return true upon successful completion, false otherwise.
  bool
  load_ksymtab_gpl_symbols()
  {
    return load_kernel_symbol_table(KERNEL_SYMBOL_TABLE_KIND_KSYMTAB_GPL);
  }

  /// Load linux kernel (module) specific exported symbol sections.
  ///
  /// @return true upon successful completion, false otherwise.
  bool
  load_linux_specific_exported_symbol_maps()
  {
    bool loaded = false;
    if (!linux_exported_fn_syms_
	|| !linux_exported_var_syms_)
      loaded |= load_ksymtab_symbols();

    if (!linux_exported_gpl_fn_syms_
	|| !linux_exported_gpl_var_syms_)
      loaded |= load_ksymtab_gpl_symbols();

    return loaded;
  }

  /// Load the maps of function symbol address -> function symbol,
  /// global variable symbol address -> variable symbol and also the
  /// maps of function and variable undefined symbols.
  ///
  /// All these maps are loaded only if they are not loaded already.
  ///
  /// @return true iff everything went fine.
  bool
  load_symbol_maps()
  {
    bool load_fun_map = !fun_addr_sym_map_ ;
    bool load_var_map = !var_addr_sym_map_;
    bool load_undefined_fun_map = !undefined_fun_syms_;
    bool load_undefined_var_map = !undefined_var_syms_;

    if (!fun_syms_)
      fun_syms_.reset(new string_elf_symbols_map_type);

    if (!fun_addr_sym_map_)
      fun_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!fun_entry_addr_sym_map_ && elf_architecture_is_ppc64())
      fun_entry_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!var_syms_)
      var_syms_.reset(new string_elf_symbols_map_type);

    if (!var_addr_sym_map_)
      var_addr_sym_map_.reset(new addr_elf_symbol_sptr_map_type);

    if (!undefined_fun_syms_)
      undefined_fun_syms_.reset(new string_elf_symbols_map_type);

    if (!undefined_var_syms_)
      undefined_var_syms_.reset(new string_elf_symbols_map_type);

    if (!options_.ignore_symbol_table)
      {
	if (load_symbol_maps_from_symtab_section(load_fun_map,
						 load_var_map,
						 load_undefined_fun_map,
						 load_undefined_var_map))
	  {
	    if (load_in_linux_kernel_mode() && is_linux_kernel_binary())
	      return load_linux_specific_exported_symbol_maps();
	    return true;
	  }
	return false;
      }
    return true;
  }

  /// Return true if an address is in the ".opd" section that is
  /// present on the ppc64 platform.
  ///
  /// @param addr the address to consider.
  ///
  /// @return true iff @p addr is designates a word that is in the
  /// ".opd" section.
  bool
  address_is_in_opd_section(Dwarf_Addr addr)
  {
    Elf_Scn * opd_section = find_opd_section();
    if (!opd_section)
      return false;
    if (address_is_in_section(addr, opd_section))
      return true;
    return false;
  }

  /// Load the symbol maps if necessary.
  ///
  /// @return true iff the symbol maps has been loaded by this
  /// invocation.
  bool
  maybe_load_symbol_maps() const
  {
    if (!fun_addr_sym_map_
	|| !var_addr_sym_map_
	|| !fun_syms_
	|| !var_syms_
	|| !undefined_fun_syms_
	|| !undefined_var_syms_)
      return const_cast<read_context*>(this)->load_symbol_maps();
    return false;
  }

  /// Load the DT_NEEDED and DT_SONAME elf TAGS.
  ///
  void
  load_dt_soname_and_needed()
  {
    lookup_data_tag_from_dynamic_segment(elf_handle(), DT_NEEDED, dt_needed_);

    vector<string> dt_tag_data;
    lookup_data_tag_from_dynamic_segment(elf_handle(), DT_SONAME, dt_tag_data);
    if (!dt_tag_data.empty())
      dt_soname_ = dt_tag_data[0];
  }

  /// Read the string representing the architecture of the current ELF
  /// file.
  void
  load_elf_architecture()
  {
    if (!elf_handle())
      return;

    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    elf_architecture_ = e_machine_to_string(elf_header->e_machine);
  }

  /// Load various ELF data.
  ///
  /// This function loads ELF data that are not symbol maps or debug
  /// info.  That is, things like various tags, elf architecture and
  /// so on.
  void
  load_elf_properties()
  {
    load_dt_soname_and_needed();
    load_elf_architecture();
  }

  /// Convert the value of the symbol address part of a post V4.19
  /// ksymtab entry (that contains place-relative addresses) into its
  /// corresponding symbol value in the .symtab section.  The value of
  /// the symbol in .symtab equals to addr_offset + address-of-ksymtab
  /// + addr.
  ///
  /// @param addr the address read from the ksymtab section.
  ///
  /// @param addr_offset the offset at which @p addr was read.
  ///
  /// @param ksymtab_section the kymstab section @p addr was read
  /// from.
  GElf_Addr
  maybe_adjust_sym_address_from_v4_19_ksymtab(GElf_Addr addr,
					      size_t addr_offset,
					      Elf_Scn *ksymtab_section) const
  {
    GElf_Addr result = addr;

    if (get_ksymtab_format() == V4_19_KSYMTAB_FORMAT)
      {
	GElf_Shdr mem;
	GElf_Shdr *section_header = gelf_getshdr(ksymtab_section, &mem);
	if (architecture_word_size() == 4)
	  result = (uint32_t)(addr + section_header->sh_addr + addr_offset);
	else if (architecture_word_size() == 8)
	  {
	    result = addr + section_header->sh_addr + addr_offset;
	    if (result < ((uint64_t)1 << 32))
	      // The symbol address is expressed in 32 bits.  So let's
	      // convert it to a 64 bits address with the 4 most
	      // significant bytes set to ff each.  This is how 64
	      // bits addresses of symbols are in the .symbol section,
	      // so we need this address to be consistent with that
	      // format.
	      result = ((uint64_t)0xffffffff << 32) | result;
	  }
	else
	  ABG_ASSERT_NOT_REACHED;
      }

    return result;
  }

  /// This is a sub-routine of maybe_adjust_fn_sym_address and
  /// maybe_adjust_var_sym_address.
  ///
  /// Given an address that we got by looking at some debug
  /// information (e.g, a symbol's address referred to by a DWARF
  /// TAG), If the ELF file we are interested in is a shared library
  /// or an executable, then adjust the address to be coherent with
  /// where the executable (or shared library) is loaded.  That way,
  /// the address can be used to look for symbols in the executable or
  /// shared library.
  ///
  /// @return the adjusted address, or the same address as @p addr if
  /// it didn't need any adjustment.
  Dwarf_Addr
  maybe_adjust_address_for_exec_or_dyn(Dwarf_Addr addr) const
  {
    GElf_Ehdr eh_mem;
    GElf_Ehdr *elf_header = gelf_getehdr(elf_handle(), &eh_mem);

    if (elf_header->e_type == ET_DYN || elf_header->e_type == ET_EXEC)
      {
	Dwarf_Addr dwarf_elf_load_address = 0, elf_load_address = 0;
	ABG_ASSERT(get_binary_load_address(dwarf_elf_handle(),
				       dwarf_elf_load_address));
	ABG_ASSERT(get_binary_load_address(elf_handle(),
				       elf_load_address));
	if (dwarf_is_splitted()
	    && (dwarf_elf_load_address != elf_load_address))
	  // This means that in theory the DWARF an the executable are
	  // not loaded at the same address.  And addr is meaningful
	  // only in the context of the DWARF.
	  //
	  // So let's transform addr into an offset relative to where
	  // the DWARF is loaded, and let's add that relative offset
	  // to the load address of the executable.  That way, addr
	  // becomes meaningful in the context of the executable and
	  // can thus be used to compare against the address of
	  // symbols of the executable, for instance.
	  addr = addr - dwarf_elf_load_address + elf_load_address;
      }

    return addr;
  }

  /// For a relocatable (*.o) elf file, this function expects an
  /// absolute address, representing a function symbol.  It then
  /// extracts the address of the .text section from the symbol
  /// absolute address to get the relative address of the function
  /// from the beginning of the .text section.
  ///
  /// For executable or shared library, this function expects an
  /// address of a function symbol that was retrieved by looking at a
  /// DWARF "file".  The function thus adjusts the address to make it
  /// be meaningful in the context of the ELF file.
  ///
  /// In both cases, the address can then be compared against the
  /// st_value field of a function symbol from the ELF file.
  ///
  /// @param addr an adress for a function symbol that was retrieved
  /// from a DWARF file.
  ///
  /// @return the (possibly) adjusted address, or just @p addr if no
  /// adjustment took place.
  Dwarf_Addr
  maybe_adjust_fn_sym_address(Dwarf_Addr addr) const
  {
    Elf* elf = elf_handle();
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf, &eh_mem);

    if (elf_header->e_type == ET_REL)
      {
	Elf_Scn* text_section = find_text_section(elf);
	ABG_ASSERT(text_section);

	GElf_Shdr sheader_mem;
	GElf_Shdr* text_sheader = gelf_getshdr(text_section, &sheader_mem);
	ABG_ASSERT(text_sheader);
	addr = addr - text_sheader->sh_addr;
      }
    else
      addr = maybe_adjust_address_for_exec_or_dyn(addr);

    return addr;
  }

  /// Test if a given address is in a given section.
  ///
  /// @param addr the address to consider.
  ///
  /// @param section the section to consider.
  bool
  address_is_in_section(Dwarf_Addr addr, Elf_Scn* section) const
  {
    if (!section)
      return false;

    GElf_Shdr sheader_mem;
    GElf_Shdr* sheader = gelf_getshdr(section, &sheader_mem);

    if (sheader->sh_addr <= addr && addr <= sheader->sh_addr + sheader->sh_size)
      return true;

    return false;
  }

  /// Get the section which a global variable address comes from.
  ///
  /// @param var_addr the address for the variable.
  ///
  /// @return the ELF section the @p var_addr comes from, or nil if no
  /// section was found for that variable address.
  Elf_Scn*
  get_data_section_for_variable_address(Dwarf_Addr var_addr) const
  {
    // There are several potential 'data sections" from which a
    // variable address can come from: .data, .data1 and .rodata.
    // Let's try to try them all in sequence.

    Elf_Scn* data_scn = bss_section();
    if (!address_is_in_section(var_addr, data_scn))
      {
	data_scn = data_section();
	if (!address_is_in_section(var_addr, data_scn))
	  {
	    data_scn = data1_section();
	    if (!address_is_in_section(var_addr, data_scn))
	      {
		data_scn = rodata_section();
		if (!address_is_in_section(var_addr, data_scn))
		  return 0;
	      }
	  }
      }
    return data_scn;
  }

  /// For a relocatable (*.o) elf file, this function expects an
  /// absolute address, representing a global variable symbol.  It
  /// then extracts the address of the {.data,.data1,.rodata,.bss}
  /// section from the symbol absolute address to get the relative
  /// address of the variable from the beginning of the data section.
  ///
  /// For executable or shared library, this function expects an
  /// address of a variable symbol that was retrieved by looking at a
  /// DWARF "file".  The function thus adjusts the address to make it
  /// be meaningful in the context of the ELF file.
  ///
  /// In both cases, the address can then be compared against the
  /// st_value field of a function symbol from the ELF file.
  ///
  /// @param addr an address for a global variable symbol that was
  /// retrieved from a DWARF file.
  ///
  /// @return the (possibly) adjusted address, or just @p addr if no
  /// adjustment took place.
  Dwarf_Addr
  maybe_adjust_var_sym_address(Dwarf_Addr addr) const
  {
    Elf* elf = elf_handle();
    GElf_Ehdr eh_mem;
    GElf_Ehdr* elf_header = gelf_getehdr(elf, &eh_mem);

    if (elf_header->e_type == ET_REL)
      {
	Elf_Scn* data_section = get_data_section_for_variable_address(addr);
	if (!data_section)
	  // It's likely that this address doesn't come from any
	  // data section.
	  return addr;

	GElf_Shdr sheader_mem;
	GElf_Shdr* data_sheader = gelf_getshdr(data_section, &sheader_mem);
	ABG_ASSERT(data_sheader);

	return addr - data_sheader->sh_addr;
      }
    else
      addr = maybe_adjust_address_for_exec_or_dyn(addr);

    return addr;
  }


  /// Get the address of the function.
  ///
  /// The address of the function is considered to be the value of the
  /// DW_AT_low_pc attribute, possibly adjusted (in relocatable files
  /// only) to not point to an absolute address anymore, but rather to
  /// the address of the function inside the .text segment.
  ///
  /// @param function_die the die of the function to consider.
  ///
  /// @param address the resulting address iff the function returns
  /// true.
  ///
  /// @return true if the function address was found.
  bool
  get_function_address(Dwarf_Die* function_die,
		       Dwarf_Addr& address) const
  {
    Dwarf_Addr low_pc = 0;
    if (!die_address_attribute(function_die, DW_AT_low_pc, low_pc))
      return false;

    low_pc = maybe_adjust_fn_sym_address(low_pc);
    address = low_pc;
    return true;
  }

  /// Get the address of the global variable.
  ///
  /// The address of the global variable is considered to be the value
  /// of the DW_AT_location attribute, possibly adjusted (in
  /// relocatable files only) to not point to an absolute address
  /// anymore, but rather to the address of the global variable inside
  /// the data segment.
  ///
  /// @param variable_die the die of the function to consider.
  ///
  /// @param address the resulting address iff this function returns
  /// true.
  ///
  /// @return true if the variable address was found.
  bool
  get_variable_address(Dwarf_Die*	variable_die,
		       Dwarf_Addr&	address) const
  {
    bool is_tls_address = false;
    if (!die_location_address(variable_die, address, is_tls_address))
      return false;
    if (!is_tls_address)
      address = maybe_adjust_var_sym_address(address);
    return true;
  }

  /// Tests if a suppression specification can match ABI artifacts
  /// coming from the binary being analyzed.
  ///
  /// This tests if the suppression matches the soname of and binary
  /// name of the ELF binary being analyzed.
  ///
  /// @param s the suppression specification to consider.
  bool
  suppression_can_match(const suppr::suppression_base& s) const
  {
    if (s.priv_->matches_soname(dt_soname())
	&& s.priv_->matches_binary_name(elf_path()))
      return true;
    return false;
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its linkage
  /// name (symbol name).
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function linkage name
  ///
  /// @param fn_linkage_name the linkage name of the function of interest.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose linkage name is @p fn_linkage_name.
  bool
  suppression_matches_function_sym_name(const suppr::function_suppression_sptr& s,
					const string& fn_linkage_name) const
  {
    if (!s)
      return false;
    return suppression_matches_function_sym_name(*s,fn_linkage_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its linkage
  /// name (symbol name).
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function linkage name
  ///
  /// @param fn_linkage_name the linkage name of the function of interest.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose linkage name is @p fn_linkage_name.
  bool
  suppression_matches_function_sym_name(const suppr::function_suppression& s,
					const string& fn_linkage_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_function_sym_name(s, fn_linkage_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its name.
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function name.
  ///
  /// @param fn_name the name of the function of interest.  Note that
  /// this name must be *non* qualified.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose name is @p fn_name.
  bool
  suppression_matches_function_name(const suppr::function_suppression_sptr& s,
				    const string& fn_name) const
  {
    if (!s)
      return false;
    return suppression_matches_function_name(*s, fn_name);
  }

  /// Test whether if a given function suppression matches a function
  /// designated by a regular expression that describes its name.
  ///
  /// @param s the suppression specification to evaluate to see if it
  /// matches a given function name.
  ///
  /// @param fn_name the name of the function of interest.  Note that
  /// this name must be *non* qualified.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// function whose name is @p fn_name.
  bool
  suppression_matches_function_name(const suppr::function_suppression& s,
				    const string& fn_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_function_name(s, fn_name);
  }

  /// Test whether if a given variable suppression specification
  /// matches a variable denoted by its name.
  ///
  /// @param s the variable suppression specification to consider.
  ///
  /// @param var_name the name of the variable to consider.
  ///
  /// @return true iff the suppression specification @p s matches the
  /// variable whose name is @p var_name.
  bool
  suppression_matches_variable_name(const suppr::variable_suppression& s,
				    const string& var_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_variable_name(s, var_name);
  }

  /// Test whether if a given variable suppression specification
  /// matches a variable denoted by its linkage name.
  ///
  /// @param s the variable suppression specification to consider.
  ///
  /// @param var_linkage_name the linkage name of the variable to consider.
  ///
  /// @return true iff variable suppression specification @p s matches
  /// the variable denoted by linkage name @p var_linkage_name.
  bool
  suppression_matches_variable_sym_name(const suppr::variable_suppression& s,
					const string& var_linkage_name) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_variable_sym_name(s, var_linkage_name);
  }

  /// Test if a given type suppression specification matches a type
  /// designated by its name and location.
  ///
  /// @param s the suppression specification to consider.
  ///
  /// @param type_name the fully qualified type name to consider.
  ///
  /// @param type_location the type location to consider.
  ///
  /// @return true iff the type suppression specification matches a
  /// type of a given name and location.
  bool
  suppression_matches_type_name_or_location(const suppr::type_suppression& s,
					    const string& type_name,
					    const location& type_location) const
  {
    if (!suppression_can_match(s))
      return false;

    return suppr::suppression_matches_type_name_or_location(s, type_name,
							    type_location);
  }

  /// Test if a type suppression specification matches the name of a
  /// type within a given scope.
  ///
  /// @param s the type suppression specification to consider.
  ///
  /// @param type_scope the type scope to consider.
  ///
  /// @param type the type to consider.
  ///
  /// @return true iff the type suppression specification matches a
  /// the name of type @p type.
  bool
  suppression_matches_type_name(const suppr::type_suppression&	s,
				const scope_decl*		type_scope,
				const type_base_sptr&		type) const
  {
    if (!suppression_can_match(s))
      return false;
    return suppr::suppression_matches_type_name(s, type_scope, type);
  }

  /// Getter of the exported decls builder object.
  ///
  /// @return the exported decls builder.
  corpus::exported_decls_builder*
  exported_decls_builder()
  {return exported_decls_builder_;}

  /// Setter of the exported decls builder object.
  ///
  /// Note that this @ref read_context is not responsible for the live
  /// time of the exported_decls_builder object.  The corpus is.
  ///
  /// @param b the new builder.
  void
  exported_decls_builder(corpus::exported_decls_builder* b)
  {exported_decls_builder_ = b;}

  /// Getter of the "load_all_types" flag.  This flag tells if all the
  /// types (including those not reachable by public declarations) are
  /// to be read and represented in the final ABI corpus.
  ///
  /// @return the load_all_types flag.
  bool
  load_all_types() const
  {return options_.load_all_types;}

  /// Setter of the "load_all_types" flag.  This flag tells if all the
  /// types (including those not reachable by public declarations) are
  /// to be read and represented in the final ABI corpus.
  ///
  /// @param f the new load_all_types flag.
  void
  load_all_types(bool f)
  {options_.load_all_types = f;}

  bool
  load_in_linux_kernel_mode() const
  {return options_.load_in_linux_kernel_mode;}

  void
  load_in_linux_kernel_mode(bool f)
  {options_.load_in_linux_kernel_mode = f;}

  /// Guess if the current binary is a Linux Kernel or a Linux Kernel module.
  ///
  /// To guess that, the function looks for the presence of the
  /// special "__ksymtab_strings" section in the binary.
  ///
  bool
  is_linux_kernel_binary() const
  {return find_section(elf_handle(), "__ksymtab_strings", SHT_PROGBITS);}

  /// Getter of the "show_stats" flag.
  ///
  /// This flag tells if we should emit statistics about various
  /// internal stuff.
  ///
  /// @return the value of the flag.
  bool
  show_stats() const
  {return options_.show_stats;}

  /// Setter of the "show_stats" flag.
  ///
  /// This flag tells if we should emit statistics about various
  /// internal stuff.
  ///
  /// @param f the value of the flag.
  void
  show_stats(bool f)
  {options_.show_stats = f;}

  /// Getter of the "do_log" flag.
  ///
  /// This flag tells if we should log about various internal
  /// details.
  ///
  /// return the "do_log" flag.
  bool
  do_log() const
  {return options_.do_log;}

  /// Setter of the "do_log" flag.
  ///
  /// This flag tells if we should log about various internal details.
  ///
  /// @param f the new value of the flag.
  void
  do_log(bool f)
  {options_.do_log = f;}

  /// If a given function decl is suitable for the set of exported
  /// functions of the current corpus, this function adds it to that
  /// set.
  ///
  /// @param fn the function to consider for inclusion into the set of
  /// exported functions of the current corpus.
  void
  maybe_add_fn_to_exported_decls(function_decl* fn)
  {
    if (fn)
      if (corpus::exported_decls_builder* b = exported_decls_builder())
	b->maybe_add_fn_to_exported_fns(fn);
  }

  /// If a given variable decl is suitable for the set of exported
  /// variables of the current corpus, this variable adds it to that
  /// set.
  ///
  /// @param fn the variable to consider for inclusion into the set of
  /// exported variables of the current corpus.
  void
  maybe_add_var_to_exported_decls(var_decl* var)
  {
    if (var)
      if (corpus::exported_decls_builder* b = exported_decls_builder())
	b->maybe_add_var_to_exported_vars(var);
  }

  /// Walk the DIEs under a given die and for each child, populate the
  /// die -> parent map to record the child -> parent relationship
  /// that
  /// exists between the child and the given die.
  ///
  /// The function also builds the vector of places where units are
  /// imported.
  ///
  /// This is done recursively as for each child DIE, this function
  /// walks its children as well.
  ///
  /// @param die the DIE whose children to walk recursively.
  ///
  /// @param source where the DIE @p die comes from.
  ///
  /// @param imported_units a vector containing all the offsets of the
  /// points where unit have been imported, under @p die.
  void
  build_die_parent_relations_under(Dwarf_Die*			die,
				   die_source			source,
				   imported_unit_points_type &	imported_units)
  {
    if (!die)
      return;

    offset_offset_map_type& parent_of = die_parent_map(source);

    Dwarf_Die child;
    if (dwarf_child(die, &child) != 0)
      return;

    do
      {
	parent_of[dwarf_dieoffset(&child)] = dwarf_dieoffset(die);
	if (dwarf_tag(&child) == DW_TAG_imported_unit)
	  {
	    Dwarf_Die imported_unit;
	    if (die_die_attribute(&child, DW_AT_import, imported_unit))
	      {
		die_source imported_unit_die_source = NO_DEBUG_INFO_DIE_SOURCE;
		ABG_ASSERT(get_die_source(imported_unit, imported_unit_die_source));
		imported_units.push_back
		  (imported_unit_point(dwarf_dieoffset(&child),
				       imported_unit,
				       imported_unit_die_source));
	      }
	  }
	build_die_parent_relations_under(&child, source, imported_units);
      }
    while (dwarf_siblingof(&child, &child) == 0);

  }

  /// Determine if we do have to build a DIE -> parent map, depending
  /// on a given language.
  ///
  /// Some languages like C++, Ada etc, do have the concept of
  /// namespace and yet, the DIE data structure doesn't provide us
  /// with a way to get the parent namespace of a given DIE.  So for
  /// those languages, we need to build a DIE -> parent map so that we
  /// can get the namespace DIE (or more generally the scope DIE) of a given
  /// DIE as we need it.
  ///
  /// But then some more basic languages like C or assembly don't have
  /// that need.
  ///
  /// This function, depending on the language, tells us if we need to
  /// build the DIE -> parent map or not.
  ///
  /// @param lang the language to consider.
  ///
  /// @return true iff we need to build the DIE -> parent map for this
  /// language.
  bool
  do_we_build_die_parent_maps(translation_unit::language lang)
  {
    if (is_c_language(lang))
      return false;

    switch (lang)
      {
      case translation_unit::LANG_UNKNOWN:
#ifdef HAVE_DW_LANG_Mips_Assembler_enumerator
      case translation_unit::LANG_Mips_Assembler:
#endif
	return false;
      default:
	break;
      }
    return true;
  }

  /// Walk all the DIEs accessible in the debug info (and in the
  /// alternate debug info as well) and build maps representing the
  /// relationship DIE -> parent.  That is, make it so that we can get
  /// the parent for a given DIE.
  ///
  /// Note that the goal of this map is to be able to get the parent
  /// of a given DIE. This is to mainly to handle namespaces.  For instance,
  /// when we get a DIE of a type, and we want to build an internal
  /// representation for it, we need to get its fully qualified name.
  /// For that, we need to know what is the parent DIE of that type
  /// DIE, so that we can know what the namespace of that type is.
  ///
  /// Note that as the C language doesn't have namespaces (all types
  /// are defined in the same global namespace), this function doesn't
  /// build the DIE -> parent map if the current translation unit
  /// comes from C.  This saves time on big C ELF files with a lot of
  /// DIEs.
  void
  build_die_parent_maps()
  {
    bool we_do_have_to_build_die_parent_map = false;
    uint8_t address_size = 0;
    size_t header_size = 0;
    // Get the DIE of the current translation unit, look at it to get
    // its language. If that language is in C, then all types are in
    // the global namespace so we don't need to build the DIE ->
    // parent map.  So we dont build it in that case.
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL, NULL, NULL) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;
	if (!dwarf_offdie(dwarf(), die_offset, &cu))
	  continue;

	uint64_t l = 0;
	die_unsigned_constant_attribute(&cu, DW_AT_language, l);
	translation_unit::language lang = dwarf_language_to_tu_language(l);
	if (do_we_build_die_parent_maps(lang))
	  we_do_have_to_build_die_parent_map = true;
      }

    if (!we_do_have_to_build_die_parent_map)
      return;

    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_info section in the alternate debug info file.
    die_source source = ALT_DEBUG_INFO_DIE_SOURCE;
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(alt_dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL, NULL, NULL) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;
	if (!dwarf_offdie(alt_dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);

	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }

    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_info section of the main debug info file.
    source = PRIMARY_DEBUG_INFO_DIE_SOURCE;
    address_size = 0;
    header_size = 0;
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL, NULL, NULL) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;
	if (!dwarf_offdie(dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);
	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }

    // Build the DIE -> parent relation for DIEs coming from the
    // .debug_types section.
    source = TYPE_UNIT_DIE_SOURCE;
    address_size = 0;
    header_size = 0;
    uint64_t type_signature = 0;
    Dwarf_Off type_offset;
    for (Dwarf_Off offset = 0, next_offset = 0;
	 (dwarf_next_unit(dwarf(), offset, &next_offset, &header_size,
			  NULL, NULL, &address_size, NULL,
			  &type_signature, &type_offset) == 0);
	 offset = next_offset)
      {
	Dwarf_Off die_offset = offset + header_size;
	Dwarf_Die cu;

	if (!dwarf_offdie_types(dwarf(), die_offset, &cu))
	  continue;
	cur_tu_die(&cu);
	imported_unit_points_type& imported_units =
	  tu_die_imported_unit_points_map(source)[die_offset] =
	  imported_unit_points_type();
	build_die_parent_relations_under(&cu, source, imported_units);
      }
  }
};// end class read_context.

static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       scope_decl*	scope,
		       bool		called_from_public_decl,
		       size_t		where_offset,
		       bool		is_required_decl_spec = false);

static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset);

static class_decl_sptr
add_or_update_class_type(read_context&	ctxt,
			 Dwarf_Die*	die,
			 scope_decl*	scope,
			 bool		is_struct,
			 class_decl_sptr  klass,
			 bool		called_from_public_decl,
			 size_t		where_offset);

static union_decl_sptr
add_or_update_union_type(read_context&	ctxt,
			 Dwarf_Die*	die,
			 scope_decl*	scope,
			 union_decl_sptr  union_type,
			 bool		called_from_public_decl,
			 size_t		where_offset);

static decl_base_sptr
build_ir_node_for_void_type(read_context& ctxt);

static function_decl_sptr
build_function_decl(read_context&	ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    function_decl_sptr	fn);

static bool
function_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *function_die);

static function_decl_sptr
build_or_get_fn_decl_if_not_suppressed(read_context&	ctxt,
				       scope_decl	*scope,
				       Dwarf_Die	*die,
				       size_t	where_offset,
				       function_decl_sptr f = function_decl_sptr());

static var_decl_sptr
build_var_decl(read_context&	ctxt,
	       Dwarf_Die	*die,
	       size_t		where_offset,
	       var_decl_sptr	result = var_decl_sptr());

static var_decl_sptr
build_or_get_var_decl_if_not_suppressed(read_context&	ctxt,
					scope_decl	*scope,
					Dwarf_Die	*die,
					size_t	where_offset,
					var_decl_sptr	res = var_decl_sptr(),
					bool is_required_decl_spec = false);
static bool
variable_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *variable_die,
		       bool is_required_decl_spec = false);

static void
finish_member_function_reading(Dwarf_Die*		 die,
			       const function_decl_sptr& f,
			       const class_or_union_sptr& klass,
			       read_context&		 ctxt);

/// Setter of the debug info root path for a dwarf reader context.
///
/// @param ctxt the dwarf reader context to consider.
///
/// @param path the new debug info root path.  This must be a pointer to a
/// character string which life time should be greater than the life
/// time of the read context.
void
set_debug_info_root_path(read_context& ctxt, char** path)
{ctxt.offline_callbacks()->debuginfo_path = path;}

/// Setter of the debug info root path for a dwarf reader context.
///
/// @param ctxt the dwarf reader context to consider.
///
/// @return a pointer to the debug info root path.
///
/// time of the read context.
char**
get_debug_info_root_path(read_context& ctxt)
{return ctxt.offline_callbacks()->debuginfo_path;}

/// Getter of the "show_stats" flag.
///
/// This flag tells if we should emit statistics about various
/// internal stuff.
///
/// @param ctx the read context to consider for this flag.
///
/// @return the value of the flag.
bool
get_show_stats(read_context& ctxt)
{return ctxt.show_stats();}

/// Setter of the "show_stats" flag.
///
/// This flag tells if we should emit statistics about various
/// internal stuff.
///
/// @param ctxt the read context to consider for this flag.
///
/// @param f the value of the flag.
void
set_show_stats(read_context& ctxt, bool f)
{ctxt.show_stats(f);}

/// Setter of the "do_log" flag.
///
/// This flag tells if we should emit verbose logs for various
/// internal things related to DWARF reading.
///
/// @param ctxt the DWARF reading context to consider.
///
/// @param f the new value of the flag.
void
set_do_log(read_context& ctxt, bool f)
{ctxt.do_log(f);}

/// Setter of the "set_ignore_symbol_table" flag.
///
/// This flag tells if we should load information about ELF symbol
/// tables.  Not loading the symbol tables is a speed optimization
/// that is done when the set of symbols we care about is provided
/// off-hand.  This is the case when we are supposed to analyze a
/// Linux kernel binary.  In that case, because we have the white list
/// of functions/variable symbols we care about, we don't need to
/// analyze the symbol table; things are thus faster in that case.
///
/// By default, the symbol table is analyzed so this boolean is set to
/// false.
///
/// @param ctxt the read context to consider.
///
/// @param f the new value of the flag.
void
set_ignore_symbol_table(read_context &ctxt, bool f)
{ctxt.options_.ignore_symbol_table = f;}

/// Getter of the "set_ignore_symbol_table" flag.
///
/// This flag tells if we should load information about ELF symbol
/// tables.  Not loading the symbol tables is a speed optimization
/// that is done when the set of symbols we care about is provided
/// off-hand.  This is the case when we are supposed to analyze a
/// Linux kernel binary.  In that case, because we have the white list
/// of functions/variable symbols we care about, we don't need to
/// analyze the symbol table; things are thus faster in that case.
///
/// By default, the symbol table is analyzed so this boolean is set to
/// false.
///
/// @param ctxt the read context to consider.
///
/// @return the value of the flag.
bool
get_ignore_symbol_table(const read_context& ctxt)
{return ctxt.options_.ignore_symbol_table;}

/// Test if a given DIE is anonymous
///
/// @param die the DIE to consider.
///
/// @return true iff @p die is anonymous.
static bool
die_is_anonymous(Dwarf_Die* die)
{
  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, DW_AT_name, &attr))
    return true;
  return false;
}

/// Get the value of an attribute that is supposed to be a string, or
/// an empty string if the attribute could not be found.
///
/// @param die the DIE to get the attribute value from.
///
/// @param attr_name the attribute name.  Must come from dwarf.h and
/// be an enumerator representing an attribute like, e.g, DW_AT_name.
///
/// @return the string representing the value of the attribute, or an
/// empty string if no string attribute could be found.
static string
die_string_attribute(Dwarf_Die* die, unsigned attr_name)
{
  if (!die)
    return "";

  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return "";

  const char* str = dwarf_formstring(&attr);
  return str ? str : "";
}

/// Get the value of an attribute that is supposed to be an unsigned
/// constant.
///
/// @param die the DIE to read the information from.
///
/// @param attr_name the DW_AT_* name of the attribute.  Must come
/// from dwarf.h and be an enumerator representing an attribute like,
/// e.g, DW_AT_decl_line.
///
///@param cst the output parameter that is set to the value of the
/// attribute @p attr_name.  This parameter is set iff the function
/// return true.
///
/// @return true if there was an attribute of the name @p attr_name
/// and with a value that is a constant, false otherwise.
static bool
die_unsigned_constant_attribute(Dwarf_Die*	die,
				unsigned	attr_name,
				uint64_t&	cst)
{
  if (!die)
    return false;

  Dwarf_Attribute attr;
  Dwarf_Word result = 0;
  if (!dwarf_attr_integrate(die, attr_name, &attr)
      || dwarf_formudata(&attr, &result))
    return false;

  cst = result;
  return true;
}

/// Read a signed constant value from a given attribute.
///
/// The signed constant expected must be of form DW_FORM_sdata.
///
/// @param die the DIE to get the attribute from.
///
/// @param attr_name the attribute name.
///
/// @param cst the resulting signed constant read.
///
/// @return true iff a signed constant attribute of the name @p
/// attr_name was found on the DIE @p die.
static bool
die_signed_constant_attribute(Dwarf_Die *die,
			      unsigned	attr_name,
			      int64_t&	cst)
{
  if (!die)
    return false;

  Dwarf_Attribute attr;
  Dwarf_Sword result = 0;
  if (!dwarf_attr_integrate(die, attr_name, &attr)
      || dwarf_formsdata(&attr, &result))
    return false;

  cst = result;
  return true;
}

/// Read the value of a constant attribute that is either signed or
/// unsigned into a array_type_def::subrange_type::bound_value value.
///
/// The bound_value instance will capture the actual signedness of the
/// read attribute.
///
/// @param die the DIE from which to read the value of the attribute.
///
/// @param attr_name the attribute name to consider.
///
/// @param value the resulting value read from attribute @p attr_name
/// on DIE @p die.
///
/// @return true iff DIE @p die has an attribute named @p attr_name
/// with a constant value.
static bool
die_constant_attribute(Dwarf_Die *die,
		       unsigned attr_name,
		       array_type_def::subrange_type::bound_value &value)
{
  if (die_attribute_is_unsigned(die, attr_name)
      || die_attribute_has_no_signedness(die, attr_name))
    {
      uint64_t l = 0;
      if (!die_unsigned_constant_attribute(die, attr_name, l))
	return false;
      value.set_unsigned(l);
    }
  else
    {
      int64_t l = 0;
      if (!die_signed_constant_attribute(die, attr_name, l))
	return false;
      value.set_signed(l);
    }
  return true;
}

/// Test if a given attribute on a DIE has a particular form.
///
/// @param die the DIE to consider.
///
/// @param attr_name the attribute name to consider on DIE @p die.
///
/// @param attr_form the attribute form that we expect attribute @p
/// attr_name has on DIE @p die.
///
/// @return true iff the attribute named @p attr_name on DIE @p die
/// has the form @p attr_form.
static bool
die_attribute_has_form(Dwarf_Die	*die,
		       unsigned	attr_name,
		       unsigned int	attr_form)
{
  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return false;

  return dwarf_hasform(&attr, attr_form);
}

/// Test if a given DIE attribute is signed.
///
/// @param die the DIE to consider.
///
/// @param attr_name the attribute name to consider.
///
/// @return true iff the attribute named @p attr_name on DIE @p die is
/// signed.
static bool
die_attribute_is_signed(Dwarf_Die* die, unsigned attr_name)
{
  if (die_attribute_has_form(die, attr_name, DW_FORM_sdata))
    return true;
  return false;
}

/// Test if a given DIE attribute is unsigned.
///
/// @param die the DIE to consider.
///
/// @param attr_name the attribute name to consider.
///
/// @return true iff the attribute named @p attr_name on DIE @p die is
/// unsigned.
static bool
die_attribute_is_unsigned(Dwarf_Die* die, unsigned attr_name)
{
  if (die_attribute_has_form(die, attr_name, DW_FORM_udata))
    return true;
  return false;
}

/// Test if a given DIE attribute is neither explicitely signed nor
/// unsigned.  Usually this is the case for attribute of the form
/// DW_FORM_data*.
///
/// @param die the DIE to consider.
///
/// @param attr_name the name of the attribute to consider.
///
/// @return true iff the attribute named @p attr_name of DIE @p die is
/// neither specifically signed nor unsigned.
static bool
die_attribute_has_no_signedness(Dwarf_Die *die, unsigned attr_name)
{
  return (!die_attribute_is_unsigned(die, attr_name)
	  && !die_attribute_is_signed(die, attr_name));
}

/// Get the value of a DIE attribute; that value is meant to be a
/// flag.
///
/// @param die the DIE to get the attribute from.
///
/// @param attr_name the DW_AT_* name of the attribute.  Must come
/// from dwarf.h and be an enumerator representing an attribute like,
/// e.g, DW_AT_external.
///
/// @param flag the output parameter to store the flag value into.
/// This is set iff the function returns true.
///
/// @return true if the DIE has a flag attribute named @p attr_name,
/// false otherwise.
static bool
die_flag_attribute(Dwarf_Die* die, unsigned attr_name, bool& flag)
{
  Dwarf_Attribute attr;
  bool f = false;
  if (!dwarf_attr_integrate(die, attr_name, &attr)
      || dwarf_formflag(&attr, &f))
    return false;

  flag = f;
  return true;
}

/// Get the mangled name from a given DIE.
///
/// @param die the DIE to read the mangled name from.
///
/// @return the mangled name if it's present in the DIE, or just an
/// empty string if it's not.
static string
die_linkage_name(Dwarf_Die* die)
{
  if (!die)
    return "";

  string linkage_name = die_string_attribute(die, DW_AT_linkage_name);
  if (linkage_name.empty())
    linkage_name = die_string_attribute(die, DW_AT_MIPS_linkage_name);
  return linkage_name;
}

/// Get the file path that is the value of the DW_AT_decl_file
/// attribute on a given DIE, if the DIE is a decl DIE having that
/// attribute.
///
/// @param die the DIE to consider.
///
/// @return a string containing the file path that is the logical
/// value of the DW_AT_decl_file attribute.  If the DIE @p die
/// doesn't have a DW_AT_decl_file attribute, then the return value is
/// just an empty string.
static string
die_decl_file_attribute(Dwarf_Die* die)
{
  if (!die)
    return "";

  const char* str = dwarf_decl_file(die);

  return str ? str : "";
}

/// Get the value of an attribute which value is supposed to be a
/// reference to a DIE.
///
/// @param die the DIE to read the value from.
///
/// @param die_is_in_alt_di true if @p die comes from alternate debug
/// info sections.
///
/// @param attr_name the DW_AT_* attribute name to read.
///
/// @param result the DIE resulting from reading the attribute value.
/// This is set iff the function returns true.
///
/// @param look_thru_abstract_origin if yes, the function looks
/// through the possible DW_AT_abstract_origin attribute all the way
/// down to the initial DIE that is cloned and look on that DIE to see
/// if it has the @p attr_name attribute.
///
/// @return true if the DIE @p die contains an attribute named @p
/// attr_name that is a DIE reference, false otherwise.
static bool
die_die_attribute(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Die& result,
		  bool look_thru_abstract_origin)
{
  Dwarf_Attribute attr;
  if (look_thru_abstract_origin)
    {
      if (!dwarf_attr_integrate(die, attr_name, &attr))
	return false;
    }
  else
    {
      if (!dwarf_attr(die, attr_name, &attr))
	return false;
    }
  bool r = dwarf_formref_die(&attr, &result);
  return r;
}

/// Read and return a DW_FORM_addr attribute from a given DIE.
///
/// @param die the DIE to consider.
///
/// @param attr_name the name of the DW_FORM_addr attribute to read
/// the value from.
///
/// @param the resulting address.
///
/// @return true iff the attribute could be read, was of the expected
/// DW_FORM_addr and could thus be translated into the @p result.
static bool
die_address_attribute(Dwarf_Die* die, unsigned attr_name, Dwarf_Addr& result)
{
  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return false;
  return dwarf_formaddr(&attr, &result) == 0;
}

/// Returns the source location associated with a decl DIE.
///
/// @param ctxt the @ref read_context to use.
///
/// @param die the DIE the read the source location from.
///
/// @return the location associated with @p die.
static location
die_location(const read_context& ctxt, Dwarf_Die* die)
{
  if (!die)
    return location();

  string file = die_decl_file_attribute(die);
  uint64_t line = 0;
  die_unsigned_constant_attribute(die, DW_AT_decl_line, line);

  if (!file.empty() && line != 0)
    {
      translation_unit_sptr tu = ctxt.cur_transl_unit();
      location l = tu->get_loc_mgr().create_new_location(file, line, 1);
      return l;
    }
  return location();
}

/// Return a copy of the name of a DIE.
///
/// @param die the DIE to consider.
///
/// @return a copy of the name of the DIE.
static string
die_name(Dwarf_Die* die)
{
  string name = die_string_attribute(die, DW_AT_name);
  return name;
}

/// Return the location, the name and the mangled name of a given DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read location and names from.
///
/// @param loc the location output parameter to set.
///
/// @param name the name output parameter to set.
///
/// @param linkage_name the linkage_name output parameter to set.
static void
die_loc_and_name(const read_context&	ctxt,
		 Dwarf_Die*		die,
		 location&		loc,
		 string&		name,
		 string&		linkage_name)
{
  loc = die_location(ctxt, die);
  name = die_name(die);
  linkage_name = die_linkage_name(die);
}

/// Get the size of a (type) DIE as the value for the parameter
/// DW_AT_byte_size or DW_AT_bit_size.
///
/// @param die the DIE to read the information from.
///
/// @param size the resulting size in bits.  This is set iff the
/// function return true.
///
/// @return true if the size attribute was found.
static bool
die_size_in_bits(Dwarf_Die* die, uint64_t& size)
{
  if (!die)
    return false;

  uint64_t byte_size = 0, bit_size = 0;

  if (!die_unsigned_constant_attribute(die, DW_AT_byte_size, byte_size))
    {
      if (!die_unsigned_constant_attribute(die, DW_AT_bit_size, bit_size))
	return false;
    }
  else
    bit_size = byte_size * 8;

  size = bit_size;

  return true;
}

/// Get the access specifier (from the DW_AT_accessibility attribute
/// value) of a given DIE.
///
/// @param die the DIE to consider.
///
/// @param access the resulting access.  This is set iff the function
/// returns true.
///
/// @return bool if the DIE contains the DW_AT_accessibility die.
static bool
die_access_specifier(Dwarf_Die * die, access_specifier& access)
{
  if (!die)
    return false;

  uint64_t a = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_accessibility, a))
    return false;

  access_specifier result = private_access;

  switch (a)
    {
    case private_access:
      result = private_access;
      break;

    case protected_access:
      result = protected_access;
      break;

    case public_access:
      result = public_access;
      break;

    default:
      break;
    }

  access = result;
  return true;
}

/// Test whether a given DIE represents a decl that is public.  That
/// is, one with the DW_AT_external attribute set.
///
/// @param die the DIE to consider for testing.
///
/// @return true if a DW_AT_external attribute is present and its
/// value is set to the true; return false otherwise.
static bool
die_is_public_decl(Dwarf_Die* die)
{
  bool is_public = false;
  die_flag_attribute(die, DW_AT_external, is_public);
  return is_public;
}

/// Test whether a given DIE represents a declaration-only DIE.
///
/// That is, if the DIE has the DW_AT_declaration flag set.
///
/// @param die the DIE to consider.
//
/// @return true if a DW_AT_declaration is present, false otherwise.
static bool
die_is_declaration_only(Dwarf_Die* die)
{
  bool is_declaration_only = false;
  die_flag_attribute(die, DW_AT_declaration, is_declaration_only);
  return is_declaration_only;
}

/// Tests whether a given DIE is artificial.
///
/// @param die the test to test for.
///
/// @return true if the DIE is artificial, false otherwise.
static bool
die_is_artificial(Dwarf_Die* die)
{
  bool is_artificial;
  return die_flag_attribute(die, DW_AT_artificial, is_artificial);
}

///@return true if a tag represents a type, false otherwise.
///
///@param tag the tag to consider.
static bool
is_type_tag(unsigned tag)
{
  bool result = false;

  switch (tag)
    {
    case DW_TAG_array_type:
    case DW_TAG_class_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_string_type:
    case DW_TAG_structure_type:
    case DW_TAG_subroutine_type:
    case DW_TAG_typedef:
    case DW_TAG_union_type:
    case DW_TAG_ptr_to_member_type:
    case DW_TAG_set_type:
    case DW_TAG_subrange_type:
    case DW_TAG_base_type:
    case DW_TAG_const_type:
    case DW_TAG_file_type:
    case DW_TAG_packed_type:
    case DW_TAG_thrown_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_interface_type:
    case DW_TAG_unspecified_type:
    case DW_TAG_shared_type:
    case DW_TAG_rvalue_reference_type:
      result = true;
      break;

    default:
      result = false;
      break;
    }

  return result;
}

/// Test if a given DIE is a type to be canonicalized.  note that a
/// function DIE (DW_TAG_subprogram) is considered to be a
/// canonicalize-able type too because we can consider that DIE as
/// being the type of the function, as well as the function decl
/// itself.
///
/// @param tag the tag of the DIE to consider.
///
/// @return true iff the DIE of tag @p tag is a canonicalize-able DIE.
static bool
is_canonicalizeable_type_tag(unsigned tag)
{
  bool result = false;

  switch (tag)
    {
    case DW_TAG_array_type:
    case DW_TAG_class_type:
    case DW_TAG_enumeration_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_structure_type:
    case DW_TAG_subroutine_type:
    case DW_TAG_subprogram:
    case DW_TAG_typedef:
    case DW_TAG_union_type:
    case DW_TAG_base_type:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_rvalue_reference_type:
      result = true;
      break;

    default:
      result = false;
      break;
    }

  return result;
}

/// Test if a DIE tag represents a declaration.
///
/// @param tag the DWARF tag to consider.
///
/// @return true iff @p tag is for a declaration.
static bool
is_decl_tag(unsigned tag)
{
  switch (tag)
    {
    case DW_TAG_formal_parameter:
    case DW_TAG_imported_declaration:
    case DW_TAG_member:
    case DW_TAG_unspecified_parameters:
    case DW_TAG_subprogram:
    case DW_TAG_variable:
    case DW_TAG_namespace:
    case DW_TAG_GNU_template_template_param:
    case DW_TAG_GNU_template_parameter_pack:
    case DW_TAG_GNU_formal_parameter_pack:
      return true;
    }
  return false;
}

/// Test if a DIE represents a type DIE.
///
/// @param die the DIE to consider.
///
/// @return true if @p die represents a type, false otherwise.
static bool
die_is_type(Dwarf_Die* die)
{
  if (!die)
    return false;
  return is_type_tag(dwarf_tag(die));
}

/// Test if a DIE represents a declaration.
///
/// @param die the DIE to consider.
///
/// @return true if @p die represents a decl, false otherwise.
static bool
die_is_decl(Dwarf_Die* die)
{
  if (!die)
    return false;
  return is_decl_tag(dwarf_tag(die));
}

/// Test if a DIE represents a namespace.
///
/// @param die the DIE to consider.
///
/// @return true if @p die represents a namespace, false otherwise.
static bool
die_is_namespace(Dwarf_Die* die)
{
  if (!die)
    return false;
  return (dwarf_tag(die) == DW_TAG_namespace);
}

/// Test if a DIE has tag DW_TAG_unspecified_type.
///
/// @param die the DIE to consider.
///
/// @return true if @p die has tag DW_TAG_unspecified_type.
static bool
die_is_unspecified(Dwarf_Die* die)
{
  if (!die)
    return false;
  return (dwarf_tag(die) == DW_TAG_unspecified_type);
}

/// Test if a DIE represents a void type.
///
/// @param die the DIE to consider.
///
/// @return true if @p die represents a void type, false otherwise.
static bool
die_is_void_type(Dwarf_Die* die)
{
  if (!die || dwarf_tag(die) != DW_TAG_base_type)
    return false;

  string name = die_name(die);
  if (name == "void")
    return true;

  return false;
}

/// Test if a DIE represents a pointer type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents a pointer type.
static bool
die_is_pointer_type(Dwarf_Die* die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);
  if (tag == DW_TAG_pointer_type)
    return true;

  return false;
}

/// Test if a DIE is for a pointer, reference or qualified type to
/// anonymous class or struct.
///
/// @param die the DIE to consider.
///
/// @return true iff @p is for a pointer, reference or qualified type
/// to anonymous class or struct.
static bool
pointer_or_qual_die_of_anonymous_class_type(Dwarf_Die* die)
{
  if (!die_is_pointer_or_reference_type(die)
      && !die_is_qualified_type(die))
    return false;

  Dwarf_Die underlying_type_die;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    return false;

  if (!die_is_class_type(&underlying_type_die))
    return false;

  string name = die_name(&underlying_type_die);

  return name.empty();
}

/// Test if a DIE represents a reference type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents a reference type.
static bool
die_is_reference_type(Dwarf_Die* die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);
  if (tag == DW_TAG_reference_type || tag == DW_TAG_rvalue_reference_type)
    return true;

  return false;
}

/// Test if a DIE represents an array type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents an array type.
static bool
die_is_array_type(Dwarf_Die* die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);
  if (tag == DW_TAG_array_type)
    return true;

  return false;
}

/// Test if a DIE represents a pointer, reference or array type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents a pointer or reference type.
static bool
die_is_pointer_or_reference_type(Dwarf_Die* die)
{return (die_is_pointer_type(die)
	 || die_is_reference_type(die)
	 || die_is_array_type(die));}

/// Test if a DIE represents a pointer, a reference or a typedef type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents a pointer, a reference or a
/// typedef type.
static bool
die_is_pointer_reference_or_typedef_type(Dwarf_Die* die)
{return (die_is_pointer_or_reference_type(die)
	 || dwarf_tag(die) == DW_TAG_typedef);}

/// Test if a DIE represents a class type.
///
/// @param die the die to consider.
///
/// @return true iff @p die represents a class type.
static bool
die_is_class_type(Dwarf_Die* die)
{
  int tag = dwarf_tag(die);

  if (tag == DW_TAG_class_type || tag == DW_TAG_structure_type)
    return true;

  return false;
}

/// Test if a DIE is for a qualified type.
///
/// @param die the DIE to consider.
///
/// @return true iff @p die is for a qualified type.
static bool
die_is_qualified_type(Dwarf_Die* die)
{
    int tag = dwarf_tag(die);
    if (tag == DW_TAG_const_type
	|| tag == DW_TAG_volatile_type
	|| tag == DW_TAG_restrict_type)
      return true;

    return false;
}

/// Test if a DIE for a function pointer or member function has an
/// DW_AT_object_pointer attribute.
///
/// @param die the DIE to consider.
///
/// @param object_pointer out parameter.  It's set to the DIE for the
/// object pointer iff the function returns true.
///
/// @return true iff the DIE @p die has an object pointer.  In that
/// case, the parameter @p object_pointer is set to the DIE of that
/// object pointer.
static bool
die_has_object_pointer(Dwarf_Die* die, Dwarf_Die& object_pointer)
{
  if (!die)
    return false;

  if (die_die_attribute(die, DW_AT_object_pointer, object_pointer))
    return true;

  return false;
}

/// When given the object pointer DIE of a function type or member
/// function DIE, this function returns the "this" pointer that points
/// to the associated class.
///
/// @param die the DIE of the object pointer of the function or member
/// function to consider.
///
/// @param this_pointer_die out parameter.  This is set to the DIE of
/// the "this" pointer iff the function returns true.
///
/// @return true iff the function found the "this" pointer from the
/// object pointer DIE @p die.  In that case, the parameter @p
/// this_pointer_die is set to the DIE of that "this" pointer.
static bool
die_this_pointer_from_object_pointer(Dwarf_Die* die,
				     Dwarf_Die& this_pointer_die)
{
  ABG_ASSERT(die);
  ABG_ASSERT(dwarf_tag(die) == DW_TAG_formal_parameter);

  if (die_die_attribute(die, DW_AT_type, this_pointer_die))
    return true;

  return false;
}

/// Test if a given "this" pointer that points to a particular class
/// type is for a const class or not.  If it's for a const class, then
/// it means the function type or the member function associated to
/// that "this" pointer is const.
///
/// @param die the DIE of the "this" pointer to consider.
///
/// @return true iff @p die points to a const class type.
static bool
die_this_pointer_is_const(Dwarf_Die* die)
{
  ABG_ASSERT(die);

  if (dwarf_tag(die) == DW_TAG_pointer_type)
    {
      Dwarf_Die pointed_to_type_die;
      if (die_die_attribute(die, DW_AT_type, pointed_to_type_die))
	if (dwarf_tag(&pointed_to_type_die) == DW_TAG_const_type)
	  return true;
    }

  return false;
}

/// Test if an object pointer (referred-to via a DW_AT_object_pointer
/// attribute) points to a const implicit class and so is for a const
/// method or or a const member function type.
///
/// @param die the DIE of the object pointer to consider.
///
/// @return true iff the object pointer represented by @p die is for a
/// a const method or const member function type.
static bool
die_object_pointer_is_for_const_method(Dwarf_Die* die)
{
  ABG_ASSERT(die);
  ABG_ASSERT(dwarf_tag(die) == DW_TAG_formal_parameter);

  Dwarf_Die this_pointer_die;
  if (die_this_pointer_from_object_pointer(die, this_pointer_die))
    if (die_this_pointer_is_const(&this_pointer_die))
      return true;

  return false;
}

/// Test if a DIE represents an entity that is at class scope.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @param class_scope_die out parameter.  Set to the DIE of the
/// containing class iff @p die happens to be at class scope; that is,
/// iff the function returns true.
///
/// @return true iff @p die is at class scope.  In that case, @p
/// class_scope_die is set to the DIE of the class that contains @p
/// die.
static bool
die_is_at_class_scope(const read_context& ctxt,
		      Dwarf_Die* die,
		      size_t where_offset,
		      Dwarf_Die& class_scope_die)
{
  if (!get_scope_die(ctxt, die, where_offset, class_scope_die))
    return false;

  int tag = dwarf_tag(&class_scope_die);

  return (tag == DW_TAG_structure_type
	  || tag == DW_TAG_class_type
	  || tag == DW_TAG_union_type);
}

/// Return the leaf object under a pointer, reference or qualified
/// type DIE.
///
/// @param die the DIE of the type to consider.
///
/// @param peeled_die out parameter.  Set to the DIE of the leaf
/// object iff the function actually peeled anything.
///
/// @return true upon successful completion.
static bool
die_peel_qual_ptr(Dwarf_Die *die, Dwarf_Die& peeled_die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);

  if (tag == DW_TAG_const_type
      || tag == DW_TAG_volatile_type
      || tag == DW_TAG_restrict_type
      || tag == DW_TAG_pointer_type
      || tag == DW_TAG_reference_type
      || tag == DW_TAG_rvalue_reference_type)
    {
      if (!die_die_attribute(die, DW_AT_type, peeled_die))
	return false;
    }
  else
    return false;

  while (tag == DW_TAG_const_type
	 || tag == DW_TAG_volatile_type
	 || tag == DW_TAG_restrict_type
	 || tag == DW_TAG_pointer_type
	 || tag == DW_TAG_reference_type
	 || tag == DW_TAG_rvalue_reference_type)
    {
      if (!die_die_attribute(&peeled_die, DW_AT_type, peeled_die))
	break;
      tag = dwarf_tag(&peeled_die);
    }

  return true;
}

/// Return the leaf object under a typedef type DIE.
///
/// @param die the DIE of the type to consider.
///
/// @param peeled_die out parameter.  Set to the DIE of the leaf
/// object iff the function actually peeled anything.
///
/// @return true upon successful completion.
static bool
die_peel_typedef(Dwarf_Die *die, Dwarf_Die& peeled_die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);

  if (tag == DW_TAG_typedef)
    {
      if (!die_die_attribute(die, DW_AT_type, peeled_die))
	return false;
    }
  else
    return false;

  while (tag == DW_TAG_typedef)
    {
      if (!die_die_attribute(&peeled_die, DW_AT_type, peeled_die))
	break;
      tag = dwarf_tag(&peeled_die);
    }

  return true;

}

/// Return the leaf DIE under a pointer, a reference or a typedef DIE.
///
/// @param die the DIE to consider.
///
/// @param peeled_die the resulting peeled (or leaf) DIE.  This is set
/// iff the function returned true.
///
/// @return true iff the function could peel @p die.
static bool
die_peel_pointer_and_typedef(Dwarf_Die *die, Dwarf_Die& peeled_die)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);

  if (tag == DW_TAG_pointer_type
      || tag == DW_TAG_reference_type
      || tag == DW_TAG_rvalue_reference_type
      || tag == DW_TAG_typedef)
    {
      if (!die_die_attribute(die, DW_AT_type, peeled_die))
	return false;
    }
  else
    return false;

  while (tag == DW_TAG_pointer_type
	 || tag == DW_TAG_reference_type
	 || tag == DW_TAG_rvalue_reference_type
	 || tag == DW_TAG_typedef)
    {
      if (!die_die_attribute(&peeled_die, DW_AT_type, peeled_die))
	break;
      tag = dwarf_tag(&peeled_die);
    }
  return true;
}

/// Test if a DIE for a function type represents a method type.
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param where_offset where we logically are in the stream of DIEs.
///
/// @param object_pointer_die out parameter.  This is set by the
/// function to the DIE that refers to the formal function parameter
/// which holds the implicit "this" pointer of the method.  That die
/// is called the object pointer DIE. This is set iff the function
///
/// @param class_die out parameter.  This is set by the function to
/// the DIE that represents the class of the method type.  This is set
/// iff the function returns true.
///
/// @param is_static out parameter.  This is set to true by the
/// function if @p die is a static method.  This is set iff the
/// function returns true.
///
/// @return true iff @p die is a DIE for a method type.
static bool
die_function_type_is_method_type(const read_context& ctxt,
				 Dwarf_Die *die,
				 size_t where_offset,
				 Dwarf_Die& object_pointer_die,
				 Dwarf_Die& class_die,
				 bool& is_static)
{
  if (!die)
    return false;

  int tag = dwarf_tag(die);
  ABG_ASSERT(tag == DW_TAG_subroutine_type || tag == DW_TAG_subprogram);

  bool has_object_pointer = false;
  is_static = false;
  if (tag == DW_TAG_subprogram)
    {
      Dwarf_Die spec_or_origin_die;
      if (die_die_attribute(die, DW_AT_specification,
			    spec_or_origin_die)
	  || die_die_attribute(die, DW_AT_abstract_origin,
			       spec_or_origin_die))
	{
	  if (die_has_object_pointer(&spec_or_origin_die,
				     object_pointer_die))
	    has_object_pointer = true;
	  else
	    {
	      if (die_is_at_class_scope(ctxt, &spec_or_origin_die,
					where_offset, class_die))
		is_static = true;
	      else
		return false;
	    }
	}
      else
	{
	  if (die_has_object_pointer(die, object_pointer_die))
	    has_object_pointer = true;
	  else
	    {
	      if (die_is_at_class_scope(ctxt, die, where_offset, class_die))
		is_static = true;
	      else
		return false;
	    }
	}
    }
  else
    {
      if (die_has_object_pointer(die, object_pointer_die))
	has_object_pointer = true;
      else
	return false;
    }

  if (!is_static)
    {
      ABG_ASSERT(has_object_pointer);
      // The object pointer die points to a DW_TAG_formal_parameter which
      // is the "this" parameter.  The type of the "this" parameter is a
      // pointer.  Let's get that pointer type.
      Dwarf_Die this_type_die;
      if (!die_die_attribute(&object_pointer_die, DW_AT_type, this_type_die))
	return false;

      // So the class type is the type pointed to by the type of the "this"
      // parameter.
      if (!die_peel_qual_ptr(&this_type_die, class_die))
	return false;

      // And make we return a class type, rather than a typedef to a
      // class.
      die_peel_typedef(&class_die, class_die);
    }

  return true;
}

enum virtuality
{
  VIRTUALITY_NOT_VIRTUAL,
  VIRTUALITY_VIRTUAL,
  VIRTUALITY_PURE_VIRTUAL
};

/// Get the virtual-ness of a given DIE, that is, the value of the
/// DW_AT_virtuality attribute.
///
/// @param die the DIE to read from.
///
/// @param virt the resulting virtuality attribute.  This is set iff
/// the function returns true.
///
/// @return true if the virtual-ness could be determined.
static bool
die_virtuality(Dwarf_Die* die, virtuality& virt)
{
  if (!die)
    return false;

  uint64_t v = 0;
  die_unsigned_constant_attribute(die, DW_AT_virtuality, v);

  if (v == DW_VIRTUALITY_virtual)
    virt = VIRTUALITY_VIRTUAL;
  else if (v == DW_VIRTUALITY_pure_virtual)
    virt = VIRTUALITY_PURE_VIRTUAL;
  else
    virt = VIRTUALITY_NOT_VIRTUAL;

  return true;
}

/// Test whether the DIE represent either a virtual base or function.
///
/// @param die the DIE to consider.
///
/// @return bool if the DIE represents a virtual base or function,
/// false othersise.
static bool
die_is_virtual(Dwarf_Die* die)
{
  virtuality v;
  if (!die_virtuality(die, v))
    return false;

  return v == VIRTUALITY_PURE_VIRTUAL || v == VIRTUALITY_VIRTUAL;
}

/// Test if the DIE represents an entity that was declared inlined.
///
/// @param die the DIE to test for.
///
/// @return true if the DIE represents an entity that was declared
/// inlined.
static bool
die_is_declared_inline(Dwarf_Die* die)
{
  uint64_t inline_value = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_inline, inline_value))
    return false;
  return inline_value == DW_INL_declared_inlined;
}

/// This function is a fast routine (optmization) to compare the values of
/// two string attributes of two DIEs.
///
/// @param l the first DIE to consider.
///
/// @param r the second DIE to consider.
///
/// @param attr_name the name of the attribute to compare, on the two
/// DIEs above.
///
/// @param result out parameter.  This is set to the result of the
/// comparison.  If the value of attribute @p attr_name on DIE @p l
/// equals the value of attribute @p attr_name on DIE @p r, then the
/// the argument of this parameter is set to true.  Otherwise, it's
/// set to false.  Note that the argument of this parameter is set iff
/// the function returned true.
///
/// @return true iff the comparison could be performed.  There are
/// cases in which the comparison cannot be performed.  For instance,
/// if one of the DIEs does not have the attribute @p attr_name.  In
/// any case, if this function returns true, then the parameter @p
/// result is set to the result of the comparison.
static bool
compare_dies_string_attribute_value(Dwarf_Die *l, Dwarf_Die *r,
				    unsigned attr_name,
				    bool &result)
{
  Dwarf_Attribute l_attr, r_attr;
  if (!dwarf_attr_integrate(l, attr_name, &l_attr)
      || !dwarf_attr_integrate(r, attr_name, &r_attr))
    return false;

  ABG_ASSERT(l_attr.form == DW_FORM_strp
	 || l_attr.form == DW_FORM_string
	 || l_attr.form == DW_FORM_GNU_strp_alt);

  ABG_ASSERT(r_attr.form == DW_FORM_strp
	 || r_attr.form == DW_FORM_string
	 || r_attr.form == DW_FORM_GNU_strp_alt);

  if ((l_attr.form == DW_FORM_strp
       && r_attr.form == DW_FORM_strp)
      || (l_attr.form == DW_FORM_GNU_strp_alt
	  && r_attr.form == DW_FORM_GNU_strp_alt))
    {
      // So these string attributes are actually pointers into a
      // string table.  The string table is most likely de-duplicated
      // so comparing the *values* of the pointers should be enough.
      //
      // This is the fast path.
      if (l_attr.valp == r_attr.valp)
	  result = true;
      else if (l_attr.valp && r_attr.valp)
	result = *l_attr.valp == *r_attr.valp;
      else
	result = false;
      return true;
    }

  // If we reached this point it means we couldn't use the fast path
  // because the string atttributes are strings that are "inline" in
  // the debug info section.  Let's just compare them the slow and
  // obvious way.
  string l_str = die_string_attribute(l, attr_name),
    r_str = die_string_attribute(r, attr_name);
  result = l_str == r_str;

  return true;
}

/// Compare the file path of the compilation units (aka CUs)
/// associated to two DIEs.
///
/// If the DIEs are for pointers or typedefs, this function also
/// compares the file paths of the CUs of the leaf DIEs (underlying
/// DIEs of the pointer or the typedef).
///
/// @param l the first type DIE to consider.
///
/// @param r the second type DIE to consider.
///
/// @return true iff the file paths of the DIEs of the two types are
/// equal.
static bool
compare_dies_cu_decl_file(Dwarf_Die* l, Dwarf_Die *r, bool &result)
{
  Dwarf_Die l_cu, r_cu;
  if (!dwarf_diecu(l, &l_cu, 0, 0)
      ||!dwarf_diecu(r, &r_cu, 0, 0))
    return false;

  bool compared =
    compare_dies_string_attribute_value(&l_cu, &r_cu,
					DW_AT_name,
					result);
  if (compared)
    {
      Dwarf_Die peeled_l, peeled_r;
      if (die_is_pointer_reference_or_typedef_type(l)
	  && die_is_pointer_reference_or_typedef_type(r)
	  && die_peel_pointer_and_typedef(l, peeled_l)
	  && die_peel_pointer_and_typedef(r, peeled_r))
	{
	  if (!dwarf_diecu(&peeled_l, &l_cu, 0, 0)
	      ||!dwarf_diecu(&peeled_r, &r_cu, 0, 0))
	    return false;
	  compared =
	    compare_dies_string_attribute_value(&l_cu, &r_cu,
						DW_AT_name,
						result);
	}
    }

  return  compared;
}

// -----------------------------------
// <location expression evaluation>
// -----------------------------------

/// Get the value of a given DIE attribute, knowing that it must be a
/// location expression.
///
/// @param die the DIE to read the attribute from.
///
/// @param attr_name the name of the attribute to read the value for.
///
/// @param expr the pointer to allocate and fill with the resulting
/// array of operators + operands forming a dwarf expression.  This is
/// set iff the function returns true.
///
/// @param expr_len the length of the resulting dwarf expression.
/// This is set iff the function returns true.
///
/// @return true if the attribute exists and has a dwarf expression as
/// value.  In that case the expr and expr_len arguments are set to
/// the resulting dwarf exprssion.
static bool
die_location_expr(Dwarf_Die* die,
		  unsigned attr_name,
		  Dwarf_Op** expr,
		  uint64_t* expr_len)
{
  if (!die)
    return false;

  Dwarf_Attribute attr;
  if (!dwarf_attr_integrate(die, attr_name, &attr))
    return false;

  size_t len = 0;
  bool result = (dwarf_getlocation(&attr, expr, &len) == 0);

  if (result)
    *expr_len = len;

  return result;
}

/// If the current operation in the dwarf expression represents a push
/// of a constant value onto the dwarf expr virtual machine (aka
/// DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param ops the array of the dwarf expression operations to consider.
///
/// @param ops_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually pushes a constant
/// value onto the DEVM stack, false otherwise.
static bool
op_pushes_constant_value(Dwarf_Op*			ops,
			 uint64_t			ops_len,
			 uint64_t			index,
			 uint64_t&			next_index,
			 dwarf_expr_eval_context&	ctxt)
{
  ABG_ASSERT(index < ops_len);

  Dwarf_Op& op = ops[index];
  int64_t value = 0;

  switch (op.atom)
    {
    case DW_OP_addr:
      value = ops[index].number;
      break;

    case DW_OP_const1u:
    case DW_OP_const1s:
    case DW_OP_const2u:
    case DW_OP_const2s:
    case DW_OP_const4u:
    case DW_OP_const4s:
    case DW_OP_const8u:
    case DW_OP_const8s:
    case DW_OP_constu:
    case DW_OP_consts:
      value = ops[index].number;
      break;

    case DW_OP_lit0:
      value = 0;
      break;
    case DW_OP_lit1:
      value = 1;
      break;
    case DW_OP_lit2:
      value = 2;
      break;
    case DW_OP_lit3:
      value = 3;
      break;
    case DW_OP_lit4:
      value = 4;
      break;
    case DW_OP_lit5:
      value = 5;
      break;
    case DW_OP_lit6:
      value = 6;
      break;
    case DW_OP_lit7:
      value = 7;
      break;
    case DW_OP_lit8:
      value = 8;
      break;
    case DW_OP_lit9:
      value = 9;
      break;
    case DW_OP_lit10:
      value = 10;
      break;
    case DW_OP_lit11:
      value = 11;
      break;
    case DW_OP_lit12:
      value = 12;
      break;
    case DW_OP_lit13:
      value = 13;
      break;
    case DW_OP_lit14:
      value = 14;
      break;
    case DW_OP_lit15:
      value = 15;
      break;
    case DW_OP_lit16:
      value = 16;
      break;
    case DW_OP_lit17:
      value = 17;
      break;
    case DW_OP_lit18:
      value = 18;
      break;
    case DW_OP_lit19:
      value = 19;
      break;
    case DW_OP_lit20:
      value = 20;
      break;
    case DW_OP_lit21:
      value = 21;
      break;
    case DW_OP_lit22:
      value = 22;
      break;
    case DW_OP_lit23:
      value = 23;
      break;
    case DW_OP_lit24:
      value = 24;
      break;
    case DW_OP_lit25:
      value = 25;
      break;
    case DW_OP_lit26:
      value = 26;
      break;
    case DW_OP_lit27:
      value = 27;
      break;
    case DW_OP_lit28:
      value = 28;
      break;
    case DW_OP_lit29:
      value = 29;
      break;
    case DW_OP_lit30:
      value = 30;
      break;
    case DW_OP_lit31:
      value = 31;
      break;

    default:
      return false;
    }

  expr_result r(value);
  ctxt.push(r);
  ctxt.accum = r;
  next_index = index + 1;

  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of a non-constant value onto the dwarf expr virtual machine (aka
/// DEVM), perform the operation and update the DEVM.  A non-constant
/// is namely a quantity for which we need inferior (a running program
/// image) state to know the exact value.
///
/// Upon successful completion, as the result of the operation is a
/// non-constant the DEVM accumulator value is left to its state as of
/// before the invocation of this function.
///
/// @param ops the array of the dwarf expression operations to consider.
///
/// @param ops_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually pushes a
/// non-constant value onto the DEVM stack, false otherwise.
static bool
op_pushes_non_constant_value(Dwarf_Op* ops,
			     uint64_t ops_len,
			     uint64_t index,
			     uint64_t& next_index,
			     dwarf_expr_eval_context& ctxt)
{
  ABG_ASSERT(index < ops_len);
  Dwarf_Op& op = ops[index];

  switch (op.atom)
    {
    case DW_OP_reg0:
    case DW_OP_reg1:
    case DW_OP_reg2:
    case DW_OP_reg3:
    case DW_OP_reg4:
    case DW_OP_reg5:
    case DW_OP_reg6:
    case DW_OP_reg7:
    case DW_OP_reg8:
    case DW_OP_reg9:
    case DW_OP_reg10:
    case DW_OP_reg11:
    case DW_OP_reg12:
    case DW_OP_reg13:
    case DW_OP_reg14:
    case DW_OP_reg15:
    case DW_OP_reg16:
    case DW_OP_reg17:
    case DW_OP_reg18:
    case DW_OP_reg19:
    case DW_OP_reg20:
    case DW_OP_reg21:
    case DW_OP_reg22:
    case DW_OP_reg23:
    case DW_OP_reg24:
    case DW_OP_reg25:
    case DW_OP_reg26:
    case DW_OP_reg27:
    case DW_OP_reg28:
    case DW_OP_reg29:
    case DW_OP_reg30:
    case DW_OP_reg31:
      next_index = index + 1;
      break;

    case DW_OP_breg0:
    case DW_OP_breg1:
    case DW_OP_breg2:
    case DW_OP_breg3:
    case DW_OP_breg4:
    case DW_OP_breg5:
    case DW_OP_breg6:
    case DW_OP_breg7:
    case DW_OP_breg8:
    case DW_OP_breg9:
    case DW_OP_breg10:
    case DW_OP_breg11:
    case DW_OP_breg12:
    case DW_OP_breg13:
    case DW_OP_breg14:
    case DW_OP_breg15:
    case DW_OP_breg16:
    case DW_OP_breg17:
    case DW_OP_breg18:
    case DW_OP_breg19:
    case DW_OP_breg20:
    case DW_OP_breg21:
    case DW_OP_breg22:
    case DW_OP_breg23:
    case DW_OP_breg24:
    case DW_OP_breg25:
    case DW_OP_breg26:
    case DW_OP_breg27:
    case DW_OP_breg28:
    case DW_OP_breg29:
    case DW_OP_breg30:
    case DW_OP_breg31:
      next_index = index + 1;
      break;

    case DW_OP_regx:
      next_index = index + 2;
      break;

    case DW_OP_fbreg:
      next_index = index + 1;
      break;

    case DW_OP_bregx:
      next_index = index + 1;
      break;

    default:
      return false;
    }

  expr_result r(false);
  ctxt.push(r);

  return true;
}

/// If the current operation in the dwarf expression represents a
/// manipulation of the stack of the DWARF Expression Virtual Machine
/// (aka DEVM), this function performs the operation and updates the
/// state of the DEVM.  If the result of the operation represents a
/// constant value, then the accumulator of the DEVM is set to that
/// result's value, Otherwise, the DEVM accumulator is left with its
/// previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p ops array above.
///
/// @param index the index of the operation to interpret, in @p ops.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually manipulates the
/// DEVM stack, false otherwise.
static bool
op_manipulates_stack(Dwarf_Op* expr,
		     uint64_t expr_len,
		     uint64_t index,
		     uint64_t& next_index,
		     dwarf_expr_eval_context& ctxt)
{
  Dwarf_Op& op = expr[index];
  expr_result v;

  switch (op.atom)
    {
    case DW_OP_dup:
      v = ctxt.stack.front();
      ctxt.push(v);
      break;

    case DW_OP_drop:
      v = ctxt.stack.front();
      ctxt.pop();
      break;

    case DW_OP_over:
      ABG_ASSERT(ctxt.stack.size() > 1);
      v = ctxt.stack[1];
      ctxt.push(v);
      break;

    case DW_OP_pick:
      ABG_ASSERT(index + 1 < expr_len);
      v = op.number;
      ctxt.push(v);
      break;

    case DW_OP_swap:
      ABG_ASSERT(ctxt.stack.size() > 1);
      v = ctxt.stack[1];
      ctxt.stack.erase(ctxt.stack.begin() + 1);
      ctxt.push(v);
      break;

    case DW_OP_rot:
      ABG_ASSERT(ctxt.stack.size() > 2);
      v = ctxt.stack[2];
      ctxt.stack.erase(ctxt.stack.begin() + 2);
      ctxt.push(v);
      break;

    case DW_OP_deref:
    case DW_OP_deref_size:
      ABG_ASSERT(ctxt.stack.size() > 0);
      ctxt.pop();
      v.is_const(false);
      ctxt.push(v);
      break;

    case DW_OP_xderef:
    case DW_OP_xderef_size:
      ABG_ASSERT(ctxt.stack.size() > 1);
      ctxt.pop();
      ctxt.pop();
      v.is_const(false);
      ctxt.push(v);
      break;

    case DW_OP_push_object_address:
      v.is_const(false);
      ctxt.push(v);
      break;

    case DW_OP_form_tls_address:
    case DW_OP_GNU_push_tls_address:
      ABG_ASSERT(ctxt.stack.size() > 0);
      v = ctxt.pop();
      if (op.atom == DW_OP_form_tls_address)
	v.is_const(false);
      ctxt.push(v);
      break;

    case DW_OP_call_frame_cfa:
      v.is_const(false);
      ctxt.push(v);
      break;

    default:
      return false;
    }

  if (v.is_const())
    ctxt.accum = v;

  if (op.atom == DW_OP_form_tls_address
      || op.atom == DW_OP_GNU_push_tls_address)
    ctxt.set_tls_address(true);
  else
    ctxt.set_tls_address(false);

  next_index = index + 1;

  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of an arithmetic or logic operation onto the dwarf expr virtual
/// machine (aka DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p expr array above.
///
/// @param index the index of the operation to interpret, in @p expr.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually represent an
/// arithmetic or logic operation.
static bool
op_is_arith_logic(Dwarf_Op* expr,
		  uint64_t expr_len,
		  uint64_t index,
		  uint64_t& next_index,
		  dwarf_expr_eval_context& ctxt)
{
  ABG_ASSERT(index < expr_len);

  Dwarf_Op& op = expr[index];
  expr_result val1, val2;

  switch (op.atom)
    {
    case DW_OP_abs:
      val1 = ctxt.pop();
      val1 = val1.abs();
      ctxt.push(val1);
      break;

    case DW_OP_and:
      ABG_ASSERT(ctxt.stack.size() > 1);
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val1 & val2);
      break;

    case DW_OP_div:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      if (!val1.is_const())
	val1 = 1;
      ctxt.push(val2 / val1);
      break;

    case DW_OP_minus:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 - val1);
      break;

    case DW_OP_mod:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 % val1);
      break;

    case DW_OP_mul:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 * val1);
      break;

    case DW_OP_neg:
      val1 = ctxt.pop();
      ctxt.push(-val1);
      break;

    case DW_OP_not:
      val1 = ctxt.pop();
      ctxt.push(~val1);
      break;

    case DW_OP_or:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val1 | val2);
      break;

    case DW_OP_plus:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 + val1);
      break;

    case DW_OP_plus_uconst:
      val1 = ctxt.pop();
      val1 += op.number;
      ctxt.push(val1);
      break;

    case DW_OP_shl:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 << val1);
      break;

    case DW_OP_shr:
    case DW_OP_shra:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 >> val1);
      break;

    case DW_OP_xor:
      val1 = ctxt.pop();
      val2 = ctxt.pop();
      ctxt.push(val2 ^ val1);
      break;

    default:
      return false;
    }

  if (ctxt.stack.front().is_const())
    ctxt.accum = ctxt.stack.front();

  next_index = index + 1;
  return true;
}

/// If the current operation in the dwarf expression represents a push
/// of a control flow operation onto the dwarf expr virtual machine
/// (aka DEVM), perform the operation and update the DEVM.
///
/// If the result of the operation is a constant, update the DEVM
/// accumulator with its value.  Otherwise, the DEVM accumulator is
/// left with its previous value.
///
/// @param expr the array of the dwarf expression operations to consider.
///
/// @param expr_len the lengths of @p expr array above.
///
/// @param index the index of the operation to interpret, in @p expr.
///
/// @param next_index the index of the operation to interpret at the
/// next step, after this function completed and returned.  This is
/// set an output parameter that is set iff the function returns true.
///
/// @param ctxt the DEVM evaluation context.
///
/// @return true if the current operation actually represents a
/// control flow operation, false otherwise.
static bool
op_is_control_flow(Dwarf_Op* expr,
		   uint64_t expr_len,
		   uint64_t index,
		   uint64_t& next_index,
		   dwarf_expr_eval_context& ctxt)
{
  ABG_ASSERT(index < expr_len);

  Dwarf_Op& op = expr[index];
  expr_result val1, val2;

  switch (op.atom)
    {
    case DW_OP_eq:
    case DW_OP_ge:
    case DW_OP_gt:
    case DW_OP_le:
    case DW_OP_lt:
    case DW_OP_ne:
      {
	bool value = true;
	val1 = ctxt.pop();
	val2 = ctxt.pop();
	if (op.atom == DW_OP_eq)
	  value = val2 == val1;
	else if (op.atom == DW_OP_ge)
	  value = val2 >= val1;
	else if (op.atom == DW_OP_gt)
	  value = val2 > val1;
	else if (op.atom == DW_OP_le)
	  value = val2 <= val1;
	else if (op.atom == DW_OP_lt)
	  value = val2 < val1;
	else if (op.atom == DW_OP_ne)
	  value = val2 != val1;

	val1 = value ? 1 : 0;
	ctxt.push(val1);
      }
      break;

    case DW_OP_skip:
      if (op.number > 0)
	index += op.number - 1;
      break;

    case DW_OP_bra:
      val1 = ctxt.pop();
      if (val1 != 0)
	index += val1.const_value() - 1;
      break;

    case DW_OP_call2:
    case DW_OP_call4:
    case DW_OP_call_ref:
    case DW_OP_nop:
      break;

    default:
      return false;
    }

  if (ctxt.stack.front().is_const())
    ctxt.accum = ctxt.stack.front();

  next_index = index + 1;
  return true;
}

/// This function quickly evaluates a DWARF expression that is a
/// constant.
///
/// This is a "fast path" function that quickly evaluates a DWARF
/// expression that is only made of a DW_OP_plus_uconst operator.
///
/// This is a sub-routine of die_member_offset.
///
/// @param expr the DWARF expression to evaluate.
///
/// @param expr_len the length of the expression @p expr.
///
/// @param value out parameter.  This is set to the result of the
/// evaluation of @p expr, iff this function returns true.
///
/// @return true iff the evaluation of @p expr went OK.
static bool
eval_quickly(Dwarf_Op*	expr,
	     uint64_t	expr_len,
	     int64_t&	value)
{
  if (expr_len == 1 && (expr[0].atom == DW_OP_plus_uconst))
    {
      value = expr[0].number;
      return true;
    }
  return false;
}

/// Evaluate the value of the last sub-expression that is a constant,
/// inside a given DWARF expression.
///
/// @param expr the DWARF expression to consider.
///
/// @param expr_len the length of the expression to consider.
///
/// @param value the resulting value of the last constant
/// sub-expression of the DWARF expression.  This is set iff the
/// function returns true.
///
/// @param is_tls_address out parameter.  This is set to true iff
/// the resulting value of the evaluation is a TLS (thread local
/// storage) address.
///
/// @param eval_ctxt the evaluation context to (re)use.  Note that
/// this function initializes this context before using it.
///
/// @return true if the function could find a constant sub-expression
/// to evaluate, false otherwise.
static bool
eval_last_constant_dwarf_sub_expr(Dwarf_Op*	expr,
				  uint64_t	expr_len,
				  int64_t&	value,
				  bool&	is_tls_address,
				  dwarf_expr_eval_context &eval_ctxt)
{
  // Reset the evaluation context before evaluating the constant sub
  // expression contained in the DWARF expression 'expr'.
  eval_ctxt.reset();

  uint64_t index = 0, next_index = 0;
  do
    {
      if (op_is_arith_logic(expr, expr_len, index,
			    next_index, eval_ctxt)
	  || op_pushes_constant_value(expr, expr_len, index,
				      next_index, eval_ctxt)
	  || op_manipulates_stack(expr, expr_len, index,
				  next_index, eval_ctxt)
	  || op_pushes_non_constant_value(expr, expr_len, index,
					  next_index, eval_ctxt)
	  || op_is_control_flow(expr, expr_len, index,
				next_index, eval_ctxt))
	;
      else
	next_index = index + 1;

      ABG_ASSERT(next_index > index);
      index = next_index;
    } while (index < expr_len);

  is_tls_address = eval_ctxt.set_tls_address();
  if (eval_ctxt.accum.is_const())
    {
      value = eval_ctxt.accum;
      return true;
    }
  return false;
}

/// Evaluate the value of the last sub-expression that is a constant,
/// inside a given DWARF expression.
///
/// @param expr the DWARF expression to consider.
///
/// @param expr_len the length of the expression to consider.
///
/// @param value the resulting value of the last constant
/// sub-expression of the DWARF expression.  This is set iff the
/// function returns true.
///
/// @return true if the function could find a constant sub-expression
/// to evaluate, false otherwise.
static bool
eval_last_constant_dwarf_sub_expr(Dwarf_Op*	expr,
				  uint64_t	expr_len,
				  int64_t&	value,
				  bool&	is_tls_address)
{
  dwarf_expr_eval_context eval_ctxt;
  return eval_last_constant_dwarf_sub_expr(expr, expr_len, value,
					   is_tls_address, eval_ctxt);
}

// -----------------------------------
// </location expression evaluation>
// -----------------------------------

/// Get the offset of a struct/class member as represented by the
/// value of the DW_AT_data_member_location attribute.
///
/// There is a huge gotcha in here.  The value of the
/// DW_AT_data_member_location is not necessarily a constant that one
/// would just read and be done with it.  Rather, it can be a DWARF
/// expression that one has to interpret.  In general, the offset can
/// be given by the DW_AT_bit_offset attribute.  In that case the
/// offset is a constant.  But it can also be given by the
/// DW_AT_data_member_location attribute.  In that case it's a DWARF
/// location expression.
///
/// When the it's the DW_AT_data_member_location that is present,
/// there are three cases to possibly take into account:
///
///     1/ The offset in the vtable where the offset of a virtual base
///        can be found, aka vptr offset.  Given the address of a
///        given object O, the vptr offset for B is given by the
///        (DWARF) expression:
///
///            address(O) + *(*address(0) - VIRTUAL_OFFSET)
///
///        where VIRTUAL_OFFSET is a constant value; In this case,
///        this function returns the constant VIRTUAL_OFFSET, as this
///        is enough to detect changes in a given virtual base
///        relative to the other virtual bases.
///
///     2/ The offset of a regular data member.  Given the address of
///        a struct object named O, the memory location for a
///        particular data member is given by the (DWARF) expression:
///
///            address(O) + OFFSET
///
///       where OFFSET is a constant.  In this case, this function
///       returns the OFFSET constant.
///
///     3/ The offset of a virtual member function in the virtual
///     pointer.  The DWARF expression is a constant that designates
///     the offset of the function in the vtable.  In this case this
///     function returns that constant.
///
///@param ctxt the read context to consider.
///
///@param die the DIE to read the information from.
///
///@param offset the resulting constant offset, in bits.  This
///argument is set iff the function returns true.
static bool
die_member_offset(const read_context& ctxt, Dwarf_Die* die, int64_t& offset)
{
  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;
  uint64_t off = 0;

  if (die_unsigned_constant_attribute(die, DW_AT_bit_offset, off))
    {
      // The DW_AT_bit_offset is present.  If it contains a non-zero
      // value, let's read that one.
      if (off != 0)
	{
	  offset = off;
	  return true;
	}
    }

  if (!die_location_expr(die, DW_AT_data_member_location, &expr, &expr_len))
    return false;

  // Otherwise, the DW_AT_data_member_location attribute is present.
  // In that case, let's evaluate it and get its constant
  // sub-expression and return that one.

  if (!eval_quickly(expr, expr_len, offset))
    {
      bool is_tls_address = false;
      if (!eval_last_constant_dwarf_sub_expr(expr, expr_len,
					     offset, is_tls_address,
					     ctxt.dwarf_expr_eval_ctxt()))
	return false;
    }

  offset *= 8;
  return true;
}

/// Read the value of the DW_AT_location attribute from a DIE,
/// evaluate the resulting DWARF expression and, if it's a constant
/// expression, return it.
///
/// @param die the DIE to consider.
///
/// @param address the resulting constant address.  This is set iff
/// the function returns true.
///
/// @return true iff the whole sequence of action described above
/// could be completed normally.
static bool
die_location_address(Dwarf_Die*	die,
		     Dwarf_Addr&	address,
		     bool&		is_tls_address)
{
  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;

  is_tls_address = false;
  if (!die_location_expr(die, DW_AT_location, &expr, &expr_len))
    return false;

  int64_t addr = 0;
  if (!eval_last_constant_dwarf_sub_expr(expr, expr_len, addr, is_tls_address))
    return false;

  address = addr;
  return true;
}


/// Return the index of a function in its virtual table.  That is,
/// return the value of the DW_AT_vtable_elem_location attribute.
///
/// @param die the DIE of the function to consider.
///
/// @param vindex the resulting index.  This is set iff the function
/// returns true.
///
/// @return true if the DIE has a DW_AT_vtable_elem_location
/// attribute.
static bool
die_virtual_function_index(Dwarf_Die* die,
			   int64_t& vindex)
{
  if (!die)
    return false;

  Dwarf_Op* expr = NULL;
  uint64_t expr_len = 0;
  if (!die_location_expr(die, DW_AT_vtable_elem_location,
			 &expr, &expr_len))
    return false;

  int64_t i = 0;
  bool is_tls_addr = false;
  if (!eval_last_constant_dwarf_sub_expr(expr, expr_len, i, is_tls_addr))
    return false;

  vindex = i;
  return true;
}

/// Test if a given DIE represents an anonymous type.
///
/// Anonymous types we are interested in are classes, unions and
/// enumerations.
///
/// @param die the DIE to consider.
///
/// @return true iff @p die represents an anonymous type.
bool
is_anonymous_type_die(Dwarf_Die *die)
{
  int tag = dwarf_tag(die);

  if (tag == DW_TAG_class_type
      || tag == DW_TAG_structure_type
      || tag == DW_TAG_union_type
      || tag == DW_TAG_enumeration_type)
    return die_is_anonymous(die);

  return false;
}

/// Return the base of the internal name to represent an anonymous
/// type.
///
/// Typically, anonymous enums would be named
/// __anonymous_enum__<number>, anonymous struct or classes would be
/// named __anonymous_struct__<number> and anonymous unions would be
/// named __anonymous_union__<number>.  The first part of these
/// anonymous names (i.e, __anonymous_{enum,struct,union}__ is called
/// the base name.  This function returns that base name, depending on
/// the kind of type DIE we are looking at.
///
/// @param die the type DIE to look at.  This function expects a type
/// DIE with an empty DW_AT_name property value (anonymous).
///
/// @return a string representing the base of the internal anonymous
/// name.
static string
get_internal_anonynous_die_base_name(Dwarf_Die *die)
{
  ABG_ASSERT(die_is_type(die));
  ABG_ASSERT(die_string_attribute(die, DW_AT_name) == "");

  int tag = dwarf_tag(die);
  string type_name;
  if (tag == DW_TAG_class_type || tag == DW_TAG_structure_type)
    type_name = "__anonymous_struct__";
  else if (tag == DW_TAG_union_type)
    type_name = "__anonymous_union__";
  else if (tag == DW_TAG_enumeration_type)
    type_name = "__anonymous_enum__";

  return type_name;
}

/// Build a full internal anonymous type name.
///
/// @param base_name this is the base name as returned by the function
/// @ref get_internal_anonynous_die_base_name.
///
/// @param anonymous_type_index this is the index of the anonymous
/// type in its scope.  That is, if there are more than one anonymous
/// types of a given kind in a scope, this index is what tells them
/// appart, starting from 0.
///
/// @return the built string, which is a concatenation of @p base_name
/// and @p anonymous_type_index.
static string
build_internal_anonymous_die_name(const string &base_name,
				  size_t anonymous_type_index)
{
  string name = base_name;
  if (anonymous_type_index && !base_name.empty())
    {
      std::ostringstream o;
      o << base_name << anonymous_type_index;
      name = o.str();
    }
  return name;
}

/// Build a full internal anonymous type name.
///
/// @param die the DIE representing the anonymous type to consider.
///
/// @param anonymous_type_index the index of the anonymous type
/// represented by @p DIE, in its scope.  That is, if there are
/// several different anonymous types of the same kind as @p die, this
/// index is what tells them appart.
///
/// @return the internal name of the anonymous type represented by @p
/// DIE.
static string
get_internal_anonymous_die_name(Dwarf_Die *die,
				size_t anonymous_type_index)
{
  string name = get_internal_anonynous_die_base_name(die);
  name = build_internal_anonymous_die_name(name, anonymous_type_index);
  return name;
}

// ------------------------------------
// <DIE pretty printer>
// ------------------------------------

/// Compute the qualified name of a DIE that represents a type.
///
/// For instance, if the DIE tag is DW_TAG_subprogram then this
/// function computes the name of the function *type*.
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param where_offset where in the are logically are in the DIE
/// stream.
///
/// @return a copy of the qualified name of the type.
static string
die_qualified_type_name(const read_context& ctxt,
			Dwarf_Die* die,
			size_t where_offset)
{
  if (!die)
    return "";

  int tag = dwarf_tag (die);
  if (tag == DW_TAG_compile_unit
      || tag == DW_TAG_partial_unit
      || tag == DW_TAG_type_unit)
    return "";

  string name = die_name(die);

  Dwarf_Die scope_die;
  if (!get_scope_die(ctxt, die, where_offset, scope_die))
    return "";

  string parent_name = die_qualified_name(ctxt, &scope_die, where_offset);
  bool colon_colon = die_is_type(die) || die_is_namespace(die);
  string separator = colon_colon ? "::" : ".";

  string repr;

  switch (tag)
    {
    case DW_TAG_unspecified_type:
      break;

    case DW_TAG_base_type:
      {
	abigail::ir::integral_type int_type;
	if (parse_integral_type(name, int_type))
	  repr = int_type;
	else
	  repr = name;
      }
      break;

    case DW_TAG_typedef:
    case DW_TAG_enumeration_type:
    case DW_TAG_structure_type:
    case DW_TAG_class_type:
    case DW_TAG_union_type:
      {
	if (tag == DW_TAG_typedef)
	  {
	    // If the underlying type of the typedef is unspecified,
	    // bail out as we don't support that yet.
	    Dwarf_Die underlying_type_die;
	    if (die_die_attribute(die, DW_AT_type, underlying_type_die))
	      {
		string n = die_qualified_type_name(ctxt, &underlying_type_die,
						   where_offset);
		if (die_is_unspecified(&underlying_type_die)
		    || n.empty())
		  break;
	      }
	  }

	if (name.empty())
	  // TODO: handle cases where there are more than one
	  // anonymous type of the same kind in the same scope.  In
	  // that case, their name must be built with the function
	  // get_internal_anonymous_die_name or something of the same
	  // kind.
	  name = get_internal_anonynous_die_base_name(die);

	ABG_ASSERT(!name.empty());
	repr = parent_name.empty() ? name : parent_name + separator + name;
      }
      break;

    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      {
	Dwarf_Die underlying_type_die;
	bool has_underlying_type_die =
	  die_die_attribute(die, DW_AT_type, underlying_type_die);

	if (has_underlying_type_die && die_is_unspecified(&underlying_type_die))
	  break;

	if (tag == DW_TAG_const_type)
	  {
	    if (has_underlying_type_die
		&& die_is_reference_type(&underlying_type_die))
	      // A reference is always const.  So, to lower false
	      // positive reports in diff computations, we consider a
	      // const reference just as a reference.  But we need to
	      // keep the qualified-ness of the type.  So we introduce
	      // a 'no-op' qualifier here.  Please remember that this
	      // has to be kept in sync with what is done in
	      // get_name_of_qualified_type.  So if you change this
	      // here, you have to change that code there too.
	      repr = "";
	    else if (!has_underlying_type_die
		     || die_is_void_type(&underlying_type_die))
	      {
		repr = "void";
		break;
	      }
	    else
	      repr = "const";
	  }
	else if (tag == DW_TAG_volatile_type)
	  repr = "volatile";
	else if (tag == DW_TAG_restrict_type)
	  repr = "restrict";
	else
	  ABG_ASSERT_NOT_REACHED;

	string underlying_type_repr;
	if (has_underlying_type_die)
	  underlying_type_repr =
	    die_qualified_type_name(ctxt, &underlying_type_die, where_offset);
	else
	  underlying_type_repr = "void";

	if (underlying_type_repr.empty())
	  repr.clear();
	else
	  {
	    if (has_underlying_type_die
		&& die_is_pointer_or_reference_type(&underlying_type_die))
	      repr = underlying_type_repr + " " + repr;
	    else
	      repr += " " + underlying_type_repr;
	  }
      }
      break;

    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      {
	Dwarf_Die pointed_to_type_die;
	if (!die_die_attribute(die, DW_AT_type, pointed_to_type_die))
	  {
	    if (tag == DW_TAG_pointer_type)
	      repr = "void*";
	    break;
	  }

	if (die_is_unspecified(&pointed_to_type_die))
	  break;

	string pointed_type_repr =
	  die_qualified_type_name(ctxt, &pointed_to_type_die, where_offset);

	repr = pointed_type_repr;
	if (repr.empty())
	  break;

	if (tag == DW_TAG_pointer_type)
	  repr += "*";
	else if (tag == DW_TAG_reference_type)
	  repr += "&";
	else if (tag == DW_TAG_rvalue_reference_type)
	  repr += "&&";
	else
	  ABG_ASSERT_NOT_REACHED;
      }
      break;

    case DW_TAG_subrange_type:
      {
	// In Ada, this one can be generated on its own, that is, not
	// as a sub-type of an array.  So we need to support it on its
	// own.  Note that when it's emitted as the sub-type of an
	// array like in C and C++, this is handled differently, for
	// now.  But we try to make this usable by other languages
	// that are not Ada, even if we modelled it after Ada.

	// So we build a subrange type for the sole purpose of using
	// the ::as_string() method of that type.  So we don't add
	// that type to the current type tree being built.
	array_type_def::subrange_sptr s =
	  build_subrange_type(const_cast<read_context&>(ctxt),
			      die, where_offset,
			      /*associate_die_to_type=*/false);
	repr += s->as_string();
	break;
      }

    case DW_TAG_array_type:
      {
	Dwarf_Die element_type_die;
	if (!die_die_attribute(die, DW_AT_type, element_type_die))
	  break;
	string element_type_name =
	  die_qualified_type_name(ctxt, &element_type_die, where_offset);
	if (element_type_name.empty())
	  break;

	array_type_def::subranges_type subranges;
	build_subranges_from_array_type_die(const_cast<read_context&>(ctxt),
					    die, subranges, where_offset,
					    /*associate_type_to_die=*/false);

	repr = element_type_name;
	repr += array_type_def::subrange_type::vector_as_string(subranges);
      }
      break;

    case DW_TAG_subroutine_type:
    case DW_TAG_subprogram:
      {
	string return_type_name;
	string class_name;
	vector<string> parm_names;
	bool is_const = false;
	bool is_static = false;

	die_return_and_parm_names_from_fn_type_die(ctxt, die, where_offset,
						   /*pretty_print=*/true,
						   return_type_name, class_name,
						   parm_names, is_const,
						   is_static);
	if (return_type_name.empty())
	  return_type_name = "void";

	repr = return_type_name;

	if (!class_name.empty())
	  {
	    // This is a method, so print the class name.
	    repr += " (" + class_name + "::*)";
	  }

	// Now parameters.
	repr += " (";
	for (vector<string>::const_iterator i = parm_names.begin();
	     i != parm_names.end();
	     ++i)
	  {
	    if (i != parm_names.begin())
	      repr += ", ";
	    repr += *i;
	  }
	repr += ")";

      }
      break;

    case DW_TAG_string_type:
    case DW_TAG_ptr_to_member_type:
    case DW_TAG_set_type:
    case DW_TAG_file_type:
    case DW_TAG_packed_type:
    case DW_TAG_thrown_type:
    case DW_TAG_interface_type:
    case DW_TAG_shared_type:
      break;
    }

  return repr;
}

/// Compute the qualified name of a decl represented by a given DIE.
///
/// For instance, for a DIE of tag DW_TAG_subprogram this function
/// computes the signature of the function *declaration*.
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return a copy of the computed name.
static string
die_qualified_decl_name(const read_context& ctxt,
			Dwarf_Die* die,
			size_t where_offset)
{
  if (!die || !die_is_decl(die))
    return "";

  string name = die_name(die);

  Dwarf_Die scope_die;
  if (!get_scope_die(ctxt, die, where_offset, scope_die))
    return "";

  string scope_name = die_qualified_name(ctxt, &scope_die, where_offset);
  string separator = "::";

  string repr;

  int tag = dwarf_tag(die);
  switch (tag)
    {
    case DW_TAG_namespace:
    case DW_TAG_member:
    case DW_TAG_variable:
      repr = scope_name.empty() ? name : scope_name + separator + name;
      break;
    case DW_TAG_subprogram:
      repr = die_function_signature(ctxt, die, where_offset);
      break;

    case DW_TAG_unspecified_parameters:
      repr = "...";
      break;

    case DW_TAG_formal_parameter:
    case DW_TAG_imported_declaration:
    case DW_TAG_GNU_template_template_param:
    case DW_TAG_GNU_template_parameter_pack:
    case DW_TAG_GNU_formal_parameter_pack:
      break;
    }
  return repr;
}

/// Compute the qualified name of the artifact represented by a given
/// DIE.
///
/// If the DIE represents a type, then the function computes the name
/// of the type.  Otherwise, if the DIE represents a decl then the
/// function computes the name of the decl.  Note that a DIE of tag
/// DW_TAG_subprogram is going to be considered as a "type" -- just
/// like if it was a DW_TAG_subroutine_type.
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return a copy of the computed name.
static string
die_qualified_name(const read_context& ctxt, Dwarf_Die* die, size_t where)
{
  if (die_is_type(die))
    return die_qualified_type_name(ctxt, die, where);
  else if (die_is_decl(die))
    return die_qualified_decl_name(ctxt, die, where);
  return "";
}

/// Test if the qualified name of a given type should be empty.
///
/// The reason why the name of a DIE with a given tag would be empty
/// is that libabigail's internal representation doesn't yet support
/// that tag; or if the DIE's qualified name is built from names of
/// sub-types DIEs whose tags are not yet supported.
///
/// @param ctxt the reading context.
///
/// @param die the DIE to consider.
///
/// @param where where we are logically at, in the DIE stream.
///
/// @param qualified_name the qualified name of the DIE.  This is set
/// only iff the function returns false.
///
/// @return true if the qualified name of the DIE is empty.
static bool
die_qualified_type_name_empty(const read_context& ctxt, Dwarf_Die* die,
			      size_t where, string &qualified_name)
{
  if (!die)
    return true;

  int tag = dwarf_tag(die);

  string qname;
  if (tag == DW_TAG_typedef
      || tag == DW_TAG_pointer_type
      || tag == DW_TAG_reference_type
      || tag == DW_TAG_rvalue_reference_type
      || tag == DW_TAG_array_type
      || tag == DW_TAG_const_type
      || tag == DW_TAG_volatile_type
      || tag == DW_TAG_restrict_type)
    {
      Dwarf_Die underlying_type_die;
      if (die_die_attribute(die, DW_AT_type, underlying_type_die))
	{
	  string name =
	    die_qualified_type_name(ctxt, &underlying_type_die, where);
	  if (name.empty())
	    return true;
	}
    }
  else
    {
      string name = die_qualified_type_name(ctxt, die, where);
      if (name.empty())
	return true;
    }

  qname = die_qualified_type_name(ctxt, die, where);
  if (qname.empty())
    return true;

  qualified_name = qname;
  return false;
}

/// Given the DIE that represents a function type, compute the names
/// of the following properties the function's type:
///
///   - return type
///   - enclosing class (if the function is a member function)
///   - function parameter types
///
/// When the function we are looking at is a member function, it also
/// tells if it's const.
///
/// @param ctxt the reading context.
///
/// @param die the DIE of the function or function type we are looking
/// at.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @param pretty_print if set to yes, the type names are going to be
/// pretty-printed names; otherwise, they are just qualified type
/// names.
///
/// @param return_type_name out parameter.  This contains the name of
/// the return type of the function.
///
/// @param class_name out parameter.  If the function is a member
/// function, this contains the name of the enclosing class.
///
/// @param parm_names out parameter.  This vector is set to the names
/// of the types of the parameters of the function.
///
/// @param is_const out parameter.  If the function is a member
/// function, this is set to true iff the member function is const.
///
/// @param is_static out parameter.  If the function is a static
/// member function, then this is set to true.
static void
die_return_and_parm_names_from_fn_type_die(const read_context& ctxt,
					   Dwarf_Die* die,
					   size_t where_offset,
					   bool pretty_print,
					   string &return_type_name,
					   string &class_name,
					   vector<string>& parm_names,
					   bool& is_const,
					   bool& is_static)
{
  Dwarf_Die child;
  Dwarf_Die ret_type_die;
  if (!die_die_attribute(die, DW_AT_type, ret_type_die))
    return_type_name = "void";
  else
    return_type_name =
      pretty_print
      ? ctxt.get_die_pretty_representation(&ret_type_die, where_offset)
      : ctxt.get_die_qualified_type_name(&ret_type_die, where_offset);

  if (return_type_name.empty())
    return_type_name = "void";

  Dwarf_Die object_pointer_die, class_die;
  bool is_method_type =
    die_function_type_is_method_type(ctxt, die, where_offset,
				     object_pointer_die,
				     class_die, is_static);

  is_const = false;
  if (is_method_type)
    {
      class_name = ctxt.get_die_qualified_type_name(&class_die, where_offset);

      Dwarf_Die this_pointer_die;
      Dwarf_Die pointed_to_die;
      if (!is_static
	  && die_die_attribute(&object_pointer_die, DW_AT_type,
			       this_pointer_die))
	if (die_die_attribute(&this_pointer_die, DW_AT_type, pointed_to_die))
	  if (dwarf_tag(&pointed_to_die) == DW_TAG_const_type)
	    is_const = true;

      string fn_name = die_name(die);
      string non_qualified_class_name = die_name(&class_die);
      bool is_ctor = fn_name == non_qualified_class_name;
      bool is_dtor = !fn_name.empty() && fn_name[0] == '~';

      if (is_ctor || is_dtor)
	return_type_name.clear();
    }

  if (dwarf_child(die, &child) == 0)
    do
      {
	int child_tag = dwarf_tag(&child);
	if (child_tag == DW_TAG_formal_parameter)
	  {
	    Dwarf_Die parm_type_die;
	    if (!die_die_attribute(&child, DW_AT_type, parm_type_die))
	      continue;
	    string qualified_name =
	      pretty_print
	      ? ctxt.get_die_pretty_representation(&parm_type_die, where_offset)
	      : ctxt.get_die_qualified_type_name(&parm_type_die, where_offset);

	    if (qualified_name.empty())
	      continue;
	    parm_names.push_back(qualified_name);
	  }
	else if (child_tag == DW_TAG_unspecified_parameters)
	  {
	    // This is a variadic function parameter.
	    parm_names.push_back("variadic parameter type");
	    // After a DW_TAG_unspecified_parameters tag, we shouldn't
	    // keep reading for parameters.  The
	    // unspecified_parameters TAG should be the last parameter
	    // that we record. For instance, if there are multiple
	    // DW_TAG_unspecified_parameters DIEs then we should care
	    // only for the first one.
	    break;
	  }
      }
    while (dwarf_siblingof(&child, &child) == 0);

  if (class_name.empty())
    {
      Dwarf_Die parent_die;
      if (get_parent_die(ctxt, die, parent_die, where_offset))
	{
	  if (die_is_class_type(&parent_die))
	    class_name =
	      ctxt.get_die_qualified_type_name(&parent_die, where_offset);
	}
    }
}

/// This computes the signature of the a function declaration
/// represented by a DIE.
///
/// @param ctxt the reading context.
///
/// @param fn_die the DIE of the function to consider.
///
/// @param where_offset where we are logically at in the stream of
/// DIEs.
///
/// @return a copy of the computed function signature string.
static string
die_function_signature(const read_context& ctxt,
		       Dwarf_Die *fn_die,
		       size_t where_offset)
{

  translation_unit::language lang;
  bool has_lang = false;
  if ((has_lang = ctxt.get_die_language(fn_die, lang)))
    {
      // In a binary originating from the C language, it's OK to use
      // the linkage name of the function as a key for the map which
      // is meant to reduce the number of DIE comparisons involved
      // during DIE canonicalization computation.
      if (is_c_language(lang))
	{
	  string fn_name = die_linkage_name(fn_die);
	  if (fn_name.empty())
	    fn_name = die_name(fn_die);
	  return fn_name;
	}
    }

  // TODO: When we can structurally compare DIEs originating from C++
  // as well, we can use the linkage name of functions in C++ too, to
  // reduce the number of comparisons involved during DIE
  // canonicalization.

  string return_type_name;
  Dwarf_Die ret_type_die;
  if (die_die_attribute(fn_die, DW_AT_type, ret_type_die))
    return_type_name = ctxt.get_die_qualified_type_name(&ret_type_die,
							where_offset);

  if (return_type_name.empty())
    return_type_name = "void";

  Dwarf_Die scope_die;
  string scope_name;
  if (get_scope_die(ctxt, fn_die, where_offset, scope_die))
    scope_name = ctxt.get_die_qualified_name(&scope_die, where_offset);
  string fn_name = die_name(fn_die);
  if (!scope_name.empty())
    fn_name  = scope_name + "::" + fn_name;

  string class_name;
  vector<string> parm_names;
  bool is_const = false;
  bool is_static = false;

  die_return_and_parm_names_from_fn_type_die(ctxt, fn_die, where_offset,
					     /*pretty_print=*/false,
					     return_type_name, class_name,
					     parm_names, is_const, is_static);

  bool is_virtual = die_is_virtual(fn_die);

  string repr = class_name.empty() ? "function" : "method";
  if (is_virtual)
    repr += " virtual";

  if (!return_type_name.empty())
    repr += " " + return_type_name;

  repr += " " + fn_name;

  // Now parameters.
  repr += "(";
  bool some_parm_emitted = false;
  for (vector<string>::const_iterator i = parm_names.begin();
       i != parm_names.end();
       ++i)
    {
      if (i != parm_names.begin())
	{
	  if (some_parm_emitted)
	    repr += ", ";
	}
      else
	if (!is_static && !class_name.empty())
	  // We are printing a non-static method name, skip the implicit "this"
	  // parameter type.
	  continue;
      repr += *i;
      some_parm_emitted = true;
    }
  repr += ")";

  if (is_const)
    {
      ABG_ASSERT(!class_name.empty());
      repr += " const";
    }

  return repr;
}

/// Return a pretty string representation of a type, for internal purposes.
///
/// By internal purpose, we mean things like key-ing types for lookup
/// purposes and so on.
///
/// Note that this function is also used to pretty print functions.
/// For functions, it prints the *type* of the function.
///
/// @param ctxt the context to use.
///
/// @param the DIE of the type to pretty print.
///
/// @param where_offset where we logically are placed when calling
/// this.  It's useful to handle inclusion of DW_TAG_compile_unit
/// entries.
///
/// @return the resulting pretty representation.
static string
die_pretty_print_type(read_context& ctxt, Dwarf_Die* die, size_t where_offset)
{
  if (!die || (!die_is_type(die) && dwarf_tag(die) != DW_TAG_subprogram))
    return "";

  string repr;

  int tag = dwarf_tag(die);
  switch (tag)
    {
    case DW_TAG_string_type:
      // For now, we won't try to go get the actual representation of
      // the string because this would make things more complicated;
      // for that we'd need to interpret some location expressions to
      // get the length of the string.  And for dynamically allocated
      // strings, the result of the location expression evaluation
      // might not even be a constant.  So at the moment I consider
      // this to be a lot of hassle for no great return.  Until proven
      // otherwise, of course.
      repr = "string type";

    case DW_TAG_unspecified_type:
    case DW_TAG_ptr_to_member_type:
      break;

    case DW_TAG_namespace:
      repr = "namespace " + ctxt.get_die_qualified_type_name(die, where_offset);
      break;

    case DW_TAG_base_type:
      repr = ctxt.get_die_qualified_type_name(die, where_offset);
      break;

    case DW_TAG_typedef:
      {
	string qualified_name;
	if (!die_qualified_type_name_empty(ctxt, die,
					   where_offset,
					   qualified_name))
	  repr = "typedef " + qualified_name;
      }
      break;

    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      repr = ctxt.get_die_qualified_type_name(die, where_offset);
      break;

    case DW_TAG_enumeration_type:
      {
	string qualified_name =
	  ctxt.get_die_qualified_type_name(die, where_offset);
	repr = "enum " + qualified_name;
      }
      break;

    case DW_TAG_structure_type:
    case DW_TAG_class_type:
      {
	string qualified_name =
	  ctxt.get_die_qualified_type_name(die, where_offset);
	repr = "class " + qualified_name;
      }
      break;

    case DW_TAG_union_type:
      {
	string qualified_name =
	  ctxt.get_die_qualified_type_name(die, where_offset);
	repr = "union " + qualified_name;
      }
      break;

    case DW_TAG_array_type:
      {
	Dwarf_Die element_type_die;
	if (!die_die_attribute(die, DW_AT_type, element_type_die))
	  break;
	string element_type_name =
	  ctxt.get_die_qualified_type_name(&element_type_die, where_offset);
	if (element_type_name.empty())
	  break;

	array_type_def::subranges_type subranges;
	build_subranges_from_array_type_die(ctxt, die, subranges, where_offset,
					    /*associate_type_to_die=*/false);

	repr = element_type_name;
	repr += array_type_def::subrange_type::vector_as_string(subranges);
      }
      break;

    case DW_TAG_subrange_type:
      {
	// So this can be generated by Ada, on its own; that is, not
	// as a subtype of an array.  In that case we need to handle
	// it properly.

	// For now, we consider that the pretty printed name of the
	// subrange type is its name.  We might need something more
	// advance, should the needs of the users get more
	// complicated.
	repr += die_qualified_type_name(ctxt, die, where_offset);
      }
      break;

    case DW_TAG_subroutine_type:
    case DW_TAG_subprogram:
      {
	string return_type_name;
	string class_name;
	vector<string> parm_names;
	bool is_const = false;
	bool is_static = false;

	die_return_and_parm_names_from_fn_type_die(ctxt, die, where_offset,
						   /*pretty_print=*/true,
						   return_type_name, class_name,
						   parm_names, is_const,
						   is_static);
	if (class_name.empty())
	  repr = "function type";
	else
	  repr = "method type";
	repr += " " + ctxt.get_die_qualified_type_name(die, where_offset);
      }
      break;

    case DW_TAG_set_type:
    case DW_TAG_file_type:
    case DW_TAG_packed_type:
    case DW_TAG_thrown_type:
    case DW_TAG_interface_type:
    case DW_TAG_shared_type:
      ABG_ASSERT_NOT_REACHED;
    }

  return repr;
}

/// Return a pretty string representation of a declaration, for
/// internal purposes.
///
/// By internal purpose, we mean things like key-ing declarations for
/// lookup purposes and so on.
///
/// Note that this function is also used to pretty print functions.
/// For functions, it prints the signature of the function.
///
/// @param ctxt the context to use.
///
/// @param the DIE of the declaration to pretty print.
///
/// @param where_offset where we logically are placed when calling
/// this.  It's useful to handle inclusion of DW_TAG_compile_unit
/// entries.
///
/// @return the resulting pretty representation.
static string
die_pretty_print_decl(read_context& ctxt, Dwarf_Die* die, size_t where_offset)
{
  if (!die || !die_is_decl(die))
    return "";

  string repr;

  int tag = dwarf_tag(die);
  switch (tag)
    {
    case DW_TAG_namespace:
      repr = "namespace " + die_qualified_name(ctxt, die, where_offset);
      break;

    case DW_TAG_member:
    case DW_TAG_variable:
      {
	string type_repr = "void";
	Dwarf_Die type_die;
	if (die_die_attribute(die, DW_AT_type, type_die))
	  type_repr = die_qualified_type_name(ctxt, &type_die, where_offset);
	repr = die_qualified_name(ctxt, die, where_offset);
	if (!repr.empty())
	  repr = type_repr + " " + repr;
      }
      break;

    case DW_TAG_subprogram:
      repr = die_function_signature(ctxt, die, where_offset);
      break;

    default:
      break;
    }
  return repr;
}

/// Compute the pretty printed representation of an artifact
/// represented by a DIE.
///
/// If the DIE is a type, compute the its pretty representation as a
/// type; otherwise, if it's a declaration, compute its pretty
/// representation as a declaration.  Note for For instance, that a
/// DW_TAG_subprogram DIE is going to be represented as a function
/// *type*.
///
/// @param ctxt the reading context.
///
/// @param die the DIE to consider.
///
/// @param where_offset we in the DIE stream we are logically at.
///
/// @return a copy of the pretty printed artifact.
static string
die_pretty_print(read_context& ctxt, Dwarf_Die* die, size_t where_offset)
{
  if (die_is_type(die))
    return die_pretty_print_type(ctxt, die, where_offset);
  else if (die_is_decl(die))
    return die_pretty_print_decl(ctxt, die, where_offset);
  return "";
}

// -----------------------------------
// </die pretty printer>
// -----------------------------------


// ----------------------------------
// <die comparison engine>
// ---------------------------------

/// Compares two decls DIEs
///
/// This works only for DIEs emitted by the C language.
///
/// This implementation doesn't yet support namespaces.
///
/// This is a subroutine of compare_dies.
///
/// @return true iff @p l equals @p r.
static bool
compare_as_decl_dies(Dwarf_Die *l, Dwarf_Die *r)
{
  ABG_ASSERT(l && r);

  if (dwarf_tag(l) != dwarf_tag(r))
    return false;

  bool result = false;
  if (compare_dies_string_attribute_value(l, r, DW_AT_linkage_name,
					  result)
      || compare_dies_string_attribute_value(l, r, DW_AT_MIPS_linkage_name,
					     result))
    {
      if (!result)
	return false;
    }

  if (compare_dies_string_attribute_value(l, r, DW_AT_name,
					  result))
    {
      if (!result)
	return false;
    }

  return true;
}

/// Compares two type DIEs
///
/// This is a subroutine of compare_dies.
///
/// @param l the left operand of the comparison operator.
///
/// @param r the right operand of the comparison operator.
///
/// @return true iff @p l equals @p r.
static bool
compare_as_type_dies(Dwarf_Die *l, Dwarf_Die *r)
{
  ABG_ASSERT(l && r);
  ABG_ASSERT(die_is_type(l));
  ABG_ASSERT(die_is_type(r));

  if (dwarf_tag(l) == DW_TAG_string_type
      && dwarf_tag(r) == DW_TAG_string_type
      && dwarf_dieoffset(l) != dwarf_dieoffset(r))
    // For now, we cannot compare DW_TAG_string_type because of its
    // string_length attribute that is a location descriptor that is
    // not necessarily a constant.  So it's super hard to evaluate it
    // in a libabigail context.  So for now, we just say that all
    // DW_TAG_string_type DIEs are different, by default.
    return false;

  uint64_t l_size = 0, r_size = 0;
  die_size_in_bits(l, l_size);
  die_size_in_bits(r, r_size);

  return l_size == r_size;
}

/// Compare two DIEs emitted by a C compiler.
///
/// @param ctxt the read context used to load the DWARF information.
///
/// @param l the left-hand-side argument of this comparison operator.
///
/// @param r the righ-hand-side argument of this comparison operator.
///
/// @param aggregates_being_compared this holds the names of the set
/// of aggregates being compared.  It's used by the comparison
/// function to avoid recursing infinitely when faced with types
/// referencing themselves through pointers or references.  By
/// default, just pass an empty instance of @ref istring_set_type to
/// it.
///
/// @param update_canonical_dies_on_the_fly if true, when two
/// sub-types compare equal (during the comparison of @p l and @p r)
/// update their canonical type.  That way, two types of the same name
/// are structurally compared to each other only once.  So the
/// non-linear structural comparison of two types of the same name
/// only happen once.
///
/// @return true iff @p l equals @p r.
static bool
compare_dies(const read_context& ctxt, Dwarf_Die *l, Dwarf_Die *r,
	     istring_set_type& aggregates_being_compared,
	     bool update_canonical_dies_on_the_fly)
{
  ABG_ASSERT(l);
  ABG_ASSERT(r);

  int l_tag = dwarf_tag(l), r_tag = dwarf_tag(r);

  if (l_tag != r_tag)
    return false;

  Dwarf_Off l_offset = dwarf_dieoffset(l), r_offset = dwarf_dieoffset(r);
  Dwarf_Off l_canonical_die_offset = 0, r_canonical_die_offset = 0;
  die_source l_die_source, r_die_source;
  ABG_ASSERT(ctxt.get_die_source(l, l_die_source));
  ABG_ASSERT(ctxt.get_die_source(r, r_die_source));

  // If 'l' and 'r' already have canonical DIEs, then just compare the
  // offsets of their canonical DIEs.
  bool l_has_canonical_die_offset =
    (l_canonical_die_offset =
     ctxt.get_canonical_die_offset(l_offset, l_die_source,
				   /*die_as_type=*/true));

  bool r_has_canonical_die_offset =
    (r_canonical_die_offset =
     ctxt.get_canonical_die_offset(r_offset, r_die_source,
				   /*die_as_type=*/true));

  if (l_has_canonical_die_offset && r_has_canonical_die_offset)
    return l_canonical_die_offset == r_canonical_die_offset;

  bool result = true;

  switch (l_tag)
    {
    case DW_TAG_base_type:
    case DW_TAG_string_type:
      if (!compare_as_type_dies(l, r)
	  || !compare_as_decl_dies(l, r))
	result = false;
      break;

    case DW_TAG_typedef:
    case DW_TAG_pointer_type:
    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      {
	if (!compare_as_type_dies(l, r))
	  {
	    result = false;
	    break;
	  }

	bool from_the_same_tu = false;
	if (!pointer_or_qual_die_of_anonymous_class_type(l)
	    && compare_dies_cu_decl_file(l, r, from_the_same_tu)
	    && from_the_same_tu)
	  {
	    // These two typedefs, pointer, reference, or qualified
	    // types have the same name and are defined in the same TU.
	    // They thus ought to be the same.
	    //
	    // Note that pointers, reference or qualified types to
	    // anonymous types are not taking into account here because
	    // those always need to be structurally compared.
	    result = true;
	    break;
	  }
      }

      {
	// No fancy optimization in this case.  We need to
	// structurally compare the two DIEs.
	Dwarf_Die lu_type_die, ru_type_die;
	bool lu_is_void, ru_is_void;

	lu_is_void = !die_die_attribute(l, DW_AT_type, lu_type_die);
	ru_is_void = !die_die_attribute(r, DW_AT_type, ru_type_die);

	if (lu_is_void && ru_is_void)
	  result = true;
	else if (lu_is_void != ru_is_void)
	  result = false;
	else
	  result = compare_dies(ctxt, &lu_type_die, &ru_type_die,
				aggregates_being_compared,
				update_canonical_dies_on_the_fly);
      }
      break;

    case DW_TAG_enumeration_type:
      if (!compare_as_type_dies(l, r)
	  || !compare_as_decl_dies(l, r))
	result = false;
      else
	{
	  // Walk the enumerators.
	  Dwarf_Die l_enumtor, r_enumtor;
	  bool found_l_enumtor, found_r_enumtor;

	  for (found_l_enumtor = dwarf_child(l, &l_enumtor) == 0,
		 found_r_enumtor = dwarf_child(r, &r_enumtor) == 0;
	       found_l_enumtor && found_r_enumtor;
	       found_l_enumtor = dwarf_siblingof(&l_enumtor, &l_enumtor) == 0,
		 found_r_enumtor = dwarf_siblingof(&r_enumtor, &r_enumtor) == 0)
	    {
	      int l_tag = dwarf_tag(&l_enumtor), r_tag = dwarf_tag(&r_enumtor);
	      if ( l_tag != r_tag)
		{
		  result = false;
		  break;
		}

	      if (l_tag != DW_TAG_enumerator)
		continue;

	      uint64_t l_val = 0, r_val = 0;
	      die_unsigned_constant_attribute(&l_enumtor,
					      DW_AT_const_value,
					      l_val);
	      die_unsigned_constant_attribute(&r_enumtor,
					      DW_AT_const_value,
					      r_val);
	      if (l_val != r_val)
		{
		  result = false;
		  break;
		}
	    }
	  if (found_l_enumtor != found_r_enumtor )
	    result = false;

	}
      break;

    case DW_TAG_structure_type:
    case DW_TAG_union_type:
      {
	interned_string ln = ctxt.get_die_pretty_type_representation(l, 0);
	interned_string rn = ctxt.get_die_pretty_type_representation(r, 0);

	if ((aggregates_being_compared.find(ln)
	     != aggregates_being_compared.end())
	    || (aggregates_being_compared.find(rn)
		!= aggregates_being_compared.end()))
	  result = true;
	else if (!compare_as_decl_dies(l, r))
	  result = false;
	else if (!compare_as_type_dies(l, r))
	  result = false;
	else
	  {
	    aggregates_being_compared.insert(ln);
	    aggregates_being_compared.insert(rn);

	    Dwarf_Die l_member, r_member;
	    bool found_l_member, found_r_member;
	    for (found_l_member = dwarf_child(l, &l_member) == 0,
		   found_r_member = dwarf_child(r, &r_member) == 0;
		 found_l_member && found_r_member;
		 found_l_member = dwarf_siblingof(&l_member, &l_member) == 0,
		   found_r_member = dwarf_siblingof(&r_member, &r_member) == 0)
	      {
		int l_tag = dwarf_tag(&l_member), r_tag = dwarf_tag(&r_member);
		if (l_tag != r_tag)
		  {
		    result = false;
		    break;
		  }

		if (l_tag != DW_TAG_member && l_tag != DW_TAG_variable)
		  continue;

		if (!compare_dies(ctxt, &l_member, &r_member,
				  aggregates_being_compared,
				  update_canonical_dies_on_the_fly))
		  {
		    result = false;
		    break;
		  }
	      }
	    if (found_l_member != found_r_member)
	      result = false;

	    aggregates_being_compared.erase(ln);
	    aggregates_being_compared.erase(rn);
	  }
      }
      break;

    case DW_TAG_array_type:
      {
	Dwarf_Die l_child, r_child;
	bool found_l_child, found_r_child;
	for (found_l_child = dwarf_child(l, &l_child) == 0,
	       found_r_child = dwarf_child(r, &r_child) == 0;
	     found_l_child && found_r_child;
	     found_l_child = dwarf_siblingof(&l_child, &l_child) == 0,
	       found_r_child = dwarf_siblingof(&r_child, &r_child) == 0)
	  {
	    int l_child_tag = dwarf_tag(&l_child),
	      r_child_tag = dwarf_tag(&r_child);
	    if (l_child_tag == DW_TAG_subrange_type
		|| r_child_tag == DW_TAG_subrange_type)
	      if (!compare_dies(ctxt, &l_child, &r_child,
				aggregates_being_compared,
				update_canonical_dies_on_the_fly))
		{
		  result = false;
		  break;
		}
	  }
	if (found_l_child != found_r_child)
	  result = false;
      }
      break;

    case DW_TAG_subrange_type:
      {
	uint64_t l_lower_bound = 0, r_lower_bound = 0,
	  l_upper_bound = 0, r_upper_bound = 0;
	die_unsigned_constant_attribute(l, DW_AT_lower_bound, l_lower_bound);
	die_unsigned_constant_attribute(r, DW_AT_lower_bound, r_lower_bound);
	if (!die_unsigned_constant_attribute(l, DW_AT_upper_bound,
					     l_upper_bound))
	  {
	    uint64_t l_count = 0;
	    if (die_unsigned_constant_attribute(l, DW_AT_count, l_count))
	      {
		l_upper_bound = l_lower_bound + l_count;
		if (l_upper_bound)
		  --l_upper_bound;
	      }
	  }
	if (!die_unsigned_constant_attribute(r, DW_AT_upper_bound,
					     r_upper_bound))
	  {
	    uint64_t r_count = 0;
	    if (die_unsigned_constant_attribute(l, DW_AT_count, r_count))
	      {
		r_upper_bound = r_lower_bound + r_count;
		if (r_upper_bound)
		  --r_upper_bound;
	      }
	  }

	if ((l_lower_bound != r_lower_bound)
	    || (l_upper_bound != r_upper_bound))
	  result = false;
      }
      break;

    case DW_TAG_subroutine_type:
    case DW_TAG_subprogram:
      {
	interned_string ln = ctxt.get_die_pretty_type_representation(l, 0);
	interned_string rn = ctxt.get_die_pretty_type_representation(r, 0);

	if ((aggregates_being_compared.find(ln)
	     != aggregates_being_compared.end())
	    || (aggregates_being_compared.find(rn)
		!= aggregates_being_compared.end()))
	  result = true;
      else if (l_tag == DW_TAG_subroutine_type)
	{
	  // The string reprs of l and r are already equal.  Now let's
	  // just check if they both come from the same TU.
	  bool from_the_same_tu = false;
	  if (compare_dies_cu_decl_file(l, r, from_the_same_tu)
	      && from_the_same_tu)
	    result = true;
	}
	else
	  {
	    aggregates_being_compared.insert(ln);
	    aggregates_being_compared.insert(rn);

	    Dwarf_Die l_return_type, r_return_type;
	    bool l_return_type_is_void = !die_die_attribute(l, DW_AT_type,
							    l_return_type);
	    bool r_return_type_is_void = !die_die_attribute(r, DW_AT_type,
							    r_return_type);
	    if (l_return_type_is_void != r_return_type_is_void
		|| (!l_return_type_is_void
		    && !compare_dies(ctxt,
				     &l_return_type, &r_return_type,
				     aggregates_being_compared,
				     update_canonical_dies_on_the_fly)))
	      result = false;
	    else
	      {
		Dwarf_Die l_child, r_child;
		bool found_l_child, found_r_child;
		for (found_l_child = dwarf_child(l, &l_child) == 0,
		       found_r_child = dwarf_child(r, &r_child) == 0;
		     found_l_child && found_r_child;
		     found_l_child = dwarf_siblingof(&l_child,
						     &l_child) == 0,
		       found_r_child = dwarf_siblingof(&r_child,
						       &r_child)==0)
		  {
		    int l_child_tag = dwarf_tag(&l_child);
		    int r_child_tag = dwarf_tag(&r_child);
		    if (l_child_tag != r_child_tag
			|| (l_child_tag == DW_TAG_formal_parameter
			    && !compare_dies(ctxt, &l_child, &r_child,
					     aggregates_being_compared,
					     update_canonical_dies_on_the_fly)))
		      {
			result = false;
			break;
		      }
		  }
		if (found_l_child != found_r_child)
		  result = false;
	      }

	    aggregates_being_compared.erase(ln);
	    aggregates_being_compared.erase(rn);
	  }
      }
      break;

    case DW_TAG_formal_parameter:
      {
	Dwarf_Die l_type, r_type;
	bool l_type_is_void = !die_die_attribute(l, DW_AT_type, l_type);
	bool r_type_is_void = !die_die_attribute(r, DW_AT_type, r_type);
	if ((l_type_is_void != r_type_is_void)
	    || !compare_dies(ctxt, &l_type, &r_type,
			     aggregates_being_compared,
			     update_canonical_dies_on_the_fly))
	  result = false;
      }
      break;

    case DW_TAG_variable:
    case DW_TAG_member:
      if (compare_as_decl_dies(l, r))
	{
	  // Compare the offsets of the data members
	  if (l_tag == DW_TAG_member)
	    {
	      int64_t l_offset_in_bits = 0, r_offset_in_bits = 0;
	      die_member_offset(ctxt, l, l_offset_in_bits);
	      die_member_offset(ctxt, r, r_offset_in_bits);
	      if (l_offset_in_bits != r_offset_in_bits)
		result = false;
	    }
	  if (result)
	    {
	      // Compare the types of the data members or variables.
	      Dwarf_Die l_type, r_type;
	      ABG_ASSERT(die_die_attribute(l, DW_AT_type, l_type));
	      ABG_ASSERT(die_die_attribute(r, DW_AT_type, r_type));
	      if (aggregates_being_compared.size () < 5)
		{
		  if (!compare_dies(ctxt, &l_type, &r_type,
				    aggregates_being_compared,
				    update_canonical_dies_on_the_fly))
		    result = false;
		}
	      else
		{
		  if (!compare_as_type_dies(&l_type, &r_type)
		      ||!compare_as_decl_dies(&l_type, &r_type))
		    return false;
		}
	    }
	}
      else
	result = false;
      break;

    case DW_TAG_class_type:
    case DW_TAG_enumerator:
    case DW_TAG_packed_type:
    case DW_TAG_set_type:
    case DW_TAG_file_type:
    case DW_TAG_ptr_to_member_type:
    case DW_TAG_thrown_type:
    case DW_TAG_interface_type:
    case DW_TAG_unspecified_type:
    case DW_TAG_shared_type:
    case DW_TAG_compile_unit:
    case DW_TAG_namespace:
    case DW_TAG_module:
    case DW_TAG_constant:
    case DW_TAG_partial_unit:
    case DW_TAG_imported_unit:
    case DW_TAG_dwarf_procedure:
    case DW_TAG_imported_declaration:
    case DW_TAG_entry_point:
    case DW_TAG_label:
    case DW_TAG_lexical_block:
    case DW_TAG_unspecified_parameters:
    case DW_TAG_variant:
    case DW_TAG_common_block:
    case DW_TAG_common_inclusion:
    case DW_TAG_inheritance:
    case DW_TAG_inlined_subroutine:
    case DW_TAG_with_stmt:
    case DW_TAG_access_declaration:
    case DW_TAG_catch_block:
    case DW_TAG_friend:
    case DW_TAG_namelist:
    case DW_TAG_namelist_item:
    case DW_TAG_template_type_parameter:
    case DW_TAG_template_value_parameter:
    case DW_TAG_try_block:
    case DW_TAG_variant_part:
    case DW_TAG_imported_module:
    case DW_TAG_condition:
    case DW_TAG_type_unit:
    case DW_TAG_template_alias:
    case DW_TAG_lo_user:
    case DW_TAG_MIPS_loop:
    case DW_TAG_format_label:
    case DW_TAG_function_template:
    case DW_TAG_class_template:
    case DW_TAG_GNU_BINCL:
    case DW_TAG_GNU_EINCL:
    case DW_TAG_GNU_template_template_param:
    case DW_TAG_GNU_template_parameter_pack:
    case DW_TAG_GNU_formal_parameter_pack:
    case DW_TAG_GNU_call_site:
    case DW_TAG_GNU_call_site_parameter:
    case DW_TAG_hi_user:
      ABG_ASSERT_NOT_REACHED;
    }

  if (result == true
      && update_canonical_dies_on_the_fly
      && is_canonicalizeable_type_tag(l_tag))
    {
      // If 'l' has no canonical DIE and if 'r' has one, then propagage
      // the canonical DIE of 'r' to 'l'.
      //
      // In case 'r' has no canonical DIE, then compute it, and then
      // propagate that canonical DIE to 'r'.
      die_source l_source = NO_DEBUG_INFO_DIE_SOURCE,
	r_source = NO_DEBUG_INFO_DIE_SOURCE;
      ABG_ASSERT(ctxt.get_die_source(l, l_source));
      ABG_ASSERT(ctxt.get_die_source(r, r_source));
      if (!l_has_canonical_die_offset
	  // A DIE can be equivalent only to another DIE of the same
	  // source.
	  && l_source == r_source)
	{
	  if (!r_has_canonical_die_offset)
	    ctxt.compute_canonical_die_offset(r, r_canonical_die_offset,
					      /*die_as_type=*/true);
	  ABG_ASSERT(r_canonical_die_offset);
	  ctxt.set_canonical_die_offset(l, r_canonical_die_offset,
					/*die_as_type=*/true);
	}
    }
  return result;
}

/// Compare two DIEs emitted by a C compiler.
///
/// @param ctxt the read context used to load the DWARF information.
///
/// @param l the left-hand-side argument of this comparison operator.
///
/// @param r the righ-hand-side argument of this comparison operator.
///
/// @param update_canonical_dies_on_the_fly if yes, then this function
/// updates the canonical DIEs of sub-type DIEs of 'l' and 'r', while
/// comparing l and r.  This helps in making so that sub-type DIEs of
/// 'l' and 'r' are compared structurally only once.  This is how we
/// turn this exponential comparison problem into a problem that is a
/// closer to a linear one.
///
/// @return true iff @p l equals @p r.
static bool
compare_dies(const read_context& ctxt,
	     Dwarf_Die *l, Dwarf_Die *r,
	     bool update_canonical_dies_on_the_fly)
{
  istring_set_type aggregates_being_compared;
  return compare_dies(ctxt, l, r, aggregates_being_compared,
		      update_canonical_dies_on_the_fly);
}

// ----------------------------------
// </die comparison engine>
// ---------------------------------

/// Get the point where a DW_AT_import DIE is used to import a given
/// (unit) DIE, between two DIEs.
///
/// @param ctxt the dwarf reading context to consider.
///
/// @param partial_unit_offset the imported unit for which we want to
/// know the insertion point.  This is usually a partial unit (with
/// tag DW_TAG_partial_unit) but it does not necessarily have to be
/// so.
///
/// @param first_die_offset the offset of the DIE from which this
/// function starts looking for the import point of
/// @partial_unit_offset.  Note that this offset is excluded from the
/// set of potential solutions.
///
/// @param first_die_cu_offset the offset of the (compilation) unit
/// that @p first_die_cu_offset belongs to.
///
/// @param source where the DIE of first_die_cu_offset unit comes
/// from.
///
/// @param last_die_offset the offset of the last DIE of the up to
/// which this function looks for the import point of @p
/// partial_unit_offset.  Note that this offset is excluded from the
/// set of potential solutions.
///
/// @param imported_point_offset.  The resulting
/// imported_point_offset.  Note that if the imported DIE @p
/// partial_unit_offset is not found between @p first_die_offset and
/// @p last_die_offset, this parameter is left untouched by this
/// function.
///
/// @return true iff an imported unit is found between @p
/// first_die_offset and @p last_die_offset.
static bool
find_import_unit_point_between_dies(const read_context& ctxt,
				    size_t		partial_unit_offset,
				    Dwarf_Off		first_die_offset,
				    Dwarf_Off		first_die_cu_offset,
				    die_source		source,
				    size_t		last_die_offset,
				    size_t&		imported_point_offset)
{
  const tu_die_imported_unit_points_map_type& tu_die_imported_unit_points_map =
    ctxt.tu_die_imported_unit_points_map(source);

  tu_die_imported_unit_points_map_type::const_iterator iter =
    tu_die_imported_unit_points_map.find(first_die_cu_offset);

  ABG_ASSERT(iter != tu_die_imported_unit_points_map.end());

  const imported_unit_points_type& imported_unit_points = iter->second;
  if (imported_unit_points.empty())
    return false;

  imported_unit_points_type::const_iterator b = imported_unit_points.begin();
  imported_unit_points_type::const_iterator e = imported_unit_points.end();

  find_lower_bound_in_imported_unit_points(imported_unit_points,
					   first_die_offset,
					   b);

  if (last_die_offset != static_cast<size_t>(-1))
    find_lower_bound_in_imported_unit_points(imported_unit_points,
					     last_die_offset,
					     e);

  if (e != imported_unit_points.end())
    {
      for (imported_unit_points_type::const_iterator i = e; i >= b; --i)
	if (i->imported_unit_die_off == partial_unit_offset)
	  {
	    imported_point_offset = i->offset_of_import ;
	    return true;
	  }

      for (imported_unit_points_type::const_iterator i = e; i >= b; --i)
	{
	  if (find_import_unit_point_between_dies(ctxt,
						  partial_unit_offset,
						  i->imported_unit_child_off,
						  i->imported_unit_cu_off,
						  i->imported_unit_die_source,
						  /*(Dwarf_Off)*/-1,
						  imported_point_offset))
	    return true;
	}
    }
  else
    {
      for (imported_unit_points_type::const_iterator i = b; i != e; ++i)
	if (i->imported_unit_die_off == partial_unit_offset)
	  {
	    imported_point_offset = i->offset_of_import ;
	    return true;
	  }

      for (imported_unit_points_type::const_iterator i = b; i != e; ++i)
	{
	  if (find_import_unit_point_between_dies(ctxt,
						  partial_unit_offset,
						  i->imported_unit_child_off,
						  i->imported_unit_cu_off,
						  i->imported_unit_die_source,
						  /*(Dwarf_Off)*/-1,
						  imported_point_offset))
	    return true;
	}
    }

  return false;
}

/// In the current translation unit, get the last point where a
/// DW_AT_import DIE is used to import a given (unit) DIE, before a
/// given DIE is found.  That given DIE is called the limit DIE.
///
/// Said otherwise, this function returns the last import point of a
/// unit, before a limit.
///
/// @param ctxt the dwarf reading context to consider.
///
/// @param partial_unit_offset the imported unit for which we want to
/// know the insertion point of.  This is usually a partial unit (with
/// tag DW_TAG_partial_unit) but it does not necessarily have to be
/// so.
///
/// @param where_offset the offset of the limit DIE.
///
/// @param imported_point_offset.  The resulting imported_point_offset.
/// Note that if the imported DIE @p partial_unit_offset is not found
/// before @p die_offset, this is set to the last @p
/// partial_unit_offset found under @p parent_die.
///
/// @return true iff an imported unit is found before @p die_offset.
/// Note that if an imported unit is found after @p die_offset then @p
/// imported_point_offset is set and the function return false.
static bool
find_import_unit_point_before_die(const read_context&	ctxt,
				  size_t		partial_unit_offset,
				  size_t		where_offset,
				  size_t&		imported_point_offset)
{
  size_t import_point_offset = 0;
  Dwarf_Die first_die_of_tu;

  if (dwarf_child(const_cast<Dwarf_Die*>(ctxt.cur_tu_die()),
		  &first_die_of_tu) != 0)
    return false;

  Dwarf_Die cu_die_memory;
  Dwarf_Die *cu_die;

  cu_die = dwarf_diecu(const_cast<Dwarf_Die*>(&first_die_of_tu),
		       &cu_die_memory, 0, 0);

  if (find_import_unit_point_between_dies(ctxt, partial_unit_offset,
					  dwarf_dieoffset(&first_die_of_tu),
					  dwarf_dieoffset(cu_die),
					  /*source=*/PRIMARY_DEBUG_INFO_DIE_SOURCE,
					  where_offset,
					  import_point_offset))
    {
      imported_point_offset = import_point_offset;
      return true;
    }

  if (import_point_offset)
    {
      imported_point_offset = import_point_offset;
      return true;
    }

  return false;
}

/// Return the parent DIE for a given DIE.
///
/// Note that the function build_die_parent_map() must have been
/// called before this one can work.  This function either succeeds or
/// aborts the current process.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE for which we want the parent.
///
/// @param parent_die the output parameter set to the parent die of
/// @p die.  Its memory must be allocated and handled by the caller.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return true if the function could get a parent DIE, false
/// otherwise.
static bool
get_parent_die(const read_context&	ctxt,
	       Dwarf_Die*		die,
	       Dwarf_Die&		parent_die,
	       size_t			where_offset)
{
  ABG_ASSERT(ctxt.dwarf());

  die_source source = NO_DEBUG_INFO_DIE_SOURCE;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  const offset_offset_map_type& m = ctxt.die_parent_map(source);
  offset_offset_map_type::const_iterator i = m.find(dwarf_dieoffset(die));

  if (i == m.end())
    return false;

  switch (source)
    {
    case PRIMARY_DEBUG_INFO_DIE_SOURCE:
      ABG_ASSERT(dwarf_offdie(ctxt.dwarf(), i->second, &parent_die));
      break;
    case ALT_DEBUG_INFO_DIE_SOURCE:
      ABG_ASSERT(dwarf_offdie(ctxt.alt_dwarf(), i->second, &parent_die));
      break;
    case TYPE_UNIT_DIE_SOURCE:
      ABG_ASSERT(dwarf_offdie_types(ctxt.dwarf(), i->second, &parent_die));
      break;
    case NO_DEBUG_INFO_DIE_SOURCE:
    case NUMBER_OF_DIE_SOURCES:
      ABG_ASSERT_NOT_REACHED;
    }

  if (dwarf_tag(&parent_die) == DW_TAG_partial_unit)
    {
      if (where_offset == 0)
	{
	  parent_die = *ctxt.cur_tu_die();
	  return true;
	}
      size_t import_point_offset = 0;
      bool found =
	find_import_unit_point_before_die(ctxt,
					  dwarf_dieoffset(&parent_die),
					  where_offset,
					  import_point_offset);
      if (!found)
	// It looks like parent_die (which comes from the alternate
	// debug info file) hasn't been imported into this TU.  So,
	// Let's assume its logical parent is the DIE of the current
	// TU.
	parent_die = *ctxt.cur_tu_die();
      else
	{
	  ABG_ASSERT(import_point_offset);
	  Dwarf_Die import_point_die;
	  ABG_ASSERT(dwarf_offdie(ctxt.dwarf(),
			      import_point_offset,
			      &import_point_die));
	  return get_parent_die(ctxt, &import_point_die,
				parent_die, where_offset);
	}
    }

  return true;
}

/// Get the DIE representing the scope of a given DIE.
///
/// Please note that when the DIE we are looking at has a
/// DW_AT_specification or DW_AT_abstract_origin attribute, the scope
/// DIE is the parent DIE of the DIE referred to by that attribute.
/// This is the only case where a scope DIE is different from the
/// parent DIE of a given DIE.
///
/// Also note that if the current translation unit is from C, then
/// this returns the global scope.
///
/// @param ctxt the reading context to use.
///
/// @param die the DIE to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @param scope_die out parameter.  This is set to the resulting
/// scope DIE iff the function returns true.
static bool
get_scope_die(const read_context&	ctxt,
	      Dwarf_Die*		die,
	      size_t			where_offset,
	      Dwarf_Die&		scope_die)
{
  if (is_c_language(ctxt.cur_transl_unit()->get_language()))
    {
      ABG_ASSERT(dwarf_tag(die) != DW_TAG_member);
      return dwarf_diecu(die, &scope_die, 0, 0);
    }

  Dwarf_Die logical_parent_die;
  if (die_die_attribute(die, DW_AT_specification,
			logical_parent_die, false)
      || die_die_attribute(die, DW_AT_abstract_origin,
			   logical_parent_die, false))
    return get_scope_die(ctxt, &logical_parent_die, where_offset, scope_die);

  if (!get_parent_die(ctxt, die, scope_die, where_offset))
    return false;

  if (dwarf_tag(&scope_die) == DW_TAG_subprogram
      || dwarf_tag(&scope_die) == DW_TAG_subroutine_type
      || dwarf_tag(&scope_die) == DW_TAG_array_type)
    return get_scope_die(ctxt, &scope_die, where_offset, scope_die);

  return true;
}

/// Return the abigail IR node representing the scope of a given DIE.
///
/// Note that it is the logical scope that is returned.  That is, if
/// the DIE has a DW_AT_specification or DW_AT_abstract_origin
/// attribute, it's the scope of the referred-to DIE (via these
/// attributes) that is returned.
///
/// Also note that if the current translation unit is from C, then
/// this returns the global scope.
///
/// @param ctxt the dwarf reading context to use.
///
/// @param die the DIE to get the scope for.
///
/// @param called_from_public_decl is true if this function has been
/// initially called within the context of a public decl.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
static scope_decl_sptr
get_scope_for_die(read_context& ctxt,
		  Dwarf_Die*	die,
		  bool		called_for_public_decl,
		  size_t	where_offset)
{
  die_source source_of_die;
  ABG_ASSERT(ctxt.get_die_source(die, source_of_die));

  if (is_c_language(ctxt.cur_transl_unit()->get_language()))
    {
      ABG_ASSERT(dwarf_tag(die) != DW_TAG_member);
      return ctxt.global_scope();
    }

  Dwarf_Die cloned_die;
  if (die_die_attribute(die, DW_AT_specification, cloned_die, false)
      || die_die_attribute(die, DW_AT_abstract_origin, cloned_die, false))
    return get_scope_for_die(ctxt, &cloned_die,
			     called_for_public_decl,
			     where_offset);

  Dwarf_Die parent_die;

  if (!get_parent_die(ctxt, die, parent_die, where_offset))
    return ctxt.nil_scope();

  if (dwarf_tag(&parent_die) == DW_TAG_compile_unit
      || dwarf_tag(&parent_die) == DW_TAG_partial_unit
      || dwarf_tag(&parent_die) == DW_TAG_type_unit)
    {
      if (dwarf_tag(&parent_die) == DW_TAG_partial_unit
	  || dwarf_tag(&parent_die) == DW_TAG_type_unit)
	{
	  ABG_ASSERT(source_of_die == ALT_DEBUG_INFO_DIE_SOURCE
		 || source_of_die == TYPE_UNIT_DIE_SOURCE);
	  return ctxt.cur_transl_unit()->get_global_scope();
	}

      // For top level DIEs like DW_TAG_compile_unit, we just want to
      // return the global scope for the corresponding translation
      // unit.  This must have been set by
      // build_translation_unit_and_add_to_ir if we already started to
      // build the translation unit of parent_die.  Otherwise, just
      // return the global scope of the current translation unit.
      die_tu_map_type::const_iterator i =
	ctxt.die_tu_map().find(dwarf_dieoffset(&parent_die));
      if (i != ctxt.die_tu_map().end())
	return i->second->get_global_scope();
      return ctxt.cur_transl_unit()->get_global_scope();
    }

  scope_decl_sptr s;
  type_or_decl_base_sptr d;
  if (dwarf_tag(&parent_die) == DW_TAG_subprogram
      || dwarf_tag(&parent_die) == DW_TAG_array_type)
    // this is an entity defined in a scope that is a function.
    // Normally, I would say that this should be dropped.  But I have
    // seen a case where a typedef DIE needed by a function parameter
    // was defined right before the parameter, under the scope of the
    // function.  Yeah, weird.  So if I drop the typedef DIE, I'd drop
    // the function parm too.  So for that case, let's say that the
    // scope is the scope of the function itself.  Note that this is
    // an error of the DWARF emitter.  We should never see this DIE in
    // this context.
    {
      scope_decl_sptr s = get_scope_for_die(ctxt, &parent_die,
					    called_for_public_decl,
					    where_offset);
      if (is_anonymous_type_die(die))
	// For anonymous type that have nothing to do in a function or
	// array type context, let's put it in the containing
	// namespace.  That is, do not let it be in a containing class
	// or union where it has nothing to do.
	while (is_class_or_union_type(s))
	  {
	    if (!get_parent_die(ctxt, &parent_die, parent_die, where_offset))
	      return ctxt.nil_scope();
	    s = get_scope_for_die(ctxt, &parent_die,
				  called_for_public_decl,
				  where_offset);
	  }
      return s;
    }
  else
    d = build_ir_node_from_die(ctxt, &parent_die,
			       called_for_public_decl,
			       where_offset);
  s =  dynamic_pointer_cast<scope_decl>(d);
  if (!s)
    // this is an entity defined in someting that is not a scope.
    // Let's drop it.
    return ctxt.nil_scope();

  class_decl_sptr cl = dynamic_pointer_cast<class_decl>(d);
  if (cl && cl->get_is_declaration_only())
    {
      scope_decl_sptr scop (cl->get_definition_of_declaration());
      if (scop)
	s = scop;
      else
	s = cl;
    }
  return s;
}

/// Convert a DWARF constant representing the value of the
/// DW_AT_language property into the translation_unit::language
/// enumerator.
///
/// @param l the DWARF constant to convert.
///
/// @return the resulting translation_unit::language enumerator.
static translation_unit::language
dwarf_language_to_tu_language(size_t l)
{
  switch (l)
    {
    case DW_LANG_C89:
      return translation_unit::LANG_C89;
    case DW_LANG_C:
      return translation_unit::LANG_C;
    case DW_LANG_Ada83:
      return translation_unit::LANG_Ada83;
    case DW_LANG_C_plus_plus:
      return translation_unit::LANG_C_plus_plus;
    case DW_LANG_Cobol74:
      return translation_unit::LANG_Cobol74;
    case DW_LANG_Cobol85:
      return translation_unit::LANG_Cobol85;
    case DW_LANG_Fortran77:
      return translation_unit::LANG_Fortran77;
    case DW_LANG_Fortran90:
      return translation_unit::LANG_Fortran90;
    case DW_LANG_Pascal83:
      return translation_unit::LANG_Pascal83;
    case DW_LANG_Modula2:
      return translation_unit::LANG_Modula2;
    case DW_LANG_Java:
      return translation_unit::LANG_Java;
    case DW_LANG_C99:
      return translation_unit::LANG_C99;
    case DW_LANG_Ada95:
      return translation_unit::LANG_Ada95;
    case DW_LANG_Fortran95:
      return translation_unit::LANG_Fortran95;
    case DW_LANG_PL1:
      return translation_unit::LANG_PL1;
    case DW_LANG_ObjC:
      return translation_unit::LANG_ObjC;
    case DW_LANG_ObjC_plus_plus:
      return translation_unit::LANG_ObjC_plus_plus;

#ifdef HAVE_DW_LANG_Rust_enumerator
    case DW_LANG_Rust:
      return translation_unit::LANG_Rust;
#endif

#ifdef HAVE_DW_LANG_UPC_enumerator
    case DW_LANG_UPC:
      return translation_unit::LANG_UPC;
#endif

#ifdef HAVE_DW_LANG_D_enumerator
    case DW_LANG_D:
      return translation_unit::LANG_D;
#endif

#ifdef HAVE_DW_LANG_Python_enumerator
    case DW_LANG_Python:
      return translation_unit::LANG_Python;
#endif

#ifdef HAVE_DW_LANG_Go_enumerator
    case DW_LANG_Go:
      return translation_unit::LANG_Go;
#endif

#ifdef HAVE_DW_LANG_C11_enumerator
    case DW_LANG_C11:
      return translation_unit::LANG_C11;
#endif

#ifdef HAVE_DW_LANG_C_plus_plus_03_enumerator
      case DW_LANG_C_plus_plus_03:
	return translation_unit::LANG_C_plus_plus_03;
#endif

#ifdef HAVE_DW_LANG_C_plus_plus_11_enumerator
    case DW_LANG_C_plus_plus_11:
      return translation_unit::LANG_C_plus_plus_11;
#endif

#ifdef HAVE_DW_LANG_C_plus_plus_14_enumerator
    case DW_LANG_C_plus_plus_14:
      return translation_unit::LANG_C_plus_plus_14;
#endif

#ifdef HAVE_DW_LANG_Mips_Assembler_enumerator
    case DW_LANG_Mips_Assembler:
      return translation_unit::LANG_Mips_Assembler;
#endif

    default:
      return translation_unit::LANG_UNKNOWN;
    }
}

/// Get the default array lower bound value as defined by the DWARF
/// specification, version 4, depending on the language of the
/// translation unit.
///
/// @param l the language of the translation unit.
///
/// @return the default array lower bound value.
static uint64_t
get_default_array_lower_bound(translation_unit::language l)
{
  int value = 0;
  switch (l)
    {
    case translation_unit::LANG_UNKNOWN:
      value = 0;
      break;
    case translation_unit::LANG_Cobol74:
    case translation_unit::LANG_Cobol85:
      value = 1;
      break;
    case translation_unit::LANG_C89:
    case translation_unit::LANG_C99:
    case translation_unit::LANG_C11:
    case translation_unit::LANG_C:
    case translation_unit::LANG_C_plus_plus_03:
    case translation_unit::LANG_C_plus_plus_11:
    case translation_unit::LANG_C_plus_plus_14:
    case translation_unit::LANG_C_plus_plus:
    case translation_unit::LANG_ObjC:
    case translation_unit::LANG_ObjC_plus_plus:
    case translation_unit::LANG_Rust:
      value = 0;
      break;
    case translation_unit::LANG_Fortran77:
    case translation_unit::LANG_Fortran90:
    case translation_unit::LANG_Fortran95:
    case translation_unit::LANG_Ada83:
    case translation_unit::LANG_Ada95:
    case translation_unit::LANG_Pascal83:
    case translation_unit::LANG_Modula2:
      value = 1;
      break;
    case translation_unit::LANG_Java:
      value = 0;
      break;
    case translation_unit::LANG_PL1:
      value = 1;
      break;
    case translation_unit::LANG_UPC:
    case translation_unit::LANG_D:
    case translation_unit::LANG_Python:
    case translation_unit::LANG_Go:
    case translation_unit::LANG_Mips_Assembler:
      value = 0;
      break;
    }

  return value;
}

/// For a given offset, find the lower bound of a sorted vector of
/// imported unit point offset.
///
/// The lower bound is the smallest point (the point with the smallest
/// offset) which is the greater than a given offset.
///
/// @param imported_unit_points_type the sorted vector  of imported
/// unit points.
///
/// @param val the offset to consider when looking for the lower
/// bound.
///
/// @param r an iterator to the lower bound found.  This parameter is
/// set iff the function returns true.
///
/// @return true iff the lower bound has been found.
static bool
find_lower_bound_in_imported_unit_points(const imported_unit_points_type& p,
					 Dwarf_Off val,
					 imported_unit_points_type::const_iterator& r)
{
  imported_unit_point v(val);
  imported_unit_points_type::const_iterator result =
    std::lower_bound(p.begin(), p.end(), v);

  bool is_ok = result != p.end();

  if (is_ok)
    r = result;

  return is_ok;
}

/// Given a DW_TAG_compile_unit, build and return the corresponding
/// abigail::translation_unit ir node.  Note that this function
/// recursively reads the children dies of the current DIE and
/// populates the resulting translation unit.
///
/// @param ctxt the read_context to use.
///
/// @param die the DW_TAG_compile_unit DIE to consider.
///
/// @param address_size the size of the addresses expressed in this
/// translation unit in general.
///
/// @return a pointer to the resulting translation_unit.
static translation_unit_sptr
build_translation_unit_and_add_to_ir(read_context&	ctxt,
				     Dwarf_Die*	die,
				     char		address_size)
{
  translation_unit_sptr result;

  if (!die)
    return result;
  ABG_ASSERT(dwarf_tag(die) == DW_TAG_compile_unit);

  // Clear the part of the context that is dependent on the translation
  // unit we are reading.
  ctxt.clear_per_translation_unit_data();

  ctxt.cur_tu_die(die);

  string path = die_string_attribute(die, DW_AT_name);
  string compilation_dir = die_string_attribute(die, DW_AT_comp_dir);

  // See if the same translation unit exits already in the current
  // corpus.  Sometimes, the same translation unit can be present
  // several times in the same debug info.  The content of the
  // different instances of the translation unit are different.  So to
  // represent that, we are going to re-use the same translation
  // unit.  That is, it's going to be the union of all the translation
  // units of the same path.
  {
    string abs_path = compilation_dir + "/" + path;
    result = ctxt.current_corpus()->find_translation_unit(abs_path);
  }

  if (!result)
    {
      result.reset(new translation_unit(ctxt.env(),
					path,
					address_size));
      result->set_compilation_dir_path(compilation_dir);
      ctxt.current_corpus()->add(result);
      uint64_t l = 0;
      die_unsigned_constant_attribute(die, DW_AT_language, l);
      result->set_language(dwarf_language_to_tu_language(l));
    }

  ctxt.cur_transl_unit(result);
  ctxt.die_tu_map()[dwarf_dieoffset(die)] = result;

  Dwarf_Die child;
  if (dwarf_child(die, &child) != 0)
    return result;

  result->set_is_constructed(false);

  do
    build_ir_node_from_die(ctxt, &child,
			   die_is_public_decl(&child),
			   dwarf_dieoffset(&child));
  while (dwarf_siblingof(&child, &child) == 0);

  if (!ctxt.var_decls_to_re_add_to_tree().empty())
    for (list<var_decl_sptr>::const_iterator v =
	   ctxt.var_decls_to_re_add_to_tree().begin();
	 v != ctxt.var_decls_to_re_add_to_tree().end();
	 ++v)
      {
	if (is_member_decl(*v))
	  continue;

	ABG_ASSERT((*v)->get_scope());
	string demangled_name =
	  demangle_cplus_mangled_name((*v)->get_linkage_name());
	if (!demangled_name.empty())
	  {
	    std::list<string> fqn_comps;
	    fqn_to_components(demangled_name, fqn_comps);
	    string mem_name = fqn_comps.back();
	    fqn_comps.pop_back();
	    class_decl_sptr class_type;
	    string ty_name;
	    if (!fqn_comps.empty())
	      {
		ty_name = components_to_type_name(fqn_comps);
		class_type =
		  lookup_class_type(ty_name, *ctxt.cur_transl_unit());
	      }
	    if (class_type)
	      {
		// So we are seeing a member variable for which there
		// is a global variable definition DIE not having a
		// reference attribute pointing back to the member
		// variable declaration DIE.  Thus remove the global
		// variable definition from its current non-class
		// scope ...
		decl_base_sptr d;
		if ((d = lookup_var_decl_in_scope(mem_name, class_type)))
		  // This is the data member with the same name in cl.
		  // We just need to flag it as static.
		  ;
		else
		  {
		    // In this case there is no data member with the
		    // same name in cl already.  Let's add it there then
		    // ...
		    remove_decl_from_scope(*v);
		    d = add_decl_to_scope(*v, class_type);
		  }

		ABG_ASSERT(dynamic_pointer_cast<var_decl>(d));
		// Let's flag the data member as static.
		set_member_is_static(d, true);
	      }
	  }
      }
  ctxt.var_decls_to_re_add_to_tree().clear();

  result->set_is_constructed(true);

  return result;
}

/// Build a abigail::namespace_decl out of a DW_TAG_namespace or
/// DW_TAG_module (for fortran) DIE.
///
/// Note that this function connects the DW_TAG_namespace to the IR
/// being currently created, reads the children of the DIE and
/// connects them to the IR as well.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.  Must be either DW_TAG_namespace
/// or DW_TAG_module.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting @ref abigail::namespace_decl or NULL if it
/// couldn't be created.
static namespace_decl_sptr
build_namespace_decl_and_add_to_ir(read_context&	ctxt,
				   Dwarf_Die*		die,
				   size_t		where_offset)
{
  namespace_decl_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_namespace && tag != DW_TAG_module)
    return result;

  scope_decl_sptr scope = get_scope_for_die(ctxt, die,
					    /*called_for_public_decl=*/false,
					    where_offset);

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  result.reset(new namespace_decl(ctxt.env(), name, loc));
  add_decl_to_scope(result, scope.get());
  ctxt.associate_die_to_decl(die, result, where_offset);

  Dwarf_Die child;
  if (dwarf_child(die, &child) != 0)
    return result;

  ctxt.scope_stack().push(result.get());
  do
    build_ir_node_from_die(ctxt, &child,
			   /*called_from_public_decl=*/false,
			   where_offset);
  while (dwarf_siblingof(&child, &child) == 0);
  ctxt.scope_stack().pop();

  return result;
}

/// Build a @ref type_decl out of a DW_TAG_base_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DW_TAG_base_type to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return the resulting decl_base_sptr.
static type_decl_sptr
build_type_decl(read_context& ctxt, Dwarf_Die* die, size_t where_offset)
{
  type_decl_sptr result;

  if (!die)
    return result;
  ABG_ASSERT(dwarf_tag(die) == DW_TAG_base_type);

  uint64_t byte_size = 0, bit_size = 0;
  if (!die_unsigned_constant_attribute(die, DW_AT_byte_size, byte_size))
    if (!die_unsigned_constant_attribute(die, DW_AT_bit_size, bit_size))
      return result;

  if (bit_size == 0 && byte_size != 0)
    // Update the bit size.
    bit_size = byte_size * 8;

  string type_name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, type_name, linkage_name);

  if (byte_size == 0)
    {
      // The size of the type is zero, that must mean that we are
      // looking at the definition of the void type.
      if (type_name == "void")
	result = is_type_decl(build_ir_node_for_void_type(ctxt));
      else
	// A type of size zero that is not void? Hmmh, I am not sure
	// what that means.  Return nil for now.
	return result;
    }

  if (corpus_sptr corp = ctxt.should_reuse_type_from_corpus_group())
    {
      string normalized_type_name = type_name;
      integral_type int_type;
      if (parse_integral_type(type_name, int_type))
	normalized_type_name = int_type.to_string();
      result = lookup_basic_type(normalized_type_name, *corp);
    }

  if (!result)
    if (corpus_sptr corp = ctxt.current_corpus())
      result = lookup_basic_type(type_name, *corp);
  if (!result)
    result.reset(new type_decl(ctxt.env(), type_name, bit_size,
			       /*alignment=*/0, loc, linkage_name));
  ctxt.associate_die_to_type(die, result, where_offset);
  return result;
}

/// Build an enum_type_decl from a DW_TAG_enumeration_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.
///
/// @param scope the scope of the final enum.  Note that this function
/// does *NOT* add the built type to this scope.  The scope is just so
/// that the function knows how to name anonymous enums.
///
/// @return the built enum_type_decl or NULL if it could not be built.
static enum_type_decl_sptr
build_enum_type(read_context&	ctxt,
		Dwarf_Die*	die,
		scope_decl*	scope,
		size_t		where_offset)
{
  enum_type_decl_sptr result;
  if (!die)
    return result;

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_enumeration_type)
    return result;

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  bool enum_is_anonymous = false;
  // If the enum is anonymous, let's give it a name.
  if (name.empty())
    {
      name = get_internal_anonynous_die_base_name(die);
      ABG_ASSERT(!name.empty());
      // But we remember that the type is anonymous.
      enum_is_anonymous = true;

      if (size_t s = scope->get_num_anonymous_member_enums())
	name = build_internal_anonymous_die_name(name, s);
    }

  bool use_odr = ctxt.odr_is_relevant(die);
  // If the type has location, then associate it to its
  // representation.  This way, all occurences of types with the same
  // representation (name) and location can be later detected as being
  // for the same type.

  if (!enum_is_anonymous)
    {
      if (use_odr)
	{
	  if (enum_type_decl_sptr pre_existing_enum =
	      is_enum_type(ctxt.lookup_artifact_from_die(die)))
	    result = pre_existing_enum;
	}
      else if (corpus_sptr corp = ctxt.should_reuse_type_from_corpus_group())
	{
	  if (loc)
	    result = lookup_enum_type_per_location(loc.expand(), *corp);
	}
      else if (loc)
	{
	  if (enum_type_decl_sptr pre_existing_enum =
	      is_enum_type(ctxt.lookup_artifact_from_die(die)))
	    if (pre_existing_enum->get_location() == loc)
	      result = pre_existing_enum;
	}

      if (result)
	{
	  ctxt.associate_die_to_type(die, result, where_offset);
	  return result;
	}
    }
  // TODO: for anonymous enums, maybe have a map of loc -> enums so that
  // we can look them up?

  uint64_t size = 0;
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    size *= 8;

  // for now we consider that underlying types of enums are all anonymous
  bool enum_underlying_type_is_anonymous= true;
  string underlying_type_name;
  if (enum_underlying_type_is_anonymous)
    {
      underlying_type_name = "unnamed-enum";
      enum_underlying_type_is_anonymous = true;
    }
  else
    underlying_type_name = string("enum-") + name;
  underlying_type_name += "-underlying-type";

  enum_type_decl::enumerators enms;
  Dwarf_Die child;
  if (dwarf_child(die, &child) == 0)
    {
      do
	{
	  if (dwarf_tag(&child) != DW_TAG_enumerator)
	    continue;

	  string n, m;
	  location l;
	  die_loc_and_name(ctxt, &child, l, n, m);
	  uint64_t val = 0;
	  die_unsigned_constant_attribute(&child, DW_AT_const_value, val);
	  enms.push_back(enum_type_decl::enumerator(ctxt.env(), n, val));
	}
      while (dwarf_siblingof(&child, &child) == 0);
    }

  // DWARF up to version 4 (at least) doesn't seem to carry the
  // underlying type, so let's create an artificial one here, which
  // sole purpose is to be passed to the constructor of the
  // enum_type_decl type.
  type_decl_sptr t(new type_decl(ctxt.env(), underlying_type_name,
				 size, size, location()));
  t->set_is_anonymous(enum_underlying_type_is_anonymous);
  translation_unit_sptr tu = ctxt.cur_transl_unit();
  decl_base_sptr d =
    add_decl_to_scope(t, tu->get_global_scope().get());
  canonicalize(t);

  t = dynamic_pointer_cast<type_decl>(d);
  ABG_ASSERT(t);
  result.reset(new enum_type_decl(name, loc, t, enms, linkage_name));
  result->set_is_anonymous(enum_is_anonymous);
  ctxt.associate_die_to_type(die, result, where_offset);
  return result;
}

/// Once a function_decl has been built and added to a class as a
/// member function, this function updates the information of the
/// function_decl concerning the properties of its relationship with
/// the member class.  That is, it updates properties like
/// virtualness, access, constness, cdtorness, etc ...
///
/// @param die the DIE of the function_decl that has been just built.
///
/// @param f the function_decl that has just been built from @p die.
///
/// @param klass the @ref class_or_union that @p f belongs to.
///
/// @param ctxt the context used to read the ELF/DWARF information.
static void
finish_member_function_reading(Dwarf_Die*		  die,
			       const function_decl_sptr&  f,
			       const class_or_union_sptr& klass,
			       read_context&		  ctxt)
{
  ABG_ASSERT(klass);

  method_decl_sptr m = is_method_decl(f);
  ABG_ASSERT(m);

  method_type_sptr method_t = is_method_type(m->get_type());
  ABG_ASSERT(method_t);

  bool is_ctor = (f->get_name() == klass->get_name());
  bool is_dtor = (!f->get_name().empty()
		  && static_cast<string>(f->get_name())[0] == '~');
  bool is_virtual = die_is_virtual(die);
  int64_t vindex = -1;
  if (is_virtual)
    die_virtual_function_index(die, vindex);
  access_specifier access = private_access;
  if (class_decl_sptr c = is_class_type(klass))
    if (c->is_struct())
      access = public_access;
  die_access_specifier(die, access);

  bool is_static = false;
  {
    // Let's see if the first parameter is a pointer to an instance of
    // the same class type as the current class and has a
    // DW_AT_artificial attribute flag set.  We are not looking at
    // DW_AT_object_pointer (for DWARF 3) because it wasn't being
    // emitted in GCC 4_4, which was already DWARF 3.
    function_decl::parameter_sptr first_parm;
    if (!f->get_parameters().empty())
      first_parm = f->get_parameters()[0];

    bool is_artificial =
      first_parm && first_parm->get_artificial();;
    pointer_type_def_sptr this_ptr_type;
    type_base_sptr other_klass;

    if (is_artificial)
      this_ptr_type = is_pointer_type(first_parm->get_type());
    if (this_ptr_type)
      other_klass = this_ptr_type->get_pointed_to_type();
    // Sometimes, other_klass can be qualified; e.g, volatile.  In
    // that case, let's get the unqualified version of other_klass.
    if (qualified_type_def_sptr q = is_qualified_type(other_klass))
      other_klass = q->get_underlying_type();

    if (other_klass
	&& get_type_name(other_klass) == klass->get_qualified_name())
      ;
    else
      is_static = true;
  }
  set_member_access_specifier(m, access);
  if (vindex != -1)
    set_member_function_vtable_offset(m, vindex);
  set_member_function_is_virtual(m, is_virtual);
  set_member_is_static(m, is_static);
  set_member_function_is_ctor(m, is_ctor);
  set_member_function_is_dtor(m, is_dtor);
  set_member_function_is_const(m, method_t->get_is_const());

  ABG_ASSERT(is_member_function(m));

  if (is_virtual && !f->get_linkage_name().empty() && !f->get_symbol())
    {
      // This is a virtual member function which has a linkage name
      // but has no underlying symbol set.
      //
      // The underlying elf symbol to set to this function can show up
      // later in the DWARF input or it can be that, because of some
      // compiler optimization, the relation between this function and
      // its underlying elf symbol is simply not emitted in the DWARF.
      //
      // Let's thus schedule this function for a later fixup pass
      // (performed by
      // read_context::fixup_functions_with_no_symbols()) that will
      // set its underlying symbol.
      //
      // Note that if the underying symbol is encountered later in the
      // DWARF input, then the part of build_function_decl() that
      // updates the function to set its underlying symbol will
      // de-schedule this function wrt fixup pass.
      Dwarf_Off die_offset = dwarf_dieoffset(die);
      die_function_decl_map_type &fns_with_no_symbol =
	ctxt.die_function_decl_with_no_symbol_map();
      die_function_decl_map_type::const_iterator i =
	fns_with_no_symbol.find(die_offset);
      if (i == fns_with_no_symbol.end())
	fns_with_no_symbol[die_offset] = f;
    }

}

/// If a function DIE has attributes which have not yet been read and
/// added to the internal representation that represents that function
/// then read those extra attributes and update the internal
/// representation.
///
/// @param ctxt the read context to use.
///
/// @param die the function DIE to consider.
///
/// @param where_offset where we logical are, currently, in the stream
/// of DIEs.  If you don't know what this is, you can just set it to zero.
///
/// @param existing_fn the representation of the function to update.
///
/// @return the updated function  representation.
static function_decl_sptr
maybe_finish_function_decl_reading(read_context&		ctxt,
				   Dwarf_Die*			die,
				   size_t			where_offset,
				   const function_decl_sptr&	existing_fn)
{
  function_decl_sptr result = build_function_decl(ctxt, die,
						  where_offset,
						  existing_fn);

  return result;
}

/// Lookup a class or a typedef with a given qualified name in the
/// corpus that a given scope belongs to.
///
/// @param scope the scope to consider.
///
/// @param type_name the qualified name of the type to look for.
///
/// @return the typedef or class type found.
static type_base_sptr
lookup_class_or_typedef_from_corpus(scope_decl* scope, const string& type_name)
{
  string qname = build_qualified_name(scope, type_name);
  corpus* corp = scope->get_corpus();
  type_base_sptr result = lookup_class_or_typedef_type(qname, *corp);
  return result;
}

/// Lookup a class of typedef type from the current corpus being
/// constructed.
///
/// The type being looked for has the same name as a given DIE.
///
/// @param ctxt the reading context to use.
///
/// @param die the DIE which has the same name as the type we are
/// looking for.
///
/// @param called_for_public_decl whether this function is being
/// called from a a publicly defined declaration.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return the type found.
static type_base_sptr
lookup_class_or_typedef_from_corpus(read_context& ctxt,
				    Dwarf_Die* die,
				    bool called_for_public_decl,
				    size_t where_offset)
{
  if (!die)
    return class_decl_sptr();

  string class_name = die_string_attribute(die, DW_AT_name);
  if (class_name.empty())
    return class_decl_sptr();

  scope_decl_sptr scope = get_scope_for_die(ctxt, die,
					    called_for_public_decl,
					    where_offset);
  if (scope)
    return lookup_class_or_typedef_from_corpus(scope.get(), class_name);

  return type_base_sptr();
}

/// Lookup a class, typedef or enum type with a given qualified name
/// in the corpus that a given scope belongs to.
///
/// @param scope the scope to consider.
///
/// @param type_name the qualified name of the type to look for.
///
/// @return the typedef, enum or class type found.
static type_base_sptr
lookup_class_typedef_or_enum_type_from_corpus(scope_decl* scope,
					      const string& type_name)
{
  string qname = build_qualified_name(scope, type_name);
  corpus* corp = scope->get_corpus();
  type_base_sptr result = lookup_class_typedef_or_enum_type(qname, *corp);
  return result;
}

/// Lookup a class, typedef or enum type in a given scope, in the
/// corpus that scope belongs to.
///
/// @param die the DIE of the class, typedef or enum to lookup.
///
/// @param anonymous_member_type_idx if @p DIE represents an anonymous
/// type, this is the index of that anonymous type in its scope, in
/// case there are several anonymous types of the same kind in that
/// scope.
///
/// @param scope the scope in which to look the type for.
///
/// @return the typedef, enum or class type found.
static type_base_sptr
lookup_class_typedef_or_enum_type_from_corpus(Dwarf_Die* die,
					      size_t anonymous_member_type_idx,
					      scope_decl* scope)
{
  if (!die)
    return class_decl_sptr();

  string type_name = die_string_attribute(die, DW_AT_name);
  if (is_anonymous_type_die(die))
    type_name =
      get_internal_anonymous_die_name(die, anonymous_member_type_idx);

  if (type_name.empty())
    return class_decl_sptr();

  return lookup_class_typedef_or_enum_type_from_corpus(scope, type_name);
}

/// Test if a DIE represents a function that is a member of a given
/// class type.
///
/// @param ctxt the reading context.
///
/// @param function_die the DIE of the function to consider.
///
/// @param class_type the class type to consider.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return the method declaration corresponding to the member
/// function of @p class_type, iff @p function_die is for a member
/// function of @p class_type.
static method_decl_sptr
is_function_for_die_a_member_of_class(read_context& ctxt,
				      Dwarf_Die* function_die,
				      const class_or_union_sptr& class_type)
{
  type_or_decl_base_sptr artifact = ctxt.lookup_artifact_from_die(function_die);

  if (!artifact)
    return method_decl_sptr();

  method_decl_sptr method = is_method_decl(artifact);
  method_type_sptr method_type;

  if (method)
    method_type = method->get_type();
  else
    method_type = is_method_type(artifact);
  ABG_ASSERT(method_type);

  class_or_union_sptr method_class = method_type->get_class_type();
  ABG_ASSERT(method_class);

  string method_class_name = method_class->get_qualified_name(),
    class_type_name = class_type->get_qualified_name();

  if (method_class_name == class_type_name)
    {
      //ABG_ASSERT(class_type.get() == method_class.get());
      return method;
    }

  return method_decl_sptr();
}

/// If a given function DIE represents an existing member function of
/// a given class, then update that member function with new
/// properties present in the DIE.  Otherwise, if the DIE represents a
/// new member function that is not already present in the class then
/// add that new member function to the class.
///
/// @param ctxt the reading context.
///
/// @param function_die the DIE of the potential member function to
/// consider.
///
/// @param class_type the class type to consider.
///
/// @param called_from_public_decl is true iff this function was
/// called from a publicly defined and exported declaration.
///
/// @param where_offset where we are logically at in the DIE stream.
///
/// @return the method decl representing the member function.
static method_decl_sptr
add_or_update_member_function(read_context& ctxt,
			      Dwarf_Die* function_die,
			      const class_or_union_sptr& class_type,
			      bool called_from_public_decl,
			      size_t where_offset)
{
  method_decl_sptr method =
    is_function_for_die_a_member_of_class(ctxt, function_die, class_type);

  if (!method)
    method = is_method_decl(build_ir_node_from_die(ctxt, function_die,
						   class_type.get(),
						   called_from_public_decl,
						   where_offset));
  if (!method)
    return method_decl_sptr();

  finish_member_function_reading(function_die,
				 is_function_decl(method),
				 class_type, ctxt);
  return method;
}

/// Build a an IR node for class type from a DW_TAG_structure_type or
/// DW_TAG_class_type DIE and add that node to the ABI corpus being
/// currently built.
///
/// If the represents class type that already exists, then update the
/// existing class type with the new properties found in the DIE.
///
/// It meanst that this function can also update an existing
/// class_decl node with data members, member functions and other
/// properties coming from the DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read information from.  Must be either a
/// DW_TAG_structure_type or a DW_TAG_class_type.
///
/// @param scope a pointer to the scope_decl* under which this class
/// is to be added to.
///
/// @param is_struct whether the class was declared as a struct.
///
/// @param klass if non-null, this is a klass to append the members
/// to.  Otherwise, this function just builds the class from scratch.
///
/// @param called_from_public_decl set to true if this class is being
/// called from a "Public declaration like vars or public symbols".
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting class_type.
static class_decl_sptr
add_or_update_class_type(read_context&	 ctxt,
			 Dwarf_Die*	 die,
			 scope_decl*	 scope,
			 bool		 is_struct,
			 class_decl_sptr klass,
			 bool		 called_from_public_decl,
			 size_t	 where_offset)
{
  class_decl_sptr result;
  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_class_type && tag != DW_TAG_structure_type)
    return result;

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	class_decl_sptr class_type = is_class_type(i->second);
	ABG_ASSERT(class_type);
	return class_type;
      }
  }

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);
  bool is_declaration_only = die_is_declaration_only(die);

  bool is_anonymous = false;
  if (name.empty())
    {
      // So we are looking at an anonymous struct.  Let's
      // give it a name.
      name = get_internal_anonynous_die_base_name(die);
      ABG_ASSERT(!name.empty());
      // But we remember that the type is anonymous.
      is_anonymous = true;

      if (size_t s = scope->get_num_anonymous_member_classes())
	name = build_internal_anonymous_die_name(name, s);
    }

  if (!is_anonymous)
    {
      if (corpus_sptr corp = ctxt.should_reuse_type_from_corpus_group())
	{
	  if (loc)
	    // TODO: if there is only one class defined in the corpus
	    // for this location, then re-use it.  But if there are
	    // more than one, then do not re-use it, for now.
	    result = lookup_class_type_per_location(loc.expand(), *corp);
	  else
	    // TODO: if there is just one class for that name defined,
	    // then re-use it.  Otherwise, don't.
	    result = lookup_class_type(name, *corp);
	  if (result
	      // If we are seeing a declaration of a definition we
	      // already had, or if we are seing a type with the same
	      // declaration-only-ness that we had before, then keep
	      // the one we already had.
	      && (result->get_is_declaration_only() == is_declaration_only
		  || (!result->get_is_declaration_only()
		      && is_declaration_only)))
	    {
	      ctxt.associate_die_to_type(die, result, where_offset);
	      return result;
	    }
	  else
	    // We might be seeing the definition of a declaration we
	    // already had.  In that case, keep the definition and
	    // drop the declaration.
	    result.reset();
	}
    }

  // If we've already seen the same class as 'die', then let's re-use
  // that one, unless it's an anonymous class.  We can't really safely
  // re-use anonymous classes as they have no name, by construction.
  // What we can do, rather, is to reuse the typedef that name them,
  // when they do have a naming typedef.
  if (!is_anonymous)
    if (class_decl_sptr pre_existing_class =
	is_class_type(ctxt.lookup_type_artifact_from_die(die)))
      klass = pre_existing_class;

  uint64_t size = 0;
  die_size_in_bits(die, size);

  Dwarf_Die child;
  bool has_child = (dwarf_child(die, &child) == 0);

  decl_base_sptr res;
  if (klass)
    {
      res = result = klass;
      if (loc)
	result->set_location(loc);
    }
  else
    {
      result.reset(new class_decl(ctxt.env(), name, size,
				  /*alignment=*/0, is_struct, loc,
				  decl_base::VISIBILITY_DEFAULT));
      result->set_is_anonymous(is_anonymous);

      if (is_declaration_only)
	result->set_is_declaration_only(true);

      res = add_decl_to_scope(result, scope);
      result = dynamic_pointer_cast<class_decl>(res);
      ABG_ASSERT(result);
    }

  if (size)
    result->set_size_in_bits(size);

  ctxt.associate_die_to_type(die, result, where_offset);

  ctxt.maybe_schedule_declaration_only_class_for_resolution(result);

  if (!has_child)
    // TODO: set the access specifier for the declaration-only class
    // here.
    return result;

  ctxt.die_wip_classes_map(source)[dwarf_dieoffset(die)] = result;

  scope_decl_sptr scop =
    dynamic_pointer_cast<scope_decl>(res);
  ABG_ASSERT(scop);
  ctxt.scope_stack().push(scop.get());

  if (has_child)
    {
      int anonymous_member_class_index = -1;
      int anonymous_member_union_index = -1;
      int anonymous_member_enum_index = -1;

      do
	{
	  tag = dwarf_tag(&child);

	  // Handle base classes.
	  if (tag == DW_TAG_inheritance)
	    {
	      result->set_is_declaration_only(false);

	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      type_base_sptr base_type;
	      if (!(base_type =
		    lookup_class_or_typedef_from_corpus(ctxt, &type_die,
							called_from_public_decl,
							where_offset)))
		{
		  base_type =
		    is_type(build_ir_node_from_die(ctxt, &type_die,
						   called_from_public_decl,
						   where_offset));
		}
	      // Sometimes base_type can be a typedef.  Let's make
	      // sure that typedef is compatible with a class type.
	      class_decl_sptr b = is_compatible_with_class_type(base_type);
	      if (!b)
		continue;

	      access_specifier access =
		is_struct
		? public_access
		: private_access;

	      die_access_specifier(&child, access);

	      bool is_virt= die_is_virtual(&child);
	      int64_t offset = 0;
	      bool is_offset_present =
		die_member_offset(ctxt, &child, offset);

	      class_decl::base_spec_sptr base(new class_decl::base_spec
					      (b, access,
					       is_offset_present ? offset : -1,
					       is_virt));
	      if (b->get_is_declaration_only())
		ABG_ASSERT(ctxt.is_decl_only_class_scheduled_for_resolution(b));
	      if (result->find_base_class(b->get_qualified_name()))
		continue;
	      result->add_base_specifier(base);
	    }
	  // Handle data members.
	  else if (tag == DW_TAG_member
		   || tag == DW_TAG_variable)
	    {
	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      string n, m;
	      location loc;
	      die_loc_and_name(ctxt, &child, loc, n, m);
	      /// For now, we skip the hidden vtable pointer.
	      /// Currently, we're looking for a member starting with
	      /// "_vptr[^0-9a-zA-Z_]", which is what Clang and GCC
	      /// use as a name for the hidden vtable pointer.
	      if (n.substr(0, 5) == "_vptr"
		  && !std::isalnum(n.at(5))
		  && n.at(5) != '_')
		continue;

	      // If the variable is already a member of this class,
	      // move on.
	      if (lookup_var_decl_in_scope(n, result))
		continue;

	      int64_t offset_in_bits = 0;
	      bool is_laid_out = die_member_offset(ctxt, &child,
						   offset_in_bits);
	      // For now, is_static == !is_laid_out.  When we have
	      // templates, we'll try to be more specific.  For now,
	      // this approximation should do OK.
	      bool is_static = !is_laid_out;

	      if (is_static && variable_is_suppressed(ctxt,
						      result.get(),
						      &child))
		continue;

	      decl_base_sptr ty = is_decl(
					  build_ir_node_from_die(ctxt, &type_die,
								 called_from_public_decl,
								 where_offset));
	      type_base_sptr t = is_type(ty);
	      if (!t)
		continue;

	      // The call to build_ir_node_from_die above could have
	      // triggered the adding of a data member named 'n' into
	      // result.  So let's check again if the variable is
	      // already a member of this class.
	      if (lookup_var_decl_in_scope(n, result))
		continue;

	      if (!is_static)
		// We have a non-static data member.  So this class
		// cannot be a declaration-only class anymore, even if
		// some DWARF emitters might consider it otherwise.
		result->set_is_declaration_only(false);
	      access_specifier access =
		is_struct
		? public_access
		: private_access;

	      die_access_specifier(&child, access);

	      var_decl_sptr dm(new var_decl(n, t, loc, m));
	      result->add_data_member(dm, access, is_laid_out,
				      is_static, offset_in_bits);
	      ABG_ASSERT(has_scope(dm));
	      ctxt.associate_die_to_decl(&child, dm, where_offset,
					 /*associate_by_repr=*/false);
	    }
	  // Handle member functions;
	  else if (tag == DW_TAG_subprogram)
	    {
	      decl_base_sptr r =
		add_or_update_member_function(ctxt, &child, result,
					      called_from_public_decl,
					      where_offset);
	      if (function_decl_sptr f = is_function_decl(r))
		ctxt.associate_die_to_decl(&child, f, where_offset,
					   /*associate_by_repr=*/true);
	    }
	  // Handle member types
	  else if (die_is_type(&child))
	    {
	      // Track the anonymous type index in the current
	      // scope. Look for what this means by reading the
	      // comment of the function
	      // build_internal_anonymous_die_name.
	      int anonymous_member_type_index = 0;
	      if (is_anonymous_type_die(&child))
		{
		  // Update the anonymous type index.
		  if (die_is_class_type(&child))
		    anonymous_member_type_index =
		      ++anonymous_member_class_index;
		  else if (dwarf_tag(&child) == DW_TAG_union_type)
		    anonymous_member_type_index =
		      ++anonymous_member_union_index;
		  else if (dwarf_tag(&child) == DW_TAG_enumeration_type)
		    anonymous_member_type_index =
		      ++anonymous_member_enum_index;
		}
	      // if the type is not already a member of this class,
	      // then add it to the class.
	      if (!lookup_class_typedef_or_enum_type_from_corpus
		  (&child, anonymous_member_type_index, result.get()))
		build_ir_node_from_die(ctxt, &child, result.get(),
				       called_from_public_decl,
				       where_offset);
	    }
	} while (dwarf_siblingof(&child, &child) == 0);
    }

  ctxt.scope_stack().pop();

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	if (is_member_type(i->second))
	  set_member_access_specifier(res,
				      get_member_access_specifier(i->second));
	ctxt.die_wip_classes_map(source).erase(i);
      }
  }

  ctxt.maybe_schedule_declaration_only_class_for_resolution(result);
  return result;
}

/// Build an @ref union_decl from a DW_TAG_union_type DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE to read from.
///
/// @param scope the scope the resulting @ref union_decl belongs to.
///
/// @param union_type if this parameter is non-nil, then this function
/// updates the @ref union_decl that it points to, rather than
/// creating a new @ref union_decl.
///
/// @param called_from_public_decl is true if this function has been
/// initially called within the context of a public decl.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
static union_decl_sptr
add_or_update_union_type(read_context&	ctxt,
			 Dwarf_Die*	die,
			 scope_decl*	scope,
			 union_decl_sptr union_type,
			 bool		called_from_public_decl,
			 size_t	where_offset)
{
  union_decl_sptr result;
  if (!die)
    return result;

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_union_type)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));
  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	union_decl_sptr u = is_union_type(i->second);
	ABG_ASSERT(u);
	return u;
      }
  }

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);
  bool is_declaration_only = die_is_declaration_only(die);

  bool is_anonymous = false;
  if (name.empty())
    {
      // So we are looking at an anonymous union.  Let's give it a
      // name.
      name = get_internal_anonynous_die_base_name(die);
      ABG_ASSERT(!name.empty());
      // But we remember that the type is anonymous.
      is_anonymous = true;

      if (size_t s = scope->get_num_anonymous_member_unions())
	name = build_internal_anonymous_die_name(name, s);
    }

  // If the type has location, then associate it to its
  // representation.  This way, all occurences of types with the same
  // representation (name) and location can be later detected as being
  // for the same type.

  if (!is_anonymous)
    {
      if (corpus_sptr corp = ctxt.should_reuse_type_from_corpus_group())
	{
	  if (loc)
	    result = lookup_union_type_per_location(loc.expand(), *corp);
	  else
	    result = lookup_union_type(name, *corp);

	  if (result)
	    {
	      ctxt.associate_die_to_type(die, result, where_offset);
	      return result;
	    }
	}
    }

  // if we've already seen a union with the same union as 'die' then
  // let's re-use that one. We can't really safely re-use anonymous
  // classes as they have no name, by construction.  What we can do,
  // rather, is to reuse the typedef that name them, when they do have
  // a naming typedef.
  if (!is_anonymous)
    if (union_decl_sptr pre_existing_union =
	is_union_type(ctxt.lookup_artifact_from_die(die)))
      union_type = pre_existing_union;

  uint64_t size = 0;
  die_size_in_bits(die, size);

  if (union_type)
    {
      result = union_type;
      result->set_location(loc);
    }
  else
    {
      result.reset(new union_decl(ctxt.env(), name, size,
				  loc, decl_base::VISIBILITY_DEFAULT));
      result->set_is_anonymous(is_anonymous);
      if (is_declaration_only)
	result->set_is_declaration_only(true);
      result = is_union_type(add_decl_to_scope(result, scope));
      ABG_ASSERT(result);
    }

  if (size)
    {
      result->set_size_in_bits(size);
      result->set_is_declaration_only(false);
    }

  ctxt.associate_die_to_type(die, result, where_offset);

  // TODO: maybe schedule declaration-only union for result like we do
  // for classes:
  // ctxt.maybe_schedule_declaration_only_class_for_resolution(result);

  Dwarf_Die child;
  bool has_child = (dwarf_child(die, &child) == 0);
  if (!has_child)
    return result;

  ctxt.die_wip_classes_map(source)[dwarf_dieoffset(die)] = result;

  scope_decl_sptr scop =
    dynamic_pointer_cast<scope_decl>(result);
  ABG_ASSERT(scop);
  ctxt.scope_stack().push(scop.get());

  if (has_child)
    {
      do
	{
	  tag = dwarf_tag(&child);
	  // Handle data members.
	  if (tag == DW_TAG_member || tag == DW_TAG_variable)
	    {
	      Dwarf_Die type_die;
	      if (!die_die_attribute(&child, DW_AT_type, type_die))
		continue;

	      string n, m;
	      location loc;
	      die_loc_and_name(ctxt, &child, loc, n, m);

	      if (lookup_var_decl_in_scope(n, result))
		continue;

	      ssize_t offset_in_bits = 0;
	      decl_base_sptr ty =
		is_decl(build_ir_node_from_die(ctxt, &type_die,
					       called_from_public_decl,
					       where_offset));
	      type_base_sptr t = is_type(ty);
	      if (!t)
		continue;

	      // We have a non-static data member.  So this class
	      // cannot be a declaration-only class anymore, even if
	      // some DWARF emitters might consider it otherwise.
	      result->set_is_declaration_only(false);
	      access_specifier access = private_access;

	      die_access_specifier(&child, access);

	      var_decl_sptr dm(new var_decl(n, t, loc, m));
	      result->add_data_member(dm, access, /*is_laid_out=*/true,
				      /*is_static=*/false,
				      offset_in_bits);
	      ABG_ASSERT(has_scope(dm));
	      ctxt.associate_die_to_decl(&child, dm, where_offset,
					 /*associate_by_repr=*/false);
	    }
	  // Handle member functions;
	  else if (tag == DW_TAG_subprogram)
	    {
	      decl_base_sptr r =
		is_decl(build_ir_node_from_die(ctxt, &child,
					       result.get(),
					       called_from_public_decl,
					       where_offset));
	      if (!r)
		continue;

	      function_decl_sptr f = dynamic_pointer_cast<function_decl>(r);
	      ABG_ASSERT(f);

	      finish_member_function_reading(&child, f, result, ctxt);

	      ctxt.associate_die_to_decl(&child, f, where_offset,
					 /*associate_by_repr=*/false);
	    }
	  // Handle member types
	  else if (die_is_type(&child))
	    decl_base_sptr td =
	      is_decl(build_ir_node_from_die(ctxt, &child, result.get(),
					     called_from_public_decl,
					     where_offset));
	} while (dwarf_siblingof(&child, &child) == 0);
    }

  ctxt.scope_stack().pop();

  {
    die_class_or_union_map_type::const_iterator i =
      ctxt.die_wip_classes_map(source).find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_classes_map(source).end())
      {
	if (is_member_type(i->second))
	  set_member_access_specifier(result,
				      get_member_access_specifier(i->second));
	ctxt.die_wip_classes_map(source).erase(i);
      }
  }

  return result;
}

/// build a qualified type from a DW_TAG_const_type,
/// DW_TAG_volatile_type or DW_TAG_restrict_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the input DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting qualified_type_def.
static type_base_sptr
build_qualified_type(read_context&	ctxt,
		     Dwarf_Die*	die,
		     bool		called_from_public_decl,
		     size_t		where_offset)
{
  type_base_sptr result;
  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);

  if (tag != DW_TAG_const_type
      && tag != DW_TAG_volatile_type
      && tag != DW_TAG_restrict_type)
    return result;

  Dwarf_Die underlying_type_die;
  decl_base_sptr utype_decl;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    // So, if no DW_AT_type is present, then this means (if we are
    // looking at a debug info emitted by GCC) that we are looking
    // at a qualified void type.
    utype_decl = build_ir_node_for_void_type(ctxt);

  if (!utype_decl)
    utype_decl = is_decl(build_ir_node_from_die(ctxt, &underlying_type_die,
						called_from_public_decl,
						where_offset));
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die(die))
    {
      result = t;
      ctxt.associate_die_to_type(die, result, where_offset);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  ABG_ASSERT(utype);

  qualified_type_def::CV qual = qualified_type_def::CV_NONE;
  if (tag == DW_TAG_const_type)
    qual |= qualified_type_def::CV_CONST;
  else if (tag == DW_TAG_volatile_type)
    qual |= qualified_type_def::CV_VOLATILE;
  else if (tag == DW_TAG_restrict_type)
    qual |= qualified_type_def::CV_RESTRICT;
  else
    ABG_ASSERT_NOT_REACHED;

  if (!result)
    result.reset(new qualified_type_def(utype, qual, location()));

  ctxt.associate_die_to_type(die, result, where_offset);

  return result;
}

/// Strip qualification from a qualified type, when it makes sense.
///
/// DWARF constructs "const reference".  This is redundant because a
/// reference is always const.  The issue is these redundant types then
/// leak into the IR and make for bad diagnostics.
///
/// This function thus strips the const qualifier from the type in
/// that case.  It might contain code to strip other cases like this
/// in the future.
///
/// @param t the type to strip const qualification from.
///
/// @param ctxt the @ref read_context to use.
///
/// @return the stripped type or just return @p t.
static decl_base_sptr
maybe_strip_qualification(const qualified_type_def_sptr t,
			  read_context &ctxt)
{
  if (!t)
    return t;

  decl_base_sptr result = t;
  type_base_sptr u = t->get_underlying_type();
  environment* env = t->get_environment();

  if (t->get_cv_quals() & qualified_type_def::CV_CONST
      && (is_reference_type(u)))
    {
      // Let's strip only the const qualifier.  To do that, the "const"
      // qualified is turned into a no-op "none" qualified.
      result.reset(new qualified_type_def
		   (u, t->get_cv_quals() & ~qualified_type_def::CV_CONST,
		    t->get_location()));
    }
  else if (t->get_cv_quals() & qualified_type_def::CV_CONST
	   && env->is_void_type(u))
    {
      // So this type is a "const void".  Let's strip the "const"
      // qualifier out and make this just be "void", so that a "const
      // void" type and a "void" type compare equal after going through
      // this function.
      result = is_decl(u);
    }
  else if (is_array_of_qualified_element(u))
    {
      // In C and C++, a cv qualifiers of a qualified array apply to
      // the array element type.  So the qualifiers of the array can
      // be dropped and applied to the element type.
      //
      // Here, the element type is qualified already.  So apply the
      // qualifiers of the array itself to the already qualified
      // element type and drop the array qualifiers.
      array_type_def_sptr array = is_array_type(u);
      qualified_type_def_sptr element_type =
	is_qualified_type(array->get_element_type());
      qualified_type_def::CV quals = element_type->get_cv_quals();
      quals |= t->get_cv_quals();
      element_type->set_cv_quals(quals);
      result = is_decl(u);
      if (u->get_canonical_type()
	  || element_type->get_canonical_type())
	// We shouldn't be editing types that were already
	// canonicalized.  For those, canonicalization should be
	// delayed until after all editing is done.
	ABG_ASSERT_NOT_REACHED;
    }
  else if (is_array_type(u) && !is_array_of_qualified_element(is_array_type(u)))
    {
      // In C and C++, a cv qualifiers of a qualified array apply to
      // the array element type.  So the qualifiers of the array can
      // be dropped and applied to the element type.
      //
      // Here, the element type is not qualified.  So apply the
      // qualifiers of the array itself to the element type and drop
      // the array qualifiers.
      array_type_def_sptr array = is_array_type(u);
      type_base_sptr element_type = array->get_element_type();
      qualified_type_def_sptr qual_type
	(new qualified_type_def(element_type,
				t->get_cv_quals(),
				t->get_location()));
      add_decl_to_scope(qual_type, is_decl(element_type)->get_scope());
      array->set_element_type(qual_type);
      ctxt.schedule_type_for_late_canonicalization(is_type(qual_type));
      result = is_decl(u);
      if (u->get_canonical_type())
	// We shouldn't be editing types that were already
	// canonicalized.  For those, canonicalization should be
	// delayed until after all editing is done.
	ABG_ASSERT_NOT_REACHED;
    }

  return result;
}

/// Build a pointer type from a DW_TAG_pointer_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read information from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting pointer to pointer_type_def.
static pointer_type_def_sptr
build_pointer_type_def(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset)
{
  pointer_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_pointer_type)
    return result;

  type_or_decl_base_sptr utype_decl;
  Dwarf_Die underlying_type_die;
  bool has_underlying_type_die = false;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    // If the DW_AT_type attribute is missing, that means we are
    // looking at a pointer to "void".
    utype_decl = build_ir_node_for_void_type(ctxt);
  else
    has_underlying_type_die = true;

  if (!utype_decl && has_underlying_type_die)
    utype_decl = build_ir_node_from_die(ctxt, &underlying_type_die,
					called_from_public_decl,
					where_offset);
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die(die))
    {
      result = is_pointer_type(t);
      ABG_ASSERT(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  ABG_ASSERT(utype);

  // if the DIE for the pointer type doesn't have a byte_size
  // attribute then we assume the size of the pointer is the address
  // size of the current translation unit.
  uint64_t size = ctxt.cur_transl_unit()->get_address_size();
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    // The size as expressed by DW_AT_byte_size is in byte, so let's
    // convert it to bits.
    size *= 8;

  // And the size of the pointer must be the same as the address size
  // of the current translation unit.
  ABG_ASSERT((size_t) ctxt.cur_transl_unit()->get_address_size() == size);

  result.reset(new pointer_type_def(utype, size, /*alignment=*/0, location()));
  ABG_ASSERT(result->get_pointed_to_type());

  ctxt.associate_die_to_type(die, result, where_offset);
  return result;
}

/// Build a reference type from either a DW_TAG_reference_type or
/// DW_TAG_rvalue_reference_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting reference_type_def.
static reference_type_def_sptr
build_reference_type(read_context&	ctxt,
		     Dwarf_Die*	die,
		     bool		called_from_public_decl,
		     size_t		where_offset)
{
  reference_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_reference_type
      && tag != DW_TAG_rvalue_reference_type)
    return result;

  Dwarf_Die underlying_type_die;
  if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
    return result;

  type_or_decl_base_sptr utype_decl =
    build_ir_node_from_die(ctxt, &underlying_type_die,
			   called_from_public_decl,
			   where_offset);
  if (!utype_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die(die))
    {
      result = is_reference_type(t);
      ABG_ASSERT(result);
      return result;
    }

  type_base_sptr utype = is_type(utype_decl);
  ABG_ASSERT(utype);

  // if the DIE for the reference type doesn't have a byte_size
  // attribute then we assume the size of the reference is the address
  // size of the current translation unit.
  uint64_t size = ctxt.cur_transl_unit()->get_address_size();
  if (die_unsigned_constant_attribute(die, DW_AT_byte_size, size))
    size *= 8;

  // And the size of the pointer must be the same as the address size
  // of the current translation unit.
  ABG_ASSERT((size_t) ctxt.cur_transl_unit()->get_address_size() == size);

  bool is_lvalue = (tag == DW_TAG_reference_type) ? true : false;

  result.reset(new reference_type_def(utype, is_lvalue, size,
				      /*alignment=*/0,
				      location()));
  if (corpus_sptr corp = ctxt.current_corpus())
    if (reference_type_def_sptr t = lookup_reference_type(*result, *corp))
      result = t;
  ctxt.associate_die_to_type(die, result, where_offset);
  return result;
}

/// Build a subroutine type from a DW_TAG_subroutine_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param is_method points to a class or union declaration iff we're
/// building the type for a method.  This is the enclosing class or
/// union of the method.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positioned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting function_type_sptr.
static function_type_sptr
build_function_type(read_context&	ctxt,
		    Dwarf_Die*		die,
		    class_or_union_sptr is_method,
		    size_t		where_offset)
{
  function_type_sptr result;

  if (!die)
    return result;

  ABG_ASSERT(dwarf_tag(die) == DW_TAG_subroutine_type
	     || dwarf_tag(die) == DW_TAG_subprogram);

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  decl_base_sptr type_decl;

  translation_unit_sptr tu = ctxt.cur_transl_unit();
  ABG_ASSERT(tu);

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die(die))
    {
      result = is_function_type(t);
      ABG_ASSERT(result);
      ctxt.associate_die_to_type(die, result, where_offset);
      return result;
    }

  bool odr_is_relevant = ctxt.odr_is_relevant(die);
  if (odr_is_relevant)
    {
      // So we can rely on the One Definition Rule to say that if
      // several different function types have the same name (or
      // rather, representation) across the entire binary, then they
      // ought to designate the same function type.  So let's ensure
      // that if we've already seen a function type with the same
      // representation as the function type 'die', then it's the same
      // type as the one denoted by 'die'.
      if (function_type_sptr fn_type =
	  is_function_type(ctxt.lookup_type_artifact_from_die(die)))
	{
	  ctxt.associate_die_to_type(die, fn_type, where_offset);
	  return fn_type;
	}
    }
  else
    {
      // We cannot rely on the One Definition Rule across the entire
      // binary.  But then we can still say that inside the current
      // *translation unit*, if two function types have the same name
      // (rather representation) then they designate the same type.
      // Thus, if we've already seen a DIE with the same
      // representation as the one of 'die', then it designates the
      // same function type.
      if (function_type_sptr fn_type =
	  is_function_type(ctxt.lookup_type_from_die(die)))
	{
	  ctxt.associate_die_to_type(die, fn_type, where_offset);
	  return fn_type;
	}
    }

  // Let's look at the DIE to detect if it's the DIE for a method
  // (type).  If it is, we can deduce the name of its enclosing class
  // and if it's a static or const.
  bool is_const = false;
  bool is_static = false;
  Dwarf_Die object_pointer_die;
  Dwarf_Die class_type_die;
  bool has_this_parm_die =
    die_function_type_is_method_type(ctxt, die, where_offset,
				     object_pointer_die,
				     class_type_die,
				     is_static);
  if (has_this_parm_die)
    {
      // The function (type) has a "this" parameter DIE. It means it's
      // a member function DIE.
      if (!is_static)
	if (die_object_pointer_is_for_const_method(&object_pointer_die))
	  is_const = true;

      if (!is_method)
	{
	  // We were initially called as if the function represented
	  // by DIE was *NOT* a member function.  But now we know it's
	  // a member function.  Let's take that into account.
	  class_or_union_sptr klass_type =
	    is_class_or_union_type(build_ir_node_from_die(ctxt, &class_type_die,
							  /*called_from_pub_decl=*/true,
							  where_offset));
	  ABG_ASSERT(klass_type);
	  is_method = klass_type;
	}
    }

  // Let's create the type early and record it as being for the DIE
  // 'die'.  This way, when building the sub-type triggers the
  // creation of a type matching the same 'die', then we'll reuse this
  // one.

  result.reset(is_method
	       ? new method_type(is_method, is_const,
				 tu->get_address_size(),
				 /*alignment=*/0)
	       : new function_type(ctxt.env(), tu->get_address_size(),
				   /*alignment=*/0));
  ctxt.associate_die_to_type(die, result, where_offset);
  ctxt.die_wip_function_types_map(source)[dwarf_dieoffset(die)] = result;

  type_base_sptr return_type;
  Dwarf_Die ret_type_die;
  if (die_die_attribute(die, DW_AT_type, ret_type_die))
    return_type =
      is_type(build_ir_node_from_die(ctxt, &ret_type_die,
				     /*called_from_public_decl=*/true,
				     where_offset));
  if (!return_type)
    return_type = is_type(build_ir_node_for_void_type(ctxt));
  result->set_return_type(return_type);

  Dwarf_Die child;
  function_decl::parameters function_parms;

  if (dwarf_child(die, &child) == 0)
    do
      {
	int child_tag = dwarf_tag(&child);
	if (child_tag == DW_TAG_formal_parameter)
	  {
	    // This is a "normal" function parameter.
	    string name, linkage_name;
	    location loc;
	    die_loc_and_name(ctxt, &child, loc, name, linkage_name);
	    if (!tools_utils::string_is_ascii_identifier(name))
	      // Sometimes, bogus compiler emit names that are
	      // non-ascii garbage.  Let's just ditch that for now.
	      name.clear();
	    bool is_artificial = die_is_artificial(&child);
	    type_base_sptr parm_type;
	    Dwarf_Die parm_type_die;
	    if (die_die_attribute(&child, DW_AT_type, parm_type_die))
	      parm_type =
		is_type(build_ir_node_from_die(ctxt, &parm_type_die,
					       /*called_from_public_decl=*/true,
					       where_offset));
	    if (!parm_type)
	      continue;
	    function_decl::parameter_sptr p
	      (new function_decl::parameter(parm_type, name, loc,
					    /*variadic_marker=*/false,
					    is_artificial));
	    function_parms.push_back(p);
	  }
	else if (child_tag == DW_TAG_unspecified_parameters)
	  {
	    // This is a variadic function parameter.
	    bool is_artificial = die_is_artificial(&child);
	    ir::environment* env = ctxt.env();
	    ABG_ASSERT(env);
	    type_base_sptr parm_type = env->get_variadic_parameter_type();
	    function_decl::parameter_sptr p
	      (new function_decl::parameter(parm_type,
					    /*name=*/"",
					    location(),
					    /*variadic_marker=*/true,
					    is_artificial));
	    function_parms.push_back(p);
	    // After a DW_TAG_unspecified_parameters tag, we shouldn't
	    // keep reading for parameters.  The
	    // unspecified_parameters TAG should be the last parameter
	    // that we record. For instance, if there are multiple
	    // DW_TAG_unspecified_parameters DIEs then we should care
	    // only for the first one.
	    break;
	  }
      }
    while (dwarf_siblingof(&child, &child) == 0);

  result->set_parameters(function_parms);

  tu->bind_function_type_life_time(result);

  {
    die_function_type_map_type::const_iterator i =
      ctxt.die_wip_function_types_map(source).
      find(dwarf_dieoffset(die));
    if (i != ctxt.die_wip_function_types_map(source).end())
      ctxt.die_wip_function_types_map(source).erase(i);
  }

  return result;
}

/// Build a subrange type from a DW_TAG_subrange_type.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at in the DIE tree.  This is useful when @p die is
/// e,g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param associate_die_to_type if this is true then the resulting
/// type is associated to the @p die, so that next time when the
/// system looks up the type associated to it, the current resulting
/// type is returned.  If false, then no association is done and the
/// resulting type can be destroyed right after.  This can be useful
/// when the sole purpose of building the @ref
/// array_type_def::subrange_type is to use some of its method like,
/// e.g, its name pretty printing methods.
///
/// @return the newly built instance of @ref
/// array_type_def::subrange_type, or nil if no type could be built.
static array_type_def::subrange_sptr
build_subrange_type(read_context&	ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    bool		associate_type_to_die)
{
  array_type_def::subrange_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_subrange_type)
    return result;

  string name = die_name(die);

  translation_unit::language language = ctxt.cur_transl_unit()->get_language();
  array_type_def::subrange_type::bound_value lower_bound =
    get_default_array_lower_bound(language);
  array_type_def::subrange_type::bound_value upper_bound;
  uint64_t count = 0;
  bool is_infinite = false;

  // The DWARF 4 specifications says, in [5.11 Subrange
  // Type Entries]:
  //
  //     The subrange entry may have the attributes
  //     DW_AT_lower_bound and DW_AT_upper_bound to
  //     specify, respectively, the lower and upper bound
  //     values of the subrange.
  //
  // So let's look for DW_AT_lower_bound first.
  die_constant_attribute(die, DW_AT_lower_bound, lower_bound);

  // Then, DW_AT_upper_bound.
  if (!die_constant_attribute(die, DW_AT_upper_bound, upper_bound))
    {
      // The DWARF 4 spec says, in [5.11 Subrange Type
      // Entries]:
      //
      //   The DW_AT_upper_bound attribute may be replaced
      //   by a DW_AT_count attribute, whose value
      //   describes the number of elements in the
      //   subrange rather than the value of the last
      //   element."
      //
      // So, as DW_AT_upper_bound is not present in this
      // case, let's see if there is a DW_AT_count.
      die_unsigned_constant_attribute(die, DW_AT_count, count);

      // We can deduce the upper_bound from the
      // lower_bound and the number of elements of the
      // array:
      if (int64_t u = lower_bound.get_signed_value() + count)
	upper_bound = u - 1;

      if (upper_bound.get_unsigned_value() == 0 && count == 0)
	// No upper_bound nor count was present on the DIE, this means
	// the array is considered to have an infinite (or rather not
	// known) size.
	is_infinite = true;
    }

  if (UINT64_MAX == upper_bound.get_unsigned_value())
    {
      // If the upper_bound size is the max of the integer value, then
      // it most certainly means infinite size.
      is_infinite = true;
      upper_bound.set_unsigned(0);
    }

  result.reset
    (new array_type_def::subrange_type(ctxt.env(),
				       name,
				       lower_bound,
				       upper_bound,
				       location()));
  result->is_infinite(is_infinite);

  // load the underlying type.
  Dwarf_Die underlying_type_die;
  type_base_sptr underlying_type;
  if (die_die_attribute(die, DW_AT_type, underlying_type_die))
    underlying_type =
      is_type(build_ir_node_from_die(ctxt,
				     &underlying_type_die,
				     /*called_from_public_decl=*/true,
				     where_offset));

  if (underlying_type)
    result->set_underlying_type(underlying_type);

  if (associate_type_to_die)
    ctxt.associate_die_to_type(die, result, where_offset);

  return result;
}

/// Build the sub-ranges of an array type.
///
/// This is a sub-routine of build_array_type().
///
/// @param ctxt the context to read from.
///
/// @param die the DIE of tag DW_TAG_array_type which contains
/// children DIEs that represent the sub-ranges.
///
/// @param subranges out parameter.  This is set to the sub-ranges
/// that are built from @p die.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positioned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
static void
build_subranges_from_array_type_die(read_context&	ctxt,
				    Dwarf_Die*		die,
				    array_type_def::subranges_type&	subranges,
				    size_t		where_offset,
				    bool		associate_type_to_die)
{
  Dwarf_Die child;

  if (dwarf_child(die, &child) == 0)
    {
      do
	{
	  int child_tag = dwarf_tag(&child);
	  if (child_tag == DW_TAG_subrange_type)
	    {
	      array_type_def::subrange_sptr s;
	      if (associate_type_to_die)
		{
		  // We are being called to create the type, add it to
		  // the current type graph and associate it to the
		  // DIE it's been created from.
		  type_or_decl_base_sptr t =
		    build_ir_node_from_die(ctxt, &child,
					   /*called_from_public_decl=*/true,
					   where_offset);
		  s = is_subrange_type(t);
		}
	      else
		// We are being called to create the type but *NOT*
		// add it to the current tyupe tree, *NOR* associate
		// it to the DIE it's been created from.
		s = build_subrange_type(ctxt, &child,
					where_offset,
					/*associate_type_to_die=*/false);
	      if (s)
		subranges.push_back(s);
	    }
	}
      while (dwarf_siblingof(&child, &child) == 0);
    }
}

/// Build an array type from a DW_TAG_array_type DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positioned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return a pointer to the resulting array_type_def.
static array_type_def_sptr
build_array_type(read_context&	ctxt,
		 Dwarf_Die*	die,
		 bool		called_from_public_decl,
		 size_t	where_offset)
{
  array_type_def_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_array_type)
    return result;

  decl_base_sptr type_decl;
  Dwarf_Die type_die;

  if (die_die_attribute(die, DW_AT_type, type_die))
    type_decl = is_decl(build_ir_node_from_die(ctxt, &type_die,
					       called_from_public_decl,
					       where_offset));
  if (!type_decl)
    return result;

  // The call to build_ir_node_from_die() could have triggered the
  // creation of the type for this DIE.  In that case, just return it.
  if (type_base_sptr t = ctxt.lookup_type_from_die(die))
    {
      result = is_array_type(t);
      ABG_ASSERT(result);
      return result;
    }

  type_base_sptr type = is_type(type_decl);
  ABG_ASSERT(type);

  array_type_def::subranges_type subranges;

  build_subranges_from_array_type_die(ctxt, die, subranges, where_offset);

  result.reset(new array_type_def(type, subranges, location()));

  return result;
}

/// Create a typedef_decl from a DW_TAG_typedef DIE.
///
/// @param ctxt the read context to consider.
///
/// @param die the DIE to read from.
///
/// @param called_from_public_decl true if this function was called
/// from a context where either a public function or a public variable
/// is being built.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the newly created typedef_decl.
static typedef_decl_sptr
build_typedef_type(read_context&	ctxt,
		   Dwarf_Die*		die,
		   bool		called_from_public_decl,
		   size_t		where_offset)
{
  typedef_decl_sptr result;

  if (!die)
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  unsigned tag = dwarf_tag(die);
  if (tag != DW_TAG_typedef)
    return result;

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  if (corpus_sptr corp = ctxt.should_reuse_type_from_corpus_group())
    if (loc)
      result = lookup_typedef_type_per_location(loc.expand(), *corp);

  if (!ctxt.odr_is_relevant(die))
    if (typedef_decl_sptr t = is_typedef(ctxt.lookup_artifact_from_die(die)))
      result = t;

  if (!result)
    {
      type_base_sptr utype;
      Dwarf_Die underlying_type_die;
      if (!die_die_attribute(die, DW_AT_type, underlying_type_die))
	// A typedef DIE with no underlying type means a typedef to
	// void type.
	utype = ctxt.env()->get_void_type();

      if (!utype)
	utype =
	  is_type(build_ir_node_from_die(ctxt,
					 &underlying_type_die,
					 called_from_public_decl,
					 where_offset));
      if (!utype)
	return result;

      // The call to build_ir_node_from_die() could have triggered the
      // creation of the type for this DIE.  In that case, just return
      // it.
      if (type_base_sptr t = ctxt.lookup_type_from_die(die))
	{
	  result = is_typedef(t);
	  ABG_ASSERT(result);
	  return result;
	}

      ABG_ASSERT(utype);
      result.reset(new typedef_decl(name, utype, loc, linkage_name));

      if (class_decl_sptr klass = is_class_type(utype))
	if (is_anonymous_type(klass))
	  klass->set_naming_typedef(result);
    }

  ctxt.associate_die_to_type(die, result, where_offset);

  return result;
}

/// Build a @ref var_decl out of a DW_TAG_variable DIE if the variable
/// denoted by the DIE is not suppressed by a suppression
/// specification associated to the current read context.
///
/// Note that if a member variable declaration with the same name as
/// the name of the DIE we are looking at exists, this function returns
/// that existing variable declaration.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE representing the variable we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param result if this is set to an existing var_decl, this means
/// that the function will append the new properties it sees on @p die
/// to that exising var_decl.  Otherwise, if this parameter is NULL, a
/// new var_decl is going to be allocated and returned.
///
/// @param is_required_decl_spec this is true iff the variable to
/// build is referred to as being the specification of another
/// variable.
///
/// @return a pointer to the newly created var_decl.  If the var_decl
/// could not be built, this function returns NULL.
static var_decl_sptr
build_or_get_var_decl_if_not_suppressed(read_context&	ctxt,
					scope_decl	*scope,
					Dwarf_Die	*die,
					size_t	where_offset,
					var_decl_sptr	result,
					bool is_required_decl_spec)
{
  var_decl_sptr var;
  if (variable_is_suppressed(ctxt, scope, die, is_required_decl_spec))
    return var;

  if (class_decl* class_type = is_class_type(scope))
    {
      string var_name = die_name(die);
      if (!var_name.empty())
	if ((var = class_type->find_data_member(var_name)))
	  return var;
    }
  var = build_var_decl(ctxt, die, where_offset, result);
  return var;
}

/// Create a variable symbol with a given name.
///
/// @param sym_name the name of the variable symbol.
///
/// @param env the environment to create the default symbol in.
///
/// @return the newly created symbol.
static elf_symbol_sptr
create_default_var_sym(const string& sym_name, const environment *env)
{
  elf_symbol::version ver;
  elf_symbol::visibility vis = elf_symbol::DEFAULT_VISIBILITY;
  elf_symbol_sptr result =
    elf_symbol::create(env,
		       /*symbol index=*/ 0,
		       /*symbol size=*/ 0,
		       sym_name,
		       /*symbol type=*/ elf_symbol::OBJECT_TYPE,
		       /*symbol binding=*/ elf_symbol::GLOBAL_BINDING,
		       /*symbol is defined=*/ true,
		       /*symbol is common=*/ false,
		       /*symbol version=*/ ver,
		       /*symbol_visibility=*/vis);
  return result;
}

/// Build a @ref var_decl out of a DW_TAG_variable DIE.
///
/// @param ctxt the read context to use.
///
/// @param die the DIE representing the variable we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param result if this is set to an existing var_decl, this means
/// that the function will append the new properties it sees on @p die
/// to that exising var_decl.  Otherwise, if this parameter is NULL, a
/// new var_decl is going to be allocated and returned.
///
/// @return a pointer to the newly created var_decl.  If the var_decl
/// could not be built, this function returns NULL.
static var_decl_sptr
build_var_decl(read_context&	ctxt,
	       Dwarf_Die	*die,
	       size_t		where_offset,
	       var_decl_sptr	result)
{
  if (!die)
    return result;

  int tag = dwarf_tag(die);
  ABG_ASSERT(tag == DW_TAG_variable || tag == DW_TAG_member);

  if (!die_is_public_decl(die))
    return result;

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  type_base_sptr type;
  Dwarf_Die type_die;
  if (die_die_attribute(die, DW_AT_type, type_die))
    {
      decl_base_sptr ty =
	is_decl(build_ir_node_from_die(ctxt, &type_die,
				       /*called_from_public_decl=*/true,
				       where_offset));
      if (!ty)
	return result;
      type = is_type(ty);
      ABG_ASSERT(type);
    }

  if (!type)
    return result;

  string name, linkage_name;
  location loc;
  die_loc_and_name(ctxt, die, loc, name, linkage_name);

  if (!result)
    result.reset(new var_decl(name, type, loc, linkage_name));
  else
    {
      // We were called to append properties that might have been
      // missing from the first version of the variable.  And usually
      // that missing property is the mangled name.
      if (!linkage_name.empty())
	result->set_linkage_name(linkage_name);
    }

  // Check if a variable symbol with this name is exported by the elf
  // binary.  If it is, then set the symbol of the variable, if it's
  // not set already.
  if (!result->get_symbol())
    {
      elf_symbol_sptr var_sym;
      if (get_ignore_symbol_table(ctxt))
	{
	  string var_name =
	    result->get_linkage_name().empty()
	    ? result->get_name()
	    : result->get_linkage_name();

	  var_sym = create_default_var_sym(var_name, ctxt.env());
	  ABG_ASSERT(var_sym);
	  add_symbol_to_map(var_sym, ctxt.var_syms());
	}
      else
	{
	  Dwarf_Addr var_addr;
	  if (ctxt.get_variable_address(die, var_addr))
	    var_sym = var_sym = ctxt.variable_symbol_is_exported(var_addr);
	}

      if (var_sym)
	{
	  result->set_symbol(var_sym);
	  // If the linkage name is not set or is wrong, set it to
	  // the name of the underlying symbol.
	  string linkage_name = result->get_linkage_name();
	  if (linkage_name.empty()
	      || !var_sym->get_alias_from_name(linkage_name))
	    result->set_linkage_name(var_sym->get_name());
	  result->set_is_in_public_symbol_table(true);
	}
    }

  return result;
}

/// Test if a given function denoted by its DIE and its scope is
/// suppressed by any of the suppression specifications associated to
/// a given context of ELF/DWARF reading.
///
/// Note that a non-member function which symbol is not exported is
/// also suppressed.
///
/// @param ctxt the ELF/DWARF reading content of interest.
///
/// @param scope of the scope of the function.
///
/// @param function_die the DIE representing the function.
///
/// @return true iff @p function_die is suppressed by at least one
/// suppression specification attached to the @p ctxt.
static bool
function_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *function_die)
{
  if (function_die == 0
      || dwarf_tag(function_die) != DW_TAG_subprogram)
    return false;

  string fname = die_string_attribute(function_die, DW_AT_name);
  string flinkage_name = die_linkage_name(function_die);
  string qualified_name = build_qualified_name(scope, fname);

  // A non-member function which symbol is not exported is suppressed.
  if (!is_class_type(scope) && !die_is_declaration_only(function_die))
    {
      Dwarf_Addr fn_addr;
      elf_symbol_sptr fn_sym;
      if (!ctxt.get_function_address(function_die, fn_addr))
	return true;
      if (!get_ignore_symbol_table(ctxt))
	{
	  // We were not instructed to ignore (avoid loading) the
	  // symbol table, so we can rely on its presence to see if
	  // the address corresponds to the address of an exported
	  // function symbol.
	  if (!ctxt.function_symbol_is_exported(fn_addr))
	    return true;
	}
    }

  return suppr::function_is_suppressed(ctxt, qualified_name,
				       flinkage_name,
				       /*require_drop_property=*/true);
}

/// Build a @ref function_decl out of a DW_TAG_subprogram DIE if the
/// function denoted by the DIE is not suppressed by a suppression
/// specification associated to the current read context.
///
/// Note that if a member function declaration with the same signature
/// (pretty representation) as one of the DIE we are looking at
/// exists, this function returns that existing function declaration.
///
/// @param ctxt the read context to use.
///
/// @param scope the scope of the function we are looking at.
///
/// @param fn_die the DIE representing the function we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param result if this is set to an existing function_decl, this
/// means that the function will append the new properties it sees on
/// @p fn_die to that exising function_decl.  Otherwise, if this
/// parameter is NULL, a new function_decl is going to be allocated
/// and returned.
///
/// @return a pointer to the newly created var_decl.  If the var_decl
/// could not be built, this function returns NULL.
static function_decl_sptr
build_or_get_fn_decl_if_not_suppressed(read_context&	  ctxt,
				       scope_decl	  *scope,
				       Dwarf_Die	  *fn_die,
				       size_t		  where_offset,
				       function_decl_sptr result)
{
  function_decl_sptr fn;
  if (function_is_suppressed(ctxt, scope, fn_die))
    return fn;

  if (!result)
    if ((fn = is_function_decl(ctxt.lookup_artifact_from_die(fn_die))))
      {
	fn = maybe_finish_function_decl_reading(ctxt, fn_die, where_offset, fn);
	ctxt.associate_die_to_decl(fn_die, fn, /*do_associate_by_repr=*/true);
	ctxt.associate_die_to_type(fn_die, fn->get_type(), where_offset);
	return fn;
      }

  fn = build_function_decl(ctxt, fn_die, where_offset, result);

  return fn;
}

/// Test if a given variable denoted by its DIE and its scope is
/// suppressed by any of the suppression specifications associated to
/// a given context of ELF/DWARF reading.
///
/// @param ctxt the ELF/DWARF reading content of interest.
///
/// @param scope of the scope of the variable.
///
/// @param variable_die the DIE representing the variable.
///
/// @param is_required_decl_spec if true, means that the @p
/// variable_die being considered is for a variable decl that is a
/// specification for a concrete variable being built.
///
/// @return true iff @p variable_die is suppressed by at least one
/// suppression specification attached to the @p ctxt.
static bool
variable_is_suppressed(const read_context& ctxt,
		       const scope_decl* scope,
		       Dwarf_Die *variable_die,
		       bool is_required_decl_spec)
{
  if (variable_die == 0
      || (dwarf_tag(variable_die) != DW_TAG_variable
	  && dwarf_tag(variable_die) != DW_TAG_member))
    return false;

  string name = die_string_attribute(variable_die, DW_AT_name);
  string linkage_name = die_linkage_name(variable_die);
  string qualified_name = build_qualified_name(scope, name);

  // If a non member variable that is a declaration (has no exported
  // symbol), is not the specification of another concrete variable,
  // then it's suppressed.  This is a size optimization; it removes
  // useless declaration-only variables from the IR.
  //
  // Otherwise, if a non-member variable is the specification of
  // another concrete variable, then this function looks at
  // suppression specification specifications to know if its
  // suppressed.
  if (!is_class_type(scope) && !is_required_decl_spec)
    {
      Dwarf_Addr var_addr = 0;
      elf_symbol_sptr var_sym;
      if (!ctxt.get_variable_address(variable_die, var_addr))
	return true;
      if (!get_ignore_symbol_table(ctxt))
	{
	  // We were not instructed to ignore (avoid loading) the
	  // symbol table, so we can rely on its presence to see if
	  // the address corresponds to the address of an exported
	  // variable symbol.
	  if (!ctxt.variable_symbol_is_exported(var_addr))
	    return true;
	}
    }

  return suppr::variable_is_suppressed(ctxt, qualified_name,
				       linkage_name,
				       /*require_drop_property=*/true);
}

/// Test if a type (designated by a given DIE) in a given scope is
/// suppressed by the suppression specifications that are associated
/// to a given read context.
///
/// @param ctxt the read context to consider.
///
/// @param scope of the scope of the type DIE to consider.
///
/// @param type_die the DIE that designates the type to consider.
///
/// @param type_is_private out parameter.  If this function returns
/// true (the type @p type_die is suppressed) and if the type was
/// suppressed because it's private then this parameter is set to
/// true.
///
/// @return true iff the type designated by the DIE @p type_die, in
/// the scope @p scope is suppressed by at the suppression
/// specifications associated to the current read context.
static bool
type_is_suppressed(const read_context& ctxt,
		   const scope_decl* scope,
		   Dwarf_Die *type_die,
		   bool &type_is_private)
{
  if (type_die == 0
      || (dwarf_tag(type_die) != DW_TAG_enumeration_type
	  && dwarf_tag(type_die) != DW_TAG_class_type
	  && dwarf_tag(type_die) != DW_TAG_structure_type
	  && dwarf_tag(type_die) != DW_TAG_union_type))
    return false;

  string type_name, linkage_name;
  location type_location;
  die_loc_and_name(ctxt, type_die, type_location, type_name, linkage_name);
  string qualified_name = build_qualified_name(scope, type_name);

  return suppr::type_is_suppressed(ctxt, qualified_name,
				   type_location,
				   type_is_private,
				   /*require_drop_property=*/true);
}

/// Test if a type (designated by a given DIE) in a given scope is
/// suppressed by the suppression specifications that are associated
/// to a given read context.
///
/// @param ctxt the read context to consider.
///
/// @param scope of the scope of the type DIE to consider.
///
/// @param type_die the DIE that designates the type to consider.
///
/// @return true iff the type designated by the DIE @p type_die, in
/// the scope @p scope is suppressed by at the suppression
/// specifications associated to the current read context.
static bool
type_is_suppressed(const read_context& ctxt,
		   const scope_decl* scope,
		   Dwarf_Die *type_die)
{
  bool type_is_private = false;
  return type_is_suppressed(ctxt, scope, type_die, type_is_private);
}

/// Get the opaque version of a type that was suppressed because it's
/// a private type.
///
/// The opaque version version of the type is just a declared-only
/// version of the type (class or union type) denoted by @p type_die.
///
/// @param ctxt the read context in use.
///
/// @param scope the scope of the type die we are looking at.
///
/// @param type_die the type DIE we are looking at.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the opaque version of the type denoted by @p type_die or
/// nil if no opaque version was found.
static class_or_union_sptr
get_opaque_version_of_type(read_context	&ctxt,
			   scope_decl		*scope,
			   Dwarf_Die		*type_die,
			   size_t		where_offset)
{
  class_or_union_sptr result;

  if (type_die == 0)
    return result;

  unsigned tag = dwarf_tag(type_die);
  if (tag != DW_TAG_class_type
      && tag != DW_TAG_structure_type
      && tag != DW_TAG_union_type)
    return result;

  string type_name, linkage_name;
  location type_location;
  die_loc_and_name(ctxt, type_die, type_location, type_name, linkage_name);
  if (!type_location)
    return result;

  string qualified_name = build_qualified_name(scope, type_name);

  // TODO: also handle declaration-only unions.  To do that, we mostly
  // need to adapt add_or_update_union_type to make it schedule
  // declaration-only unions for resolution too.
  string_classes_map::const_iterator i =
    ctxt.declaration_only_classes().find(qualified_name);
  if (i != ctxt.declaration_only_classes().end())
    result = i->second.back();

  if (!result)
    {
      if (tag == DW_TAG_class_type || tag == DW_TAG_structure_type)
	{
	  // So we didn't find any pre-existing forward-declared-only
	  // class for the class definition that we could return as an
	  // opaque type.  So let's build one.
	  //
	  // TODO: we need to be able to do this for unions too!
	  class_decl_sptr klass(new class_decl(ctxt.env(), type_name,
					       /*alignment=*/0, /*size=*/0,
					       tag == DW_TAG_structure_type,
					       type_location,
					       decl_base::VISIBILITY_DEFAULT));
	  klass->set_is_declaration_only(true);
	  add_decl_to_scope(klass, scope);
	  ctxt.associate_die_to_type(type_die, klass, where_offset);
	  ctxt.maybe_schedule_declaration_only_class_for_resolution(klass);
	  result = klass;
	}
    }

  return result;
}

/// Create a function symbol with a given name.
///
/// @param sym_name the name of the symbol to create.
///
/// @param env the environment to create the symbol in.
///
/// @return the newly created symbol.
elf_symbol_sptr
create_default_fn_sym(const string& sym_name, const environment *env)
{
  elf_symbol::version ver;
  elf_symbol_sptr result =
    elf_symbol::create(env,
		       /*symbol index=*/ 0,
		       /*symbol size=*/ 0,
		       sym_name,
		       /*symbol type=*/ elf_symbol::FUNC_TYPE,
		       /*symbol binding=*/ elf_symbol::GLOBAL_BINDING,
		       /*symbol is defined=*/ true,
		       /*symbol is common=*/ false,
		       /*symbol version=*/ ver,
		       /*symbol visibility=*/elf_symbol::DEFAULT_VISIBILITY);
  return result;
}

/// Build a @ref function_decl our of a DW_TAG_subprogram DIE.
///
/// @param ctxt the read context to use
///
/// @param die the DW_TAG_subprogram DIE to read from.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param called_for_public_decl this is set to true if the function
/// was called for a public (function) decl.
static function_decl_sptr
build_function_decl(read_context&	ctxt,
		    Dwarf_Die*		die,
		    size_t		where_offset,
		    function_decl_sptr	fn)
{
  function_decl_sptr result = fn;
  if (!die)
    return result;
  ABG_ASSERT(dwarf_tag(die) == DW_TAG_subprogram);

  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  if (!die_is_public_decl(die))
    return result;

  translation_unit_sptr tu = ctxt.cur_transl_unit();
  ABG_ASSERT(tu);

  string fname, flinkage_name;
  location floc;
  die_loc_and_name(ctxt, die, floc, fname, flinkage_name);

  size_t is_inline = die_is_declared_inline(die);
  class_or_union_sptr is_method =
    is_class_or_union_type(get_scope_for_die(ctxt, die, true, where_offset));

  if (result)
    {
      // Add the properties that might have been missing from the
      // first declaration of the function.  For now, it usually is
      // the mangled name that goes missing in the first declarations.
      //
      // Also note that if 'fn' has just been cloned, the current
      // linkage name (of the current DIE) might be different from the
      // linkage name of 'fn'.  In that case, update the linkage name
      // of 'fn' too.
      if (!flinkage_name.empty()
	  && result->get_linkage_name() != flinkage_name)
	result->set_linkage_name(flinkage_name);
      if (floc)
	if (!result->get_location())
	  result->set_location(floc);
    }
  else
    {
      function_type_sptr fn_type(build_function_type(ctxt, die, is_method,
						     where_offset));
      if (!fn_type)
	return result;

      result.reset(is_method
		   ? new method_decl(fname, fn_type,
				     is_inline, floc,
				     flinkage_name)
		   : new function_decl(fname, fn_type,
				       is_inline, floc,
				       flinkage_name));
    }

  // Set the symbol of the function.  If the linkage name is not set
  // or is wrong, set it to the name of the underlying symbol.
  if (!result->get_symbol())
    {
      elf_symbol_sptr fn_sym;
      if (get_ignore_symbol_table(ctxt))
	{
	  string fn_name =
	    result->get_linkage_name().empty()
	    ? result->get_name()
	    : result->get_linkage_name();

	  fn_sym = create_default_fn_sym(fn_name, ctxt.env());
	  ABG_ASSERT(fn_sym);
	  add_symbol_to_map(fn_sym, ctxt.fun_syms());
	}
      else
	{
	  Dwarf_Addr fn_addr;
	  if (ctxt.get_function_address(die, fn_addr))
	    fn_sym = ctxt.function_symbol_is_exported(fn_addr);
	}

      if (fn_sym)
	{
	  result->set_symbol(fn_sym);
	  string linkage_name = result->get_linkage_name();
	  if (linkage_name.empty()
	      || !fn_sym->get_alias_from_name(linkage_name))
	    result->set_linkage_name(fn_sym->get_name());
	  result->set_is_in_public_symbol_table(true);
	}
    }

  ctxt.associate_die_to_type(die, result->get_type(), where_offset);

  size_t die_offset = dwarf_dieoffset(die);

  if (fn
      && is_member_function(fn)
      && get_member_function_is_virtual(fn)
      && !result->get_linkage_name().empty())
    // This function is a virtual member function which has its
    // linkage name *and* and has its underlying symbol correctly set.
    // It thus doesn't need any fixup related to elf symbol.  So
    // remove it from the set of virtual member functions with linkage
    // names and no elf symbol that need to be fixed up.
    ctxt.die_function_decl_with_no_symbol_map().erase(die_offset);
  return result;
}

/// Add a set of addresses (representing function symbols) to a
/// function symbol name -> symbol map.
///
/// For a given symbol address, the function retrieves the name of the
/// symbol as well as the symbol itself and inserts an entry {symbol
/// name, symbol} into a map of symbol name -> symbol map.
///
/// @param syms the set of symbol addresses to consider.
///
/// @param map the map to populate.
///
/// @param ctxt the context in which we are loading a given ELF file.
static void
add_fn_symbols_to_map(address_set_type& syms,
		      string_elf_symbols_map_type& map,
		      read_context& ctxt)
{
  for (address_set_type::iterator i = syms.begin(); i != syms.end(); ++i)
    {
      elf_symbol_sptr sym = ctxt.lookup_elf_fn_symbol_from_address(*i);
      ABG_ASSERT(sym);
      string_elf_symbols_map_type::iterator it =
	ctxt.fun_syms().find(sym->get_name());
      ABG_ASSERT(it != ctxt.fun_syms().end());
      map.insert(*it);
    }
}

/// Add a symbol to a symbol map.
///
/// @param sym the symbol to add.
///
/// @param map the symbol map to add the symbol into.
static void
add_symbol_to_map(const elf_symbol_sptr& sym,
		  string_elf_symbols_map_type& map)
{
  if (!sym)
    return;

  string_elf_symbols_map_type::iterator it = map.find(sym->get_name());
  if (it == map.end())
    {
      elf_symbols syms;
      syms.push_back(sym);
      map[sym->get_name()] = syms;
    }
  else
    it->second.push_back(sym);
}

/// Add a set of addresses (representing variable symbols) to a
/// variable symbol name -> symbol map.
///
/// For a given symbol address, the variable retrieves the name of the
/// symbol as well as the symbol itself and inserts an entry {symbol
/// name, symbol} into a map of symbol name -> symbol map.
///
/// @param syms the set of symbol addresses to consider.
///
/// @param map the map to populate.
///
/// @param ctxt the context in which we are loading a given ELF file.
static void
add_var_symbols_to_map(address_set_type& syms,
		       string_elf_symbols_map_type& map,
		       read_context& ctxt)
{
  for (address_set_type::iterator i = syms.begin(); i != syms.end(); ++i)
    {
      elf_symbol_sptr sym = ctxt.lookup_elf_var_symbol_from_address(*i);
      ABG_ASSERT(sym);
      string_elf_symbols_map_type::iterator it =
	ctxt.var_syms().find(sym->get_name());
      ABG_ASSERT(it != ctxt.var_syms().end());
      map.insert(*it);
    }
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible through a DWARF Front End Library handle, and stuff
/// them into a libabigail ABI Corpus.
///
/// @param ctxt the read context.
///
/// @return a pointer to the resulting corpus, or NULL if the corpus
/// could not be constructed.
static corpus_sptr
read_debug_info_into_corpus(read_context& ctxt)
{
  ctxt.clear_per_corpus_data();

  if (!ctxt.current_corpus())
    {
      corpus_sptr corp (new corpus(ctxt.env(), ctxt.elf_path()));
      ctxt.current_corpus(corp);
      if (!ctxt.env())
	ctxt.env(corp->get_environment());
    }

  // First set some mundane properties of the corpus gathered from
  // ELF.
  ctxt.current_corpus()->set_path(ctxt.elf_path());
  ctxt.current_corpus()->set_origin(corpus::DWARF_ORIGIN);
  ctxt.current_corpus()->set_soname(ctxt.dt_soname());
  ctxt.current_corpus()->set_needed(ctxt.dt_needed());
  ctxt.current_corpus()->set_architecture_name(ctxt.elf_architecture());

  // Set symbols information to the corpus.
  if (!get_ignore_symbol_table(ctxt))
    {
      if (ctxt.load_in_linux_kernel_mode() && ctxt.is_linux_kernel_binary())
	{
	  string_elf_symbols_map_sptr exported_fn_symbols_map
	    (new string_elf_symbols_map_type);
	  add_fn_symbols_to_map(*ctxt.linux_exported_fn_syms(),
				*exported_fn_symbols_map,
				ctxt);
	  add_fn_symbols_to_map(*ctxt.linux_exported_gpl_fn_syms(),
				*exported_fn_symbols_map,
				ctxt);
	  ctxt.current_corpus()->set_fun_symbol_map(exported_fn_symbols_map);

	  string_elf_symbols_map_sptr exported_var_symbols_map
	    (new string_elf_symbols_map_type);
	  add_var_symbols_to_map(*ctxt.linux_exported_var_syms(),
				 *exported_var_symbols_map,
				 ctxt);
	  add_var_symbols_to_map(*ctxt.linux_exported_gpl_var_syms(),
				 *exported_var_symbols_map,
				 ctxt);
	  ctxt.current_corpus()->set_var_symbol_map(exported_var_symbols_map);
	}
      else
	{
	  ctxt.current_corpus()->set_fun_symbol_map(ctxt.fun_syms_sptr());
	  ctxt.current_corpus()->set_var_symbol_map(ctxt.var_syms_sptr());
	}

      ctxt.current_corpus()->set_undefined_fun_symbol_map
	(ctxt.undefined_fun_syms_sptr());
      ctxt.current_corpus()->set_undefined_var_symbol_map
	(ctxt.undefined_var_syms_sptr());
    }
  else
    {
      ctxt.current_corpus()->set_fun_symbol_map(ctxt.fun_syms_sptr());
      ctxt.current_corpus()->set_var_symbol_map(ctxt.var_syms_sptr());
    }

  // Get out now if no debug info is found.
  if (!ctxt.dwarf())
    return ctxt.current_corpus();

  uint8_t address_size = 0;
  size_t header_size = 0;

  // Set the set of exported declaration that are defined.
  ctxt.exported_decls_builder
    (ctxt.current_corpus()->get_exported_decls_builder().get());

  // Walk all the DIEs of the debug info to build a DIE -> parent map
  // useful for get_die_parent() to work.
  if (ctxt.do_log())
    cerr << "building die -> parent maps ...";

  ctxt.build_die_parent_maps();

  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() << "\n";

  ctxt.env()->canonicalization_is_done(false);

  if (ctxt.do_log())
    cerr << "building the libabigail internal representation ...";
  // And now walk all the DIEs again to build the libabigail IR.
  Dwarf_Half dwarf_version = 0;
  for (Dwarf_Off offset = 0, next_offset = 0;
       (dwarf_next_unit(ctxt.dwarf(), offset, &next_offset, &header_size,
			&dwarf_version, NULL, &address_size, NULL,
			NULL, NULL) == 0);
       offset = next_offset)
    {
      Dwarf_Off die_offset = offset + header_size;
      Dwarf_Die unit;
      if (!dwarf_offdie(ctxt.dwarf(), die_offset, &unit)
	  || dwarf_tag(&unit) != DW_TAG_compile_unit)
	continue;

      ctxt.dwarf_version(dwarf_version);

      address_size *= 8;

      // Build a translation_unit IR node from cu; note that cu must
      // be a DW_TAG_compile_unit die.
      translation_unit_sptr ir_node =
	build_translation_unit_and_add_to_ir(ctxt, &unit, address_size);
      ABG_ASSERT(ir_node);
    }
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() << "\n";

  if (ctxt.do_log())
    cerr << "resolving declaration only classes ...";
  ctxt.resolve_declaration_only_classes();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<"\n";

  if (ctxt.do_log())
    cerr << "fixing up functions with linkage name but "
	 << "no advertised underlying symbols ....";
  ctxt.fixup_functions_with_no_symbols();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<"\n";

  /// Now, look at the types that needs to be canonicalized after the
  /// translation has been constructed (which is just now) and
  /// canonicalize them.
  ///
  /// These types need to be constructed at the end of the translation
  /// unit reading phase because some types are modified by some DIEs
  /// even after the principal DIE describing the type has been read;
  /// this happens for clones of virtual destructors (for instance) or
  /// even for some static data members.  We need to do that for types
  /// are in the alternate debug info section and for types that in
  /// the main debug info section.
  if (ctxt.do_log())
    cerr << "perform late type canonicalizing ...\n";
  ctxt.perform_late_type_canonicalizing();
  if (ctxt.do_log())
    cerr << "late type canonicalizing DONE@"
	 << ctxt.current_corpus()->get_path()
	 << "\n";

  ctxt.env()->canonicalization_is_done(true);

  if (ctxt.do_log())
    cerr << "sort functions and variables ...";
  ctxt.current_corpus()->sort_functions();
  ctxt.current_corpus()->sort_variables();
  if (ctxt.do_log())
    cerr << " DONE@" << ctxt.current_corpus()->get_path() <<" \n";

  return ctxt.current_corpus();
}

/// Canonicalize a type if it's suitable for early canonicalizing, or,
/// if it's not, schedule it for late canonicalization, after the
/// debug info of the current translation unit has been fully read.
///
/// A (composite) type is deemed suitable for early canonicalizing iff
/// all of its sub-types are canonicalized themselve.  Non composite
/// types are always deemed suitable for early canonicalization.
///
/// @param die the type DIE to consider for canonicalization.  Note
/// that this DIE must have been associated with its type using the
/// function read_context::associate_die_to_type() prior to calling
/// this function.
///
/// @param ctxt the @ref read_context to use.
static void
maybe_canonicalize_type(Dwarf_Die *die, read_context&	ctxt)
{
  die_source source;
  ABG_ASSERT(ctxt.get_die_source(die, source));

  size_t die_offset = dwarf_dieoffset(die);
  type_base_sptr t = ctxt.lookup_type_from_die(die);

  if (!t)
    return;

  type_base_sptr peeled_type =
    peel_typedef_pointer_or_reference_type(t, /*peel_qual_types=*/false);
  if (is_class_type(peeled_type)
      || is_union_type(peeled_type)
      || is_function_type(peeled_type)
      || is_array_type(peeled_type)
      || is_qualified_type(peeled_type))
    // We delay canonicalization of classes/unions or typedef,
    // pointers, references and array to classes/unions.  This is
    // because the (underlying) class might not be finished yet and we
    // might not be able to able detect it here (thinking about
    // classes that are work-in-progress, or classes that might be
    // later amended by some DWARF construct).  So we err on the safe
    // side.  We also delay canonicalization for array and qualified
    // types because they can be edited (in particular by
    // maybe_strip_qualification) after they are initially built.
    ctxt.schedule_type_for_late_canonicalization(die);
  else if ((is_function_type(t)
	    && ctxt.is_wip_function_type_die_offset(die_offset, source))
	   || type_has_non_canonicalized_subtype(t))
    ctxt.schedule_type_for_late_canonicalization(die);
  else
    canonicalize(t);
}

/// If a given decl is a member type declaration, set its access
/// specifier from the DIE that represents it.
///
/// @param member_type_declaration the member type declaration to
/// consider.
static void
maybe_set_member_type_access_specifier(decl_base_sptr member_type_declaration,
				       Dwarf_Die* die)
{
  if (is_type(member_type_declaration)
      && is_member_decl(member_type_declaration))
    {
      class_or_union* scope =
	is_class_or_union_type(member_type_declaration->get_scope());
      ABG_ASSERT(scope);

      access_specifier access = private_access;
      if (class_decl* cl = is_class_type(scope))
	if (cl->is_struct())
	  access = public_access;

      die_access_specifier(die, access);
      set_member_access_specifier(member_type_declaration, access);
    }
}

/// Build an IR node from a given DIE and add the node to the current
/// IR being build and held in the read_context.  Doing that is called
/// "emitting an IR node for the DIE".
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param scope the scope under which the resulting IR node has to be
/// added.
///
/// @param called_from_public_decl set to yes if this function is
/// called from the functions used to build a public decl (functions
/// and variables).  In that case, this function accepts building IR
/// nodes representing types.  Otherwise, this function only creates
/// IR nodes representing public decls (functions and variables).
/// This is done to avoid emitting IR nodes for types that are not
/// referenced by public functions or variables.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @param is_required_decl_spec if true, it means the ir node to
/// build is for a decl that is a specification for another decl that
/// is concrete.  If you don't know what this is, set it to false.
///
/// @return the resulting IR node.
static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       scope_decl*	scope,
		       bool		called_from_public_decl,
		       size_t		where_offset,
		       bool		is_required_decl_spec)
{
  type_or_decl_base_sptr result;

  if (!die || !scope)
    return result;

  int tag = dwarf_tag(die);

  if (!called_from_public_decl)
    {
      if (ctxt.load_all_types() && die_is_type(die))
	/* We were instructed to load debug info for all types,
	   included those that are not reachable from a public
	   declaration.  So load the debug info for this type.  */;
      else if (tag != DW_TAG_subprogram
	       && tag != DW_TAG_variable
	       && tag != DW_TAG_member
	       && tag != DW_TAG_namespace)
	return result;
    }

  die_source source_of_die;
  ABG_ASSERT(ctxt.get_die_source(die, source_of_die));

  if ((result = ctxt.lookup_decl_from_die_offset(dwarf_dieoffset(die),
						 source_of_die)))
    return result;

  switch (tag)
    {
      // Type DIEs we support.
    case DW_TAG_base_type:
      if (type_decl_sptr t = build_type_decl(ctxt, die, where_offset))
	{
	  result =
	    add_decl_to_scope(t, ctxt.cur_transl_unit()->get_global_scope());
	  canonicalize(t);
	}
      break;

    case DW_TAG_typedef:
      {
	typedef_decl_sptr t = build_typedef_type(ctxt, die,
						 called_from_public_decl,
						 where_offset);
	result = add_decl_to_scope(t, scope);
	if (result)
	  {
	    maybe_set_member_type_access_specifier(is_decl(result), die);
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;

    case DW_TAG_pointer_type:
      {
	pointer_type_def_sptr p =
	  build_pointer_type_def(ctxt, die,
				 called_from_public_decl,
				 where_offset);
	if (p)
	  {
	    result =
	      add_decl_to_scope(p, ctxt.cur_transl_unit()->get_global_scope());
	    ABG_ASSERT(result->get_translation_unit());
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;

    case DW_TAG_reference_type:
    case DW_TAG_rvalue_reference_type:
      {
	reference_type_def_sptr r =
	  build_reference_type(ctxt, die,
			       called_from_public_decl,
			       where_offset);
	if (r)
	  {
	    result =
	      add_decl_to_scope(r, ctxt.cur_transl_unit()->get_global_scope());

	    ctxt.associate_die_to_type(die, r, where_offset);
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;

    case DW_TAG_const_type:
    case DW_TAG_volatile_type:
    case DW_TAG_restrict_type:
      {
	type_base_sptr q =
	  build_qualified_type(ctxt, die,
			       called_from_public_decl,
			       where_offset);
	if (q)
	  {
	    // Strip some potentially redundant type qualifiers from
	    // the qualified type we just built.
	    decl_base_sptr d = maybe_strip_qualification(is_qualified_type(q),
							 ctxt);
	    if (!d)
	      d = get_type_declaration(q);
	    ABG_ASSERT(d);
	    type_base_sptr ty = is_type(d);
	    // Associate the die to type ty again because 'ty'might be
	    // different from 'q', because 'ty' is 'q' possibly
	    // stripped from some redundant type qualifier.
	    ctxt.associate_die_to_type(die, ty, where_offset);
	    result =
	      add_decl_to_scope(d, ctxt.cur_transl_unit()->get_global_scope());
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;

    case DW_TAG_enumeration_type:
      {
	if (!type_is_suppressed(ctxt, scope, die))
	  {
	    enum_type_decl_sptr e = build_enum_type(ctxt, die, scope,
						    where_offset);
	    result = add_decl_to_scope(e, scope);
	    if (result)
	      {
		maybe_set_member_type_access_specifier(is_decl(result), die);
		maybe_canonicalize_type(die, ctxt);
	      }
	  }
      }
      break;

    case DW_TAG_class_type:
    case DW_TAG_structure_type:
      {
	bool type_is_private = false;
	bool type_suppressed=
	  type_is_suppressed(ctxt, scope, die, type_is_private);

	if (type_suppressed && type_is_private)
	  // The type is suppressed because it's private.  If other
	  // non-suppressed and declaration-only instances of this
	  // type exist in the current corpus, then it means those
	  // non-suppressed instances are opaque versions of the
	  // suppressed private type.  Lets return one of these opaque
	  // types then.
	  result = get_opaque_version_of_type(ctxt, scope, die, where_offset);
	else if (!type_suppressed)
	  {
	    Dwarf_Die spec_die;
	    scope_decl_sptr scop;
	    class_decl_sptr klass;
	    if (die_die_attribute(die, DW_AT_specification, spec_die))
	      {
		scope_decl_sptr skope =
		  get_scope_for_die(ctxt, &spec_die,
				    called_from_public_decl,
				    where_offset);
		ABG_ASSERT(skope);
		decl_base_sptr cl =
		  is_decl(build_ir_node_from_die(ctxt, &spec_die,
						 skope.get(),
						 called_from_public_decl,
						 where_offset));
		ABG_ASSERT(cl);
		klass = dynamic_pointer_cast<class_decl>(cl);
		ABG_ASSERT(klass);

		klass =
		  add_or_update_class_type(ctxt, die,
					   skope.get(),
					   tag == DW_TAG_structure_type,
					   klass,
					   called_from_public_decl,
					   where_offset);
	      }
	    else
	      klass =
		add_or_update_class_type(ctxt, die, scope,
					 tag == DW_TAG_structure_type,
					 class_decl_sptr(),
					 called_from_public_decl,
					 where_offset);
	    result = klass;
	    if (klass)
	      {
		maybe_set_member_type_access_specifier(klass, die);
		maybe_canonicalize_type(die, ctxt);
	      }
	  }
      }
      break;
    case DW_TAG_union_type:
      if (!type_is_suppressed(ctxt, scope, die))
	{
	  union_decl_sptr union_type =
	    add_or_update_union_type(ctxt, die, scope,
				     union_decl_sptr(),
				     called_from_public_decl,
				     where_offset);
	  if (union_type)
	    {
	      maybe_set_member_type_access_specifier(union_type, die);
	      maybe_canonicalize_type(die, ctxt);
	    }
	  result = union_type;
	}
      break;
    case DW_TAG_string_type:
      break;
    case DW_TAG_subroutine_type:
      {
	function_type_sptr f = build_function_type(ctxt, die,
						   class_decl_sptr(),
						   where_offset);
	if (f)
	  {
	    result = f;
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;
    case DW_TAG_array_type:
      {
	array_type_def_sptr a = build_array_type(ctxt,
						 die,
						 called_from_public_decl,
						 where_offset);
	if (a)
	  {
	    result =
	      add_decl_to_scope(a, ctxt.cur_transl_unit()->get_global_scope());
	    ctxt.associate_die_to_type(die, a, where_offset);
	    maybe_canonicalize_type(die, ctxt);
	  }
	break;
      }
    case DW_TAG_subrange_type:
      {
	// If we got here, this means the subrange type is a "free
	// form" defined in the global namespace of the current
	// translation unit, like what is found in Ada.
	array_type_def::subrange_sptr s =
	  build_subrange_type(ctxt, die, where_offset);
	if (s)
	  {
	    result =
	      add_decl_to_scope(s, ctxt.cur_transl_unit()->get_global_scope());
	    ctxt.associate_die_to_type(die, s, where_offset);
	    maybe_canonicalize_type(die, ctxt);
	  }
      }
      break;
    case DW_TAG_packed_type:
      break;
    case DW_TAG_set_type:
      break;
    case DW_TAG_file_type:
      break;
    case DW_TAG_ptr_to_member_type:
      break;
    case DW_TAG_thrown_type:
      break;
    case DW_TAG_interface_type:
      break;
    case DW_TAG_unspecified_type:
      break;
    case DW_TAG_shared_type:
      break;

    case DW_TAG_compile_unit:
      // We shouldn't reach this point b/c this should be handled by
      // build_translation_unit.
      ABG_ASSERT_NOT_REACHED;

    case DW_TAG_namespace:
    case DW_TAG_module:
      result = build_namespace_decl_and_add_to_ir(ctxt, die, where_offset);
      break;

    case DW_TAG_variable:
    case DW_TAG_member:
      {
	Dwarf_Die spec_die;
	bool var_is_cloned = false;

	if (tag == DW_TAG_member)
	  ABG_ASSERT(!is_c_language(ctxt.cur_transl_unit()->get_language()));

	if (die_die_attribute(die, DW_AT_specification, spec_die,false)
	    || (var_is_cloned = die_die_attribute(die, DW_AT_abstract_origin,
						  spec_die, false)))
	  {
	    scope_decl_sptr spec_scope = get_scope_for_die(ctxt, &spec_die,
							   called_from_public_decl,
							   where_offset);
	    if (spec_scope)
	      {
		decl_base_sptr d =
		  is_decl(build_ir_node_from_die(ctxt, &spec_die,
						 spec_scope.get(),
						 called_from_public_decl,
						 where_offset,
						 /*is_required_decl_spec=*/true));
		if (d)
		  {
		    var_decl_sptr m =
		      dynamic_pointer_cast<var_decl>(d);
		    if (var_is_cloned)
		      m = m->clone();
		    m = build_var_decl(ctxt, die, where_offset, m);
		    if (is_data_member(m))
		      {
			set_member_is_static(m, true);
			ctxt.associate_die_to_decl(die, m, where_offset,
						   /*associate_by_repr=*/false);
		      }
		    else
		      {
			ABG_ASSERT(has_scope(m));
			ctxt.var_decls_to_re_add_to_tree().push_back(m);
		      }
		    ABG_ASSERT(m->get_scope());
		    ctxt.maybe_add_var_to_exported_decls(m.get());
		    return m;
		  }
	      }
	  }
	else if (var_decl_sptr v =
		 build_or_get_var_decl_if_not_suppressed(ctxt, scope, die,
							 where_offset,
							 /*result=*/var_decl_sptr(),
							 is_required_decl_spec))
	  {
	    result = add_decl_to_scope(v, scope);
	    ABG_ASSERT(is_decl(result)->get_scope());
	    v = dynamic_pointer_cast<var_decl>(result);
	    ABG_ASSERT(v);
	    ABG_ASSERT(v->get_scope());
	    ctxt.var_decls_to_re_add_to_tree().push_back(v);
	    ctxt.maybe_add_var_to_exported_decls(v.get());
	  }
      }
      break;

    case DW_TAG_subprogram:
      {
	Dwarf_Die spec_die;
	Dwarf_Die abstract_origin_die;
	Dwarf_Die *interface_die = 0, *origin_die = 0;
	scope_decl_sptr interface_scope;
	if (die_is_artificial(die))
	  break;

	function_decl_sptr fn;
	bool has_spec = die_die_attribute(die, DW_AT_specification,
					  spec_die, true);
	bool has_abstract_origin =
	  die_die_attribute(die, DW_AT_abstract_origin,
			    abstract_origin_die, true);
	if (has_spec || has_abstract_origin)
	  {
	    interface_die =
	      has_spec
	      ? &spec_die
	      : &abstract_origin_die;
	    origin_die =
	      has_abstract_origin
	      ? &abstract_origin_die
	      : &spec_die;

	    string linkage_name = die_linkage_name(die);
	    string spec_linkage_name = die_linkage_name(interface_die);

	    interface_scope = get_scope_for_die(ctxt, interface_die,
						called_from_public_decl,
						where_offset);
	    if (interface_scope)
	      {
		decl_base_sptr d =
		  is_decl(build_ir_node_from_die(ctxt,
						 origin_die,
						 interface_scope.get(),
						 called_from_public_decl,
						 where_offset));
		if (d)
		  {
		    fn = dynamic_pointer_cast<function_decl>(d);
		    if (has_abstract_origin
			&& (linkage_name != spec_linkage_name))
		      // The current DIE has 'd' as abstract orign,
		      // and has a linkage name that is different
		      // from from the linkage name of 'd'.  That
		      // means, the current DIE represents a clone
		      // of 'd'.
		      fn = fn->clone();
		  }
	      }
	  }
	ctxt.scope_stack().push(scope);

	scope_decl* logical_scope =
	  interface_scope
	  ? interface_scope.get()
	  : scope;

	result = build_or_get_fn_decl_if_not_suppressed(ctxt, logical_scope,
							die, where_offset, fn);

	if (result && !fn)
	  result = add_decl_to_scope(is_decl(result), logical_scope);

	fn = is_function_decl(result);
	if (fn && is_member_function(fn))
	  {
	    class_decl_sptr klass(static_cast<class_decl*>(logical_scope),
				  sptr_utils::noop_deleter());
	    ABG_ASSERT(klass);
	    finish_member_function_reading(die, fn, klass, ctxt);
	  }

	if (fn)
	  {
	    ctxt.maybe_add_fn_to_exported_decls(fn.get());
	    ctxt.associate_die_to_decl(die, fn, where_offset,
				       /*associate_by_repr=*/false);
	    maybe_canonicalize_type(die, ctxt);
	  }

	ctxt.scope_stack().pop();
      }
      break;

    case DW_TAG_formal_parameter:
      // We should not read this case as it should have been dealt
      // with by build_function_decl above.
      ABG_ASSERT_NOT_REACHED;

    case DW_TAG_constant:
      break;
    case DW_TAG_enumerator:
      break;

    case DW_TAG_partial_unit:
    case DW_TAG_imported_unit:
      // For now, the DIEs under these are read lazily when they are
      // referenced by a public decl DIE that is under a
      // DW_TAG_compile_unit, so we shouldn't get here.
      ABG_ASSERT_NOT_REACHED;

      // Other declaration we don't really intend to support yet.
    case DW_TAG_dwarf_procedure:
    case DW_TAG_imported_declaration:
    case DW_TAG_entry_point:
    case DW_TAG_label:
    case DW_TAG_lexical_block:
    case DW_TAG_unspecified_parameters:
    case DW_TAG_variant:
    case DW_TAG_common_block:
    case DW_TAG_common_inclusion:
    case DW_TAG_inheritance:
    case DW_TAG_inlined_subroutine:
    case DW_TAG_with_stmt:
    case DW_TAG_access_declaration:
    case DW_TAG_catch_block:
    case DW_TAG_friend:
    case DW_TAG_namelist:
    case DW_TAG_namelist_item:
    case DW_TAG_template_type_parameter:
    case DW_TAG_template_value_parameter:
    case DW_TAG_try_block:
    case DW_TAG_variant_part:
    case DW_TAG_imported_module:
    case DW_TAG_condition:
    case DW_TAG_type_unit:
    case DW_TAG_template_alias:
    case DW_TAG_lo_user:
    case DW_TAG_MIPS_loop:
    case DW_TAG_format_label:
    case DW_TAG_function_template:
    case DW_TAG_class_template:
    case DW_TAG_GNU_BINCL:
    case DW_TAG_GNU_EINCL:
    case DW_TAG_GNU_template_template_param:
    case DW_TAG_GNU_template_parameter_pack:
    case DW_TAG_GNU_formal_parameter_pack:
    case DW_TAG_GNU_call_site:
    case DW_TAG_GNU_call_site_parameter:
    case DW_TAG_hi_user:
    default:
      break;
    }

  if (result && tag != DW_TAG_subroutine_type)
    ctxt.associate_die_to_decl(die, is_decl(result), where_offset,
			       /*associate_by_repr=*/false);

  return result;
}

///  Build the IR node for a void type.
///
///  @param ctxt the read context to use.
///
///  @return the void type node.
static decl_base_sptr
build_ir_node_for_void_type(read_context& ctxt)
{
  ir::environment* env = ctxt.env();
  ABG_ASSERT(env);
  type_base_sptr t = env->get_void_type();
  decl_base_sptr type_declaration = get_type_declaration(t);
  if (!has_scope(type_declaration))
    add_decl_to_scope(type_declaration,
		      ctxt.cur_transl_unit()->get_global_scope());
  canonicalize(t);
  return type_declaration;
}

/// Build an IR node from a given DIE and add the node to the current
/// IR being build and held in the read_context.  Doing that is called
/// "emitting an IR node for the DIE".
///
/// @param ctxt the read context.
///
/// @param die the DIE to consider.
///
/// @param called_from_public_decl set to yes if this function is
/// called from the functions used to build a public decl (functions
/// and variables).  In that case, this function accepts building IR
/// nodes representing types.  Otherwise, this function only creates
/// IR nodes representing public decls (functions and variables).
/// This is done to avoid emitting IR nodes for types that are not
/// referenced by public functions or variables.
///
/// @param where_offset the offset of the DIE where we are "logically"
/// positionned at, in the DIE tree.  This is useful when @p die is
/// e.g, DW_TAG_partial_unit that can be included in several places in
/// the DIE tree.
///
/// @return the resulting IR node.
static type_or_decl_base_sptr
build_ir_node_from_die(read_context&	ctxt,
		       Dwarf_Die*	die,
		       bool		called_from_public_decl,
		       size_t		where_offset)
{
  if (!die)
    return decl_base_sptr();

  if (is_c_language(ctxt.cur_transl_unit()->get_language()))
    {
      const scope_decl_sptr& scop = ctxt.global_scope();
      return build_ir_node_from_die(ctxt, die, scop.get(),
				    called_from_public_decl,
				    where_offset);
    }

  scope_decl_sptr scope = get_scope_for_die(ctxt, die,
					    called_from_public_decl,
					    where_offset);
  return build_ir_node_from_die(ctxt, die, scope.get(),
				called_from_public_decl,
				where_offset);
}

status
operator|(status l, status r)
{
  return static_cast<status>(static_cast<unsigned>(l)
			     | static_cast<unsigned>(r));
}

status
operator&(status l, status r)
{
  return static_cast<status>(static_cast<unsigned>(l)
			     & static_cast<unsigned>(r));
}

status&
operator|=(status& l, status r)
{
  l = l | r;
  return l;
}

status&
operator&=(status& l, status r)
{
  l = l & r;
  return l;
}

/// Emit a diagnostic status with english sentences to describe the
/// problems encoded in a given abigail::dwarf_reader::status, if
/// there is an error.
///
/// @param status the status to diagnose
///
/// @return a string containing sentences that describe the possible
/// errors encoded in @p s.  If there is no error to encode, then the
/// empty string is returned.
string
status_to_diagnostic_string(status s)
{
  string str;

  if (s & STATUS_DEBUG_INFO_NOT_FOUND)
    str += "could not find debug info\n";

  if (s & STATUS_ALT_DEBUG_INFO_NOT_FOUND)
    str += "could not find alternate debug info\n";

  if (s & STATUS_NO_SYMBOLS_FOUND)
    str += "could not load ELF symbols\n";

  return str;
}

/// Create a dwarf_reader::read_context.
///
/// @param elf_path the path to the elf file the context is to be used for.
///
/// @param debug_info_root_paths a pointer to the path to the root
/// directory under which the debug info is to be found for @p
/// elf_path.  Leave this to NULL if the debug info is not in a split
/// file.
///
/// @param environment the environment used by the current context.
/// This environment contains resources needed by the reader and by
/// the types and declarations that are to be created later.  Note
/// that ABI artifacts that are to be compared all need to be created
/// within the same environment.
///
/// Please also note that the life time of this environment object
/// must be greater than the life time of the resulting @ref
/// read_context the context uses resources that are allocated in the
/// environment.
///
/// @param load_all_types if set to false only the types that are
/// reachable from publicly exported declarations (of functions and
/// variables) are read.  If set to true then all types found in the
/// debug information are loaded.
///
/// @param linux_kernel_mode if set to true, then consider the special
/// linux kernel symbol tables when determining if a symbol is
/// exported or not.
///
/// @return a smart pointer to the resulting dwarf_reader::read_context.
read_context_sptr
create_read_context(const std::string&		elf_path,
		    const vector<char**>&	debug_info_root_paths,
		    ir::environment*		environment,
		    bool			load_all_types,
		    bool			linux_kernel_mode)
{
  // Create a DWARF Front End Library handle to be used by functions
  // of that library.
  read_context_sptr result(new read_context(elf_path, debug_info_root_paths,
					    environment, load_all_types,
					    linux_kernel_mode));
  return result;
}

/// Getter for the path to the binary this @ref read_context is for.
///
/// @return the path to the binary the @ref read_context is for.
const string&
read_context_get_path(const read_context& ctxt)
{return ctxt.elf_path();}

/// Re-initialize a read_context so that it can re-used to read
/// another binary.
///
/// @param ctxt the context to re-initialize.
///
/// @param elf_path the path to the elf file the context is to be used
/// for.
///
/// @param debug_info_root_path a pointer to the path to the root
/// directory under which the debug info is to be found for @p
/// elf_path.  Leave this to NULL if the debug info is not in a split
/// file.
///
/// @param environment the environment used by the current context.
/// This environment contains resources needed by the reader and by
/// the types and declarations that are to be created later.  Note
/// that ABI artifacts that are to be compared all need to be created
/// within the same environment.
///
/// Please also note that the life time of this environment object
/// must be greater than the life time of the resulting @ref
/// read_context the context uses resources that are allocated in the
/// environment.
///
/// @param load_all_types if set to false only the types that are
/// reachable from publicly exported declarations (of functions and
/// variables) are read.  If set to true then all types found in the
/// debug information are loaded.
///
/// @param linux_kernel_mode if set to true, then consider the special
/// linux kernel symbol tables when determining if a symbol is
/// exported or not.
///
/// @return a smart pointer to the resulting dwarf_reader::read_context.
void
reset_read_context(read_context_sptr	&ctxt,
		   const std::string&	 elf_path,
		   const vector<char**>& debug_info_root_path,
		   ir::environment*	 environment,
		   bool		 read_all_types,
		   bool		 linux_kernel_mode)
{
  if (ctxt)
    ctxt->initialize(elf_path, debug_info_root_path, environment,
		     read_all_types, linux_kernel_mode);
}

/// Add suppressions specifications to the set of suppressions to be
/// used during the construction of the ABI internal representation
/// (the ABI corpus) from ELF and DWARF.
///
/// During the construction of the ABI corpus, ABI artifacts that
/// match the a given suppression specification are dropped on the
/// floor; that is, they are discarded and won't be part of the final
/// ABI corpus.  This is a way to reduce the amount of data held by
/// the final ABI corpus.
///
/// Note that the suppression specifications provided to this function
/// are only considered during the construction of the ABI corpus.
/// For instance, they are not taken into account during e.g
/// comparisons of two ABI corpora that might happen later.  If you
/// want to apply suppression specificatins to the comparison (or
/// reporting) of ABI corpora please refer to the documentation of the
/// @ref diff_context type to learn how to set suppressions that are
/// to be used in that context.
///
/// @param ctxt the context that is going to be used by functions that
/// read ELF and DWARF information to construct and ABI corpus.
///
/// @param supprs the suppression specifications to be applied during
/// the construction of the ABI corpus.
void
add_read_context_suppressions(read_context& ctxt,
			      const suppr::suppressions_type& supprs)
{
  for (suppr::suppressions_type::const_iterator i = supprs.begin();
       i != supprs.end();
       ++i)
    if ((*i)->get_drops_artifact_from_ir())
      ctxt.get_suppressions().push_back(*i);
}

/// Set the @ref corpus_group being created to the current read context.
///
/// @param ctxt the read_context to consider.
///
/// @param group the @ref corpus_group to set.
void
set_read_context_corpus_group(read_context& ctxt,
			      corpus_group_sptr& group)
{
  ctxt.cur_corpus_group_ = group;
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible from an elf file, stuff them into a libabigail ABI
/// Corpus and return it.
///
/// @param ctxt the context to use for reading the elf file.
///
/// @param resulting_corp a pointer to the resulting abigail::corpus.
///
/// @return the resulting status.
corpus_sptr
read_corpus_from_elf(read_context& ctxt, status& status)
{
  status = STATUS_UNKNOWN;

  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    status |= STATUS_DEBUG_INFO_NOT_FOUND;

  {
    string alt_di_path;
    if (refers_to_alt_debug_info(ctxt, alt_di_path) && !ctxt.alt_dwarf())
      status |= STATUS_ALT_DEBUG_INFO_NOT_FOUND;
  }

  if (!get_ignore_symbol_table(ctxt))
    {
      ctxt.load_elf_properties();
      // Read the symbols for publicly defined decls
      if (!ctxt.load_symbol_maps())
	status |= STATUS_NO_SYMBOLS_FOUND;
    }

  if (// If no elf symbol was found ...
      status & STATUS_NO_SYMBOLS_FOUND
      // ... or if debug info was found but not the required alternate
      // debug info ...
      || ((status & STATUS_ALT_DEBUG_INFO_NOT_FOUND)
	  && !(status & STATUS_DEBUG_INFO_NOT_FOUND)))
    // ... then we cannot handle the binary.
    return corpus_sptr();

  // Read the variable and function descriptions from the debug info
  // we have, through the dwfl handle.
  corpus_sptr corp = read_debug_info_into_corpus(ctxt);

  status |= STATUS_OK;

  return corp;
}

/// Read a corpus and add it to a given @ref corpus_group.
///
/// @param ctxt the reading context to consider.
///
/// @param group the @ref corpus_group to add the new corpus to.
///
/// @param status output parameter. The status of the read.  It is set
/// by this function upon its completion.
corpus_sptr
read_and_add_corpus_to_group_from_elf(read_context& ctxt,
				      corpus_group& group,
				      status& status)
{
  corpus_sptr result;
  corpus_sptr corp = read_corpus_from_elf(ctxt, status);
  if (status & STATUS_OK)
    {
      group.add_corpus(corp);
      result = corp;
    }

  return result;
}

/// Read all @ref abigail::translation_unit possible from the debug info
/// accessible from an elf file, stuff them into a libabigail ABI
/// Corpus and return it.
///
/// @param elf_path the path to the elf file.
///
/// @param debug_info_root_paths a vector of pointers to root paths
/// under which to look for the debug info of the elf files that are
/// later handled by the Dwfl.  This for cases where the debug info is
/// split into a different file from the binary we want to inspect.
/// On Red Hat compatible systems, this root path is usually
/// /usr/lib/debug by default.  If this argument is set to NULL, then
/// "./debug" and /usr/lib/debug will be searched for sub-directories
/// containing the debug info file.
///
/// @param environment the environment used by the current context.
/// This environment contains resources needed by the reader and by
/// the types and declarations that are to be created later.  Note
/// that ABI artifacts that are to be compared all need to be created
/// within the same environment.  Also, the lifetime of the
/// environment must be greater than the lifetime of the resulting
/// corpus because the corpus uses resources that are allocated in the
/// environment.
///
/// @param load_all_types if set to false only the types that are
/// reachable from publicly exported declarations (of functions and
/// variables) are read.  If set to true then all types found in the
/// debug information are loaded.
///
/// @param resulting_corp a pointer to the resulting abigail::corpus.
///
/// @return the resulting status.
corpus_sptr
read_corpus_from_elf(const std::string& elf_path,
		     const vector<char**>& debug_info_root_paths,
		     ir::environment*	environment,
		     bool		load_all_types,
		     status&		status)
{
  read_context_sptr c = create_read_context(elf_path,
					    debug_info_root_paths,
					    environment,
					    load_all_types);
  read_context& ctxt = *c;
  return read_corpus_from_elf(ctxt, status);
}

/// Look into the symbol tables of a given elf file and see if we find
/// a given symbol.
///
/// @param env the environment we are operating from.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param symbol_name the name of the symbol to look for.
///
/// @param demangle if true, try to demangle the symbol name found in
/// the symbol table.
///
/// @param syms the vector of symbols found with the name @p symbol_name.
///
/// @return true iff the symbol was found among the publicly exported
/// symbols of the ELF file.
bool
lookup_symbol_from_elf(const environment*		env,
		       const string&			elf_path,
		       const string&			symbol_name,
		       bool				demangle,
		       vector<elf_symbol_sptr>&	syms)

{
  if (elf_version(EV_CURRENT) == EV_NONE)
    return false;

  int fd = open(elf_path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  struct stat s;
  if (fstat(fd, &s))
    return false;

  Elf* elf = elf_begin(fd, ELF_C_READ, 0);
  if (elf == 0)
    return false;

  bool value = lookup_symbol_from_elf(env, elf, symbol_name,
				      demangle, syms);
  elf_end(elf);
  close(fd);

  return value;
}

/// Look into the symbol tables of an elf file to see if a public
/// function of a given name is found.
///
/// @param env the environment we are operating from.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param symbol_name the name of the function to look for.
///
/// @param syms the vector of public function symbols found with the
/// name @p symname.
///
/// @return true iff a function with symbol name @p symbol_name is
/// found.
bool
lookup_public_function_symbol_from_elf(const environment*		env,
				       const string&			path,
				       const string&			symname,
				       vector<elf_symbol_sptr>&	syms)
{
  if (elf_version(EV_CURRENT) == EV_NONE)
    return false;

  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return false;

  struct stat s;
  if (fstat(fd, &s))
    return false;

  Elf* elf = elf_begin(fd, ELF_C_READ, 0);
  if (elf == 0)
    return false;

  bool value = lookup_public_function_symbol_from_elf(env, elf, symname, syms);
  elf_end(elf);
  close(fd);

  return value;
}

/// Check if the underlying elf file refers to an alternate debug info
/// file associated to it.
///
/// Note that "alternate debug info sections" is a GNU extension as
/// of DWARF4 and is described at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @param ctxt the context used to read the elf file.
///
/// @param alt_di the path to the alternate debug info file.  This is
/// set iff the function returns true.
///
/// @return true if the ELF file refers to an alternate debug info
/// file.
bool
refers_to_alt_debug_info(const read_context&	ctxt,
			 string&		alt_di_path)
{
  if (!ctxt.alt_debug_info_path().empty())
    {
      alt_di_path = ctxt.alt_debug_info_path();
      return true;
    }
  return false;
}

/// Check if the underlying elf file has an alternate debug info file
/// associated to it.
///
/// Note that "alternate debug info sections" is a GNU extension as
/// of DWARF4 and is described at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @param ctxt the read_context to use to handle the underlying elf file.
///
/// @param has_alt_di out parameter.  This is set to true upon
/// succesful completion of the function iff an alternate debug info
/// file was found, false otherwise.  Note thas this parameter is set
/// only if the function returns STATUS_OK.
///
/// @param alt_debug_info_path if the function returned STATUS_OK and
/// if @p has been set to true, then this parameter contains the path
/// to the alternate debug info file found.
///
/// return STATUS_OK upon successful completion, false otherwise.
status
has_alt_debug_info(read_context&	ctxt,
		   bool&		has_alt_di,
		   string&		alt_debug_info_path)
{
  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    return STATUS_DEBUG_INFO_NOT_FOUND;

  if (ctxt.alt_dwarf())
    {
      has_alt_di = true;
      alt_debug_info_path = ctxt.alt_debug_info_path();
    }
  else
    has_alt_di = false;

  return STATUS_OK;
}

/// Check if a given elf file has an alternate debug info file
/// associated to it.
///
/// Note that "alternate debug info sections" is a GNU extension as
/// of DWARF4 and is described at
/// http://www.dwarfstd.org/ShowIssue.php?issue=120604.1.
///
/// @param elf_path the path to the elf file to consider.
///
/// @param a pointer to the root directory under which the split debug info
/// file associated to elf_path is to be found.  This has to be NULL
/// if the debug info file is not in a split file.
///
/// @param has_alt_di out parameter.  This is set to true upon
/// succesful completion of the function iff an alternate debug info
/// file was found, false otherwise.  Note thas this parameter is set
/// only if the function returns STATUS_OK.
///
/// @param alt_debug_info_path if the function returned STATUS_OK and
/// if @p has been set to true, then this parameter contains the path
/// to the alternate debug info file found.
///
/// return STATUS_OK upon successful completion, false otherwise.
status
has_alt_debug_info(const string&	elf_path,
		   char**		debug_info_root_path,
		   bool&		has_alt_di,
		   string&		alt_debug_info_path)
{
  vector<char**> di_roots;
  di_roots.push_back(debug_info_root_path);
  read_context_sptr c = create_read_context(elf_path, di_roots, 0);
  read_context& ctxt = *c;

  // Load debug info from the elf path.
  if (!ctxt.load_debug_info())
    return STATUS_DEBUG_INFO_NOT_FOUND;

  if (ctxt.alt_dwarf())
    {
      has_alt_di = true;
      alt_debug_info_path = ctxt.alt_debug_info_path();
    }
  else
    has_alt_di = false;

  return STATUS_OK;
}

/// Fetch the SONAME ELF property from an ELF binary file.
///
/// @param path The path to the elf file to consider.
///
/// @param soname out parameter. Set to the SONAME property of the
/// binary file, if it present in the ELF file.
///
/// return false if an error occured while looking for the SONAME
/// property in the binary, true otherwise.
bool
get_soname_of_elf_file(const string& path, string &soname)
{

  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1)
    return false;

  elf_version (EV_CURRENT);
  Elf* elf = elf_begin (fd, ELF_C_READ_MMAP, NULL);

  GElf_Ehdr ehdr_mem;
  GElf_Ehdr* ehdr = gelf_getehdr (elf, &ehdr_mem);
  if (ehdr == NULL)
    return false;

  for (int i = 0; i < ehdr->e_phnum; ++i)
    {
      GElf_Phdr phdr_mem;
      GElf_Phdr* phdr = gelf_getphdr (elf, i, &phdr_mem);

      if (phdr != NULL && phdr->p_type == PT_DYNAMIC)
        {
          Elf_Scn* scn = gelf_offscn (elf, phdr->p_offset);
          GElf_Shdr shdr_mem;
          GElf_Shdr* shdr = gelf_getshdr (scn, &shdr_mem);
          int maxcnt = (shdr != NULL
                        ? shdr->sh_size / shdr->sh_entsize : INT_MAX);
          ABG_ASSERT (shdr == NULL || shdr->sh_type == SHT_DYNAMIC);
          Elf_Data* data = elf_getdata (scn, NULL);
          if (data == NULL)
            break;

          for (int cnt = 0; cnt < maxcnt; ++cnt)
            {
              GElf_Dyn dynmem;
              GElf_Dyn* dyn = gelf_getdyn (data, cnt, &dynmem);
              if (dyn == NULL)
                continue;

              if (dyn->d_tag == DT_NULL)
                break;

              if (dyn->d_tag != DT_SONAME)
                continue;

              soname = elf_strptr (elf, shdr->sh_link, dyn->d_un.d_val);
              break;
            }
          break;
        }
    }

  elf_end(elf);
  close(fd);

  return true;
}

/// Get the type of a given elf type.
///
/// @param path the absolute path to the ELF file to analyzed.
///
/// @param type the kind of the ELF file designated by @p path.
///
/// @param out parameter.  Is set to the type of ELF file of @p path.
/// This parameter is set iff the function returns true.
///
/// @return true iff the file could be opened and analyzed.
bool
get_type_of_elf_file(const string& path, elf_type& type)
{
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1)
    return false;

  elf_version (EV_CURRENT);
  Elf *elf = elf_begin (fd, ELF_C_READ_MMAP, NULL);
  type = elf_file_type(elf);
  elf_end(elf);
  close(fd);

  return true;
}

}// end namespace dwarf_reader

}// end namespace abigail
