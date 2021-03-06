AC_INIT([ecoach], [1.67], [https://bugs.maemo.org/enter_bug.cgi?product=ecoach])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.17])
AC_PROG_CC
AC_PROG_CXX
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([ po/Makefile.in
		 Makefile
		 src/Makefile
		 gfx/Makefile
		 gfx/26x26/Makefile
		 gfx/scalable/Makefile
		 data/Makefile
		 data/ecoach.service
		 data/ecoach.desktop
		 ])

AC_PATH_PROG([GLIB_GENMARSHAL], [glib-genmarshal])

PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.12.12)
AC_SUBST(GTREAH_CFLAGS)
AC_SUBST(GTHREAD_LIBS)

PKG_CHECK_MODULES(GTK, gtk+-2.0 >= 2.10.0)
AC_SUBST(GTK_CFLAGS)
AC_SUBST(GTK_LIBS)

PKG_CHECK_MODULES(HILDON, hildon-1 >= 1.99.0)
AC_SUBST(HILDON_CFLAGS)
AC_SUBST(HILDON_LIBS)

PKG_CHECK_MODULES(HILDONFM, hildon-fm-2 >= 1.9.49)
AC_SUBST(HILDONFM_CFLAGS)
AC_SUBST(HILDONFM_LIBS)

PKG_CHECK_MODULES(BLUEZ, bluez >= 3.20)
AC_SUBST(BLUEZ_CFLAGS)
AC_SUBST(BLUEZ_LIBS)

PKG_CHECK_MODULES(DBUS, dbus-glib-1 >= 0.74)
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

PKG_CHECK_MODULES(GCONF, gconf-2.0 >= 2.16.0)
AC_SUBST(GCONF_CFLAGS)
AC_SUBST(GCONF_LIBS)

PKG_CHECK_MODULES(OSSO, libosso >= 2.0)
AC_SUBST(OSSO_CFLAGS)
AC_SUBST(OSSO_LIBS)

PKG_CHECK_MODULES(LOCATION, liblocation >= 0.26)
AC_SUBST(LOCATION_CFLAGS)
AC_SUBST(LOCATION_LIBS)

PKG_CHECK_MODULES(LIBXML2, libxml-2.0 >= 2.6.27)
AC_SUBST(LIBXML_CFLAGS)
AC_SUBST(LIBXML_LIBS)

PKG_CHECK_MODULES(LIBSOUP, libsoup-2.4 )
AC_SUBST(LIBSOUP_CFLAGS)
AC_SUBST(LIBSOUP_LIBS)

PKG_CHECK_MODULES(CALENDAR, calendar-backend)
AC_SUBST(CALENDAR_CFLAGS)
AC_SUBST(CALENDAR_LIBS)

PKG_CHECK_MODULES(OAUTH, oauth)
AC_SUBST(OAUTH_CFLAGS)
AC_SUBST(OAUTH_LIBS)

AC_ARG_ENABLE(ecg-view,
	      [  --enable-ecg-view		Enable ecg view], , enable_ecg_view=no )

AM_CONDITIONAL([WANT_ECG_VIEW],[test "x${enable_ecg_view}" = "xyes"])

if test "${enable_ecg_view}" = "yes" ; then
	AC_DEFINE([ENABLE_ECG_VIEW], , [Enable ecg view])
fi

AC_DEFINE([_POSIX_C_SOURCE], 200112L, [Required for some feature test macros])
AC_DEFINE([_XOPEN_SOURCE], , [Required for some feature test macros])

AC_OUTPUT
