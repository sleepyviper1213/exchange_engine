include_guard(GLOBAL)
include(GenerateExportHeader)

# Wrapper around generate_export_header() that adds one macro to the header it
# already writes: <TARGET>_AUTOTEST_EXPORT, alongside the usual <TARGET>_EXPORT
# and <TARGET>_NO_EXPORT.
#
# Modelled on Qt's Q_AUTOTEST_EXPORT. These libraries are SHARED and annotate
# their public API explicitly, which switches off MinGW's auto-export: only
# annotated symbols reach the import library, and CMAKE_CXX_VISIBILITY_PRESET
# gives ELF the same rule. That leaves no way for a test to link against an
# internal symbol short of making it permanently public.
# <TARGET>_AUTOTEST_EXPORT is the escape hatch — it resolves to the normal
# export/import attribute in an internal build (ORDER_BOOK_BUILD_INTERNAL, on
# by default whenever ORDER_BOOK_BUILD_TESTS is) and to <TARGET>_NO_EXPORT in a
# shipping one, so the symbol never becomes part of the released ABI.
#
# The switch is a global compile definition rather than a per-target one: the
# library and its tests must agree on it, or one side would annotate the
# declaration differently from the other.
function(generate_module_export_header target)
    string(TOUPPER "${target}" _upper)

    # CUSTOM_CONTENT_FROM_VARIABLE takes the *name* of a variable, not its
    # value; the contents are spliced in just before the header's closing
    # #endif, so the include guard covers them.
    set(_autotest_content
        "
#ifndef ${_upper}_AUTOTEST_EXPORT
#  ifdef ORDER_BOOK_BUILD_TESTS
#    define ${_upper}_AUTOTEST_EXPORT ${_upper}_EXPORT
#  else
#    define ${_upper}_AUTOTEST_EXPORT ${_upper}_NO_EXPORT
#  endif
#endif
")

    generate_export_header(${target} CUSTOM_CONTENT_FROM_VARIABLE
                           _autotest_content)
endfunction()
