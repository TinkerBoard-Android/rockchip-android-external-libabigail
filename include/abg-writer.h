// -*- mode: C++ -*-
//
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

/// @file

#ifndef __ABG_WRITER_H__
#define __ABG_WRITER_H__

#include <iosfwd>
#include "abg-corpus.h"

namespace abigail
{
/// Namespace for writer.
namespace writer
{
bool 
write_to_ostream(const translation_unit& tu, std::ostream& out);
}//end namespace writer

}// end namespace abigail
#endif //__ABG_WRITER_H__
