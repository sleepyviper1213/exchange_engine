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
# export/import attribute when ORDER_BOOK_BUILD_TESTS is set, and to
# <TARGET>_NO_EXPORT otherwise, so a shipping build never carries the symbol in
# its export table or dynamic symbol table.
#
# ORDER_BOOK_BUILD_TESTS is a global compile definition (see ProjectOptions),
# never a per-target one: the library and its tests must agree on it, or one
# side would annotate the declaration differently from the other.
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

	set(_export_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_export_file "${_export_dir}/${target}_export.hpp")
    file(MAKE_DIRECTORY "${_export_dir}")

    # Existing _autotest_content setup remains here.

    generate_export_header(${target}
        EXPORT_FILE_NAME "${_export_file}"
        CUSTOM_CONTENT_FROM_VARIABLE _autotest_content
    )

    target_include_directories(${target}
        PUBLIC
        $<BUILD_INTERFACE:${_export_dir}>
    )
endfunction()
