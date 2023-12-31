dnl This Source Code Form is subject to the terms of the Mozilla Public
dnl License, v. 2.0. If a copy of the MPL was not distributed with this
dnl file, You can obtain one at http://mozilla.org/MPL/2.0/.

dnl Set the MOZ_ICU_VERSION variable to denote the current version of the
dnl ICU library, as well as a few other things.

AC_DEFUN([MOZ_CONFIG_ICU], [

ICU_LIB_NAMES=

MOZ_ARG_WITH_STRING(intl-api,
[  --with-intl-api, --with-intl-api=build, --without-intl-api
    Determine the status of the ECMAScript Internationalization API.  The first
    (or lack of any of these) builds and exposes the API.  The second builds it
    but doesn't use ICU or expose the API to script.  The third doesn't build
    ICU at all.],
    _INTL_API=$withval)

ENABLE_INTL_API=
EXPOSE_INTL_API=
case "$_INTL_API" in
no)
    ;;
build)
    ENABLE_INTL_API=1
    ;;
yes)
    ENABLE_INTL_API=1
    EXPOSE_INTL_API=1
    ;;
*)
    AC_MSG_ERROR([Invalid value passed to --with-intl-api: $_INTL_API])
    ;;
esac

if test -n "$ENABLE_INTL_API"; then
    USE_ICU=1
fi

if test -n "$EXPOSE_INTL_API"; then
    AC_DEFINE(EXPOSE_INTL_API)
fi

if test -n "$ENABLE_INTL_API"; then
    AC_DEFINE(ENABLE_INTL_API)
fi

dnl Settings for the implementation of the ECMAScript Internationalization API
AC_SUBST(MOZ_ICU_VERSION)
AC_SUBST(ENABLE_INTL_API)
AC_SUBST(USE_ICU)

if test -n "$USE_ICU"; then
    dnl Source files that use ICU should have control over which parts of the ICU
    dnl namespace they want to use.
    AC_DEFINE(U_USING_ICU_NAMESPACE,0)
fi
])
