# ABIGAIL_INIT
#
# Handle the detection of the libabigail header and library files on
# the system.  If these are present, set the 'abigailinc' variable to
# the compiler option necessary to locate the headers of the library;
# also set the 'abigaillibs' variable to the compiler/linker option
# necessary to locate the library.
#
# Note that this macro defines the handling of --with-abigail,
# --with-abigail-include, --with-abigail-lib and
# --enable-abigail-version-check switches to the configure script.
AC_DEFUN([ABIGAIL_INIT],
[
 AC_ARG_WITH([abigail],
	     [AS_HELP_STRING([--with-abigail],
			     [Prefix directory for abigail library])],
	     [],
	     [])

 AC_ARG_WITH([abigail-include],
	     [AS_HELP_STRING([--with-abigail-include],
			     [Directory for installed abigail include files])],
	     [],
	     [])

 AC_ARG_WITH([abigail-lib],
	     [AS_HELP_STRING([--with-abigail-lib],
			     [Directory for installed abigail library])],
	     [],
	     [])

 AC_ARG_ENABLE(abigail-version-check,
	       [AS_HELP_STRING([--enable-abigail-version-check],
			       [Enable check for libabigail version])],
	       [ENABLE_ABIGAIL_VERSION_CHECK=$enableval],
	       [ENABLE_ABIGAIL_VERSION_CHECK=yes])

 if test x$with_abigail != x -a x$with_abigail != xno; then
   abigailinc="-I$with_abigail/include"
   abigaillibs="-L$with_abigail/lib"
   found_abigail_lib=yes
   found_abigail_inc=yes
 fi

 if test x$with_abigail_include != x -a x$with_abigail != xno; then
   abigailinc="-I$with_abigail_include"
   found_abigail_inc=yes
 fi

 if test x$with_abigail_lib != x -a x$with_abigail != xno; then
   abigaillibs="-L$with_abigail_lib"
   found_abigail_lib=yes
 fi

 if test x$abigaillibs = x; then
    AC_CHECK_LIB(abigail, abigail_get_library_version,
    		 [found_abigail_lib=yes], [], [])
 fi

 if test x$found_abigail_lib = xyes; then
    abigaillibs="$abigaillibs -labigail"
 fi

 if test x$abigailinc = x; then
    AC_LANG_PUSH(C++)
    AC_CHECK_HEADER([libabigail/abg-version.h],
		    [found_abigail_inc=yes], [], [])
    AC_LANG_POP(C++)
 fi

 if test x$found_abigail_lib = xyes -a x$found_abigail_inc = xyes; then
    HAVE_LIBABIGAIL=yes
 else
    HAVE_LIBABIGAIL=no
 fi
]
)

# IF_ABIGAIL_PRESENT(ACTION-IF-PRESENT)
AC_DEFUN([IF_ABIGAIL_PRESENT],
[
 AC_REQUIRE([ABIGAIL_INIT])

 if test x$HAVE_LIBABIGAIL = xyes; then
   $1
 fi
]
)

# IF_ABIGAIL_NOT_PRESENT(ACTION-IF-NOT-PRESENT)
AC_DEFUN([IF_ABIGAIL_NOT_PRESENT],
[
  AC_REQUIRE([ABIGAIL_INIT])
  if test x$HAVE_LIBABIGAIL != xyes; then
     $1
  fi
]
)

# ABIGAIL_CHECK_VERSION(MAJOR, MINOR)
# 
# Test the whether the found major and minor version numbers of the
# found abigail library is compatible with the MAJOR.MINOR version
# number given in argument.  The result of the test ('yes' or 'no') is
# put in the variable has_right_abigail_version.
AC_DEFUN([ABIGAIL_CHECK_VERSION],
[
 AC_REQUIRE([ABIGAIL_INIT])	

 if test x$ENABLE_ABIGAIL_VERSION_CHECK = xyes; then
   _abigail_saved_CXXFLAGS=$CXXFLAGLS
   _abigail_saved_LDFLAGS=$LDFLAGS

   CXXFLAGS="$abigailinc"
   LDFLAGS="$abigaillibs"

   AC_MSG_CHECKING([for version $1.$2 of libabigail])
   AC_LANG_PUSH(C++)
   AC_COMPILE_IFELSE([AC_LANG_PROGRAM([#include "libabigail/abg-version.h"],
				      [#if ABIGAIL_VERSION_MAJOR != $1 || ABIGAIL_VERSION_MINOR < $2
				          choke here
				       #endif
				      ])
		      		      ],
		     has_right_abigail_version=yes,
		     has_right_abigail_version=no)
   AC_LANG_POP(C++)
   AC_MSG_RESULT([$has_right_abigail_version])

   CXXFLAGS=$_abigail_saved_CXXFLAGS
   LDFLAGS=$_abigail_saved_LDFLAGS
 fi
]
)
