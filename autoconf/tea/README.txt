This is the SQLite extension for Tcl using the Tcl Extension
Architecture (TEA).  For additional information on SQLite see

        http://www.sqlite.org/

-------------------------------------------------------------------
Update 2024-10-11:

A better way to build the TCL extension for SQLite is to use the
canonical source code tarball.  For Unix:

   ./configure --with-tclsh=$(TCLSH)
   make tclextension-install

For Windows:

   nmake /f Makefile.msc tclextension-install TCLSH_CMD=$(TCLSH)

In both of the above, replace $(TCLSH) with the full pathname of
of the tclsh that you want the SQLite extension to work with.

This TEA builder is antiquated.  It does not work for TCL9.  The
SQLite devs don't know how to fix it.  If you would like to help
fix it, contact us.
------------------------------------------------------------------


UNIX BUILD
==========

Building under most UNIX systems is easy, just run the configure script
and then run make. For more information about the build process, see
the tcl/unix/README file in the Tcl src dist. The following minimal
example will install the extension in the /opt/tcl directory.

	$ cd sqlite-*-tea
	$ ./configure --prefix=/opt/tcl
	$ make
	$ make install

WINDOWS BUILD
=============

The recommended method to build extensions under windows is to use the
Msys + Mingw build process. This provides a Unix-style build while
generating native Windows binaries. Using the Msys + Mingw build tools
means that you can use the same configure script as per the Unix build
to create a Makefile. See the tcl/win/README file for the URL of
the Msys + Mingw download.

If you have VC++ then you may wish to use the files in the win
subdirectory and build the extension using just VC++. These files have
been designed to be as generic as possible but will require some
additional maintenance by the project developer to synchronise with
the TEA configure.in and Makefile.in files. Instructions for using the
VC++ makefile are written in the first part of the Makefile.vc
file.
